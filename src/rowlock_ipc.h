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
#ifndef SQLITE_ROWLOCK_IPC_H
#define SQLITE_ROWLOCK_IPC_H

#include "sqliteInt.h"
#if SQLITE_OS_WIN
#include "Windows.h"
typedef DWORD PID;
typedef DWORD TID;
#define THREAD_LOCAL __declspec(thread)
typedef HANDLE MUTEX_HANDLE;
#else
typedef pid_t PID;
typedef pid_t TID;
#define THREAD_LOCAL __thread
typedef pthread_mutex_t MUTEX_HANDLE;
#endif

typedef struct IpcHandle {
#if SQLITE_OS_WIN
  HANDLE hRecordLock;
  HANDLE hTableLock;
  HANDLE rlMutex;
  HANDLE tlMutex;
#endif
  void *pRecordLock; /* RowMetaData + RowElement[] */
  void *pTableLock;  /* CachedRowid[] */
  u64 owner; /* Owner of the handle. The pointer of Btree is set. */
} IpcHandle;


#define IPC_CLASS_ROW   0
#define IPC_CLASS_TABLE 1

typedef struct IpcClass{
  void (*xInitArea)(void *pMap, u64 nElem);
  u64 (*xElemCount)(void *pMap);
  u8 (*xElemIsValid)(void *pElem);
  u8 (*xElemIsTarget)(void *pElem1, void *pElem2);
  void *(*xElemGet)(void *pMap, u64 idx);
  u64 (*xElemHash)(void *pMap, u64 idx);
  void (*xElemClear)(void *pMap, u64 idx);
  void (*xElemCopy)(void *pMap, u64 iDest, u64 iSrc);
  u64 (*xIndexPrev)(void *pMap, u64 idx);
  u64 (*xIndexNext)(void *pMap, u64 idx);
  u64 (*xCalcHash)(void *pMap, ...);
} IpcClass;

u64 rowlockIpcCalcHash(u64 nBucket, unsigned char *buf, u32 len);


void rowlockIpcMutexLock(MUTEX_HANDLE mutex);
void rowlockIpcMutexUnlock(MUTEX_HANDLE mutex);
int rowlockIpcSearch(void *pMap, u8 iClass, void *pTarget, u64 hash, u64 *pIdx);
void rowlockIpcDelete(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idxEnd);


int sqlite3rowlockIpcInit(IpcHandle *pHandle, u64 nByteRow, u64 nByteTable, const void *owner);
void sqlite3rowlockIpcFinish(IpcHandle *pHandle);
int sqlite3rowlockIpcLockRecord(IpcHandle *pHandle, int iTable, i64 rowid);
int sqlite3rowlockIpcLockRecordQuery(IpcHandle *pHandle, int iTable, i64 rowid);
void sqlite3rowlockIpcUnlockRecord(IpcHandle *pHandle, int iTable, i64 rowid);
void sqlite3rowlockIpcUnlockRecordProc(IpcHandle *pHandle);
void sqlite3rowlockIpcUnlockRecordAll(void);

/* Mode for sqlite3rowlockIpcUnlockTableCore() */
#define MODE_LOCK_NORMAL 0
#define MODE_LOCK_COMMIT 1
#define MODE_LOCK_FORCE  2

int sqlite3rowlockIpcLockTable(IpcHandle *pHandle, int iTable, u8 eLock, int mode, u8 *prevLock);
void sqlite3rowlockIpcUnlockTablesStmtProc(IpcHandle *pHandle);
int sqlite3rowlockIpcTableDeletable(IpcHandle *pHandle, int iTable);
u8 sqlite3rowlockIpcLockTableQuery(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcUnlockTable(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcUnlockTablesProc(IpcHandle *pHandle);
void sqlite3rowlockIpcUnlockTablesAll(void);

int sqlite3rowlockIpcCachedRowidSet(IpcHandle *pHandle, int iTable, i64 rowid);
i64 sqlite3rowlockIpcCachedRowidGet(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcCachedRowidDropTable(IpcHandle *pHandle, int iTable);

#endif /* SQLITE_ROWLOCK_IPC_H */
#endif /* SQLITE_OMIT_ROWLOCK */
