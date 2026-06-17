/*
** 2025-10-14
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
** Test the ability of run-time extension loading to use the
** very latest interfaces.
**
** Compile something like this:
**
** Linux:  gcc -g -fPIC shared testloadext.c -o testloadext.so
**
** Mac:    cc -g -fPIC -dynamiclib testloadext.c -o testloadext.dylib
**
** Win11:  cl testloadext.c -link -dll -out:testloadext.dll
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

/*
** Implementation of the set_errmsg(CODE,MSG) SQL function.
**
** Raise an error that has numeric code CODE and text message MSG
** using the capdb_set_errmsg() API.
*/
static void seterrmsgfunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb *db;
  char *zRes;
  int rc;
  assert( argc==2 );
  db = capdb_context_db_handle(context);
  rc = capdb_set_errmsg(db, 
     capdb_value_int(argv[0]),
     capdb_value_text(argv[1]));
  zRes = capdb_mprintf("%d %d %s",
              rc, capdb_errcode(db), capdb_errmsg(db));
  capdb_result_text64(context, zRes, strlen(zRes),
                        CAPDB_TRANSIENT, CAPDB_UTF8);
  capdb_free(zRes);
}

/*
** Implementation of the tempbuf_spill() SQL function.
**
** Return the value of CAPDB_DBSTATUS_TEMPBUF_SPILL.
*/
static void tempbuf_spill_func(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb *db;
  capdb_int64 iHi = 0, iCur = 0;
  int rc;
  int bReset;
  assert( argc==1 );
  bReset = capdb_value_int(argv[0]);
  db = capdb_context_db_handle(context);
  (void)capdb_db_status64(db, CAPDB_DBSTATUS_TEMPBUF_SPILL,
                            &iCur, &iHi, bReset);
  capdb_result_int64(context, iCur);
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_testloadext_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "set_errmsg", 2,
                   CAPDB_UTF8,
                   0, seterrmsgfunc, 0, 0);
  if( rc ) return rc;
  rc = capdb_create_function(db, "tempbuf_spill", 1,
                   CAPDB_UTF8,
                   0, tempbuf_spill_func, 0, 0);
  if( rc ) return rc;
  return CAPDB_OK;
}
