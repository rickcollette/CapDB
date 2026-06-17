
#if !defined(__CAPDB_TLS_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_TLS_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct capdb_stream capdb_stream;

typedef struct capdb_tls_config {
  const char *zCertFile;
  const char *zKeyFile;
  const char *zCaFile;
  int bServer;
  int bInsecure;
  const char *zHostname;   /* SNI / verify hostname for client */
  int connectTimeoutMs;    /* client TCP+TLS connect deadline; 0 => default */
  void *pSharedCtx;        /* optional SSL_CTX*; caller retains ownership */
} capdb_tls_config;

int  capdb_stream_connect(const char *zHost, int port,
                            const capdb_tls_config *pCfg,
                            capdb_stream **pp);
int  capdb_stream_accept(int fd, const capdb_tls_config *pCfg,
                           capdb_stream **pp);
int  capdb_stream_read(capdb_stream *s, void *buf, int n);
int  capdb_stream_write(capdb_stream *s, const void *buf, int n);
void capdb_stream_close(capdb_stream *s);
int  capdb_stream_fd(capdb_stream *s);
int  capdb_stream_broken(capdb_stream *s);
void capdb_stream_abort(capdb_stream *s);

int  capdb_tls_global_init(void);
void capdb_tls_global_shutdown(void);
void *capdb_tls_server_ctx(const capdb_tls_config *pCfg);
void capdb_tls_ctx_free(void *pCtx);

int  capdb_tcp_listen(const char *zListen, int *pFd);
int  capdb_tcp_accept(int listenFd, int *pFd);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_TLS_H_ */
