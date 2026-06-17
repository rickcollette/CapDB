/*
** 2017-10-11
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
** This file contains an implementation of the "sqlite_dbpage" virtual table.
**
** The sqlite_dbpage virtual table is used to read or write whole raw
** pages of the database file.  The pager interface is used so that 
** uncommitted changes and changes recorded in the WAL file are correctly
** retrieved.
**
** Usage example:
**
**    SELECT data FROM sqlite_dbpage('aux1') WHERE pgno=123;
**
** This is an eponymous virtual table so it does not need to be created before
** use.  The optional argument to the sqlite_dbpage() table name is the
** schema for the database file that is to be read.  The default schema is
** "main".
**
** The data field of sqlite_dbpage table can be updated.  The new
** value must be a BLOB which is the correct page size, otherwise the
** update fails.  INSERT operations also work, and operate as if they
** where REPLACE.  The size of the database can be extended by INSERT-ing
** new pages on the end.
**
** Rows may not be deleted.  However, doing an INSERT to page number N
** with NULL page data causes the N-th page and all subsequent pages to be
** deleted and the database to be truncated.
*/

#include "capdbInt.h"   /* Requires access to internal data structures */
#if (defined(CAPDB_ENABLE_DBPAGE_VTAB) || defined(CAPDB_TEST)) \
    && !defined(CAPDB_OMIT_VIRTUALTABLE)

typedef struct DbpageTable DbpageTable;
typedef struct DbpageCursor DbpageCursor;

struct DbpageCursor {
  capdb_vtab_cursor base;       /* Base class.  Must be first */
  Pgno pgno;                      /* Current page number */
  Pgno mxPgno;                    /* Last page to visit on this scan */
  Pager *pPager;                  /* Pager being read/written */
  DbPage *pPage1;                 /* Page 1 of the database */
  int iDb;                        /* Index of database to analyze */
  int szPage;                     /* Size of each page in bytes */
};

struct DbpageTable {
  capdb_vtab base;              /* Base class.  Must be first */
  capdb *db;                    /* The database */
  int iDbTrunc;                   /* Database to truncate */
  Pgno pgnoTrunc;                 /* Size to truncate to */
};

/* Columns */
#define DBPAGE_COLUMN_PGNO    0
#define DBPAGE_COLUMN_DATA    1
#define DBPAGE_COLUMN_SCHEMA  2


/*
** Connect to or create a dbpagevfs virtual table.
*/
static int dbpageConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  DbpageTable *pTab = 0;
  int rc = CAPDB_OK;
  (void)pAux;
  (void)argc;
  (void)argv;
  (void)pzErr;

  capdb_vtab_config(db, CAPDB_VTAB_DIRECTONLY);
  capdb_vtab_config(db, CAPDB_VTAB_USES_ALL_SCHEMAS);
  rc = capdb_declare_vtab(db, 
          "CREATE TABLE x(pgno INTEGER PRIMARY KEY, data BLOB, schema HIDDEN)");
  if( rc==CAPDB_OK ){
    pTab = (DbpageTable *)capdb_malloc64(sizeof(DbpageTable));
    if( pTab==0 ) rc = CAPDB_NOMEM_BKPT;
  }

  assert( rc==CAPDB_OK || pTab==0 );
  if( rc==CAPDB_OK ){
    memset(pTab, 0, sizeof(DbpageTable));
    pTab->db = db;
  }

  *ppVtab = (capdb_vtab*)pTab;
  return rc;
}

/*
** Disconnect from or destroy a dbpagevfs virtual table.
*/
static int dbpageDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** idxNum:
**
**     0     schema=main, full table scan
**     1     schema=main, pgno=?1
**     2     schema=?1, full table scan
**     3     schema=?1, pgno=?2
*/
static int dbpageBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  int i;
  int iPlan = 0;
  (void)tab;

  /* If there is a schema= constraint, it must be honored.  Report a
  ** ridiculously large estimated cost if the schema= constraint is
  ** unavailable
  */
  for(i=0; i<pIdxInfo->nConstraint; i++){
    struct capdb_index_constraint *p = &pIdxInfo->aConstraint[i];
    if( p->iColumn!=DBPAGE_COLUMN_SCHEMA ) continue;
    if( p->op!=CAPDB_INDEX_CONSTRAINT_EQ ) continue;
    if( !p->usable ){
      /* No solution. */
      return CAPDB_CONSTRAINT;
    }
    iPlan = 2;
    pIdxInfo->aConstraintUsage[i].argvIndex = 1;
    pIdxInfo->aConstraintUsage[i].omit = 1;
    break;
  }

  /* If we reach this point, it means that either there is no schema=
  ** constraint (in which case we use the "main" schema) or else the
  ** schema constraint was accepted.  Lower the estimated cost accordingly
  */
  pIdxInfo->estimatedCost = 1.0e6;

  /* Check for constraints against pgno */
  for(i=0; i<pIdxInfo->nConstraint; i++){
    struct capdb_index_constraint *p = &pIdxInfo->aConstraint[i];
    if( p->usable && p->iColumn<=0 && p->op==CAPDB_INDEX_CONSTRAINT_EQ ){
      pIdxInfo->estimatedRows = 1;
      pIdxInfo->idxFlags = CAPDB_INDEX_SCAN_UNIQUE;
      pIdxInfo->estimatedCost = 1.0;
      pIdxInfo->aConstraintUsage[i].argvIndex = iPlan ? 2 : 1;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      iPlan |= 1;
      break;
    }
  }
  pIdxInfo->idxNum = iPlan;

  if( pIdxInfo->nOrderBy>=1
   && pIdxInfo->aOrderBy[0].iColumn<=0
   && pIdxInfo->aOrderBy[0].desc==0
  ){
    pIdxInfo->orderByConsumed = 1;
  }
  return CAPDB_OK;
}

/*
** Open a new dbpagevfs cursor.
*/
static int dbpageOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  DbpageCursor *pCsr;

  pCsr = (DbpageCursor *)capdb_malloc64(sizeof(DbpageCursor));
  if( pCsr==0 ){
    return CAPDB_NOMEM_BKPT;
  }else{
    memset(pCsr, 0, sizeof(DbpageCursor));
    pCsr->base.pVtab = pVTab;
    pCsr->pgno = 0;
  }

  *ppCursor = (capdb_vtab_cursor *)pCsr;
  return CAPDB_OK;
}

/*
** Close a dbpagevfs cursor.
*/
static int dbpageClose(capdb_vtab_cursor *pCursor){
  DbpageCursor *pCsr = (DbpageCursor *)pCursor;
  if( pCsr->pPage1 ) capdbPagerUnrefPageOne(pCsr->pPage1);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** Move a dbpagevfs cursor to the next entry in the file.
*/
static int dbpageNext(capdb_vtab_cursor *pCursor){
  int rc = CAPDB_OK;
  DbpageCursor *pCsr = (DbpageCursor *)pCursor;
  pCsr->pgno++;
  return rc;
}

static int dbpageEof(capdb_vtab_cursor *pCursor){
  DbpageCursor *pCsr = (DbpageCursor *)pCursor;
  return pCsr->pgno > pCsr->mxPgno;
}

/*
** idxNum:
**
**     0     schema=main, full table scan
**     1     schema=main, pgno=?1
**     2     schema=?1, full table scan
**     3     schema=?1, pgno=?2
**
** idxStr is not used
*/
static int dbpageFilter(
  capdb_vtab_cursor *pCursor,
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  DbpageCursor *pCsr = (DbpageCursor *)pCursor;
  DbpageTable *pTab = (DbpageTable *)pCursor->pVtab;
  int rc;
  capdb *db = pTab->db;
  Btree *pBt;

  UNUSED_PARAMETER(idxStr);
  UNUSED_PARAMETER(argc);

  /* Default setting is no rows of result */
  pCsr->pgno = 1;
  pCsr->mxPgno = 0;

  if( idxNum & 2 ){
    const char *zSchema;
    assert( argc>=1 );
    zSchema = (const char*)capdb_value_text(argv[0]);
    pCsr->iDb = capdbFindDbName(db, zSchema);
    if( pCsr->iDb<0 ) return CAPDB_OK;
  }else{
    pCsr->iDb = 0;
  }
  pBt = db->aDb[pCsr->iDb].pBt;
  if( NEVER(pBt==0) ) return CAPDB_OK;
  pCsr->pPager = capdbBtreePager(pBt);
  pCsr->szPage = capdbBtreeGetPageSize(pBt);
  pCsr->mxPgno = capdbBtreeLastPage(pBt);
  if( idxNum & 1 ){
    i64 iPg = capdb_value_int64(argv[idxNum>>1]);
    assert( argc>(idxNum>>1) );
    if( iPg<1 || iPg>pCsr->mxPgno ){
      pCsr->pgno = 1;
      pCsr->mxPgno = 0;
    }else{
      pCsr->pgno = (Pgno)iPg;
      pCsr->mxPgno = pCsr->pgno;
    }
  }else{
    assert( pCsr->pgno==1 );
  }
  if( pCsr->pPage1 ) capdbPagerUnrefPageOne(pCsr->pPage1);
  rc = capdbPagerGet(pCsr->pPager, 1, &pCsr->pPage1, 0);
  return rc;
}

static int dbpageColumn(
  capdb_vtab_cursor *pCursor,
  capdb_context *ctx,
  int i
){
  DbpageCursor *pCsr = (DbpageCursor *)pCursor;
  int rc = CAPDB_OK;
  switch( i ){
    case 0: {           /* pgno */
      capdb_result_int64(ctx, (capdb_int64)pCsr->pgno);
      break;
    }
    case 1: {           /* data */
      DbPage *pDbPage = 0;
      if( pCsr->pgno==(Pgno)((PENDING_BYTE/pCsr->szPage)+1) ){
        /* The pending byte page. Assume it is zeroed out. Attempting to
        ** request this page from the page is an CAPDB_CORRUPT error. */
        capdb_result_zeroblob(ctx, pCsr->szPage);
      }else{
        rc = capdbPagerGet(pCsr->pPager, pCsr->pgno, (DbPage**)&pDbPage, 0);
        if( rc==CAPDB_OK ){
          capdb_result_blob(ctx, capdbPagerGetData(pDbPage), pCsr->szPage,
              CAPDB_TRANSIENT);
        }
        capdbPagerUnref(pDbPage);
      }
      break;
    }
    default: {          /* schema */
      capdb *db = capdb_context_db_handle(ctx);
      capdb_result_text(ctx, db->aDb[pCsr->iDb].zDbSName, -1, CAPDB_STATIC);
      break;
    }
  }
  return rc;
}

static int dbpageRowid(capdb_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  DbpageCursor *pCsr = (DbpageCursor *)pCursor;
  *pRowid = pCsr->pgno;
  return CAPDB_OK;
}

/* 
** Open write transactions. Since we do not know in advance which database
** files will be written by the sqlite_dbpage virtual table, start a write
** transaction on them all.
**
** Return CAPDB_OK if successful, or an SQLite error code otherwise.
*/
static int dbpageBeginTrans(DbpageTable *pTab){
  capdb *db = pTab->db;
  int rc = CAPDB_OK;
  int i;
  for(i=0; rc==CAPDB_OK && i<db->nDb; i++){
    Btree *pBt = db->aDb[i].pBt;
    if( pBt ) rc = capdbBtreeBeginTrans(pBt, 1, 0);
  }
  return rc;
}

static int dbpageUpdate(
  capdb_vtab *pVtab,
  int argc,
  capdb_value **argv,
  sqlite_int64 *pRowid
){
  DbpageTable *pTab = (DbpageTable *)pVtab;
  Pgno pgno;
  capdb_int64 pgno64;
  DbPage *pDbPage = 0;
  int rc = CAPDB_OK;
  char *zErr = 0;
  int iDb;
  Btree *pBt;
  Pager *pPager;
  int szPage;
  int isInsert;

  (void)pRowid;
  if( pTab->db->flags & CAPDB_Defensive ){
    zErr = "read-only";
    goto update_fail;
  }
  if( argc==1 ){
    zErr = "cannot delete";
    goto update_fail;
  }
  if( capdb_value_type(argv[0])==CAPDB_NULL ){
    pgno64 = capdb_value_int64(argv[2]);
    isInsert = 1;
  }else{
    pgno64 = (Pgno)capdb_value_int64(argv[0]);
    if( capdb_value_int64(argv[1])!=pgno64 ){
      zErr = "cannot insert";
      goto update_fail;
    }
    isInsert = 0;
  }
  if( capdb_value_type(argv[4])==CAPDB_NULL ){
    iDb = 0;
  }else{
    const char *zSchema = (const char*)capdb_value_text(argv[4]);
    iDb = capdbFindDbName(pTab->db, zSchema);
    if( iDb<0 ){
      zErr = "no such schema";
      goto update_fail;
    }
  }
  pBt = pTab->db->aDb[iDb].pBt;
  if( pgno64<1 || pgno64>4294967294 || NEVER(pBt==0) ){
    zErr = "bad page number";
    goto update_fail;
  }
  pgno = (Pgno)pgno64;
  szPage = capdbBtreeGetPageSize(pBt);
  if( capdb_value_type(argv[3])!=CAPDB_BLOB 
   || capdb_value_bytes(argv[3])!=szPage
  ){
    if( capdb_value_type(argv[3])==CAPDB_NULL && isInsert && pgno>1 ){
      /* "INSERT INTO dbpage($PGNO,NULL)" causes page number $PGNO and
      ** all subsequent pages to be deleted. */
      pTab->iDbTrunc = iDb;
      pTab->pgnoTrunc = pgno-1;
      pgno = 1;
    }else{
      zErr = "bad page value";
      goto update_fail;
    }
  }

  if( dbpageBeginTrans(pTab)!=CAPDB_OK ){
    zErr = "failed to open transaction";
    goto update_fail;
  }

  pPager = capdbBtreePager(pBt);
  rc = capdbPagerGet(pPager, pgno, (DbPage**)&pDbPage, 0);
  if( rc==CAPDB_OK ){
    const void *pData = capdb_value_blob(argv[3]);
    if( (rc = capdbPagerWrite(pDbPage))==CAPDB_OK && pData ){
      unsigned char *aPage = capdbPagerGetData(pDbPage);
      memcpy(aPage, pData, szPage);
      pTab->pgnoTrunc = 0;
    }
  }
  if( rc!=CAPDB_OK ){
    pTab->pgnoTrunc = 0;
  }
  capdbPagerUnref(pDbPage);
  return rc;

update_fail:
  pTab->pgnoTrunc = 0;
  capdb_free(pVtab->zErrMsg);
  pVtab->zErrMsg = capdb_mprintf("%s", zErr);
  return CAPDB_ERROR;
}

static int dbpageBegin(capdb_vtab *pVtab){
  DbpageTable *pTab = (DbpageTable *)pVtab;
  pTab->pgnoTrunc = 0;
  return CAPDB_OK;
}

/* Invoke capdbPagerTruncate() as necessary, just prior to COMMIT
*/
static int dbpageSync(capdb_vtab *pVtab){
  DbpageTable *pTab = (DbpageTable *)pVtab;
  if( pTab->pgnoTrunc>0 ){
    Btree *pBt = pTab->db->aDb[pTab->iDbTrunc].pBt;
    Pager *pPager = capdbBtreePager(pBt);
    capdbBtreeEnter(pBt);
    if( pTab->pgnoTrunc<capdbBtreeLastPage(pBt) ){
      capdbPagerTruncateImage(pPager, pTab->pgnoTrunc);
    }
    capdbBtreeLeave(pBt);
  }
  pTab->pgnoTrunc = 0;
  return CAPDB_OK;
}

/* Cancel any pending truncate.
*/
static int dbpageRollbackTo(capdb_vtab *pVtab, int notUsed1){
  DbpageTable *pTab = (DbpageTable *)pVtab;
  pTab->pgnoTrunc = 0;
  (void)notUsed1;
  return CAPDB_OK;
}

/*
** Invoke this routine to register the "dbpage" virtual table module
*/
int capdbDbpageRegister(capdb *db){
  static capdb_module dbpage_module = {
    2,                            /* iVersion */
    dbpageConnect,                /* xCreate */
    dbpageConnect,                /* xConnect */
    dbpageBestIndex,              /* xBestIndex */
    dbpageDisconnect,             /* xDisconnect */
    dbpageDisconnect,             /* xDestroy */
    dbpageOpen,                   /* xOpen - open a cursor */
    dbpageClose,                  /* xClose - close a cursor */
    dbpageFilter,                 /* xFilter - configure scan constraints */
    dbpageNext,                   /* xNext - advance a cursor */
    dbpageEof,                    /* xEof - check for end of scan */
    dbpageColumn,                 /* xColumn - read data */
    dbpageRowid,                  /* xRowid - read data */
    dbpageUpdate,                 /* xUpdate */
    dbpageBegin,                  /* xBegin */
    dbpageSync,                   /* xSync */
    0,                            /* xCommit */
    0,                            /* xRollback */
    0,                            /* xFindMethod */
    0,                            /* xRename */
    0,                            /* xSavepoint */
    0,                            /* xRelease */
    dbpageRollbackTo,             /* xRollbackTo */
    0,                            /* xShadowName */
    0                             /* xIntegrity */
  };
  return capdb_create_module(db, "sqlite_dbpage", &dbpage_module, 0);
}
#elif defined(CAPDB_ENABLE_DBPAGE_VTAB)
int capdbDbpageRegister(capdb *db){ return CAPDB_OK; }
#endif /* CAPDB_ENABLE_DBSTAT_VTAB */
