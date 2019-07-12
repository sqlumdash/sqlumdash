/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains pager functions for row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
/* This file needs to be included into pager.c because Pager structure is defined that file. */
#if defined(SQLUMDASH_INCLUDED_FROM_PAGER_C) || defined(SQLITE_AMALGAMATION)

static int assert_pager_state_original(Pager *p);
static int pager_write_pagelist_original(Pager *pPager, PgHdr *pList);
static void pager_reset(Pager *pPager);
static int readDbPage(PgHdr *pPg);
static int pagerBeginReadTransaction(Pager *pPager);
static int pagerPagecount(Pager *pPager, Pgno *pnPage);
static int pagerLockDb(Pager *pPager, int eLock);

#ifndef NDEBUG 
static int assert_pager_state(Pager *p){
  int rc;
  Pager *pPager = p;
  u8 eLockOrig = pPager->eLock;
  if( !(pPager->changeCountDone==0 || pPager->eLock>=RESERVED_LOCK) ){
    pPager->eLock = RESERVED_LOCK;
  }
  switch( p->eState ){
    case PAGER_WRITER_LOCKED:
    case PAGER_WRITER_CACHEMOD:
      if( !(p->eLock>=RESERVED_LOCK) ) pPager->eLock = RESERVED_LOCK;
      break;
    case PAGER_WRITER_DBMOD:
    case PAGER_WRITER_FINISHED:
      if( !(p->eLock==EXCLUSIVE_LOCK) ) pPager->eLock = EXCLUSIVE_LOCK;
      break;
  }
  rc = assert_pager_state_original(p);
  pPager->eLock = eLockOrig;
  return rc;
}
#endif

static int pager_write_pagelist(Pager *pPager, PgHdr *pList){
  int rc;
  u8 eLockOrig = pPager->eLock;
  pPager->eLock = EXCLUSIVE_LOCK;
  rc = pager_write_pagelist_original(pPager, pList);
  pPager->eLock = eLockOrig;
  return rc;
}

int rowlockPagerCheckSchemaVers(Pager *pPager, int version, u8 *needReset){
  int rc;
  int schemaVersion;
  char scVers[sizeof(schemaVersion)];

  /* No need to check if it is memory database. */
  if( pPager->memDb ){
    needReset = 0;
    return SQLITE_OK;
  }

  IOTRACE(("CKSCVERS %p %d\n", pPager, sizeof(scVers)));
  rc = sqlite3OsRead(pPager->fd, &scVers, sizeof(scVers), 36 + BTREE_SCHEMA_VERSION*4);
  if( rc==SQLITE_OK ){
    schemaVersion = sqlite3Get4byte((const u8*)scVers);
  }else{
    if( rc!=SQLITE_IOERR_SHORT_READ ){
      goto failed;
    }
    rc = SQLITE_OK;
    schemaVersion = 0;
  }

  if( schemaVersion!=version && schemaVersion!=0 && version!=0 ){
    *needReset = 1;
  }else{
    *needReset = 0;
  }

failed:
  return rc;
}

int rowlockPagerCheckDbFileVers(Pager *pPager, u8 *needReset){
  int rc = SQLITE_OK;
  char dbFileVers[sizeof(pPager->dbFileVers)];

  if( pPager->fd->pMethods==NULL ){
    *needReset = 0;
    return SQLITE_OK;
  }

  IOTRACE(("CKVERS %p %d\n", pPager, sizeof(dbFileVers)));
  rc = sqlite3OsRead(pPager->fd, &dbFileVers, sizeof(dbFileVers), 24);
  if( rc!=SQLITE_OK ){
    if( rc!=SQLITE_IOERR_SHORT_READ ){
      goto failed;
    }
    rc = SQLITE_OK;
    memset(dbFileVers, 0, sizeof(dbFileVers));
  }
  
  if( memcmp(pPager->dbFileVers, dbFileVers, sizeof(dbFileVers))!=0 ){
    *needReset = 1;
  }else{
    *needReset = 0;
  }

failed:
  return rc;
}

int rowlockPagerCacheReset(Pager *pPager){
  int rc = SQLITE_OK;

  pager_reset(pPager);
  if( USEFETCH(pPager) ) sqlite3OsUnfetch(pPager->fd, 0, 0);

  /* Update dbSize variable. */
  if( pPager->tempFile==0 && pPager->eLock!=NO_LOCK){
    u8 eState;
    /*
    ** We call pagerBeginReadTransaction() in order to be pWal->readLock>=0
    ** which sqlite3WalDbsize() called by pagerPagecount() requires.
    */
    if( pagerUseWal(pPager) ){
      rc = pagerBeginReadTransaction(pPager);
      if( rc ) return rc;
    }
    /*
    ** pagerPagecount() requires PAGER_OPEN. So change state forcibly and
    ** revert to the original state after the function.
    */
    eState = pPager->eState;
    pPager->eState = PAGER_OPEN;
    rc = pagerPagecount(pPager, &pPager->dbSize);
    pPager->eState = eState;
  }
  return rc;
}

int rowlockPagerReloadDbPage(PgHdr *pPg, Pager *pPager){
  int rc = readDbPage(pPg);
  if( rc==SQLITE_OK ){
    pPager->xReiniter(pPg);
  }
  return rc;
}

int rowlockPagerExclusiveLock(Pager *pPager){
  int rc = SQLITE_OK;
  do {
    rc = pagerLockDb(pPager, EXCLUSIVE_LOCK);
  }while( rc==SQLITE_BUSY && pPager->xBusyHandler(pPager->pBusyHandlerArg) );
  return rc;
}
#endif /* SQLUMDASH_INCLUDED_FROM_PAGER_C || SQLITE_AMALGAMATION */
#endif /* SQLITE_OMIT_ROWLOCK */
