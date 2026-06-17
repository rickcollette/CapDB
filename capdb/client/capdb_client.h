
#if !defined(__CAPDB_CLIENT_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_CLIENT_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct capdb_conn capdb_conn;
typedef struct capdb_net_stmt capdb_net_stmt;

#define CAPDB_NET_OK           0
#define CAPDB_NET_ERROR        1
#define CAPDB_NET_AUTH_FAIL    2
#define CAPDB_NET_MISUSE       3
#define CAPDB_NET_BUSY         5

int capdb_net_connect(const char *zUri, capdb_conn **pp);
int capdb_net_close(capdb_conn *p);
int capdb_net_open_db(capdb_conn *p, const char *zPath);
int capdb_net_exec(capdb_conn *p, const char *zSql,
                   int (*xCallback)(void*,int,char**,char**), void *pArg);
int capdb_net_prepare(capdb_conn *p, const char *zSql, capdb_net_stmt **pp);
int capdb_net_step(capdb_net_stmt *p);
int capdb_net_finalize(capdb_net_stmt *p);
int capdb_net_column_count(capdb_net_stmt *p);
int capdb_net_column_type(capdb_net_stmt *p, int i);
long long capdb_net_column_int64(capdb_net_stmt *p, int i);
double capdb_net_column_double(capdb_net_stmt *p, int i);
const unsigned char *capdb_net_column_text(capdb_net_stmt *p, int i);
const void *capdb_net_column_blob(capdb_net_stmt *p, int i);
/* Text column pointers are NUL-terminated copies valid until the next
** capdb_net_step/finalize on this thread. Blob pointers are length-only. */
int capdb_net_column_bytes(capdb_net_stmt *p, int i);
int capdb_net_errcode(capdb_conn *p);
const char *capdb_net_errmsg(capdb_conn *p);

/* changes() / last_insert_rowid() from the most recent capdb_net_exec(),
** captured from the server's EXEC_RESULT (reliable under server-side pooling). */
int capdb_net_changes(capdb_conn *p);
long long capdb_net_last_insert_rowid(capdb_conn *p);

/* Abort an in-flight blocking call by shutting down the socket (best-effort from
** another thread). The connection must be closed afterwards. */
int capdb_net_cancel(capdb_conn *p);

/* False once the transport has failed (peer reset, server restart, timeout);
** the caller should discard the connection. */
int capdb_net_alive(capdb_conn *p);

/* Remote VFS RPC (requires authenticated open session) */
int capdb_net_vfs_open(capdb_conn *p, const char *zPath, int *pFid);
int capdb_net_vfs_close(capdb_conn *p, int fid);
int capdb_net_vfs_read(capdb_conn *p, int fid, long long iOff,
                       void *zBuf, int n);
int capdb_net_vfs_write(capdb_conn *p, int fid, long long iOff,
                        const void *zBuf, int n);
int capdb_net_vfs_truncate(capdb_conn *p, int fid, long long nByte);
int capdb_net_vfs_sync(capdb_conn *p, int fid, int flags);
int capdb_net_vfs_size(capdb_conn *p, int fid, long long *pSize);
int capdb_net_vfs_lock(capdb_conn *p, int fid, int eLock);
int capdb_net_vfs_check_reserved(capdb_conn *p, int fid, int *pReserved);

int capdb_net_vfs_register(const char *zDefaultUri, int makeDefault);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_CLIENT_H_ */
