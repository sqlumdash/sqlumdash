/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for Inter Process Communication used by 
** row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_IPC_TABLE_H
#define SQLITE_ROWLOCK_IPC_TABLE_H

#include "sqliteInt.h"
#if SQLITE_OS_WIN
#include "Windows.h"
#endif

/* MMAP name's suffix */
#define MMAP_SUFFIX_TABLELOCK "-tablelock"
/* Mutex name's suffix */
#define MUTEX_SUFFIX_TABLELOCK "-tablelock-mutex"

/* Define Process_id process table lock */
#define PID_CLEANER 0

typedef struct TableMetaData {
#if SQLITE_OS_UNIX
  MUTEX_HANDLE mutex; /* Mutex is shared on Linux */
#endif
  u64 nElement; /* The number of elements */
  u64 nLock;    /* The number of stored elements for table lock */
  u64 nCache;   /* The number of stored elements for cachedRowid */
} TableMetaData;

typedef struct TableElement {
  u64 hash;
  int iTable;
  PID pid;
  u64 owner;
  u8 eLock;
  u8 inUse; /* Statement using this table is processing */
} TableElement;

typedef struct CachedRowid {
  int iTable;
  i64 rowid;
} CachedRowid;

int tableClassMapName(char *buf, size_t bufSize, const char *name);
u8 tableClassIsInitialized(void *pMap);
void tableClassInitArea(void *pMap, u64 nElem);
u64 tableClassElemCount(void *pMap);
u8 tableClassIsValid(void *pElem);
u8 tableClassElemIsTarget(void *pElem1, void *pElem2);
void *tableClassElemGet(void *pMap, u64 idx);
u64 tableClassElemHash(void *pMap, u64 idx);
void tableClassElemClear(void *pMap, u64 idx);
void tableClassElemCopy(void *pMap, u64 iDest, u64 iSrc);
u64 tableClassIndexPrev(void *pMap, u64 idx);
u64 tableClassIndexNext(void *pMap, u64 idx);
u64 tableClassCalcHash(void *pMap, ...);
u64 tableClassTestHash(void *pMap, ...);


#endif /* SQLITE_ROWLOCK_IPC_TABLE_H */
#endif /* SQLITE_OMIT_ROWLOCK */
