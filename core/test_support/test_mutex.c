/*
** 2008 June 18
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains test logic for the capdb_mutex interfaces.
*/
#include "tclsqlite.h"
#include "capdb.h"
#include "capdbInt.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define MAX_MUTEXES        (CAPDB_MUTEX_STATIC_VFS3+1)
#define STATIC_MUTEXES     (MAX_MUTEXES-(CAPDB_MUTEX_RECURSIVE+1))

/* defined in main.c */
extern const char *capdbErrName(int);

static const char *aName[MAX_MUTEXES+1] = {
  "fast",        "recursive",   "static_main",   "static_mem",
  "static_open", "static_prng", "static_lru",    "static_pmem",
  "static_app1", "static_app2", "static_app3",   "static_vfs1",
  "static_vfs2", "static_vfs3", 0
};

/* A countable mutex */
struct capdb_mutex {
  capdb_mutex *pReal;
  int eType;
};

/* State variables */
static struct test_mutex_globals {
  int isInstalled;           /* True if installed */
  int disableInit;           /* True to cause capdb_initialize() to fail */
  int disableTry;            /* True to force capdb_mutex_try() to fail */
  int isInit;                /* True if initialized */
  capdb_mutex_methods m;   /* Interface to "real" mutex system */
  int aCounter[MAX_MUTEXES]; /* Number of grabs of each type of mutex */
  capdb_mutex aStatic[STATIC_MUTEXES]; /* The static mutexes */
} g = {0};

/* Return true if the countable mutex is currently held */
static int counterMutexHeld(capdb_mutex *p){
  return g.m.xMutexHeld(p->pReal);
}

/* Return true if the countable mutex is not currently held */
static int counterMutexNotheld(capdb_mutex *p){
  return g.m.xMutexNotheld(p->pReal);
}

/* Initialize the countable mutex interface
** Or, if g.disableInit is non-zero, then do not initialize but instead
** return the value of g.disableInit as the result code.  This can be used
** to simulate an initialization failure.
*/
static int counterMutexInit(void){ 
  int rc;
  if( g.disableInit ) return g.disableInit;
  rc = g.m.xMutexInit();
  g.isInit = 1;
  return rc;
}

/*
** Uninitialize the mutex subsystem
*/
static int counterMutexEnd(void){ 
  g.isInit = 0;
  return g.m.xMutexEnd();
}

/*
** Allocate a countable mutex
*/
static capdb_mutex *counterMutexAlloc(int eType){
  capdb_mutex *pReal;
  capdb_mutex *pRet = 0;

  assert( g.isInit );
  assert( eType>=CAPDB_MUTEX_FAST );
  assert( eType<=CAPDB_MUTEX_STATIC_VFS3 );

  pReal = g.m.xMutexAlloc(eType);
  if( !pReal ) return 0;

  if( eType==CAPDB_MUTEX_FAST || eType==CAPDB_MUTEX_RECURSIVE ){
    pRet = (capdb_mutex *)malloc(sizeof(capdb_mutex));
  }else{
    int eStaticType = eType - (MAX_MUTEXES - STATIC_MUTEXES);
    assert( eStaticType>=0 );
    assert( eStaticType<STATIC_MUTEXES );
    pRet = &g.aStatic[eStaticType];
  }

  pRet->eType = eType;
  pRet->pReal = pReal;
  return pRet;
}

/*
** Free a countable mutex
*/
static void counterMutexFree(capdb_mutex *p){
  assert( g.isInit );
  g.m.xMutexFree(p->pReal);
  if( p->eType==CAPDB_MUTEX_FAST || p->eType==CAPDB_MUTEX_RECURSIVE ){
    free(p);
  }
}

/*
** Enter a countable mutex.  Block until entry is safe.
*/
static void counterMutexEnter(capdb_mutex *p){
  assert( g.isInit );
  assert( p->eType>=0 );
  assert( p->eType<MAX_MUTEXES );
  g.aCounter[p->eType]++;
  g.m.xMutexEnter(p->pReal);
}

/*
** Try to enter a mutex.  Return true on success.
*/
static int counterMutexTry(capdb_mutex *p){
  assert( g.isInit );
  assert( p->eType>=0 );
  assert( p->eType<MAX_MUTEXES );
  g.aCounter[p->eType]++;
  if( g.disableTry ) return CAPDB_BUSY;
  return g.m.xMutexTry(p->pReal);
}

/* Leave a mutex
*/
static void counterMutexLeave(capdb_mutex *p){
  assert( g.isInit );
  g.m.xMutexLeave(p->pReal);
}

/*
** capdb_shutdown
*/
static int CAPDB_TCLAPI test_shutdown(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  rc = capdb_shutdown();
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** capdb_initialize
*/
static int CAPDB_TCLAPI test_initialize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  rc = capdb_initialize();
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** install_mutex_counters BOOLEAN
*/
static int CAPDB_TCLAPI test_install_mutex_counters(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = CAPDB_OK;
  int isInstall;

  capdb_mutex_methods counter_methods = {
    counterMutexInit,
    counterMutexEnd,
    counterMutexAlloc,
    counterMutexFree,
    counterMutexEnter,
    counterMutexTry,
    counterMutexLeave,
    counterMutexHeld,
    counterMutexNotheld
  };

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[1], &isInstall) ){
    return TCL_ERROR;
  }

  assert(isInstall==0 || isInstall==1);
  assert(g.isInstalled==0 || g.isInstalled==1);
  if( isInstall==g.isInstalled ){
    Tcl_AppendResult(interp, "mutex counters are ", NULL);
    Tcl_AppendResult(interp, isInstall?"already installed":"not installed", NULL);
    return TCL_ERROR;
  }

  if( isInstall ){
    assert( g.m.xMutexAlloc==0 );
    rc = capdb_config(CAPDB_CONFIG_GETMUTEX, &g.m);
    if( rc==CAPDB_OK ){
      capdb_config(CAPDB_CONFIG_MUTEX, &counter_methods);
    }
    g.disableTry = 0;
  }else{
    assert( g.m.xMutexAlloc );
    rc = capdb_config(CAPDB_CONFIG_MUTEX, &g.m);
    memset(&g.m, 0, sizeof(capdb_mutex_methods));
  }

  if( rc==CAPDB_OK ){
    g.isInstalled = isInstall;
  }

  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** read_mutex_counters
*/
static int CAPDB_TCLAPI test_read_mutex_counters(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Obj *pRet;
  int ii;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  pRet = Tcl_NewObj();
  Tcl_IncrRefCount(pRet);
  for(ii=0; ii<MAX_MUTEXES; ii++){
    Tcl_ListObjAppendElement(interp, pRet, Tcl_NewStringObj(aName[ii], -1));
    Tcl_ListObjAppendElement(interp, pRet, Tcl_NewIntObj(g.aCounter[ii]));
  }
  Tcl_SetObjResult(interp, pRet);
  Tcl_DecrRefCount(pRet);

  return TCL_OK;
}

/*
** clear_mutex_counters
*/
static int CAPDB_TCLAPI test_clear_mutex_counters(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int ii;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  for(ii=0; ii<MAX_MUTEXES; ii++){
    g.aCounter[ii] = 0;
  }
  return TCL_OK;
}

/*
** Create and free a mutex.  Return the mutex pointer.  The pointer
** will be invalid since the mutex has already been freed.  The
** return pointer just checks to see if the mutex really was allocated.
*/
static int CAPDB_TCLAPI test_alloc_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#if CAPDB_THREADSAFE
  capdb_mutex *p = capdb_mutex_alloc(CAPDB_MUTEX_FAST);
  char zBuf[100];
  capdb_mutex_free(p);
  capdb_snprintf(sizeof(zBuf), zBuf, "%p", p);
  Tcl_AppendResult(interp, zBuf, (char*)0);
#endif
  return TCL_OK;
}

/*
** capdb_config OPTION
**
** OPTION can be either one of the keywords:
**
**            CAPDB_CONFIG_SINGLETHREAD
**            CAPDB_CONFIG_MULTITHREAD
**            CAPDB_CONFIG_SERIALIZED
**
** Or OPTION can be an raw integer.
*/
static int CAPDB_TCLAPI test_config(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct ConfigOption {
    const char *zName;
    int iValue;
  } aOpt[] = {
    {"singlethread", CAPDB_CONFIG_SINGLETHREAD},
    {"multithread",  CAPDB_CONFIG_MULTITHREAD},
    {"serialized",   CAPDB_CONFIG_SERIALIZED},
    {0, 0}
  };
  int s = sizeof(struct ConfigOption);
  int i;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  if( Tcl_GetIndexFromObjStruct(interp, objv[1], aOpt, s, "flag", 0, &i) ){
    if( Tcl_GetIntFromObj(interp, objv[1], &i) ){
      return TCL_ERROR;
    }
  }else{
    i = aOpt[i].iValue;
  }

  rc = capdb_config(i);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

static capdb *getDbPointer(Tcl_Interp *pInterp, Tcl_Obj *pObj){
  capdb *db;
  Tcl_CmdInfo info;
  char *zCmd = Tcl_GetString(pObj);
  if( Tcl_GetCommandInfo(pInterp, zCmd, &info) ){
    db = *((capdb **)info.objClientData);
  }else{
    db = (capdb*)capdbTestTextToPtr(zCmd);
  }
  assert( db );
  return db;
}

static capdb_mutex *getStaticMutexPointer(
  Tcl_Interp *pInterp,
  Tcl_Obj *pObj
){
  int iMutex;
  if( Tcl_GetIndexFromObj(pInterp, pObj, aName, "mutex name", 0, &iMutex) ){
    return 0;
  }
  assert( iMutex!=CAPDB_MUTEX_FAST && iMutex!=CAPDB_MUTEX_RECURSIVE );
  return counterMutexAlloc(iMutex);
}

static int CAPDB_TCLAPI test_enter_static_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb_mutex *pMutex;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME");
    return TCL_ERROR;
  }
  pMutex = getStaticMutexPointer(interp, objv[1]);
  if( !pMutex ){
    return TCL_ERROR;
  }
  capdb_mutex_enter(pMutex);
  return TCL_OK;
}

static int CAPDB_TCLAPI test_leave_static_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb_mutex *pMutex;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME");
    return TCL_ERROR;
  }
  pMutex = getStaticMutexPointer(interp, objv[1]);
  if( !pMutex ){
    return TCL_ERROR;
  }
  capdb_mutex_leave(pMutex);
  return TCL_OK;
}

static int CAPDB_TCLAPI test_enter_db_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  db = getDbPointer(interp, objv[1]);
  if( !db ){
    return TCL_ERROR;
  }
  capdb_mutex_enter(capdb_db_mutex(db));
  return TCL_OK;
}

static int CAPDB_TCLAPI test_leave_db_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  db = getDbPointer(interp, objv[1]);
  if( !db ){
    return TCL_ERROR;
  }
  capdb_mutex_leave(capdb_db_mutex(db));
  return TCL_OK;
}

int Sqlitetest_mutex_Init(Tcl_Interp *interp){
  static struct {
    char *zName;
    Tcl_ObjCmdProc *xProc;
  } aCmd[] = {
    { "capdb_shutdown",        (Tcl_ObjCmdProc*)test_shutdown },
    { "capdb_initialize",      (Tcl_ObjCmdProc*)test_initialize },
    { "capdb_config",          (Tcl_ObjCmdProc*)test_config },

    { "enter_static_mutex",      (Tcl_ObjCmdProc*)test_enter_static_mutex },
    { "leave_static_mutex",      (Tcl_ObjCmdProc*)test_leave_static_mutex },

    { "enter_db_mutex",          (Tcl_ObjCmdProc*)test_enter_db_mutex },
    { "leave_db_mutex",          (Tcl_ObjCmdProc*)test_leave_db_mutex },

    { "alloc_dealloc_mutex",     (Tcl_ObjCmdProc*)test_alloc_mutex },
    { "install_mutex_counters",  (Tcl_ObjCmdProc*)test_install_mutex_counters },
    { "read_mutex_counters",     (Tcl_ObjCmdProc*)test_read_mutex_counters },
    { "clear_mutex_counters",    (Tcl_ObjCmdProc*)test_clear_mutex_counters },
  };
  int i;
  for(i=0; i<sizeof(aCmd)/sizeof(aCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
  }

  Tcl_LinkVar(interp, "disable_mutex_init", 
              (char*)&g.disableInit, TCL_LINK_INT);
  Tcl_LinkVar(interp, "disable_mutex_try", 
              (char*)&g.disableTry, TCL_LINK_INT);
  return CAPDB_OK;
}
