/*
** Port of test/poolbusy.test — pool concurrency.
*/
#include "capsuite.h"
#include "capdb.h"
#include "capdb_pool.h"
#include <string.h>
#include <unistd.h>

static char g_poolbusy_db[64];

static void poolbusy_db_init(void){
  snprintf(g_poolbusy_db, sizeof(g_poolbusy_db), "poolbusy_%d.db", (int)getpid());
}

static int exec_sql(capdb *db, const char *zSql){
  char *zErr = 0;
  int rc = capdb_exec(db, zSql, 0, 0, &zErr);
  if( zErr ) capdb_free(zErr);
  return rc;
}

static int scalar_int(capdb *db, const char *zSql, int *pOut){
  capdb_stmt *pStmt = 0;
  int rc, v = -1;
  rc = capdb_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=CAPDB_OK ) return rc;
  rc = capdb_step(pStmt);
  if( rc==CAPDB_ROW ) v = capdb_column_int(pStmt, 0);
  capdb_finalize(pStmt);
  if( rc==CAPDB_ROW ) *pOut = v;
  return rc==CAPDB_ROW ? CAPDB_OK : rc;
}

static int poolbusy_1_reads(void){
  capdb_pool_config cfg;
  capdb_pool *pPool = 0;
  capdb *d1=0, *d2=0, *d3=0, *d4=0;
  int nIdle, nActive, nTotal, n;
  int rc;

  poolbusy_db_init();
  capsuite_rm_rf(g_poolbusy_db);
  {
    char wal[80], shm[80];
    snprintf(wal, sizeof(wal), "%s-wal", g_poolbusy_db);
    snprintf(shm, sizeof(shm), "%s-shm", g_poolbusy_db);
    capsuite_rm_rf(wal);
    capsuite_rm_rf(shm);
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.minSize = 4;
  cfg.maxSize = 8;
  cfg.busyTimeoutMs = 5000;
  cfg.flags = 7;

  rc = capdb_pool_open(g_poolbusy_db, &cfg, &pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &d1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &d2);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &d3);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &d4);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = exec_sql(d1, "CREATE TABLE t1(x INTEGER PRIMARY KEY)");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(d1, "INSERT INTO t1 VALUES(1)");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_stats(pPool, &nIdle, &nActive, &nTotal);
  CAPSUITE_ASSERT_EQ_int(nIdle, 0);
  CAPSUITE_ASSERT_EQ_int(nActive, 4);
  CAPSUITE_ASSERT_EQ_int(nTotal, 4);

  rc = scalar_int(d2, "SELECT count(*) FROM t1", &n);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  CAPSUITE_ASSERT_EQ_int(n, 1);
  rc = scalar_int(d3, "SELECT count(*) FROM t1", &n);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  CAPSUITE_ASSERT_EQ_int(n, 1);
  rc = scalar_int(d4, "SELECT count(*) FROM t1", &n);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  CAPSUITE_ASSERT_EQ_int(n, 1);

  capdb_pool_release(pPool, d1);
  capdb_pool_release(pPool, d2);
  capdb_pool_release(pPool, d3);
  capdb_pool_release(pPool, d4);

  rc = capdb_pool_stats(pPool, &nIdle, &nActive, &nTotal);
  CAPSUITE_ASSERT_EQ_int(nIdle, 4);
  CAPSUITE_ASSERT_EQ_int(nActive, 0);
  CAPSUITE_ASSERT_EQ_int(nTotal, 4);

  capdb_pool_close(pPool);
  return 0;
}

static int poolbusy_2_writes(void){
  capdb_pool *pPool = 0;
  capdb *w1 = 0, *db = 0;
  capdb *dummy = 0;
  int n, rc;

  poolbusy_db_init();

  rc = capdb_pool_open(g_poolbusy_db, 0, &pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, -1, &w1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, 0, &dummy);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_BUSY);

  rc = capdb_pool_release(pPool, w1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, -1, &w1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(w1, "INSERT INTO t1 VALUES(2)");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(w1, "COMMIT");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, w1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = scalar_int(db, "SELECT count(*) FROM t1", &n);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  CAPSUITE_ASSERT_EQ_int(n, 2);
  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_close(pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  return 0;
}

void capsuite_register_pool_busy(void){
  static CapsuiteTest a[] = {
    { "poolbusy-1-reads", poolbusy_1_reads },
    { "poolbusy-2-writes", poolbusy_2_writes },
  };
  capsuite_register_tests(a, (int)(sizeof(a)/sizeof(a[0])));
}
