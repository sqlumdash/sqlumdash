/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the implementation of new functions in vtab.c for
** row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if defined(SQLUMDASH_INCLUDED_FROM_VTAB_C) || defined(SQLITE_AMALGAMATION)

/*
** Set inOpTrans inVtabSavepoint in order to avoid to be called
** sqlite3VtabSavepoint() recursively because the function might
** execute SQL and be called in OP_Transaction and OP_Savepoint
** again infinitely.
*/
int sqlite3VtabSavepoint(sqlite3 *db, int op, int iSavepoint){
  int rc = SQLITE_OK;

  db->inVtabSavepoint++;
  rc = sqlite3VtabSavepointOriginal(db, op, iSavepoint);
  db->inVtabSavepoint--;

  return rc;
}
#endif
#endif
