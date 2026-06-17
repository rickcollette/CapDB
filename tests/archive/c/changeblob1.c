
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "capdb.h"

int main(void){
#ifdef CAPDB_ENABLE_SESSION
  capdb *db;
  capdb_changegroup *pGrp;
  char *zErr = 0;
  char *buf = malloc(64);
  int rc = CAPDB_OK;

  capdb_open(":memory:", &db);
  capdb_exec(db, "CREATE TABLE t1(a INTEGER PRIMARY KEY, b TEXT);", 0, 0, 0);

  capdb_changegroup_new(&pGrp);
  capdb_changegroup_schema(pGrp, db, "main");
  capdb_changegroup_change_begin(pGrp, CAPDB_INSERT, "t1", 0, &zErr);
  capdb_changegroup_change_int64(pGrp, 1, 0, 42);

  memset(buf, 'X', 64);

  /* This should return an OOM error: */
  rc = capdb_changegroup_change_blob(pGrp, 1, 1, buf, 2147483647);

  free(buf);
  capdb_changegroup_delete(pGrp);
  capdb_close(db);
  return (rc==7) ? 0 : -1;
#else
  return 0;
#endif
}
