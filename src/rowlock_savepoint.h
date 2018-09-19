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
  RLH_TABLE
} HistoryType;

typedef struct RowLockHistory {
  i64 rowid;
  int iTable;
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


int sqlite3rowlockSavepointInit(RowLockSavepoint *pLockSavepoint);
void sqlite3rowlockSavepointFinish(RowLockSavepoint *pLockSavepoint);
int sqlite3rowlockHistoryAddRecord(RowLockSavepoint *pLockSavepoint, int iTable, i64 rowid);
int sqlite3rowlockHistoryAddTable(RowLockSavepoint *pLockSavepoint, int iTable);

#endif /* SQLITE_ROWLOCK_SAVEPOINT_H */
#endif /* SQLITE_OMIT_ROWLOCK */
