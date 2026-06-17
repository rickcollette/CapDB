/*
** This module interfaces SQLite to the Google OSS-Fuzz, fuzzer as a service.
** (https://github.com/google/oss-fuzz)
*/
#include <stddef.h>
#if !defined(_MSC_VER)
# include <stdint.h>
#endif
#include <stdio.h>
#include <string.h>
#include "capdb.h"

#if defined(_MSC_VER)
typedef unsigned char uint8_t;
#endif

/* Global debugging settings.  OSS-Fuzz will have all debugging turned
** off.  But if LLVMFuzzerTestOneInput() is called interactively from
** the ossshell utility program, then these flags might be set.
*/
static unsigned mDebug = 0;
#define FUZZ_SQL_TRACE       0x0001   /* Set an capdb_trace() callback */
#define FUZZ_SHOW_MAX_DELAY  0x0002   /* Show maximum progress callback delay */
#define FUZZ_SHOW_ERRORS     0x0004   /* Print error messages from SQLite */

/* The ossshell utility program invokes this interface to see the
** debugging flags.  Unused by OSS-Fuzz.
*/
void ossfuzz_set_debug_flags(unsigned x){
  mDebug = x;
}

/* Return the current real-world time in milliseconds since the
** Julian epoch (-4714-11-24).
*/
static capdb_int64 timeOfDay(void){
  static capdb_vfs *clockVfs = 0;
  capdb_int64 t;
  if( clockVfs==0 ){
    clockVfs = capdb_vfs_find(0);
    if( clockVfs==0 ) return 0;
  }
  if( clockVfs->iVersion>=2 && clockVfs->xCurrentTimeInt64!=0 ){
    clockVfs->xCurrentTimeInt64(clockVfs, &t);
  }else{
    double r;
    clockVfs->xCurrentTime(clockVfs, &r);
    t = (capdb_int64)(r*86400000.0);
  }
  return t;
}

/* An instance of the following object is passed by pointer as the
** client data to various callbacks.
*/
typedef struct FuzzCtx {
  capdb *db;               /* The database connection */
  capdb_int64 iCutoffTime; /* Stop processing at this time. */
  capdb_int64 iLastCb;     /* Time recorded for previous progress callback */
  capdb_int64 mxInterval;  /* Longest interval between two progress calls */
  unsigned nCb;              /* Number of progress callbacks */
  unsigned execCnt;          /* Number of calls to the capdb_exec callback */
} FuzzCtx;

/*
** Progress handler callback.
**
** The argument is the cutoff-time after which all processing should
** stop.  So return non-zero if the cut-off time is exceeded.
*/
static int progress_handler(void *pClientData) {
  FuzzCtx *p = (FuzzCtx*)pClientData;
  capdb_int64 iNow = timeOfDay();
  int rc = iNow>=p->iCutoffTime;
  capdb_int64 iDiff = iNow - p->iLastCb;
  if( iDiff > p->mxInterval ) p->mxInterval = iDiff;
  p->nCb++;
  return rc;
}

/*
** Disallow debugging pragmas such as "PRAGMA vdbe_debug" and
** "PRAGMA parser_trace" since they can dramatically increase the
** amount of output without actually testing anything useful.
*/
static int block_debug_pragmas(
  void *Notused,
  int eCode,
  const char *zArg1,
  const char *zArg2,
  const char *zArg3,
  const char *zArg4
){
  if( eCode==CAPDB_PRAGMA
   && (capdb_strnicmp("vdbe_", zArg1, 5)==0
        || capdb_stricmp("parser_trace", zArg1)==0)
  ){
    return CAPDB_DENY;
  }
  return CAPDB_OK;
}

/*
** Callback for capdb_exec().
*/
static int exec_handler(void *pClientData, int argc, char **argv, char **namev){
  FuzzCtx *p = (FuzzCtx*)pClientData;
  int i;
  if( argv ){
    for(i=0; i<argc; i++) capdb_free(capdb_mprintf("%s", argv[i]));
  }
  return (p->execCnt--)<=0 || progress_handler(pClientData);
}

/*
** Main entry point.  The fuzzer invokes this function with each
** fuzzed input.
*/
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  char *zErrMsg = 0;       /* Error message returned by sqlite_exec() */
  uint8_t uSelector;       /* First byte of input data[] */
  int rc;                  /* Return code from various interfaces */
  char *zSql;              /* Zero-terminated copy of data[] */
  FuzzCtx cx;              /* Fuzzing context */

  memset(&cx, 0, sizeof(cx));
  if( size<3 ) return 0;   /* Early out if unsufficient data */

  /* Extract the selector byte from the beginning of the input.  But only
  ** do this if the second byte is a \n.  If the second byte is not \n,
  ** then use a default selector */
  if( data[1]=='\n' ){
    uSelector = data[0];  data += 2; size -= 2;
  }else{
    uSelector = 0xfd;
  }

  /* Open the database connection.  Only use an in-memory database. */
  if( capdb_initialize() ) return 0;
  rc = capdb_open_v2("fuzz.db", &cx.db,
           CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE | CAPDB_OPEN_MEMORY, 0);
  if( rc ) return 0;

  /* Invoke the progress handler frequently to check to see if we
  ** are taking too long.  The progress handler will return true
  ** (which will block further processing) if more than 10 seconds have
  ** elapsed since the start of the test.
  */
  cx.iLastCb = timeOfDay();
  cx.iCutoffTime = cx.iLastCb + 10000;  /* Now + 10 seconds */
#ifndef CAPDB_OMIT_PROGRESS_CALLBACK
  capdb_progress_handler(cx.db, 10, progress_handler, (void*)&cx);
#endif

  /* Set a limit on the maximum size of a prepared statement */
  capdb_limit(cx.db, CAPDB_LIMIT_VDBE_OP, 25000);

  /* Set a limit on the maximum LIKE or GLOB pattern length due to
  ** https://issues.oss-fuzz.com/issues/453240497.  The default is 50K
  ** which is causing timeouts in OSS-Fuzz */
  capdb_limit(cx.db, CAPDB_LIMIT_LIKE_PATTERN_LENGTH, 250);

  /* Limit total memory available to SQLite to 20MB */
  capdb_hard_heap_limit64(20000000);

  /* Set a limit on the maximum length of a string or BLOB.  Without this
  ** limit, fuzzers will invoke randomblob(N) for a large N, and the process
  ** will timeout trying to generate the huge blob */
  capdb_limit(cx.db, CAPDB_LIMIT_LENGTH, 50000);

  /* Bit 1 of the selector enables foreign key constraints */
  capdb_db_config(cx.db, CAPDB_DBCONFIG_ENABLE_FKEY, uSelector&1, &rc);
  uSelector >>= 1;

  /* Do not allow debugging pragma statements that might cause excess output */
  capdb_set_authorizer(cx.db, block_debug_pragmas, 0);

  /* Remaining bits of the selector determine a limit on the number of
  ** output rows */
  cx.execCnt = uSelector + 1;

  /* Run the SQL.  The sqlite_exec() interface expects a zero-terminated
  ** string, so make a copy. */
  zSql = capdb_mprintf("%.*s", (int)size, data);
#ifndef CAPDB_OMIT_COMPLETE
  capdb_complete(zSql);
#endif
  capdb_exec(cx.db, zSql, exec_handler, (void*)&cx, &zErrMsg);

  /* Show any errors */
  if( (mDebug & FUZZ_SHOW_ERRORS)!=0 && zErrMsg ){
    printf("Error: %s\n", zErrMsg);
  }

  /* Cleanup and return */
  capdb_free(zErrMsg);
  capdb_free(zSql);
  capdb_exec(cx.db, "PRAGMA temp_store_directory=''", 0, 0, 0);
  capdb_close(cx.db);

  if( mDebug & FUZZ_SHOW_MAX_DELAY ){
    printf("Progress callback count....... %d\n", cx.nCb);
    printf("Max time between callbacks.... %d ms\n", (int)cx.mxInterval);
  }
  return 0;
}
