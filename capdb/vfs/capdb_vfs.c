
#if defined(CAPDB_ENABLE_NETWORK)

#include "../client/capdb_client.h"
#include "capdb.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CAPDB_VFS_NAME "capdbvfs"

typedef struct CapdbRemoteFile CapdbRemoteFile;
struct CapdbRemoteFile {
  capdb_file base;
  capdb_conn *pConn;
  int vfsId;
  char *zPath;
};

typedef struct CapdbRemoteVfs {
  capdb_vfs base;
  char *zDefaultUri;
  capdb_conn *pConn;
  int nOpenFiles;
  pthread_mutex_t mutex;
  int bMutexInit;
} CapdbRemoteVfs;

static CapdbRemoteVfs gVfs;

static void vfsConnLock(void){
  if( gVfs.bMutexInit ) pthread_mutex_lock(&gVfs.mutex);
}

static void vfsConnUnlock(void){
  if( gVfs.bMutexInit ) pthread_mutex_unlock(&gVfs.mutex);
}

static int mFileClose(capdb_file *pFile){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  if( p->vfsId>=0 && p->pConn ){
    vfsConnLock();
    capdb_net_vfs_close(p->pConn, p->vfsId);
    vfsConnUnlock();
    p->vfsId = -1;
  }
  if( p->pConn ){
    vfsConnLock();
    if( --gVfs.nOpenFiles<=0 ){
      capdb_net_close(gVfs.pConn);
      gVfs.pConn = 0;
      gVfs.nOpenFiles = 0;
    }
    vfsConnUnlock();
    p->pConn = 0;
  }
  free(p->zPath);
  return CAPDB_OK;
}

static int mFileRead(capdb_file *pFile, void *zBuf, int iAmt, capdb_int64 iOfst){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  int rc;
  vfsConnLock();
  rc = capdb_net_vfs_read(p->pConn, p->vfsId, iOfst, zBuf, iAmt);
  vfsConnUnlock();
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_READ;
  return CAPDB_OK;
}

static int mFileWrite(capdb_file *pFile, const void *zBuf, int iAmt,
                      capdb_int64 iOfst){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  int rc;
  vfsConnLock();
  rc = capdb_net_vfs_write(p->pConn, p->vfsId, iOfst, zBuf, iAmt);
  vfsConnUnlock();
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_WRITE;
  return CAPDB_OK;
}

static int mFileTruncate(capdb_file *pFile, capdb_int64 size){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  int rc;
  vfsConnLock();
  rc = capdb_net_vfs_truncate(p->pConn, p->vfsId, size);
  vfsConnUnlock();
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_TRUNCATE;
  return CAPDB_OK;
}

static int mFileSync(capdb_file *pFile, int flags){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  int rc;
  vfsConnLock();
  rc = capdb_net_vfs_sync(p->pConn, p->vfsId, flags);
  vfsConnUnlock();
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_FSYNC;
  return CAPDB_OK;
}

static int mFileSize(capdb_file *pFile, capdb_int64 *pSize){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  long long sz = 0;
  int rc;
  vfsConnLock();
  rc = capdb_net_vfs_size(p->pConn, p->vfsId, &sz);
  vfsConnUnlock();
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_FSTAT;
  *pSize = sz;
  return CAPDB_OK;
}

static int mFileLock(capdb_file *pFile, int eLock){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  int rc;
  vfsConnLock();
  rc = capdb_net_vfs_lock(p->pConn, p->vfsId, eLock);
  vfsConnUnlock();
  if( rc==CAPDB_NET_BUSY ) return CAPDB_BUSY;
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_LOCK;
  return CAPDB_OK;
}

static int mFileUnlock(capdb_file *pFile, int eLock){
  return mFileLock(pFile, eLock);
}

static int mFileCheckReservedLock(capdb_file *pFile, int *pResOut){
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  int reserved = 0;
  int rc;
  if( p->pConn==0 || p->vfsId<0 ){
    *pResOut = 0;
    return CAPDB_OK;
  }
  vfsConnLock();
  rc = capdb_net_vfs_check_reserved(p->pConn, p->vfsId, &reserved);
  vfsConnUnlock();
  if( rc!=CAPDB_NET_OK ) return CAPDB_IOERR_LOCK;
  *pResOut = reserved;
  return CAPDB_OK;
}

static int mFileFileControl(capdb_file *pFile, int op, void *pArg){
  (void)pFile; (void)op; (void)pArg;
  return CAPDB_NOTFOUND;
}

static int mFileSectorSize(capdb_file *pFile){
  (void)pFile;
  return 512;
}

static int mFileDeviceCharacteristics(capdb_file *pFile){
  (void)pFile;
  return 0;
}

static int mVfsOpen(capdb_vfs *pVfs, const char *zName, capdb_file *pFile,
                    int flags, int *pOutFlags){
  CapdbRemoteVfs *pV = (CapdbRemoteVfs*)pVfs;
  CapdbRemoteFile *p = (CapdbRemoteFile*)pFile;
  const char *zUri = 0;
  const char *zPath = zName;
  char *zDup = 0;
  int rc;
  (void)flags;

  memset(p, 0, sizeof(*p));
  p->base.pMethods = 0;
  p->vfsId = -1;

  if( zName==0 ){
    return CAPDB_IOERR;
  }
  if( strncmp(zName, "file:", 5)==0 ){
    zPath = zName + 5;
  }
  {
    const char *q = strstr(zName, "?capdb=");
    if( q ){
      zUri = q + 9;
      zDup = strdup(zName);
      if( zDup ){
        char *cut = strstr(zDup, "?capdb=");
        if( cut ) *cut = 0;
        zPath = zDup;
        if( strncmp(zPath, "file:", 5)==0 ) zPath += 5;
      }
    }
  }
  if( zUri==0 ) zUri = pV->zDefaultUri;
  if( zUri==0 ) return CAPDB_CANTOPEN;

  vfsConnLock();
  if( pV->pConn==0 ){
    rc = capdb_net_connect(zUri, &pV->pConn);
    if( rc!=CAPDB_OK ){
      vfsConnUnlock();
      free(zDup);
      return CAPDB_CANTOPEN;
    }
    pV->nOpenFiles = 0;
  }
  p->pConn = pV->pConn;
  pV->nOpenFiles++;
  vfsConnUnlock();
  p->zPath = strdup(zPath ? zPath : "remote.db");
  vfsConnLock();
  rc = capdb_net_vfs_open(p->pConn, p->zPath, &p->vfsId);
  vfsConnUnlock();
  free(zDup);
  if( rc!=CAPDB_OK ){
    vfsConnLock();
    if( --pV->nOpenFiles<=0 ){
      capdb_net_close(pV->pConn);
      pV->pConn = 0;
    }
    vfsConnUnlock();
    free(p->zPath);
    p->zPath = 0;
    p->pConn = 0;
    return CAPDB_CANTOPEN;
  }
  p->base.pMethods = 0;
  if( pOutFlags ) *pOutFlags = flags;
  return CAPDB_OK;
}

static int mVfsDelete(capdb_vfs *NotUsed, const char *zName, int syncDir){
  (void)NotUsed; (void)zName; (void)syncDir;
  return CAPDB_IOERR_DELETE;
}

static int mVfsAccess(capdb_vfs *NotUsed, const char *zName, int flags,
                      int *pResOut){
  (void)NotUsed; (void)zName; (void)flags;
  *pResOut = 0;
  return CAPDB_OK;
}

static int mVfsFullPathname(capdb_vfs *NotUsed, const char *zName, int nOut,
                              char *zOut){
  (void)NotUsed;
  capdb_snprintf(nOut, zOut, "%s", zName);
  return CAPDB_OK;
}

static void *mVfsDlOpen(capdb_vfs *NotUsed, const char *zFilename){
  (void)NotUsed; (void)zFilename;
  return 0;
}
static void mVfsDlError(capdb_vfs *NotUsed, int nByte, char *zErrMsg){
  (void)NotUsed;
  capdb_snprintf(nByte, zErrMsg, "capdbvfs: no dlopen");
}
static void (*mVfsDlSym(capdb_vfs *NotUsed, void *pHandle,
                         const char *zSymbol))(void){
  (void)NotUsed; (void)pHandle; (void)zSymbol;
  return 0;
}
static void mVfsDlClose(capdb_vfs *NotUsed, void *pHandle){
  (void)NotUsed; (void)pHandle;
}

static int mVfsRandomness(capdb_vfs *NotUsed, int nByte, char *zOut){
  (void)NotUsed;
  capdb_randomness(nByte, zOut);
  return CAPDB_OK;
}

static int mVfsSleep(capdb_vfs *NotUsed, int microseconds){
  (void)NotUsed;
  capdb_sleep(microseconds/1000);
  return microseconds;
}

static int mVfsCurrentTime(capdb_vfs *pVfs, double *pTimeOut){
  capdb_vfs *pDef = capdb_vfs_find(0);
  (void)pVfs;
  if( pDef && pDef->xCurrentTime ){
    return pDef->xCurrentTime(pDef, pTimeOut);
  }
  *pTimeOut = 0;
  return CAPDB_OK;
}

static capdb_io_methods mIoMethods = {
  3,
  mFileClose,
  mFileRead,
  mFileWrite,
  mFileTruncate,
  mFileSync,
  mFileSize,
  mFileLock,
  mFileUnlock,
  mFileCheckReservedLock,
  mFileFileControl,
  mFileSectorSize,
  mFileDeviceCharacteristics,
  0, 0, 0, 0, 0, 0
};

static int mVfsOpenFixed(capdb_vfs *pVfs, const char *zName, capdb_file *pFile,
                         int flags, int *pOutFlags){
  int rc = mVfsOpen(pVfs, zName, pFile, flags, pOutFlags);
  if( rc==CAPDB_OK ){
    ((CapdbRemoteFile*)pFile)->base.pMethods = &mIoMethods;
  }
  return rc;
}

int capdb_net_vfs_register(const char *zDefaultUri, int makeDefault){
  capdb_vfs *pParent = capdb_vfs_find(0);
  if( gVfs.base.szOsFile==0 ){
    pthread_mutex_init(&gVfs.mutex, 0);
    gVfs.bMutexInit = 1;
    gVfs.base.iVersion = 2;
    gVfs.base.szOsFile = sizeof(CapdbRemoteFile);
    gVfs.base.mxPathname = 512;
    gVfs.base.zName = CAPDB_VFS_NAME;
    gVfs.base.xOpen = mVfsOpenFixed;
    gVfs.base.xDelete = mVfsDelete;
    gVfs.base.xAccess = mVfsAccess;
    gVfs.base.xFullPathname = mVfsFullPathname;
    gVfs.base.xDlOpen = mVfsDlOpen;
    gVfs.base.xDlError = mVfsDlError;
    gVfs.base.xDlSym = mVfsDlSym;
    gVfs.base.xDlClose = mVfsDlClose;
    gVfs.base.xRandomness = mVfsRandomness;
    gVfs.base.xSleep = mVfsSleep;
    gVfs.base.xCurrentTime = mVfsCurrentTime;
    if( pParent ){
      gVfs.base.pAppData = pParent->pAppData;
    }
  }
  free(gVfs.zDefaultUri);
  gVfs.zDefaultUri = zDefaultUri ? strdup(zDefaultUri) : 0;
  return capdb_vfs_register(&gVfs.base, makeDefault);
}

#endif /* CAPDB_ENABLE_NETWORK */
