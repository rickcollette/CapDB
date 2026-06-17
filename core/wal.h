/*
** 2010 February 1
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This header file defines the interface to the write-ahead logging 
** system. Refer to the comments below and the header comment attached to 
** the implementation of each function in log.c for further details.
*/

#ifndef CAPDB_WAL_H
#define CAPDB_WAL_H

#include "capdbInt.h"

/* Macros for extracting appropriate sync flags for either transaction
** commits (WAL_SYNC_FLAGS(X)) or for checkpoint ops (CKPT_SYNC_FLAGS(X)):
*/
#define WAL_SYNC_FLAGS(X)   ((X)&0x03)
#define CKPT_SYNC_FLAGS(X)  (((X)>>2)&0x03)

#ifdef CAPDB_OMIT_WAL
# define capdbWalOpen(x,y,z)                   0
# define capdbWalLimit(x,y)
# define capdbWalClose(v,w,x,y,z)              0
# define capdbWalBeginReadTransaction(y,z)     0
# define capdbWalEndReadTransaction(z)
# define capdbWalDbsize(y)                     0
# define capdbWalBeginWriteTransaction(y)      0
# define capdbWalEndWriteTransaction(x)        0
# define capdbWalUndo(x,y,z)                   0
# define capdbWalSavepoint(y,z)
# define capdbWalSavepointUndo(y,z)            0
# define capdbWalFrames(u,v,w,x,y,z)           0
# define capdbWalCheckpoint(q,r,s,t,u,v,w,x,y,z) 0
# define capdbWalCallback(z)                   0
# define capdbWalExclusiveMode(y,z)            0
# define capdbWalHeapMemory(z)                 0
# define capdbWalFramesize(z)                  0
# define capdbWalFindFrame(x,y,z)              0
# define capdbWalFile(x)                       0
# undef CAPDB_USE_SEH
#else

#define WAL_SAVEPOINT_NDATA 4

/* Connection to a write-ahead log (WAL) file. 
** There is one object of this type for each pager. 
*/
typedef struct Wal Wal;

/* Open and close a connection to a write-ahead log. */
int capdbWalOpen(capdb_vfs*, capdb_file*, const char *, int, i64, Wal**);
int capdbWalClose(Wal *pWal, capdb*, int sync_flags, int, u8 *);

/* Set the limiting size of a WAL file. */
void capdbWalLimit(Wal*, i64);

/* Used by readers to open (lock) and close (unlock) a snapshot.  A 
** snapshot is like a read-transaction.  It is the state of the database
** at an instant in time.  capdbWalOpenSnapshot gets a read lock and
** preserves the current state even if the other threads or processes
** write to or checkpoint the WAL.  capdbWalCloseSnapshot() closes the
** transaction and releases the lock.
*/
int capdbWalBeginReadTransaction(Wal *pWal, int *);
void capdbWalEndReadTransaction(Wal *pWal);

/* Read a page from the write-ahead log, if it is present. */
int capdbWalFindFrame(Wal *, Pgno, u32 *);
int capdbWalReadFrame(Wal *, u32, int, u8 *);

/* If the WAL is not empty, return the size of the database. */
Pgno capdbWalDbsize(Wal *pWal);

/* Obtain or release the WRITER lock. */
int capdbWalBeginWriteTransaction(Wal *pWal);
int capdbWalEndWriteTransaction(Wal *pWal);

/* Undo any frames written (but not committed) to the log */
int capdbWalUndo(Wal *pWal, int (*xUndo)(void *, Pgno), void *pUndoCtx);

/* Return an integer that records the current (uncommitted) write
** position in the WAL */
void capdbWalSavepoint(Wal *pWal, u32 *aWalData);

/* Move the write position of the WAL back to iFrame.  Called in
** response to a ROLLBACK TO command. */
int capdbWalSavepointUndo(Wal *pWal, u32 *aWalData);

/* Write a frame or frames to the log. */
int capdbWalFrames(Wal *pWal, int, PgHdr *, Pgno, int, int);

/* Copy pages from the log to the database file */ 
int capdbWalCheckpoint(
  Wal *pWal,                      /* Write-ahead log connection */
  capdb *db,                    /* Check this handle's interrupt flag */
  int eMode,                      /* One of PASSIVE, FULL and RESTART */
  int (*xBusy)(void*),            /* Function to call when busy */
  void *pBusyArg,                 /* Context argument for xBusyHandler */
  int sync_flags,                 /* Flags to sync db file with (or 0) */
  int nBuf,                       /* Size of buffer nBuf */
  u8 *zBuf,                       /* Temporary buffer to use */
  int *pnLog,                     /* OUT: Number of frames in WAL */
  int *pnCkpt                     /* OUT: Number of backfilled frames in WAL */
);

/* Return the value to pass to a capdb_wal_hook callback, the
** number of frames in the WAL at the point of the last commit since
** capdbWalCallback() was called.  If no commits have occurred since
** the last call, then return 0.
*/
int capdbWalCallback(Wal *pWal);

/* Tell the wal layer that an EXCLUSIVE lock has been obtained (or released)
** by the pager layer on the database file.
*/
int capdbWalExclusiveMode(Wal *pWal, int op);

/* Return true if the argument is non-NULL and the WAL module is using
** heap-memory for the wal-index. Otherwise, if the argument is NULL or the
** WAL module is using shared-memory, return false. 
*/
int capdbWalHeapMemory(Wal *pWal);

#ifdef CAPDB_ENABLE_SNAPSHOT
int capdbWalSnapshotGet(Wal *pWal, capdb_snapshot **ppSnapshot);
void capdbWalSnapshotOpen(Wal *pWal, capdb_snapshot *pSnapshot);
int capdbWalSnapshotRecover(Wal *pWal);
int capdbWalSnapshotCheck(Wal *pWal, capdb_snapshot *pSnapshot);
void capdbWalSnapshotUnlock(Wal *pWal);
#endif

#ifdef CAPDB_ENABLE_ZIPVFS
/* If the WAL file is not empty, return the number of bytes of content
** stored in each frame (i.e. the db page-size when the WAL was created).
*/
int capdbWalFramesize(Wal *pWal);
#endif

/* Return the capdb_file object for the WAL file */
capdb_file *capdbWalFile(Wal *pWal);

#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
int capdbWalWriteLock(Wal *pWal, int bLock);
void capdbWalDb(Wal *pWal, capdb *db);
#endif

#ifdef CAPDB_USE_SEH
int capdbWalSystemErrno(Wal*);
#endif

#endif /* ifndef CAPDB_OMIT_WAL */
#endif /* CAPDB_WAL_H */
