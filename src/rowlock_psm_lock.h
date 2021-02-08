/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for storing index information on
** Process Shared Memory used by row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_PSM_INDEX_H
#define SQLITE_ROWLOCK_PSM_INDEX_H

#include "psm.h"
#include "rowlock_hash.h"

/* PSM name's suffix */
#define PSM_LOCK_NAME_SUFFIX "-psmlock"
/* Mutex name's suffix */
#define MUTEX_LOCK_NAME_SUFFIX "-psmlock-mutex"
/* File name's suffix for initialization */
#define INIT_LOCK_NAME_SUFFIX "-initlock"

/* Locked index key information. */
typedef struct PsmIdxElem {
  PID pid;
  u64 owner;
} PsmIdxElem;

/* Meta data shared by other processes. */
typedef struct PsmLockMetaData {
  HashI64 list;       /* todo */
#if SQLITE_OS_UNIX
  MUTEX_HANDLE mutex; /* Mutex is shared on Linux */
#endif
} PsmLockMetaData;

typedef struct PsmLockHandle {
  PSMHandle psmHandle;
  PsmLockMetaData *pMeta;
#if SQLITE_OS_WIN
  MUTEX_HANDLE mutex;
#endif
} PsmLockHandle;

int sqlite3rowlockPsmInit(PsmLockHandle *pHandle, size_t nByte,
                          const char *name);
void sqlite3rowlockPsmFinish(PsmLockHandle *pHandle);
int sqlite3rowlockPsmCreateTable(PsmLockHandle *pHandle, int iTable);
void sqlite3rowlockPsmDropTable(PsmLockHandle *pHandle, int iTable);
int sqlite3rowlockPsmLockRecord(PsmLockHandle *pHandle, int iTable,
                                const void *pKey, int nKey,
                                void *owner, const CollSeq *pColl);
int sqlite3rowlockPsmLockRecordQuery(PsmLockHandle *pHandle, int iTable,
                                     const void *pKey, int nKey,
                                     void *owner, const CollSeq *pColl);
void sqlite3rowlockPsmUnlockRecord(PsmLockHandle *pHandle, int iTable,
                                   const void *pKey, int nKey,
                                   void *owner, const CollSeq *pColl);
void sqlite3rowlockPsmUnlockRecordProcCore(PsmLockHandle *pHandle, PID pid, u64 owner,
                                          const char *name);
void sqlite3rowlockPsmUnlockRecordProc(PsmLockHandle *pHandle, u64 owner,
                                      const char *name);
void sqlite3rowlockPsmUnlockRecordAll(const char *name);

#endif /* SQLITE_ROWLOCK_PSM_INDEX_H */
#endif /* SQLITE_OMIT_ROWLOCK */
