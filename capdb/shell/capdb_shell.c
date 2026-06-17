
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int shellCapdbIsUri(const char *zName){
  return zName && strncmp(zName, "capdb://", 8)==0;
}

int shellCapdbOpen(const char *zUri, capdb_conn **pp){
  return capdb_net_connect(zUri, pp);
}

void shellCapdbClose(capdb_conn *p){
  capdb_net_close(p);
}

typedef struct ShellOutCtx {
  FILE *out;
} ShellOutCtx;

static int shellRowCallback(void *pArg, int nCol, char **azVal, char **azCol){
  ShellOutCtx *p = (ShellOutCtx*)pArg;
  int i;
  (void)azCol;
  if( p->out==0 ) return 0;
  for(i=0; i<nCol; i++){
    if( i>0 ) fputc('|', p->out);
    if( azVal[i] ) fputs(azVal[i], p->out);
  }
  fputc('\n', p->out);
  return 0;
}

int shellCapdbExec(capdb_conn *p, const char *zSql, FILE *out, char **pzErrMsg){
  ShellOutCtx ctx;
  int rc;
  if( pzErrMsg ) *pzErrMsg = 0;
  ctx.out = out;
  rc = capdb_net_exec(p, zSql, shellRowCallback, &ctx);
  if( rc!=CAPDB_OK && pzErrMsg ){
    const char *z = capdb_net_errmsg(p);
    if( z && z[0] ){
      *pzErrMsg = strdup(z);
    }
  }
  return rc==CAPDB_OK ? 0 : 1;
}

#endif /* CAPDB_ENABLE_NETWORK */
