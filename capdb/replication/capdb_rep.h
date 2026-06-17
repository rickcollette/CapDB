
#if !defined(__CAPDB_REP_H_) && defined(CAPDB_ENABLE_REPLICATION)
#define __CAPDB_REP_H_ 1

#include "../store/capdb_store.h"
#include "../store/capdb_store_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAPDB_REP_PROTO_VERSION  1

#define CAPDB_REP_HELLO          1
#define CAPDB_REP_HELLO_ACK      2
#define CAPDB_REP_AUTH           3
#define CAPDB_REP_AUTH_OK        4
#define CAPDB_REP_STREAM_START   5
#define CAPDB_REP_WAL_CHUNK      6
#define CAPDB_REP_ACK            7
#define CAPDB_REP_PING           8
#define CAPDB_REP_PONG           9
#define CAPDB_REP_ERROR          10

#define CAPDB_REP_SYNC_OFF       0
#define CAPDB_REP_SYNC_ON        1

typedef struct capdb_rep_sender capdb_rep_sender;
typedef struct capdb_rep_receiver capdb_rep_receiver;

typedef struct capdb_rep_config {
  const char *zListen;
  const char *zPrimaryHost;
  int iPrimaryPort;
  const char *zVolumePath;
  const char *zAuthToken;
  int bTls;
  void *pSslCtx;
  int syncMode;
  int walKeepSegments;
} capdb_rep_config;

int  capdb_rep_sender_start(const capdb_rep_config *pCfg, capdb_volume *pVol,
                           capdb_rep_sender **pp);
void capdb_rep_sender_stop(capdb_rep_sender *p);
int  capdb_rep_sender_wait_ack(capdb_rep_sender *p, unsigned long long lsn,
                               int timeoutMs);
void capdb_rep_sender_note_ack(capdb_rep_sender *p, unsigned long long lsn);
unsigned long long capdb_rep_sender_last_lsn(capdb_rep_sender *p);
capdb_rep_sender *capdb_rep_sender_global(void);
void capdb_rep_primary_on_wal(capdb_volume *pVol, const CapdbStoreWalHdr *hdr,
                              const void *payload, int nPayload);

int  capdb_rep_receiver_start(const capdb_rep_config *pCfg,
                              capdb_rep_receiver **pp);
int  capdb_rep_receiver_run_async(capdb_rep_receiver *p);
void capdb_rep_receiver_stop(capdb_rep_receiver *p);
int  capdb_rep_replica_apply_chunk(const char *zVolumeRoot, const void *hdr,
                                   int nHdr, const void *payload, int nPayload);
unsigned long long capdb_rep_receiver_lag_bytes(capdb_rep_receiver *p);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_REP_H_ */
