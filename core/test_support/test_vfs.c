/*
** 2010 May 05
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
** This file contains the implementation of the Tcl [testvfs] command,
** used to create SQLite VFS implementations with various properties and
** instrumentation to support testing SQLite.
**
**   testvfs VFSNAME ?OPTIONS?
**
** Available options are:
**
**   -noshm      BOOLEAN        (True to omit shm methods. Default false)
**   -default    BOOLEAN        (True to make the vfs default. Default false)
**   -szosfile   INTEGER        (Value for capdb_vfs.szOsFile)
**   -mxpathname INTEGER        (Value for capdb_vfs.mxPathname)
**   -iversion   INTEGER        (Value for capdb_vfs.iVersion)
*/
#if CAPDB_TEST          /* This file is used for testing only */

#include "capdb.h"
#include "capdbInt.h"
#include "tclsqlite.h"

typedef struct Testvfs Testvfs;
typedef struct TestvfsShm TestvfsShm;
typedef struct TestvfsBuffer TestvfsBuffer;
typedef struct TestvfsFile TestvfsFile;
typedef struct TestvfsFd TestvfsFd;

/*
** An open file handle.
*/
struct TestvfsFile {
  capdb_file base;              /* Base class.  Must be first */
  TestvfsFd *pFd;                 /* File data */
};
#define tvfsGetFd(pFile) (((TestvfsFile *)pFile)->pFd)

struct TestvfsFd {
  capdb_vfs *pVfs;              /* The VFS */
  const char *zFilename;          /* Filename as passed to xOpen() */
  capdb_file *pReal;            /* The real, underlying file descriptor */
  Tcl_Obj *pShmId;                /* Shared memory id for Tcl callbacks */

  TestvfsBuffer *pShm;            /* Shared memory buffer */
  u32 excllock;                   /* Mask of exclusive locks */
  u32 sharedlock;                 /* Mask of shared locks */
  TestvfsFd *pNext;               /* Next handle opened on the same file */
};


#define FAULT_INJECT_NONE       0
#define FAULT_INJECT_TRANSIENT  1
#define FAULT_INJECT_PERSISTENT 2

typedef struct TestFaultInject TestFaultInject;
struct TestFaultInject {
  int iCnt;                       /* Remaining calls before fault injection */
  int eFault;                     /* A FAULT_INJECT_* value */
  int nFail;                      /* Number of faults injected */
};

/*
** An instance of this structure is allocated for each VFS created. The
** capdb_vfs.pAppData field of the VFS structure registered with SQLite
** is set to point to it.
*/
struct Testvfs {
  char *zName;                    /* Name of this VFS */
  capdb_vfs *pParent;           /* The VFS to use for file IO */
  capdb_vfs *pVfs;              /* The testvfs registered with SQLite */
  Tcl_Interp *interp;             /* Interpreter to run script in */
  Tcl_Obj *pScript;               /* Script to execute */
  TestvfsBuffer *pBuffer;         /* List of shared buffers */
  int isNoshm;
  int isFullshm;

  int mask;                       /* Mask controlling [script] and [ioerr] */

  TestFaultInject ioerr_err;
  TestFaultInject full_err;
  TestFaultInject cantopen_err;

#if 0
  int iIoerrCnt;
  int ioerr;
  int nIoerrFail;
  int iFullCnt;
  int fullerr;
  int nFullFail;
#endif

  int iDevchar;
  int iSectorsize;
};

/*
** The Testvfs.mask variable is set to a combination of the following.
** If a bit is clear in Testvfs.mask, then calls made by SQLite to the 
** corresponding VFS method is ignored for purposes of:
**
**   + Simulating IO errors, and
**   + Invoking the Tcl callback script.
*/
#define TESTVFS_SHMOPEN_MASK      0x00000001
#define TESTVFS_SHMLOCK_MASK      0x00000010
#define TESTVFS_SHMMAP_MASK       0x00000020
#define TESTVFS_SHMBARRIER_MASK   0x00000040
#define TESTVFS_SHMCLOSE_MASK     0x00000080

#define TESTVFS_OPEN_MASK         0x00000100
#define TESTVFS_SYNC_MASK         0x00000200
#define TESTVFS_DELETE_MASK       0x00000400
#define TESTVFS_CLOSE_MASK        0x00000800
#define TESTVFS_WRITE_MASK        0x00001000
#define TESTVFS_TRUNCATE_MASK     0x00002000
#define TESTVFS_ACCESS_MASK       0x00004000
#define TESTVFS_FULLPATHNAME_MASK 0x00008000
#define TESTVFS_READ_MASK         0x00010000
#define TESTVFS_UNLOCK_MASK       0x00020000
#define TESTVFS_LOCK_MASK         0x00040000
#define TESTVFS_CKLOCK_MASK       0x00080000
#define TESTVFS_FCNTL_MASK        0x00100000
#define TESTVFS_SLEEP_MASK        0x00200000

#define TESTVFS_ALL_MASK          0x003FFFFF


#define TESTVFS_MAX_PAGES 1024

/*
** A shared-memory buffer. There is one of these objects for each shared
** memory region opened by clients. If two clients open the same file,
** there are two TestvfsFile structures but only one TestvfsBuffer structure.
*/
struct TestvfsBuffer {
  char *zFile;                    /* Associated file name */
  int pgsz;                       /* Page size */
  u8 *aPage[TESTVFS_MAX_PAGES];   /* Array of ckalloc'd pages */
  TestvfsFd *pFile;               /* List of open handles */
  TestvfsBuffer *pNext;           /* Next in linked list of all buffers */
};


#define PARENTVFS(x) (((Testvfs *)((x)->pAppData))->pParent)

#define TESTVFS_MAX_ARGS 12


/*
** Method declarations for TestvfsFile.
*/
static int tvfsClose(capdb_file*);
static int tvfsRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int tvfsWrite(capdb_file*,const void*,int iAmt, capdb_int64 iOfst);
static int tvfsTruncate(capdb_file*, capdb_int64 size);
static int tvfsSync(capdb_file*, int flags);
static int tvfsFileSize(capdb_file*, capdb_int64 *pSize);
static int tvfsLock(capdb_file*, int);
static int tvfsUnlock(capdb_file*, int);
static int tvfsCheckReservedLock(capdb_file*, int *);
static int tvfsFileControl(capdb_file*, int op, void *pArg);
static int tvfsSectorSize(capdb_file*);
static int tvfsDeviceCharacteristics(capdb_file*);

/*
** Method declarations for tvfs_vfs.
*/
static int tvfsOpen(capdb_vfs*, const char *, capdb_file*, int , int *);
static int tvfsDelete(capdb_vfs*, const char *zName, int syncDir);
static int tvfsAccess(capdb_vfs*, const char *zName, int flags, int *);
static int tvfsFullPathname(capdb_vfs*, const char *zName, int, char *zOut);
#ifndef CAPDB_OMIT_LOAD_EXTENSION
static void *tvfsDlOpen(capdb_vfs*, const char *zFilename);
static void tvfsDlError(capdb_vfs*, int nByte, char *zErrMsg);
static void (*tvfsDlSym(capdb_vfs*,void*, const char *zSymbol))(void);
static void tvfsDlClose(capdb_vfs*, void*);
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
static int tvfsRandomness(capdb_vfs*, int nByte, char *zOut);
static int tvfsSleep(capdb_vfs*, int microseconds);
static int tvfsCurrentTime(capdb_vfs*, double*);

static int tvfsShmOpen(capdb_file*);
static int tvfsShmLock(capdb_file*, int , int, int);
static int tvfsShmMap(capdb_file*,int,int,int, void volatile **);
static void tvfsShmBarrier(capdb_file*);
static int tvfsShmUnmap(capdb_file*, int);

static int tvfsFetch(capdb_file*, capdb_int64, int, void**);
static int tvfsUnfetch(capdb_file*, capdb_int64, void*);

static capdb_io_methods tvfs_io_methods = {
  3,                              /* iVersion */
  tvfsClose,                      /* xClose */
  tvfsRead,                       /* xRead */
  tvfsWrite,                      /* xWrite */
  tvfsTruncate,                   /* xTruncate */
  tvfsSync,                       /* xSync */
  tvfsFileSize,                   /* xFileSize */
  tvfsLock,                       /* xLock */
  tvfsUnlock,                     /* xUnlock */
  tvfsCheckReservedLock,          /* xCheckReservedLock */
  tvfsFileControl,                /* xFileControl */
  tvfsSectorSize,                 /* xSectorSize */
  tvfsDeviceCharacteristics,      /* xDeviceCharacteristics */
  tvfsShmMap,                     /* xShmMap */
  tvfsShmLock,                    /* xShmLock */
  tvfsShmBarrier,                 /* xShmBarrier */
  tvfsShmUnmap,                   /* xShmUnmap */
  tvfsFetch,
  tvfsUnfetch
};

static int tvfsResultCode(Testvfs *p, int *pRc){
  struct errcode {
    int eCode;
    const char *zCode;
  } aCode[] = {
    { CAPDB_OK,       "CAPDB_OK"     },
    { CAPDB_ERROR,    "CAPDB_ERROR"  },
    { CAPDB_IOERR,    "CAPDB_IOERR"  },
    { CAPDB_LOCKED,   "CAPDB_LOCKED" },
    { CAPDB_BUSY,     "CAPDB_BUSY"   },
    { CAPDB_READONLY, "CAPDB_READONLY"   },
    { CAPDB_READONLY_CANTINIT, "CAPDB_READONLY_CANTINIT"   },
    { CAPDB_NOTFOUND, "CAPDB_NOTFOUND"   },
    { -1,              "CAPDB_OMIT"   },
  };

  const char *z;
  int i;

  z = Tcl_GetStringResult(p->interp);
  for(i=0; i<ArraySize(aCode); i++){
    if( 0==strcmp(z, aCode[i].zCode) ){
      *pRc = aCode[i].eCode;
      return 1;
    }
  }

  return 0;
}

static int tvfsInjectFault(TestFaultInject *p){
  int ret = 0;
  if( p->eFault ){
    p->iCnt--;
    if( p->iCnt==0 || (p->iCnt<0 && p->eFault==FAULT_INJECT_PERSISTENT ) ){
      ret = 1;
      p->nFail++;
    }
  }
  return ret;
}


static int tvfsInjectIoerr(Testvfs *p){
  return tvfsInjectFault(&p->ioerr_err);
}

static int tvfsInjectFullerr(Testvfs *p){
  return tvfsInjectFault(&p->full_err);
}
static int tvfsInjectCantopenerr(Testvfs *p){
  return tvfsInjectFault(&p->cantopen_err);
}


static void tvfsExecTcl(
  Testvfs *p, 
  const char *zMethod,
  Tcl_Obj *arg1,
  Tcl_Obj *arg2,
  Tcl_Obj *arg3,
  Tcl_Obj *arg4
){
  int rc;                         /* Return code from Tcl_EvalObj() */
  Tcl_Obj *pEval;
  assert( p->pScript );

  assert( zMethod );
  assert( p );
  assert( arg2==0 || arg1!=0 );
  assert( arg3==0 || arg2!=0 );

  pEval = Tcl_DuplicateObj(p->pScript);
  Tcl_IncrRefCount(p->pScript);
  Tcl_ListObjAppendElement(p->interp, pEval, Tcl_NewStringObj(zMethod, -1));
  if( arg1 ) Tcl_ListObjAppendElement(p->interp, pEval, arg1);
  if( arg2 ) Tcl_ListObjAppendElement(p->interp, pEval, arg2);
  if( arg3 ) Tcl_ListObjAppendElement(p->interp, pEval, arg3);
  if( arg4 ) Tcl_ListObjAppendElement(p->interp, pEval, arg4);

  rc = Tcl_EvalObjEx(p->interp, pEval, TCL_EVAL_GLOBAL);
  if( rc!=TCL_OK ){
    Tcl_BackgroundError(p->interp);
    Tcl_ResetResult(p->interp);
  }
}


/*
** Close an tvfs-file.
*/
static int tvfsClose(capdb_file *pFile){
  TestvfsFile *pTestfile = (TestvfsFile *)pFile;
  TestvfsFd *pFd = pTestfile->pFd;
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;

  if( p->pScript && p->mask&TESTVFS_CLOSE_MASK ){
    tvfsExecTcl(p, "xClose", 
        Tcl_NewStringObj(pFd->zFilename, -1), pFd->pShmId, 0, 0
    );
  }

  if( pFd->pShmId ){
    Tcl_DecrRefCount(pFd->pShmId);
    pFd->pShmId = 0;
  }
  if( pFile->pMethods ){
    ckfree((char *)pFile->pMethods);
  }
  capdbOsClose(pFd->pReal);
  ckfree((char *)pFd);
  pTestfile->pFd = 0;
  return CAPDB_OK;
}

/*
** Read data from an tvfs-file.
*/
static int tvfsRead(
  capdb_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( p->pScript && p->mask&TESTVFS_READ_MASK ){
    tvfsExecTcl(p, "xRead", 
        Tcl_NewStringObj(pFd->zFilename, -1), pFd->pShmId, 0, 0
    );
    tvfsResultCode(p, &rc);
  }
  if( rc==CAPDB_OK && p->mask&TESTVFS_READ_MASK && tvfsInjectIoerr(p) ){
    rc = CAPDB_IOERR;
  }
  if( rc==CAPDB_OK ){
    rc = capdbOsRead(pFd->pReal, zBuf, iAmt, iOfst);
  }
  return rc;
}

/*
** Write data to an tvfs-file.
*/
static int tvfsWrite(
  capdb_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;

  if( p->pScript && p->mask&TESTVFS_WRITE_MASK ){
    tvfsExecTcl(p, "xWrite", 
        Tcl_NewStringObj(pFd->zFilename, -1), pFd->pShmId, 
        Tcl_NewWideIntObj(iOfst), Tcl_NewIntObj(iAmt)
    );
    tvfsResultCode(p, &rc);
    if( rc<0 ) return CAPDB_OK;
  }

  if( rc==CAPDB_OK && tvfsInjectFullerr(p) ){
    rc = CAPDB_FULL;
  }
  if( rc==CAPDB_OK && p->mask&TESTVFS_WRITE_MASK && tvfsInjectIoerr(p) ){
    rc = CAPDB_IOERR;
  }
  
  if( rc==CAPDB_OK ){
    rc = capdbOsWrite(pFd->pReal, zBuf, iAmt, iOfst);
  }
  return rc;
}

/*
** Truncate an tvfs-file.
*/
static int tvfsTruncate(capdb_file *pFile, sqlite_int64 size){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;

  if( p->pScript && p->mask&TESTVFS_TRUNCATE_MASK ){
    tvfsExecTcl(p, "xTruncate", 
        Tcl_NewStringObj(pFd->zFilename, -1), pFd->pShmId, 0, 0
    );
    tvfsResultCode(p, &rc);
  }
  
  if( rc==CAPDB_OK ){
    rc = capdbOsTruncate(pFd->pReal, size);
  }
  return rc;
}

/*
** Sync an tvfs-file.
*/
static int tvfsSync(capdb_file *pFile, int flags){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;

  if( p->pScript && p->mask&TESTVFS_SYNC_MASK ){
    char *zFlags = 0;

    switch( flags ){
      case CAPDB_SYNC_NORMAL:
        zFlags = "normal";
        break;
      case CAPDB_SYNC_FULL:
        zFlags = "full";
        break;
      case CAPDB_SYNC_NORMAL|CAPDB_SYNC_DATAONLY:
        zFlags = "normal|dataonly";
        break;
      case CAPDB_SYNC_FULL|CAPDB_SYNC_DATAONLY:
        zFlags = "full|dataonly";
        break;
      default:
        assert(0);
    }

    tvfsExecTcl(p, "xSync", 
        Tcl_NewStringObj(pFd->zFilename, -1), pFd->pShmId,
        Tcl_NewStringObj(zFlags, -1), 0
    );
    tvfsResultCode(p, &rc);
  }

  if( rc==CAPDB_OK && tvfsInjectFullerr(p) ) rc = CAPDB_FULL;

  if( rc==CAPDB_OK ){
    rc = capdbOsSync(pFd->pReal, flags);
  }

  return rc;
}

/*
** Return the current file-size of an tvfs-file.
*/
static int tvfsFileSize(capdb_file *pFile, sqlite_int64 *pSize){
  TestvfsFd *p = tvfsGetFd(pFile);
  return capdbOsFileSize(p->pReal, pSize);
}

/*
** Lock an tvfs-file.
*/
static int tvfsLock(capdb_file *pFile, int eLock){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( p->pScript && p->mask&TESTVFS_LOCK_MASK ){
    char zLock[30];
    capdb_snprintf(sizeof(zLock),zLock,"%d",eLock);
    tvfsExecTcl(p, "xLock", Tcl_NewStringObj(pFd->zFilename, -1), 
                   Tcl_NewStringObj(zLock, -1), 0, 0);
  }
  if( p->mask&TESTVFS_LOCK_MASK && tvfsInjectIoerr(p) ){
    return CAPDB_IOERR_LOCK;
  }
  return capdbOsLock(pFd->pReal, eLock);
}

/*
** Unlock an tvfs-file.
*/
static int tvfsUnlock(capdb_file *pFile, int eLock){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( p->pScript && p->mask&TESTVFS_UNLOCK_MASK ){
    char zLock[30];
    capdb_snprintf(sizeof(zLock),zLock,"%d",eLock);
    tvfsExecTcl(p, "xUnlock", Tcl_NewStringObj(pFd->zFilename, -1), 
                   Tcl_NewStringObj(zLock, -1), 0, 0);
  }
  if( p->mask&TESTVFS_UNLOCK_MASK && tvfsInjectIoerr(p) ){
    return CAPDB_IOERR_UNLOCK;
  }
  return capdbOsUnlock(pFd->pReal, eLock);
}

/*
** Check if another file-handle holds a RESERVED lock on an tvfs-file.
*/
static int tvfsCheckReservedLock(capdb_file *pFile, int *pResOut){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( p->pScript && p->mask&TESTVFS_CKLOCK_MASK ){
    tvfsExecTcl(p, "xCheckReservedLock", Tcl_NewStringObj(pFd->zFilename, -1),
                   0, 0, 0);
  }
  return capdbOsCheckReservedLock(pFd->pReal, pResOut);
}

/*
** File control method. For custom operations on an tvfs-file.
*/
static int tvfsFileControl(capdb_file *pFile, int op, void *pArg){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( op==CAPDB_FCNTL_PRAGMA ){
    char **argv = (char**)pArg;
    if( capdb_stricmp(argv[1],"error")==0 ){
      int rc = CAPDB_ERROR;
      if( argv[2] ){
        const char *z = argv[2];
        int x = atoi(z);
        if( x ){
          rc = x;
          while( capdbIsdigit(z[0]) ){ z++; }
          while( capdbIsspace(z[0]) ){ z++; }
        }
        if( z[0] ) argv[0] = capdb_mprintf("%s", z);
      }
      return rc;
    }
    if( capdb_stricmp(argv[1], "filename")==0 ){
      argv[0] = capdb_mprintf("%s", pFd->zFilename);
      return CAPDB_OK;
    }
  }
  if( p->pScript && (p->mask&TESTVFS_FCNTL_MASK) ){
    struct Fcntl {
      int iFnctl;
      const char *zFnctl;
    } aF[] = {
      { CAPDB_FCNTL_BEGIN_ATOMIC_WRITE, "BEGIN_ATOMIC_WRITE" },
      { CAPDB_FCNTL_COMMIT_ATOMIC_WRITE, "COMMIT_ATOMIC_WRITE" },
      { CAPDB_FCNTL_ZIPVFS, "ZIPVFS" },
    };
    int i;
    for(i=0; i<sizeof(aF)/sizeof(aF[0]); i++){
      if( op==aF[i].iFnctl ) break;
    }
    if( i<sizeof(aF)/sizeof(aF[0]) ){
      int rc = 0;
      tvfsExecTcl(p, "xFileControl", 
          Tcl_NewStringObj(pFd->zFilename, -1), 
          Tcl_NewStringObj(aF[i].zFnctl, -1),
          0, 0
      );
      tvfsResultCode(p, &rc);
      if( rc ) return (rc<0 ? CAPDB_OK : rc);
    }
  }
  return capdbOsFileControl(pFd->pReal, op, pArg);
}

/*
** Return the sector-size in bytes for an tvfs-file.
*/
static int tvfsSectorSize(capdb_file *pFile){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( p->iSectorsize>=0 ){
    return p->iSectorsize;
  }
  return capdbOsSectorSize(pFd->pReal);
}

/*
** Return the device characteristic flags supported by an tvfs-file.
*/
static int tvfsDeviceCharacteristics(capdb_file *pFile){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)pFd->pVfs->pAppData;
  if( p->iDevchar>=0 ){
    return p->iDevchar;
  }
  return capdbOsDeviceCharacteristics(pFd->pReal);
}

/*
** Open an tvfs file handle.
*/
static int tvfsOpen(
  capdb_vfs *pVfs,
  const char *zName,
  capdb_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  TestvfsFile *pTestfile = (TestvfsFile *)pFile;
  TestvfsFd *pFd;
  Tcl_Obj *pId = 0;
  Testvfs *p = (Testvfs *)pVfs->pAppData;

  pFd = (TestvfsFd *)ckalloc(sizeof(TestvfsFd) + PARENTVFS(pVfs)->szOsFile);
  memset(pFd, 0, sizeof(TestvfsFd) + PARENTVFS(pVfs)->szOsFile);
  pFd->pShm = 0;
  pFd->pShmId = 0;
  pFd->zFilename = zName;
  pFd->pVfs = pVfs;
  pFd->pReal = (capdb_file *)&pFd[1];
  memset(pTestfile, 0, sizeof(TestvfsFile));
  pTestfile->pFd = pFd;

  /* Evaluate the Tcl script: 
  **
  **   SCRIPT xOpen FILENAME KEY-VALUE-ARGS
  **
  ** If the script returns an SQLite error code other than CAPDB_OK, an
  ** error is returned to the caller. If it returns CAPDB_OK, the new
  ** connection is named "anon". Otherwise, the value returned by the
  ** script is used as the connection name.
  */
  Tcl_ResetResult(p->interp);
  if( p->pScript && p->mask&TESTVFS_OPEN_MASK ){
    Tcl_Obj *pArg = Tcl_NewObj();
    Tcl_IncrRefCount(pArg);
    if( flags&CAPDB_OPEN_MAIN_DB ){
      const char *z = &zName[strlen(zName)+1];
      while( *z ){
        Tcl_ListObjAppendElement(0, pArg, Tcl_NewStringObj(z, -1));
        z += strlen(z) + 1;
        Tcl_ListObjAppendElement(0, pArg, Tcl_NewStringObj(z, -1));
        z += strlen(z) + 1;
      }
    }
    tvfsExecTcl(p, "xOpen", Tcl_NewStringObj(pFd->zFilename, -1), pArg, 0, 0);
    Tcl_DecrRefCount(pArg);
    if( tvfsResultCode(p, &rc) ){
      if( rc!=CAPDB_OK ) return rc;
    }else{
      pId = Tcl_GetObjResult(p->interp);
    }
  }

  if( (p->mask&TESTVFS_OPEN_MASK) &&  tvfsInjectIoerr(p) ) return CAPDB_IOERR;
  if( tvfsInjectCantopenerr(p) ) return CAPDB_CANTOPEN;
  if( tvfsInjectFullerr(p) ) return CAPDB_FULL;

  if( !pId ){
    pId = Tcl_NewStringObj("anon", -1);
  }
  Tcl_IncrRefCount(pId);
  pFd->pShmId = pId;
  Tcl_ResetResult(p->interp);

  rc = capdbOsOpen(PARENTVFS(pVfs), zName, pFd->pReal, flags, pOutFlags);
  if( pFd->pReal->pMethods ){
    capdb_io_methods *pMethods;
    int nByte;

    if( pVfs->iVersion>1 ){
      nByte = sizeof(capdb_io_methods);
    }else{
      nByte = offsetof(capdb_io_methods, xShmMap);
    }

    pMethods = (capdb_io_methods *)ckalloc(nByte);
    memcpy(pMethods, &tvfs_io_methods, nByte);
    pMethods->iVersion = pFd->pReal->pMethods->iVersion;
    if( pMethods->iVersion>pVfs->iVersion ){
      pMethods->iVersion = pVfs->iVersion;
    }
    if( pVfs->iVersion>1 && ((Testvfs *)pVfs->pAppData)->isNoshm ){
      pMethods->xShmUnmap = 0;
      pMethods->xShmLock = 0;
      pMethods->xShmBarrier = 0;
      pMethods->xShmMap = 0;
    }
    pFile->pMethods = pMethods;
  }

  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int tvfsDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  int rc = CAPDB_OK;
  Testvfs *p = (Testvfs *)pVfs->pAppData;

  if( p->pScript && p->mask&TESTVFS_DELETE_MASK ){
    tvfsExecTcl(p, "xDelete", 
        Tcl_NewStringObj(zPath, -1), Tcl_NewIntObj(dirSync), 0, 0
    );
    tvfsResultCode(p, &rc);
  }
  if( rc==CAPDB_OK ){
    rc = capdbOsDelete(PARENTVFS(pVfs), zPath, dirSync);
  }
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int tvfsAccess(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  Testvfs *p = (Testvfs *)pVfs->pAppData;
  if( p->pScript && p->mask&TESTVFS_ACCESS_MASK ){
    int rc;
    char *zArg = 0;
    if( flags==CAPDB_ACCESS_EXISTS ) zArg = "CAPDB_ACCESS_EXISTS";
    if( flags==CAPDB_ACCESS_READWRITE ) zArg = "CAPDB_ACCESS_READWRITE";
    if( flags==CAPDB_ACCESS_READ ) zArg = "CAPDB_ACCESS_READ";
    tvfsExecTcl(p, "xAccess", 
        Tcl_NewStringObj(zPath, -1), Tcl_NewStringObj(zArg, -1), 0, 0
    );
    if( tvfsResultCode(p, &rc) ){
      if( rc!=CAPDB_OK ) return rc;
    }else{
      Tcl_Interp *interp = p->interp;
      if( TCL_OK==Tcl_GetBooleanFromObj(0, Tcl_GetObjResult(interp), pResOut) ){
        return CAPDB_OK;
      }
    }
  }
  return capdbOsAccess(PARENTVFS(pVfs), zPath, flags, pResOut);
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int tvfsFullPathname(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  Testvfs *p = (Testvfs *)pVfs->pAppData;
  if( p->pScript && p->mask&TESTVFS_FULLPATHNAME_MASK ){
    int rc;
    tvfsExecTcl(p, "xFullPathname", Tcl_NewStringObj(zPath, -1), 0, 0, 0);
    if( tvfsResultCode(p, &rc) ){
      if( rc!=CAPDB_OK ) return rc;
    }
  }
  return capdbOsFullPathname(PARENTVFS(pVfs), zPath, nOut, zOut);
}

#ifndef CAPDB_OMIT_LOAD_EXTENSION
/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *tvfsDlOpen(capdb_vfs *pVfs, const char *zPath){
  return capdbOsDlOpen(PARENTVFS(pVfs), zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void tvfsDlError(capdb_vfs *pVfs, int nByte, char *zErrMsg){
  capdbOsDlError(PARENTVFS(pVfs), nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*tvfsDlSym(capdb_vfs *pVfs, void *p, const char *zSym))(void){
  return capdbOsDlSym(PARENTVFS(pVfs), p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void tvfsDlClose(capdb_vfs *pVfs, void *pHandle){
  capdbOsDlClose(PARENTVFS(pVfs), pHandle);
}
#endif /* CAPDB_OMIT_LOAD_EXTENSION */

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int tvfsRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  return capdbOsRandomness(PARENTVFS(pVfs), nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int tvfsSleep(capdb_vfs *pVfs, int nMicro){
  Testvfs *p = (Testvfs *)pVfs->pAppData;
  if( p->pScript && (p->mask&TESTVFS_SLEEP_MASK) ){
    tvfsExecTcl(p, "xSleep", Tcl_NewIntObj(nMicro), 0, 0, 0);
  }
  return capdbOsSleep(PARENTVFS(pVfs), nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int tvfsCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  return PARENTVFS(pVfs)->xCurrentTime(PARENTVFS(pVfs), pTimeOut);
}

static int tvfsShmOpen(capdb_file *pFile){
  Testvfs *p;
  int rc = CAPDB_OK;             /* Return code */
  TestvfsBuffer *pBuffer;         /* Buffer to open connection to */
  TestvfsFd *pFd;                 /* The testvfs file structure */

  pFd = tvfsGetFd(pFile);
  p = (Testvfs *)pFd->pVfs->pAppData;
  assert( 0==p->isFullshm );
  assert( pFd->pShmId && pFd->pShm==0 && pFd->pNext==0 );

  /* Evaluate the Tcl script: 
  **
  **   SCRIPT xShmOpen FILENAME
  */
  Tcl_ResetResult(p->interp);
  if( p->pScript && p->mask&TESTVFS_SHMOPEN_MASK ){
    tvfsExecTcl(p, "xShmOpen", Tcl_NewStringObj(pFd->zFilename, -1), 0, 0, 0);
    if( tvfsResultCode(p, &rc) ){
      if( rc!=CAPDB_OK ) return rc;
    }
  }

  assert( rc==CAPDB_OK );
  if( p->mask&TESTVFS_SHMOPEN_MASK && tvfsInjectIoerr(p) ){
    return CAPDB_IOERR;
  }

  /* Search for a TestvfsBuffer. Create a new one if required. */
  for(pBuffer=p->pBuffer; pBuffer; pBuffer=pBuffer->pNext){
    if( 0==strcmp(pFd->zFilename, pBuffer->zFile) ) break;
  }
  if( !pBuffer ){
    int szName = (int)strlen(pFd->zFilename);
    int nByte = sizeof(TestvfsBuffer) + szName + 1;
    pBuffer = (TestvfsBuffer *)ckalloc(nByte);
    memset(pBuffer, 0, nByte);
    pBuffer->zFile = (char *)&pBuffer[1];
    memcpy(pBuffer->zFile, pFd->zFilename, szName+1);
    pBuffer->pNext = p->pBuffer;
    p->pBuffer = pBuffer;
  }

  /* Connect the TestvfsBuffer to the new TestvfsShm handle and return. */
  pFd->pNext = pBuffer->pFile;
  pBuffer->pFile = pFd;
  pFd->pShm = pBuffer;
  return rc;
}

static void tvfsAllocPage(TestvfsBuffer *p, int iPage, int pgsz){
  assert( iPage<TESTVFS_MAX_PAGES );
  if( p->aPage[iPage]==0 ){
    p->aPage[iPage] = (u8 *)ckalloc(pgsz);
    memset(p->aPage[iPage], 0, pgsz);
    p->pgsz = pgsz;
  }
}

static int tvfsShmMap(
  capdb_file *pFile,            /* Handle open on database file */
  int iPage,                      /* Page to retrieve */
  int pgsz,                       /* Size of pages */
  int isWrite,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)(pFd->pVfs->pAppData);

  if( p->isFullshm ){
    capdb_file *pReal = pFd->pReal;
    return pReal->pMethods->xShmMap(pReal, iPage, pgsz, isWrite, pp);
  }

  if( 0==pFd->pShm ){
    rc = tvfsShmOpen(pFile);
    if( rc!=CAPDB_OK ){
      return rc;
    }
  }

  if( p->pScript && p->mask&TESTVFS_SHMMAP_MASK ){
    Tcl_Obj *pArg = Tcl_NewObj();
    Tcl_IncrRefCount(pArg);
    Tcl_ListObjAppendElement(p->interp, pArg, Tcl_NewIntObj(iPage));
    Tcl_ListObjAppendElement(p->interp, pArg, Tcl_NewIntObj(pgsz));
    Tcl_ListObjAppendElement(p->interp, pArg, Tcl_NewIntObj(isWrite));
    tvfsExecTcl(p, "xShmMap", 
        Tcl_NewStringObj(pFd->pShm->zFile, -1), pFd->pShmId, pArg, 0
    );
    tvfsResultCode(p, &rc);
    Tcl_DecrRefCount(pArg);
  }
  if( rc==CAPDB_OK && p->mask&TESTVFS_SHMMAP_MASK && tvfsInjectIoerr(p) ){
    rc = CAPDB_IOERR;
  }

  if( rc==CAPDB_OK && isWrite && !pFd->pShm->aPage[iPage] ){
    tvfsAllocPage(pFd->pShm, iPage, pgsz);
  }
  if( rc==CAPDB_OK || rc==CAPDB_READONLY ){
    *pp = (void volatile *)pFd->pShm->aPage[iPage];
  }

  return rc;
}


static int tvfsShmLock(
  capdb_file *pFile,
  int ofst,
  int n,
  int flags
){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)(pFd->pVfs->pAppData);
  int nLock;
  char zLock[80];

  if( p->isFullshm ){
    capdb_file *pReal = pFd->pReal;
    return pReal->pMethods->xShmLock(pReal, ofst, n, flags);
  }

  if( p->pScript && p->mask&TESTVFS_SHMLOCK_MASK ){
    capdb_snprintf(sizeof(zLock), zLock, "%d %d", ofst, n);
    nLock = (int)strlen(zLock);
    if( flags & CAPDB_SHM_LOCK ){
      strcpy(&zLock[nLock], " lock");
    }else{
      strcpy(&zLock[nLock], " unlock");
    }
    nLock += (int)strlen(&zLock[nLock]);
    if( flags & CAPDB_SHM_SHARED ){
      strcpy(&zLock[nLock], " shared");
    }else{
      strcpy(&zLock[nLock], " exclusive");
    }
    tvfsExecTcl(p, "xShmLock", 
        Tcl_NewStringObj(pFd->pShm->zFile, -1), pFd->pShmId,
        Tcl_NewStringObj(zLock, -1), 0
    );
    tvfsResultCode(p, &rc);
  }

  if( rc==CAPDB_OK && p->mask&TESTVFS_SHMLOCK_MASK && tvfsInjectIoerr(p) ){
    rc = CAPDB_IOERR;
  }

  if( rc==CAPDB_OK ){
    int isLock = (flags & CAPDB_SHM_LOCK);
    int isExcl = (flags & CAPDB_SHM_EXCLUSIVE);
    u32 mask = (((1<<n)-1) << ofst);
    if( isLock ){
      TestvfsFd *p2;
      for(p2=pFd->pShm->pFile; p2; p2=p2->pNext){
        if( p2==pFd ) continue;
        if( (p2->excllock&mask) || (isExcl && p2->sharedlock&mask) ){
          rc = CAPDB_BUSY;
          break;
        }
      }
      if( rc==CAPDB_OK ){
        if( isExcl )  pFd->excllock |= mask;
        if( !isExcl ) pFd->sharedlock |= mask;
      }
    }else{
      if( isExcl )  pFd->excllock &= (~mask);
      if( !isExcl ) pFd->sharedlock &= (~mask);
    }
  }

  return rc;
}

static void tvfsShmBarrier(capdb_file *pFile){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)(pFd->pVfs->pAppData);

  if( p->pScript && p->mask&TESTVFS_SHMBARRIER_MASK ){
    const char *z = pFd->pShm ? pFd->pShm->zFile : "";
    tvfsExecTcl(p, "xShmBarrier", Tcl_NewStringObj(z, -1), pFd->pShmId, 0, 0);
  }

  if( p->isFullshm ){
    capdb_file *pReal = pFd->pReal;
    pReal->pMethods->xShmBarrier(pReal);
    return;
  }
}

static int tvfsShmUnmap(
  capdb_file *pFile,
  int deleteFlag
){
  int rc = CAPDB_OK;
  TestvfsFd *pFd = tvfsGetFd(pFile);
  Testvfs *p = (Testvfs *)(pFd->pVfs->pAppData);
  TestvfsBuffer *pBuffer = pFd->pShm;
  TestvfsFd **ppFd;

  if( p->isFullshm ){
    capdb_file *pReal = pFd->pReal;
    return pReal->pMethods->xShmUnmap(pReal, deleteFlag);
  }

  if( !pBuffer ) return CAPDB_OK;
  assert( pFd->pShmId && pFd->pShm );

  if( p->pScript && p->mask&TESTVFS_SHMCLOSE_MASK ){
    tvfsExecTcl(p, "xShmUnmap", 
        Tcl_NewStringObj(pFd->pShm->zFile, -1), pFd->pShmId, 0, 0
    );
    tvfsResultCode(p, &rc);
  }

  for(ppFd=&pBuffer->pFile; *ppFd!=pFd; ppFd=&((*ppFd)->pNext));
  assert( (*ppFd)==pFd );
  *ppFd = pFd->pNext;
  pFd->pNext = 0;

  if( pBuffer->pFile==0 ){
    int i;
    TestvfsBuffer **pp;
    for(pp=&p->pBuffer; *pp!=pBuffer; pp=&((*pp)->pNext));
    *pp = (*pp)->pNext;
    for(i=0; pBuffer->aPage[i]; i++){
      ckfree((char *)pBuffer->aPage[i]);
    }
    ckfree((char *)pBuffer);
  }
  pFd->pShm = 0;

  return rc;
}

static int tvfsFetch(
    capdb_file *pFile, 
    capdb_int64 iOfst, 
    int iAmt, 
    void **pp
){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  return capdbOsFetch(pFd->pReal, iOfst, iAmt, pp);
}

static int tvfsUnfetch(capdb_file *pFile, capdb_int64 iOfst, void *p){
  TestvfsFd *pFd = tvfsGetFd(pFile);
  return capdbOsUnfetch(pFd->pReal, iOfst, p);
}

static int CAPDB_TCLAPI testvfs_obj_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Testvfs *p = (Testvfs *)cd;

  enum DB_enum { 
    CMD_SHM, CMD_DELETE, CMD_FILTER, CMD_IOERR, CMD_SCRIPT, 
    CMD_DEVCHAR, CMD_SECTORSIZE, CMD_FULLERR, CMD_CANTOPENERR
  };
  struct TestvfsSubcmd {
    char *zName;
    enum DB_enum eCmd;
  } aSubcmd[] = {
    { "shm",         CMD_SHM         },
    { "delete",      CMD_DELETE      },
    { "filter",      CMD_FILTER      },
    { "ioerr",       CMD_IOERR       },
    { "fullerr",     CMD_FULLERR     },
    { "cantopenerr", CMD_CANTOPENERR },
    { "script",      CMD_SCRIPT      },
    { "devchar",     CMD_DEVCHAR     },
    { "sectorsize",  CMD_SECTORSIZE  },
    { 0, 0 }
  };
  int i;
  
  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObjStruct(
        interp, objv[1], aSubcmd, sizeof(aSubcmd[0]), "subcommand", 0, &i) 
  ){
    return TCL_ERROR;
  }
  Tcl_ResetResult(interp);

  switch( aSubcmd[i].eCmd ){
    case CMD_SHM: {
      Tcl_Obj *pObj;
      int rc;
      TestvfsBuffer *pBuffer;
      char *zName;
      if( objc!=3 && objc!=4 ){
        Tcl_WrongNumArgs(interp, 2, objv, "FILE ?VALUE?");
        return TCL_ERROR;
      }
      zName = ckalloc(p->pParent->mxPathname);
      rc = p->pParent->xFullPathname(
          p->pParent, Tcl_GetString(objv[2]), 
          p->pParent->mxPathname, zName
      );
      if( rc!=CAPDB_OK ){
        Tcl_AppendResult(interp, "failed to get full path: ",
                         Tcl_GetString(objv[2]), NULL);
        ckfree(zName);
        return TCL_ERROR;
      }
      for(pBuffer=p->pBuffer; pBuffer; pBuffer=pBuffer->pNext){
        if( 0==strcmp(pBuffer->zFile, zName) ) break;
      }
      ckfree(zName);
      if( !pBuffer ){
        Tcl_AppendResult(interp, "no such file: ", Tcl_GetString(objv[2]), NULL);
        return TCL_ERROR;
      }
      if( objc==4 ){
        Tcl_Size n;
        u8 *a = Tcl_GetByteArrayFromObj(objv[3], &n);
        int pgsz = pBuffer->pgsz;
        if( pgsz==0 ) pgsz = 65536;
        for(i=0; i*pgsz<(int)n; i++){
          int nByte = pgsz;
          tvfsAllocPage(pBuffer, i, pgsz);
          if( n-i*pgsz<pgsz ){
            nByte = (int)n;
          }
          memcpy(pBuffer->aPage[i], &a[i*pgsz], nByte);
        }
      }

      pObj = Tcl_NewObj();
      for(i=0; pBuffer->aPage[i]; i++){
        int pgsz = pBuffer->pgsz;
        if( pgsz==0 ) pgsz = 65536;
        Tcl_AppendObjToObj(pObj, Tcl_NewByteArrayObj(pBuffer->aPage[i], pgsz));
      }
      Tcl_SetObjResult(interp, pObj);
      break;
    }

    /*  TESTVFS filter METHOD-LIST
    **
    **     Activate special processing for those methods contained in the list
    */
    case CMD_FILTER: {
      static struct VfsMethod {
        char *zName;
        int mask;
      } vfsmethod [] = {
        { "xShmOpen",           TESTVFS_SHMOPEN_MASK },
        { "xShmLock",           TESTVFS_SHMLOCK_MASK },
        { "xShmBarrier",        TESTVFS_SHMBARRIER_MASK },
        { "xShmUnmap",          TESTVFS_SHMCLOSE_MASK },
        { "xShmMap",            TESTVFS_SHMMAP_MASK },
        { "xSync",              TESTVFS_SYNC_MASK },
        { "xDelete",            TESTVFS_DELETE_MASK },
        { "xWrite",             TESTVFS_WRITE_MASK },
        { "xRead",              TESTVFS_READ_MASK },
        { "xTruncate",          TESTVFS_TRUNCATE_MASK },
        { "xOpen",              TESTVFS_OPEN_MASK },
        { "xClose",             TESTVFS_CLOSE_MASK },
        { "xAccess",            TESTVFS_ACCESS_MASK },
        { "xFullPathname",      TESTVFS_FULLPATHNAME_MASK },
        { "xUnlock",            TESTVFS_UNLOCK_MASK },
        { "xLock",              TESTVFS_LOCK_MASK },
        { "xCheckReservedLock", TESTVFS_CKLOCK_MASK },
        { "xFileControl",       TESTVFS_FCNTL_MASK },
        { "xSleep",             TESTVFS_SLEEP_MASK },
      };
      Tcl_Obj **apElem = 0;
      Tcl_Size nElem = 0;
      int mask = 0;
      if( objc!=3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "LIST");
        return TCL_ERROR;
      }
      if( Tcl_ListObjGetElements(interp, objv[2], &nElem, &apElem) ){
        return TCL_ERROR;
      }
      Tcl_ResetResult(interp);
      for(i=0; i<(int)nElem; i++){
        int iMethod;
        char *zElem = Tcl_GetString(apElem[i]);
        for(iMethod=0; iMethod<ArraySize(vfsmethod); iMethod++){
          if( strcmp(zElem, vfsmethod[iMethod].zName)==0 ){
            mask |= vfsmethod[iMethod].mask;
            break;
          }
        }
        if( iMethod==ArraySize(vfsmethod) ){
          Tcl_AppendResult(interp, "unknown method: ", zElem, NULL);
          return TCL_ERROR;
        }
      }
      p->mask = mask;
      break;
    }

    /*
    **  TESTVFS script ?SCRIPT?
    **
    **  Query or set the script to be run when filtered VFS events
    **  occur.
    */
    case CMD_SCRIPT: {
      if( objc==3 ){
        Tcl_Size nByte;
        if( p->pScript ){
          Tcl_DecrRefCount(p->pScript);
          p->pScript = 0;
        }
        Tcl_GetStringFromObj(objv[2], &nByte);
        if( nByte>0 ){
          p->pScript = Tcl_DuplicateObj(objv[2]);
          Tcl_IncrRefCount(p->pScript);
        }
      }else if( objc!=2 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?SCRIPT?");
        return TCL_ERROR;
      }

      Tcl_ResetResult(interp);
      if( p->pScript ) Tcl_SetObjResult(interp, p->pScript);

      break;
    }

    /*
    ** TESTVFS ioerr ?IFAIL PERSIST?
    **
    **   Where IFAIL is an integer and PERSIST is boolean.
    */
    case CMD_CANTOPENERR:
    case CMD_IOERR:
    case CMD_FULLERR: {
      TestFaultInject *pTest = 0;
      int iRet;

      switch( aSubcmd[i].eCmd ){
        case CMD_IOERR: pTest = &p->ioerr_err; break;
        case CMD_FULLERR: pTest = &p->full_err; break;
        case CMD_CANTOPENERR: pTest = &p->cantopen_err; break;
        default: assert(0);
      }
      iRet = pTest->nFail;
      pTest->nFail = 0;
      pTest->eFault = 0;
      pTest->iCnt = 0;

      if( objc==4 ){
        int iCnt, iPersist;
        if( TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &iCnt)
         || TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[3], &iPersist)
        ){
          return TCL_ERROR;
        }
        pTest->eFault = iPersist?FAULT_INJECT_PERSISTENT:FAULT_INJECT_TRANSIENT;
        pTest->iCnt = iCnt;
      }else if( objc!=2 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?CNT PERSIST?");
        return TCL_ERROR;
      }
      Tcl_SetObjResult(interp, Tcl_NewIntObj(iRet));
      break;
    }

    case CMD_DELETE: {
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;
    }

    case CMD_DEVCHAR: {
      struct DeviceFlag {
        char *zName;
        int iValue;
      } aFlag[] = {
        { "default",               -1 },
        { "atomic",                CAPDB_IOCAP_ATOMIC                },
        { "atomic512",             CAPDB_IOCAP_ATOMIC512             },
        { "atomic1k",              CAPDB_IOCAP_ATOMIC1K              },
        { "atomic2k",              CAPDB_IOCAP_ATOMIC2K              },
        { "atomic4k",              CAPDB_IOCAP_ATOMIC4K              },
        { "atomic8k",              CAPDB_IOCAP_ATOMIC8K              },
        { "atomic16k",             CAPDB_IOCAP_ATOMIC16K             },
        { "atomic32k",             CAPDB_IOCAP_ATOMIC32K             },
        { "atomic64k",             CAPDB_IOCAP_ATOMIC64K             },
        { "sequential",            CAPDB_IOCAP_SEQUENTIAL            },
        { "safe_append",           CAPDB_IOCAP_SAFE_APPEND           },
        { "undeletable_when_open", CAPDB_IOCAP_UNDELETABLE_WHEN_OPEN },
        { "powersafe_overwrite",   CAPDB_IOCAP_POWERSAFE_OVERWRITE   },
        { "immutable",             CAPDB_IOCAP_IMMUTABLE             },
        { 0, 0 }
      };
      Tcl_Obj *pRet;
      int iFlag;

      if( objc>3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?ATTR-LIST?");
        return TCL_ERROR;
      }
      if( objc==3 ){
        int j;
        int iNew = 0;
        Tcl_Obj **flags = 0;
        Tcl_Size nFlags = 0;

        if( Tcl_ListObjGetElements(interp, objv[2], &nFlags, &flags) ){
          return TCL_ERROR;
        }

        for(j=0; j<(int)nFlags; j++){
          int idx = 0;
          if( Tcl_GetIndexFromObjStruct(interp, flags[j], aFlag, 
                sizeof(aFlag[0]), "flag", 0, &idx) 
          ){
            return TCL_ERROR;
          }
          if( aFlag[idx].iValue<0 && nFlags>1 ){
            Tcl_AppendResult(interp, "bad flags: ", Tcl_GetString(objv[2]), NULL);
            return TCL_ERROR;
          }
          iNew |= aFlag[idx].iValue;
        }

        p->iDevchar = iNew| 0x10000000;
      }

      pRet = Tcl_NewObj();
      for(iFlag=0; iFlag<sizeof(aFlag)/sizeof(aFlag[0]); iFlag++){
        if( p->iDevchar & aFlag[iFlag].iValue ){
          Tcl_ListObjAppendElement(
              interp, pRet, Tcl_NewStringObj(aFlag[iFlag].zName, -1)
          );
        }
      }
      Tcl_SetObjResult(interp, pRet);

      break;
    }

    case CMD_SECTORSIZE: {
      if( objc>3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "?VALUE?");
        return TCL_ERROR;
      }
      if( objc==3 ){
        int iNew = 0;
        if( Tcl_GetIntFromObj(interp, objv[2], &iNew) ){
          return TCL_ERROR;
        }
        p->iSectorsize = iNew;
      }
      Tcl_SetObjResult(interp, Tcl_NewIntObj(p->iSectorsize));
      break;
    }
  }

  return TCL_OK;
}

static void CAPDB_TCLAPI testvfs_obj_del(ClientData cd){
  Testvfs *p = (Testvfs *)cd;
  if( p->pScript ) Tcl_DecrRefCount(p->pScript);
  capdb_vfs_unregister(p->pVfs);
  memset(p->pVfs, 0, sizeof(capdb_vfs));
  ckfree((char *)p->pVfs);
  memset(p, 0, sizeof(Testvfs));
  ckfree((char *)p);
}

/*
** Usage:  testvfs VFSNAME ?SWITCHES?
**
** Switches are:
**
**   -noshm   BOOLEAN             (True to omit shm methods. Default false)
**   -default BOOLEAN             (True to make the vfs default. Default false)
**
** This command creates two things when it is invoked: an SQLite VFS, and
** a Tcl command. Both are named VFSNAME. The VFS is installed. It is not
** installed as the default VFS.
**
** The VFS passes all file I/O calls through to the underlying VFS.
**
** Whenever the xShmMap method of the VFS
** is invoked, the SCRIPT is executed as follows:
**
**   SCRIPT xShmMap    FILENAME ID
**
** The value returned by the invocation of SCRIPT above is interpreted as
** an SQLite error code and returned to SQLite. Either a symbolic 
** "CAPDB_OK" or numeric "0" value may be returned.
**
** The contents of the shared-memory buffer associated with a given file
** may be read and set using the following command:
**
**   VFSNAME shm FILENAME ?NEWVALUE?
**
** When the xShmLock method is invoked by SQLite, the following script is
** run:
**
**   SCRIPT xShmLock    FILENAME ID LOCK
**
** where LOCK is of the form "OFFSET NBYTE lock/unlock shared/exclusive"
*/
static int CAPDB_TCLAPI testvfs_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static capdb_vfs tvfs_vfs = {
    3,                            /* iVersion */
    0,                            /* szOsFile */
    0,                            /* mxPathname */
    0,                            /* pNext */
    0,                            /* zName */
    0,                            /* pAppData */
    tvfsOpen,                     /* xOpen */
    tvfsDelete,                   /* xDelete */
    tvfsAccess,                   /* xAccess */
    tvfsFullPathname,             /* xFullPathname */
#ifndef CAPDB_OMIT_LOAD_EXTENSION
    tvfsDlOpen,                   /* xDlOpen */
    tvfsDlError,                  /* xDlError */
    tvfsDlSym,                    /* xDlSym */
    tvfsDlClose,                  /* xDlClose */
#else
    0,                            /* xDlOpen */
    0,                            /* xDlError */
    0,                            /* xDlSym */
    0,                            /* xDlClose */
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
    tvfsRandomness,               /* xRandomness */
    tvfsSleep,                    /* xSleep */
    tvfsCurrentTime,              /* xCurrentTime */
    0,                            /* xGetLastError */
    0,                            /* xCurrentTimeInt64 */
    0,                            /* xSetSystemCall */
    0,                            /* xGetSystemCall */
    0,                            /* xNextSystemCall */
  };

  Testvfs *p;                     /* New object */
  capdb_vfs *pVfs;              /* New VFS */
  char *zVfs;
  int nByte;                      /* Bytes of space to allocate at p */

  int i;
  int isNoshm = 0;                /* True if -noshm is passed */
  int isFullshm = 0;              /* True if -fullshm is passed */
  int isDefault = 0;              /* True if -default is passed */
  int szOsFile = 0;               /* Value passed to -szosfile */
  int mxPathname = -1;            /* Value passed to -mxpathname */
  int iVersion = 3;               /* Value passed to -iversion */

  if( objc<2 || 0!=(objc%2) ) goto bad_args;
  for(i=2; i<objc; i += 2){
    Tcl_Size nSwitch;
    char *zSwitch;
    zSwitch = Tcl_GetStringFromObj(objv[i], &nSwitch); 

    if( nSwitch>2 && 0==strncmp("-noshm", zSwitch, nSwitch) ){
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &isNoshm) ){
        return TCL_ERROR;
      }
      if( isNoshm ) isFullshm = 0;
    }
    else if( nSwitch>2 && 0==strncmp("-default", zSwitch, nSwitch) ){
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &isDefault) ){
        return TCL_ERROR;
      }
    }
    else if( nSwitch>2 && 0==strncmp("-szosfile", zSwitch, nSwitch) ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &szOsFile) ){
        return TCL_ERROR;
      }
    }
    else if( nSwitch>2 && 0==strncmp("-mxpathname", zSwitch, nSwitch) ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &mxPathname) ){
        return TCL_ERROR;
      }
    }
    else if( nSwitch>2 && 0==strncmp("-iversion", zSwitch, nSwitch) ){
      if( Tcl_GetIntFromObj(interp, objv[i+1], &iVersion) ){
        return TCL_ERROR;
      }
    }
    else if( nSwitch>2 && 0==strncmp("-fullshm", zSwitch, nSwitch) ){
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &isFullshm) ){
        return TCL_ERROR;
      }
      if( isFullshm ) isNoshm = 0;
    }
    else{
      goto bad_args;
    }
  }

  if( szOsFile<sizeof(TestvfsFile) ){
    szOsFile = sizeof(TestvfsFile);
  }

  zVfs = Tcl_GetString(objv[1]);
  nByte = sizeof(Testvfs) + (int)strlen(zVfs)+1;
  p = (Testvfs *)ckalloc(nByte);
  memset(p, 0, nByte);
  p->iDevchar = -1;
  p->iSectorsize = -1;

  /* Create the new object command before querying SQLite for a default VFS
  ** to use for 'real' IO operations. This is because creating the new VFS
  ** may delete an existing [testvfs] VFS of the same name. If such a VFS
  ** is currently the default, the new [testvfs] may end up calling the 
  ** methods of a deleted object.
  */
  Tcl_CreateObjCommand(interp, zVfs, testvfs_obj_cmd, p, testvfs_obj_del);
  p->pParent = capdb_vfs_find(0);
  p->interp = interp;

  p->zName = (char *)&p[1];
  memcpy(p->zName, zVfs, strlen(zVfs)+1);

  pVfs = (capdb_vfs *)ckalloc(sizeof(capdb_vfs));
  memcpy(pVfs, &tvfs_vfs, sizeof(capdb_vfs));
  pVfs->pAppData = (void *)p;
  pVfs->iVersion = iVersion;
  pVfs->zName = p->zName;
  pVfs->mxPathname = p->pParent->mxPathname;
  if( mxPathname>=0 && mxPathname<pVfs->mxPathname ){
    pVfs->mxPathname = mxPathname;
  }
  pVfs->szOsFile = szOsFile;
  p->pVfs = pVfs;
  p->isNoshm = isNoshm;
  p->isFullshm = isFullshm;
  p->mask = TESTVFS_ALL_MASK;

  capdb_vfs_register(pVfs, isDefault);

  return TCL_OK;

 bad_args:
  Tcl_WrongNumArgs(interp, 1, objv, "VFSNAME ?-noshm BOOL? ?-fullshm BOOL? ?-default BOOL? ?-mxpathname INT? ?-szosfile INT? ?-iversion INT?");
  return TCL_ERROR;
}

extern int getDbPointer(Tcl_Interp *interp, const char *zA, capdb **ppDb);
extern const char *capdbErrName(int);

/*
** tclcmd: vfs_shmlock DB DBNAME (shared|exclusive) (lock|unlock) OFFSET N
*/
static int CAPDB_TCLAPI test_vfs_shmlock(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *azArg1[] = {"shared", "exclusive", 0};
  const char *azArg2[] = {"lock", "unlock", 0};
  capdb *db = 0;
  int rc = CAPDB_OK;
  const char *zDbname = 0;
  int iArg1 = 0;
  int iArg2 = 0;
  int iOffset = 0;
  int n = 0;
  capdb_file *pFd;

  if( objc!=7 ){
    Tcl_WrongNumArgs(interp, 1, objv, 
        "DB DBNAME (shared|exclusive) (lock|unlock) OFFSET N"
    );
    return TCL_ERROR;
  }

  zDbname = Tcl_GetString(objv[2]);
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) 
   || Tcl_GetIndexFromObj(interp, objv[3], azArg1, "ARG", 0, &iArg1) 
   || Tcl_GetIndexFromObj(interp, objv[4], azArg2, "ARG", 0, &iArg2) 
   || Tcl_GetIntFromObj(interp, objv[5], &iOffset)
   || Tcl_GetIntFromObj(interp, objv[6], &n)
  ){
    return TCL_ERROR;
  }

  capdb_file_control(db, zDbname, CAPDB_FCNTL_FILE_POINTER, (void*)&pFd);
  if( pFd==0 ){
    return TCL_ERROR;
  }
  rc = pFd->pMethods->xShmLock(pFd, iOffset, n, 
      (iArg1==0 ? CAPDB_SHM_SHARED : CAPDB_SHM_EXCLUSIVE)
    | (iArg2==0 ? CAPDB_SHM_LOCK : CAPDB_SHM_UNLOCK)
  );
  Tcl_SetObjResult(interp, Tcl_NewStringObj(capdbErrName(rc), -1));
  return TCL_OK;
}

static int CAPDB_TCLAPI test_vfs_set_readmark(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db = 0;
  int rc = CAPDB_OK;
  const char *zDbname = 0;
  int iSlot = 0;
  int iVal = -1;
  capdb_file *pFd;
  void volatile *pShm = 0;
  u32 *aShm;
  int iOff;

  if( objc!=4 && objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB DBNAME SLOT ?VALUE?");
    return TCL_ERROR;
  }

  zDbname = Tcl_GetString(objv[2]);
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) 
   || Tcl_GetIntFromObj(interp, objv[3], &iSlot)
   || (objc==5 && Tcl_GetIntFromObj(interp, objv[4], &iVal))
  ){
    return TCL_ERROR;
  }

  capdb_file_control(db, zDbname, CAPDB_FCNTL_FILE_POINTER, (void*)&pFd);
  if( pFd==0 ){
    return TCL_ERROR;
  }
  rc = pFd->pMethods->xShmMap(pFd, 0, 32*1024, 0, &pShm);
  if( rc!=CAPDB_OK ){
    Tcl_SetObjResult(interp, Tcl_NewStringObj(capdbErrName(rc), -1));
    return TCL_ERROR;
  }
  if( pShm==0 ){
    Tcl_AppendResult(interp, "*-shm is not yet mapped", NULL);
    return TCL_ERROR;
  }
  aShm = (u32*)pShm;
  iOff = 12*2+1+iSlot;

  if( objc==5 ){
    aShm[iOff] = iVal;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(aShm[iOff]));

  return TCL_OK;
}

int Sqlitetestvfs_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "testvfs", testvfs_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "vfs_shmlock", test_vfs_shmlock, 0, 0);
  Tcl_CreateObjCommand(interp, "vfs_set_readmark", test_vfs_set_readmark, 0, 0);
  return TCL_OK;
}

#endif
