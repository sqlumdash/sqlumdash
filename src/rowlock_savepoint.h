/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for the savepoint of row and table lock.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_SAVEPOINT_H
#define SQLITE_ROWLOCK_SAVEPOINT_H
#include "sqliteInt.h"


typedef enum HistoryType {
  RLH_REOCRD,
  RLH_INDEX,
  RLH_NEW_TABLE,
  RLH_NEW_INDEX,
  RLH_TABLE_LOCK,
  RLH_TABLE_CLEAR
} HistoryType;

typedef struct RowLockHistory {
  i64 n;   /* rowid or nKey */
  void *p; /* pKey */
  int iTable;
  const CollSeq *pColl;
  u8 prev;
  HistoryType type;
} RowLockHistory;

typedef struct RowLockSavepointEntry {
  u64 iLockRecord;
  int iSavepoint;
} RowLockSavepointEntry;

typedef struct RowLockSavepoint {
  RowLockHistory *pHistory;
  u64 nHistory;            /* The number of entries in the stack */
  u64 nHistoryMax;         /* The number of entries wihch can be stored in the stack */
  RowLockSavepointEntry *pSavepoints;
  u32 nSavepoints;
  u32 nSavepointMax;
} RowLockSavepoint;


void sqlite3rowlockSavepointClose(RowLockSavepoint *pLockSavepoint);
int sqlite3rowlockHistoryAddRecord(RowLockSavepoint *pLockSavepoint, int iTable, i64 rowid);
int sqlite3rowlockHistoryAddIndex(RowLockSavepoint *pLockSavepoint, int iTable, i64 nKey,
                                  const void *pKey, const CollSeq *pColl);
int sqlite3rowlockHistoryAddNewTable(RowLockSavepoint *pLockSavepoint, int iTable);
int sqlite3rowlockHistoryAddNewIndex(RowLockSavepoint *pLockSavepoint, int iTable);
int sqlite3rowlockHistoryAddTableLock(RowLockSavepoint *pLockSavepoint, int iTable, u8 prevLock);
int sqlite3rowlockHistoryAddTableClear(RowLockSavepoint *pLockSavepoint, int iTable, u8 deleteAll);

#endif /* SQLITE_ROWLOCK_SAVEPOINT_H */
#endif /* SQLITE_OMIT_ROWLOCK */
