/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for common functions of OS dependent code
** used by row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_OS_H
#define SQLITE_ROWLOCK_OS_H

#include "sqliteInt.h"
#if SQLITE_OS_WIN
#include "rowlock_os_win.h"
#else
#include "rowlock_os_linux.h"
#endif

int rowlockOsSetSignalAction(int *signals, int nSignal, void *action);

/*
** Create recursive mutex which can be shared by processes.
** It is used when it refers a shared object of row lock.
*/
int rowlockOsMutexOpen(const char *name, MUTEX_HANDLE *pMutex);

void rowlockOsMutexClose(MUTEX_HANDLE *pMutex);
void rowlockOsMutexEnter(MUTEX_HANDLE *pMutex);
void rowlockOsMutexLeave(MUTEX_HANDLE *pMutex);
#ifndef NDEBUG
int rowlockOsMutexHeld(MUTEX_HANDLE *pMutex);
#endif
int rowlockOsMmapOpen(u64 allocSize, const char *name, MMAP_HANDLE *phMap, void **ppMap);
void rowlockOsMmapClose(MMAP_HANDLE hMap, void *pMap);
int rowlockOsMmapSync(void *pMap);

#endif /* SQLITE_ROWLOCK_OS_H */
#endif /* SQLITE_OMIT_ROWLOCK */
