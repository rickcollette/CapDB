
#if !defined(__CAPDB_CLUSTER_H_) && defined(CAPDB_ENABLE_STORE)
#define __CAPDB_CLUSTER_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

#define CAPDB_CLUSTER_ROLE_PRIMARY   1
#define CAPDB_CLUSTER_ROLE_REPLICA   2
#define CAPDB_CLUSTER_ROLE_DEMOTED   3
#define CAPDB_CLUSTER_ROLE_RECOVERING 4

typedef struct capdb_cluster_status {
  int role;
  int generation;
  unsigned long long lsn;
  unsigned long long applied_lsn;
  unsigned long long lag_bytes;
  const char *zVolumePath;
} capdb_cluster_status;

int  capdb_cluster_role_name(int role, char *zBuf, int nBuf);
int  capdb_cluster_promote(const char *zVolumePath, int waitMs);
int  capdb_cluster_demote(const char *zVolumePath);
int  capdb_cluster_status_fill(const char *zVolumePath, capdb_cluster_status *p);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_CLUSTER_H_ */
