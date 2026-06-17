/*
** 2007 June 22
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
** This is part of an SQLite module implementing full-text search.
** This particular file implements the generic tokenizer interface.
*/

/*
** The code in this file is only compiled if:
**
**     * The FTS3 module is being built as an extension
**       (in which case CAPDB_CORE is not defined), or
**
**     * The FTS3 module is being built into the core of
**       SQLite (in which case CAPDB_ENABLE_FTS3 is defined).
*/
#include "fts3Int.h"
#if !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS3)

#include <assert.h>
#include <string.h>

/*
** Return true if the two-argument version of fts3_tokenizer()
** has been activated via a prior call to capdb_db_config(db,
** CAPDB_DBCONFIG_ENABLE_FTS3_TOKENIZER, 1, 0);
*/
static int fts3TokenizerEnabled(capdb_context *context){
  capdb *db = capdb_context_db_handle(context);
  int isEnabled = 0;
  capdb_db_config(db,CAPDB_DBCONFIG_ENABLE_FTS3_TOKENIZER,-1,&isEnabled);
  return isEnabled;
}

/*
** Implementation of the SQL scalar function for accessing the underlying 
** hash table. This function may be called as follows:
**
**   SELECT <function-name>(<key-name>);
**   SELECT <function-name>(<key-name>, <pointer>);
**
** where <function-name> is the name passed as the second argument
** to the capdbFts3InitHashTable() function (e.g. 'fts3_tokenizer').
**
** If the <pointer> argument is specified, it must be a blob value
** containing a pointer to be stored as the hash data corresponding
** to the string <key-name>. If <pointer> is not specified, then
** the string <key-name> must already exist in the has table. Otherwise,
** an error is returned.
**
** Whether or not the <pointer> argument is specified, the value returned
** is a blob containing the pointer stored as the hash data corresponding
** to string <key-name> (after the hash-table is updated, if applicable).
*/
static void fts3TokenizerFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  Fts3Hash *pHash;
  void *pPtr = 0;
  const unsigned char *zName;
  int nName;

  assert( argc==1 || argc==2 );

  pHash = (Fts3Hash *)capdb_user_data(context);

  zName = capdb_value_text(argv[0]);
  nName = capdb_value_bytes(argv[0])+1;

  if( argc==2 ){
    if( fts3TokenizerEnabled(context) || capdb_value_frombind(argv[1]) ){
      void *pOld;
      int n = capdb_value_bytes(argv[1]);
      if( zName==0 || n!=sizeof(pPtr) ){
        capdb_result_error(context, "argument type mismatch", -1);
        return;
      }
      pPtr = *(void **)capdb_value_blob(argv[1]);
      pOld = capdbFts3HashInsert(pHash, (void *)zName, nName, pPtr);
      if( pOld==pPtr ){
        capdb_result_error(context, "out of memory", -1);
      }
    }else{
      capdb_result_error(context, "fts3tokenize disabled", -1);
      return;
    }
  }else{
    if( zName ){
      pPtr = capdbFts3HashFind(pHash, zName, nName);
    }
    if( !pPtr ){
      char *zErr = capdb_mprintf("unknown tokenizer: %s", zName);
      capdb_result_error(context, zErr, -1);
      capdb_free(zErr);
      return;
    }
  }
  if( fts3TokenizerEnabled(context) || capdb_value_frombind(argv[0]) ){
    capdb_result_blob(context, (void *)&pPtr, sizeof(pPtr), CAPDB_TRANSIENT);
  }
}

int capdbFts3IsIdChar(char c){
  static const char isFtsIdChar[] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x */
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 1x */
      0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 2x */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 3x */
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 4x */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,  /* 5x */
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 6x */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  /* 7x */
  };
  return (c&0x80 || isFtsIdChar[(int)(c)]);
}

const char *capdbFts3NextToken(const char *zStr, int *pn){
  const char *z1;
  const char *z2 = 0;

  /* Find the start of the next token. */
  z1 = zStr;
  while( z2==0 ){
    char c = *z1;
    switch( c ){
      case '\0': return 0;        /* No more tokens here */
      case '\'':
      case '"':
      case '`': {
        z2 = z1;
        while( *++z2 && (*z2!=c || *++z2==c) );
        break;
      }
      case '[':
        z2 = &z1[1];
        while( *z2 && z2[0]!=']' ) z2++;
        if( *z2 ) z2++;
        break;

      default:
        if( capdbFts3IsIdChar(*z1) ){
          z2 = &z1[1];
          while( capdbFts3IsIdChar(*z2) ) z2++;
        }else{
          z1++;
        }
    }
  }

  *pn = (int)(z2-z1);
  return z1;
}

int capdbFts3InitTokenizer(
  Fts3Hash *pHash,                /* Tokenizer hash table */
  const char *zArg,               /* Tokenizer name */
  capdb_tokenizer **ppTok,      /* OUT: Tokenizer (if applicable) */
  char **pzErr                    /* OUT: Set to malloced error message */
){
  int rc;
  char *z = (char *)zArg;
  int n = 0;
  char *zCopy;
  char *zEnd;                     /* Pointer to nul-term of zCopy */
  capdb_tokenizer_module *m;

  zCopy = capdb_mprintf("%s", zArg);
  if( !zCopy ) return CAPDB_NOMEM;
  zEnd = &zCopy[strlen(zCopy)];

  z = (char *)capdbFts3NextToken(zCopy, &n);
  if( z==0 ){
    assert( n==0 );
    z = zCopy;
  }
  z[n] = '\0';
  capdbFts3Dequote(z);

  m = (capdb_tokenizer_module *)capdbFts3HashFind(pHash,z,(int)strlen(z)+1);
  if( !m ){
    capdbFts3ErrMsg(pzErr, "unknown tokenizer: %s", z);
    rc = CAPDB_ERROR;
  }else{
    char const **aArg = 0;
    int iArg = 0;
    z = &z[n+1];
    while( z<zEnd && (NULL!=(z = (char *)capdbFts3NextToken(z, &n))) ){
      capdb_int64 nNew = sizeof(char *)*(iArg+1);
      char const **aNew = (const char **)capdb_realloc64((void *)aArg, nNew);
      if( !aNew ){
        capdb_free(zCopy);
        capdb_free((void *)aArg);
        return CAPDB_NOMEM;
      }
      aArg = aNew;
      aArg[iArg++] = z;
      z[n] = '\0';
      capdbFts3Dequote(z);
      z = &z[n+1];
    }
    rc = m->xCreate(iArg, aArg, ppTok);
    assert( rc!=CAPDB_OK || *ppTok );
    if( rc!=CAPDB_OK ){
      capdbFts3ErrMsg(pzErr, "unknown tokenizer");
    }else{
      (*ppTok)->pModule = m; 
    }
    capdb_free((void *)aArg);
  }

  capdb_free(zCopy);
  return rc;
}


#ifdef CAPDB_TEST

#include "tclsqlite.h"
#include <string.h>

/*
** Implementation of a special SQL scalar function for testing tokenizers 
** designed to be used in concert with the Tcl testing framework. This
** function must be called with two or more arguments:
**
**   SELECT <function-name>(<key-name>, ..., <input-string>);
**
** where <function-name> is the name passed as the second argument
** to the capdbFts3InitHashTable() function (e.g. 'fts3_tokenizer')
** concatenated with the string '_test' (e.g. 'fts3_tokenizer_test').
**
** The return value is a string that may be interpreted as a Tcl
** list. For each token in the <input-string>, three elements are
** added to the returned list. The first is the token position, the 
** second is the token text (folded, stemmed, etc.) and the third is the
** substring of <input-string> associated with the token. For example, 
** using the built-in "simple" tokenizer:
**
**   SELECT fts_tokenizer_test('simple', 'I don't see how');
**
** will return the string:
**
**   "{0 i I 1 dont don't 2 see see 3 how how}"
**   
*/
static void testFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  Fts3Hash *pHash;
  capdb_tokenizer_module *p;
  capdb_tokenizer *pTokenizer = 0;
  capdb_tokenizer_cursor *pCsr = 0;

  const char *zErr = 0;

  const char *zName;
  int nName;
  const char *zInput;
  int nInput;

  const char *azArg[64];

  const char *zToken;
  int nToken = 0;
  int iStart = 0;
  int iEnd = 0;
  int iPos = 0;
  int i;

  Tcl_Obj *pRet;

  if( argc<2 ){
    capdb_result_error(context, "insufficient arguments", -1);
    return;
  }

  nName = capdb_value_bytes(argv[0]);
  zName = (const char *)capdb_value_text(argv[0]);
  nInput = capdb_value_bytes(argv[argc-1]);
  zInput = (const char *)capdb_value_text(argv[argc-1]);

  pHash = (Fts3Hash *)capdb_user_data(context);
  p = (capdb_tokenizer_module *)capdbFts3HashFind(pHash, zName, nName+1);

  if( !p ){
    char *zErr2 = capdb_mprintf("unknown tokenizer: %s", zName);
    capdb_result_error(context, zErr2, -1);
    capdb_free(zErr2);
    return;
  }

  pRet = Tcl_NewObj();
  Tcl_IncrRefCount(pRet);

  for(i=1; i<argc-1; i++){
    azArg[i-1] = (const char *)capdb_value_text(argv[i]);
  }

  if( CAPDB_OK!=p->xCreate(argc-2, azArg, &pTokenizer) ){
    zErr = "error in xCreate()";
    goto finish;
  }
  pTokenizer->pModule = p;
  if( capdbFts3OpenTokenizer(pTokenizer, 0, zInput, nInput, &pCsr) ){
    zErr = "error in xOpen()";
    goto finish;
  }

  while( CAPDB_OK==p->xNext(pCsr, &zToken, &nToken, &iStart, &iEnd, &iPos) ){
    Tcl_ListObjAppendElement(0, pRet, Tcl_NewIntObj(iPos));
    Tcl_ListObjAppendElement(0, pRet, Tcl_NewStringObj(zToken, nToken));
    zToken = &zInput[iStart];
    nToken = iEnd-iStart;
    Tcl_ListObjAppendElement(0, pRet, Tcl_NewStringObj(zToken, nToken));
  }

  if( CAPDB_OK!=p->xClose(pCsr) ){
    zErr = "error in xClose()";
    goto finish;
  }
  if( CAPDB_OK!=p->xDestroy(pTokenizer) ){
    zErr = "error in xDestroy()";
    goto finish;
  }

finish:
  if( zErr ){
    capdb_result_error(context, zErr, -1);
  }else{
    capdb_result_text(context, Tcl_GetString(pRet), -1, CAPDB_TRANSIENT);
  }
  Tcl_DecrRefCount(pRet);
}

static
int registerTokenizer(
  capdb *db, 
  char *zName, 
  const capdb_tokenizer_module *p
){
  int rc;
  capdb_stmt *pStmt;
  const char zSql[] = "SELECT fts3_tokenizer(?, ?)";

  rc = capdb_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=CAPDB_OK ){
    return rc;
  }

  capdb_bind_text(pStmt, 1, zName, -1, CAPDB_STATIC);
  capdb_bind_blob(pStmt, 2, &p, sizeof(p), CAPDB_STATIC);
  capdb_step(pStmt);

  return capdb_finalize(pStmt);
}


static
int queryTokenizer(
  capdb *db, 
  char *zName,  
  const capdb_tokenizer_module **pp
){
  int rc;
  capdb_stmt *pStmt;
  const char zSql[] = "SELECT fts3_tokenizer(?)";

  *pp = 0;
  rc = capdb_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=CAPDB_OK ){
    return rc;
  }

  capdb_bind_text(pStmt, 1, zName, -1, CAPDB_STATIC);
  if( CAPDB_ROW==capdb_step(pStmt) ){
    if( capdb_column_type(pStmt, 0)==CAPDB_BLOB
     && capdb_column_bytes(pStmt, 0)==sizeof(*pp)
    ){
      memcpy((void *)pp, capdb_column_blob(pStmt, 0), sizeof(*pp));
    }
  }

  return capdb_finalize(pStmt);
}

void capdbFts3SimpleTokenizerModule(capdb_tokenizer_module const**ppModule);

/*
** Implementation of the scalar function fts3_tokenizer_internal_test().
** This function is used for testing only, it is not included in the
** build unless CAPDB_TEST is defined.
**
** The purpose of this is to test that the fts3_tokenizer() function
** can be used as designed by the C-code in the queryTokenizer and
** registerTokenizer() functions above. These two functions are repeated
** in the README.tokenizer file as an example, so it is important to
** test them.
**
** To run the tests, evaluate the fts3_tokenizer_internal_test() scalar
** function with no arguments. An assert() will fail if a problem is
** detected. i.e.:
**
**     SELECT fts3_tokenizer_internal_test();
**
*/
static void intTestFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int rc;
  const capdb_tokenizer_module *p1;
  const capdb_tokenizer_module *p2;
  capdb *db = (capdb *)capdb_user_data(context);

  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);

  /* Test the query function */
  capdbFts3SimpleTokenizerModule(&p1);
  rc = queryTokenizer(db, "simple", &p2);
  assert( rc==CAPDB_OK );
  assert( p1==p2 );
  rc = queryTokenizer(db, "nosuchtokenizer", &p2);
  assert( rc==CAPDB_ERROR );
  assert( p2==0 );
  assert( 0==strcmp(capdb_errmsg(db), "unknown tokenizer: nosuchtokenizer") );

  /* Test the storage function */
  if( fts3TokenizerEnabled(context) ){
    rc = registerTokenizer(db, "nosuchtokenizer", p1);
    assert( rc==CAPDB_OK );
    rc = queryTokenizer(db, "nosuchtokenizer", &p2);
    assert( rc==CAPDB_OK );
    assert( p2==p1 );
  }

  capdb_result_text(context, "ok", -1, CAPDB_STATIC);
}

#endif

/*
** Set up SQL objects in database db used to access the contents of
** the hash table pointed to by argument pHash. The hash table must
** been initialized to use string keys, and to take a private copy 
** of the key when a value is inserted. i.e. by a call similar to:
**
**    capdbFts3HashInit(pHash, FTS3_HASH_STRING, 1);
**
** This function adds a scalar function (see header comment above
** fts3TokenizerFunc() in this file for details) and, if ENABLE_TABLE is
** defined at compilation time, a temporary virtual table (see header 
** comment above struct HashTableVtab) to the database schema. Both 
** provide read/write access to the contents of *pHash.
**
** The third argument to this function, zName, is used as the name
** of both the scalar and, if created, the virtual table.
*/
int capdbFts3InitHashTable(
  capdb *db, 
  Fts3Hash *pHash, 
  const char *zName
){
  int rc = CAPDB_OK;
  void *p = (void *)pHash;
  const int any = CAPDB_UTF8|CAPDB_DIRECTONLY;

#ifdef CAPDB_TEST
  char *zTest = 0;
  char *zTest2 = 0;
  void *pdb = (void *)db;
  zTest = capdb_mprintf("%s_test", zName);
  zTest2 = capdb_mprintf("%s_internal_test", zName);
  if( !zTest || !zTest2 ){
    rc = CAPDB_NOMEM;
  }
#endif

  if( CAPDB_OK==rc ){
    rc = capdb_create_function(db, zName, 1, any, p, fts3TokenizerFunc, 0, 0);
  }
  if( CAPDB_OK==rc ){
    rc = capdb_create_function(db, zName, 2, any, p, fts3TokenizerFunc, 0, 0);
  }
#ifdef CAPDB_TEST
  if( CAPDB_OK==rc ){
    rc = capdb_create_function(db, zTest, -1, any, p, testFunc, 0, 0);
  }
  if( CAPDB_OK==rc ){
    rc = capdb_create_function(db, zTest2, 0, any, pdb, intTestFunc, 0, 0);
  }
#endif

#ifdef CAPDB_TEST
  capdb_free(zTest);
  capdb_free(zTest2);
#endif

  return rc;
}

#endif /* !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS3) */
