
#if !defined(__CAPDB_POOL_H_) && defined(CAPDB_ENABLE_POOL)
#define __CAPDB_POOL_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

#include "capdb.h"

/*
** CAPI3REF: Connection Pool Handle
**
** An instance of this object manages a set of [database connection | database
** connections] to a single database file.  Use [capdb_pool_open()] to create
** a pool and [capdb_pool_close()] to destroy it.
*/
typedef struct capdb_pool capdb_pool;

/*
** CAPI3REF: Connection Pool Configuration Object
**
** An instance of this structure configures a new connection pool.  Any
** member set to zero (or NULL for zVfs) is replaced with a library default
** when passed to [capdb_pool_open()].
**
** ^The default values are:
** <ul>
** <li> minSize = 1
** <li> maxSize = 8
** <li> busyTimeoutMs = 5000
** <li> flags = CAPDB_POOL_DEFAULT_WAL | CAPDB_POOL_SERIALIZE_WRITES
**             | CAPDB_POOL_RESET_ON_RELEASE
** <li> openFlags = 0 (no extra flags beyond READWRITE and CREATE)
** <li> zVfs = NULL (use default VFS)
** </ul>
*/
typedef struct capdb_pool_config {
  int minSize;              /* Target number of idle connections */
  int maxSize;              /* Maximum number of open connections */
  int busyTimeoutMs;        /* Per-connection busy timeout in milliseconds */
  unsigned flags;           /* CAPDB_POOL_* bitmask */
  unsigned openFlags;       /* Extra flags for capdb_open_v2() */
  const char *zVfs;         /* Optional VFS name */
} capdb_pool_config;

/*
** CAPI3REF: Connection Pool Configuration Flags
**
** These bit values may be ORed together for the flags field of
** [capdb_pool_config].
*/
#define CAPDB_POOL_DEFAULT_WAL        0x0001  /* PRAGMA journal_mode=WAL */
#define CAPDB_POOL_SERIALIZE_WRITES   0x0002  /* One WRITE checkout at a time */
#define CAPDB_POOL_RESET_ON_RELEASE   0x0004  /* Roll back on release */
#define CAPDB_POOL_DEFER_WRITE_BEGIN  0x0008  /* BEGIN IMMEDIATE on demand */

/*
** CAPI3REF: Connection Pool Access Mode
**
** The second argument to [capdb_pool_acquire()] must be one of these
** values.
*/
typedef enum {
  CAPDB_POOL_READ = 0,
  CAPDB_POOL_WRITE = 1
} capdb_pool_mode;

/*
** CAPI3REF: Open A Connection Pool
** CONSTRUCTOR: capdb_pool
**
** ^Create a new connection pool for the database file zFilename.
** ^If pCfg is NULL, default configuration values are used.
** ^On success, *ppPool is written with a pointer to the new pool object
** and CAPDB_OK is returned.
**
** ^Each connection in the pool uses a private page cache (shared-cache mode
** is never enabled).  File-backed databases are the intended use case;
** :memory: databases are not supported because each connection would receive
** a separate empty database.
**
** ^Only the main database file named by zFilename is managed.  ATTACHed
** databases are not supported in this version of the pool API.
*/
int capdb_pool_open(
  const char *zFilename,
  const capdb_pool_config *pCfg,
  capdb_pool **ppPool
);

/*
** CAPI3REF: Close A Connection Pool
** DESTRUCTOR: capdb_pool
**
** ^Close a connection pool previously created by [capdb_pool_open()].
** ^All connections must be released before the pool can be closed.
** ^If any connection is still checked out, this routine returns
** CAPDB_BUSY without closing the pool.
**
** ^After a successful close, the pool pointer must not be used.
*/
int capdb_pool_close(capdb_pool *pPool);

/*
** CAPI3REF: Check Out A Database Connection From A Pool
**
** ^Obtain a [database connection] from the pool pPool.  ^The connection
** must later be returned with [capdb_pool_release()].
**
** ^If mode is CAPDB_POOL_READ, any idle connection may be returned and
** multiple READ checkouts may be active at the same time (subject to maxSize).
**
** ^If mode is CAPDB_POOL_WRITE and the pool was created with
** CAPDB_POOL_SERIALIZE_WRITES, at most one WRITE checkout is active at a
** time.  ^When a write connection is checked out, BEGIN IMMEDIATE is executed
** so that lock acquisition happens during acquire rather than on the first
** write statement.
**
** ^The waitMs parameter controls how long to wait when a connection is not
** immediately available.  ^If waitMs is 0, the routine returns CAPDB_BUSY
** without waiting.  ^If waitMs is negative, the routine waits until a
** connection becomes available.  ^Otherwise waitMs is the maximum number of
** milliseconds to wait.
**
** ^On success, *ppDb is set to a database handle and CAPDB_OK is returned.
** ^The caller must not call [capdb_close()] on a checked-out handle; use
** [capdb_pool_release()] instead.
*/
int capdb_pool_acquire(
  capdb_pool *pPool,
  capdb_pool_mode mode,
  int waitMs,
  capdb **ppDb
);

/*
** CAPI3REF: Return A Database Connection To A Pool
**
** ^Return a database connection previously obtained from
** [capdb_pool_acquire()] to its pool.
**
** ^If the pool was created with CAPDB_POOL_RESET_ON_RELEASE, any open
** transaction is rolled back and unfinalized prepared statements are
** finalized before the connection is returned to the idle list.  Callers
** that perform writes during a WRITE checkout should invoke COMMIT before
** [capdb_pool_release()] to persist their changes.
**
** ^If CAPDB_POOL_RESET_ON_RELEASE is not set and the connection still has
** unfinalized prepared statements, this routine returns CAPDB_MISUSE.
**
** ^It is an error to pass a database handle that was not obtained from the
** given pool.
*/
int capdb_pool_release(capdb_pool *pPool, capdb *db);

/*
** CAPI3REF: Connection Pool Statistics
**
** ^Query statistics about a connection pool.  Any output pointer may be NULL
** if that statistic is not needed.
**
** ^(*pnIdle) is set to the number of connections currently in the idle list.
** ^(*pnActive) is set to the number of checked-out connections.
** ^(*pnTotal) is set to the total number of connections owned by the pool.
*/
int capdb_pool_stats(
  capdb_pool *pPool,
  int *pnIdle,
  int *pnActive,
  int *pnTotal
);

#ifdef __cplusplus
}  /* end of the 'extern "C"' block */
#endif

#endif /* __CAPDB_POOL_H_ */
