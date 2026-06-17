/*
** 2007 September 14
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** OVERVIEW:
**
**   This file contains some example code demonstrating how the SQLite 
**   vfs feature can be used to have SQLite operate directly on an 
**   embedded media, without using an intermediate file system.
**
**   Because this is only a demo designed to run on a workstation, the
**   underlying media is simulated using a regular file-system file. The
**   size of the file is fixed when it is first created (default size 10 MB).
**   From SQLite's point of view, this space is used to store a single
**   database file and the journal file. 
**
**   Any statement journal created is stored in volatile memory obtained 
**   from capdb_malloc(). Any attempt to create a temporary database file 
**   will fail (CAPDB_IOERR). To prevent SQLite from attempting this,
**   it should be configured to store all temporary database files in 
**   main memory (see pragma "temp_store" or the CAPDB_TEMP_STORE compile 
**   time option).
**
** ASSUMPTIONS:
**
**   After it has been created, the blob file is accessed using the
**   following three functions only:
**
**       mediaRead();            - Read a 512 byte block from the file.
**       mediaWrite();           - Write a 512 byte block to the file.
**       mediaSync();            - Tell the media hardware to sync.
**
**   It is assumed that these can be easily implemented by any "real"
**   media vfs driver adapting this code.
**
** FILE FORMAT:
**
**   The basic principle is that the "database file" is stored at the
**   beginning of the 10 MB blob and grows in a forward direction. The 
**   "journal file" is stored at the end of the 10MB blob and grows
**   in the reverse direction. If, during a transaction, insufficient
**   space is available to expand either the journal or database file,
**   an CAPDB_FULL error is returned. The database file is never allowed
**   to consume more than 90% of the blob space. If SQLite tries to
**   create a file larger than this, CAPDB_FULL is returned.
**
**   No allowance is made for "wear-leveling", as is required by.
**   embedded devices in the absence of equivalent hardware features.
**
**   The first 512 block byte of the file is reserved for storing the
**   size of the "database file". It is updated as part of the sync()
**   operation. On startup, it can only be trusted if no journal file
**   exists. If a journal-file does exist, then it stores the real size
**   of the database region. The second and subsequent blocks store the 
**   actual database content.
**
**   The size of the "journal file" is not stored persistently in the 
**   file. When the system is running, the size of the journal file is
**   stored in volatile memory. When recovering from a crash, this vfs
**   reports a very large size for the journal file. The normal journal
**   header and checksum mechanisms serve to prevent SQLite from 
**   processing any data that lies past the logical end of the journal.
**
**   When SQLite calls OsDelete() to delete the journal file, the final
**   512 bytes of the blob (the area containing the first journal header)
**   are zeroed.
**
** LOCKING:
**
**   File locking is a no-op. Only one connection may be open at any one
**   time using this demo vfs.
*/

#include "capdb.h"
#include <assert.h>
#include <string.h>

/*
** Maximum pathname length supported by the fs backend.
*/
#define BLOCKSIZE 512
#define BLOBSIZE 10485760

/*
** Name used to identify this VFS.
*/
#define FS_VFS_NAME "fs"

typedef struct fs_real_file fs_real_file;
struct fs_real_file {
  capdb_file *pFile;
  const char *zName;
  int nDatabase;              /* Current size of database region */
  int nJournal;               /* Current size of journal region */
  int nBlob;                  /* Total size of allocated blob */
  int nRef;                   /* Number of pointers to this structure */
  fs_real_file *pNext;
  fs_real_file **ppThis;
};

typedef struct fs_file fs_file;
struct fs_file {
  capdb_file base;
  int eType;
  fs_real_file *pReal;
};

typedef struct tmp_file tmp_file;
struct tmp_file {
  capdb_file base;
  int nSize;
  int nAlloc;
  char *zAlloc;
};

/* Values for fs_file.eType. */
#define DATABASE_FILE   1
#define JOURNAL_FILE    2

/*
** Method declarations for fs_file.
*/
static int fsClose(capdb_file*);
static int fsRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int fsWrite(capdb_file*, const void*, int iAmt, capdb_int64 iOfst);
static int fsTruncate(capdb_file*, capdb_int64 size);
static int fsSync(capdb_file*, int flags);
static int fsFileSize(capdb_file*, capdb_int64 *pSize);
static int fsLock(capdb_file*, int);
static int fsUnlock(capdb_file*, int);
static int fsCheckReservedLock(capdb_file*, int *pResOut);
static int fsFileControl(capdb_file*, int op, void *pArg);
static int fsSectorSize(capdb_file*);
static int fsDeviceCharacteristics(capdb_file*);

/*
** Method declarations for tmp_file.
*/
static int tmpClose(capdb_file*);
static int tmpRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int tmpWrite(capdb_file*, const void*, int iAmt, capdb_int64 iOfst);
static int tmpTruncate(capdb_file*, capdb_int64 size);
static int tmpSync(capdb_file*, int flags);
static int tmpFileSize(capdb_file*, capdb_int64 *pSize);
static int tmpLock(capdb_file*, int);
static int tmpUnlock(capdb_file*, int);
static int tmpCheckReservedLock(capdb_file*, int *pResOut);
static int tmpFileControl(capdb_file*, int op, void *pArg);
static int tmpSectorSize(capdb_file*);
static int tmpDeviceCharacteristics(capdb_file*);

/*
** Method declarations for fs_vfs.
*/
static int fsOpen(capdb_vfs*, const char *, capdb_file*, int , int *);
static int fsDelete(capdb_vfs*, const char *zName, int syncDir);
static int fsAccess(capdb_vfs*, const char *zName, int flags, int *);
static int fsFullPathname(capdb_vfs*, const char *zName, int nOut,char *zOut);
static void *fsDlOpen(capdb_vfs*, const char *zFilename);
static void fsDlError(capdb_vfs*, int nByte, char *zErrMsg);
static void (*fsDlSym(capdb_vfs*,void*, const char *zSymbol))(void);
static void fsDlClose(capdb_vfs*, void*);
static int fsRandomness(capdb_vfs*, int nByte, char *zOut);
static int fsSleep(capdb_vfs*, int microseconds);
static int fsCurrentTime(capdb_vfs*, double*);


typedef struct fs_vfs_t fs_vfs_t;
struct fs_vfs_t {
  capdb_vfs base;
  fs_real_file *pFileList;
  capdb_vfs *pParent;
};

static fs_vfs_t fs_vfs = {
  {
    1,                                          /* iVersion */
    0,                                          /* szOsFile */
    0,                                          /* mxPathname */
    0,                                          /* pNext */
    FS_VFS_NAME,                                /* zName */
    0,                                          /* pAppData */
    fsOpen,                                     /* xOpen */
    fsDelete,                                   /* xDelete */
    fsAccess,                                   /* xAccess */
    fsFullPathname,                             /* xFullPathname */
    fsDlOpen,                                   /* xDlOpen */
    fsDlError,                                  /* xDlError */
    fsDlSym,                                    /* xDlSym */
    fsDlClose,                                  /* xDlClose */
    fsRandomness,                               /* xRandomness */
    fsSleep,                                    /* xSleep */
    fsCurrentTime,                              /* xCurrentTime */
    0                                           /* xCurrentTimeInt64 */
  }, 
  0,                                            /* pFileList */
  0                                             /* pParent */
};

static capdb_io_methods fs_io_methods = {
  1,                            /* iVersion */
  fsClose,                      /* xClose */
  fsRead,                       /* xRead */
  fsWrite,                      /* xWrite */
  fsTruncate,                   /* xTruncate */
  fsSync,                       /* xSync */
  fsFileSize,                   /* xFileSize */
  fsLock,                       /* xLock */
  fsUnlock,                     /* xUnlock */
  fsCheckReservedLock,          /* xCheckReservedLock */
  fsFileControl,                /* xFileControl */
  fsSectorSize,                 /* xSectorSize */
  fsDeviceCharacteristics,      /* xDeviceCharacteristics */
  0,                            /* xShmMap */
  0,                            /* xShmLock */
  0,                            /* xShmBarrier */
  0                             /* xShmUnmap */
};


static capdb_io_methods tmp_io_methods = {
  1,                            /* iVersion */
  tmpClose,                     /* xClose */
  tmpRead,                      /* xRead */
  tmpWrite,                     /* xWrite */
  tmpTruncate,                  /* xTruncate */
  tmpSync,                      /* xSync */
  tmpFileSize,                  /* xFileSize */
  tmpLock,                      /* xLock */
  tmpUnlock,                    /* xUnlock */
  tmpCheckReservedLock,         /* xCheckReservedLock */
  tmpFileControl,               /* xFileControl */
  tmpSectorSize,                /* xSectorSize */
  tmpDeviceCharacteristics,     /* xDeviceCharacteristics */
  0,                            /* xShmMap */
  0,                            /* xShmLock */
  0,                            /* xShmBarrier */
  0                             /* xShmUnmap */
};

/* Useful macros used in several places */
#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))


/*
** Close a tmp-file.
*/
static int tmpClose(capdb_file *pFile){
  tmp_file *pTmp = (tmp_file *)pFile;
  capdb_free(pTmp->zAlloc);
  return CAPDB_OK;
}

/*
** Read data from a tmp-file.
*/
static int tmpRead(
  capdb_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  tmp_file *pTmp = (tmp_file *)pFile;
  if( (iAmt+iOfst)>pTmp->nSize ){
    return CAPDB_IOERR_SHORT_READ;
  }
  memcpy(zBuf, &pTmp->zAlloc[iOfst], iAmt);
  return CAPDB_OK;
}

/*
** Write data to a tmp-file.
*/
static int tmpWrite(
  capdb_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  tmp_file *pTmp = (tmp_file *)pFile;
  if( (iAmt+iOfst)>pTmp->nAlloc ){
    int nNew = (int)(2*(iAmt+iOfst+pTmp->nAlloc));
    char *zNew = capdb_realloc(pTmp->zAlloc, nNew);
    if( !zNew ){
      return CAPDB_NOMEM;
    }
    pTmp->zAlloc = zNew;
    pTmp->nAlloc = nNew;
  }
  memcpy(&pTmp->zAlloc[iOfst], zBuf, iAmt);
  pTmp->nSize = (int)MAX(pTmp->nSize, iOfst+iAmt);
  return CAPDB_OK;
}

/*
** Truncate a tmp-file.
*/
static int tmpTruncate(capdb_file *pFile, sqlite_int64 size){
  tmp_file *pTmp = (tmp_file *)pFile;
  pTmp->nSize = (int)MIN(pTmp->nSize, size);
  return CAPDB_OK;
}

/*
** Sync a tmp-file.
*/
static int tmpSync(capdb_file *pFile, int flags){
  return CAPDB_OK;
}

/*
** Return the current file-size of a tmp-file.
*/
static int tmpFileSize(capdb_file *pFile, sqlite_int64 *pSize){
  tmp_file *pTmp = (tmp_file *)pFile;
  *pSize = pTmp->nSize;
  return CAPDB_OK;
}

/*
** Lock a tmp-file.
*/
static int tmpLock(capdb_file *pFile, int eLock){
  return CAPDB_OK;
}

/*
** Unlock a tmp-file.
*/
static int tmpUnlock(capdb_file *pFile, int eLock){
  return CAPDB_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on a tmp-file.
*/
static int tmpCheckReservedLock(capdb_file *pFile, int *pResOut){
  *pResOut = 0;
  return CAPDB_OK;
}

/*
** File control method. For custom operations on a tmp-file.
*/
static int tmpFileControl(capdb_file *pFile, int op, void *pArg){
  return CAPDB_OK;
}

/*
** Return the sector-size in bytes for a tmp-file.
*/
static int tmpSectorSize(capdb_file *pFile){
  return 0;
}

/*
** Return the device characteristic flags supported by a tmp-file.
*/
static int tmpDeviceCharacteristics(capdb_file *pFile){
  return 0;
}

/*
** Close an fs-file.
*/
static int fsClose(capdb_file *pFile){
  int rc = CAPDB_OK;
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = p->pReal;

  /* Decrement the real_file ref-count. */
  pReal->nRef--;
  assert(pReal->nRef>=0);

  /* When the ref-count reaches 0, destroy the structure */
  if( pReal->nRef==0 ){
    *pReal->ppThis = pReal->pNext;
    if( pReal->pNext ){
      pReal->pNext->ppThis = pReal->ppThis;
    }
    rc = pReal->pFile->pMethods->xClose(pReal->pFile);
    capdb_free(pReal);
  }

  return rc;
}

/*
** Read data from an fs-file.
*/
static int fsRead(
  capdb_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc = CAPDB_OK;
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = p->pReal;
  capdb_file *pF = pReal->pFile;

  if( (p->eType==DATABASE_FILE && (iAmt+iOfst)>pReal->nDatabase)
   || (p->eType==JOURNAL_FILE && (iAmt+iOfst)>pReal->nJournal)
  ){
    rc = CAPDB_IOERR_SHORT_READ;
  }else if( p->eType==DATABASE_FILE ){
    rc = pF->pMethods->xRead(pF, zBuf, iAmt, iOfst+BLOCKSIZE);
  }else{
    /* Journal file. */
    int iRem = iAmt;
    int iBuf = 0;
    int ii = (int)iOfst;
    while( iRem>0 && rc==CAPDB_OK ){
      int iRealOff = pReal->nBlob - BLOCKSIZE*((ii/BLOCKSIZE)+1) + ii%BLOCKSIZE;
      int iRealAmt = MIN(iRem, BLOCKSIZE - (iRealOff%BLOCKSIZE));

      rc = pF->pMethods->xRead(pF, &((char *)zBuf)[iBuf], iRealAmt, iRealOff);
      ii += iRealAmt;
      iBuf += iRealAmt;
      iRem -= iRealAmt;
    }
  }

  return rc;
}

/*
** Write data to an fs-file.
*/
static int fsWrite(
  capdb_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc = CAPDB_OK;
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = p->pReal;
  capdb_file *pF = pReal->pFile;

  if( p->eType==DATABASE_FILE ){
    if( (iAmt+iOfst+BLOCKSIZE)>(pReal->nBlob-pReal->nJournal) ){
      rc = CAPDB_FULL;
    }else{
      rc = pF->pMethods->xWrite(pF, zBuf, iAmt, iOfst+BLOCKSIZE);
      if( rc==CAPDB_OK ){
        pReal->nDatabase = (int)MAX(pReal->nDatabase, iAmt+iOfst);
      }
    }
  }else{
    /* Journal file. */
    int iRem = iAmt;
    int iBuf = 0;
    int ii = (int)iOfst;
    while( iRem>0 && rc==CAPDB_OK ){
      int iRealOff = pReal->nBlob - BLOCKSIZE*((ii/BLOCKSIZE)+1) + ii%BLOCKSIZE;
      int iRealAmt = MIN(iRem, BLOCKSIZE - (iRealOff%BLOCKSIZE));

      if( iRealOff<(pReal->nDatabase+BLOCKSIZE) ){
        rc = CAPDB_FULL;
      }else{
        rc = pF->pMethods->xWrite(pF, &((char *)zBuf)[iBuf], iRealAmt,iRealOff);
        ii += iRealAmt;
        iBuf += iRealAmt;
        iRem -= iRealAmt;
      }
    }
    if( rc==CAPDB_OK ){
      pReal->nJournal = (int)MAX(pReal->nJournal, iAmt+iOfst);
    }
  }

  return rc;
}

/*
** Truncate an fs-file.
*/
static int fsTruncate(capdb_file *pFile, sqlite_int64 size){
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = p->pReal;
  if( p->eType==DATABASE_FILE ){
    pReal->nDatabase = (int)MIN(pReal->nDatabase, size);
  }else{
    pReal->nJournal = (int)MIN(pReal->nJournal, size);
  }
  return CAPDB_OK;
}

/*
** Sync an fs-file.
*/
static int fsSync(capdb_file *pFile, int flags){
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = p->pReal;
  capdb_file *pRealFile = pReal->pFile;
  int rc = CAPDB_OK;

  if( p->eType==DATABASE_FILE ){
    unsigned char zSize[4];
    zSize[0] = (pReal->nDatabase&0xFF000000)>>24;
    zSize[1] = (unsigned char)((pReal->nDatabase&0x00FF0000)>>16);
    zSize[2] = (pReal->nDatabase&0x0000FF00)>>8;
    zSize[3] = (pReal->nDatabase&0x000000FF);
    rc = pRealFile->pMethods->xWrite(pRealFile, zSize, 4, 0);
  }
  if( rc==CAPDB_OK ){
    rc = pRealFile->pMethods->xSync(pRealFile, flags&(~CAPDB_SYNC_DATAONLY));
  }

  return rc;
}

/*
** Return the current file-size of an fs-file.
*/
static int fsFileSize(capdb_file *pFile, sqlite_int64 *pSize){
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = p->pReal;
  if( p->eType==DATABASE_FILE ){
    *pSize = pReal->nDatabase;
  }else{
    *pSize = pReal->nJournal;
  }
  return CAPDB_OK;
}

/*
** Lock an fs-file.
*/
static int fsLock(capdb_file *pFile, int eLock){
  return CAPDB_OK;
}

/*
** Unlock an fs-file.
*/
static int fsUnlock(capdb_file *pFile, int eLock){
  return CAPDB_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on an fs-file.
*/
static int fsCheckReservedLock(capdb_file *pFile, int *pResOut){
  *pResOut = 0;
  return CAPDB_OK;
}

/*
** File control method. For custom operations on an fs-file.
*/
static int fsFileControl(capdb_file *pFile, int op, void *pArg){
  if( op==CAPDB_FCNTL_PRAGMA ) return CAPDB_NOTFOUND;
  return CAPDB_OK;
}

/*
** Return the sector-size in bytes for an fs-file.
*/
static int fsSectorSize(capdb_file *pFile){
  return BLOCKSIZE;
}

/*
** Return the device characteristic flags supported by an fs-file.
*/
static int fsDeviceCharacteristics(capdb_file *pFile){
  return 0;
}

/*
** Open an fs file handle.
*/
static int fsOpen(
  capdb_vfs *pVfs,
  const char *zName,
  capdb_file *pFile,
  int flags,
  int *pOutFlags
){
  fs_vfs_t *pFsVfs = (fs_vfs_t *)pVfs;
  fs_file *p = (fs_file *)pFile;
  fs_real_file *pReal = 0;
  int eType;
  int nName;
  int rc = CAPDB_OK;

  if( 0==(flags&(CAPDB_OPEN_MAIN_DB|CAPDB_OPEN_MAIN_JOURNAL)) ){
    tmp_file *p2 = (tmp_file *)pFile;
    memset(p2, 0, sizeof(*p2));
    p2->base.pMethods = &tmp_io_methods;
    return CAPDB_OK;
  }

  eType = ((flags&(CAPDB_OPEN_MAIN_DB))?DATABASE_FILE:JOURNAL_FILE);
  p->base.pMethods = &fs_io_methods;
  p->eType = eType;

  assert(strlen("-journal")==8);
  nName = (int)strlen(zName)-((eType==JOURNAL_FILE)?8:0);
  pReal=pFsVfs->pFileList; 
  for(; pReal && strncmp(pReal->zName, zName, nName); pReal=pReal->pNext);

  if( !pReal ){
    int real_flags = (flags&~(CAPDB_OPEN_MAIN_DB))|CAPDB_OPEN_TEMP_DB;
    capdb_int64 size;
    capdb_file *pRealFile;
    capdb_vfs *pParent = pFsVfs->pParent;
    assert(eType==DATABASE_FILE);

    pReal = (fs_real_file *)capdb_malloc(sizeof(*pReal)+pParent->szOsFile);
    if( !pReal ){
      rc = CAPDB_NOMEM;
      goto open_out;
    }
    memset(pReal, 0, sizeof(*pReal)+pParent->szOsFile);
    pReal->zName = zName;
    pReal->pFile = (capdb_file *)(&pReal[1]);

    rc = pParent->xOpen(pParent, zName, pReal->pFile, real_flags, pOutFlags);
    if( rc!=CAPDB_OK ){
      goto open_out;
    }
    pRealFile = pReal->pFile;

    rc = pRealFile->pMethods->xFileSize(pRealFile, &size);
    if( rc!=CAPDB_OK ){
      goto open_out;
    }
    if( size==0 ){
      rc = pRealFile->pMethods->xWrite(pRealFile, "\0", 1, BLOBSIZE-1);
      pReal->nBlob = BLOBSIZE;
    }else{
      unsigned char zS[4];
      pReal->nBlob = (int)size;
      rc = pRealFile->pMethods->xRead(pRealFile, zS, 4, 0);
      pReal->nDatabase = (zS[0]<<24)+(zS[1]<<16)+(zS[2]<<8)+zS[3];
      if( rc==CAPDB_OK ){
        rc = pRealFile->pMethods->xRead(pRealFile, zS, 4, pReal->nBlob-4);
        if( zS[0] || zS[1] || zS[2] || zS[3] ){
          pReal->nJournal = pReal->nBlob;
        }
      }
    }

    if( rc==CAPDB_OK ){
      pReal->pNext = pFsVfs->pFileList;
      if( pReal->pNext ){
        pReal->pNext->ppThis = &pReal->pNext;
      }
      pReal->ppThis = &pFsVfs->pFileList;
      pFsVfs->pFileList = pReal;
    }
  }

open_out:
  if( pReal ){
    if( rc==CAPDB_OK ){
      p->pReal = pReal;
      pReal->nRef++;
    }else{
      if( pReal->pFile->pMethods ){
        pReal->pFile->pMethods->xClose(pReal->pFile);
      }
      capdb_free(pReal);
    }
  }
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int fsDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  int rc = CAPDB_OK;
  fs_vfs_t *pFsVfs = (fs_vfs_t *)pVfs;
  fs_real_file *pReal;
  capdb_file *pF;
  int nName = (int)strlen(zPath) - 8;

  assert(strlen("-journal")==8);
  assert(strcmp("-journal", &zPath[nName])==0);

  pReal = pFsVfs->pFileList; 
  for(; pReal && strncmp(pReal->zName, zPath, nName); pReal=pReal->pNext);
  if( pReal ){
    pF = pReal->pFile;
    rc = pF->pMethods->xWrite(pF, "\0\0\0\0", 4, pReal->nBlob-BLOCKSIZE);
    if( rc==CAPDB_OK ){
      pReal->nJournal = 0;
    }
  }
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int fsAccess(
  capdb_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  fs_vfs_t *pFsVfs = (fs_vfs_t *)pVfs;
  fs_real_file *pReal;
  int isJournal = 0;
  int nName = (int)strlen(zPath);

  if( flags!=CAPDB_ACCESS_EXISTS ){
    capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
    return pParent->xAccess(pParent, zPath, flags, pResOut);
  }

  assert(strlen("-journal")==8);
  if( nName>8 && strcmp("-journal", &zPath[nName-8])==0 ){
    nName -= 8;
    isJournal = 1;
  }

  pReal = pFsVfs->pFileList; 
  for(; pReal && strncmp(pReal->zName, zPath, nName); pReal=pReal->pNext);

  *pResOut = (pReal && (!isJournal || pReal->nJournal>0));
  return CAPDB_OK;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (FS_MAX_PATHNAME+1) bytes.
*/
static int fsFullPathname(
  capdb_vfs *pVfs,            /* Pointer to vfs object */
  const char *zPath,            /* Possibly relative input path */
  int nOut,                     /* Size of output buffer in bytes */
  char *zOut                    /* Output buffer */
){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  return pParent->xFullPathname(pParent, zPath, nOut, zOut);
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *fsDlOpen(capdb_vfs *pVfs, const char *zPath){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  return pParent->xDlOpen(pParent, zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void fsDlError(capdb_vfs *pVfs, int nByte, char *zErrMsg){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  pParent->xDlError(pParent, nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*fsDlSym(capdb_vfs *pVfs, void *pH, const char *zSym))(void){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  return pParent->xDlSym(pParent, pH, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void fsDlClose(capdb_vfs *pVfs, void *pHandle){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  pParent->xDlClose(pParent, pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int fsRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  return pParent->xRandomness(pParent, nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int fsSleep(capdb_vfs *pVfs, int nMicro){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  return pParent->xSleep(pParent, nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int fsCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  capdb_vfs *pParent = ((fs_vfs_t *)pVfs)->pParent;
  return pParent->xCurrentTime(pParent, pTimeOut);
}

/*
** This procedure registers the fs vfs with SQLite. If the argument is
** true, the fs vfs becomes the new default vfs. It is the only publicly
** available function in this file.
*/
int fs_register(void){
  if( fs_vfs.pParent ) return CAPDB_OK;
  fs_vfs.pParent = capdb_vfs_find(0);
  fs_vfs.base.mxPathname = fs_vfs.pParent->mxPathname;
  fs_vfs.base.szOsFile = MAX(sizeof(tmp_file), sizeof(fs_file));
  return capdb_vfs_register(&fs_vfs.base, 0);
}

#ifdef CAPDB_TEST
  int SqlitetestOnefile_Init() {return fs_register();}
#endif
