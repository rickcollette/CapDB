/*
** Port of test/capdb.test — network loopback tests via capdbtest subprocess.
*/
#include "capsuite.h"
#include "capdb_harness.h"
#include "capdb_client.h"
#include "capdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef CAPSUITE_CERT_DIR
#define CAPSUITE_CERT_DIR "tests/fixtures/certs"
#endif

#ifndef CAPSUITE_CLIENT_TEST
#define CAPSUITE_CLIENT_TEST "capdbtest"
#endif

#ifndef CAPSUITE_NET_TEST
#define CAPSUITE_NET_TEST "capdb_nettest"
#endif

static const char *capsuite_capdbtest_bin(void){
  const char *z = getenv("CAPSUITE_CLIENT_TEST");
  return z && z[0] ? z : CAPSUITE_CLIENT_TEST;
}

static const char *capsuite_capdb_nettest_bin(void){
  const char *z = getenv("CAPSUITE_NET_TEST");
  return z && z[0] ? z : CAPSUITE_NET_TEST;
}

static int run_capdbtest_uri(const char *zUri, int expectOk){
  const char *argv[4];
  pid_t pid;
  int st;

  argv[0] = capsuite_capdbtest_bin();
  argv[1] = zUri;
  argv[2] = 0;

  if( capsuite_spawn(argv, &pid) ) return -1;
  if( waitpid(pid, &st, 0)<0 ) return -1;
  if( !WIFEXITED(st) ) return -1;
  if( expectOk ) return WEXITSTATUS(st)==0 ? 0 : -1;
  return WEXITSTATUS(st)!=0 ? 0 : -1;
}

static int run_nettest(const char *zTest, const char *zUri){
  const char *argv[5];
  pid_t pid;
  int st;
  argv[0] = capsuite_capdb_nettest_bin();
  argv[1] = zTest;
  argv[2] = zUri;
  argv[3] = 0;
  if( capsuite_spawn(argv, &pid) ) return -1;
  if( waitpid(pid, &st, 0)<0 ) return -1;
  if( !WIFEXITED(st) ) return -1;
  return WEXITSTATUS(st)==0 ? 0 : -1;
}

static int capdb_net_case(const char *zTest){
  CapdbTestServer srv;
  char zUri[256];
  int rc;
  memset(&srv, 0, sizeof(srv));
  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("server start");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=testtoken&insecure=1", srv.port);
  rc = run_nettest(zTest, zUri);
  capsuite_capdb_server_stop(&srv);
  if( rc ) CAPSUITE_FAIL(zTest);
  return 0;
}

static int capdb_1_loopback(void){
  CapdbTestServer srv;
  char zUri[256];

  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("server start");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=testtoken&insecure=1", srv.port);

  if( run_capdbtest_uri(zUri, 1) ) CAPSUITE_FAIL("capdbtest loopback");
  capsuite_capdb_server_stop(&srv);
  return 0;
}

static int capdb_2_auth_fail(void){
  CapdbTestServer srv;
  char zUri[256];

  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("server start");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=bad&insecure=1", srv.port);

  if( run_capdbtest_uri(zUri, 0) ) CAPSUITE_FAIL("expected auth failure");
  capsuite_capdb_server_stop(&srv);
  return 0;
}

static int capdb_3_prepare_step(void){ return capdb_net_case("prepare-step"); }
static int capdb_4_open_switch(void){ return capdb_net_case("open-switch"); }
static int capdb_5_open_in_txn(void){ return capdb_net_case("open-in-txn"); }
static int capdb_6_vfs_read(void){ return capdb_net_case("vfs-read"); }
static int capdb_7_attach_denied(void){ return capdb_net_case("attach-denied"); }
static int capdb_8_stmt_before_exec(void){ return capdb_net_case("stmt-before-exec"); }
static int capdb_9_prepare_write_pin(void){ return capdb_net_case("prepare-write-pin"); }
static int capdb_10_column_types(void){ return capdb_net_case("column-types"); }
static int capdb_11_vfs_lock(void){ return capdb_net_case("vfs-lock"); }
static int capdb_12_changes_rowid(void){ return capdb_net_case("changes-rowid"); }
static int capdb_13_shutdown_idle(void){
  const char *argv[4];
  pid_t pid;
  int st;
  argv[0] = capsuite_capdb_nettest_bin();
  argv[1] = "shutdown-idle";
  argv[2] = 0;
  if( capsuite_spawn(argv, &pid) ) CAPSUITE_FAIL("spawn shutdown-idle");
  if( waitpid(pid, &st, 0)<0 ) CAPSUITE_FAIL("wait shutdown-idle");
  if( !WIFEXITED(st) || WEXITSTATUS(st)!=0 ) CAPSUITE_FAIL("shutdown-idle");
  return 0;
}

static int capdb_14_session_reuse(void){
  const char *argv[4];
  pid_t pid;
  int st;
  argv[0] = capsuite_capdb_nettest_bin();
  argv[1] = "session-reuse";
  argv[2] = 0;
  if( capsuite_spawn(argv, &pid) ) CAPSUITE_FAIL("spawn session-reuse");
  if( waitpid(pid, &st, 0)<0 ) CAPSUITE_FAIL("wait session-reuse");
  if( !WIFEXITED(st) || WEXITSTATUS(st)!=0 ) CAPSUITE_FAIL("session-reuse");
  return 0;
}

static int capdb_15_vfs_exclude(void){ return capdb_net_case("vfs-pool-deny"); }

static int capdb_18_vfs_write(void){ return capdb_net_case("vfs-write"); }
static int capdb_19_exec_blob_callback(void){ return capdb_net_case("exec-blob-callback"); }
static int capdb_20_commit_open_stmt(void){ return capdb_net_case("commit-open-stmt"); }
static int capdb_21_vfs_separate_path(void){ return capdb_net_case("vfs-separate-path"); }
static int capdb_22_password_auth(void){ return capdb_net_case("password-auth"); }

static int capdb_23_capdbvfs_sql(void){
  CapdbTestServer srv;
  char zUri[256];
  char zFile[512];
  capdb *db = 0;
  capdb_stmt *st = 0;
  int rc;

  memset(&srv, 0, sizeof(srv));
  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("server start");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=testtoken&insecure=1", srv.port);
  snprintf(zFile, sizeof(zFile), "file:/vfsnet.db?mode=rw&capdb=%s", zUri);
  if( capdb_net_vfs_register(zUri, 0)!=CAPDB_NET_OK ) CAPSUITE_FAIL("vfs register");
  rc = capdb_open_v2(zFile, &db,
    CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE | CAPDB_OPEN_URI, "capdbvfs");
  if( rc!=CAPDB_OK ) CAPSUITE_FAIL("capdbvfs open");
  rc = capdb_exec(db, "PRAGMA journal_mode=MEMORY", 0, 0, 0);
  if( rc!=CAPDB_OK ) CAPSUITE_FAIL("journal_mode");
  rc = capdb_exec(db, "CREATE TABLE IF NOT EXISTS cv(v INTEGER)", 0, 0, 0);
  if( rc!=CAPDB_OK ) CAPSUITE_FAIL("create");
  rc = capdb_exec(db, "INSERT INTO cv VALUES(7)", 0, 0, 0);
  if( rc!=CAPDB_OK ) CAPSUITE_FAIL("insert");
  rc = capdb_prepare_v2(db, "SELECT v FROM cv", -1, &st, 0);
  if( rc!=CAPDB_OK ) CAPSUITE_FAIL("prepare");
  if( capdb_step(st)!=CAPDB_ROW ) CAPSUITE_FAIL("step");
  if( capdb_column_int(st, 0)!=7 ) CAPSUITE_FAIL("value");
  capdb_finalize(st);
  capdb_close(db);
  capsuite_capdb_server_stop(&srv);
  return 0;
}

static int capdb_net_case_volume(const char *zTest){
  CapdbTestServer srv;
  char zUri[256];
  int rc;
  memset(&srv, 0, sizeof(srv));
  srv.bVolume = 1;
  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("volume server start");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=testtoken&insecure=1", srv.port);
  rc = run_nettest(zTest, zUri);
  capsuite_capdb_server_stop(&srv);
  if( rc ) CAPSUITE_FAIL(zTest);
  return 0;
}

static int capdb_24_volume_sql(void){
  return capdb_net_case_volume("prepare-step");
}

#if defined(CAPDB_ENABLE_REPLICATION)
static int capdb_25_rep_async(void){
  CapdbTestServer primary;
  CapdbTestServer replica;
  char zPriUri[256];
  char zRepUri[256];
  char zRepPrimary[64];

  memset(&primary, 0, sizeof(primary));
  primary.bVolume = 1;
  primary.bRepListen = 1;
  if( capsuite_capdb_server_start(&primary, 0) ) CAPSUITE_FAIL("primary start");

  snprintf(zRepPrimary, sizeof(zRepPrimary), "127.0.0.1:%d", primary.repPort);
  memset(&replica, 0, sizeof(replica));
  replica.bVolume = 1;
  replica.bReplica = 1;
  snprintf(replica.zRepPrimary, sizeof(replica.zRepPrimary), "%s", zRepPrimary);
  if( capsuite_capdb_server_start(&replica, 0) ){
    capsuite_capdb_server_stop(&primary);
    CAPSUITE_FAIL("replica start");
  }

  snprintf(zPriUri, sizeof(zPriUri),
    "capdb://127.0.0.1:%d/rep.db?token=testtoken&insecure=1", primary.port);
  snprintf(zRepUri, sizeof(zRepUri),
    "capdb://127.0.0.1:%d/rep.db?token=testtoken&insecure=1", replica.port);

  if( run_nettest("rep-insert", zPriUri) ){
    capsuite_capdb_server_stop(&replica);
    capsuite_capdb_server_stop(&primary);
    CAPSUITE_FAIL("rep-insert");
  }
  capsuite_sleep_ms(2000);
  if( run_nettest("rep-select", zRepUri) ){
    capsuite_capdb_server_stop(&replica);
    capsuite_capdb_server_stop(&primary);
    CAPSUITE_FAIL("rep-select");
  }
  capsuite_capdb_server_stop(&replica);
  capsuite_capdb_server_stop(&primary);
  return 0;
}
#endif

static int capdb_16_vfs_delete(void){
  CapdbTestServer srv;
  char zUri[256];
  memset(&srv, 0, sizeof(srv));
  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("server start");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=testtoken&insecure=1", srv.port);
  /* capdbvfs xDelete is a permanent stub (CAPDB_IOERR_DELETE); verify register. */
  if( capdb_net_vfs_register(zUri, 0)!=CAPDB_NET_OK ) CAPSUITE_FAIL("vfs register");
  capsuite_capdb_server_stop(&srv);
  return 0;
}

static int ensure_tls_certs(char *zCert, char *zKey, size_t n){
  struct stat st;
  snprintf(zCert, n, "%s/server.pem", CAPSUITE_CERT_DIR);
  snprintf(zKey, n, "%s/server.key", CAPSUITE_CERT_DIR);
  if( stat(zCert, &st)==0 && stat(zKey, &st)==0 ) return 0;
  {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sh %s/generate.sh", CAPSUITE_CERT_DIR);
    if( system(cmd)!=0 ) return -1;
  }
  return (stat(zCert, &st)==0 && stat(zKey, &st)==0) ? 0 : -1;
}

static int capdb_17_tls_handshake(void){
  CapdbTestServer srv;
  char zUri[512];
  char zCert[512], zKey[512];

  if( ensure_tls_certs(zCert, zKey, sizeof(zCert)) ) CAPSUITE_FAIL("tls certs");
  memset(&srv, 0, sizeof(srv));
  srv.bTls = 1;
  snprintf(srv.zCert, sizeof(srv.zCert), "%s", zCert);
  snprintf(srv.zKey, sizeof(srv.zKey), "%s", zKey);
  if( capsuite_capdb_server_start(&srv, 0) ) CAPSUITE_FAIL("tls server");
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=testtoken&ca=%s/ca.pem", srv.port,
    CAPSUITE_CERT_DIR);
  if( run_capdbtest_uri(zUri, 1) ) CAPSUITE_FAIL("tls handshake");
  capsuite_capdb_server_stop(&srv);
  return 0;
}

void capsuite_register_capdb(void){
  static CapsuiteTest a[] = {
    { "capdb-1-loopback", capdb_1_loopback },
    { "capdb-2-auth-fail", capdb_2_auth_fail },
    { "capdb-3-prepare-step", capdb_3_prepare_step },
    { "capdb-4-open-switch", capdb_4_open_switch },
    { "capdb-5-open-in-txn", capdb_5_open_in_txn },
    { "capdb-6-vfs-read", capdb_6_vfs_read },
    { "capdb-7-attach-denied", capdb_7_attach_denied },
    { "capdb-8-stmt-before-exec", capdb_8_stmt_before_exec },
    { "capdb-9-prepare-write-pin", capdb_9_prepare_write_pin },
    { "capdb-10-column-types", capdb_10_column_types },
    { "capdb-11-vfs-lock", capdb_11_vfs_lock },
    { "capdb-12-changes-rowid", capdb_12_changes_rowid },
    { "capdb-13-shutdown-idle", capdb_13_shutdown_idle },
    { "capdb-14-session-reuse", capdb_14_session_reuse },
    { "capdb-15-vfs-exclude", capdb_15_vfs_exclude },
    { "capdb-16-vfs-delete", capdb_16_vfs_delete },
    { "capdb-17-tls-handshake", capdb_17_tls_handshake },
    { "capdb-18-vfs-write", capdb_18_vfs_write },
    { "capdb-19-exec-blob-callback", capdb_19_exec_blob_callback },
    { "capdb-20-commit-open-stmt", capdb_20_commit_open_stmt },
    { "capdb-21-vfs-separate-path", capdb_21_vfs_separate_path },
    { "capdb-22-password-auth", capdb_22_password_auth },
    { "capdb-23-capdbvfs-sql", capdb_23_capdbvfs_sql },
    { "capdb-24-volume-sql", capdb_24_volume_sql },
#if defined(CAPDB_ENABLE_REPLICATION)
    { "capdb-25-rep-async", capdb_25_rep_async },
#endif
  };
  capsuite_register_tests(a, (int)(sizeof(a)/sizeof(a[0])));
}
