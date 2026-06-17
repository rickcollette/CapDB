
#if defined(CAPDB_ENABLE_REPLICATION) && defined(CAPDB_ENABLE_NETWORK)

#include "capdb_rep_io.h"
#include "capdb_rep.h"
#include "../proto/capdb_proto.h"
#include "../store/capdb_store_format.h"
#include <string.h>

int capdb_rep_send(capdb_stream *s, unsigned char type, capdb_buf *payload){
  return capdb_stream_send_frame(s, type, payload);
}

int capdb_rep_recv(capdb_stream *s, unsigned char *pType, capdb_buf *payload){
  return capdb_stream_recv_frame(s, pType, payload);
}

int capdb_rep_handshake_client(capdb_stream *s, unsigned int protoVersion){
  capdb_buf pl;
  capdb_buf rx;
  unsigned char type;
  capdb_reader r;
  unsigned int ver;
  capdb_buf_init(&pl);
  capdb_buf_append_u32(&pl, protoVersion);
  if( capdb_rep_send(s, CAPDB_REP_HELLO, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  if( capdb_rep_recv(s, &type, &rx) ) return -1;
  if( type!=CAPDB_REP_HELLO_ACK ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  if( capdb_reader_u32(&r, &ver) || ver!=protoVersion ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  return 0;
}

int capdb_rep_handshake_server(capdb_stream *s, unsigned int protoVersion){
  capdb_buf rx, pl;
  unsigned char type;
  capdb_reader r;
  unsigned int ver;
  if( capdb_rep_recv(s, &type, &rx) ) return -1;
  if( type!=CAPDB_REP_HELLO ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  if( capdb_reader_u32(&r, &ver) ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  capdb_buf_init(&pl);
  capdb_buf_append_u32(&pl, protoVersion);
  if( capdb_rep_send(s, CAPDB_REP_HELLO_ACK, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  return 0;
}

int capdb_rep_auth(capdb_stream *s, const char *zToken){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zToken ? zToken : "");
  if( capdb_rep_send(s, CAPDB_REP_AUTH, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  if( capdb_rep_recv(s, &type, &rx) ) return -1;
  capdb_buf_clear(&rx);
  return type==CAPDB_REP_AUTH_OK ? 0 : -1;
}

int capdb_rep_auth_ok(capdb_stream *s){
  capdb_buf pl;
  capdb_buf_init(&pl);
  if( capdb_rep_send(s, CAPDB_REP_AUTH_OK, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  return 0;
}

int capdb_rep_send_wal_chunk(capdb_stream *s, const void *hdr, int nHdr,
                             const void *payload, int nPayload){
  capdb_buf pl;
  capdb_buf_init(&pl);
  if( hdr && nHdr>0 ) capdb_buf_append(&pl, hdr, nHdr);
  if( payload && nPayload>0 ) capdb_buf_append(&pl, payload, nPayload);
  if( capdb_rep_send(s, CAPDB_REP_WAL_CHUNK, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  return 0;
}

int capdb_rep_recv_wal_chunk(capdb_stream *s, capdb_buf *hdrOut,
                             capdb_buf *payloadOut){
  capdb_buf rx;
  unsigned char type;
  CapdbStoreWalHdr wh;
  if( capdb_rep_recv(s, &type, &rx) ) return -1;
  if( type!=CAPDB_REP_WAL_CHUNK ){
    capdb_buf_clear(&rx);
    return -1;
  }
  if( rx.n < (int)sizeof(wh) ){
    capdb_buf_clear(&rx);
    return -1;
  }
  memcpy(&wh, rx.a, sizeof(wh));
  capdb_buf_init(hdrOut);
  capdb_buf_append(hdrOut, rx.a, (int)sizeof(wh));
  capdb_buf_init(payloadOut);
  if( rx.n > (int)sizeof(wh) ){
    capdb_buf_append(payloadOut, rx.a+sizeof(wh), rx.n-(int)sizeof(wh));
  }
  capdb_buf_clear(&rx);
  return 0;
}

int capdb_rep_send_ack(capdb_stream *s, unsigned long long lsn){
  capdb_buf pl;
  capdb_buf_init(&pl);
  capdb_buf_append_i64(&pl, (long long)lsn);
  if( capdb_rep_send(s, CAPDB_REP_ACK, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  return 0;
}

int capdb_rep_recv_ack(capdb_stream *s, unsigned long long *pLsn){
  capdb_buf rx;
  unsigned char type;
  capdb_reader r;
  long long v = 0;
  if( capdb_rep_recv(s, &type, &rx) ) return -1;
  if( type!=CAPDB_REP_ACK ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  if( capdb_reader_i64(&r, &v) ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  if( pLsn ) *pLsn = (unsigned long long)v;
  return 0;
}

#endif /* CAPDB_ENABLE_REPLICATION && CAPDB_ENABLE_NETWORK */
