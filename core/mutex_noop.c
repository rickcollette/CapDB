/*
** 2008 October 07
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
** This implementation in this file does not provide any mutual
** exclusion and is thus suitable for use only in applications
** that use SQLite in a single thread.  The routines defined
** here are place-holders.  Applications can substitute working
** mutex routines at start-time using the
**
**     capdb_config(CAPDB_CONFIG_MUTEX,...)
**
** interface.
**
** If compiled with CAPDB_DEBUG, then additional logic is inserted
** that does error checking on mutexes to make sure they are being
** called correctly.
*/
#include "capdbInt.h"

#ifndef CAPDB_MUTEX_OMIT

#ifndef CAPDB_DEBUG
/*
** Stub routines for all mutex methods.
**
** This routines provide no mutual exclusion or error checking.
*/
static int noopMutexInit(void){ return CAPDB_OK; }
static int noopMutexEnd(void){ return CAPDB_OK; }
static capdb_mutex *noopMutexAlloc(int id){ 
  UNUSED_PARAMETER(id);
  return (capdb_mutex*)8; 
}
static void noopMutexFree(capdb_mutex *p){ UNUSED_PARAMETER(p); return; }
static void noopMutexEnter(capdb_mutex *p){ UNUSED_PARAMETER(p); return; }
static int noopMutexTry(capdb_mutex *p){
  UNUSED_PARAMETER(p);
  return CAPDB_OK;
}
static void noopMutexLeave(capdb_mutex *p){ UNUSED_PARAMETER(p); return; }

capdb_mutex_methods const *capdbNoopMutex(void){
  static const capdb_mutex_methods sMutex = {
    noopMutexInit,
    noopMutexEnd,
    noopMutexAlloc,
    noopMutexFree,
    noopMutexEnter,
    noopMutexTry,
    noopMutexLeave,

    0,
    0,
  };

  return &sMutex;
}
#endif /* !CAPDB_DEBUG */

#ifdef CAPDB_DEBUG
/*
** In this implementation, error checking is provided for testing
** and debugging purposes.  The mutexes still do not provide any
** mutual exclusion.
*/

/*
** The mutex object
*/
typedef struct capdb_debug_mutex {
  int id;     /* The mutex type */
  int cnt;    /* Number of entries without a matching leave */
} capdb_debug_mutex;

/*
** The capdb_mutex_held() and capdb_mutex_notheld() routine are
** intended for use inside assert() statements.
*/
static int debugMutexHeld(capdb_mutex *pX){
  capdb_debug_mutex *p = (capdb_debug_mutex*)pX;
  return p==0 || p->cnt>0;
}
static int debugMutexNotheld(capdb_mutex *pX){
  capdb_debug_mutex *p = (capdb_debug_mutex*)pX;
  return p==0 || p->cnt==0;
}

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int debugMutexInit(void){ return CAPDB_OK; }
static int debugMutexEnd(void){ return CAPDB_OK; }

/*
** The capdb_mutex_alloc() routine allocates a new
** mutex and returns a pointer to it.  If it returns NULL
** that means that a mutex could not be allocated. 
*/
static capdb_mutex *debugMutexAlloc(int id){
  static capdb_debug_mutex aStatic[CAPDB_MUTEX_STATIC_VFS3 - 1];
  capdb_debug_mutex *pNew = 0;
  switch( id ){
    case CAPDB_MUTEX_FAST:
    case CAPDB_MUTEX_RECURSIVE: {
      pNew = capdbMalloc(sizeof(*pNew));
      if( pNew ){
        pNew->id = id;
        pNew->cnt = 0;
      }
      break;
    }
    default: {
#ifdef CAPDB_ENABLE_API_ARMOR
      if( id-2<0 || id-2>=ArraySize(aStatic) ){
        (void)CAPDB_MISUSE_BKPT;
        return 0;
      }
#endif
      pNew = &aStatic[id-2];
      pNew->id = id;
      break;
    }
  }
  return (capdb_mutex*)pNew;
}

/*
** This routine deallocates a previously allocated mutex.
*/
static void debugMutexFree(capdb_mutex *pX){
  capdb_debug_mutex *p = (capdb_debug_mutex*)pX;
  assert( p->cnt==0 );
  if( p->id==CAPDB_MUTEX_RECURSIVE || p->id==CAPDB_MUTEX_FAST ){
    capdb_free(p);
  }else{
#ifdef CAPDB_ENABLE_API_ARMOR
    (void)CAPDB_MISUSE_BKPT;
#endif
  }
}

/*
** The capdb_mutex_enter() and capdb_mutex_try() routines attempt
** to enter a mutex.  If another thread is already within the mutex,
** capdb_mutex_enter() will block and capdb_mutex_try() will return
** CAPDB_BUSY.  The capdb_mutex_try() interface returns CAPDB_OK
** upon successful entry.  Mutexes created using CAPDB_MUTEX_RECURSIVE can
** be entered multiple times by the same thread.  In such cases the,
** mutex must be exited an equal number of times before another thread
** can enter.  If the same thread tries to enter any other kind of mutex
** more than once, the behavior is undefined.
*/
static void debugMutexEnter(capdb_mutex *pX){
  capdb_debug_mutex *p = (capdb_debug_mutex*)pX;
  assert( p->id==CAPDB_MUTEX_RECURSIVE || debugMutexNotheld(pX) );
  p->cnt++;
}
static int debugMutexTry(capdb_mutex *pX){
  capdb_debug_mutex *p = (capdb_debug_mutex*)pX;
  assert( p->id==CAPDB_MUTEX_RECURSIVE || debugMutexNotheld(pX) );
  p->cnt++;
  return CAPDB_OK;
}

/*
** The capdb_mutex_leave() routine exits a mutex that was
** previously entered by the same thread.  The behavior
** is undefined if the mutex is not currently entered or
** is not currently allocated.  SQLite will never do either.
*/
static void debugMutexLeave(capdb_mutex *pX){
  capdb_debug_mutex *p = (capdb_debug_mutex*)pX;
  assert( debugMutexHeld(pX) );
  p->cnt--;
  assert( p->id==CAPDB_MUTEX_RECURSIVE || debugMutexNotheld(pX) );
}

capdb_mutex_methods const *capdbNoopMutex(void){
  static const capdb_mutex_methods sMutex = {
    debugMutexInit,
    debugMutexEnd,
    debugMutexAlloc,
    debugMutexFree,
    debugMutexEnter,
    debugMutexTry,
    debugMutexLeave,

    debugMutexHeld,
    debugMutexNotheld
  };

  return &sMutex;
}
#endif /* CAPDB_DEBUG */

/*
** If compiled with CAPDB_MUTEX_NOOP, then the no-op mutex implementation
** is used regardless of the run-time threadsafety setting.
*/
#ifdef CAPDB_MUTEX_NOOP
capdb_mutex_methods const *capdbDefaultMutex(void){
  return capdbNoopMutex();
}
#endif /* defined(CAPDB_MUTEX_NOOP) */
#endif /* !defined(CAPDB_MUTEX_OMIT) */
