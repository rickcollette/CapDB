/*
** 2017-10-24
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
** This file contains an implementation of the "sqlite_btreeinfo" virtual table.
**
** The sqlite_btreeinfo virtual table is a read-only eponymous-only virtual
** table that shows information about all btrees in an SQLite database file.
** The schema is like this:
**
** CREATE TABLE sqlite_btreeinfo(
**    type TEXT,                   -- "table" or "index"
**    name TEXT,                   -- Name of table or index for this btree.
**    tbl_name TEXT,               -- Associated table
**    rootpage INT,                -- The root page of the btree
**    sql TEXT,                    -- SQL for this btree - from sqlite_schema
**    hasRowid BOOLEAN,            -- True if the btree has a rowid
**    nEntry INT,                  -- Estimated number of entries
**    nPage INT,                   -- Estimated number of pages
**    depth INT,                   -- Depth of the btree
**    szPage INT,                  -- Size of each page in bytes
**    zSchema TEXT HIDDEN          -- The schema to which this btree belongs
** );
**
** The first 5 fields are taken directly from the sqlite_schema table.
** Considering only the first 5 fields, the only difference between 
** this virtual table and the sqlite_schema table is that this virtual
** table omits all entries that have a 0 or NULL rowid - in other words
** it omits triggers and views.
**
** The value added by this table comes in the next 5 fields.
**
** Note that nEntry and nPage are *estimated*.  They are computed doing
** a single search from the root to a leaf, counting the number of cells
** at each level, and assuming that unvisited pages have a similar number
** of cells.
**
** The sqlite_dbpage virtual table must be available for this virtual table
** to operate.
**
** USAGE EXAMPLES:
**
** Show the table btrees in a schema order with the tables with the most
** rows occurring first:
**
**      SELECT name, nEntry
**        FROM sqlite_btreeinfo
**       WHERE type='table'
**       ORDER BY nEntry DESC, name;
**
** Show the names of all WITHOUT ROWID tables: 
**
**      SELECT name FROM sqlite_btreeinfo
**       WHERE type='table' AND NOT hasRowid;
**
** UNUSED, UNTESTED, UNSUPPORTED
**
** This extension exists for information and demonstration purposes
** only.  The developers are not aware of any real-world use of this
** extension.  The extension has no formal test cases.  The developers
** do not actively support it.
*/
#if !defined(SQLITEINT_H)
#include "capdbext.h"
#endif
CAPDB_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

/* Columns available in this virtual table */
#define BINFO_COLUMN_TYPE         0
#define BINFO_COLUMN_NAME         1
#define BINFO_COLUMN_TBL_NAME     2
#define BINFO_COLUMN_ROOTPAGE     3
#define BINFO_COLUMN_SQL          4
#define BINFO_COLUMN_HASROWID     5
#define BINFO_COLUMN_NENTRY       6
#define BINFO_COLUMN_NPAGE        7
#define BINFO_COLUMN_DEPTH        8
#define BINFO_COLUMN_SZPAGE       9
#define BINFO_COLUMN_SCHEMA      10

/* Forward declarations */
typedef struct BinfoTable BinfoTable;
typedef struct BinfoCursor BinfoCursor;

/* A cursor for the sqlite_btreeinfo table */
struct BinfoCursor {
  capdb_vtab_cursor base;       /* Base class.  Must be first */
  capdb_stmt *pStmt;            /* Query against sqlite_schema */
  int rc;                         /* Result of previous sqlite_step() call */
  int hasRowid;                   /* hasRowid value.  Negative if unknown. */
  capdb_int64 nEntry;           /* nEntry value */
  int nPage;                      /* nPage value */
  int depth;                      /* depth value */
  int szPage;                     /* size of a btree page.  0 if unknown */
  char *zSchema;                  /* Schema being interrogated */
};

/* The sqlite_btreeinfo table */
struct BinfoTable {
  capdb_vtab base;              /* Base class.  Must be first */
  capdb *db;                    /* The databse connection */
};

/*
** Connect to the sqlite_btreeinfo virtual table.
*/
static int binfoConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  BinfoTable *pTab = 0;
  int rc = CAPDB_OK;
  rc = capdb_declare_vtab(db, 
      "CREATE TABLE x(\n"
      " type TEXT,\n"
      " name TEXT,\n"
      " tbl_name TEXT,\n"
      " rootpage INT,\n"
      " sql TEXT,\n"
      " hasRowid BOOLEAN,\n"
      " nEntry INT,\n"
      " nPage INT,\n"
      " depth INT,\n"
      " szPage INT,\n"
      " zSchema TEXT HIDDEN\n"
      ")");
  if( rc==CAPDB_OK ){
    pTab = (BinfoTable *)capdb_malloc64(sizeof(BinfoTable));
    if( pTab==0 ) rc = CAPDB_NOMEM;
  }
  assert( rc==CAPDB_OK || pTab==0 );
  if( pTab ){
    pTab->db = db;
  }
  *ppVtab = (capdb_vtab*)pTab;
  return rc;
}

/*
** Disconnect from or destroy a btreeinfo virtual table.
*/
static int binfoDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** idxNum:
**
**     0     Use "main" for the schema
**     1     Schema identified by parameter ?1
*/
static int binfoBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  int i;
  pIdxInfo->estimatedCost = 10000.0;  /* Cost estimate */
  pIdxInfo->estimatedRows = 100;
  for(i=0; i<pIdxInfo->nConstraint; i++){
    struct capdb_index_constraint *p = &pIdxInfo->aConstraint[i];
    if( p->usable
     && p->iColumn==BINFO_COLUMN_SCHEMA
     && p->op==CAPDB_INDEX_CONSTRAINT_EQ
    ){
      pIdxInfo->estimatedCost = 1000.0;
      pIdxInfo->idxNum = 1;
      pIdxInfo->aConstraintUsage[i].argvIndex = 1;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      break;
    }
  }
  return CAPDB_OK;
}

/*
** Open a new btreeinfo cursor.
*/
static int binfoOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  BinfoCursor *pCsr;

  pCsr = (BinfoCursor *)capdb_malloc64(sizeof(BinfoCursor));
  if( pCsr==0 ){
    return CAPDB_NOMEM;
  }else{
    memset(pCsr, 0, sizeof(BinfoCursor));
    pCsr->base.pVtab = pVTab;
  }

  *ppCursor = (capdb_vtab_cursor *)pCsr;
  return CAPDB_OK;
}

/*
** Close a btreeinfo cursor.
*/
static int binfoClose(capdb_vtab_cursor *pCursor){
  BinfoCursor *pCsr = (BinfoCursor *)pCursor;
  capdb_finalize(pCsr->pStmt);
  capdb_free(pCsr->zSchema);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** Move a btreeinfo cursor to the next entry in the file.
*/
static int binfoNext(capdb_vtab_cursor *pCursor){
  BinfoCursor *pCsr = (BinfoCursor *)pCursor;
  pCsr->rc = capdb_step(pCsr->pStmt);
  pCsr->hasRowid = -1;
  return pCsr->rc==CAPDB_ERROR ? CAPDB_ERROR : CAPDB_OK;
}

/* We have reached EOF if previous capdb_step() returned
** anything other than CAPDB_ROW;
*/
static int binfoEof(capdb_vtab_cursor *pCursor){
  BinfoCursor *pCsr = (BinfoCursor *)pCursor;
  return pCsr->rc!=CAPDB_ROW;
}

/* Position a cursor back to the beginning.
*/
static int binfoFilter(
  capdb_vtab_cursor *pCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  BinfoCursor *pCsr = (BinfoCursor *)pCursor;
  BinfoTable *pTab = (BinfoTable *)pCursor->pVtab;
  char *zSql;
  int rc;

  capdb_free(pCsr->zSchema);
  if( idxNum==1 && capdb_value_type(argv[0])!=CAPDB_NULL ){
    pCsr->zSchema = capdb_mprintf("%s", capdb_value_text(argv[0]));
  }else{
    pCsr->zSchema = capdb_mprintf("main");
  }
  zSql = capdb_mprintf(
      "SELECT 0, 'table','sqlite_schema','sqlite_schema',1,NULL "
      "UNION ALL "
      "SELECT rowid, type, name, tbl_name, rootpage, sql"
      " FROM \"%w\".sqlite_schema WHERE rootpage>=1",
       pCsr->zSchema);
  capdb_finalize(pCsr->pStmt);
  pCsr->pStmt = 0;
  pCsr->hasRowid = -1;
  rc = capdb_prepare_v2(pTab->db, zSql, -1, &pCsr->pStmt, 0);
  capdb_free(zSql);
  if( rc==CAPDB_OK ){
    rc = binfoNext(pCursor);
  }
  return rc;
}

/* Decode big-endian integers */
static unsigned int get_uint16(unsigned char *a){
  return (a[0]<<8)|a[1];
}
static unsigned int get_uint32(unsigned char *a){
  return (a[0]<<24)|(a[1]<<16)|(a[2]<<8)|a[3];
}

/* Examine the b-tree rooted at pgno and estimate its size.
** Return non-zero if anything goes wrong.
*/
static int binfoCompute(capdb *db, int pgno, BinfoCursor *pCsr){
  capdb_int64 nEntry = 1;
  int nPage = 1;
  unsigned char *aData;
  capdb_stmt *pStmt = 0;
  int rc = CAPDB_OK;
  int pgsz = 0;
  int nCell;
  int iCell;

  rc = capdb_prepare_v2(db, 
           "SELECT data FROM sqlite_dbpage('main') WHERE pgno=?1", -1,
           &pStmt, 0);
  if( rc ) return rc;
  pCsr->depth = 1;
  while(1){
    capdb_bind_int(pStmt, 1, pgno);
    if( pCsr->depth>25 ){
      capdb_set_errmsg(db, CAPDB_CORRUPT, "btree nested too deep");
      rc = CAPDB_ERROR;
      break;
    }
    rc = capdb_step(pStmt);
    if( rc!=CAPDB_ROW ){
      rc = CAPDB_ERROR;
      break;
    }
    pCsr->szPage = pgsz = capdb_column_bytes(pStmt, 0);
    aData = (unsigned char*)capdb_column_blob(pStmt, 0);
    if( aData==0 ){    
      rc = CAPDB_NOMEM;
      break;
    }
    if( pgno==1 ){
      aData += 100;
      pgsz -= 100;
    }
    pCsr->hasRowid = aData[0]!=2 && aData[0]!=10;
    nCell = get_uint16(aData+3);
    nEntry *= (nCell+1);
    if( aData[0]==10 || aData[0]==13 ) break;
    nPage *= (nCell+1);
    if( 14+2*(nCell/2)>=pgsz ){
      rc = CAPDB_CORRUPT;
      break;
    }
    if( nCell<=1 ){
      pgno = get_uint32(aData+8);
    }else{
      iCell = get_uint16(aData+12+2*(nCell/2));
      if( pgno==1 ) iCell -= 100;
      if( iCell<=12 || iCell>=pgsz-4 ){
        rc = CAPDB_CORRUPT;
        break;
      }
      pgno = get_uint32(aData+iCell);
    }
    pCsr->depth++;
    capdb_reset(pStmt);
  }
  capdb_finalize(pStmt);
  pCsr->nPage = nPage;
  pCsr->nEntry = nEntry;
  if( rc==CAPDB_ROW ) rc = CAPDB_OK;
  return rc;
}

/* Return a column for the sqlite_btreeinfo table */
static int binfoColumn(
  capdb_vtab_cursor *pCursor, 
  capdb_context *ctx, 
  int i
){
  BinfoCursor *pCsr = (BinfoCursor *)pCursor;
  if( i>=BINFO_COLUMN_HASROWID && i<=BINFO_COLUMN_SZPAGE && pCsr->hasRowid<0 ){
    int pgno = capdb_column_int(pCsr->pStmt, BINFO_COLUMN_ROOTPAGE+1);
    capdb *db = capdb_context_db_handle(ctx);
    int rc = binfoCompute(db, pgno, pCsr);
    if( rc ){
      pCursor->pVtab->zErrMsg = capdb_mprintf("%s", capdb_errstr(rc));
      return CAPDB_ERROR;
    }
  }
  switch( i ){
    case BINFO_COLUMN_NAME:
    case BINFO_COLUMN_TYPE:
    case BINFO_COLUMN_TBL_NAME:
    case BINFO_COLUMN_ROOTPAGE:
    case BINFO_COLUMN_SQL: {
      capdb_result_value(ctx, capdb_column_value(pCsr->pStmt, i+1));
      break;
    }
    case BINFO_COLUMN_HASROWID: {
      capdb_result_int(ctx, pCsr->hasRowid);
      break;
    }
    case BINFO_COLUMN_NENTRY: {
      capdb_result_int64(ctx, pCsr->nEntry);
      break;
    }
    case BINFO_COLUMN_NPAGE: {
      capdb_result_int(ctx, pCsr->nPage);
      break;
    }
    case BINFO_COLUMN_DEPTH: {
      capdb_result_int(ctx, pCsr->depth);
      break;
    }
    case BINFO_COLUMN_SCHEMA: {
      capdb_result_text(ctx, pCsr->zSchema, -1, CAPDB_STATIC);
      break;
    }
  }
  return CAPDB_OK;
}

/* Return the ROWID for the sqlite_btreeinfo table */
static int binfoRowid(capdb_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  BinfoCursor *pCsr = (BinfoCursor *)pCursor;
  *pRowid = capdb_column_int64(pCsr->pStmt, 0);
  return CAPDB_OK;
}

/*
** Invoke this routine to register the "sqlite_btreeinfo" virtual table module
*/
int capdbBinfoRegister(capdb *db){
  static capdb_module binfo_module = {
    0,                           /* iVersion */
    0,                           /* xCreate */
    binfoConnect,                /* xConnect */
    binfoBestIndex,              /* xBestIndex */
    binfoDisconnect,             /* xDisconnect */
    0,                           /* xDestroy */
    binfoOpen,                   /* xOpen - open a cursor */
    binfoClose,                  /* xClose - close a cursor */
    binfoFilter,                 /* xFilter - configure scan constraints */
    binfoNext,                   /* xNext - advance a cursor */
    binfoEof,                    /* xEof - check for end of scan */
    binfoColumn,                 /* xColumn - read data */
    binfoRowid,                  /* xRowid - read data */
    0,                           /* xUpdate */
    0,                           /* xBegin */
    0,                           /* xSync */
    0,                           /* xCommit */
    0,                           /* xRollback */
    0,                           /* xFindMethod */
    0,                           /* xRename */
    0,                           /* xSavepoint */
    0,                           /* xRelease */
    0,                           /* xRollbackTo */
    0,                           /* xShadowName */
    0                            /* xIntegrity */
  };
  return capdb_create_module(db, "sqlite_btreeinfo", &binfo_module, 0);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_btreeinfo_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  CAPDB_EXTENSION_INIT2(pApi);
  return capdbBinfoRegister(db);
}
