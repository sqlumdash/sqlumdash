/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the implementation of new btree function for row 
** lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if defined(SQLUMDASH_INCLUDED_FROM_BTREE_C) || defined(SQLITE_AMALGAMATION)

#ifdef SQLITE_DEBUG
int hasSharedCacheTableLock(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType){
  return hasSharedCacheTableLockAll(pBtree, iRoot, isIndex, eLockType);
}
#endif
int sqlite3BtreeCursorHasMoved(BtCursor *pCur){
  return sqlite3BtreeCursorHasMovedAll(pCur);
}
int sqlite3BtreeCursorRestore(BtCursor *pCur, int *pDifferentRow){
  return sqlite3BtreeCursorRestoreAll(pCur, pDifferentRow);
}
int sqlite3BtreeOpen(sqlite3_vfs *pVfs, const char *zFilename, sqlite3 *db, Btree **ppBtree, int flags, int vfsFlags){
  return sqlite3BtreeOpenAll(pVfs, zFilename, db, ppBtree, flags, vfsFlags);
}
int sqlite3BtreeClose(Btree *p){
  return sqlite3BtreeCloseAll(p);
}
int sqlite3BtreeBeginTrans(Btree *p, int wrflag, int *pSchemaVersion){
  return sqlite3BtreeBeginTransAll(p, wrflag, pSchemaVersion);
}
int sqlite3BtreeRollback(Btree *p, int tripCode, int writeOnly){
  return sqlite3BtreeRollbackAll(p, tripCode, writeOnly);
}
int sqlite3BtreeBeginStmt(Btree *p, int iStatement){
  return sqlite3BtreeBeginStmtAll(p, iStatement);
}
int sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint){
  return sqlite3TransBtreeSavepoint(p, op, iSavepoint);
}
int sqlite3BtreeCursor(Btree *p, int iTable, int wrFlag, struct KeyInfo *pKeyInfo, BtCursor *pCur){
  return sqlite3BtreeCursorAll(p, iTable, wrFlag, pKeyInfo, pCur, 0);
}
int sqlite3BtreeCloseCursor(BtCursor *pCur){
  return sqlite3BtreeCloseCursorAll(pCur);
}
#ifndef NDEBUG
int sqlite3BtreeCursorIsValid(BtCursor *pCur){
  return sqlite3BtreeCursorIsValidAll(pCur);
}
#endif

#ifndef SQLITE_OMIT_WINDOWFUNC
void sqlite3BtreeSkipNext(BtCursor *pCur){
  sqlite3BtreeSkipNextAll(pCur);
}
#endif /* SQLITE_OMIT_WINDOWFUNC */

i64 sqlite3BtreeIntegerKey(BtCursor *pCur){
  return sqlite3BtreeIntegerKeyAll(pCur);
}
u32 sqlite3BtreePayloadSize(BtCursor *pCur){
  return sqlite3BtreePayloadSizeAll(pCur);
}
int sqlite3BtreePayload(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  return sqlite3BtreePayloadAll(pCur, offset, amt, pBuf);
}
const void *sqlite3BtreePayloadFetch(BtCursor *pCur, u32 *pAmt){
  return sqlite3BtreePayloadFetchAll(pCur, pAmt);
}
int sqlite3BtreeCreateTable(Btree *p, int *piTable, int flags){
  return sqlite3BtreeCreateTableWithTransOpen(p, piTable, flags);
}
int sqlite3BtreeDropTable(Btree *p, int iTable, int *piMoved){
  return sqlite3BtreeDropTableAll(p, iTable, piMoved);
}
int sqlite3BtreeUpdateMeta(Btree *p, int idx, u32 iMeta){
  return sqlite3BtreeUpdateMetaWithTransOpen(p, idx, iMeta);
}
int sqlite3BtreeIsInTrans(Btree *p){
  return sqlite3BtreeIsInTransAll(p);
}
int sqlite3BtreeLockTable(Btree *p, int iTab, u8 isWriteLock){
  return sqlite3BtreeLockTableForRowLock(p, iTab, isWriteLock);
}
int sqlite3BtreeSetVersion(Btree *pBtree, int iVersion){
  return sqlite3BtreeSetVersionWithTransOpen(pBtree, iVersion);
}
int sqlite3BtreeIncrVacuum(Btree *p){
  return sqlite3BtreeIncrVacuumForRowLock(p);
}

/* The following functions enable to use SQLite's original static functions from rowlock.c. */
#ifdef SQLITE_DEBUG
int hasSharedCacheTableLockOriginal(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType){
  return hasSharedCacheTableLockStatic(pBtree, iRoot, isIndex, eLockType);
}
#endif

int btreeMovetoOriginal(BtCursor *pCur, const void *pKey, i64 nKey, int bias, int *pRes){
  return btreeMoveto(pCur, pKey, nKey, bias, pRes);
}

int saveCursorPositionOriginal(BtCursor *pCur){
  return saveCursorPosition(pCur);
}

int restoreCursorPositionOriginal(BtCursor *pCur){
  return restoreCursorPosition(pCur);
}

void getCellInfoOriginal(BtCursor *pCur){
  getCellInfo(pCur);
}

BtShared *sharedCacheListGet(void){
  return GLOBAL(BtShared*,sqlite3SharedCacheList);
}

int querySharedCacheTableLockOriginal(Btree *p, Pgno iTab, u8 eLock){
  return querySharedCacheTableLock(p, iTab, eLock);
}
#endif
#endif