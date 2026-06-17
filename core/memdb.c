/*
** 2016-09-07
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
** This file implements an in-memory VFS. A database is held as a contiguous
** block of memory.
**
** This file also implements interface capdb_serialize() and
** capdb_deserialize().
*/
#include "capdbInt.h"
#ifndef CAPDB_OMIT_DESERIALIZE

/*
** Forward declaration of objects used by this utility
*/
typedef struct capdb_vfs MemVfs;
typedef struct MemFile MemFile;
typedef struct MemStore MemStore;

/* Access to a lower-level VFS that (might) implement dynamic loading,
** access to randomness, etc.
*/
#define ORIGVFS(p) ((capdb_vfs*)((p)->pAppData))

/* Storage for a memdb file.
**
** An memdb object can be shared or separate.  Shared memdb objects can be
** used by more than one database connection.  Mutexes are used by shared
** memdb objects to coordinate access.  Separate memdb objects are only
** connected to a single database connection and do not require additional
** mutexes.
**
** Shared memdb objects have .zFName!=0 and .pMutex!=0.  They are created
** using "file:/name?vfs=memdb".  The first character of the name must be
** "/" or else the object will be a separate memdb object.  All shared
** memdb objects are stored in memdb_g.apMemStore[] in an arbitrary order.
**
** Separate memdb objects are created using a name that does not begin
** with "/" or using capdb_deserialize().
**
** Access rules for shared MemStore objects:
**
**   *  .zFName is initialized when the object is created and afterwards
**      is unchanged until the object is destroyed.  So it can be accessed
**      at any time as long as we know the object is not being destroyed,
**      which means while either the CAPDB_MUTEX_STATIC_VFS1 or
**      .pMutex is held or the object is not part of memdb_g.apMemStore[].
**
**   *  Can .pMutex can only be changed while holding the 
**      CAPDB_MUTEX_STATIC_VFS1 mutex or while the object is not part
**      of memdb_g.apMemStore[].
**
**   *  Other fields can only be changed while holding the .pMutex mutex
**      or when the .nRef is less than zero and the object is not part of
**      memdb_g.apMemStore[].
**
**   *  The .aData pointer has the added requirement that it can can only
**      be changed (for resizing) when nMmap is zero.
**      
*/
struct MemStore {
  capdb_int64 sz;               /* Size of the file */
  capdb_int64 szAlloc;          /* Space allocated to aData */
  capdb_int64 szMax;            /* Maximum allowed size of the file */
  unsigned char *aData;           /* content of the file */
  capdb_mutex *pMutex;          /* Used by shared stores only */
  int nMmap;                      /* Number of memory mapped pages */
  unsigned mFlags;                /* Flags */
  int nRdLock;                    /* Number of readers */
  int nWrLock;                    /* Number of writers.  (Always 0 or 1) */
  int nRef;                       /* Number of users of this MemStore */
  char *zFName;                   /* The filename for shared stores */
};

/* An open file */
struct MemFile {
  capdb_file base;              /* IO methods */
  MemStore *pStore;               /* The storage */
  int eLock;                      /* Most recent lock against this file */
};

/*
** File-scope variables for holding the memdb files that are accessible
** to multiple database connections in separate threads.
**
** Must hold CAPDB_MUTEX_STATIC_VFS1 to access any part of this object.
*/
static struct MemFS {
  int nMemStore;                  /* Number of shared MemStore objects */
  MemStore **apMemStore;          /* Array of all shared MemStore objects */
} memdb_g;

/*
** Methods for MemFile
*/
static int memdbClose(capdb_file*);
static int memdbRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int memdbWrite(capdb_file*,const void*,int iAmt, capdb_int64 iOfst);
static int memdbTruncate(capdb_file*, capdb_int64 size);
static int memdbSync(capdb_file*, int flags);
static int memdbFileSize(capdb_file*, capdb_int64 *pSize);
static int memdbLock(capdb_file*, int);
static int memdbUnlock(capdb_file*, int);
/* static int memdbCheckReservedLock(capdb_file*, int *pResOut);// not used */
static int memdbFileControl(capdb_file*, int op, void *pArg);
/* static int memdbSectorSize(capdb_file*); // not used */
static int memdbDeviceCharacteristics(capdb_file*);
static int memdbFetch(capdb_file*, capdb_int64 iOfst, int iAmt, void **pp);
static int memdbUnfetch(capdb_file*, capdb_int64 iOfst, void *p);

/*
** Methods for MemVfs
*/
static int memdbOpen(capdb_vfs*, const char *, capdb_file*, int , int *);
/* static int memdbDelete(capdb_vfs*, const char *zName, int syncDir); */
static int memdbAccess(capdb_vfs*, const char *zName, int flags, int *);
static int memdbFullPathname(capdb_vfs*, const char *zName, int, char *zOut);
static void *memdbDlOpen(capdb_vfs*, const char *zFilename);
static void memdbDlError(capdb_vfs*, int nByte, char *zErrMsg);
static void (*memdbDlSym(capdb_vfs *pVfs, void *p, const char*zSym))(void);
static void memdbDlClose(capdb_vfs*, void*);
static int memdbRandomness(capdb_vfs*, int nByte, char *zOut);
static int memdbSleep(capdb_vfs*, int microseconds);
/* static int memdbCurrentTime(capdb_vfs*, double*); */
static int memdbGetLastError(capdb_vfs*, int, char *);
static int memdbCurrentTimeInt64(capdb_vfs*, capdb_int64*);

static capdb_vfs memdb_vfs = {
  2,                           /* iVersion */
  0,                           /* szOsFile (set when registered) */
  1024,                        /* mxPathname */
  0,                           /* pNext */
  "memdb",                     /* zName */
  0,                           /* pAppData (set when registered) */ 
  memdbOpen,                   /* xOpen */
  0, /* memdbDelete, */        /* xDelete */
  memdbAccess,                 /* xAccess */
  memdbFullPathname,           /* xFullPathname */
  memdbDlOpen,                 /* xDlOpen */
  memdbDlError,                /* xDlError */
  memdbDlSym,                  /* xDlSym */
  memdbDlClose,                /* xDlClose */
  memdbRandomness,             /* xRandomness */
  memdbSleep,                  /* xSleep */
  0, /* memdbCurrentTime, */   /* xCurrentTime */
  memdbGetLastError,           /* xGetLastError */
  memdbCurrentTimeInt64,       /* xCurrentTimeInt64 */
  0,                           /* xSetSystemCall */
  0,                           /* xGetSystemCall */
  0,                           /* xNextSystemCall */
};

static const capdb_io_methods memdb_io_methods = {
  3,                              /* iVersion */
  memdbClose,                      /* xClose */
  memdbRead,                       /* xRead */
  memdbWrite,                      /* xWrite */
  memdbTruncate,                   /* xTruncate */
  memdbSync,                       /* xSync */
  memdbFileSize,                   /* xFileSize */
  memdbLock,                       /* xLock */
  memdbUnlock,                     /* xUnlock */
  0, /* memdbCheckReservedLock, */ /* xCheckReservedLock */
  memdbFileControl,                /* xFileControl */
  0, /* memdbSectorSize,*/         /* xSectorSize */
  memdbDeviceCharacteristics,      /* xDeviceCharacteristics */
  0,                               /* xShmMap */
  0,                               /* xShmLock */
  0,                               /* xShmBarrier */
  0,                               /* xShmUnmap */
  memdbFetch,                      /* xFetch */
  memdbUnfetch                     /* xUnfetch */
};

/*
** Enter/leave the mutex on a MemStore
*/
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE==0
static void memdbEnter(MemStore *p){
  UNUSED_PARAMETER(p);
}
static void memdbLeave(MemStore *p){
  UNUSED_PARAMETER(p);
}
#else
static void memdbEnter(MemStore *p){
  capdb_mutex_enter(p->pMutex);
}
static void memdbLeave(MemStore *p){
  capdb_mutex_leave(p->pMutex);
}
#endif



/*
** Close an memdb-file.
** Free the underlying MemStore object when its refcount drops to zero
** or less.
*/
static int memdbClose(capdb_file *pFile){
  MemStore *p = ((MemFile*)pFile)->pStore;
  if( p->zFName ){
    int i;
#ifndef CAPDB_MUTEX_OMIT
    capdb_mutex *pVfsMutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_VFS1);
#endif
    capdb_mutex_enter(pVfsMutex);
    for(i=0; ALWAYS(i<memdb_g.nMemStore); i++){
      if( memdb_g.apMemStore[i]==p ){
        memdbEnter(p);
        if( p->nRef==1 ){
          memdb_g.apMemStore[i] = memdb_g.apMemStore[--memdb_g.nMemStore];
          if( memdb_g.nMemStore==0 ){
            capdb_free(memdb_g.apMemStore);
            memdb_g.apMemStore = 0;
          }
        }
        break;
      }
    }
    capdb_mutex_leave(pVfsMutex);
  }else{
    memdbEnter(p);
  }
  p->nRef--;
  if( p->nRef<=0 ){
    if( p->mFlags & CAPDB_DESERIALIZE_FREEONCLOSE ){
      capdb_free(p->aData);
    }
    memdbLeave(p);
    capdb_mutex_free(p->pMutex);
    capdb_free(p);
  }else{
    memdbLeave(p);
  }
  return CAPDB_OK;
}

/*
** Read data from an memdb-file.
*/
static int memdbRead(
  capdb_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  MemStore *p = ((MemFile*)pFile)->pStore;
  memdbEnter(p);
  if( iOfst+iAmt>p->sz ){
    memset(zBuf, 0, iAmt);
    if( iOfst<p->sz ) memcpy(zBuf, p->aData+iOfst, p->sz - iOfst);
    memdbLeave(p);
    return CAPDB_IOERR_SHORT_READ;
  }
  memcpy(zBuf, p->aData+iOfst, iAmt);
  memdbLeave(p);
  return CAPDB_OK;
}

/*
** Try to enlarge the memory allocation to hold at least sz bytes
*/
static int memdbEnlarge(MemStore *p, capdb_int64 newSz){
  unsigned char *pNew;
  if( (p->mFlags & CAPDB_DESERIALIZE_RESIZEABLE)==0 || NEVER(p->nMmap>0) ){
    return CAPDB_FULL;
  }
  if( newSz>p->szMax ){
    return CAPDB_FULL;
  }
  newSz *= 2;
  if( newSz>p->szMax ) newSz = p->szMax;
  pNew = capdbRealloc(p->aData, newSz);
  if( pNew==0 ) return CAPDB_IOERR_NOMEM;
  p->aData = pNew;
  p->szAlloc = newSz;
  return CAPDB_OK;
}

/*
** Write data to an memdb-file.
*/
static int memdbWrite(
  capdb_file *pFile,
  const void *z,
  int iAmt,
  sqlite_int64 iOfst
){
  MemStore *p = ((MemFile*)pFile)->pStore;
  memdbEnter(p);
  if( NEVER(p->mFlags & CAPDB_DESERIALIZE_READONLY) ){
    /* Can't happen: memdbLock() will return CAPDB_READONLY before
    ** reaching this point */
    memdbLeave(p);
    return CAPDB_IOERR_WRITE;
  }
  if( iOfst+iAmt>p->sz ){
    int rc;
    if( iOfst+iAmt>p->szAlloc
     && (rc = memdbEnlarge(p, iOfst+iAmt))!=CAPDB_OK
    ){
      memdbLeave(p);
      return rc;
    }
    if( iOfst>p->sz ) memset(p->aData+p->sz, 0, iOfst-p->sz);
    p->sz = iOfst+iAmt;
  }
  memcpy(p->aData+iOfst, z, iAmt);
  memdbLeave(p);
  return CAPDB_OK;
}

/*
** Truncate an memdb-file.
**
** In rollback mode (which is always the case for memdb, as it does not
** support WAL mode) the truncate() method is only used to reduce
** the size of a file, never to increase the size.
*/
static int memdbTruncate(capdb_file *pFile, sqlite_int64 size){
  MemStore *p = ((MemFile*)pFile)->pStore;
  int rc = CAPDB_OK;
  memdbEnter(p);
  if( size>p->sz ){
    /* This can only happen with a corrupt wal mode db */
    rc = CAPDB_CORRUPT;
  }else{
    p->sz = size; 
  }
  memdbLeave(p);
  return rc;
}

/*
** Sync an memdb-file.
*/
static int memdbSync(capdb_file *pFile, int flags){
  UNUSED_PARAMETER(pFile);
  UNUSED_PARAMETER(flags);
  return CAPDB_OK;
}

/*
** Return the current file-size of an memdb-file.
*/
static int memdbFileSize(capdb_file *pFile, sqlite_int64 *pSize){
  MemStore *p = ((MemFile*)pFile)->pStore;
  memdbEnter(p);
  *pSize = p->sz;
  memdbLeave(p);
  return CAPDB_OK;
}

/*
** Lock an memdb-file.
*/
static int memdbLock(capdb_file *pFile, int eLock){
  MemFile *pThis = (MemFile*)pFile;
  MemStore *p = pThis->pStore;
  int rc = CAPDB_OK;
  if( eLock<=pThis->eLock ) return CAPDB_OK;
  memdbEnter(p);

  assert( p->nWrLock==0 || p->nWrLock==1 );
  assert( pThis->eLock<=CAPDB_LOCK_SHARED || p->nWrLock==1 );
  assert( pThis->eLock==CAPDB_LOCK_NONE || p->nRdLock>=1 );

  if( eLock>CAPDB_LOCK_SHARED && (p->mFlags & CAPDB_DESERIALIZE_READONLY) ){
    rc = CAPDB_READONLY;
  }else{
    switch( eLock ){
      case CAPDB_LOCK_SHARED: {
        assert( pThis->eLock==CAPDB_LOCK_NONE );
        if( p->nWrLock>0 ){
          rc = CAPDB_BUSY;
        }else{
          p->nRdLock++;
        }
        break;
      };
  
      case CAPDB_LOCK_RESERVED:
      case CAPDB_LOCK_PENDING: {
        assert( pThis->eLock>=CAPDB_LOCK_SHARED );
        if( ALWAYS(pThis->eLock==CAPDB_LOCK_SHARED) ){
          if( p->nWrLock>0 ){
            rc = CAPDB_BUSY;
          }else{
            p->nWrLock = 1;
          }
        }
        break;
      }
  
      default: {
        assert(  eLock==CAPDB_LOCK_EXCLUSIVE );
        assert( pThis->eLock>=CAPDB_LOCK_SHARED );
        if( p->nRdLock>1 ){
          rc = CAPDB_BUSY;
        }else if( pThis->eLock==CAPDB_LOCK_SHARED ){
          p->nWrLock = 1;
        }
        break;
      }
    }
  }
  if( rc==CAPDB_OK ) pThis->eLock = eLock;
  memdbLeave(p);
  return rc;
}

/*
** Unlock an memdb-file.
*/
static int memdbUnlock(capdb_file *pFile, int eLock){
  MemFile *pThis = (MemFile*)pFile;
  MemStore *p = pThis->pStore;
  if( eLock>=pThis->eLock ) return CAPDB_OK;
  memdbEnter(p);

  assert( eLock==CAPDB_LOCK_SHARED || eLock==CAPDB_LOCK_NONE );
  if( eLock==CAPDB_LOCK_SHARED ){
    if( ALWAYS(pThis->eLock>CAPDB_LOCK_SHARED) ){
      p->nWrLock--;
    }
  }else{
    if( pThis->eLock>CAPDB_LOCK_SHARED ){
      p->nWrLock--;
    }
    p->nRdLock--;
  }

  pThis->eLock = eLock;
  memdbLeave(p);
  return CAPDB_OK;
}

#if 0
/*
** This interface is only used for crash recovery, which does not
** occur on an in-memory database.
*/
static int memdbCheckReservedLock(capdb_file *pFile, int *pResOut){
  *pResOut = 0;
  return CAPDB_OK;
}
#endif


/*
** File control method. For custom operations on an memdb-file.
*/
static int memdbFileControl(capdb_file *pFile, int op, void *pArg){
  MemStore *p = ((MemFile*)pFile)->pStore;
  int rc = CAPDB_NOTFOUND;
  memdbEnter(p);
  if( op==CAPDB_FCNTL_VFSNAME ){
    *(char**)pArg = capdb_mprintf("memdb(%p,%lld)", p->aData, p->sz);
    rc = CAPDB_OK;
  }
  if( op==CAPDB_FCNTL_SIZE_LIMIT ){
    capdb_int64 iLimit = *(capdb_int64*)pArg;
    if( iLimit<p->sz ){
      if( iLimit<0 ){
        iLimit = p->szMax;
      }else{
        iLimit = p->sz;
      }
    }
    p->szMax = iLimit;
    *(capdb_int64*)pArg = iLimit;
    rc = CAPDB_OK;
  }
  memdbLeave(p);
  return rc;
}

#if 0  /* Not used because of CAPDB_IOCAP_POWERSAFE_OVERWRITE */
/*
** Return the sector-size in bytes for an memdb-file.
*/
static int memdbSectorSize(capdb_file *pFile){
  return 1024;
}
#endif

/*
** Return the device characteristic flags supported by an memdb-file.
*/
static int memdbDeviceCharacteristics(capdb_file *pFile){
  UNUSED_PARAMETER(pFile);
  return CAPDB_IOCAP_ATOMIC | 
         CAPDB_IOCAP_POWERSAFE_OVERWRITE |
         CAPDB_IOCAP_SAFE_APPEND |
         CAPDB_IOCAP_SEQUENTIAL;
}

/* Fetch a page of a memory-mapped file */
static int memdbFetch(
  capdb_file *pFile,
  capdb_int64 iOfst,
  int iAmt,
  void **pp
){
  MemStore *p = ((MemFile*)pFile)->pStore;
  memdbEnter(p);
  if( iOfst+iAmt>p->sz || (p->mFlags & CAPDB_DESERIALIZE_RESIZEABLE)!=0 ){
    *pp = 0;
  }else{
    p->nMmap++;
    *pp = (void*)(p->aData + iOfst);
  }
  memdbLeave(p);
  return CAPDB_OK;
}

/* Release a memory-mapped page */
static int memdbUnfetch(capdb_file *pFile, capdb_int64 iOfst, void *pPage){
  MemStore *p = ((MemFile*)pFile)->pStore;
  UNUSED_PARAMETER(iOfst);
  UNUSED_PARAMETER(pPage);
  memdbEnter(p);
  p->nMmap--;
  memdbLeave(p);
  return CAPDB_OK;
}

/*
** Open an mem file handle.
*/
static int memdbOpen(
  capdb_vfs *pVfs,
  const char *zName,
  capdb_file *pFd,
  int flags,
  int *pOutFlags
){
  MemFile *pFile = (MemFile*)pFd;
  MemStore *p = 0;
  int szName;
  UNUSED_PARAMETER(pVfs);

  memset(pFile, 0, sizeof(*pFile));
  szName = capdbStrlen30(zName);
  if( szName>1 && (zName[0]=='/' || zName[0]=='\\') ){
    int i;
#ifndef CAPDB_MUTEX_OMIT
    capdb_mutex *pVfsMutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_VFS1);
#endif
    capdb_mutex_enter(pVfsMutex);
    for(i=0; i<memdb_g.nMemStore; i++){
      if( strcmp(memdb_g.apMemStore[i]->zFName,zName)==0 ){
        p = memdb_g.apMemStore[i];
        break;
      }
    }
    if( p==0 ){
      MemStore **apNew;
      p = capdbMalloc( sizeof(*p) + (i64)szName + 3 );
      if( p==0 ){
        capdb_mutex_leave(pVfsMutex);
        return CAPDB_NOMEM;
      }
      apNew = capdbRealloc(memdb_g.apMemStore,
                             sizeof(apNew[0])*(1+(i64)memdb_g.nMemStore) );
      if( apNew==0 ){
        capdb_free(p);
        capdb_mutex_leave(pVfsMutex);
        return CAPDB_NOMEM;
      }
      apNew[memdb_g.nMemStore++] = p;
      memdb_g.apMemStore = apNew;
      memset(p, 0, sizeof(*p));
      p->mFlags = CAPDB_DESERIALIZE_RESIZEABLE|CAPDB_DESERIALIZE_FREEONCLOSE;
      p->szMax = capdbGlobalConfig.mxMemdbSize;
      p->zFName = (char*)&p[1];
      memcpy(p->zFName, zName, szName+1);
      p->pMutex = capdb_mutex_alloc(CAPDB_MUTEX_FAST);
      if( p->pMutex==0 ){
        memdb_g.nMemStore--;
        capdb_free(p);
        capdb_mutex_leave(pVfsMutex);
        return CAPDB_NOMEM;
      }
      p->nRef = 1;
      memdbEnter(p);
    }else{
      memdbEnter(p);
      p->nRef++;
    }
    capdb_mutex_leave(pVfsMutex);
  }else{
    p = capdbMalloc( sizeof(*p) );
    if( p==0 ){
      return CAPDB_NOMEM;
    }
    memset(p, 0, sizeof(*p));
    p->mFlags = CAPDB_DESERIALIZE_RESIZEABLE | CAPDB_DESERIALIZE_FREEONCLOSE;
    p->szMax = capdbGlobalConfig.mxMemdbSize;
  }
  pFile->pStore = p;
  if( pOutFlags!=0 ){
    *pOutFlags = flags | CAPDB_OPEN_MEMORY;
  }
  pFd->pMethods = &memdb_io_methods;
  memdbLeave(p);
  return CAPDB_OK;
}

#if 0 /* Only used to delete rollback journals, super-journals, and WAL
      ** files, none of which exist in memdb.  So this routine is never used */
/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int memdbDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  return CAPDB_IOERR_DELETE;
}
#endif

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
**
** With memdb, no files ever exist on disk.  So always return false.
*/
static int memdbAccess(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  UNUSED_PARAMETER(pVfs);
  UNUSED_PARAMETER(zPath);
  UNUSED_PARAMETER(flags);
  *pResOut = 0;
  return CAPDB_OK;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (INST_MAX_PATHNAME+1) bytes.
*/
static int memdbFullPathname(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  UNUSED_PARAMETER(pVfs);
  capdb_snprintf(nOut, zOut, "%s", zPath);
  return CAPDB_OK;
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *memdbDlOpen(capdb_vfs *pVfs, const char *zPath){
  return ORIGVFS(pVfs)->xDlOpen(ORIGVFS(pVfs), zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void memdbDlError(capdb_vfs *pVfs, int nByte, char *zErrMsg){
  ORIGVFS(pVfs)->xDlError(ORIGVFS(pVfs), nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*memdbDlSym(capdb_vfs *pVfs, void *p, const char *zSym))(void){
  return ORIGVFS(pVfs)->xDlSym(ORIGVFS(pVfs), p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void memdbDlClose(capdb_vfs *pVfs, void *pHandle){
  ORIGVFS(pVfs)->xDlClose(ORIGVFS(pVfs), pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int memdbRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  return ORIGVFS(pVfs)->xRandomness(ORIGVFS(pVfs), nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int memdbSleep(capdb_vfs *pVfs, int nMicro){
  return ORIGVFS(pVfs)->xSleep(ORIGVFS(pVfs), nMicro);
}

#if 0  /* Never used.  Modern cores only call xCurrentTimeInt64() */
/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int memdbCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  return ORIGVFS(pVfs)->xCurrentTime(ORIGVFS(pVfs), pTimeOut);
}
#endif

static int memdbGetLastError(capdb_vfs *pVfs, int a, char *b){
  return ORIGVFS(pVfs)->xGetLastError(ORIGVFS(pVfs), a, b);
}
static int memdbCurrentTimeInt64(capdb_vfs *pVfs, capdb_int64 *p){
  return ORIGVFS(pVfs)->xCurrentTimeInt64(ORIGVFS(pVfs), p);
}

/*
** Translate a database connection pointer and schema name into a
** MemFile pointer.
*/
static MemFile *memdbFromDbSchema(capdb *db, const char *zSchema){
  MemFile *p = 0;
  MemStore *pStore;
  int rc = capdb_file_control(db, zSchema, CAPDB_FCNTL_FILE_POINTER, &p);
  if( rc ) return 0;
  if( p->base.pMethods!=&memdb_io_methods ) return 0;
  pStore = p->pStore;
  memdbEnter(pStore);
  if( pStore->zFName!=0 ) p = 0;
  memdbLeave(pStore);
  return p;
}

/*
** Return the serialization of a database
*/
unsigned char *capdb_serialize(
  capdb *db,              /* The database connection */
  const char *zSchema,      /* Which database within the connection */
  capdb_int64 *piSize,    /* Write size here, if not NULL */
  unsigned int mFlags       /* Maybe CAPDB_SERIALIZE_NOCOPY */
){
  MemFile *p;
  int iDb;
  Btree *pBt;
  capdb_int64 sz;
  int szPage = 0;
  capdb_stmt *pStmt = 0;
  unsigned char *pOut;
  char *zSql;
  int rc;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif

  if( zSchema==0 ) zSchema = db->aDb[0].zDbSName;
  p = memdbFromDbSchema(db, zSchema);
  iDb = capdbFindDbName(db, zSchema);
  if( piSize ) *piSize = -1;
  if( iDb<0 ) return 0;
  if( p ){
    MemStore *pStore = p->pStore;
    assert( pStore->pMutex==0 );
    if( piSize ) *piSize = pStore->sz;
    if( mFlags & CAPDB_SERIALIZE_NOCOPY ){
      pOut = pStore->aData;
    }else{
      pOut = capdb_malloc64( pStore->sz );
      if( pOut ) memcpy(pOut, pStore->aData, pStore->sz);
    }
    return pOut;
  }
  pBt = db->aDb[iDb].pBt;
  if( pBt==0 ) return 0;
  szPage = capdbBtreeGetPageSize(pBt);
  zSql = capdb_mprintf("PRAGMA \"%w\".page_count", zSchema);
  rc = zSql ? capdb_prepare_v2(db, zSql, -1, &pStmt, 0) : CAPDB_NOMEM;
  capdb_free(zSql);
  if( rc ) return 0;
  rc = capdb_step(pStmt);
  if( rc!=CAPDB_ROW ){
    pOut = 0;
  }else{
    sz = capdb_column_int64(pStmt, 0)*szPage;
    if( sz==0 ){
      capdb_reset(pStmt);
      capdb_exec(db, "BEGIN IMMEDIATE; COMMIT;", 0, 0, 0);
      rc = capdb_step(pStmt);
      if( rc==CAPDB_ROW ){
        sz = capdb_column_int64(pStmt, 0)*szPage;
      }
    }
    if( piSize ) *piSize = sz;
    if( mFlags & CAPDB_SERIALIZE_NOCOPY ){
      pOut = 0;
    }else{
      pOut = capdb_malloc64( sz );
      if( pOut ){
        int nPage = capdb_column_int(pStmt, 0);
        Pager *pPager = capdbBtreePager(pBt);
        int pgno;
        for(pgno=1; pgno<=nPage; pgno++){
          DbPage *pPage = 0;
          unsigned char *pTo = pOut + szPage*(capdb_int64)(pgno-1);
          rc = capdbPagerGet(pPager, pgno, (DbPage**)&pPage, 0);
          if( rc==CAPDB_OK ){
            memcpy(pTo, capdbPagerGetData(pPage), szPage);
          }else{
            memset(pTo, 0, szPage);
          }
          capdbPagerUnref(pPage);       
        }
      }
    }
  }
  capdb_finalize(pStmt);
  return pOut;
}

/* Convert zSchema to a MemDB and initialize its content.
*/
int capdb_deserialize(
  capdb *db,            /* The database connection */
  const char *zSchema,    /* Which DB to reopen with the deserialization */
  unsigned char *pData,   /* The serialized database content */
  capdb_int64 szDb,     /* Number bytes in the deserialization */
  capdb_int64 szBuf,    /* Total size of buffer pData[] */
  unsigned mFlags         /* Zero or more CAPDB_DESERIALIZE_* flags */
){
  MemFile *p;
  char *zSql;
  capdb_stmt *pStmt = 0;
  int rc;
  int iDb;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
  if( szDb<0 ) return CAPDB_MISUSE_BKPT;
  if( szBuf<0 ) return CAPDB_MISUSE_BKPT;
#endif

  capdb_mutex_enter(db->mutex);
  if( zSchema==0 ) zSchema = db->aDb[0].zDbSName;
  iDb = capdbFindDbName(db, zSchema);
  testcase( iDb==1 );
  if( iDb<2 && iDb!=0 ){
    rc = CAPDB_ERROR;
    goto end_deserialize;
  }
  zSql = capdb_mprintf("ATTACH x AS %Q", zSchema);
  if( zSql==0 ){
    rc = CAPDB_NOMEM;
  }else{
    rc = capdb_prepare_v2(db, zSql, -1, &pStmt, 0);
    capdb_free(zSql);
  }
  if( rc ) goto end_deserialize;
  db->init.iDb = (u8)iDb;
  db->init.reopenMemdb = 1;
  capdb_step(pStmt);
  db->init.reopenMemdb = 0;
  rc = capdb_finalize(pStmt);
  if( rc!=CAPDB_OK ){
    goto end_deserialize;
  }
  p = memdbFromDbSchema(db, zSchema);
  if( p==0 ){
    rc = CAPDB_ERROR;
  }else{
    MemStore *pStore = p->pStore;
    pStore->aData = pData;
    pData = 0;
    pStore->sz = szDb;
    pStore->szAlloc = szBuf;
    pStore->szMax = szBuf;
    if( pStore->szMax<capdbGlobalConfig.mxMemdbSize ){
      pStore->szMax = capdbGlobalConfig.mxMemdbSize;
    }
    pStore->mFlags = mFlags;
    rc = CAPDB_OK;
  }

end_deserialize:
  if( pData && (mFlags & CAPDB_DESERIALIZE_FREEONCLOSE)!=0 ){
    capdb_free(pData);
  }
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Return true if the VFS is the memvfs.
*/
int capdbIsMemdb(const capdb_vfs *pVfs){
  return pVfs==&memdb_vfs;
}

/* 
** This routine is called when the extension is loaded.
** Register the new VFS.
*/
int capdbMemdbInit(void){
  capdb_vfs *pLower = capdb_vfs_find(0);
  unsigned int sz;
  if( NEVER(pLower==0) ) return CAPDB_ERROR;
  sz = pLower->szOsFile;
  memdb_vfs.pAppData = pLower;
  /* The following conditional can only be true when compiled for
  ** Windows x86 and CAPDB_MAX_MMAP_SIZE=0.  We always leave
  ** it in, to be safe, but it is marked as NO_TEST since there
  ** is no way to reach it under most builds. */
  if( sz<sizeof(MemFile) ) sz = sizeof(MemFile); /*NO_TEST*/
  memdb_vfs.szOsFile = sz;
  return capdb_vfs_register(&memdb_vfs, 0);
}
#endif /* CAPDB_OMIT_DESERIALIZE */
