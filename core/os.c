/*
** 2005 November 29
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains OS interface code that is common to all
** architectures.
*/
#include "capdbInt.h"

/*
** If we compile with the CAPDB_TEST macro set, then the following block
** of code will give us the ability to simulate a disk I/O error.  This
** is used for testing the I/O recovery logic.
*/
#if defined(CAPDB_TEST)
int capdb_io_error_hit = 0;            /* Total number of I/O Errors */
int capdb_io_error_hardhit = 0;        /* Number of non-benign errors */
int capdb_io_error_pending = 0;        /* Count down to first I/O error */
int capdb_io_error_persist = 0;        /* True if I/O errors persist */
int capdb_io_error_benign = 0;         /* True if errors are benign */
int capdb_diskfull_pending = 0;
int capdb_diskfull = 0;
#endif /* defined(CAPDB_TEST) */

/*
** When testing, also keep a count of the number of open files.
*/
#if defined(CAPDB_TEST)
int capdb_open_file_count = 0;
#endif /* defined(CAPDB_TEST) */

/*
** The default SQLite capdb_vfs implementations do not allocate
** memory (actually, os_unix.c allocates a small amount of memory
** from within OsOpen()), but some third-party implementations may.
** So we test the effects of a malloc() failing and the capdbOsXXX()
** function returning CAPDB_IOERR_NOMEM using the DO_OS_MALLOC_TEST macro.
**
** The following functions are instrumented for malloc() failure
** testing:
**
**     capdbOsRead()
**     capdbOsWrite()
**     capdbOsSync()
**     capdbOsFileSize()
**     capdbOsLock()
**     capdbOsCheckReservedLock()
**     capdbOsFileControl()
**     capdbOsShmMap()
**     capdbOsOpen()
**     capdbOsDelete()
**     capdbOsAccess()
**     capdbOsFullPathname()
**
*/
#if defined(CAPDB_TEST)
int capdb_memdebug_vfs_oom_test = 1;
  #define DO_OS_MALLOC_TEST(x)                                       \
  if (capdb_memdebug_vfs_oom_test && (!x || !capdbJournalIsInMemory(x))) { \
    void *pTstAlloc = capdbMalloc(10);                             \
    if (!pTstAlloc) return CAPDB_IOERR_NOMEM_BKPT;                  \
    capdb_free(pTstAlloc);                                         \
  }
#else
  #define DO_OS_MALLOC_TEST(x)
#endif

/*
** The following routines are convenience wrappers around methods
** of the capdb_file object.  This is mostly just syntactic sugar. All
** of this would be completely automatic if SQLite were coded using
** C++ instead of plain old C.
*/
void capdbOsClose(capdb_file *pId){
  if( pId->pMethods ){
    pId->pMethods->xClose(pId);
    pId->pMethods = 0;
  }
}
int capdbOsRead(capdb_file *id, void *pBuf, int amt, i64 offset){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xRead(id, pBuf, amt, offset);
}
int capdbOsWrite(capdb_file *id, const void *pBuf, int amt, i64 offset){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xWrite(id, pBuf, amt, offset);
}
int capdbOsTruncate(capdb_file *id, i64 size){
  return id->pMethods->xTruncate(id, size);
}
int capdbOsSync(capdb_file *id, int flags){
  DO_OS_MALLOC_TEST(id);
  return flags ? id->pMethods->xSync(id, flags) : CAPDB_OK;
}
int capdbOsFileSize(capdb_file *id, i64 *pSize){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xFileSize(id, pSize);
}
int capdbOsLock(capdb_file *id, int lockType){
  DO_OS_MALLOC_TEST(id);
  assert( lockType>=CAPDB_LOCK_SHARED && lockType<=CAPDB_LOCK_EXCLUSIVE );
  return id->pMethods->xLock(id, lockType);
}
int capdbOsUnlock(capdb_file *id, int lockType){
  assert( lockType==CAPDB_LOCK_NONE || lockType==CAPDB_LOCK_SHARED );
  return id->pMethods->xUnlock(id, lockType);
}
int capdbOsCheckReservedLock(capdb_file *id, int *pResOut){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xCheckReservedLock(id, pResOut);
}

/*
** Use capdbOsFileControl() when we are doing something that might fail
** and we need to know about the failures.  Use capdbOsFileControlHint()
** when simply tossing information over the wall to the VFS and we do not
** really care if the VFS receives and understands the information since it
** is only a hint and can be safely ignored.  The capdbOsFileControlHint()
** routine has no return value since the return value would be meaningless.
*/
int capdbOsFileControl(capdb_file *id, int op, void *pArg){
  if( id->pMethods==0 ) return CAPDB_NOTFOUND;
#ifdef CAPDB_TEST
  if( op!=CAPDB_FCNTL_COMMIT_PHASETWO
   && op!=CAPDB_FCNTL_LOCK_TIMEOUT
   && op!=CAPDB_FCNTL_CKPT_DONE
   && op!=CAPDB_FCNTL_CKPT_START
  ){
    /* Faults are not injected into COMMIT_PHASETWO because, assuming SQLite
    ** is using a regular VFS, it is called after the corresponding
    ** transaction has been committed. Injecting a fault at this point
    ** confuses the test scripts - the COMMIT command returns CAPDB_NOMEM
    ** but the transaction is committed anyway.
    **
    ** The core must call OsFileControl() though, not OsFileControlHint(),
    ** as if a custom VFS (e.g. zipvfs) returns an error here, it probably
    ** means the commit really has failed and an error should be returned
    ** to the user.
    **
    ** The CKPT_DONE and CKPT_START file-controls are write-only signals
    ** to the cksumvfs.  Their return code is meaningless and is ignored
    ** by the SQLite core, so there is no point in simulating OOMs for them.
    */
    DO_OS_MALLOC_TEST(id);
  }
#endif
  return id->pMethods->xFileControl(id, op, pArg);
}
void capdbOsFileControlHint(capdb_file *id, int op, void *pArg){
  if( id->pMethods ) (void)id->pMethods->xFileControl(id, op, pArg);
}

int capdbOsSectorSize(capdb_file *id){
  int (*xSectorSize)(capdb_file*) = id->pMethods->xSectorSize;
  return (xSectorSize ? xSectorSize(id) : CAPDB_DEFAULT_SECTOR_SIZE);
}
int capdbOsDeviceCharacteristics(capdb_file *id){
  if( NEVER(id->pMethods==0) ) return 0;
  return id->pMethods->xDeviceCharacteristics(id);
}
#ifndef CAPDB_OMIT_WAL
int capdbOsShmLock(capdb_file *id, int offset, int n, int flags){
  return id->pMethods->xShmLock(id, offset, n, flags);
}
void capdbOsShmBarrier(capdb_file *id){
  id->pMethods->xShmBarrier(id);
}
int capdbOsShmUnmap(capdb_file *id, int deleteFlag){
  return id->pMethods->xShmUnmap(id, deleteFlag);
}
int capdbOsShmMap(
  capdb_file *id,               /* Database file handle */
  int iPage,
  int pgsz,
  int bExtend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Pointer to mapping */
){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xShmMap(id, iPage, pgsz, bExtend, pp);
}
#endif /* CAPDB_OMIT_WAL */

#if CAPDB_MAX_MMAP_SIZE>0
/* The real implementation of xFetch and xUnfetch */
int capdbOsFetch(capdb_file *id, i64 iOff, int iAmt, void **pp){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xFetch(id, iOff, iAmt, pp);
}
int capdbOsUnfetch(capdb_file *id, i64 iOff, void *p){
  return id->pMethods->xUnfetch(id, iOff, p);
}
#else
/* No-op stubs to use when memory-mapped I/O is disabled */
int capdbOsFetch(capdb_file *id, i64 iOff, int iAmt, void **pp){
  *pp = 0;
  return CAPDB_OK;
}
int capdbOsUnfetch(capdb_file *id, i64 iOff, void *p){
  return CAPDB_OK;
}
#endif

/*
** The next group of routines are convenience wrappers around the
** VFS methods.
*/
int capdbOsOpen(
  capdb_vfs *pVfs,
  const char *zPath,
  capdb_file *pFile,
  int flags,
  int *pFlagsOut
){
  int rc;
  DO_OS_MALLOC_TEST(0);
  /* 0x87f7f is a mask of CAPDB_OPEN_ flags that are valid to be passed
  ** down into the VFS layer.  Some CAPDB_OPEN_ flags (for example,
  ** CAPDB_OPEN_FULLMUTEX or CAPDB_OPEN_SHAREDCACHE) are blocked before
  ** reaching the VFS. */
  assert( zPath || (flags & CAPDB_OPEN_EXCLUSIVE) );
  rc = pVfs->xOpen(pVfs, zPath, pFile, flags & 0x1087f7f, pFlagsOut);
  assert( rc==CAPDB_OK || pFile->pMethods==0 );
  return rc;
}
int capdbOsDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  DO_OS_MALLOC_TEST(0);
  assert( dirSync==0 || dirSync==1 );
  return pVfs->xDelete!=0 ? pVfs->xDelete(pVfs, zPath, dirSync) : CAPDB_OK;
}
int capdbOsAccess(
  capdb_vfs *pVfs,
  const char *zPath,
  int flags,
  int *pResOut
){
  DO_OS_MALLOC_TEST(0);
  return pVfs->xAccess(pVfs, zPath, flags, pResOut);
}
int capdbOsFullPathname(
  capdb_vfs *pVfs,
  const char *zPath,
  int nPathOut,
  char *zPathOut
){
  DO_OS_MALLOC_TEST(0);
  zPathOut[0] = 0;
  return pVfs->xFullPathname(pVfs, zPath, nPathOut, zPathOut);
}
#ifndef CAPDB_OMIT_LOAD_EXTENSION
void *capdbOsDlOpen(capdb_vfs *pVfs, const char *zPath){
  assert( zPath!=0 );
  assert( strlen(zPath)<=CAPDB_MAX_PATHLEN );  /* tag-20210611-1 */
  return pVfs->xDlOpen(pVfs, zPath);
}
void capdbOsDlError(capdb_vfs *pVfs, int nByte, char *zBufOut){
  pVfs->xDlError(pVfs, nByte, zBufOut);
}
void (*capdbOsDlSym(capdb_vfs *pVfs, void *pHdle, const char *zSym))(void){
  return pVfs->xDlSym(pVfs, pHdle, zSym);
}
void capdbOsDlClose(capdb_vfs *pVfs, void *pHandle){
  pVfs->xDlClose(pVfs, pHandle);
}
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
int capdbOsRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  if( capdbConfig.iPrngSeed ){
    memset(zBufOut, 0, nByte);
    if( ALWAYS(nByte>(signed)sizeof(unsigned)) ) nByte = sizeof(unsigned int);
    memcpy(zBufOut, &capdbConfig.iPrngSeed, nByte);
    return CAPDB_OK;
  }else{
    return pVfs->xRandomness(pVfs, nByte, zBufOut);
  }

}
int capdbOsSleep(capdb_vfs *pVfs, int nMicro){
  return pVfs->xSleep(pVfs, nMicro);
}
int capdbOsGetLastError(capdb_vfs *pVfs){
  return pVfs->xGetLastError ? pVfs->xGetLastError(pVfs, 0, 0) : 0;
}
int capdbOsCurrentTimeInt64(capdb_vfs *pVfs, capdb_int64 *pTimeOut){
  int rc;
  /* IMPLEMENTATION-OF: R-49045-42493 SQLite will use the xCurrentTimeInt64()
  ** method to get the current date and time if that method is available
  ** (if iVersion is 2 or greater and the function pointer is not NULL) and
  ** will fall back to xCurrentTime() if xCurrentTimeInt64() is
  ** unavailable.
  */
  if( pVfs->iVersion>=2 && pVfs->xCurrentTimeInt64 ){
    rc = pVfs->xCurrentTimeInt64(pVfs, pTimeOut);
  }else{
    double r;
    rc = pVfs->xCurrentTime(pVfs, &r);
    *pTimeOut = (capdb_int64)(r*86400000.0);
  }
  return rc;
}

int capdbOsOpenMalloc(
  capdb_vfs *pVfs,
  const char *zFile,
  capdb_file **ppFile,
  int flags,
  int *pOutFlags
){
  int rc;
  capdb_file *pFile;
  pFile = (capdb_file *)capdbMallocZero(pVfs->szOsFile);
  if( pFile ){
    rc = capdbOsOpen(pVfs, zFile, pFile, flags, pOutFlags);
    if( rc!=CAPDB_OK ){
      capdb_free(pFile);
      *ppFile = 0;
    }else{
      *ppFile = pFile;
    }
  }else{
    *ppFile = 0;
    rc = CAPDB_NOMEM_BKPT;
  }
  assert( *ppFile!=0 || rc!=CAPDB_OK );
  return rc;
}
void capdbOsCloseFree(capdb_file *pFile){
  assert( pFile );
  capdbOsClose(pFile);
  capdb_free(pFile);
}

/*
** This function is a wrapper around the OS specific implementation of
** capdb_os_init(). The purpose of the wrapper is to provide the
** ability to simulate a malloc failure, so that the handling of an
** error in capdb_os_init() by the upper layers can be tested.
*/
int capdbOsInit(void){
  void *p = capdb_malloc(10);
  if( p==0 ) return CAPDB_NOMEM_BKPT;
  capdb_free(p);
  return capdb_os_init();
}

/*
** The list of all registered VFS implementations.
*/
static capdb_vfs * CAPDB_WSD vfsList = 0;
#define vfsList GLOBAL(capdb_vfs *, vfsList)

/*
** Locate a VFS by name.  If no name is given, simply return the
** first VFS on the list.
*/
capdb_vfs *capdb_vfs_find(const char *zVfs){
  capdb_vfs *pVfs = 0;
#if CAPDB_THREADSAFE
  capdb_mutex *mutex;
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  int rc = capdb_initialize();
  if( rc ) return 0;
#endif
#if CAPDB_THREADSAFE
  mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN);
#endif
  capdb_mutex_enter(mutex);
  for(pVfs = vfsList; pVfs; pVfs=pVfs->pNext){
    if( zVfs==0 ) break;
    if( strcmp(zVfs, pVfs->zName)==0 ) break;
  }
  capdb_mutex_leave(mutex);
  return pVfs;
}

/*
** Unlink a VFS from the linked list
*/
static void vfsUnlink(capdb_vfs *pVfs){
  assert( capdb_mutex_held(capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN)) );
  if( pVfs==0 ){
    /* No-op */
  }else if( vfsList==pVfs ){
    vfsList = pVfs->pNext;
  }else if( vfsList ){
    capdb_vfs *p = vfsList;
    while( p->pNext && p->pNext!=pVfs ){
      p = p->pNext;
    }
    if( p->pNext==pVfs ){
      p->pNext = pVfs->pNext;
    }
  }
}

/*
** Register a VFS with the system.  It is harmless to register the same
** VFS multiple times.  The new VFS becomes the default if makeDflt is
** true.
*/
int capdb_vfs_register(capdb_vfs *pVfs, int makeDflt){
  MUTEX_LOGIC(capdb_mutex *mutex;)
#ifndef CAPDB_OMIT_AUTOINIT
  int rc = capdb_initialize();
  if( rc ) return rc;
#endif
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pVfs==0 ) return CAPDB_MISUSE_BKPT;
#endif

  MUTEX_LOGIC( mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN); )
  capdb_mutex_enter(mutex);
  vfsUnlink(pVfs);
  if( makeDflt || vfsList==0 ){
    pVfs->pNext = vfsList;
    vfsList = pVfs;
  }else{
    pVfs->pNext = vfsList->pNext;
    vfsList->pNext = pVfs;
  }
  assert(vfsList);
  capdb_mutex_leave(mutex);
  return CAPDB_OK;
}

/*
** Unregister a VFS so that it is no longer accessible.
*/
int capdb_vfs_unregister(capdb_vfs *pVfs){
  MUTEX_LOGIC(capdb_mutex *mutex;)
#ifndef CAPDB_OMIT_AUTOINIT
  int rc = capdb_initialize();
  if( rc ) return rc;
#endif
  MUTEX_LOGIC( mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN); )
  capdb_mutex_enter(mutex);
  vfsUnlink(pVfs);
  capdb_mutex_leave(mutex);
  return CAPDB_OK;
}
