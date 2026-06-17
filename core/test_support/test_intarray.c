/*
** 2009 November 10
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
** This file implements a read-only VIRTUAL TABLE that contains the
** content of a C-language array of integer values.  See the corresponding
** header file for full details.
**
** This virtual table is used for internal testing of SQLite only.  It is
** not recommended for use in production.  For a similar virtual table that
** is production-ready, see the "carray" virtual table over in ext/misc.
*/
#include "test_intarray.h"
#include <string.h>
#include <assert.h>


/*
** Definition of the capdb_intarray object.
**
** The internal representation of an intarray object is subject
** to change, is not externally visible, and should be used by
** the implementation of intarray only.  This object is opaque
** to users.
*/
struct capdb_intarray {
  int n;                    /* Number of elements in the array */
  capdb_int64 *a;         /* Contents of the array */
  void (*xFree)(void*);     /* Function used to free a[] */
};

/* Objects used internally by the virtual table implementation */
typedef struct intarray_vtab intarray_vtab;
typedef struct intarray_cursor intarray_cursor;

/* An intarray table object */
struct intarray_vtab {
  capdb_vtab base;            /* Base class */
  capdb_intarray *pContent;   /* Content of the integer array */
};

/* An intarray cursor object */
struct intarray_cursor {
  capdb_vtab_cursor base;    /* Base class */
  int i;                       /* Current cursor position */
};

/*
** None of this works unless we have virtual tables.
*/
#ifndef CAPDB_OMIT_VIRTUALTABLE

/*
** Free an capdb_intarray object.
*/
static void intarrayFree(void *pX){
  capdb_intarray *p = (capdb_intarray*)pX;
  if( p->xFree ){
    p->xFree(p->a);
  }
  capdb_free(p);
}

/*
** Table destructor for the intarray module.
*/
static int intarrayDestroy(capdb_vtab *p){
  intarray_vtab *pVtab = (intarray_vtab*)p;
  capdb_free(pVtab);
  return 0;
}

/*
** Table constructor for the intarray module.
*/
static int intarrayCreate(
  capdb *db,              /* Database where module is created */
  void *pAux,               /* clientdata for the module */
  int argc,                 /* Number of arguments */
  const char *const*argv,   /* Value for all arguments */
  capdb_vtab **ppVtab,    /* Write the new virtual table object here */
  char **pzErr              /* Put error message text here */
){
  int rc = CAPDB_NOMEM;
  intarray_vtab *pVtab = capdb_malloc64(sizeof(intarray_vtab));

  if( pVtab ){
    memset(pVtab, 0, sizeof(intarray_vtab));
    pVtab->pContent = (capdb_intarray*)pAux;
    rc = capdb_declare_vtab(db, "CREATE TABLE x(value INTEGER PRIMARY KEY)");
  }
  *ppVtab = (capdb_vtab *)pVtab;
  return rc;
}

/*
** Open a new cursor on the intarray table.
*/
static int intarrayOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  int rc = CAPDB_NOMEM;
  intarray_cursor *pCur;
  pCur = capdb_malloc64(sizeof(intarray_cursor));
  if( pCur ){
    memset(pCur, 0, sizeof(intarray_cursor));
    *ppCursor = (capdb_vtab_cursor *)pCur;
    rc = CAPDB_OK;
  }
  return rc;
}

/*
** Close a intarray table cursor.
*/
static int intarrayClose(capdb_vtab_cursor *cur){
  intarray_cursor *pCur = (intarray_cursor *)cur;
  capdb_free(pCur);
  return CAPDB_OK;
}

/*
** Retrieve a column of data.
*/
static int intarrayColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  intarray_cursor *pCur = (intarray_cursor*)cur;
  intarray_vtab *pVtab = (intarray_vtab*)cur->pVtab;
  if( pCur->i>=0 && pCur->i<pVtab->pContent->n ){
    capdb_result_int64(ctx, pVtab->pContent->a[pCur->i]);
  }
  return CAPDB_OK;
}

/*
** Retrieve the current rowid.
*/
static int intarrayRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  intarray_cursor *pCur = (intarray_cursor *)cur;
  *pRowid = pCur->i;
  return CAPDB_OK;
}

static int intarrayEof(capdb_vtab_cursor *cur){
  intarray_cursor *pCur = (intarray_cursor *)cur;
  intarray_vtab *pVtab = (intarray_vtab *)cur->pVtab;
  return pCur->i>=pVtab->pContent->n;
}

/*
** Advance the cursor to the next row.
*/
static int intarrayNext(capdb_vtab_cursor *cur){
  intarray_cursor *pCur = (intarray_cursor *)cur;
  pCur->i++;
  return CAPDB_OK;
}

/*
** Reset a intarray table cursor.
*/
static int intarrayFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  intarray_cursor *pCur = (intarray_cursor *)pVtabCursor;
  pCur->i = 0;
  return CAPDB_OK;
}

/*
** Analyse the WHERE condition.
*/
static int intarrayBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  return CAPDB_OK;
}

/*
** A virtual table module that merely echos method calls into TCL
** variables.
*/
static capdb_module intarrayModule = {
  0,                           /* iVersion */
  intarrayCreate,              /* xCreate - create a new virtual table */
  intarrayCreate,              /* xConnect - connect to an existing vtab */
  intarrayBestIndex,           /* xBestIndex - find the best query index */
  intarrayDestroy,             /* xDisconnect - disconnect a vtab */
  intarrayDestroy,             /* xDestroy - destroy a vtab */
  intarrayOpen,                /* xOpen - open a cursor */
  intarrayClose,               /* xClose - close a cursor */
  intarrayFilter,              /* xFilter - configure scan constraints */
  intarrayNext,                /* xNext - advance a cursor */
  intarrayEof,                 /* xEof */
  intarrayColumn,              /* xColumn - read data */
  intarrayRowid,               /* xRowid - read data */
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

/*
** Invoke this routine to create a specific instance of an intarray object.
** The new intarray object is returned by the 3rd parameter.
**
** Each intarray object corresponds to a virtual table in the TEMP table
** with a name of zName.
**
** Destroy the intarray object by dropping the virtual table.  If not done
** explicitly by the application, the virtual table will be dropped implicitly
** by the system when the database connection is closed.
*/
CAPDB_API int capdb_intarray_create(
  capdb *db,
  const char *zName,
  capdb_intarray **ppReturn
){
  int rc = CAPDB_OK;
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdb_intarray *p;

  *ppReturn = p = capdb_malloc64( sizeof(*p) );
  if( p==0 ){
    return CAPDB_NOMEM;
  }
  memset(p, 0, sizeof(*p));
  rc = capdb_create_module_v2(db, zName, &intarrayModule, p,
                                (void(*)(void*))intarrayFree);
  if( rc==CAPDB_OK ){
    char *zSql;
    zSql = capdb_mprintf("CREATE VIRTUAL TABLE temp.%Q USING %Q",
                           zName, zName);
    rc = capdb_exec(db, zSql, 0, 0, 0);
    capdb_free(zSql);
  }
#endif
  return rc;
}

/*
** Bind a new array array of integers to a specific intarray object.
**
** The array of integers bound must be unchanged for the duration of
** any query against the corresponding virtual table.  If the integer
** array does change or is deallocated undefined behavior will result.
*/
CAPDB_API int capdb_intarray_bind(
  capdb_intarray *pIntArray,   /* The intarray object to bind to */
  int nElements,                 /* Number of elements in the intarray */
  capdb_int64 *aElements,      /* Content of the intarray */
  void (*xFree)(void*)           /* How to dispose of the intarray when done */
){
  if( pIntArray->xFree ){
    pIntArray->xFree(pIntArray->a);
  }
  pIntArray->n = nElements;
  pIntArray->a = aElements;
  pIntArray->xFree = xFree;
  return CAPDB_OK;
}


/*****************************************************************************
** Everything below is interface for testing this module.
*/
#ifdef CAPDB_TEST
#include "tclsqlite.h"

/*
** Routines to encode and decode pointers
*/
extern int getDbPointer(Tcl_Interp *interp, const char *zA, capdb **ppDb);
extern void *capdbTestTextToPtr(const char*);
extern int capdbTestMakePointerStr(Tcl_Interp*, char *zPtr, void*);
extern const char *capdbErrName(int);

/*
**    capdb_intarray_create  DB  NAME
**
** Invoke the capdb_intarray_create interface.  A string that becomes
** the first parameter to capdb_intarray_bind.
*/
static int CAPDB_TCLAPI test_intarray_create(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  capdb *db;
  const char *zName;
  capdb_intarray *pArray;
  int rc = CAPDB_OK;
  char zPtr[100];

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  zName = Tcl_GetString(objv[2]);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdb_intarray_create(db, zName, &pArray);
#endif
  if( rc!=CAPDB_OK ){
    Tcl_AppendResult(interp, capdbErrName(rc), (char*)0);
    return TCL_ERROR;
  }
  capdbTestMakePointerStr(interp, zPtr, pArray);
  Tcl_AppendResult(interp, zPtr, (char*)0);
  return TCL_OK;
}

/*
**    capdb_intarray_bind  INTARRAY  ?VALUE ...?
**
** Invoke the capdb_intarray_bind interface on the given array of integers.
*/
static int CAPDB_TCLAPI test_intarray_bind(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  capdb_intarray *pArray;
  int rc = CAPDB_OK;
  int i, n;
  capdb_int64 *a;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "INTARRAY");
    return TCL_ERROR;
  }
  pArray = (capdb_intarray*)capdbTestTextToPtr(Tcl_GetString(objv[1]));
  n = objc - 2;
#ifndef CAPDB_OMIT_VIRTUALTABLE
  a = capdb_malloc64( sizeof(a[0])*n );
  if( a==0 ){
    Tcl_AppendResult(interp, "CAPDB_NOMEM", (char*)0);
    return TCL_ERROR;
  }
  for(i=0; i<n; i++){
    Tcl_WideInt x = 0;
    Tcl_GetWideIntFromObj(0, objv[i+2], &x);
    a[i] = x;
  }
  rc = capdb_intarray_bind(pArray, n, a, capdb_free);
  if( rc!=CAPDB_OK ){
    Tcl_AppendResult(interp, capdbErrName(rc), (char*)0);
    return TCL_ERROR;
  }
#endif
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetestintarray_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
     void *clientData;
  } aObjCmd[] = {
     { "capdb_intarray_create", test_intarray_create, 0 },
     { "capdb_intarray_bind", test_intarray_bind, 0 },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, 
        aObjCmd[i].xProc, aObjCmd[i].clientData, 0);
  }
  return TCL_OK;
}

#endif /* CAPDB_TEST */
