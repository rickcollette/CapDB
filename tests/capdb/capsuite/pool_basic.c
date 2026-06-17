/*
** Port of test/pool.test — connection pool basics.
*/
#include "capsuite.h"
#include "capdb.h"
#include "capdb_pool.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_pool_db[64];

static void pool_db_init(void){
  snprintf(g_pool_db, sizeof(g_pool_db), "pool_%d.db", (int)getpid());
}

static int exec_sql(capdb *db, const char *zSql){
  char *zErr = 0;
  int rc = capdb_exec(db, zSql, 0, 0, &zErr);
  if( zErr ){ capdb_free(zErr); }
  return rc;
}

static int pool_1_basic(void){
  capdb_pool_config cfg;
  capdb_pool *pPool = 0;
  capdb *db = 0;
  int nIdle, nActive, nTotal;
  int rc;

  pool_db_init();
  capsuite_rm_rf(g_pool_db);
  {
    char wal[72], shm[72];
    snprintf(wal, sizeof(wal), "%s-wal", g_pool_db);
    snprintf(shm, sizeof(shm), "%s-shm", g_pool_db);
    capsuite_rm_rf(wal);
    capsuite_rm_rf(shm);
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.minSize = 2;
  cfg.maxSize = 4;
  cfg.busyTimeoutMs = 5000;
  cfg.flags = 7;

  rc = capdb_pool_open(g_pool_db, &cfg, &pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_stats(pPool, &nIdle, &nActive, &nTotal);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  CAPSUITE_ASSERT_EQ_int(nIdle, 2);
  CAPSUITE_ASSERT_EQ_int(nActive, 0);
  CAPSUITE_ASSERT_EQ_int(nTotal, 2);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(db, "CREATE TABLE t1(x INTEGER PRIMARY KEY, y TEXT)");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(db, "INSERT INTO t1 VALUES(1,'one')");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_stats(pPool, &nIdle, &nActive, &nTotal);
  CAPSUITE_ASSERT_EQ_int(nIdle, 2);
  CAPSUITE_ASSERT_EQ_int(nActive, 0);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(db, "SELECT y FROM t1 WHERE x=1");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(db, "INSERT INTO t1 VALUES(2,'two')");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(db, "COMMIT");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = exec_sql(db, "SELECT x FROM t1 ORDER BY x");
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_close(pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  return 0;
}

static int pool_2_capacity(void){
  capdb_pool_config cfg;
  capdb_pool *pPool = 0;
  capdb *d1 = 0, *d2 = 0;
  capdb *d3 = 0;
  int rc;

  pool_db_init();
  capsuite_rm_rf(g_pool_db);

  memset(&cfg, 0, sizeof(cfg));
  cfg.minSize = 1;
  cfg.maxSize = 2;
  cfg.busyTimeoutMs = 5000;
  cfg.flags = 7;

  rc = capdb_pool_open(g_pool_db, &cfg, &pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &d1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &d2);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, 0, &d3);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_BUSY);

  rc = capdb_pool_release(pPool, d1);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_release(pPool, d2);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_close(pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  return 0;
}

static int pool_3_close_busy(void){
  capdb_pool *pPool = 0;
  capdb *db = 0;
  int rc;

  pool_db_init();
  capsuite_rm_rf(g_pool_db);

  rc = capdb_pool_open(g_pool_db, 0, &pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_pool_close(pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_BUSY);

  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_close(pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  return 0;
}

static int pool_4_wal_default(void){
  capdb_pool *pPool = 0;
  capdb *db = 0;
  capdb_stmt *pStmt = 0;
  const char *zMode = 0;
  int rc;

  pool_db_init();
  capsuite_rm_rf(g_pool_db);

  rc = capdb_pool_open(g_pool_db, 0, &pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);

  rc = capdb_prepare_v2(db, "PRAGMA journal_mode", -1, &pStmt, 0);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_step(pStmt);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_ROW);
  zMode = (const char*)capdb_column_text(pStmt, 0);
  CAPSUITE_ASSERT_EQ_str(zMode, "wal");
  capdb_finalize(pStmt);

  rc = capdb_pool_release(pPool, db);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  rc = capdb_pool_close(pPool);
  CAPSUITE_ASSERT_EQ_int(rc, CAPDB_OK);
  return 0;
}

void capsuite_register_pool_basic(void){
  static CapsuiteTest a[] = {
    { "pool-1-basic", pool_1_basic },
    { "pool-2-capacity", pool_2_capacity },
    { "pool-3-close-busy", pool_3_close_busy },
    { "pool-4-wal-default", pool_4_wal_default },
  };
  capsuite_register_tests(a, (int)(sizeof(a)/sizeof(a[0])));
}
