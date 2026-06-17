
#if !defined(__CAPDB_REP_RECOVERY_H_) && defined(CAPDB_ENABLE_REPLICATION)
#define __CAPDB_REP_RECOVERY_H_ 1

#include "../store/capdb_store.h"

#ifdef __cplusplus
extern "C" {
#endif

int capdb_rep_recovery_replay_dir(capdb_volume *pVol);

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_REP_RECOVERY_H_ */
