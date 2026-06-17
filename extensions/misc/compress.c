/*
** 2014-06-13
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
** This SQLite extension implements SQL compression functions
** compress() and uncompress() using ZLIB.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <zlib.h>

/*
** Implementation of the "compress(X)" SQL function.  The input X is
** compressed using zLib and the output is returned.
**
** The output is a BLOB that begins with a variable-length integer that
** is the input size in bytes (the size of X before compression).  The
** variable-length integer is implemented as 1 to 5 bytes.  There are
** seven bits per integer stored in the lower seven bits of each byte.
** More significant bits occur first.  The most significant bit (0x80)
** is a flag to indicate the end of the integer.
**
** This function, SQLAR, and ZIP all use the same "deflate" compression
** algorithm, but each is subtly different:
**
**   *  ZIP uses raw deflate.
**
**   *  SQLAR uses the "zlib format" which is raw deflate with a two-byte
**      algorithm-identification header and a four-byte checksum at the end.
**
**   *  This utility uses the "zlib format" like SQLAR, but adds the variable-
**      length integer uncompressed size value at the beginning.
**
** This function might be extended in the future to support compression
** formats other than deflate, by providing a different algorithm-id
** mark following the variable-length integer size parameter.
*/
static void compressFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *pIn;
  unsigned char *pOut;
  unsigned int nIn;
  unsigned long int nOut;
  unsigned char x[8];
  int rc;
  int i, j;

  pIn = capdb_value_blob(argv[0]);
  nIn = capdb_value_bytes(argv[0]);
  nOut = 13 + nIn + (nIn+999)/1000;
  pOut = capdb_malloc64( nOut+5 );
  if( pOut==0 ){
    capdb_result_error_nomem(context);
    return;
  }
  for(i=4; i>=0; i--){
    x[i] = (nIn >> (7*(4-i)))&0x7f;
  }
  for(i=0; i<4 && x[i]==0; i++){}
  for(j=0; i<=4; i++, j++) pOut[j] = x[i];
  pOut[j-1] |= 0x80;
  rc = compress(&pOut[j], &nOut, pIn, nIn);
  if( rc==Z_OK ){
    capdb_result_blob(context, pOut, nOut+j, capdb_free);
  }else{
    capdb_free(pOut);
  }
}

/*
** Implementation of the "uncompress(X)" SQL function.  The argument X
** is a blob which was obtained from compress(Y).  The output will be
** the value Y.
*/
static void uncompressFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *pIn;
  unsigned char *pOut;
  unsigned int nIn;
  unsigned long int nOut;
  int rc;
  unsigned int i;

  pIn = capdb_value_blob(argv[0]);
  nIn = capdb_value_bytes(argv[0]);
  nOut = 0;
  for(i=0; i<nIn && i<5; i++){
    nOut = (nOut<<7) | (pIn[i]&0x7f);
    if( (pIn[i]&0x80)!=0 ){ i++; break; }
  }
  pOut = capdb_malloc64( nOut+1 );
  if( pOut==0 ){
    capdb_result_error_nomem(context);
    return;
  }
  rc = uncompress(pOut, &nOut, &pIn[i], nIn-i);
  if( rc==Z_OK ){
    capdb_result_blob(context, pOut, nOut, capdb_free);
  }else{
    capdb_free(pOut);
  }
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_compress_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "compress", 1, 
                    CAPDB_UTF8 | CAPDB_INNOCUOUS,
                    0, compressFunc, 0, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "uncompress", 1,
                    CAPDB_UTF8 | CAPDB_INNOCUOUS | CAPDB_DETERMINISTIC,
                    0, uncompressFunc, 0, 0);
  }
  return rc;
}
