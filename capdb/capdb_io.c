
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_io.h"
#include <stdlib.h>
#include <string.h>

int capdb_stream_send_frame(capdb_stream *s, unsigned char type,
                              capdb_buf *payload){
  capdb_buf frame;
  int n, rc;
  if( capdb_frame_encode(type, payload, &frame) ) return -1;
  n = frame.n;
  rc = capdb_stream_write(s, frame.a, n);
  capdb_buf_clear(&frame);
  return rc==n ? 0 : -1;
}

int capdb_stream_recv_frame(capdb_stream *s, unsigned char *pType,
                              capdb_buf *payload){
  unsigned char hdr[4];
  unsigned int len;
  unsigned char *a = 0;
  int nFrame;
  capdb_reader r;

  capdb_buf_init(payload);
  if( capdb_stream_read(s, hdr, 4)!=4 ) return -1;
  capdb_reader_init(&r, hdr, 4);
  if( capdb_reader_u32(&r, &len) ) return -1;
  if( len<1 || len>(unsigned)CAPDB_MAX_FRAME_SIZE ) return -1;
  nFrame = (int)(4 + len);
  a = (unsigned char*)malloc((size_t)nFrame);
  if( a==0 ) return -1;
  memcpy(a, hdr, 4);
  if( capdb_stream_read(s, a+4, (int)len)!=(int)len ){
    free(a);
    return -1;
  }
  {
    unsigned char t;
    capdb_reader pr;
    nFrame = capdb_frame_decode(a, nFrame, &t, &pr);
    if( nFrame<0 ){
      free(a);
      return -1;
    }
    *pType = t;
    if( pr.n>0 ){
      if( capdb_buf_append(payload, pr.a, pr.n) ){
        free(a);
        return -1;
      }
    }
    free(a);
  }
  return 0;
}

#endif /* CAPDB_ENABLE_NETWORK */
