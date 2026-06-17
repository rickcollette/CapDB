/*
** 2007 March 29
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
** This file contains obscure tests of the C-interface required
** for completeness. Test code is written in C for these cases
** as there is not much point in binding to Tcl.
*/
#include "capdbInt.h"
#include "tclsqlite.h"
#include <stdlib.h>
#include <string.h>

/*
** c_collation_test
*/
static int CAPDB_TCLAPI c_collation_test(
  ClientData clientData, /* Pointer to capdb_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  const char *zErrFunction = "N/A";
  capdb *db;

  int rc;
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  /* Open a database. */
  rc = capdb_open(":memory:", &db);
  if( rc!=CAPDB_OK ){
    zErrFunction = "capdb_open";
    goto error_out;
  }

  rc = capdb_create_collation(db, "collate", 456, 0, 0);
  if( rc!=CAPDB_MISUSE ){
    capdb_close(db);
    zErrFunction = "capdb_create_collation";
    goto error_out;
  }

  capdb_close(db);
  return TCL_OK;

error_out:
  Tcl_ResetResult(interp);
  Tcl_AppendResult(interp, "Error testing function: ", zErrFunction, NULL);
  return TCL_ERROR;
}

/*
** c_realloc_test
*/
static int CAPDB_TCLAPI c_realloc_test(
  ClientData clientData, /* Pointer to capdb_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  void *p;
  const char *zErrFunction = "N/A";

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  p = capdb_malloc(5);
  if( !p ){
    zErrFunction = "capdb_malloc";
    goto error_out;
  }

  /* Test that realloc()ing a block of memory to a negative size is
  ** the same as free()ing that memory.
  */
  p = capdb_realloc(p, -1);
  if( p ){
    zErrFunction = "capdb_realloc";
    goto error_out;
  }

  return TCL_OK;

error_out:
  Tcl_ResetResult(interp);
  Tcl_AppendResult(interp, "Error testing function: ", zErrFunction, NULL);
  return TCL_ERROR;
}


/*
** c_misuse_test
*/
static int CAPDB_TCLAPI c_misuse_test(
  ClientData clientData, /* Pointer to capdb_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  const char *zErrFunction = "N/A";
  capdb *db = 0;
  capdb_stmt *pStmt;
  int rc;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  /* Open a database. Then close it again. We need to do this so that
  ** we have a "closed database handle" to pass to various API functions.
  */
  rc = capdb_open(":memory:", &db);
  if( rc!=CAPDB_OK ){
    zErrFunction = "capdb_open";
    goto error_out;
  }
  capdb_close(db);


  rc = capdb_errcode(db);
  if( rc!=CAPDB_MISUSE ){
    zErrFunction = "capdb_errcode";
    goto error_out;
  }

  pStmt = (capdb_stmt*)1234;
  rc = capdb_prepare(db, 0, 0, &pStmt, 0);
  if( rc!=CAPDB_MISUSE ){
    zErrFunction = "capdb_prepare";
    goto error_out;
  }
  assert( pStmt==0 ); /* Verify that pStmt is zeroed even on a MISUSE error */

  pStmt = (capdb_stmt*)1234;
  rc = capdb_prepare_v2(db, 0, 0, &pStmt, 0);
  if( rc!=CAPDB_MISUSE ){
    zErrFunction = "capdb_prepare_v2";
    goto error_out;
  }
  assert( pStmt==0 );

#ifndef CAPDB_OMIT_UTF16
  pStmt = (capdb_stmt*)1234;
  rc = capdb_prepare16(db, 0, 0, &pStmt, 0);
  if( rc!=CAPDB_MISUSE ){
    zErrFunction = "capdb_prepare16";
    goto error_out;
  }
  assert( pStmt==0 );
  pStmt = (capdb_stmt*)1234;
  rc = capdb_prepare16_v2(db, 0, 0, &pStmt, 0);
  if( rc!=CAPDB_MISUSE ){
    zErrFunction = "capdb_prepare16_v2";
    goto error_out;
  }
  assert( pStmt==0 );
#endif

  return TCL_OK;

error_out:
  Tcl_ResetResult(interp);
  Tcl_AppendResult(interp, "Error testing function: ", zErrFunction, NULL);
  return TCL_ERROR;
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest9_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
     void *clientData;
  } aObjCmd[] = {
     { "c_misuse_test",    c_misuse_test, 0 },
     { "c_realloc_test",   c_realloc_test, 0 },
     { "c_collation_test", c_collation_test, 0 },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, 
        aObjCmd[i].xProc, aObjCmd[i].clientData, 0);
  }
  return TCL_OK;
}
