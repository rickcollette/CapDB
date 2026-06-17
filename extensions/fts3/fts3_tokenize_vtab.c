/*
** 2013 Apr 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code for the "fts3tokenize" virtual table module.
** An fts3tokenize virtual table is created as follows:
**
**   CREATE VIRTUAL TABLE <tbl> USING fts3tokenize(
**       <tokenizer-name>, <arg-1>, ...
**   );
**
** The table created has the following schema:
**
**   CREATE TABLE <tbl>(input, token, start, end, position)
**
** When queried, the query must include a WHERE clause of type:
**
**   input = <string>
**
** The virtual table module tokenizes this <string>, using the FTS3 
** tokenizer specified by the arguments to the CREATE VIRTUAL TABLE 
** statement and returns one row for each token in the result. With
** fields set as follows:
**
**   input:   Always set to a copy of <string>
**   token:   A token from the input.
**   start:   Byte offset of the token within the input <string>.
**   end:     Byte offset of the byte immediately following the end of the
**            token within the input string.
**   pos:     Token offset of token within input.
**
*/
#include "fts3Int.h"
#if !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS3)

#include <string.h>
#include <assert.h>

typedef struct Fts3tokTable Fts3tokTable;
typedef struct Fts3tokCursor Fts3tokCursor;

/*
** Virtual table structure.
*/
struct Fts3tokTable {
  capdb_vtab base;              /* Base class used by SQLite core */
  const capdb_tokenizer_module *pMod;
  capdb_tokenizer *pTok;
};

/*
** Virtual table cursor structure.
*/
struct Fts3tokCursor {
  capdb_vtab_cursor base;       /* Base class used by SQLite core */
  char *zInput;                   /* Input string */
  capdb_tokenizer_cursor *pCsr; /* Cursor to iterate through zInput */
  int iRowid;                     /* Current 'rowid' value */
  const char *zToken;             /* Current 'token' value */
  int nToken;                     /* Size of zToken in bytes */
  int iStart;                     /* Current 'start' value */
  int iEnd;                       /* Current 'end' value */
  int iPos;                       /* Current 'pos' value */
};

/*
** Query FTS for the tokenizer implementation named zName.
*/
static int fts3tokQueryTokenizer(
  Fts3Hash *pHash,
  const char *zName,
  const capdb_tokenizer_module **pp,
  char **pzErr
){
  capdb_tokenizer_module *p;
  int nName = (int)strlen(zName);

  p = (capdb_tokenizer_module *)capdbFts3HashFind(pHash, zName, nName+1);
  if( !p ){
    capdbFts3ErrMsg(pzErr, "unknown tokenizer: %s", zName);
    return CAPDB_ERROR;
  }

  *pp = p;
  return CAPDB_OK;
}

/*
** The second argument, argv[], is an array of pointers to nul-terminated
** strings. This function makes a copy of the array and strings into a 
** single block of memory. It then dequotes any of the strings that appear
** to be quoted.
**
** If successful, output parameter *pazDequote is set to point at the
** array of dequoted strings and CAPDB_OK is returned. The caller is
** responsible for eventually calling capdb_free() to free the array
** in this case. Or, if an error occurs, an SQLite error code is returned.
** The final value of *pazDequote is undefined in this case.
*/
static int fts3tokDequoteArray(
  int argc,                       /* Number of elements in argv[] */
  const char * const *argv,       /* Input array */
  char ***pazDequote              /* Output array */
){
  int rc = CAPDB_OK;             /* Return code */
  if( argc==0 ){
    *pazDequote = 0;
  }else{
    int i;
    int nByte = 0;
    char **azDequote;

    for(i=0; i<argc; i++){
      nByte += (int)(strlen(argv[i]) + 1);
    }

    *pazDequote = azDequote = capdb_malloc64(sizeof(char *)*argc + nByte);
    if( azDequote==0 ){
      rc = CAPDB_NOMEM;
    }else{
      char *pSpace = (char *)&azDequote[argc];
      for(i=0; i<argc; i++){
        int n = (int)strlen(argv[i]);
        azDequote[i] = pSpace;
        memcpy(pSpace, argv[i], n+1);
        capdbFts3Dequote(pSpace);
        pSpace += (n+1);
      }
    }
  }

  return rc;
}

/*
** Schema of the tokenizer table.
*/
#define FTS3_TOK_SCHEMA "CREATE TABLE x(input, token, start, end, position)"

/*
** This function does all the work for both the xConnect and xCreate methods.
** These tables have no persistent representation of their own, so xConnect
** and xCreate are identical operations.
**
**   argv[0]: module name
**   argv[1]: database name 
**   argv[2]: table name
**   argv[3]: first argument (tokenizer name)
*/
static int fts3tokConnectMethod(
  capdb *db,                    /* Database connection */
  void *pHash,                    /* Hash table of tokenizers */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  capdb_vtab **ppVtab,          /* OUT: New capdb_vtab object */
  char **pzErr                    /* OUT: capdb_malloc'd error message */
){
  Fts3tokTable *pTab = 0;
  const capdb_tokenizer_module *pMod = 0;
  capdb_tokenizer *pTok = 0;
  int rc;
  char **azDequote = 0;
  int nDequote;

  rc = capdb_declare_vtab(db, FTS3_TOK_SCHEMA);
  if( rc!=CAPDB_OK ) return rc;

  nDequote = argc-3;
  rc = fts3tokDequoteArray(nDequote, &argv[3], &azDequote);

  if( rc==CAPDB_OK ){
    const char *zModule;
    if( nDequote<1 ){
      zModule = "simple";
    }else{
      zModule = azDequote[0];
    }
    rc = fts3tokQueryTokenizer((Fts3Hash*)pHash, zModule, &pMod, pzErr);
  }

  assert( (rc==CAPDB_OK)==(pMod!=0) );
  if( rc==CAPDB_OK ){
    const char * const *azArg = 0;
    if( nDequote>1 ) azArg = (const char * const *)&azDequote[1];
    rc = pMod->xCreate((nDequote>1 ? nDequote-1 : 0), azArg, &pTok);
  }

  if( rc==CAPDB_OK ){
    pTab = (Fts3tokTable *)capdb_malloc(sizeof(Fts3tokTable));
    if( pTab==0 ){
      rc = CAPDB_NOMEM;
    }
  }

  if( rc==CAPDB_OK ){
    memset(pTab, 0, sizeof(Fts3tokTable));
    pTab->pMod = pMod;
    pTab->pTok = pTok;
    *ppVtab = &pTab->base;
  }else{
    if( pTok ){
      pMod->xDestroy(pTok);
    }
  }

  capdb_free(azDequote);
  return rc;
}

/*
** This function does the work for both the xDisconnect and xDestroy methods.
** These tables have no persistent representation of their own, so xDisconnect
** and xDestroy are identical operations.
*/
static int fts3tokDisconnectMethod(capdb_vtab *pVtab){
  Fts3tokTable *pTab = (Fts3tokTable *)pVtab;

  pTab->pMod->xDestroy(pTab->pTok);
  capdb_free(pTab);
  return CAPDB_OK;
}

/*
** xBestIndex - Analyze a WHERE and ORDER BY clause.
*/
static int fts3tokBestIndexMethod(
  capdb_vtab *pVTab, 
  capdb_index_info *pInfo
){
  int i;
  UNUSED_PARAMETER(pVTab);

  for(i=0; i<pInfo->nConstraint; i++){
    if( pInfo->aConstraint[i].usable 
     && pInfo->aConstraint[i].iColumn==0 
     && pInfo->aConstraint[i].op==CAPDB_INDEX_CONSTRAINT_EQ 
    ){
      pInfo->idxNum = 1;
      pInfo->aConstraintUsage[i].argvIndex = 1;
      pInfo->aConstraintUsage[i].omit = 1;
      pInfo->estimatedCost = 1;
      return CAPDB_OK;
    }
  }

  pInfo->idxNum = 0;
  assert( pInfo->estimatedCost>1000000.0 );

  return CAPDB_OK;
}

/*
** xOpen - Open a cursor.
*/
static int fts3tokOpenMethod(capdb_vtab *pVTab, capdb_vtab_cursor **ppCsr){
  Fts3tokCursor *pCsr;
  UNUSED_PARAMETER(pVTab);

  pCsr = (Fts3tokCursor *)capdb_malloc(sizeof(Fts3tokCursor));
  if( pCsr==0 ){
    return CAPDB_NOMEM;
  }
  memset(pCsr, 0, sizeof(Fts3tokCursor));

  *ppCsr = (capdb_vtab_cursor *)pCsr;
  return CAPDB_OK;
}

/*
** Reset the tokenizer cursor passed as the only argument. As if it had
** just been returned by fts3tokOpenMethod().
*/
static void fts3tokResetCursor(Fts3tokCursor *pCsr){
  if( pCsr->pCsr ){
    Fts3tokTable *pTab = (Fts3tokTable *)(pCsr->base.pVtab);
    pTab->pMod->xClose(pCsr->pCsr);
    pCsr->pCsr = 0;
  }
  capdb_free(pCsr->zInput);
  pCsr->zInput = 0;
  pCsr->zToken = 0;
  pCsr->nToken = 0;
  pCsr->iStart = 0;
  pCsr->iEnd = 0;
  pCsr->iPos = 0;
  pCsr->iRowid = 0;
}

/*
** xClose - Close a cursor.
*/
static int fts3tokCloseMethod(capdb_vtab_cursor *pCursor){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;

  fts3tokResetCursor(pCsr);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** xNext - Advance the cursor to the next row, if any.
*/
static int fts3tokNextMethod(capdb_vtab_cursor *pCursor){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  Fts3tokTable *pTab = (Fts3tokTable *)(pCursor->pVtab);
  int rc;                         /* Return code */

  pCsr->iRowid++;
  rc = pTab->pMod->xNext(pCsr->pCsr,
      &pCsr->zToken, &pCsr->nToken,
      &pCsr->iStart, &pCsr->iEnd, &pCsr->iPos
  );

  if( rc!=CAPDB_OK ){
    fts3tokResetCursor(pCsr);
    if( rc==CAPDB_DONE ) rc = CAPDB_OK;
  }

  return rc;
}

/*
** xFilter - Initialize a cursor to point at the start of its data.
*/
static int fts3tokFilterMethod(
  capdb_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  capdb_value **apVal           /* Arguments for the indexing scheme */
){
  int rc = CAPDB_ERROR;
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  Fts3tokTable *pTab = (Fts3tokTable *)(pCursor->pVtab);
  UNUSED_PARAMETER(idxStr);
  UNUSED_PARAMETER(nVal);

  fts3tokResetCursor(pCsr);
  if( idxNum==1 ){
    const char *zByte = (const char *)capdb_value_text(apVal[0]);
    capdb_int64 nByte = capdb_value_bytes(apVal[0]);
    pCsr->zInput = capdb_malloc64(nByte+1);
    if( pCsr->zInput==0 ){
      rc = CAPDB_NOMEM;
    }else{
      if( nByte>0 ) memcpy(pCsr->zInput, zByte, nByte);
      pCsr->zInput[nByte] = 0;
      rc = pTab->pMod->xOpen(pTab->pTok, pCsr->zInput, nByte, &pCsr->pCsr);
      if( rc==CAPDB_OK ){
        pCsr->pCsr->pTokenizer = pTab->pTok;
      }
    }
  }

  if( rc!=CAPDB_OK ) return rc;
  return fts3tokNextMethod(pCursor);
}

/*
** xEof - Return true if the cursor is at EOF, or false otherwise.
*/
static int fts3tokEofMethod(capdb_vtab_cursor *pCursor){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  return (pCsr->zToken==0);
}

/*
** xColumn - Return a column value.
*/
static int fts3tokColumnMethod(
  capdb_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  capdb_context *pCtx,          /* Context for capdb_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;

  /* CREATE TABLE x(input, token, start, end, position) */
  switch( iCol ){
    case 0:
      capdb_result_text(pCtx, pCsr->zInput, -1, CAPDB_TRANSIENT);
      break;
    case 1:
      capdb_result_text(pCtx, pCsr->zToken, pCsr->nToken, CAPDB_TRANSIENT);
      break;
    case 2:
      capdb_result_int(pCtx, pCsr->iStart);
      break;
    case 3:
      capdb_result_int(pCtx, pCsr->iEnd);
      break;
    default:
      assert( iCol==4 );
      capdb_result_int(pCtx, pCsr->iPos);
      break;
  }
  return CAPDB_OK;
}

/*
** xRowid - Return the current rowid for the cursor.
*/
static int fts3tokRowidMethod(
  capdb_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite_int64 *pRowid            /* OUT: Rowid value */
){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  *pRowid = (capdb_int64)pCsr->iRowid;
  return CAPDB_OK;
}

/*
** Register the fts3tok module with database connection db. Return CAPDB_OK
** if successful or an error code if capdb_create_module() fails.
*/
int capdbFts3InitTok(capdb *db, Fts3Hash *pHash, void(*xDestroy)(void*)){
  static const capdb_module fts3tok_module = {
     0,                           /* iVersion      */
     fts3tokConnectMethod,        /* xCreate       */
     fts3tokConnectMethod,        /* xConnect      */
     fts3tokBestIndexMethod,      /* xBestIndex    */
     fts3tokDisconnectMethod,     /* xDisconnect   */
     fts3tokDisconnectMethod,     /* xDestroy      */
     fts3tokOpenMethod,           /* xOpen         */
     fts3tokCloseMethod,          /* xClose        */
     fts3tokFilterMethod,         /* xFilter       */
     fts3tokNextMethod,           /* xNext         */
     fts3tokEofMethod,            /* xEof          */
     fts3tokColumnMethod,         /* xColumn       */
     fts3tokRowidMethod,          /* xRowid        */
     0,                           /* xUpdate       */
     0,                           /* xBegin        */
     0,                           /* xSync         */
     0,                           /* xCommit       */
     0,                           /* xRollback     */
     0,                           /* xFindFunction */
     0,                           /* xRename       */
     0,                           /* xSavepoint    */
     0,                           /* xRelease      */
     0,                           /* xRollbackTo   */
     0,                           /* xShadowName   */
     0                            /* xIntegrity    */
  };
  int rc;                         /* Return code */

  rc = capdb_create_module_v2(
      db, "fts3tokenize", &fts3tok_module, (void*)pHash, xDestroy
  );
  return rc;
}

#endif /* !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS3) */
