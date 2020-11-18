/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file implements Windows dependent functions used by row lock
** feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if SQLITE_OS_WIN

#include <windows.h>
#include <process.h>
#include <signal.h>
#include "rowlock_os.h"
#include "sqlite3.h"
#include "sqliteInt.h"

int rowlockOsSetSignalAction(int *signals, int nSignal, void *action){
  int i;

  for( i=0; i<nSignal; i++){
    void (*ret)(int);
    ret = signal(signals[i], (void(__cdecl*)(int))action);
    if( ret==SIG_ERR ){
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

/*
** Windows does not allow to use a string inclugin '\' as a mutex name.
** The name given to rowlockOsMutexOpen() is a database file path which 
** might includes '\'. So this function replaces '\' to '_'.
** The memory of 2nd argument 'pOut' must be allocated outside of this 
** function.
*/
static void createMutexName(const char *pIn, char *pOut){
  int i = 0;
  for(; pIn[i]!='\0'; i++ ){
    pOut[i] = (pIn[i]=='\\') ? '_' : pIn[i];
  }
}

int rowlockOsMutexOpen(const char *name, MUTEX_HANDLE *pMutex){
  HANDLE mtx;
  SECURITY_DESCRIPTOR secDesc;
  SECURITY_ATTRIBUTES secAttr;
  char mtxName[MAX_PATH_LEN] = {0};

  InitializeSecurityDescriptor(&secDesc,SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl(&secDesc, TRUE, 0, FALSE);	    
  secAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  secAttr.lpSecurityDescriptor = &secDesc;
  secAttr.bInheritHandle = TRUE; 

  createMutexName(name, mtxName);
  mtx = CreateMutex(&secAttr, FALSE, TEXT(mtxName));
  if( mtx==NULL ) return SQLITE_ERROR;

  pMutex->handle = mtx;
  pMutex->held = 0;

  return SQLITE_OK;
}

void rowlockOsMutexClose(MUTEX_HANDLE *pMutex){
  CloseHandle(pMutex->handle);
  pMutex->handle = NULL;
  pMutex->held = 0;
}

void rowlockOsMutexEnter(MUTEX_HANDLE *pMutex){
  WaitForSingleObject(pMutex->handle, INFINITE);
  pMutex->held = 1;
}

void rowlockOsMutexLeave(MUTEX_HANDLE *pMutex){
  pMutex->held = 0;
  ReleaseMutex(pMutex->handle);
}

int rowlockOsMutexHeld(MUTEX_HANDLE *pMutex){
  return pMutex->held;
}

int rowlockOsMmapOpen(u64 allocSize, const char *name, MMAP_HANDLE *phMap, void **ppMap){
  int rc = SQLITE_OK;
  MMAP_HANDLE hMap = {0};
  int created = 0;
  void *pMap = NULL;

  hMap.hdlFile = CreateFile(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 
                     NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_RANDOM_ACCESS, NULL);
  /* CreateFile return INVALID_HANDLE_VALUE when fail to create file */

  if( hMap.hdlFile==INVALID_HANDLE_VALUE ) return SQLITE_CANTOPEN_BKPT;

  created = (GetLastError() != ERROR_ALREADY_EXISTS);

  hMap.hdlMap = CreateFileMapping(hMap.hdlFile, NULL, PAGE_READWRITE, allocSize>>32, (DWORD)allocSize, NULL);
  if( !hMap.hdlMap ){
    rc = SQLITE_CANTOPEN_BKPT;
    goto mmap_open_error;
  }

  pMap = MapViewOfFile(hMap.hdlMap, FILE_MAP_WRITE, 0, 0, 0);
  if( !pMap ){
    rc =  SQLITE_IOERR_SHMMAP;
    goto mmap_open_error;
  }

  /* Set output parameters. */
  phMap->hdlFile = hMap.hdlFile;
  phMap->hdlMap = hMap.hdlMap;
  strcpy_s(phMap->name, sizeof(phMap->name), name);
  *ppMap = pMap;

  return SQLITE_OK;

mmap_open_error:
  rowlockOsMmapClose(hMap, pMap);
  if( created ) DeleteFile(name);
  return rc;
}

void rowlockOsMmapClose(MMAP_HANDLE hMap, void *pMap){
  if( pMap ) UnmapViewOfFile(pMap);
  if( hMap.hdlMap ) CloseHandle(hMap.hdlMap);
  if( hMap.hdlFile ) CloseHandle(hMap.hdlFile);
  /*
  ** Delete mapped file if no one opens it. DeleteFile fails 
  ** if the file is opend by anyone. So we ignoew the error.
  ** GetLastError() is called in order to reset the last error.
  */
  if( hMap.name[0]!=0 ){
    DeleteFile(hMap.name);
    GetLastError();
  }
}

int rowlockOsMmapSync(void *pMap){
  return FlushViewOfFile(pMap, 0);
}

#endif /* SQLITE_OS_WIN */
#endif /* SQLITE_OMIT_ROWLOCK */
