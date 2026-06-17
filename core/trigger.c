/*
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation for TRIGGERs
*/
#include "capdbInt.h"

#ifndef CAPDB_OMIT_TRIGGER
/*
** Delete a linked list of TriggerStep structures.
*/
void capdbDeleteTriggerStep(capdb *db, TriggerStep *pTriggerStep){
  while( pTriggerStep ){
    TriggerStep * pTmp = pTriggerStep;
    pTriggerStep = pTriggerStep->pNext;

    capdbExprDelete(db, pTmp->pWhere);
    capdbExprListDelete(db, pTmp->pExprList);
    capdbSelectDelete(db, pTmp->pSelect);
    capdbIdListDelete(db, pTmp->pIdList);
    capdbUpsertDelete(db, pTmp->pUpsert);
    capdbSrcListDelete(db, pTmp->pSrc);
    capdbDbFree(db, pTmp->zSpan);

    capdbDbFree(db, pTmp);
  }
}

/*
** Given table pTab, return a list of all the triggers attached to 
** the table. The list is connected by Trigger.pNext pointers.
**
** All of the triggers on pTab that are in the same database as pTab
** are already attached to pTab->pTrigger.  But there might be additional
** triggers on pTab in the TEMP schema.  This routine prepends all
** TEMP triggers on pTab to the beginning of the pTab->pTrigger list
** and returns the combined list.
**
** To state it another way:  This routine returns a list of all triggers
** that fire off of pTab.  The list will include any TEMP triggers on
** pTab as well as the triggers lised in pTab->pTrigger.
*/
Trigger *capdbTriggerList(Parse *pParse, Table *pTab){
  Schema *pTmpSchema;       /* Schema of the pTab table */
  Trigger *pList;           /* List of triggers to return */
  HashElem *p;              /* Loop variable for TEMP triggers */

  assert( pParse->disableTriggers==0 );
  pTmpSchema = pParse->db->aDb[1].pSchema;
  p = sqliteHashFirst(&pTmpSchema->trigHash);
  pList = pTab->pTrigger;
  while( p ){
    Trigger *pTrig = (Trigger *)sqliteHashData(p);
    if( pTrig->pTabSchema==pTab->pSchema
     && pTrig->table
     && 0==capdbStrICmp(pTrig->table, pTab->zName)
     && (pTrig->pTabSchema!=pTmpSchema || pTrig->bReturning)
    ){
      pTrig->pNext = pList;
      pList = pTrig;
    }else if( pTrig->op==TK_RETURNING ){
#ifndef CAPDB_OMIT_VIRTUALTABLE
      assert( pParse->db->pVtabCtx==0 );
#endif
      assert( pParse->bReturning );
      assert( !pParse->isCreate );
      assert( &(pParse->u1.d.pReturning->retTrig) == pTrig );
      pTrig->table = pTab->zName;
      pTrig->pTabSchema = pTab->pSchema;
      pTrig->pNext = pList;
      pList = pTrig;
    }        
    p = sqliteHashNext(p);    
  }
#if 0
  if( pList ){
    Trigger *pX;
    printf("Triggers for %s:", pTab->zName);
    for(pX=pList; pX; pX=pX->pNext){
      printf(" %s", pX->zName);
    }
    printf("\n");
    fflush(stdout);
  }
#endif
  return pList;  
}

/*
** This is called by the parser when it sees a CREATE TRIGGER statement
** up to the point of the BEGIN before the trigger actions.  A Trigger
** structure is generated based on the information available and stored
** in pParse->pNewTrigger.  After the trigger actions have been parsed, the
** capdbFinishTrigger() function is called to complete the trigger
** construction process.
*/
void capdbBeginTrigger(
  Parse *pParse,      /* The parse context of the CREATE TRIGGER statement */
  Token *pName1,      /* The name of the trigger */
  Token *pName2,      /* The name of the trigger */
  int tr_tm,          /* One of TK_BEFORE, TK_AFTER, TK_INSTEAD */
  int op,             /* One of TK_INSERT, TK_UPDATE, TK_DELETE */
  IdList *pColumns,   /* column list if this is an UPDATE OF trigger */
  SrcList *pTableName,/* The name of the table/view the trigger applies to */
  Expr *pWhen,        /* WHEN clause */
  int isTemp,         /* True if the TEMPORARY keyword is present */
  int noErr           /* Suppress errors if the trigger already exists */
){
  Trigger *pTrigger = 0;  /* The new trigger */
  Table *pTab;            /* Table that the trigger fires off of */
  char *zName = 0;        /* Name of the trigger */
  capdb *db = pParse->db;  /* The database connection */
  int iDb;                /* The database to store the trigger in */
  Token *pName;           /* The unqualified db name */
  DbFixer sFix;           /* State vector for the DB fixer */

  assert( pName1!=0 );   /* pName1->z might be NULL, but not pName1 itself */
  assert( pName2!=0 );
  assert( op==TK_INSERT || op==TK_UPDATE || op==TK_DELETE );
  assert( op>0 && op<0xff );
  if( isTemp ){
    /* If TEMP was specified, then the trigger name may not be qualified. */
    if( pName2->n>0 ){
      capdbErrorMsg(pParse, "temporary trigger may not have qualified name");
      goto trigger_cleanup;
    }
    iDb = 1;
    pName = pName1;
  }else{
    /* Figure out the db that the trigger will be created in */
    iDb = capdbTwoPartName(pParse, pName1, pName2, &pName);
    if( iDb<0 ){
      goto trigger_cleanup;
    }
  }
  if( !pTableName || db->mallocFailed ){
    goto trigger_cleanup;
  }

  /* A long-standing parser bug is that this syntax was allowed:
  **
  **    CREATE TRIGGER attached.demo AFTER INSERT ON attached.tab ....
  **                                                 ^^^^^^^^
  **
  ** To maintain backwards compatibility, ignore the database
  ** name on pTableName if we are reparsing out of the schema table
  */
  if( db->init.busy && iDb!=1 ){
    assert( pTableName->a[0].fg.fixedSchema==0 );
    assert( pTableName->a[0].fg.isSubquery==0 );
    capdbDbFree(db, pTableName->a[0].u4.zDatabase);
    pTableName->a[0].u4.zDatabase = 0;
  }

  /* If the trigger name was unqualified, and the table is a temp table,
  ** then set iDb to 1 to create the trigger in the temporary database.
  ** If capdbSrcListLookup() returns 0, indicating the table does not
  ** exist, the error is caught by the block below.
  */
  pTab = capdbSrcListLookup(pParse, pTableName);
  if( db->init.busy==0 && pName2->n==0 && pTab
        && pTab->pSchema==db->aDb[1].pSchema ){
    iDb = 1;
  }

  /* Ensure the table name matches database name and that the table exists */
  if( db->mallocFailed ) goto trigger_cleanup;
  assert( pTableName->nSrc==1 );
  capdbFixInit(&sFix, pParse, iDb, "trigger", pName);
  if( capdbFixSrcList(&sFix, pTableName) ){
    goto trigger_cleanup;
  }
  pTab = capdbSrcListLookup(pParse, pTableName);
  if( !pTab ){
    /* The table does not exist. */
    goto trigger_orphan_error;
  }
  if( IsVirtual(pTab) ){
    capdbErrorMsg(pParse, "cannot create triggers on virtual tables");
    goto trigger_orphan_error;
  }
  if( (pTab->tabFlags & TF_Shadow)!=0 && capdbReadOnlyShadowTables(db) ){
    capdbErrorMsg(pParse, "cannot create triggers on shadow tables");
    goto trigger_orphan_error;
  }

  /* Check that the trigger name is not reserved and that no trigger of the
  ** specified name exists */
  zName = capdbNameFromToken(db, pName);
  if( zName==0 ){
    assert( db->mallocFailed );
    goto trigger_cleanup;
  }
  if( capdbCheckObjectName(pParse, zName, "trigger", pTab->zName) ){
    goto trigger_cleanup;
  }
  assert( capdbSchemaMutexHeld(db, iDb, 0) );
  if( !IN_RENAME_OBJECT ){
    if( capdbHashFind(&(db->aDb[iDb].pSchema->trigHash),zName) ){
      if( !noErr ){
        capdbErrorMsg(pParse, "trigger %T already exists", pName);
      }else{
        assert( !db->init.busy );
        capdbCodeVerifySchema(pParse, iDb);
        VVA_ONLY( pParse->ifNotExists = 1; )
      }
      goto trigger_cleanup;
    }
  }

  /* NB: The CAPDB_ALLOW_TRIGGERS_ON_SYSTEM_TABLES compile-time option is
  ** experimental and unsupported. Do not use it unless understand the
  ** implications and you cannot get by without this capability. */
#if !defined(CAPDB_ALLOW_TRIGGERS_ON_SYSTEM_TABLES) /* Experimental */
  /* Do not create a trigger on a system table */
  if( capdbStrNICmp(pTab->zName, "sqlite_", 7)==0 ){
    capdbErrorMsg(pParse, "cannot create trigger on system table");
    goto trigger_cleanup;
  }
#endif

  /* INSTEAD of triggers are only for views and views only support INSTEAD
  ** of triggers.
  */
  if( IsView(pTab) && tr_tm!=TK_INSTEAD ){
    capdbErrorMsg(pParse, "cannot create %s trigger on view: %S", 
        (tr_tm == TK_BEFORE)?"BEFORE":"AFTER", pTableName->a);
    goto trigger_orphan_error;
  }
  if( !IsView(pTab) && tr_tm==TK_INSTEAD ){
    capdbErrorMsg(pParse, "cannot create INSTEAD OF"
        " trigger on table: %S", pTableName->a);
    goto trigger_orphan_error;
  }

#ifndef CAPDB_OMIT_AUTHORIZATION
  if( !IN_RENAME_OBJECT ){
    int iTabDb = capdbSchemaToIndex(db, pTab->pSchema);
    int code = CAPDB_CREATE_TRIGGER;
    const char *zDb = db->aDb[iTabDb].zDbSName;
    const char *zDbTrig = isTemp ? db->aDb[1].zDbSName : zDb;
    if( iTabDb==1 || isTemp ) code = CAPDB_CREATE_TEMP_TRIGGER;
    if( capdbAuthCheck(pParse, code, zName, pTab->zName, zDbTrig) ){
      goto trigger_cleanup;
    }
    if( capdbAuthCheck(pParse, CAPDB_INSERT, SCHEMA_TABLE(iTabDb),0,zDb)){
      goto trigger_cleanup;
    }
  }
#endif

  /* INSTEAD OF triggers can only appear on views and BEFORE triggers
  ** cannot appear on views.  So we might as well translate every
  ** INSTEAD OF trigger into a BEFORE trigger.  It simplifies code
  ** elsewhere.
  */
  if (tr_tm == TK_INSTEAD){
    tr_tm = TK_BEFORE;
  }

  /* Build the Trigger object */
  pTrigger = (Trigger*)capdbDbMallocZero(db, sizeof(Trigger));
  if( pTrigger==0 ) goto trigger_cleanup;
  pTrigger->zName = zName;
  zName = 0;
  pTrigger->table = capdbDbStrDup(db, pTableName->a[0].zName);
  pTrigger->pSchema = db->aDb[iDb].pSchema;
  pTrigger->pTabSchema = pTab->pSchema;
  pTrigger->op = (u8)op;
  pTrigger->tr_tm = tr_tm==TK_BEFORE ? TRIGGER_BEFORE : TRIGGER_AFTER;
  if( IN_RENAME_OBJECT ){
    capdbRenameTokenRemap(pParse, pTrigger->table, pTableName->a[0].zName);
    pTrigger->pWhen = pWhen;
    pWhen = 0;
  }else{
    pTrigger->pWhen = capdbExprDup(db, pWhen, EXPRDUP_REDUCE);
  }
  pTrigger->pColumns = pColumns;
  pColumns = 0;
  assert( pParse->pNewTrigger==0 );
  pParse->pNewTrigger = pTrigger;

trigger_cleanup:
  capdbDbFree(db, zName);
  capdbSrcListDelete(db, pTableName);
  capdbIdListDelete(db, pColumns);
  capdbExprDelete(db, pWhen);
  if( !pParse->pNewTrigger ){
    capdbDeleteTrigger(db, pTrigger);
  }else{
    assert( pParse->pNewTrigger==pTrigger );
  }
  return;

trigger_orphan_error:
  if( db->init.iDb==1 ){
    /* Ticket #3810.
    ** Normally, whenever a table is dropped, all associated triggers are
    ** dropped too.  But if a TEMP trigger is created on a non-TEMP table
    ** and the table is dropped by a different database connection, the
    ** trigger is not visible to the database connection that does the
    ** drop so the trigger cannot be dropped.  This results in an
    ** "orphaned trigger" - a trigger whose associated table is missing.
    **
    ** 2020-11-05 see also https://sqlite.org/forum/forumpost/157dc791df
    */
    db->init.orphanTrigger = 1;
  }
  goto trigger_cleanup;
}

/*
** This routine is called after all of the trigger actions have been parsed
** in order to complete the process of building the trigger.
*/
void capdbFinishTrigger(
  Parse *pParse,          /* Parser context */
  TriggerStep *pStepList, /* The triggered program */
  Token *pAll             /* Token that describes the complete CREATE TRIGGER */
){
  Trigger *pTrig = pParse->pNewTrigger;   /* Trigger being finished */
  char *zName;                            /* Name of trigger */
  capdb *db = pParse->db;               /* The database */
  DbFixer sFix;                           /* Fixer object */
  int iDb;                                /* Database containing the trigger */
  Token nameToken;                        /* Trigger name for error reporting */

  pParse->pNewTrigger = 0;
  if( NEVER(pParse->nErr) || !pTrig ) goto triggerfinish_cleanup;
  zName = pTrig->zName;
  iDb = capdbSchemaToIndex(pParse->db, pTrig->pSchema);
  assert( iDb>=00 && iDb<db->nDb );
  pTrig->step_list = pStepList;
  while( pStepList ){
    pStepList->pTrig = pTrig;
    pStepList = pStepList->pNext;
  }
  capdbTokenInit(&nameToken, pTrig->zName);
  capdbFixInit(&sFix, pParse, iDb, "trigger", &nameToken);
  if( capdbFixTriggerStep(&sFix, pTrig->step_list) 
   || capdbFixExpr(&sFix, pTrig->pWhen) 
  ){
    goto triggerfinish_cleanup;
  }

#ifndef CAPDB_OMIT_ALTERTABLE
  if( IN_RENAME_OBJECT ){
    assert( !db->init.busy );
    pParse->pNewTrigger = pTrig;
    pTrig = 0;
  }else
#endif

  /* if we are not initializing,
  ** build the sqlite_schema entry
  */
  if( !db->init.busy ){
    Vdbe *v;
    char *z;

    /* If this is a new CREATE TABLE statement, and if shadow tables
    ** are read-only, and the trigger makes a change to a shadow table,
    ** then raise an error - do not allow the trigger to be created. */
    if( capdbReadOnlyShadowTables(db) ){
      TriggerStep *pStep;
      for(pStep=pTrig->step_list; pStep; pStep=pStep->pNext){
        if( pStep->pSrc!=0
         && capdbShadowTableName(db, pStep->pSrc->a[0].zName)
        ){
          capdbErrorMsg(pParse, 
            "trigger \"%s\" may not write to shadow table \"%s\"",
            pTrig->zName, pStep->pSrc->a[0].zName);
          goto triggerfinish_cleanup;
        }
      }
    }

    /* Make an entry in the sqlite_schema table */
    v = capdbGetVdbe(pParse);
    if( v==0 ) goto triggerfinish_cleanup;
    capdbBeginWriteOperation(pParse, 0, iDb);
    z = capdbDbStrNDup(db, (char*)pAll->z, pAll->n);
    testcase( z==0 );
    capdbNestedParse(pParse,
       "INSERT INTO %Q." LEGACY_SCHEMA_TABLE
       " VALUES('trigger',%Q,%Q,0,'CREATE TRIGGER %q')",
       db->aDb[iDb].zDbSName, zName,
       pTrig->table, z);
    capdbDbFree(db, z);
    capdbChangeCookie(pParse, iDb);
    capdbVdbeAddParseSchemaOp(v, iDb,
        capdbMPrintf(db, "type='trigger' AND name='%q'", zName), 0);
  }

  if( db->init.busy ){
    Trigger *pLink = pTrig;
    Hash *pHash = &db->aDb[iDb].pSchema->trigHash;
    assert( capdbSchemaMutexHeld(db, iDb, 0) );
    assert( pLink!=0 );
    pTrig = capdbHashInsert(pHash, zName, pTrig);
    if( pTrig ){
      capdbOomFault(db);
    }else if( pLink->pSchema==pLink->pTabSchema ){
      Table *pTab;
      pTab = capdbHashFind(&pLink->pTabSchema->tblHash, pLink->table);
      assert( pTab!=0 );
      pLink->pNext = pTab->pTrigger;
      pTab->pTrigger = pLink;
    }
  }

triggerfinish_cleanup:
  capdbDeleteTrigger(db, pTrig);
  assert( IN_RENAME_OBJECT || !pParse->pNewTrigger );
  capdbDeleteTriggerStep(db, pStepList);
}

/*
** Duplicate a range of text from an SQL statement, then convert all
** whitespace characters into ordinary space characters.
*/
static char *triggerSpanDup(capdb *db, const char *zStart, const char *zEnd){
  char *z = capdbDbSpanDup(db, zStart, zEnd);
  int i;
  if( z ) for(i=0; z[i]; i++) if( capdbIsspace(z[i]) ) z[i] = ' ';
  return z;
}    

/*
** Turn a SELECT statement (that the pSelect parameter points to) into
** a trigger step.  Return a pointer to a TriggerStep structure.
**
** The parser calls this routine when it finds a SELECT statement in
** body of a TRIGGER.  
*/
TriggerStep *capdbTriggerSelectStep(
  capdb *db,                /* Database connection */
  Select *pSelect,            /* The SELECT statement */
  const char *zStart,         /* Start of SQL text */
  const char *zEnd            /* End of SQL text */
){
  TriggerStep *pTriggerStep = capdbDbMallocZero(db, sizeof(TriggerStep));
  if( pTriggerStep==0 ) {
    capdbSelectDelete(db, pSelect);
    return 0;
  }
  pTriggerStep->op = TK_SELECT;
  pTriggerStep->pSelect = pSelect;
  pTriggerStep->orconf = OE_Default;
  pTriggerStep->zSpan = triggerSpanDup(db, zStart, zEnd);
  return pTriggerStep;
}

/*
** Allocate space to hold a new trigger step.  The allocated space
** holds both the TriggerStep object and the TriggerStep.target.z string.
**
** If an OOM error occurs, NULL is returned and db->mallocFailed is set.
*/
static TriggerStep *triggerStepAllocate(
  Parse *pParse,              /* Parser context */
  u8 op,                      /* Trigger opcode */
  SrcList *pTabList,          /* Target table */
  const char *zStart,         /* Start of SQL text */
  const char *zEnd            /* End of SQL text */
){
  Trigger *pNew = pParse->pNewTrigger;
  capdb *db = pParse->db;
  TriggerStep *pTriggerStep = 0;

  if( pParse->nErr==0 ){
    if( pNew 
     && pNew->pSchema!=db->aDb[1].pSchema 
     && pTabList->a[0].u4.zDatabase 
    ){
      capdbErrorMsg(pParse, 
          "qualified table names are not allowed on INSERT, UPDATE, and DELETE "
          "statements within triggers");
    }else{
      pTriggerStep = capdbDbMallocZero(db, sizeof(TriggerStep));
      if( pTriggerStep ){
        pTriggerStep->pSrc = capdbSrcListDup(db, pTabList, EXPRDUP_REDUCE);
        pTriggerStep->op = op;
        pTriggerStep->zSpan = triggerSpanDup(db, zStart, zEnd);
        if( pTriggerStep->pSrc && IN_RENAME_OBJECT ){
          capdbRenameTokenRemap(pParse, 
              pTriggerStep->pSrc->a[0].zName, 
              pTabList->a[0].zName
          );
        }
      }
    }
  }

  capdbSrcListDelete(db, pTabList);
  return pTriggerStep;
}

/*
** Build a trigger step out of an INSERT statement.  Return a pointer
** to the new trigger step.
**
** The parser calls this routine when it sees an INSERT inside the
** body of a trigger.
*/
TriggerStep *capdbTriggerInsertStep(
  Parse *pParse,      /* Parser */
  SrcList *pTabList,  /* Table to INSERT into */
  IdList *pColumn,    /* List of columns in pTableName to insert into */
  Select *pSelect,    /* A SELECT statement that supplies values */
  u8 orconf,          /* The conflict algorithm (OE_Abort, OE_Replace, etc.) */
  Upsert *pUpsert,    /* ON CONFLICT clauses for upsert */
  const char *zStart, /* Start of SQL text */
  const char *zEnd    /* End of SQL text */
){
  capdb *db = pParse->db;
  TriggerStep *pTriggerStep;

  assert(pSelect != 0 || db->mallocFailed);

  pTriggerStep = triggerStepAllocate(pParse, TK_INSERT, pTabList, zStart, zEnd);
  if( pTriggerStep ){
    if( IN_RENAME_OBJECT ){
      pTriggerStep->pSelect = pSelect;
      pSelect = 0;
    }else{
      pTriggerStep->pSelect = capdbSelectDup(db, pSelect, EXPRDUP_REDUCE);
    }
    pTriggerStep->pIdList = pColumn;
    pTriggerStep->pUpsert = pUpsert;
    pTriggerStep->orconf = orconf;
    if( pUpsert ){
      capdbHasExplicitNulls(pParse, pUpsert->pUpsertTarget);
    }
  }else{
    testcase( pColumn );
    capdbIdListDelete(db, pColumn);
    testcase( pUpsert );
    capdbUpsertDelete(db, pUpsert);
  }
  capdbSelectDelete(db, pSelect);

  return pTriggerStep;
}

/*
** Construct a trigger step that implements an UPDATE statement and return
** a pointer to that trigger step.  The parser calls this routine when it
** sees an UPDATE statement inside the body of a CREATE TRIGGER.
*/
TriggerStep *capdbTriggerUpdateStep(
  Parse *pParse,          /* Parser */
  SrcList *pTabList,   /* Name of the table to be updated */
  SrcList *pFrom,      /* FROM clause for an UPDATE-FROM, or NULL */
  ExprList *pEList,    /* The SET clause: list of column and new values */
  Expr *pWhere,        /* The WHERE clause */
  u8 orconf,           /* The conflict algorithm. (OE_Abort, OE_Ignore, etc) */
  const char *zStart,  /* Start of SQL text */
  const char *zEnd     /* End of SQL text */
){
  capdb *db = pParse->db;
  TriggerStep *pTriggerStep;

  pTriggerStep = triggerStepAllocate(pParse, TK_UPDATE, pTabList, zStart, zEnd);
  if( pTriggerStep ){
    SrcList *pFromDup = 0;
    if( IN_RENAME_OBJECT ){
      pTriggerStep->pExprList = pEList;
      pTriggerStep->pWhere = pWhere;
      pFromDup = pFrom;
      pEList = 0;
      pWhere = 0;
      pFrom = 0;
    }else{
      pTriggerStep->pExprList = capdbExprListDup(db, pEList, EXPRDUP_REDUCE);
      pTriggerStep->pWhere = capdbExprDup(db, pWhere, EXPRDUP_REDUCE);
      pFromDup = capdbSrcListDup(db, pFrom, EXPRDUP_REDUCE);
    }
    pTriggerStep->orconf = orconf;

    if( pFromDup && !IN_RENAME_OBJECT){
      Select *pSub;
      Token as = {0, 0};
      pSub = capdbSelectNew(pParse, 0, pFromDup, 0,0,0,0, SF_NestedFrom, 0);
      pFromDup = capdbSrcListAppendFromTerm(pParse, 0, 0, 0, &as, pSub ,0);
    }
    if( pFromDup && pTriggerStep->pSrc ){
      pTriggerStep->pSrc = capdbSrcListAppendList(
          pParse, pTriggerStep->pSrc, pFromDup
      );
    }else{
      capdbSrcListDelete(db, pFromDup);
    }
  }
  capdbExprListDelete(db, pEList);
  capdbExprDelete(db, pWhere);
  capdbSrcListDelete(db, pFrom);
  return pTriggerStep;
}

/*
** Construct a trigger step that implements a DELETE statement and return
** a pointer to that trigger step.  The parser calls this routine when it
** sees a DELETE statement inside the body of a CREATE TRIGGER.
*/
TriggerStep *capdbTriggerDeleteStep(
  Parse *pParse,          /* Parser */
  SrcList *pTabList,      /* The table from which rows are deleted */
  Expr *pWhere,           /* The WHERE clause */
  const char *zStart,     /* Start of SQL text */
  const char *zEnd        /* End of SQL text */
){
  capdb *db = pParse->db;
  TriggerStep *pTriggerStep;

  pTriggerStep = triggerStepAllocate(pParse, TK_DELETE, pTabList, zStart, zEnd);
  if( pTriggerStep ){
    if( IN_RENAME_OBJECT ){
      pTriggerStep->pWhere = pWhere;
      pWhere = 0;
    }else{
      pTriggerStep->pWhere = capdbExprDup(db, pWhere, EXPRDUP_REDUCE);
    }
    pTriggerStep->orconf = OE_Default;
  }
  capdbExprDelete(db, pWhere);
  return pTriggerStep;
}

/* 
** Recursively delete a Trigger structure
*/
void capdbDeleteTrigger(capdb *db, Trigger *pTrigger){
  if( pTrigger==0 || pTrigger->bReturning ) return;
  capdbDeleteTriggerStep(db, pTrigger->step_list);
  capdbDbFree(db, pTrigger->zName);
  capdbDbFree(db, pTrigger->table);
  capdbExprDelete(db, pTrigger->pWhen);
  capdbIdListDelete(db, pTrigger->pColumns);
  capdbDbFree(db, pTrigger);
}

/*
** This function is called to drop a trigger from the database schema. 
**
** This may be called directly from the parser and therefore identifies
** the trigger by name.  The capdbDropTriggerPtr() routine does the
** same job as this routine except it takes a pointer to the trigger
** instead of the trigger name.
**/
void capdbDropTrigger(Parse *pParse, SrcList *pName, int noErr){
  Trigger *pTrigger = 0;
  int i;
  const char *zDb;
  const char *zName;
  capdb *db = pParse->db;

  if( db->mallocFailed ) goto drop_trigger_cleanup;
  if( CAPDB_OK!=capdbReadSchema(pParse) ){
    goto drop_trigger_cleanup;
  }

  assert( pName->nSrc==1 );
  assert( pName->a[0].fg.fixedSchema==0 && pName->a[0].fg.isSubquery==0 );
  zDb = pName->a[0].u4.zDatabase;
  zName = pName->a[0].zName;
  assert( zDb!=0 || capdbBtreeHoldsAllMutexes(db) );
  for(i=OMIT_TEMPDB; i<db->nDb; i++){
    int j = (i<2) ? i^1 : i;  /* Search TEMP before MAIN */
    if( zDb && capdbDbIsNamed(db, j, zDb)==0 ) continue;
    assert( capdbSchemaMutexHeld(db, j, 0) );
    pTrigger = capdbHashFind(&(db->aDb[j].pSchema->trigHash), zName);
    if( pTrigger ) break;
  }
  if( !pTrigger ){
    if( !noErr ){
      capdbErrorMsg(pParse, "no such trigger: %S", pName->a);
    }else{
      capdbCodeVerifyNamedSchema(pParse, zDb);
    }
    pParse->checkSchema = 1;
    goto drop_trigger_cleanup;
  }
  capdbDropTriggerPtr(pParse, pTrigger);

drop_trigger_cleanup:
  capdbSrcListDelete(db, pName);
}

/*
** Return a pointer to the Table structure for the table that a trigger
** is set on.
*/
static Table *tableOfTrigger(Trigger *pTrigger){
  return capdbHashFind(&pTrigger->pTabSchema->tblHash, pTrigger->table);
}


/*
** Drop a trigger given a pointer to that trigger. 
*/
void capdbDropTriggerPtr(Parse *pParse, Trigger *pTrigger){
  Table   *pTable;
  Vdbe *v;
  capdb *db = pParse->db;
  int iDb;

  iDb = capdbSchemaToIndex(pParse->db, pTrigger->pSchema);
  assert( iDb>=0 && iDb<db->nDb );
  pTable = tableOfTrigger(pTrigger);
  assert( (pTable && pTable->pSchema==pTrigger->pSchema) || iDb==1 );
#ifndef CAPDB_OMIT_AUTHORIZATION
  if( pTable ){
    int code = CAPDB_DROP_TRIGGER;
    const char *zDb = db->aDb[iDb].zDbSName;
    const char *zTab = SCHEMA_TABLE(iDb);
    if( iDb==1 ) code = CAPDB_DROP_TEMP_TRIGGER;
    if( capdbAuthCheck(pParse, code, pTrigger->zName, pTable->zName, zDb) ||
      capdbAuthCheck(pParse, CAPDB_DELETE, zTab, 0, zDb) ){
      return;
    }
  }
#endif

  /* Generate code to destroy the database record of the trigger.
  */
  if( (v = capdbGetVdbe(pParse))!=0 ){
    capdbNestedParse(pParse,
       "DELETE FROM %Q." LEGACY_SCHEMA_TABLE " WHERE name=%Q AND type='trigger'",
       db->aDb[iDb].zDbSName, pTrigger->zName
    );
    capdbChangeCookie(pParse, iDb);
    capdbVdbeAddOp4(v, OP_DropTrigger, iDb, 0, 0, pTrigger->zName, 0);
  }
}

/*
** Remove a trigger from the hash tables of the sqlite* pointer.
*/
void capdbUnlinkAndDeleteTrigger(capdb *db, int iDb, const char *zName){
  Trigger *pTrigger;
  Hash *pHash;

  assert( capdbSchemaMutexHeld(db, iDb, 0) );
  pHash = &(db->aDb[iDb].pSchema->trigHash);
  pTrigger = capdbHashInsert(pHash, zName, 0);
  if( ALWAYS(pTrigger) ){
    if( pTrigger->pSchema==pTrigger->pTabSchema ){
      Table *pTab = tableOfTrigger(pTrigger);
      if( pTab ){
        Trigger **pp;
        for(pp=&pTab->pTrigger; *pp; pp=&((*pp)->pNext)){
          if( *pp==pTrigger ){
            *pp = (*pp)->pNext;
            break;
          }
        }
      }
    }
    capdbDeleteTrigger(db, pTrigger);
    db->mDbFlags |= DBFLAG_SchemaChange;
  }
}

/*
** pEList is the SET clause of an UPDATE statement.  Each entry
** in pEList is of the format <id>=<expr>.  If any of the entries
** in pEList have an <id> which matches an identifier in pIdList,
** then return TRUE.  If pIdList==NULL, then it is considered a
** wildcard that matches anything.  Likewise if pEList==NULL then
** it matches anything so always return true.  Return false only
** if there is no match.
*/
static int checkColumnOverlap(IdList *pIdList, ExprList *pEList){
  int e;
  if( pIdList==0 || NEVER(pEList==0) ) return 1;
  for(e=0; e<pEList->nExpr; e++){
    if( capdbIdListIndex(pIdList, pEList->a[e].zEName)>=0 ) return 1;
  }
  return 0; 
}

/*
** Return true if any TEMP triggers exist
*/
static int tempTriggersExist(capdb *db){
  if( NEVER(db->aDb[1].pSchema==0) ) return 0;
  if( sqliteHashFirst(&db->aDb[1].pSchema->trigHash)==0 ) return 0;
  return 1;
}

/*
** Return a list of all triggers on table pTab if there exists at least
** one trigger that must be fired when an operation of type 'op' is 
** performed on the table, and, if that operation is an UPDATE, if at
** least one of the columns in pChanges is being modified.
*/
static CAPDB_NOINLINE Trigger *triggersReallyExist(
  Parse *pParse,          /* Parse context */
  Table *pTab,            /* The table the contains the triggers */
  int op,                 /* one of TK_DELETE, TK_INSERT, TK_UPDATE */
  ExprList *pChanges,     /* Columns that change in an UPDATE statement */
  int *pMask              /* OUT: Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
){
  int mask = 0;
  Trigger *pList = 0;
  Trigger *p;

  pList = capdbTriggerList(pParse, pTab);
  assert( pList==0 || IsVirtual(pTab)==0 
           || (pList->bReturning && pList->pNext==0) );
  if( pList!=0 ){
    p = pList;
    if( (pParse->db->flags & CAPDB_EnableTrigger)==0
     && pTab->pTrigger!=0
     && capdbSchemaToIndex(pParse->db, pTab->pTrigger->pSchema)!=1
    ){
      /* The CAPDB_DBCONFIG_ENABLE_TRIGGER setting is off.  That means that
      ** only TEMP triggers are allowed.  Truncate the pList so that it
      ** includes only TEMP triggers */
      if( pList==pTab->pTrigger ){
        pList = 0;
        goto exit_triggers_exist;
      }
      while( ALWAYS(p->pNext) && p->pNext!=pTab->pTrigger ) p = p->pNext;
      p->pNext = 0;
      p = pList;
    }
    do{
      if( p->op==op && checkColumnOverlap(p->pColumns, pChanges) ){
        mask |= p->tr_tm;
      }else if( p->op==TK_RETURNING ){
        /* The first time a RETURNING trigger is seen, the "op" value tells
        ** us what time of trigger it should be. */
        assert( capdbIsToplevel(pParse) );
        p->op = op;
        if( IsVirtual(pTab) ){
          if( op!=TK_INSERT ){
            capdbErrorMsg(pParse,
              "%s RETURNING is not available on virtual tables",
              op==TK_DELETE ? "DELETE" : "UPDATE");
          }
          p->tr_tm = TRIGGER_BEFORE;
        }else{
          p->tr_tm = TRIGGER_AFTER;
        }
        mask |= p->tr_tm;
      }else if( p->bReturning && p->op==TK_INSERT && op==TK_UPDATE
                && capdbIsToplevel(pParse) ){
        /* Also fire a RETURNING trigger for an UPSERT */
        mask |= p->tr_tm;
      }
      p = p->pNext;
    }while( p );
  }
exit_triggers_exist:
  if( pMask ){
    *pMask = mask;
  }
  return (mask ? pList : 0);
}
Trigger *capdbTriggersExist(
  Parse *pParse,          /* Parse context */
  Table *pTab,            /* The table the contains the triggers */
  int op,                 /* one of TK_DELETE, TK_INSERT, TK_UPDATE */
  ExprList *pChanges,     /* Columns that change in an UPDATE statement */
  int *pMask              /* OUT: Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
){
  assert( pTab!=0 );
  if( (pTab->pTrigger==0 && !tempTriggersExist(pParse->db))
   || pParse->disableTriggers
  ){
    if( pMask ) *pMask = 0;
    return 0;
  }
  return triggersReallyExist(pParse,pTab,op,pChanges,pMask);
}

/*
** Return true if the pExpr term from the RETURNING clause argument
** list is of the form "*".  Raise an error if the terms if of the
** form "table.*".
*/
static int isAsteriskTerm(
  Parse *pParse,      /* Parsing context */
  Expr *pTerm         /* A term in the RETURNING clause */
){
  assert( pTerm!=0 );
  if( pTerm->op==TK_ASTERISK ) return 1;
  if( pTerm->op!=TK_DOT ) return 0;
  assert( pTerm->pRight!=0 );
  assert( pTerm->pLeft!=0 );
  if( pTerm->pRight->op!=TK_ASTERISK ) return 0;
  capdbErrorMsg(pParse, "RETURNING may not use \"TABLE.*\" wildcards");
  return 1;
}

/* The input list pList is the list of result set terms from a RETURNING
** clause.  The table that we are returning from is pTab.
**
** This routine makes a copy of the pList, and at the same time expands
** any "*" wildcards to be the complete set of columns from pTab.
*/
static ExprList *capdbExpandReturning(
  Parse *pParse,        /* Parsing context */
  ExprList *pList,      /* The arguments to RETURNING */
  Table *pTab           /* The table being updated */
){
  ExprList *pNew = 0;
  capdb *db = pParse->db;
  int i;

  for(i=0; i<pList->nExpr; i++){
    Expr *pOldExpr = pList->a[i].pExpr;
    if( NEVER(pOldExpr==0) ) continue;
    if( isAsteriskTerm(pParse, pOldExpr) ){
      int jj;
      for(jj=0; jj<pTab->nCol; jj++){
        Expr *pNewExpr;
        if( IsHiddenColumn(pTab->aCol+jj) ) continue;
        pNewExpr = capdbExpr(db, TK_ID, pTab->aCol[jj].zCnName);
        pNew = capdbExprListAppend(pParse, pNew, pNewExpr);
        if( !db->mallocFailed ){
          struct ExprList_item *pItem = &pNew->a[pNew->nExpr-1];
          pItem->zEName = capdbDbStrDup(db, pTab->aCol[jj].zCnName);
          pItem->fg.eEName = ENAME_NAME;
        }
      }
    }else{
      Expr *pNewExpr = capdbExprDup(db, pOldExpr, 0);
      pNew = capdbExprListAppend(pParse, pNew, pNewExpr);
      if( !db->mallocFailed && ALWAYS(pList->a[i].zEName!=0) ){
        struct ExprList_item *pItem = &pNew->a[pNew->nExpr-1];
        pItem->zEName = capdbDbStrDup(db, pList->a[i].zEName);
        pItem->fg.eEName = pList->a[i].fg.eEName;
      }
    }
  }
  return pNew;
}

/* If the Expr node is a subquery or an EXISTS operator or an IN operator that
** uses a subquery, and if the subquery is SF_Correlated, then mark the
** expression as EP_VarSelect.
*/
static int capdbReturningSubqueryVarSelect(Walker *NotUsed, Expr *pExpr){
  UNUSED_PARAMETER(NotUsed);
  if( ExprUseXSelect(pExpr)
   && (pExpr->x.pSelect->selFlags & SF_Correlated)!=0
  ){
    testcase( ExprHasProperty(pExpr, EP_VarSelect) );
    ExprSetProperty(pExpr, EP_VarSelect);
  }
  return WRC_Continue;
}


/*
** If the SELECT references the table pWalker->u.pTab, then do two things:
**
**    (1) Mark the SELECT as as SF_Correlated.
**    (2) Set pWalker->eCode to non-zero so that the caller will know
**        that (1) has happened.
*/
static int capdbReturningSubqueryCorrelated(Walker *pWalker, Select *pSelect){
  int i;
  SrcList *pSrc;
  assert( pSelect!=0 );
  pSrc = pSelect->pSrc;
  assert( pSrc!=0 );
  for(i=0; i<pSrc->nSrc; i++){
    if( pSrc->a[i].pSTab==pWalker->u.pTab ){
      testcase( pSelect->selFlags & SF_Correlated );
      pSelect->selFlags |= SF_Correlated;
      pWalker->eCode = 1;
      break;
    }
  }
  return WRC_Continue;
}

/*
** Scan the expression list that is the argument to RETURNING looking
** for subqueries that depend on the table which is being modified in the
** statement that is hosting the RETURNING clause (pTab).  Mark all such
** subqueries as SF_Correlated.  If the subqueries are part of an
** expression, mark the expression as EP_VarSelect.
**
** https://sqlite.org/forum/forumpost/2c83569ce8945d39
*/
static void capdbProcessReturningSubqueries(
  ExprList *pEList,
  Table *pTab
){
  Walker w;
  memset(&w, 0, sizeof(w));
  w.xExprCallback = capdbExprWalkNoop;
  w.xSelectCallback = capdbReturningSubqueryCorrelated;
  w.u.pTab = pTab;
  capdbWalkExprList(&w, pEList);
  if( w.eCode ){
    w.xExprCallback = capdbReturningSubqueryVarSelect;
    w.xSelectCallback = capdbSelectWalkNoop;
    capdbWalkExprList(&w, pEList);
  }
}

/*
** Generate code for the RETURNING trigger.  Unlike other triggers
** that invoke a subprogram in the bytecode, the code for RETURNING
** is generated in-line.
*/
static void codeReturningTrigger(
  Parse *pParse,       /* Parse context */
  Trigger *pTrigger,   /* The trigger step that defines the RETURNING */
  Table *pTab,         /* The table to code triggers from */
  int regIn            /* The first in an array of registers */
){
  Vdbe *v = pParse->pVdbe;
  capdb *db = pParse->db;
  ExprList *pNew;
  Returning *pReturning;
  Select sSelect;
  SrcList *pFrom;
  union {
    SrcList sSrc;
    u8 fromSpace[SZ_SRCLIST_1];
  } uSrc;

  assert( v!=0 );
  if( !pParse->bReturning ){
    /* This RETURNING trigger must be for a different statement as
    ** this statement lacks a RETURNING clause. */
    return;
  }
  assert( db->pParse==pParse );
  assert( !pParse->isCreate );
  pReturning = pParse->u1.d.pReturning;
  if( pTrigger != &(pReturning->retTrig) ){
    /* This RETURNING trigger is for a different statement */
    return;
  }
  memset(&sSelect, 0, sizeof(sSelect));
  memset(&uSrc, 0, sizeof(uSrc));
  pFrom = &uSrc.sSrc;
  sSelect.pEList = capdbExprListDup(db, pReturning->pReturnEL, 0);
  sSelect.pSrc = pFrom;
  pFrom->nSrc = 1;
  pFrom->a[0].pSTab = pTab;
  pFrom->a[0].zName = pTab->zName; /* tag-20240424-1 */
  pFrom->a[0].iCursor = -1;
  capdbSelectPrep(pParse, &sSelect, 0);
  if( pParse->nErr==0 ){
    assert( db->mallocFailed==0 );
    capdbGenerateColumnNames(pParse, &sSelect);
  }
  capdbExprListDelete(db, sSelect.pEList);
  pNew = capdbExpandReturning(pParse, pReturning->pReturnEL, pTab);
  if( pParse->nErr==0 ){
    NameContext sNC;
    memset(&sNC, 0, sizeof(sNC));
    if( pReturning->nRetCol==0 ){
      pReturning->nRetCol = pNew->nExpr;
      pReturning->iRetCur = pParse->nTab++;
    }
    sNC.pParse = pParse;
    sNC.uNC.iBaseReg = regIn;
    sNC.ncFlags = NC_UBaseReg;
    pParse->eTriggerOp = pTrigger->op;
    pParse->pTriggerTab = pTab;
    if( capdbResolveExprListNames(&sNC, pNew)==CAPDB_OK
     && ALWAYS(!db->mallocFailed)
    ){
      int i;
      int nCol = pNew->nExpr;
      int reg = pParse->nMem+1;
      capdbProcessReturningSubqueries(pNew, pTab);
      pParse->nMem += nCol+2;
      pReturning->iRetReg = reg;
      for(i=0; i<nCol; i++){
        Expr *pCol = pNew->a[i].pExpr;
        assert( pCol!=0 ); /* Due to !db->mallocFailed ~9 lines above */
        capdbExprCodeFactorable(pParse, pCol, reg+i);
        if( capdbExprAffinity(pCol)==CAPDB_AFF_REAL ){
          capdbVdbeAddOp1(v, OP_RealAffinity, reg+i);
        }
      }
      capdbVdbeAddOp3(v, OP_MakeRecord, reg, i, reg+i);
      capdbVdbeAddOp2(v, OP_NewRowid, pReturning->iRetCur, reg+i+1);
      capdbVdbeAddOp3(v, OP_Insert, pReturning->iRetCur, reg+i, reg+i+1);
    }
  }
  capdbExprListDelete(db, pNew);
  pParse->eTriggerOp = 0;
  pParse->pTriggerTab = 0;
}



/*
** Generate VDBE code for the statements inside the body of a single 
** trigger.
*/
static int codeTriggerProgram(
  Parse *pParse,            /* The parser context */
  TriggerStep *pStepList,   /* List of statements inside the trigger body */
  int orconf                /* Conflict algorithm. (OE_Abort, etc) */  
){
  TriggerStep *pStep;
  Vdbe *v = pParse->pVdbe;
  capdb *db = pParse->db;

  assert( pParse->pTriggerTab && pParse->pToplevel );
  assert( pStepList );
  assert( v!=0 );
  for(pStep=pStepList; pStep; pStep=pStep->pNext){
    /* Figure out the ON CONFLICT policy that will be used for this step
    ** of the trigger program. If the statement that caused this trigger
    ** to fire had an explicit ON CONFLICT, then use it. Otherwise, use
    ** the ON CONFLICT policy that was specified as part of the trigger
    ** step statement. Example:
    **
    **   CREATE TRIGGER AFTER INSERT ON t1 BEGIN;
    **     INSERT OR REPLACE INTO t2 VALUES(new.a, new.b);
    **   END;
    **
    **   INSERT INTO t1 ... ;            -- insert into t2 uses REPLACE policy
    **   INSERT OR IGNORE INTO t1 ... ;  -- insert into t2 uses IGNORE policy
    */
    pParse->eOrconf = (orconf==OE_Default)?pStep->orconf:(u8)orconf;
    assert( pParse->okConstFactor==0 );

#ifndef CAPDB_OMIT_TRACE
    if( pStep->zSpan ){
      capdbVdbeAddOp4(v, OP_Trace, 0x7fffffff, 1, 0,
                        capdbMPrintf(db, "-- %s", pStep->zSpan),
                        P4_DYNAMIC);
    }
#endif

    switch( pStep->op ){
      case TK_UPDATE: {
        capdbUpdate(pParse, 
          capdbSrcListDup(db, pStep->pSrc, 0),
          capdbExprListDup(db, pStep->pExprList, 0), 
          capdbExprDup(db, pStep->pWhere, 0), 
          pParse->eOrconf, 0, 0, 0
        );
        capdbVdbeAddOp0(v, OP_ResetCount);
        break;
      }
      case TK_INSERT: {
        capdbInsert(pParse, 
          capdbSrcListDup(db, pStep->pSrc, 0),
          capdbSelectDup(db, pStep->pSelect, 0), 
          capdbIdListDup(db, pStep->pIdList), 
          pParse->eOrconf,
          capdbUpsertDup(db, pStep->pUpsert)
        );
        capdbVdbeAddOp0(v, OP_ResetCount);
        break;
      }
      case TK_DELETE: {
        capdbDeleteFrom(pParse, 
          capdbSrcListDup(db, pStep->pSrc, 0),
          capdbExprDup(db, pStep->pWhere, 0), 0, 0
        );
        capdbVdbeAddOp0(v, OP_ResetCount);
        break;
      }
      default: assert( pStep->op==TK_SELECT ); {
        SelectDest sDest;
        Select *pSelect = capdbSelectDup(db, pStep->pSelect, 0);
        capdbSelectDestInit(&sDest, SRT_Discard, 0);
        capdbSelect(pParse, pSelect, &sDest);
        capdbSelectDelete(db, pSelect);
        break;
      }
    } 
  }

  return 0;
}

#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
/*
** This function is used to add VdbeComment() annotations to a VDBE
** program. It is not used in production code, only for debugging.
*/
static const char *onErrorText(int onError){
  switch( onError ){
    case OE_Abort:    return "abort";
    case OE_Rollback: return "rollback";
    case OE_Fail:     return "fail";
    case OE_Replace:  return "replace";
    case OE_Ignore:   return "ignore";
    case OE_Default:  return "default";
  }
  return "n/a";
}
#endif

/*
** Parse context structure pFrom has just been used to create a sub-vdbe
** (trigger program). If an error has occurred, transfer error information
** from pFrom to pTo.
*/
static void transferParseError(Parse *pTo, Parse *pFrom){
  assert( pFrom->zErrMsg==0 || pFrom->nErr );
  assert( pTo->zErrMsg==0 || pTo->nErr );
  if( pTo->nErr==0 ){
    pTo->zErrMsg = pFrom->zErrMsg;
    pTo->nErr = pFrom->nErr;
    pTo->rc = pFrom->rc;
  }else{
    capdbDbFree(pFrom->db, pFrom->zErrMsg);
  }
}

/*
** Create and populate a new TriggerPrg object with a sub-program 
** implementing trigger pTrigger with ON CONFLICT policy orconf.
*/
static TriggerPrg *codeRowTrigger(
  Parse *pParse,       /* Current parse context */
  Trigger *pTrigger,   /* Trigger to code */
  Table *pTab,         /* The table pTrigger is attached to */
  int orconf           /* ON CONFLICT policy to code trigger program with */
){
  Parse *pTop = capdbParseToplevel(pParse);
  capdb *db = pParse->db;   /* Database handle */
  TriggerPrg *pPrg;           /* Value to return */
  Expr *pWhen = 0;            /* Duplicate of trigger WHEN expression */
  Vdbe *v;                    /* Temporary VM */
  NameContext sNC;            /* Name context for sub-vdbe */
  SubProgram *pProgram = 0;   /* Sub-vdbe for trigger program */
  int iEndTrigger = 0;        /* Label to jump to if WHEN is false */
  Parse sSubParse;            /* Parse context for sub-vdbe */

  assert( pTrigger->zName==0 || pTab==tableOfTrigger(pTrigger) );
  assert( pTop->pVdbe );

  /* Allocate the TriggerPrg and SubProgram objects. To ensure that they
  ** are freed if an error occurs, link them into the Parse.pTriggerPrg 
  ** list of the top-level Parse object sooner rather than later.  */
  pPrg = capdbDbMallocZero(db, sizeof(TriggerPrg));
  if( !pPrg ) return 0;
  pPrg->pNext = pTop->pTriggerPrg;
  pTop->pTriggerPrg = pPrg;
  pPrg->pProgram = pProgram = capdbDbMallocZero(db, sizeof(SubProgram));
  if( !pProgram ) return 0;
  capdbVdbeLinkSubProgram(pTop->pVdbe, pProgram);
  pPrg->pTrigger = pTrigger;
  pPrg->orconf = orconf;
  pPrg->aColmask[0] = 0xffffffff;
  pPrg->aColmask[1] = 0xffffffff;

  /* Allocate and populate a new Parse context to use for coding the 
  ** trigger sub-program.  */
  capdbParseObjectInit(&sSubParse, db);
  memset(&sNC, 0, sizeof(sNC));
  sNC.pParse = &sSubParse;
  sSubParse.pTriggerTab = pTab;
  sSubParse.pToplevel = pTop;
  sSubParse.zAuthContext = pTrigger->zName;
  sSubParse.eTriggerOp = pTrigger->op;
  sSubParse.nQueryLoop = pParse->nQueryLoop;
  sSubParse.prepFlags = pParse->prepFlags;
  sSubParse.oldmask = 0;
  sSubParse.newmask = 0;

  v = capdbGetVdbe(&sSubParse);
  if( v ){
    VdbeComment((v, "Start: %s.%s (%s %s%s%s ON %s)", 
      pTrigger->zName, onErrorText(orconf),
      (pTrigger->tr_tm==TRIGGER_BEFORE ? "BEFORE" : "AFTER"),
        (pTrigger->op==TK_UPDATE ? "UPDATE" : ""),
        (pTrigger->op==TK_INSERT ? "INSERT" : ""),
        (pTrigger->op==TK_DELETE ? "DELETE" : ""),
      pTab->zName
    ));
#ifndef CAPDB_OMIT_TRACE
    if( pTrigger->zName ){
      capdbVdbeChangeP4(v, -1, 
        capdbMPrintf(db, "-- TRIGGER %s", pTrigger->zName), P4_DYNAMIC
      );
    }
#endif

    /* If one was specified, code the WHEN clause. If it evaluates to false
    ** (or NULL) the sub-vdbe is immediately halted by jumping to the 
    ** OP_Halt inserted at the end of the program.  */
    if( pTrigger->pWhen ){
      pWhen = capdbExprDup(db, pTrigger->pWhen, 0);
      if( db->mallocFailed==0
       && CAPDB_OK==capdbResolveExprNames(&sNC, pWhen) 
      ){
        iEndTrigger = capdbVdbeMakeLabel(&sSubParse);
        capdbExprIfFalse(&sSubParse, pWhen, iEndTrigger, CAPDB_JUMPIFNULL);
      }
      capdbExprDelete(db, pWhen);
    }

    /* Code the trigger program into the sub-vdbe. */
    codeTriggerProgram(&sSubParse, pTrigger->step_list, orconf);

    /* Insert an OP_Halt at the end of the sub-program. */
    if( iEndTrigger ){
      capdbVdbeResolveLabel(v, iEndTrigger);
    }
    capdbVdbeAddOp0(v, OP_Halt);
    VdbeComment((v, "End: %s.%s", pTrigger->zName, onErrorText(orconf)));
    transferParseError(pParse, &sSubParse);

    if( pParse->nErr==0 ){
      assert( db->mallocFailed==0 );
      pProgram->aOp = capdbVdbeTakeOpArray(v, &pProgram->nOp, &pTop->nMaxArg);
    }
    pProgram->nMem = sSubParse.nMem;
    pProgram->nCsr = sSubParse.nTab;
    pProgram->token = (void *)pTrigger;
    pPrg->aColmask[0] = sSubParse.oldmask;
    pPrg->aColmask[1] = sSubParse.newmask;
    capdbVdbeDelete(v);
  }else{
    transferParseError(pParse, &sSubParse);
  }

  assert( !sSubParse.pTriggerPrg && !sSubParse.nMaxArg );
  capdbParseObjectReset(&sSubParse);
  return pPrg;
}
    
/*
** Return a pointer to a TriggerPrg object containing the sub-program for
** trigger pTrigger with default ON CONFLICT algorithm orconf. If no such
** TriggerPrg object exists, a new object is allocated and populated before
** being returned.
*/
static TriggerPrg *getRowTrigger(
  Parse *pParse,       /* Current parse context */
  Trigger *pTrigger,   /* Trigger to code */
  Table *pTab,         /* The table trigger pTrigger is attached to */
  int orconf           /* ON CONFLICT algorithm. */
){
  Parse *pRoot = capdbParseToplevel(pParse);
  TriggerPrg *pPrg;

  assert( pTrigger->zName==0 || pTab==tableOfTrigger(pTrigger) );

  /* It may be that this trigger has already been coded (or is in the
  ** process of being coded). If this is the case, then an entry with
  ** a matching TriggerPrg.pTrigger field will be present somewhere
  ** in the Parse.pTriggerPrg list. Search for such an entry.  */
  for(pPrg=pRoot->pTriggerPrg; 
      pPrg && (pPrg->pTrigger!=pTrigger || pPrg->orconf!=orconf); 
      pPrg=pPrg->pNext
  );

  /* If an existing TriggerPrg could not be located, create a new one. */
  if( !pPrg ){
    pPrg = codeRowTrigger(pParse, pTrigger, pTab, orconf);
    pParse->db->errByteOffset = -1;
  }

  return pPrg;
}

/*
** Generate code for the trigger program associated with trigger p on 
** table pTab. The reg, orconf and ignoreJump parameters passed to this
** function are the same as those described in the header function for
** capdbCodeRowTrigger()
*/
void capdbCodeRowTriggerDirect(
  Parse *pParse,       /* Parse context */
  Trigger *p,          /* Trigger to code */
  Table *pTab,         /* The table to code triggers from */
  int reg,             /* Reg array containing OLD.* and NEW.* values */
  int orconf,          /* ON CONFLICT policy */
  int ignoreJump       /* Instruction to jump to for RAISE(IGNORE) */
){
  Vdbe *v = capdbGetVdbe(pParse); /* Main VM */
  TriggerPrg *pPrg;
  pPrg = getRowTrigger(pParse, p, pTab, orconf);
  assert( pPrg || pParse->nErr );

  /* Code the OP_Program opcode in the parent VDBE. P4 of the OP_Program 
  ** is a pointer to the sub-vdbe containing the trigger program.  */
  if( pPrg ){
    int bRecursive = (p->zName && 0==(pParse->db->flags&CAPDB_RecTriggers));

    capdbVdbeAddOp4(v, OP_Program, reg, ignoreJump, ++pParse->nMem,
                      (const char *)pPrg->pProgram, P4_SUBPROGRAM);
    VdbeComment(
        (v, "Call: %s.%s", (p->zName?p->zName:"fkey"), onErrorText(orconf)));

    /* Set the P5 operand of the OP_Program instruction to non-zero if
    ** recursive invocation of this trigger program is disallowed. Recursive
    ** invocation is disallowed if (a) the sub-program is really a trigger,
    ** not a foreign key action, and (b) the flag to enable recursive triggers
    ** is clear.  */
    capdbVdbeChangeP5(v, (u16)bRecursive);
  }
}

/*
** This is called to code the required FOR EACH ROW triggers for an operation
** on table pTab. The operation to code triggers for (INSERT, UPDATE or DELETE)
** is given by the op parameter. The tr_tm parameter determines whether the
** BEFORE or AFTER triggers are coded. If the operation is an UPDATE, then
** parameter pChanges is passed the list of columns being modified.
**
** If there are no triggers that fire at the specified time for the specified
** operation on pTab, this function is a no-op.
**
** The reg argument is the address of the first in an array of registers 
** that contain the values substituted for the new.* and old.* references
** in the trigger program. If N is the number of columns in table pTab
** (a copy of pTab->nCol), then registers are populated as follows:
**
**   Register       Contains
**   ------------------------------------------------------
**   reg+0          OLD.rowid
**   reg+1          OLD.* value of left-most column of pTab
**   ...            ...
**   reg+N          OLD.* value of right-most column of pTab
**   reg+N+1        NEW.rowid
**   reg+N+2        NEW.* value of left-most column of pTab
**   ...            ...
**   reg+N+N+1      NEW.* value of right-most column of pTab
**
** For ON DELETE triggers, the registers containing the NEW.* values will
** never be accessed by the trigger program, so they are not allocated or 
** populated by the caller (there is no data to populate them with anyway). 
** Similarly, for ON INSERT triggers the values stored in the OLD.* registers
** are never accessed, and so are not allocated by the caller. So, for an
** ON INSERT trigger, the value passed to this function as parameter reg
** is not a readable register, although registers (reg+N) through 
** (reg+N+N+1) are.
**
** Parameter orconf is the default conflict resolution algorithm for the
** trigger program to use (REPLACE, IGNORE etc.). Parameter ignoreJump
** is the instruction that control should jump to if a trigger program
** raises an IGNORE exception.
*/
void capdbCodeRowTrigger(
  Parse *pParse,       /* Parse context */
  Trigger *pTrigger,   /* List of triggers on table pTab */
  int op,              /* One of TK_UPDATE, TK_INSERT, TK_DELETE */
  ExprList *pChanges,  /* Changes list for any UPDATE OF triggers */
  int tr_tm,           /* One of TRIGGER_BEFORE, TRIGGER_AFTER */
  Table *pTab,         /* The table to code triggers from */
  int reg,             /* The first in an array of registers (see above) */
  int orconf,          /* ON CONFLICT policy */
  int ignoreJump       /* Instruction to jump to for RAISE(IGNORE) */
){
  Trigger *p;          /* Used to iterate through pTrigger list */

  assert( op==TK_UPDATE || op==TK_INSERT || op==TK_DELETE );
  assert( tr_tm==TRIGGER_BEFORE || tr_tm==TRIGGER_AFTER );
  assert( (op==TK_UPDATE)==(pChanges!=0) );

  for(p=pTrigger; p; p=p->pNext){

    /* Sanity checking:  The schema for the trigger and for the table are
    ** always defined.  The trigger must be in the same schema as the table
    ** or else it must be a TEMP trigger. */
    assert( p->pSchema!=0 );
    assert( p->pTabSchema!=0 );
    assert( p->pSchema==p->pTabSchema 
         || p->pSchema==pParse->db->aDb[1].pSchema );

    /* Determine whether we should code this trigger.  One of two choices:
    **   1. The trigger is an exact match to the current DML statement
    **   2. This is a RETURNING trigger for INSERT but we are currently
    **      doing the UPDATE part of an UPSERT.
    */
    if( (p->op==op || (p->bReturning && p->op==TK_INSERT && op==TK_UPDATE))
     && p->tr_tm==tr_tm 
     && checkColumnOverlap(p->pColumns, pChanges)
    ){
      if( !p->bReturning ){
        capdbCodeRowTriggerDirect(pParse, p, pTab, reg, orconf, ignoreJump);
      }else if( capdbIsToplevel(pParse) ){
        codeReturningTrigger(pParse, p, pTab, reg);
      }
    }
  }
}

/*
** Triggers may access values stored in the old.* or new.* pseudo-table. 
** This function returns a 32-bit bitmask indicating which columns of the 
** old.* or new.* tables actually are used by triggers. This information 
** may be used by the caller, for example, to avoid having to load the entire
** old.* record into memory when executing an UPDATE or DELETE command.
**
** Bit 0 of the returned mask is set if the left-most column of the
** table may be accessed using an [old|new].<col> reference. Bit 1 is set if
** the second leftmost column value is required, and so on. If there
** are more than 32 columns in the table, and at least one of the columns
** with an index greater than 32 may be accessed, 0xffffffff is returned.
**
** It is not possible to determine if the old.rowid or new.rowid column is 
** accessed by triggers. The caller must always assume that it is.
**
** Parameter isNew must be either 1 or 0. If it is 0, then the mask returned
** applies to the old.* table. If 1, the new.* table.
**
** Parameter tr_tm must be a mask with one or both of the TRIGGER_BEFORE
** and TRIGGER_AFTER bits set. Values accessed by BEFORE triggers are only
** included in the returned mask if the TRIGGER_BEFORE bit is set in the
** tr_tm parameter. Similarly, values accessed by AFTER triggers are only
** included in the returned mask if the TRIGGER_AFTER bit is set in tr_tm.
*/
u32 capdbTriggerColmask(
  Parse *pParse,       /* Parse context */
  Trigger *pTrigger,   /* List of triggers on table pTab */
  ExprList *pChanges,  /* Changes list for any UPDATE OF triggers */
  int isNew,           /* 1 for new.* ref mask, 0 for old.* ref mask */
  int tr_tm,           /* Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
  Table *pTab,         /* The table to code triggers from */
  int orconf           /* Default ON CONFLICT policy for trigger steps */
){
  const int op = pChanges ? TK_UPDATE : TK_DELETE;
  u32 mask = 0;
  Trigger *p;

  assert( isNew==1 || isNew==0 );
  if( IsView(pTab) ){
    return 0xffffffff;
  }
  for(p=pTrigger; p; p=p->pNext){
    if( p->op==op
     && (tr_tm&p->tr_tm)
     && checkColumnOverlap(p->pColumns,pChanges)
    ){
      if( p->bReturning ){
        mask = 0xffffffff;
      }else{
        TriggerPrg *pPrg;
        pPrg = getRowTrigger(pParse, p, pTab, orconf);
        if( pPrg ){
          mask |= pPrg->aColmask[isNew];
        }
      }
    }
  }

  return mask;
}

#endif /* !defined(CAPDB_OMIT_TRIGGER) */
