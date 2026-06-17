/*
** 2001 September 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This header file (together with is companion C source-code file
** "os.c") attempt to abstract the underlying operating system so that
** the SQLite library will work on both POSIX and windows systems.
**
** This header file is #include-ed by capdbInt.h and thus ends up
** being included by every source file.
*/
#ifndef _CAPDB_OS_H_
#define _CAPDB_OS_H_

/*
** Attempt to automatically detect the operating system and setup the
** necessary pre-processor macros for it.
*/
#include "os_setup.h"

/* If the SET_FULLSYNC macro is not defined above, then make it
** a no-op
*/
#ifndef SET_FULLSYNC
# define SET_FULLSYNC(x,y)
#endif

/* Maximum pathname length.  Note: FILENAME_MAX defined by stdio.h
*/
#ifndef CAPDB_MAX_PATHLEN
# define CAPDB_MAX_PATHLEN FILENAME_MAX
#endif

/* Maximum number of symlinks that will be resolved while trying to
** expand a filename in xFullPathname() in the VFS.
*/
#ifndef CAPDB_MAX_SYMLINK
# define CAPDB_MAX_SYMLINK 200
#endif

/*
** The default size of a disk sector
*/
#ifndef CAPDB_DEFAULT_SECTOR_SIZE
# define CAPDB_DEFAULT_SECTOR_SIZE 4096
#endif

/*
** Temporary files are named starting with this prefix followed by 16 random
** alphanumeric characters, and no file extension. They are stored in the
** OS's standard temporary file directory, and are deleted prior to exit.
** If sqlite is being embedded in another program, you may wish to change the
** prefix to reflect your program's name, so that if your program exits
** prematurely, old temporary files can be easily identified. This can be done
** using -DCAPDB_TEMP_FILE_PREFIX=myprefix_ on the compiler command line.
**
** 2006-10-31:  The default prefix used to be "sqlite_".  But then
** Mcafee started using SQLite in their anti-virus product and it
** started putting files with the "sqlite" name in the c:/temp folder.
** This annoyed many windows users.  Those users would then do a 
** Google search for "sqlite", find the telephone numbers of the
** developers and call to wake them up at night and complain.
** For this reason, the default name prefix is changed to be "sqlite" 
** spelled backwards.  So the temp files are still identified, but
** anybody smart enough to figure out the code is also likely smart
** enough to know that calling the developer will not help get rid
** of the file.
*/
#ifndef CAPDB_TEMP_FILE_PREFIX
# define CAPDB_TEMP_FILE_PREFIX "etilqs_"
#endif

/*
** The following values may be passed as the second argument to
** capdbOsLock(). The various locks exhibit the following semantics:
**
** SHARED:    Any number of processes may hold a SHARED lock simultaneously.
** RESERVED:  A single process may hold a RESERVED lock on a file at
**            any time. Other processes may hold and obtain new SHARED locks.
** PENDING:   A single process may hold a PENDING lock on a file at
**            any one time. Existing SHARED locks may persist, but no new
**            SHARED locks may be obtained by other processes.
** EXCLUSIVE: An EXCLUSIVE lock precludes all other locks.
**
** PENDING_LOCK may not be passed directly to capdbOsLock(). Instead, a
** process that requests an EXCLUSIVE lock may actually obtain a PENDING
** lock. This can be upgraded to an EXCLUSIVE lock by a subsequent call to
** capdbOsLock().
*/
#define NO_LOCK         0
#define SHARED_LOCK     1
#define RESERVED_LOCK   2
#define PENDING_LOCK    3
#define EXCLUSIVE_LOCK  4

/*
** File Locking Notes:  (Mostly about windows but also some info for Unix)
**
** We cannot use LockFileEx() or UnlockFileEx() on Win95/98/ME because
** those functions are not available.  So we use only LockFile() and
** UnlockFile().
**
** LockFile() prevents not just writing but also reading by other processes.
** A SHARED_LOCK is obtained by locking a single randomly-chosen 
** byte out of a specific range of bytes. The lock byte is obtained at 
** random so two separate readers can probably access the file at the 
** same time, unless they are unlucky and choose the same lock byte.
** An EXCLUSIVE_LOCK is obtained by locking all bytes in the range.
** There can only be one writer.  A RESERVED_LOCK is obtained by locking
** a single byte of the file that is designated as the reserved lock byte.
** A PENDING_LOCK is obtained by locking a designated byte different from
** the RESERVED_LOCK byte.
**
** On WinNT/2K/XP systems, LockFileEx() and UnlockFileEx() are available,
** which means we can use reader/writer locks.  When reader/writer locks
** are used, the lock is placed on the same range of bytes that is used
** for probabilistic locking in Win95/98/ME.  Hence, the locking scheme
** will support two or more Win95 readers or two or more WinNT readers.
** But a single Win95 reader will lock out all WinNT readers and a single
** WinNT reader will lock out all other Win95 readers.
**
** The following #defines specify the range of bytes used for locking.
** SHARED_SIZE is the number of bytes available in the pool from which
** a random byte is selected for a shared lock.  The pool of bytes for
** shared locks begins at SHARED_FIRST. 
**
** The same locking strategy and
** byte ranges are used for Unix.  This leaves open the possibility of having
** clients on win95, winNT, and unix all talking to the same shared file
** and all locking correctly.  To do so would require that samba (or whatever
** tool is being used for file sharing) implements locks correctly between
** windows and unix.  I'm guessing that isn't likely to happen, but by
** using the same locking range we are at least open to the possibility.
**
** Locking in windows is manditory.  For this reason, we cannot store
** actual data in the bytes used for locking.  The pager never allocates
** the pages involved in locking therefore.  SHARED_SIZE is selected so
** that all locks will fit on a single page even at the minimum page size.
** PENDING_BYTE defines the beginning of the locks.  By default PENDING_BYTE
** is set high so that we don't have to allocate an unused page except
** for very large databases.  But one should test the page skipping logic 
** by setting PENDING_BYTE low and running the entire regression suite.
**
** Changing the value of PENDING_BYTE results in a subtly incompatible
** file format.  Depending on how it is changed, you might not notice
** the incompatibility right away, even running a full regression test.
** The default location of PENDING_BYTE is the first byte past the
** 1GB boundary.
**
*/
#ifdef CAPDB_OMIT_WSD
# define PENDING_BYTE     (0x40000000)
#else
# define PENDING_BYTE      capdbPendingByte
#endif
#define RESERVED_BYTE     (PENDING_BYTE+1)
#define SHARED_FIRST      (PENDING_BYTE+2)
#define SHARED_SIZE       510

/*
** Wrapper around OS specific capdb_os_init() function.
*/
int capdbOsInit(void);

/* 
** Functions for accessing capdb_file methods 
*/
void capdbOsClose(capdb_file*);
int capdbOsRead(capdb_file*, void*, int amt, i64 offset);
int capdbOsWrite(capdb_file*, const void*, int amt, i64 offset);
int capdbOsTruncate(capdb_file*, i64 size);
int capdbOsSync(capdb_file*, int);
int capdbOsFileSize(capdb_file*, i64 *pSize);
int capdbOsLock(capdb_file*, int);
int capdbOsUnlock(capdb_file*, int);
int capdbOsCheckReservedLock(capdb_file *id, int *pResOut);
int capdbOsFileControl(capdb_file*,int,void*);
void capdbOsFileControlHint(capdb_file*,int,void*);
#define CAPDB_FCNTL_DB_UNCHANGED 0xca093fa0
int capdbOsSectorSize(capdb_file *id);
int capdbOsDeviceCharacteristics(capdb_file *id);
#ifndef CAPDB_OMIT_WAL
int capdbOsShmMap(capdb_file *,int,int,int,void volatile **);
int capdbOsShmLock(capdb_file *id, int, int, int);
void capdbOsShmBarrier(capdb_file *id);
int capdbOsShmUnmap(capdb_file *id, int);
#endif /* CAPDB_OMIT_WAL */
int capdbOsFetch(capdb_file *id, i64, int, void **);
int capdbOsUnfetch(capdb_file *, i64, void *);


/* 
** Functions for accessing capdb_vfs methods 
*/
int capdbOsOpen(capdb_vfs *, const char *, capdb_file*, int, int *);
int capdbOsDelete(capdb_vfs *, const char *, int);
int capdbOsAccess(capdb_vfs *, const char *, int, int *pResOut);
int capdbOsFullPathname(capdb_vfs *, const char *, int, char *);
#ifndef CAPDB_OMIT_LOAD_EXTENSION
void *capdbOsDlOpen(capdb_vfs *, const char *);
void capdbOsDlError(capdb_vfs *, int, char *);
void (*capdbOsDlSym(capdb_vfs *, void *, const char *))(void);
void capdbOsDlClose(capdb_vfs *, void *);
#endif /* CAPDB_OMIT_LOAD_EXTENSION */
int capdbOsRandomness(capdb_vfs *, int, char *);
int capdbOsSleep(capdb_vfs *, int);
int capdbOsGetLastError(capdb_vfs*);
int capdbOsCurrentTimeInt64(capdb_vfs *, capdb_int64*);

/*
** Convenience functions for opening and closing files using 
** capdb_malloc() to obtain space for the file-handle structure.
*/
int capdbOsOpenMalloc(capdb_vfs *, const char *, capdb_file **, int,int*);
void capdbOsCloseFree(capdb_file *);

#endif /* _CAPDB_OS_H_ */
