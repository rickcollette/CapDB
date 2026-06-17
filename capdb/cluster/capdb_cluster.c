
#if defined(CAPDB_ENABLE_STORE)

#include "capdb_cluster.h"
#include "capdb.h"
#include "../store/capdb_store.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(CAPDB_ENABLE_REPLICATION)
#include "../replication/capdb_rep_recovery.h"
#endif

int capdb_cluster_role_name(int role, char *zBuf, int nBuf){
  const char *z = "unknown";
  switch( role ){
    case CAPDB_CLUSTER_ROLE_PRIMARY: z = "primary"; break;
    case CAPDB_CLUSTER_ROLE_REPLICA: z = "replica"; break;
    case CAPDB_CLUSTER_ROLE_DEMOTED: z = "demoted"; break;
    case CAPDB_CLUSTER_ROLE_RECOVERING: z = "recovering"; break;
  }
  if( zBuf==0 || nBuf<=0 ) return CAPDB_MISUSE;
  snprintf(zBuf, (size_t)nBuf, "%s", z);
  return CAPDB_OK;
}

int capdb_cluster_promote(const char *zVolumePath, int waitMs){
  capdb_volume *pVol = 0;
  int gen;
  (void)waitMs;
  if( zVolumePath==0 ) return CAPDB_MISUSE;
  if( capdb_volume_open(zVolumePath, 0, &pVol)!=CAPDB_OK ) return CAPDB_CANTOPEN;
#if defined(CAPDB_ENABLE_REPLICATION)
  capdb_rep_recovery_replay_dir(pVol);
#endif
  gen = capdb_volume_generation(pVol) + 1;
  capdb_volume_set_generation(pVol, gen);
  capdb_volume_close(pVol);
  return CAPDB_OK;
}

int capdb_cluster_status_fill(const char *zVolumePath, capdb_cluster_status *p){
  capdb_volume *pVol = 0;
  if( zVolumePath==0 || p==0 ) return CAPDB_MISUSE;
  memset(p, 0, sizeof(*p));
  p->zVolumePath = zVolumePath;
  if( capdb_volume_open(zVolumePath, CAPDB_VOLUME_OPEN_RDONLY, &pVol)!=CAPDB_OK ){
    return CAPDB_CANTOPEN;
  }
  p->generation = capdb_volume_generation(pVol);
  p->lsn = capdb_volume_lsn(pVol);
  p->applied_lsn = capdb_volume_applied_lsn(pVol);
  if( p->lsn > p->applied_lsn ){
    p->lag_bytes = p->lsn - p->applied_lsn;
  }
  p->role = CAPDB_CLUSTER_ROLE_PRIMARY;
  capdb_volume_close(pVol);
  return CAPDB_OK;
}

#endif /* CAPDB_ENABLE_STORE */
