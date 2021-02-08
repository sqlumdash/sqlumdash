/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for the blob key hash-table implementation
** used by row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_PSM_HASH_H
#define SQLITE_ROWLOCK_PSM_HASH_H
#include "sqlite3.h"
#include "sqliteInt.h"

/* Forward declarations of structures. */
typedef struct HashBlob HashBlob;
typedef struct HashElemBlob HashElemBlob;

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
struct HashBlob {
  unsigned int htsize;      /* Number of buckets in the hash table */
  unsigned int count;       /* Number of entries in this table */
  HashElemBlob *first;      /* The first element of the array */
  struct _htBlob {          /* the hash table */
    int count;              /* Number of entries with this hash */
    HashElemBlob *chain;    /* Pointer to first entry with this hash */
  } *ht;
};

/* Each element in the hash table is an instance of the following 
** structure.  All elements are stored on a single doubly-linked list.
**
** Again, this structure is intended to be opaque, but it can't really
** be opaque because it is used by macros.
*/
struct HashElemBlob {
  HashElemBlob *next, *prev;   /* Next and previous elements in the table */
  void *data;                  /* Data associated with this element */
  void *pKey;                  /* Key associated with this element */
  sqlite3_int64 nKey;          /* Key size */
};

/*
** Access routines.  To delete, insert a NULL pointer.
*/
void sqlite3HashBlobInit(HashBlob*);
void *sqlite3HashBlobInsert(HashBlob*, const void *pKey,
                            sqlite3_int64 nKey, void *data,
                            void *allocator,
                            void *(*xMalloc)(void*, sqlite3_int64),
                            void (*xFree)(void*, void*),
                            int(*xCompare)(const void*, sqlite3_int64, const void*, sqlite3_int64, const CollSeq *pColl),
                            const CollSeq *pColl);
void *sqlite3HashBlobFind(const HashBlob*, const void *pKey,
                          sqlite3_int64 nKey,
                          int (*xCompare)(const void*, sqlite3_int64, const void*, sqlite3_int64, const CollSeq *pColl),
                          const CollSeq *pColl);
void sqlite3HashBlobClear(HashBlob*, void *allocator,
                          void (*xFree)(void*, void*));
void sqlite3HashBlobRemoveElement(HashBlob *pH, HashElemBlob *elem,
                                  void *allocator, void (*xFree)(void*, void*));

/*
** Macros for looping over all elements of a hash table.  The idiom is
** like this:
**
**   Hash h;
**   HashElem *p;
**   ...
**   for(p=sqliteHashBlobFirst(&h); p; p=sqliteHashBlobNext(p)){
**     SomeStructure *pData = sqliteHashBlobData(p);
**     // do something with pData
**   }
*/
#define sqliteHashBlobFirst(H)   ((H)->first)
#define sqliteHashBlobNext(E)    ((E)->next)
#define sqliteHashBlobKey(E)     ((E)->pKey)
#define sqliteHashBlobKeySize(E) ((E)->nKey)
#define sqliteHashBlobData(E)    ((E)->data)

#endif /* SQLITE_ROWLOCK_PSM_HASH_H */
#endif /* SQLITE_OMIT_ROWLOCK */
