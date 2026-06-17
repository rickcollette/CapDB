
#if defined(CAPDB_ENABLE_POOL)

#include "capdb_pool.h"
#include "capdb.h"
#include <string.h>
#include <stdlib.h>

#ifndef CAPDB_AMALGAMATION
typedef unsigned char u8;
#endif

#define POOL_DEFAULT_MIN_SIZE        1
#define POOL_DEFAULT_MAX_SIZE        8
#define POOL_DEFAULT_BUSY_TIMEOUT    5000
#define POOL_DEFAULT_FLAGS \
  (CAPDB_POOL_DEFAULT_WAL \
   | CAPDB_POOL_SERIALIZE_WRITES \
   | CAPDB_POOL_RESET_ON_RELEASE)
#define POOL_WAIT_SLICE_MS           10

typedef struct PoolSlot PoolSlot;
struct PoolSlot {
  capdb *db;
  u8 inUse;
  u8 mode;
};

struct capdb_pool {
  capdb_mutex *mutex;
  char *zFilename;
  capdb_pool_config cfg;
  PoolSlot *aSlot;
  int nConn;
  int nWriteActive;
  int shutdown;
};

static void poolApplyDefaults(capdb_pool_config *pCfg){
  if( pCfg->minSize < 0 ) pCfg->minSize = POOL_DEFAULT_MIN_SIZE;
  if( pCfg->maxSize<=0 ) pCfg->maxSize = POOL_DEFAULT_MAX_SIZE;
  if( pCfg->busyTimeoutMs<=0 ) pCfg->busyTimeoutMs = POOL_DEFAULT_BUSY_TIMEOUT;
  if( pCfg->flags==0 ) pCfg->flags = POOL_DEFAULT_FLAGS;
  if( pCfg->minSize > pCfg->maxSize ) pCfg->minSize = pCfg->maxSize;
}

/* Deny ATTACH so SQL executed on a pooled connection cannot open database
** files outside the managed one (e.g. ATTACH DATABASE '/etc/...'), which would
** otherwise bypass any path jail the caller enforces on the primary file. */
static int poolAuthorizer(void *pUnused, int action, const char *a,
                          const char *b, const char *c, const char *d){
  (void)pUnused; (void)a; (void)b; (void)c; (void)d;
  if( action==CAPDB_ATTACH || action==CAPDB_DETACH ) return CAPDB_DENY;
  return CAPDB_OK;
}

static int poolCreateConnection(capdb_pool *pPool, capdb **ppDb){
  unsigned flags;
  int rc;
  char *zErr = 0;

  flags = CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE | pPool->cfg.openFlags;
  rc = capdb_open_v2(pPool->zFilename, ppDb, flags, pPool->cfg.zVfs);
  if( rc ) return rc;

  capdb_busy_timeout(*ppDb, pPool->cfg.busyTimeoutMs);
  capdb_set_authorizer(*ppDb, poolAuthorizer, 0);

  if( pPool->cfg.flags & CAPDB_POOL_DEFAULT_WAL ){
    rc = capdb_exec(*ppDb, "PRAGMA journal_mode=WAL", 0, 0, &zErr);
    if( rc!=CAPDB_OK ){
      capdb_free(zErr);
      /* Ignore WAL setup failure (e.g. read-only database). */
      rc = CAPDB_OK;
    }else{
      capdb_free(zErr);
    }
  }

  return CAPDB_OK;
}

static int poolGrow(capdb_pool *pPool){
  PoolSlot *aNew;
  int iNew;
  capdb *db = 0;
  int rc;

  if( pPool->nConn >= pPool->cfg.maxSize ) return CAPDB_BUSY;

  rc = poolCreateConnection(pPool, &db);
  if( rc ) return rc;

  aNew = (PoolSlot*)capdb_realloc(pPool->aSlot, sizeof(PoolSlot)*(pPool->nConn+1));
  if( aNew==0 ){
    capdb_close_v2(db);
    return CAPDB_NOMEM;
  }
  pPool->aSlot = aNew;
  iNew = pPool->nConn++;
  pPool->aSlot[iNew].db = db;
  pPool->aSlot[iNew].inUse = 0;
  pPool->aSlot[iNew].mode = (u8)CAPDB_POOL_READ;
  return CAPDB_OK;
}

static PoolSlot *poolFindSlot(capdb_pool *pPool, capdb *db){
  int i;
  for(i=0; i<pPool->nConn; i++){
    if( pPool->aSlot[i].db==db ) return &pPool->aSlot[i];
  }
  return 0;
}

static int poolCountIdle(capdb_pool *pPool){
  int i;
  int n = 0;
  for(i=0; i<pPool->nConn; i++){
    if( pPool->aSlot[i].inUse==0 ) n++;
  }
  return n;
}

static int poolCountActive(capdb_pool *pPool){
  int i;
  int n = 0;
  for(i=0; i<pPool->nConn; i++){
    if( pPool->aSlot[i].inUse ) n++;
  }
  return n;
}

static PoolSlot *poolFindIdleSlot(capdb_pool *pPool){
  int i;
  for(i=0; i<pPool->nConn; i++){
    if( pPool->aSlot[i].inUse==0 ) return &pPool->aSlot[i];
  }
  return 0;
}

static int poolReplaceConnection(capdb_pool *pPool, PoolSlot *pSlot){
  capdb *dbNew = 0;
  int rc;
  capdb_close_v2(pSlot->db);
  pSlot->db = 0;
  rc = poolCreateConnection(pPool, &dbNew);
  if( rc!=CAPDB_OK ) return rc;
  pSlot->db = dbNew;
  return CAPDB_OK;
}

static int poolResetConnection(capdb_pool *pPool, capdb *db){
  capdb_stmt *pStmt;
  int rc;
  if( capdb_get_autocommit(db) ) return CAPDB_OK;
  while( (pStmt = capdb_next_stmt(db, 0))!=0 ){
    capdb_finalize(pStmt);
  }
  rc = capdb_exec(db, "ROLLBACK", 0, 0, 0);
  if( rc!=CAPDB_OK ){
    PoolSlot *pSlot = poolFindSlot(pPool, db);
    if( pSlot ) return poolReplaceConnection(pPool, pSlot);
    return rc;
  }
  return CAPDB_OK;
}

static int poolCanAcquireWrite(capdb_pool *pPool){
  if( (pPool->cfg.flags & CAPDB_POOL_SERIALIZE_WRITES)==0 ) return 1;
  return pPool->nWriteActive==0;
}

static int poolTryAcquire(
  capdb_pool *pPool,
  capdb_pool_mode mode,
  capdb **ppDb
){
  PoolSlot *pSlot;
  int rc;

  if( pPool->shutdown ) return CAPDB_MISUSE;
  if( mode==CAPDB_POOL_WRITE && !poolCanAcquireWrite(pPool) ){
    return CAPDB_BUSY;
  }

  pSlot = poolFindIdleSlot(pPool);
  if( pSlot==0 ){
    rc = poolGrow(pPool);
    if( rc ) return rc;
    pSlot = poolFindIdleSlot(pPool);
    if( pSlot==0 ) return CAPDB_NOMEM;
  }

  pSlot->inUse = 1;
  pSlot->mode = (u8)mode;
  if( mode==CAPDB_POOL_WRITE ){
    pPool->nWriteActive++;
    if( (pPool->cfg.flags & CAPDB_POOL_DEFER_WRITE_BEGIN)==0 ){
      rc = capdb_exec(pSlot->db, "BEGIN IMMEDIATE", 0, 0, 0);
      if( rc!=CAPDB_OK ){
        pSlot->inUse = 0;
        pSlot->mode = (u8)CAPDB_POOL_READ;
        pPool->nWriteActive--;
        return rc;
      }
    }else{
      rc = CAPDB_OK;
    }
  }

  *ppDb = pSlot->db;
  return CAPDB_OK;
}

int capdb_pool_open(
  const char *zFilename,
  const capdb_pool_config *pCfgIn,
  capdb_pool **ppPool
){
  capdb_pool *pPool;
  capdb_pool_config cfg;
  int rc;
  int i;

  if( ppPool==0 || zFilename==0 ) return CAPDB_MISUSE;
  *ppPool = 0;

  rc = capdb_initialize();
  if( rc ) return rc;

  memset(&cfg, 0, sizeof(cfg));
  if( pCfgIn ){
    cfg = *pCfgIn;
  }
  poolApplyDefaults(&cfg);

  pPool = (capdb_pool*)capdb_malloc64(sizeof(*pPool));
  if( pPool==0 ) return CAPDB_NOMEM;
  memset(pPool, 0, sizeof(*pPool));
  pPool->cfg = cfg;
  pPool->zFilename = capdb_mprintf("%s", zFilename);
  if( pPool->zFilename==0 ){
    capdb_free(pPool);
    return CAPDB_NOMEM;
  }

  pPool->mutex = capdb_mutex_alloc(CAPDB_MUTEX_FAST);
  if( pPool->mutex==0 ){
    capdb_free(pPool->zFilename);
    capdb_free(pPool);
    return CAPDB_NOMEM;
  }

  for(i=0; i<pPool->cfg.minSize; i++){
    rc = poolGrow(pPool);
    if( rc ){
      capdb_pool_close(pPool);
      return rc;
    }
  }

  *ppPool = pPool;
  return CAPDB_OK;
}

int capdb_pool_close(capdb_pool *pPool){
  int i;
  if( pPool==0 ) return CAPDB_MISUSE;

  capdb_mutex_enter(pPool->mutex);
  pPool->shutdown = 1;
  if( poolCountActive(pPool)>0 ){
    capdb_mutex_leave(pPool->mutex);
    return CAPDB_BUSY;
  }

  for(i=0; i<pPool->nConn; i++){
    capdb_close_v2(pPool->aSlot[i].db);
  }
  capdb_free(pPool->aSlot);
  capdb_free(pPool->zFilename);
  capdb_mutex_leave(pPool->mutex);
  capdb_mutex_free(pPool->mutex);
  capdb_free(pPool);
  return CAPDB_OK;
}

int capdb_pool_acquire(
  capdb_pool *pPool,
  capdb_pool_mode mode,
  int waitMs,
  capdb **ppDb
){
  int rc;
  int waited = 0;

  if( pPool==0 || ppDb==0 ) return CAPDB_MISUSE;
  if( mode!=CAPDB_POOL_READ && mode!=CAPDB_POOL_WRITE ) return CAPDB_MISUSE;
  *ppDb = 0;

  capdb_mutex_enter(pPool->mutex);
  for(;;){
    rc = poolTryAcquire(pPool, mode, ppDb);
    if( rc!=CAPDB_BUSY ) break;
    if( waitMs==0 ) break;
    if( waitMs>0 && waited>=waitMs ) break;
    capdb_mutex_leave(pPool->mutex);
    capdb_sleep(POOL_WAIT_SLICE_MS);
    waited += POOL_WAIT_SLICE_MS;
    capdb_mutex_enter(pPool->mutex);
    if( pPool->shutdown ){
      rc = CAPDB_MISUSE;
      break;
    }
  }
  capdb_mutex_leave(pPool->mutex);
  return rc;
}

int capdb_pool_release(capdb_pool *pPool, capdb *db){
  PoolSlot *pSlot;
  int rc = CAPDB_OK;

  if( pPool==0 || db==0 ) return CAPDB_MISUSE;

  capdb_mutex_enter(pPool->mutex);
  pSlot = poolFindSlot(pPool, db);
  if( pSlot==0 || pSlot->inUse==0 ){
    capdb_mutex_leave(pPool->mutex);
    return CAPDB_MISUSE;
  }

  if( pPool->cfg.flags & CAPDB_POOL_RESET_ON_RELEASE ){
    rc = poolResetConnection(pPool, db);
    if( rc!=CAPDB_OK ){
      capdb_mutex_leave(pPool->mutex);
      return rc;
    }
  }else if( capdb_next_stmt(db, 0)!=0 ){
    capdb_mutex_leave(pPool->mutex);
    return CAPDB_MISUSE;
  }

  if( pSlot->mode==CAPDB_POOL_WRITE ){
    if( pPool->nWriteActive>0 ) pPool->nWriteActive--;
  }
  pSlot->inUse = 0;
  pSlot->mode = (u8)CAPDB_POOL_READ;
  capdb_mutex_leave(pPool->mutex);
  return rc;
}

int capdb_pool_stats(
  capdb_pool *pPool,
  int *pnIdle,
  int *pnActive,
  int *pnTotal
){
  if( pPool==0 ) return CAPDB_MISUSE;
  capdb_mutex_enter(pPool->mutex);
  if( pnIdle ) *pnIdle = poolCountIdle(pPool);
  if( pnActive ) *pnActive = poolCountActive(pPool);
  if( pnTotal ) *pnTotal = pPool->nConn;
  capdb_mutex_leave(pPool->mutex);
  return CAPDB_OK;
}

#endif /* CAPDB_ENABLE_POOL */
