/*
** 2015 May 08
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
** This is an SQLite virtual table module implementing direct access to an
** existing FTS5 index. The module may create several different types of 
** tables:
**
** col:
**     CREATE TABLE vocab(term, col, doc, cnt, PRIMARY KEY(term, col));
**
**   One row for each term/column combination. The value of $doc is set to
**   the number of fts5 rows that contain at least one instance of term
**   $term within column $col. Field $cnt is set to the total number of 
**   instances of term $term in column $col (in any row of the fts5 table). 
**
** row:
**     CREATE TABLE vocab(term, doc, cnt, PRIMARY KEY(term));
**
**   One row for each term in the database. The value of $doc is set to
**   the number of fts5 rows that contain at least one instance of term
**   $term. Field $cnt is set to the total number of instances of term 
**   $term in the database.
**
** instance:
**     CREATE TABLE vocab(term, doc, col, offset, PRIMARY KEY(<all-fields>));
**
**   One row for each term instance in the database. 
*/


#include "fts5Int.h"


typedef struct Fts5VocabTable Fts5VocabTable;
typedef struct Fts5VocabCursor Fts5VocabCursor;

struct Fts5VocabTable {
  capdb_vtab base;
  char *zFts5Tbl;                 /* Name of fts5 table */
  char *zFts5Db;                  /* Db containing fts5 table */
  capdb *db;                    /* Database handle */
  Fts5Global *pGlobal;            /* FTS5 global object for this database */
  int eType;                      /* FTS5_VOCAB_COL, ROW or INSTANCE */
  unsigned bBusy;                 /* True if busy */
};

struct Fts5VocabCursor {
  capdb_vtab_cursor base;
  capdb_stmt *pStmt;            /* Statement holding lock on pIndex */
  Fts5Table *pFts5;               /* Associated FTS5 table */

  int bEof;                       /* True if this cursor is at EOF */
  Fts5IndexIter *pIter;           /* Term/rowid iterator object */
  void *pStruct;                  /* From capdbFts5StructureRef() */

  int nLeTerm;                    /* Size of zLeTerm in bytes */
  char *zLeTerm;                  /* (term <= $zLeTerm) paramater, or NULL */
  int colUsed;                    /* Copy of capdb_index_info.colUsed */

  /* These are used by 'col' tables only */
  int iCol;
  i64 *aCnt;
  i64 *aDoc;

  /* Output values used by all tables. */
  i64 rowid;                      /* This table's current rowid value */
  Fts5Buffer term;                /* Current value of 'term' column */

  /* Output values Used by 'instance' tables only */
  i64 iInstPos;
  int iInstOff;
};

#define FTS5_VOCAB_COL      0
#define FTS5_VOCAB_ROW      1
#define FTS5_VOCAB_INSTANCE 2

#define FTS5_VOCAB_COL_SCHEMA  "term, col, doc, cnt"
#define FTS5_VOCAB_ROW_SCHEMA  "term, doc, cnt"
#define FTS5_VOCAB_INST_SCHEMA "term, doc, col, offset"

/*
** Bits for the mask used as the idxNum value by xBestIndex/xFilter.
*/
#define FTS5_VOCAB_TERM_EQ 0x0100
#define FTS5_VOCAB_TERM_GE 0x0200
#define FTS5_VOCAB_TERM_LE 0x0400

#define FTS5_VOCAB_COLUSED_MASK 0xFF


/*
** Translate a string containing an fts5vocab table type to an 
** FTS5_VOCAB_XXX constant. If successful, set *peType to the output
** value and return CAPDB_OK. Otherwise, set *pzErr to an error message
** and return CAPDB_ERROR.
*/
static int fts5VocabTableType(const char *zType, char **pzErr, int *peType){
  int rc = CAPDB_OK;
  char *zCopy = capdbFts5Strndup(&rc, zType, -1);
  if( rc==CAPDB_OK ){
    capdbFts5Dequote(zCopy);
    if( capdb_stricmp(zCopy, "col")==0 ){
      *peType = FTS5_VOCAB_COL;
    }else

    if( capdb_stricmp(zCopy, "row")==0 ){
      *peType = FTS5_VOCAB_ROW;
    }else
    if( capdb_stricmp(zCopy, "instance")==0 ){
      *peType = FTS5_VOCAB_INSTANCE;
    }else
    {
      *pzErr = capdb_mprintf("fts5vocab: unknown table type: %Q", zCopy);
      rc = CAPDB_ERROR;
    }
    capdb_free(zCopy);
  }

  return rc;
}


/*
** The xDisconnect() virtual table method.
*/
static int fts5VocabDisconnectMethod(capdb_vtab *pVtab){
  Fts5VocabTable *pTab = (Fts5VocabTable*)pVtab;
  capdb_free(pTab);
  return CAPDB_OK;
}

/*
** The xDestroy() virtual table method.
*/
static int fts5VocabDestroyMethod(capdb_vtab *pVtab){
  Fts5VocabTable *pTab = (Fts5VocabTable*)pVtab;
  capdb_free(pTab);
  return CAPDB_OK;
}

/*
** This function is the implementation of both the xConnect and xCreate
** methods of the FTS3 virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fts5vocab")
**   argv[1]   -> database name
**   argv[2]   -> table name
**
** then:
**
**   argv[3]   -> name of fts5 table
**   argv[4]   -> type of fts5vocab table
**
** or, for tables in the TEMP schema only.
**
**   argv[3]   -> name of fts5 tables database
**   argv[4]   -> name of fts5 table
**   argv[5]   -> type of fts5vocab table
*/
static int fts5VocabInitVtab(
  capdb *db,                    /* The SQLite database connection */
  void *pAux,                     /* Pointer to Fts5Global object */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  capdb_vtab **ppVTab,          /* Write the resulting vtab structure here */
  char **pzErr                    /* Write any error message here */
){
  const char *azSchema[] = { 
    "CREATE TABlE vocab(" FTS5_VOCAB_COL_SCHEMA  ")", 
    "CREATE TABlE vocab(" FTS5_VOCAB_ROW_SCHEMA  ")",
    "CREATE TABlE vocab(" FTS5_VOCAB_INST_SCHEMA ")"
  };

  Fts5VocabTable *pRet = 0;
  int rc = CAPDB_OK;             /* Return code */
  int bDb;

  bDb = (argc==6 && strlen(argv[1])==4 && memcmp("temp", argv[1], 4)==0);

  if( argc!=5 && bDb==0 ){
    *pzErr = capdb_mprintf("wrong number of vtable arguments");
    rc = CAPDB_ERROR;
  }else{
    i64 nByte;                      /* Bytes of space to allocate */
    const char *zDb = bDb ? argv[3] : argv[1];
    const char *zTab = bDb ? argv[4] : argv[3];
    const char *zType = bDb ? argv[5] : argv[4];
    i64 nDb = strlen(zDb)+1; 
    i64 nTab = strlen(zTab)+1;
    int eType = 0;
    
    rc = fts5VocabTableType(zType, pzErr, &eType);
    if( rc==CAPDB_OK ){
      assert( eType>=0 && eType<ArraySize(azSchema) );
      rc = capdb_declare_vtab(db, azSchema[eType]);
    }

    nByte = sizeof(Fts5VocabTable) + nDb + nTab;
    pRet = capdbFts5MallocZero(&rc, nByte);
    if( pRet ){
      pRet->pGlobal = (Fts5Global*)pAux;
      pRet->eType = eType;
      pRet->db = db;
      pRet->zFts5Tbl = (char*)&pRet[1];
      pRet->zFts5Db = &pRet->zFts5Tbl[nTab];
      memcpy(pRet->zFts5Tbl, zTab, nTab);
      memcpy(pRet->zFts5Db, zDb, nDb);
      capdbFts5Dequote(pRet->zFts5Tbl);
      capdbFts5Dequote(pRet->zFts5Db);
    }
  }

  *ppVTab = (capdb_vtab*)pRet;
  return rc;
}


/*
** The xConnect() and xCreate() methods for the virtual table. All the
** work is done in function fts5VocabInitVtab().
*/
static int fts5VocabConnectMethod(
  capdb *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  capdb_vtab **ppVtab,          /* OUT: New capdb_vtab object */
  char **pzErr                    /* OUT: capdb_malloc'd error message */
){
  return fts5VocabInitVtab(db, pAux, argc, argv, ppVtab, pzErr);
}
static int fts5VocabCreateMethod(
  capdb *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  capdb_vtab **ppVtab,          /* OUT: New capdb_vtab object */
  char **pzErr                    /* OUT: capdb_malloc'd error message */
){
  return fts5VocabInitVtab(db, pAux, argc, argv, ppVtab, pzErr);
}

/* 
** Implementation of the xBestIndex method.
**
** Only constraints of the form:
**
**     term <= ?
**     term == ?
**     term >= ?
**
** are interpreted. Less-than and less-than-or-equal are treated 
** identically, as are greater-than and greater-than-or-equal.
*/
static int fts5VocabBestIndexMethod(
  capdb_vtab *pUnused,
  capdb_index_info *pInfo
){
  int i;
  int iTermEq = -1;
  int iTermGe = -1;
  int iTermLe = -1;
  int idxNum = (int)pInfo->colUsed;
  int nArg = 0;

  UNUSED_PARAM(pUnused);

  assert( (pInfo->colUsed & FTS5_VOCAB_COLUSED_MASK)==pInfo->colUsed );

  for(i=0; i<pInfo->nConstraint; i++){
    struct capdb_index_constraint *p = &pInfo->aConstraint[i];
    if( p->usable==0 ) continue;
    if( p->iColumn==0 ){          /* term column */
      if( p->op==CAPDB_INDEX_CONSTRAINT_EQ ) iTermEq = i;
      if( p->op==CAPDB_INDEX_CONSTRAINT_LE ) iTermLe = i;
      if( p->op==CAPDB_INDEX_CONSTRAINT_LT ) iTermLe = i;
      if( p->op==CAPDB_INDEX_CONSTRAINT_GE ) iTermGe = i;
      if( p->op==CAPDB_INDEX_CONSTRAINT_GT ) iTermGe = i;
    }
  }

  if( iTermEq>=0 ){
    idxNum |= FTS5_VOCAB_TERM_EQ;
    pInfo->aConstraintUsage[iTermEq].argvIndex = ++nArg;
    pInfo->estimatedCost = 100;
  }else{
    pInfo->estimatedCost = 1000000;
    if( iTermGe>=0 ){
      idxNum |= FTS5_VOCAB_TERM_GE;
      pInfo->aConstraintUsage[iTermGe].argvIndex = ++nArg;
      pInfo->estimatedCost = pInfo->estimatedCost / 2;
    }
    if( iTermLe>=0 ){
      idxNum |= FTS5_VOCAB_TERM_LE;
      pInfo->aConstraintUsage[iTermLe].argvIndex = ++nArg;
      pInfo->estimatedCost = pInfo->estimatedCost / 2;
    }
  }

  /* This virtual table always delivers results in ascending order of
  ** the "term" column (column 0). So if the user has requested this
  ** specifically - "ORDER BY term" or "ORDER BY term ASC" - set the
  ** capdb_index_info.orderByConsumed flag to tell the core the results
  ** are already in sorted order.  */
  if( pInfo->nOrderBy==1 
   && pInfo->aOrderBy[0].iColumn==0 
   && pInfo->aOrderBy[0].desc==0
  ){
    pInfo->orderByConsumed = 1;
  }

  pInfo->idxNum = idxNum;
  return CAPDB_OK;
}

/*
** Implementation of xOpen method.
*/
static int fts5VocabOpenMethod(
  capdb_vtab *pVTab, 
  capdb_vtab_cursor **ppCsr
){
  Fts5VocabTable *pTab = (Fts5VocabTable*)pVTab;
  Fts5Table *pFts5 = 0;
  Fts5VocabCursor *pCsr = 0;
  int rc = CAPDB_OK;
  capdb_stmt *pStmt = 0;
  char *zSql = 0;

  if( pTab->bBusy ){
    pVTab->zErrMsg = capdb_mprintf(
       "recursive definition for %s.%s", pTab->zFts5Db, pTab->zFts5Tbl
    );
    return CAPDB_ERROR;
  }
  zSql = capdbFts5Mprintf(&rc,
      "SELECT t.%Q FROM %Q.%Q AS t WHERE t.%Q MATCH '*id'",
      pTab->zFts5Tbl, pTab->zFts5Db, pTab->zFts5Tbl, pTab->zFts5Tbl
  );
  if( zSql ){
    rc = capdb_prepare_v2(pTab->db, zSql, -1, &pStmt, 0);
  }
  capdb_free(zSql);
  assert( rc==CAPDB_OK || pStmt==0 );
  if( rc==CAPDB_ERROR ) rc = CAPDB_OK;

  pTab->bBusy = 1;
  if( pStmt && capdb_step(pStmt)==CAPDB_ROW ){
    i64 iId = capdb_column_int64(pStmt, 0);
    pFts5 = capdbFts5TableFromCsrid(pTab->pGlobal, iId);
  }
  pTab->bBusy = 0;

  if( rc==CAPDB_OK ){
    if( pFts5==0 ){
      rc = capdb_finalize(pStmt);
      pStmt = 0;
      if( rc==CAPDB_OK ){
        pVTab->zErrMsg = capdb_mprintf(
            "no such fts5 table: %s.%s", pTab->zFts5Db, pTab->zFts5Tbl
        );
        rc = CAPDB_ERROR;
      }
    }else{
      rc = capdbFts5FlushToDisk(pFts5);
    }
  }

  if( rc==CAPDB_OK ){
    i64 nByte = pFts5->pConfig->nCol * sizeof(i64)*2 + sizeof(Fts5VocabCursor);
    pCsr = (Fts5VocabCursor*)capdbFts5MallocZero(&rc, nByte);
  }

  if( pCsr ){
    pCsr->pFts5 = pFts5;
    pCsr->pStmt = pStmt;
    pCsr->aCnt = (i64*)&pCsr[1];
    pCsr->aDoc = &pCsr->aCnt[pFts5->pConfig->nCol];
  }else{
    capdb_finalize(pStmt);
  }

  *ppCsr = (capdb_vtab_cursor*)pCsr;
  return rc;
}

/*
** Restore cursor pCsr to the state it was in immediately after being
** created by the xOpen() method.
*/
static void fts5VocabResetCursor(Fts5VocabCursor *pCsr){
  int nCol = pCsr->pFts5->pConfig->nCol;
  pCsr->rowid = 0;
  capdbFts5IterClose(pCsr->pIter);
  capdbFts5StructureRelease(pCsr->pStruct);
  pCsr->pStruct = 0;
  pCsr->pIter = 0;
  capdb_free(pCsr->zLeTerm);
  pCsr->nLeTerm = -1;
  pCsr->zLeTerm = 0;
  pCsr->bEof = 0;
  pCsr->iCol = 0;
  pCsr->iInstPos = 0;
  pCsr->iInstOff = 0;
  pCsr->colUsed = 0;
  memset(pCsr->aCnt, 0, sizeof(i64)*nCol);
  memset(pCsr->aDoc, 0, sizeof(i64)*nCol);
}

/*
** Close the cursor.  For additional information see the documentation
** on the xClose method of the virtual table interface.
*/
static int fts5VocabCloseMethod(capdb_vtab_cursor *pCursor){
  Fts5VocabCursor *pCsr = (Fts5VocabCursor*)pCursor;
  fts5VocabResetCursor(pCsr);
  capdbFts5BufferFree(&pCsr->term);
  capdb_finalize(pCsr->pStmt);
  capdb_free(pCsr);
  return CAPDB_OK;
}

static int fts5VocabInstanceNewTerm(Fts5VocabCursor *pCsr){
  int rc = CAPDB_OK;
  
  if( capdbFts5IterEof(pCsr->pIter) ){
    pCsr->bEof = 1;
  }else{
    const char *zTerm;
    int nTerm;
    zTerm = capdbFts5IterTerm(pCsr->pIter, &nTerm);
    if( pCsr->nLeTerm>=0 ){
      int nCmp = MIN(nTerm, pCsr->nLeTerm);
      int bCmp = memcmp(pCsr->zLeTerm, zTerm, nCmp);
      if( bCmp<0 || (bCmp==0 && pCsr->nLeTerm<nTerm) ){
        pCsr->bEof = 1;
      }
    }

    capdbFts5BufferSet(&rc, &pCsr->term, nTerm, (const u8*)zTerm);
  }
  return rc;
}

static int fts5VocabInstanceNext(Fts5VocabCursor *pCsr){
  int eDetail = pCsr->pFts5->pConfig->eDetail;
  int rc = CAPDB_OK;
  Fts5IndexIter *pIter = pCsr->pIter;
  i64 *pp = &pCsr->iInstPos;
  int *po = &pCsr->iInstOff;
  
  assert( capdbFts5IterEof(pIter)==0 );
  assert( pCsr->bEof==0 );
  while( eDetail==FTS5_DETAIL_NONE
      || capdbFts5PoslistNext64(pIter->pData, pIter->nData, po, pp) 
  ){
    pCsr->iInstPos = 0;
    pCsr->iInstOff = 0;

    rc = capdbFts5IterNextScan(pCsr->pIter);
    if( rc==CAPDB_OK ){
      rc = fts5VocabInstanceNewTerm(pCsr);
      if( pCsr->bEof || eDetail==FTS5_DETAIL_NONE ) break;
    }
    if( rc ){
      pCsr->bEof = 1;
      break;
    }
  }

  return rc;
}

/*
** Advance the cursor to the next row in the table.
*/
static int fts5VocabNextMethod(capdb_vtab_cursor *pCursor){
  Fts5VocabCursor *pCsr = (Fts5VocabCursor*)pCursor;
  Fts5VocabTable *pTab = (Fts5VocabTable*)pCursor->pVtab;
  int nCol = pCsr->pFts5->pConfig->nCol;
  int rc;

  rc = capdbFts5StructureTest(pCsr->pFts5->pIndex, pCsr->pStruct);
  if( rc!=CAPDB_OK ) return rc;
  pCsr->rowid++;

  if( pTab->eType==FTS5_VOCAB_INSTANCE ){
    return fts5VocabInstanceNext(pCsr);
  }

  if( pTab->eType==FTS5_VOCAB_COL ){
    for(pCsr->iCol++; pCsr->iCol<nCol; pCsr->iCol++){
      if( pCsr->aDoc[pCsr->iCol] ) break;
    }
  }

  if( pTab->eType!=FTS5_VOCAB_COL || pCsr->iCol>=nCol ){
    if( capdbFts5IterEof(pCsr->pIter) ){
      pCsr->bEof = 1;
    }else{
      const char *zTerm;
      int nTerm;

      zTerm = capdbFts5IterTerm(pCsr->pIter, &nTerm);
      assert( nTerm>=0 );
      if( pCsr->nLeTerm>=0 ){
        int nCmp = MIN(nTerm, pCsr->nLeTerm);
        int bCmp = memcmp(pCsr->zLeTerm, zTerm, nCmp);
        if( bCmp<0 || (bCmp==0 && pCsr->nLeTerm<nTerm) ){
          pCsr->bEof = 1;
          return CAPDB_OK;
        }
      }

      capdbFts5BufferSet(&rc, &pCsr->term, nTerm, (const u8*)zTerm);
      memset(pCsr->aCnt, 0, nCol * sizeof(i64));
      memset(pCsr->aDoc, 0, nCol * sizeof(i64));
      pCsr->iCol = 0;

      assert( pTab->eType==FTS5_VOCAB_COL || pTab->eType==FTS5_VOCAB_ROW );
      while( rc==CAPDB_OK ){
        int eDetail = pCsr->pFts5->pConfig->eDetail;
        const u8 *pPos; int nPos;   /* Position list */
        i64 iPos = 0;               /* 64-bit position read from poslist */
        int iOff = 0;               /* Current offset within position list */

        pPos = pCsr->pIter->pData;
        nPos = pCsr->pIter->nData;

        switch( pTab->eType ){
          case FTS5_VOCAB_ROW:
            /* Do not bother counting the number of instances if the "cnt"
            ** column is not being read (according to colUsed).  */
            if( eDetail==FTS5_DETAIL_FULL && (pCsr->colUsed & 0x04) ){
              while( iPos<nPos ){
                u32 ii;
                fts5FastGetVarint32(pPos, iPos, ii);
                if( ii==1 ){ 
                  /* New column in the position list */
                  fts5FastGetVarint32(pPos, iPos, ii);
                }else{
                  /* An instance - increment pCsr->aCnt[] */
                  pCsr->aCnt[0]++;
                }
              }
            }
            pCsr->aDoc[0]++;
            break;

          case FTS5_VOCAB_COL:
            if( eDetail==FTS5_DETAIL_FULL ){
              int iCol = -1;
              while( 0==capdbFts5PoslistNext64(pPos, nPos, &iOff, &iPos) ){
                int ii = FTS5_POS2COLUMN(iPos);
                if( iCol!=ii ){
                  if( ii>=nCol ){
                    rc = FTS5_CORRUPT;
                    break;
                  }
                  pCsr->aDoc[ii]++;
                  iCol = ii;
                }
                pCsr->aCnt[ii]++;
              }
            }else if( eDetail==FTS5_DETAIL_COLUMNS ){
              while( 0==capdbFts5PoslistNext64(pPos, nPos, &iOff,&iPos) ){
                assert_nc( iPos>=0 && iPos<nCol );
                if( iPos>=nCol ){
                  rc = FTS5_CORRUPT;
                  break;
                }
                pCsr->aDoc[iPos]++;
              }
            }else{
              assert( eDetail==FTS5_DETAIL_NONE );
              pCsr->aDoc[0]++;
            }
            break;

          default:
            assert( pTab->eType==FTS5_VOCAB_INSTANCE );
            break;
        }

        if( rc==CAPDB_OK ){
          rc = capdbFts5IterNextScan(pCsr->pIter);
        }
        if( pTab->eType==FTS5_VOCAB_INSTANCE ) break;

        if( rc==CAPDB_OK ){
          zTerm = capdbFts5IterTerm(pCsr->pIter, &nTerm);
          if( nTerm!=pCsr->term.n 
          || (nTerm>0 && memcmp(zTerm, pCsr->term.p, nTerm)) 
          ){
            break;
          }
          if( capdbFts5IterEof(pCsr->pIter) ) break;
        }
      }
    }
  }

  if( rc==CAPDB_OK && pCsr->bEof==0 && pTab->eType==FTS5_VOCAB_COL ){
    for(/* noop */; pCsr->iCol<nCol && pCsr->aDoc[pCsr->iCol]==0; pCsr->iCol++);
    if( pCsr->iCol==nCol ){
      rc = FTS5_CORRUPT;
    }
  }
  return rc;
}

/*
** This is the xFilter implementation for the virtual table.
*/
static int fts5VocabFilterMethod(
  capdb_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *zUnused,            /* Unused */
  int nUnused,                    /* Number of elements in apVal */
  capdb_value **apVal           /* Arguments for the indexing scheme */
){
  Fts5VocabTable *pTab = (Fts5VocabTable*)pCursor->pVtab;
  Fts5VocabCursor *pCsr = (Fts5VocabCursor*)pCursor;
  int eType = pTab->eType;
  int rc = CAPDB_OK;

  int iVal = 0;
  int f = FTS5INDEX_QUERY_SCAN;
  const char *zTerm = 0;
  int nTerm = 0;

  capdb_value *pEq = 0;
  capdb_value *pGe = 0;
  capdb_value *pLe = 0;

  UNUSED_PARAM2(zUnused, nUnused);

  fts5VocabResetCursor(pCsr);
  if( idxNum & FTS5_VOCAB_TERM_EQ ) pEq = apVal[iVal++];
  if( idxNum & FTS5_VOCAB_TERM_GE ) pGe = apVal[iVal++];
  if( idxNum & FTS5_VOCAB_TERM_LE ) pLe = apVal[iVal++];
  pCsr->colUsed = (idxNum & FTS5_VOCAB_COLUSED_MASK);

  if( pEq ){
    zTerm = (const char *)capdb_value_text(pEq);
    nTerm = capdb_value_bytes(pEq);
    f = FTS5INDEX_QUERY_NOTOKENDATA;
  }else{
    if( pGe ){
      zTerm = (const char *)capdb_value_text(pGe);
      nTerm = capdb_value_bytes(pGe);
    }
    if( pLe ){
      const char *zCopy = (const char *)capdb_value_text(pLe);
      if( zCopy==0 ) zCopy = "";
      pCsr->nLeTerm = capdb_value_bytes(pLe);
      pCsr->zLeTerm = capdb_malloc64((i64)pCsr->nLeTerm+1);
      if( pCsr->zLeTerm==0 ){
        rc = CAPDB_NOMEM;
      }else{
        memcpy(pCsr->zLeTerm, zCopy, pCsr->nLeTerm+1);
      }
    }
  }

  if( rc==CAPDB_OK ){
    Fts5Index *pIndex = pCsr->pFts5->pIndex;
    rc = capdbFts5IndexQuery(pIndex, zTerm, nTerm, f, 0, &pCsr->pIter);
    if( rc==CAPDB_OK ){
      pCsr->pStruct = capdbFts5StructureRef(pIndex);
    }
  }
  if( rc==CAPDB_OK && eType==FTS5_VOCAB_INSTANCE ){
    rc = fts5VocabInstanceNewTerm(pCsr);
  }
  if( rc==CAPDB_OK && !pCsr->bEof 
   && (eType!=FTS5_VOCAB_INSTANCE 
    || pCsr->pFts5->pConfig->eDetail!=FTS5_DETAIL_NONE)
  ){
    rc = fts5VocabNextMethod(pCursor);
  }

  return rc;
}

/* 
** This is the xEof method of the virtual table. SQLite calls this 
** routine to find out if it has reached the end of a result set.
*/
static int fts5VocabEofMethod(capdb_vtab_cursor *pCursor){
  Fts5VocabCursor *pCsr = (Fts5VocabCursor*)pCursor;
  return pCsr->bEof;
}

static int fts5VocabColumnMethod(
  capdb_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  capdb_context *pCtx,          /* Context for capdb_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts5VocabCursor *pCsr = (Fts5VocabCursor*)pCursor;
  int eDetail = pCsr->pFts5->pConfig->eDetail;
  int eType = ((Fts5VocabTable*)(pCursor->pVtab))->eType;
  i64 iVal = 0;

  if( iCol==0 ){
    capdb_result_text(
        pCtx, (const char*)pCsr->term.p, pCsr->term.n, CAPDB_TRANSIENT
    );
  }else if( eType==FTS5_VOCAB_COL ){
    assert( iCol==1 || iCol==2 || iCol==3 );
    if( iCol==1 ){
      if( eDetail!=FTS5_DETAIL_NONE ){
        const char *z = pCsr->pFts5->pConfig->azCol[pCsr->iCol];
        capdb_result_text(pCtx, z, -1, CAPDB_STATIC);
      }
    }else if( iCol==2 ){
      iVal = pCsr->aDoc[pCsr->iCol];
    }else{
      iVal = pCsr->aCnt[pCsr->iCol];
    }
  }else if( eType==FTS5_VOCAB_ROW ){
    assert( iCol==1 || iCol==2 );
    if( iCol==1 ){
      iVal = pCsr->aDoc[0];
    }else{
      iVal = pCsr->aCnt[0];
    }
  }else{
    assert( eType==FTS5_VOCAB_INSTANCE );
    switch( iCol ){
      case 1:
        capdb_result_int64(pCtx, pCsr->pIter->iRowid);
        break;
      case 2: {
        int ii = -1;
        if( eDetail==FTS5_DETAIL_FULL ){
          ii = FTS5_POS2COLUMN(pCsr->iInstPos);
        }else if( eDetail==FTS5_DETAIL_COLUMNS ){
          ii = (int)pCsr->iInstPos;
        }
        if( ii>=0 && ii<pCsr->pFts5->pConfig->nCol ){
          const char *z = pCsr->pFts5->pConfig->azCol[ii];
          capdb_result_text(pCtx, z, -1, CAPDB_STATIC);
        }
        break;
      }
      default: {
        assert( iCol==3 );
        if( eDetail==FTS5_DETAIL_FULL ){
          int ii = FTS5_POS2OFFSET(pCsr->iInstPos);
          capdb_result_int(pCtx, ii);
        }
        break;
      }
    }
  }

  if( iVal>0 ) capdb_result_int64(pCtx, iVal);
  return CAPDB_OK;
}

/* 
** This is the xRowid method. The SQLite core calls this routine to
** retrieve the rowid for the current row of the result set. The
** rowid should be written to *pRowid.
*/
static int fts5VocabRowidMethod(
  capdb_vtab_cursor *pCursor, 
  sqlite_int64 *pRowid
){
  Fts5VocabCursor *pCsr = (Fts5VocabCursor*)pCursor;
  *pRowid = pCsr->rowid;
  return CAPDB_OK;
}

int capdbFts5VocabInit(Fts5Global *pGlobal, capdb *db){
  static const capdb_module fts5Vocab = {
    /* iVersion      */ 2,
    /* xCreate       */ fts5VocabCreateMethod,
    /* xConnect      */ fts5VocabConnectMethod,
    /* xBestIndex    */ fts5VocabBestIndexMethod,
    /* xDisconnect   */ fts5VocabDisconnectMethod,
    /* xDestroy      */ fts5VocabDestroyMethod,
    /* xOpen         */ fts5VocabOpenMethod,
    /* xClose        */ fts5VocabCloseMethod,
    /* xFilter       */ fts5VocabFilterMethod,
    /* xNext         */ fts5VocabNextMethod,
    /* xEof          */ fts5VocabEofMethod,
    /* xColumn       */ fts5VocabColumnMethod,
    /* xRowid        */ fts5VocabRowidMethod,
    /* xUpdate       */ 0,
    /* xBegin        */ 0,
    /* xSync         */ 0,
    /* xCommit       */ 0,
    /* xRollback     */ 0,
    /* xFindFunction */ 0,
    /* xRename       */ 0,
    /* xSavepoint    */ 0,
    /* xRelease      */ 0,
    /* xRollbackTo   */ 0,
    /* xShadowName   */ 0,
    /* xIntegrity    */ 0
  };
  void *p = (void*)pGlobal;

  return capdb_create_module_v2(db, "fts5vocab", &fts5Vocab, p, 0);
}
