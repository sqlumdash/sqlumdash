/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2019 Toshiba Corporation
**
*************************************************************************
** This file implements functions for storing index information on
** Process Shared Memory shaing with the other processes.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#include <fcntl.h>
#include <errno.h>
#include "sqliteInt.h"
#include "btreeInt.h"
#include "vdbeInt.h"
#include "rowlock.h"
#include "rowlock_os.h"
#include "rowlock_psm_lock.h"
#include "rowlock_psm_hash.h"
#include "psm.h"

typedef void *(*PsmIdxMallocFunc)(void*, sqlite3_int64);
typedef void (*PsmIdxFreeFunc)(void*, void*);

#if SQLITE_OS_WIN
#define PsmLockMutex(pHandle) (&pHandle->mutex)
#else
#define PsmLockMutex(pHandle) (&pHandle->pMeta->mutex)
#endif

/* For cleanup tool */
void sqlite3_rowlock_psm_unlock_record_all(const char *name){
  sqlite3rowlockPsmUnlockRecordAll(name);
}

/*
** Blob comparator.
*/
static int blobComparator(const void *pKey1, sqlite3_int64 nKey1,
                          const void *pKey2, sqlite3_int64 nKey2,
                          const CollSeq *pColl){
  int ret;
  sqlite3_int64 n = MIN(nKey1, nKey2);

  ret = memcmp(pKey1, pKey2, n);
  if( ret==0 ){
    if( nKey1==nKey2 ){
      return 0;
    }else if( nKey1>nKey2 ){
      return 1;
    }else{
      return -1;
    }
  }else{
    return ret;
  }
}

/*
** Record comparator.
*/
static int recordComparator(const void *pKey1, sqlite3_int64 nKey1,
                            const void *pKey2, sqlite3_int64 nKey2,
                            const CollSeq *pColl){
  int rc;
  const unsigned char *aKey1 = (const unsigned char *)pKey1;
  const unsigned char *aKey2 = (const unsigned char *)pKey2;
  u32 d1, d2; 
  u32 idx1, idx2;
  u32 szHdr1, szHdr2;
  u16 u = 0;                          /* Unsigned loop counter */
  u8 enc = SQLITE_UTF8;

  idx1 = getVarint32(aKey1, szHdr1);
  idx2 = getVarint32(aKey2, szHdr2);
  d1 = szHdr1;
  d2 = szHdr2;

  if( pColl ) enc = pColl->enc;

  while( idx1<szHdr1 && d1<=(u32)nKey1 &&
         idx2<szHdr2 && d2<=(u32)nKey2){
    u32 serial_type1, serial_type2;
    Mem mem1 = {0}, mem2 = {0};
    mem1.enc = enc;
    mem2.enc = enc;

    idx1 += getVarint32(&aKey1[idx1], serial_type1);
    idx2 += getVarint32(&aKey2[idx2], serial_type2);
    d1 += sqlite3VdbeSerialGet(&aKey1[d1], serial_type1, &mem1);
    d2 += sqlite3VdbeSerialGet(&aKey2[d2], serial_type2, &mem2);

    rc = sqlite3MemCompare(&mem1, &mem2, pColl);
    if( rc!=0 ) return rc;
  }
  return 0;
}

/*
** Calculate name for PSM. It is database name + PSM_SUFFIX_INDEX.
*/
static int rowlockPsmLockName(char *buf, size_t bufSize, const char *name){
  char fullPath[MAX_PATH_LEN] = {0};
  int rc;
  sqlite3_vfs *pVfs = sqlite3_vfs_find(0);

  rc = sqlite3OsFullPathname(pVfs, name, sizeof(fullPath), fullPath);
  if( rc ) return rc;
  
  rc = rowlockStrCat(buf, bufSize, fullPath, PSM_LOCK_NAME_SUFFIX);
  if( rc!=0 ) return SQLITE_CANTOPEN;

  return rc;
}

#if SQLITE_OS_UNIX
static int rowlockPsmInitLock(const char *file){
  int fd = -1;
  /* Retry to open unti succeed. */
  do {
    fd = open(file, O_RDWR|O_CREAT|O_EXCL, 0666);  
    if( fd>=0 ) break;
    if( errno!=EEXIST ) return SQLITE_CANTOPEN;
    sqlite3_sleep(1);
  } while(1);
  close(fd);

  return SQLITE_OK;
}

static void rowlockPsmInitUnlock(const char *file){
  unlink(file);
}
#endif

/*
** Initialize a shared object for storing lock information.
** Shared memory handle must be initialized sequentially.
** On Windows, mutex handle is created on each processes.
** So this mutex can be used for locking the initialization.
** On Linux, mutex handle must be created on shared memory.
** SQLumDash gets a file lock for locking the initialization.
** Then initialize shared memory and creates mutex into it.
**
** pHandle:
**   Handle object to be initialized.
** nByte:
**   The maximum memory allocation size for psm in byte.
** name:
**  It is used for psm name with suffix. SQLite engine specifies DB full path. 
*/
int sqlite3rowlockPsmInit(PsmLockHandle *pHandle, size_t nByte,
                          const char *name){
  int rc;
  char psmName[MAX_PATH_LEN] = {0};
  char mtxName[MAX_PATH_LEN] = {0};
  MUTEX_HANDLE idxMutex = {0};
  PSMHandle psmHandle = NULL;
  PsmLockMetaData **ppMeta, *pMeta = NULL;
#if SQLITE_OS_UNIX
  char lockName[MAX_PATH_LEN] = {0};
#endif

  /*
  ** Create PSM, mutex and file names.
  ** File name is used for locking the file in order not to
  ** do initialization in parallel.
  */
  rc = rowlockPsmLockName(psmName, sizeof(psmName), name);
  if( rc ) return rc;
  rc = rowlockStrCat(mtxName, sizeof(mtxName), name, MUTEX_LOCK_NAME_SUFFIX);
  if( rc ) return rc;
#if SQLITE_OS_UNIX
  rc = rowlockStrCat(lockName, sizeof(lockName), name, INIT_LOCK_NAME_SUFFIX);
  if( rc ) return rc;

  rc = rowlockPsmInitLock(lockName);
  if( rc ) return rc;
#else
  /* Open mutex. The mutex handle is created in non-shared memory on Windows. */
  rc = rowlockOsMutexOpen(mtxName, &pHandle->mutex);
  if( rc ) return rc;

  rowlockOsMutexEnter(&pHandle->mutex);
#endif

  /* Create PSM. */
  PSMinit(psmName, nByte, NULL, &psmHandle);
  if( !psmHandle ){
    rc = SQLITE_CANTOPEN;
    goto psm_init_error;
  }

  /* Initialize an index list if I'm a first opener of shared memory. */
  ppMeta = (PsmLockMetaData**)PSMgetUser(psmHandle);
  pMeta = *ppMeta;
  if( !pMeta ){
    pMeta = (PsmLockMetaData*)PSMalloc(psmHandle, sizeof(PsmLockMetaData));
    if( !pMeta ){
      rc = SQLITE_NOMEM_BKPT;
      goto psm_init_error;
    }
#if SQLITE_OS_UNIX
    /* Open mutex. The mutex handle is created in shared memory on Linux. */
    rc = rowlockOsMutexOpen(mtxName, &pMeta->mutex);
    if( rc ) goto psm_init_error;
#endif

    /* Write the meta data address into fixed area in shared memory. */
    *ppMeta = pMeta;
    sqlite3HashI64Init(&pMeta->list);
  }
  
  pHandle->psmHandle = psmHandle;
  pHandle->pMeta = pMeta;

#if SQLITE_OS_UNIX
  rowlockPsmInitUnlock(lockName);
#else
  rowlockOsMutexLeave(&pHandle->mutex);
#endif

  return SQLITE_OK;

psm_init_error:
  if( pMeta ) PSMfree(psmHandle, pMeta);
  if( psmHandle ) PSMdeinit(psmHandle);
#if SQLITE_OS_UNIX
  rowlockPsmInitUnlock(lockName);
#else
  rowlockOsMutexLeave(&pHandle->mutex);
#endif
  return rc;
}

static int getPsmHandle(sqlite3 *db, PSMHandle *psmHandle, int index){
  assert( index<db->nDb );
  Db *pDb = NULL;
  Btree *p = NULL;
  BtreeTrans *pBtTrans = NULL;
  PsmLockHandle *pPsmHandle = NULL;

  pDb = &db->aDb[index];
  p = pDb->pBt;
  if( !p ) return SQLITE_CANTOPEN;
  pBtTrans = &p->btTrans;
  if( !pBtTrans ) return SQLITE_CANTOPEN;
  pPsmHandle = &pBtTrans->psmHandle;
  if( !pPsmHandle ) return SQLITE_CANTOPEN;
  *psmHandle = pPsmHandle->psmHandle;

  return SQLITE_OK;
}

static int setPsmHandle(sqlite3 *db, PSMHandle psmHandle, int index){
  assert( psmHandle );
  assert( index<db->nDb );
  Db *pDb = NULL;
  Btree *p = NULL;
  BtreeTrans *pBtTrans = NULL;
  PsmLockHandle *pPsmHandle = NULL;

  pDb = &db->aDb[index];
  p = pDb->pBt;
  if( !p ) return SQLITE_CANTOPEN;
  pBtTrans = &p->btTrans;
  if( !pBtTrans ) return SQLITE_CANTOPEN;
  pPsmHandle = &pBtTrans->psmHandle;
  if( !pPsmHandle ) return SQLITE_CANTOPEN;
  pPsmHandle->psmHandle = psmHandle;

  return SQLITE_OK;
}

/*
** Prepare to inherit a PSM handle to child process.
*/
int sqlite3_prepare_inherit(sqlite3 *db){
  int i;
  int rc = SQLITE_OK;
  PSMHandle psmHandle = 0;
  for(i=0; i<db->nDb; i++){
    rc = getPsmHandle(db, &psmHandle, i);
    if( rc==SQLITE_OK ){
      psmHandle = PSMprepareInherit(psmHandle);
      if( psmHandle ) setPsmHandle(db, psmHandle, i);
      else return SQLITE_ERROR;
    }
  }
  return SQLITE_OK;
}

/*
** Validate a inherited PSM handle in child process.
*/
int sqlite3_execute_inherit(sqlite3 *db){
  int i;
  int rc = SQLITE_OK;
  PSMHandle psmHandle = 0;
  for(i=0; i<db->nDb; i++){
    rc = getPsmHandle(db, &psmHandle, i);
    if( rc==SQLITE_OK ){
      psmHandle = PSMexecuteInherit(psmHandle);
      if( psmHandle ) setPsmHandle(db, psmHandle, i);
      else return SQLITE_ERROR;
    }
  }
  return SQLITE_OK;
}


/*
** Cancel a preparation of inheritance preparation.
** If fork() was failed after sqlite3_prepare_inherit(), this API should be called.
** If you want to cancel 2 preparation, you need to call this API 2 times.
*/
int sqlite3_cancel_inherit(sqlite3 *db){
  int i;
  int rc = SQLITE_OK;
  PSMHandle psmHandle = 0;
  for(i=0; i<db->nDb; i++){
    rc = getPsmHandle(db, &psmHandle, i);
    if( rc==SQLITE_OK ) PSMcancelInherit(psmHandle);
  }
  return SQLITE_OK;
}

void sqlite3rowlockPsmFinish(PsmLockHandle *pHandle){
  if( !pHandle->psmHandle ) return;
  /* Do not close mutex if it is saved in shared memory */
#if SQLITE_OS_WIN
  rowlockOsMutexClose(PsmLockMutex(pHandle));
#endif
  PSMdeinit(pHandle->psmHandle);
}

/*
** Create a hash in shared memory.
** pHandle:
**   Handle object to be initialized.
** iTable:
**   Index number.
*/
int sqlite3rowlockPsmCreateTable(PsmLockHandle *pHandle, int iTable){
  int rc = SQLITE_OK;
  PSMHandle psmHandle = pHandle->psmHandle;
  PsmLockMetaData *pMeta = pHandle->pMeta;
  HashI64 *pList = &pMeta->list;
  HashBlob *pOld;
  
  rowlockOsMutexEnter(PsmLockMutex(pHandle));

  pOld = (HashBlob*)sqlite3HashI64Find(pList, iTable);
  if( !pOld ){
    /* Create new hash if it does not exist. */
    HashBlob *pNew;
    pNew = (HashBlob*)PSMalloc(psmHandle, sizeof(HashBlob));
    if( !pNew ){
      rc = SQLITE_NOMEM;
      goto psm_create_table_end;
    }

    sqlite3HashBlobInit(pNew);
    pOld = (HashBlob*)sqlite3HashI64Insert(pList, iTable, pNew, psmHandle,
                                            (PsmIdxMallocFunc)PSMalloc,
                                            (PsmIdxFreeFunc)PSMfree);
    assert( !pOld );
  }

psm_create_table_end:
  rowlockOsMutexLeave(PsmLockMutex(pHandle));
  return rc;
}

/*
** Drop a hash in shared memory.
** pHandle:
**   Handle object to be initialized.
** iTable:
**   Index number.
*/
void sqlite3rowlockPsmDropTable(PsmLockHandle *pHandle, int iTable){
  PSMHandle psmHandle = pHandle->psmHandle;
  PsmLockMetaData *pMeta = pHandle->pMeta;
  HashI64 *pList = &pMeta->list;
  HashBlob *pOld;

  rowlockOsMutexEnter(PsmLockMutex(pHandle));

  /* Drop a table if no one uses it. */
  if( pList->count==0 ){
    pOld = (HashBlob*)sqlite3HashI64Insert(pList, iTable, NULL, NULL,
                                           (PsmIdxMallocFunc)PSMalloc,
                                           (PsmIdxFreeFunc)PSMfree);
    if( pOld ) PSMfree(psmHandle, pOld);
  }

  rowlockOsMutexLeave(PsmLockMutex(pHandle));
}

/*
** Lock a record.
** Return SQLITE_DONE if a record is locked by myself.
** Return SQLITE_LOCKED if a record is locked by another user.
** Return SQLITE_OK if a record is locked successfully.
*/
int sqlite3rowlockPsmLockRecord(PsmLockHandle *pHandle, int iTable,
                                const void *pKey, int nKey,
                                void *owner, const CollSeq *pColl){
  int rc;
  PSMHandle psmHandle = pHandle->psmHandle;
  PsmLockMetaData *pMeta = pHandle->pMeta;
  HashI64 *pList = &pMeta->list;
  PID pid = rowlockGetPid();
  HashBlob *pTable;
  PsmIdxElem *pElem;

  rowlockOsMutexEnter(PsmLockMutex(pHandle));

  pTable = (HashBlob*)sqlite3HashI64Find(pList, iTable);
  assert( pTable );

  pElem = (PsmIdxElem*)sqlite3HashBlobFind(pTable, pKey, nKey,
                                           recordComparator, pColl);
  if( pElem ){
    if( pElem->pid==pid && pElem->owner==(u64)owner ){
      /* I have the lock. */
      rc = SQLITE_DONE;
    }else{
      /* Someone has the lock. */
      rc = SQLITE_LOCKED;
    }
  }else{
    /* No one lock the record. I can lock it. */
    PsmIdxElem *pNew = (PsmIdxElem*)PSMalloc(psmHandle, sizeof(PsmIdxElem));
    if( !pNew ){
      rc = SQLITE_NOMEM;
      goto psm_lock_record_end;
    }

    pNew->owner = (u64)owner;
    pNew->pid = pid;
    pElem = (PsmIdxElem*)sqlite3HashBlobInsert(pTable, pKey, nKey,
                                               pNew, psmHandle,
                                               (PsmIdxMallocFunc)PSMalloc,
                                               (PsmIdxFreeFunc)PSMfree,
                                               recordComparator, pColl);
    assert( !pElem );
    rc = SQLITE_OK;
  }

psm_lock_record_end:
  rowlockOsMutexLeave(PsmLockMutex(pHandle));
  return rc;
}

/* Check if a record is locked by another user or not.
** Return SQLITE_LOCKED if a record is locked by another user.
** Return SQLITE_OK if record is not locked or lock owner is me.
*/
int sqlite3rowlockPsmLockRecordQuery(PsmLockHandle *pHandle, int iTable,
                                     const void *pKey, int nKey,
                                     void *owner, const CollSeq *pColl){
  PSMHandle psmHandle = pHandle->psmHandle;
  PsmLockMetaData *pMeta = pHandle->pMeta;
  HashI64 *pList = &pMeta->list;
  HashBlob *pTable;
  PsmIdxElem *pElem;

  rowlockOsMutexEnter(PsmLockMutex(pHandle));

  pTable = (HashBlob*)sqlite3HashI64Find(pList, iTable);
  assert( pTable );

  pElem = (PsmIdxElem*)sqlite3HashBlobFind(pTable, pKey, nKey,
                                           recordComparator, pColl);
  rowlockOsMutexLeave(PsmLockMutex(pHandle));

  if( pElem ){
    PID pid = rowlockGetPid();
    if( pElem->pid==pid && pElem->owner==(u64)owner ){
      /* I have the lock. */
      return SQLITE_DONE;
    }else{
      /* Someone has the lock. */
      return SQLITE_LOCKED;
    }
  }else{
    /* No one lock the index. */
    return SQLITE_OK;
  }
}

/*
** Unlock an record.
*/
void sqlite3rowlockPsmUnlockRecord(PsmLockHandle *pHandle, int iTable,
                                   const void *pKey, int nKey,
                                   void *owner, const CollSeq *pColl){
  PSMHandle psmHandle = pHandle->psmHandle;
  PsmLockMetaData *pMeta = pHandle->pMeta;
  HashI64 *pList = &pMeta->list;
  PID pid = rowlockGetPid();
  HashBlob *pTable;
  PsmIdxElem *pOld;

  rowlockOsMutexEnter(PsmLockMutex(pHandle));

  pTable = (HashBlob*)sqlite3HashI64Find(pList, iTable);
  assert( pTable );

  pOld = (PsmIdxElem*)sqlite3HashBlobInsert(pTable, pKey, nKey, NULL, psmHandle,
                                            (PsmIdxMallocFunc)PSMalloc,
                                            (PsmIdxFreeFunc)PSMfree,
                                            recordComparator, pColl);
  if( pOld ) PSMfree(psmHandle, pOld);

  rowlockOsMutexLeave(PsmLockMutex(pHandle));
}

/* 
** Release all locks acquired by the specified process.
** Process ID can be specified by the argument.
** If Process ID is 0, locks are unlocked regardless process ID.
** Process ID=0, owner=0:  Called by rowlock cleaner.
** Process ID=0, owner!=0: No case.
** Process ID>0, owner=0:  Called by SQLite engine when dll is unloaded.
** Process ID>0, owner!=0: Called by SQLite engine at transaction or statement is closing.
*/
void sqlite3rowlockPsmUnlockRecordProcCore(PsmLockHandle *pHandle, PID pid, u64 owner,
                                           const char *name){
  PsmLockHandle lockHandle = {0};
  int opened = 0;
  HashI64 *pList;
  HashElemI64 *i;

  assert( pid!=0 || owner!=0 );
  assert( pHandle || name );

  if( !pHandle ){
    int rc = sqlite3rowlockPsmInit(&lockHandle, ROWLOCK_DEFAULT_PSM_INDEX_SIZE, name);
    assert( rc==SQLITE_OK );
    pHandle = &lockHandle;
    opened = 1;
  }

  rowlockOsMutexEnter(PsmLockMutex(pHandle));

  /* Travers all indexes in the hash. */
  pList = &pHandle->pMeta->list;
  for(i=sqliteHashI64First(pList); i; i=sqliteHashI64Next(i)){
    HashBlob *pTable = (HashBlob*)sqliteHashData(i);
    HashElemBlob *j;
    /* Travers all elements in the hash. */
    j = sqliteHashBlobFirst(pTable);
    while( j ){
      PsmIdxElem *pData = (PsmIdxElem*)sqliteHashBlobData(j);
      HashElemBlob *next = sqliteHashBlobNext(j);
      if( (pid==0 || pData->pid==pid) && (owner==0 || pData->owner==owner) ){
        sqlite3HashBlobRemoveElement(pTable, j, pHandle->psmHandle,
                                     (PsmIdxFreeFunc)PSMfree);
        PSMfree(pHandle->psmHandle, pData);
      }
      j = next;
    }
  }

  rowlockOsMutexLeave(PsmLockMutex(pHandle));

  /* Close the handle if it was opend in this function. */
  if( opened==1 ){
    sqlite3rowlockPsmFinish(&lockHandle);
  }
}

/* Unlock all lock information owned by this process. */
void sqlite3rowlockPsmUnlockRecordProc(PsmLockHandle *pHandle, u64 owner,
                                      const char *name){
  PID pid = rowlockGetPid();
  sqlite3rowlockPsmUnlockRecordProcCore(pHandle, pid, owner, name);
}

/* Unlock all lock information regardless of owner. */
void sqlite3rowlockPsmUnlockRecordAll(const char *name){
  sqlite3rowlockPsmUnlockRecordProcCore(NULL, 0, 0, name);
}


#endif /* SQLITE_OMIT_ROWLOCK */
