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
** This file contains code for the "fts5tokenize" virtual table module.
** An fts5tokenize virtual table is created as follows:
**
**   CREATE VIRTUAL TABLE <tbl> USING fts5tokenize(
**       <tokenizer-name>, <arg-1>, ...
**   );
**
** The table created has the following schema:
**
**   CREATE TABLE <tbl>(input HIDDEN, token, start, end, position)
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
#if defined(CAPDB_TEST) && defined(CAPDB_ENABLE_FTS5)

#include "fts5.h"
#include <string.h>
#include <assert.h>

typedef struct Fts5tokTable Fts5tokTable;
typedef struct Fts5tokCursor Fts5tokCursor;
typedef struct Fts5tokRow Fts5tokRow;

/*
** Virtual table structure.
*/
struct Fts5tokTable {
  capdb_vtab base;              /* Base class used by SQLite core */
  fts5_tokenizer tok;             /* Tokenizer functions */
  Fts5Tokenizer *pTok;            /* Tokenizer instance */
};

/*
** A container for a rows values.
*/
struct Fts5tokRow {
  char *zToken;
  int iStart;
  int iEnd;
  int iPos;
};

/*
** Virtual table cursor structure.
*/
struct Fts5tokCursor {
  capdb_vtab_cursor base;       /* Base class used by SQLite core */
  int iRowid;                     /* Current 'rowid' value */
  char *zInput;                   /* Input string */
  int nRow;                       /* Number of entries in aRow[] */
  Fts5tokRow *aRow;               /* Array of rows to return */
};

static void fts5tokDequote(char *z){
  char q = z[0];

  if( q=='[' || q=='\'' || q=='"' || q=='`' ){
    int iIn = 1;
    int iOut = 0;
    if( q=='[' ) q = ']';  

    while( z[iIn] ){
      if( z[iIn]==q ){
        if( z[iIn+1]!=q ){
          /* Character iIn was the close quote. */
          iIn++;
          break;
        }else{
          /* Character iIn and iIn+1 form an escaped quote character. Skip
          ** the input cursor past both and copy a single quote character 
          ** to the output buffer. */
          iIn += 2;
          z[iOut++] = q;
        }
      }else{
        z[iOut++] = z[iIn++];
      }
    }

    z[iOut] = '\0';
  }
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
static int fts5tokDequoteArray(
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
        fts5tokDequote(pSpace);
        pSpace += (n+1);
      }
    }
  }

  return rc;
}

/*
** Schema of the tokenizer table.
*/
#define FTS3_TOK_SCHEMA "CREATE TABLE x(input HIDDEN, token, start, end, position)"

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
static int fts5tokConnectMethod(
  capdb *db,                    /* Database connection */
  void *pCtx,                     /* Pointer to fts5_api object */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  capdb_vtab **ppVtab,          /* OUT: New capdb_vtab object */
  char **pzErr                    /* OUT: capdb_malloc'd error message */
){
  fts5_api *pApi = (fts5_api*)pCtx;
  Fts5tokTable *pTab = 0;
  int rc;
  char **azDequote = 0;
  int nDequote = 0;

  rc = capdb_declare_vtab(db, 
       "CREATE TABLE x(input HIDDEN, token, start, end, position)"
  );

  if( rc==CAPDB_OK ){
    nDequote = argc-3;
    rc = fts5tokDequoteArray(nDequote, &argv[3], &azDequote);
  }

  if( rc==CAPDB_OK ){
    pTab = (Fts5tokTable*)capdb_malloc64(sizeof(Fts5tokTable));
    if( pTab==0 ){
      rc = CAPDB_NOMEM;
    }else{
      memset(pTab, 0, sizeof(Fts5tokTable));
    }
  }

  if( rc==CAPDB_OK ){
    void *pTokCtx = 0;
    const char *zModule = 0;
    if( nDequote>0 ){
      zModule = azDequote[0];
    }

    rc = pApi->xFindTokenizer(pApi, zModule, &pTokCtx, &pTab->tok);
    if( rc==CAPDB_OK ){
      const char **azArg = (nDequote>1 ? (const char **)&azDequote[1] : 0);
      int nArg = nDequote>0 ? nDequote-1 : 0;
      rc = pTab->tok.xCreate(pTokCtx, azArg, nArg, &pTab->pTok);
    }
  }

  if( rc!=CAPDB_OK ){
    capdb_free(pTab);
    pTab = 0;
  }

  *ppVtab = (capdb_vtab*)pTab;
  capdb_free(azDequote);
  return rc;
}

/*
** This function does the work for both the xDisconnect and xDestroy methods.
** These tables have no persistent representation of their own, so xDisconnect
** and xDestroy are identical operations.
*/
static int fts5tokDisconnectMethod(capdb_vtab *pVtab){
  Fts5tokTable *pTab = (Fts5tokTable *)pVtab;
  if( pTab->pTok ){
    pTab->tok.xDelete(pTab->pTok);
  }
  capdb_free(pTab);
  return CAPDB_OK;
}

/*
** xBestIndex - Analyze a WHERE and ORDER BY clause.
*/
static int fts5tokBestIndexMethod(
  capdb_vtab *pVTab, 
  capdb_index_info *pInfo
){
  int i;

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
static int fts5tokOpenMethod(capdb_vtab *pVTab, capdb_vtab_cursor **ppCsr){
  Fts5tokCursor *pCsr;

  pCsr = (Fts5tokCursor *)capdb_malloc64(sizeof(Fts5tokCursor));
  if( pCsr==0 ){
    return CAPDB_NOMEM;
  }
  memset(pCsr, 0, sizeof(Fts5tokCursor));

  *ppCsr = (capdb_vtab_cursor *)pCsr;
  return CAPDB_OK;
}

/*
** Reset the tokenizer cursor passed as the only argument. As if it had
** just been returned by fts5tokOpenMethod().
*/
static void fts5tokResetCursor(Fts5tokCursor *pCsr){
  int i;
  for(i=0; i<pCsr->nRow; i++){
    capdb_free(pCsr->aRow[i].zToken);
  }
  capdb_free(pCsr->zInput);
  capdb_free(pCsr->aRow);
  pCsr->zInput = 0;
  pCsr->aRow = 0;
  pCsr->nRow = 0;
  pCsr->iRowid = 0;
}

/*
** xClose - Close a cursor.
*/
static int fts5tokCloseMethod(capdb_vtab_cursor *pCursor){
  Fts5tokCursor *pCsr = (Fts5tokCursor *)pCursor;
  fts5tokResetCursor(pCsr);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** xNext - Advance the cursor to the next row, if any.
*/
static int fts5tokNextMethod(capdb_vtab_cursor *pCursor){
  Fts5tokCursor *pCsr = (Fts5tokCursor *)pCursor;
  pCsr->iRowid++;
  return CAPDB_OK;
}

static int fts5tokCb(
  void *pCtx,         /* Pointer to Fts5tokCursor */
  int tflags,         /* Mask of FTS5_TOKEN_* flags */
  const char *pToken, /* Pointer to buffer containing token */
  int nToken,         /* Size of token in bytes */
  int iStart,         /* Byte offset of token within input text */
  int iEnd            /* Byte offset of end of token within input text */
){
  Fts5tokCursor *pCsr = (Fts5tokCursor*)pCtx;
  Fts5tokRow *pRow;

  if( (pCsr->nRow & (pCsr->nRow-1))==0 ){
    int nNew = pCsr->nRow ? pCsr->nRow*2 : 32;
    Fts5tokRow *aNew;
    aNew = (Fts5tokRow*)capdb_realloc64(pCsr->aRow, nNew*sizeof(Fts5tokRow));
    if( aNew==0 ) return CAPDB_NOMEM;
    memset(&aNew[pCsr->nRow], 0, sizeof(Fts5tokRow)*(nNew-pCsr->nRow));
    pCsr->aRow = aNew;
  }

  pRow = &pCsr->aRow[pCsr->nRow];
  pRow->iStart = iStart;
  pRow->iEnd = iEnd;
  if( pCsr->nRow ){
    pRow->iPos = pRow[-1].iPos + ((tflags & FTS5_TOKEN_COLOCATED) ? 0 : 1);
  }
  pRow->zToken = capdb_malloc64((capdb_int64)nToken+1);
  if( pRow->zToken==0 ) return CAPDB_NOMEM;
  memcpy(pRow->zToken, pToken, nToken);
  pRow->zToken[nToken] = 0;
  pCsr->nRow++;

  return CAPDB_OK;
}

/*
** xFilter - Initialize a cursor to point at the start of its data.
*/
static int fts5tokFilterMethod(
  capdb_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  capdb_value **apVal           /* Arguments for the indexing scheme */
){
  int rc = CAPDB_ERROR;
  Fts5tokCursor *pCsr = (Fts5tokCursor *)pCursor;
  Fts5tokTable *pTab = (Fts5tokTable *)(pCursor->pVtab);

  fts5tokResetCursor(pCsr);
  if( idxNum==1 ){
    const char *zByte = (const char *)capdb_value_text(apVal[0]);
    capdb_int64 nByte = capdb_value_bytes(apVal[0]);
    pCsr->zInput = capdb_malloc64(nByte+1);
    if( pCsr->zInput==0 ){
      rc = CAPDB_NOMEM;
    }else{
      if( nByte>0 ) memcpy(pCsr->zInput, zByte, nByte);
      pCsr->zInput[nByte] = 0;
      rc = pTab->tok.xTokenize(
          pTab->pTok, (void*)pCsr, 0, zByte, nByte, fts5tokCb
      );
    }
  }

  if( rc!=CAPDB_OK ) return rc;
  return fts5tokNextMethod(pCursor);
}

/*
** xEof - Return true if the cursor is at EOF, or false otherwise.
*/
static int fts5tokEofMethod(capdb_vtab_cursor *pCursor){
  Fts5tokCursor *pCsr = (Fts5tokCursor *)pCursor;
  return (pCsr->iRowid>pCsr->nRow);
}

/*
** xColumn - Return a column value.
*/
static int fts5tokColumnMethod(
  capdb_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  capdb_context *pCtx,          /* Context for capdb_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts5tokCursor *pCsr = (Fts5tokCursor *)pCursor;
  Fts5tokRow *pRow = &pCsr->aRow[pCsr->iRowid-1];

  /* CREATE TABLE x(input, token, start, end, position) */
  switch( iCol ){
    case 0:
      capdb_result_text(pCtx, pCsr->zInput, -1, CAPDB_TRANSIENT);
      break;
    case 1:
      capdb_result_text(pCtx, pRow->zToken, -1, CAPDB_TRANSIENT);
      break;
    case 2:
      capdb_result_int(pCtx, pRow->iStart);
      break;
    case 3:
      capdb_result_int(pCtx, pRow->iEnd);
      break;
    default:
      assert( iCol==4 );
      capdb_result_int(pCtx, pRow->iPos);
      break;
  }
  return CAPDB_OK;
}

/*
** xRowid - Return the current rowid for the cursor.
*/
static int fts5tokRowidMethod(
  capdb_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite_int64 *pRowid            /* OUT: Rowid value */
){
  Fts5tokCursor *pCsr = (Fts5tokCursor *)pCursor;
  *pRowid = (capdb_int64)pCsr->iRowid;
  return CAPDB_OK;
}

/*
** Register the fts5tok module with database connection db. Return CAPDB_OK
** if successful or an error code if capdb_create_module() fails.
*/
int capdbFts5TestRegisterTok(capdb *db, fts5_api *pApi){
  static const capdb_module fts5tok_module = {
     0,                           /* iVersion      */
     fts5tokConnectMethod,        /* xCreate       */
     fts5tokConnectMethod,        /* xConnect      */
     fts5tokBestIndexMethod,      /* xBestIndex    */
     fts5tokDisconnectMethod,     /* xDisconnect   */
     fts5tokDisconnectMethod,     /* xDestroy      */
     fts5tokOpenMethod,           /* xOpen         */
     fts5tokCloseMethod,          /* xClose        */
     fts5tokFilterMethod,         /* xFilter       */
     fts5tokNextMethod,           /* xNext         */
     fts5tokEofMethod,            /* xEof          */
     fts5tokColumnMethod,         /* xColumn       */
     fts5tokRowidMethod,          /* xRowid        */
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

  rc = capdb_create_module(db, "fts5tokenize", &fts5tok_module, (void*)pApi);
  return rc;
}

#endif /* defined(CAPDB_TEST) && defined(CAPDB_ENABLE_FTS5) */
