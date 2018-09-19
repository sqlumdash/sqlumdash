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

static int assert_pager_state_original(Pager *p);
static int pager_write_pagelist_original(Pager *pPager, PgHdr *pList);
static void pager_reset(Pager *pPager);
static int readDbPage(PgHdr *pPg);

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

int rowlockPagerCacheReset(Pager *pPager, int *reset){
  int rc = SQLITE_OK;
  char dbFileVers[sizeof(pPager->dbFileVers)];

  if( pPager->fd->pMethods==NULL ){
    *reset = 0;
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
    pager_reset(pPager);
    *reset = 1;
    if( USEFETCH(pPager) ) sqlite3OsUnfetch(pPager->fd, 0, 0);
  }else{
    *reset = 0;
  }
  
failed:
  return rc;
}

int rowlockPagerReloadDbPage(PgHdr *pPg, Pager *pPager){
  int rc = readDbPage(pPg);
  if( rc==SQLITE_OK ){
    pPager->xReiniter(pPg);
  }
  return rc;
}

#endif