/*
** capdb-ctl — cluster and volume administration.
*/
#if defined(CAPDB_ENABLE_STORE)

#include "capdb.h"
#include "capdb/cluster/capdb_cluster.h"
#include "capdb/store/capdb_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *zProg){
  fprintf(stderr,
    "Usage: %s <command> [options]\n"
    "  status --volume PATH\n"
    "  promote --volume PATH [--wait MS]\n"
    "  backup --volume PATH --out FILE.db\n"
    "  export --volume PATH --out FILE.db\n",
    zProg);
}

int main(int argc, char **argv){
  const char *zCmd = 0;
  const char *zVolume = 0;
  const char *zOut = 0;
  int waitMs = 0;
  int i;
  if( argc<2 ){
    usage(argv[0]);
    return 1;
  }
  zCmd = argv[1];
  for(i=2; i<argc; i++){
    if( strcmp(argv[i],"--volume")==0 && i+1<argc ) zVolume = argv[++i];
    else if( strcmp(argv[i],"--out")==0 && i+1<argc ) zOut = argv[++i];
    else if( strcmp(argv[i],"--wait")==0 && i+1<argc ) waitMs = atoi(argv[++i]);
    else { usage(argv[0]); return 1; }
  }
  if( strcmp(zCmd,"status")==0 ){
    capdb_cluster_status st;
    char zRole[32];
    if( zVolume==0 ){ usage(argv[0]); return 1; }
    if( capdb_cluster_status_fill(zVolume, &st) ){
      fprintf(stderr, "error: cannot read volume %s\n", zVolume);
      return 1;
    }
    capdb_cluster_role_name(st.role, zRole, sizeof(zRole));
    printf("volume: %s\nrole: %s\ngeneration: %d\nlsn: %llu\napplied_lsn: %llu\nlag_bytes: %llu\n",
           zVolume, zRole, st.generation,
           st.lsn, st.applied_lsn, st.lag_bytes);
    return 0;
  }
  if( strcmp(zCmd,"promote")==0 ){
    if( zVolume==0 ){ usage(argv[0]); return 1; }
    if( capdb_cluster_promote(zVolume, waitMs) ){
      fprintf(stderr, "error: promote failed\n");
      return 1;
    }
    printf("promoted %s\n", zVolume);
    return 0;
  }
  if( strcmp(zCmd,"backup")==0 || strcmp(zCmd,"export")==0 ){
    capdb_volume *pVol = 0;
    if( zVolume==0 || zOut==0 ){ usage(argv[0]); return 1; }
    if( capdb_volume_open(zVolume, 0, &pVol)!=CAPDB_OK ){
      fprintf(stderr, "error: cannot open volume\n");
      return 1;
    }
    if( capdb_volume_export_db(pVol, zOut) ){
      capdb_volume_close(pVol);
      fprintf(stderr, "error: export failed\n");
      return 1;
    }
    capdb_volume_close(pVol);
    printf("exported %s -> %s\n", zVolume, zOut);
    return 0;
  }
  usage(argv[0]);
  return 1;
}

#else
#include <stdio.h>
int main(void){
  fprintf(stderr, "capdb-ctl requires CAPDB_ENABLE_STORE\n");
  return 1;
}
#endif
