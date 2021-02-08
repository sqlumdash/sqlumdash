/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the implementation of the blob key hash-table 
** implementation used by row lock feature. Memory allocator can be
** specified.
*/
#include "rowlock_psm_hash.h"
#include "sqliteInt.h"
#include <assert.h>

static int setKey(HashBlob *pH, HashElemBlob *elem, const void *pKey,
                  sqlite3_int64 nKey, void *allocator,
                  void *(*xMalloc)(void*, sqlite3_int64)){
  void *p = xMalloc(allocator, nKey);
  if( p ){
    memcpy(p, pKey, nKey);
    elem->pKey = p;
    elem->nKey = nKey;
    return 1;
  }else{
    return 0;
  }
}

/* Turn bulk memory into a hash table object by initializing the
** fields of the Hash structure.
**
** "pNew" is a pointer to the hash table that is to be initialized.
*/
void sqlite3HashBlobInit(HashBlob *pNew){
  assert( pNew!=0 );
  pNew->first = 0;
  pNew->count = 0;
  pNew->htsize = 0;
  pNew->ht = 0;
}

/* Remove all entries from a hash table.  Reclaim all memory.
** Call this routine to delete a hash table or to reset a hash table
** to the empty state.
*/
void sqlite3HashBlobClear(HashBlob *pH, void *allocator,
                          void (*xFree)(void*, void*)){
  HashElemBlob *elem;         /* For looping over all elements of the table */

  assert( pH!=0 );
  elem = pH->first;
  pH->first = 0;
  xFree(allocator, pH->ht);
  pH->ht = 0;
  pH->htsize = 0;
  while( elem ){
    HashElemBlob *next_elem = elem->next;
    xFree(allocator, elem->pKey);
    xFree(allocator, elem);
    elem = next_elem;
  }
  pH->count = 0;
}

/*
** The hashing function.
*/
static unsigned int blobHash(const void *p, sqlite3_int64 n){
  sqlite3_int64 i;
  unsigned int h = 0;
  unsigned char c;
  unsigned char *z = (unsigned char*)p;

  for(i=0;i<n;i++){
    c = *z++;
    h += c;
    h *= 0x9e3779b1;
  }
  return h;
}


/* Link pNew element into the hash table pH.  If pEntry!=0 then also
** insert pNew into the pEntry hash bucket.
*/
static void insertElementBlob(
  HashBlob *pH,              /* The complete hash table */
  struct _htBlob *pEntry,    /* The entry into which pNew is inserted */
  HashElemBlob *pNew         /* The element to be inserted */
){
  HashElemBlob *pHead;       /* First element already in pEntry */
  if( pEntry ){
    pHead = pEntry->count ? pEntry->chain : 0;
    pEntry->count++;
    pEntry->chain = pNew;
  }else{
    pHead = 0;
  }
  if( pHead ){
    pNew->next = pHead;
    pNew->prev = pHead->prev;
    if( pHead->prev ){ pHead->prev->next = pNew; }
    else             { pH->first = pNew; }
    pHead->prev = pNew;
  }else{
    pNew->next = pH->first;
    if( pH->first ){ pH->first->prev = pNew; }
    pNew->prev = 0;
    pH->first = pNew;
  }
}


/* Resize the hash table so that it cantains "new_size" buckets.
**
** The hash table might fail to resize if xMalloc() fails or
** if the new size is the same as the prior size.
** Return TRUE if the resize occurs and false if not.
*/
static int rehashBlob(HashBlob *pH, unsigned int new_size,
                      void *allocator,
                      void *(*xMalloc)(void*, sqlite3_int64),
                      void (*xFree)(void*, void*)){
  struct _htBlob *new_ht;            /* The new hash table */
  HashElemBlob *elem, *next_elem;    /* For looping over existing elements */

#if SQLITE_MALLOC_SOFT_LIMIT>0
  if( new_size*sizeof(struct _htBlob)>SQLITE_MALLOC_SOFT_LIMIT ){
    new_size = SQLITE_MALLOC_SOFT_LIMIT/sizeof(struct _htBlob);
  }
  if( new_size==pH->htsize ) return 0;
#endif

  new_ht = (struct _htBlob *)xMalloc( allocator, new_size*sizeof(struct _htBlob) );

  if( new_ht==0 ) return 0;
  xFree(allocator, pH->ht);
  pH->ht = new_ht;
  pH->htsize = new_size;
  memset(new_ht, 0, new_size*sizeof(struct _htBlob));
  for(elem=pH->first, pH->first=0; elem; elem = next_elem){
    unsigned int h = blobHash(elem->pKey, elem->nKey) % new_size;
    next_elem = elem->next;
    insertElementBlob(pH, &new_ht[h], elem);
  }
  return 1;
}

/* This function (for internal use only) locates an element in an
** hash table that matches the given key.  If no element is found,
** a pointer to a static null element with HashElem.data==0 is returned.
** If pH is not NULL, then the hash for this key is written to *pH.
*/
static HashElemBlob *findElementWithHashBlob(
  const HashBlob *pH,  /* The pH to be searched */
  const void *pKey,    /* The key we are searching for */
  sqlite_int64 nKey,   /* Key size. */
  unsigned int *pHash, /* Write the hash value here */
  int (*xCompare)(const void*, sqlite3_int64, const void*, sqlite3_int64, const CollSeq *pColl),
  const CollSeq *pColl
){
  HashElemBlob *elem;                /* Used to loop thru the element list */
  unsigned int count;                /* Number of elements left to test */
  unsigned int h;                    /* The computed hash */
  static HashElemBlob nullElement = { 0, 0, 0, 0 };

  if( pH->ht ){   /*OPTIMIZATION-IF-TRUE*/
    struct _htBlob *pEntry;
    h = blobHash(pKey, nKey) % pH->htsize;
    pEntry = &pH->ht[h];
    elem = pEntry->chain;
    count = pEntry->count;
  }else{
    h = 0;
    elem = pH->first;
    count = pH->count;
  }
  if( pHash ) *pHash = h;
  while( count-- ){
    assert( elem!=0 );
    if( xCompare(elem->pKey, elem->nKey, pKey, nKey, pColl)==0 ){ 
      return elem;
    }
    elem = elem->next;
  }
  return &nullElement;
}

/* Remove a single entry from the hash table given a pointer to that
** element and a hash on the element's key.
*/
static void removeElementGivenHashBlob(
  HashBlob *pH,         /* The pH containing "elem" */
  HashElemBlob *elem,   /* The element to be removed from the pH */
  unsigned int h,       /* Hash value for the element */
  void *allocator,      /* 1st argument for xFree */
  void (*xFree)(void*, void*)
){
  struct _htBlob *pEntry;
  if( elem->prev ){
    elem->prev->next = elem->next; 
  }else{
    pH->first = elem->next;
  }
  if( elem->next ){
    elem->next->prev = elem->prev;
  }
  if( pH->ht ){
    pEntry = &pH->ht[h];
    if( pEntry->chain==elem ){
      pEntry->chain = elem->next;
    }
    assert( pEntry->count>0 );
    pEntry->count--;
  }
  xFree( allocator, elem->pKey );
  xFree( allocator, elem );
  pH->count--;
  if( pH->count==0 ){
    assert( pH->first==0 );
    assert( pH->count==0 );
    sqlite3HashBlobClear(pH, allocator, xFree);
  }
}

/* Attempt to locate an element of the hash table pH with a key
** that matches iKey.  Return the data for this element if it is
** found, or NULL if there is no match.
*/
void *sqlite3HashBlobFind(const HashBlob *pH, const void *pKey,
                          sqlite_int64 nKey,
                          int (*xCompare)(const void*, sqlite3_int64, const void*, sqlite3_int64, const CollSeq *pColl),
                          const CollSeq *pColl){
  assert( pH!=0 );
  assert( pKey!=0 );
  assert( nKey>0 );
  return findElementWithHashBlob(pH, pKey, nKey, 0, xCompare, pColl)->data;
}

/* Insert an element into the hash table pH.  The key is pKey
** and the data is "data".
**
** If no element exists with a matching key, then a new
** element is created and NULL is returned.
**
** If another element already exists with the same key, then the
** new data replaces the old data and the old data is returned.
** The key is not copied in this instance.  If a malloc fails, then
** the new data is returned and the hash table is unchanged.
**
** If the "data" parameter to this function is NULL, then the
** element corresponding to "key" is removed from the hash table.
*/
void *sqlite3HashBlobInsert(HashBlob *pH, const void *pKey,
                            sqlite_int64 nKey, void *data,
                            void *allocator,
                            void *(*xMalloc)(void*, sqlite3_int64),
                            void (*xFree)(void*, void*),
                            int (*xCompare)(const void*, sqlite3_int64, const void*, sqlite3_int64, const CollSeq *pColl),
                            const CollSeq *pColl){
  unsigned int h;       /* the hash of the key modulo hash table size */
  HashElemBlob *elem;       /* Used to loop thru the element list */
  HashElemBlob *new_elem;   /* New element added to the pH */

  assert( pH!=0 );
  assert( pKey!=0 );
  assert( nKey>0 );
  elem = findElementWithHashBlob(pH,pKey,nKey,&h, xCompare, pColl);
  if( elem->data ){
    void *old_data = elem->data;
    if( data==0 ){
      removeElementGivenHashBlob(pH,elem,h, allocator, xFree);
    }else{
      elem->data = data;
      if( !setKey(pH, elem, pKey, nKey, allocator, xMalloc) ) return data;
    }
    return old_data;
  }
  if( data==0 ) return 0;
  new_elem = (HashElemBlob*)xMalloc(allocator, sizeof(HashElemBlob) );
  if( new_elem==0 ) return data;

  if( !setKey(pH, new_elem, pKey, nKey, allocator, xMalloc) ){
    xFree(allocator, new_elem);
    return data;
  }

  new_elem->data = data;
  pH->count++;
  if( pH->count>=10 && pH->count > 2*pH->htsize ){
    if( rehashBlob(pH, pH->count*2, allocator, xMalloc, xFree) ){
      assert( pH->htsize>0 );
      h = blobHash(pKey, nKey) % pH->htsize;
    }
  }
  insertElementBlob(pH, pH->ht ? &pH->ht[h] : 0, new_elem);
  return 0;
}

void sqlite3HashBlobRemoveElement(HashBlob *pH, HashElemBlob *elem,
                                  void *allocator, void (*xFree)(void*, void*)){
  unsigned int h;       /* the hash of the key modulo hash table size */

  if( pH->ht ){
    h = blobHash(elem->pKey, elem->nKey) % pH->htsize;
  }else{
    h = 0;
  }
  removeElementGivenHashBlob(pH, elem, h, allocator, xFree);
}
