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
** This file implements a table-valued function:
**
**      prefixes('abcdefg')
**
** The function has a single (non-HIDDEN) column named prefix that takes
** on all prefixes of the string in its argument, including an empty string
** and the input string itself.  The order of prefixes is from longest
** to shortest.
*/
#if !defined(CAPDB_CORE) || !defined(CAPDB_OMIT_VIRTUALTABLE)
#if !defined(SQLITEINT_H)
#include "capdbext.h"
#endif
CAPDB_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

/* prefixes_vtab is a subclass of capdb_vtab which is
** underlying representation of the virtual table
*/
typedef struct prefixes_vtab prefixes_vtab;
struct prefixes_vtab {
  capdb_vtab base;  /* Base class - must be first */
  /* No additional fields are necessary */
};

/* prefixes_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct prefixes_cursor prefixes_cursor;
struct prefixes_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb_int64 iRowid;      /* The rowid */
  char *zStr;                /* Original string to be prefixed */
  int nStr;                  /* Length of the string in bytes */
};

/*
** The prefixesConnect() method is invoked to create a new
** template virtual table.
**
** Think of this routine as the constructor for prefixes_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the prefixes_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the capdb_declare_vtab() interface) what the
**        result set of queries against the virtual table will look like.
*/
static int prefixesConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  prefixes_vtab *pNew;
  int rc;

  rc = capdb_declare_vtab(db,
           "CREATE TABLE prefixes(prefix TEXT, original_string TEXT HIDDEN)"
       );
  if( rc==CAPDB_OK ){
    pNew = capdb_malloc64( sizeof(*pNew) );
    *ppVtab = (capdb_vtab*)pNew;
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    capdb_vtab_config(db, CAPDB_VTAB_INNOCUOUS);
  }
  return rc;
}

/*
** This method is the destructor for prefixes_vtab objects.
*/
static int prefixesDisconnect(capdb_vtab *pVtab){
  prefixes_vtab *p = (prefixes_vtab*)pVtab;
  capdb_free(p);
  return CAPDB_OK;
}

/*
** Constructor for a new prefixes_cursor object.
*/
static int prefixesOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  prefixes_cursor *pCur;
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Destructor for a prefixes_cursor.
*/
static int prefixesClose(capdb_vtab_cursor *cur){
  prefixes_cursor *pCur = (prefixes_cursor*)cur;
  capdb_free(pCur->zStr);
  capdb_free(pCur);
  return CAPDB_OK;
}


/*
** Advance a prefixes_cursor to its next row of output.
*/
static int prefixesNext(capdb_vtab_cursor *cur){
  prefixes_cursor *pCur = (prefixes_cursor*)cur;
  pCur->iRowid++;
  return CAPDB_OK;
}

/*
** Return values of columns for the row at which the prefixes_cursor
** is currently pointing.
*/
static int prefixesColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  prefixes_cursor *pCur = (prefixes_cursor*)cur;
  switch( i ){
    case 0:
      capdb_result_text(ctx, pCur->zStr, pCur->nStr - (int)pCur->iRowid,
                          0); 
      break;
    default:
      capdb_result_text(ctx, pCur->zStr, pCur->nStr, 0);
      break;
  }
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int prefixesRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  prefixes_cursor *pCur = (prefixes_cursor*)cur;
  *pRowid = pCur->iRowid;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int prefixesEof(capdb_vtab_cursor *cur){
  prefixes_cursor *pCur = (prefixes_cursor*)cur;
  return pCur->iRowid>pCur->nStr;
}

/*
** This method is called to "rewind" the prefixes_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to prefixesColumn() or prefixesRowid() or 
** prefixesEof().
*/
static int prefixesFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  prefixes_cursor *pCur = (prefixes_cursor *)pVtabCursor;
  capdb_free(pCur->zStr);
  if( argc>0 ){
    pCur->zStr = capdb_mprintf("%s", capdb_value_text(argv[0]));
    pCur->nStr = pCur->zStr ? (int)strlen(pCur->zStr) : 0;
  }else{
    pCur->zStr = 0;
    pCur->nStr = 0;
  }
  pCur->iRowid = 0;
  return CAPDB_OK;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int prefixesBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  /* Search for a usable equality constraint against column 1 
  ** (original_string) and use it if at all possible */
  int i;
  const struct capdb_index_constraint *p;

  for(i=0, p=pIdxInfo->aConstraint; i<pIdxInfo->nConstraint; i++, p++){
    if( p->iColumn!=1 ) continue;
    if( p->op!=CAPDB_INDEX_CONSTRAINT_EQ ) continue;
    if( !p->usable ) continue;
    pIdxInfo->aConstraintUsage[i].argvIndex = 1;
    pIdxInfo->aConstraintUsage[i].omit = 1;
    pIdxInfo->estimatedCost = (double)10;
    pIdxInfo->estimatedRows = 10;
    return CAPDB_OK;
  }
  pIdxInfo->estimatedCost = (double)1000000000;
  pIdxInfo->estimatedRows = 1000000000;
  return CAPDB_OK;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static capdb_module prefixesModule = {
  /* iVersion    */ 0,
  /* xCreate     */ 0,
  /* xConnect    */ prefixesConnect,
  /* xBestIndex  */ prefixesBestIndex,
  /* xDisconnect */ prefixesDisconnect,
  /* xDestroy    */ 0,
  /* xOpen       */ prefixesOpen,
  /* xClose      */ prefixesClose,
  /* xFilter     */ prefixesFilter,
  /* xNext       */ prefixesNext,
  /* xEof        */ prefixesEof,
  /* xColumn     */ prefixesColumn,
  /* xRowid      */ prefixesRowid,
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

/*
** This is a copy of the CAPDB_SKIP_UTF8(zIn) macro in capdbInt.h.
**
** Assuming zIn points to the first byte of a UTF-8 character,
** advance zIn to point to the first byte of the next UTF-8 character.
*/
#define PREFIX_SKIP_UTF8(zIn) {                        \
  if( (*(zIn++))>=0xc0 ){                              \
    while( (*zIn & 0xc0)==0x80 ){ zIn++; }             \
  }                                                    \
}

/*
** Implementation of function prefix_length(). This function accepts two
** strings as arguments and returns the length in characters (not bytes), 
** of the longest prefix shared by the two strings. For example:
**
**   prefix_length('abcdxxx', 'abcyy') == 3
**   prefix_length('abcdxxx', 'bcyyy') == 0
**   prefix_length('abcdxxx', 'ab')    == 2
**   prefix_length('ab',      'abcd')  == 2
**
** This function assumes the input is well-formed utf-8. If it is not,
** it is possible for this function to return -1.
*/
static void prefixLengthFunc(
  capdb_context *ctx,
  int nVal,
  capdb_value **apVal
){
  int nByte;                      /* Number of bytes to compare */
  int nRet = 0;                   /* Return value */
  const unsigned char *zL = capdb_value_text(apVal[0]);
  const unsigned char *zR = capdb_value_text(apVal[1]);
  int nL = capdb_value_bytes(apVal[0]);
  int nR = capdb_value_bytes(apVal[1]);
  int i;
  if( zL==0 || zR==0 ){
    capdb_result_int(ctx, 0);
    return;
  }

  nByte = (nL > nR ? nL : nR);
  for(i=0; i<nByte; i++){
    if( zL[i]==0 || zL[i]!=zR[i] ) break;
    if( (zL[i] & 0xC0)!=0x80 ) nRet++;
  }

  if( (zL[i] & 0xC0)==0x80 ) nRet--;
  capdb_result_int(ctx, nRet);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_prefixes_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  rc = capdb_create_module(db, "prefixes", &prefixesModule, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(
        db, "prefix_length", 2, CAPDB_UTF8, 0, prefixLengthFunc, 0, 0
    );
  }
  return rc;
}
#endif /* !defined(CAPDB_CORE) || !defined(CAPDB_OMIT_VIRTUALTABLE) */
