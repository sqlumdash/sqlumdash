/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the main implementation of row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#include "btreeInt.h"
#include "vdbeInt.h"
#include "rowlock_hash.h"
#include "rowlock_ipc.h"
#include "rowlock.h"
#include <signal.h>

static u8 cachedRowidFlagGet(BtCursor *pCur);
int btreeValidTableKey(BtCursor *, i64 *, int);

static void rowlockIpcCleanup(void){
  BtShared *pBt;
  for(pBt=sharedCacheListGet(); pBt; pBt=pBt->pNext){
    const char *dbFullPath = sqlite3PagerFilename(pBt->pPager, 0);
    sqlite3rowlockIpcUnlockRecordProc(NULL, dbFullPath);
    sqlite3rowlockIpcUnlockTablesProc(NULL, dbFullPath);
    sqlite3rowlockIpcCachedRowidReset(NULL, dbFullPath);
  }
}

/* Launched when a signal is catched. */
#if SQLITE_OS_WIN
static void rowlockSignalHandler(int signal){
#else
static void rowlockSignalHandler(int signal, siginfo_t *info, void *ctx) {
#endif
  switch( signal ){
    case SIGINT:
    case SIGILL: 
    case SIGFPE: 
    case SIGSEGV: 
    case SIGTERM: 
#if SQLITE_OS_WIN
    case SIGBREAK: 
#endif
    case SIGABRT:
      rowlockIpcCleanup();
      break;
  }
}

/* Setting of signal handler */
static int rowlockSetSignalAction(){
  int signals[] = {SIGINT, SIGILL, SIGFPE, SIGSEGV, SIGTERM, SIGABRT
#if SQLITE_OS_WIN
    ,SIGBREAK
#endif
  };

  return rowlockOsSetSignalAction(signals, sizeof(signals)/sizeof(int), &rowlockSignalHandler);
}

/*
** Initialize rowlock feature. It must be called before sqlite3_open().
** It is called automatically when this library is loaded or sqlite_initialize for the test.
*/
int rowlockInitialize(){
  int ret;
  sqlite3_enable_shared_cache(1);
  ret = rowlockSetSignalAction();
  if( ret!=EXIT_SUCCESS ) return SQLITE_ERROR;
  return SQLITE_OK;
}

/* 
** The following functions are called when library is loaded or unloaded.
** Row lock feature requires to enable shared cache. 
*/
#if SQLITE_OS_WIN
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD fdwReason, LPVOID lpvReserved){
  int rc;
  switch (fdwReason) {
    /* DLL is loaded */
    case DLL_PROCESS_ATTACH:
      rc = rowlockInitialize();
      if( rc!=SQLITE_OK ) return FALSE;
      break;
    /* New thread is creating */
    case DLL_THREAD_ATTACH:
      break;
    /* New thread is closing */
    case DLL_THREAD_DETACH:
      break;
    /* DLL is unloaded */
    case DLL_PROCESS_DETACH:
      rowlockIpcCleanup();
      break;
  }

  return TRUE;
}
#else
__attribute__((constructor)) static void constructor(){
  rowlockInitialize();
}

__attribute__((destructor)) static void destructor(){
  rowlockIpcCleanup();
}
#endif

/* Concatenate two strings and store it into buffer.
** Return 0 if successful.
** Return 1 if buffer is too small.
*/
int rowlockStrCat(char *dest, size_t size, const char *src1, const char *src2){
  size_t len = strlen(src1) + strlen(src2) + 1;
  if( len>size ) return 1;
  xSnprintf(dest, size, "%s%s", src1, src2);
  return 0;
}

/* Return 1 if the btree uses a transaction btree. */
static int transBtreeIsUsed(Btree *pBtree){
  BtreeTrans *pBtTrans = &pBtree->btTrans;
  return (pBtTrans->pBtree != NULL);
}

/* Return 1 if the cursor uses a transaction cursor. */
static int transBtreeCursorIsUsed(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  return (pCurTrans->state != CURSOR_NOT_USE);
}

/* Return 1 if the shared cursor is using. */
#define cursorSharedIsUsed(pCur) ((pCur->state&CURSOR_USE_SHARED)!=0)

/* Return 1 if the transaction cursor is using. */
#define cursorTransIsUsed(pCur) \
  (pCur!=sqlite3BtreeFakeValidCursor() && \
   (pCur->btCurTrans.state&CURSOR_USE_TRANS)!=0)

void *rowlockDefaultMalloc(void *allocator, sqlite3_int64 n){
  return sqlite3_malloc((int)n);
}

void rowlockDefaultFree(void *allocator, void *p){
  sqlite3_free(p);
}

/* 
** Initialize a root page mapping which is a map of root page 
** between shared btree and transaction btree.
** Its structure is hash. Key is a root page number of shared
** btree. Value is a instance of TransRootPage structure.
*/
static void transRootPagesInit(HashI64 *pTransRootPages){
  sqlite3HashI64Init(pTransRootPages);
}

/* Finalize a root page mapping. */
static void transRootPagesFinish(HashI64 *pTransRootPages){
  HashElemI64 *elem;
  
  if( !pTransRootPages ) return;
  
  /* Cleanup for every element in a hash. */
  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    i64 iKey = sqliteHashI64Key(elem);
    TransRootPage *pData = (TransRootPage*)sqliteHashI64Data(elem);
    sqlite3KeyInfoUnref(pData->pKeyInfo);
    sqlite3_free(pData);
    /* Remove the element from hash. */
    sqlite3HashI64Insert(pTransRootPages, iKey, NULL, NULL,
                         rowlockDefaultMalloc,
                         rowlockDefaultFree);
    elem = sqliteHashI64First(pTransRootPages);
  }
  sqlite3HashI64Clear(pTransRootPages, NULL, rowlockDefaultFree);
}

/*
** Generate a file path for shared memory db.
** Shared database is named ":memory:". We cannot use the name for
** MMAP file name such as ".\:memory:-rowlock". Therefore we generates
** file name as "memory-<PID>.db". <PID> is process ID.
*/
int rowlockMemoryDbFileName(sqlite3_vfs *pVfs, char **ppzName){
  int rc;
  PID pid = rowlockGetPid();
  char zFilename[21] = {0}; /* Max PID digits + "memory-.db\0" */
  int nFullPathname = pVfs->mxPathname+1;
  char *zFullPathname = (char*)sqlite3Malloc(nFullPathname);

  if( !zFullPathname ) return SQLITE_NOMEM_BKPT;

  sqlite3_snprintf(sizeof(zFilename), zFilename, "memory-%d.db", pid);
  rc = sqlite3OsFullPathname(pVfs, zFilename,
                             nFullPathname, zFullPathname);
  if( rc ){
    sqlite3_free(zFullPathname);
    return rc;
  }
  *ppzName = zFullPathname;
  return SQLITE_OK;
}

/*
** Open transaction databases.
*/
static int sqlite3TransBtreeOpen(
  Btree *pBtree,        /* Pointer to Btree object created in */
  int flags,            /* Flags used to open btree */
  int vfsFlags,         /* vfsFlags used to open btree */
  sqlite3_vfs *pVfs
){
  int rc;
  sqlite3 *db = pBtree->db;
  static const int transFlags =
    BTREE_TRANS;
  static const int transVfsFlags =
    SQLITE_OPEN_READWRITE |
    SQLITE_OPEN_CREATE |
    SQLITE_OPEN_EXCLUSIVE |
    SQLITE_OPEN_DELETEONCLOSE |
    SQLITE_OPEN_TEMP_DB |
    SQLITE_OPEN_MEMORY;
  BtreeTrans *pBtTrans = &pBtree->btTrans;
  const char *dbFullPath;
  char *pFree = NULL;

  /*
  ** Only main database should manage data in transaction btree.
  ** Don't need to create trans Btree for Ephemeral or non-shareable.
  */
  if( (vfsFlags & SQLITE_OPEN_MAIN_DB)==0 || !pBtree->sharable){
    pBtTrans->pBtree = NULL;
    return SQLITE_OK;
  }

  /* 
  ** Do not create transaction btree when pager is not asociated with
  ** file. This situation occurs by "ATTACH DATABASE '' AS dbname"
  ** which is executed by VACUUM.
  */
  dbFullPath = sqlite3PagerFilename(pBtree->pBt->pPager, 0);
  if( sqlite3_stricmp(dbFullPath, "")== 0 ) return SQLITE_OK;

  if( strcmp(dbFullPath, ":memory:")==0 ){
    rc = rowlockMemoryDbFileName(pVfs, &pFree);
    if( rc ) return rc;
    dbFullPath = pFree;
  }

  /* Open transaction Btree for Inserted record. */
  rc = sqlite3BtreeOpenOriginal(db->pVfs, 0, db, &pBtTrans->pBtree,
    transFlags, transVfsFlags);
  if( rc ) goto trans_btree_open_failed1;

  /* Initialize BtreeTrans members. */
  memset(&pBtTrans->lockSavepoint, 0, sizeof(RowLockSavepoint));
  rc = sqlite3rowlockIpcInit(&pBtTrans->ipcHandle, sqlite3GlobalConfig.szMmapRowLock, sqlite3GlobalConfig.szMmapTableLock, pBtree, dbFullPath);
  if( rc ) goto trans_btree_open_failed2;

  rc = sqlite3rowlockPsmInit(&pBtTrans->psmHandle, ROWLOCK_DEFAULT_PSM_INDEX_SIZE, dbFullPath);
  if( rc ) goto trans_btree_open_failed3;

  sqlite3DbFree(db, pFree);
  pFree = NULL;

  transRootPagesInit(&pBtTrans->rootPages);

  return SQLITE_OK;

trans_btree_open_failed3:
  sqlite3rowlockIpcFinish(&pBtTrans->ipcHandle);
  memset(&pBtTrans->ipcHandle, 0, sizeof(IpcHandle));
trans_btree_open_failed2:
  sqlite3BtreeCloseOriginal(pBtTrans->pBtree);
  pBtTrans->pBtree = NULL;
trans_btree_open_failed1:
  sqlite3_free(pFree);
  return rc;
}

/* 
** Open shared btree and transaction btree.
*/
int sqlite3BtreeOpenAll(sqlite3_vfs *pVfs, const char *zFilename, sqlite3 *db, Btree **ppBtree, int flags, int vfsFlags){
  int rc;
  Btree *pBtree;

  rc = sqlite3BtreeOpenOriginal(pVfs, zFilename, db, &pBtree, flags, vfsFlags);
  if( rc ) return rc;

  rc = sqlite3TransBtreeOpen(pBtree, flags, vfsFlags, pVfs);
  if( rc ){
    sqlite3BtreeClose(pBtree);
    return rc;
  }

  *ppBtree = pBtree;
  return SQLITE_OK;
}

/*
** Close transaction databases and invalidate cursors.
*/
static int sqlite3TransBtreeClose(Btree *pBtree){
  BtreeTrans *pBtTrans = &pBtree->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;

  if( pBtreeTrans ){
    sqlite3rowlockSavepointClose(&pBtTrans->lockSavepoint);
    sqlite3rowlockIpcFinish(&pBtTrans->ipcHandle);
    sqlite3rowlockPsmFinish(&pBtTrans->psmHandle);
    sqlite3BtreeCloseOriginal(pBtreeTrans);
    transRootPagesFinish(&pBtTrans->rootPages);
  }
  memset(pBtTrans, 0, sizeof(BtreeTrans));

  return SQLITE_OK;
}

/*
** Close transation btree and shared btree.
*/
int sqlite3BtreeCloseAll(Btree *p){
  sqlite3TransBtreeClose(p);
  return sqlite3BtreeCloseOriginal(p);
}

/*
** Begin a transaction for transaction btree.
*/
int sqlite3TransBtreeBeginTrans(Btree *p, int wrflag){
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  int rc;

  /* Do nothing if this is a transaction btree. */
  if( !pBtreeTrans ){
    return SQLITE_OK;
  }

  rc = sqlite3BtreeBeginTransOriginal(pBtreeTrans, wrflag, 0);
  if( rc ) return rc;

  rc = sqlite3TransBtreeSavepointCreate(p, p->db->nStatement+p->db->nSavepoint);
  return rc;
}

/* Add new entry into a root page mapping. */
static TransRootPage *addTransRootPage(Btree *p, Pgno iTable, Pgno iInsTable, 
                                       Pgno iDelTable, struct KeyInfo *pKeyInfo){
  TransRootPage *pNew = NULL;
  TransRootPage *pOld = NULL;

  pOld = (TransRootPage*)sqlite3HashI64Find(&p->btTrans.rootPages, iTable);
  if( pOld ){
    /* If it already exists, it was created when delete all records. */
    pNew = pOld;
    assert( pNew->iIns==0 );
    assert( pNew->iDel==0 );
    assert( !pNew->pKeyInfo );
  }else{
    pNew = (TransRootPage*)sqlite3MallocZero(sizeof(TransRootPage));
    if( !pNew ) return NULL;

    /* Initialization */
    pNew->deleteAll = 0;

    pOld = (TransRootPage*)sqlite3HashI64Insert(&p->btTrans.rootPages, iTable, pNew,
                                                NULL, rowlockDefaultMalloc,
                                                rowlockDefaultFree);
    if( pOld == pNew ){
      sqlite3_free(pNew);
      return NULL;
    }
    assert( pOld==0 );
  }

  /* 
  ** Increment a reference counter of pKeyInfo. We use pKeyInfo at COMMIT.
  ** If we do not increment it, pKeyInfo will be freed when statement is closed 
  ** and we cannot use it at COMMIT.
  */
  sqlite3KeyInfoRef(pKeyInfo);

  pNew->iIns = iInsTable;
  pNew->iDel = iDelTable;
  pNew->pKeyInfo = pKeyInfo;

  return pNew;
}

/*
** Convert a btree pointer into the iDb index that indicates
** which database file in db->aDb[] the btree refers to.
** Refer sqlite3SchemaToIndex() to understand the logic.
*/
static int rowlockBtreeToIndex(sqlite3 *db, Btree *p){
  int i = -1000000;

  assert( sqlite3_mutex_held(db->mutex) );
  assert( p );
  for(i=0; 1; i++){
    assert( i<db->nDb );
    if( db->aDb[i].pBt==p ){
      break;
    }
  }
  assert( i>=0 && i<db->nDb );
  return i;
}

/* Judge a table type. */
static u8 rowlockJudgeTableType(Btree *p, Pgno rootPage){
  int iDb;
  Schema *pSchema;
  HashElem *i;
  Index *pIndex = NULL;

#ifdef SQLITE_TEST
  if( p->db->nDb==0 ) return TABLE_NORMAL;
#endif

  iDb = rowlockBtreeToIndex(p->db, p);

  assert( sqlite3SchemaMutexHeld(p->db, iDb, 0) );
  pSchema = p->db->aDb[iDb].pSchema;

  /* Search index. */
  for(i=sqliteHashFirst(&pSchema->idxHash); i; i=sqliteHashNext(i)){
    pIndex = (Index*)sqliteHashData(i);
    if( pIndex->tnum==rootPage ) break;
  }
  
  if( i ){
    if( IsPrimaryKeyIndex(pIndex) || IsUniqueIndex(pIndex) ){
      return INDEX_UNIQUE;
    }else{
      return INDEX_NORMAL;
    }
  }

  if( pSchema->pSeqTab && pSchema->pSeqTab->tnum==rootPage ){
    return TABLE_SEQUENCE;
  }else{
    return TABLE_NORMAL;
  }
}

/* Judge if it is table or index. */
static u8 rowlockIsIndex(Btree *p, Pgno rootPage){
  u8 type = rowlockJudgeTableType(p, rootPage);
  if( type==INDEX_UNIQUE || type==INDEX_NORMAL ){
    return 1;
  }else{
    return 0;
  }
}

/*
** TransRootPage **ppRootPage : The pointer of rootPage information in 
** transaction btree. This information is managed in the hash 
** (Btree.btTrans.rootPages). It should be freed when the hash is 
** cleared. 
*/
int sqlite3TransBtreeCreateTable(Btree *p, Pgno iTable, struct KeyInfo *pKeyInfo, 
                                 TransRootPage **ppRootPage){
  int rc = SQLITE_OK;
  int flags;
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  Pgno iInsTable;
  Pgno iDelTable = 0;
  int iMoved;
  TransRootPage *pNew = NULL;

  if( pKeyInfo ){
    /* Index creation */
    flags = BTREE_BLOBKEY;
  }else{
    /* Table creation */
    flags = BTREE_INTKEY;
  }

  if( pBtreeTrans->pBt->inTransaction!=TRANS_WRITE ){
    rc = sqlite3TransBtreeBeginTrans(p, 2);
    if( rc ) return rc;
  }

  rc = sqlite3BtreeCreateTableOriginal(pBtreeTrans, &iInsTable, flags);
  if( rc ) return rc;

  rc = sqlite3BtreeCreateTableOriginal(pBtreeTrans, &iDelTable, flags);
  if( rc ) goto trans_btree_create_table_failed;

  /* Create an index space in shared memory. */
  if( pKeyInfo && rowlockJudgeTableType(p, iTable)==INDEX_UNIQUE ){
    rc = sqlite3rowlockPsmCreateTable(&pBtTrans->psmHandle, iTable);
    if( rc ) goto trans_btree_create_table_failed;
    rc = sqlite3rowlockHistoryAddNewIndex(&pBtTrans->lockSavepoint, iTable);
    if( rc ) goto trans_btree_create_table_failed;
  }

  /* Associates iTable to iInsTable and iDelTable. */
  pNew = addTransRootPage(p, iTable, iInsTable, iDelTable, pKeyInfo);
  if( !pNew ){
    rc = SQLITE_NOMEM_BKPT;
    goto trans_btree_create_table_failed;
  }

  rc = sqlite3rowlockHistoryAddNewTable(&pBtTrans->lockSavepoint, iTable);
  if( rc ) goto trans_btree_create_table_failed;

  if( ppRootPage ){
    *ppRootPage = pNew;
  }

  return SQLITE_OK;

trans_btree_create_table_failed:
  sqlite3BtreeDropTable(pBtreeTrans, iInsTable, &iMoved);
  if( iDelTable>0 ) sqlite3BtreeDropTable(pBtreeTrans, iDelTable, &iMoved);
  return rc;
}

/* 
** Drop tables in  transaction btree. The talbe number of shared btree is 
** spevified by an argument. We delete 2 tables. One is a insertion table, 
** another is adeletion table.
*/
static int transBtreeDropTable(Btree *p, Pgno iTable){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  TransRootPage *pRootPage;
  int iMoved;
  TransRootPage *pData = NULL;

  if( !transBtreeIsUsed(p) ){
    return SQLITE_OK;
  }

  pRootPage = (TransRootPage*)sqlite3HashI64Find(&p->btTrans.rootPages, iTable);
  if( !pRootPage ){
    return SQLITE_OK;
  }

  if( pRootPage->iIns>0 ){
    assert( pRootPage->iDel>0 );
    rc = sqlite3BtreeDropTableOriginal(pBtreeTrans, pRootPage->iIns, &iMoved);
    if( rc ) return rc;
    rc = sqlite3BtreeDropTableOriginal(pBtreeTrans, pRootPage->iDel, &iMoved);
  }
  /* Delete the mapping and Free memory in DROP table command.*/
  pData = (TransRootPage*)sqlite3HashI64Insert(&p->btTrans.rootPages, iTable, NULL,
                                               NULL, rowlockDefaultMalloc,
                                               rowlockDefaultFree);
  if( pData->pKeyInfo ){
    sqlite3KeyInfoUnref(pData->pKeyInfo);
    /* Drop an index in shared memory. */
    if( rowlockJudgeTableType(p, iTable)==INDEX_UNIQUE )
      sqlite3rowlockPsmDropTable(&pBtTrans->psmHandle, iTable);
  }
  sqlite3_free(pData);

  return rc;
}

/*
** Drop tables in both shared btree and transaction btree.
*/
int sqlite3BtreeDropTableAll(Btree *p, Pgno iTable, int *piMoved){
  int rc = SQLITE_OK;
  if( transBtreeIsUsed(p) ){
    rc = transBtreeDropTable(p, iTable);
    if( rc ) return rc;
  }
  rc = sqlite3BtreeDropTableOriginal(p, iTable, piMoved);
  if( rc ) return rc;

  /* Delete cachedRowid if it is a table. */
  if( transBtreeIsUsed(p) && !rowlockIsIndex(p, iTable) ){
    sqlite3rowlockIpcCachedRowidDropTable(&p->btTrans.ipcHandle, iTable);
  }

  return SQLITE_OK;
}

/* 
** Count the record in a table. 
** In order to call sqlite3BtreeCount(), we create a cursor. 
*/
static int btreeCountEntry(Btree *p, Pgno iTable, i64 *pnEntry){
  i64 nEntry = 0;                      /* Value to return in *pnEntry */
  int rc;                              /* Return code */
  BtCursor *pCur = NULL;

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ) return SQLITE_NOMEM_BKPT;

  rc = sqlite3BtreeCursorOriginal(p, iTable, 0, NULL, pCur);
  if( rc ){
    sqlite3_free(pCur);
    return rc;
  }

  rc = sqlite3BtreeCount(p->db, pCur, &nEntry);
  sqlite3BtreeCloseCursorOriginal(pCur);
  sqlite3_free(pCur);
  if( rc ) return rc;

  *pnEntry = nEntry;
  return SQLITE_OK;
}

/*
** Check if on one have a record lock in a table.
** Return SQLITE_OK if no one have a record lock in table.
** Return SQLITE_LOCKED if someone have a lock for any one of records in table.
*/
static int rowlockLockRecordsQuery(Btree *p, Pgno iTable){
  int rc = SQLITE_OK;
  BtCursor *pCur;
  int res;

  if( rowlockIsIndex(p,iTable) ){
    /* Do nothing if it is an index. */
    return SQLITE_OK;
  }

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ) return SQLITE_NOMEM_BKPT;

  rc = sqlite3BtreeCursorOriginal(p, iTable, 0, NULL, pCur);
  if( rc ) goto rowlock_lock_records_query_end_1;

  rc = sqlite3BtreeFirst(pCur, &res);
  if( rc ) goto rowlock_lock_records_query_end_2;
  if( res==0 ){
    do{
      i64 rowid = sqlite3BtreeIntegerKeyOriginal(pCur);
      rc = sqlite3rowlockIpcLockRecordQuery(&p->btTrans.ipcHandle, iTable, rowid);
      if( rc ) goto rowlock_lock_records_query_end_2;

      rc = sqlite3BtreeNext(pCur, 0);
    } while( rc==SQLITE_OK );
    if( rc != SQLITE_DONE ) goto rowlock_lock_records_query_end_2;
    assert( rc==SQLITE_DONE );
    rc = SQLITE_OK;
  }
  /* Here, no one have a lock for all records in the table. */

rowlock_lock_records_query_end_2:
  sqlite3BtreeCloseCursorOriginal(pCur);
rowlock_lock_records_query_end_1:
  sqlite3_free(pCur);

  return rc;
}

/* Get a table lock. If it is successful, we memorize locked table id into history. */
int sqlite3rowlockIpcLockTableAndAddHistory(Btree *p, int iTable, u8 eLock){
  int rc = SQLITE_OK;
  u8 prevLock = eLock;

  if( !transBtreeIsUsed(p) ) return SQLITE_OK;

  rc = sqlite3rowlockIpcLockTable(&p->btTrans.ipcHandle, iTable, eLock, MODE_LOCK_NORMAL, &prevLock);
  if( rc ) return rc;

  if( eLock>prevLock ){
    rc = sqlite3rowlockHistoryAddTableLock(&p->btTrans.lockSavepoint, iTable, prevLock);
  }

  return rc;
}

/*
** Delete all records in a table.
** We just set a flag for the performance.
*/
int sqlite3TransBtreeClearTable(Btree *p, int iTable, int *pnChange){
  if( transBtreeIsUsed(p) ){
    int rc = SQLITE_OK;
    BtreeTrans *pBtTrans = &p->btTrans;
    HashI64 *pTransRootPages = &pBtTrans->rootPages;
    TransRootPage *pRootPage = NULL;
    int nChangeIns = 0;

    /* Check if someone has a table lock. */
    rc = sqlite3rowlockIpcLockTableAndAddHistory(p, iTable, WRITEEX_LOCK);
    if( rc ) return rc;

    /* Check if someone has a record lock in the table. */
    rc = rowlockLockRecordsQuery(p, iTable);
    if( rc ) return rc;

    pRootPage = (TransRootPage*)sqlite3HashI64Find(pTransRootPages, 
                                                   iTable);
    if( pRootPage ){
      /* Delete all records in transaction btree. */
      if( pRootPage->iIns>0 ){
        rc = sqlite3BtreeClearTable(pBtTrans->pBtree, pRootPage->iIns, 
                                    (pnChange ? &nChangeIns : 0));
        if( rc ) return rc;
      }
    }else{
      pRootPage = addTransRootPage(p, iTable, 0, 0, NULL);
      if( !pRootPage ) return SQLITE_NOMEM_BKPT;
    }

    /* Invalidate for all blob open on this table */
    invalidateIncrblobCursorsOriginal(p, (Pgno)iTable, 0, 1);

    /* Count deleted record number if necessary. */
    if( pnChange ){
      i64 nChange = 0;
      rc = btreeCountEntry(p, iTable, &nChange);
      if( rc ) return rc;
      *pnChange = nChange + nChangeIns; /* ToDo: sqlite3BtreeClearTable cannot return nChange by i64 type. */
    }

    rc = sqlite3rowlockHistoryAddTableClear(&p->btTrans.lockSavepoint, iTable, pRootPage->deleteAll);
    if( rc ) return rc;
    /* Set the flag */
    pRootPage->deleteAll = 1;
    return SQLITE_OK;
  }else{
    return sqlite3BtreeClearTable(p, iTable, pnChange);
  }
}

static int rowlockIsSequenceTable(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  return pCurTrans->type==TABLE_SEQUENCE;
}

static int rowlockIsUniqueIndex(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  return pCurTrans->type==INDEX_UNIQUE;
}

/* 
** Judge a table type of cursor.
** The result is stored into pCurTrans->type.
*/
static void rowlockJudgeCursorType(Btree *p, BtCursor *pCur,
                                   struct KeyInfo *pKeyInfo){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;

  pCurTrans->type = rowlockJudgeTableType(p, pCur->pgnoRoot);
}

/*
** Create cursors for insertion table and deletion talbe in trancation btree.
*/
static int transBtreeCursor(
  Btree *p,                                   /* The btree */
  Pgno iTable,                                 /* Root page of table to open */
  int wrFlag,                                 /* 1 to write. 0 read-only */
  struct KeyInfo *pKeyInfo,                   /* First arg to xCompare() */
  BtCursor *pCur                              /* Write new cursor here */
){
  int rc;
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurTransIns;
  BtCursor *pCurTransDel;
  TransRootPage *pRootPage;
  Pgno iInsTable;
  Pgno iDelTable;

  /* Do nothing if it is transaction btree. */
  if( iTable==1 || !transBtreeIsUsed(p) ){
    pCurTrans->pCurIns = NULL;
    pCurTrans->pCurDel = NULL;
    pCurTrans->state = CURSOR_NOT_USE;
    return SQLITE_OK;
  }

  /*
  ** Search root page from transaction btree.
  ** If not found, create new table.
  */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pBtTrans->rootPages, iTable);
  if( !pRootPage || pRootPage->iIns==0 ){
    TransRootPage *pRootPage = NULL;
    if( !sqlite3BtreeIsInTransOriginal(pBtreeTrans) && !wrFlag ){
      pCurTrans->pCurIns = NULL;
      pCurTrans->pCurDel = NULL;
      pCurTrans->state = CURSOR_NOT_USE;
      return SQLITE_OK;
    }
    rc = sqlite3TransBtreeCreateTable(p, iTable, pKeyInfo, &pRootPage);
    if( rc ) return rc;
    iInsTable = pRootPage->iIns;
    iDelTable = pRootPage->iDel;
  }else{
    iInsTable = pRootPage->iIns;
    iDelTable = pRootPage->iDel;
  }

  pCurTransIns = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCurTransIns ){
    return SQLITE_NOMEM_BKPT;
  }
  pCurTransDel = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCurTransDel ){
    sqlite3_free(pCurTransIns);
    return SQLITE_NOMEM_BKPT;
  }

  rc = sqlite3BtreeCursorOriginal(pBtreeTrans, iInsTable, wrFlag, pKeyInfo, 
                                pCurTransIns);
  if( rc ) goto trans_btree_cursor_failed;

  rc = sqlite3BtreeCursorOriginal(pBtreeTrans, iDelTable, wrFlag, pKeyInfo, 
                                pCurTransDel);
  if( rc ) goto trans_btree_cursor_failed;

  pCurTrans->pCurIns = pCurTransIns;
  pCurTrans->pCurDel = pCurTransDel;
  pCurTrans->state = CURSOR_USE_SHARED;
  rowlockJudgeCursorType(p, pCur, pKeyInfo);

  return SQLITE_OK;

trans_btree_cursor_failed:
  sqlite3BtreeCloseCursorOriginal(pCurTransIns);
  sqlite3BtreeCloseCursorOriginal(pCurTransDel);
  sqlite3_free(pCurTransIns);
  sqlite3_free(pCurTransDel);
  return rc;
}

/*
** Create cursors for shared btree and transaction btree.
** Cursor for shared btree is read only, cursor for transaction btree depends on the flag.
** Please see sqlite3BtreeBeginTrans(). Write transaction for shared btree is not started.
** But we create a write cursor for sqlite_master because it prevents from modification of
** sqlite_master from multiple users. So we have to start a write transaction before 
** creating a write cursor for sqlite_master.
**
** This function is called from during SQL execution and commit. The 6th argument 'flag' 
** indicates it. If 'flag' is 0, it is in SQL execution and this function behaves as above.
** If 'flag' is 1, it is in commit process and we can create a write cursor for shared
** btree. A write transaction was already started by sqlite3BtreeBeginTransForCommit().
*/
int sqlite3BtreeCursorAll(Btree *p, Pgno iTable, int wrFlag, 
                          struct KeyInfo *pKeyInfo, BtCursor *pCur, int flag){
  int rc;

  if (p->sharable && iTable==SCHEMA_ROOT && wrFlag>0 && !sqlite3BtreeIsInTransOriginal(p)) {
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if (rc) return rc;
  }

  sqlite3BtreeCursorZero(pCur);
  if (p->sharable && iTable!=SCHEMA_ROOT && flag==ROW_LOCK_CURSOR_IN_TRANS) {
    rc = sqlite3BtreeCursorOriginal(p, iTable, 0, pKeyInfo, pCur);
  }
  else {
    rc = sqlite3BtreeCursorOriginal(p, iTable, wrFlag, pKeyInfo, pCur);
  }
  if ( rc ) return rc;

  rc = transBtreeCursor(p, iTable, wrFlag, pKeyInfo, pCur);
  if ( rc ) {
    sqlite3BtreeCloseCursorOriginal(pCur);
  }

  return rc;
}

/* Close sursors for transaction btree. */
static int sqlite3TransBtreeCloseCursor(BtCursor *pCur){
  if( transBtreeCursorIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    sqlite3BtreeCloseCursorOriginal(pCurTrans->pCurIns);
    sqlite3BtreeCloseCursorOriginal(pCurTrans->pCurDel);
    sqlite3_free(pCurTrans->pCurIns);
    sqlite3_free(pCurTrans->pCurDel);
    pCurTrans->pCurIns = NULL;
    pCurTrans->pCurDel = NULL;
    pCurTrans->state = CURSOR_NOT_USE;
    pCurTrans->deleteAll = 0;
  }
  return SQLITE_OK;
}

/* Close cursors for both shared btree and transaction btree. */
int sqlite3BtreeCloseCursorAll(BtCursor *pCur){
  Btree *pBtree = pCur->pBtree;
  if( pBtree ) sqlite3TransBtreeCloseCursor(pCur);
  return sqlite3BtreeCloseCursorOriginal(pCur);
}

/* Clear an ephemeral table and reset cached rowid. */
int sqlite3BtreeClearTableOfCursorAll(BtCursor *pCur){
  int rc;
  assert( !transBtreeCursorIsUsed(pCur) );
  rc = sqlite3BtreeClearTableOfCursorOriginal(pCur);
  if( rc==SQLITE_OK ) sqlite3BtreeCachedRowidSet(pCur, 0); 
  return rc;
}

/* Compare a key and an index key. */
static int btreeKeyCompare(u32 nKey, void *pKey, UnpackedRecord *pIdxKey){
  RecordCompare xRecordCompare;
  int ret;

  assert( pIdxKey );
  xRecordCompare = sqlite3VdbeFindCompare(pIdxKey);
  assert( xRecordCompare );

  if( pIdxKey->nField>pIdxKey->pKeyInfo->nKeyField ){
    /* 
    ** For a WITHOUT ROWID table. nField is used as the number of key columns
    ** in xRecordCompare. See the comment of btreeCursorMovetoKey().
    */
    u16 nField = pIdxKey->nField;
    pIdxKey->nField = pIdxKey->pKeyInfo->nKeyField;
    ret = xRecordCompare(nKey, pKey, pIdxKey);
    pIdxKey->nField = nField; /* Restore. */
  }else{
    ret = xRecordCompare(nKey, pKey, pIdxKey);
  }

  return ret;
}

/* Compare key pointed by cursors. */
static int btreeKeyCompareCursors(BtCursor *pCur1, BtCursor *pCur2, i64 *pRet){
  if( !pCur1->pKeyInfo ){
    /* Table case */
    i64 nKey1 = sqlite3BtreeIntegerKeyOriginal(pCur1);
    i64 nKey2 = sqlite3BtreeIntegerKeyOriginal(pCur2);
    *pRet = nKey1 - nKey2;
    return SQLITE_OK;
  }else{
    /* Index case */
    int rc = SQLITE_OK;             /* Status code */
    UnpackedRecord *pIdxKey = NULL; /* Unpacked index key */
    void *pKey1, *pKey2;            /* Packed key */
    u32 nKey1, nKey2;               /* Size of pKey */

    assert( pCur2->pKeyInfo );
    /* 
    ** Calculate pKey and nKey which is required for calling 
    ** the comparison function.
    */
    nKey1 = sqlite3BtreePayloadSizeOriginal(pCur1);
    assert( nKey1==(i64)(int)nKey1 );
    nKey2 = sqlite3BtreePayloadSizeOriginal(pCur2);
    assert( nKey2==(i64)(int)nKey2 );

    pKey1 = (char*)sqlite3DbMallocZero(pCur1->pKeyInfo->db, nKey1);
    if( !pKey1 ){
      return SQLITE_NOMEM_BKPT;
    }
    pKey2 = (char*)sqlite3DbMallocZero(pCur2->pKeyInfo->db, nKey2);
    if( !pKey2 ){
      rc = SQLITE_NOMEM_BKPT;
      goto btree_idxkey_compare_done;
    }

    rc = sqlite3BtreePayloadOriginal(pCur1, 0, nKey1, pKey1);
    if( rc ) goto btree_idxkey_compare_done;
    rc = sqlite3BtreePayloadOriginal(pCur2, 0, nKey2, pKey2);
    if( rc ) goto btree_idxkey_compare_done;

    pIdxKey = sqlite3VdbeAllocUnpackedRecord(pCur2->pKeyInfo);
    if( pIdxKey==0 ) {
      rc = SQLITE_NOMEM_BKPT;
      goto btree_idxkey_compare_done;
    }
    sqlite3VdbeRecordUnpack(pCur2->pKeyInfo, (int)nKey2, pKey2, pIdxKey);
    if( pIdxKey->nField==0 ){
      rc = SQLITE_CORRUPT_BKPT;
      goto btree_idxkey_compare_done;
    }

    /* Compare index keys. */
    *pRet = btreeKeyCompare(nKey1, pKey1, pIdxKey);

btree_idxkey_compare_done:
    sqlite3DbFree(pCur1->pKeyInfo->db, pKey1);
    sqlite3DbFree(pCur2->pKeyInfo->db, pKey2);
    sqlite3DbFree(pCur2->pKeyInfo->db, pIdxKey);
    return rc;
  }
}

/* Return 1 if the cursor is pointing valid record. */
static int btreeCursorIsPointing(BtCursor *pCur){
  return pCur && pCur->eState==CURSOR_VALID;
}

/*
** Return 1 if number of pages in the database is 0.
*/
static int isBtreeEmpty(Btree *pBtree){
  return pBtree->pBt->nPage==0;
}

/* 
** Move a cursor on the basis of key info.
** There are 3 patterns about a record structure.
** Record data consists of record-key and record-value.
** 1. pKey is NULL. This case is a normal table.
** 2. pKey has record-key. This case is an index.
** 3. pKey has record-key and record-value. This case is a WITHOUT ROWID table.
** For the pattern-3, this function compares record by only record-key.
*/
static int btreeCursorMovetoKey(BtCursor *pCur, const void *pKey, i64 nKey, int *pRet){
  int rc = SQLITE_OK; /* Status code */
  UnpackedRecord *pIdxKey = NULL; /* Unpacked index key */

  if( pCur->pKeyInfo ){
    /* Create unpacked record. */
    pIdxKey = sqlite3VdbeAllocUnpackedRecord(pCur->pKeyInfo);
    if( pIdxKey==0 ) return SQLITE_NOMEM_BKPT;

    sqlite3VdbeRecordUnpack(pCur->pKeyInfo, (int)nKey, pKey, pIdxKey);
    if( pIdxKey->nField==0 ){
      sqlite3DbFree(pCur->pKeyInfo->db, pIdxKey);
      return SQLITE_CORRUPT_BKPT;
    }

    /* For the pattern-3, the comparison is done by only record-key. */
    pIdxKey->nField = pIdxKey->pKeyInfo->nKeyField;
  }

  /* Search for pCur. */
  rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, nKey, 0, pRet);
  if( pCur->pKeyInfo ) sqlite3DbFree(pCur->pKeyInfo->db, pIdxKey);

  return rc;
}

/* This is called instead of sqlite3BtreeInsert. */
int sqlite3TransBtreeInsert(
  BtCursor *pCur,                /* Insert data into the table of this cursor */
  const BtreePayload *pX,        /* Content of the row to be inserted */
  int flags,                     /* True if this is likely an append */
  int seekResult                 /* Result of prior MovetoUnpacked() call */
){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  BtCursor *pCurDel = pCurTrans->pCurDel;
  int btFlags = flags;
  int res = 1;
  int isUpdate = 0;
  int isTransUpdate = 0;

  if( !pCurIns ){
    return sqlite3BtreeInsert(pCur, pX, flags, seekResult);
  }

  /* Save other cursor that point to the same btree */
  if( pCur->curFlags & BTCF_Multiple ){
      rc = saveAllCursorsOriginal(pCur->pBtree->pBt, pCur->pgnoRoot, pCur);
      if( rc ) return rc;
  }

  /* Judge whether it is update case or append case.
  ** If OPFLAG_APPEND is specified or seekResult is not 0, it means the append case.
  ** Otherwise, we check it by confirming the existence of record in shared btree.
  ** If a record is found, it means the update case, else the append case.
  */
  if( !(flags&OPFLAG_APPEND) && seekResult==0 ){
    if( cursorSharedIsUsed(pCurTrans) ){
      /* If a record exists in shared btree, it should be updated. */
      rc = btreeCursorMovetoKey(pCur, pX->pKey, pX->nKey, &res);
      if( rc ) return rc;
      btFlags = BTREE_APPEND;
      if( res==0 ){
        isUpdate = 1;
      }
    }
    if( cursorTransIsUsed(pCur) ) {
    /* Detect Update on Transaction Btree */
      rc = btreeCursorMovetoKey(pCurIns, pX->pKey, pX->nKey, &res);
      if( rc ) return rc;
      btFlags = BTREE_APPEND;
      if( res==0 ) {
        isTransUpdate = 1;
      }
    }
  }

  /*
  ** Get a row/index lock. But we don't get a record lock for sqlite_sequence
  ** table and non unique index. sqlite_sequence is used for auto-increment feature.
  */
  if( pCur->pKeyInfo ){
    if( rowlockIsUniqueIndex(pCur) ){
      BtreeTrans *pBtTrans = &pCur->pBtree->btTrans;
      rc = sqlite3rowlockPsmLockRecord(&pBtTrans->psmHandle, pCur->pgnoRoot,
                                       (unsigned char*)pX->pKey, (u32)pX->nKey,
                                       pCur->pBtree, pCur->pKeyInfo->aColl[0]);
      if( rc==SQLITE_LOCKED ){
        /* Unique index constraint error. */
        return SQLITE_CONSTRAINT;
      }else if( rc==SQLITE_OK ){
        rc = sqlite3rowlockHistoryAddIndex(&pBtTrans->lockSavepoint,
                                           pCur->pgnoRoot, pX->nKey, pX->pKey,
                                           pCur->pKeyInfo->aColl[0]);
        if( rc ) return rc;
      }else if( rc==SQLITE_DONE ){
        /* Do nothing. */
      }else{
        /* Error. */
        return rc;
      }
    }
  }else if( !rowlockIsSequenceTable(pCur) ){
    /* Lock record */
    rc = sqlite3rowlockIpcLockRecord(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot, pX->nKey);
    if( rc==SQLITE_DONE ){
      /* Do nothing if it already has a lock. We can insert new record. */
    }else if( rc ){
      if( rc==SQLITE_LOCKED && cachedRowidFlagGet(pCur)==1 ){
        /* If it is already locked, a record having same rowid is already inserted
        ** by the other process. In this case, if rowid is issued automatically,
        ** we need to retry the query execution. Therefore we return an special
        ** error code.
        */
        return SQLITE_CORRUPT_ROWID;
      }
      return rc;
    }else{
      rc = sqlite3rowlockHistoryAddRecord(&pCur->pBtree->btTrans.lockSavepoint, pCur->pgnoRoot, pX->nKey);
      if( rc ) return rc;
    }
  }

  /* If update case, we add a record into a delete table. */
  if( isUpdate ){
    rc = sqlite3BtreeInsert(pCurDel, pX, 0, 0);
    if( rc ) return rc;
    rc = btreeMovetoOriginal(pCurDel, pX->pKey, pX->nKey, 0, &res);
    if (rc) return rc;
  }

  rc = sqlite3BtreeInsert(pCurIns, pX, btFlags, 0);
  if( rc ) return rc;

  rc = btreeMovetoOriginal(pCurIns, pX->pKey, pX->nKey, 0, &res);
  if( rc ) return rc;
  assert( sqlite3BtreeCursorIsValid(pCurIns) );

  /*Does not move the pCur when Update row on shared btree or row in 
  transaction btree*/
  if( !isUpdate && !isTransUpdate ) {
    rc = btreeMovetoOriginal(pCur, pX->pKey, pX->nKey, 0, &res);
    if( rc ) return rc;
  }

  /* Success inserting to trans (ins & del), invalidate incrblob cursor
  And pCur already move to corresponding position */
  invalidateIncrblobCursorsOriginal(pCur->pBtree, pCur->pgnoRoot, pX->nKey, 0);

  /* When update on transaction btree, return cursor state to USE_TRANS so that
  it does not move the pCur in OP_Next action*/
  if( isTransUpdate ){
    pCurTrans->state = CURSOR_USE_TRANS;
  } else {
    pCurTrans->state = CURSOR_USE_SHARED | CURSOR_USE_TRANS;
  }

  return SQLITE_OK;
}

/*
** This function stres a payloadt into ppKey with memory allocation.
** If the memory space is already allocated (*ppKey != NULL),
** it is reused. Otherwise new memory is allocated.
*/
static int btreePayloadWithMalloc(BtCursor *pCur, u32 *pnKey, void **ppKey){
  int rc = SQLITE_OK;        /* Status code */
  i64 nKey = 0;
  void *pKey = NULL;

  nKey = sqlite3BtreePayloadSizeOriginal(pCur);
  assert( nKey==(i64)(int)nKey );

  pKey = (char*)sqlite3Realloc(*ppKey, nKey);
  if( !pKey ) return SQLITE_NOMEM_BKPT;

  rc = sqlite3BtreePayloadOriginal(pCur, 0, (u32)nKey, pKey);
  if( rc ){
    sqlite3_free(pKey);
    *ppKey = NULL;
    return rc;
  }

  *pnKey = (u32)nKey;
  *ppKey = pKey;

  return SQLITE_OK;
}

/*
** This is called instead of sqlite3BtreeDelete.
** Remember Deleted record in a transaction.
*/
int sqlite3TransBtreeDelete(BtCursor *pCur, u8 flags){
  int rc = SQLITE_OK;        /* Status code */
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns =  pCurTrans->pCurIns;
  BtCursor *pCurDel =  pCurTrans->pCurDel;
  BtreeTrans *pBtTrans = &pCur->pBtree->btTrans;
  i64 nKey = 0;
  u32 u32nKey;
  void *pKey = NULL;
  UnpackedRecord* pIdxKey = NULL;
  BtreePayload x = {0};   /* Payload to be inserted */
  int resDel = 0;

  if( !pCurIns ){
    return sqlite3BtreeDelete(pCur, flags);
  }

    /* Save to cursor to detect modify conflict */
  if( pCur->curFlags & BTCF_Multiple ){
      rc = saveAllCursorsOriginal(pCur->pBtree->pBt, pCur->pgnoRoot, pCur);
      if( rc ) return rc;
  }

  if( cursorTransIsUsed(pCur) ){
    /*
    ** If the cursor is pointing a transaction tree, the row was
    ** inserted/updated in a transaction
    ** - In case insertion, we can delete row on transaction btree
    ** and unlock unique key/rowid
    ** - In case updation, we can delete row on transaction btree but
    ** can not unlock the unique key/rowid
    */
    if( pCur->pKeyInfo && rowlockIsUniqueIndex(pCur) ){
      rc = btreePayloadWithMalloc(pCurIns, &u32nKey, &pKey);
      if( rc ) return rc;
      sqlite3rowlockPsmUnlockRecord(&pBtTrans->psmHandle, pCur->pgnoRoot,
                                    (unsigned char*)pKey, u32nKey,
                                    pCur->pBtree, pCur->pKeyInfo->aColl[0]);
      sqlite3_free(pKey);
    }else{
      /* Unlock the unused rowid if it is not existed in shared btree */
      nKey = sqlite3BtreeIntegerKeyOriginal(pCurIns);
      rc = sqlite3BtreeMovetoUnpacked(pCurDel, pIdxKey, nKey, 0, &resDel);
      if( rc ) return rc;
      if( resDel!=0 )
        sqlite3rowlockIpcUnlockRecord(&pBtTrans->ipcHandle, pCur->pgnoRoot, nKey);
    }
    return sqlite3BtreeDelete(pCurIns, flags);
  }

  /*
  ** If the curor is pointing shared btree, we delete the row
  ** shared btree.
  */
  if( pCur->pKeyInfo ){
    /* For an index btree. */
    rc = btreePayloadWithMalloc(pCur, &u32nKey, &pKey);
    if( rc ) return rc;
    nKey = u32nKey;

    /* Get index lock if it is unique index. */
    if( rowlockIsUniqueIndex(pCur) ){
      /* Lock index. */
      rc = sqlite3rowlockPsmLockRecord(&pBtTrans->psmHandle, pCur->pgnoRoot,
                                       (unsigned char*)pKey, u32nKey,
                                       pCur->pBtree, pCur->pKeyInfo->aColl[0]);
      if( rc==SQLITE_LOCKED ){
        /* Unique index constraint error. */
        rc = SQLITE_CONSTRAINT;
        goto trans_btree_delete_end;
      }else if( rc==SQLITE_OK ){
        rc = sqlite3rowlockHistoryAddIndex(&pCur->pBtree->btTrans.lockSavepoint,
                                           pCur->pgnoRoot, nKey, pKey,
                                           pCur->pKeyInfo->aColl[0]);
        if( rc ) goto trans_btree_delete_end;
      }else if( rc==SQLITE_DONE ){
        /* Do nothing if I have already a lock. */
      }else{
        /* Error. */
        goto trans_btree_delete_end;
      }
    }
  }else{
    /* For a table btree. */
    nKey = sqlite3BtreeIntegerKeyOriginal(pCur);
    pKey = NULL;

    /* Lock record */
    rc = sqlite3rowlockIpcLockRecord(&pBtTrans->ipcHandle, pCur->pgnoRoot, nKey);
    if( rc==SQLITE_DONE ){
      /* Do nothing if I have already a lock. */
    }else if( rc ){
      return rc;
    }else{
      rc = sqlite3rowlockHistoryAddRecord(&pBtTrans->lockSavepoint, pCur->pgnoRoot, nKey);
      if( rc ) return rc;
    }
  }

  /* Operate INSERT */
  x.pKey = pKey;
  x.nKey = nKey;
  rc = sqlite3BtreeInsert(pCurDel, &x, OPFLAG_APPEND, 0);
  if( rc ) goto trans_btree_delete_end;

    /* Invalidate cursor after delete by inserting to delete table on transaction tree */
    if ( !pKey ) {
      invalidateIncrblobCursorsOriginal(pCur->pBtree, pCur->pgnoRoot, nKey, 0);
    }

trans_btree_delete_end:
  sqlite3_free(pKey);
  return rc;
}

/* Search and move pCur to valid row. */
static int btreeSeekToExist(BtCursor *pCur,
                     int (*xAdvance)(BtCursor*, int),
                     int flags){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurDel =  pCurTrans->pCurDel;
  i64 nOldKey = 0;

  /* Get current key before move */
  if( btreeCursorIsPointing(pCur) ){
    rc = btreeValidTableKey( pCur, &nOldKey, xAdvance==sqlite3BtreeNext );
    if( rc ) return rc;
  }

  if( pCurDel->eState>=CURSOR_REQUIRESEEK && !isBtreeEmpty(pCurDel->pBtree) ){
    int isDifferentRow;
    /* Restore the cursor position if saveCursorPosition() was called */
    rc = sqlite3BtreeCursorRestoreOriginal(pCurDel, &isDifferentRow);
    if( rc ) return rc;
  }

  while( btreeCursorIsPointing(pCur) &&
          btreeCursorIsPointing(pCurDel) ){
    i64 cmp = 0;
    rc = btreeKeyCompareCursors(pCur, pCurDel, &cmp);
    if( rc ) return rc;

    if( (xAdvance==sqlite3BtreeNext && cmp>0) ||
        (xAdvance==sqlite3BtreePrevious && cmp<0) ){
      /* Move pCurDel to the nearest row of pCur. */
      rc = xAdvance(pCurDel, flags);
    }else if( cmp==0 ){
      /* 101: Record in shared btree is deleted. */
      rc = xAdvance(pCur, flags);
      /* Detect if transver meet abnormal rowid/key not ACS or DES order after
      valid delete case */
      if( rc==SQLITE_OK ){
        rc = btreeValidTableKey( pCur, &nOldKey, xAdvance==sqlite3BtreeNext );
        if ( rc ) return rc;
      }
    }else{
      /* pCur is pointing a valid row. */
      return SQLITE_OK;
    }
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
  }
  return SQLITE_OK;
}

/*
** Move to existing row from current position
** Records are sorted by the key.
** If xAdvance is btreeNext, search the minimum record which is greater than current value.
** If xAdvance is btreePrevious, search the maximum record which is smaller than current value.
**
** (pCur,pCurIns,pCurDel)
** (111) - Record was updated
** (110) - Same key records
** (101) - Record was deleted
** (100) - Record is not updated and not deleted
** (011) - Nerver occur
** (010) - Record was inserted
** (001) - Nerver occur
** (000) - No record
*/
static int btreeSeekToExistAll(BtCursor *pCur, 
                     int (*xAdvance)(BtCursor*, int),
                     int flags, int *pRes){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns =  pCurTrans->pCurIns;
  BtCursor *pCurDel =  pCurTrans->pCurDel;

  /* Move pCur to valid row. */
  rc = btreeSeekToExist(pCur, xAdvance, flags);
  if( rc ) return rc;

  if( btreeCursorIsPointing(pCur) ){
    if( btreeCursorIsPointing(pCurIns) ){
      /* Both pCur and pCurIns are valid. */
      i64 cmp = 0;
      rc = btreeKeyCompareCursors(pCur, pCurIns, &cmp);
      if( rc ) return rc;

      if( cmp == 0 ||
          (cmp<0 && xAdvance==sqlite3BtreeNext) ||
          (cmp>0 && xAdvance==sqlite3BtreePrevious) ){
        /* 100: Record in shared btree is valid. */
        /* 110: Same key records. We use a record in shared btree firstly. */
        pCurTrans->state = CURSOR_USE_SHARED;
      }else{
        /* 010: Record exists in trans btree. */
        pCurTrans->state = CURSOR_USE_TRANS;
      }
    }else{
      /* 100: Record in shared btree is valid. */
      pCurTrans->state = CURSOR_USE_SHARED;
    }
    *pRes = 0;
  }else{
    if( btreeCursorIsPointing(pCurIns) ){
      /* 010: Only pCurIns is valid. */
      pCurTrans->state = CURSOR_USE_TRANS;
      *pRes = 0;
    }else{
      /* Both pCur and pCurIns are invalid. */
      *pRes = 1;
    }
  }
  return SQLITE_OK;
}

/* Move cursors to the first position. */
int sqlite3BtreeFirstAll(BtCursor *pCur, int *pRes){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  TransRootPage *pRootPage = NULL;
  int res = 1;
  int resIns = 1;
  int resDel = 1;

  rc = sqlite3BtreeFirst(pCur, &res);
  if( rc ) return rc;

  if( !transBtreeCursorIsUsed(pCur) ){
    *pRes = res;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurTrans->pCurIns, &resIns);
  if( rc ) return rc;

  rc = sqlite3BtreeFirst(pCurTrans->pCurDel, &resDel);
  if( rc ) return rc;
  
  /* Check if all records in shared btree are deleted. */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  assert( pRootPage );
  if( pRootPage->deleteAll ){
    pCur->eState = CURSOR_INVALID;
  }

  rc = btreeSeekToExistAll(pCur, sqlite3BtreeNext, 0, pRes);
  return rc;
}

/* Move cursors to the last position. */
int sqlite3BtreeLastAll(BtCursor *pCur, int *pRes){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  TransRootPage *pRootPage = NULL;
  int res = 0xFF;
  int resIns = 1;
  int resDel = 1;

  rc = sqlite3BtreeLast(pCur, &res);
  if( rc ) return rc;

  if( !transBtreeCursorIsUsed(pCur) ){
    /* If cursor is already on the last entry of the btree then sqlite3BtreeLast
    ** is NO-OP, value of res will not change, set the value of res to 0
    */
    if (res == 0xFF) {
      res = 0;
    }
    *pRes = res;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeLast(pCurTrans->pCurIns, &resIns);
  if( rc ) return rc;

  rc = sqlite3BtreeLast(pCurTrans->pCurDel, &resDel);
  if( rc ) return rc;

  /* Check if all records in shared btree are deleted. */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  assert( pRootPage );
  if( pRootPage->deleteAll ){
    pCur->eState = CURSOR_INVALID;
  }

  return btreeSeekToExistAll(pCur, sqlite3BtreePrevious, 0, pRes);
}

/* Move cursors to the next position. */
int sqlite3BtreeAdvanceAll(BtCursor *pCur, int flags, int(*xAdvance)(BtCursor*, int)){
  int rc;
  int res;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  i64 nOldKey = 0;

  assert( xAdvance==sqlite3BtreePrevious || xAdvance==sqlite3BtreeNext );

  /* Get current key before move */
  if( btreeCursorIsPointing(pCur) ){
    rc = btreeValidTableKey( pCur, &nOldKey, xAdvance==sqlite3BtreeNext );
    if( rc ) return rc;
  }

  if( !transBtreeCursorIsUsed(pCur) ){
    return xAdvance(pCur, flags);
  }

  /*
  ** If both pCurIns and pCur are pointing to a valid record,
  ** maybe the position of these cursors is not correct due to
  ** the previous operation.
  ** We need to check and correct the position:
  ** In case of OP_Next:
  ** - If pCur is used, pCurIns should not point to an entry that is
  ** smaller than pCur's entry.
  ** - If pCurIns is used, pCur should not point to an entry that is
  ** smaller than pCurIns's entry.
  ** In case of OP_Prev:
  ** - If pCur is used, pCurIns should not point to an entry that is
  ** greater than pCur's entry.
  ** - If pCurIns is used, pCur should not point to an entry that is
  ** greater than pCurIns's entry.
  */
  while( btreeCursorIsPointing(pCurIns) && btreeCursorIsPointing(pCur) ){
    i64 cmp;
    /* Check the position of pCur and pCurIns */
    rc = btreeKeyCompareCursors(pCur, pCurIns, &cmp);
    if( rc ) return rc;
    if( cursorSharedIsUsed(pCurTrans) ) {
      if( (xAdvance==sqlite3BtreeNext && cmp>0)
        || (xAdvance==sqlite3BtreePrevious && cmp<0) ){
        /* pCurIns is pointing to the wrong position, move it to the next position */
        rc = xAdvance(pCurIns, flags);
        if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      }else{
        break;
      }
    }
    if( cursorTransIsUsed(pCur) ){
      if( (xAdvance==sqlite3BtreeNext && cmp<0)
        || (xAdvance==sqlite3BtreePrevious && cmp>0) ) {
        /* pCur is pointing to the wrong position, move it to the next position */
        rc = xAdvance(pCur, flags);
        if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      }else{
        break;
      }
    }
  }

  if( cursorSharedIsUsed(pCurTrans) ){
    rc = xAdvance(pCur, flags);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;

    /* Check shared cursor after move */
    if ( btreeCursorIsPointing(pCur) && nOldKey ){
      rc = btreeValidTableKey( pCur, &nOldKey, xAdvance==sqlite3BtreeNext );
      if ( rc ) return rc;
    }
  }

  if( cursorTransIsUsed(pCur) ){
    if( pCurIns->eState>=CURSOR_REQUIRESEEK && isBtreeEmpty(pCurIns->pBtree) ){
      int isDifferentRow;
      /* If rollback or commit transaction, insert table is dropped */
      if( pCur->eState>=CURSOR_REQUIRESEEK ){
        /* Restore the cursor position of pCur if saveCursorPosition() was called */
        rc = sqlite3BtreeCursorRestoreOriginal(pCur, &isDifferentRow);
        if( rc ) return rc;
      }else if( pCur->eState==CURSOR_INVALID && pCurIns->eState==CURSOR_REQUIRESEEK ){
        /* Try to restore the cursor position of pCur using pKey and nKey of pCurIns.
        ** If record is found in pCur then move pCur to the next position.
        ** If record is not found, pCur should be CURSOR_INVALID */
        pCur->pKey = pCurIns->pKey;
        pCur->nKey = pCurIns->nKey;
        pCur->eState = CURSOR_REQUIRESEEK;
        rc = sqlite3BtreeCursorRestoreOriginal(pCur, &isDifferentRow);
        if( rc ) return rc;
        rc = xAdvance(pCur, flags);
        if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      }
      /* Invalidate pCurIns */
      pCurIns->eState = CURSOR_INVALID;
    }else{
      rc = xAdvance(pCurIns, flags);
      if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
    }
  }

  /* validate the cursor. */
  rc = btreeSeekToExistAll(pCur, xAdvance, flags, &res);
  if( rc ) return rc;
  if( res ){
    return SQLITE_DONE;
  }else{
    return SQLITE_OK;
  }
}

#ifndef SQLITE_OMIT_WINDOWFUNC
void sqlite3BtreeSkipNextAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;

  sqlite3BtreeSkipNextOriginal(pCur);
  if( transBtreeCursorIsUsed(pCur) ){
    sqlite3BtreeSkipNextOriginal(pCurIns);
    return;
  }
}
#endif /* SQLITE_OMIT_WINDOWFUNC */

/* Compare key specified by an argument with key pointed by cursor. */
static int btreeKeyCompareOfCursor(UnpackedRecord *pIdxKey, i64 intKey, BtCursor *pCur, i64 *pRes){
  if( !pIdxKey ){
    i64 nKey = sqlite3BtreeIntegerKeyOriginal(pCur);
    *pRes = intKey - nKey;
  }else{
    int rc;
    void *pKey = NULL; /* Packed key */
    u32 nKey;          /* Size of pKey */

    rc = btreePayloadWithMalloc(pCur, &nKey, &pKey);
    if( rc ) return rc;

    *pRes = btreeKeyCompare(nKey, pKey, pIdxKey);
    sqlite3_free(pKey);
  }
  return SQLITE_OK;
}

/* 
** This function gathers cursors to one side from key. 
** In case of OP_SeekGE or OP_SeekGT,
**   - both cursors are moved to the right side from key.
**   - *pRes is set to 1 which indicates cursor is pointing at an entry that is larger than key
** In case of OP_SeekLE or OP_SeekLT,
**   - both cursors are moved to the left side from key.
**   - *pRes is set to -1 which indicates cursor is pointing at an entry that is smaller than key
** See the explanation of sqlite3BtreeMovetoUnpackedAll().
**
** res indicates where the pCur is pointing at. It is aqcuired by sqlite3MovetoUnpacked for pCur.
** resIns is that of pCurIns.
*/
static int sqlite3BtreeMovetoSameSide(BtCursor *pCur, int opcode, int res, int resIns, int *pRes){
  int rc;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  BtCursor *pCurTarget;
  i64 resCmp;
  int direction; /* 1 if opcode is OP_SeekGE or OP_SeekGT, else -1. It is also used for *pRes. */
  int(*xAdvance)(BtCursor*, int);

  assert( opcode==OP_SeekGE || opcode==OP_SeekGT || opcode==OP_SeekLE || opcode==OP_SeekLT );
  assert( btreeCursorIsPointing(pCur) );
  assert( btreeCursorIsPointing(pCurIns) );

  if( opcode==OP_SeekGE || opcode==OP_SeekGT ){
    direction = 1;
    xAdvance = sqlite3BtreeNext;
  }else{
    direction = -1;
    xAdvance = sqlite3BtreePrevious;
  }

  /* 
  ** If cursors are pointing at the different side, we move cursor.
  ** In case of OP_SeekGE or OP_SeekGT, the cursor pointing at an 
  ** entry that is smaller than key is moved until pointing at larger 
  ** entry.
  ** In case of OP_SeekLE or OP_SeekLT, the cursor pointing at an 
  ** entry that is larger than key is moved until pointing at smaller 
  ** entry.
  */
  if( res*resIns<0 ){
    /* 
    ** Decide which sursor should be moved.
    ** direction*res means (direction==1 && res<0) || (direction==-1 && res>0)
    */
    if( (direction*res<0) ){
      pCurTarget = pCur;
    }else{
      pCurTarget = pCurIns;
    }

    /* Move cursor as it points at the same side. */
    rc = xAdvance(pCurTarget, 0);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;

    if( pCurTarget == pCur ){
      rc = btreeSeekToExist(pCurTarget, xAdvance, 0); 
      if( rc ) return rc;
    }

    /* Check if moved cursor is invalid. If it is invalid, we use another cursor. */
    if( !btreeCursorIsPointing(pCurTarget) ){
      if( pCurTarget==pCur ){
        pCurTrans->state = CURSOR_USE_TRANS;
      }else{
        pCurTrans->state = CURSOR_USE_SHARED;
      }
      *pRes = direction;
      return SQLITE_OK;
    }
  }

  /*
  ** Decide which cursor should be used.
  ** The pCurTrans->state and pRes value is based on
  ** - direction: 1 (sqlite3BtreeNext) or -1 (sqlite3BtreePrevious)
  ** - resCmp: 1 (pCur > pCurIns) or -1 (pCur < pCurIns)
  ** - res: 1 (pCur > pCurTarget) or -1 (pCur < pCurTarget)
  ** - resIns: 1 (pCurIns > pCurTarget) or -1 (pCurIns < pCurTarget)
  **
  ** |direction|resCmp|res|resIns|pCurTrans->state |pRes|   Remark      |
  ** |---------|------|---|------|-----------------|----|---------------|
  ** |    1    |   1  | 1 |   1  |CURSOR_USE_TRANS |  1 |               |
  ** |    1    |   1  | 1 |  -1  |CURSOR_USE_SHARED|  1 |               |
  ** |    1    |   1  |-1 |   1  |        x        |  x |Never happenned|
  ** |    1    |   1  |-1 |  -1  |CURSOR_USE_SHARED| -1 |               |
  ** |    1    |  -1  | 1 |   1  |CURSOR_USE_SHARED|  1 |               |
  ** |    1    |  -1  | 1 |  -1  |        x        |  x |Never happenned|
  ** |    1    |  -1  |-1 |   1  |CURSOR_USE_TRANS |  1 |               |
  ** |    1    |  -1  |-1 |  -1  |CURSOR_USE_TRANS | -1 |               |
  ** |   -1    |   1  | 1 |   1  |CURSOR_USE_TRANS |  1 |               |
  ** |   -1    |   1  | 1 |  -1  |CURSOR_USE_TRANS | -1 |               |
  ** |   -1    |   1  |-1 |   1  |        x        |  x |Never happenned|
  ** |   -1    |   1  |-1 |  -1  |CURSOR_USE_TRANS | -1 |               |
  ** |   -1    |  -1  | 1 |   1  |CURSOR_USE_SHARED|  1 |               |
  ** |   -1    |  -1  | 1 |  -1  |        x        |  x |Never happenned|
  ** |   -1    |  -1  |-1 |   1  |CURSOR_USE_SHARED| -1 |               |
  ** |   -1    |  -1  |-1 |  -1  |CURSOR_USE_SHARED| -1 |               |
  */
  rc = btreeKeyCompareCursors(pCur, pCurIns, &resCmp);
  if( rc ) return rc;
  if( res*resIns<0 ){
    if( direction*resCmp<0 ) pCurTrans->state = CURSOR_USE_TRANS;
    else pCurTrans->state = CURSOR_USE_SHARED;
    *pRes = direction;
  }else{
    if( res<0 ){
      if( direction*resCmp<0 ) pCurTrans->state = CURSOR_USE_TRANS;
      else pCurTrans->state = CURSOR_USE_SHARED;
    }else{
      if( direction*resCmp<0 ) pCurTrans->state = CURSOR_USE_SHARED;
      else pCurTrans->state = CURSOR_USE_TRANS;
    }
    *pRes = res;
  }

  return SQLITE_OK;
}

/* 
** Chenck if an entry which pCur points at is deleted.
** *pRet indicetes the check result. It is set by sqlite3MovetoUnpacked().
** If *pRet is set to 0, it means an entry is deleted. 
*/
static int btreeIsDeleted(BtCursor *pCur, int *pRet){
  int rc = SQLITE_OK;        /* Status code */
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurDel = pCurTrans->pCurDel;
  void *pKey = NULL;       /* Packed key */
  i64 nKey;
  UnpackedRecord *pIdxKey = NULL;   /* Unpacked index key */

  if( !btreeCursorIsPointing(pCur) ){
    *pRet = -1;
    return SQLITE_OK;
  }

  if( !pCur->pKeyInfo ){
    nKey = sqlite3BtreeIntegerKeyOriginal(pCur);
  }else{
    u32 n;
    rc = btreePayloadWithMalloc(pCur, &n, &pKey);
    if( rc ) return rc;
    nKey = n;
  }

  /* Search for pCurDel. */
  if( pCurDel->eState>=CURSOR_REQUIRESEEK && !isBtreeEmpty(pCurDel->pBtree) ){
    int isDifferentRow;
    /* Restore the cursor position if saveCursorPosition() was called */
    rc = sqlite3BtreeCursorRestoreOriginal(pCurDel, &isDifferentRow);
    if( rc ) return rc;
  }
  rc = btreeCursorMovetoKey(pCurDel, pKey, nKey, pRet);
  if( pKey ) sqlite3_free(pKey);

  return rc;
}

/*
** Count the number of entries having index key. The start point for the
** search is a current position of cursor, and xAdvance is a direction.
** For example, btree has 8 entries of which keys are 1, 2, 2, 3, 4, 4, 4, 5.
** If the cursor points to 2(left-most) and search key is 2, pCount is set to 2.
** If the cursor points to 3 and serch key is 3, pCount is set to 1.
** If the cursor points to 4(right-most) and search key is 4, pCount is set to 3.
*/
static int btreeCursorCountSameKey(BtCursor *pCur, UnpackedRecord *pIdxKey,
                                   int (*xAdvance)(BtCursor*, int), u64 *pCount){
  int rc;
  i8 default_rc_bak = pIdxKey->default_rc;
  i64 cmp;
  u64 count = 0;

  pIdxKey->default_rc = 0;

  do {
    rc = btreeKeyCompareOfCursor(pIdxKey, 0, pCur, &cmp);
    if( rc ) return rc;
    if( cmp!=0 ) break;
    count++;
    rc = xAdvance(pCur, 0);
    if( rc==SQLITE_DONE ) break;
  } while( rc==SQLITE_OK );

  pIdxKey->default_rc = default_rc_bak;

  *pCount = count;

  return SQLITE_OK;
}

/*
** Search both cursors(pCur and pCurIns) by the key.
** If it is found in either of cursors, pRes is 0 or pIndex->default_rc.
** If it is not found, pRes is not 0 (-1 or 1) and we judge which cursor should 
** be used.
** When sqlite3BtreeMovetoUnpackedAll() is called from OP_SeekXX (OP_SeekLT, 
** OP_SeekLE, OP_SeekGE, OP_SeekGT), we have to take case of the search
** direction. After this function, the cursor is moved based on pRes value.
** If cursors are pointing the different side from the key (Ex: pCur is pointing
** at an entry that is larger than key and pCur is pointing at an entry that is
** smaller than key), it is difficult to move cursors based on pRes. Therefore 
** both cursors has to be pointing the same side. This is implemented in
** sqlite3BtreeMovetoSameSide().
*/
int sqlite3BtreeMovetoUnpackedAll(BtCursor *pCur, UnpackedRecord *pIdxKey, 
                                  i64 intKey, int biasRight, int *pRes, 
                                  int opcode){
  int rc;
  int res, resIns, resDel;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  BtCursor *pCurDel = pCurTrans->pCurDel;
  TransRootPage *pRootPage = NULL;
  u8 eqSeen = 0, eqSeenIns = 0;

  /* Move pCur. */
  if( pIdxKey ) pIdxKey->eqSeen = 0;
  rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, intKey, biasRight, &res);
  if( rc ) return rc;

  if( !transBtreeCursorIsUsed(pCur) ){
    *pRes = res;
    return SQLITE_OK;
  }

  /*
  ** If an index entry is found in shared btree (pIdxKey->eqSeen!=0),
  ** it checks whether the entry is deleted or not.
  ** To confirm it, it compare the count of entries having target key
  ** of sharedbtree and deletion btree. If the count is same, all entries
  ** are deleted, so eqSeen should not set 1. Otherwise, there are at
  ** least one valid entry having search key in shared btree, so eqSeen
  ** can be set to 1.
  */
  if( pIdxKey && pIdxKey->eqSeen && pIdxKey->default_rc!=0 ){
    u64 count = 0, countDel = 0;
    i64 resCmp;
    int (*xAdvance)(BtCursor*, int);
    i8 default_rc_bak = pIdxKey->default_rc;

    if( opcode==OP_SeekGE || opcode==OP_SeekGT ){
      xAdvance = sqlite3BtreeNext;
    }else {
      assert( opcode==OP_SeekLE || opcode==OP_SeekLT);
      xAdvance = sqlite3BtreePrevious;
    }

    /*
    ** If pCur is not pointing an index key, move pCur to point to
    ** the index key.
    */
    pIdxKey->default_rc = 0;
    rc = btreeKeyCompareOfCursor(pIdxKey, 0, pCur, &resCmp);
    pIdxKey->default_rc = default_rc_bak;
    if( rc ) return rc;
    if( resCmp!=0 ){
      if( res!=pIdxKey->default_rc ){
        rc = xAdvance(pCur, 0);
        if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      }
    }

    /* Count the number of entries having same key in shared btree. */
    if( rc==SQLITE_OK ){
      rc = btreeCursorCountSameKey(pCur, pIdxKey, xAdvance, &count);
      if( rc ) return rc;
    }

    /* Count the number of entries having same key in deletion btree. */
    pIdxKey->eqSeen = 0;
    rc = sqlite3BtreeMovetoUnpacked(pCurDel, pIdxKey, intKey, biasRight, &resDel);
    if( rc ) return rc;
    if( pIdxKey->eqSeen ){
      if( resDel!=pIdxKey->default_rc ){
        rc = xAdvance(pCurDel, 0);
        if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      }
      if( rc==SQLITE_OK ){
        rc = btreeCursorCountSameKey(pCurDel, pIdxKey, xAdvance, &countDel);
        if( rc ) return rc;
      }
    }
    /*
    ** eqSeen can be set to 1 only when number of entry having search key
    ** in shared btree is greater than deletion btree.
    ** If an entry is not found in shared btree and found in deletion btree,
    ** eqSeen can not be set to 1.
    */
    if( count>countDel ) eqSeen = 1;

    rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, intKey, biasRight, &res);
    if( rc ) return rc;
  }else if( pIdxKey ) {
    eqSeen = pIdxKey->eqSeen;
  }else{
    eqSeen = 0;
  }

  /* Check if the record pointed by pCur is valid. */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  if( !pRootPage ){
    *pRes = res;
    return SQLITE_OK;
  }

  if( pRootPage->deleteAll ){
    pCur->eState = CURSOR_INVALID;
    res = -1;
    eqSeen = 0;
  }else{
    /* Check if an entry which pCur points at is deleted. */
    rc = btreeIsDeleted(pCur, &resDel);
    if( rc ) return rc;

    if( resDel==0 && opcode ){
      rc = sqlite3BtreeNext(pCur, 0);
      if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      /* Move cursor to valid location */
      rc = btreeSeekToExist(pCur, sqlite3BtreeNext, 0); 
      if( rc ) return rc;
      if( btreeCursorIsPointing(pCur) ){
        res = 1;
      }else{
        /* 
        ** Not found a valid record in the next to current position.
        ** So search an entry that is smaller than intKey/pIdxKey.
        */
        rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, intKey, biasRight, &res);
        if( rc ) return rc;
        if( resDel==0 ){
          rc = sqlite3BtreePrevious(pCur, 0);
          if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
        }
        rc = btreeSeekToExist(pCur, sqlite3BtreePrevious, 0); 
        if( rc ) return rc;
        res = -1;
      }
      if( !(pIdxKey && pIdxKey->eqSeen && pIdxKey->default_rc!=0) ) eqSeen = 0;
    }
  }

  /* Reset eqSeen then search for transaction btree. */
  if( pIdxKey ) pIdxKey->eqSeen = 0;
  rc = sqlite3BtreeMovetoUnpacked(pCurIns, pIdxKey, intKey, biasRight, &resIns);
  if( rc ) return rc;
  if( pIdxKey ) eqSeenIns = pIdxKey->eqSeen;

  /* Judge which cursor should be used. */
  if( eqSeen||eqSeenIns||resIns==0||res==0 ){
    if( eqSeenIns||resIns==0 ){
      /* Found in transaction btree. */
      pCurTrans->state = CURSOR_USE_TRANS;
      /* Move another cursor(pCur) to the expected side. */
      if( btreeCursorIsPointing(pCur) )
        if( res<0 && (opcode==OP_SeekGE || opcode==OP_SeekGT) ){
          rc = sqlite3BtreeNext(pCur, 0);
        }else if( res>0 && (opcode==OP_SeekLE || opcode==OP_SeekLT) ){
          rc = sqlite3BtreePrevious(pCur, 0);
        }
      *pRes = resIns;
    }else{
      /* Found in shared btree. */
      pCurTrans->state = CURSOR_USE_SHARED;
      /* Move another cursor(pCurIns) to the expected side. */
      if( btreeCursorIsPointing(pCurIns) )
        if( resIns<0 && (opcode==OP_SeekGE || opcode==OP_SeekGT) ){
          rc = sqlite3BtreeNext(pCurIns, 0);
        }else if( resIns>0 && (opcode==OP_SeekLE || opcode==OP_SeekLT) ){
          rc = sqlite3BtreePrevious(pCurIns, 0);
        }
      *pRes = res;
    }
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
  }else{
    /* 
     * Not found in both shared btree and transaction btree.
     * Decide which cursor should be used.
     */
    if( btreeCursorIsPointing(pCur) ){
      if( btreeCursorIsPointing(pCurIns) ){
        /* Both cursors are valid. Check which cursor is closer to the key. */
        if( opcode==OP_SeekGE || opcode==OP_SeekGT || opcode==OP_SeekLE || opcode==OP_SeekLT ){
          /* Move pCurDel to correct position (only want to call btreeCursorMovetoKey). */
          rc = btreeIsDeleted(pCur, &resDel);
          if( rc ) return rc;
          rc = sqlite3BtreeMovetoSameSide(pCur, opcode, res, resIns, pRes);
          if( rc ) return rc;
        }else{
          *pRes = res;
          pCurTrans->state = CURSOR_USE_SHARED;
        }
      }else{
        /* Only cursor of share btree is valid. */
        *pRes = res;
        pCurTrans->state = CURSOR_USE_SHARED;
     }
    }else{
      /* Cursor of share btree is invalid. So use transaction cursor. */
      *pRes = resIns;
      pCurTrans->state = CURSOR_USE_TRANS;
    }
  }

  /* Restore pCurDel position in SeekGE to check row deletion at next OP_Next*/
  if( pCur->btCurTrans.pCurDel->eState==CURSOR_INVALID && opcode==OP_SeekGE) {
    sqlite3BtreeMovetoUnpacked(pCurDel, pIdxKey, intKey, biasRight, &resDel);
  }

  if( pIdxKey ) pIdxKey->eqSeen = eqSeen + eqSeenIns;

  return SQLITE_OK;
}


/* Create serialized record. Refer OP_MakeRecord. */
static int vdbeMakeRecord(UnpackedRecord *pIdxKey, int file_format, u8 **ppRec,
                          int *pSize){
  u8 *zNewRecord = NULL; /* A buffer to hold the data for the new record */
  Mem *pRec;             /* Serialized record. */
  u32 nData = 0;         /* Number of bytes of data space */
  u32 nHdr = 0;          /* Number of bytes of header space */
  u32 nByte;             /* Data space required for this record */
  int nVarint;           /* Number of bytes in a varint */
  u32 serial_type;       /* Type field */
  Mem *pData0;           /* First field to be combined into the record */
  Mem *pLast;            /* Last field of the record */
  int nField;            /* Number of fields in the record */
  u32 i;                 /* Space used in zNewRecord[] header */
  u32 j;                 /* Space used in zNewRecord[] content */
  u32 len;               /* Length of a field */

  nField = pIdxKey->nField;
  assert( nField>0 );
  pData0 = &pIdxKey->aMem[0];
  pLast = &pData0[nField-1];

  /* Calculate record size. */
  pRec = pLast;
  do{
    assert( memIsValid(pRec) );
    serial_type = sqlite3VdbeSerialType(pRec, file_format, &len);
    if( pRec->flags & MEM_Zero ){
      if( serial_type==0 ){
        serial_type = 10;
      }else if( nData ){
        /* No nothing. */
      }else{
        len -= pRec->u.nZero;
      }
    }
    nData += len;
    nHdr += serial_type<=127 ? 1 : sqlite3VarintLen(serial_type);
    pRec->uTemp = serial_type;
    if( pRec==pData0 ) break;
    pRec--;
  }while(1);

  if( nHdr<=126 ){
    nHdr += 1;
  }else{
    nVarint = sqlite3VarintLen(nHdr);
    nHdr += nVarint;
    if( nVarint<sqlite3VarintLen(nHdr) ) nHdr++;
  }
  nByte = nHdr+nData;

  /* Allocate memory for new record. */
  zNewRecord = (u8*)sqlite3MallocZero(nByte);
  if( !zNewRecord ) return SQLITE_NOMEM_BKPT;

  /* Create serialized record. */
  i = putVarint32(zNewRecord, nHdr);
  j = nHdr;
  assert( pData0<=pLast );
  pRec = pData0;
  do{
    serial_type = pRec->uTemp;
    i += putVarint32(&zNewRecord[i], serial_type);            /* serial type */
    j += sqlite3VdbeSerialPut(&zNewRecord[j], pRec, serial_type); /* content */
  }while( (++pRec)<=pLast );
  assert( i==nHdr );
  assert( j==nByte );

  *ppRec = zNewRecord;
  *pSize = nByte;

  return SQLITE_OK;
}



static int btreeLockIndexKeyQuery(Btree *p, int iTable, UnpackedRecord *pIdxKey,
                                  int file_format){
  int rc;
  BtShared *pBt = p->pBt;
  BtreeTrans *pBtTrans = &p->btTrans;
  PsmLockHandle *pPsmHandle = &pBtTrans->psmHandle;
  u8 *pKey;
  int nKey;

  assert( pIdxKey );

  /* Create serialized record from unpacked record. */
  rc = vdbeMakeRecord(pIdxKey, file_format, &pKey, &nKey);
  if( rc ) return rc;

  /* Check if someone has a lock. */
  rc = sqlite3rowlockPsmLockRecordQuery(pPsmHandle, iTable, pKey, nKey, p,
                                        pIdxKey->pKeyInfo->aColl[0]);

  sqlite3_free(pKey);

  return rc;
}

int sqlite3BtreeNoConflict(BtCursor *pCur, UnpackedRecord *pIdxKey, int file_format,
                           int *pRes){
  int rc = SQLITE_OK;
  Btree *pBtree = pCur->pBtree;
  int iTable = pCur->pgnoRoot;
  int res = 0;

  if( transBtreeIsUsed(pBtree) ){
    rc = btreeLockIndexKeyQuery(pBtree, iTable, pIdxKey, file_format);
  }

  if( rc==SQLITE_OK || rc==SQLITE_DONE ){
    rc = sqlite3BtreeMovetoUnpackedAll(pCur, pIdxKey, 0, 0, &res, OP_NoConflict);
  }else if( rc==SQLITE_LOCKED ){
    res = 0;
    rc = SQLITE_OK;
  }else{
    /* Error */
    return rc;
  }

  *pRes = res;

  return rc;
}

/* Rollback of transaction btree. */
static void sqlite3TransBtreeRollback(Btree *p, int tripCode, int writeOnly){
  if( transBtreeIsUsed(p) ){
    BtreeTrans *pBtTrans = &p->btTrans;
    sqlite3BtreeRollbackOriginal(pBtTrans->pBtree, tripCode, writeOnly);
    sqlite3rowlockSavepointClose(&pBtTrans->lockSavepoint);
    transRootPagesFinish(&pBtTrans->rootPages);
    sqlite3rowlockIpcUnlockRecordProc(&pBtTrans->ipcHandle, NULL);
    sqlite3rowlockIpcUnlockTablesProc(&pBtTrans->ipcHandle, NULL);
    sqlite3rowlockPsmUnlockRecordProc(&pBtTrans->psmHandle, (u64)p, NULL);
    sqlite3rowlockIpcCachedRowidReset(&pBtTrans->ipcHandle, NULL);
  }
}

/* Rollback of both shared btree and transaction btree. */
int sqlite3BtreeRollbackAll(Btree *p, int tripCode, int writeOnly){
  if( transBtreeIsUsed(p) ){
    sqlite3TransBtreeRollback(p, tripCode, writeOnly);
  }
  return sqlite3BtreeRollbackOriginal(p, tripCode, writeOnly);
}

/* Open write transaction and get metadata. */
int sqlite3BtreeUpdateMetaWithTransOpen(Btree *p, int idx, u32 iMeta){
  int rc = SQLITE_OK;

  /* Begin write transaction for shared btree. */
  if( !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ) return rc;
  }

  return sqlite3BtreeUpdateMetaOriginal(p, idx, iMeta);
}

#ifndef SQLITE_OMIT_BTREECOUNT
int sqlite3BtreeCountAll(sqlite3 *db, BtCursor *pCur, i64 *pnEntry){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  TransRootPage *pRootPage;
  i64 nEntry;
  i64 nEntryIns;
  i64 nEntryDel;

  if( !transBtreeCursorIsUsed(pCur) ){
    return sqlite3BtreeCountOriginal(db, pCur, pnEntry);
  }

  /* Get inserted record count in a transaction. */
  rc = sqlite3BtreeCountOriginal(db, pCurTrans->pCurIns, &nEntryIns);
  if( rc ) return rc;

  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  assert( pRootPage );

  if( pRootPage->deleteAll ){
    /* In case all record in shared btree is deleted. */
    *pnEntry = nEntryIns;
  }else{
    /* Get record count in shared btree. */
    rc = sqlite3BtreeCountOriginal(db, pCur, &nEntry);
    if( rc ) return rc;
    /* Get deleted record count in a transaction. */
    rc = sqlite3BtreeCountOriginal(db, pCurTrans->pCurDel, &nEntryDel);
    if( rc ) return rc;
    *pnEntry = nEntry + nEntryIns - nEntryDel;
  }

  return SQLITE_OK;
}
#endif

/*
** Get a table lock.
** For sqlite_master:
**    Only one user can get WRITE_LOCK.
** For normal table:
**    Multiple users can get WRITE_LOCK.
**    Only one user can get EXCLUSIVE_LOCK if no one get WRITE_LOCK and EXCLUSIVE_LOCK.
*/
int sqlite3BtreeLockTableForRowLock(Btree *p, int iTab, u8 isWriteLock){
  u8 lockType = READ_LOCK;

  assert( isWriteLock==0 || isWriteLock==1 || isWriteLock==2 );
  switch( isWriteLock ){
    case 0: /* Require READ_LOCK */
    case 1: /* Require WRITE_LOCK */
      lockType = READ_LOCK + isWriteLock;
      break;
    case 2: /* Require EXCLUSIVE_LOCK */
      lockType = EXCLSV_LOCK;
      break;
  }
  return sqlite3rowlockIpcLockTableAndAddHistory(p, iTab, lockType);
}

static int rowlockPagecount(BtShared *pBt){
  MemPage *pPage1;     /* Page 1 of the database file */
  int nPage;           /* Number of pages in the database */
  int nPageFile = 0;   /* Number of pages in the database file */

  pPage1 = pBt->pPage1;
  nPage = get4byte(28+(u8*)pPage1->aData);

  sqlite3PagerPagecount(pBt->pPager, &nPageFile);
  if( nPage==0 || memcmp(24+(u8*)pPage1->aData, 92+(u8*)pPage1->aData,4)!=0 ){
    nPage = nPageFile;
  }
  if( (pBt->db->flags & SQLITE_ResetDatabase)!=0 ){
    nPage = 0;
  }

  return nPage;
}

/* Reset page cache. */
int rowlockBtreeCacheReset(Btree *p){
  int rc = SQLITE_OK;
  int iDb;
  int schemaCookie;
  u8 needReset = 0;
  
  if( p->db->init.busy ) return SQLITE_OK;
  if( p->db->nVdbeExec>1 ) return SQLITE_OK;
  if( !transBtreeIsUsed(p) ) return SQLITE_OK;
#ifdef SQLITE_TEST
  if( p->db->nDb==0 ) return SQLITE_OK;
#endif

  /* Check schema version. */
  iDb = rowlockBtreeToIndex(p->db, p);
  schemaCookie = p->db->aDb[iDb].pSchema->schema_cookie;
  rc = rowlockPagerCheckSchemaVers(p->pBt->pPager, schemaCookie, &needReset);
  if( rc ) return rc;

  /* Check database file version. */
  if( !needReset ){
    rc = rowlockPagerCheckDbFileVers(p->pBt->pPager, &needReset);
    if( rc ) return rc;
  }

  if( needReset ){
    rowlockPagerCacheReset(p->pBt->pPager);
    if( p->pBt->pPage1 ){
      PgHdr *pPg = p->pBt->pPage1->pDbPage;
      rc = rowlockPagerReloadDbPage(pPg, p->pBt->pPager);
      if( rc ) return rc;
      p->pBt->nPage = rowlockPagecount(p->pBt);
    }
  }

  return SQLITE_OK;
}

/* Call sqlite3BtreePayloadFetch() for using cursor. */
const void *sqlite3BtreePayloadFetchAll(BtCursor *pCur, u32 *pAmt){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreePayloadFetchOriginal(pCurTrans->pCurIns, pAmt);
  }else{
    return sqlite3BtreePayloadFetchOriginal(pCur, pAmt);
  }
}

/* Call sqlite3VdbeMemFromBtree() for using cursor. */
int sqlite3VdbeMemFromBtreeAll(BtCursor *pCur, u32 offset, u32 amt, Mem *pMem){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3VdbeMemFromBtreeOriginal(pCurTrans->pCurIns, offset, amt, pMem);
  }else{
    return sqlite3VdbeMemFromBtreeOriginal(pCur, offset, amt, pMem);
  }
}

/* Call sqlite3BtreePayload() for using cursor. */
int sqlite3BtreePayloadAll(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreePayloadOriginal(pCurTrans->pCurIns, offset, amt, pBuf);
  }else{
    return sqlite3BtreePayloadOriginal(pCur, offset, amt, pBuf);
  }
}

/* Call sqlite3BtreePayloadSize() for using cursor. */
u32 sqlite3BtreePayloadSizeAll(BtCursor *pCur){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreePayloadSizeOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreePayloadSizeOriginal(pCur);
  }
}

/* Call sqlite3BtreeIntegerKey() for using cursor. */
i64 sqlite3BtreeIntegerKeyAll(BtCursor *pCur){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreeIntegerKeyOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreeIntegerKeyOriginal(pCur);
  }
}

/* Call sqlite3BtreeCursorRestore() for using cursor. */
int sqlite3BtreeCursorRestoreAll(BtCursor *pCur, int *pDifferentRow){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreeCursorRestoreOriginal(pCurTrans->pCurIns, pDifferentRow);
  }else{
    return sqlite3BtreeCursorRestoreOriginal(pCur, pDifferentRow);
  }
}

/* Call sqlite3BtreeCursorHasMoved() for using cursor. */
int sqlite3BtreeCursorHasMovedAll(BtCursor *pCur){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreeCursorHasMovedOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreeCursorHasMovedOriginal(pCur);
  }
}

#ifndef NDEBUG
/* Call sqlite3BtreeCursorIsValid() for using cursor. */
int sqlite3BtreeCursorIsValidAll(BtCursor *pCur){
  if( cursorTransIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    return sqlite3BtreeCursorIsValidOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreeCursorIsValidOriginal(pCur);
  }
}
#endif

/* Set flag which indicates that new rowid is issuued automatically. */
void sqlite3BtreeCachedRowidFlagSet(BtCursor *pCur, u8 flag){
  pCur->autoRowid = flag;
}

static u8 cachedRowidFlagGet(BtCursor *pCur){
  return pCur->autoRowid;
}

/*
** Set the cached rowid value of every cursor in the same database file
** as pCur and having the same root page number as pCur.  The value is
** set to iRowid.
**
** Only positive rowid values are considered valid for this cache.
** The cache is initialized to zero, indicating an invalid cache.
** A btree will work fine with zero or negative rowids.  We just cannot
** cache zero or negative rowids, which means tables that use zero or
** negative rowids might run a little slower.  But in practice, zero
** or negative rowids are very uncommon so this should not be a problem.
*/
void sqlite3BtreeCachedRowidSet(BtCursor *pCur, i64 iRowid){
  if( transBtreeIsUsed(pCur->pBtree) ){
    sqlite3rowlockIpcCachedRowidSet(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot, iRowid);
  }else{
    pCur->cachedRowid = iRowid;
  }
}

/*
** Return the cached rowid for the given cursor.  A negative or zero
** return value indicates that the rowid cache is invalid and should be
** ignored.  If the transaction btree is not used, then a zero is returned.
*/
i64 sqlite3BtreeCachedRowidGet(BtCursor *pCur){
  if( transBtreeIsUsed(pCur->pBtree) ){
    return sqlite3rowlockIpcCachedRowidGet(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot);
  }else{
    return 0;
  }
}

/*
** Set cached rowid. This function is called when cursor is opened.
** Firstly, we read cached rowid which might be set by the other users.
** If it is set, we use it. If it is not set, we calculate it by moving
** cursor to the last and getting the largest rowid.
** The cursor must belongs to shared btree.
*/
int sqlite3BtreeCachedRowidSetByOpenCursor(BtCursor *pCur){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  int res = 0;

  /* If it is a cursor of index, do nothing. */
  if( pCur->pKeyInfo ){
    return SQLITE_OK;
  }

  /* If it is already set, do nothing. */
  if( sqlite3BtreeCachedRowidGet(pCur)!=0 ){
    return SQLITE_OK;
  }

  rc = sqlite3BtreeLastAll(pCur, &res);
  if( rc ) return rc;

  if( res ){
    /* If it is empty table, new rowid is 0. */
    sqlite3BtreeCachedRowidSet(pCur, 0);
  }else{
    u64 rowid = 0;
    assert( sqlite3BtreeCursorIsValid(pCur) );
    rowid = sqlite3BtreeIntegerKeyAll(pCur);
    sqlite3BtreeCachedRowidSet(pCur, rowid);
  }

  return SQLITE_OK;
}

/*
** Begin transaction for shared btree and transaction btree.
** If the btree is sharable(!=ephemeral btree), it should not begin write transaction.
** This is because it enables to begin a write transaction by multiple users. 
** Data insertion and deletion are operated for transaction btree. So we start only
** transaction btree's transaction.
** But it returns an error if write transaction is required for read only database.
*/
int sqlite3BtreeBeginTransAll(Btree *p, int wrflag, int *pSchemaVersion){
  int rc = SQLITE_OK;
  int wrflagNew = wrflag;

  if( !p->db->init.busy && transBtreeIsUsed(p) ){
    u8 eLock = sqlite3rowlockIpcLockTableQuery(&p->btTrans.ipcHandle, SCHEMA_ROOT);
    if( eLock==NOT_LOCKED ){
      rc = sqlite3BtreeLockTable(p, SCHEMA_ROOT, 0);
      if( rc ) return rc;
    }

    rc = rowlockBtreeCacheReset(p);

    if( eLock==NOT_LOCKED ){
      sqlite3rowlockIpcUnlockTable(&p->btTrans.ipcHandle, SCHEMA_ROOT);
    }

    if( rc ) return rc;
  }

  if( p->sharable ){
    if( (p->pBt->btsFlags & BTS_READ_ONLY)!=0 && wrflag ) return SQLITE_READONLY;
    /* If it is not BEGIN EXCLUSIVE, we start a read transaction of shared btree. */
    if( wrflag<=1 ) wrflagNew = 0;
    /* If it is BEGIN IMMEDIATE, start a write transaction of shared btree. */
    else if( wrflag==BEGIN_IMMEDIATE_FLAG ){
      BtreeTrans *btreeTrans = &p->btTrans;
      wrflagNew = 1;
      /* Return error if cannot be accquired EXCLUSIVE_LOCK on pager
      or any record lock/table lock is existed */
      if( !rowlockPagerCheckLockAvailable(p->pBt->pPager, EXCLUSIVE_LOCK)
      && sqlite3rowlockIpcCheckTableLockExisted(&btreeTrans->ipcHandle) )
        return SQLITE_BUSY;
    }
  }
  rc = sqlite3BtreeBeginTransOriginal(p, wrflagNew, pSchemaVersion);
  if( rc ) return rc;

  /* 
  ** In the sqlite3BtreeBeginTransOriginal() function, p->pBt->btsFlags maybe changed
  ** while loading page1. Therefore the program need check p->pBt->btsFlags again.
  ** If the database reloaded into read-only and wrflag is write flag return SQLITE_READONLY
  ** because write transactions are not possible on a read-only database.
  */
  if( p->sharable && (p->pBt->btsFlags & BTS_READ_ONLY)!=0 && wrflag ) return SQLITE_READONLY;

  if( transBtreeIsUsed(p) ){
    rc = sqlite3TransBtreeBeginTrans(p, wrflag);
    if( rc ) return rc;
  }
  return SQLITE_OK;
}

/* 
** Return 1 if it is in a transaction. We consider transaction 
** btree and shared btree.
*/
int sqlite3BtreeIsInTransAll(Btree *p){
  if( !p || !transBtreeIsUsed(p) ){
    return sqlite3BtreeIsInTransOriginal(p);
  }else{
    BtreeTrans *pBtTrans = &p->btTrans;
    return sqlite3BtreeIsInTransOriginal(pBtTrans->pBtree);
  }
}

/*
** Open write transaction of share btree in order to commit data into share btree.
*/
int sqlite3BtreeBeginTransForCommit(sqlite3 *db){
  int rc = SQLITE_OK;
  int i;

  /* 
  ** Begin write transaction for shared btree if only transaction btree is in 
  ** transaction and it is not read only database.
  */
  for(i=0; rc==SQLITE_OK && i<db->nDb; i++){ 
    Btree *pBt = db->aDb[i].pBt;
    if( pBt && transBtreeIsUsed(pBt) && 
        sqlite3BtreeIsInTransOriginal(pBt->btTrans.pBtree) && 
        !sqlite3BtreeIsInTransOriginal(pBt) ){
      int readOnly = (pBt->pBt->btsFlags & BTS_READ_ONLY)==1;
      rc = sqlite3BtreeBeginTransOriginal(pBt, !readOnly, 0);
    }
  }
  return rc;
}

/* Unlock tables locks acquired in the statement. */
void sqlite3BtreeUnlockStmtTableLock(sqlite3 *db){
  int i;

  for(i=0; i<db->nDb; i++){ 
    Btree *pBt = db->aDb[i].pBt;
    if( pBt && transBtreeIsUsed(pBt) ){
      sqlite3rowlockIpcUnlockTablesStmtProc(&pBt->btTrans.ipcHandle, NULL);
    }
  }
}

/* Call sqlite3BtreeBeginStmt() for transaction btree if used. */
int sqlite3BtreeBeginStmtAll(Btree *p, int iStatement){
  if( !transBtreeIsUsed(p) ){
    return sqlite3BtreeBeginStmtOriginal(p, iStatement);
  }else{
    BtreeTrans *pBtTrans = &p->btTrans;
    int rc = sqlite3BtreeBeginStmtOriginal(pBtTrans->pBtree, iStatement);
    if( rc ) return rc;
    return sqlite3TransBtreeSavepointCreate(p, p->db->nSavepoint);
  }
}

/* Call hasSharedCacheTableLock() for transaction btree if used. */
#ifdef SQLITE_DEBUG
int hasSharedCacheTableLockAll(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType){
  if(  !transBtreeIsUsed(pBtree) ){
    return hasSharedCacheTableLockOriginal(pBtree, iRoot, isIndex, eLockType);
  }else{
    BtreeTrans *pBtTrans = &pBtree->btTrans;
    return hasSharedCacheTableLockOriginal(pBtTrans->pBtree, iRoot, isIndex, eLockType);
  }
}
#endif

/* Open write transaction and create table. */
int sqlite3BtreeCreateTableWithTransOpen(Btree *p, Pgno *piTable, int flags){
  int rc = SQLITE_OK;

  if( !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ) return rc;
  }

  return sqlite3BtreeCreateTableOriginal(p, piTable, flags);
}

/* 
** Insert records into shared btree from transaction btree. 
** These records are inserted in a transaction.
*/
static int transBtreeCommitTableInsert(BtCursor *pCur){
  int rc = SQLITE_OK;
  BtCursor *pCurIns = pCur->btCurTrans.pCurIns;
  int res = 0;
  char *buff = NULL;

  rc = sqlite3BtreeFirst(pCurIns, &res);
  if( rc ) return rc;
  if( res==0 ) {
    rc = SQLITE_OK;
  }else{
    rc = SQLITE_DONE;
  }
  while( rc==SQLITE_OK ){
    char *tmp = NULL;
    u64 nKey = 0;
    u32 u32nKey = 0;
    void *pKey = NULL;
    u32 nData = 0;
    void *pData = NULL;
    BtreePayload x = {0};
    int flags;

    rc = btreePayloadWithMalloc(pCurIns, &u32nKey, &buff);
    if( rc ) goto commit_table_insert_failed;

    if( pCur->pKeyInfo ){
      /* For an index btree. */
      nKey = u32nKey;
      pKey = buff;
      nData = 0;
      pData = NULL;
    }else{
      /* For a table btree. */
      nKey = sqlite3BtreeIntegerKey(pCurIns);
      pKey = NULL;
      nData = u32nKey;
      pData = buff;
    }

    /* 
    ** Check if same key exists in shared btree.
    ** If found, it is UPDATE, else it is INSERT.
    */
    rc = btreeMovetoOriginal(pCur, pKey, nKey, 0, &res);
    if( rc ) goto commit_table_insert_failed;
    if( res==0 ){
      flags = OPFLAG_SAVEPOSITION;
      getCellInfoOriginal(pCur);
    }else{
      flags = OPFLAG_APPEND;
    }

    /* Operate INSERT or UPDATE. */
    x.pKey = pKey;
    x.nKey = nKey;
    x.pData = pData;
    x.nData = nData;
    rc = sqlite3BtreeInsert(pCur, &x, flags, 0);
    if( rc ) goto commit_table_insert_failed;

    rc = sqlite3BtreeNext(pCurIns, 0);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) goto commit_table_insert_failed;
  }
  assert( rc==SQLITE_DONE );

  sqlite3_free(buff);
  return SQLITE_OK;

commit_table_insert_failed:
  sqlite3_free(buff);
  return rc;
}

/* 
** Delete records from shared btree. These records are deleted in a transaction 
** and information was stored in transaction btree.
*/
static int transBtreeCommitTableDelete(BtCursor *pCur, TransRootPage *pRootPage){
  int rc = SQLITE_OK;
  BtCursor *pCurDel = pCur->btCurTrans.pCurDel;
  int res = 0;
  char *buff = NULL;

  if( pRootPage->deleteAll ){
    return sqlite3BtreeClearTableOfCursorOriginal(pCur);
  }

  if( pRootPage->iDel==0 ){
    /* Table does not exist in transaction. So do nothing. */
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurDel, &res);
  if( rc ) return rc;
  if( res==0 ) {
    rc = SQLITE_OK;
  }else{
    rc = SQLITE_DONE;
  }

  while( rc == SQLITE_OK ){
    char *tmp = NULL;
    u64 nKey = 0;
    void *pKey = NULL;
    int nData = 0;
    void *pData = NULL;

    if( pCur->pKeyInfo ){
      /* For an index btree. */
      u32 amt = sqlite3BtreePayloadSizeOriginal(pCurDel);
      tmp = (char*)sqlite3Realloc(buff, amt);
      if( !tmp ){
        rc = SQLITE_NOMEM_BKPT;
        goto commit_table_delete_failed;
      }
      buff = tmp;
      rc = sqlite3BtreePayloadOriginal(pCurDel, 0, amt, buff);
      if( rc ) goto commit_table_delete_failed;

      nKey = amt;
      pKey = buff;
    }else{
      /* For a table btree. */
      nKey = sqlite3BtreeIntegerKeyOriginal(pCurDel);
      pKey = NULL;
    }

    rc = btreeCursorMovetoKey(pCur, pKey, nKey, &res);
    if( rc ) goto commit_table_delete_failed;
    assert( res==0 );

    rc = sqlite3BtreeDelete(pCur, BTREE_AUXDELETE);
    if( rc ) goto commit_table_delete_failed;

    rc = sqlite3BtreeNext(pCurDel, 0);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) goto commit_table_delete_failed;
  }
  assert( rc==SQLITE_DONE );

  sqlite3_free(buff);
  return SQLITE_OK;

commit_table_delete_failed:
  sqlite3_free(buff);
  return rc;
}

/*
** sqlite3BtreeIncrVacuum() for rowlock. Before calling sqlite3BtreeIncrVacuum(),
** this function gets a EXCLSV_LOCK of sqlite_master and begins a write transaction.
*/
int sqlite3BtreeIncrVacuumForRowLock(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;

  assert( p!=NULL );

  if( transBtreeIsUsed(p) ){
    rc = sqlite3rowlockIpcLockTableAndAddHistory(p, SCHEMA_ROOT, WRITE_LOCK);
    if( rc ) return rc;

    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ) return rc;
  }

  return sqlite3BtreeIncrVacuumOriginal(p);
}


/* Get EXCLSV_LOCK of all tables for VACUUM. */
int sqlite3rowlockExclusiveLockAllTables(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  u8 prevLock;
  Schema *pSchema;
  unsigned int nTables = 0;
  unsigned int counter = 0;
  HashElem *i;

  assert( p!=NULL );

  if( !transBtreeIsUsed(p) ) return SQLITE_OK;
  
  /* Get EXCLSV_LOCK of sqlite_master. */
  rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, SCHEMA_ROOT, WRITE_LOCK, MODE_LOCK_COMMIT, &prevLock);
  if( rc ) return rc;
  assert( prevLock==NOT_LOCKED );
  
  /* Get EXCLSV_LOCK of normal tables. */
  pSchema = (Schema*)p->pBt->pSchema;
  for(i=sqliteHashFirst(&pSchema->tblHash); i; i=sqliteHashNext(i)){
    Table *pTable = (Table*)sqliteHashData(i);
    if( pTable->tnum==SCHEMA_ROOT || pTable->tnum==0 ) continue;
    rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, pTable->tnum, EXCLSV_LOCK, MODE_LOCK_COMMIT, &prevLock);
    if( rc ) goto exclusive_lock_all_tables_failed;
    assert( prevLock==NOT_LOCKED );
    nTables++;
  }

  return SQLITE_OK;

exclusive_lock_all_tables_failed:
  for(i=sqliteHashFirst(&pSchema->tblHash); i && counter<nTables; i=sqliteHashNext(i)){
    Table *pTable = (Table*)sqliteHashData(i);
    if( pTable->tnum==SCHEMA_ROOT || pTable->tnum==0 ) continue;
    sqlite3rowlockIpcUnlockTable(&pBtTrans->ipcHandle, pTable->tnum);
    counter++;
  }
  sqlite3rowlockIpcUnlockTable(&pBtTrans->ipcHandle, SCHEMA_ROOT);
  return rc;
}

/*
** Check whether the table is mofdified by confirmig the existence of record
** of insertion table and deletion table.
*/
static int isTableModified(BtCursorTrans *pCurTrans, int *isModified){
  int rc;
  int res;

  if( pCurTrans->deleteAll ){
    *isModified = 1;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurTrans->pCurIns, &res);
  if( rc ) return rc;
  if( res == 0 ){
    *isModified = 1;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurTrans->pCurDel, &res);
  if( rc ) return rc;
  if( res == 0 ){
    *isModified = 1;
    return SQLITE_OK;
  }

  *isModified = 0;
  return SQLITE_OK;
}

/* Get EXCLSV_LOCK of modified tables for COMMIT. */
static int exclusiveLockTables(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  HashI64 *pTransRootPages = &pBtTrans->rootPages;
  BtCursor *pCur = NULL;
  HashElemI64 *elem = NULL;
  int *isModified = NULL;
  u8 *prevLock = NULL;
  u8 prevLockMaster = 0;
  int i = 0;
  int iTableRollback = 0;
  u64 counter = 0;

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ) return SQLITE_NOMEM_BKPT;
  
  /* Count the number of elements. */
  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    counter++;
    elem = sqliteHashI64Next(elem);
  }
  if( counter>0 ){
    rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, SCHEMA_ROOT, WRITE_LOCK, MODE_LOCK_COMMIT, &prevLockMaster);
    if( rc ){
      sqlite3_free(pCur);
      return rc;
    }
    isModified = (int*)sqlite3MallocZero(sizeof(int) * counter);
    prevLock = (u8*)sqlite3MallocZero(sizeof(u8) * counter);
    if( !isModified || !prevLock ){
      rc = SQLITE_NOMEM_BKPT;
      goto exclusive_lock_tables_failed;
    }
  }

  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    Pgno iTable = sqliteHashI64Key(elem);
    TransRootPage *pRootPage = (TransRootPage*)sqliteHashI64Data(elem);
    struct KeyInfo *pKeyInfo = pRootPage->pKeyInfo;
    assert( iTable==(i64)(int)iTable );

    rc = sqlite3BtreeCursorAll(p, iTable, 0, pKeyInfo, pCur, 1);
    if( rc ) goto exclusive_lock_tables_failed;

    /* Check if the table is modified. */
    rc = isTableModified(&pCur->btCurTrans, &isModified[i]);
    if( rc ){
      sqlite3BtreeCloseCursorAll(pCur);
      goto exclusive_lock_tables_failed;
    }

    /* Get EXCLSV_LOCK if table is modified. */
    if( isModified[i] ){
      rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, (int)iTable, EXCLSV_LOCK, MODE_LOCK_COMMIT, &prevLock[i]);
    }
    sqlite3BtreeCloseCursorAll(pCur);
    if( rc ) goto exclusive_lock_tables_failed;

    i++;
    elem = sqliteHashI64Next(elem);
  }

  sqlite3_free(isModified);
  sqlite3_free(prevLock);
  sqlite3_free(pCur);
  return SQLITE_OK;

exclusive_lock_tables_failed:
  /* Rollback the lock status. */
  if( isModified && prevLock ){
    elem = sqliteHashI64First(pTransRootPages);
    i = 0;
    while( elem ){
      i64 iTable = sqliteHashI64Key(elem);
      if( isModified[i] ) sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, (int)iTable, MODE_LOCK_FORCE, prevLock[i], NULL);
      i++;
      elem = sqliteHashI64Next(elem);
    }
  }
  if( counter>0 ) sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, SCHEMA_ROOT, MODE_LOCK_FORCE, prevLockMaster, NULL);
  sqlite3_free(isModified);
  sqlite3_free(prevLock);
  sqlite3_free(pCur);
  return rc;
}

/*
** Commit records.
** Modified record information is stored in a transaction btree. Read them and modifiy a shared btree.
*/
int sqlite3TransBtreeCommit(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  HashI64 *pTransRootPages = &pBtTrans->rootPages;
  BtCursor *pCur = NULL;
  HashElemI64 *elem = NULL;
  u8 eLock;
  u8 isDdl = 0;

  if( !transBtreeIsUsed(p) || (p->pBt->btsFlags & BTS_READ_ONLY)==1 ) return SQLITE_OK;

  eLock = sqlite3rowlockIpcLockTableQuery(&p->btTrans.ipcHandle, SCHEMA_ROOT);
  if( eLock==WRITE_LOCK ) isDdl = 1;

  /* Get EXCLSV_LOCK for modified tables in order to prevent from the table access by the other process. */
  rc = exclusiveLockTables(p);
  if( rc ) return rc;

  /*
  ** Reset the page cache if database file is changed.
  ** We check the schema version and database file version. But the scheam version on memory is always
  ** different from that of database file if DDL is executing.
  ** But in case of DDL, the modification is done for pager in shared btree. We should not reset the cache.
  ** If we reset the cache, the modification is also deleted. So we reset the cache if it is not DDL.
  ** It is no problem because DDL is operated under exclusion control.
  ** In order to know it is DDL or not, we check if sqlite_master is locked or not.
  */
  if( !isDdl ){
    rc = rowlockBtreeCacheReset(p);
    if( rc ) return rc;
  }

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ){
    return SQLITE_NOMEM_BKPT;
  }

  /* Begin write transaction for shared btree if only transaction btree is in transaction. */
  if( sqlite3BtreeIsInTransOriginal(pBtTrans->pBtree) && 
      !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ){
      sqlite3_free(pCur);
      return rc;
    }
  }

  /* 
  ** Traverse the transaction tree and write all the rows from
  ** this tree to the shared cache.
  */
  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    i64 iTable = sqliteHashI64Key(elem);
    TransRootPage *pRootPage = (TransRootPage*)sqliteHashI64Data(elem);
    struct KeyInfo *pKeyInfo = pRootPage->pKeyInfo;
    assert( iTable==(i64)(int)iTable );

    /* Create cursor for the target table. Table lock is required for a write cursor creation. */
    rc = sqlite3BtreeLockTableOriginal(p, (int)iTable, 1);
    if( rc ) goto trans_btree_commit_failed;
    rc = sqlite3BtreeCursorAll(p, iTable, BTREE_WRCSR, pKeyInfo, pCur, 1);
    if( rc ) goto trans_btree_commit_failed;

    /* Delete */
    rc = transBtreeCommitTableDelete(pCur, pRootPage);
    if( rc ) goto trans_btree_commit_failed;

    /* INSERT or UPDATE */
    rc = transBtreeCommitTableInsert(pCur);
    if( rc ) goto trans_btree_commit_failed;
    
    sqlite3BtreeCloseCursorAll(pCur);
    elem = sqliteHashI64Next(elem);
  }

  /* Clear Btree Cursor in normal execution routine */
  sqlite3_free(pCur);

  /* Clear transaction data. */
  sqlite3TransBtreeRollback(p, SQLITE_OK, 0);
 
  return SQLITE_OK;

trans_btree_commit_failed:
  sqlite3BtreeCloseCursorAll(pCur);
  sqlite3_free(pCur);
  return rc;
}

/* 
** Set autocommit in order to execute commit after DDL.
** If I have an exclusive lock, DDL is executing.
*/
void sqlite3SetForceCommit(Vdbe *pVdbe){
  int i;
  sqlite3 *db = pVdbe->db;

  if( db->autoCommit ) return;
  for(i=0; i<db->nDb; i++){
    Btree *p = db->aDb[i].pBt;
    u8 eLock;
    if( !p || !transBtreeIsUsed(p) ) continue;
    eLock = sqlite3rowlockIpcLockTableQuery(&p->btTrans.ipcHandle, SCHEMA_ROOT);
    if( eLock==WRITE_LOCK ){
      p->db->autoCommit = 1;
      pVdbe->forceCommit = 1;
      return;
    }
  }
}

int btreeValidTableKey(BtCursor *pCur, i64 *prev, int isNext) {
  i64 nKey1 = 0;
  i64 result = 0;
  if( pCur->pKeyInfo || !prev ){
    /* Index case */
    return SQLITE_OK;
  }

  nKey1 = sqlite3BtreeIntegerKeyOriginal(pCur);
  if( *prev==0 ){
    *prev = nKey1;
    return SQLITE_OK;
  }

  result = nKey1 - *prev;
  if( (isNext && result>0) ||
    (!isNext && result<0) ) {
    return SQLITE_OK;
  }

  return SQLITE_CORRUPT_BKPT;
}

int sqlite3BtreePutDataAll(BtCursor *pCur, u32 offset, u32 amt, void *z) {
  int rc;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtreePayload x = { 0 };
  void* pBuf;
  if( cursorTransIsUsed(pCur) ){
    pCurTrans->pCurIns->curFlags |= BTCF_Incrblob; /* temporary fix */
    return sqlite3BtreePutDataOriginal(pCurTrans->pCurIns, offset, amt, z);
  } else if( cursorSharedIsUsed(pCurTrans) ){
    /* The trans cursor must be opened with write flag */
    if( (pCurTrans->pCurIns->curFlags & BTCF_WriteFlag)==0 ||
        (pCurTrans->pCurDel->curFlags & BTCF_WriteFlag)==0 ){
      return SQLITE_READONLY;
    }
    /* Need update in both shared and trans. */
    getCellInfoOriginal(pCur);
    /* Malloc pBuf for get total payload */
    pBuf = sqlite3_malloc(pCur->info.nPayload);
    if( !pBuf ) return SQLITE_NOMEM_BKPT;
    rc = sqlite3BtreePayloadOriginal(pCur, 0, pCur->info.nPayload, pBuf);
    if( rc ) goto blob_put_data_fail;
    x.pKey = NULL;
    x.nKey = pCur->info.nKey;
    x.nData = pCur->info.nPayload;
    x.pData = pBuf;
    /* Add a 'delete' to pCurDel, and a 'insert' to pCurIns */

    /* Disable invalidate incrblob before inserting */
    pCur->pBtree->hasIncrblobCur = 0;
    rc = sqlite3TransBtreeInsert(pCur, &x, 0, 0);

    /* Re-enable invalidate incrblob after inserting */
    pCur->pBtree->hasIncrblobCur = 1;
    if( rc ) goto blob_put_data_fail;

    /* At this point, pCur (in shared) already seek to necessary position */
    assert(pCur->eState != CURSOR_REQUIRESEEK);
    if ( pCur->eState!=CURSOR_VALID ) {
      return SQLITE_ABORT;
    }

    // Save (and release page) other cursor which is point at the same cell
    VVA_ONLY(rc = ) saveAllCursorsOriginal(pCur->pBt, pCur->pgnoRoot, pCur);
    assert(rc == SQLITE_OK);

    /* Put data to trans */
    pCurTrans->pCurIns->curFlags |= BTCF_Incrblob; /* temporary fix */
    rc = sqlite3BtreePutDataOriginal(pCurTrans->pCurIns, offset, amt, z);
  }else{
    return sqlite3BtreePutDataOriginal(pCur, offset, amt, z);
  }
blob_put_data_fail:
  if( pBuf ) sqlite3_free(pBuf);
  return rc;
}

/*
** Mark this cursor as an incremental blob cursor
*/
void sqlite3BtreeIncrblobCursorAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans;
  BtCursor *pCurIns;
  BtCursor *pCurDel;
  /* Mark incremental blob cursor for bt share */
  sqlite3BtreeIncrblobCursorOriginal(pCur);
  /* Mark incremental blob cursor for bt trans */
  if( !transBtreeCursorIsUsed(pCur) ){
    return;
  }
  pCurTrans = &pCur->btCurTrans;
  pCurIns = pCurTrans->pCurIns;
  pCurDel = pCurTrans->pCurDel;
  sqlite3BtreeIncrblobCursorOriginal(pCurIns);
  sqlite3BtreeIncrblobCursorOriginal(pCurDel);
}

/*
 * Move Cursor to valid in both shard and transaction tree
 */
static int btreeMovetoAll(BtCursor *pCur, const void *pKey, i64 nKey, int bias, int *pRes) {
  int rc;                    /* Status code */
  UnpackedRecord *pIdxKey;   /* Unpacked index key */

  if ( pKey ) {
    KeyInfo *pKeyInfo = pCur->pKeyInfo;
    assert(nKey == (i64)(int)nKey);
    pIdxKey = sqlite3VdbeAllocUnpackedRecord(pKeyInfo);
    if ( pIdxKey==0 ) return SQLITE_NOMEM_BKPT;
    sqlite3VdbeRecordUnpack(pKeyInfo, (int)nKey, pKey, pIdxKey);
    if ( pIdxKey->nField==0 || pIdxKey->nField>pKeyInfo->nAllField ) {
      rc = SQLITE_CORRUPT_BKPT;
      goto moveto_done;
    }
  }
  else {
    pIdxKey = 0;
  }
  rc = sqlite3BtreeMovetoUnpackedAll(pCur, pIdxKey, nKey, bias, pRes, 0);
moveto_done:
  if ( pIdxKey ) {
    sqlite3DbFree(pCur->pKeyInfo->db, pIdxKey);
  }
  return rc;
}

/*
 * Restore the Cursor and Repoint to correct cursor between share and transaction
 * Btree incase of blob is updated.
 */
static int btreeRestoreCursorPositionAll(BtCursor *pCur) {
  int rc;
  int skipNext = 0;
  /* assert(cursorOwnsBtShared(pCur)); */
  assert(pCur->eState >= CURSOR_REQUIRESEEK);
  if ( pCur->eState==CURSOR_FAULT ) {
    return pCur->skipNext;
  }
  pCur->eState = CURSOR_INVALID;
  if ( sqlite3FaultSim(410) ) {
    rc = SQLITE_IOERR;
  }
  else {
    rc = btreeMovetoAll(pCur, pCur->pKey, pCur->nKey, 0, &skipNext);
  }
  if ( rc==SQLITE_OK ) {
    sqlite3_free(pCur->pKey);
    pCur->pKey = 0;
    assert(pCur->eState==CURSOR_VALID || pCur->eState==CURSOR_INVALID);
    if ( skipNext ) pCur->skipNext = skipNext;
    if ( pCur->skipNext && pCur->eState==CURSOR_VALID ) {
      pCur->eState = CURSOR_SKIPNEXT;
    }
  }
  return rc;
}

#ifndef SQLITE_OMIT_INCRBLOB
/* Check invalidate state for both Pcur and PcurIns(Data is in transaction)*/
static u8 getCursorState(BtCursor *pCur) {
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  return (cursorTransIsUsed(pCur)) ? pCurTrans->pCurIns->eState : pCur->eState;
}

/*
 * Read Blob pay load from cursor
 * Allocate transaction cursor if READONLY cursor need to access transaction data 
 */
int sqlite3BtreePayloadCheckedAll(BtCursor *pCur, u32 offset, u32 amt, void *pBuf) {
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  u8 curState = getCursorState(pCur);
  int rc = SQLITE_OK;

  if( curState==CURSOR_VALID ) {
    /* assert(cursorOwnsBtShared(pCur)); */
    return sqlite3BtreePayloadAll(pCur, offset, amt, pBuf);
  } else if( curState==CURSOR_INVALID ) {
    return SQLITE_ABORT;
  }

  /* Check if the cursor is READONLY and Transaction btree is in WRITE and
   * Transaction cursor has not been created
   * Allocate transaction cursor so that READONLY cursor can read transaction data 
   */
  if ( ((pCur->curFlags & BTCF_WriteFlag)==0) &&
    (sqlite3BtreeIsInTransOriginal(pCur->pBtree->btTrans.pBtree)) &&
    (pCur->btCurTrans.pCurIns == NULL) ) {  
    rc = transBtreeCursor(pCur->pBtree, pCur->pgnoRoot, 0, NULL, pCur);
    if ( rc ) return rc;
  }

  /* Seek and access payload */
  /* assert(cursorOwnsBtShared(pCur)); */
  rc = btreeRestoreCursorPositionAll(pCur);
  return rc ? rc : sqlite3BtreePayloadAll(pCur, offset, amt, pBuf);
}
#endif

#endif /* SQLITE_OMIT_ROWLOCK */
