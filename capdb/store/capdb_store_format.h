
#if !defined(__CAPDB_STORE_FORMAT_H_)
#define __CAPDB_STORE_FORMAT_H_ 1

#define CAPDB_STORE_FORMAT_VERSION  2
#define CAPDB_STORE_PAGE_SIZE_DEFAULT 4096
#define CAPDB_STORE_WAL_MAGIC         0x43574442u  /* "CWDB" */

#define CAPDB_STORE_DIR_DATA      "data"
#define CAPDB_STORE_DIR_WAL       "wal"
#define CAPDB_STORE_DIR_META      "meta"
#define CAPDB_STORE_DIR_PAGES     "pages"
#define CAPDB_STORE_DIR_SNAPSHOTS "snapshots"
#define CAPDB_STORE_MANIFEST      "manifest.json"
#define CAPDB_STORE_META_STATE    "meta/state"
#define CAPDB_STORE_MAIN_DB       "data/main.db"

#define CAPDB_STORE_VOL_ID_MAX    48

/* Sentinel wal_offset: payload is a full data/main.db snapshot (not a WAL slice). */
#define CAPDB_STORE_WAL_OFFSET_MAIN_DB  0xFFFFFFFFFFFFFFFFULL

typedef struct CapdbStoreWalHdr CapdbStoreWalHdr;
struct CapdbStoreWalHdr {
  unsigned magic;
  unsigned format;
  unsigned generation;
  unsigned long long lsn;
  unsigned long long prev_lsn;
  unsigned long long wal_offset;
  unsigned payload_len;
  unsigned crc32;
  char vol_id[CAPDB_STORE_VOL_ID_MAX];
};

#endif /* __CAPDB_STORE_FORMAT_H_ */
