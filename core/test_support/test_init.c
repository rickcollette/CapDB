/*
** 2009 August 17
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
** The code in this file is used for testing SQLite. It is not part of
** the source code used in production systems.
**
** Specifically, this file tests the effect of errors while initializing
** the various pluggable sub-systems from within capdb_initialize().
** If an error occurs in capdb_initialize() the following should be
** true:
**
**   1) An error code is returned to the user, and
**   2) A subsequent call to capdb_shutdown() calls the shutdown method
**      of those subsystems that were initialized, and
**   3) A subsequent call to capdb_initialize() attempts to initialize
**      the remaining, uninitialized, subsystems.
*/

#include "capdbInt.h"
#include <string.h>
#include "tclsqlite.h"

static struct Wrapped {
  capdb_pcache_methods2 pcache;
  capdb_mem_methods     mem;
  capdb_mutex_methods   mutex;

  int mem_init;              /* True if mem subsystem is initialized */
  int mem_fail;              /* True to fail mem subsystem initialization */
  int mutex_init;            /* True if mutex subsystem is initialized */
  int mutex_fail;            /* True to fail mutex subsystem initialization */
  int pcache_init;           /* True if pcache subsystem is initialized */
  int pcache_fail;           /* True to fail pcache subsystem initialization */
} wrapped;

static int wrMemInit(void *pAppData){
  int rc;
  if( wrapped.mem_fail ){
    rc = CAPDB_ERROR;
  }else{
    rc = wrapped.mem.xInit(wrapped.mem.pAppData);
  }
  if( rc==CAPDB_OK ){
    wrapped.mem_init = 1;
  }
  return rc;
}
static void wrMemShutdown(void *pAppData){
  wrapped.mem.xShutdown(wrapped.mem.pAppData);
  wrapped.mem_init = 0;
}
static void *wrMemMalloc(int n)           {return wrapped.mem.xMalloc(n);}
static void wrMemFree(void *p)            {wrapped.mem.xFree(p);}
static void *wrMemRealloc(void *p, int n) {return wrapped.mem.xRealloc(p, n);}
static int wrMemSize(void *p)             {return wrapped.mem.xSize(p);}
static int wrMemRoundup(int n)            {return wrapped.mem.xRoundup(n);}


static int wrMutexInit(void){
  int rc;
  if( wrapped.mutex_fail ){
    rc = CAPDB_ERROR;
  }else{
    rc = wrapped.mutex.xMutexInit();
  }
  if( rc==CAPDB_OK ){
    wrapped.mutex_init = 1;
  }
  return rc;
}
static int wrMutexEnd(void){
  wrapped.mutex.xMutexEnd();
  wrapped.mutex_init = 0;
  return CAPDB_OK;
}
static capdb_mutex *wrMutexAlloc(int e){
  return wrapped.mutex.xMutexAlloc(e);
}
static void wrMutexFree(capdb_mutex *p){
  wrapped.mutex.xMutexFree(p);
}
static void wrMutexEnter(capdb_mutex *p){
  wrapped.mutex.xMutexEnter(p);
}
static int wrMutexTry(capdb_mutex *p){
  return wrapped.mutex.xMutexTry(p);
}
static void wrMutexLeave(capdb_mutex *p){
  wrapped.mutex.xMutexLeave(p);
}
static int wrMutexHeld(capdb_mutex *p){
  return wrapped.mutex.xMutexHeld(p);
}
static int wrMutexNotheld(capdb_mutex *p){
  return wrapped.mutex.xMutexNotheld(p);
}



static int wrPCacheInit(void *pArg){
  int rc;
  if( wrapped.pcache_fail ){
    rc = CAPDB_ERROR;
  }else{
    rc = wrapped.pcache.xInit(wrapped.pcache.pArg);
  }
  if( rc==CAPDB_OK ){
    wrapped.pcache_init = 1;
  }
  return rc;
}
static void wrPCacheShutdown(void *pArg){
  wrapped.pcache.xShutdown(wrapped.pcache.pArg);
  wrapped.pcache_init = 0;
}

static capdb_pcache *wrPCacheCreate(int a, int b, int c){
  return wrapped.pcache.xCreate(a, b, c);
}  
static void wrPCacheCachesize(capdb_pcache *p, int n){
  wrapped.pcache.xCachesize(p, n);
}  
static int wrPCachePagecount(capdb_pcache *p){
  return wrapped.pcache.xPagecount(p);
}  
static capdb_pcache_page *wrPCacheFetch(capdb_pcache *p, unsigned a, int b){
  return wrapped.pcache.xFetch(p, a, b);
}  
static void wrPCacheUnpin(capdb_pcache *p, capdb_pcache_page *a, int b){
  wrapped.pcache.xUnpin(p, a, b);
}  
static void wrPCacheRekey(
  capdb_pcache *p, 
  capdb_pcache_page *a, 
  unsigned b, 
  unsigned c
){
  wrapped.pcache.xRekey(p, a, b, c);
}  
static void wrPCacheTruncate(capdb_pcache *p, unsigned a){
  wrapped.pcache.xTruncate(p, a);
}  
static void wrPCacheDestroy(capdb_pcache *p){
  wrapped.pcache.xDestroy(p);
}  

static void installInitWrappers(void){
  capdb_mutex_methods mutexmethods = {
    wrMutexInit,  wrMutexEnd,   wrMutexAlloc,
    wrMutexFree,  wrMutexEnter, wrMutexTry,
    wrMutexLeave, wrMutexHeld,  wrMutexNotheld
  };
  capdb_pcache_methods2 pcachemethods = {
    1, 0,
    wrPCacheInit,      wrPCacheShutdown,  wrPCacheCreate, 
    wrPCacheCachesize, wrPCachePagecount, wrPCacheFetch,
    wrPCacheUnpin,     wrPCacheRekey,     wrPCacheTruncate,  
    wrPCacheDestroy
  };
  capdb_mem_methods memmethods = {
    wrMemMalloc,   wrMemFree,    wrMemRealloc,
    wrMemSize,     wrMemRoundup, wrMemInit,
    wrMemShutdown,
    0
  };

  memset(&wrapped, 0, sizeof(wrapped));

  capdb_shutdown();
  capdb_config(CAPDB_CONFIG_GETMUTEX, &wrapped.mutex);
  capdb_config(CAPDB_CONFIG_GETMALLOC, &wrapped.mem);
  capdb_config(CAPDB_CONFIG_GETPCACHE2, &wrapped.pcache);
  capdb_config(CAPDB_CONFIG_MUTEX, &mutexmethods);
  capdb_config(CAPDB_CONFIG_MALLOC, &memmethods);
  capdb_config(CAPDB_CONFIG_PCACHE2, &pcachemethods);
}

static int CAPDB_TCLAPI init_wrapper_install(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  int i;
  installInitWrappers();
  for(i=1; i<objc; i++){
    char *z = Tcl_GetString(objv[i]);
    if( strcmp(z, "mem")==0 ){
      wrapped.mem_fail = 1;
    }else if( strcmp(z, "mutex")==0 ){
      wrapped.mutex_fail = 1;
    }else if( strcmp(z, "pcache")==0 ){
      wrapped.pcache_fail = 1;
    }else{
      Tcl_AppendResult(interp, "Unknown argument: \"", z, "\"", NULL);
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

static int CAPDB_TCLAPI init_wrapper_uninstall(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  capdb_shutdown();
  capdb_config(CAPDB_CONFIG_MUTEX, &wrapped.mutex);
  capdb_config(CAPDB_CONFIG_MALLOC, &wrapped.mem);
  capdb_config(CAPDB_CONFIG_PCACHE2, &wrapped.pcache);
  return TCL_OK;
}

static int CAPDB_TCLAPI init_wrapper_clear(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  wrapped.mem_fail = 0;
  wrapped.mutex_fail = 0;
  wrapped.pcache_fail = 0;
  return TCL_OK;
}

static int CAPDB_TCLAPI init_wrapper_query(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  Tcl_Obj *pRet;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  pRet = Tcl_NewObj();
  if( wrapped.mutex_init ){
    Tcl_ListObjAppendElement(interp, pRet, Tcl_NewStringObj("mutex", -1));
  }
  if( wrapped.mem_init ){
    Tcl_ListObjAppendElement(interp, pRet, Tcl_NewStringObj("mem", -1));
  }
  if( wrapped.pcache_init ){
    Tcl_ListObjAppendElement(interp, pRet, Tcl_NewStringObj("pcache", -1));
  }

  Tcl_SetObjResult(interp, pRet);
  return TCL_OK;
}

int Sqlitetest_init_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
  } aObjCmd[] = {
    {"init_wrapper_install",   init_wrapper_install},
    {"init_wrapper_query",     init_wrapper_query  },
    {"init_wrapper_uninstall", init_wrapper_uninstall},
    {"init_wrapper_clear",     init_wrapper_clear}
  };
  int i;

  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, 0, 0);
  }

  return TCL_OK;
}
