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

/**********************************************************************/
/* For testing */
static int rowlockSavepointCreate(RowLockSavepoint *pLockSavepoint, int iSavepoint);
static void rowlockSavepointClose(RowLockSavepoint *pLockSavepoint, int op, int iSavepoint, IpcHandle *pHandle, void *pRootPages);

int sqlite3_rowlock_savepoint_init(RowLockSavepoint *pLockSavepoint){
  return sqlite3rowlockSavepointInit(pLockSavepoint);
}

void sqlite3_rowlock_savepoint_finish(RowLockSavepoint *pLockSavepoint){
  sqlite3rowlockSavepointFinish(pLockSavepoint);
}

int sqlite3_rowlock_savepoint_create(RowLockSavepoint *pLockSavepoint, int iSavepoint){
  return rowlockSavepointCreate(pLockSavepoint, iSavepoint);
}

void sqlite3_rowlock_savepoint_close(RowLockSavepoint *pLockSavepoint, int op, int iSavepoint, IpcHandle *pHandle, void *pRootPages){
  rowlockSavepointClose(pLockSavepoint, op, iSavepoint, pHandle, (HashI64*)pRootPages);
}

int sqlite3_rowlock_history_add_record(RowLockSavepoint *pLockSavepoint, int iTable, sqlite3_int64 rowid){
  return sqlite3rowlockHistoryAddRecord(pLockSavepoint, iTable, rowid);
}

int sqlite3_rowlock_history_add_table(RowLockSavepoint *pLockSavepoint, int iTable){
  return sqlite3rowlockHistoryAddTable(pLockSavepoint, iTable);
}

static void (*xRowlockIpcUnlockRecord)(IpcHandle*,int,sqlite3_int64) = sqlite3rowlockIpcUnlockRecord;
void sqlite3_rowlock_register_unlockRecord_func(void(*xFunc)(IpcHandle*,int,sqlite3_int64)){
  if( xFunc ){
    xRowlockIpcUnlockRecord = xFunc;
  }else{
    xRowlockIpcUnlockRecord = sqlite3rowlockIpcUnlockRecord;
  }
}

static void *(*xRootPageDelete)(HashI64*,sqlite3_int64,void*) = sqlite3HashI64Insert;
void sqlite3_rowlock_register_rootPageDel_func(void*(*xFunc)(void*,sqlite3_int64,void*)){
  if( xFunc ){
    xRootPageDelete = xFunc;
  }else{
    xRootPageDelete = sqlite3HashI64Insert;
  }
}

/**********************************************************************/



int sqlite3rowlockSavepointInit(RowLockSavepoint *pLockSavepoint){
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

void sqlite3rowlockSavepointFinish(RowLockSavepoint *pLockSavepoint){
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
  if( pLockSavepoint->nSavepointMax<=pLockSavepoint->nSavepoints ){
    /* Expand the area twice. */
    RowLockSavepointEntry *pSavepoints = (RowLockSavepointEntry*)sqlite3Realloc(pLockSavepoint->pSavepoints, 
                                                                                pLockSavepoint->nSavepointMax*2);
    if( !pSavepoints ) return SQLITE_NOMEM_BKPT;
    pLockSavepoint->nSavepointMax *= 2;
    pLockSavepoint->pSavepoints = pSavepoints;
  }

  pLockSavepoint->pSavepoints[pLockSavepoint->nSavepoints].iSavepoint = iSavepoint;
  pLockSavepoint->pSavepoints[pLockSavepoint->nSavepoints].iLockRecord = pLockSavepoint->nHistory;
  pLockSavepoint->nSavepoints++;

  return SQLITE_OK;
}

static void rowlockSavepointClose(RowLockSavepoint *pLockSavepoint, int op, int iSavepoint, IpcHandle *pHandle, HashI64 *pRootPages){
  int i;
  u64 idxRowid = 0;
  u32 nSavepoints = 0;
  u64 iStack;

  /* Search the target savepoint. */
  if( pLockSavepoint->nSavepoints>0 ){
    for( i=pLockSavepoint->nSavepoints-1; ; i-- ){
      if( pLockSavepoint->pSavepoints[i].iSavepoint<=iSavepoint ){
        idxRowid = pLockSavepoint->pSavepoints[i].iLockRecord;
        nSavepoints = i;
        break;
      }
      if( i==0 ) break;
    }
  }

  /* Unlock rows and delete root page relation. */
  if( op==SAVEPOINT_ROLLBACK && pLockSavepoint->nHistory>0 ){
    for( iStack=pLockSavepoint->nHistory-1; iStack>=idxRowid; iStack-- ){
      RowLockHistory *pHistory = &pLockSavepoint->pHistory[iStack];
      if( pHistory->type==RLH_REOCRD ){
        xRowlockIpcUnlockRecord(pHandle, pHistory->iTable, pHistory->rowid);
      }else{
        assert( pHistory->type==RLH_TABLE );
        xRootPageDelete(pRootPages, pHistory->iTable, NULL);
      }
      if( iStack==idxRowid ) break;
    }
  }

  /* Erase savepoint. */
  memset(&pLockSavepoint->pHistory[idxRowid], 0, sizeof(RowLockHistory)*(pLockSavepoint->nHistory-idxRowid));
  memset(&pLockSavepoint->pSavepoints[nSavepoints], 0, sizeof(RowLockSavepointEntry)*(pLockSavepoint->nSavepoints-nSavepoints));
  pLockSavepoint->nHistory = idxRowid;
  pLockSavepoint->nSavepoints = nSavepoints;

  return;
}

/*
** We memorize locked rowid and created table id of transaction btree.
** This information is used for savepoint rollback.
*/
static int sqlite3rowlockHistoryAdd(RowLockSavepoint *pLockSavepoint, HistoryType type, int iTable, i64 rowid){
  if( pLockSavepoint->nHistoryMax<=pLockSavepoint->nHistory ){
    /* Expand the area twice. */
    RowLockHistory *pHistory = (RowLockHistory*)sqlite3Realloc(pLockSavepoint->pHistory, 
                                                             pLockSavepoint->nHistoryMax*2);
    if( !pHistory ) return SQLITE_NOMEM_BKPT;
    pLockSavepoint->nHistoryMax *= 2;
    pLockSavepoint->pHistory = pHistory;
  }

  pLockSavepoint->pHistory[pLockSavepoint->nHistory].rowid = rowid;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].iTable = iTable;
  pLockSavepoint->pHistory[pLockSavepoint->nHistory].type = type;
  pLockSavepoint->nHistory++;

  return SQLITE_OK;
}

int sqlite3rowlockHistoryAddRecord(RowLockSavepoint *pLockSavepoint, int iTable, i64 rowid){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_REOCRD, iTable, rowid);
}

int sqlite3rowlockHistoryAddTable(RowLockSavepoint *pLockSavepoint, int iTable){
  return sqlite3rowlockHistoryAdd(pLockSavepoint, RLH_TABLE, iTable, 0);
}

int sqlite3TransBtreeSavepoint(Btree *p, int op, int iSavepoint){
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;

  if( !pBtreeTrans ){
    return sqlite3BtreeSavepointOriginal(p, op, iSavepoint);
  }else{
    /* Rollback or clear a row lock information. */
    rowlockSavepointClose(&pBtTrans->lockSavepoint, op, iSavepoint, &pBtTrans->ipcHandle, &pBtTrans->rootPages);
    /* We use trans btree instead of shared btree. */
    return sqlite3BtreeSavepointOriginal(pBtreeTrans, op, iSavepoint);
  }
}

#endif /* SQLITE_OMIT_ROWLOCK */
