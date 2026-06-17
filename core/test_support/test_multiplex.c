/*
** 2010 October 28
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
** This file contains a VFS "shim" - a layer that sits in between the
** pager and the real VFS - that breaks up a very large database file
** into two or more smaller files on disk.  This is useful, for example,
** in order to support large, multi-gigabyte databases on older filesystems
** that limit the maximum file size to 2 GiB.
**
** USAGE:
**
** Compile this source file and link it with your application.  Then
** at start-time, invoke the following procedure:
**
**   int capdb_multiplex_initialize(
**      const char *zOrigVfsName,    // The underlying real VFS
**      int makeDefault              // True to make multiplex the default VFS
**   );
**
** The procedure call above will create and register a new VFS shim named
** "multiplex".  The multiplex VFS will use the VFS named by zOrigVfsName to
** do the actual disk I/O.  (The zOrigVfsName parameter may be NULL, in
** which case the default VFS at the moment capdb_multiplex_initialize()
** is called will be used as the underlying real VFS.)
**
** If the makeDefault parameter is TRUE then multiplex becomes the new
** default VFS.  Otherwise, you can use the multiplex VFS by specifying
** "multiplex" as the 4th parameter to capdb_open_v2() or by employing
** URI filenames and adding "vfs=multiplex" as a parameter to the filename
** URI.
**
** The multiplex VFS allows databases up to 32 GiB in size.  But it splits
** the files up into smaller pieces, so that they will work even on
** filesystems that do not support large files.  The default chunk size
** is 2147418112 bytes (which is 64KiB less than 2GiB) but this can be
** changed at compile-time by defining the CAPDB_MULTIPLEX_CHUNK_SIZE
** macro.  Use the "chunksize=NNNN" query parameter with a URI filename
** in order to select an alternative chunk size for individual connections
** at run-time.
*/
#include "capdb.h"
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "test_multiplex.h"

#ifndef CAPDB_CORE
  #define CAPDB_CORE 1  /* Disable the API redefinition in capdbext.h */
#endif
#include "capdbext.h"

/*
** These should be defined to be the same as the values in
** capdbInt.h.  They are defined separately here so that
** the multiplex VFS shim can be built as a loadable
** module.
*/
#define UNUSED_PARAMETER(x) (void)(x)
#define MAX_PAGE_SIZE       0x10000
#define DEFAULT_SECTOR_SIZE 0x1000

/* Maximum chunk number */
#define MX_CHUNK_NUMBER 299

/* First chunk for rollback journal files */
#define CAPDB_MULTIPLEX_JOURNAL_8_3_OFFSET 400
#define CAPDB_MULTIPLEX_WAL_8_3_OFFSET 700


/************************ Shim Definitions ******************************/

#ifndef CAPDB_MULTIPLEX_VFS_NAME
# define CAPDB_MULTIPLEX_VFS_NAME "multiplex"
#endif

/* This is the limit on the chunk size.  It may be changed by calling
** the xFileControl() interface.  It will be rounded up to a
** multiple of MAX_PAGE_SIZE.  We default it here to 2GiB less 64KiB.
*/
#ifndef CAPDB_MULTIPLEX_CHUNK_SIZE
# define CAPDB_MULTIPLEX_CHUNK_SIZE 2147418112
#endif

/* This used to be the default limit on number of chunks, but
** it is no longer enforced. There is currently no limit to the
** number of chunks.
**
** May be changed by calling the xFileControl() interface.
*/
#ifndef CAPDB_MULTIPLEX_MAX_CHUNKS
# define CAPDB_MULTIPLEX_MAX_CHUNKS 12
#endif

/************************ Object Definitions ******************************/

/* Forward declaration of all object types */
typedef struct multiplexGroup multiplexGroup;
typedef struct multiplexConn multiplexConn;

/*
** A "multiplex group" is a collection of files that collectively
** makeup a single SQLite DB file.  This allows the size of the DB
** to exceed the limits imposed by the file system.
**
** There is an instance of the following object for each defined multiplex
** group.
*/
struct multiplexGroup {
  struct multiplexReal {           /* For each chunk */
    capdb_file *p;                  /* Handle for the chunk */
    char *z;                          /* Name of this chunk */
  } *aReal;                        /* list of all chunks */
  int nReal;                       /* Number of chunks */
  char *zName;                     /* Base filename of this group */
  int nName;                       /* Length of base filename */
  int flags;                       /* Flags used for original opening */
  unsigned int szChunk;            /* Chunk size used for this group */
  unsigned char bEnabled;          /* TRUE to use Multiplex VFS for this file */
  unsigned char bTruncate;         /* TRUE to enable truncation of databases */
};

/*
** An instance of the following object represents each open connection
** to a file that is multiplex'ed.  This object is a
** subclass of capdb_file.  The capdb_file object for the underlying
** VFS is appended to this structure.
*/
struct multiplexConn {
  capdb_file base;              /* Base class - must be first */
  multiplexGroup *pGroup;         /* The underlying group of files */
};

/************************* Global Variables **********************************/
/*
** All global variables used by this file are containing within the following
** gMultiplex structure.
*/
static struct {
  /* The pOrigVfs is the real, original underlying VFS implementation.
  ** Most operations pass-through to the real VFS.  This value is read-only
  ** during operation.  It is only modified at start-time and thus does not
  ** require a mutex.
  */
  capdb_vfs *pOrigVfs;

  /* The sThisVfs is the VFS structure used by this shim.  It is initialized
  ** at start-time and thus does not require a mutex
  */
  capdb_vfs sThisVfs;

  /* The sIoMethods defines the methods used by capdb_file objects
  ** associated with this shim.  It is initialized at start-time and does
  ** not require a mutex.
  **
  ** When the underlying VFS is called to open a file, it might return
  ** either a version 1 or a version 2 capdb_file object.  This shim
  ** has to create a wrapper capdb_file of the same version.  Hence
  ** there are two I/O method structures, one for version 1 and the other
  ** for version 2.
  */
  capdb_io_methods sIoMethodsV1;
  capdb_io_methods sIoMethodsV2;

  /* True when this shim has been initialized.
  */
  int isInitialized;
} gMultiplex;

/************************* Utility Routines *********************************/
/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
**
** The value returned will never be negative.  Nor will it ever be greater
** than the actual length of the string.  For very long strings (greater
** than 1GiB) the value returned might be less than the true string length.
*/
static int multiplexStrlen30(const char *z){
  const char *z2 = z;
  if( z==0 ) return 0;
  while( *z2 ){ z2++; }
  return 0x3fffffff & (int)(z2 - z);
}

/*
** Generate the file-name for chunk iChunk of the group with base name
** zBase. The file-name is written to buffer zOut before returning. Buffer
** zOut must be allocated by the caller so that it is at least (nBase+5)
** bytes in size, where nBase is the length of zBase, not including the
** nul-terminator.
**
** If iChunk is 0 (or 400 - the number for the first journal file chunk),
** the output is a copy of the input string. Otherwise, if
** CAPDB_ENABLE_8_3_NAMES is not defined or the input buffer does not contain
** a "." character, then the output is a copy of the input string with the
** three-digit zero-padded decimal representation if iChunk appended to it.
** For example:
**
**   zBase="test.db", iChunk=4  ->  zOut="test.db004"
**
** Or, if CAPDB_ENABLE_8_3_NAMES is defined and the input buffer contains
** a "." character, then everything after the "." is replaced by the
** three-digit representation of iChunk.
**
**   zBase="test.db", iChunk=4  ->  zOut="test.004"
**
** The output buffer string is terminated by 2 0x00 bytes. This makes it safe
** to pass to capdb_uri_parameter() and similar.
*/
static void multiplexFilename(
  const char *zBase,              /* Filename for chunk 0 */
  int nBase,                      /* Size of zBase in bytes (without \0) */
  int flags,                      /* Flags used to open file */
  int iChunk,                     /* Chunk to generate filename for */
  char *zOut                      /* Buffer to write generated name to */
){
  int n = nBase;
  memcpy(zOut, zBase, n+1);
  if( iChunk!=0 && iChunk<=MX_CHUNK_NUMBER ){
#ifdef CAPDB_ENABLE_8_3_NAMES
    int i;
    for(i=n-1; i>0 && i>=n-4 && zOut[i]!='.'; i--){}
    if( i>=n-4 ) n = i+1;
    if( flags & CAPDB_OPEN_MAIN_JOURNAL ){
      /* The extensions on overflow files for main databases are 001, 002,
      ** 003 and so forth.  To avoid name collisions, add 400 to the
      ** extensions of journal files so that they are 401, 402, 403, ....
      */
      iChunk += CAPDB_MULTIPLEX_JOURNAL_8_3_OFFSET;
    }else if( flags & CAPDB_OPEN_WAL ){
      /* To avoid name collisions, add 700 to the
      ** extensions of WAL files so that they are 701, 702, 703, ....
      */
      iChunk += CAPDB_MULTIPLEX_WAL_8_3_OFFSET;
    }
#endif
    capdb_snprintf(4,&zOut[n],"%03d",iChunk);
    n += 3;
  }

  assert( zOut[n]=='\0' );
  zOut[n+1] = '\0';
}

/* Compute the filename for the iChunk-th chunk
*/
static int multiplexSubFilename(multiplexGroup *pGroup, int iChunk){
  if( iChunk>=pGroup->nReal ){
    struct multiplexReal *p;
    p = capdb_realloc64(pGroup->aReal, (iChunk+1)*sizeof(*p));
    if( p==0 ){
      return CAPDB_NOMEM;
    }
    memset(&p[pGroup->nReal], 0, sizeof(p[0])*(iChunk+1-pGroup->nReal));
    pGroup->aReal = p;
    pGroup->nReal = iChunk+1;
  }
  if( pGroup->zName && pGroup->aReal[iChunk].z==0 ){
    char *z;
    int n = pGroup->nName;
    z = capdb_malloc64( n+5 );
    if( z==0 ){
      return CAPDB_NOMEM;
    }
    multiplexFilename(pGroup->zName, pGroup->nName, pGroup->flags, iChunk, z);
    pGroup->aReal[iChunk].z = (char*)capdb_create_filename(z,"","",0,0);
    capdb_free(z);
    if( pGroup->aReal[iChunk].z==0 ) return CAPDB_NOMEM;
  }
  return CAPDB_OK;
}

/* Translate an capdb_file* that is really a multiplexGroup* into
** the capdb_file* for the underlying original VFS.
**
** For chunk 0, the pGroup->flags determines whether or not a new file
** is created if it does not already exist.  For chunks 1 and higher, the
** file is created only if createFlag is 1.
*/
static capdb_file *multiplexSubOpen(
  multiplexGroup *pGroup,    /* The multiplexor group */
  int iChunk,                /* Which chunk to open.  0==original file */
  int *rc,                   /* Result code in and out */
  int *pOutFlags,            /* Output flags */
  int createFlag             /* True to create if iChunk>0 */
){
  capdb_file *pSubOpen = 0;
  capdb_vfs *pOrigVfs = gMultiplex.pOrigVfs;        /* Real VFS */

#ifdef CAPDB_ENABLE_8_3_NAMES
  /* If JOURNAL_8_3_OFFSET is set to (say) 400, then any overflow files are
  ** part of a database journal are named db.401, db.402, and so on. A
  ** database may therefore not grow to larger than 400 chunks. Attempting
  ** to open chunk 401 indicates the database is full. */
  if( iChunk>=CAPDB_MULTIPLEX_JOURNAL_8_3_OFFSET ){
    capdb_log(CAPDB_FULL, "multiplexed chunk overflow: %s", pGroup->zName);
    *rc = CAPDB_FULL;
    return 0;
  }
#endif

  *rc = multiplexSubFilename(pGroup, iChunk);
  if( (*rc)==CAPDB_OK && (pSubOpen = pGroup->aReal[iChunk].p)==0 ){
    int flags, bExists;
    flags = pGroup->flags;
    if( createFlag ){
      flags |= CAPDB_OPEN_CREATE;
    }else if( iChunk==0 ){
      /* Fall through */
    }else if( pGroup->aReal[iChunk].z==0 ){
      return 0;
    }else{
      *rc = pOrigVfs->xAccess(pOrigVfs, pGroup->aReal[iChunk].z,
                              CAPDB_ACCESS_EXISTS, &bExists);
     if( *rc || !bExists ){
        if( *rc ){
          capdb_log(*rc, "multiplexor.xAccess failure on %s",
                      pGroup->aReal[iChunk].z);
        }
        return 0;
      }
      flags &= ~CAPDB_OPEN_CREATE;
    }
    pSubOpen = capdb_malloc64( pOrigVfs->szOsFile );
    if( pSubOpen==0 ){
      *rc = CAPDB_IOERR_NOMEM;
      return 0;
    }
    pGroup->aReal[iChunk].p = pSubOpen;
    *rc = pOrigVfs->xOpen(pOrigVfs, pGroup->aReal[iChunk].z, pSubOpen,
                          flags, pOutFlags);
    if( (*rc)!=CAPDB_OK ){
      capdb_log(*rc, "multiplexor.xOpen failure on %s",
                  pGroup->aReal[iChunk].z);
      capdb_free(pSubOpen);
      pGroup->aReal[iChunk].p = 0;
      return 0;
    }
  }
  return pSubOpen;
}

/*
** Return the size, in bytes, of chunk number iChunk.  If that chunk
** does not exist, then return 0.  This function does not distinguish between
** non-existent files and zero-length files.
*/
static capdb_int64 multiplexSubSize(
  multiplexGroup *pGroup,    /* The multiplexor group */
  int iChunk,                /* Which chunk to open.  0==original file */
  int *rc                    /* Result code in and out */
){
  capdb_file *pSub;
  capdb_int64 sz = 0;

  if( *rc ) return 0;
  pSub = multiplexSubOpen(pGroup, iChunk, rc, NULL, 0);
  if( pSub==0 ) return 0;
  *rc = pSub->pMethods->xFileSize(pSub, &sz);
  return sz;
}  

/*
** This is the implementation of the multiplex_control() SQL function.
*/
static void multiplexControlFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int rc = CAPDB_OK;
  capdb *db = capdb_context_db_handle(context);
  int op = 0;
  int iVal;

  if( !db || argc!=2 ){
    rc = CAPDB_ERROR;
  }else{
    /* extract params */
    op = capdb_value_int(argv[0]);
    iVal = capdb_value_int(argv[1]);
    /* map function op to file_control op */
    switch( op ){
      case 1:
        op = MULTIPLEX_CTRL_ENABLE;
        break;
      case 2:
        op = MULTIPLEX_CTRL_SET_CHUNK_SIZE;
        break;
      case 3:
        op = MULTIPLEX_CTRL_SET_MAX_CHUNKS;
        break;
      default:
        rc = CAPDB_NOTFOUND;
        break;
    }
  }
  if( rc==CAPDB_OK ){
    rc = capdb_file_control(db, 0, op, &iVal);
  }
  capdb_result_error_code(context, rc);
}

/*
** This is the entry point to register the auto-extension for the
** multiplex_control() function.
*/
static int multiplexFuncInit(
  capdb *db,
  char **pzErrMsg,
  const capdb_api_routines *pApi
){
  int rc;
  rc = capdb_create_function(db, "multiplex_control", 2, CAPDB_ANY,
      0, multiplexControlFunc, 0, 0);
  return rc;
}

/*
** Close a single sub-file in the connection group.
*/
static void multiplexSubClose(
  multiplexGroup *pGroup,
  int iChunk,
  capdb_vfs *pOrigVfs
){
  capdb_file *pSubOpen = pGroup->aReal[iChunk].p;
  if( pSubOpen ){
    pSubOpen->pMethods->xClose(pSubOpen);
    if( pOrigVfs && pGroup->aReal[iChunk].z ){
      pOrigVfs->xDelete(pOrigVfs, pGroup->aReal[iChunk].z, 0);
    }
    capdb_free(pGroup->aReal[iChunk].p);
  }
  capdb_free_filename(pGroup->aReal[iChunk].z);
  memset(&pGroup->aReal[iChunk], 0, sizeof(pGroup->aReal[iChunk]));
}

/*
** Deallocate memory held by a multiplexGroup
*/
static void multiplexFreeComponents(multiplexGroup *pGroup){
  int i;
  for(i=0; i<pGroup->nReal; i++){ multiplexSubClose(pGroup, i, 0); }
  capdb_free(pGroup->aReal);
  pGroup->aReal = 0;
  pGroup->nReal = 0;
}


/************************* VFS Method Wrappers *****************************/

/*
** This is the xOpen method used for the "multiplex" VFS.
**
** Most of the work is done by the underlying original VFS.  This method
** simply links the new file into the appropriate multiplex group if it is a
** file that needs to be tracked.
*/
static int multiplexOpen(
  capdb_vfs *pVfs,         /* The multiplex VFS */
  const char *zName,         /* Name of file to be opened */
  capdb_file *pConn,       /* Fill in this file descriptor */
  int flags,                 /* Flags to control the opening */
  int *pOutFlags             /* Flags showing results of opening */
){
  int rc = CAPDB_OK;                  /* Result code */
  multiplexConn *pMultiplexOpen;       /* The new multiplex file descriptor */
  multiplexGroup *pGroup = 0;          /* Corresponding multiplexGroup object */
  capdb_file *pSubOpen = 0;                    /* Real file descriptor */
  capdb_vfs *pOrigVfs = gMultiplex.pOrigVfs;   /* Real VFS */
  int nName = 0;
  int sz = 0;
  char *zToFree = 0;

  UNUSED_PARAMETER(pVfs);
  memset(pConn, 0, pVfs->szOsFile);
  assert( zName || (flags & CAPDB_OPEN_DELETEONCLOSE) );

  /* We need to create a group structure and manage
  ** access to this group of files.
  */
  pMultiplexOpen = (multiplexConn*)pConn;

  if( rc==CAPDB_OK ){
    /* allocate space for group */
    nName = zName ? multiplexStrlen30(zName) : 0;
    sz = sizeof(multiplexGroup)                             /* multiplexGroup */
       + nName + 1;                                         /* zName */
    pGroup = capdb_malloc64( sz );
    if( pGroup==0 ){
      rc = CAPDB_NOMEM;
    }
  }

  if( rc==CAPDB_OK ){
    const char *zUri = (flags & CAPDB_OPEN_URI) ? zName : 0;
    /* assign pointers to extra space allocated */
    memset(pGroup, 0, sz);
    pMultiplexOpen->pGroup = pGroup;
    pGroup->bEnabled = (unsigned char)-1;
    pGroup->bTruncate = (unsigned char)capdb_uri_boolean(zUri, "truncate",
                                   (flags & CAPDB_OPEN_MAIN_DB)==0);
    pGroup->szChunk = (int)capdb_uri_int64(zUri, "chunksize",
                                        CAPDB_MULTIPLEX_CHUNK_SIZE);
    pGroup->szChunk = (pGroup->szChunk+0xffff)&~0xffff;
    if( zName ){
      char *p = (char *)&pGroup[1];
      pGroup->zName = p;
      memcpy(pGroup->zName, zName, nName+1);
      pGroup->nName = nName;
    }
    if( pGroup->bEnabled ){
      /* Make sure that the chunksize is such that the pending byte does not
      ** falls at the end of a chunk.  A region of up to 64K following
      ** the pending byte is never written, so if the pending byte occurs
      ** near the end of a chunk, that chunk will be too small. */
#ifndef CAPDB_OMIT_WSD
      extern int capdbPendingByte;
#else
      int capdbPendingByte = 0x40000000;
#endif
      while( (capdbPendingByte % pGroup->szChunk)>=(pGroup->szChunk-65536) ){
        pGroup->szChunk += 65536;
      }
    }
    pGroup->flags = (flags & ~CAPDB_OPEN_URI);
    rc = multiplexSubFilename(pGroup, 1);
    if( rc==CAPDB_OK ){
      pSubOpen = multiplexSubOpen(pGroup, 0, &rc, pOutFlags, 0);
      if( pSubOpen==0 && rc==CAPDB_OK ) rc = CAPDB_CANTOPEN;
    }
    if( rc==CAPDB_OK ){
      capdb_int64 sz64;

      rc = pSubOpen->pMethods->xFileSize(pSubOpen, &sz64);
      if( rc==CAPDB_OK && zName ){
        int bExists;
        if( flags & CAPDB_OPEN_SUPER_JOURNAL ){
          pGroup->bEnabled = 0;
        }else
        if( sz64==0 ){
          if( flags & CAPDB_OPEN_MAIN_JOURNAL ){
            /* If opening a main journal file and the first chunk is zero
            ** bytes in size, delete any subsequent chunks from the
            ** file-system. */
            int iChunk = 1;
            do {
              rc = pOrigVfs->xAccess(pOrigVfs,
                  pGroup->aReal[iChunk].z, CAPDB_ACCESS_EXISTS, &bExists
              );
              if( rc==CAPDB_OK && bExists ){
                rc = pOrigVfs->xDelete(pOrigVfs, pGroup->aReal[iChunk].z, 0);
                if( rc==CAPDB_OK ){
                  rc = multiplexSubFilename(pGroup, ++iChunk);
                }
              }
            }while( rc==CAPDB_OK && bExists );
          }
        }else{
          /* If the first overflow file exists and if the size of the main file
          ** is different from the chunk size, that means the chunk size is set
          ** set incorrectly.  So fix it.
          **
          ** Or, if the first overflow file does not exist and the main file is
          ** larger than the chunk size, that means the chunk size is too small.
          ** But we have no way of determining the intended chunk size, so
          ** just disable the multiplexor all together.
          */
          rc = pOrigVfs->xAccess(pOrigVfs, pGroup->aReal[1].z,
              CAPDB_ACCESS_EXISTS, &bExists);
          bExists = multiplexSubSize(pGroup, 1, &rc)>0;
          if( rc==CAPDB_OK && bExists && sz64==(sz64&0xffff0000) && sz64>0
              && sz64!=pGroup->szChunk ){
            pGroup->szChunk = (int)sz64;
          }else if( rc==CAPDB_OK && !bExists && sz64>pGroup->szChunk ){
            pGroup->bEnabled = 0;
          }
        }
      }
    }

    if( rc==CAPDB_OK ){
      if( pSubOpen->pMethods->iVersion==1 ){
        pConn->pMethods = &gMultiplex.sIoMethodsV1;
      }else{
        pConn->pMethods = &gMultiplex.sIoMethodsV2;
      }
    }else{
      multiplexFreeComponents(pGroup);
      capdb_free(pGroup);
    }
  }
  capdb_free(zToFree);
  return rc;
}

/*
** This is the xDelete method used for the "multiplex" VFS.
** It attempts to delete the filename specified.
*/
static int multiplexDelete(
  capdb_vfs *pVfs,         /* The multiplex VFS */
  const char *zName,         /* Name of file to delete */
  int syncDir
){
  int rc;
  capdb_vfs *pOrigVfs = gMultiplex.pOrigVfs;   /* Real VFS */
  rc = pOrigVfs->xDelete(pOrigVfs, zName, syncDir);
  if( rc==CAPDB_OK ){
    /* If the main chunk was deleted successfully, also delete any subsequent
    ** chunks - starting with the last (highest numbered).
    */
    int nName = (int)strlen(zName);
    char *z;
    z = capdb_malloc64(nName + 5);
    if( z==0 ){
      rc = CAPDB_IOERR_NOMEM;
    }else{
      int iChunk = 0;
      int bExists;
      do{
        multiplexFilename(zName, nName, CAPDB_OPEN_MAIN_JOURNAL, ++iChunk, z);
        rc = pOrigVfs->xAccess(pOrigVfs, z, CAPDB_ACCESS_EXISTS, &bExists);
      }while( rc==CAPDB_OK && bExists );
      while( rc==CAPDB_OK && iChunk>1 ){
        multiplexFilename(zName, nName, CAPDB_OPEN_MAIN_JOURNAL, --iChunk, z);
        rc = pOrigVfs->xDelete(pOrigVfs, z, syncDir);
      }
      if( rc==CAPDB_OK ){
        iChunk = 0;
        do{
          multiplexFilename(zName, nName, CAPDB_OPEN_WAL, ++iChunk, z);
          rc = pOrigVfs->xAccess(pOrigVfs, z, CAPDB_ACCESS_EXISTS, &bExists);
        }while( rc==CAPDB_OK && bExists );
        while( rc==CAPDB_OK && iChunk>1 ){
          multiplexFilename(zName, nName, CAPDB_OPEN_WAL, --iChunk, z);
          rc = pOrigVfs->xDelete(pOrigVfs, z, syncDir);
        }
      }
    }
    capdb_free(z);
  }
  return rc;
}

static int multiplexAccess(capdb_vfs *a, const char *b, int c, int *d){
  return gMultiplex.pOrigVfs->xAccess(gMultiplex.pOrigVfs, b, c, d);
}
static int multiplexFullPathname(capdb_vfs *a, const char *b, int c, char *d){
  return gMultiplex.pOrigVfs->xFullPathname(gMultiplex.pOrigVfs, b, c, d);
}
static void *multiplexDlOpen(capdb_vfs *a, const char *b){
  return gMultiplex.pOrigVfs->xDlOpen(gMultiplex.pOrigVfs, b);
}
static void multiplexDlError(capdb_vfs *a, int b, char *c){
  gMultiplex.pOrigVfs->xDlError(gMultiplex.pOrigVfs, b, c);
}
static void (*multiplexDlSym(capdb_vfs *a, void *b, const char *c))(void){
  return gMultiplex.pOrigVfs->xDlSym(gMultiplex.pOrigVfs, b, c);
}
static void multiplexDlClose(capdb_vfs *a, void *b){
  gMultiplex.pOrigVfs->xDlClose(gMultiplex.pOrigVfs, b);
}
static int multiplexRandomness(capdb_vfs *a, int b, char *c){
  return gMultiplex.pOrigVfs->xRandomness(gMultiplex.pOrigVfs, b, c);
}
static int multiplexSleep(capdb_vfs *a, int b){
  return gMultiplex.pOrigVfs->xSleep(gMultiplex.pOrigVfs, b);
}
static int multiplexCurrentTime(capdb_vfs *a, double *b){
  return gMultiplex.pOrigVfs->xCurrentTime(gMultiplex.pOrigVfs, b);
}
static int multiplexGetLastError(capdb_vfs *a, int b, char *c){
  if( gMultiplex.pOrigVfs->xGetLastError ){
    return gMultiplex.pOrigVfs->xGetLastError(gMultiplex.pOrigVfs, b, c);
  }else{
    return 0;
  }
}
static int multiplexCurrentTimeInt64(capdb_vfs *a, capdb_int64 *b){
  return gMultiplex.pOrigVfs->xCurrentTimeInt64(gMultiplex.pOrigVfs, b);
}

/************************ I/O Method Wrappers *******************************/

/* xClose requests get passed through to the original VFS.
** We loop over all open chunk handles and close them.
** The group structure for this file is unlinked from
** our list of groups and freed.
*/
static int multiplexClose(capdb_file *pConn){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_OK;
  multiplexFreeComponents(pGroup);
  capdb_free(pGroup);
  return rc;
}

/* Pass xRead requests thru to the original VFS after
** determining the correct chunk to operate on.
** Break up reads across chunk boundaries.
*/
static int multiplexRead(
  capdb_file *pConn,
  void *pBuf,
  int iAmt,
  capdb_int64 iOfst
){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_OK;
  if( !pGroup->bEnabled ){
    capdb_file *pSubOpen = multiplexSubOpen(pGroup, 0, &rc, NULL, 0);
    if( pSubOpen==0 ){
      rc = CAPDB_IOERR_READ;
    }else{
      rc = pSubOpen->pMethods->xRead(pSubOpen, pBuf, iAmt, iOfst);
    }
  }else{
    while( iAmt > 0 ){
      int i = (int)(iOfst / pGroup->szChunk);
      capdb_file *pSubOpen;
      pSubOpen = multiplexSubOpen(pGroup, i, &rc, NULL, 1);
      if( pSubOpen ){
        int extra = ((int)(iOfst % pGroup->szChunk) + iAmt) - pGroup->szChunk;
        if( extra<0 ) extra = 0;
        iAmt -= extra;
        rc = pSubOpen->pMethods->xRead(pSubOpen, pBuf, iAmt,
                                       iOfst % pGroup->szChunk);
        if( rc!=CAPDB_OK ) break;
        pBuf = (char *)pBuf + iAmt;
        iOfst += iAmt;
        iAmt = extra;
      }else{
        rc = CAPDB_IOERR_READ;
        break;
      }
    }
  }

  return rc;
}

/* Pass xWrite requests thru to the original VFS after
** determining the correct chunk to operate on.
** Break up writes across chunk boundaries.
*/
static int multiplexWrite(
  capdb_file *pConn,
  const void *pBuf,
  int iAmt,
  capdb_int64 iOfst
){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_OK;
  if( !pGroup->bEnabled ){
    capdb_file *pSubOpen = multiplexSubOpen(pGroup, 0, &rc, NULL, 0);
    if( pSubOpen==0 ){
      rc = CAPDB_IOERR_WRITE;
    }else{
      rc = pSubOpen->pMethods->xWrite(pSubOpen, pBuf, iAmt, iOfst);
    }
  }else{
    while( rc==CAPDB_OK && iAmt>0 ){
      int i = (int)(iOfst / pGroup->szChunk);
      capdb_file *pSubOpen = multiplexSubOpen(pGroup, i, &rc, NULL, 1);
      if( pSubOpen ){
        int extra = ((int)(iOfst % pGroup->szChunk) + iAmt) -
                    pGroup->szChunk;
        if( extra<0 ) extra = 0;
        iAmt -= extra;
        rc = pSubOpen->pMethods->xWrite(pSubOpen, pBuf, iAmt,
                                        iOfst % pGroup->szChunk);
        pBuf = (char *)pBuf + iAmt;
        iOfst += iAmt;
        iAmt = extra;
      }
    }
  }
  return rc;
}

/* Pass xTruncate requests thru to the original VFS after
** determining the correct chunk to operate on.  Delete any
** chunks above the truncate mark.
*/
static int multiplexTruncate(capdb_file *pConn, capdb_int64 size){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_OK;
  if( !pGroup->bEnabled ){
    capdb_file *pSubOpen = multiplexSubOpen(pGroup, 0, &rc, NULL, 0);
    if( pSubOpen==0 ){
      rc = CAPDB_IOERR_TRUNCATE;
    }else{
      rc = pSubOpen->pMethods->xTruncate(pSubOpen, size);
    }
  }else{
    int i;
    int iBaseGroup = (int)(size / pGroup->szChunk);
    capdb_file *pSubOpen;
    capdb_vfs *pOrigVfs = gMultiplex.pOrigVfs;   /* Real VFS */
    /* delete the chunks above the truncate limit */
    for(i = pGroup->nReal-1; i>iBaseGroup && rc==CAPDB_OK; i--){
      if( pGroup->bTruncate ){
        multiplexSubClose(pGroup, i, pOrigVfs);
      }else{
        pSubOpen = multiplexSubOpen(pGroup, i, &rc, 0, 0);
        if( pSubOpen ){
          rc = pSubOpen->pMethods->xTruncate(pSubOpen, 0);
        }
      }
    }
    if( rc==CAPDB_OK ){
      pSubOpen = multiplexSubOpen(pGroup, iBaseGroup, &rc, 0, 0);
      if( pSubOpen ){
        rc = pSubOpen->pMethods->xTruncate(pSubOpen, size % pGroup->szChunk);
      }
    }
    if( rc ) rc = CAPDB_IOERR_TRUNCATE;
  }
  return rc;
}

/* Pass xSync requests through to the original VFS without change
*/
static int multiplexSync(capdb_file *pConn, int flags){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_OK;
  int i;
  for(i=0; i<pGroup->nReal; i++){
    capdb_file *pSubOpen = pGroup->aReal[i].p;
    if( pSubOpen ){
      int rc2 = pSubOpen->pMethods->xSync(pSubOpen, flags);
      if( rc2!=CAPDB_OK ) rc = rc2;
    }
  }
  return rc;
}

/* Pass xFileSize requests through to the original VFS.
** Aggregate the size of all the chunks before returning.
*/
static int multiplexFileSize(capdb_file *pConn, capdb_int64 *pSize){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_OK;
  int i;
  if( !pGroup->bEnabled ){
    capdb_file *pSubOpen = multiplexSubOpen(pGroup, 0, &rc, NULL, 0);
    if( pSubOpen==0 ){
      rc = CAPDB_IOERR_FSTAT;
    }else{
      rc = pSubOpen->pMethods->xFileSize(pSubOpen, pSize);
    }
  }else{
    *pSize = 0;
    for(i=0; rc==CAPDB_OK; i++){
      capdb_int64 sz = multiplexSubSize(pGroup, i, &rc);
      if( sz==0 ) break;
      *pSize = i*(capdb_int64)pGroup->szChunk + sz;
    }
  }
  return rc;
}

/* Pass xLock requests through to the original VFS unchanged.
*/
static int multiplexLock(capdb_file *pConn, int lock){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xLock(pSubOpen, lock);
  }
  return CAPDB_BUSY;
}

/* Pass xUnlock requests through to the original VFS unchanged.
*/
static int multiplexUnlock(capdb_file *pConn, int lock){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xUnlock(pSubOpen, lock);
  }
  return CAPDB_IOERR_UNLOCK;
}

/* Pass xCheckReservedLock requests through to the original VFS unchanged.
*/
static int multiplexCheckReservedLock(capdb_file *pConn, int *pResOut){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xCheckReservedLock(pSubOpen, pResOut);
  }
  return CAPDB_IOERR_CHECKRESERVEDLOCK;
}

/* Pass xFileControl requests through to the original VFS unchanged,
** except for any MULTIPLEX_CTRL_* requests here.
*/
static int multiplexFileControl(capdb_file *pConn, int op, void *pArg){
  multiplexConn *p = (multiplexConn*)pConn;
  multiplexGroup *pGroup = p->pGroup;
  int rc = CAPDB_ERROR;
  capdb_file *pSubOpen;

  if( !gMultiplex.isInitialized ) return CAPDB_MISUSE;
  switch( op ){
    case MULTIPLEX_CTRL_ENABLE:
      if( pArg ) {
        int bEnabled = *(int *)pArg;
        pGroup->bEnabled = (unsigned char)bEnabled;
        rc = CAPDB_OK;
      }
      break;
    case MULTIPLEX_CTRL_SET_CHUNK_SIZE:
      if( pArg ) {
        unsigned int szChunk = *(unsigned*)pArg;
        if( szChunk<1 ){
          rc = CAPDB_MISUSE;
        }else{
          /* Round up to nearest multiple of MAX_PAGE_SIZE. */
          szChunk = (szChunk + (MAX_PAGE_SIZE-1));
          szChunk &= ~(MAX_PAGE_SIZE-1);
          pGroup->szChunk = szChunk;
          rc = CAPDB_OK;
        }
      }
      break;
    case MULTIPLEX_CTRL_SET_MAX_CHUNKS:
      rc = CAPDB_OK;
      break;
    case CAPDB_FCNTL_SIZE_HINT:
    case CAPDB_FCNTL_CHUNK_SIZE:
      /* no-op these */
      rc = CAPDB_OK;
      break;
    case CAPDB_FCNTL_PRAGMA: {
      char **aFcntl = (char**)pArg;
      /*
      ** EVIDENCE-OF: R-29875-31678 The argument to the CAPDB_FCNTL_PRAGMA
      ** file control is an array of pointers to strings (char**) in which the
      ** second element of the array is the name of the pragma and the third
      ** element is the argument to the pragma or NULL if the pragma has no
      ** argument.
      */
      if( aFcntl[1] && capdb_strnicmp(aFcntl[1],"multiplex_",10)==0 ){
        capdb_int64 sz = 0;
        (void)multiplexFileSize(pConn, &sz);
        /*
        ** PRAGMA multiplex_truncate=BOOLEAN;
        ** PRAGMA multiplex_truncate;
        **
        ** Turn the multiplexor truncate feature on or off.  Return either
        ** "on" or "off" to indicate the new setting.  If the BOOLEAN argument
        ** is omitted, just return the current value for the truncate setting.
        */
        if( capdb_stricmp(aFcntl[1],"multiplex_truncate")==0 ){
          if( aFcntl[2] && aFcntl[2][0] ){
            if( capdb_stricmp(aFcntl[2], "on")==0
             || capdb_stricmp(aFcntl[2], "1")==0 ){
              pGroup->bTruncate = 1;
            }else
            if( capdb_stricmp(aFcntl[2], "off")==0
             || capdb_stricmp(aFcntl[2], "0")==0 ){
              pGroup->bTruncate = 0;
            }
          }
          /* EVIDENCE-OF: R-27806-26076 The handler for an CAPDB_FCNTL_PRAGMA
          ** file control can optionally make the first element of the char**
          ** argument point to a string obtained from capdb_mprintf() or the
          ** equivalent and that string will become the result of the pragma
          ** or the error message if the pragma fails.
          */
          aFcntl[0] = capdb_mprintf(pGroup->bTruncate ? "on" : "off");
          rc = CAPDB_OK;
          break;
        }
        /*
        ** PRAGMA multiplex_enabled;
        **
        ** Return 0 or 1 depending on whether the multiplexor is enabled or
        ** disabled, respectively.
        */
        if( capdb_stricmp(aFcntl[1],"multiplex_enabled")==0 ){
          aFcntl[0] = capdb_mprintf("%d", pGroup->bEnabled!=0);
          rc = CAPDB_OK;
          break;
        }
        /*
        ** PRAGMA multiplex_chunksize;
        **
        ** Return the chunksize for the multiplexor, or no-op if the
        ** multiplexor is not active.
        */
        if( capdb_stricmp(aFcntl[1],"multiplex_chunksize")==0
         && pGroup->bEnabled
        ){
          aFcntl[0] = capdb_mprintf("%u", pGroup->szChunk);
          rc = CAPDB_OK;
          break;
        }
        /*
        ** PRAGMA multiplex_filecount;
        **
        ** Return the number of disk files currently in use by the
        ** multiplexor.  This should be the total database size size
        ** divided by the chunksize and rounded up.
        */
        if( capdb_stricmp(aFcntl[1],"multiplex_filecount")==0 ){
          int n = 0;
          int ii;
          for(ii=0; ii<pGroup->nReal; ii++){
            if( pGroup->aReal[ii].p!=0 ) n++;
          }
          aFcntl[0] = capdb_mprintf("%d", n);
          rc = CAPDB_OK;
          break;
        }
      }
      /* If the multiplexor does not handle the pragma, pass it through
      ** into the default case. */
    }
    default:
      pSubOpen = multiplexSubOpen(pGroup, 0, &rc, NULL, 0);
      if( pSubOpen ){
        rc = pSubOpen->pMethods->xFileControl(pSubOpen, op, pArg);
        if( op==CAPDB_FCNTL_VFSNAME && rc==CAPDB_OK ){
         *(char**)pArg = capdb_mprintf("multiplex/%z", *(char**)pArg);
        }
      }
      break;
  }
  return rc;
}

/* Pass xSectorSize requests through to the original VFS unchanged.
*/
static int multiplexSectorSize(capdb_file *pConn){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen && pSubOpen->pMethods->xSectorSize ){
    return pSubOpen->pMethods->xSectorSize(pSubOpen);
  }
  return DEFAULT_SECTOR_SIZE;
}

/* Pass xDeviceCharacteristics requests through to the original VFS unchanged.
*/
static int multiplexDeviceCharacteristics(capdb_file *pConn){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xDeviceCharacteristics(pSubOpen);
  }
  return 0;
}

/* Pass xShmMap requests through to the original VFS unchanged.
*/
static int multiplexShmMap(
  capdb_file *pConn,            /* Handle open on database file */
  int iRegion,                    /* Region to retrieve */
  int szRegion,                   /* Size of regions */
  int bExtend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xShmMap(pSubOpen, iRegion, szRegion, bExtend,pp);
  }
  return CAPDB_IOERR;
}

/* Pass xShmLock requests through to the original VFS unchanged.
*/
static int multiplexShmLock(
  capdb_file *pConn,       /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xShmLock(pSubOpen, ofst, n, flags);
  }
  return CAPDB_BUSY;
}

/* Pass xShmBarrier requests through to the original VFS unchanged.
*/
static void multiplexShmBarrier(capdb_file *pConn){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    pSubOpen->pMethods->xShmBarrier(pSubOpen);
  }
}

/* Pass xShmUnmap requests through to the original VFS unchanged.
*/
static int multiplexShmUnmap(capdb_file *pConn, int deleteFlag){
  multiplexConn *p = (multiplexConn*)pConn;
  int rc;
  capdb_file *pSubOpen = multiplexSubOpen(p->pGroup, 0, &rc, NULL, 0);
  if( pSubOpen ){
    return pSubOpen->pMethods->xShmUnmap(pSubOpen, deleteFlag);
  }
  return CAPDB_OK;
}

/************************** Public Interfaces *****************************/
/*
** CAPI: Initialize the multiplex VFS shim - capdb_multiplex_initialize()
**
** Use the VFS named zOrigVfsName as the VFS that does the actual work.
** Use the default if zOrigVfsName==NULL.
**
** The multiplex VFS shim is named "multiplex".  It will become the default
** VFS if makeDefault is non-zero.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once
** during start-up.
*/
int capdb_multiplex_initialize(const char *zOrigVfsName, int makeDefault){
  capdb_vfs *pOrigVfs;
  if( gMultiplex.isInitialized ) return CAPDB_MISUSE;
  pOrigVfs = capdb_vfs_find(zOrigVfsName);
  if( pOrigVfs==0 ) return CAPDB_ERROR;
  assert( pOrigVfs!=&gMultiplex.sThisVfs );
  gMultiplex.isInitialized = 1;
  gMultiplex.pOrigVfs = pOrigVfs;
  gMultiplex.sThisVfs = *pOrigVfs;
  gMultiplex.sThisVfs.szOsFile += sizeof(multiplexConn);
  gMultiplex.sThisVfs.zName = CAPDB_MULTIPLEX_VFS_NAME;
  gMultiplex.sThisVfs.xOpen = multiplexOpen;
  gMultiplex.sThisVfs.xDelete = multiplexDelete;
  gMultiplex.sThisVfs.xAccess = multiplexAccess;
  gMultiplex.sThisVfs.xFullPathname = multiplexFullPathname;
  gMultiplex.sThisVfs.xDlOpen = multiplexDlOpen;
  gMultiplex.sThisVfs.xDlError = multiplexDlError;
  gMultiplex.sThisVfs.xDlSym = multiplexDlSym;
  gMultiplex.sThisVfs.xDlClose = multiplexDlClose;
  gMultiplex.sThisVfs.xRandomness = multiplexRandomness;
  gMultiplex.sThisVfs.xSleep = multiplexSleep;
  gMultiplex.sThisVfs.xCurrentTime = multiplexCurrentTime;
  gMultiplex.sThisVfs.xGetLastError = multiplexGetLastError;
  gMultiplex.sThisVfs.xCurrentTimeInt64 = multiplexCurrentTimeInt64;

  gMultiplex.sIoMethodsV1.iVersion = 1;
  gMultiplex.sIoMethodsV1.xClose = multiplexClose;
  gMultiplex.sIoMethodsV1.xRead = multiplexRead;
  gMultiplex.sIoMethodsV1.xWrite = multiplexWrite;
  gMultiplex.sIoMethodsV1.xTruncate = multiplexTruncate;
  gMultiplex.sIoMethodsV1.xSync = multiplexSync;
  gMultiplex.sIoMethodsV1.xFileSize = multiplexFileSize;
  gMultiplex.sIoMethodsV1.xLock = multiplexLock;
  gMultiplex.sIoMethodsV1.xUnlock = multiplexUnlock;
  gMultiplex.sIoMethodsV1.xCheckReservedLock = multiplexCheckReservedLock;
  gMultiplex.sIoMethodsV1.xFileControl = multiplexFileControl;
  gMultiplex.sIoMethodsV1.xSectorSize = multiplexSectorSize;
  gMultiplex.sIoMethodsV1.xDeviceCharacteristics =
                                            multiplexDeviceCharacteristics;
  gMultiplex.sIoMethodsV2 = gMultiplex.sIoMethodsV1;
  gMultiplex.sIoMethodsV2.iVersion = 2;
  gMultiplex.sIoMethodsV2.xShmMap = multiplexShmMap;
  gMultiplex.sIoMethodsV2.xShmLock = multiplexShmLock;
  gMultiplex.sIoMethodsV2.xShmBarrier = multiplexShmBarrier;
  gMultiplex.sIoMethodsV2.xShmUnmap = multiplexShmUnmap;
  capdb_vfs_register(&gMultiplex.sThisVfs, makeDefault);

  capdb_auto_extension((void(*)(void))multiplexFuncInit);

  return CAPDB_OK;
}

/*
** CAPI: Shutdown the multiplex system - capdb_multiplex_shutdown()
**
** All SQLite database connections must be closed before calling this
** routine.
**
** THIS ROUTINE IS NOT THREADSAFE.  Call this routine exactly once while
** shutting down in order to free all remaining multiplex groups.
*/
int capdb_multiplex_shutdown(int eForce){
  int rc = CAPDB_OK;
  if( gMultiplex.isInitialized==0 ) return CAPDB_MISUSE;
  gMultiplex.isInitialized = 0;
  capdb_vfs_unregister(&gMultiplex.sThisVfs);
  memset(&gMultiplex, 0, sizeof(gMultiplex));
  return rc;
}

/***************************** Test Code ***********************************/
#ifdef CAPDB_TEST
#include "tclsqlite.h"
extern const char *capdbErrName(int);


/*
** tclcmd: capdb_multiplex_initialize NAME MAKEDEFAULT
*/
static int CAPDB_TCLAPI test_multiplex_initialize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *zName;              /* Name of new multiplex VFS */
  int makeDefault;                /* True to make the new VFS the default */
  int rc;                         /* Value returned by multiplex_initialize() */

  UNUSED_PARAMETER(clientData);

  /* Process arguments */
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME MAKEDEFAULT");
    return TCL_ERROR;
  }
  zName = Tcl_GetString(objv[1]);
  if( Tcl_GetBooleanFromObj(interp, objv[2], &makeDefault) ) return TCL_ERROR;
  if( zName[0]=='\0' ) zName = 0;

  /* Call capdb_multiplex_initialize() */
  rc = capdb_multiplex_initialize(zName, makeDefault);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_STATIC);

  return TCL_OK;
}

/*
** tclcmd: capdb_multiplex_shutdown
*/
static int CAPDB_TCLAPI test_multiplex_shutdown(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Value returned by multiplex_shutdown() */

  UNUSED_PARAMETER(clientData);

  if( objc==2 && strcmp(Tcl_GetString(objv[1]),"-force")!=0 ){
    objc = 3;
  }
  if( (objc!=1 && objc!=2) ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-force?");
    return TCL_ERROR;
  }

  /* Call capdb_multiplex_shutdown() */
  rc = capdb_multiplex_shutdown(objc==2);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_STATIC);

  return TCL_OK;
}

/*
** Tclcmd: test_multiplex_control HANDLE DBNAME SUB-COMMAND ?INT-VALUE?
*/
static int CAPDB_TCLAPI test_multiplex_control(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Return code from file_control() */
  int idx;                        /* Index in aSub[] */
  Tcl_CmdInfo cmdInfo;            /* Command info structure for HANDLE */
  capdb *db;                    /* Underlying db handle for HANDLE */
  int iValue = 0;
  void *pArg = 0;

  struct SubCommand {
    const char *zName;
    int op;
    int argtype;
  } aSub[] = {
    { "enable",       MULTIPLEX_CTRL_ENABLE,           1 },
    { "chunk_size",   MULTIPLEX_CTRL_SET_CHUNK_SIZE,   1 },
    { "max_chunks",   MULTIPLEX_CTRL_SET_MAX_CHUNKS,   1 },
    { 0, 0, 0 }
  };

  if( objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "HANDLE DBNAME SUB-COMMAND INT-VALUE");
    return TCL_ERROR;
  }

  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(objv[1]), &cmdInfo) ){
    Tcl_AppendResult(interp, "expected database handle, got \"", NULL);
    Tcl_AppendResult(interp, Tcl_GetString(objv[1]), "\"", NULL);
    return TCL_ERROR;
  }else{
    db = *(capdb **)cmdInfo.objClientData;
  }

  rc = Tcl_GetIndexFromObjStruct(
      interp, objv[3], aSub, sizeof(aSub[0]), "sub-command", 0, &idx
  );
  if( rc!=TCL_OK ) return rc;

  switch( aSub[idx].argtype ){
    case 1:
      if( Tcl_GetIntFromObj(interp, objv[4], &iValue) ){
        return TCL_ERROR;
      }
      pArg = (void *)&iValue;
      break;
    default:
      Tcl_WrongNumArgs(interp, 4, objv, "SUB-COMMAND");
      return TCL_ERROR;
  }

  rc = capdb_file_control(db, Tcl_GetString(objv[2]), aSub[idx].op, pArg);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_STATIC);
  return (rc==CAPDB_OK) ? TCL_OK : TCL_ERROR;
}

/*
** This routine registers the custom TCL commands defined in this
** module.  This should be the only procedure visible from outside
** of this module.
*/
int Sqlitemultiplex_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
  } aCmd[] = {
    { "capdb_multiplex_initialize", test_multiplex_initialize },
    { "capdb_multiplex_shutdown", test_multiplex_shutdown },
    { "capdb_multiplex_control", test_multiplex_control },
  };
  int i;

  for(i=0; i<sizeof(aCmd)/sizeof(aCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
  }

  return TCL_OK;
}
#endif
