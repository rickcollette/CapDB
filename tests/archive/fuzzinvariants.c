/*
** 2022-06-14
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
** This library is used by fuzzcheck to test query invariants.
**
** An capdb_stmt is passed in that has just returned CAPDB_ROW.  This
** routine does:
**
**     *   Record the output of the current row
**     *   Construct an alternative query that should return the same row
**     *   Run the alternative query and verify that it does in fact return
**         the same row
**
*/
#include "capdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Forward references */
static char *fuzz_invariant_sql(capdb_stmt*, int);
static int sameValue(capdb_stmt*,int,capdb_stmt*,int,capdb_stmt*);
static void reportInvariantFailed(
  capdb_stmt *pOrig,   /* The original query */
  capdb_stmt *pTest,   /* The alternative test query with a missing row */
  int iRow,              /* Row number in pOrig */
  unsigned int dbOpt,    /* Optimization flags on pOrig */
  int noOpt              /* True if opt flags inverted for pTest */
);

/*
** Special parameter binding, for testing and debugging purposes.
**
**     $int_NNN        ->   integer value NNN
**     $text_TTTT      ->   floating point value TTT with destructor
**     $carray_clr     ->   First argument to carray() for color names
**     $carray_primes  ->   First argument to carray() for prime numbers
*/
static void bindDebugParameters(capdb_stmt *pStmt){
  int nVar = capdb_bind_parameter_count(pStmt);
  int i;
  for(i=1; i<=nVar; i++){
    const char *zVar = capdb_bind_parameter_name(pStmt, i);
    if( zVar==0 ) continue;
#ifdef CAPDB_ENABLE_CARRAY
    if( strcmp(zVar,"$carray_clr")==0 ){
      static char *azColorNames[] = {
        "azure", "black", "blue",   "brown", "cyan",   "fuchsia", "gold",
        "gray",  "green", "indigo", "khaki", "lime",   "magenta", "maroon",
        "navy",  "olive", "orange", "pink",  "purple", "red",     "silver",
        "tan",   "teal",  "violet", "white", "yellow"
      };
      capdb_carray_bind(pStmt,i,azColorNames,26,CAPDB_CARRAY_TEXT,0);
    }else
    if( strcmp(zVar,"$carray_primes")==0 ){
      static int aPrimes[] = {
        1, 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
       53, 59, 61, 67, 71, 73, 79, 83, 89, 97
      };
      capdb_carray_bind(pStmt,i,aPrimes,26,CAPDB_CARRAY_INT32,0);
    }else
#endif
    if( strncmp(zVar, "$int_", 5)==0 ){
      capdb_bind_int(pStmt, i, atoi(&zVar[5]));
    }else
    if( strncmp(zVar, "$text_", 6)==0 ){
      size_t szVar = strlen(zVar);
      char *zBuf = capdb_malloc64( szVar-5 );
      if( zBuf ){
        memcpy(zBuf, &zVar[6], szVar-5);
        capdb_bind_text64(pStmt, i, zBuf, szVar-6, capdb_free, CAPDB_UTF8);
      }
    }
  }
}

/*
** Do an invariant check on pStmt.  iCnt determines which invariant check to
** perform.  The first check is iCnt==0.
**
** *pbCorrupt is a flag that, if true, indicates that the database file
** is known to be corrupt.  A value of non-zero means "yes, the database
** is corrupt".  A zero value means "we do not know whether or not the
** database is corrupt".  The value might be set prior to entry, or this
** routine might set the value.
**
** Return values:
**
**     CAPDB_OK          This check was successful.
**
**     CAPDB_DONE        iCnt is out of range.  The caller typically sets
**                        up a loop on iCnt starting with zero, and increments
**                        iCnt until this code is returned.
**
**     CAPDB_CORRUPT     The invariant failed, but the underlying database
**                        file is indicating that it is corrupt, which might
**                        be the cause of the malfunction.  The *pCorrupt
**                        value will also be set.
**
**     CAPDB_INTERNAL    The invariant failed, and the database file is not
**                        corrupt.  (This never happens because this function
**                        will call abort() following an invariant failure.)
**
**     (other)            Some other kind of error occurred.
*/
int fuzz_invariant(
  capdb *db,            /* The database connection */
  capdb_stmt *pStmt,    /* Test statement stopped on an CAPDB_ROW */
  int iCnt,               /* Invariant sequence number, starting at 0 */
  int iRow,               /* Current row number */
  int nRow,               /* Number of output rows from pStmt */
  int *pbCorrupt,         /* IN/OUT: Flag indicating a corrupt database file */
  int eVerbosity,         /* How much debugging output */
  unsigned int dbOpt      /* Default optimization flags */
){
  char *zTest;
  capdb_stmt *pTestStmt = 0;
  int rc;
  int i;
  int nCol;
  int nParam;
  int noOpt = (iCnt%3)==0;

  if( *pbCorrupt ) return CAPDB_DONE;
  nParam = capdb_bind_parameter_count(pStmt);
  if( nParam>100 ) return CAPDB_DONE;
  zTest = fuzz_invariant_sql(pStmt, iCnt);
  if( zTest==0 ) return CAPDB_DONE;
  if( noOpt ){
    capdb_test_control(CAPDB_TESTCTRL_OPTIMIZATIONS, db, ~dbOpt);
  }
  rc = capdb_prepare_v2(db, zTest, -1, &pTestStmt, 0);
  if( noOpt ){
    capdb_test_control(CAPDB_TESTCTRL_OPTIMIZATIONS, db, dbOpt);
  }
  if( rc ){
    if( eVerbosity ){
      printf("invariant compile failed: %s\n%s\n",
             capdb_errmsg(db), zTest);
    }
    capdb_free(zTest);
    capdb_finalize(pTestStmt);
    return rc;
  }
  capdb_free(zTest);
  bindDebugParameters(pTestStmt);
  nCol = capdb_column_count(pStmt);
  for(i=0; i<nCol; i++){
    rc = capdb_bind_value(pTestStmt,i+1+nParam,capdb_column_value(pStmt,i));
    if( rc!=CAPDB_OK && rc!=CAPDB_RANGE ){
      capdb_finalize(pTestStmt);
      return rc;
    }
  }
  if( eVerbosity>=2 ){
    char *zSql = capdb_expanded_sql(pTestStmt);
    printf("invariant-sql row=%d #%d:\n%s\n", iRow, iCnt, zSql);
    capdb_free(zSql);
  }
  while( (rc = capdb_step(pTestStmt))==CAPDB_ROW ){
    for(i=0; i<nCol; i++){
      if( !sameValue(pStmt, i, pTestStmt, i, 0) ) break;
    }
    if( i>=nCol ) break;
  }
  if( rc==CAPDB_DONE ){
    /* No matching output row found */
    capdb_stmt *pCk = 0;
    int iOrigRSO;


    /* This is not a fault if the database file is corrupt, because anything
    ** can happen with a corrupt database file */
    rc = capdb_prepare_v2(db, "PRAGMA integrity_check", -1, &pCk, 0);
    if( rc ){
      capdb_finalize(pCk);
      capdb_finalize(pTestStmt);
      return rc;
    }
    if( eVerbosity>=2 ){
      char *zSql = capdb_expanded_sql(pCk);
      printf("invariant-validity-check #1:\n%s\n", zSql);
      capdb_free(zSql);
    }

    rc = capdb_step(pCk);
    if( rc!=CAPDB_ROW
     || capdb_column_text(pCk, 0)==0
     || strcmp((const char*)capdb_column_text(pCk,0),"ok")!=0
    ){
      *pbCorrupt = 1;
      capdb_finalize(pCk);
      capdb_finalize(pTestStmt);
      return CAPDB_CORRUPT;
    }
    capdb_finalize(pCk);

    /*
    ** If inverting the scan order also results in a miss, assume that the
    ** query is ambiguous and do not report a fault.
    */
    capdb_db_config(db, CAPDB_DBCONFIG_REVERSE_SCANORDER, -1, &iOrigRSO);
    capdb_db_config(db, CAPDB_DBCONFIG_REVERSE_SCANORDER, !iOrigRSO, 0);
    capdb_prepare_v2(db, capdb_sql(pStmt), -1, &pCk, 0);
    capdb_db_config(db, CAPDB_DBCONFIG_REVERSE_SCANORDER, iOrigRSO, 0);
    if( eVerbosity>=2 ){
      char *zSql = capdb_expanded_sql(pCk);
      printf("invariant-validity-check #2:\n%s\n", zSql);
      capdb_free(zSql);
    }
    bindDebugParameters(pCk);
    while( (rc = capdb_step(pCk))==CAPDB_ROW ){
      for(i=0; i<nCol; i++){
        if( !sameValue(pStmt, i, pTestStmt, i, 0) ) break;
      }
      if( i>=nCol ) break;
    }
    capdb_finalize(pCk);
    if( rc==CAPDB_DONE ){
      capdb_finalize(pTestStmt);
      return CAPDB_DONE;
    }

    /* The original sameValue() comparison assumed a collating sequence
    ** of "binary".  It can sometimes get an incorrect result for different
    ** collating sequences.  So rerun the test with no assumptions about
    ** collations.
    */
    rc = capdb_prepare_v2(db,
       "SELECT ?1=?2 OR ?1=?2 COLLATE nocase OR ?1=?2 COLLATE rtrim",
       -1, &pCk, 0);
    if( rc==CAPDB_OK ){
      if( eVerbosity>=2 ){
        char *zSql = capdb_expanded_sql(pCk);
        printf("invariant-validity-check #3:\n%s\n", zSql);
        capdb_free(zSql);
      }

      capdb_reset(pTestStmt);
      bindDebugParameters(pCk);
      while( (rc = capdb_step(pTestStmt))==CAPDB_ROW ){
        for(i=0; i<nCol; i++){
          if( !sameValue(pStmt, i, pTestStmt, i, pCk) ) break;
        }
        if( i>=nCol ){
          capdb_finalize(pCk);
          goto not_a_fault;
        }
      }
    }
    capdb_finalize(pCk);

    /* Invariants do not necessarily work if there are virtual tables
    ** or scalar subqueries involved in the query */
    rc = capdb_prepare_v2(db,
            "SELECT 1 FROM bytecode(?1)"
            " WHERE opcode='VOpen' OR"
            "      (opcode='Explain' AND p4 GLOB 'SCALAR SUBQUERY*')",
            -1, &pCk, 0);
    if( rc==CAPDB_OK ){
      if( eVerbosity>=2 ){
        char *zSql = capdb_expanded_sql(pCk);
        printf("invariant-validity-check #4:\n%s\n", zSql);
        capdb_free(zSql);
      }
      capdb_bind_pointer(pCk, 1, pStmt, "stmt-pointer", 0);
      rc = capdb_step(pCk);
    }
    capdb_finalize(pCk);
    if( rc==CAPDB_DONE ){
      reportInvariantFailed(pStmt, pTestStmt, iRow, dbOpt, noOpt);
      return CAPDB_INTERNAL;
    }else if( eVerbosity>0 ){
      printf("invariant-error ignored due to the use of virtual tables\n");
    }
  }
not_a_fault:
  capdb_finalize(pTestStmt);
  return CAPDB_OK;
}

/*
** Generate SQL used to test a statement invariant.
**
** Return 0 if the iCnt is out of range.
**
** iCnt meanings:
**
**   0     SELECT * FROM (<query>)
**   1     SELECT DISTINCT * FROM (<query>)
**   2     SELECT * FROM (<query>) WHERE ORDER BY 1
**   3     SELECT DISTINCT * FROM (<query>) ORDER BY 1
**   4     SELECT * FROM (<query>) WHERE <all-columns>=<all-values>
**   5     SELECT DISTINCT * FROM (<query>) WHERE <all-columns=<all-values
**   6     SELECT * FROM (<query>) WHERE <all-column>=<all-value> ORDER BY 1
**   7     SELECT DISTINCT * FROM (<query>) WHERE <all-column>=<all-value>
**                           ORDER BY 1
**   N+0   SELECT * FROM (<query>) WHERE <nth-column>=<value>
**   N+1   SELECT DISTINCT * FROM (<query>) WHERE <Nth-column>=<value>
**   N+2   SELECT * FROM (<query>) WHERE <Nth-column>=<value> ORDER BY 1
**   N+3   SELECT DISTINCT * FROM (<query>) WHERE <Nth-column>=<value>
**                           ORDER BY N
**
*/
static char *fuzz_invariant_sql(capdb_stmt *pStmt, int iCnt){
  const char *zIn;
  size_t nIn;
  const char *zAnd = "WHERE";
  int i, j;
  capdb_str *pTest;
  capdb_stmt *pBase = 0;
  capdb *db = capdb_db_handle(pStmt);
  int rc;
  int nCol = capdb_column_count(pStmt);
  int mxCnt;
  int bDistinct = 0;
  int bOrderBy = 0;
  int nParam = capdb_bind_parameter_count(pStmt);
  int hasGroupBy = 0;

  switch( iCnt % 4 ){
    case 1:  bDistinct = 1;              break;
    case 2:  bOrderBy = 1;               break;
    case 3:  bDistinct = bOrderBy = 1;   break;
  }
  iCnt /= 4;
  mxCnt = nCol;
  if( iCnt<0 || iCnt>mxCnt ) return 0;
  zIn = capdb_sql(pStmt);
  if( zIn==0 ) return 0;
  nIn = strlen(zIn);
  while( nIn>0 && (isspace(zIn[nIn-1]) || zIn[nIn-1]==';') ) nIn--;
  if( strchr(zIn, '?') ) return 0;
  pTest = capdb_str_new(0);
  capdb_str_appendf(pTest, "SELECT %s* FROM (",  
                      bDistinct ? "DISTINCT " : "");
  capdb_str_append(pTest, zIn, (int)nIn);
  capdb_str_append(pTest, ")", 1);
  rc = capdb_prepare_v2(db, capdb_str_value(pTest), -1, &pBase, 0);
  if( rc ){
    capdb_finalize(pBase);
    pBase = pStmt;
  }
  hasGroupBy = capdb_strlike("%GROUP BY%",zIn,0)==0;
  bindDebugParameters(pBase);
  for(i=0; i<capdb_column_count(pStmt); i++){
    const char *zColName = capdb_column_name(pBase,i);
    const char *zSuffix = zColName ? strrchr(zColName, ':') : 0;
    if( zSuffix 
     && isdigit(zSuffix[1])
     && (zSuffix[1]>'3' || isdigit(zSuffix[2]))
    ){
      /* This is a randomized column name and so cannot be used in the
      ** WHERE clause. */
      continue;
    }
    for(j=0; j<i; j++){
      const char *zPrior = capdb_column_name(pBase, j);
      if( capdb_stricmp(zPrior, zColName)==0 ) break;
    }
    if( j<i ){
      /* Duplicate column name */
      continue;
    }
    if( iCnt==0 ) continue;
    if( iCnt>1 && i+2!=iCnt ) continue;
    if( zColName==0 ) continue;
    if( capdb_column_type(pStmt, i)==CAPDB_NULL ){
      const char *zPlus = hasGroupBy ? "+" : "";
      capdb_str_appendf(pTest, " %s %s\"%w\" ISNULL", zAnd, zPlus, zColName);
    }else{
      capdb_str_appendf(pTest, " %s \"%w\"=?%d", zAnd, zColName, 
                          i+1+nParam);
    }
    zAnd = "AND";
  }
  if( pBase!=pStmt ) capdb_finalize(pBase);
  if( bOrderBy ){
    capdb_str_appendf(pTest, " ORDER BY %d", iCnt>2 ? iCnt-1 : 1);
  }
  return capdb_str_finish(pTest);
}

/*
** Return true if and only if v1 and is the same as v2.
*/
static int sameValue(
  capdb_stmt *pS1, int i1,       /* Value to text on the left */
  capdb_stmt *pS2, int i2,       /* Value to test on the right */
  capdb_stmt *pTestCompare       /* COLLATE comparison statement or NULL */
){
  int x = 1;
  int t1 = capdb_column_type(pS1,i1);
  int t2 = capdb_column_type(pS2,i2);
  if( t1!=t2 ){
    if( (t1==CAPDB_INTEGER && t2==CAPDB_FLOAT)
     || (t1==CAPDB_FLOAT && t2==CAPDB_INTEGER)
    ){
      /* Comparison of numerics is ok */
    }else{
      return 0;
    }
  }
  switch( capdb_column_type(pS1,i1) ){
    case CAPDB_INTEGER: {
      x =  capdb_column_int64(pS1,i1)==capdb_column_int64(pS2,i2);
      break;
    }
    case CAPDB_FLOAT: {
      x = capdb_column_double(pS1,i1)==capdb_column_double(pS2,i2);
      break;
    }
    case CAPDB_TEXT: {
      int e1 = capdb_value_encoding(capdb_column_value(pS1,i1));
      int e2 = capdb_value_encoding(capdb_column_value(pS2,i2));
      if( e1!=e2 ){
        const char *z1 = (const char*)capdb_column_text(pS1,i1);
        const char *z2 = (const char*)capdb_column_text(pS2,i2);
        x = ((z1==0 && z2==0) || (z1!=0 && z2!=0 && strcmp(z1,z1)==0));
        printf("Encodings differ.  %d on left and %d on right\n", e1, e2);
        abort();
      }
      if( pTestCompare ){
        capdb_bind_value(pTestCompare, 1, capdb_column_value(pS1,i1));
        capdb_bind_value(pTestCompare, 2, capdb_column_value(pS2,i2));
        x = capdb_step(pTestCompare)==CAPDB_ROW
                      && capdb_column_int(pTestCompare,0)!=0;
        capdb_reset(pTestCompare);
        break;
      }
      if( e1!=CAPDB_UTF8 ){
        int len1 = capdb_column_bytes16(pS1,i1);
        const unsigned char *b1 = capdb_column_blob(pS1,i1);
        int len2 = capdb_column_bytes16(pS2,i2);
        const unsigned char *b2 = capdb_column_blob(pS2,i2);
        if( len1!=len2 ){
          x = 0;
        }else if( len1==0 ){
          x = 1;
        }else{
          x = (b1!=0 && b2!=0 && memcmp(b1,b2,len1)==0);
        }
        break;
      }
      /* Fall through into the CAPDB_BLOB case */
    }
    case CAPDB_BLOB: {
      int len1 = capdb_column_bytes(pS1,i1);
      const unsigned char *b1 = capdb_column_blob(pS1,i1);
      int len2 = capdb_column_bytes(pS2,i2);
      const unsigned char *b2 = capdb_column_blob(pS2,i2);
      if( len1!=len2 ){
        x = 0;
      }else if( len1==0 ){
        x = 1;
      }else{
        x = (b1!=0 && b2!=0 && memcmp(b1,b2,len1)==0);
      }
      break;
    }
  }
  return x;
}

/*
** Print binary data as hex
*/
static void printHex(const unsigned char *a, int n, int mx){
  int j;
  for(j=0; j<mx && j<n; j++){
    printf("%02x", a[j]);
  }
  if( j<n ) printf("...");
}

/*
** Print a single row from the prepared statement
*/
static void printRow(capdb_stmt *pStmt, int iRow){
  int i, n, nCol;
  unsigned const char *data;
  nCol = capdb_column_count(pStmt);
  for(i=0; i<nCol; i++){
    printf("row%d.col%d = ", iRow, i);
    switch( capdb_column_type(pStmt, i) ){
      case CAPDB_NULL: {
        printf("NULL\n");
        break;
      }
      case CAPDB_INTEGER: {
        printf("(integer) %lld\n", capdb_column_int64(pStmt, i));
        break;
      }
      case CAPDB_FLOAT: {
        printf("(float) %f\n", capdb_column_double(pStmt, i));
        break;
      }
      case CAPDB_TEXT: {
        switch( capdb_value_encoding(capdb_column_value(pStmt,i)) ){
          case CAPDB_UTF8: {
            printf("(utf8) x'");
            n = capdb_column_bytes(pStmt, i);
            data = capdb_column_blob(pStmt, i);
            printHex(data, n, 35);
            printf("'\n");
            break;
          }
          case CAPDB_UTF16BE: {
            printf("(utf16be) x'");
            n = capdb_column_bytes16(pStmt, i);
            data = capdb_column_blob(pStmt, i);
            printHex(data, n, 35);
            printf("'\n");
            break;
          }
          case CAPDB_UTF16LE: {
            printf("(utf16le) x'");
            n = capdb_column_bytes16(pStmt, i);
            data = capdb_column_blob(pStmt, i);
            printHex(data, n, 35);
            printf("'\n");
            break;
          }
          default: {
            printf("Illegal return from capdb_value_encoding(): %d\n",
                capdb_value_encoding(capdb_column_value(pStmt,i)));
            abort();
          }
        }
        break;
      }
      case CAPDB_BLOB: {
        n = capdb_column_bytes(pStmt, i);
        data = capdb_column_blob(pStmt, i);
        printf("(blob %d bytes) x'", n);
        printHex(data, n, 35);
        printf("'\n");
        break;
      }
    }
  }
}

/*
** Report a failure of the invariant:  The current output row of pOrig
** does not appear in any row of the output from pTest.
*/
static void reportInvariantFailed(
  capdb_stmt *pOrig,   /* The original query */
  capdb_stmt *pTest,   /* The alternative test query with a missing row */
  int iRow,              /* Row number in pOrig */
  unsigned int dbOpt,    /* Optimization flags on pOrig */
  int noOpt              /* True if opt flags inverted for pTest */
){
  int iTestRow = 0;
  printf("Invariant check failed on row %d.\n", iRow);
  printf("Original query (opt-flags: 0x%08x) --------------------------\n",
         dbOpt);
  printf("%s\n", capdb_expanded_sql(pOrig));
  printf("Alternative query (opt-flags: 0x%08x) -----------------------\n",
         noOpt ? ~dbOpt : dbOpt);
  printf("%s\n", capdb_expanded_sql(pTest));
  printf("Result row that is missing from the alternative -----------------\n");
  printRow(pOrig, iRow);
  printf("Complete results from the alternative query ---------------------\n");
  capdb_reset(pTest);
  while( capdb_step(pTest)==CAPDB_ROW ){
    iTestRow++;
    printRow(pTest, iTestRow);
  }
  capdb_finalize(pTest);
  abort();
}
