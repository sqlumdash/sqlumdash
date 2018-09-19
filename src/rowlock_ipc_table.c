/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file implements functions for shaing table lock information with 
** the other processes.
**/
#ifndef SQLITE_OMIT_ROWLOCK
#include "sqliteInt.h"
#include "btreeInt.h"
#include "rowlock_ipc.h"
#include "rowlock_ipc_table.h"

#if SQLITE_OS_WIN
#include <windows.h>
#define rowlockGetPid GetCurrentProcessId
#define rowlockGetTid GetCurrentThreadId
#else
#define rowlockGetPid getpid
#define rowlockGetTid gettid
#endif

#define TableLockPointer(pHandle) (TableElement*)((char*)pHandle->pTableLock + sizeof(TableMetaData))
#define CachedRowidPointer(pHandle) (CachedRowid*)((char*)pHandle->pTableLock + (sizeof(TableMetaData) + sizeof(TableElement) * pMeta->nElement))
#if SQLITE_OS_WIN
#define IpcTableLockMutex() (HANDLE)pHandle->tlMutex
#else
#define IpcTableLockMutex() (pthread_mutex_t)(TableMetaData())->mutex
#endif

extern IpcClass ipcClasses[];

/* Mode for sqlite3rowlockIpcUnlockTableCore() */
#define MODE_UNLOCK_TRANS 0
#define MODE_UNLOCK_STMT  1

void tableClassInitArea(void *pMap, u64 allocSize){
  u64 nElem = (allocSize - sizeof(TableMetaData)) / (sizeof(TableElement) + sizeof(CachedRowid));
  CachedRowid *pCachedRowid = (CachedRowid*)((char*)pMap + sizeof(TableMetaData) + sizeof(TableElement) * nElem);
  TableMetaData *pMeta = (TableMetaData*)pMap;

  /* Initialize tablelock structure */
  memset(pMeta, 0, sizeof(TableMetaData) + sizeof(TableElement) * nElem);
  pMeta->nElement = nElem;
  /* Initialize cachedRowid structure */
  memset(pCachedRowid, 0, sizeof(CachedRowid) * nElem);
}

u64 tableClassElemCount(void *pMap){
  TableMetaData *pMeta = (TableMetaData*)pMap;
  return pMeta->nElement;
}

u8 tableClassIsValid(void *pElem){
  TableElement *pElement = (TableElement*)pElem;
  return pElement->iTable!=0;
}

u8 tableClassElemIsTarget(void *pElem1, void *pElem2){
  TableElement *pElement1 = (TableElement*)pElem1;
  TableElement *pElement2 = (TableElement*)pElem2;
  return pElement1->iTable==pElement2->iTable && 
         (!pElement1->owner || !pElement2->owner || pElement1->owner==pElement2->owner);
}

void *tableClassElemGet(void *pMap, u64 idx){
  TableElement *pData = (TableElement*)((char*)pMap+sizeof(TableMetaData));
  return &pData[idx];
}

u64 tableClassElemHash(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  TableElement *pElem = (TableElement*)xClass->xElemGet(pMap, idx);
  return pElem->hash;
}

void tableClassElemClear(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  TableElement *pElem = (TableElement*)xClass->xElemGet(pMap, idx);
  pElem->hash = 0;
  pElem->iTable = 0;
  pElem->pid = 0;
  pElem->owner = 0;
  pElem->eLock = 0;
}

void tableClassElemCopy(void *pMap, u64 iDest, u64 iSrc){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  TableElement *pElemDest = (TableElement*)xClass->xElemGet(pMap, iDest);
  TableElement *pElemSrc = (TableElement*)xClass->xElemGet(pMap, iSrc);

  pElemDest->hash = pElemSrc->hash;
  pElemDest->iTable = pElemSrc->iTable;
  pElemDest->pid = pElemSrc->pid;
  pElemDest->owner = pElemSrc->owner;
  pElemDest->eLock = pElemSrc->eLock;
}

u64 tableClassIndexPrev(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  u64 nElem = xClass->xElemCount(pMap);
  return (idx + nElem - 1) % nElem;
}

u64 tableClassIndexNext(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  u64 nElem = xClass->xElemCount(pMap);
  return (idx + 1) % nElem;
}

u64 tableClassCalcHash(void *pMap, ...){
  TableMetaData *pMeta = (TableMetaData*)pMap;
  unsigned char buf[sizeof(int)] = {0};
  va_list val;
  int iTable;

  va_start(val, pMap);
  iTable = va_arg(val, int);
  va_end(val);

  memcpy(buf, &iTable, sizeof(iTable));

  return rowlockIpcCalcHash(pMeta->nElement, buf, sizeof(buf));
}

#ifndef NDEBUG
void tableClassPrintData(void *pMap){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  TableMetaData *pMeta = (TableMetaData*)pMap;
  TableElement *pData = (TableElement*)((char*)pMap+sizeof(TableMetaData));
  u64 idx;
  
  for( idx=0; idx<xClass->xElemCount(pMap); idx++ ){
    TableElement *pElem = (TableElement*)xClass->xElemGet(pMap, idx);
    int isLocked = (xClass->xElemIsValid(pElem))? 1:0;
    printf("[%0lld]iTable=%d, hash=%llu, owner=%p, eLock=%d\n", idx, pElem->iTable, pElem->hash, pElem->owner, pElem->eLock);
    if( (idx+1)%10==0 ) printf("\n");
  }
  printf("\n");
}
#endif

/********************************************************************************/
/* Class IPC_CLASS_TABLE */

static void rowlockIpcTableValueSet(TableElement *pElement, u64 idx, u64 hash, PID pid, int iTable, u64 owner, u8 eLock){
  pElement[idx].hash = hash;
  pElement[idx].pid = pid;
  pElement[idx].iTable = iTable;
  pElement[idx].owner = owner;
  pElement[idx].eLock = eLock;
  pElement[idx].inUse = 1;
}

/*
** Lock table.
** Return SQLITE_LOCKED if table is locked by another user.
** Return SQLITE_OK if table is locked successfully.
**
** There are 3 modes.
** MODE_LOCK_COMMIT and MODE_LOCK_ROLLBACK are used during COMMIT.
** Try to get EXCLSV_LOCK for all tables which are modified in a transaction,
** If it cannot get the lock after getting lock of some tables, we must revert to
** orevious lock state. In this case, MODE_LOCK_ROLLBACK is used.
** 
**   MODE_LOCK_NORMAL   : Used for query execution.
**   MODE_LOCK_COMMIT   : Get EXCLSV_LOCK and memorize a previous lock level.
**   MODE_LOCK_ROLLBACK : Revert to previous lock level.
*/
int sqlite3rowlockIpcLockTable(IpcHandle *pHandle, int iTable, u8 eLock, int mode){
  int rc = SQLITE_OK;
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  void *pMap = pHandle->pTableLock;
  TableMetaData *pMeta = (TableMetaData*)pMap;
  TableElement *pElement = TableLockPointer(pHandle);
  u64 hash = xClass->xCalcHash(pMap, iTable);
  PID pid = rowlockGetPid();
  TableElement tablelockTarget = {0};
  TableElement *pElem;
  u64 idx = hash;
  u64 iidx = 0;
  int found = 0;

  assert( iTable!=0 );
  assert( eLock!=EXCLSV_LOCK || iTable!=MASTER_ROOT );

  rowlockIpcMutexLock(IpcTableLockMutex());

  /* Find the first element having same iTable. */
  pElem = (TableElement*)xClass->xElemGet(pMap, idx);
  /* Check until element is not valid. */
  while( xClass->xElemIsValid(pElem) ){
    /* Check lock status of target table. */
    if( pElem->iTable==iTable ){
      if( pElem->pid == pid && pElem->owner == pHandle->owner ){
        /* Owner is myself. */
        if( mode==MODE_LOCK_ROLLBACK ){
          pElement[idx].eLock = pElement[idx].eLockPrev;
          goto lock_table_end;
        }
        if( pElem->eLock>=eLock ) goto lock_table_end;
        iidx = idx;
        found = 1;
      }else{
        /* Other user has a table lock. */
        if( iTable==MASTER_ROOT ){
          /* Cannot get a write lock if someone has a write lock of sqlite_master. */
          assert( pElem->eLock==WRITE_LOCK );
          rc = SQLITE_LOCKED;
          goto lock_table_end;
        }else{ /* Case of normal table. */
          /*
          ** We cannot get a lock if someone has an exclusive lock. In addition,
          ** In case if MODE_LOCK_NORMAL,
          **   We cannot get an exclusive lock if someone has a write lock.
          ** In case if MODE_LOCK_COMMIT,
          **   We try to get exclusive lock. We can get exclusive lock even if someone has
          **   read lock and write lock. But someone must has been finished a query processing.
          */
          if( pElem->eLock==EXCLSV_LOCK ||
              (mode==MODE_LOCK_NORMAL && (pElem->eLock==WRITE_LOCK && eLock==EXCLSV_LOCK)) ||
              (mode==MODE_LOCK_COMMIT && pElem->inUse==1) ){
            rc = SQLITE_LOCKED;
            goto lock_table_end;
          }
        }
      }
    }
    /* Goto the next element. */
    idx = xClass->xIndexNext(pMap, idx);
    if( idx == hash ) {
      /* All entries are checked. */
      rc = SQLITE_NOMEM_BKPT;
      goto lock_table_end;
    }
    pElem = (TableElement*)xClass->xElemGet(pMap, idx);
  }

  if( found==0 ) iidx = idx;

  /* 
  ** At least 1 empty element is required. So we can add new entry until 
  ** the element count is less than pMeta->nElement - 1.
  */
  if( pMeta->count>=pMeta->nElement-1 ){
    rc = SQLITE_NOMEM_BKPT;
    goto lock_table_end;
  }

  /* If it reaches here, I can lock it. */
  if( mode==MODE_LOCK_COMMIT ) pElement[iidx].eLockPrev = pElement[iidx].eLock;
  rowlockIpcTableValueSet(pElement, iidx, hash, pid, iTable, pHandle->owner, eLock);
  pMeta->count++;

lock_table_end:
  rowlockIpcMutexUnlock(IpcTableLockMutex());
  return rc;
}

/* Return a obtaining lock type.  */
u8 sqlite3rowlockIpcLockTableQuery(IpcHandle *pHandle, int iTable){
  int rc = SQLITE_OK;
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  void *pMap = pHandle->pTableLock;
  u64 hash = xClass->xCalcHash(pMap, iTable);
  u64 idx;
  TableElement tablelockTarget = {0};
  TableElement *pElem;
  u8 eLock = READ_LOCK;

  assert( iTable!=0 );

  rowlockIpcMutexLock(IpcTableLockMutex());

  /* Search a target */
  if( !xClass->xElemIsValid(xClass->xElemGet(pMap,hash)) ){
    /* There are no entry in the bucket. */
    goto lock_table_query;
  }

  tablelockTarget.iTable = iTable;
  tablelockTarget.owner = pHandle->owner;
  rc = rowlockIpcSearch(pMap, IPC_CLASS_TABLE, &tablelockTarget, hash, &idx);
  if( rc!=SQLITE_LOCKED ) goto lock_table_query;

  pElem = (TableElement*)xClass->xElemGet(pMap, idx);
  eLock = pElem->eLock;

lock_table_query:
  rowlockIpcMutexUnlock(IpcTableLockMutex());
  return eLock;
}

/*
** Unlock table lock.
** There are 2 mode.
**  MODE_UNLOCK_TRANS: Unlock table lock when end transaction.
**  MODE_UNLOCK_STMT : Unlock statement table lock.
**                     During query rocessing, inUse flag is set to 1. 
**                     After finishing to prcess query, this flag is set to 0. 
**                     In this case, this mode is used.
*/
static void sqlite3rowlockIpcUnlockTableCore(IpcHandle *pHandle, int iTable, int mode){
  int rc = SQLITE_OK;
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  void *pMap = pHandle->pTableLock;
  TableMetaData *pMeta = (TableMetaData*)pMap;
  u64 hash = xClass->xCalcHash(pMap, iTable);
  PID pid = rowlockGetPid();
  u64 idx;
  u64 idxEmpty;
  u64 idxDel;
  int found = 0;
  TableElement tablelockTarget = {0};
  TableElement *pElement;

  assert( iTable!=0 );

  rowlockIpcMutexLock(IpcTableLockMutex());

  /* Search a deleting target */
  if( !xClass->xElemIsValid(xClass->xElemGet(pMap,hash)) ){
    /* There are no entry in the bucket. */
    goto unlock_table;
  }

  /* Search a deleting target. */
  tablelockTarget.iTable = iTable;
  tablelockTarget.owner = pHandle->owner;
  rc = rowlockIpcSearch(pMap, IPC_CLASS_TABLE, &tablelockTarget, hash, &idxDel);
  if( rc!=SQLITE_LOCKED ) goto unlock_table;

  /* Search empty element. */
  idx = xClass->xIndexNext(pMap, idxDel);
  while( xClass->xElemIsValid(xClass->xElemGet(pMap,idx)) ){
    idx = xClass->xIndexNext(pMap, idx);
    if( idx == hash ) {
      /* There is no empty element. */
      assert(0); /* Not reach here. There is always at least 1 enpty element. */
      break;
    }
  }
  idxEmpty = idx;

  pElement = (TableElement*)xClass->xElemGet(pMap,idxDel);
  if( mode==MODE_UNLOCK_TRANS || pElement->eLock==READ_LOCK ){
    rowlockIpcDelete(pMap, IPC_CLASS_TABLE, hash, idxDel, xClass->xIndexPrev(pMap,idxEmpty));
    pMeta->count--;
  }else{
    pElement->inUse = 0;
  }

unlock_table:
  rowlockIpcMutexUnlock(IpcTableLockMutex());
}

void sqlite3rowlockIpcUnlockTable(IpcHandle *pHandle, int iTable){
  sqlite3rowlockIpcUnlockTableCore(pHandle, iTable, MODE_UNLOCK_TRANS);
}

void sqlite3rowlockIpcUnlockTableStmt(IpcHandle *pHandle, int iTable){
  sqlite3rowlockIpcUnlockTableCore(pHandle, iTable, MODE_UNLOCK_STMT);
}

/* 
** Release all locks acquired by the specified process or owner.
** Process ID and owner can be specified by the argument.
** If Process ID is 0, locks are unlocked regardless process ID.
** If owner is NULL, locks are unlocked regardless owner.
** Process ID=0, owner=0: Called by rowlock cleaner.
** Process ID=0, owner=1: No case.
** Process ID=1, owner=0: Called by SQLite engine when dll is unloaded.
** Process ID=1, owner=1: Called by SQLite engine at transaction or statement is closing.
*/
static void sqlite3rowlockIpcUnlockTablesProcCore(IpcHandle *pHandle, PID pid, int mode){
  IpcHandle ipcHandle = {0};
  IpcClass *xClass = &ipcClasses[IPC_CLASS_TABLE];
  void *pMap;
  TableMetaData *pMeta;
  TableElement *pElement;
  u64 nElem;
  u64 idx;
  u64 idxStart;

  assert( pid!=0 || !pHandle );
  if( !pHandle ){
    int rc = sqlite3rowlockIpcInit(&ipcHandle, ROWLOCK_DEFAULT_MMAP_ROW_SIZE, ROWLOCK_DEFAULT_MMAP_TABLE_SIZE, NULL);
    assert (rc==SQLITE_OK );
    pHandle = &ipcHandle;
  }

  rowlockIpcMutexLock(IpcTableLockMutex());

  pMap = pHandle->pTableLock;
  pMeta = (TableMetaData*)pHandle->pTableLock;
  pElement = TableLockPointer(pHandle);
  nElem = xClass->xElemCount(pMap);
  if( nElem==0 ) goto unlock_tables_proc_end;

  /*
  ** Search an empty element. If the deletion is started from the previous of empty 
  ** element, it is first because the element replacement does not occur. 
  */
  idxStart = nElem - 1;
  while( xClass->xElemIsValid(xClass->xElemGet(pMap,idxStart)) ){
    idxStart = xClass->xIndexPrev(pMap, idxStart);
    if( idxStart==0 ) {
      /* There is no empty element. */
      assert(0); /* Not reach here. There is always at least 1 enpty element. */
      break;
    }
  }

  idx=idxStart;
  do{
    /* Judge the element should be removed. */
    while( xClass->xElemIsValid(xClass->xElemGet(pMap,idx)) &&
           (pid==0 || pElement[idx].pid==pid) && 
           (!pHandle->owner || pElement[idx].owner==pHandle->owner) ){
      sqlite3rowlockIpcUnlockTableCore(pHandle, pElement[idx].iTable, mode);
      if( mode==MODE_UNLOCK_STMT && pElement[idx].inUse==0 ) break;
    }
    idx = xClass->xIndexPrev(pMap, idx);
  }while( idx!=idxStart );

unlock_tables_proc_end:
  rowlockIpcMutexUnlock(IpcTableLockMutex());

  /* Close ipc handle if it was opend in this function. */
  if( ipcHandle.pTableLock ){
    sqlite3rowlockIpcFinish(pHandle);
  }
}

void sqlite3rowlockIpcUnlockTablesProc(IpcHandle *pHandle){
  PID procId = rowlockGetPid();
  sqlite3rowlockIpcUnlockTablesProcCore(pHandle, procId, MODE_UNLOCK_TRANS);
}

void sqlite3rowlockIpcUnlockTablesStmtProc(IpcHandle *pHandle){
  PID procId = rowlockGetPid();
  sqlite3rowlockIpcUnlockTablesProcCore(pHandle, procId, MODE_UNLOCK_STMT);
}

void sqlite3rowlockIpcUnlockTablesAll(void){
  sqlite3rowlockIpcUnlockTablesProcCore(NULL, 0, MODE_UNLOCK_TRANS);
}

/************************************************/
/* CachedRowid */
int sqlite3rowlockIpcCachedRowidSet(IpcHandle *pHandle, int iTable, i64 rowid){
  int rc = SQLITE_OK;
  TableMetaData *pMeta = (TableMetaData*)pHandle->pTableLock;
  CachedRowid *pCachedRowid = CachedRowidPointer(pHandle);
  u32 i;

  rowlockIpcMutexLock(IpcTableLockMutex());

  for( i=0; i<pMeta->nElement; i++ ){
    if( pCachedRowid[i].iTable==iTable || pCachedRowid[i].iTable==0 ){
      pCachedRowid[i].iTable = iTable;
      pCachedRowid[i].rowid = rowid;
      break;
    }
  }

  if( i==pMeta->nElement ) rc = SQLITE_NOMEM_BKPT;

  rowlockIpcMutexUnlock(IpcTableLockMutex());
  return rc;
}

i64 sqlite3rowlockIpcCachedRowidGet(IpcHandle *pHandle, int iTable){
  TableMetaData *pMeta = (TableMetaData*)pHandle->pTableLock;
  CachedRowid *pCachedRowid = CachedRowidPointer(pHandle);
  u32 i;
  i64 rowid = 0;

  rowlockIpcMutexLock(IpcTableLockMutex());

  for( i=0; i<pMeta->nElement; i++ ){
    if( pCachedRowid[i].iTable==iTable ){
      rowid = pCachedRowid[i].rowid;
      break;
    }
  }

  rowlockIpcMutexUnlock(IpcTableLockMutex());
  return rowid;
}

void sqlite3rowlockIpcCachedRowidDropTable(IpcHandle *pHandle, int iTable){
  TableMetaData *pMeta = (TableMetaData*)pHandle->pTableLock;
  CachedRowid *pCachedRowid = CachedRowidPointer(pHandle);
  u32 i;

  rowlockIpcMutexLock(IpcTableLockMutex());

  for( i=0; i<pMeta->nElement; i++ ){
    if( pCachedRowid[i].iTable==iTable ){
      pCachedRowid[i].iTable = 0;
      pCachedRowid[i].rowid = 0;
      break;
    }
    if( pCachedRowid[i].iTable==0 ) break;
  }

  rowlockIpcMutexUnlock(IpcTableLockMutex());
}


#endif /* SQLITE_OMIT_ROWLOCK */
