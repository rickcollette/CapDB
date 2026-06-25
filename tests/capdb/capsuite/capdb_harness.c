#include "capdb_harness.h"
#include "capsuite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <errno.h>

#ifndef CAPSUITE_SERVER
#define CAPSUITE_SERVER "capdb-server"
#endif

const char *capsuite_capdb_server_bin(void){
  const char *z = getenv("CAPSUITE_SERVER");
  return z && z[0] ? z : CAPSUITE_SERVER;
}

static int port_is_free(int port){
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  if( fd<0 ) return 0;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if( bind(fd, (struct sockaddr*)&addr, sizeof(addr))==0 ){
    close(fd);
    return 1;
  }
  close(fd);
  return 0;
}

static int pick_free_port(int seed){
  int port, i;
  for(i=0; i<200; i++){
    port = 20000 + ((seed + i*997) % 30000);
    if( port_is_free(port) ) return port;
  }
  return 0;
}

static int server_tcp_ready(int port){
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  if( fd<0 ) return 0;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if( connect(fd, (struct sockaddr*)&addr, sizeof(addr))==0 ){
    close(fd);
    return 1;
  }
  close(fd);
  return 0;
}

static int capsuite_verbose(void){
  const char *z = getenv("CAPSUITE_VERBOSE");
  return z && (z[0]=='1' || z[0]=='y' || z[0]=='Y');
}

static int capsuite_join_path(char *zOut, size_t nOut, const char *zDir,
                              const char *zName){
  size_t nDir, nName;
  if( zOut==0 || nOut==0 || zDir==0 || zName==0 ) return -1;
  nDir = strlen(zDir);
  nName = strlen(zName);
  if( nDir + 1 + nName + 1 > nOut ) return -1;
  memcpy(zOut, zDir, nDir);
  zOut[nDir] = '/';
  memcpy(zOut+nDir+1, zName, nName+1);
  return 0;
}

int capsuite_capdb_server_start(CapdbTestServer *p, int port){
  char zPort[32];
  char zListen[64];
  const char *zBin;
  const char *argv[24];
  int i = 0;
  int tries;
  int maxClients = 0;
  int bTls = 0;
  int bVolume = 0;
  int bRepListen = 0;
  int repPort = 0;
  int bReplica = 0;
  int bRepSync = 0;
  char zRepPrimary[64];
  char zCert[512];
  char zKey[512];

  if( p==0 ) return -1;
  maxClients = p->maxClients>0 ? p->maxClients : 0;
  bTls = p->bTls==1 ? 1 : 0;
  bVolume = p->bVolume==1 ? 1 : 0;
  bRepListen = p->bRepListen==1 ? 1 : 0;
  repPort = p->repPort;
  bReplica = p->bReplica==1 ? 1 : 0;
  bRepSync = p->bRepSync==1 ? 1 : 0;
  snprintf(zRepPrimary, sizeof(zRepPrimary), "%s", p->zRepPrimary);
  if( bTls ){
    snprintf(zCert, sizeof(zCert), "%s", p->zCert);
    snprintf(zKey, sizeof(zKey), "%s", p->zKey);
  }

  memset(p, 0, sizeof(*p));
  p->maxClients = maxClients;
  p->bTls = bTls;
  p->bVolume = bVolume;
  p->bRepListen = bRepListen;
  p->repPort = repPort;
  p->bReplica = bReplica;
  p->bRepSync = bRepSync;
  snprintf(p->zRepPrimary, sizeof(p->zRepPrimary), "%s", zRepPrimary);
  if( bTls ){
    snprintf(p->zCert, sizeof(p->zCert), "%s", zCert);
    snprintf(p->zKey, sizeof(p->zKey), "%s", zKey);
  }

  if( port<=0 ) port = pick_free_port((int)getpid());
  if( port<=0 ) return -1;
  p->port = port;
  if( p->bRepListen && p->repPort<=0 ) p->repPort = p->port + 1;
  snprintf(p->zDir, sizeof(p->zDir), "capdb_data_%d", port);
  if( capsuite_join_path(p->zAuthFile, sizeof(p->zAuthFile), p->zDir,
                         "mnet_auth.txt")!=0 ){
    return -1;
  }
  snprintf(p->zDbRoot, sizeof(p->zDbRoot), "%s", p->zDir);

  capsuite_rm_rf(p->zDir);
  capsuite_mkdir_p(p->zDir);
  if( p->bVolume ){
    if( capsuite_join_path(p->zVolumeRoot, sizeof(p->zVolumeRoot), p->zDir,
                           "volumes")!=0 ){
      return -1;
    }
    capsuite_mkdir_p(p->zVolumeRoot);
  }
  if( capsuite_write_file(p->zAuthFile, "testtoken\n") ) return -1;

  snprintf(zPort, sizeof(zPort), "%d", port);
  snprintf(zListen, sizeof(zListen), "127.0.0.1:%d", port);
  zBin = capsuite_capdb_server_bin();

  argv[i++] = zBin;
  argv[i++] = "--listen";
  argv[i++] = zListen;
  argv[i++] = "--auth-file";
  argv[i++] = p->zAuthFile;
  if( p->bVolume ){
    argv[i++] = "--storage";
    argv[i++] = "volume";
    argv[i++] = "--volume-root";
    argv[i++] = p->zVolumeRoot;
  }else{
    argv[i++] = "--db-root";
    argv[i++] = p->zDbRoot;
  }
  if( p->bRepListen && p->repPort>0 ){
    static char zRepListen[64];
    snprintf(zRepListen, sizeof(zRepListen), "127.0.0.1:%d", p->repPort);
    argv[i++] = "--rep-listen";
    argv[i++] = zRepListen;
    argv[i++] = "--rep-token";
    argv[i++] = "testtoken";
    if( p->bRepSync ){
      argv[i++] = "--sync-replication";
    }
  }
  if( p->bReplica ){
    argv[i++] = "--role";
    argv[i++] = "replica";
    if( p->zRepPrimary[0] ){
      argv[i++] = "--rep-primary";
      argv[i++] = p->zRepPrimary;
    }
    argv[i++] = "--rep-token";
    argv[i++] = "testtoken";
  }
  if( p->maxClients>0 ){
    static char zMax[16];
    snprintf(zMax, sizeof(zMax), "%d", p->maxClients);
    argv[i++] = "--max-clients";
    argv[i++] = zMax;
  }
  if( p->bTls ){
    argv[i++] = "--cert";
    argv[i++] = p->zCert;
    argv[i++] = "--key";
    argv[i++] = p->zKey;
  }else{
    argv[i++] = "--insecure";
  }
  argv[i++] = 0;

  if( capsuite_spawn(argv, &p->pid) ) return -1;
  if( capsuite_verbose() ){
    fprintf(stderr, "capsuite: spawned %s on port %d (dir %s)\n",
            zBin, port, p->zDir);
    fflush(stderr);
  }
  for(tries=0; tries<50; tries++){
    int st;
    pid_t w = waitpid(p->pid, &st, WNOHANG);
    if( w==p->pid ){
      if( capsuite_verbose() ){
        fprintf(stderr, "capsuite: server exited before ready (status=%d)\n", st);
        fflush(stderr);
      }
      p->pid = 0;
      return -1;
    }
    if( w<0 && errno!=ECHILD ) return -1;
    if( server_tcp_ready(port) || !port_is_free(port) ){
      if( capsuite_verbose() ){
        fprintf(stderr, "capsuite: server ready on port %d (try %d)\n",
                port, tries);
        fflush(stderr);
      }
      return 0;
    }
    capsuite_sleep_ms(100);
  }
  capsuite_kill_wait(p->pid);
  p->pid = 0;
  return -1;
}

void capsuite_capdb_server_stop(CapdbTestServer *p){
  if( p==0 ) return;
  capsuite_kill_wait(p->pid);
  p->pid = 0;
  capsuite_rm_rf(p->zDir);
}
