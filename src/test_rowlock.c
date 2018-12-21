/*
** 2018 October 23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing rowlock additional feature of SQLumDash.  This code
** is not in scope of Sqlite, for testing purpose only.
*/

#include "sqliteInt.h"
#if SQLITE_OS_WIN
#  include "os_win.h"
#endif

#include "vdbeInt.h"
#if defined(INCLUDE_SQLITE_TCL_H)
#  include "sqlite_tcl.h"
#else
#  include "tcl.h"
#endif
#include <stdlib.h>
#include <string.h>

/*
** tclcmd:   sqlite3_rowlock_config_mmap SETTING VALUE
**
** Invoke sqlite3_config_mmap() for one of the setting values.
**  CONFIG_ROWLOCK_MMAP_SIZE
**  CONFIG_TABLELOCK_MMAP_SIZE
*/
static int SQLITE_TCLAPI test_sqlite3_config_mmap(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static const struct {
    const char *zName;
    int eVal;
  } aSetting[] = {
    { "CONFIG_ROWLOCK_MMAP_SIZE",           SQLITE_CONFIG_MMAP_ROW_SIZE },
    { "CONFIG_TABLELOCK_MMAP_SIZE",         SQLITE_CONFIG_MMAP_TABLE_SIZE },
  };
  int i;
  int v;
  int rc = 0;
  const char *zSetting;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB ROWLOCK MMAP SETTING VALUE");
    return TCL_ERROR;
  }

  zSetting = Tcl_GetString(objv[1]);
  for(i=0; i<ArraySize(aSetting); i++){
    if( strcmp(zSetting, aSetting[i].zName)==0 ) break;
  }
  if( i>=ArraySize(aSetting) ){
    Tcl_SetObjResult(interp,
      Tcl_NewStringObj("unknown sqlite3_config setting", -1));
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &v) ) return TCL_ERROR;
  rc = sqlite3_config(aSetting[i].eVal, v, &v);
  if (rc != SQLITE_OK) {
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(v));
  return TCL_OK;
}

int Sqlitetest_rowlock_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "sqlite3_rowlock_config_mmap", test_sqlite3_config_mmap, 0, 0);
  return TCL_OK;
}