/*
** 2016 September 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains test code to delete an SQLite database and all
** of its associated files. Associated files include:
**
**   * The journal file.
**   * The wal file.
**   * The CAPDB_ENABLE_8_3_NAMES version of the db, journal or wal files.
**   * Files created by the test_multiplex.c module to extend any of the 
**     above.
*/

#ifndef CAPDB_OS_WIN
#  include <unistd.h>
#  include <errno.h>
#endif
#include <string.h>
#include <assert.h>
#include "capdb.h"

/* The following #defines are copied from test_multiplex.c */
#ifndef MX_CHUNK_NUMBER 
# define MX_CHUNK_NUMBER 299
#endif
#ifndef CAPDB_MULTIPLEX_JOURNAL_8_3_OFFSET
# define CAPDB_MULTIPLEX_JOURNAL_8_3_OFFSET 400
#endif
#ifndef CAPDB_MULTIPLEX_WAL_8_3_OFFSET
# define CAPDB_MULTIPLEX_WAL_8_3_OFFSET 700
#endif

/*
** This routine is a copy of (most of) the code from SQLite function
** capdbFileSuffix3(). It modifies the filename in buffer z in the
** same way as SQLite does when in 8.3 filenames mode.
*/
static void capdbDelete83Name(char *z){
  int i, sz;
  sz = (int)strlen(z);
  for(i=sz-1; i>0 && z[i]!='/' && z[i]!='.'; i--){}
  if( z[i]=='.' && (sz>i+4) ) memmove(&z[i+1], &z[sz-3], 4);
}

/*
** zFile is a filename. Assuming no error occurs, if this file exists, 
** set *pbExists to true and unlink it. Or, if the file does not exist,
** set *pbExists to false before returning.
**
** If an error occurs, non-zero is returned. Or, if no error occurs, zero.
*/
static int capdbDeleteUnlinkIfExists(
  capdb_vfs *pVfs,
  const char *zFile, 
  int *pbExists
){
  int rc = CAPDB_ERROR;
#ifdef _WIN32
  if( pVfs ){
    if( pbExists ) *pbExists = 1;
    rc = pVfs->xDelete(pVfs, zFile, 0);
    if( rc==CAPDB_IOERR_DELETE_NOENT ){
      if( pbExists ) *pbExists = 0;
      rc = CAPDB_OK;
    }
  }
#else
  assert( pVfs==0 );
  rc = access(zFile, F_OK);
  if( rc ){
    if( errno==ENOENT ){ 
      if( pbExists ) *pbExists = 0;
      rc = CAPDB_OK; 
    }
  }else{
    if( pbExists ) *pbExists = 1;
    rc = unlink(zFile);
  }
#endif
  return rc;
}

/*
** Delete the database file identified by the string argument passed to this
** function. The string must contain a filename, not an SQLite URI.
*/
CAPDB_API int capdb_delete_database(
  const char *zFile               /* File to delete */
){
  char *zBuf;                     /* Buffer to sprintf() filenames to */
  int nBuf;                       /* Size of buffer in bytes */
  int rc = 0;                     /* System error code */
  int i;                          /* Iterate through azFmt[] and aMFile[] */

  const char *azFmt[] = { "%s", "%s-journal", "%s-wal", "%s-shm" };

  struct MFile {
    const char *zFmt;
    int iOffset;
    int b83;
  } aMFile[] = {
    { "%s%03d",         0,   0 },
    { "%s-journal%03d", 0,   0 },
    { "%s-wal%03d",     0,   0 },
    { "%s%03d",         0,   1 },
    { "%s-journal%03d", CAPDB_MULTIPLEX_JOURNAL_8_3_OFFSET, 1 },
    { "%s-wal%03d",     CAPDB_MULTIPLEX_WAL_8_3_OFFSET, 1 },
  };

#ifdef _WIN32
  capdb_vfs *pVfs = capdb_vfs_find("win32");
#else
  capdb_vfs *pVfs = 0;
#endif

  /* Allocate a buffer large enough for any of the files that need to be
  ** deleted.  */
  nBuf = (int)strlen(zFile) + 100;
  zBuf = (char*)capdb_malloc(nBuf);
  if( zBuf==0 ) return CAPDB_NOMEM;

  /* Delete both the regular and 8.3 filenames versions of the database,
  ** journal, wal and shm files.  */
  for(i=0; rc==0 && i<sizeof(azFmt)/sizeof(azFmt[0]); i++){
    capdb_snprintf(nBuf, zBuf, azFmt[i], zFile);
    rc = capdbDeleteUnlinkIfExists(pVfs, zBuf, 0);
    if( rc==0 && i!=0 ){
      capdbDelete83Name(zBuf);
      rc = capdbDeleteUnlinkIfExists(pVfs, zBuf, 0);
    }
  }

  /* Delete any multiplexor files */
  for(i=0; rc==0 && i<sizeof(aMFile)/sizeof(aMFile[0]); i++){
    struct MFile *p = &aMFile[i];
    int iChunk;
    for(iChunk=1; iChunk<=MX_CHUNK_NUMBER; iChunk++){
      int bExists;
      capdb_snprintf(nBuf, zBuf, p->zFmt, zFile, iChunk+p->iOffset);
      if( p->b83 ) capdbDelete83Name(zBuf);
      rc = capdbDeleteUnlinkIfExists(pVfs, zBuf, &bExists);
      if( bExists==0 || rc!=0 ) break;
    }
  }

  capdb_free(zBuf);
  return (rc ? CAPDB_ERROR : CAPDB_OK);
}
