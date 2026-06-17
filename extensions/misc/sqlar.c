/*
** 2017-12-17
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
** Utility functions sqlar_compress() and sqlar_uncompress(). Useful
** for working with sqlar archives and used by the shell tool's built-in
** sqlar support.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <zlib.h>
#include <assert.h>

/*
** Implementation of the "sqlar_compress(X)" SQL function.
**
** If the type of X is CAPDB_BLOB, and compressing that blob using
** zlib utility function compress() yields a smaller blob, return the
** compressed blob. Otherwise, return a copy of X.
**
** SQLar uses the "zlib format" for compressed content.  The zlib format
** contains a two-byte identification header and a four-byte checksum at
** the end.  This is different from ZIP which uses the raw deflate format.
**
** Future enhancements to SQLar might add support for new compression formats.
** If so, those new formats will be identified by alternative headers in the
** compressed data.
*/
static void sqlarCompressFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  assert( argc==1 );
  if( capdb_value_type(argv[0])==CAPDB_BLOB ){
    const Bytef *pData = capdb_value_blob(argv[0]);
    uLong nData = capdb_value_bytes(argv[0]);
    uLongf nOut = compressBound(nData);
    Bytef *pOut;

    pOut = (Bytef*)capdb_malloc64(nOut);
    if( pOut==0 ){
      capdb_result_error_nomem(context);
      return;
    }else{
      if( Z_OK!=compress(pOut, &nOut, pData, nData) ){
        capdb_result_error(context, "error in compress()", -1);
      }else if( nOut<nData ){
        capdb_result_blob(context, pOut, nOut, CAPDB_TRANSIENT);
      }else{
        capdb_result_value(context, argv[0]);
      }
      capdb_free(pOut);
    }
  }else{
    capdb_result_value(context, argv[0]);
  }
}

/*
** Implementation of the "sqlar_uncompress(X,SZ)" SQL function
**
** Parameter SZ is interpreted as an integer. If it is less than or
** equal to zero, then this function returns a copy of X. Or, if
** SZ is equal to the size of X when interpreted as a blob, also
** return a copy of X. Otherwise, decompress blob X using zlib
** utility function uncompress() and return the results (another
** blob).
*/
static void sqlarUncompressFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  uLong nData;
  capdb_int64 sz;

  assert( argc==2 );
  sz = capdb_value_int64(argv[1]);

  if( sz<=0 || sz==(nData = capdb_value_bytes(argv[0])) ){
    capdb_result_value(context, argv[0]);
  }else{
    uLongf szf = sz;
    const Bytef *pData= capdb_value_blob(argv[0]);
    Bytef *pOut = capdb_malloc64(sz);
    if( pOut==0 ){
      capdb_result_error_nomem(context);
    }else if( Z_OK!=uncompress(pOut, &szf, pData, nData) ){
      capdb_result_error(context, "error in uncompress()", -1);
    }else{
      capdb_result_blob(context, pOut, szf, CAPDB_TRANSIENT);
    }
    capdb_free(pOut);
  }
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_sqlar_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "sqlar_compress", 1, 
                               CAPDB_UTF8|CAPDB_INNOCUOUS, 0,
                               sqlarCompressFunc, 0, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "sqlar_uncompress", 2,
                                 CAPDB_UTF8|CAPDB_INNOCUOUS, 0,
                                 sqlarUncompressFunc, 0, 0);
  }
  return rc;
}
