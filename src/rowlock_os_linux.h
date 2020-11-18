/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for Linux dependent functions used by row
** lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if SQLITE_OS_UNIX
#ifndef SQLITE_ROWLOCK_OS_LINUX_H
#define SQLITE_ROWLOCK_OS_LINUX_H

#include <pthread.h>

/* Maximum length of file path.
** The value is same as MAX_PATHNAME defined in os_unix.c.
*/
#define MAX_PATH_LEN 512

#define THREAD_LOCAL __thread

typedef pid_t PID;  /* Data type of process ID */
typedef pid_t TID;  /* Data type of thread ID */
typedef struct MUTEX_HANDLE {
  pthread_mutex_t handle;
  int held; /* True if I have the mutex. */
  int init; /* 1 if mutex is initialized. */
} MUTEX_HANDLE;
typedef struct MMAP_HANDLE {
  int fdMmap; /* File descriptor of MMAP */
  int fdMng;  /* File descriptor of management file */
  char name[MAX_PATH_LEN]; /* MMAP name */
  size_t size; /* MMAP size. */
} MMAP_HANDLE;

/* The suffix of MMAP management file. */
#define MMAP_MNG_FILE_SUFFIX "_MNG"

#define xSnprintf snprintf
#define rowlockGetPid getpid
#define rowlockGetTid gettid

/*
** Flags indicating user of which opens a file.
** They are used in fileUser().
**   OPEN_NOME : No one opens the file.
**   OPEN_ME   : Other processes are opening the file.
**   OPEN_OTHER: I am opening the file.
*/
#define OPEN_NONE  0x0
#define OPEN_ME    0x1
#define OPEN_OTHER 0x2

#endif /* SQLITE_ROWLOCK_OS_LINUX_H */
#endif /* SQLITE_OS_UNIX */
#endif /* SQLITE_OMIT_ROWLOCK */
