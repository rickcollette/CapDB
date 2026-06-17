/*
** 2010 November 19
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Example code for obtaining an exclusive lock on an SQLite database
** file. This method is complicated, but works for both WAL and rollback
** mode database files. The interface to the example code in this file 
** consists of the following two functions:
**
**   capdbdemo_superlock()
**   capdbdemo_superunlock()
*/

#include "capdb.h"
#include <string.h>               /* memset(), strlen() */
#include <assert.h>               /* assert() */

/*
** A structure to collect a busy-handler callback and argument and a count
** of the number of times it has been invoked.
*/
struct SuperlockBusy {
  int (*xBusy)(void*,int);        /* Pointer to busy-handler function */
  void *pBusyArg;                 /* First arg to pass to xBusy */
  int nBusy;                      /* Number of times xBusy has been invoked */
};
typedef struct SuperlockBusy SuperlockBusy;

/*
** An instance of the following structure is allocated for each active
** superlock. The opaque handle returned by capdbdemo_superlock() is
** actually a pointer to an instance of this structure.
*/
struct Superlock {
  capdb *db;                    /* Database handle used to lock db */
  int bWal;                       /* True if db is a WAL database */
};
typedef struct Superlock Superlock;

/*
** The pCtx pointer passed to this function is actually a pointer to a
** SuperlockBusy structure. Invoke the busy-handler function encapsulated
** by the structure and return the result.
*/
static int superlockBusyHandler(void *pCtx, int UNUSED){
  SuperlockBusy *pBusy = (SuperlockBusy *)pCtx;
  if( pBusy->xBusy==0 ) return 0;
  return pBusy->xBusy(pBusy->pBusyArg, pBusy->nBusy++);
}

/*
** This function is used to determine if the main database file for 
** connection db is open in WAL mode or not. If no error occurs and the
** database file is in WAL mode, set *pbWal to true and return CAPDB_OK.
** If it is not in WAL mode, set *pbWal to false.
**
** If an error occurs, return an SQLite error code. The value of *pbWal
** is undefined in this case.
*/
static int superlockIsWal(Superlock *pLock){
  int rc;                         /* Return Code */
  capdb_stmt *pStmt;            /* Compiled PRAGMA journal_mode statement */

  rc = capdb_prepare(pLock->db, "PRAGMA main.journal_mode", -1, &pStmt, 0);
  if( rc!=CAPDB_OK ) return rc;

  pLock->bWal = 0;
  if( CAPDB_ROW==capdb_step(pStmt) ){
    const char *zMode = (const char *)capdb_column_text(pStmt, 0);
    if( zMode && strlen(zMode)==3 && capdb_strnicmp("wal", zMode, 3)==0 ){
      pLock->bWal = 1;
    }
  }

  return capdb_finalize(pStmt);
}

/*
** Obtain an exclusive shm-lock on nByte bytes starting at offset idx
** of the file fd. If the lock cannot be obtained immediately, invoke
** the busy-handler until either it is obtained or the busy-handler
** callback returns 0.
*/
static int superlockShmLock(
  capdb_file *fd,               /* Database file handle */
  int idx,                        /* Offset of shm-lock to obtain */
  int nByte,                      /* Number of consective bytes to lock */
  SuperlockBusy *pBusy            /* Busy-handler wrapper object */
){
  int rc;
  int (*xShmLock)(capdb_file*, int, int, int) = fd->pMethods->xShmLock;
  do {
    rc = xShmLock(fd, idx, nByte, CAPDB_SHM_LOCK|CAPDB_SHM_EXCLUSIVE);
  }while( rc==CAPDB_BUSY && superlockBusyHandler((void *)pBusy, 0) );
  return rc;
}

/*
** Obtain the extra locks on the database file required for WAL databases.
** Invoke the supplied busy-handler as required.
*/
static int superlockWalLock(
  capdb *db,                    /* Database handle open on WAL database */
  SuperlockBusy *pBusy            /* Busy handler wrapper object */
){
  int rc;                         /* Return code */
  capdb_file *fd = 0;           /* Main database file handle */
  void volatile *p = 0;           /* Pointer to first page of shared memory */

  /* Obtain a pointer to the capdb_file object open on the main db file. */
  rc = capdb_file_control(db, "main", CAPDB_FCNTL_FILE_POINTER, (void *)&fd);
  if( rc!=CAPDB_OK ) return rc;

  /* Obtain the "recovery" lock. Normally, this lock is only obtained by
  ** clients running database recovery.  
  */
  rc = superlockShmLock(fd, 2, 1, pBusy);
  if( rc!=CAPDB_OK ) return rc;

  /* Zero the start of the first shared-memory page. This means that any
  ** clients that open read or write transactions from this point on will
  ** have to run recovery before proceeding. Since they need the "recovery"
  ** lock that this process is holding to do that, no new read or write
  ** transactions may now be opened. Nor can a checkpoint be run, for the
  ** same reason.
  */
  rc = fd->pMethods->xShmMap(fd, 0, 32*1024, 1, &p);
  if( rc!=CAPDB_OK ) return rc;
  memset((void *)p, 0, 32);

  /* Obtain exclusive locks on all the "read-lock" slots. Once these locks
  ** are held, it is guaranteed that there are no active reader, writer or 
  ** checkpointer clients.
  */
  rc = superlockShmLock(fd, 3, CAPDB_SHM_NLOCK-3, pBusy);
  return rc;
}

/*
** Release a superlock held on a database file. The argument passed to 
** this function must have been obtained from a successful call to
** capdbdemo_superlock().
*/
void capdbdemo_superunlock(void *pLock){
  Superlock *p = (Superlock *)pLock;
  if( p->bWal ){
    int rc;                         /* Return code */
    int flags = CAPDB_SHM_UNLOCK | CAPDB_SHM_EXCLUSIVE;
    capdb_file *fd = 0;
    rc = capdb_file_control(p->db, "main", CAPDB_FCNTL_FILE_POINTER, (void *)&fd);
    if( rc==CAPDB_OK ){
      fd->pMethods->xShmLock(fd, 2, 1, flags);
      fd->pMethods->xShmLock(fd, 3, CAPDB_SHM_NLOCK-3, flags);
    }
  }
  capdb_close(p->db);
  capdb_free(p);
}

/*
** Obtain a superlock on the database file identified by zPath, using the
** locking primitives provided by VFS zVfs. If successful, CAPDB_OK is
** returned and output variable *ppLock is populated with an opaque handle
** that may be used with capdbdemo_superunlock() to release the lock.
**
** If an error occurs, *ppLock is set to 0 and an SQLite error code 
** (e.g. CAPDB_BUSY) is returned.
**
** If a required lock cannot be obtained immediately and the xBusy parameter
** to this function is not NULL, then xBusy is invoked in the same way
** as a busy-handler registered with SQLite (using capdb_busy_handler())
** until either the lock can be obtained or the busy-handler function returns
** 0 (indicating "give up").
*/
int capdbdemo_superlock(
  const char *zPath,              /* Path to database file to lock */
  const char *zVfs,               /* VFS to use to access database file */
  int (*xBusy)(void*,int),        /* Busy handler callback */
  void *pBusyArg,                 /* Context arg for busy handler */
  void **ppLock                   /* OUT: Context to pass to superunlock() */
){
  SuperlockBusy busy = {0, 0, 0}; /* Busy handler wrapper object */
  int rc;                         /* Return code */
  Superlock *pLock;

  pLock = capdb_malloc(sizeof(Superlock));
  if( !pLock ) return CAPDB_NOMEM;
  memset(pLock, 0, sizeof(Superlock));

  /* Open a database handle on the file to superlock. */
  rc = capdb_open_v2(
      zPath, &pLock->db, CAPDB_OPEN_READWRITE|CAPDB_OPEN_CREATE, zVfs
  );

  /* Install a busy-handler and execute a BEGIN EXCLUSIVE. If this is not
  ** a WAL database, this is all we need to do.  
  **
  ** A wrapper function is used to invoke the busy-handler instead of
  ** registering the busy-handler function supplied by the user directly
  ** with SQLite. This is because the same busy-handler function may be
  ** invoked directly later on when attempting to obtain the extra locks
  ** required in WAL mode. By using the wrapper, we are able to guarantee
  ** that the "nBusy" integer parameter passed to the users busy-handler
  ** represents the total number of busy-handler invocations made within
  ** this call to capdbdemo_superlock(), including any made during the
  ** "BEGIN EXCLUSIVE".
  */
  if( rc==CAPDB_OK ){
    busy.xBusy = xBusy;
    busy.pBusyArg = pBusyArg;
    capdb_busy_handler(pLock->db, superlockBusyHandler, (void *)&busy);
    rc = capdb_exec(pLock->db, "BEGIN EXCLUSIVE", 0, 0, 0);
  }

  /* If the BEGIN EXCLUSIVE was executed successfully and this is a WAL
  ** database, call superlockWalLock() to obtain the extra locks required
  ** to prevent readers, writers and/or checkpointers from accessing the
  ** db while this process is holding the superlock.
  **
  ** Before attempting any WAL locks, commit the transaction started above
  ** to drop the WAL read and write locks currently held. Otherwise, the
  ** new WAL locks may conflict with the old.
  */
  if( rc==CAPDB_OK ){
    if( CAPDB_OK==(rc = superlockIsWal(pLock)) && pLock->bWal ){
      rc = capdb_exec(pLock->db, "COMMIT", 0, 0, 0);
      if( rc==CAPDB_OK ){
        rc = superlockWalLock(pLock->db, &busy);
      }
    }
  }

  if( rc!=CAPDB_OK ){
    capdbdemo_superunlock(pLock);
    *ppLock = 0;
  }else{
    *ppLock = pLock;
  }

  return rc;
}

/*
** End of example code. Everything below here is the test harness.
**************************************************************************
**************************************************************************
*************************************************************************/


#ifdef CAPDB_TEST

#include "tclsqlite.h"

struct InterpAndScript {
  Tcl_Interp *interp;
  Tcl_Obj *pScript;
};
typedef struct InterpAndScript InterpAndScript;

static void CAPDB_TCLAPI superunlock_del(ClientData cd){
  capdbdemo_superunlock((void *)cd);
}

static int CAPDB_TCLAPI superunlock_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
  Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
  return TCL_OK;
}

static int superlock_busy(void *pCtx, int nBusy){
  InterpAndScript *p = (InterpAndScript *)pCtx;
  Tcl_Obj *pEval;                 /* Script to evaluate */
  int iVal = 0;                   /* Value to return */

  pEval = Tcl_DuplicateObj(p->pScript);
  Tcl_IncrRefCount(pEval);
  Tcl_ListObjAppendElement(p->interp, pEval, Tcl_NewIntObj(nBusy));
  Tcl_EvalObjEx(p->interp, pEval, TCL_EVAL_GLOBAL);
  Tcl_GetIntFromObj(p->interp, Tcl_GetObjResult(p->interp), &iVal);
  Tcl_DecrRefCount(pEval);

  return iVal;
}

/*
** Tclcmd: capdbdemo_superlock CMDNAME PATH VFS BUSY-HANDLER-SCRIPT
*/
static int CAPDB_TCLAPI superlock_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *pLock;                    /* Lock context */
  char *zPath;
  char *zVfs = 0;
  InterpAndScript busy = {0, 0};
  int (*xBusy)(void*,int) = 0;    /* Busy handler callback */
  int rc;                         /* Return code from capdbdemo_superlock() */

  if( objc<3 || objc>5 ){
    Tcl_WrongNumArgs(
        interp, 1, objv, "CMDNAME PATH ?VFS? ?BUSY-HANDLER-SCRIPT?");
    return TCL_ERROR;
  }

  zPath = Tcl_GetString(objv[2]);

  if( objc>3 ){
    zVfs = Tcl_GetString(objv[3]);
    if( strlen(zVfs)==0 ) zVfs = 0;
  }
  if( objc>4 ){
    busy.interp = interp;
    busy.pScript = objv[4];
    xBusy = superlock_busy;
  }

  rc = capdbdemo_superlock(zPath, zVfs, xBusy, &busy, &pLock);
  assert( rc==CAPDB_OK || pLock==0 );
  assert( rc!=CAPDB_OK || pLock!=0 );

  if( rc!=CAPDB_OK ){
    extern const char *capdbErrStr(int);
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, capdbErrStr(rc), NULL);
    return TCL_ERROR;
  }

  Tcl_CreateObjCommand(
      interp, Tcl_GetString(objv[1]), superunlock_cmd, pLock, superunlock_del
  );
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

int SqliteSuperlock_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "capdbdemo_superlock", superlock_cmd, 0, 0);
  return TCL_OK;
}
#endif
