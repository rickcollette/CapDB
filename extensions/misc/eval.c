/*
** 2014-11-10
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
** This SQLite extension implements SQL function eval() which runs
** SQL statements recursively.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <string.h>

/*
** Structure used to accumulate the output
*/
struct EvalResult {
  char *z;               /* Accumulated output */
  const char *zSep;      /* Separator */
  int szSep;             /* Size of the separator string */
  capdb_int64 nAlloc;  /* Number of bytes allocated for z[] */
  capdb_int64 nUsed;   /* Number of bytes of z[] actually used */
};

/*
** Callback from sqlite_exec() for the eval() function.
*/
static int callback(void *pCtx, int argc, char **argv, char **colnames){
  struct EvalResult *p = (struct EvalResult*)pCtx;
  int i; 
  if( argv==0 ) return 0;
  for(i=0; i<argc; i++){
    const char *z = argv[i] ? argv[i] : "";
    size_t sz = strlen(z);
    if( (capdb_int64)sz+p->nUsed+p->szSep+1 > p->nAlloc ){
      char *zNew;
      p->nAlloc = p->nAlloc*2 + sz + p->szSep + 1;
      /* Using capdb_realloc64() would be better, but it is a recent
      ** addition and will cause a segfault if loaded by an older version
      ** of SQLite.  */
      zNew = p->nAlloc<=0x7fffffff ? capdb_realloc64(p->z, p->nAlloc) : 0;
      if( zNew==0 ){
        capdb_free(p->z);
        memset(p, 0, sizeof(*p));
        return 1;
      }
      p->z = zNew;
    }
    if( p->nUsed>0 ){
      memcpy(&p->z[p->nUsed], p->zSep, p->szSep);
      p->nUsed += p->szSep;
    }
    memcpy(&p->z[p->nUsed], z, sz);
    p->nUsed += sz;
  }
  return 0;
}

/*
** Implementation of the eval(X) and eval(X,Y) SQL functions.
**
** Evaluate the SQL text in X.  Return the results, using string
** Y as the separator.  If Y is omitted, use a single space character.
*/
static void sqlEvalFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSql;
  capdb *db;
  char *zErr = 0;
  int rc;
  struct EvalResult x;

  memset(&x, 0, sizeof(x));
  x.zSep = " ";
  zSql = (const char*)capdb_value_text(argv[0]);
  if( zSql==0 ) return;
  if( argc>1 ){
    x.zSep = (const char*)capdb_value_text(argv[1]);
    if( x.zSep==0 ) return;
  }
  x.szSep = (int)strlen(x.zSep);
  db = capdb_context_db_handle(context);
  rc = capdb_exec(db, zSql, callback, &x, &zErr);
  if( rc!=CAPDB_OK ){
    capdb_result_error(context, zErr, -1);
    capdb_free(zErr);
  }else if( x.zSep==0 ){
    capdb_result_error_nomem(context);
    capdb_free(x.z);
  }else{
    capdb_result_text(context, x.z, (int)x.nUsed, capdb_free);
  }
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_eval_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "eval", 1, 
                               CAPDB_UTF8|CAPDB_DIRECTONLY, 0,
                               sqlEvalFunc, 0, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "eval", 2,
                                 CAPDB_UTF8|CAPDB_DIRECTONLY, 0,
                                 sqlEvalFunc, 0, 0);
  }
  return rc;
}
