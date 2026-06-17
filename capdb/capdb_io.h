
#if !defined(__CAPDB_IO_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_IO_H_ 1

#include "tls/capdb_tls.h"
#include "proto/capdb_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

int capdb_stream_send_frame(capdb_stream *s, unsigned char type,
                              capdb_buf *payload);
int capdb_stream_recv_frame(capdb_stream *s, unsigned char *pType,
                              capdb_buf *payload);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_IO_H_ */
