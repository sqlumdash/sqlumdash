/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** The row lock cleanup tool.
*/
#include "rowlock.h"

int sqlite3CantopenError(int lineno){
  return SQLITE_CANTOPEN;
}

int sqlite3NomemError(int lineno){
  return SQLITE_NOMEM;
}

int main(){
  sqlite3_rowlock_ipc_unlock_record_all();
  sqlite3_rowlock_ipc_unlock_tables_all();
  return 0;
}
