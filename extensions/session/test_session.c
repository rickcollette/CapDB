
#if defined(CAPDB_TEST) && defined(CAPDB_ENABLE_SESSION) \
 && defined(CAPDB_ENABLE_PREUPDATE_HOOK)

#include "capdb_session.h"
#include <assert.h>
#include <string.h>
#include "tclsqlite.h"

#include <stdlib.h>

#ifndef CAPDB_AMALGAMATION
  typedef unsigned char u8;
#endif

extern const char *capdbErrName(int);

typedef struct TestSession TestSession;
struct TestSession {
  capdb_session *pSession;
  Tcl_Interp *interp;
  Tcl_Obj *pFilterScript;
};

typedef struct TestStreamInput TestStreamInput;
struct TestStreamInput {
  int nStream;                    /* Maximum chunk size */
  unsigned char *aData;           /* Pointer to buffer containing data */
  int nData;                      /* Size of buffer aData in bytes */
  int iData;                      /* Bytes of data already read by sessions */
};

/*
** Extract an capdb* db handle from the object passed as the second
** argument. If successful, set *pDb to point to the db handle and return
** TCL_OK. Otherwise, return TCL_ERROR.
*/
static int dbHandleFromObj(Tcl_Interp *interp, Tcl_Obj *pObj, capdb **pDb){
  Tcl_CmdInfo info;
  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(pObj), &info) ){
    Tcl_AppendResult(interp, "no such handle: ", Tcl_GetString(pObj), NULL);
    return TCL_ERROR;
  }

  *pDb = *(capdb **)info.objClientData;
  return TCL_OK;
}

/*************************************************************************
** The following code is copied byte-for-byte from the sessions module
** documentation.  It is used by some of the sessions modules tests to
** ensure that the example in the documentation does actually work.
*/ 
/*
** Argument zSql points to a buffer containing an SQL script to execute 
** against the database handle passed as the first argument. As well as
** executing the SQL script, this function collects a changeset recording
** all changes made to the "main" database file. Assuming no error occurs,
** output variables (*ppChangeset) and (*pnChangeset) are set to point
** to a buffer containing the changeset and the size of the changeset in
** bytes before returning CAPDB_OK. In this case it is the responsibility
** of the caller to eventually free the changeset blob by passing it to
** the capdb_free function.
**
** Or, if an error does occur, return an SQLite error code. The final
** value of (*pChangeset) and (*pnChangeset) are undefined in this case.
*/
int sql_exec_changeset(
  capdb *db,                  /* Database handle */
  const char *zSql,             /* SQL script to execute */
  int *pnChangeset,             /* OUT: Size of changeset blob in bytes */
  void **ppChangeset            /* OUT: Pointer to changeset blob */
){
  capdb_session *pSession = 0;
  int rc;
  int val = 1;

  /* Create a new session object */
  rc = capdb_session_create(db, "main", &pSession);
  capdb_session_object_config(pSession, CAPDB_SESSION_OBJCONFIG_ROWID, &val);

  /* Configure the session object to record changes to all tables */
  if( rc==CAPDB_OK ) rc = capdb_session_attach(pSession, NULL);

  /* Execute the SQL script */
  if( rc==CAPDB_OK ) rc = capdb_exec(db, zSql, 0, 0, 0);

  /* Collect the changeset */
  if( rc==CAPDB_OK ){
    rc = capdb_session_changeset(pSession, pnChangeset, ppChangeset);
  }

  /* Delete the session object */
  capdb_session_delete(pSession);

  return rc;
}
/************************************************************************/


#ifdef CAPDB_DEBUG
static int capdb_test_changeset(int, void *, char **);
static void assert_changeset_is_ok(int n, void *p){
  char *z = 0;
  (void)capdb_test_changeset(n, p, &z);
  assert( z==0 );
}
#else
# define assert_changeset_is_ok(n,p)
#endif

/*
** Tclcmd: sql_exec_changeset DB SQL
*/
static int CAPDB_TCLAPI test_sql_exec_changeset(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  const char *zSql;
  capdb *db;
  void *pChangeset;
  int nChangeset;
  int rc;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB SQL");
    return TCL_ERROR;
  }
  if( dbHandleFromObj(interp, objv[1], &db) ) return TCL_ERROR;
  zSql = (const char*)Tcl_GetString(objv[2]);

  rc = sql_exec_changeset(db, zSql, &nChangeset, &pChangeset);
  if( rc!=CAPDB_OK ){
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "error in sql_exec_changeset()", NULL);
    return TCL_ERROR;
  }

  assert_changeset_is_ok(nChangeset, pChangeset);
  Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(pChangeset, nChangeset));
  capdb_free(pChangeset);
  return TCL_OK;
}



#define SESSION_STREAM_TCL_VAR "capdb_session_streams"

/*
** Attempt to find the global variable zVar within interpreter interp
** and extract an integer value from it. Return this value.
**
** If the named variable cannot be found, or if it cannot be interpreted
** as a integer, return 0.
*/
static int test_tcl_integer(Tcl_Interp *interp, const char *zVar){
  Tcl_Obj *pObj;
  int iVal = 0;
  Tcl_Obj *pName = Tcl_NewStringObj(zVar, -1);
  Tcl_IncrRefCount(pName);
  pObj = Tcl_ObjGetVar2(interp, pName, 0, TCL_GLOBAL_ONLY);
  Tcl_DecrRefCount(pName);
  if( pObj ) Tcl_GetIntFromObj(0, pObj, &iVal);
  return iVal;
}

static int test_session_error(Tcl_Interp *interp, int rc, char *zErr){
  extern const char *capdbErrName(int);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(capdbErrName(rc), -1));
  if( zErr ){
    Tcl_AppendResult(interp, " - ", zErr, NULL);
    capdb_free(zErr);
  }
  return TCL_ERROR;
}

static int test_table_filter(void *pCtx, const char *zTbl){
  TestSession *p = (TestSession*)pCtx;
  Tcl_Obj *pEval;
  int rc;
  int bRes = 0;

  pEval = Tcl_DuplicateObj(p->pFilterScript);
  Tcl_IncrRefCount(pEval);
  rc = Tcl_ListObjAppendElement(p->interp, pEval, Tcl_NewStringObj(zTbl, -1));
  if( rc==TCL_OK ){
    rc = Tcl_EvalObjEx(p->interp, pEval, TCL_EVAL_GLOBAL);
  }
  if( rc==TCL_OK ){
    rc = Tcl_GetBooleanFromObj(p->interp, Tcl_GetObjResult(p->interp), &bRes);
  }
  if( rc!=TCL_OK ){
    /* printf("error: %s\n", Tcl_GetStringResult(p->interp)); */
    Tcl_BackgroundError(p->interp);
  }
  Tcl_DecrRefCount(pEval);

  return bRes;
}

struct TestSessionsBlob {
  void *p;
  int n;
};
typedef struct TestSessionsBlob TestSessionsBlob;

static int testStreamOutput(
  void *pCtx,
  const void *pData,
  int nData
){
  TestSessionsBlob *pBlob = (TestSessionsBlob*)pCtx;
  char *pNew;

  assert( nData>0 );
  pNew = (char*)capdb_realloc(pBlob->p, pBlob->n + nData);
  if( pNew==0 ){
    return CAPDB_NOMEM;
  }
  pBlob->p = (void*)pNew;
  memcpy(&pNew[pBlob->n], pData, nData);
  pBlob->n += nData;
  return CAPDB_OK;
}

/*
** Tclcmd:  $session attach TABLE
**          $session changeset
**          $session delete
**          $session enable BOOL
**          $session indirect INTEGER
**          $session patchset
**          $session table_filter SCRIPT
*/
static int CAPDB_TCLAPI test_session_cmd(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  TestSession *p = (TestSession*)clientData;
  capdb_session *pSession = p->pSession;
  static struct SessionSubcmd {
    const char *zSub;
    int nArg;
    const char *zMsg;
    int iSub;
  } aSub[] = {
    { "attach",       1, "TABLE",      }, /* 0 */
    { "changeset",    0, "",           }, /* 1 */
    { "delete",       0, "",           }, /* 2 */
    { "enable",       1, "BOOL",       }, /* 3 */
    { "indirect",     1, "BOOL",       }, /* 4 */
    { "isempty",      0, "",           }, /* 5 */
    { "table_filter", 1, "SCRIPT",     }, /* 6 */
    { "patchset",     0, "",           }, /* 7 */
    { "diff",         2, "FROMDB TBL", }, /* 8 */
    { "memory_used",  0, "",           }, /* 9 */
    { "changeset_size", 0, "",         }, /* 10 */
    { "object_config", 2, "OPTION INTEGER", }, /* 11 */
    { 0 }
  };
  int iSub;
  int rc;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[1], aSub, sizeof(aSub[0]), "sub-command", 0, &iSub
  );
  if( rc!=TCL_OK ) return rc;
  if( objc!=2+aSub[iSub].nArg ){
    Tcl_WrongNumArgs(interp, 2, objv, aSub[iSub].zMsg);
    return TCL_ERROR;
  }

  switch( iSub ){
    case 0: {      /* attach */
      char *zArg = Tcl_GetString(objv[2]);
      if( zArg[0]=='*' && zArg[1]=='\0' ) zArg = 0;
      rc = capdb_session_attach(pSession, zArg);
      if( rc!=CAPDB_OK ){
        return test_session_error(interp, rc, 0);
      }
      break;
    }

    case 7:        /* patchset */
    case 1: {      /* changeset */
      TestSessionsBlob o = {0, 0};
      if( test_tcl_integer(interp, SESSION_STREAM_TCL_VAR) ){
        void *pCtx = (void*)&o;
        if( iSub==7 ){
          rc = capdb_session_patchset_strm(pSession, testStreamOutput, pCtx);
        }else{
          rc = capdb_session_changeset_strm(pSession, testStreamOutput, pCtx);
        }
      }else{
        if( iSub==7 ){
          rc = capdb_session_patchset(pSession, &o.n, &o.p);
        }else{
          rc = capdb_session_changeset(pSession, &o.n, &o.p);
        }
      }
      if( rc==CAPDB_OK ){
        assert_changeset_is_ok(o.n, o.p);
        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(o.p, o.n)); 
      }
      capdb_free(o.p);
      if( rc!=CAPDB_OK ){
        return test_session_error(interp, rc, 0);
      }
      break;
    }

    case 2:        /* delete */
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;

    case 3: {      /* enable */
      int val;
      if( Tcl_GetIntFromObj(interp, objv[2], &val) ) return TCL_ERROR;
      val = capdb_session_enable(pSession, val);
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(val));
      break;
    }

    case 4: {      /* indirect */
      int val;
      if( Tcl_GetIntFromObj(interp, objv[2], &val) ) return TCL_ERROR;
      val = capdb_session_indirect(pSession, val);
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(val));
      break;
    }

    case 5: {      /* isempty */
      int val;
      val = capdb_session_isempty(pSession);
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(val));
      break;
    }
            
    case 6: {      /* table_filter */
      if( p->pFilterScript ) Tcl_DecrRefCount(p->pFilterScript);
      p->interp = interp;
      p->pFilterScript = Tcl_DuplicateObj(objv[2]);
      Tcl_IncrRefCount(p->pFilterScript);
      capdb_session_table_filter(pSession, test_table_filter, clientData);
      break;
    }

    case 8: {      /* diff */
      char *zErr = 0;
      rc = capdb_session_diff(pSession, 
          Tcl_GetString(objv[2]),
          Tcl_GetString(objv[3]),
          &zErr
      );
      assert( rc!=CAPDB_OK || zErr==0 );
      if( rc ){
        return test_session_error(interp, rc, zErr);
      }
      break;
    }

    case 9: {      /* memory_used */
      capdb_int64 nMalloc = capdb_session_memory_used(pSession);
      Tcl_SetObjResult(interp, Tcl_NewWideIntObj(nMalloc));
      break;
    }

    case 10: {
      capdb_int64 nSize = capdb_session_changeset_size(pSession);
      Tcl_SetObjResult(interp, Tcl_NewWideIntObj(nSize));
      break;
    }
    case 11: {    /* object_config */
      struct ObjConfOpt {
        const char *zName;
        int opt;
      } aOpt[] = {
        { "size", CAPDB_SESSION_OBJCONFIG_SIZE },
        { "rowid", CAPDB_SESSION_OBJCONFIG_ROWID },
        { 0, 0 }
      };
      int sz = (int)sizeof(aOpt[0]);

      int iArg;
      Tcl_Size iOpt;
      if( Tcl_GetIndexFromObjStruct(interp,objv[2],aOpt,sz,"option",0,&iOpt) ){
        return TCL_ERROR;
      }
      if( Tcl_GetIntFromObj(interp, objv[3], &iArg) ){
        return TCL_ERROR;
      }
      rc = capdb_session_object_config(pSession, aOpt[iOpt].opt, &iArg);
      if( rc!=CAPDB_OK ){
        Tcl_SetObjResult(interp, Tcl_NewStringObj(capdbErrName(rc), -1));
      }else{
        Tcl_SetObjResult(interp, Tcl_NewIntObj(iArg));
      }
      break;
    }
  }

  return TCL_OK;
}

static void CAPDB_TCLAPI test_session_del(void *clientData){
  TestSession *p = (TestSession*)clientData;
  if( p->pFilterScript ) Tcl_DecrRefCount(p->pFilterScript);
  capdb_session_delete(p->pSession);
  ckfree((char*)p);
}

/*
** Tclcmd:  capdb_session CMD DB-HANDLE DB-NAME
*/
static int CAPDB_TCLAPI test_capdb_session(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;
  Tcl_CmdInfo info;
  int rc;                         /* capdb_session_create() return code */
  TestSession *p;                 /* New wrapper object */
  int iArg = -1;

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "CMD DB-HANDLE DB-NAME");
    return TCL_ERROR;
  }

  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(objv[2]), &info) ){
    Tcl_AppendResult(interp, "no such handle: ", Tcl_GetString(objv[2]), NULL);
    return TCL_ERROR;
  }
  db = *(capdb **)info.objClientData;

  p = (TestSession*)ckalloc(sizeof(TestSession));
  memset(p, 0, sizeof(TestSession));
  rc = capdb_session_create(db, Tcl_GetString(objv[3]), &p->pSession);
  if( rc!=CAPDB_OK ){
    ckfree((char*)p);
    return test_session_error(interp, rc, 0);
  }

  /* Query the CAPDB_SESSION_OBJCONFIG_SIZE option to ensure that it
  ** is clear by default. Then set it. */
  capdb_session_object_config(p->pSession,CAPDB_SESSION_OBJCONFIG_SIZE,&iArg);
  assert( iArg==0 );
  iArg = 1;
  capdb_session_object_config(p->pSession,CAPDB_SESSION_OBJCONFIG_SIZE,&iArg);

  Tcl_CreateObjCommand(
      interp, Tcl_GetString(objv[1]), test_session_cmd, (ClientData)p,
      test_session_del
  );
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

static void test_append_value(Tcl_Obj *pList, capdb_value *pVal){
  if( pVal==0 ){
    Tcl_ListObjAppendElement(0, pList, Tcl_NewObj());
    Tcl_ListObjAppendElement(0, pList, Tcl_NewObj());
  }else{
    Tcl_Obj *pObj;
    switch( capdb_value_type(pVal) ){
      case CAPDB_NULL:
        Tcl_ListObjAppendElement(0, pList, Tcl_NewStringObj("n", 1));
        pObj = Tcl_NewObj();
        break;
      case CAPDB_INTEGER:
        Tcl_ListObjAppendElement(0, pList, Tcl_NewStringObj("i", 1));
        pObj = Tcl_NewWideIntObj(capdb_value_int64(pVal));
        break;
      case CAPDB_FLOAT:
        Tcl_ListObjAppendElement(0, pList, Tcl_NewStringObj("f", 1));
        pObj = Tcl_NewDoubleObj(capdb_value_double(pVal));
        break;
      case CAPDB_TEXT: {
        const char *z = (char*)capdb_value_blob(pVal);
        int n = capdb_value_bytes(pVal);
        Tcl_ListObjAppendElement(0, pList, Tcl_NewStringObj("t", 1));
        pObj = Tcl_NewStringObj(z, n);
        break;
      }
      default:
        assert( capdb_value_type(pVal)==CAPDB_BLOB );
        Tcl_ListObjAppendElement(0, pList, Tcl_NewStringObj("b", 1));
        pObj = Tcl_NewByteArrayObj(
            capdb_value_blob(pVal),
            capdb_value_bytes(pVal)
        );
        break;
    }
    Tcl_ListObjAppendElement(0, pList, pObj);
  }
}

typedef struct TestConflictHandler TestConflictHandler;
struct TestConflictHandler {
  Tcl_Interp *interp;
  Tcl_Obj *pConflictScript;
  Tcl_Obj *pFilterScript;
};

static int test_obj_eq_string(Tcl_Obj *p, const char *z){
  Tcl_Size n;
  Tcl_Size nObj;
  char *zObj;

  n = (Tcl_Size)strlen(z);
  zObj = Tcl_GetStringFromObj(p, &nObj);

  return (nObj==n && (n==0 || 0==memcmp(zObj, z, n)));
}

static Tcl_Obj *testIterData(capdb_changeset_iter *pIter){
  Tcl_Obj *pVar = 0;
  int nCol;                       /* Number of columns in table */
  int nCol2;                      /* Number of columns in table */
  int op;                         /* CAPDB_INSERT, UPDATE or DELETE */
  const char *zTab;               /* Name of table change applies to */
  Tcl_Obj *pOld;                  /* Vector of old.* values */
  Tcl_Obj *pNew;                  /* Vector of new.* values */
  int bIndirect;
    
  char *zPK;
  unsigned char *abPK;
  int i;

  capdb_changeset_op(pIter, &zTab, &nCol, &op, &bIndirect);
  pVar = Tcl_NewObj();

  Tcl_ListObjAppendElement(0, pVar, Tcl_NewStringObj(
        op==CAPDB_INSERT ? "INSERT" :
        op==CAPDB_UPDATE ? "UPDATE" : 
        "DELETE", -1
  ));

  Tcl_ListObjAppendElement(0, pVar, Tcl_NewStringObj(zTab, -1));
  Tcl_ListObjAppendElement(0, pVar, Tcl_NewBooleanObj(bIndirect));

  zPK = ckalloc(nCol+1);
  memset(zPK, 0, nCol+1);
  capdb_changeset_pk(pIter, &abPK, &nCol2);
  assert( nCol==nCol2 );
  for(i=0; i<nCol; i++){
    zPK[i] = (abPK[i] ? 'X' : '.');
  }
  Tcl_ListObjAppendElement(0, pVar, Tcl_NewStringObj(zPK, -1));
  ckfree(zPK);

  pOld = Tcl_NewObj();
  if( op!=CAPDB_INSERT ){
    for(i=0; i<nCol; i++){
      capdb_value *pVal;
      capdb_changeset_old(pIter, i, &pVal);
      test_append_value(pOld, pVal);
    }
  }
  pNew = Tcl_NewObj();
  if( op!=CAPDB_DELETE ){
    for(i=0; i<nCol; i++){
      capdb_value *pVal;
      capdb_changeset_new(pIter, i, &pVal);
      test_append_value(pNew, pVal);
    }
  }
  Tcl_ListObjAppendElement(0, pVar, pOld);
  Tcl_ListObjAppendElement(0, pVar, pNew);

  return pVar;
}


static int test_filter_handler(
  void *pCtx,                     /* Pointer to TestConflictHandler structure */
  const char *zTab                /* Table name */
){
  TestConflictHandler *p = (TestConflictHandler *)pCtx;
  int res = 1;
  Tcl_Obj *pEval;
  Tcl_Interp *interp = p->interp;

  pEval = Tcl_DuplicateObj(p->pFilterScript);
  Tcl_IncrRefCount(pEval);

  if( TCL_OK!=Tcl_ListObjAppendElement(0, pEval, Tcl_NewStringObj(zTab, -1))
   || TCL_OK!=Tcl_EvalObjEx(interp, pEval, TCL_EVAL_GLOBAL) 
   || TCL_OK!=Tcl_GetIntFromObj(interp, Tcl_GetObjResult(interp), &res)
  ){
    Tcl_BackgroundError(interp);
  }

  Tcl_DecrRefCount(pEval);
  return res;
}  

static int test_filter_v3_handler(
  void *pCtx,                     /* Pointer to TestConflictHandler structure */
  capdb_changeset_iter *pIter
){
  TestConflictHandler *p = (TestConflictHandler *)pCtx;
  int res = 1;
  Tcl_Obj *pEval = 0;
  Tcl_Interp *interp = p->interp;

  pEval = Tcl_DuplicateObj(p->pFilterScript);
  Tcl_IncrRefCount(pEval);
  Tcl_ListObjAppendElement(0, pEval, testIterData(pIter));

  if( TCL_OK!=Tcl_EvalObjEx(interp, pEval, TCL_EVAL_GLOBAL) 
   || TCL_OK!=Tcl_GetIntFromObj(interp, Tcl_GetObjResult(interp), &res)
  ){
    Tcl_BackgroundError(interp);
  }

  Tcl_DecrRefCount(pEval);
  return res;
}  

static int test_conflict_handler(
  void *pCtx,                     /* Pointer to TestConflictHandler structure */
  int eConf,                      /* DATA, MISSING, CONFLICT, CONSTRAINT */
  capdb_changeset_iter *pIter   /* Handle describing change and conflict */
){
  TestConflictHandler *p = (TestConflictHandler *)pCtx;
  Tcl_Obj *pEval;
  Tcl_Interp *interp = p->interp;
  int ret = 0;                    /* Return value */

  int op;                         /* CAPDB_UPDATE, DELETE or INSERT */
  const char *zTab;               /* Name of table conflict is on */
  int nCol;                       /* Number of columns in table zTab */

  pEval = Tcl_DuplicateObj(p->pConflictScript);
  Tcl_IncrRefCount(pEval);

  capdb_changeset_op(pIter, &zTab, &nCol, &op, 0);

  if( eConf==CAPDB_CHANGESET_FOREIGN_KEY ){
    int nFk;
    capdb_changeset_fk_conflicts(pIter, &nFk);
    Tcl_ListObjAppendElement(0, pEval, Tcl_NewStringObj("FOREIGN_KEY", -1));
    Tcl_ListObjAppendElement(0, pEval, Tcl_NewIntObj(nFk));
  }else{

    /* Append the operation type. */
    Tcl_ListObjAppendElement(0, pEval, Tcl_NewStringObj(
        op==CAPDB_INSERT ? "INSERT" :
        op==CAPDB_UPDATE ? "UPDATE" : 
        "DELETE", -1
    ));
  
    /* Append the table name. */
    Tcl_ListObjAppendElement(0, pEval, Tcl_NewStringObj(zTab, -1));
  
    /* Append the conflict type. */
    switch( eConf ){
      case CAPDB_CHANGESET_DATA:
        Tcl_ListObjAppendElement(interp, pEval,Tcl_NewStringObj("DATA",-1));
        break;
      case CAPDB_CHANGESET_NOTFOUND:
        Tcl_ListObjAppendElement(interp, pEval,Tcl_NewStringObj("NOTFOUND",-1));
        break;
      case CAPDB_CHANGESET_CONFLICT:
        Tcl_ListObjAppendElement(interp, pEval,Tcl_NewStringObj("CONFLICT",-1));
        break;
      case CAPDB_CHANGESET_CONSTRAINT:
        Tcl_ListObjAppendElement(interp, pEval,Tcl_NewStringObj("CONSTRAINT",-1));
        break;
    }
  
    /* If this is not an INSERT, append the old row */
    if( op!=CAPDB_INSERT ){
      int i;
      Tcl_Obj *pOld = Tcl_NewObj();
      for(i=0; i<nCol; i++){
        capdb_value *pVal;
        capdb_changeset_old(pIter, i, &pVal);
        test_append_value(pOld, pVal);
      }
      Tcl_ListObjAppendElement(0, pEval, pOld);
    }

    /* If this is not a DELETE, append the new row */
    if( op!=CAPDB_DELETE ){
      int i;
      Tcl_Obj *pNew = Tcl_NewObj();
      for(i=0; i<nCol; i++){
        capdb_value *pVal;
        capdb_changeset_new(pIter, i, &pVal);
        test_append_value(pNew, pVal);
      }
      Tcl_ListObjAppendElement(0, pEval, pNew);
    }

    /* If this is a CHANGESET_DATA or CHANGESET_CONFLICT conflict, append
     ** the conflicting row.  */
    if( eConf==CAPDB_CHANGESET_DATA || eConf==CAPDB_CHANGESET_CONFLICT ){
      int i;
      Tcl_Obj *pConflict = Tcl_NewObj();
      for(i=0; i<nCol; i++){
        int rc;
        capdb_value *pVal;
        rc = capdb_changeset_conflict(pIter, i, &pVal);
        assert( rc==CAPDB_OK );
        test_append_value(pConflict, pVal);
      }
      Tcl_ListObjAppendElement(0, pEval, pConflict);
    }

    /***********************************************************************
     ** This block is purely for testing some error conditions.
     */
    if( eConf==CAPDB_CHANGESET_CONSTRAINT 
     || eConf==CAPDB_CHANGESET_NOTFOUND 
    ){
      capdb_value *pVal;
      int rc = capdb_changeset_conflict(pIter, 0, &pVal);
      assert( rc==CAPDB_MISUSE );
    }else{
      capdb_value *pVal;
      int rc = capdb_changeset_conflict(pIter, -1, &pVal);
      assert( rc==CAPDB_RANGE );
      rc = capdb_changeset_conflict(pIter, nCol, &pVal);
      assert( rc==CAPDB_RANGE );
    }
    if( op==CAPDB_DELETE ){
      capdb_value *pVal;
      int rc = capdb_changeset_new(pIter, 0, &pVal);
      assert( rc==CAPDB_MISUSE );
    }else{
      capdb_value *pVal;
      int rc = capdb_changeset_new(pIter, -1, &pVal);
      assert( rc==CAPDB_RANGE );
      rc = capdb_changeset_new(pIter, nCol, &pVal);
      assert( rc==CAPDB_RANGE );
    }
    if( op==CAPDB_INSERT ){
      capdb_value *pVal;
      int rc = capdb_changeset_old(pIter, 0, &pVal);
      assert( rc==CAPDB_MISUSE );
    }else{
      capdb_value *pVal;
      int rc = capdb_changeset_old(pIter, -1, &pVal);
      assert( rc==CAPDB_RANGE );
      rc = capdb_changeset_old(pIter, nCol, &pVal);
      assert( rc==CAPDB_RANGE );
    }
    if( eConf!=CAPDB_CHANGESET_FOREIGN_KEY ){
      /* eConf!=FOREIGN_KEY is always true at this point. The condition is 
      ** just there to make it clearer what is being tested.  */
      int nDummy;
      int rc = capdb_changeset_fk_conflicts(pIter, &nDummy);
      assert( rc==CAPDB_MISUSE );
    }
    /* End of testing block
    ***********************************************************************/
  }

  if( TCL_OK!=Tcl_EvalObjEx(interp, pEval, TCL_EVAL_GLOBAL) ){
    Tcl_BackgroundError(interp);
  }else{
    Tcl_Obj *pRes = Tcl_GetObjResult(interp);
    if( test_obj_eq_string(pRes, "OMIT") || test_obj_eq_string(pRes, "") ){
      ret = CAPDB_CHANGESET_OMIT;
    }else if( test_obj_eq_string(pRes, "REPLACE") ){
      ret = CAPDB_CHANGESET_REPLACE;
    }else if( test_obj_eq_string(pRes, "ABORT") ){
      ret = CAPDB_CHANGESET_ABORT;
    }else{
      Tcl_GetIntFromObj(0, pRes, &ret);
    }
  }

  Tcl_DecrRefCount(pEval);
  return ret;
}

/*
** The conflict handler used by capdb_changeset_apply_replace_all(). 
** This conflict handler calls capdb_value_text16() on all available
** capdb_value objects and then returns CHANGESET_REPLACE, or 
** CHANGESET_OMIT if REPLACE is not applicable. This is used to test the
** effect of a malloc failure within an capdb_value_xxx() function
** invoked by a conflict-handler callback.
*/
static int replace_handler(
  void *pCtx,                     /* Pointer to TestConflictHandler structure */
  int eConf,                      /* DATA, MISSING, CONFLICT, CONSTRAINT */
  capdb_changeset_iter *pIter   /* Handle describing change and conflict */
){
  int op;                         /* CAPDB_UPDATE, DELETE or INSERT */
  const char *zTab;               /* Name of table conflict is on */
  int nCol;                       /* Number of columns in table zTab */
  int i;

  capdb_changeset_op(pIter, &zTab, &nCol, &op, 0);

  if( op!=CAPDB_INSERT ){
    for(i=0; i<nCol; i++){
      capdb_value *pVal;
      capdb_changeset_old(pIter, i, &pVal);
      capdb_value_text16(pVal);
    }
  }

  if( op!=CAPDB_DELETE ){
    for(i=0; i<nCol; i++){
      capdb_value *pVal;
      capdb_changeset_new(pIter, i, &pVal);
      capdb_value_text16(pVal);
    }
  }

  if( eConf==CAPDB_CHANGESET_DATA ){
    return CAPDB_CHANGESET_REPLACE;
  }
  return CAPDB_CHANGESET_OMIT;
}

static int testStreamInput(
  void *pCtx,                     /* Context pointer */
  void *pData,                    /* Buffer to populate */
  int *pnData                     /* IN/OUT: Bytes requested/supplied */
){
  TestStreamInput *p = (TestStreamInput*)pCtx;
  int nReq = *pnData;             /* Bytes of data requested */
  int nRem = p->nData - p->iData; /* Bytes of data available */
  int nRet = p->nStream;          /* Bytes actually returned */

  /* Allocate and free some space. There is no point to this, other than
  ** that it allows the regular OOM fault-injection tests to cause an error
  ** in this function.  */
  void *pAlloc = capdb_malloc(10);
  if( pAlloc==0 ) return CAPDB_NOMEM;
  capdb_free(pAlloc);

  if( nRet>nReq ) nRet = nReq;
  if( nRet>nRem ) nRet = nRem;

  assert( nRet>=0 );
  if( nRet>0 ){
    memcpy(pData, &p->aData[p->iData], nRet);
    p->iData += nRet;
  }

  *pnData = nRet;
  return CAPDB_OK;
}

/*
** This works like Tcl_GetByteArrayFromObj(), except that it returns a buffer
** allocated using malloc() that must be freed by the caller. This is done
** because Tcl's buffers are often padded by a few bytes, which prevents
** small overreads from being detected when tests are run under asan.
*/
static void *testGetByteArrayFromObj(Tcl_Obj *p, Tcl_Size *pnByte){
  Tcl_Size nByte = 0;
  void *aByte = Tcl_GetByteArrayFromObj(p, &nByte);
  void *aCopy = malloc(nByte ? (size_t)nByte : 1);
  memcpy(aCopy, aByte, (size_t)nByte);
  *pnByte = nByte;
  return aCopy;
}


static int CAPDB_TCLAPI testSqlite3changesetApply(
  int iVersion,
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;                    /* Database handle */
  Tcl_CmdInfo info;               /* Database Tcl command (objv[1]) info */
  int rc;                         /* Return code from changeset_invert() */
  void *pChangeset;               /* Buffer containing changeset */
  Tcl_Size nChangeset;            /* Size of buffer aChangeset in bytes */
  TestConflictHandler ctx;
  TestStreamInput sStr;
  void *pRebase = 0;
  int nRebase = 0;
  int flags = 0;                  /* Flags for apply_v2() */

  assert( iVersion==1 || iVersion==2 || iVersion==3 );

  memset(&sStr, 0, sizeof(sStr));
  sStr.nStream = test_tcl_integer(interp, SESSION_STREAM_TCL_VAR);

  /* Check for the -nosavepoint, -invert or -ignorenoop switches */
  if( iVersion==2 || iVersion==3 ){
    while( objc>1 ){
      const char *z1 = Tcl_GetString(objv[1]);
      int n = (int)strlen(z1);
      if( n>3 && n<=12 && 0==capdb_strnicmp("-nosavepoint", z1, n) ){
        flags |= CAPDB_CHANGESETAPPLY_NOSAVEPOINT;
      }
      else if( n>3 && n<=9 && 0==capdb_strnicmp("-noaction", z1, n) ){
        flags |= CAPDB_CHANGESETAPPLY_FKNOACTION;
      }
      else if( n>2 && n<=7 && 0==capdb_strnicmp("-invert", z1, n) ){
        flags |= CAPDB_CHANGESETAPPLY_INVERT;
      }
      else if( n>2 && n<=11 && 0==capdb_strnicmp("-ignorenoop", z1, n) ){
        flags |= CAPDB_CHANGESETAPPLY_IGNORENOOP;
      }
      else if( n>3 && n<=13 && 0==capdb_strnicmp("-noupdateloop", z1, n) ){
        flags |= CAPDB_CHANGESETAPPLY_NOUPDATELOOP;
      }else{
        break;
      }
      objc--;
      objv++;
    }
  }

  if( objc!=4 && objc!=5 ){
    const char *zMsg;
    if( iVersion==2 || iVersion==3  ){
      zMsg = "?-nosavepoint? ?-inverse? ?-ignorenoop? "
        "DB CHANGESET CONFLICT-SCRIPT ?FILTER-SCRIPT?";
    }else{
      zMsg = "DB CHANGESET CONFLICT-SCRIPT ?FILTER-SCRIPT?";
    }
    Tcl_WrongNumArgs(interp, 1, objv, zMsg);
    return TCL_ERROR;
  }
  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(objv[1]), &info) ){
    Tcl_AppendResult(interp, "no such handle: ", Tcl_GetString(objv[1]), NULL);
    return TCL_ERROR;
  }
  db = *(capdb **)info.objClientData;
  pChangeset = (void *)testGetByteArrayFromObj(objv[2], &nChangeset);
  ctx.pConflictScript = objv[3];
  ctx.pFilterScript = objc==5 ? objv[4] : 0;
  ctx.interp = interp;

  if( sStr.nStream==0 ){
    switch( iVersion ){
      case 1:
        rc = capdb_changeset_apply(db, (int)nChangeset, pChangeset, 
            (objc==5)?test_filter_handler:0, test_conflict_handler, (void*)&ctx
        );
        break;
      case 2:
        rc = capdb_changeset_apply_v2(db, (int)nChangeset, pChangeset, 
            (objc==5)?test_filter_handler:0, test_conflict_handler, (void*)&ctx,
            &pRebase, &nRebase, flags
        );
        break;
      case 3:
        rc = capdb_changeset_apply_v3(db, (int)nChangeset, pChangeset, 
            (objc==5)?test_filter_v3_handler:0, test_conflict_handler, 
            (void*)&ctx, &pRebase, &nRebase, flags
        );
        break;
    }
  }else{
    sStr.aData = (unsigned char*)pChangeset;
    sStr.nData = (int)nChangeset;
    switch( iVersion ){
      case 1:
        rc = capdb_changeset_apply_strm(db, testStreamInput, (void*)&sStr,
            (objc==5) ? test_filter_handler : 0, 
            test_conflict_handler, (void *)&ctx
        );
        break;
      case 2:
        rc = capdb_changeset_apply_v2_strm(db, testStreamInput, (void*)&sStr,
            (objc==5) ? test_filter_handler : 0, 
            test_conflict_handler, (void *)&ctx,
            &pRebase, &nRebase, flags
        );
        break;
      case 3:
        rc = capdb_changeset_apply_v3_strm(db, testStreamInput, (void*)&sStr,
            (objc==5) ? test_filter_v3_handler : 0, 
            test_conflict_handler, (void *)&ctx,
            &pRebase, &nRebase, flags
        );
        break;
    }
  }

  free(pChangeset);
  if( rc!=CAPDB_OK ){
    return test_session_error(interp, rc, 0);
  }else{
    Tcl_ResetResult(interp);
    if( (iVersion==2 || iVersion==3) && pRebase ){
      Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(pRebase, nRebase));
    }
  }
  capdb_free(pRebase);
  return TCL_OK;
}

/*
** capdb_changeset_apply DB CHANGESET CONFLICT-SCRIPT ?FILTER-SCRIPT?
*/
static int CAPDB_TCLAPI test_capdb_changeset_apply(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  return testSqlite3changesetApply(1, clientData, interp, objc, objv);
}
/*
** capdb_changeset_apply_v2 DB CHANGESET CONFLICT-SCRIPT ?FILTER-SCRIPT?
*/
static int CAPDB_TCLAPI test_capdb_changeset_apply_v2(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  return testSqlite3changesetApply(2, clientData, interp, objc, objv);
}
/*
** capdb_changeset_apply_v3 DB CHANGESET CONFLICT-SCRIPT ?FILTER-SCRIPT?
*/
static int CAPDB_TCLAPI test_capdb_changeset_apply_v3(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  return testSqlite3changesetApply(3, clientData, interp, objc, objv);
}

/*
** capdb_changeset_apply_replace_all DB CHANGESET 
*/
static int CAPDB_TCLAPI test_capdb_changeset_apply_replace_all(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;                    /* Database handle */
  Tcl_CmdInfo info;               /* Database Tcl command (objv[1]) info */
  int rc;                         /* Return code from changeset_invert() */
  void *pChangeset;               /* Buffer containing changeset */
  Tcl_Size nChangeset;            /* Size of buffer aChangeset in bytes */

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB CHANGESET");
    return TCL_ERROR;
  }
  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(objv[1]), &info) ){
    Tcl_AppendResult(interp, "no such handle: ", Tcl_GetString(objv[2]), NULL);
    return TCL_ERROR;
  }
  db = *(capdb **)info.objClientData;
  pChangeset = (void *)Tcl_GetByteArrayFromObj(objv[2], &nChangeset);

  rc = capdb_changeset_apply(db, (int)nChangeset, pChangeset,
                              0, replace_handler,0);
  if( rc!=CAPDB_OK ){
    return test_session_error(interp, rc, 0);
  }
  Tcl_ResetResult(interp);
  return TCL_OK;
}


/*
** capdb_changeset_invert CHANGESET
*/
static int CAPDB_TCLAPI test_capdb_changeset_invert(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Return code from changeset_invert() */
  Tcl_Size nn;
  TestStreamInput sIn;            /* Input stream */
  TestSessionsBlob sOut;          /* Output blob */

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "CHANGESET");
    return TCL_ERROR;
  }

  memset(&sIn, 0, sizeof(sIn));
  memset(&sOut, 0, sizeof(sOut));
  sIn.nStream = test_tcl_integer(interp, SESSION_STREAM_TCL_VAR);
  sIn.aData = testGetByteArrayFromObj(objv[1], &nn);
  sIn.nData = (int)nn;

  if( sIn.nStream ){
    rc = capdb_changeset_invert_strm(
        testStreamInput, (void*)&sIn, testStreamOutput, (void*)&sOut
    );
  }else{
    rc = capdb_changeset_invert(sIn.nData, sIn.aData, &sOut.n, &sOut.p);
  }
  if( rc!=CAPDB_OK ){
    rc = test_session_error(interp, rc, 0);
  }else{
    assert_changeset_is_ok(sOut.n, sOut.p);
    Tcl_SetObjResult(interp,Tcl_NewByteArrayObj((unsigned char*)sOut.p,sOut.n));
  }
  capdb_free(sOut.p);
  free(sIn.aData);
  return rc;
}

/*
** Copy buffer aIn[] to a new nIn byte buffer obtained from malloc(). Use
** plain malloc() instead of any Tcl function because valgrind and asan are
** better at detecting small overflows in that case. Avoid capdb_malloc()
** here because that means dealing with injected OOM errors. 
**
** The caller is responsible for eventually calling free() on the returned
** value.
*/
static u8 *copyToMalloc(const u8 *aIn, int nIn){
  u8 *pRet = malloc(nIn);
  memcpy(pRet, aIn, nIn);
  return pRet;
}

/*
** capdb_changeset_concat LEFT RIGHT
*/
static int CAPDB_TCLAPI test_capdb_changeset_concat(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* Return code from changeset_invert() */
  Tcl_Size nn;

  TestStreamInput sLeft;          /* Input stream */
  TestStreamInput sRight;         /* Input stream */
  TestSessionsBlob sOut = {0,0};  /* Output blob */

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "LEFT RIGHT");
    return TCL_ERROR;
  }

  memset(&sLeft, 0, sizeof(sLeft));
  memset(&sRight, 0, sizeof(sRight));
  sLeft.aData = Tcl_GetByteArrayFromObj(objv[1], &nn);
  sLeft.nData = (int)nn;
  sRight.aData = Tcl_GetByteArrayFromObj(objv[2], &nn);
  sRight.nData = (int)nn;
  sLeft.nStream = test_tcl_integer(interp, SESSION_STREAM_TCL_VAR);
  sRight.nStream = sLeft.nStream;

  sLeft.aData = copyToMalloc(sLeft.aData, sLeft.nData);
  sRight.aData = copyToMalloc(sRight.aData, sRight.nData);

  if( sLeft.nStream>0 ){
    rc = capdb_changeset_concat_strm(
        testStreamInput, (void*)&sLeft,
        testStreamInput, (void*)&sRight,
        testStreamOutput, (void*)&sOut
    );
  }else{
    rc = capdb_changeset_concat(
        sLeft.nData, sLeft.aData, sRight.nData, sRight.aData, &sOut.n, &sOut.p
    );
  }

  free(sLeft.aData);
  free(sRight.aData);

  if( rc!=CAPDB_OK ){
    rc = test_session_error(interp, rc, 0);
  }else{
    assert_changeset_is_ok(sOut.n, sOut.p);
    Tcl_SetObjResult(interp,Tcl_NewByteArrayObj((unsigned char*)sOut.p,sOut.n));
  }
  capdb_free(sOut.p);
  return rc;
}

/*
** capdb_session_foreach VARNAME CHANGESET SCRIPT
*/
static int CAPDB_TCLAPI test_capdb_session_foreach(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *pChangeset;
  Tcl_Size nChangeset;
  capdb_changeset_iter *pIter;
  int rc;
  Tcl_Obj *pVarname;
  Tcl_Obj *pCS;
  Tcl_Obj *pScript;
  int isCheckNext = 0;
  int isInvert = 0;

  TestStreamInput sStr;
  memset(&sStr, 0, sizeof(sStr));

  while( objc>1 ){
    char *zOpt = Tcl_GetString(objv[1]);
    int nOpt = (int)strlen(zOpt);
    if( zOpt[0]!='-' ) break;
    if( nOpt<=7 && 0==capdb_strnicmp(zOpt, "-invert", nOpt) ){
      isInvert = 1;
    }else
    if( nOpt<=5 && 0==capdb_strnicmp(zOpt, "-next", nOpt) ){
      isCheckNext = 1;
    }else{
      break;
    }
    objv++;
    objc--;
  }
  if( objc!=4 ){
    Tcl_WrongNumArgs(
        interp, 1, objv, "?-next? ?-invert? VARNAME CHANGESET SCRIPT");
    return TCL_ERROR;
  }

  pVarname = objv[1];
  pCS = objv[2];
  pScript = objv[3];

  /* Take a copy of the changeset into an exact sized buffer allocated 
  ** using malloc(). The Tcl buffer will be padded by a few bytes, which
  ** prevents small overreads from being detected by ASAN when the tests
  ** are run.  */
  pChangeset = (void*)testGetByteArrayFromObj(pCS, &nChangeset);

  sStr.nStream = test_tcl_integer(interp, SESSION_STREAM_TCL_VAR);
  if( isInvert ){
    int f = CAPDB_CHANGESETSTART_INVERT;
    if( sStr.nStream==0 ){
      rc = capdb_changeset_start_v2(&pIter, (int)nChangeset, pChangeset, f);
    }else{
      void *pCtx = (void*)&sStr;
      sStr.aData = (unsigned char*)pChangeset;
      sStr.nData = (int)nChangeset;
      rc = capdb_changeset_start_v2_strm(&pIter, testStreamInput, pCtx, f);
    }
  }else{
    if( sStr.nStream==0 ){
      rc = capdb_changeset_start(&pIter, (int)nChangeset, pChangeset);
    }else{
      sStr.aData = (unsigned char*)pChangeset;
      sStr.nData = (int)nChangeset;
      rc = capdb_changeset_start_strm(&pIter, testStreamInput, (void*)&sStr);
    }
  }

  if( rc==CAPDB_OK ){
    while( CAPDB_ROW==capdb_changeset_next(pIter) ){
      Tcl_Obj *pVar = 0;            /* Tcl value to set $VARNAME to */
      pVar = testIterData(pIter);
      Tcl_ObjSetVar2(interp, pVarname, 0, pVar, 0);
      rc = Tcl_EvalObjEx(interp, pScript, 0);
      if( rc!=TCL_OK && rc!=TCL_CONTINUE ){
        capdb_changeset_finalize(pIter);
        free(pChangeset);
        return rc==TCL_BREAK ? TCL_OK : rc;
      }
    }

    if( isCheckNext ){
      int rc2 = capdb_changeset_next(pIter);
      rc = capdb_changeset_finalize(pIter);
      assert( (rc2==CAPDB_DONE && rc==CAPDB_OK) || rc2==rc );
    }else{
      rc = capdb_changeset_finalize(pIter);
    }
  }

  free(pChangeset);
  if( rc!=CAPDB_OK ){
    return test_session_error(interp, rc, 0);
  }
  return TCL_OK;
}

/*
** tclcmd: CMD configure REBASE-BLOB
** tclcmd: CMD rebase CHANGESET
** tclcmd: CMD delete
*/
static int CAPDB_TCLAPI test_rebaser_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static struct RebaseSubcmd {
    const char *zSub;
    int nArg;
    const char *zMsg;
    int iSub;
  } aSub[] = {
    { "configure",    1, "REBASE-BLOB" }, /* 0 */
    { "delete",       0, ""            }, /* 1 */
    { "rebase",       1, "CHANGESET"   }, /* 2 */
    { 0 }
  };

  capdb_rebaser *p = (capdb_rebaser*)clientData;
  int iSub;
  int rc;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[1], aSub, sizeof(aSub[0]), "sub-command", 0, &iSub
  );
  if( rc!=TCL_OK ) return rc;
  if( objc!=2+aSub[iSub].nArg ){
    Tcl_WrongNumArgs(interp, 2, objv, aSub[iSub].zMsg);
    return TCL_ERROR;
  }

  assert( iSub==0 || iSub==1 || iSub==2 );
  assert( rc==CAPDB_OK );
  switch( iSub ){
    case 0: {   /* configure */
      Tcl_Size nRebase = 0;
      unsigned char *pRebase = Tcl_GetByteArrayFromObj(objv[2], &nRebase);
      rc = capdb_rebaser_configure(p, (int)nRebase, pRebase);
      break;
    }

    case 1:     /* delete */
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;

    default: {  /* rebase */
      TestStreamInput sStr;                 /* Input stream */
      TestSessionsBlob sOut;                /* Output blob */
      Tcl_Size nn;

      memset(&sStr, 0, sizeof(sStr));
      memset(&sOut, 0, sizeof(sOut));
      sStr.aData = Tcl_GetByteArrayFromObj(objv[2], &nn);
      sStr.nData = nn;
      sStr.nStream = test_tcl_integer(interp, SESSION_STREAM_TCL_VAR);

      if( sStr.nStream ){
        rc = capdb_rebaser_rebase_strm(p, 
            testStreamInput, (void*)&sStr,
            testStreamOutput, (void*)&sOut
        );
      }else{
        rc = capdb_rebaser_rebase(p, sStr.nData, sStr.aData, &sOut.n, &sOut.p);
      }

      if( rc==CAPDB_OK ){
        assert_changeset_is_ok(sOut.n, sOut.p);
        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(sOut.p, sOut.n));
      }
      capdb_free(sOut.p);
      break;
    }
  }

  if( rc!=CAPDB_OK ){
    return test_session_error(interp, rc, 0);
  }
  return TCL_OK;
}

static void CAPDB_TCLAPI test_rebaser_del(void *clientData){
  capdb_rebaser *p = (capdb_rebaser*)clientData;
  capdb_rebaser_delete(p);
}

/*
** tclcmd: capdb_rebaser_create NAME
*/
static int CAPDB_TCLAPI test_capdb_rebaser_create(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  capdb_rebaser *pNew = 0;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME");
    return CAPDB_ERROR;
  }

  rc = capdb_rebaser_create(&pNew);
  if( rc!=CAPDB_OK ){
    return test_session_error(interp, rc, 0);
  }

  Tcl_CreateObjCommand(interp, Tcl_GetString(objv[1]), test_rebaser_cmd,
      (ClientData)pNew, test_rebaser_del
  );
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

/*
** Run some sanity checks on the changeset in nChangeset byte buffer
** pChangeset. If any fail, return a non-zero value and, optionally,
** set output variable (*pzErr) to point to a buffer containing an
** English language error message describing the problem. In this
** case it is the responsibility of the caller to free the buffer
** using capdb_free().
**
** Or, if the changeset appears to be well-formed, this function
** returns CAPDB_OK and sets (*pzErr) to NULL.
*/
static int capdb_test_changeset(
  int nChangeset,
  void *pChangeset,
  char **pzErr
){
  capdb_changeset_iter *pIter = 0;
  char *zErr = 0;
  int rc = CAPDB_OK;
  int bPatch = (nChangeset>0 && ((char*)pChangeset)[0]=='P');

  rc = capdb_changeset_start(&pIter, nChangeset, pChangeset);
  if( rc==CAPDB_OK ){
    int rc2;
    while( rc==CAPDB_OK && CAPDB_ROW==capdb_changeset_next(pIter) ){
      unsigned char *aPk = 0;
      int nCol = 0;
      int op = 0;
      const char *zTab = 0;

      capdb_changeset_pk(pIter, &aPk, &nCol);
      capdb_changeset_op(pIter, &zTab, &nCol, &op, 0);

      if( op==CAPDB_UPDATE ){
        int iCol;
        for(iCol=0; iCol<nCol; iCol++){
          capdb_value *pNew = 0;
          capdb_value *pOld = 0;
          capdb_changeset_new(pIter, iCol, &pNew);
          capdb_changeset_old(pIter, iCol, &pOld);

          if( aPk[iCol] ){
            if( pOld==0 ) rc = CAPDB_ERROR;
          }else if( bPatch ){
            if( pOld ) rc = CAPDB_ERROR;
          }else{
            if( (pOld==0)!=(pNew==0) ) rc = CAPDB_ERROR;
          }

          if( rc!=CAPDB_OK ){
            zErr = capdb_mprintf(
                "unexpected CAPDB_UPDATE (bPatch=%d pk=%d pOld=%d pNew=%d)",
                bPatch, (int)aPk[iCol], pOld!=0, pNew!=0
            );
            break;
          }
        }
      }
    }
    rc2 = capdb_changeset_finalize(pIter);
    if( rc==CAPDB_OK ){
      rc = rc2;
    }
  }

  *pzErr = zErr;
  return rc;
}

/*
** test_changeset CHANGESET
*/
static int CAPDB_TCLAPI test_changeset(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *pChangeset = 0;           /* Buffer containing changeset */
  Tcl_Size nChangeset = 0;        /* Size of buffer aChangeset in bytes */
  int rc = CAPDB_OK;
  char *z = 0;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "CHANGESET");
    return TCL_ERROR;
  }
  pChangeset = (void *)Tcl_GetByteArrayFromObj(objv[1], &nChangeset);

  Tcl_ResetResult(interp);
  rc = capdb_test_changeset((int)nChangeset, pChangeset, &z);
  if( rc!=CAPDB_OK ){
    char *zErr = capdb_mprintf("(%d) - \"%s\"", rc, z);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(zErr, -1));
    capdb_free(zErr);
  }
  capdb_free(z);

  return rc ? TCL_ERROR : TCL_OK;
}

/*
** tclcmd: capdb_rebaser_configure OP VALUE
*/
static int CAPDB_TCLAPI test_capdb_session_config(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static struct ConfigOpt {
    const char *zSub;
    int op;
  } aSub[] = {
    { "strm_size",    CAPDB_SESSION_CONFIG_STRMSIZE },
    { "invalid",      0 },
    { 0 }
  };
  int rc;
  int iSub;
  int iVal;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "OP VALUE");
    return CAPDB_ERROR;
  }
  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[1], aSub, sizeof(aSub[0]), "sub-command", 0, &iSub
  );
  if( rc!=TCL_OK ) return rc;
  if( Tcl_GetIntFromObj(interp, objv[2], &iVal) ) return TCL_ERROR;

  rc = capdb_session_config(aSub[iSub].op, (void*)&iVal);
  if( rc!=CAPDB_OK ){
    return test_session_error(interp, rc, 0);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(iVal));
  return TCL_OK;
}

typedef struct TestChangegroup TestChangegroup;
struct TestChangegroup {
  capdb_changegroup *pGrp;
};

typedef struct TestChangeIter TestChangeIter;
struct TestChangeIter {
  capdb_changeset_iter *pIter;

  /* If this iter uses streaming. */
  TestStreamInput in;
};


/*
** Destructor for Tcl changegroup command object.
*/
static void test_changegroup_del(void *clientData){
  TestChangegroup *pGrp = (TestChangegroup*)clientData;
  capdb_changegroup_delete(pGrp->pGrp);
  ckfree(pGrp);
}

static int testGetNewOrOld(Tcl_Interp *interp, Tcl_Obj *pObj, int *pbNew){
  const char *azVal[] = { "old", "new", 0 };
  int iIdx = 0;
  int rc = Tcl_GetIndexFromObj(interp, pObj, azVal, "record", 0, &iIdx);
  *pbNew = iIdx;
  return rc;
}

/*
** Tclcmd:  $changegroup schema DB DBNAME
** Tclcmd:  $changegroup add CHANGESET
** Tclcmd:  $changegroup output
** Tclcmd:  $changegroup delete
*/
static int CAPDB_TCLAPI test_changegroup_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  TestChangegroup *p = (TestChangegroup*)clientData;
  static struct ChangegroupCmd {
    const char *zSub;
    int nArg;
    const char *zMsg;
  } aSub[] = {
    { "schema",          2, "DB DBNAME"            },    /* 0 */
    { "add",             1, "CHANGESET"            },    /* 1 */
    { "output",          0, ""                     },    /* 2 */
    { "delete",          0, ""                     },    /* 3 */
    { "add_change",      1, "ITERATOR"             },    /* 4 */

    { "change_begin",    3, "TYPE TABLE INDIRECT"  },    /* 5 */
    { "change_int64",    3, "[new|old] ICOL VALUE" },    /* 6 */
    { "change_null",     2, "[new|old] ICOL"       },    /* 7 */
    { "change_double",   3, "[new|old] ICOL VALUE" },    /* 8 */
    { "change_text",     3, "[new|old] ICOL VALUE" },    /* 9 */
    { "change_blob",     3, "[new|old] ICOL VALUE" },    /* 10 */
    { "change_finish",   1, "BDISCARD"             },    /* 11 */
    { "change_finishne", 1, "BDISCARD"             },    /* 12 */

    { "config",          2, "OPTION INTVAL"        },    /* 13 */
    { "change_text-1",   3, "[new|old] ICOL VALUE" },    /* 14 */
    { "change_begin_ne", 3, "TYPE TABLE INDIRECT"  },    /* 15 */
    { 0, 0, 0 }
  };
  int rc = TCL_OK;
  int iSub = 0;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[1], aSub, sizeof(aSub[0]), "sub-command", 0, &iSub
  );
  if( rc!=TCL_OK ) return rc;
  if( objc!=2+aSub[iSub].nArg ){
    Tcl_WrongNumArgs(interp, 2, objv, aSub[iSub].zMsg);
    return TCL_ERROR;
  }

  switch( iSub ){
    case 0: {      /* schema */
      capdb *db = 0;
      const char *zDb = Tcl_GetString(objv[3]);
      if( dbHandleFromObj(interp, objv[2], &db) ){
        return TCL_ERROR;
      }
      rc = capdb_changegroup_schema(p->pGrp, db, zDb);
      if( rc!=CAPDB_OK ) rc = test_session_error(interp, rc, 0);
      break;
    };

    case 1: {      /* add */
      Tcl_Size nByte = 0;
      void *aByte = testGetByteArrayFromObj(objv[2], &nByte);
      rc = capdb_changegroup_add(p->pGrp, (int)nByte, aByte);
      if( rc!=CAPDB_OK ) rc = test_session_error(interp, rc, 0);
      free(aByte);
      break;
    };

    case 2: {      /* output */
      int nByte = 0;
      u8 *aByte = 0;
      rc = capdb_changegroup_output(p->pGrp, &nByte, (void**)&aByte);
      if( rc!=CAPDB_OK ){
        rc = test_session_error(interp, rc, 0);
      }else{
        Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(aByte, nByte));
      }
      capdb_free(aByte);
      break;
    };

    case 4: {      /* add_change */
      Tcl_CmdInfo cmdInfo;            /* Database Tcl command (objv[2]) info */
      TestChangeIter *pIter = 0;
      const char *zIter = Tcl_GetString(objv[2]);
      if( 0==Tcl_GetCommandInfo(interp, zIter, &cmdInfo) ){
        Tcl_AppendResult(interp, "no such iter: ", Tcl_GetString(objv[2]), NULL);
        return TCL_ERROR;
      }

      pIter = (struct TestChangeIter*)cmdInfo.objClientData;

      rc = capdb_changegroup_add_change(p->pGrp, pIter->pIter);
      if( rc!=CAPDB_OK ){
        rc = test_session_error(interp, rc, 0);
      }
      break;
    };

    case 15:        /* change_beginne */
    case 5: {       /* change_begin */
      struct ChangeType {
        const char *zType;
        int eType;
      } aType[] = {
        { "INSERT", CAPDB_INSERT },
        { "UPDATE", CAPDB_UPDATE },
        { "DELETE", CAPDB_DELETE },
        { 0, 0 }
      };
      int eType = 0;
      const char *zTab = 0;
      int bIndirect;
      int iIdx = 0;
      char *zErr = 0;
      char **pz = ((iSub==5) ? &zErr : 0);

      if( TCL_OK!=Tcl_GetIntFromObj(0, objv[2], &eType) ){
        rc = Tcl_GetIndexFromObjStruct(
            interp, objv[2], aType, sizeof(aType[0]), "TYPE", 0, &iIdx
        );
        if( rc!=TCL_OK ) return rc;
        eType = aType[iIdx].eType;
      }
      zTab = Tcl_GetString(objv[3]);
      if( Tcl_GetBooleanFromObj(interp, objv[4], &bIndirect) ){
        return TCL_ERROR;
      }

      rc = capdb_changegroup_change_begin(p->pGrp, eType, zTab, bIndirect, pz);
      assert( zErr==0 || rc!=CAPDB_OK );
      if( rc!=CAPDB_OK ){
        rc = test_session_error(interp, rc, zErr);
      }

      break;
    }

    case 6: {      /* change_int64 */
      int bNew = 0;
      int iCol = 0;
      capdb_int64 iVal = 0;
      if( TCL_OK!=testGetNewOrOld(interp, objv[2], &bNew)
       || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &iCol)
       || TCL_OK!=Tcl_GetWideIntFromObj(interp, objv[4], &iVal)
      ){
        rc = TCL_ERROR;
      }else{
        rc = capdb_changegroup_change_int64(p->pGrp, bNew, iCol, iVal);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }
      }
      break;
    }

    case 7: {      /* change_null */
      int bNew = 0;
      int iCol = 0;
      if( TCL_OK!=testGetNewOrOld(interp, objv[2], &bNew)
       || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &iCol)
      ){
        rc = TCL_ERROR;
      }else{
        rc = capdb_changegroup_change_null(p->pGrp, bNew, iCol);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }
      }
      break;
    }

    case 8: {      /* change_double */
      int bNew = 0;
      int iCol = 0;
      double rVal = 0;
      if( TCL_OK!=testGetNewOrOld(interp, objv[2], &bNew)
       || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &iCol)
       || TCL_OK!=Tcl_GetDoubleFromObj(interp, objv[4], &rVal)
      ){
        rc = TCL_ERROR;
      }else{
        rc = capdb_changegroup_change_double(p->pGrp, bNew, iCol, rVal);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }
      }
      break;
    }

    case 9: {      /* change_text */
      int bNew = 0;
      int iCol = 0;
      if( TCL_OK!=testGetNewOrOld(interp, objv[2], &bNew)
       || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &iCol)
      ){
        rc = TCL_ERROR;
      }else{
        Tcl_Size nVal = 0;
        const char *pVal = Tcl_GetStringFromObj(objv[4], &nVal);
        rc = capdb_changegroup_change_text(p->pGrp, bNew, iCol, pVal, nVal);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }
      }
      break;
    }

    case 10: {      /* change_blob */
      int bNew = 0;
      int iCol = 0;
      if( TCL_OK!=testGetNewOrOld(interp, objv[2], &bNew)
       || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &iCol)
      ){
        rc = TCL_ERROR;
      }else{
        Tcl_Size nVal = 0;
        const u8 *pVal = Tcl_GetByteArrayFromObj(objv[4], &nVal);
        rc = capdb_changegroup_change_blob(p->pGrp, bNew, iCol, pVal, nVal);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }
      }
      break;
    }

    case 12:        /* change_finishne */
    case 11: {      /* change_finish */
      int bDiscard = 0;
      if( TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[2], &bDiscard) ){
        rc = TCL_ERROR;
      }else{
        char *zErr = 0;
        char **pz = &zErr;
        if( iSub==12 ) pz = 0;
        rc = capdb_changegroup_change_finish(p->pGrp, bDiscard, pz);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, zErr);
        }
      }
      break;
    }

    case 13: {      /* config */
      struct OptionName {
        const char *zOpt;
        int op;
      } aOp[] = {
        { "patchset", CAPDB_CHANGEGROUP_CONFIG_PATCHSET },
        { 0, 0 }
      };
      int iIdx = 0;
      int iArg = 0;
      rc = Tcl_GetIndexFromObjStruct(
          interp, objv[2], aOp, sizeof(aOp[0]), "option", 0, &iIdx
      );
      if( rc==TCL_OK 
       && (rc = Tcl_GetIntFromObj(interp, objv[3], &iArg))==TCL_OK 
      ){
        int op = aOp[iIdx].op;
        void *pArg = (void*)&iArg;

        rc = capdb_changegroup_config(p->pGrp, op, pArg);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }else{
          Tcl_SetObjResult(interp, Tcl_NewIntObj(iArg));
        }
      }
      break;
    }

    case 14: {      /* change_text-1 */
      int bNew = 0;
      int iCol = 0;
      if( TCL_OK!=testGetNewOrOld(interp, objv[2], &bNew)
       || TCL_OK!=Tcl_GetIntFromObj(interp, objv[3], &iCol)
      ){
        rc = TCL_ERROR;
      }else{
        const char *pVal = Tcl_GetString(objv[4]);
        rc = capdb_changegroup_change_text(p->pGrp, bNew, iCol, pVal, -1);
        if( rc!=CAPDB_OK ){
          rc = test_session_error(interp, rc, 0);
        }
      }
      break;
    }

    default: {     /* delete */
      assert( iSub==3 );
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;
    }
  }

  return rc;
}

/*
** Tclcmd:  capdb_changegroup CMD
*/
static int CAPDB_TCLAPI test_capdb_changegroup(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;                         /* capdb_changegroup_new() return code */
  TestChangegroup *p;             /* New wrapper object */

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "CMD");
    return TCL_ERROR;
  }

  p = (TestChangegroup*)ckalloc(sizeof(TestChangegroup));
  memset(p, 0, sizeof(TestChangegroup));
  rc = capdb_changegroup_new(&p->pGrp);
  if( rc!=CAPDB_OK ){
    ckfree((char*)p);
    return test_session_error(interp, rc, 0);
  }

  Tcl_CreateObjCommand(
      interp, Tcl_GetString(objv[1]), test_changegroup_cmd, (ClientData)p,
      test_changegroup_del
  );
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

extern const char *capdbErrName(int);

/*
** Destructor for Tcl iterator command object.
*/
static void test_iter_del(void *clientData){
  TestChangeIter *p = (TestChangeIter*)clientData;
  capdb_changeset_finalize(p->pIter);
  ckfree(p);
}

static int CAPDB_TCLAPI test_iter_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static const char *aSub[] = {
    "next",        /* 0 */
    "data",        /* 1 */
    "finalize",    /* 2 */
    0
  };
  int iSub = 0;

  TestChangeIter *p = (TestChangeIter*)clientData;
  int rc = CAPDB_OK;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "CMD");
    return TCL_ERROR;
  }

  if( Tcl_GetIndexFromObj(interp, objv[1], aSub, "sub-command", 0, &iSub) ){
    return TCL_ERROR;
  }
  switch( iSub ){
    case 0:
      rc = capdb_changeset_next(p->pIter);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(capdbErrName(rc), -1));
      break;
    case 1:
      Tcl_SetObjResult(interp, testIterData(p->pIter));
      break;
    case 2:
      rc = capdb_changeset_finalize(p->pIter);
      p->pIter = 0;
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      Tcl_SetObjResult(interp, Tcl_NewStringObj(capdbErrName(rc), -1));
      break;
    default:
      assert( 0 );
      break;
  }

  return TCL_OK;
}

/*
** Tclcmd:  capdb_changeset_start ?-invert? CHANGESET
*/
static int CAPDB_TCLAPI test_capdb_changeset_start(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int isInvert = 0;
  void *pChangeset = 0;           /* Buffer containing changeset */
  Tcl_Size nChangeset = 0;        /* Size of buffer aChangeset in bytes */
  TestChangeIter *pNew = 0;
  capdb_changeset_iter *pIter = 0;
  int flags = 0;
  int rc = CAPDB_OK;
  int nAlloc = 0;                 /* Bytes of space to allocate */

  static int iCmd = 1;
  char zCmd[64];

  if( objc==3 ){
    Tcl_Size n = 0;
    const char *z = Tcl_GetStringFromObj(objv[1], &n);
    isInvert = (n>=2 && capdb_strnicmp(z, "-invert", (int)n)==0);
  }

  if( objc!=2 && (objc!=3 || !isInvert) ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-invert? CHANGESET");
    return TCL_ERROR;
  }

  pChangeset = (void *)Tcl_GetByteArrayFromObj(objv[objc-1], &nChangeset);
  flags = isInvert ? CAPDB_CHANGESETSTART_INVERT : 0;

  nAlloc = sizeof(TestChangeIter);
  if( test_tcl_integer(interp, SESSION_STREAM_TCL_VAR) ){
    nAlloc += nChangeset;
  }
  pNew = (TestChangeIter*)ckalloc(nAlloc);
  memset(pNew, 0, nAlloc);
  if( test_tcl_integer(interp, SESSION_STREAM_TCL_VAR) ){
    pNew->in.nStream = test_tcl_integer(interp, SESSION_STREAM_TCL_VAR);
    pNew->in.nData = nChangeset;
    pNew->in.aData = (unsigned char*)&pNew[1];
    memcpy(pNew->in.aData, pChangeset, nChangeset);
  }

  if( pNew->in.nStream ){
    void *pCtx = (void*)&pNew->in;
    rc = capdb_changeset_start_v2_strm(&pIter, testStreamInput, pCtx, flags);
  }else{
    rc = capdb_changeset_start_v2(&pIter, (int)nChangeset, pChangeset, flags);
  }
  if( rc!=CAPDB_OK ){
    char *zErr = capdb_mprintf(
        "error in capdb_changeset_start_v2() - %d", rc
    );
    Tcl_AppendResult(interp, zErr, (char*)0);
    ckfree(pNew);
    return TCL_ERROR;
  }
  pNew->pIter = pIter;

  sprintf(zCmd, "csiter%d", iCmd++);
  Tcl_CreateObjCommand(interp, zCmd, test_iter_cmd, (void*)pNew, test_iter_del);
  Tcl_SetObjResult(interp, Tcl_NewStringObj(zCmd, -1));
  return TCL_OK;
}

int TestSession_Init(Tcl_Interp *interp){
  struct Cmd {
    const char *zCmd;
    Tcl_ObjCmdProc *xProc;
  } aCmd[] = {
    { "capdb_session", test_capdb_session },
    { "capdb_changegroup", test_capdb_changegroup },
    { "capdb_changeset_start", test_capdb_changeset_start },
    { "capdb_session_foreach", test_capdb_session_foreach },
    { "capdb_changeset_invert", test_capdb_changeset_invert },
    { "capdb_changeset_concat", test_capdb_changeset_concat },
    { "capdb_changeset_apply", test_capdb_changeset_apply },
    { "capdb_changeset_apply_v2", test_capdb_changeset_apply_v2 },
    { "capdb_changeset_apply_v3", test_capdb_changeset_apply_v3 },
    { "capdb_changeset_apply_replace_all", 
      test_capdb_changeset_apply_replace_all },
    { "sql_exec_changeset", test_sql_exec_changeset },
    { "capdb_rebaser_create", test_capdb_rebaser_create },
    { "capdb_session_config", test_capdb_session_config },
    { "test_changeset", test_changeset },
  };
  int i;

  for(i=0; i<sizeof(aCmd)/sizeof(struct Cmd); i++){
    struct Cmd *p = &aCmd[i];
    Tcl_CreateObjCommand(interp, p->zCmd, p->xProc, 0, 0);
  }

  return TCL_OK;
}

#endif /* CAPDB_TEST && CAPDB_SESSION && CAPDB_PREUPDATE_HOOK */
