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
** Code for testing the virtual table interfaces.  This code
** is not included in the SQLite library.  It is used for automated
** testing of the SQLite library.
*/
#include "capdbInt.h"
#include "tclsqlite.h"
#include <stdlib.h>
#include <string.h>

#ifndef CAPDB_OMIT_VIRTUALTABLE

typedef struct echo_vtab echo_vtab;
typedef struct echo_cursor echo_cursor;

/*
** The test module defined in this file uses four global Tcl variables to
** communicate with test-scripts:
**
**     $::echo_module
**     $::echo_module_sync_fail
**     $::echo_module_begin_fail
**     $::echo_module_cost
**
** The variable ::echo_module is a list. Each time one of the following
** methods is called, one or more elements are appended to the list.
** This is used for automated testing of virtual table modules.
**
** The ::echo_module_sync_fail variable is set by test scripts and read
** by code in this file. If it is set to the name of a real table in the
** the database, then all xSync operations on echo virtual tables that
** use the named table as a backing store will fail.
*/

/*
** Errors can be provoked within the following echo virtual table methods:
**
**   xBestIndex   xOpen     xFilter   xNext   
**   xColumn      xRowid    xUpdate   xSync   
**   xBegin       xRename
**
** This is done by setting the global tcl variable:
**
**   echo_module_fail($method,$tbl)
**
** where $method is set to the name of the virtual table method to fail
** (i.e. "xBestIndex") and $tbl is the name of the table being echoed (not
** the name of the virtual table, the name of the underlying real table).
*/

/* 
** An echo virtual-table object.
**
** echo.vtab.aIndex is an array of booleans. The nth entry is true if 
** the nth column of the real table is the left-most column of an index
** (implicit or otherwise). In other words, if SQLite can optimize
** a query like "SELECT * FROM real_table WHERE col = ?".
**
** Member variable aCol[] contains copies of the column names of the real
** table.
*/
struct echo_vtab {
  capdb_vtab base;
  Tcl_Interp *interp;     /* Tcl interpreter containing debug variables */
  capdb *db;            /* Database connection */

  int isPattern;
  int inTransaction;      /* True if within a transaction */
  char *zThis;            /* Name of the echo table */
  char *zTableName;       /* Name of the real table */
  char *zLogName;         /* Name of the log table */
  int nCol;               /* Number of columns in the real table */
  int *aIndex;            /* Array of size nCol. True if column has an index */
  char **aCol;            /* Array of size nCol. Column names */
};

/* An echo cursor object */
struct echo_cursor {
  capdb_vtab_cursor base;
  capdb_stmt *pStmt;
};

static int simulateVtabError(echo_vtab *p, const char *zMethod){
  const char *zErr;
  char zVarname[128];
  zVarname[127] = '\0';
  capdb_snprintf(127, zVarname, "echo_module_fail(%s,%s)", zMethod, p->zTableName);
  zErr = Tcl_GetVar(p->interp, zVarname, TCL_GLOBAL_ONLY);
  if( zErr ){
    p->base.zErrMsg = capdb_mprintf("echo-vtab-error: %s", zErr);
  }
  return (zErr!=0);
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
static void dequoteString(char *z){
  int quote;
  int i, j;
  if( z==0 ) return;
  quote = z[0];
  switch( quote ){
    case '\'':  break;
    case '"':   break;
    case '`':   break;                /* For MySQL compatibility */
    case '[':   quote = ']';  break;  /* For MS SqlServer compatibility */
    default:    return;
  }
  for(i=1, j=0; z[i]; i++){
    if( z[i]==quote ){
      if( z[i+1]==quote ){
        z[j++] = quote;
        i++;
      }else{
        z[j++] = 0;
        break;
      }
    }else{
      z[j++] = z[i];
    }
  }
}

/*
** Retrieve the column names for the table named zTab via database
** connection db. CAPDB_OK is returned on success, or an sqlite error
** code otherwise.
**
** If successful, the number of columns is written to *pnCol. *paCol is
** set to point at capdb_malloc()'d space containing the array of
** nCol column names. The caller is responsible for calling capdb_free
** on *paCol.
*/
static int getColumnNames(
  capdb *db, 
  const char *zTab,
  char ***paCol, 
  int *pnCol
){
  char **aCol = 0;
  char *zSql;
  capdb_stmt *pStmt = 0;
  int rc = CAPDB_OK;
  int nCol = 0;

  /* Prepare the statement "SELECT * FROM <tbl>". The column names
  ** of the result set of the compiled SELECT will be the same as
  ** the column names of table <tbl>.
  */
  zSql = capdb_mprintf("SELECT * FROM %Q", zTab);
  if( !zSql ){
    rc = CAPDB_NOMEM;
    goto out;
  }
  rc = capdb_prepare(db, zSql, -1, &pStmt, 0);
  capdb_free(zSql);

  if( rc==CAPDB_OK ){
    int ii;
    int nBytes;
    char *zSpace;
    nCol = capdb_column_count(pStmt);

    /* Figure out how much space to allocate for the array of column names 
    ** (including space for the strings themselves). Then allocate it.
    */
    nBytes = sizeof(char *) * nCol;
    for(ii=0; ii<nCol; ii++){
      const char *zName = capdb_column_name(pStmt, ii);
      if( !zName ){
        rc = CAPDB_NOMEM;
        goto out;
      }
      nBytes += (int)strlen(zName)+1;
    }
    aCol = (char **)capdbMallocZero(nBytes);
    if( !aCol ){
      rc = CAPDB_NOMEM;
      goto out;
    }

    /* Copy the column names into the allocated space and set up the
    ** pointers in the aCol[] array.
    */
    zSpace = (char *)(&aCol[nCol]);
    for(ii=0; ii<nCol; ii++){
      aCol[ii] = zSpace;
      capdb_snprintf(nBytes, zSpace, "%s", capdb_column_name(pStmt,ii));
      zSpace += (int)strlen(zSpace) + 1;
    }
    assert( (zSpace-nBytes)==(char *)aCol );
  }

  *paCol = aCol;
  *pnCol = nCol;

out:
  capdb_finalize(pStmt);
  return rc;
}

/*
** Parameter zTab is the name of a table in database db with nCol 
** columns. This function allocates an array of integers nCol in 
** size and populates it according to any implicit or explicit 
** indices on table zTab.
**
** If successful, CAPDB_OK is returned and *paIndex set to point 
** at the allocated array. Otherwise, an error code is returned.
**
** See comments associated with the member variable aIndex above 
** "struct echo_vtab" for details of the contents of the array.
*/
static int getIndexArray(
  capdb *db,             /* Database connection */
  const char *zTab,        /* Name of table in database db */
  int nCol,
  int **paIndex
){
  capdb_stmt *pStmt = 0;
  int *aIndex = 0;
  int rc;
  char *zSql;

  /* Allocate space for the index array */
  aIndex = (int *)capdbMallocZero(sizeof(int) * nCol);
  if( !aIndex ){
    rc = CAPDB_NOMEM;
    goto get_index_array_out;
  }

  /* Compile an sqlite pragma to loop through all indices on table zTab */
  zSql = capdb_mprintf("PRAGMA index_list(%s)", zTab);
  if( !zSql ){
    rc = CAPDB_NOMEM;
    goto get_index_array_out;
  }
  rc = capdb_prepare(db, zSql, -1, &pStmt, 0);
  capdb_free(zSql);

  /* For each index, figure out the left-most column and set the 
  ** corresponding entry in aIndex[] to 1.
  */
  while( pStmt && capdb_step(pStmt)==CAPDB_ROW ){
    const char *zIdx = (const char *)capdb_column_text(pStmt, 1);
    capdb_stmt *pStmt2 = 0;
    if( zIdx==0 ) continue;
    zSql = capdb_mprintf("PRAGMA index_info(%s)", zIdx);
    if( !zSql ){
      rc = CAPDB_NOMEM;
      goto get_index_array_out;
    }
    rc = capdb_prepare(db, zSql, -1, &pStmt2, 0);
    capdb_free(zSql);
    if( pStmt2 && capdb_step(pStmt2)==CAPDB_ROW ){
      int cid = capdb_column_int(pStmt2, 1);
      assert( cid>=0 && cid<nCol );
      aIndex[cid] = 1;
    }
    if( pStmt2 ){
      rc = capdb_finalize(pStmt2);
    }
    if( rc!=CAPDB_OK ){
      goto get_index_array_out;
    }
  }


get_index_array_out:
  if( pStmt ){
    int rc2 = capdb_finalize(pStmt);
    if( rc==CAPDB_OK ){
      rc = rc2;
    }
  }
  if( rc!=CAPDB_OK ){
    capdb_free(aIndex);
    aIndex = 0;
  }
  *paIndex = aIndex;
  return rc;
}

/*
** Global Tcl variable $echo_module is a list. This routine appends
** the string element zArg to that list in interpreter interp.
*/
static void appendToEchoModule(Tcl_Interp *interp, const char *zArg){
  int flags = (TCL_APPEND_VALUE | TCL_LIST_ELEMENT | TCL_GLOBAL_ONLY);
  Tcl_SetVar(interp, "echo_module", (zArg?zArg:""), flags);
}

/*
** This function is called from within the echo-modules xCreate and
** xConnect methods. The argc and argv arguments are copies of those 
** passed to the calling method. This function is responsible for
** calling capdb_declare_vtab() to declare the schema of the virtual
** table being created or connected.
**
** If the constructor was passed just one argument, i.e.:
**
**   CREATE TABLE t1 AS echo(t2);
**
** Then t2 is assumed to be the name of a *real* database table. The
** schema of the virtual table is declared by passing a copy of the 
** CREATE TABLE statement for the real table to capdb_declare_vtab().
** Hence, the virtual table should have exactly the same column names and 
** types as the real table.
*/
static int echoDeclareVtab(
  echo_vtab *pVtab, 
  capdb *db 
){
  int rc = CAPDB_OK;

  if( pVtab->zTableName ){
    capdb_stmt *pStmt = 0;
    rc = capdb_prepare(db, 
        "SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = ?",
        -1, &pStmt, 0);
    if( rc==CAPDB_OK ){
      capdb_bind_text(pStmt, 1, pVtab->zTableName, -1, 0);
      if( capdb_step(pStmt)==CAPDB_ROW ){
        int rc2;
        const char *zCreateTable = (const char *)capdb_column_text(pStmt, 0);
        rc = capdb_declare_vtab(db, zCreateTable);
        rc2 = capdb_finalize(pStmt);
        if( rc==CAPDB_OK ){
          rc = rc2;
        }
      } else {
        rc = capdb_finalize(pStmt);
        if( rc==CAPDB_OK ){ 
          rc = CAPDB_ERROR;
        }
      }
      if( rc==CAPDB_OK ){
        rc = getColumnNames(db, pVtab->zTableName, &pVtab->aCol, &pVtab->nCol);
      }
      if( rc==CAPDB_OK ){
        rc = getIndexArray(db, pVtab->zTableName, pVtab->nCol, &pVtab->aIndex);
      }
    }
  }

  return rc;
}

/*
** This function frees all runtime structures associated with the virtual
** table pVtab.
*/
static int echoDestructor(capdb_vtab *pVtab){
  echo_vtab *p = (echo_vtab*)pVtab;
  capdb_free(p->aIndex);
  capdb_free(p->aCol);
  capdb_free(p->zThis);
  capdb_free(p->zTableName);
  capdb_free(p->zLogName);
  capdb_free(p);
  return 0;
}

typedef struct EchoModule EchoModule;
struct EchoModule {
  Tcl_Interp *interp;
  capdb *db;
};

/*
** This function is called to do the work of the xConnect() method -
** to allocate the required in-memory structures for a newly connected
** virtual table.
*/
static int echoConstructor(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  int rc;
  int i;
  echo_vtab *pVtab;

  /* Allocate the capdb_vtab/echo_vtab structure itself */
  pVtab = capdbMallocZero( sizeof(*pVtab) );
  if( !pVtab ){
    return CAPDB_NOMEM;
  }
  pVtab->interp = ((EchoModule *)pAux)->interp;
  pVtab->db = db;

  /* Allocate echo_vtab.zThis */
  pVtab->zThis = capdb_mprintf("%s", argv[2]);
  if( !pVtab->zThis ){
    echoDestructor((capdb_vtab *)pVtab);
    return CAPDB_NOMEM;
  }

  /* Allocate echo_vtab.zTableName */
  if( argc>3 ){
    pVtab->zTableName = capdb_mprintf("%s", argv[3]);
    dequoteString(pVtab->zTableName);
    if( pVtab->zTableName && pVtab->zTableName[0]=='*' ){
      char *z = capdb_mprintf("%s%s", argv[2], &(pVtab->zTableName[1]));
      capdb_free(pVtab->zTableName);
      pVtab->zTableName = z;
      pVtab->isPattern = 1;
    }
    if( !pVtab->zTableName ){
      echoDestructor((capdb_vtab *)pVtab);
      return CAPDB_NOMEM;
    }
  }

  /* Log the arguments to this function to Tcl var ::echo_module */
  for(i=0; i<argc; i++){
    appendToEchoModule(pVtab->interp, argv[i]);
  }

  /* Invoke capdb_declare_vtab and set up other members of the echo_vtab
  ** structure. If an error occurs, delete the capdb_vtab structure and
  ** return an error code.
  */
  rc = echoDeclareVtab(pVtab, db);
  if( rc!=CAPDB_OK ){
    echoDestructor((capdb_vtab *)pVtab);
    return rc;
  }

  /* Success. Set *ppVtab and return */
  *ppVtab = &pVtab->base;
  return CAPDB_OK;
}

/* 
** Echo virtual table module xCreate method.
*/
static int echoCreate(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  int rc = CAPDB_OK;
  appendToEchoModule(((EchoModule *)pAux)->interp, "xCreate");
  rc = echoConstructor(db, pAux, argc, argv, ppVtab, pzErr);

  /* If there were two arguments passed to the module at the SQL level 
  ** (i.e. "CREATE VIRTUAL TABLE tbl USING echo(arg1, arg2)"), then 
  ** the second argument is used as a table name. Attempt to create
  ** such a table with a single column, "logmsg". This table will
  ** be used to log calls to the xUpdate method. It will be deleted
  ** when the virtual table is DROPed.
  **
  ** Note: The main point of this is to test that we can drop tables
  ** from within an xDestroy method call.
  */
  if( rc==CAPDB_OK && argc==5 ){
    char *zSql;
    echo_vtab *pVtab = *(echo_vtab **)ppVtab;
    pVtab->zLogName = capdb_mprintf("%s", argv[4]);
    zSql = capdb_mprintf("CREATE TABLE %Q(logmsg)", pVtab->zLogName);
    rc = capdb_exec(db, zSql, 0, 0, 0);
    capdb_free(zSql);
    if( rc!=CAPDB_OK ){
      *pzErr = capdb_mprintf("%s", capdb_errmsg(db));
    }
  }

  if( *ppVtab && rc!=CAPDB_OK ){
    echoDestructor(*ppVtab);
    *ppVtab = 0;
  }

  if( rc==CAPDB_OK ){
    (*(echo_vtab**)ppVtab)->inTransaction = 1;
  }

  return rc;
}

/* 
** Echo virtual table module xConnect method.
*/
static int echoConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  appendToEchoModule(((EchoModule *)pAux)->interp, "xConnect");
  return echoConstructor(db, pAux, argc, argv, ppVtab, pzErr);
}

/* 
** Echo virtual table module xDisconnect method.
*/
static int echoDisconnect(capdb_vtab *pVtab){
  appendToEchoModule(((echo_vtab *)pVtab)->interp, "xDisconnect");
  return echoDestructor(pVtab);
}

/* 
** Echo virtual table module xDestroy method.
*/
static int echoDestroy(capdb_vtab *pVtab){
  int rc = CAPDB_OK;
  echo_vtab *p = (echo_vtab *)pVtab;
  appendToEchoModule(((echo_vtab *)pVtab)->interp, "xDestroy");

  /* Drop the "log" table, if one exists (see echoCreate() for details) */
  if( p && p->zLogName ){
    char *zSql;
    zSql = capdb_mprintf("DROP TABLE %Q", p->zLogName);
    rc = capdb_exec(p->db, zSql, 0, 0, 0);
    capdb_free(zSql);
  }

  if( rc==CAPDB_OK ){
    rc = echoDestructor(pVtab);
  }
  return rc;
}

/* 
** Echo virtual table module xOpen method.
*/
static int echoOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  echo_cursor *pCur;
  if( simulateVtabError((echo_vtab *)pVTab, "xOpen") ){
    return CAPDB_ERROR;
  }
  pCur = capdbMallocZero(sizeof(echo_cursor));
  *ppCursor = (capdb_vtab_cursor *)pCur;
  return (pCur ? CAPDB_OK : CAPDB_NOMEM);
}

/* 
** Echo virtual table module xClose method.
*/
static int echoClose(capdb_vtab_cursor *cur){
  int rc;
  echo_cursor *pCur = (echo_cursor *)cur;
  capdb_stmt *pStmt = pCur->pStmt;
  pCur->pStmt = 0;
  capdb_free(pCur);
  rc = capdb_finalize(pStmt);
  return rc;
}

/*
** Return non-zero if the cursor does not currently point to a valid record
** (i.e if the scan has finished), or zero otherwise.
*/
static int echoEof(capdb_vtab_cursor *cur){
  return (((echo_cursor *)cur)->pStmt ? 0 : 1);
}

/* 
** Echo virtual table module xNext method.
*/
static int echoNext(capdb_vtab_cursor *cur){
  int rc = CAPDB_OK;
  echo_cursor *pCur = (echo_cursor *)cur;

  if( simulateVtabError((echo_vtab *)(cur->pVtab), "xNext") ){
    return CAPDB_ERROR;
  }

  if( pCur->pStmt ){
    rc = capdb_step(pCur->pStmt);
    if( rc==CAPDB_ROW ){
      rc = CAPDB_OK;
    }else{
      rc = capdb_finalize(pCur->pStmt);
      pCur->pStmt = 0;
    }
  }

  return rc;
}

/* 
** Echo virtual table module xColumn method.
*/
static int echoColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  int iCol = i + 1;
  capdb_stmt *pStmt = ((echo_cursor *)cur)->pStmt;

  if( simulateVtabError((echo_vtab *)(cur->pVtab), "xColumn") ){
    return CAPDB_ERROR;
  }

  if( !pStmt ){
    capdb_result_null(ctx);
  }else{
    assert( capdb_data_count(pStmt)>iCol );
    capdb_result_value(ctx, capdb_column_value(pStmt, iCol));
  }
  return CAPDB_OK;
}

/* 
** Echo virtual table module xRowid method.
*/
static int echoRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  capdb_stmt *pStmt = ((echo_cursor *)cur)->pStmt;

  if( simulateVtabError((echo_vtab *)(cur->pVtab), "xRowid") ){
    return CAPDB_ERROR;
  }

  *pRowid = capdb_column_int64(pStmt, 0);
  return CAPDB_OK;
}

/*
** Compute a simple hash of the null terminated string zString.
**
** This module uses only capdb_index_info.idxStr, not 
** capdb_index_info.idxNum. So to test idxNum, when idxStr is set
** in echoBestIndex(), idxNum is set to the corresponding hash value.
** In echoFilter(), code assert()s that the supplied idxNum value is
** indeed the hash of the supplied idxStr.
*/
static int hashString(const char *zString){
  u32 val = 0;
  int ii;
  for(ii=0; zString[ii]; ii++){
    val = (val << 3) + (int)zString[ii];
  }
  return (int)(val&0x7fffffff);
}

/* 
** Echo virtual table module xFilter method.
*/
static int echoFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  int rc;
  int i;

  echo_cursor *pCur = (echo_cursor *)pVtabCursor;
  echo_vtab *pVtab = (echo_vtab *)pVtabCursor->pVtab;
  capdb *db = pVtab->db;

  if( simulateVtabError(pVtab, "xFilter") ){
    return CAPDB_ERROR;
  }

  /* Check that idxNum matches idxStr */
  assert( idxNum==hashString(idxStr) );

  /* Log arguments to the ::echo_module Tcl variable */
  appendToEchoModule(pVtab->interp, "xFilter");
  appendToEchoModule(pVtab->interp, idxStr);
  for(i=0; i<argc; i++){
    appendToEchoModule(pVtab->interp, (const char*)capdb_value_text(argv[i]));
  }

  capdb_finalize(pCur->pStmt);
  pCur->pStmt = 0;

  /* Prepare the SQL statement created by echoBestIndex and bind the
  ** runtime parameters passed to this function to it.
  */
  rc = capdb_prepare(db, idxStr, -1, &pCur->pStmt, 0);
  assert( pCur->pStmt || rc!=CAPDB_OK );
  for(i=0; rc==CAPDB_OK && i<argc; i++){
    rc = capdb_bind_value(pCur->pStmt, i+1, argv[i]);
  }

  /* If everything was successful, advance to the first row of the scan */
  if( rc==CAPDB_OK ){
    rc = echoNext(pVtabCursor);
  }

  return rc;
}


/*
** A helper function used by echoUpdate() and echoBestIndex() for
** manipulating strings in concert with the capdb_mprintf() function.
**
** Parameter pzStr points to a pointer to a string allocated with
** capdb_mprintf. The second parameter, zAppend, points to another
** string. The two strings are concatenated together and *pzStr
** set to point at the result. The initial buffer pointed to by *pzStr
** is deallocated via capdb_free().
**
** If the third argument, doFree, is true, then capdb_free() is
** also called to free the buffer pointed to by zAppend.
*/
static void string_concat(char **pzStr, char *zAppend, int doFree, int *pRc){
  char *zIn = *pzStr;
  if( !zAppend && doFree && *pRc==CAPDB_OK ){
    *pRc = CAPDB_NOMEM;
  }
  if( *pRc!=CAPDB_OK ){
    capdb_free(zIn);
    zIn = 0;
  }else{
    if( zIn ){
      char *zTemp = zIn;
      zIn = capdb_mprintf("%s%s", zIn, zAppend);
      capdb_free(zTemp);
    }else{
      zIn = capdb_mprintf("%s", zAppend);
    }
    if( !zIn ){
      *pRc = CAPDB_NOMEM;
    }
  }
  *pzStr = zIn;
  if( doFree ){
    capdb_free(zAppend);
  }
}

/*
** This function returns a pointer to an capdb_malloc()ed buffer 
** containing the select-list (the thing between keywords SELECT and FROM)
** to query the underlying real table with for the scan described by
** argument pIdxInfo.
**
** If the current SQLite version is earlier than 3.10.0, this is just "*"
** (select all columns). Or, for version 3.10.0 and greater, the list of
** columns identified by the pIdxInfo->colUsed mask.
*/
static char *echoSelectList(echo_vtab *pTab, capdb_index_info *pIdxInfo){
  char *zRet = 0;
  if( capdb_libversion_number()<3010000 ){
    zRet = capdb_mprintf(", *");
  }else{
    int i;
    for(i=0; i<pTab->nCol; i++){
      if( pIdxInfo->colUsed & ((capdb_uint64)1 << (i>=63 ? 63 : i)) ){
        zRet = capdb_mprintf("%z, %s", zRet, pTab->aCol[i]);
      }else{
        zRet = capdb_mprintf("%z, NULL", zRet);
      }
      if( !zRet ) break;
    }
  }
  return zRet;
}

/*
** The echo module implements the subset of query constraints and sort
** orders that may take advantage of SQLite indices on the underlying
** real table. For example, if the real table is declared as:
**
**     CREATE TABLE real(a, b, c);
**     CREATE INDEX real_index ON real(b);
**
** then the echo module handles WHERE or ORDER BY clauses that refer
** to the column "b", but not "a" or "c". If a multi-column index is
** present, only its left most column is considered. 
**
** This xBestIndex method encodes the proposed search strategy as
** an SQL query on the real table underlying the virtual echo module 
** table and stores the query in capdb_index_info.idxStr. The SQL
** statement is of the form:
**
**   SELECT rowid, * FROM <real-table> ?<where-clause>? ?<order-by-clause>?
**
** where the <where-clause> and <order-by-clause> are determined
** by the contents of the structure pointed to by the pIdxInfo argument.
*/
static int echoBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  int ii;
  char *zQuery = 0;
  char *zCol = 0;
  char *zNew;
  int nArg = 0;
  const char *zSep = "WHERE";
  echo_vtab *pVtab = (echo_vtab *)tab;
  capdb_stmt *pStmt = 0;
  Tcl_Interp *interp = pVtab->interp;

  int nRow = 0;
  int useIdx = 0;
  int rc = CAPDB_OK;
  int useCost = 0;
  double cost = 0;
  int isIgnoreUsable = 0;
  if( Tcl_GetVar(interp, "echo_module_ignore_usable", TCL_GLOBAL_ONLY) ){
    isIgnoreUsable = 1;
  }

  if( simulateVtabError(pVtab, "xBestIndex") ){
    return CAPDB_ERROR;
  }

  /* Determine the number of rows in the table and store this value in local
  ** variable nRow. The 'estimated-cost' of the scan will be the number of
  ** rows in the table for a linear scan, or the log (base 2) of the 
  ** number of rows if the proposed scan uses an index.  
  */
  if( Tcl_GetVar(interp, "echo_module_cost", TCL_GLOBAL_ONLY) ){
    cost = atof(Tcl_GetVar(interp, "echo_module_cost", TCL_GLOBAL_ONLY));
    useCost = 1;
  } else {
    zQuery = capdb_mprintf("SELECT count(*) FROM %Q", pVtab->zTableName);
    if( !zQuery ){
      return CAPDB_NOMEM;
    }
    rc = capdb_prepare(pVtab->db, zQuery, -1, &pStmt, 0);
    capdb_free(zQuery);
    if( rc!=CAPDB_OK ){
      return rc;
    }
    capdb_step(pStmt);
    nRow = capdb_column_int(pStmt, 0);
    rc = capdb_finalize(pStmt);
    if( rc!=CAPDB_OK ){
      return rc;
    }
  }

  zCol = echoSelectList(pVtab, pIdxInfo);
  if( !zCol ) return CAPDB_NOMEM;
  zQuery = capdb_mprintf("SELECT rowid%z FROM %Q", zCol, pVtab->zTableName);
  if( !zQuery ) return CAPDB_NOMEM;

  for(ii=0; ii<pIdxInfo->nConstraint; ii++){
    const struct capdb_index_constraint *pConstraint;
    struct capdb_index_constraint_usage *pUsage;
    int iCol;

    pConstraint = &pIdxInfo->aConstraint[ii];
    pUsage = &pIdxInfo->aConstraintUsage[ii];

    if( !isIgnoreUsable && !pConstraint->usable ) continue;

    iCol = pConstraint->iColumn;
    if( iCol<0 || pVtab->aIndex[iCol] ){
      char *zNewCol = iCol>=0 ? pVtab->aCol[iCol] : "rowid";
      char *zOp = 0;
      useIdx = 1;
      switch( pConstraint->op ){
        case CAPDB_INDEX_CONSTRAINT_EQ:
          zOp = "="; break;
        case CAPDB_INDEX_CONSTRAINT_LT:
          zOp = "<"; break;
        case CAPDB_INDEX_CONSTRAINT_GT:
          zOp = ">"; break;
        case CAPDB_INDEX_CONSTRAINT_LE:
          zOp = "<="; break;
        case CAPDB_INDEX_CONSTRAINT_GE:
          zOp = ">="; break;
        case CAPDB_INDEX_CONSTRAINT_MATCH:
          /* Purposely translate the MATCH operator into a LIKE, which
          ** will be used by the next block of code to construct a new
          ** query.  It should also be noted here that the next block
          ** of code requires the first letter of this operator to be
          ** in upper-case to trigger the special MATCH handling (i.e.
          ** wrapping the bound parameter with literal '%'s).
          */
          zOp = "LIKE"; break;
        case CAPDB_INDEX_CONSTRAINT_LIKE:
          zOp = "like"; break;
        case CAPDB_INDEX_CONSTRAINT_GLOB:
          zOp = "glob"; break;
        case CAPDB_INDEX_CONSTRAINT_REGEXP:
          zOp = "regexp"; break;
      }
      if( zOp ){
        if( zOp[0]=='L' ){
          zNew = capdb_mprintf(" %s %s LIKE (SELECT '%%'||?||'%%')", 
              zSep, zNewCol);
        } else {
          zNew = capdb_mprintf(" %s %s %s ?", zSep, zNewCol, zOp);
        }
        string_concat(&zQuery, zNew, 1, &rc);
        zSep = "AND";
        pUsage->argvIndex = ++nArg;
        pUsage->omit = 1;
      }
    }
  }

  /* If there is only one term in the ORDER BY clause, and it is
  ** on a column that this virtual table has an index for, then consume 
  ** the ORDER BY clause.
  */
  if( pIdxInfo->nOrderBy==1 && (
        pIdxInfo->aOrderBy->iColumn<0 ||
        pVtab->aIndex[pIdxInfo->aOrderBy->iColumn]) ){
    int iCol = pIdxInfo->aOrderBy->iColumn;
    char *zNewCol = iCol>=0 ? pVtab->aCol[iCol] : "rowid";
    char *zDir = pIdxInfo->aOrderBy->desc?"DESC":"ASC";
    zNew = capdb_mprintf(" ORDER BY %s %s", zNewCol, zDir);
    string_concat(&zQuery, zNew, 1, &rc);
    pIdxInfo->orderByConsumed = 1;
  }

  appendToEchoModule(pVtab->interp, "xBestIndex");;
  appendToEchoModule(pVtab->interp, zQuery);

  if( !zQuery ){
    return rc;
  }
  pIdxInfo->idxNum = hashString(zQuery);
  pIdxInfo->idxStr = zQuery;
  pIdxInfo->needToFreeIdxStr = 1;
  if( useCost ){
    pIdxInfo->estimatedCost = cost;
  }else if( useIdx ){
    /* Approximation of log2(nRow). */
    for( ii=0; ii<(sizeof(int)*8)-1; ii++ ){
      if( nRow & (1<<ii) ){
        pIdxInfo->estimatedCost = (double)ii;
      }
    }
  }else{
    pIdxInfo->estimatedCost = (double)nRow;
  }
  return rc;
}

/*
** The xUpdate method for echo module virtual tables.
** 
**    apData[0]  apData[1]  apData[2..]
**
**    INTEGER                              DELETE            
**
**    INTEGER    NULL       (nCol args)    UPDATE (do not set rowid)
**    INTEGER    INTEGER    (nCol args)    UPDATE (with SET rowid = <arg1>)
**
**    NULL       NULL       (nCol args)    INSERT INTO (automatic rowid value)
**    NULL       INTEGER    (nCol args)    INSERT (incl. rowid value)
**
*/
int echoUpdate(
  capdb_vtab *tab, 
  int nData, 
  capdb_value **apData, 
  sqlite_int64 *pRowid
){
  echo_vtab *pVtab = (echo_vtab *)tab;
  capdb *db = pVtab->db;
  int rc = CAPDB_OK;

  capdb_stmt *pStmt = 0;
  char *z = 0;               /* SQL statement to execute */
  int bindArgZero = 0;       /* True to bind apData[0] to sql var no. nData */
  int bindArgOne = 0;        /* True to bind apData[1] to sql var no. 1 */
  int i;                     /* Counter variable used by for loops */

  assert( nData==pVtab->nCol+2 || nData==1 );

  /* Ticket #3083 - make sure we always start a transaction prior to
  ** making any changes to a virtual table */
  assert( pVtab->inTransaction );

  if( simulateVtabError(pVtab, "xUpdate") ){
    return CAPDB_ERROR;
  }

  /* If apData[0] is an integer and nData>1 then do an UPDATE */
  if( nData>1 && capdb_value_type(apData[0])==CAPDB_INTEGER ){
    char *zSep = " SET";
    z = capdb_mprintf("UPDATE %Q", pVtab->zTableName);
    if( !z ){
      rc = CAPDB_NOMEM;
    }

    bindArgOne = (apData[1] && capdb_value_type(apData[1])==CAPDB_INTEGER);
    bindArgZero = 1;

    if( bindArgOne ){
       string_concat(&z, " SET rowid=?1 ", 0, &rc);
       zSep = ",";
    }
    for(i=2; i<nData; i++){
      if( apData[i]==0 ) continue;
      string_concat(&z, capdb_mprintf(
          "%s %Q=?%d", zSep, pVtab->aCol[i-2], i), 1, &rc);
      zSep = ",";
    }
    string_concat(&z, capdb_mprintf(" WHERE rowid=?%d", nData), 1, &rc);
  }

  /* If apData[0] is an integer and nData==1 then do a DELETE */
  else if( nData==1 && capdb_value_type(apData[0])==CAPDB_INTEGER ){
    z = capdb_mprintf("DELETE FROM %Q WHERE rowid = ?1", pVtab->zTableName);
    if( !z ){
      rc = CAPDB_NOMEM;
    }
    bindArgZero = 1;
  }

  /* If the first argument is NULL and there are more than two args, INSERT */
  else if( nData>2 && capdb_value_type(apData[0])==CAPDB_NULL ){
    int ii;
    char *zInsert = 0;
    char *zValues = 0;
  
    zInsert = capdb_mprintf("INSERT INTO %Q (", pVtab->zTableName);
    if( !zInsert ){
      rc = CAPDB_NOMEM;
    }
    if( capdb_value_type(apData[1])==CAPDB_INTEGER ){
      bindArgOne = 1;
      zValues = capdb_mprintf("?");
      string_concat(&zInsert, "rowid", 0, &rc);
    }

    assert((pVtab->nCol+2)==nData);
    for(ii=2; ii<nData; ii++){
      string_concat(&zInsert, 
          capdb_mprintf("%s%Q", zValues?", ":"", pVtab->aCol[ii-2]), 1, &rc);
      string_concat(&zValues, 
          capdb_mprintf("%s?%d", zValues?", ":"", ii), 1, &rc);
    }

    string_concat(&z, zInsert, 1, &rc);
    string_concat(&z, ") VALUES(", 0, &rc);
    string_concat(&z, zValues, 1, &rc);
    string_concat(&z, ")", 0, &rc);
  }

  /* Anything else is an error */
  else{
    assert(0);
    return CAPDB_ERROR;
  }

  if( rc==CAPDB_OK ){
    rc = capdb_prepare(db, z, -1, &pStmt, 0);
  }
  assert( rc!=CAPDB_OK || pStmt );
  capdb_free(z);
  if( rc==CAPDB_OK ) {
    if( bindArgZero ){
      capdb_bind_value(pStmt, nData, apData[0]);
    }
    if( bindArgOne ){
      capdb_bind_value(pStmt, 1, apData[1]);
    }
    for(i=2; i<nData && rc==CAPDB_OK; i++){
      if( apData[i] ) rc = capdb_bind_value(pStmt, i, apData[i]);
    }
    if( rc==CAPDB_OK ){
      capdb_step(pStmt);
      rc = capdb_finalize(pStmt);
    }else{
      capdb_finalize(pStmt);
    }
  }

  if( pRowid && rc==CAPDB_OK ){
    *pRowid = capdb_last_insert_rowid(db);
  }
  if( rc!=CAPDB_OK ){
    tab->zErrMsg = capdb_mprintf("echo-vtab-error: %s", capdb_errmsg(db));
  }

  return rc;
}

/*
** xBegin, xSync, xCommit and xRollback callbacks for echo module
** virtual tables. Do nothing other than add the name of the callback
** to the $::echo_module Tcl variable.
*/
static int echoTransactionCall(capdb_vtab *tab, const char *zCall){
  char *z;
  echo_vtab *pVtab = (echo_vtab *)tab;
  z = capdb_mprintf("echo(%s)", pVtab->zTableName);
  if( z==0 ) return CAPDB_NOMEM;
  appendToEchoModule(pVtab->interp, zCall);
  appendToEchoModule(pVtab->interp, z);
  capdb_free(z);
  return CAPDB_OK;
}
static int echoBegin(capdb_vtab *tab){
  int rc;
  echo_vtab *pVtab = (echo_vtab *)tab;
  Tcl_Interp *interp = pVtab->interp;
  const char *zVal; 

  /* Ticket #3083 - do not start a transaction if we are already in
  ** a transaction */
  assert( !pVtab->inTransaction );

  if( simulateVtabError(pVtab, "xBegin") ){
    return CAPDB_ERROR;
  }

  rc = echoTransactionCall(tab, "xBegin");

  if( rc==CAPDB_OK ){
    /* Check if the $::echo_module_begin_fail variable is defined. If it is,
    ** and it is set to the name of the real table underlying this virtual
    ** echo module table, then cause this xSync operation to fail.
    */
    zVal = Tcl_GetVar(interp, "echo_module_begin_fail", TCL_GLOBAL_ONLY);
    if( zVal && 0==strcmp(zVal, pVtab->zTableName) ){
      rc = CAPDB_ERROR;
    }
  }
  if( rc==CAPDB_OK ){
    pVtab->inTransaction = 1;
  }
  return rc;
}
static int echoSync(capdb_vtab *tab){
  int rc;
  echo_vtab *pVtab = (echo_vtab *)tab;
  Tcl_Interp *interp = pVtab->interp;
  const char *zVal; 

  /* Ticket #3083 - Only call xSync if we have previously started a
  ** transaction */
  assert( pVtab->inTransaction );

  if( simulateVtabError(pVtab, "xSync") ){
    return CAPDB_ERROR;
  }

  rc = echoTransactionCall(tab, "xSync");

  if( rc==CAPDB_OK ){
    /* Check if the $::echo_module_sync_fail variable is defined. If it is,
    ** and it is set to the name of the real table underlying this virtual
    ** echo module table, then cause this xSync operation to fail.
    */
    zVal = Tcl_GetVar(interp, "echo_module_sync_fail", TCL_GLOBAL_ONLY);
    if( zVal && 0==strcmp(zVal, pVtab->zTableName) ){
      rc = -1;
    }
  }
  return rc;
}
static int echoCommit(capdb_vtab *tab){
  echo_vtab *pVtab = (echo_vtab*)tab;
  int rc;

  /* Ticket #3083 - Only call xCommit if we have previously started
  ** a transaction */
  assert( pVtab->inTransaction );

  if( simulateVtabError(pVtab, "xCommit") ){
    return CAPDB_ERROR;
  }

  capdbBeginBenignMalloc();
  rc = echoTransactionCall(tab, "xCommit");
  capdbEndBenignMalloc();
  pVtab->inTransaction = 0;
  return rc;
}
static int echoRollback(capdb_vtab *tab){
  int rc;
  echo_vtab *pVtab = (echo_vtab*)tab;

  /* Ticket #3083 - Only call xRollback if we have previously started
  ** a transaction */
  assert( pVtab->inTransaction );

  rc = echoTransactionCall(tab, "xRollback");
  pVtab->inTransaction = 0;
  return rc;
}

/*
** Implementation of "GLOB" function on the echo module.  Pass
** all arguments to the ::echo_glob_overload procedure of TCL
** and return the result of that procedure as a string.
*/
static void overloadedGlobFunction(
  capdb_context *pContext,
  int nArg,
  capdb_value **apArg
){
  Tcl_Interp *interp = capdb_user_data(pContext);
  Tcl_DString str;
  int i;
  int rc;
  Tcl_DStringInit(&str);
  Tcl_DStringAppendElement(&str, "::echo_glob_overload");
  for(i=0; i<nArg; i++){
    Tcl_DStringAppendElement(&str, (char*)capdb_value_text(apArg[i]));
  }
  rc = Tcl_Eval(interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  if( rc ){
    capdb_result_error(pContext, Tcl_GetStringResult(interp), -1);
  }else{
    capdb_result_text(pContext, Tcl_GetStringResult(interp),
                        -1, CAPDB_TRANSIENT);
  }
  Tcl_ResetResult(interp);
}

/*
** This is the xFindFunction implementation for the echo module.
** SQLite calls this routine when the first argument of a function
** is a column of an echo virtual table.  This routine can optionally
** override the implementation of that function.  It will choose to
** do so if the function is named "glob", and a TCL command named
** ::echo_glob_overload exists.
*/
static int echoFindFunction(
  capdb_vtab *vtab,
  int nArg,
  const char *zFuncName,
  void (**pxFunc)(capdb_context*,int,capdb_value**),
  void **ppArg
){
  echo_vtab *pVtab = (echo_vtab *)vtab;
  Tcl_Interp *interp = pVtab->interp;
  Tcl_CmdInfo info;
  if( strcmp(zFuncName,"glob")!=0 ){
    return 0;
  }
  if( Tcl_GetCommandInfo(interp, "::echo_glob_overload", &info)==0 ){
    return 0;
  }
  *pxFunc = overloadedGlobFunction;
  *ppArg = interp;
  return 1;
}

static int echoRename(capdb_vtab *vtab, const char *zNewName){
  int rc = CAPDB_OK;
  echo_vtab *p = (echo_vtab *)vtab;

  if( simulateVtabError(p, "xRename") ){
    return CAPDB_ERROR;
  }

  if( p->isPattern ){
    int nThis = (int)strlen(p->zThis);
    char *zSql = capdb_mprintf("ALTER TABLE %s RENAME TO %s%s", 
        p->zTableName, zNewName, &p->zTableName[nThis]
    );
    rc = capdb_exec(p->db, zSql, 0, 0, 0);
    capdb_free(zSql);
  }

  return rc;
}

static int echoSavepoint(capdb_vtab *pVTab, int iSavepoint){
  assert( pVTab );
  return CAPDB_OK;
}

static int echoRelease(capdb_vtab *pVTab, int iSavepoint){
  assert( pVTab );
  return CAPDB_OK;
}

static int echoRollbackTo(capdb_vtab *pVTab, int iSavepoint){
  assert( pVTab );
  return CAPDB_OK;
}

/*
** A virtual table module that merely "echos" the contents of another
** table (like an SQL VIEW).
*/
static capdb_module echoModule = {
  1,                         /* iVersion */
  echoCreate,
  echoConnect,
  echoBestIndex,
  echoDisconnect, 
  echoDestroy,
  echoOpen,                  /* xOpen - open a cursor */
  echoClose,                 /* xClose - close a cursor */
  echoFilter,                /* xFilter - configure scan constraints */
  echoNext,                  /* xNext - advance a cursor */
  echoEof,                   /* xEof */
  echoColumn,                /* xColumn - read data */
  echoRowid,                 /* xRowid - read data */
  echoUpdate,                /* xUpdate - write data */
  echoBegin,                 /* xBegin - begin transaction */
  echoSync,                  /* xSync - sync transaction */
  echoCommit,                /* xCommit - commit transaction */
  echoRollback,              /* xRollback - rollback transaction */
  echoFindFunction,          /* xFindFunction - function overloading */
  echoRename,                /* xRename - rename the table */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  0,                         /* xShadowName */
  0                          /* xIntegrity */
};

static capdb_module echoModuleV2 = {
  2,                         /* iVersion */
  echoCreate,
  echoConnect,
  echoBestIndex,
  echoDisconnect, 
  echoDestroy,
  echoOpen,                  /* xOpen - open a cursor */
  echoClose,                 /* xClose - close a cursor */
  echoFilter,                /* xFilter - configure scan constraints */
  echoNext,                  /* xNext - advance a cursor */
  echoEof,                   /* xEof */
  echoColumn,                /* xColumn - read data */
  echoRowid,                 /* xRowid - read data */
  echoUpdate,                /* xUpdate - write data */
  echoBegin,                 /* xBegin - begin transaction */
  echoSync,                  /* xSync - sync transaction */
  echoCommit,                /* xCommit - commit transaction */
  echoRollback,              /* xRollback - rollback transaction */
  echoFindFunction,          /* xFindFunction - function overloading */
  echoRename,                /* xRename - rename the table */
  echoSavepoint,
  echoRelease,
  echoRollbackTo,
  0,                         /* xShadowName */
  0                          /* xIntegrity  */
};

/*
** Decode a pointer to an capdb object.
*/
extern int getDbPointer(Tcl_Interp *interp, const char *zA, capdb **ppDb);
extern const char *capdbErrName(int);

static void moduleDestroy(void *p){
  EchoModule *pMod = (EchoModule*)p;
  capdb_create_function(pMod->db, "function_that_does_not_exist_0982ma98",
                          CAPDB_ANY, 1, 0, 0, 0, 0);
  capdb_free(p);
}

/*
** Register the echo virtual table module.
*/
static int CAPDB_TCLAPI register_echo_module(
  ClientData clientData, /* Pointer to capdb_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  int rc;
  capdb *db;
  EchoModule *pMod;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  /* Virtual table module "echo" */
  pMod = capdb_malloc(sizeof(EchoModule));
  pMod->interp = interp;
  pMod->db = db;
  rc = capdb_create_module_v2(
      db, "echo", &echoModule, (void*)pMod, moduleDestroy
  );

  /* Virtual table module "echo_v2" */
  if( rc==CAPDB_OK ){
    pMod = capdb_malloc(sizeof(EchoModule));
    pMod->interp = interp;
    pMod->db = db;
    rc = capdb_create_module_v2(db, "echo_v2", 
        &echoModuleV2, (void*)pMod, moduleDestroy
    );
  }

  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_STATIC);
  return TCL_OK;
}

/*
** Tcl interface to capdb_declare_vtab, invoked as follows from Tcl:
**
** capdb_declare_vtab DB SQL
*/
static int CAPDB_TCLAPI declare_vtab(
  ClientData clientData, /* Pointer to capdb_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  capdb *db;
  int rc;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB SQL");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  rc = capdb_declare_vtab(db, Tcl_GetString(objv[2]));
  if( rc!=CAPDB_OK ){
    Tcl_SetResult(interp, (char *)capdb_errmsg(db), TCL_VOLATILE);
    return TCL_ERROR;
  }
  return TCL_OK;
}

#endif /* ifndef CAPDB_OMIT_VIRTUALTABLE */

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest8_Init(Tcl_Interp *interp){
#ifndef CAPDB_OMIT_VIRTUALTABLE
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
     void *clientData;
  } aObjCmd[] = {
     { "register_echo_module",       register_echo_module, 0 },
     { "capdb_declare_vtab",       declare_vtab, 0 },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, 
        aObjCmd[i].xProc, aObjCmd[i].clientData, 0);
  }
#endif
  return TCL_OK;
}
