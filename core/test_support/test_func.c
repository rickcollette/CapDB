/*
** 2008 March 19
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing all sorts of SQLite interfaces.  This code
** implements new SQL functions used by the test scripts.
*/
#include "capdb.h"
#include "tclsqlite.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "capdbInt.h"
#include "vdbeInt.h"

/*
** Allocate nByte bytes of space using capdb_malloc(). If the
** allocation fails, call capdb_result_error_nomem() to notify
** the database handle that malloc() has failed.
*/
static void *testContextMalloc(capdb_context *context, int nByte){
  char *z = capdb_malloc(nByte);
  if( !z && nByte>0 ){
    capdb_result_error_nomem(context);
  }
  return z;
}

/*
** This function generates a string of random characters.  Used for
** generating test data.
*/
static void randStr(capdb_context *context, int argc, capdb_value **argv){
  static const unsigned char zSrc[] = 
     "abcdefghijklmnopqrstuvwxyz"
     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
     "0123456789"
     ".-!,:*^+=_|?/<> ";
  int iMin, iMax, n, r, i;
  unsigned char zBuf[1000];

  /* It used to be possible to call randstr() with any number of arguments,
  ** but now it is registered with SQLite as requiring exactly 2.
  */
  assert(argc==2);

  iMin = capdb_value_int(argv[0]);
  if( iMin<0 ) iMin = 0;
  if( iMin>=sizeof(zBuf) ) iMin = sizeof(zBuf)-1;
  iMax = capdb_value_int(argv[1]);
  if( iMax<iMin ) iMax = iMin;
  if( iMax>=sizeof(zBuf) ) iMax = sizeof(zBuf)-1;
  n = iMin;
  if( iMax>iMin ){
    capdb_randomness(sizeof(r), &r);
    r &= 0x7fffffff;
    n += r%(iMax + 1 - iMin);
  }
  assert( n<sizeof(zBuf) );
  capdb_randomness(n, zBuf);
  for(i=0; i<n; i++){
    zBuf[i] = zSrc[zBuf[i]%(sizeof(zSrc)-1)];
  }
  zBuf[n] = 0;
  capdb_result_text(context, (char*)zBuf, n, CAPDB_TRANSIENT);
}

/*
** The following two SQL functions are used to test returning a text
** result with a destructor. Function 'test_destructor' takes one argument
** and returns the same argument interpreted as TEXT. A destructor is
** passed with the capdb_result_text() call.
**
** SQL function 'test_destructor_count' returns the number of outstanding 
** allocations made by 'test_destructor';
**
** WARNING: Not threadsafe.
*/
static int test_destructor_count_var = 0;
static void destructor(void *p){
  char *zVal = (char *)p;
  assert(zVal);
  zVal--;
  capdb_free(zVal);
  test_destructor_count_var--;
}
static void test_destructor(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  char *zVal;
  int len;
  
  test_destructor_count_var++;
  assert( nArg==1 );
  if( capdb_value_type(argv[0])==CAPDB_NULL ) return;
  len = capdb_value_bytes(argv[0]); 
  zVal = testContextMalloc(pCtx, len+3);
  if( !zVal ){
    return;
  }
  zVal[len+1] = 0;
  zVal[len+2] = 0;
  zVal++;
  memcpy(zVal, capdb_value_text(argv[0]), len);
  capdb_result_text(pCtx, zVal, -1, destructor);
}
#ifndef CAPDB_OMIT_UTF16
static void test_destructor16(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  char *zVal;
  int len;
  
  test_destructor_count_var++;
  assert( nArg==1 );
  if( capdb_value_type(argv[0])==CAPDB_NULL ) return;
  len = capdb_value_bytes16(argv[0]); 
  zVal = testContextMalloc(pCtx, len+3);
  if( !zVal ){
    return;
  }
  zVal[len+1] = 0;
  zVal[len+2] = 0;
  zVal++;
  memcpy(zVal, capdb_value_text16(argv[0]), len);
  capdb_result_text16(pCtx, zVal, -1, destructor);
}
#endif
static void test_destructor_count(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  capdb_result_int(pCtx, test_destructor_count_var);
}

/*
** The following aggregate function, test_agg_errmsg16(), takes zero 
** arguments. It returns the text value returned by the capdb_errmsg16()
** API function.
*/
#ifndef CAPDB_UNTESTABLE
void capdbBeginBenignMalloc(void);
void capdbEndBenignMalloc(void);
#else
  #define capdbBeginBenignMalloc()
  #define capdbEndBenignMalloc()
#endif
static void test_agg_errmsg16_step(capdb_context *a, int b,capdb_value **c){
}
static void test_agg_errmsg16_final(capdb_context *ctx){
#ifndef CAPDB_OMIT_UTF16
  const void *z;
  capdb * db = capdb_context_db_handle(ctx);
  capdb_aggregate_context(ctx, 2048);
  z = capdb_errmsg16(db);
  capdb_result_text16(ctx, z, -1, CAPDB_TRANSIENT);
#endif
}

/*
** Routines for testing the capdb_get_auxdata() and capdb_set_auxdata()
** interface.
**
** The test_auxdata() SQL function attempts to register each of its arguments
** as auxiliary data.  If there are no prior registrations of aux data for
** that argument (meaning the argument is not a constant or this is its first
** call) then the result for that argument is 0.  If there is a prior
** registration, the result for that argument is 1.  The overall result
** is the individual argument results separated by spaces.
*/
static void free_test_auxdata(void *p) {capdb_free(p);}
static void test_auxdata(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  int i;
  char *zRet = testContextMalloc(pCtx, nArg*2);
  if( !zRet ) return;
  memset(zRet, 0, nArg*2);
  for(i=0; i<nArg; i++){
    char const *z = (char*)capdb_value_text(argv[i]);
    if( z ){
      int n;
      char *zAux = capdb_get_auxdata(pCtx, i);
      if( zAux ){
        zRet[i*2] = '1';
        assert( strcmp(zAux,z)==0 );
      }else {
        zRet[i*2] = '0';
      }
      n = (int)strlen(z) + 1;
      zAux = testContextMalloc(pCtx, n);
      if( zAux ){
        memcpy(zAux, z, n);
        capdb_set_auxdata(pCtx, i, zAux, free_test_auxdata);
      }
      zRet[i*2+1] = ' ';
    }
  }
  capdb_result_text(pCtx, zRet, 2*nArg-1, free_test_auxdata);
}

/*
** A function to test error reporting from user functions. This function
** returns a copy of its first argument as the error message.  If the
** second argument exists, it becomes the error code.
*/
static void test_error(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  capdb_result_error(pCtx, (char*)capdb_value_text(argv[0]), -1);
  if( nArg==2 ){
    capdb_result_error_code(pCtx, capdb_value_int(argv[1]));
  }
}

/*
** Implementation of the counter(X) function.  If X is an integer
** constant, then the first invocation will return X.  The second X+1.
** and so forth.  Can be used (for example) to provide a sequence number
** in a result set.
*/
static void counterFunc(
  capdb_context *pCtx,   /* Function context */
  int nArg,                /* Number of function arguments */
  capdb_value **argv     /* Values for all function arguments */
){
  int *pCounter = (int*)capdb_get_auxdata(pCtx, 0);
  if( pCounter==0 ){
    pCounter = capdb_malloc( sizeof(*pCounter) );
    if( pCounter==0 ){
      capdb_result_error_nomem(pCtx);
      return;
    }
    *pCounter = capdb_value_int(argv[0]);
    capdb_set_auxdata(pCtx, 0, pCounter, capdb_free);
  }else{
    ++*pCounter;
  }
  capdb_result_int(pCtx, *pCounter);
}


/*
** This function takes two arguments.  It performance UTF-8/16 type
** conversions on the first argument then returns a copy of the second
** argument.
**
** This function is used in cases such as the following:
**
**      SELECT test_isolation(x,x) FROM t1;
**
** We want to verify that the type conversions that occur on the
** first argument do not invalidate the second argument.
*/
static void test_isolation(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
#ifndef CAPDB_OMIT_UTF16
  capdb_value_text16(argv[0]);
  capdb_value_text(argv[0]);
  capdb_value_text16(argv[0]);
  capdb_value_text(argv[0]);
#endif
  capdb_result_value(pCtx, argv[1]);
}

/*
** Invoke an SQL statement recursively.  The function result is the 
** first column of the first row of the result set.
*/
static void test_eval(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  capdb_stmt *pStmt;
  int rc;
  capdb *db = capdb_context_db_handle(pCtx);
  const char *zSql;

  zSql = (char*)capdb_value_text(argv[0]);
  rc = capdb_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_step(pStmt);
    if( rc==CAPDB_ROW ){
      capdb_result_value(pCtx, capdb_column_value(pStmt, 0));
    }
    rc = capdb_finalize(pStmt);
  }
  if( rc ){
    char *zErr;
    assert( pStmt==0 );
    zErr = capdb_mprintf("capdb_prepare_v2() error: %s",capdb_errmsg(db));
    capdb_result_text(pCtx, zErr, -1, capdb_free);
    capdb_result_error_code(pCtx, rc);
  }
}


/*
** convert one character from hex to binary
*/
static int testHexChar(char c){
  if( c>='0' && c<='9' ){
    return c - '0';
  }else if( c>='a' && c<='f' ){
    return c - 'a' + 10;
  }else if( c>='A' && c<='F' ){
    return c - 'A' + 10;
  }
  return 0;
}

/*
** Convert hex to binary.
*/
static void testHexToBin(const char *zIn, char *zOut){
  while( zIn[0] && zIn[1] ){
    *(zOut++) = (testHexChar(zIn[0])<<4) + testHexChar(zIn[1]);
    zIn += 2;
  }
}

/*
**      hex_to_utf16be(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using capdb_result_text16le().
*/
#ifndef CAPDB_OMIT_UTF16
static void testHexToUtf16be(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  int n;
  const char *zIn;
  char *zOut;
  assert( nArg==1 );
  n = capdb_value_bytes(argv[0]);
  zIn = (const char*)capdb_value_text(argv[0]);
  zOut = capdb_malloc( n/2 );
  if( zOut==0 ){
    capdb_result_error_nomem(pCtx);
  }else{
    testHexToBin(zIn, zOut);
    capdb_result_text16be(pCtx, zOut, n/2, capdb_free);
  }
}
#endif

/*
**      hex_to_utf8(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using capdb_result_text16le().
*/
static void testHexToUtf8(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  int n;
  const char *zIn;
  char *zOut;
  assert( nArg==1 );
  n = capdb_value_bytes(argv[0]);
  zIn = (const char*)capdb_value_text(argv[0]);
  zOut = capdb_malloc( n/2 );
  if( zOut==0 ){
    capdb_result_error_nomem(pCtx);
  }else{
    testHexToBin(zIn, zOut);
    capdb_result_text(pCtx, zOut, n/2, capdb_free);
  }
}

/*
**      hex_to_utf16le(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using capdb_result_text16le().
*/
#ifndef CAPDB_OMIT_UTF16
static void testHexToUtf16le(
  capdb_context *pCtx, 
  int nArg,
  capdb_value **argv
){
  int n;
  const char *zIn;
  char *zOut;
  assert( nArg==1 );
  n = capdb_value_bytes(argv[0]);
  zIn = (const char*)capdb_value_text(argv[0]);
  zOut = capdb_malloc( n/2 );
  if( zOut==0 ){
    capdb_result_error_nomem(pCtx);
  }else{
    testHexToBin(zIn, zOut);
    capdb_result_text16le(pCtx, zOut, n/2, capdb_free);
  }
}
#endif

/*
** SQL function:   real2hex(X)
**
** If argument X is a real number, then convert it into a string which is
** the big-endian hexadecimal representation of the ieee754 encoding of
** that number.  If X is not a real number, return NULL.
*/
static void real2hex(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  union {
    capdb_uint64 i;
    double r;
    unsigned char x[8];
  } v;
  char zOut[20];
  int i;
  int bigEndian;
  v.i = 1;
  bigEndian = v.x[0]==0;
  v.r = capdb_value_double(argv[0]);
  for(i=0; i<8; i++){
    if( bigEndian ){
      zOut[i*2]   = "0123456789abcdef"[v.x[i]>>4];
      zOut[i*2+1] = "0123456789abcdef"[v.x[i]&0xf];
    }else{
      zOut[14-i*2]   = "0123456789abcdef"[v.x[i]>>4];
      zOut[14-i*2+1] = "0123456789abcdef"[v.x[i]&0xf];
    }
  }
  zOut[16] = 0;
  capdb_result_text(context, zOut, -1, CAPDB_TRANSIENT);
}

/*
**     test_extract(record, field)
**
** This function implements an SQL user-function that accepts a blob
** containing a formatted database record as the first argument. The
** second argument is the index of the field within that record to
** extract and return.
*/
static void test_extract(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb *db = capdb_context_db_handle(context);
  u8 *pRec;
  u8 *pEndHdr;                    /* Points to one byte past record header */
  u8 *pHdr;                       /* Current point in record header */
  u8 *pBody;                      /* Current point in record data */
  u64 nHdr;                       /* Bytes in record header */
  int iIdx;                       /* Required field */
  int iCurrent = 0;               /* Current field */

  assert( argc==2 );
  pRec = (u8*)capdb_value_blob(argv[0]);
  iIdx = capdb_value_int(argv[1]);

  pHdr = pRec + capdbGetVarint(pRec, &nHdr);
  pBody = pEndHdr = &pRec[nHdr];

  for(iCurrent=0; pHdr<pEndHdr && iCurrent<=iIdx; iCurrent++){
    u64 iSerialType;
    Mem mem;

    memset(&mem, 0, sizeof(mem));
    mem.db = db;
    mem.enc = ENC(db);
    pHdr += capdbGetVarint(pHdr, &iSerialType);
    capdbVdbeSerialGet(pBody, (u32)iSerialType, &mem);
    pBody += capdbVdbeSerialTypeLen((u32)iSerialType);

    if( iCurrent==iIdx ){
      capdb_result_value(context, &mem);
    }

    if( mem.szMalloc ) capdbDbFree(db, mem.zMalloc);
  }
}

/*
**      test_decode(record)
**
** This function implements an SQL user-function that accepts a blob
** containing a formatted database record as its only argument. It returns
** a tcl list (type CAPDB_TEXT) containing each of the values stored
** in the record.
*/
static void test_decode(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb *db = capdb_context_db_handle(context);
  u8 *pRec;
  u8 *pEndHdr;                    /* Points to one byte past record header */
  u8 *pHdr;                       /* Current point in record header */
  u8 *pBody;                      /* Current point in record data */
  u64 nHdr;                       /* Bytes in record header */
  Tcl_Obj *pRet;                  /* Return value */

  pRet = Tcl_NewObj();
  Tcl_IncrRefCount(pRet);

  assert( argc==1 );
  pRec = (u8*)capdb_value_blob(argv[0]);

  pHdr = pRec + capdbGetVarint(pRec, &nHdr);
  pBody = pEndHdr = &pRec[nHdr];
  while( pHdr<pEndHdr ){
    Tcl_Obj *pVal = 0;
    u64 iSerialType;
    Mem mem;

    memset(&mem, 0, sizeof(mem));
    mem.db = db;
    mem.enc = ENC(db);
    pHdr += capdbGetVarint(pHdr, &iSerialType);
    capdbVdbeSerialGet(pBody, (u32)iSerialType, &mem);
    pBody += capdbVdbeSerialTypeLen((u32)iSerialType);

    switch( capdb_value_type(&mem) ){
      case CAPDB_TEXT:
        pVal = Tcl_NewStringObj((const char*)capdb_value_text(&mem), -1);
        break;

      case CAPDB_BLOB: {
        char hexdigit[] = {
          '0', '1', '2', '3', '4', '5', '6', '7',
          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
        };
        int n = capdb_value_bytes(&mem);
        u8 *z = (u8*)capdb_value_blob(&mem);
        int i;
        pVal = Tcl_NewStringObj("x'", -1);
        for(i=0; i<n; i++){
          char hex[3];
          hex[0] = hexdigit[((z[i] >> 4) & 0x0F)];
          hex[1] = hexdigit[(z[i] & 0x0F)];
          hex[2] = '\0';
          Tcl_AppendStringsToObj(pVal, hex, 0);
        }
        Tcl_AppendStringsToObj(pVal, "'", 0);
        break;
      }

      case CAPDB_FLOAT:
        pVal = Tcl_NewDoubleObj(capdb_value_double(&mem));
        break;

      case CAPDB_INTEGER:
        pVal = Tcl_NewWideIntObj(capdb_value_int64(&mem));
        break;

      case CAPDB_NULL:
        pVal = Tcl_NewStringObj("NULL", -1);
        break;

      default:
        assert( 0 );
    }

    Tcl_ListObjAppendElement(0, pRet, pVal);

    if( mem.szMalloc ){
      capdbDbFree(db, mem.zMalloc);
    }
  }

  capdb_result_text(context, Tcl_GetString(pRet), -1, CAPDB_TRANSIENT);
  Tcl_DecrRefCount(pRet);
}

/*
**       test_zeroblob(N)
**
** The implementation of scalar SQL function "test_zeroblob()". This is
** similar to the built-in zeroblob() function, except that it does not
** check that the integer parameter is within range before passing it
** to capdb_result_zeroblob().
*/
static void test_zeroblob(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int nZero = capdb_value_int(argv[0]);
  capdb_result_zeroblob(context, nZero);
}

/*         test_getsubtype(V)
**
** Return the subtype for value V.
*/
static void test_getsubtype(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_result_int(context, (int)capdb_value_subtype(argv[0]));
}

/*         test_frombind(A,B,C,...)
**
** Return an integer bitmask that has a bit set for every argument
** (up to the first 63 arguments) that originates from a bind a parameter.
*/
static void test_frombind(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_uint64 m = 0;
  int i;
  for(i=0; i<argc && i<63; i++){
    if( capdb_value_frombind(argv[i]) ) m |= ((capdb_uint64)1)<<i;
  }
  capdb_result_int64(context, (capdb_int64)m);
}

/*         test_setsubtype(V, T)
**
** Return the value V with its subtype changed to T
*/
static void test_setsubtype(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_result_value(context, argv[0]);
  capdb_result_subtype(context, (unsigned int)capdb_value_int(argv[1]));
}

static int registerTestFunctions(
  capdb *db,
  char **pzErrMsg,
  const capdb_api_routines *pThunk
){
  static const struct {
     char *zName;
     signed char nArg;
     unsigned int eTextRep; /* 1: UTF-16.  0: UTF-8 */
     void (*xFunc)(capdb_context*,int,capdb_value **);
  } aFuncs[] = {
    { "randstr",               2, CAPDB_UTF8, randStr    },
    { "test_destructor",       1, CAPDB_UTF8, test_destructor},
#ifndef CAPDB_OMIT_UTF16
    { "test_destructor16",     1, CAPDB_UTF8, test_destructor16},
    { "hex_to_utf16be",        1, CAPDB_UTF8, testHexToUtf16be},
    { "hex_to_utf16le",        1, CAPDB_UTF8, testHexToUtf16le},
#endif
    { "hex_to_utf8",           1, CAPDB_UTF8, testHexToUtf8},
    { "test_destructor_count", 0, CAPDB_UTF8, test_destructor_count},
    { "test_auxdata",         -1, CAPDB_UTF8, test_auxdata},
    { "test_error",            1, CAPDB_UTF8, test_error},
    { "test_error",            2, CAPDB_UTF8, test_error},
    { "test_eval",             1, CAPDB_UTF8, test_eval},
    { "test_isolation",        2, CAPDB_UTF8, test_isolation},
    { "test_counter",          1, CAPDB_UTF8, counterFunc},
    { "real2hex",              1, CAPDB_UTF8, real2hex},
    { "test_decode",           1, CAPDB_UTF8, test_decode},
    { "test_extract",          2, CAPDB_UTF8, test_extract},
    { "test_zeroblob",  1, CAPDB_UTF8|CAPDB_DETERMINISTIC, test_zeroblob},
    { "test_getsubtype",       1, CAPDB_UTF8, test_getsubtype},
    { "test_setsubtype",       2, CAPDB_UTF8|CAPDB_RESULT_SUBTYPE,
                                               test_setsubtype},
    { "test_frombind",        -1, CAPDB_UTF8, test_frombind},
  };
  int i;

  for(i=0; i<sizeof(aFuncs)/sizeof(aFuncs[0]); i++){
    capdb_create_function(db, aFuncs[i].zName, aFuncs[i].nArg,
        aFuncs[i].eTextRep, 0, aFuncs[i].xFunc, 0, 0);
  }

  capdb_create_function(db, "test_agg_errmsg16", 0, CAPDB_ANY, 0, 0, 
      test_agg_errmsg16_step, test_agg_errmsg16_final);
      
  return CAPDB_OK;
}

/*
** TCLCMD:  autoinstall_test_functions
**
** Invoke this TCL command to use capdb_auto_extension() to cause
** the standard set of test functions to be loaded into each new
** database connection.
*/
static int CAPDB_TCLAPI autoinstall_test_funcs(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int Md5_Register(capdb *, char **, const capdb_api_routines *);
  int rc = capdb_auto_extension((void(*)(void))registerTestFunctions);
  if( rc==CAPDB_OK ){
    rc = capdb_auto_extension((void(*)(void))Md5_Register);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** A bogus step function and finalizer function.
*/
static void tStep(capdb_context *a, int b, capdb_value **c){}
static void tFinal(capdb_context *a){}


/*
** tclcmd:  abuse_create_function
**
** Make various calls to capdb_create_function that do not have valid
** parameters.  Verify that the error condition is detected and reported.
*/
static int CAPDB_TCLAPI abuse_create_function(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int getDbPointer(Tcl_Interp*, const char*, capdb**);
  capdb *db;
  int rc;
  int mxArg;

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  rc = capdb_create_function(db, "tx", 1, CAPDB_UTF8, 0, tStep,tStep,tFinal);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  rc = capdb_create_function(db, "tx", 1, CAPDB_UTF8, 0, tStep, tStep, 0);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  rc = capdb_create_function(db, "tx", 1, CAPDB_UTF8, 0, tStep, 0, tFinal);
  if( rc!=CAPDB_MISUSE) goto abuse_err;

  rc = capdb_create_function(db, "tx", 1, CAPDB_UTF8, 0, 0, 0, tFinal);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  rc = capdb_create_function(db, "tx", 1, CAPDB_UTF8, 0, 0, tStep, 0);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  rc = capdb_create_function(db, "tx", -2, CAPDB_UTF8, 0, tStep, 0, 0);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  rc = capdb_create_function(db, "tx", 32768, CAPDB_UTF8, 0, tStep, 0, 0);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  rc = capdb_create_function(db, "funcxx"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789",
       1, CAPDB_UTF8, 0, tStep, 0, 0);
  if( rc!=CAPDB_MISUSE ) goto abuse_err;

  /* This last function registration should actually work.  Generate
  ** a no-op function (that always returns NULL) and which has the
  ** maximum-length function name and the maximum number of parameters.
  */
  capdb_limit(db, CAPDB_LIMIT_FUNCTION_ARG, 1000000);
  mxArg = capdb_limit(db, CAPDB_LIMIT_FUNCTION_ARG, -1);
  rc = capdb_create_function(db, "nullx"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789",
       mxArg, CAPDB_UTF8, 0, tStep, 0, 0);
  if( rc!=CAPDB_OK ) goto abuse_err;
                                
  return TCL_OK;

abuse_err:
  Tcl_AppendResult(interp, "capdb_create_function abused test failed", 
                   (char*)0);
  return TCL_ERROR;
}


/*
** SQLite user defined function to use with matchinfo() to calculate the
** relevancy of an FTS match. The value returned is the relevancy score
** (a real value greater than or equal to zero). A larger value indicates 
** a more relevant document.
**
** The overall relevancy returned is the sum of the relevancies of each 
** column value in the FTS table. The relevancy of a column value is the
** sum of the following for each reportable phrase in the FTS query:
**
**   (<hit count> / <global hit count>) * <column weight>
**
** where <hit count> is the number of instances of the phrase in the
** column value of the current row and <global hit count> is the number
** of instances of the phrase in the same column of all rows in the FTS
** table. The <column weight> is a weighting factor assigned to each
** column by the caller (see below).
**
** The first argument to this function must be the return value of the FTS 
** matchinfo() function. Following this must be one argument for each column 
** of the FTS table containing a numeric weight factor for the corresponding 
** column. Example:
**
**     CREATE VIRTUAL TABLE documents USING fts3(title, content)
**
** The following query returns the docids of documents that match the full-text
** query <query> sorted from most to least relevant. When calculating
** relevance, query term instances in the 'title' column are given twice the
** weighting of those in the 'content' column.
**
**     SELECT docid FROM documents 
**     WHERE documents MATCH <query> 
**     ORDER BY rank(matchinfo(documents), 1.0, 0.5) DESC
*/
static void rankfunc(capdb_context *pCtx, int nVal, capdb_value **apVal){
  int *aMatchinfo;                /* Return value of matchinfo() */
  int nMatchinfo;                 /* Number of elements in aMatchinfo[] */
  int nCol = 0;                   /* Number of columns in the table */
  int nPhrase = 0;                /* Number of phrases in the query */
  int iPhrase;                    /* Current phrase */
  double score = 0.0;             /* Value to return */

  assert( sizeof(int)==4 );

  /* Check that the number of arguments passed to this function is correct.
  ** If not, jump to wrong_number_args. Set aMatchinfo to point to the array
  ** of unsigned integer values returned by FTS function matchinfo. Set
  ** nPhrase to contain the number of reportable phrases in the users full-text
  ** query, and nCol to the number of columns in the table. Then check that the
  ** size of the matchinfo blob is as expected. Return an error if it is not.
  */
  if( nVal<1 ) goto wrong_number_args;
  aMatchinfo = (int*)capdb_value_blob(apVal[0]);
  nMatchinfo = capdb_value_bytes(apVal[0]) / sizeof(int);
  if( nMatchinfo>=2 ){
    nPhrase = aMatchinfo[0];
    nCol = aMatchinfo[1];
  }
  if( nMatchinfo!=(2+3*nCol*nPhrase) ){
    capdb_result_error(pCtx,
        "invalid matchinfo blob passed to function rank()", -1);
    return;
  }
  if( nVal!=(1+nCol) ) goto wrong_number_args;

  /* Iterate through each phrase in the users query. */
  for(iPhrase=0; iPhrase<nPhrase; iPhrase++){
    int iCol;                     /* Current column */

    /* Now iterate through each column in the users query. For each column,
    ** increment the relevancy score by:
    **
    **   (<hit count> / <global hit count>) * <column weight>
    **
    ** aPhraseinfo[] points to the start of the data for phrase iPhrase. So
    ** the hit count and global hit counts for each column are found in 
    ** aPhraseinfo[iCol*3] and aPhraseinfo[iCol*3+1], respectively.
    */
    int *aPhraseinfo = &aMatchinfo[2 + iPhrase*nCol*3];
    for(iCol=0; iCol<nCol; iCol++){
      int nHitCount = aPhraseinfo[3*iCol];
      int nGlobalHitCount = aPhraseinfo[3*iCol+1];
      double weight = capdb_value_double(apVal[iCol+1]);
      if( nHitCount>0 ){
        score += ((double)nHitCount / (double)nGlobalHitCount) * weight;
      }
    }
  }

  capdb_result_double(pCtx, score);
  return;

  /* Jump here if the wrong number of arguments are passed to this function */
wrong_number_args:
  capdb_result_error(pCtx, "wrong number of arguments to function rank()", -1);
}

static int CAPDB_TCLAPI install_fts3_rank_function(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int getDbPointer(Tcl_Interp*, const char*, capdb**);
  capdb *db;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  capdb_create_function(db, "rank", -1, CAPDB_UTF8, 0, rankfunc, 0, 0);
  return TCL_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest_func_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
  } aObjCmd[] = {
     { "autoinstall_test_functions",    autoinstall_test_funcs },
     { "abuse_create_function",         abuse_create_function  },
     { "install_fts3_rank_function",    install_fts3_rank_function  },
  };
  int i;
  extern int Md5_Register(capdb *, char **, const capdb_api_routines *);

  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, 0, 0);
  }
  capdb_initialize();
  capdb_auto_extension((void(*)(void))registerTestFunctions);
  capdb_auto_extension((void(*)(void))Md5_Register);
  return TCL_OK;
}
