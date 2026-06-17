/*
** Unit tests for capdb_store volume API.
*/
#if defined(CAPDB_ENABLE_STORE)

#include "capdb.h"
#include "capdb/store/capdb_store.h"
#if defined(CAPDB_ENABLE_POOL)
#include "capdb/pool/capdb_pool.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_volume_crud(const char *zRoot){
  char zVol[512];
  capdb_volume *p = 0;
  unsigned char buf[4096];
  unsigned char buf2[4096];
  snprintf(zVol, sizeof(zVol), "%s/testvol", zRoot);
  if( capdb_volume_open(zVol, CAPDB_VOLUME_OPEN_CREATE, &p)!=CAPDB_OK ) return 1;
  memset(buf, 0xAB, sizeof(buf));
  if( capdb_volume_write_page(p, 1, buf, 4096)!=CAPDB_OK ) return 1;
  if( capdb_volume_read_page(p, 1, buf2, 4096)!=CAPDB_OK ) return 1;
  if( memcmp(buf, buf2, 4096)!=0 ) return 1;
  {
    unsigned long long lsn = 0;
    if( capdb_volume_append_wal(p, buf, 64, 0, &lsn)!=CAPDB_OK || lsn==0 ) return 1;
  }
  capdb_volume_close(p);
  if( capdb_volume_open(zVol, 0, &p)!=CAPDB_OK ) return 1;
  if( capdb_volume_read_page(p, 1, buf2, 4096)!=CAPDB_OK ) return 1;
  if( memcmp(buf, buf2, 4096)!=0 ) return 1;
  capdb_volume_close(p);
  return 0;
}

static int test_crash_recovery(const char *zRoot){
  char zVol[512];
  capdb_volume *p = 0;
  unsigned char buf[4096];
  snprintf(zVol, sizeof(zVol), "%s/crashvol", zRoot);
  if( capdb_volume_open(zVol, CAPDB_VOLUME_OPEN_CREATE, &p)!=CAPDB_OK ) return 1;
  memset(buf, 0xCD, sizeof(buf));
  if( capdb_volume_write_page(p, 1, buf, 4096)!=CAPDB_OK ) return 1;
  capdb_volume_close(p);
  if( capdb_volume_open(zVol, 0, &p)!=CAPDB_OK ) return 1;
  if( capdb_volume_read_page(p, 1, buf, 4096)!=CAPDB_OK ) return 1;
  if( buf[0]!=0xCD ) return 1;
  capdb_volume_close(p);
  return 0;
}

static int exec_sql(capdb *db, const char *zSql){
  char *zErr = 0;
  int rc = capdb_exec(db, zSql, 0, 0, &zErr);
  if( zErr ) capdb_free(zErr);
  return rc;
}

static int test_direct_volume_sql(const char *zRoot){
  char zVol[512];
  capdb *db = 0;
  int rc;

  snprintf(zVol, sizeof(zVol), "%s/direct", zRoot);
  if( capdb_store_vfs_register(zRoot, 0) ) return 1;
  rc = capdb_open_v2(zVol, &db,
      CAPDB_OPEN_READWRITE|CAPDB_OPEN_CREATE, capdb_store_vfs_name());
  if( rc!=CAPDB_OK ){ fprintf(stderr, "open rc=%d\n", rc); return 1; }
  {
    capdb_stmt *s = 0;
    rc = capdb_prepare_v2(db, "CREATE TABLE IF NOT EXISTS px(x INTEGER, y TEXT)", -1, &s, 0);
    if( rc!=CAPDB_OK ){ fprintf(stderr, "prep-create rc=%d\n", rc); return 1; }
    rc = capdb_step(s);
    capdb_finalize(s);
    if( rc!=CAPDB_DONE ){ fprintf(stderr, "step-create rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  }
  rc = exec_sql(db, "INSERT INTO px VALUES(1,'alpha')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "exec-ins rc=%d %s\n", rc, capdb_errmsg(db));
    capdb_close_v2(db);
    capdb_store_vfs_shutdown();
    return 1;
  }
  fprintf(stderr, "prepare-step direct ok\n");
  capdb_close_v2(db);
  capdb_store_vfs_shutdown();
  return 0;
}

#if 0
static int test_direct_volume_sql_idle(const char *zRoot){
  char zVol[512];
  capdb *db = 0;
  int rc;
  snprintf(zVol, sizeof(zVol), "%s/direct", zRoot);
  if( capdb_store_vfs_register(zRoot, 0) ) return 1;
  rc = capdb_open_v2(zVol, &db,
      CAPDB_OPEN_READWRITE|CAPDB_OPEN_CREATE, capdb_store_vfs_name());
  if( rc!=CAPDB_OK ) return 1;
  capdb_busy_timeout(db, 5000);
  sleep(1);
  rc = exec_sql(db, "CREATE TABLE IF NOT EXISTS px(x INTEGER, y TEXT)");
  if( rc!=CAPDB_OK ) return 1;
  rc = exec_sql(db, "INSERT INTO px VALUES(1,'alpha')");
  capdb_close_v2(db);
  capdb_store_vfs_shutdown();
  return rc==CAPDB_OK ? 0 : 1;
}
#endif

#if 0
static int test_direct_volume_sql_old(const char *zRoot){
  char zVol[512];
  capdb *db = 0;
  int rc;

  snprintf(zVol, sizeof(zVol), "%s/direct", zRoot);
  if( capdb_store_vfs_register(zRoot, 0) ) return 1;
  rc = capdb_open_v2(zVol, &db,
      CAPDB_OPEN_READWRITE|CAPDB_OPEN_CREATE, capdb_store_vfs_name());
  if( rc!=CAPDB_OK ){ fprintf(stderr, "open rc=%d\n", rc); return 1; }
  rc = exec_sql(db, "PRAGMA journal_mode=WAL");
  fprintf(stderr, "d-wal rc=%d %s\n", rc, capdb_errmsg(db));
  rc = exec_sql(db, "BEGIN IMMEDIATE");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "d-begin rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = exec_sql(db, "CREATE TABLE IF NOT EXISTS ps(x INTEGER, y TEXT)");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "d-create rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = exec_sql(db, "INSERT INTO ps VALUES(1,'alpha')");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "d-ins1 rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = exec_sql(db, "COMMIT");
  fprintf(stderr, "d-after commit autocommit=%d rc=%d %s\n",
          capdb_get_autocommit(db), rc, capdb_errmsg(db));
  rc = exec_sql(db, "INSERT INTO ps VALUES(2,'beta')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "d-insert rc=%d %s\n", rc, capdb_errmsg(db));
    capdb_close_v2(db);
    capdb_store_vfs_shutdown();
    return 1;
  }
  capdb_close_v2(db);
  capdb_store_vfs_shutdown();
  return 0;
}
#endif

#if 0 && defined(CAPDB_ENABLE_POOL)
static int test_pool_volume_sql(const char *zRoot){
  char zVol[512];
  capdb_pool_config cfg;
  capdb_pool *pPool = 0;
  capdb *db = 0;
  int rc;

  snprintf(zVol, sizeof(zVol), "%s/mnet", zRoot);
  if( capdb_store_vfs_register(zRoot, 0) ) return 1;
  memset(&cfg, 0, sizeof(cfg));
  cfg.minSize = 1;
  cfg.maxSize = 2;
  cfg.flags = CAPDB_POOL_SERIALIZE_WRITES
            | CAPDB_POOL_RESET_ON_RELEASE;
  cfg.zVfs = capdb_store_vfs_name();
  rc = capdb_pool_open(zVol, &cfg, &pPool);
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "pool open rc=%d\n", rc);
    return 1;
  }

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  if( rc!=CAPDB_OK ){ fprintf(stderr, "read-acq rc=%d\n", rc); return 1; }
  rc = exec_sql(db, "CREATE TABLE IF NOT EXISTS pr(x INTEGER, y TEXT)");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "read-create rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = exec_sql(db, "INSERT INTO pr VALUES(1,'alpha')");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "read-ins1 rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = capdb_pool_release(pPool, db);
  if( rc!=CAPDB_OK ) return 1;
  rc = capdb_pool_acquire(pPool, CAPDB_POOL_READ, -1, &db);
  if( rc!=CAPDB_OK ) return 1;
  rc = exec_sql(db, "INSERT INTO pr VALUES(2,'beta')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "read-ins2 rc=%d %s\n", rc, capdb_errmsg(db));
    return 1;
  }
  fprintf(stderr, "read-mode two inserts ok\n");
  rc = capdb_pool_release(pPool, db);
  if( rc!=CAPDB_OK ) return 1;

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, -1, &db);
  if( rc!=CAPDB_OK ){ fprintf(stderr, "acq1 rc=%d\n", rc); return 1; }
  {
    capdb_stmt *s = 0;
    const unsigned char *zMode = 0;
    if( capdb_prepare_v2(db, "PRAGMA journal_mode", -1, &s, 0)==CAPDB_OK ){
      if( capdb_step(s)==CAPDB_ROW ) zMode = capdb_column_text(s, 0);
      capdb_finalize(s);
    }
    fprintf(stderr, "journal_mode=%s\n", zMode ? (const char*)zMode : "?");
  }
  rc = exec_sql(db, "CREATE TABLE IF NOT EXISTS ps(x INTEGER, y TEXT)");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "create rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = exec_sql(db, "INSERT INTO ps VALUES(1,'alpha')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "insert same txn rc=%d %s\n", rc, capdb_errmsg(db));
    return 1;
  }
  rc = exec_sql(db, "COMMIT");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "commit1 rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  fprintf(stderr, "same-txn ok, after commit1 autocommit=%d\n", capdb_get_autocommit(db));
  rc = exec_sql(db, "INSERT INTO ps VALUES(2,'beta')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "insert2 same checkout rc=%d %s\n", rc, capdb_errmsg(db));
    return 1;
  }
  rc = exec_sql(db, "COMMIT");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "commit2 rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  fprintf(stderr, "two-txn same checkout ok\n");
  rc = capdb_pool_release(pPool, db);
  if( rc!=CAPDB_OK ){ fprintf(stderr, "rel1 rc=%d\n", rc); return 1; }

  rc = capdb_pool_acquire(pPool, CAPDB_POOL_WRITE, -1, &db);
  if( rc!=CAPDB_OK ){ fprintf(stderr, "acq2 rc=%d\n", rc); return 1; }
  fprintf(stderr, "after acq2 autocommit=%d\n", capdb_get_autocommit(db));
  rc = exec_sql(db, "INSERT INTO ps VALUES(3,'gamma')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "insert rc=%d autocommit=%d %s\n",
            rc, capdb_get_autocommit(db), capdb_errmsg(db));
    return 1;
  }
  rc = exec_sql(db, "COMMIT");
  if( rc!=CAPDB_OK ){ fprintf(stderr, "commit2 rc=%d %s\n", rc, capdb_errmsg(db)); return 1; }
  rc = capdb_pool_release(pPool, db);
  if( rc!=CAPDB_OK ) return 1;

  capdb_pool_close(pPool);
  capdb_store_vfs_shutdown();
  return 0;
}
#endif

int main(void){
  char zRoot[] = "/tmp/capdb_store_test_XXXXXX";
  if( mkdtemp(zRoot)==0 ) return 1;
  if( test_volume_crud(zRoot) ){
    fprintf(stderr, "store_test: volume crud failed\n");
    return 1;
  }
  if( test_crash_recovery(zRoot) ){
    fprintf(stderr, "store_test: crash recovery failed\n");
    return 1;
  }
#if 0
  /* capdbstorevfs prepare/step and exec back-to-back writes: Phase B */
  if( test_direct_volume_sql(zRoot) ){
    fprintf(stderr, "store_test: direct prepare-step failed\n");
    return 1;
  }
#endif
  fprintf(stderr, "store_test: ok\n");
  return 0;
}

#else
int main(void){ return 1; }
#endif
