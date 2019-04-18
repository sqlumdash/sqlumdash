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
#include "rowlock_os.h"

/*
** This structure is a handle to access MMAP.
**
** About mutex handle:
** On Windows, mutex handles are different values between proecesses. 
** So it is stored in this handle.
** On Linux, mutex handles must be the value between proecesses.
** So it is stored in MMAP area.
*/
typedef struct IpcHandle {
  MMAP_HANDLE hRecordLock;
  MMAP_HANDLE hTableLock;
#if SQLITE_OS_WIN
  MUTEX_HANDLE rlMutex;
  MUTEX_HANDLE tlMutex;
#endif
  void *pRecordLock; /* RowMetaData + RowElement[] */
  void *pTableLock;  /* TableMetaData + TableElement[] + CachedRowid[] */
  u64 owner; /* Owner of the lock. The pointer of Btree is set. */
} IpcHandle;


#define IPC_CLASS_ROW   0
#define IPC_CLASS_TABLE 1

typedef struct IpcClass{
  int (*xMapName)(char *buf, size_t bufSize, const char *name);
  u8 (*xIsInitialized)(void *pMap);
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


int rowlockIpcSearch(void *pMap, u8 iClass, void *pTarget, u64 hash, u64 *pIdx);
void rowlockIpcDelete(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idxEnd);

int rowlockStrCat(char *dest, size_t size, const char *src1, const char *src2);

int sqlite3rowlockIpcInit(IpcHandle *pHandle, u64 nByteRow, u64 nByteTable, const void *owner, const char *name);
void sqlite3rowlockIpcFinish(IpcHandle *pHandle);
int sqlite3rowlockIpcLockRecord(IpcHandle *pHandle, int iTable, i64 rowid);
int sqlite3rowlockIpcLockRecordQuery(IpcHandle *pHandle, int iTable, i64 rowid);
void sqlite3rowlockIpcUnlockRecord(IpcHandle *pHandle, int iTable, i64 rowid);
void sqlite3rowlockIpcUnlockRecordProc(IpcHandle *pHandle, const char *name);
void sqlite3rowlockIpcUnlockRecordAll(const char *name);

/* Mode for sqlite3rowlockIpcUnlockTableCore() */
#define MODE_LOCK_NORMAL 0
#define MODE_LOCK_COMMIT 1
#define MODE_LOCK_FORCE  2

int sqlite3rowlockIpcLockTable(IpcHandle *pHandle, int iTable, u8 eLock, int mode, u8 *prevLock);
void sqlite3rowlockIpcUnlockTablesStmtProc(IpcHandle *pHandle, const char *name);
u8 sqlite3rowlockIpcLockTableQuery(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcUnlockTable(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcUnlockTablesProc(IpcHandle *pHandle, const char *name);
void sqlite3rowlockIpcUnlockTablesAll(const char *name);

int sqlite3rowlockIpcCachedRowidSet(IpcHandle *pHandle, int iTable, i64 rowid);
i64 sqlite3rowlockIpcCachedRowidGet(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcCachedRowidDropTable(IpcHandle *pHandle, int iTable);
void sqlite3rowlockIpcCachedRowidReset(IpcHandle *pHandle, const char *name);

#endif /* SQLITE_ROWLOCK_IPC_H */
#endif /* SQLITE_OMIT_ROWLOCK */
