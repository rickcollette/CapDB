
#if defined(CAPDB_ENABLE_REPLICATION)

#include "capdb_rep.h"
#include "capdb_rep_io.h"
#include "capdb.h"
#include "../store/capdb_store.h"
#include "../store/capdb_store_format.h"
#include "../tls/capdb_tls.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

#define CAPDB_REP_MAX_REPLICAS 8

typedef struct RepReplicaLink {
  capdb_stream *pStream;
  unsigned long long lastAck;
} RepReplicaLink;

typedef struct RepServeArg {
  capdb_rep_sender *pSender;
  capdb_stream *pConn;
} RepServeArg;

struct capdb_rep_sender {
  capdb_rep_config cfg;
  capdb_volume *pVol;
  pthread_mutex_t mutex;
  unsigned long long lastAckLsn;
  unsigned long long lastWalLsn;
  RepReplicaLink aReplica[CAPDB_REP_MAX_REPLICAS];
  int nReplica;
  int repListenFd;
  volatile int stop;
  pthread_t acceptThread;
  void *pSslCtx;
};

static void repRemoveStream(capdb_rep_sender *p, capdb_stream *s){
  int i, j;
  pthread_mutex_lock(&p->mutex);
  for(i=0; i<p->nReplica; i++){
    if( p->aReplica[i].pStream==s ){
      capdb_stream_close(p->aReplica[i].pStream);
      for(j=i; j<p->nReplica-1; j++) p->aReplica[j] = p->aReplica[j+1];
      p->nReplica--;
      break;
    }
  }
  pthread_mutex_unlock(&p->mutex);
}

static int repSendWalSegment(capdb_stream *s, const char *zPath){
  CapdbStoreWalHdr wh;
  unsigned char *payload = 0;
  ssize_t nHdr, nBody;
  int fd, rc = CAPDB_IOERR;
  fd = open(zPath, O_RDONLY);
  if( fd<0 ) return CAPDB_IOERR;
  nHdr = read(fd, &wh, sizeof(wh));
  if( nHdr!=(ssize_t)sizeof(wh) || wh.magic!=CAPDB_STORE_WAL_MAGIC
   || wh.payload_len==0 ){
    close(fd);
    return CAPDB_CORRUPT;
  }
  payload = (unsigned char*)malloc(wh.payload_len);
  if( payload==0 ){ close(fd); return CAPDB_NOMEM; }
  nBody = read(fd, payload, wh.payload_len);
  close(fd);
  if( nBody!=(ssize_t)wh.payload_len ){
    free(payload);
    return CAPDB_IOERR;
  }
  rc = capdb_rep_send_wal_chunk(s, &wh, (int)sizeof(wh), payload, (int)nBody);
  free(payload);
  return rc;
}

static void repReplayVolumeWal(capdb_rep_sender *p, capdb_stream *s,
                               const char *zVolPath,
                               unsigned long long fromLsn){
  char zWalDir[1024];
  DIR *d;
  struct dirent *de;
  (void)p;
  snprintf(zWalDir, sizeof(zWalDir), "%s/" CAPDB_STORE_DIR_WAL, zVolPath);
  d = opendir(zWalDir);
  if( d==0 ) return;
  while( (de = readdir(d))!=0 ){
    char zSeg[1024];
    unsigned long long lsn = 0;
    struct stat st;
    if( sscanf(de->d_name, "%08llu.wal", &lsn)!=1 ) continue;
    if( lsn<=fromLsn ) continue;
    snprintf(zSeg, sizeof(zSeg), "%s/%s", zWalDir, de->d_name);
    if( stat(zSeg, &st)!=0 || !S_ISREG(st.st_mode) ) continue;
    repSendWalSegment(s, zSeg);
  }
  closedir(d);
}

static void repReplayToStream(capdb_rep_sender *p, capdb_stream *s,
                              unsigned long long fromLsn){
  DIR *d;
  struct dirent *de;
  const char *zRoot = p->cfg.zVolumePath;
  if( zRoot==0 || s==0 ) return;
  d = opendir(zRoot);
  if( d==0 ) return;
  while( (de = readdir(d))!=0 ){
    char zVol[1024];
    struct stat st;
    if( de->d_name[0]=='.' ) continue;
    snprintf(zVol, sizeof(zVol), "%s/%s", zRoot, de->d_name);
    if( stat(zVol, &st)!=0 || !S_ISDIR(st.st_mode) ) continue;
    repReplayVolumeWal(p, s, zVol, fromLsn);
  }
  closedir(d);
}

static void repServeReplica(capdb_rep_sender *p, capdb_stream *pConn){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  unsigned long long fromLsn = 0;
  int i, registered = 0;

  if( capdb_rep_handshake_server(pConn, CAPDB_REP_PROTO_VERSION) ) goto done;
  if( capdb_rep_recv(pConn, &type, &rx) ) goto done;
  if( type!=CAPDB_REP_AUTH ){
    capdb_buf_clear(&rx);
    goto done;
  }
  {
    char *zTok = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_str(&r, &zTok);
    capdb_buf_clear(&rx);
    if( p->cfg.zAuthToken && zTok==0 ) goto done;
    if( p->cfg.zAuthToken && zTok && strcmp(zTok, p->cfg.zAuthToken)!=0 ){
      free(zTok);
      goto done;
    }
    if( p->cfg.zAuthToken==0 && zTok && zTok[0]!=0 ){
      free(zTok);
      goto done;
    }
    free(zTok);
  }
  if( capdb_rep_auth_ok(pConn) ) goto done;
  if( capdb_rep_recv(pConn, &type, &rx) ) goto done;
  if( type!=CAPDB_REP_STREAM_START ){
    capdb_buf_clear(&rx);
    goto done;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  capdb_reader_i64(&r, (long long*)&fromLsn);
  capdb_buf_clear(&rx);

  pthread_mutex_lock(&p->mutex);
  if( p->nReplica < CAPDB_REP_MAX_REPLICAS ){
    p->aReplica[p->nReplica].pStream = pConn;
    p->aReplica[p->nReplica].lastAck = fromLsn;
    p->nReplica++;
    registered = 1;
  }
  pthread_mutex_unlock(&p->mutex);
  if( !registered ) goto done;

  repReplayToStream(p, pConn, fromLsn);

  while( !p->stop ){
    struct pollfd pf;
    int fd = capdb_stream_fd(pConn);
    if( fd<0 ) break;
    pf.fd = fd;
    pf.events = POLLIN;
    if( poll(&pf, 1, 1000)<=0 ) continue;
    if( capdb_rep_recv(pConn, &type, &rx) ) break;
    if( type==CAPDB_REP_ACK ){
      unsigned long long ackLsn = 0;
      capdb_reader_init(&r, rx.a, rx.n);
      capdb_reader_i64(&r, (long long*)&ackLsn);
      capdb_buf_clear(&rx);
      capdb_rep_sender_note_ack(p, ackLsn);
      pthread_mutex_lock(&p->mutex);
      for(i=0; i<p->nReplica; i++){
        if( p->aReplica[i].pStream==pConn ){
          p->aReplica[i].lastAck = ackLsn;
          break;
        }
      }
      pthread_mutex_unlock(&p->mutex);
    }else if( type==CAPDB_REP_PING ){
      capdb_buf_clear(&rx);
      capdb_buf_init(&pl);
      capdb_rep_send(pConn, CAPDB_REP_PONG, &pl);
      capdb_buf_clear(&pl);
    }else{
      capdb_buf_clear(&rx);
      break;
    }
  }

done:
  if( registered ) repRemoveStream(p, pConn);
  else capdb_stream_close(pConn);
}

static void *repServeThread(void *pArg){
  RepServeArg *a = (RepServeArg*)pArg;
  if( a ){
    repServeReplica(a->pSender, a->pConn);
    free(a);
  }
  return 0;
}

static void *repAcceptLoop(void *pArg){
  capdb_rep_sender *p = (capdb_rep_sender*)pArg;
  while( !p->stop ){
    int cfd = -1;
    capdb_stream *s = 0;
    capdb_tls_config tlsCfg;
    RepServeArg *a;
    pthread_t tid;
    if( capdb_tcp_accept(p->repListenFd, &cfd) ) continue;
    memset(&tlsCfg, 0, sizeof(tlsCfg));
    tlsCfg.bServer = 1;
    tlsCfg.bInsecure = p->cfg.bTls ? 0 : 1;
    tlsCfg.pSharedCtx = p->pSslCtx;
    if( capdb_stream_accept(cfd, &tlsCfg, &s) ){
      close(cfd);
      continue;
    }
    a = (RepServeArg*)malloc(sizeof(*a));
    if( a==0 ){
      capdb_stream_close(s);
      continue;
    }
    a->pSender = p;
    a->pConn = s;
    if( pthread_create(&tid, 0, repServeThread, a)!=0 ){
      free(a);
      capdb_stream_close(s);
      continue;
    }
    pthread_detach(tid);
  }
  return 0;
}

static capdb_rep_sender *gRepSender = 0;

int capdb_rep_sender_start(const capdb_rep_config *pCfg, capdb_volume *pVol,
                           capdb_rep_sender **pp){
  capdb_rep_sender *p;
  if( pCfg==0 || pp==0 ) return CAPDB_MISUSE;
  if( gRepSender ) return CAPDB_BUSY;
  p = (capdb_rep_sender*)calloc(1, sizeof(*p));
  if( p==0 ) return CAPDB_NOMEM;
  p->cfg = *pCfg;
  p->pVol = pVol;
  p->pSslCtx = pCfg->pSslCtx;
  p->repListenFd = -1;
  pthread_mutex_init(&p->mutex, 0);
  if( pCfg->zListen && capdb_tcp_listen(pCfg->zListen, &p->repListenFd) ){
    free(p);
    return CAPDB_ERROR;
  }
  if( p->repListenFd>=0 ){
    pthread_create(&p->acceptThread, 0, repAcceptLoop, p);
  }
  gRepSender = p;
  *pp = p;
  return CAPDB_OK;
}

void capdb_rep_sender_stop(capdb_rep_sender *p){
  int i;
  if( p==0 ) return;
  p->stop = 1;
  if( p->repListenFd>=0 ){
    close(p->repListenFd);
    p->repListenFd = -1;
  }
  if( p->acceptThread ) pthread_join(p->acceptThread, 0);
  pthread_mutex_lock(&p->mutex);
  for(i=0; i<p->nReplica; i++){
    capdb_stream_close(p->aReplica[i].pStream);
  }
  p->nReplica = 0;
  pthread_mutex_unlock(&p->mutex);
  pthread_mutex_destroy(&p->mutex);
  if( gRepSender==p ) gRepSender = 0;
  free(p);
}

capdb_rep_sender *capdb_rep_sender_global(void){
  return gRepSender;
}

int capdb_rep_sender_wait_ack(capdb_rep_sender *p, unsigned long long lsn,
                              int timeoutMs){
  int i, maxTry;
  if( p==0 ) return CAPDB_MISUSE;
  if( p->cfg.syncMode==CAPDB_REP_SYNC_OFF ) return CAPDB_OK;
  maxTry = timeoutMs>0 ? timeoutMs/10 : 500;
  for(i=0; i<maxTry; i++){
    pthread_mutex_lock(&p->mutex);
    if( p->lastAckLsn>=lsn ){
      pthread_mutex_unlock(&p->mutex);
      return CAPDB_OK;
    }
    pthread_mutex_unlock(&p->mutex);
    usleep(10000);
  }
  return CAPDB_BUSY;
}

void capdb_rep_sender_note_ack(capdb_rep_sender *p, unsigned long long lsn){
  if( p==0 ) return;
  pthread_mutex_lock(&p->mutex);
  if( lsn>p->lastAckLsn ) p->lastAckLsn = lsn;
  pthread_mutex_unlock(&p->mutex);
}

unsigned long long capdb_rep_sender_last_lsn(capdb_rep_sender *p){
  unsigned long long v;
  if( p==0 ) return 0;
  pthread_mutex_lock(&p->mutex);
  v = p->lastWalLsn;
  pthread_mutex_unlock(&p->mutex);
  return v;
}

void capdb_rep_primary_on_wal(capdb_volume *pVol, const CapdbStoreWalHdr *hdr,
                              const void *payload, int nPayload){
  capdb_rep_sender *p = gRepSender;
  int i;
  if( p==0 || hdr==0 || payload==0 || nPayload<=0 ) return;
  pthread_mutex_lock(&p->mutex);
  p->lastWalLsn = hdr->lsn;
  for(i=0; i<p->nReplica; i++){
    capdb_stream *s = p->aReplica[i].pStream;
    if( s ) capdb_rep_send_wal_chunk(s, hdr, (int)sizeof(*hdr), payload, nPayload);
  }
  pthread_mutex_unlock(&p->mutex);
  (void)pVol;
}

#endif /* CAPDB_ENABLE_REPLICATION */
