/*
** 2018-04-19
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
** This file implements a template virtual-table.
** Developers can make a copy of this file as a baseline for writing
** new virtual tables and/or table-valued functions.
**
** Steps for writing a new virtual table implementation:
**
**     (1)  Make a copy of this file.  Perhaps call it "mynewvtab.c"
**
**     (2)  Replace this header comment with something appropriate for
**          the new virtual table
**
**     (3)  Change every occurrence of "templatevtab" to some other string
**          appropriate for the new virtual table.  Ideally, the new string
**          should be the basename of the source file: "mynewvtab".  Also
**          globally change "TEMPLATEVTAB" to "MYNEWVTAB".
**
**     (4)  Run a test compilation to make sure the unmodified virtual
**          table works.
**
**     (5)  Begin making incremental changes, testing as you go, to evolve
**          the new virtual table to do what you want it to do.
**
** This template is minimal, in the sense that it uses only the required
** methods on the capdb_module object.  As a result, templatevtab is
** a read-only and eponymous-only table.  Those limitation can be removed
** by adding new methods.
**
** This template implements an eponymous-only virtual table with a rowid and
** two columns named "a" and "b".  The table as 10 rows with fixed integer
** values. Usage example:
**
**     SELECT rowid, a, b FROM templatevtab;
*/
#if !defined(SQLITEINT_H)
#include "capdbext.h"
#endif
CAPDB_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

/* templatevtab_vtab is a subclass of capdb_vtab which is
** underlying representation of the virtual table
*/
typedef struct templatevtab_vtab templatevtab_vtab;
struct templatevtab_vtab {
  capdb_vtab base;  /* Base class - must be first */
  /* Add new fields here, as necessary */
};

/* templatevtab_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct templatevtab_cursor templatevtab_cursor;
struct templatevtab_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  /* Insert new fields here.  For this templatevtab we only keep track
  ** of the rowid */
  capdb_int64 iRowid;      /* The rowid */
};

/*
** The templatevtabConnect() method is invoked to create a new
** template virtual table.
**
** Think of this routine as the constructor for templatevtab_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the templatevtab_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the capdb_declare_vtab() interface) what the
**        result set of queries against the virtual table will look like.
*/
static int templatevtabConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  templatevtab_vtab *pNew;
  int rc;

  rc = capdb_declare_vtab(db,
           "CREATE TABLE x(a,b)"
       );
  /* For convenience, define symbolic names for the index to each column. */
#define TEMPLATEVTAB_A  0
#define TEMPLATEVTAB_B  1
  if( rc==CAPDB_OK ){
    pNew = capdb_malloc64( sizeof(*pNew) );
    *ppVtab = (capdb_vtab*)pNew;
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
  }
  return rc;
}

/*
** This method is the destructor for templatevtab_vtab objects.
*/
static int templatevtabDisconnect(capdb_vtab *pVtab){
  templatevtab_vtab *p = (templatevtab_vtab*)pVtab;
  capdb_free(p);
  return CAPDB_OK;
}

/*
** Constructor for a new templatevtab_cursor object.
*/
static int templatevtabOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  templatevtab_cursor *pCur;
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Destructor for a templatevtab_cursor.
*/
static int templatevtabClose(capdb_vtab_cursor *cur){
  templatevtab_cursor *pCur = (templatevtab_cursor*)cur;
  capdb_free(pCur);
  return CAPDB_OK;
}


/*
** Advance a templatevtab_cursor to its next row of output.
*/
static int templatevtabNext(capdb_vtab_cursor *cur){
  templatevtab_cursor *pCur = (templatevtab_cursor*)cur;
  pCur->iRowid++;
  return CAPDB_OK;
}

/*
** Return values of columns for the row at which the templatevtab_cursor
** is currently pointing.
*/
static int templatevtabColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  templatevtab_cursor *pCur = (templatevtab_cursor*)cur;
  switch( i ){
    case TEMPLATEVTAB_A:
      capdb_result_int(ctx, 1000 + pCur->iRowid);
      break;
    default:
      assert( i==TEMPLATEVTAB_B );
      capdb_result_int(ctx, 2000 + pCur->iRowid);
      break;
  }
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int templatevtabRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  templatevtab_cursor *pCur = (templatevtab_cursor*)cur;
  *pRowid = pCur->iRowid;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int templatevtabEof(capdb_vtab_cursor *cur){
  templatevtab_cursor *pCur = (templatevtab_cursor*)cur;
  return pCur->iRowid>=10;
}

/*
** This method is called to "rewind" the templatevtab_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to templatevtabColumn() or templatevtabRowid() or 
** templatevtabEof().
*/
static int templatevtabFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  templatevtab_cursor *pCur = (templatevtab_cursor *)pVtabCursor;
  pCur->iRowid = 1;
  return CAPDB_OK;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int templatevtabBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  pIdxInfo->estimatedCost = (double)10;
  pIdxInfo->estimatedRows = 10;
  return CAPDB_OK;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static capdb_module templatevtabModule = {
  /* iVersion    */ 0,
  /* xCreate     */ 0,
  /* xConnect    */ templatevtabConnect,
  /* xBestIndex  */ templatevtabBestIndex,
  /* xDisconnect */ templatevtabDisconnect,
  /* xDestroy    */ 0,
  /* xOpen       */ templatevtabOpen,
  /* xClose      */ templatevtabClose,
  /* xFilter     */ templatevtabFilter,
  /* xNext       */ templatevtabNext,
  /* xEof        */ templatevtabEof,
  /* xColumn     */ templatevtabColumn,
  /* xRowid      */ templatevtabRowid,
  /* xUpdate     */ 0,
  /* xBegin      */ 0,
  /* xSync       */ 0,
  /* xCommit     */ 0,
  /* xRollback   */ 0,
  /* xFindMethod */ 0,
  /* xRename     */ 0,
  /* xSavepoint  */ 0,
  /* xRelease    */ 0,
  /* xRollbackTo */ 0,
  /* xShadowName */ 0,
  /* xIntegrity  */ 0
};


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_templatevtab_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  rc = capdb_create_module(db, "templatevtab", &templatevtabModule, 0);
  return rc;
}
