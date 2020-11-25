/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file implements the savepoint of row and table lock.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#include "rowlock_savepoint.h"
#include "rowlock_ipc.h"
#include "sqliteInt.h"
#include "btreeInt.h"

#define ROWLOCK_STACK_DEAULT_SIZE (1024)
#define ROWLOCK_SAVEPOINT_DEAULT_SIZE (64)

static int rowlockSavepointRollbackTableLock(IpcHandle *pHandle, int iTable, u8 prevLock);

static void (*xRowlockIpcUnlockRecord)(IpcHandle*,int,sqlite3_int64) = sqlite3rowlockIpcUnlockRecord;
static int (*xSavepointRollbackTableLock)(IpcHandle*,int,u8) = rowlockSavepointRollbackTableLock;
static void *(*xRootPageDelete)(HashI64*,sqlite3_int64,void*,void*,
                                void *(*xMalloc)(void*, sqlite3_int64),
                                void (*xFree)(void*, void*)) = sqlite3HashI64Insert;

/**********************************************************************/
/* For testing */
static int rowlockSavepointInit(RowLockSavepoint *pLockSavepoint);
static int rowlockSavepointCreate(RowLockSavepoint *pLockSavepoint, int iSavepoint);
static void rowlockSavepoint(RowLockSavepoint *pLockSavepoint, int op, int iSavepoint,
                             IpcHandle *pIpcHandle, PsmLockHandle *pPsmHandle, void *owner,
                             HashI64 *pRootPages);

int sqlite3_rowlock_savepoint_init(RowLockSavepoint *pLockSavepoint){
  return rowlockSavepointInit(pLockSavepoint);
}

void sqlite3_rowlock_savepoint_close(RowLockSavepoint *pLockSavepoint){
  sqlite3rowlockSavepointClose(pLockSavepoint);
}

int sqlite3_rowlock_savepoint_create(RowLockSavepoint *pLockSavepoint, int iSavepoint){
  return rowlockSavepointCreate(pLockSavepoint, iSavepoint);
}

void sqlite3_rowlock_savepoint(RowLockSavepoint *pLockSavepoint, int op, int iSavepoint,
                               IpcHandle *pIpcHandle, PsmLockHandle *pPsmHandle, void *owner,
                               void *pRootPages){
  rowlockSavepoint(pLockSavepoint, op, iSavepoint, pIpcHandle, pPsmHandle, owner, (HashI64*)pRootPages);
}

int sqlite3_rowlock_history_add_record(RowLockSavepoint *pLockSavepoint, int iTable, sqlite3_int64 rowid){
  return sqlite3rowlockHistoryAddRecord(pLockSavepoint, iTable, rowid);
}

int sqlite3_rowlock_history_add_new_table(RowLockSavepoint *pLockSavepoint, int iTable){
  return sqlite3rowlockHistoryAddNewTable(pLockSavepoint, iTable);
}

int sqlite3_rowlock_history_add_table_lock(RowLockSavepoint *pLockSavepoint, int iTable, unsigned char prevLock){
  return sqlite3rowlockHistoryAddTableLock(pLockSavepoint, iTable, prevLock);
}

void sqlite3_rowlock_register_unlockRecord_func(void(*xFunc)(IpcHandle*,int,sqlite3_int64)){
  if( xFunc ){
    xRowlockIpcUnlockRecord = xFunc;
  }else{
    xRowlockIpcUnlockRecord = sqlite3rowlockIpcUnlockRecord;
  }
}

void sqlite3_rowlock_register_lockTable_func(int(*xFunc)(IpcHandle*,int,u8)){
  if( xFunc ){
    xSavepointRollbackTableLock = xFunc;
  }else{
    xSavepointRollbackTableLock = rowlockSavepointRollbackTableLock;
  }
}

void sqlite3_rowlock_register_rootPageDel_func(void*(*xFunc)(void*,sqlite3_int64,void*,void*,
                                               void *(*xMalloc)(void*, sqlite3_int64),
                                               void (*xFree)(void*, void*))){
  if( xFunc ){
    xRootPageDelete = (void*(*)(HashI64*,sqlite3_int64,void*,void*,
                                void *(*xMalloc)(void*, sqlite3_int64),
                                void (*xFree)(void*, void*)))xFunc;
  }else{
    xRootPageDelete = sqlite3HashI64Insert;
  }
}

/**********************************************************************/



static int rowlockSavepointInit(RowLockSavepoint *pLockSavepoint){
  RowLockHistory *pHistory;
  RowLockSavepointEntry *pSavepoints;

  pHistory = (RowLockHistory*)sqlite3MallocZero(sizeof(RowLockHistory) * ROWLOCK_STACK_DEAULT_SIZE);
  if( !pHistory ) return SQLITE_NOMEM_BKPT;

  pSavepoints = (RowLockSavepointEntry*)sqlite3MallocZero(sizeof(RowLockSavepointEntry) * ROWLOCK_SAVEPOINT_DEAULT_SIZE);
  if( !pSavepoints ){
    sqlite3_free(pHistory);
    return SQLITE_NOMEM_BKPT;
  }

  pLockSavepoint->nHistoryMax = ROWLOCK_STACK_DEAULT_SIZE;
  pLockSavepoint->nHistory = 0;
  pLockSavepoint->pHistory = pHistory;
  pLockSavepoint->nSavepointMax = ROWLOCK_SAVEPOINT_DEAULT_SIZE;
  pLockSavepoint->nSavepoints = 0;
  pLockSavepoint->pSavepoints = pSavepoints;

  return SQLITE_OK;
}

void sqlite3rowlockSavepointClose(RowLockSavepoint *pLockSavepoint){
  int i;
  if( !pLockSavepoint ) return;
  for(i=0;i<pLockSavepoint->nHistory;i++){
    sqlite3_free(pLockSavepoint->pHistory[i].p);
  }
  sqlite3_free(pLockSavepoint->pHistory);
  pLockSavepoint->pHistory = NULL;
  pLockSavepoint->nHistoryMax = 0;
  pLockSavepoint->nHistory = 0;
  sqlite3_free(pLockSavepoint->pSavepoints);
  pLockSavepoint->pSavepoints = NULL;
  pLockSavepoint->nSavepoints = 0;
  pLockSavepoint->nSavepointMax = 0;
}

static int rowlockSavepointCreate(RowLockSavepoint *pLockSavepoint, int iSavepoint){
  int nSavepoints;
  RowLockSavepointEntry *pSavepoints;

  if( !pLockSavepoint->pSavepoints ){
    int rc = rowlockSavepointInit(pLockSavepoint);
    if( rc ) return rc;
  }

  if( pLockSavepoint->nSavepointMax<=pLockSavepoint->nSavepoints ){
    /* Expand the area twice. */
    RowLockSavepointEntry *pNew = (RowLockSavepointEntry*)sqlite3Realloc(pLockSavepoint->pSavepoints, 
                                                                         sizeof(RowLockSavepointEntry)*pLockSavepoint->nSavepointMax*2);
    if( !pNew ) return SQLITE_NOMEM_BKPT;
    pLockSavepoint->nSavepointMax *= 2;
    pLockSavepoint->pSavepoints = pNew;
  }

  pSavepoints = pLockSavepoint->pSavepoints;
  nSavepoints = pLockSavepoint->nSavepoints;

  /* Don't create new element if it is same as the latest one. */
  if( nSavepoints>0 && pSavepoints[nSavepoints-1].iSavepoint==iSavepoint &&
      pSavepoints[nSavepoints-1].iLockRecord==pLockSavepoint->nHistory){
    return SQLITE_OK;
  }    

  /* Add new element. */
  pLockSavepoint->pSavepoints[pLockSavepoint->nSavepoints].iSavepoint = iSavepoint;
  pLockSavepoint->pSavepoints[pLockSavepoint->nSavepoints].iLockRecord = pLockSavepoint->nHistory;
  pLockSavepoint->nSavepoints++;

  return SQLITE_OK;
}

static int rowlockSavepointRollbackTableLock(IpcHandle *pHandle, int iTable, u8 prevLock){
  return sqlite3rowlockIpcLockTable(pHandle, iTable, prevLock, MODE_LOCK_FORCE, NULL);
}

static void rowlockSavepoint(RowLockSavepoint *pLockSavepoint, int op, int iSavepoint,
                             IpcHandle *pIpcHandle, PsmLockHandle *pPsmHandle, void *owner,
                             HashI64 *pRootPages){
  int i;
  u64 idxRowid = 0;
  u32 nSavepoints = 0;
  int founded = 0;
  u64 iStack;
  TransRootPage *pData = NULL;

  /* Search the target savepoint. */
  if( pLockSavepoint->nSavepoints>0 ){
    for( i=pLockSavepoint->nSavepoints-1; ; i-- ){
      if( pLockSavepoint->pSavepoints[i].iSavepoint==iSavepoint ){
        idxRowid = pLockSavepoint->pSavepoints[i].iLockRecord;
        nSavepoints = i;
        founded = 1;
        break;
      }
      if( i==0 ) break;
    }
  }
  if( !founded ) return;

  /* Unlock rows and delete root page relation. */
  if( op==SAVEPOINT_ROLLBACK && pLockSavepoint->nHistory>0 ){
    for( iStack=pLockSavepoint->nHistory-1; iStack>=idxRowid; iStack-- ){
      RowLockHistory *pHistory = &pLockSavepoint->pHistory[iStack];
      switch( pHistory->type ){
        case RLH_REOCRD:
          xRowlockIpcUnlockRecord(pIpcHandle, pHistory->iTable, pHistory->n);
          break;
        case RLH_INDEX: {
          sqlite3rowlockPsmUnlockRecord(pPsmHandle, pHistory->iTable,
                                        pHistory->p, pHistory->n, owner,
                                        pHistory->pColl);
          break;
        }
        case RLH_NEW_INDEX:
          sqlite3rowlockPsmDropTable(pPsmHandle, pHistory->iTable);
          break;
        case RLH_NEW_TABLE:
          /* Free memory when DELETE and UPDADE fail by Rowlock.*/
          pData = (TransRootPage*)xRootPageDelete(pRootPages, pHistory->iTable, NULL, NULL, 
                                                  rowlockDefaultMalloc, rowlockDefaultFree);
          sqlite3KeyInfoUnref(pData->pKeyInfo);
          sqlite3_free(pData);
        case RLH_TABLE_LOCK:
          xSavepointRollbackTableLock(pIpcHandle, pHistory->iTable, pHistory->prev);
          break;
        case RLH_TABLE_CLEAR: {
          TransRootPage *pRootPage = (TransRootPage*)sqlite3HashI64Find(pRootPages, pHistory->iTable);
          if( pRootPage ) {
            pRootPage->deleteAll = pHistory->prev;
          }
          break;
        }
      }
      if( iStack==idxRowid ) break;
    }
  }

  /* Erase savepoint. */
  if( op==SAVEPOINT_ROLLBACK ){
    /* Keep the target savepoint, erase all the intervening savepoints. */
    nSavepoints++;
  }
  if( pLockSavepoint->nSavepoints>nSavepoints ){
    memset(&pLockSavepoint->pSavepoints[nSavepoints], 0, sizeof(RowLockSavepointEntry)*(pLockSavepoint->nSavepoints-nSavepoints));
    pLockSavepoint->nSavepoints = nSavepoints;
  }
  if( op==SAVEPOINT_ROLLBACK && pLockSavepoint->nHistory>idxRowid ){
    /* Erase history. */
    for(i=idxRowid;i<pLockSavepoint->nHistory;i++){
      sqlite3_free(pLockSavepoint->pHistory[i].p);
    }
    memset(&pLockSavepoint->pHistory[idxRowid], 0, sizeof(RowLockHistory)*(pLockSavepoint->nHistory-idxRowid));
    pLockSavepoint->nHistory = idxRowid;
  }

  return;
}

/*
** We memorize locked information(rowid or table id) and created table id of transaction btree.
** type:
**   RLH_REOCRD     : Memorize locked rowid
**   RLH_INDEX      : Memorize locked index key
**   RLH_NEW_TABLE  : Memorize created table id
**   RLH_NEW_INDEX  : Memorize created index id
**   RLH_TABLE_LOCK : Memorize locked table id
** This information is used for savepoint rollback.
*/
static int sqlite3rowlockHistoryAdd(RowLockSavepoint *pLockSavepoint, HistoryType type, int iTable,
                                   i64 n, const void *p, const CollSeq *pColl, u8 prev){
  void *pNew = NULL;

  if( !pLockSavepoint->pHistory ){
    int rc = rowlockSavepointInit(pLockSavepoint);
    if( rc ) return rc;
  }

  if( pLockSavepoint->nHistoryMax<=pLockSavepoint->nHistory ){
    /* Expand the area twice. */
    RowLockHistory *pHistory = (RowLockHistory*)sqlite3Realloc(pLockSavepoint->pHistory, 
                                                               sizeof(RowLockHistory)*pLockSavepoint->nHistoryMax*2);
    if( !pHistory ) return SQLITE_NOMEM_BKPT;
    pLockSavepoint->nHistoryMax *= 2;
    pLockSavepoint->pHistory = pHistory;
  }

  if( p ){
    pNew = sqlite3Malloc(n);
    if( !pNew ) return SQLITE_NOMEM_BKPT;
    memcpy(pNew, p, n);
  }
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].n = n;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].p = pNew;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].iTable = iTable;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].pColl = pColl;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].prev = prev;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].type = type;
  pLockSavepoint->nHistory++;

  return SQLITE_OK;
}

int sqlite3rowlockHistoryAddRecord(RowLockSavepoint *pLockSavepoint, int iTable, i64 rowid){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_REOCRD, iTable, rowid, NULL, NULL, 0);
}

int sqlite3rowlockHistoryAddIndex(RowLockSavepoint *pLockSavepoint, int iTable, i64 nKey,
                                  const void *pKey, const CollSeq *pColl){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_INDEX, iTable, nKey, pKey, pColl, 0);
}

int sqlite3rowlockHistoryAddNewTable(RowLockSavepoint *pLockSavepoint, int iTable){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_NEW_TABLE, iTable, 0, NULL, NULL, 0);
}

int sqlite3rowlockHistoryAddNewIndex(RowLockSavepoint *pLockSavepoint, int iTable){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_NEW_INDEX, iTable, 0, NULL, NULL, 0);
}

int sqlite3rowlockHistoryAddTableLock(RowLockSavepoint *pLockSavepoint, int iTable, u8 prevLock){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_TABLE_LOCK, iTable, 0, NULL, NULL, prevLock);
}

int sqlite3rowlockHistoryAddTableClear(RowLockSavepoint *pLockSavepoint, int iTable, u8 deleteAll){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_TABLE_CLEAR, iTable, 0, NULL, NULL, deleteAll);
}

/* Create new savepoint. */
int sqlite3TransBtreeSavepointCreate(Btree *p, int iSavepoint){
  BtreeTrans *pBtTrans;
  Btree *pBtreeTrans;

  if( !p ) return SQLITE_OK;

  pBtTrans = &p->btTrans;
  pBtreeTrans = pBtTrans->pBtree;
  if( !pBtreeTrans ) return SQLITE_OK;
  return rowlockSavepointCreate(&pBtTrans->lockSavepoint, iSavepoint);
}

/* Rollback to or release savepoint. */
int sqlite3TransBtreeSavepoint(Btree *p, int op, int iSavepoint){
  BtreeTrans *pBtTrans;
  Btree *pBtreeTrans;

  if( !p ) return SQLITE_OK;

  pBtTrans = &p->btTrans;
  pBtreeTrans = pBtTrans->pBtree;
  if( !pBtreeTrans ){
    return sqlite3BtreeSavepointOriginal(p, op, iSavepoint);
  }else{
    /* Rollback to or release savepoint about shared memory information. */
    rowlockSavepoint(&pBtTrans->lockSavepoint, op, iSavepoint, &pBtTrans->ipcHandle,
                     &pBtTrans->psmHandle, p, &pBtTrans->rootPages);
    /* We use trans btree instead of shared btree. */
    return sqlite3BtreeSavepointOriginal(pBtreeTrans, op, iSavepoint);
  }
}

/* Close all savepoints. */
static void sqlite3TransBtreeSavepointClose(Btree *p){
  BtreeTrans *pBtTrans;
  Btree *pBtreeTrans;

  if( !p ) return;

  pBtTrans = &p->btTrans;
  pBtreeTrans = pBtTrans->pBtree;
  if( !&pBtTrans->lockSavepoint ) return;

  sqlite3rowlockSavepointClose(&pBtTrans->lockSavepoint);
}

/* Called instead of original sqlite3CloseSavepoints(). */
void sqlite3CloseSavepointsAll(sqlite3 *db){
  int i;

  sqlite3CloseSavepointsOriginal(db);

  for(i=0; i<db->nDb; i++){
    sqlite3TransBtreeSavepointClose(db->aDb[i].pBt);
  }
}

#endif /* SQLITE_OMIT_ROWLOCK */
