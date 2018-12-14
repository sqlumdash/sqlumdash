/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for the int64 key hash-table implementation
** used by row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_HASH_H
#define SQLITE_ROWLOCK_HASH_H
#include "sqlite3.h"

/* Forward declarations of structures. */
typedef struct HashI64 HashI64;
typedef struct HashElemI64 HashElemI64;

/* A complete hash table is an instance of the following structure.
** The internals of this structure are intended to be opaque -- client
** code should not attempt to access or modify the fields of this structure
** directly.  Change this structure only by using the routines below.
** However, some of the "procedures" and "functions" for modifying and
** accessing this structure are really macros, so we can't really make
** this structure opaque.
**
** All elements of the hash table are on a single doubly-linked list.
** Hash.first points to the head of this list.
**
** There are Hash.htsize buckets.  Each bucket points to a spot in
** the global doubly-linked list.  The contents of the bucket are the
** element pointed to plus the next _ht.count-1 elements in the list.
**
** Hash.htsize and Hash.ht may be zero.  In that case lookup is done
** by a linear search of the global list.  For small tables, the 
** Hash.ht table is never allocated because if there are few elements
** in the table, it is faster to do a linear search than to manage
** the hash table.
*/
struct HashI64 {
  unsigned int htsize;      /* Number of buckets in the hash table */
  unsigned int count;       /* Number of entries in this table */
  HashElemI64 *first;       /* The first element of the array */
  struct _htI64 {           /* the hash table */
    int count;                 /* Number of entries with this hash */
    HashElemI64 *chain;        /* Pointer to first entry with this hash */
  } *ht;
};

/* Each element in the hash table is an instance of the following 
** structure.  All elements are stored on a single doubly-linked list.
**
** Again, this structure is intended to be opaque, but it can't really
** be opaque because it is used by macros.
*/
struct HashElemI64 {
  HashElemI64 *next, *prev;    /* Next and previous elements in the table */
  void *data;                  /* Data associated with this element */
  sqlite_int64 iKey;           /* Key associated with this element */
};

/*
** Access routines.  To delete, insert a NULL pointer.
*/
void sqlite3HashI64Init(HashI64*);
void *sqlite3HashI64Insert(HashI64*, sqlite_int64 iKey, void *pData);
void *sqlite3HashI64Find(const HashI64*, sqlite_int64 iKey);
void sqlite3HashI64Clear(HashI64*);

/*
** Macros for looping over all elements of a hash table.  The idiom is
** like this:
**
**   Hash h;
**   HashElem *p;
**   ...
**   for(p=sqliteHashI64First(&h); p; p=sqliteHashI64Next(p)){
**     SomeStructure *pData = sqliteHashI64Data(p);
**     // do something with pData
**   }
*/
#define sqliteHashI64First(H)  ((H)->first)
#define sqliteHashI64Next(E)   ((E)->next)
#define sqliteHashI64Key(E)   ((E)->iKey)
#define sqliteHashI64Data(E)   ((E)->data)

#endif /* SQLITE_ROWLOCK_HASH_H */
#endif /* SQLITE_OMIT_ROWLOCK */
