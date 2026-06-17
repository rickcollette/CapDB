/*
** 2026-06-12
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** Stress test for the connection pool under concurrent load.
**
** Usage:
**
**      ./threadtest_pool ?DATABASE?
**
** If DATABASE is omitted, it defaults to "test_pool_thread.db".
*/
#include "capdb.h"
#include "capdb_pool.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *zDbName = 0;
static capdb_pool *pPool = 0;
static int eVerbose = 0;
static int nWorker = 4;
static int nDone = 0;
static pthread_mutex_t doneMutex = PTHREAD_MUTEX_INITIALIZER;

static void error_out(int rc, const char *zCtx, int lineno){
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "error %d at %d in \"%s\"\n", rc, lineno, zCtx);
    exit(-1);
  }
}

static int exec(capdb *db, const char *zSql){
  char *zErr = 0;
  int rc = capdb_exec(db, zSql, 0, 0, &zErr);
  if( rc ){
    fprintf(stderr, "exec error: %s (%s)\n", zErr ? zErr : "", zSql);
    capdb_free(zErr);
    exit(-1);
  }
  return rc;
}

static void *worker(void *pArg){
  int iWorker = *(int*)pArg;
  char zName[32];
  int i;

  capdb_snprintf(sizeof(zName), zName, "worker%d", iWorker);

  for(i=0; i<25; i++){
    capdb *db = 0;
    int rc;
    char zSql[128];

    rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, 10000, &db);
    error_out(rc, "capdb_pool_acquire WRITE", __LINE__);

    capdb_snprintf(sizeof(zSql), zSql,
        "INSERT INTO counter VALUES(%d, %Q);", iWorker, zName);
    exec(db, zSql);
    exec(db, "COMMIT");

    rc = capdb_pool_release(pPool, db);
    error_out(rc, "capdb_pool_release", __LINE__);
  }

  pthread_mutex_lock(&doneMutex);
  nDone++;
  pthread_mutex_unlock(&doneMutex);
  return 0;
}

int main(int argc, char **argv){
  capdb_pool_config cfg;
  capdb *db = 0;
  int rc;
  int i;
  pthread_t aTid[32];
  int aId[32];

  zDbName = "test_pool_thread.db";
  for(i=1; i<argc; i++){
    if( argv[i][0]=='-' && strcmp(argv[i], "-v")==0 ){
      eVerbose = 1;
    }else{
      zDbName = argv[i];
    }
  }

  remove(zDbName);
  memset(&cfg, 0, sizeof(cfg));
  cfg.minSize = 2;
  cfg.maxSize = 8;
  cfg.busyTimeoutMs = 5000;
  cfg.flags = CAPDB_POOL_DEFAULT_WAL
            | CAPDB_POOL_SERIALIZE_WRITES
            | CAPDB_POOL_RESET_ON_RELEASE;

  rc = capdb_pool_open(zDbName, &cfg, &pPool);
  error_out(rc, "capdb_pool_open", __LINE__);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, -1, &db);
  error_out(rc, "initial acquire", __LINE__);
  exec(db, "CREATE TABLE counter(n INTEGER, worker TEXT);");
  exec(db, "COMMIT");
  rc = capdb_pool_release(pPool, db);
  error_out(rc, "initial release", __LINE__);

  if( nWorker>(int)(sizeof(aTid)/sizeof(aTid[0])) ) nWorker = 4;
  for(i=0; i<nWorker; i++){
    aId[i] = i;
    rc = pthread_create(&aTid[i], 0, worker, &aId[i]);
    if( rc ){
      fprintf(stderr, "pthread_create failed\n");
      return 1;
    }
  }
  for(i=0; i<nWorker; i++){
    pthread_join(aTid[i], 0);
  }

  if( nDone!=nWorker ){
    fprintf(stderr, "expected %d workers done, got %d\n", nWorker, nDone);
    return 1;
  }

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  error_out(rc, "verify acquire", __LINE__);
  {
    capdb_stmt *pStmt = 0;
    int n = 0;
    rc = capdb_prepare_v2(db, "SELECT count(*) FROM counter", -1, &pStmt, 0);
    error_out(rc, "prepare", __LINE__);
    if( capdb_step(pStmt)==CAPDB_ROW ){
      n = capdb_column_int(pStmt, 0);
    }
    capdb_finalize(pStmt);
    if( n!=nWorker*25 ){
      fprintf(stderr, "expected %d rows, got %d\n", nWorker*25, n);
      return 1;
    }
  }
  capdb_pool_release(pPool, db);

  rc = capdb_pool_close(pPool);
  error_out(rc, "capdb_pool_close", __LINE__);

  if( eVerbose ) printf("threadtest_pool: ok\n");
  return 0;
}
