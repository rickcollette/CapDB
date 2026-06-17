/*
** 2024-02-08
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/

#include "capdbintck.h"
#include <string.h>
#include <assert.h>

#include <stdio.h>

/*
** nKeyVal:
**   The number of values that make up the 'key' for the current pCheck
**   statement.
**
** rc:
**   Error code returned by most recent capdb_intck_step() or 
**   capdb_intck_unlock() call. This is set to CAPDB_DONE when
**   the integrity-check operation is finished.
**
** zErr:
**   If the object has entered the error state, this is the error message.
**   Is freed using capdb_free() when the object is deleted.
**
** zTestSql:
**   The value returned by the most recent call to capdb_intck_testsql().
**   Each call to testsql() frees the previous zTestSql value (using
**   capdb_free()) and replaces it with the new value it will return.
*/
struct capdb_intck {
  capdb *db;
  const char *zDb;                /* Copy of zDb parameter to _open() */
  char *zObj;                     /* Current object. Or NULL. */

  capdb_stmt *pCheck;           /* Current check statement */
  char *zKey;
  int nKeyVal;

  char *zMessage;
  int bCorruptSchema;

  int rc;                         /* Error code */
  char *zErr;                     /* Error message */
  char *zTestSql;                 /* Returned by capdb_intck_test_sql() */
};


/*
** Some error has occurred while using database p->db. Save the error message
** and error code currently held by the database handle in p->rc and p->zErr.
*/
static void intckSaveErrmsg(capdb_intck *p){
  p->rc = capdb_errcode(p->db);
  capdb_free(p->zErr);
  p->zErr = capdb_mprintf("%s", capdb_errmsg(p->db));
}

/*
** If the handle passed as the first argument is already in the error state,
** then this function is a no-op (returns NULL immediately). Otherwise, if an
** error occurs within this function, it leaves an error in said handle.
**
** Otherwise, this function attempts to prepare SQL statement zSql and
** return the resulting statement handle to the user.
*/
static capdb_stmt *intckPrepare(capdb_intck *p, const char *zSql){
  capdb_stmt *pRet = 0;
  if( p->rc==CAPDB_OK ){
    p->rc = capdb_prepare_v2(p->db, zSql, -1, &pRet, 0);
    if( p->rc!=CAPDB_OK ){
      intckSaveErrmsg(p);
      assert( pRet==0 );
    }
  }
  return pRet;
}

/*
** If the handle passed as the first argument is already in the error state,
** then this function is a no-op (returns NULL immediately). Otherwise, if an
** error occurs within this function, it leaves an error in said handle.
**
** Otherwise, this function treats argument zFmt as a printf() style format
** string. It formats it according to the trailing arguments and then 
** attempts to prepare the results and return the resulting prepared
** statement.
*/
static capdb_stmt *intckPrepareFmt(capdb_intck *p, const char *zFmt, ...){
  capdb_stmt *pRet = 0;
  va_list ap;
  char *zSql = 0;
  va_start(ap, zFmt);
  zSql = capdb_vmprintf(zFmt, ap);
  if( p->rc==CAPDB_OK && zSql==0 ){
    p->rc = CAPDB_NOMEM;
  }
  pRet = intckPrepare(p, zSql);
  capdb_free(zSql);
  va_end(ap);
  return pRet;
}

/*
** Finalize SQL statement pStmt. If an error occurs and the handle passed
** as the first argument does not already contain an error, store the
** error in the handle.
*/
static void intckFinalize(capdb_intck *p, capdb_stmt *pStmt){
  int rc = capdb_finalize(pStmt);
  if( p->rc==CAPDB_OK && rc!=CAPDB_OK ){
    intckSaveErrmsg(p);
  }
}

/*
** If there is already an error in handle p, return it. Otherwise, call
** capdb_step() on the statement handle and return that value.
*/
static int intckStep(capdb_intck *p, capdb_stmt *pStmt){
  if( p->rc ) return p->rc;
  return capdb_step(pStmt);
}

/*
** Execute SQL statement zSql. There is no way to obtain any results 
** returned by the statement. This function uses the capdb_intck error
** code convention.
*/
static void intckExec(capdb_intck *p, const char *zSql){
  capdb_stmt *pStmt = 0;
  pStmt = intckPrepare(p, zSql);
  intckStep(p, pStmt);
  intckFinalize(p, pStmt);
}

/*
** A wrapper around capdb_mprintf() that uses the capdb_intck error
** code convention.
*/
static char *intckMprintf(capdb_intck *p, const char *zFmt, ...){
  va_list ap;
  char *zRet = 0;
  va_start(ap, zFmt);
  zRet = capdb_vmprintf(zFmt, ap);
  if( p->rc==CAPDB_OK ){
    if( zRet==0 ){
      p->rc = CAPDB_NOMEM;
    }
  }else{
    capdb_free(zRet);
    zRet = 0;
  }
  va_end(ap);
  return zRet;
}

/*
** This is used by capdb_intck_unlock() to save the vector key value 
** required to restart the current pCheck query as a nul-terminated string 
** in p->zKey.
*/
static void intckSaveKey(capdb_intck *p){
  int ii;
  char *zSql = 0;
  capdb_stmt *pStmt = 0;
  capdb_stmt *pXinfo = 0;
  const char *zDir = 0;

  assert( p->pCheck );
  assert( p->zKey==0 );

  pXinfo = intckPrepareFmt(p, 
      "SELECT group_concat(desc, '') FROM %Q.sqlite_schema s, "
      "pragma_index_xinfo(%Q, %Q) "
      "WHERE s.type='index' AND s.name=%Q",
      p->zDb, p->zObj, p->zDb, p->zObj
  );
  if( p->rc==CAPDB_OK && CAPDB_ROW==capdb_step(pXinfo) ){
    zDir = (const char*)capdb_column_text(pXinfo, 0);
  }

  if( zDir==0 ){
    /* Object is a table, not an index. This is the easy case,as there are 
    ** no DESC columns or NULL values in a primary key.  */
    const char *zSep = "SELECT '(' || ";
    for(ii=0; ii<p->nKeyVal; ii++){
      zSql = intckMprintf(p, "%z%squote(?)", zSql, zSep);
      zSep = " || ', ' || ";
    }
    zSql = intckMprintf(p, "%z || ')'", zSql);
  }else{

    /* Object is an index. */
    assert( p->nKeyVal>1 );
    for(ii=p->nKeyVal; ii>0; ii--){
      int bLastIsDesc = zDir[ii-1]=='1';
      int bLastIsNull = capdb_column_type(p->pCheck, ii)==CAPDB_NULL;
      const char *zLast = capdb_column_name(p->pCheck, ii);
      char *zLhs = 0;
      char *zRhs = 0;
      char *zWhere = 0;

      if( bLastIsNull ){
        if( bLastIsDesc ) continue;
        zWhere = intckMprintf(p, "'%s IS NOT NULL'", zLast);
      }else{
        const char *zOp = bLastIsDesc ? "<" : ">";
        zWhere = intckMprintf(p, "'%s %s ' || quote(?%d)", zLast, zOp, ii);
      }

      if( ii>1 ){
        const char *zLhsSep = "";
        const char *zRhsSep = "";
        int jj;
        for(jj=0; jj<ii-1; jj++){
          const char *zAlias = (const char*)capdb_column_name(p->pCheck,jj+1);
          zLhs = intckMprintf(p, "%z%s%s", zLhs, zLhsSep, zAlias);
          zRhs = intckMprintf(p, "%z%squote(?%d)", zRhs, zRhsSep, jj+1);
          zLhsSep = ",";
          zRhsSep = " || ',' || ";
        }

        zWhere = intckMprintf(p, 
            "'(%z) IS (' || %z || ') AND ' || %z",
            zLhs, zRhs, zWhere);
      }
      zWhere = intckMprintf(p, "'WHERE ' || %z", zWhere);

      zSql = intckMprintf(p, "%z%s(quote( %z ) )",
          zSql,
          (zSql==0 ? "VALUES" : ",\n      "),
          zWhere
      );
    }
    zSql = intckMprintf(p, 
        "WITH wc(q) AS (\n%z\n)"
        "SELECT 'VALUES' || group_concat('(' || q || ')', ',\n      ') FROM wc"
        , zSql
    );
  }

  pStmt = intckPrepare(p, zSql);
  if( p->rc==CAPDB_OK ){
    for(ii=0; ii<p->nKeyVal; ii++){
      capdb_bind_value(pStmt, ii+1, capdb_column_value(p->pCheck, ii+1));
    }
    if( CAPDB_ROW==capdb_step(pStmt) ){
      p->zKey = intckMprintf(p,"%s",(const char*)capdb_column_text(pStmt, 0));
    }
    intckFinalize(p, pStmt);
  }

  capdb_free(zSql);
  intckFinalize(p, pXinfo);
}

/*
** Find the next database object (table or index) to check. If successful,
** set capdb_intck.zObj to point to a nul-terminated buffer containing
** the object's name before returning.
*/
static void intckFindObject(capdb_intck *p){
  capdb_stmt *pStmt = 0;
  char *zPrev = p->zObj;
  p->zObj = 0;

  assert( p->rc==CAPDB_OK );
  assert( p->pCheck==0 );

  pStmt = intckPrepareFmt(p, 
    "WITH tables(table_name) AS (" 
    "  SELECT name"
    "  FROM %Q.sqlite_schema WHERE (type='table' OR type='index') AND rootpage"
    "  UNION ALL "
    "  SELECT 'sqlite_schema'"
    ")"
    "SELECT table_name FROM tables "
    "WHERE ?1 IS NULL OR table_name%s?1 "
    "ORDER BY 1"
    , p->zDb, (p->zKey ? ">=" : ">")
  );

  if( p->rc==CAPDB_OK ){
    capdb_bind_text(pStmt, 1, zPrev, -1, CAPDB_TRANSIENT);
    if( capdb_step(pStmt)==CAPDB_ROW ){
      p->zObj = intckMprintf(p,"%s",(const char*)capdb_column_text(pStmt, 0));
    }
  }
  intckFinalize(p, pStmt);

  /* If this is a new object, ensure the previous key value is cleared. */
  if( capdb_stricmp(p->zObj, zPrev) ){
    capdb_free(p->zKey);
    p->zKey = 0;
  }

  capdb_free(zPrev);
}

/*
** Return the size in bytes of the first token in nul-terminated buffer z.
** For the purposes of this call, a token is either:
**
**   *  a quoted SQL string,
*    *  a contiguous series of ascii alphabet characters, or
*    *  any other single byte.
*/
static int intckGetToken(const char *z){
  char c = z[0];
  int iRet = 1;
  if( c=='\'' || c=='"' || c=='`' ){
    while( z[iRet] ){
      if( z[iRet]==c ){
        iRet++;
        if( z[iRet]!=c ) break;
      }
      iRet++;
    }
  }
  else if( c=='[' ){
    while( z[iRet++]!=']' && z[iRet] );
  }
  else if( (c>='A' && c<='Z') || (c>='a' && c<='z') ){
    while( (z[iRet]>='A' && z[iRet]<='Z') || (z[iRet]>='a' && z[iRet]<='z') ){
      iRet++;
    }
  }

  return iRet;
}

/*
** Return true if argument c is an ascii whitespace character.
*/
static int intckIsSpace(char c){
  return (c==' ' || c=='\t' || c=='\n' || c=='\r');
}

/*
** Argument z points to the text of a CREATE INDEX statement. This function
** identifies the part of the text that contains either the index WHERE 
** clause (if iCol<0) or the iCol'th column of the index.
**
** If (iCol<0), the identified fragment does not include the "WHERE" keyword,
** only the expression that follows it. If (iCol>=0) then the identified
** fragment does not include any trailing sort-order keywords - "ASC" or 
** "DESC".
**
** If the CREATE INDEX statement does not contain the requested field or
** clause, NULL is returned and (*pnByte) is set to 0. Otherwise, a pointer to
** the identified fragment is returned and output parameter (*pnByte) set
** to its size in bytes.
*/
static const char *intckParseCreateIndex(const char *z, int iCol, int *pnByte){
  int iOff = 0;
  int iThisCol = 0;
  int iStart = 0;
  int nOpen = 0;

  const char *zRet = 0;
  int nRet = 0;

  int iEndOfCol = 0;

  /* Skip forward until the first "(" token */
  while( z[iOff]!='(' ){
    iOff += intckGetToken(&z[iOff]);
    if( z[iOff]=='\0' ) return 0;
  }
  assert( z[iOff]=='(' );

  nOpen = 1;
  iOff++;
  iStart = iOff;
  while( z[iOff] ){
    const char *zToken = &z[iOff];
    int nToken = 0;

    /* Check if this is the end of the current column - either a "," or ")"
    ** when nOpen==1.  */
    if( nOpen==1 ){
      if( z[iOff]==',' || z[iOff]==')' ){
        if( iCol==iThisCol ){
          int iEnd = iEndOfCol ? iEndOfCol : iOff;
          nRet = (iEnd - iStart);
          zRet = &z[iStart];
          break;
        }
        iStart = iOff+1;
        while( intckIsSpace(z[iStart]) ) iStart++;
        iThisCol++;
      }
      if( z[iOff]==')' ) break;
    }
    if( z[iOff]=='(' ) nOpen++;
    if( z[iOff]==')' ) nOpen--;
    nToken = intckGetToken(zToken);

    if( (nToken==3 && 0==capdb_strnicmp(zToken, "ASC", nToken))
     || (nToken==4 && 0==capdb_strnicmp(zToken, "DESC", nToken))
    ){
      iEndOfCol = iOff;
    }else if( 0==intckIsSpace(zToken[0]) ){
      iEndOfCol = 0;
    }

    iOff += nToken;
  }

  /* iStart is now the byte offset of 1 byte passed the final ')' in the
  ** CREATE INDEX statement. Try to find a WHERE clause to return.  */
  while( zRet==0 && z[iOff] ){
    int n = intckGetToken(&z[iOff]);
    if( n==5 && 0==capdb_strnicmp(&z[iOff], "where", 5) ){
      zRet = &z[iOff+5];
      nRet = (int)strlen(zRet);
    }
    iOff += n;
  }

  /* Trim any whitespace from the start and end of the returned string. */
  if( zRet ){
    while( intckIsSpace(zRet[0]) ){
      nRet--;
      zRet++;
    }
    while( nRet>0 && intckIsSpace(zRet[nRet-1]) ) nRet--;
  }

  *pnByte = nRet;
  return zRet;
}

/*
** User-defined SQL function wrapper for intckParseCreateIndex():
**
**     SELECT parse_create_index(<sql>, <icol>);
*/
static void intckParseCreateIndexFunc(
  capdb_context *pCtx, 
  int nVal, 
  capdb_value **apVal
){
  const char *zSql = (const char*)capdb_value_text(apVal[0]);
  int idx = capdb_value_int(apVal[1]);
  const char *zRes = 0;
  int nRes = 0;

  assert( nVal==2 );
  if( zSql ){
    zRes = intckParseCreateIndex(zSql, idx, &nRes);
  }
  capdb_result_text(pCtx, zRes, nRes, CAPDB_TRANSIENT);
}

/*
** Return true if capdb_intck.db has automatic indexes enabled, false
** otherwise.
*/
static int intckGetAutoIndex(capdb_intck *p){
  int bRet = 0;
  capdb_stmt *pStmt = 0;
  pStmt = intckPrepare(p, "PRAGMA automatic_index");
  if( CAPDB_ROW==intckStep(p, pStmt) ){
    bRet = capdb_column_int(pStmt, 0);
  }
  intckFinalize(p, pStmt);
  return bRet;
}

/*
** Return true if zObj is an index, or false otherwise.
*/
static int intckIsIndex(capdb_intck *p, const char *zObj){
  int bRet = 0;
  capdb_stmt *pStmt = 0;
  pStmt = intckPrepareFmt(p, 
      "SELECT 1 FROM %Q.sqlite_schema WHERE name=%Q AND type='index'",
      p->zDb, zObj
  );
  if( p->rc==CAPDB_OK && CAPDB_ROW==capdb_step(pStmt) ){
    bRet = 1;
  }
  intckFinalize(p, pStmt);
  return bRet;
}

/*
** Return a pointer to a nul-terminated buffer containing the SQL statement
** used to check database object zObj (a table or index) for corruption.
** If parameter zPrev is not NULL, then it must be a string containing the
** vector key required to restart the check where it left off last time.
** If pnKeyVal is not NULL, then (*pnKeyVal) is set to the number of
** columns in the vector key value for the specified object.
**
** This function uses the capdb_intck error code convention.
*/
static char *intckCheckObjectSql(
  capdb_intck *p,               /* Integrity check object */
  const char *zObj,               /* Object (table or index) to scan */
  const char *zPrev,              /* Restart key vector, if any */
  int *pnKeyVal                   /* OUT: Number of key-values for this scan */
){
  char *zRet = 0;
  capdb_stmt *pStmt = 0;
  int bAutoIndex = 0;
  int bIsIndex = 0;

  const char *zCommon = 
      /* Relation without_rowid also contains just one row. Column "b" is
      ** set to true if the table being examined is a WITHOUT ROWID table,
      ** or false otherwise.  */
      ", without_rowid(b) AS ("
      "  SELECT EXISTS ("
      "    SELECT 1 FROM tabname, pragma_index_list(tab, db) AS l"
      "      WHERE origin='pk' "
      "      AND NOT EXISTS (SELECT 1 FROM sqlite_schema WHERE name=l.name)"
      "  )"
      ")"
      ""
      /* Table idx_cols contains 1 row for each column in each index on the
      ** table being checked. Columns are:
      **
      **   idx_name: Name of the index.
      **   idx_ispk: True if this index is the PK of a WITHOUT ROWID table.
      **   col_name: Name of indexed column, or NULL for index on expression.
      **   col_expr: Indexed expression, including COLLATE clause.
      **   col_alias: Alias used for column in 'intck_wrapper' table.
      */
      ", idx_cols(idx_name, idx_ispk, col_name, col_expr, col_alias) AS ("
      "  SELECT l.name, (l.origin=='pk' AND w.b), i.name, COALESCE(("
      "    SELECT parse_create_index(sql, i.seqno) FROM "
      "    sqlite_schema WHERE name = l.name"
      "  ), format('\"%w\"', i.name) || ' COLLATE ' || quote(i.coll)),"
      "  'c' || row_number() OVER ()"
      "  FROM "
      "      tabname t,"
      "      without_rowid w,"
      "      pragma_index_list(t.tab, t.db) l,"
      "      pragma_index_xinfo(l.name) i"
      "      WHERE i.key"
      "  UNION ALL"
      "  SELECT '', 1, '_rowid_', '_rowid_', 'r1' FROM without_rowid WHERE b=0"
      ")"
      ""
      ""
      /*
      ** For a PK declared as "PRIMARY KEY(a, b) ... WITHOUT ROWID", where
      ** the intck_wrapper aliases of "a" and "b" are "c1" and "c2":
      **
      **   o_pk:   "o.c1, o.c2"
      **   i_pk:   "i.'a', i.'b'"
      **   ...
      **   n_pk:   2
      */ 
      ", tabpk(db, tab, idx, o_pk, i_pk, q_pk, eq_pk, ps_pk, pk_pk, n_pk) AS ("
      "    WITH pkfields(f, a) AS ("
      "      SELECT i.col_name, i.col_alias FROM idx_cols i WHERE i.idx_ispk"
      "    )"
      "    SELECT t.db, t.tab, t.idx, "
      "           group_concat(a, ', '), "
      "           group_concat('i.'||quote(f), ', '), "
      "           group_concat('quote(o.'||a||')', ' || '','' || '),  "
      "           format('(%s)==(%s)',"
      "               group_concat('o.'||a, ', '), "
      "               group_concat(format('\"%w\"', f), ', ')"
      "           ),"
      "           group_concat('%s', ','),"
      "           group_concat('quote('||a||')', ', '),  "
      "           count(*)"
      "    FROM tabname t, pkfields"
      ")"
      ""
      ", idx(name, match_expr, partial, partial_alias, idx_ps, idx_idx) AS ("
      "  SELECT idx_name,"
      "    format('(%s,%s) IS (%s,%s)', "
      "           group_concat(i.col_expr, ', '), i_pk,"
      "           group_concat('o.'||i.col_alias, ', '), o_pk"
      "    ), "
      "    parse_create_index("
      "        (SELECT sql FROM sqlite_schema WHERE name=idx_name), -1"
      "    ),"
      "    'cond' || row_number() OVER ()"
      "    , group_concat('%s', ',')"
      "    , group_concat('quote('||i.col_alias||')', ', ')"
      "  FROM tabpk t, "
      "       without_rowid w,"
      "       idx_cols i"
      "  WHERE i.idx_ispk==0 "
      "  GROUP BY idx_name"
      ")"
      ""
      ", wrapper_with(s) AS ("
      "  SELECT 'intck_wrapper AS (\n  SELECT\n    ' || ("
      "      WITH f(a, b) AS ("
      "        SELECT col_expr, col_alias FROM idx_cols"
      "          UNION ALL "
      "        SELECT partial, partial_alias FROM idx WHERE partial IS NOT NULL"
      "      )"
      "      SELECT group_concat(format('%s AS %s', a, b), ',\n    ') FROM f"
      "    )"
      "    || format('\n  FROM %Q.%Q ', t.db, t.tab)"
           /* If the object being checked is a table, append "NOT INDEXED".
           ** Otherwise, append "INDEXED BY <index>", and then, if the index 
           ** is a partial index " WHERE <condition>".  */
      "    || CASE WHEN t.idx IS NULL THEN "
      "        'NOT INDEXED'"
      "       ELSE"
      "        format('INDEXED BY %Q%s', t.idx, ' WHERE '||i.partial)"
      "       END"
      "    || '\n)'"
      "    FROM tabname t LEFT JOIN idx i ON (i.name=t.idx)"
      ")"
      ""
  ;

  bAutoIndex = intckGetAutoIndex(p);
  if( bAutoIndex ) intckExec(p, "PRAGMA automatic_index = 0");

  bIsIndex = intckIsIndex(p, zObj);
  if( bIsIndex ){
    pStmt = intckPrepareFmt(p,
      /* Table idxname contains a single row. The first column, "db", contains
      ** the name of the db containing the table (e.g. "main") and the second,
      ** "tab", the name of the table itself.  */
      "WITH tabname(db, tab, idx) AS ("
      "  SELECT %Q, (SELECT tbl_name FROM %Q.sqlite_schema WHERE name=%Q), %Q "
      ")"
      ""
      ", whereclause(w_c) AS (%s)"
      ""
      "%s" /* zCommon */
      ""
      ", case_statement(c) AS ("
      "  SELECT "
      "    'CASE WHEN (' || group_concat(col_alias, ', ') || ', 1) IS (\n' "
      "    || '      SELECT ' || group_concat(col_expr, ', ') || ', 1 FROM '"
      "    || format('%%Q.%%Q NOT INDEXED WHERE %%s\n', t.db, t.tab, p.eq_pk)"
      "    || '    )\n  THEN NULL\n    '"
      "    || 'ELSE format(''surplus entry ('"
      "    ||   group_concat('%%s', ',') || ',' || p.ps_pk"
      "    || ') in index ' || t.idx || ''', ' "
      "    ||   group_concat('quote('||i.col_alias||')', ', ') || ', ' || p.pk_pk"
      "    || ')'"
      "    || '\n  END AS error_message'"
      "  FROM tabname t, tabpk p, idx_cols i WHERE i.idx_name=t.idx"
      ")"
      ""
      ", thiskey(k, n) AS ("
      "    SELECT group_concat(i.col_alias, ', ') || ', ' || p.o_pk, "
      "           count(*) + p.n_pk "
      "    FROM tabpk p, idx_cols i WHERE i.idx_name=p.idx"
      ")"
      ""
      ", main_select(m, n) AS ("
      "  SELECT format("
      "      'WITH %%s\n' ||"
      "      ', idx_checker AS (\n' ||"
      "      '  SELECT %%s,\n' ||"
      "      '  %%s\n' || "
      "      '  FROM intck_wrapper AS o\n' ||"
      "      ')\n',"
      "      ww.s, c, t.k"
      "  ), t.n"
      "  FROM case_statement, wrapper_with ww, thiskey t"
      ")"

      "SELECT m || "
      "    group_concat('SELECT * FROM idx_checker ' || w_c, ' UNION ALL '), n"
      " FROM "
      "main_select, whereclause "
      , p->zDb, p->zDb, zObj, zObj
      , zPrev ? zPrev : "VALUES('')", zCommon
      );
  }else{
    pStmt = intckPrepareFmt(p,
      /* Table tabname contains a single row. The first column, "db", contains
      ** the name of the db containing the table (e.g. "main") and the second,
      ** "tab", the name of the table itself.  */
      "WITH tabname(db, tab, idx, prev) AS (SELECT %Q, %Q, NULL, %Q)"
      ""
      "%s" /* zCommon */

      /* expr(e) contains one row for each index on table zObj. Value e
      ** is set to an expression that evaluates to NULL if the required
      ** entry is present in the index, or an error message otherwise.  */
      ", expr(e, p) AS ("
      "  SELECT format('CASE WHEN EXISTS \n"
      "    (SELECT 1 FROM %%Q.%%Q AS i INDEXED BY %%Q WHERE %%s%%s)\n"
      "    THEN NULL\n"
      "    ELSE format(''entry (%%s,%%s) missing from index %%s'', %%s, %%s)\n"
      "  END\n'"
      "    , t.db, t.tab, i.name, i.match_expr, ' AND (' || partial || ')',"
      "      i.idx_ps, t.ps_pk, i.name, i.idx_idx, t.pk_pk),"
      "    CASE WHEN partial IS NULL THEN NULL ELSE i.partial_alias END"
      "  FROM tabpk t, idx i"
      ")"

      ", numbered(ii, cond, e) AS ("
      "  SELECT 0, 'n.ii=0', 'NULL'"
      "    UNION ALL "
      "  SELECT row_number() OVER (),"
      "      '(n.ii='||row_number() OVER ()||COALESCE(' AND '||p||')', ')'), e"
      "  FROM expr"
      ")"

      ", counter_with(w) AS ("
      "    SELECT 'WITH intck_counter(ii) AS (\n  ' || "
      "       group_concat('SELECT '||ii, ' UNION ALL\n  ') "
      "    || '\n)' FROM numbered"
      ")"
      ""
      ", case_statement(c) AS ("
      "    SELECT 'CASE ' || "
      "    group_concat(format('\n  WHEN %%s THEN (%%s)', cond, e), '') ||"
      "    '\nEND AS error_message'"
      "    FROM numbered"
      ")"
      ""

      /* This table contains a single row consisting of a single value -
      ** the text of an SQL expression that may be used by the main SQL
      ** statement to output an SQL literal that can be used to resume
      ** the scan if it is suspended. e.g. for a rowid table, an expression
      ** like:
      **
      **     format('(%d,%d)', _rowid_, n.ii)
      */
      ", thiskey(k, n) AS ("
      "    SELECT o_pk || ', ii', n_pk+1 FROM tabpk"
      ")"
      ""
      ", whereclause(w_c) AS ("
      "    SELECT CASE WHEN prev!='' THEN "
      "    '\nWHERE (' || o_pk ||', n.ii) > ' || prev"
      "    ELSE ''"
      "    END"
      "    FROM tabpk, tabname"
      ")"
      ""
      ", main_select(m, n) AS ("
      "  SELECT format("
      "      '%%s, %%s\nSELECT %%s,\n%%s\nFROM intck_wrapper AS o"
               ", intck_counter AS n%%s\nORDER BY %%s', "
      "      w, ww.s, c, thiskey.k, whereclause.w_c, t.o_pk"
      "  ), thiskey.n"
      "  FROM case_statement, tabpk t, counter_with, "
      "       wrapper_with ww, thiskey, whereclause"
      ")"

      "SELECT m, n FROM main_select",
      p->zDb, zObj, zPrev, zCommon
    );
  }

  while( p->rc==CAPDB_OK && CAPDB_ROW==capdb_step(pStmt) ){
    zRet = intckMprintf(p, "%s", (const char*)capdb_column_text(pStmt, 0));
    if( pnKeyVal ){
      *pnKeyVal = capdb_column_int(pStmt, 1);
    }
  }
  intckFinalize(p, pStmt);

  if( bAutoIndex ) intckExec(p, "PRAGMA automatic_index = 1");
  return zRet;
}

/*
** Open a new integrity-check object.
*/
int capdb_intck_open(
  capdb *db,                    /* Database handle to operate on */
  const char *zDbArg,             /* "main", "temp" etc. */
  capdb_intck **ppOut           /* OUT: New integrity-check handle */
){
  capdb_intck *pNew = 0;
  int rc = CAPDB_OK;
  const char *zDb = zDbArg ? zDbArg : "main";
  int nDb = (int)strlen(zDb);

  pNew = (capdb_intck*)capdb_malloc(sizeof(*pNew) + nDb + 1);
  if( pNew==0 ){
    rc = CAPDB_NOMEM;
  }else{
    memset(pNew, 0, sizeof(*pNew));
    pNew->db = db;
    pNew->zDb = (const char*)&pNew[1];
    memcpy(&pNew[1], zDb, nDb+1);
    rc = capdb_create_function(db, "parse_create_index", 
        2, CAPDB_UTF8, 0, intckParseCreateIndexFunc, 0, 0
    );
    if( rc!=CAPDB_OK ){
      capdb_intck_close(pNew);
      pNew = 0;
    }
  }

  *ppOut = pNew;
  return rc;
}

/*
** Free the integrity-check object.
*/
void capdb_intck_close(capdb_intck *p){
  if( p ){
    capdb_finalize(p->pCheck);
    capdb_create_function(
        p->db, "parse_create_index", 1, CAPDB_UTF8, 0, 0, 0, 0
    );
    capdb_free(p->zObj);
    capdb_free(p->zKey);
    capdb_free(p->zTestSql);
    capdb_free(p->zErr);
    capdb_free(p->zMessage);
    capdb_free(p);
  }
}

/*
** Step the integrity-check object.
*/
int capdb_intck_step(capdb_intck *p){
  if( p->rc==CAPDB_OK ){

    if( p->zMessage ){
      capdb_free(p->zMessage);
      p->zMessage = 0;
    }

    if( p->bCorruptSchema ){
      p->rc = CAPDB_DONE;
    }else
    if( p->pCheck==0 ){
      intckFindObject(p);
      if( p->rc==CAPDB_OK ){
        if( p->zObj ){
          char *zSql = 0;
          zSql = intckCheckObjectSql(p, p->zObj, p->zKey, &p->nKeyVal);
          p->pCheck = intckPrepare(p, zSql);
          capdb_free(zSql);
          capdb_free(p->zKey);
          p->zKey = 0;
        }else{
          p->rc = CAPDB_DONE;
        }
      }else if( p->rc==CAPDB_CORRUPT ){
        p->rc = CAPDB_OK;
        p->zMessage = intckMprintf(p, "%s",
            "corruption found while reading database schema"
        );
        p->bCorruptSchema = 1;
      }
    }

    if( p->pCheck ){
      assert( p->rc==CAPDB_OK );
      if( capdb_step(p->pCheck)==CAPDB_ROW ){
        /* Normal case, do nothing. */
      }else{
        intckFinalize(p, p->pCheck);
        p->pCheck = 0;
        p->nKeyVal = 0;
        if( p->rc==CAPDB_CORRUPT ){
          p->rc = CAPDB_OK;
          p->zMessage = intckMprintf(p, 
              "corruption found while scanning database object %s", p->zObj
          );
        }
      }
    }
  }

  return p->rc;
}

/*
** Return a message describing the corruption encountered by the most recent
** call to capdb_intck_step(), or NULL if no corruption was encountered.
*/
const char *capdb_intck_message(capdb_intck *p){
  assert( p->pCheck==0 || p->zMessage==0 );
  if( p->zMessage ){
    return p->zMessage;
  }
  if( p->pCheck ){
    return (const char*)capdb_column_text(p->pCheck, 0);
  }
  return 0;
}

/*
** Return the error code and message.
*/
int capdb_intck_error(capdb_intck *p, const char **pzErr){
  if( pzErr ) *pzErr = p->zErr;
  return (p->rc==CAPDB_DONE ? CAPDB_OK : p->rc);
}

/*
** Close any read transaction the integrity-check object is holding open
** on the database.
*/
int capdb_intck_unlock(capdb_intck *p){
  if( p->rc==CAPDB_OK && p->pCheck ){
    assert( p->zKey==0 && p->nKeyVal>0 );
    intckSaveKey(p);
    intckFinalize(p, p->pCheck);
    p->pCheck = 0;
  }
  return p->rc;
}

/*
** Return the SQL statement used to check object zObj. Or, if zObj is 
** NULL, the current SQL statement.
*/
const char *capdb_intck_test_sql(capdb_intck *p, const char *zObj){
  capdb_free(p->zTestSql);
  if( zObj ){
    p->zTestSql = intckCheckObjectSql(p, zObj, 0, 0);
  }else{
    if( p->zObj ){
      p->zTestSql = intckCheckObjectSql(p, p->zObj, p->zKey, 0);
    }else{
      capdb_free(p->zTestSql);
      p->zTestSql = 0;
    }
  }
  return p->zTestSql;
}
