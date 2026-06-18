
#if defined(CAPDB_ENABLE_STORE)

#if defined(CAPDB_ENABLE_REPLICATION)
#include "../replication/capdb_rep.h"
#endif
#include "capdb_store.h"
#include "capdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

struct capdb_volume {
  char *zPath;
  char *zMainDb;
  char *zWalDir;
  int pageSize;
  int generation;
  unsigned long long lsn;
  unsigned long long appliedLsn;
  int fdMain;
  pthread_mutex_t mutex;
};

#if defined(CAPDB_ENABLE_REPLICATION)
static struct capdb_rep_sender *gStoreRepSender = 0;

void capdb_store_set_rep_sender(struct capdb_rep_sender *p){
  gStoreRepSender = p;
}

struct capdb_rep_sender *capdb_store_rep_sender(void){
  return gStoreRepSender;
}
#endif

static int storeVolIdHasDotDot(const char *zRel){
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

int capdb_store_vol_id_valid(const char *zVolId){
  const char *p;
  size_t n;
  if( zVolId==0 || zVolId[0]==0 ) return 0;
  n = strlen(zVolId);
  if( n>=CAPDB_STORE_VOL_ID_MAX ) return 0;
  if( storeVolIdHasDotDot(zVolId) ) return 0;
  for( p=zVolId; *p; p++ ){
    if( *p=='/' || *p=='\\' ) return 0;
  }
  return 1;
}

static int storeEnsureMainFd(capdb_volume *p, int bRdonly){
  int flags;
  if( p==0 ) return CAPDB_MISUSE;
  if( p->fdMain>=0 ) return CAPDB_OK;
  flags = bRdonly ? O_RDONLY : (O_RDWR|O_CREAT);
  p->fdMain = open(p->zMainDb, flags, 0644);
  if( p->fdMain<0 ) return CAPDB_CANTOPEN;
  return CAPDB_OK;
}

static int storeMkdirP(const char *zPath){
  char buf[1024];
  size_t i, n;
  if( zPath==0 || zPath[0]==0 ) return -1;
  n = strlen(zPath);
  if( n>=sizeof(buf) ) return -1;
  memcpy(buf, zPath, n+1);
  for(i=1; i<n; i++){
    if( buf[i]=='/' ){
      buf[i] = 0;
      if( mkdir(buf, 0755)!=0 && errno!=EEXIST ) return -1;
      buf[i] = '/';
    }
  }
  if( mkdir(buf, 0755)!=0 && errno!=EEXIST ) return -1;
  return 0;
}

static int storeWriteFile(const char *zPath, const char *zContent){
  FILE *f = fopen(zPath, "w");
  if( f==0 ) return -1;
  if( zContent ) fputs(zContent, f);
  fclose(f);
  return 0;
}

static unsigned storeCrc32(const void *pData, int n){
  const unsigned char *p = (const unsigned char*)pData;
  unsigned crc = 0xffffffffu;
  int i, j;
  for(i=0; i<n; i++){
    crc ^= p[i];
    for(j=0; j<8; j++){
      unsigned m = (unsigned)(-(int)(crc & 1u));
      crc = (crc >> 1) ^ (0xedb88320u & m);
    }
  }
  return ~crc;
}

unsigned capdb_store_crc32(const void *pData, int n){
  return storeCrc32(pData, n);
}

static void storeVolumeBasename(capdb_volume *p, char *zOut, size_t nOut){
  const char *z = p->zPath;
  const char *slash;
  if( z==0 || nOut==0 ) return;
  slash = strrchr(z, '/');
  if( slash && slash[1] ) z = slash+1;
  snprintf(zOut, nOut, "%s", z);
}

static int storeJoinPath(char *zOut, int nOut, const char *a, const char *b){
  size_t na, nb;
  if( a==0 || b==0 || nOut<=0 ) return -1;
  na = strlen(a);
  nb = strlen(b);
  if( na+1+nb+1 > (size_t)nOut ) return -1;
  snprintf(zOut, (size_t)nOut, "%s/%s", a, b);
  return 0;
}

static int storeLoadState(capdb_volume *p){
  char zState[1024];
  FILE *f;
  if( storeJoinPath(zState, sizeof(zState), p->zPath, CAPDB_STORE_META_STATE) ) return -1;
  f = fopen(zState, "r");
  if( f==0 ) return 0;
  if( fscanf(f, "lsn=%llu\napplied_lsn=%llu\ngeneration=%d\npage_size=%d\n",
             &p->lsn, &p->appliedLsn, &p->generation, &p->pageSize)!=4 ){
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int storeSaveState(capdb_volume *p){
  char zState[1024];
  char zTmp[1040];
  FILE *f;
  int fd;
  if( storeJoinPath(zState, sizeof(zState), p->zPath, CAPDB_STORE_META_STATE) ) return -1;
  snprintf(zTmp, sizeof(zTmp), "%s.tmp", zState);
  f = fopen(zTmp, "w");
  if( f==0 ) return -1;
  fprintf(f, "lsn=%llu\napplied_lsn=%llu\ngeneration=%d\npage_size=%d\n",
          p->lsn, p->appliedLsn, p->generation, p->pageSize);
  fflush(f);
  fd = fileno(f);
  if( fd>=0 && fsync(fd)!=0 ){
    fclose(f);
    unlink(zTmp);
    return -1;
  }
  fclose(f);
  if( rename(zTmp, zState)!=0 ){
    unlink(zTmp);
    return -1;
  }
  return 0;
}

static int storeWriteManifest(capdb_volume *p){
  char zManifest[1024];
  char zBody[512];
  time_t now = time(0);
  snprintf(zBody, sizeof(zBody),
    "{\n"
    "  \"format_version\": %d,\n"
    "  \"page_size\": %d,\n"
    "  \"generation\": %d,\n"
    "  \"created\": %lld\n"
    "}\n",
    CAPDB_STORE_FORMAT_VERSION, p->pageSize, p->generation, (long long)now);
  if( storeJoinPath(zManifest, sizeof(zManifest), p->zPath, CAPDB_STORE_MANIFEST) ) return -1;
  return storeWriteFile(zManifest, zBody);
}

static int storeEnsureLayout(capdb_volume *p, int bCreate){
  char zSub[1024];
  struct stat st;
  if( storeJoinPath(zSub, sizeof(zSub), p->zPath, CAPDB_STORE_DIR_DATA) ) return -1;
  if( storeMkdirP(zSub) ) return -1;
  if( storeJoinPath(zSub, sizeof(zSub), p->zPath, CAPDB_STORE_DIR_WAL) ) return -1;
  if( storeMkdirP(zSub) ) return -1;
  if( storeJoinPath(zSub, sizeof(zSub), p->zPath, CAPDB_STORE_DIR_META) ) return -1;
  if( storeMkdirP(zSub) ) return -1;
  if( storeJoinPath(zSub, sizeof(zSub), p->zPath, CAPDB_STORE_DIR_PAGES) ) return -1;
  if( storeMkdirP(zSub) ) return -1;
  if( storeJoinPath(zSub, sizeof(zSub), p->zPath, CAPDB_STORE_DIR_SNAPSHOTS) ) return -1;
  if( storeMkdirP(zSub) ) return -1;
  if( stat(p->zMainDb, &st)!=0 ){
    if( !bCreate ) return CAPDB_CANTOPEN;
    /* Leave data/main.db absent until SQLite creates it via OPEN_CREATE. */
    if( storeWriteManifest(p) ) return CAPDB_IOERR;
    if( storeSaveState(p) ) return CAPDB_IOERR;
  }
  return CAPDB_OK;
}

int capdb_store_init(void){
  return CAPDB_OK;
}

int capdb_volume_prepare(const char *zPath, int flags){
  capdb_volume *p = 0;
  int rc;
  if( zPath==0 ) return CAPDB_MISUSE;
  rc = capdb_volume_open(zPath, flags & CAPDB_VOLUME_OPEN_CREATE, &p);
  if( rc!=CAPDB_OK ) return rc;
  capdb_volume_close(p);
  return CAPDB_OK;
}

void capdb_store_shutdown(void){
  capdb_store_vfs_shutdown();
}

int capdb_volume_open(const char *zPath, int flags, capdb_volume **pp){
  capdb_volume *p;
  if( zPath==0 || pp==0 ) return CAPDB_MISUSE;
  *pp = 0;
  p = (capdb_volume*)calloc(1, sizeof(*p));
  if( p==0 ) return CAPDB_NOMEM;
  p->zPath = strdup(zPath);
  if( p->zPath==0 ){ free(p); return CAPDB_NOMEM; }
  p->pageSize = CAPDB_STORE_PAGE_SIZE_DEFAULT;
  p->generation = 1;
  p->fdMain = -1;
  p->zMainDb = (char*)malloc(strlen(zPath)+64);
  p->zWalDir = (char*)malloc(strlen(zPath)+64);
  if( p->zMainDb==0 || p->zWalDir==0 ){
    free(p->zPath); free(p->zMainDb); free(p->zWalDir); free(p);
    return CAPDB_NOMEM;
  }
  storeJoinPath(p->zMainDb, (int)strlen(zPath)+64, zPath, CAPDB_STORE_MAIN_DB);
  storeJoinPath(p->zWalDir, (int)strlen(zPath)+64, zPath, CAPDB_STORE_DIR_WAL);
  pthread_mutex_init(&p->mutex, 0);
  if( storeLoadState(p) ){
    free(p->zPath); free(p->zMainDb); free(p->zWalDir); free(p);
    return CAPDB_CORRUPT;
  }
  {
    int rc = storeEnsureLayout(p, flags & CAPDB_VOLUME_OPEN_CREATE);
    if( rc!=CAPDB_OK ){
      capdb_volume_close(p);
      return rc;
    }
  }
  *pp = p;
  return CAPDB_OK;
}

void capdb_volume_close(capdb_volume *p){
  if( p==0 ) return;
  storeSaveState(p);
  if( p->fdMain>=0 ){
    close(p->fdMain);
    p->fdMain = -1;
  }
  pthread_mutex_destroy(&p->mutex);
  free(p->zPath);
  free(p->zMainDb);
  free(p->zWalDir);
  free(p);
}

int capdb_volume_read_page(capdb_volume *p, unsigned pgno, void *buf, int sz){
  off_t off;
  ssize_t n;
  int rc;
  if( p==0 || buf==0 || pgno==0 || sz<=0 ) return CAPDB_MISUSE;
  if( sz!=p->pageSize ) return CAPDB_RANGE;
  pthread_mutex_lock(&p->mutex);
  rc = storeEnsureMainFd(p, 1);
  if( rc!=CAPDB_OK ){
    pthread_mutex_unlock(&p->mutex);
    return rc;
  }
  off = (off_t)(pgno-1) * (off_t)p->pageSize;
  n = pread(p->fdMain, buf, (size_t)sz, off);
  pthread_mutex_unlock(&p->mutex);
  if( n!=sz ) return CAPDB_IOERR_READ;
  return CAPDB_OK;
}

int capdb_volume_write_page(capdb_volume *p, unsigned pgno, const void *buf, int sz){
  off_t off;
  ssize_t n;
  int rc;
  if( p==0 || buf==0 || pgno==0 || sz<=0 ) return CAPDB_MISUSE;
  if( sz!=p->pageSize ) return CAPDB_RANGE;
  pthread_mutex_lock(&p->mutex);
  rc = storeEnsureMainFd(p, 0);
  if( rc!=CAPDB_OK ){
    pthread_mutex_unlock(&p->mutex);
    return rc;
  }
  off = (off_t)(pgno-1) * (off_t)p->pageSize;
  n = pwrite(p->fdMain, buf, (size_t)sz, off);
  pthread_mutex_unlock(&p->mutex);
  if( n!=sz ) return CAPDB_IOERR_WRITE;
  return CAPDB_OK;
}

int capdb_volume_append_wal(capdb_volume *p, const void *walData, int n,
                            unsigned long long walOffset,
                            unsigned long long *pLsn){
  char zSeg[1024];
  CapdbStoreWalHdr hdr;
  int fd;
  ssize_t w;
  if( p==0 || walData==0 || n<=0 ) return CAPDB_MISUSE;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = CAPDB_STORE_WAL_MAGIC;
  hdr.format = CAPDB_STORE_FORMAT_VERSION;
  hdr.generation = (unsigned)p->generation;
  hdr.prev_lsn = p->lsn;
  hdr.wal_offset = walOffset;
  hdr.payload_len = (unsigned)n;
  hdr.crc32 = storeCrc32(walData, n);
  storeVolumeBasename(p, hdr.vol_id, sizeof(hdr.vol_id));
  pthread_mutex_lock(&p->mutex);
  p->lsn++;
  hdr.lsn = p->lsn;
  snprintf(zSeg, sizeof(zSeg), "%s/%08llu.wal", p->zWalDir,
           (unsigned long long)p->lsn);
  fd = open(zSeg, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if( fd<0 ){ pthread_mutex_unlock(&p->mutex); return CAPDB_IOERR; }
  w = write(fd, &hdr, sizeof(hdr));
  if( w==(ssize_t)sizeof(hdr) ){
    w = write(fd, walData, (size_t)n);
  }
  if( w==n ){
    /* Best-effort durability; do not fail the sqlite WAL write if fsync fails. */
    (void)fsync(fd);
  }
  close(fd);
  if( w!=n ){ pthread_mutex_unlock(&p->mutex); return CAPDB_IOERR; }
  storeSaveState(p);
  if( pLsn ) *pLsn = p->lsn;
  pthread_mutex_unlock(&p->mutex);
#if defined(CAPDB_ENABLE_REPLICATION)
  capdb_rep_primary_on_wal(p, &hdr, walData, n);
#endif
  return CAPDB_OK;
}

int capdb_volume_replicate_sql_main(capdb_volume *p,
                                    unsigned long long *pLsn){
  struct stat st;
  unsigned char *buf = 0;
  ssize_t nRead, nTotal;
  int fd, rc;
  if( p==0 ) return CAPDB_MISUSE;
  fd = open(p->zMainDb, O_RDONLY);
  if( fd<0 ) return CAPDB_IOERR;
  if( fstat(fd, &st)!=0 || st.st_size<=0 ){
    close(fd);
    return CAPDB_OK;
  }
  if( st.st_size > 64*1024*1024 ){
    close(fd);
    return CAPDB_RANGE;
  }
  buf = (unsigned char*)malloc((size_t)st.st_size);
  if( buf==0 ){ close(fd); return CAPDB_NOMEM; }
  nTotal = 0;
  while( nTotal < st.st_size ){
    nRead = read(fd, buf+nTotal, (size_t)st.st_size - (size_t)nTotal);
    if( nRead<=0 ){
      free(buf);
      close(fd);
      return CAPDB_IOERR;
    }
    nTotal += nRead;
  }
  close(fd);
  rc = capdb_volume_append_wal(p, buf, (int)nTotal,
                               CAPDB_STORE_WAL_OFFSET_MAIN_DB, pLsn);
  free(buf);
  return rc;
}

int capdb_volume_checkpoint(capdb_volume *p, unsigned long long *pSnapLsn){
  int rc;
  if( p==0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&p->mutex);
  rc = storeEnsureMainFd(p, 0);
  if( rc==CAPDB_OK && fsync(p->fdMain)!=0 ) rc = CAPDB_IOERR;
  if( rc==CAPDB_OK ){
    p->appliedLsn = p->lsn;
    storeSaveState(p);
    if( pSnapLsn ) *pSnapLsn = p->appliedLsn;
  }
  pthread_mutex_unlock(&p->mutex);
  return rc;
}

int capdb_volume_sync(capdb_volume *p, int flags){
  int rc;
  (void)flags;
  if( p==0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&p->mutex);
  rc = storeEnsureMainFd(p, 0);
  if( rc==CAPDB_OK && fsync(p->fdMain)!=0 ) rc = CAPDB_IOERR;
  pthread_mutex_unlock(&p->mutex);
  return rc;
}

unsigned long long capdb_volume_lsn(capdb_volume *p){
  return p ? p->lsn : 0;
}

unsigned long long capdb_volume_applied_lsn(capdb_volume *p){
  return p ? p->appliedLsn : 0;
}

int capdb_volume_generation(capdb_volume *p){
  return p ? p->generation : 0;
}

int capdb_volume_set_generation(capdb_volume *p, int gen){
  if( p==0 || gen<=0 ) return CAPDB_MISUSE;
  pthread_mutex_lock(&p->mutex);
  p->generation = gen;
  storeWriteManifest(p);
  storeSaveState(p);
  pthread_mutex_unlock(&p->mutex);
  return CAPDB_OK;
}

const char *capdb_volume_path(capdb_volume *p){
  return p ? p->zPath : 0;
}

const char *capdb_volume_main_db_path(capdb_volume *p){
  return p ? p->zMainDb : 0;
}

const char *capdb_volume_wal_dir(capdb_volume *p){
  return p ? p->zWalDir : 0;
}

int capdb_volume_export_db(capdb_volume *p, const char *zOutPath){
  char buf[8192];
  ssize_t n;
  int fdIn, fdOut;
  if( p==0 || zOutPath==0 ) return CAPDB_MISUSE;
  fdIn = open(p->zMainDb, O_RDONLY);
  if( fdIn<0 ) return CAPDB_IOERR;
  fdOut = open(zOutPath, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if( fdOut<0 ){ close(fdIn); return CAPDB_IOERR; }
  for(;;){
    n = read(fdIn, buf, sizeof(buf));
    if( n==0 ) break;
    if( n<0 || write(fdOut, buf, (size_t)n)!=n ){
      close(fdIn); close(fdOut);
      return CAPDB_IOERR;
    }
  }
  close(fdIn);
  if( fsync(fdOut)!=0 ){ close(fdOut); return CAPDB_IOERR; }
  close(fdOut);
  return CAPDB_OK;
}

int capdb_volume_snapshot(capdb_volume *p, unsigned long long lsn,
                          char *zOutDir, int nOutDir){
  char zSnap[1024];
  char zDest[1024 + 16];
  int n;
  if( p==0 || zOutDir==0 || nOutDir<=0 ) return CAPDB_MISUSE;
  n = snprintf(zSnap, sizeof(zSnap), "%s/" CAPDB_STORE_DIR_SNAPSHOTS "/snap_%llu",
               p->zPath, lsn);
  if( n<0 || (size_t)n>=sizeof(zSnap) ) return CAPDB_IOERR;
  if( storeMkdirP(zSnap) ) return CAPDB_IOERR;
  snprintf(zDest, sizeof(zDest), "%s/main.db", zSnap);
  return capdb_volume_export_db(p, zDest);
}

#endif /* CAPDB_ENABLE_STORE */
