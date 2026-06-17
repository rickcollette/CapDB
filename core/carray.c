/*
** 2016-06-29
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
** This file implements a table-valued-function that
** returns the values in a C-language array.
** Examples:
**
**      SELECT * FROM carray($ptr,5)
**
** The query above returns 5 integers contained in a C-language array
** at the address $ptr.  $ptr is a pointer to the array of integers.
** The pointer value must be assigned to $ptr using the
** capdb_bind_pointer() interface with a pointer type of "carray".
** For example:
**
**    static int aX[] = { 53, 9, 17, 2231, 4, 99 };
**    int i = capdb_bind_parameter_index(pStmt, "$ptr");
**    capdb_bind_pointer(pStmt, i, aX, "carray", 0);
**
** There is an optional third parameter to determine the datatype of
** the C-language array.  Allowed values of the third parameter are
** 'int32', 'int64', 'double', 'char*', 'struct iovec'.  Example:
**
**      SELECT * FROM carray($ptr,10,'char*');
**
** The default value of the third parameter is 'int32'.
**
** HOW IT WORKS
**
** The carray "function" is really a virtual table with the
** following schema:
**
**     CREATE TABLE carray(
**       value,
**       pointer HIDDEN,
**       count HIDDEN,
**       ctype TEXT HIDDEN
**     );
**
** If the hidden columns "pointer" and "count" are unconstrained, then 
** the virtual table has no rows.  Otherwise, the virtual table interprets
** the integer value of "pointer" as a pointer to the array and "count"
** as the number of elements in the array.  The virtual table steps through
** the array, element by element.
*/
#if !defined(CAPDB_OMIT_VIRTUALTABLE) && defined(CAPDB_ENABLE_CARRAY)
#include "capdbInt.h"
#if defined(_WIN32) || defined(__RTP__) || defined(_WRS_KERNEL)
  struct iovec {
    void *iov_base;
    size_t iov_len;
  };
#else
# include <sys/uio.h>
#endif

/*
** Names of allowed datatypes
*/
static const char *azCarrayType[] = {
  "int32", "int64", "double", "char*", "struct iovec"
};

/*
** Structure used to hold the capdb_carray_bind() information
*/
typedef struct carray_bind carray_bind;
struct carray_bind {
  void *aData;                /* The data */
  int nData;                  /* Number of elements */
  int mFlags;                 /* Control flags */
  void (*xDel)(void*);        /* Destructor for aData */
  void *pDel;                 /* Alternative argument to xDel() */
};


/* carray_cursor is a subclass of capdb_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct carray_cursor carray_cursor;
struct carray_cursor {
  capdb_vtab_cursor base;  /* Base class - must be first */
  capdb_int64 iRowid;      /* The rowid */
  void *pPtr;                /* Pointer to the array of values */
  capdb_int64 iCnt;        /* Number of integers in the array */
  unsigned char eType;       /* One of the CARRAY_type values */
};

/*
** The carrayConnect() method is invoked to create a new
** carray_vtab that describes the carray virtual table.
**
** Think of this routine as the constructor for carray_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the carray_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the capdb_declare_vtab() interface) what the
**        result set of queries against carray will look like.
*/
static int carrayConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  capdb_vtab *pNew;
  int rc;

/* Column numbers */
#define CARRAY_COLUMN_VALUE   0
#define CARRAY_COLUMN_POINTER 1
#define CARRAY_COLUMN_COUNT   2
#define CARRAY_COLUMN_CTYPE   3

  rc = capdb_declare_vtab(db,
     "CREATE TABLE x(value,pointer hidden,count hidden,ctype hidden)");
  if( rc==CAPDB_OK ){
    pNew = *ppVtab = capdb_malloc( sizeof(*pNew) );
    if( pNew==0 ) return CAPDB_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
  }
  return rc;
}

/*
** This method is the destructor for carray_cursor objects.
*/
static int carrayDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** Constructor for a new carray_cursor object.
*/
static int carrayOpen(capdb_vtab *p, capdb_vtab_cursor **ppCursor){
  carray_cursor *pCur;
  pCur = capdb_malloc( sizeof(*pCur) );
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Destructor for a carray_cursor.
*/
static int carrayClose(capdb_vtab_cursor *cur){
  capdb_free(cur);
  return CAPDB_OK;
}


/*
** Advance a carray_cursor to its next row of output.
*/
static int carrayNext(capdb_vtab_cursor *cur){
  carray_cursor *pCur = (carray_cursor*)cur;
  pCur->iRowid++;
  return CAPDB_OK;
}

/*
** Return values of columns for the row at which the carray_cursor
** is currently pointing.
*/
static int carrayColumn(
  capdb_vtab_cursor *cur,   /* The cursor */
  capdb_context *ctx,       /* First argument to capdb_result_...() */
  int i                       /* Which column to return */
){
  carray_cursor *pCur = (carray_cursor*)cur;
  capdb_int64 x = 0;
  switch( i ){
    case CARRAY_COLUMN_POINTER:   return CAPDB_OK;
    case CARRAY_COLUMN_COUNT:     x = pCur->iCnt;   break;
    case CARRAY_COLUMN_CTYPE: {
      capdb_result_text(ctx, azCarrayType[pCur->eType], -1, CAPDB_STATIC);
      return CAPDB_OK;
    }
    default: {
      switch( pCur->eType ){
        case CARRAY_INT32: {
          int *p = (int*)pCur->pPtr;
          capdb_result_int(ctx, p[pCur->iRowid-1]);
          return CAPDB_OK;
        }
        case CARRAY_INT64: {
          capdb_int64 *p = (capdb_int64*)pCur->pPtr;
          capdb_result_int64(ctx, p[pCur->iRowid-1]);
          return CAPDB_OK;
        }
        case CARRAY_DOUBLE: {
          double *p = (double*)pCur->pPtr;
          capdb_result_double(ctx, p[pCur->iRowid-1]);
          return CAPDB_OK;
        }
        case CARRAY_TEXT: {
          const char **p = (const char**)pCur->pPtr;
          capdb_result_text(ctx, p[pCur->iRowid-1], -1, CAPDB_TRANSIENT);
          return CAPDB_OK;
        }
        default: {
          const struct iovec *p = (struct iovec*)pCur->pPtr;
          assert( pCur->eType==CARRAY_BLOB );
          capdb_result_blob(ctx, p[pCur->iRowid-1].iov_base,
                              (int)p[pCur->iRowid-1].iov_len, CAPDB_TRANSIENT);
          return CAPDB_OK;
        }
      }
    }
  }
  capdb_result_int64(ctx, x);
  return CAPDB_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int carrayRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  carray_cursor *pCur = (carray_cursor*)cur;
  *pRowid = pCur->iRowid;
  return CAPDB_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int carrayEof(capdb_vtab_cursor *cur){
  carray_cursor *pCur = (carray_cursor*)cur;
  return pCur->iRowid>pCur->iCnt;
}

/*
** This method is called to "rewind" the carray_cursor object back
** to the first row of output.
*/
static int carrayFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  carray_cursor *pCur = (carray_cursor *)pVtabCursor;
  pCur->pPtr = 0;
  pCur->iCnt = 0;
  switch( idxNum ){
    case 1: {
      carray_bind *pBind = capdb_value_pointer(argv[0], "carray-bind");
      if( pBind==0 ) break;
      pCur->pPtr = pBind->aData;
      pCur->iCnt = pBind->nData;
      pCur->eType = pBind->mFlags & 0x07;
      break;
    }
    case 2:
    case 3: {
      pCur->pPtr = capdb_value_pointer(argv[0], "carray");
      pCur->iCnt = pCur->pPtr ? capdb_value_int64(argv[1]) : 0;
      if( idxNum<3 ){
        pCur->eType = CARRAY_INT32;
      }else{
        unsigned char i;
        const char *zType = (const char*)capdb_value_text(argv[2]);
        for(i=0; i<sizeof(azCarrayType)/sizeof(azCarrayType[0]); i++){
          if( capdb_stricmp(zType, azCarrayType[i])==0 ) break;
        }
        if( i>=sizeof(azCarrayType)/sizeof(azCarrayType[0]) ){
          pVtabCursor->pVtab->zErrMsg = capdb_mprintf(
            "unknown datatype: %Q", zType);
          return CAPDB_ERROR;
        }else{
          pCur->eType = i;
        }
      }
      break;
    }
  }
  pCur->iRowid = 1;
  return CAPDB_OK;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the carray virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
**
** In this implementation idxNum is used to represent the
** query plan.  idxStr is unused.
**
** idxNum is:
**
**    1    If only the pointer= constraint exists.  In this case, the
**         parameter must be bound using capdb_carray_bind().
**
**    2    if the pointer= and count= constraints exist.
**
**    3    if the ctype= constraint also exists.
**
** idxNum is 0 otherwise and carray becomes an empty table.
*/
static int carrayBestIndex(
  capdb_vtab *tab,
  capdb_index_info *pIdxInfo
){
  int i;                 /* Loop over constraints */
  int ptrIdx = -1;       /* Index of the pointer= constraint, or -1 if none */
  int cntIdx = -1;       /* Index of the count= constraint, or -1 if none */
  int ctypeIdx = -1;     /* Index of the ctype= constraint, or -1 if none */
  unsigned seen = 0;     /* Bitmask of == constrainted columns */

  const struct capdb_index_constraint *pConstraint;
  pConstraint = pIdxInfo->aConstraint;
  for(i=0; i<pIdxInfo->nConstraint; i++, pConstraint++){
    if( pConstraint->op!=CAPDB_INDEX_CONSTRAINT_EQ ) continue;
    if( pConstraint->iColumn>=0 ) seen |= 1 << pConstraint->iColumn;
    if( pConstraint->usable==0 ) continue;
    switch( pConstraint->iColumn ){
      case CARRAY_COLUMN_POINTER:
        ptrIdx = i;
        break;
      case CARRAY_COLUMN_COUNT:
        cntIdx = i;
        break;
      case CARRAY_COLUMN_CTYPE:
        ctypeIdx = i;
        break;
    }
  }
  if( ptrIdx>=0 ){
    pIdxInfo->aConstraintUsage[ptrIdx].argvIndex = 1;
    pIdxInfo->aConstraintUsage[ptrIdx].omit = 1;
    pIdxInfo->estimatedCost = (double)1;
    pIdxInfo->estimatedRows = 100;
    pIdxInfo->idxNum = 1;
    if( cntIdx>=0 ){
      pIdxInfo->aConstraintUsage[cntIdx].argvIndex = 2;
      pIdxInfo->aConstraintUsage[cntIdx].omit = 1;
      pIdxInfo->idxNum = 2;
      if( ctypeIdx>=0 ){
        pIdxInfo->aConstraintUsage[ctypeIdx].argvIndex = 3;
        pIdxInfo->aConstraintUsage[ctypeIdx].omit = 1;
        pIdxInfo->idxNum = 3;
      }else if( seen & (1<<CARRAY_COLUMN_CTYPE) ){
        /* In a three-argument carray(), we need to know the value of all
        ** three arguments */
        return CAPDB_CONSTRAINT;
      }
    }else if( seen & (1<<CARRAY_COLUMN_COUNT) ){
      /* In a two-argument carray(), we need to know the value of both
      ** arguments */
      return CAPDB_CONSTRAINT;
    }
  }else{
    pIdxInfo->estimatedCost = (double)2147483647;
    pIdxInfo->estimatedRows = 2147483647;
    pIdxInfo->idxNum = 0;
  }
  return CAPDB_OK;
}

/*
** This following structure defines all the methods for the 
** carray virtual table.
*/
static capdb_module carrayModule = {
  0,                         /* iVersion */
  0,                         /* xCreate */
  carrayConnect,             /* xConnect */
  carrayBestIndex,           /* xBestIndex */
  carrayDisconnect,          /* xDisconnect */
  0,                         /* xDestroy */
  carrayOpen,                /* xOpen - open a cursor */
  carrayClose,               /* xClose - close a cursor */
  carrayFilter,              /* xFilter - configure scan constraints */
  carrayNext,                /* xNext - advance a cursor */
  carrayEof,                 /* xEof - check for end of scan */
  carrayColumn,              /* xColumn - read data */
  carrayRowid,               /* xRowid - read data */
  0,                         /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindMethod */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  0,                         /* xShadow */
  0                          /* xIntegrity */
};

/*
** Destructor for the carray_bind object
*/
static void carrayBindDel(void *pPtr){
  carray_bind *p = (carray_bind*)pPtr;
  if( p->xDel!=CAPDB_STATIC ){
    p->xDel(p->pDel);
  }
  capdb_free(p);
}

/*
** Invoke this interface in order to bind to the single-argument
** version of CARRAY().
**
**    pStmt        The prepared statement to which to bind
**    idx          The index of the parameter of pStmt to which to bind
**    aData        The data to be bound
**    nData        The number of elements in aData
**    mFlags       One of CAPDB_CARRAY_xxxx indicating datatype of aData
**    xDestroy     Destructor for pDestroy or aData if pDestroy==NULL.
**    pDestroy     Invoke xDestroy on this pointer if not NULL
**
** The destructor is called pDestroy if pDestroy!=NULL, or against
** aData if pDestroy==NULL.
*/
CAPDB_API int capdb_carray_bind_v2(
  capdb_stmt *pStmt,
  int idx,
  void *aData,
  int nData,
  int mFlags,
  void (*xDestroy)(void*),
  void *pDestroy
){
  carray_bind *pNew = 0;
  int i;
  int rc = CAPDB_OK;
  
  /* Ensure that the mFlags value is acceptable. */
  assert( CARRAY_INT32==0 && CARRAY_INT64==1 && CARRAY_DOUBLE==2 );
  assert( CARRAY_TEXT==3 && CARRAY_BLOB==4 );
  if( mFlags<CARRAY_INT32 || mFlags>CARRAY_BLOB ){
    rc = CAPDB_ERROR;
    goto carray_bind_error;
  }

  pNew = capdb_malloc64(sizeof(*pNew));
  if( pNew==0 ){
    rc = CAPDB_NOMEM;
    goto carray_bind_error;
  }

  pNew->nData = nData;
  pNew->mFlags = mFlags;
  if( xDestroy==CAPDB_TRANSIENT ){
    capdb_int64 sz = nData;
    switch( mFlags ){
      case CARRAY_INT32:   sz *= 4;                     break;
      case CARRAY_INT64:   sz *= 8;                     break;
      case CARRAY_DOUBLE:  sz *= 8;                     break;
      case CARRAY_TEXT:    sz *= sizeof(char*);         break;
      default:             sz *= sizeof(struct iovec);  break;
    }
    if( mFlags==CARRAY_TEXT ){
      for(i=0; i<nData; i++){
        const char *z = ((char**)aData)[i];
        if( z ) sz += strlen(z) + 1;
      }
    }else if( mFlags==CARRAY_BLOB ){
      for(i=0; i<nData; i++){
        sz += ((struct iovec*)aData)[i].iov_len;
      }
    } 

    pNew->aData = capdb_malloc64( sz );
    if( pNew->aData==0 ){
      rc = CAPDB_NOMEM;
      goto carray_bind_error;
    }

    if( mFlags==CARRAY_TEXT ){
      char **az = (char**)pNew->aData;
      char *z = (char*)&az[nData];
      for(i=0; i<nData; i++){
        const char *zData = ((char**)aData)[i];
        capdb_int64 n;
        if( zData==0 ){
          az[i] = 0;
          continue;
        }
        az[i] = z;
        n = strlen(zData);
        memcpy(z, zData, n+1);
        z += n+1;
      }
    }else if( mFlags==CARRAY_BLOB ){
      struct iovec *p = (struct iovec*)pNew->aData;
      unsigned char *z = (unsigned char*)&p[nData];
      for(i=0; i<nData; i++){
        size_t n = ((struct iovec*)aData)[i].iov_len;
        p[i].iov_len = n;
        p[i].iov_base = z;
        z += n;
        memcpy(p[i].iov_base, ((struct iovec*)aData)[i].iov_base, n);
      }
    }else{
      memcpy(pNew->aData, aData, sz);
    }
    pNew->xDel = capdb_free;
    pNew->pDel = pNew->aData;
  }else{
    pNew->aData = aData;
    pNew->xDel = xDestroy;
    pNew->pDel = pDestroy;
  }
  return capdb_bind_pointer(pStmt, idx, pNew, "carray-bind", carrayBindDel);
 
 carray_bind_error:
  if( xDestroy!=CAPDB_STATIC && xDestroy!=CAPDB_TRANSIENT ){
    xDestroy(pDestroy);
  }
  capdb_free(pNew);
  return rc;
}

/*
** Invoke this interface in order to bind to the single-argument
** version of CARRAY().  Same as capdb_carray_bind_v2() with the
** pDestroy parameter set to NULL.
*/
CAPDB_API int capdb_carray_bind(
  capdb_stmt *pStmt,
  int idx,
  void *aData,
  int nData,
  int mFlags,
  void (*xDestroy)(void*)
){
  return capdb_carray_bind_v2(pStmt,idx,aData,nData,mFlags,xDestroy,aData);
}

/*
** Invoke this routine to register the carray() function.
*/
Module *capdbCarrayRegister(capdb *db){
  return capdbVtabCreateModule(db, "carray", &carrayModule, 0, 0);
}

#endif /* !defined(CAPDB_OMIT_VIRTUALTABLE) && defined(CAPDB_ENABLE_CARRAY) */
