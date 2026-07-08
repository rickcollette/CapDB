
#if defined(CAPDB_ENABLE_NETWORK)

#include "capdb_client.h"
#include "../capdb_io.h"
#include "../proto/capdb_proto.h"
#include "../tls/capdb_tls.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <ctype.h>
#include <strings.h>

#define CAPDB_MAX_REPLICA_STREAMS 8

struct capdb_conn {
  capdb_stream *pStream;
  capdb_stream *pReadStream;
  capdb_stream *aReadStream[CAPDB_MAX_REPLICA_STREAMS];
  int nReadStream;
  char *zReplicas;
  int iReplica;
  int errcode;
  char *zErrmsg;
  int bOpen;
  int nChange;            /* changes() from the last EXEC_RESULT */
  long long lastRowid;    /* last_insert_rowid() from the last EXEC_RESULT */
};

struct capdb_net_stmt {
  capdb_conn *pConn;
  capdb_stream *pStream;
  int id;
  int rc;
  int nCol;
  int iCol;
  unsigned char *row;        /* current row payload (non-owning: into aRow[]) */
  int nRow;
  capdb_reader rowReader;
  /* Row prefetch buffer: capdb_net_step pulls a batch of rows per round-trip and
  ** serves them locally, so listing N rows costs ~N/CAPDB_NET_BATCH round-trips. */
  unsigned char **aRow;      /* owned row payloads */
  int *aRowLen;
  int nRowBuf;               /* rows currently buffered */
  int iRowBuf;               /* index of next buffered row to serve */
  int eof;                   /* terminal STEP_DONE (more=0) seen */
  int finalRc;               /* rc carried by the terminal STEP_DONE */
};

#define CAPDB_NET_BATCH 256

typedef struct UriParams {
  char *zHost;
  int port;
  char *zPath;
  char *zToken;
  char *zUser;
  char *zPass;
  char *zCa;
  char *zRole;
  char *zReadPref;
  char *zReplicas;
  int bInsecure;
  int bInsecureAny;
  int connectTimeoutMs;
} UriParams;

static int uriHostIsLoopback(const char *zHost){
  if( zHost==0 ) return 0;
  return strcmp(zHost,"127.0.0.1")==0
      || strcmp(zHost,"localhost")==0
      || strcmp(zHost,"::1")==0
      || strcmp(zHost,"[::1]")==0;
}

static void connSetError(capdb_conn *p, int rc, const char *z){
  p->errcode = rc;
  free(p->zErrmsg);
  p->zErrmsg = z ? strdup(z) : 0;
}

static void uriFree(UriParams *u){
  free(u->zHost);
  free(u->zPath);
  free(u->zToken);
  free(u->zUser);
  free(u->zPass);
  free(u->zCa);
  free(u->zRole);
  free(u->zReadPref);
  free(u->zReplicas);
  memset(u, 0, sizeof(*u));
}

static const unsigned char *sqlSkipSpace(const unsigned char *z){
  while( *z && isspace(*z) ) z++;
  return z;
}

static const unsigned char *sqlSkipString(const unsigned char *z, unsigned char q){
  z++;
  while( *z ){
    if( *z==q ){
      z++;
      if( *z!=q ) break;
    }else{
      z++;
    }
  }
  return z;
}

static int sqlKeywordAt(const unsigned char *z, const char *kw, int n){
  return strncasecmp((const char*)z, kw, n)==0
      && !isalnum(z[n]) && z[n]!='_';
}

static int sqlWithIsReadOnly(const unsigned char *z){
  int depth = 0;
  z += 4; /* WITH */
  z = sqlSkipSpace(z);
  if( sqlKeywordAt(z, "RECURSIVE", 9) ){
    z += 9;
  }
  for(; *z; z++){
    if( *z=='\'' || *z=='"' || *z=='`' || *z=='[' ){
      z = sqlSkipString(z, *z=='[' ? ']' : *z);
      if( z==0 || *z==0 ) break;
      z--;
    }else if( *z=='(' ){
      depth++;
    }else if( *z==')' ){
      if( depth>0 ) depth--;
    }else if( depth==0 ){
      if( *z==',' ){
        continue;
      }
      if( isspace(*z) ){
        continue;
      }
      if( sqlKeywordAt(z, "SELECT", 6) ) return 1;
      if( sqlKeywordAt(z, "INSERT", 6)
       || sqlKeywordAt(z, "UPDATE", 6)
       || sqlKeywordAt(z, "DELETE", 6)
       || sqlKeywordAt(z, "REPLACE", 7) ){
        return 0;
      }
    }
  }
  return 0;
}

static int sqlIsReadOnly(const char *zSql){
  const unsigned char *z = (const unsigned char*)zSql;
  z = sqlSkipSpace(z);
  if( strncasecmp((const char*)z, "SELECT", 6)==0 ) return 1;
  if( strncasecmp((const char*)z, "PRAGMA", 6)==0 ) return 1;
  if( strncasecmp((const char*)z, "EXPLAIN", 7)==0 ) return 1;
  if( sqlKeywordAt(z, "WITH", 4) ) return sqlWithIsReadOnly(z);
  return 0;
}

static int streamHandshake(capdb_stream *strm){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  capdb_buf_init(&pl);
  capdb_buf_append_u32(&pl, CAPDB_PROTO_VERSION);
  if( capdb_stream_send_frame(strm, CAPDB_MSG_HELLO, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(strm, &type, &rx) ) return -1;
  if( type!=CAPDB_MSG_HELLO_ACK ) return -1;
  capdb_reader_init(&r, rx.a, rx.n);
  {
    unsigned int ver = 0;
    if( capdb_reader_u32(&r, &ver) || ver!=CAPDB_PROTO_VERSION ){
      capdb_buf_clear(&rx);
      return -1;
    }
  }
  capdb_buf_clear(&rx);
  return 0;
}

static int streamAuth(capdb_stream *strm, UriParams *u){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_buf_init(&pl);
  if( u->zToken ){
    capdb_buf_append_u8(&pl, CAPDB_AUTH_TOKEN);
    capdb_buf_append_str(&pl, u->zToken);
  }else{
    capdb_buf_append_u8(&pl, CAPDB_AUTH_PASSWORD);
    capdb_buf_append_str(&pl, u->zUser ? u->zUser : "");
    capdb_buf_append_str(&pl, u->zPass ? u->zPass : "");
  }
  if( capdb_stream_send_frame(strm, CAPDB_MSG_AUTH, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(strm, &type, &rx) ) return -1;
  if( type!=CAPDB_MSG_AUTH_OK ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  return 0;
}

static int streamOpenDb(capdb_stream *strm, const char *zPath){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zPath);
  if( capdb_stream_send_frame(strm, CAPDB_MSG_OPEN, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(strm, &type, &rx) ) return -1;
  if( type!=CAPDB_MSG_OPEN_OK ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  return 0;
}

static int uriParse(const char *zUri, UriParams *u){
  const char *z = zUri;
  char *q;
  memset(u, 0, sizeof(*u));
  u->port = CAPDB_DEFAULT_PORT;
  if( strncmp(z, "capdb://", 8)!=0 ) return -1;
  z += 8;
  {
    const char *slash = strchr(z, '/');
    const char *at = strchr(z, '@');
    const char *hostStart = z;
    const char *hostEnd;
    if( at && (!slash || at < slash) ){
      const char *colon = strchr(z, ':');
      if( colon && colon < at ){
        u->zUser = strndup(z, (size_t)(colon-z));
        u->zPass = strndup(colon+1, (size_t)(at-colon-1));
      }else{
        u->zUser = strndup(z, (size_t)(at-z));
      }
      hostStart = at+1;
    }
    hostEnd = slash ? slash : (z + strlen(z));
    if( hostStart < hostEnd ){
      const char *colon = strchr(hostStart, ':');
      if( colon && colon < hostEnd ){
        u->zHost = strndup(hostStart, (size_t)(colon-hostStart));
        u->port = atoi(colon+1);
      }else{
        u->zHost = strndup(hostStart, (size_t)(hostEnd-hostStart));
      }
    }
    if( slash ){
      q = strchr(slash, '?');
      if( q ){
        u->zPath = strndup(slash, (size_t)(q-slash));
        q++;
        while( q && *q ){
          char *amp = strchr(q, '&');
          char *eq = strchr(q, '=');
          char key[64], val[512];
          if( eq && (!amp || eq < amp) ){
            size_t nk = (size_t)(eq-q);
            size_t nv = amp ? (size_t)(amp-eq-1) : strlen(eq+1);
            if( nk >= sizeof(key) ) nk = sizeof(key)-1;
            memcpy(key, q, nk); key[nk]=0;
            if( nv >= sizeof(val) ) nv = sizeof(val)-1;
            memcpy(val, eq+1, nv); val[nv]=0;
            if( strcmp(key,"token")==0 ){ free(u->zToken); u->zToken = strdup(val); }
            else if( strcmp(key,"token_file")==0 ){
              char buf[4096];
              FILE *f = fopen(val, "r");
              if( f ){
                if( fgets(buf, sizeof(buf), f) ){
                  size_t len = strlen(buf);
                  if( len > 0 && buf[len-1]=='\n' ) buf[len-1]=0;
                  if( len > 1 && buf[len-2]=='\r' ) buf[len-2]=0;
                  free(u->zToken);
                  u->zToken = strdup(buf);
                }
                fclose(f);
              }
            }
            else if( strcmp(key,"ca")==0 ){ free(u->zCa); u->zCa = strdup(val); }
            else if( strcmp(key,"insecure")==0 ){
              u->bInsecure = (strcmp(val,"1")==0 || strcmp(val,"true")==0);
            }
            else if( strcmp(key,"insecure_any")==0 ){
              u->bInsecureAny = (strcmp(val,"1")==0 || strcmp(val,"true")==0);
            }
            else if( strcmp(key,"connect_timeout")==0 ) u->connectTimeoutMs = atoi(val)*1000;
            else if( strcmp(key,"password")==0 ){ free(u->zPass); u->zPass = strdup(val); }
            else if( strcmp(key,"role")==0 ){ free(u->zRole); u->zRole = strdup(val); }
            else if( strcmp(key,"read_preference")==0 ){
              free(u->zReadPref); u->zReadPref = strdup(val);
            }
            else if( strcmp(key,"replicas")==0 ){
              free(u->zReplicas); u->zReplicas = strdup(val);
            }
          }
          q = amp ? amp+1 : 0;
        }
      }else{
        u->zPath = strdup(slash);
      }
    }
  }
  if( u->zHost==0 || u->zPath==0 ) return -1;
  return 0;
}

static int connHandshake(capdb_conn *p){
  return streamHandshake(p->pStream);
}

static int connAuth(capdb_conn *p, UriParams *u){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_buf_init(&pl);
  if( u->zToken ){
    capdb_buf_append_u8(&pl, CAPDB_AUTH_TOKEN);
    capdb_buf_append_str(&pl, u->zToken);
  }else{
    capdb_buf_append_u8(&pl, CAPDB_AUTH_PASSWORD);
    capdb_buf_append_str(&pl, u->zUser ? u->zUser : "");
    capdb_buf_append_str(&pl, u->zPass ? u->zPass : "");
  }
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_AUTH, &pl) ){
    capdb_buf_clear(&pl);
    return -1;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return -1;
  if( type==CAPDB_MSG_AUTH_FAIL ){
    char *zMsg = 0;
    capdb_reader r;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_str(&r, &zMsg);
    connSetError(p, CAPDB_NET_AUTH_FAIL, zMsg ? zMsg : "auth failed");
    free(zMsg);
    capdb_buf_clear(&rx);
    return -1;
  }
  if( type!=CAPDB_MSG_AUTH_OK ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  return 0;
}

static int connOpenDb(capdb_conn *p, const char *zPath){
  if( streamOpenDb(p->pStream, zPath) ) return -1;
  p->bOpen = 1;
  return 0;
}

static capdb_stream *connChooseReadStream(capdb_conn *p){
  capdb_stream *strm;
  if( p==0 || p->nReadStream<=0 ) return 0;
  if( p->iReplica<0 ) p->iReplica = 0;
  strm = p->aReadStream[p->iReplica % p->nReadStream];
  p->iReplica = (p->iReplica + 1) % p->nReadStream;
  return strm;
}

static void connAddReplicaStream(capdb_conn *p, capdb_stream *strm){
  if( p==0 || strm==0 ) return;
  if( p->nReadStream>=CAPDB_MAX_REPLICA_STREAMS ){
    capdb_stream_close(strm);
    return;
  }
  p->aReadStream[p->nReadStream++] = strm;
  if( p->pReadStream==0 ) p->pReadStream = strm;
}

int capdb_net_connect(const char *zUri, capdb_conn **pp){
  UriParams u;
  capdb_tls_config tls;
  capdb_conn *p = 0;
  *pp = 0;
  if( uriParse(zUri, &u) ) return CAPDB_NET_MISUSE;
  if( u.bInsecure && !u.bInsecureAny && !uriHostIsLoopback(u.zHost) ){
    uriFree(&u);
    return CAPDB_NET_MISUSE;
  }
  {
    p = (capdb_conn*)calloc(1, sizeof(*p));
    if( p==0 ){ uriFree(&u); return CAPDB_NET_ERROR; }
    memset(&tls, 0, sizeof(tls));
    tls.zCaFile = u.zCa;
    tls.zHostname = u.zHost;
    tls.bInsecure = u.bInsecure;
    tls.connectTimeoutMs = u.connectTimeoutMs;
    if( capdb_stream_connect(u.zHost, u.port, &tls, &p->pStream) ){
      connSetError(p, CAPDB_NET_ERROR, "connect failed");
      uriFree(&u);
      capdb_net_close(p);
      return CAPDB_NET_ERROR;
    }
  }
  if( connHandshake(p) || connAuth(p, &u) || connOpenDb(p, u.zPath) ){
    int err = p->errcode ? p->errcode : CAPDB_NET_ERROR;
    uriFree(&u);
    capdb_net_close(p);
    return err;
  }
  if( u.zReadPref && strcmp(u.zReadPref,"replica")==0 && u.zReplicas ){
    char zRepHost[256];
    const char *entry = u.zReplicas;
    while( entry && *entry && p->nReadStream<CAPDB_MAX_REPLICA_STREAMS ){
      const char *comma = strchr(entry, ',');
      char buf[256];
      const char *colon;
      size_t n = comma ? (size_t)(comma-entry) : strlen(entry);
      int repPort = u.port;
      capdb_stream *rep = 0;
      while( n>0 && isspace((unsigned char)entry[0]) ){
        entry++;
        n--;
      }
      while( n>0 && isspace((unsigned char)entry[n-1]) ) n--;
      if( n==0 || n>=sizeof(buf) ){
        entry = comma ? comma+1 : 0;
        continue;
      }
      memcpy(buf, entry, n);
      buf[n] = 0;
      colon = strrchr(buf, ':');
      if( colon ){
        size_t off = (size_t)(colon - buf);
        buf[off] = 0;
        snprintf(zRepHost, sizeof(zRepHost), "%s", buf);
        repPort = atoi(buf + off + 1);
      }else{
        snprintf(zRepHost, sizeof(zRepHost), "%s", u.zHost);
      }
      memset(&tls, 0, sizeof(tls));
      tls.zCaFile = u.zCa;
      tls.zHostname = zRepHost;
      tls.bInsecure = u.bInsecure;
      tls.connectTimeoutMs = u.connectTimeoutMs;
      if( capdb_stream_connect(zRepHost, repPort, &tls, &rep)==0
       && streamHandshake(rep)==0
       && streamAuth(rep, &u)==0
       && streamOpenDb(rep, u.zPath)==0 ){
        connAddReplicaStream(p, rep);
        rep = 0;
      }
      if( rep ) capdb_stream_close(rep);
      entry = comma ? comma+1 : 0;
    }
  if( p->nReadStream>0 ){
    p->zReplicas = strdup(u.zReplicas);
  }
}
  uriFree(&u);
  *pp = p;
  return CAPDB_NET_OK;
}

int capdb_net_close(capdb_conn *p){
  capdb_buf pl;
  if( p==0 ) return CAPDB_NET_OK;
  if( p->pStream && p->bOpen ){
    capdb_buf_init(&pl);
    capdb_stream_send_frame(p->pStream, CAPDB_MSG_CLOSE, &pl);
    capdb_buf_clear(&pl);
  }
  capdb_stream_close(p->pStream);
  if( p->nReadStream>0 ){
    int i;
    capdb_buf pl;
    for(i=0; i<p->nReadStream; i++){
      capdb_buf_init(&pl);
      capdb_stream_send_frame(p->aReadStream[i], CAPDB_MSG_CLOSE, &pl);
      capdb_buf_clear(&pl);
      capdb_stream_close(p->aReadStream[i]);
    }
  }
  free(p->zReplicas);
  free(p->zErrmsg);
  free(p);
  return CAPDB_NET_OK;
}

int capdb_net_open_db(capdb_conn *p, const char *zPath){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  if( p==0 || zPath==0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zPath);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_OPEN, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type==CAPDB_MSG_ERROR ){
    int rc;
    char *zMsg = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_i32(&r, &rc);
    capdb_reader_str(&r, &zMsg);
    connSetError(p, rc, zMsg);
    free(zMsg);
    capdb_buf_clear(&rx);
    return rc;
  }
  if( type!=CAPDB_MSG_OPEN_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&rx);
  p->bOpen = 1;
  return CAPDB_NET_OK;
}

int capdb_net_exec(capdb_conn *p, const char *zSql,
                 int (*xCallback)(void*,int,char**,char**), void *pArg){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  capdb_stream *strm;
  if( p==0 || !p->bOpen ) return CAPDB_NET_MISUSE;
  strm = sqlIsReadOnly(zSql) ? connChooseReadStream(p) : 0;
  if( strm==0 ) strm = p->pStream;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zSql);
  if( capdb_stream_send_frame(strm, CAPDB_MSG_EXEC, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  for(;;){
    if( capdb_stream_recv_frame(strm, &type, &rx) ) return CAPDB_NET_ERROR;
    if( type==CAPDB_MSG_ERROR ){
      int rc;
      char *zMsg = 0;
      capdb_reader_init(&r, rx.a, rx.n);
      capdb_reader_i32(&r, &rc);
      capdb_reader_str(&r, &zMsg);
      connSetError(p, rc, zMsg);
      free(zMsg);
      capdb_buf_clear(&rx);
      return rc;
    }
    if( type==CAPDB_MSG_STEP_ROW ){
      if( xCallback ){
        unsigned int nCol;
        char **azCol = 0;
        char **azRow = 0;
        int ii;
        capdb_reader_init(&r, rx.a, rx.n);
        capdb_reader_u32(&r, &nCol);
        azCol = (char**)calloc(nCol+1, sizeof(char*));
        azRow = (char**)calloc(nCol+1, sizeof(char*));
        if( azCol==0 || azRow==0 ){
          free(azCol);
          free(azRow);
          capdb_buf_clear(&rx);
          return CAPDB_NET_ERROR;
        }
        for(ii=0; ii<(int)nCol; ii++){
          unsigned char t;
          if( capdb_reader_u8(&r, &t) ){
            for(ii=0; ii<(int)nCol; ii++) free(azRow[ii]);
            free(azRow);
            free(azCol);
            capdb_buf_clear(&rx);
            return CAPDB_NET_ERROR;
          }
          azCol[ii] = (char*)""; /* names not sent in exec row MVP */
          if( t==CAPDB_VAL_NULL ) azRow[ii] = 0;
          else if( t==CAPDB_VAL_INT ){
            long long v;
            char buf[32];
            capdb_reader_i64(&r, &v);
            snprintf(buf, sizeof(buf), "%lld", v);
            azRow[ii] = strdup(buf);
          }else if( t==CAPDB_VAL_TEXT ){
            char *z = 0;
            capdb_reader_str(&r, &z);
            azRow[ii] = z;
          }else if( t==CAPDB_VAL_FLOAT ){
            double fv;
            char buf[64];
            capdb_reader_double(&r, &fv);
            snprintf(buf, sizeof(buf), "%g", fv);
            azRow[ii] = strdup(buf);
          }else if( t==CAPDB_VAL_BLOB ){
            unsigned int bn;
            char *hex;
            unsigned int j;
            if( capdb_reader_u32(&r, &bn)
             || !capdb_reader_bytes(&r, (int)bn) ){
              azRow[ii] = strdup("");
            }else{
              hex = (char*)malloc((size_t)bn*2 + 1);
              if( hex ){
                for(j=0; j<bn; j++)
                  snprintf(hex+j*2, 3, "%02x", r.a[r.i+(int)j]);
                hex[bn*2] = 0;
                azRow[ii] = hex;
              }else{
                azRow[ii] = strdup("");
              }
              r.i += (int)bn;
            }
          }else{
            azRow[ii] = strdup("");
          }
        }
        xCallback(pArg, (int)nCol, azRow, azCol);
        for(ii=0; ii<(int)nCol; ii++) free(azRow[ii]);
        free(azRow);
        free(azCol);
      }
      capdb_buf_clear(&rx);
      continue;
    }
    if( type==CAPDB_MSG_EXEC_RESULT ){
      int rc;
      int nChange = 0;
      long long lastRowid = 0;
      capdb_reader_init(&r, rx.a, rx.n);
      capdb_reader_i32(&r, &rc);
      /* The server also reports changes() and last_insert_rowid() here; capture
      ** them so capdb_net_changes()/capdb_net_last_insert_rowid() are correct
      ** even when the server's pool hands a different handle to the next call. */
      capdb_reader_i32(&r, &nChange);
      capdb_reader_i64(&r, &lastRowid);
      p->nChange = nChange;
      p->lastRowid = lastRowid;
      capdb_buf_clear(&rx);
      return rc;
    }
    capdb_buf_clear(&rx);
    break;
  }
  return CAPDB_NET_ERROR;
}

int capdb_net_prepare(capdb_conn *p, const char *zSql, capdb_net_stmt **pp){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  capdb_net_stmt *st;
  capdb_stream *strm;
  *pp = 0;
  if( p==0 || !p->bOpen ) return CAPDB_NET_MISUSE;
  strm = sqlIsReadOnly(zSql) ? connChooseReadStream(p) : 0;
  if( strm==0 ) strm = p->pStream;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zSql);
  if( capdb_stream_send_frame(strm, CAPDB_MSG_PREPARE, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(strm, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type==CAPDB_MSG_ERROR ){
    int rc;
    char *zMsg = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_i32(&r, &rc);
    capdb_reader_str(&r, &zMsg);
    connSetError(p, rc, zMsg);
    free(zMsg);
    capdb_buf_clear(&rx);
    return rc;
  }
  if( type!=CAPDB_MSG_PREPARE_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  {
    int stmtId = 0, nCol = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_i32(&r, &stmtId);
    capdb_reader_i32(&r, &nCol);
    capdb_buf_clear(&rx);
    st = (capdb_net_stmt*)calloc(1, sizeof(*st));
    if( st==0 ){
      capdb_buf pl2, rx2;
      unsigned char type2;
      capdb_buf_init(&pl2);
      capdb_buf_append_i32(&pl2, stmtId);
      capdb_stream_send_frame(strm, CAPDB_MSG_FINALIZE, &pl2);
      capdb_buf_clear(&pl2);
      if( capdb_stream_recv_frame(strm, &type2, &rx2) ){
        return CAPDB_NET_ERROR;
      }
      capdb_buf_clear(&rx2);
      return CAPDB_NET_ERROR;
    }
    st->pConn = p;
    st->pStream = strm;
    st->id = stmtId;
    st->nCol = nCol;
    *pp = st;
    return CAPDB_NET_OK;
  }
}

static void stmtClearBuf(capdb_net_stmt *st){
  int i;
  for(i=0; i<st->nRowBuf; i++) free(st->aRow[i]);
  free(st->aRow);
  free(st->aRowLen);
  st->aRow = 0;
  st->aRowLen = 0;
  st->nRowBuf = 0;
  st->iRowBuf = 0;
  st->row = 0;               /* non-owning pointer into the (freed) buffer */
  st->nRow = 0;
}

/* Send one STEP request for a batch of rows and buffer the STEP_ROW frames until
** the terminal STEP_DONE (which carries rc + a "more rows pending" flag). */
static int stmtFetchBatch(capdb_net_stmt *st){
  capdb_buf pl, rx;
  unsigned char type;
  int more = 0;
  stmtClearBuf(st);
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, st->id);
  capdb_buf_append_i32(&pl, CAPDB_NET_BATCH);
  if( capdb_stream_send_frame(st->pStream, CAPDB_MSG_STEP, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  for(;;){
    if( capdb_stream_recv_frame(st->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
    if( type==CAPDB_MSG_STEP_ROW ){
      unsigned char **na = (unsigned char**)realloc(st->aRow,
                              sizeof(*na)*(st->nRowBuf+1));
      int *nl;
      if( na==0 ){ capdb_buf_clear(&rx); return CAPDB_NET_ERROR; }
      st->aRow = na;
      nl = (int*)realloc(st->aRowLen, sizeof(*nl)*(st->nRowBuf+1));
      if( nl==0 ){ capdb_buf_clear(&rx); return CAPDB_NET_ERROR; }
      st->aRowLen = nl;
      st->aRow[st->nRowBuf] = rx.a;   /* take ownership of the frame payload */
      st->aRowLen[st->nRowBuf] = rx.n;
      rx.a = 0;
      st->nRowBuf++;
      capdb_buf_clear(&rx);
    }else if( type==CAPDB_MSG_STEP_DONE ){
      capdb_reader r;
      unsigned char m = 0;
      capdb_reader_init(&r, rx.a, rx.n);
      capdb_reader_i32(&r, &st->finalRc);
      capdb_reader_u8(&r, &m);        /* optional "more" flag */
      more = m;
      capdb_buf_clear(&rx);
      break;
    }else if( type==CAPDB_MSG_ERROR ){
      capdb_reader r;
      int rc;
      char *zMsg = 0;
      capdb_reader_init(&r, rx.a, rx.n);
      capdb_reader_i32(&r, &rc);
      capdb_reader_str(&r, &zMsg);
      connSetError(st->pConn, rc, zMsg);
      free(zMsg);
      capdb_buf_clear(&rx);
      st->eof = 1;
      st->finalRc = rc;
      return rc;
    }else{
      capdb_buf_clear(&rx);
      return CAPDB_NET_ERROR;
    }
  }
  if( !more ) st->eof = 1;
  if( more && st->nRowBuf==0 ) return CAPDB_NET_ERROR;
  return CAPDB_NET_OK;
}

int capdb_net_step(capdb_net_stmt *st){
  if( st==0 ) return CAPDB_NET_MISUSE;
  for(;;){
    /* Serve a buffered row without a round-trip. */
    if( st->iRowBuf < st->nRowBuf ){
      st->row = st->aRow[st->iRowBuf];
      st->nRow = st->aRowLen[st->iRowBuf];
      st->iCol = 0;
      st->iRowBuf++;
      capdb_reader_init(&st->rowReader, st->row, st->nRow);
      st->rc = 100; /* CAPDB_ROW */
      return 100;
    }
    /* Buffer drained: if the stream ended, report the terminal rc. */
    if( st->eof ){
      st->rc = st->finalRc;
      return st->rc;
    }
    /* Otherwise pull the next batch and loop to serve from it. */
    if( stmtFetchBatch(st) ){
      st->rc = CAPDB_NET_ERROR;
      return CAPDB_NET_ERROR;
    }
  }
}

int capdb_net_finalize(capdb_net_stmt *st){
  capdb_buf pl, rx;
  unsigned char type;
  if( st==0 ) return CAPDB_NET_OK;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, st->id);
  if( capdb_stream_send_frame(st->pStream, CAPDB_MSG_FINALIZE, &pl) ){
    capdb_buf_clear(&pl);
    stmtClearBuf(st);
    free(st);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(st->pStream, &type, &rx) ){
    stmtClearBuf(st);
    free(st);
    return CAPDB_NET_ERROR;
  }
  if( type==CAPDB_MSG_ERROR ){
    capdb_reader r;
    int rc;
    char *zMsg = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_i32(&r, &rc);
    capdb_reader_str(&r, &zMsg);
    connSetError(st->pConn, rc, zMsg);
    free(zMsg);
    capdb_buf_clear(&rx);
    stmtClearBuf(st);
    free(st);
    return rc;
  }
  if( type!=CAPDB_MSG_FINALIZE_OK ){
    capdb_buf_clear(&rx);
    stmtClearBuf(st);
    free(st);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&rx);
  stmtClearBuf(st);
  free(st);
  return CAPDB_NET_OK;
}

int capdb_net_column_count(capdb_net_stmt *st){ return st ? st->nCol : 0; }

int capdb_net_column_type(capdb_net_stmt *st, int i){
  unsigned char t = CAPDB_VAL_NULL;
  if( st && st->row ){
    capdb_reader r = st->rowReader;
    unsigned int nCol;
    int j;
    capdb_reader_u32(&r, &nCol);
    for(j=0; j<(int)nCol; j++){
      if( capdb_reader_u8(&r, &t) ) break;
      if( j==i ) return t;
      /* skip value */
      if( t==CAPDB_VAL_INT ) r.i += 8;
      else if( t==CAPDB_VAL_FLOAT ) r.i += 8;
      else if( t==CAPDB_VAL_TEXT ){
        unsigned int n;
        capdb_reader_u32(&r, &n);
        r.i += (int)n;
      }else if( t==CAPDB_VAL_BLOB ){
        unsigned int n;
        capdb_reader_u32(&r, &n);
        r.i += (int)n;
      }
    }
  }
  return CAPDB_VAL_NULL;
}

int capdb_net_errcode(capdb_conn *p){ return p ? p->errcode : CAPDB_NET_ERROR; }
const char *capdb_net_errmsg(capdb_conn *p){ return p && p->zErrmsg ? p->zErrmsg : ""; }

int capdb_net_changes(capdb_conn *p){ return p ? p->nChange : 0; }
long long capdb_net_last_insert_rowid(capdb_conn *p){ return p ? p->lastRowid : 0; }

/* Report whether the connection's transport is still healthy. After any socket
** read/write failure (peer reset, server restart, timeout) this returns false,
** letting the Go driver evict the connection from the pool instead of reusing a
** dead socket. */
int capdb_net_alive(capdb_conn *p){
  return p && p->pStream && !capdb_stream_broken(p->pStream);
}

/* Abort any in-flight blocking call on this connection by shutting down its
** socket. Intended for context-cancellation from another thread; the connection
** is unusable afterwards and must be closed. Prefer using one thread per
** connection when possible — cross-thread shutdown during OpenSSL I/O is best-effort. */
int capdb_net_cancel(capdb_conn *p){
  if( p==0 || p->pStream==0 ) return CAPDB_NET_MISUSE;
  capdb_stream_abort(p->pStream);
  return CAPDB_NET_OK;
}

static int connExpectOk(capdb_conn *p, unsigned char want){
  capdb_buf rx;
  unsigned char type;
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return -1;
  if( type==CAPDB_MSG_ERROR ){
    capdb_reader r;
    int rc;
    char *zMsg = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_i32(&r, &rc);
    capdb_reader_str(&r, &zMsg);
    connSetError(p, rc, zMsg);
    free(zMsg);
    capdb_buf_clear(&rx);
    return -1;
  }
  if( type!=want ){
    capdb_buf_clear(&rx);
    return -1;
  }
  capdb_buf_clear(&rx);
  return 0;
}

int capdb_net_vfs_open(capdb_conn *p, const char *zPath, int *pFid){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  if( p==0 || !p->bOpen || pFid==0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zPath);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_OPEN, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type==CAPDB_MSG_ERROR ){
    int rc;
    char *zMsg = 0;
    capdb_reader_init(&r, rx.a, rx.n);
    capdb_reader_i32(&r, &rc);
    capdb_reader_str(&r, &zMsg);
    connSetError(p, rc, zMsg);
    free(zMsg);
    capdb_buf_clear(&rx);
    return rc;
  }
  if( type!=CAPDB_MSG_VFS_OPEN_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  capdb_reader_i32(&r, pFid);
  capdb_buf_clear(&rx);
  return CAPDB_NET_OK;
}

int capdb_net_vfs_close(capdb_conn *p, int fid){
  capdb_buf pl;
  if( p==0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_CLOSE, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  return connExpectOk(p, CAPDB_MSG_PONG) ? CAPDB_NET_ERROR : CAPDB_NET_OK;
}

int capdb_net_vfs_read(capdb_conn *p, int fid, long long iOff, void *zBuf, int n){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  if( p==0 || zBuf==0 || n<0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  capdb_buf_append_i64(&pl, iOff);
  capdb_buf_append_i32(&pl, n);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_READ, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type!=CAPDB_MSG_VFS_READ_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  {
    int got = 0;
    if( capdb_reader_i32(&r, &got) || got<0 || got>n
     || !capdb_reader_bytes(&r, got) ){
      capdb_buf_clear(&rx);
      return CAPDB_NET_ERROR;
    }
    memcpy(zBuf, r.a + r.i, (size_t)got);
    if( got < n ) memset((unsigned char*)zBuf + got, 0, (size_t)(n-got));
    r.i += got;
  }
  capdb_buf_clear(&rx);
  return CAPDB_NET_OK;
}

int capdb_net_vfs_write(capdb_conn *p, int fid, long long iOff,
                      const void *zBuf, int n){
  capdb_buf pl;
  if( p==0 || n<0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  capdb_buf_append_i64(&pl, iOff);
  capdb_buf_append_i32(&pl, n);
  if( n>0 ) capdb_buf_append(&pl, zBuf, n);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_WRITE, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  return connExpectOk(p, CAPDB_MSG_PONG) ? CAPDB_NET_ERROR : CAPDB_NET_OK;
}

int capdb_net_vfs_truncate(capdb_conn *p, int fid, long long nByte){
  capdb_buf pl;
  if( p==0 || fid<0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  capdb_buf_append_i64(&pl, nByte);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_TRUNCATE, &pl) )
    return CAPDB_NET_ERROR;
  capdb_buf_clear(&pl);
  return connExpectOk(p, CAPDB_MSG_PONG) ? CAPDB_NET_ERROR : CAPDB_NET_OK;
}

int capdb_net_vfs_sync(capdb_conn *p, int fid, int flags){
  capdb_buf pl;
  if( p==0 || fid<0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  capdb_buf_append_i32(&pl, flags);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_SYNC, &pl) )
    return CAPDB_NET_ERROR;
  capdb_buf_clear(&pl);
  return connExpectOk(p, CAPDB_MSG_PONG) ? CAPDB_NET_ERROR : CAPDB_NET_OK;
}

int capdb_net_vfs_size(capdb_conn *p, int fid, long long *pSize){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  if( p==0 || fid<0 || pSize==0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_SIZE, &pl) )
    return CAPDB_NET_ERROR;
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type!=CAPDB_MSG_VFS_SIZE_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  capdb_reader_i64(&r, pSize);
  capdb_buf_clear(&rx);
  return CAPDB_NET_OK;
}

int capdb_net_vfs_lock(capdb_conn *p, int fid, int eLock){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  int got;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  capdb_buf_append_i32(&pl, eLock);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_LOCK, &pl) )
    return CAPDB_NET_ERROR;
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type!=CAPDB_MSG_VFS_LOCK_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  capdb_reader_i32(&r, &got);
  capdb_buf_clear(&rx);
  return got ? CAPDB_NET_OK : CAPDB_NET_BUSY;
}

int capdb_net_vfs_check_reserved(capdb_conn *p, int fid, int *pReserved){
  capdb_buf pl, rx;
  unsigned char type;
  capdb_reader r;
  int got = 0;
  if( p==0 || pReserved==0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_i32(&pl, fid);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_CHECK_RESERVED, &pl) )
    return CAPDB_NET_ERROR;
  capdb_buf_clear(&pl);
  if( capdb_stream_recv_frame(p->pStream, &type, &rx) ) return CAPDB_NET_ERROR;
  if( type!=CAPDB_MSG_VFS_CHECK_RESERVED_OK ){
    capdb_buf_clear(&rx);
    return CAPDB_NET_ERROR;
  }
  capdb_reader_init(&r, rx.a, rx.n);
  capdb_reader_i32(&r, &got);
  *pReserved = got;
  capdb_buf_clear(&rx);
  return CAPDB_NET_OK;
}

int capdb_net_vfs_delete(capdb_conn *p, const char *zPath, int syncDir){
  capdb_buf pl;
  if( p==0 || zPath==0 ) return CAPDB_NET_MISUSE;
  capdb_buf_init(&pl);
  capdb_buf_append_str(&pl, zPath);
  capdb_buf_append_i32(&pl, syncDir ? 1 : 0);
  if( capdb_stream_send_frame(p->pStream, CAPDB_MSG_VFS_DELETE, &pl) ){
    capdb_buf_clear(&pl);
    return CAPDB_NET_ERROR;
  }
  capdb_buf_clear(&pl);
  return connExpectOk(p, CAPDB_MSG_PONG) ? CAPDB_NET_ERROR : CAPDB_NET_OK;
}

static int stmtSeekColumn(capdb_net_stmt *st, int iCol, capdb_reader *pR){
  capdb_reader r;
  unsigned int nCol;
  int j;
  if( st==0 || st->row==0 || iCol<0 ) return -1;
  r = st->rowReader;
  if( capdb_reader_u32(&r, &nCol) ) return -1;
  if( iCol>=(int)nCol ) return -1;
  for(j=0; j<iCol; j++){
    unsigned char t;
    if( capdb_reader_u8(&r, &t) ) return -1;
    if( t==CAPDB_VAL_INT || t==CAPDB_VAL_FLOAT ) r.i += 8;
    else if( t==CAPDB_VAL_TEXT || t==CAPDB_VAL_BLOB ){
      unsigned int n;
      if( capdb_reader_u32(&r, &n) ) return -1;
      r.i += (int)n;
    }
  }
  if( pR ) *pR = r;
  return 0;
}

long long capdb_net_column_int64(capdb_net_stmt *st, int i){
  capdb_reader r;
  unsigned char t;
  long long v = 0;
  if( stmtSeekColumn(st, i, &r) ) return 0;
  if( capdb_reader_u8(&r, &t) || t!=CAPDB_VAL_INT ) return 0;
  capdb_reader_i64(&r, &v);
  return v;
}

double capdb_net_column_double(capdb_net_stmt *st, int i){
  capdb_reader r;
  unsigned char t;
  double v = 0.0;
  if( stmtSeekColumn(st, i, &r) ) return 0.0;
  if( capdb_reader_u8(&r, &t) ) return 0.0;
  if( t==CAPDB_VAL_FLOAT ){
    capdb_reader_double(&r, &v);
  }else if( t==CAPDB_VAL_INT ){
    long long iv;
    capdb_reader_i64(&r, &iv);
    v = (double)iv;
  }
  return v;
}

const unsigned char *capdb_net_column_text(capdb_net_stmt *st, int i){
  static __thread unsigned char *zColText = 0;
  static __thread int nColTextCap = 0;
  capdb_reader r;
  unsigned char t;
  unsigned int n;
  if( stmtSeekColumn(st, i, &r) ) return (const unsigned char*)"";
  if( capdb_reader_u8(&r, &t) || t!=CAPDB_VAL_TEXT ) return (const unsigned char*)"";
  if( capdb_reader_u32(&r, &n) || !capdb_reader_bytes(&r, (int)n) )
    return (const unsigned char*)"";
  if( (int)n+1 > nColTextCap ){
    unsigned char *zNew = (unsigned char*)realloc(zColText, (size_t)n+1);
    if( zNew==0 ) return (const unsigned char*)"";
    zColText = zNew;
    nColTextCap = (int)n+1;
  }
  memcpy(zColText, r.a + r.i, n);
  zColText[n] = 0;
  return zColText;
}

const void *capdb_net_column_blob(capdb_net_stmt *st, int i){
  capdb_reader r;
  unsigned char t;
  unsigned int n;
  if( stmtSeekColumn(st, i, &r) ) return 0;
  if( capdb_reader_u8(&r, &t) || t!=CAPDB_VAL_BLOB ) return 0;
  if( capdb_reader_u32(&r, &n) || !capdb_reader_bytes(&r, (int)n) ) return 0;
  return r.a + r.i;
}

int capdb_net_column_bytes(capdb_net_stmt *st, int i){
  capdb_reader r;
  unsigned char t;
  unsigned int n = 0;
  if( stmtSeekColumn(st, i, &r) ) return 0;
  if( capdb_reader_u8(&r, &t) ) return 0;
  if( t==CAPDB_VAL_TEXT || t==CAPDB_VAL_BLOB ){
    capdb_reader_u32(&r, &n);
    return (int)n;
  }
  if( t==CAPDB_VAL_INT ) return 8;
  if( t==CAPDB_VAL_FLOAT ) return 8;
  return 0;
}

#endif /* CAPDB_ENABLE_NETWORK */
