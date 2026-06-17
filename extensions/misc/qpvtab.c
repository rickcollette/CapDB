/*
** 2022-01-19
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
** This file implements a virtual-table that returns information about
** how the query planner called the xBestIndex method.  This virtual table
** is intended for testing and debugging only.
**
** The schema of the virtual table is this:
**
**    CREATE TABLE qpvtab(
**      vn     TEXT,           -- Name of an capdb_index_info field
**      ix     INTEGER,        -- Array index or value
**      cn     TEXT,           -- Column name
**      op     INTEGER,        -- operator
**      ux     BOOLEAN,        -- "usable" field
**      rhs    TEXT,           -- capdb_vtab_rhs_value()
**
**      a, b, c, d, e,         -- Extra columns to attach constraints to
**
**      flags    INTEGER HIDDEN  -- control flags
**    );
**
** The virtual table returns a description of the capdb_index_info object
** that was provided to the (successful) xBestIndex method.  There is one
** row in the result table for each field in the capdb_index_info object.
**
** The values of the "a" through "e" columns are one of:
**
**    1.   TEXT - the same as the column name
**    2.   INTEGER - 1 for "a", 2 for "b", and so forth
**
** Option 1 is the default behavior.  2 is use if there is a usable
** constraint on "flags" with an integer right-hand side that where the
** value of the right-hand side has its 0x001 bit set.
**
** All constraints on columns "a" through "e" are marked as "omit".
**
** If there is a usable constraint on "flags" that has a RHS value that
** is an integer and that integer has its 0x02 bit set, then the
** orderByConsumed flag is set.
**
** FLAGS SUMMARY:
**
**   0x001               Columns 'a' through 'e' have INT values
**   0x002               orderByConsumed is set
**   0x004               OFFSET and LIMIT have omit set
**
** COMPILE:
**
**   gcc -Wall -g -shared -fPIC -I. qpvtab.c -o qqvtab.so
**
** EXAMPLE USAGE:
**
**   .load ./qpvtab
**   SELECT rowid, *, flags FROM qpvtab(102)
**    WHERE a=19
**      AND b BETWEEN 4.5 and 'hello'
**      AND c<>x'aabbcc'
**    ORDER BY d, e DESC;
*/
#if !defined(SQLITEINT_H)
#include "capdbext.h"
#endif
CAPDB_EXTENSION_INIT1
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#if !defined(CAPDB_OMIT_VIRTUALTABLE)

/* qpvtab_vtab is a subclass of capdb_vtab which is
** underlying representation of the virtual table
*/
typedef struct qpvtab_vtab qpvtab_vtab;
struct qpvtab_vtab {
  capdb_vtab base;  /* Base class - must be first */
};

/* qpvtab_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct qpvtab_cursor qpvtab_cursor;
struct qpvtab_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb_int64 iRowid;      /* The rowid */
  const char *zData;         /* Data to return */
  int nData;                 /* Number of bytes of data */
  int flags;                 /* Flags value */
};

/*
** Names of columns
*/
static const char *azColname[] = {
  "vn",
  "ix",
  "cn",
  "op",
  "ux",
  "rhs",
  "a", "b", "c", "d", "e",
  "flags",
  ""
};

/*
** The qpvtabConnect() method is invoked to create a new
** qpvtab virtual table.
*/
static int qpvtabConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  qpvtab_vtab *pNew;
  int rc;

  rc = capdb_declare_vtab(db,
         "CREATE TABLE x("
         " vn TEXT,"
         " ix INT,"
         " cn TEXT,"
         " op INT,"
         " ux BOOLEAN,"
         " rhs TEXT,"
         " a, b, c, d, e,"
         " flags INT HIDDEN)"
       );
#define QPVTAB_VN      0
#define QPVTAB_IX      1
#define QPVTAB_CN      2
#define QPVTAB_OP      3
#define QPVTAB_UX      4
#define QPVTAB_RHS     5
#define QPVTAB_A       6
#define QPVTAB_B       7
#define QPVTAB_C       8
#define QPVTAB_D       9
#define QPVTAB_E      10
#define QPVTAB_FLAGS  11
#define QPVTAB_NONE   12
  if( rc==CAPDB_OK ){
    pNew = capdb_malloc64( sizeof(*pNew) );
    *ppVtab = (capdb_vtab*)pNew;
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
  }
  return rc;
}

/*
** This method is the destructor for qpvtab_vtab objects.
*/
static int qpvtabDisconnect(capdb_vtab *pVtab){
  qpvtab_vtab *p = (qpvtab_vtab*)pVtab;
  capdb_free(p);
  return CAPDB_OK;
}

/*
** Constructor for a new qpvtab_cursor object.
*/
static int qpvtabOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  qpvtab_cursor *pCur;
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Destructor for a qpvtab_cursor.
*/
static int qpvtabClose(capdb_vtab_cursor *cur){
  qpvtab_cursor *pCur = (qpvtab_cursor*)cur;
  capdb_free(pCur);
  return CAPDB_OK;
}


/*
** Advance a qpvtab_cursor to its next row of output.
*/
static int qpvtabNext(capdb_vtab_cursor *cur){
  qpvtab_cursor *pCur = (qpvtab_cursor*)cur;
  if( pCur->iRowid<pCur->nData ){
    const char *z = &pCur->zData[pCur->iRowid];
    const char *zEnd = strchr(z, '\n');
    if( zEnd ) zEnd++;
    pCur->iRowid = (int)(zEnd - pCur->zData);
  }
  return CAPDB_OK;
}

/*
** Return values of columns for the row at which the qpvtab_cursor
** is currently pointing.
*/
static int qpvtabColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  qpvtab_cursor *pCur = (qpvtab_cursor*)cur;
  if( i>=QPVTAB_VN && i<=QPVTAB_RHS && pCur->iRowid<pCur->nData ){
    const char *z = &pCur->zData[pCur->iRowid];
    const char *zEnd;
    int j;
    j = QPVTAB_VN;
    while(1){
      zEnd = strchr(z, j==QPVTAB_RHS ? '\n' : ',');
      if( j==i || zEnd==0 ) break;
      z = zEnd+1;
      j++;
    }
    if( zEnd==z ){
      capdb_result_null(ctx);
    }else if( i==QPVTAB_IX || i==QPVTAB_OP || i==QPVTAB_UX ){
      capdb_result_int(ctx, atoi(z));
    }else{
      capdb_result_text64(ctx, z, zEnd-z, CAPDB_TRANSIENT, CAPDB_UTF8);
    }
  }else if( i>=QPVTAB_A && i<=QPVTAB_E ){
    if( pCur->flags & 0x001 ){
      capdb_result_int(ctx, i-QPVTAB_A+1);
    }else{
      char x = 'a'+i-QPVTAB_A;
      capdb_result_text64(ctx, &x, 1, CAPDB_TRANSIENT, CAPDB_UTF8);
    }
  }else if( i==QPVTAB_FLAGS ){
    capdb_result_int(ctx, pCur->flags);
  }
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int qpvtabRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  qpvtab_cursor *pCur = (qpvtab_cursor*)cur;
  *pRowid = pCur->iRowid;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int qpvtabEof(capdb_vtab_cursor *cur){
  qpvtab_cursor *pCur = (qpvtab_cursor*)cur;
  return pCur->iRowid>=pCur->nData;
}

/*
** This method is called to "rewind" the qpvtab_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to qpvtabColumn() or qpvtabRowid() or 
** qpvtabEof().
*/
static int qpvtabFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  qpvtab_cursor *pCur = (qpvtab_cursor *)pVtabCursor;
  pCur->iRowid = 0;
  pCur->zData = idxStr;
  pCur->nData = (int)strlen(idxStr);
  pCur->flags = idxNum;
  return CAPDB_OK;
}

/*
** Append the text of a value to pStr
*/
static void qpvtabStrAppendValue(
  capdb_str *pStr,
  capdb_value *pVal
){
  switch( capdb_value_type(pVal) ){
    case CAPDB_NULL:
      capdb_str_appendf(pStr, "NULL");
      break;
    case CAPDB_INTEGER:
      capdb_str_appendf(pStr, "%lld", capdb_value_int64(pVal));
      break;
    case CAPDB_FLOAT:
      capdb_str_appendf(pStr, "%!f", capdb_value_double(pVal));
      break;
    case CAPDB_TEXT: {
      int i;
      const char *a = (const char*)capdb_value_text(pVal);
      int n = capdb_value_bytes(pVal);
      capdb_str_append(pStr, "'", 1);
      for(i=0; i<n; i++){
        char c = a[i];
        if( c=='\n' ) c = ' ';
        capdb_str_append(pStr, &c, 1);
        if( c=='\'' ) capdb_str_append(pStr, &c, 1);
      }
      capdb_str_append(pStr, "'", 1);
      break;
    }
    case CAPDB_BLOB: {
      int i;
      const unsigned char *a = capdb_value_blob(pVal);
      int n = capdb_value_bytes(pVal);
      capdb_str_append(pStr, "x'", 2);
      for(i=0; i<n; i++){
        capdb_str_appendf(pStr, "%02x", a[i]);
      }
      capdb_str_append(pStr, "'", 1);
      break;
    }
  }
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int qpvtabBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  capdb_str *pStr = capdb_str_new(0);
  int i, k = 0;
  int rc;
  capdb_str_appendf(pStr, "nConstraint,%d,,,,\n", pIdxInfo->nConstraint);
  for(i=0; i<pIdxInfo->nConstraint; i++){
    capdb_value *pVal;
    int iCol = pIdxInfo->aConstraint[i].iColumn;
    int op = pIdxInfo->aConstraint[i].op;
    if( iCol==QPVTAB_FLAGS &&  pIdxInfo->aConstraint[i].usable ){
      pVal = 0;
      rc = capdb_vtab_rhs_value(pIdxInfo, i, &pVal);
      assert( rc==CAPDB_OK || pVal==0 );
      if( pVal ){
        pIdxInfo->idxNum = capdb_value_int(pVal);
        if( pIdxInfo->idxNum & 0x002 ) pIdxInfo->orderByConsumed = 1;
      }
    }
    if( op==CAPDB_INDEX_CONSTRAINT_LIMIT
     || op==CAPDB_INDEX_CONSTRAINT_OFFSET
    ){
      iCol = QPVTAB_NONE;
    }
    capdb_str_appendf(pStr,"aConstraint,%d,%s,%d,%d,",
       i,
       iCol>=0 ? azColname[iCol] : "rowid",
       op,
       pIdxInfo->aConstraint[i].usable);
    pVal = 0;
    rc = capdb_vtab_rhs_value(pIdxInfo, i, &pVal);
    assert( rc==CAPDB_OK || pVal==0 );
    if( pVal ){
      qpvtabStrAppendValue(pStr, pVal);
    }
    capdb_str_append(pStr, "\n", 1);
  }
  for(i=0; i<pIdxInfo->nConstraint; i++){
    int iCol = pIdxInfo->aConstraint[i].iColumn;
    int op = pIdxInfo->aConstraint[i].op;
    if( op==CAPDB_INDEX_CONSTRAINT_LIMIT
     || op==CAPDB_INDEX_CONSTRAINT_OFFSET
    ){
      iCol = QPVTAB_NONE;
    }
    if( iCol>=QPVTAB_A && pIdxInfo->aConstraint[i].usable ){
      pIdxInfo->aConstraintUsage[i].argvIndex = ++k;
      if( iCol<=QPVTAB_FLAGS || (pIdxInfo->idxNum & 0x004)!=0 ){
        pIdxInfo->aConstraintUsage[i].omit = 1;
      }
    }
  }
  capdb_str_appendf(pStr, "nOrderBy,%d,,,,\n", pIdxInfo->nOrderBy);
  for(i=0; i<pIdxInfo->nOrderBy; i++){
    int iCol = pIdxInfo->aOrderBy[i].iColumn;
    capdb_str_appendf(pStr, "aOrderBy,%d,%s,%d,,\n",i,
      iCol>=0 ? azColname[iCol] : "rowid",
      pIdxInfo->aOrderBy[i].desc
    );
  }
  capdb_str_appendf(pStr, "capdb_vtab_distinct,%d,,,,\n", 
                      capdb_vtab_distinct(pIdxInfo));
  capdb_str_appendf(pStr, "idxFlags,%d,,,,\n", pIdxInfo->idxFlags);
  capdb_str_appendf(pStr, "colUsed,%d,,,,\n", (int)pIdxInfo->colUsed);
  pIdxInfo->estimatedCost = (double)10;
  pIdxInfo->estimatedRows = 10;
  capdb_str_appendf(pStr, "idxNum,%d,,,,\n", pIdxInfo->idxNum);
  capdb_str_appendf(pStr, "orderByConsumed,%d,,,,\n",
                      pIdxInfo->orderByConsumed);
  pIdxInfo->idxStr = capdb_str_finish(pStr);
  pIdxInfo->needToFreeIdxStr = 1;
  return CAPDB_OK;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static capdb_module qpvtabModule = {
  /* iVersion    */ 0,
  /* xCreate     */ 0,
  /* xConnect    */ qpvtabConnect,
  /* xBestIndex  */ qpvtabBestIndex,
  /* xDisconnect */ qpvtabDisconnect,
  /* xDestroy    */ 0,
  /* xOpen       */ qpvtabOpen,
  /* xClose      */ qpvtabClose,
  /* xFilter     */ qpvtabFilter,
  /* xNext       */ qpvtabNext,
  /* xEof        */ qpvtabEof,
  /* xColumn     */ qpvtabColumn,
  /* xRowid      */ qpvtabRowid,
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
#endif /* CAPDB_OMIT_VIRTUALTABLE */


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_qpvtab_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdb_create_module(db, "qpvtab", &qpvtabModule, 0);
#endif
  return rc;
}
