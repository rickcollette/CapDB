
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_proto.h"
#include <stdlib.h>
#include <string.h>

static int bufGrow(capdb_buf *p, int nAdd){
  int nNeed;
  /* Guard against signed-int overflow in the size arithmetic below. */
  if( nAdd<0 || p->n > 0x7fffffff - nAdd ) return -1;
  nNeed = p->n + nAdd;
  if( nNeed <= p->nAlloc ) return 0;
  {
    int nNew = p->nAlloc ? p->nAlloc : 256;
    while( nNew < nNeed ){
      if( nNew > 0x7fffffff/2 ){ nNew = nNeed; break; }
      nNew *= 2;
    }
    {
      unsigned char *aNew = (unsigned char*)realloc(p->a, (size_t)nNew);
      if( aNew==0 ) return -1;
      p->a = aNew;
      p->nAlloc = nNew;
    }
  }
  return 0;
}

void capdb_buf_init(capdb_buf *p){
  memset(p, 0, sizeof(*p));
}

void capdb_buf_clear(capdb_buf *p){
  free(p->a);
  p->a = 0;
  p->n = 0;
  p->nAlloc = 0;
}

int capdb_buf_append(capdb_buf *p, const void *z, int n){
  if( n<0 ) return -1;
  if( bufGrow(p, n) ) return -1;
  memcpy(p->a + p->n, z, n);
  p->n += n;
  return 0;
}

int capdb_buf_append_u8(capdb_buf *p, unsigned char v){
  return capdb_buf_append(p, &v, 1);
}

int capdb_buf_append_u32(capdb_buf *p, unsigned int v){
  unsigned char b[4];
  b[0] = (unsigned char)((v>>24)&0xff);
  b[1] = (unsigned char)((v>>16)&0xff);
  b[2] = (unsigned char)((v>>8)&0xff);
  b[3] = (unsigned char)(v&0xff);
  return capdb_buf_append(p, b, 4);
}

int capdb_buf_append_i32(capdb_buf *p, int v){
  return capdb_buf_append_u32(p, (unsigned int)v);
}

int capdb_buf_append_i64(capdb_buf *p, long long v){
  unsigned char b[8];
  int i;
  for(i=7; i>=0; i--){
    b[i] = (unsigned char)(v & 0xff);
    v >>= 8;
  }
  return capdb_buf_append(p, b, 8);
}

int capdb_buf_append_double(capdb_buf *p, double r){
  unsigned char b[8];
  unsigned long long u;
  int i;
  /* Serialize the IEEE-754 bit pattern big-endian so REAL values are portable
  ** across machines of differing endianness (matches the integer encoders). */
  memcpy(&u, &r, 8);
  for(i=7; i>=0; i--){ b[i] = (unsigned char)(u & 0xff); u >>= 8; }
  return capdb_buf_append(p, b, 8);
}

int capdb_buf_append_str(capdb_buf *p, const char *z){
  int n = z ? (int)strlen(z) : 0;
  if( capdb_buf_append_u32(p, (unsigned)n) ) return -1;
  if( n>0 && capdb_buf_append(p, z, n) ) return -1;
  return 0;
}

void capdb_reader_init(capdb_reader *r, const unsigned char *a, int n){
  r->a = a;
  r->n = n;
  r->i = 0;
}

int capdb_reader_bytes(capdb_reader *r, int n){
  /* Reject negative n (e.g. an oversized u32 length cast to int) and compute the
  ** bound by subtraction so a large n cannot overflow r->i + n. n must fit in
  ** the unread remainder of the buffer. */
  return n >= 0 && n <= r->n - r->i;
}

int capdb_reader_consume(capdb_reader *r, int n){
  if( !capdb_reader_bytes(r, n) ) return -1;
  r->i += n;
  return 0;
}

int capdb_reader_u8(capdb_reader *r, unsigned char *p){
  if( !capdb_reader_bytes(r, 1) ) return -1;
  *p = r->a[r->i++];
  return 0;
}

int capdb_reader_u32(capdb_reader *r, unsigned int *p){
  unsigned int v = 0;
  if( !capdb_reader_bytes(r, 4) ) return -1;
  v = (unsigned int)r->a[r->i++] << 24;
  v |= (unsigned int)r->a[r->i++] << 16;
  v |= (unsigned int)r->a[r->i++] << 8;
  v |= (unsigned int)r->a[r->i++];
  *p = v;
  return 0;
}

int capdb_reader_i32(capdb_reader *r, int *p){
  unsigned int u;
  if( capdb_reader_u32(r, &u) ) return -1;
  *p = (int)u;
  return 0;
}

int capdb_reader_i64(capdb_reader *r, long long *p){
  long long v = 0;
  int i;
  if( !capdb_reader_bytes(r, 8) ) return -1;
  for(i=0; i<8; i++){
    v = (v<<8) | r->a[r->i++];
  }
  *p = v;
  return 0;
}

int capdb_reader_double(capdb_reader *r, double *p){
  unsigned long long u = 0;
  double d;
  int i;
  if( !capdb_reader_bytes(r, 8) ) return -1;
  for(i=0; i<8; i++){ u = (u<<8) | r->a[r->i++]; }
  memcpy(&d, &u, 8);
  *p = d;
  return 0;
}

int capdb_reader_str(capdb_reader *r, char **pz){
  unsigned int n;
  char *z;
  if( capdb_reader_u32(r, &n) ) return -1;
  if( !capdb_reader_bytes(r, (int)n) ) return -1;
  z = (char*)malloc((size_t)n + 1);
  if( z==0 ) return -1;
  if( n>0 ) memcpy(z, r->a + r->i, n);
  z[n] = 0;
  r->i += (int)n;
  *pz = z;
  return 0;
}

int capdb_reader_blob(capdb_reader *r, unsigned char **pa, int *pn){
  unsigned int n;
  unsigned char *a;
  if( capdb_reader_u32(r, &n) ) return -1;
  if( !capdb_reader_bytes(r, (int)n) ) return -1;
  a = (unsigned char*)malloc(n ? n : 1);
  if( a==0 ) return -1;
  if( n>0 ) memcpy(a, r->a + r->i, n);
  r->i += (int)n;
  *pa = a;
  *pn = (int)n;
  return 0;
}

int capdb_frame_encode(unsigned char type, capdb_buf *payload,
                         capdb_buf *out){
  unsigned int n = (unsigned int)(payload ? payload->n : 0) + 1;
  capdb_buf_init(out);
  if( capdb_buf_append_u32(out, n) ) return -1;
  if( capdb_buf_append_u8(out, type) ) return -1;
  if( payload && payload->n>0 ){
    if( capdb_buf_append(out, payload->a, payload->n) ) return -1;
  }
  return 0;
}

int capdb_frame_decode(const unsigned char *a, int n,
                         unsigned char *pType, capdb_reader *payload){
  unsigned int len;
  capdb_reader r;
  if( n<5 ) return -1;
  capdb_reader_init(&r, a, n);
  if( capdb_reader_u32(&r, &len) ) return -1;
  if( len<1 || len > (unsigned)CAPDB_MAX_FRAME_SIZE ) return -1;
  if( (int)(4 + len) > n ) return -1;
  if( capdb_reader_u8(&r, pType) ) return -1;
  capdb_reader_init(payload, a + r.i, (int)(4 + len - r.i));
  return (int)(4 + len);
}

#endif /* CAPDB_ENABLE_NETWORK */
