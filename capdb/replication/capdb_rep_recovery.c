
#if defined(CAPDB_ENABLE_REPLICATION)

#include "capdb_rep_recovery.h"
#include "capdb.h"
#include "../store/capdb_store.h"
#include "../store/capdb_store_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

int capdb_rep_apply_wal_payload(capdb_volume *pVol, const CapdbStoreWalHdr *wh,
                                const void *payload, int nPayload){
  char zVolPath[1024];
  char zSqlWal[1024];
  int fd;
  ssize_t w;
  if( pVol==0 || wh==0 || payload==0 || nPayload<=0 ) return CAPDB_MISUSE;
  if( wh->magic!=CAPDB_STORE_WAL_MAGIC || wh->payload_len!=(unsigned)nPayload ){
    return CAPDB_CORRUPT;
  }
  if( capdb_store_crc32(payload, nPayload)!=wh->crc32 ) return CAPDB_CORRUPT;
  {
    int localGen = capdb_volume_generation(pVol);
    if( (int)wh->generation < localGen ) return CAPDB_READONLY;
    if( (int)wh->generation > localGen ) return CAPDB_READONLY;
  }
  snprintf(zVolPath, sizeof(zVolPath), "%s", capdb_volume_path(pVol));
  if( wh->wal_offset==CAPDB_STORE_WAL_OFFSET_MAIN_DB ){
    char zSqlMain[1024];
    char zSqlWal[1024];
    char zSqlShm[1024];
    snprintf(zSqlMain, sizeof(zSqlMain), "%s/" CAPDB_STORE_MAIN_DB, zVolPath);
    snprintf(zSqlWal, sizeof(zSqlWal), "%s/" CAPDB_STORE_MAIN_DB "-wal", zVolPath);
    snprintf(zSqlShm, sizeof(zSqlShm), "%s/" CAPDB_STORE_MAIN_DB "-shm", zVolPath);
    unlink(zSqlWal);
    unlink(zSqlShm);
    fd = open(zSqlMain, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if( fd<0 ) return CAPDB_IOERR;
    w = write(fd, payload, (size_t)nPayload);
    if( w==nPayload ) w = fsync(fd)==0 ? nPayload : -1;
    close(fd);
    if( w!=nPayload ) return CAPDB_IOERR;
  }else{
    snprintf(zSqlWal, sizeof(zSqlWal), "%s/" CAPDB_STORE_MAIN_DB "-wal", zVolPath);
    fd = open(zSqlWal, O_RDWR|O_CREAT, 0644);
    if( fd<0 ) return CAPDB_IOERR;
    w = pwrite(fd, payload, (size_t)nPayload, (off_t)wh->wal_offset);
    if( w==nPayload ){
      (void)fsync(fd);
    }else{
      close(fd);
      return CAPDB_IOERR;
    }
    close(fd);
  }
  {
    unsigned long long snap = 0;
    capdb_volume_checkpoint(pVol, &snap);
  }
  return CAPDB_OK;
}

typedef struct WalSeg WalSeg;
struct WalSeg {
  unsigned long long lsn;
  char zPath[1200];
};

static int walSegCmp(const void *a, const void *b){
  const WalSeg *pa = (const WalSeg*)a;
  const WalSeg *pb = (const WalSeg*)b;
  if( pa->lsn < pb->lsn ) return -1;
  if( pa->lsn > pb->lsn ) return 1;
  return 0;
}

int capdb_rep_recovery_replay_dir(capdb_volume *pVol){
  char zWalDir[1024];
  DIR *d;
  struct dirent *e;
  WalSeg *aSeg = 0;
  int nSeg = 0;
  int nAlloc = 0;
  int i;
  capdb *db = 0;
  char *zErr = 0;
  if( pVol==0 ) return CAPDB_MISUSE;
  snprintf(zWalDir, sizeof(zWalDir), "%s", capdb_volume_wal_dir(pVol));
  d = opendir(zWalDir);
  if( d==0 ) return CAPDB_OK;
  while( (e=readdir(d))!=0 ){
    unsigned long long lsn = 0;
    WalSeg *slot;
    if( sscanf(e->d_name, "%08llu.wal", &lsn)!=1 ) continue;
    if( nSeg>=nAlloc ){
      nAlloc = nAlloc ? nAlloc*2 : 32;
      aSeg = (WalSeg*)realloc(aSeg, (size_t)nAlloc*sizeof(WalSeg));
      if( aSeg==0 ){ closedir(d); return CAPDB_NOMEM; }
    }
    slot = &aSeg[nSeg++];
    slot->lsn = lsn;
    snprintf(slot->zPath, sizeof(slot->zPath), "%s/%s", zWalDir, e->d_name);
  }
  closedir(d);
  if( nSeg==0 ){ free(aSeg); return CAPDB_OK; }
  qsort(aSeg, (size_t)nSeg, sizeof(WalSeg), walSegCmp);
  for(i=0; i<nSeg; i++){
    CapdbStoreWalHdr hdr;
    unsigned char *payload = 0;
    ssize_t nHdr, nBody;
    int fd, rc;
    fd = open(aSeg[i].zPath, O_RDONLY);
    if( fd<0 ) continue;
    nHdr = read(fd, &hdr, sizeof(hdr));
    if( nHdr!=(ssize_t)sizeof(hdr) || hdr.magic!=CAPDB_STORE_WAL_MAGIC
     || hdr.payload_len==0 ){
      close(fd);
      continue;
    }
    payload = (unsigned char*)malloc(hdr.payload_len);
    if( payload==0 ){ close(fd); free(aSeg); return CAPDB_NOMEM; }
    nBody = read(fd, payload, hdr.payload_len);
    close(fd);
    if( nBody!=(ssize_t)hdr.payload_len ){
      free(payload);
      continue;
    }
    rc = capdb_rep_apply_wal_payload(pVol, &hdr, payload, (int)nBody);
    free(payload);
    if( rc!=CAPDB_OK ){
      free(aSeg);
      return rc;
    }
  }
  free(aSeg);
  if( capdb_open_v2(capdb_volume_main_db_path(pVol), &db,
      CAPDB_OPEN_READWRITE, 0)==CAPDB_OK ){
    capdb_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, &zErr);
    capdb_free(zErr);
    capdb_close_v2(db);
  }
  return CAPDB_OK;
}

#endif /* CAPDB_ENABLE_REPLICATION */
