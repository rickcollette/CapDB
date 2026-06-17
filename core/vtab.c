/*
** 2006 June 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to help implement virtual tables.
*/
#ifndef CAPDB_OMIT_VIRTUALTABLE
#include "capdbInt.h"

/*
** Before a virtual table xCreate() or xConnect() method is invoked, the
** capdb.pVtabCtx member variable is set to point to an instance of
** this struct allocated on the stack. It is used by the implementation of
** the capdb_declare_vtab() and capdb_vtab_config() APIs, both of which
** are invoked only from within xCreate and xConnect methods.
*/
struct VtabCtx {
  VTable *pVTable;    /* The virtual table being constructed */
  Table *pTab;        /* The Table object to which the virtual table belongs */
  VtabCtx *pPrior;    /* Parent context (if any) */
  int bDeclared;      /* True after capdb_declare_vtab() is called */
};

/*
** Construct and install a Module object for a virtual table.  When this
** routine is called, it is guaranteed that all appropriate locks are held
** and the module is not already part of the connection.
**
** If there already exists a module with zName, replace it with the new one.
** If pModule==0, then delete the module zName if it exists.
*/
Module *capdbVtabCreateModule(
  capdb *db,                    /* Database in which module is registered */
  const char *zName,              /* Name assigned to this module */
  const capdb_module *pModule,  /* The definition of the module */
  void *pAux,                     /* Context pointer for xCreate/xConnect */
  void (*xDestroy)(void *)        /* Module destructor function */
){
  Module *pMod;
  Module *pDel;
  char *zCopy;
  if( pModule==0 ){
    zCopy = (char*)zName;
    pMod = 0;
  }else{
    int nName = capdbStrlen30(zName);
    pMod = (Module *)capdbMalloc(sizeof(Module) + nName + 1);
    if( pMod==0 ){
      capdbOomFault(db);
      return 0;
    }
    zCopy = (char *)(&pMod[1]);
    memcpy(zCopy, zName, nName+1);
    pMod->zName = zCopy;
    pMod->pModule = pModule;
    pMod->pAux = pAux;
    pMod->xDestroy = xDestroy;
    pMod->pEpoTab = 0;
    pMod->nRefModule = 1;
  }
  pDel = (Module *)capdbHashInsert(&db->aModule,zCopy,(void*)pMod);
  if( pDel ){
    if( pDel==pMod ){
      capdbOomFault(db);
      capdbDbFree(db, pDel);
      pMod = 0;
    }else{
      capdbVtabEponymousTableClear(db, pDel);
      capdbVtabModuleUnref(db, pDel);
    }
  }
  return pMod;
}

/*
** The actual function that does the work of creating a new module.
** This function implements the capdb_create_module() and
** capdb_create_module_v2() interfaces.
*/
static int createModule(
  capdb *db,                    /* Database in which module is registered */
  const char *zName,              /* Name assigned to this module */
  const capdb_module *pModule,  /* The definition of the module */
  void *pAux,                     /* Context pointer for xCreate/xConnect */
  void (*xDestroy)(void *)        /* Module destructor function */
){
  int rc = CAPDB_OK;

  capdb_mutex_enter(db->mutex);
  (void)capdbVtabCreateModule(db, zName, pModule, pAux, xDestroy);
  rc = capdbApiExit(db, rc);
  if( rc!=CAPDB_OK && xDestroy ) xDestroy(pAux);
  capdb_mutex_leave(db->mutex);
  return rc;
}


/*
** External API function used to create a new virtual-table module.
*/
int capdb_create_module(
  capdb *db,                    /* Database in which module is registered */
  const char *zName,              /* Name assigned to this module */
  const capdb_module *pModule,  /* The definition of the module */
  void *pAux                      /* Context pointer for xCreate/xConnect */
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zName==0 ) return CAPDB_MISUSE_BKPT;
#endif
  return createModule(db, zName, pModule, pAux, 0);
}

/*
** External API function used to create a new virtual-table module.
*/
int capdb_create_module_v2(
  capdb *db,                    /* Database in which module is registered */
  const char *zName,              /* Name assigned to this module */
  const capdb_module *pModule,  /* The definition of the module */
  void *pAux,                     /* Context pointer for xCreate/xConnect */
  void (*xDestroy)(void *)        /* Module destructor function */
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zName==0 ) return CAPDB_MISUSE_BKPT;
#endif
  return createModule(db, zName, pModule, pAux, xDestroy);
}

/*
** External API to drop all virtual-table modules, except those named
** on the azNames list.
*/
int capdb_drop_modules(capdb *db, const char** azNames){
  HashElem *pThis, *pNext;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  for(pThis=sqliteHashFirst(&db->aModule); pThis; pThis=pNext){
    Module *pMod = (Module*)sqliteHashData(pThis);
    pNext = sqliteHashNext(pThis);
    if( azNames ){
      int ii;
      for(ii=0; azNames[ii]!=0 && strcmp(azNames[ii],pMod->zName)!=0; ii++){}
      if( azNames[ii]!=0 ) continue;
    }
    createModule(db, pMod->zName, 0, 0, 0);
  }
  return CAPDB_OK;
}

/*
** Decrement the reference count on a Module object.  Destroy the
** module when the reference count reaches zero.
*/
void capdbVtabModuleUnref(capdb *db, Module *pMod){
  assert( pMod->nRefModule>0 );
  pMod->nRefModule--;
  if( pMod->nRefModule==0 ){
    if( pMod->xDestroy ){
      pMod->xDestroy(pMod->pAux);
    }
    assert( pMod->pEpoTab==0 );
    capdbDbFree(db, pMod);
  }
}

/*
** Lock the virtual table so that it cannot be disconnected.
** Locks nest.  Every lock should have a corresponding unlock.
** If an unlock is omitted, resources leaks will occur.
**
** If a disconnect is attempted while a virtual table is locked,
** the disconnect is deferred until all locks have been removed.
*/
void capdbVtabLock(VTable *pVTab){
  pVTab->nRef++;
}


/*
** pTab is a pointer to a Table structure representing a virtual-table.
** Return a pointer to the VTable object used by connection db to access
** this virtual-table, if one has been created, or NULL otherwise.
*/
VTable *capdbGetVTable(capdb *db, Table *pTab){
  VTable *pVtab;
  assert( IsVirtual(pTab) );
  for(pVtab=pTab->u.vtab.p; pVtab && pVtab->db!=db; pVtab=pVtab->pNext);
  return pVtab;
}

/*
** Decrement the ref-count on a virtual table object. When the ref-count
** reaches zero, call the xDisconnect() method to delete the object.
*/
void capdbVtabUnlock(VTable *pVTab){
  capdb *db = pVTab->db;

  assert( db );
  assert( pVTab->nRef>0 );
  assert( db->eOpenState==CAPDB_STATE_OPEN
       || db->eOpenState==CAPDB_STATE_ZOMBIE );

  pVTab->nRef--;
  if( pVTab->nRef==0 ){
    capdb_vtab *p = pVTab->pVtab;
    if( p ){
      p->pModule->xDisconnect(p);
    }
    capdbVtabModuleUnref(pVTab->db, pVTab->pMod);
    capdbDbFree(db, pVTab);
  }
}

/*
** Table p is a virtual table. This function moves all elements in the
** p->u.vtab.p list to the capdb.pDisconnect lists of their associated
** database connections to be disconnected at the next opportunity.
** Except, if argument db is not NULL, then the entry associated with
** connection db is left in the p->u.vtab.p list.
*/
static VTable *vtabDisconnectAll(capdb *db, Table *p){
  VTable *pRet = 0;
  VTable *pVTable;

  assert( IsVirtual(p) );
  pVTable = p->u.vtab.p;
  p->u.vtab.p = 0;

  /* Assert that the mutex (if any) associated with the BtShared database
  ** that contains table p is held by the caller. See header comments
  ** above function capdbVtabUnlockList() for an explanation of why
  ** this makes it safe to access the capdb.pDisconnect list of any
  ** database connection that may have an entry in the p->u.vtab.p list.
  */
  assert( db==0 || capdbSchemaMutexHeld(db, 0, p->pSchema) );

  while( pVTable ){
    capdb *db2 = pVTable->db;
    VTable *pNext = pVTable->pNext;
    assert( db2 );
    if( db2==db ){
      pRet = pVTable;
      p->u.vtab.p = pRet;
      pRet->pNext = 0;
    }else{
      pVTable->pNext = db2->pDisconnect;
      db2->pDisconnect = pVTable;
    }
    pVTable = pNext;
  }

  assert( !db || pRet );
  return pRet;
}

/*
** Table *p is a virtual table. This function removes the VTable object
** for table *p associated with database connection db from the linked
** list in p->pVTab. It also decrements the VTable ref count. This is
** used when closing database connection db to free all of its VTable
** objects without disturbing the rest of the Schema object (which may
** be being used by other shared-cache connections).
*/
void capdbVtabDisconnect(capdb *db, Table *p){
  VTable **ppVTab;

  assert( IsVirtual(p) );
  assert( capdbBtreeHoldsAllMutexes(db) );
  assert( capdb_mutex_held(db->mutex) );

  for(ppVTab=&p->u.vtab.p; *ppVTab; ppVTab=&(*ppVTab)->pNext){
    if( (*ppVTab)->db==db  ){
      VTable *pVTab = *ppVTab;
      *ppVTab = pVTab->pNext;
      capdbVtabUnlock(pVTab);
      break;
    }
  }
}


/*
** Disconnect all the virtual table objects in the capdb.pDisconnect list.
**
** This function may only be called when the mutexes associated with all
** shared b-tree databases opened using connection db are held by the
** caller. This is done to protect the capdb.pDisconnect list. The
** capdb.pDisconnect list is accessed only as follows:
**
**   1) By this function. In this case, all BtShared mutexes and the mutex
**      associated with the database handle itself must be held.
**
**   2) By function vtabDisconnectAll(), when it adds a VTable entry to
**      the capdb.pDisconnect list. In this case either the BtShared mutex
**      associated with the database the virtual table is stored in is held
**      or, if the virtual table is stored in a non-sharable database, then
**      the database handle mutex is held.
**
** As a result, a capdb.pDisconnect cannot be accessed simultaneously
** by multiple threads. It is thread-safe.
*/
void capdbVtabUnlockList(capdb *db){
  VTable *p = db->pDisconnect;

  assert( capdbBtreeHoldsAllMutexes(db) );
  assert( capdb_mutex_held(db->mutex) );

  if( p ){
    db->pDisconnect = 0;
    do {
      VTable *pNext = p->pNext;
      capdbVtabUnlock(p);
      p = pNext;
    }while( p );
  }
}

/*
** Clear any and all virtual-table information from the Table record.
** This routine is called, for example, just before deleting the Table
** record.
**
** Since it is a virtual-table, the Table structure contains a pointer
** to the head of a linked list of VTable structures. Each VTable
** structure is associated with a single capdb* user of the schema.
** The reference count of the VTable structure associated with database
** connection db is decremented immediately (which may lead to the
** structure being xDisconnected and free). Any other VTable structures
** in the list are moved to the capdb.pDisconnect list of the associated
** database connection.
*/
void capdbVtabClear(capdb *db, Table *p){
  assert( IsVirtual(p) );
  assert( db!=0 );
  if( db->pnBytesFreed==0 ) vtabDisconnectAll(0, p);
  if( p->u.vtab.azArg ){
    int i;
    for(i=0; i<p->u.vtab.nArg; i++){
      if( i!=1 ) capdbDbFree(db, p->u.vtab.azArg[i]);
    }
    capdbDbFree(db, p->u.vtab.azArg);
  }
}

/*
** Add a new module argument to pTable->u.vtab.azArg[].
** The string is not copied - the pointer is stored.  The
** string will be freed automatically when the table is
** deleted.
*/
static void addModuleArgument(Parse *pParse, Table *pTable, char *zArg){
  capdb_int64 nBytes;
  char **azModuleArg;
  capdb *db = pParse->db;

  assert( IsVirtual(pTable) );
  nBytes = sizeof(char *)*(2+pTable->u.vtab.nArg);
  if( pTable->u.vtab.nArg+3>=db->aLimit[CAPDB_LIMIT_COLUMN] ){
    capdbErrorMsg(pParse, "too many columns on %s", pTable->zName);
  }
  azModuleArg = capdbDbRealloc(db, pTable->u.vtab.azArg, nBytes);
  if( azModuleArg==0 ){
    capdbDbFree(db, zArg);
  }else{
    int i = pTable->u.vtab.nArg++;
    azModuleArg[i] = zArg;
    azModuleArg[i+1] = 0;
    pTable->u.vtab.azArg = azModuleArg;
  }
}

/*
** The parser calls this routine when it first sees a CREATE VIRTUAL TABLE
** statement.  The module name has been parsed, but the optional list
** of parameters that follow the module name are still pending.
*/
void capdbVtabBeginParse(
  Parse *pParse,        /* Parsing context */
  Token *pName1,        /* Name of new table, or database name */
  Token *pName2,        /* Name of new table or NULL */
  Token *pModuleName,   /* Name of the module for the virtual table */
  int ifNotExists       /* No error if the table already exists */
){
  Table *pTable;        /* The new virtual table */
  capdb *db;          /* Database connection */

  capdbStartTable(pParse, pName1, pName2, 0, 0, 1, ifNotExists);
  pTable = pParse->pNewTable;
  if( pTable==0 ) return;
  assert( 0==pTable->pIndex );
  pTable->eTabType = TABTYP_VTAB;

  db = pParse->db;

  assert( pTable->u.vtab.nArg==0 );
  addModuleArgument(pParse, pTable, capdbNameFromToken(db, pModuleName));
  addModuleArgument(pParse, pTable, 0);
  addModuleArgument(pParse, pTable, capdbDbStrDup(db, pTable->zName));
  assert( (pParse->sNameToken.z==pName2->z && pName2->z!=0)
       || (pParse->sNameToken.z==pName1->z && pName2->z==0)
  );
  pParse->sNameToken.n = (int)(
      &pModuleName->z[pModuleName->n] - pParse->sNameToken.z
  );

#ifndef CAPDB_OMIT_AUTHORIZATION
  /* Creating a virtual table invokes the authorization callback twice.
  ** The first invocation, to obtain permission to INSERT a row into the
  ** sqlite_schema table, has already been made by capdbStartTable().
  ** The second call, to obtain permission to create the table, is made now.
  */
  if( pTable->u.vtab.azArg ){
    int iDb = capdbSchemaToIndex(db, pTable->pSchema);
    assert( iDb>=0 ); /* The database the table is being created in */
    capdbAuthCheck(pParse, CAPDB_CREATE_VTABLE, pTable->zName,
            pTable->u.vtab.azArg[0], pParse->db->aDb[iDb].zDbSName);
  }
#endif
}

/*
** This routine takes the module argument that has been accumulating
** in pParse->zArg[] and appends it to the list of arguments on the
** virtual table currently under construction in pParse->pTable.
*/
static void addArgumentToVtab(Parse *pParse){
  if( pParse->sArg.z && pParse->pNewTable ){
    const char *z = (const char*)pParse->sArg.z;
    int n = pParse->sArg.n;
    capdb *db = pParse->db;
    addModuleArgument(pParse, pParse->pNewTable, capdbDbStrNDup(db, z, n));
  }
}

/*
** The parser calls this routine after the CREATE VIRTUAL TABLE statement
** has been completely parsed.
*/
void capdbVtabFinishParse(Parse *pParse, Token *pEnd){
  Table *pTab = pParse->pNewTable;  /* The table being constructed */
  capdb *db = pParse->db;         /* The database connection */

  if( pTab==0 ) return;
  assert( IsVirtual(pTab) );
  addArgumentToVtab(pParse);
  pParse->sArg.z = 0;
  if( pTab->u.vtab.nArg<1 ) return;

  /* If the CREATE VIRTUAL TABLE statement is being entered for the
  ** first time (in other words if the virtual table is actually being
  ** created now instead of just being read out of sqlite_schema) then
  ** do additional initialization work and store the statement text
  ** in the sqlite_schema table.
  */
  if( !db->init.busy ){
    char *zStmt;
    char *zWhere;
    int iDb;
    int iReg;
    Vdbe *v;

    capdbMayAbort(pParse);

    /* Compute the complete text of the CREATE VIRTUAL TABLE statement */
    if( pEnd ){
      pParse->sNameToken.n = (int)(pEnd->z - pParse->sNameToken.z) + pEnd->n;
    }
    zStmt = capdbMPrintf(db, "CREATE VIRTUAL TABLE %T", &pParse->sNameToken);

    /* A slot for the record has already been allocated in the
    ** schema table.  We just need to update that slot with all
    ** the information we've collected.
    **
    ** The VM register number pParse->u1.cr.regRowid holds the rowid of an
    ** entry in the sqlite_schema table that was created for this vtab
    ** by capdbStartTable().
    */
    iDb = capdbSchemaToIndex(db, pTab->pSchema);
    assert( pParse->isCreate );
    capdbNestedParse(pParse,
      "UPDATE %Q." LEGACY_SCHEMA_TABLE " "
         "SET type='table', name=%Q, tbl_name=%Q, rootpage=0, sql=%Q "
       "WHERE rowid=#%d",
      db->aDb[iDb].zDbSName,
      pTab->zName,
      pTab->zName,
      zStmt,
      pParse->u1.cr.regRowid
    );
    v = capdbGetVdbe(pParse);
    capdbChangeCookie(pParse, iDb);

    capdbVdbeAddOp0(v, OP_Expire);
    zWhere = capdbMPrintf(db, "name=%Q AND sql=%Q", pTab->zName, zStmt);
    capdbVdbeAddParseSchemaOp(v, iDb, zWhere, 0);
    capdbDbFree(db, zStmt);

    iReg = ++pParse->nMem;
    capdbVdbeLoadString(v, iReg, pTab->zName);
    capdbVdbeAddOp2(v, OP_VCreate, iDb, iReg);
  }else{
    /* If we are rereading the sqlite_schema table create the in-memory
    ** record of the table. */
    Table *pOld;
    Schema *pSchema = pTab->pSchema;
    const char *zName = pTab->zName;
    assert( zName!=0 );
    capdbMarkAllShadowTablesOf(db, pTab);
    pOld = capdbHashInsert(&pSchema->tblHash, zName, pTab);
    if( pOld ){
      capdbOomFault(db);
      assert( pTab==pOld );  /* Malloc must have failed inside HashInsert() */
      return;
    }
    pParse->pNewTable = 0;
  }
}

/*
** The parser calls this routine when it sees the first token
** of an argument to the module name in a CREATE VIRTUAL TABLE statement.
*/
void capdbVtabArgInit(Parse *pParse){
  addArgumentToVtab(pParse);
  pParse->sArg.z = 0;
  pParse->sArg.n = 0;
}

/*
** The parser calls this routine for each token after the first token
** in an argument to the module name in a CREATE VIRTUAL TABLE statement.
*/
void capdbVtabArgExtend(Parse *pParse, Token *p){
  Token *pArg = &pParse->sArg;
  if( pArg->z==0 ){
    pArg->z = p->z;
    pArg->n = p->n;
  }else{
    assert(pArg->z <= p->z);
    pArg->n = (int)(&p->z[p->n] - pArg->z);
  }
}

/*
** Invoke a virtual table constructor (either xCreate or xConnect). The
** pointer to the function to invoke is passed as the fourth parameter
** to this procedure.
*/
static int vtabCallConstructor(
  capdb *db,
  Table *pTab,
  Module *pMod,
  int (*xConstruct)(capdb*,void*,int,const char*const*,capdb_vtab**,char**),
  char **pzErr
){
  VtabCtx sCtx;
  VTable *pVTable;
  int rc;
  const char *const*azArg;
  int nArg = pTab->u.vtab.nArg;
  char *zErr = 0;
  char *zModuleName;
  int iDb;
  VtabCtx *pCtx;

  assert( IsVirtual(pTab) );
  azArg = (const char *const*)pTab->u.vtab.azArg;

  /* Check that the virtual-table is not already being initialized */
  for(pCtx=db->pVtabCtx; pCtx; pCtx=pCtx->pPrior){
    if( pCtx->pTab==pTab ){
      *pzErr = capdbMPrintf(db,
          "vtable constructor called recursively: %s", pTab->zName
      );
      return CAPDB_LOCKED;
    }
  }

  zModuleName = capdbDbStrDup(db, pTab->zName);
  if( !zModuleName ){
    return CAPDB_NOMEM_BKPT;
  }

  pVTable = capdbMallocZero(sizeof(VTable));
  if( !pVTable ){
    capdbOomFault(db);
    capdbDbFree(db, zModuleName);
    return CAPDB_NOMEM_BKPT;
  }
  pVTable->db = db;
  pVTable->pMod = pMod;
  pVTable->eVtabRisk = CAPDB_VTABRISK_Normal;

  iDb = capdbSchemaToIndex(db, pTab->pSchema);
  pTab->u.vtab.azArg[1] = db->aDb[iDb].zDbSName;

  /* Invoke the virtual table constructor */
  assert( &db->pVtabCtx );
  assert( xConstruct );
  sCtx.pTab = pTab;
  sCtx.pVTable = pVTable;
  sCtx.pPrior = db->pVtabCtx;
  sCtx.bDeclared = 0;
  db->pVtabCtx = &sCtx;
  pTab->nTabRef++;
  rc = xConstruct(db, pMod->pAux, nArg, azArg, &pVTable->pVtab, &zErr);
  assert( pTab!=0 );
  assert( pTab->nTabRef>1 || rc!=CAPDB_OK );
  capdbDeleteTable(db, pTab);
  db->pVtabCtx = sCtx.pPrior;
  if( rc==CAPDB_NOMEM ) capdbOomFault(db);
  assert( sCtx.pTab==pTab );

  if( CAPDB_OK!=rc ){
    if( zErr==0 ){
      *pzErr = capdbMPrintf(db, "vtable constructor failed: %s", zModuleName);
    }else {
      *pzErr = capdbMPrintf(db, "%s", zErr);
      capdb_free(zErr);
    }
    capdbDbFree(db, pVTable);
  }else if( ALWAYS(pVTable->pVtab) ){
    /* Justification of ALWAYS():  A correct vtab constructor must allocate
    ** the capdb_vtab object if successful.  */
    memset(pVTable->pVtab, 0, sizeof(pVTable->pVtab[0]));
    pVTable->pVtab->pModule = pMod->pModule;
    pMod->nRefModule++;
    pVTable->nRef = 1;
    if( sCtx.bDeclared==0 ){
      const char *zFormat = "vtable constructor did not declare schema: %s";
      *pzErr = capdbMPrintf(db, zFormat, zModuleName);
      capdbVtabUnlock(pVTable);
      rc = CAPDB_ERROR;
    }else{
      int iCol;
      u16 oooHidden = 0;
      /* If everything went according to plan, link the new VTable structure
      ** into the linked list headed by pTab->u.vtab.p. Then loop through the
      ** columns of the table to see if any of them contain the token "hidden".
      ** If so, set the Column COLFLAG_HIDDEN flag and remove the token from
      ** the type string.  */
      pVTable->pNext = pTab->u.vtab.p;
      pTab->u.vtab.p = pVTable;

      for(iCol=0; iCol<pTab->nCol; iCol++){
        char *zType = capdbColumnType(&pTab->aCol[iCol], "");
        int nType;
        int i = 0;
        nType = capdbStrlen30(zType);
        for(i=0; i<nType; i++){
          if( 0==capdbStrNICmp("hidden", &zType[i], 6)
           && (i==0 || zType[i-1]==' ')
           && (zType[i+6]=='\0' || zType[i+6]==' ')
          ){
            break;
          }
        }
        if( i<nType ){
          int j;
          int nDel = 6 + (zType[i+6] ? 1 : 0);
          for(j=i; (j+nDel)<=nType; j++){
            zType[j] = zType[j+nDel];
          }
          if( zType[i]=='\0' && i>0 ){
            assert(zType[i-1]==' ');
            zType[i-1] = '\0';
          }
          pTab->aCol[iCol].colFlags |= COLFLAG_HIDDEN;
          pTab->tabFlags |= TF_HasHidden;
          oooHidden = TF_OOOHidden;
        }else{
          pTab->tabFlags |= oooHidden;
        }
      }
    }
  }

  capdbDbFree(db, zModuleName);
  return rc;
}

/*
** This function is invoked by the parser to call the xConnect() method
** of the virtual table pTab. If an error occurs, an error code is returned
** and an error left in pParse.
**
** This call is a no-op if table pTab is not a virtual table.
*/
int capdbVtabCallConnect(Parse *pParse, Table *pTab){
  capdb *db = pParse->db;
  const char *zMod;
  Module *pMod;
  int rc;

  assert( pTab );
  assert( IsVirtual(pTab) );
  if( capdbGetVTable(db, pTab) ){
    return CAPDB_OK;
  }

  /* Locate the required virtual table module */
  zMod = pTab->u.vtab.azArg[0];
  pMod = (Module*)capdbHashFind(&db->aModule, zMod);

  if( !pMod ){
    const char *zModule = pTab->u.vtab.azArg[0];
    capdbErrorMsg(pParse, "no such module: %s", zModule);
    rc = CAPDB_ERROR;
  }else{
    char *zErr = 0;
    rc = vtabCallConstructor(db, pTab, pMod, pMod->pModule->xConnect, &zErr);
    if( rc!=CAPDB_OK ){
      capdbErrorMsg(pParse, "%s", zErr);
      pParse->rc = rc;
    }
    capdbDbFree(db, zErr);
  }

  return rc;
}
/*
** Grow the db->aVTrans[] array so that there is room for at least one
** more v-table. Return CAPDB_NOMEM if a malloc fails, or CAPDB_OK otherwise.
*/
static int growVTrans(capdb *db){
  const int ARRAY_INCR = 5;

  /* Grow the capdb.aVTrans array if required */
  if( (db->nVTrans%ARRAY_INCR)==0 ){
    VTable **aVTrans;
    capdb_int64 nBytes = sizeof(capdb_vtab*)*
                                 ((capdb_int64)db->nVTrans + ARRAY_INCR);
    aVTrans = capdbDbRealloc(db, (void *)db->aVTrans, nBytes);
    if( !aVTrans ){
      return CAPDB_NOMEM_BKPT;
    }
    memset(&aVTrans[db->nVTrans], 0, sizeof(capdb_vtab *)*ARRAY_INCR);
    db->aVTrans = aVTrans;
  }

  return CAPDB_OK;
}

/*
** Add the virtual table pVTab to the array capdb.aVTrans[]. Space should
** have already been reserved using growVTrans().
*/
static void addToVTrans(capdb *db, VTable *pVTab){
  /* Add pVtab to the end of capdb.aVTrans */
  db->aVTrans[db->nVTrans++] = pVTab;
  capdbVtabLock(pVTab);
}

/*
** This function is invoked by the vdbe to call the xCreate method
** of the virtual table named zTab in database iDb.
**
** If an error occurs, *pzErr is set to point to an English language
** description of the error and an CAPDB_XXX error code is returned.
** In this case the caller must call capdbDbFree(db, ) on *pzErr.
*/
int capdbVtabCallCreate(capdb *db, int iDb, const char *zTab, char **pzErr){
  int rc = CAPDB_OK;
  Table *pTab;
  Module *pMod;
  const char *zMod;

  pTab = capdbFindTable(db, zTab, db->aDb[iDb].zDbSName);
  assert( pTab && IsVirtual(pTab) && !pTab->u.vtab.p );

  /* Locate the required virtual table module */
  zMod = pTab->u.vtab.azArg[0];
  pMod = (Module*)capdbHashFind(&db->aModule, zMod);

  /* If the module has been registered and includes a Create method,
  ** invoke it now. If the module has not been registered, return an
  ** error. Otherwise, do nothing.
  */
  if( pMod==0 || pMod->pModule->xCreate==0 || pMod->pModule->xDestroy==0 ){
    *pzErr = capdbMPrintf(db, "no such module: %s", zMod);
    rc = CAPDB_ERROR;
  }else{
    rc = vtabCallConstructor(db, pTab, pMod, pMod->pModule->xCreate, pzErr);
  }

  /* Justification of ALWAYS():  The xConstructor method is required to
  ** create a valid capdb_vtab if it returns CAPDB_OK. */
  if( rc==CAPDB_OK && ALWAYS(capdbGetVTable(db, pTab)) ){
    rc = growVTrans(db);
    if( rc==CAPDB_OK ){
      addToVTrans(db, capdbGetVTable(db, pTab));
    }
  }

  return rc;
}

/*
** This function is used to set the schema of a virtual table.  It is only
** valid to call this function from within the xCreate() or xConnect() of a
** virtual table module.
*/
int capdb_declare_vtab(capdb *db, const char *zCreateTable){
  VtabCtx *pCtx;
  int rc = CAPDB_OK;
  Table *pTab;
  Parse sParse;
  int initBusy;
  int i;
  const unsigned char *z;
  static const u8 aKeyword[] = { TK_CREATE, TK_TABLE, 0 };

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zCreateTable==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif

  /* Verify that the first two keywords in the CREATE TABLE statement
  ** really are "CREATE" and "TABLE".  If this is not the case, then
  ** capdb_declare_vtab() is being misused.
  */
  z = (const unsigned char*)zCreateTable;
  for(i=0; aKeyword[i]; i++){
    int tokenType = 0;
    do{
      z += capdbGetToken(z, &tokenType);
    }while( tokenType==TK_SPACE || tokenType==TK_COMMENT );
    if( tokenType!=aKeyword[i] ){
      capdbErrorWithMsg(db, CAPDB_ERROR, "syntax error");
      return CAPDB_ERROR;
    }
  }
 
  capdb_mutex_enter(db->mutex);
  pCtx = db->pVtabCtx;
  if( !pCtx || pCtx->bDeclared ){
    capdbError(db, CAPDB_MISUSE_BKPT);
    capdb_mutex_leave(db->mutex);
    return CAPDB_MISUSE_BKPT;
  }

  pTab = pCtx->pTab;
  assert( IsVirtual(pTab) );

  capdbParseObjectInit(&sParse, db);
  sParse.eParseMode = PARSE_MODE_DECLARE_VTAB;
  sParse.disableTriggers = 1;
  /* We should never be able to reach this point while loading the
  ** schema.  Nevertheless, defend against that (turn off db->init.busy)
  ** in case a bug arises. */
  assert( db->init.busy==0 );
  initBusy = db->init.busy;
  db->init.busy = 0;
  sParse.nQueryLoop = 1;
  if( CAPDB_OK==capdbRunParser(&sParse, zCreateTable) ){
    assert( sParse.pNewTable!=0 );
    assert( !db->mallocFailed );
    assert( IsOrdinaryTable(sParse.pNewTable) );
    assert( sParse.zErrMsg==0 );
    if( !pTab->aCol ){
      Table *pNew = sParse.pNewTable;
      Index *pIdx;
      pTab->aCol = pNew->aCol;
      assert( IsOrdinaryTable(pNew) );
      capdbExprListDelete(db, pNew->u.tab.pDfltList);
      pTab->nNVCol = pTab->nCol = pNew->nCol;
      pTab->tabFlags |= pNew->tabFlags & (TF_WithoutRowid|TF_NoVisibleRowid);
      pNew->nCol = 0;
      pNew->aCol = 0;
      assert( pTab->pIndex==0 );
      assert( HasRowid(pNew) || capdbPrimaryKeyIndex(pNew)!=0 );
      if( !HasRowid(pNew)
       && pCtx->pVTable->pMod->pModule->xUpdate!=0
       && capdbPrimaryKeyIndex(pNew)->nKeyCol!=1
      ){
        /* WITHOUT ROWID virtual tables must either be read-only (xUpdate==0)
        ** or else must have a single-column PRIMARY KEY */
        rc = CAPDB_ERROR;
      }
      pIdx = pNew->pIndex;
      if( pIdx ){
        assert( pIdx->pNext==0 );
        pTab->pIndex = pIdx;
        pNew->pIndex = 0;
        pIdx->pTable = pTab;
      }
    }
    pCtx->bDeclared = 1;
  }else{
    capdbErrorWithMsg(db, CAPDB_ERROR,
          (sParse.zErrMsg ? "%s" : 0), sParse.zErrMsg);
    capdbDbFree(db, sParse.zErrMsg);
    rc = CAPDB_ERROR;
  }
  sParse.eParseMode = PARSE_MODE_NORMAL;

  if( sParse.pVdbe ){
    capdbVdbeFinalize(sParse.pVdbe);
  }
  capdbDeleteTable(db, sParse.pNewTable);
  capdbParseObjectReset(&sParse);
  db->init.busy = initBusy;

  assert( (rc&0xff)==rc );
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** This function is invoked by the vdbe to call the xDestroy method
** of the virtual table named zTab in database iDb. This occurs
** when a DROP TABLE is mentioned.
**
** This call is a no-op if zTab is not a virtual table.
*/
int capdbVtabCallDestroy(capdb *db, int iDb, const char *zTab){
  int rc = CAPDB_OK;
  Table *pTab;

  pTab = capdbFindTable(db, zTab, db->aDb[iDb].zDbSName);
  if( ALWAYS(pTab!=0)
   && ALWAYS(IsVirtual(pTab))
   && ALWAYS(pTab->u.vtab.p!=0)
  ){
    VTable *p;
    int (*xDestroy)(capdb_vtab *);
    for(p=pTab->u.vtab.p; p; p=p->pNext){
      assert( p->pVtab );
      if( p->pVtab->nRef>0 ){
        return CAPDB_LOCKED;
      }
    }
    p = vtabDisconnectAll(db, pTab);
    xDestroy = p->pMod->pModule->xDestroy;
    if( xDestroy==0 ) xDestroy = p->pMod->pModule->xDisconnect;
    assert( xDestroy!=0 );
    pTab->nTabRef++;
    rc = xDestroy(p->pVtab);
    /* Remove the capdb_vtab* from the aVTrans[] array, if applicable */
    if( rc==CAPDB_OK ){
      assert( pTab->u.vtab.p==p && p->pNext==0 );
      p->pVtab = 0;
      pTab->u.vtab.p = 0;
      capdbVtabUnlock(p);
    }
    capdbDeleteTable(db, pTab);
  }

  return rc;
}

/*
** This function invokes either the xRollback or xCommit method
** of each of the virtual tables in the capdb.aVTrans array. The method
** called is identified by the second argument, "offset", which is
** the offset of the method to call in the capdb_module structure.
**
** The array is cleared after invoking the callbacks.
*/
static void callFinaliser(capdb *db, int offset){
  int i;
  if( db->aVTrans ){
    VTable **aVTrans = db->aVTrans;
    db->aVTrans = 0;
    for(i=0; i<db->nVTrans; i++){
      VTable *pVTab = aVTrans[i];
      capdb_vtab *p = pVTab->pVtab;
      if( p ){
        int (*x)(capdb_vtab *);
        x = *(int (**)(capdb_vtab *))((char *)p->pModule + offset);
        if( x ) x(p);
      }
      pVTab->iSavepoint = 0;
      capdbVtabUnlock(pVTab);
    }
    capdbDbFree(db, aVTrans);
    db->nVTrans = 0;
  }
}

/*
** Invoke the xSync method of all virtual tables in the capdb.aVTrans
** array. Return the error code for the first error that occurs, or
** CAPDB_OK if all xSync operations are successful.
**
** If an error message is available, leave it in p->zErrMsg.
*/
int capdbVtabSync(capdb *db, Vdbe *p){
  int i;
  int rc = CAPDB_OK;
  VTable **aVTrans = db->aVTrans;

  db->aVTrans = 0;
  for(i=0; rc==CAPDB_OK && i<db->nVTrans; i++){
    int (*x)(capdb_vtab *);
    capdb_vtab *pVtab = aVTrans[i]->pVtab;
    if( pVtab && (x = pVtab->pModule->xSync)!=0 ){
      rc = x(pVtab);
      capdbVtabImportErrmsg(p, pVtab);
    }
  }
  db->aVTrans = aVTrans;
  return rc;
}

/*
** Invoke the xRollback method of all virtual tables in the
** capdb.aVTrans array. Then clear the array itself.
*/
int capdbVtabRollback(capdb *db){
  callFinaliser(db, offsetof(capdb_module,xRollback));
  return CAPDB_OK;
}

/*
** Invoke the xCommit method of all virtual tables in the
** capdb.aVTrans array. Then clear the array itself.
*/
int capdbVtabCommit(capdb *db){
  callFinaliser(db, offsetof(capdb_module,xCommit));
  return CAPDB_OK;
}

/*
** If the virtual table pVtab supports the transaction interface
** (xBegin/xRollback/xCommit and optionally xSync) and a transaction is
** not currently open, invoke the xBegin method now.
**
** If the xBegin call is successful, place the capdb_vtab pointer
** in the capdb.aVTrans array.
*/
int capdbVtabBegin(capdb *db, VTable *pVTab){
  int rc = CAPDB_OK;
  const capdb_module *pModule;

  /* Special case: If db->aVTrans is NULL and db->nVTrans is greater
  ** than zero, then this function is being called from within a
  ** virtual module xSync() callback. It is illegal to write to
  ** virtual module tables in this case, so return CAPDB_LOCKED.
  */
  if( capdbVtabInSync(db) ){
    return CAPDB_LOCKED;
  }
  if( !pVTab ){
    return CAPDB_OK;
  }
  pModule = pVTab->pVtab->pModule;

  if( pModule->xBegin ){
    int i;

    /* If pVtab is already in the aVTrans array, return early */
    for(i=0; i<db->nVTrans; i++){
      if( db->aVTrans[i]==pVTab ){
        return CAPDB_OK;
      }
    }

    /* Invoke the xBegin method. If successful, add the vtab to the
    ** capdb.aVTrans[] array. */
    rc = growVTrans(db);
    if( rc==CAPDB_OK ){
      rc = pModule->xBegin(pVTab->pVtab);
      if( rc==CAPDB_OK ){
        int iSvpt = db->nStatement + db->nSavepoint;
        addToVTrans(db, pVTab);
        if( iSvpt && pModule->xSavepoint ){
          pVTab->iSavepoint = iSvpt;
          rc = pModule->xSavepoint(pVTab->pVtab, iSvpt-1);
        }
      }
    }
  }
  return rc;
}

/*
** Invoke either the xSavepoint, xRollbackTo or xRelease method of all
** virtual tables that currently have an open transaction. Pass iSavepoint
** as the second argument to the virtual table method invoked.
**
** If op is SAVEPOINT_BEGIN, the xSavepoint method is invoked. If it is
** SAVEPOINT_ROLLBACK, the xRollbackTo method. Otherwise, if op is
** SAVEPOINT_RELEASE, then the xRelease method of each virtual table with
** an open transaction is invoked.
**
** If any virtual table method returns an error code other than CAPDB_OK,
** processing is abandoned and the error returned to the caller of this
** function immediately. If all calls to virtual table methods are successful,
** CAPDB_OK is returned.
*/
int capdbVtabSavepoint(capdb *db, int op, int iSavepoint){
  int rc = CAPDB_OK;

  assert( op==SAVEPOINT_RELEASE||op==SAVEPOINT_ROLLBACK||op==SAVEPOINT_BEGIN );
  assert( iSavepoint>=-1 );
  if( db->aVTrans ){
    int i;
    for(i=0; rc==CAPDB_OK && i<db->nVTrans; i++){
      VTable *pVTab = db->aVTrans[i];
      const capdb_module *pMod = pVTab->pMod->pModule;
      if( pVTab->pVtab && pMod->iVersion>=2 ){
        int (*xMethod)(capdb_vtab *, int);
        capdbVtabLock(pVTab);
        switch( op ){
          case SAVEPOINT_BEGIN:
            xMethod = pMod->xSavepoint;
            pVTab->iSavepoint = iSavepoint+1;
            break;
          case SAVEPOINT_ROLLBACK:
            xMethod = pMod->xRollbackTo;
            break;
          default:
            xMethod = pMod->xRelease;
            break;
        }
        if( xMethod && pVTab->iSavepoint>iSavepoint ){
          u64 savedFlags = (db->flags & CAPDB_Defensive);
          db->flags &= ~(u64)CAPDB_Defensive;
          rc = xMethod(pVTab->pVtab, iSavepoint);
          db->flags |= savedFlags;
        }
        capdbVtabUnlock(pVTab);
      }
    }
  }
  return rc;
}

/*
** The first parameter (pDef) is a function implementation.  The
** second parameter (pExpr) is the first argument to this function.
** If pExpr is a column in a virtual table, then let the virtual
** table implementation have an opportunity to overload the function.
**
** This routine is used to allow virtual table implementations to
** overload MATCH, LIKE, GLOB, and REGEXP operators.
**
** Return either the pDef argument (indicating no change) or a
** new FuncDef structure that is marked as ephemeral using the
** CAPDB_FUNC_EPHEM flag.
*/
FuncDef *capdbVtabOverloadFunction(
  capdb *db,    /* Database connection for reporting malloc problems */
  FuncDef *pDef,  /* Function to possibly overload */
  int nArg,       /* Number of arguments to the function */
  Expr *pExpr     /* First argument to the function */
){
  Table *pTab;
  capdb_vtab *pVtab;
  capdb_module *pMod;
  void (*xSFunc)(capdb_context*,int,capdb_value**) = 0;
  void *pArg = 0;
  FuncDef *pNew;
  int rc = 0;

  /* Check to see the left operand is a column in a virtual table */
  if( NEVER(pExpr==0) ) return pDef;
  if( pExpr->op!=TK_COLUMN ) return pDef;
  assert( ExprUseYTab(pExpr) );
  pTab = pExpr->y.pTab;
  if( NEVER(pTab==0) ) return pDef;
  if( !IsVirtual(pTab) ) return pDef;
  pVtab = capdbGetVTable(db, pTab)->pVtab;
  assert( pVtab!=0 );
  assert( pVtab->pModule!=0 );
  pMod = (capdb_module *)pVtab->pModule;
  if( pMod->xFindFunction==0 ) return pDef;

  /* Call the xFindFunction method on the virtual table implementation
  ** to see if the implementation wants to overload this function.
  **
  ** Though undocumented, we have historically always invoked xFindFunction
  ** with an all lower-case function name.  Continue in this tradition to
  ** avoid any chance of an incompatibility.
  */
#ifdef CAPDB_DEBUG
  {
    int i;
    for(i=0; pDef->zName[i]; i++){
      unsigned char x = (unsigned char)pDef->zName[i];
      assert( x==capdbUpperToLower[x] );
    }
  }
#endif
  rc = pMod->xFindFunction(pVtab, nArg, pDef->zName, &xSFunc, &pArg);
  if( rc==0 ){
    return pDef;
  }

  /* Create a new ephemeral function definition for the overloaded
  ** function */
  pNew = capdbDbMallocZero(db, sizeof(*pNew)
                             + capdbStrlen30(pDef->zName) + 1);
  if( pNew==0 ){
    return pDef;
  }
  *pNew = *pDef;
  pNew->zName = (const char*)&pNew[1];
  memcpy((char*)&pNew[1], pDef->zName, capdbStrlen30(pDef->zName)+1);
  pNew->xSFunc = xSFunc;
  pNew->pUserData = pArg;
  pNew->funcFlags |= CAPDB_FUNC_EPHEM;
  return pNew;
}

/*
** Make sure virtual table pTab is contained in the pParse->apVirtualLock[]
** array so that an OP_VBegin will get generated for it.  Add pTab to the
** array if it is missing.  If pTab is already in the array, this routine
** is a no-op.
*/
void capdbVtabMakeWritable(Parse *pParse, Table *pTab){
  Parse *pToplevel = capdbParseToplevel(pParse);
  int i, n;
  Table **apVtabLock;

  assert( IsVirtual(pTab) );
  for(i=0; i<pToplevel->nVtabLock; i++){
    if( pTab==pToplevel->apVtabLock[i] ) return;
  }
  n = (pToplevel->nVtabLock+1)*sizeof(pToplevel->apVtabLock[0]);
  apVtabLock = capdbRealloc(pToplevel->apVtabLock, n);
  if( apVtabLock ){
    pToplevel->apVtabLock = apVtabLock;
    pToplevel->apVtabLock[pToplevel->nVtabLock++] = pTab;
  }else{
    capdbOomFault(pToplevel->db);
  }
}

/*
** Check to see if virtual table module pMod can be have an eponymous
** virtual table instance.  If it can, create one if one does not already
** exist. Return non-zero if either the eponymous virtual table instance
** exists when this routine returns or if an attempt to create it failed
** and an error message was left in pParse.
**
** An eponymous virtual table instance is one that is named after its
** module, and more importantly, does not require a CREATE VIRTUAL TABLE
** statement in order to come into existence.  Eponymous virtual table
** instances always exist.  They cannot be DROP-ed.
**
** Any virtual table module for which xConnect and xCreate are the same
** method can have an eponymous virtual table instance.
*/
int capdbVtabEponymousTableInit(Parse *pParse, Module *pMod){
  const capdb_module *pModule = pMod->pModule;
  Table *pTab;
  char *zErr = 0;
  int rc;
  capdb *db = pParse->db;
  if( pMod->pEpoTab ) return 1;
  if( pModule->xCreate!=0 && pModule->xCreate!=pModule->xConnect ) return 0;
  pTab = capdbDbMallocZero(db, sizeof(Table));
  if( pTab==0 ) return 0;
  pTab->zName = capdbDbStrDup(db, pMod->zName);
  if( pTab->zName==0 ){
    capdbDbFree(db, pTab);
    return 0;
  }
  pMod->pEpoTab = pTab;
  pTab->nTabRef = 1;
  pTab->eTabType = TABTYP_VTAB;
  pTab->pSchema = db->aDb[0].pSchema;
  assert( pTab->u.vtab.nArg==0 );
  pTab->iPKey = -1;
  pTab->tabFlags |= TF_Eponymous;
  addModuleArgument(pParse, pTab, capdbDbStrDup(db, pTab->zName));
  addModuleArgument(pParse, pTab, 0);
  addModuleArgument(pParse, pTab, capdbDbStrDup(db, pTab->zName));
  db->nSchemaLock++;
  rc = vtabCallConstructor(db, pTab, pMod, pModule->xConnect, &zErr);
  db->nSchemaLock--;
  if( rc ){
    capdbErrorMsg(pParse, "%s", zErr);
    pParse->rc = rc;
    capdbDbFree(db, zErr);
    capdbVtabEponymousTableClear(db, pMod);
  }
  return 1;
}

/*
** Erase the eponymous virtual table instance associated with
** virtual table module pMod, if it exists.
*/
void capdbVtabEponymousTableClear(capdb *db, Module *pMod){
  Table *pTab = pMod->pEpoTab;
  if( pTab!=0 ){
    /* Mark the table as Ephemeral prior to deleting it, so that the
    ** capdbDeleteTable() routine will know that it is not stored in
    ** the schema. */
    pTab->tabFlags |= TF_Ephemeral;
    capdbDeleteTable(db, pTab);
    pMod->pEpoTab = 0;
  }
}

/*
** Return the ON CONFLICT resolution mode in effect for the virtual
** table update operation currently in progress.
**
** The results of this routine are undefined unless it is called from
** within an xUpdate method.
*/
int capdb_vtab_on_conflict(capdb *db){
  static const unsigned char aMap[] = {
    CAPDB_ROLLBACK, CAPDB_ABORT, CAPDB_FAIL, CAPDB_IGNORE, CAPDB_REPLACE
  };
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  assert( OE_Rollback==1 && OE_Abort==2 && OE_Fail==3 );
  assert( OE_Ignore==4 && OE_Replace==5 );
  assert( db->vtabOnConflict>=1 && db->vtabOnConflict<=5 );
  return (int)aMap[db->vtabOnConflict-1];
}

/*
** Call from within the xCreate() or xConnect() methods to provide
** the SQLite core with additional information about the behavior
** of the virtual table being implemented.
*/
int capdb_vtab_config(capdb *db, int op, ...){
  va_list ap;
  int rc = CAPDB_OK;
  VtabCtx *p;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  p = db->pVtabCtx;
  if( !p ){
    rc = CAPDB_MISUSE_BKPT;
  }else{
    assert( p->pTab==0 || IsVirtual(p->pTab) );
    va_start(ap, op);
    switch( op ){
      case CAPDB_VTAB_CONSTRAINT_SUPPORT: {
        p->pVTable->bConstraint = (u8)va_arg(ap, int);
        break;
      }
      case CAPDB_VTAB_INNOCUOUS: {
        p->pVTable->eVtabRisk = CAPDB_VTABRISK_Low;
        break;
      }
      case CAPDB_VTAB_DIRECTONLY: {
        p->pVTable->eVtabRisk = CAPDB_VTABRISK_High;
        break;
      }
      case CAPDB_VTAB_USES_ALL_SCHEMAS: {
        p->pVTable->bAllSchemas = 1;
        break;
      }
      default: {
        rc = CAPDB_MISUSE_BKPT;
        break;
      }
    }
    va_end(ap);
  }

  if( rc!=CAPDB_OK ) capdbError(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

#endif /* CAPDB_OMIT_VIRTUALTABLE */
