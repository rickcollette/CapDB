/*
** 2009 November 25
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
** This file contains code used to insert the values of host parameters
** (aka "wildcards") into the SQL text output by capdb_trace().
**
** The Vdbe parse-tree explainer is also found here.
*/
#include "capdbInt.h"
#include "vdbeInt.h"

#ifndef CAPDB_OMIT_TRACE

/*
** zSql is a zero-terminated string of UTF-8 SQL text.  Return the number of
** bytes in this text up to but excluding the first character in
** a host parameter.  If the text contains no host parameters, return
** the total number of bytes in the text.
*/
static i64 findNextHostParameter(const char *zSql, i64 *pnToken){
  int tokenType;
  i64 nTotal = 0;
  i64 n;

  *pnToken = 0;
  while( zSql[0] ){
    n = capdbGetToken((u8*)zSql, &tokenType);
    assert( n>0 && tokenType!=TK_ILLEGAL );
    if( tokenType==TK_VARIABLE ){
      *pnToken = n;
      break;
    }
    nTotal += n;
    zSql += n;
  }
  return nTotal;
}

/*
** This function returns a pointer to a nul-terminated string in memory
** obtained from capdbDbMalloc(). If capdb.nVdbeExec is 1, then the
** string contains a copy of zRawSql but with host parameters expanded to 
** their current bindings. Or, if capdb.nVdbeExec is greater than 1, 
** then the returned string holds a copy of zRawSql with "-- " prepended
** to each line of text.
**
** If the CAPDB_TRACE_SIZE_LIMIT macro is defined to an integer, then
** then long strings and blobs are truncated to that many bytes.  This
** can be used to prevent unreasonably large trace strings when dealing
** with large (multi-megabyte) strings and blobs.
**
** The calling function is responsible for making sure the memory returned
** is eventually freed.
**
** ALGORITHM:  Scan the input string looking for host parameters in any of
** these forms:  ?, ?N, $A, @A, :A.  Take care to avoid text within
** string literals, quoted identifier names, and comments.  For text forms,
** the host parameter index is found by scanning the prepared
** statement for the corresponding OP_Variable opcode.  Once the host
** parameter index is known, locate the value in p->aVar[].  Then render
** the value as a literal in place of the host parameter name.
*/
char *capdbVdbeExpandSql(
  Vdbe *p,                 /* The prepared statement being evaluated */
  const char *zRawSql      /* Raw text of the SQL statement */
){
  capdb *db;             /* The database connection */
  int idx = 0;             /* Index of a host parameter */
  int nextIndex = 1;       /* Index of next ? host parameter */
  i64 n;                   /* Length of a token prefix */
  i64 nToken;              /* Length of the parameter token */
  int i;                   /* Loop counter */
  Mem *pVar;               /* Value of a host parameter */
  StrAccum out;            /* Accumulate the output here */
#ifndef CAPDB_OMIT_UTF16
  Mem utf8;                /* Used to convert UTF16 into UTF8 for display */
#endif

  db = p->db;
  capdbStrAccumInit(&out, 0, 0, 0, db->aLimit[CAPDB_LIMIT_LENGTH]);
  if( db->nVdbeExec>1 ){
    while( *zRawSql ){
      const char *zStart = zRawSql;
      while( *(zRawSql++)!='\n' && *zRawSql );
      capdb_str_append(&out, "-- ", 3);
      assert( (zRawSql - zStart) > 0 );
      capdb_str_append(&out, zStart, (int)(zRawSql-zStart));
    }
  }else if( p->nVar==0 ){
    capdb_str_append(&out, zRawSql, capdbStrlen30(zRawSql));
  }else{
    while( zRawSql[0] ){
      n = findNextHostParameter(zRawSql, &nToken);
      assert( n>0 );
      capdb_str_append(&out, zRawSql, n);
      zRawSql += n;
      assert( zRawSql[0] || nToken==0 );
      if( nToken==0 ) break;
      if( zRawSql[0]=='?' ){
        if( nToken>1 ){
          assert( capdbIsdigit(zRawSql[1]) );
          capdbGetInt32(&zRawSql[1], &idx);
        }else{
          idx = nextIndex;
        }
      }else{
        assert( zRawSql[0]==':' || zRawSql[0]=='$' ||
                zRawSql[0]=='@' || zRawSql[0]=='#' );
        testcase( zRawSql[0]==':' );
        testcase( zRawSql[0]=='$' );
        testcase( zRawSql[0]=='@' );
        testcase( zRawSql[0]=='#' );
        idx = capdbVdbeParameterIndex(p, zRawSql, nToken);
        assert( idx>0 );
      }
      zRawSql += nToken;
      nextIndex = MAX(idx + 1, nextIndex);
      assert( idx>0 && idx<=p->nVar );
      pVar = &p->aVar[idx-1];
      if( pVar->flags & MEM_Null ){
        capdb_str_append(&out, "NULL", 4);
      }else if( pVar->flags & (MEM_Int|MEM_IntReal) ){
        capdb_str_appendf(&out, "%lld", pVar->u.i);
      }else if( pVar->flags & MEM_Real ){
        capdb_str_appendf(&out, "%!.15g", pVar->u.r);
      }else if( pVar->flags & MEM_Str ){
        int nOut;  /* Number of bytes of the string text to include in output */
#ifndef CAPDB_OMIT_UTF16
        u8 enc = ENC(db);
        if( enc!=CAPDB_UTF8 ){
          memset(&utf8, 0, sizeof(utf8));
          utf8.db = db;
          capdbVdbeMemSetStr(&utf8, pVar->z, pVar->n, enc, CAPDB_STATIC);
          if( CAPDB_NOMEM==capdbVdbeChangeEncoding(&utf8, CAPDB_UTF8) ){
            out.accError = CAPDB_NOMEM;
            out.nAlloc = 0;
          }
          pVar = &utf8;
        }
#endif
        nOut = pVar->n;
#ifdef CAPDB_TRACE_SIZE_LIMIT
        if( nOut>CAPDB_TRACE_SIZE_LIMIT ){
          nOut = CAPDB_TRACE_SIZE_LIMIT;
          while( nOut<pVar->n && (pVar->z[nOut]&0xc0)==0x80 ){ nOut++; }
        }
#endif    
        capdb_str_appendf(&out, "'%.*q'", nOut, pVar->z);
#ifdef CAPDB_TRACE_SIZE_LIMIT
        if( nOut<pVar->n ){
          capdb_str_appendf(&out, "/*+%d bytes*/", pVar->n-nOut);
        }
#endif
#ifndef CAPDB_OMIT_UTF16
        if( enc!=CAPDB_UTF8 ) capdbVdbeMemRelease(&utf8);
#endif
      }else if( pVar->flags & MEM_Zero ){
        capdb_str_appendf(&out, "zeroblob(%d)", pVar->u.nZero);
      }else{
        int nOut;  /* Number of bytes of the blob to include in output */
        assert( pVar->flags & MEM_Blob );
        capdb_str_append(&out, "x'", 2);
        nOut = pVar->n;
#ifdef CAPDB_TRACE_SIZE_LIMIT
        if( nOut>CAPDB_TRACE_SIZE_LIMIT ) nOut = CAPDB_TRACE_SIZE_LIMIT;
#endif
        for(i=0; i<nOut; i++){
          capdb_str_appendf(&out, "%02x", pVar->z[i]&0xff);
        }
        capdb_str_append(&out, "'", 1);
#ifdef CAPDB_TRACE_SIZE_LIMIT
        if( nOut<pVar->n ){
          capdb_str_appendf(&out, "/*+%d bytes*/", pVar->n-nOut);
        }
#endif
      }
    }
  }
  if( out.accError ) capdb_str_reset(&out);
  return capdbStrAccumFinish(&out);
}

#endif /* #ifndef CAPDB_OMIT_TRACE */
