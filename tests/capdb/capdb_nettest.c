/*
** In-process network regression tests (libcapdb_client only).
** Usage: capdb_nettest <test-name> <uri>
**
** shutdown-idle spawns its own server (ignores uri).
*/
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef CAPSUITE_SERVER
#define CAPSUITE_SERVER "capdb-server"
#endif

static const char *server_bin(void){
  const char *z = getenv("CAPSUITE_SERVER");
  return z && z[0] ? z : CAPSUITE_SERVER;
}

static int nettest_rm_rf(const char *zPath){
  struct stat st;
  if( zPath==0 || zPath[0]==0 ) return 0;
  if( stat(zPath, &st)!=0 ) return 0;
  if( S_ISDIR(st.st_mode) ){
    DIR *d = opendir(zPath);
    if( d ){
      struct dirent *e;
      char buf[1024];
      while( (e=readdir(d))!=0 ){
        if( strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0 ) continue;
        snprintf(buf, sizeof(buf), "%s/%s", zPath, e->d_name);
        nettest_rm_rf(buf);
      }
      closedir(d);
    }
    return rmdir(zPath);
  }
  return unlink(zPath);
}

static int nettest_mkdir_p(const char *zPath){
  char buf[1024];
  size_t i, n;
  if( zPath==0 ) return -1;
  n = strlen(zPath);
  if( n>=sizeof(buf) ) return -1;
  memcpy(buf, zPath, n+1);
  for(i=1; i<n; i++){
    if( buf[i]=='/' ){
      buf[i] = 0;
      if( mkdir(buf, 0755)!=0 && errno!=EEXIST ) return -1;
      buf[i] = '/';
    }
  }
  if( mkdir(buf, 0755)!=0 && errno!=EEXIST ) return -1;
  return 0;
}

static int port_is_free(int port){
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  if( fd<0 ) return 0;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if( bind(fd, (struct sockaddr*)&addr, sizeof(addr))==0 ){
    close(fd);
    return 1;
  }
  close(fd);
  return 0;
}

static int pick_free_port(void){
  int port, i;
  for(i=0; i<200; i++){
    port = 20000 + ((getpid() + i*997) % 30000);
    if( port_is_free(port) ) return port;
  }
  return 0;
}

static int test_volume_multi_write(const char *zUri){
  capdb_conn *p = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS mw(v INTEGER)", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  if( capdb_net_exec(p, "DELETE FROM mw", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  if( capdb_net_exec(p, "INSERT INTO mw VALUES(1)", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  if( capdb_net_exec(p, "INSERT INTO mw VALUES(2)", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  capdb_net_close(p);
  return 0;
}

static int test_prepare_step(const char *zUri){
  capdb_conn *p = 0;
  capdb_net_stmt *st = 0;
  int rc;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS ps(x INTEGER, y TEXT)", 0, 0) ){
    fprintf(stderr, "create fail\n"); return 1;
  }
  if( capdb_net_exec(p, "INSERT INTO ps VALUES(1,'alpha')", 0, 0) ){
    fprintf(stderr, "insert fail\n"); return 1;
  }
  rc = capdb_net_prepare(p, "SELECT y FROM ps WHERE x=1", &st);
  if( rc!=CAPDB_NET_OK || st==0 ){ fprintf(stderr, "prep fail rc=%d\n", rc); return 1; }
  rc = capdb_net_step(st);
  if( rc!=100 ){ fprintf(stderr, "step fail rc=%d\n", rc); return 1; }
  {
    const unsigned char *z = capdb_net_column_text(st, 0);
    if( z==0 || strcmp((const char*)z, "alpha")!=0 ) return 1;
  }
  if( capdb_net_step(st)!=101 ) return 1;
  if( capdb_net_finalize(st)!=CAPDB_NET_OK ) return 1;
  capdb_net_close(p);
  return 0;
}

static int test_open_switch(const char *zUri){
  capdb_conn *p = 0;
  capdb_net_stmt *st = 0;
  signal(SIGPIPE, SIG_IGN);
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS a1(v INTEGER)", 0, 0) ) return 1;
  if( capdb_net_prepare(p, "SELECT v FROM a1", &st) ) return 1;
  if( capdb_net_open_db(p, "/other.db") ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS b1(v INTEGER)", 0, 0) ) return 1;
  if( capdb_net_step(st)==100 ) return 1;
  capdb_net_finalize(st);
  capdb_net_close(p);
  return 0;
}

static int test_open_in_txn(const char *zUri){
  capdb_conn *p = 0;
  int rc;
  signal(SIGPIPE, SIG_IGN);
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "BEGIN", 0, 0) ) return 1;
  rc = capdb_net_open_db(p, "/other.db");
  if( rc==CAPDB_NET_OK ) return 1;
  if( capdb_net_exec(p, "ROLLBACK", 0, 0) ) return 1;
  capdb_net_close(p);
  return 0;
}

static int test_vfs_read(const char *zUri){
  capdb_conn *p = 0;
  int fid = -1;
  unsigned char buf[16];
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS vf(x)", 0, 0) ) return 1;
  if( capdb_net_vfs_open(p, "/vfsnet.db", &fid) ) return 1;
  if( capdb_net_vfs_read(p, fid, 0, buf, 16) ) return 1;
  capdb_net_vfs_close(p, fid);
  capdb_net_close(p);
  return 0;
}

static int test_vfs_write(const char *zUri){
  capdb_conn *p = 0;
  int fid = -1;
  unsigned char wr[8] = { 'C','a','p','D','B',0,0,0 };
  unsigned char rd[8];
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_vfs_open(p, "/vfsnet.db", &fid) ) return 1;
  if( capdb_net_vfs_write(p, fid, 0, wr, 8) ) return 1;
  memset(rd, 0, sizeof(rd));
  if( capdb_net_vfs_read(p, fid, 0, rd, 8) ) return 1;
  if( memcmp(wr, rd, 8)!=0 ) return 1;
  capdb_net_vfs_close(p, fid);
  capdb_net_close(p);
  return 0;
}

static int blobExecCb(void *pArg, int nCol, char **azVal, char **azCol){
  (void)pArg; (void)azCol;
  if( nCol<1 || azVal[0]==0 ) return 1;
  return strcmp(azVal[0], "0102")==0 ? 0 : 1;
}

static int test_exec_blob_callback(const char *zUri){
  capdb_conn *p = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS bb(b BLOB)", 0, 0) ) return 1;
  if( capdb_net_exec(p, "INSERT INTO bb VALUES(X'0102')", 0, 0) ) return 1;
  if( capdb_net_exec(p, "SELECT b FROM bb", blobExecCb, 0) ) return 1;
  capdb_net_close(p);
  return 0;
}

static int test_commit_open_stmt(const char *zUri){
  capdb_conn *p = 0;
  capdb_net_stmt *st = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS cos(v INTEGER)", 0, 0) ) return 1;
  /* BEGIN/COMMIT require no pinned prepared statements (server v4 contract). */
  if( capdb_net_exec(p, "BEGIN", 0, 0) ) return 1;
  if( capdb_net_exec(p, "COMMIT", 0, 0) ) return 1;
  if( capdb_net_prepare(p, "SELECT v FROM cos", &st)!=CAPDB_NET_OK || st==0 ) return 1;
  if( capdb_net_step(st)!=101 ) return 1; /* CAPDB_DONE on empty table */
  capdb_net_finalize(st);
  capdb_net_close(p);
  return 0;
}

static int test_vfs_separate_path(const char *zUri){
  capdb_conn *p = 0;
  int fid = -1;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS sp(x)", 0, 0) ) return 1;
  if( capdb_net_vfs_open(p, "/mnet.db", &fid)==CAPDB_NET_OK ) return 1;
  if( capdb_net_vfs_open(p, "/vfsnet.db", &fid) ) return 1;
  capdb_net_vfs_close(p, fid);
  capdb_net_close(p);
  return 0;
}

static int test_password_auth(const char *zUri){
  capdb_conn *p = 0;
  (void)zUri;
  {
    char zDir[128], zAuth[160], zListen[64], zUri2[256], zUri3[256];
    const char *argv[20];
    pid_t pid;
    int port, i, st, waited;

    port = pick_free_port();
    if( port<=0 ) return 1;
    snprintf(zDir, sizeof(zDir), "capdb_pwd_%d", port);
    snprintf(zAuth, sizeof(zAuth), "%s/auth.txt", zDir);
    snprintf(zListen, sizeof(zListen), "127.0.0.1:%d", port);
    snprintf(zUri2, sizeof(zUri2),
      "capdb://capuser:capsecret@127.0.0.1:%d/mnet.db?insecure=1", port);
    snprintf(zUri3, sizeof(zUri3),
      "capdb://127.0.0.1:%d/mnet.db?token=hashtoken&insecure=1", port);

    nettest_rm_rf(zDir);
    if( nettest_mkdir_p(zDir) ) return 1;
    {
      FILE *f = fopen(zAuth, "w");
      if( f==0 ) return 1;
      fputs("capuser:sha256:deb118697ff047e9604d2b4913cec462ade29fbfe1a954858b825630960e177f\n", f);
      fputs("sha256:3417e614ffc302bd9db8f3b86b9016cb0e50423522f457ff5e2c1c20a7410dda\n", f);
      fclose(f);
    }

    i = 0;
    argv[i++] = server_bin();
    argv[i++] = "--listen";
    argv[i++] = zListen;
    argv[i++] = "--auth-file";
    argv[i++] = zAuth;
    argv[i++] = "--db-root";
    argv[i++] = zDir;
    argv[i++] = "--insecure";
    argv[i++] = 0;

    pid = fork();
    if( pid<0 ) return 1;
    if( pid==0 ){
      execv(argv[0], (char*const*)argv);
      _exit(127);
    }

    for(waited=0; waited<50; waited++){
      if( kill(pid, 0)!=0 ) return 1;
      if( !port_is_free(port) ) break;
      usleep(100000);
    }
    if( port_is_free(port) ){
      kill(pid, SIGKILL);
      waitpid(pid, 0, 0);
      return 1;
    }

    if( capdb_net_connect(zUri2, &p) ) return 1;
    if( capdb_net_exec(p, "SELECT 1", 0, 0) ) return 1;
    capdb_net_close(p);
    p = 0;
    if( capdb_net_connect(zUri3, &p) ) return 1;
    if( capdb_net_exec(p, "SELECT 1", 0, 0) ) return 1;
    capdb_net_close(p);

    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    nettest_rm_rf(zDir);
    return (WIFEXITED(st) && WEXITSTATUS(st)==0) ? 0 : 1;
  }
}

static int test_attach_denied(const char *zUri){
  capdb_conn *p = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "ATTACH DATABASE '/tmp/x.db' AS x", 0, 0)==CAPDB_NET_OK ) return 1;
  capdb_net_close(p);
  return 0;
}

static int test_stmt_before_exec(const char *zUri){
  capdb_conn *p = 0;
  capdb_net_stmt *st = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS sbe(v INTEGER)", 0, 0) ) return 1;
  if( capdb_net_prepare(p, "SELECT v FROM sbe", &st)!=CAPDB_NET_OK || st==0 ) return 1;
  if( capdb_net_exec(p, "INSERT INTO sbe VALUES(1)", 0, 0)==CAPDB_NET_OK ) return 1;
  capdb_net_finalize(st);
  capdb_net_close(p);
  return 0;
}

static int test_prepare_write_pin(const char *zUri){
  capdb_conn *p1 = 0, *p2 = 0;
  capdb_net_stmt *stW = 0, *stR = 0;
  char zUri2[512];
  const char *q;
  if( capdb_net_connect(zUri, &p1) ) return 1;
  q = strchr(zUri, '?');
  if( q ){
    snprintf(zUri2, sizeof(zUri2), "%.*s?%s", (int)(q-zUri), zUri, q+1);
  }else{
    snprintf(zUri2, sizeof(zUri2), "%s", zUri);
  }
  if( capdb_net_connect(zUri2, &p2) ) return 1;
  if( capdb_net_exec(p1, "CREATE TABLE IF NOT EXISTS pwp(v INTEGER)", 0, 0) ) return 1;
  if( capdb_net_prepare(p1, "INSERT INTO pwp VALUES(1)", &stW)!=CAPDB_NET_OK || stW==0 )
    return 1;
  /* Write checkout stays pinned; read checkouts on another connection still work. */
  if( capdb_net_prepare(p2, "SELECT v FROM pwp", &stR)!=CAPDB_NET_OK || stR==0 ) return 1;
  capdb_net_finalize(stR);
  capdb_net_finalize(stW);
  capdb_net_close(p1);
  capdb_net_close(p2);
  return 0;
}

static int test_column_types(const char *zUri){
  capdb_conn *p = 0;
  capdb_net_stmt *st = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p,
      "CREATE TABLE IF NOT EXISTS ct(i INTEGER, f REAL, t TEXT, b BLOB)", 0, 0) ) return 1;
  if( capdb_net_exec(p,
      "INSERT INTO ct VALUES(42, 3.5, 'hi', X'0102')", 0, 0) ) return 1;
  if( capdb_net_prepare(p, "SELECT i,f,t,b FROM ct", &st) ) return 1;
  if( capdb_net_step(st)!=100 ) return 1;
  if( capdb_net_column_int64(st, 0)!=42 ) return 1;
  if( capdb_net_column_double(st, 1)<3.49 || capdb_net_column_double(st, 1)>3.51 ) return 1;
  {
    const unsigned char *z = capdb_net_column_text(st, 2);
    if( z==0 || strcmp((const char*)z, "hi")!=0 ) return 1;
  }
  if( capdb_net_column_bytes(st, 3)!=2 ) return 1;
  {
    const void *b = capdb_net_column_blob(st, 3);
    const unsigned char *a = (const unsigned char*)b;
    if( b==0 || a[0]!=1 || a[1]!=2 ) return 1;
  }
  if( capdb_net_step(st)!=101 ) return 1;
  capdb_net_finalize(st);
  capdb_net_close(p);
  return 0;
}

static int test_vfs_lock(const char *zUri){
  capdb_conn *p = 0;
  int fid = -1;
  int reserved = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_vfs_open(p, "/vfsnet.db", &fid) ) return 1;
  if( capdb_net_vfs_lock(p, fid, 1)!=CAPDB_NET_OK ) return 1;      /* SHARED */
  if( capdb_net_vfs_lock(p, fid, 2)!=CAPDB_NET_OK ) return 1;      /* RESERVED */
  if( capdb_net_vfs_check_reserved(p, fid, &reserved) ) return 1;
  if( !reserved ) return 1;
  if( capdb_net_vfs_lock(p, fid, 4)!=CAPDB_NET_OK ) return 1;      /* EXCLUSIVE */
  if( capdb_net_vfs_lock(p, fid, 0)!=CAPDB_NET_OK ) return 1;      /* NONE */
  capdb_net_vfs_close(p, fid);
  capdb_net_close(p);
  return 0;
}

static int test_changes_rowid(const char *zUri){
  capdb_conn *p = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS cr(id INTEGER PRIMARY KEY, v)", 0, 0) )
    return 1;
  if( capdb_net_exec(p, "INSERT INTO cr(v) VALUES('x')", 0, 0) ) return 1;
  if( capdb_net_changes(p)!=1 ) return 1;
  if( capdb_net_last_insert_rowid(p)!=1 ) return 1;
  capdb_net_close(p);
  return 0;
}

static int test_vfs_pool_deny(const char *zUri){
  capdb_conn *p = 0;
  int fid = -1;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS pd(x)", 0, 0) ) return 1;
  if( capdb_net_vfs_open(p, "/mnet.db", &fid)==CAPDB_NET_OK ) return 1;
  if( capdb_net_vfs_open(p, "/vfsnet.db", &fid) ) return 1;
  capdb_net_vfs_close(p, fid);
  capdb_net_close(p);
  return 0;
}

static int test_session_reuse(void){
  char zDir[128], zAuth[160], zListen[64], zUri[256];
  const char *argv[20];
  pid_t pid;
  capdb_conn *p = 0;
  int port, i, st, n;
  int waited;

  port = pick_free_port();
  if( port<=0 ) return 1;
  snprintf(zDir, sizeof(zDir), "capdb_reuse_%d", port);
  snprintf(zAuth, sizeof(zAuth), "%s/auth.txt", zDir);
  snprintf(zListen, sizeof(zListen), "127.0.0.1:%d", port);
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=reusetoken&insecure=1", port);

  nettest_rm_rf(zDir);
  if( nettest_mkdir_p(zDir) ) return 1;
  {
    FILE *f = fopen(zAuth, "w");
    if( f==0 ) return 1;
    fputs("reusetoken\n", f);
    fclose(f);
  }

  i = 0;
  argv[i++] = server_bin();
  argv[i++] = "--listen";
  argv[i++] = zListen;
  argv[i++] = "--auth-file";
  argv[i++] = zAuth;
  argv[i++] = "--db-root";
  argv[i++] = zDir;
  argv[i++] = "--max-clients";
  argv[i++] = "2";
  argv[i++] = "--insecure";
  argv[i++] = 0;

  pid = fork();
  if( pid<0 ) return 1;
  if( pid==0 ){
    execv(argv[0], (char*const*)argv);
    _exit(127);
  }

  for(waited=0; waited<50; waited++){
    if( kill(pid, 0)!=0 ) return 1;
    if( !port_is_free(port) ) break;
    usleep(100000);
  }
  if( port_is_free(port) ){
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    return 1;
  }

  for(n=0; n<5; n++){
    if( capdb_net_connect(zUri, &p) ) return 1;
    if( capdb_net_exec(p, "SELECT 1", 0, 0) ) return 1;
    capdb_net_close(p);
    p = 0;
  }

  kill(pid, SIGTERM);
  for(waited=0; waited<50; waited++){
    pid_t w = waitpid(pid, &st, WNOHANG);
    if( w>0 ){
      nettest_rm_rf(zDir);
      return (WIFEXITED(st) && WEXITSTATUS(st)==0) ? 0 : 1;
    }
    if( w<0 ) return 1;
    usleep(100000);
  }

  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
  nettest_rm_rf(zDir);
  return 1;
}

static int repSelectCb(void *pArg, int nCol, char **azCol, char **azRow){
  int *pFound = (int*)pArg;
  (void)nCol;
  (void)azRow;
  if( azCol && azCol[0] && atoi(azCol[0])==42 ) *pFound = 1;
  return 0;
}

static int test_rep_insert(const char *zUri){
  capdb_conn *p = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "PRAGMA journal_mode=WAL", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  if( capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS rep_t(v INTEGER)", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  if( capdb_net_exec(p, "DELETE FROM rep_t", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  if( capdb_net_exec(p, "INSERT INTO rep_t VALUES(42)", 0, 0) ){
    capdb_net_close(p);
    return 1;
  }
  capdb_net_close(p);
  return 0;
}

static int test_rep_select(const char *zUri){
  capdb_conn *p = 0;
  int found = 0;
  if( capdb_net_connect(zUri, &p) ) return 1;
  if( capdb_net_exec(p, "SELECT v FROM rep_t", repSelectCb, &found) ){
    capdb_net_close(p);
    return 1;
  }
  capdb_net_close(p);
  return found ? 0 : 1;
}

static int test_shutdown_idle(void){
  char zDir[128], zAuth[160], zListen[64], zUri[256];
  const char *argv[16];
  pid_t pid;
  capdb_conn *p = 0;
  int port, i, st;
  int waited;

  port = pick_free_port();
  if( port<=0 ) return 1;
  snprintf(zDir, sizeof(zDir), "capdb_idle_%d", port);
  snprintf(zAuth, sizeof(zAuth), "%s/auth.txt", zDir);
  snprintf(zListen, sizeof(zListen), "127.0.0.1:%d", port);
  snprintf(zUri, sizeof(zUri),
    "capdb://127.0.0.1:%d/mnet.db?token=idletoken&insecure=1", port);

  nettest_rm_rf(zDir);
  if( nettest_mkdir_p(zDir) ) return 1;
  {
    FILE *f = fopen(zAuth, "w");
    if( f==0 ) return 1;
    fputs("idletoken\n", f);
    fclose(f);
  }

  i = 0;
  argv[i++] = server_bin();
  argv[i++] = "--listen";
  argv[i++] = zListen;
  argv[i++] = "--auth-file";
  argv[i++] = zAuth;
  argv[i++] = "--db-root";
  argv[i++] = zDir;
  argv[i++] = "--insecure";
  argv[i++] = 0;

  pid = fork();
  if( pid<0 ) return 1;
  if( pid==0 ){
    execv(argv[0], (char*const*)argv);
    _exit(127);
  }

  for(waited=0; waited<50; waited++){
    if( kill(pid, 0)!=0 ) return 1;
    if( !port_is_free(port) ) break;
    usleep(100000);
  }
  if( port_is_free(port) ){
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    return 1;
  }

  if( capdb_net_connect(zUri, &p) ) return 1;
  /* idle: no further traffic */

  kill(pid, SIGTERM);
  for(waited=0; waited<50; waited++){
    pid_t w = waitpid(pid, &st, WNOHANG);
    if( w>0 ){
      capdb_net_close(p);
      nettest_rm_rf(zDir);
      return (WIFEXITED(st) && WEXITSTATUS(st)==0) ? 0 : 1;
    }
    if( w<0 ) return 1;
    usleep(100000);
  }

  capdb_net_close(p);
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
  return 1;
}

int main(int argc, char **argv){
  const char *zTest, *zUri;
  setvbuf(stderr, 0, _IONBF, 0);
  signal(SIGPIPE, SIG_IGN);
  if( argc<2 ){
    fprintf(stderr, "Usage: %s <test> [<uri>]\n", argv[0]);
    return 1;
  }
  zTest = argv[1];
  zUri = argc>=3 ? argv[2] : "";
  fprintf(stderr, "capdb_nettest: %s\n", zTest);
  fflush(stderr);
  if( strcmp(zTest,"shutdown-idle")==0 ) return test_shutdown_idle() ? 1 : 0;
  if( strcmp(zTest,"session-reuse")==0 ) return test_session_reuse() ? 1 : 0;
  if( zUri[0]==0 ){
    fprintf(stderr, "Usage: %s <test> <uri>\n", argv[0]);
    return 1;
  }
  if( strcmp(zTest,"prepare-step")==0 ) return test_prepare_step(zUri) ? 1 : 0;
  if( strcmp(zTest,"volume-multi-write")==0 ) return test_volume_multi_write(zUri) ? 1 : 0;
  if( strcmp(zTest,"open-switch")==0 ) return test_open_switch(zUri) ? 1 : 0;
  if( strcmp(zTest,"open-in-txn")==0 ) return test_open_in_txn(zUri) ? 1 : 0;
  if( strcmp(zTest,"vfs-read")==0 ) return test_vfs_read(zUri) ? 1 : 0;
  if( strcmp(zTest,"vfs-write")==0 ) return test_vfs_write(zUri) ? 1 : 0;
  if( strcmp(zTest,"exec-blob-callback")==0 ) return test_exec_blob_callback(zUri) ? 1 : 0;
  if( strcmp(zTest,"commit-open-stmt")==0 ) return test_commit_open_stmt(zUri) ? 1 : 0;
  if( strcmp(zTest,"vfs-separate-path")==0 ) return test_vfs_separate_path(zUri) ? 1 : 0;
  if( strcmp(zTest,"password-auth")==0 ) return test_password_auth(zUri) ? 1 : 0;
  if( strcmp(zTest,"attach-denied")==0 ) return test_attach_denied(zUri) ? 1 : 0;
  if( strcmp(zTest,"stmt-before-exec")==0 ) return test_stmt_before_exec(zUri) ? 1 : 0;
  if( strcmp(zTest,"prepare-write-pin")==0 ) return test_prepare_write_pin(zUri) ? 1 : 0;
  if( strcmp(zTest,"column-types")==0 ) return test_column_types(zUri) ? 1 : 0;
  if( strcmp(zTest,"vfs-lock")==0 ) return test_vfs_lock(zUri) ? 1 : 0;
  if( strcmp(zTest,"vfs-pool-deny")==0 ) return test_vfs_pool_deny(zUri) ? 1 : 0;
  if( strcmp(zTest,"changes-rowid")==0 ) return test_changes_rowid(zUri) ? 1 : 0;
  if( strcmp(zTest,"rep-insert")==0 ) return test_rep_insert(zUri) ? 1 : 0;
  if( strcmp(zTest,"rep-select")==0 ) return test_rep_select(zUri) ? 1 : 0;
  fprintf(stderr, "unknown test: %s\n", zTest);
  return 1;
}

#else
int main(void){ return 1; }
#endif
