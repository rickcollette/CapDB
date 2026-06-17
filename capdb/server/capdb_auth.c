
#if defined(CAPDB_ENABLE_NETWORK)

#include "proto/capdb_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

#define AUTH_MAX_FAILS    5
#define AUTH_LOCKOUT_SEC  30
#define AUTH_MAX_PEERS    4096

typedef struct AuthFailEntry AuthFailEntry;
struct AuthFailEntry {
  char zPeer[64];
  int nFails;
  time_t lockUntil;
  AuthFailEntry *pNext;
};

static AuthFailEntry *gAuthFails = 0;
static pthread_mutex_t gAuthMutex = PTHREAD_MUTEX_INITIALIZER;

static int authConstantTimeEq(const char *a, const char *b){
  size_t i;
  size_t la = strlen(a);
  size_t lb = strlen(b);
  unsigned char c = la ^ lb;
  for(i=0; i<la && i<lb; i++){
    c |= (unsigned char)(a[i] ^ b[i]);
  }
  return c==0 && la==lb;
}

static AuthFailEntry *authFindPeer(const char *zPeer){
  AuthFailEntry *e;
  for(e=gAuthFails; e; e=e->pNext){
    if( strcmp(e->zPeer, zPeer)==0 ) return e;
  }
  return 0;
}

static int authThrottleCheck(const char *zPeer){
  AuthFailEntry *e;
  time_t now = time(0);
  const char *p = (zPeer && zPeer[0]) ? zPeer : "?";
  int locked = 0;
  pthread_mutex_lock(&gAuthMutex);
  e = authFindPeer(p);
  if( e && e->lockUntil > now ) locked = 1;
  pthread_mutex_unlock(&gAuthMutex);
  if( locked ){
    fprintf(stderr, "capdb audit event=auth.throttle peer=%s\n", p);
    return -1;
  }
  return 0;
}

static void authPrunePeers(void){
  AuthFailEntry *e, **pp;
  int n = 0;
  for(e=gAuthFails; e; e=e->pNext) n++;
  while( n > AUTH_MAX_PEERS ){
    pp = &gAuthFails;
    while( *pp && (*pp)->pNext ) pp = &(*pp)->pNext;
    if( *pp ){
      e = *pp;
      *pp = 0;
      free(e);
      n--;
    }else{
      break;
    }
  }
}

static void authRecordFail(const char *zPeer){
  AuthFailEntry *e;
  const char *p = (zPeer && zPeer[0]) ? zPeer : "?";
  pthread_mutex_lock(&gAuthMutex);
  e = authFindPeer(p);
  if( e==0 ){
    authPrunePeers();
    e = (AuthFailEntry*)calloc(1, sizeof(*e));
    if( e==0 ){
      pthread_mutex_unlock(&gAuthMutex);
      return;
    }
    snprintf(e->zPeer, sizeof(e->zPeer), "%s", p);
    e->pNext = gAuthFails;
    gAuthFails = e;
  }
  e->nFails++;
  if( e->nFails >= AUTH_MAX_FAILS ){
    e->lockUntil = time(0) + AUTH_LOCKOUT_SEC;
    e->nFails = 0;
  }
  pthread_mutex_unlock(&gAuthMutex);
}

static void authRecordOk(const char *zPeer){
  AuthFailEntry *e, **pp;
  const char *p = (zPeer && zPeer[0]) ? zPeer : "?";
  pthread_mutex_lock(&gAuthMutex);
  for(pp=&gAuthFails; *pp; pp=&(*pp)->pNext){
    if( strcmp((*pp)->zPeer, p)==0 ){
      e = *pp;
      *pp = e->pNext;
      free(e);
      break;
    }
  }
  pthread_mutex_unlock(&gAuthMutex);
}

int capdb_auth_check(const char *zAuthFile, int method,
                       const char *zUser, const char *zSecret){
  return capdb_auth_check_peer(zAuthFile, method, zUser, zSecret, 0);
}

int capdb_auth_check_peer(const char *zAuthFile, int method,
                            const char *zUser, const char *zSecret,
                            const char *zPeer){
  FILE *f;
  char line[512];
  if( zAuthFile==0 || zSecret==0 ) return -1;
  if( authThrottleCheck(zPeer) ) return -1;
  f = fopen(zAuthFile, "r");
  if( f==0 ) return -1;
  while( fgets(line, sizeof(line), f) ){
    char *zTok;
    char *zPass;
    size_t n;
    if( line[0]=='#' || line[0]=='\n' ) continue;
    n = strlen(line);
    while( n>0 && (line[n-1]=='\n' || line[n-1]=='\r') ){
      line[--n] = 0;
    }
    if( n>=sizeof(line)-1 ) continue;
    if( method==CAPDB_AUTH_TOKEN ){
      zTok = line;
      if( authConstantTimeEq(zTok, zSecret) ){
        fclose(f);
        authRecordOk(zPeer);
        return 0;
      }
    }else if( method==CAPDB_AUTH_PASSWORD ){
      zTok = line;
      zPass = strchr(line, ':');
      if( zPass ){
        *zPass++ = 0;
        if( zUser && authConstantTimeEq(zTok, zUser)
         && authConstantTimeEq(zPass, zSecret) ){
          fclose(f);
          authRecordOk(zPeer);
          return 0;
        }
      }
    }
  }
  fclose(f);
  authRecordFail(zPeer);
  return -1;
}

#endif /* CAPDB_ENABLE_NETWORK */
