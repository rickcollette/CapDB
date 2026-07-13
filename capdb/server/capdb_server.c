
#if defined(CAPDB_ENABLE_NETWORK) && defined(CAPDB_ENABLE_POOL)

#include "capdb_server.h"
#include "../capdb_io.h"
#include "../capdb_network.h"
#include "../proto/capdb_proto.h"
#include "../tls/capdb_tls.h"
#include "../pool/capdb_pool.h"
#if defined(CAPDB_ENABLE_STORE)
#include "../store/capdb_store.h"
#include "../cluster/capdb_cluster.h"
#endif
#if defined(CAPDB_ENABLE_REPLICATION)
#include "../replication/capdb_rep.h"
#include "../replication/capdb_rep_recovery.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>  /* amalgamator: keep */
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <poll.h>
#include <sys/file.h>  /* amalgamator: keep */

extern int capdb_auth_check(const char *zAuthFile, int method,
                              const char *zUser, const char *zSecret);
extern int capdb_auth_check_peer(const char *zAuthFile, int method,
                                   const char *zUser, const char *zSecret,
                                   const char *zPeer);

typedef struct PoolEntry PoolEntry;
struct PoolEntry {
  char *zPath;
  capdb_pool *pPool;
  PoolEntry *pNext;
};

typedef struct ServerStmt ServerStmt;
struct ServerStmt {
  int id;
  int bWrite;
  capdb_stmt *pStmt;
  ServerStmt *pNext;
};

typedef struct VfsSlot VfsSlot;
struct VfsSlot {
  int id;
  int fd;
  char *zPath;
  int eLock;
  VfsSlot *pNext;
};

typedef struct PathEntry PathEntry;
struct PathEntry {
  char *zPath;
  dev_t dev;
  ino_t ino;
  int nPoolPin;
  int nPoolRefs;
  int nVfsOpen;
  int vfsLock;
  int lockFd;
  PathEntry *pNext;
};

struct capdb_server {
  capdb_server_config cfg;
  int listenFd;
  volatile int stop;
  int acceptStarted;
  pthread_t acceptThread;
  PoolEntry *pPools;
  pthread_mutex_t poolMutex;
  int nSessions;            /* live session threads, guarded by poolMutex */
  pthread_t *aSessionTids;
  capdb_stream **aSessionStreams;
  int nSessionActive;
  int nSessionTidsAlloc;
  void *pSslCtx;            /* shared SSL_CTX* for accept handshakes */
  pthread_mutex_t pathMutex;
  PathEntry *pPaths;
#if defined(CAPDB_ENABLE_REPLICATION)
  capdb_rep_sender *pRepSender;
  capdb_rep_receiver *pRepReceiver;
#endif
};

#define CAPDB_DEFAULT_MAX_CLIENTS 256
#define CAPDB_MAX_STMTS 256
#define CAPDB_MAX_VFS 64
#define CAPDB_BATCH_BYTE_BUDGET (1*1024*1024)   /* cap a STEP batch's payload */
#define CAPDB_EXEC_MAX_ROWS     4096

typedef struct Session Session;
struct Session {
  capdb_server *pSrv;
  capdb_stream *pStream;
  int bAuthed;
  char *zDbPath;
  capdb *db;
  capdb_pool *pPool;
  ServerStmt *pStmts;
  VfsSlot *pVfs;
  int nStmtId;
  int nVfsId;
  int inTxn;                 /* explicit client transaction open (db is pinned) */
  int iTrackSlot;            /* index in server session track arrays, or -1 */
  char zPeer[64];            /* "ip:port" of the client, for audit logging */
};

#if defined(CAPDB_ENABLE_STORE)
static int serverUsesVolume(capdb_server *pSrv);
#endif

/* ---- audit logging --------------------------------------------------------
** Structured stderr audit lines for connection/auth events so failed AUTH and
** unauthorized access are observable (review item S3). One line per event;
** volume is bounded by the max-clients cap. Audit logging can be suppressed
** with the --quiet flag via pSrv->cfg.bQuiet. */
static void capdbAudit(capdb_server *pSrv, const char *zEvent, const char *zPeer, const char *zDetail){
  const char *p = (zPeer && zPeer[0]) ? zPeer : "?";
  if( pSrv && pSrv->cfg.bQuiet ) return;
  if( zDetail && zDetail[0] ){
    fprintf(stderr, "capdb audit event=%s peer=%s detail=%s\n", zEvent, p, zDetail);
  }else{
    fprintf(stderr, "capdb audit event=%s peer=%s\n", zEvent, p);
  }
}

/* capdbPeer fills buf with the socket peer's "ip:port", or "?" on failure. */
static void capdbPeer(int fd, char *buf, size_t n){
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  char host[64];
  unsigned port = 0;
  if( n==0 ) return;
  buf[0] = 0;
  if( getpeername(fd, (struct sockaddr*)&ss, &sl)!=0 ){
    snprintf(buf, n, "?");
    return;
  }
  if( ss.ss_family==AF_INET ){
    struct sockaddr_in *s4 = (struct sockaddr_in*)&ss;
    inet_ntop(AF_INET, &s4->sin_addr, host, sizeof(host));
    port = ntohs(s4->sin_port);
  }else if( ss.ss_family==AF_INET6 ){
    struct sockaddr_in6 *s6 = (struct sockaddr_in6*)&ss;
    inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof(host));
    port = ntohs(s6->sin6_port);
  }else{
    snprintf(buf, n, "?");
    return;
  }
  snprintf(buf, n, "%s:%u", host, port);
}

static void sendError(Session *p, int rc, const char *zMsg){
  capdb_buf pl;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, rc);
  capdb_buf_append_str(&pl, zMsg ? zMsg : "error");
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_ERROR, &pl);
  capdb_buf_clear(&pl);
}

/* Bound a phase of the session with a socket recv timeout. A short timeout is
** applied to the unauthenticated handshake so a client that connects but never
** completes HELLO/AUTH (slow-loris) cannot pin a session thread forever; it is
** cleared (ms==0 => block) once authenticated so legitimate idle keep-alive
** connections from the client-side pool are not disconnected. */
static void sessionSetRecvTimeout(Session *p, int ms){
  int fd = capdb_stream_fd(p->pStream);
  struct timeval tv;
  if( fd<0 ) return;
  tv.tv_sec = ms/1000;
  tv.tv_usec = (ms%1000)*1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int sessionRequireAuthed(Session *p){
  if( !p->bAuthed ){
    sendError(p, CAPDB_PERM, "not authenticated");
    return 0;
  }
  return 1;
}

static int sessionRequireOpen(Session *p){
  if( !sessionRequireAuthed(p) ) return 0;
#if defined(CAPDB_ENABLE_STORE)
  if( serverUsesVolume(p->pSrv) ){
    if( p->zDbPath==0 ){
      sendError(p, CAPDB_MISUSE, "not open");
      return 0;
    }
    return 1;
  }
#endif
  if( p->pPool==0 ){
    sendError(p, CAPDB_MISUSE, "not open");
    return 0;
  }
  return 1;
}

static int sessionRequireDb(Session *p){
  if( !sessionRequireOpen(p) ) return 0;
  if( p->db==0 ){
    sendError(p, CAPDB_MISUSE, "no active database connection");
    return 0;
  }
  return 1;
}

static int sessionHasStmts(Session *p){
  return p!=0 && p->pStmts!=0;
}

static int sessionParseFail(Session *p){
  sendError(p, CAPDB_MISUSE, "malformed frame");
  return -1;
}

static int sqlKeywordBoundary(const unsigned char *z, size_t nKw){
  unsigned char c = z[nKw];
  return c==0 || isspace(c) || c==';';
}

static int serverSessionFindSlot(capdb_server *pSrv){
  int i;
  for(i=0; i<pSrv->nSessionTidsAlloc; i++){
    if( pSrv->aSessionStreams[i]==0 ) return i;
  }
  return -1;
}

static void serverSessionAssign(capdb_server *pSrv, int slot,
                                pthread_t tid, capdb_stream *pStream){
  pSrv->aSessionTids[slot] = tid;
  pSrv->aSessionStreams[slot] = pStream;
  pSrv->nSessionActive++;
}

static void serverSessionReleaseSlot(capdb_server *pSrv, int slot){
  if( slot<0 || slot>=pSrv->nSessionTidsAlloc ) return;
  if( pSrv->aSessionStreams[slot]==0 ) return;
  pSrv->aSessionStreams[slot] = 0;
  pSrv->nSessionActive--;
}

/* Case-insensitive search for a DML write verb anywhere in the statement.
** Used to classify CTEs (WITH ...) that wrap INSERT/UPDATE/DELETE/REPLACE so
** they route to a write connection and get auto-committed instead of being run
** on a read connection and silently rolled back on release. A false positive
** (a read CTE that mentions a write verb in an identifier/string) only costs a
** write checkout; a false negative would lose data, so we err toward write. */
static int sqlHasWriteVerb(const unsigned char *z){
  for(; *z; z++){
    if( capdb_strnicmp((const char*)z, "INSERT", 6)==0 ) return 1;
    if( capdb_strnicmp((const char*)z, "UPDATE", 6)==0 ) return 1;
    if( capdb_strnicmp((const char*)z, "DELETE", 6)==0 ) return 1;
    if( capdb_strnicmp((const char*)z, "REPLACE", 7)==0 ) return 1;
  }
  return 0;
}

static int sqlIsWrite(const char *zSql){
  const unsigned char *z = (const unsigned char*)zSql;
  while( *z && isspace(*z) ) z++;
  if( capdb_strnicmp((const char*)z, "SELECT", 6)==0 ) return 0;
  if( capdb_strnicmp((const char*)z, "WITH", 4)==0 ) return sqlHasWriteVerb(z);
  if( capdb_strnicmp((const char*)z, "EXPLAIN", 7)==0 ) return 0;
  if( capdb_strnicmp((const char*)z, "PRAGMA", 6)==0 ){
    /* read-only pragmas - treat as read for safety use write if contains = */
    if( strchr((const char*)z, '=')==0 ) return 0;
  }
  return 1;
}

/* True if zReal is the real root itself or sits beneath it, with a '/' boundary
** so "/root" does not match a sibling like "/rootEVIL". */
static int pathWithinRoot(const char *zReal, const char *zRoot, size_t nRoot){
  return strncmp(zReal, zRoot, nRoot)==0
      && (zReal[nRoot]=='/' || zReal[nRoot]==0);
}

static int pathHasDotDot(const char *zRel){
  const char *z = zRel;
  while( z && z[0] ){
    const char *slash = strchr(z, '/');
    size_t n = slash ? (size_t)(slash-z) : strlen(z);
    if( n==2 && z[0]=='.' && z[1]=='.' ) return 1;
    if( slash==0 ) break;
    z = slash+1;
  }
  return 0;
}

#if defined(CAPDB_ENABLE_STORE)
static int serverUsesVolume(capdb_server *pSrv){
  return pSrv && pSrv->cfg.zStorage && strcmp(pSrv->cfg.zStorage,"volume")==0;
}

static int serverIsReplica(capdb_server *pSrv){
  return pSrv && pSrv->cfg.clusterRole==CAPDB_CLUSTER_ROLE_REPLICA;
}

static int serverIsDemoted(capdb_server *pSrv){
  return pSrv && pSrv->cfg.clusterRole==CAPDB_CLUSTER_ROLE_DEMOTED;
}

#if defined(CAPDB_ENABLE_REPLICATION)
static int serverRepStart(capdb_server *p){
  capdb_rep_config rcfg;
  char zHost[256];
  memset(&rcfg, 0, sizeof(rcfg));
  rcfg.bTls = p->cfg.bInsecure ? 0 : 1;
  rcfg.pSslCtx = p->pSslCtx;
  rcfg.zAuthToken = p->cfg.zRepToken;
  rcfg.syncMode = p->cfg.repSyncMode ? CAPDB_REP_SYNC_ON : CAPDB_REP_SYNC_OFF;
  if( !serverIsReplica(p) && p->cfg.zRepListen ){
    rcfg.zListen = p->cfg.zRepListen;
    rcfg.zVolumePath = p->cfg.zVolumeRoot;
    if( capdb_rep_sender_start(&rcfg, 0, &p->pRepSender)!=CAPDB_OK ) return -1;
    capdb_store_set_rep_sender(p->pRepSender);
  }
  if( serverIsReplica(p) && p->cfg.zRepPrimary && p->cfg.zVolumeRoot ){
    const char *colon = strrchr(p->cfg.zRepPrimary, ':');
    int port = 5433;
    if( colon ){
      size_t n = (size_t)(colon - p->cfg.zRepPrimary);
      if( n < sizeof(zHost) ){
        memcpy(zHost, p->cfg.zRepPrimary, n);
        zHost[n] = 0;
        port = atoi(colon+1);
        rcfg.zPrimaryHost = zHost;
        rcfg.iPrimaryPort = port;
        rcfg.zVolumePath = p->cfg.zVolumeRoot;
        if( capdb_rep_receiver_start(&rcfg, &p->pRepReceiver)==CAPDB_OK ){
          capdb_rep_receiver_run_async(p->pRepReceiver);
        }
      }
    }
  }
  return 0;
}

static void serverRepStop(capdb_server *p){
  if( p->pRepSender ){
    capdb_store_set_rep_sender(0);
    capdb_rep_sender_stop(p->pRepSender);
    p->pRepSender = 0;
  }
  if( p->pRepReceiver ){
    capdb_rep_receiver_stop(p->pRepReceiver);
    p->pRepReceiver = 0;
  }
}

#endif

#if defined(CAPDB_ENABLE_STORE) && defined(CAPDB_ENABLE_REPLICATION)
static void serverVolumeReplicateSnapshot(Session *p){
  capdb_volume *pVol = 0;
  char *zErr = 0;
  if( p==0 || p->db==0 || p->zDbPath==0 || p->pSrv==0 ) return;
  if( p->pSrv->pRepSender==0 || serverIsReplica(p->pSrv) ) return;
  capdb_exec(p->db, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, &zErr);
  capdb_free(zErr);
  if( capdb_volume_open(p->zDbPath, 0, &pVol)==CAPDB_OK ){
    capdb_volume_replicate_sql_main(pVol, 0);
    capdb_volume_close(pVol);
  }
}
#endif

#if defined(CAPDB_ENABLE_STORE) && defined(CAPDB_ENABLE_REPLICATION)
static int serverReplicaSyncVolume(const char *zDbPath){
  capdb_volume *pVol = 0;
  int rc = CAPDB_OK;
  if( zDbPath==0 ) return CAPDB_MISUSE;
  if( capdb_volume_open(zDbPath, CAPDB_VOLUME_OPEN_CREATE, &pVol)!=CAPDB_OK ){
    return CAPDB_CANTOPEN;
  }
  rc = capdb_rep_recovery_replay_dir(pVol);
  capdb_volume_close(pVol);
  return rc;
}
#endif

static int pathVolumeIdFromClient(const char *zPath, char *zId, size_t nId){
  const char *z = zPath;
  size_t n;
  if( z[0]=='/' ) z++;
  n = strlen(z);
  if( n==0 || n>=nId ) return -1;
  memcpy(zId, z, n+1);
  if( n>3 && strcmp(zId+n-3, ".db")==0 ) zId[n-3] = 0;
  if( zId[0]==0 || pathHasDotDot(zId) ) return -1;
  return 0;
}

static int pathIsAllowedVolume(capdb_server *pSrv, const char *zPath, char **pzOut){
  char zId[256];
  char *zRealRoot = 0;
  char *zCand = 0;
  char *zResolved = 0;
  size_t nRoot, nOut;
  if( pSrv->cfg.zVolumeRoot==0 ) return -1;
  if( pathVolumeIdFromClient(zPath, zId, sizeof(zId)) ) return -1;
  if( !capdb_store_vol_id_valid(zId) ) return -1;
  zRealRoot = realpath(pSrv->cfg.zVolumeRoot, 0);
  if( zRealRoot==0 ) return -1;
  nRoot = strlen(zRealRoot);
  nOut = nRoot + strlen(zId) + 2;
  zCand = (char*)malloc(nOut);
  if( zCand==0 ){ free(zRealRoot); return -1; }
  snprintf(zCand, nOut, "%s/%s", zRealRoot, zId);
  zResolved = realpath(zCand, 0);
  if( zResolved==0 ){
    if( mkdir(zCand, 0755)!=0 && errno!=EEXIST ){
      free(zCand);
      free(zRealRoot);
      return -1;
    }
    zResolved = realpath(zCand, 0);
  }
  if( zResolved==0 || !pathWithinRoot(zResolved, zRealRoot, nRoot) ){
    free(zResolved);
    free(zCand);
    free(zRealRoot);
    return -1;
  }
  *pzOut = zCand;
  free(zResolved);
  free(zRealRoot);
  return 0;
}
#endif

static int pathIsAllowed(capdb_server *pSrv, const char *zPath, char **pzOut){
#if defined(CAPDB_ENABLE_STORE)
  if( serverUsesVolume(pSrv) ) return pathIsAllowedVolume(pSrv, zPath, pzOut);
#endif
  char *zRealRoot = 0;
  char *zCand = 0;
  char *zResolved = 0;
  size_t nRoot, nOut;
  const char *zRel = zPath;
  int rc = -1;

  if( pSrv->cfg.zDbRoot==0 ) return -1;
  if( zRel[0]=='/' ) zRel++;
  if( zRel[0]==0 || pathHasDotDot(zRel) ) return -1;

  zRealRoot = realpath(pSrv->cfg.zDbRoot, 0);
  if( zRealRoot==0 ) return -1;
  nRoot = strlen(zRealRoot);

  nOut = nRoot + strlen(zRel) + 2;
  zCand = (char*)malloc(nOut);
  if( zCand==0 ){ free(zRealRoot); return -1; }
  snprintf(zCand, nOut, "%s/%s", zRealRoot, zRel);

  /* Resolve symlinks and require the result to stay within the real root. If the
  ** target exists, resolve the whole path; otherwise (a new file) resolve its
  ** parent directory. This defeats symlinks planted inside the root. */
  zResolved = realpath(zCand, 0);
  if( zResolved ){
    if( pathWithinRoot(zResolved, zRealRoot, nRoot) ) rc = 0;
  }else{
    char *zSlash = strrchr(zCand, '/');
    if( zSlash && zSlash!=zCand ){
      char *zParent;
      *zSlash = 0;
      zParent = realpath(zCand, 0);
      *zSlash = '/';
      if( zParent ){
        if( pathWithinRoot(zParent, zRealRoot, nRoot) ) rc = 0;
        free(zParent);
      }
    }
  }
  free(zResolved);

  if( rc==0 ){
    *pzOut = zCand;
  }else{
    free(zCand);
  }
  free(zRealRoot);
  return rc;
}

static capdb_pool *getPool(capdb_server *pSrv, const char *zPath){
  PoolEntry *p;
  capdb_pool_config cfg;
  capdb_pool *pPool = 0;
  int rc;

  pthread_mutex_lock(&pSrv->poolMutex);
  for(p=pSrv->pPools; p; p=p->pNext){
    if( strcmp(p->zPath, zPath)==0 ){
      pPool = p->pPool;
      break;
    }
  }
  if( pPool==0 ){
    p = (PoolEntry*)calloc(1, sizeof(*p));
    if( p ){
      memset(&cfg, 0, sizeof(cfg));
      cfg.minSize = pSrv->cfg.poolMin ? pSrv->cfg.poolMin : 1;
      cfg.maxSize = pSrv->cfg.poolMax ? pSrv->cfg.poolMax : 8;
#if defined(CAPDB_ENABLE_STORE)
      if( serverUsesVolume(pSrv) ){
        struct stat stMain;
        char zMain[1024];
        snprintf(zMain, sizeof(zMain), "%s/" CAPDB_STORE_MAIN_DB, zPath);
        if( stat(zMain, &stMain)==0 && stMain.st_size==0 ) unlink(zMain);
        cfg.minSize = 0;
        cfg.maxSize = pSrv->cfg.poolMax ? pSrv->cfg.poolMax : 8;
        cfg.flags = CAPDB_POOL_SERIALIZE_WRITES | CAPDB_POOL_RESET_ON_RELEASE
                  | CAPDB_POOL_DEFER_WRITE_BEGIN;
        cfg.zVfs = capdb_store_vfs_name();
      }
#endif
      p->zPath = strdup(zPath);
      rc = capdb_pool_open(zPath, &cfg, &pPool);
      if( rc==CAPDB_OK ){
        p->pPool = pPool;
        p->pNext = pSrv->pPools;
        pSrv->pPools = p;
      }else{
        free(p->zPath);
        free(p);
        pPool = 0;
      }
    }
  }
  pthread_mutex_unlock(&pSrv->poolMutex);
  return pPool;
}

static void pathEntryFree(capdb_server *pSrv, PathEntry *e);

static PathEntry *pathEntryFind(capdb_server *pSrv, const char *zPath, int bCreate){
  PathEntry *e;
  for(e=pSrv->pPaths; e; e=e->pNext){
    if( strcmp(e->zPath, zPath)==0 ) return e;
  }
  if( !bCreate ) return 0;
  e = (PathEntry*)calloc(1, sizeof(*e));
  if( e==0 ) return 0;
  e->zPath = strdup(zPath);
  if( e->zPath==0 ){ free(e); return 0; }
  e->lockFd = -1;
  e->pNext = pSrv->pPaths;
  pSrv->pPaths = e;
  return e;
}

static PathEntry *pathEntryFindByInode(capdb_server *pSrv, dev_t dev, ino_t ino){
  PathEntry *e;
  if( ino==0 ) return 0;
  for(e=pSrv->pPaths; e; e=e->pNext){
    if( e->ino==ino && e->dev==dev ) return e;
  }
  return 0;
}

static int pathEntryStat(const char *zPath, int dataFd, struct stat *pSt){
  if( dataFd>=0 ) return fstat(dataFd, pSt);
  return stat(zPath, pSt);
}

static int pathEntryResolve(capdb_server *pSrv, const char *zPath, int dataFd,
                              int bCreate, PathEntry **ppE){
  struct stat st;
  PathEntry *e, *byInode;
  int rc;
  *ppE = 0;
  if( zPath==0 ) return CAPDB_MISUSE;
  e = pathEntryFind(pSrv, zPath, 0);
  if( e ){
    *ppE = e;
    return CAPDB_OK;
  }
  memset(&st, 0, sizeof(st));
  rc = pathEntryStat(zPath, dataFd, &st);
  if( rc==0 && st.st_ino!=0 ){
    byInode = pathEntryFindByInode(pSrv, st.st_dev, st.st_ino);
    if( byInode && strcmp(byInode->zPath, zPath)!=0 ) return CAPDB_BUSY;
  }
  if( !bCreate ) return CAPDB_OK;
  e = pathEntryFind(pSrv, zPath, 1);
  if( e==0 ) return CAPDB_NOMEM;
  if( rc==0 ){
    e->dev = st.st_dev;
    e->ino = st.st_ino;
  }
  *ppE = e;
  return CAPDB_OK;
}

static void pathEntryMaybeFree(capdb_server *pSrv, PathEntry *e){
  if( e==0 ) return;
  if( e->nPoolPin==0 && e->nPoolRefs==0 && e->nVfsOpen==0 && e->lockFd<0 ){
    pathEntryFree(pSrv, e);
  }
}

static void pathEntryFree(capdb_server *pSrv, PathEntry *e){
  PathEntry **pp;
  for(pp=&pSrv->pPaths; *pp; pp=&(*pp)->pNext){
    if( *pp==e ){
      *pp = e->pNext;
      if( e->lockFd>=0 ) close(e->lockFd);
      free(e->zPath);
      free(e);
      return;
    }
  }
}

static int pathRegistryPinPool(capdb_server *pSrv, const char *zPath){
  PathEntry *e = 0;
  int rc;
  if( zPath==0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&pSrv->pathMutex);
  rc = pathEntryResolve(pSrv, zPath, -1, 1, &e);
  if( rc!=CAPDB_OK ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return rc;
  }
  if( e->nVfsOpen>0 ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return CAPDB_BUSY;
  }
  e->nPoolPin++;
  pthread_mutex_unlock(&pSrv->pathMutex);
  return CAPDB_OK;
}

static void pathRegistryUnpinPool(capdb_server *pSrv, const char *zPath){
  PathEntry *e;
  if( zPath==0 ) return;
  pthread_mutex_lock(&pSrv->pathMutex);
  e = pathEntryFind(pSrv, zPath, 0);
  if( e ){
    if( e->nPoolPin>0 ) e->nPoolPin--;
    pathEntryMaybeFree(pSrv, e);
  }
  pthread_mutex_unlock(&pSrv->pathMutex);
}

static int pathRegistryAddPoolRef(capdb_server *pSrv, const char *zPath){
  PathEntry *e = 0;
  int rc;
  if( zPath==0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&pSrv->pathMutex);
  rc = pathEntryResolve(pSrv, zPath, -1, 1, &e);
  if( rc!=CAPDB_OK ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return rc;
  }
  if( e->nVfsOpen>0 ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return CAPDB_BUSY;
  }
  e->nPoolRefs++;
  pthread_mutex_unlock(&pSrv->pathMutex);
  return CAPDB_OK;
}

static void pathRegistryDropPoolRef(capdb_server *pSrv, const char *zPath){
  PathEntry *e;
  if( zPath==0 ) return;
  pthread_mutex_lock(&pSrv->pathMutex);
  e = pathEntryFind(pSrv, zPath, 0);
  if( e ){
    if( e->nPoolRefs>0 ) e->nPoolRefs--;
    pathEntryMaybeFree(pSrv, e);
  }
  pthread_mutex_unlock(&pSrv->pathMutex);
}

static int pathRegistryVfsOpen(capdb_server *pSrv, const char *zPath, int dataFd){
  PathEntry *e = 0;
  struct stat st;
  int rc;
  if( zPath==0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&pSrv->pathMutex);
  rc = pathEntryResolve(pSrv, zPath, dataFd, 1, &e);
  if( rc!=CAPDB_OK ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return rc;
  }
  if( e->nPoolPin>0 || e->nPoolRefs>0 ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return CAPDB_BUSY;
  }
  if( e->lockFd<0 ){
    e->lockFd = dup(dataFd);
    if( e->lockFd<0 ){
      pthread_mutex_unlock(&pSrv->pathMutex);
      return CAPDB_IOERR;
    }
    if( fstat(e->lockFd, &st)==0 ){
      e->dev = st.st_dev;
      e->ino = st.st_ino;
    }
  }
  e->nVfsOpen++;
  pthread_mutex_unlock(&pSrv->pathMutex);
  return CAPDB_OK;
}

static void pathRegistryVfsClose(capdb_server *pSrv, const char *zPath){
  PathEntry *e;
  if( zPath==0 ) return;
  pthread_mutex_lock(&pSrv->pathMutex);
  e = pathEntryFind(pSrv, zPath, 0);
  if( e ){
    if( e->nVfsOpen>0 ) e->nVfsOpen--;
    if( e->nVfsOpen==0 ){
      if( e->lockFd>=0 ){
        flock(e->lockFd, LOCK_UN);
        close(e->lockFd);
        e->lockFd = -1;
      }
      e->vfsLock = 0;
      pathEntryMaybeFree(pSrv, e);
    }
  }
  pthread_mutex_unlock(&pSrv->pathMutex);
}

static int pathRegistryCanDelete(capdb_server *pSrv, const char *zPath){
  PathEntry *e;
  int busy = 0;
  if( zPath==0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&pSrv->pathMutex);
  e = pathEntryFind(pSrv, zPath, 0);
  if( e && (e->nPoolPin>0 || e->nPoolRefs>0 || e->nVfsOpen>0) ){
    busy = 1;
  }
  pthread_mutex_unlock(&pSrv->pathMutex);
  return busy ? CAPDB_BUSY : CAPDB_OK;
}

static int pathRegistrySetLock(capdb_server *pSrv, const char *zPath, int eNew){
  PathEntry *e;
  int eOld;
  int rc = 0;
  if( zPath==0 ) return 0;
  pthread_mutex_lock(&pSrv->pathMutex);
  e = pathEntryFind(pSrv, zPath, 0);
  if( e==0 || e->lockFd<0 ){
    pthread_mutex_unlock(&pSrv->pathMutex);
    return 0;
  }
  eOld = e->vfsLock;
  if( eNew==eOld ){
    rc = 1;
  }else if( eNew==4 ){
    if( flock(e->lockFd, LOCK_EX | LOCK_NB)==0 ){
      e->vfsLock = 4;
      rc = 1;
    }
  }else if( eNew==0 ){
    if( eOld==4 || eOld==1 ){
      flock(e->lockFd, LOCK_UN | LOCK_NB);
    }
    e->vfsLock = 0;
    rc = 1;
  }else if( eNew==1 ){
    if( eOld==4 ) flock(e->lockFd, LOCK_UN | LOCK_NB);
    if( eOld==0 ){
      if( flock(e->lockFd, LOCK_SH | LOCK_NB)!=0 ){
        pthread_mutex_unlock(&pSrv->pathMutex);
        return 0;
      }
    }
    e->vfsLock = 1;
    rc = 1;
  }else if( eNew==2 ){
    if( eOld==4 ){
      pthread_mutex_unlock(&pSrv->pathMutex);
      return 0;
    }
    if( eOld==0 ){
      if( flock(e->lockFd, LOCK_SH | LOCK_NB)!=0 ){
        pthread_mutex_unlock(&pSrv->pathMutex);
        return 0;
      }
    }
    e->vfsLock = 2;
    rc = 1;
  }else if( eNew==3 ){
    if( eOld==4 ){
      pthread_mutex_unlock(&pSrv->pathMutex);
      return 0;
    }
    if( eOld==0 ){
      if( flock(e->lockFd, LOCK_SH | LOCK_NB)!=0 ){
        pthread_mutex_unlock(&pSrv->pathMutex);
        return 0;
      }
    }
    e->vfsLock = 3;
    rc = 1;
  }
  pthread_mutex_unlock(&pSrv->pathMutex);
  return rc;
}

static int pathRegistryCheckReserved(capdb_server *pSrv, const char *zPath){
  PathEntry *e;
  int reserved = 0;
  if( zPath==0 ) return 0;
  pthread_mutex_lock(&pSrv->pathMutex);
  e = pathEntryFind(pSrv, zPath, 0);
  if( e && e->vfsLock>=2 ) reserved = 1;
  pthread_mutex_unlock(&pSrv->pathMutex);
  return reserved;
}

static void sessionFreeStmts(Session *p){
  ServerStmt *st, *stNext;
  for(st=p->pStmts; st; st=stNext){
    stNext = st->pNext;
    capdb_finalize(st->pStmt);
    free(st);
  }
  p->pStmts = 0;
}

static int sessionReleaseDb(Session *p){
#if defined(CAPDB_ENABLE_STORE)
  if( serverUsesVolume(p->pSrv) && p->db ){
    capdb_close_v2(p->db);
    p->db = 0;
    if( p->zDbPath ) pathRegistryUnpinPool(p->pSrv, p->zDbPath);
    return 0;
  }
#endif
  if( p->db && p->pPool ){
    capdb_pool_release(p->pPool, p->db);
    p->db = 0;
    if( p->zDbPath ) pathRegistryUnpinPool(p->pSrv, p->zDbPath);
  }
  return 0;
}

static int sessionMaybeReleaseDb(Session *p){
#if defined(CAPDB_ENABLE_STORE)
  if( serverUsesVolume(p->pSrv) ) return 0;
#endif
  return sessionReleaseDb(p);
}

#if defined(CAPDB_ENABLE_STORE)
static int volumeAuthorizer(void *pUnused, int action, const char *a,
                            const char *b, const char *c, const char *d){
  (void)pUnused; (void)a; (void)b; (void)c; (void)d;
  if( action==CAPDB_ATTACH || action==CAPDB_DETACH ) return CAPDB_DENY;
  return CAPDB_OK;
}
#endif

static int sessionAcquireDb(Session *p, int bWrite){
  int rc;
  (void)bWrite;
  if( p->db ) return CAPDB_OK;
  if( p->zDbPath==0 ) return CAPDB_MISUSE;
#if defined(CAPDB_ENABLE_STORE)
  if( serverUsesVolume(p->pSrv) ){
    rc = pathRegistryPinPool(p->pSrv, p->zDbPath);
    if( rc!=CAPDB_OK ) return rc;
    if( capdb_volume_prepare(p->zDbPath, CAPDB_VOLUME_OPEN_CREATE)!=CAPDB_OK ){
      pathRegistryUnpinPool(p->pSrv, p->zDbPath);
      return CAPDB_CANTOPEN;
    }
#if defined(CAPDB_ENABLE_REPLICATION)
    if( serverIsReplica(p->pSrv) ){
      rc = serverReplicaSyncVolume(p->zDbPath);
      if( rc!=CAPDB_OK ){
        pathRegistryUnpinPool(p->pSrv, p->zDbPath);
        return rc;
      }
    }
#endif
    rc = capdb_open_v2(p->zDbPath, &p->db,
        CAPDB_OPEN_READWRITE|CAPDB_OPEN_CREATE, capdb_store_vfs_name());
    if( rc!=CAPDB_OK ){
      pathRegistryUnpinPool(p->pSrv, p->zDbPath);
      return rc;
    }
    capdb_set_authorizer(p->db, volumeAuthorizer, 0);
    {
      char *zErr = 0;
      rc = capdb_exec(p->db, "PRAGMA journal_mode=WAL", 0, 0, &zErr);
      if( rc!=CAPDB_OK ){
        capdb_free(zErr);
        capdb_close_v2(p->db);
        p->db = 0;
        pathRegistryUnpinPool(p->pSrv, p->zDbPath);
        return rc;
      }
      capdb_free(zErr);
    }
    capdb_busy_timeout(p->db, 30000);
    return CAPDB_OK;
  }
#endif
  if( p->pPool==0 ) return CAPDB_MISUSE;
  rc = pathRegistryPinPool(p->pSrv, p->zDbPath);
  if( rc!=CAPDB_OK ) return rc;
  rc = capdb_pool_acquire(p->pPool,
      bWrite ? CAPDB_POOL_WRITE : CAPDB_POOL_READ, 30000, &p->db);
  if( rc!=CAPDB_OK ){
    pathRegistryUnpinPool(p->pSrv, p->zDbPath);
    return rc;
  }
  return CAPDB_OK;
}

static int sessionReacquireDb(Session *p, int bWrite){
  int rc;
  if( p->inTxn ) return CAPDB_OK;
  if( sessionHasStmts(p) ){
    sendError(p, CAPDB_MISUSE, "finalize prepared statements first");
    return CAPDB_MISUSE;
  }
#if defined(CAPDB_ENABLE_STORE)
  if( serverUsesVolume(p->pSrv) ){
    if( p->db ) return CAPDB_OK;
    return sessionAcquireDb(p, bWrite);
  }
#endif
  sessionReleaseDb(p);
  rc = sessionAcquireDb(p, bWrite);
  return rc;
}

static int appendColumnValue(capdb_buf *p, capdb_stmt *s, int i){
  int t = capdb_column_type(s, i);
  unsigned char wire;
  switch( t ){
    case CAPDB_INTEGER: wire = CAPDB_VAL_INT; break;
    case CAPDB_FLOAT:   wire = CAPDB_VAL_FLOAT; break;
    case CAPDB_TEXT:    wire = CAPDB_VAL_TEXT; break;
    case CAPDB_BLOB:    wire = CAPDB_VAL_BLOB; break;
    default:             wire = CAPDB_VAL_NULL; break;
  }
  capdb_buf_append_u8(p, wire);
  switch( wire ){
    case CAPDB_VAL_INT:
      capdb_buf_append_i64(p, capdb_column_int64(s, i));
      break;
    case CAPDB_VAL_FLOAT:
      capdb_buf_append_double(p, capdb_column_double(s, i));
      break;
    case CAPDB_VAL_TEXT:
      capdb_buf_append_str(p, (const char*)capdb_column_text(s, i));
      break;
    case CAPDB_VAL_BLOB: {
      int n = capdb_column_bytes(s, i);
      const void *b = capdb_column_blob(s, i);
      capdb_buf_append_u32(p, (unsigned)n);
      if( n>0 ) capdb_buf_append(p, b, n);
      break;
    }
    default:
      break;
  }
  return 0;
}

/* Explicit transaction control. The CapDB server normally runs each statement in
** autocommit mode on a freshly checked-out pooled connection. To support real
** interactive transactions, a BEGIN pins a WRITE connection to the session for
** the life of the transaction and suppresses per-statement auto-commit, and a
** COMMIT/ROLLBACK ends it and releases the connection back to the pool. The
** WRITE checkout's own "BEGIN IMMEDIATE" *is* the transaction, so the client's
** BEGIN is intercepted (not forwarded) to avoid nesting. */
#define TXN_NONE     0
#define TXN_BEGIN    1
#define TXN_COMMIT   2
#define TXN_ROLLBACK 3
static int sqlTxnAction(const char *zSql){
  const unsigned char *z = (const unsigned char*)zSql;
  while( *z && isspace(*z) ) z++;
  if( capdb_strnicmp((const char*)z, "BEGIN", 5)==0 ){
    const unsigned char *t = z+5;
    while( *t && isspace(*t) ) t++;
    if( *t==0 || *t==';' ) return TXN_BEGIN;
    if( capdb_strnicmp((const char*)t, "TRANSACTION", 11)==0 ){
      t += 11;
      while( *t && isspace(*t) ) t++;
      if( *t==0 || *t==';' ) return TXN_BEGIN;
    }
    return TXN_NONE;
  }
  if( capdb_strnicmp((const char*)z, "COMMIT", 6)==0
   && sqlKeywordBoundary(z, 6) ) return TXN_COMMIT;
  if( capdb_strnicmp((const char*)z, "END", 3)==0 ){
    const unsigned char *t = z+3;
    while( *t && isspace(*t) ) t++;
    if( *t==0 || *t==';' ) return TXN_COMMIT;
    return TXN_NONE;
  }
  if( capdb_strnicmp((const char*)z, "ROLLBACK", 8)==0 ){
    const unsigned char *t = z + 8;
    if( !sqlKeywordBoundary(z, 8) ) return TXN_NONE;
    while( *t && isspace(*t) ) t++;
    /* ROLLBACK TO <savepoint> does not end the transaction */
    if( capdb_strnicmp((const char*)t, "TO", 2)==0 ) return TXN_NONE;
    return TXN_ROLLBACK;
  }
  return TXN_NONE;
}

static void sendExecResult(Session *p, int rc, int nChange, long long lastRowid){
  capdb_buf pl;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, rc);
  capdb_buf_append_i32(&pl, nChange);
  capdb_buf_append_i64(&pl, lastRowid);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_EXEC_RESULT, &pl);
  capdb_buf_clear(&pl);
}

static int handleExec(Session *p, capdb_reader *r){
  char *zSql = 0;
  int bWrite;
  int rc;
  int txn;
  capdb_buf pl, row;
  if( capdb_reader_str(r, &zSql) ) return sessionParseFail(p);
  txn = sqlTxnAction(zSql);

#if defined(CAPDB_ENABLE_STORE)
  if( serverIsReplica(p->pSrv) || serverIsDemoted(p->pSrv) ){
    bWrite = sqlIsWrite(zSql);
    if( bWrite || txn==TXN_BEGIN || txn==TXN_COMMIT || txn==TXN_ROLLBACK ){
      free(zSql);
      sendError(p, CAPDB_READONLY,
          serverIsDemoted(p->pSrv) ? "demoted primary is read-only" : "replica is read-only");
      return 0;
    }
  }
#endif

  /* --- explicit transaction control --- */
  if( txn==TXN_BEGIN ){
    if( p->inTxn ){
      sendError(p, CAPDB_ERROR, "already in a transaction");
      free(zSql);
      return 0;
    }
    if( sessionHasStmts(p) ){
      sendError(p, CAPDB_MISUSE, "finalize prepared statements first");
      free(zSql);
      return 0;
    }
    sessionReleaseDb(p);
    rc = sessionAcquireDb(p, 1);   /* WRITE checkout */
    free(zSql);
    if( rc!=CAPDB_OK ){
      sendError(p, rc, "pool acquire failed");
      return 0;
    }
#if defined(CAPDB_ENABLE_STORE)
    if( serverUsesVolume(p->pSrv) ){
      char *zErr = 0;
      rc = capdb_exec(p->db, "BEGIN IMMEDIATE", 0, 0, &zErr);
      if( rc!=CAPDB_OK ){
        sendError(p, rc, zErr ? zErr : capdb_errmsg(p->db));
        capdb_free(zErr);
        sessionReleaseDb(p);
        return 0;
      }
      capdb_free(zErr);
    }
#endif
    p->inTxn = 1;                  /* swallow the client BEGIN (already begun) */
    sendExecResult(p, CAPDB_OK, 0, 0);
    return 0;
  }
  if( txn==TXN_COMMIT || txn==TXN_ROLLBACK ){
    free(zSql);
    if( !p->inTxn ){
      sendError(p, CAPDB_MISUSE, "not in a transaction");
      return 0;
    }
    {
      char *zErr = 0;
      rc = capdb_exec(p->db, txn==TXN_COMMIT ? "COMMIT" : "ROLLBACK", 0, 0, &zErr);
      if( rc!=CAPDB_OK ){
        sendError(p, rc, zErr ? zErr : capdb_errmsg(p->db));
        capdb_free(zErr);
        return 0;
      }
      capdb_free(zErr);
#if defined(CAPDB_ENABLE_STORE) && defined(CAPDB_ENABLE_REPLICATION)
      if( txn==TXN_COMMIT && serverUsesVolume(p->pSrv) && p->pSrv->pRepSender ){
        serverVolumeReplicateSnapshot(p);
      }
#endif
#if defined(CAPDB_ENABLE_REPLICATION)
      if( txn==TXN_COMMIT && p->pSrv->pRepSender && p->pSrv->cfg.repSyncMode ){
        unsigned long long lsn = capdb_rep_sender_last_lsn(p->pSrv->pRepSender);
        if( lsn>0 && capdb_rep_sender_wait_ack(p->pSrv->pRepSender, lsn, 5000)!=CAPDB_OK ){
          p->inTxn = 0;
          sessionFreeStmts(p);
          sessionReleaseDb(p);
          sendError(p, CAPDB_BUSY, "sync replication timeout");
          return 0;
        }
      }
#endif
      p->inTxn = 0;
      sessionFreeStmts(p);
      sessionMaybeReleaseDb(p);
    }
    sendExecResult(p, CAPDB_OK, 0, 0);
    return 0;
  }

  /* --- normal statement --- */
  bWrite = sqlIsWrite(zSql);
  if( !p->inTxn ){
    rc = sessionReacquireDb(p, bWrite);
    if( rc!=CAPDB_OK ){
      free(zSql);
      if( rc!=CAPDB_MISUSE ) sendError(p, rc, "pool acquire failed");
      return 0;
    }
  }
  {
    capdb_stmt *s = 0;
    rc = capdb_prepare_v2(p->db, zSql, -1, &s, 0);
    free(zSql);
    if( rc!=CAPDB_OK ){
      sendError(p, rc, capdb_errmsg(p->db));
      if( !p->inTxn ) sessionMaybeReleaseDb(p);
      return 0;
    }
    capdb_buf_init(&pl);
    capdb_buf_append_i32(&pl, 0);
    {
      long nBytes = 0;
      int nRows = 0;
      while( (rc = capdb_step(s))==CAPDB_ROW ){
        int nCol = capdb_column_count(s);
        int i;
        if( nRows>=CAPDB_EXEC_MAX_ROWS || nBytes>=CAPDB_BATCH_BYTE_BUDGET ){
          capdb_finalize(s);
          if( !p->inTxn ) sessionMaybeReleaseDb(p);
          sendError(p, CAPDB_MISUSE, "result set too large");
          return 0;
        }
        capdb_buf_init(&row);
        capdb_buf_append_u32(&row, (unsigned)nCol);
        for(i=0; i<nCol; i++){
          appendColumnValue(&row, s, i);
        }
        nBytes += row.n;
        if( nBytes>CAPDB_BATCH_BYTE_BUDGET ){
          capdb_buf_clear(&row);
          capdb_finalize(s);
          if( !p->inTxn ) sessionMaybeReleaseDb(p);
          sendError(p, CAPDB_MISUSE, "result set too large");
          return 0;
        }
        capdb_stream_send_frame(p->pStream, CAPDB_MSG_STEP_ROW, &row);
        capdb_buf_clear(&row);
        nRows++;
      }
    }
    capdb_buf_clear(&pl);
    capdb_buf_init(&pl);
    capdb_buf_append_i32(&pl, rc==CAPDB_DONE ? CAPDB_OK : rc);
    capdb_buf_append_i32(&pl, capdb_changes(p->db));
    capdb_buf_append_i64(&pl, capdb_last_insert_rowid(p->db));
    /* auto-commit only when not inside an explicit transaction */
    if( bWrite && rc==CAPDB_DONE && !p->inTxn ){
#if defined(CAPDB_ENABLE_REPLICATION)
      if( serverUsesVolume(p->pSrv) && p->pSrv->pRepSender ){
        serverVolumeReplicateSnapshot(p);
        if( p->pSrv->cfg.repSyncMode ){
          unsigned long long lsn = capdb_rep_sender_last_lsn(p->pSrv->pRepSender);
          if( lsn>0 && capdb_rep_sender_wait_ack(p->pSrv->pRepSender, lsn, 5000)!=CAPDB_OK ){
            capdb_finalize(s);
            sessionReleaseDb(p);
            sendError(p, CAPDB_BUSY, "sync replication timeout");
            return 0;
          }
        }
      } else
#endif
#if defined(CAPDB_ENABLE_STORE)
      if( !serverUsesVolume(p->pSrv) )
#endif
      {
      char *zErr = 0;
      int crc = capdb_exec(p->db, "COMMIT", 0, 0, &zErr);
      if( crc!=CAPDB_OK ){
        sendError(p, crc, zErr ? zErr : capdb_errmsg(p->db));
        capdb_free(zErr);
        capdb_finalize(s);
        if( !p->inTxn ) sessionMaybeReleaseDb(p);
        return 0;
      }
      capdb_free(zErr);
#if defined(CAPDB_ENABLE_REPLICATION)
      if( p->pSrv->pRepSender && p->pSrv->cfg.repSyncMode ){
        unsigned long long lsn = capdb_rep_sender_last_lsn(p->pSrv->pRepSender);
        if( lsn>0 && capdb_rep_sender_wait_ack(p->pSrv->pRepSender, lsn, 5000)!=CAPDB_OK ){
          capdb_finalize(s);
          sessionReleaseDb(p);
          sendError(p, CAPDB_BUSY, "sync replication timeout");
          return 0;
        }
      }
#endif
      }
    }
    capdb_finalize(s);
    if( !p->inTxn ) sessionMaybeReleaseDb(p);
    capdb_stream_send_frame(p->pStream, CAPDB_MSG_EXEC_RESULT, &pl);
    capdb_buf_clear(&pl);
  }
  return 0;
}

static ServerStmt *findStmt(Session *p, int id){
  ServerStmt *s;
  for(s=p->pStmts; s; s=s->pNext){
    if( s->id==id ) return s;
  }
  return 0;
}

static int handlePrepare(Session *p, capdb_reader *r){
  char *zSql = 0;
  int bWrite;
  int rc;
  ServerStmt *st;
  capdb_buf pl;
  int nStmts = 0;
  ServerStmt *s;
  if( capdb_reader_str(r, &zSql) ) return sessionParseFail(p);
  for(s=p->pStmts; s; s=s->pNext) nStmts++;
  if( nStmts >= CAPDB_MAX_STMTS ){
    sendError(p, CAPDB_MISUSE, "too many prepared statements");
    free(zSql);
    return 0;
  }
  bWrite = sqlIsWrite(zSql);
#if defined(CAPDB_ENABLE_STORE)
  if( serverIsReplica(p->pSrv) || serverIsDemoted(p->pSrv) ){
    int txn = sqlTxnAction(zSql);
    if( bWrite || txn==TXN_BEGIN || txn==TXN_COMMIT || txn==TXN_ROLLBACK ){
      free(zSql);
      sendError(p, CAPDB_READONLY,
          serverIsDemoted(p->pSrv) ? "demoted primary is read-only" : "replica is read-only");
      return 0;
    }
  }
#endif
  if( !p->inTxn ){
    rc = sessionReacquireDb(p, bWrite);
    if( rc!=CAPDB_OK ){
      free(zSql);
      if( rc!=CAPDB_MISUSE ) sendError(p, rc, "pool acquire failed");
      return 0;
    }
  }
  st = (ServerStmt*)calloc(1, sizeof(*st));
  if( st==0 ){
    free(zSql);
    sendError(p, CAPDB_NOMEM, "out of memory");
    return 0;
  }
  st->id = ++p->nStmtId;
  st->bWrite = bWrite;
  rc = capdb_prepare_v2(p->db, zSql, -1, &st->pStmt, 0);
  free(zSql);
  if( rc!=CAPDB_OK ){
    free(st);
    sendError(p, rc, capdb_errmsg(p->db));
    if( !p->inTxn ) sessionMaybeReleaseDb(p);
    return 0;
  }
  st->pNext = p->pStmts;
  p->pStmts = st;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, st->id);
  capdb_buf_append_i32(&pl, capdb_column_count(st->pStmt));
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_PREPARE_OK, &pl);
  capdb_buf_clear(&pl);
  return 0;
}

static int handleStep(Session *p, capdb_reader *r){
  int id, rc;
  int batch = 1;
  int sent = 0;
  long nBytes = 0;
  ServerStmt *st;
  capdb_buf pl;
  if( !sessionRequireDb(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  capdb_reader_i32(r, &batch);   /* optional; absent => single-row (legacy) */
  if( batch < 1 ) batch = 1;
  st = findStmt(p, id);
  if( st==0 ){
    sendError(p, CAPDB_MISUSE, "bad stmt id");
    return 0;
  }
#if defined(CAPDB_ENABLE_STORE)
  if( (serverIsReplica(p->pSrv) || serverIsDemoted(p->pSrv)) && st->bWrite ){
    sendError(p, CAPDB_READONLY,
        serverIsDemoted(p->pSrv) ? "demoted primary is read-only" : "replica is read-only");
    return 0;
  }
#endif
  /* Stream up to `batch` rows OR ~CAPDB_BATCH_BYTE_BUDGET bytes (whichever comes
  ** first), then a terminal STEP_DONE carrying rc + a "more rows pending" flag.
  ** Bounding by bytes as well as count caps the client's prefetch buffer so a
  ** result set of very wide rows cannot make it allocate gigabytes. */
  for(;;){
    rc = capdb_step(st->pStmt);
    if( rc!=CAPDB_ROW ) break;
    {
      int nCol = capdb_column_count(st->pStmt);
      int i;
      capdb_buf_init(&pl);
      capdb_buf_append_u32(&pl, (unsigned)nCol);
      for(i=0; i<nCol; i++) appendColumnValue(&pl, st->pStmt, i);
      nBytes += pl.n;
      capdb_stream_send_frame(p->pStream, CAPDB_MSG_STEP_ROW, &pl);
      capdb_buf_clear(&pl);
    }
    /* both limits leave rc==CAPDB_ROW => "more" so the client requests again */
    if( ++sent >= batch ) break;
    if( nBytes >= CAPDB_BATCH_BYTE_BUDGET ) break;
  }
  capdb_buf_init(&pl);
  if( rc==CAPDB_ROW ){
    capdb_buf_append_i32(&pl, CAPDB_ROW);   /* paused at batch limit */
    capdb_buf_append_u8(&pl, 1);            /* more rows pending */
  }else{
    capdb_buf_append_i32(&pl, rc);          /* DONE or error */
    capdb_buf_append_u8(&pl, 0);
  }
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_STEP_DONE, &pl);
  capdb_buf_clear(&pl);
  return 0;
}

static int handleFinalize(Session *p, capdb_reader *r){
  int id;
  int found = 0;
  ServerStmt *st, **pp;
  capdb_buf reply;
  if( !sessionRequireDb(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  for(pp=&p->pStmts; *pp; pp=&(*pp)->pNext){
    if( (*pp)->id==id ){
      st = *pp;
      *pp = st->pNext;
      capdb_finalize(st->pStmt);
      free(st);
      found = 1;
      break;
    }
  }
  if( !found ){
    sendError(p, CAPDB_MISUSE, "bad stmt id");
    return 0;
  }
  /* Keep the connection pinned while an explicit transaction is open. */
  if( p->pStmts==0 && !p->inTxn ) sessionMaybeReleaseDb(p);
  capdb_buf_init(&reply);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_FINALIZE_OK, &reply);
  capdb_buf_clear(&reply);
  return 0;
}

static VfsSlot *findVfs(Session *p, int id){
  VfsSlot *v;
  for(v=p->pVfs; v; v=v->pNext){
    if( v->id==id ) return v;
  }
  return 0;
}

static void vfsSendPong(Session *p){
  capdb_buf reply;
  capdb_buf_init(&reply);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_PONG, &reply);
  capdb_buf_clear(&reply);
}

static int pathEndsWithDbExt(const char *z){
  static const char *aExt[] = { ".db", ".sqlite", ".sqlite3", 0 };
  size_t n = strlen(z);
  int i;
  for(i=0; aExt[i]; i++){
    size_t ne = strlen(aExt[i]);
    if( n>=ne && strcmp(z+n-ne, aExt[i])==0 ) return 1;
  }
  return 0;
}

static int vfsSetLock(capdb_server *pSrv, const char *zPath, int eNew){
  return pathRegistrySetLock(pSrv, zPath, eNew);
}

static int handleVfsOpen(Session *p, capdb_reader *r){
  char *zPath = 0;
  char *zAllowed = 0;
  VfsSlot *v;
  capdb_buf reply;
  int fd;
  int nVfs = 0;
  VfsSlot *vv;
  struct stat st;
  int flags = O_RDWR;
  if( !sessionRequireOpen(p) ) return 0;
  for(vv=p->pVfs; vv; vv=vv->pNext) nVfs++;
  if( nVfs >= CAPDB_MAX_VFS ){
    sendError(p, CAPDB_MISUSE, "too many vfs handles");
    return 0;
  }
  if( capdb_reader_str(r, &zPath) ) return sessionParseFail(p);
  if( pathIsAllowed(p->pSrv, zPath, &zAllowed) ){
    sendError(p, CAPDB_PERM, "path not allowed");
    free(zPath);
    return 0;
  }
  free(zPath);
  if( access(zAllowed, F_OK)!=0 ){
    if( !pathEndsWithDbExt(zAllowed) ){
      sendError(p, CAPDB_PERM, "create only allowed for .db paths");
      free(zAllowed);
      return 0;
    }
    flags |= O_CREAT;
  }
  fd = open(zAllowed, flags, 0644);
  if( fd<0 ){
    sendError(p, CAPDB_CANTOPEN, strerror(errno));
    free(zAllowed);
    return 0;
  }
  if( fstat(fd, &st) || !S_ISREG(st.st_mode) ){
    close(fd);
    sendError(p, CAPDB_CANTOPEN, "not a regular file");
    free(zAllowed);
    return 0;
  }
  {
    int prc = pathRegistryVfsOpen(p->pSrv, zAllowed, fd);
    if( prc!=CAPDB_OK ){
      close(fd);
      free(zAllowed);
      if( prc==CAPDB_BUSY ){
        sendError(p, CAPDB_BUSY, "path in use by pool");
      }else{
        sendError(p, prc, "vfs open failed");
      }
      return 0;
    }
  }
  v = (VfsSlot*)calloc(1, sizeof(*v));
  if( v==0 ){
    close(fd);
    pathRegistryVfsClose(p->pSrv, zAllowed);
    free(zAllowed);
    sendError(p, CAPDB_NOMEM, "out of memory");
    return 0;
  }
  v->id = ++p->nVfsId;
  v->fd = fd;
  v->zPath = zAllowed;
  v->pNext = p->pVfs;
  p->pVfs = v;
  capdb_buf_init(&reply);
  capdb_buf_append_i32(&reply, v->id);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_OPEN_OK, &reply);
  capdb_buf_clear(&reply);
  return 0;
}

static int handleVfsClose(Session *p, capdb_reader *r){
  int id;
  int found = 0;
  VfsSlot *v, **pp;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  for(pp=&p->pVfs; *pp; pp=&(*pp)->pNext){
    if( (*pp)->id==id ){
      v = *pp;
      *pp = v->pNext;
      pathRegistryVfsClose(p->pSrv, v->zPath);
      close(v->fd);
      free(v->zPath);
      free(v);
      found = 1;
      break;
    }
  }
  if( !found ){
    sendError(p, CAPDB_MISUSE, "bad vfs id");
    return 0;
  }
  vfsSendPong(p);
  return 0;
}

static int handleVfsRead(Session *p, capdb_reader *r){
  int id, n;
  long long iOff;
  VfsSlot *v;
  capdb_buf reply;
  unsigned char *a;
  ssize_t got;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  if( capdb_reader_i64(r, &iOff) ) return sessionParseFail(p);
  if( capdb_reader_i32(r, &n) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  if( n<0 || n>CAPDB_MAX_FRAME_SIZE ){
    sendError(p, CAPDB_MISUSE, "bad read size");
    return 0;
  }
  a = (unsigned char*)malloc(n>0 ? (size_t)n : 1);
  if( a==0 ){
    sendError(p, CAPDB_NOMEM, "out of memory");
    return 0;
  }
  got = pread(v->fd, a, (size_t)n, (off_t)iOff);
  if( got<0 ){
    free(a);
    sendError(p, CAPDB_IOERR, strerror(errno));
    return 0;
  }
  capdb_buf_init(&reply);
  if( capdb_buf_append_i32(&reply, (int)got)
   || capdb_buf_append(&reply, a, (int)got) ){
    free(a);
    sendError(p, CAPDB_NOMEM, "out of memory");
    return 0;
  }
  free(a);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_READ_OK, &reply);
  capdb_buf_clear(&reply);
  return 0;
}

static int handleVfsWrite(Session *p, capdb_reader *r){
  int id, n;
  long long iOff;
  VfsSlot *v;
  const unsigned char *a;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  if( capdb_reader_i64(r, &iOff) ) return sessionParseFail(p);
  if( capdb_reader_i32(r, &n) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  if( !capdb_reader_bytes(r, n) ) return sessionParseFail(p);
  if( capdb_reader_consume(r, n) ) return sessionParseFail(p);
  a = r->a + (r->i - n);
  if( pwrite(v->fd, a, (size_t)n, (off_t)iOff)!=(ssize_t)n ){
    sendError(p, CAPDB_IOERR, "write failed");
    return 0;
  }
  vfsSendPong(p);
  return 0;
}

static int handleVfsTruncate(Session *p, capdb_reader *r){
  int id;
  long long nByte;
  VfsSlot *v;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  if( capdb_reader_i64(r, &nByte) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  if( ftruncate(v->fd, (off_t)nByte) ){
    sendError(p, CAPDB_IOERR, "truncate failed");
    return 0;
  }
  vfsSendPong(p);
  return 0;
}

static int handleVfsSync(Session *p, capdb_reader *r){
  int id, flags;
  VfsSlot *v;
  (void)flags;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  if( capdb_reader_i32(r, &flags) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  if( flags & 0x08 ){
    if( fdatasync(v->fd) ){
      sendError(p, CAPDB_IOERR, "fdatasync failed");
      return 0;
    }
  }else if( fsync(v->fd) ){
    sendError(p, CAPDB_IOERR, "fsync failed");
    return 0;
  }
  vfsSendPong(p);
  return 0;
}

static int handleVfsSize(Session *p, capdb_reader *r){
  int id;
  VfsSlot *v;
  struct stat st;
  capdb_buf reply;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  if( fstat(v->fd, &st) ){
    sendError(p, CAPDB_IOERR, "fstat failed");
    return 0;
  }
  capdb_buf_init(&reply);
  capdb_buf_append_i64(&reply, (long long)st.st_size);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_SIZE_OK, &reply);
  capdb_buf_clear(&reply);
  return 0;
}

static int handleVfsLock(Session *p, capdb_reader *r){
  int id, eLock, ok;
  VfsSlot *v;
  capdb_buf reply;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  if( capdb_reader_i32(r, &eLock) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  ok = vfsSetLock(p->pSrv, v->zPath, eLock);
  capdb_buf_init(&reply);
  capdb_buf_append_i32(&reply, ok);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_LOCK_OK, &reply);
  capdb_buf_clear(&reply);
  return 0;
}

static int handleVfsCheckReserved(Session *p, capdb_reader *r){
  int id, reserved = 0;
  VfsSlot *v;
  capdb_buf reply;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_i32(r, &id) ) return sessionParseFail(p);
  v = findVfs(p, id);
  if( v==0 ){ sendError(p, CAPDB_MISUSE, "bad vfs id"); return 0; }
  reserved = pathRegistryCheckReserved(p->pSrv, v->zPath);
  capdb_buf_init(&reply);
  capdb_buf_append_i32(&reply, reserved);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_CHECK_RESERVED_OK, &reply);
  capdb_buf_clear(&reply);
  return 0;
}

static int handleVfsDelete(Session *p, capdb_reader *r){
  char *zPath = 0;
  char *zAllowed = 0;
  int syncDir = 0;
  int rc;
  if( !sessionRequireOpen(p) ) return 0;
  if( capdb_reader_str(r, &zPath) ) return sessionParseFail(p);
  if( capdb_reader_i32(r, &syncDir) ){
    free(zPath);
    return sessionParseFail(p);
  }
  if( pathIsAllowed(p->pSrv, zPath, &zAllowed) ){
    sendError(p, CAPDB_PERM, "path not allowed");
    free(zPath);
    return 0;
  }
  free(zPath);
  rc = pathRegistryCanDelete(p->pSrv, zAllowed);
  if( rc!=CAPDB_OK ){
    free(zAllowed);
    sendError(p, rc, rc==CAPDB_BUSY ? "path in use" : "delete denied");
    return 0;
  }
  if( unlink(zAllowed)!=0 && errno!=ENOENT ){
    sendError(p, CAPDB_IOERR_DELETE, strerror(errno));
    free(zAllowed);
    return 0;
  }
  if( syncDir ){
    char *zSlash = strrchr(zAllowed, '/');
    if( zSlash ){
      int dfd;
      *zSlash = 0;
      dfd = open(zAllowed[0] ? zAllowed : "/", O_RDONLY);
      if( dfd>=0 ){
        fsync(dfd);
        close(dfd);
      }
    }
  }
  free(zAllowed);
  vfsSendPong(p);
  return 0;
}

static void sessionResetDbState(Session *p){
  VfsSlot *v, *vNext;
  if( p==0 ) return;
  sessionFreeStmts(p);
  p->nStmtId = 0;
  for(v=p->pVfs; v; v=vNext){
    vNext = v->pNext;
    pathRegistryVfsClose(p->pSrv, v->zPath);
    close(v->fd);
    free(v->zPath);
    free(v);
  }
  p->pVfs = 0;
  p->nVfsId = 0;
  if( p->inTxn ){
    p->inTxn = 0;
  }
  if( p->zDbPath && (p->pPool
#if defined(CAPDB_ENABLE_STORE)
      || serverUsesVolume(p->pSrv)
#endif
  )){
    pathRegistryDropPoolRef(p->pSrv, p->zDbPath);
  }
  sessionReleaseDb(p);
  p->pPool = 0;
}

static void serverSessionFree(Session *p){
  if( p==0 ) return;
  sessionResetDbState(p);
  free(p->zDbPath);
  p->zDbPath = 0;
  capdbAudit(p->pSrv, "conn.close", p->zPeer, 0);
  capdb_stream_close(p->pStream);
  free(p);
}

static int sessionHandle(Session *p){
  unsigned char type;
  capdb_buf payload;
  capdb_reader r;

  if( capdb_stream_recv_frame(p->pStream, &type, &payload) ) return -1;
  capdb_reader_init(&r, payload.a, payload.n);

  switch( type ){
    case CAPDB_MSG_AUTH: {
      unsigned char method;
      char *zUser = 0, *zSecret = 0;
      capdb_buf reply;
      if( !p->bAuthed ){
        if( capdb_reader_u8(&r, &method) ){
          sendError(p, CAPDB_MISUSE, "malformed frame");
          capdb_buf_clear(&payload);
          return -1;
        }
        if( method==CAPDB_AUTH_PASSWORD ){
          if( capdb_reader_str(&r, &zUser) ){
            sendError(p, CAPDB_MISUSE, "malformed frame");
            capdb_buf_clear(&payload);
            return -1;
          }
        }
        if( capdb_reader_str(&r, &zSecret) ){
          sendError(p, CAPDB_MISUSE, "malformed frame");
          free(zUser);
          capdb_buf_clear(&payload);
          return -1;
        }
        if( capdb_auth_check_peer(p->pSrv->cfg.zAuthFile, method, zUser, zSecret,
                                  p->zPeer)==0 ){
          p->bAuthed = 1;
          capdbAudit(p->pSrv, "auth.ok", p->zPeer,
                     method==CAPDB_AUTH_PASSWORD ? (zUser ? zUser : "user") : "token");
          sessionSetRecvTimeout(p, CAPDB_IDLE_TIMEOUT_MS);
          capdb_buf_init(&reply);
          capdb_stream_send_frame(p->pStream, CAPDB_MSG_AUTH_OK, &reply);
          capdb_buf_clear(&reply);
        }else{
          capdbAudit(p->pSrv, "auth.fail", p->zPeer,
                     method==CAPDB_AUTH_PASSWORD ? (zUser ? zUser : "user") : "token");
          capdb_buf_init(&reply);
          capdb_buf_append_str(&reply, "authentication failed");
          capdb_stream_send_frame(p->pStream, CAPDB_MSG_AUTH_FAIL, &reply);
          capdb_buf_clear(&reply);
          free(zUser); free(zSecret);
          capdb_buf_clear(&payload);
          return -1;
        }
        free(zUser); free(zSecret);
      }else{
        capdb_buf reply;
        capdb_buf_init(&reply);
        capdb_stream_send_frame(p->pStream, CAPDB_MSG_AUTH_OK, &reply);
        capdb_buf_clear(&reply);
      }
      break;
    }
    case CAPDB_MSG_OPEN: {
      char *zPath = 0;
      char *zAllowed = 0;
      capdb_buf reply;
      if( !sessionRequireAuthed(p) ) break;
      if( capdb_reader_str(&r, &zPath) ){
        sendError(p, CAPDB_MISUSE, "malformed frame");
        capdb_buf_clear(&payload);
        return -1;
      }
      if( p->inTxn ){
        sendError(p, CAPDB_MISUSE, "cannot switch database during transaction");
        free(zPath);
        break;
      }
      if( pathIsAllowed(p->pSrv, zPath, &zAllowed) ){
        sendError(p, CAPDB_PERM, "path not allowed");
        free(zPath);
        break;
      }
      sessionResetDbState(p);
      free(p->zDbPath);
      p->zDbPath = zAllowed;
      zAllowed = 0;
#if defined(CAPDB_ENABLE_STORE)
      if( serverUsesVolume(p->pSrv) ){
        p->pPool = 0;
        {
          int prc = pathRegistryAddPoolRef(p->pSrv, p->zDbPath);
          if( prc!=CAPDB_OK ){
            free(p->zDbPath);
            p->zDbPath = 0;
            if( prc==CAPDB_BUSY ){
              sendError(p, CAPDB_BUSY, "path in use by vfs");
            }else{
              sendError(p, prc, "pool register failed");
            }
            break;
          }
        }
      }else
#endif
      {
      p->pPool = getPool(p->pSrv, p->zDbPath);
      if( p->pPool==0 ){
        sendError(p, CAPDB_ERROR, "pool open failed");
        break;
      }
      {
        int prc = pathRegistryAddPoolRef(p->pSrv, p->zDbPath);
        if( prc!=CAPDB_OK ){
          p->pPool = 0;
          free(p->zDbPath);
          p->zDbPath = 0;
          if( prc==CAPDB_BUSY ){
            sendError(p, CAPDB_BUSY, "path in use by vfs");
          }else{
            sendError(p, prc, "pool register failed");
          }
          break;
        }
      }
      }
      free(zPath);
      capdb_buf_init(&reply);
      capdb_stream_send_frame(p->pStream, CAPDB_MSG_OPEN_OK, &reply);
      capdb_buf_clear(&reply);
      break;
    }
    case CAPDB_MSG_EXEC:
      if( sessionRequireOpen(p) ) handleExec(p, &r);
      break;
    case CAPDB_MSG_PREPARE:
      if( sessionRequireOpen(p) ) handlePrepare(p, &r);
      break;
    case CAPDB_MSG_STEP:
      handleStep(p, &r);
      break;
    case CAPDB_MSG_FINALIZE:
      handleFinalize(p, &r);
      break;
    case CAPDB_MSG_PING: {
      capdb_buf reply;
      capdb_buf_init(&reply);
      capdb_stream_send_frame(p->pStream, CAPDB_MSG_PONG, &reply);
      capdb_buf_clear(&reply);
      break;
    }
    case CAPDB_MSG_CLOSE:
      capdb_buf_clear(&payload);
      return -1;
    case CAPDB_MSG_VFS_OPEN:
      handleVfsOpen(p, &r);
      break;
    case CAPDB_MSG_VFS_CLOSE:
      handleVfsClose(p, &r);
      break;
    case CAPDB_MSG_VFS_READ:
      handleVfsRead(p, &r);
      break;
    case CAPDB_MSG_VFS_WRITE:
      handleVfsWrite(p, &r);
      break;
    case CAPDB_MSG_VFS_TRUNCATE:
      handleVfsTruncate(p, &r);
      break;
    case CAPDB_MSG_VFS_SYNC:
      handleVfsSync(p, &r);
      break;
    case CAPDB_MSG_VFS_SIZE:
      handleVfsSize(p, &r);
      break;
    case CAPDB_MSG_VFS_LOCK:
      handleVfsLock(p, &r);
      break;
    case CAPDB_MSG_VFS_CHECK_RESERVED:
      handleVfsCheckReserved(p, &r);
      break;
    case CAPDB_MSG_VFS_DELETE:
      handleVfsDelete(p, &r);
      break;
    default:
      sendError(p, CAPDB_MISUSE, "unknown message type");
      capdb_buf_clear(&payload);
      return -1;
  }
  if( r.i != payload.n ){
    sendError(p, CAPDB_MISUSE, "trailing frame data");
    capdb_buf_clear(&payload);
    return -1;
  }
  capdb_buf_clear(&payload);
  return 0;
}

static void *sessionThread(void *pArg){
  Session *p = (Session*)pArg;
  capdb_buf pl;
  capdb_reader r;
  unsigned char type;

  /* Bound the unauthenticated handshake (HELLO + AUTH) against slow-loris. */
  sessionSetRecvTimeout(p, CAPDB_HANDSHAKE_TIMEOUT_MS);

  /* Expect HELLO then respond */
  if( capdb_stream_recv_frame(p->pStream, &type, &pl) ) goto done;
  if( type!=CAPDB_MSG_HELLO ) goto done;
  capdb_reader_init(&r, pl.a, pl.n);
  {
    unsigned int ver = 0;
    if( capdb_reader_u32(&r, &ver) || ver!=CAPDB_PROTO_VERSION ) goto done;
  }
  capdb_buf_clear(&pl);

  capdb_buf_init(&pl);
  capdb_buf_append_u32(&pl, CAPDB_PROTO_VERSION);
  capdb_stream_send_frame(p->pStream, CAPDB_MSG_HELLO_ACK, &pl);
  capdb_buf_clear(&pl);

  while( !p->pSrv->stop ){
    if( sessionHandle(p) ) break;
  }
done:
  {
    capdb_server *pSrv = p->pSrv;
    int slot = p->iTrackSlot;
    serverSessionFree(p);
    serverSessionReleaseSlot(pSrv, slot);
    pthread_mutex_lock(&pSrv->poolMutex);
    pSrv->nSessions--;
    pthread_mutex_unlock(&pSrv->poolMutex);
  }
  return 0;
}

static void *acceptThread(void *pArg){
  capdb_server *pSrv = (capdb_server*)pArg;
  capdb_tls_config tlsCfg;
  memset(&tlsCfg, 0, sizeof(tlsCfg));
  tlsCfg.zCertFile = pSrv->cfg.zCertFile;
  tlsCfg.zKeyFile = pSrv->cfg.zKeyFile;
  tlsCfg.zCaFile = pSrv->cfg.zCaFile;
  tlsCfg.bServer = 1;
  tlsCfg.bInsecure = pSrv->cfg.bInsecure;
  tlsCfg.pSharedCtx = pSrv->pSslCtx;

  while( !pSrv->stop ){
    int fd = -1;
    capdb_stream *strm = 0;
    Session *sess = 0;
    pthread_t tid;
    int slot = -1;
    struct pollfd pf;
    if( pSrv->listenFd<0 ) break;
    pf.fd = pSrv->listenFd;
    pf.events = POLLIN;
    if( poll(&pf, 1, 200)<=0 ) continue;
    if( pSrv->stop ) break;
    if( capdb_tcp_accept(pSrv->listenFd, &fd) ) continue;
    if( capdb_stream_accept(fd, &tlsCfg, &strm) ){
      close(fd);
      continue;
    }
    /* Enforce a maximum number of concurrent sessions so a flood of
    ** connections cannot exhaust threads/memory. Reserve a slot before
    ** spawning; the slot is released when the session thread exits. */
    {
      int cap = pSrv->cfg.maxClients>0 ? pSrv->cfg.maxClients
                                       : CAPDB_DEFAULT_MAX_CLIENTS;
      int over;
      pthread_mutex_lock(&pSrv->poolMutex);
      over = pSrv->nSessions >= cap;
      if( !over ) pSrv->nSessions++;
      pthread_mutex_unlock(&pSrv->poolMutex);
      if( over ){
        capdb_stream_close(strm);
        continue;
      }
    }
    sess = (Session*)calloc(1, sizeof(*sess));
    if( sess==0 ){
      pthread_mutex_lock(&pSrv->poolMutex);
      pSrv->nSessions--;
      pthread_mutex_unlock(&pSrv->poolMutex);
      capdb_stream_close(strm);
      continue;
    }
    sess->pSrv = pSrv;
    sess->pStream = strm;
    sess->iTrackSlot = -1;
    capdbPeer(fd, sess->zPeer, sizeof(sess->zPeer));
    capdbAudit(pSrv, "conn.accept", sess->zPeer, 0);
    pthread_mutex_lock(&pSrv->poolMutex);
    slot = serverSessionFindSlot(pSrv);
    pthread_mutex_unlock(&pSrv->poolMutex);
    if( slot<0 ){
      pthread_mutex_lock(&pSrv->poolMutex);
      pSrv->nSessions--;
      pthread_mutex_unlock(&pSrv->poolMutex);
      capdb_stream_close(strm);
      free(sess);
      continue;
    }
    if( pthread_create(&tid, 0, sessionThread, sess)!=0 ){
      pthread_mutex_lock(&pSrv->poolMutex);
      pSrv->nSessions--;
      pthread_mutex_unlock(&pSrv->poolMutex);
      capdb_stream_close(strm);
      free(sess);
      continue;
    }
    sess->iTrackSlot = slot;
    pthread_mutex_lock(&pSrv->poolMutex);
    serverSessionAssign(pSrv, slot, tid, strm);
    pthread_mutex_unlock(&pSrv->poolMutex);
  }
  return 0;
}

int capdb_server_start(const capdb_server_config *pCfg, capdb_server **pp){
  capdb_server *p;
  if( pCfg==0 || pp==0 ) return -1;
  capdb_tls_global_init();
  capdb_initialize();
  p = (capdb_server*)calloc(1, sizeof(*p));
  if( p==0 ) return -1;
  p->cfg = *pCfg;
  pthread_mutex_init(&p->poolMutex, 0);
  pthread_mutex_init(&p->pathMutex, 0);
  if( !pCfg->bInsecure ){
    capdb_tls_config tlsCfg;
    memset(&tlsCfg, 0, sizeof(tlsCfg));
    tlsCfg.zCertFile = pCfg->zCertFile;
    tlsCfg.zKeyFile = pCfg->zKeyFile;
    tlsCfg.zCaFile = pCfg->zCaFile;
    tlsCfg.bServer = 1;
    p->pSslCtx = capdb_tls_server_ctx(&tlsCfg);
    if( p->pSslCtx==0 ){
      pthread_mutex_destroy(&p->poolMutex);
      free(p);
      return -1;
    }
  }
  if( capdb_tcp_listen(pCfg->zListen ? pCfg->zListen : CAPDB_DEFAULT_LISTEN,
                         &p->listenFd) ){
    capdb_tls_ctx_free(p->pSslCtx);
    pthread_mutex_destroy(&p->poolMutex);
    free(p);
    return -1;
  }
#if defined(CAPDB_ENABLE_STORE)
  if( pCfg->zStorage && strcmp(pCfg->zStorage,"volume")==0 && pCfg->zVolumeRoot ){
    if( capdb_store_vfs_register(pCfg->zVolumeRoot, 0) ){
      capdb_tls_ctx_free(p->pSslCtx);
      pthread_mutex_destroy(&p->poolMutex);
      free(p);
      return -1;
    }
    capdb_store_init();
  }
#if defined(CAPDB_ENABLE_REPLICATION)
  if( serverRepStart(p) ){
    capdb_tls_ctx_free(p->pSslCtx);
    pthread_mutex_destroy(&p->poolMutex);
    free(p);
    return -1;
  }
#endif
#endif /* CAPDB_ENABLE_STORE */
  *pp = p;
  return 0;
}

void capdb_server_request_stop(capdb_server *p){
  if( p ) p->stop = 1;
}

static void capdb_server_wake(capdb_server *p){
  int i;
  if( p==0 ) return;
  if( p->listenFd>=0 ){
    close(p->listenFd);
    p->listenFd = -1;
  }
  for(i=0; i<p->nSessionTidsAlloc; i++){
    if( p->aSessionStreams && p->aSessionStreams[i] ){
      int fd = capdb_stream_fd(p->aSessionStreams[i]);
      if( fd>=0 ) shutdown(fd, SHUT_RDWR);
    }
  }
}

int capdb_server_run(capdb_server *p){
  int max;
  if( p==0 ) return -1;
  max = p->cfg.maxClients>0 ? p->cfg.maxClients : CAPDB_DEFAULT_MAX_CLIENTS;
  p->aSessionTids = (pthread_t*)calloc((size_t)max, sizeof(pthread_t));
  p->aSessionStreams = (capdb_stream**)calloc((size_t)max, sizeof(capdb_stream*));
  if( p->aSessionTids==0 || p->aSessionStreams==0 ){
    free(p->aSessionTids);
    free(p->aSessionStreams);
    p->aSessionTids = 0;
    p->aSessionStreams = 0;
    return -1;
  }
  p->nSessionTidsAlloc = max;
  p->stop = 0;
  p->acceptStarted = 1;
  if( pthread_create(&p->acceptThread, 0, acceptThread, p)!=0 ) return -1;
  while( !p->stop ){
    usleep(10000);
  }
  capdb_server_wake(p);
  return 0;
}

void capdb_server_stop(capdb_server *p){
  PoolEntry *e, *eNext;
  int i;
  if( p==0 ) return;
  capdb_server_request_stop(p);
  capdb_server_wake(p);
#if defined(CAPDB_ENABLE_REPLICATION)
  serverRepStop(p);
#endif

  if( p->acceptStarted ) pthread_join(p->acceptThread, 0);
  for(i=0; i<p->nSessionTidsAlloc; i++){
    if( p->aSessionStreams && p->aSessionStreams[i] ){
      pthread_join(p->aSessionTids[i], 0);
    }
  }
  free(p->aSessionTids);
  free(p->aSessionStreams);
  p->aSessionTids = 0;
  p->aSessionStreams = 0;
  p->nSessionActive = 0;
  p->nSessionTidsAlloc = 0;

  {
    PathEntry *pe, *peNext;
    for(pe=p->pPaths; pe; pe=peNext){
      peNext = pe->pNext;
      if( pe->lockFd>=0 ) close(pe->lockFd);
      free(pe->zPath);
      free(pe);
    }
    p->pPaths = 0;
  }

  for(e=p->pPools; e; e=eNext){
    eNext = e->pNext;
    if( capdb_pool_close(e->pPool)==CAPDB_BUSY ){
      fprintf(stderr, "capdb: pool still busy at shutdown for %s\n",
              e->zPath ? e->zPath : "?");
    }
    free(e->zPath);
    free(e);
  }
  p->pPools = 0;
  capdb_tls_ctx_free(p->pSslCtx);
  p->pSslCtx = 0;
  pthread_mutex_destroy(&p->poolMutex);
  pthread_mutex_destroy(&p->pathMutex);
  free(p);
}

#endif /* CAPDB_ENABLE_NETWORK && CAPDB_ENABLE_POOL */
