/*
** 2017 April 09
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
#include "capdbexpert.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

#if !defined(CAPDB_AMALGAMATION)
#if defined(CAPDB_COVERAGE_TEST) || defined(CAPDB_MUTATION_TEST)
# define CAPDB_OMIT_AUXILIARY_SAFETY_CHECKS 1
#endif
#if defined(CAPDB_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif
#endif /* !defined(CAPDB_AMALGAMATION) */


#ifndef CAPDB_OMIT_VIRTUALTABLE

typedef capdb_int64 i64;
typedef capdb_uint64 u64;

typedef struct IdxColumn IdxColumn;
typedef struct IdxConstraint IdxConstraint;
typedef struct IdxScan IdxScan;
typedef struct IdxStatement IdxStatement;
typedef struct IdxTable IdxTable;
typedef struct IdxWrite IdxWrite;

#define STRLEN  (int)strlen

/*
** A temp table name that we assume no user database will actually use.
** If this assumption proves incorrect triggers on the table with the
** conflicting name will be ignored.
*/
#define UNIQUE_TABLE_NAME "t592690916721053953805701627921227776"

/*
** A single constraint. Equivalent to either "col = ?" or "col < ?" (or
** any other type of single-ended range constraint on a column).
**
** pLink:
**   Used to temporarily link IdxConstraint objects into lists while
**   creating candidate indexes.
*/
struct IdxConstraint {
  char *zColl;                    /* Collation sequence */
  int bRange;                     /* True for range, false for eq */
  int iCol;                       /* Constrained table column */
  int bFlag;                      /* Used by idxFindCompatible() */
  int bDesc;                      /* True if ORDER BY <expr> DESC */
  IdxConstraint *pNext;           /* Next constraint in pEq or pRange list */
  IdxConstraint *pLink;           /* See above */
};

/*
** A single scan of a single table.
*/
struct IdxScan {
  IdxTable *pTab;                 /* Associated table object */
  int iDb;                        /* Database containing table zTable */
  i64 covering;                   /* Mask of columns required for cov. index */
  IdxConstraint *pOrder;          /* ORDER BY columns */
  IdxConstraint *pEq;             /* List of == constraints */
  IdxConstraint *pRange;          /* List of < constraints */
  IdxScan *pNextScan;             /* Next IdxScan object for same analysis */
};

/*
** Information regarding a single database table. Extracted from 
** "PRAGMA table_info" by function idxGetTableInfo().
*/
struct IdxColumn {
  char *zName;
  char *zColl;
  int iPk;
};
struct IdxTable {
  int nCol;
  char *zName;                    /* Table name */
  IdxColumn *aCol;
  IdxTable *pNext;                /* Next table in linked list of all tables */
};

/*
** An object of the following type is created for each unique table/write-op
** seen. The objects are stored in a singly-linked list beginning at
** capdbexpert.pWrite.
*/
struct IdxWrite {
  IdxTable *pTab;
  int eOp;                        /* CAPDB_UPDATE, DELETE or INSERT */
  IdxWrite *pNext;
};

/*
** Each statement being analyzed is represented by an instance of this
** structure.
*/
struct IdxStatement {
  int iId;                        /* Statement number */
  char *zSql;                     /* SQL statement */
  char *zIdx;                     /* Indexes */
  char *zEQP;                     /* Plan */
  IdxStatement *pNext;
};


/*
** A hash table for storing strings. With space for a payload string
** with each entry. Methods are:
**
**   idxHashInit()
**   idxHashClear()
**   idxHashAdd()
**   idxHashSearch()
*/
#define IDX_HASH_SIZE 1023
typedef struct IdxHashEntry IdxHashEntry;
typedef struct IdxHash IdxHash;
struct IdxHashEntry {
  char *zKey;                     /* nul-terminated key */
  char *zVal;                     /* nul-terminated value string */
  char *zVal2;                    /* nul-terminated value string 2 */
  IdxHashEntry *pHashNext;        /* Next entry in same hash bucket */
  IdxHashEntry *pNext;            /* Next entry in hash */
};
struct IdxHash {
  IdxHashEntry *pFirst;
  IdxHashEntry *aHash[IDX_HASH_SIZE];
};

/*
** capdbexpert object.
*/
struct capdbexpert {
  int iSample;                    /* Percentage of tables to sample for stat1 */
  capdb *db;                    /* User database */
  capdb *dbm;                   /* In-memory db for this analysis */
  capdb *dbv;                   /* Vtab schema for this analysis */
  IdxTable *pTable;               /* List of all IdxTable objects */
  IdxScan *pScan;                 /* List of scan objects */
  IdxWrite *pWrite;               /* List of write objects */
  IdxStatement *pStatement;       /* List of IdxStatement objects */
  int bRun;                       /* True once analysis has run */
  char **pzErrmsg;
  int rc;                         /* Error code from whereinfo hook */
  IdxHash hIdx;                   /* Hash containing all candidate indexes */
  char *zCandidates;              /* For EXPERT_REPORT_CANDIDATES */
};


/*
** Allocate and return nByte bytes of zeroed memory using capdb_malloc(). 
** If the allocation fails, set *pRc to CAPDB_NOMEM and return NULL.
*/
static void *idxMalloc(int *pRc, i64 nByte){
  void *pRet;
  assert( *pRc==CAPDB_OK );
  assert( nByte>0 );
  pRet = capdb_malloc64(nByte);
  if( pRet ){
    memset(pRet, 0, nByte);
  }else{
    *pRc = CAPDB_NOMEM;
  }
  return pRet;
}

/*
** Initialize an IdxHash hash table.
*/
static void idxHashInit(IdxHash *pHash){
  memset(pHash, 0, sizeof(IdxHash));
}

/*
** Reset an IdxHash hash table.
*/
static void idxHashClear(IdxHash *pHash){
  int i;
  for(i=0; i<IDX_HASH_SIZE; i++){
    IdxHashEntry *pEntry;
    IdxHashEntry *pNext;
    for(pEntry=pHash->aHash[i]; pEntry; pEntry=pNext){
      pNext = pEntry->pHashNext;
      capdb_free(pEntry->zVal2);
      capdb_free(pEntry);
    }
  }
  memset(pHash, 0, sizeof(IdxHash));
}

/*
** Return the index of the hash bucket that the string specified by the
** arguments to this function belongs.
*/
static int idxHashString(const char *z, int n){
  unsigned int ret = 0;
  int i;
  for(i=0; i<n; i++){
    ret += (ret<<3) + (unsigned char)(z[i]);
  }
  return (int)(ret % IDX_HASH_SIZE);
}

/*
** If zKey is already present in the hash table, return non-zero and do
** nothing. Otherwise, add an entry with key zKey and payload string zVal to
** the hash table passed as the second argument. 
*/
static int idxHashAdd(
  int *pRc, 
  IdxHash *pHash, 
  const char *zKey,
  const char *zVal
){
  int nKey = STRLEN(zKey);
  int iHash = idxHashString(zKey, nKey);
  int nVal = (zVal ? STRLEN(zVal) : 0);
  IdxHashEntry *pEntry;
  assert( iHash>=0 );
  for(pEntry=pHash->aHash[iHash]; pEntry; pEntry=pEntry->pHashNext){
    if( STRLEN(pEntry->zKey)==nKey && 0==memcmp(pEntry->zKey, zKey, nKey) ){
      return 1;
    }
  }
  pEntry = idxMalloc(pRc, sizeof(IdxHashEntry) + (i64)nKey+1 + (i64)nVal+1);
  if( pEntry ){
    pEntry->zKey = (char*)&pEntry[1];
    memcpy(pEntry->zKey, zKey, nKey);
    if( zVal ){
      pEntry->zVal = &pEntry->zKey[nKey+1];
      memcpy(pEntry->zVal, zVal, nVal);
    }
    pEntry->pHashNext = pHash->aHash[iHash];
    pHash->aHash[iHash] = pEntry;

    pEntry->pNext = pHash->pFirst;
    pHash->pFirst = pEntry;
  }
  return 0;
}

/*
** If zKey/nKey is present in the hash table, return a pointer to the 
** hash-entry object.
*/
static IdxHashEntry *idxHashFind(IdxHash *pHash, const char *zKey, int nKey){
  int iHash;
  IdxHashEntry *pEntry;
  if( nKey<0 ) nKey = STRLEN(zKey);
  iHash = idxHashString(zKey, nKey);
  assert( iHash>=0 );
  for(pEntry=pHash->aHash[iHash]; pEntry; pEntry=pEntry->pHashNext){
    if( STRLEN(pEntry->zKey)==nKey && 0==memcmp(pEntry->zKey, zKey, nKey) ){
      return pEntry;
    }
  }
  return 0;
}

/*
** If the hash table contains an entry with a key equal to the string
** passed as the final two arguments to this function, return a pointer
** to the payload string. Otherwise, if zKey/nKey is not present in the
** hash table, return NULL.
*/
static const char *idxHashSearch(IdxHash *pHash, const char *zKey, int nKey){
  IdxHashEntry *pEntry = idxHashFind(pHash, zKey, nKey);
  if( pEntry ) return pEntry->zVal;
  return 0;
}

/*
** Allocate and return a new IdxConstraint object. Set the IdxConstraint.zColl
** variable to point to a copy of nul-terminated string zColl.
*/
static IdxConstraint *idxNewConstraint(int *pRc, const char *zColl){
  IdxConstraint *pNew;
  int nColl = STRLEN(zColl);

  assert( *pRc==CAPDB_OK );
  pNew = (IdxConstraint*)idxMalloc(pRc, sizeof(IdxConstraint) * nColl + 1);
  if( pNew ){
    pNew->zColl = (char*)&pNew[1];
    memcpy(pNew->zColl, zColl, nColl+1);
  }
  return pNew;
}

/*
** An error associated with database handle db has just occurred. Pass
** the error message to callback function xOut.
*/
static void idxDatabaseError(
  capdb *db,                    /* Database handle */
  char **pzErrmsg                 /* Write error here */
){
  *pzErrmsg = capdb_mprintf("%s", capdb_errmsg(db));
}

/*
** Prepare an SQL statement.
*/
static int idxPrepareStmt(
  capdb *db,                    /* Database handle to compile against */
  capdb_stmt **ppStmt,          /* OUT: Compiled SQL statement */
  char **pzErrmsg,                /* OUT: capdb_malloc()ed error message */
  const char *zSql                /* SQL statement to compile */
){
  int rc = capdb_prepare_v2(db, zSql, -1, ppStmt, 0);
  if( rc!=CAPDB_OK ){
    *ppStmt = 0;
    idxDatabaseError(db, pzErrmsg);
  }
  return rc;
}

/*
** Prepare an SQL statement using the results of a printf() formatting.
*/
static int idxPrintfPrepareStmt(
  capdb *db,                    /* Database handle to compile against */
  capdb_stmt **ppStmt,          /* OUT: Compiled SQL statement */
  char **pzErrmsg,                /* OUT: capdb_malloc()ed error message */
  const char *zFmt,               /* printf() format of SQL statement */
  ...                             /* Trailing printf() arguments */
){
  va_list ap;
  int rc;
  char *zSql;
  va_start(ap, zFmt);
  zSql = capdb_vmprintf(zFmt, ap);
  if( zSql==0 ){
    rc = CAPDB_NOMEM;
  }else{
    rc = idxPrepareStmt(db, ppStmt, pzErrmsg, zSql);
    capdb_free(zSql);
  }
  va_end(ap);
  return rc;
}


/*************************************************************************
** Beginning of virtual table implementation.
*/
typedef struct ExpertVtab ExpertVtab;
struct ExpertVtab {
  capdb_vtab base;
  IdxTable *pTab;
  capdbexpert *pExpert;
};

typedef struct ExpertCsr ExpertCsr;
struct ExpertCsr {
  capdb_vtab_cursor base;
  capdb_stmt *pData;
};

static char *expertDequote(const char *zIn){
  i64 n = STRLEN(zIn);
  char *zRet = capdb_malloc64(n);

  assert( zIn[0]=='\'' );
  assert( zIn[n-1]=='\'' );

  if( zRet ){
    i64 iOut = 0;
    i64 iIn = 0;
    for(iIn=1; iIn<(n-1); iIn++){
      if( zIn[iIn]=='\'' ){
        assert( zIn[iIn+1]=='\'' );
        iIn++;
      }
      zRet[iOut++] = zIn[iIn];
    }
    zRet[iOut] = '\0';
  }

  return zRet;
}

/* 
** This function is the implementation of both the xConnect and xCreate
** methods of the r-tree virtual table.
**
**   argv[0]   -> module name
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> column names...
*/
static int expertConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  capdbexpert *pExpert = (capdbexpert*)pAux;
  ExpertVtab *p = 0;
  int rc;

  if( argc!=4 ){
    *pzErr = capdb_mprintf("internal error!");
    rc = CAPDB_ERROR;
  }else{
    char *zCreateTable = expertDequote(argv[3]);
    if( zCreateTable ){
      rc = capdb_declare_vtab(db, zCreateTable);
      if( rc==CAPDB_OK ){
        p = idxMalloc(&rc, sizeof(ExpertVtab));
      }
      if( rc==CAPDB_OK ){
        p->pExpert = pExpert;
        p->pTab = pExpert->pTable;
        assert( capdb_stricmp(p->pTab->zName, argv[2])==0 );
      }
      capdb_free(zCreateTable);
    }else{
      rc = CAPDB_NOMEM;
    }
  }

  *ppVtab = (capdb_vtab*)p;
  return rc;
}

static int expertDisconnect(capdb_vtab *pVtab){
  ExpertVtab *p = (ExpertVtab*)pVtab;
  capdb_free(p);
  return CAPDB_OK;
}

static int expertBestIndex(capdb_vtab *pVtab, capdb_index_info *pIdxInfo){
  ExpertVtab *p = (ExpertVtab*)pVtab;
  int rc = CAPDB_OK;
  int n = 0;
  IdxScan *pScan;
  const int opmask = 
    CAPDB_INDEX_CONSTRAINT_EQ | CAPDB_INDEX_CONSTRAINT_GT |
    CAPDB_INDEX_CONSTRAINT_LT | CAPDB_INDEX_CONSTRAINT_GE |
    CAPDB_INDEX_CONSTRAINT_LE;

  pScan = idxMalloc(&rc, sizeof(IdxScan));
  if( pScan ){
    int i;

    /* Link the new scan object into the list */
    pScan->pTab = p->pTab;
    pScan->pNextScan = p->pExpert->pScan;
    p->pExpert->pScan = pScan;

    /* Add the constraints to the IdxScan object */
    for(i=0; i<pIdxInfo->nConstraint; i++){
      struct capdb_index_constraint *pCons = &pIdxInfo->aConstraint[i];
      if( pCons->usable 
       && pCons->iColumn>=0 
       && p->pTab->aCol[pCons->iColumn].iPk==0
       && (pCons->op & opmask) 
      ){
        IdxConstraint *pNew;
        const char *zColl = capdb_vtab_collation(pIdxInfo, i);
        pNew = idxNewConstraint(&rc, zColl);
        if( pNew ){
          pNew->iCol = pCons->iColumn;
          if( pCons->op==CAPDB_INDEX_CONSTRAINT_EQ ){
            pNew->pNext = pScan->pEq;
            pScan->pEq = pNew;
          }else{
            pNew->bRange = 1;
            pNew->pNext = pScan->pRange;
            pScan->pRange = pNew;
          }
        }
        n++;
        pIdxInfo->aConstraintUsage[i].argvIndex = n;
      }
    }

    /* Add the ORDER BY to the IdxScan object */
    for(i=pIdxInfo->nOrderBy-1; i>=0; i--){
      int iCol = pIdxInfo->aOrderBy[i].iColumn;
      if( iCol>=0 ){
        IdxConstraint *pNew = idxNewConstraint(&rc, p->pTab->aCol[iCol].zColl);
        if( pNew ){
          pNew->iCol = iCol;
          pNew->bDesc = pIdxInfo->aOrderBy[i].desc;
          pNew->pNext = pScan->pOrder;
          pNew->pLink = pScan->pOrder;
          pScan->pOrder = pNew;
          n++;
        }
      }
    }
  }

  pIdxInfo->estimatedCost = 1000000.0 / (n+1);
  return rc;
}

static int expertUpdate(
  capdb_vtab *pVtab, 
  int nData, 
  capdb_value **azData, 
  sqlite_int64 *pRowid
){
  (void)pVtab;
  (void)nData;
  (void)azData;
  (void)pRowid;
  return CAPDB_OK;
}

/* 
** Virtual table module xOpen method.
*/
static int expertOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  int rc = CAPDB_OK;
  ExpertCsr *pCsr;
  (void)pVTab;
  pCsr = idxMalloc(&rc, sizeof(ExpertCsr));
  *ppCursor = (capdb_vtab_cursor*)pCsr;
  return rc;
}

/* 
** Virtual table module xClose method.
*/
static int expertClose(capdb_vtab_cursor *cur){
  ExpertCsr *pCsr = (ExpertCsr*)cur;
  capdb_finalize(pCsr->pData);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** Virtual table module xEof method.
**
** Return non-zero if the cursor does not currently point to a valid 
** record (i.e if the scan has finished), or zero otherwise.
*/
static int expertEof(capdb_vtab_cursor *cur){
  ExpertCsr *pCsr = (ExpertCsr*)cur;
  return pCsr->pData==0;
}

/* 
** Virtual table module xNext method.
*/
static int expertNext(capdb_vtab_cursor *cur){
  ExpertCsr *pCsr = (ExpertCsr*)cur;
  int rc = CAPDB_OK;

  assert( pCsr->pData );
  rc = capdb_step(pCsr->pData);
  if( rc!=CAPDB_ROW ){
    rc = capdb_finalize(pCsr->pData);
    pCsr->pData = 0;
  }else{
    rc = CAPDB_OK;
  }

  return rc;
}

/* 
** Virtual table module xRowid method.
*/
static int expertRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  (void)cur;
  *pRowid = 0;
  return CAPDB_OK;
}

/* 
** Virtual table module xColumn method.
*/
static int expertColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  ExpertCsr *pCsr = (ExpertCsr*)cur;
  capdb_value *pVal;
  pVal = capdb_column_value(pCsr->pData, i);
  if( pVal ){
    capdb_result_value(ctx, pVal);
  }
  return CAPDB_OK;
}

/* 
** Virtual table module xFilter method.
*/
static int expertFilter(
  capdb_vtab_cursor *cur, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  ExpertCsr *pCsr = (ExpertCsr*)cur;
  ExpertVtab *pVtab = (ExpertVtab*)(cur->pVtab);
  capdbexpert *pExpert = pVtab->pExpert;
  int rc;

  (void)idxNum;
  (void)idxStr;
  (void)argc;
  (void)argv;
  rc = capdb_finalize(pCsr->pData);
  pCsr->pData = 0;
  if( rc==CAPDB_OK ){
    rc = idxPrintfPrepareStmt(pExpert->db, &pCsr->pData, &pVtab->base.zErrMsg,
        "SELECT * FROM main.%Q WHERE sqlite_expert_sample()", pVtab->pTab->zName
    );
  }

  if( rc==CAPDB_OK ){
    rc = expertNext(cur);
  }
  return rc;
}

static int idxRegisterVtab(capdbexpert *p){
  static capdb_module expertModule = {
    2,                            /* iVersion */
    expertConnect,                /* xCreate - create a table */
    expertConnect,                /* xConnect - connect to an existing table */
    expertBestIndex,              /* xBestIndex - Determine search strategy */
    expertDisconnect,             /* xDisconnect - Disconnect from a table */
    expertDisconnect,             /* xDestroy - Drop a table */
    expertOpen,                   /* xOpen - open a cursor */
    expertClose,                  /* xClose - close a cursor */
    expertFilter,                 /* xFilter - configure scan constraints */
    expertNext,                   /* xNext - advance a cursor */
    expertEof,                    /* xEof */
    expertColumn,                 /* xColumn - read data */
    expertRowid,                  /* xRowid - read data */
    expertUpdate,                 /* xUpdate - write data */
    0,                            /* xBegin - begin transaction */
    0,                            /* xSync - sync transaction */
    0,                            /* xCommit - commit transaction */
    0,                            /* xRollback - rollback transaction */
    0,                            /* xFindFunction - function overloading */
    0,                            /* xRename - rename the table */
    0,                            /* xSavepoint */
    0,                            /* xRelease */
    0,                            /* xRollbackTo */
    0,                            /* xShadowName */
    0,                            /* xIntegrity */
  };

  return capdb_create_module(p->dbv, "expert", &expertModule, (void*)p);
}
/*
** End of virtual table implementation.
*************************************************************************/
/*
** Finalize SQL statement pStmt. If (*pRc) is CAPDB_OK when this function
** is called, set it to the return value of capdb_finalize() before
** returning. Otherwise, discard the capdb_finalize() return value.
*/
static void idxFinalize(int *pRc, capdb_stmt *pStmt){
  int rc = capdb_finalize(pStmt);
  if( *pRc==CAPDB_OK ) *pRc = rc;
}

/*
** Attempt to allocate an IdxTable structure corresponding to table zTab
** in the main database of connection db. If successful, set (*ppOut) to
** point to the new object and return CAPDB_OK. Otherwise, return an
** SQLite error code and set (*ppOut) to NULL. In this case *pzErrmsg may be
** set to point to an error string.
**
** It is the responsibility of the caller to eventually free either the
** IdxTable object or error message using capdb_free().
*/
static int idxGetTableInfo(
  capdb *db,                    /* Database connection to read details from */
  const char *zTab,               /* Table name */
  IdxTable **ppOut,               /* OUT: New object (if successful) */
  char **pzErrmsg                 /* OUT: Error message (if not) */
){
  capdb_stmt *p1 = 0;
  int nCol = 0;
  int nTab;
  i64 nByte;
  IdxTable *pNew = 0;
  int rc, rc2;
  char *pCsr = 0;
  int nPk = 0;

  *ppOut = 0;
  if( zTab==0 ) return CAPDB_ERROR;
  nTab = STRLEN(zTab);
  nByte = sizeof(IdxTable) + nTab + 1;
  rc = idxPrintfPrepareStmt(db, &p1, pzErrmsg, "PRAGMA table_xinfo=%Q", zTab);
  while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(p1) ){
    const char *zCol = (const char*)capdb_column_text(p1, 1);
    const char *zColSeq = 0;
    if( zCol==0 ){
      rc = CAPDB_ERROR;
      break;
    }
    nByte += 1 + STRLEN(zCol);
    rc = capdb_table_column_metadata(
        db, "main", zTab, zCol, 0, &zColSeq, 0, 0, 0
    );
    if( zColSeq==0 ) zColSeq = "binary";
    nByte += 1 + STRLEN(zColSeq);
    nCol++;
    nPk += (capdb_column_int(p1, 5)>0);
  }
  rc2 = capdb_reset(p1);
  if( rc==CAPDB_OK ) rc = rc2;

  nByte += sizeof(IdxColumn) * nCol;
  if( rc==CAPDB_OK ){
    pNew = idxMalloc(&rc, nByte);
  }
  if( rc==CAPDB_OK ){
    pNew->aCol = (IdxColumn*)&pNew[1];
    pNew->nCol = nCol;
    pCsr = (char*)&pNew->aCol[nCol];
  }

  nCol = 0;
  while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(p1) ){
    const char *zCol = (const char*)capdb_column_text(p1, 1);
    const char *zColSeq = 0;
    int nCopy;
    if( zCol==0 ) continue;
    nCopy = STRLEN(zCol) + 1;
    pNew->aCol[nCol].zName = pCsr;
    pNew->aCol[nCol].iPk = (capdb_column_int(p1, 5)==1 && nPk==1);
    memcpy(pCsr, zCol, nCopy);
    pCsr += nCopy;

    rc = capdb_table_column_metadata(
        db, "main", zTab, zCol, 0, &zColSeq, 0, 0, 0
    );
    if( rc==CAPDB_OK ){
      if( zColSeq==0 ) zColSeq = "binary";
      nCopy = STRLEN(zColSeq) + 1;
      pNew->aCol[nCol].zColl = pCsr;
      memcpy(pCsr, zColSeq, nCopy);
      pCsr += nCopy;
    }

    nCol++;
  }
  idxFinalize(&rc, p1);

  if( rc!=CAPDB_OK ){
    capdb_free(pNew);
    pNew = 0;
  }else if( ALWAYS(pNew!=0) ){
    pNew->zName = pCsr;
    if( ALWAYS(pNew->zName!=0) ) memcpy(pNew->zName, zTab, nTab+1);
  }

  *ppOut = pNew;
  return rc;
}

/*
** This function is a no-op if *pRc is set to anything other than 
** CAPDB_OK when it is called.
**
** If *pRc is initially set to CAPDB_OK, then the text specified by
** the printf() style arguments is appended to zIn and the result returned
** in a buffer allocated by capdb_malloc(). capdb_free() is called on
** zIn before returning.
*/
static char *idxAppendText(int *pRc, char *zIn, const char *zFmt, ...){
  va_list ap;
  char *zAppend = 0;
  char *zRet = 0;
  i64 nIn = zIn ? STRLEN(zIn) : 0;
  i64 nAppend = 0;
  va_start(ap, zFmt);
  if( *pRc==CAPDB_OK ){
    zAppend = capdb_vmprintf(zFmt, ap);
    if( zAppend ){
      nAppend = STRLEN(zAppend);
      zRet = (char*)capdb_malloc64(nIn + nAppend + 1);
    }
    if( zAppend && zRet ){
      if( nIn ) memcpy(zRet, zIn, nIn);
      memcpy(&zRet[nIn], zAppend, nAppend+1);
    }else{
      capdb_free(zRet);
      zRet = 0;
      *pRc = CAPDB_NOMEM;
    }
    capdb_free(zAppend);
    capdb_free(zIn);
  }
  va_end(ap);
  return zRet;
}

/*
** Return true if zId must be quoted in order to use it as an SQL
** identifier, or false otherwise.
*/
static int idxIdentifierRequiresQuotes(const char *zId){
  int i;
  int nId = STRLEN(zId);
  
  if( capdb_keyword_check(zId, nId) ) return 1;

  for(i=0; zId[i]; i++){
    if( !(zId[i]=='_')
     && !(zId[i]>='0' && zId[i]<='9')
     && !(zId[i]>='a' && zId[i]<='z')
     && !(zId[i]>='A' && zId[i]<='Z')
    ){
      return 1;
    }
  }
  return 0;
}

/*
** This function appends an index column definition suitable for constraint
** pCons to the string passed as zIn and returns the result.
*/
static char *idxAppendColDefn(
  int *pRc,                       /* IN/OUT: Error code */
  char *zIn,                      /* Column defn accumulated so far */
  IdxTable *pTab,                 /* Table index will be created on */
  IdxConstraint *pCons
){
  char *zRet = zIn;
  IdxColumn *p = &pTab->aCol[pCons->iCol];
  if( zRet ) zRet = idxAppendText(pRc, zRet, ", ");

  if( idxIdentifierRequiresQuotes(p->zName) ){
    zRet = idxAppendText(pRc, zRet, "%Q", p->zName);
  }else{
    zRet = idxAppendText(pRc, zRet, "%s", p->zName);
  }

  if( capdb_stricmp(p->zColl, pCons->zColl) ){
    if( idxIdentifierRequiresQuotes(pCons->zColl) ){
      zRet = idxAppendText(pRc, zRet, " COLLATE %Q", pCons->zColl);
    }else{
      zRet = idxAppendText(pRc, zRet, " COLLATE %s", pCons->zColl);
    }
  }

  if( pCons->bDesc ){
    zRet = idxAppendText(pRc, zRet, " DESC");
  }
  return zRet;
}

/*
** Search database dbm for an index compatible with the one idxCreateFromCons()
** would create from arguments pScan, pEq and pTail. If no error occurs and 
** such an index is found, return non-zero. Or, if no such index is found,
** return zero.
**
** If an error occurs, set *pRc to an SQLite error code and return zero.
*/
static int idxFindCompatible(
  int *pRc,                       /* OUT: Error code */
  capdb* dbm,                   /* Database to search */
  IdxScan *pScan,                 /* Scan for table to search for index on */
  IdxConstraint *pEq,             /* List of == constraints */
  IdxConstraint *pTail            /* List of range constraints */
){
  const char *zTbl = pScan->pTab->zName;
  capdb_stmt *pIdxList = 0;
  IdxConstraint *pIter;
  int nEq = 0;                    /* Number of elements in pEq */
  int rc;

  /* Count the elements in list pEq */
  for(pIter=pEq; pIter; pIter=pIter->pLink) nEq++;

  rc = idxPrintfPrepareStmt(dbm, &pIdxList, 0, "PRAGMA index_list=%Q", zTbl);
  while( rc==CAPDB_OK && capdb_step(pIdxList)==CAPDB_ROW ){
    int bMatch = 1;
    IdxConstraint *pT = pTail;
    capdb_stmt *pInfo = 0;
    const char *zIdx = (const char*)capdb_column_text(pIdxList, 1);
    if( zIdx==0 ) continue;

    /* Zero the IdxConstraint.bFlag values in the pEq list */
    for(pIter=pEq; pIter; pIter=pIter->pLink) pIter->bFlag = 0;

    rc = idxPrintfPrepareStmt(dbm, &pInfo, 0, "PRAGMA index_xInfo=%Q", zIdx);
    while( rc==CAPDB_OK && capdb_step(pInfo)==CAPDB_ROW ){
      int iIdx = capdb_column_int(pInfo, 0);
      int iCol = capdb_column_int(pInfo, 1);
      const char *zColl = (const char*)capdb_column_text(pInfo, 4);

      if( iIdx<nEq ){
        for(pIter=pEq; pIter; pIter=pIter->pLink){
          if( pIter->bFlag ) continue;
          if( pIter->iCol!=iCol ) continue;
          if( capdb_stricmp(pIter->zColl, zColl) ) continue;
          pIter->bFlag = 1;
          break;
        }
        if( pIter==0 ){
          bMatch = 0;
          break;
        }
      }else{
        if( pT ){
          if( pT->iCol!=iCol || capdb_stricmp(pT->zColl, zColl) ){
            bMatch = 0;
            break;
          }
          pT = pT->pLink;
        }
      }
    }
    idxFinalize(&rc, pInfo);

    if( rc==CAPDB_OK && bMatch ){
      capdb_finalize(pIdxList);
      return 1;
    }
  }
  idxFinalize(&rc, pIdxList);

  *pRc = rc;
  return 0;
}

/* Callback for capdb_exec() with query with leading count(*) column.
 * The first argument is expected to be an int*, referent to be incremented
 * if that leading column is not exactly '0'.
 */
static int countNonzeros(void* pCount, int nc,
                         char* azResults[], char* azColumns[]){
  (void)azColumns;  /* Suppress unused parameter warning */
  if( nc>0 && (azResults[0][0]!='0' || azResults[0][1]!=0) ){
    *((int *)pCount) += 1;
  }
  return 0;
}

static int idxCreateFromCons(
  capdbexpert *p,
  IdxScan *pScan,
  IdxConstraint *pEq, 
  IdxConstraint *pTail
){
  capdb *dbm = p->dbm;
  int rc = CAPDB_OK;
  if( (pEq || pTail) && 0==idxFindCompatible(&rc, dbm, pScan, pEq, pTail) ){
    IdxTable *pTab = pScan->pTab;
    char *zCols = 0;
    char *zIdx = 0;
    IdxConstraint *pCons;
    unsigned int h = 0;
    const char *zFmt;

    for(pCons=pEq; pCons; pCons=pCons->pLink){
      zCols = idxAppendColDefn(&rc, zCols, pTab, pCons);
    }
    for(pCons=pTail; pCons; pCons=pCons->pLink){
      zCols = idxAppendColDefn(&rc, zCols, pTab, pCons);
    }

    if( rc==CAPDB_OK ){
      /* Hash the list of columns to come up with a name for the index */
      const char *zTable = pScan->pTab->zName;
      int quoteTable = idxIdentifierRequiresQuotes(zTable);
      char *zName = 0;          /* Index name */
      int collisions = 0;
      do{
        int i;
        char *zFind;
        for(i=0; zCols[i]; i++){
          h += ((h<<3) + zCols[i]);
        }
        capdb_free(zName);
        zName = capdb_mprintf("%s_idx_%08x", zTable, h);
        if( zName==0 ) break;
        /* Is is unique among table, view and index names? */
        zFmt = "SELECT count(*) FROM sqlite_schema WHERE name=%Q"
          " AND type in ('index','table','view')";
        zFind = capdb_mprintf(zFmt, zName);
        i = 0;
        rc = capdb_exec(dbm, zFind, countNonzeros, &i, 0);
        assert(rc==CAPDB_OK);
        capdb_free(zFind);
        if( i==0 ){
          collisions = 0;
          break;
        }
        ++collisions;
      }while( collisions<50 && zName!=0 );
      if( collisions ){
        /* This return means "Gave up trying to find a unique index name." */
        rc = CAPDB_BUSY_TIMEOUT;
      }else if( zName==0 ){
        rc = CAPDB_NOMEM;
      }else{
        if( quoteTable ){
          zFmt = "CREATE INDEX \"%w\" ON \"%w\"(%s)";
        }else{
          zFmt = "CREATE INDEX %s ON %s(%s)";
        }
        zIdx = capdb_mprintf(zFmt, zName, zTable, zCols);
        if( !zIdx ){
          rc = CAPDB_NOMEM;
        }else{
          rc = capdb_exec(dbm, zIdx, 0, 0, p->pzErrmsg);
          if( rc!=CAPDB_OK ){
            rc = CAPDB_BUSY_TIMEOUT;
          }else{
            idxHashAdd(&rc, &p->hIdx, zName, zIdx);
          }
        }
        capdb_free(zName);
        capdb_free(zIdx);
      }
    }

    capdb_free(zCols);
  }
  return rc;
}

/*
** Return true if list pList (linked by IdxConstraint.pLink) contains
** a constraint compatible with *p. Otherwise return false.
*/
static int idxFindConstraint(IdxConstraint *pList, IdxConstraint *p){
  IdxConstraint *pCmp;
  for(pCmp=pList; pCmp; pCmp=pCmp->pLink){
    if( p->iCol==pCmp->iCol ) return 1;
  }
  return 0;
}

static int idxCreateFromWhere(
  capdbexpert *p, 
  IdxScan *pScan,                 /* Create indexes for this scan */
  IdxConstraint *pTail            /* range/ORDER BY constraints for inclusion */
){
  IdxConstraint *p1 = 0;
  IdxConstraint *pCon;
  int rc;

  /* Gather up all the == constraints. */
  for(pCon=pScan->pEq; pCon; pCon=pCon->pNext){
    if( !idxFindConstraint(p1, pCon) && !idxFindConstraint(pTail, pCon) ){
      pCon->pLink = p1;
      p1 = pCon;
    }
  }

  /* Create an index using the == constraints collected above. And the
  ** range constraint/ORDER BY terms passed in by the caller, if any. */
  rc = idxCreateFromCons(p, pScan, p1, pTail);

  /* If no range/ORDER BY passed by the caller, create a version of the
  ** index for each range constraint.  */
  if( pTail==0 ){
    for(pCon=pScan->pRange; rc==CAPDB_OK && pCon; pCon=pCon->pNext){
      assert( pCon->pLink==0 );
      if( !idxFindConstraint(p1, pCon) && !idxFindConstraint(pTail, pCon) ){
        rc = idxCreateFromCons(p, pScan, p1, pCon);
      }
    }
  }

  return rc;
}

/*
** Create candidate indexes in database [dbm] based on the data in 
** linked-list pScan.
*/
static int idxCreateCandidates(capdbexpert *p){
  int rc = CAPDB_OK;
  IdxScan *pIter;

  for(pIter=p->pScan; pIter && rc==CAPDB_OK; pIter=pIter->pNextScan){
    rc = idxCreateFromWhere(p, pIter, 0);
    if( rc==CAPDB_OK && pIter->pOrder ){
      rc = idxCreateFromWhere(p, pIter, pIter->pOrder);
    }
  }

  return rc;
}

/*
** Free all elements of the linked list starting at pConstraint.
*/
static void idxConstraintFree(IdxConstraint *pConstraint){
  IdxConstraint *pNext;
  IdxConstraint *p;

  for(p=pConstraint; p; p=pNext){
    pNext = p->pNext;
    capdb_free(p);
  }
}

/*
** Free all elements of the linked list starting from pScan up until pLast
** (pLast is not freed).
*/
static void idxScanFree(IdxScan *pScan, IdxScan *pLast){
  IdxScan *p;
  IdxScan *pNext;
  for(p=pScan; p!=pLast; p=pNext){
    pNext = p->pNextScan;
    idxConstraintFree(p->pOrder);
    idxConstraintFree(p->pEq);
    idxConstraintFree(p->pRange);
    capdb_free(p);
  }
}

/*
** Free all elements of the linked list starting from pStatement up 
** until pLast (pLast is not freed).
*/
static void idxStatementFree(IdxStatement *pStatement, IdxStatement *pLast){
  IdxStatement *p;
  IdxStatement *pNext;
  for(p=pStatement; p!=pLast; p=pNext){
    pNext = p->pNext;
    capdb_free(p->zEQP);
    capdb_free(p->zIdx);
    capdb_free(p);
  }
}

/*
** Free the linked list of IdxTable objects starting at pTab.
*/
static void idxTableFree(IdxTable *pTab){
  IdxTable *pIter;
  IdxTable *pNext;
  for(pIter=pTab; pIter; pIter=pNext){
    pNext = pIter->pNext;
    capdb_free(pIter);
  }
}

/*
** Free the linked list of IdxWrite objects starting at pTab.
*/
static void idxWriteFree(IdxWrite *pTab){
  IdxWrite *pIter;
  IdxWrite *pNext;
  for(pIter=pTab; pIter; pIter=pNext){
    pNext = pIter->pNext;
    capdb_free(pIter);
  }
}



/*
** This function is called after candidate indexes have been created. It
** runs all the queries to see which indexes they prefer, and populates
** IdxStatement.zIdx and IdxStatement.zEQP with the results.
*/
static int idxFindIndexes(
  capdbexpert *p,
  char **pzErr                         /* OUT: Error message (capdb_malloc) */
){
  IdxStatement *pStmt;
  capdb *dbm = p->dbm;
  int rc = CAPDB_OK;

  IdxHash hIdx;
  idxHashInit(&hIdx);

  for(pStmt=p->pStatement; rc==CAPDB_OK && pStmt; pStmt=pStmt->pNext){
    IdxHashEntry *pEntry;
    capdb_stmt *pExplain = 0;
    idxHashClear(&hIdx);
    rc = idxPrintfPrepareStmt(dbm, &pExplain, pzErr,
        "EXPLAIN QUERY PLAN %s", pStmt->zSql
    );
    while( rc==CAPDB_OK && capdb_step(pExplain)==CAPDB_ROW ){
      /* int iId = capdb_column_int(pExplain, 0); */
      /* int iParent = capdb_column_int(pExplain, 1); */
      /* int iNotUsed = capdb_column_int(pExplain, 2); */
      const char *zDetail = (const char*)capdb_column_text(pExplain, 3);
      int nDetail;
      int i;

      if( !zDetail ) continue;
      nDetail = STRLEN(zDetail);

      for(i=0; i<nDetail; i++){
        const char *zIdx = 0;
        if( i+13<nDetail && memcmp(&zDetail[i], " USING INDEX ", 13)==0 ){
          zIdx = &zDetail[i+13];
        }else if( i+22<nDetail 
            && memcmp(&zDetail[i], " USING COVERING INDEX ", 22)==0 
        ){
          zIdx = &zDetail[i+22];
        }
        if( zIdx ){
          const char *zSql;
          int nIdx = 0;
          while( zIdx[nIdx]!='\0' && (zIdx[nIdx]!=' ' || zIdx[nIdx+1]!='(') ){
            nIdx++;
          }
          zSql = idxHashSearch(&p->hIdx, zIdx, nIdx);
          if( zSql ){
            idxHashAdd(&rc, &hIdx, zSql, 0);
            if( rc ) goto find_indexes_out;
          }
          break;
        }
      }

      if( zDetail[0]!='-' ){
        pStmt->zEQP = idxAppendText(&rc, pStmt->zEQP, "%s\n", zDetail);
      }
    }

    for(pEntry=hIdx.pFirst; pEntry; pEntry=pEntry->pNext){
      pStmt->zIdx = idxAppendText(&rc, pStmt->zIdx, "%s;\n", pEntry->zKey);
    }

    idxFinalize(&rc, pExplain);
  }

 find_indexes_out:
  idxHashClear(&hIdx);
  return rc;
}

static int idxAuthCallback(
  void *pCtx,
  int eOp,
  const char *z3,
  const char *z4,
  const char *zDb,
  const char *zTrigger
){
  int rc = CAPDB_OK;
  (void)z4;
  (void)zTrigger;
  if( eOp==CAPDB_INSERT || eOp==CAPDB_UPDATE || eOp==CAPDB_DELETE ){
    if( capdb_stricmp(zDb, "main")==0 ){
      capdbexpert *p = (capdbexpert*)pCtx;
      IdxTable *pTab;
      for(pTab=p->pTable; pTab; pTab=pTab->pNext){
        if( 0==capdb_stricmp(z3, pTab->zName) ) break;
      }
      if( pTab ){
        IdxWrite *pWrite;
        for(pWrite=p->pWrite; pWrite; pWrite=pWrite->pNext){
          if( pWrite->pTab==pTab && pWrite->eOp==eOp ) break;
        }
        if( pWrite==0 ){
          pWrite = idxMalloc(&rc, sizeof(IdxWrite));
          if( rc==CAPDB_OK ){
            pWrite->pTab = pTab;
            pWrite->eOp = eOp;
            pWrite->pNext = p->pWrite;
            p->pWrite = pWrite;
          }
        }
      }
    }
  }
  return rc;
}

static int idxProcessOneTrigger(
  capdbexpert *p, 
  IdxWrite *pWrite, 
  char **pzErr
){
  static const char *zInt = UNIQUE_TABLE_NAME;
  static const char *zDrop = "DROP TABLE " UNIQUE_TABLE_NAME;
  IdxTable *pTab = pWrite->pTab;
  const char *zTab = pTab->zName;
  const char *zSql = 
    "SELECT 'CREATE TEMP' || substr(sql, 7) FROM sqlite_schema "
    "WHERE tbl_name = %Q AND type IN ('table', 'trigger') "
    "ORDER BY type;";
  capdb_stmt *pSelect = 0;
  int rc = CAPDB_OK;
  char *zWrite = 0;

  /* Create the table and its triggers in the temp schema */
  rc = idxPrintfPrepareStmt(p->db, &pSelect, pzErr, zSql, zTab, zTab);
  while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pSelect) ){
    const char *zCreate = (const char*)capdb_column_text(pSelect, 0);
    if( zCreate==0 ) continue;
    rc = capdb_exec(p->dbv, zCreate, 0, 0, pzErr);
  }
  idxFinalize(&rc, pSelect);

  /* Rename the table in the temp schema to zInt */
  if( rc==CAPDB_OK ){
    char *z = capdb_mprintf("ALTER TABLE temp.%Q RENAME TO %Q", zTab, zInt);
    if( z==0 ){
      rc = CAPDB_NOMEM;
    }else{
      rc = capdb_exec(p->dbv, z, 0, 0, pzErr);
      capdb_free(z);
    }
  }

  switch( pWrite->eOp ){
    case CAPDB_INSERT: {
      int i;
      zWrite = idxAppendText(&rc, zWrite, "INSERT INTO %Q VALUES(", zInt);
      for(i=0; i<pTab->nCol; i++){
        zWrite = idxAppendText(&rc, zWrite, "%s?", i==0 ? "" : ", ");
      }
      zWrite = idxAppendText(&rc, zWrite, ")");
      break;
    }
    case CAPDB_UPDATE: {
      int i;
      zWrite = idxAppendText(&rc, zWrite, "UPDATE %Q SET ", zInt);
      for(i=0; i<pTab->nCol; i++){
        zWrite = idxAppendText(&rc, zWrite, "%s%Q=?", i==0 ? "" : ", ", 
            pTab->aCol[i].zName
        );
      }
      break;
    }
    default: {
      assert( pWrite->eOp==CAPDB_DELETE );
      if( rc==CAPDB_OK ){
        zWrite = capdb_mprintf("DELETE FROM %Q", zInt);
        if( zWrite==0 ) rc = CAPDB_NOMEM;
      }
    }
  }

  if( rc==CAPDB_OK ){
    capdb_stmt *pX = 0;
    rc = capdb_prepare_v2(p->dbv, zWrite, -1, &pX, 0);
    idxFinalize(&rc, pX);
    if( rc!=CAPDB_OK ){
      idxDatabaseError(p->dbv, pzErr);
    }
  }
  capdb_free(zWrite);

  if( rc==CAPDB_OK ){
    rc = capdb_exec(p->dbv, zDrop, 0, 0, pzErr);
  }

  return rc;
}

static int idxProcessTriggers(capdbexpert *p, char **pzErr){
  int rc = CAPDB_OK;
  IdxWrite *pEnd = 0;
  IdxWrite *pFirst = p->pWrite;

  while( rc==CAPDB_OK && pFirst!=pEnd ){
    IdxWrite *pIter;
    for(pIter=pFirst; rc==CAPDB_OK && pIter!=pEnd; pIter=pIter->pNext){
      rc = idxProcessOneTrigger(p, pIter, pzErr);
    }
    pEnd = pFirst;
    pFirst = p->pWrite;
  }

  return rc;
}

/*
** This function tests if the schema of the main database of database handle
** db contains an object named zTab. Assuming no error occurs, output parameter
** (*pbContains) is set to true if zTab exists, or false if it does not.
**
** Or, if an error occurs, an SQLite error code is returned. The final value
** of (*pbContains) is undefined in this case.
*/
static int expertDbContainsObject(
  capdb *db, 
  const char *zTab, 
  int *pbContains                 /* OUT: True if object exists */
){
  const char *zSql = "SELECT 1 FROM sqlite_schema WHERE name = ?";
  capdb_stmt *pSql = 0;
  int rc = CAPDB_OK;
  int ret = 0;

  rc = capdb_prepare_v2(db, zSql, -1, &pSql, 0);
  if( rc==CAPDB_OK ){
    capdb_bind_text(pSql, 1, zTab, -1, CAPDB_STATIC);
    if( CAPDB_ROW==capdb_step(pSql) ){
      ret = 1;
    }
    rc = capdb_finalize(pSql);
  }

  *pbContains = ret;
  return rc;
}

/*
** Execute SQL command zSql using database handle db. If no error occurs,
** set (*pzErr) to NULL and return CAPDB_OK. 
**
** If an error does occur, return an SQLite error code and set (*pzErr) to
** point to a buffer containing an English language error message. Except,
** if the error message begins with "no such module:", then ignore the
** error and return as if the SQL statement had succeeded.
**
** This is used to copy as much of the database schema as possible while 
** ignoring any errors related to missing virtual table modules.
*/
static int expertSchemaSql(capdb *db, const char *zSql, char **pzErr){
  int rc = CAPDB_OK;
  char *zErr = 0;

  rc = capdb_exec(db, zSql, 0, 0, &zErr);
  if( rc!=CAPDB_OK && zErr ){
    int nErr = STRLEN(zErr);
    if( nErr>=15 && memcmp(zErr, "no such module:", 15)==0 ){
      capdb_free(zErr);
      rc = CAPDB_OK;
      zErr = 0;
    }
  }

  *pzErr = zErr;
  return rc;
}

static int idxCreateVtabSchema(capdbexpert *p, char **pzErrmsg){
  int rc = idxRegisterVtab(p);
  capdb_stmt *pSchema = 0;

  /* For each table in the main db schema:
  **
  **   1) Add an entry to the p->pTable list, and
  **   2) Create the equivalent virtual table in dbv.
  */
  rc = idxPrepareStmt(p->db, &pSchema, pzErrmsg,
      "SELECT type, name, sql, 1, "
      "       substr(sql,1,14)=='create virtual' COLLATE nocase "
      "FROM sqlite_schema "
      "WHERE type IN ('table','view') AND "
      "      substr(name,1,7)!='sqlite_' COLLATE nocase "
      " UNION ALL "
      "SELECT type, name, sql, 2, 0 FROM sqlite_schema "
      "WHERE type = 'trigger'"
      "  AND tbl_name IN(SELECT name FROM sqlite_schema WHERE type = 'view') "
      "ORDER BY 4, 5 DESC, 1"
  );
  while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pSchema) ){
    const char *zType = (const char*)capdb_column_text(pSchema, 0);
    const char *zName = (const char*)capdb_column_text(pSchema, 1);
    const char *zSql = (const char*)capdb_column_text(pSchema, 2);
    int bVirtual = capdb_column_int(pSchema, 4);
    int bExists = 0;

    if( zType==0 || zName==0 ) continue;
    rc = expertDbContainsObject(p->dbv, zName, &bExists);
    if( rc || bExists ) continue;

    if( zType[0]=='v' || zType[1]=='r' || bVirtual ){
      /* A view. Or a trigger on a view. */
      if( zSql ) rc = expertSchemaSql(p->dbv, zSql, pzErrmsg);
    }else{
      IdxTable *pTab;
      rc = idxGetTableInfo(p->db, zName, &pTab, pzErrmsg);
      if( rc==CAPDB_OK && ALWAYS(pTab!=0) ){
        int i;
        char *zInner = 0;
        char *zOuter = 0;
        pTab->pNext = p->pTable;
        p->pTable = pTab;

        /* The statement the vtab will pass to capdb_declare_vtab() */
        zInner = idxAppendText(&rc, 0, "CREATE TABLE x(");
        for(i=0; i<pTab->nCol; i++){
          zInner = idxAppendText(&rc, zInner, "%s%Q COLLATE %Q",
              (i==0 ? "" : ", "), pTab->aCol[i].zName, pTab->aCol[i].zColl
          );
        }
        zInner = idxAppendText(&rc, zInner, ")");

        /* The CVT statement to create the vtab */
        zOuter = idxAppendText(&rc, 0, 
            "CREATE VIRTUAL TABLE %Q USING expert(%Q)", zName, zInner
        );
        if( rc==CAPDB_OK ){
          rc = capdb_exec(p->dbv, zOuter, 0, 0, pzErrmsg);
        }
        capdb_free(zInner);
        capdb_free(zOuter);
      }
    }
  }
  idxFinalize(&rc, pSchema);
  return rc;
}

struct IdxSampleCtx {
  int iTarget;
  double target;                  /* Target nRet/nRow value */
  double nRow;                    /* Number of rows seen */
  double nRet;                    /* Number of rows returned */
};

static void idxSampleFunc(
  capdb_context *pCtx,
  int argc,
  capdb_value **argv
){
  struct IdxSampleCtx *p = (struct IdxSampleCtx*)capdb_user_data(pCtx);
  int bRet;

  (void)argv;
  assert( argc==0 );
  if( p->nRow==0.0 ){
    bRet = 1;
  }else{
    bRet = (p->nRet / p->nRow) <= p->target;
    if( bRet==0 ){
      unsigned short rnd;
      capdb_randomness(2, (void*)&rnd);
      bRet = ((int)rnd % 100) <= p->iTarget;
    }
  }

  capdb_result_int(pCtx, bRet);
  p->nRow += 1.0;
  p->nRet += (double)bRet;
}

struct IdxRemCtx {
  int nSlot;
  struct IdxRemSlot {
    int eType;                    /* CAPDB_NULL, INTEGER, REAL, TEXT, BLOB */
    i64 iVal;                     /* CAPDB_INTEGER value */
    double rVal;                  /* CAPDB_FLOAT value */
    i64 nByte;                    /* Bytes of space allocated at z */
    i64 n;                        /* Size of buffer z */
    char *z;                      /* CAPDB_TEXT/BLOB value */
  } aSlot[1];
};

/*
** Implementation of scalar function sqlite_expert_rem().
*/
static void idxRemFunc(
  capdb_context *pCtx,
  int argc,
  capdb_value **argv
){
  struct IdxRemCtx *p = (struct IdxRemCtx*)capdb_user_data(pCtx);
  struct IdxRemSlot *pSlot;
  int iSlot;
  assert( argc==2 );

  iSlot = capdb_value_int(argv[0]);
  assert( iSlot<p->nSlot );
  pSlot = &p->aSlot[iSlot];

  switch( pSlot->eType ){
    case CAPDB_NULL:
      /* no-op */
      break;

    case CAPDB_INTEGER:
      capdb_result_int64(pCtx, pSlot->iVal);
      break;

    case CAPDB_FLOAT:
      capdb_result_double(pCtx, pSlot->rVal);
      break;

    case CAPDB_BLOB:
      assert( pSlot->n <= 0x7fffffff );
      capdb_result_blob(pCtx, pSlot->z, (int)pSlot->n, CAPDB_TRANSIENT);
      break;

    case CAPDB_TEXT:
      assert( pSlot->n <= 0x7fffffff );
      capdb_result_text(pCtx, pSlot->z, (int)pSlot->n, CAPDB_TRANSIENT);
      break;
  }

  pSlot->eType = capdb_value_type(argv[1]);
  switch( pSlot->eType ){
    case CAPDB_NULL:
      /* no-op */
      break;

    case CAPDB_INTEGER:
      pSlot->iVal = capdb_value_int64(argv[1]);
      break;

    case CAPDB_FLOAT:
      pSlot->rVal = capdb_value_double(argv[1]);
      break;

    case CAPDB_BLOB:
    case CAPDB_TEXT: {
      i64 nByte = capdb_value_bytes(argv[1]);
      const void *pData = 0;
      if( nByte>pSlot->nByte ){
        char *zNew = (char*)capdb_realloc64(pSlot->z, nByte*2);
        if( zNew==0 ){
          capdb_result_error_nomem(pCtx);
          return;
        }
        pSlot->nByte = nByte*2;
        pSlot->z = zNew;
      }
      pSlot->n = nByte;
      if( pSlot->eType==CAPDB_BLOB ){
        pData = capdb_value_blob(argv[1]);
        if( pData ) memcpy(pSlot->z, pData, nByte);
      }else{
        pData = capdb_value_text(argv[1]);
        memcpy(pSlot->z, pData, nByte);
      }
      break;
    }
  }
}

static int idxLargestIndex(capdb *db, int *pnMax, char **pzErr){
  int rc = CAPDB_OK;
  const char *zMax = 
    "SELECT max(i.seqno) FROM "
    "  sqlite_schema AS s, "
    "  pragma_index_list(s.name) AS l, "
    "  pragma_index_info(l.name) AS i "
    "WHERE s.type = 'table'";
  capdb_stmt *pMax = 0;

  *pnMax = 0;
  rc = idxPrepareStmt(db, &pMax, pzErr, zMax);
  if( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pMax) ){
    *pnMax = capdb_column_int(pMax, 0) + 1;
  }
  idxFinalize(&rc, pMax);

  return rc;
}

static int idxPopulateOneStat1(
  capdbexpert *p,
  capdb_stmt *pIndexXInfo,
  capdb_stmt *pWriteStat,
  const char *zTab,
  const char *zIdx,
  char **pzErr
){
  char *zCols = 0;
  char *zOrder = 0;
  char *zQuery = 0;
  int nCol = 0;
  int i;
  capdb_stmt *pQuery = 0;
  i64 *aStat = 0;
  int rc = CAPDB_OK;

  assert( p->iSample>0 );

  /* Formulate the query text */
  capdb_bind_text(pIndexXInfo, 1, zIdx, -1, CAPDB_STATIC);
  while( CAPDB_OK==rc && CAPDB_ROW==capdb_step(pIndexXInfo) ){
    const char *zComma = zCols==0 ? "" : ", ";
    const char *zName = (const char*)capdb_column_text(pIndexXInfo, 0);
    const char *zColl = (const char*)capdb_column_text(pIndexXInfo, 1);
    if( zName==0 ){
      /* This index contains an expression. Ignore it. */
      capdb_free(zCols);
      capdb_free(zOrder);
      return capdb_reset(pIndexXInfo);
    }
    zCols = idxAppendText(&rc, zCols, 
        "%sx.%Q IS sqlite_expert_rem(%d, x.%Q) COLLATE %Q", 
        zComma, zName, nCol, zName, zColl
    );
    zOrder = idxAppendText(&rc, zOrder, "%s%d", zComma, ++nCol);
  }
  capdb_reset(pIndexXInfo);
  if( rc==CAPDB_OK ){
    if( p->iSample==100 ){
      zQuery = capdb_mprintf(
          "SELECT %s FROM %Q x ORDER BY %s", zCols, zTab, zOrder
      );
    }else{
      zQuery = capdb_mprintf(
          "SELECT %s FROM temp."UNIQUE_TABLE_NAME" x ORDER BY %s", zCols, zOrder
      );
    }
  }
  capdb_free(zCols);
  capdb_free(zOrder);

  /* Formulate the query text */
  if( rc==CAPDB_OK ){
    capdb *dbrem = (p->iSample==100 ? p->db : p->dbv);
    rc = idxPrepareStmt(dbrem, &pQuery, pzErr, zQuery);
  }
  capdb_free(zQuery);

  if( rc==CAPDB_OK ){
    aStat = (i64*)idxMalloc(&rc, sizeof(i64)*(nCol+1));
  }
  if( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pQuery) ){
    IdxHashEntry *pEntry;
    char *zStat = 0;
    for(i=0; i<=nCol; i++) aStat[i] = 1;
    while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pQuery) ){
      aStat[0]++;
      for(i=0; i<nCol; i++){
        if( capdb_column_int(pQuery, i)==0 ) break;
      }
      for(/*no-op*/; i<nCol; i++){
        aStat[i+1]++;
      }
    }

    if( rc==CAPDB_OK ){
      i64 s0 = aStat[0];
      zStat = capdb_mprintf("%lld", s0);
      if( zStat==0 ) rc = CAPDB_NOMEM;
      for(i=1; rc==CAPDB_OK && i<=nCol; i++){
        zStat = idxAppendText(&rc, zStat, " %lld", (s0+aStat[i]/2) / aStat[i]);
      }
    }

    if( rc==CAPDB_OK ){
      capdb_bind_text(pWriteStat, 1, zTab, -1, CAPDB_STATIC);
      capdb_bind_text(pWriteStat, 2, zIdx, -1, CAPDB_STATIC);
      capdb_bind_text(pWriteStat, 3, zStat, -1, CAPDB_STATIC);
      capdb_step(pWriteStat);
      rc = capdb_reset(pWriteStat);
    }

    pEntry = idxHashFind(&p->hIdx, zIdx, STRLEN(zIdx));
    if( pEntry ){
      assert( pEntry->zVal2==0 );
      pEntry->zVal2 = zStat;
    }else{
      capdb_free(zStat);
    }
  }
  capdb_free(aStat);
  idxFinalize(&rc, pQuery);

  return rc;
}

static int idxBuildSampleTable(capdbexpert *p, const char *zTab){
  int rc;
  char *zSql;

  rc = capdb_exec(p->dbv,"DROP TABLE IF EXISTS temp."UNIQUE_TABLE_NAME,0,0,0);
  if( rc!=CAPDB_OK ) return rc;

  zSql = capdb_mprintf(
      "CREATE TABLE temp." UNIQUE_TABLE_NAME " AS SELECT * FROM %Q", zTab
  );
  if( zSql==0 ) return CAPDB_NOMEM;
  rc = capdb_exec(p->dbv, zSql, 0, 0, 0);
  capdb_free(zSql);

  return rc;
}

/*
** This function is called as part of capdb_expert_analyze(). Candidate
** indexes have already been created in database capdbexpert.dbm, this
** function populates sqlite_stat1 table in the same database.
**
** The stat1 data is generated by querying the 
*/
static int idxPopulateStat1(capdbexpert *p, char **pzErr){
  int rc = CAPDB_OK;
  int nMax =0;
  struct IdxRemCtx *pCtx = 0;
  struct IdxSampleCtx samplectx; 
  int i;
  i64 iPrev = -100000;
  capdb_stmt *pAllIndex = 0;
  capdb_stmt *pIndexXInfo = 0;
  capdb_stmt *pWrite = 0;

  const char *zAllIndex =
    "SELECT s.rowid, s.name, l.name FROM "
    "  sqlite_schema AS s, "
    "  pragma_index_list(s.name) AS l "
    "WHERE s.type = 'table'";
  const char *zIndexXInfo = 
    "SELECT name, coll FROM pragma_index_xinfo(?) WHERE key";
  const char *zWrite = "INSERT INTO sqlite_stat1 VALUES(?, ?, ?)";

  /* If iSample==0, no sqlite_stat1 data is required. */
  if( p->iSample==0 ) return CAPDB_OK;

  rc = idxLargestIndex(p->dbm, &nMax, pzErr);
  if( nMax<=0 || rc!=CAPDB_OK ) return rc;

  rc = capdb_exec(p->dbm, "ANALYZE; PRAGMA writable_schema=1", 0, 0, 0);

  if( rc==CAPDB_OK ){
    i64 nByte = sizeof(struct IdxRemCtx) + (sizeof(struct IdxRemSlot) * nMax);
    pCtx = (struct IdxRemCtx*)idxMalloc(&rc, nByte);
  }

  if( rc==CAPDB_OK ){
    capdb *dbrem = (p->iSample==100 ? p->db : p->dbv);
    rc = capdb_create_function(dbrem, "sqlite_expert_rem", 
        2, CAPDB_UTF8, (void*)pCtx, idxRemFunc, 0, 0
    );
  }
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(p->db, "sqlite_expert_sample", 
        0, CAPDB_UTF8, (void*)&samplectx, idxSampleFunc, 0, 0
    );
  }

  if( rc==CAPDB_OK ){
    pCtx->nSlot = (i64)nMax+1;
    rc = idxPrepareStmt(p->dbm, &pAllIndex, pzErr, zAllIndex);
  }
  if( rc==CAPDB_OK ){
    rc = idxPrepareStmt(p->dbm, &pIndexXInfo, pzErr, zIndexXInfo);
  }
  if( rc==CAPDB_OK ){
    rc = idxPrepareStmt(p->dbm, &pWrite, pzErr, zWrite);
  }

  while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pAllIndex) ){
    i64 iRowid = capdb_column_int64(pAllIndex, 0);
    const char *zTab = (const char*)capdb_column_text(pAllIndex, 1);
    const char *zIdx = (const char*)capdb_column_text(pAllIndex, 2);
    if( zTab==0 || zIdx==0 ) continue;
    if( p->iSample<100 && iPrev!=iRowid ){
      samplectx.target = (double)p->iSample / 100.0;
      samplectx.iTarget = p->iSample;
      samplectx.nRow = 0.0;
      samplectx.nRet = 0.0;
      rc = idxBuildSampleTable(p, zTab);
      if( rc!=CAPDB_OK ) break;
    }
    rc = idxPopulateOneStat1(p, pIndexXInfo, pWrite, zTab, zIdx, pzErr);
    iPrev = iRowid;
  }
  if( rc==CAPDB_OK && p->iSample<100 ){
    rc = capdb_exec(p->dbv, 
        "DROP TABLE IF EXISTS temp." UNIQUE_TABLE_NAME, 0,0,0
    );
  }

  idxFinalize(&rc, pAllIndex);
  idxFinalize(&rc, pIndexXInfo);
  idxFinalize(&rc, pWrite);

  if( pCtx ){
    for(i=0; i<pCtx->nSlot; i++){
      capdb_free(pCtx->aSlot[i].z);
    }
    capdb_free(pCtx);
  }

  if( rc==CAPDB_OK ){
    rc = capdb_exec(p->dbm, "ANALYZE sqlite_schema", 0, 0, 0);
  }

  capdb_create_function(p->db, "sqlite_expert_rem", 2, CAPDB_UTF8, 0,0,0,0);
  capdb_create_function(p->db, "sqlite_expert_sample", 0,CAPDB_UTF8,0,0,0,0);

  capdb_exec(p->db, "DROP TABLE IF EXISTS temp."UNIQUE_TABLE_NAME,0,0,0);
  return rc;
}

/*
** Define and possibly pretend to use a useless collation sequence.
** This pretense allows expert to accept SQL using custom collations.
*/
int dummyCompare(void *up1, int up2, const void *up3, int up4, const void *up5){
  (void)up1;
  (void)up2;
  (void)up3;
  (void)up4;
  (void)up5;
  assert(0); /* VDBE should never be run. */
  return 0;
}
/* And a callback to register above upon actual need */
void useDummyCS(void *up1, capdb *db, int etr, const char *zName){
  (void)up1;
  capdb_create_collation_v2(db, zName, etr, 0, dummyCompare, 0);
}

#if !defined(CAPDB_OMIT_SCHEMA_PRAGMAS) \
  && !defined(CAPDB_OMIT_INTROSPECTION_PRAGMAS)
/*
** dummy functions for no-op implementation of UDFs during expert's work
*/
void dummyUDF(capdb_context *up1, int up2, capdb_value **up3){
  (void)up1;
  (void)up2;
  (void)up3;
  assert(0); /* VDBE should never be run. */
}
void dummyUDFvalue(capdb_context *up1){
  (void)up1;
  assert(0); /* VDBE should never be run. */
}

/*
** Register UDFs from user database with another.
*/
int registerUDFs(capdb *dbSrc, capdb *dbDst){
  capdb_stmt *pStmt;
  int rc = capdb_prepare_v2(dbSrc,
            "SELECT name,type,enc,narg,flags "
            "FROM pragma_function_list() "
            "WHERE builtin==0", -1, &pStmt, 0);
  if( rc==CAPDB_OK ){
    while( CAPDB_ROW==(rc = capdb_step(pStmt)) ){
      int nargs = capdb_column_int(pStmt,3);
      int flags = capdb_column_int(pStmt,4);
      const char *name = (char*)capdb_column_text(pStmt,0);
      const char *type = (char*)capdb_column_text(pStmt,1);
      const char *enc = (char*)capdb_column_text(pStmt,2);
      if( name==0 || type==0 || enc==0 ){
        /* no-op.  Only happens on OOM */
      }else{
        int ienc = CAPDB_UTF8;
        int rcf = CAPDB_ERROR;
        if( strcmp(enc,"utf16le")==0 ) ienc = CAPDB_UTF16LE;
        else if( strcmp(enc,"utf16be")==0 ) ienc = CAPDB_UTF16BE;
        ienc |= (flags & (CAPDB_DETERMINISTIC|CAPDB_DIRECTONLY));
        if( strcmp(type,"w")==0 ){
          rcf = capdb_create_window_function(dbDst,name,nargs,ienc,0,
                                               dummyUDF,dummyUDFvalue,0,0,0);
        }else if( strcmp(type,"a")==0 ){
          rcf = capdb_create_function(dbDst,name,nargs,ienc,0,
                                        0,dummyUDF,dummyUDFvalue);
        }else if( strcmp(type,"s")==0 ){
          rcf = capdb_create_function(dbDst,name,nargs,ienc,0,
                                        dummyUDF,0,0);
        }
        if( rcf!=CAPDB_OK ){
          rc = rcf;
          break;
        }
      }
    }
    capdb_finalize(pStmt);
    if( rc==CAPDB_DONE ) rc = CAPDB_OK;
  }
  return rc;
}
#endif

/*
** Allocate a new capdbexpert object.
*/
capdbexpert *capdb_expert_new(capdb *db, char **pzErrmsg){
  int rc = CAPDB_OK;
  capdbexpert *pNew;

  pNew = (capdbexpert*)idxMalloc(&rc, sizeof(capdbexpert));

  /* Open two in-memory databases to work with. The "vtab database" (dbv)
  ** will contain a virtual table corresponding to each real table in
  ** the user database schema, and a copy of each view. It is used to
  ** collect information regarding the WHERE, ORDER BY and other clauses
  ** of the user's query.
  */
  if( rc==CAPDB_OK ){
    pNew->db = db;
    pNew->iSample = 100;
    rc = capdb_open(":memory:", &pNew->dbv);
  }
  if( rc==CAPDB_OK ){
    rc = capdb_open(":memory:", &pNew->dbm);
    if( rc==CAPDB_OK ){
      capdb_db_config(pNew->dbm, CAPDB_DBCONFIG_TRIGGER_EQP, 1, (int*)0);
    }
  }

  /* Allow custom collations to be dealt with through prepare. */
  if( rc==CAPDB_OK ) rc = capdb_collation_needed(pNew->dbm,0,useDummyCS);
  if( rc==CAPDB_OK ) rc = capdb_collation_needed(pNew->dbv,0,useDummyCS);

#if !defined(CAPDB_OMIT_SCHEMA_PRAGMAS) \
  && !defined(CAPDB_OMIT_INTROSPECTION_PRAGMAS)
  /* Register UDFs from database [db] with [dbm] and [dbv]. */
  if( rc==CAPDB_OK ){
    rc = registerUDFs(pNew->db, pNew->dbm);
  }
  if( rc==CAPDB_OK ){
    rc = registerUDFs(pNew->db, pNew->dbv);
  }
#endif

  /* Copy the entire schema of database [db] into [dbm]. */
  if( rc==CAPDB_OK ){
    capdb_stmt *pSql = 0;
    rc = idxPrintfPrepareStmt(pNew->db, &pSql, pzErrmsg, 
        "SELECT sql, name, substr(sql,1,14)=='create virtual' COLLATE nocase"
        " FROM sqlite_schema WHERE substr(name,1,7)!='sqlite_' COLLATE nocase"
        " ORDER BY 3 DESC, rowid"
    );
    while( rc==CAPDB_OK && CAPDB_ROW==capdb_step(pSql) ){
      const char *zSql = (const char*)capdb_column_text(pSql, 0);
      const char *zName = (const char*)capdb_column_text(pSql, 1);
      int bExists = 0;
      rc = expertDbContainsObject(pNew->dbm, zName, &bExists);
      if( rc==CAPDB_OK && zSql && bExists==0 ){
        rc = expertSchemaSql(pNew->dbm, zSql, pzErrmsg);
      }
    }
    idxFinalize(&rc, pSql);
  }

  /* Create the vtab schema */
  if( rc==CAPDB_OK ){
    rc = idxCreateVtabSchema(pNew, pzErrmsg);
  }

  /* Register the auth callback with dbv */
  if( rc==CAPDB_OK ){
    capdb_set_authorizer(pNew->dbv, idxAuthCallback, (void*)pNew);
  }

  /* If an error has occurred, free the new object and return NULL. Otherwise,
  ** return the new capdbexpert handle.  */
  if( rc!=CAPDB_OK ){
    capdb_expert_destroy(pNew);
    pNew = 0;
  }
  return pNew;
}

/*
** Configure an capdbexpert object.
*/
int capdb_expert_config(capdbexpert *p, int op, ...){
  int rc = CAPDB_OK;
  va_list ap;
  va_start(ap, op);
  switch( op ){
    case EXPERT_CONFIG_SAMPLE: {
      int iVal = va_arg(ap, int);
      if( iVal<0 ) iVal = 0;
      if( iVal>100 ) iVal = 100;
      p->iSample = iVal;
      break;
    }
    default:
      rc = CAPDB_NOTFOUND;
      break;
  }

  va_end(ap);
  return rc;
}

/*
** Add an SQL statement to the analysis.
*/
int capdb_expert_sql(
  capdbexpert *p,               /* From capdb_expert_new() */
  const char *zSql,               /* SQL statement to add */
  char **pzErr                    /* OUT: Error message (if any) */
){
  IdxScan *pScanOrig = p->pScan;
  IdxStatement *pStmtOrig = p->pStatement;
  int rc = CAPDB_OK;
  const char *zStmt = zSql;

  if( p->bRun ) return CAPDB_MISUSE;

  while( rc==CAPDB_OK && zStmt && zStmt[0] ){
    capdb_stmt *pStmt = 0;
    /* Ensure that the provided statement compiles against user's DB. */
    rc = idxPrepareStmt(p->db, &pStmt, pzErr, zStmt);
    if( rc!=CAPDB_OK ) break;
    capdb_finalize(pStmt);
    rc = capdb_prepare_v2(p->dbv, zStmt, -1, &pStmt, &zStmt);
    if( rc==CAPDB_OK ){
      if( pStmt ){
        IdxStatement *pNew;
        const char *z = capdb_sql(pStmt);
        i64 n = STRLEN(z);
        pNew = (IdxStatement*)idxMalloc(&rc, sizeof(IdxStatement) + n+1);
        if( rc==CAPDB_OK ){
          pNew->zSql = (char*)&pNew[1];
          memcpy(pNew->zSql, z, n+1);
          pNew->pNext = p->pStatement;
          if( p->pStatement ) pNew->iId = p->pStatement->iId+1;
          p->pStatement = pNew;
        }
        capdb_finalize(pStmt);
      }
    }else{
      idxDatabaseError(p->dbv, pzErr);
    }
  }

  if( rc!=CAPDB_OK ){
    idxScanFree(p->pScan, pScanOrig);
    idxStatementFree(p->pStatement, pStmtOrig);
    p->pScan = pScanOrig;
    p->pStatement = pStmtOrig;
  }

  return rc;
}

int capdb_expert_analyze(capdbexpert *p, char **pzErr){
  int rc;
  IdxHashEntry *pEntry;

  /* Do trigger processing to collect any extra IdxScan structures */
  rc = idxProcessTriggers(p, pzErr);

  /* Create candidate indexes within the in-memory database file */
  if( rc==CAPDB_OK ){
    rc = idxCreateCandidates(p);
  }else if ( rc==CAPDB_BUSY_TIMEOUT ){
    if( pzErr )
      *pzErr = capdb_mprintf("Cannot find a unique index name to propose.");
    return rc;
  }

  /* Generate the stat1 data */
  if( rc==CAPDB_OK ){
    rc = idxPopulateStat1(p, pzErr);
  }

  /* Formulate the EXPERT_REPORT_CANDIDATES text */
  for(pEntry=p->hIdx.pFirst; pEntry; pEntry=pEntry->pNext){
    p->zCandidates = idxAppendText(&rc, p->zCandidates, 
        "%s;%s%s\n", pEntry->zVal, 
        pEntry->zVal2 ? " -- stat1: " : "", pEntry->zVal2
    );
  }

  /* Figure out which of the candidate indexes are preferred by the query
  ** planner and report the results to the user.  */
  if( rc==CAPDB_OK ){
    rc = idxFindIndexes(p, pzErr);
  }

  if( rc==CAPDB_OK ){
    p->bRun = 1;
  }
  return rc;
}

/*
** Return the total number of statements that have been added to this
** capdbexpert using capdb_expert_sql().
*/
int capdb_expert_count(capdbexpert *p){
  int nRet = 0;
  if( p->pStatement ) nRet = p->pStatement->iId+1;
  return nRet;
}

/*
** Return a component of the report.
*/
const char *capdb_expert_report(capdbexpert *p, int iStmt, int eReport){
  const char *zRet = 0;
  IdxStatement *pStmt;

  if( p->bRun==0 ) return 0;
  for(pStmt=p->pStatement; pStmt && pStmt->iId!=iStmt; pStmt=pStmt->pNext);
  switch( eReport ){
    case EXPERT_REPORT_SQL:
      if( pStmt ) zRet = pStmt->zSql;
      break;
    case EXPERT_REPORT_INDEXES:
      if( pStmt ) zRet = pStmt->zIdx;
      break;
    case EXPERT_REPORT_PLAN:
      if( pStmt ) zRet = pStmt->zEQP;
      break;
    case EXPERT_REPORT_CANDIDATES:
      zRet = p->zCandidates;
      break;
  }
  return zRet;
}

/*
** Free an capdbexpert object.
*/
void capdb_expert_destroy(capdbexpert *p){
  if( p ){
    capdb_close(p->dbm);
    capdb_close(p->dbv);
    idxScanFree(p->pScan, 0);
    idxStatementFree(p->pStatement, 0);
    idxTableFree(p->pTable);
    idxWriteFree(p->pWrite);
    idxHashClear(&p->hIdx);
    capdb_free(p->zCandidates);
    capdb_free(p);
  }
}

#endif /* ifndef CAPDB_OMIT_VIRTUALTABLE */
