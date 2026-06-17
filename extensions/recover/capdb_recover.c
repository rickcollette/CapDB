/*
** 2022-08-27
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
*/


#include "capdb_recover.h"
#include <assert.h>
#include <string.h>

#ifndef CAPDB_OMIT_VIRTUALTABLE

/*
** Declaration for public API function in file dbdata.c. This may be called
** with NULL as the final two arguments to register the sqlite_dbptr and
** sqlite_dbdata virtual tables with a database handle.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_dbdata_init(capdb*, char**, const capdb_api_routines*);

typedef unsigned int u32;
typedef unsigned char u8;
typedef capdb_int64 i64;

/*
** Work around C99 "flex-array" syntax for pre-C99 compilers, so as
** to avoid complaints from -fsanitize=strict-bounds.
*/
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEXARRAY
#else
# define FLEXARRAY 1
#endif

typedef struct RecoverTable RecoverTable;
typedef struct RecoverColumn RecoverColumn;

/*
** When recovering rows of data that can be associated with table
** definitions recovered from the sqlite_schema table, each table is
** represented by an instance of the following object.
**
** iRoot:
**   The root page in the original database. Not necessarily (and usually
**   not) the same in the recovered database.
**
** zTab:
**   Name of the table.
**
** nCol/aCol[]:
**   aCol[] is an array of nCol columns. In the order in which they appear 
**   in the table.
**
** bIntkey:
**   Set to true for intkey tables, false for WITHOUT ROWID.
**
** iRowidBind:
**   Each column in the aCol[] array has associated with it the index of
**   the bind parameter its values will be bound to in the INSERT statement
**   used to construct the output database. If the table does has a rowid
**   but not an INTEGER PRIMARY KEY column, then iRowidBind contains the
**   index of the bind paramater to which the rowid value should be bound.
**   Otherwise, it contains -1. If the table does contain an INTEGER PRIMARY 
**   KEY column, then the rowid value should be bound to the index associated
**   with the column.
**
** pNext:
**   All RecoverTable objects used by the recovery operation are allocated
**   and populated as part of creating the recovered database schema in
**   the output database, before any non-schema data are recovered. They
**   are then stored in a singly-linked list linked by this variable beginning
**   at capdb_recover.pTblList.
*/
struct RecoverTable {
  u32 iRoot;                      /* Root page in original database */
  char *zTab;                     /* Name of table */
  int nCol;                       /* Number of columns in table */
  RecoverColumn *aCol;            /* Array of columns */
  int bIntkey;                    /* True for intkey, false for without rowid */
  int iRowidBind;                 /* If >0, bind rowid to INSERT here */
  RecoverTable *pNext;
};

/*
** Each database column is represented by an instance of the following object
** stored in the RecoverTable.aCol[] array of the associated table.
**
** iField:
**   The index of the associated field within database records. Or -1 if
**   there is no associated field (e.g. for virtual generated columns).
**
** iBind:
**   The bind index of the INSERT statement to bind this columns values
**   to. Or 0 if there is no such index (iff (iField<0)).
**
** bIPK:
**   True if this is the INTEGER PRIMARY KEY column.
**
** zCol:
**   Name of column.
**
** eHidden:
**   A RECOVER_EHIDDEN_* constant value (see below for interpretation of each).
*/
struct RecoverColumn {
  int iField;                     /* Field in record on disk */
  int iBind;                      /* Binding to use in INSERT */
  int bIPK;                       /* True for IPK column */
  char *zCol;
  int eHidden;
};

#define RECOVER_EHIDDEN_NONE    0      /* Normal database column */
#define RECOVER_EHIDDEN_HIDDEN  1      /* Column is __HIDDEN__ */
#define RECOVER_EHIDDEN_VIRTUAL 2      /* Virtual generated column */
#define RECOVER_EHIDDEN_STORED  3      /* Stored generated column */

/*
** Bitmap object used to track pages in the input database. Allocated
** and manipulated only by the following functions:
**
**     recoverBitmapAlloc()
**     recoverBitmapFree()
**     recoverBitmapSet()
**     recoverBitmapQuery()
**
** nPg:
**   Largest page number that may be stored in the bitmap. The range
**   of valid keys is 1 to nPg, inclusive.
**
** aElem[]:
**   Array large enough to contain a bit for each key. For key value
**   iKey, the associated bit is the bit (iKey%32) of aElem[iKey/32].
**   In other words, the following is true if bit iKey is set, or 
**   false if it is clear:
**
**       (aElem[iKey/32] & (1 << (iKey%32))) ? 1 : 0
*/
typedef struct RecoverBitmap RecoverBitmap;
struct RecoverBitmap {
  i64 nPg;                        /* Size of bitmap */
  u32 aElem[FLEXARRAY];           /* Array of 32-bit bitmasks */
};

/* Size in bytes of a RecoverBitmap object sufficient to cover 32 pages */
#define SZ_RECOVERBITMAP_32  (16)

/*
** State variables (part of the capdb_recover structure) used while
** recovering data for tables identified in the recovered schema (state
** RECOVER_STATE_WRITING).
*/
typedef struct RecoverStateW1 RecoverStateW1;
struct RecoverStateW1 {
  capdb_stmt *pTbls;
  capdb_stmt *pSel;
  capdb_stmt *pInsert;
  int nInsert;

  RecoverTable *pTab;             /* Table currently being written */
  int nMax;                       /* Max column count in any schema table */
  capdb_value **apVal;          /* Array of nMax values */
  int nVal;                       /* Number of valid entries in apVal[] */
  int bHaveRowid;
  i64 iRowid;
  i64 iPrevPage;
  int iPrevCell;
};

/*
** State variables (part of the capdb_recover structure) used while
** recovering data destined for the lost and found table (states
** RECOVER_STATE_LOSTANDFOUND[123]).
*/
typedef struct RecoverStateLAF RecoverStateLAF;
struct RecoverStateLAF {
  RecoverBitmap *pUsed;
  i64 nPg;                        /* Size of db in pages */
  capdb_stmt *pAllAndParent;
  capdb_stmt *pMapInsert;
  capdb_stmt *pMaxField;
  capdb_stmt *pUsedPages;
  capdb_stmt *pFindRoot;
  capdb_stmt *pInsert;          /* INSERT INTO lost_and_found ... */
  capdb_stmt *pAllPage;
  capdb_stmt *pPageData;
  capdb_value **apVal;
  int nMaxField;
};

/*
** Main recover handle structure.
*/
struct capdb_recover {
  /* Copies of capdb_recover_init[_sql]() parameters */
  capdb *dbIn;                  /* Input database */
  char *zDb;                      /* Name of input db ("main" etc.) */
  char *zUri;                     /* URI for output database */
  void *pSqlCtx;                  /* SQL callback context */
  int (*xSql)(void*,const char*); /* Pointer to SQL callback function */

  /* Values configured by capdb_recover_config() */
  char *zStateDb;                 /* State database to use (or NULL) */
  char *zLostAndFound;            /* Name of lost-and-found table (or NULL) */
  int bFreelistCorrupt;           /* CAPDB_RECOVER_FREELIST_CORRUPT setting */
  int bRecoverRowid;              /* CAPDB_RECOVER_ROWIDS setting */
  int bSlowIndexes;               /* CAPDB_RECOVER_SLOWINDEXES setting */

  int pgsz;
  int detected_pgsz;
  int nReserve;
  u8 *pPage1Disk;
  u8 *pPage1Cache;

  /* Error code and error message */
  int errCode;                    /* For capdb_recover_errcode() */
  char *zErrMsg;                  /* For capdb_recover_errmsg() */

  int eState;
  int bCloseTransaction;

  /* Variables used with eState==RECOVER_STATE_WRITING */
  RecoverStateW1 w1;

  /* Variables used with states RECOVER_STATE_LOSTANDFOUND[123] */
  RecoverStateLAF laf;

  /* Fields used within capdb_recover_run() */
  capdb *dbOut;                 /* Output database */
  capdb_stmt *pGetPage;         /* SELECT against input db sqlite_dbdata */
  RecoverTable *pTblList;         /* List of tables recovered from schema */
};

/*
** The various states in which an capdb_recover object may exist:
**
**   RECOVER_STATE_INIT:
**    The object is initially created in this state. capdb_recover_step()
**    has yet to be called. This is the only state in which it is permitted
**    to call capdb_recover_config().
**
**   RECOVER_STATE_WRITING:
**
**   RECOVER_STATE_LOSTANDFOUND1:
**    State to populate the bitmap of pages used by other tables or the
**    database freelist.
**
**   RECOVER_STATE_LOSTANDFOUND2:
**    Populate the recovery.map table - used to figure out a "root" page
**    for each lost page from in the database from which records are
**    extracted.
**
**   RECOVER_STATE_LOSTANDFOUND3:
**    Populate the lost-and-found table itself.
*/
#define RECOVER_STATE_INIT           0
#define RECOVER_STATE_WRITING        1
#define RECOVER_STATE_LOSTANDFOUND1  2
#define RECOVER_STATE_LOSTANDFOUND2  3
#define RECOVER_STATE_LOSTANDFOUND3  4
#define RECOVER_STATE_SCHEMA2        5
#define RECOVER_STATE_DONE           6


/*
** Global variables used by this extension.
*/
typedef struct RecoverGlobal RecoverGlobal;
struct RecoverGlobal {
  const capdb_io_methods *pMethods;
  capdb_recover *p;
};
static RecoverGlobal recover_g;

/*
** Use this static SQLite mutex to protect the globals during the
** first call to capdb_recover_step().
*/ 
#define RECOVER_MUTEX_ID CAPDB_MUTEX_STATIC_APP2


/* 
** Default value for CAPDB_RECOVER_ROWIDS (capdb_recover.bRecoverRowid).
*/
#define RECOVER_ROWID_DEFAULT 1

/*
** Mutex handling:
**
**    recoverEnterMutex()       -   Enter the recovery mutex
**    recoverLeaveMutex()       -   Leave the recovery mutex
**    recoverAssertMutexHeld()  -   Assert that the recovery mutex is held
*/
#if defined(CAPDB_THREADSAFE) && CAPDB_THREADSAFE==0
# define recoverEnterMutex()
# define recoverLeaveMutex()
#else
static void recoverEnterMutex(void){
  capdb_mutex_enter(capdb_mutex_alloc(RECOVER_MUTEX_ID));
}
static void recoverLeaveMutex(void){
  capdb_mutex_leave(capdb_mutex_alloc(RECOVER_MUTEX_ID));
}
#endif
#if CAPDB_THREADSAFE+0>=1 && defined(CAPDB_DEBUG)
static void recoverAssertMutexHeld(void){
  assert( capdb_mutex_held(capdb_mutex_alloc(RECOVER_MUTEX_ID)) );
}
#else
# define recoverAssertMutexHeld()
#endif


/*
** Like strlen(). But handles NULL pointer arguments.
*/
static int recoverStrlen(const char *zStr){
  if( zStr==0 ) return 0;
  return (int)(strlen(zStr)&0x7fffffff);
}

/*
** This function is a no-op if the recover handle passed as the first 
** argument already contains an error (if p->errCode!=CAPDB_OK). 
**
** Otherwise, an attempt is made to allocate, zero and return a buffer nByte
** bytes in size. If successful, a pointer to the new buffer is returned. Or,
** if an OOM error occurs, NULL is returned and the handle error code
** (p->errCode) set to CAPDB_NOMEM.
*/
static void *recoverMalloc(capdb_recover *p, i64 nByte){
  void *pRet = 0;
  assert( nByte>0 );
  if( p->errCode==CAPDB_OK ){
    pRet = capdb_malloc64(nByte);
    if( pRet ){
      memset(pRet, 0, nByte);
    }else{
      p->errCode = CAPDB_NOMEM;
    }
  }
  return pRet;
}

/*
** Set the error code and error message for the recover handle passed as
** the first argument. The error code is set to the value of parameter
** errCode.
**
** Parameter zFmt must be a printf() style formatting string. The handle 
** error message is set to the result of using any trailing arguments for 
** parameter substitutions in the formatting string.
**
** For example:
**
**   recoverError(p, CAPDB_ERROR, "no such table: %s", zTablename);
*/
static int recoverError(
  capdb_recover *p, 
  int errCode, 
  const char *zFmt, ...
){
  char *z = 0;
  va_list ap;
  va_start(ap, zFmt);
  if( zFmt ){
    z = capdb_vmprintf(zFmt, ap);
  }
  va_end(ap);
  capdb_free(p->zErrMsg);
  p->zErrMsg = z;
  p->errCode = errCode;
  return errCode;
}


/*
** This function is a no-op if p->errCode is initially other than CAPDB_OK.
** In this case it returns NULL.
**
** Otherwise, an attempt is made to allocate and return a bitmap object
** large enough to store a bit for all page numbers between 1 and nPg,
** inclusive. The bitmap is initially zeroed.
*/
static RecoverBitmap *recoverBitmapAlloc(capdb_recover *p, i64 nPg){
  int nElem = (nPg+1+31) / 32;
  int nByte = SZ_RECOVERBITMAP_32 + nElem*sizeof(u32);
  RecoverBitmap *pRet = (RecoverBitmap*)recoverMalloc(p, nByte);

  if( pRet ){
    pRet->nPg = nPg;
  }
  return pRet;
}

/*
** Free a bitmap object allocated by recoverBitmapAlloc().
*/
static void recoverBitmapFree(RecoverBitmap *pMap){
  capdb_free(pMap);
}

/*
** Set the bit associated with page iPg in bitvec pMap.
*/
static void recoverBitmapSet(RecoverBitmap *pMap, i64 iPg){
  if( iPg<=pMap->nPg ){
    int iElem = (iPg / 32);
    int iBit = (iPg % 32);
    pMap->aElem[iElem] |= (((u32)1) << iBit);
  }
}

/*
** Query bitmap object pMap for the state of the bit associated with page
** iPg. Return 1 if it is set, or 0 otherwise.
*/
static int recoverBitmapQuery(RecoverBitmap *pMap, i64 iPg){
  int ret = 1;
  if( iPg<=pMap->nPg && iPg>0 ){
    int iElem = (iPg / 32);
    int iBit = (iPg % 32);
    ret = (pMap->aElem[iElem] & (((u32)1) << iBit)) ? 1 : 0;
  }
  return ret;
}

/*
** Set the recover handle error to the error code and message returned by
** calling capdb_errcode() and capdb_errmsg(), respectively, on database
** handle db.
*/
static int recoverDbError(capdb_recover *p, capdb *db){
  return recoverError(p, capdb_errcode(db), "%s", capdb_errmsg(db));
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). 
**
** Otherwise, it attempts to prepare the SQL statement in zSql against
** database handle db. If successful, the statement handle is returned.
** Or, if an error occurs, NULL is returned and an error left in the
** recover handle.
*/
static capdb_stmt *recoverPrepare(
  capdb_recover *p,
  capdb *db, 
  const char *zSql
){
  capdb_stmt *pStmt = 0;
  if( p->errCode==CAPDB_OK ){
    if( capdb_prepare_v2(db, zSql, -1, &pStmt, 0) ){
      recoverDbError(p, db);
    }
  }
  return pStmt;
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). 
**
** Otherwise, argument zFmt is used as a printf() style format string,
** along with any trailing arguments, to create an SQL statement. This
** SQL statement is prepared against database handle db and, if successful,
** the statment handle returned. Or, if an error occurs - either during
** the printf() formatting or when preparing the resulting SQL - an
** error code and message are left in the recover handle.
*/
static capdb_stmt *recoverPreparePrintf(
  capdb_recover *p,
  capdb *db, 
  const char *zFmt, ...
){
  capdb_stmt *pStmt = 0;
  if( p->errCode==CAPDB_OK ){
    va_list ap;
    char *z;
    va_start(ap, zFmt);
    z = capdb_vmprintf(zFmt, ap);
    va_end(ap);
    if( z==0 ){
      p->errCode = CAPDB_NOMEM;
    }else{
      pStmt = recoverPrepare(p, db, z);
      capdb_free(z);
    }
  }
  return pStmt;
}

/*
** Reset SQLite statement handle pStmt. If the call to capdb_reset() 
** indicates that an error occurred, and there is not already an error
** in the recover handle passed as the first argument, set the error
** code and error message appropriately.
**
** This function returns a copy of the statement handle pointer passed
** as the second argument.
*/
static capdb_stmt *recoverReset(capdb_recover *p, capdb_stmt *pStmt){
  int rc = capdb_reset(pStmt);
  if( rc!=CAPDB_OK && rc!=CAPDB_CONSTRAINT && p->errCode==CAPDB_OK ){
    recoverDbError(p, capdb_db_handle(pStmt));
  }
  return pStmt;
}

/*
** Finalize SQLite statement handle pStmt. If the call to capdb_reset() 
** indicates that an error occurred, and there is not already an error
** in the recover handle passed as the first argument, set the error
** code and error message appropriately.
*/
static void recoverFinalize(capdb_recover *p, capdb_stmt *pStmt){
  capdb *db = capdb_db_handle(pStmt);
  int rc = capdb_finalize(pStmt);
  if( rc!=CAPDB_OK && p->errCode==CAPDB_OK ){
    recoverDbError(p, db);
  }
}

/*
** Run a single SQL statement in zSql.  If zSql contains two or more
** SQL statements separated by ';', only the first is run.
**
** Return the capdb_finalizer() or capdb_prepare() result code
** from running the zSql statement.
*/
static int recoverOneStmt(capdb *db, const char *zSql){
  capdb_stmt *pStmt = 0;
  int rc;
  if( zSql==0 ) return CAPDB_OK;
  rc = capdb_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc ){
    capdb_finalize(pStmt);
    return rc;
  }
  while( CAPDB_ROW==capdb_step(pStmt) ){}
  return capdb_finalize(pStmt);
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). A copy of p->errCode is returned in this 
** case.
**
** Otherwise, execute a single SQL statment in zSql.  Even if zSql contains
** two or more SQL statements separated by ';', only execute the first one.
** If successful, return CAPDB_OK.  Or, if an error occurs, leave an error
** code and message in the recover handle and return a copy of the error code.
*/
static int recoverExec(capdb_recover *p, capdb *db, const char *zSql){
  if( p->errCode==CAPDB_OK ){
    int rc = recoverOneStmt(db, zSql);
    if( rc ){
      recoverDbError(p, db);
    }
  }
  return p->errCode;
}

/*
** Bind the value pVal to parameter iBind of statement pStmt. Leave an
** error in the recover handle passed as the first argument if an error
** (e.g. an OOM) occurs.
*/
static void recoverBindValue(
  capdb_recover *p, 
  capdb_stmt *pStmt, 
  int iBind, 
  capdb_value *pVal
){
  if( p->errCode==CAPDB_OK ){
    int rc = capdb_bind_value(pStmt, iBind, pVal);
    if( rc ) recoverError(p, rc, 0);
  }
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). NULL is returned in this case.
**
** Otherwise, an attempt is made to interpret zFmt as a printf() style
** formatting string and the result of using the trailing arguments for
** parameter substitution with it written into a buffer obtained from
** capdb_malloc(). If successful, a pointer to the buffer is returned.
** It is the responsibility of the caller to eventually free the buffer
** using capdb_free().
**
** Or, if an error occurs, an error code and message is left in the recover
** handle and NULL returned.
*/
static char *recoverMPrintf(capdb_recover *p, const char *zFmt, ...){
  va_list ap;
  char *z;
  va_start(ap, zFmt);
  z = capdb_vmprintf(zFmt, ap);
  va_end(ap);
  if( p->errCode==CAPDB_OK ){
    if( z==0 ) p->errCode = CAPDB_NOMEM;
  }else{
    capdb_free(z);
    z = 0;
  }
  return z;
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). Zero is returned in this case.
**
** Otherwise, execute "PRAGMA page_count" against the input database. If
** successful, return the integer result. Or, if an error occurs, leave an
** error code and error message in the capdb_recover handle and return
** zero.
*/
static i64 recoverPageCount(capdb_recover *p){
  i64 nPg = 0;
  if( p->errCode==CAPDB_OK ){
    capdb_stmt *pStmt = 0;
    pStmt = recoverPreparePrintf(p, p->dbIn, "PRAGMA %Q.page_count", p->zDb);
    if( pStmt ){
      capdb_step(pStmt);
      nPg = capdb_column_int64(pStmt, 0);
    }
    recoverFinalize(p, pStmt);
  }
  return nPg;
}

/*
** Implementation of SQL scalar function "read_i32". The first argument to 
** this function must be a blob. The second a non-negative integer. This 
** function reads and returns a 32-bit big-endian integer from byte
** offset (4*<arg2>) of the blob.
**
**     SELECT read_i32(<blob>, <idx>)
*/
static void recoverReadI32(
  capdb_context *context, 
  int argc, 
  capdb_value **argv
){
  const unsigned char *pBlob;
  int nBlob;
  int iInt;

  assert( argc==2 );
  nBlob = capdb_value_bytes(argv[0]);
  pBlob = (const unsigned char*)capdb_value_blob(argv[0]);
  iInt = capdb_value_int(argv[1]) & 0xFFFF;

  if( (iInt+1)*4<=nBlob ){
    const unsigned char *a = &pBlob[iInt*4];
    i64 iVal = ((i64)a[0]<<24)
             + ((i64)a[1]<<16)
             + ((i64)a[2]<< 8)
             + ((i64)a[3]<< 0);
    capdb_result_int64(context, iVal);
  }
}

/*
** Implementation of SQL scalar function "page_is_used". This function
** is used as part of the procedure for locating orphan rows for the
** lost-and-found table, and it depends on those routines having populated
** the capdb_recover.laf.pUsed variable.
**
** The only argument to this function is a page-number. It returns true 
** if the page has already been used somehow during data recovery, or false
** otherwise.
**
**     SELECT page_is_used(<pgno>);
*/
static void recoverPageIsUsed(
  capdb_context *pCtx,
  int nArg,
  capdb_value **apArg
){
  capdb_recover *p = (capdb_recover*)capdb_user_data(pCtx);
  i64 pgno = capdb_value_int64(apArg[0]);
  assert( nArg==1 );
  capdb_result_int(pCtx, recoverBitmapQuery(p->laf.pUsed, pgno));
}

/*
** The implementation of a user-defined SQL function invoked by the 
** sqlite_dbdata and sqlite_dbptr virtual table modules to access pages
** of the database being recovered.
**
** This function always takes a single integer argument. If the argument
** is zero, then the value returned is the number of pages in the db being
** recovered. If the argument is greater than zero, it is a page number. 
** The value returned in this case is an SQL blob containing the data for 
** the identified page of the db being recovered. e.g.
**
**     SELECT getpage(0);       -- return number of pages in db
**     SELECT getpage(4);       -- return page 4 of db as a blob of data 
*/
static void recoverGetPage(
  capdb_context *pCtx,
  int nArg,
  capdb_value **apArg
){
  capdb_recover *p = (capdb_recover*)capdb_user_data(pCtx);
  i64 pgno = capdb_value_int64(apArg[0]);
  capdb_stmt *pStmt = 0;

  assert( nArg==1 );
  if( pgno==0 ){
    i64 nPg = recoverPageCount(p);
    capdb_result_int64(pCtx, nPg);
    return;
  }else{
    if( p->pGetPage==0 ){
      pStmt = p->pGetPage = recoverPreparePrintf(
          p, p->dbIn, "SELECT data FROM sqlite_dbpage(%Q) WHERE pgno=?", p->zDb
      );
    }else if( p->errCode==CAPDB_OK ){
      pStmt = p->pGetPage;
    }

    if( pStmt ){
      capdb_bind_int64(pStmt, 1, pgno);
      if( CAPDB_ROW==capdb_step(pStmt) ){
        const u8 *aPg;
        int nPg;
        assert( p->errCode==CAPDB_OK );
        aPg = capdb_column_blob(pStmt, 0);
        nPg = capdb_column_bytes(pStmt, 0);
        if( pgno==1 && nPg==p->pgsz && 0==memcmp(p->pPage1Cache, aPg, nPg) ){
          aPg = p->pPage1Disk;
        }
        capdb_result_blob(pCtx, aPg, nPg-p->nReserve, CAPDB_TRANSIENT);
      }
      recoverReset(p, pStmt);
    }
  }

  if( p->errCode ){
    if( p->zErrMsg ) capdb_result_error(pCtx, p->zErrMsg, -1);
    capdb_result_error_code(pCtx, p->errCode);
  }
}

/*
** Find a string that is not found anywhere in z[].  Return a pointer
** to that string.
**
** Try to use zA and zB first.  If both of those are already found in z[]
** then make up some string and store it in the buffer zBuf.
*/
static const char *recoverUnusedString(
  const char *z,                    /* Result must not appear anywhere in z */
  const char *zA, const char *zB,   /* Try these first */
  char *zBuf                        /* Space to store a generated string */
){
  unsigned i = 0;
  if( strstr(z, zA)==0 ) return zA;
  if( strstr(z, zB)==0 ) return zB;
  do{
    capdb_snprintf(20,zBuf,"(%s%u)", zA, i++);
  }while( strstr(z,zBuf)!=0 );
  return zBuf;
}

/*
** Implementation of scalar SQL function "escape_crlf".  The argument passed to
** this function is the output of built-in function quote(). If the first
** character of the input is "'", indicating that the value passed to quote()
** was a text value, then this function searches the input for "\n" and "\r"
** characters and adds a wrapper similar to the following:
**
**   replace(replace(<input>, '\n', char(10), '\r', char(13));
**
** Or, if the first character of the input is not "'", then a copy of the input
** is returned.
*/
static void recoverEscapeCrlf(
  capdb_context *context, 
  int argc, 
  capdb_value **argv
){
  const char *zText = (const char*)capdb_value_text(argv[0]);
  (void)argc;
  if( zText && zText[0]=='\'' ){
    int nText = capdb_value_bytes(argv[0]);
    int i;
    char zBuf1[20];
    char zBuf2[20];
    const char *zNL = 0;
    const char *zCR = 0;
    int nCR = 0;
    int nNL = 0;

    for(i=0; zText[i]; i++){
      if( zNL==0 && zText[i]=='\n' ){
        zNL = recoverUnusedString(zText, "\\n", "\\012", zBuf1);
        nNL = (int)strlen(zNL);
      }
      if( zCR==0 && zText[i]=='\r' ){
        zCR = recoverUnusedString(zText, "\\r", "\\015", zBuf2);
        nCR = (int)strlen(zCR);
      }
    }

    if( zNL || zCR ){
      int iOut = 0;
      i64 nMax = (nNL > nCR) ? nNL : nCR;
      i64 nAlloc = nMax * nText + (nMax+64)*2;
      char *zOut = (char*)capdb_malloc64(nAlloc);
      if( zOut==0 ){
        capdb_result_error_nomem(context);
        return;
      }

      if( zNL && zCR ){
        memcpy(&zOut[iOut], "replace(replace(", 16);
        iOut += 16;
      }else{
        memcpy(&zOut[iOut], "replace(", 8);
        iOut += 8;
      }
      for(i=0; zText[i]; i++){
        if( zText[i]=='\n' ){
          memcpy(&zOut[iOut], zNL, nNL);
          iOut += nNL;
        }else if( zText[i]=='\r' ){
          memcpy(&zOut[iOut], zCR, nCR);
          iOut += nCR;
        }else{
          zOut[iOut] = zText[i];
          iOut++;
        }
      }

      if( zNL ){
        memcpy(&zOut[iOut], ",'", 2); iOut += 2;
        memcpy(&zOut[iOut], zNL, nNL); iOut += nNL;
        memcpy(&zOut[iOut], "', char(10))", 12); iOut += 12;
      }
      if( zCR ){
        memcpy(&zOut[iOut], ",'", 2); iOut += 2;
        memcpy(&zOut[iOut], zCR, nCR); iOut += nCR;
        memcpy(&zOut[iOut], "', char(13))", 12); iOut += 12;
      }

      capdb_result_text(context, zOut, iOut, CAPDB_TRANSIENT);
      capdb_free(zOut);
      return;
    }
  }

  capdb_result_value(context, argv[0]);
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). A copy of the error code is returned in
** this case. 
**
** Otherwise, attempt to populate temporary table "recovery.schema" with the
** parts of the database schema that can be extracted from the input database.
**
** If no error occurs, CAPDB_OK is returned. Otherwise, an error code
** and error message are left in the recover handle and a copy of the
** error code returned. It is not considered an error if part of all of
** the database schema cannot be recovered due to corruption.
*/
static int recoverCacheSchema(capdb_recover *p){
  return recoverExec(p, p->dbOut,
    "WITH RECURSIVE pages(p) AS ("
    "  SELECT 1"
    "    UNION"
    "  SELECT child FROM sqlite_dbptr('getpage()'), pages WHERE pgno=p"
    ")"
    "INSERT INTO recovery.schema SELECT"
    "  max(CASE WHEN field=0 THEN value ELSE NULL END),"
    "  max(CASE WHEN field=1 THEN value ELSE NULL END),"
    "  max(CASE WHEN field=2 THEN value ELSE NULL END),"
    "  max(CASE WHEN field=3 THEN value ELSE NULL END),"
    "  max(CASE WHEN field=4 THEN value ELSE NULL END)"
    "FROM sqlite_dbdata('getpage()') WHERE pgno IN ("
    "  SELECT p FROM pages"
    ") GROUP BY pgno, cell"
  );
}

/*
** If this recover handle is not in SQL callback mode (i.e. was not created 
** using capdb_recover_init_sql()) of if an error has already occurred, 
** this function is a no-op. Otherwise, issue a callback with SQL statement
** zSql as the parameter. 
**
** If the callback returns non-zero, set the recover handle error code to
** the value returned (so that the caller will abandon processing).
*/
static void recoverSqlCallback(capdb_recover *p, const char *zSql){
  if( p->errCode==CAPDB_OK && p->xSql ){
    int res = p->xSql(p->pSqlCtx, zSql);
    if( res ){
      recoverError(p, CAPDB_ERROR, "callback returned an error - %d", res);
    }
  }
}

/*
** Transfer the following settings from the input database to the output
** database:
**
**   + page-size,
**   + auto-vacuum settings,
**   + database encoding,
**   + user-version (PRAGMA user_version), and
**   + application-id (PRAGMA application_id), and
*/
static void recoverTransferSettings(capdb_recover *p){
  const char *aPragma[] = {
    "encoding",
    "page_size",
    "auto_vacuum",
    "user_version",
    "application_id"
  };
  int ii;

  /* Truncate the output database to 0 pages in size. This is done by 
  ** opening a new, empty, temp db, then using the backup API to clobber 
  ** any existing output db with a copy of it. */
  if( p->errCode==CAPDB_OK ){
    capdb *db2 = 0;
    int rc = capdb_open("", &db2);
    if( rc!=CAPDB_OK ){
      recoverDbError(p, db2);
      return;
    }

    for(ii=0; ii<(int)(sizeof(aPragma)/sizeof(aPragma[0])); ii++){
      const char *zPrag = aPragma[ii];
      capdb_stmt *p1 = 0;
      p1 = recoverPreparePrintf(p, p->dbIn, "PRAGMA %Q.%s", p->zDb, zPrag);
      if( p->errCode==CAPDB_OK && capdb_step(p1)==CAPDB_ROW ){
        const char *zArg = (const char*)capdb_column_text(p1, 0);
        char *z2 = recoverMPrintf(p, "PRAGMA %s = %Q", zPrag, zArg);
        recoverSqlCallback(p, z2);
        recoverExec(p, db2, z2);
        capdb_free(z2);
        if( zArg==0 ){
          recoverError(p, CAPDB_NOMEM, 0);
        }
      }
      recoverFinalize(p, p1);
    }
    recoverExec(p, db2, "CREATE TABLE t1(a)");
    recoverExec(p, db2, "DROP TABLE t1");

    if( p->errCode==CAPDB_OK ){
      capdb *db = p->dbOut;
      capdb_backup *pBackup = capdb_backup_init(db, "main", db2, "main");
      if( pBackup ){
        capdb_backup_step(pBackup, -1);
        p->errCode = capdb_backup_finish(pBackup);
      }else{
        recoverDbError(p, db);
      }
    }

    capdb_close(db2);
  }
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). A copy of the error code is returned in
** this case. 
**
** Otherwise, an attempt is made to open the output database, attach
** and create the schema of the temporary database used to store
** intermediate data, and to register all required user functions and
** virtual table modules with the output handle.
**
** If no error occurs, CAPDB_OK is returned. Otherwise, an error code
** and error message are left in the recover handle and a copy of the
** error code returned.
*/
static int recoverOpenOutput(capdb_recover *p){
  struct Func {
    const char *zName;
    int nArg;
    void (*xFunc)(capdb_context*,int,capdb_value **);
  } aFunc[] = {
    { "getpage", 1, recoverGetPage },
    { "page_is_used", 1, recoverPageIsUsed },
    { "read_i32", 2, recoverReadI32 },
    { "escape_crlf", 1, recoverEscapeCrlf },
  };

  const int flags = CAPDB_OPEN_URI|CAPDB_OPEN_CREATE|CAPDB_OPEN_READWRITE;
  capdb *db = 0;                /* New database handle */
  int ii;                         /* For iterating through aFunc[] */

  assert( p->dbOut==0 );

  if( capdb_open_v2(p->zUri, &db, flags, 0) ){
    recoverDbError(p, db);
  }

  /* Register the sqlite_dbdata and sqlite_dbptr virtual table modules.
  ** These two are registered with the output database handle - this
  ** module depends on the input handle supporting the sqlite_dbpage
  ** virtual table only.  */
  if( p->errCode==CAPDB_OK ){
    p->errCode = capdb_dbdata_init(db, 0, 0);
  }

  /* Register the custom user-functions with the output handle. */
  for(ii=0;
      p->errCode==CAPDB_OK && ii<(int)(sizeof(aFunc)/sizeof(aFunc[0]));
      ii++){
    p->errCode = capdb_create_function(db, aFunc[ii].zName, 
        aFunc[ii].nArg, CAPDB_UTF8, (void*)p, aFunc[ii].xFunc, 0, 0
    );
  }

  p->dbOut = db;
  return p->errCode;
}

/*
** Attach the auxiliary database 'recovery' to the output database handle.
** This temporary database is used during the recovery process and then 
** discarded.
*/
static void recoverOpenRecovery(capdb_recover *p){
  char *zSql = recoverMPrintf(p, "ATTACH %Q AS recovery;", p->zStateDb);
  recoverExec(p, p->dbOut, zSql);
  capdb_free(zSql);
  recoverExec(p, p->dbOut, "PRAGMA writable_schema = 1");
  recoverExec(p, p->dbOut,
      "CREATE TABLE recovery.map(pgno INTEGER PRIMARY KEY, parent INT)");
  recoverExec(p, p->dbOut,
      "CREATE TABLE recovery.schema(type, name, tbl_name, rootpage, sql)");
}


/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK).
**
** Otherwise, argument zName must be the name of a table that has just been
** created in the output database. This function queries the output db
** for the schema of said table, and creates a RecoverTable object to
** store the schema in memory. The new RecoverTable object is linked into
** the list at capdb_recover.pTblList.
**
** Parameter iRoot must be the root page of table zName in the INPUT 
** database.
*/
static void recoverAddTable(
  capdb_recover *p, 
  const char *zName,              /* Name of table created in output db */
  i64 iRoot                       /* Root page of same table in INPUT db */
){
  capdb_stmt *pStmt = recoverPreparePrintf(p, p->dbOut, 
      "PRAGMA table_xinfo(%Q)", zName
  );

  if( pStmt ){
    int iPk = -1;
    int iBind = 1;
    RecoverTable *pNew = 0;
    int nCol = 0;
    int nName = recoverStrlen(zName);
    int nByte = 0;
    while( capdb_step(pStmt)==CAPDB_ROW ){
      nCol++;
      nByte += (capdb_column_bytes(pStmt, 1)+1);
    }
    nByte += sizeof(RecoverTable) + nCol*sizeof(RecoverColumn) + nName+1;
    recoverReset(p, pStmt);

    pNew = recoverMalloc(p, nByte);
    if( pNew ){
      int i = 0;
      int iField = 0;
      char *csr = 0;
      pNew->aCol = (RecoverColumn*)&pNew[1];
      pNew->zTab = csr = (char*)&pNew->aCol[nCol];
      pNew->nCol = nCol;
      pNew->iRoot = iRoot;
      memcpy(csr, zName, nName);
      csr += nName+1;

      for(i=0; capdb_step(pStmt)==CAPDB_ROW; i++){
        int iPKF = capdb_column_int(pStmt, 5);
        int n = capdb_column_bytes(pStmt, 1);
        const char *z = (const char*)capdb_column_text(pStmt, 1);
        const char *zType = (const char*)capdb_column_text(pStmt, 2);
        int eHidden = capdb_column_int(pStmt, 6);

        if( iPk==-1 && iPKF==1 && !capdb_stricmp("integer", zType) ) iPk = i;
        if( iPKF>1 ) iPk = -2;
        pNew->aCol[i].zCol = csr;
        pNew->aCol[i].eHidden = eHidden;
        if( eHidden==RECOVER_EHIDDEN_VIRTUAL ){
          pNew->aCol[i].iField = -1;
        }else{
          pNew->aCol[i].iField = iField++;
        }
        if( eHidden!=RECOVER_EHIDDEN_VIRTUAL
         && eHidden!=RECOVER_EHIDDEN_STORED
        ){
          pNew->aCol[i].iBind = iBind++;
        }
        memcpy(csr, z, n);
        csr += (n+1);
      }

      pNew->pNext = p->pTblList;
      p->pTblList = pNew;
      pNew->bIntkey = 1;
    }

    recoverFinalize(p, pStmt);

    pStmt = recoverPreparePrintf(p, p->dbOut, "PRAGMA index_xinfo(%Q)", zName);
    while( pStmt && capdb_step(pStmt)==CAPDB_ROW ){
      int iField = capdb_column_int(pStmt, 0);
      int iCol = capdb_column_int(pStmt, 1);

      assert( iCol<pNew->nCol );
      pNew->aCol[iCol].iField = iField;

      pNew->bIntkey = 0;
      iPk = -2;
    }
    recoverFinalize(p, pStmt);

    if( p->errCode==CAPDB_OK ){
      if( iPk>=0 ){
        pNew->aCol[iPk].bIPK = 1;
      }else if( pNew->bIntkey ){
        pNew->iRowidBind = iBind++;
      }
    }
  }
}

/*
** This function is called after recoverCacheSchema() has cached those parts
** of the input database schema that could be recovered in temporary table
** "recovery.schema". This function creates in the output database copies
** of all parts of that schema that must be created before the tables can
** be populated. Specifically, this means:
**
**     * all tables that are not VIRTUAL, and
**     * UNIQUE indexes.
**
** If the recovery handle uses SQL callbacks, then callbacks containing
** the associated "CREATE TABLE" and "CREATE INDEX" statements are made.
**
** Additionally, records are added to the sqlite_schema table of the
** output database for any VIRTUAL tables. The CREATE VIRTUAL TABLE
** records are written directly to sqlite_schema, not actually executed.
** If the handle is in SQL callback mode, then callbacks are invoked 
** with equivalent SQL statements.
*/
static int recoverWriteSchema1(capdb_recover *p){
  capdb_stmt *pSelect = 0;
  capdb_stmt *pTblname = 0;

  pSelect = recoverPrepare(p, p->dbOut,
      "WITH dbschema(rootpage, name, sql, tbl, isVirtual, isIndex) AS ("
      "  SELECT rootpage, name, sql, "
      "    type='table', "
      "    sql LIKE 'create virtual%',"
      "    (type='index' AND (sql LIKE '%unique%' OR ?1))"
      "  FROM recovery.schema"
      ")"
      "SELECT rootpage, tbl, isVirtual, name, sql"
      " FROM dbschema "
      "  WHERE (tbl OR isIndex) AND sql GLOB 'CREATE *'"
      "  ORDER BY tbl DESC, name=='sqlite_sequence' DESC"
  );

  pTblname = recoverPrepare(p, p->dbOut,
      "SELECT name FROM sqlite_schema "
      "WHERE type='table' ORDER BY rowid DESC LIMIT 1"
  );

  if( pSelect ){
    capdb_bind_int(pSelect, 1, p->bSlowIndexes);
    while( capdb_step(pSelect)==CAPDB_ROW ){
      i64 iRoot = capdb_column_int64(pSelect, 0);
      int bTable = capdb_column_int(pSelect, 1);
      int bVirtual = capdb_column_int(pSelect, 2);
      const char *zName = (const char*)capdb_column_text(pSelect, 3);
      const char *zSql = (const char*)capdb_column_text(pSelect, 4);
      char *zFree = 0;
      int rc = CAPDB_OK;

      if( bVirtual ){
        zSql = (const char*)(zFree = recoverMPrintf(p,
            "INSERT INTO sqlite_schema VALUES('table', %Q, %Q, 0, %Q)",
            zName, zName, zSql
        ));
      }
      rc = recoverOneStmt(p->dbOut, zSql);
      if( rc==CAPDB_OK ){
        recoverSqlCallback(p, zSql);
        if( bTable && !bVirtual ){
          if( CAPDB_ROW==capdb_step(pTblname) ){
            const char *zTbl = (const char*)capdb_column_text(pTblname, 0);
            if( zTbl ) recoverAddTable(p, zTbl, iRoot);
          }
          recoverReset(p, pTblname);
        }
      }else if( rc!=CAPDB_ERROR ){
        recoverDbError(p, p->dbOut);
      }
      capdb_free(zFree);
    }
  }
  recoverFinalize(p, pSelect);
  recoverFinalize(p, pTblname);

  return p->errCode;
}

/*
** This function is called after the output database has been populated. It
** adds all recovered schema elements that were not created in the output
** database by recoverWriteSchema1() - everything except for tables and
** UNIQUE indexes. Specifically:
**
**     * views,
**     * triggers,
**     * non-UNIQUE indexes.
**
** If the recover handle is in SQL callback mode, then equivalent callbacks
** are issued to create the schema elements.
*/
static int recoverWriteSchema2(capdb_recover *p){
  capdb_stmt *pSelect = 0;

  pSelect = recoverPrepare(p, p->dbOut,
      p->bSlowIndexes ?
      "SELECT rootpage, sql FROM recovery.schema "
      "  WHERE type!='table' AND type!='index'"
      "    AND sql GLOB 'CREATE *'"
      :
      "SELECT rootpage, sql FROM recovery.schema "
      "  WHERE type!='table' AND (type!='index' OR sql NOT LIKE '%unique%')"
      "    AND sql GLOB 'CREATE *'"
  );

  if( pSelect ){
    while( capdb_step(pSelect)==CAPDB_ROW ){
      const char *zSql = (const char*)capdb_column_text(pSelect, 1);
      int rc = recoverOneStmt(p->dbOut, zSql);
      if( rc==CAPDB_OK ){
        recoverSqlCallback(p, zSql);
      }else if( rc!=CAPDB_ERROR ){
        recoverDbError(p, p->dbOut);
      }
    }
  }
  recoverFinalize(p, pSelect);

  return p->errCode;
}

/*
** This function is a no-op if recover handle p already contains an error
** (if p->errCode!=CAPDB_OK). In this case it returns NULL.
**
** Otherwise, if the recover handle is configured to create an output
** database (was created by capdb_recover_init()), then this function
** prepares and returns an SQL statement to INSERT a new record into table
** pTab, assuming the first nField fields of a record extracted from disk
** are valid.
**
** For example, if table pTab is:
**
**     CREATE TABLE name(a, b GENERATED ALWAYS AS (a+1) STORED, c, d, e);
**
** And nField is 4, then the SQL statement prepared and returned is:
**
**     INSERT INTO (a, c, d) VALUES (?1, ?2, ?3);
**
** In this case even though 4 values were extracted from the input db,
** only 3 are written to the output, as the generated STORED column 
** cannot be written.
**
** If the recover handle is in SQL callback mode, then the SQL statement
** prepared is such that evaluating it returns a single row containing
** a single text value - itself an SQL statement similar to the above,
** except with SQL literals in place of the variables. For example:
**
**     SELECT 'INSERT INTO (a, c, d) VALUES (' 
**          || quote(?1) || ', '
**          || quote(?2) || ', '
**          || quote(?3) || ')';
**
** In either case, it is the responsibility of the caller to eventually
** free the statement handle using capdb_finalize().
*/
static capdb_stmt *recoverInsertStmt(
  capdb_recover *p, 
  RecoverTable *pTab,
  int nField
){
  capdb_stmt *pRet = 0;
  const char *zSep = "";
  const char *zSqlSep = "";
  char *zSql = 0;
  char *zFinal = 0;
  char *zBind = 0;
  int ii;
  int bSql = p->xSql ? 1 : 0;

  if( nField<=0 ) return 0;

  assert( nField<=pTab->nCol );

  zSql = recoverMPrintf(p, "INSERT OR IGNORE INTO %Q(", pTab->zTab);

  if( pTab->iRowidBind ){
    assert( pTab->bIntkey );
    zSql = recoverMPrintf(p, "%z_rowid_", zSql);
    if( bSql ){
      zBind = recoverMPrintf(p, "%zquote(?%d)", zBind, pTab->iRowidBind);
    }else{
      zBind = recoverMPrintf(p, "%z?%d", zBind, pTab->iRowidBind);
    }
    zSqlSep = "||', '||";
    zSep = ", ";
  }

  for(ii=0; ii<nField; ii++){
    int eHidden = pTab->aCol[ii].eHidden;
    if( eHidden!=RECOVER_EHIDDEN_VIRTUAL
     && eHidden!=RECOVER_EHIDDEN_STORED
    ){
      assert( pTab->aCol[ii].iField>=0 && pTab->aCol[ii].iBind>=1 );
      zSql = recoverMPrintf(p, "%z%s%Q", zSql, zSep, pTab->aCol[ii].zCol);

      if( bSql ){
        zBind = recoverMPrintf(p, 
            "%z%sescape_crlf(quote(?%d))", zBind, zSqlSep, pTab->aCol[ii].iBind
        );
        zSqlSep = "||', '||";
      }else{
        zBind = recoverMPrintf(p, "%z%s?%d", zBind, zSep, pTab->aCol[ii].iBind);
      }
      zSep = ", ";
    }
  }

  if( bSql ){
    zFinal = recoverMPrintf(p, "SELECT %Q || ') VALUES (' || %s || ')'", 
        zSql, zBind
    );
  }else{
    zFinal = recoverMPrintf(p, "%s) VALUES (%s)", zSql, zBind);
  }

  pRet = recoverPrepare(p, p->dbOut, zFinal);
  capdb_free(zSql);
  capdb_free(zBind);
  capdb_free(zFinal);
  
  return pRet;
}


/*
** Search the list of RecoverTable objects at p->pTblList for one that
** has root page iRoot in the input database. If such an object is found,
** return a pointer to it. Otherwise, return NULL.
*/
static RecoverTable *recoverFindTable(capdb_recover *p, u32 iRoot){
  RecoverTable *pRet = 0;
  for(pRet=p->pTblList; pRet && pRet->iRoot!=iRoot; pRet=pRet->pNext);
  return pRet;
}

/*
** This function attempts to create a lost and found table within the 
** output db. If successful, it returns a pointer to a buffer containing
** the name of the new table. It is the responsibility of the caller to
** eventually free this buffer using capdb_free().
**
** If an error occurs, NULL is returned and an error code and error 
** message left in the recover handle.
*/
static char *recoverLostAndFoundCreate(
  capdb_recover *p,             /* Recover object */
  int nField                      /* Number of column fields in new table */
){
  char *zTbl = 0;
  capdb_stmt *pProbe = 0;
  int ii = 0;

  pProbe = recoverPrepare(p, p->dbOut,
    "SELECT 1 FROM sqlite_schema WHERE name=?"
  );
  for(ii=-1; zTbl==0 && p->errCode==CAPDB_OK && ii<1000; ii++){
    int bFail = 0;
    if( ii<0 ){
      zTbl = recoverMPrintf(p, "%s", p->zLostAndFound);
    }else{
      zTbl = recoverMPrintf(p, "%s_%d", p->zLostAndFound, ii);
    }

    if( p->errCode==CAPDB_OK ){
      capdb_bind_text(pProbe, 1, zTbl, -1, CAPDB_STATIC);
      if( CAPDB_ROW==capdb_step(pProbe) ){
        bFail = 1;
      }
      recoverReset(p, pProbe);
    }

    if( bFail ){
      capdb_clear_bindings(pProbe);
      capdb_free(zTbl);
      zTbl = 0;
    }
  }
  recoverFinalize(p, pProbe);

  if( zTbl ){
    const char *zSep = 0;
    char *zField = 0;
    char *zSql = 0;

    zSep = "rootpgno INTEGER, pgno INTEGER, nfield INTEGER, id INTEGER, ";
    for(ii=0; p->errCode==CAPDB_OK && ii<nField; ii++){
      zField = recoverMPrintf(p, "%z%sc%d", zField, zSep, ii);
      zSep = ", ";
    }

    zSql = recoverMPrintf(p, "CREATE TABLE %s(%s)", zTbl, zField);
    capdb_free(zField);

    recoverExec(p, p->dbOut, zSql);
    recoverSqlCallback(p, zSql);
    capdb_free(zSql);
  }else if( p->errCode==CAPDB_OK ){
    recoverError(
        p, CAPDB_ERROR, "failed to create %s output table", p->zLostAndFound
    );
  }

  return zTbl;
}

/*
** Synthesize and prepare an INSERT statement to write to the lost_and_found
** table in the output database. The name of the table is zTab, and it has
** nField c* fields.
*/
static capdb_stmt *recoverLostAndFoundInsert(
  capdb_recover *p,
  const char *zTab,
  int nField
){
  int nTotal = nField + 4;
  int ii;
  char *zBind = 0;
  capdb_stmt *pRet = 0;

  if( p->xSql==0 ){
    for(ii=0; ii<nTotal; ii++){
      zBind = recoverMPrintf(p, "%z%s?", zBind, zBind?", ":"", ii);
    }
    pRet = recoverPreparePrintf(
        p, p->dbOut, "INSERT INTO %s VALUES(%s)", zTab, zBind
    );
  }else{
    const char *zSep = "";
    for(ii=0; ii<nTotal; ii++){
      zBind = recoverMPrintf(p, "%z%squote(?)", zBind, zSep);
      zSep = "|| ', ' ||";
    }
    pRet = recoverPreparePrintf(
        p, p->dbOut, "SELECT 'INSERT INTO %s VALUES(' || %s || ')'", zTab, zBind
    );
  }

  capdb_free(zBind);
  return pRet;
}

/*
** Input database page iPg contains data that will be written to the
** lost-and-found table of the output database. This function attempts
** to identify the root page of the tree that page iPg belonged to.
** If successful, it sets output variable (*piRoot) to the page number
** of the root page and returns CAPDB_OK. Otherwise, if an error occurs,
** an SQLite error code is returned and the final value of *piRoot 
** undefined.
*/
static int recoverLostAndFoundFindRoot(
  capdb_recover *p, 
  i64 iPg,
  i64 *piRoot
){
  RecoverStateLAF *pLaf = &p->laf;

  if( pLaf->pFindRoot==0 ){
    pLaf->pFindRoot = recoverPrepare(p, p->dbOut,
        "WITH RECURSIVE p(pgno) AS ("
        "  SELECT ?"
        "    UNION"
        "  SELECT parent FROM recovery.map AS m, p WHERE m.pgno=p.pgno"
        ") "
        "SELECT p.pgno FROM p, recovery.map m WHERE m.pgno=p.pgno "
        "    AND m.parent IS NULL"
    );
  }
  if( p->errCode==CAPDB_OK ){
    capdb_bind_int64(pLaf->pFindRoot, 1, iPg);
    if( capdb_step(pLaf->pFindRoot)==CAPDB_ROW ){
      *piRoot = capdb_column_int64(pLaf->pFindRoot, 0);
    }else{
      *piRoot = iPg;
    }
    recoverReset(p, pLaf->pFindRoot);
  }
  return p->errCode;
}

/*
** Recover data from page iPage of the input database and write it to
** the lost-and-found table in the output database.
*/
static void recoverLostAndFoundOnePage(capdb_recover *p, i64 iPage){
  RecoverStateLAF *pLaf = &p->laf;
  capdb_value **apVal = pLaf->apVal;
  capdb_stmt *pPageData = pLaf->pPageData;
  capdb_stmt *pInsert = pLaf->pInsert;

  int nVal = -1;
  int iPrevCell = 0;
  i64 iRoot = 0;
  int bHaveRowid = 0;
  i64 iRowid = 0;
  int ii = 0;

  if( recoverLostAndFoundFindRoot(p, iPage, &iRoot) ) return;
  capdb_bind_int64(pPageData, 1, iPage);
  while( p->errCode==CAPDB_OK && CAPDB_ROW==capdb_step(pPageData) ){
    int iCell = capdb_column_int64(pPageData, 0);
    int iField = capdb_column_int64(pPageData, 1);

    if( iPrevCell!=iCell && nVal>=0 ){
      /* Insert the new row */
      capdb_bind_int64(pInsert, 1, iRoot);      /* rootpgno */
      capdb_bind_int64(pInsert, 2, iPage);      /* pgno */
      capdb_bind_int(pInsert, 3, nVal);         /* nfield */
      if( bHaveRowid ){
        capdb_bind_int64(pInsert, 4, iRowid);   /* id */
      }
      for(ii=0; ii<nVal; ii++){
        recoverBindValue(p, pInsert, 5+ii, apVal[ii]);
      }
      if( capdb_step(pInsert)==CAPDB_ROW ){
        recoverSqlCallback(p, (const char*)capdb_column_text(pInsert, 0));
      }
      recoverReset(p, pInsert);

      /* Discard the accumulated row data */
      for(ii=0; ii<nVal; ii++){
        capdb_value_free(apVal[ii]);
        apVal[ii] = 0;
      }
      capdb_clear_bindings(pInsert);
      bHaveRowid = 0;
      nVal = -1;
    }

    if( iCell<0 ) break;

    if( iField<0 ){
      assert( nVal==-1 );
      iRowid = capdb_column_int64(pPageData, 2);
      bHaveRowid = 1;
      nVal = 0;
    }else if( iField<pLaf->nMaxField ){
      capdb_value *pVal = capdb_column_value(pPageData, 2);
      apVal[iField] = capdb_value_dup(pVal);
      assert( iField==nVal || (nVal==-1 && iField==0) );
      nVal = iField+1;
      if( apVal[iField]==0 ){
        recoverError(p, CAPDB_NOMEM, 0);
      }
    }

    iPrevCell = iCell;
  }
  recoverReset(p, pPageData);

  for(ii=0; ii<nVal; ii++){
    capdb_value_free(apVal[ii]);
    apVal[ii] = 0;
  }
}

/*
** Perform one step (capdb_recover_step()) of work for the connection 
** passed as the only argument, which is guaranteed to be in
** RECOVER_STATE_LOSTANDFOUND3 state - during which the lost-and-found 
** table of the output database is populated with recovered data that can 
** not be assigned to any recovered schema object.
*/ 
static int recoverLostAndFound3Step(capdb_recover *p){
  RecoverStateLAF *pLaf = &p->laf;
  if( p->errCode==CAPDB_OK ){
    if( pLaf->pInsert==0 ){
      return CAPDB_DONE;
    }else{
      if( p->errCode==CAPDB_OK ){
        int res = capdb_step(pLaf->pAllPage);
        if( res==CAPDB_ROW ){
          i64 iPage = capdb_column_int64(pLaf->pAllPage, 0);
          if( recoverBitmapQuery(pLaf->pUsed, iPage)==0 ){
            recoverLostAndFoundOnePage(p, iPage);
          }
        }else{
          recoverReset(p, pLaf->pAllPage);
          return CAPDB_DONE;
        }
      }
    }
  }
  return CAPDB_OK;
}

/*
** Initialize resources required in RECOVER_STATE_LOSTANDFOUND3 
** state - during which the lost-and-found table of the output database 
** is populated with recovered data that can not be assigned to any 
** recovered schema object.
*/ 
static void recoverLostAndFound3Init(capdb_recover *p){
  RecoverStateLAF *pLaf = &p->laf;

  if( pLaf->nMaxField>0 ){
    char *zTab = 0;               /* Name of lost_and_found table */

    zTab = recoverLostAndFoundCreate(p, pLaf->nMaxField);
    pLaf->pInsert = recoverLostAndFoundInsert(p, zTab, pLaf->nMaxField);
    capdb_free(zTab);

    pLaf->pAllPage = recoverPreparePrintf(p, p->dbOut,
        "WITH RECURSIVE seq(ii) AS ("
        "  SELECT 1 UNION ALL SELECT ii+1 FROM seq WHERE ii<%lld"
        ")"
        "SELECT ii FROM seq" , p->laf.nPg
    );
    pLaf->pPageData = recoverPrepare(p, p->dbOut,
        "SELECT cell, field, value "
        "FROM sqlite_dbdata('getpage()') d WHERE d.pgno=? "
        "UNION ALL "
        "SELECT -1, -1, -1"
    );

    pLaf->apVal = (capdb_value**)recoverMalloc(p, 
        pLaf->nMaxField*sizeof(capdb_value*)
    );
  }
}

/*
** Initialize resources required in RECOVER_STATE_WRITING state - during which
** tables recovered from the schema of the input database are populated with
** recovered data.
*/ 
static int recoverWriteDataInit(capdb_recover *p){
  RecoverStateW1 *p1 = &p->w1;
  RecoverTable *pTbl = 0;
  int nByte = 0;

  /* Figure out the maximum number of columns for any table in the schema */
  assert( p1->nMax==0 );
  for(pTbl=p->pTblList; pTbl; pTbl=pTbl->pNext){
    if( pTbl->nCol>p1->nMax ) p1->nMax = pTbl->nCol;
  }

  /* Allocate an array of (capdb_value*) in which to accumulate the values
  ** that will be written to the output database in a single row. */
  nByte = sizeof(capdb_value*) * (p1->nMax+1);
  p1->apVal = (capdb_value**)recoverMalloc(p, nByte);
  if( p1->apVal==0 ) return p->errCode;

  /* Prepare the SELECT to loop through schema tables (pTbls) and the SELECT
  ** to loop through cells that appear to belong to a single table (pSel). */
  p1->pTbls = recoverPrepare(p, p->dbOut,
      "SELECT rootpage FROM recovery.schema "
      "  WHERE type='table' AND (sql NOT LIKE 'create virtual%')"
      "  ORDER BY (tbl_name='sqlite_sequence') ASC"
  );
  p1->pSel = recoverPrepare(p, p->dbOut, 
      "WITH RECURSIVE pages(page) AS ("
      "  SELECT ?1"
      "    UNION"
      "  SELECT child FROM sqlite_dbptr('getpage()'), pages "
      "    WHERE pgno=page"
      ") "
      "SELECT page, cell, field, value "
      "FROM sqlite_dbdata('getpage()') d, pages p WHERE p.page=d.pgno "
      "UNION ALL "
      "SELECT 0, 0, 0, 0"
  );

  return p->errCode;
}

/*
** Clean up resources allocated by recoverWriteDataInit() (stuff in 
** capdb_recover.w1).
*/
static void recoverWriteDataCleanup(capdb_recover *p){
  RecoverStateW1 *p1 = &p->w1;
  int ii;
  for(ii=0; ii<p1->nVal; ii++){
    capdb_value_free(p1->apVal[ii]);
  }
  capdb_free(p1->apVal);
  recoverFinalize(p, p1->pInsert);
  recoverFinalize(p, p1->pTbls);
  recoverFinalize(p, p1->pSel);
  memset(p1, 0, sizeof(*p1));
}

/*
** Perform one step (capdb_recover_step()) of work for the connection 
** passed as the only argument, which is guaranteed to be in
** RECOVER_STATE_WRITING state - during which tables recovered from the
** schema of the input database are populated with recovered data.
*/ 
static int recoverWriteDataStep(capdb_recover *p){
  RecoverStateW1 *p1 = &p->w1;
  capdb_stmt *pSel = p1->pSel;
  capdb_value **apVal = p1->apVal;

  if( p->errCode==CAPDB_OK && p1->pTab==0 ){
    if( capdb_step(p1->pTbls)==CAPDB_ROW ){
      i64 iRoot = capdb_column_int64(p1->pTbls, 0);
      p1->pTab = recoverFindTable(p, iRoot);

      recoverFinalize(p, p1->pInsert);
      p1->pInsert = 0;

      /* If this table is unknown, return early. The caller will invoke this
      ** function again and it will move on to the next table.  */
      if( p1->pTab==0 ) return p->errCode;

      /* If this is the sqlite_sequence table, delete any rows added by
      ** earlier INSERT statements on tables with AUTOINCREMENT primary
      ** keys before recovering its contents. The p1->pTbls SELECT statement
      ** is rigged to deliver "sqlite_sequence" last of all, so we don't
      ** worry about it being modified after it is recovered. */
      if( capdb_stricmp("sqlite_sequence", p1->pTab->zTab)==0 ){
        recoverExec(p, p->dbOut, "DELETE FROM sqlite_sequence");
        recoverSqlCallback(p, "DELETE FROM sqlite_sequence");
      }

      /* Bind the root page of this table within the original database to 
      ** SELECT statement p1->pSel. The SELECT statement will then iterate
      ** through cells that look like they belong to table pTab.  */
      capdb_bind_int64(pSel, 1, iRoot);

      p1->nVal = 0;
      p1->bHaveRowid = 0;
      p1->iPrevPage = -1;
      p1->iPrevCell = -1;
    }else{
      return CAPDB_DONE;
    }
  }
  assert( p->errCode!=CAPDB_OK || p1->pTab );

  if( p->errCode==CAPDB_OK && capdb_step(pSel)==CAPDB_ROW ){
    RecoverTable *pTab = p1->pTab;

    i64 iPage = capdb_column_int64(pSel, 0);
    int iCell = capdb_column_int(pSel, 1);
    int iField = capdb_column_int(pSel, 2);
    capdb_value *pVal = capdb_column_value(pSel, 3);
    int bNewCell = (p1->iPrevPage!=iPage || p1->iPrevCell!=iCell);

    assert( bNewCell==0 || (iField==-1 || iField==0) );
    assert( bNewCell || iField==p1->nVal || p1->nVal==pTab->nCol );

    if( bNewCell ){
      int ii = 0;
      if( p1->nVal>=0 ){
        if( p1->pInsert==0 || p1->nVal!=p1->nInsert ){
          recoverFinalize(p, p1->pInsert);
          p1->pInsert = recoverInsertStmt(p, pTab, p1->nVal);
          p1->nInsert = p1->nVal;
        }
        if( p1->nVal>0 ){
          capdb_stmt *pInsert = p1->pInsert;
          for(ii=0; ii<pTab->nCol; ii++){
            RecoverColumn *pCol = &pTab->aCol[ii];
            int iBind = pCol->iBind;
            if( iBind>0 ){
              if( pCol->bIPK ){
                capdb_bind_int64(pInsert, iBind, p1->iRowid);
              }else if( pCol->iField<p1->nVal ){
                recoverBindValue(p, pInsert, iBind, apVal[pCol->iField]);
              }
            }
          }
          if( p->bRecoverRowid && pTab->iRowidBind>0 && p1->bHaveRowid ){
            capdb_bind_int64(pInsert, pTab->iRowidBind, p1->iRowid);
          }
          if( CAPDB_ROW==capdb_step(pInsert) ){
            const char *z = (const char*)capdb_column_text(pInsert, 0);
            recoverSqlCallback(p, z);
          }
          recoverReset(p, pInsert);
          assert( p->errCode || pInsert );
          if( pInsert ) capdb_clear_bindings(pInsert);
        }
      }

      for(ii=0; ii<p1->nVal; ii++){
        capdb_value_free(apVal[ii]);
        apVal[ii] = 0;
      }
      p1->nVal = -1;
      p1->bHaveRowid = 0;
    }

    if( iPage!=0 ){
      if( iField<0 ){
        p1->iRowid = capdb_column_int64(pSel, 3);
        assert( p1->nVal==-1 );
        p1->nVal = 0;
        p1->bHaveRowid = 1;
      }else if( iField<pTab->nCol ){
        assert( apVal[iField]==0 );
        apVal[iField] = capdb_value_dup( pVal );
        if( apVal[iField]==0 ){
          recoverError(p, CAPDB_NOMEM, 0);
        }
        p1->nVal = iField+1;
      }else if( pTab->nCol==0 ){
        p1->nVal = pTab->nCol;
      }
      p1->iPrevCell = iCell;
      p1->iPrevPage = iPage;
    }
  }else{
    recoverReset(p, pSel);
    p1->pTab = 0;
  }

  return p->errCode;
}

/*
** Initialize resources required by capdb_recover_step() in
** RECOVER_STATE_LOSTANDFOUND1 state - during which the set of pages not
** already allocated to a recovered schema element is determined.
*/ 
static void recoverLostAndFound1Init(capdb_recover *p){
  RecoverStateLAF *pLaf = &p->laf;
  capdb_stmt *pStmt = 0;

  assert( p->laf.pUsed==0 );
  pLaf->nPg = recoverPageCount(p);
  pLaf->pUsed = recoverBitmapAlloc(p, pLaf->nPg);

  /* Prepare a statement to iterate through all pages that are part of any tree
  ** in the recoverable part of the input database schema to the bitmap. And,
  ** if !p->bFreelistCorrupt, add all pages that appear to be part of the
  ** freelist.  */
  pStmt = recoverPrepare(
      p, p->dbOut,
      "WITH trunk(pgno) AS ("
      "  SELECT read_i32(getpage(1), 8) AS x WHERE x>0"
      "    UNION"
      "  SELECT read_i32(getpage(trunk.pgno), 0) AS x FROM trunk WHERE x>0"
      "),"
      "trunkdata(pgno, data) AS ("
      "  SELECT pgno, getpage(pgno) FROM trunk"
      "),"
      "freelist(data, n, freepgno) AS ("
      "  SELECT data, min(16384, read_i32(data, 1)-1), pgno FROM trunkdata"
      "    UNION ALL"
      "  SELECT data, n-1, read_i32(data, 2+n) FROM freelist WHERE n>=0"
      "),"
      ""
      "roots(r) AS ("
      "  SELECT 1 UNION ALL"
      "  SELECT rootpage FROM recovery.schema WHERE rootpage>0"
      "),"
      "used(page) AS ("
      "  SELECT r FROM roots"
      "    UNION"
      "  SELECT child FROM sqlite_dbptr('getpage()'), used "
      "    WHERE pgno=page"
      ") "
      "SELECT page FROM used"
      " UNION ALL "
      "SELECT freepgno FROM freelist WHERE NOT ?"
  );
  if( pStmt ) capdb_bind_int(pStmt, 1, p->bFreelistCorrupt);
  pLaf->pUsedPages = pStmt;
}

/*
** Perform one step (capdb_recover_step()) of work for the connection 
** passed as the only argument, which is guaranteed to be in
** RECOVER_STATE_LOSTANDFOUND1 state - during which the set of pages not
** already allocated to a recovered schema element is determined.
*/ 
static int recoverLostAndFound1Step(capdb_recover *p){
  RecoverStateLAF *pLaf = &p->laf;
  int rc = p->errCode;
  if( rc==CAPDB_OK ){
    rc = capdb_step(pLaf->pUsedPages);
    if( rc==CAPDB_ROW ){
      i64 iPg = capdb_column_int64(pLaf->pUsedPages, 0);
      recoverBitmapSet(pLaf->pUsed, iPg);
      rc = CAPDB_OK;
    }else{
      recoverFinalize(p, pLaf->pUsedPages);
      pLaf->pUsedPages = 0;
    }
  }
  return rc;
}

/*
** Initialize resources required by RECOVER_STATE_LOSTANDFOUND2 
** state - during which the pages identified in RECOVER_STATE_LOSTANDFOUND1
** are sorted into sets that likely belonged to the same database tree.
*/ 
static void recoverLostAndFound2Init(capdb_recover *p){
  RecoverStateLAF *pLaf = &p->laf;

  assert( p->laf.pAllAndParent==0 );
  assert( p->laf.pMapInsert==0 );
  assert( p->laf.pMaxField==0 );
  assert( p->laf.nMaxField==0 );

  pLaf->pMapInsert = recoverPrepare(p, p->dbOut,
      "INSERT OR IGNORE INTO recovery.map(pgno, parent) VALUES(?, ?)"
  );
  pLaf->pAllAndParent = recoverPreparePrintf(p, p->dbOut,
      "WITH RECURSIVE seq(ii) AS ("
      "  SELECT 1 UNION ALL SELECT ii+1 FROM seq WHERE ii<%lld"
      ")"
      "SELECT pgno, child FROM sqlite_dbptr('getpage()') "
      " UNION ALL "
      "SELECT NULL, ii FROM seq", p->laf.nPg
  );
  pLaf->pMaxField = recoverPreparePrintf(p, p->dbOut,
      "SELECT max(field)+1 FROM sqlite_dbdata('getpage') WHERE pgno = ?"
  );
}

/*
** Perform one step (capdb_recover_step()) of work for the connection 
** passed as the only argument, which is guaranteed to be in
** RECOVER_STATE_LOSTANDFOUND2 state - during which the pages identified 
** in RECOVER_STATE_LOSTANDFOUND1 are sorted into sets that likely belonged 
** to the same database tree.
*/ 
static int recoverLostAndFound2Step(capdb_recover *p){
  RecoverStateLAF *pLaf = &p->laf;
  if( p->errCode==CAPDB_OK ){
    int res = capdb_step(pLaf->pAllAndParent);
    if( res==CAPDB_ROW ){
      i64 iChild = capdb_column_int(pLaf->pAllAndParent, 1);
      if( recoverBitmapQuery(pLaf->pUsed, iChild)==0 ){
        capdb_bind_int64(pLaf->pMapInsert, 1, iChild);
        capdb_bind_value(pLaf->pMapInsert, 2, 
            capdb_column_value(pLaf->pAllAndParent, 0)
        );
        capdb_step(pLaf->pMapInsert);
        recoverReset(p, pLaf->pMapInsert);
        capdb_bind_int64(pLaf->pMaxField, 1, iChild);
        if( CAPDB_ROW==capdb_step(pLaf->pMaxField) ){
          int nMax = capdb_column_int(pLaf->pMaxField, 0);
          if( nMax>pLaf->nMaxField ) pLaf->nMaxField = nMax;
        }
        recoverReset(p, pLaf->pMaxField);
      }
    }else{
      recoverFinalize(p, pLaf->pAllAndParent);
      pLaf->pAllAndParent =0;
      return CAPDB_DONE;
    }
  }
  return p->errCode;
}

/*
** Free all resources allocated as part of capdb_recover_step() calls
** in one of the RECOVER_STATE_LOSTANDFOUND[123] states.
*/
static void recoverLostAndFoundCleanup(capdb_recover *p){
  recoverBitmapFree(p->laf.pUsed);
  p->laf.pUsed = 0;
  capdb_finalize(p->laf.pUsedPages);
  capdb_finalize(p->laf.pAllAndParent);
  capdb_finalize(p->laf.pMapInsert);
  capdb_finalize(p->laf.pMaxField);
  capdb_finalize(p->laf.pFindRoot);
  capdb_finalize(p->laf.pInsert);
  capdb_finalize(p->laf.pAllPage);
  capdb_finalize(p->laf.pPageData);
  p->laf.pUsedPages = 0;
  p->laf.pAllAndParent = 0;
  p->laf.pMapInsert = 0;
  p->laf.pMaxField = 0;
  p->laf.pFindRoot = 0;
  p->laf.pInsert = 0;
  p->laf.pAllPage = 0;
  p->laf.pPageData = 0;
  capdb_free(p->laf.apVal);
  p->laf.apVal = 0;
}

/*
** Free all resources allocated as part of capdb_recover_step() calls.
*/
static void recoverFinalCleanup(capdb_recover *p){
  RecoverTable *pTab = 0;
  RecoverTable *pNext = 0;

  recoverWriteDataCleanup(p);
  recoverLostAndFoundCleanup(p);

  for(pTab=p->pTblList; pTab; pTab=pNext){
    pNext = pTab->pNext;
    capdb_free(pTab);
  }
  p->pTblList = 0;
  capdb_finalize(p->pGetPage);
  p->pGetPage = 0;
  capdb_file_control(p->dbIn, p->zDb, CAPDB_FCNTL_RESET_CACHE, 0);

  {
#ifndef NDEBUG
    int res = 
#endif
       capdb_close(p->dbOut);
    assert( res==CAPDB_OK );
  }
  p->dbOut = 0;
}

/*
** Decode and return an unsigned 16-bit big-endian integer value from 
** buffer a[].
*/
static u32 recoverGetU16(const u8 *a){
  return (((u32)a[0])<<8) + ((u32)a[1]);
}

/*
** Decode and return an unsigned 32-bit big-endian integer value from 
** buffer a[].
*/
static u32 recoverGetU32(const u8 *a){
  return (((u32)a[0])<<24) + (((u32)a[1])<<16) + (((u32)a[2])<<8) + ((u32)a[3]);
}

/*
** Decode an SQLite varint from buffer a[]. Write the decoded value to (*pVal)
** and return the number of bytes consumed.
*/
static int recoverGetVarint(const u8 *a, i64 *pVal){
  capdb_uint64 u = 0;
  int i;
  for(i=0; i<8; i++){
    u = (u<<7) + (a[i]&0x7f);
    if( (a[i]&0x80)==0 ){ *pVal = (capdb_int64)u; return i+1; }
  }
  u = (u<<8) + (a[i]&0xff);
  *pVal = (capdb_int64)u;
  return 9;
}

/*
** The second argument points to a buffer n bytes in size. If this buffer
** or a prefix thereof appears to contain a well-formed SQLite b-tree page, 
** return the page-size in bytes. Otherwise, if the buffer does not 
** appear to contain a well-formed b-tree page, return 0.
*/
static int recoverIsValidPage(u8 *aTmp, const u8 *a, int n){
  u8 *aUsed = aTmp;
  int nFrag = 0;
  int nActual = 0;
  int iFree = 0;
  int nCell = 0;                  /* Number of cells on page */
  int iCellOff = 0;               /* Offset of cell array in page */
  int iContent = 0;
  int eType = 0;
  int ii = 0;

  eType = (int)a[0];
  if( eType!=0x02 && eType!=0x05 && eType!=0x0A && eType!=0x0D ) return 0;

  iFree = (int)recoverGetU16(&a[1]);
  nCell = (int)recoverGetU16(&a[3]);
  iContent = (int)recoverGetU16(&a[5]);
  if( iContent==0 ) iContent = 65536;
  nFrag = (int)a[7];

  if( iContent>n ) return 0;

  memset(aUsed, 0, n);
  memset(aUsed, 0xFF, iContent);

  /* Follow the free-list. This is the same format for all b-tree pages. */
  if( iFree && iFree<=iContent ) return 0;
  while( iFree ){
    int iNext = 0;
    int nByte = 0;
    if( iFree>(n-4) ) return 0;
    iNext = recoverGetU16(&a[iFree]);
    nByte = recoverGetU16(&a[iFree+2]);
    if( iFree+nByte>n || nByte<4 ) return 0;
    if( iNext && iNext<iFree+nByte ) return 0;
    memset(&aUsed[iFree], 0xFF, nByte);
    iFree = iNext;
  }

  /* Run through the cells */
  if( eType==0x02 || eType==0x05 ){
    iCellOff = 12;
  }else{
    iCellOff = 8;
  }
  if( (iCellOff + 2*nCell)>iContent ) return 0;
  for(ii=0; ii<nCell; ii++){
    int iByte;
    i64 nPayload = 0;
    int nByte = 0;
    int iOff = recoverGetU16(&a[iCellOff + 2*ii]);
    if( iOff<iContent || iOff>n ){
      return 0;
    }
    if( eType==0x05 || eType==0x02 ) nByte += 4;
    nByte += recoverGetVarint(&a[iOff+nByte], &nPayload);
    if( eType==0x0D ){
      i64 dummy = 0;
      nByte += recoverGetVarint(&a[iOff+nByte], &dummy);
    }
    if( eType!=0x05 ){
      int X = (eType==0x0D) ? n-35 : (((n-12)*64/255)-23);
      int M = ((n-12)*32/255)-23;
      int K = M+((nPayload-M)%(n-4));

      if( nPayload<X ){
        nByte += nPayload;
      }else if( K<=X ){
        nByte += K+4;
      }else{
        nByte += M+4;
      }
    }

    if( iOff+nByte>n ){
      return 0;
    }
    for(iByte=iOff; iByte<(iOff+nByte); iByte++){
      if( aUsed[iByte]!=0 ){
        return 0;
      }
      aUsed[iByte] = 0xFF;
    }
  }

  nActual = 0;
  for(ii=0; ii<n; ii++){
    if( aUsed[ii]==0 ) nActual++;
  }
  return (nActual==nFrag);
}


static int recoverVfsClose(capdb_file*);
static int recoverVfsRead(capdb_file*, void*, int iAmt, capdb_int64 iOfst);
static int recoverVfsWrite(capdb_file*, const void*, int, capdb_int64);
static int recoverVfsTruncate(capdb_file*, capdb_int64 size);
static int recoverVfsSync(capdb_file*, int flags);
static int recoverVfsFileSize(capdb_file*, capdb_int64 *pSize);
static int recoverVfsLock(capdb_file*, int);
static int recoverVfsUnlock(capdb_file*, int);
static int recoverVfsCheckReservedLock(capdb_file*, int *pResOut);
static int recoverVfsFileControl(capdb_file*, int op, void *pArg);
static int recoverVfsSectorSize(capdb_file*);
static int recoverVfsDeviceCharacteristics(capdb_file*);
static int recoverVfsShmMap(capdb_file*, int, int, int, void volatile**);
static int recoverVfsShmLock(capdb_file*, int offset, int n, int flags);
static void recoverVfsShmBarrier(capdb_file*);
static int recoverVfsShmUnmap(capdb_file*, int deleteFlag);
static int recoverVfsFetch(capdb_file*, capdb_int64, int, void**);
static int recoverVfsUnfetch(capdb_file *pFd, capdb_int64 iOff, void *p);

static capdb_io_methods recover_methods = {
  2, /* iVersion */
  recoverVfsClose,
  recoverVfsRead,
  recoverVfsWrite,
  recoverVfsTruncate,
  recoverVfsSync,
  recoverVfsFileSize,
  recoverVfsLock,
  recoverVfsUnlock,
  recoverVfsCheckReservedLock,
  recoverVfsFileControl,
  recoverVfsSectorSize,
  recoverVfsDeviceCharacteristics,
  recoverVfsShmMap,
  recoverVfsShmLock,
  recoverVfsShmBarrier,
  recoverVfsShmUnmap,
  recoverVfsFetch,
  recoverVfsUnfetch
};

static int recoverVfsClose(capdb_file *pFd){
  assert( pFd->pMethods!=&recover_methods );
  return pFd->pMethods->xClose(pFd);
}

/*
** Write value v to buffer a[] as a 16-bit big-endian unsigned integer.
*/
static void recoverPutU16(u8 *a, u32 v){
  a[0] = (v>>8) & 0x00FF;
  a[1] = (v>>0) & 0x00FF;
}

/*
** Write value v to buffer a[] as a 32-bit big-endian unsigned integer.
*/
static void recoverPutU32(u8 *a, u32 v){
  a[0] = (v>>24) & 0x00FF;
  a[1] = (v>>16) & 0x00FF;
  a[2] = (v>>8) & 0x00FF;
  a[3] = (v>>0) & 0x00FF;
}

/*
** Detect the page-size of the database opened by file-handle pFd by 
** searching the first part of the file for a well-formed SQLite b-tree 
** page. If parameter nReserve is non-zero, then as well as searching for
** a b-tree page with zero reserved bytes, this function searches for one
** with nReserve reserved bytes at the end of it.
**
** If successful, set variable p->detected_pgsz to the detected page-size
** in bytes and return CAPDB_OK. Or, if no error occurs but no valid page
** can be found, return CAPDB_OK but leave p->detected_pgsz set to 0. Or,
** if an error occurs (e.g. an IO or OOM error), then an SQLite error code
** is returned. The final value of p->detected_pgsz is undefined in this
** case.
*/
static int recoverVfsDetectPagesize(
  capdb_recover *p,             /* Recover handle */
  capdb_file *pFd,              /* File-handle open on input database */
  u32 nReserve,                   /* Possible nReserve value */
  i64 nSz                         /* Size of database file in bytes */
){
  int rc = CAPDB_OK;
  const int nMin = 512;
  const int nMax = 65536;
  const int nMaxBlk = 4;
  u32 pgsz = 0;
  int iBlk = 0;
  u8 *aPg = 0;
  u8 *aTmp = 0;
  int nBlk = 0;

  aPg = (u8*)capdb_malloc(2*nMax);
  if( aPg==0 ) return CAPDB_NOMEM;
  aTmp = &aPg[nMax];

  nBlk = (nSz+nMax-1)/nMax;
  if( nBlk>nMaxBlk ) nBlk = nMaxBlk;

  do {
    for(iBlk=0; rc==CAPDB_OK && iBlk<nBlk; iBlk++){
      int nByte = (nSz>=((iBlk+1)*nMax)) ? nMax : (nSz % nMax);
      memset(aPg, 0, nMax);
      rc = pFd->pMethods->xRead(pFd, aPg, nByte, iBlk*nMax);
      if( rc==CAPDB_OK ){
        int pgsz2;
        for(pgsz2=(pgsz ? pgsz*2 : nMin); pgsz2<=nMax; pgsz2=pgsz2*2){
          int iOff;
          for(iOff=0; iOff<nMax; iOff+=pgsz2){
            if( recoverIsValidPage(aTmp, &aPg[iOff], pgsz2-nReserve) ){
              pgsz = pgsz2;
              break;
            }
          }
        }
      }
    }
    if( pgsz>(u32)p->detected_pgsz ){
      p->detected_pgsz = pgsz;
      p->nReserve = nReserve;
    }
    if( nReserve==0 ) break;
    nReserve = 0;
  }while( 1 );

  p->detected_pgsz = pgsz;
  capdb_free(aPg);
  return rc;
}

/*
** The xRead() method of the wrapper VFS. This is used to intercept calls
** to read page 1 of the input database.
*/
static int recoverVfsRead(capdb_file *pFd, void *aBuf, int nByte, i64 iOff){
  int rc = CAPDB_OK;
  if( pFd->pMethods==&recover_methods ){
    pFd->pMethods = recover_g.pMethods;
    rc = pFd->pMethods->xRead(pFd, aBuf, nByte, iOff);
    if( nByte==16 ){
      capdb_randomness(16, aBuf);
    }else
    if( rc==CAPDB_OK && iOff==0 && nByte>=108 ){
      /* Ensure that the database has a valid header file. The only fields
      ** that really matter to recovery are:
      **
      **   + Database page size (16-bits at offset 16)
      **   + Size of db in pages (32-bits at offset 28)
      **   + Database encoding (32-bits at offset 56)
      **
      ** Also preserved are:
      **
      **   + first freelist page (32-bits at offset 32)
      **   + size of freelist (32-bits at offset 36)
      **   + the wal-mode flags (16-bits at offset 18)
      **
      ** We also try to preserve the auto-vacuum, incr-value, user-version
      ** and application-id fields - all 32 bit quantities at offsets 
      ** 52, 60, 64 and 68. All other fields are set to known good values.
      **
      ** Byte offset 105 should also contain the page-size as a 16-bit 
      ** integer.
      */
      const int aPreserve[] = {32, 36, 52, 60, 64, 68};
      u8 aHdr[108] = {
        0x53, 0x51, 0x4c, 0x69, 0x74, 0x65, 0x20, 0x66, 
        0x6f, 0x72, 0x6d, 0x61, 0x74, 0x20, 0x33, 0x00,
        0xFF, 0xFF, 0x01, 0x01, 0x00, 0x40, 0x20, 0x20,
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x10, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x2e, 0x5b, 0x30,

        0x0D, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00
      };
      u8 *a = (u8*)aBuf;

      u32 pgsz = recoverGetU16(&a[16]);
      u32 nReserve = a[20];
      u32 enc = recoverGetU32(&a[56]);
      u32 dbsz = 0;
      i64 dbFileSize = 0;
      int ii;
      capdb_recover *p = recover_g.p;

      if( pgsz==0x01 ) pgsz = 65536;
      rc = pFd->pMethods->xFileSize(pFd, &dbFileSize);

      if( rc==CAPDB_OK && p->detected_pgsz==0 ){
        rc = recoverVfsDetectPagesize(p, pFd, nReserve, dbFileSize);
      }
      if( p->detected_pgsz ){
        pgsz = p->detected_pgsz;
        nReserve = p->nReserve;
      }

      if( pgsz ){
        dbsz = dbFileSize / pgsz;
      }
      if( enc!=CAPDB_UTF8 && enc!=CAPDB_UTF16BE && enc!=CAPDB_UTF16LE ){
        enc = CAPDB_UTF8;
      }

      capdb_free(p->pPage1Cache);
      p->pPage1Cache = 0;
      p->pPage1Disk = 0;

      p->pgsz = nByte;
      p->pPage1Cache = (u8*)recoverMalloc(p, nByte*2);
      if( p->pPage1Cache ){
        p->pPage1Disk = &p->pPage1Cache[nByte];
        memcpy(p->pPage1Disk, aBuf, nByte);
        aHdr[18] = a[18];
        aHdr[19] = a[19];
        recoverPutU32(&aHdr[28], dbsz);
        recoverPutU32(&aHdr[56], enc);
        recoverPutU16(&aHdr[105], pgsz-nReserve);
        if( pgsz==65536 ) pgsz = 1;
        recoverPutU16(&aHdr[16], pgsz);
        aHdr[20] = nReserve;
        for(ii=0; ii<(int)(sizeof(aPreserve)/sizeof(aPreserve[0])); ii++){
          memcpy(&aHdr[aPreserve[ii]], &a[aPreserve[ii]], 4);
        }
        memcpy(aBuf, aHdr, sizeof(aHdr));
        memset(&((u8*)aBuf)[sizeof(aHdr)], 0, nByte-sizeof(aHdr));

        memcpy(p->pPage1Cache, aBuf, nByte);
      }else{
        rc = p->errCode;
      }

    }
    pFd->pMethods = &recover_methods;
  }else{
    rc = pFd->pMethods->xRead(pFd, aBuf, nByte, iOff);
  }
  return rc;
}

/*
** Used to make capdb_io_methods wrapper methods less verbose.
*/
#define RECOVER_VFS_WRAPPER(code)                         \
  int rc = CAPDB_OK;                                     \
  if( pFd->pMethods==&recover_methods ){                  \
    pFd->pMethods = recover_g.pMethods;                   \
    rc = code;                                            \
    pFd->pMethods = &recover_methods;                     \
  }else{                                                  \
    rc = code;                                            \
  }                                                       \
  return rc;                                              

/*
** Methods of the wrapper VFS. All methods except for xRead() and xClose()
** simply uninstall the capdb_io_methods wrapper, invoke the equivalent
** method on the lower level VFS, then reinstall the wrapper before returning.
** Those that return an integer value use the RECOVER_VFS_WRAPPER macro.
*/
static int recoverVfsWrite(
  capdb_file *pFd, const void *aBuf, int nByte, i64 iOff
){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xWrite(pFd, aBuf, nByte, iOff)
  );
}
static int recoverVfsTruncate(capdb_file *pFd, capdb_int64 size){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xTruncate(pFd, size)
  );
}
static int recoverVfsSync(capdb_file *pFd, int flags){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xSync(pFd, flags)
  );
}
static int recoverVfsFileSize(capdb_file *pFd, capdb_int64 *pSize){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xFileSize(pFd, pSize)
  );
}
static int recoverVfsLock(capdb_file *pFd, int eLock){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xLock(pFd, eLock)
  );
}
static int recoverVfsUnlock(capdb_file *pFd, int eLock){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xUnlock(pFd, eLock)
  );
}
static int recoverVfsCheckReservedLock(capdb_file *pFd, int *pResOut){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xCheckReservedLock(pFd, pResOut)
  );
}
static int recoverVfsFileControl(capdb_file *pFd, int op, void *pArg){
  RECOVER_VFS_WRAPPER (
    (pFd->pMethods ?  pFd->pMethods->xFileControl(pFd, op, pArg) : CAPDB_NOTFOUND)
  );
}
static int recoverVfsSectorSize(capdb_file *pFd){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xSectorSize(pFd)
  );
}
static int recoverVfsDeviceCharacteristics(capdb_file *pFd){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xDeviceCharacteristics(pFd)
  );
}
static int recoverVfsShmMap(
  capdb_file *pFd, int iPg, int pgsz, int bExtend, void volatile **pp
){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xShmMap(pFd, iPg, pgsz, bExtend, pp)
  );
}
static int recoverVfsShmLock(capdb_file *pFd, int offset, int n, int flags){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xShmLock(pFd, offset, n, flags)
  );
}
static void recoverVfsShmBarrier(capdb_file *pFd){
  if( pFd->pMethods==&recover_methods ){
    pFd->pMethods = recover_g.pMethods;
    pFd->pMethods->xShmBarrier(pFd);
    pFd->pMethods = &recover_methods;
  }else{
    pFd->pMethods->xShmBarrier(pFd);
  }
}
static int recoverVfsShmUnmap(capdb_file *pFd, int deleteFlag){
  RECOVER_VFS_WRAPPER (
      pFd->pMethods->xShmUnmap(pFd, deleteFlag)
  );
}

static int recoverVfsFetch(
  capdb_file *pFd, 
  capdb_int64 iOff, 
  int iAmt, 
  void **pp
){
  (void)pFd;
  (void)iOff;
  (void)iAmt;
  *pp = 0;
  return CAPDB_OK;
}
static int recoverVfsUnfetch(capdb_file *pFd, capdb_int64 iOff, void *p){
  (void)pFd;
  (void)iOff;
  (void)p;
  return CAPDB_OK;
}

/*
** Install the VFS wrapper around the file-descriptor open on the input
** database for recover handle p. Mutex RECOVER_MUTEX_ID must be held
** when this function is called.
*/
static void recoverInstallWrapper(capdb_recover *p){
  capdb_file *pFd = 0;
  assert( recover_g.pMethods==0 );
  recoverAssertMutexHeld();
  capdb_file_control(p->dbIn, p->zDb, CAPDB_FCNTL_FILE_POINTER, (void*)&pFd);
  assert( pFd==0 || pFd->pMethods!=&recover_methods );
  if( pFd && pFd->pMethods ){
    int iVersion = 1 + (pFd->pMethods->iVersion>1 && pFd->pMethods->xShmMap!=0);
    recover_g.pMethods = pFd->pMethods;
    recover_g.p = p;
    recover_methods.iVersion = iVersion;
    pFd->pMethods = &recover_methods;
  }
}

/*
** Uninstall the VFS wrapper that was installed around the file-descriptor open
** on the input database for recover handle p. Mutex RECOVER_MUTEX_ID must be
** held when this function is called.
*/
static void recoverUninstallWrapper(capdb_recover *p){
  capdb_file *pFd = 0;
  recoverAssertMutexHeld();
  capdb_file_control(p->dbIn, p->zDb,CAPDB_FCNTL_FILE_POINTER,(void*)&pFd);
  if( pFd && pFd->pMethods ){
    pFd->pMethods = recover_g.pMethods;
    recover_g.pMethods = 0;
    recover_g.p = 0;
  }
}

/*
** This function does the work of a single capdb_recover_step() call. It
** is guaranteed that the handle is not in an error state when this
** function is called.
*/
static void recoverStep(capdb_recover *p){
  assert( p && p->errCode==CAPDB_OK );
  switch( p->eState ){
    case RECOVER_STATE_INIT: {
      int bUseWrapper = 1;
      /* This is the very first call to capdb_recover_step() on this object.
      */
      recoverSqlCallback(p, "BEGIN");
      recoverSqlCallback(p, "PRAGMA writable_schema = on");
      recoverSqlCallback(p, "PRAGMA foreign_keys = off");

      recoverEnterMutex();

      /* Open the output database. And register required virtual tables and 
      ** user functions with the new handle. */
      recoverOpenOutput(p);

      /* Attempt to open a transaction and read page 1 of the input database.
      ** Two attempts may be made - one with a wrapper installed to ensure
      ** that the database header is sane, and then if that attempt returns
      ** CAPDB_NOTADB, then again with no wrapper. The second attempt is
      ** required for encrypted databases.  */
      if( p->errCode==CAPDB_OK ){
        do{
          p->errCode = CAPDB_OK;
          if( bUseWrapper ) recoverInstallWrapper(p);

          /* Open a transaction on the input database. */
          capdb_file_control(p->dbIn, p->zDb, CAPDB_FCNTL_RESET_CACHE, 0);
          recoverExec(p, p->dbIn, "PRAGMA writable_schema = on");
          recoverExec(p, p->dbIn, "BEGIN");
          if( p->errCode==CAPDB_OK ) p->bCloseTransaction = 1;
          recoverExec(p, p->dbIn, "SELECT 1 FROM sqlite_schema");
          recoverTransferSettings(p);
          recoverOpenRecovery(p);
          recoverCacheSchema(p);

          if( bUseWrapper ) recoverUninstallWrapper(p);
        }while( p->errCode==CAPDB_NOTADB 
             && (bUseWrapper--) 
             && CAPDB_OK==recoverOneStmt(p->dbIn, "ROLLBACK")
        );
      }

      recoverLeaveMutex();
      recoverExec(p, p->dbOut, "BEGIN");
      recoverWriteSchema1(p);
      p->eState = RECOVER_STATE_WRITING;
      break;
    }
      
    case RECOVER_STATE_WRITING: {
      if( p->w1.pTbls==0 ){
        recoverWriteDataInit(p);
      }
      if( CAPDB_DONE==recoverWriteDataStep(p) ){
        recoverWriteDataCleanup(p);
        if( p->zLostAndFound ){
          p->eState = RECOVER_STATE_LOSTANDFOUND1;
        }else{
          p->eState = RECOVER_STATE_SCHEMA2;
        }
      }
      break;
    }

    case RECOVER_STATE_LOSTANDFOUND1: {
      if( p->laf.pUsed==0 ){
        recoverLostAndFound1Init(p);
      }
      if( CAPDB_DONE==recoverLostAndFound1Step(p) ){
        p->eState = RECOVER_STATE_LOSTANDFOUND2;
      }
      break;
    }
    case RECOVER_STATE_LOSTANDFOUND2: {
      if( p->laf.pAllAndParent==0 ){
        recoverLostAndFound2Init(p);
      }
      if( CAPDB_DONE==recoverLostAndFound2Step(p) ){
        p->eState = RECOVER_STATE_LOSTANDFOUND3;
      }
      break;
    }

    case RECOVER_STATE_LOSTANDFOUND3: {
      if( p->laf.pInsert==0 ){
        recoverLostAndFound3Init(p);
      }
      if( CAPDB_DONE==recoverLostAndFound3Step(p) ){
        p->eState = RECOVER_STATE_SCHEMA2;
      }
      break;
    }

    case RECOVER_STATE_SCHEMA2: {
      int rc = CAPDB_OK;

      recoverWriteSchema2(p);
      p->eState = RECOVER_STATE_DONE;

      /* If no error has occurred, commit the write transaction on the output
      ** database. Regardless of whether or not an error has occurred, make
      ** an attempt to end the read transaction on the input database.  */
      recoverExec(p, p->dbOut, "COMMIT");
      rc = recoverOneStmt(p->dbIn, "END");
      if( p->errCode==CAPDB_OK ) p->errCode = rc;

      recoverSqlCallback(p, "PRAGMA writable_schema = off");
      recoverSqlCallback(p, "COMMIT");
      p->eState = RECOVER_STATE_DONE;
      recoverFinalCleanup(p);
      break;
    };

    case RECOVER_STATE_DONE: {
      /* no-op */
      break;
    };
  }
}


/*
** This is a worker function that does the heavy lifting for both init
** functions:
**
**     capdb_recover_init()
**     capdb_recover_init_sql()
**
** All this function does is allocate space for the recover handle and
** take copies of the input parameters. All the real work is done within
** capdb_recover_run().
*/
capdb_recover *recoverInit(
  capdb* db, 
  const char *zDb, 
  const char *zUri,               /* Output URI for _recover_init() */
  int (*xSql)(void*, const char*),/* SQL callback for _recover_init_sql() */
  void *pSqlCtx                   /* Context arg for _recover_init_sql() */
){
  capdb_recover *pRet = 0;
  int nDb = 0;
  int nUri = 0;
  int nByte = 0;

  if( zDb==0 ){ zDb = "main"; }

  nDb = recoverStrlen(zDb);
  nUri = recoverStrlen(zUri);

  nByte = sizeof(capdb_recover) + nDb+1 + nUri+1;
  pRet = (capdb_recover*)capdb_malloc(nByte);
  if( pRet ){
    memset(pRet, 0, nByte);
    pRet->dbIn = db;
    pRet->zDb = (char*)&pRet[1];
    pRet->zUri = &pRet->zDb[nDb+1];
    memcpy(pRet->zDb, zDb, nDb);
    if( nUri>0 && zUri ) memcpy(pRet->zUri, zUri, nUri);
    pRet->xSql = xSql;
    pRet->pSqlCtx = pSqlCtx;
    pRet->bRecoverRowid = RECOVER_ROWID_DEFAULT;
  }

  return pRet;
}

/*
** Initialize a recovery handle that creates a new database containing
** the recovered data.
*/
capdb_recover *capdb_recover_init(
  capdb* db, 
  const char *zDb, 
  const char *zUri
){
  return recoverInit(db, zDb, zUri, 0, 0);
}

/*
** Initialize a recovery handle that returns recovered data in the
** form of SQL statements via a callback.
*/
capdb_recover *capdb_recover_init_sql(
  capdb* db, 
  const char *zDb, 
  int (*xSql)(void*, const char*),
  void *pSqlCtx
){
  return recoverInit(db, zDb, 0, xSql, pSqlCtx);
}

/*
** Return the handle error message, if any.
*/
const char *capdb_recover_errmsg(capdb_recover *p){
  return (p && p->errCode!=CAPDB_NOMEM) ? p->zErrMsg : "out of memory";
}

/*
** Return the handle error code.
*/
int capdb_recover_errcode(capdb_recover *p){
  return p ? p->errCode : CAPDB_NOMEM;
}

/*
** Configure the handle.
*/
int capdb_recover_config(capdb_recover *p, int op, void *pArg){
  int rc = CAPDB_OK;
  if( p==0 ){
    rc = CAPDB_NOMEM;
  }else if( p->eState!=RECOVER_STATE_INIT ){
    rc = CAPDB_MISUSE;
  }else{
    switch( op ){
      case 789:
        /* This undocumented magic configuration option is used to set the
        ** name of the auxiliary database that is ATTACH-ed to the database
        ** connection and used to hold state information during the
        ** recovery process.  This option is for debugging use only and
        ** is subject to change or removal at any time. */
        capdb_free(p->zStateDb);
        p->zStateDb = recoverMPrintf(p, "%s", (char*)pArg);
        break;

      case CAPDB_RECOVER_LOST_AND_FOUND: {
        const char *zArg = (const char*)pArg;
        capdb_free(p->zLostAndFound);
        if( zArg ){
          p->zLostAndFound = recoverMPrintf(p, "%s", zArg);
        }else{
          p->zLostAndFound = 0;
        }
        break;
      }

      case CAPDB_RECOVER_FREELIST_CORRUPT:
        p->bFreelistCorrupt = *(int*)pArg;
        break;

      case CAPDB_RECOVER_ROWIDS:
        p->bRecoverRowid = *(int*)pArg;
        break;

      case CAPDB_RECOVER_SLOWINDEXES:
        p->bSlowIndexes = *(int*)pArg;
        break;

      default:
        rc = CAPDB_NOTFOUND;
        break;
    }
  }

  return rc;
}

/*
** Do a unit of work towards the recovery job. Return CAPDB_OK if
** no error has occurred but database recovery is not finished, CAPDB_DONE
** if database recovery has been successfully completed, or an SQLite
** error code if an error has occurred.
*/
int capdb_recover_step(capdb_recover *p){
  if( p==0 ) return CAPDB_NOMEM;
  if( p->errCode==CAPDB_OK ) recoverStep(p);
  if( p->eState==RECOVER_STATE_DONE && p->errCode==CAPDB_OK ){
    return CAPDB_DONE;
  }
  return p->errCode;
}

/*
** Do the configured recovery operation. Return CAPDB_OK if successful, or
** else an SQLite error code.
*/
int capdb_recover_run(capdb_recover *p){
  while( CAPDB_OK==capdb_recover_step(p) );
  return capdb_recover_errcode(p);
}


/*
** Free all resources associated with the recover handle passed as the only
** argument. The results of using a handle with any capdb_recover_**
** API function after it has been passed to this function are undefined.
**
** A copy of the value returned by the first call made to capdb_recover_run()
** on this handle is returned, or CAPDB_OK if capdb_recover_run() has
** not been called on this handle.
*/
int capdb_recover_finish(capdb_recover *p){
  int rc;
  if( p==0 ){
    rc = CAPDB_NOMEM;
  }else{
    recoverFinalCleanup(p);
    if( p->bCloseTransaction && capdb_get_autocommit(p->dbIn)==0 ){
      rc = recoverOneStmt(p->dbIn, "END");
      if( p->errCode==CAPDB_OK ) p->errCode = rc;
    }
    rc = p->errCode;
    capdb_free(p->zErrMsg);
    capdb_free(p->zStateDb);
    capdb_free(p->zLostAndFound);
    capdb_free(p->pPage1Cache);
    capdb_free(p);
  }
  return rc;
}

#endif /* ifndef CAPDB_OMIT_VIRTUALTABLE */
