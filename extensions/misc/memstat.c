/*
** 2018-09-27
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
** from capdb_status64() and capdb_db_status().
**
** Usage example:
**
**     .load ./memstat
**     .mode quote
**     .header on
**     SELECT * FROM memstat;
*/
#if !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_MEMSTATVTAB)
#if !defined(SQLITEINT_H)
#include "capdbext.h"
#endif
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

#ifndef CAPDB_OMIT_VIRTUALTABLE

/* memstat_vtab is a subclass of capdb_vtab which will
** serve as the underlying representation of a memstat virtual table
*/
typedef struct memstat_vtab memstat_vtab;
struct memstat_vtab {
  capdb_vtab base;  /* Base class - must be first */
  capdb *db;        /* Database connection for this memstat vtab */
};

/* memstat_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct memstat_cursor memstat_cursor;
struct memstat_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb *db;               /* Database connection for this cursor */
  int iRowid;                /* Current row in aMemstatColumn[] */
  int iDb;                   /* Which schema we are looking at */
  int nDb;                   /* Number of schemas */
  char **azDb;               /* Names of all schemas */
  capdb_int64 aVal[2];     /* Result values */
};

/*
** The memstatConnect() method is invoked to create a new
** memstat_vtab that describes the memstat virtual table.
**
** Think of this routine as the constructor for memstat_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the memstat_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the capdb_declare_vtab() interface) what the
**        result set of queries against memstat will look like.
*/
static int memstatConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  memstat_vtab *pNew;
  int rc;

/* Column numbers */
#define MSV_COLUMN_NAME    0   /* Name of quantity being measured */
#define MSV_COLUMN_SCHEMA  1   /* schema name */
#define MSV_COLUMN_VALUE   2   /* Current value */
#define MSV_COLUMN_HIWTR   3   /* Highwater mark */

  rc = capdb_declare_vtab(db,"CREATE TABLE x(name,schema,value,hiwtr)");
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
** This method is the destructor for memstat_cursor objects.
*/
static int memstatDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** Constructor for a new memstat_cursor object.
*/
static int memstatOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  memstat_cursor *pCur;
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->db = ((memstat_vtab*)p)->db;
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Clear all the schema names from a cursor
*/
static void memstatClearSchema(memstat_cursor *pCur){
  int i;
  if( pCur->azDb==0 ) return;
  for(i=0; i<pCur->nDb; i++){
    capdb_free(pCur->azDb[i]);
  }
  capdb_free(pCur->azDb);
  pCur->azDb = 0;
  pCur->nDb = 0;
}

/*
** Fill in the azDb[] array for the cursor.
*/
static int memstatFindSchemas(memstat_cursor *pCur){
  capdb_stmt *pStmt = 0;
  int rc;
  if( pCur->nDb ) return CAPDB_OK;
  rc = capdb_prepare_v2(pCur->db, "PRAGMA database_list", -1, &pStmt, 0);
  if( rc ){
    capdb_finalize(pStmt);
    return rc;
  }
  while( capdb_step(pStmt)==CAPDB_ROW ){
    char **az, *z;
    az = capdb_realloc64(pCur->azDb, sizeof(char*)*(pCur->nDb+1));
    if( az==0 ){
      memstatClearSchema(pCur);
      return CAPDB_NOMEM;
    }
    pCur->azDb = az;
    z = capdb_mprintf("%s", capdb_column_text(pStmt, 1));
    if( z==0 ){
      memstatClearSchema(pCur);
      return CAPDB_NOMEM;
    }
    pCur->azDb[pCur->nDb] = z;
    pCur->nDb++;
  }
  capdb_finalize(pStmt);
  return CAPDB_OK;
}


/*
** Destructor for a memstat_cursor.
*/
static int memstatClose(capdb_vtab_cursor *cur){
  memstat_cursor *pCur = (memstat_cursor*)cur;
  memstatClearSchema(pCur);
  capdb_free(cur);
  return CAPDB_OK;
}


/*
** Allowed values for aMemstatColumn[].eType
*/
#define MSV_GSTAT   0          /* capdb_status64() information */
#define MSV_DB      1          /* capdb_db_status() information */
#define MSV_ZIPVFS  2          /* ZIPVFS file-control with 64-bit return */

/*
** An array of quantities that can be measured and reported by
** this virtual table
*/
static const struct MemstatColumns {
  const char *zName;    /* Symbolic name */
  unsigned char eType;  /* Type of interface */
  unsigned char mNull;  /* Bitmask of which columns are NULL */
                        /* 2: dbname,  4: current,  8: hiwtr */
  int eOp;              /* Opcode */
} aMemstatColumn[] = {
 {"MEMORY_USED",            MSV_GSTAT,  2, CAPDB_STATUS_MEMORY_USED          },
 {"MALLOC_SIZE",            MSV_GSTAT,  6, CAPDB_STATUS_MALLOC_SIZE          },
 {"MALLOC_COUNT",           MSV_GSTAT,  2, CAPDB_STATUS_MALLOC_COUNT         },
 {"PAGECACHE_USED",         MSV_GSTAT,  2, CAPDB_STATUS_PAGECACHE_USED       },
 {"PAGECACHE_OVERFLOW",     MSV_GSTAT,  2, CAPDB_STATUS_PAGECACHE_OVERFLOW   },
 {"PAGECACHE_SIZE",         MSV_GSTAT,  6, CAPDB_STATUS_PAGECACHE_SIZE       },
 {"PARSER_STACK",           MSV_GSTAT,  6, CAPDB_STATUS_PARSER_STACK         },
 {"DB_LOOKASIDE_USED",      MSV_DB,     2, CAPDB_DBSTATUS_LOOKASIDE_USED     },
 {"DB_LOOKASIDE_HIT",       MSV_DB,     6, CAPDB_DBSTATUS_LOOKASIDE_HIT      },
 {"DB_LOOKASIDE_MISS_SIZE", MSV_DB,     6, CAPDB_DBSTATUS_LOOKASIDE_MISS_SIZE},
 {"DB_LOOKASIDE_MISS_FULL", MSV_DB,     6, CAPDB_DBSTATUS_LOOKASIDE_MISS_FULL},
 {"DB_CACHE_USED",          MSV_DB,    10, CAPDB_DBSTATUS_CACHE_USED         },
#if CAPDB_VERSION_NUMBER >= 3140000
 {"DB_CACHE_USED_SHARED",   MSV_DB,    10, CAPDB_DBSTATUS_CACHE_USED_SHARED  },
#endif
 {"DB_SCHEMA_USED",         MSV_DB,    10, CAPDB_DBSTATUS_SCHEMA_USED        },
 {"DB_STMT_USED",           MSV_DB,    10, CAPDB_DBSTATUS_STMT_USED          },
 {"DB_CACHE_HIT",           MSV_DB,    10, CAPDB_DBSTATUS_CACHE_HIT          },
 {"DB_CACHE_MISS",          MSV_DB,    10, CAPDB_DBSTATUS_CACHE_MISS         },
 {"DB_CACHE_WRITE",         MSV_DB,    10, CAPDB_DBSTATUS_CACHE_WRITE        },
#if CAPDB_VERSION_NUMBER >= 3230000
 {"DB_CACHE_SPILL",         MSV_DB,    10, CAPDB_DBSTATUS_CACHE_SPILL        },
#endif
 {"DB_DEFERRED_FKS",        MSV_DB,    10, CAPDB_DBSTATUS_DEFERRED_FKS       },
#ifdef CAPDB_ENABLE_ZIPVFS
 {"ZIPVFS_CACHE_USED",      MSV_ZIPVFS, 8, 231454 },
 {"ZIPVFS_CACHE_HIT",       MSV_ZIPVFS, 8, 231455 },
 {"ZIPVFS_CACHE_MISS",      MSV_ZIPVFS, 8, 231456 },
 {"ZIPVFS_CACHE_WRITE",     MSV_ZIPVFS, 8, 231457 },
 {"ZIPVFS_DIRECT_READ",     MSV_ZIPVFS, 8, 231458 },
 {"ZIPVFS_DIRECT_BYTES",    MSV_ZIPVFS, 8, 231459 },
#endif /* CAPDB_ENABLE_ZIPVFS */
};
#define MSV_NROW (sizeof(aMemstatColumn)/sizeof(aMemstatColumn[0]))

/*
** Advance a memstat_cursor to its next row of output.
*/
static int memstatNext(capdb_vtab_cursor *cur){
  memstat_cursor *pCur = (memstat_cursor*)cur;
  int i;
  assert( pCur->iRowid<=MSV_NROW );
  while(1){
    i = (int)pCur->iRowid - 1;
    if( i<0 || (aMemstatColumn[i].mNull & 2)!=0 || (++pCur->iDb)>=pCur->nDb ){
      pCur->iRowid++;
      if( pCur->iRowid>MSV_NROW ) return CAPDB_OK;  /* End of the table */
      pCur->iDb = 0;
      i++;
    }
    pCur->aVal[0] = 0;
    pCur->aVal[1] = 0;    
    switch( aMemstatColumn[i].eType ){
      case MSV_GSTAT: {
        if( capdb_libversion_number()>=3010000 ){
          capdb_status64(aMemstatColumn[i].eOp,
                           &pCur->aVal[0], &pCur->aVal[1],0);
        }else{
          int xCur, xHiwtr;
          capdb_status(aMemstatColumn[i].eOp, &xCur, &xHiwtr, 0);
          pCur->aVal[0] = xCur;
          pCur->aVal[1] = xHiwtr;
        }
        break;
      }
      case MSV_DB: {
        int xCur, xHiwtr;
        capdb_db_status(pCur->db, aMemstatColumn[i].eOp, &xCur, &xHiwtr, 0);
        pCur->aVal[0] = xCur;
        pCur->aVal[1] = xHiwtr;
        break;
      }
      case MSV_ZIPVFS: {
        int rc;
        rc = capdb_file_control(pCur->db, pCur->azDb[pCur->iDb],
                                  aMemstatColumn[i].eOp, (void*)&pCur->aVal[0]);
        if( rc!=CAPDB_OK ) continue;
        break;
      }
    }
    break;
  }
  return CAPDB_OK;
}
  

/*
** Return values of columns for the row at which the memstat_cursor
** is currently pointing.
*/
static int memstatColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int iCol                    /* Which column to return */
){
  memstat_cursor *pCur = (memstat_cursor*)cur;
  int i;
  assert( pCur->iRowid>0 && pCur->iRowid<=MSV_NROW );
  i = (int)pCur->iRowid - 1;
  if( (aMemstatColumn[i].mNull & (1<<iCol))!=0 ){
    return CAPDB_OK;
  }
  switch( iCol ){
    case MSV_COLUMN_NAME: {
      capdb_result_text(ctx, aMemstatColumn[i].zName, -1, CAPDB_STATIC);
      break;
    }
    case MSV_COLUMN_SCHEMA: {
      capdb_result_text(ctx, pCur->azDb[pCur->iDb], -1, 0);
      break;
    }
    case MSV_COLUMN_VALUE: {
      capdb_result_int64(ctx, pCur->aVal[0]);
      break;
    }
    case MSV_COLUMN_HIWTR: {
      capdb_result_int64(ctx, pCur->aVal[1]);
      break;
    }
  }
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int memstatRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  memstat_cursor *pCur = (memstat_cursor*)cur;
  *pRowid = pCur->iRowid*1000 + pCur->iDb;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int memstatEof(capdb_vtab_cursor *cur){
  memstat_cursor *pCur = (memstat_cursor*)cur;
  return pCur->iRowid>MSV_NROW;
}

/*
** This method is called to "rewind" the memstat_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to memstatColumn() or memstatRowid() or 
** memstatEof().
*/
static int memstatFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  memstat_cursor *pCur = (memstat_cursor *)pVtabCursor;
  int rc = memstatFindSchemas(pCur);
  if( rc ) return rc;
  pCur->iRowid = 0;
  pCur->iDb = 0;
  return memstatNext(pVtabCursor);
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the memstat virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int memstatBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  pIdxInfo->estimatedCost = (double)500;
  pIdxInfo->estimatedRows = 500;
  return CAPDB_OK;
}

/*
** This following structure defines all the methods for the 
** memstat virtual table.
*/
static capdb_module memstatModule = {
  0,                         /* iVersion */
  0,                         /* xCreate */
  memstatConnect,            /* xConnect */
  memstatBestIndex,          /* xBestIndex */
  memstatDisconnect,         /* xDisconnect */
  0,                         /* xDestroy */
  memstatOpen,               /* xOpen - open a cursor */
  memstatClose,              /* xClose - close a cursor */
  memstatFilter,             /* xFilter - configure scan constraints */
  memstatNext,               /* xNext - advance a cursor */
  memstatEof,                /* xEof - check for end of scan */
  memstatColumn,             /* xColumn - read data */
  memstatRowid,              /* xRowid - read data */
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

int capdbMemstatVtabInit(
  capdb *db,
  char **NotUsed1,
  const capdb_api_routines *NotUsed2
){
  int rc = CAPDB_OK;
  (void)NotUsed1;
  (void)NotUsed2;
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdb_create_module(db, "sqlite_memstat", &memstatModule, 0);
#endif
  return rc;
}

#ifndef CAPDB_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_memstat_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  rc = capdbMemstatVtabInit(db, 0, 0);
#endif
  return rc;
}
#endif /* CAPDB_CORE */
#endif /* !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_MEMSTATVTAB) */
