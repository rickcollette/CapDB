/*
** 2020-03-23
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
** This file implements virtual-tables for examining the bytecode content
** of a prepared statement.
*/
#include "capdbInt.h"
#if defined(CAPDB_ENABLE_BYTECODE_VTAB) && !defined(CAPDB_OMIT_VIRTUALTABLE)
#include "vdbeInt.h"

/* An instance of the bytecode() table-valued function.
*/
typedef struct bytecodevtab bytecodevtab;
struct bytecodevtab {
  capdb_vtab base;     /* Base class - must be first */
  capdb *db;           /* Database connection */
  int bTablesUsed;       /* 2 for tables_used().  0 for bytecode(). */
};

/* A cursor for scanning through the bytecode
*/
typedef struct bytecodevtab_cursor bytecodevtab_cursor;
struct bytecodevtab_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb_stmt *pStmt;       /* The statement whose bytecode is displayed */
  int iRowid;                /* The rowid of the output table */
  int iAddr;                 /* Address */
  int needFinalize;          /* Cursors owns pStmt and must finalize it */
  int showSubprograms;       /* Provide a listing of subprograms */
  Op *aOp;                   /* Operand array */
  char *zP4;                 /* Rendered P4 value */
  const char *zType;         /* tables_used.type */
  const char *zSchema;       /* tables_used.schema */
  const char *zName;         /* tables_used.name */
  Mem sub;                   /* Subprograms */
};

/*
** Create a new bytecode() table-valued function.
*/
static int bytecodevtabConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  bytecodevtab *pNew;
  int rc;
  int isTabUsed = pAux!=0;
  const char *azSchema[2] = {
    /* bytecode() schema */
    "CREATE TABLE x("
      "addr INT,"
      "opcode TEXT,"
      "p1 INT,"
      "p2 INT,"
      "p3 INT,"
      "p4 TEXT,"
      "p5 INT,"
      "comment TEXT,"
      "subprog TEXT," 
      "nexec INT,"
      "ncycle INT,"
      "stmt HIDDEN"
    ");",

    /* Tables_used() schema */
    "CREATE TABLE x("
      "type TEXT,"
      "schema TEXT,"
      "name TEXT,"
      "wr INT,"
      "subprog TEXT," 
      "stmt HIDDEN"
   ");"
  };

  (void)argc;
  (void)argv;
  (void)pzErr;
  rc = capdb_declare_vtab(db, azSchema[isTabUsed]);
  if( rc==CAPDB_OK ){
    pNew = capdb_malloc( sizeof(*pNew) );
    *ppVtab = (capdb_vtab*)pNew;
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    pNew->db = db;
    pNew->bTablesUsed = isTabUsed*2;
  }
  return rc;
}

/*
** This method is the destructor for bytecodevtab objects.
*/
static int bytecodevtabDisconnect(capdb_vtab *pVtab){
  bytecodevtab *p = (bytecodevtab*)pVtab;
  capdb_free(p);
  return CAPDB_OK;
}

/*
** Constructor for a new bytecodevtab_cursor object.
*/
static int bytecodevtabOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  bytecodevtab *pVTab = (bytecodevtab*)p;
  bytecodevtab_cursor *pCur;
  pCur = capdb_malloc( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  capdbVdbeMemInit(&pCur->sub, pVTab->db, 1);
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Clear all internal content from a bytecodevtab cursor.
*/
static void bytecodevtabCursorClear(bytecodevtab_cursor *pCur){
  capdb_free(pCur->zP4);
  pCur->zP4 = 0;
  capdbVdbeMemRelease(&pCur->sub);
  capdbVdbeMemSetNull(&pCur->sub);
  if( pCur->needFinalize ){
    capdb_finalize(pCur->pStmt);
  }
  pCur->pStmt = 0;
  pCur->needFinalize = 0;
  pCur->zType = 0;
  pCur->zSchema = 0;
  pCur->zName = 0;
}

/*
** Destructor for a bytecodevtab_cursor.
*/
static int bytecodevtabClose(capdb_vtab_cursor *cur){
  bytecodevtab_cursor *pCur = (bytecodevtab_cursor*)cur;
  bytecodevtabCursorClear(pCur);
  capdb_free(pCur);
  return CAPDB_OK;
}


/*
** Advance a bytecodevtab_cursor to its next row of output.
*/
static int bytecodevtabNext(capdb_vtab_cursor *cur){
  bytecodevtab_cursor *pCur = (bytecodevtab_cursor*)cur;
  bytecodevtab *pTab = (bytecodevtab*)cur->pVtab;
  int rc;
  if( pCur->zP4 ){
    capdb_free(pCur->zP4);
    pCur->zP4 = 0;
  }
  if( pCur->zName ){
    pCur->zName = 0;
    pCur->zType = 0;
    pCur->zSchema = 0;
  }
  rc = capdbVdbeNextOpcode(
           (Vdbe*)pCur->pStmt, 
           pCur->showSubprograms ? &pCur->sub : 0,
           pTab->bTablesUsed,
           &pCur->iRowid,
           &pCur->iAddr,
           &pCur->aOp);
  if( rc!=CAPDB_OK ){
    capdbVdbeMemSetNull(&pCur->sub);
    pCur->aOp = 0;
  }
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int bytecodevtabEof(capdb_vtab_cursor *cur){
  bytecodevtab_cursor *pCur = (bytecodevtab_cursor*)cur;
  return pCur->aOp==0;
}

/*
** Return values of columns for the row at which the bytecodevtab_cursor
** is currently pointing.
*/
static int bytecodevtabColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  bytecodevtab_cursor *pCur = (bytecodevtab_cursor*)cur;
  bytecodevtab *pVTab = (bytecodevtab*)cur->pVtab;
  Op *pOp = pCur->aOp + pCur->iAddr;
  if( pVTab->bTablesUsed ){
    if( i==4 ){
      i = 8;
    }else{
      if( i<=2 && pCur->zType==0 ){
        Schema *pSchema;
        HashElem *k;
        int iDb = pOp->p3;
        Pgno iRoot = (Pgno)pOp->p2;
        capdb *db = pVTab->db;
        pSchema = db->aDb[iDb].pSchema;
        pCur->zSchema = db->aDb[iDb].zDbSName;
        for(k=sqliteHashFirst(&pSchema->tblHash); k; k=sqliteHashNext(k)){
          Table *pTab = (Table*)sqliteHashData(k);
          if( !IsVirtual(pTab) && pTab->tnum==iRoot ){
            pCur->zName = pTab->zName;
            pCur->zType = "table";
            break;
          }
        }
        if( pCur->zName==0 ){
          for(k=sqliteHashFirst(&pSchema->idxHash); k; k=sqliteHashNext(k)){
            Index *pIdx = (Index*)sqliteHashData(k);
            if( pIdx->tnum==iRoot ){
              pCur->zName = pIdx->zName;
              pCur->zType = "index";
            }
          }
        }
      }
      i += 20;
    }
  }
  switch( i ){
    case 0:   /* addr */
      capdb_result_int(ctx, pCur->iAddr);
      break;
    case 1:   /* opcode */
      capdb_result_text(ctx, (char*)capdbOpcodeName(pOp->opcode),
                          -1, CAPDB_STATIC);
      break;
    case 2:   /* p1 */
      capdb_result_int(ctx, pOp->p1);
      break;
    case 3:   /* p2 */
      capdb_result_int(ctx, pOp->p2);
      break;
    case 4:   /* p3 */
      capdb_result_int(ctx, pOp->p3);
      break;
    case 5:   /* p4 */
    case 7:   /* comment */
      if( pCur->zP4==0 ){
        pCur->zP4 = capdbVdbeDisplayP4(pVTab->db, pOp);
      }
      if( i==5 ){
        capdb_result_text(ctx, pCur->zP4, -1, CAPDB_STATIC);
      }else{
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
        char *zCom = capdbVdbeDisplayComment(pVTab->db, pOp, pCur->zP4);
        capdb_result_text(ctx, zCom, -1, capdb_free);
#endif
      }
      break;
    case 6:     /* p5 */
      capdb_result_int(ctx, pOp->p5);
      break;
    case 8: {   /* subprog */
      Op *aOp = pCur->aOp;
      assert( aOp[0].opcode==OP_Init );
      assert( aOp[0].p4.z==0 || strncmp(aOp[0].p4.z,"-" "- ",3)==0 );
      if( pCur->iRowid==pCur->iAddr+1 ){
        break;  /* Result is NULL for the main program */
      }else if( aOp[0].p4.z!=0 ){
         capdb_result_text(ctx, aOp[0].p4.z+3, -1, CAPDB_STATIC);
      }else{
         capdb_result_text(ctx, "(FK)", 4, CAPDB_STATIC);
      }
      break;
    }

#ifdef CAPDB_ENABLE_STMT_SCANSTATUS
    case 9:     /* nexec */
      capdb_result_int64(ctx, pOp->nExec);
      break;
    case 10:    /* ncycle */
      capdb_result_int64(ctx, pOp->nCycle);
      break;
#else
    case 9:     /* nexec */
    case 10:    /* ncycle */
      capdb_result_int(ctx, 0);
      break;
#endif

    case 20:  /* tables_used.type */
      capdb_result_text(ctx, pCur->zType, -1, CAPDB_STATIC);
      break;
    case 21:  /* tables_used.schema */
      capdb_result_text(ctx, pCur->zSchema, -1, CAPDB_STATIC);
      break;
    case 22:  /* tables_used.name */
      capdb_result_text(ctx, pCur->zName, -1, CAPDB_STATIC);
      break;
    case 23:  /* tables_used.wr */
      capdb_result_int(ctx, pOp->opcode==OP_OpenWrite);
      break;
  }
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int bytecodevtabRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  bytecodevtab_cursor *pCur = (bytecodevtab_cursor*)cur;
  *pRowid = pCur->iRowid;
  return CAPDB_OK;
}

/*
** Initialize a cursor.
**
**    idxNum==0     means show all subprograms
**    idxNum==1     means show only the main bytecode and omit subprograms.
*/
static int bytecodevtabFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  bytecodevtab_cursor *pCur = (bytecodevtab_cursor *)pVtabCursor;
  bytecodevtab *pVTab = (bytecodevtab *)pVtabCursor->pVtab;
  int rc = CAPDB_OK;
  (void)idxStr;

  bytecodevtabCursorClear(pCur);
  pCur->iRowid = 0;
  pCur->iAddr = 0;
  pCur->showSubprograms = idxNum==0;
  assert( argc==1 );
  if( capdb_value_type(argv[0])==CAPDB_TEXT ){
    const char *zSql = (const char*)capdb_value_text(argv[0]);
    if( zSql==0 ){
      rc = CAPDB_NOMEM;
    }else{
      rc = capdb_prepare_v2(pVTab->db, zSql, -1, &pCur->pStmt, 0);
      pCur->needFinalize = 1;
    }
  }else{
    pCur->pStmt = (capdb_stmt*)capdb_value_pointer(argv[0],"stmt-pointer");
  }
  if( pCur->pStmt==0 ){
    pVTab->base.zErrMsg = capdb_mprintf(
       "argument to %s() is not a valid SQL statement",
       pVTab->bTablesUsed ? "tables_used" : "bytecode"
    );
    rc = CAPDB_ERROR;
  }else{
    bytecodevtabNext(pVtabCursor);
  }
  return rc;
}

/*
** We must have a single stmt=? constraint that will be passed through
** into the xFilter method.  If there is no valid stmt=? constraint,
** then return an CAPDB_CONSTRAINT error.
*/
static int bytecodevtabBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  int i;
  int rc = CAPDB_CONSTRAINT;
  struct capdb_index_constraint *p;
  bytecodevtab *pVTab = (bytecodevtab*)tab;
  int iBaseCol = pVTab->bTablesUsed ? 4 : 10;
  pIdxInfo->estimatedCost = (double)100;
  pIdxInfo->estimatedRows = 100;
  pIdxInfo->idxNum = 0;
  for(i=0, p=pIdxInfo->aConstraint; i<pIdxInfo->nConstraint; i++, p++){
    if( p->usable==0 ) continue;
    if( p->op==CAPDB_INDEX_CONSTRAINT_EQ && p->iColumn==iBaseCol+1 ){
      rc = CAPDB_OK;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      pIdxInfo->aConstraintUsage[i].argvIndex = 1;
    }
    if( p->op==CAPDB_INDEX_CONSTRAINT_ISNULL && p->iColumn==iBaseCol ){
      pIdxInfo->aConstraintUsage[i].omit = 1;
      pIdxInfo->idxNum = 1;
    }
  }
  return rc;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static capdb_module bytecodevtabModule = {
  /* iVersion    */ 0,
  /* xCreate     */ 0,
  /* xConnect    */ bytecodevtabConnect,
  /* xBestIndex  */ bytecodevtabBestIndex,
  /* xDisconnect */ bytecodevtabDisconnect,
  /* xDestroy    */ 0,
  /* xOpen       */ bytecodevtabOpen,
  /* xClose      */ bytecodevtabClose,
  /* xFilter     */ bytecodevtabFilter,
  /* xNext       */ bytecodevtabNext,
  /* xEof        */ bytecodevtabEof,
  /* xColumn     */ bytecodevtabColumn,
  /* xRowid      */ bytecodevtabRowid,
  /* xUpdate     */ 0,
  /* xBegin      */ 0,
  /* xSync       */ 0,
  /* xCommit     */ 0,
  /* xRollback   */ 0,
  /* xFindMethod */ 0,
  /* xRename     */ 0,
  /* xSavepoint  */ 0,
  /* xRelease    */ 0,
  /* xRollbackTo */ 0,
  /* xShadowName */ 0,
  /* xIntegrity  */ 0
};


int capdbVdbeBytecodeVtabInit(capdb *db){
  int rc;
  rc = capdb_create_module(db, "bytecode", &bytecodevtabModule, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_module(db, "tables_used", &bytecodevtabModule, &db);
  }
  return rc;
}
#elif defined(CAPDB_ENABLE_BYTECODE_VTAB)
int capdbVdbeBytecodeVtabInit(capdb *db){ return CAPDB_OK; }
#endif /* CAPDB_ENABLE_BYTECODE_VTAB */
