
#if !defined(__CAPDB_STORE_H_) && defined(CAPDB_ENABLE_STORE)
#define __CAPDB_STORE_H_ 1

#include "capdb_store_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct capdb_volume capdb_volume;

#define CAPDB_VOLUME_OPEN_CREATE  0x01
#define CAPDB_VOLUME_OPEN_RDONLY  0x02

#define CAPDB_STORE_SYNC_FULL     0x01

int  capdb_store_init(void);
void capdb_store_shutdown(void);

int  capdb_volume_open(const char *zPath, int flags, capdb_volume **pp);
int  capdb_volume_prepare(const char *zPath, int flags);
void capdb_volume_close(capdb_volume *p);

int  capdb_volume_read_page(capdb_volume *p, unsigned pgno, void *buf, int sz);
int  capdb_volume_write_page(capdb_volume *p, unsigned pgno,
                             const void *buf, int sz);

int  capdb_volume_append_wal(capdb_volume *p, const void *walData, int n,
                             unsigned long long walOffset,
                             unsigned long long *pLsn);
int  capdb_volume_replicate_sql_main(capdb_volume *p,
                                     unsigned long long *pLsn);
int  capdb_volume_checkpoint(capdb_volume *p, unsigned long long *pSnapLsn);
int  capdb_volume_sync(capdb_volume *p, int flags);

unsigned long long capdb_volume_lsn(capdb_volume *p);
unsigned long long capdb_volume_applied_lsn(capdb_volume *p);
int  capdb_volume_generation(capdb_volume *p);
int  capdb_volume_set_generation(capdb_volume *p, int gen);

const char *capdb_volume_path(capdb_volume *p);
const char *capdb_volume_main_db_path(capdb_volume *p);
const char *capdb_volume_wal_dir(capdb_volume *p);

unsigned capdb_store_crc32(const void *pData, int n);

int  capdb_volume_export_db(capdb_volume *p, const char *zOutPath);
int  capdb_volume_snapshot(capdb_volume *p, unsigned long long lsn,
                           char *zOutDir, int nOutDir);

int  capdb_store_vfs_register(const char *zVolumeRoot, int makeDefault);
void capdb_store_vfs_shutdown(void);
const char *capdb_store_vfs_name(void);

int  capdb_store_vol_id_valid(const char *zVolId);

#if defined(CAPDB_ENABLE_REPLICATION)
struct capdb_rep_sender;
void capdb_store_set_rep_sender(struct capdb_rep_sender *p);
struct capdb_rep_sender *capdb_store_rep_sender(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __CAPDB_STORE_H_ */
