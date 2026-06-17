
#if !defined(__CAPDB_SERVER_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_SERVER_H_ 1

#include "../capdb_network.h"
#if defined(CAPDB_ENABLE_STORE)
#include "../cluster/capdb_cluster.h"
#endif
#if defined(CAPDB_ENABLE_REPLICATION)
#include "../replication/capdb_rep.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct capdb_server capdb_server;

typedef struct capdb_server_config {
  const char *zListen;
  const char *zCertFile;
  const char *zKeyFile;
  const char *zCaFile;
  const char *zAuthFile;
  const char *zDbRoot;
  const char *zVolumeRoot;
  const char *zStorage;      /* "legacy" or "volume" */
  int clusterRole;           /* CAPDB_CLUSTER_ROLE_* */
  int repSyncMode;
  int maxClients;
  int poolMin;
  int poolMax;
  int bInsecure;
#if defined(CAPDB_ENABLE_REPLICATION)
  const char *zRepListen;
  const char *zRepPrimary;
  const char *zRepToken;
#endif
} capdb_server_config;

int  capdb_server_start(const capdb_server_config *pCfg,
                          capdb_server **pp);
void capdb_server_stop(capdb_server *p);
int  capdb_server_run(capdb_server *p);  /* blocks until stop */
void capdb_server_request_stop(capdb_server *p);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_SERVER_H_ */
