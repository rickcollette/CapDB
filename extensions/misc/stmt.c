/*
** 2017-05-31
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
** This file demonstrates an eponymous virtual table that returns information
** about all prepared statements for the database connection.
**
** Usage example:
**
**     .load ./stmt
**     .mode line
**     .header on
**     SELECT * FROM stmt;
*/
#if !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_STMTVTAB)
#if !defined(SQLITEINT_H)
#include "capdbext.h"
#endif
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

#ifndef CAPDB_OMIT_VIRTUALTABLE


#define STMT_NUM_INTEGER_COLUMN 10
typedef struct StmtRow StmtRow;
struct StmtRow {
  capdb_int64 iRowid;                /* Rowid value */
  char *zSql;                          /* column "sql" */
  int aCol[STMT_NUM_INTEGER_COLUMN+1]; /* all other column values */
  StmtRow *pNext;                      /* Next row to return */
};

/* stmt_vtab is a subclass of capdb_vtab which will
** serve as the underlying representation of a stmt virtual table
*/
typedef struct stmt_vtab stmt_vtab;
struct stmt_vtab {
  capdb_vtab base;  /* Base class - must be first */
  capdb *db;        /* Database connection for this stmt vtab */
};

/* stmt_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct stmt_cursor stmt_cursor;
struct stmt_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb *db;               /* Database connection for this cursor */
  StmtRow *pRow;             /* Current row */
};

/*
** The stmtConnect() method is invoked to create a new
** stmt_vtab that describes the stmt virtual table.
**
** Think of this routine as the constructor for stmt_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the stmt_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the capdb_declare_vtab() interface) what the
**        result set of queries against stmt will look like.
*/
static int stmtConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  stmt_vtab *pNew;
  int rc;

/* Column numbers */
#define STMT_COLUMN_SQL     0   /* SQL for the statement */
#define STMT_COLUMN_NCOL    1   /* Number of result columns */
#define STMT_COLUMN_RO      2   /* True if read-only */
#define STMT_COLUMN_BUSY    3   /* True if currently busy */
#define STMT_COLUMN_NSCAN   4   /* CAPDB_STMTSTATUS_FULLSCAN_STEP */
#define STMT_COLUMN_NSORT   5   /* CAPDB_STMTSTATUS_SORT */
#define STMT_COLUMN_NAIDX   6   /* CAPDB_STMTSTATUS_AUTOINDEX */
#define STMT_COLUMN_NSTEP   7   /* CAPDB_STMTSTATUS_VM_STEP */
#define STMT_COLUMN_REPREP  8   /* CAPDB_STMTSTATUS_REPREPARE */
#define STMT_COLUMN_RUN     9   /* CAPDB_STMTSTATUS_RUN */
#define STMT_COLUMN_MEM    10   /* CAPDB_STMTSTATUS_MEMUSED */


  (void)pAux;
  (void)argc;
  (void)argv;
  (void)pzErr;
  rc = capdb_declare_vtab(db,
     "CREATE TABLE x(sql,ncol,ro,busy,nscan,nsort,naidx,nstep,"
                    "reprep,run,mem)");
  if( rc==CAPDB_OK ){
    pNew = capdb_malloc64( sizeof(*pNew) );
    *ppVtab = (capdb_vtab*)pNew;
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    pNew->db = db;
  }
  return rc;
}

/*
** This method is the destructor for stmt_cursor objects.
*/
static int stmtDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** Constructor for a new stmt_cursor object.
*/
static int stmtOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  stmt_cursor *pCur;
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->db = ((stmt_vtab*)p)->db;
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

static void stmtCsrReset(stmt_cursor *pCur){
  StmtRow *pRow = 0;
  StmtRow *pNext = 0;
  for(pRow=pCur->pRow; pRow; pRow=pNext){
    pNext = pRow->pNext;
    capdb_free(pRow);
  }
  pCur->pRow = 0;
}

/*
** Destructor for a stmt_cursor.
*/
static int stmtClose(capdb_vtab_cursor *cur){
  stmtCsrReset((stmt_cursor*)cur);
  capdb_free(cur);
  return CAPDB_OK;
}


/*
** Advance a stmt_cursor to its next row of output.
*/
static int stmtNext(capdb_vtab_cursor *cur){
  stmt_cursor *pCur = (stmt_cursor*)cur;
  StmtRow *pNext = pCur->pRow->pNext;
  capdb_free(pCur->pRow);
  pCur->pRow = pNext;
  return CAPDB_OK;
}

/*
** Return values of columns for the row at which the stmt_cursor
** is currently pointing.
*/
static int stmtColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  stmt_cursor *pCur = (stmt_cursor*)cur;
  StmtRow *pRow = pCur->pRow;
  if( i==STMT_COLUMN_SQL ){
    capdb_result_text(ctx, pRow->zSql, -1, CAPDB_TRANSIENT);
  }else{
    capdb_result_int(ctx, pRow->aCol[i]);
  }
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int stmtRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  stmt_cursor *pCur = (stmt_cursor*)cur;
  *pRowid = pCur->pRow->iRowid;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int stmtEof(capdb_vtab_cursor *cur){
  stmt_cursor *pCur = (stmt_cursor*)cur;
  return pCur->pRow==0;
}

/*
** This method is called to "rewind" the stmt_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to stmtColumn() or stmtRowid() or 
** stmtEof().
*/
static int stmtFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  stmt_cursor *pCur = (stmt_cursor *)pVtabCursor;
  capdb_stmt *p = 0;
  capdb_int64 iRowid = 1;
  StmtRow **ppRow = 0;

  (void)idxNum;
  (void)idxStr;
  (void)argc;
  (void)argv;
  stmtCsrReset(pCur);
  ppRow = &pCur->pRow;
  for(p=capdb_next_stmt(pCur->db, 0); p; p=capdb_next_stmt(pCur->db, p)){
    const char *zSql = capdb_sql(p);
    capdb_int64 nSql = zSql ? strlen(zSql)+1 : 0;
    StmtRow *pNew = (StmtRow*)capdb_malloc64(sizeof(StmtRow) + nSql);

    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(StmtRow));
    if( zSql ){
      pNew->zSql = (char*)&pNew[1];
      memcpy(pNew->zSql, zSql, nSql);
    }
    pNew->aCol[STMT_COLUMN_NCOL] = capdb_column_count(p);
    pNew->aCol[STMT_COLUMN_RO] = capdb_stmt_readonly(p);
    pNew->aCol[STMT_COLUMN_BUSY] = capdb_stmt_busy(p);
    pNew->aCol[STMT_COLUMN_NSCAN] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_FULLSCAN_STEP, 0
    );
    pNew->aCol[STMT_COLUMN_NSORT] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_SORT, 0
    );
    pNew->aCol[STMT_COLUMN_NAIDX] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_AUTOINDEX, 0
    );
    pNew->aCol[STMT_COLUMN_NSTEP] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_VM_STEP, 0
    );
    pNew->aCol[STMT_COLUMN_REPREP] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_REPREPARE, 0
    );
    pNew->aCol[STMT_COLUMN_RUN] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_RUN, 0
    );
    pNew->aCol[STMT_COLUMN_MEM] = capdb_stmt_status(
        p, CAPDB_STMTSTATUS_MEMUSED, 0
    );
    pNew->iRowid = iRowid++;
    *ppRow = pNew;
    ppRow = &pNew->pNext;
  }

  return CAPDB_OK;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the stmt virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int stmtBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  (void)tab;
  pIdxInfo->estimatedCost = (double)500;
  pIdxInfo->estimatedRows = 500;
  return CAPDB_OK;
}

/*
** This following structure defines all the methods for the 
** stmt virtual table.
*/
static capdb_module stmtModule = {
  0,                         /* iVersion */
  0,                         /* xCreate */
  stmtConnect,               /* xConnect */
  stmtBestIndex,             /* xBestIndex */
  stmtDisconnect,            /* xDisconnect */
  0,                         /* xDestroy */
  stmtOpen,                  /* xOpen - open a cursor */
  stmtClose,                 /* xClose - close a cursor */
  stmtFilter,                /* xFilter - configure scan constraints */
  stmtNext,                  /* xNext - advance a cursor */
  stmtEof,                   /* xEof - check for end of scan */
  stmtColumn,                /* xColumn - read data */
  stmtRowid,                 /* xRowid - read data */
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

int capdbStmtVtabInit(capdb *db){
  int rc = CAPDB_OK;
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdb_create_module(db, "sqlite_stmt", &stmtModule, 0);
#endif
  return rc;
}

#ifndef CAPDB_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_stmt_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdbStmtVtabInit(db);
#endif
  return rc;
}
#endif /* CAPDB_CORE */
#endif /* !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_STMTVTAB) */
