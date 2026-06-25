
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

static int repJoinPath(char *zOut, size_t nOut, const char *zDir,
                       const char *zName){
  size_t nDir, nName;
  if( zOut==0 || nOut==0 || zDir==0 || zName==0 ) return CAPDB_MISUSE;
  nDir = strlen(zDir);
  nName = strlen(zName);
  if( nDir + 1 + nName + 1 > nOut ) return CAPDB_TOOBIG;
  memcpy(zOut, zDir, nDir);
  zOut[nDir] = '/';
  memcpy(zOut+nDir+1, zName, nName+1);
  return CAPDB_OK;
}

static int repConstantTimeEq(const char *a, const char *b){
  size_t i;
  size_t la = a ? strlen(a) : 0;
  size_t lb = b ? strlen(b) : 0;
  unsigned char c = (unsigned char)(la ^ lb);
  for(i=0; i<la && i<lb; i++){
    c |= (unsigned char)(a[i] ^ b[i]);
  }
  return c==0 && la==lb;
}

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

static void repBootstrapVolume(capdb_stream *s, const char *zVolPath){
  capdb_volume *pVol = 0;
  unsigned long long lsn = 0;
  char zWalDir[1024];
  char zWalName[32];
  char zSeg[1024];
  if( s==0 || zVolPath==0 ) return;
  if( capdb_volume_open(zVolPath, 0, &pVol)!=CAPDB_OK ) return;
  if( capdb_volume_replicate_sql_main(pVol, &lsn)!=CAPDB_OK ){
    capdb_volume_close(pVol);
    return;
  }
  if( repJoinPath(zWalDir, sizeof(zWalDir), zVolPath,
                  CAPDB_STORE_DIR_WAL)!=CAPDB_OK ){
    capdb_volume_close(pVol);
    return;
  }
  snprintf(zWalName, sizeof(zWalName), "%08llu.wal", lsn);
  if( repJoinPath(zSeg, sizeof(zSeg), zWalDir, zWalName)!=CAPDB_OK ){
    capdb_volume_close(pVol);
    return;
  }
  repSendWalSegment(s, zSeg);
  capdb_volume_close(pVol);
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

static void repReplayVolumeWal(capdb_rep_sender *p, capdb_stream *s,
                               const char *zVolPath,
                               unsigned long long fromLsn){
  char zWalDir[1024];
  DIR *d;
  struct dirent *de;
  WalSeg *aSeg = 0;
  int nSeg = 0;
  int nAlloc = 0;
  int i;
  (void)p;
  if( repJoinPath(zWalDir, sizeof(zWalDir), zVolPath,
                  CAPDB_STORE_DIR_WAL)!=CAPDB_OK ){
    return;
  }
  d = opendir(zWalDir);
  if( d==0 ) return;
  while( (de = readdir(d))!=0 ){
    char zSeg[1024];
    unsigned long long lsn = 0;
    struct stat st;
    WalSeg *slot;
    if( sscanf(de->d_name, "%08llu.wal", &lsn)!=1 ) continue;
    if( lsn<=fromLsn ) continue;
    if( repJoinPath(zSeg, sizeof(zSeg), zWalDir, de->d_name)!=CAPDB_OK ){
      continue;
    }
    if( stat(zSeg, &st)!=0 || !S_ISREG(st.st_mode) ) continue;
    if( nSeg>=nAlloc ){
      nAlloc = nAlloc ? nAlloc*2 : 32;
      aSeg = (WalSeg*)realloc(aSeg, (size_t)nAlloc*sizeof(WalSeg));
      if( aSeg==0 ){ closedir(d); return; }
    }
    slot = &aSeg[nSeg++];
    slot->lsn = lsn;
    snprintf(slot->zPath, sizeof(slot->zPath), "%s", zSeg);
  }
  closedir(d);
  if( nSeg==0 ){ free(aSeg); return; }
  qsort(aSeg, (size_t)nSeg, sizeof(WalSeg), walSegCmp);
  for(i=0; i<nSeg; i++) repSendWalSegment(s, aSeg[i].zPath);
  free(aSeg);
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
    if( repJoinPath(zVol, sizeof(zVol), zRoot, de->d_name)!=CAPDB_OK ){
      continue;
    }
    if( stat(zVol, &st)!=0 || !S_ISDIR(st.st_mode) ) continue;
    if( fromLsn==0 ) repBootstrapVolume(s, zVol);
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
    if( p->cfg.zAuthToken==0 || p->cfg.zAuthToken[0]==0 ) goto done;
    if( zTok==0 || !repConstantTimeEq(zTok, p->cfg.zAuthToken) ){
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

static unsigned long long repMinReplicaAck(capdb_rep_sender *p){
  unsigned long long min = ~0ULL;
  int i;
  if( p==0 || p->nReplica==0 ) return 0;
  for(i=0; i<p->nReplica; i++){
    if( p->aReplica[i].lastAck < min ) min = p->aReplica[i].lastAck;
  }
  return min;
}

int capdb_rep_sender_start(const capdb_rep_config *pCfg, capdb_volume *pVol,
                           capdb_rep_sender **pp){
  capdb_rep_sender *p;
  if( pCfg==0 || pp==0 ) return CAPDB_MISUSE;
  if( pCfg->zListen && (!pCfg->zAuthToken || pCfg->zAuthToken[0]==0) ){
    return CAPDB_MISUSE;
  }
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
  *pp = p;
  return CAPDB_OK;
}

void capdb_rep_sender_set_active(capdb_rep_sender *p){
  capdb_store_set_rep_sender(p);
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
  if( capdb_store_rep_sender()==p ) capdb_store_set_rep_sender(0);
  free(p);
}

capdb_rep_sender *capdb_rep_sender_global(void){
  return capdb_store_rep_sender();
}

int capdb_rep_sender_wait_ack(capdb_rep_sender *p, unsigned long long lsn,
                              int timeoutMs){
  int i, maxTry;
  if( p==0 ) return CAPDB_MISUSE;
  if( p->cfg.syncMode==CAPDB_REP_SYNC_OFF ) return CAPDB_OK;
  if( p->nReplica==0 ) return CAPDB_BUSY;
  maxTry = timeoutMs>0 ? timeoutMs/10 : 500;
  for(i=0; i<maxTry; i++){
    pthread_mutex_lock(&p->mutex);
    if( repMinReplicaAck(p)>=lsn ){
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
  capdb_rep_sender *p = capdb_store_rep_sender();
  int i;
  if( p==0 || hdr==0 || payload==0 || nPayload<=0 ) return;
  pthread_mutex_lock(&p->mutex);
  p->lastWalLsn = hdr->lsn;
  for(i=0; i<p->nReplica; i++){
    capdb_stream *s = p->aReplica[i].pStream;
    if( s ) capdb_rep_send_wal_chunk(s, hdr, (int)sizeof(*hdr), payload, nPayload);
  }
  pthread_mutex_unlock(&p->mutex);
  if( p->cfg.syncMode==CAPDB_REP_SYNC_ON ){
    (void)capdb_rep_sender_wait_ack(p, hdr->lsn, 5000);
  }
  (void)pVol;
}

#endif /* CAPDB_ENABLE_REPLICATION */
