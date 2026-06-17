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
**
** Memory allocation functions used throughout sqlite.
*/
#include "capdbInt.h"
#include <stdarg.h>

/*
** Attempt to release up to n bytes of non-essential memory currently
** held by SQLite. An example of non-essential memory is memory used to
** cache database pages that are not currently in use.
*/
int capdb_release_memory(int n){
#ifdef CAPDB_ENABLE_MEMORY_MANAGEMENT
  return capdbPcacheReleaseMemory(n);
#else
  /* IMPLEMENTATION-OF: R-34391-24921 The capdb_release_memory() routine
  ** is a no-op returning zero if SQLite is not compiled with
  ** CAPDB_ENABLE_MEMORY_MANAGEMENT. */
  UNUSED_PARAMETER(n);
  return 0;
#endif
}

/*
** Default value of the hard heap limit.  0 means "no limit".
*/
#ifndef CAPDB_MAX_MEMORY
# define CAPDB_MAX_MEMORY 0
#endif

/*
** State information local to the memory allocation subsystem.
*/
static CAPDB_WSD struct Mem0Global {
  capdb_mutex *mutex;         /* Mutex to serialize access */
  capdb_int64 alarmThreshold; /* The soft heap limit */
  capdb_int64 hardLimit;      /* The hard upper bound on memory */

  /*
  ** True if heap is nearly "full" where "full" is defined by the
  ** capdb_soft_heap_limit() setting.
  */
  int nearlyFull;
} mem0 = { 0, CAPDB_MAX_MEMORY, CAPDB_MAX_MEMORY, 0 };

#define mem0 GLOBAL(struct Mem0Global, mem0)

/*
** Return the memory allocator mutex. capdb_status() needs it.
*/
capdb_mutex *capdbMallocMutex(void){
  return mem0.mutex;
}

#ifndef CAPDB_OMIT_DEPRECATED
/*
** Deprecated external interface.  It used to set an alarm callback
** that was invoked when memory usage grew too large.  Now it is a
** no-op.
*/
int capdb_memory_alarm(
  void(*xCallback)(void *pArg, capdb_int64 used,int N),
  void *pArg,
  capdb_int64 iThreshold
){
  (void)xCallback;
  (void)pArg;
  (void)iThreshold;
  return CAPDB_OK;
}
#endif

/*
** Set the soft heap-size limit for the library.  An argument of
** zero disables the limit.  A negative argument is a no-op used to
** obtain the return value.
**
** The return value is the value of the heap limit just before this
** interface was called.
**
** If the hard heap limit is enabled, then the soft heap limit cannot
** be disabled nor raised above the hard heap limit.
*/
capdb_int64 capdb_soft_heap_limit64(capdb_int64 n){
  capdb_int64 priorLimit;
  capdb_int64 excess;
  capdb_int64 nUsed;
#ifndef CAPDB_OMIT_AUTOINIT
  int rc = capdb_initialize();
  if( rc ) return -1;
#endif
  capdb_mutex_enter(mem0.mutex);
  priorLimit = mem0.alarmThreshold;
  if( n<0 ){
    capdb_mutex_leave(mem0.mutex);
    return priorLimit;
  }
  if( mem0.hardLimit>0 && (n>mem0.hardLimit || n==0) ){
    n = mem0.hardLimit;
  }
  mem0.alarmThreshold = n;
  nUsed = capdbStatusValue(CAPDB_STATUS_MEMORY_USED);
  AtomicStore(&mem0.nearlyFull, n>0 && n<=nUsed);
  capdb_mutex_leave(mem0.mutex);
  excess = capdb_memory_used() - n;
  if( excess>0 ) capdb_release_memory((int)(excess & 0x7fffffff));
  return priorLimit;
}
void capdb_soft_heap_limit(int n){
  if( n<0 ) n = 0;
  capdb_soft_heap_limit64(n);
}

/*
** Set the hard heap-size limit for the library. An argument of zero
** disables the hard heap limit.  A negative argument is a no-op used
** to obtain the return value without affecting the hard heap limit.
**
** The return value is the value of the hard heap limit just prior to
** calling this interface.
**
** Setting the hard heap limit will also activate the soft heap limit
** and constrain the soft heap limit to be no more than the hard heap
** limit.
*/
capdb_int64 capdb_hard_heap_limit64(capdb_int64 n){
  capdb_int64 priorLimit;
#ifndef CAPDB_OMIT_AUTOINIT
  int rc = capdb_initialize();
  if( rc ) return -1;
#endif
  capdb_mutex_enter(mem0.mutex);
  priorLimit = mem0.hardLimit;
  if( n>=0 ){
    mem0.hardLimit = n;
    if( n<mem0.alarmThreshold || mem0.alarmThreshold==0 ){
      mem0.alarmThreshold = n;
    }
  }
  capdb_mutex_leave(mem0.mutex);
  return priorLimit;
}


/*
** Initialize the memory allocation subsystem.
*/
int capdbMallocInit(void){
  int rc;
  if( capdbGlobalConfig.m.xMalloc==0 ){
    capdbMemSetDefault();
  }
  mem0.mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MEM);
  if( capdbGlobalConfig.pPage==0 || capdbGlobalConfig.szPage<512
      || capdbGlobalConfig.nPage<=0 ){
    capdbGlobalConfig.pPage = 0;
    capdbGlobalConfig.szPage = 0;
  }
  rc = capdbGlobalConfig.m.xInit(capdbGlobalConfig.m.pAppData);
  if( rc!=CAPDB_OK ) memset(&mem0, 0, sizeof(mem0));
  return rc;
}

/*
** Return true if the heap is currently under memory pressure - in other
** words if the amount of heap used is close to the limit set by
** capdb_soft_heap_limit().
*/
int capdbHeapNearlyFull(void){
  return AtomicLoad(&mem0.nearlyFull);
}

/*
** Deinitialize the memory allocation subsystem.
*/
void capdbMallocEnd(void){
  if( capdbGlobalConfig.m.xShutdown ){
    capdbGlobalConfig.m.xShutdown(capdbGlobalConfig.m.pAppData);
  }
  memset(&mem0, 0, sizeof(mem0));
}

/*
** Return the amount of memory currently checked out.
*/
capdb_int64 capdb_memory_used(void){
  capdb_int64 res, mx;
  capdb_status64(CAPDB_STATUS_MEMORY_USED, &res, &mx, 0);
  return res;
}

/*
** Return the maximum amount of memory that has ever been
** checked out since either the beginning of this process
** or since the most recent reset.
*/
capdb_int64 capdb_memory_highwater(int resetFlag){
  capdb_int64 res, mx;
  capdb_status64(CAPDB_STATUS_MEMORY_USED, &res, &mx, resetFlag);
  return mx;
}

/*
** Trigger the alarm 
*/
static void capdbMallocAlarm(int nByte){
  if( mem0.alarmThreshold<=0 ) return;
  capdb_mutex_leave(mem0.mutex);
  capdb_release_memory(nByte);
  capdb_mutex_enter(mem0.mutex);
}

#ifdef CAPDB_DEBUG
/*
** This routine is called whenever an out-of-memory condition is seen,
** It's only purpose to to serve as a breakpoint for gdb or similar
** code debuggers when working on out-of-memory conditions, for example
** caused by PRAGMA hard_heap_limit=N.
*/
static CAPDB_NOINLINE void test_oom_breakpoint(u64 n){
  static u64 nOomFault = 0;
  nOomFault += n;
  /* The assert() is never reached in a human lifetime.  It  is here mostly
  ** to prevent code optimizers from optimizing out this function. */
  assert( (nOomFault>>32) < 0xffffffff );
}
#else
# define test_oom_breakpoint(X)   /* No-op for production builds */
#endif

/*
** Do a memory allocation with statistics and alarms.  Assume the
** lock is already held.
*/
static void mallocWithAlarm(int n, void **pp){
  void *p;
  int nFull;
  assert( capdb_mutex_held(mem0.mutex) );
  assert( n>0 );

  /* In Firefox (circa 2017-02-08), xRoundup() is remapped to an internal
  ** implementation of malloc_good_size(), which must be called in debug
  ** mode and specifically when the DMD "Dark Matter Detector" is enabled
  ** or else a crash results.  Hence, do not attempt to optimize out the
  ** following xRoundup() call. */
  nFull = capdbGlobalConfig.m.xRoundup(n);

  capdbStatusHighwater(CAPDB_STATUS_MALLOC_SIZE, n);
  if( mem0.alarmThreshold>0 ){
    capdb_int64 nUsed = capdbStatusValue(CAPDB_STATUS_MEMORY_USED);
    if( nUsed >= mem0.alarmThreshold - nFull ){
      AtomicStore(&mem0.nearlyFull, 1);
      capdbMallocAlarm(nFull);
      if( mem0.hardLimit ){
        nUsed = capdbStatusValue(CAPDB_STATUS_MEMORY_USED);
        if( nUsed >= mem0.hardLimit - nFull ){
          test_oom_breakpoint(1);
          *pp = 0;
          return;
        }
      }
    }else{
      AtomicStore(&mem0.nearlyFull, 0);
    }
  }
  p = capdbGlobalConfig.m.xMalloc(nFull);
#ifdef CAPDB_ENABLE_MEMORY_MANAGEMENT
  if( p==0 && mem0.alarmThreshold>0 ){
    capdbMallocAlarm(nFull);
    p = capdbGlobalConfig.m.xMalloc(nFull);
  }
#endif
  if( p ){
    nFull = capdbMallocSize(p);
    capdbStatusUp(CAPDB_STATUS_MEMORY_USED, nFull);
    capdbStatusUp(CAPDB_STATUS_MALLOC_COUNT, 1);
  }
  *pp = p;
}

/*
** Allocate memory.  This routine is like capdb_malloc() except that it
** assumes the memory subsystem has already been initialized.
*/
void *capdbMalloc(u64 n){
  void *p;
  if( n==0 || n>CAPDB_MAX_ALLOCATION_SIZE ){
    p = 0;
  }else if( capdbGlobalConfig.bMemstat ){
    capdb_mutex_enter(mem0.mutex);
    mallocWithAlarm((int)n, &p);
    capdb_mutex_leave(mem0.mutex);
  }else{
    p = capdbGlobalConfig.m.xMalloc((int)n);
  }
  assert( EIGHT_BYTE_ALIGNMENT(p) );  /* IMP: R-11148-40995 */
  return p;
}

/*
** This version of the memory allocation is for use by the application.
** First make sure the memory subsystem is initialized, then do the
** allocation.
*/
void *capdb_malloc(int n){
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return n<=0 ? 0 : capdbMalloc(n);
}
void *capdb_malloc64(capdb_uint64 n){
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return capdbMalloc(n);
}

/*
** TRUE if p is a lookaside memory allocation from db
*/
#ifndef CAPDB_OMIT_LOOKASIDE
static int isLookaside(capdb *db, const void *p){
  return CAPDB_WITHIN(p, db->lookaside.pStart, db->lookaside.pTrueEnd);
}
#else
#define isLookaside(A,B) 0
#endif

/*
** Return the size of a memory allocation previously obtained from
** capdbMalloc() or capdb_malloc().
*/
int capdbMallocSize(const void *p){
  assert( capdbMemdebugHasType(p, MEMTYPE_HEAP) );
  return capdbGlobalConfig.m.xSize((void*)p);
}
static int lookasideMallocSize(capdb *db, const void *p){
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE    
  return p<db->lookaside.pMiddle ? db->lookaside.szTrue : LOOKASIDE_SMALL;
#else
  return db->lookaside.szTrue;
#endif  
}
int capdbDbMallocSize(capdb *db, const void *p){
  assert( p!=0 );
#ifdef CAPDB_DEBUG
  if( db==0 ){
    assert( capdbMemdebugNoType(p, (u8)~MEMTYPE_HEAP) );
    assert( capdbMemdebugHasType(p, MEMTYPE_HEAP) );
  }else if( !isLookaside(db,p) ){
    assert( capdbMemdebugHasType(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
    assert( capdbMemdebugNoType(p, (u8)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  }
#endif
  if( db ){
    if( ((uptr)p)<(uptr)(db->lookaside.pTrueEnd) ){
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
      if( ((uptr)p)>=(uptr)(db->lookaside.pMiddle) ){
        assert( capdb_mutex_held(db->mutex) );
        return LOOKASIDE_SMALL;
      }
#endif
      if( ((uptr)p)>=(uptr)(db->lookaside.pStart) ){
        assert( capdb_mutex_held(db->mutex) );
        return db->lookaside.szTrue;
      }
    }
  }
  return capdbGlobalConfig.m.xSize((void*)p);
}
capdb_uint64 capdb_msize(void *p){
  assert( capdbMemdebugNoType(p, (u8)~MEMTYPE_HEAP) );
  assert( capdbMemdebugHasType(p, MEMTYPE_HEAP) );
  return p ? capdbGlobalConfig.m.xSize(p) : 0;
}

/*
** Free memory previously obtained from capdbMalloc().
*/
void capdb_free(void *p){
  if( p==0 ) return;  /* IMP: R-49053-54554 */
  assert( capdbMemdebugHasType(p, MEMTYPE_HEAP) );
  assert( capdbMemdebugNoType(p, (u8)~MEMTYPE_HEAP) );
  if( capdbGlobalConfig.bMemstat ){
    capdb_mutex_enter(mem0.mutex);
    capdbStatusDown(CAPDB_STATUS_MEMORY_USED, capdbMallocSize(p));
    capdbStatusDown(CAPDB_STATUS_MALLOC_COUNT, 1);
    capdbGlobalConfig.m.xFree(p);
    capdb_mutex_leave(mem0.mutex);
  }else{
    capdbGlobalConfig.m.xFree(p);
  }
}

/*
** Add the size of memory allocation "p" to the count in
** *db->pnBytesFreed.
*/
static CAPDB_NOINLINE void measureAllocationSize(capdb *db, void *p){
  *db->pnBytesFreed += capdbDbMallocSize(db,p);
}

/*
** Free memory that might be associated with a particular database
** connection.  Calling capdbDbFree(D,X) for X==0 is a harmless no-op.
** The capdbDbFreeNN(D,X) version requires that X be non-NULL.
*/
void capdbDbFreeNN(capdb *db, void *p){
  assert( db==0 || capdb_mutex_held(db->mutex) );
  assert( p!=0 );
  if( db ){
    if( ((uptr)p)<(uptr)(db->lookaside.pEnd) ){
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
      if( ((uptr)p)>=(uptr)(db->lookaside.pMiddle) ){
        LookasideSlot *pBuf = (LookasideSlot*)p;
        assert( db->pnBytesFreed==0 );
#ifdef CAPDB_DEBUG
        memset(p, 0xaa, LOOKASIDE_SMALL);  /* Trash freed content */
#endif
        pBuf->pNext = db->lookaside.pSmallFree;
        db->lookaside.pSmallFree = pBuf;
        return;
      }
#endif /* CAPDB_OMIT_TWOSIZE_LOOKASIDE */
      if( ((uptr)p)>=(uptr)(db->lookaside.pStart) ){
        LookasideSlot *pBuf = (LookasideSlot*)p;
        assert( db->pnBytesFreed==0 );
#ifdef CAPDB_DEBUG
        memset(p, 0xaa, db->lookaside.szTrue);  /* Trash freed content */
#endif
        pBuf->pNext = db->lookaside.pFree;
        db->lookaside.pFree = pBuf;
        return;
      }
    }
    if( db->pnBytesFreed ){
      measureAllocationSize(db, p);
      return;
    }
  }
  assert( capdbMemdebugHasType(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  assert( capdbMemdebugNoType(p, (u8)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  assert( db!=0 || capdbMemdebugNoType(p, MEMTYPE_LOOKASIDE) );
  capdbMemdebugSetType(p, MEMTYPE_HEAP);
  capdb_free(p);
}
void capdbDbNNFreeNN(capdb *db, void *p){
  assert( db!=0 );
  assert( capdb_mutex_held(db->mutex) );
  assert( p!=0 );
  if( ((uptr)p)<(uptr)(db->lookaside.pEnd) ){
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
    if( ((uptr)p)>=(uptr)(db->lookaside.pMiddle) ){
      LookasideSlot *pBuf = (LookasideSlot*)p;
      assert( db->pnBytesFreed==0 );
#ifdef CAPDB_DEBUG
      memset(p, 0xaa, LOOKASIDE_SMALL);  /* Trash freed content */
#endif
      pBuf->pNext = db->lookaside.pSmallFree;
      db->lookaside.pSmallFree = pBuf;
      return;
    }
#endif /* CAPDB_OMIT_TWOSIZE_LOOKASIDE */
    if( ((uptr)p)>=(uptr)(db->lookaside.pStart) ){
      LookasideSlot *pBuf = (LookasideSlot*)p;
      assert( db->pnBytesFreed==0 );
#ifdef CAPDB_DEBUG
      memset(p, 0xaa, db->lookaside.szTrue);  /* Trash freed content */
#endif
      pBuf->pNext = db->lookaside.pFree;
      db->lookaside.pFree = pBuf;
      return;
    }
  }
  if( db->pnBytesFreed ){
    measureAllocationSize(db, p);
    return;
  }
  assert( capdbMemdebugHasType(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  assert( capdbMemdebugNoType(p, (u8)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  capdbMemdebugSetType(p, MEMTYPE_HEAP);
  capdb_free(p);
}
void capdbDbFree(capdb *db, void *p){
  assert( db==0 || capdb_mutex_held(db->mutex) );
  if( p ) capdbDbFreeNN(db, p);
}

/*
** Change the size of an existing memory allocation
*/
void *capdbRealloc(void *pOld, u64 nBytes){
  int nOld, nNew, nDiff;
  void *pNew;
  assert( capdbMemdebugHasType(pOld, MEMTYPE_HEAP) );
  assert( capdbMemdebugNoType(pOld, (u8)~MEMTYPE_HEAP) );
  if( pOld==0 ){
    return capdbMalloc(nBytes); /* IMP: R-04300-56712 */
  }
  if( nBytes==0 ){
    capdb_free(pOld); /* IMP: R-26507-47431 */
    return 0;
  }
  if( nBytes>CAPDB_MAX_ALLOCATION_SIZE ){
    return 0;
  }
  nOld = capdbMallocSize(pOld);
  /* IMPLEMENTATION-OF: R-46199-30249 SQLite guarantees that the second
  ** argument to xRealloc is always a value returned by a prior call to
  ** xRoundup. */
  nNew = capdbGlobalConfig.m.xRoundup((int)nBytes);
  if( nOld==nNew ){
    pNew = pOld;
  }else if( capdbGlobalConfig.bMemstat ){
    capdb_int64 nUsed;
    capdb_mutex_enter(mem0.mutex);
    capdbStatusHighwater(CAPDB_STATUS_MALLOC_SIZE, (int)nBytes);
    nDiff = nNew - nOld;
    if( nDiff>0 && (nUsed = capdbStatusValue(CAPDB_STATUS_MEMORY_USED)) >= 
          mem0.alarmThreshold-nDiff ){
      capdbMallocAlarm(nDiff);
      if( mem0.hardLimit>0 && nUsed >= mem0.hardLimit - nDiff ){
        capdb_mutex_leave(mem0.mutex);
        test_oom_breakpoint(1);
        return 0;
      }
    }
    pNew = capdbGlobalConfig.m.xRealloc(pOld, nNew);
#ifdef CAPDB_ENABLE_MEMORY_MANAGEMENT
    if( pNew==0 && mem0.alarmThreshold>0 ){
      capdbMallocAlarm((int)nBytes);
      pNew = capdbGlobalConfig.m.xRealloc(pOld, nNew);
    }
#endif
    if( pNew ){
      nNew = capdbMallocSize(pNew);
      capdbStatusUp(CAPDB_STATUS_MEMORY_USED, nNew-nOld);
    }
    capdb_mutex_leave(mem0.mutex);
  }else{
    pNew = capdbGlobalConfig.m.xRealloc(pOld, nNew);
  }
  assert( EIGHT_BYTE_ALIGNMENT(pNew) ); /* IMP: R-11148-40995 */
  return pNew;
}

/*
** The public interface to capdbRealloc.  Make sure that the memory
** subsystem is initialized prior to invoking sqliteRealloc.
*/
void *capdb_realloc(void *pOld, int n){
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  if( n<0 ) n = 0;  /* IMP: R-26507-47431 */
  return capdbRealloc(pOld, n);
}
void *capdb_realloc64(void *pOld, capdb_uint64 n){
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return capdbRealloc(pOld, n);
}


/*
** Allocate and zero memory.
*/ 
void *capdbMallocZero(u64 n){
  void *p = capdbMalloc(n);
  if( p ){
    memset(p, 0, (size_t)n);
  }
  return p;
}

/*
** Allocate and zero memory.  If the allocation fails, make
** the mallocFailed flag in the connection pointer.
*/
void *capdbDbMallocZero(capdb *db, u64 n){
  void *p;
  testcase( db==0 );
  p = capdbDbMallocRaw(db, n);
  if( p ) memset(p, 0, (size_t)n);
  return p;
}


/* Finish the work of capdbDbMallocRawNN for the unusual and
** slower case when the allocation cannot be fulfilled using lookaside.
*/
static CAPDB_NOINLINE void *dbMallocRawFinish(capdb *db, u64 n){
  void *p;
  assert( db!=0 );
  p = capdbMalloc(n);
  if( !p ) capdbOomFault(db);
  capdbMemdebugSetType(p, 
         (db->lookaside.bDisable==0) ? MEMTYPE_LOOKASIDE : MEMTYPE_HEAP);
  return p;
}

/*
** Allocate memory, either lookaside (if possible) or heap.  
** If the allocation fails, set the mallocFailed flag in
** the connection pointer.
**
** If db!=0 and db->mallocFailed is true (indicating a prior malloc
** failure on the same database connection) then always return 0.
** Hence for a particular database connection, once malloc starts
** failing, it fails consistently until mallocFailed is reset.
** This is an important assumption.  There are many places in the
** code that do things like this:
**
**         int *a = (int*)capdbDbMallocRaw(db, 100);
**         int *b = (int*)capdbDbMallocRaw(db, 200);
**         if( b ) a[10] = 9;
**
** In other words, if a subsequent malloc (ex: "b") worked, it is assumed
** that all prior mallocs (ex: "a") worked too.
**
** The capdbMallocRawNN() variant guarantees that the "db" parameter is
** not a NULL pointer.
*/
void *capdbDbMallocRaw(capdb *db, u64 n){
  void *p;
  if( db ) return capdbDbMallocRawNN(db, n);
  p = capdbMalloc(n);
  capdbMemdebugSetType(p, MEMTYPE_HEAP);
  return p;
}
void *capdbDbMallocRawNN(capdb *db, u64 n){
#ifndef CAPDB_OMIT_LOOKASIDE
  LookasideSlot *pBuf;
  assert( db!=0 );
  assert( capdb_mutex_held(db->mutex) );
  assert( db->pnBytesFreed==0 );
  if( n>db->lookaside.sz ){
    if( !db->lookaside.bDisable ){
      db->lookaside.anStat[1]++;      
    }else if( db->mallocFailed ){
      return 0;
    }
    return dbMallocRawFinish(db, n);
  }
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
  if( n<=LOOKASIDE_SMALL ){
    if( (pBuf = db->lookaside.pSmallFree)!=0 ){
      db->lookaside.pSmallFree = pBuf->pNext;
      db->lookaside.anStat[0]++;
      return (void*)pBuf;
    }else if( (pBuf = db->lookaside.pSmallInit)!=0 ){
      db->lookaside.pSmallInit = pBuf->pNext;
      db->lookaside.anStat[0]++;
      return (void*)pBuf;
    }
  }
#endif
  if( (pBuf = db->lookaside.pFree)!=0 ){
    db->lookaside.pFree = pBuf->pNext;
    db->lookaside.anStat[0]++;
    return (void*)pBuf;
  }else if( (pBuf = db->lookaside.pInit)!=0 ){
    db->lookaside.pInit = pBuf->pNext;
    db->lookaside.anStat[0]++;
    return (void*)pBuf;
  }else{
    db->lookaside.anStat[2]++;
  }
#else
  assert( db!=0 );
  assert( capdb_mutex_held(db->mutex) );
  assert( db->pnBytesFreed==0 );
  if( db->mallocFailed ){
    return 0;
  }
#endif
  return dbMallocRawFinish(db, n);
}

/* Forward declaration */
static CAPDB_NOINLINE void *dbReallocFinish(capdb *db, void *p, u64 n);

/*
** Resize the block of memory pointed to by p to n bytes. If the
** resize fails, set the mallocFailed flag in the connection object.
*/
void *capdbDbRealloc(capdb *db, void *p, u64 n){
  assert( db!=0 );
  if( p==0 ) return capdbDbMallocRawNN(db, n);
  assert( capdb_mutex_held(db->mutex) );
  if( ((uptr)p)<(uptr)db->lookaside.pEnd ){
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
    if( ((uptr)p)>=(uptr)db->lookaside.pMiddle ){
      if( n<=LOOKASIDE_SMALL ) return p;
    }else
#endif
    if( ((uptr)p)>=(uptr)db->lookaside.pStart ){
      if( n<=db->lookaside.szTrue ) return p;
    }
  }
  return dbReallocFinish(db, p, n);
}
static CAPDB_NOINLINE void *dbReallocFinish(capdb *db, void *p, u64 n){
  void *pNew = 0;
  assert( db!=0 );
  assert( p!=0 );
  if( db->mallocFailed==0 ){
    if( isLookaside(db, p) ){
      pNew = capdbDbMallocRawNN(db, n);
      if( pNew ){
        memcpy(pNew, p, lookasideMallocSize(db, p));
        capdbDbFree(db, p);
      }
    }else{
      assert( capdbMemdebugHasType(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
      assert( capdbMemdebugNoType(p, (u8)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
      capdbMemdebugSetType(p, MEMTYPE_HEAP);
      pNew = capdbRealloc(p, n);
      if( !pNew ){
        capdbOomFault(db);
      }
      capdbMemdebugSetType(pNew,
            (db->lookaside.bDisable==0 ? MEMTYPE_LOOKASIDE : MEMTYPE_HEAP));
    }
  }
  return pNew;
}

/*
** Attempt to reallocate p.  If the reallocation fails, then free p
** and set the mallocFailed flag in the database connection.
*/
void *capdbDbReallocOrFree(capdb *db, void *p, u64 n){
  void *pNew;
  pNew = capdbDbRealloc(db, p, n);
  if( !pNew ){
    capdbDbFree(db, p);
  }
  return pNew;
}

/*
** Make a copy of a string in memory obtained from sqliteMalloc(). These 
** functions call capdbMallocRaw() directly instead of sqliteMalloc(). This
** is because when memory debugging is turned on, these two functions are 
** called via macros that record the current file and line number in the
** ThreadData structure.
*/
char *capdbDbStrDup(capdb *db, const char *z){
  char *zNew;
  size_t n;
  if( z==0 ){
    return 0;
  }
  n = strlen(z) + 1;
  zNew = capdbDbMallocRaw(db, n);
  if( zNew ){
    memcpy(zNew, z, n);
  }
  return zNew;
}
char *capdbDbStrNDup(capdb *db, const char *z, u64 n){
  char *zNew;
  assert( db!=0 );
  assert( z!=0 || n==0 );
  assert( (n&0x7fffffff)==n );
  zNew = z ? capdbDbMallocRawNN(db, n+1) : 0;
  if( zNew ){
    memcpy(zNew, z, (size_t)n);
    zNew[n] = 0;
  }
  return zNew;
}

/*
** The text between zStart and zEnd represents a phrase within a larger
** SQL statement.  Make a copy of this phrase in space obtained form
** capdbDbMalloc().  Omit leading and trailing whitespace.
*/
char *capdbDbSpanDup(capdb *db, const char *zStart, const char *zEnd){
  int n;
#ifdef CAPDB_DEBUG
  /* Because of the way the parser works, the span is guaranteed to contain
  ** at least one non-space character */
  for(n=0; capdbIsspace(zStart[n]); n++){ assert( &zStart[n]<zEnd ); }
#endif
  while( capdbIsspace(zStart[0]) ) zStart++;
  n = (int)(zEnd - zStart);
  while( capdbIsspace(zStart[n-1]) ) n--;
  return capdbDbStrNDup(db, zStart, n);
}

/*
** Free any prior content in *pz and replace it with a copy of zNew.
*/
void capdbSetString(char **pz, capdb *db, const char *zNew){
  char *z = capdbDbStrDup(db, zNew);
  capdbDbFree(db, *pz);
  *pz = z;
}

/*
** Call this routine to record the fact that an OOM (out-of-memory) error
** has happened.  This routine will set db->mallocFailed, and also
** temporarily disable the lookaside memory allocator and interrupt
** any running VDBEs.
**
** Always return a NULL pointer so that this routine can be invoked using
**
**      return capdbOomFault(db);
**
** and thereby avoid unnecessary stack frame allocations for the overwhelmingly
** common case where no OOM occurs.
*/
void *capdbOomFault(capdb *db){
  if( db->mallocFailed==0 && db->bBenignMalloc==0 ){
    db->mallocFailed = 1;
    if( db->nVdbeExec>0 ){
      AtomicStore(&db->u1.isInterrupted, 1);
    }
    DisableLookaside;
    if( db->pParse ){
      Parse *pParse;
      capdbErrorMsg(db->pParse, "out of memory");
      db->pParse->rc = CAPDB_NOMEM_BKPT;
      for(pParse=db->pParse->pOuterParse; pParse; pParse = pParse->pOuterParse){
        pParse->nErr++;
        pParse->rc = CAPDB_NOMEM;
      } 
    }
  }
  return 0;
}

/*
** This routine reactivates the memory allocator and clears the
** db->mallocFailed flag as necessary.
**
** The memory allocator is not restarted if there are running
** VDBEs.
*/
void capdbOomClear(capdb *db){
  if( db->mallocFailed && db->nVdbeExec==0 ){
    db->mallocFailed = 0;
    AtomicStore(&db->u1.isInterrupted, 0);
    assert( db->lookaside.bDisable>0 );
    EnableLookaside;
  }
}

/*
** Take actions at the end of an API call to deal with error codes.
*/
static CAPDB_NOINLINE int apiHandleError(capdb *db, int rc){
  if( db->mallocFailed || rc==CAPDB_IOERR_NOMEM ){
    capdbOomClear(db);
    capdbError(db, CAPDB_NOMEM);
    return CAPDB_NOMEM_BKPT;
  }
  return rc & db->errMask;
}

/*
** This function must be called before exiting any API function (i.e. 
** returning control to the user) that has called capdb_malloc or
** capdb_realloc.
**
** The returned value is normally a copy of the second argument to this
** function. However, if a malloc() failure has occurred since the previous
** invocation CAPDB_NOMEM is returned instead. 
**
** If an OOM as occurred, then the connection error-code (the value
** returned by capdb_errcode()) is set to CAPDB_NOMEM.
*/
int capdbApiExit(capdb* db, int rc){
  /* If the db handle must hold the connection handle mutex here.
  ** Otherwise the read (and possible write) of db->mallocFailed 
  ** is unsafe, as is the call to capdbError().
  */
  assert( db!=0 );
  assert( capdb_mutex_held(db->mutex) );
  if( db->mallocFailed || rc ){
    return apiHandleError(db, rc);
  }
  return 0;
}
