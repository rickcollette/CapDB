/*
** 2017-08-10
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
** This file implements a virtual table that prints diagnostic information
** on stdout when its key interfaces are called.  This is intended for
** interactive analysis and debugging of virtual table interfaces.
**
** HOW TO COMPILE:
**
** To build this extension as a separately loaded shared library or
** DLL, use compiler command-lines similar to the following:
**
**   (linux)    gcc -fPIC -shared vtablog.c -o vtablog.so
**   (mac)      clang -fPIC -dynamiclib vtablog.c -o vtablog.dylib
**   (windows)  cl vtablog.c -link -dll -out:vtablog.dll
**
** USAGE EXAMPLE:
**
**     .load ./vtablog
**     CREATE VIRTUAL TABLE temp.log USING vtablog(
**        schema='CREATE TABLE x(a,b,c)',
**        rows=25
**     );
**     SELECT * FROM log;
**
** ARGUMENTS TO CREATE VIRTUAL TABLE:
**
** In "CREATE VIRTUAL TABLE temp.log AS vtablog(ARGS....)" statement, the
** ARGS argument is a list of key-value pairs that can be any of the
** following.
**
**     schema=TEXT            Text is a CREATE TABLE statement that defines
**                            the schema of the new virtual table.
**
**     rows=N                 The table as N rows.
**
**     consume_order_by=N     If the left-most ORDER BY terms is ASC and
**                            against column N (where the leftmost column
**                            is #1) then set the orderByConsumed=1 flag in
**                            xBestIndex.  Or if the left-most ORDER BY is
**                            DESC and against column -N, do likewise.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>


/* vtablog_vtab is a subclass of capdb_vtab which will
** serve as the underlying representation of a vtablog virtual table
*/
typedef struct vtablog_vtab vtablog_vtab;
struct vtablog_vtab {
  capdb_vtab base;  /* Base class - must be first */
  char *zDb;          /* Schema name.  argv[1] of xConnect/xCreate */
  char *zName;        /* Table name.  argv[2] of xConnect/xCreate */
  int nRow;           /* Number of rows in the table */
  int nCursor;        /* Number of cursors created */
  int iConsumeOB;     /* Consume the ORDER BY clause if on column N-th
                      ** and consumeOB=N or consumeOB=(-N) and DESC */
};

/* vtablog_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct vtablog_cursor vtablog_cursor;
struct vtablog_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  int iCursor;               /* Cursor number */
  capdb_int64 iRowid;      /* The rowid */
};

/* Skip leading whitespace.  Return a pointer to the first non-whitespace
** character, or to the zero terminator if the string has only whitespace */
static const char *vtablog_skip_whitespace(const char *z){
  while( isspace((unsigned char)z[0]) ) z++;
  return z;
}

/* Remove trailing whitespace from the end of string z[] */
static void vtablog_trim_whitespace(char *z){
  size_t n = strlen(z);
  while( n>0 && isspace((unsigned char)z[n]) ) n--;
  z[n] = 0;
}

/* Dequote the string */
static void vtablog_dequote(char *z){
  int j;
  char cQuote = z[0];
  size_t i, n;

  if( cQuote!='\'' && cQuote!='"' ) return;
  n = strlen(z);
  if( n<2 || z[n-1]!=z[0] ) return;
  for(i=1, j=0; i<n-1; i++){
    if( z[i]==cQuote && z[i+1]==cQuote ) i++;
    z[j++] = z[i];
  }
  z[j] = 0;
}

/* Check to see if the string is of the form:  "TAG = VALUE" with optional
** whitespace before and around tokens.  If it is, return a pointer to the
** first character of VALUE.  If it is not, return NULL.
*/
static const char *vtablog_parameter(const char *zTag, int nTag, const char *z){
  z = vtablog_skip_whitespace(z);
  if( strncmp(zTag, z, nTag)!=0 ) return 0;
  z = vtablog_skip_whitespace(z+nTag);
  if( z[0]!='=' ) return 0;
  return vtablog_skip_whitespace(z+1);
}

/* Decode a parameter that requires a dequoted string.
**
** Return non-zero on an error.
*/
static int vtablog_string_parameter(
  char **pzErr,            /* Leave the error message here, if there is one */
  const char *zParam,      /* Parameter we are checking for */
  const char *zArg,        /* Raw text of the virtual table argment */
  char **pzVal             /* Write the dequoted string value here */
){
  const char *zValue;
  zValue = vtablog_parameter(zParam,(int)strlen(zParam),zArg);
  if( zValue==0 ) return 0;
  if( *pzVal ){
    *pzErr = capdb_mprintf("more than one '%s' parameter", zParam);
    return 1;
  }
  *pzVal = capdb_mprintf("%s", zValue);
  if( *pzVal==0 ){
    *pzErr = capdb_mprintf("out of memory");
    return 1;
  }
  vtablog_trim_whitespace(*pzVal);
  vtablog_dequote(*pzVal);
  return 0;
}

#if 0 /* not used - yet */
/* Return 0 if the argument is false and 1 if it is true.  Return -1 if
** we cannot really tell.
*/
static int vtablog_boolean(const char *z){
  if( capdb_stricmp("yes",z)==0
   || capdb_stricmp("on",z)==0
   || capdb_stricmp("true",z)==0
   || (z[0]=='1' && z[1]==0)
  ){
    return 1;
  }
  if( capdb_stricmp("no",z)==0
   || capdb_stricmp("off",z)==0
   || capdb_stricmp("false",z)==0
   || (z[0]=='0' && z[1]==0)
  ){
    return 0;
  }
  return -1;
}
#endif

/*
** The vtablogConnect() method is invoked to create a new
** vtablog_vtab that describes the vtablog virtual table.
**
** Think of this routine as the constructor for vtablog_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the vtablog_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the capdb_declare_vtab() interface) what the
**        result set of queries against vtablog will look like.
*/
static int vtablogConnectCreate(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr,
  int isCreate
){
  vtablog_vtab *pNew;
  int i;
  int rc;
  char *zSchema = 0;
  char *zNRow = 0;
  char *zConsumeOB = 0;

  printf("%s.%s.%s():\n", argv[1], argv[2], 
         isCreate ? "xCreate" : "xConnect");
  printf("  argc=%d\n", argc);
  for(i=0; i<argc; i++){
    printf("  argv[%d] = ", i);
    if( argv[i] ){
      printf("[%s]\n", argv[i]);
    }else{
      printf("NULL\n");
    }
  }

  for(i=3; i<argc; i++){
    const char *z = argv[i];
    if( vtablog_string_parameter(pzErr, "schema", z, &zSchema) ){
      rc = CAPDB_ERROR;
      goto vtablog_end_connect;
    }
    if( vtablog_string_parameter(pzErr, "rows", z, &zNRow) ){
      rc = CAPDB_ERROR;
      goto vtablog_end_connect;
    }
    if( vtablog_string_parameter(pzErr, "consume_order_by", z, &zConsumeOB) ){
      rc = CAPDB_ERROR;
      goto vtablog_end_connect;
    }
  }
  if( zSchema==0 ){
    zSchema = capdb_mprintf("%s","CREATE TABLE x(a,b);");
    if( zSchema==0 ){
      rc = CAPDB_NOMEM;
      goto vtablog_end_connect;
    }
  }
  printf("  schema = '%s'\n", zSchema);
  rc = capdb_declare_vtab(db, zSchema);
  if( rc==CAPDB_OK ){
    pNew = capdb_malloc64( sizeof(*pNew) );
    *ppVtab = (capdb_vtab*)pNew;
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    pNew->nRow = 10;
    if( zNRow ) pNew->nRow = atoi(zNRow);
    printf("  nrow = %d\n", pNew->nRow);
    if( zConsumeOB ) pNew->iConsumeOB = atoi(zConsumeOB);
    if( pNew->iConsumeOB ){
      printf("  consume_order_by = %d\n", pNew->iConsumeOB);
    }
    pNew->zDb = capdb_mprintf("%s", argv[1]);
    pNew->zName = capdb_mprintf("%s", argv[2]);
  }

vtablog_end_connect:
  capdb_free(zSchema);
  capdb_free(zNRow);
  capdb_free(zConsumeOB);
  return rc;
}
static int vtablogCreate(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  return vtablogConnectCreate(db,pAux,argc,argv,ppVtab,pzErr,1);
}
static int vtablogConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  return vtablogConnectCreate(db,pAux,argc,argv,ppVtab,pzErr,0);
}


/*
** This method is the destructor for vtablog_vtab objects.
*/
static int vtablogDisconnect(capdb_vtab *pVtab){
  vtablog_vtab *pTab = (vtablog_vtab*)pVtab;
  printf("%s.%s.xDisconnect()\n", pTab->zDb, pTab->zName);
  capdb_free(pTab->zDb);
  capdb_free(pTab->zName);
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** This method is (also) the destructor for vtablog_vtab objects.
*/
static int vtablogDestroy(capdb_vtab *pVtab){
  vtablog_vtab *pTab = (vtablog_vtab*)pVtab;
  printf("%s.%s.xDestroy()\n", pTab->zDb, pTab->zName);
  capdb_free(pTab->zDb);
  capdb_free(pTab->zName);
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** Constructor for a new vtablog_cursor object.
*/
static int vtablogOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  vtablog_vtab *pTab = (vtablog_vtab*)p;
  vtablog_cursor *pCur;
  printf("%s.%s.xOpen(cursor=%d)\n", pTab->zDb, pTab->zName,
         ++pTab->nCursor);
  pCur = capdb_malloc64( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->iCursor = pTab->nCursor;
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Destructor for a vtablog_cursor.
*/
static int vtablogClose(capdb_vtab_cursor *cur){
  vtablog_cursor *pCur = (vtablog_cursor*)cur;
  vtablog_vtab *pTab = (vtablog_vtab*)cur->pVtab;
  printf("%s.%s.xClose(cursor=%d)\n", pTab->zDb, pTab->zName, pCur->iCursor);
  capdb_free(cur);
  return CAPDB_OK;
}


/*
** Advance a vtablog_cursor to its next row of output.
*/
static int vtablogNext(capdb_vtab_cursor *cur){
  vtablog_cursor *pCur = (vtablog_cursor*)cur;
  vtablog_vtab *pTab = (vtablog_vtab*)cur->pVtab;
  printf("%s.%s.xNext(cursor=%d)  rowid %d -> %d\n", 
         pTab->zDb, pTab->zName, pCur->iCursor,
         (int)pCur->iRowid, (int)pCur->iRowid+1);
  pCur->iRowid++;
  return CAPDB_OK;
}

/*
** Return values of columns for the row at which the vtablog_cursor
** is currently pointing.
*/
static int vtablogColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  vtablog_cursor *pCur = (vtablog_cursor*)cur;
  vtablog_vtab *pTab = (vtablog_vtab*)cur->pVtab;
  char zVal[50];

  if( i<26 ){
    capdb_snprintf(sizeof(zVal),zVal,"%c%d", 
                     "abcdefghijklmnopqrstuvwyz"[i], pCur->iRowid);
  }else{
    capdb_snprintf(sizeof(zVal),zVal,"{%d}%d", i, pCur->iRowid);
  }
  printf("%s.%s.xColumn(cursor=%d, i=%d): [%s]\n",
         pTab->zDb, pTab->zName, pCur->iCursor, i, zVal);
  capdb_result_text(ctx, zVal, -1, CAPDB_TRANSIENT);
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int vtablogRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  vtablog_cursor *pCur = (vtablog_cursor*)cur;
  vtablog_vtab *pTab = (vtablog_vtab*)cur->pVtab;
  printf("%s.%s.xRowid(cursor=%d): %d\n",
         pTab->zDb, pTab->zName, pCur->iCursor, (int)pCur->iRowid);
  *pRowid = pCur->iRowid;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int vtablogEof(capdb_vtab_cursor *cur){
  vtablog_cursor *pCur = (vtablog_cursor*)cur;
  vtablog_vtab *pTab = (vtablog_vtab*)cur->pVtab;
  int rc = pCur->iRowid >= pTab->nRow;
  printf("%s.%s.xEof(cursor=%d): %d\n",
         pTab->zDb, pTab->zName, pCur->iCursor, rc);
  return rc;
}

/*
** Output an capdb_value object's value as an SQL literal.
*/
static void vtablogQuote(capdb_value *p){
  char z[50];
  switch( capdb_value_type(p) ){
    case CAPDB_NULL: {
      printf("NULL");
      break;
    }
    case CAPDB_INTEGER: {
      capdb_snprintf(50,z,"%lld", capdb_value_int64(p));
      printf("%s", z);
      break;
    }
    case CAPDB_FLOAT: {
      capdb_snprintf(50,z,"%!.20g", capdb_value_double(p));
      printf("%s", z);
      break;
    }
    case CAPDB_BLOB: {
      int n = capdb_value_bytes(p);
      const unsigned char *z = (const unsigned char*)capdb_value_blob(p);
      int i;
      printf("x'");
      for(i=0; i<n; i++) printf("%02x", z[i]);
      printf("'");
      break;
    }
    case CAPDB_TEXT: {
      const char *z = (const char*)capdb_value_text(p);
      int i;
      char c;
      for(i=0; (c = z[i])!=0 && c!='\''; i++){}
      if( c==0 ){
        printf("'%s'",z);
      }else{
        printf("'");
        while( *z ){
          for(i=0; (c = z[i])!=0 && c!='\''; i++){}
          if( c=='\'' ) i++;
          if( i ){
            printf("%.*s", i, z);
            z += i;
          }
          if( c=='\'' ){
            printf("'");
            continue;
          }
          if( c==0 ){
            break;
          }
          z++;
        }
        printf("'");
      }
      break;
    }
  }
}


/*
** This method is called to "rewind" the vtablog_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to vtablogColumn() or vtablogRowid() or 
** vtablogEof().
*/
static int vtablogFilter(
  capdb_vtab_cursor *cur,
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  vtablog_cursor *pCur = (vtablog_cursor *)cur;
  vtablog_vtab *pTab = (vtablog_vtab*)cur->pVtab;
  printf("%s.%s.xFilter(cursor=%d):\n", pTab->zDb, pTab->zName, pCur->iCursor);
  pCur->iRowid = 0;
  return CAPDB_OK;
}

/*
** Return an capdb_index_info operator name in static space.
** The name is possibly overwritten on subsequent calls.
*/
static char *vtablogOpName(unsigned char op){
  static char zUnknown[30];
  char *zOut;
  switch( op ){
    case CAPDB_INDEX_CONSTRAINT_EQ:        zOut = "EQ";        break;
    case CAPDB_INDEX_CONSTRAINT_GT:        zOut = "GT";        break;
    case CAPDB_INDEX_CONSTRAINT_LE:        zOut = "LE";        break;
    case CAPDB_INDEX_CONSTRAINT_LT:        zOut = "LT";        break;
    case CAPDB_INDEX_CONSTRAINT_GE:        zOut = "GE";        break;
    case CAPDB_INDEX_CONSTRAINT_MATCH:     zOut = "MATCH";     break;
    case CAPDB_INDEX_CONSTRAINT_LIKE:      zOut = "LIKE";      break;
    case CAPDB_INDEX_CONSTRAINT_GLOB:      zOut = "GLOB";      break;
    case CAPDB_INDEX_CONSTRAINT_REGEXP:    zOut = "REGEXP";    break;
    case CAPDB_INDEX_CONSTRAINT_NE:        zOut = "NE";        break;
    case CAPDB_INDEX_CONSTRAINT_ISNOT:     zOut = "ISNOT";     break;
    case CAPDB_INDEX_CONSTRAINT_ISNOTNULL: zOut = "ISNOTNULL"; break;
    case CAPDB_INDEX_CONSTRAINT_ISNULL:    zOut = "ISNULL";    break;
    case CAPDB_INDEX_CONSTRAINT_IS:        zOut = "IS";        break;
    case CAPDB_INDEX_CONSTRAINT_LIMIT:     zOut = "LIMIT";     break;
    case CAPDB_INDEX_CONSTRAINT_OFFSET:    zOut = "OFFSET";    break;
    case CAPDB_INDEX_CONSTRAINT_FUNCTION:  zOut = "FUNCTION";  break;
    default:
      capdb_snprintf(sizeof(zUnknown),zUnknown,"%d",op);
      zOut = zUnknown;
      break;
  }
  return zOut;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the vtablog virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int vtablogBestIndex(
  capdb_vtab *tab,
  capdb_index_info *p
){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  int i;
  printf("%s.%s.xBestIndex():\n", pTab->zDb, pTab->zName);
  printf("  colUsed: 0x%016llx\n", p->colUsed);
  printf("  nConstraint: %d\n", p->nConstraint);
  for(i=0; i<p->nConstraint; i++){
    capdb_value *pVal = 0;
    int rc = capdb_vtab_rhs_value(p, i, &pVal);
    printf(
      "  constraint[%d]: col=%d termid=%d op=%s usabled=%d coll=%s rhs=",
       i,
       p->aConstraint[i].iColumn,
       p->aConstraint[i].iTermOffset,
       vtablogOpName(p->aConstraint[i].op),
       p->aConstraint[i].usable,
       capdb_vtab_collation(p,i)
    );
    if( rc==CAPDB_OK ){
      vtablogQuote(pVal);
      printf("\n");
    }else{
      printf("N/A\n");
    }
  }
  printf("  nOrderBy: %d\n", p->nOrderBy);
  if( p->nOrderBy ){
    for(i=0; i<p->nOrderBy; i++){
      printf("  orderby[%d]: col=%d desc=%d\n",
         i,
         p->aOrderBy[i].iColumn,
         p->aOrderBy[i].desc);
    }
    if( pTab->iConsumeOB ){
      int N = p->aOrderBy[0].iColumn+1;
      if( (p->aOrderBy[0].desc && N==-pTab->iConsumeOB)
       || (!p->aOrderBy[0].desc && N==pTab->iConsumeOB)
      ){
        p->orderByConsumed = 1;
      }
    }
  }
  p->estimatedCost = (double)500;
  p->estimatedRows = 500;
  printf("  idxNum=%d\n", p->idxNum);
  printf("  idxStr=NULL\n");
  printf("  capdb_vtab_distinct()=%d\n", capdb_vtab_distinct(p));
  printf("  orderByConsumed=%d\n", p->orderByConsumed);
  printf("  estimatedCost=%g\n", p->estimatedCost);
  printf("  estimatedRows=%lld\n", p->estimatedRows);
  return CAPDB_OK;
}

/*
** SQLite invokes this method to INSERT, UPDATE, or DELETE content from
** the table. 
**
** This implementation does not actually make any changes to the table
** content.  It merely logs the fact that the method was invoked
*/
static int vtablogUpdate(
  capdb_vtab *tab,
  int argc,
  capdb_value **argv,
  sqlite_int64 *pRowid
){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  int i;
  printf("%s.%s.xUpdate():\n", pTab->zDb, pTab->zName);
  printf("  argc=%d\n", argc);
  for(i=0; i<argc; i++){
    printf("  argv[%d]=", i);
    vtablogQuote(argv[i]);
    printf("\n");
  }
  return CAPDB_OK;
}

static int vtablogBegin(capdb_vtab *tab){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xBegin()\n", pTab->zDb, pTab->zName);
  return CAPDB_OK;
}
static int vtablogSync(capdb_vtab *tab){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xSync()\n", pTab->zDb, pTab->zName);
  return CAPDB_OK;
}
static int vtablogCommit(capdb_vtab *tab){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xCommit()\n", pTab->zDb, pTab->zName);
  return CAPDB_OK;
}
static int vtablogRollback(capdb_vtab *tab){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xRollback()\n", pTab->zDb, pTab->zName);
  return CAPDB_OK;
}
static int vtablogSavepoint(capdb_vtab *tab, int N){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xSavepoint(%d)\n", pTab->zDb, pTab->zName, N);
  return CAPDB_OK;
}
static int vtablogRelease(capdb_vtab *tab, int N){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xRelease(%d)\n", pTab->zDb, pTab->zName, N);
  return CAPDB_OK;
}
static int vtablogRollbackTo(capdb_vtab *tab, int N){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xRollbackTo(%d)\n", pTab->zDb, pTab->zName, N);
  return CAPDB_OK;
}

static int vtablogFindMethod(
  capdb_vtab *tab,
  int nArg,
  const char *zName,
  void (**pxFunc)(capdb_context*,int,capdb_value**),
  void **ppArg
){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xFindMethod(nArg=%d, zName=%s)\n",
         pTab->zDb, pTab->zName, nArg, zName);
  return CAPDB_OK;
}
static int vtablogRename(capdb_vtab *tab, const char *zNew){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xRename('%s')\n", pTab->zDb, pTab->zName, zNew);
  capdb_free(pTab->zName);
  pTab->zName = capdb_mprintf("%s", zNew);
  return CAPDB_OK;
}

/* Any table name that contains the text "shadow" is seen as a
** shadow table.  Nothing else is.
*/
static int vtablogShadowName(const char *zName){
  printf("vtablog.xShadowName('%s')\n", zName);
  return capdb_strglob("*shadow*", zName)==0;
}

static int vtablogIntegrity(
  capdb_vtab *tab,
  const char *zSchema,
  const char *zTabName,
  int mFlags,
  char **pzErr
){
  vtablog_vtab *pTab = (vtablog_vtab*)tab;
  printf("%s.%s.xIntegrity(mFlags=0x%x)\n", pTab->zDb, pTab->zName, mFlags);
  return 0;
}

/*
** This following structure defines all the methods for the 
** vtablog virtual table.
*/
static capdb_module vtablogModule = {
  4,                         /* iVersion */
  vtablogCreate,             /* xCreate */
  vtablogConnect,            /* xConnect */
  vtablogBestIndex,          /* xBestIndex */
  vtablogDisconnect,         /* xDisconnect */
  vtablogDestroy,            /* xDestroy */
  vtablogOpen,               /* xOpen - open a cursor */
  vtablogClose,              /* xClose - close a cursor */
  vtablogFilter,             /* xFilter - configure scan constraints */
  vtablogNext,               /* xNext - advance a cursor */
  vtablogEof,                /* xEof - check for end of scan */
  vtablogColumn,             /* xColumn - read data */
  vtablogRowid,              /* xRowid - read data */
  vtablogUpdate,             /* xUpdate */
  vtablogBegin,              /* xBegin */
  vtablogSync,               /* xSync */
  vtablogCommit,             /* xCommit */
  vtablogRollback,           /* xRollback */
  vtablogFindMethod,         /* xFindMethod */
  vtablogRename,             /* xRename */
  vtablogSavepoint,          /* xSavepoint */
  vtablogRelease,            /* xRelease */
  vtablogRollbackTo,         /* xRollbackTo */
  vtablogShadowName,         /* xShadowName */
  vtablogIntegrity           /* xIntegrity */
};

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_vtablog_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc;
  CAPDB_EXTENSION_INIT2(pApi);
  rc = capdb_create_module(db, "vtablog", &vtablogModule, 0);
  return rc;
}
