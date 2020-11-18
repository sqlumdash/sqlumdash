/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the implementation of the int64 key hash-table 
** implementation used by row lock feature.
*/
#include "rowlock_hash.h"
#include "sqliteInt.h"
#include <assert.h>

void *rowlockDefaultMalloc(void *allocator, sqlite3_int64 n);

/* Turn bulk memory into a hash table object by initializing the
** fields of the Hash structure.
**
** "pNew" is a pointer to the hash table that is to be initialized.
*/
void sqlite3HashI64Init(HashI64 *pNew){
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
void sqlite3HashI64Clear(HashI64 *pH, void *allocator,
                         void (*xFree)(void*, void*)){
  HashElemI64 *elem;         /* For looping over all elements of the table */

  assert( pH!=0 );
  elem = pH->first;
  pH->first = 0;
  xFree(allocator, pH->ht);
  pH->ht = 0;
  pH->htsize = 0;
  while( elem ){
    HashElemI64 *next_elem = elem->next;
    xFree(allocator, elem);
    elem = next_elem;
  }
  pH->count = 0;
}

/* Link pNew element into the hash table pH.  If pEntry!=0 then also
** insert pNew into the pEntry hash bucket.
*/
static void insertElementI64(
  HashI64 *pH,              /* The complete hash table */
  struct _htI64 *pEntry,    /* The entry into which pNew is inserted */
  HashElemI64 *pNew         /* The element to be inserted */
){
  HashElemI64 *pHead;       /* First element already in pEntry */
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
** The hash table might fail to resize if sqlite3_malloc() fails or
** if the new size is the same as the prior size.
** Return TRUE if the resize occurs and false if not.
*/
static int rehashI64(HashI64 *pH, unsigned int new_size, void *allocator,
                     void *(*xMalloc)(void*, sqlite3_int64),
                     void (*xFree)(void*, void*)){
  struct _htI64 *new_ht;            /* The new hash table */
  HashElemI64 *elem, *next_elem;    /* For looping over existing elements */

#if SQLITE_MALLOC_SOFT_LIMIT>0
  if( new_size*sizeof(struct _htI64)>SQLITE_MALLOC_SOFT_LIMIT ){
    new_size = SQLITE_MALLOC_SOFT_LIMIT/sizeof(struct _htI64);
  }
  if( new_size==pH->htsize ) return 0;
#endif

  /* The inability to allocates space for a larger hash table is
  ** a performance hit but it is not a fatal error.  So mark the
  ** allocation as a benign. Use sqlite3Malloc()/memset(0) instead of 
  ** sqlite3MallocZero() to make the allocation, as sqlite3MallocZero()
  ** only zeroes the requested number of bytes whereas this module will
  ** use the actual amount of space allocated for the hash table (which
  ** may be larger than the requested amount).
  */
  if( xMalloc==rowlockDefaultMalloc ){
    sqlite3BeginBenignMalloc();
    new_ht = (struct _htI64 *)sqlite3Malloc( new_size*sizeof(struct _htI64) );
    sqlite3EndBenignMalloc();
  }else{
    new_ht = (struct _htI64 *)xMalloc( allocator, new_size*sizeof(struct _htI64) );
  }

  if( new_ht==0 ) return 0;
  xFree(allocator, pH->ht);
  pH->ht = new_ht;
  pH->htsize = new_size;
  memset(new_ht, 0, new_size*sizeof(struct _htI64));
  for(elem=pH->first, pH->first=0; elem; elem = next_elem){
    unsigned int h = elem->iKey % new_size;
    next_elem = elem->next;
    insertElementI64(pH, &new_ht[h], elem);
  }
  return 1;
}

/* This function (for internal use only) locates an element in an
** hash table that matches the given key.  If no element is found,
** a pointer to a static null element with HashElem.data==0 is returned.
** If pH is not NULL, then the hash for this key is written to *pH.
*/
static HashElemI64 *findElementWithHashI64(
  const HashI64 *pH,     /* The pH to be searched */
  sqlite_int64 iKey,   /* The key we are searching for */
  unsigned int *pHash /* Write the hash value here */
){
  HashElemI64 *elem;                /* Used to loop thru the element list */
  int count;                     /* Number of elements left to test */
  unsigned int h;                /* The computed hash */
  static HashElemI64 nullElement = { 0, 0, 0, 0 };

  if( pH->ht ){   /*OPTIMIZATION-IF-TRUE*/
    struct _htI64 *pEntry;
    h = iKey % pH->htsize;
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
    if( elem->iKey == iKey ){ 
      return elem;
    }
    elem = elem->next;
  }
  return &nullElement;
}

/* Remove a single entry from the hash table given a pointer to that
** element and a hash on the element's key.
*/
static void removeElementGivenHashI64(
  HashI64 *pH,         /* The pH containing "elem" */
  HashElemI64 *elem,   /* The element to be removed from the pH */
  unsigned int h,      /* Hash value for the element */
  void *allocator,     /* 1st argument for xFree */
  void (*xFree)(void*, void*)
){
  struct _htI64 *pEntry;
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
    pEntry->count--;
    assert( pEntry->count>=0 );
  }
  xFree( allocator, elem );
  pH->count--;
  if( pH->count==0 ){
    assert( pH->first==0 );
    assert( pH->count==0 );
    sqlite3HashI64Clear(pH, allocator, xFree);
  }
}

/* Attempt to locate an element of the hash table pH with a key
** that matches iKey.  Return the data for this element if it is
** found, or NULL if there is no match.
*/
void *sqlite3HashI64Find(const HashI64 *pH, sqlite_int64 iKey){
  assert( pH!=0 );
  return findElementWithHashI64(pH, iKey, 0)->data;
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
void *sqlite3HashI64Insert(HashI64 *pH, sqlite_int64 iKey, void *data,
                          void *allocator,
                          void *(*xMalloc)(void*, sqlite3_int64),
                          void (*xFree)(void*, void*)){
  unsigned int h;       /* the hash of the key modulo hash table size */
  HashElemI64 *elem;       /* Used to loop thru the element list */
  HashElemI64 *new_elem;   /* New element added to the pH */

  assert( pH!=0 );
  elem = findElementWithHashI64(pH,iKey,&h);
  if( elem->data ){
    void *old_data = elem->data;
    if( data==0 ){
      removeElementGivenHashI64(pH,elem,h,allocator,xFree);
    }else{
      elem->data = data;
      elem->iKey = iKey;
    }
    return old_data;
  }
  if( data==0 ) return 0;
  new_elem = (HashElemI64*)xMalloc(allocator, sizeof(HashElemI64) );
  if( new_elem==0 ) return data;
  new_elem->iKey = iKey;
  new_elem->data = data;
  pH->count++;
  if( pH->count>=10 && pH->count > 2*pH->htsize ){
    if( rehashI64(pH, pH->count*2, allocator, xMalloc, xFree) ){
      assert( pH->htsize>0 );
      h = iKey % pH->htsize;
    }
  }
  insertElementI64(pH, pH->ht ? &pH->ht[h] : 0, new_elem);
  return 0;
}
