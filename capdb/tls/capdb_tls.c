
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_tls.h"
#include "../capdb_network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>

#include <openssl/ssl.h>

#define CAPDB_DEFAULT_CONNECT_TIMEOUT_MS 10000
#include <openssl/err.h>

struct capdb_stream {
  int fd;
  SSL *ssl;
  int bInsecure;
  int bBroken;          /* set once any read/write fails: the conn is unusable */
};

int capdb_tls_global_init(void){
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();
#endif
  return 0;
}

void capdb_tls_global_shutdown(void){
}

static SSL_CTX *capdb_tls_ctx(const capdb_tls_config *pCfg);

void *capdb_tls_server_ctx(const capdb_tls_config *pCfg){
  return capdb_tls_ctx(pCfg);
}

void capdb_tls_ctx_free(void *pCtx){
  if( pCtx ) SSL_CTX_free((SSL_CTX*)pCtx);
}

static SSL_CTX *tlsPickCtx(const capdb_tls_config *pCfg){
  if( pCfg && pCfg->pSharedCtx ) return (SSL_CTX*)pCfg->pSharedCtx;
  return capdb_tls_ctx(pCfg);
}

static SSL_CTX *capdb_tls_ctx(const capdb_tls_config *pCfg){
  const SSL_METHOD *method;
  SSL_CTX *ctx;
  if( pCfg->bInsecure ) return 0;
  method = pCfg->bServer ? TLS_server_method() : TLS_client_method();
  ctx = SSL_CTX_new(method);
  if( ctx==0 ) return 0;
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  if( pCfg->bServer ){
    if( pCfg->zCertFile && pCfg->zKeyFile ){
      if( SSL_CTX_use_certificate_file(ctx, pCfg->zCertFile, SSL_FILETYPE_PEM)<=0
       || SSL_CTX_use_PrivateKey_file(ctx, pCfg->zKeyFile, SSL_FILETYPE_PEM)<=0 ){
        SSL_CTX_free(ctx);
        return 0;
      }
    }
    /* If a client CA bundle is configured, require and verify client certs
    ** (mutual TLS). Without it, client-cert auth is simply not requested. */
    if( pCfg->zCaFile ){
      if( SSL_CTX_load_verify_locations(ctx, pCfg->zCaFile, 0)<=0 ){
        SSL_CTX_free(ctx);
        return 0;
      }
      SSL_CTX_set_verify(ctx,
          SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, 0);
    }
  }else{
    /* Verify the server certificate by default. A configured CA bundle is the
    ** trust anchor; otherwise fall back to the system trust store. Hostname
    ** verification is enabled per-connection via SSL_set1_host(). The only way
    ** to skip verification is the explicit bInsecure (dev) flag handled above. */
    if( pCfg->zCaFile ){
      if( SSL_CTX_load_verify_locations(ctx, pCfg->zCaFile, 0)<=0 ){
        SSL_CTX_free(ctx);
        return 0;
      }
    }else{
      SSL_CTX_set_default_verify_paths(ctx);
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, 0);
  }
  return ctx;
}

/* Connect fd to addr, bounded by timeoutMs, so a black-holed host fails fast
** instead of blocking for the OS default. Leaves the socket in blocking mode. */
static int connectFdTimeout(int fd, const struct sockaddr *addr,
                            socklen_t alen, int timeoutMs){
  int flags = fcntl(fd, F_GETFL, 0);
  if( flags<0 ) return -1;
  if( fcntl(fd, F_SETFL, flags | O_NONBLOCK)<0 ) return -1;
  if( connect(fd, addr, alen)==0 ){
    fcntl(fd, F_SETFL, flags);
    return 0;
  }
  if( errno!=EINPROGRESS ) return -1;
  {
    struct pollfd pfd;
    int rc, soerr = 0;
    socklen_t slen = sizeof(soerr);
    pfd.fd = fd;
    pfd.events = POLLOUT;
    rc = poll(&pfd, 1, timeoutMs>0 ? timeoutMs : -1);
    if( rc<=0 ) return -1;  /* timeout (0) or poll error */
    if( getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen)<0 || soerr!=0 ) return -1;
  }
  if( fcntl(fd, F_SETFL, flags)<0 ) return -1;  /* restore blocking */
  return 0;
}

static int tcpConnect(const char *zHost, int port, int timeoutMs){
  struct addrinfo hints, *res, *rp;
  char portbuf[16];
  int fd = -1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portbuf, sizeof(portbuf), "%d", port);
  if( getaddrinfo(zHost, portbuf, &hints, &res) ) return -1;
  for(rp=res; rp; rp=rp->ai_next){
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if( fd<0 ) continue;
    if( connectFdTimeout(fd, rp->ai_addr, rp->ai_addrlen, timeoutMs)==0 ) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

int capdb_tcp_listen(const char *zListen, int *pFd){
  char host[256];
  char portbuf[16];
  const char *zPort = portbuf;
  int port = CAPDB_DEFAULT_PORT;
  struct addrinfo hints, *res, *rp;
  int fd = -1;
  int yes = 1;

  if( zListen[0]=='/' ){
    /* Unix socket not implemented in phase 1 */
    return -1;
  }
  if( strchr(zListen, ':') ){
    const char *colon = strrchr(zListen, ':');
    size_t nHost = (size_t)(colon - zListen);
    if( nHost >= sizeof(host) ) return -1;
    memcpy(host, zListen, nHost);
    host[nHost] = 0;
    port = atoi(colon+1);
    snprintf(portbuf, sizeof(portbuf), "%d", port);
  }else{
    snprintf(host, sizeof(host), "%s", zListen);
    snprintf(portbuf, sizeof(portbuf), "%d", port);
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if( getaddrinfo(host[0] ? host : 0, zPort, &hints, &res) ) return -1;
  for(rp=res; rp; rp=rp->ai_next){
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if( fd<0 ) continue;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if( bind(fd, rp->ai_addr, rp->ai_addrlen)==0
     && listen(fd, 64)==0 ) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if( fd>=0 ) *pFd = fd;
  return fd>=0 ? 0 : -1;
}

int capdb_tcp_accept(int listenFd, int *pFd){
  int fd = accept(listenFd, 0, 0);
  if( fd<0 ) return -1;
  *pFd = fd;
  return 0;
}

int capdb_stream_connect(const char *zHost, int port,
                            const capdb_tls_config *pCfg,
                            capdb_stream **pp){
  capdb_stream *s;
  int fd;
  int timeoutMs = (pCfg && pCfg->connectTimeoutMs>0)
                    ? pCfg->connectTimeoutMs : CAPDB_DEFAULT_CONNECT_TIMEOUT_MS;
  *pp = 0;
  fd = tcpConnect(zHost, port, timeoutMs);
  if( fd<0 ) return -1;
  s = (capdb_stream*)calloc(1, sizeof(*s));
  if( s==0 ){ close(fd); return -1; }
  s->fd = fd;
  s->bInsecure = pCfg && pCfg->bInsecure;
  if( !s->bInsecure ){
    SSL_CTX *ctx = tlsPickCtx(pCfg);
    int owned = (pCfg==0 || pCfg->pSharedCtx==0);
    struct timeval tv;
    if( ctx==0 ){ close(fd); free(s); return -1; }
    s->ssl = SSL_new(ctx);
    if( owned ) SSL_CTX_free(ctx);
    if( s->ssl==0 ){ close(fd); free(s); return -1; }
    SSL_set_fd(s->ssl, fd);
    if( pCfg && pCfg->zHostname ){
      SSL_set_tlsext_host_name(s->ssl, pCfg->zHostname);
      /* Bind verification to the expected hostname so a valid cert issued for a
      ** different host cannot be used to MITM the connection. */
      SSL_set1_host(s->ssl, pCfg->zHostname);
    }
    /* Bound the TLS handshake too: a connected-but-silent peer otherwise hangs
    ** SSL_connect forever. Clear the timeout after the handshake so long-lived
    ** idle connections are not torn down. */
    tv.tv_sec = timeoutMs/1000;
    tv.tv_usec = (timeoutMs%1000)*1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if( SSL_connect(s->ssl)<=0 ){
      capdb_stream_close(s);
      return -1;
    }
    {
      struct timeval z;
      z.tv_sec = 0; z.tv_usec = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof(z));
    }
  }
  *pp = s;
  return 0;
}

int capdb_stream_accept(int fd, const capdb_tls_config *pCfg,
                           capdb_stream **pp){
  capdb_stream *s;
  *pp = 0;
  s = (capdb_stream*)calloc(1, sizeof(*s));
  if( s==0 ) return -1;
  s->fd = fd;
  s->bInsecure = pCfg && pCfg->bInsecure;
  if( !s->bInsecure ){
    SSL_CTX *ctx = tlsPickCtx(pCfg);
    int owned = (pCfg==0 || pCfg->pSharedCtx==0);
    struct timeval tv;
    int timeoutMs = CAPDB_HANDSHAKE_TIMEOUT_MS;
    if( ctx==0 ){ free(s); return -1; }
    s->ssl = SSL_new(ctx);
    if( owned ) SSL_CTX_free(ctx);
    if( s->ssl==0 ){ free(s); return -1; }
    SSL_set_fd(s->ssl, fd);
    tv.tv_sec = timeoutMs/1000;
    tv.tv_usec = (timeoutMs%1000)*1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if( SSL_accept(s->ssl)<=0 ){
      capdb_stream_close(s);
      return -1;
    }
    {
      struct timeval z;
      z.tv_sec = 0; z.tv_usec = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof(z));
    }
  }
  *pp = s;
  return 0;
}

int capdb_stream_read(capdb_stream *s, void *buf, int n){
  int got = 0;
  unsigned char *p = (unsigned char*)buf;
  while( got < n ){
    int rc;
    if( s->ssl ){
      rc = SSL_read(s->ssl, p+got, n-got);
    }else{
      rc = (int)read(s->fd, p+got, (size_t)(n-got));
    }
    if( rc<=0 ){ s->bBroken = 1; return got>0 ? got : -1; }
    got += rc;
  }
  return got;
}

int capdb_stream_broken(capdb_stream *s){ return s ? s->bBroken : 1; }

void capdb_stream_abort(capdb_stream *s){
  if( s==0 ) return;
  s->bBroken = 1;
  if( s->ssl ) SSL_set_quiet_shutdown(s->ssl, 1);
  if( s->fd>=0 ) shutdown(s->fd, SHUT_RDWR);
}

int capdb_stream_write(capdb_stream *s, const void *buf, int n){
  int sent = 0;
  const unsigned char *p = (const unsigned char*)buf;
  while( sent < n ){
    int rc;
    if( s->ssl ){
      rc = SSL_write(s->ssl, p+sent, n-sent);
    }else{
      rc = (int)write(s->fd, p+sent, (size_t)(n-sent));
    }
    if( rc<=0 ){ s->bBroken = 1; return -1; }
    sent += rc;
  }
  return sent;
}

void capdb_stream_close(capdb_stream *s){
  if( s==0 ) return;
  if( s->ssl ){
    SSL_shutdown(s->ssl);
    SSL_free(s->ssl);
  }
  if( s->fd>=0 ) close(s->fd);
  free(s);
}

int capdb_stream_fd(capdb_stream *s){
  return s ? s->fd : -1;
}

#endif /* CAPDB_ENABLE_NETWORK */
