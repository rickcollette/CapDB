/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code to implement a pseudo-random number
** generator (PRNG) for SQLite.
**
** Random numbers are used by some of the database backends in order
** to generate random integer keys for tables or random filenames.
*/
#include "capdbInt.h"


/* All threads share a single random number generator.
** This structure is the current state of the generator.
*/
static CAPDB_WSD struct capdbPrngType {
  u32 s[16];                 /* 64 bytes of chacha20 state */
  u8 out[64];                /* Output bytes */
  u8 n;                      /* Output bytes remaining */
} capdbPrng;


/* The RFC-7539 ChaCha20 block function
*/
#define ROTL(a,b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) ( \
    a += b, d ^= a, d = ROTL(d,16), \
    c += d, b ^= c, b = ROTL(b,12), \
    a += b, d ^= a, d = ROTL(d, 8), \
    c += d, b ^= c, b = ROTL(b, 7))
static void chacha_block(u32 *out, const u32 *in){
  int i;
  u32 x[16];
  memcpy(x, in, 64);
  for(i=0; i<10; i++){
    QR(x[0], x[4], x[ 8], x[12]);
    QR(x[1], x[5], x[ 9], x[13]);
    QR(x[2], x[6], x[10], x[14]);
    QR(x[3], x[7], x[11], x[15]);
    QR(x[0], x[5], x[10], x[15]);
    QR(x[1], x[6], x[11], x[12]);
    QR(x[2], x[7], x[ 8], x[13]);
    QR(x[3], x[4], x[ 9], x[14]);
  }
  for(i=0; i<16; i++) out[i] = x[i]+in[i];
}

/*
** Return N random bytes.
*/
void capdb_randomness(int N, void *pBuf){
  unsigned char *zBuf = pBuf;

  /* The "wsdPrng" macro will resolve to the pseudo-random number generator
  ** state vector.  If writable static data is unsupported on the target,
  ** we have to locate the state vector at run-time.  In the more common
  ** case where writable static data is supported, wsdPrng can refer directly
  ** to the "capdbPrng" state vector declared above.
  */
#ifdef CAPDB_OMIT_WSD
  struct capdbPrngType *p = &GLOBAL(struct capdbPrngType, capdbPrng);
# define wsdPrng p[0]
#else
# define wsdPrng capdbPrng
#endif

#if CAPDB_THREADSAFE
  capdb_mutex *mutex;
#endif

#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return;
#endif

#if CAPDB_THREADSAFE
  mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_PRNG);
#endif

  capdb_mutex_enter(mutex);
  if( N<=0 || pBuf==0 ){
    wsdPrng.s[0] = 0;
    capdb_mutex_leave(mutex);
    return;
  }

  /* Initialize the state of the random number generator once,
  ** the first time this routine is called.
  */
  if( wsdPrng.s[0]==0 ){
    capdb_vfs *pVfs = capdb_vfs_find(0);
    static const u32 chacha20_init[] = {
      0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
    };
    memcpy(&wsdPrng.s[0], chacha20_init, 16);
    if( NEVER(pVfs==0) ){
      memset(&wsdPrng.s[4], 0, 44);
    }else{
      capdbOsRandomness(pVfs, 44, (char*)&wsdPrng.s[4]);
    }
    wsdPrng.s[15] = wsdPrng.s[12];
    wsdPrng.s[12] = 0;
    wsdPrng.n = 0;
  }

  assert( N>0 );
  while( 1 /* exit by break */ ){
    if( N<=wsdPrng.n ){
      memcpy(zBuf, &wsdPrng.out[wsdPrng.n-N], N);
      wsdPrng.n -= N;
      break;
    }
    if( wsdPrng.n>0 ){
      memcpy(zBuf, wsdPrng.out, wsdPrng.n);
      N -= wsdPrng.n;
      zBuf += wsdPrng.n;
    }
    wsdPrng.s[12]++;
    chacha_block((u32*)wsdPrng.out, wsdPrng.s);
    wsdPrng.n = 64;
  }
  capdb_mutex_leave(mutex);
}

#ifndef CAPDB_UNTESTABLE
/*
** For testing purposes, we sometimes want to preserve the state of
** PRNG and restore the PRNG to its saved state at a later time, or
** to reset the PRNG to its initial state.  These routines accomplish
** those tasks.
**
** The capdb_test_control() interface calls these routines to
** control the PRNG.
*/
static CAPDB_WSD struct capdbPrngType capdbSavedPrng;
void capdbPrngSaveState(void){
  memcpy(
    &GLOBAL(struct capdbPrngType, capdbSavedPrng),
    &GLOBAL(struct capdbPrngType, capdbPrng),
    sizeof(capdbPrng)
  );
}
void capdbPrngRestoreState(void){
  memcpy(
    &GLOBAL(struct capdbPrngType, capdbPrng),
    &GLOBAL(struct capdbPrngType, capdbSavedPrng),
    sizeof(capdbPrng)
  );
}
#endif /* CAPDB_UNTESTABLE */
