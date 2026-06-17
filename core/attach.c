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
** This file contains code used to implement the ATTACH and DETACH commands.
*/
#include "capdbInt.h"

#ifndef CAPDB_OMIT_ATTACH
/*
** Resolve an expression that was part of an ATTACH or DETACH statement. This
** is slightly different from resolving a normal SQL expression, because simple
** identifiers are treated as strings, not possible column names or aliases.
**
** i.e. if the parser sees:
**
**     ATTACH DATABASE abc AS def
**
** it treats the two expressions as literal strings 'abc' and 'def' instead of
** looking for columns of the same name.
**
** This only applies to the root node of pExpr, so the statement:
**
**     ATTACH DATABASE abc||def AS 'db2'
**
** will fail because neither abc or def can be resolved.
*/
static int resolveAttachExpr(NameContext *pName, Expr *pExpr)
{
  int rc = CAPDB_OK;
  if( pExpr ){
    if( pExpr->op!=TK_ID ){
      rc = capdbResolveExprNames(pName, pExpr);
    }else{
      pExpr->op = TK_STRING;
    }
  }
  return rc;
}

/*
** Return true if zName points to a name that may be used to refer to
** database iDb attached to handle db.
*/
int capdbDbIsNamed(capdb *db, int iDb, const char *zName){
  return (
      capdbStrICmp(db->aDb[iDb].zDbSName, zName)==0
   || (iDb==0 && capdbStrICmp("main", zName)==0)
  );
}

/*
** An SQL user-function registered to do the work of an ATTACH statement. The
** three arguments to the function come directly from an attach statement:
**
**     ATTACH DATABASE x AS y KEY z
**
**     SELECT sqlite_attach(x, y, z)
**
** If the optional "KEY z" syntax is omitted, an SQL NULL is passed as the
** third argument.
**
** If the db->init.reopenMemdb flags is set, then instead of attaching a
** new database, close the database on db->init.iDb and reopen it as an
** empty MemDB.
*/
static void attachFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **argv
){
  int i;
  int rc = 0;
  capdb *db = capdb_context_db_handle(context);
  const char *zName;
  const char *zFile;
  char *zPath = 0;
  char *zErr = 0;
  unsigned int flags;
  Db *aNew;                 /* New array of Db pointers */
  Db *pNew = 0;             /* Db object for the newly attached database */
  char *zErrDyn = 0;
  capdb_vfs *pVfs;

  UNUSED_PARAMETER(NotUsed);
  zFile = (const char *)capdb_value_text(argv[0]);
  zName = (const char *)capdb_value_text(argv[1]);
  if( zFile==0 ) zFile = "";
  if( zName==0 ) zName = "";

#ifndef CAPDB_OMIT_DESERIALIZE
# define REOPEN_AS_MEMDB(db)  (db->init.reopenMemdb)
#else
# define REOPEN_AS_MEMDB(db)  (0)
#endif

  if( REOPEN_AS_MEMDB(db) ){
    /* This is not a real ATTACH.  Instead, this routine is being called
    ** from capdb_deserialize() to close database db->init.iDb and
    ** reopen it as a MemDB */
    Btree *pNewBt = 0;

    pNew = &db->aDb[db->init.iDb];
    assert( pNew->pBt!=0 );
    if( capdbBtreeTxnState(pNew->pBt)!=CAPDB_TXN_NONE
     || capdbBtreeIsInBackup(pNew->pBt)
    ){
      rc = CAPDB_BUSY;
      goto attach_error;
    }

    pVfs = capdb_vfs_find("memdb");
    if( pVfs==0 ) return;
    rc = capdbBtreeOpen(pVfs, "x\0", db, &pNewBt, 0, CAPDB_OPEN_MAIN_DB);
    if( rc==CAPDB_OK ){
      Schema *pNewSchema = capdbSchemaGet(db, pNewBt);
      if( pNewSchema ){
        /* Both the Btree and the new Schema were allocated successfully.
        ** Close the old db and update the aDb[] slot with the new memdb
        ** values.  */
        capdbBtreeClose(pNew->pBt);
        pNew->pBt = pNewBt;
        pNew->pSchema = pNewSchema;
      }else{
        capdbBtreeClose(pNewBt);
        rc = CAPDB_NOMEM;
      }
    }
    if( rc ) goto attach_error;
  }else{
    /* This is a real ATTACH
    **
    ** Check for the following errors:
    **
    **     * Too many attached databases,
    **     * Transaction currently open
    **     * Specified database name already being used.
    */
    if( db->nDb>=db->aLimit[CAPDB_LIMIT_ATTACHED]+2 ){
      zErrDyn = capdbMPrintf(db, "too many attached databases - max %d", 
        db->aLimit[CAPDB_LIMIT_ATTACHED]
      );
      goto attach_error;
    }
    for(i=0; i<db->nDb; i++){
      assert( zName );
      if( capdbDbIsNamed(db, i, zName) ){
        zErrDyn = capdbMPrintf(db, "database %s is already in use", zName);
        goto attach_error;
      }
    }
  
    /* Allocate the new entry in the db->aDb[] array and initialize the schema
    ** hash tables.
    */
    if( db->aDb==db->aDbStatic ){
      aNew = capdbDbMallocRawNN(db, sizeof(db->aDb[0])*3 );
      if( aNew==0 ) return;
      memcpy(aNew, db->aDb, sizeof(db->aDb[0])*2);
    }else{
      aNew = capdbDbRealloc(db, db->aDb, sizeof(db->aDb[0])*(1+(i64)db->nDb));
      if( aNew==0 ) return;
    }
    db->aDb = aNew;
    pNew = &db->aDb[db->nDb];
    memset(pNew, 0, sizeof(*pNew));
  
    /* Open the database file. If the btree is successfully opened, use
    ** it to obtain the database schema. At this point the schema may
    ** or may not be initialized.
    */
    flags = db->openFlags;
    rc = capdbParseUri(db->pVfs->zName, zFile, &flags, &pVfs, &zPath, &zErr);
    if( rc!=CAPDB_OK ){
      if( rc==CAPDB_NOMEM ) capdbOomFault(db);
      capdb_result_error(context, zErr, -1);
      capdb_free(zErr);
      return;
    }
    if( (db->flags & CAPDB_AttachWrite)==0 ){
      flags &= ~(CAPDB_OPEN_CREATE|CAPDB_OPEN_READWRITE);
      flags |= CAPDB_OPEN_READONLY;
    }else if( (db->flags & CAPDB_AttachCreate)==0 ){
      flags &= ~CAPDB_OPEN_CREATE;
    }
    assert( pVfs );
    flags |= CAPDB_OPEN_MAIN_DB;
    rc = capdbBtreeOpen(pVfs, zPath, db, &pNew->pBt, 0, flags);
    db->nDb++;
    pNew->zDbSName = capdbDbStrDup(db, zName);
  }
  db->noSharedCache = 0;
  if( rc==CAPDB_CONSTRAINT ){
    rc = CAPDB_ERROR;
    zErrDyn = capdbMPrintf(db, "database is already attached");
  }else if( rc==CAPDB_OK ){
    Pager *pPager;
    pNew->pSchema = capdbSchemaGet(db, pNew->pBt);
    if( !pNew->pSchema ){
      rc = CAPDB_NOMEM_BKPT;
    }else if( pNew->pSchema->file_format && pNew->pSchema->enc!=ENC(db) ){
      zErrDyn = capdbMPrintf(db, 
        "attached databases must use the same text encoding as main database");
      rc = CAPDB_ERROR;
    }
    capdbBtreeEnter(pNew->pBt);
    pPager = capdbBtreePager(pNew->pBt);
    capdbPagerLockingMode(pPager, db->dfltLockMode);
    capdbBtreeSecureDelete(pNew->pBt,
                             capdbBtreeSecureDelete(db->aDb[0].pBt,-1) );
#ifndef CAPDB_OMIT_PAGER_PRAGMAS
    capdbBtreeSetPagerFlags(pNew->pBt,
                      PAGER_SYNCHRONOUS_FULL | (db->flags & PAGER_FLAGS_MASK));
#endif
    capdbBtreeLeave(pNew->pBt);
  }
  pNew->safety_level = CAPDB_DEFAULT_SYNCHRONOUS+1;
  if( rc==CAPDB_OK && pNew->zDbSName==0 ){
    rc = CAPDB_NOMEM_BKPT;
  }
  capdb_free_filename( zPath );

  /* If the file was opened successfully, read the schema for the new database.
  ** If this fails, or if opening the file failed, then close the file and 
  ** remove the entry from the db->aDb[] array. i.e. put everything back the
  ** way we found it.
  */
  if( rc==CAPDB_OK ){
    capdbBtreeEnterAll(db);
    db->init.iDb = 0;
    db->mDbFlags &= ~(DBFLAG_SchemaKnownOk);
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
    if( db->setlkFlags & CAPDB_SETLK_BLOCK_ON_CONNECT ){
      int val = 1;
      capdb_file *fd = capdbPagerFile(capdbBtreePager(pNew->pBt));
      capdbOsFileControlHint(fd, CAPDB_FCNTL_BLOCK_ON_CONNECT, &val);
    }
#endif
    if( !REOPEN_AS_MEMDB(db) ){
      rc = capdbInit(db, &zErrDyn);
    }
    capdbBtreeLeaveAll(db);
    assert( zErrDyn==0 || rc!=CAPDB_OK );
  }
  if( rc ){
    if( ALWAYS(!REOPEN_AS_MEMDB(db)) ){
      int iDb = db->nDb - 1;
      assert( iDb>=2 );
      if( db->aDb[iDb].pBt ){
        capdbBtreeClose(db->aDb[iDb].pBt);
        db->aDb[iDb].pBt = 0;
        db->aDb[iDb].pSchema = 0;
      }
      capdbResetAllSchemasOfConnection(db);
      db->nDb = iDb;
      if( rc==CAPDB_NOMEM || rc==CAPDB_IOERR_NOMEM ){
        capdbOomFault(db);
        capdbDbFree(db, zErrDyn);
        zErrDyn = capdbMPrintf(db, "out of memory");
      }else if( zErrDyn==0 ){
        zErrDyn = capdbMPrintf(db, "unable to open database: %s", zFile);
      }
    }
    goto attach_error;
  }
  
  return;

attach_error:
  /* Return an error if we get here */
  if( zErrDyn ){
    capdb_result_error(context, zErrDyn, -1);
    capdbDbFree(db, zErrDyn);
  }
  if( rc ) capdb_result_error_code(context, rc);
}

/*
** An SQL user-function registered to do the work of an DETACH statement. The
** three arguments to the function come directly from a detach statement:
**
**     DETACH DATABASE x
**
**     SELECT sqlite_detach(x)
*/
static void detachFunc(
  capdb_context *context,
  int NotUsed,
  capdb_value **argv
){
  const char *zName = (const char *)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  int i;
  Db *pDb = 0;
  HashElem *pEntry;
  char zErr[128];

  UNUSED_PARAMETER(NotUsed);

  if( zName==0 ) zName = "";
  for(i=0; i<db->nDb; i++){
    pDb = &db->aDb[i];
    if( pDb->pBt==0 ) continue;
    if( capdbDbIsNamed(db, i, zName) ) break;
  }

  if( i>=db->nDb ){
    capdb_snprintf(sizeof(zErr),zErr, "no such database: %s", zName);
    goto detach_error;
  }
  if( i<2 ){
    capdb_snprintf(sizeof(zErr),zErr, "cannot detach database %s", zName);
    goto detach_error;
  }
  if( capdbBtreeTxnState(pDb->pBt)!=CAPDB_TXN_NONE
   || capdbBtreeIsInBackup(pDb->pBt)
  ){
    capdb_snprintf(sizeof(zErr),zErr, "database %s is locked", zName);
    goto detach_error;
  }

  /* If any TEMP triggers reference the schema being detached, move those
  ** triggers to reference the TEMP schema itself. */
  assert( db->aDb[1].pSchema );
  pEntry = sqliteHashFirst(&db->aDb[1].pSchema->trigHash);
  while( pEntry ){
    Trigger *pTrig = (Trigger*)sqliteHashData(pEntry);
    if( pTrig->pTabSchema==pDb->pSchema ){
      pTrig->pTabSchema = pTrig->pSchema;
    }
    pEntry = sqliteHashNext(pEntry);
  }

  capdbBtreeClose(pDb->pBt);
  pDb->pBt = 0;
  pDb->pSchema = 0;
  capdbCollapseDatabaseArray(db);
  return;

detach_error:
  capdb_result_error(context, zErr, -1);
}

/*
** This procedure generates VDBE code for a single invocation of either the
** sqlite_detach() or sqlite_attach() SQL user functions.
*/
static void codeAttach(
  Parse *pParse,       /* The parser context */
  int type,            /* Either CAPDB_ATTACH or CAPDB_DETACH */
  FuncDef const *pFunc,/* FuncDef wrapper for detachFunc() or attachFunc() */
  Expr *pAuthArg,      /* Expression to pass to authorization callback */
  Expr *pFilename,     /* Name of database file */
  Expr *pDbname,       /* Name of the database to use internally */
  Expr *pKey           /* Database key for encryption extension */
){
  int rc;
  NameContext sName;
  Vdbe *v;
  capdb* db = pParse->db;
  int regArgs;

  if( CAPDB_OK!=capdbReadSchema(pParse) ) goto attach_end;

  if( pParse->nErr ) goto attach_end;
  memset(&sName, 0, sizeof(NameContext));
  sName.pParse = pParse;

  if( 
      CAPDB_OK!=resolveAttachExpr(&sName, pFilename) ||
      CAPDB_OK!=resolveAttachExpr(&sName, pDbname) ||
      CAPDB_OK!=resolveAttachExpr(&sName, pKey)
  ){
    goto attach_end;
  }

#ifndef CAPDB_OMIT_AUTHORIZATION
  if( ALWAYS(pAuthArg) ){
    char *zAuthArg;
    if( pAuthArg->op==TK_STRING ){
      assert( !ExprHasProperty(pAuthArg, EP_IntValue) );
      zAuthArg = pAuthArg->u.zToken;
    }else{
      zAuthArg = 0;
    }
    rc = capdbAuthCheck(pParse, type, zAuthArg, 0, 0);
    if(rc!=CAPDB_OK ){
      goto attach_end;
    }
  }
#endif /* CAPDB_OMIT_AUTHORIZATION */


  v = capdbGetVdbe(pParse);
  regArgs = capdbGetTempRange(pParse, 4);
  capdbExprCode(pParse, pFilename, regArgs);
  capdbExprCode(pParse, pDbname, regArgs+1);
  capdbExprCode(pParse, pKey, regArgs+2);

  assert( v || db->mallocFailed );
  if( v ){
    capdbVdbeAddFunctionCall(pParse, 0, regArgs+3-pFunc->nArg, regArgs+3,
                               pFunc->nArg, pFunc, 0);
    /* Code an OP_Expire. For an ATTACH statement, set P1 to true (expire this
    ** statement only). For DETACH, set it to false (expire all existing
    ** statements).
    */
    capdbVdbeAddOp1(v, OP_Expire, (type==CAPDB_ATTACH));
  }
  
attach_end:
  capdbExprDelete(db, pFilename);
  capdbExprDelete(db, pDbname);
  capdbExprDelete(db, pKey);
}

/*
** Called by the parser to compile a DETACH statement.
**
**     DETACH pDbname
*/
void capdbDetach(Parse *pParse, Expr *pDbname){
  static const FuncDef detach_func = {
    1,                /* nArg */
    CAPDB_UTF8,      /* funcFlags */
    0,                /* pUserData */
    0,                /* pNext */
    detachFunc,       /* xSFunc */
    0,                /* xFinalize */
    0, 0,             /* xValue, xInverse */
    "sqlite_detach",  /* zName */
    {0}
  };
  codeAttach(pParse, CAPDB_DETACH, &detach_func, pDbname, 0, 0, pDbname);
}

/*
** Called by the parser to compile an ATTACH statement.
**
**     ATTACH p AS pDbname KEY pKey
*/
void capdbAttach(Parse *pParse, Expr *p, Expr *pDbname, Expr *pKey){
  static const FuncDef attach_func = {
    3,                /* nArg */
    CAPDB_UTF8,      /* funcFlags */
    0,                /* pUserData */
    0,                /* pNext */
    attachFunc,       /* xSFunc */
    0,                /* xFinalize */
    0, 0,             /* xValue, xInverse */
    "sqlite_attach",  /* zName */
    {0}
  };
  codeAttach(pParse, CAPDB_ATTACH, &attach_func, p, p, pDbname, pKey);
}
#endif /* CAPDB_OMIT_ATTACH */

/*
** Expression callback used by capdbFixAAAA() routines.
*/
static int fixExprCb(Walker *p, Expr *pExpr){
  DbFixer *pFix = p->u.pFix;
  if( !pFix->bTemp ) ExprSetProperty(pExpr, EP_FromDDL);
  if( pExpr->op==TK_VARIABLE ){
    if( pFix->pParse->db->init.busy ){
      pExpr->op = TK_NULL;
    }else{
      capdbErrorMsg(pFix->pParse, "%s cannot use variables", pFix->zType);
      return WRC_Abort;
    }
  }
  return WRC_Continue;
}

/*
** Select callback used by capdbFixAAAA() routines.
*/
static int fixSelectCb(Walker *p, Select *pSelect){
  DbFixer *pFix = p->u.pFix;
  int i;
  SrcItem *pItem;
  capdb *db = pFix->pParse->db;
  int iDb = capdbFindDbName(db, pFix->zDb);
  SrcList *pList = pSelect->pSrc;

  if( NEVER(pList==0) ) return WRC_Continue;
  for(i=0, pItem=pList->a; i<pList->nSrc; i++, pItem++){
    if( pFix->bTemp==0 && pItem->fg.isSubquery==0 ){
      if( pItem->fg.fixedSchema==0 && pItem->u4.zDatabase!=0 ){
        if( iDb!=capdbFindDbName(db, pItem->u4.zDatabase) ){
          capdbErrorMsg(pFix->pParse,
              "%s %T cannot reference objects in database %s",
              pFix->zType, pFix->pName, pItem->u4.zDatabase);
          return WRC_Abort;
        }
        capdbDbFree(db, pItem->u4.zDatabase);
        pItem->fg.notCte = 1;
        pItem->fg.hadSchema = 1;
      }
      pItem->u4.pSchema = pFix->pSchema;
      pItem->fg.fromDDL = 1;
      pItem->fg.fixedSchema = 1;
    }
#if !defined(CAPDB_OMIT_VIEW) || !defined(CAPDB_OMIT_TRIGGER)
    if( pList->a[i].fg.isUsing==0
     && capdbWalkExpr(&pFix->w, pList->a[i].u3.pOn)
    ){
      return WRC_Abort;
    }
#endif
  }
  if( pSelect->pWith ){
    for(i=0; i<pSelect->pWith->nCte; i++){
      if( capdbWalkSelect(p, pSelect->pWith->a[i].pSelect) ){
        return WRC_Abort;
      }
    }
  }
  return WRC_Continue;
}

/*
** Initialize a DbFixer structure.  This routine must be called prior
** to passing the structure to one of the sqliteFixAAAA() routines below.
*/
void capdbFixInit(
  DbFixer *pFix,      /* The fixer to be initialized */
  Parse *pParse,      /* Error messages will be written here */
  int iDb,            /* This is the database that must be used */
  const char *zType,  /* "view", "trigger", or "index" */
  const Token *pName  /* Name of the view, trigger, or index */
){
  capdb *db = pParse->db;
  assert( db->nDb>iDb );
  pFix->pParse = pParse;
  pFix->zDb = db->aDb[iDb].zDbSName;
  pFix->pSchema = db->aDb[iDb].pSchema;
  pFix->zType = zType;
  pFix->pName = pName;
  pFix->bTemp = (iDb==1);
  pFix->w.pParse = pParse;
  pFix->w.xExprCallback = fixExprCb;
  pFix->w.xSelectCallback = fixSelectCb;
  pFix->w.xSelectCallback2 = capdbWalkWinDefnDummyCallback;
  pFix->w.walkerDepth = 0;
  pFix->w.eCode = 0;
  pFix->w.u.pFix = pFix;
}

/*
** The following set of routines walk through the parse tree and assign
** a specific database to all table references where the database name
** was left unspecified in the original SQL statement.  The pFix structure
** must have been initialized by a prior call to capdbFixInit().
**
** These routines are used to make sure that an index, trigger, or
** view in one database does not refer to objects in a different database.
** (Exception: indices, triggers, and views in the TEMP database are
** allowed to refer to anything.)  If a reference is explicitly made
** to an object in a different database, an error message is added to
** pParse->zErrMsg and these routines return non-zero.  If everything
** checks out, these routines return 0.
*/
int capdbFixSrcList(
  DbFixer *pFix,       /* Context of the fixation */
  SrcList *pList       /* The Source list to check and modify */
){
  int res = 0;
  if( pList ){
    Select s; 
    memset(&s, 0, sizeof(s));
    s.pSrc = pList;
    res = capdbWalkSelect(&pFix->w, &s);
  }
  return res;
}
#if !defined(CAPDB_OMIT_VIEW) || !defined(CAPDB_OMIT_TRIGGER)
int capdbFixSelect(
  DbFixer *pFix,       /* Context of the fixation */
  Select *pSelect      /* The SELECT statement to be fixed to one database */
){
  return capdbWalkSelect(&pFix->w, pSelect);
}
int capdbFixExpr(
  DbFixer *pFix,     /* Context of the fixation */
  Expr *pExpr        /* The expression to be fixed to one database */
){
  return capdbWalkExpr(&pFix->w, pExpr);
}
#endif

#ifndef CAPDB_OMIT_TRIGGER
int capdbFixTriggerStep(
  DbFixer *pFix,     /* Context of the fixation */
  TriggerStep *pStep /* The trigger step be fixed to one database */
){
  while( pStep ){
    if( capdbWalkSelect(&pFix->w, pStep->pSelect)
     || capdbWalkExpr(&pFix->w, pStep->pWhere) 
     || capdbWalkExprList(&pFix->w, pStep->pExprList)
     || capdbFixSrcList(pFix, pStep->pSrc)
    ){
      return 1;
    }
#ifndef CAPDB_OMIT_UPSERT
    {
      Upsert *pUp;
      for(pUp=pStep->pUpsert; pUp; pUp=pUp->pNextUpsert){
        if( capdbWalkExprList(&pFix->w, pUp->pUpsertTarget)
         || capdbWalkExpr(&pFix->w, pUp->pUpsertTargetWhere)
         || capdbWalkExprList(&pFix->w, pUp->pUpsertSet)
         || capdbWalkExpr(&pFix->w, pUp->pUpsertWhere)
        ){
          return 1;
        }
      }
    }
#endif
    pStep = pStep->pNext;
  }

  return 0;
}
#endif
