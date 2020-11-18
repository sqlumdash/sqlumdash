/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for Windows dependent functions used by row
** lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if SQLITE_OS_WIN
#ifndef SQLITE_ROWLOCK_OS_WIN_H
#define SQLITE_ROWLOCK_OS_WIN_H

#include <Windows.h>

/* Maximum length of file path.
** The value is same as SQLITE_WIN32_MAX_PATH defined in os_win.c because the
** Unit is char
*/
#define MAX_PATH_LEN (MAX_PATH)

#define THREAD_LOCAL __declspec(thread)

typedef DWORD PID; /* Data type of process ID */
typedef DWORD TID; /* Data type of thread ID */
typedef struct MUTEX_HANDLE {
  HANDLE handle;
  int held; /* True if I have the mutex. */
} MUTEX_HANDLE;
typedef struct MMAP_HANDLE {
  HANDLE hdlFile;
  HANDLE hdlMap;
  char name[MAX_PATH_LEN];
} MMAP_HANDLE;

#define xSnprintf sprintf_s
#define rowlockGetPid GetCurrentProcessId
#define rowlockGetTid GetCurrentThreadId

#endif /* SQLITE_ROWLOCK_OS_WIN_H */
#endif /* SQLITE_OS_WIN */
#endif /* SQLITE_OMIT_ROWLOCK */
