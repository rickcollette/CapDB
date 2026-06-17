/*
** 2007 August 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains the common header for all mutex implementations.
** The capdbInt.h header #includes this file so that it is available
** to all source files.  We break it out in an effort to keep the code
** better organized.
**
** NOTE:  source files should *not* #include this header file directly.
** Source files should #include the capdbInt.h file and let that file
** include this one indirectly.
*/


/*
** Figure out what version of the code to use.  The choices are
**
**   CAPDB_MUTEX_OMIT         No mutex logic.  Not even stubs.  The
**                             mutexes implementation cannot be overridden
**                             at start-time.
**
**   CAPDB_MUTEX_NOOP         For single-threaded applications.  No
**                             mutual exclusion is provided.  But this
**                             implementation can be overridden at
**                             start-time.
**
**   CAPDB_MUTEX_PTHREADS     For multi-threaded applications on Unix.
**
**   CAPDB_MUTEX_W32          For multi-threaded applications on Win32.
*/
#if !CAPDB_THREADSAFE
# define CAPDB_MUTEX_OMIT
#endif
#if CAPDB_THREADSAFE && !defined(CAPDB_MUTEX_NOOP)
#  if CAPDB_OS_UNIX
#    define CAPDB_MUTEX_PTHREADS
#  elif CAPDB_OS_WIN
#    define CAPDB_MUTEX_W32
#  else
#    define CAPDB_MUTEX_NOOP
#  endif
#endif

#ifdef CAPDB_MUTEX_OMIT
/*
** If this is a no-op implementation, implement everything as macros.
*/
#define capdb_mutex_alloc(X)    ((capdb_mutex*)8)
#define capdb_mutex_free(X)
#define capdb_mutex_enter(X)    
#define capdb_mutex_try(X)      CAPDB_OK
#define capdb_mutex_leave(X)    
#define capdb_mutex_held(X)     ((void)(X),1)
#define capdb_mutex_notheld(X)  ((void)(X),1)
#define capdbMutexAlloc(X)      ((capdb_mutex*)8)
#define capdbMutexInit()        CAPDB_OK
#define capdbMutexEnd()
#define MUTEX_LOGIC(X)
#else
#define MUTEX_LOGIC(X)            X
int capdb_mutex_held(capdb_mutex*);
#endif /* defined(CAPDB_MUTEX_OMIT) */
