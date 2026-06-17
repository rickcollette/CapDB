/*
** Helpers to spawn capdb-server for loopback tests.
*/
#ifndef CAPSUITE_CAPDB_HARNESS_H
#define CAPSUITE_CAPDB_HARNESS_H

#include <sys/types.h>

typedef struct CapdbTestServer CapdbTestServer;
struct CapdbTestServer {
  pid_t pid;
  int port;
  int maxClients;
  int bTls;
  int bVolume;
  int bRepListen;
  int repPort;
  int bReplica;
  char zRepPrimary[64];
  char zCert[512];
  char zKey[512];
  char zDir[512];
  char zAuthFile[512];
  char zDbRoot[512];
  char zVolumeRoot[512];
};

const char *capsuite_capdb_server_bin(void);
int capsuite_capdb_server_start(CapdbTestServer *p, int port);
void capsuite_capdb_server_stop(CapdbTestServer *p);

#endif
