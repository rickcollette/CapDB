/*
** 2003 January 11
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the capdb_set_authorizer()
** API.  This facility is an optional feature of the library.  Embedded
** systems that do not need this facility may omit it by recompiling
** the library with -DCAPDB_OMIT_AUTHORIZATION=1
*/
#include "capdbInt.h"

/*
** All of the code in this file may be omitted by defining a single
** macro.
*/
#ifndef CAPDB_OMIT_AUTHORIZATION

/*
** Set or clear the access authorization function.
**
** The access authorization function is be called during the compilation
** phase to verify that the user has read and/or write access permission on
** various fields of the database.  The first argument to the auth function
** is a copy of the 3rd argument to this routine.  The second argument
** to the auth function is one of these constants:
**
**       CAPDB_CREATE_INDEX
**       CAPDB_CREATE_TABLE
**       CAPDB_CREATE_TEMP_INDEX
**       CAPDB_CREATE_TEMP_TABLE
**       CAPDB_CREATE_TEMP_TRIGGER
**       CAPDB_CREATE_TEMP_VIEW
**       CAPDB_CREATE_TRIGGER
**       CAPDB_CREATE_VIEW
**       CAPDB_DELETE
**       CAPDB_DROP_INDEX
**       CAPDB_DROP_TABLE
**       CAPDB_DROP_TEMP_INDEX
**       CAPDB_DROP_TEMP_TABLE
**       CAPDB_DROP_TEMP_TRIGGER
**       CAPDB_DROP_TEMP_VIEW
**       CAPDB_DROP_TRIGGER
**       CAPDB_DROP_VIEW
**       CAPDB_INSERT
**       CAPDB_PRAGMA
**       CAPDB_READ
**       CAPDB_SELECT
**       CAPDB_TRANSACTION
**       CAPDB_UPDATE
**
** The third and fourth arguments to the auth function are the name of
** the table and the column that are being accessed.  The auth function
** should return either CAPDB_OK, CAPDB_DENY, or CAPDB_IGNORE.  If
** CAPDB_OK is returned, it means that access is allowed.  CAPDB_DENY
** means that the SQL statement will never-run - the capdb_exec() call
** will return with an error.  CAPDB_IGNORE means that the SQL statement
** should run but attempts to read the specified column will return NULL
** and attempts to write the column will be ignored.
**
** Setting the auth function to NULL disables this hook.  The default
** setting of the auth function is NULL.
*/
int capdb_set_authorizer(
  capdb *db,
  int (*xAuth)(void*,int,const char*,const char*,const char*,const char*),
  void *pArg
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  db->xAuth = (capdb_xauth)xAuth;
  db->pAuthArg = pArg;
  capdbExpirePreparedStatements(db, 1);
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

/*
** Write an error message into pParse->zErrMsg that explains that the
** user-supplied authorization function returned an illegal value.
*/
static void sqliteAuthBadReturnCode(Parse *pParse){
  capdbErrorMsg(pParse, "authorizer malfunction");
  pParse->rc = CAPDB_ERROR;
}

/*
** Invoke the authorization callback for permission to read column zCol from
** table zTab in database zDb. This function assumes that an authorization
** callback has been registered (i.e. that capdb.xAuth is not NULL).
**
** If CAPDB_IGNORE is returned and pExpr is not NULL, then pExpr is changed
** to an SQL NULL expression. Otherwise, if pExpr is NULL, then CAPDB_IGNORE
** is treated as CAPDB_DENY. In this case an error is left in pParse.
*/
int capdbAuthReadCol(
  Parse *pParse,                  /* The parser context */
  const char *zTab,               /* Table name */
  const char *zCol,               /* Column name */
  int iDb                         /* Index of containing database. */
){
  capdb *db = pParse->db;          /* Database handle */
  char *zDb = db->aDb[iDb].zDbSName; /* Schema name of attached database */
  int rc;                            /* Auth callback return code */

  if( db->init.busy ) return CAPDB_OK;
  rc = db->xAuth(db->pAuthArg, CAPDB_READ, zTab,zCol,zDb,pParse->zAuthContext);
  if( rc==CAPDB_DENY ){
    char *z = capdb_mprintf("%s.%s", zTab, zCol);
    if( db->nDb>2 || iDb!=0 ) z = capdb_mprintf("%s.%z", zDb, z);
    capdbErrorMsg(pParse, "access to %z is prohibited", z);
    pParse->rc = CAPDB_AUTH;
  }else if( rc!=CAPDB_IGNORE && rc!=CAPDB_OK ){
    sqliteAuthBadReturnCode(pParse);
  }
  return rc;
}

/*
** The pExpr should be a TK_COLUMN expression.  The table referred to
** is in pTabList or else it is the NEW or OLD table of a trigger.  
** Check to see if it is OK to read this particular column.
**
** If the auth function returns CAPDB_IGNORE, change the TK_COLUMN 
** instruction into a TK_NULL.  If the auth function returns CAPDB_DENY,
** then generate an error.
*/
void capdbAuthRead(
  Parse *pParse,        /* The parser context */
  Expr *pExpr,          /* The expression to check authorization on */
  Schema *pSchema,      /* The schema of the expression */
  SrcList *pTabList     /* All table that pExpr might refer to */
){
  Table *pTab = 0;      /* The table being read */
  const char *zCol;     /* Name of the column of the table */
  int iSrc;             /* Index in pTabList->a[] of table being read */
  int iDb;              /* The index of the database the expression refers to */
  int iCol;             /* Index of column in table */

  assert( pExpr->op==TK_COLUMN || pExpr->op==TK_TRIGGER );
  assert( !IN_RENAME_OBJECT );
  assert( pParse->db->xAuth!=0 );
  iDb = capdbSchemaToIndex(pParse->db, pSchema);
  if( iDb<0 ){
    /* An attempt to read a column out of a subquery or other
    ** temporary table. */
    return;
  }

  if( pExpr->op==TK_TRIGGER ){
    pTab = pParse->pTriggerTab;
  }else{
    assert( pTabList );
    for(iSrc=0; iSrc<pTabList->nSrc; iSrc++){
      if( pExpr->iTable==pTabList->a[iSrc].iCursor ){
        pTab = pTabList->a[iSrc].pSTab;
        break;
      }
    }
  }
  iCol = pExpr->iColumn;
  if( pTab==0 ) return;

  if( iCol>=0 ){
    assert( iCol<pTab->nCol );
    zCol = pTab->aCol[iCol].zCnName;
  }else if( pTab->iPKey>=0 ){
    assert( pTab->iPKey<pTab->nCol );
    zCol = pTab->aCol[pTab->iPKey].zCnName;
  }else{
    zCol = "ROWID";
  }
  assert( iDb>=0 && iDb<pParse->db->nDb );
  if( CAPDB_IGNORE==capdbAuthReadCol(pParse, pTab->zName, zCol, iDb) ){
    pExpr->op = TK_NULL;
  }
}

/*
** Do an authorization check using the code and arguments given.  Return
** either CAPDB_OK (zero) or CAPDB_IGNORE or CAPDB_DENY.  If CAPDB_DENY
** is returned, then the error count and error message in pParse are
** modified appropriately.
**
** Divided into two routines.  realAuthCheck() does the work.  The
** capdbAuthCheck() routine is usually a fast no-op but invokes
** realAuthCheck() (and spends time doing some stack pushes and pops
** as a result) in the uncommon case where an authorization check is
** actually needed.
*/
static int CAPDB_NOINLINE realAuthCheck(
  Parse *pParse,
  int code,
  const char *zArg1,
  const char *zArg2,
  const char *zArg3
){
  capdb *db = pParse->db;
  int rc;

  /* Don't do any authorization checks if the database is initializing
  ** or if the parser is being invoked from within capdb_declare_vtab.
  */
  assert( !IN_RENAME_OBJECT || db->xAuth==0 );
  if( IN_SPECIAL_PARSE ){
    return CAPDB_OK;
  }

  /* EVIDENCE-OF: R-43249-19882 The third through sixth parameters to the
  ** callback are either NULL pointers or zero-terminated strings that
  ** contain additional details about the action to be authorized.
  **
  ** The following testcase() macros show that any of the 3rd through 6th
  ** parameters can be either NULL or a string. */
  testcase( zArg1==0 );
  testcase( zArg2==0 );
  testcase( zArg3==0 );
  testcase( pParse->zAuthContext==0 );

  rc = db->xAuth(db->pAuthArg,code,zArg1,zArg2,zArg3,pParse->zAuthContext);
  if( rc==CAPDB_DENY ){
    capdbErrorMsg(pParse, "not authorized");
    pParse->rc = CAPDB_AUTH;
  }else if( rc!=CAPDB_OK && rc!=CAPDB_IGNORE ){
    rc = CAPDB_DENY;
    sqliteAuthBadReturnCode(pParse);
  }
  return rc;
}
int capdbAuthCheck(
  Parse *pParse,
  int code,
  const char *zArg1,
  const char *zArg2,
  const char *zArg3
){
  if( pParse->db->xAuth!=0 && pParse->db->init.busy==0 ){
    return realAuthCheck(pParse,code,zArg1,zArg2,zArg3);
  }else{
    return CAPDB_OK;
  }
}

/*
** Push an authorization context.  After this routine is called, the
** zArg3 argument to authorization callbacks will be zContext until
** popped.  Or if pParse==0, this routine is a no-op.
*/
void capdbAuthContextPush(
  Parse *pParse,
  AuthContext *pContext, 
  const char *zContext
){
  assert( pParse );
  pContext->pParse = pParse;
  pContext->zAuthContext = pParse->zAuthContext;
  pParse->zAuthContext = zContext;
}

/*
** Pop an authorization context that was previously pushed
** by capdbAuthContextPush
*/
void capdbAuthContextPop(AuthContext *pContext){
  if( pContext->pParse ){
    pContext->pParse->zAuthContext = pContext->zAuthContext;
    pContext->pParse = 0;
  }
}

#endif /* CAPDB_OMIT_AUTHORIZATION */
