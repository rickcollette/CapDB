/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Main file for the SQLite library.  The routines in this file
** implement the programmer interface to the library.  Routines in
** other files are for internal use by SQLite and should not be
** accessed by users of the library.
*/

#include "capdbInt.h"

/*
** Execute SQL code.  Return one of the CAPDB_ success/failure
** codes.  Also write an error message into memory obtained from
** malloc() and make *pzErrMsg point to that message.
**
** If the SQL is a query, then for each row in the query result
** the xCallback() function is called.  pArg becomes the first
** argument to xCallback().  If xCallback=NULL then no callback
** is invoked, even for queries.
*/
int capdb_exec(
  capdb *db,                /* The database on which the SQL executes */
  const char *zSql,           /* The SQL to be executed */
  capdb_callback xCallback, /* Invoke this callback routine */
  void *pArg,                 /* First argument to xCallback() */
  char **pzErrMsg             /* Write error messages here */
){
  int rc = CAPDB_OK;         /* Return code */
  const char *zLeftover;      /* Tail of unprocessed SQL */
  capdb_stmt *pStmt = 0;    /* The current SQL statement */
  char **azCols = 0;          /* Names of result columns */
  int callbackIsInit;         /* True if callback data is initialized */

  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
  if( zSql==0 ) zSql = "";

  capdb_mutex_enter(db->mutex);
  capdbError(db, CAPDB_OK);
  while( rc==CAPDB_OK && zSql[0] ){
    int nCol = 0;
    char **azVals = 0;

    pStmt = 0;
    rc = capdb_prepare_v2(db, zSql, -1, &pStmt, &zLeftover);
    assert( rc==CAPDB_OK || pStmt==0 );
    if( rc!=CAPDB_OK ){
      continue;
    }
    if( !pStmt ){
      /* this happens for a comment or white-space */
      zSql = zLeftover;
      continue;
    }
    callbackIsInit = 0;

    while( 1 ){
      int i;
      rc = capdb_step(pStmt);

      /* Invoke the callback function if required */
      if( xCallback && (CAPDB_ROW==rc || 
          (CAPDB_DONE==rc && !callbackIsInit
                           && db->flags&CAPDB_NullCallback)) ){
        if( !callbackIsInit ){
          nCol = capdb_column_count(pStmt);
          azCols = capdbDbMallocRaw(db, (2*nCol+1)*sizeof(const char*));
          if( azCols==0 ){
            goto exec_out;
          }
          for(i=0; i<nCol; i++){
            azCols[i] = (char *)capdb_column_name(pStmt, i);
            /* capdbVdbeSetColName() installs column names as UTF8
            ** strings so there is no way for capdb_column_name() to fail. */
            assert( azCols[i]!=0 );
          }
          callbackIsInit = 1;
        }
        if( rc==CAPDB_ROW ){
          azVals = &azCols[nCol];
          for(i=0; i<nCol; i++){
            azVals[i] = (char *)capdb_column_text(pStmt, i);
            if( !azVals[i] && capdb_column_type(pStmt, i)!=CAPDB_NULL ){
              capdbOomFault(db);
              goto exec_out;
            }
          }
          azVals[i] = 0;
        }
        if( xCallback(pArg, nCol, azVals, azCols) ){
          /* EVIDENCE-OF: R-38229-40159 If the callback function to
          ** capdb_exec() returns non-zero, then capdb_exec() will
          ** return CAPDB_ABORT. */
          rc = CAPDB_ABORT;
          capdbVdbeFinalize((Vdbe *)pStmt);
          pStmt = 0;
          capdbError(db, CAPDB_ABORT);
          goto exec_out;
        }
      }

      if( rc!=CAPDB_ROW ){
        rc = capdbVdbeFinalize((Vdbe *)pStmt);
        pStmt = 0;
        zSql = zLeftover;
        while( capdbIsspace(zSql[0]) ) zSql++;
        break;
      }
    }

    capdbDbFree(db, azCols);
    azCols = 0;
  }

exec_out:
  if( pStmt ) capdbVdbeFinalize((Vdbe *)pStmt);
  capdbDbFree(db, azCols);

  rc = capdbApiExit(db, rc);
  if( rc!=CAPDB_OK && pzErrMsg ){
    *pzErrMsg = capdbDbStrDup(0, capdb_errmsg(db));
    if( *pzErrMsg==0 ){
      rc = CAPDB_NOMEM_BKPT;
      capdbError(db, CAPDB_NOMEM);
    }
  }else if( pzErrMsg ){
    *pzErrMsg = 0;
  }

  assert( (rc&db->errMask)==rc );
  capdb_mutex_leave(db->mutex);
  return rc;
}
