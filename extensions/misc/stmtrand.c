/*
** 2024-05-24
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
** An SQL function that return pseudo-random non-negative integers.
**
**      SELECT stmtrand(123);
**
** A special feature of this function is that the same sequence of random
** integers is returned for each invocation of the statement.  This makes
** the results repeatable, and hence useful for testing.  The argument is
** an integer which is the seed for the random number sequence.  The seed
** is used by the first invocation of this function only and is ignored
** for all subsequent calls within the same statement.
**
** Resetting a statement (capdb_reset()) also resets the random number
** sequence.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

/* State of the pseudo-random number generator */
typedef struct Stmtrand {
  unsigned int x, y;
} Stmtrand;

/* auxdata key */
#define STMTRAND_KEY  (-4418371)

/*
** Function:     stmtrand(SEED)
**
** Return a pseudo-random number.
*/
static void stmtrandFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  Stmtrand *p;

  p = (Stmtrand*)capdb_get_auxdata(context, STMTRAND_KEY);
  if( p==0 ){
    unsigned int seed;
    p = capdb_malloc64( sizeof(*p) );
    if( p==0 ){
      capdb_result_error_nomem(context);
      return;
    }
    if( argc>=1 ){
      seed = (unsigned int)capdb_value_int(argv[0]);
    }else{
      seed = 0;
    }
    p->x = seed | 1;
    p->y = seed;
    capdb_set_auxdata(context, STMTRAND_KEY, p, capdb_free);
    p = (Stmtrand*)capdb_get_auxdata(context, STMTRAND_KEY);
    if( p==0 ){
      capdb_result_error_nomem(context);
      return;
    }
  }
  p->x = (p->x>>1) ^ ((1+~(p->x&1)) & 0xd0000001);
  p->y = p->y*1103515245 + 12345;
  capdb_result_int(context, (int)((p->x ^ p->y)&0x7fffffff));
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_stmtrand_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "stmtrand", 1, CAPDB_UTF8, 0,
                               stmtrandFunc, 0, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "stmtrand", 0, CAPDB_UTF8, 0,
                                 stmtrandFunc, 0, 0);
  }
  return rc;
}
