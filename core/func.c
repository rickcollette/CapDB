/*
** 2002 February 23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C-language implementations for many of the SQL
** functions of SQLite.  (Some function, and in particular the date and
** time functions, are implemented separately.)
*/
#include "capdbInt.h"
#include <stdlib.h>
#include <assert.h>
#ifndef CAPDB_OMIT_FLOATING_POINT
#include <math.h>
#endif
#include "vdbeInt.h"

/*
** Return the collating function associated with a function.
*/
static CollSeq *capdbGetFuncCollSeq(capdb_context *context){
  VdbeOp *pOp;
  assert( context->pVdbe!=0 );
  pOp = &context->pVdbe->aOp[context->iOp-1];
  assert( pOp->opcode==OP_CollSeq );
  assert( pOp->p4type==P4_COLLSEQ );
  return pOp->p4.pColl;
}

/*
** Indicate that the accumulator load should be skipped on this
** iteration of the aggregate loop.
*/
static void capdbSkipAccumulatorLoad(capdb_context *context){
  assert( context->isError<=0 );
  context->isError = -1;
  context->skipFlag = 1;
}

/*
** Implementation of the non-aggregate min() and max() functions
*/
static void minmaxFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int i;
  int mask;    /* 0 for min() or 0xffffffff for max() */
  int iBest;
  CollSeq *pColl;

  assert( argc>1 );
  mask = capdb_user_data(context)==0 ? 0 : -1;
  pColl = capdbGetFuncCollSeq(context);
  assert( pColl );
  assert( mask==-1 || mask==0 );
  iBest = 0;
  if( capdb_value_type(argv[0])==CAPDB_NULL ) return;
  for(i=1; i<argc; i++){
    if( capdb_value_type(argv[i])==CAPDB_NULL ) return;
    if( (capdbMemCompare(argv[iBest], argv[i], pColl)^mask)>=0 ){
      testcase( mask==0 );
      iBest = i;
    }
  }
  capdb_result_value(context, argv[iBest]);
}

/*
** Return the type of the argument.
*/
static void typeofFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **argv
){
  static const char *azType[] = { "integer", "real", "text", "blob", "null" };
  int i = capdb_value_type(argv[0]) - 1;
  UNUSED_PARAMETER(NotUsed);
  assert( i>=0 && i<ArraySize(azType) );
  assert( CAPDB_INTEGER==1 );
  assert( CAPDB_FLOAT==2 );
  assert( CAPDB_TEXT==3 );
  assert( CAPDB_BLOB==4 );
  assert( CAPDB_NULL==5 );
  /* EVIDENCE-OF: R-01470-60482 The capdb_value_type(V) interface returns
  ** the datatype code for the initial datatype of the capdb_value object
  ** V. The returned value is one of CAPDB_INTEGER, CAPDB_FLOAT,
  ** CAPDB_TEXT, CAPDB_BLOB, or CAPDB_NULL. */
  capdb_result_text(context, azType[i], -1, CAPDB_STATIC);
}

/* subtype(X)
**
** Return the subtype of X
*/
static void subtypeFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  UNUSED_PARAMETER(argc);
  capdb_result_int(context, capdb_value_subtype(argv[0]));
}

/*
** Implementation of the length() function
*/
static void lengthFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( capdb_value_type(argv[0]) ){
    case CAPDB_BLOB:
    case CAPDB_INTEGER:
    case CAPDB_FLOAT: {
      capdb_result_int(context, capdb_value_bytes(argv[0]));
      break;
    }
    case CAPDB_TEXT: {
      const unsigned char *z = capdb_value_text(argv[0]);
      const unsigned char *z0;
      if( z==0 ) return;
      z0 = z;
      while( 1 /*exit-by-break*/ ){
                      /*  vvvvvv----  See tag-20260418-01 */
        if( (u8)(z[0]-1)<(0x80-1) ){
          z++;
        }else if( z[0]==0 ){
          break;
        }else{
          z++;
          while( (z[0]&0xc0)==0x80 ){ z++; z0++; }
        }
      }
      capdb_result_int(context, (int)(z-z0));
      break;
    }
    default: {
      capdb_result_null(context);
      break;
    }
  }
}

/*
** Implementation of the octet_length() function
*/
static void bytelengthFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( capdb_value_type(argv[0]) ){
    case CAPDB_BLOB: {
      capdb_result_int(context, capdb_value_bytes(argv[0]));
      break;
    }
    case CAPDB_INTEGER:
    case CAPDB_FLOAT: {
      i64 m = capdb_context_db_handle(context)->enc<=CAPDB_UTF8 ? 1 : 2;
      capdb_result_int64(context, capdb_value_bytes(argv[0])*m);
      break;
    }
    case CAPDB_TEXT: {
      if( capdb_value_encoding(argv[0])<=CAPDB_UTF8 ){
        capdb_result_int(context, capdb_value_bytes(argv[0]));
      }else{
        capdb_result_int(context, capdb_value_bytes16(argv[0]));
      }
      break;
    }
    default: {
      capdb_result_null(context);
      break;
    }
  }
}

/*
** Implementation of the abs() function.
**
** IMP: R-23979-26855 The abs(X) function returns the absolute value of
** the numeric argument X.
*/
static void absFunc(capdb_context *context, int argc, capdb_value **argv){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( capdb_value_type(argv[0]) ){
    case CAPDB_INTEGER: {
      i64 iVal = capdb_value_int64(argv[0]);
      if( iVal<0 ){
        if( iVal==SMALLEST_INT64 ){
          /* IMP: R-31676-45509 If X is the integer -9223372036854775808
          ** then abs(X) throws an integer overflow error since there is no
          ** equivalent positive 64-bit two complement value. */
          capdb_result_error(context, "integer overflow", -1);
          return;
        }
        iVal = -iVal;
      }
      capdb_result_int64(context, iVal);
      break;
    }
    case CAPDB_NULL: {
      /* IMP: R-37434-19929 Abs(X) returns NULL if X is NULL. */
      capdb_result_null(context);
      break;
    }
    default: {
      /* Because capdb_value_double() returns 0.0 if the argument is not
      ** something that can be converted into a number, we have:
      ** IMP: R-01992-00519 Abs(X) returns 0.0 if X is a string or blob
      ** that cannot be converted to a numeric value.
      */
      double rVal = capdb_value_double(argv[0]);
      if( rVal<0 ) rVal = -rVal;
      capdb_result_double(context, rVal);
      break;
    }
  }
}

/*
** Implementation of the instr() function.
**
** instr(haystack,needle) finds the first occurrence of needle
** in haystack and returns the number of previous characters plus 1,
** or 0 if needle does not occur within haystack.
**
** If both haystack and needle are BLOBs, then the result is one more than
** the number of bytes in haystack prior to the first occurrence of needle,
** or 0 if needle never occurs in haystack.
*/
static void instrFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *zHaystack;
  const unsigned char *zNeedle;
  int nHaystack;
  int nNeedle;
  int typeHaystack, typeNeedle;
  int N = 1;
  int isText;
  unsigned char firstChar;
  capdb_value *pC1 = 0;
  capdb_value *pC2 = 0;

  UNUSED_PARAMETER(argc);
  typeHaystack = capdb_value_type(argv[0]);
  typeNeedle = capdb_value_type(argv[1]);
  if( typeHaystack==CAPDB_NULL || typeNeedle==CAPDB_NULL ) return;
  nHaystack = capdb_value_bytes(argv[0]);
  nNeedle = capdb_value_bytes(argv[1]);
  if( nNeedle>0 ){
    if( typeHaystack==CAPDB_BLOB && typeNeedle==CAPDB_BLOB ){
      zHaystack = capdb_value_blob(argv[0]);
      zNeedle = capdb_value_blob(argv[1]);
      isText = 0;
    }else if( typeHaystack!=CAPDB_BLOB && typeNeedle!=CAPDB_BLOB ){
      zHaystack = capdb_value_text(argv[0]);
      zNeedle = capdb_value_text(argv[1]);
      isText = 1;
    }else{
      pC1 = capdb_value_dup(argv[0]);
      zHaystack = capdb_value_text(pC1);
      if( zHaystack==0 ) goto endInstrOOM;
      nHaystack = capdb_value_bytes(pC1);
      pC2 = capdb_value_dup(argv[1]);
      zNeedle = capdb_value_text(pC2);
      if( zNeedle==0 ) goto endInstrOOM;
      nNeedle = capdb_value_bytes(pC2);
      isText = 1;
    }
    if( zNeedle==0 || (nHaystack && zHaystack==0) ) goto endInstrOOM;
    firstChar = zNeedle[0];
    while( nNeedle<=nHaystack
       && (zHaystack[0]!=firstChar || memcmp(zHaystack, zNeedle, nNeedle)!=0)
    ){
      N++;
      do{
        nHaystack--;
        zHaystack++;
      }while( isText && (zHaystack[0]&0xc0)==0x80 );
    }
    if( nNeedle>nHaystack ) N = 0;
  }
  capdb_result_int(context, N);
endInstr:
  capdb_value_free(pC1);
  capdb_value_free(pC2);
  return;
endInstrOOM:
  capdb_result_error_nomem(context);
  goto endInstr;
}

/*
** Implementation of the printf() (a.k.a. format()) SQL function.
*/
static void printfFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  PrintfArguments x;
  StrAccum str;
  const char *zFormat;
  int n;
  capdb *db = capdb_context_db_handle(context);

  if( argc>=1 && (zFormat = (const char*)capdb_value_text(argv[0]))!=0 ){
    x.nArg = argc-1;
    x.nUsed = 0;
    x.apArg = argv+1;
    capdbStrAccumInit(&str, db, 0, 0, db->aLimit[CAPDB_LIMIT_LENGTH]);
    str.printfFlags = CAPDB_PRINTF_SQLFUNC;
    capdb_str_appendf(&str, zFormat, &x);
    if( str.accError==CAPDB_OK ){
      n = str.nChar;
      capdb_result_text(context, capdbStrAccumFinish(&str), n,
                          CAPDB_DYNAMIC);
    }else{
      if( str.accError==CAPDB_NOMEM ){
        capdb_result_error_nomem(context);
      }else{
        capdb_result_error_toobig(context);
      }
      capdb_str_reset(&str);
    }
  }
}

/*
** Implementation of the substr() function.
**
** substr(x,p1,p2)  returns p2 characters of x[] beginning with p1.
** p1 is 1-indexed.  So substr(x,1,1) returns the first character
** of x.  If x is text, then we actually count UTF-8 characters.
** If x is a blob, then we count bytes.
**
** If p1 is negative, then we begin abs(p1) from the end of x[].
**
** If p2 is negative, return the p2 characters preceding p1.
*/
static void substrFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *z;
  const unsigned char *z2;
  int len;
  int p0type;
  i64 p1, p2;

  assert( argc==3 || argc==2 );
  p0type = capdb_value_type(argv[0]);
  p1 = capdb_value_int64(argv[1]);
  if( p0type==CAPDB_BLOB ){
    len = capdb_value_bytes(argv[0]);
    z = capdb_value_blob(argv[0]);
    if( z==0 ) return;
    assert( len==capdb_value_bytes(argv[0]) );
  }else{
    z = capdb_value_text(argv[0]);
    if( z==0 ) return;
    len = 0;
    if( p1<0 ){
      for(z2=z; *z2; len++){
        CAPDB_SKIP_UTF8(z2);
      }
    }
  }
  if( argc==3 ){
    p2 = capdb_value_int64(argv[2]);
    if( p2==0 && capdb_value_type(argv[2])==CAPDB_NULL ) return;
  }else{
    p2 = capdb_context_db_handle(context)->aLimit[CAPDB_LIMIT_LENGTH];
  }
  if( p1==0 ){
#ifdef CAPDB_SUBSTR_COMPATIBILITY
    /* If SUBSTR_COMPATIBILITY is defined then substr(X,0,N) work the same as
    ** as substr(X,1,N) - it returns the first N characters of X.  This
    ** is essentially a back-out of the bug-fix in check-in [5fc125d362df4b8]
    ** from 2009-02-02 for compatibility of applications that exploited the
    ** old buggy behavior. */
    p1 = 1; /* <rdar://problem/6778339> */
#endif
    if( capdb_value_type(argv[1])==CAPDB_NULL ) return;
  }
  if( p1<0 ){
    p1 += len;
    if( p1<0 ){
      if( p2<0 ){
        p2 = 0;
      }else{
        p2 += p1;
      }
      p1 = 0;
    }
  }else if( p1>0 ){
    p1--;
  }else if( p2>0 ){
    p2--;
  }
  if( p2<0 ){
    if( p2<-p1 ){
      p2 = p1;
    }else{
      p2 = -p2;
    }
    p1 -= p2;
  }
  assert( p1>=0 && p2>=0 );
  if( p0type!=CAPDB_BLOB ){
    for( ; p1>0; p1--){
                    /*  vvvvvv----  See tag-20260418-01 */
      if( (u8)(z[0]-1)<(0x80-1) ){
        z++;
      }else if( z[0]==0 ){
        break;
      }else{
        do{ z++; }while( (z[0]&0xc0)==0x80 );
      }
    }
    for(z2=z; p2>0; p2--){
                     /*  vvvvvv----  See tag-20260418-01 */
      if( (u8)(z2[0]-1)<(0x80-1) ){
        z2++;
      }else if( z2[0]==0 ){
        break;
      }else{
        do{ z2++; }while( (z2[0]&0xc0)==0x80 );
      }
    }
    capdb_result_text64(context, (char*)z, z2-z, CAPDB_TRANSIENT,
                          CAPDB_UTF8);
  }else{
    if( p1>=len ){
      p1 = p2 = 0;
    }else if( p2>len-p1 ){
      p2 = len-p1;
      assert( p2>0 );
    }
    capdb_result_blob64(context, (char*)&z[p1], (u64)p2, CAPDB_TRANSIENT);
  }
}

/*
** Implementation of the round() function
*/
#ifndef CAPDB_OMIT_FLOATING_POINT
static void roundFunc(capdb_context *context, int argc, capdb_value **argv){
  i64 n = 0;
  double r;
  char *zBuf;
  assert( argc==1 || argc==2 );
  if( argc==2 ){
    if( CAPDB_NULL==capdb_value_type(argv[1]) ) return;
    n = capdb_value_int64(argv[1]);
    if( n>30 ) n = 30;
    if( n<0 ) n = 0;
  }
  if( capdb_value_type(argv[0])==CAPDB_NULL ) return;
  r = capdb_value_double(argv[0]);
  /* If Y==0 and X will fit in a 64-bit int,
  ** handle the rounding directly,
  ** otherwise use printf.
  */
  if( r<-4503599627370496.0 || r>+4503599627370496.0 ){
    /* The value has no fractional part so there is nothing to round */
  }else if( n==0 ){ 
    r = (double)((sqlite_int64)(r+(r<0?-0.5:+0.5)));
  }else{
    zBuf = capdb_mprintf("%!.*f",(int)n,r);
    if( zBuf==0 ){
      capdb_result_error_nomem(context);
      return;
    }
    capdbAtoF(zBuf, &r);
    capdb_free(zBuf);
  }
  capdb_result_double(context, r);
}
#endif

/*
** Allocate nByte bytes of space using capdbMalloc(). If the
** allocation fails, call capdb_result_error_nomem() to notify
** the database handle that malloc() has failed and return NULL.
** If nByte is larger than the maximum string or blob length, then
** raise an CAPDB_TOOBIG exception and return NULL.
*/
static void *contextMalloc(capdb_context *context, i64 nByte){
  char *z;
  capdb *db = capdb_context_db_handle(context);
  assert( nByte>0 );
  testcase( nByte==db->aLimit[CAPDB_LIMIT_LENGTH] );
  testcase( nByte==(i64)db->aLimit[CAPDB_LIMIT_LENGTH]+1 );
  if( nByte>db->aLimit[CAPDB_LIMIT_LENGTH] ){
    capdb_result_error_toobig(context);
    z = 0;
  }else{
    z = capdbMalloc(nByte);
    if( !z ){
      capdb_result_error_nomem(context);
    }
  }
  return z;
}

/*
** Implementation of the upper() and lower() SQL functions.
*/
static void upperFunc(capdb_context *context, int argc, capdb_value **argv){
  char *z1;
  const char *z2;
  int i, n;
  UNUSED_PARAMETER(argc);
  z2 = (char*)capdb_value_text(argv[0]);
  n = capdb_value_bytes(argv[0]);
  /* Verify that the call to _bytes() does not invalidate the _text() pointer */
  assert( z2==(char*)capdb_value_text(argv[0]) );
  if( z2 ){
    z1 = contextMalloc(context, ((i64)n)+1);
    if( z1 ){
      for(i=0; i<n; i++){
        z1[i] = (char)capdbToupper(z2[i]);
      }
      capdb_result_text(context, z1, n, capdb_free);
    }
  }
}
static void lowerFunc(capdb_context *context, int argc, capdb_value **argv){
  char *z1;
  const char *z2;
  int i, n;
  UNUSED_PARAMETER(argc);
  z2 = (char*)capdb_value_text(argv[0]);
  n = capdb_value_bytes(argv[0]);
  /* Verify that the call to _bytes() does not invalidate the _text() pointer */
  assert( z2==(char*)capdb_value_text(argv[0]) );
  if( z2 ){
    z1 = contextMalloc(context, ((i64)n)+1);
    if( z1 ){
      for(i=0; i<n; i++){
        z1[i] = capdbTolower(z2[i]);
      }
      capdb_result_text(context, z1, n, capdb_free);
    }
  }
}

/*
** Some functions like COALESCE() and IFNULL() and UNLIKELY() are implemented
** as VDBE code so that unused argument values do not have to be computed.
** However, we still need some kind of function implementation for this
** routines in the function table.  The noopFunc macro provides this.
** noopFunc will never be called so it doesn't matter what the implementation
** is.  We might as well use the "version()" function as a substitute.
*/
#define noopFunc versionFunc   /* Substitute function - never called */

/*
** Implementation of random().  Return a random integer. 
*/
static void randomFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **NotUsed2
){
  sqlite_int64 r;
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  capdb_randomness(sizeof(r), &r);
  if( r<0 ){
    /* We need to prevent a random number of 0x8000000000000000
    ** (or -9223372036854775808) since when you do abs() of that
    ** number of you get the same value back again.  To do this
    ** in a way that is testable, mask the sign bit off of negative
    ** values, resulting in a positive value.  Then take the
    ** 2s complement of that positive value.  The end result can
    ** therefore be no less than -9223372036854775807.
    */
    r = -(r & LARGEST_INT64);
  }
  capdb_result_int64(context, r);
}

/*
** Implementation of randomblob(N).  Return a random blob
** that is N bytes long.
*/
static void randomBlob(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_int64 n;
  unsigned char *p;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  n = capdb_value_int64(argv[0]);
  if( n<1 ){
    n = 1;
  }
  p = contextMalloc(context, n);
  if( p ){
    capdb_randomness(n, p);
    capdb_result_blob(context, (char*)p, n, capdb_free);
  }
}

/*
** Implementation of the last_insert_rowid() SQL function.  The return
** value is the same as the capdb_last_insert_rowid() API function.
*/
static void last_insert_rowid(
  capdb_context *context,
  int NotUsed,
  capdb_value **NotUsed2
){
  capdb *db = capdb_context_db_handle(context);
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  /* IMP: R-51513-12026 The last_insert_rowid() SQL function is a
  ** wrapper around the capdb_last_insert_rowid() C/C++ interface
  ** function. */
  capdb_result_int64(context, capdb_last_insert_rowid(db));
}

/*
** Implementation of the changes() SQL function.
**
** IMP: R-32760-32347 The changes() SQL function is a wrapper
** around the capdb_changes64() C/C++ function and hence follows the
** same rules for counting changes.
*/
static void changes(
  capdb_context *context,
  int NotUsed,
  capdb_value **NotUsed2
){
  capdb *db = capdb_context_db_handle(context);
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  capdb_result_int64(context, capdb_changes64(db));
}

/*
** Implementation of the total_changes() SQL function.  The return value is
** the same as the capdb_total_changes64() API function.
*/
static void total_changes(
  capdb_context *context,
  int NotUsed,
  capdb_value **NotUsed2
){
  capdb *db = capdb_context_db_handle(context);
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  /* IMP: R-11217-42568 This function is a wrapper around the
  ** capdb_total_changes64() C/C++ interface. */
  capdb_result_int64(context, capdb_total_changes64(db));
}

/*
** A structure defining how to do GLOB-style comparisons.
*/
struct compareInfo {
  u8 matchAll;          /* "*" or "%" */
  u8 matchOne;          /* "?" or "_" */
  u8 matchSet;          /* "[" or 0 */
  u8 noCase;            /* true to ignore case differences */
};

/*
** For LIKE and GLOB matching on EBCDIC machines, assume that every
** character is exactly one byte in size.  Also, provide the Utf8Read()
** macro for fast reading of the next character in the common case where
** the next character is ASCII.
*/
#if defined(CAPDB_EBCDIC)
# define capdbUtf8Read(A)        (*((*A)++))
# define Utf8Read(A)               (*(A++))
#else
# define Utf8Read(A)               (A[0]<0x80?*(A++):capdbUtf8Read(&A))
#endif

static const struct compareInfo globInfo = { '*', '?', '[', 0 };
/* The correct SQL-92 behavior is for the LIKE operator to ignore
** case.  Thus  'a' LIKE 'A' would be true. */
static const struct compareInfo likeInfoNorm = { '%', '_',   0, 1 };
/* If CAPDB_CASE_SENSITIVE_LIKE is defined, then the LIKE operator
** is case sensitive causing 'a' LIKE 'A' to be false */
static const struct compareInfo likeInfoAlt = { '%', '_',   0, 0 };

/*
** Possible error returns from patternMatch()
*/
#define CAPDB_MATCH             0
#define CAPDB_NOMATCH           1
#define CAPDB_NOWILDCARDMATCH   2

/*
** Compare two UTF-8 strings for equality where the first string is
** a GLOB or LIKE expression.  Return values:
**
**    CAPDB_MATCH:            Match
**    CAPDB_NOMATCH:          No match
**    CAPDB_NOWILDCARDMATCH:  No match in spite of having * or % wildcards.
**
** Globbing rules:
**
**      '*'       Matches any sequence of zero or more characters.
**
**      '?'       Matches exactly one character.
**
**     [...]      Matches one character from the enclosed list of
**                characters.
**
**     [^...]     Matches one character not in the enclosed list.
**
** With the [...] and [^...] matching, a ']' character can be included
** in the list by making it the first character after '[' or '^'.  A
** range of characters can be specified using '-'.  Example:
** "[a-z]" matches any single lower-case letter.  To match a '-', make
** it the last character in the list.
**
** Like matching rules:
**
**      '%'       Matches any sequence of zero or more characters
**
***     '_'       Matches any one character
**
**      Ec        Where E is the "esc" character and c is any other
**                character, including '%', '_', and esc, match exactly c.
**
** The comments within this routine usually assume glob matching.
**
** This routine is usually quick, but can be N**2 in the worst case.
*/
static int patternCompare(
  const u8 *zPattern,              /* The glob pattern */
  const u8 *zString,               /* The string to compare against the glob */
  const struct compareInfo *pInfo, /* Information about how to do the compare */
  u32 matchOther                   /* The escape char (LIKE) or '[' (GLOB) */
){
  u32 c, c2;                       /* Next pattern and input string chars */
  u32 matchOne = pInfo->matchOne;  /* "?" or "_" */
  u32 matchAll = pInfo->matchAll;  /* "*" or "%" */
  u8 noCase = pInfo->noCase;       /* True if uppercase==lowercase */
  const u8 *zEscaped = 0;          /* One past the last escaped input char */
 
  while( (c = Utf8Read(zPattern))!=0 ){
    if( c==matchAll ){  /* Match "*" */
      /* Skip over multiple "*" characters in the pattern.  If there
      ** are also "?" characters, skip those as well, but consume a
      ** single character of the input string for each "?" skipped */
      while( (c=Utf8Read(zPattern)) == matchAll
             || (c == matchOne && matchOne!=0) ){
        if( c==matchOne && capdbUtf8Read(&zString)==0 ){
          return CAPDB_NOWILDCARDMATCH;
        }
      }
      if( c==0 ){
        return CAPDB_MATCH;   /* "*" at the end of the pattern matches */
      }else if( c==matchOther ){
        if( pInfo->matchSet==0 ){
          c = capdbUtf8Read(&zPattern);
          if( c==0 ) return CAPDB_NOWILDCARDMATCH;
        }else{
          /* "[...]" immediately follows the "*".  We have to do a slow
          ** recursive search in this case, but it is an unusual case. */
          assert( matchOther<0x80 );  /* '[' is a single-byte character */
          while( *zString ){
            int bMatch = patternCompare(&zPattern[-1],zString,pInfo,matchOther);
            if( bMatch!=CAPDB_NOMATCH ) return bMatch;
            CAPDB_SKIP_UTF8(zString);
          }
          return CAPDB_NOWILDCARDMATCH;
        }
      }

      /* At this point variable c contains the first character of the
      ** pattern string past the "*".  Search in the input string for the
      ** first matching character and recursively continue the match from
      ** that point.
      **
      ** For a case-insensitive search, set variable cx to be the same as
      ** c but in the other case and search the input string for either
      ** c or cx.
      */
      if( c<0x80 ){
        char zStop[3];
        int bMatch;
        if( noCase ){
          zStop[0] = capdbToupper(c);
          zStop[1] = capdbTolower(c);
          zStop[2] = 0;
        }else{
          zStop[0] = c;
          zStop[1] = 0;
        }
        while(1){
          zString += strcspn((const char*)zString, zStop);
          if( zString[0]==0 ) break;
          zString++;
          bMatch = patternCompare(zPattern,zString,pInfo,matchOther);
          if( bMatch!=CAPDB_NOMATCH ) return bMatch;
        }
      }else{
        int bMatch;
        while( (c2 = Utf8Read(zString))!=0 ){
          if( c2!=c ) continue;
          bMatch = patternCompare(zPattern,zString,pInfo,matchOther);
          if( bMatch!=CAPDB_NOMATCH ) return bMatch;
        }
      }
      return CAPDB_NOWILDCARDMATCH;
    }
    if( c==matchOther ){
      if( pInfo->matchSet==0 ){
        c = capdbUtf8Read(&zPattern);
        if( c==0 ) return CAPDB_NOMATCH;
        zEscaped = zPattern;
      }else{
        u32 prior_c = 0;
        int seen = 0;
        int invert = 0;
        c = capdbUtf8Read(&zString);
        if( c==0 ) return CAPDB_NOMATCH;
        c2 = capdbUtf8Read(&zPattern);
        if( c2=='^' ){
          invert = 1;
          c2 = capdbUtf8Read(&zPattern);
        }
        if( c2==']' ){
          if( c==']' ) seen = 1;
          c2 = capdbUtf8Read(&zPattern);
        }
        while( c2 && c2!=']' ){
          if( c2=='-' && zPattern[0]!=']' && zPattern[0]!=0 && prior_c>0 ){
            c2 = capdbUtf8Read(&zPattern);
            if( c>=prior_c && c<=c2 ) seen = 1;
            prior_c = 0;
          }else{
            if( c==c2 ){
              seen = 1;
            }
            prior_c = c2;
          }
          c2 = capdbUtf8Read(&zPattern);
        }
        if( c2==0 || (seen ^ invert)==0 ){
          return CAPDB_NOMATCH;
        }
        continue;
      }
    }
    c2 = Utf8Read(zString);
    if( c==c2 ) continue;
    if( noCase  && capdbTolower(c)==capdbTolower(c2) && c<0x80 && c2<0x80 ){
      continue;
    }
    if( c==matchOne && zPattern!=zEscaped && c2!=0 ) continue;
    return CAPDB_NOMATCH;
  }
  return *zString==0 ? CAPDB_MATCH : CAPDB_NOMATCH;
}

/*
** The capdb_strglob() interface.  Return 0 on a match (like strcmp()) and
** non-zero if there is no match.
*/
int capdb_strglob(const char *zGlobPattern, const char *zString){
  if( zString==0 ){
    return zGlobPattern!=0;
  }else if( zGlobPattern==0 ){
    return 1;
  }else {
    return patternCompare((u8*)zGlobPattern, (u8*)zString, &globInfo, '[');
  }
}

/*
** The capdb_strlike() interface.  Return 0 on a match and non-zero for
** a miss - like strcmp().
*/
int capdb_strlike(const char *zPattern, const char *zStr, unsigned int esc){
  if( zStr==0 ){
    return zPattern!=0;
  }else if( zPattern==0 ){
    return 1;
  }else{
    return patternCompare((u8*)zPattern, (u8*)zStr, &likeInfoNorm, esc);
  }
}

/*
** Count the number of times that the LIKE operator (or GLOB which is
** just a variation of LIKE) gets called.  This is used for testing
** only.
*/
#ifdef CAPDB_TEST
int capdb_like_count = 0;
#endif


/*
** Implementation of the like() SQL function.  This function implements
** the built-in LIKE operator.  The first argument to the function is the
** pattern and the second argument is the string.  So, the SQL statements:
**
**       A LIKE B
**
** is implemented as like(B,A).
**
** This same function (with a different compareInfo structure) computes
** the GLOB operator.
*/
static void likeFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *zA, *zB;
  u32 escape;
  int nPat;
  capdb *db = capdb_context_db_handle(context);
  struct compareInfo *pInfo = capdb_user_data(context);
  struct compareInfo backupInfo;

#ifdef CAPDB_LIKE_DOESNT_MATCH_BLOBS
  if( capdb_value_type(argv[0])==CAPDB_BLOB
   || capdb_value_type(argv[1])==CAPDB_BLOB
  ){
#ifdef CAPDB_TEST
    capdb_like_count++;
#endif
    capdb_result_int(context, 0);
    return;
  }
#endif

  /* Limit the length of the LIKE or GLOB pattern to avoid problems
  ** of deep recursion and N*N behavior in patternCompare().
  */
  nPat = capdb_value_bytes(argv[0]);
  testcase( nPat==db->aLimit[CAPDB_LIMIT_LIKE_PATTERN_LENGTH] );
  testcase( nPat==db->aLimit[CAPDB_LIMIT_LIKE_PATTERN_LENGTH]+1 );
  if( nPat > db->aLimit[CAPDB_LIMIT_LIKE_PATTERN_LENGTH] ){
    capdb_result_error(context, "LIKE or GLOB pattern too complex", -1);
    return;
  }
  if( argc==3 ){
    /* The escape character string must consist of a single UTF-8 character.
    ** Otherwise, return an error.
    */
    const unsigned char *zEsc = capdb_value_text(argv[2]);
    if( zEsc==0 ) return;
    if( capdbUtf8CharLen((char*)zEsc, -1)!=1 ){
      capdb_result_error(context,
          "ESCAPE expression must be a single character", -1);
      return;
    }
    escape = capdbUtf8Read(&zEsc);
    if( escape==pInfo->matchAll || escape==pInfo->matchOne ){
      memcpy(&backupInfo, pInfo, sizeof(backupInfo));
      pInfo = &backupInfo;
      if( escape==pInfo->matchAll ) pInfo->matchAll = 0;
      if( escape==pInfo->matchOne ) pInfo->matchOne = 0;
    }
  }else{
    escape = pInfo->matchSet;
  }
  zB = capdb_value_text(argv[0]);
  zA = capdb_value_text(argv[1]);
  if( zA && zB ){
#ifdef CAPDB_TEST
    capdb_like_count++;
#endif
    capdb_result_int(context,
                      patternCompare(zB, zA, pInfo, escape)==CAPDB_MATCH);
  }
}

/*
** Implementation of the NULLIF(x,y) function.  The result is the first
** argument if the arguments are different.  The result is NULL if the
** arguments are equal to each other.
*/
static void nullifFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **argv
){
  CollSeq *pColl = capdbGetFuncCollSeq(context);
  UNUSED_PARAMETER(NotUsed);
  if( capdbMemCompare(argv[0], argv[1], pColl)!=0 ){
    capdb_result_value(context, argv[0]);
  }
}

/*
** Implementation of the sqlite_version() function.  The result is the version
** of the SQLite library that is running.
*/
static void versionFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **NotUsed2
){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  /* IMP: R-48699-48617 This function is an SQL wrapper around the
  ** capdb_libversion() C-interface. */
  capdb_result_text(context, capdb_libversion(), -1, CAPDB_STATIC);
}

/*
** Implementation of the sqlite_source_id() function. The result is a string
** that identifies the particular version of the source code used to build
** SQLite.
*/
static void sourceidFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **NotUsed2
){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  /* IMP: R-24470-31136 This function is an SQL wrapper around the
  ** capdb_sourceid() C interface. */
  capdb_result_text(context, capdb_sourceid(), -1, CAPDB_STATIC);
}

/*
** Implementation of the sqlite_log() function.  This is a wrapper around
** capdb_log().  The return value is NULL.  The function exists purely for
** its side-effects.
*/
static void errlogFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(context);
  capdb_log(capdb_value_int(argv[0]), "%s", capdb_value_text(argv[1]));
}

/*
** Implementation of the sqlite_compileoption_used() function.
** The result is an integer that identifies if the compiler option
** was used to build SQLite.
*/
#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
static void compileoptionusedFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zOptName;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  /* IMP: R-39564-36305 The sqlite_compileoption_used() SQL
  ** function is a wrapper around the capdb_compileoption_used() C/C++
  ** function.
  */
  if( (zOptName = (const char*)capdb_value_text(argv[0]))!=0 ){
    capdb_result_int(context, capdb_compileoption_used(zOptName));
  }
}
#endif /* CAPDB_OMIT_COMPILEOPTION_DIAGS */

/*
** Implementation of the sqlite_compileoption_get() function.
** The result is a string that identifies the compiler options
** used to build SQLite.
*/
#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
static void compileoptiongetFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int n;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  /* IMP: R-04922-24076 The sqlite_compileoption_get() SQL function
  ** is a wrapper around the capdb_compileoption_get() C/C++ function.
  */
  n = capdb_value_int(argv[0]);
  capdb_result_text(context, capdb_compileoption_get(n), -1, CAPDB_STATIC);
}
#endif /* CAPDB_OMIT_COMPILEOPTION_DIAGS */

/* Array for converting from half-bytes (nybbles) into ASCII hex
** digits. */
static const char hexdigits[] = {
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

/*
** Append to pStr text that is the SQL literal representation of the
** value contained in pValue.
*/
void capdbQuoteValue(StrAccum *pStr, capdb_value *pValue, int bEscape){
  /* As currently implemented, the string must be initially empty.
  ** we might relax this requirement in the future, but that will
  ** require enhancements to the implementation. */
  assert( pStr!=0 && pStr->nChar==0 );

  switch( capdb_value_type(pValue) ){
    case CAPDB_FLOAT: {
                             /*    ,---  Show infinity as 9.0e+999
                             **    |   
                             **    | ,--- 17 precision guarantees round-trip
                             **    v v                                       */
      capdb_str_appendf(pStr, "%!0.17g", capdb_value_double(pValue));
      break;
    }
    case CAPDB_INTEGER: {
      capdb_str_appendf(pStr, "%lld", capdb_value_int64(pValue));
      break;
    }
    case CAPDB_BLOB: {
      char const *zBlob = capdb_value_blob(pValue);
      i64 nBlob = capdb_value_bytes(pValue);
      assert( zBlob==capdb_value_blob(pValue) ); /* No encoding change */
      capdbStrAccumEnlarge(pStr, nBlob*2 + 4);
      if( pStr->accError==0 ){
        char *zText = pStr->zText;
        int i;
        for(i=0; i<nBlob; i++){
          zText[(i*2)+2] = hexdigits[(zBlob[i]>>4)&0x0F];
          zText[(i*2)+3] = hexdigits[(zBlob[i])&0x0F];
        }
        zText[(nBlob*2)+2] = '\'';
        zText[(nBlob*2)+3] = '\0';
        zText[0] = 'X';
        zText[1] = '\'';
        pStr->nChar = nBlob*2 + 3;
      }
      break;
    }
    case CAPDB_TEXT: {
      const unsigned char *zArg = capdb_value_text(pValue);
      capdb_str_appendf(pStr, bEscape ? "%#Q" : "%Q", zArg);
      break;
    }
    default: {
      assert( capdb_value_type(pValue)==CAPDB_NULL );
      capdb_str_append(pStr, "NULL", 4);
      break;
    }
  }
}

/*
** Return true if z[] begins with N hexadecimal digits, and write
** a decoding of those digits into *pVal.  Or return false if any
** one of the first N characters in z[] is not a hexadecimal digit.
*/
static int isNHex(const char *z, int N, u32 *pVal){
  int i;
  u32 v = 0;
  for(i=0; i<N; i++){
    if( !capdbIsxdigit(z[i]) ) return 0;
    v = (v<<4) + capdbHexToInt(z[i]);
  }
  *pVal = v;
  return 1;
}

/*
** Implementation of the UNISTR() function.
**
** This is intended to be a work-alike of the UNISTR() function in
** PostgreSQL.  Quoting from the PG documentation (PostgreSQL 17 -
** scraped on 2025-02-22):
**
**    Evaluate escaped Unicode characters in the argument. Unicode
**    characters can be specified as \XXXX (4 hexadecimal digits),
**    \+XXXXXX (6 hexadecimal digits), \uXXXX (4 hexadecimal digits),
**    or \UXXXXXXXX (8 hexadecimal digits). To specify a backslash,
**    write two backslashes. All other characters are taken literally.
*/
static void unistrFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  char *zOut;
  const char *zIn;
  int nIn;
  int i, j, n;
  u32 v;

  assert( argc==1 );
  UNUSED_PARAMETER( argc );
  zIn = (const char*)capdb_value_text(argv[0]);
  if( zIn==0 ) return;
  nIn = capdb_value_bytes(argv[0]);
  zOut = capdb_malloc64(nIn+1);
  if( zOut==0 ){
    capdb_result_error_nomem(context);
    return;
  }
  i = j = 0;
  while( i<nIn ){
    const char *z = strchr(&zIn[i],'\\');
    if( z==0 ){
      n = nIn - i;
      memmove(&zOut[j], &zIn[i], n);
      j += n;
      break;
    }
    n = z - &zIn[i];
    if( n>0 ){
      memmove(&zOut[j], &zIn[i], n);
      j += n;
      i += n;
    }
    if( zIn[i+1]=='\\' ){
      i += 2;
      zOut[j++] = '\\';
    }else if( capdbIsxdigit(zIn[i+1]) ){
      if( !isNHex(&zIn[i+1], 4, &v) ) goto unistr_error;
      i += 5;
      j += capdbAppendOneUtf8Character(&zOut[j], v);
    }else if( zIn[i+1]=='+' ){
      if( !isNHex(&zIn[i+2], 6, &v) ) goto unistr_error;
      i += 8;
      j += capdbAppendOneUtf8Character(&zOut[j], v);
    }else if( zIn[i+1]=='u' ){
      if( !isNHex(&zIn[i+2], 4, &v) ) goto unistr_error;
      i += 6;
      j += capdbAppendOneUtf8Character(&zOut[j], v);
    }else if( zIn[i+1]=='U' ){
      if( !isNHex(&zIn[i+2], 8, &v) ) goto unistr_error;
      i += 10;
      j += capdbAppendOneUtf8Character(&zOut[j], v);
    }else{
      goto unistr_error;
    }
  }
  zOut[j] = 0;
  capdb_result_text64(context, zOut, j, capdb_free, CAPDB_UTF8_ZT);
  return;

unistr_error:
  capdb_free(zOut);
  capdb_result_error(context, "invalid Unicode escape", -1);
  return;
}


/*
** Implementation of the QUOTE() function. 
**
** The quote(X) function returns the text of an SQL literal which is the
** value of its argument suitable for inclusion into an SQL statement.
** Strings are surrounded by single-quotes with escapes on interior quotes
** as needed. BLOBs are encoded as hexadecimal literals. Strings with
** embedded NUL characters cannot be represented as string literals in SQL
** and hence the returned string literal is truncated prior to the first NUL.
**
** If capdb_user_data() is non-zero, then the UNISTR_QUOTE() function is
** implemented instead.  The difference is that UNISTR_QUOTE() uses the
** UNISTR() function to escape control characters.
*/
static void quoteFunc(capdb_context *context, int argc, capdb_value **argv){
  capdb_str str;
  capdb *db = capdb_context_db_handle(context);
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  capdbStrAccumInit(&str, db, 0, 0, db->aLimit[CAPDB_LIMIT_LENGTH]);
  capdbQuoteValue(&str,argv[0],CAPDB_PTR_TO_INT(capdb_user_data(context)));
  capdb_result_text(context, capdbStrAccumFinish(&str), str.nChar,
                      CAPDB_DYNAMIC);
  if( str.accError!=CAPDB_OK ){
    capdb_result_null(context);
    capdb_result_error_code(context, str.accError);
  }
}

/*
** The unicode() function.  Return the integer unicode code-point value
** for the first character of the input string.
*/
static void unicodeFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *z = capdb_value_text(argv[0]);
  (void)argc;
  if( z && z[0] ) capdb_result_int(context, capdbUtf8Read(&z));
}

/*
** The char() function takes zero or more arguments, each of which is
** an integer.  It constructs a string where each character of the string
** is the unicode character for the corresponding integer argument.
*/
static void charFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  unsigned char *z, *zOut;
  int i;
  zOut = z = capdb_malloc64( argc*4+1 );
  if( z==0 ){
    capdb_result_error_nomem(context);
    return;
  }
  for(i=0; i<argc; i++){
    capdb_int64 x;
    unsigned c;
    x = capdb_value_int64(argv[i]);
    if( x<0 || x>0x10ffff ) x = 0xfffd;
    c = (unsigned)(x & 0x1fffff);
    if( c<0x00080 ){
      *zOut++ = (u8)(c&0xFF);
    }else if( c<0x00800 ){
      *zOut++ = 0xC0 + (u8)((c>>6)&0x1F);
      *zOut++ = 0x80 + (u8)(c & 0x3F);
    }else if( c<0x10000 ){
      *zOut++ = 0xE0 + (u8)((c>>12)&0x0F);
      *zOut++ = 0x80 + (u8)((c>>6) & 0x3F);
      *zOut++ = 0x80 + (u8)(c & 0x3F);
    }else{
      *zOut++ = 0xF0 + (u8)((c>>18) & 0x07);
      *zOut++ = 0x80 + (u8)((c>>12) & 0x3F);
      *zOut++ = 0x80 + (u8)((c>>6) & 0x3F);
      *zOut++ = 0x80 + (u8)(c & 0x3F);
    }                                                    \
  }
  *zOut = 0;
  capdb_result_text64(context, (char*)z, zOut-z,capdb_free,CAPDB_UTF8_ZT);
}

/*
** The hex() function.  Interpret the argument as a blob.  Return
** a hexadecimal rendering as text.
*/
static void hexFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int i, n;
  const unsigned char *pBlob;
  char *zHex, *z;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  pBlob = capdb_value_blob(argv[0]);
  n = capdb_value_bytes(argv[0]);
  assert( pBlob==capdb_value_blob(argv[0]) );  /* No encoding change */
  z = zHex = contextMalloc(context, ((i64)n)*2 + 1);
  if( zHex ){
    for(i=0; i<n; i++, pBlob++){
      unsigned char c = *pBlob;
      *(z++) = hexdigits[(c>>4)&0xf];
      *(z++) = hexdigits[c&0xf];
    }
    *z = 0;
    capdb_result_text64(context, zHex, (u64)(z-zHex),
                          capdb_free, CAPDB_UTF8_ZT);
  }
}

/*
** Buffer zStr contains nStr bytes of utf-8 encoded text. Return 1 if zStr
** contains character ch, or 0 if it does not.
*/
static int strContainsChar(const u8 *zStr, int nStr, u32 ch){
  const u8 *zEnd = &zStr[nStr];
  const u8 *z = zStr;
  while( z<zEnd ){
    u32 tst = Utf8Read(z);
    if( tst==ch ) return 1;
  }
  return 0;
}

/*
** The unhex() function. This function may be invoked with either one or
** two arguments. In both cases the first argument is interpreted as text
** a text value containing a set of pairs of hexadecimal digits which are
** decoded and returned as a blob.
**
** If there is only a single argument, then it must consist only of an
** even number of hexadecimal digits. Otherwise, return NULL.
**
** Or, if there is a second argument, then any character that appears in
** the second argument is also allowed to appear between pairs of hexadecimal
** digits in the first argument. If any other character appears in the
** first argument, or if one of the allowed characters appears between
** two hexadecimal digits that make up a single byte, NULL is returned.
**
** The following expressions are all true:
**
**     unhex('ABCD')       IS x'ABCD'
**     unhex('AB CD')      IS NULL
**     unhex('AB CD', ' ') IS x'ABCD'
**     unhex('A BCD', ' ') IS NULL
*/
static void unhexFunc(
  capdb_context *pCtx,
  int argc,
  capdb_value **argv
){
  const u8 *zPass = (const u8*)"";
  int nPass = 0;
  const u8 *zHex = capdb_value_text(argv[0]);
  int nHex = capdb_value_bytes(argv[0]);
#ifdef CAPDB_DEBUG
  const u8 *zEnd = zHex ? &zHex[nHex] : 0;
#endif
  u8 *pBlob = 0;
  u8 *p = 0;

  assert( argc==1 || argc==2 );
  if( argc==2 ){
    zPass = capdb_value_text(argv[1]);
    nPass = capdb_value_bytes(argv[1]);
  }
  if( !zHex || !zPass ) return;

  p = pBlob = contextMalloc(pCtx, (nHex/2)+1);
  if( pBlob ){
    u8 c;                         /* Most significant digit of next byte */
    u8 d;                         /* Least significant digit of next byte */

    while( (c = *zHex)!=0x00 ){
      while( !capdbIsxdigit(c) ){
        u32 ch = Utf8Read(zHex);
        assert( zHex<=zEnd );
        if( !strContainsChar(zPass, nPass, ch) ) goto unhex_null;
        c = *zHex;
        if( c==0x00 ) goto unhex_done;
      }
      zHex++;
      assert( *zEnd==0x00 );
      assert( zHex<=zEnd );
      d = *(zHex++);
      if( !capdbIsxdigit(d) ) goto unhex_null;
      *(p++) = (capdbHexToInt(c)<<4) | capdbHexToInt(d);
    }
  }

 unhex_done:
  capdb_result_blob(pCtx, pBlob, (p - pBlob), capdb_free);
  return;

 unhex_null:
  capdb_free(pBlob);
  return;
}


/*
** The zeroblob(N) function returns a zero-filled blob of size N bytes.
*/
static void zeroblobFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  i64 n;
  int rc;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  n = capdb_value_int64(argv[0]);
  if( n<0 ) n = 0;
  rc = capdb_result_zeroblob64(context, n); /* IMP: R-00293-64994 */
  if( rc ){
    capdb_result_error_code(context, rc);
  }
}

/*
** The replace() function.  Three arguments are all strings: call
** them A, B, and C. The result is also a string which is derived
** from A by replacing every occurrence of B with C.  The match
** must be exact.  Collating sequences are not used.
*/
static void replaceFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *zStr;        /* The input string A */
  const unsigned char *zPattern;    /* The pattern string B */
  const unsigned char *zRep;        /* The replacement string C */
  unsigned char *zOut;              /* The output */
  int nStr;                /* Size of zStr */
  int nPattern;            /* Size of zPattern */
  int nRep;                /* Size of zRep */
  i64 nOut;                /* Maximum size of zOut */
  int loopLimit;           /* Last zStr[] that might match zPattern[] */
  i64 i, j;                /* Loop counters */
  unsigned cntExpand;      /* Number zOut expansions */
  capdb *db = capdb_context_db_handle(context);

  assert( argc==3 );
  UNUSED_PARAMETER(argc);
  zStr = capdb_value_text(argv[0]);
  if( zStr==0 ) return;
  nStr = capdb_value_bytes(argv[0]);
  assert( zStr==capdb_value_text(argv[0]) );  /* No encoding change */
  zPattern = capdb_value_text(argv[1]);
  if( zPattern==0 ){
    assert( capdb_value_type(argv[1])==CAPDB_NULL
            || capdb_context_db_handle(context)->mallocFailed );
    return;
  }
  if( zPattern[0]==0 ){
    assert( capdb_value_type(argv[1])!=CAPDB_NULL );
    capdb_result_text(context, (const char*)zStr, nStr, CAPDB_TRANSIENT);
    return;
  }
  nPattern = capdb_value_bytes(argv[1]);
  assert( zPattern==capdb_value_text(argv[1]) );  /* No encoding change */
  zRep = capdb_value_text(argv[2]);
  if( zRep==0 ) return;
  nRep = capdb_value_bytes(argv[2]);
  assert( zRep==capdb_value_text(argv[2]) );
  nOut = nStr + 1;
  assert( nOut<CAPDB_MAX_LENGTH );
  zOut = contextMalloc(context, nOut);
  if( zOut==0 ){
    return;
  }
  loopLimit = nStr - nPattern; 
  cntExpand = 0;
  for(i=j=0; i<=loopLimit; i++){
    if( zStr[i]!=zPattern[0] || memcmp(&zStr[i], zPattern, nPattern) ){
      zOut[j++] = zStr[i];
    }else{
      if( nRep>nPattern ){
        nOut += nRep - nPattern;
        testcase( nOut-1==db->aLimit[CAPDB_LIMIT_LENGTH] );
        testcase( nOut-2==db->aLimit[CAPDB_LIMIT_LENGTH] );
        if( nOut-1>db->aLimit[CAPDB_LIMIT_LENGTH] ){
          capdb_result_error_toobig(context);
          capdb_free(zOut);
          return;
        }
        cntExpand++;
        if( (cntExpand&(cntExpand-1))==0 ){
          /* Grow the size of the output buffer only on substitutions
          ** whose index is a power of two: 1, 2, 4, 8, 16, 32, ... */
          u8 *zOld;
          zOld = zOut;
          zOut = capdbRealloc(zOut, (int)nOut + (nOut - nStr - 1));
          if( zOut==0 ){
            capdb_result_error_nomem(context);
            capdb_free(zOld);
            return;
          }
        }
      }
      memcpy(&zOut[j], zRep, nRep);
      j += nRep;
      i += nPattern-1;
    }
  }
  assert( j+nStr-i+1<=nOut );
  memcpy(&zOut[j], &zStr[i], nStr-i);
  j += nStr - i;
  assert( j<=nOut );
  zOut[j] = 0;
  capdb_result_text(context, (char*)zOut, j, capdb_free);
}

/*
** Implementation of the TRIM(), LTRIM(), and RTRIM() functions.
** The userdata is 0x1 for left trim, 0x2 for right trim, 0x3 for both.
*/
static void trimFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const unsigned char *zIn;         /* Input string */
  const unsigned char *zCharSet;    /* Set of characters to trim */
  unsigned int nIn;                 /* Number of bytes in input */
  int flags;                        /* 1: trimleft  2: trimright  3: trim */
  int i;                            /* Loop counter */
  unsigned int *aLen = 0;           /* Length of each character in zCharSet */
  unsigned char **azChar = 0;       /* Individual characters in zCharSet */
  int nChar;                        /* Number of characters in zCharSet */

  if( capdb_value_type(argv[0])==CAPDB_NULL ){
    return;
  }
  zIn = capdb_value_text(argv[0]);
  if( zIn==0 ) return;
  nIn = (unsigned)capdb_value_bytes(argv[0]);
  assert( zIn==capdb_value_text(argv[0]) );
  if( argc==1 ){
    static const unsigned lenOne[] = { 1 };
    static unsigned char * const azOne[] = { (u8*)" " };
    nChar = 1;
    aLen = (unsigned*)lenOne;
    azChar = (unsigned char **)azOne;
    zCharSet = 0;
  }else if( (zCharSet = capdb_value_text(argv[1]))==0 ){
    return;
  }else{
    const unsigned char *z;
    for(z=zCharSet, nChar=0; *z; nChar++){
      CAPDB_SKIP_UTF8(z);
    }
    if( nChar>0 ){
      azChar = contextMalloc(context,
                     ((i64)nChar)*(sizeof(char*)+sizeof(unsigned)));
      if( azChar==0 ){
        return;
      }
      aLen = (unsigned*)&azChar[nChar];
      for(z=zCharSet, nChar=0; *z; nChar++){
        azChar[nChar] = (unsigned char *)z;
        CAPDB_SKIP_UTF8(z);
        aLen[nChar] = (unsigned)(z - azChar[nChar]);
      }
    }
  }
  if( nChar>0 ){
    flags = CAPDB_PTR_TO_INT(capdb_user_data(context));
    if( flags & 1 ){
      while( nIn>0 ){
        unsigned int len = 0;
        for(i=0; i<nChar; i++){
          len = aLen[i];
          if( len<=nIn && memcmp(zIn, azChar[i], len)==0 ) break;
        }
        if( i>=nChar ) break;
        zIn += len;
        nIn -= len;
      }
    }
    if( flags & 2 ){
      while( nIn>0 ){
        unsigned int len = 0;
        for(i=0; i<nChar; i++){
          len = aLen[i];
          if( len<=nIn && memcmp(&zIn[nIn-len],azChar[i],len)==0 ) break;
        }
        if( i>=nChar ) break;
        nIn -= len;
      }
    }
    if( zCharSet ){
      capdb_free(azChar);
    }
  }
  capdb_result_text(context, (char*)zIn, nIn, CAPDB_TRANSIENT);
}

/* The core implementation of the CONCAT(...) and CONCAT_WS(SEP,...)
** functions.
**
** Return a string value that is the concatenation of all non-null
** entries in argv[].  Use zSep as the separator.
*/
static void concatFuncCore(
  capdb_context *context,
  int argc,
  capdb_value **argv,
  int nSep,
  const char *zSep
){
  i64 j, n = 0;
  int i;
  int bNotNull = 0;   /* True after at least NOT NULL argument seen */
  char *z;
  for(i=0; i<argc; i++){
    n += capdb_value_bytes(argv[i]);
  }
  n += (argc-1)*(i64)nSep;
  z = capdb_malloc64(n+1);
  if( z==0 ){
    capdb_result_error_nomem(context);
    return;
  }
  j = 0;
  for(i=0; i<argc; i++){
    if( capdb_value_type(argv[i])!=CAPDB_NULL ){
      int k = capdb_value_bytes(argv[i]);
      const char *v = (const char*)capdb_value_text(argv[i]);
      if( v!=0 ){
        if( bNotNull && nSep>0 ){
          memcpy(&z[j], zSep, nSep);
          j += nSep;
        }
        memcpy(&z[j], v, k);
        j += k;
        bNotNull = 1;
      }
    }
  }
  z[j] = 0;
  assert( j<=n );
  capdb_result_text64(context, z, j, capdb_free, CAPDB_UTF8_ZT);
}

/*
** The CONCAT(...) function.  Generate a string result that is the
** concatentation of all non-null arguments.
*/
static void concatFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  concatFuncCore(context, argc, argv, 0, "");
}

/*
** The CONCAT_WS(separator, ...) function.
**
** Generate a string that is the concatenation of 2nd through the Nth
** argument.  Use the first argument (which must be non-NULL) as the
** separator.
*/
static void concatwsFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int nSep = capdb_value_bytes(argv[0]);
  const char *zSep = (const char*)capdb_value_text(argv[0]);
  if( zSep==0 ) return;
  concatFuncCore(context, argc-1, argv+1, nSep, zSep);
}


#ifdef CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION
/*
** The "unknown" function is automatically substituted in place of
** any unrecognized function name when doing an EXPLAIN or EXPLAIN QUERY PLAN
** when the CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION compile-time option is used.
** When the "capdb" command-line shell is built using this functionality,
** that allows an EXPLAIN or EXPLAIN QUERY PLAN for complex queries
** involving application-defined functions to be examined in a generic
** capdb shell.
*/
static void unknownFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  /* no-op */
  (void)context;
  (void)argc;
  (void)argv;
}
#endif /*CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION*/


/* IMP: R-25361-16150 This function is omitted from SQLite by default. It
** is only available if the CAPDB_SOUNDEX compile-time option is used
** when SQLite is built.
*/
#ifdef CAPDB_SOUNDEX
/*
** Compute the soundex encoding of a word.
**
** IMP: R-59782-00072 The soundex(X) function returns a string that is the
** soundex encoding of the string X.
*/
static void soundexFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  char zResult[8];
  const u8 *zIn;
  int i, j;
  static const unsigned char iCode[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
    1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
    1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
  };
  assert( argc==1 );
  zIn = (u8*)capdb_value_text(argv[0]);
  if( zIn==0 ) zIn = (u8*)"";
  for(i=0; zIn[i] && !capdbIsalpha(zIn[i]); i++){}
  if( zIn[i] ){
    u8 prevcode = iCode[zIn[i]&0x7f];
    zResult[0] = capdbToupper(zIn[i]);
    for(j=1; j<4 && zIn[i]; i++){
      int code = iCode[zIn[i]&0x7f];
      if( code>0 ){
        if( code!=prevcode ){
          prevcode = code;
          zResult[j++] = code + '0';
        }
      }else{
        prevcode = 0;
      }
    }
    while( j<4 ){
      zResult[j++] = '0';
    }
    zResult[j] = 0;
    capdb_result_text(context, zResult, 4, CAPDB_TRANSIENT);
  }else{
    /* IMP: R-64894-50321 The string "?000" is returned if the argument
    ** is NULL or contains no ASCII alphabetic characters. */
    capdb_result_text(context, "?000", 4, CAPDB_STATIC);
  }
}
#endif /* CAPDB_SOUNDEX */

#ifndef CAPDB_OMIT_LOAD_EXTENSION
/*
** A function that loads a shared-library extension then returns NULL.
*/
static void loadExt(capdb_context *context, int argc, capdb_value **argv){
  const char *zFile = (const char *)capdb_value_text(argv[0]);
  const char *zProc;
  capdb *db = capdb_context_db_handle(context);
  char *zErrMsg = 0;

  /* Disallow the load_extension() SQL function unless the CAPDB_LoadExtFunc
  ** flag is set.  See the capdb_enable_load_extension() API.
  */
  if( (db->flags & CAPDB_LoadExtFunc)==0 ){
    capdb_result_error(context, "not authorized", -1);
    return;
  }

  if( argc==2 ){
    zProc = (const char *)capdb_value_text(argv[1]);
  }else{
    zProc = 0;
  }
  if( zFile && capdb_load_extension(db, zFile, zProc, &zErrMsg) ){
    capdb_result_error(context, zErrMsg, -1);
    capdb_free(zErrMsg);
  }
}
#endif


/*
** An instance of the following structure holds the context of a
** sum() or avg() aggregate computation.
*/
typedef struct SumCtx SumCtx;
struct SumCtx {
  double rSum;      /* Running sum as as a double */
  double rErr;      /* Error term for Kahan-Babushka-Neumaier summation */
  i64 iSum;         /* Running sum as a signed integer */
  i64 cnt;          /* Number of elements summed */
  u8 approx;        /* True if any non-integer value was input to the sum */
  u8 ovrfl;         /* Integer overflow seen */
};

/*
** Do one step of the Kahan-Babushka-Neumaier summation.
**
** https://en.wikipedia.org/wiki/Kahan_summation_algorithm
**
** Variables are marked "volatile" to defeat c89 x86 floating point
** optimizations can mess up this algorithm.
*/
static void kahanBabuskaNeumaierStep(
  volatile SumCtx *pSum,
  volatile double r
){
  volatile double s = pSum->rSum;
  volatile double t = s + r;
  if( fabs(s) > fabs(r) ){
    pSum->rErr += (s - t) + r;
  }else{
    pSum->rErr += (r - t) + s;
  }
  pSum->rSum = t;
}

/*
** Add a (possibly large) integer to the running sum.
*/
static void kahanBabuskaNeumaierStepInt64(volatile SumCtx *pSum, i64 iVal){
  if( iVal<=-4503599627370496LL || iVal>=+4503599627370496LL ){
    i64 iBig, iSm;
    iSm = iVal % 16384;
    iBig = iVal - iSm;
    kahanBabuskaNeumaierStep(pSum, iBig);
    kahanBabuskaNeumaierStep(pSum, iSm);
  }else{
    kahanBabuskaNeumaierStep(pSum, (double)iVal);
  }
}

/*
** Initialize the Kahan-Babaska-Neumaier sum from a 64-bit integer
*/
static void kahanBabuskaNeumaierInit(
  volatile SumCtx *p,
  i64 iVal
){
  if( iVal<=-4503599627370496LL || iVal>=+4503599627370496LL ){
    i64 iSm = iVal % 16384;
    p->rSum = (double)(iVal - iSm);
    p->rErr = (double)iSm;
  }else{
    p->rSum = (double)iVal;
    p->rErr = 0.0;
  }
}

/*
** Routines used to compute the sum, average, and total.
**
** The SUM() function follows the (broken) SQL standard which means
** that it returns NULL if it sums over no inputs.  TOTAL returns
** 0.0 in that case.  In addition, TOTAL always returns a float where
** SUM might return an integer if it never encounters a floating point
** value.  TOTAL never fails, but SUM might throw an exception if
** it overflows an integer.
*/
static void sumStep(capdb_context *context, int argc, capdb_value **argv){
  SumCtx *p;
  int type;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  p = capdb_aggregate_context(context, sizeof(*p));
  type = capdb_value_numeric_type(argv[0]);
  if( p && type!=CAPDB_NULL ){
    p->cnt++;
    if( p->approx==0 ){
      if( type!=CAPDB_INTEGER ){
        kahanBabuskaNeumaierInit(p, p->iSum);
        p->approx = 1;
        kahanBabuskaNeumaierStep(p, capdb_value_double(argv[0]));
      }else{
        i64 x = p->iSum;
        if( capdbAddInt64(&x, capdb_value_int64(argv[0]))==0 ){
          p->iSum = x;
        }else{
          p->ovrfl = 1;
          kahanBabuskaNeumaierInit(p, p->iSum);
          p->approx = 1;
          kahanBabuskaNeumaierStepInt64(p, capdb_value_int64(argv[0]));
        }
      }
    }else{
      if( type==CAPDB_INTEGER ){
        kahanBabuskaNeumaierStepInt64(p, capdb_value_int64(argv[0]));
      }else{
        p->ovrfl = 0;
        kahanBabuskaNeumaierStep(p, capdb_value_double(argv[0]));
      }
    }
  }
}
#ifndef CAPDB_OMIT_WINDOWFUNC
static void sumInverse(capdb_context *context, int argc, capdb_value**argv){
  SumCtx *p;
  int type;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  p = capdb_aggregate_context(context, sizeof(*p));
  type = capdb_value_numeric_type(argv[0]);
  /* p is always non-NULL because sumStep() will have been called first
  ** to initialize it */
  if( ALWAYS(p) && type!=CAPDB_NULL ){
    assert( p->cnt>0 );
    p->cnt--;
    if( !p->approx ){
      i64 x = p->iSum;
      if( capdbSubInt64(&x, capdb_value_int64(argv[0]))==0 ){
        p->iSum = x;
        return;
      }
      p->ovrfl = 1;
      p->approx = 1;
      kahanBabuskaNeumaierInit(p, p->iSum);
    }
    if( type==CAPDB_INTEGER ){
      i64 iVal = capdb_value_int64(argv[0]);
      if( iVal!=SMALLEST_INT64 ){
        kahanBabuskaNeumaierStepInt64(p, -iVal);
      }else{
        kahanBabuskaNeumaierStepInt64(p, LARGEST_INT64);
        kahanBabuskaNeumaierStepInt64(p, 1);
      }       
    }else{
      kahanBabuskaNeumaierStep(p, -capdb_value_double(argv[0]));
    }
  }
}
#else
# define sumInverse 0
#endif /* CAPDB_OMIT_WINDOWFUNC */
static void sumFinalize(capdb_context *context){
  SumCtx *p;
  p = capdb_aggregate_context(context, 0);
  if( p && p->cnt>0 ){
    if( p->approx ){
      if( p->ovrfl ){
        capdb_result_error(context,"integer overflow",-1);
      }else if( !capdbIsOverflow(p->rErr) ){
        capdb_result_double(context, p->rSum+p->rErr);
      }else{
        capdb_result_double(context, p->rSum);
      }
    }else{
      capdb_result_int64(context, p->iSum);
    }
  }
}
static void avgFinalize(capdb_context *context){
  SumCtx *p;
  p = capdb_aggregate_context(context, 0);
  if( p && p->cnt>0 ){
    double r;
    if( p->approx ){
      r = p->rSum;
      if( !capdbIsOverflow(p->rErr) ) r += p->rErr;
    }else{
      r = (double)(p->iSum);
    }
    capdb_result_double(context, r/(double)p->cnt);
  }
}
static void totalFinalize(capdb_context *context){
  SumCtx *p;
  double r = 0.0;
  p = capdb_aggregate_context(context, 0);
  if( p ){
    if( p->approx ){
      r = p->rSum;
      if( !capdbIsOverflow(p->rErr) ) r += p->rErr;
    }else{
      r = (double)(p->iSum);
    }
  }
  capdb_result_double(context, r);
}

/*
** The following structure keeps track of state information for the
** count() aggregate function.
*/
typedef struct CountCtx CountCtx;
struct CountCtx {
  i64 n;
#ifdef CAPDB_DEBUG
  int bInverse;                   /* True if xInverse() ever called */
#endif
};

/*
** Routines to implement the count() aggregate function.
*/
static void countStep(capdb_context *context, int argc, capdb_value **argv){
  CountCtx *p;
  p = capdb_aggregate_context(context, sizeof(*p));
  if( (argc==0 || CAPDB_NULL!=capdb_value_type(argv[0])) && p ){
    p->n++;
  }

#ifndef CAPDB_OMIT_DEPRECATED
  /* The capdb_aggregate_count() function is deprecated.  But just to make
  ** sure it still operates correctly, verify that its count agrees with our
  ** internal count when using count(*) and when the total count can be
  ** expressed as a 32-bit integer. */
  assert( argc==1 || p==0 || p->n>0x7fffffff || p->bInverse
          || p->n==capdb_aggregate_count(context) );
#endif
}  
static void countFinalize(capdb_context *context){
  CountCtx *p;
  p = capdb_aggregate_context(context, 0);
  capdb_result_int64(context, p ? p->n : 0);
}
#ifndef CAPDB_OMIT_WINDOWFUNC
static void countInverse(capdb_context *ctx, int argc, capdb_value **argv){
  CountCtx *p;
  p = capdb_aggregate_context(ctx, sizeof(*p));
  /* p is always non-NULL since countStep() will have been called first */
  if( (argc==0 || CAPDB_NULL!=capdb_value_type(argv[0])) && ALWAYS(p) ){
    p->n--;
#ifdef CAPDB_DEBUG
    p->bInverse = 1;
#endif
  }
}  
#else
# define countInverse 0
#endif /* CAPDB_OMIT_WINDOWFUNC */

/*
** Routines to implement min() and max() aggregate functions.
*/
static void minmaxStep(
  capdb_context *context,
  int NotUsed,
  capdb_value **argv
){
  Mem *pArg  = (Mem *)argv[0];
  Mem *pBest;
  UNUSED_PARAMETER(NotUsed);

  pBest = (Mem *)capdb_aggregate_context(context, sizeof(*pBest));
  if( !pBest ) return;

  if( capdb_value_type(pArg)==CAPDB_NULL ){
    if( pBest->flags ) capdbSkipAccumulatorLoad(context);
  }else if( pBest->flags ){
    int max;
    int cmp;
    CollSeq *pColl = capdbGetFuncCollSeq(context);
    /* This step function is used for both the min() and max() aggregates,
    ** the only difference between the two being that the sense of the
    ** comparison is inverted. For the max() aggregate, the
    ** capdb_user_data() function returns (void *)-1. For min() it
    ** returns (void *)db, where db is the capdb* database pointer.
    ** Therefore the next statement sets variable 'max' to 1 for the max()
    ** aggregate, or 0 for min().
    */
    max = capdb_user_data(context)!=0;
    cmp = capdbMemCompare(pBest, pArg, pColl);
    if( (max && cmp<0) || (!max && cmp>0) ){
      capdbVdbeMemCopy(pBest, pArg);
    }else{
      capdbSkipAccumulatorLoad(context);
    }
  }else{
    pBest->db = capdb_context_db_handle(context);
    capdbVdbeMemCopy(pBest, pArg);
  }
}
static void minMaxValueFinalize(capdb_context *context, int bValue){
  capdb_value *pRes;
  pRes = (capdb_value *)capdb_aggregate_context(context, 0);
  if( pRes ){
    if( pRes->flags ){
      capdb_result_value(context, pRes);
    }
    if( bValue==0 ) capdbVdbeMemRelease(pRes);
  }
}
#ifndef CAPDB_OMIT_WINDOWFUNC
static void minMaxValue(capdb_context *context){
  minMaxValueFinalize(context, 1);
}
#else
# define minMaxValue 0
#endif /* CAPDB_OMIT_WINDOWFUNC */
static void minMaxFinalize(capdb_context *context){
  minMaxValueFinalize(context, 0);
}

/*
** group_concat(EXPR, ?SEPARATOR?)
** string_agg(EXPR, SEPARATOR)
**
** Content is accumulated in GroupConcatCtx.str with the SEPARATOR
** coming before the EXPR value, except for the first entry which
** omits the SEPARATOR.
**
** It is tragic that the SEPARATOR goes before the EXPR string.  The
** groupConcatInverse() implementation would have been easier if the
** SEPARATOR were appended after EXPR.  And the order is undocumented,
** so we could change it, in theory.  But the old behavior has been
** around for so long that we dare not, for fear of breaking something.
*/
typedef struct {
  StrAccum str;          /* The accumulated concatenation */
#ifndef CAPDB_OMIT_WINDOWFUNC
  int nAccum;            /* Number of strings presently concatenated */
  int nFirstSepLength;   /* Used to detect separator length change */
  /* If pnSepLengths!=0, refs an array of inter-string separator lengths,
  ** stored as actually incorporated into presently accumulated result.
  ** (Hence, its slots in use number nAccum-1 between method calls.)
  ** If pnSepLengths==0, nFirstSepLength is the length used throughout.
  */
  int *pnSepLengths;
#endif
} GroupConcatCtx;

static void groupConcatStep(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zVal;
  GroupConcatCtx *pGCC;
  const char *zSep;
  int nVal, nSep;
  assert( argc==1 || argc==2 );
  if( capdb_value_type(argv[0])==CAPDB_NULL ) return;
  pGCC = (GroupConcatCtx*)capdb_aggregate_context(context, sizeof(*pGCC));
  if( pGCC ){
    capdb *db = capdb_context_db_handle(context);
    int firstTerm = pGCC->str.mxAlloc==0;
    pGCC->str.mxAlloc = db->aLimit[CAPDB_LIMIT_LENGTH];
    if( argc==1 ){
      if( !firstTerm ){
        capdb_str_appendchar(&pGCC->str, 1, ',');
      }
#ifndef CAPDB_OMIT_WINDOWFUNC
      else{
        pGCC->nFirstSepLength = 1;
      }
#endif
    }else if( !firstTerm ){
      zSep = (char*)capdb_value_text(argv[1]);
      nSep = capdb_value_bytes(argv[1]);
      if( zSep ){
        capdb_str_append(&pGCC->str, zSep, nSep);
      }
#ifndef CAPDB_OMIT_WINDOWFUNC
      else{
        nSep = 0;
      }
      if( nSep != pGCC->nFirstSepLength || pGCC->pnSepLengths != 0 ){
        int *pnsl = pGCC->pnSepLengths;
        if( pnsl == 0 ){
          /* First separator length variation seen, start tracking them. */
          pnsl = (int*)capdb_malloc64((pGCC->nAccum+1) * sizeof(int));
          if( pnsl!=0 ){
            int i = 0, nA = pGCC->nAccum-1;
            while( i<nA ) pnsl[i++] = pGCC->nFirstSepLength;
          }
        }else{
          pnsl = (int*)capdb_realloc64(pnsl, pGCC->nAccum * sizeof(int));
        }
        if( pnsl!=0 ){
          if( ALWAYS(pGCC->nAccum>0) ){
            pnsl[pGCC->nAccum-1] = nSep;
          }
          pGCC->pnSepLengths = pnsl;
        }else{
          capdbStrAccumSetError(&pGCC->str, CAPDB_NOMEM);
        }
      }
#endif
    }
#ifndef CAPDB_OMIT_WINDOWFUNC
    else{
      pGCC->nFirstSepLength = capdb_value_bytes(argv[1]);
    }
    pGCC->nAccum += 1;
#endif
    zVal = (char*)capdb_value_text(argv[0]);
    nVal = capdb_value_bytes(argv[0]);
    if( zVal ) capdb_str_append(&pGCC->str, zVal, nVal);
  }
}

#ifndef CAPDB_OMIT_WINDOWFUNC
static void groupConcatInverse(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  GroupConcatCtx *pGCC;
  assert( argc==1 || argc==2 );
  (void)argc;  /* Suppress unused parameter warning */
  if( capdb_value_type(argv[0])==CAPDB_NULL ) return;
  pGCC = (GroupConcatCtx*)capdb_aggregate_context(context, sizeof(*pGCC));
  /* pGCC is always non-NULL since groupConcatStep() will have always
  ** run first to initialize it */
  if( ALWAYS(pGCC) ){
    int nVS;  /* Number of characters to remove */
    /* Must call capdb_value_text() to convert the argument into text prior
    ** to invoking capdb_value_bytes(), in case the text encoding is UTF16 */
    (void)capdb_value_text(argv[0]);
    nVS = capdb_value_bytes(argv[0]);
    pGCC->nAccum -= 1;
    if( pGCC->pnSepLengths!=0 ){
      assert(pGCC->nAccum >= 0);
      if( pGCC->nAccum>0 ){
        nVS += *pGCC->pnSepLengths;
        memmove(pGCC->pnSepLengths, pGCC->pnSepLengths+1,
               (pGCC->nAccum-1)*sizeof(int));
      }
    }else{
      /* If removing single accumulated string, harmlessly over-do. */
      nVS += pGCC->nFirstSepLength;
    }
    if( nVS>=(int)pGCC->str.nChar ){
      pGCC->str.nChar = 0;
    }else{
      pGCC->str.nChar -= nVS;
      memmove(pGCC->str.zText, &pGCC->str.zText[nVS], pGCC->str.nChar);
    }
    if( pGCC->str.nChar==0 ){
      pGCC->str.mxAlloc = 0;
      capdb_free(pGCC->pnSepLengths);
      pGCC->pnSepLengths = 0;
    }
  }
}
#else
# define groupConcatInverse 0
#endif /* CAPDB_OMIT_WINDOWFUNC */
static void groupConcatFinalize(capdb_context *context){
  GroupConcatCtx *pGCC
    = (GroupConcatCtx*)capdb_aggregate_context(context, 0);
  if( pGCC ){
    capdbResultStrAccum(context, &pGCC->str);
#ifndef CAPDB_OMIT_WINDOWFUNC
    capdb_free(pGCC->pnSepLengths);
#endif
  }
}
#ifndef CAPDB_OMIT_WINDOWFUNC
static void groupConcatValue(capdb_context *context){
  GroupConcatCtx *pGCC
    = (GroupConcatCtx*)capdb_aggregate_context(context, 0);
  if( pGCC ){
    StrAccum *pAccum = &pGCC->str;
    if( pAccum->accError==CAPDB_TOOBIG ){
      capdb_result_error_toobig(context);
    }else if( pAccum->accError==CAPDB_NOMEM ){
      capdb_result_error_nomem(context);
    }else if( pGCC->nAccum>0 && pAccum->nChar==0 ){
      capdb_result_text(context, "", 1, CAPDB_STATIC);
    }else{   
      const char *zText = capdb_str_value(pAccum);
      capdb_result_text(context, zText, pAccum->nChar, CAPDB_TRANSIENT);
    }
  }
}
#else
# define groupConcatValue 0
#endif /* CAPDB_OMIT_WINDOWFUNC */

/*
** This routine does per-connection function registration.  Most
** of the built-in functions above are part of the global function set.
** This routine only deals with those that are not global.
*/
void capdbRegisterPerConnectionBuiltinFunctions(capdb *db){
  int rc = capdb_overload_function(db, "MATCH", 2);
  assert( rc==CAPDB_NOMEM || rc==CAPDB_OK );
  if( rc==CAPDB_NOMEM ){
    capdbOomFault(db);
  }
}

/*
** Re-register the built-in LIKE functions.  The caseSensitive
** parameter determines whether or not the LIKE operator is case
** sensitive.
*/
void capdbRegisterLikeFunctions(capdb *db, int caseSensitive){
  FuncDef *pDef;
  struct compareInfo *pInfo;
  int flags;
  int nArg;
  if( caseSensitive ){
    pInfo = (struct compareInfo*)&likeInfoAlt;
    flags = CAPDB_FUNC_LIKE | CAPDB_FUNC_CASE;
  }else{
    pInfo = (struct compareInfo*)&likeInfoNorm;
    flags = CAPDB_FUNC_LIKE;
  }
  for(nArg=2; nArg<=3; nArg++){
    capdbCreateFunc(db, "like", nArg, CAPDB_UTF8, pInfo, likeFunc, 
                      0, 0, 0, 0, 0);
    pDef = capdbFindFunction(db, "like", nArg, CAPDB_UTF8, 0);
    assert( pDef!=0 ); /* The capdbCreateFunc() call above cannot fail
                       ** because the "like" SQL-function already exists */
    pDef->funcFlags |= flags;
    pDef->funcFlags &= ~CAPDB_FUNC_UNSAFE;
  }
}

/*
** pExpr points to an expression which implements a function.  If
** it is appropriate to apply the LIKE optimization to that function
** then set aWc[0] through aWc[2] to the wildcard characters and the
** escape character and then return TRUE.  If the function is not a
** LIKE-style function then return FALSE.
**
** The expression "a LIKE b ESCAPE c" is only considered a valid LIKE
** operator if c is a string literal that is exactly one byte in length.
** That one byte is stored in aWc[3].  aWc[3] is set to zero if there is
** no ESCAPE clause.
**
** *pIsNocase is set to true if uppercase and lowercase are equivalent for
** the function (default for LIKE).  If the function makes the distinction
** between uppercase and lowercase (as does GLOB) then *pIsNocase is set to
** false.
*/
int capdbIsLikeFunction(capdb *db, Expr *pExpr, int *pIsNocase, char *aWc){
  FuncDef *pDef;
  int nExpr;
  assert( pExpr!=0 );
  assert( pExpr->op==TK_FUNCTION );
  assert( ExprUseXList(pExpr) );
  if( !pExpr->x.pList ){
    return 0;
  }
  nExpr = pExpr->x.pList->nExpr;
  assert( !ExprHasProperty(pExpr, EP_IntValue) );
  pDef = capdbFindFunction(db, pExpr->u.zToken, nExpr, CAPDB_UTF8, 0);
#ifdef CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION
  if( pDef==0 ) return 0;
#endif
  if( NEVER(pDef==0) || (pDef->funcFlags & CAPDB_FUNC_LIKE)==0 ){
    return 0;
  }

  /* The memcpy() statement assumes that the wildcard characters are
  ** the first three statements in the compareInfo structure.  The
  ** asserts() that follow verify that assumption
  */
  memcpy(aWc, pDef->pUserData, 3);
  assert( (char*)&likeInfoAlt == (char*)&likeInfoAlt.matchAll );
  assert( &((char*)&likeInfoAlt)[1] == (char*)&likeInfoAlt.matchOne );
  assert( &((char*)&likeInfoAlt)[2] == (char*)&likeInfoAlt.matchSet );

  if( nExpr<3 ){
    aWc[3] = 0;
  }else{
    Expr *pEscape = pExpr->x.pList->a[2].pExpr;
    char *zEscape;
    if( pEscape->op!=TK_STRING ) return 0;
    assert( !ExprHasProperty(pEscape, EP_IntValue) );
    zEscape = pEscape->u.zToken;
    if( zEscape[0]==0 || zEscape[1]!=0 ) return 0;
    if( zEscape[0]==aWc[0] ) return 0;
    if( zEscape[0]==aWc[1] ) return 0;
    aWc[3] = zEscape[0];
  }

  *pIsNocase = (pDef->funcFlags & CAPDB_FUNC_CASE)==0;
  return 1;
}

/* Mathematical Constants */
#ifndef M_PI
# define M_PI   3.141592653589793238462643383279502884
#endif
#ifndef M_LN10
# define M_LN10 2.302585092994045684017991454684364208
#endif
#ifndef M_LN2
# define M_LN2  0.693147180559945309417232121458176568
#endif


/* Extra math functions that require linking with -lm
*/
#ifdef CAPDB_ENABLE_MATH_FUNCTIONS
/*
** Implementation SQL functions:
**
**   ceil(X)
**   ceiling(X)
**   floor(X)
**
** The capdb_user_data() pointer is a pointer to the libm implementation
** of the underlying C function.
*/
static void ceilingFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  assert( argc==1 );
  switch( capdb_value_numeric_type(argv[0]) ){
    case CAPDB_INTEGER: {
       capdb_result_int64(context, capdb_value_int64(argv[0]));
       break;
    }
    case CAPDB_FLOAT: {
       double (*x)(double) = (double(*)(double))capdb_user_data(context);
       capdb_result_double(context, x(capdb_value_double(argv[0])));
       break;
    }
    default: {
       break;
    }
  }
}

/*
** On some systems, ceil() and floor() are intrinsic function.  You are
** unable to take a pointer to these functions.  Hence, we here wrap them
** in our own actual functions.
*/
static double xCeil(double x){ return ceil(x); }
static double xFloor(double x){ return floor(x); }

/*
** Some systems do not have log2() and log10() in their standard math
** libraries.
*/
#if defined(HAVE_LOG10) && HAVE_LOG10==0
# define log10(X) (0.4342944819032517867*log(X))
#endif
#if defined(HAVE_LOG2) && HAVE_LOG2==0
# define log2(X) (1.442695040888963456*log(X))
#endif


/*
** Implementation of SQL functions:
**
**   ln(X)       - natural logarithm
**   log(X)      - log X base 10
**   log10(X)    - log X base 10
**   log(B,X)    - log X base B
*/
static void logFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  double x, b, ans;
  assert( argc==1 || argc==2 );
  switch( capdb_value_numeric_type(argv[0]) ){
    case CAPDB_INTEGER:
    case CAPDB_FLOAT:
      x = capdb_value_double(argv[0]);
      if( x<=0.0 ) return;
      break;
    default:
      return;
  }
  if( argc==2 ){
    switch( capdb_value_numeric_type(argv[0]) ){
      case CAPDB_INTEGER:
      case CAPDB_FLOAT:
        b = log(x);
        if( b<=0.0 ) return;
        x = capdb_value_double(argv[1]);
        if( x<=0.0 ) return;
        break;
     default:
        return;
    }
    ans = log(x)/b;
  }else{
    switch( CAPDB_PTR_TO_INT(capdb_user_data(context)) ){
      case 1:
        ans = log10(x);
        break;
      case 2:
        ans = log2(x);
        break;
      default:
        ans = log(x);
        break;
    }
  }
  capdb_result_double(context, ans);
}

/*
** Functions to converts degrees to radians and radians to degrees.
*/
static double degToRad(double x){ return x*(M_PI/180.0); }
static double radToDeg(double x){ return x*(180.0/M_PI); }

/*
** Implementation of 1-argument SQL math functions:
**
**   exp(X)  - Compute e to the X-th power
*/
static void math1Func(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int type0;
  double v0, ans;
  double (*x)(double);
  assert( argc==1 );
  type0 = capdb_value_numeric_type(argv[0]);
  if( type0!=CAPDB_INTEGER && type0!=CAPDB_FLOAT ) return;
  v0 = capdb_value_double(argv[0]);
  x = (double(*)(double))capdb_user_data(context);
  ans = x(v0);
  capdb_result_double(context, ans);
}

/*
** Implementation of 2-argument SQL math functions:
**
**   power(X,Y)  - Compute X to the Y-th power
*/
static void math2Func(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int type0, type1;
  double v0, v1, ans;
  double (*x)(double,double);
  assert( argc==2 );
  type0 = capdb_value_numeric_type(argv[0]);
  if( type0!=CAPDB_INTEGER && type0!=CAPDB_FLOAT ) return;
  type1 = capdb_value_numeric_type(argv[1]);
  if( type1!=CAPDB_INTEGER && type1!=CAPDB_FLOAT ) return;
  v0 = capdb_value_double(argv[0]);
  v1 = capdb_value_double(argv[1]);
  x = (double(*)(double,double))capdb_user_data(context);
  ans = x(v0, v1);
  capdb_result_double(context, ans);
}

/*
** Implementation of 0-argument pi() function.
*/
static void piFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  assert( argc==0 );
  (void)argv;
  capdb_result_double(context, M_PI);
}

#endif /* CAPDB_ENABLE_MATH_FUNCTIONS */

/*
** Implementation of sign(X) function.
*/
static void signFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int type0;
  double x;
  UNUSED_PARAMETER(argc);
  assert( argc==1 );
  type0 = capdb_value_numeric_type(argv[0]);
  if( type0!=CAPDB_INTEGER && type0!=CAPDB_FLOAT ) return;
  x = capdb_value_double(argv[0]);
  capdb_result_int(context, x<0.0 ? -1 : x>0.0 ? +1 : 0);
}

#if defined(CAPDB_ENABLE_PERCENTILE)
/***********************************************************************
** This section implements the percentile(Y,P) SQL function and similar.
** Requirements:
**
**   (1)  The percentile(Y,P) function is an aggregate function taking
**        exactly two arguments.
**
**   (2)  If the P argument to percentile(Y,P) is not the same for every
**        row in the aggregate then an error is thrown.  The word "same"
**        in the previous sentence means that the value differ by less
**        than 0.001.
**
**   (3)  If the P argument to percentile(Y,P) evaluates to anything other
**        than a number in the range of 0.0 to 100.0 inclusive then an
**        error is thrown.
**
**   (4)  If any Y argument to percentile(Y,P) evaluates to a value that
**        is not NULL and is not numeric then an error is thrown.
**
**   (5)  If any Y argument to percentile(Y,P) evaluates to plus or minus
**        infinity then an error is thrown.  (SQLite always interprets NaN
**        values as NULL.)
**
**   (6)  Both Y and P in percentile(Y,P) can be arbitrary expressions,
**        including CASE WHEN expressions.
**
**   (7)  The percentile(Y,P) aggregate is able to handle inputs of at least
**        one million (1,000,000) rows.
**
**   (8)  If there are no non-NULL values for Y, then percentile(Y,P)
**        returns NULL.
**
**   (9)  If there is exactly one non-NULL value for Y, the percentile(Y,P)
**        returns the one Y value.
**
**  (10)  If there N non-NULL values of Y where N is two or more and
**        the Y values are ordered from least to greatest and a graph is
**        drawn from 0 to N-1 such that the height of the graph at J is
**        the J-th Y value and such that straight lines are drawn between
**        adjacent Y values, then the percentile(Y,P) function returns
**        the height of the graph at P*(N-1)/100.
**
**  (11)  The percentile(Y,P) function always returns either a floating
**        point number or NULL.
**
**  (12)  The percentile(Y,P) is implemented as a single C99 source-code
**        file that compiles into a shared-library or DLL that can be loaded
**        into SQLite using the capdb_load_extension() interface.
**
**  (13)  A separate median(Y) function is the equivalent percentile(Y,50).
**
**  (14)  A separate percentile_cont(Y,P) function is equivalent to
**        percentile(Y,P/100.0).  In other words, the fraction value in
**        the second argument is in the range of 0 to 1 instead of 0 to 100.
**
**  (15)  A separate percentile_disc(Y,P) function is like
**        percentile_cont(Y,P) except that instead of returning the weighted
**        average of the nearest two input values, it returns the next lower
**        value.  So the percentile_disc(Y,P) will always return a value
**        that was one of the inputs.
**
**  (16)  All of median(), percentile(Y,P), percentile_cont(Y,P) and
**        percentile_disc(Y,P) can be used as window functions.
**
** Differences from standard SQL:
**
**  *  The percentile_cont(X,P) function is equivalent to the following in
**     standard SQL:
**
**         (percentile_cont(P) WITHIN GROUP (ORDER BY X))
**
**     The SQLite syntax is much more compact.  The standard SQL syntax
**     is also supported if SQLite is compiled with the
**     -DCAPDB_ENABLE_ORDERED_SET_AGGREGATES option.
**
**  *  No median(X) function exists in the SQL standard.  App developers
**     are expected to write "percentile_cont(0.5)WITHIN GROUP(ORDER BY X)".
**
**  *  No percentile(Y,P) function exists in the SQL standard.  Instead of
**     percential(Y,P), developers must write this:
**     "percentile_cont(P/100.0) WITHIN GROUP (ORDER BY Y)".  Note that
**     the fraction parameter to percentile() goes from 0 to 100 whereas
**     the fraction parameter in SQL standard percentile_cont() goes from
**     0 to 1.
**
** Implementation notes as of 2024-08-31:
**
**  *  The regular aggregate-function versions of these routines work
**     by accumulating all values in an array of doubles, then sorting
**     that array using quicksort before computing the answer. Thus
**     the runtime is O(NlogN) where N is the number of rows of input.
**
**  *  For the window-function versions of these routines, the array of
**     inputs is sorted as soon as the first value is computed.  Thereafter,
**     the array is kept in sorted order using an insert-sort.  This
**     results in O(N*K) performance where K is the size of the window.
**     One can imagine alternative implementations that give O(N*logN*logK)
**     performance, but they require more complex logic and data structures.
**     The developers have elected to keep the asymptotically slower
**     algorithm for now, for simplicity, under the theory that window
**     functions are seldom used and when they are, the window size K is
**     often small.  The developers might revisit that decision later,
**     should the need arise.
*/

/* The following object is the group context for a single percentile()
** aggregate.  Remember all input Y values until the very end.
** Those values are accumulated in the Percentile.a[] array.
*/
typedef struct Percentile Percentile;
struct Percentile {
  u64 nAlloc;          /* Number of slots allocated for a[] */
  u64 nUsed;           /* Number of slots actually used in a[] */
  char bSorted;        /* True if a[] is already in sorted order */
  char bKeepSorted;    /* True if advantageous to keep a[] sorted */
  char bPctValid;      /* True if rPct is valid */
  double rPct;         /* Fraction.  0.0 to 1.0 */
  double *a;           /* Array of Y values */
};

/*
** Return TRUE if the input floating-point number is an infinity.
*/
static int percentIsInfinity(double r){
  capdb_uint64 u;
  assert( sizeof(u)==sizeof(r) );
  memcpy(&u, &r, sizeof(u));
  return ((u>>52)&0x7ff)==0x7ff;
}

/*
** Return TRUE if two doubles differ by 0.001 or less.
*/
static int percentSameValue(double a, double b){
  a -= b;
  return a>=-0.001 && a<=0.001;
}

/*
** Search p (which must have p->bSorted) looking for an entry with
** value y.  Return the index of that entry.
**
** If bExact is true, return -1 if the entry is not found.
**
** If bExact is false, return the index at which a new entry with
** value y should be insert in order to keep the values in sorted
** order.  The smallest return value in this case will be 0, and
** the largest return value will be p->nUsed.
*/
static i64 percentBinarySearch(Percentile *p, double y, int bExact){
  i64 iFirst = 0;                   /* First element of search range */
  i64 iLast = (i64)p->nUsed - 1;    /* Last element of search range */
  while( iLast>=iFirst ){
    i64 iMid = (iFirst+iLast)/2;
    double x = p->a[iMid];
    if( x<y ){
      iFirst = iMid + 1;
    }else if( x>y ){
      iLast = iMid - 1;
    }else{
      return iMid;
    }
  }
  if( bExact ) return -1;
  return iFirst;
}

/*
** Generate an error for a percentile function.
**
** The error format string must have exactly one occurrence of "%%s()"
** (with two '%' characters).  That substring will be replaced by the name
** of the function.
*/
static void percentError(capdb_context *pCtx, const char *zFormat, ...){
  char *zMsg1;
  char *zMsg2;
  va_list ap;

  va_start(ap, zFormat);
  zMsg1 = capdb_vmprintf(zFormat, ap);
  va_end(ap);
  zMsg2 = zMsg1 ? capdb_mprintf(zMsg1, capdbVdbeFuncName(pCtx)) : 0;
  capdb_result_error(pCtx, zMsg2, -1);
  capdb_free(zMsg1);
  capdb_free(zMsg2);
}

/*
** The "step" function for percentile(Y,P) is called once for each
** input row.
*/
static void percentStep(capdb_context *pCtx, int argc, capdb_value **argv){
  Percentile *p;
  double rPct;
  int eType;
  double y;
  assert( argc==2 || argc==1 );

  if( argc==1 ){
    /* Requirement 13:  median(Y) is the same as percentile(Y,50). */
    rPct = 0.5;
  }else{
    /* P must be a number between 0 and 100 for percentile() or between
    ** 0.0 and 1.0 for percentile_cont() and percentile_disc().
    **
    ** The user-data is an integer which is 10 times the upper bound.
    */
    double mxFrac = (CAPDB_PTR_TO_INT(capdb_user_data(pCtx))&2)? 100.0 : 1.0;
    eType = capdb_value_numeric_type(argv[1]);
    rPct = capdb_value_double(argv[1])/mxFrac;
    if( (eType!=CAPDB_INTEGER && eType!=CAPDB_FLOAT)
     || rPct<0.0 || rPct>1.0
    ){
      percentError(pCtx, "the fraction argument to %%s()"
                        " is not between 0.0 and %.1f",
                        (double)mxFrac);
      return;
    }
  }

  /* Allocate the session context. */
  p = (Percentile*)capdb_aggregate_context(pCtx, sizeof(*p));
  if( p==0 ) return;

  /* Remember the P value.  Throw an error if the P value is different
  ** from any prior row, per Requirement (2). */
  if( !p->bPctValid ){
    p->rPct = rPct;
    p->bPctValid = 1;
  }else if( !percentSameValue(p->rPct,rPct) ){
    percentError(pCtx, "the fraction argument to %%s()"
                      " is not the same for all input rows");
    return;
  }

  /* Ignore rows for which Y is NULL */
  eType = capdb_value_type(argv[0]);
  if( eType==CAPDB_NULL ) return;

  /* If not NULL, then Y must be numeric.  Otherwise throw an error.
  ** Requirement 4 */
  if( eType!=CAPDB_INTEGER && eType!=CAPDB_FLOAT ){
    percentError(pCtx, "input to %%s() is not numeric");
    return;
  }

  /* Throw an error if the Y value is infinity or NaN */
  y = capdb_value_double(argv[0]);
  if( percentIsInfinity(y) ){
    percentError(pCtx, "Inf input to %%s()");
    return;
  }

  /* Allocate and store the Y */
  if( p->nUsed>=p->nAlloc ){
    u64 n = p->nAlloc*2 + 250;
    double *a = capdb_realloc64(p->a, sizeof(double)*n);
    if( a==0 ){
      capdb_free(p->a);
      memset(p, 0, sizeof(*p));
      capdb_result_error_nomem(pCtx);
      return;
    }
    p->nAlloc = n;
    p->a = a;
  }
  if( p->nUsed==0 ){
    p->a[p->nUsed++] = y;
    p->bSorted = 1;
  }else if( !p->bSorted || y>=p->a[p->nUsed-1] ){
    p->a[p->nUsed++] = y;
  }else if( p->bKeepSorted ){
    i64 i;
    i = percentBinarySearch(p, y, 0);
    if( i<(int)p->nUsed ){
      memmove(&p->a[i+1], &p->a[i], (p->nUsed-i)*sizeof(p->a[0]));
    }
    p->a[i] = y;
    p->nUsed++;
  }else{
    p->a[p->nUsed++] = y;
    p->bSorted = 0;
  }
}

/*
** Interchange two doubles.
*/
#define SWAP_DOUBLE(X,Y)  {double ttt=(X);(X)=(Y);(Y)=ttt;}

/*
** Sort an array of doubles.
**
** Algorithm: quicksort
**
** This is implemented separately rather than using the qsort() routine
** from the standard library because:
**
**    (1)  To avoid a dependency on qsort()
**    (2)  To avoid the function call to the comparison routine for each
**         comparison.
*/
static void percentSort(double *a, unsigned int n){
  int iLt;  /* Entries before a[iLt] are less than rPivot */
  int iGt;  /* Entries at or after a[iGt] are greater than rPivot */
  int i;         /* Loop counter */
  double rPivot; /* The pivot value */

  assert( n>=2 );
  do{
    if( a[0]>a[n-1] ){
      SWAP_DOUBLE(a[0],a[n-1])
    }
    if( n==2 ) return;
    iGt = n-1;
    i = n/2;
    if( a[0]>a[i] ){
      SWAP_DOUBLE(a[0],a[i])
    }else if( a[i]>a[iGt] ){
      SWAP_DOUBLE(a[i],a[iGt])
    }
    if( n==3 ) return;
    rPivot = a[i];
    iLt = i = 1;
    do{
      if( a[i]<rPivot ){
        if( i>iLt ) SWAP_DOUBLE(a[i],a[iLt])
        iLt++;
        i++;
      }else if( a[i]>rPivot ){
        do{
          iGt--;
        }while( iGt>i && a[iGt]>rPivot );
        SWAP_DOUBLE(a[i],a[iGt])
      }else{
        i++;
      }
    }while( i<iGt );

    /* Recurse on the smaller partition only.  The smaller partition
    ** will hold n/2 or fewer entries, which assures that the stack
    ** depth will not exceed O(log(n)), even for pathological cases.
    ** Loop without recursion for the larger partition. */
    if( iLt>(int)(n/2) ){
      if( n-iGt>=2 ) percentSort(a+iGt, n-iGt);
      n = iLt;
    }else{
      if( iLt>=2 ) percentSort(a, iLt);
      a += iGt;
      n -= iGt;
    }
  }while( n>=2 );
}

/*
** The "inverse" function for percentile(Y,P) is called to remove a
** row that was previously inserted by "step".
*/
static void percentInverse(capdb_context *pCtx,int argc,capdb_value **argv){
  Percentile *p;
  int eType;
  double y;
  i64 i;
  assert( argc==2 || argc==1 );

  /* Allocate the session context. */
  p = (Percentile*)capdb_aggregate_context(pCtx, sizeof(*p));
  assert( p!=0 );

  /* Ignore rows for which Y is NULL */
  eType = capdb_value_type(argv[0]);
  if( eType==CAPDB_NULL ) return;

  /* If not NULL, then Y must be numeric.  Otherwise throw an error.
  ** Requirement 4 */
  if( eType!=CAPDB_INTEGER && eType!=CAPDB_FLOAT ){
    return;
  }

  /* Ignore the Y value if it is infinity or NaN */
  y = capdb_value_double(argv[0]);
  if( percentIsInfinity(y) ){
    return;
  }
  if( p->bSorted==0 ){
    assert( p->nUsed>1 );
    percentSort(p->a, p->nUsed);
    p->bSorted = 1;
  }
  p->bKeepSorted = 1;

  /* Find and remove the row */
  i = percentBinarySearch(p, y, 1);
  if( i>=0 ){
    p->nUsed--;
    if( i<(int)p->nUsed ){
      memmove(&p->a[i], &p->a[i+1], (p->nUsed - i)*sizeof(p->a[0]));
    }
  }
}

/*
** Compute the final output of percentile().  Clean up all allocated
** memory if and only if bIsFinal is true.
*/
static void percentCompute(capdb_context *pCtx, int bIsFinal){
  Percentile *p;
  int settings = CAPDB_PTR_TO_INT(capdb_user_data(pCtx))&1; /* Discrete? */
  unsigned i1, i2;
  double v1, v2;
  double ix, vx;
  p = (Percentile*)capdb_aggregate_context(pCtx, 0);
  if( p==0 ) return;
  if( p->a==0 ) return;
  if( p->nUsed ){
    if( p->bSorted==0 ){
      assert( p->nUsed>1 );
      percentSort(p->a, p->nUsed);
      p->bSorted = 1;
    }
    ix = p->rPct*(p->nUsed-1);
    i1 = (unsigned)ix;
    if( settings & 1 ){
      vx = p->a[i1];
    }else{
      i2 = ix==(double)i1 || i1==p->nUsed-1 ? i1 : i1+1;
      v1 = p->a[i1];
      v2 = p->a[i2];
      vx = v1 + (v2-v1)*(ix-i1);
    }
    capdb_result_double(pCtx, vx);
  }
  if( bIsFinal ){
    capdb_free(p->a);
    memset(p, 0, sizeof(*p));
  }else{
    p->bKeepSorted = 1;
  }
}
static void percentFinal(capdb_context *pCtx){
  percentCompute(pCtx, 1);
}
static void percentValue(capdb_context *pCtx){
  percentCompute(pCtx, 0);
}
/****** End of percentile family of functions ******/
#endif /* CAPDB_ENABLE_PERCENTILE */

#if defined(CAPDB_DEBUG) || defined(CAPDB_ENABLE_FILESTAT)
/*
** Implementation of sqlite_filestat(SCHEMA).
**
** Return JSON text that describes low-level debug/diagnostic information
** about the capdb_file object associated with SCHEMA.
*/
static void filestatFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb *db = capdb_context_db_handle(context);
  const char *zDbName;
  capdb_str *pStr;
  Btree *pBtree;

  zDbName = (const char*)capdb_value_text(argv[0]);
  pBtree = capdbDbNameToBtree(db, zDbName);
  if( pBtree ){
    Pager *pPager;
    capdb_file *fd;
    int rc;
    capdbBtreeEnter(pBtree);
    pPager = capdbBtreePager(pBtree);
    assert( pPager!=0 );
    fd = capdbPagerFile(pPager);
    pStr = capdb_str_new(db);
    if( capdb_str_errcode(pStr) ){
      capdb_result_error_nomem(context);
    }else{
      capdb_str_append(pStr, "{\"db\":", 6);
      rc = capdbOsFileControl(fd, CAPDB_FCNTL_FILESTAT, pStr);
      if( rc ) capdb_str_append(pStr, "null", 4);
      fd = capdbPagerJrnlFile(pPager);
      if( fd && fd->pMethods!=0 ){
        capdb_str_appendall(pStr, ",\"journal\":");
        rc = capdbOsFileControl(fd, CAPDB_FCNTL_FILESTAT, pStr);
        if( rc ) capdb_str_append(pStr, "null", 4);
      }
      capdb_str_append(pStr, "}", 1);
      capdb_result_text(context, capdb_str_finish(pStr), -1,
                          capdb_free);
    }
    capdbBtreeLeave(pBtree);
  }else{
    capdb_result_text(context, "{}", 2, CAPDB_STATIC);
  }
}
#endif /* CAPDB_DEBUG || CAPDB_ENABLE_FILESTAT */

#ifdef CAPDB_DEBUG
/*
** Implementation of fpdecode(x,y,z) function.
**
** x is a real number that is to be decoded.  y is the precision.
** z is the maximum real precision.  Return a string that shows the
** results of the capdbFpDecode() function.
**
** Used for testing and debugging only, specifically testing and debugging
** of the capdbFpDecode() function.  This SQL function does not appear
** in production builds.  This function is not an API and is subject to
** modification or removal in future versions of SQLite.
*/
static void fpdecodeFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  FpDecode s;
  double x;
  int y, z;
  char zBuf[100];
  UNUSED_PARAMETER(argc);
  assert( argc==3 );
  x = capdb_value_double(argv[0]);
  y = capdb_value_int(argv[1]);
  z = capdb_value_int(argv[2]);
  if( z<=0 ) z = 1;
  capdbFpDecode(&s, x, y, z);
  if( s.isSpecial==2 ){
    capdb_snprintf(sizeof(zBuf), zBuf, "NaN");
  }else{
    capdb_snprintf(sizeof(zBuf), zBuf, "%c%.*s/%d", s.sign, s.n, s.z, s.iDP);
  }
  capdb_result_text(context, zBuf, -1, CAPDB_TRANSIENT);
}
#endif /* CAPDB_DEBUG */

#ifdef CAPDB_DEBUG
/*
** Implementation of parseuri(uri,flags) function.
**
** Required Arguments:
**    "uri"        The URI to parse.
**    "flags"      Bitmask of flags, as if to capdb_open_v2().
**
** Additional arguments beyond the first two make calls to
** capdb_uri_key() for integers and capdb_uri_parameter for
** anything else.
**
** The result is a string showing the results of calling capdbParseUri().
**
** Used for testing and debugging only, specifically testing and debugging
** of the capdbParseUri() function.  This SQL function does not appear
** in production builds.  This function is not an API and is subject to
** modification or removal in future versions of SQLite.
*/
static void parseuriFunc(
  capdb_context *ctx,
  int argc,
  capdb_value **argv
){
  capdb_str *pResult;
  const char *zVfs;
  const char *zUri;
  unsigned int flgs;
  int rc;
  capdb_vfs *pVfs = 0;
  char *zFile = 0;
  char *zErr = 0;

  if( argc<2 ) return;
  pVfs = capdb_vfs_find(0);
  assert( pVfs );
  zVfs = pVfs->zName;
  zUri = (const char*)capdb_value_text(argv[0]);
  if( zUri==0 ) return;
  flgs = (unsigned int)capdb_value_int(argv[1]);
  rc = capdbParseUri(zVfs, zUri, &flgs, &pVfs, &zFile, &zErr);
  pResult = capdb_str_new(0);
  if( !capdb_str_errcode(pResult) ){
    int i;
    capdb_str_appendf(pResult, "rc=%d", rc);
    capdb_str_appendf(pResult, ", flags=0x%x", flgs);
    capdb_str_appendf(pResult, ", vfs=%Q", pVfs ? pVfs->zName: 0);
    capdb_str_appendf(pResult, ", err=%Q", zErr);
    capdb_str_appendf(pResult, ", file=%Q", zFile);
    if( zFile ){
      const char *z = zFile;
      z += capdbStrlen30(z)+1;
      while( z[0] ){
        capdb_str_appendf(pResult, ", %Q", z);
        z += capdbStrlen30(z)+1;
      }
      for(i=2; i<argc; i++){
        const char *zArg;
        if( capdb_value_type(argv[i])==CAPDB_INTEGER ){
          int k = capdb_value_int(argv[i]);
          capdb_str_appendf(pResult, ", '%d:%q'",k,capdb_uri_key(zFile, k));
        }else if( (zArg = (const char*)capdb_value_text(argv[i]))!=0 ){
          capdb_str_appendf(pResult, ", '%q:%q'",
                 zArg, capdb_uri_parameter(zFile,zArg));
        }else{
          capdb_str_appendf(pResult, ", NULL");
        }
      }
    }
    capdb_result_text(ctx, capdb_str_finish(pResult), -1, capdb_free);
  }
  capdb_free_filename(zFile);
  capdb_free(zErr);
}
#endif /* CAPDB_DEBUG */

/*
** All of the FuncDef structures in the aBuiltinFunc[] array above
** to the global function hash table.  This occurs at start-time (as
** a consequence of calling capdb_initialize()).
**
** After this routine runs
*/
void capdbRegisterBuiltinFunctions(void){
  /*
  ** The following array holds FuncDef structures for all of the functions
  ** defined in this file.
  **
  ** The array cannot be constant since changes are made to the
  ** FuncDef.pHash elements at start-time.  The elements of this array
  ** are read-only after initialization is complete.
  **
  ** For peak efficiency, put the most frequently used function last.
  */
  static FuncDef aBuiltinFunc[] = {
/***** Functions only available with CAPDB_TESTCTRL_INTERNAL_FUNCTIONS *****/
#if !defined(CAPDB_UNTESTABLE)
    TEST_FUNC(implies_nonnull_row, 2, INLINEFUNC_implies_nonnull_row, 0),
    TEST_FUNC(expr_compare,        2, INLINEFUNC_expr_compare,        0),
    TEST_FUNC(expr_implies_expr,   2, INLINEFUNC_expr_implies_expr,   0),
    TEST_FUNC(affinity,            1, INLINEFUNC_affinity,            0),
#endif /* !defined(CAPDB_UNTESTABLE) */
/***** Regular functions *****/
#ifdef CAPDB_SOUNDEX
    FUNCTION(soundex,            1, 0, 0, soundexFunc      ),
#endif
#ifndef CAPDB_OMIT_LOAD_EXTENSION
    SFUNCTION(load_extension,    1, 0, 0, loadExt          ),
    SFUNCTION(load_extension,    2, 0, 0, loadExt          ),
#endif
#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
    DFUNCTION(sqlite_compileoption_used,1, 0, 0, compileoptionusedFunc  ),
    DFUNCTION(sqlite_compileoption_get, 1, 0, 0, compileoptiongetFunc  ),
#endif /* CAPDB_OMIT_COMPILEOPTION_DIAGS */
    INLINE_FUNC(unlikely,        1, INLINEFUNC_unlikely, CAPDB_FUNC_UNLIKELY),
    INLINE_FUNC(likelihood,      2, INLINEFUNC_unlikely, CAPDB_FUNC_UNLIKELY),
    INLINE_FUNC(likely,          1, INLINEFUNC_unlikely, CAPDB_FUNC_UNLIKELY),
#ifdef CAPDB_ENABLE_OFFSET_SQL_FUNC
    INLINE_FUNC(sqlite_offset,   1, INLINEFUNC_sqlite_offset, 0 ),
#endif
#if defined(CAPDB_DEBUG) || defined(CAPDB_ENABLE_FILESTAT)
    FUNCTION(sqlite_filestat,    1, 0, 0, filestatFunc     ),
#endif
    FUNCTION(ltrim,              1, 1, 0, trimFunc         ),
    FUNCTION(ltrim,              2, 1, 0, trimFunc         ),
    FUNCTION(rtrim,              1, 2, 0, trimFunc         ),
    FUNCTION(rtrim,              2, 2, 0, trimFunc         ),
    FUNCTION(trim,               1, 3, 0, trimFunc         ),
    FUNCTION(trim,               2, 3, 0, trimFunc         ),
    FUNCTION(min,               -3, 0, 1, minmaxFunc       ),
    WAGGREGATE(min, 1, 0, 1, minmaxStep, minMaxFinalize, minMaxValue, 0,
                                 CAPDB_FUNC_MINMAX|CAPDB_FUNC_ANYORDER ),
    FUNCTION(max,               -3, 1, 1, minmaxFunc       ),
    WAGGREGATE(max, 1, 1, 1, minmaxStep, minMaxFinalize, minMaxValue, 0,
                                 CAPDB_FUNC_MINMAX|CAPDB_FUNC_ANYORDER ),
    FUNCTION2(typeof,            1, 0, 0, typeofFunc,  CAPDB_FUNC_TYPEOF),
    FUNCTION2(subtype,           1, 0, 0, subtypeFunc,
                                           CAPDB_FUNC_TYPEOF|CAPDB_SUBTYPE),
    FUNCTION2(length,            1, 0, 0, lengthFunc,  CAPDB_FUNC_LENGTH),
    FUNCTION2(octet_length,      1, 0, 0, bytelengthFunc,CAPDB_FUNC_BYTELEN),
    FUNCTION(instr,              2, 0, 0, instrFunc        ),
    FUNCTION(printf,            -1, 0, 0, printfFunc       ),
    FUNCTION(format,            -1, 0, 0, printfFunc       ),
    FUNCTION(unicode,            1, 0, 0, unicodeFunc      ),
    FUNCTION(char,              -1, 0, 0, charFunc         ),
    FUNCTION(abs,                1, 0, 0, absFunc          ),
#ifdef CAPDB_DEBUG
    FUNCTION(fpdecode,           3, 0, 0, fpdecodeFunc     ),
    FUNCTION(parseuri,          -1, 0, 0, parseuriFunc     ),
#endif
#ifndef CAPDB_OMIT_FLOATING_POINT
    FUNCTION(round,              1, 0, 0, roundFunc        ),
    FUNCTION(round,              2, 0, 0, roundFunc        ),
#endif
    FUNCTION(upper,              1, 0, 0, upperFunc        ),
    FUNCTION(lower,              1, 0, 0, lowerFunc        ),
    FUNCTION(hex,                1, 0, 0, hexFunc          ),
    FUNCTION(unhex,              1, 0, 0, unhexFunc        ),
    FUNCTION(unhex,              2, 0, 0, unhexFunc        ),
    FUNCTION(concat,            -3, 0, 0, concatFunc       ),
    FUNCTION(concat_ws,         -4, 0, 0, concatwsFunc     ),
    INLINE_FUNC(ifnull,          2, INLINEFUNC_coalesce, 0 ),
    VFUNCTION(random,            0, 0, 0, randomFunc       ),
    VFUNCTION(randomblob,        1, 0, 0, randomBlob       ),
    FUNCTION(nullif,             2, 0, 1, nullifFunc       ),
    DFUNCTION(sqlite_version,    0, 0, 0, versionFunc      ),
    DFUNCTION(sqlite_source_id,  0, 0, 0, sourceidFunc     ),
    FUNCTION(sqlite_log,         2, 0, 0, errlogFunc       ),
    FUNCTION(unistr,             1, 0, 0, unistrFunc       ),
    FUNCTION(quote,              1, 0, 0, quoteFunc        ),
    FUNCTION(unistr_quote,       1, 1, 0, quoteFunc        ),
    VFUNCTION(last_insert_rowid, 0, 0, 0, last_insert_rowid),
    VFUNCTION(changes,           0, 0, 0, changes          ),
    VFUNCTION(total_changes,     0, 0, 0, total_changes    ),
    FUNCTION(replace,            3, 0, 0, replaceFunc      ),
    FUNCTION(zeroblob,           1, 0, 0, zeroblobFunc     ),
    FUNCTION(substr,             2, 0, 0, substrFunc       ),
    FUNCTION(substr,             3, 0, 0, substrFunc       ),
    FUNCTION(substring,          2, 0, 0, substrFunc       ),
    FUNCTION(substring,          3, 0, 0, substrFunc       ),
    WAGGREGATE(sum,   1,0,0, sumStep, sumFinalize, sumFinalize, sumInverse, 0),
    WAGGREGATE(total, 1,0,0, sumStep,totalFinalize,totalFinalize,sumInverse, 0),
    WAGGREGATE(avg,   1,0,0, sumStep, avgFinalize, avgFinalize, sumInverse, 0),
    WAGGREGATE(count, 0,0,0, countStep,
        countFinalize, countFinalize, countInverse,
        CAPDB_FUNC_COUNT|CAPDB_FUNC_ANYORDER  ),
    WAGGREGATE(count, 1,0,0, countStep,
        countFinalize, countFinalize, countInverse, CAPDB_FUNC_ANYORDER ),
    WAGGREGATE(group_concat, 1, 0, 0, groupConcatStep,
        groupConcatFinalize, groupConcatValue, groupConcatInverse, 0),
    WAGGREGATE(group_concat, 2, 0, 0, groupConcatStep,
        groupConcatFinalize, groupConcatValue, groupConcatInverse, 0),
    WAGGREGATE(string_agg,   2, 0, 0, groupConcatStep,
        groupConcatFinalize, groupConcatValue, groupConcatInverse, 0),

#ifdef CAPDB_ENABLE_PERCENTILE
    WAGGREGATE(median,          1,   0,0, percentStep,
        percentFinal, percentValue, percentInverse,
        CAPDB_INNOCUOUS|CAPDB_SELFORDER1),
    WAGGREGATE(percentile,      2, 0x2,0, percentStep,
        percentFinal, percentValue, percentInverse,
        CAPDB_INNOCUOUS|CAPDB_SELFORDER1),
    WAGGREGATE(percentile_cont, 2,   0,0, percentStep,
        percentFinal, percentValue, percentInverse,
        CAPDB_INNOCUOUS|CAPDB_SELFORDER1),
    WAGGREGATE(percentile_disc, 2, 0x1,0, percentStep,
        percentFinal, percentValue, percentInverse,
        CAPDB_INNOCUOUS|CAPDB_SELFORDER1),
#endif /* CAPDB_ENABLE_PERCENTILE */
 
    LIKEFUNC(glob, 2, &globInfo, CAPDB_FUNC_LIKE|CAPDB_FUNC_CASE),
#ifdef CAPDB_CASE_SENSITIVE_LIKE
    LIKEFUNC(like, 2, &likeInfoAlt, CAPDB_FUNC_LIKE|CAPDB_FUNC_CASE),
    LIKEFUNC(like, 3, &likeInfoAlt, CAPDB_FUNC_LIKE|CAPDB_FUNC_CASE),
#else
    LIKEFUNC(like, 2, &likeInfoNorm, CAPDB_FUNC_LIKE),
    LIKEFUNC(like, 3, &likeInfoNorm, CAPDB_FUNC_LIKE),
#endif
#ifdef CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION
    FUNCTION(unknown,           -1, 0, 0, unknownFunc      ),
#endif
#ifdef CAPDB_ENABLE_MATH_FUNCTIONS
    MFUNCTION(ceil,              1, xCeil,     ceilingFunc ),
    MFUNCTION(ceiling,           1, xCeil,     ceilingFunc ),
    MFUNCTION(floor,             1, xFloor,    ceilingFunc ),
#if CAPDB_HAVE_C99_MATH_FUNCS
    MFUNCTION(trunc,             1, trunc,     ceilingFunc ),
#endif
    FUNCTION(ln,                 1, 0, 0,      logFunc     ),
    FUNCTION(log,                1, 1, 0,      logFunc     ),
    FUNCTION(log10,              1, 1, 0,      logFunc     ),
    FUNCTION(log2,               1, 2, 0,      logFunc     ),
    FUNCTION(log,                2, 0, 0,      logFunc     ),
    MFUNCTION(exp,               1, exp,       math1Func   ),
    MFUNCTION(pow,               2, pow,       math2Func   ),
    MFUNCTION(power,             2, pow,       math2Func   ),
    MFUNCTION(mod,               2, fmod,      math2Func   ),
    MFUNCTION(acos,              1, acos,      math1Func   ),
    MFUNCTION(asin,              1, asin,      math1Func   ),
    MFUNCTION(atan,              1, atan,      math1Func   ),
    MFUNCTION(atan2,             2, atan2,     math2Func   ),
    MFUNCTION(cos,               1, cos,       math1Func   ),
    MFUNCTION(sin,               1, sin,       math1Func   ),
    MFUNCTION(tan,               1, tan,       math1Func   ),
    MFUNCTION(cosh,              1, cosh,      math1Func   ),
    MFUNCTION(sinh,              1, sinh,      math1Func   ),
    MFUNCTION(tanh,              1, tanh,      math1Func   ),
#if CAPDB_HAVE_C99_MATH_FUNCS
    MFUNCTION(acosh,             1, acosh,     math1Func   ),
    MFUNCTION(asinh,             1, asinh,     math1Func   ),
    MFUNCTION(atanh,             1, atanh,     math1Func   ),
#endif
    MFUNCTION(sqrt,              1, sqrt,      math1Func   ),
    MFUNCTION(radians,           1, degToRad,  math1Func   ),
    MFUNCTION(degrees,           1, radToDeg,  math1Func   ),
    MFUNCTION(pi,                0, 0,         piFunc      ),
#endif /* CAPDB_ENABLE_MATH_FUNCTIONS */
    FUNCTION(sign,               1, 0, 0,      signFunc    ),
    INLINE_FUNC(coalesce,       -4, INLINEFUNC_coalesce, 0 ),
    INLINE_FUNC(iif,            -4, INLINEFUNC_iif,      0 ),
    INLINE_FUNC(if,             -4, INLINEFUNC_iif,      0 ),
  };
#ifndef CAPDB_OMIT_ALTERTABLE
  capdbAlterFunctions();
#endif
  capdbWindowFunctions();
  capdbRegisterDateTimeFunctions();
  capdbRegisterJsonFunctions();
  capdbInsertBuiltinFuncs(aBuiltinFunc, ArraySize(aBuiltinFunc));

#if 0  /* Enable to print out how the built-in functions are hashed */
  {
    int i;
    FuncDef *p;
    for(i=0; i<CAPDB_FUNC_HASH_SZ; i++){
      printf("FUNC-HASH %02d:", i);
      for(p=capdbBuiltinFunctions.a[i]; p; p=p->u.pHash){
        int n = capdbStrlen30(p->zName);
        int h = p->zName[0] + n;
        assert( p->funcFlags & CAPDB_FUNC_BUILTIN );
        printf(" %s(%d)", p->zName, h);
      }
      printf("\n");
    }
  }
#endif
}
