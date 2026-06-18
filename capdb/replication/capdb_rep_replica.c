
#if defined(CAPDB_ENABLE_REPLICATION)

#include "capdb_rep.h"
#include "capdb_rep_io.h"
#include "capdb_rep_recovery.h"
#include "capdb.h"
#include "../store/capdb_store.h"
#include "../store/capdb_store_format.h"
#include "../tls/capdb_tls.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

struct capdb_rep_receiver {
  capdb_rep_config cfg;
  unsigned long long lagBytes;
  volatile int stop;
  pthread_t thread;
  pthread_mutex_t mutex;
};

int capdb_rep_replica_apply_chunk(const char *zVolumeRoot, const void *hdr,
                                  int nHdr, const void *payload, int nPayload){
  CapdbStoreWalHdr wh;
  capdb_volume *pVol = 0;
  char zVolPath[1024];
  char zSeg[1024];
  int fd;
  ssize_t w;
  int rc;
  if( zVolumeRoot==0 || hdr==0 || nHdr<(int)sizeof(wh) || payload==0 || nPayload<=0 ){
    return CAPDB_MISUSE;
  }
  memcpy(&wh, hdr, sizeof(wh));
  if( wh.magic!=CAPDB_STORE_WAL_MAGIC || wh.payload_len!=(unsigned)nPayload ){
    return CAPDB_CORRUPT;
  }
  if( capdb_store_crc32(payload, nPayload)!=wh.crc32 ) return CAPDB_CORRUPT;
  if( !capdb_store_vol_id_valid(wh.vol_id) ) return CAPDB_MISUSE;
  snprintf(zVolPath, sizeof(zVolPath), "%s/%s", zVolumeRoot, wh.vol_id);
  if( capdb_volume_open(zVolPath, CAPDB_VOLUME_OPEN_CREATE, &pVol)!=CAPDB_OK ){
    return CAPDB_CANTOPEN;
  }
  snprintf(zSeg, sizeof(zSeg), "%s/%08llu.wal",
           capdb_volume_wal_dir(pVol), wh.lsn);
  fd = open(zSeg, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if( fd<0 ){
    capdb_volume_close(pVol);
    return CAPDB_IOERR;
  }
  w = write(fd, hdr, (size_t)nHdr);
  if( w==(ssize_t)nHdr ) w = write(fd, payload, (size_t)nPayload);
  if( w==nPayload ) w = fsync(fd)==0 ? nPayload : -1;
  close(fd);
  if( w!=nPayload ){
    capdb_volume_close(pVol);
    return CAPDB_IOERR;
  }
  rc = capdb_rep_apply_wal_payload(pVol, &wh, payload, nPayload);
  capdb_volume_close(pVol);
  return rc;
}

static void *repReplicaLoopFixed(void *pArg){
  capdb_rep_receiver *p = (capdb_rep_receiver*)pArg;
  capdb_tls_config tls;
  capdb_stream *s = 0;

  while( !p->stop ){
    capdb_buf pl, rx;
    unsigned char type;
    if( s==0 ){
      memset(&tls, 0, sizeof(tls));
      tls.zHostname = p->cfg.zPrimaryHost;
      tls.bInsecure = p->cfg.bTls ? 0 : 1;
      if( capdb_stream_connect(p->cfg.zPrimaryHost, p->cfg.iPrimaryPort,
                               &tls, &s) ){
        usleep(500000);
        continue;
      }
      if( capdb_rep_handshake_client(s, CAPDB_REP_PROTO_VERSION)
       || capdb_rep_auth(s, p->cfg.zAuthToken) ){
        capdb_stream_close(s); s = 0;
        usleep(500000);
        continue;
      }
      capdb_buf_init(&pl);
      capdb_buf_append_i64(&pl, 0);
      if( capdb_rep_send(s, CAPDB_REP_STREAM_START, &pl) ){
        capdb_buf_clear(&pl);
        capdb_stream_close(s); s = 0;
        usleep(500000);
        continue;
      }
      capdb_buf_clear(&pl);
    }
    if( capdb_rep_recv(s, &type, &rx) ){
      capdb_stream_close(s); s = 0;
      continue;
    }
    if( type==CAPDB_REP_WAL_CHUNK ){
      CapdbStoreWalHdr wh;
      const unsigned char *body;
      int nBody;
      if( rx.n < (int)sizeof(wh) ){
        capdb_buf_clear(&rx);
        capdb_stream_close(s); s = 0;
        continue;
      }
      memcpy(&wh, rx.a, sizeof(wh));
      body = rx.a + sizeof(wh);
      nBody = rx.n - (int)sizeof(wh);
      if( capdb_rep_replica_apply_chunk(p->cfg.zVolumePath, &wh, (int)sizeof(wh),
                                        body, nBody)==CAPDB_OK ){
        capdb_rep_send_ack(s, wh.lsn);
        pthread_mutex_lock(&p->mutex);
        p->lagBytes = 0;
        pthread_mutex_unlock(&p->mutex);
      }
      capdb_buf_clear(&rx);
    }else if( type==CAPDB_REP_PING ){
      capdb_buf_clear(&rx);
      capdb_buf_init(&pl);
      capdb_rep_send(s, CAPDB_REP_PONG, &pl);
      capdb_buf_clear(&pl);
    }else{
      capdb_buf_clear(&rx);
      capdb_stream_close(s); s = 0;
    }
  }
  if( s ) capdb_stream_close(s);
  return 0;
}

int capdb_rep_receiver_start(const capdb_rep_config *pCfg,
                             capdb_rep_receiver **pp){
  capdb_rep_receiver *p;
  if( pCfg==0 || pp==0 || pCfg->zVolumePath==0 ) return CAPDB_MISUSE;
  p = (capdb_rep_receiver*)calloc(1, sizeof(*p));
  if( p==0 ) return CAPDB_NOMEM;
  p->cfg = *pCfg;
  if( pCfg->zPrimaryHost ){
    p->cfg.zPrimaryHost = strdup(pCfg->zPrimaryHost);
    if( p->cfg.zPrimaryHost==0 ){ free(p); return CAPDB_NOMEM; }
  }
  if( pCfg->zVolumePath ){
    p->cfg.zVolumePath = strdup(pCfg->zVolumePath);
    if( p->cfg.zVolumePath==0 ){
      free((void*)p->cfg.zPrimaryHost);
      free(p);
      return CAPDB_NOMEM;
    }
  }
  if( pCfg->zAuthToken ){
    p->cfg.zAuthToken = strdup(pCfg->zAuthToken);
    if( p->cfg.zAuthToken==0 ){
      free((void*)p->cfg.zPrimaryHost);
      free((void*)p->cfg.zVolumePath);
      free(p);
      return CAPDB_NOMEM;
    }
  }
  pthread_mutex_init(&p->mutex, 0);
  *pp = p;
  return CAPDB_OK;
}

int capdb_rep_receiver_run_async(capdb_rep_receiver *p){
  if( p==0 ) return CAPDB_MISUSE;
  p->stop = 0;
  return pthread_create(&p->thread, 0, repReplicaLoopFixed, p) ? CAPDB_ERROR : CAPDB_OK;
}

void capdb_rep_receiver_stop(capdb_rep_receiver *p){
  if( p==0 ) return;
  p->stop = 1;
  if( p->thread ) pthread_join(p->thread, 0);
  pthread_mutex_destroy(&p->mutex);
  free((void*)p->cfg.zPrimaryHost);
  free((void*)p->cfg.zVolumePath);
  free((void*)p->cfg.zAuthToken);
  free(p);
}

unsigned long long capdb_rep_receiver_lag_bytes(capdb_rep_receiver *p){
  unsigned long long v;
  if( p==0 ) return 0;
  pthread_mutex_lock(&p->mutex);
  v = p->lagBytes;
  pthread_mutex_unlock(&p->mutex);
  return v;
}

#endif /* CAPDB_ENABLE_REPLICATION */
