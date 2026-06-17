/*
** 2003 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the PRAGMA command.
*/
#include "capdbInt.h"

#if !defined(CAPDB_ENABLE_LOCKING_STYLE)
#  if defined(__APPLE__)
#    define CAPDB_ENABLE_LOCKING_STYLE 1
#  else
#    define CAPDB_ENABLE_LOCKING_STYLE 0
#  endif
#endif

/***************************************************************************
** The "pragma.h" include file is an automatically generated file that
** that includes the PragType_XXXX macro definitions and the aPragmaName[]
** object.  This ensures that the aPragmaName[] table is arranged in
** lexicographical order to facility a binary search of the pragma name.
** Do not edit pragma.h directly.  Edit and rerun the script in at
** ../tool/mkpragmatab.tcl. */
#include "pragma.h"

/*
** When the 0x10 bit of PRAGMA optimize is set, any ANALYZE commands
** will be run with an analysis_limit set to the lessor of the value of
** the following macro or to the actual analysis_limit if it is non-zero,
** in order to prevent PRAGMA optimize from running for too long.
**
** The value of 2000 is chosen empirically so that the worst-case run-time
** for PRAGMA optimize does not exceed 100 milliseconds against a variety
** of test databases on a RaspberryPI-4 compiled using -Os and without
** -DCAPDB_DEBUG.  Of course, your mileage may vary.  For the purpose of
** this paragraph, "worst-case" means that ANALYZE ends up being
** run on every table in the database.  The worst case typically only
** happens if PRAGMA optimize is run on a database file for which ANALYZE
** has not been previously run and the 0x10000 flag is included so that
** all tables are analyzed.  The usual case for PRAGMA optimize is that
** no ANALYZE commands will be run at all, or if any ANALYZE happens it
** will be against a single table, so that expected timing for PRAGMA
** optimize on a PI-4 is more like 1 millisecond or less with the 0x10000
** flag or less than 100 microseconds without the 0x10000 flag.
**
** An analysis limit of 2000 is almost always sufficient for the query
** planner to fully characterize an index.  The additional accuracy from
** a larger analysis is not usually helpful.
*/
#ifndef CAPDB_DEFAULT_OPTIMIZE_LIMIT
# define CAPDB_DEFAULT_OPTIMIZE_LIMIT 2000
#endif

/*
** Interpret the given string as a safety level.  Return 0 for OFF,
** 1 for ON or NORMAL, 2 for FULL, and 3 for EXTRA.  Return 1 for an empty or
** unrecognized string argument.  The FULL and EXTRA option is disallowed
** if the omitFull parameter it 1.
**
** Note that the values returned are one less that the values that
** should be passed into capdbBtreeSetSafetyLevel().  The is done
** to support legacy SQL code.  The safety level used to be boolean
** and older scripts may have used numbers 0 for OFF and 1 for ON.
*/
static u8 getSafetyLevel(const char *z, int omitFull, u8 dflt){
                             /* 123456789 123456789 123 */
  static const char zText[] = "onoffalseyestruextrafull";
  static const u8 iOffset[] = {0, 1, 2,  4,    9,  12,  15,   20};
  static const u8 iLength[] = {2, 2, 3,  5,    3,   4,   5,    4};
  static const u8 iValue[] =  {1, 0, 0,  0,    1,   1,   3,    2};
                            /* on no off false yes true extra full */
  int i, n;
  if( capdbIsdigit(*z) ){
    return (u8)capdbAtoi(z);
  }
  n = capdbStrlen30(z);
  for(i=0; i<ArraySize(iLength); i++){
    if( iLength[i]==n && capdbStrNICmp(&zText[iOffset[i]],z,n)==0
     && (!omitFull || iValue[i]<=1)
    ){
      return iValue[i];
    }
  }
  return dflt;
}

/*
** Interpret the given string as a boolean value.
*/
u8 capdbGetBoolean(const char *z, u8 dflt){
  return getSafetyLevel(z,1,dflt)!=0;
}

/* The capdbGetBoolean() function is used by other modules but the
** remainder of this file is specific to PRAGMA processing.  So omit
** the rest of the file if PRAGMAs are omitted from the build.
*/
#if !defined(CAPDB_OMIT_PRAGMA)

/*
** Interpret the given string as a locking mode value.
*/
static int getLockingMode(const char *z){
  if( z ){
    if( 0==capdbStrICmp(z, "exclusive") ) return PAGER_LOCKINGMODE_EXCLUSIVE;
    if( 0==capdbStrICmp(z, "normal") ) return PAGER_LOCKINGMODE_NORMAL;
  }
  return PAGER_LOCKINGMODE_QUERY;
}

#ifndef CAPDB_OMIT_AUTOVACUUM
/*
** Interpret the given string as an auto-vacuum mode value.
**
** The following strings, "none", "full" and "incremental" are
** acceptable, as are their numeric equivalents: 0, 1 and 2 respectively.
*/
static int getAutoVacuum(const char *z){
  int i;
  if( 0==capdbStrICmp(z, "none") ) return BTREE_AUTOVACUUM_NONE;
  if( 0==capdbStrICmp(z, "full") ) return BTREE_AUTOVACUUM_FULL;
  if( 0==capdbStrICmp(z, "incremental") ) return BTREE_AUTOVACUUM_INCR;
  i = capdbAtoi(z);
  return (u8)((i>=0&&i<=2)?i:0);
}
#endif /* ifndef CAPDB_OMIT_AUTOVACUUM */

#ifndef CAPDB_OMIT_PAGER_PRAGMAS
/*
** Interpret the given string as a temp db location. Return 1 for file
** backed temporary databases, 2 for the Red-Black tree in memory database
** and 0 to use the compile-time default.
*/
static int getTempStore(const char *z){
  if( z[0]>='0' && z[0]<='2' ){
    return z[0] - '0';
  }else if( capdbStrICmp(z, "file")==0 ){
    return 1;
  }else if( capdbStrICmp(z, "memory")==0 ){
    return 2;
  }else{
    return 0;
  }
}
#endif /* CAPDB_PAGER_PRAGMAS */

#ifndef CAPDB_OMIT_PAGER_PRAGMAS
/*
** Invalidate temp storage, either when the temp storage is changed
** from default, or when 'file' and the temp_store_directory has changed
*/
static int invalidateTempStorage(Parse *pParse){
  capdb *db = pParse->db;
  if( db->aDb[1].pBt!=0 ){
    if( !db->autoCommit
     || capdbBtreeTxnState(db->aDb[1].pBt)!=CAPDB_TXN_NONE
    ){
      capdbErrorMsg(pParse, "temporary storage cannot be changed "
        "from within a transaction");
      return CAPDB_ERROR;
    }
    capdbBtreeClose(db->aDb[1].pBt);
    db->aDb[1].pBt = 0;
    capdbResetAllSchemasOfConnection(db);
  }
  return CAPDB_OK;
}
#endif /* CAPDB_PAGER_PRAGMAS */

#ifndef CAPDB_OMIT_PAGER_PRAGMAS
/*
** If the TEMP database is open, close it and mark the database schema
** as needing reloading.  This must be done when using the CAPDB_TEMP_STORE
** or DEFAULT_TEMP_STORE pragmas.
*/
static int changeTempStorage(Parse *pParse, const char *zStorageType){
  int ts = getTempStore(zStorageType);
  capdb *db = pParse->db;
  if( db->temp_store==ts ) return CAPDB_OK;
  if( invalidateTempStorage( pParse ) != CAPDB_OK ){
    return CAPDB_ERROR;
  }
  db->temp_store = (u8)ts;
  return CAPDB_OK;
}
#endif /* CAPDB_PAGER_PRAGMAS */

/*
** Set result column names for a pragma.
*/
static void setPragmaResultColumnNames(
  Vdbe *v,                     /* The query under construction */
  const PragmaName *pPragma    /* The pragma */
){
  u8 n = pPragma->nPragCName;
  capdbVdbeSetNumCols(v, n==0 ? 1 : n);
  if( n==0 ){
    capdbVdbeSetColName(v, 0, COLNAME_NAME, pPragma->zName, CAPDB_STATIC);
  }else{
    int i, j;
    for(i=0, j=pPragma->iPragCName; i<n; i++, j++){
      capdbVdbeSetColName(v, i, COLNAME_NAME, pragCName[j], CAPDB_STATIC);
    }
  }
}

/*
** Generate code to return a single integer value.
*/
static void returnSingleInt(Vdbe *v, i64 value){
  capdbVdbeAddOp4Dup8(v, OP_Int64, 0, 1, 0, (const u8*)&value, P4_INT64);
  capdbVdbeAddOp2(v, OP_ResultRow, 1, 1);
}

/*
** Generate code to return a single text value.
*/
static void returnSingleText(
  Vdbe *v,                /* Prepared statement under construction */
  const char *zValue      /* Value to be returned */
){
  if( zValue ){
    capdbVdbeLoadString(v, 1, (const char*)zValue);
    capdbVdbeAddOp2(v, OP_ResultRow, 1, 1);
  }
}


/*
** Set the safety_level and pager flags for pager iDb.  Or if iDb<0
** set these values for all pagers.
*/
#ifndef CAPDB_OMIT_PAGER_PRAGMAS
static void setAllPagerFlags(capdb *db){
  if( db->autoCommit ){
    Db *pDb = db->aDb;
    int n = db->nDb;
    assert( CAPDB_FullFSync==PAGER_FULLFSYNC );
    assert( CAPDB_CkptFullFSync==PAGER_CKPT_FULLFSYNC );
    assert( CAPDB_CacheSpill==PAGER_CACHESPILL );
    assert( (PAGER_FULLFSYNC | PAGER_CKPT_FULLFSYNC | PAGER_CACHESPILL)
             ==  PAGER_FLAGS_MASK );
    assert( (pDb->safety_level & PAGER_SYNCHRONOUS_MASK)==pDb->safety_level );
    while( (n--) > 0 ){
      if( pDb->pBt ){
        capdbBtreeSetPagerFlags(pDb->pBt,
                 pDb->safety_level | (db->flags & PAGER_FLAGS_MASK) );
      }
      pDb++;
    }
  }
}
#else
# define setAllPagerFlags(X)  /* no-op */
#endif


/*
** Return a human-readable name for a constraint resolution action.
*/
#ifndef CAPDB_OMIT_FOREIGN_KEY
static const char *actionName(u8 action){
  const char *zName;
  switch( action ){
    case OE_SetNull:  zName = "SET NULL";        break;
    case OE_SetDflt:  zName = "SET DEFAULT";     break;
    case OE_Cascade:  zName = "CASCADE";         break;
    case OE_Restrict: zName = "RESTRICT";        break;
    default:          zName = "NO ACTION"; 
                      assert( action==OE_None ); break;
  }
  return zName;
}
#endif


/*
** Parameter eMode must be one of the PAGER_JOURNALMODE_XXX constants
** defined in pager.h. This function returns the associated lowercase
** journal-mode name.
*/
const char *capdbJournalModename(int eMode){
  static char * const azModeName[] = {
    "delete", "persist", "off", "truncate", "memory"
#ifndef CAPDB_OMIT_WAL
     , "wal"
#endif
  };
  assert( PAGER_JOURNALMODE_DELETE==0 );
  assert( PAGER_JOURNALMODE_PERSIST==1 );
  assert( PAGER_JOURNALMODE_OFF==2 );
  assert( PAGER_JOURNALMODE_TRUNCATE==3 );
  assert( PAGER_JOURNALMODE_MEMORY==4 );
  assert( PAGER_JOURNALMODE_WAL==5 );
  assert( eMode>=0 && eMode<=ArraySize(azModeName) );

  if( eMode==ArraySize(azModeName) ) return 0;
  return azModeName[eMode];
}

/*
** Locate a pragma in the aPragmaName[] array.
*/
static const PragmaName *pragmaLocate(const char *zName){
  int upr, lwr, mid = 0, rc;
  lwr = 0;
  upr = ArraySize(aPragmaName)-1;
  while( lwr<=upr ){
    mid = (lwr+upr)/2;
    rc = capdb_stricmp(zName, aPragmaName[mid].zName);
    if( rc==0 ) break;
    if( rc<0 ){
      upr = mid - 1;
    }else{
      lwr = mid + 1;
    }
  }
  return lwr>upr ? 0 : &aPragmaName[mid];
}

/*
** Create zero or more entries in the output for the SQL functions
** defined by FuncDef p.
*/
static void pragmaFunclistLine(
  Vdbe *v,               /* The prepared statement being created */
  FuncDef *p,            /* A particular function definition */
  int isBuiltin,         /* True if this is a built-in function */
  int showInternFuncs    /* True if showing internal functions */
){
  u32 mask =
      CAPDB_DETERMINISTIC |
      CAPDB_DIRECTONLY |
      CAPDB_SUBTYPE |
      CAPDB_INNOCUOUS |
      CAPDB_FUNC_INTERNAL
  ;
  if( showInternFuncs ) mask = 0xffffffff;
  for(; p; p=p->pNext){
    const char *zType;
    static const char *azEnc[] = { 0, "utf8", "utf16le", "utf16be" };

    assert( CAPDB_FUNC_ENCMASK==0x3 );
    assert( strcmp(azEnc[CAPDB_UTF8],"utf8")==0 );
    assert( strcmp(azEnc[CAPDB_UTF16LE],"utf16le")==0 );
    assert( strcmp(azEnc[CAPDB_UTF16BE],"utf16be")==0 );

    if( p->xSFunc==0 ) continue;
    if( (p->funcFlags & CAPDB_FUNC_INTERNAL)!=0
     && showInternFuncs==0
    ){
      continue;
    }   
    if( p->xValue!=0 ){
      zType = "w";
    }else if( p->xFinalize!=0 ){
      zType = "a";
    }else{
      zType = "s";
    }
    capdbVdbeMultiLoad(v, 1, "sissii",
       p->zName, isBuiltin,
       zType, azEnc[p->funcFlags&CAPDB_FUNC_ENCMASK],
       p->nArg,
       (p->funcFlags & mask) ^ CAPDB_INNOCUOUS
    );
  }
}


/*
** Helper subroutine for PRAGMA integrity_check:
**
** Generate code to output a single-column result row with a value of the
** string held in register 3.  Decrement the result count in register 1
** and halt if the maximum number of result rows have been issued.
*/
static int integrityCheckResultRow(Vdbe *v){
  int addr;
  capdbVdbeAddOp2(v, OP_ResultRow, 3, 1);
  addr = capdbVdbeAddOp3(v, OP_IfPos, 1, capdbVdbeCurrentAddr(v)+2, 1);
  VdbeCoverage(v);
  capdbVdbeAddOp0(v, OP_Halt);
  return addr;
}

/*
** Should table pTab be skipped when doing an integrity_check?
** Return true or false.
**
** If pObjTab is not null, the return true if pTab matches pObjTab.
**
** If pObjTab is null, then return true only if pTab is an imposter table.
*/
static int tableSkipIntegrityCheck(const Table *pTab, const Table *pObjTab){
  if( pObjTab ){
    return pTab!=pObjTab;
  }else{
    return (pTab->tabFlags & TF_Imposter)!=0;
  }
}

/*
** Process a pragma statement. 
**
** Pragmas are of this form:
**
**      PRAGMA [schema.]id [= value]
**
** The identifier might also be a string.  The value is a string, and
** identifier, or a number.  If minusFlag is true, then the value is
** a number that was preceded by a minus sign.
**
** If the left side is "database.id" then pId1 is the database name
** and pId2 is the id.  If the left side is just "id" then pId1 is the
** id and pId2 is any empty string.
*/
void capdbPragma(
  Parse *pParse,
  Token *pId1,        /* First part of [schema.]id field */
  Token *pId2,        /* Second part of [schema.]id field, or NULL */
  Token *pValue,      /* Token for <value>, or NULL */
  int minusFlag       /* True if a '-' sign preceded <value> */
){
  char *zLeft = 0;       /* Nul-terminated UTF-8 string <id> */
  char *zRight = 0;      /* Nul-terminated UTF-8 string <value>, or NULL */
  const char *zDb = 0;   /* The database name */
  Token *pId;            /* Pointer to <id> token */
  char *aFcntl[4];       /* Argument to CAPDB_FCNTL_PRAGMA */
  int iDb;               /* Database index for <database> */
  int rc;                      /* return value form CAPDB_FCNTL_PRAGMA */
  capdb *db = pParse->db;    /* The database connection */
  Db *pDb;                     /* The specific database being pragmaed */
  Vdbe *v = capdbGetVdbe(pParse);  /* Prepared statement */
  const PragmaName *pPragma;   /* The pragma */

  if( v==0 ) return;
  capdbVdbeRunOnlyOnce(v);
  pParse->nMem = 2;

  /* Interpret the [schema.] part of the pragma statement. iDb is the
  ** index of the database this pragma is being applied to in db.aDb[]. */
  iDb = capdbTwoPartName(pParse, pId1, pId2, &pId);
  if( iDb<0 ) return;
  pDb = &db->aDb[iDb];

  /* If the temp database has been explicitly named as part of the
  ** pragma, make sure it is open.
  */
  if( iDb==1 && capdbOpenTempDatabase(pParse) ){
    return;
  }

  zLeft = capdbNameFromToken(db, pId);
  if( !zLeft ) return;
  if( minusFlag ){
    zRight = capdbMPrintf(db, "-%T", pValue);
  }else{
    zRight = capdbNameFromToken(db, pValue);
  }

  assert( pId2 );
  zDb = pId2->n>0 ? pDb->zDbSName : 0;
  if( capdbAuthCheck(pParse, CAPDB_PRAGMA, zLeft, zRight, zDb) ){
    goto pragma_out;
  }

  /* Send an CAPDB_FCNTL_PRAGMA file-control to the underlying VFS
  ** connection.  If it returns CAPDB_OK, then assume that the VFS
  ** handled the pragma and generate a no-op prepared statement.
  **
  ** IMPLEMENTATION-OF: R-12238-55120 Whenever a PRAGMA statement is parsed,
  ** an CAPDB_FCNTL_PRAGMA file control is sent to the open capdb_file
  ** object corresponding to the database file to which the pragma
  ** statement refers.
  **
  ** IMPLEMENTATION-OF: R-29875-31678 The argument to the CAPDB_FCNTL_PRAGMA
  ** file control is an array of pointers to strings (char**) in which the
  ** second element of the array is the name of the pragma and the third
  ** element is the argument to the pragma or NULL if the pragma has no
  ** argument.
  */
  aFcntl[0] = 0;
  aFcntl[1] = zLeft;
  aFcntl[2] = zRight;
  aFcntl[3] = 0;
  db->busyHandler.nBusy = 0;
  rc = capdb_file_control(db, zDb, CAPDB_FCNTL_PRAGMA, (void*)aFcntl);
  if( rc==CAPDB_OK ){
    capdbVdbeSetNumCols(v, 1);
    capdbVdbeSetColName(v, 0, COLNAME_NAME, aFcntl[0], CAPDB_TRANSIENT);
    returnSingleText(v, aFcntl[0]);
    capdb_free(aFcntl[0]);
    goto pragma_out;
  }
  if( rc!=CAPDB_NOTFOUND ){
    if( aFcntl[0] ){
      capdbErrorMsg(pParse, "%s", aFcntl[0]);
      capdb_free(aFcntl[0]);
    }
    pParse->nErr++;
    pParse->rc = rc;
    goto pragma_out;
  }

  /* Locate the pragma in the lookup table */
  pPragma = pragmaLocate(zLeft);
  if( pPragma==0 ){
    /* IMP: R-43042-22504 No error messages are generated if an
    ** unknown pragma is issued. */
    goto pragma_out;
  }

  /* Make sure the database schema is loaded if the pragma requires that */
  if( (pPragma->mPragFlg & PragFlg_NeedSchema)!=0 ){
    if( capdbReadSchema(pParse) ) goto pragma_out;
  }

  /* Register the result column names for pragmas that return results */
  if( (pPragma->mPragFlg & PragFlg_NoColumns)==0
   && ((pPragma->mPragFlg & PragFlg_NoColumns1)==0 || zRight==0)
  ){
    setPragmaResultColumnNames(v, pPragma);
  }

  /* Jump to the appropriate pragma handler */
  switch( pPragma->ePragTyp ){
 
#if !defined(CAPDB_OMIT_PAGER_PRAGMAS) && !defined(CAPDB_OMIT_DEPRECATED)
  /*
  **  PRAGMA [schema.]default_cache_size
  **  PRAGMA [schema.]default_cache_size=N
  **
  ** The first form reports the current persistent setting for the
  ** page cache size.  The value returned is the maximum number of
  ** pages in the page cache.  The second form sets both the current
  ** page cache size value and the persistent page cache size value
  ** stored in the database file.
  **
  ** Older versions of SQLite would set the default cache size to a
  ** negative number to indicate synchronous=OFF.  These days, synchronous
  ** is always on by default regardless of the sign of the default cache
  ** size.  But continue to take the absolute value of the default cache
  ** size of historical compatibility.
  */
  case PragTyp_DEFAULT_CACHE_SIZE: {
    static const int iLn = VDBE_OFFSET_LINENO(2);
    static const VdbeOpList getCacheSize[] = {
      { OP_Transaction, 0, 0,        0},                         /* 0 */
      { OP_ReadCookie,  0, 1,        BTREE_DEFAULT_CACHE_SIZE},  /* 1 */
      { OP_IfPos,       1, 8,        0},
      { OP_Integer,     0, 2,        0},
      { OP_Subtract,    1, 2,        1},
      { OP_IfPos,       1, 8,        0},
      { OP_Integer,     0, 1,        0},                         /* 6 */
      { OP_Noop,        0, 0,        0},
      { OP_ResultRow,   1, 1,        0},
    };
    VdbeOp *aOp;
    capdbVdbeUsesBtree(v, iDb);
    if( !zRight ){
      pParse->nMem += 2;
      capdbVdbeVerifyNoMallocRequired(v, ArraySize(getCacheSize));
      aOp = capdbVdbeAddOpList(v, ArraySize(getCacheSize), getCacheSize, iLn);
      if( ONLY_IF_REALLOC_STRESS(aOp==0) ) break;
      aOp[0].p1 = iDb;
      aOp[1].p1 = iDb;
      aOp[6].p1 = CAPDB_DEFAULT_CACHE_SIZE;
    }else{
      int size = capdbAbsInt32(capdbAtoi(zRight));
      capdbBeginWriteOperation(pParse, 0, iDb);
      capdbVdbeAddOp3(v, OP_SetCookie, iDb, BTREE_DEFAULT_CACHE_SIZE, size);
      assert( capdbSchemaMutexHeld(db, iDb, 0) );
      pDb->pSchema->cache_size = size;
      capdbBtreeSetCacheSize(pDb->pBt, pDb->pSchema->cache_size);
    }
    break;
  }
#endif /* !CAPDB_OMIT_PAGER_PRAGMAS && !CAPDB_OMIT_DEPRECATED */

#if !defined(CAPDB_OMIT_PAGER_PRAGMAS)
  /*
  **  PRAGMA [schema.]page_size
  **  PRAGMA [schema.]page_size=N
  **
  ** The first form reports the current setting for the
  ** database page size in bytes.  The second form sets the
  ** database page size value.  The value can only be set if
  ** the database has not yet been created.
  */
  case PragTyp_PAGE_SIZE: {
    Btree *pBt = pDb->pBt;
    assert( pBt!=0 );
    if( !zRight ){
      int size = ALWAYS(pBt) ? capdbBtreeGetPageSize(pBt) : 0;
      returnSingleInt(v, size);
    }else{
      /* Malloc may fail when setting the page-size, as there is an internal
      ** buffer that the pager module resizes using capdb_realloc().
      */
      db->nextPagesize = capdbAtoi(zRight);
      if( CAPDB_NOMEM==capdbBtreeSetPageSize(pBt, db->nextPagesize,0,0) ){
        capdbOomFault(db);
      }
    }
    break;
  }

  /*
  **  PRAGMA [schema.]secure_delete
  **  PRAGMA [schema.]secure_delete=ON/OFF/FAST
  **
  ** The first form reports the current setting for the
  ** secure_delete flag.  The second form changes the secure_delete
  ** flag setting and reports the new value.
  */
  case PragTyp_SECURE_DELETE: {
    Btree *pBt = pDb->pBt;
    int b = -1;
    assert( pBt!=0 );
    if( zRight ){
      if( capdb_stricmp(zRight, "fast")==0 ){
        b = 2;
      }else{
        b = capdbGetBoolean(zRight, 0);
      }
    }
    if( pId2->n==0 && b>=0 ){
      int ii;
      for(ii=0; ii<db->nDb; ii++){
        capdbBtreeSecureDelete(db->aDb[ii].pBt, b);
      }
    }
    b = capdbBtreeSecureDelete(pBt, b);
    returnSingleInt(v, b);
    break;
  }

  /*
  **  PRAGMA [schema.]max_page_count
  **  PRAGMA [schema.]max_page_count=N
  **
  ** The first form reports the current setting for the
  ** maximum number of pages in the database file.  The
  ** second form attempts to change this setting.  Both
  ** forms return the current setting.
  **
  ** The absolute value of N is used.  This is undocumented and might
  ** change.  The only purpose is to provide an easy way to test
  ** the capdbAbsInt32() function.
  **
  **  PRAGMA [schema.]page_count
  **
  ** Return the number of pages in the specified database.
  */
  case PragTyp_PAGE_COUNT: {
    int iReg;
    i64 x = 0;
    capdbCodeVerifySchema(pParse, iDb);
    iReg = ++pParse->nMem;
    if( capdbTolower(zLeft[0])=='p' ){
      capdbVdbeAddOp2(v, OP_Pagecount, iDb, iReg);
    }else{
      if( zRight && capdbDecOrHexToI64(zRight,&x)==0 ){
        if( x<0 ) x = 0;
        else if( x>0xfffffffe ) x = 0xfffffffe;
      }else{
        x = 0;
      }
      capdbVdbeAddOp3(v, OP_MaxPgcnt, iDb, iReg, (int)x);
    }
    capdbVdbeAddOp2(v, OP_ResultRow, iReg, 1);
    break;
  }

  /*
  **  PRAGMA [schema.]locking_mode
  **  PRAGMA [schema.]locking_mode = (normal|exclusive)
  */
  case PragTyp_LOCKING_MODE: {
    const char *zRet = "normal";
    int eMode = getLockingMode(zRight);

    if( pId2->n==0 && eMode==PAGER_LOCKINGMODE_QUERY ){
      /* Simple "PRAGMA locking_mode;" statement. This is a query for
      ** the current default locking mode (which may be different to
      ** the locking-mode of the main database).
      */
      eMode = db->dfltLockMode;
    }else{
      Pager *pPager;
      if( pId2->n==0 ){
        /* This indicates that no database name was specified as part
        ** of the PRAGMA command. In this case the locking-mode must be
        ** set on all attached databases, as well as the main db file.
        **
        ** Also, the capdb.dfltLockMode variable is set so that
        ** any subsequently attached databases also use the specified
        ** locking mode.
        */
        int ii;
        assert(pDb==&db->aDb[0]);
        for(ii=2; ii<db->nDb; ii++){
          pPager = capdbBtreePager(db->aDb[ii].pBt);
          capdbPagerLockingMode(pPager, eMode);
        }
        db->dfltLockMode = (u8)eMode;
      }
      pPager = capdbBtreePager(pDb->pBt);
      eMode = capdbPagerLockingMode(pPager, eMode);
    }

    assert( eMode==PAGER_LOCKINGMODE_NORMAL
            || eMode==PAGER_LOCKINGMODE_EXCLUSIVE );
    if( eMode==PAGER_LOCKINGMODE_EXCLUSIVE ){
      zRet = "exclusive";
    }
    returnSingleText(v, zRet);
    break;
  }

  /*
  **  PRAGMA [schema.]journal_mode
  **  PRAGMA [schema.]journal_mode =
  **                      (delete|persist|off|truncate|memory|wal|off)
  */
  case PragTyp_JOURNAL_MODE: {
    int eMode;        /* One of the PAGER_JOURNALMODE_XXX symbols */
    int ii;           /* Loop counter */

    if( zRight==0 ){
      /* If there is no "=MODE" part of the pragma, do a query for the
      ** current mode */
      eMode = PAGER_JOURNALMODE_QUERY;
    }else{
      const char *zMode;
      int n = capdbStrlen30(zRight);
      for(eMode=0; (zMode = capdbJournalModename(eMode))!=0; eMode++){
        if( capdbStrNICmp(zRight, zMode, n)==0 ) break;
      }
      if( !zMode ){
        /* If the "=MODE" part does not match any known journal mode,
        ** then do a query */
        eMode = PAGER_JOURNALMODE_QUERY;
      }
      if( eMode==PAGER_JOURNALMODE_OFF && (db->flags & CAPDB_Defensive)!=0 ){
        /* Do not allow journal-mode "OFF" in defensive since the database
        ** can become corrupted using ordinary SQL when the journal is off */
        eMode = PAGER_JOURNALMODE_QUERY;
      }
    }
    if( eMode==PAGER_JOURNALMODE_QUERY && pId2->n==0 ){
      /* Convert "PRAGMA journal_mode" into "PRAGMA main.journal_mode" */
      iDb = 0;
      pId2->n = 1;
    }
    for(ii=db->nDb-1; ii>=0; ii--){
      if( db->aDb[ii].pBt && (ii==iDb || pId2->n==0) ){
        capdbVdbeUsesBtree(v, ii);
        capdbVdbeAddOp3(v, OP_JournalMode, ii, 1, eMode);
      }
    }
    capdbVdbeAddOp2(v, OP_ResultRow, 1, 1);
    break;
  }

  /*
  **  PRAGMA [schema.]journal_size_limit
  **  PRAGMA [schema.]journal_size_limit=N
  **
  ** Get or set the size limit on rollback journal files.
  */
  case PragTyp_JOURNAL_SIZE_LIMIT: {
    Pager *pPager = capdbBtreePager(pDb->pBt);
    i64 iLimit = -2;
    if( zRight ){
      capdbDecOrHexToI64(zRight, &iLimit);
      if( iLimit<-1 ) iLimit = -1;
    }
    iLimit = capdbPagerJournalSizeLimit(pPager, iLimit);
    returnSingleInt(v, iLimit);
    break;
  }

#endif /* CAPDB_OMIT_PAGER_PRAGMAS */

  /*
  **  PRAGMA [schema.]auto_vacuum
  **  PRAGMA [schema.]auto_vacuum=N
  **
  ** Get or set the value of the database 'auto-vacuum' parameter.
  ** The value is one of:  0 NONE 1 FULL 2 INCREMENTAL
  */
#ifndef CAPDB_OMIT_AUTOVACUUM
  case PragTyp_AUTO_VACUUM: {
    Btree *pBt = pDb->pBt;
    assert( pBt!=0 );
    if( !zRight ){
      returnSingleInt(v, capdbBtreeGetAutoVacuum(pBt));
    }else{
      int eAuto = getAutoVacuum(zRight);
      assert( eAuto>=0 && eAuto<=2 );
      db->nextAutovac = (u8)eAuto;
      /* Call SetAutoVacuum() to set initialize the internal auto and
      ** incr-vacuum flags. This is required in case this connection
      ** creates the database file. It is important that it is created
      ** as an auto-vacuum capable db.
      */
      rc = capdbBtreeSetAutoVacuum(pBt, eAuto);
      if( rc==CAPDB_OK && (eAuto==1 || eAuto==2) ){
        /* When setting the auto_vacuum mode to either "full" or
        ** "incremental", write the value of meta[6] in the database
        ** file. Before writing to meta[6], check that meta[3] indicates
        ** that this really is an auto-vacuum capable database.
        */
        static const int iLn = VDBE_OFFSET_LINENO(2);
        static const VdbeOpList setMeta6[] = {
          { OP_Transaction,    0,         1,                 0},    /* 0 */
          { OP_ReadCookie,     0,         1,         BTREE_LARGEST_ROOT_PAGE},
          { OP_If,             1,         0,                 0},    /* 2 */
          { OP_Halt,           CAPDB_OK, OE_Abort,          0},    /* 3 */
          { OP_SetCookie,      0,         BTREE_INCR_VACUUM, 0},    /* 4 */
        };
        VdbeOp *aOp;
        int iAddr = capdbVdbeCurrentAddr(v);
        capdbVdbeVerifyNoMallocRequired(v, ArraySize(setMeta6));
        aOp = capdbVdbeAddOpList(v, ArraySize(setMeta6), setMeta6, iLn);
        if( ONLY_IF_REALLOC_STRESS(aOp==0) ) break;
        aOp[0].p1 = iDb;
        aOp[1].p1 = iDb;
        aOp[2].p2 = iAddr+4;
        aOp[4].p1 = iDb;
        aOp[4].p3 = eAuto - 1;
        capdbVdbeUsesBtree(v, iDb);
      }
    }
    break;
  }
#endif

  /*
  **  PRAGMA [schema.]incremental_vacuum(N)
  **
  ** Do N steps of incremental vacuuming on a database.
  */
#ifndef CAPDB_OMIT_AUTOVACUUM
  case PragTyp_INCREMENTAL_VACUUM: {
    int iLimit = 0, addr;
    if( zRight==0 || !capdbGetInt32(zRight, &iLimit) || iLimit<=0 ){
      iLimit = 0x7fffffff;
    }
    capdbBeginWriteOperation(pParse, 0, iDb);
    capdbVdbeAddOp2(v, OP_Integer, iLimit, 1);
    addr = capdbVdbeAddOp1(v, OP_IncrVacuum, iDb); VdbeCoverage(v);
    capdbVdbeAddOp1(v, OP_ResultRow, 1);
    capdbVdbeAddOp2(v, OP_AddImm, 1, -1);
    capdbVdbeAddOp2(v, OP_IfPos, 1, addr); VdbeCoverage(v);
    capdbVdbeJumpHere(v, addr);
    break;
  }
#endif

#ifndef CAPDB_OMIT_PAGER_PRAGMAS
  /*
  **  PRAGMA [schema.]cache_size
  **  PRAGMA [schema.]cache_size=N
  **
  ** The first form reports the current local setting for the
  ** page cache size. The second form sets the local
  ** page cache size value.  If N is positive then that is the
  ** number of pages in the cache.  If N is negative, then the
  ** number of pages is adjusted so that the cache uses -N kibibytes
  ** of memory.
  */
  case PragTyp_CACHE_SIZE: {
    assert( capdbSchemaMutexHeld(db, iDb, 0) );
    if( !zRight ){
      returnSingleInt(v, pDb->pSchema->cache_size);
    }else{
      int size = capdbAtoi(zRight);
      pDb->pSchema->cache_size = size;
      capdbBtreeSetCacheSize(pDb->pBt, pDb->pSchema->cache_size);
    }
    break;
  }

  /*
  **  PRAGMA [schema.]cache_spill
  **  PRAGMA cache_spill=BOOLEAN
  **  PRAGMA [schema.]cache_spill=N
  **
  ** The first form reports the current local setting for the
  ** page cache spill size. The second form turns cache spill on
  ** or off.  When turning cache spill on, the size is set to the
  ** current cache_size.  The third form sets a spill size that
  ** may be different form the cache size.
  ** If N is positive then that is the
  ** number of pages in the cache.  If N is negative, then the
  ** number of pages is adjusted so that the cache uses -N kibibytes
  ** of memory.
  **
  ** If the number of cache_spill pages is less then the number of
  ** cache_size pages, no spilling occurs until the page count exceeds
  ** the number of cache_size pages.
  **
  ** The cache_spill=BOOLEAN setting applies to all attached schemas,
  ** not just the schema specified.
  */
  case PragTyp_CACHE_SPILL: {
    assert( capdbSchemaMutexHeld(db, iDb, 0) );
    if( !zRight ){
      returnSingleInt(v,
         (db->flags & CAPDB_CacheSpill)==0 ? 0 :
            capdbBtreeSetSpillSize(pDb->pBt,0));
    }else{
      int size = 1;
      if( capdbGetInt32(zRight, &size) ){
        capdbBtreeSetSpillSize(pDb->pBt, size);
      }
      if( capdbGetBoolean(zRight, size!=0) ){
        db->flags |= CAPDB_CacheSpill;
      }else{
        db->flags &= ~(u64)CAPDB_CacheSpill;
      }
      setAllPagerFlags(db);
    }
    break;
  }

  /*
  **  PRAGMA [schema.]mmap_size(N)
  **
  ** Used to set mapping size limit. The mapping size limit is
  ** used to limit the aggregate size of all memory mapped regions of the
  ** database file. If this parameter is set to zero, then memory mapping
  ** is not used at all.  If N is negative, then the default memory map
  ** limit determined by capdb_config(CAPDB_CONFIG_MMAP_SIZE) is set.
  ** The parameter N is measured in bytes.
  **
  ** This value is advisory.  The underlying VFS is free to memory map
  ** as little or as much as it wants.  Except, if N is set to 0 then the
  ** upper layers will never invoke the xFetch interfaces to the VFS.
  */
  case PragTyp_MMAP_SIZE: {
    capdb_int64 sz;
#if CAPDB_MAX_MMAP_SIZE>0
    assert( capdbSchemaMutexHeld(db, iDb, 0) );
    if( zRight ){
      int ii;
      capdbDecOrHexToI64(zRight, &sz);
      if( sz<0 ) sz = capdbGlobalConfig.szMmap;
      if( pId2->n==0 ) db->szMmap = sz;
      for(ii=db->nDb-1; ii>=0; ii--){
        if( db->aDb[ii].pBt && (ii==iDb || pId2->n==0) ){
          capdbBtreeSetMmapLimit(db->aDb[ii].pBt, sz);
        }
      }
    }
    sz = -1;
    rc = capdb_file_control(db, zDb, CAPDB_FCNTL_MMAP_SIZE, &sz);
#else
    sz = 0;
    rc = CAPDB_OK;
#endif
    if( rc==CAPDB_OK ){
      returnSingleInt(v, sz);
    }else if( rc!=CAPDB_NOTFOUND ){
      pParse->nErr++;
      pParse->rc = rc;
    }
    break;
  }

  /*
  **   PRAGMA temp_store
  **   PRAGMA temp_store = "default"|"memory"|"file"
  **
  ** Return or set the local value of the temp_store flag.  Changing
  ** the local value does not make changes to the disk file and the default
  ** value will be restored the next time the database is opened.
  **
  ** Note that it is possible for the library compile-time options to
  ** override this setting
  */
  case PragTyp_TEMP_STORE: {
    if( !zRight ){
      returnSingleInt(v, db->temp_store);
    }else{
      changeTempStorage(pParse, zRight);
    }
    break;
  }

  /*
  **   PRAGMA temp_store_directory
  **   PRAGMA temp_store_directory = ""|"directory_name"
  **
  ** Return or set the local value of the temp_store_directory flag.  Changing
  ** the value sets a specific directory to be used for temporary files.
  ** Setting to a null string reverts to the default temporary directory search.
  ** If temporary directory is changed, then invalidateTempStorage.
  **
  */
  case PragTyp_TEMP_STORE_DIRECTORY: {
    capdb_mutex_enter(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
    if( !zRight ){
      returnSingleText(v, capdb_temp_directory);
    }else{
#ifndef CAPDB_OMIT_WSD
      if( zRight[0] ){
        int res;
        rc = capdbOsAccess(db->pVfs, zRight, CAPDB_ACCESS_READWRITE, &res);
        if( rc!=CAPDB_OK || res==0 ){
          capdbErrorMsg(pParse, "not a writable directory");
          capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
          goto pragma_out;
        }
      }
      if( CAPDB_TEMP_STORE==0
       || (CAPDB_TEMP_STORE==1 && db->temp_store<=1)
       || (CAPDB_TEMP_STORE==2 && db->temp_store==1)
      ){
        invalidateTempStorage(pParse);
      }
      capdb_free(capdb_temp_directory);
      if( zRight[0] ){
        capdb_temp_directory = capdb_mprintf("%s", zRight);
      }else{
        capdb_temp_directory = 0;
      }
#endif /* CAPDB_OMIT_WSD */
    }
    capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
    break;
  }

#if CAPDB_OS_WIN
  /*
  **   PRAGMA data_store_directory
  **   PRAGMA data_store_directory = ""|"directory_name"
  **
  ** Return or set the local value of the data_store_directory flag.  Changing
  ** the value sets a specific directory to be used for database files that
  ** were specified with a relative pathname.  Setting to a null string reverts
  ** to the default database directory, which for database files specified with
  ** a relative path will probably be based on the current directory for the
  ** process.  Database file specified with an absolute path are not impacted
  ** by this setting, regardless of its value.
  **
  */
  case PragTyp_DATA_STORE_DIRECTORY: {
    capdb_mutex_enter(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
    if( !zRight ){
      returnSingleText(v, capdb_data_directory);
    }else{
#ifndef CAPDB_OMIT_WSD
      if( zRight[0] ){
        int res;
        rc = capdbOsAccess(db->pVfs, zRight, CAPDB_ACCESS_READWRITE, &res);
        if( rc!=CAPDB_OK || res==0 ){
          capdbErrorMsg(pParse, "not a writable directory");
          capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
          goto pragma_out;
        }
      }
      capdb_free(capdb_data_directory);
      if( zRight[0] ){
        capdb_data_directory = capdb_mprintf("%s", zRight);
      }else{
        capdb_data_directory = 0;
      }
#endif /* CAPDB_OMIT_WSD */
    }
    capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
    break;
  }
#endif

#if CAPDB_ENABLE_LOCKING_STYLE
  /*
  **   PRAGMA [schema.]lock_proxy_file
  **   PRAGMA [schema.]lock_proxy_file = ":auto:"|"lock_file_path"
  **
  ** Return or set the value of the lock_proxy_file flag.  Changing
  ** the value sets a specific file to be used for database access locks.
  **
  */
  case PragTyp_LOCK_PROXY_FILE: {
    if( !zRight ){
      Pager *pPager = capdbBtreePager(pDb->pBt);
      char *proxy_file_path = NULL;
      capdb_file *pFile = capdbPagerFile(pPager);
      capdbOsFileControlHint(pFile, CAPDB_GET_LOCKPROXYFILE,
                           &proxy_file_path);
      returnSingleText(v, proxy_file_path);
    }else{
      Pager *pPager = capdbBtreePager(pDb->pBt);
      capdb_file *pFile = capdbPagerFile(pPager);
      int res;
      if( zRight[0] ){
        res=capdbOsFileControl(pFile, CAPDB_SET_LOCKPROXYFILE,
                                     zRight);
      } else {
        res=capdbOsFileControl(pFile, CAPDB_SET_LOCKPROXYFILE,
                                     NULL);
      }
      if( res!=CAPDB_OK ){
        capdbErrorMsg(pParse, "failed to set lock proxy file");
        goto pragma_out;
      }
    }
    break;
  }
#endif /* CAPDB_ENABLE_LOCKING_STYLE */     
   
  /*
  **   PRAGMA [schema.]synchronous
  **   PRAGMA [schema.]synchronous=OFF|ON|NORMAL|FULL|EXTRA
  **
  ** Return or set the local value of the synchronous flag.  Changing
  ** the local value does not make changes to the disk file and the
  ** default value will be restored the next time the database is
  ** opened.
  */
  case PragTyp_SYNCHRONOUS: {
    if( !zRight ){
      returnSingleInt(v, pDb->safety_level-1);
    }else{
      if( !db->autoCommit ){
        capdbErrorMsg(pParse,
            "Safety level may not be changed inside a transaction");
      }else if( iDb!=1 ){
        int iLevel = (getSafetyLevel(zRight,0,1)+1) & PAGER_SYNCHRONOUS_MASK;
        if( iLevel==0 ) iLevel = 1;
        pDb->safety_level = iLevel;
        pDb->bSyncSet = 1;
        setAllPagerFlags(db);
      }
    }
    break;
  }
#endif /* CAPDB_OMIT_PAGER_PRAGMAS */

#ifndef CAPDB_OMIT_FLAG_PRAGMAS
  case PragTyp_FLAG: {
    if( zRight==0 ){
      setPragmaResultColumnNames(v, pPragma);
      returnSingleInt(v, (db->flags & pPragma->iArg)!=0 );
    }else{
      u64 mask = pPragma->iArg;    /* Mask of bits to set or clear. */
      if( db->autoCommit==0 ){
        /* Foreign key support may not be enabled or disabled while not
        ** in auto-commit mode.  */
        mask &= ~(CAPDB_ForeignKeys);
      }

      if( capdbGetBoolean(zRight, 0) ){
        if( (mask & CAPDB_WriteSchema)==0
         || (db->flags & CAPDB_Defensive)==0
        ){
          db->flags |= mask;
        }
      }else{
        db->flags &= ~mask;
        if( mask==CAPDB_DeferFKs ){
          db->nDeferredImmCons = 0;
          db->nDeferredCons = 0;
        }
        if( (mask & CAPDB_WriteSchema)!=0
         && capdb_stricmp(zRight, "reset")==0
        ){
          /* IMP: R-60817-01178 If the argument is "RESET" then schema
          ** writing is disabled (as with "PRAGMA writable_schema=OFF") and,
          ** in addition, the schema is reloaded. */
          capdbResetAllSchemasOfConnection(db);
        }
      }

      /* Many of the flag-pragmas modify the code generated by the SQL
      ** compiler (eg. count_changes). So add an opcode to expire all
      ** compiled SQL statements after modifying a pragma value.
      */
      capdbVdbeAddOp0(v, OP_Expire);
      setAllPagerFlags(db);
    }
    break;
  }
#endif /* CAPDB_OMIT_FLAG_PRAGMAS */

#ifndef CAPDB_OMIT_SCHEMA_PRAGMAS
  /*
  **   PRAGMA table_info(<table>)
  **
  ** Return a single row for each column of the named table. The columns of
  ** the returned data set are:
  **
  ** cid:        Column id (numbered from left to right, starting at 0)
  ** name:       Column name
  ** type:       Column declaration type.
  ** notnull:    True if 'NOT NULL' is part of column declaration
  ** dflt_value: The default value for the column, if any.
  ** pk:         Non-zero for PK fields.
  */
  case PragTyp_TABLE_INFO: if( zRight ){
    Table *pTab;
    capdbCodeVerifyNamedSchema(pParse, zDb);
    pTab = capdbLocateTable(pParse, LOCATE_NOERR, zRight, zDb);
    if( pTab ){
      int i, k;
      int nHidden = 0;
      Column *pCol;
      Index *pPk = capdbPrimaryKeyIndex(pTab);
      pParse->nMem = 7;
      capdbViewGetColumnNames(pParse, pTab);
      for(i=0, pCol=pTab->aCol; i<pTab->nCol; i++, pCol++){
        int isHidden = 0;
        const Expr *pColExpr;
        if( pCol->colFlags & COLFLAG_NOINSERT ){
          if( pPragma->iArg==0 ){
            nHidden++;
            continue;
          }
          if( pCol->colFlags & COLFLAG_VIRTUAL ){
            isHidden = 2;  /* GENERATED ALWAYS AS ... VIRTUAL */
          }else if( pCol->colFlags & COLFLAG_STORED ){
            isHidden = 3;  /* GENERATED ALWAYS AS ... STORED */
          }else{ assert( pCol->colFlags & COLFLAG_HIDDEN );
            isHidden = 1;  /* HIDDEN */
          }
        }
        if( (pCol->colFlags & COLFLAG_PRIMKEY)==0 ){
          k = 0;
        }else if( pPk==0 ){
          k = 1;
        }else{
          for(k=1; k<=pTab->nCol && pPk->aiColumn[k-1]!=i; k++){}
        }
        pColExpr = capdbColumnExpr(pTab,pCol);
        assert( pColExpr==0 || pColExpr->op==TK_SPAN || isHidden>=2 );
        assert( pColExpr==0 || !ExprHasProperty(pColExpr, EP_IntValue)
                  || isHidden>=2 );
        capdbVdbeMultiLoad(v, 1, pPragma->iArg ? "issisii" : "issisi",
               i-nHidden,
               pCol->zCnName,
               capdbColumnType(pCol,""),
               pCol->notNull ? 1 : 0,
               (isHidden>=2 || pColExpr==0) ? 0 : pColExpr->u.zToken,
               k,
               isHidden);
      }
    }
  }
  break;

  /*
  **   PRAGMA table_list
  **
  ** Return a single row for each table, virtual table, or view in the
  ** entire schema.
  **
  ** schema:     Name of attached database hold this table
  ** name:       Name of the table itself
  ** type:       "table", "view", "virtual", "shadow"
  ** ncol:       Number of columns
  ** wr:         True for a WITHOUT ROWID table
  ** strict:     True for a STRICT table
  */
  case PragTyp_TABLE_LIST: {
    int ii;
    pParse->nMem = 6;
    capdbCodeVerifyNamedSchema(pParse, zDb);
    for(ii=0; ii<db->nDb; ii++){
      HashElem *k;
      Hash *pHash;
      int initNCol;
      if( zDb && capdb_stricmp(zDb, db->aDb[ii].zDbSName)!=0 ) continue;

      /* Ensure that the Table.nCol field is initialized for all views
      ** and virtual tables.  Each time we initialize a Table.nCol value
      ** for a table, that can potentially disrupt the hash table, so restart
      ** the initialization scan.
      */
      pHash = &db->aDb[ii].pSchema->tblHash;
      initNCol = sqliteHashCount(pHash);
      while( initNCol-- ){
        for(k=sqliteHashFirst(pHash); 1; k=sqliteHashNext(k) ){
          Table *pTab;
          if( k==0 ){ initNCol = 0; break; }
          pTab = sqliteHashData(k);
          if( pTab->nCol==0 ){
            char *zSql = capdbMPrintf(db, "SELECT*FROM\"%w\"", pTab->zName);
            if( zSql ){
              capdb_stmt *pDummy = 0;
              (void)capdb_prepare_v3(db, zSql, -1, CAPDB_PREPARE_DONT_LOG,
                                       &pDummy, 0);
              (void)capdb_finalize(pDummy);
              capdbDbFree(db, zSql);
            }
            if( db->mallocFailed ){
              capdbErrorMsg(db->pParse, "out of memory");
              db->pParse->rc = CAPDB_NOMEM_BKPT;
            }
            pHash = &db->aDb[ii].pSchema->tblHash;
            break;
          }
        }
      }

      for(k=sqliteHashFirst(pHash); k; k=sqliteHashNext(k) ){
        Table *pTab = sqliteHashData(k);
        const char *zType;
        if( zRight && capdb_stricmp(zRight, pTab->zName)!=0 ) continue;
        if( IsView(pTab) ){
          zType = "view";
        }else if( IsVirtual(pTab) ){
          zType = "virtual";
        }else if( pTab->tabFlags & TF_Shadow ){
          zType = "shadow";
        }else{
          zType = "table";
        }
        capdbVdbeMultiLoad(v, 1, "sssiii",
           db->aDb[ii].zDbSName,
           capdbPreferredTableName(pTab->zName),
           zType,
           pTab->nCol,
           (pTab->tabFlags & TF_WithoutRowid)!=0,
           (pTab->tabFlags & TF_Strict)!=0
        );
      }
    }
  }
  break;

#ifdef CAPDB_DEBUG
  case PragTyp_STATS: {
    Index *pIdx;
    HashElem *i;
    pParse->nMem = 5;
    capdbCodeVerifySchema(pParse, iDb);
    for(i=sqliteHashFirst(&pDb->pSchema->tblHash); i; i=sqliteHashNext(i)){
      Table *pTab = sqliteHashData(i);
      capdbVdbeMultiLoad(v, 1, "ssiii",
           capdbPreferredTableName(pTab->zName),
           0,
           pTab->szTabRow,
           pTab->nRowLogEst,
           pTab->tabFlags);
      for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
        capdbVdbeMultiLoad(v, 2, "siiiX",
           pIdx->zName,
           pIdx->szIdxRow,
           pIdx->aiRowLogEst[0],
           pIdx->hasStat1);
        capdbVdbeAddOp2(v, OP_ResultRow, 1, 5);
      }
    }
  }
  break;
#endif

  case PragTyp_INDEX_INFO: if( zRight ){
    Index *pIdx;
    Table *pTab;
    pIdx = capdbFindIndex(db, zRight, zDb);
    if( pIdx==0 ){
      /* If there is no index named zRight, check to see if there is a
      ** WITHOUT ROWID table named zRight, and if there is, show the
      ** structure of the PRIMARY KEY index for that table. */
      pTab = capdbLocateTable(pParse, LOCATE_NOERR, zRight, zDb);
      if( pTab && !HasRowid(pTab) ){
        pIdx = capdbPrimaryKeyIndex(pTab);
      }
    }
    if( pIdx ){
      int iIdxDb = capdbSchemaToIndex(db, pIdx->pSchema);
      int i;
      int mx;
      if( pPragma->iArg ){
        /* PRAGMA index_xinfo (newer version with more rows and columns) */
        mx = pIdx->nColumn;
        pParse->nMem = 6;
      }else{
        /* PRAGMA index_info (legacy version) */
        mx = pIdx->nKeyCol;
        pParse->nMem = 3;
      }
      pTab = pIdx->pTable;
      capdbCodeVerifySchema(pParse, iIdxDb);
      assert( pParse->nMem<=pPragma->nPragCName );
      for(i=0; i<mx; i++){
        i16 cnum = pIdx->aiColumn[i];
        capdbVdbeMultiLoad(v, 1, "iisX", i, cnum,
                             cnum<0 ? 0 : pTab->aCol[cnum].zCnName);
        if( pPragma->iArg ){
          capdbVdbeMultiLoad(v, 4, "isiX",
            pIdx->aSortOrder[i],
            pIdx->azColl[i],
            i<pIdx->nKeyCol);
        }
        capdbVdbeAddOp2(v, OP_ResultRow, 1, pParse->nMem);
      }
    }
  }
  break;

  case PragTyp_INDEX_LIST: if( zRight ){
    Index *pIdx;
    Table *pTab;
    int i;
    pTab = capdbFindTable(db, zRight, zDb);
    if( pTab ){
      int iTabDb = capdbSchemaToIndex(db, pTab->pSchema);
      pParse->nMem = 5;
      capdbCodeVerifySchema(pParse, iTabDb);
      for(pIdx=pTab->pIndex, i=0; pIdx; pIdx=pIdx->pNext, i++){
        const char *azOrigin[] = { "c", "u", "pk" };
        capdbVdbeMultiLoad(v, 1, "isisi",
           i,
           pIdx->zName,
           IsUniqueIndex(pIdx),
           azOrigin[pIdx->idxType],
           pIdx->pPartIdxWhere!=0);
      }
    }
  }
  break;

  case PragTyp_DATABASE_LIST: {
    int i;
    pParse->nMem = 3;
    for(i=0; i<db->nDb; i++){
      if( db->aDb[i].pBt==0 ) continue;
      assert( db->aDb[i].zDbSName!=0 );
      capdbVdbeMultiLoad(v, 1, "iss",
         i,
         db->aDb[i].zDbSName,
         capdbBtreeGetFilename(db->aDb[i].pBt));
    }
  }
  break;

  case PragTyp_COLLATION_LIST: {
    int i = 0;
    HashElem *p;
    pParse->nMem = 2;
    for(p=sqliteHashFirst(&db->aCollSeq); p; p=sqliteHashNext(p)){
      CollSeq *pColl = (CollSeq *)sqliteHashData(p);
      capdbVdbeMultiLoad(v, 1, "is", i++, pColl->zName);
    }
  }
  break;

#ifndef CAPDB_OMIT_INTROSPECTION_PRAGMAS
  case PragTyp_FUNCTION_LIST: {
    int i;
    HashElem *j;
    FuncDef *p;
    int showInternFunc = (db->mDbFlags & DBFLAG_InternalFunc)!=0;
    pParse->nMem = 6;
    for(i=0; i<CAPDB_FUNC_HASH_SZ; i++){
      for(p=capdbBuiltinFunctions.a[i]; p; p=p->u.pHash ){
        assert( p->funcFlags & CAPDB_FUNC_BUILTIN );
        pragmaFunclistLine(v, p, 1, showInternFunc);
      }
    }
    for(j=sqliteHashFirst(&db->aFunc); j; j=sqliteHashNext(j)){
      p = (FuncDef*)sqliteHashData(j);
      assert( (p->funcFlags & CAPDB_FUNC_BUILTIN)==0 );
      pragmaFunclistLine(v, p, 0, showInternFunc);
    }
  }
  break;

#ifndef CAPDB_OMIT_VIRTUALTABLE
  case PragTyp_MODULE_LIST: {
    HashElem *j;
    pParse->nMem = 1;
    for(j=sqliteHashFirst(&db->aModule); j; j=sqliteHashNext(j)){
      Module *pMod = (Module*)sqliteHashData(j);
      capdbVdbeMultiLoad(v, 1, "s", pMod->zName);
    }
  }
  break;
#endif /* CAPDB_OMIT_VIRTUALTABLE */

  case PragTyp_PRAGMA_LIST: {
    int i;
    for(i=0; i<ArraySize(aPragmaName); i++){
      capdbVdbeMultiLoad(v, 1, "s", aPragmaName[i].zName);
    }
  }
  break;
#endif /* CAPDB_INTROSPECTION_PRAGMAS */

#endif /* CAPDB_OMIT_SCHEMA_PRAGMAS */

#ifndef CAPDB_OMIT_FOREIGN_KEY
  case PragTyp_FOREIGN_KEY_LIST: if( zRight ){
    FKey *pFK;
    Table *pTab;
    pTab = capdbFindTable(db, zRight, zDb);
    if( pTab && IsOrdinaryTable(pTab) ){
      pFK = pTab->u.tab.pFKey;
      if( pFK ){
        int iTabDb = capdbSchemaToIndex(db, pTab->pSchema);
        int i = 0;
        pParse->nMem = 8;
        capdbCodeVerifySchema(pParse, iTabDb);
        while(pFK){
          int j;
          for(j=0; j<pFK->nCol; j++){
            capdbVdbeMultiLoad(v, 1, "iissssss",
                   i,
                   j,
                   pFK->zTo,
                   pTab->aCol[pFK->aCol[j].iFrom].zCnName,
                   pFK->aCol[j].zCol,
                   actionName(pFK->aAction[1]),  /* ON UPDATE */
                   actionName(pFK->aAction[0]),  /* ON DELETE */
                   "NONE");
          }
          ++i;
          pFK = pFK->pNextFrom;
        }
      }
    }
  }
  break;
#endif /* !defined(CAPDB_OMIT_FOREIGN_KEY) */

#ifndef CAPDB_OMIT_FOREIGN_KEY
#ifndef CAPDB_OMIT_TRIGGER
  case PragTyp_FOREIGN_KEY_CHECK: {
    FKey *pFK;             /* A foreign key constraint */
    Table *pTab;           /* Child table contain "REFERENCES" keyword */
    Table *pParent;        /* Parent table that child points to */
    Index *pIdx;           /* Index in the parent table */
    int i;                 /* Loop counter:  Foreign key number for pTab */
    int j;                 /* Loop counter:  Field of the foreign key */
    HashElem *k;           /* Loop counter:  Next table in schema */
    int x;                 /* result variable */
    int regResult;         /* 3 registers to hold a result row */
    int regRow;            /* Registers to hold a row from pTab */
    int addrTop;           /* Top of a loop checking foreign keys */
    int addrOk;            /* Jump here if the key is OK */
    int *aiCols;           /* child to parent column mapping */

    regResult = pParse->nMem+1;
    pParse->nMem += 4;
    regRow = ++pParse->nMem;
    k = sqliteHashFirst(&db->aDb[iDb].pSchema->tblHash);
    while( k ){
      if( zRight ){
        pTab = capdbLocateTable(pParse, 0, zRight, zDb);
        k = 0;
      }else{
        pTab = (Table*)sqliteHashData(k);
        k = sqliteHashNext(k);
      }
      if( pTab==0 || !IsOrdinaryTable(pTab) || pTab->u.tab.pFKey==0 ) continue;
      iDb = capdbSchemaToIndex(db, pTab->pSchema);
      zDb = db->aDb[iDb].zDbSName;
      capdbCodeVerifySchema(pParse, iDb);
      capdbTableLock(pParse, iDb, pTab->tnum, 0, pTab->zName);
      capdbTouchRegister(pParse, pTab->nCol+regRow);
      capdbOpenTable(pParse, 0, iDb, pTab, OP_OpenRead);
      capdbVdbeLoadString(v, regResult, pTab->zName);
      assert( IsOrdinaryTable(pTab) );
      for(i=1, pFK=pTab->u.tab.pFKey; pFK; i++, pFK=pFK->pNextFrom){
        pParent = capdbFindTable(db, pFK->zTo, zDb);
        if( pParent==0 ) continue;
        pIdx = 0;
        capdbTableLock(pParse, iDb, pParent->tnum, 0, pParent->zName);
        x = capdbFkLocateIndex(pParse, pParent, pFK, &pIdx, 0);
        if( x==0 ){
          if( pIdx==0 ){
            capdbOpenTable(pParse, i, iDb, pParent, OP_OpenRead);
          }else{
            capdbVdbeAddOp3(v, OP_OpenRead, i, pIdx->tnum, iDb);
            capdbVdbeSetP4KeyInfo(pParse, pIdx);
          }
        }else{
          k = 0;
          break;
        }
      }
      assert( pParse->nErr>0 || pFK==0 );
      if( pFK ) break;
      if( pParse->nTab<i ) pParse->nTab = i;
      addrTop = capdbVdbeAddOp1(v, OP_Rewind, 0); VdbeCoverage(v);
      assert( IsOrdinaryTable(pTab) );
      for(i=1, pFK=pTab->u.tab.pFKey; pFK; i++, pFK=pFK->pNextFrom){
        pParent = capdbFindTable(db, pFK->zTo, zDb);
        pIdx = 0;
        aiCols = 0;
        if( pParent ){
          x = capdbFkLocateIndex(pParse, pParent, pFK, &pIdx, &aiCols);
          assert( x==0 || db->mallocFailed );
        }
        addrOk = capdbVdbeMakeLabel(pParse);

        /* Generate code to read the child key values into registers
        ** regRow..regRow+n. If any of the child key values are NULL, this
        ** row cannot cause an FK violation. Jump directly to addrOk in
        ** this case. */
        capdbTouchRegister(pParse, regRow + pFK->nCol);
        for(j=0; j<pFK->nCol; j++){
          int iCol = aiCols ? aiCols[j] : pFK->aCol[j].iFrom;
          capdbExprCodeGetColumnOfTable(v, pTab, 0, iCol, regRow+j);
          capdbVdbeAddOp2(v, OP_IsNull, regRow+j, addrOk); VdbeCoverage(v);
        }

        /* Generate code to query the parent index for a matching parent
        ** key. If a match is found, jump to addrOk. */
        if( pIdx ){
          capdbVdbeAddOp4(v, OP_Affinity, regRow, pFK->nCol, 0,
              capdbIndexAffinityStr(db,pIdx), pFK->nCol);
          capdbVdbeAddOp4Int(v, OP_Found, i, addrOk, regRow, pFK->nCol);
          VdbeCoverage(v);
        }else if( pParent ){
          int jmp = capdbVdbeCurrentAddr(v)+2;
          capdbVdbeAddOp3(v, OP_SeekRowid, i, jmp, regRow); VdbeCoverage(v);
          capdbVdbeGoto(v, addrOk);
          assert( pFK->nCol==1 || db->mallocFailed );
        }

        /* Generate code to report an FK violation to the caller. */
        if( HasRowid(pTab) ){
          capdbVdbeAddOp2(v, OP_Rowid, 0, regResult+1);
        }else{
          capdbVdbeAddOp2(v, OP_Null, 0, regResult+1);
        }
        capdbVdbeMultiLoad(v, regResult+2, "siX", pFK->zTo, i-1);
        capdbVdbeAddOp2(v, OP_ResultRow, regResult, 4);
        capdbVdbeResolveLabel(v, addrOk);
        capdbDbFree(db, aiCols);
      }
      capdbVdbeAddOp2(v, OP_Next, 0, addrTop+1); VdbeCoverage(v);
      capdbVdbeJumpHere(v, addrTop);
    }
  }
  break;
#endif /* !defined(CAPDB_OMIT_TRIGGER) */
#endif /* !defined(CAPDB_OMIT_FOREIGN_KEY) */

#ifndef CAPDB_OMIT_CASE_SENSITIVE_LIKE_PRAGMA
  /* Reinstall the LIKE and GLOB functions.  The variant of LIKE
  ** used will be case sensitive or not depending on the RHS.
  */
  case PragTyp_CASE_SENSITIVE_LIKE: {
    if( zRight ){
      capdbRegisterLikeFunctions(db, capdbGetBoolean(zRight, 0));
    }
  }
  break;
#endif /* CAPDB_OMIT_CASE_SENSITIVE_LIKE_PRAGMA */

#ifndef CAPDB_INTEGRITY_CHECK_ERROR_MAX
# define CAPDB_INTEGRITY_CHECK_ERROR_MAX 100
#endif

#ifndef CAPDB_OMIT_INTEGRITY_CHECK
  /*    PRAGMA integrity_check
  **    PRAGMA integrity_check(N)
  **    PRAGMA quick_check
  **    PRAGMA quick_check(N)
  **
  ** Verify the integrity of the database.
  **
  ** The "quick_check" is reduced version of
  ** integrity_check designed to detect most database corruption
  ** without the overhead of cross-checking indexes.  Quick_check
  ** is linear time whereas integrity_check is O(NlogN).
  **
  ** The maximum number of errors is 100 by default.  A different default
  ** can be specified using a numeric parameter N.
  **
  ** Or, the parameter N can be the name of a table.  In that case, only
  ** the one table named is verified.  The freelist is only verified if
  ** the named table is "sqlite_schema" (or one of its aliases).
  **
  ** All schemas are checked by default.  To check just a single
  ** schema, use the form:
  **
  **      PRAGMA schema.integrity_check;
  */
  case PragTyp_INTEGRITY_CHECK: {
    int i, j, addr, mxErr;
    Table *pObjTab = 0;     /* Check only this one table, if not NULL */

    int isQuick = (capdbTolower(zLeft[0])=='q');

    /* If the PRAGMA command was of the form "PRAGMA <db>.integrity_check",
    ** then iDb is set to the index of the database identified by <db>.
    ** In this case, the integrity of database iDb only is verified by
    ** the VDBE created below.
    **
    ** Otherwise, if the command was simply "PRAGMA integrity_check" (or
    ** "PRAGMA quick_check"), then iDb is set to 0. In this case, set iDb
    ** to -1 here, to indicate that the VDBE should verify the integrity
    ** of all attached databases.  */
    assert( iDb>=0 );
    assert( iDb==0 || pId2->z );
    if( pId2->z==0 ) iDb = -1;

    /* Initialize the VDBE program */
    pParse->nMem = 6;

    /* Set the maximum error count */
    mxErr = CAPDB_INTEGRITY_CHECK_ERROR_MAX;
    if( zRight ){
      if( capdbGetInt32(pValue->z, &mxErr) ){
        if( mxErr<=0 ){
          mxErr = CAPDB_INTEGRITY_CHECK_ERROR_MAX;
        }
      }else{
        pObjTab = capdbLocateTable(pParse, 0, zRight,
                      iDb>=0 ? db->aDb[iDb].zDbSName : 0);
      }
    }
    capdbVdbeAddOp2(v, OP_Integer, mxErr-1, 1); /* reg[1] holds errors left */

    /* Do an integrity check on each database file */
    for(i=0; i<db->nDb; i++){
      HashElem *x;     /* For looping over tables in the schema */
      Hash *pTbls;     /* Set of all tables in the schema */
      int *aRoot;      /* Array of root page numbers of all btrees */
      int cnt = 0;     /* Number of entries in aRoot[] */

      if( OMIT_TEMPDB && i==1 ) continue;
      if( iDb>=0 && i!=iDb ) continue;

      capdbCodeVerifySchema(pParse, i);
      pParse->okConstFactor = 0;  /* tag-20230327-1 */

      /* Do an integrity check of the B-Tree
      **
      ** Begin by finding the root pages numbers
      ** for all tables and indices in the database.
      */
      assert( capdbSchemaMutexHeld(db, i, 0) );
      pTbls = &db->aDb[i].pSchema->tblHash;
      for(cnt=0, x=sqliteHashFirst(pTbls); x; x=sqliteHashNext(x)){
        Table *pTab = sqliteHashData(x);  /* Current table */
        Index *pIdx;                      /* An index on pTab */
        int nIdx;                         /* Number of indexes on pTab */
        if( tableSkipIntegrityCheck(pTab,pObjTab) ) continue;
        if( HasRowid(pTab) ) cnt++;
        for(nIdx=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, nIdx++){ cnt++; }
      }
      if( cnt==0 ) continue;
      if( pObjTab ) cnt++;
      aRoot = capdbDbMallocRawNN(db, sizeof(int)*(cnt+1));
      if( aRoot==0 ) break;
      cnt = 0;
      if( pObjTab ) aRoot[++cnt] = 0;
      for(x=sqliteHashFirst(pTbls); x; x=sqliteHashNext(x)){
        Table *pTab = sqliteHashData(x);
        Index *pIdx;
        if( tableSkipIntegrityCheck(pTab,pObjTab) ) continue;
        if( HasRowid(pTab) ) aRoot[++cnt] = pTab->tnum;
        for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
          aRoot[++cnt] = pIdx->tnum;
        }
      }
      aRoot[0] = cnt;

      /* Make sure sufficient number of registers have been allocated */
      capdbTouchRegister(pParse, 8+cnt);
      capdbVdbeAddOp3(v, OP_Null, 0, 8, 8+cnt);
      capdbClearTempRegCache(pParse);

      /* Do the b-tree integrity checks */
      capdbVdbeAddOp4(v, OP_IntegrityCk, 1, cnt, 8, (char*)aRoot,P4_INTARRAY);
      capdbVdbeChangeP5(v, (u16)i);
      addr = capdbVdbeAddOp1(v, OP_IsNull, 2); VdbeCoverage(v);
      capdbVdbeAddOp4(v, OP_String8, 0, 3, 0,
         capdbMPrintf(db, "*** in database %s ***\n", db->aDb[i].zDbSName),
         P4_DYNAMIC);
      capdbVdbeAddOp3(v, OP_Concat, 2, 3, 3);
      integrityCheckResultRow(v);
      capdbVdbeJumpHere(v, addr);

      /* Check that the indexes all have the right number of rows */
      cnt = pObjTab ? 1 : 0;
      capdbVdbeLoadString(v, 2, "wrong # of entries in index ");
      for(x=sqliteHashFirst(pTbls); x; x=sqliteHashNext(x)){
        int iTab = 0;
        Table *pTab = sqliteHashData(x);
        Index *pIdx;
        if( tableSkipIntegrityCheck(pTab,pObjTab) ) continue;
        if( HasRowid(pTab) ){
          iTab = cnt++;
        }else{
          iTab = cnt;
          for(pIdx=pTab->pIndex; ALWAYS(pIdx); pIdx=pIdx->pNext){
            if( IsPrimaryKeyIndex(pIdx) ) break;
            iTab++;
          }
        }
        for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
          if( pIdx->pPartIdxWhere==0 ){
            addr = capdbVdbeAddOp3(v, OP_Eq, 8+cnt, 0, 8+iTab);
            VdbeCoverageNeverNull(v);
            capdbVdbeLoadString(v, 4, pIdx->zName);
            capdbVdbeAddOp3(v, OP_Concat, 4, 2, 3);
            integrityCheckResultRow(v);
            capdbVdbeJumpHere(v, addr);
          }
          cnt++;
        }
      }

      /* Make sure all the indices are constructed correctly.
      */
      for(x=sqliteHashFirst(pTbls); x; x=sqliteHashNext(x)){
        Table *pTab = sqliteHashData(x);
        Index *pIdx, *pPk;
        Index *pPrior = 0;      /* Previous index */
        int loopTop;
        int iDataCur, iIdxCur;
        int r1 = -1;
        int bStrict;            /* True for a STRICT table */
        int r2;                 /* Previous key for WITHOUT ROWID tables */
        int mxCol;              /* Maximum non-virtual column number */

        if( tableSkipIntegrityCheck(pTab,pObjTab) ) continue;
        if( !IsOrdinaryTable(pTab) ) continue;
        if( isQuick || HasRowid(pTab) ){
          pPk = 0;
          r2 = 0;
        }else{
          pPk = capdbPrimaryKeyIndex(pTab);
          r2 = capdbGetTempRange(pParse, pPk->nKeyCol);
          capdbVdbeAddOp3(v, OP_Null, 1, r2, r2+pPk->nKeyCol-1);
        }
        capdbOpenTableAndIndices(pParse, pTab, OP_OpenRead, 0,
                                   1, 0, &iDataCur, &iIdxCur);
        /* reg[7] counts the number of entries in the table.
        ** reg[8+i] counts the number of entries in the i-th index
        */
        capdbVdbeAddOp2(v, OP_Integer, 0, 7);
        for(j=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, j++){
          capdbVdbeAddOp2(v, OP_Integer, 0, 8+j); /* index entries counter */
        }
        assert( pParse->nMem>=8+j );
        assert( capdbNoTempsInRange(pParse,1,7+j) );
        capdbVdbeAddOp2(v, OP_Rewind, iDataCur, 0); VdbeCoverage(v);
        loopTop = capdbVdbeAddOp2(v, OP_AddImm, 7, 1);

        /* Fetch the right-most column from the table.  This will cause
        ** the entire record header to be parsed and sanity checked.  It
        ** will also prepopulate the cursor column cache that is used
        ** by the OP_IsType code, so it is a required step.
        */
        assert( !IsVirtual(pTab) );
        if( HasRowid(pTab) ){
          mxCol = -1;
          for(j=0; j<pTab->nCol; j++){
            if( (pTab->aCol[j].colFlags & COLFLAG_VIRTUAL)==0 ) mxCol++;
          }
          if( mxCol==pTab->iPKey ) mxCol--;
        }else{
          /* COLFLAG_VIRTUAL columns are not included in the WITHOUT ROWID
          ** PK index column-count, so there is no need to account for them
          ** in this case. */
          mxCol = capdbPrimaryKeyIndex(pTab)->nColumn-1;
        }
        if( mxCol>=0 ){
          capdbVdbeAddOp3(v, OP_Column, iDataCur, mxCol, 3);
          capdbVdbeTypeofColumn(v, 3);
        }

        if( !isQuick ){
          if( pPk ){
            /* Verify WITHOUT ROWID keys are in ascending order */
            int a1;
            char *zErr;
            a1 = capdbVdbeAddOp4Int(v, OP_IdxGT, iDataCur, 0,r2,pPk->nKeyCol);
            VdbeCoverage(v);
            capdbVdbeAddOp1(v, OP_IsNull, r2); VdbeCoverage(v);
            zErr = capdbMPrintf(db,
                   "row not in PRIMARY KEY order for %s",
                    pTab->zName);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0, zErr, P4_DYNAMIC);
            integrityCheckResultRow(v);
            capdbVdbeJumpHere(v, a1);
            capdbVdbeJumpHere(v, a1+1);
            for(j=0; j<pPk->nKeyCol; j++){
              capdbExprCodeLoadIndexColumn(pParse, pPk, iDataCur, j, r2+j);
            }
          }
        }
        /* Verify datatypes for all columns:
        **
        **   (1) NOT NULL columns may not contain a NULL
        **   (2) Datatype must be exact for non-ANY columns in STRICT tables
        **   (3) Datatype for TEXT columns in non-STRICT tables must be
        **       NULL, TEXT, or BLOB.
        **   (4) Datatype for numeric columns in non-STRICT tables must not
        **       be a TEXT value that can be losslessly converted to numeric.
        */
        bStrict = (pTab->tabFlags & TF_Strict)!=0;
        for(j=0; j<pTab->nCol; j++){
          char *zErr;
          Column *pCol = pTab->aCol + j;  /* The column to be checked */
          int labelError;               /* Jump here to report an error */
          int labelOk;                  /* Jump here if all looks ok */
          int p1, p3, p4;               /* Operands to the OP_IsType opcode */
          int doTypeCheck;              /* Check datatypes (besides NOT NULL) */

          if( j==pTab->iPKey ) continue;
          if( bStrict ){
            doTypeCheck = pCol->eCType>COLTYPE_ANY;
          }else{
            doTypeCheck = pCol->affinity>CAPDB_AFF_BLOB;
          }
          if( pCol->notNull==0 && !doTypeCheck ) continue;

          /* Compute the operands that will be needed for OP_IsType */
          p4 = CAPDB_NULL;
          if( pCol->colFlags & COLFLAG_VIRTUAL ){
            capdbExprCodeGetColumnOfTable(v, pTab, iDataCur, j, 3);
            p1 = -1;
            p3 = 3;
          }else{
            if( pCol->iDflt ){
              capdb_value *pDfltValue = 0;
              capdbValueFromExpr(db, capdbColumnExpr(pTab,pCol), ENC(db),
                                   pCol->affinity, &pDfltValue);
              if( pDfltValue ){
                p4 = capdb_value_type(pDfltValue);
                capdbValueFree(pDfltValue);
              }
            }
            p1 = iDataCur;
            if( !HasRowid(pTab) ){
              testcase( j!=capdbTableColumnToStorage(pTab, j) );
              p3 = capdbTableColumnToIndex(capdbPrimaryKeyIndex(pTab), j);
            }else{
              p3 = capdbTableColumnToStorage(pTab,j);
              testcase( p3!=j);
            }
          }

          labelError = capdbVdbeMakeLabel(pParse);
          labelOk = capdbVdbeMakeLabel(pParse);
          if( pCol->notNull ){
            /* (1) NOT NULL columns may not contain a NULL */
            int jmp3;
            int jmp2 = capdbVdbeAddOp4Int(v, OP_IsType, p1, labelOk, p3, p4);
            VdbeCoverage(v);
            if( p1<0 ){
              capdbVdbeChangeP5(v, 0x0f); /* INT, REAL, TEXT, or BLOB */
              jmp3 = jmp2;
            }else{
              capdbVdbeChangeP5(v, 0x0d); /* INT, TEXT, or BLOB */
              /* OP_IsType does not detect NaN values in the database file
              ** which should be treated as a NULL.  So if the header type
              ** is REAL, we have to load the actual data using OP_Column
              ** to reliably determine if the value is a NULL. */
              capdbVdbeAddOp3(v, OP_Column, p1, p3, 3);
              capdbColumnDefault(v, pTab, j, 3);
              jmp3 = capdbVdbeAddOp2(v, OP_NotNull, 3, labelOk);
              VdbeCoverage(v);
            }           
            zErr = capdbMPrintf(db, "NULL value in %s.%s", pTab->zName,
                                pCol->zCnName);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0, zErr, P4_DYNAMIC);
            if( doTypeCheck ){
              capdbVdbeGoto(v, labelError);
              capdbVdbeJumpHere(v, jmp2);
              capdbVdbeJumpHere(v, jmp3);
            }else{
              /* VDBE byte code will fall thru */
            }
          }
          if( bStrict && doTypeCheck ){
            /* (2) Datatype must be exact for non-ANY columns in STRICT tables*/
            static unsigned char aStdTypeMask[] = {
               0x1f,    /* ANY */
               0x18,    /* BLOB */
               0x11,    /* INT */
               0x11,    /* INTEGER */
               0x13,    /* REAL */
               0x14     /* TEXT */
            };
            capdbVdbeAddOp4Int(v, OP_IsType, p1, labelOk, p3, p4);
            assert( pCol->eCType>=1 && pCol->eCType<=sizeof(aStdTypeMask) );
            capdbVdbeChangeP5(v, aStdTypeMask[pCol->eCType-1]);
            VdbeCoverage(v);
            zErr = capdbMPrintf(db, "non-%s value in %s.%s",
                                  capdbStdType[pCol->eCType-1],
                                  pTab->zName, pTab->aCol[j].zCnName);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0, zErr, P4_DYNAMIC);
          }else if( !bStrict && pCol->affinity==CAPDB_AFF_TEXT ){
            /* (3) Datatype for TEXT columns in non-STRICT tables must be
            **     NULL, TEXT, or BLOB. */
            capdbVdbeAddOp4Int(v, OP_IsType, p1, labelOk, p3, p4);
            capdbVdbeChangeP5(v, 0x1c); /* NULL, TEXT, or BLOB */
            VdbeCoverage(v);
            zErr = capdbMPrintf(db, "NUMERIC value in %s.%s",
                                  pTab->zName, pTab->aCol[j].zCnName);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0, zErr, P4_DYNAMIC);
          }else if( !bStrict && pCol->affinity>=CAPDB_AFF_NUMERIC ){
            /* (4) Datatype for numeric columns in non-STRICT tables must not
            **     be a TEXT value that can be converted to numeric. */
            capdbVdbeAddOp4Int(v, OP_IsType, p1, labelOk, p3, p4);
            capdbVdbeChangeP5(v, 0x1b); /* NULL, INT, FLOAT, or BLOB */
            VdbeCoverage(v);
            if( p1>=0 ){
              capdbExprCodeGetColumnOfTable(v, pTab, iDataCur, j, 3);
            }
            capdbVdbeAddOp4(v, OP_Affinity, 3, 1, 0, "C", P4_STATIC);
            capdbVdbeAddOp4Int(v, OP_IsType, -1, labelOk, 3, p4);
            capdbVdbeChangeP5(v, 0x1c); /* NULL, TEXT, or BLOB */
            VdbeCoverage(v);
            zErr = capdbMPrintf(db, "TEXT value in %s.%s",
                                  pTab->zName, pTab->aCol[j].zCnName);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0, zErr, P4_DYNAMIC);
          }
          capdbVdbeResolveLabel(v, labelError);
          integrityCheckResultRow(v);
          capdbVdbeResolveLabel(v, labelOk);
        }
        /* Verify CHECK constraints */
        if( pTab->pCheck && (db->flags & CAPDB_IgnoreChecks)==0 ){
          ExprList *pCheck = capdbExprListDup(db, pTab->pCheck, 0);
          if( db->mallocFailed==0 ){
            int addrCkFault = capdbVdbeMakeLabel(pParse);
            int addrCkOk = capdbVdbeMakeLabel(pParse);
            char *zErr;
            int k;
            pParse->iSelfTab = iDataCur + 1;
            for(k=pCheck->nExpr-1; k>0; k--){
              capdbExprIfFalse(pParse, pCheck->a[k].pExpr, addrCkFault, 0);
            }
            capdbExprIfTrue(pParse, pCheck->a[0].pExpr, addrCkOk,
                CAPDB_JUMPIFNULL);
            capdbVdbeResolveLabel(v, addrCkFault);
            pParse->iSelfTab = 0;
            zErr = capdbMPrintf(db, "CHECK constraint failed in %s",
                pTab->zName);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0, zErr, P4_DYNAMIC);
            integrityCheckResultRow(v);
            capdbVdbeResolveLabel(v, addrCkOk);
          }
          capdbExprListDelete(db, pCheck);
        }
        if( !isQuick ){ /* Omit the remaining tests for quick_check */
          /* Validate index entries for the current row */
          for(j=0, pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext, j++){
            int jmp2, jmp3, jmp4, jmp5, label6;
            int kk;
            int ckUniq = capdbVdbeMakeLabel(pParse);
            if( pPk==pIdx ) continue;
            r1 = capdbGenerateIndexKey(pParse, pIdx, iDataCur, 0, 0, &jmp3,
                                         pPrior, r1);
            pPrior = pIdx;
            capdbVdbeAddOp2(v, OP_AddImm, 8+j, 1);/* increment entry count */
            /* Verify that an index entry exists for the current table row */
            capdbVdbeAddOp4Int(v, OP_Found, iIdxCur+j, ckUniq, r1,
                                        pIdx->nColumn); VdbeCoverage(v);
            jmp2 = capdbVdbeAddOp3(v, OP_IFindKey, iIdxCur+j, ckUniq, r1); 
            VdbeCoverage(v);
            capdbVdbeChangeP4(v, -1, (const char*)pIdx, P4_INDEX);
            capdbVdbeAddOp4(v, OP_String8, 0, 3, 0,
              capdbMPrintf(db, "index %s stores an imprecise floating-point "
                                 "value for row ", pIdx->zName),
              P4_DYNAMIC);
            capdbVdbeAddOp3(v, OP_Concat, 7, 3, 3);
            integrityCheckResultRow(v);
            capdbVdbeAddOp2(v, OP_Goto, 0, ckUniq);

            capdbVdbeJumpHere(v, jmp2);
            capdbVdbeLoadString(v, 3, "row ");
            capdbVdbeAddOp3(v, OP_Concat, 7, 3, 3);
            capdbVdbeLoadString(v, 4, " missing from index ");
            capdbVdbeAddOp3(v, OP_Concat, 4, 3, 3);
            jmp5 = capdbVdbeLoadString(v, 4, pIdx->zName);
            capdbVdbeAddOp3(v, OP_Concat, 4, 3, 3);
            jmp4 = integrityCheckResultRow(v);
            capdbVdbeResolveLabel(v, ckUniq);

            /* The OP_IdxRowid opcode is an optimized version of OP_Column
            ** that extracts the rowid off the end of the index record.
            ** But it only works correctly if index record does not have
            ** any extra bytes at the end.  Verify that this is the case. */
            if( HasRowid(pTab) ){
              int jmp7;
              capdbVdbeAddOp2(v, OP_IdxRowid, iIdxCur+j, 3);
              jmp7 = capdbVdbeAddOp3(v, OP_Eq, 3, 0, r1+pIdx->nColumn-1);
              VdbeCoverageNeverNull(v);
              capdbVdbeLoadString(v, 3,
                 "rowid not at end-of-record for row ");
              capdbVdbeAddOp3(v, OP_Concat, 7, 3, 3);
              capdbVdbeLoadString(v, 4, " of index ");
              capdbVdbeGoto(v, jmp5-1);
              capdbVdbeJumpHere(v, jmp7);
            }

            /* Any indexed columns with non-BINARY collations must still hold
            ** the exact same text value as the table. */
            label6 = 0;
            for(kk=0; kk<pIdx->nKeyCol; kk++){
              if( pIdx->azColl[kk]==capdbStrBINARY ) continue;
              if( label6==0 ) label6 = capdbVdbeMakeLabel(pParse);
              capdbVdbeAddOp3(v, OP_Column, iIdxCur+j, kk, 3);
              capdbVdbeAddOp3(v, OP_Ne, 3, label6, r1+kk); VdbeCoverage(v);
            }
            if( label6 ){
              int jmp6 = capdbVdbeAddOp0(v, OP_Goto);
              capdbVdbeResolveLabel(v, label6);
              capdbVdbeLoadString(v, 3, "row ");
              capdbVdbeAddOp3(v, OP_Concat, 7, 3, 3);
              capdbVdbeLoadString(v, 4, " values differ from index ");
              capdbVdbeGoto(v, jmp5-1);
              capdbVdbeJumpHere(v, jmp6);
            }
             
            /* For UNIQUE indexes, verify that only one entry exists with the
            ** current key.  The entry is unique if (1) any column is NULL
            ** or (2) the next entry has a different key */
            if( IsUniqueIndex(pIdx) ){
              int uniqOk = capdbVdbeMakeLabel(pParse);
              int jmp6;
              for(kk=0; kk<pIdx->nKeyCol; kk++){
                int iCol = pIdx->aiColumn[kk];
                assert( iCol!=XN_ROWID && iCol<pTab->nCol );
                if( iCol>=0 && pTab->aCol[iCol].notNull ) continue;
                capdbVdbeAddOp2(v, OP_IsNull, r1+kk, uniqOk);
                VdbeCoverage(v);
              }
              jmp6 = capdbVdbeAddOp1(v, OP_Next, iIdxCur+j); VdbeCoverage(v);
              capdbVdbeGoto(v, uniqOk);
              capdbVdbeJumpHere(v, jmp6);
              capdbVdbeAddOp4Int(v, OP_IdxGT, iIdxCur+j, uniqOk, r1,
                                   pIdx->nKeyCol); VdbeCoverage(v);
              capdbVdbeLoadString(v, 3, "non-unique entry in index ");
              capdbVdbeGoto(v, jmp5);
              capdbVdbeResolveLabel(v, uniqOk);
            }
            capdbVdbeJumpHere(v, jmp4);
            capdbResolvePartIdxLabel(pParse, jmp3);
          }
        }
        capdbVdbeAddOp2(v, OP_Next, iDataCur, loopTop); VdbeCoverage(v);
        capdbVdbeJumpHere(v, loopTop-1);
        if( pPk ){
          assert( !isQuick );
          capdbReleaseTempRange(pParse, r2, pPk->nKeyCol);
        }
      }

#ifndef CAPDB_OMIT_VIRTUALTABLE
      /* Second pass to invoke the xIntegrity method on all virtual
      ** tables.
      */
      for(x=sqliteHashFirst(pTbls); x; x=sqliteHashNext(x)){
        Table *pTab = sqliteHashData(x);
        capdb_vtab *pVTab;
        int a1;
        if( tableSkipIntegrityCheck(pTab,pObjTab) ) continue;
        if( IsOrdinaryTable(pTab) ) continue;
        if( !IsVirtual(pTab) ) continue;
        if( pTab->nCol<=0 ){
          const char *zMod = pTab->u.vtab.azArg[0];
          if( capdbHashFind(&db->aModule, zMod)==0 ) continue;
        }
        capdbViewGetColumnNames(pParse, pTab);
        if( pTab->u.vtab.p==0 ) continue;
        pVTab = pTab->u.vtab.p->pVtab;
        if( NEVER(pVTab==0) ) continue;
        if( NEVER(pVTab->pModule==0) ) continue;
        if( pVTab->pModule->iVersion<4 ) continue;
        if( pVTab->pModule->xIntegrity==0 ) continue;
        capdbVdbeAddOp3(v, OP_VCheck, i, 3, isQuick);
        pTab->nTabRef++;
        capdbVdbeAppendP4(v, pTab, P4_TABLEREF);
        a1 = capdbVdbeAddOp1(v, OP_IsNull, 3); VdbeCoverage(v);
        integrityCheckResultRow(v);
        capdbVdbeJumpHere(v, a1);
        continue;
      }
#endif
    }
    {
      static const int iLn = VDBE_OFFSET_LINENO(2);
      static const VdbeOpList endCode[] = {
        { OP_AddImm,      1, 0,        0},    /* 0 */
        { OP_IfNotZero,   1, 4,        0},    /* 1 */
        { OP_String8,     0, 3,        0},    /* 2 */
        { OP_ResultRow,   3, 1,        0},    /* 3 */
        { OP_Halt,        0, 0,        0},    /* 4 */
        { OP_String8,     0, 3,        0},    /* 5 */
        { OP_Goto,        0, 3,        0},    /* 6 */
      };
      VdbeOp *aOp;

      aOp = capdbVdbeAddOpList(v, ArraySize(endCode), endCode, iLn);
      if( aOp ){
        aOp[0].p2 = 1-mxErr;
        aOp[2].p4type = P4_STATIC;
        aOp[2].p4.z = "ok";
        aOp[5].p4type = P4_STATIC;
        aOp[5].p4.z = (char*)capdbErrStr(CAPDB_CORRUPT);
      }
      capdbVdbeChangeP3(v, 0, capdbVdbeCurrentAddr(v)-2);
    }
  }
  break;
#endif /* CAPDB_OMIT_INTEGRITY_CHECK */

#ifndef CAPDB_OMIT_UTF16
  /*
  **   PRAGMA encoding
  **   PRAGMA encoding = "utf-8"|"utf-16"|"utf-16le"|"utf-16be"
  **
  ** In its first form, this pragma returns the encoding of the main
  ** database. If the database is not initialized, it is initialized now.
  **
  ** The second form of this pragma is a no-op if the main database file
  ** has not already been initialized. In this case it sets the default
  ** encoding that will be used for the main database file if a new file
  ** is created. If an existing main database file is opened, then the
  ** default text encoding for the existing database is used.
  **
  ** In all cases new databases created using the ATTACH command are
  ** created to use the same default text encoding as the main database. If
  ** the main database has not been initialized and/or created when ATTACH
  ** is executed, this is done before the ATTACH operation.
  **
  ** In the second form this pragma sets the text encoding to be used in
  ** new database files created using this database handle. It is only
  ** useful if invoked immediately after the main database i
  */
  case PragTyp_ENCODING: {
    static const struct EncName {
      char *zName;
      u8 enc;
    } encnames[] = {
      { "UTF8",     CAPDB_UTF8        },
      { "UTF-8",    CAPDB_UTF8        },  /* Must be element [1] */
      { "UTF-16le", CAPDB_UTF16LE     },  /* Must be element [2] */
      { "UTF-16be", CAPDB_UTF16BE     },  /* Must be element [3] */
      { "UTF16le",  CAPDB_UTF16LE     },
      { "UTF16be",  CAPDB_UTF16BE     },
      { "UTF-16",   0                  }, /* CAPDB_UTF16NATIVE */
      { "UTF16",    0                  }, /* CAPDB_UTF16NATIVE */
      { 0, 0 }
    };
    const struct EncName *pEnc;
    if( !zRight ){    /* "PRAGMA encoding" */
      if( capdbReadSchema(pParse) ) goto pragma_out;
      assert( encnames[CAPDB_UTF8].enc==CAPDB_UTF8 );
      assert( encnames[CAPDB_UTF16LE].enc==CAPDB_UTF16LE );
      assert( encnames[CAPDB_UTF16BE].enc==CAPDB_UTF16BE );
      returnSingleText(v, encnames[ENC(pParse->db)].zName);
    }else{                        /* "PRAGMA encoding = XXX" */
      /* Only change the value of sqlite.enc if the database handle is not
      ** initialized. If the main database exists, the new sqlite.enc value
      ** will be overwritten when the schema is next loaded. If it does not
      ** already exists, it will be created to use the new encoding value.
      */
      if( (db->mDbFlags & DBFLAG_EncodingFixed)==0 ){
        for(pEnc=&encnames[0]; pEnc->zName; pEnc++){
          if( 0==capdbStrICmp(zRight, pEnc->zName) ){
            u8 enc = pEnc->enc ? pEnc->enc : CAPDB_UTF16NATIVE;
            SCHEMA_ENC(db) = enc;
            capdbSetTextEncoding(db, enc);
            break;
          }
        }
        if( !pEnc->zName ){
          capdbErrorMsg(pParse, "unsupported encoding: %s", zRight);
        }
      }
    }
  }
  break;
#endif /* CAPDB_OMIT_UTF16 */

#ifndef CAPDB_OMIT_SCHEMA_VERSION_PRAGMAS
  /*
  **   PRAGMA [schema.]schema_version
  **   PRAGMA [schema.]schema_version = <integer>
  **
  **   PRAGMA [schema.]user_version
  **   PRAGMA [schema.]user_version = <integer>
  **
  **   PRAGMA [schema.]freelist_count
  **
  **   PRAGMA [schema.]data_version
  **
  **   PRAGMA [schema.]application_id
  **   PRAGMA [schema.]application_id = <integer>
  **
  ** The pragma's schema_version and user_version are used to set or get
  ** the value of the schema-version and user-version, respectively. Both
  ** the schema-version and the user-version are 32-bit signed integers
  ** stored in the database header.
  **
  ** The schema-cookie is usually only manipulated internally by SQLite. It
  ** is incremented by SQLite whenever the database schema is modified (by
  ** creating or dropping a table or index). The schema version is used by
  ** SQLite each time a query is executed to ensure that the internal cache
  ** of the schema used when compiling the SQL query matches the schema of
  ** the database against which the compiled query is actually executed.
  ** Subverting this mechanism by using "PRAGMA schema_version" to modify
  ** the schema-version is potentially dangerous and may lead to program
  ** crashes or database corruption. Use with caution!
  **
  ** The user-version is not used internally by SQLite. It may be used by
  ** applications for any purpose.
  */
  case PragTyp_HEADER_VALUE: {
    int iCookie = pPragma->iArg;  /* Which cookie to read or write */
    capdbVdbeUsesBtree(v, iDb);
    if( zRight && (pPragma->mPragFlg & PragFlg_ReadOnly)==0 ){
      /* Write the specified cookie value */
      static const VdbeOpList setCookie[] = {
        { OP_Transaction,    0,  1,  0},    /* 0 */
        { OP_SetCookie,      0,  0,  0},    /* 1 */
      };
      VdbeOp *aOp;
      capdbVdbeVerifyNoMallocRequired(v, ArraySize(setCookie));
      aOp = capdbVdbeAddOpList(v, ArraySize(setCookie), setCookie, 0);
      if( ONLY_IF_REALLOC_STRESS(aOp==0) ) break;
      aOp[0].p1 = iDb;
      aOp[1].p1 = iDb;
      aOp[1].p2 = iCookie;
      aOp[1].p3 = capdbAtoi(zRight);
      aOp[1].p5 = 1;
      if( iCookie==BTREE_SCHEMA_VERSION && (db->flags & CAPDB_Defensive)!=0 ){
        /* Do not allow the use of PRAGMA schema_version=VALUE in defensive
        ** mode.  Change the OP_SetCookie opcode into a no-op.  */
        aOp[1].opcode = OP_Noop;
      }
    }else{
      /* Read the specified cookie value */
      static const VdbeOpList readCookie[] = {
        { OP_Transaction,     0,  0,  0},    /* 0 */
        { OP_ReadCookie,      0,  1,  0},    /* 1 */
        { OP_ResultRow,       1,  1,  0}
      };
      VdbeOp *aOp;
      capdbVdbeVerifyNoMallocRequired(v, ArraySize(readCookie));
      aOp = capdbVdbeAddOpList(v, ArraySize(readCookie),readCookie,0);
      if( ONLY_IF_REALLOC_STRESS(aOp==0) ) break;
      aOp[0].p1 = iDb;
      aOp[1].p1 = iDb;
      aOp[1].p3 = iCookie;
      capdbVdbeReusable(v);
    }
  }
  break;
#endif /* CAPDB_OMIT_SCHEMA_VERSION_PRAGMAS */

#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
  /*
  **   PRAGMA compile_options
  **
  ** Return the names of all compile-time options used in this build,
  ** one option per row.
  */
  case PragTyp_COMPILE_OPTIONS: {
    int i = 0;
    const char *zOpt;
    pParse->nMem = 1;
    while( (zOpt = capdb_compileoption_get(i++))!=0 ){
      capdbVdbeLoadString(v, 1, zOpt);
      capdbVdbeAddOp2(v, OP_ResultRow, 1, 1);
    }
    capdbVdbeReusable(v);
  }
  break;
#endif /* CAPDB_OMIT_COMPILEOPTION_DIAGS */

#ifndef CAPDB_OMIT_WAL
  /*
  **   PRAGMA [schema.]wal_checkpoint = passive|full|restart|truncate
  **
  ** Checkpoint the database.
  */
  case PragTyp_WAL_CHECKPOINT: {
    int iBt = (pId2->z?iDb:CAPDB_MAX_DB);
    int eMode = CAPDB_CHECKPOINT_PASSIVE;
    if( zRight ){
      if( capdbStrICmp(zRight, "full")==0 ){
        eMode = CAPDB_CHECKPOINT_FULL;
      }else if( capdbStrICmp(zRight, "restart")==0 ){
        eMode = CAPDB_CHECKPOINT_RESTART;
      }else if( capdbStrICmp(zRight, "truncate")==0 ){
        eMode = CAPDB_CHECKPOINT_TRUNCATE;
      }else if( capdbStrICmp(zRight, "noop")==0 ){
        eMode = CAPDB_CHECKPOINT_NOOP;
      }
    }
    pParse->nMem = 3;
    capdbVdbeAddOp3(v, OP_Checkpoint, iBt, eMode, 1);
    capdbVdbeAddOp2(v, OP_ResultRow, 1, 3);
  }
  break;

  /*
  **   PRAGMA wal_autocheckpoint
  **   PRAGMA wal_autocheckpoint = N
  **
  ** Configure a database connection to automatically checkpoint a database
  ** after accumulating N frames in the log. Or query for the current value
  ** of N.
  */
  case PragTyp_WAL_AUTOCHECKPOINT: {
    if( zRight ){
      capdb_wal_autocheckpoint(db, capdbAtoi(zRight));
    }
    returnSingleInt(v,
       db->xWalCallback==capdbWalDefaultHook ?
           CAPDB_PTR_TO_INT(db->pWalArg) : 0);
  }
  break;
#endif

  /*
  **  PRAGMA shrink_memory
  **
  ** IMPLEMENTATION-OF: R-23445-46109 This pragma causes the database
  ** connection on which it is invoked to free up as much memory as it
  ** can, by calling capdb_db_release_memory().
  */
  case PragTyp_SHRINK_MEMORY: {
    capdb_db_release_memory(db);
    break;
  }

  /*
  **  PRAGMA optimize
  **  PRAGMA optimize(MASK)
  **  PRAGMA schema.optimize
  **  PRAGMA schema.optimize(MASK)
  **
  ** Attempt to optimize the database.  All schemas are optimized in the first
  ** two forms, and only the specified schema is optimized in the latter two.
  **
  ** The details of optimizations performed by this pragma are expected
  ** to change and improve over time.  Applications should anticipate that
  ** this pragma will perform new optimizations in future releases.
  **
  ** The optional argument is a bitmask of optimizations to perform:
  **
  **    0x00001    Debugging mode.  Do not actually perform any optimizations
  **               but instead return one line of text for each optimization
  **               that would have been done.  Off by default.
  **
  **    0x00002    Run ANALYZE on tables that might benefit.  On by default.
  **               See below for additional information.
  **
  **    0x00010    Run all ANALYZE operations using an analysis_limit that
  **               is the lessor of the current analysis_limit and the
  **               CAPDB_DEFAULT_OPTIMIZE_LIMIT compile-time option.
  **               The default value of CAPDB_DEFAULT_OPTIMIZE_LIMIT is
  **               currently (2024-02-19) set to 2000, which is such that
  **               the worst case run-time for PRAGMA optimize on a 100MB
  **               database will usually be less than 100 milliseconds on
  **               a RaspberryPI-4 class machine.  On by default.
  **
  **    0x10000    Look at tables to see if they need to be reanalyzed
  **               due to growth or shrinkage even if they have not been
  **               queried during the current connection.  Off by default.
  **
  ** The default MASK is and always shall be 0x0fffe.  In the current
  ** implementation, the default mask only covers the 0x00002 optimization,
  ** though additional optimizations that are covered by 0x0fffe might be
  ** added in the future.  Optimizations that are off by default and must
  ** be explicitly requested have masks of 0x10000 or greater.
  **
  ** DETERMINATION OF WHEN TO RUN ANALYZE
  **
  ** In the current implementation, a table is analyzed if only if all of
  ** the following are true:
  **
  ** (1) MASK bit 0x00002 is set.
  **
  ** (2) The table is an ordinary table, not a virtual table or view.
  **
  ** (3) The table name does not begin with "sqlite_".
  **
  ** (4) One or more of the following is true:
  **      (4a) The 0x10000 MASK bit is set.
  **      (4b) One or more indexes on the table lacks an entry
  **           in the sqlite_stat1 table.
  **      (4c) The query planner used sqlite_stat1-style statistics for one
  **           or more indexes of the table at some point during the lifetime
  **           of the current connection.
  **
  ** (5) One or more of the following is true:
  **      (5a) One or more indexes on the table lacks an entry
  **           in the sqlite_stat1 table.  (Same as 4a)
  **      (5b) The number of rows in the table has increased or decreased by
  **           10-fold.  In other words, the current size of the table is
  **           10 times larger than the size in sqlite_stat1 or else the
  **           current size is less than 1/10th the size in sqlite_stat1.
  **
  ** The rules for when tables are analyzed are likely to change in
  ** future releases.  Future versions of SQLite might accept a string
  ** literal argument to this pragma that contains a mnemonic description
  ** of the options rather than a bitmap.
  */
  case PragTyp_OPTIMIZE: {
    int iDbLast;           /* Loop termination point for the schema loop */
    int iTabCur;           /* Cursor for a table whose size needs checking */
    HashElem *k;           /* Loop over tables of a schema */
    Schema *pSchema;       /* The current schema */
    Table *pTab;           /* A table in the schema */
    Index *pIdx;           /* An index of the table */
    LogEst szThreshold;    /* Size threshold above which reanalysis needed */
    char *zSubSql;         /* SQL statement for the OP_SqlExec opcode */
    u32 opMask;            /* Mask of operations to perform */
    int nLimit;            /* Analysis limit to use */
    int nCheck = 0;        /* Number of tables to be optimized */
    int nBtree = 0;        /* Number of btrees to scan */
    int nIndex;            /* Number of indexes on the current table */

    if( zRight ){
      opMask = (u32)capdbAtoi(zRight);
      if( (opMask & 0x02)==0 ) break;
    }else{
      opMask = 0xfffe;
    }
    if( (opMask & 0x10)==0 ){
      nLimit = 0;
    }else if( db->nAnalysisLimit>0
           && db->nAnalysisLimit<CAPDB_DEFAULT_OPTIMIZE_LIMIT ){
      nLimit = 0;
    }else{
      nLimit = CAPDB_DEFAULT_OPTIMIZE_LIMIT;
    }
    iTabCur = pParse->nTab++;
    for(iDbLast = zDb?iDb:db->nDb-1; iDb<=iDbLast; iDb++){
      if( iDb==1 ) continue;
      capdbCodeVerifySchema(pParse, iDb);
      pSchema = db->aDb[iDb].pSchema;
      for(k=sqliteHashFirst(&pSchema->tblHash); k; k=sqliteHashNext(k)){
        pTab = (Table*)sqliteHashData(k);

        /* This only works for ordinary tables */
        if( !IsOrdinaryTable(pTab) ) continue;

        /* Do not scan system tables */
        if( 0==capdbStrNICmp(pTab->zName, "sqlite_", 7) ) continue;

        /* Find the size of the table as last recorded in sqlite_stat1.
        ** If any index is unanalyzed, then the threshold is -1 to
        ** indicate a new, unanalyzed index
        */
        szThreshold = pTab->nRowLogEst;
        nIndex = 0;
        for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
          nIndex++;
          if( !pIdx->hasStat1 ){
            szThreshold = -1; /* Always analyze if any index lacks statistics */
          }
        }

        /* If table pTab has not been used in a way that would benefit from
        ** having analysis statistics during the current session, then skip it,
        ** unless the 0x10000 MASK bit is set. */
        if( (pTab->tabFlags & TF_MaybeReanalyze)!=0 ){
          /* Check for size change if stat1 has been used for a query */
        }else if( opMask & 0x10000 ){
          /* Check for size change if 0x10000 is set */
        }else if( pTab->pIndex!=0 && szThreshold<0 ){
          /* Do analysis if unanalyzed indexes exists */
        }else{
          /* Otherwise, we can skip this table */
          continue;
        }

        nCheck++;
        if( nCheck==2 ){
          /* If ANALYZE might be invoked two or more times, hold a write
          ** transaction for efficiency */
          capdbBeginWriteOperation(pParse, 0, iDb);
        }
        nBtree += nIndex+1;

        /* Reanalyze if the table is 10 times larger or smaller than
        ** the last analysis.  Unconditional reanalysis if there are
        ** unanalyzed indexes. */
        capdbOpenTable(pParse, iTabCur, iDb, pTab, OP_OpenRead);
        if( szThreshold>=0 ){
          const LogEst iRange = 33;   /* 10x size change */
          capdbVdbeAddOp4Int(v, OP_IfSizeBetween, iTabCur,
                         capdbVdbeCurrentAddr(v)+2+(opMask&1),
                         szThreshold>=iRange ? szThreshold-iRange : -1,
                         szThreshold+iRange);
          VdbeCoverage(v);
        }else{
          capdbVdbeAddOp2(v, OP_Rewind, iTabCur,
                         capdbVdbeCurrentAddr(v)+2+(opMask&1));
          VdbeCoverage(v);
        }
        zSubSql = capdbMPrintf(db, "ANALYZE \"%w\".\"%w\"",
                                 db->aDb[iDb].zDbSName, pTab->zName);
        if( opMask & 0x01 ){
          int r1 = capdbGetTempReg(pParse);
          capdbVdbeAddOp4(v, OP_String8, 0, r1, 0, zSubSql, P4_DYNAMIC);
          capdbVdbeAddOp2(v, OP_ResultRow, r1, 1);
        }else{
          capdbVdbeAddOp4(v, OP_SqlExec, nLimit ? 0x02 : 00, nLimit, 0,
                            zSubSql, P4_DYNAMIC);
        }
      }
    }
    capdbVdbeAddOp0(v, OP_Expire);

    /* In a schema with a large number of tables and indexes, scale back
    ** the analysis_limit to avoid excess run-time in the worst case.
    */
    if( !db->mallocFailed && nLimit>0 && nBtree>100 ){
      int iAddr, iEnd;
      VdbeOp *aOp;
      nLimit = 100*nLimit/nBtree;
      if( nLimit<100 ) nLimit = 100;
      aOp = capdbVdbeGetOp(v, 0);
      iEnd = capdbVdbeCurrentAddr(v);
      for(iAddr=0; iAddr<iEnd; iAddr++){
        if( aOp[iAddr].opcode==OP_SqlExec ) aOp[iAddr].p2 = nLimit;
      }
    }
    break;
  }

  /*
  **   PRAGMA busy_timeout
  **   PRAGMA busy_timeout = N
  **
  ** Call capdb_busy_timeout(db, N).  Return the current timeout value
  ** if one is set.  If no busy handler or a different busy handler is set
  ** then 0 is returned.  Setting the busy_timeout to 0 or negative
  ** disables the timeout.
  */
  /*case PragTyp_BUSY_TIMEOUT*/ default: {
    assert( pPragma->ePragTyp==PragTyp_BUSY_TIMEOUT );
    if( zRight ){
      capdb_busy_timeout(db, capdbAtoi(zRight));
    }
    returnSingleInt(v, db->busyTimeout);
    break;
  }

  /*
  **   PRAGMA soft_heap_limit
  **   PRAGMA soft_heap_limit = N
  **
  ** IMPLEMENTATION-OF: R-26343-45930 This pragma invokes the
  ** capdb_soft_heap_limit64() interface with the argument N, if N is
  ** specified and is a non-negative integer.
  ** IMPLEMENTATION-OF: R-64451-07163 The soft_heap_limit pragma always
  ** returns the same integer that would be returned by the
  ** capdb_soft_heap_limit64(-1) C-language function.
  */
  case PragTyp_SOFT_HEAP_LIMIT: {
    capdb_int64 N;
    if( zRight && capdbDecOrHexToI64(zRight, &N)==CAPDB_OK ){
      capdb_soft_heap_limit64(N);
    }
    returnSingleInt(v, capdb_soft_heap_limit64(-1));
    break;
  }

  /*
  **   PRAGMA hard_heap_limit
  **   PRAGMA hard_heap_limit = N
  **
  ** Invoke capdb_hard_heap_limit64() to query or set the hard heap
  ** limit.  The hard heap limit can be activated or lowered by this
  ** pragma, but not raised or deactivated.  Only the
  ** capdb_hard_heap_limit64() C-language API can raise or deactivate
  ** the hard heap limit.  This allows an application to set a heap limit
  ** constraint that cannot be relaxed by an untrusted SQL script.
  */
  case PragTyp_HARD_HEAP_LIMIT: {
    capdb_int64 N;
    if( zRight && capdbDecOrHexToI64(zRight, &N)==CAPDB_OK ){
      capdb_int64 iPrior = capdb_hard_heap_limit64(-1);
      if( N>0 && (iPrior==0 || iPrior>N) ) capdb_hard_heap_limit64(N);
    }
    returnSingleInt(v, capdb_hard_heap_limit64(-1));
    break;
  }

  /*
  **   PRAGMA threads
  **   PRAGMA threads = N
  **
  ** Configure the maximum number of worker threads.  Return the new
  ** maximum, which might be less than requested.
  */
  case PragTyp_THREADS: {
    capdb_int64 N;
    if( zRight
     && capdbDecOrHexToI64(zRight, &N)==CAPDB_OK
     && N>=0
    ){
      capdb_limit(db, CAPDB_LIMIT_WORKER_THREADS, (int)(N&0x7fffffff));
    }
    returnSingleInt(v, capdb_limit(db, CAPDB_LIMIT_WORKER_THREADS, -1));
    break;
  }

  /*
  **   PRAGMA analysis_limit
  **   PRAGMA analysis_limit = N
  **
  ** Configure the maximum number of rows that ANALYZE will examine
  ** in each index that it looks at.  Return the new limit.
  */
  case PragTyp_ANALYSIS_LIMIT: {
    capdb_int64 N;
    if( zRight
     && capdbDecOrHexToI64(zRight, &N)==CAPDB_OK /* IMP: R-40975-20399 */
     && N>=0
    ){
      db->nAnalysisLimit = (int)(N&0x7fffffff);
    }
    returnSingleInt(v, db->nAnalysisLimit); /* IMP: R-57594-65522 */
    break;
  }

#if defined(CAPDB_DEBUG) || defined(CAPDB_TEST)
  /*
  ** Report the current state of file logs for all databases
  */
  case PragTyp_LOCK_STATUS: {
    static const char *const azLockName[] = {
      "unlocked", "shared", "reserved", "pending", "exclusive"
    };
    int i;
    pParse->nMem = 2;
    for(i=0; i<db->nDb; i++){
      Btree *pBt;
      const char *zState = "unknown";
      int j;
      if( db->aDb[i].zDbSName==0 ) continue;
      pBt = db->aDb[i].pBt;
      if( pBt==0 || capdbBtreePager(pBt)==0 ){
        zState = "closed";
      }else if( capdb_file_control(db, i ? db->aDb[i].zDbSName : 0,
                                     CAPDB_FCNTL_LOCKSTATE, &j)==CAPDB_OK ){
         zState = azLockName[j];
      }
      capdbVdbeMultiLoad(v, 1, "ss", db->aDb[i].zDbSName, zState);
    }
    break;
  }
#endif

#if defined(CAPDB_ENABLE_CEROD)
  case PragTyp_ACTIVATE_EXTENSIONS: if( zRight ){
    if( capdbStrNICmp(zRight, "cerod-", 6)==0 ){
      capdb_activate_cerod(&zRight[6]);
    }
  }
  break;
#endif

  } /* End of the PRAGMA switch */

  /* The following block is a no-op unless CAPDB_DEBUG is defined. Its only
  ** purpose is to execute assert() statements to verify that if the
  ** PragFlg_NoColumns1 flag is set and the caller specified an argument
  ** to the PRAGMA, the implementation has not added any OP_ResultRow
  ** instructions to the VM.  */
  if( (pPragma->mPragFlg & PragFlg_NoColumns1) && zRight ){
    capdbVdbeVerifyNoResultRow(v);
  }

pragma_out:
  capdbDbFree(db, zLeft);
  capdbDbFree(db, zRight);
}
#ifndef CAPDB_OMIT_VIRTUALTABLE
/*****************************************************************************
** Implementation of an eponymous virtual table that runs a pragma.
**
*/
typedef struct PragmaVtab PragmaVtab;
typedef struct PragmaVtabCursor PragmaVtabCursor;
struct PragmaVtab {
  capdb_vtab base;        /* Base class.  Must be first */
  capdb *db;              /* The database connection to which it belongs */
  const PragmaName *pName;  /* Name of the pragma */
  u8 nHidden;               /* Number of hidden columns */
  u8 iHidden;               /* Index of the first hidden column */
};
struct PragmaVtabCursor {
  capdb_vtab_cursor base; /* Base class.  Must be first */
  capdb_stmt *pPragma;    /* The pragma statement to run */
  sqlite_int64 iRowid;      /* Current rowid */
  char *azArg[2];           /* Value of the argument and schema */
};

/*
** Pragma virtual table module xConnect method.
*/
static int pragmaVtabConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  const PragmaName *pPragma = (const PragmaName*)pAux;
  PragmaVtab *pTab = 0;
  int rc;
  int i, j;
  char cSep = '(';
  StrAccum acc;
  char zBuf[200];

  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);
  capdbStrAccumInit(&acc, 0, zBuf, sizeof(zBuf), 0);
  capdb_str_appendall(&acc, "CREATE TABLE x");
  for(i=0, j=pPragma->iPragCName; i<pPragma->nPragCName; i++, j++){
    capdb_str_appendf(&acc, "%c\"%s\"", cSep, pragCName[j]);
    cSep = ',';
  }
  if( i==0 ){
    capdb_str_appendf(&acc, "(\"%s\"", pPragma->zName);
    i++;
  }
  j = 0;
  if( pPragma->mPragFlg & PragFlg_Result1 ){
    capdb_str_appendall(&acc, ",arg HIDDEN");
    j++;
  }
  if( pPragma->mPragFlg & (PragFlg_SchemaOpt|PragFlg_SchemaReq) ){
    capdb_str_appendall(&acc, ",schema HIDDEN");
    j++;
  }
  capdb_str_append(&acc, ")", 1);
  capdbStrAccumFinish(&acc);
  assert( strlen(zBuf) < sizeof(zBuf)-1 );
  rc = capdb_declare_vtab(db, zBuf);
  if( rc==CAPDB_OK ){
    pTab = (PragmaVtab*)capdb_malloc(sizeof(PragmaVtab));
    if( pTab==0 ){
      rc = CAPDB_NOMEM;
    }else{
      memset(pTab, 0, sizeof(PragmaVtab));
      pTab->pName = pPragma;
      pTab->db = db;
      pTab->iHidden = i;
      pTab->nHidden = j;
    }
  }else{
    *pzErr = capdb_mprintf("%s", capdb_errmsg(db));
  }

  *ppVtab = (capdb_vtab*)pTab;
  return rc;
}

/*
** Pragma virtual table module xDisconnect method.
*/
static int pragmaVtabDisconnect(capdb_vtab *pVtab){
  PragmaVtab *pTab = (PragmaVtab*)pVtab;
  capdb_free(pTab);
  return CAPDB_OK;
}

/* Figure out the best index to use to search a pragma virtual table.
**
** There are not really any index choices.  But we want to encourage the
** query planner to give == constraints on as many hidden parameters as
** possible, and especially on the first hidden parameter.  So return a
** high cost if hidden parameters are unconstrained.
*/
static int pragmaVtabBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  PragmaVtab *pTab = (PragmaVtab*)tab;
  const struct capdb_index_constraint *pConstraint;
  int i, j;
  int seen[2];

  pIdxInfo->estimatedCost = (double)1;
  if( pTab->nHidden==0 ){ return CAPDB_OK; }
  pConstraint = pIdxInfo->aConstraint;
  seen[0] = 0;
  seen[1] = 0;
  for(i=0; i<pIdxInfo->nConstraint; i++, pConstraint++){
    if( pConstraint->iColumn < pTab->iHidden ) continue;
    if( pConstraint->op!=CAPDB_INDEX_CONSTRAINT_EQ ) continue;
    if( pConstraint->usable==0 ) return CAPDB_CONSTRAINT;
    j = pConstraint->iColumn - pTab->iHidden;
    assert( j < 2 );
    seen[j] = i+1;
  }
  if( seen[0]==0 ){
    pIdxInfo->estimatedCost = (double)2147483647;
    pIdxInfo->estimatedRows = 2147483647;
    return CAPDB_OK;
  }
  j = seen[0]-1;
  pIdxInfo->aConstraintUsage[j].argvIndex = 1;
  pIdxInfo->aConstraintUsage[j].omit = 1;
  pIdxInfo->estimatedCost = (double)20;
  pIdxInfo->estimatedRows = 20;
  if( seen[1] ){
    j = seen[1]-1;
    pIdxInfo->aConstraintUsage[j].argvIndex = 2;
    pIdxInfo->aConstraintUsage[j].omit = 1;
  }
  return CAPDB_OK;
}

/* Create a new cursor for the pragma virtual table */
static int pragmaVtabOpen(capdb_vtab *pVtab, capdb_vtab_cursor **ppCursor){
  PragmaVtabCursor *pCsr;
  pCsr = (PragmaVtabCursor*)capdb_malloc(sizeof(*pCsr));
  if( pCsr==0 ) return CAPDB_NOMEM;
  memset(pCsr, 0, sizeof(PragmaVtabCursor));
  pCsr->base.pVtab = pVtab;
  *ppCursor = &pCsr->base;
  return CAPDB_OK;
}

/* Clear all content from pragma virtual table cursor. */
static void pragmaVtabCursorClear(PragmaVtabCursor *pCsr){
  int i;
  capdb_finalize(pCsr->pPragma);
  pCsr->pPragma = 0;
  pCsr->iRowid = 0;
  for(i=0; i<ArraySize(pCsr->azArg); i++){
    capdb_free(pCsr->azArg[i]);
    pCsr->azArg[i] = 0;
  }
}

/* Close a pragma virtual table cursor */
static int pragmaVtabClose(capdb_vtab_cursor *cur){
  PragmaVtabCursor *pCsr = (PragmaVtabCursor*)cur;
  pragmaVtabCursorClear(pCsr);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/* Advance the pragma virtual table cursor to the next row */
static int pragmaVtabNext(capdb_vtab_cursor *pVtabCursor){
  PragmaVtabCursor *pCsr = (PragmaVtabCursor*)pVtabCursor;
  int rc = CAPDB_OK;

  /* Increment the xRowid value */
  pCsr->iRowid++;
  assert( pCsr->pPragma );
  if( CAPDB_ROW!=capdb_step(pCsr->pPragma) ){
    rc = capdb_finalize(pCsr->pPragma);
    pCsr->pPragma = 0;
    pragmaVtabCursorClear(pCsr);
  }
  return rc;
}

/*
** Pragma virtual table module xFilter method.
*/
static int pragmaVtabFilter(
  capdb_vtab_cursor *pVtabCursor,
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  PragmaVtabCursor *pCsr = (PragmaVtabCursor*)pVtabCursor;
  PragmaVtab *pTab = (PragmaVtab*)(pVtabCursor->pVtab);
  int rc;
  int i, j;
  StrAccum acc;
  char *zSql;

  UNUSED_PARAMETER(idxNum);
  UNUSED_PARAMETER(idxStr);
  pragmaVtabCursorClear(pCsr);
  j = (pTab->pName->mPragFlg & PragFlg_Result1)!=0 ? 0 : 1;
  for(i=0; i<argc; i++, j++){
    const char *zText = (const char*)capdb_value_text(argv[i]);
    assert( j<ArraySize(pCsr->azArg) );
    assert( pCsr->azArg[j]==0 );
    if( zText ){
      pCsr->azArg[j] = capdb_mprintf("%s", zText);
      if( pCsr->azArg[j]==0 ){
        return CAPDB_NOMEM;
      }
    }
  }
  capdbStrAccumInit(&acc, 0, 0, 0, pTab->db->aLimit[CAPDB_LIMIT_SQL_LENGTH]);
  capdb_str_appendall(&acc, "PRAGMA ");
  if( pCsr->azArg[1] ){
    capdb_str_appendf(&acc, "%Q.", pCsr->azArg[1]);
  }
  capdb_str_appendall(&acc, pTab->pName->zName);
  if( pCsr->azArg[0] ){
    capdb_str_appendf(&acc, "=%Q", pCsr->azArg[0]);
  }
  zSql = capdbStrAccumFinish(&acc);
  if( zSql==0 ) return CAPDB_NOMEM;
  rc = capdb_prepare_v2(pTab->db, zSql, -1, &pCsr->pPragma, 0);
  capdb_free(zSql);
  if( rc!=CAPDB_OK ){
    pTab->base.zErrMsg = capdb_mprintf("%s", capdb_errmsg(pTab->db));
    return rc;
  }
  return pragmaVtabNext(pVtabCursor);
}

/*
** Pragma virtual table module xEof method.
*/
static int pragmaVtabEof(capdb_vtab_cursor *pVtabCursor){
  PragmaVtabCursor *pCsr = (PragmaVtabCursor*)pVtabCursor;
  return (pCsr->pPragma==0);
}

/* The xColumn method simply returns the corresponding column from
** the PRAGMA. 
*/
static int pragmaVtabColumn(
  capdb_vtab_cursor *pVtabCursor,
  capdb_context *ctx,
  int i
){
  PragmaVtabCursor *pCsr = (PragmaVtabCursor*)pVtabCursor;
  PragmaVtab *pTab = (PragmaVtab*)(pVtabCursor->pVtab);
  if( i<pTab->iHidden ){
    capdb_result_value(ctx, capdb_column_value(pCsr->pPragma, i));
  }else{
    capdb_result_text(ctx, pCsr->azArg[i-pTab->iHidden],-1,CAPDB_TRANSIENT);
  }
  return CAPDB_OK;
}

/*
** Pragma virtual table module xRowid method.
*/
static int pragmaVtabRowid(capdb_vtab_cursor *pVtabCursor, sqlite_int64 *p){
  PragmaVtabCursor *pCsr = (PragmaVtabCursor*)pVtabCursor;
  *p = pCsr->iRowid;
  return CAPDB_OK;
}

/* The pragma virtual table object */
static const capdb_module pragmaVtabModule = {
  0,                           /* iVersion */
  0,                           /* xCreate - create a table */
  pragmaVtabConnect,           /* xConnect - connect to an existing table */
  pragmaVtabBestIndex,         /* xBestIndex - Determine search strategy */
  pragmaVtabDisconnect,        /* xDisconnect - Disconnect from a table */
  0,                           /* xDestroy - Drop a table */
  pragmaVtabOpen,              /* xOpen - open a cursor */
  pragmaVtabClose,             /* xClose - close a cursor */
  pragmaVtabFilter,            /* xFilter - configure scan constraints */
  pragmaVtabNext,              /* xNext - advance a cursor */
  pragmaVtabEof,               /* xEof */
  pragmaVtabColumn,            /* xColumn - read data */
  pragmaVtabRowid,             /* xRowid - read data */
  0,                           /* xUpdate - write data */
  0,                           /* xBegin - begin transaction */
  0,                           /* xSync - sync transaction */
  0,                           /* xCommit - commit transaction */
  0,                           /* xRollback - rollback transaction */
  0,                           /* xFindFunction - function overloading */
  0,                           /* xRename - rename the table */
  0,                           /* xSavepoint */
  0,                           /* xRelease */
  0,                           /* xRollbackTo */
  0,                           /* xShadowName */
  0                            /* xIntegrity */
};

/*
** Check to see if zTabName is really the name of a pragma.  If it is,
** then register an eponymous virtual table for that pragma and return
** a pointer to the Module object for the new virtual table.
*/
Module *capdbPragmaVtabRegister(capdb *db, const char *zName){
  const PragmaName *pName;
  assert( capdb_strnicmp(zName, "pragma_", 7)==0 );
  pName = pragmaLocate(zName+7);
  if( pName==0 ) return 0;
  if( (pName->mPragFlg & (PragFlg_Result0|PragFlg_Result1))==0 ) return 0;
  assert( capdbHashFind(&db->aModule, zName)==0 );
  return capdbVtabCreateModule(db, zName, &pragmaVtabModule, (void*)pName, 0);
}

#endif /* CAPDB_OMIT_VIRTUALTABLE */

#endif /* CAPDB_OMIT_PRAGMA */
