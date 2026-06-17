
#if !defined(__CAPDB_NETWORK_H_) && defined(CAPDB_ENABLE_NETWORK)
#define __CAPDB_NETWORK_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

#define CAPDB_PROTO_VERSION     1
#define CAPDB_DEFAULT_PORT      5432
#define CAPDB_MAX_FRAME_SIZE    (16*1024*1024)
#define CAPDB_DEFAULT_LISTEN    "127.0.0.1:5432"
#define CAPDB_HANDSHAKE_TIMEOUT_MS 15000
#define CAPDB_IDLE_TIMEOUT_MS      (15*60*1000)

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_NETWORK_H_ */
