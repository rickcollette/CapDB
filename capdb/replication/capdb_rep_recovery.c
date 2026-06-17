
#if defined(CAPDB_ENABLE_REPLICATION)

#include "capdb_rep.h"
#include "capdb.h"
#include "../store/capdb_store.h"
#include "../store/capdb_store_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

int capdb_rep_recovery_replay_dir(capdb_volume *pVol){
  char zWalDir[1024];
  DIR *d;
  struct dirent *e;
  if( pVol==0 ) return CAPDB_MISUSE;
  snprintf(zWalDir, sizeof(zWalDir), "%s", capdb_volume_wal_dir(pVol));
  d = opendir(zWalDir);
  if( d==0 ) return CAPDB_OK;
  while( (e=readdir(d))!=0 ){
    char zPath[1200];
    CapdbStoreWalHdr hdr;
    int fd;
    ssize_t n;
    if( e->d_name[0]=='.' ) continue;
    snprintf(zPath, sizeof(zPath), "%s/%s", zWalDir, e->d_name);
    fd = open(zPath, O_RDONLY);
    if( fd<0 ) continue;
    n = read(fd, &hdr, sizeof(hdr));
    close(fd);
    if( n==(ssize_t)sizeof(hdr) && hdr.magic==CAPDB_STORE_WAL_MAGIC ){
      unsigned long long snap = 0;
      capdb_volume_checkpoint(pVol, &snap);
    }
  }
  closedir(d);
  return CAPDB_OK;
}

#endif /* CAPDB_ENABLE_REPLICATION */
