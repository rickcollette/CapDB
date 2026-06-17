/*
** Standalone loopback smoke test for libcapdb_client.
*/
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_client.h"
#include "capdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int gRows;

static int countCallback(void *pArg, int nCol, char **azVal, char **azCol){
  (void)pArg; (void)nCol; (void)azCol;
  if( azVal && azVal[0] ) gRows++;
  return 0;
}

static int run_prepare_path(capdb_conn *p){
  capdb_net_stmt *st = 0;
  int rc;
  rc = capdb_net_prepare(p, "SELECT y FROM t1 WHERE x=1", &st);
  if( rc!=CAPDB_NET_OK || st==0 ) return -1;
  rc = capdb_net_step(st);
  if( rc!=100 ) return -1;
  if( capdb_net_column_text(st, 0)==0 ) return -1;
  rc = capdb_net_finalize(st);
  return rc==CAPDB_NET_OK ? 0 : -1;
}

int main(int argc, char **argv){
  capdb_conn *p = 0;
  const char *zUri;
  int rc;

  if( argc<2 ){
    fprintf(stderr, "Usage: %s URI\n", argv[0]);
    return 1;
  }
  zUri = argv[1];
  rc = capdb_net_connect(zUri, &p);
  if( rc!=CAPDB_NET_OK ){
    fprintf(stderr, "connect failed: %d %s\n", rc, capdb_net_errmsg(p));
    capdb_net_close(p);
    return 1;
  }
  rc = capdb_net_exec(p, "CREATE TABLE IF NOT EXISTS t1(x INTEGER PRIMARY KEY, y TEXT)", 0, 0);
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "create failed: %s\n", capdb_net_errmsg(p));
    capdb_net_close(p);
    return 1;
  }
  rc = capdb_net_exec(p, "INSERT OR REPLACE INTO t1 VALUES(1,'one')", 0, 0);
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "insert failed: %s\n", capdb_net_errmsg(p));
    capdb_net_close(p);
    return 1;
  }
  gRows = 0;
  rc = capdb_net_exec(p, "SELECT y FROM t1 WHERE x=1", countCallback, 0);
  if( rc!=CAPDB_OK || gRows!=1 ){
    fprintf(stderr, "select failed: rc=%d rows=%d\n", rc, gRows);
    capdb_net_close(p);
    return 1;
  }
  if( run_prepare_path(p) ){
    fprintf(stderr, "prepare/step failed\n");
    capdb_net_close(p);
    return 1;
  }
  capdb_net_close(p);
  printf("capdbtest ok\n");
  return 0;
}

#else
#include <stdio.h>
int main(void){
  fprintf(stderr, "capdbtest requires CAPDB_ENABLE_NETWORK\n");
  return 1;
}
#endif
