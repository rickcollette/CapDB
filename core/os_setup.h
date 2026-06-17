/*
** 2013 November 25
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
** This file contains pre-processor directives related to operating system
** detection and/or setup.
*/
#ifndef CAPDB_OS_SETUP_H
#define CAPDB_OS_SETUP_H

/*
** Figure out if we are dealing with Unix, Windows, or some other operating
** system.
**
** After the following block of preprocess macros, all of 
**
**    CAPDB_OS_KV
**    CAPDB_OS_OTHER
**    CAPDB_OS_UNIX
**    CAPDB_OS_WIN
**
** will defined to either 1 or 0. One of them will be 1. The others will be 0.
** If none of the macros are initially defined, then select either
** CAPDB_OS_UNIX or CAPDB_OS_WIN depending on the target platform.
**
** If CAPDB_OS_OTHER=1 is specified at compile-time, then the application
** must provide its own VFS implementation together with capdb_os_init()
** and capdb_os_end() routines.
*/
#if CAPDB_OS_KV+1<=1  && CAPDB_OS_OTHER+1<=1 &&  \
    CAPDB_OS_WIN+1<=1 && CAPDB_OS_UNIX+1<=1 
#  if defined(_WIN32) || defined(WIN32) || defined(__CYGWIN__) || \
          defined(__MINGW32__) || defined(__BORLANDC__)
#    define CAPDB_OS_WIN 1
#    define CAPDB_OS_UNIX 0
#  else
#    define CAPDB_OS_WIN 0
#    define CAPDB_OS_UNIX 1
#  endif
#endif
#if CAPDB_OS_OTHER+1>1
#  undef CAPDB_OS_KV
#  define CAPDB_OS_KV 0
#  undef CAPDB_OS_UNIX
#  define CAPDB_OS_UNIX 0
#  undef CAPDB_OS_WIN
#  define CAPDB_OS_WIN 0
#endif
#if CAPDB_OS_KV+1>1
#  undef CAPDB_OS_OTHER
#  define CAPDB_OS_OTHER 0
#  undef CAPDB_OS_UNIX
#  define CAPDB_OS_UNIX 0
#  undef CAPDB_OS_WIN
#  define CAPDB_OS_WIN 0
#  define CAPDB_OMIT_LOAD_EXTENSION 1
#  define CAPDB_OMIT_WAL 1
#  define CAPDB_OMIT_DEPRECATED 1
#  undef CAPDB_TEMP_STORE
#  define CAPDB_TEMP_STORE 3  /* Always use memory for temporary storage */
#  define CAPDB_DQS 0
#  define CAPDB_OMIT_SHARED_CACHE 1
#  define CAPDB_OMIT_AUTOINIT 1
#endif
#if CAPDB_OS_UNIX+1>1
#  undef CAPDB_OS_KV
#  define CAPDB_OS_KV 0
#  undef CAPDB_OS_OTHER
#  define CAPDB_OS_OTHER 0
#  undef CAPDB_OS_WIN
#  define CAPDB_OS_WIN 0
#endif
#if CAPDB_OS_WIN+1>1
#  undef CAPDB_OS_KV
#  define CAPDB_OS_KV 0
#  undef CAPDB_OS_OTHER
#  define CAPDB_OS_OTHER 0
#  undef CAPDB_OS_UNIX
#  define CAPDB_OS_UNIX 0
#endif


#endif /* CAPDB_OS_SETUP_H */
