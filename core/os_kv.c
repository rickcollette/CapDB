/*
** 2022-09-06
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
** This file contains an experimental VFS layer that operates on a
** Key/Value storage engine where both keys and values must be pure
** text.
*/
#include <capdbInt.h>
#if CAPDB_OS_KV || (CAPDB_OS_UNIX && defined(CAPDB_OS_KV_OPTIONAL))

/*****************************************************************************
** Debugging logic
*/

/* CAPDB_KV_TRACE() is used for tracing calls to kvrecord routines. */
#if 0
#define CAPDB_KV_TRACE(X)  printf X
#else
#define CAPDB_KV_TRACE(X)
#endif

/* CAPDB_KV_LOG() is used for tracing calls to the VFS interface */
#if 0
#define CAPDB_KV_LOG(X)  printf X
#else
#define CAPDB_KV_LOG(X)
#endif

/*
** Forward declaration of objects used by this VFS implementation
*/
typedef struct KVVfsFile KVVfsFile;

/* A single open file.  There are only two files represented by this
** VFS - the database and the rollback journal.
**
** Maintenance reminder: if this struct changes in any way, the JSON
** rendering of its structure must be updated in
** capdb-wasm.c:capdb__wasm_enum_json(). There are no binary
** compatibility concerns, so it does not need an iVersion member.
*/
struct KVVfsFile {
  capdb_file base;              /* IO methods */
  const char *zClass;             /* Storage class */
  int isJournal;                  /* True if this is a journal file */
  unsigned int nJrnl;             /* Space allocated for aJrnl[] */
  char *aJrnl;                    /* Journal content */
  int szPage;                     /* Last known page size */
  capdb_int64 szDb;             /* Database file size.  -1 means unknown */
  char *aData;                    /* Buffer to hold page data */
};
#define CAPDB_KVOS_SZ 133073

/*
** Methods for KVVfsFile
*/
static int kvvfsClose(capdb_file*);
static int kvvfsReadDb(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int kvvfsReadJrnl(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int kvvfsWriteDb(capdb_file*,const void*,int iAmt, capdb_int64);
static int kvvfsWriteJrnl(capdb_file*,const void*,int iAmt, capdb_int64);
static int kvvfsTruncateDb(capdb_file*, capdb_int64 size);
static int kvvfsTruncateJrnl(capdb_file*, capdb_int64 size);
static int kvvfsSyncDb(capdb_file*, int flags);
static int kvvfsSyncJrnl(capdb_file*, int flags);
static int kvvfsFileSizeDb(capdb_file*, capdb_int64 *pSize);
static int kvvfsFileSizeJrnl(capdb_file*, capdb_int64 *pSize);
static int kvvfsLock(capdb_file*, int);
static int kvvfsUnlock(capdb_file*, int);
static int kvvfsCheckReservedLock(capdb_file*, int *pResOut);
static int kvvfsFileControlDb(capdb_file*, int op, void *pArg);
static int kvvfsFileControlJrnl(capdb_file*, int op, void *pArg);
static int kvvfsSectorSize(capdb_file*);
static int kvvfsDeviceCharacteristics(capdb_file*);

/*
** Methods for capdb_vfs
*/
static int kvvfsOpen(capdb_vfs*, const char *, capdb_file*, int , int *);
static int kvvfsDelete(capdb_vfs*, const char *zName, int syncDir);
static int kvvfsAccess(capdb_vfs*, const char *zName, int flags, int *);
static int kvvfsFullPathname(capdb_vfs*, const char *zName, int, char *zOut);
static void *kvvfsDlOpen(capdb_vfs*, const char *zFilename);
static int kvvfsRandomness(capdb_vfs*, int nByte, char *zOut);
static int kvvfsSleep(capdb_vfs*, int microseconds);
static int kvvfsCurrentTime(capdb_vfs*, double*);
static int kvvfsCurrentTimeInt64(capdb_vfs*, capdb_int64*);

static capdb_vfs capdbOsKvvfsObject = {
  2,                              /* iVersion */
  sizeof(KVVfsFile),              /* szOsFile */
  1024,                           /* mxPathname */
  0,                              /* pNext */
  "kvvfs",                        /* zName */
  0,                              /* pAppData */
  kvvfsOpen,                      /* xOpen */
  kvvfsDelete,                    /* xDelete */
  kvvfsAccess,                    /* xAccess */
  kvvfsFullPathname,              /* xFullPathname */
  kvvfsDlOpen,                    /* xDlOpen */
  0,                              /* xDlError */
  0,                              /* xDlSym */
  0,                              /* xDlClose */
  kvvfsRandomness,                /* xRandomness */
  kvvfsSleep,                     /* xSleep */
  kvvfsCurrentTime,               /* xCurrentTime */
  0,                              /* xGetLastError */
  kvvfsCurrentTimeInt64           /* xCurrentTimeInt64 */
};

/* Methods for capdb_file objects referencing a database file
*/
static capdb_io_methods kvvfs_db_io_methods = {
  1,                              /* iVersion */
  kvvfsClose,                     /* xClose */
  kvvfsReadDb,                    /* xRead */
  kvvfsWriteDb,                   /* xWrite */
  kvvfsTruncateDb,                /* xTruncate */
  kvvfsSyncDb,                    /* xSync */
  kvvfsFileSizeDb,                /* xFileSize */
  kvvfsLock,                      /* xLock */
  kvvfsUnlock,                    /* xUnlock */
  kvvfsCheckReservedLock,         /* xCheckReservedLock */
  kvvfsFileControlDb,             /* xFileControl */
  kvvfsSectorSize,                /* xSectorSize */
  kvvfsDeviceCharacteristics,     /* xDeviceCharacteristics */
  0,                              /* xShmMap */
  0,                              /* xShmLock */
  0,                              /* xShmBarrier */
  0,                              /* xShmUnmap */
  0,                              /* xFetch */
  0                               /* xUnfetch */
};

/* Methods for capdb_file objects referencing a rollback journal
*/
static capdb_io_methods kvvfs_jrnl_io_methods = {
  1,                              /* iVersion */
  kvvfsClose,                     /* xClose */
  kvvfsReadJrnl,                  /* xRead */
  kvvfsWriteJrnl,                 /* xWrite */
  kvvfsTruncateJrnl,              /* xTruncate */
  kvvfsSyncJrnl,                  /* xSync */
  kvvfsFileSizeJrnl,              /* xFileSize */
  kvvfsLock,                      /* xLock */
  kvvfsUnlock,                    /* xUnlock */
  kvvfsCheckReservedLock,         /* xCheckReservedLock */
  kvvfsFileControlJrnl,           /* xFileControl */
  kvvfsSectorSize,                /* xSectorSize */
  kvvfsDeviceCharacteristics,     /* xDeviceCharacteristics */
  0,                              /* xShmMap */
  0,                              /* xShmLock */
  0,                              /* xShmBarrier */
  0,                              /* xShmUnmap */
  0,                              /* xFetch */
  0                               /* xUnfetch */
};

/****** Storage subsystem **************************************************/
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/* Forward declarations for the low-level storage engine
*/
#ifndef CAPDB_WASM
/* In WASM builds these are implemented in JS. */
static int kvrecordWrite(const char*, const char *zKey, const char *zData);
static int kvrecordDelete(const char*, const char *zKey);
static int kvrecordRead(const char*, const char *zKey, char *zBuf, int nBuf);
#endif
#ifndef KVRECORD_KEY_SZ
#define KVRECORD_KEY_SZ 32
#endif

/* Expand the key name with an appropriate prefix and put the result
** in zKeyOut[].  The zKeyOut[] buffer is assumed to hold at least
** KVRECORD_KEY_SZ bytes.
*/
static void kvrecordMakeKey(
  const char *zClass,
  const char *zKeyIn,
  char *zKeyOut
){
  assert( zKeyIn );
  assert( zKeyOut );
  assert( zClass );
  capdb_snprintf(KVRECORD_KEY_SZ, zKeyOut, "kvvfs-%s-%s",
                   zClass, zKeyIn);
}

#ifndef CAPDB_WASM
/* In WASM builds do not define APIs which use fopen(), fwrite(),
** and the like because those APIs are a portability issue for
** WASM.
*/
/* Write content into a key.  zClass is the particular namespace of the
** underlying key/value store to use - either "local" or "session".
**
** Both zKey and zData are zero-terminated pure text strings.
**
** Return the number of errors.
*/
static int kvrecordWrite(
  const char *zClass,
  const char *zKey,
  const char *zData
){
  FILE *fd;
  char zXKey[KVRECORD_KEY_SZ];
  kvrecordMakeKey(zClass, zKey, zXKey);
  fd = fopen(zXKey, "wb");
  if( fd ){
    CAPDB_KV_TRACE(("KVVFS-WRITE  %-15s (%d) %.50s%s\n", zXKey,
                 (int)strlen(zData), zData,
                 strlen(zData)>50 ? "..." : ""));
    fputs(zData, fd);
    fclose(fd);
    return 0;
  }else{
    return 1;
  }
}

/* Delete a key (with its corresponding data) from the key/value
** namespace given by zClass.  If the key does not previously exist,
** this routine is a no-op.
*/
static int kvrecordDelete(const char *zClass, const char *zKey){
  char zXKey[KVRECORD_KEY_SZ];
  kvrecordMakeKey(zClass, zKey, zXKey);
  unlink(zXKey);
  CAPDB_KV_TRACE(("KVVFS-DELETE %-15s\n", zXKey));
  return 0;
}

/* Read the value associated with a zKey from the key/value namespace given
** by zClass and put the text data associated with that key in the first
** nBuf bytes of zBuf[].  The value might be truncated if zBuf is not large
** enough to hold it all.  The value put into zBuf must always be zero
** terminated, even if it gets truncated because nBuf is not large enough.
**
** Return the total number of bytes in the data, without truncation, and
** not counting the final zero terminator.   Return -1 if the key does
** not exist or its key cannot be read.
**
** If nBuf<=0 then this routine simply returns the size of the data
** without actually reading it.  Similarly, if nBuf==1 then it
** zero-terminates zBuf at zBuf[0] and returns the size of the data
** without reading it.
*/
static int kvrecordRead(
  const char *zClass,
  const char *zKey,
  char *zBuf,
  int nBuf
){
  FILE *fd;
  struct stat buf;
  char zXKey[KVRECORD_KEY_SZ];
  kvrecordMakeKey(zClass, zKey, zXKey);
  if( access(zXKey, R_OK)!=0
   || stat(zXKey, &buf)!=0
   || !S_ISREG(buf.st_mode)
  ){
    CAPDB_KV_TRACE(("KVVFS-READ   %-15s (-1)\n", zXKey));
    return -1;
  }
  if( nBuf<=0 ){
    return (int)buf.st_size;
  }else if( nBuf==1 ){
    zBuf[0] = 0;
    CAPDB_KV_TRACE(("KVVFS-READ   %-15s (%d)\n", zXKey,
                 (int)buf.st_size));
    return (int)buf.st_size;
  }
  if( nBuf > buf.st_size + 1 ){
    nBuf = buf.st_size + 1;
  }
  fd = fopen(zXKey, "rb");
  if( fd==0 ){
    CAPDB_KV_TRACE(("KVVFS-READ   %-15s (-1)\n", zXKey));
    return -1;
  }else{
    capdb_int64 n = fread(zBuf, 1, nBuf-1, fd);
    fclose(fd);
    zBuf[n] = 0;
    CAPDB_KV_TRACE(("KVVFS-READ   %-15s (%lld) %.50s%s\n", zXKey,
                 n, zBuf, n>50 ? "..." : ""));
    return (int)n;
  }
}
#endif /* #ifndef CAPDB_WASM */


/*
** An internal level of indirection which enables us to replace the
** kvvfs i/o methods with JavaScript implementations in WASM builds.
** Maintenance reminder: if this struct changes in any way, the JSON
** rendering of its structure must be updated in
** capdb-wasm.c:capdb__wasm_enum_json(). There are no binary
** compatibility concerns, so it does not need an iVersion member.
*/
typedef struct capdb_kvvfs_methods capdb_kvvfs_methods;
struct capdb_kvvfs_methods {
  int (*xRcrdRead)(const char*, const char *zKey, char *zBuf, int nBuf);
  int (*xRcrdWrite)(const char*, const char *zKey, const char *zData);
  int (*xRcrdDelete)(const char*, const char *zKey);
  const int nKeySize;
  const int nBufferSize;
#ifndef CAPDB_WASM
#  define MAYBE_CONST const
#else
#  define MAYBE_CONST
#endif
  MAYBE_CONST capdb_vfs * pVfs;
  MAYBE_CONST capdb_io_methods *pIoDb;
  MAYBE_CONST capdb_io_methods *pIoJrnl;
#undef MAYBE_CONST
};


/*
** This object holds the kvvfs I/O methods which may be swapped out
** for JavaScript-side implementations in WASM builds. In such builds
** it cannot be const, but in native builds it should be so that
** the compiler can hopefully optimize this level of indirection out.
** That said, kvvfs is intended primarily for use in WASM builds.
**
** This is not explicitly flagged as static because the amalgamation
** build will tag it with CAPDB_PRIVATE.
*/
#ifndef CAPDB_WASM
const
#endif
capdb_kvvfs_methods capdbKvvfsMethods = {
#ifndef CAPDB_WASM
  .xRcrdRead       = kvrecordRead,
  .xRcrdWrite      = kvrecordWrite,
  .xRcrdDelete     = kvrecordDelete,
#else
  .xRcrdRead       = 0,
  .xRcrdWrite      = 0,
  .xRcrdDelete     = 0,
#endif
  .nKeySize        = KVRECORD_KEY_SZ,
  .nBufferSize     = CAPDB_KVOS_SZ,
  .pVfs            = &capdbOsKvvfsObject,
  .pIoDb           = &kvvfs_db_io_methods,
  .pIoJrnl         = &kvvfs_jrnl_io_methods
};

/****** Utility subroutines ************************************************/

/*
** Encode binary into the text encoded used to persist on disk.
** The output text is stored in aOut[], which must be at least
** nData+1 bytes in length.
**
** Return the actual length of the encoded text, not counting the
** zero terminator at the end.
**
** Encoding format
** ---------------
**
**   *  Non-zero bytes are encoded as upper-case hexadecimal
**
**   *  A sequence of one or more zero-bytes that are not at the
**      beginning of the buffer are encoded as a little-endian
**      base-26 number using a..z.  "a" means 0.  "b" means 1,
**      "z" means 25.  "ab" means 26.  "ac" means 52.  And so forth.
**
**   *  Because there is no overlap between the encoding characters
**      of hexadecimal and base-26 numbers, it is always clear where
**      one stops and the next begins.
*/
#ifndef CAPDB_WASM
static
#endif
int kvvfsEncode(const char *aData, int nData, char *aOut){
  int i, j;
  const unsigned char *a = (const unsigned char*)aData;
  for(i=j=0; i<nData; i++){
    unsigned char c = a[i];
    if( c!=0 ){
      aOut[j++] = "0123456789ABCDEF"[c>>4];
      aOut[j++] = "0123456789ABCDEF"[c&0xf];
    }else{
      /* A sequence of 1 or more zeros is stored as a little-endian
      ** base-26 number using a..z as the digits. So one zero is "b".
      ** Two zeros is "c". 25 zeros is "z", 26 zeros is "ab", 27 is "bb",
      ** and so forth.
      */
      int k;
      for(k=1; i+k<nData && a[i+k]==0; k++){}
      i += k-1;
      while( k>0 ){
        aOut[j++] = 'a'+(k%26);
        k /= 26;
      }
    }
  }
  aOut[j] = 0;
  return j;
}

static const signed char kvvfsHexValue[256] = {
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
   0,  1,  2,  3,  4,  5,  6,  7,    8,  9, -1, -1, -1, -1, -1, -1,
  -1, 10, 11, 12, 13, 14, 15, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,

  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1,   -1, -1, -1, -1, -1, -1, -1, -1
};

/*
** Decode the text encoding back to binary.  The binary content is
** written into pOut, which must be at least nOut bytes in length.
**
** The return value is the number of bytes actually written into aOut[], or
** -1 for malformed inputs.
*/
#ifndef CAPDB_WASM
static
#endif
int kvvfsDecode(const char *a, char *aOut, int nOut){
  int i, j;
  int c;
  const unsigned char *aIn = (const unsigned char*)a;
  i = 0;
  j = 0;
  while( 1 ){
    c = kvvfsHexValue[aIn[i]];
    if( c<0 ){
      int n = 0;
      int mult = 1;
      c = aIn[i];
      if( c==0 ) break;
      while( c>='a' && c<='z' ){
        n += (c - 'a')*mult;
        mult *= 26;
        c = aIn[++i];
      }
      if( j+n>nOut ) return -1;
      memset(&aOut[j], 0, n);
      j += n;
      if( c==0 || mult==1 ) break; /* progress stalled if mult==1 */
    }else{
      aOut[j] = c<<4;
      c = kvvfsHexValue[aIn[++i]];
      if( c<0 ) return -1 /* hex bytes are always in pairs */;
      aOut[j++] += c;
      i++;
    }
  }
  return j;
}

/*
** Decode a complete journal file.  Allocate space in pFile->aJrnl
** and store the decoding there.  Or leave pFile->aJrnl set to NULL
** if an error is encountered.
**
** The first few characters of the text encoding will be a little-endian
** base-26 number (digits a..z) that is the total number of bytes
** in the decoded journal file image.  This base-26 number is followed
** by a single space, then the encoding of the journal.  The space
** separator is required to act as a terminator for the base-26 number.
*/
static void kvvfsDecodeJournal(
  KVVfsFile *pFile,      /* Store decoding in pFile->aJrnl */
  const char *zTxt,      /* Text encoding.  Zero-terminated */
  int nTxt               /* Bytes in zTxt, excluding zero terminator */
){
  unsigned int n = 0;
  int c, i, mult;
  i = 0;
  mult = 1;
  while( (c = zTxt[i++])>='a' && c<='z' ){
    n += (zTxt[i] - 'a')*mult;
    mult *= 26;
  }
  capdb_free(pFile->aJrnl);
  pFile->aJrnl = capdb_malloc64( n );
  if( pFile->aJrnl==0 ){
    pFile->nJrnl = 0;
    return;
  }
  pFile->nJrnl = n;
  n = kvvfsDecode(zTxt+i, pFile->aJrnl, pFile->nJrnl);
  if( n<pFile->nJrnl ){
    capdb_free(pFile->aJrnl);
    pFile->aJrnl = 0;
    pFile->nJrnl = 0;
  }
}

/*
** Read or write the "sz" element, containing the database file size.
*/
static capdb_int64 kvvfsReadFileSize(KVVfsFile *pFile){
  char zData[50];
  zData[0] = 0;
  capdbKvvfsMethods.xRcrdRead(pFile->zClass, "sz", zData,
                                sizeof(zData)-1);
  return strtoll(zData, 0, 0);
}
static int kvvfsWriteFileSize(KVVfsFile *pFile, capdb_int64 sz){
  char zData[50];
  capdb_snprintf(sizeof(zData), zData, "%lld", sz);
  return capdbKvvfsMethods.xRcrdWrite(pFile->zClass, "sz", zData);
}

/****** capdb_io_methods methods ******************************************/

/*
** Close an kvvfs-file.
*/
static int kvvfsClose(capdb_file *pProtoFile){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;

  CAPDB_KV_LOG(("xClose %s %s\n", pFile->zClass,
             pFile->isJournal ? "journal" : "db"));
  capdb_free(pFile->aJrnl);
  capdb_free(pFile->aData);
#ifdef CAPDB_WASM
  memset(pFile, 0, sizeof(*pFile));
#endif
  return CAPDB_OK;
}

/*
** Read from the -journal file.
*/
static int kvvfsReadJrnl(
  capdb_file *pProtoFile,
  void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  KVVfsFile *pFile = (KVVfsFile*)pProtoFile;
  assert( pFile->isJournal );
  CAPDB_KV_LOG(("xRead('%s-journal',%d,%lld)\n", pFile->zClass, iAmt, iOfst));
  if( pFile->aJrnl==0 ){
    int rc;
    int szTxt = capdbKvvfsMethods.xRcrdRead(pFile->zClass, "jrnl",
                                              0, 0);
    char *aTxt;
    if( szTxt<=4 ){
      return CAPDB_IOERR;
    }
    aTxt = capdb_malloc64( szTxt+1 );
    if( aTxt==0 ) return CAPDB_NOMEM;
    rc = capdbKvvfsMethods.xRcrdRead(pFile->zClass, "jrnl",
                                       aTxt, szTxt+1);
    if( rc>=0 ){
      kvvfsDecodeJournal(pFile, aTxt, szTxt);
    }
    capdb_free(aTxt);
    if( rc ) return rc;
    if( pFile->aJrnl==0 ) return CAPDB_IOERR;
  }
  if( iOfst+iAmt>pFile->nJrnl ){
    return CAPDB_IOERR_SHORT_READ;
  }
  memcpy(zBuf, pFile->aJrnl+iOfst, iAmt);
  return CAPDB_OK;
}

/*
** Read from the database file.
*/
static int kvvfsReadDb(
  capdb_file *pProtoFile,
  void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  KVVfsFile *pFile = (KVVfsFile*)pProtoFile;
  unsigned int pgno;
  int got, n;
  char zKey[30];
  char *aData = pFile->aData;
  assert( iOfst>=0 );
  assert( iAmt>=0 );
  CAPDB_KV_LOG(("xRead('%s-db',%d,%lld)\n", pFile->zClass, iAmt, iOfst));
  if( iOfst+iAmt>=512 ){
    if( (iOfst % iAmt)!=0 ){
      return CAPDB_IOERR_READ;
    }
    if( (iAmt & (iAmt-1))!=0 || iAmt<512 || iAmt>65536 ){
      return CAPDB_IOERR_READ;
    }
    pFile->szPage = iAmt;
    pgno = 1 + iOfst/iAmt;
  }else{
    pgno = 1;
  }
  capdb_snprintf(sizeof(zKey), zKey, "%u", pgno);
  got = capdbKvvfsMethods.xRcrdRead(pFile->zClass, zKey,
                                      aData, CAPDB_KVOS_SZ-1);
  if( got<0 ){
    n = 0;
  }else{
    aData[got] = 0;
    if( iOfst+iAmt<512 ){
      int k = iOfst+iAmt;
      aData[k*2] = 0;
      n = kvvfsDecode(aData, &aData[2000], CAPDB_KVOS_SZ-2000);
      if( n>=iOfst+iAmt ){
        memcpy(zBuf, &aData[2000+iOfst], iAmt);
        n = iAmt;
      }else{
        n = 0;
      }
    }else{
      n = kvvfsDecode(aData, zBuf, iAmt);
    }
  }
  if( n<iAmt ){
    memset(zBuf+n, 0, iAmt-n);
    return CAPDB_IOERR_SHORT_READ;
  }
  return CAPDB_OK;
}


/*
** Write into the -journal file.
*/
static int kvvfsWriteJrnl(
  capdb_file *pProtoFile,
  const void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  KVVfsFile *pFile = (KVVfsFile*)pProtoFile;
  capdb_int64 iEnd = iOfst+iAmt;
  CAPDB_KV_LOG(("xWrite('%s-journal',%d,%lld)\n", pFile->zClass, iAmt, iOfst));
  if( iEnd>=0x10000000 ) return CAPDB_FULL;
  if( pFile->aJrnl==0 || pFile->nJrnl<iEnd ){
    char *aNew = capdb_realloc(pFile->aJrnl, iEnd);
    if( aNew==0 ){
      return CAPDB_IOERR_NOMEM;
    }
    pFile->aJrnl = aNew;
    if( pFile->nJrnl<iOfst ){
      memset(pFile->aJrnl+pFile->nJrnl, 0, iOfst-pFile->nJrnl);
    }
    pFile->nJrnl = iEnd;
  }
  memcpy(pFile->aJrnl+iOfst, zBuf, iAmt);
  return CAPDB_OK;
}

/*
** Write into the database file.
*/
static int kvvfsWriteDb(
  capdb_file *pProtoFile,
  const void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  KVVfsFile *pFile = (KVVfsFile*)pProtoFile;
  unsigned int pgno;
  char zKey[30];
  char *aData = pFile->aData;
  int rc;
  CAPDB_KV_LOG(("xWrite('%s-db',%d,%lld)\n", pFile->zClass, iAmt, iOfst));
  assert( iAmt>=512 && iAmt<=65536 );
  assert( (iAmt & (iAmt-1))==0 );
  assert( pFile->szPage<0 || pFile->szPage==iAmt );
  pFile->szPage = iAmt;
  pgno = 1 + iOfst/iAmt;
  capdb_snprintf(sizeof(zKey), zKey, "%u", pgno);
  kvvfsEncode(zBuf, iAmt, aData);
  rc = capdbKvvfsMethods.xRcrdWrite(pFile->zClass, zKey, aData);
  if( 0==rc ){
    if( iOfst+iAmt > pFile->szDb ){
      pFile->szDb = iOfst + iAmt;
    }
  }
  return rc;
}

/*
** Truncate an kvvfs-file.
*/
static int kvvfsTruncateJrnl(capdb_file *pProtoFile, sqlite_int64 size){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  CAPDB_KV_LOG(("xTruncate('%s-journal',%lld)\n", pFile->zClass, size));
  assert( size==0 );
  capdbKvvfsMethods.xRcrdDelete(pFile->zClass, "jrnl");
  capdb_free(pFile->aJrnl);
  pFile->aJrnl = 0;
  pFile->nJrnl = 0;
  return CAPDB_OK;
}
static int kvvfsTruncateDb(capdb_file *pProtoFile, sqlite_int64 size){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  if( pFile->szDb>size
   && pFile->szPage>0
   && (size % pFile->szPage)==0
  ){
    char zKey[50];
    unsigned int pgno, pgnoMax;
    CAPDB_KV_LOG(("xTruncate('%s-db',%lld)\n", pFile->zClass, size));
    pgno = 1 + size/pFile->szPage;
    pgnoMax = 2 + pFile->szDb/pFile->szPage;
    while( pgno<=pgnoMax ){
      capdb_snprintf(sizeof(zKey), zKey, "%u", pgno);
      capdbKvvfsMethods.xRcrdDelete(pFile->zClass, zKey);
      pgno++;
    }
    pFile->szDb = size;
    return kvvfsWriteFileSize(pFile, size) ? CAPDB_IOERR : CAPDB_OK;
  }
  return CAPDB_IOERR;
}

/*
** Sync an kvvfs-file.
*/
static int kvvfsSyncJrnl(capdb_file *pProtoFile, int flags){
  int i, n;
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  char *zOut;
  CAPDB_KV_LOG(("xSync('%s-journal')\n", pFile->zClass));
  if( pFile->nJrnl<=0 ){
    return kvvfsTruncateJrnl(pProtoFile, 0);
  }
  zOut = capdb_malloc64( pFile->nJrnl*2 + 50 );
  if( zOut==0 ){
    return CAPDB_IOERR_NOMEM;
  }
  n = pFile->nJrnl;
  i = 0;
  do{
    zOut[i++] = 'a' + (n%26);
    n /= 26;
  }while( n>0 );
  zOut[i++] = ' ';
  kvvfsEncode(pFile->aJrnl, pFile->nJrnl, &zOut[i]);
  i = capdbKvvfsMethods.xRcrdWrite(pFile->zClass, "jrnl", zOut);
  capdb_free(zOut);
  return i ? CAPDB_IOERR : CAPDB_OK;
}
static int kvvfsSyncDb(capdb_file *pProtoFile, int flags){
  return CAPDB_OK;
}

/*
** Return the current file-size of an kvvfs-file.
*/
static int kvvfsFileSizeJrnl(capdb_file *pProtoFile, sqlite_int64 *pSize){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  CAPDB_KV_LOG(("xFileSize('%s-journal')\n", pFile->zClass));
  *pSize = pFile->nJrnl;
  return CAPDB_OK;
}
static int kvvfsFileSizeDb(capdb_file *pProtoFile, sqlite_int64 *pSize){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  CAPDB_KV_LOG(("xFileSize('%s-db')\n", pFile->zClass));
  if( pFile->szDb>=0 ){
    *pSize = pFile->szDb;
  }else{
    *pSize = kvvfsReadFileSize(pFile);
  }
  return CAPDB_OK;
}

/*
** Lock an kvvfs-file.
*/
static int kvvfsLock(capdb_file *pProtoFile, int eLock){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  assert( !pFile->isJournal );
  CAPDB_KV_LOG(("xLock(%s,%d)\n", pFile->zClass, eLock));

  if( eLock!=CAPDB_LOCK_NONE ){
    pFile->szDb = kvvfsReadFileSize(pFile);
  }
  return CAPDB_OK;
}

/*
** Unlock an kvvfs-file.
*/
static int kvvfsUnlock(capdb_file *pProtoFile, int eLock){
  KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
  assert( !pFile->isJournal );
  CAPDB_KV_LOG(("xUnlock(%s,%d)\n", pFile->zClass, eLock));
  if( eLock==CAPDB_LOCK_NONE ){
    pFile->szDb = -1;
  }
  return CAPDB_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on an kvvfs-file.
*/
static int kvvfsCheckReservedLock(capdb_file *pProtoFile, int *pResOut){
  CAPDB_KV_LOG(("xCheckReservedLock\n"));
  *pResOut = 0;
  return CAPDB_OK;
}

/*
** File control method. For custom operations on an kvvfs-file.
*/
static int kvvfsFileControlJrnl(capdb_file *pProtoFile, int op, void *pArg){
  CAPDB_KV_LOG(("xFileControl(%d) on journal\n", op));
  return CAPDB_NOTFOUND;
}
static int kvvfsFileControlDb(capdb_file *pProtoFile, int op, void *pArg){
  CAPDB_KV_LOG(("xFileControl(%d) on database\n", op));
  if( op==CAPDB_FCNTL_SYNC ){
    KVVfsFile *pFile = (KVVfsFile *)pProtoFile;
    int rc = CAPDB_OK;
    CAPDB_KV_LOG(("xSync('%s-db')\n", pFile->zClass));
    if( pFile->szDb>0 && 0!=kvvfsWriteFileSize(pFile, pFile->szDb) ){
      rc = CAPDB_IOERR;
    }
    return rc;
  }
  return CAPDB_NOTFOUND;
}

/*
** Return the sector-size in bytes for an kvvfs-file.
*/
static int kvvfsSectorSize(capdb_file *pFile){
  return 512;
}

/*
** Return the device characteristic flags supported by an kvvfs-file.
*/
static int kvvfsDeviceCharacteristics(capdb_file *pProtoFile){
  return 0;
}

/****** capdb_vfs methods *************************************************/

/*
** Open an kvvfs file handle.
*/
static int kvvfsOpen(
  capdb_vfs *pProtoVfs,
  const char *zName,
  capdb_file *pProtoFile,
  int flags,
  int *pOutFlags
){
  KVVfsFile *pFile = (KVVfsFile*)pProtoFile;
  if( zName==0 ) zName = "";
  CAPDB_KV_LOG(("xOpen(\"%s\")\n", zName));
  assert(!pFile->zClass);
  assert(!pFile->aData);
  assert(!pFile->aJrnl);
  assert(!pFile->nJrnl);
  assert(!pFile->base.pMethods);
  pFile->szPage = -1;
  pFile->szDb = -1;
  if( 0==capdb_strglob("*-journal", zName) ){
    pFile->isJournal = 1;
    pFile->base.pMethods = &kvvfs_jrnl_io_methods;
    if( 0==strcmp("session-journal",zName) ){
      pFile->zClass = "session";
    }else if( 0==strcmp("local-journal",zName) ){
      pFile->zClass = "local";
    }
  }else{
    pFile->isJournal = 0;
    pFile->base.pMethods = &kvvfs_db_io_methods;
  }
  if( !pFile->zClass ){
    pFile->zClass = zName;
  }
  pFile->aData = capdb_malloc64(CAPDB_KVOS_SZ);
  if( pFile->aData==0 ){
    return CAPDB_NOMEM;
  }
  return CAPDB_OK;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int kvvfsDelete(capdb_vfs *pVfs, const char *zPath, int dirSync){
  int rc /* The JS impl can fail with OOM in argument conversion */;
  if( strcmp(zPath, "local-journal")==0 ){
    rc = capdbKvvfsMethods.xRcrdDelete("local", "jrnl");
  }else
  if( strcmp(zPath, "session-journal")==0 ){
    rc = capdbKvvfsMethods.xRcrdDelete("session", "jrnl");
  }
  else{
    rc = 0;
  }
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int kvvfsAccess(
  capdb_vfs *pProtoVfs,
  const char *zPath,
  int flags,
  int *pResOut
){
  CAPDB_KV_LOG(("xAccess(\"%s\")\n", zPath));
#if 0 && defined(CAPDB_WASM)
  /*
  ** This is not having the desired effect in the JS bindings.
  ** It's ostensibly the same logic as the #else block, but
  ** it's not behaving that way.
  **
  ** In JS we map all zPaths to Storage objects, and -journal files
  ** are mapped to the storage for the main db (which is is exactly
  ** what the mapping of "local-journal" -> "local" is doing).
  */
  const char *zKey = (0==capdb_strglob("*-journal", zPath))
    ? "jrnl" : "sz";
  *pResOut =
    capdbKvvfsMethods.xRcrdRead(zPath, zKey, 0, 0)>0;
#else
  if( strcmp(zPath, "local-journal")==0 ){
    *pResOut =
      capdbKvvfsMethods.xRcrdRead("local", "jrnl", 0, 0)>0;
  }else
  if( strcmp(zPath, "session-journal")==0 ){
    *pResOut =
      capdbKvvfsMethods.xRcrdRead("session", "jrnl", 0, 0)>0;
  }else
  if( strcmp(zPath, "local")==0 ){
    *pResOut =
      capdbKvvfsMethods.xRcrdRead("local", "sz", 0, 0)>0;
  }else
  if( strcmp(zPath, "session")==0 ){
    *pResOut =
      capdbKvvfsMethods.xRcrdRead("session", "sz", 0, 0)>0;
  }else
  {
    *pResOut = 0;
  }
  /*all current JS tests avoid triggering: assert( *pResOut == 0 ); */
#endif
  CAPDB_KV_LOG(("xAccess returns %d\n",*pResOut));
  return CAPDB_OK;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (INST_MAX_PATHNAME+1) bytes.
*/
static int kvvfsFullPathname(
  capdb_vfs *pVfs,
  const char *zPath,
  int nOut,
  char *zOut
){
  size_t nPath;
#ifdef CAPDB_OS_KV_ALWAYS_LOCAL
  zPath = "local";
#endif
  nPath = strlen(zPath);
  CAPDB_KV_LOG(("xFullPathname(\"%s\")\n", zPath));
  if( nOut<nPath+1 ) nPath = nOut - 1;
  memcpy(zOut, zPath, nPath);
  zOut[nPath] = 0;
  return CAPDB_OK;
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *kvvfsDlOpen(capdb_vfs *pVfs, const char *zPath){
  return 0;
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of
** random data.
*/
static int kvvfsRandomness(capdb_vfs *pVfs, int nByte, char *zBufOut){
  memset(zBufOut, 0, nByte);
  return nByte;
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds
** actually slept.
*/
static int kvvfsSleep(capdb_vfs *pVfs, int nMicro){
  return CAPDB_OK;
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int kvvfsCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  capdb_int64 i = 0;
  int rc;
  rc = kvvfsCurrentTimeInt64(0, &i);
  *pTimeOut = i/86400000.0;
  return rc;
}
#include <sys/time.h>
static int kvvfsCurrentTimeInt64(capdb_vfs *pVfs, capdb_int64 *pTimeOut){
  static const capdb_int64 unixEpoch = 24405875*(capdb_int64)8640000;
  struct timeval sNow;
  (void)gettimeofday(&sNow, 0);  /* Cannot fail given valid arguments */
  *pTimeOut = unixEpoch + 1000*(capdb_int64)sNow.tv_sec + sNow.tv_usec/1000;
  return CAPDB_OK;
}
#endif /* CAPDB_OS_KV || CAPDB_OS_UNIX */

#if CAPDB_OS_KV
/*
** This routine is called initialize the KV-vfs as the default VFS.
*/
int capdb_os_init(void){
  return capdb_vfs_register(&capdbOsKvvfsObject, 1);
}
int capdb_os_end(void){
  return CAPDB_OK;
}
#endif /* CAPDB_OS_KV */

#if CAPDB_OS_UNIX && defined(CAPDB_OS_KV_OPTIONAL)
int capdbKvvfsInit(void){
  return capdb_vfs_register(&capdbOsKvvfsObject, 0);
}
#endif
