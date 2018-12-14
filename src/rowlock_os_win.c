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

int rowlockOsMutexOpen(char *name, MUTEX_HANDLE *pMutex){
  HANDLE mtx;
  SECURITY_DESCRIPTOR secDesc;
  SECURITY_ATTRIBUTES secAttr;

  InitializeSecurityDescriptor(&secDesc,SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl(&secDesc, TRUE, 0, FALSE);	    
  secAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  secAttr.lpSecurityDescriptor = &secDesc;
  secAttr.bInheritHandle = TRUE; 

  mtx = CreateMutex(&secAttr, FALSE, TEXT(name));
  if( mtx==NULL ){
    return SQLITE_ERROR;
  }

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

int rowlockOsMmapOpen(u64 allocSize, char *name, MMAP_HANDLE *phMap, void **ppMap){
  HANDLE hMap;
  void *pMap = NULL;

  hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, allocSize>>32, (DWORD)allocSize, name);
  if( !hMap ) return SQLITE_CANTOPEN_BKPT;

  pMap = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0);
  if( !pMap ){
    CloseHandle(hMap);
    return SQLITE_IOERR_SHMMAP;
  }

  phMap->handle = hMap;
  *ppMap = pMap;

  return SQLITE_OK;
}

void rowlockOsMmapClose(MMAP_HANDLE hMap, void *pMap){
  if( pMap ){
    UnmapViewOfFile(pMap);
    CloseHandle(hMap.handle);
  }
}

int rowlockOsMmapSync(void *pMap){
  return FlushViewOfFile(pMap, 0);
}

#endif /* SQLITE_OS_WIN */
#endif /* SQLITE_OMIT_ROWLOCK */
