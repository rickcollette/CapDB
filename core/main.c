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
** Main file for the SQLite library.  The routines in this file
** implement the programmer interface to the library.  Routines in
** other files are for internal use by SQLite and should not be
** accessed by users of the library.
*/
#include "capdbInt.h"

#ifdef CAPDB_ENABLE_FTS3
# include "fts3.h"
#endif
#ifdef CAPDB_ENABLE_RTREE
# include "rtree.h"
#endif
#if defined(CAPDB_ENABLE_ICU) || defined(CAPDB_ENABLE_ICU_COLLATIONS)
# include "sqliteicu.h"
#endif

/*
** This is an extension initializer that is a no-op and always
** succeeds, except that it fails if the fault-simulation is set
** to 500.
*/
static int capdbTestExtInit(capdb *db){
  (void)db;
  return capdbFaultSim(500);
}


/*
** Forward declarations of external module initializer functions
** for modules that need them.
*/
#ifdef CAPDB_ENABLE_FTS5
int capdbFts5Init(capdb*);
#endif
#ifdef CAPDB_ENABLE_STMTVTAB
int capdbStmtVtabInit(capdb*);
#endif
#ifdef CAPDB_EXTRA_AUTOEXT
int CAPDB_EXTRA_AUTOEXT(capdb*);
#endif
/*
** An array of pointers to extension initializer functions for
** built-in extensions.
*/
static int (*const capdbBuiltinExtensions[])(capdb*) = {
#ifdef CAPDB_ENABLE_FTS3
  capdbFts3Init,
#endif
#ifdef CAPDB_ENABLE_FTS5
  capdbFts5Init,
#endif
#if defined(CAPDB_ENABLE_ICU) || defined(CAPDB_ENABLE_ICU_COLLATIONS)
  capdbIcuInit,
#endif
#ifdef CAPDB_ENABLE_RTREE
  capdbRtreeInit,
#endif
#ifdef CAPDB_ENABLE_DBPAGE_VTAB
  capdbDbpageRegister,
#endif
#ifdef CAPDB_ENABLE_DBSTAT_VTAB
  capdbDbstatRegister,
#endif
  capdbTestExtInit,
#ifdef CAPDB_ENABLE_STMTVTAB
  capdbStmtVtabInit,
#endif
#ifdef CAPDB_ENABLE_BYTECODE_VTAB
  capdbVdbeBytecodeVtabInit,
#endif
#ifdef CAPDB_EXTRA_AUTOEXT
  CAPDB_EXTRA_AUTOEXT,
#endif
};

#ifndef CAPDB_AMALGAMATION
/* IMPLEMENTATION-OF: R-46656-45156 The capdb_version[] string constant
** contains the text of CAPDB_VERSION macro.
*/
const char capdb_version[] = CAPDB_VERSION;
#endif

/* IMPLEMENTATION-OF: R-53536-42575 The capdb_libversion() function returns
** a pointer to the to the capdb_version[] string constant.
*/
const char *capdb_libversion(void){ return capdb_version; }

/* IMPLEMENTATION-OF: R-25063-23286 The capdb_sourceid() function returns a
** pointer to a string constant whose value is the same as the
** CAPDB_SOURCE_ID C preprocessor macro. Except if SQLite is built using
** an edited copy of the amalgamation, then the last four characters of
** the hash might be different from CAPDB_SOURCE_ID.
*/
const char *capdb_sourceid(void){ return CAPDB_SOURCE_ID; }

/* IMPLEMENTATION-OF: R-35210-63508 The capdb_libversion_number() function
** returns an integer equal to CAPDB_VERSION_NUMBER.
*/
int capdb_libversion_number(void){ return CAPDB_VERSION_NUMBER; }

/* IMPLEMENTATION-OF: R-20790-14025 The capdb_threadsafe() function returns
** zero if and only if SQLite was compiled with mutexing code omitted due to
** the CAPDB_THREADSAFE compile-time option being set to 0.
*/
int capdb_threadsafe(void){ return CAPDB_THREADSAFE; }

/*
** When compiling the test fixture or with debugging enabled (on Win32),
** this variable being set to non-zero will cause OSTRACE macros to emit
** extra diagnostic information.
*/
#ifdef CAPDB_HAVE_OS_TRACE
# ifndef CAPDB_DEBUG_OS_TRACE
#   define CAPDB_DEBUG_OS_TRACE 0
# endif
  int capdbOSTrace = CAPDB_DEBUG_OS_TRACE;
#endif

#if !defined(CAPDB_OMIT_TRACE) && defined(CAPDB_ENABLE_IOTRACE)
/*
** If the following function pointer is not NULL and if
** CAPDB_ENABLE_IOTRACE is enabled, then messages describing
** I/O active are written using this function.  These messages
** are intended for debugging activity only.
*/
CAPDB_API void (CAPDB_CDECL *capdbIoTrace)(const char*, ...) = 0;
#endif

/*
** If the following global variable points to a string which is the
** name of a directory, then that directory will be used to store
** temporary files.
**
** See also the "PRAGMA temp_store_directory" SQL command.
*/
char *capdb_temp_directory = 0;

/*
** If the following global variable points to a string which is the
** name of a directory, then that directory will be used to store
** all database files specified with a relative pathname.
**
** See also the "PRAGMA data_store_directory" SQL command.
*/
char *capdb_data_directory = 0;

/*
** Initialize SQLite. 
**
** This routine must be called to initialize the memory allocation,
** VFS, and mutex subsystems prior to doing any serious work with
** SQLite.  But as long as you do not compile with CAPDB_OMIT_AUTOINIT
** this routine will be called automatically by key routines such as
** capdb_open(). 
**
** This routine is a no-op except on its very first call for the process,
** or for the first call after a call to capdb_shutdown.
**
** The first thread to call this routine runs the initialization to
** completion.  If subsequent threads call this routine before the first
** thread has finished the initialization process, then the subsequent
** threads must block until the first thread finishes with the initialization.
**
** The first thread might call this routine recursively.  Recursive
** calls to this routine should not block, of course.  Otherwise the
** initialization process would never complete.
**
** Let X be the first thread to enter this routine.  Let Y be some other
** thread.  Then while the initial invocation of this routine by X is
** incomplete, it is required that:
**
**    *  Calls to this routine from Y must block until the outer-most
**       call by X completes.
**
**    *  Recursive calls to this routine from thread X return immediately
**       without blocking.
*/
int capdb_initialize(void){
  MUTEX_LOGIC( capdb_mutex *pMainMtx; )      /* The main static mutex */
  int rc;                                      /* Result code */
#ifdef CAPDB_EXTRA_INIT
  int bRunExtraInit = 0;                       /* Extra initialization needed */
#endif

#ifdef CAPDB_OMIT_WSD
  rc = capdb_wsd_init(4096, 24);
  if( rc!=CAPDB_OK ){
    return rc;
  }
#endif

  /* If the following assert() fails on some obscure processor/compiler
  ** combination, the work-around is to set the correct pointer
  ** size at compile-time using -DCAPDB_PTRSIZE=n compile-time option */
  assert( CAPDB_PTRSIZE==sizeof(char*) );

  /* If SQLite is already completely initialized, then this call
  ** to capdb_initialize() should be a no-op.  But the initialization
  ** must be complete.  So isInit must not be set until the very end
  ** of this routine.
  */
  if( capdbGlobalConfig.isInit ){
    capdbMemoryBarrier();
    return CAPDB_OK;
  }

  /* Make sure the mutex subsystem is initialized.  If unable to
  ** initialize the mutex subsystem, return early with the error.
  ** If the system is so sick that we are unable to allocate a mutex,
  ** there is not much SQLite is going to be able to do.
  **
  ** The mutex subsystem must take care of serializing its own
  ** initialization.
  */
  rc = capdbMutexInit();
  if( rc ) return rc;

  /* Initialize the malloc() system and the recursive pInitMutex mutex.
  ** This operation is protected by the STATIC_MAIN mutex.  Note that
  ** MutexAlloc() is called for a static mutex prior to initializing the
  ** malloc subsystem - this implies that the allocation of a static
  ** mutex must not require support from the malloc subsystem.
  */
  MUTEX_LOGIC( pMainMtx = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN); )
  capdb_mutex_enter(pMainMtx);
  capdbGlobalConfig.isMutexInit = 1;
  if( !capdbGlobalConfig.isMallocInit ){
    rc = capdbMallocInit();
  }
  if( rc==CAPDB_OK ){
    capdbGlobalConfig.isMallocInit = 1;
    if( !capdbGlobalConfig.pInitMutex ){
      capdbGlobalConfig.pInitMutex =
           capdbMutexAlloc(CAPDB_MUTEX_RECURSIVE);
      if( capdbGlobalConfig.bCoreMutex && !capdbGlobalConfig.pInitMutex ){
        rc = CAPDB_NOMEM_BKPT;
      }
    }
  }
  if( rc==CAPDB_OK ){
    capdbGlobalConfig.nRefInitMutex++;
  }
  capdb_mutex_leave(pMainMtx);

  /* If rc is not CAPDB_OK at this point, then either the malloc
  ** subsystem could not be initialized or the system failed to allocate
  ** the pInitMutex mutex. Return an error in either case.  */
  if( rc!=CAPDB_OK ){
    return rc;
  }

  /* Do the rest of the initialization under the recursive mutex so
  ** that we will be able to handle recursive calls into
  ** capdb_initialize().  The recursive calls normally come through
  ** capdb_os_init() when it invokes capdb_vfs_register(), but other
  ** recursive calls might also be possible.
  **
  ** IMPLEMENTATION-OF: R-00140-37445 SQLite automatically serializes calls
  ** to the xInit method, so the xInit method need not be threadsafe.
  **
  ** The following mutex is what serializes access to the appdef pcache xInit
  ** methods.  The capdb_pcache_methods.xInit() all is embedded in the
  ** call to capdbPcacheInitialize().
  */
  capdb_mutex_enter(capdbGlobalConfig.pInitMutex);
  if( capdbGlobalConfig.isInit==0 && capdbGlobalConfig.inProgress==0 ){
    capdbGlobalConfig.inProgress = 1;
#ifdef CAPDB_ENABLE_SQLLOG
    {
      extern void capdb_init_sqllog(void);
      capdb_init_sqllog();
    }
#endif
    memset(&capdbBuiltinFunctions, 0, sizeof(capdbBuiltinFunctions));
    capdbRegisterBuiltinFunctions();
    if( capdbGlobalConfig.isPCacheInit==0 ){
      rc = capdbPcacheInitialize();
    }
    if( rc==CAPDB_OK ){
      capdbGlobalConfig.isPCacheInit = 1;
      rc = capdbOsInit();
    }
#ifndef CAPDB_OMIT_DESERIALIZE
    if( rc==CAPDB_OK ){
      rc = capdbMemdbInit();
    }
#endif
    if( rc==CAPDB_OK ){
      capdbPCacheBufferSetup( capdbGlobalConfig.pPage,
          capdbGlobalConfig.szPage, capdbGlobalConfig.nPage);
#ifdef CAPDB_EXTRA_INIT_MUTEXED
      {
        int CAPDB_EXTRA_INIT_MUTEXED(const char*);
        rc = CAPDB_EXTRA_INIT_MUTEXED(0);
      }
#endif
    }
    if( rc==CAPDB_OK ){
      capdbMemoryBarrier();
      capdbGlobalConfig.isInit = 1;
#ifdef CAPDB_EXTRA_INIT
      bRunExtraInit = 1;
#endif
    }
    capdbGlobalConfig.inProgress = 0;
  }
  capdb_mutex_leave(capdbGlobalConfig.pInitMutex);

  /* Go back under the static mutex and clean up the recursive
  ** mutex to prevent a resource leak.
  */
  capdb_mutex_enter(pMainMtx);
  capdbGlobalConfig.nRefInitMutex--;
  if( capdbGlobalConfig.nRefInitMutex<=0 ){
    assert( capdbGlobalConfig.nRefInitMutex==0 );
    capdb_mutex_free(capdbGlobalConfig.pInitMutex);
    capdbGlobalConfig.pInitMutex = 0;
  }
  capdb_mutex_leave(pMainMtx);

  /* The following is just a sanity check to make sure SQLite has
  ** been compiled correctly.  It is important to run this code, but
  ** we don't want to run it too often and soak up CPU cycles for no
  ** reason.  So we run it once during initialization.
  */
#ifndef NDEBUG
#ifndef CAPDB_OMIT_FLOATING_POINT
  /* This section of code's only "output" is via assert() statements. */
  if( rc==CAPDB_OK ){
    u64 x = (((u64)1)<<63)-1;
    double y;
    assert(sizeof(x)==8);
    assert(sizeof(x)==sizeof(y));
    memcpy(&y, &x, 8);
    assert( capdbIsNaN(y) );
  }
#endif
#endif

  /* Do extra initialization steps requested by the CAPDB_EXTRA_INIT
  ** compile-time option.
  */
#ifdef CAPDB_EXTRA_INIT
  if( bRunExtraInit ){
    int CAPDB_EXTRA_INIT(const char*);
    rc = CAPDB_EXTRA_INIT(0);
  }
#endif
  return rc;
}

/*
** Undo the effects of capdb_initialize().  Must not be called while
** there are outstanding database connections or memory allocations or
** while any part of SQLite is otherwise in use in any thread.  This
** routine is not threadsafe.  But it is safe to invoke this routine
** on when SQLite is already shut down.  If SQLite is already shut down
** when this routine is invoked, then this routine is a harmless no-op.
*/
int capdb_shutdown(void){
#ifdef CAPDB_OMIT_WSD
  int rc = capdb_wsd_init(4096, 24);
  if( rc!=CAPDB_OK ){
    return rc;
  }
#endif

  if( capdbGlobalConfig.isInit ){
#ifdef CAPDB_EXTRA_SHUTDOWN
    void CAPDB_EXTRA_SHUTDOWN(void);
    CAPDB_EXTRA_SHUTDOWN();
#endif
    capdb_os_end();
    capdb_reset_auto_extension();
    capdbGlobalConfig.isInit = 0;
  }
  if( capdbGlobalConfig.isPCacheInit ){
    capdbPcacheShutdown();
    capdbGlobalConfig.isPCacheInit = 0;
  }
  if( capdbGlobalConfig.isMallocInit ){
    capdbMallocEnd();
    capdbGlobalConfig.isMallocInit = 0;

#ifndef CAPDB_OMIT_SHUTDOWN_DIRECTORIES
    /* The heap subsystem has now been shutdown and these values are supposed
    ** to be NULL or point to memory that was obtained from capdb_malloc(),
    ** which would rely on that heap subsystem; therefore, make sure these
    ** values cannot refer to heap memory that was just invalidated when the
    ** heap subsystem was shutdown.  This is only done if the current call to
    ** this function resulted in the heap subsystem actually being shutdown.
    */
    capdb_data_directory = 0;
    capdb_temp_directory = 0;
#endif
  }
  if( capdbGlobalConfig.isMutexInit ){
    capdbMutexEnd();
    capdbGlobalConfig.isMutexInit = 0;
  }

  return CAPDB_OK;
}

/*
** This API allows applications to modify the global configuration of
** the SQLite library at run-time.
**
** This routine should only be called when there are no outstanding
** database connections or memory allocations.  This routine is not
** threadsafe.  Failure to heed these warnings can lead to unpredictable
** behavior.
*/
int capdb_config(int op, ...){
  va_list ap;
  int rc = CAPDB_OK;

  /* capdb_config() normally returns CAPDB_MISUSE if it is invoked while
  ** the SQLite library is in use.  Except, a few selected opcodes
  ** are allowed.
  */
  if( capdbGlobalConfig.isInit ){
    static const u64 mAnytimeConfigOption = 0
       | MASKBIT64( CAPDB_CONFIG_LOG )
       | MASKBIT64( CAPDB_CONFIG_PCACHE_HDRSZ )
    ;
    if( op<0 || op>63 || (MASKBIT64(op) & mAnytimeConfigOption)==0 ){
      return CAPDB_MISUSE_BKPT;
    }
    testcase( op==CAPDB_CONFIG_LOG );
    testcase( op==CAPDB_CONFIG_PCACHE_HDRSZ );
  }

  va_start(ap, op);
  switch( op ){

    /* Mutex configuration options are only available in a threadsafe
    ** compile.
    */
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE>0  /* IMP: R-54466-46756 */
    case CAPDB_CONFIG_SINGLETHREAD: {
      /* EVIDENCE-OF: R-02748-19096 This option sets the threading mode to
      ** Single-thread. */
      capdbGlobalConfig.bCoreMutex = 0;  /* Disable mutex on core */
      capdbGlobalConfig.bFullMutex = 0;  /* Disable mutex on connections */
      break;
    }
#endif
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE>0 /* IMP: R-20520-54086 */
    case CAPDB_CONFIG_MULTITHREAD: {
      /* EVIDENCE-OF: R-14374-42468 This option sets the threading mode to
      ** Multi-thread. */
      capdbGlobalConfig.bCoreMutex = 1;  /* Enable mutex on core */
      capdbGlobalConfig.bFullMutex = 0;  /* Disable mutex on connections */
      break;
    }
#endif
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE>0 /* IMP: R-59593-21810 */
    case CAPDB_CONFIG_SERIALIZED: {
      /* EVIDENCE-OF: R-41220-51800 This option sets the threading mode to
      ** Serialized. */
      capdbGlobalConfig.bCoreMutex = 1;  /* Enable mutex on core */
      capdbGlobalConfig.bFullMutex = 1;  /* Enable mutex on connections */
      break;
    }
#endif
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE>0 /* IMP: R-63666-48755 */
    case CAPDB_CONFIG_MUTEX: {
      /* Specify an alternative mutex implementation */
      capdbGlobalConfig.mutex = *va_arg(ap, capdb_mutex_methods*);
      break;
    }
#endif
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE>0 /* IMP: R-14450-37597 */
    case CAPDB_CONFIG_GETMUTEX: {
      /* Retrieve the current mutex implementation */
      *va_arg(ap, capdb_mutex_methods*) = capdbGlobalConfig.mutex;
      break;
    }
#endif

    case CAPDB_CONFIG_MALLOC: {
      /* EVIDENCE-OF: R-55594-21030 The CAPDB_CONFIG_MALLOC option takes a
      ** single argument which is a pointer to an instance of the
      ** capdb_mem_methods structure. The argument specifies alternative
      ** low-level memory allocation routines to be used in place of the memory
      ** allocation routines built into SQLite. */
      capdbGlobalConfig.m = *va_arg(ap, capdb_mem_methods*);
      break;
    }
    case CAPDB_CONFIG_GETMALLOC: {
      /* EVIDENCE-OF: R-51213-46414 The CAPDB_CONFIG_GETMALLOC option takes a
      ** single argument which is a pointer to an instance of the
      ** capdb_mem_methods structure. The capdb_mem_methods structure is
      ** filled with the currently defined memory allocation routines. */
      if( capdbGlobalConfig.m.xMalloc==0 ) capdbMemSetDefault();
      *va_arg(ap, capdb_mem_methods*) = capdbGlobalConfig.m;
      break;
    }
    case CAPDB_CONFIG_MEMSTATUS: {
      assert( !capdbGlobalConfig.isInit );  /* Cannot change at runtime */
      /* EVIDENCE-OF: R-61275-35157 The CAPDB_CONFIG_MEMSTATUS option takes
      ** single argument of type int, interpreted as a boolean, which enables
      ** or disables the collection of memory allocation statistics. */
      capdbGlobalConfig.bMemstat = va_arg(ap, int);
      break;
    }
    case CAPDB_CONFIG_SMALL_MALLOC: {
      capdbGlobalConfig.bSmallMalloc = va_arg(ap, int)!=0;
      break;
    }
    case CAPDB_CONFIG_PAGECACHE: {
      /* EVIDENCE-OF: R-18761-36601 There are three arguments to
      ** CAPDB_CONFIG_PAGECACHE: A pointer to 8-byte aligned memory (pMem),
      ** the size of each page cache line (sz), and the number of cache lines
      ** (N). */
      capdbGlobalConfig.pPage = va_arg(ap, void*);
      capdbGlobalConfig.szPage = va_arg(ap, int);
      capdbGlobalConfig.nPage = va_arg(ap, int);
      break;
    }
    case CAPDB_CONFIG_PCACHE_HDRSZ: {
      /* EVIDENCE-OF: R-39100-27317 The CAPDB_CONFIG_PCACHE_HDRSZ option takes
      ** a single parameter which is a pointer to an integer and writes into
      ** that integer the number of extra bytes per page required for each page
      ** in CAPDB_CONFIG_PAGECACHE. */
      *va_arg(ap, int*) =
          capdbHeaderSizeBtree() +
          capdbHeaderSizePcache() +
          capdbHeaderSizePcache1();
      break;
    }

    case CAPDB_CONFIG_PCACHE: {
      /* no-op */
      break;
    }
    case CAPDB_CONFIG_GETPCACHE: {
      /* now an error */
      rc = CAPDB_ERROR;
      break;
    }

    case CAPDB_CONFIG_PCACHE2: {
      /* EVIDENCE-OF: R-63325-48378 The CAPDB_CONFIG_PCACHE2 option takes a
      ** single argument which is a pointer to an capdb_pcache_methods2
      ** object. This object specifies the interface to a custom page cache
      ** implementation. */
      capdbGlobalConfig.pcache2 = *va_arg(ap, capdb_pcache_methods2*);
      break;
    }
    case CAPDB_CONFIG_GETPCACHE2: {
      /* EVIDENCE-OF: R-22035-46182 The CAPDB_CONFIG_GETPCACHE2 option takes a
      ** single argument which is a pointer to an capdb_pcache_methods2
      ** object. SQLite copies of the current page cache implementation into
      ** that object. */
      if( capdbGlobalConfig.pcache2.xInit==0 ){
        capdbPCacheSetDefault();
      }
      *va_arg(ap, capdb_pcache_methods2*) = capdbGlobalConfig.pcache2;
      break;
    }

/* EVIDENCE-OF: R-06626-12911 The CAPDB_CONFIG_HEAP option is only
** available if SQLite is compiled with either CAPDB_ENABLE_MEMSYS3 or
** CAPDB_ENABLE_MEMSYS5 and returns CAPDB_ERROR if invoked otherwise. */
#if defined(CAPDB_ENABLE_MEMSYS3) || defined(CAPDB_ENABLE_MEMSYS5)
    case CAPDB_CONFIG_HEAP: {
      /* EVIDENCE-OF: R-19854-42126 There are three arguments to
      ** CAPDB_CONFIG_HEAP: An 8-byte aligned pointer to the memory, the
      ** number of bytes in the memory buffer, and the minimum allocation size.
      */
      capdbGlobalConfig.pHeap = va_arg(ap, void*);
      capdbGlobalConfig.nHeap = va_arg(ap, int);
      capdbGlobalConfig.mnReq = va_arg(ap, int);

      if( capdbGlobalConfig.mnReq<1 ){
        capdbGlobalConfig.mnReq = 1;
      }else if( capdbGlobalConfig.mnReq>(1<<12) ){
        /* cap min request size at 2^12 */
        capdbGlobalConfig.mnReq = (1<<12);
      }

      if( capdbGlobalConfig.pHeap==0 ){
        /* EVIDENCE-OF: R-49920-60189 If the first pointer (the memory pointer)
        ** is NULL, then SQLite reverts to using its default memory allocator
        ** (the system malloc() implementation), undoing any prior invocation of
        ** CAPDB_CONFIG_MALLOC.
        **
        ** Setting capdbGlobalConfig.m to all zeros will cause malloc to
        ** revert to its default implementation when capdb_initialize() is run
        */
        memset(&capdbGlobalConfig.m, 0, sizeof(capdbGlobalConfig.m));
      }else{
        /* EVIDENCE-OF: R-61006-08918 If the memory pointer is not NULL then the
        ** alternative memory allocator is engaged to handle all of SQLites
        ** memory allocation needs. */
#ifdef CAPDB_ENABLE_MEMSYS3
        capdbGlobalConfig.m = *capdbMemGetMemsys3();
#endif
#ifdef CAPDB_ENABLE_MEMSYS5
        capdbGlobalConfig.m = *capdbMemGetMemsys5();
#endif
      }
      break;
    }
#endif

    case CAPDB_CONFIG_LOOKASIDE: {
      capdbGlobalConfig.szLookaside = va_arg(ap, int);
      capdbGlobalConfig.nLookaside = va_arg(ap, int);
      break;
    }
   
    /* Record a pointer to the logger function and its first argument.
    ** The default is NULL.  Logging is disabled if the function pointer is
    ** NULL.
    */
    case CAPDB_CONFIG_LOG: {
      /* MSVC is picky about pulling func ptrs from va lists.
      ** http://support.microsoft.com/kb/47961
      ** capdbGlobalConfig.xLog = va_arg(ap, void(*)(void*,int,const char*));
      */
      typedef void(*LOGFUNC_t)(void*,int,const char*);
      LOGFUNC_t xLog = va_arg(ap, LOGFUNC_t);
      void *pLogArg = va_arg(ap, void*);
      AtomicStore(&capdbGlobalConfig.xLog, xLog);
      AtomicStore(&capdbGlobalConfig.pLogArg, pLogArg);
      break;
    }

    /* EVIDENCE-OF: R-55548-33817 The compile-time setting for URI filenames
    ** can be changed at start-time using the
    ** capdb_config(CAPDB_CONFIG_URI,1) or
    ** capdb_config(CAPDB_CONFIG_URI,0) configuration calls.
    */
    case CAPDB_CONFIG_URI: {
      /* EVIDENCE-OF: R-25451-61125 The CAPDB_CONFIG_URI option takes a single
      ** argument of type int. If non-zero, then URI handling is globally
      ** enabled. If the parameter is zero, then URI handling is globally
      ** disabled. */
      int bOpenUri = va_arg(ap, int);
      AtomicStore(&capdbGlobalConfig.bOpenUri, bOpenUri);
      break;
    }

    case CAPDB_CONFIG_COVERING_INDEX_SCAN: {
      /* EVIDENCE-OF: R-36592-02772 The CAPDB_CONFIG_COVERING_INDEX_SCAN
      ** option takes a single integer argument which is interpreted as a
      ** boolean in order to enable or disable the use of covering indices for
      ** full table scans in the query optimizer. */
      capdbGlobalConfig.bUseCis = va_arg(ap, int);
      break;
    }

#ifdef CAPDB_ENABLE_SQLLOG
    case CAPDB_CONFIG_SQLLOG: {
      typedef void(*SQLLOGFUNC_t)(void*, capdb*, const char*, int);
      capdbGlobalConfig.xSqllog = va_arg(ap, SQLLOGFUNC_t);
      capdbGlobalConfig.pSqllogArg = va_arg(ap, void *);
      break;
    }
#endif

    case CAPDB_CONFIG_MMAP_SIZE: {
      /* EVIDENCE-OF: R-58063-38258 CAPDB_CONFIG_MMAP_SIZE takes two 64-bit
      ** integer (capdb_int64) values that are the default mmap size limit
      ** (the default setting for PRAGMA mmap_size) and the maximum allowed
      ** mmap size limit. */
      capdb_int64 szMmap = va_arg(ap, capdb_int64);
      capdb_int64 mxMmap = va_arg(ap, capdb_int64);
      /* EVIDENCE-OF: R-53367-43190 If either argument to this option is
      ** negative, then that argument is changed to its compile-time default.
      **
      ** EVIDENCE-OF: R-34993-45031 The maximum allowed mmap size will be
      ** silently truncated if necessary so that it does not exceed the
      ** compile-time maximum mmap size set by the CAPDB_MAX_MMAP_SIZE
      ** compile-time option.
      */
      if( mxMmap<0 || mxMmap>CAPDB_MAX_MMAP_SIZE ){
        mxMmap = CAPDB_MAX_MMAP_SIZE;
      }
      if( szMmap<0 ) szMmap = CAPDB_DEFAULT_MMAP_SIZE;
      if( szMmap>mxMmap) szMmap = mxMmap;
      capdbGlobalConfig.mxMmap = mxMmap;
      capdbGlobalConfig.szMmap = szMmap;
      break;
    }

#if CAPDB_OS_WIN && defined(CAPDB_WIN32_MALLOC) /* IMP: R-04780-55815 */
    case CAPDB_CONFIG_WIN32_HEAPSIZE: {
      /* EVIDENCE-OF: R-34926-03360 CAPDB_CONFIG_WIN32_HEAPSIZE takes a 32-bit
      ** unsigned integer value that specifies the maximum size of the created
      ** heap. */
      capdbGlobalConfig.nHeap = va_arg(ap, int);
      break;
    }
#endif

    case CAPDB_CONFIG_PMASZ: {
      capdbGlobalConfig.szPma = va_arg(ap, unsigned int);
      break;
    }

    case CAPDB_CONFIG_STMTJRNL_SPILL: {
      capdbGlobalConfig.nStmtSpill = va_arg(ap, int);
      break;
    }

#ifdef CAPDB_ENABLE_SORTER_REFERENCES
    case CAPDB_CONFIG_SORTERREF_SIZE: {
      int iVal = va_arg(ap, int);
      if( iVal<0 ){
        iVal = CAPDB_DEFAULT_SORTERREF_SIZE;
      }
      capdbGlobalConfig.szSorterRef = (u32)iVal;
      break;
    }
#endif /* CAPDB_ENABLE_SORTER_REFERENCES */

#ifndef CAPDB_OMIT_DESERIALIZE
    case CAPDB_CONFIG_MEMDB_MAXSIZE: {
      capdbGlobalConfig.mxMemdbSize = va_arg(ap, capdb_int64);
      break;
    }
#endif /* CAPDB_OMIT_DESERIALIZE */

    case CAPDB_CONFIG_ROWID_IN_VIEW: {
      int *pVal = va_arg(ap,int*);
#ifdef CAPDB_ALLOW_ROWID_IN_VIEW
      if( 0==*pVal ) capdbGlobalConfig.mNoVisibleRowid = TF_NoVisibleRowid;
      if( 1==*pVal ) capdbGlobalConfig.mNoVisibleRowid = 0;
      *pVal = (capdbGlobalConfig.mNoVisibleRowid==0);
#else
      *pVal = 0;
#endif
      break;
    }

    default: {
      rc = CAPDB_ERROR;
      break;
    }
  }
  va_end(ap);
  return rc;
}

/*
** Set up the lookaside buffers for a database connection.
** Return CAPDB_OK on success. 
** If lookaside is already active, return CAPDB_BUSY.
**
** The sz parameter is the number of bytes in each lookaside slot.
** The cnt parameter is the number of slots.  If pBuf is NULL the
** space for the lookaside memory is obtained from capdb_malloc()
** or similar.  If pBuf is not NULL then it is sz*cnt bytes of memory
** to use for the lookaside memory.
*/
static int setupLookaside(
  capdb *db,    /* Database connection being configured */
  void *pBuf,     /* Memory to use for lookaside.  May be NULL */
  int sz,         /* Desired size of each lookaside memory slot */
  int cnt         /* Number of slots to allocate */
){
#ifndef CAPDB_OMIT_LOOKASIDE
  void *pStart;          /* Start of the lookaside buffer */
  capdb_int64 szAlloc; /* Total space set aside for lookaside memory */
  int nBig;              /* Number of full-size slots */
  int nSm;               /* Number smaller LOOKASIDE_SMALL-byte slots */
 
  if( capdbLookasideUsed(db,0)>0 ){
    return CAPDB_BUSY;
  }
  /* Free any existing lookaside buffer for this handle before
  ** allocating a new one so we don't have to have space for
  ** both at the same time.
  */
  if( db->lookaside.bMalloced ){
    capdb_free(db->lookaside.pStart);
  }
  /* The size of a lookaside slot after ROUNDDOWN8 needs to be larger
  ** than a pointer and small enough to fit in a u16.
  */
  sz = ROUNDDOWN8(sz);
  if( sz<=(int)sizeof(LookasideSlot*) ) sz = 0;
  if( sz>65528 ) sz = 65528;
  /* Count must be at least 1 to be useful, but not so large as to use
  ** more than 0x7fff0000 total bytes for lookaside. */
  if( cnt<1 ) cnt = 0;
  if( sz>0 && cnt>(0x7fff0000/sz) ) cnt = 0x7fff0000/sz;
  szAlloc = (i64)sz*(i64)cnt;
  if( szAlloc==0 ){
    sz = 0;
    pStart = 0;
  }else if( pBuf==0 ){
    capdbBeginBenignMalloc();
    pStart = capdbMalloc( szAlloc );
    capdbEndBenignMalloc();
    if( pStart ) szAlloc = capdbMallocSize(pStart);
  }else{
    pStart = pBuf;
  }
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
  if( sz>=LOOKASIDE_SMALL*3 ){
    nBig = szAlloc/(3*LOOKASIDE_SMALL+sz);
    nSm = (szAlloc - (i64)sz*(i64)nBig)/LOOKASIDE_SMALL;
  }else if( sz>=LOOKASIDE_SMALL*2 ){
    nBig = szAlloc/(LOOKASIDE_SMALL+sz);
    nSm = (szAlloc - (i64)sz*(i64)nBig)/LOOKASIDE_SMALL;
  }else
#endif /* CAPDB_OMIT_TWOSIZE_LOOKASIDE */
  if( sz>0 ){
    nBig = szAlloc/sz;
    nSm = 0;
  }else{
    nBig = nSm = 0;
  }
  db->lookaside.pStart = pStart;
  db->lookaside.pInit = 0;
  db->lookaside.pFree = 0;
  db->lookaside.sz = (u16)sz;
  db->lookaside.szTrue = (u16)sz;
  if( pStart ){
    int i;
    LookasideSlot *p;
    assert( sz > (int)sizeof(LookasideSlot*) );
    p = (LookasideSlot*)pStart;
    for(i=0; i<nBig; i++){
      p->pNext = db->lookaside.pInit;
      db->lookaside.pInit = p;
      p = (LookasideSlot*)&((u8*)p)[sz];
    }
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
    db->lookaside.pSmallInit = 0;
    db->lookaside.pSmallFree = 0;
    db->lookaside.pMiddle = p;
    for(i=0; i<nSm; i++){
      p->pNext = db->lookaside.pSmallInit;
      db->lookaside.pSmallInit = p;
      p = (LookasideSlot*)&((u8*)p)[LOOKASIDE_SMALL];
    }
#endif /* CAPDB_OMIT_TWOSIZE_LOOKASIDE */
    assert( ((uptr)p)<=szAlloc + (uptr)pStart );
    db->lookaside.pEnd = p;
    db->lookaside.bDisable = 0;
    db->lookaside.bMalloced = pBuf==0 ?1:0;
    db->lookaside.nSlot = nBig+nSm;
  }else{
    db->lookaside.pStart = 0;
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
    db->lookaside.pSmallInit = 0;
    db->lookaside.pSmallFree = 0;
    db->lookaside.pMiddle = 0;
#endif /* CAPDB_OMIT_TWOSIZE_LOOKASIDE */
    db->lookaside.pEnd = 0;
    db->lookaside.bDisable = 1;
    db->lookaside.sz = 0;
    db->lookaside.bMalloced = 0;
    db->lookaside.nSlot = 0;
  }
  db->lookaside.pTrueEnd = db->lookaside.pEnd;
  assert( capdbLookasideUsed(db,0)==0 );
#endif /* CAPDB_OMIT_LOOKASIDE */
  return CAPDB_OK;
}

/*
** Return the mutex associated with a database connection.
*/
capdb_mutex *capdb_db_mutex(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return db->mutex;
}

/*
** Free up as much memory as we can from the given database
** connection.
*/
int capdb_db_release_memory(capdb *db){
  int i;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  capdbBtreeEnterAll(db);
  for(i=0; i<db->nDb; i++){
    Btree *pBt = db->aDb[i].pBt;
    if( pBt ){
      Pager *pPager = capdbBtreePager(pBt);
      capdbPagerShrink(pPager);
    }
  }
  capdbBtreeLeaveAll(db);
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

/*
** Flush any dirty pages in the pager-cache for any attached database
** to disk.
*/
int capdb_db_cacheflush(capdb *db){
  int i;
  int rc = CAPDB_OK;
  int bSeenBusy = 0;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  capdbBtreeEnterAll(db);
  for(i=0; rc==CAPDB_OK && i<db->nDb; i++){
    Btree *pBt = db->aDb[i].pBt;
    if( pBt && capdbBtreeTxnState(pBt)==CAPDB_TXN_WRITE ){
      Pager *pPager = capdbBtreePager(pBt);
      rc = capdbPagerFlush(pPager);
      if( rc==CAPDB_BUSY ){
        bSeenBusy = 1;
        rc = CAPDB_OK;
      }
    }
  }
  capdbBtreeLeaveAll(db);
  capdb_mutex_leave(db->mutex);
  return ((rc==CAPDB_OK && bSeenBusy) ? CAPDB_BUSY : rc);
}

/*
** Configuration settings for an individual database connection
*/
int capdb_db_config(capdb *db, int op, ...){
  va_list ap;
  int rc;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  va_start(ap, op);
  switch( op ){
    case CAPDB_DBCONFIG_MAINDBNAME: {
      /* IMP: R-06824-28531 */
      /* IMP: R-36257-52125 */
      db->aDb[0].zDbSName = va_arg(ap,char*);
      rc = CAPDB_OK;
      break;
    }
    case CAPDB_DBCONFIG_LOOKASIDE: {
      void *pBuf = va_arg(ap, void*); /* IMP: R-26835-10964 */
      int sz = va_arg(ap, int);       /* IMP: R-47871-25994 */
      int cnt = va_arg(ap, int);      /* IMP: R-04460-53386 */
      rc = setupLookaside(db, pBuf, sz, cnt);
      break;
    }
    case CAPDB_DBCONFIG_FP_DIGITS: {
      int nIn = va_arg(ap, int);
      int *pOut = va_arg(ap, int*);
      if( nIn>3 && nIn<24 ) db->nFpDigit = (u8)nIn;
      if( pOut ) *pOut = db->nFpDigit;
      rc = CAPDB_OK;
      break;
    }
    default: {
      static const struct {
        int op;      /* The opcode */
        u64 mask;    /* Mask of the bit in capdb.flags to set/clear */
      } aFlagOp[] = {
        { CAPDB_DBCONFIG_ENABLE_FKEY,           CAPDB_ForeignKeys    },
        { CAPDB_DBCONFIG_ENABLE_TRIGGER,        CAPDB_EnableTrigger  },
        { CAPDB_DBCONFIG_ENABLE_VIEW,           CAPDB_EnableView     },
        { CAPDB_DBCONFIG_ENABLE_FTS3_TOKENIZER, CAPDB_Fts3Tokenizer  },
        { CAPDB_DBCONFIG_ENABLE_LOAD_EXTENSION, CAPDB_LoadExtension  },
        { CAPDB_DBCONFIG_NO_CKPT_ON_CLOSE,      CAPDB_NoCkptOnClose  },
        { CAPDB_DBCONFIG_ENABLE_QPSG,           CAPDB_EnableQPSG     },
        { CAPDB_DBCONFIG_TRIGGER_EQP,           CAPDB_TriggerEQP     },
        { CAPDB_DBCONFIG_RESET_DATABASE,        CAPDB_ResetDatabase  },
        { CAPDB_DBCONFIG_DEFENSIVE,             CAPDB_Defensive      },
        { CAPDB_DBCONFIG_WRITABLE_SCHEMA,       CAPDB_WriteSchema|
                                                 CAPDB_NoSchemaError  },
        { CAPDB_DBCONFIG_LEGACY_ALTER_TABLE,    CAPDB_LegacyAlter    },
        { CAPDB_DBCONFIG_DQS_DDL,               CAPDB_DqsDDL         },
        { CAPDB_DBCONFIG_DQS_DML,               CAPDB_DqsDML         },
        { CAPDB_DBCONFIG_LEGACY_FILE_FORMAT,    CAPDB_LegacyFileFmt  },
        { CAPDB_DBCONFIG_TRUSTED_SCHEMA,        CAPDB_TrustedSchema  },
        { CAPDB_DBCONFIG_STMT_SCANSTATUS,       CAPDB_StmtScanStatus },
        { CAPDB_DBCONFIG_REVERSE_SCANORDER,     CAPDB_ReverseOrder   },
        { CAPDB_DBCONFIG_ENABLE_ATTACH_CREATE,  CAPDB_AttachCreate   },
        { CAPDB_DBCONFIG_ENABLE_ATTACH_WRITE,   CAPDB_AttachWrite    },
        { CAPDB_DBCONFIG_ENABLE_COMMENTS,       CAPDB_Comments       },
      };
      unsigned int i;
      rc = CAPDB_ERROR; /* IMP: R-42790-23372 */
      for(i=0; i<ArraySize(aFlagOp); i++){
        if( aFlagOp[i].op==op ){
          int onoff = va_arg(ap, int);
          int *pRes = va_arg(ap, int*);
          u64 oldFlags = db->flags;
          if( onoff>0 ){
            db->flags |= aFlagOp[i].mask;
          }else if( onoff==0 ){
            db->flags &= ~(u64)aFlagOp[i].mask;
          }
          if( oldFlags!=db->flags ){
            capdbExpirePreparedStatements(db, 0);
          }
          if( pRes ){
            *pRes = (db->flags & aFlagOp[i].mask)!=0;
          }
          rc = CAPDB_OK;
          break;
        }
      }
      break;
    }
  }
  va_end(ap);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** This is the default collating function named "BINARY" which is always
** available.
*/
static int binCollFunc(
  void *NotUsed,
  int nKey1, const void *pKey1,
  int nKey2, const void *pKey2
){
  int rc, n;
  UNUSED_PARAMETER(NotUsed);
  n = nKey1<nKey2 ? nKey1 : nKey2;
  /* EVIDENCE-OF: R-65033-28449 The built-in BINARY collation compares
  ** strings byte by byte using the memcmp() function from the standard C
  ** library. */
  assert( pKey1 && pKey2 );
  rc = memcmp(pKey1, pKey2, n);
  if( rc==0 ){
    rc = nKey1 - nKey2;
  }
  return rc;
}

/*
** This is the collating function named "RTRIM" which is always
** available.  Ignore trailing spaces.
*/
static int rtrimCollFunc(
  void *pUser,
  int nKey1, const void *pKey1,
  int nKey2, const void *pKey2
){
  const u8 *pK1 = (const u8*)pKey1;
  const u8 *pK2 = (const u8*)pKey2;
  while( nKey1 && pK1[nKey1-1]==' ' ) nKey1--;
  while( nKey2 && pK2[nKey2-1]==' ' ) nKey2--;
  return binCollFunc(pUser, nKey1, pKey1, nKey2, pKey2);
}

/*
** Return true if CollSeq is the default built-in BINARY.
*/
int capdbIsBinary(const CollSeq *p){
  assert( p==0 || p->xCmp!=binCollFunc || strcmp(p->zName,"BINARY")==0 );
  return p==0 || p->xCmp==binCollFunc;
}

/*
** Another built-in collating sequence: NOCASE.
**
** This collating sequence is intended to be used for "case independent
** comparison". SQLite's knowledge of upper and lower case equivalents
** extends only to the 26 characters used in the English language.
**
** At the moment there is only a UTF-8 implementation.
*/
static int nocaseCollatingFunc(
  void *NotUsed,
  int nKey1, const void *pKey1,
  int nKey2, const void *pKey2
){
  int r = capdbStrNICmp(
      (const char *)pKey1, (const char *)pKey2, (nKey1<nKey2)?nKey1:nKey2);
  UNUSED_PARAMETER(NotUsed);
  if( 0==r ){
    r = nKey1-nKey2;
  }
  return r;
}

/*
** Return the ROWID of the most recent insert
*/
sqlite_int64 capdb_last_insert_rowid(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return db->lastRowid;
}

/*
** Set the value returned by the capdb_last_insert_rowid() API function.
*/
void capdb_set_last_insert_rowid(capdb *db, capdb_int64 iRowid){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return;
  }
#endif
  capdb_mutex_enter(db->mutex);
  db->lastRowid = iRowid;
  capdb_mutex_leave(db->mutex);
}

/*
** Return the number of changes in the most recently executed DML
** statement.
*/
capdb_int64 capdb_changes64(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return db->nChange;
}
int capdb_changes(capdb *db){
  return (int)capdb_changes64(db);
}

/*
** Return the number of changes since the database handle was opened.
*/
capdb_int64 capdb_total_changes64(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return db->nTotalChange;
}
int capdb_total_changes(capdb *db){
  return (int)capdb_total_changes64(db);
}

/*
** Close all open savepoints. This function only manipulates fields of the
** database handle object, it does not close any savepoints that may be open
** at the b-tree/pager level.
*/
void capdbCloseSavepoints(capdb *db){
  while( db->pSavepoint ){
    Savepoint *pTmp = db->pSavepoint;
    db->pSavepoint = pTmp->pNext;
    capdbDbFree(db, pTmp);
  }
  db->nSavepoint = 0;
  db->nStatement = 0;
  db->isTransactionSavepoint = 0;
}

/*
** Invoke the destructor function associated with FuncDef p, if any. Except,
** if this is not the last copy of the function, do not invoke it. Multiple
** copies of a single function are created when create_function() is called
** with CAPDB_ANY as the encoding.
*/
static void functionDestroy(capdb *db, FuncDef *p){
  FuncDestructor *pDestructor;
  assert( (p->funcFlags & CAPDB_FUNC_BUILTIN)==0 );
  pDestructor = p->u.pDestructor;
  if( pDestructor ){
    pDestructor->nRef--;
    if( pDestructor->nRef==0 ){
      pDestructor->xDestroy(pDestructor->pUserData);
      capdbDbFree(db, pDestructor);
    }
  }
}

/*
** Disconnect all capdb_vtab objects that belong to database connection
** db. This is called when db is being closed.
*/
static void disconnectAllVtab(capdb *db){
#ifndef CAPDB_OMIT_VIRTUALTABLE
  int i;
  HashElem *p;
  capdbBtreeEnterAll(db);
  for(i=0; i<db->nDb; i++){
    Schema *pSchema = db->aDb[i].pSchema;
    if( pSchema ){
      for(p=sqliteHashFirst(&pSchema->tblHash); p; p=sqliteHashNext(p)){
        Table *pTab = (Table *)sqliteHashData(p);
        if( IsVirtual(pTab) ) capdbVtabDisconnect(db, pTab);
      }
    }
  }
  for(p=sqliteHashFirst(&db->aModule); p; p=sqliteHashNext(p)){
    Module *pMod = (Module *)sqliteHashData(p);
    if( pMod->pEpoTab ){
      capdbVtabDisconnect(db, pMod->pEpoTab);
    }
  }
  capdbVtabUnlockList(db);
  capdbBtreeLeaveAll(db);
#else
  UNUSED_PARAMETER(db);
#endif
}

/*
** Return TRUE if database connection db has unfinalized prepared
** statements or unfinished capdb_backup objects. 
*/
static int connectionIsBusy(capdb *db){
  int j;
  assert( capdb_mutex_held(db->mutex) );
  if( db->pVdbe ) return 1;
  for(j=0; j<db->nDb; j++){
    Btree *pBt = db->aDb[j].pBt;
    if( pBt && capdbBtreeIsInBackup(pBt) ) return 1;
  }
  return 0;
}

/*
** Close an existing SQLite database
*/
static int capdbClose(capdb *db, int forceZombie){
  if( !db ){
    /* EVIDENCE-OF: R-63257-11740 Calling capdb_close() or
    ** capdb_close_v2() with a NULL pointer argument is a harmless no-op. */
    return CAPDB_OK;
  }
  if( !capdbSafetyCheckSickOrOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
  capdb_mutex_enter(db->mutex);
  if( db->mTrace & CAPDB_TRACE_CLOSE ){
    db->trace.xV2(CAPDB_TRACE_CLOSE, db->pTraceArg, db, 0);
  }

  /* Force xDisconnect calls on all virtual tables */
  disconnectAllVtab(db);

  /* If a transaction is open, the disconnectAllVtab() call above
  ** will not have called the xDisconnect() method on any virtual
  ** tables in the db->aVTrans[] array. The following capdbVtabRollback()
  ** call will do so. We need to do this before the check for active
  ** SQL statements below, as the v-table implementation may be storing
  ** some prepared statements internally.
  */
  capdbVtabRollback(db);

  /* Legacy behavior (capdb_close() behavior) is to return
  ** CAPDB_BUSY if the connection can not be closed immediately.
  */
  if( !forceZombie && connectionIsBusy(db) ){
    capdbErrorWithMsg(db, CAPDB_BUSY, "unable to close due to unfinalized "
       "statements or unfinished backups");
    capdb_mutex_leave(db->mutex);
    return CAPDB_BUSY;
  }

#ifdef CAPDB_ENABLE_SQLLOG
  if( capdbGlobalConfig.xSqllog ){
    /* Closing the handle. Fourth parameter is passed the value 2. */
    capdbGlobalConfig.xSqllog(capdbGlobalConfig.pSqllogArg, db, 0, 2);
  }
#endif

  while( db->pDbData ){
    DbClientData *p = db->pDbData;
    db->pDbData = p->pNext;
    assert( p->pData!=0 );
    if( p->xDestructor ) p->xDestructor(p->pData);
    capdb_free(p);
  }

  /* Convert the connection into a zombie and then close it.
  */
  db->eOpenState = CAPDB_STATE_ZOMBIE;
  capdbLeaveMutexAndCloseZombie(db);
  return CAPDB_OK;
}

/*
** Return the transaction state for a single databse, or the maximum
** transaction state over all attached databases if zSchema is null.
*/
int capdb_txn_state(capdb *db, const char *zSchema){
  int iDb, nDb;
  int iTxn = -1;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return -1;
  }
#endif
  capdb_mutex_enter(db->mutex);
  if( zSchema ){
    nDb = iDb = capdbFindDbName(db, zSchema);
    if( iDb<0 ) nDb--;
  }else{
    iDb = 0;
    nDb = db->nDb-1;
  }
  for(; iDb<=nDb; iDb++){
    Btree *pBt = db->aDb[iDb].pBt;
    int x = pBt!=0 ? capdbBtreeTxnState(pBt) : CAPDB_TXN_NONE;
    if( x>iTxn ) iTxn = x;
  }
  capdb_mutex_leave(db->mutex);
  return iTxn;
}

/*
** Two variations on the public interface for closing a database
** connection. The capdb_close() version returns CAPDB_BUSY and
** leaves the connection open if there are unfinalized prepared
** statements or unfinished capdb_backups.  The capdb_close_v2()
** version forces the connection to become a zombie if there are
** unclosed resources, and arranges for deallocation when the last
** prepare statement or capdb_backup closes.
*/
int capdb_close(capdb *db){ return capdbClose(db,0); }
int capdb_close_v2(capdb *db){ return capdbClose(db,1); }


/*
** Close the mutex on database connection db.
**
** Furthermore, if database connection db is a zombie (meaning that there
** has been a prior call to capdb_close(db) or capdb_close_v2(db)) and
** every capdb_stmt has now been finalized and every capdb_backup has
** finished, then free all resources.
*/
void capdbLeaveMutexAndCloseZombie(capdb *db){
  HashElem *i;                    /* Hash table iterator */
  int j;

  /* If there are outstanding capdb_stmt or capdb_backup objects
  ** or if the connection has not yet been closed by capdb_close_v2(),
  ** then just leave the mutex and return.
  */
  if( db->eOpenState!=CAPDB_STATE_ZOMBIE || connectionIsBusy(db) ){
    capdb_mutex_leave(db->mutex);
    return;
  }

  /* If we reach this point, it means that the database connection has
  ** closed all capdb_stmt and capdb_backup objects and has been
  ** passed to capdb_close (meaning that it is a zombie).  Therefore,
  ** go ahead and free all resources.
  */

  /* If a transaction is open, roll it back. This also ensures that if
  ** any database schemas have been modified by an uncommitted transaction
  ** they are reset. And that the required b-tree mutex is held to make
  ** the pager rollback and schema reset an atomic operation. */
  capdbRollbackAll(db, CAPDB_OK);

  /* Free any outstanding Savepoint structures. */
  capdbCloseSavepoints(db);

  /* Close all database connections */
  for(j=0; j<db->nDb; j++){
    struct Db *pDb = &db->aDb[j];
    if( pDb->pBt ){
      capdbBtreeClose(pDb->pBt);
      pDb->pBt = 0;
      if( j!=1 ){
        pDb->pSchema = 0;
      }
    }
  }
  /* Clear the TEMP schema separately and last */
  if( db->aDb[1].pSchema ){
    capdbSchemaClear(db->aDb[1].pSchema);
    assert( db->aDb[1].pSchema->trigHash.count==0 );
  }
  capdbVtabUnlockList(db);

  /* Free up the array of auxiliary databases */
  capdbCollapseDatabaseArray(db);
  assert( db->nDb<=2 );
  assert( db->aDb==db->aDbStatic );

  /* Tell the code in notify.c that the connection no longer holds any
  ** locks and does not require any further unlock-notify callbacks.
  */
  capdbConnectionClosed(db);

  for(i=sqliteHashFirst(&db->aFunc); i; i=sqliteHashNext(i)){
    FuncDef *pNext, *p;
    p = sqliteHashData(i);
    do{
      functionDestroy(db, p);
      pNext = p->pNext;
      capdbDbFree(db, p);
      p = pNext;
    }while( p );
  }
  capdbHashClear(&db->aFunc);
  for(i=sqliteHashFirst(&db->aCollSeq); i; i=sqliteHashNext(i)){
    CollSeq *pColl = (CollSeq *)sqliteHashData(i);
    /* Invoke any destructors registered for collation sequence user data. */
    for(j=0; j<3; j++){
      if( pColl[j].xDel ){
        pColl[j].xDel(pColl[j].pUser);
      }
    }
    capdbDbFree(db, pColl);
  }
  capdbHashClear(&db->aCollSeq);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  for(i=sqliteHashFirst(&db->aModule); i; i=sqliteHashNext(i)){
    Module *pMod = (Module *)sqliteHashData(i);
    capdbVtabEponymousTableClear(db, pMod);
    capdbVtabModuleUnref(db, pMod);
  }
  capdbHashClear(&db->aModule);
#endif

  capdbError(db, CAPDB_OK); /* Deallocates any cached error strings. */
  capdbValueFree(db->pErr);
  capdbCloseExtensions(db);

  db->eOpenState = CAPDB_STATE_ERROR;

  /* The temp-database schema is allocated differently from the other schema
  ** objects (using sqliteMalloc() directly, instead of capdbBtreeSchema()).
  ** So it needs to be freed here. Todo: Why not roll the temp schema into
  ** the same sqliteMalloc() as the one that allocates the database
  ** structure?
  */
  capdbDbFree(db, db->aDb[1].pSchema);
  if( db->xAutovacDestr ){
    db->xAutovacDestr(db->pAutovacPagesArg);
  }
  capdb_mutex_leave(db->mutex);
  db->eOpenState = CAPDB_STATE_CLOSED;
  capdb_mutex_free(db->mutex);
  assert( capdbLookasideUsed(db,0)==0 );
  if( db->lookaside.bMalloced ){
    capdb_free(db->lookaside.pStart);
  }
  capdb_free(db);
}

/*
** Rollback all database files.  If tripCode is not CAPDB_OK, then
** any write cursors are invalidated ("tripped" - as in "tripping a circuit
** breaker") and made to return tripCode if there are any further
** attempts to use that cursor.  Read cursors remain open and valid
** but are "saved" in case the table pages are moved around.
*/
void capdbRollbackAll(capdb *db, int tripCode){
  int i;
  int inTrans = 0;
  int schemaChange;
  assert( capdb_mutex_held(db->mutex) );
  capdbBeginBenignMalloc();

  /* Obtain all b-tree mutexes before making any calls to BtreeRollback().
  ** This is important in case the transaction being rolled back has
  ** modified the database schema. If the b-tree mutexes are not taken
  ** here, then another shared-cache connection might sneak in between
  ** the database rollback and schema reset, which can cause false
  ** corruption reports in some cases.  */
  capdbBtreeEnterAll(db);
  schemaChange = (db->mDbFlags & DBFLAG_SchemaChange)!=0 && db->init.busy==0;

  for(i=0; i<db->nDb; i++){
    Btree *p = db->aDb[i].pBt;
    if( p ){
      if( capdbBtreeTxnState(p)==CAPDB_TXN_WRITE ){
        inTrans = 1;
      }
      capdbBtreeRollback(p, tripCode, !schemaChange);
    }
  }
  capdbVtabRollback(db);
  capdbEndBenignMalloc();

  if( schemaChange ){
    capdbExpirePreparedStatements(db, 0);
    capdbResetAllSchemasOfConnection(db);
  }
  capdbBtreeLeaveAll(db);

  /* Any deferred constraint violations have now been resolved. */
  db->nDeferredCons = 0;
  db->nDeferredImmCons = 0;
  db->flags &= ~(u64)(CAPDB_DeferFKs|CAPDB_CorruptRdOnly);

  /* If one has been configured, invoke the rollback-hook callback */
  if( db->xRollbackCallback && (inTrans || !db->autoCommit) ){
    db->xRollbackCallback(db->pRollbackArg);
  }
}

/*
** Return a static string containing the name corresponding to the error code
** specified in the argument.
*/
#if defined(CAPDB_NEED_ERR_NAME)
const char *capdbErrName(int rc){
  const char *zName = 0;
  int i, origRc = rc;
  for(i=0; i<2 && zName==0; i++, rc &= 0xff){
    switch( rc ){
      case CAPDB_OK:                 zName = "CAPDB_OK";                break;
      case CAPDB_ERROR:              zName = "CAPDB_ERROR";             break;
      case CAPDB_ERROR_SNAPSHOT:     zName = "CAPDB_ERROR_SNAPSHOT";    break;
      case CAPDB_ERROR_RETRY:        zName = "CAPDB_ERROR_RETRY";       break;
      case CAPDB_ERROR_MISSING_COLLSEQ:
                                zName = "CAPDB_ERROR_MISSING_COLLSEQ";   break;
      case CAPDB_INTERNAL:           zName = "CAPDB_INTERNAL";          break;
      case CAPDB_PERM:               zName = "CAPDB_PERM";              break;
      case CAPDB_ABORT:              zName = "CAPDB_ABORT";             break;
      case CAPDB_ABORT_ROLLBACK:     zName = "CAPDB_ABORT_ROLLBACK";    break;
      case CAPDB_BUSY:               zName = "CAPDB_BUSY";              break;
      case CAPDB_BUSY_RECOVERY:      zName = "CAPDB_BUSY_RECOVERY";     break;
      case CAPDB_BUSY_SNAPSHOT:      zName = "CAPDB_BUSY_SNAPSHOT";     break;
      case CAPDB_LOCKED:             zName = "CAPDB_LOCKED";            break;
      case CAPDB_LOCKED_SHAREDCACHE: zName = "CAPDB_LOCKED_SHAREDCACHE";break;
      case CAPDB_NOMEM:              zName = "CAPDB_NOMEM";             break;
      case CAPDB_READONLY:           zName = "CAPDB_READONLY";          break;
      case CAPDB_READONLY_RECOVERY:  zName = "CAPDB_READONLY_RECOVERY"; break;
      case CAPDB_READONLY_CANTINIT:  zName = "CAPDB_READONLY_CANTINIT"; break;
      case CAPDB_READONLY_ROLLBACK:  zName = "CAPDB_READONLY_ROLLBACK"; break;
      case CAPDB_READONLY_DBMOVED:   zName = "CAPDB_READONLY_DBMOVED";  break;
      case CAPDB_READONLY_DIRECTORY: zName = "CAPDB_READONLY_DIRECTORY";break;
      case CAPDB_INTERRUPT:          zName = "CAPDB_INTERRUPT";         break;
      case CAPDB_IOERR:              zName = "CAPDB_IOERR";             break;
      case CAPDB_IOERR_READ:         zName = "CAPDB_IOERR_READ";        break;
      case CAPDB_IOERR_SHORT_READ:   zName = "CAPDB_IOERR_SHORT_READ";  break;
      case CAPDB_IOERR_WRITE:        zName = "CAPDB_IOERR_WRITE";       break;
      case CAPDB_IOERR_FSYNC:        zName = "CAPDB_IOERR_FSYNC";       break;
      case CAPDB_IOERR_DIR_FSYNC:    zName = "CAPDB_IOERR_DIR_FSYNC";   break;
      case CAPDB_IOERR_TRUNCATE:     zName = "CAPDB_IOERR_TRUNCATE";    break;
      case CAPDB_IOERR_FSTAT:        zName = "CAPDB_IOERR_FSTAT";       break;
      case CAPDB_IOERR_UNLOCK:       zName = "CAPDB_IOERR_UNLOCK";      break;
      case CAPDB_IOERR_RDLOCK:       zName = "CAPDB_IOERR_RDLOCK";      break;
      case CAPDB_IOERR_DELETE:       zName = "CAPDB_IOERR_DELETE";      break;
      case CAPDB_IOERR_NOMEM:        zName = "CAPDB_IOERR_NOMEM";       break;
      case CAPDB_IOERR_ACCESS:       zName = "CAPDB_IOERR_ACCESS";      break;
      case CAPDB_IOERR_CHECKRESERVEDLOCK:
                                zName = "CAPDB_IOERR_CHECKRESERVEDLOCK"; break;
      case CAPDB_IOERR_LOCK:         zName = "CAPDB_IOERR_LOCK";        break;
      case CAPDB_IOERR_CLOSE:        zName = "CAPDB_IOERR_CLOSE";       break;
      case CAPDB_IOERR_DIR_CLOSE:    zName = "CAPDB_IOERR_DIR_CLOSE";   break;
      case CAPDB_IOERR_SHMOPEN:      zName = "CAPDB_IOERR_SHMOPEN";     break;
      case CAPDB_IOERR_SHMSIZE:      zName = "CAPDB_IOERR_SHMSIZE";     break;
      case CAPDB_IOERR_SHMLOCK:      zName = "CAPDB_IOERR_SHMLOCK";     break;
      case CAPDB_IOERR_SHMMAP:       zName = "CAPDB_IOERR_SHMMAP";      break;
      case CAPDB_IOERR_SEEK:         zName = "CAPDB_IOERR_SEEK";        break;
      case CAPDB_IOERR_DELETE_NOENT: zName = "CAPDB_IOERR_DELETE_NOENT";break;
      case CAPDB_IOERR_MMAP:         zName = "CAPDB_IOERR_MMAP";        break;
      case CAPDB_IOERR_GETTEMPPATH:  zName = "CAPDB_IOERR_GETTEMPPATH"; break;
      case CAPDB_IOERR_CONVPATH:     zName = "CAPDB_IOERR_CONVPATH";    break;
      case CAPDB_CORRUPT:            zName = "CAPDB_CORRUPT";           break;
      case CAPDB_CORRUPT_VTAB:       zName = "CAPDB_CORRUPT_VTAB";      break;
      case CAPDB_NOTFOUND:           zName = "CAPDB_NOTFOUND";          break;
      case CAPDB_FULL:               zName = "CAPDB_FULL";              break;
      case CAPDB_CANTOPEN:           zName = "CAPDB_CANTOPEN";          break;
      case CAPDB_CANTOPEN_NOTEMPDIR: zName = "CAPDB_CANTOPEN_NOTEMPDIR";break;
      case CAPDB_CANTOPEN_ISDIR:     zName = "CAPDB_CANTOPEN_ISDIR";    break;
      case CAPDB_CANTOPEN_FULLPATH:  zName = "CAPDB_CANTOPEN_FULLPATH"; break;
      case CAPDB_CANTOPEN_CONVPATH:  zName = "CAPDB_CANTOPEN_CONVPATH"; break;
      case CAPDB_CANTOPEN_SYMLINK:   zName = "CAPDB_CANTOPEN_SYMLINK";  break;
      case CAPDB_PROTOCOL:           zName = "CAPDB_PROTOCOL";          break;
      case CAPDB_EMPTY:              zName = "CAPDB_EMPTY";             break;
      case CAPDB_SCHEMA:             zName = "CAPDB_SCHEMA";            break;
      case CAPDB_TOOBIG:             zName = "CAPDB_TOOBIG";            break;
      case CAPDB_CONSTRAINT:         zName = "CAPDB_CONSTRAINT";        break;
      case CAPDB_CONSTRAINT_UNIQUE:  zName = "CAPDB_CONSTRAINT_UNIQUE"; break;
      case CAPDB_CONSTRAINT_TRIGGER: zName = "CAPDB_CONSTRAINT_TRIGGER";break;
      case CAPDB_CONSTRAINT_FOREIGNKEY:
                                zName = "CAPDB_CONSTRAINT_FOREIGNKEY";   break;
      case CAPDB_CONSTRAINT_CHECK:   zName = "CAPDB_CONSTRAINT_CHECK";  break;
      case CAPDB_CONSTRAINT_PRIMARYKEY:
                                zName = "CAPDB_CONSTRAINT_PRIMARYKEY";   break;
      case CAPDB_CONSTRAINT_NOTNULL: zName = "CAPDB_CONSTRAINT_NOTNULL";break;
      case CAPDB_CONSTRAINT_COMMITHOOK:
                                zName = "CAPDB_CONSTRAINT_COMMITHOOK";   break;
      case CAPDB_CONSTRAINT_VTAB:    zName = "CAPDB_CONSTRAINT_VTAB";   break;
      case CAPDB_CONSTRAINT_FUNCTION:
                                zName = "CAPDB_CONSTRAINT_FUNCTION";     break;
      case CAPDB_CONSTRAINT_ROWID:   zName = "CAPDB_CONSTRAINT_ROWID";  break;
      case CAPDB_MISMATCH:           zName = "CAPDB_MISMATCH";          break;
      case CAPDB_MISUSE:             zName = "CAPDB_MISUSE";            break;
      case CAPDB_NOLFS:              zName = "CAPDB_NOLFS";             break;
      case CAPDB_AUTH:               zName = "CAPDB_AUTH";              break;
      case CAPDB_FORMAT:             zName = "CAPDB_FORMAT";            break;
      case CAPDB_RANGE:              zName = "CAPDB_RANGE";             break;
      case CAPDB_NOTADB:             zName = "CAPDB_NOTADB";            break;
      case CAPDB_ROW:                zName = "CAPDB_ROW";               break;
      case CAPDB_NOTICE:             zName = "CAPDB_NOTICE";            break;
      case CAPDB_NOTICE_RECOVER_WAL: zName = "CAPDB_NOTICE_RECOVER_WAL";break;
      case CAPDB_NOTICE_RECOVER_ROLLBACK:
                                zName = "CAPDB_NOTICE_RECOVER_ROLLBACK"; break;
      case CAPDB_NOTICE_RBU:         zName = "CAPDB_NOTICE_RBU"; break;
      case CAPDB_WARNING:            zName = "CAPDB_WARNING";           break;
      case CAPDB_WARNING_AUTOINDEX:  zName = "CAPDB_WARNING_AUTOINDEX"; break;
      case CAPDB_DONE:               zName = "CAPDB_DONE";              break;
    }
  }
  if( zName==0 ){
    static char zBuf[50];
    capdb_snprintf(sizeof(zBuf), zBuf, "CAPDB_UNKNOWN(%d)", origRc);
    zName = zBuf;
  }
  return zName;
}
#endif

/*
** Return a static string that describes the kind of error specified in the
** argument.
*/
const char *capdbErrStr(int rc){
  static const char* const aMsg[] = {
    /* CAPDB_OK          */ "not an error",
    /* CAPDB_ERROR       */ "SQL logic error",
    /* CAPDB_INTERNAL    */ 0,
    /* CAPDB_PERM        */ "access permission denied",
    /* CAPDB_ABORT       */ "query aborted",
    /* CAPDB_BUSY        */ "database is locked",
    /* CAPDB_LOCKED      */ "database table is locked",
    /* CAPDB_NOMEM       */ "out of memory",
    /* CAPDB_READONLY    */ "attempt to write a readonly database",
    /* CAPDB_INTERRUPT   */ "interrupted",
    /* CAPDB_IOERR       */ "disk I/O error",
    /* CAPDB_CORRUPT     */ "database disk image is malformed",
    /* CAPDB_NOTFOUND    */ "unknown operation",
    /* CAPDB_FULL        */ "database or disk is full",
    /* CAPDB_CANTOPEN    */ "unable to open database file",
    /* CAPDB_PROTOCOL    */ "locking protocol",
    /* CAPDB_EMPTY       */ 0,
    /* CAPDB_SCHEMA      */ "database schema has changed",
    /* CAPDB_TOOBIG      */ "string or blob too big",
    /* CAPDB_CONSTRAINT  */ "constraint failed",
    /* CAPDB_MISMATCH    */ "datatype mismatch",
    /* CAPDB_MISUSE      */ "bad parameter or other API misuse",
#ifdef CAPDB_DISABLE_LFS
    /* CAPDB_NOLFS       */ "large file support is disabled",
#else
    /* CAPDB_NOLFS       */ 0,
#endif
    /* CAPDB_AUTH        */ "authorization denied",
    /* CAPDB_FORMAT      */ 0,
    /* CAPDB_RANGE       */ "column index out of range",
    /* CAPDB_NOTADB      */ "file is not a database",
    /* CAPDB_NOTICE      */ "notification message",
    /* CAPDB_WARNING     */ "warning message",
  };
  const char *zErr = "unknown error";
  switch( rc ){
    case CAPDB_ABORT_ROLLBACK: {
      zErr = "abort due to ROLLBACK";
      break;
    }
    case CAPDB_ROW: {
      zErr = "another row available";
      break;
    }
    case CAPDB_DONE: {
      zErr = "no more rows available";
      break;
    }
    default: {
      rc &= 0xff;
      if( ALWAYS(rc>=0) && rc<ArraySize(aMsg) && aMsg[rc]!=0 ){
        zErr = aMsg[rc];
      }
      break;
    }
  }
  return zErr;
}

/*
** This routine implements a busy callback that sleeps and tries
** again until a timeout value is reached.  The timeout value is
** an integer number of milliseconds passed in as the first
** argument.
**
** Return non-zero to retry the lock.  Return zero to stop trying
** and cause SQLite to return CAPDB_BUSY.
*/
static int sqliteDefaultBusyCallback(
  void *ptr,               /* Database connection */
  int count                /* Number of times table has been busy */
){
#if CAPDB_OS_WIN || !defined(HAVE_NANOSLEEP) || HAVE_NANOSLEEP
  /* This case is for systems that have support for sleeping for fractions of
  ** a second.  Examples:  All windows systems, unix systems with nanosleep() */
  static const u8 delays[] =
     { 1, 2, 5, 10, 15, 20, 25, 25,  25,  50,  50, 100 };
  static const u8 totals[] =
     { 0, 1, 3,  8, 18, 33, 53, 78, 103, 128, 178, 228 };
# define NDELAY ArraySize(delays)
  capdb *db = (capdb *)ptr;
  int tmout = db->busyTimeout;
  int delay, prior;

  assert( count>=0 );
  if( count < NDELAY ){
    delay = delays[count];
    prior = totals[count];
  }else{
    delay = delays[NDELAY-1];
    prior = totals[NDELAY-1] + delay*(count-(NDELAY-1));
  }
  if( prior + delay > tmout ){
    delay = tmout - prior;
    if( delay<=0 ) return 0;
  }
  capdbOsSleep(db->pVfs, delay*1000);
  return 1;
#else
  /* This case for unix systems that lack usleep() support.  Sleeping
  ** must be done in increments of whole seconds */
  capdb *db = (capdb *)ptr;
  int tmout = ((capdb *)ptr)->busyTimeout;
  if( (count+1)*1000 > tmout ){
    return 0;
  }
  capdbOsSleep(db->pVfs, 1000000);
  return 1;
#endif
}

/*
** Invoke the given busy handler.
**
** This routine is called when an operation failed to acquire a
** lock on VFS file pFile.
**
** If this routine returns non-zero, the lock is retried.  If it
** returns 0, the operation aborts with an CAPDB_BUSY error.
*/
int capdbInvokeBusyHandler(BusyHandler *p){
  int rc;
  if( p->xBusyHandler==0 || p->nBusy<0 ) return 0;
  rc = p->xBusyHandler(p->pBusyArg, p->nBusy);
  if( rc==0 ){
    p->nBusy = -1;
  }else{
    p->nBusy++;
  }
  return rc;
}

/*
** This routine sets the busy callback for an Sqlite database to the
** given callback function with the given argument.
*/
int capdb_busy_handler(
  capdb *db,
  int (*xBusy)(void*,int),
  void *pArg
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  db->busyHandler.xBusyHandler = xBusy;
  db->busyHandler.pBusyArg = pArg;
  db->busyHandler.nBusy = 0;
  db->busyTimeout = 0;
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  db->setlkTimeout = 0;
#endif
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

#ifndef CAPDB_OMIT_PROGRESS_CALLBACK
/*
** This routine sets the progress callback for an Sqlite database to the
** given callback function with the given argument. The progress callback will
** be invoked every nOps opcodes.
*/
void capdb_progress_handler(
  capdb *db,
  int nOps,
  int (*xProgress)(void*),
  void *pArg
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return;
  }
#endif
  capdb_mutex_enter(db->mutex);
  if( nOps>0 ){
    db->xProgress = xProgress;
    db->nProgressOps = (unsigned)nOps;
    db->pProgressArg = pArg;
  }else{
    db->xProgress = 0;
    db->nProgressOps = 0;
    db->pProgressArg = 0;
  }
  capdb_mutex_leave(db->mutex);
}
#endif


/*
** This routine installs a default busy handler that waits for the
** specified number of milliseconds before returning 0.
*/
int capdb_busy_timeout(capdb *db, int ms){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  if( ms>0 ){
    capdb_busy_handler(db, (int(*)(void*,int))sqliteDefaultBusyCallback,
                             (void*)db);
    db->busyTimeout = ms;
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
    db->setlkTimeout = ms;
#endif
  }else{
    capdb_busy_handler(db, 0, 0);
  }
  return CAPDB_OK;
}

/*
** Set the setlk timeout value.
*/
int capdb_setlk_timeout(capdb *db, int ms, int flags){
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  int iDb;
  int bBOC = ((flags & CAPDB_SETLK_BLOCK_ON_CONNECT) ? 1 : 0);
#endif
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  if( ms<-1 ) return CAPDB_RANGE;
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  capdb_mutex_enter(db->mutex);
  db->setlkTimeout = ms;
  db->setlkFlags = flags;
  capdbBtreeEnterAll(db);
  for(iDb=0; iDb<db->nDb; iDb++){
    Btree *pBt = db->aDb[iDb].pBt;
    if( pBt ){
      capdb_file *fd = capdbPagerFile(capdbBtreePager(pBt));
      capdbOsFileControlHint(fd, CAPDB_FCNTL_BLOCK_ON_CONNECT, (void*)&bBOC);
    }
  }
  capdbBtreeLeaveAll(db);
  capdb_mutex_leave(db->mutex);
#endif
#if !defined(CAPDB_ENABLE_API_ARMOR) && !defined(CAPDB_ENABLE_SETLK_TIMEOUT)
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(flags);
#endif
  return CAPDB_OK;
}

/*
** Cause any pending operation to stop at its earliest opportunity.
*/
void capdb_interrupt(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db)
   && (db==0 || db->eOpenState!=CAPDB_STATE_ZOMBIE)
  ){
    (void)CAPDB_MISUSE_BKPT;
    return;
  }
#endif
  AtomicStore(&db->u1.isInterrupted, 1);
}

/*
** Return true or false depending on whether or not an interrupt is
** pending on connection db.
*/
int capdb_is_interrupted(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db)
   && (db==0 || db->eOpenState!=CAPDB_STATE_ZOMBIE)
  ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return AtomicLoad(&db->u1.isInterrupted)!=0;
}

/*
** This function is exactly the same as capdb_create_function(), except
** that it is designed to be called by internal code. The difference is
** that if a malloc() fails in capdb_create_function(), an error code
** is returned and the mallocFailed flag cleared.
*/
int capdbCreateFunc(
  capdb *db,
  const char *zFunctionName,
  int nArg,
  int enc,
  void *pUserData,
  void (*xSFunc)(capdb_context*,int,capdb_value **),
  void (*xStep)(capdb_context*,int,capdb_value **),
  void (*xFinal)(capdb_context*),
  void (*xValue)(capdb_context*),
  void (*xInverse)(capdb_context*,int,capdb_value **),
  FuncDestructor *pDestructor
){
  FuncDef *p;
  int extraFlags;

  assert( capdb_mutex_held(db->mutex) );
  assert( xValue==0 || xSFunc==0 );
  if( zFunctionName==0                /* Must have a valid name */
   || (xSFunc!=0 && xFinal!=0)        /* Not both xSFunc and xFinal */
   || ((xFinal==0)!=(xStep==0))       /* Both or neither of xFinal and xStep */
   || ((xValue==0)!=(xInverse==0))    /* Both or neither of xValue, xInverse */
   || (nArg<-1 || nArg>CAPDB_MAX_FUNCTION_ARG)
   || (255<capdbStrlen30(zFunctionName))
  ){
    return CAPDB_MISUSE_BKPT;
  }

  assert( CAPDB_FUNC_CONSTANT==CAPDB_DETERMINISTIC );
  assert( CAPDB_FUNC_DIRECT==CAPDB_DIRECTONLY );
  extraFlags = enc &  (CAPDB_DETERMINISTIC|CAPDB_DIRECTONLY|
                       CAPDB_SUBTYPE|CAPDB_INNOCUOUS|
                       CAPDB_RESULT_SUBTYPE|CAPDB_SELFORDER1);
  enc &= (CAPDB_FUNC_ENCMASK|CAPDB_ANY);

  /* The CAPDB_INNOCUOUS flag is the same bit as CAPDB_FUNC_UNSAFE.  But
  ** the meaning is inverted.  So flip the bit. */
  assert( CAPDB_FUNC_UNSAFE==CAPDB_INNOCUOUS );
  extraFlags ^= CAPDB_FUNC_UNSAFE;  /* tag-20230109-1 */

 
#ifndef CAPDB_OMIT_UTF16
  /* If CAPDB_UTF16 is specified as the encoding type, transform this
  ** to one of CAPDB_UTF16LE or CAPDB_UTF16BE using the
  ** CAPDB_UTF16NATIVE macro. CAPDB_UTF16 is not used internally.
  **
  ** If CAPDB_ANY is specified, add three versions of the function
  ** to the hash table.
  */
  switch( enc ){
    case CAPDB_UTF16:
      enc = CAPDB_UTF16NATIVE;
      break;
    case CAPDB_ANY: {
      int rc;
      rc = capdbCreateFunc(db, zFunctionName, nArg,
           (CAPDB_UTF8|extraFlags)^CAPDB_FUNC_UNSAFE, /* tag-20230109-1 */
           pUserData, xSFunc, xStep, xFinal, xValue, xInverse, pDestructor);
      if( rc==CAPDB_OK ){
        rc = capdbCreateFunc(db, zFunctionName, nArg,
             (CAPDB_UTF16LE|extraFlags)^CAPDB_FUNC_UNSAFE, /* tag-20230109-1*/
             pUserData, xSFunc, xStep, xFinal, xValue, xInverse, pDestructor);
      }
      if( rc!=CAPDB_OK ){
        return rc;
      }
      enc = CAPDB_UTF16BE;
      break;
    }
    case CAPDB_UTF8:
    case CAPDB_UTF16LE:
    case CAPDB_UTF16BE:
      break;
    default:
      enc = CAPDB_UTF8;
      break;
  }
#else
  enc = CAPDB_UTF8;
#endif
 
  /* Check if an existing function is being overridden or deleted. If so,
  ** and there are active VMs, then return CAPDB_BUSY. If a function
  ** is being overridden/deleted but there are no active VMs, allow the
  ** operation to continue but invalidate all precompiled statements.
  */
  p = capdbFindFunction(db, zFunctionName, nArg, (u8)enc, 0);
  if( p && (p->funcFlags & CAPDB_FUNC_ENCMASK)==(u32)enc && p->nArg==nArg ){
    if( db->nVdbeActive ){
      capdbErrorWithMsg(db, CAPDB_BUSY,
        "unable to delete/modify user-function due to active statements");
      assert( !db->mallocFailed );
      return CAPDB_BUSY;
    }else{
      capdbExpirePreparedStatements(db, 0);
    }
  }else if( xSFunc==0 && xFinal==0 ){
    /* Trying to delete a function that does not exist.  This is a no-op.
    ** https://sqlite.org/forum/forumpost/726219164b */
    return CAPDB_OK;
  }

  p = capdbFindFunction(db, zFunctionName, nArg, (u8)enc, 1);
  assert(p || db->mallocFailed);
  if( !p ){
    return CAPDB_NOMEM_BKPT;
  }

  /* If an older version of the function with a configured destructor is
  ** being replaced invoke the destructor function here. */
  functionDestroy(db, p);

  if( pDestructor ){
    pDestructor->nRef++;
  }
  p->u.pDestructor = pDestructor;
  p->funcFlags = (p->funcFlags & CAPDB_FUNC_ENCMASK) | extraFlags;
  testcase( p->funcFlags & CAPDB_DETERMINISTIC );
  testcase( p->funcFlags & CAPDB_DIRECTONLY );
  p->xSFunc = xSFunc ? xSFunc : xStep;
  p->xFinalize = xFinal;
  p->xValue = xValue;
  p->xInverse = xInverse;
  p->pUserData = pUserData;
  p->nArg = (u16)nArg;
  return CAPDB_OK;
}

/*
** Worker function used by utf-8 APIs that create new functions:
**
**    capdb_create_function()
**    capdb_create_function_v2()
**    capdb_create_window_function()
*/
static int createFunctionApi(
  capdb *db,
  const char *zFunc,
  int nArg,
  int enc,
  void *p,
  void (*xSFunc)(capdb_context*,int,capdb_value**),
  void (*xStep)(capdb_context*,int,capdb_value**),
  void (*xFinal)(capdb_context*),
  void (*xValue)(capdb_context*),
  void (*xInverse)(capdb_context*,int,capdb_value**),
  void(*xDestroy)(void*)
){
  int rc = CAPDB_ERROR;
  FuncDestructor *pArg = 0;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  capdb_mutex_enter(db->mutex);
  if( xDestroy ){
    pArg = (FuncDestructor *)capdbMalloc(sizeof(FuncDestructor));
    if( !pArg ){
      capdbOomFault(db);
      xDestroy(p);
      goto out;
    }
    pArg->nRef = 0;
    pArg->xDestroy = xDestroy;
    pArg->pUserData = p;
  }
  rc = capdbCreateFunc(db, zFunc, nArg, enc, p,
      xSFunc, xStep, xFinal, xValue, xInverse, pArg
  );
  if( pArg && pArg->nRef==0 ){
    assert( rc!=CAPDB_OK || (xStep==0 && xFinal==0) );
    xDestroy(p);
    capdb_free(pArg);
  }

 out:
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Create new user functions.
*/
int capdb_create_function(
  capdb *db,
  const char *zFunc,
  int nArg,
  int enc,
  void *p,
  void (*xSFunc)(capdb_context*,int,capdb_value **),
  void (*xStep)(capdb_context*,int,capdb_value **),
  void (*xFinal)(capdb_context*)
){
  return createFunctionApi(db, zFunc, nArg, enc, p, xSFunc, xStep,
                                    xFinal, 0, 0, 0);
}
int capdb_create_function_v2(
  capdb *db,
  const char *zFunc,
  int nArg,
  int enc,
  void *p,
  void (*xSFunc)(capdb_context*,int,capdb_value **),
  void (*xStep)(capdb_context*,int,capdb_value **),
  void (*xFinal)(capdb_context*),
  void (*xDestroy)(void *)
){
  return createFunctionApi(db, zFunc, nArg, enc, p, xSFunc, xStep,
                                    xFinal, 0, 0, xDestroy);
}
int capdb_create_window_function(
  capdb *db,
  const char *zFunc,
  int nArg,
  int enc,
  void *p,
  void (*xStep)(capdb_context*,int,capdb_value **),
  void (*xFinal)(capdb_context*),
  void (*xValue)(capdb_context*),
  void (*xInverse)(capdb_context*,int,capdb_value **),
  void (*xDestroy)(void *)
){
  return createFunctionApi(db, zFunc, nArg, enc, p, 0, xStep,
                                    xFinal, xValue, xInverse, xDestroy);
}

#ifndef CAPDB_OMIT_UTF16
int capdb_create_function16(
  capdb *db,
  const void *zFunctionName,
  int nArg,
  int eTextRep,
  void *p,
  void (*xSFunc)(capdb_context*,int,capdb_value**),
  void (*xStep)(capdb_context*,int,capdb_value**),
  void (*xFinal)(capdb_context*)
){
  int rc;
  char *zFunc8;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zFunctionName==0 ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  assert( !db->mallocFailed );
  zFunc8 = capdbUtf16to8(db, zFunctionName, -1, CAPDB_UTF16NATIVE);
  rc = capdbCreateFunc(db, zFunc8, nArg, eTextRep, p, xSFunc,xStep,xFinal,0,0,0);
  capdbDbFree(db, zFunc8);
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}
#endif


/*
** The following is the implementation of an SQL function that always
** fails with an error message stating that the function is used in the
** wrong context.  The capdb_overload_function() API might construct
** SQL function that use this routine so that the functions will exist
** for name resolution but are actually overloaded by the xFindFunction
** method of virtual tables.
*/
static void capdbInvalidFunction(
  capdb_context *context,  /* The function calling context */
  int NotUsed,               /* Number of arguments to the function */
  capdb_value **NotUsed2   /* Value of each argument */
){
  const char *zName = (const char*)capdb_user_data(context);
  char *zErr;
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  zErr = capdb_mprintf(
      "unable to use function %s in the requested context", zName);
  capdb_result_error(context, zErr, -1);
  capdb_free(zErr);
}

/*
** Declare that a function has been overloaded by a virtual table.
**
** If the function already exists as a regular global function, then
** this routine is a no-op.  If the function does not exist, then create
** a new one that always throws a run-time error. 
**
** When virtual tables intend to provide an overloaded function, they
** should call this routine to make sure the global function exists.
** A global function must exist in order for name resolution to work
** properly.
*/
int capdb_overload_function(
  capdb *db,
  const char *zName,
  int nArg
){
  int rc;
  char *zCopy;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zName==0 || nArg<-2 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  capdb_mutex_enter(db->mutex);
  rc = capdbFindFunction(db, zName, nArg, CAPDB_UTF8, 0)!=0;
  capdb_mutex_leave(db->mutex);
  if( rc ) return CAPDB_OK;
  zCopy = capdb_mprintf("%s", zName);
  if( zCopy==0 ) return CAPDB_NOMEM;
  return capdb_create_function_v2(db, zName, nArg, CAPDB_UTF8,
                           zCopy, capdbInvalidFunction, 0, 0, capdb_free);
}

#ifndef CAPDB_OMIT_TRACE
/*
** Register a trace function.  The pArg from the previously registered trace
** is returned. 
**
** A NULL trace function means that no tracing is executes.  A non-NULL
** trace is a pointer to a function that is invoked at the start of each
** SQL statement.
*/
#ifndef CAPDB_OMIT_DEPRECATED
void *capdb_trace(capdb *db, void(*xTrace)(void*,const char*), void *pArg){
  void *pOld;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pOld = db->pTraceArg;
  db->mTrace = xTrace ? CAPDB_TRACE_LEGACY : 0;
  db->trace.xLegacy = xTrace;
  db->pTraceArg = pArg;
  capdb_mutex_leave(db->mutex);
  return pOld;
}
#endif /* CAPDB_OMIT_DEPRECATED */

/* Register a trace callback using the version-2 interface.
*/
int capdb_trace_v2(
  capdb *db,                               /* Trace this connection */
  unsigned mTrace,                           /* Mask of events to be traced */
  int(*xTrace)(unsigned,void*,void*,void*),  /* Callback to invoke */
  void *pArg                                 /* Context */
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  capdb_mutex_enter(db->mutex);
  if( mTrace==0 ) xTrace = 0;
  if( xTrace==0 ) mTrace = 0;
  db->mTrace = mTrace;
  db->trace.xV2 = xTrace;
  db->pTraceArg = pArg;
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

#ifndef CAPDB_OMIT_DEPRECATED
/*
** Register a profile function.  The pArg from the previously registered
** profile function is returned. 
**
** A NULL profile function means that no profiling is executes.  A non-NULL
** profile is a pointer to a function that is invoked at the conclusion of
** each SQL statement that is run.
*/
void *capdb_profile(
  capdb *db,
  void (*xProfile)(void*,const char*,sqlite_uint64),
  void *pArg
){
  void *pOld;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pOld = db->pProfileArg;
  db->xProfile = xProfile;
  db->pProfileArg = pArg;
  db->mTrace &= CAPDB_TRACE_NONLEGACY_MASK;
  if( db->xProfile ) db->mTrace |= CAPDB_TRACE_XPROFILE;
  capdb_mutex_leave(db->mutex);
  return pOld;
}
#endif /* CAPDB_OMIT_DEPRECATED */
#endif /* CAPDB_OMIT_TRACE */

/*
** Register a function to be invoked when a transaction commits.
** If the invoked function returns non-zero, then the commit becomes a
** rollback.
*/
void *capdb_commit_hook(
  capdb *db,              /* Attach the hook to this database */
  int (*xCallback)(void*),  /* Function to invoke on each commit */
  void *pArg                /* Argument to the function */
){
  void *pOld;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pOld = db->pCommitArg;
  db->xCommitCallback = xCallback;
  db->pCommitArg = pArg;
  capdb_mutex_leave(db->mutex);
  return pOld;
}

/*
** Register a callback to be invoked each time a row is updated,
** inserted or deleted using this database connection.
*/
void *capdb_update_hook(
  capdb *db,              /* Attach the hook to this database */
  void (*xCallback)(void*,int,char const *,char const *,sqlite_int64),
  void *pArg                /* Argument to the function */
){
  void *pRet;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pRet = db->pUpdateArg;
  db->xUpdateCallback = xCallback;
  db->pUpdateArg = pArg;
  capdb_mutex_leave(db->mutex);
  return pRet;
}

/*
** Register a callback to be invoked each time a transaction is rolled
** back by this database connection.
*/
void *capdb_rollback_hook(
  capdb *db,              /* Attach the hook to this database */
  void (*xCallback)(void*), /* Callback function */
  void *pArg                /* Argument to the function */
){
  void *pRet;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pRet = db->pRollbackArg;
  db->xRollbackCallback = xCallback;
  db->pRollbackArg = pArg;
  capdb_mutex_leave(db->mutex);
  return pRet;
}

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** Register a callback to be invoked each time a row is updated,
** inserted or deleted using this database connection.
*/
void *capdb_preupdate_hook(
  capdb *db,              /* Attach the hook to this database */
  void(*xCallback)(         /* Callback function */
    void*,capdb*,int,char const*,char const*,capdb_int64,capdb_int64),
  void *pArg                /* First callback argument */
){
  void *pRet;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( db==0 ){
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pRet = db->pPreUpdateArg;
  db->xPreUpdateCallback = xCallback;
  db->pPreUpdateArg = pArg;
  capdb_mutex_leave(db->mutex);
  return pRet;
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

/*
** Register a function to be invoked prior to each autovacuum that
** determines the number of pages to vacuum.
*/
int capdb_autovacuum_pages(
  capdb *db,                 /* Attach the hook to this database */
  unsigned int (*xCallback)(void*,const char*,u32,u32,u32),
  void *pArg,                  /* Argument to the function */
  void (*xDestructor)(void*)   /* Destructor for pArg */
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    if( xDestructor ) xDestructor(pArg);
    return CAPDB_MISUSE_BKPT;
  }
#endif
  capdb_mutex_enter(db->mutex);
  if( db->xAutovacDestr ){
    db->xAutovacDestr(db->pAutovacPagesArg);
  }
  db->xAutovacPages = xCallback;
  db->pAutovacPagesArg = pArg;
  db->xAutovacDestr = xDestructor;
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}


#ifndef CAPDB_OMIT_WAL
/*
** The capdb_wal_hook() callback registered by capdb_wal_autocheckpoint().
** Invoke capdb_wal_checkpoint if the number of frames in the log file
** is greater than capdb.pWalArg cast to an integer (the value configured by
** wal_autocheckpoint()).
*/
int capdbWalDefaultHook(
  void *pClientData,     /* Argument */
  capdb *db,           /* Connection */
  const char *zDb,       /* Database */
  int nFrame             /* Size of WAL */
){
  if( nFrame>=CAPDB_PTR_TO_INT(pClientData) ){
    capdbBeginBenignMalloc();
    capdb_wal_checkpoint(db, zDb);
    capdbEndBenignMalloc();
  }
  return CAPDB_OK;
}
#endif /* CAPDB_OMIT_WAL */

/*
** Configure an capdb_wal_hook() callback to automatically checkpoint
** a database after committing a transaction if there are nFrame or
** more frames in the log file. Passing zero or a negative value as the
** nFrame parameter disables automatic checkpoints entirely.
**
** The callback registered by this function replaces any existing callback
** registered using capdb_wal_hook(). Likewise, registering a callback
** using capdb_wal_hook() disables the automatic checkpoint mechanism
** configured by this function.
*/
int capdb_wal_autocheckpoint(capdb *db, int nFrame){
#ifdef CAPDB_OMIT_WAL
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(nFrame);
#else
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  if( nFrame>0 ){
    capdb_wal_hook(db, capdbWalDefaultHook, CAPDB_INT_TO_PTR(nFrame));
  }else{
    capdb_wal_hook(db, 0, 0);
  }
#endif
  return CAPDB_OK;
}

/*
** Register a callback to be invoked each time a transaction is written
** into the write-ahead-log by this database connection.
*/
void *capdb_wal_hook(
  capdb *db,                    /* Attach the hook to this db handle */
  int(*xCallback)(void *, capdb*, const char*, int),
  void *pArg                      /* First argument passed to xCallback() */
){
#ifndef CAPDB_OMIT_WAL
  void *pRet;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  pRet = db->pWalArg;
  db->xWalCallback = xCallback;
  db->pWalArg = pArg;
  capdb_mutex_leave(db->mutex);
  return pRet;
#else
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(xCallback);
  UNUSED_PARAMETER(pArg);
  return 0;
#endif
}

/*
** Checkpoint database zDb.
*/
int capdb_wal_checkpoint_v2(
  capdb *db,                    /* Database handle */
  const char *zDb,                /* Name of attached database (or NULL) */
  int eMode,                      /* CAPDB_CHECKPOINT_* value */
  int *pnLog,                     /* OUT: Size of WAL log in frames */
  int *pnCkpt                     /* OUT: Total number of frames checkpointed */
){
#ifdef CAPDB_OMIT_WAL
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(zDb);
  UNUSED_PARAMETER(eMode);
  UNUSED_PARAMETER(pnLog);
  UNUSED_PARAMETER(pnCkpt);
  return CAPDB_OK;
#else
  int rc;                         /* Return code */
  int iDb;                        /* Schema to checkpoint */

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif

  /* Initialize the output variables to -1 in case an error occurs. */
  if( pnLog ) *pnLog = -1;
  if( pnCkpt ) *pnCkpt = -1;

  assert( CAPDB_CHECKPOINT_NOOP==-1 );
  assert( CAPDB_CHECKPOINT_PASSIVE==0 );
  assert( CAPDB_CHECKPOINT_FULL==1 );
  assert( CAPDB_CHECKPOINT_RESTART==2 );
  assert( CAPDB_CHECKPOINT_TRUNCATE==3 );
  if( eMode<CAPDB_CHECKPOINT_NOOP || eMode>CAPDB_CHECKPOINT_TRUNCATE ){
    /* EVIDENCE-OF: R-03996-12088 The M parameter must be a valid checkpoint
    ** mode: */
    return CAPDB_MISUSE_BKPT;
  }

  capdb_mutex_enter(db->mutex);
  if( zDb && zDb[0] ){
    iDb = capdbFindDbName(db, zDb);
  }else{
    iDb = CAPDB_MAX_DB;   /* This means process all schemas */
  }
  if( iDb<0 ){
    rc = CAPDB_ERROR;
    capdbErrorWithMsg(db, CAPDB_ERROR, "unknown database: %s", zDb);
  }else{
    db->busyHandler.nBusy = 0;
    rc = capdbCheckpoint(db, iDb, eMode, pnLog, pnCkpt);
    capdbError(db, rc);
  }
  rc = capdbApiExit(db, rc);

  /* If there are no active statements, clear the interrupt flag at this
  ** point.  */
  if( db->nVdbeActive==0 ){
    AtomicStore(&db->u1.isInterrupted, 0);
  }

  capdb_mutex_leave(db->mutex);
  return rc;
#endif
}


/*
** Checkpoint database zDb. If zDb is NULL, or if the buffer zDb points
** to contains a zero-length string, all attached databases are
** checkpointed.
*/
int capdb_wal_checkpoint(capdb *db, const char *zDb){
  /* EVIDENCE-OF: R-41613-20553 The capdb_wal_checkpoint(D,X) is equivalent to
  ** capdb_wal_checkpoint_v2(D,X,CAPDB_CHECKPOINT_PASSIVE,0,0). */
  return capdb_wal_checkpoint_v2(db,zDb,CAPDB_CHECKPOINT_PASSIVE,0,0);
}

#ifndef CAPDB_OMIT_WAL
/*
** Run a checkpoint on database iDb. This is a no-op if database iDb is
** not currently open in WAL mode.
**
** If a transaction is open on the database being checkpointed, this
** function returns CAPDB_LOCKED and a checkpoint is not attempted. If
** an error occurs while running the checkpoint, an SQLite error code is
** returned (i.e. CAPDB_IOERR). Otherwise, CAPDB_OK.
**
** The mutex on database handle db should be held by the caller. The mutex
** associated with the specific b-tree being checkpointed is taken by
** this function while the checkpoint is running.
**
** If iDb is passed CAPDB_MAX_DB then all attached databases are
** checkpointed. If an error is encountered it is returned immediately -
** no attempt is made to checkpoint any remaining databases.
**
** Parameter eMode is one of CAPDB_CHECKPOINT_PASSIVE, FULL, RESTART
** or TRUNCATE.
*/
int capdbCheckpoint(capdb *db, int iDb, int eMode, int *pnLog, int *pnCkpt){
  int rc = CAPDB_OK;             /* Return code */
  int i;                          /* Used to iterate through attached dbs */
  int bBusy = 0;                  /* True if CAPDB_BUSY has been encountered */

  assert( capdb_mutex_held(db->mutex) );
  assert( !pnLog || *pnLog==-1 );
  assert( !pnCkpt || *pnCkpt==-1 );
  testcase( iDb==CAPDB_MAX_ATTACHED ); /* See forum post a006d86f72 */
  testcase( iDb==CAPDB_MAX_DB );

  for(i=0; i<db->nDb && rc==CAPDB_OK; i++){
    if( i==iDb || iDb==CAPDB_MAX_DB ){
      rc = capdbBtreeCheckpoint(db->aDb[i].pBt, eMode, pnLog, pnCkpt);
      pnLog = 0;
      pnCkpt = 0;
      if( rc==CAPDB_BUSY ){
        bBusy = 1;
        rc = CAPDB_OK;
      }
    }
  }

  return (rc==CAPDB_OK && bBusy) ? CAPDB_BUSY : rc;
}
#endif /* CAPDB_OMIT_WAL */

/*
** This function returns true if main-memory should be used instead of
** a temporary file for transient pager files and statement journals.
** The value returned depends on the value of db->temp_store (runtime
** parameter) and the compile time value of CAPDB_TEMP_STORE. The
** following table describes the relationship between these two values
** and this functions return value.
**
**   CAPDB_TEMP_STORE     db->temp_store     Location of temporary database
**   -----------------     --------------     ------------------------------
**   0                     any                file      (return 0)
**   1                     1                  file      (return 0)
**   1                     2                  memory    (return 1)
**   1                     0                  file      (return 0)
**   2                     1                  file      (return 0)
**   2                     2                  memory    (return 1)
**   2                     0                  memory    (return 1)
**   3                     any                memory    (return 1)
*/
int capdbTempInMemory(const capdb *db){
#if CAPDB_TEMP_STORE==1
  return ( db->temp_store==2 );
#endif
#if CAPDB_TEMP_STORE==2
  return ( db->temp_store!=1 );
#endif
#if CAPDB_TEMP_STORE==3
  UNUSED_PARAMETER(db);
  return 1;
#endif
#if CAPDB_TEMP_STORE<1 || CAPDB_TEMP_STORE>3
  UNUSED_PARAMETER(db);
  return 0;
#endif
}

/*
** Return UTF-8 encoded English language explanation of the most recent
** error.
*/
const char *capdb_errmsg(capdb *db){
  const char *z;
  if( !db ){
    return capdbErrStr(CAPDB_NOMEM_BKPT);
  }
  if( !capdbSafetyCheckSickOrOk(db) ){
    return capdbErrStr(CAPDB_MISUSE_BKPT);
  }
  capdb_mutex_enter(db->mutex);
  if( db->mallocFailed ){
    z = capdbErrStr(CAPDB_NOMEM_BKPT);
  }else{
    testcase( db->pErr==0 );
    z = db->errCode ? (char*)capdb_value_text(db->pErr) : 0;
    assert( !db->mallocFailed );
    if( z==0 ){
      z = capdbErrStr(db->errCode);
    }
  }
  capdb_mutex_leave(db->mutex);
  return z;
}

/*
** Set the error code and error message associated with the database handle.
**
** This routine is intended to be called by outside extensions (ex: the
** Session extension). Internal logic should invoke capdbError() or
** capdbErrorWithMsg() directly.
*/
int capdb_set_errmsg(capdb *db, int errcode, const char *zMsg){
  int rc = CAPDB_OK;
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
  capdb_mutex_enter(db->mutex);
  if( zMsg ){
    capdbErrorWithMsg(db, errcode, "%s", zMsg);
  }else{
    capdbError(db, errcode);
  }
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Return the byte offset of the most recent error
*/
int capdb_error_offset(capdb *db){
  int iOffset = -1;
  if( db && capdbSafetyCheckSickOrOk(db) && db->errCode ){
    capdb_mutex_enter(db->mutex);
    iOffset = db->errByteOffset;
    capdb_mutex_leave(db->mutex);
  }
  return iOffset;
}

#ifndef CAPDB_OMIT_UTF16
/*
** Return UTF-16 encoded English language explanation of the most recent
** error.
*/
const void *capdb_errmsg16(capdb *db){
  static const u16 outOfMem[] = {
    'o', 'u', 't', ' ', 'o', 'f', ' ', 'm', 'e', 'm', 'o', 'r', 'y', 0
  };
  static const u16 misuse[] = {
    'b', 'a', 'd', ' ', 'p', 'a', 'r', 'a', 'm', 'e', 't', 'e', 'r', ' ',
    'o', 'r', ' ', 'o', 't', 'h', 'e', 'r', ' ', 'A', 'P', 'I', ' ',
    'm', 'i', 's', 'u', 's', 'e', 0
  };

  const void *z;
  if( !db ){
    return (void *)outOfMem;
  }
  if( !capdbSafetyCheckSickOrOk(db) ){
    return (void *)misuse;
  }
  capdb_mutex_enter(db->mutex);
  if( db->mallocFailed ){
    z = (void *)outOfMem;
  }else{
    z = capdb_value_text16(db->pErr);
    if( z==0 ){
      capdbErrorWithMsg(db, db->errCode, capdbErrStr(db->errCode));
      z = capdb_value_text16(db->pErr);
    }
    /* A malloc() may have failed within the call to capdb_value_text16()
    ** above. If this is the case, then the db->mallocFailed flag needs to
    ** be cleared before returning. Do this directly, instead of via
    ** capdbApiExit(), to avoid setting the database handle error message.
    */
    capdbOomClear(db);
  }
  capdb_mutex_leave(db->mutex);
  return z;
}
#endif /* CAPDB_OMIT_UTF16 */

/*
** Return the most recent error code generated by an SQLite routine. If NULL is
** passed to this function, we assume a malloc() failed during capdb_open().
*/
int capdb_errcode(capdb *db){
  if( db && !capdbSafetyCheckSickOrOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
  if( !db || db->mallocFailed ){
    return CAPDB_NOMEM_BKPT;
  }
  return db->errCode & db->errMask;
}
int capdb_extended_errcode(capdb *db){
  if( db && !capdbSafetyCheckSickOrOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
  if( !db || db->mallocFailed ){
    return CAPDB_NOMEM_BKPT;
  }
  return db->errCode;
}
int capdb_system_errno(capdb *db){
  return db ? db->iSysErrno : 0;
} 

/*
** Return a string that describes the kind of error specified in the
** argument.  For now, this simply calls the internal capdbErrStr()
** function.
*/
const char *capdb_errstr(int rc){
  return capdbErrStr(rc);
}

/*
** Create a new collating function for database "db".  The name is zName
** and the encoding is enc.
*/
static int createCollation(
  capdb* db,
  const char *zName,
  u8 enc,
  void* pCtx,
  int(*xCompare)(void*,int,const void*,int,const void*),
  void(*xDel)(void*)
){
  CollSeq *pColl;
  int enc2;
 
  assert( capdb_mutex_held(db->mutex) );

  /* If CAPDB_UTF16 is specified as the encoding type, transform this
  ** to one of CAPDB_UTF16LE or CAPDB_UTF16BE using the
  ** CAPDB_UTF16NATIVE macro. CAPDB_UTF16 is not used internally.
  */
  enc2 = enc;
  testcase( enc2==CAPDB_UTF16 );
  testcase( enc2==CAPDB_UTF16_ALIGNED );
  if( enc2==CAPDB_UTF16 || enc2==CAPDB_UTF16_ALIGNED ){
    enc2 = CAPDB_UTF16NATIVE;
  }
  if( enc2<CAPDB_UTF8 || enc2>CAPDB_UTF16BE ){
    return CAPDB_MISUSE_BKPT;
  }

  /* Check if this call is removing or replacing an existing collation
  ** sequence. If so, and there are active VMs, return busy. If there
  ** are no active VMs, invalidate any pre-compiled statements.
  */
  pColl = capdbFindCollSeq(db, (u8)enc2, zName, 0);
  if( pColl && pColl->xCmp ){
    if( db->nVdbeActive ){
      capdbErrorWithMsg(db, CAPDB_BUSY,
        "unable to delete/modify collation sequence due to active statements");
      return CAPDB_BUSY;
    }
    capdbExpirePreparedStatements(db, 0);

    /* If collation sequence pColl was created directly by a call to
    ** capdb_create_collation, and not generated by synthCollSeq(),
    ** then any copies made by synthCollSeq() need to be invalidated.
    ** Also, collation destructor - CollSeq.xDel() - function may need
    ** to be called.
    */
    if( (pColl->enc & ~CAPDB_UTF16_ALIGNED)==enc2 ){
      CollSeq *aColl = capdbHashFind(&db->aCollSeq, zName);
      int j;
      for(j=0; j<3; j++){
        CollSeq *p = &aColl[j];
        if( p->enc==pColl->enc ){
          if( p->xDel ){
            p->xDel(p->pUser);
          }
          p->xCmp = 0;
        }
      }
    }
  }

  pColl = capdbFindCollSeq(db, (u8)enc2, zName, 1);
  if( pColl==0 ) return CAPDB_NOMEM_BKPT;
  pColl->xCmp = xCompare;
  pColl->pUser = pCtx;
  pColl->xDel = xDel;
  pColl->enc = (u8)(enc2 | (enc & CAPDB_UTF16_ALIGNED));
  capdbError(db, CAPDB_OK);
  return CAPDB_OK;
}


/*
** This array defines hard upper bounds on limit values.  The
** initializer must be kept in sync with the CAPDB_LIMIT_*
** #defines in capdb.h.
*/
static const int aHardLimit[] = {
  CAPDB_MAX_LENGTH,
  CAPDB_MAX_SQL_LENGTH,
  CAPDB_MAX_COLUMN,
  CAPDB_MAX_EXPR_DEPTH,
  CAPDB_MAX_COMPOUND_SELECT,
  CAPDB_MAX_VDBE_OP,
  CAPDB_MAX_FUNCTION_ARG,
  CAPDB_MAX_ATTACHED,
  CAPDB_MAX_LIKE_PATTERN_LENGTH,
  CAPDB_MAX_VARIABLE_NUMBER,      /* IMP: R-38091-32352 */
  CAPDB_MAX_TRIGGER_DEPTH,
  CAPDB_MAX_WORKER_THREADS,
  CAPDB_MAX_PARSER_DEPTH,
};

/*
** Make sure the hard limits are set to reasonable values
*/
#if CAPDB_MAX_LENGTH<100
# error CAPDB_MAX_LENGTH must be at least 100
#endif
#if CAPDB_MAX_SQL_LENGTH<100
# error CAPDB_MAX_SQL_LENGTH must be at least 100
#endif
#if CAPDB_MAX_SQL_LENGTH>CAPDB_MAX_LENGTH
# error CAPDB_MAX_SQL_LENGTH must not be greater than CAPDB_MAX_LENGTH
#endif
#if CAPDB_MAX_SQL_LENGTH>2147482624     /* 1024 less than 2^31 */
# error CAPDB_MAX_SQL_LENGTH must not be greater than 2147482624
#endif
#if CAPDB_MAX_COMPOUND_SELECT<2
# error CAPDB_MAX_COMPOUND_SELECT must be at least 2
#endif
#if CAPDB_MAX_VDBE_OP<40
# error CAPDB_MAX_VDBE_OP must be at least 40
#endif
#if CAPDB_MAX_FUNCTION_ARG<0 || CAPDB_MAX_FUNCTION_ARG>32767
# error CAPDB_MAX_FUNCTION_ARG must be between 0 and 32767
#endif
#if CAPDB_MAX_ATTACHED<0 || CAPDB_MAX_ATTACHED>125
# error CAPDB_MAX_ATTACHED must be between 0 and 125
#endif
#if CAPDB_MAX_LIKE_PATTERN_LENGTH<1
# error CAPDB_MAX_LIKE_PATTERN_LENGTH must be at least 1
#endif
#if CAPDB_MAX_COLUMN>32767
# error CAPDB_MAX_COLUMN must not exceed 32767
#endif
#if CAPDB_MAX_TRIGGER_DEPTH<1
# error CAPDB_MAX_TRIGGER_DEPTH must be at least 1
#endif
#if CAPDB_MAX_WORKER_THREADS<0 || CAPDB_MAX_WORKER_THREADS>50
# error CAPDB_MAX_WORKER_THREADS must be between 0 and 50
#endif


/*
** Change the value of a limit.  Report the old value.
** If an invalid limit index is supplied, report -1.
** Make no changes but still report the old value if the
** new limit is negative.
**
** A new lower limit does not shrink existing constructs.
** It merely prevents new constructs that exceed the limit
** from forming.
*/
int capdb_limit(capdb *db, int limitId, int newLimit){
  int oldLimit;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return -1;
  }
#endif

  /* EVIDENCE-OF: R-30189-54097 For each limit category CAPDB_LIMIT_NAME
  ** there is a hard upper bound set at compile-time by a C preprocessor
  ** macro called CAPDB_MAX_NAME. (The "_LIMIT_" in the name is changed to
  ** "_MAX_".)
  */
  assert( aHardLimit[CAPDB_LIMIT_LENGTH]==CAPDB_MAX_LENGTH );
  assert( aHardLimit[CAPDB_LIMIT_SQL_LENGTH]==CAPDB_MAX_SQL_LENGTH );
  assert( aHardLimit[CAPDB_LIMIT_COLUMN]==CAPDB_MAX_COLUMN );
  assert( aHardLimit[CAPDB_LIMIT_EXPR_DEPTH]==CAPDB_MAX_EXPR_DEPTH );
  assert( aHardLimit[CAPDB_LIMIT_PARSER_DEPTH]==CAPDB_MAX_PARSER_DEPTH );
  assert( aHardLimit[CAPDB_LIMIT_COMPOUND_SELECT]==CAPDB_MAX_COMPOUND_SELECT);
  assert( aHardLimit[CAPDB_LIMIT_VDBE_OP]==CAPDB_MAX_VDBE_OP );
  assert( aHardLimit[CAPDB_LIMIT_FUNCTION_ARG]==CAPDB_MAX_FUNCTION_ARG );
  assert( aHardLimit[CAPDB_LIMIT_ATTACHED]==CAPDB_MAX_ATTACHED );
  assert( aHardLimit[CAPDB_LIMIT_LIKE_PATTERN_LENGTH]==
                                               CAPDB_MAX_LIKE_PATTERN_LENGTH );
  assert( aHardLimit[CAPDB_LIMIT_VARIABLE_NUMBER]==CAPDB_MAX_VARIABLE_NUMBER);
  assert( aHardLimit[CAPDB_LIMIT_TRIGGER_DEPTH]==CAPDB_MAX_TRIGGER_DEPTH );
  assert( aHardLimit[CAPDB_LIMIT_WORKER_THREADS]==CAPDB_MAX_WORKER_THREADS );
  assert( CAPDB_LIMIT_PARSER_DEPTH==(CAPDB_N_LIMIT-1) );


  if( limitId<0 || limitId>=CAPDB_N_LIMIT ){
    return -1;
  }
  oldLimit = db->aLimit[limitId];
  if( newLimit>=0 ){                   /* IMP: R-52476-28732 */
    if( newLimit>aHardLimit[limitId] ){
      newLimit = aHardLimit[limitId];  /* IMP: R-51463-25634 */
    }else if( newLimit<CAPDB_MIN_LENGTH && limitId==CAPDB_LIMIT_LENGTH ){
      newLimit = CAPDB_MIN_LENGTH;
    }
    db->aLimit[limitId] = newLimit;
  }
  return oldLimit;                     /* IMP: R-53341-35419 */
}

/*
** This function is used to parse both URIs and non-URI filenames passed by the
** user to API functions capdb_open() or capdb_open_v2(), and for database
** URIs specified as part of ATTACH statements.
**
** The first argument to this function is the name of the VFS to use (or
** a NULL to signify the default VFS) if the URI does not contain a "vfs=xxx"
** query parameter. The second argument contains the URI (or non-URI filename)
** itself. When this function is called the *pFlags variable should contain
** the default flags to open the database handle with. The value stored in
** *pFlags may be updated before returning if the URI filename contains
** "cache=xxx" or "mode=xxx" query parameters.
**
** If successful, CAPDB_OK is returned. In this case *ppVfs is set to point to
** the VFS that should be used to open the database file. *pzFile is set to
** point to a buffer containing the name of the file to open.  The value
** stored in *pzFile is a database name acceptable to capdb_uri_parameter()
** and is in the same format as names created using capdb_create_filename().
** The caller must invoke capdb_free_filename() (not capdb_free()!) on
** the value returned in *pzFile to avoid a memory leak.
**
** If an error occurs, then an SQLite error code is returned and *pzErrMsg
** may be set to point to a buffer containing an English language error
** message. It is the responsibility of the caller to eventually release
** this buffer by calling capdb_free().
*/
int capdbParseUri(
  const char *zDefaultVfs,        /* VFS to use if no "vfs=xxx" query option */
  const char *zUri,               /* Nul-terminated URI to parse */
  unsigned int *pFlags,           /* IN/OUT: CAPDB_OPEN_XXX flags */
  capdb_vfs **ppVfs,            /* OUT: VFS to use */
  char **pzFile,                  /* OUT: Filename component of URI */
  char **pzErrMsg                 /* OUT: Error message (if rc!=CAPDB_OK) */
){
  int rc = CAPDB_OK;
  unsigned int flags = *pFlags;
  const char *zVfs = zDefaultVfs;
  char *zFile;
  char c;
  int nUri = capdbStrlen30(zUri);

  assert( *pzErrMsg==0 );

  if( ((flags & CAPDB_OPEN_URI)                     /* IMP: R-48725-32206 */
       || AtomicLoad(&capdbGlobalConfig.bOpenUri)) /* IMP: R-51689-46548 */
   && nUri>=5 && memcmp(zUri, "file:", 5)==0         /* IMP: R-57884-37496 */
  ){
    char *zOpt;
    int eState;                   /* Parser state when parsing URI */
    int iIn;                      /* Input character index */
    int iOut = 0;                 /* Output character index */
    u64 nByte = nUri+8;           /* Bytes of space to allocate */

    /* Make sure the CAPDB_OPEN_URI flag is set to indicate to the VFS xOpen
    ** method that there may be extra parameters following the file-name.  */
    flags |= CAPDB_OPEN_URI;

    for(iIn=0; iIn<nUri; iIn++) nByte += (zUri[iIn]=='&');
    zFile = capdb_malloc64(nByte);
    if( !zFile ) return CAPDB_NOMEM_BKPT;

    memset(zFile, 0, 4);  /* 4-byte of 0x00 is the start of DB name marker */
    zFile += 4;

    iIn = 5;
#ifdef CAPDB_ALLOW_URI_AUTHORITY
    if( strncmp(zUri+5, "///", 3)==0 ){
      iIn = 7;
      /* The following condition causes URIs with five leading / characters
      ** like file://///host/path to be converted into UNCs like //host/path.
      ** The correct URI for that UNC has only two or four leading / characters
      ** file://host/path or file:////host/path.  But 5 leading slashes is a
      ** common error, we are told, so we handle it as a special case. */
      if( strncmp(zUri+7, "///", 3)==0 ){ iIn++; }
    }else if( strncmp(zUri+5, "//localhost/", 12)==0 ){
      iIn = 16;
    }
#else
    /* Discard the scheme and authority segments of the URI. */
    if( zUri[5]=='/' && zUri[6]=='/' ){
      iIn = 7;
      while( zUri[iIn] && zUri[iIn]!='/' ) iIn++;
      if( iIn!=7 && (iIn!=16 || memcmp("localhost", &zUri[7], 9)) ){
        *pzErrMsg = capdb_mprintf("invalid uri authority: %.*s",
            iIn-7, &zUri[7]);
        rc = CAPDB_ERROR;
        goto parse_uri_out;
      }
    }
#endif

    /* Copy the filename and any query parameters into the zFile buffer.
    ** Decode %HH escape codes along the way.
    **
    ** Within this loop, variable eState may be set to 0, 1 or 2, depending
    ** on the parsing context. As follows:
    **
    **   0: Parsing file-name.
    **   1: Parsing name section of a name=value query parameter.
    **   2: Parsing value section of a name=value query parameter.
    */
    eState = 0;
    while( (c = zUri[iIn])!=0 && c!='#' ){
      iIn++;
      if( c=='%'
       && capdbIsxdigit(zUri[iIn])
       && capdbIsxdigit(zUri[iIn+1])
      ){
        int octet = (capdbHexToInt(zUri[iIn++]) << 4);
        octet += capdbHexToInt(zUri[iIn++]);

        assert( octet>=0 && octet<256 );
        if( octet==0 ){
#ifndef CAPDB_ENABLE_URI_00_ERROR
          /* This branch is taken when "%00" appears within the URI. In this
          ** case we ignore all text in the remainder of the path, name or
          ** value currently being parsed. So ignore the current character
          ** and skip to the next "?", "=" or "&", as appropriate. */
          while( (c = zUri[iIn])!=0 && c!='#'
              && (eState!=0 || c!='?')
              && (eState!=1 || (c!='=' && c!='&'))
              && (eState!=2 || c!='&')
          ){
            iIn++;
          }
          continue;
#else
          /* If ENABLE_URI_00_ERROR is defined, "%00" in a URI is an error. */
          *pzErrMsg = capdb_mprintf("unexpected %%00 in uri");
          rc = CAPDB_ERROR;
          goto parse_uri_out;
#endif
        }
        c = octet;
      }else if( eState==1 && (c=='&' || c=='=') ){
        if( zFile[iOut-1]==0 ){
          /* An empty option name. Ignore this option altogether. */
          while( zUri[iIn] && zUri[iIn]!='#' && zUri[iIn-1]!='&' ) iIn++;
          continue;
        }
        if( c=='&' ){
          zFile[iOut++] = '\0';
        }else{
          eState = 2;
        }
        c = 0;
      }else if( (eState==0 && c=='?') || (eState==2 && c=='&') ){
        c = 0;
        eState = 1;
      }
      zFile[iOut++] = c;
    }
    if( eState==1 ) zFile[iOut++] = '\0';
    memset(zFile+iOut, 0, 4); /* end-of-options + empty journal filenames */

    /* Check if there were any options specified that should be interpreted
    ** here. Options that are interpreted here include "vfs" and those that
    ** correspond to flags that may be passed to the capdb_open_v2()
    ** method. */
    zOpt = &zFile[capdbStrlen30(zFile)+1];
    while( zOpt[0] ){
      int nOpt = capdbStrlen30(zOpt);
      char *zVal = &zOpt[nOpt+1];
      int nVal = capdbStrlen30(zVal);

      if( nOpt==3 && memcmp("vfs", zOpt, 3)==0 ){
        zVfs = zVal;
      }else{
        struct OpenMode {
          const char *z;
          int mode;
        } *aMode = 0;
        char *zModeType = 0;
        int mask = 0;
        int limit = 0;

        if( nOpt==5 && memcmp("cache", zOpt, 5)==0 ){
          static struct OpenMode aCacheMode[] = {
            { "shared",  CAPDB_OPEN_SHAREDCACHE },
            { "private", CAPDB_OPEN_PRIVATECACHE },
            { 0, 0 }
          };

          mask = CAPDB_OPEN_SHAREDCACHE|CAPDB_OPEN_PRIVATECACHE;
          aMode = aCacheMode;
          limit = mask;
          zModeType = "cache";
        }
        if( nOpt==4 && memcmp("mode", zOpt, 4)==0 ){
          static struct OpenMode aOpenMode[] = {
            { "ro",  CAPDB_OPEN_READONLY },
            { "rw",  CAPDB_OPEN_READWRITE },
            { "rwc", CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE },
            { "memory", CAPDB_OPEN_MEMORY },
            { 0, 0 }
          };

          mask = CAPDB_OPEN_READONLY | CAPDB_OPEN_READWRITE
                   | CAPDB_OPEN_CREATE | CAPDB_OPEN_MEMORY;
          aMode = aOpenMode;
          limit = mask & flags;
          zModeType = "access";
        }

        if( aMode ){
          int i;
          int mode = 0;
          for(i=0; aMode[i].z; i++){
            const char *z = aMode[i].z;
            if( nVal==capdbStrlen30(z) && 0==memcmp(zVal, z, nVal) ){
              mode = aMode[i].mode;
              break;
            }
          }
          if( mode==0 ){
            *pzErrMsg = capdb_mprintf("no such %s mode: %s", zModeType, zVal);
            rc = CAPDB_ERROR;
            goto parse_uri_out;
          }
          if( (mode & ~CAPDB_OPEN_MEMORY)>limit ){
            *pzErrMsg = capdb_mprintf("%s mode not allowed: %s",
                                        zModeType, zVal);
            rc = CAPDB_PERM;
            goto parse_uri_out;
          }
          flags = (flags & ~mask) | mode;
        }
      }

      zOpt = &zVal[nVal+1];
    }

  }else{
    zFile = capdb_malloc64(nUri+8);
    if( !zFile ) return CAPDB_NOMEM_BKPT;
    memset(zFile, 0, 4);
    zFile += 4;
    if( nUri ){
      memcpy(zFile, zUri, nUri);
    }
    memset(zFile+nUri, 0, 4);
    flags &= ~CAPDB_OPEN_URI;
  }

  *ppVfs = capdb_vfs_find(zVfs);
  if( *ppVfs==0 ){
    *pzErrMsg = capdb_mprintf("no such vfs: %s", zVfs);
    rc = CAPDB_ERROR;
  }
 parse_uri_out:
  if( rc!=CAPDB_OK ){
    capdb_free_filename(zFile);
    zFile = 0;
  }
  *pFlags = flags;
  *pzFile = zFile;
  return rc;
}

/*
** This routine does the core work of extracting URI parameters from a
** database filename for the capdb_uri_parameter() interface.
*/
static const char *uriParameter(const char *zFilename, const char *zParam){
  zFilename += capdbStrlen30(zFilename) + 1;
  while( ALWAYS(zFilename!=0) && zFilename[0] ){
    int x = strcmp(zFilename, zParam);
    zFilename += capdbStrlen30(zFilename) + 1;
    if( x==0 ) return zFilename;
    zFilename += capdbStrlen30(zFilename) + 1;
  }
  return 0;
}



/*
** This routine does the work of opening a database on behalf of
** capdb_open() and capdb_open16(). The database filename "zFilename" 
** is UTF-8 encoded.
*/
static int openDatabase(
  const char *zFilename, /* Database filename UTF-8 encoded */
  capdb **ppDb,        /* OUT: Returned database handle */
  unsigned int flags,    /* Operational flags */
  const char *zVfs       /* Name of the VFS to use */
){
  capdb *db;                    /* Store allocated handle here */
  int rc;                         /* Return code */
  int isThreadsafe;               /* True for threadsafe connections */
  char *zOpen = 0;                /* Filename argument to pass to BtreeOpen() */
  char *zErrMsg = 0;              /* Error message from capdbParseUri() */
  int i;                          /* Loop counter */

#ifdef CAPDB_ENABLE_API_ARMOR
  if( ppDb==0 ) return CAPDB_MISUSE_BKPT;
#endif
  *ppDb = 0;
#ifndef CAPDB_OMIT_AUTOINIT
  rc = capdb_initialize();
  if( rc ) return rc;
#endif

  if( capdbGlobalConfig.bCoreMutex==0 ){
    isThreadsafe = 0;
  }else if( flags & CAPDB_OPEN_NOMUTEX ){
    isThreadsafe = 0;
  }else if( flags & CAPDB_OPEN_FULLMUTEX ){
    isThreadsafe = 1;
  }else{
    isThreadsafe = capdbGlobalConfig.bFullMutex;
  }

  if( flags & CAPDB_OPEN_PRIVATECACHE ){
    flags &= ~CAPDB_OPEN_SHAREDCACHE;
  }else if( capdbGlobalConfig.sharedCacheEnabled ){
    flags |= CAPDB_OPEN_SHAREDCACHE;
  }

  /* Remove harmful bits from the flags parameter
  **
  ** The CAPDB_OPEN_NOMUTEX and CAPDB_OPEN_FULLMUTEX flags were
  ** dealt with in the previous code block.  Besides these, the only
  ** valid input flags for capdb_open_v2() are CAPDB_OPEN_READONLY,
  ** CAPDB_OPEN_READWRITE, CAPDB_OPEN_CREATE, CAPDB_OPEN_SHAREDCACHE,
  ** CAPDB_OPEN_PRIVATECACHE, CAPDB_OPEN_EXRESCODE, and some reserved
  ** bits.  Silently mask off all other flags.
  */
  flags &=  ~( CAPDB_OPEN_DELETEONCLOSE |
               CAPDB_OPEN_EXCLUSIVE |
               CAPDB_OPEN_MAIN_DB |
               CAPDB_OPEN_TEMP_DB |
               CAPDB_OPEN_TRANSIENT_DB |
               CAPDB_OPEN_MAIN_JOURNAL |
               CAPDB_OPEN_TEMP_JOURNAL |
               CAPDB_OPEN_SUBJOURNAL |
               CAPDB_OPEN_SUPER_JOURNAL |
               CAPDB_OPEN_NOMUTEX |
               CAPDB_OPEN_FULLMUTEX |
               CAPDB_OPEN_WAL
             );

  /* Allocate the sqlite data structure */
  db = capdbMallocZero( sizeof(capdb) );
  if( db==0 ) goto opendb_out;
  if( isThreadsafe
#if defined(CAPDB_THREAD_MISUSE_WARNINGS)
   || capdbGlobalConfig.bCoreMutex
#endif
  ){
    db->mutex = capdbMutexAlloc(CAPDB_MUTEX_RECURSIVE);
    if( db->mutex==0 ){
      capdb_free(db);
      db = 0;
      goto opendb_out;
    }
    if( isThreadsafe==0 ){
      capdbMutexWarnOnContention(db->mutex);
    }
  }
  capdb_mutex_enter(db->mutex);
  db->errMask = (flags & CAPDB_OPEN_EXRESCODE)!=0 ? 0xffffffff : 0xff;
  db->nDb = 2;
  db->eOpenState = CAPDB_STATE_BUSY;
  db->aDb = db->aDbStatic;
  db->lookaside.bDisable = 1;
  db->lookaside.sz = 0;
  db->nFpDigit = 17;

  assert( sizeof(db->aLimit)==sizeof(aHardLimit) );
  memcpy(db->aLimit, aHardLimit, sizeof(db->aLimit));
  db->aLimit[CAPDB_LIMIT_WORKER_THREADS] = CAPDB_DEFAULT_WORKER_THREADS;
  db->autoCommit = 1;
  db->nextAutovac = -1;
  db->szMmap = capdbGlobalConfig.szMmap;
  db->nextPagesize = 0;
  db->init.azInit = capdbStdType; /* Any array of string ptrs will do */
#ifdef CAPDB_ENABLE_SORTER_MMAP
  /* Beginning with version 3.37.0, using the VFS xFetch() API to memory-map
  ** the temporary files used to do external sorts (see code in vdbesort.c)
  ** is disabled. It can still be used either by defining
  ** CAPDB_ENABLE_SORTER_MMAP at compile time or by using the
  ** CAPDB_TESTCTRL_SORTER_MMAP test-control at runtime. */
  db->nMaxSorterMmap = 0x7FFFFFFF;
#endif
  db->flags |= CAPDB_ShortColNames
                 | CAPDB_EnableTrigger
                 | CAPDB_EnableView
                 | CAPDB_CacheSpill
                 | CAPDB_AttachCreate
                 | CAPDB_AttachWrite
                 | CAPDB_Comments
#if !defined(CAPDB_TRUSTED_SCHEMA) || CAPDB_TRUSTED_SCHEMA+0!=0
                 | CAPDB_TrustedSchema
#endif
/* The CAPDB_DQS compile-time option determines the default settings
** for CAPDB_DBCONFIG_DQS_DDL and CAPDB_DBCONFIG_DQS_DML.
**
**    CAPDB_DQS     CAPDB_DBCONFIG_DQS_DDL    CAPDB_DBCONFIG_DQS_DML
**    ----------     -----------------------    -----------------------
**     undefined               on                          on
**         3                   on                          on
**         2                   on                         off
**         1                  off                          on
**         0                  off                         off
**
** Legacy behavior is 3 (double-quoted string literals are allowed anywhere)
** and so that is the default.  But developers are encouraged to use
** -DCAPDB_DQS=0 (best) or -DCAPDB_DQS=1 (second choice) if possible.
*/
#if !defined(CAPDB_DQS)
# define CAPDB_DQS 3
#endif
#if (CAPDB_DQS&1)==1
                 | CAPDB_DqsDML
#endif
#if (CAPDB_DQS&2)==2
                 | CAPDB_DqsDDL
#endif

#if !defined(CAPDB_DEFAULT_AUTOMATIC_INDEX) || CAPDB_DEFAULT_AUTOMATIC_INDEX
                 | CAPDB_AutoIndex
#endif
#if CAPDB_DEFAULT_CKPTFULLFSYNC
                 | CAPDB_CkptFullFSync
#endif
#if CAPDB_DEFAULT_FILE_FORMAT<4
                 | CAPDB_LegacyFileFmt
#endif
#ifdef CAPDB_ENABLE_LOAD_EXTENSION
                 | CAPDB_LoadExtension
#endif
#if CAPDB_DEFAULT_RECURSIVE_TRIGGERS
                 | CAPDB_RecTriggers
#endif
#if defined(CAPDB_DEFAULT_FOREIGN_KEYS) && CAPDB_DEFAULT_FOREIGN_KEYS
                 | CAPDB_ForeignKeys
#endif
#if defined(CAPDB_REVERSE_UNORDERED_SELECTS)
                 | CAPDB_ReverseOrder
#endif
#if defined(CAPDB_ENABLE_OVERSIZE_CELL_CHECK)
                 | CAPDB_CellSizeCk
#endif
#if defined(CAPDB_ENABLE_FTS3_TOKENIZER)
                 | CAPDB_Fts3Tokenizer
#endif
#if defined(CAPDB_ENABLE_QPSG)
                 | CAPDB_EnableQPSG
#endif
#if defined(CAPDB_DEFAULT_DEFENSIVE)
                 | CAPDB_Defensive
#endif
#if defined(CAPDB_DEFAULT_LEGACY_ALTER_TABLE)
                 | CAPDB_LegacyAlter
#endif
#if defined(CAPDB_ENABLE_STMT_SCANSTATUS)
                 | CAPDB_StmtScanStatus
#endif
      ;
  capdbHashInit(&db->aCollSeq);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdbHashInit(&db->aModule);
#endif

  /* Add the default collation sequence BINARY. BINARY works for both UTF-8
  ** and UTF-16, so add a version for each to avoid any unnecessary
  ** conversions. The only error that can occur here is a malloc() failure.
  **
  ** EVIDENCE-OF: R-52786-44878 SQLite defines three built-in collating
  ** functions:
  */
  createCollation(db, capdbStrBINARY, CAPDB_UTF8, 0, binCollFunc, 0);
  createCollation(db, capdbStrBINARY, CAPDB_UTF16BE, 0, binCollFunc, 0);
  createCollation(db, capdbStrBINARY, CAPDB_UTF16LE, 0, binCollFunc, 0);
  createCollation(db, "NOCASE", CAPDB_UTF8, 0, nocaseCollatingFunc, 0);
  createCollation(db, "RTRIM", CAPDB_UTF8, 0, rtrimCollFunc, 0);
  if( db->mallocFailed ){
    goto opendb_out;
  }

#if CAPDB_OS_UNIX && defined(CAPDB_OS_KV_OPTIONAL)
  /* Process magic filenames ":localStorage:" and ":sessionStorage:" */
  if( zFilename && zFilename[0]==':' ){
    if( strcmp(zFilename, ":localStorage:")==0 ){
      zFilename = "file:local?vfs=kvvfs";
      flags |= CAPDB_OPEN_URI;
    }else if( strcmp(zFilename, ":sessionStorage:")==0 ){
      zFilename = "file:session?vfs=kvvfs";
      flags |= CAPDB_OPEN_URI;
    }
  }
#endif /* CAPDB_OS_UNIX && defined(CAPDB_OS_KV_OPTIONAL) */

  /* Parse the filename/URI argument
  **
  ** Only allow sensible combinations of bits in the flags argument. 
  ** Throw an error if any non-sense combination is used.  If we
  ** do not block illegal combinations here, it could trigger
  ** assert() statements in deeper layers.  Sensible combinations
  ** are:
  **
  **  1:  CAPDB_OPEN_READONLY
  **  2:  CAPDB_OPEN_READWRITE
  **  6:  CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE
  */
  db->openFlags = flags;
  assert( CAPDB_OPEN_READONLY  == 0x01 );
  assert( CAPDB_OPEN_READWRITE == 0x02 );
  assert( CAPDB_OPEN_CREATE    == 0x04 );
  testcase( (1<<(flags&7))==0x02 ); /* READONLY */
  testcase( (1<<(flags&7))==0x04 ); /* READWRITE */
  testcase( (1<<(flags&7))==0x40 ); /* READWRITE | CREATE */
  if( ((1<<(flags&7)) & 0x46)==0 ){
    rc = CAPDB_MISUSE_BKPT;  /* IMP: R-18321-05872 */
  }else{
    if( zFilename==0 ) zFilename = ":memory:";
    rc = capdbParseUri(zVfs, zFilename, &flags, &db->pVfs, &zOpen, &zErrMsg);
  }
  if( rc!=CAPDB_OK ){
    if( rc==CAPDB_NOMEM ) capdbOomFault(db);
    capdbErrorWithMsg(db, rc, zErrMsg ? "%s" : 0, zErrMsg);
    capdb_free(zErrMsg);
    goto opendb_out;
  }
  assert( db->pVfs!=0 );
#if CAPDB_OS_KV || defined(CAPDB_OS_KV_OPTIONAL)
  if( capdb_stricmp(db->pVfs->zName, "kvvfs")==0 ){
    db->temp_store = 2;
  }
#endif

  /* Open the backend database driver */
  rc = capdbBtreeOpen(db->pVfs, zOpen, db, &db->aDb[0].pBt, 0,
                        flags | CAPDB_OPEN_MAIN_DB);
  if( rc!=CAPDB_OK ){
    if( rc==CAPDB_IOERR_NOMEM ){
      rc = CAPDB_NOMEM_BKPT;
    }
    capdbError(db, rc);
    goto opendb_out;
  }
  capdbBtreeEnter(db->aDb[0].pBt);
  db->aDb[0].pSchema = capdbSchemaGet(db, db->aDb[0].pBt);
  if( !db->mallocFailed ){
    capdbSetTextEncoding(db, SCHEMA_ENC(db));
  }
  capdbBtreeLeave(db->aDb[0].pBt);
  db->aDb[1].pSchema = capdbSchemaGet(db, 0);

  /* The default safety_level for the main database is FULL; for the temp
  ** database it is OFF. This matches the pager layer defaults. 
  */
  db->aDb[0].zDbSName = "main";
  db->aDb[0].safety_level = CAPDB_DEFAULT_SYNCHRONOUS+1;
  db->aDb[1].zDbSName = "temp";
  db->aDb[1].safety_level = PAGER_SYNCHRONOUS_OFF;

  db->eOpenState = CAPDB_STATE_OPEN;
  if( db->mallocFailed ){
    goto opendb_out;
  }

  /* Register all built-in functions, but do not attempt to read the
  ** database schema yet. This is delayed until the first time the database
  ** is accessed.
  */
  capdbError(db, CAPDB_OK);
  capdbRegisterPerConnectionBuiltinFunctions(db);
  rc = capdb_errcode(db);


  /* Load compiled-in extensions */
  for(i=0; rc==CAPDB_OK && i<ArraySize(capdbBuiltinExtensions); i++){
    rc = capdbBuiltinExtensions[i](db);
  }

  /* Load automatic extensions - extensions that have been registered
  ** using the capdb_automatic_extension() API.
  */
  if( rc==CAPDB_OK ){
    capdbAutoLoadExtensions(db);
    rc = capdb_errcode(db);
    if( rc!=CAPDB_OK ){
      goto opendb_out;
    }
  }

#ifdef CAPDB_ENABLE_INTERNAL_FUNCTIONS
  /* Testing use only!!! The -DCAPDB_ENABLE_INTERNAL_FUNCTIONS=1 compile-time
  ** option gives access to internal functions by default. 
  ** Testing use only!!! */
  db->mDbFlags |= DBFLAG_InternalFunc;
#endif

  /* -DCAPDB_DEFAULT_LOCKING_MODE=1 makes EXCLUSIVE the default locking
  ** mode.  -DCAPDB_DEFAULT_LOCKING_MODE=0 make NORMAL the default locking
  ** mode.  Doing nothing at all also makes NORMAL the default.
  */
#ifdef CAPDB_DEFAULT_LOCKING_MODE
  db->dfltLockMode = CAPDB_DEFAULT_LOCKING_MODE;
  capdbPagerLockingMode(capdbBtreePager(db->aDb[0].pBt),
                          CAPDB_DEFAULT_LOCKING_MODE);
#endif

  if( rc ) capdbError(db, rc);

  /* Enable the lookaside-malloc subsystem */
  setupLookaside(db, 0, capdbGlobalConfig.szLookaside,
                        capdbGlobalConfig.nLookaside);

  capdb_wal_autocheckpoint(db, CAPDB_DEFAULT_WAL_AUTOCHECKPOINT);

opendb_out:
  if( db ){
    assert( db->mutex!=0 || isThreadsafe==0
           || capdbGlobalConfig.bFullMutex==0 );
    capdb_mutex_leave(db->mutex);
  }
  rc = capdb_errcode(db);
  assert( db!=0 || (rc&0xff)==CAPDB_NOMEM );
  if( (rc&0xff)==CAPDB_NOMEM ){
    capdb_close(db);
    db = 0;
  }else if( rc!=CAPDB_OK ){
    db->eOpenState = CAPDB_STATE_SICK;
  }
  *ppDb = db;
#ifdef CAPDB_ENABLE_SQLLOG
  if( capdbGlobalConfig.xSqllog ){
    /* Opening a db handle. Fourth parameter is passed 0. */
    void *pArg = capdbGlobalConfig.pSqllogArg;
    capdbGlobalConfig.xSqllog(pArg, db, zFilename, 0);
  }
#endif
  capdb_free_filename(zOpen);
  return rc;
}


/*
** Open a new database handle.
*/
int capdb_open(
  const char *zFilename,
  capdb **ppDb
){
  return openDatabase(zFilename, ppDb,
                      CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE, 0);
}
int capdb_open_v2(
  const char *filename,   /* Database filename (UTF-8) */
  capdb **ppDb,         /* OUT: SQLite db handle */
  int flags,              /* Flags */
  const char *zVfs        /* Name of VFS module to use */
){
  return openDatabase(filename, ppDb, (unsigned int)flags, zVfs);
}

#ifndef CAPDB_OMIT_UTF16
/*
** Open a new database handle.
*/
int capdb_open16(
  const void *zFilename,
  capdb **ppDb
){
  char const *zFilename8;   /* zFilename encoded in UTF-8 instead of UTF-16 */
  capdb_value *pVal;
  int rc;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( ppDb==0 ) return CAPDB_MISUSE_BKPT;
#endif
  *ppDb = 0;
#ifndef CAPDB_OMIT_AUTOINIT
  rc = capdb_initialize();
  if( rc ) return rc;
#endif
  if( zFilename==0 ) zFilename = "\000\000";
  pVal = capdbValueNew(0);
  capdbValueSetStr(pVal, -1, zFilename, CAPDB_UTF16NATIVE, CAPDB_STATIC);
  zFilename8 = capdbValueText(pVal, CAPDB_UTF8);
  if( zFilename8 ){
    rc = openDatabase(zFilename8, ppDb,
                      CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE, 0);
    assert( *ppDb || rc==CAPDB_NOMEM );
    if( rc==CAPDB_OK && !DbHasProperty(*ppDb, 0, DB_SchemaLoaded) ){
      SCHEMA_ENC(*ppDb) = ENC(*ppDb) = CAPDB_UTF16NATIVE;
    }
  }else{
    rc = CAPDB_NOMEM_BKPT;
  }
  capdbValueFree(pVal);

  return rc & 0xff;
}
#endif /* CAPDB_OMIT_UTF16 */

/*
** Register a new collation sequence with the database handle db.
*/
int capdb_create_collation(
  capdb* db,
  const char *zName,
  int enc,
  void* pCtx,
  int(*xCompare)(void*,int,const void*,int,const void*)
){
  return capdb_create_collation_v2(db, zName, enc, pCtx, xCompare, 0);
}

/*
** Register a new collation sequence with the database handle db.
*/
int capdb_create_collation_v2(
  capdb* db,
  const char *zName,
  int enc,
  void* pCtx,
  int(*xCompare)(void*,int,const void*,int,const void*),
  void(*xDel)(void*)
){
  int rc;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zName==0 ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  assert( !db->mallocFailed );
  rc = createCollation(db, zName, (u8)enc, pCtx, xCompare, xDel);
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

#ifndef CAPDB_OMIT_UTF16
/*
** Register a new collation sequence with the database handle db.
*/
int capdb_create_collation16(
  capdb* db,
  const void *zName,
  int enc,
  void* pCtx,
  int(*xCompare)(void*,int,const void*,int,const void*)
){
  int rc = CAPDB_OK;
  char *zName8;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zName==0 ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  assert( !db->mallocFailed );
  zName8 = capdbUtf16to8(db, zName, -1, CAPDB_UTF16NATIVE);
  if( zName8 ){
    rc = createCollation(db, zName8, (u8)enc, pCtx, xCompare, 0);
    capdbDbFree(db, zName8);
  }
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}
#endif /* CAPDB_OMIT_UTF16 */

/*
** Register a collation sequence factory callback with the database handle
** db. Replace any previously installed collation sequence factory.
*/
int capdb_collation_needed(
  capdb *db,
  void *pCollNeededArg,
  void(*xCollNeeded)(void*,capdb*,int eTextRep,const char*)
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  db->xCollNeeded = xCollNeeded;
  db->xCollNeeded16 = 0;
  db->pCollNeededArg = pCollNeededArg;
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

#ifndef CAPDB_OMIT_UTF16
/*
** Register a collation sequence factory callback with the database handle
** db. Replace any previously installed collation sequence factory.
*/
int capdb_collation_needed16(
  capdb *db,
  void *pCollNeededArg,
  void(*xCollNeeded16)(void*,capdb*,int eTextRep,const void*)
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  db->xCollNeeded = 0;
  db->xCollNeeded16 = xCollNeeded16;
  db->pCollNeededArg = pCollNeededArg;
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}
#endif /* CAPDB_OMIT_UTF16 */

/*
** Find existing client data.
*/
void *capdb_get_clientdata(capdb *db, const char *zName){
  DbClientData *p;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zName || !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(db->mutex);
  for(p=db->pDbData; p; p=p->pNext){
    if( strcmp(p->zName, zName)==0 ){
      void *pResult = p->pData;
      capdb_mutex_leave(db->mutex);
      return pResult;
    }
  }
  capdb_mutex_leave(db->mutex);
  return 0;
}

/*
** Add new client data to a database connection.
*/
int capdb_set_clientdata(
  capdb *db,                   /* Attach client data to this connection */
  const char *zName,             /* Name of the client data */
  void *pData,                   /* The client data itself */
  void (*xDestructor)(void*)     /* Destructor */
){
  DbClientData *p, **pp;
  capdb_mutex_enter(db->mutex);
  pp = &db->pDbData;
  for(p=db->pDbData; p && strcmp(p->zName,zName); p=p->pNext){
    pp = &p->pNext;
  }
  if( p ){
    assert( p->pData!=0 );
    if( p->xDestructor ) p->xDestructor(p->pData);
    if( pData==0 ){
      *pp = p->pNext;
      capdb_free(p);
      capdb_mutex_leave(db->mutex);
      return CAPDB_OK;
    }
  }else if( pData==0 ){
    capdb_mutex_leave(db->mutex);
    return CAPDB_OK;
  }else{
    size_t n = strlen(zName);
    p = capdb_malloc64( SZ_DBCLIENTDATA(n+1) );
    if( p==0 ){
      if( xDestructor ) xDestructor(pData);
      capdb_mutex_leave(db->mutex);
      return CAPDB_NOMEM;
    }
    memcpy(p->zName, zName, n+1);
    p->pNext = db->pDbData;
    db->pDbData = p;
  }
  p->pData = pData;
  p->xDestructor = xDestructor;
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}


#ifndef CAPDB_OMIT_DEPRECATED
/*
** This function is now an anachronism. It used to be used to recover from a
** malloc() failure, but SQLite now does this automatically.
*/
int capdb_global_recover(void){
  return CAPDB_OK;
}
#endif

/*
** Test to see whether or not the database connection is in autocommit
** mode.  Return TRUE if it is and FALSE if not.  Autocommit mode is on
** by default.  Autocommit is disabled by a BEGIN statement and reenabled
** by the next COMMIT or ROLLBACK.
*/
int capdb_get_autocommit(capdb *db){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return db->autoCommit;
}

/*
** The following routines are substitutes for constants CAPDB_CORRUPT,
** CAPDB_MISUSE, CAPDB_CANTOPEN, CAPDB_NOMEM and possibly other error
** constants.  They serve two purposes:
**
**   1.  Serve as a convenient place to set a breakpoint in a debugger
**       to detect when version error conditions occurs.
**
**   2.  Invoke capdb_log() to provide the source code location where
**       a low-level error is first detected.
*/
int capdbReportError(int iErr, int lineno, const char *zType){
  capdb_log(iErr, "%s at line %d of [%.10s]",
              zType, lineno, 20+capdb_sourceid());
  return iErr;
}
int capdbCorruptError(int lineno){
  testcase( capdbGlobalConfig.xLog!=0 );
  return capdbReportError(CAPDB_CORRUPT, lineno, "database corruption");
}
int capdbMisuseError(int lineno){
  testcase( capdbGlobalConfig.xLog!=0 );
  return capdbReportError(CAPDB_MISUSE, lineno, "misuse");
}
int capdbCantopenError(int lineno){
  testcase( capdbGlobalConfig.xLog!=0 );
  return capdbReportError(CAPDB_CANTOPEN, lineno, "cannot open file");
}
#if defined(CAPDB_DEBUG) || defined(CAPDB_ENABLE_CORRUPT_PGNO)
int capdbCorruptPgnoError(int lineno, Pgno pgno){
  char zMsg[100];
  capdb_snprintf(sizeof(zMsg), zMsg, "database corruption page %d", pgno);
  testcase( capdbGlobalConfig.xLog!=0 );
  return capdbReportError(CAPDB_CORRUPT, lineno, zMsg);
}
#endif
#ifdef CAPDB_DEBUG
int capdbNomemError(int lineno){
  testcase( capdbGlobalConfig.xLog!=0 );
  return capdbReportError(CAPDB_NOMEM, lineno, "OOM");
}
int capdbIoerrnomemError(int lineno){
  testcase( capdbGlobalConfig.xLog!=0 );
  return capdbReportError(CAPDB_IOERR_NOMEM, lineno, "I/O OOM error");
}
#endif

#ifndef CAPDB_OMIT_DEPRECATED
/*
** This is a convenience routine that makes sure that all thread-specific
** data for this thread has been deallocated.
**
** SQLite no longer uses thread-specific data so this routine is now a
** no-op.  It is retained for historical compatibility.
*/
void capdb_thread_cleanup(void){
}
#endif

/*
** Return meta information about a specific column of a database table.
** See comment in capdb.h (sqlite.h.in) for details.
*/
int capdb_table_column_metadata(
  capdb *db,                /* Connection handle */
  const char *zDbName,        /* Database name or NULL */
  const char *zTableName,     /* Table name */
  const char *zColumnName,    /* Column name */
  char const **pzDataType,    /* OUTPUT: Declared data type */
  char const **pzCollSeq,     /* OUTPUT: Collation sequence name */
  int *pNotNull,              /* OUTPUT: True if NOT NULL constraint exists */
  int *pPrimaryKey,           /* OUTPUT: True if column part of PK */
  int *pAutoinc               /* OUTPUT: True if column is auto-increment */
){
  int rc;
  char *zErrMsg = 0;
  Table *pTab = 0;
  Column *pCol = 0;
  int iCol = 0;
  char const *zDataType = 0;
  char const *zCollSeq = 0;
  int notnull = 0;
  int primarykey = 0;
  int autoinc = 0;


#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zTableName==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif

  /* Ensure the database schema has been loaded */
  capdb_mutex_enter(db->mutex);
  capdbBtreeEnterAll(db);
  rc = capdbInit(db, &zErrMsg);
  if( CAPDB_OK!=rc ){
    goto error_out;
  }

  /* Locate the table in question */
  pTab = capdbFindTable(db, zTableName, zDbName);
  if( !pTab || IsView(pTab) ){
    pTab = 0;
    goto error_out;
  }

  /* Find the column for which info is requested */
  if( zColumnName==0 ){
    /* Query for existence of table only */
  }else{
    iCol = capdbColumnIndex(pTab, zColumnName);
    if( iCol>=0 ){
      pCol = &pTab->aCol[iCol];
    }else{
      if( HasRowid(pTab) && capdbIsRowid(zColumnName) ){
        iCol = pTab->iPKey;
        pCol = iCol>=0 ? &pTab->aCol[iCol] : 0;
      }else{
        pTab = 0;
        goto error_out;
      }
    }
  }

  /* The following block stores the meta information that will be returned
  ** to the caller in local variables zDataType, zCollSeq, notnull, primarykey
  ** and autoinc. At this point there are two possibilities:
  **
  **     1. The specified column name was rowid", "oid" or "_rowid_"
  **        and there is no explicitly declared IPK column.
  **
  **     2. The table is not a view and the column name identified an
  **        explicitly declared column. Copy meta information from *pCol.
  */
  if( pCol ){
    zDataType = capdbColumnType(pCol,0);
    zCollSeq = capdbColumnColl(pCol);
    notnull = pCol->notNull!=0;
    primarykey  = (pCol->colFlags & COLFLAG_PRIMKEY)!=0;
    autoinc = pTab->iPKey==iCol && (pTab->tabFlags & TF_Autoincrement)!=0;
  }else{
    zDataType = "INTEGER";
    primarykey = 1;
  }
  if( !zCollSeq ){
    zCollSeq = capdbStrBINARY;
  }

error_out:
  capdbBtreeLeaveAll(db);

  /* Whether the function call succeeded or failed, set the output parameters
  ** to whatever their local counterparts contain. If an error did occur,
  ** this has the effect of zeroing all output parameters.
  */
  if( pzDataType ) *pzDataType = zDataType;
  if( pzCollSeq ) *pzCollSeq = zCollSeq;
  if( pNotNull ) *pNotNull = notnull;
  if( pPrimaryKey ) *pPrimaryKey = primarykey;
  if( pAutoinc ) *pAutoinc = autoinc;

  if( CAPDB_OK==rc && !pTab ){
    capdbDbFree(db, zErrMsg);
    zErrMsg = capdbMPrintf(db, "no such table column: %s.%s", zTableName,
        zColumnName);
    rc = CAPDB_ERROR;
  }
  capdbErrorWithMsg(db, rc, (zErrMsg?"%s":0), zErrMsg);
  capdbDbFree(db, zErrMsg);
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Sleep for a little while.  Return the amount of time slept.
*/
int capdb_sleep(int ms){
  capdb_vfs *pVfs;
  int rc;
  pVfs = capdb_vfs_find(0);
  if( pVfs==0 ) return 0;

  /* This function works in milliseconds, but the underlying OsSleep()
  ** API uses microseconds. Hence the 1000's.
  */
  rc = (capdbOsSleep(pVfs, ms<0 ? 0 : 1000*ms)/1000);
  return rc;
}

/*
** Enable or disable the extended result codes.
*/
int capdb_extended_result_codes(capdb *db, int onoff){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  db->errMask = onoff ? 0xffffffff : 0xff;
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

/*
** Invoke the xFileControl method on a particular database.
*/
int capdb_file_control(capdb *db, const char *zDbName, int op, void *pArg){
  int rc = CAPDB_ERROR;
  Btree *pBtree;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  pBtree = capdbDbNameToBtree(db, zDbName);
  if( pBtree ){
    Pager *pPager;
    capdb_file *fd;
    capdbBtreeEnter(pBtree);
    pPager = capdbBtreePager(pBtree);
    assert( pPager!=0 );
    fd = capdbPagerFile(pPager);
    assert( fd!=0 );
    if( op==CAPDB_FCNTL_FILE_POINTER ){
      *(capdb_file**)pArg = fd;
      rc = CAPDB_OK;
    }else if( op==CAPDB_FCNTL_VFS_POINTER ){
      *(capdb_vfs**)pArg = capdbPagerVfs(pPager);
      rc = CAPDB_OK;
    }else if( op==CAPDB_FCNTL_JOURNAL_POINTER ){
      *(capdb_file**)pArg = capdbPagerJrnlFile(pPager);
      rc = CAPDB_OK;
    }else if( op==CAPDB_FCNTL_DATA_VERSION ){
      *(unsigned int*)pArg = capdbPagerDataVersion(pPager);
      rc = CAPDB_OK;
    }else if( op==CAPDB_FCNTL_RESERVE_BYTES ){
      int iNew = *(int*)pArg;
      *(int*)pArg = capdbBtreeGetRequestedReserve(pBtree);
      if( iNew>=0 && iNew<=255 ){
        capdbBtreeSetPageSize(pBtree, 0, iNew, 0);
      }
      rc = CAPDB_OK;
    }else if( op==CAPDB_FCNTL_RESET_CACHE ){
      capdbBtreeClearCache(pBtree);
      rc = CAPDB_OK;
    }else{
      int nSave = db->busyHandler.nBusy;
      rc = capdbOsFileControl(fd, op, pArg);
      db->busyHandler.nBusy = nSave;
    }
    capdbBtreeLeave(pBtree);
  }
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Interface to the testing logic.
*/
int capdb_test_control(int op, ...){
  int rc = 0;
#ifdef CAPDB_UNTESTABLE
  UNUSED_PARAMETER(op);
#else
  va_list ap;
  va_start(ap, op);
  switch( op ){

    /*
    ** Save the current state of the PRNG.
    */
    case CAPDB_TESTCTRL_PRNG_SAVE: {
      capdbPrngSaveState();
      break;
    }

    /*
    ** Restore the state of the PRNG to the last state saved using
    ** PRNG_SAVE.  If PRNG_SAVE has never before been called, then
    ** this verb acts like PRNG_RESET.
    */
    case CAPDB_TESTCTRL_PRNG_RESTORE: {
      capdbPrngRestoreState();
      break;
    }

    /*  capdb_test_control(CAPDB_TESTCTRL_PRNG_SEED, int x, capdb *db);
    **
    ** Control the seed for the pseudo-random number generator (PRNG) that
    ** is built into SQLite.  Cases:
    **
    **    x!=0 && db!=0       Seed the PRNG to the current value of the
    **                        schema cookie in the main database for db, or
    **                        x if the schema cookie is zero.  This case
    **                        is convenient to use with database fuzzers
    **                        as it allows the fuzzer some control over the
    **                        the PRNG seed.
    **
    **    x!=0 && db==0       Seed the PRNG to the value of x.
    **
    **    x==0 && db==0       Revert to default behavior of using the
    **                        xRandomness method on the primary VFS.
    **
    ** This test-control also resets the PRNG so that the new seed will
    ** be used for the next call to capdb_randomness().
    */
#ifndef CAPDB_OMIT_WSD
    case CAPDB_TESTCTRL_PRNG_SEED: {
      int x = va_arg(ap, int);
      int y;
      capdb *db = va_arg(ap, capdb*);
      assert( db==0 || db->aDb[0].pSchema!=0 );
      if( db && (y = db->aDb[0].pSchema->schema_cookie)!=0 ){ x = y; }
      capdbConfig.iPrngSeed = x;
      capdb_randomness(0,0);
      break;
    }
#endif

    /*  capdb_test_control(CAPDB_TESTCTRL_FK_NO_ACTION, capdb *db, int b);
    **
    ** If b is true, then activate the CAPDB_FkNoAction setting.  If b is
    ** false then clear that setting.  If the CAPDB_FkNoAction setting is
    ** enabled, all foreign key ON DELETE and ON UPDATE actions behave as if
    ** they were NO ACTION, regardless of how they are defined.
    **
    ** NB:  One must usually run "PRAGMA writable_schema=RESET" after
    ** using this test-control, before it will take full effect.  failing
    ** to reset the schema can result in some unexpected behavior.
    */
    case CAPDB_TESTCTRL_FK_NO_ACTION: {
      capdb *db = va_arg(ap, capdb*);
      int b = va_arg(ap, int);
      if( b ){
        db->flags |= CAPDB_FkNoAction;
      }else{
        db->flags &= ~CAPDB_FkNoAction;
      }
      break;
    }

    /*
    **  capdb_test_control(BITVEC_TEST, size, program)
    **
    ** Run a test against a Bitvec object of size.  The program argument
    ** is an array of integers that defines the test.  Return -1 on a
    ** memory allocation error, 0 on success, or non-zero for an error.
    ** See the capdbBitvecBuiltinTest() for additional information.
    */
    case CAPDB_TESTCTRL_BITVEC_TEST: {
      int sz = va_arg(ap, int);
      int *aProg = va_arg(ap, int*);
      rc = capdbBitvecBuiltinTest(sz, aProg);
      break;
    }

    /*
    **  capdb_test_control(FAULT_INSTALL, xCallback)
    **
    ** Arrange to invoke xCallback() whenever capdbFaultSim() is called,
    ** if xCallback is not NULL.
    **
    ** As a test of the fault simulator mechanism itself, capdbFaultSim(0)
    ** is called immediately after installing the new callback and the return
    ** value from capdbFaultSim(0) becomes the return from
    ** capdb_test_control().
    */
    case CAPDB_TESTCTRL_FAULT_INSTALL: {
      /* A bug in MSVC prevents it from understanding pointers to functions
      ** types in the second argument to va_arg().  Work around the problem
      ** using a typedef.
      ** http://support.microsoft.com/kb/47961  <-- dead hyperlink
      ** Search at http://web.archive.org/ to find the 2015-03-16 archive
      ** of the link above to see the original text.
      ** capdbGlobalConfig.xTestCallback = va_arg(ap, int(*)(int));
      */
      typedef int(*capdbFaultFuncType)(int);
      capdbGlobalConfig.xTestCallback = va_arg(ap, capdbFaultFuncType);
      rc = capdbFaultSim(0);
      break;
    }

    /*
    **  capdb_test_control(BENIGN_MALLOC_HOOKS, xBegin, xEnd)
    **
    ** Register hooks to call to indicate which malloc() failures
    ** are benign.
    */
    case CAPDB_TESTCTRL_BENIGN_MALLOC_HOOKS: {
      typedef void (*void_function)(void);
      void_function xBenignBegin;
      void_function xBenignEnd;
      xBenignBegin = va_arg(ap, void_function);
      xBenignEnd = va_arg(ap, void_function);
      capdbBenignMallocHooks(xBenignBegin, xBenignEnd);
      break;
    }

    /*
    **  capdb_test_control(CAPDB_TESTCTRL_PENDING_BYTE, unsigned int X)
    **
    ** Set the PENDING byte to the value in the argument, if X>0.
    ** Make no changes if X==0.  Return the value of the pending byte
    ** as it existing before this routine was called.
    **
    ** IMPORTANT:  Changing the PENDING byte from 0x40000000 results in
    ** an incompatible database file format.  Changing the PENDING byte
    ** while any database connection is open results in undefined and
    ** deleterious behavior.
    */
    case CAPDB_TESTCTRL_PENDING_BYTE: {
      rc = PENDING_BYTE;
#ifndef CAPDB_OMIT_WSD
      {
        unsigned int newVal = va_arg(ap, unsigned int);
        if( newVal ) capdbPendingByte = newVal;
      }
#endif
      break;
    }

    /*
    **  capdb_test_control(CAPDB_TESTCTRL_ASSERT, int X)
    **
    ** This action provides a run-time test to see whether or not
    ** assert() was enabled at compile-time.  If X is true and assert()
    ** is enabled, then the return value is true.  If X is true and
    ** assert() is disabled, then the return value is zero.  If X is
    ** false and assert() is enabled, then the assertion fires and the
    ** process aborts.  If X is false and assert() is disabled, then the
    ** return value is zero.
    */
    case CAPDB_TESTCTRL_ASSERT: {
      volatile int x = 0;
      assert( /*side-effects-ok*/ (x = va_arg(ap,int))!=0 );
      rc = x;
#if defined(CAPDB_DEBUG)
      /* Invoke these debugging routines so that the compiler does not
      ** issue "defined but not used" warnings. */
      if( x==9999 ){
        capdbShowExpr(0);
        capdbShowExprList(0);
        capdbShowIdList(0);
        capdbShowSrcList(0);
        capdbShowWith(0);
        capdbShowUpsert(0);
#ifndef CAPDB_OMIT_TRIGGER
        capdbShowTriggerStep(0);
        capdbShowTriggerStepList(0);
        capdbShowTrigger(0);
        capdbShowTriggerList(0);
#endif
#ifndef CAPDB_OMIT_WINDOWFUNC
        capdbShowWindow(0);
        capdbShowWinFunc(0);
#endif
        capdbShowSelect(0);
      }
#endif
      break;
    }


    /*
    **  capdb_test_control(CAPDB_TESTCTRL_ALWAYS, int X)
    **
    ** This action provides a run-time test to see how the ALWAYS and
    ** NEVER macros were defined at compile-time.
    **
    ** The return value is ALWAYS(X) if X is true, or 0 if X is false.
    **
    ** The recommended test is X==2.  If the return value is 2, that means
    ** ALWAYS() and NEVER() are both no-op pass-through macros, which is the
    ** default setting.  If the return value is 1, then ALWAYS() is either
    ** hard-coded to true or else it asserts if its argument is false.
    ** The first behavior (hard-coded to true) is the case if
    ** CAPDB_TESTCTRL_ASSERT shows that assert() is disabled and the second
    ** behavior (assert if the argument to ALWAYS() is false) is the case if
    ** CAPDB_TESTCTRL_ASSERT shows that assert() is enabled.
    **
    ** The run-time test procedure might look something like this:
    **
    **    if( capdb_test_control(CAPDB_TESTCTRL_ALWAYS, 2)==2 ){
    **      // ALWAYS() and NEVER() are no-op pass-through macros
    **    }else if( capdb_test_control(CAPDB_TESTCTRL_ASSERT, 1) ){
    **      // ALWAYS(x) asserts that x is true. NEVER(x) asserts x is false.
    **    }else{
    **      // ALWAYS(x) is a constant 1.  NEVER(x) is a constant 0.
    **    }
    */
    case CAPDB_TESTCTRL_ALWAYS: {
      int x = va_arg(ap,int);
      rc = x ? ALWAYS(x) : 0;
      break;
    }

    /*
    **   capdb_test_control(CAPDB_TESTCTRL_BYTEORDER);
    **
    ** The integer returned reveals the byte-order of the computer on which
    ** SQLite is running:
    **
    **       1     big-endian,    determined at run-time
    **      10     little-endian, determined at run-time
    **  432101     big-endian,    determined at compile-time
    **  123410     little-endian, determined at compile-time
    */
    case CAPDB_TESTCTRL_BYTEORDER: {
      rc = CAPDB_BYTEORDER*100 + CAPDB_LITTLEENDIAN*10 + CAPDB_BIGENDIAN;
      break;
    }

    /*  capdb_test_control(CAPDB_TESTCTRL_OPTIMIZATIONS, capdb *db, int N)
    **
    ** Enable or disable various optimizations for testing purposes.  The
    ** argument N is a bitmask of optimizations to be disabled.  For normal
    ** operation N should be 0.  The idea is that a test program (like the
    ** SQL Logic Test or SLT test module) can run the same SQL multiple times
    ** with various optimizations disabled to verify that the same answer
    ** is obtained in every case.
    */
    case CAPDB_TESTCTRL_OPTIMIZATIONS: {
      capdb *db = va_arg(ap, capdb*);
      db->dbOptFlags = va_arg(ap, u32);
      break;
    }

    /*  capdb_test_control(CAPDB_TESTCTRL_GETOPT, capdb *db, int *N)
    **
    ** Write the current optimization settings into *N.  A zero bit means that
    ** the optimization is on, and a 1 bit means that the optimization is off.
    */
    case CAPDB_TESTCTRL_GETOPT: {
      capdb *db = va_arg(ap, capdb*);
      int *pN = va_arg(ap, int*);
      *pN = db->dbOptFlags;
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_LOCALTIME_FAULT, onoff, xAlt);
    **
    ** If parameter onoff is 1, subsequent calls to localtime() fail.
    ** If 2, then invoke xAlt() instead of localtime().  If 0, normal
    ** processing.
    **
    ** xAlt arguments are void pointers, but they really want to be:
    **
    **    int xAlt(const time_t*, struct tm*);
    **
    ** xAlt should write results in to struct tm object of its 2nd argument
    ** and return zero on success, or return non-zero on failure.
    */
    case CAPDB_TESTCTRL_LOCALTIME_FAULT: {
      capdbGlobalConfig.bLocaltimeFault = va_arg(ap, int);
      if( capdbGlobalConfig.bLocaltimeFault==2 ){
        typedef int(*capdbLocaltimeType)(const void*,void*);
        capdbGlobalConfig.xAltLocaltime = va_arg(ap, capdbLocaltimeType);
      }else{
        capdbGlobalConfig.xAltLocaltime = 0;
      }
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_INTERNAL_FUNCTIONS, capdb*);
    **
    ** Toggle the ability to use internal functions on or off for
    ** the database connection given in the argument.
    */
    case CAPDB_TESTCTRL_INTERNAL_FUNCTIONS: {
      capdb *db = va_arg(ap, capdb*);
      db->mDbFlags ^= DBFLAG_InternalFunc;
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_NEVER_CORRUPT, int);
    **
    ** Set or clear a flag that indicates that the database file is always well-
    ** formed and never corrupt.  This flag is clear by default, indicating that
    ** database files might have arbitrary corruption.  Setting the flag during
    ** testing causes certain assert() statements in the code to be activated
    ** that demonstrate invariants on well-formed database files.
    */
    case CAPDB_TESTCTRL_NEVER_CORRUPT: {
      capdbGlobalConfig.neverCorrupt = va_arg(ap, int);
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_EXTRA_SCHEMA_CHECKS, int);
    **
    ** Set or clear a flag that causes SQLite to verify that type, name,
    ** and tbl_name fields of the sqlite_schema table.  This is normally
    ** on, but it is sometimes useful to turn it off for testing.
    **
    ** 2020-07-22:  Disabling EXTRA_SCHEMA_CHECKS also disables the
    ** verification of rootpage numbers when parsing the schema.  This
    ** is useful to make it easier to reach strange internal error states
    ** during testing.  The EXTRA_SCHEMA_CHECKS setting is always enabled
    ** in production.
    */
    case CAPDB_TESTCTRL_EXTRA_SCHEMA_CHECKS: {
      capdbGlobalConfig.bExtraSchemaChecks = va_arg(ap, int);
      break;
    }

    /* Set the threshold at which OP_Once counters reset back to zero.
    ** By default this is 0x7ffffffe (over 2 billion), but that value is
    ** too big to test in a reasonable amount of time, so this control is
    ** provided to set a small and easily reachable reset value.
    */
    case CAPDB_TESTCTRL_ONCE_RESET_THRESHOLD: {
      capdbGlobalConfig.iOnceResetThreshold = va_arg(ap, int);
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_VDBE_COVERAGE, xCallback, ptr);
    **
    ** Set the VDBE coverage callback function to xCallback with context
    ** pointer ptr.
    */
    case CAPDB_TESTCTRL_VDBE_COVERAGE: {
#ifdef CAPDB_VDBE_COVERAGE
      typedef void (*branch_callback)(void*,unsigned int,
                                      unsigned char,unsigned char);
      capdbGlobalConfig.xVdbeBranch = va_arg(ap,branch_callback);
      capdbGlobalConfig.pVdbeBranchArg = va_arg(ap,void*);
#endif
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_SORTER_MMAP, db, nMax); */
    case CAPDB_TESTCTRL_SORTER_MMAP: {
      capdb *db = va_arg(ap, capdb*);
      db->nMaxSorterMmap = va_arg(ap, int);
      break;
    }

    /*   capdb_test_control(CAPDB_TESTCTRL_ISINIT);
    **
    ** Return CAPDB_OK if SQLite has been initialized and CAPDB_ERROR if
    ** not.
    */
    case CAPDB_TESTCTRL_ISINIT: {
      if( capdbGlobalConfig.isInit==0 ) rc = CAPDB_ERROR;
      break;
    }

    /*  capdb_test_control(CAPDB_TESTCTRL_IMPOSTER, db, dbName, mode, tnum);
    **
    ** This test control is used to create imposter tables.  "db" is a pointer
    ** to the database connection.  dbName is the database name (ex: "main" or
    ** "temp") which will receive the imposter.  "mode" turns imposter mode on
    ** or off.  mode==0 means imposter mode is off.  mode==1 means imposter mode
    ** is on.  mode==2 means imposter mode is on but results in an imposter
    ** table that is read-only unless writable_schema is on.  "tnum" is the
    ** root page of the b-tree to which the imposter table should connect.
    **
    ** Enable imposter mode only when the schema has already been parsed.  Then
    ** run a single CREATE TABLE statement to construct the imposter table in
    ** the parsed schema.  Then turn imposter mode back off again.
    **
    ** If onOff==0 and tnum>0 then reset the schema for all databases, causing
    ** the schema to be reparsed the next time it is needed.  This has the
    ** effect of erasing all imposter tables.
    */
    case CAPDB_TESTCTRL_IMPOSTER: {
      capdb *db = va_arg(ap, capdb*);
      int iDb;
      capdb_mutex_enter(db->mutex);
      iDb = capdbFindDbName(db, va_arg(ap,const char*));
      if( iDb>=0 ){
        db->init.iDb = iDb;
        db->init.busy = db->init.imposterTable = va_arg(ap,int);
        db->init.newTnum = va_arg(ap,int);
        if( db->init.busy==0 && db->init.newTnum>0 ){
          capdbResetAllSchemasOfConnection(db);
        }
      }
      capdb_mutex_leave(db->mutex);
      break;
    }

#if defined(YYCOVERAGE)
    /*  capdb_test_control(CAPDB_TESTCTRL_PARSER_COVERAGE, FILE *out)
    **
    ** This test control (only available when SQLite is compiled with
    ** -DYYCOVERAGE) writes a report onto "out" that shows all
    ** state/lookahead combinations in the parser state machine
    ** which are never exercised.  If any state is missed, make the
    ** return code CAPDB_ERROR.
    */
    case CAPDB_TESTCTRL_PARSER_COVERAGE: {
      FILE *out = va_arg(ap, FILE*);
      if( capdbParserCoverage(out) ) rc = CAPDB_ERROR;
      break;
    }
#endif /* defined(YYCOVERAGE) */

    /*  capdb_test_control(CAPDB_TESTCTRL_RESULT_INTREAL, capdb_context*);
    **
    ** This test-control causes the most recent capdb_result_int64() value
    ** to be interpreted as a MEM_IntReal instead of as an MEM_Int.  Normally,
    ** MEM_IntReal values only arise during an INSERT operation of integer
    ** values into a REAL column, so they can be challenging to test.  This
    ** test-control enables us to write an intreal() SQL function that can
    ** inject an intreal() value at arbitrary places in an SQL statement,
    ** for testing purposes.
    */
    case CAPDB_TESTCTRL_RESULT_INTREAL: {
      capdb_context *pCtx = va_arg(ap, capdb_context*);
      capdbResultIntReal(pCtx);
      break;
    }

    /*  capdb_test_control(CAPDB_TESTCTRL_SEEK_COUNT,
    **    capdb *db,    // Database connection
    **    u64 *pnSeek     // Write seek count here
    **  );
    **
    ** This test-control queries the seek-counter on the "main" database
    ** file.  The seek-counter is written into *pnSeek and is then reset.
    ** The seek-count is only available if compiled with CAPDB_DEBUG.
    */
    case CAPDB_TESTCTRL_SEEK_COUNT: {
      capdb *db = va_arg(ap, capdb*);
      u64 *pn = va_arg(ap, capdb_uint64*);
      *pn = capdbBtreeSeekCount(db->aDb->pBt);
      (void)db;  /* Silence harmless unused variable warning */
      break;
    }

    /*  capdb_test_control(CAPDB_TESTCTRL_TRACEFLAGS, op, ptr)
    **
    **  "ptr" is a pointer to a u32. 
    **
    **   op==0       Store the current capdbTreeTrace in *ptr
    **   op==1       Set capdbTreeTrace to the value *ptr
    **   op==2       Store the current capdbWhereTrace in *ptr
    **   op==3       Set capdbWhereTrace to the value *ptr
    */
    case CAPDB_TESTCTRL_TRACEFLAGS: {
       int opTrace = va_arg(ap, int);
       u32 *ptr = va_arg(ap, u32*);
       switch( opTrace ){
         case 0:   *ptr = capdbTreeTrace;      break;
         case 1:   capdbTreeTrace = *ptr;      break;
         case 2:   *ptr = capdbWhereTrace;     break;
         case 3:   capdbWhereTrace = *ptr;     break;
       }
       break;
    }

    /* capdb_test_control(CAPDB_TESTCTRL_LOGEST,
    **      double fIn,     // Input value
    **      int *pLogEst,   // capdbLogEstFromDouble(fIn)
    **      u64 *pInt,      // capdbLogEstToInt(*pLogEst)
    **      int *pLogEst2   // capdbLogEst(*pInt)
    ** );
    **
    ** Test access for the LogEst conversion routines.
    */
    case CAPDB_TESTCTRL_LOGEST: {
      double rIn = va_arg(ap, double);
      LogEst rLogEst = capdbLogEstFromDouble(rIn);
      int *pI1 = va_arg(ap,int*);
      u64 *pU64 = va_arg(ap,u64*);
      int *pI2 = va_arg(ap,int*);
      *pI1 = rLogEst;
      *pU64 = capdbLogEstToInt(rLogEst);
      *pI2 = capdbLogEst(*pU64);
      break;
    }

    /* capdb_test_control(CAPDB_TESTCTRL_ATOF, const char *z, double *p);
    **
    ** Test access to the capdbAtoF() routine.
    */
    case CAPDB_TESTCTRL_ATOF: {
      const char *z = va_arg(ap,const char*);
      double *pR = va_arg(ap,double*);
      rc = capdbAtoF(z,pR);
      break;
    }

#if defined(CAPDB_DEBUG) && !defined(CAPDB_OMIT_WSD)
    /* capdb_test_control(CAPDB_TESTCTRL_TUNE, id, *piValue)
    **
    ** If "id" is an integer between 1 and CAPDB_NTUNE then set the value
    ** of the id-th tuning parameter to *piValue.  If "id" is between -1
    ** and -CAPDB_NTUNE, then write the current value of the (-id)-th
    ** tuning parameter into *piValue.
    **
    ** Tuning parameters are for use during transient development builds,
    ** to help find the best values for constants in the query planner.
    ** Access tuning parameters using the Tuning(ID) macro.  Set the
    ** parameters in the CLI using ".testctrl tune ID VALUE".
    **
    ** Transient use only.  Tuning parameters should not be used in
    ** checked-in code.
    */
    case CAPDB_TESTCTRL_TUNE: {
      int id = va_arg(ap, int);
      int *piValue = va_arg(ap, int*);
      if( id>0 && id<=CAPDB_NTUNE ){
        Tuning(id) = *piValue;
      }else if( id<0 && id>=-CAPDB_NTUNE ){
        *piValue = Tuning(-id);
      }else{
        rc = CAPDB_NOTFOUND;
      }
      break;
    }
#endif

    /* capdb_test_control(CAPDB_TESTCTRL_JSON_SELFCHECK, &onOff);
    **
    ** Activate or deactivate validation of JSONB that is generated from
    ** text.  Off by default, as the validation is slow.  Validation is
    ** only available if compiled using CAPDB_DEBUG.
    **
    ** If onOff is initially 1, then turn it on.  If onOff is initially
    ** off, turn it off.  If onOff is initially -1, then change onOff
    ** to be the current setting.
    */
    case CAPDB_TESTCTRL_JSON_SELFCHECK: {
#if defined(CAPDB_DEBUG) && !defined(CAPDB_OMIT_WSD)
      int *pOnOff = va_arg(ap, int*);
      if( *pOnOff<0 ){
        *pOnOff = capdbConfig.bJsonSelfcheck;
      }else{
        capdbConfig.bJsonSelfcheck = (u8)((*pOnOff)&0xff);
      }
#endif
      break;
    }
  }
  va_end(ap);
#endif /* CAPDB_UNTESTABLE */
  return rc;
}

/*
** The Pager stores the Database filename, Journal filename, and WAL filename
** consecutively in memory, in that order.  The database filename is prefixed
** by four zero bytes.  Locate the start of the database filename by searching
** backwards for the first byte following four consecutive zero bytes.
**
** This only works if the filename passed in was obtained from the Pager.
*/
static const char *databaseName(const char *zName){
  while( zName[-1]!=0 || zName[-2]!=0 || zName[-3]!=0 || zName[-4]!=0 ){
    zName--;
  }
  return zName;
}

/*
** Append text z[] to the end of p[].  Return a pointer to the first
** character after then zero terminator on the new text in p[].
*/
static char *appendText(char *p, const char *z){
  size_t n = strlen(z);
  memcpy(p, z, n+1);
  return p+n+1;
}

/*
** Allocate memory to hold names for a database, journal file, WAL file,
** and query parameters.  The pointer returned is valid for use by
** capdb_filename_database() and capdb_uri_parameter() and related
** functions.
**
** Memory layout must be compatible with that generated by the pager
** and expected by capdb_uri_parameter() and databaseName().
*/
const char *capdb_create_filename(
  const char *zDatabase,
  const char *zJournal,
  const char *zWal,
  int nParam,
  const char **azParam
){
  capdb_int64 nByte;
  int i;
  char *pResult, *p;
  nByte = strlen(zDatabase) + strlen(zJournal) + strlen(zWal) + 10;
  for(i=0; i<nParam*2; i++){
    nByte += strlen(azParam[i])+1;
  }
  pResult = p = capdb_malloc64( nByte );
  if( p==0 ) return 0;
  memset(p, 0, 4);
  p += 4;
  p = appendText(p, zDatabase);
  for(i=0; i<nParam*2; i++){
    p = appendText(p, azParam[i]);
  }
  *(p++) = 0;
  p = appendText(p, zJournal);
  p = appendText(p, zWal);
  *(p++) = 0;
  *(p++) = 0;
  assert( (capdb_int64)(p - pResult)==nByte );
  return pResult + 4;
}

/*
** Free memory obtained from capdb_create_filename().  It is a severe
** error to call this routine with any parameter other than a pointer
** previously obtained from capdb_create_filename() or a NULL pointer.
*/
void capdb_free_filename(const char *p){
  if( p==0 ) return;
  p = databaseName(p);
  capdb_free((char*)p - 4);
}


/*
** This is a utility routine, useful to VFS implementations, that checks
** to see if a database file was a URI that contained a specific query
** parameter, and if so obtains the value of the query parameter.
**
** The zFilename argument is the filename pointer passed into the xOpen()
** method of a VFS implementation.  The zParam argument is the name of the
** query parameter we seek.  This routine returns the value of the zParam
** parameter if it exists.  If the parameter does not exist, this routine
** returns a NULL pointer.
*/
const char *capdb_uri_parameter(const char *zFilename, const char *zParam){
  if( zFilename==0 || zParam==0 ) return 0;
  zFilename = databaseName(zFilename);
  return uriParameter(zFilename, zParam);
}

/*
** Return a pointer to the name of Nth query parameter of the filename.
*/
const char *capdb_uri_key(const char *zFilename, int N){
  if( zFilename==0 || N<0 ) return 0;
  zFilename = databaseName(zFilename);
  zFilename += capdbStrlen30(zFilename) + 1;
  while( ALWAYS(zFilename) && zFilename[0] && (N--)>0 ){
    zFilename += capdbStrlen30(zFilename) + 1;
    zFilename += capdbStrlen30(zFilename) + 1;
  }
  return zFilename[0] ? zFilename : 0;
}

/*
** Return a boolean value for a query parameter.
*/
int capdb_uri_boolean(const char *zFilename, const char *zParam, int bDflt){
  const char *z = capdb_uri_parameter(zFilename, zParam);
  bDflt = bDflt!=0;
  return z ? capdbGetBoolean(z, bDflt) : bDflt;
}

/*
** Return a 64-bit integer value for a query parameter.
*/
capdb_int64 capdb_uri_int64(
  const char *zFilename,    /* Filename as passed to xOpen */
  const char *zParam,       /* URI parameter sought */
  capdb_int64 bDflt       /* return if parameter is missing */
){
  const char *z = capdb_uri_parameter(zFilename, zParam);
  capdb_int64 v;
  if( z && capdbDecOrHexToI64(z, &v)==0 ){
    bDflt = v;
  }
  return bDflt;
}

/*
** Translate a filename that was handed to a VFS routine into the corresponding
** database, journal, or WAL file.
**
** It is an error to pass this routine a filename string that was not
** passed into the VFS from the SQLite core.  Doing so is similar to
** passing free() a pointer that was not obtained from malloc() - it is
** an error that we cannot easily detect but that will likely cause memory
** corruption.
*/
const char *capdb_filename_database(const char *zFilename){
  if( zFilename==0 ) return 0;
  return databaseName(zFilename);
}
const char *capdb_filename_journal(const char *zFilename){
  if( zFilename==0 ) return 0;
  zFilename = databaseName(zFilename);
  zFilename += capdbStrlen30(zFilename) + 1;
  while( ALWAYS(zFilename) && zFilename[0] ){
    zFilename += capdbStrlen30(zFilename) + 1;
    zFilename += capdbStrlen30(zFilename) + 1;
  }
  return zFilename + 1;
}
const char *capdb_filename_wal(const char *zFilename){
#ifdef CAPDB_OMIT_WAL
  UNUSED_PARAMETER(zFilename);
  return 0;
#else
  zFilename = capdb_filename_journal(zFilename);
  if( zFilename ) zFilename += capdbStrlen30(zFilename) + 1;
  return zFilename;
#endif
}

/*
** Return the Btree pointer identified by zDbName.  Return NULL if not found.
*/
Btree *capdbDbNameToBtree(capdb *db, const char *zDbName){
  int iDb = zDbName ? capdbFindDbName(db, zDbName) : 0;
  return iDb<0 ? 0 : db->aDb[iDb].pBt;
}

/*
** Return the name of the N-th database schema.  Return NULL if N is out
** of range.
*/
const char *capdb_db_name(capdb *db, int N){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  if( N<0 || N>=db->nDb ){
    return 0;
  }else{
    return db->aDb[N].zDbSName;
  }
}

/*
** Return the filename of the database associated with a database
** connection.
*/
const char *capdb_db_filename(capdb *db, const char *zDbName){
  Btree *pBt;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  pBt = capdbDbNameToBtree(db, zDbName);
  return pBt ? capdbBtreeGetFilename(pBt) : 0;
}

/*
** Return 1 if database is read-only or 0 if read/write.  Return -1 if
** no such database exists.
*/
int capdb_db_readonly(capdb *db, const char *zDbName){
  Btree *pBt;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    (void)CAPDB_MISUSE_BKPT;
    return -1;
  }
#endif
  pBt = capdbDbNameToBtree(db, zDbName);
  return pBt ? capdbBtreeIsReadonly(pBt) : -1;
}

#ifdef CAPDB_ENABLE_SNAPSHOT
/*
** Obtain a snapshot handle for the snapshot of database zDb currently
** being read by handle db.
*/
int capdb_snapshot_get(
  capdb *db,
  const char *zDb,
  capdb_snapshot **ppSnapshot
){
  int rc = CAPDB_ERROR;
#ifndef CAPDB_OMIT_WAL

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  capdb_mutex_enter(db->mutex);

  if( db->autoCommit==0 ){
    int iDb = capdbFindDbName(db, zDb);
    if( iDb==0 || iDb>1 ){
      Btree *pBt = db->aDb[iDb].pBt;
      if( CAPDB_TXN_WRITE!=capdbBtreeTxnState(pBt) ){
        Pager *pPager = capdbBtreePager(pBt);
        i64 dummy = 0;
        capdbPagerSnapshotOpen(pPager, (capdb_snapshot*)&dummy);
        rc = capdbBtreeBeginTrans(pBt, 0, 0);
        capdbPagerSnapshotOpen(pPager, 0);
        if( rc==CAPDB_OK ){
          rc = capdbPagerSnapshotGet(capdbBtreePager(pBt), ppSnapshot);
        }
      }
    }
  }

  capdb_mutex_leave(db->mutex);
#endif   /* CAPDB_OMIT_WAL */
  return rc;
}

/*
** Open a read-transaction on the snapshot identified by pSnapshot.
*/
int capdb_snapshot_open(
  capdb *db,
  const char *zDb,
  capdb_snapshot *pSnapshot
){
  int rc = CAPDB_ERROR;
#ifndef CAPDB_OMIT_WAL

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  capdb_mutex_enter(db->mutex);
  if( db->autoCommit==0 ){
    int iDb;
    iDb = capdbFindDbName(db, zDb);
    if( iDb==0 || iDb>1 ){
      Btree *pBt = db->aDb[iDb].pBt;
      if( capdbBtreeTxnState(pBt)!=CAPDB_TXN_WRITE ){
        Pager *pPager = capdbBtreePager(pBt);
        int bUnlock = 0;
        if( capdbBtreeTxnState(pBt)!=CAPDB_TXN_NONE ){
          if( db->nVdbeActive==0 ){
            rc = capdbPagerSnapshotCheck(pPager, pSnapshot);
            if( rc==CAPDB_OK ){
              bUnlock = 1;
              rc = capdbBtreeCommit(pBt);
            }
          }
        }else{
          rc = CAPDB_OK;
        }
        if( rc==CAPDB_OK ){
          rc = capdbPagerSnapshotOpen(pPager, pSnapshot);
        }
        if( rc==CAPDB_OK ){
          rc = capdbBtreeBeginTrans(pBt, 0, 0);
          capdbPagerSnapshotOpen(pPager, 0);
        }
        if( bUnlock ){
          capdbPagerSnapshotUnlock(pPager);
        }
      }
    }
  }

  capdb_mutex_leave(db->mutex);
#endif   /* CAPDB_OMIT_WAL */
  return rc;
}

/*
** Recover as many snapshots as possible from the wal file associated with
** schema zDb of database db.
*/
int capdb_snapshot_recover(capdb *db, const char *zDb){
  int rc = CAPDB_ERROR;
#ifndef CAPDB_OMIT_WAL
  int iDb;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ){
    return CAPDB_MISUSE_BKPT;
  }
#endif

  capdb_mutex_enter(db->mutex);
  iDb = capdbFindDbName(db, zDb);
  if( iDb==0 || iDb>1 ){
    Btree *pBt = db->aDb[iDb].pBt;
    if( CAPDB_TXN_NONE==capdbBtreeTxnState(pBt) ){
      rc = capdbBtreeBeginTrans(pBt, 0, 0);
      if( rc==CAPDB_OK ){
        rc = capdbPagerSnapshotRecover(capdbBtreePager(pBt));
        capdbBtreeCommit(pBt);
      }
    }
  }
  capdb_mutex_leave(db->mutex);
#endif   /* CAPDB_OMIT_WAL */
  return rc;
}

/*
** Free a snapshot handle obtained from capdb_snapshot_get().
*/
void capdb_snapshot_free(capdb_snapshot *pSnapshot){
  capdb_free(pSnapshot);
}
#endif /* CAPDB_ENABLE_SNAPSHOT */

#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
/*
** Given the name of a compile-time option, return true if that option
** was used and false if not.
**
** The name can optionally begin with "CAPDB_" but the "CAPDB_" prefix
** is not required for a match.
*/
int capdb_compileoption_used(const char *zOptName){
  int i, n;
  int nOpt;
  const char **azCompileOpt;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( zOptName==0 ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif

  azCompileOpt = capdbCompileOptions(&nOpt);

  if( capdbStrNICmp(zOptName, "CAPDB_", 7)==0 ) zOptName += 7;
  n = capdbStrlen30(zOptName);

  /* Since nOpt is normally in single digits, a linear search is
  ** adequate. No need for a binary search. */
  for(i=0; i<nOpt; i++){
    if( capdbStrNICmp(zOptName, azCompileOpt[i], n)==0
     && capdbIsIdChar((unsigned char)azCompileOpt[i][n])==0
    ){
      return 1;
    }
  }
  return 0;
}

/*
** Return the N-th compile-time option string.  If N is out of range,
** return a NULL pointer.
*/
const char *capdb_compileoption_get(int N){
  int nOpt;
  const char **azCompileOpt;
  azCompileOpt = capdbCompileOptions(&nOpt);
  if( N>=0 && N<nOpt ){
    return azCompileOpt[N];
  }
  return 0;
}
#endif /* CAPDB_OMIT_COMPILEOPTION_DIAGS */
