/*
** 2013-10-09
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
** This file contains the implementation of an SQLite vfs wrapper for
** unix that generates per-database log files of all disk activity.
*/

/*
** This module contains code for a wrapper VFS that causes a log of
** most VFS calls to be written into a file on disk.
**
** Each database connection creates a separate log file in the same
** directory as the original database and named after the original
** database.  A unique suffix is added to avoid name collisions.  
** Separate log files are used so that concurrent processes do not
** try to write log operations to the same file at the same instant, 
** resulting in overwritten or comingled log text.
**
** Each individual log file records operations by a single database
** connection on both the original database and its associated rollback
** journal.
**
** The log files are in the comma-separated-value (CSV) format.  The
** log files can be imported into an SQLite database using the ".import"
** command of the SQLite command-line shell for analysis.
**
** One technique for using this module is to append the text of this
** module to the end of a standard "capdb.c" amalgamation file then
** add the following compile-time options:
**
**     -DCAPDB_EXTRA_INIT=capdb_register_vfslog
**     -DCAPDB_USE_FCNTL_TRACE
**
** The first compile-time option causes the capdb_register_vfslog()
** function, defined below, to be invoked when SQLite is initialized.
** That causes this custom VFS to become the default VFS for all
** subsequent connections.  The CAPDB_USE_FCNTL_TRACE option causes
** the SQLite core to issue extra capdb_file_control() operations
** with CAPDB_FCNTL_TRACE to give some indication of what is going
** on in the core.
*/

#include "capdb.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#if CAPDB_OS_UNIX
# include <unistd.h>
#endif

/*
** Forward declaration of objects used by this utility
*/
typedef struct VLogLog VLogLog;
typedef struct VLogVfs VLogVfs;
typedef struct VLogFile VLogFile;

/* There is a pair (an array of size 2) of the following objects for
** each database file being logged.  The first contains the filename
** and is used to log I/O with the main database.  The second has
** a NULL filename and is used to log I/O for the journal.  Both
** out pointers are the same.
*/
struct VLogLog {
  VLogLog *pNext;                 /* Next in a list of all active logs */
  VLogLog **ppPrev;               /* Pointer to this in the list */
  int nRef;                       /* Number of references to this object */
  int nFilename;                  /* Length of zFilename in bytes */
  char *zFilename;                /* Name of database file.  NULL for journal */
  FILE *out;                      /* Write information here */
};

struct VLogVfs {
  capdb_vfs base;               /* VFS methods */
  capdb_vfs *pVfs;              /* Parent VFS */
};

struct VLogFile {
  capdb_file base;              /* IO methods */
  capdb_file *pReal;            /* Underlying file handle */
  VLogLog *pLog;                  /* The log file for this file */
};

#define REALVFS(p) (((VLogVfs*)(p))->pVfs)

/*
** Methods for VLogFile
*/
static int vlogClose(capdb_file*);
static int vlogRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int vlogWrite(capdb_file*,const void*,int iAmt, capdb_int64 iOfst);
static int vlogTruncate(capdb_file*, capdb_int64 size);
static int vlogSync(capdb_file*, int flags);
static int vlogFileSize(capdb_file*, capdb_int64 *pSize);
static int vlogLock(capdb_file*, int);
static int vlogUnlock(capdb_file*, int);
static int vlogCheckReservedLock(capdb_file*, int *pResOut);
static int vlogFileControl(capdb_file*, int op, void *pArg);
static int vlogSectorSize(capdb_file*);
static int vlogDeviceCharacteristics(capdb_file*);

/*
** Methods for VLogVfs
*/
static int vlogOpen(capdb_vfs*, const char *, capdb_file*, int , int *);
static int vlogDelete(capdb_vfs*, const char *zName, int syncDir);
static int vlogAccess(capdb_vfs*, const char *zName, int flags, int *);
static int vlogFullPathname(capdb_vfs*, const char *zName, int, char *zOut);
static void *vlogDlOpen(capdb_vfs*, const char *zFilename);
static void vlogDlError(capdb_vfs*, int nByte, char *zErrMsg);
static void (*vlogDlSym(capdb_vfs *pVfs, void *p, const char*zSym))(void);
static void vlogDlClose(capdb_vfs*, void*);
static int vlogRandomness(capdb_vfs*, int nByte, char *zOut);
static int vlogSleep(capdb_vfs*, int microseconds);
static int vlogCurrentTime(capdb_vfs*, double*);
static int vlogGetLastError(capdb_vfs*, int, char *);
static int vlogCurrentTimeInt64(capdb_vfs*, capdb_int64*);

static VLogVfs vlog_vfs = {
  {
    1,                            /* iVersion */
    0,                            /* szOsFile (set by register_vlog()) */
    1024,                         /* mxPathname */
    0,                            /* pNext */
    "vfslog",                     /* zName */
    0,                            /* pAppData */
    vlogOpen,                     /* xOpen */
    vlogDelete,                   /* xDelete */
    vlogAccess,                   /* xAccess */
    vlogFullPathname,             /* xFullPathname */
    vlogDlOpen,                   /* xDlOpen */
    vlogDlError,                  /* xDlError */
    vlogDlSym,                    /* xDlSym */
    vlogDlClose,                  /* xDlClose */
    vlogRandomness,               /* xRandomness */
    vlogSleep,                    /* xSleep */
    vlogCurrentTime,              /* xCurrentTime */
    vlogGetLastError,             /* xGetLastError */
    vlogCurrentTimeInt64          /* xCurrentTimeInt64 */
  },
  0
};

static capdb_io_methods vlog_io_methods = {
  1,                              /* iVersion */
  vlogClose,                      /* xClose */
  vlogRead,                       /* xRead */
  vlogWrite,                      /* xWrite */
  vlogTruncate,                   /* xTruncate */
  vlogSync,                       /* xSync */
  vlogFileSize,                   /* xFileSize */
  vlogLock,                       /* xLock */
  vlogUnlock,                     /* xUnlock */
  vlogCheckReservedLock,          /* xCheckReservedLock */
  vlogFileControl,                /* xFileControl */
  vlogSectorSize,                 /* xSectorSize */
  vlogDeviceCharacteristics,      /* xDeviceCharacteristics */
  0,                              /* xShmMap */
  0,                              /* xShmLock */
  0,                              /* xShmBarrier */
  0                               /* xShmUnmap */
};

#if CAPDB_OS_UNIX && !defined(NO_GETTOD)
#include <sys/time.h>
static capdb_uint64 vlog_time(){
  struct timeval sTime;
  gettimeofday(&sTime, 0);
  return sTime.tv_usec + (capdb_uint64)sTime.tv_sec * 1000000;
}
#elif CAPDB_OS_WIN
#include <windows.h>
#include <time.h>
static capdb_uint64 vlog_time(){
  FILETIME ft;
  capdb_uint64 u64time = 0;
 
  GetSystemTimeAsFileTime(&ft);

  u64time |= ft.dwHighDateTime;
  u64time <<= 32;
  u64time |= ft.dwLowDateTime;

  /* ft is 100-nanosecond intervals, we want microseconds */
  return u64time /(capdb_uint64)10;
}
#else
static capdb_uint64 vlog_time(){
  return 0;
}
#endif


/*
** Write a message to the log file
*/
static void vlogLogPrint(
  VLogLog *pLog,                 /* The log file to write into */
  capdb_int64 tStart,            /* Start time of system call */
  capdb_int64 tElapse,           /* Elapse time of system call */
  const char *zOp,                 /* Type of system call */
  capdb_int64 iArg1,             /* First argument */
  capdb_int64 iArg2,             /* Second argument */
  const char *zArg3,               /* Third argument */
  int iRes                         /* Result */
){
  char z1[40], z2[40], z3[2000];
  if( pLog==0 ) return;
  if( iArg1>=0 ){
    capdb_snprintf(sizeof(z1), z1, "%lld", iArg1);
  }else{
    z1[0] = 0;
  }
  if( iArg2>=0 ){
    capdb_snprintf(sizeof(z2), z2, "%lld", iArg2);
  }else{
    z2[0] = 0;
  }
  if( zArg3 ){
    capdb_snprintf(sizeof(z3), z3, "\"%.*w\"", sizeof(z3)-4, zArg3);
  }else{
    z3[0] = 0;
  }
  fprintf(pLog->out,"%lld,%lld,%s,%d,%s,%s,%s,%d\n",
      tStart, tElapse, zOp, pLog->zFilename==0, z1, z2, z3, iRes);
}

/*
** List of all active log connections.  Protected by the master mutex.
*/
static VLogLog *allLogs = 0;

/*
** Close a VLogLog object
*/
static void vlogLogClose(VLogLog *p){
  if( p ){
    capdb_mutex *pMutex;
    p->nRef--;
    if( p->nRef>0 || p->zFilename==0 ) return;
    pMutex = capdb_mutex_alloc(CAPDB_MUTEX_STATIC_MASTER);
    capdb_mutex_enter(pMutex);
    *p->ppPrev = p->pNext;
    if( p->pNext ) p->pNext->ppPrev = p->ppPrev;
    capdb_mutex_leave(pMutex);
    fclose(p->out);
    capdb_free(p);
  }
}

/*
** Open a VLogLog object on the given file
*/
static VLogLog *vlogLogOpen(const char *zFilename){
  int nName = (int)strlen(zFilename);
  int isJournal = 0;
  capdb_mutex *pMutex;
  VLogLog *pLog, *pTemp;
  capdb_int64 tNow = 0;
  if( nName>4 && strcmp(zFilename+nName-4,"-wal")==0 ){
    return 0;  /* Do not log wal files */
  }else
  if( nName>8 && strcmp(zFilename+nName-8,"-journal")==0 ){
    nName -= 8;
    isJournal = 1;
  }else if( nName>12 
         && capdb_strglob("-mj??????9??", zFilename+nName-12)==0 ){
    return 0;  /* Do not log master journal files */
  }
  pTemp = capdb_malloc64( sizeof(*pLog)*2 + nName + 60 );
  if( pTemp==0 ) return 0;
  pMutex = capdb_mutex_alloc(CAPDB_MUTEX_STATIC_MASTER);
  capdb_mutex_enter(pMutex);
  for(pLog=allLogs; pLog; pLog=pLog->pNext){
    if( pLog->nFilename==nName && !memcmp(pLog->zFilename, zFilename, nName) ){
      break;
    }
  }
  if( pLog==0 ){
    pLog = pTemp;
    pTemp = 0;
    memset(pLog, 0, sizeof(*pLog)*2);
    pLog->zFilename = (char*)&pLog[2];
    tNow = vlog_time();
    capdb_snprintf(nName+60, pLog->zFilename, "%.*s-debuglog-%lld",
                     nName, zFilename, tNow);
    pLog->out = fopen(pLog->zFilename, "a");
    if( pLog->out==0 ){
      capdb_mutex_leave(pMutex);
      capdb_free(pLog);
      return 0;
    }
    pLog->nFilename = nName;
    pLog[1].out = pLog[0].out;
    pLog->ppPrev = &allLogs;
    if( allLogs ) allLogs->ppPrev = &pLog->pNext;
    pLog->pNext = allLogs;
    allLogs = pLog;
  }
  capdb_mutex_leave(pMutex);
  if( pTemp ){
    capdb_free(pTemp);
  }else{
#if CAPDB_OS_UNIX
    char zHost[200];
    zHost[0] = 0;
    gethostname(zHost, sizeof(zHost)-1);
    zHost[sizeof(zHost)-1] = 0;
    vlogLogPrint(pLog, tNow, 0, "IDENT", getpid(), -1, zHost, 0);
#endif
  }
  if( pLog && isJournal ) pLog++;
  pLog->nRef++;
  return pLog;
}


/*
** Close an vlog-file.
*/
static int vlogClose(capdb_file *pFile){
  capdb_uint64 tStart, tElapse;
  int rc = CAPDB_OK;
  VLogFile *p = (VLogFile *)pFile;

  tStart = vlog_time();
  if( p->pReal->pMethods ){
    rc = p->pReal->pMethods->xClose(p->pReal);
  }
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "CLOSE", -1, -1, 0, rc);
  vlogLogClose(p->pLog);
  return rc;
}

/*
** Compute signature for a block of content.
**
** For blocks of 16 or fewer bytes, the signature is just a hex dump of
** the entire block.
**
** For blocks of more than 16 bytes, the signature is a hex dump of the
** first 8 bytes followed by a 64-bit has of the entire block.
*/
static void vlogSignature(unsigned char *p, int n, char *zCksum){
  unsigned int s0 = 0, s1 = 0;
  unsigned int *pI;
  int i;
  if( n<=16 ){
    for(i=0; i<n; i++) capdb_snprintf(3, zCksum+i*2, "%02x", p[i]);
  }else{ 
    pI = (unsigned int*)p;
    for(i=0; i<n-7; i+=8){
      s0 += pI[0] + s1;
      s1 += pI[1] + s0;
      pI += 2;
    }
    for(i=0; i<8; i++) capdb_snprintf(3, zCksum+i*2, "%02x", p[i]);
    capdb_snprintf(18, zCksum+i*2, "-%08x%08x", s0, s1);
  }
}

/*
** Convert a big-endian 32-bit integer into a native integer
*/
static int bigToNative(const unsigned char *x){
  return (x[0]<<24) + (x[1]<<16) + (x[2]<<8) + x[3];
}

/*
** Read data from an vlog-file.
*/
static int vlogRead(
  capdb_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  char zSig[40];

  tStart = vlog_time();
  rc = p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst);
  tElapse = vlog_time() - tStart;
  if( rc==CAPDB_OK ){
    vlogSignature(zBuf, iAmt, zSig);
  }else{
    zSig[0] = 0;
  }
  vlogLogPrint(p->pLog, tStart, tElapse, "READ", iAmt, iOfst, zSig, rc);
  if( rc==CAPDB_OK
   && p->pLog
   && p->pLog->zFilename
   && iOfst<=24
   && iOfst+iAmt>=28
  ){
    unsigned char *x = ((unsigned char*)zBuf)+(24-iOfst);
    unsigned iCtr, nFree = -1;
    char *zFree = 0;
    char zStr[12];
    iCtr = bigToNative(x);
    if( iOfst+iAmt>=40 ){
      zFree = zStr;
      capdb_snprintf(sizeof(zStr), zStr, "%d", bigToNative(x+8));
      nFree = bigToNative(x+12);
    }
    vlogLogPrint(p->pLog, tStart, 0, "CHNGCTR-READ", iCtr, nFree, zFree, 0);
  }
  return rc;
}

/*
** Write data to an vlog-file.
*/
static int vlogWrite(
  capdb_file *pFile,
  const void *z,
  int iAmt,
  sqlite_int64 iOfst
){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  char zSig[40];

  tStart = vlog_time();
  vlogSignature((unsigned char*)z, iAmt, zSig);
  rc = p->pReal->pMethods->xWrite(p->pReal, z, iAmt, iOfst);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "WRITE", iAmt, iOfst, zSig, rc);
  if( rc==CAPDB_OK
   && p->pLog
   && p->pLog->zFilename
   && iOfst<=24
   && iOfst+iAmt>=28
  ){
    unsigned char *x = ((unsigned char*)z)+(24-iOfst);
    unsigned iCtr, nFree = -1;
    char *zFree = 0;
    char zStr[12];
    iCtr = bigToNative(x);
    if( iOfst+iAmt>=40 ){
      zFree = zStr;
      capdb_snprintf(sizeof(zStr), zStr, "%d", bigToNative(x+8));
      nFree = bigToNative(x+12);
    }
    vlogLogPrint(p->pLog, tStart, 0, "CHNGCTR-WRITE", iCtr, nFree, zFree, 0);
  }
  return rc;
}

/*
** Truncate an vlog-file.
*/
static int vlogTruncate(capdb_file *pFile, sqlite_int64 size){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xTruncate(p->pReal, size);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "TRUNCATE", size, -1, 0, rc);
  return rc;
}

/*
** Sync an vlog-file.
*/
static int vlogSync(capdb_file *pFile, int flags){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "SYNC", flags, -1, 0, rc);
  return rc;
}

/*
** Return the current file-size of an vlog-file.
*/
static int vlogFileSize(capdb_file *pFile, sqlite_int64 *pSize){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xFileSize(p->pReal, pSize);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "FILESIZE", *pSize, -1, 0, rc);
  return rc;
}

/*
** Lock an vlog-file.
*/
static int vlogLock(capdb_file *pFile, int eLock){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xLock(p->pReal, eLock);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "LOCK", eLock, -1, 0, rc);
  return rc;
}

/*
** Unlock an vlog-file.
*/
static int vlogUnlock(capdb_file *pFile, int eLock){
  int rc;
  capdb_uint64 tStart;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  vlogLogPrint(p->pLog, tStart, 0, "UNLOCK", eLock, -1, 0, 0);
  rc = p->pReal->pMethods->xUnlock(p->pReal, eLock);
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an vlog-file.
*/
static int vlogCheckReservedLock(capdb_file *pFile, int *pResOut){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "CHECKRESERVEDLOCK",
                 *pResOut, -1, "", rc);
  return rc;
}

/*
** File control method. For custom operations on an vlog-file.
*/
static int vlogFileControl(capdb_file *pFile, int op, void *pArg){
  VLogFile *p = (VLogFile *)pFile;
  capdb_uint64 tStart, tElapse;
  int rc;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
  if( op==CAPDB_FCNTL_VFSNAME && rc==CAPDB_OK ){
    *(char**)pArg = capdb_mprintf("vlog/%z", *(char**)pArg);
  }
  tElapse = vlog_time() - tStart;
  if( op==CAPDB_FCNTL_TRACE ){
    vlogLogPrint(p->pLog, tStart, tElapse, "TRACE", op, -1, pArg, rc);
  }else if( op==CAPDB_FCNTL_PRAGMA ){
    const char **azArg = (const char **)pArg;
    vlogLogPrint(p->pLog, tStart, tElapse, "FILECONTROL", op, -1, azArg[1], rc);
  }else if( op==CAPDB_FCNTL_SIZE_HINT ){
    capdb_int64 sz = *(capdb_int64*)pArg;
    vlogLogPrint(p->pLog, tStart, tElapse, "FILECONTROL", op, sz, 0, rc);
  }else{
    vlogLogPrint(p->pLog, tStart, tElapse, "FILECONTROL", op, -1, 0, rc);
  }
  return rc;
}

/*
** Return the sector-size in bytes for an vlog-file.
*/
static int vlogSectorSize(capdb_file *pFile){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xSectorSize(p->pReal);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "SECTORSIZE", -1, -1, 0, rc);
  return rc;
}

/*
** Return the device characteristic flags supported by an vlog-file.
*/
static int vlogDeviceCharacteristics(capdb_file *pFile){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogFile *p = (VLogFile *)pFile;
  tStart = vlog_time();
  rc = p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
  tElapse = vlog_time() - tStart;
  vlogLogPrint(p->pLog, tStart, tElapse, "DEVCHAR", -1, -1, 0, rc);
  return rc;
}


/*
** Open an vlog file handle.
*/
static int vlogOpen(
  capdb_vfs *pVfs,
  const char *zName,
  capdb_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  capdb_uint64 tStart, tElapse;
  capdb_int64 iArg2;
  VLogFile *p = (VLogFile*)pFile;

  p->pReal = (capdb_file*)&p[1];
  if( (flags & (CAPDB_OPEN_MAIN_DB|CAPDB_OPEN_MAIN_JOURNAL))!=0 ){
    p->pLog = vlogLogOpen(zName);
  }else{
    p->pLog = 0;
  }
  tStart = vlog_time();
  rc = REALVFS(pVfs)->xOpen(REALVFS(pVfs), zName, p->pReal, flags, pOutFlags);
  tElapse = vlog_time() - tStart;
  iArg2 = pOutFlags ? *pOutFlags : -1;
  vlogLogPrint(p->pLog, tStart, tElapse, "OPEN", flags, iArg2, 0, rc);
  if( rc==CAPDB_OK ){
    pFile->pMethods = &vlog_io_methods;
  }else{
    if( p->pLog ) vlogLogClose(p->pLog);
    p->pLog = 0;
  }
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int vlogDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogLog *pLog;
  tStart = vlog_time();
  rc = REALVFS(pVfs)->xDelete(REALVFS(pVfs), zPath, dirSync);
  tElapse = vlog_time() - tStart;
  pLog = vlogLogOpen(zPath);
  vlogLogPrint(pLog, tStart, tElapse, "DELETE", dirSync, -1, 0, rc);
  vlogLogClose(pLog);
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int vlogAccess(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  int rc;
  capdb_uint64 tStart, tElapse;
  VLogLog *pLog;
  tStart = vlog_time();
  rc = REALVFS(pVfs)->xAccess(REALVFS(pVfs), zPath, flags, pResOut);
  tElapse = vlog_time() - tStart;
  pLog = vlogLogOpen(zPath);
  vlogLogPrint(pLog, tStart, tElapse, "ACCESS", flags, *pResOut, 0, rc);
  vlogLogClose(pLog);
  return rc;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (INST_MAX_PATHNAME+1) bytes.
*/
static int vlogFullPathname(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  return REALVFS(pVfs)->xFullPathname(REALVFS(pVfs), zPath, nOut, zOut);
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *vlogDlOpen(capdb_vfs *pVfs, const char *zPath){
  return REALVFS(pVfs)->xDlOpen(REALVFS(pVfs), zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void vlogDlError(capdb_vfs *pVfs, int nByte, char *zErrMsg){
  REALVFS(pVfs)->xDlError(REALVFS(pVfs), nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*vlogDlSym(capdb_vfs *pVfs, void *p, const char *zSym))(void){
  return REALVFS(pVfs)->xDlSym(REALVFS(pVfs), p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void vlogDlClose(capdb_vfs *pVfs, void *pHandle){
  REALVFS(pVfs)->xDlClose(REALVFS(pVfs), pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int vlogRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  return REALVFS(pVfs)->xRandomness(REALVFS(pVfs), nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int vlogSleep(capdb_vfs *pVfs, int nMicro){
  return REALVFS(pVfs)->xSleep(REALVFS(pVfs), nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int vlogCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  return REALVFS(pVfs)->xCurrentTime(REALVFS(pVfs), pTimeOut);
}

static int vlogGetLastError(capdb_vfs *pVfs, int a, char *b){
  return REALVFS(pVfs)->xGetLastError(REALVFS(pVfs), a, b);
}
static int vlogCurrentTimeInt64(capdb_vfs *pVfs, capdb_int64 *p){
  return REALVFS(pVfs)->xCurrentTimeInt64(REALVFS(pVfs), p);
}

/*
** Register debugvfs as the default VFS for this process.
*/
int capdb_register_vfslog(const char *zArg){
  vlog_vfs.pVfs = capdb_vfs_find(0);
  if( vlog_vfs.pVfs==0 ) return CAPDB_ERROR;
  vlog_vfs.base.szOsFile = sizeof(VLogFile) + vlog_vfs.pVfs->szOsFile;
  return capdb_vfs_register(&vlog_vfs.base, 1);
}
