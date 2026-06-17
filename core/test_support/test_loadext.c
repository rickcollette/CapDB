/*
** 2006 June 14
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Test extension for testing the capdb_load_extension() function.
*/
#include <string.h>
#include "capdbext.h"
CAPDB_EXTENSION_INIT1

/*
** The half() SQL function returns half of its input value.
*/
static void halfFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_result_double(context, 0.5*capdb_value_double(argv[0]));
}

/*
** SQL functions to call the capdb_status function and return results.
*/
static void statusFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int op = 0, mx, cur, resetFlag, rc;
  if( capdb_value_type(argv[0])==CAPDB_INTEGER ){
    op = capdb_value_int(argv[0]);
  }else if( capdb_value_type(argv[0])==CAPDB_TEXT ){
    int i;
    const char *zName;
    static const struct {
      const char *zName;
      int op;
    } aOp[] = {
      { "MEMORY_USED",         CAPDB_STATUS_MEMORY_USED         },
      { "PAGECACHE_USED",      CAPDB_STATUS_PAGECACHE_USED      },
      { "PAGECACHE_OVERFLOW",  CAPDB_STATUS_PAGECACHE_OVERFLOW  },
      { "SCRATCH_USED",        CAPDB_STATUS_SCRATCH_USED        },
      { "SCRATCH_OVERFLOW",    CAPDB_STATUS_SCRATCH_OVERFLOW    },
      { "MALLOC_SIZE",         CAPDB_STATUS_MALLOC_SIZE         },
    };
    int nOp = sizeof(aOp)/sizeof(aOp[0]);
    zName = (const char*)capdb_value_text(argv[0]);
    for(i=0; i<nOp; i++){
      if( strcmp(aOp[i].zName, zName)==0 ){
        op = aOp[i].op;
        break;
      }
    }
    if( i>=nOp ){
      char *zMsg = capdb_mprintf("unknown status property: %s", zName);
      capdb_result_error(context, zMsg, -1);
      capdb_free(zMsg);
      return;
    }
  }else{
    capdb_result_error(context, "unknown status type", -1);
    return;
  }
  if( argc==2 ){
    resetFlag = capdb_value_int(argv[1]);
  }else{
    resetFlag = 0;
  }
  rc = capdb_status(op, &cur, &mx, resetFlag);
  if( rc!=CAPDB_OK ){
    char *zMsg = capdb_mprintf("capdb_status(%d,...) returns %d", op, rc);
    capdb_result_error(context, zMsg, -1);
    capdb_free(zMsg);
    return;
  } 
  if( argc==2 ){
    capdb_result_int(context, mx);
  }else{
    capdb_result_int(context, cur);
  }
}

/*
** Extension load function.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int testloadext_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int nErr = 0;
  CAPDB_EXTENSION_INIT2(pApi);
  nErr |= capdb_create_function(db, "half", 1, CAPDB_ANY, 0, halfFunc, 0, 0);
  nErr |= capdb_create_function(db, "capdb_status", 1, CAPDB_ANY, 0,
                          statusFunc, 0, 0);
  nErr |= capdb_create_function(db, "capdb_status", 2, CAPDB_ANY, 0,
                          statusFunc, 0, 0);
  return nErr ? CAPDB_ERROR : CAPDB_OK;
}

/*
** Another extension entry point. This one always fails.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int testbrokenext_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  char *zErr;
  CAPDB_EXTENSION_INIT2(pApi);
  zErr = capdb_mprintf("broken!");
  *pzErrMsg = zErr;
  return 1;
}
