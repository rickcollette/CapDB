/*
** 2007 September 9
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
** This file contains the implementation of some Tcl commands used to
** test that capdb database handles may be concurrently accessed by 
** multiple threads. Right now this only works on unix.
*/

#include "capdbInt.h"
#include "tclsqlite.h"

#if CAPDB_THREADSAFE

#include <errno.h>

#if !defined(_MSC_VER)
#include <unistd.h>
#endif

/*
** One of these is allocated for each thread created by [sqlthread spawn].
*/
typedef struct SqlThread SqlThread;
struct SqlThread {
  Tcl_ThreadId parent;     /* Thread id of parent thread */
  Tcl_Interp *interp;      /* Parent interpreter */
  char *zScript;           /* The script to execute. */
  char *zVarname;          /* Varname in parent script */
};

/*
** A custom Tcl_Event type used by this module. When the event is
** handled, script zScript is evaluated in interpreter interp. If
** the evaluation throws an exception (returns TCL_ERROR), then the
** error is handled by Tcl_BackgroundError(). If no error occurs,
** the result is simply discarded.
*/
typedef struct EvalEvent EvalEvent;
struct EvalEvent {
  Tcl_Event base;          /* Base class of type Tcl_Event */
  char *zScript;           /* The script to execute. */
  Tcl_Interp *interp;      /* The interpreter to execute it in. */
};

static Tcl_ObjCmdProc sqlthread_proc;
static Tcl_ObjCmdProc clock_seconds_proc;
#if CAPDB_OS_UNIX && defined(CAPDB_ENABLE_UNLOCK_NOTIFY)
static Tcl_ObjCmdProc blocking_step_proc;
static Tcl_ObjCmdProc blocking_prepare_v2_proc;
#endif
int Sqlitetest1_Init(Tcl_Interp *);
int Sqlite3_Init(Tcl_Interp *);

/* Functions from main.c */
extern const char *capdbErrName(int);

/* Functions from test1.c */
extern void *capdbTestTextToPtr(const char *);
extern int getDbPointer(Tcl_Interp *, const char *, capdb **);
extern int capdbTestMakePointerStr(Tcl_Interp *, char *, void *);
extern int capdbTestErrCode(Tcl_Interp *, capdb *, int);

/*
** Handler for events of type EvalEvent.
*/
static int CAPDB_TCLAPI tclScriptEvent(Tcl_Event *evPtr, int flags){
  int rc;
  EvalEvent *p = (EvalEvent *)evPtr;
  rc = Tcl_Eval(p->interp, p->zScript);
  if( rc!=TCL_OK ){
    Tcl_BackgroundError(p->interp);
  }
  UNUSED_PARAMETER(flags);
  return 1;
}

/*
** Register an EvalEvent to evaluate the script pScript in the
** parent interpreter/thread of SqlThread p.
*/
static void postToParent(SqlThread *p, Tcl_Obj *pScript){
  EvalEvent *pEvent;
  char *zMsg;
  Tcl_Size nMsg;

  zMsg = Tcl_GetStringFromObj(pScript, &nMsg); 
  pEvent = (EvalEvent *)ckalloc(sizeof(EvalEvent)+nMsg+1);
  pEvent->base.nextPtr = 0;
  pEvent->base.proc = tclScriptEvent;
  pEvent->zScript = (char *)&pEvent[1];
  memcpy(pEvent->zScript, zMsg, nMsg+1);
  pEvent->interp = p->interp;

  Tcl_ThreadQueueEvent(p->parent, (Tcl_Event *)pEvent, TCL_QUEUE_TAIL);
  Tcl_ThreadAlert(p->parent);
}

/*
** The main function for threads created with [sqlthread spawn].
*/
static Tcl_ThreadCreateType tclScriptThread(ClientData pSqlThread){
  Tcl_Interp *interp;
  Tcl_Obj *pRes;
  Tcl_Obj *pList;
  int rc;
  SqlThread *p = (SqlThread *)pSqlThread;
  extern int Sqlitetest_mutex_Init(Tcl_Interp*);

  interp = Tcl_CreateInterp();
  Tcl_CreateObjCommand(interp, "clock_seconds", clock_seconds_proc, 0, 0);
  Tcl_CreateObjCommand(interp, "sqlthread", sqlthread_proc, pSqlThread, 0);
#if CAPDB_OS_UNIX && defined(CAPDB_ENABLE_UNLOCK_NOTIFY)
  Tcl_CreateObjCommand(interp, "capdb_blocking_step", blocking_step_proc,0,0);
  Tcl_CreateObjCommand(interp, 
      "capdb_blocking_prepare_v2", blocking_prepare_v2_proc, (void *)1, 0);
  Tcl_CreateObjCommand(interp, 
      "capdb_nonblocking_prepare_v2", blocking_prepare_v2_proc, 0, 0);
#endif
  Sqlitetest1_Init(interp);
  Sqlitetest_mutex_Init(interp);
  Sqlite3_Init(interp);

  rc = Tcl_Eval(interp, p->zScript);
  pRes = Tcl_GetObjResult(interp);
  pList = Tcl_NewObj();
  Tcl_IncrRefCount(pList);
  Tcl_IncrRefCount(pRes);

  if( rc!=TCL_OK ){
    Tcl_ListObjAppendElement(interp, pList, Tcl_NewStringObj("error", -1));
    Tcl_ListObjAppendElement(interp, pList, pRes);
    postToParent(p, pList);
    Tcl_DecrRefCount(pList);
    pList = Tcl_NewObj();
  }

  Tcl_ListObjAppendElement(interp, pList, Tcl_NewStringObj("set", -1));
  Tcl_ListObjAppendElement(interp, pList, Tcl_NewStringObj(p->zVarname, -1));
  Tcl_ListObjAppendElement(interp, pList, pRes);
  postToParent(p, pList);

  ckfree((void *)p);
  Tcl_DecrRefCount(pList);
  Tcl_DecrRefCount(pRes);
  Tcl_DeleteInterp(interp);
  while( Tcl_DoOneEvent(TCL_ALL_EVENTS|TCL_DONT_WAIT) );
  Tcl_ExitThread(0);
  TCL_THREAD_CREATE_RETURN;
}

/*
** sqlthread spawn VARNAME SCRIPT
**
**     Spawn a new thread with its own Tcl interpreter and run the
**     specified SCRIPT(s) in it. The thread terminates after running
**     the script. The result of the script is stored in the variable
**     VARNAME.
**
**     The caller can wait for the script to terminate using [vwait VARNAME].
*/
static int CAPDB_TCLAPI sqlthread_spawn(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_ThreadId x;
  SqlThread *pNew;
  int rc;

  Tcl_Size nVarname; char *zVarname;
  Tcl_Size nScript; char *zScript;

  /* Parameters for thread creation */
  const int nStack = TCL_THREAD_STACK_DEFAULT;
  const int flags = TCL_THREAD_NOFLAGS;

  assert(objc==4);
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);

  zVarname = Tcl_GetStringFromObj(objv[2], &nVarname);
  zScript = Tcl_GetStringFromObj(objv[3], &nScript);

  pNew = (SqlThread *)ckalloc(sizeof(SqlThread)+nVarname+nScript+2);
  pNew->zVarname = (char *)&pNew[1];
  pNew->zScript = (char *)&pNew->zVarname[nVarname+1];
  memcpy(pNew->zVarname, zVarname, nVarname+1);
  memcpy(pNew->zScript, zScript, nScript+1);
  pNew->parent = Tcl_GetCurrentThread();
  pNew->interp = interp;

  rc = Tcl_CreateThread(&x, tclScriptThread, (void *)pNew, nStack, flags);
  if( rc!=TCL_OK ){
    Tcl_AppendResult(interp, "Error in Tcl_CreateThread()", NULL);
    ckfree((char *)pNew);
    return TCL_ERROR;
  }

  return TCL_OK;
}

/*
** sqlthread parent SCRIPT
**
**     This can be called by spawned threads only. It sends the specified
**     script back to the parent thread for execution. The result of
**     evaluating the SCRIPT is returned. The parent thread must enter
**     the event loop for this to work - otherwise the caller will
**     block indefinitely.
**
**     NOTE: At the moment, this doesn't work. FIXME.
*/
static int CAPDB_TCLAPI sqlthread_parent(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  EvalEvent *pEvent;
  char *zMsg;
  Tcl_Size nMsg;
  SqlThread *p = (SqlThread *)clientData;

  assert(objc==3);
  UNUSED_PARAMETER(objc);

  if( p==0 ){
    Tcl_AppendResult(interp, "no parent thread", NULL);
    return TCL_ERROR;
  }

  zMsg = Tcl_GetStringFromObj(objv[2], &nMsg);
  pEvent = (EvalEvent *)ckalloc(sizeof(EvalEvent)+nMsg+1);
  pEvent->base.nextPtr = 0;
  pEvent->base.proc = tclScriptEvent;
  pEvent->zScript = (char *)&pEvent[1];
  memcpy(pEvent->zScript, zMsg, nMsg+1);
  pEvent->interp = p->interp;
  Tcl_ThreadQueueEvent(p->parent, (Tcl_Event *)pEvent, TCL_QUEUE_TAIL);
  Tcl_ThreadAlert(p->parent);

  return TCL_OK;
}

static int xBusy(void *pArg, int nBusy){
  UNUSED_PARAMETER(pArg);
  UNUSED_PARAMETER(nBusy);
  capdb_sleep(50);
  return 1;             /* Try again... */
}

/*
** sqlthread open
**
**     Open a database handle and return the string representation of
**     the pointer value.
*/
static int CAPDB_TCLAPI sqlthread_open(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int capdbTestMakePointerStr(Tcl_Interp *interp, char *zPtr, void *p);

  const char *zFilename;
  capdb *db;
  char zBuf[100];
  extern int Md5_Register(capdb*,char**,const capdb_api_routines*);

  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);

  zFilename = Tcl_GetString(objv[2]);
  capdb_open(zFilename, &db);
  Md5_Register(db, 0, 0);
  capdb_busy_handler(db, xBusy, 0);
  
  if( capdbTestMakePointerStr(interp, zBuf, db) ) return TCL_ERROR;
  Tcl_AppendResult(interp, zBuf, NULL);

  return TCL_OK;
}


/*
** sqlthread open
**
**     Return the current thread-id (Tcl_GetCurrentThread()) cast to
**     an integer.
*/
static int CAPDB_TCLAPI sqlthread_id(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_ThreadId id = Tcl_GetCurrentThread();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(CAPDB_PTR_TO_INT(id)));
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
  return TCL_OK;
}


/*
** Dispatch routine for the sub-commands of [sqlthread].
*/
static int CAPDB_TCLAPI sqlthread_proc(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct SubCommand {
    char *zName;
    Tcl_ObjCmdProc *xProc;
    int nArg;
    char *zUsage;
  } aSub[] = {
    {"parent", sqlthread_parent, 1, "SCRIPT"},
    {"spawn",  sqlthread_spawn,  2, "VARNAME SCRIPT"},
    {"open",   sqlthread_open,   1, "DBNAME"},
    {"id",     sqlthread_id,     0, ""},
    {0, 0, 0}
  };
  struct SubCommand *pSub;
  int rc;
  int iIndex;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND");
    return TCL_ERROR;
  }

  rc = Tcl_GetIndexFromObjStruct(
      interp, objv[1], aSub, sizeof(aSub[0]), "sub-command", 0, &iIndex
  );
  if( rc!=TCL_OK ) return rc;
  pSub = &aSub[iIndex];

  if( objc<(pSub->nArg+2) ){
    Tcl_WrongNumArgs(interp, 2, objv, pSub->zUsage);
    return TCL_ERROR;
  }

  return pSub->xProc(clientData, interp, objc, objv);
}

/*
** The [clock_seconds] command. This is more or less the same as the
** regular tcl [clock seconds], except that it is available in testfixture
** when linked against both Tcl 8.4 and 8.5. Because [clock seconds] is
** implemented as a script in Tcl 8.5, it is not usually available to
** testfixture.
*/ 
static int CAPDB_TCLAPI clock_seconds_proc(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Time now;
  Tcl_GetTime(&now);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(now.sec));
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
  return TCL_OK;
}

/*
** The [clock_milliseconds] command. This is more or less the same as the
** regular tcl [clock milliseconds]. 
*/ 
static int CAPDB_TCLAPI clock_milliseconds_proc(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Time now;
  Tcl_GetTime(&now);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(
    ((Tcl_WideInt)now.sec * 1000) + (now.usec / 1000)
  ));
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
  return TCL_OK;
}

/*************************************************************************
** This block contains the implementation of the [capdb_blocking_step]
** command available to threads created by [sqlthread spawn] commands. It
** is only available on UNIX for now. This is because pthread condition
** variables are used.
**
** The source code for the C functions capdb_blocking_step(),
** blocking_step_notify() and the structure UnlockNotification is
** automatically extracted from this file and used as part of the
** documentation for the capdb_unlock_notify() API function. This
** should be considered if these functions are to be extended (i.e. to 
** support windows) in the future.
*/ 
#if CAPDB_OS_UNIX && defined(CAPDB_ENABLE_UNLOCK_NOTIFY)

/* BEGIN_CAPDB_BLOCKING_STEP */
/* This example uses the pthreads API */
#include <pthread.h>

/*
** A pointer to an instance of this structure is passed as the user-context
** pointer when registering for an unlock-notify callback.
*/
typedef struct UnlockNotification UnlockNotification;
struct UnlockNotification {
  int fired;                         /* True after unlock event has occurred */
  pthread_cond_t cond;               /* Condition variable to wait on */
  pthread_mutex_t mutex;             /* Mutex to protect structure */
};

/*
** This function is an unlock-notify callback registered with SQLite.
*/
static void unlock_notify_cb(void **apArg, int nArg){
  int i;
  for(i=0; i<nArg; i++){
    UnlockNotification *p = (UnlockNotification *)apArg[i];
    pthread_mutex_lock(&p->mutex);
    p->fired = 1;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
  }
}

/*
** This function assumes that an SQLite API call (either capdb_prepare_v2() 
** or capdb_step()) has just returned CAPDB_LOCKED. The argument is the
** associated database connection.
**
** This function calls capdb_unlock_notify() to register for an 
** unlock-notify callback, then blocks until that callback is delivered 
** and returns CAPDB_OK. The caller should then retry the failed operation.
**
** Or, if capdb_unlock_notify() indicates that to block would deadlock 
** the system, then this function returns CAPDB_LOCKED immediately. In 
** this case the caller should not retry the operation and should roll 
** back the current transaction (if any).
*/
static int wait_for_unlock_notify(capdb *db){
  int rc;
  UnlockNotification un;

  /* Initialize the UnlockNotification structure. */
  un.fired = 0;
  pthread_mutex_init(&un.mutex, 0);
  pthread_cond_init(&un.cond, 0);

  /* Register for an unlock-notify callback. */
  rc = capdb_unlock_notify(db, unlock_notify_cb, (void *)&un);
  assert( rc==CAPDB_LOCKED || rc==CAPDB_OK );

  /* The call to capdb_unlock_notify() always returns either CAPDB_LOCKED 
  ** or CAPDB_OK. 
  **
  ** If CAPDB_LOCKED was returned, then the system is deadlocked. In this
  ** case this function needs to return CAPDB_LOCKED to the caller so 
  ** that the current transaction can be rolled back. Otherwise, block
  ** until the unlock-notify callback is invoked, then return CAPDB_OK.
  */
  if( rc==CAPDB_OK ){
    pthread_mutex_lock(&un.mutex);
    if( !un.fired ){
      pthread_cond_wait(&un.cond, &un.mutex);
    }
    pthread_mutex_unlock(&un.mutex);
  }

  /* Destroy the mutex and condition variables. */
  pthread_cond_destroy(&un.cond);
  pthread_mutex_destroy(&un.mutex);

  return rc;
}

/*
** This function is a wrapper around the SQLite function capdb_step().
** It functions in the same way as step(), except that if a required
** shared-cache lock cannot be obtained, this function may block waiting for
** the lock to become available. In this scenario the normal API step()
** function always returns CAPDB_LOCKED.
**
** If this function returns CAPDB_LOCKED, the caller should rollback
** the current transaction (if any) and try again later. Otherwise, the
** system may become deadlocked.
*/
int capdb_blocking_step(capdb_stmt *pStmt){
  int rc;
  while( CAPDB_LOCKED==(rc = capdb_step(pStmt)) ){
    rc = wait_for_unlock_notify(capdb_db_handle(pStmt));
    if( rc!=CAPDB_OK ) break;
    capdb_reset(pStmt);
  }
  return rc;
}

/*
** This function is a wrapper around the SQLite function capdb_prepare_v2().
** It functions in the same way as prepare_v2(), except that if a required
** shared-cache lock cannot be obtained, this function may block waiting for
** the lock to become available. In this scenario the normal API prepare_v2()
** function always returns CAPDB_LOCKED.
**
** If this function returns CAPDB_LOCKED, the caller should rollback
** the current transaction (if any) and try again later. Otherwise, the
** system may become deadlocked.
*/
int capdb_blocking_prepare_v2(
  capdb *db,              /* Database handle. */
  const char *zSql,         /* UTF-8 encoded SQL statement. */
  int nSql,                 /* Length of zSql in bytes. */
  capdb_stmt **ppStmt,    /* OUT: A pointer to the prepared statement */
  const char **pz           /* OUT: End of parsed string */
){
  int rc;
  while( CAPDB_LOCKED==(rc = capdb_prepare_v2(db, zSql, nSql, ppStmt, pz)) ){
    rc = wait_for_unlock_notify(db);
    if( rc!=CAPDB_OK ) break;
  }
  return rc;
}
/* END_CAPDB_BLOCKING_STEP */

/*
** Usage: capdb_blocking_step STMT
**
** Advance the statement to the next row.
*/
static int CAPDB_TCLAPI blocking_step_proc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){

  capdb_stmt *pStmt;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "STMT");
    return TCL_ERROR;
  }

  pStmt = (capdb_stmt*)capdbTestTextToPtr(Tcl_GetString(objv[1]));
  rc = capdb_blocking_step(pStmt);

  Tcl_SetResult(interp, (char *)capdbErrName(rc), 0);
  return TCL_OK;
}

/*
** Usage: capdb_blocking_prepare_v2 DB sql bytes ?tailvar?
** Usage: capdb_nonblocking_prepare_v2 DB sql bytes ?tailvar?
*/
static int CAPDB_TCLAPI blocking_prepare_v2_proc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;
  const char *zSql;
  int bytes;
  const char *zTail = 0;
  capdb_stmt *pStmt = 0;
  char zBuf[50];
  int rc;
  int isBlocking = !(clientData==0);

  if( objc!=5 && objc!=4 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", 
       Tcl_GetString(objv[0]), " DB sql bytes tailvar", 0);
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  zSql = Tcl_GetString(objv[2]);
  if( Tcl_GetIntFromObj(interp, objv[3], &bytes) ) return TCL_ERROR;

  if( isBlocking ){
    rc = capdb_blocking_prepare_v2(db, zSql, bytes, &pStmt, &zTail);
  }else{
    rc = capdb_prepare_v2(db, zSql, bytes, &pStmt, &zTail);
  }

  assert(rc==CAPDB_OK || pStmt==0);
  if( zTail && objc>=5 ){
    if( bytes>=0 ){
      bytes = bytes - (zTail-zSql);
    }
    Tcl_ObjSetVar2(interp, objv[4], 0, Tcl_NewStringObj(zTail, bytes), 0);
  }
  if( rc!=CAPDB_OK ){
    assert( pStmt==0 );
    capdb_snprintf(sizeof(zBuf), zBuf, "%s ", (char *)capdbErrName(rc));
    Tcl_AppendResult(interp, zBuf, capdb_errmsg(db), NULL);
    return TCL_ERROR;
  }

  if( pStmt ){
    if( capdbTestMakePointerStr(interp, zBuf, pStmt) ) return TCL_ERROR;
    Tcl_AppendResult(interp, zBuf, NULL);
  }
  return TCL_OK;
}

#endif /* CAPDB_OS_UNIX && CAPDB_ENABLE_UNLOCK_NOTIFY */
/*
** End of implementation of [capdb_blocking_step].
************************************************************************/

/*
** Register commands with the TCL interpreter.
*/
int SqlitetestThread_Init(Tcl_Interp *interp){
  struct TclCmd {
    int (*xProc)(void*, Tcl_Interp*, int, Tcl_Obj*const*);
    const char *zName;
    int iCtx;
  } aCmd[] = {
    { sqlthread_proc,           "sqlthread",                      0 },
    { clock_seconds_proc,       "clock_second",                   0 },
    { clock_milliseconds_proc,  "clock_milliseconds",             0 },
#if CAPDB_OS_UNIX && defined(CAPDB_ENABLE_UNLOCK_NOTIFY)
    { blocking_step_proc,       "capdb_blocking_step",          0 },
    { blocking_prepare_v2_proc, "capdb_blocking_prepare_v2",    1 },
    { blocking_prepare_v2_proc, "capdb_nonblocking_prepare_v2", 0 },
#endif
  };
  int ii;

  for(ii=0; ii<sizeof(aCmd)/sizeof(aCmd[0]); ii++){
    void *p = CAPDB_INT_TO_PTR(aCmd[ii].iCtx);
    Tcl_CreateObjCommand(interp, aCmd[ii].zName, aCmd[ii].xProc, p, 0);
  }
  return TCL_OK;
}
#else
int SqlitetestThread_Init(Tcl_Interp *interp){
  return TCL_OK;
}
#endif
