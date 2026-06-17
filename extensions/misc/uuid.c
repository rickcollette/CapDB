/*
** 2019-10-23
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
** This SQLite extension implements functions that handling RFC-4122 UUIDs
** Three SQL functions are implemented:
**
**     uuid()        - generate a version 4 UUID as a string
**     uuid_str(X)   - convert a UUID X into a well-formed UUID string
**     uuid_blob(X)  - convert a UUID X into a 16-byte blob
**
** The output from uuid() and uuid_str(X) are always well-formed RFC-4122
** UUID strings in this format:
**
**        xxxxxxxx-xxxx-Mxxx-Nxxx-xxxxxxxxxxxx
**
** All of the 'x', 'M', and 'N' values are lower-case hexadecimal digits.
** The M digit indicates the "version".  For uuid()-generated UUIDs, the
** version is always "4" (a random UUID).  The upper three bits of N digit
** are the "variant".  This library only supports variant 1 (indicated
** by values of N between '8' and 'b') as those are overwhelming the most
** common.  Other variants are for legacy compatibility only.
**
** The output of uuid_blob(X) is always a 16-byte blob.  The UUID input
** string is converted in network byte order (big-endian) in accordance
** with RFC-4122 specifications for variant-1 UUIDs.  Note that network
** byte order is *always* used, even if the input self-identifies as a
** variant-2 UUID.
**
** The input X to the uuid_str() and uuid_blob() functions can be either
** a string or a BLOB.  If it is a BLOB it must be exactly 16 bytes in
** length or else a NULL is returned.  If the input is a string it must
** consist of 32 hexadecimal digits, upper or lower case, optionally
** surrounded by {...} and with optional "-" characters interposed in the
** middle.  The flexibility of input is inspired by the PostgreSQL
** implementation of UUID functions that accept in all of the following
** formats:
**
**     A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11
**     {a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11}
**     a0eebc999c0b4ef8bb6d6bb9bd380a11
**     a0ee-bc99-9c0b-4ef8-bb6d-6bb9-bd38-0a11
**     {a0eebc99-9c0b4ef8-bb6d6bb9-bd380a11}
**
** If any of the above inputs are passed into uuid_str(), the output will
** always be in the canonical RFC-4122 format:
**
**     a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11
**
** If the X input string has too few or too many digits or contains
** stray characters other than {, }, or -, then NULL is returned.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <ctype.h>

#if !defined(CAPDB_ASCII) && !defined(CAPDB_EBCDIC)
# define CAPDB_ASCII 1
#endif

/*
** Translate a single byte of Hex into an integer.
** This routine only works if h really is a valid hexadecimal
** character:  0..9a..fA..F
*/
static unsigned char capdbUuidHexToInt(int h){
  assert( (h>='0' && h<='9') ||  (h>='a' && h<='f') ||  (h>='A' && h<='F') );
#ifdef CAPDB_ASCII
  h += 9*(1&(h>>6));
#endif
#ifdef CAPDB_EBCDIC
  h += 9*(1&~(h>>4));
#endif
  return (unsigned char)(h & 0xf);
}

/*
** Convert a 16-byte BLOB into a well-formed RFC-4122 UUID.  The output
** buffer zStr should be at least 37 bytes in length.   The output will
** be zero-terminated.
*/
static void capdbUuidBlobToStr(
  const unsigned char *aBlob,  /* Input blob */
  unsigned char *zStr          /* Write the answer here */
){
  static const char zDigits[] = "0123456789abcdef";
  int i, k;
  unsigned char x;
  k = 0;
  for(i=0, k=0x550; i<16; i++, k=k>>1){
    if( k&1 ){
      zStr[0] = '-';
      zStr++;
    }
    x = aBlob[i];
    zStr[0] = zDigits[x>>4];
    zStr[1] = zDigits[x&0xf];
    zStr += 2;
  }
  *zStr = 0;
}

/*
** Attempt to parse a zero-terminated input string zStr into a binary
** UUID.  Return 0 on success, or non-zero if the input string is not
** parsable.
*/
static int capdbUuidStrToBlob(
  const unsigned char *zStr,   /* Input string */
  unsigned char *aBlob         /* Write results here */
){
  int i;
  if( zStr==0 ) return 1;
  if( zStr[0]=='{' ) zStr++;
  for(i=0; i<16; i++){
    if( zStr[0]=='-' ) zStr++;
    if( isxdigit(zStr[0]) && isxdigit(zStr[1]) ){
      aBlob[i] = (capdbUuidHexToInt(zStr[0])<<4)
                      + capdbUuidHexToInt(zStr[1]);
      zStr += 2;
    }else{
      return 1;
    }
  }
  if( zStr[0]=='}' ) zStr++;
  return zStr[0]!=0;
}

/*
** Render capdb_value pIn as a 16-byte UUID blob.  Return a pointer
** to the blob, or NULL if the input is not well-formed.
*/
static const unsigned char *capdbUuidInputToBlob(
  capdb_value *pIn,     /* Input text */
  unsigned char *pBuf     /* output buffer */
){
  switch( capdb_value_type(pIn) ){
    case CAPDB_TEXT: {
      const unsigned char *z = capdb_value_text(pIn);
      if( capdbUuidStrToBlob(z, pBuf) ) return 0;
      return pBuf;
    }
    case CAPDB_BLOB: {
      int n = capdb_value_bytes(pIn);
      return n==16 ? capdb_value_blob(pIn) : 0;
    }
    default: {
      return 0;
    }
  }
}

/* Implementation of uuid() */
static void capdbUuidFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  unsigned char aBlob[16];
  unsigned char zStr[37];
  (void)argc;
  (void)argv;
  capdb_randomness(16, aBlob);
  aBlob[6] = (aBlob[6]&0x0f) + 0x40;
  aBlob[8] = (aBlob[8]&0x3f) + 0x80;
  capdbUuidBlobToStr(aBlob, zStr);
  capdb_result_text(context, (char*)zStr, 36, CAPDB_TRANSIENT);
}

/* Implementation of uuid_str() */
static void capdbUuidStrFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  unsigned char aBlob[16];
  unsigned char zStr[37];
  const unsigned char *pBlob;
  (void)argc;
  pBlob = capdbUuidInputToBlob(argv[0], aBlob);
  if( pBlob==0 ) return;
  capdbUuidBlobToStr(pBlob, zStr);
  capdb_result_text(context, (char*)zStr, 36, CAPDB_TRANSIENT);
}

/* Implementation of uuid_blob() */
static void capdbUuidBlobFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  unsigned char aBlob[16];
  const unsigned char *pBlob;
  (void)argc;
  pBlob = capdbUuidInputToBlob(argv[0], aBlob);
  if( pBlob==0 ) return;
  capdb_result_blob(context, pBlob, 16, CAPDB_TRANSIENT);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_uuid_init(
  capdb *db,
  char **pzErrMsg,
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "uuid", 0, CAPDB_UTF8|CAPDB_INNOCUOUS, 0,
                               capdbUuidFunc, 0, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "uuid_str", 1, 
                       CAPDB_UTF8|CAPDB_INNOCUOUS|CAPDB_DETERMINISTIC,
                       0, capdbUuidStrFunc, 0, 0);
  }
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "uuid_blob", 1,
                       CAPDB_UTF8|CAPDB_INNOCUOUS|CAPDB_DETERMINISTIC,
                       0, capdbUuidBlobFunc, 0, 0);
  }
  return rc;
}
