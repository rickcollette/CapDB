/*
** 2011 April 02
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
** This file implements a virtual table that returns the whole numbers
** between 1 and 4294967295, inclusive.
**
** TESTING AND DEBUG USE ONLY ->  This is not production code.  This is
** not a deliverable. Use the generate_series() virtual table for
** real-world applications instead of this extension.  THIS EXTENSION
** MAY CONTAIN BUGS.
**
** Example:
**
**     CREATE VIRTUAL TABLE nums USING wholenumber;
**     SELECT value FROM nums WHERE value<10;
**
** Results in:
**
**     1 2 3 4 5 6 7 8 9
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

#ifndef CAPDB_OMIT_VIRTUALTABLE


/* A wholenumber cursor object */
typedef struct wholenumber_cursor wholenumber_cursor;
struct wholenumber_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb_int64 iValue;      /* Current value */
  capdb_int64 mxValue;     /* Maximum value */
};

/* Methods for the wholenumber module */
static int wholenumberConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  capdb_vtab *pNew;
  pNew = *ppVtab = capdb_malloc64( sizeof(*pNew) );
  if( pNew==0 ) return CAPDB_NOMEM;
  capdb_declare_vtab(db, "CREATE TABLE x(value)");
  capdb_vtab_config(db, CAPDB_VTAB_INNOCUOUS);
  memset(pNew, 0, sizeof(*pNew));
  return CAPDB_OK;
}
/* Note that for this virtual table, the xCreate and xConnect
** methods are identical. */

static int wholenumberDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}
/* The xDisconnect and xDestroy methods are also the same */


/*
** Open a new wholenumber cursor.
*/
static int wholenumberOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  wholenumber_cursor *pCur;
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Close a wholenumber cursor.
*/
static int wholenumberClose(capdb_vtab_cursor *cur){
  capdb_free(cur);
  return CAPDB_OK;
}


/*
** Advance a cursor to its next row of output
*/
static int wholenumberNext(capdb_vtab_cursor *cur){
  wholenumber_cursor *pCur = (wholenumber_cursor*)cur;
  pCur->iValue++;
  return CAPDB_OK;
}

/*
** Return the value associated with a wholenumber.
*/
static int wholenumberColumn(
  capdb_vtab_cursor *cur,
  capdb_context *ctx,
  int i
){
  wholenumber_cursor *pCur = (wholenumber_cursor*)cur;
  capdb_result_int64(ctx, pCur->iValue);
  return CAPDB_OK;
}

/*
** The rowid.
*/
static int wholenumberRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  wholenumber_cursor *pCur = (wholenumber_cursor*)cur;
  *pRowid = pCur->iValue;
  return CAPDB_OK;
}

/*
** When the wholenumber_cursor.rLimit value is 0 or less, that is a signal
** that the cursor has nothing more to output.
*/
static int wholenumberEof(capdb_vtab_cursor *cur){
  wholenumber_cursor *pCur = (wholenumber_cursor*)cur;
  return pCur->iValue>pCur->mxValue || pCur->iValue==0;
}

/*
** Called to "rewind" a cursor back to the beginning so that
** it starts its output over again.  Always called at least once
** prior to any wholenumberColumn, wholenumberRowid, or wholenumberEof call.
**
**    idxNum   Constraints
**    ------   ---------------------
**      0      (none)
**      1      value > $argv0
**      2      value >= $argv0
**      4      value < $argv0
**      8      value <= $argv0
**
**      5      value > $argv0 AND value < $argv1
**      6      value >= $argv0 AND value < $argv1
**      9      value > $argv0 AND value <= $argv1
**     10      value >= $argv0 AND value <= $argv1
*/
static int wholenumberFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  wholenumber_cursor *pCur = (wholenumber_cursor *)pVtabCursor;
  capdb_int64 v;
  int i = 0;
  pCur->iValue = 1;
  pCur->mxValue = 0xffffffff;  /* 4294967295 */
  if( idxNum & 3 ){
    v = capdb_value_int64(argv[0]);
    if( v<=pCur->mxValue && (idxNum&1)!=0 ) v++;
    if( v>pCur->iValue && v<=pCur->mxValue ) pCur->iValue = v;
    i++;
  }
  if( idxNum & 12 ){
    v = capdb_value_int64(argv[i]);
    if( v>0 && ((idxNum>>2)&1)!=0 ) v--;
    if( v>=pCur->iValue && v<pCur->mxValue ) pCur->mxValue = v;
  }
  return CAPDB_OK;
}

/*
** Search for terms of these forms:
**
**  (1)  value > $value
**  (2)  value >= $value
**  (4)  value < $value
**  (8)  value <= $value
**
** idxNum is an ORed combination of 1 or 2 with 4 or 8.
*/
static int wholenumberBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  int i;
  int idxNum = 0;
  int argvIdx = 1;
  int ltIdx = -1;
  int gtIdx = -1;
  const struct capdb_index_constraint *pConstraint;
  pConstraint = pIdxInfo->aConstraint;
  for(i=0; i<pIdxInfo->nConstraint; i++, pConstraint++){
    if( pConstraint->usable==0 ) continue;
    if( (idxNum & 3)==0 && pConstraint->op==CAPDB_INDEX_CONSTRAINT_GT ){
      idxNum |= 1;
      ltIdx = i;
    }
    if( (idxNum & 3)==0 && pConstraint->op==CAPDB_INDEX_CONSTRAINT_GE ){
      idxNum |= 2;
      ltIdx = i;
    }
    if( (idxNum & 12)==0 && pConstraint->op==CAPDB_INDEX_CONSTRAINT_LT ){
      idxNum |= 4;
      gtIdx = i;
    }
    if( (idxNum & 12)==0 && pConstraint->op==CAPDB_INDEX_CONSTRAINT_LE ){
      idxNum |= 8;
      gtIdx = i;
    }
  }
  pIdxInfo->idxNum = idxNum;
  if( ltIdx>=0 ){
    pIdxInfo->aConstraintUsage[ltIdx].argvIndex = argvIdx++;
    pIdxInfo->aConstraintUsage[ltIdx].omit = 1;
  }
  if( gtIdx>=0 ){
    pIdxInfo->aConstraintUsage[gtIdx].argvIndex = argvIdx;
    pIdxInfo->aConstraintUsage[gtIdx].omit = 1;
  }
  if( pIdxInfo->nOrderBy==1
   && pIdxInfo->aOrderBy[0].desc==0
  ){
    pIdxInfo->orderByConsumed = 1;
  }
  if( (idxNum & 12)==0 ){
    pIdxInfo->estimatedCost = 1e99;
  }else if( (idxNum & 3)==0 ){
    pIdxInfo->estimatedCost = (double)5;
  }else{
    pIdxInfo->estimatedCost = (double)1;
  }
  return CAPDB_OK;
}

/*
** A virtual table module that provides read-only access to a
** Tcl global variable namespace.
*/
static capdb_module wholenumberModule = {
  0,                         /* iVersion */
  wholenumberConnect,
  wholenumberConnect,
  wholenumberBestIndex,
  wholenumberDisconnect, 
  wholenumberDisconnect,
  wholenumberOpen,           /* xOpen - open a cursor */
  wholenumberClose,          /* xClose - close a cursor */
  wholenumberFilter,         /* xFilter - configure scan constraints */
  wholenumberNext,           /* xNext - advance a cursor */
  wholenumberEof,            /* xEof - check for end of scan */
  wholenumberColumn,         /* xColumn - read data */
  wholenumberRowid,          /* xRowid - read data */
  0,                         /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindMethod */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  0,                         /* xShadowName */
  0                          /* xIntegrity */
};

#endif /* CAPDB_OMIT_VIRTUALTABLE */

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_wholenumber_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdb_create_module(db, "wholenumber", &wholenumberModule, 0);
#endif
  return rc;
}
