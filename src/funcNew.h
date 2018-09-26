/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for SQL function implementation
*/
#ifndef SQLITE_FUNC_NEW_H
#define SQLITE_FUNC_NEW_H

void ceilFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void floorFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void modFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void truncFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void varianceStep(sqlite3_context *context, int argc, sqlite3_value **argv);
void varianceInverse(sqlite3_context *context, int argc, sqlite3_value **argv);
void varianceFinalize(sqlite3_context *context);
void variancePStep(sqlite3_context *context, int argc, sqlite3_value **argv);
void variancePInverse(sqlite3_context *context, int argc, sqlite3_value **argv);
void variancePFinalize(sqlite3_context *context);
void stddevStep(sqlite3_context *context, int argc, sqlite3_value **argv);
void stddevInverse(sqlite3_context *context, int argc, sqlite3_value **argv);
void stddevFinalize(sqlite3_context *context);
void stddevPStep(sqlite3_context *context, int argc, sqlite3_value **argv);
void stddevPInverse(sqlite3_context *context, int argc, sqlite3_value **argv);
void stddevPFinalize(sqlite3_context *context);
void nvlFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void asciiFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void concatFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void initcapFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void insertFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void leftFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void rightFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void lpadFunc(sqlite3_context *context, int argc, sqlite3_value **argv);
void rpadFunc(sqlite3_context *context, int argc, sqlite3_value **argv);


#define SQLUMDASH_NEW_FUNCTIONS \
  FUNCTION(ceil,               1, 0, 0, ceilFunc          ), \
  FUNCTION(ceiling,            1, 0, 0, ceilFunc          ), \
  FUNCTION(floor,              1, 0, 0, floorFunc         ), \
  FUNCTION(mod,                2, 0, 0, modFunc           ), \
  WAGGREGATE(variance,  1,0,0, varianceStep, \
      varianceFinalize, varianceFinalize, varianceInverse, 0), \
  WAGGREGATE(variancep, 1,0,0, variancePStep, \
      variancePFinalize, variancePFinalize, variancePInverse, 0), \
  WAGGREGATE(stddev,    1,0,0, stddevStep, \
      stddevFinalize, stddevFinalize, stddevInverse, 0), \
  WAGGREGATE(stddevp,   1,0,0, stddevPStep, \
      stddevPFinalize, stddevPFinalize, stddevPInverse, 0), \
  FUNCTION(nvl,                2, 0, 0, nvlFunc           ), \
  FUNCTION(asc,                1, 0, 0, asciiFunc         ), \
  FUNCTION(ascii,              1, 0, 0, asciiFunc         ), \
  FUNCTION(chr,               -1, 0, 0, charFunc          ), \
  FUNCTION2(len,               1, 0, 0, lengthFunc,       SQLITE_FUNC_LENGTH ), \
  FUNCTION2(char_length,       1, 0, 0, lengthFunc,       SQLITE_FUNC_LENGTH ), \
  FUNCTION2(character_length,  1, 0, 0, lengthFunc,       SQLITE_FUNC_LENGTH ), \
  FUNCTION(concat,            -1, 0, 0, concatFunc        ), \
  FUNCTION(initcap,            1, 0, 0, initcapFunc       ), \
  FUNCTION(lengthb,            1, 0, 0, lengthFunc        ), \
  FUNCTION(lpad,               2, 0, 0, lpadFunc          ), \
  FUNCTION(lpad,               3, 0, 0, lpadFunc          ), \
  FUNCTION(rpad,               2, 0, 0, rpadFunc          ), \
  FUNCTION(rpad,               3, 0, 0, rpadFunc          ), \
  FUNCTION(substring,          3, 0, 0, substrFunc        ), \
  FUNCTION(trunc,              1, 0, 0, truncFunc         ), \
  FUNCTION(trunc,              2, 0, 0, truncFunc         )

#endif /* SQLITE_FUNC_NEW_H */
