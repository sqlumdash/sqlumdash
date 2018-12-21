/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the implementation of new functions in main.c for
** row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if defined(SQLUMDASH_INCLUDED_FROM_MAIN_C) || defined(SQLITE_AMALGAMATION)

void sqlite3CloseSavepoints(sqlite3 *db){
  sqlite3CloseSavepointsAll(db);
}
#endif
#endif
