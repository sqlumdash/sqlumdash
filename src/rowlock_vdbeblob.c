/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains functions about blob handle for row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
/*
** This file needs to be included into vdbeblob.c because Incrblob
** structure is defined that file.
*/
#if defined(SQLUMDASH_INCLUDED_FROM_VDBEBLOB_C) || defined(SQLITE_AMALGAMATION)

/* Return VDBE pointer of sqlite3_blob variable. */
Vdbe *rowlockIncrblobVdbe(sqlite3_blob *pBlob){
  return (Vdbe *)((Incrblob*)pBlob)->pStmt;
}

/* Return db handle of sqlite3_blob variable. */
sqlite3 *rowlockIncrblobDb(sqlite3_blob *pBlob){
  return ((Incrblob*)pBlob)->db;
}

/* Return a database name of sqlite3_blob variable. */
const char *rowlockIncrblobDbName(sqlite3_blob *pBlob){
  return ((Incrblob*)pBlob)->zDb;
}

/* Return a table name of sqlite3_blob variable. */
Table *rowlockIncrblobTable(sqlite3_blob *pBlob){
  return ((Incrblob*)pBlob)->pTab;
}

/* Return a column name of sqlite3_blob variable. */
u16 rowlockIncrblobColumnNumber(sqlite3_blob *pBlob){
  return ((Incrblob*)pBlob)->iCol;
}

/* Return a rowid of sqlite3_blob variable. */
sqlite_int64 rowlockIncrblobRowNumber(sqlite3_blob *pBlob){
  Vdbe *v = (Vdbe *)((Incrblob*)pBlob)->pStmt;
  return v->aMem[1].u.i;
}

/* Return wrFlag of sqlite3_blob variable. */
int rowlockIncrblobFlag(sqlite3_blob *pBlob){
  Vdbe *v = (Vdbe *)((Incrblob*)pBlob)->pStmt;
  VdbeOp *aOp = v->aOp;
  /* wrFlag is stored in P3 of OP_Transaction. */
  return aOp[0].p3;
}

/* Allocate memory of sqlite3_blob structure. */
sqlite3_blob *rowlockIncrblobMalloc(sqlite3 *db){
  Incrblob *pNew = (Incrblob*)sqlite3DbMallocRaw(db, sizeof(Incrblob));
  return (sqlite3_blob*)pNew;
}

/* Sharow copy of sqlite3_blob structure. */
void rowlockIncrblobCopy(sqlite3_blob *src, sqlite3_blob *dest){
  memcpy(dest, src, sizeof(Incrblob));
}

/* Set NULL of Incrblob->pStmt. */
void rowlockIncrblobStmtNull(sqlite3_blob *pBlob){
  Incrblob *p = (Incrblob*)pBlob;
  p->pStmt = NULL;
}
#endif /* SQLUMDASH_INCLUDED_FROM_VDBEBLOB_C || SQLITE_AMALGAMATION */
#endif /* SQLITE_OMIT_ROWLOCK */
