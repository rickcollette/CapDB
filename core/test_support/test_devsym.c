/*
** 2008 Jan 22
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
** This file contains code that modified the OS layer in order to simulate
** different device types (by overriding the return values of the 
** xDeviceCharacteristics() and xSectorSize() methods).
*/
#if CAPDB_TEST          /* This file is used for testing only */

#include "capdb.h"
#include "capdbInt.h"

/*
** Maximum pathname length supported by the devsym backend.
*/
#define DEVSYM_MAX_PATHNAME 512

/*
** Name used to identify this VFS.
*/
#define DEVSYM_VFS_NAME "devsym"
#define WRITECRASH_NAME "writecrash"

typedef struct devsym_file devsym_file;
struct devsym_file {
  capdb_file base;
  capdb_file *pReal;
};

/*
** Method declarations for devsym_file.
*/
static int devsymClose(capdb_file*);
static int devsymRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int devsymWrite(capdb_file*,const void*,int iAmt, capdb_int64 iOfst);
static int devsymTruncate(capdb_file*, capdb_int64 size);
static int devsymSync(capdb_file*, int flags);
static int devsymFileSize(capdb_file*, capdb_int64 *pSize);
static int devsymLock(capdb_file*, int);
static int devsymUnlock(capdb_file*, int);
static int devsymCheckReservedLock(capdb_file*, int *);
static int devsymFileControl(capdb_file*, int op, void *pArg);
static int devsymSectorSize(capdb_file*);
static int devsymDeviceCharacteristics(capdb_file*);
static int devsymShmLock(capdb_file*,int,int,int);
static int devsymShmMap(capdb_file*,int,int,int, void volatile **);
static void devsymShmBarrier(capdb_file*);
static int devsymShmUnmap(capdb_file*,int);

/*
** Method declarations for devsym_vfs.
*/
static int devsymOpen(capdb_vfs*, const char *, capdb_file*, int , int *);
static int devsymDelete(capdb_vfs*, const char *zName, int syncDir);
static int devsymAccess(capdb_vfs*, const char *zName, int flags, int *);
static int devsymFullPathname(capdb_vfs*, const char *zName, int, char *zOut);
#ifndef CAPDB_OMIT_LOAD_EXTENSION
static void *devsymDlOpen(capdb_vfs*, const char *zFilename);
static void devsymDlError(capdb_vfs*, int nByte, char *zErrMsg);
static void (*devsymDlSym(capdb_vfs*,void*, const char *zSymbol))(void);
static void devsymDlClose(capdb_vfs*, void*);
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
static int devsymRandomness(capdb_vfs*, int nByte, char *zOut);
static int devsymSleep(capdb_vfs*, int microseconds);
static int devsymCurrentTime(capdb_vfs*, double*);

struct DevsymGlobal {
  capdb_vfs *pVfs;
  int iDeviceChar;
  int iSectorSize;
  int nWriteCrash;
};
struct DevsymGlobal g = {0, 0, 512, 0};

/*
** Close an devsym-file.
*/
static int devsymClose(capdb_file *pFile){
  devsym_file *p = (devsym_file *)pFile;
  capdbOsClose(p->pReal);
  return CAPDB_OK;
}

/*
** Read data from an devsym-file.
*/
static int devsymRead(
  capdb_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsRead(p->pReal, zBuf, iAmt, iOfst);
}

/*
** Write data to an devsym-file.
*/
static int devsymWrite(
  capdb_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsWrite(p->pReal, zBuf, iAmt, iOfst);
}

/*
** Truncate an devsym-file.
*/
static int devsymTruncate(capdb_file *pFile, sqlite_int64 size){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsTruncate(p->pReal, size);
}

/*
** Sync an devsym-file.
*/
static int devsymSync(capdb_file *pFile, int flags){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsSync(p->pReal, flags);
}

/*
** Return the current file-size of an devsym-file.
*/
static int devsymFileSize(capdb_file *pFile, sqlite_int64 *pSize){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsFileSize(p->pReal, pSize);
}

/*
** Lock an devsym-file.
*/
static int devsymLock(capdb_file *pFile, int eLock){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsLock(p->pReal, eLock);
}

/*
** Unlock an devsym-file.
*/
static int devsymUnlock(capdb_file *pFile, int eLock){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsUnlock(p->pReal, eLock);
}

/*
** Check if another file-handle holds a RESERVED lock on an devsym-file.
*/
static int devsymCheckReservedLock(capdb_file *pFile, int *pResOut){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsCheckReservedLock(p->pReal, pResOut);
}

/*
** File control method. For custom operations on an devsym-file.
*/
static int devsymFileControl(capdb_file *pFile, int op, void *pArg){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsFileControl(p->pReal, op, pArg);
}

/*
** Return the sector-size in bytes for an devsym-file.
*/
static int devsymSectorSize(capdb_file *pFile){
  return g.iSectorSize;
}

/*
** Return the device characteristic flags supported by an devsym-file.
*/
static int devsymDeviceCharacteristics(capdb_file *pFile){
  return g.iDeviceChar;
}

/*
** Shared-memory methods are all pass-throughs.
*/
static int devsymShmLock(capdb_file *pFile, int ofst, int n, int flags){
  devsym_file *p = (devsym_file *)pFile;
  return p->pReal->pMethods->xShmLock(p->pReal, ofst, n, flags);
}
static int devsymShmMap(
  capdb_file *pFile, 
  int iRegion, 
  int szRegion, 
  int isWrite, 
  void volatile **pp
){
  devsym_file *p = (devsym_file *)pFile;
  return p->pReal->pMethods->xShmMap(p->pReal, iRegion, szRegion, isWrite, pp);
}
static void devsymShmBarrier(capdb_file *pFile){
  devsym_file *p = (devsym_file *)pFile;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}
static int devsymShmUnmap(capdb_file *pFile, int delFlag){
  devsym_file *p = (devsym_file *)pFile;
  return p->pReal->pMethods->xShmUnmap(p->pReal, delFlag);
}



/*
** Open an devsym file handle.
*/
static int devsymOpen(
  capdb_vfs *pVfs,
  const char *zName,
  capdb_file *pFile,
  int flags,
  int *pOutFlags
){
static capdb_io_methods devsym_io_methods = {
  2,                                /* iVersion */
  devsymClose,                      /* xClose */
  devsymRead,                       /* xRead */
  devsymWrite,                      /* xWrite */
  devsymTruncate,                   /* xTruncate */
  devsymSync,                       /* xSync */
  devsymFileSize,                   /* xFileSize */
  devsymLock,                       /* xLock */
  devsymUnlock,                     /* xUnlock */
  devsymCheckReservedLock,          /* xCheckReservedLock */
  devsymFileControl,                /* xFileControl */
  devsymSectorSize,                 /* xSectorSize */
  devsymDeviceCharacteristics,      /* xDeviceCharacteristics */
  devsymShmMap,                     /* xShmMap */
  devsymShmLock,                    /* xShmLock */
  devsymShmBarrier,                 /* xShmBarrier */
  devsymShmUnmap                    /* xShmUnmap */
};

  int rc;
  devsym_file *p = (devsym_file *)pFile;
  p->pReal = (capdb_file *)&p[1];
  rc = capdbOsOpen(g.pVfs, zName, p->pReal, flags, pOutFlags);
  if( p->pReal->pMethods ){
    pFile->pMethods = &devsym_io_methods;
  }
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int devsymDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  return capdbOsDelete(g.pVfs, zPath, dirSync);
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int devsymAccess(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  return capdbOsAccess(g.pVfs, zPath, flags, pResOut);
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int devsymFullPathname(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  return capdbOsFullPathname(g.pVfs, zPath, nOut, zOut);
}

#ifndef CAPDB_OMIT_LOAD_EXTENSION
/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *devsymDlOpen(capdb_vfs *pVfs, const char *zPath){
  return capdbOsDlOpen(g.pVfs, zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void devsymDlError(capdb_vfs *pVfs, int nByte, char *zErrMsg){
  capdbOsDlError(g.pVfs, nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*devsymDlSym(capdb_vfs *pVfs, void *p, const char *zSym))(void){
  return capdbOsDlSym(g.pVfs, p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void devsymDlClose(capdb_vfs *pVfs, void *pHandle){
  capdbOsDlClose(g.pVfs, pHandle);
}
#endif /* CAPDB_OMIT_LOAD_EXTENSION */

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int devsymRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  return capdbOsRandomness(g.pVfs, nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int devsymSleep(capdb_vfs *pVfs, int nMicro){
  return capdbOsSleep(g.pVfs, nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int devsymCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  return g.pVfs->xCurrentTime(g.pVfs, pTimeOut);
}

/*
** Return the sector-size in bytes for an writecrash-file.
*/
static int writecrashSectorSize(capdb_file *pFile){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsSectorSize(p->pReal);
}

/*
** Return the device characteristic flags supported by an writecrash-file.
*/
static int writecrashDeviceCharacteristics(capdb_file *pFile){
  devsym_file *p = (devsym_file *)pFile;
  return capdbOsDeviceCharacteristics(p->pReal);
}

/*
** Write data to an writecrash-file.
*/
static int writecrashWrite(
  capdb_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  devsym_file *p = (devsym_file *)pFile;
  if( g.nWriteCrash>0 ){
    g.nWriteCrash--;
    if( g.nWriteCrash==0 ) abort();
  }
  return capdbOsWrite(p->pReal, zBuf, iAmt, iOfst);
}

/*
** Open an writecrash file handle.
*/
static int writecrashOpen(
  capdb_vfs *pVfs,
  const char *zName,
  capdb_file *pFile,
  int flags,
  int *pOutFlags
){
static capdb_io_methods writecrash_io_methods = {
  2,                                /* iVersion */
  devsymClose,                      /* xClose */
  devsymRead,                       /* xRead */
  writecrashWrite,                  /* xWrite */
  devsymTruncate,                   /* xTruncate */
  devsymSync,                       /* xSync */
  devsymFileSize,                   /* xFileSize */
  devsymLock,                       /* xLock */
  devsymUnlock,                     /* xUnlock */
  devsymCheckReservedLock,          /* xCheckReservedLock */
  devsymFileControl,                /* xFileControl */
  writecrashSectorSize,             /* xSectorSize */
  writecrashDeviceCharacteristics,  /* xDeviceCharacteristics */
  devsymShmMap,                     /* xShmMap */
  devsymShmLock,                    /* xShmLock */
  devsymShmBarrier,                 /* xShmBarrier */
  devsymShmUnmap                    /* xShmUnmap */
};

  int rc;
  devsym_file *p = (devsym_file *)pFile;
  p->pReal = (capdb_file *)&p[1];
  rc = capdbOsOpen(g.pVfs, zName, p->pReal, flags, pOutFlags);
  if( p->pReal->pMethods ){
    pFile->pMethods = &writecrash_io_methods;
  }
  return rc;
}

static capdb_vfs devsym_vfs = {
  2,                     /* iVersion */
  sizeof(devsym_file),      /* szOsFile */
  DEVSYM_MAX_PATHNAME,      /* mxPathname */
  0,                     /* pNext */
  DEVSYM_VFS_NAME,          /* zName */
  0,                     /* pAppData */
  devsymOpen,               /* xOpen */
  devsymDelete,             /* xDelete */
  devsymAccess,             /* xAccess */
  devsymFullPathname,       /* xFullPathname */
#ifndef CAPDB_OMIT_LOAD_EXTENSION
  devsymDlOpen,             /* xDlOpen */
  devsymDlError,            /* xDlError */
  devsymDlSym,              /* xDlSym */
  devsymDlClose,            /* xDlClose */
#else
  0,                        /* xDlOpen */
  0,                        /* xDlError */
  0,                        /* xDlSym */
  0,                        /* xDlClose */
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
  devsymRandomness,         /* xRandomness */
  devsymSleep,              /* xSleep */
  devsymCurrentTime,        /* xCurrentTime */
  0,                        /* xGetLastError */
  0                         /* xCurrentTimeInt64 */
};

static capdb_vfs writecrash_vfs = {
  2,                     /* iVersion */
  sizeof(devsym_file),      /* szOsFile */
  DEVSYM_MAX_PATHNAME,      /* mxPathname */
  0,                     /* pNext */
  WRITECRASH_NAME,          /* zName */
  0,                     /* pAppData */
  writecrashOpen,           /* xOpen */
  devsymDelete,             /* xDelete */
  devsymAccess,             /* xAccess */
  devsymFullPathname,       /* xFullPathname */
#ifndef CAPDB_OMIT_LOAD_EXTENSION
  devsymDlOpen,             /* xDlOpen */
  devsymDlError,            /* xDlError */
  devsymDlSym,              /* xDlSym */
  devsymDlClose,            /* xDlClose */
#else
  0,                        /* xDlOpen */
  0,                        /* xDlError */
  0,                        /* xDlSym */
  0,                        /* xDlClose */
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
  devsymRandomness,         /* xRandomness */
  devsymSleep,              /* xSleep */
  devsymCurrentTime,        /* xCurrentTime */
  0,                        /* xGetLastError */
  0                         /* xCurrentTimeInt64 */
};


/*
** This procedure registers the devsym vfs with SQLite. If the argument is
** true, the devsym vfs becomes the new default vfs. It is the only publicly
** available function in this file.
*/
void devsym_register(int iDeviceChar, int iSectorSize){

  if( g.pVfs==0 ){
    g.pVfs = capdb_vfs_find(0);
    devsym_vfs.szOsFile += g.pVfs->szOsFile;
    writecrash_vfs.szOsFile += g.pVfs->szOsFile;
    capdb_vfs_register(&devsym_vfs, 0);
    capdb_vfs_register(&writecrash_vfs, 0);
  }
  if( iDeviceChar>=0 ){
    g.iDeviceChar = iDeviceChar;
  }else{
    g.iDeviceChar = 0;
  }
  if( iSectorSize>=0 ){
    g.iSectorSize = iSectorSize;
  }else{
    g.iSectorSize = 512;
  }
}

void devsym_unregister(){
  capdb_vfs_unregister(&devsym_vfs);
  capdb_vfs_unregister(&writecrash_vfs);
  g.pVfs = 0;
  g.iDeviceChar = 0;
  g.iSectorSize = 0;
}

void devsym_crash_on_write(int nWrite){
  if( g.pVfs==0 ){
    g.pVfs = capdb_vfs_find(0);
    devsym_vfs.szOsFile += g.pVfs->szOsFile;
    writecrash_vfs.szOsFile += g.pVfs->szOsFile;
    capdb_vfs_register(&devsym_vfs, 0);
    capdb_vfs_register(&writecrash_vfs, 0);
  }
  g.nWriteCrash = nWrite;
}

#endif
