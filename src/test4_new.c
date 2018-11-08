/*
** 2018 September 21
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the SQLite library in a multithreaded environment.
** Supported Win32 with serialize model
*/
#include "sqliteInt.h"
#if defined(INCLUDE_SQLITE_TCL_H)
#  include "sqlite_tcl.h"
#else
#  include "tcl.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if SQLITE_THREADSAFE
#define strlens(s) (s == NULL?0 : strlen(s))
#if SQLITE_OS_UNIX
#include <pthread.h>
#include <sched.h>
#define sw_sched_yield sched_yield
#define thread_handle pthread_t
#define detach_thread pthread_detach
#elif SQLITE_OS_WIN
#include <Windows.h>
#include <stdarg.h>
#define sw_sched_yield SwitchToThread
#define thread_handle HANDLE
#define detach_thread CloseHandle
#endif

extern const char *sqlite3ErrName(int);

/*
** Each thread is controlled by an instance of the following
** structure.
*/
typedef struct Thread Thread;
struct Thread {
  /* The first group of fields are writable by the master and read-only
  ** to the thread. */
  char *zFilename;       /* Name of database file */
  void (*xOp)(Thread*);  /* next operation to do */
  char *zArg;            /* argument usable by xOp */
  int opnum;             /* Operation number */
  int busy;              /* True if this thread is in use */

  /* The next group of fields are writable by the thread but read-only to the
  ** master. */
  int completed;        /* Number of operations completed */
  sqlite3 *db;           /* Open database */
  sqlite3_stmt *pStmt;     /* Pending operation */
  char *zErr;           /* operation error */
  char *zStaticErr;     /* Static error message */
  int rc;               /* operation return code */
  int argc;             /* number of columns in result */
  const char *argv[100];    /* result columns */
  const char *colv[100];    /* result column names */

  char *resultset;          /* Contain result follow format */
};

/*
** There can be as many as 26 threads running at once.  Each is named
** by a capital letter: A, B, C, ..., Y, Z.
*/
#define N_THREAD 26
static Thread threadset[N_THREAD];

/*
** Constants for concatenating string follow format
*/
const char space[2] = " ";
const char underline[2] = "_";
const char zero[2] = "0";
const char one[2] = "1";

/*
** The main loop for a thread.  Threads use busy waiting. 
*/
static void *thread_main(void *pArg){
  Thread *p = (Thread*)pArg;
  if( p->db ){
    sqlite3_close(p->db);
  }
  sqlite3_open(p->zFilename, &p->db);
  if( SQLITE_OK!=sqlite3_errcode(p->db) ){
    p->zErr = strdup(sqlite3_errmsg(p->db));
    sqlite3_close(p->db);
    p->db = 0;
  }
  p->pStmt = 0;
  p->completed = 1;

  while( p->opnum<=p->completed ) sw_sched_yield();
  while( p->xOp ){
    if( p->zErr && p->zErr!=p->zStaticErr ){
      sqlite3_free(p->zErr);
      p->zErr = 0;
    }
    (*p->xOp)(p);
    p->completed++;
    while( p->opnum<=p->completed ) sw_sched_yield();
  }
  if( p->pStmt ){
    sqlite3_finalize(p->pStmt);
    p->pStmt = 0;
  }
  if( p->db ){
    sqlite3_close(p->db);
    p->db = 0;
  }
  if( p->zErr && p->zErr!=p->zStaticErr ){
    sqlite3_free(p->zErr);
    p->zErr = 0;
  }
  p->completed++;
#ifndef SQLITE_OMIT_DEPRECATED
  sqlite3_thread_cleanup();
#endif
  return 0;
}

/*
** Get a thread ID which is an upper case letter.  Return the index.
** If the argument is not a valid thread ID put an error message in
** the interpreter and return -1.
*/
static int parse_thread_id(Tcl_Interp *interp, const char *zArg){
  if( zArg==0 || zArg[0]==0 || zArg[1]!=0 || !isupper((unsigned char)zArg[0]) ){
    Tcl_AppendResult(interp, "thread ID must be an upper case letter", 0);
    return -1;
  }
  return zArg[0] - 'A';
}

/*
** Usage:    thread_create NAME  FILENAME
**
** NAME should be an upper case letter.  Start the thread running with
** an open connection to the given database.
*/
static int SQLITE_TCLAPI tcl_thread_create(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  thread_handle x;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID FILENAME", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( threadset[i].busy ){
    Tcl_AppendResult(interp, "thread ", argv[1], " is already running", 0);
    return TCL_ERROR;
  }
  threadset[i].busy = 1;
  sqlite3_free(threadset[i].zFilename);
  threadset[i].zFilename = sqlite3_mprintf("%s", argv[2]);
  threadset[i].opnum = 1;
  threadset[i].completed = 0;
#if SQLITE_OS_WIN
  x = CreateThread( NULL, 0,(LPTHREAD_START_ROUTINE) thread_main , &threadset[i],0,NULL);
  if( x == NULL ){
    Tcl_AppendResult(interp, "failed to create the thread", 0);
    sqlite3_free(threadset[i].zFilename);
    threadset[i].busy = 0;
    return TCL_ERROR;
  }
#elif SQLITE_OS_UNIX
  int rc;
  rc = pthread_create( &x , NULL , (void *) thread_main ,&threadset[i]);
  if( rc ){
    Tcl_AppendResult(interp, "failed to create the thread", 0);
    sqlite3_free(threadset[i].zFilename);
    threadset[i].busy = 0;
    return TCL_ERROR;
  }
#endif
  
  detach_thread(x);
  return TCL_OK;
}

/*
** Wait for a thread to reach its idle state.
*/
static void thread_wait(Thread *p){
  while( p->opnum>p->completed ) sw_sched_yield();
}

/*
** Usage:  thread_wait ID
**
** Wait on thread ID to reach its idle state.
*/
static int SQLITE_TCLAPI tcl_thread_wait(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  return TCL_OK;
}

/*
** Stop a thread.
*/
static void stop_thread(Thread *p){
  thread_wait(p);
  p->xOp = 0;
  p->opnum++;
  thread_wait(p);
  sqlite3_free(p->zArg);
  p->zArg = 0;
  sqlite3_free(p->zFilename);
  p->zFilename = 0;
  p->busy = 0;
}

/*
** Usage:  thread_halt ID
**
** Cause a thread to shut itself down.  Wait for the shutdown to be
** completed.  If ID is "*" then stop all threads.
*/
static int SQLITE_TCLAPI tcl_thread_halt(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  if( argv[1][0]=='*' && argv[1][1]==0 ){
    for(i=0; i<N_THREAD; i++){
      if( threadset[i].busy ) stop_thread(&threadset[i]);
    }
  }else{
    i = parse_thread_id(interp, argv[1]);
    if( i<0 ) return TCL_ERROR;
    if( !threadset[i].busy ){
      Tcl_AppendResult(interp, "no such thread", 0);
      return TCL_ERROR;
    }
    stop_thread(&threadset[i]);
  }
  return TCL_OK;
}

/*
** Usage: thread_argc  ID
**
** Wait on the most recent thread_step to complete, then return the
** number of columns in the result set.
*/
static int SQLITE_TCLAPI tcl_thread_argc(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  char zBuf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  sqlite3_snprintf(sizeof(zBuf), zBuf, "%d", threadset[i].argc);
  Tcl_AppendResult(interp, zBuf, 0);
  return TCL_OK;
}

/*
** Usage: thread_argv  ID   N
**
** Wait on the most recent thread_step to complete, then return the
** value of the N-th columns in the result set.
*/
static int SQLITE_TCLAPI tcl_thread_argv(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  int n;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID N", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n) ) return TCL_ERROR;
  thread_wait(&threadset[i]);
  if( n<0 || n>=threadset[i].argc ){
    Tcl_AppendResult(interp, "column number out of range", 0);
    return TCL_ERROR;
  }
  Tcl_AppendResult(interp, threadset[i].argv[n], 0);
  return TCL_OK;
}

/*
** Usage: thread_colname  ID   N
**
** Wait on the most recent thread_step to complete, then return the
** name of the N-th columns in the result set.
*/
static int SQLITE_TCLAPI tcl_thread_colname(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  int n;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID N", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n) ) return TCL_ERROR;
  thread_wait(&threadset[i]);
  if( n<0 || n>=threadset[i].argc ){
    Tcl_AppendResult(interp, "column number out of range", 0);
    return TCL_ERROR;
  }
  Tcl_AppendResult(interp, threadset[i].colv[n], 0);
  return TCL_OK;
}

/*
** Usage: thread_result  ID
**
** Wait on the most recent operation to complete, then return the
** result code from that operation.
*/
static int SQLITE_TCLAPI tcl_thread_result(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  const char *zName;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  zName = sqlite3ErrName(threadset[i].rc);
  Tcl_AppendResult(interp, zName, 0);
  return TCL_OK;
}

/*
** Usage: thread_error  ID
**
** Wait on the most recent operation to complete, then return the
** error string.
*/
static int SQLITE_TCLAPI tcl_thread_error(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  Tcl_AppendResult(interp, threadset[i].zErr, 0);
  return TCL_OK;
}

/*
** This procedure runs in the thread to compile an SQL statement.
*/
static void do_compile(Thread *p){
  if( p->db==0 ){
    p->zErr = p->zStaticErr = "no database is open";
    p->rc = SQLITE_ERROR;
    return;
  }
  if( p->pStmt ){
    sqlite3_finalize(p->pStmt);
    p->pStmt = 0;
  }
  p->rc = sqlite3_prepare(p->db, p->zArg, -1, &p->pStmt, 0);
}

/*
** Usage: thread_compile ID SQL
**
** Compile a new virtual machine.
*/
static int SQLITE_TCLAPI tcl_thread_compile(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID SQL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_compile;
  sqlite3_free(threadset[i].zArg);
  threadset[i].zArg = sqlite3_mprintf("%s", argv[2]);
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** This procedure runs in the thread to step the virtual machine.
*/
static void do_step(Thread *p){
  int i;
  if( p->pStmt==0 ){
    p->zErr = p->zStaticErr = "no virtual machine available";
    p->rc = SQLITE_ERROR;
    return;
  }
  p->rc = sqlite3_step(p->pStmt);
  if( p->rc==SQLITE_ROW ){
    p->argc = sqlite3_column_count(p->pStmt);
    for(i=0; i<sqlite3_data_count(p->pStmt); i++){
      p->argv[i] = (char*)sqlite3_column_text(p->pStmt, i);
    }
    for(i=0; i<p->argc; i++){
      p->colv[i] = sqlite3_column_name(p->pStmt, i);
    }
  }
}

/*
** Usage: thread_step ID
**
** Advance the virtual machine by one step
*/
static int SQLITE_TCLAPI tcl_thread_step(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " IDL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_step;
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** This procedure runs in the thread to finalize a virtual machine.
*/
static void do_finalize(Thread *p){
  if( p->pStmt==0 ){
    p->zErr = p->zStaticErr = "no virtual machine available";
    p->rc = SQLITE_ERROR;
    return;
  }
  p->rc = sqlite3_finalize(p->pStmt);
  p->pStmt = 0;
}

/*
** Usage: thread_finalize ID
**
** Finalize the virtual machine.
*/
static int SQLITE_TCLAPI tcl_thread_finalize(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " IDL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_finalize;
  sqlite3_free(threadset[i].zArg);
  threadset[i].zArg = 0;
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** Usage: thread_swap ID ID
**
** Interchange the sqlite* pointer between two threads.
*/
static int SQLITE_TCLAPI tcl_thread_swap(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i, j;
  sqlite3 *temp;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID1 ID2", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  j = parse_thread_id(interp, argv[2]);
  if( j<0 ) return TCL_ERROR;
  if( !threadset[j].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[j]);
  temp = threadset[i].db;
  threadset[i].db = threadset[j].db;
  threadset[j].db = temp;
  return TCL_OK;
}

/*
** Usage: thread_db_get ID
**
** Return the database connection pointer for the given thread.  Then
** remove the pointer from the thread itself.  Afterwards, the thread
** can be stopped and the connection can be used by the main thread.
*/
static int SQLITE_TCLAPI tcl_thread_db_get(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  char zBuf[100];
  extern int sqlite3TestMakePointerStr(Tcl_Interp*, char*, void*);
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  sqlite3TestMakePointerStr(interp, zBuf, threadset[i].db);
  threadset[i].db = 0;
  Tcl_AppendResult(interp, zBuf, (char*)0);
  return TCL_OK;
}

/*
** Usage: thread_db_put ID DB
**
*/
static int SQLITE_TCLAPI tcl_thread_db_put(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  extern int sqlite3TestMakePointerStr(Tcl_Interp*, char*, void*);
  extern void *sqlite3TestTextToPtr(const char *);
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID DB", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  assert( !threadset[i].db );
  threadset[i].db = (sqlite3*)sqlite3TestTextToPtr(argv[2]);
  return TCL_OK;
}

/*
** Usage: thread_stmt_get ID
**
** Return the database stmt pointer for the given thread.  Then
** remove the pointer from the thread itself. 
*/
static int SQLITE_TCLAPI tcl_thread_stmt_get(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
  ){
  int i;
  char zBuf[100];
  extern int sqlite3TestMakePointerStr(Tcl_Interp*, char*, void*);
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  sqlite3TestMakePointerStr(interp, zBuf, threadset[i].pStmt);
  threadset[i].pStmt = 0;
  Tcl_AppendResult(interp, zBuf, (char*)0);
  return TCL_OK;
}

/*
** Concatenate variable number of string
** and return the pointer of the destination string
*/
static char* sqlite3_concatenate_string(int count, ...) {
  va_list arguments;
  int i = 0;
  size_t size_result = 0;
  size_t size_ext = 0;
  char* result_tmp = NULL;
  char* arg_tmp = NULL;
  char* ptr_tmp = NULL;

  va_start(arguments, count);

  for (i = 0; i < count; i++) {
    arg_tmp = va_arg(arguments, char*);

    if (arg_tmp != NULL) {
      size_result =  strlens(result_tmp);
      size_ext = strlens(arg_tmp);
      ptr_tmp = result_tmp;
      result_tmp = sqlite3_realloc(result_tmp, size_result + size_ext + 1);
      if (result_tmp == NULL) {
        printf("REALLOC FAILED\n");
        sqlite3_free(ptr_tmp);
        return NULL;
      }
      memset((char*)result_tmp + size_result, 0, size_ext + 1);
      result_tmp = strcat(result_tmp, arg_tmp);
    }
  }
  va_end(arguments);
  return result_tmp;
}

/*
** Concatenate strings in argv[] follow format save it to resultset
*/
static int callback_function(void** result, int argc, char **argv, char** colv){
  int i = 0;

  char* resultset = (char*)(*result);
  char* ptr_tmp = NULL;
  for(i = 0; i<argc; i++) {
    if (resultset == NULL) {  
      resultset = sqlite3_concatenate_string(1, argv[i]);
      if (resultset == NULL) {
        printf("THE FIRST CONCATENATE FAILED\n");
        return -1;
      }
    } else {
      ptr_tmp = resultset;
      resultset = sqlite3_concatenate_string(3, resultset, space, argv[i]);
      sqlite3_free(ptr_tmp);
      if (resultset == NULL){
        printf("THE CONCATENATE NUMBER %d FAILED\n", i);
        return -1;
      }
    }
  }
  *result = resultset;
  return 0;
}

/*
** This procedure runs in the thread to execute an SQL statement.
** Including compile, step and finalize
*/
static void do_exec(Thread *p) {
  char* ptr_tmp = NULL;
  
  if (p->resultset != NULL) {
    sqlite3_free (p->resultset);
    p->resultset = NULL;
  }
  
  if( p->db==0 ) {
    p->zErr = p->zStaticErr = "no database is open";
    p->rc = SQLITE_ERROR;
    return;
  }
  p->rc = sqlite3_exec(p->db, p->zArg, &callback_function, (void*)&p->resultset, &p->zErr);
  ptr_tmp = p->resultset;

  if (p->rc == 0){
    p->resultset = sqlite3_concatenate_string(3, zero, underline, p->resultset);
  } else {
    p->resultset = sqlite3_concatenate_string(3, one, underline, p->zErr);
  }
  sqlite3_free(ptr_tmp);
  return;
}

/*
** Usage: thread_exec ID SQL
**
** Used instead of thread_finalize, thread_compile, thread_step,
** thread_result
** Compile and execute the SQL statement on specific thread,
** save the result to the thread struct
*/

static int SQLITE_TCLAPI tcl_thread_exec(
  void *NotUsed,
  Tcl_Interp *interp,   /* The TCL interpreter that invoked this command */
  int argc,             /* Number of arguments */
  const char **argv     /* Text of each argument */
  ){
  int i;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
     " ID SQL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_exec;
  sqlite3_free(threadset[i].zArg);
  threadset[i].zArg = sqlite3_mprintf("%s", argv[2]);
  threadset[i].opnum++;
  thread_wait(&threadset[i]);

  Tcl_AppendResult(interp, threadset[i].resultset, 0);
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest4_new_Init(Tcl_Interp *interp){
  static struct {
   char *zName;
   Tcl_CmdProc *xProc;
 } aCmd[] = {
   { "thread_create",     (Tcl_CmdProc*)tcl_thread_create     },
   { "thread_wait",       (Tcl_CmdProc*)tcl_thread_wait       },
   { "thread_halt",       (Tcl_CmdProc*)tcl_thread_halt       },
   { "thread_argc",       (Tcl_CmdProc*)tcl_thread_argc       },
   { "thread_argv",       (Tcl_CmdProc*)tcl_thread_argv       },
   { "thread_colname",    (Tcl_CmdProc*)tcl_thread_colname    },
   { "thread_result",     (Tcl_CmdProc*)tcl_thread_result     },
   { "thread_error",      (Tcl_CmdProc*)tcl_thread_error      },
   { "thread_compile",    (Tcl_CmdProc*)tcl_thread_compile    },
   { "thread_step",       (Tcl_CmdProc*)tcl_thread_step       },
   { "thread_finalize",   (Tcl_CmdProc*)tcl_thread_finalize   },
   { "thread_swap",       (Tcl_CmdProc*)tcl_thread_swap       },
   { "thread_db_get",     (Tcl_CmdProc*)tcl_thread_db_get     },
   { "thread_db_put",     (Tcl_CmdProc*)tcl_thread_db_put     },
   { "thread_stmt_get",   (Tcl_CmdProc*)tcl_thread_stmt_get   },
   { "thread_exec",       (Tcl_CmdProc*)tcl_thread_exec       },
 };
 int i;

 for(i=0; i<sizeof(aCmd)/sizeof(aCmd[0]); i++){
  Tcl_CreateCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
}
return TCL_OK;
}
#endif
