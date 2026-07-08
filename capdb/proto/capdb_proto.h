
#if !defined(__CAPDB_PROTO_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_PROTO_H_ 1

#include "../capdb_network.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct capdb_buf capdb_buf;
struct capdb_buf {
  unsigned char *a;
  int n;
  int nAlloc;
};

typedef struct capdb_reader capdb_reader;
struct capdb_reader {
  const unsigned char *a;
  int n;
  int i;
};

/* Message types */
#define CAPDB_MSG_HELLO        1
#define CAPDB_MSG_HELLO_ACK    2
#define CAPDB_MSG_AUTH         3
#define CAPDB_MSG_AUTH_OK      4
#define CAPDB_MSG_AUTH_FAIL    5
#define CAPDB_MSG_OPEN         6
#define CAPDB_MSG_OPEN_OK      7
#define CAPDB_MSG_EXEC         8
#define CAPDB_MSG_EXEC_RESULT  9
#define CAPDB_MSG_ERROR       10
#define CAPDB_MSG_PREPARE     11
#define CAPDB_MSG_PREPARE_OK  12
#define CAPDB_MSG_STEP        13
#define CAPDB_MSG_STEP_ROW    14
#define CAPDB_MSG_STEP_DONE   15
#define CAPDB_MSG_FINALIZE    16
#define CAPDB_MSG_CLOSE       17
#define CAPDB_MSG_CLOSE_OK    18
#define CAPDB_MSG_PING        19
#define CAPDB_MSG_PONG        20

#define CAPDB_MSG_VFS_OPEN      21
#define CAPDB_MSG_VFS_OPEN_OK   22
#define CAPDB_MSG_VFS_CLOSE     23
#define CAPDB_MSG_VFS_READ      24
#define CAPDB_MSG_VFS_READ_OK   25
#define CAPDB_MSG_VFS_WRITE     26
#define CAPDB_MSG_VFS_TRUNCATE  27
#define CAPDB_MSG_VFS_SYNC      28
#define CAPDB_MSG_VFS_SIZE      29
#define CAPDB_MSG_VFS_SIZE_OK   30
#define CAPDB_MSG_VFS_LOCK      31
#define CAPDB_MSG_VFS_LOCK_OK   32
#define CAPDB_MSG_FINALIZE_OK   33
#define CAPDB_MSG_VFS_CHECK_RESERVED     34
#define CAPDB_MSG_VFS_CHECK_RESERVED_OK  35
#define CAPDB_MSG_VFS_DELETE    36

#define CAPDB_AUTH_TOKEN       1
#define CAPDB_AUTH_PASSWORD    2

#define CAPDB_VAL_NULL    0
#define CAPDB_VAL_INT     1
#define CAPDB_VAL_FLOAT   2
#define CAPDB_VAL_TEXT    3
#define CAPDB_VAL_BLOB    4

void capdb_buf_init(capdb_buf *p);
void capdb_buf_clear(capdb_buf *p);
int  capdb_buf_append(capdb_buf *p, const void *z, int n);
int  capdb_buf_append_u8(capdb_buf *p, unsigned char v);
int  capdb_buf_append_u32(capdb_buf *p, unsigned int v);
int  capdb_buf_append_i32(capdb_buf *p, int v);
int  capdb_buf_append_i64(capdb_buf *p, long long v);
int  capdb_buf_append_double(capdb_buf *p, double r);
int  capdb_buf_append_str(capdb_buf *p, const char *z);

void capdb_reader_init(capdb_reader *r, const unsigned char *a, int n);
int  capdb_reader_bytes(capdb_reader *r, int n);
int  capdb_reader_consume(capdb_reader *r, int n);
int  capdb_reader_u8(capdb_reader *r, unsigned char *p);
int  capdb_reader_u32(capdb_reader *r, unsigned int *p);
int  capdb_reader_i32(capdb_reader *r, int *p);
int  capdb_reader_i64(capdb_reader *r, long long *p);
int  capdb_reader_double(capdb_reader *r, double *p);
int  capdb_reader_str(capdb_reader *r, char **pz);
int  capdb_reader_blob(capdb_reader *r, unsigned char **pa, int *pn);

int capdb_frame_encode(unsigned char type, capdb_buf *payload,
                         capdb_buf *out);
int capdb_frame_decode(const unsigned char *a, int n,
                         unsigned char *pType, capdb_reader *payload);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_PROTO_H_ */
