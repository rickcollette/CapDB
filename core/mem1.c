/*
** 2007 August 14
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
** This file contains low-level memory allocation drivers for when
** SQLite will use the standard C-library malloc/realloc/free interface
** to obtain the memory it needs.
**
** This file contains implementations of the low-level memory allocation
** routines specified in the capdb_mem_methods object.  The content of
** this file is only used if CAPDB_SYSTEM_MALLOC is defined.  The
** CAPDB_SYSTEM_MALLOC macro is defined automatically if neither the
** CAPDB_MEMDEBUG nor the CAPDB_WIN32_MALLOC macros are defined.  The
** default configuration is to use memory allocation routines in this
** file.
**
** C-preprocessor macro summary:
**
**    HAVE_MALLOC_USABLE_SIZE     The configure script sets this symbol if
**                                the malloc_usable_size() interface exists
**                                on the target platform.  Or, this symbol
**                                can be set manually, if desired.
**                                If an equivalent interface exists by
**                                a different name, using a separate -D
**                                option to rename it.
**
**    CAPDB_WITHOUT_ZONEMALLOC   Some older macs lack support for the zone
**                                memory allocator.  Set this symbol to enable
**                                building on older macs.
**
**    CAPDB_WITHOUT_MSIZE        Set this symbol to disable the use of
**                                _msize() on windows systems.  This might
**                                be necessary when compiling for Delphi,
**                                for example.
*/
#include "capdbInt.h"

/*
** This version of the memory allocator is the default.  It is
** used when no other memory allocator is specified using compile-time
** macros.
*/
#ifdef CAPDB_SYSTEM_MALLOC
#if defined(__APPLE__) && !defined(CAPDB_WITHOUT_ZONEMALLOC)

/*
** Use the zone allocator available on apple products unless the
** CAPDB_WITHOUT_ZONEMALLOC symbol is defined.
*/
#include <sys/sysctl.h>
#include <malloc/malloc.h>
#ifdef CAPDB_MIGHT_BE_SINGLE_CORE
#include <libkern/OSAtomic.h>
#endif /* CAPDB_MIGHT_BE_SINGLE_CORE */
static malloc_zone_t* _sqliteZone_;
#define CAPDB_MALLOC(x) malloc_zone_malloc(_sqliteZone_, (x))
#define CAPDB_FREE(x) malloc_zone_free(_sqliteZone_, (x));
#define CAPDB_REALLOC(x,y) malloc_zone_realloc(_sqliteZone_, (x), (y))
#define CAPDB_MALLOCSIZE(x) \
        (_sqliteZone_ ? _sqliteZone_->size(_sqliteZone_,x) : malloc_size(x))

#else /* if not __APPLE__ */

/*
** Use standard C library malloc and free on non-Apple systems.  
** Also used by Apple systems if CAPDB_WITHOUT_ZONEMALLOC is defined.
*/
#define CAPDB_MALLOC(x)             malloc(x)
#define CAPDB_FREE(x)               free(x)
#define CAPDB_REALLOC(x,y)          realloc((x),(y))

/*
** The malloc.h header file is needed for malloc_usable_size() function
** on some systems (e.g. Linux).
*/
#if HAVE_MALLOC_H && HAVE_MALLOC_USABLE_SIZE
#  define CAPDB_USE_MALLOC_H 1
#  define CAPDB_USE_MALLOC_USABLE_SIZE 1
/*
** The MSVCRT has malloc_usable_size(), but it is called _msize().  The
** use of _msize() is automatic, but can be disabled by compiling with
** -DCAPDB_WITHOUT_MSIZE.  Using the _msize() function also requires
** the malloc.h header file.
*/
#elif defined(_MSC_VER) && !defined(CAPDB_WITHOUT_MSIZE)
#  define CAPDB_USE_MALLOC_H
#  define CAPDB_USE_MSIZE
#endif

/*
** Include the malloc.h header file, if necessary.  Also set define macro
** CAPDB_MALLOCSIZE to the appropriate function name, which is _msize()
** for MSVC and malloc_usable_size() for most other systems (e.g. Linux).
** The memory size function can always be overridden manually by defining
** the macro CAPDB_MALLOCSIZE to the desired function name.
*/
#if defined(CAPDB_USE_MALLOC_H)
#  include <malloc.h>
#  if defined(CAPDB_USE_MALLOC_USABLE_SIZE)
#    if !defined(CAPDB_MALLOCSIZE)
#      define CAPDB_MALLOCSIZE(x)   malloc_usable_size(x)
#    endif
#  elif defined(CAPDB_USE_MSIZE)
#    if !defined(CAPDB_MALLOCSIZE)
#      define CAPDB_MALLOCSIZE      _msize
#    endif
#  endif
#endif /* defined(CAPDB_USE_MALLOC_H) */

#endif /* __APPLE__ or not __APPLE__ */

/*
** Like malloc(), but remember the size of the allocation
** so that we can find it later using capdbMemSize().
**
** For this low-level routine, we are guaranteed that nByte>0 because
** cases of nByte<=0 will be intercepted and dealt with by higher level
** routines.
*/
static void *capdbMemMalloc(int nByte){
#ifdef CAPDB_MALLOCSIZE
  void *p;
  testcase( ROUND8(nByte)==nByte );
  p = CAPDB_MALLOC( nByte );
  if( p==0 ){
    testcase( capdbGlobalConfig.xLog!=0 );
    capdb_log(CAPDB_NOMEM, "failed to allocate %u bytes of memory", nByte);
  }
  return p;
#else
  capdb_int64 *p;
  assert( nByte>0 );
  testcase( ROUND8(nByte)!=nByte );
  p = CAPDB_MALLOC( nByte+8 );
  if( p ){
    p[0] = nByte;
    p++;
  }else{
    testcase( capdbGlobalConfig.xLog!=0 );
    capdb_log(CAPDB_NOMEM, "failed to allocate %u bytes of memory", nByte);
  }
  return (void *)p;
#endif
}

/*
** Like free() but works for allocations obtained from capdbMemMalloc()
** or capdbMemRealloc().
**
** For this low-level routine, we already know that pPrior!=0 since
** cases where pPrior==0 will have been intercepted and dealt with
** by higher-level routines.
*/
static void capdbMemFree(void *pPrior){
#ifdef CAPDB_MALLOCSIZE
  CAPDB_FREE(pPrior);
#else
  capdb_int64 *p = (capdb_int64*)pPrior;
  assert( pPrior!=0 );
  p--;
  CAPDB_FREE(p);
#endif
}

/*
** Report the allocated size of a prior return from xMalloc()
** or xRealloc().
*/
static int capdbMemSize(void *pPrior){
#ifdef CAPDB_MALLOCSIZE
  assert( pPrior!=0 );
  return (int)CAPDB_MALLOCSIZE(pPrior);
#else
  capdb_int64 *p;
  assert( pPrior!=0 );
  p = (capdb_int64*)pPrior;
  p--;
  return (int)p[0];
#endif
}

/*
** Like realloc().  Resize an allocation previously obtained from
** capdbMemMalloc().
**
** For this low-level interface, we know that pPrior!=0.  Cases where
** pPrior==0 while have been intercepted by higher-level routine and
** redirected to xMalloc.  Similarly, we know that nByte>0 because
** cases where nByte<=0 will have been intercepted by higher-level
** routines and redirected to xFree.
*/
static void *capdbMemRealloc(void *pPrior, int nByte){
#ifdef CAPDB_MALLOCSIZE
  void *p = CAPDB_REALLOC(pPrior, nByte);
  if( p==0 ){
    testcase( capdbGlobalConfig.xLog!=0 );
    capdb_log(CAPDB_NOMEM,
      "failed memory resize %u to %u bytes",
      CAPDB_MALLOCSIZE(pPrior), nByte);
  }
  return p;
#else
  capdb_int64 *p = (capdb_int64*)pPrior;
  assert( pPrior!=0 && nByte>0 );
  assert( nByte==ROUND8(nByte) ); /* EV: R-46199-30249 */
  p--;
  p = CAPDB_REALLOC(p, nByte+8 );
  if( p ){
    p[0] = nByte;
    p++;
  }else{
    testcase( capdbGlobalConfig.xLog!=0 );
    capdb_log(CAPDB_NOMEM,
      "failed memory resize %u to %u bytes",
      capdbMemSize(pPrior), nByte);
  }
  return (void*)p;
#endif
}

/*
** Round up a request size to the next valid allocation size.
*/
static int capdbMemRoundup(int n){
  return ROUND8(n);
}

/*
** Initialize this module.
*/
static int capdbMemInit(void *NotUsed){
#if defined(__APPLE__) && !defined(CAPDB_WITHOUT_ZONEMALLOC)
  int cpuCount;
  size_t len;
  if( _sqliteZone_ ){
    return CAPDB_OK;
  }
  len = sizeof(cpuCount);
  /* One usually wants to use hw.activecpu for MT decisions, but not here */
  sysctlbyname("hw.ncpu", &cpuCount, &len, NULL, 0);
  if( cpuCount>1 ){
    /* defer MT decisions to system malloc */
    _sqliteZone_ = malloc_default_zone();
  }else{
    /* only 1 core, use our own zone to contention over global locks,
    ** e.g. we have our own dedicated locks */
    _sqliteZone_ = malloc_create_zone(4096, 0);
    malloc_set_zone_name(_sqliteZone_, "Sqlite_Heap");
  }
#endif /*  defined(__APPLE__) && !defined(CAPDB_WITHOUT_ZONEMALLOC) */
  UNUSED_PARAMETER(NotUsed);
  return CAPDB_OK;
}

/*
** Deinitialize this module.
*/
static void capdbMemShutdown(void *NotUsed){
  UNUSED_PARAMETER(NotUsed);
  return;
}

/*
** This routine is the only routine in this file with external linkage.
**
** Populate the low-level memory allocation function pointers in
** capdbGlobalConfig.m with pointers to the routines in this file.
*/
void capdbMemSetDefault(void){
  static const capdb_mem_methods defaultMethods = {
     capdbMemMalloc,
     capdbMemFree,
     capdbMemRealloc,
     capdbMemSize,
     capdbMemRoundup,
     capdbMemInit,
     capdbMemShutdown,
     0
  };
  capdb_config(CAPDB_CONFIG_MALLOC, &defaultMethods);
}

#endif /* CAPDB_SYSTEM_MALLOC */
