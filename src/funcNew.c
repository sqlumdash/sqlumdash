/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains SQL function implementation.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"
#include <assert.h>
#include <math.h>

void ceilFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( sqlite3_value_type(argv[0]) ){
    case SQLITE_NULL: {
      /* Ceil(X) returns NULL if X is NULL. */
      sqlite3_result_null(context);
      break;
    }
    default: {
      double val = sqlite3_value_double(argv[0]);
      sqlite3_result_double(context, ceil(val));
      break;
    }
  }
}

void floorFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( sqlite3_value_type(argv[0]) ){
    case SQLITE_NULL: {
      /* Ceil(X) returns NULL if X is NULL. */
      sqlite3_result_null(context);
      break;
    }
    default: {
      double val = sqlite3_value_double(argv[0]);
      sqlite3_result_double(context, floor(val));
      break;
    }
  }
}

void modFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  double valX = 0;
  double valY = 0;
  assert( argc==2 );
  UNUSED_PARAMETER(argc);
  if( sqlite3_value_type(argv[0])==SQLITE_NULL || sqlite3_value_type(argv[1])==SQLITE_NULL ){
    /* Mod(X,Y) returns NULL if X or Y is NULL. */
    sqlite3_result_null(context);
    return;
  }

  valX = sqlite3_value_double(argv[0]);
  valY = sqlite3_value_double(argv[1]);
  valX = floor(valX);
  valY = floor(valY);
  sqlite3_result_double(context, fmod(valX, valY));
}

void truncFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  int n = 0;
  double r;
  assert( argc==1 || argc==2 );
  if( argc==2 ){
    if( SQLITE_NULL==sqlite3_value_type(argv[1]) ) return;
    n = sqlite3_value_int(argv[1]);
    if( n>30 ) n = 30;
    if( n<0 ) n = 0;
  }
  if( sqlite3_value_type(argv[0])==SQLITE_NULL ) return;
  r = sqlite3_value_double(argv[0]);

  /* If Y==0 and X will fit in a 64-bit int,
  ** handle the rounding directly,
  ** otherwise use printf.
  */
  if( n==0 && r>=0 && r<LARGEST_INT64-1 ){
    r = (double)((sqlite3_int64)r);
  }else if( n==0 && r<0 && (-r)<LARGEST_INT64-1 ){
    r = -(double)((sqlite3_int64)((-r)));
  }else{
    double a = floor(r);         /* Integer part */
    double b = r - a;            /* Float part */
    double t = pow(10, n);
    double c = floor(b * t) / t; /* Truncation */
    r = a + c;
  }
  sqlite3_result_double(context, r);
}

/*
** An instance of the following structure holds the context of a
** variance() or stddev() aggregate computation.
*/
typedef struct VarCtx VarCtx;
struct VarCtx {
  double sum;      /* Floating point sum */
  double sqsum;    /* Floating point square sum */
  i64 cnt;         /* Number of elements summed */
};

void varianceStep(sqlite3_context *context, int argc, sqlite3_value **argv){
  VarCtx *p;
  int type;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  p = (VarCtx*)sqlite3_aggregate_context(context, sizeof(*p));
  type = sqlite3_value_numeric_type(argv[0]);
  if( p && type!=SQLITE_NULL ){
    double v = sqlite3_value_double(argv[0]);
    p->cnt++;
    p->sum += v;
    p->sqsum += v * v;
  }
}

#ifndef SQLITE_OMIT_WINDOWFUNC
void varianceInverse(sqlite3_context *context, int argc, sqlite3_value **argv){
  VarCtx *p;
  int type;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  p = (VarCtx*)sqlite3_aggregate_context(context, sizeof(*p));
  type = sqlite3_value_numeric_type(argv[0]);
  if( p && type!=SQLITE_NULL ){
    double v = sqlite3_value_double(argv[0]);
    p->cnt--;
    p->sum -= v;
    p->sqsum -= v * v;
  }
}
#else
# define varianceInverse 0
#endif /* SQLITE_OMIT_WINDOWFUNC */

void varianceFinalize(sqlite3_context *context){
  VarCtx *p;
  p = (VarCtx*)sqlite3_aggregate_context(context, 0);
  if( p && p->cnt>1 ){
    double squ = p->sqsum - (p->sum * p->sum / p->cnt);
    double var = squ / (p->cnt - 1);
    sqlite3_result_double(context, var);
  }
}

void variancePStep(sqlite3_context *context, int argc, sqlite3_value **argv){
  varianceStep(context, argc, argv);
}

#ifndef SQLITE_OMIT_WINDOWFUNC
void variancePInverse(sqlite3_context *context, int argc, sqlite3_value **argv){
  varianceInverse(context, argc, argv);
}
#else
# define variancePInverse 0
#endif /* SQLITE_OMIT_WINDOWFUNC */

void variancePFinalize(sqlite3_context *context){
  VarCtx *p;
  p = (VarCtx*)sqlite3_aggregate_context(context, 0);
  if( p && p->cnt>0 ){
    double varp = p->sqsum / p->cnt - pow(p->sum / p->cnt, 2);
    sqlite3_result_double(context, varp);
  }
}

void stddevStep(sqlite3_context *context, int argc, sqlite3_value **argv){
  varianceStep(context, argc, argv);
}

#ifndef SQLITE_OMIT_WINDOWFUNC
void stddevInverse(sqlite3_context *context, int argc, sqlite3_value **argv){
  varianceInverse(context, argc, argv);
}
#else
# define stddevInverse 0
#endif /* SQLITE_OMIT_WINDOWFUNC */

void stddevFinalize(sqlite3_context *context){
  VarCtx *p;
  p = (VarCtx*)sqlite3_aggregate_context(context, 0);
  if( p && p->cnt>1 ){
    double squ = p->sqsum - (p->sum * p->sum / p->cnt);
    double var = squ / (p->cnt - 1);
    double dev = sqrt(var);
    sqlite3_result_double(context, dev);
  }
}

void stddevPStep(sqlite3_context *context, int argc, sqlite3_value **argv){
  varianceStep(context, argc, argv);
}

#ifndef SQLITE_OMIT_WINDOWFUNC
void stddevPInverse(sqlite3_context *context, int argc, sqlite3_value **argv){
  varianceInverse(context, argc, argv);
}
#else
# define stddevPInverse 0
#endif /* SQLITE_OMIT_WINDOWFUNC */

void stddevPFinalize(sqlite3_context *context){
  VarCtx *p;
  p = (VarCtx*)sqlite3_aggregate_context(context, 0);
  if( p && p->cnt>0 ){
    double varp = p->sqsum / p->cnt - pow(p->sum / p->cnt, 2);
    double devp = sqrt(varp);
    sqlite3_result_double(context, devp);
  }
}

void nvlFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  assert( argc==2 );
  UNUSED_PARAMETER(argc);
  if( sqlite3_value_type(argv[0])!=SQLITE_NULL ){
    sqlite3_result_value(context, argv[0]);
  }else{
    sqlite3_result_value(context, argv[1]);
  }
}

void asciiFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  const unsigned char *z;

  z = sqlite3_value_text(argv[0]);
  if (z == 0) return;

  sqlite3_result_int(context, z[0]);
}

void concatFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  StrAccum str;
  sqlite3 *db = sqlite3_context_db_handle(context);
  int i;

  sqlite3StrAccumInit(&str, db, 0, 0, db->aLimit[SQLITE_LIMIT_LENGTH]);
  for(i=0; i<argc; i++){
    int len = sqlite3_value_bytes(argv[i]);
    const char *z = (const char*)sqlite3_value_text(argv[i]);
    sqlite3_str_append(&str, z, len);
  }

  sqlite3_result_text(context, sqlite3StrAccumFinish(&str), -1, SQLITE_DYNAMIC);
}

void initcapFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  const unsigned char *z;
  int len;
  char *zOut = NULL;

  len = sqlite3_value_bytes(argv[0]) + 1;
  z = sqlite3_value_text(argv[0]);
  if (z == 0) return;

  zOut = (char*)sqlite3MallocZero(len);
  if (zOut == NULL) return;

  memcpy(zOut, z, len);
  zOut[0] = (char)toupper(z[0]);

  sqlite3_result_text(context, zOut, -1, sqlite3_free);
}

void lpadFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  char c;
  int lenTotal = 0;
  const unsigned char *z = NULL;
  int len = 0;
  int lenPad = 0;
  char *zOut = NULL;

  if( sqlite3_value_type(argv[1])==SQLITE_NULL ) return;
  lenPad = sqlite3_value_int(argv[1]);

  /* Get inserting character */
  if( argc==2 ){
    c = ' ';
  }else{
    const unsigned char *zTmp = NULL;
    zTmp = sqlite3_value_text(argv[2]);
    if (zTmp == 0) return;
    c = zTmp[0];
  }

  len = sqlite3_value_bytes(argv[0]);
  z = sqlite3_value_text(argv[0]);
  if (z == 0) return;

  lenTotal = lenPad + len;
  zOut = (char*)sqlite3MallocZero(lenTotal + 1);
  if( zOut==NULL ) return;

  if( lenPad>0 ){
    memset(zOut, c, lenPad);
  }else{
    lenPad = 0;
  }
  memcpy(zOut + lenPad, z, len);

  sqlite3_result_text(context, zOut, -1, sqlite3_free);
}

void rpadFunc(sqlite3_context *context, int argc, sqlite3_value **argv){
  char c;
  int lenTotal = 0;
  const unsigned char *z = NULL;
  int len = 0;
  int lenPad = 0;
  char *zOut = NULL;

  if( sqlite3_value_type(argv[1])==SQLITE_NULL ) return;
  lenPad = sqlite3_value_int(argv[1]);

  /* Get inserting character */
  if( argc==2 ){
    c = ' ';
  }else{
    const unsigned char *zTmp = NULL;
    int type2;
    type2 = sqlite3_value_type(argv[2]);
    zTmp = sqlite3_value_text(argv[2]);
    if (zTmp == 0) return;
    c = zTmp[0];
  }

  len = sqlite3_value_bytes(argv[0]);
  z = sqlite3_value_text(argv[0]);
  if( z==0 )return;

  lenTotal = lenPad + len;
  zOut = (char*)sqlite3MallocZero(lenTotal + 1);
  if( zOut==NULL ) return;

  memcpy(zOut, z, len);

  if( lenPad>0 ){
    memset(zOut + len, c, lenPad);
  }

  sqlite3_result_text(context, zOut, -1, sqlite3_free);
}
