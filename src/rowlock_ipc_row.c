/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file implements functions for shaing row lock information with 
** the other processes.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#include "sqliteInt.h"
#include "rowlock.h"
#include "rowlock_ipc.h"
#include "rowlock_ipc_row.h"
#include "rowlock_os.h"

#if SQLITE_OS_WIN
#define IpcRowLockMutex() &(pHandle->rlMutex)
#else
#define IpcRowLockMutex() &(pMeta->mutex)
#endif

extern IpcClass ipcClasses[];

int rowClassMapName(char *buf, size_t bufSize, const char *name){
  return rowlockStrCat(buf, bufSize, name, MMAP_SUFFIX_ROWLOCK);
}

u8 rowClassIsInitialized(void *pMap){
  RowMetaData *pMeta = (RowMetaData*)pMap;
  if( pMeta && pMeta->nElement>0 ){
    return 1;
  }else{
    return 0;
  }
}

void rowClassInitArea(void *pMap, u64 allocSize){
  RowMetaData *pMeta = (RowMetaData*)pMap;
  u64 nElem = (allocSize - sizeof(RowMetaData)) / sizeof(RowElement);

  memset(pMeta, 0, allocSize);
  pMeta->nElement = nElem;
  pMeta->count = 0;
}

u64 rowClassElemCount(void *pMap){
  RowMetaData *pMeta = (RowMetaData*)pMap;
  return pMeta->nElement;
}

u8 rowClassIsValid(void *pElem){
  RowElement *pElement = (RowElement*)pElem;
  return pElement->iTable!=0;
}

u8 rowClassElemIsTarget(void *pElem1, void *pElem2){
  RowElement *pElement1 = (RowElement*)pElem1;
  RowElement *pElement2 = (RowElement*)pElem2;
  return pElement1->iTable==pElement2->iTable && pElement1->rowid==pElement2->rowid;
}

void *rowClassElemGet(void *pMap, u64 idx){
  RowElement *pData = (RowElement*)((char*)pMap+sizeof(RowMetaData));
  return &pData[idx];
}

u64 rowClassElemHash(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  RowElement *pElem = (RowElement*)xClass->xElemGet(pMap, idx);
  return pElem->hash;
}

void rowClassElemClear(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  RowElement *pElem = (RowElement*)xClass->xElemGet(pMap, idx);
  pElem->hash = 0;
  pElem->iTable = 0;
  pElem->owner = 0;
  pElem->pid = 0;
  pElem->rowid = 0;
}

void rowClassElemCopy(void *pMap, u64 iDest, u64 iSrc){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  RowElement *pElemDest = (RowElement*)xClass->xElemGet(pMap, iDest);
  RowElement *pElemSrc = (RowElement*)xClass->xElemGet(pMap, iSrc);

  pElemDest->hash = pElemSrc->hash;
  pElemDest->iTable = pElemSrc->iTable;
  pElemDest->owner = pElemSrc->owner;
  pElemDest->pid = pElemSrc->pid;
  pElemDest->rowid = pElemSrc->rowid;
}

u64 rowClassIndexPrev(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  u64 nElem = xClass->xElemCount(pMap);
  return (idx + nElem - 1) % nElem;
}

u64 rowClassIndexNext(void *pMap, u64 idx){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  u64 nElem = xClass->xElemCount(pMap);
  return (idx + 1) % nElem;
}

u64 rowClassCalcHash(void *pMap, ...){
  RowMetaData *pMeta = (RowMetaData*)pMap;
  unsigned char buf[sizeof(int)+sizeof(i64)] = {0};
  va_list val;
  int iTable;
  i64 rowid;

  va_start(val, pMap);
  iTable = va_arg(val, int);
  rowid = va_arg(val, i64);
  va_end(val);

  memcpy(buf, &iTable, sizeof(iTable));
  memcpy(buf+sizeof(iTable), &rowid, sizeof(rowid));

  return rowlockIpcCalcHash(pMeta->nElement, buf, sizeof(buf));
}

#ifndef NDEBUG
void rowClassPrintData(void *pMap){
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  RowMetaData *pMeta = (RowMetaData*)pMap;
  RowElement *pData = (RowElement*)((char*)pMap+sizeof(RowMetaData));
  u64 idx;
  
  for( idx=0; idx<xClass->xElemCount(pMap); idx++ ){
    RowElement *pElem = (RowElement*)xClass->xElemGet(pMap, idx);
    int isLocked = (xClass->xElemIsValid(pElem))? 1:0;
    printf("[%0lld]%d, ", idx, isLocked);
    if( (idx+1)%10==0 ) printf("\n");
  }
  printf("\n");
}
#endif


/********************************************************************************/
/* Class IPC_CLASS_ROW */

static void rowlockIpcRowValueSet(RowElement *pElement, u64 idx, u64 hash, PID pid, int iTable, i64 rowid, u64 owner){
  pElement[idx].hash = hash;
  pElement[idx].pid = pid;
  pElement[idx].iTable = iTable;
  pElement[idx].rowid = rowid;
  pElement[idx].owner = owner;
}

/*
** Lock record.
** Return SQLITE_DONE if row is locked by myself.
** Return SQLITE_LOCKED if row is locked by another user.
** Return SQLITE_OK if row is locked successfully.
*/
int sqlite3rowlockIpcLockRecord(IpcHandle *pHandle, int iTable, i64 rowid){
  int rc = SQLITE_OK;
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  void *pMap = pHandle->pRecordLock;
  RowMetaData *pMeta = (RowMetaData*)pMap;
  RowElement *pElement = (RowElement*)((char*)pHandle->pRecordLock+sizeof(RowMetaData));
  u64 hash = xClass->xCalcHash(pMap, iTable, rowid);
  PID pid = rowlockGetPid();
  RowElement rowlockTarget = {0};
  u64 idx;

  assert( iTable!=0 );
  rowlockTarget.iTable = iTable;
  rowlockTarget.rowid = rowid;

  rowlockOsMutexEnter(IpcRowLockMutex());

  rc = rowlockIpcSearch(pHandle->pRecordLock, IPC_CLASS_ROW, &rowlockTarget, hash, &idx);
  if( rc ){
    if( rc==SQLITE_LOCKED && pElement[idx].pid==pid && pElement[idx].owner==pHandle->owner ){
      /* I already have a lock. */
      rc = SQLITE_DONE;
    }
    goto lock_record_end;
  }

  /* 
  ** At least 1 empty element is required. So we can add new entry until 
  ** the element count is less than pMeta->nElement - 1.
  */
  if( pMeta->count>=pMeta->nElement-1 ){
    rc = SQLITE_NOMEM_BKPT;
    goto lock_record_end;
  }

  /* If SQLITE_OK is returned, no one lock the record. So we can lock it. */
  rowlockIpcRowValueSet(pElement, idx, hash, pid, iTable, rowid, pHandle->owner);
  pMeta->count++;

lock_record_end:
  rowlockOsMutexLeave(IpcRowLockMutex());
  return rc;
}

/* Check if a record is locked by another user or not.
** Return SQLITE_LOCKED if row is locked by another user.
** Return SQLITE_OK if row is not locked or lock owner is me.
*/
int sqlite3rowlockIpcLockRecordQuery(IpcHandle *pHandle, int iTable, i64 rowid){
  int rc = SQLITE_OK;
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  void *pMap = pHandle->pRecordLock;
  RowMetaData *pMeta = (RowMetaData*)pMap;
  u64 hash = xClass->xCalcHash(pMap, iTable, rowid);
  RowElement rowlockTarget = {0};
  u64 idx;

  assert( iTable!=0 );

  rowlockTarget.iTable = iTable;
  rowlockTarget.rowid = rowid;

  rowlockOsMutexEnter(IpcRowLockMutex());
  rc = rowlockIpcSearch(pHandle->pRecordLock, IPC_CLASS_ROW, &rowlockTarget, hash, &idx);
  if( rc==SQLITE_NOMEM ){
    rc = SQLITE_OK;
  }else if( rc==SQLITE_LOCKED ){
    RowElement *pElem;
    PID pid = rowlockGetPid();
    pElem = (RowElement*)xClass->xElemGet(pMap, idx);
    if( pElem->pid == pid && pElem->owner == pHandle->owner ){
      /* Owner is myself. */
      rc = SQLITE_OK;
    }
  }else{
    /* No one have a lock. */
    assert( rc==SQLITE_OK );
  }
  rowlockOsMutexLeave(IpcRowLockMutex());

  return rc;
}

void sqlite3rowlockIpcUnlockRecord(IpcHandle *pHandle, int iTable, i64 rowid){
  int rc = SQLITE_OK;
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  void *pMap = pHandle->pRecordLock;
  RowMetaData *pMeta = (RowMetaData*)pMap;
  u64 hash = xClass->xCalcHash(pMap, iTable, rowid);
  PID pid = rowlockGetPid();
  u64 idx;
  u64 idxEmpty;
  u64 idxDel;
  int found = 0;
  RowElement rowlockTarget = {0};

  assert( iTable!=0 );

  rowlockOsMutexEnter(IpcRowLockMutex());

  /* Search a deleting target */
  if( !xClass->xElemIsValid(xClass->xElemGet(pMap,hash)) ){
    /* There are no entry in the bucket. */
    goto unlock_record;
  }

  /* Search a deleting target. */
  rowlockTarget.iTable = iTable;
  rowlockTarget.rowid = rowid;
  rc = rowlockIpcSearch(pMap, IPC_CLASS_ROW, &rowlockTarget, hash, &idxDel);
  if( rc!=SQLITE_LOCKED ) goto unlock_record;

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

  rowlockIpcDelete(pMap, IPC_CLASS_ROW, hash, idxDel, xClass->xIndexPrev(pMap,idxEmpty));
  pMeta->count--;

unlock_record:
  rowlockOsMutexLeave(IpcRowLockMutex());
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
static void sqlite3rowlockIpcUnlockRecordProcCore(IpcHandle *pHandle, PID pid, const char *name){
  IpcHandle ipcHandle = {0};
  IpcClass *xClass = &ipcClasses[IPC_CLASS_ROW];
  void *pMap;
  RowMetaData *pMeta;
  RowElement *pElement;
  u64 nElem;
  u64 idx;
  u64 idxStart;

  assert( pid!=0 || !pHandle );
  assert( pHandle || name );

  if( !pHandle ){
    int rc = sqlite3rowlockIpcInit(&ipcHandle, sqlite3GlobalConfig.szMmapRowLock, sqlite3GlobalConfig.szMmapTableLock, NULL, name);
    if( rc ) return;
    pHandle = &ipcHandle;
  }

  pMap = pHandle->pRecordLock;
  pMeta = (RowMetaData*)pHandle->pRecordLock;

  rowlockOsMutexEnter(IpcRowLockMutex());

  if( !pMap ) goto unlock_record_proc_end;

  pElement = (RowElement*)((char*)pHandle->pRecordLock+sizeof(RowMetaData));
  nElem = xClass->xElemCount(pMap);
  if( nElem==0 ) goto unlock_record_proc_end;

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
      sqlite3rowlockIpcUnlockRecord(pHandle, pElement[idx].iTable, pElement[idx].rowid);
    }
    idx =xClass->xIndexPrev(pMap, idx);
  }while( idx!=idxStart );

unlock_record_proc_end:
  rowlockOsMutexLeave(IpcRowLockMutex());

  /* Close ipc handle if it was opend in this function. */
  if( ipcHandle.pRecordLock ){
    sqlite3rowlockIpcFinish(pHandle);
  }
}

/* Unlock all lock information owned by this process. */
void sqlite3rowlockIpcUnlockRecordProc(IpcHandle *pHandle, const char *name){
  PID pid = rowlockGetPid();
  sqlite3rowlockIpcUnlockRecordProcCore(pHandle, pid, name);
}

/* Unlock all lock information regardless of owner. */
void sqlite3rowlockIpcUnlockRecordAll(const char *name){
  sqlite3rowlockIpcUnlockRecordProcCore(NULL, 0, name);
}
#endif /* SQLITE_OMIT_ROWLOCK */
