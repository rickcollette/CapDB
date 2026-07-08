/*
** Unit tests for capdb_store volume API.
*/
#if defined(CAPDB_ENABLE_STORE)

#include "capdb.h"
#include "capdb/store/capdb_store.h"
#include "capdb/cluster/capdb_cluster.h"
#if defined(CAPDB_ENABLE_POOL)
#include "capdb/pool/capdb_pool.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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
  rc = exec_sql(db, "PRAGMA journal_mode=WAL");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "wal pragma rc=%d %s\n", rc, capdb_errmsg(db));
    return 1;
  }
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
  rc = exec_sql(db, "INSERT INTO px VALUES(2,'beta')");
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "exec-ins2 rc=%d %s\n", rc, capdb_errmsg(db));
    capdb_close_v2(db);
    capdb_store_vfs_shutdown();
    return 1;
  }
  fprintf(stderr, "prepare-step direct ok\n");
  capdb_close_v2(db);
  capdb_store_vfs_shutdown();
  return 0;
}

static int test_cluster_role_state(const char *zRoot){
  char zVol[512];
  capdb_cluster_status st;
  char zRole[32];
  snprintf(zVol, sizeof(zVol), "%s/cluster", zRoot);
  if( capdb_volume_prepare(zVol, CAPDB_VOLUME_OPEN_CREATE)!=CAPDB_OK ) return 1;
  if( capdb_cluster_status_fill(zVol, &st)!=CAPDB_OK ) return 1;
  if( st.role!=CAPDB_CLUSTER_ROLE_PRIMARY ) return 1;
  if( capdb_cluster_demote(zVol)!=CAPDB_OK ) return 1;
  if( capdb_cluster_status_fill(zVol, &st)!=CAPDB_OK ) return 1;
  if( st.role!=CAPDB_CLUSTER_ROLE_DEMOTED ) return 1;
  if( capdb_cluster_role_name(st.role, zRole, sizeof(zRole))!=CAPDB_OK ) return 1;
  if( strcmp(zRole, "demoted")!=0 ) return 1;
  if( capdb_cluster_promote(zVol, 100)!=CAPDB_OK ) return 1;
  if( capdb_cluster_status_fill(zVol, &st)!=CAPDB_OK ) return 1;
  if( st.role!=CAPDB_CLUSTER_ROLE_PRIMARY ) return 1;
  return 0;
}

static int test_snapshot_path(const char *zRoot){
  char zVol[512];
  char zSnap[1024];
  char zMain[1100];
  capdb_volume *p = 0;
  struct stat st;
  snprintf(zVol, sizeof(zVol), "%s/snapvol", zRoot);
  if( capdb_volume_open(zVol, CAPDB_VOLUME_OPEN_CREATE, &p)!=CAPDB_OK ) return 1;
  if( capdb_volume_snapshot(p, 123, zSnap, sizeof(zSnap))!=CAPDB_OK ){
    capdb_volume_close(p);
    return 1;
  }
  capdb_volume_close(p);
  snprintf(zMain, sizeof(zMain), "%s/main.db", zSnap);
  if( strstr(zSnap, "/snapshots/snap_123")==0 ) return 1;
  if( stat(zMain, &st)!=0 ) return 1;
  return 0;
}

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
  if( test_direct_volume_sql(zRoot) ){
    fprintf(stderr, "store_test: direct volume sql failed\n");
    return 1;
  }
  if( test_cluster_role_state(zRoot) ){
    fprintf(stderr, "store_test: cluster role state failed\n");
    return 1;
  }
  if( test_snapshot_path(zRoot) ){
    fprintf(stderr, "store_test: snapshot path failed\n");
    return 1;
  }
  fprintf(stderr, "store_test: ok\n");
  return 0;
}

#else
int main(void){ return 1; }
#endif
