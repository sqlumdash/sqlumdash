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
  RLH_NEW_TABLE,
  RLH_TABLE_LOCK
} HistoryType;

typedef struct RowLockHistory {
  i64 rowid;
  int iTable;
  u8 prevLock;
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
int sqlite3rowlockHistoryAddNewTable(RowLockSavepoint *pLockSavepoint, int iTable);
int sqlite3rowlockHistoryAddTableLock(RowLockSavepoint *pLockSavepoint, int iTable, u8 prevLock);

#endif /* SQLITE_ROWLOCK_SAVEPOINT_H */
#endif /* SQLITE_OMIT_ROWLOCK */
