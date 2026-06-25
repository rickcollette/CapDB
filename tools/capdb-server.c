/*
** capdb-server — TLS SQL server for remote CapDB access.
*/
#if defined(CAPDB_ENABLE_NETWORK) && defined(CAPDB_ENABLE_POOL)

#include "capdb/server/capdb_server.h"
#if defined(CAPDB_ENABLE_STORE)
#include "capdb/cluster/capdb_cluster.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static capdb_server *gServer = 0;

static void handleSignal(int sig){
  (void)sig;
  if( gServer ) capdb_server_request_stop(gServer);
}

static void usage(const char *zProg){
  fprintf(stderr,
    "Usage: %s [options]\n"
    "  --listen ADDR:PORT   Listen address (default 0.0.0.0:5432)\n"
    "  --cert FILE          TLS certificate PEM\n"
    "  --key FILE           TLS private key PEM\n"
    "  --ca FILE            Client CA bundle (optional)\n"
    "  --auth-file FILE     Token/password auth file\n"
    "  --db-root DIR        Jail for database paths (legacy storage)\n"
    "  --storage MODE       legacy (default) or volume\n"
    "  --volume-root DIR    Root for CapDB volumes (volume storage)\n"
    "  --role ROLE          primary (default) or replica\n"
    "  --sync-replication   Wait for replica ACK on COMMIT\n"
    "  --rep-listen ADDR    Replication listen (primary; default port+1)\n"
    "  --rep-primary HOST:PORT  Upstream primary for replica role\n"
    "  --rep-token TOKEN    Shared replication auth token\n"
    "  --pool-min N         Minimum pool size (default 1)\n"
    "  --pool-max N         Maximum pool size (default 8)\n"
    "  --max-clients N      Max concurrent connections (default 256)\n"
    "  --quiet              Suppress audit logging\n"
    "  --insecure           Disable TLS (development only)\n",
    zProg);
}

int main(int argc, char **argv){
  capdb_server_config cfg;
  int i, rc;

  memset(&cfg, 0, sizeof(cfg));
  cfg.zListen = "0.0.0.0:5432";
  cfg.zStorage = "legacy";
  cfg.clusterRole = CAPDB_CLUSTER_ROLE_PRIMARY;
  cfg.poolMin = 1;
  cfg.poolMax = 8;

  for(i=1; i<argc; i++){
    const char *z = argv[i];
    if( strcmp(z,"--listen")==0 && i+1<argc ) cfg.zListen = argv[++i];
    else if( strcmp(z,"--cert")==0 && i+1<argc ) cfg.zCertFile = argv[++i];
    else if( strcmp(z,"--key")==0 && i+1<argc ) cfg.zKeyFile = argv[++i];
    else if( strcmp(z,"--ca")==0 && i+1<argc ) cfg.zCaFile = argv[++i];
    else if( strcmp(z,"--auth-file")==0 && i+1<argc ) cfg.zAuthFile = argv[++i];
    else if( strcmp(z,"--db-root")==0 && i+1<argc ) cfg.zDbRoot = argv[++i];
    else if( strcmp(z,"--storage")==0 && i+1<argc ) cfg.zStorage = argv[++i];
    else if( strcmp(z,"--volume-root")==0 && i+1<argc ) cfg.zVolumeRoot = argv[++i];
    else if( strcmp(z,"--role")==0 && i+1<argc ){
      if( strcmp(argv[++i],"replica")==0 ) cfg.clusterRole = CAPDB_CLUSTER_ROLE_REPLICA;
      else cfg.clusterRole = CAPDB_CLUSTER_ROLE_PRIMARY;
    }
    else if( strcmp(z,"--sync-replication")==0 ) cfg.repSyncMode = 1;
    else if( strcmp(z,"--rep-listen")==0 && i+1<argc ) cfg.zRepListen = argv[++i];
    else if( strcmp(z,"--rep-primary")==0 && i+1<argc ) cfg.zRepPrimary = argv[++i];
    else if( strcmp(z,"--rep-token")==0 && i+1<argc ) cfg.zRepToken = argv[++i];
    else if( strcmp(z,"--pool-min")==0 && i+1<argc ) cfg.poolMin = atoi(argv[++i]);
    else if( strcmp(z,"--pool-max")==0 && i+1<argc ) cfg.poolMax = atoi(argv[++i]);
    else if( strcmp(z,"--max-clients")==0 && i+1<argc ) cfg.maxClients = atoi(argv[++i]);
    else if( strcmp(z,"--quiet")==0 ) cfg.bQuiet = 1;
    else if( strcmp(z,"--insecure")==0 ) cfg.bInsecure = 1;
    else if( strcmp(z,"-h")==0 || strcmp(z,"--help")==0 ){ usage(argv[0]); return 0; }
    else { usage(argv[0]); return 1; }
  }

  if( cfg.zAuthFile==0 ){
    fprintf(stderr, "error: --auth-file is required\n");
    return 1;
  }
  if( cfg.zStorage && strcmp(cfg.zStorage,"volume")==0 ){
    if( cfg.zVolumeRoot==0 ){
      fprintf(stderr, "error: --volume-root required for volume storage\n");
      return 1;
    }
  }else if( cfg.zDbRoot==0 ){
    fprintf(stderr, "error: --db-root is required for legacy storage\n");
    return 1;
  }
  if( !cfg.bInsecure && (cfg.zCertFile==0 || cfg.zKeyFile==0) ){
    fprintf(stderr, "error: --cert and --key required unless --insecure\n");
    return 1;
  }
  if( cfg.zRepListen && (cfg.zRepToken==0 || cfg.zRepToken[0]==0) ){
    fprintf(stderr, "error: --rep-token required when --rep-listen is set\n");
    return 1;
  }
  if( cfg.clusterRole==CAPDB_CLUSTER_ROLE_REPLICA && cfg.zRepPrimary
   && (cfg.zRepToken==0 || cfg.zRepToken[0]==0) ){
    fprintf(stderr, "error: --rep-token required for replica role\n");
    return 1;
  }

  signal(SIGINT, handleSignal);
  signal(SIGTERM, handleSignal);

  rc = capdb_server_start(&cfg, &gServer);
  if( rc ){
    fprintf(stderr, "error: failed to start server\n");
    return 1;
  }
  fprintf(stderr, "capdb-server listening on %s (storage=%s)\n",
          cfg.zListen, cfg.zStorage ? cfg.zStorage : "legacy");
  capdb_server_run(gServer);
  capdb_server_stop(gServer);
  gServer = 0;
  return 0;
}

#else
#include <stdio.h>
int main(void){
  fprintf(stderr, "capdb-server requires CAPDB_ENABLE_NETWORK "
                  "and CAPDB_ENABLE_POOL\n");
  return 1;
}
#endif
