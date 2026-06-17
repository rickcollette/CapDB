
#if defined(CAPDB_ENABLE_STORE)

#include "capdb_store.h"
#include "capdb.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAPDB_STORE_VFS_NAME "capdbstorevfs"

typedef struct StoreFile StoreFile;
struct StoreFile {
  capdb_file base;
  capdb_file *pReal;
  capdb_volume *pVol;
  int bWal;
};

typedef struct StoreVolEntry StoreVolEntry;
struct StoreVolEntry {
  char *zPath;
  capdb_volume *pVol;
  int nRef;
  StoreVolEntry *pNext;
};

typedef struct StoreVfs {
  capdb_vfs base;
  char *zVolumeRoot;
  capdb_vfs *pParent;
  pthread_mutex_t mutex;
  StoreVolEntry *pVolumes;
} StoreVfs;

static StoreVfs gStoreVfs;

static StoreFile *storeFileFromBase(capdb_file *p){
  return (StoreFile*)p;
}

static int storeSuffixKind(const char *zName, size_t *pBaseLen){
  static const char *aSuf[] = { "-wal", "-shm", "-journal", 0 };
  int i;
  size_t n = strlen(zName);
  for(i=0; aSuf[i]; i++){
    size_t ns = strlen(aSuf[i]);
    if( n>ns && strcmp(zName+n-ns, aSuf[i])==0 ){
      if( pBaseLen ) *pBaseLen = n-ns;
      return i+1;
    }
  }
  if( pBaseLen ) *pBaseLen = n;
  return 0;
}

static int storeResolvePath(const char *zName, char *zOut, int nOut,
                            int *pKind){
  size_t nBase = 0;
  int kind = storeSuffixKind(zName, &nBase);
  char zVol[1024];
  if( pKind ) *pKind = kind;
  if( nBase>=sizeof(zVol) ) return -1;
  memcpy(zVol, zName, nBase);
  zVol[nBase] = 0;
  if( kind==0 ){
    snprintf(zOut, (size_t)nOut, "%s/" CAPDB_STORE_MAIN_DB, zVol);
  }else if( kind==1 ){
    snprintf(zOut, (size_t)nOut, "%s/wal/main.db-wal", zVol);
  }else if( kind==2 ){
    snprintf(zOut, (size_t)nOut, "%s/wal/main.db-shm", zVol);
  }else{
    snprintf(zOut, (size_t)nOut, "%s/data/main.db-journal", zVol);
  }
  return 0;
}

static capdb_volume *storeVolumeAcquire(StoreVfs *pV, const char *zVolPath){
  StoreVolEntry *e;
  capdb_volume *pVol = 0;
  if( zVolPath==0 ) return 0;
  pthread_mutex_lock(&pV->mutex);
  for(e=pV->pVolumes; e; e=e->pNext){
    if( strcmp(e->zPath, zVolPath)==0 ){
      e->nRef++;
      pVol = e->pVol;
      pthread_mutex_unlock(&pV->mutex);
      return pVol;
    }
  }
  if( capdb_volume_open(zVolPath, CAPDB_VOLUME_OPEN_CREATE, &pVol)!=CAPDB_OK ){
    pthread_mutex_unlock(&pV->mutex);
    return 0;
  }
  e = (StoreVolEntry*)calloc(1, sizeof(*e));
  if( e==0 ){
    capdb_volume_close(pVol);
    pthread_mutex_unlock(&pV->mutex);
    return 0;
  }
  e->zPath = strdup(zVolPath);
  if( e->zPath==0 ){
    free(e);
    capdb_volume_close(pVol);
    pthread_mutex_unlock(&pV->mutex);
    return 0;
  }
  e->pVol = pVol;
  e->nRef = 1;
  e->pNext = pV->pVolumes;
  pV->pVolumes = e;
  pthread_mutex_unlock(&pV->mutex);
  return pVol;
}

static void storeVolumeRelease(StoreVfs *pV, capdb_volume *pVol){
  StoreVolEntry *e, **pp;
  if( pV==0 || pVol==0 ) return;
  pthread_mutex_lock(&pV->mutex);
  for(pp=&pV->pVolumes; (e=*pp)!=0; pp=&e->pNext){
    if( e->pVol==pVol ){
      if( e->nRef>0 ) e->nRef--;
      if( e->nRef==0 ){
        *pp = e->pNext;
        capdb_volume_close(e->pVol);
        free(e->zPath);
        free(e);
      }
      break;
    }
  }
  pthread_mutex_unlock(&pV->mutex);
}

static void storeVolumeCacheShutdown(StoreVfs *pV){
  StoreVolEntry *e, *n;
  if( pV==0 ) return;
  pthread_mutex_lock(&pV->mutex);
  for(e=pV->pVolumes; e; e=n){
    n = e->pNext;
    capdb_volume_close(e->pVol);
    free(e->zPath);
    free(e);
  }
  pV->pVolumes = 0;
  pthread_mutex_unlock(&pV->mutex);
}

static int storeWrapClose(capdb_file *pFile){
  StoreFile *p = storeFileFromBase(pFile);
  int rc = CAPDB_OK;
  if( p->pReal && p->pReal->pMethods ){
    rc = p->pReal->pMethods->xClose(p->pReal);
  }
  if( p->pVol ) storeVolumeRelease(&gStoreVfs, p->pVol);
  free(p->pReal);
  p->pReal = 0;
  p->pVol = 0;
  p->base.pMethods = 0;
  return rc;
}

#define STORE_DELEGATE2(name) \
static int storeWrap##name(capdb_file *pFile, void *zBuf, int iAmt, \
                           capdb_int64 iOfst){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, zBuf, iAmt, iOfst); \
}
#define STORE_DELEGATE_TRUNC(name) \
static int storeWrap##name(capdb_file *pFile, capdb_int64 size){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, size); \
}
#define STORE_DELEGATE_SYNC(name) \
static int storeWrap##name(capdb_file *pFile, int flags){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, flags); \
}
#define STORE_DELEGATE_FSIZE(name) \
static int storeWrap##name(capdb_file *pFile, capdb_int64 *pSize){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, pSize); \
}
#define STORE_DELEGATE_LOCK(name) \
static int storeWrap##name(capdb_file *pFile, int eLock){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, eLock); \
}
#define STORE_DELEGATE_CRL(name) \
static int storeWrap##name(capdb_file *pFile, int *pResOut){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, pResOut); \
}
#define STORE_DELEGATE_FCTL(name) \
static int storeWrap##name(capdb_file *pFile, int op, void *pArg){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal, op, pArg); \
}
#define STORE_DELEGATE0(name) \
static int storeWrap##name(capdb_file *pFile){ \
  StoreFile *p = storeFileFromBase(pFile); \
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR; \
  return p->pReal->pMethods->x##name(p->pReal); \
}

STORE_DELEGATE2(Read)
STORE_DELEGATE_TRUNC(Truncate)
STORE_DELEGATE_SYNC(Sync)
STORE_DELEGATE_FSIZE(FileSize)
STORE_DELEGATE_LOCK(Lock)
STORE_DELEGATE_LOCK(Unlock)
STORE_DELEGATE_CRL(CheckReservedLock)
STORE_DELEGATE_FCTL(FileControl)
STORE_DELEGATE0(SectorSize)
STORE_DELEGATE0(DeviceCharacteristics)

static int storeWrapWrite(capdb_file *pFile, const void *zBuf, int iAmt,
                          capdb_int64 iOfst){
  StoreFile *p = storeFileFromBase(pFile);
  int rc;
  if( p->pReal==0 || p->pReal->pMethods==0 ) return CAPDB_IOERR;
  rc = p->pReal->pMethods->xWrite(p->pReal, zBuf, iAmt, iOfst);
  if( rc==CAPDB_OK && p->bWal && p->pVol && iAmt>0 ){
    unsigned long long lsn = 0;
    (void)capdb_volume_append_wal(p->pVol, zBuf, iAmt, (unsigned long long)iOfst, &lsn);
  }
  return rc;
}

static capdb_io_methods storeIoMethods = {
  3,
  storeWrapClose,
  storeWrapRead,
  storeWrapWrite,
  storeWrapTruncate,
  storeWrapSync,
  storeWrapFileSize,
  storeWrapLock,
  storeWrapUnlock,
  storeWrapCheckReservedLock,
  storeWrapFileControl,
  storeWrapSectorSize,
  storeWrapDeviceCharacteristics,
  0, 0, 0, 0
};

static int storeWrapOpen(capdb_vfs *pVfs, const char *zName, capdb_file *pFile,
                         int flags, int *pOutFlags){
  StoreVfs *pV = (StoreVfs*)pVfs;
  StoreFile *p = storeFileFromBase(pFile);
  char zReal[2048];
  char zVol[1024];
  size_t nBase = 0;
  int kind;
  int rc;
  (void)pV;
  memset(p, 0, sizeof(*p));
  p->base.pMethods = 0;
  if( zName==0 ) return CAPDB_IOERR;
  kind = storeSuffixKind(zName, &nBase);
  if( nBase>=sizeof(zVol) ) return CAPDB_CANTOPEN;
  memcpy(zVol, zName, nBase);
  zVol[nBase] = 0;
  if( storeResolvePath(zName, zReal, sizeof(zReal), 0) ) return CAPDB_CANTOPEN;
  if( kind==0 && (flags & CAPDB_OPEN_CREATE) ){
    struct stat st;
    if( stat(zReal, &st)==0 && st.st_size==0 ) unlink(zReal);
    if( capdb_volume_prepare(zVol, CAPDB_VOLUME_OPEN_CREATE)!=CAPDB_OK ){
      return CAPDB_CANTOPEN;
    }
  }
  p->bWal = (kind==1);
  p->pVol = 0;
  if( kind==1 ){
    p->pVol = storeVolumeAcquire(pV, zVol);
    if( p->pVol==0 ) return CAPDB_CANTOPEN;
  }
  p->pReal = (capdb_file*)calloc(1, pV->pParent->szOsFile);
  if( p->pReal==0 ){
    if( p->pVol ) storeVolumeRelease(pV, p->pVol);
    return CAPDB_NOMEM;
  }
  rc = pV->pParent->xOpen(pV->pParent, zReal, p->pReal, flags, pOutFlags);
  if( rc!=CAPDB_OK ){
    free(p->pReal);
    if( p->pVol ) storeVolumeRelease(pV, p->pVol);
    p->pReal = 0;
    p->pVol = 0;
    return rc;
  }
  p->base.pMethods = &storeIoMethods;
  return CAPDB_OK;
}

static int storeWrapDelete(capdb_vfs *pVfs, const char *zName, int syncDir){
  StoreVfs *pV = (StoreVfs*)pVfs;
  char zReal[2048];
  if( storeResolvePath(zName, zReal, sizeof(zReal), 0) ) return CAPDB_IOERR_DELETE;
  return pV->pParent->xDelete(pV->pParent, zReal, syncDir);
}

static int storeWrapAccess(capdb_vfs *pVfs, const char *zName, int flags,
                           int *pResOut){
  StoreVfs *pV = (StoreVfs*)pVfs;
  char zReal[2048];
  if( storeResolvePath(zName, zReal, sizeof(zReal), 0) ) return CAPDB_IOERR;
  return pV->pParent->xAccess(pV->pParent, zReal, flags, pResOut);
}

static int storeWrapFullPathname(capdb_vfs *pVfs, const char *zName, int nOut,
                                 char *zOut){
  (void)pVfs;
  capdb_snprintf(nOut, zOut, "%s", zName);
  return CAPDB_OK;
}

const char *capdb_store_vfs_name(void){
  return CAPDB_STORE_VFS_NAME;
}

int capdb_store_vfs_register(const char *zVolumeRoot, int makeDefault){
  capdb_vfs *pParent = capdb_vfs_find(0);
  if( gStoreVfs.base.szOsFile==0 ){
    pthread_mutex_init(&gStoreVfs.mutex, 0);
    gStoreVfs.base.iVersion = 2;
    gStoreVfs.base.szOsFile = sizeof(StoreFile);
    gStoreVfs.base.mxPathname = 512;
    gStoreVfs.base.zName = CAPDB_STORE_VFS_NAME;
    gStoreVfs.base.xOpen = storeWrapOpen;
    gStoreVfs.base.xDelete = storeWrapDelete;
    gStoreVfs.base.xAccess = storeWrapAccess;
    gStoreVfs.base.xFullPathname = storeWrapFullPathname;
    gStoreVfs.base.xDlOpen = pParent ? pParent->xDlOpen : 0;
    gStoreVfs.base.xDlError = pParent ? pParent->xDlError : 0;
    gStoreVfs.base.xDlSym = pParent ? pParent->xDlSym : 0;
    gStoreVfs.base.xDlClose = pParent ? pParent->xDlClose : 0;
    gStoreVfs.base.xRandomness = pParent ? pParent->xRandomness : 0;
    gStoreVfs.base.xSleep = pParent ? pParent->xSleep : 0;
    gStoreVfs.base.xCurrentTime = pParent ? pParent->xCurrentTime : 0;
    gStoreVfs.pParent = pParent;
  }
  free(gStoreVfs.zVolumeRoot);
  gStoreVfs.zVolumeRoot = zVolumeRoot ? strdup(zVolumeRoot) : 0;
  capdb_store_init();
  return capdb_vfs_register(&gStoreVfs.base, makeDefault);
}

void capdb_store_vfs_shutdown(void){
  storeVolumeCacheShutdown(&gStoreVfs);
}

#endif /* CAPDB_ENABLE_STORE */
