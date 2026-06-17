/*
** 2006 June 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the virtual table interfaces.  This code
** is not included in the SQLite library.  It is used for automated
** testing of the SQLite library.
*/

/* The code in this file defines a capdb virtual-table module that
** provides a read-only view of the current database schema. There is one
** row in the schema table for each column in the database schema.
*/
#define SCHEMA \
"CREATE TABLE x("                                                            \
  "database,"          /* Name of database (i.e. main, temp etc.) */         \
  "tablename,"         /* Name of table */                                   \
  "cid,"               /* Column number (from left-to-right, 0 upward) */    \
  "name,"              /* Column name */                                     \
  "type,"              /* Specified type (i.e. VARCHAR(32)) */               \
  "not_null,"          /* Boolean. True if NOT NULL was specified */         \
  "dflt_value,"        /* Default value for this column */                   \
  "pk"                 /* True if this column is part of the primary key */  \
")"

/* If CAPDB_TEST is defined this code is preprocessed for use as part
** of the sqlite test binary "testfixture". Otherwise it is preprocessed
** to be compiled into an sqlite dynamic extension.
*/
#ifdef CAPDB_TEST
#  include "capdbInt.h"
#  include "tclsqlite.h"
#else
#  include "capdbext.h"
  CAPDB_EXTENSION_INIT1
#endif

#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct schema_vtab schema_vtab;
typedef struct schema_cursor schema_cursor;

/* A schema table object */
struct schema_vtab {
  capdb_vtab base;
  capdb *db;
};

/* A schema table cursor object */
struct schema_cursor {
  capdb_vtab_cursor base;
  capdb_stmt *pDbList;
  capdb_stmt *pTableList;
  capdb_stmt *pColumnList;
  int rowid;
};

/*
** None of this works unless we have virtual tables.
*/
#ifndef CAPDB_OMIT_VIRTUALTABLE

/*
** Table destructor for the schema module.
*/
static int schemaDestroy(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return 0;
}

/*
** Table constructor for the schema module.
*/
static int schemaCreate(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  int rc = CAPDB_NOMEM;
  schema_vtab *pVtab = capdb_malloc(sizeof(schema_vtab));
  if( pVtab ){
    memset(pVtab, 0, sizeof(schema_vtab));
    pVtab->db = db;
#ifndef CAPDB_OMIT_VIRTUALTABLE
    rc = capdb_declare_vtab(db, SCHEMA);
#endif
  }
  *ppVtab = (capdb_vtab *)pVtab;
  return rc;
}

/*
** Open a new cursor on the schema table.
*/
static int schemaOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  int rc = CAPDB_NOMEM;
  schema_cursor *pCur;
  pCur = capdb_malloc(sizeof(schema_cursor));
  if( pCur ){
    memset(pCur, 0, sizeof(schema_cursor));
    *ppCursor = (capdb_vtab_cursor *)pCur;
    rc = CAPDB_OK;
  }
  return rc;
}

/*
** Close a schema table cursor.
*/
static int schemaClose(capdb_vtab_cursor *cur){
  schema_cursor *pCur = (schema_cursor *)cur;
  capdb_finalize(pCur->pDbList);
  capdb_finalize(pCur->pTableList);
  capdb_finalize(pCur->pColumnList);
  capdb_free(pCur);
  return CAPDB_OK;
}

/*
** Retrieve a column of data.
*/
static int schemaColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  schema_cursor *pCur = (schema_cursor *)cur;
  switch( i ){
    case 0:
      capdb_result_value(ctx, capdb_column_value(pCur->pDbList, 1));
      break;
    case 1:
      capdb_result_value(ctx, capdb_column_value(pCur->pTableList, 0));
      break;
    default:
      capdb_result_value(ctx, capdb_column_value(pCur->pColumnList, i-2));
      break;
  }
  return CAPDB_OK;
}

/*
** Retrieve the current rowid.
*/
static int schemaRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  schema_cursor *pCur = (schema_cursor *)cur;
  *pRowid = pCur->rowid;
  return CAPDB_OK;
}

static int finalize(capdb_stmt **ppStmt){
  int rc = capdb_finalize(*ppStmt);
  *ppStmt = 0;
  return rc;
}

static int schemaEof(capdb_vtab_cursor *cur){
  schema_cursor *pCur = (schema_cursor *)cur;
  return (pCur->pDbList ? 0 : 1);
}

/*
** Advance the cursor to the next row.
*/
static int schemaNext(capdb_vtab_cursor *cur){
  int rc = CAPDB_OK;
  schema_cursor *pCur = (schema_cursor *)cur;
  schema_vtab *pVtab = (schema_vtab *)(cur->pVtab);
  char *zSql = 0;

  while( !pCur->pColumnList || CAPDB_ROW!=capdb_step(pCur->pColumnList) ){
    if( CAPDB_OK!=(rc = finalize(&pCur->pColumnList)) ) goto next_exit;

    while( !pCur->pTableList || CAPDB_ROW!=capdb_step(pCur->pTableList) ){
      if( CAPDB_OK!=(rc = finalize(&pCur->pTableList)) ) goto next_exit;

      assert(pCur->pDbList);
      while( CAPDB_ROW!=capdb_step(pCur->pDbList) ){
        rc = finalize(&pCur->pDbList);
        goto next_exit;
      }

      /* Set zSql to the SQL to pull the list of tables from the 
      ** sqlite_schema (or sqlite_temp_schema) table of the database
      ** identified by the row pointed to by the SQL statement pCur->pDbList
      ** (iterating through a "PRAGMA database_list;" statement).
      */
      if( capdb_column_int(pCur->pDbList, 0)==1 ){
        zSql = capdb_mprintf(
            "SELECT name FROM sqlite_temp_schema WHERE type='table'"
        );
      }else{
        capdb_stmt *pDbList = pCur->pDbList;
        zSql = capdb_mprintf(
            "SELECT name FROM %Q.sqlite_schema WHERE type='table'",
             capdb_column_text(pDbList, 1)
        );
      }
      if( !zSql ){
        rc = CAPDB_NOMEM;
        goto next_exit;
      }

      rc = capdb_prepare(pVtab->db, zSql, -1, &pCur->pTableList, 0);
      capdb_free(zSql);
      if( rc!=CAPDB_OK ) goto next_exit;
    }

    /* Set zSql to the SQL to the table_info pragma for the table currently
    ** identified by the rows pointed to by statements pCur->pDbList and
    ** pCur->pTableList.
    */
    zSql = capdb_mprintf("PRAGMA %Q.table_info(%Q)", 
        capdb_column_text(pCur->pDbList, 1),
        capdb_column_text(pCur->pTableList, 0)
    );

    if( !zSql ){
      rc = CAPDB_NOMEM;
      goto next_exit;
    }
    rc = capdb_prepare(pVtab->db, zSql, -1, &pCur->pColumnList, 0);
    capdb_free(zSql);
    if( rc!=CAPDB_OK ) goto next_exit;
  }
  pCur->rowid++;

next_exit:
  /* TODO: Handle rc */
  return rc;
}

/*
** Reset a schema table cursor.
*/
static int schemaFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  int rc;
  schema_vtab *pVtab = (schema_vtab *)(pVtabCursor->pVtab);
  schema_cursor *pCur = (schema_cursor *)pVtabCursor;
  pCur->rowid = 0;
  finalize(&pCur->pTableList);
  finalize(&pCur->pColumnList);
  finalize(&pCur->pDbList);
  rc = capdb_prepare(pVtab->db,"PRAGMA database_list", -1, &pCur->pDbList, 0);
  return (rc==CAPDB_OK ? schemaNext(pVtabCursor) : rc);
}

/*
** Analyse the WHERE condition.
*/
static int schemaBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  return CAPDB_OK;
}

/*
** A virtual table module that merely echos method calls into TCL
** variables.
*/
static capdb_module schemaModule = {
  0,                           /* iVersion */
  schemaCreate,
  schemaCreate,
  schemaBestIndex,
  schemaDestroy,
  schemaDestroy,
  schemaOpen,                  /* xOpen - open a cursor */
  schemaClose,                 /* xClose - close a cursor */
  schemaFilter,                /* xFilter - configure scan constraints */
  schemaNext,                  /* xNext - advance a cursor */
  schemaEof,                   /* xEof */
  schemaColumn,                /* xColumn - read data */
  schemaRowid,                 /* xRowid - read data */
  0,                           /* xUpdate */
  0,                           /* xBegin */
  0,                           /* xSync */
  0,                           /* xCommit */
  0,                           /* xRollback */
  0,                           /* xFindMethod */
  0,                           /* xRename */
  0,                           /* xSavepoint */
  0,                           /* xRelease */
  0,                           /* xRollbackTo */
  0,                           /* xShadowName */
  0                            /* xIntegrity */
};

#endif /* !defined(CAPDB_OMIT_VIRTUALTABLE) */

#ifdef CAPDB_TEST

/*
** Decode a pointer to an capdb object.
*/
extern int getDbPointer(Tcl_Interp *interp, const char *zA, capdb **ppDb);

/*
** Register the schema virtual table module.
*/
static int CAPDB_TCLAPI register_schema_module(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  capdb *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdb_create_module(db, "schema", &schemaModule, 0);
#endif
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetestschema_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
     void *clientData;
  } aObjCmd[] = {
     { "register_schema_module", register_schema_module, 0 },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, 
        aObjCmd[i].xProc, aObjCmd[i].clientData, 0);
  }
  return TCL_OK;
}

#else

/*
** Extension load function.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_schema_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  CAPDB_EXTENSION_INIT2(pApi);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdb_create_module(db, "schema", &schemaModule, 0);
#endif
  return 0;
}

#endif
