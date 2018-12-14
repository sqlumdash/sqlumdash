/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file implements Linux dependent functions used by row lock
** feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#if SQLITE_OS_UNIX

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "rowlock_os.h"
#include "sqlite3.h"

/* Set a management file name into pOut from MMAP name which is specified by pIn. */
#define SET_MANAGEMENT_FILE_NAME(pIn, pOut) \
  do { \
    strcpy(pOut, pIn); \
    strcat(pOut, MMAP_MNG_FILE_SUFFIX); \
  }while(0)

int rowlockOsSetSignalAction(int *signals, int nSignal, void *action){
  int i;
  struct sigaction sigact = {0};

  sigact.sa_sigaction = action;
  sigact.sa_flags = SA_SIGINFO;

  for( i=0; i<nSignal; i++){
    int ret = sigaction(signals[i], &sigact, NULL);
    if( ret!=0 ){
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

int rowlockOsMutexOpen(char *name, MUTEX_HANDLE *pMutex){
  int ret;
  pthread_mutexattr_t mtxattr;

  pthread_mutexattr_init(&mtxattr);

  ret = pthread_mutexattr_settype(&mtxattr, PTHREAD_MUTEX_RECURSIVE);
  if( ret!=0 ){
    pthread_mutexattr_destroy(&mtxattr);
    return SQLITE_ERROR;
  }

  /* Enable to use mutex for sharing between processes */
  ret = pthread_mutexattr_setpshared(&mtxattr, PTHREAD_PROCESS_SHARED);
  if( ret!=0 ){
    pthread_mutexattr_destroy(&mtxattr);
    return SQLITE_ERROR;
  }

  pthread_mutex_init(&pMutex->handle, &mtxattr);
  pthread_mutexattr_destroy(&mtxattr);

  return SQLITE_OK;
}

void rowlockOsMutexClose(MUTEX_HANDLE *pMutex){
  pthread_mutex_destroy(&pMutex->handle);
  pMutex->held = 0;
}

void rowlockOsMutexEnter(MUTEX_HANDLE *pMutex){
  pthread_mutex_lock(&pMutex->handle);
  pMutex->held = 1;
}

void rowlockOsMutexLeave(MUTEX_HANDLE *pMutex){
  pMutex->held = 0;
  pthread_mutex_unlock(&pMutex->handle);
}

int rowlockOsMutexHeld(MUTEX_HANDLE *pMutex){
  return pMutex->held;
}

/*
** Check if file is opend by the other process.
** pUser is set the following flags.
** OPEN_NOME, OPEN_ME, OPEN_OTHER or OPEN_ME|OPEN_OTHER
*/
int rowlockOsFileUser(const char *name, int *pUser){
  FILE *fd;
  char cmd[BUFSIZ] = {0};
  char buf[BUFSIZ] = {0};
  char *ret;
  int user = OPEN_NONE;
  
  assert( pUser );
  /* The header string of fuser command is output to stderr. So we throw away it. */
  snprintf(cmd, sizeof(cmd), "fuser %s 2>/dev/null", name);
  fd = popen(cmd, "r");
  if( fd==NULL ) return SQLITE_CANTOPEN_BKPT;

  while( fgets(buf, sizeof(buf), fd)!=NULL ){
    char *p = buf;
    int pid;
    /* Trim spaces. */
    while( *p==' ') p++;
    pid = atoi(p);
    if( pid<=0 ) continue;
    /* Check if it is me or others. */
    if( pid==getpid() ){
      user |= OPEN_ME;
    }else{
      user |= OPEN_OTHER;
    }
  }

  pclose(fd);
  *pUser = user;
  return SQLITE_OK;
}

/* Get file size from file descriptor. */
static int getFileSize(int fd, long *size){
  FILE *fp;
  int ret;
  struct stat st;
  
  assert( size );

  /*
  ** fclose() closes not only file pointer but also file descriptor.
  ** So I duplicate the file descripter and use it for fdopen().
  */
  fp = fdopen(dup(fd), "rb");
  if( !fp ) return SQLITE_CANTOPEN_BKPT;

  ret = fstat(fd, &st);
  fclose(fp);
  if( ret==-1 ) return SQLITE_CANTOPEN_BKPT;

  *size = st.st_size;

  return SQLITE_OK;
}

/* Stretch file size. */
static int stretchFileSize(int fd, off_t size){
  int ret;
  char c = 0;

  ret = lseek(fd, size, SEEK_SET);
  if( ret==-1 ) return SQLITE_IOERR_SEEK;

  ret = write(fd, &c, sizeof(char));
  if( ret==-1 ) return SQLITE_IOERR_WRITE;

  return SQLITE_OK;
}

/*
** Open memory mapped file.
** We delete the file when closing if no one is using it.
** In order to realize it, we open one more file for access management.
** When closing MMAP, we check the user of management file by "fuser" command.
** This is because "fuser" returns users of memory mapped file in spite of 
** calling munmap() and close(), as we experimented.
*/
int rowlockOsMmapOpen(u64 allocSize, char *name, MMAP_HANDLE *phMap, void **ppMap){
  int fdMmap;
  int fdMng;
  char mngName[BUFSIZ] = {0};
  int rc;
  long fileSize;
  void *pMap;
  
  /* Open file for memory mapped file. */
  fdMmap = open(name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if( fdMmap==-1 ) return SQLITE_CANTOPEN_BKPT;

  /* Open file for access management. */
  SET_MANAGEMENT_FILE_NAME(name, mngName);
  fdMng = open(mngName, O_RDONLY | O_CREAT, S_IRUSR | S_IWUSR);
  if( fdMng==-1 ){
    rc = SQLITE_CANTOPEN_BKPT;
    goto mmap_open_error1;
  }

  /* Stretch the file size. */
  rc = getFileSize(fdMmap, &fileSize);
  if( rc ) goto mmap_open_error2;
  if( fileSize<allocSize ){
    int ret;
    rc = stretchFileSize(fdMmap, allocSize);
    if( rc ) goto mmap_open_error2;
    /* Reset the location. */
    ret = lseek(fdMmap, 0, SEEK_SET);
    if( ret==-1 ){
      rc = SQLITE_IOERR_SEEK;
      goto mmap_open_error2;
    }
  }

  /* Mapping */
  /* ToDo: allocSize>INT_MAX */
  pMap = mmap(NULL, allocSize, PROT_READ | PROT_WRITE, MAP_SHARED, fdMmap, 0);
  if( pMap==MAP_FAILED ){
    rc = SQLITE_IOERR_SHMMAP;
    goto mmap_open_error2;
  }

  phMap->fdMmap = fdMmap;
  phMap->fdMng = fdMng;
  strcpy(phMap->name, name);
  *ppMap = pMap;
  return SQLITE_OK;

mmap_open_error2:
  close(fdMng);
mmap_open_error1:
  close(fdMmap);
  return rc;
}

void rowlockOsMmapClose(MMAP_HANDLE hMap, void *pMap){
  int rc;
  int user;
  char name[BUFSIZ] = {0};

  munmap(pMap, 0);
  close(hMap.fdMmap);
  close(hMap.fdMng);

  /* Delete MMAP file if no one opens it. */
  SET_MANAGEMENT_FILE_NAME(hMap.name, name);
  rc = rowlockOsFileUser(name, &user);
  if( rc==SQLITE_OK && !(user&(OPEN_ME|OPEN_OTHER)) ){
    unlink(hMap.name);
    unlink(name);
  }
}

int rowlockOsMmapSync(void *pMap){
  msync(pMap, 0, MS_SYNC);
}

#endif /* SQLITE_OS_UNIX */
#endif /* SQLITE_OMIT_ROWLOCK */
