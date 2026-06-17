/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This header file defines the interface that the sqlite page cache
** subsystem.  The page cache subsystem reads and writes a file a page
** at a time and provides a journal for rollback.
*/

#ifndef CAPDB_PAGER_H
#define CAPDB_PAGER_H

/*
** Default maximum size for persistent journal files. A negative 
** value means no limit. This value may be overridden using the 
** capdbPagerJournalSizeLimit() API. See also "PRAGMA journal_size_limit".
*/
#ifndef CAPDB_DEFAULT_JOURNAL_SIZE_LIMIT
  #define CAPDB_DEFAULT_JOURNAL_SIZE_LIMIT -1
#endif

/*
** The type used to represent a page number.  The first page in a file
** is called page 1.  0 is used to represent "not a page".
*/
typedef u32 Pgno;

/*
** Each open file is managed by a separate instance of the "Pager" structure.
*/
typedef struct Pager Pager;

/*
** Handle type for pages.
*/
typedef struct PgHdr DbPage;

/*
** Page number PAGER_SJ_PGNO is never used in an SQLite database (it is
** reserved for working around a windows/posix incompatibility). It is
** used in the journal to signify that the remainder of the journal file 
** is devoted to storing a super-journal name - there are no more pages to
** roll back. See comments for function writeSuperJournal() in pager.c 
** for details.
*/
#define PAGER_SJ_PGNO_COMPUTED(x) ((Pgno)((PENDING_BYTE/((x)->pageSize))+1))
#define PAGER_SJ_PGNO(x)          ((x)->lckPgno)

/*
** Allowed values for the flags parameter to capdbPagerOpen().
**
** NOTE: These values must match the corresponding BTREE_ values in btree.h.
*/
#define PAGER_OMIT_JOURNAL  0x0001    /* Do not use a rollback journal */
#define PAGER_MEMORY        0x0002    /* In-memory database */

/*
** Valid values for the second argument to capdbPagerLockingMode().
*/
#define PAGER_LOCKINGMODE_QUERY      -1
#define PAGER_LOCKINGMODE_NORMAL      0
#define PAGER_LOCKINGMODE_EXCLUSIVE   1

/*
** Numeric constants that encode the journalmode.
**
** The numeric values encoded here (other than PAGER_JOURNALMODE_QUERY)
** are exposed in the API via the "PRAGMA journal_mode" command and
** therefore cannot be changed without a compatibility break.
*/
#define PAGER_JOURNALMODE_QUERY     (-1)  /* Query the value of journalmode */
#define PAGER_JOURNALMODE_DELETE      0   /* Commit by deleting journal file */
#define PAGER_JOURNALMODE_PERSIST     1   /* Commit by zeroing journal header */
#define PAGER_JOURNALMODE_OFF         2   /* Journal omitted.  */
#define PAGER_JOURNALMODE_TRUNCATE    3   /* Commit by truncating journal */
#define PAGER_JOURNALMODE_MEMORY      4   /* In-memory journal file */
#define PAGER_JOURNALMODE_WAL         5   /* Use write-ahead logging */

#define isWalMode(x) ((x)==PAGER_JOURNALMODE_WAL)

/*
** The argument to this macro is a file descriptor (type capdb_file*).
** Return 0 if it is not open, or non-zero (but not 1) if it is.
**
** This is so that expressions can be written as:
**
**   if( isOpen(pPager->jfd) ){ ...
**
** instead of
**
**   if( pPager->jfd->pMethods ){ ...
*/
#define isOpen(pFd) ((pFd)->pMethods!=0)

/*
** Flags that make up the mask passed to capdbPagerGet().
*/
#define PAGER_GET_NOCONTENT     0x01  /* Do not load data from disk */
#define PAGER_GET_READONLY      0x02  /* Read-only page is acceptable */

/*
** Flags for capdbPagerSetFlags()
**
** Value constraints (enforced via assert()):
**    PAGER_FULLFSYNC      == CAPDB_FullFSync
**    PAGER_CKPT_FULLFSYNC == CAPDB_CkptFullFSync
**    PAGER_CACHE_SPILL    == CAPDB_CacheSpill
*/
#define PAGER_SYNCHRONOUS_OFF       0x01  /* PRAGMA synchronous=OFF */
#define PAGER_SYNCHRONOUS_NORMAL    0x02  /* PRAGMA synchronous=NORMAL */
#define PAGER_SYNCHRONOUS_FULL      0x03  /* PRAGMA synchronous=FULL */
#define PAGER_SYNCHRONOUS_EXTRA     0x04  /* PRAGMA synchronous=EXTRA */
#define PAGER_SYNCHRONOUS_MASK      0x07  /* Mask for four values above */
#define PAGER_FULLFSYNC             0x08  /* PRAGMA fullfsync=ON */
#define PAGER_CKPT_FULLFSYNC        0x10  /* PRAGMA checkpoint_fullfsync=ON */
#define PAGER_CACHESPILL            0x20  /* PRAGMA cache_spill=ON */
#define PAGER_FLAGS_MASK            0x38  /* All above except SYNCHRONOUS */

/*
** The remainder of this file contains the declarations of the functions
** that make up the Pager sub-system API. See source code comments for 
** a detailed description of each routine.
*/

/* Open and close a Pager connection. */ 
int capdbPagerOpen(
  capdb_vfs*,
  Pager **ppPager,
  const char*,
  int,
  int,
  int,
  void(*)(DbPage*)
);
int capdbPagerClose(Pager *pPager, capdb*);
int capdbPagerReadFileheader(Pager*, int, unsigned char*);

/* Functions used to configure a Pager object. */
void capdbPagerSetBusyHandler(Pager*, int(*)(void *), void *);
int capdbPagerSetPagesize(Pager*, u32*, int);
Pgno capdbPagerMaxPageCount(Pager*, Pgno);
void capdbPagerSetCachesize(Pager*, int);
int capdbPagerSetSpillsize(Pager*, int);
void capdbPagerSetMmapLimit(Pager *, capdb_int64);
void capdbPagerShrink(Pager*);
void capdbPagerSetFlags(Pager*,unsigned);
int capdbPagerLockingMode(Pager *, int);
int capdbPagerSetJournalMode(Pager *, int);
int capdbPagerGetJournalMode(Pager*);
int capdbPagerOkToChangeJournalMode(Pager*);
i64 capdbPagerJournalSizeLimit(Pager *, i64);
capdb_backup **capdbPagerBackupPtr(Pager*);
int capdbPagerFlush(Pager*);

/* Functions used to obtain and release page references. */ 
int capdbPagerGet(Pager *pPager, Pgno pgno, DbPage **ppPage, int clrFlag);
DbPage *capdbPagerLookup(Pager *pPager, Pgno pgno);
void capdbPagerRef(DbPage*);
void capdbPagerUnref(DbPage*);
void capdbPagerUnrefNotNull(DbPage*);
void capdbPagerUnrefPageOne(DbPage*);

/* Operations on page references. */
int capdbPagerWrite(DbPage*);
void capdbPagerDontWrite(DbPage*);
int capdbPagerMovepage(Pager*,DbPage*,Pgno,int);
int capdbPagerPageRefcount(DbPage*);
void *capdbPagerGetData(DbPage *); 
void *capdbPagerGetExtra(DbPage *); 

/* Functions used to manage pager transactions and savepoints. */
void capdbPagerPagecount(Pager*, int*);
int capdbPagerBegin(Pager*, int exFlag, int);
int capdbPagerCommitPhaseOne(Pager*,const char *zSuper, int);
int capdbPagerExclusiveLock(Pager*);
int capdbPagerSync(Pager *pPager, const char *zSuper);
int capdbPagerCommitPhaseTwo(Pager*);
int capdbPagerRollback(Pager*);
int capdbPagerOpenSavepoint(Pager *pPager, int n);
int capdbPagerSavepoint(Pager *pPager, int op, int iSavepoint);
int capdbPagerSharedLock(Pager *pPager);

#ifndef CAPDB_OMIT_WAL
  int capdbPagerCheckpoint(Pager *pPager, capdb*, int, int*, int*);
  int capdbPagerWalSupported(Pager *pPager);
  int capdbPagerWalCallback(Pager *pPager);
  int capdbPagerOpenWal(Pager *pPager, int *pisOpen);
  int capdbPagerCloseWal(Pager *pPager, capdb*);
# ifdef CAPDB_ENABLE_SNAPSHOT
  int capdbPagerSnapshotGet(Pager*, capdb_snapshot **ppSnapshot);
  int capdbPagerSnapshotOpen(Pager*, capdb_snapshot *pSnapshot);
  int capdbPagerSnapshotRecover(Pager *pPager);
  int capdbPagerSnapshotCheck(Pager *pPager, capdb_snapshot *pSnapshot);
  void capdbPagerSnapshotUnlock(Pager *pPager);
# endif
#endif

#if !defined(CAPDB_OMIT_WAL) && defined(CAPDB_ENABLE_SETLK_TIMEOUT)
  int capdbPagerWalWriteLock(Pager*, int);
  void capdbPagerWalDb(Pager*, capdb*);
#else
# define capdbPagerWalWriteLock(y,z) CAPDB_OK
# define capdbPagerWalDb(x,y)
#endif

#ifdef CAPDB_DIRECT_OVERFLOW_READ
  int capdbPagerDirectReadOk(Pager *pPager, Pgno pgno);
#endif

#ifdef CAPDB_ENABLE_ZIPVFS
  int capdbPagerWalFramesize(Pager *pPager);
#endif

/* Functions used to query pager state and configuration. */
u8 capdbPagerIsreadonly(Pager*);
u32 capdbPagerDataVersion(Pager*);
#ifdef CAPDB_DEBUG
  int capdbPagerRefcount(Pager*);
#endif
int capdbPagerMemUsed(Pager*);
const char *capdbPagerFilename(const Pager*, int);
capdb_vfs *capdbPagerVfs(Pager*);
capdb_file *capdbPagerFile(Pager*);
capdb_file *capdbPagerJrnlFile(Pager*);
const char *capdbPagerJournalname(Pager*);
void *capdbPagerTempSpace(Pager*);
int capdbPagerIsMemdb(Pager*);
void capdbPagerCacheStat(Pager *, int, int, u64*);
void capdbPagerClearCache(Pager*);
int capdbSectorSize(capdb_file *);

/* Functions used to truncate the database file. */
void capdbPagerTruncateImage(Pager*,Pgno);

void capdbPagerRekey(DbPage*, Pgno, u16);

/* Functions to support testing and debugging. */
#if !defined(NDEBUG) || defined(CAPDB_TEST)
  Pgno capdbPagerPagenumber(DbPage*);
  int capdbPagerIswriteable(DbPage*);
#endif
#ifdef CAPDB_TEST
  int *capdbPagerStats(Pager*);
  void capdbPagerRefdump(Pager*);
  void disable_simulated_io_errors(void);
  void enable_simulated_io_errors(void);
#else
# define disable_simulated_io_errors()
# define enable_simulated_io_errors()
#endif

#if defined(CAPDB_USE_SEH) && !defined(CAPDB_OMIT_WAL)
int capdbPagerWalSystemErrno(Pager*);
#endif

#endif /* CAPDB_PAGER_H */
