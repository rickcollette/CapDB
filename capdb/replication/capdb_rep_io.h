
#if !defined(__CAPDB_REP_IO_H_) && defined(CAPDB_ENABLE_REPLICATION)
#define __CAPDB_REP_IO_H_ 1

#include "../capdb_io.h"

#ifdef __cplusplus
extern "C" {
#endif

int capdb_rep_send(capdb_stream *s, unsigned char type, capdb_buf *payload);
int capdb_rep_recv(capdb_stream *s, unsigned char *pType, capdb_buf *payload);

int capdb_rep_handshake_client(capdb_stream *s, unsigned int protoVersion);
int capdb_rep_handshake_server(capdb_stream *s, unsigned int protoVersion);
int capdb_rep_auth(capdb_stream *s, const char *zToken);
int capdb_rep_auth_ok(capdb_stream *s);
int capdb_rep_send_wal_chunk(capdb_stream *s, const void *hdr, int nHdr,
                             const void *payload, int nPayload);
int capdb_rep_recv_wal_chunk(capdb_stream *s, capdb_buf *hdrOut,
                             capdb_buf *payloadOut);
int capdb_rep_send_ack(capdb_stream *s, unsigned long long lsn);
int capdb_rep_recv_ack(capdb_stream *s, unsigned long long *pLsn);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_REP_IO_H_ */
