/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** The row lock cleanup tool.
*/
#include <stdio.h>
#include "rowlock.h"

int sqlite3CantopenError(int lineno){
  return SQLITE_CANTOPEN;
}

int sqlite3NomemError(int lineno){
  return SQLITE_NOMEM;
}

int main(int argc, char *argv[]){
  char *dbname;

  if( argc!=2 ){
    fprintf(stderr, "Usage: %s dbname\n", argv[0]);
    return 1;
  }
  dbname = argv[1];
  sqlite3_rowlock_ipc_unlock_record_all(dbname);
  sqlite3_rowlock_ipc_unlock_tables_all(dbname);
  return 0;
}
