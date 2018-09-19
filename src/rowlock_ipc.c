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
#include "btreeInt.h"
#include "rowlock_ipc.h"
#include "rowlock_ipc_table.h"
#include "rowlock_ipc_row.h"

#define MMAP_NAME_ROWLOCK "ROWLOCK_MAP"
#define MMAP_NAME_TABLELOCK "TABLELOCK_MAP"

#if SQLITE_OS_WIN
#include <windows.h>
#define MUTEX_NAME_ROWLOCK TEXT("ROWLOCK_MUTEX")
#define MUTEX_NAME_TABLELOCK TEXT("TABLELOCK_MUTEX")
#define rowlockGetPid GetCurrentProcessId
#define rowlockGetTid GetCurrentThreadId
#else
#define rowlockGetPid getpid
#define rowlockGetTid gettid
#endif

#if SQLITE_OS_WIN
#define IpcRowLockMutex() (HANDLE)pHandle->rlMutex
#else
#define IpcRowLockMutex() (pthread_mutex_t)(RowMetaData())->rlMutex
#endif

#define NEXT_IDX(n) ((n + pMeta->nElement + 1) % pMeta->nElement)
#define PREV_IDX(n) ((n + pMeta->nElement - 1) % pMeta->nElement)

IpcClass ipcClasses[] = {
  {rowClassInitArea, rowClassElemCount, rowClassIsValid, rowClassElemIsTarget, rowClassElemGet, rowClassElemHash, rowClassElemClear, rowClassElemCopy, rowClassIndexPrev, rowClassIndexNext, rowClassCalcHash},
  {tableClassInitArea, tableClassElemCount, tableClassIsValid, tableClassElemIsTarget, tableClassElemGet, tableClassElemHash, tableClassElemClear, tableClassElemCopy, tableClassIndexPrev, tableClassIndexNext, tableClassCalcHash},
};

/**********************************************************************/
/* For testing */
int sqlite3_rowlock_ipc_init(IpcHandle *pHandle, sqlite3_uint64 nByteRow, sqlite3_uint64 nByteTable, const void *owner){
  return sqlite3rowlockIpcInit(pHandle, nByteRow, nByteTable, owner);
}

void sqlite3_rowlock_ipc_finish(IpcHandle *pHandle){
  sqlite3rowlockIpcFinish(pHandle);
}

int sqlite3_rowlock_ipc_lock_record(IpcHandle *pHandle, int iTable, sqlite3_int64 rowid){
  return sqlite3rowlockIpcLockRecord(pHandle, iTable, rowid);
}

void sqlite3_rowlock_ipc_unlock_record(IpcHandle *pHandle, int iTable, sqlite3_int64 rowid){
  sqlite3rowlockIpcUnlockRecord(pHandle, iTable, rowid);
}

void sqlite3_rowlock_ipc_unlock_record_proc(IpcHandle *pHandle){
  sqlite3rowlockIpcUnlockRecordProc(pHandle);
}

int sqlite3_rowlock_ipc_lock_table(IpcHandle *pHandle, int iTable, unsigned char eLock){
  return sqlite3rowlockIpcLockTable(pHandle, iTable, eLock, MODE_LOCK_NORMAL);
}

unsigned char sqlite3_rowlock_ipc_lock_table_query(IpcHandle *pHandle, int iTable){
  return sqlite3rowlockIpcLockTableQuery(pHandle, iTable);
}

void sqlite3_rowlock_ipc_unlock_table(IpcHandle *pHandle, int iTable){
  sqlite3rowlockIpcUnlockTable(pHandle, iTable);
}

void sqlite3_rowlock_ipc_register_hash_func(int iClass, sqlite3_uint64(*xFunc)(void *pMap, ...)){
  if( xFunc ){
    ipcClasses[iClass].xCalcHash = xFunc;
  }else{
    u64(*hashFunc[])(void *pMap, ...) = {rowClassCalcHash, tableClassCalcHash};
    ipcClasses[iClass].xCalcHash = hashFunc[iClass];
  }
}
/**********************************************************************/

/*
** Create recursive mutex. It is used when it refers a shared object of row lock.
** On Windows, mutex handles are different values between proecesses. So it is stored in IpcHandle.
** On Linux, mutex handles are same value between proecesses. So it is stored in RowMetaData.
*/
#if SQLITE_OS_WIN
static int rowlockIpcMutexCreate(MUTEX_HANDLE *pMutex, LPCSTR name){
  HANDLE mtx;
  SECURITY_DESCRIPTOR secDesc;
  SECURITY_ATTRIBUTES secAttr;

  InitializeSecurityDescriptor(&secDesc,SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl(&secDesc, TRUE, 0, FALSE);	    
  secAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  secAttr.lpSecurityDescriptor = &secDesc;
  secAttr.bInheritHandle = TRUE; 

  mtx = CreateMutex(&secAttr, FALSE, name);
  if( mtx==NULL ){
    return SQLITE_ERROR;
  }

  *pMutex = mtx;
#else
static int rowlockIpcMutexCreate(MUTEX_HANDLE *pMutex){
  int ret;
  pthread_mutexattr_t mtxattr;

  pthread_mutexattr_init(&mtxattr);

  /* Enable to use mutex for sharing between processes */
  ret = pthread_mutexattr_setpshared(&mtxattr, PTHREAD_PROCESS_SHARED);
  if( ret!=0 ){
    return SQLITE_ERROR;
  }

  *pMutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_init(pMutex, &mtxattr);
#endif

  return SQLITE_OK;
}

void rowlockIpcMutexLock(MUTEX_HANDLE mutex){
#if SQLITE_OS_WIN
  WaitForSingleObject(mutex, INFINITE);
#else
  pthread_mutex_lock(mutex);
#endif
}

void rowlockIpcMutexUnlock(MUTEX_HANDLE mutex){
#if SQLITE_OS_WIN
  ReleaseMutex(mutex);
#else
  pthread_mutex_unlock(mutex);
#endif
}

#if SQLITE_OS_WIN
static int rowlockIpcCreate(u8 iClass, u64 allocSize, LPCSTR name, HANDLE *phMap, void **ppMap){
  HANDLE hMap;
#else
static int rowlockIpcCreate(u8 iClass, u64 allocSize, char *name, void **ppMap){
  int fd;
#endif
  IpcClass *xClass = &ipcClasses[iClass];
  void *pMap = NULL;
  int exists;

#if SQLITE_OS_WIN
  hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, allocSize>>32, (DWORD)allocSize, name);
  if( !hMap ) return SQLITE_CANTOPEN_BKPT;

  exists = (GetLastError() == ERROR_ALREADY_EXISTS);

  pMap = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0);
  if( !pMap ){
    CloseHandle(hMap);
    return SQLITE_IOERR_SHMMAP;
  }

  *phMap = hMap;
  *ppMap = pMap;
#else
#error not implemented
  /* mutex */

  /* Check file existence. */
  if( !exists ){
    fd = create
    ret = lseek(fd, allocSize, SEEK_SET);
    if( ret<0 ) return SQLITE_IOERR_SEEK;
    ret = write(fd, &c, sizeof(char));
    if( ret==-1 ) return SQLITE_IOERR_WRITE;
  }else{
    fd = open(name, O_RDWR, 0666);
    if( fd<0 ) return SQLITE_CANTOPEN_BKPT;
  }

  pMap = mmap(NULL, allocSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if( pMap==MAP_FAILED ){
    close(fd);
    return SQLITE_IOERR_SHMMAP;
  }

  *ppMap = pMap;
#endif

  if( !exists ){
    xClass->xInitArea(pMap, allocSize);
  }

  return SQLITE_OK;
}

#if SQLITE_OS_WIN
static void rowlockIpcClose(HANDLE hMap, void *pMap, HANDLE hMutex){
  if( pMap ) {
    UnmapViewOfFile(pMap);
    CloseHandle(hMap);
  }
  CloseHandle(hMutex);
}
#else
static int rowlockIpcClose(){
#error not implemented
}
#endif

/*
** Initialize a shared object used by multi processes.
** nByte: the maximum memory allocation size in byte.
** Shared object consists of RowElementMeta and an array of RowElement structure.
** [RowElementMeta][RowElement(0)][RowElement(1)]...[RowElement(nElement-1)]
** "owner" is used to specify the lock owner in the same process and thread.
** pBtree is set to "owner" when sqlite3rowlockIpcLockRecord() is called by SQLite engine.
*/
int sqlite3rowlockIpcInit(IpcHandle *pHandle, u64 nByteRow, u64 nByteTable, const void *owner){
  int rc;
  u64 nElemRow;
  u64 nElemTable;
  u64 nAllocRow;
  u64 nAllocTable;
#if SQLITE_OS_WIN
  HANDLE hRecordLock = NULL, hTableLock = NULL;
  void *pRecordLock = NULL, *pTableLock = NULL;
  HANDLE rlMutex = NULL, tlMutex = NULL;
#endif

  nElemRow = (nByteRow - sizeof(RowMetaData)) / sizeof(RowElement);
  nElemTable = (nByteTable - sizeof(TableMetaData)) / (sizeof(TableElement) + sizeof(CachedRowid));

  nAllocRow = sizeof(RowMetaData) + sizeof(RowElement) * nElemRow;
  nAllocTable = sizeof(TableMetaData) + (sizeof(TableElement) + sizeof(CachedRowid)) * nElemTable;

#if SQLITE_OS_WIN
  rc = rowlockIpcCreate(IPC_CLASS_ROW, nAllocRow, TEXT(MMAP_NAME_ROWLOCK), &hRecordLock, &pRecordLock);
  if( rc ) return rc;
  rc = rowlockIpcCreate(IPC_CLASS_TABLE, nAllocTable, TEXT(MMAP_NAME_TABLELOCK), &hTableLock, &pTableLock);
  if( rc ) goto ipc_init_failed;

  rc = rowlockIpcMutexCreate(&rlMutex, MUTEX_NAME_ROWLOCK);
  if( rc ) goto ipc_init_failed;
  rc = rowlockIpcMutexCreate(&tlMutex, MUTEX_NAME_TABLELOCK);
  if( rc ) goto ipc_init_failed;
#else
#error not implemented

  RowMetaData *pMeta = (RowMetaData*)pHandle->pRecordLock;
  if( pMeta->nElement==0 ){
    rc = rowlockIpcMutexCreate(&pMeta->rlMutex);
    if( rc ) goto ipc_init_failed;
  }
  TableMetaData *pMeta = (TableMetaData*)pHandle->pTableLock;
  if( pMeta->nElement==0 ){
    rc = rowlockIpcMutexCreate(&pMeta->tlMutex);
    if( rc ) goto ipc_init_failed;
  }
#endif


  /* Set output variable. */
  pHandle->pRecordLock = pRecordLock;
  pHandle->pTableLock = pTableLock; 
  pHandle->owner = (u64)owner;
#if SQLITE_OS_WIN
  pHandle->hRecordLock = hRecordLock;
  pHandle->hTableLock = hTableLock;
  pHandle->rlMutex = rlMutex;
  pHandle->tlMutex = tlMutex;
#endif

  return SQLITE_OK;

ipc_init_failed:
  rowlockIpcClose(hRecordLock, pRecordLock, rlMutex);
  rowlockIpcClose(hTableLock, pTableLock, tlMutex);
  return rc;
}

void sqlite3rowlockIpcFinish(IpcHandle *pHandle){
  sqlite3rowlockIpcUnlockRecordProc(pHandle);
  sqlite3rowlockIpcUnlockTablesProc(pHandle);
#if SQLITE_OS_WIN
  rowlockIpcClose(pHandle->hRecordLock, pHandle->pRecordLock, pHandle->rlMutex);
  rowlockIpcClose(pHandle->hTableLock, pHandle->pTableLock, pHandle->tlMutex);
#else
#error not implemented
#endif
  memset(pHandle, 0, sizeof(IpcHandle));
}

/* Calculate hash value. */
u64 rowlockIpcCalcHash(u64 nBucket, unsigned char *buf, u32 len){
  u64 h = 0;
  u32 i;

  /* The following code is refered by strHash() in hash.c. */
  for(i=0; i<len; i++){
    /* Knuth multiplicative hashing.  (Sorting & Searching, p. 510).
    ** 0x9e3779b1 is 2654435761 which is the closest prime number to
    ** (2**32)*golden_ratio, where golden_ratio = (sqrt(5) - 1)/2. */
    h += (unsigned char)buf[i];
    h *= 0x9e3779b1;
  }

  return h % nBucket;
}

/*
** Search row is locked or not.
** Return SQLITE_LOCKED if row is locked by myself or another user.
** Return SQLITE_OK if row is not locked. New entry should be store at *pIdx.
** Return SQLITE_NOMEM if row is not locked and it is impossible to store new entry.
*/
int rowlockIpcSearch(void *pMap, u8 iClass, void *pTarget, u64 hash, u64 *pIdx){
  int rc = SQLITE_OK;
  u64 idx = hash;
  IpcClass *xClass = &ipcClasses[iClass];
  void *pElem = xClass->xElemGet(pMap, idx);

  /* Search until element is not valid. */
  while( xClass->xElemIsValid(pElem) ){
    /* Check if the element is a target. */
    if( xClass->xElemIsTarget(pElem,pTarget) ){
      *pIdx = idx;
      return SQLITE_LOCKED;
    }
    idx = xClass->xIndexNext(pMap, idx);
    if( idx == hash ) {
      /* All entries are checked. */
      return SQLITE_NOMEM_BKPT;
    }
    pElem = xClass->xElemGet(pMap, idx);
  }

  *pIdx = idx;
  return SQLITE_OK;
}

static int isTargetPattern1and2(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idx){
  IpcClass *xClass = &ipcClasses[iClass];
  u64 hash = xClass->xElemHash(pMap, idx);

  if( idxStart<=hash && hash<=idxDel ){
    return 1;
  }else{
    return 0;
  }
}

static int isTargetPattern3(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idx){
  IpcClass *xClass = &ipcClasses[iClass];
  u64 hash = xClass->xElemHash(pMap, idx);

  if( hash<=idxDel || idxStart<=hash ){
    return 1;
  }else{
    return 0;
  }
}

/*
** Delete an element
*/
void rowlockIpcDelete(void *pMap, u8 iClass, u64 idxStart, u64 idxDel, u64 idxEnd){
  IpcClass *xClass = &ipcClasses[iClass];
  u64 idx;
  int (*xIsTarget)(void*,u8,u64,u64,u64);

  /* Search empty element. */
  idxStart = xClass->xIndexPrev(pMap,idxStart);
  while( xClass->xElemIsValid(xClass->xElemGet(pMap,idxStart)) ){
    idxStart = xClass->xIndexPrev(pMap, idxStart);
    if( idxStart == idxEnd ) {
      /* There is no empty element. */
      break;
    }
  }
  idxStart = xClass->xIndexNext(pMap, idxStart);
  /* Here, there is no empty element between idxStart and idxDel. */

  /* 
  ** Search a moving element.
  ** Range for the search is from idxEnd to idxDel.
  ** Acceptable element for the moving target is idxStart's hash <= hash <= idxDel's hash.
  ** There are 3 patterns.
  **
  ** pElem[0]                            pElem[N-1]
  ** 1. |--------|--------|--------|--------|
  **           Start     Del      End
  **                      <--------> Searching range
  **             <--------> Acceptable hash value
  **
  ** 2. |--------|--------|--------|--------|
  **            End     Start     Del
  **    <-------->                 <--------> Searching range
  **                      <--------> Acceptable hash value
  **
  ** 3. |--------|--------|--------|--------|
  **            Del      End     Start
  **             <--------> Searching range
  **    <-------->                 <--------> Acceptable hash value
  **
  */
  if( (idxStart<=idxDel && idxDel<=idxEnd) ||
      (idxEnd<=idxStart && idxStart<=idxDel) ){
    xIsTarget = isTargetPattern1and2;
  }else{
    assert( idxDel<=idxEnd && idxEnd<=idxStart );
    xIsTarget = isTargetPattern3;
  }

  for( idx=idxEnd; idx!=idxDel; idx=xClass->xIndexPrev(pMap,idx) ){
    if( xIsTarget(pMap, iClass, idxStart, idxDel, idx) ){
      break;
    }
  }
  /* Not found */
  if( idx == idxDel ) {
    xClass->xElemClear(pMap, idxDel);
  }else{
    xClass->xElemCopy(pMap, idxDel, idx);
    rowlockIpcDelete(pMap, iClass, idxStart, idx, idxEnd);
  }

  return;
}



#endif /* SQLITE_OMIT_ROWLOCK */
