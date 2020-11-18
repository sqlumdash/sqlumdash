/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for the main implementation of row lock 
** feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_H
#define SQLITE_ROWLOCK_H

/* btreeInt.h should be included by .c file. */
#include "rowlock_hash.h"
#include "rowlock_ipc.h"
#include "rowlock_psm_lock.h"
#include "rowlock_savepoint.h"
#include "sqliteInt.h"

/* Indicates transaction cursor is using or not. */
#define CURSOR_NOT_USE    0x0
#define CURSOR_USE_SHARED 0x1
#define CURSOR_USE_TRANS  0x2

#define ROW_LOCK_CURSOR_IN_TRANS 0
#define ROW_LOCK_CURSOR_IN_COMMIT 1

/* Indicates a table type. */
#define TABLE_NORMAL   0 /* Normal table */
#define TABLE_SEQUENCE 1 /* sqlite_sequence table */
#define INDEX_NORMAL   2 /* Index */
#define INDEX_UNIQUE   3 /* Unique index or primary key (non integer) */

typedef struct BtreeTrans {
  Btree *pBtree;                  /* Store inserted/deleted/updated records in a transaction */
  HashI64 rootPages;              /* mapping of root pages between shared btree and transaction btree */
  IpcHandle ipcHandle;            /* a handle of shared object storing row lock information */
  PsmLockHandle psmHandle;        /* Handle of process shared memory */
  RowLockSavepoint lockSavepoint; /* row lock savepoint */
} BtreeTrans;

typedef struct BtCursorTrans {
  BtCursor *pCurIns;
  BtCursor *pCurDel;
  int state;
  u8 deleteAll;
  u8 type;
} BtCursorTrans;

/*
** This structure has mapping of rootpage in shared 
** btree and transaction btree.
** iIns indicates rootpage of table which has inserted records in transaction.
** iDel indicates rootpage of table which has deleted records in transaction.
** deleteAll = 1 if all records in shared btree are deleted.
** There are 3 cases:
**   case-1: iIns>0, iDel>0, deleteAll=0
**           Some records are inserted or updated or deleted.
**   case-2: iIns>0, iDel>0, deleteAll=1 
**           Some records are inserted or updated or deleted.
**           And all records in shared btree are deleted.
**   case-3: iIns=0, iDel=0, deleteAll=1
**           No record is inserted or updated.
**           And all records in shared btree are deleted.
** The tables in transaction btree are created when cursor creation 
** (not table/index creation).
*/
typedef struct TransRootPage {
  Pgno iIns;
  Pgno iDel;
  u8 deleteAll; /* True if all records in shared btree are deleted */
  struct KeyInfo *pKeyInfo;
} TransRootPage;

int rowlockInitialize();

int rowlockStrCat(char *dest, size_t size, const char *src1, const char *src2);
void *rowlockDefaultMalloc(void *allocator, sqlite3_int64 n);
void rowlockDefaultFree(void *allocator, void *p);

/*
** sqlite3TransBtreeXXX() functions operate related in transaction btree.
** sqlite3BtreeXXXAll() functions operate in consideration of both shared btree and transaction btree.
** sqlite3BtreeXXXOriginal() functions are enable to use original btree's function from rowlock.c.
**/
int sqlite3BtreeOpenAll(sqlite3_vfs *pVfs, const char *zFilename, sqlite3 *db, Btree **ppBtree, int flags, int vfsFlags);
int sqlite3BtreeOpenOriginal(sqlite3_vfs *pVfs, const char *zFilename, sqlite3 *db, Btree **ppBtree, int flags, int vfsFlags);
int sqlite3BtreeCloseAll(Btree *p);
int sqlite3BtreeCloseOriginal(Btree *p);
int sqlite3TransBtreeBeginTrans(Btree *p, int wrflag);
int sqlite3TransBtreeCreateTable(Btree *p, Pgno iTable, struct KeyInfo *pKeyInfo, TransRootPage **ppTransRootPage);
int sqlite3TransBtreeClearTable(Btree *p, int iTable, int *pnChange);
int sqlite3TransBtreeCursor(Btree *p, int iTable, int wrFlag, struct KeyInfo *pKeyInfo, BtCursor *pCur);
int sqlite3BtreeCloseCursorAll(BtCursor *pCur);
int sqlite3BtreeCloseCursorOriginal(BtCursor *pCur);
int sqlite3BtreeClearTableOfCursorAll(BtCursor *pCur);
int sqlite3BtreeClearTableOfCursorOriginal(BtCursor *pCur);
int sqlite3TransBtreeFirst(BtCursor *pCur, int *pInsRes);
int sqlite3TransBtreeLast(BtCursor *pCur, int *pInsRes);
int sqlite3TransBtreeNext(BtCursor *pCur, int flags);
int sqlite3TransBtreeMovetoUnpacked(BtCursor *pCur, UnpackedRecord *pIdxKey, i64 intKey, int biasRight, int *pRes);
int sqlite3TransBtreeInsert(BtCursor *pCur, const BtreePayload *pX, int flags, int seekResult);
int sqlite3TransBtreeDelete(BtCursor *pCur, u8 flags);
int sqlite3TransBtreeCommit(Btree *p);
int sqlite3TransBtreeSavepointCreate(Btree *p, int iSavepoint);
int sqlite3TransBtreeSavepoint(Btree *p, int op, int iSavepoint);
int sqlite3BtreeSavepointOriginal(Btree *p, int op, int iSavepoint);

int sqlite3BtreeFirstAll(BtCursor *pCur, int *pRes);
int sqlite3BtreeLastAll(BtCursor *pCur, int *pRes);
int sqlite3BtreeAdvanceAll(BtCursor *pCur, int flags, int(*xAdvance)(BtCursor*, int));

#define sqlite3BtreePreviousAll(pCur, flags) sqlite3BtreeAdvanceAll(pCur, flags, sqlite3BtreePrevious)
#define sqlite3BtreeNextAll(pCur, flags) sqlite3BtreeAdvanceAll(pCur, flags, sqlite3BtreeNext)

void sqlite3BtreeCachedRowidFlagSet(BtCursor *pCur, u8 flag);
void sqlite3BtreeCachedRowidSet(BtCursor *pCur, i64 iRowid);
i64 sqlite3BtreeCachedRowidGet(BtCursor *pCur);
int sqlite3BtreeCachedRowidSetByOpenCursor(BtCursor *pCur);

int sqlite3BtreeBeginTransAll(Btree *p, int wrflag, int *pSchemaVersion);
int sqlite3BtreeBeginTransOriginal(Btree *p, int wrflag, int *pSchemaVersion);
int sqlite3BtreeIsInTransAll(Btree *p);
int sqlite3BtreeIsInTransOriginal(Btree *p);
int sqlite3BtreeBeginTransForCommit(sqlite3 *db);
int sqlite3BtreeBeginStmtAll(Btree *p, int iStatement);
int sqlite3BtreeBeginStmtOriginal(Btree *p, int iStatement);
int hasSharedCacheTableLockAll(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType);
int hasSharedCacheTableLockOriginal(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType);
int sqlite3BtreeLockTableOriginal(Btree *p, int iTab, u8 isWriteLock);
int sqlite3BtreeCreateTableWithTransOpen(Btree *p, Pgno *piTable, int flags);
int sqlite3BtreeCreateTableOriginal(Btree *p, Pgno *piTable, int flags);
int sqlite3BtreeDropTableOriginal(Btree *p, Pgno iTable, int *piMoved);
int sqlite3BtreeDropTableAll(Btree *p, Pgno iTable, int *piMoved);
int sqlite3BtreeRollbackOriginal(Btree *p, int tripCode, int writeOnly);
int sqlite3BtreeRollbackAll(Btree *p, int tripCode, int writeOnly);
int sqlite3BtreeUpdateMetaWithTransOpen(Btree *p, int idx, u32 iMeta);
int sqlite3BtreeUpdateMetaOriginal(Btree *p, int idx, u32 iMeta);
#ifndef SQLITE_OMIT_BTREECOUNT
int sqlite3BtreeCountAll(sqlite3 *db, BtCursor *pCur, i64 *pnEntry);
int sqlite3BtreeCountOriginal(sqlite3 *db, BtCursor *pCur, i64 *pnEntry);
#endif
int sqlite3BtreeLockTableOriginal(Btree *p, int iTab, u8 isWriteLock);
void sqlite3BtreeUnlockStmtTableLock(sqlite3 *db);
int sqlite3BtreeLockTableForRowLock(Btree *p, int iTab, u8 isWriteLock);
int sqlite3BtreeIncrVacuumForRowLock(Btree *p);
int sqlite3BtreeIncrVacuumOriginal(Btree *p);
const void *sqlite3BtreePayloadFetchOriginal(BtCursor *pCur, u32 *pAmt);
const void *sqlite3BtreePayloadFetchAll(BtCursor *pCur, u32 *pAmt);
int sqlite3VdbeMemFromBtreeOriginal(BtCursor *pCur, u32 offset, u32 amt, Mem *pMem);
int sqlite3VdbeMemFromBtreeAll(BtCursor *pCur, u32 offset, u32 amt, Mem *pMem);
int sqlite3BtreePayloadOriginal(BtCursor *pCur, u32 offset, u32 amt, void *pBuf);
int sqlite3BtreePayloadAll(BtCursor *pCur, u32 offset, u32 amt, void *pBuf);
u32 sqlite3BtreePayloadSizeOriginal(BtCursor *pCur);
u32 sqlite3BtreePayloadSizeAll(BtCursor *pCur);
i64 sqlite3BtreeIntegerKeyOriginal(BtCursor *pCur);
i64 sqlite3BtreeIntegerKeyAll(BtCursor *pCur);
int sqlite3BtreeCursorOriginal(Btree *p, Pgno iTable, int wrFlag, struct KeyInfo *pKeyInfo, BtCursor *pCur);
int sqlite3BtreeCursorAll(Btree *p, Pgno iTable, int wrFlag, struct KeyInfo *pKeyInfo, BtCursor *pCur, int flag);
int sqlite3BtreeCursorRestoreOriginal(BtCursor *pCur, int *pDifferentRow);
int sqlite3BtreeCursorRestoreAll(BtCursor *pCur, int *pDifferentRow);
int sqlite3BtreeCursorHasMovedOriginal(BtCursor *pCur);
int sqlite3BtreeCursorHasMovedAll(BtCursor *pCur);
/* For Blob Write feature */
int sqlite3BtreePutDataAll(BtCursor*, u32 offset, u32 amt, void*);
int sqlite3BtreePutDataOriginal(BtCursor*, u32 offset, u32 amt, void*);
#ifndef NDEBUG
int sqlite3BtreeCursorIsValidOriginal(BtCursor *pCur);
int sqlite3BtreeCursorIsValidAll(BtCursor *pCur);
#endif
#ifndef SQLITE_OMIT_WINDOWFUNC
void sqlite3BtreeSkipNextOriginal(BtCursor *pCur);
void sqlite3BtreeSkipNextAll(BtCursor *pCur);
#endif /* SQLITE_OMIT_WINDOWFUNC */
int sqlite3BtreeMovetoUnpackedAll(BtCursor *pCur, UnpackedRecord *pIdxKey, i64 intKey, int biasRight, int *pRes, int opcode);
int sqlite3BtreeNoConflict(BtCursor *pCur, UnpackedRecord *pIdxKey, int file_format, int *pRes);


int btreeMovetoOriginal(BtCursor *pCur, const void *pKey, i64 nKey, int bias, int *pRes);
int saveCursorPositionOriginal(BtCursor *pCur);
int restoreCursorPositionOriginal(BtCursor *pCur);
void getCellInfoOriginal(BtCursor *pCur);
BtShared *sharedCacheListGet(void);
int querySharedCacheTableLockOriginal(Btree *p, Pgno iTab, u8 eLock);

void sqlite3SetForceCommit(Vdbe *p);
int rowlockBtreeCacheReset(Btree *p);
int sqlite3rowlockExclusiveLockAllTables(Btree *p);

/* rowlock_btree.c */
int hasSharedCacheTableLock(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType);

/* rowlock_pager.c */
int rowlockPagerCheckSchemaVers(Pager *pPager, int version, u8 *needReset);
int rowlockPagerCheckDbFileVers(Pager *pPager, u8 *needReset);
int rowlockPagerCacheReset(Pager *pPager);
int rowlockPagerReloadDbPage(PgHdr *pPg, Pager *pPager);
int rowlockPagerExclusiveLock(Pager *pPager);
int rowlockPagerCheckLockAvailable(Pager *pPager, u8 lockType);

/* rowlock_savepoint.c */
void sqlite3CloseSavepointsOriginal(sqlite3 *db);
void sqlite3CloseSavepointsAll(sqlite3 *db);

/* blob handle */
void sqlite3BtreeIncrblobCursorAll(BtCursor *pCur);
void sqlite3BtreeIncrblobCursorOriginal(BtCursor *pCur);
int sqlite3BtreePayloadCheckedAll(BtCursor*, u32 offset, u32 amt, void*);
int sqlite3BtreePayloadCheckedOriginal(BtCursor*, u32 offset, u32 amt, void*);

int saveAllCursorsOriginal(BtShared *pBt, Pgno iRoot, BtCursor *pExcept);
void invalidateIncrblobCursorsOriginal(Btree *pBtree, Pgno pgnoRoot, i64 iRow, int isClearTable);

#endif /* SQLITE_ROWLOCK_H */
#endif /* SQLITE_OMIT_ROWLOCK */