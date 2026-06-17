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
** This file contains the C functions that implement mutexes.
**
** This file contains code that is common across all mutex implementations.
*/
#include "capdbInt.h"

#if defined(CAPDB_DEBUG) && !defined(CAPDB_MUTEX_OMIT)
/*
** For debugging purposes, record when the mutex subsystem is initialized
** and uninitialized so that we can assert() if there is an attempt to
** allocate a mutex while the system is uninitialized.
*/
static CAPDB_WSD int mutexIsInit = 0;
#endif /* CAPDB_DEBUG && !defined(CAPDB_MUTEX_OMIT) */


#ifndef CAPDB_MUTEX_OMIT

#ifdef CAPDB_THREAD_MISUSE_WARNINGS
/*
** This block (enclosed by CAPDB_THREAD_MISUSE_WARNINGS) contains
** the implementation of a wrapper around the system default mutex
** implementation (capdbDefaultMutex()). 
**
** Most calls are passed directly through to the underlying default
** mutex implementation. Except, if a mutex is configured by calling
** capdbMutexWarnOnContention() on it, then if contention is ever
** encountered within xMutexEnter() then a warning is emitted via
** capdb_log().  Furthermore, if CAPDB_THREAD_MISUSE_ABORT is
** defined then abort() is called after the capdb_log() warning.
**
** This type of mutex is used on the database handle mutex when testing
** apps that usually use CAPDB_CONFIG_MULTITHREAD mode.  A failure
** indicates that the app ought to be using CAPDB_OPEN_FULLMUTEX or
** similar because it is trying to use the same database handle from
** two different connections at the same time.
*/

/* 
** Type for all mutexes used when CAPDB_THREAD_MISUSE_WARNINGS
** is defined. Variable CheckMutex.mutex is a pointer to the real mutex
** allocated by the system mutex implementation. Variable iType is usually set
** to the type of mutex requested - CAPDB_MUTEX_RECURSIVE, CAPDB_MUTEX_FAST
** or one of the static mutex identifiers. Or, if this is a recursive mutex
** that has been configured using capdbMutexWarnOnContention(), it is
** set to CAPDB_MUTEX_WARNONCONTENTION.
*/
typedef struct CheckMutex CheckMutex;
struct CheckMutex {
  int iType;
  capdb_mutex *mutex;
};

#define CAPDB_MUTEX_WARNONCONTENTION  (-1)

/* 
** Pointer to real mutex methods object used by the CheckMutex
** implementation. Set by checkMutexInit(). 
*/
static CAPDB_WSD const capdb_mutex_methods *pGlobalMutexMethods;

#ifdef CAPDB_DEBUG
static int checkMutexHeld(capdb_mutex *p){
  return pGlobalMutexMethods->xMutexHeld(((CheckMutex*)p)->mutex);
}
static int checkMutexNotheld(capdb_mutex *p){
  return pGlobalMutexMethods->xMutexNotheld(((CheckMutex*)p)->mutex);
}
#endif

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int checkMutexInit(void){ 
  pGlobalMutexMethods = capdbDefaultMutex();
  return pGlobalMutexMethods->xMutexInit(); 
}
static int checkMutexEnd(void){ 
  int rc = pGlobalMutexMethods->xMutexEnd(); 
  pGlobalMutexMethods = 0;
  return rc;
}

/*
** Allocate a mutex.
*/
static capdb_mutex *checkMutexAlloc(int iType){
  static CheckMutex staticMutexes[] = {
    {2, 0}, {3, 0}, {4, 0}, {5, 0},
    {6, 0}, {7, 0}, {8, 0}, {9, 0},
    {10, 0}, {11, 0}, {12, 0}, {13, 0}
  };
  CheckMutex *p = 0;

  assert( CAPDB_MUTEX_RECURSIVE==1 && CAPDB_MUTEX_FAST==0 );
  if( iType<2 ){
    p = capdbMallocZero(sizeof(CheckMutex));
    if( p==0 ) return 0;
    p->iType = iType;
  }else{
#ifdef CAPDB_ENABLE_API_ARMOR
    if( iType-2>=ArraySize(staticMutexes) ){
      (void)CAPDB_MISUSE_BKPT;
      return 0;
    }
#endif
    p = &staticMutexes[iType-2];
  }

  if( p->mutex==0 ){
    p->mutex = pGlobalMutexMethods->xMutexAlloc(iType);
    if( p->mutex==0 ){
      if( iType<2 ){
        capdb_free(p);
      }
      p = 0;
    }
  }

  return (capdb_mutex*)p;
}

/*
** Free a mutex.
*/
static void checkMutexFree(capdb_mutex *p){
  assert( CAPDB_MUTEX_RECURSIVE<2 );
  assert( CAPDB_MUTEX_FAST<2 );
  assert( CAPDB_MUTEX_WARNONCONTENTION<2 );

#ifdef CAPDB_ENABLE_API_ARMOR
  if( ((CheckMutex*)p)->iType<2 )
#endif
  {
    CheckMutex *pCheck = (CheckMutex*)p;
    pGlobalMutexMethods->xMutexFree(pCheck->mutex);
    capdb_free(pCheck);
  }
#ifdef CAPDB_ENABLE_API_ARMOR
  else{
    (void)CAPDB_MISUSE_BKPT;
  }
#endif
}

/*
** Enter the mutex.
*/
static void checkMutexEnter(capdb_mutex *p){
  CheckMutex *pCheck = (CheckMutex*)p;
  if( pCheck->iType==CAPDB_MUTEX_WARNONCONTENTION ){
    if( CAPDB_OK==pGlobalMutexMethods->xMutexTry(pCheck->mutex) ){
      return;
    }
    capdb_log(CAPDB_MISUSE, 
        "illegal multi-threaded access to database connection"
    );
#if CAPDB_THREAD_MISUSE_ABORT
    abort();
#endif
  }
  pGlobalMutexMethods->xMutexEnter(pCheck->mutex);
}

/*
** Enter the mutex (do not block).
*/
static int checkMutexTry(capdb_mutex *p){
  CheckMutex *pCheck = (CheckMutex*)p;
  return pGlobalMutexMethods->xMutexTry(pCheck->mutex);
}

/*
** Leave the mutex.
*/
static void checkMutexLeave(capdb_mutex *p){
  CheckMutex *pCheck = (CheckMutex*)p;
  pGlobalMutexMethods->xMutexLeave(pCheck->mutex);
}

capdb_mutex_methods const *multiThreadedCheckMutex(void){
  static const capdb_mutex_methods sMutex = {
    checkMutexInit,
    checkMutexEnd,
    checkMutexAlloc,
    checkMutexFree,
    checkMutexEnter,
    checkMutexTry,
    checkMutexLeave,
#ifdef CAPDB_DEBUG
    checkMutexHeld,
    checkMutexNotheld
#else
    0,
    0
#endif
  };
  return &sMutex;
}

/*
** Mark the CAPDB_MUTEX_RECURSIVE mutex passed as the only argument as
** one on which there should be no contention.
*/
void capdbMutexWarnOnContention(capdb_mutex *p){
  if( capdbGlobalConfig.mutex.xMutexAlloc==checkMutexAlloc ){
    CheckMutex *pCheck = (CheckMutex*)p;
    assert( pCheck->iType==CAPDB_MUTEX_RECURSIVE );
    pCheck->iType = CAPDB_MUTEX_WARNONCONTENTION;
  }
}
#endif   /* ifdef CAPDB_THREAD_MISUSE_WARNINGS */

/*
** Initialize the mutex system.
*/
int capdbMutexInit(void){ 
  int rc = CAPDB_OK;
  if( !capdbGlobalConfig.mutex.xMutexAlloc ){
    /* If the xMutexAlloc method has not been set, then the user did not
    ** install a mutex implementation via capdb_config() prior to 
    ** capdb_initialize() being called. This block copies pointers to
    ** the default implementation into the capdbGlobalConfig structure.
    */
    capdb_mutex_methods const *pFrom;
    capdb_mutex_methods *pTo = &capdbGlobalConfig.mutex;

    if( capdbGlobalConfig.bCoreMutex ){
#ifdef CAPDB_THREAD_MISUSE_WARNINGS
      pFrom = multiThreadedCheckMutex();
#else
      pFrom = capdbDefaultMutex();
#endif
    }else{
      pFrom = capdbNoopMutex();
    }
    pTo->xMutexInit = pFrom->xMutexInit;
    pTo->xMutexEnd = pFrom->xMutexEnd;
    pTo->xMutexFree = pFrom->xMutexFree;
    pTo->xMutexEnter = pFrom->xMutexEnter;
    pTo->xMutexTry = pFrom->xMutexTry;
    pTo->xMutexLeave = pFrom->xMutexLeave;
    pTo->xMutexHeld = pFrom->xMutexHeld;
    pTo->xMutexNotheld = pFrom->xMutexNotheld;
    capdbMemoryBarrier();
    pTo->xMutexAlloc = pFrom->xMutexAlloc;
  }
  assert( capdbGlobalConfig.mutex.xMutexInit );
  rc = capdbGlobalConfig.mutex.xMutexInit();

#ifdef CAPDB_DEBUG
  GLOBAL(int, mutexIsInit) = 1;
#endif

  capdbMemoryBarrier();
  return rc;
}

/*
** Shutdown the mutex system. This call frees resources allocated by
** capdbMutexInit().
*/
int capdbMutexEnd(void){
  int rc = CAPDB_OK;
  if( capdbGlobalConfig.mutex.xMutexEnd ){
    rc = capdbGlobalConfig.mutex.xMutexEnd();
  }

#ifdef CAPDB_DEBUG
  GLOBAL(int, mutexIsInit) = 0;
#endif

  return rc;
}

/*
** Retrieve a pointer to a static mutex or allocate a new dynamic one.
*/
capdb_mutex *capdb_mutex_alloc(int id){
#ifndef CAPDB_OMIT_AUTOINIT
  if( id<=CAPDB_MUTEX_RECURSIVE && capdb_initialize() ) return 0;
  if( id>CAPDB_MUTEX_RECURSIVE && capdbMutexInit() ) return 0;
#endif
  assert( capdbGlobalConfig.mutex.xMutexAlloc );
  return capdbGlobalConfig.mutex.xMutexAlloc(id);
}

capdb_mutex *capdbMutexAlloc(int id){
  if( !capdbGlobalConfig.bCoreMutex ){
    return 0;
  }
  assert( GLOBAL(int, mutexIsInit) );
  assert( capdbGlobalConfig.mutex.xMutexAlloc );
  return capdbGlobalConfig.mutex.xMutexAlloc(id);
}

/*
** Free a dynamic mutex.
*/
void capdb_mutex_free(capdb_mutex *p){
  if( p ){
    assert( capdbGlobalConfig.mutex.xMutexFree );
    capdbGlobalConfig.mutex.xMutexFree(p);
  }
}

/*
** Obtain the mutex p. If some other thread already has the mutex, block
** until it can be obtained.
*/
void capdb_mutex_enter(capdb_mutex *p){
  if( p ){
    assert( capdbGlobalConfig.mutex.xMutexEnter );
    capdbGlobalConfig.mutex.xMutexEnter(p);
  }
}

/*
** Obtain the mutex p. If successful, return CAPDB_OK. Otherwise, if another
** thread holds the mutex and it cannot be obtained, return CAPDB_BUSY.
*/
int capdb_mutex_try(capdb_mutex *p){
  int rc = CAPDB_OK;
  if( p ){
    assert( capdbGlobalConfig.mutex.xMutexTry );
    return capdbGlobalConfig.mutex.xMutexTry(p);
  }
  return rc;
}

/*
** The capdb_mutex_leave() routine exits a mutex that was previously
** entered by the same thread.  The behavior is undefined if the mutex 
** is not currently entered. If a NULL pointer is passed as an argument
** this function is a no-op.
*/
void capdb_mutex_leave(capdb_mutex *p){
  if( p ){
    assert( capdbGlobalConfig.mutex.xMutexLeave );
    capdbGlobalConfig.mutex.xMutexLeave(p);
  }
}

#ifndef NDEBUG
/*
** The capdb_mutex_held() and capdb_mutex_notheld() routine are
** intended for use inside assert() statements.
**
** Because these routines raise false-positive alerts in TSAN, disable
** them (make them always return 1) when compiling with TSAN.
*/
int capdb_mutex_held(capdb_mutex *p){
# if defined(__has_feature)
#   if __has_feature(thread_sanitizer)
      p = 0;
#   endif
# endif
  assert( p==0 || capdbGlobalConfig.mutex.xMutexHeld );
  return p==0 || capdbGlobalConfig.mutex.xMutexHeld(p);
}
int capdb_mutex_notheld(capdb_mutex *p){
# if defined(__has_feature)
#   if __has_feature(thread_sanitizer)
      p = 0;
#   endif
# endif
  assert( p==0 || capdbGlobalConfig.mutex.xMutexNotheld );
  return p==0 || capdbGlobalConfig.mutex.xMutexNotheld(p);
}
#endif /* NDEBUG */

#endif /* !defined(CAPDB_MUTEX_OMIT) */
