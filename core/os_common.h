/*
** 2004 May 22
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
** This file contains macros and a little bit of code that is common to
** all of the platform-specific files (os_*.c) and is #included into those
** files.
**
** This file should be #included by the os_*.c files only.  It is not a
** general purpose header file.
*/
#ifndef _OS_COMMON_H_
#define _OS_COMMON_H_

/*
** At least two bugs have slipped in because we changed the MEMORY_DEBUG
** macro to CAPDB_DEBUG and some older makefiles have not yet made the
** switch.  The following code should catch this problem at compile-time.
*/
#ifdef MEMORY_DEBUG
# error "The MEMORY_DEBUG macro is obsolete.  Use CAPDB_DEBUG instead."
#endif

/*
** Macros for performance tracing.  Normally turned off.  Only works
** on i486 hardware.
*/
#ifdef CAPDB_PERFORMANCE_TRACE

static sqlite_uint64 g_start;
static sqlite_uint64 g_elapsed;
#define TIMER_START       g_start=capdbHwtime()
#define TIMER_END         g_elapsed=capdbHwtime()-g_start
#define TIMER_ELAPSED     g_elapsed
#else
#define TIMER_START
#define TIMER_END
#define TIMER_ELAPSED     ((sqlite_uint64)0)
#endif

/*
** If we compile with the CAPDB_TEST macro set, then the following block
** of code will give us the ability to simulate a disk I/O error.  This
** is used for testing the I/O recovery logic.
*/
#if defined(CAPDB_TEST)
extern int capdb_io_error_hit;
extern int capdb_io_error_hardhit;
extern int capdb_io_error_pending;
extern int capdb_io_error_persist;
extern int capdb_io_error_benign;
extern int capdb_diskfull_pending;
extern int capdb_diskfull;
#define SimulateIOErrorBenign(X) capdb_io_error_benign=(X)
#define SimulateIOError(CODE)  \
  if( (capdb_io_error_persist && capdb_io_error_hit) \
       || capdb_io_error_pending-- == 1 )  \
              { local_ioerr(); CODE; }
static void local_ioerr(){
  IOTRACE(("IOERR\n"));
  capdb_io_error_hit++;
  if( !capdb_io_error_benign ) capdb_io_error_hardhit++;
}
#define SimulateDiskfullError(CODE) \
   if( capdb_diskfull_pending ){ \
     if( capdb_diskfull_pending == 1 ){ \
       local_ioerr(); \
       capdb_diskfull = 1; \
       capdb_io_error_hit = 1; \
       CODE; \
     }else{ \
       capdb_diskfull_pending--; \
     } \
   }
#else
#define SimulateIOErrorBenign(X)
#define SimulateIOError(A)
#define SimulateDiskfullError(A)
#endif /* defined(CAPDB_TEST) */

/*
** When testing, keep a count of the number of open files.
*/
#if defined(CAPDB_TEST)
extern int capdb_open_file_count;
#define OpenCounter(X)  capdb_open_file_count+=(X)
#else
#define OpenCounter(X)
#endif /* defined(CAPDB_TEST) */

#endif /* !defined(_OS_COMMON_H_) */
