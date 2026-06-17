/*
** 2004 May 26
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
** This file contains code use to implement APIs that are part of the
** VDBE.
*/
#include "capdbInt.h"
#include "vdbeInt.h"
#include "opcodes.h"

#ifndef CAPDB_OMIT_DEPRECATED
/*
** Return TRUE (non-zero) of the statement supplied as an argument needs
** to be recompiled.  A statement needs to be recompiled whenever the
** execution environment changes in a way that would alter the program
** that capdb_prepare() generates.  For example, if new functions or
** collating sequences are registered or if an authorizer function is
** added or changed.
*/
int capdb_expired(capdb_stmt *pStmt){
  Vdbe *p = (Vdbe*)pStmt;
  return p==0 || p->expired;
}
#endif

/*
** Check on a Vdbe to make sure it has not been finalized.  Log
** an error and return true if it has been finalized (or is otherwise
** invalid).  Return false if it is ok.
*/
static int vdbeSafety(Vdbe *p){
  if( p->db==0 ){
    capdb_log(CAPDB_MISUSE, "API called with finalized prepared statement");
    return 1;
  }else{
    return 0;
  }
}
static int vdbeSafetyNotNull(Vdbe *p){
  if( p==0 ){
    capdb_log(CAPDB_MISUSE, "API called with NULL prepared statement");
    return 1;
  }else{
    return vdbeSafety(p);
  }
}

#ifndef CAPDB_OMIT_TRACE
/*
** Invoke the profile callback.  This routine is only called if we already
** know that the profile callback is defined and needs to be invoked.
*/
static CAPDB_NOINLINE void invokeProfileCallback(capdb *db, Vdbe *p){
  capdb_int64 iNow;
  capdb_int64 iElapse;
  assert( p->startTime>0 );
  assert( db->init.busy==0 );
  assert( p->zSql!=0 );
  capdbOsCurrentTimeInt64(db->pVfs, &iNow);
  iElapse = (iNow - p->startTime)*1000000;
#ifndef CAPDB_OMIT_DEPRECATED
  if( db->xProfile ){
    db->xProfile(db->pProfileArg, p->zSql, iElapse);
  }
#endif
  if( db->mTrace & CAPDB_TRACE_PROFILE ){
    db->trace.xV2(CAPDB_TRACE_PROFILE, db->pTraceArg, p, (void*)&iElapse);
  }
  p->startTime = 0;
}
/*
** The checkProfileCallback(DB,P) macro checks to see if a profile callback
** is needed, and it invokes the callback if it is needed.
*/
# define checkProfileCallback(DB,P) \
   if( ((P)->startTime)>0 ){ invokeProfileCallback(DB,P); }
#else
# define checkProfileCallback(DB,P)  /*no-op*/
#endif

/*
** The following routine destroys a virtual machine that is created by
** the capdb_compile() routine. The integer returned is an CAPDB_
** success/failure code that describes the result of executing the virtual
** machine.
**
** This routine sets the error code and string returned by
** capdb_errcode(), capdb_errmsg() and capdb_errmsg16().
*/
int capdb_finalize(capdb_stmt *pStmt){
  int rc;
  if( pStmt==0 ){
    /* IMPLEMENTATION-OF: R-57228-12904 Invoking capdb_finalize() on a NULL
    ** pointer is a harmless no-op. */
    rc = CAPDB_OK;
  }else{
    Vdbe *v = (Vdbe*)pStmt;
    capdb *db = v->db;
    if( vdbeSafety(v) ) return CAPDB_MISUSE_BKPT;
    capdb_mutex_enter(db->mutex);
    checkProfileCallback(db, v);
    assert( v->eVdbeState>=VDBE_READY_STATE );
    rc = capdbVdbeReset(v);
    capdbVdbeDelete(v);
    rc = capdbApiExit(db, rc);
    capdbLeaveMutexAndCloseZombie(db);
  }
  return rc;
}

/*
** Terminate the current execution of an SQL statement and reset it
** back to its starting state so that it can be reused. A success code from
** the prior execution is returned.
**
** This routine sets the error code and string returned by
** capdb_errcode(), capdb_errmsg() and capdb_errmsg16().
*/
int capdb_reset(capdb_stmt *pStmt){
  int rc;
  if( pStmt==0 ){
    rc = CAPDB_OK;
  }else{
    Vdbe *v = (Vdbe*)pStmt;
    capdb *db = v->db;
    capdb_mutex_enter(db->mutex);
    checkProfileCallback(db, v);
    rc = capdbVdbeReset(v);
    capdbVdbeRewind(v);
    assert( (rc & (db->errMask))==rc );
    rc = capdbApiExit(db, rc);
    capdb_mutex_leave(db->mutex);
  }
  return rc;
}

/*
** Set all the parameters in the compiled SQL statement to NULL.
*/
int capdb_clear_bindings(capdb_stmt *pStmt){
  int i;
  int rc = CAPDB_OK;
  Vdbe *p = (Vdbe*)pStmt;
#if CAPDB_THREADSAFE
  capdb_mutex *mutex;
#endif
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pStmt==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
#if CAPDB_THREADSAFE
  mutex = p->db->mutex;
#endif
  capdb_mutex_enter(mutex);
  for(i=0; i<p->nVar; i++){
    capdbVdbeMemRelease(&p->aVar[i]);
    p->aVar[i].flags = MEM_Null;
  }
  assert( (p->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 || p->expmask==0 );
  if( p->expmask ){
    p->expired = 1;
  }
  capdb_mutex_leave(mutex);
  return rc;
}


/**************************** capdb_value_  *******************************
** The following routines extract information from a Mem or capdb_value
** structure.
*/
const void *capdb_value_blob(capdb_value *pVal){
  Mem *p = (Mem*)pVal;
  if( p->flags & (MEM_Blob|MEM_Str) ){
    if( ExpandBlob(p)!=CAPDB_OK ){
      assert( p->flags==MEM_Null && p->z==0 );
      return 0;
    }
    p->flags |= MEM_Blob;
    return p->n ? p->z : 0;
  }else{
    return capdb_value_text(pVal);
  }
}
int capdb_value_bytes(capdb_value *pVal){
  return capdbValueBytes(pVal, CAPDB_UTF8);
}
int capdb_value_bytes16(capdb_value *pVal){
  return capdbValueBytes(pVal, CAPDB_UTF16NATIVE);
}
double capdb_value_double(capdb_value *pVal){
  return capdbVdbeRealValue((Mem*)pVal);
}
int capdb_value_int(capdb_value *pVal){
  return (int)capdbVdbeIntValue((Mem*)pVal);
}
sqlite_int64 capdb_value_int64(capdb_value *pVal){
  return capdbVdbeIntValue((Mem*)pVal);
}
unsigned int capdb_value_subtype(capdb_value *pVal){
  Mem *pMem = (Mem*)pVal;
  return ((pMem->flags & MEM_Subtype) ? pMem->eSubtype : 0);
}
void *capdb_value_pointer(capdb_value *pVal, const char *zPType){
  Mem *p = (Mem*)pVal;
  if( (p->flags&(MEM_TypeMask|MEM_Term|MEM_Subtype)) ==
                 (MEM_Null|MEM_Term|MEM_Subtype)
   && zPType!=0
   && p->eSubtype=='p'
   && strcmp(p->u.zPType, zPType)==0
  ){
    return (void*)p->z;
  }else{
    return 0;
  }
}
const unsigned char *capdb_value_text(capdb_value *pVal){
  return (const unsigned char *)capdbValueText(pVal, CAPDB_UTF8);
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_value_text16(capdb_value* pVal){
  return capdbValueText(pVal, CAPDB_UTF16NATIVE);
}
const void *capdb_value_text16be(capdb_value *pVal){
  return capdbValueText(pVal, CAPDB_UTF16BE);
}
const void *capdb_value_text16le(capdb_value *pVal){
  return capdbValueText(pVal, CAPDB_UTF16LE);
}
#endif /* CAPDB_OMIT_UTF16 */
/* EVIDENCE-OF: R-12793-43283 Every value in SQLite has one of five
** fundamental datatypes: 64-bit signed integer 64-bit IEEE floating
** point number string BLOB NULL
*/
int capdb_value_type(capdb_value* pVal){
  static const u8 aType[] = {
     CAPDB_BLOB,     /* 0x00 (not possible) */
     CAPDB_NULL,     /* 0x01 NULL */
     CAPDB_TEXT,     /* 0x02 TEXT */
     CAPDB_NULL,     /* 0x03 (not possible) */
     CAPDB_INTEGER,  /* 0x04 INTEGER */
     CAPDB_NULL,     /* 0x05 (not possible) */
     CAPDB_INTEGER,  /* 0x06 INTEGER + TEXT */
     CAPDB_NULL,     /* 0x07 (not possible) */
     CAPDB_FLOAT,    /* 0x08 FLOAT */
     CAPDB_NULL,     /* 0x09 (not possible) */
     CAPDB_FLOAT,    /* 0x0a FLOAT + TEXT */
     CAPDB_NULL,     /* 0x0b (not possible) */
     CAPDB_INTEGER,  /* 0x0c (not possible) */
     CAPDB_NULL,     /* 0x0d (not possible) */
     CAPDB_INTEGER,  /* 0x0e (not possible) */
     CAPDB_NULL,     /* 0x0f (not possible) */
     CAPDB_BLOB,     /* 0x10 BLOB */
     CAPDB_NULL,     /* 0x11 (not possible) */
     CAPDB_TEXT,     /* 0x12 (not possible) */
     CAPDB_NULL,     /* 0x13 (not possible) */
     CAPDB_INTEGER,  /* 0x14 INTEGER + BLOB */
     CAPDB_NULL,     /* 0x15 (not possible) */
     CAPDB_INTEGER,  /* 0x16 (not possible) */
     CAPDB_NULL,     /* 0x17 (not possible) */
     CAPDB_FLOAT,    /* 0x18 FLOAT + BLOB */
     CAPDB_NULL,     /* 0x19 (not possible) */
     CAPDB_FLOAT,    /* 0x1a (not possible) */
     CAPDB_NULL,     /* 0x1b (not possible) */
     CAPDB_INTEGER,  /* 0x1c (not possible) */
     CAPDB_NULL,     /* 0x1d (not possible) */
     CAPDB_INTEGER,  /* 0x1e (not possible) */
     CAPDB_NULL,     /* 0x1f (not possible) */
     CAPDB_FLOAT,    /* 0x20 INTREAL */
     CAPDB_NULL,     /* 0x21 (not possible) */
     CAPDB_FLOAT,    /* 0x22 INTREAL + TEXT */
     CAPDB_NULL,     /* 0x23 (not possible) */
     CAPDB_FLOAT,    /* 0x24 (not possible) */
     CAPDB_NULL,     /* 0x25 (not possible) */
     CAPDB_FLOAT,    /* 0x26 (not possible) */
     CAPDB_NULL,     /* 0x27 (not possible) */
     CAPDB_FLOAT,    /* 0x28 (not possible) */
     CAPDB_NULL,     /* 0x29 (not possible) */
     CAPDB_FLOAT,    /* 0x2a (not possible) */
     CAPDB_NULL,     /* 0x2b (not possible) */
     CAPDB_FLOAT,    /* 0x2c (not possible) */
     CAPDB_NULL,     /* 0x2d (not possible) */
     CAPDB_FLOAT,    /* 0x2e (not possible) */
     CAPDB_NULL,     /* 0x2f (not possible) */
     CAPDB_BLOB,     /* 0x30 (not possible) */
     CAPDB_NULL,     /* 0x31 (not possible) */
     CAPDB_TEXT,     /* 0x32 (not possible) */
     CAPDB_NULL,     /* 0x33 (not possible) */
     CAPDB_FLOAT,    /* 0x34 (not possible) */
     CAPDB_NULL,     /* 0x35 (not possible) */
     CAPDB_FLOAT,    /* 0x36 (not possible) */
     CAPDB_NULL,     /* 0x37 (not possible) */
     CAPDB_FLOAT,    /* 0x38 (not possible) */
     CAPDB_NULL,     /* 0x39 (not possible) */
     CAPDB_FLOAT,    /* 0x3a (not possible) */
     CAPDB_NULL,     /* 0x3b (not possible) */
     CAPDB_FLOAT,    /* 0x3c (not possible) */
     CAPDB_NULL,     /* 0x3d (not possible) */
     CAPDB_FLOAT,    /* 0x3e (not possible) */
     CAPDB_NULL,     /* 0x3f (not possible) */
  };
#ifdef CAPDB_DEBUG
  {
    int eType = CAPDB_BLOB;
    if( pVal->flags & MEM_Null ){
      eType = CAPDB_NULL;
    }else if( pVal->flags & (MEM_Real|MEM_IntReal) ){
      eType = CAPDB_FLOAT;
    }else if( pVal->flags & MEM_Int ){
      eType = CAPDB_INTEGER;
    }else if( pVal->flags & MEM_Str ){
      eType = CAPDB_TEXT;
    }
    assert( eType == aType[pVal->flags&MEM_AffMask] );
  }
#endif
  return aType[pVal->flags&MEM_AffMask];
}
int capdb_value_encoding(capdb_value *pVal){
  return pVal->enc;
}

/* Return true if a parameter to xUpdate represents an unchanged column */
int capdb_value_nochange(capdb_value *pVal){
  return (pVal->flags&(MEM_Null|MEM_Zero))==(MEM_Null|MEM_Zero);
}

/* Return true if a parameter value originated from an capdb_bind() */
int capdb_value_frombind(capdb_value *pVal){
  return (pVal->flags&MEM_FromBind)!=0;
}

/* Make a copy of an capdb_value object
*/
capdb_value *capdb_value_dup(const capdb_value *pOrig){
  capdb_value *pNew;
  if( pOrig==0 ) return 0;
  pNew = capdb_malloc( sizeof(*pNew) );
  if( pNew==0 ) return 0;
  memset(pNew, 0, sizeof(*pNew));
  memcpy(pNew, pOrig, MEMCELLSIZE);
  pNew->flags &= ~MEM_Dyn;
  pNew->db = 0;
  if( pNew->flags&(MEM_Str|MEM_Blob) ){
    pNew->flags &= ~(MEM_Static|MEM_Dyn);
    pNew->flags |= MEM_Ephem;
    if( capdbVdbeMemMakeWriteable(pNew)!=CAPDB_OK ){
      capdbValueFree(pNew);
      pNew = 0;
    }
  }else if( pNew->flags & MEM_Null ){
    /* Do not duplicate pointer values */
    pNew->flags &= ~(MEM_Term|MEM_Subtype);
  }
  return pNew;
}

/* Destroy an capdb_value object previously obtained from
** capdb_value_dup().
*/
void capdb_value_free(capdb_value *pOld){
  capdbValueFree(pOld);
}
 

/**************************** capdb_result_  *******************************
** The following routines are used by user-defined functions to specify
** the function result.
**
** The setStrOrError() function calls capdbVdbeMemSetStr() to store the
** result as a string or blob.  Appropriate errors are set if the string/blob
** is too big or if an OOM occurs.
**
** The invokeValueDestructor(P,X) routine invokes destructor function X()
** on value P if P is not going to be used and need to be destroyed.
*/
static void setResultStrOrError(
  capdb_context *pCtx,  /* Function context */
  const char *z,          /* String pointer */
  int n,                  /* Bytes in string, or negative */
  u8 enc,                 /* Encoding of z.  0 for BLOBs */
  void (*xDel)(void*)     /* Destructor function */
){
  Mem *pOut = pCtx->pOut;
  int rc;
  if( enc==CAPDB_UTF8 ){
    rc = capdbVdbeMemSetText(pOut, z, n, xDel);
  }else if( enc==CAPDB_UTF8_ZT ){
    /* It is usually considered improper to assert() on an input. However,
    ** the following assert() is checking for inputs that are documented
    ** to result in undefined behavior. */
    assert( z==0
         || n<0 
         || n>pOut->db->aLimit[CAPDB_LIMIT_LENGTH]
         || z[n]==0
    );
    rc = capdbVdbeMemSetText(pOut, z, n, xDel);
    pOut->flags |= MEM_Term;
  }else{
    rc = capdbVdbeMemSetStr(pOut, z, n, enc, xDel);
  }
  if( rc ){
    if( rc==CAPDB_TOOBIG ){
      capdb_result_error_toobig(pCtx);
    }else{
      /* The only errors possible from capdbVdbeMemSetStr are
      ** CAPDB_TOOBIG and CAPDB_NOMEM */
      assert( rc==CAPDB_NOMEM );
      capdb_result_error_nomem(pCtx);
    }
    return;
  }
  capdbVdbeChangeEncoding(pOut, pCtx->enc);
  if( capdbVdbeMemTooBig(pOut) ){
    capdb_result_error_toobig(pCtx);
  }
}
static int invokeValueDestructor(
  const void *p,             /* Value to destroy */
  void (*xDel)(void*),       /* The destructor */
  capdb_context *pCtx      /* Set a CAPDB_TOOBIG error if not NULL */
){
  assert( xDel!=CAPDB_DYNAMIC );
  if( xDel==0 ){
    /* noop */
  }else if( xDel==CAPDB_TRANSIENT ){
    /* noop */
  }else{
    xDel((void*)p);
  }
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx!=0 ){
    capdb_result_error_toobig(pCtx);
  }
#else
  assert( pCtx!=0 );
  capdb_result_error_toobig(pCtx);
#endif
  return CAPDB_TOOBIG;
}
void capdb_result_blob(
  capdb_context *pCtx,
  const void *z,
  int n,
  void (*xDel)(void *)
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 || n<0 ){
    invokeValueDestructor(z, xDel, pCtx);
    return;
  }
#endif
  assert( n>=0 );
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  setResultStrOrError(pCtx, z, n, 0, xDel);
}
void capdb_result_blob64(
  capdb_context *pCtx,
  const void *z,
  capdb_uint64 n,
  void (*xDel)(void *)
){
  assert( xDel!=CAPDB_DYNAMIC );
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ){
    invokeValueDestructor(z, xDel, 0);
    return;
  }
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  if( n>0x7fffffff ){
    (void)invokeValueDestructor(z, xDel, pCtx);
  }else{
    setResultStrOrError(pCtx, z, (int)n, 0, xDel);
  }
}
void capdb_result_double(capdb_context *pCtx, double rVal){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  capdbVdbeMemSetDouble(pCtx->pOut, rVal);
}
void capdb_result_error(capdb_context *pCtx, const char *z, int n){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  pCtx->isError = CAPDB_ERROR;
  capdbVdbeMemSetStr(pCtx->pOut, z, n, CAPDB_UTF8, CAPDB_TRANSIENT);
}
#ifndef CAPDB_OMIT_UTF16
void capdb_result_error16(capdb_context *pCtx, const void *z, int n){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  pCtx->isError = CAPDB_ERROR;
  capdbVdbeMemSetStr(pCtx->pOut, z, n, CAPDB_UTF16NATIVE, CAPDB_TRANSIENT);
}
#endif
void capdb_result_int(capdb_context *pCtx, int iVal){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  capdbVdbeMemSetInt64(pCtx->pOut, (i64)iVal);
}
void capdb_result_int64(capdb_context *pCtx, i64 iVal){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  capdbVdbeMemSetInt64(pCtx->pOut, iVal);
}
void capdb_result_null(capdb_context *pCtx){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  capdbVdbeMemSetNull(pCtx->pOut);
}
void capdb_result_pointer(
  capdb_context *pCtx,
  void *pPtr,
  const char *zPType,
  void (*xDestructor)(void*)
){
  Mem *pOut;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ){
    invokeValueDestructor(pPtr, xDestructor, 0);
    return;
  }
#endif
  pOut = pCtx->pOut;
  assert( capdb_mutex_held(pOut->db->mutex) );
  capdbVdbeMemRelease(pOut);
  pOut->flags = MEM_Null;
  capdbVdbeMemSetPointer(pOut, pPtr, zPType, xDestructor);
}
void capdb_result_subtype(capdb_context *pCtx, unsigned int eSubtype){
  Mem *pOut;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
#if defined(CAPDB_STRICT_SUBTYPE) && CAPDB_STRICT_SUBTYPE+0!=0
  if( pCtx->pFunc!=0
   && (pCtx->pFunc->funcFlags & CAPDB_RESULT_SUBTYPE)==0
  ){
    char zErr[200];
    capdb_snprintf(sizeof(zErr), zErr,
                     "misuse of capdb_result_subtype() by %s()", 
                     pCtx->pFunc->zName);
    capdb_result_error(pCtx, zErr, -1);
    return;
  }
#endif /* CAPDB_STRICT_SUBTYPE */
  pOut = pCtx->pOut;
  assert( capdb_mutex_held(pOut->db->mutex) );
  pOut->eSubtype = eSubtype & 0xff;
  pOut->flags |= MEM_Subtype;
}
void capdb_result_text(
  capdb_context *pCtx,
  const char *z,
  int n,
  void (*xDel)(void *)
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ){
    invokeValueDestructor(z, xDel, 0);
    return;
  }
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  setResultStrOrError(pCtx, z, n, CAPDB_UTF8, xDel);
}
void capdb_result_text64(
  capdb_context *pCtx,
  const char *z,
  capdb_uint64 n,
  void (*xDel)(void *),
  unsigned char enc
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ){
    invokeValueDestructor(z, xDel, 0);
    return;
  }
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  assert( xDel!=CAPDB_DYNAMIC );
  if( enc!=CAPDB_UTF8 && enc!=CAPDB_UTF8_ZT ){
    if( enc==CAPDB_UTF16 ) enc = CAPDB_UTF16NATIVE;
    n &= ~(u64)1;
  }
  if( n>0x7fffffff ){
    (void)invokeValueDestructor(z, xDel, pCtx);
  }else{
    setResultStrOrError(pCtx, z, (int)n, enc, xDel);
    capdbVdbeMemZeroTerminateIfAble(pCtx->pOut);
  }
}
#ifndef CAPDB_OMIT_UTF16
void capdb_result_text16(
  capdb_context *pCtx,
  const void *z,
  int n,
  void (*xDel)(void *)
){
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  setResultStrOrError(pCtx, z, n & ~(u64)1, CAPDB_UTF16NATIVE, xDel);
}
void capdb_result_text16be(
  capdb_context *pCtx,
  const void *z,
  int n,
  void (*xDel)(void *)
){
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  setResultStrOrError(pCtx, z, n & ~(u64)1, CAPDB_UTF16BE, xDel);
}
void capdb_result_text16le(
  capdb_context *pCtx,
  const void *z,
  int n,
  void (*xDel)(void *)
){
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  setResultStrOrError(pCtx, z, n & ~(u64)1, CAPDB_UTF16LE, xDel);
}
#endif /* CAPDB_OMIT_UTF16 */
void capdb_result_value(capdb_context *pCtx, capdb_value *pValue){
  Mem *pOut;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
  if( pValue==0 ){
    capdb_result_null(pCtx);
    return;
  }
#endif
  pOut = pCtx->pOut;
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  capdbVdbeMemCopy(pOut, pValue);
  capdbVdbeChangeEncoding(pOut, pCtx->enc);
  if( capdbVdbeMemTooBig(pOut) ){
    capdb_result_error_toobig(pCtx);
  }
}
void capdb_result_zeroblob(capdb_context *pCtx, int n){
  capdb_result_zeroblob64(pCtx, n>0 ? n : 0);
}
int capdb_result_zeroblob64(capdb_context *pCtx, u64 n){
  Mem *pOut;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return CAPDB_MISUSE_BKPT;
#endif
  pOut = pCtx->pOut;
  assert( capdb_mutex_held(pOut->db->mutex) );
  if( n>(u64)pOut->db->aLimit[CAPDB_LIMIT_LENGTH] ){
    capdb_result_error_toobig(pCtx);
    return CAPDB_TOOBIG;
  }
#ifndef CAPDB_OMIT_INCRBLOB
  capdbVdbeMemSetZeroBlob(pCtx->pOut, (int)n);
  return CAPDB_OK;
#else
  return capdbVdbeMemSetZeroBlob(pCtx->pOut, (int)n);
#endif
}
void capdb_result_error_code(capdb_context *pCtx, int errCode){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  pCtx->isError = errCode ? errCode : -1;
#ifdef CAPDB_DEBUG
  if( pCtx->pVdbe ) pCtx->pVdbe->rcApp = errCode;
#endif
  if( pCtx->pOut->flags & MEM_Null ){
    setResultStrOrError(pCtx, capdbErrStr(errCode), -1, CAPDB_UTF8,
                        CAPDB_STATIC);
  }
}

/* Force an CAPDB_TOOBIG error. */
void capdb_result_error_toobig(capdb_context *pCtx){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  pCtx->isError = CAPDB_TOOBIG;
  capdbVdbeMemSetStr(pCtx->pOut, "string or blob too big", -1,
                       CAPDB_UTF8, CAPDB_STATIC);
}

/* An CAPDB_NOMEM error. */
void capdb_result_error_nomem(capdb_context *pCtx){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  capdbVdbeMemSetNull(pCtx->pOut);
  pCtx->isError = CAPDB_NOMEM_BKPT;
  capdbOomFault(pCtx->pOut->db);
}

#ifndef CAPDB_UNTESTABLE
/* Force the INT64 value currently stored as the result to be
** a MEM_IntReal value.  See the CAPDB_TESTCTRL_RESULT_INTREAL
** test-control.
*/
void capdbResultIntReal(capdb_context *pCtx){
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
  if( pCtx->pOut->flags & MEM_Int ){
    pCtx->pOut->flags &= ~MEM_Int;
    pCtx->pOut->flags |= MEM_IntReal;
  }
}
#endif


/*
** This function is called after a transaction has been committed. It
** invokes callbacks registered with capdb_wal_hook() as required.
*/
static int doWalCallbacks(capdb *db){
  int rc = CAPDB_OK;
#ifndef CAPDB_OMIT_WAL
  int i;
  for(i=0; i<db->nDb; i++){
    Btree *pBt = db->aDb[i].pBt;
    if( pBt ){
      int nEntry;
      capdbBtreeEnter(pBt);
      nEntry = capdbPagerWalCallback(capdbBtreePager(pBt));
      capdbBtreeLeave(pBt);
      if( nEntry>0 && db->xWalCallback && rc==CAPDB_OK ){
        rc = db->xWalCallback(db->pWalArg, db, db->aDb[i].zDbSName, nEntry);
      }
    }
  }
#else
  UNUSED_PARAMETER(db);
#endif
  return rc;
}


/*
** Execute the statement pStmt, either until a row of data is ready, the
** statement is completely executed or an error occurs.
**
** This routine implements the bulk of the logic behind the sqlite_step()
** API.  The only thing omitted is the automatic recompile if a
** schema change has occurred.  That detail is handled by the
** outer capdb_step() wrapper procedure.
*/
static int capdbStep(Vdbe *p){
  capdb *db;
  int rc;

  assert(p);
  db = p->db;
  if( p->eVdbeState!=VDBE_RUN_STATE ){
    restart_step:
    if( p->eVdbeState==VDBE_READY_STATE ){
      if( p->expired ){
        p->rc = CAPDB_SCHEMA;
        rc = CAPDB_ERROR;
        if( (p->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 ){
          /* If this statement was prepared using saved SQL and an
          ** error has occurred, then return the error code in p->rc to the
          ** caller. Set the error code in the database handle to the same
          ** value.
          */
          rc = capdbVdbeTransferError(p);
        }
        goto end_of_step;
      }

      /* If there are no other statements currently running, then
      ** reset the interrupt flag.  This prevents a call to capdb_interrupt
      ** from interrupting a statement that has not yet started.
      */
      if( db->nVdbeActive==0 ){
        AtomicStore(&db->u1.isInterrupted, 0);
      }

      assert( db->nVdbeWrite>0 || db->autoCommit==0
          || ((db->nDeferredCons + db->nDeferredImmCons)==0)
      );

#ifndef CAPDB_OMIT_TRACE
      if( (db->mTrace & (CAPDB_TRACE_PROFILE|CAPDB_TRACE_XPROFILE))!=0
          && !db->init.busy && p->zSql ){
        capdbOsCurrentTimeInt64(db->pVfs, &p->startTime);
      }else{
        assert( p->startTime==0 );
      }
#endif

      db->nVdbeActive++;
      if( p->readOnly==0 ) db->nVdbeWrite++;
      if( p->bIsReader ) db->nVdbeRead++;
      p->pc = 0;
      p->eVdbeState = VDBE_RUN_STATE;
    }else

    if( ALWAYS(p->eVdbeState==VDBE_HALT_STATE) ){
      /* We used to require that capdb_reset() be called before retrying
      ** capdb_step() after any error or after CAPDB_DONE.  But beginning
      ** with version 3.7.0, we changed this so that capdb_reset() would
      ** be called automatically instead of throwing the CAPDB_MISUSE error.
      ** This "automatic-reset" change is not technically an incompatibility,
      ** since any application that receives an CAPDB_MISUSE is broken by
      ** definition.
      **
      ** Nevertheless, some published applications that were originally written
      ** for version 3.6.23 or earlier do in fact depend on CAPDB_MISUSE
      ** returns, and those were broken by the automatic-reset change.  As a
      ** a work-around, the CAPDB_OMIT_AUTORESET compile-time restores the
      ** legacy behavior of returning CAPDB_MISUSE for cases where the
      ** previous capdb_step() returned something other than a CAPDB_LOCKED
      ** or CAPDB_BUSY error.
      */
#ifdef CAPDB_OMIT_AUTORESET
      if( (rc = p->rc&0xff)==CAPDB_BUSY || rc==CAPDB_LOCKED ){
        capdb_reset((capdb_stmt*)p);
      }else{
        return CAPDB_MISUSE_BKPT;
      }
#else
      capdb_reset((capdb_stmt*)p);
#endif
      assert( p->eVdbeState==VDBE_READY_STATE );
      goto restart_step;
    }
  }

#ifdef CAPDB_DEBUG
  p->rcApp = CAPDB_OK;
#endif
#ifndef CAPDB_OMIT_EXPLAIN
  if( p->explain ){
    rc = capdbVdbeList(p);
  }else
#endif /* CAPDB_OMIT_EXPLAIN */
  {
    db->nVdbeExec++;
    rc = capdbVdbeExec(p);
    db->nVdbeExec--;
  }

  if( rc==CAPDB_ROW ){
    assert( p->rc==CAPDB_OK );
    assert( db->mallocFailed==0 );
    db->errCode = CAPDB_ROW;
    return CAPDB_ROW;
  }else{
#ifndef CAPDB_OMIT_TRACE
    /* If the statement completed successfully, invoke the profile callback */
    checkProfileCallback(db, p);
#endif
    p->pResultRow = 0;
    if( rc==CAPDB_DONE && db->autoCommit ){
      assert( p->rc==CAPDB_OK );
      p->rc = doWalCallbacks(db);
      if( p->rc!=CAPDB_OK ){
        rc = CAPDB_ERROR;
      }
    }else if( rc!=CAPDB_DONE && (p->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 ){
      /* If this statement was prepared using saved SQL and an
      ** error has occurred, then return the error code in p->rc to the
      ** caller. Set the error code in the database handle to the same value.
      */
      rc = capdbVdbeTransferError(p);
    }
  }

  db->errCode = rc;
  if( CAPDB_NOMEM==capdbApiExit(p->db, p->rc) ){
    p->rc = CAPDB_NOMEM_BKPT;
    if( (p->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 ) rc = p->rc;
  }
end_of_step:
  /* There are only a limited number of result codes allowed from the
  ** statements prepared using the legacy capdb_prepare() interface */
  assert( (p->prepFlags & CAPDB_PREPARE_SAVESQL)!=0
       || rc==CAPDB_ROW  || rc==CAPDB_DONE   || rc==CAPDB_ERROR
       || (rc&0xff)==CAPDB_BUSY || rc==CAPDB_MISUSE
  );
  return (rc&db->errMask);
}

/*
** This is the top-level implementation of capdb_step().  Call
** capdbStep() to do most of the work.  If a schema error occurs,
** call capdbReprepare() and try again.
*/
int capdb_step(capdb_stmt *pStmt){
  int rc = CAPDB_OK;      /* Result from capdbStep() */
  Vdbe *v = (Vdbe*)pStmt;  /* the prepared statement */
  int cnt = 0;             /* Counter to prevent infinite loop of reprepares */
  capdb *db;             /* The database connection */

  if( vdbeSafetyNotNull(v) ){
    return CAPDB_MISUSE_BKPT;
  }
  db = v->db;
  capdb_mutex_enter(db->mutex);
  while( (rc = capdbStep(v))==CAPDB_SCHEMA
         && cnt++ < CAPDB_MAX_SCHEMA_RETRY ){
    int savedPc = v->pc;
    rc = capdbReprepare(v);
    if( rc!=CAPDB_OK ){
      /* This case occurs after failing to recompile an sql statement.
      ** The error message from the SQL compiler has already been loaded
      ** into the database handle. This block copies the error message
      ** from the database handle into the statement and sets the statement
      ** program counter to 0 to ensure that when the statement is
      ** finalized or reset the parser error message is available via
      ** capdb_errmsg() and capdb_errcode().
      */
      const char *zErr = (const char *)capdb_value_text(db->pErr);
      capdbDbFree(db, v->zErrMsg);
      if( !db->mallocFailed ){
        v->zErrMsg = capdbDbStrDup(db, zErr);
        v->rc = rc = capdbApiExit(db, rc);
      } else {
        v->zErrMsg = 0;
        v->rc = rc = CAPDB_NOMEM_BKPT;
      }
      break;
    }
    capdb_reset(pStmt);
    if( savedPc>=0 ){
      /* Setting minWriteFileFormat to 254 is a signal to the OP_Init and
      ** OP_Trace opcodes to *not* perform CAPDB_TRACE_STMT because it has
      ** already been done once on a prior invocation that failed due to
      ** CAPDB_SCHEMA.   tag-20220401a  */
      v->minWriteFileFormat = 254;
    }
    assert( v->expired==0 );
  }
  capdb_mutex_leave(db->mutex);
  return rc;
}


/*
** Extract the user data from a capdb_context structure and return a
** pointer to it.
*/
void *capdb_user_data(capdb_context *p){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#endif
  assert( p && p->pFunc );
  return p->pFunc->pUserData;
}

/*
** Extract the user data from a capdb_context structure and return a
** pointer to it.
**
** IMPLEMENTATION-OF: R-46798-50301 The capdb_context_db_handle() interface
** returns a copy of the pointer to the database connection (the 1st
** parameter) of the capdb_create_function() and
** capdb_create_function16() routines that originally registered the
** application defined function.
*/
capdb *capdb_context_db_handle(capdb_context *p){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#else
  assert( p && p->pOut );
#endif
  return p->pOut->db;
}

/*
** If this routine is invoked from within an xColumn method of a virtual
** table, then it returns true if and only if the the call is during an
** UPDATE operation and the value of the column will not be modified
** by the UPDATE.
**
** If this routine is called from any context other than within the
** xColumn method of a virtual table, then the return value is meaningless
** and arbitrary.
**
** Virtual table implements might use this routine to optimize their
** performance by substituting a NULL result, or some other light-weight
** value, as a signal to the xUpdate routine that the column is unchanged.
*/
int capdb_vtab_nochange(capdb_context *p){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#else
  assert( p );
#endif
  return capdb_value_nochange(p->pOut);
}

/*
** The destructor function for a ValueList object.  This needs to be
** a separate function, unknowable to the application, to ensure that
** calls to capdb_vtab_in_first()/capdb_vtab_in_next() that are not
** preceded by activation of IN processing via capdb_vtab_int() do not
** try to access a fake ValueList object inserted by a hostile extension.
*/
void capdbVdbeValueListFree(void *pToDelete){
  capdb_free(pToDelete);
}

/*
** Implementation of capdb_vtab_in_first() (if bNext==0) and
** capdb_vtab_in_next() (if bNext!=0).
*/
static int valueFromValueList(
  capdb_value *pVal,        /* Pointer to the ValueList object */
  capdb_value **ppOut,      /* Store the next value from the list here */
  int bNext                   /* 1 for _next(). 0 for _first() */
){
  int rc;
  ValueList *pRhs;

  *ppOut = 0;
  if( pVal==0 ) return CAPDB_MISUSE_BKPT;
  if( (pVal->flags & MEM_Dyn)==0 || pVal->xDel!=capdbVdbeValueListFree ){
    return CAPDB_ERROR;
  }else{
    assert( (pVal->flags&(MEM_TypeMask|MEM_Term|MEM_Subtype)) ==
                 (MEM_Null|MEM_Term|MEM_Subtype) );
    assert( pVal->eSubtype=='p' );
    assert( pVal->u.zPType!=0 && strcmp(pVal->u.zPType,"ValueList")==0 );
    pRhs = (ValueList*)pVal->z;
  }
  if( bNext ){
    rc = capdbBtreeNext(pRhs->pCsr, 0);
  }else{
    int dummy = 0;
    rc = capdbBtreeFirst(pRhs->pCsr, &dummy);
    assert( rc==CAPDB_OK || capdbBtreeEof(pRhs->pCsr) );
    if( capdbBtreeEof(pRhs->pCsr) ) rc = CAPDB_DONE;
  }
  if( rc==CAPDB_OK ){
    u32 sz;       /* Size of current row in bytes */
    Mem sMem;     /* Raw content of current row */
    memset(&sMem, 0, sizeof(sMem));
    sz = capdbBtreePayloadSize(pRhs->pCsr);
    rc = capdbVdbeMemFromBtreeZeroOffset(pRhs->pCsr,sz,&sMem);
    if( rc==CAPDB_OK ){
      u8 *zBuf = (u8*)sMem.z;
      u32 iSerial;
      capdb_value *pOut = pRhs->pOut;
      int iOff = 1 + getVarint32(&zBuf[1], iSerial);
      capdbVdbeSerialGet(&zBuf[iOff], iSerial, pOut);
      pOut->enc = ENC(pOut->db);
      if( (pOut->flags & MEM_Ephem)!=0 && capdbVdbeMemMakeWriteable(pOut) ){
        rc = CAPDB_NOMEM;
      }else{
        *ppOut = pOut;
      }
    }
    capdbVdbeMemRelease(&sMem);
  }
  return rc;
}

/*
** Set the iterator value pVal to point to the first value in the set.
** Set (*ppOut) to point to this value before returning.
*/
int capdb_vtab_in_first(capdb_value *pVal, capdb_value **ppOut){
  return valueFromValueList(pVal, ppOut, 0);
}

/*
** Set the iterator value pVal to point to the next value in the set.
** Set (*ppOut) to point to this value before returning.
*/
int capdb_vtab_in_next(capdb_value *pVal, capdb_value **ppOut){
  return valueFromValueList(pVal, ppOut, 1);
}

/*
** Return the current time for a statement.  If the current time
** is requested more than once within the same run of a single prepared
** statement, the exact same time is returned for each invocation regardless
** of the amount of time that elapses between invocations.  In other words,
** the time returned is always the time of the first call.
*/
capdb_int64 capdbStmtCurrentTime(capdb_context *p){
  int rc;
#ifndef CAPDB_ENABLE_STAT4
  capdb_int64 *piTime = &p->pVdbe->iCurrentTime;
  assert( p->pVdbe!=0 );
#else
  capdb_int64 iTime = 0;
  capdb_int64 *piTime = p->pVdbe!=0 ? &p->pVdbe->iCurrentTime : &iTime;
#endif
  if( *piTime==0 ){
    rc = capdbOsCurrentTimeInt64(p->pOut->db->pVfs, piTime);
    if( rc ) *piTime = 0;
  }
  return *piTime;
}

/*
** Create a new aggregate context for p and return a pointer to
** its pMem->z element.
*/
static CAPDB_NOINLINE void *createAggContext(capdb_context *p, int nByte){
  Mem *pMem = p->pMem;
  assert( (pMem->flags & MEM_Agg)==0 );
  if( nByte<=0 ){
    capdbVdbeMemSetNull(pMem);
    pMem->z = 0;
  }else{
    capdbVdbeMemClearAndResize(pMem, nByte);
    pMem->flags = MEM_Agg;
    pMem->u.pDef = p->pFunc;
    if( pMem->z ){
      memset(pMem->z, 0, nByte);
    }
  }
  return (void*)pMem->z;
}

/*
** Allocate or return the aggregate context for a user function.  A new
** context is allocated on the first call.  Subsequent calls return the
** same context that was returned on prior calls.
*/
void *capdb_aggregate_context(capdb_context *p, int nByte){
  assert( p && p->pFunc && p->pFunc->xFinalize );
  assert( capdb_mutex_held(p->pOut->db->mutex) );
  testcase( nByte<0 );
  if( (p->pMem->flags & MEM_Agg)==0 ){
    return createAggContext(p, nByte);
  }else{
    return (void*)p->pMem->z;
  }
}

/*
** Return the auxiliary data pointer, if any, for the iArg'th argument to
** the user-function defined by pCtx.
**
** The left-most argument is 0.
**
** Undocumented behavior:  If iArg is negative then access a cache of
** auxiliary data pointers that is available to all functions within a
** single prepared statement.  The iArg values must match.
*/
void *capdb_get_auxdata(capdb_context *pCtx, int iArg){
  AuxData *pAuxData;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return 0;
#endif
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
#if CAPDB_ENABLE_STAT4
  if( pCtx->pVdbe==0 ) return 0;
#else
  assert( pCtx->pVdbe!=0 );
#endif
  for(pAuxData=pCtx->pVdbe->pAuxData; pAuxData; pAuxData=pAuxData->pNextAux){
    if(  pAuxData->iAuxArg==iArg && (pAuxData->iAuxOp==pCtx->iOp || iArg<0) ){
      return pAuxData->pAux;
    }
  }
  return 0;
}

/*
** Set the auxiliary data pointer and delete function, for the iArg'th
** argument to the user-function defined by pCtx. Any previous value is
** deleted by calling the delete function specified when it was set.
**
** The left-most argument is 0.
**
** Undocumented behavior:  If iArg is negative then make the data available
** to all functions within the current prepared statement using iArg as an
** access code.
*/
void capdb_set_auxdata(
  capdb_context *pCtx,
  int iArg,
  void *pAux,
  void (*xDelete)(void*)
){
  AuxData *pAuxData;
  Vdbe *pVdbe;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( pCtx==0 ) return;
#endif
  pVdbe= pCtx->pVdbe;
  assert( capdb_mutex_held(pCtx->pOut->db->mutex) );
#ifdef CAPDB_ENABLE_STAT4
  if( pVdbe==0 ) goto failed;
#else
  assert( pVdbe!=0 );
#endif

  for(pAuxData=pVdbe->pAuxData; pAuxData; pAuxData=pAuxData->pNextAux){
    if( pAuxData->iAuxArg==iArg && (pAuxData->iAuxOp==pCtx->iOp || iArg<0) ){
      break;
    }
  }
  if( pAuxData==0 ){
    pAuxData = capdbDbMallocZero(pVdbe->db, sizeof(AuxData));
    if( !pAuxData ) goto failed;
    pAuxData->iAuxOp = pCtx->iOp;
    pAuxData->iAuxArg = iArg;
    pAuxData->pNextAux = pVdbe->pAuxData;
    pVdbe->pAuxData = pAuxData;
    if( pCtx->isError==0 ) pCtx->isError = -1;
  }else if( pAuxData->xDeleteAux ){
    pAuxData->xDeleteAux(pAuxData->pAux);
  }

  pAuxData->pAux = pAux;
  pAuxData->xDeleteAux = xDelete;
  return;

failed:
  if( xDelete ){
    xDelete(pAux);
  }
}

#ifndef CAPDB_OMIT_DEPRECATED
/*
** Return the number of times the Step function of an aggregate has been
** called.
**
** This function is deprecated.  Do not use it for new code.  It is
** provide only to avoid breaking legacy code.  New aggregate function
** implementations should keep their own counts within their aggregate
** context.
*/
int capdb_aggregate_count(capdb_context *p){
  assert( p && p->pMem && p->pFunc && p->pFunc->xFinalize );
  return p->pMem->n;
}
#endif

/*
** Return the number of columns in the result set for the statement pStmt.
*/
int capdb_column_count(capdb_stmt *pStmt){
  Vdbe *pVm = (Vdbe *)pStmt;
  if( pVm==0 ) return 0;
  return pVm->nResColumn;
}

/*
** Return the number of values available from the current row of the
** currently executing statement pStmt.
*/
int capdb_data_count(capdb_stmt *pStmt){
  Vdbe *pVm = (Vdbe *)pStmt;
  if( pVm==0 || pVm->pResultRow==0 ) return 0;
  return pVm->nResColumn;
}

/*
** Return a pointer to static memory containing an SQL NULL value.
*/
static const Mem *columnNullValue(void){
  /* Even though the Mem structure contains an element
  ** of type i64, on certain architectures (x86) with certain compiler
  ** switches (-Os), gcc may align this Mem object on a 4-byte boundary
  ** instead of an 8-byte one. This all works fine, except that when
  ** running with CAPDB_DEBUG defined the SQLite code sometimes assert()s
  ** that a Mem structure is located on an 8-byte boundary. To prevent
  ** these assert()s from failing, when building with CAPDB_DEBUG defined
  ** using gcc, we force nullMem to be 8-byte aligned using the magical
  ** __attribute__((aligned(8))) macro.  */
  static const Mem nullMem
#if defined(CAPDB_DEBUG) && defined(__GNUC__)
    __attribute__((aligned(8)))
#endif
    = {
        /* .u          = */ {0},
        /* .z          = */ (char*)0,
        /* .n          = */ (int)0,
        /* .flags      = */ (u16)MEM_Null,
        /* .enc        = */ (u8)0,
        /* .eSubtype   = */ (u8)0,
        /* .db         = */ (capdb*)0,
        /* .szMalloc   = */ (int)0,
        /* .uTemp      = */ (u32)0,
        /* .zMalloc    = */ (char*)0,
        /* .xDel       = */ (void(*)(void*))0,
#ifdef CAPDB_DEBUG
        /* .pScopyFrom = */ (Mem*)0,
        /* .mScopyFlags= */ 0,
        /* .bScopy     = */ 0,
#endif
      };
  return &nullMem;
}

/*
** Check to see if column iCol of the given statement is valid.  If
** it is, return a pointer to the Mem for the value of that column.
** If iCol is not valid, return a pointer to a Mem which has a value
** of NULL.
*/
static Mem *columnMem(capdb_stmt *pStmt, int i){
  Vdbe *pVm;
  Mem *pOut;

  pVm = (Vdbe *)pStmt;
  if( pVm==0 ) return (Mem*)columnNullValue();
  assert( pVm->db );
  capdb_mutex_enter(pVm->db->mutex);
  if( pVm->pResultRow!=0 && i<pVm->nResColumn && i>=0 ){
    pOut = &pVm->pResultRow[i];
  }else{
    capdbError(pVm->db, CAPDB_RANGE);
    pOut = (Mem*)columnNullValue();
  }
  return pOut;
}

/*
** This function is called after invoking an capdb_value_XXX function on a
** column value (i.e. a value returned by evaluating an SQL expression in the
** select list of a SELECT statement) that may cause a malloc() failure. If
** malloc() has failed, the threads mallocFailed flag is cleared and the result
** code of statement pStmt set to CAPDB_NOMEM.
**
** Specifically, this is called from within:
**
**     capdb_column_int()
**     capdb_column_int64()
**     capdb_column_text()
**     capdb_column_text16()
**     capdb_column_double()
**     capdb_column_bytes()
**     capdb_column_bytes16()
**     capdb_column_blob()
*/
static void columnMallocFailure(capdb_stmt *pStmt)
{
  /* If malloc() failed during an encoding conversion within an
  ** capdb_column_XXX API, then set the return code of the statement to
  ** CAPDB_NOMEM. The next call to _step() (if any) will return CAPDB_ERROR
  ** and _finalize() will return NOMEM.
  */
  Vdbe *p = (Vdbe *)pStmt;
  if( p ){
    assert( p->db!=0 );
    assert( capdb_mutex_held(p->db->mutex) );
    p->rc = capdbApiExit(p->db, p->rc);
    capdb_mutex_leave(p->db->mutex);
  }
}

/**************************** capdb_column_  *******************************
** The following routines are used to access elements of the current row
** in the result set.
*/
const void *capdb_column_blob(capdb_stmt *pStmt, int i){
  const void *val;
  val = capdb_value_blob( columnMem(pStmt,i) );
  /* Even though there is no encoding conversion, value_blob() might
  ** need to call malloc() to expand the result of a zeroblob()
  ** expression.
  */
  columnMallocFailure(pStmt);
  return val;
}
int capdb_column_bytes(capdb_stmt *pStmt, int i){
  int val = capdb_value_bytes( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
int capdb_column_bytes16(capdb_stmt *pStmt, int i){
  int val = capdb_value_bytes16( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
double capdb_column_double(capdb_stmt *pStmt, int i){
  double val = capdb_value_double( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
int capdb_column_int(capdb_stmt *pStmt, int i){
  int val = capdb_value_int( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
sqlite_int64 capdb_column_int64(capdb_stmt *pStmt, int i){
  sqlite_int64 val = capdb_value_int64( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
const unsigned char *capdb_column_text(capdb_stmt *pStmt, int i){
  const unsigned char *val = capdb_value_text( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
capdb_value *capdb_column_value(capdb_stmt *pStmt, int i){
  Mem *pOut = columnMem(pStmt, i);
  if( pOut->flags&MEM_Static ){
    pOut->flags &= ~MEM_Static;
    pOut->flags |= MEM_Ephem;
  }
  columnMallocFailure(pStmt);
  return (capdb_value *)pOut;
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_column_text16(capdb_stmt *pStmt, int i){
  const void *val = capdb_value_text16( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return val;
}
#endif /* CAPDB_OMIT_UTF16 */
int capdb_column_type(capdb_stmt *pStmt, int i){
  int iType = capdb_value_type( columnMem(pStmt,i) );
  columnMallocFailure(pStmt);
  return iType;
}

/*
** Column names appropriate for EXPLAIN or EXPLAIN QUERY PLAN.
*/
static const char * const azExplainColNames8[] = {
   "addr", "opcode", "p1", "p2", "p3", "p4", "p5", "comment",  /* EXPLAIN */
   "id", "parent", "notused", "detail"                         /* EQP */
};
static const u16 azExplainColNames16data[] = {
  /*   0 */  'a', 'd', 'd', 'r',                0,
  /*   5 */  'o', 'p', 'c', 'o', 'd', 'e',      0,
  /*  12 */  'p', '1',                          0, 
  /*  15 */  'p', '2',                          0,
  /*  18 */  'p', '3',                          0,
  /*  21 */  'p', '4',                          0,
  /*  24 */  'p', '5',                          0,
  /*  27 */  'c', 'o', 'm', 'm', 'e', 'n', 't', 0,
  /*  35 */  'i', 'd',                          0,
  /*  38 */  'p', 'a', 'r', 'e', 'n', 't',      0,
  /*  45 */  'n', 'o', 't', 'u', 's', 'e', 'd', 0,
  /*  53 */  'd', 'e', 't', 'a', 'i', 'l',      0
};
static const u8 iExplainColNames16[] = {
  0, 5, 12, 15, 18, 21, 24, 27,
  35, 38, 45, 53
};

/*
** Convert the N-th element of pStmt->pColName[] into a string using
** xFunc() then return that string.  If N is out of range, return 0.
**
** There are up to 5 names for each column.  useType determines which
** name is returned.  Here are the names:
**
**    0      The column name as it should be displayed for output
**    1      The datatype name for the column
**    2      The name of the database that the column derives from
**    3      The name of the table that the column derives from
**    4      The name of the table column that the result column derives from
**
** If the result is not a simple column reference (if it is an expression
** or a constant) then useTypes 2, 3, and 4 return NULL.
*/
static const void *columnName(
  capdb_stmt *pStmt,     /* The statement */
  int N,                   /* Which column to get the name for */
  int useUtf16,            /* True to return the name as UTF16 */
  int useType              /* What type of name */
){
  const void *ret;
  Vdbe *p;
  int n;
  capdb *db;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pStmt==0 ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  if( N<0 ) return 0;
  ret = 0;
  p = (Vdbe *)pStmt;
  db = p->db;
  assert( db!=0 );
  capdb_mutex_enter(db->mutex);

  if( p->explain ){
    if( useType>0 ) goto columnName_end;
    n = p->explain==1 ? 8 : 4;
    if( N>=n ) goto columnName_end;
    if( useUtf16 ){
      int i = iExplainColNames16[N + 8*p->explain - 8];
      ret = (void*)&azExplainColNames16data[i];
    }else{
      ret = (void*)azExplainColNames8[N + 8*p->explain - 8];
    }
    goto columnName_end;
  }
  n = p->nResColumn;
  if( N<n ){
    u8 prior_mallocFailed = db->mallocFailed;
    N += useType*n;
#ifndef CAPDB_OMIT_UTF16
    if( useUtf16 ){
      ret = capdb_value_text16((capdb_value*)&p->aColName[N]);
    }else
#endif
    {
      ret = capdb_value_text((capdb_value*)&p->aColName[N]);
    }
    /* A malloc may have failed inside of the _text() call. If this
    ** is the case, clear the mallocFailed flag and return NULL.
    */
    assert( db->mallocFailed==0 || db->mallocFailed==1 );
    if( db->mallocFailed > prior_mallocFailed ){
      capdbOomClear(db);
      ret = 0;
    }
  }
columnName_end:
  capdb_mutex_leave(db->mutex);
  return ret;
}

/*
** Return the name of the Nth column of the result set returned by SQL
** statement pStmt.
*/
const char *capdb_column_name(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 0, COLNAME_NAME);
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_column_name16(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 1, COLNAME_NAME);
}
#endif

/*
** Constraint:  If you have ENABLE_COLUMN_METADATA then you must
** not define OMIT_DECLTYPE.
*/
#if defined(CAPDB_OMIT_DECLTYPE) && defined(CAPDB_ENABLE_COLUMN_METADATA)
# error "Must not define both CAPDB_OMIT_DECLTYPE \
         and CAPDB_ENABLE_COLUMN_METADATA"
#endif

#ifndef CAPDB_OMIT_DECLTYPE
/*
** Return the column declaration type (if applicable) of the 'i'th column
** of the result set of SQL statement pStmt.
*/
const char *capdb_column_decltype(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 0, COLNAME_DECLTYPE);
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_column_decltype16(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 1, COLNAME_DECLTYPE);
}
#endif /* CAPDB_OMIT_UTF16 */
#endif /* CAPDB_OMIT_DECLTYPE */

#ifdef CAPDB_ENABLE_COLUMN_METADATA
/*
** Return the name of the database from which a result column derives.
** NULL is returned if the result column is an expression or constant or
** anything else which is not an unambiguous reference to a database column.
*/
const char *capdb_column_database_name(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 0, COLNAME_DATABASE);
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_column_database_name16(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 1, COLNAME_DATABASE);
}
#endif /* CAPDB_OMIT_UTF16 */

/*
** Return the name of the table from which a result column derives.
** NULL is returned if the result column is an expression or constant or
** anything else which is not an unambiguous reference to a database column.
*/
const char *capdb_column_table_name(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 0, COLNAME_TABLE);
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_column_table_name16(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 1, COLNAME_TABLE);
}
#endif /* CAPDB_OMIT_UTF16 */

/*
** Return the name of the table column from which a result column derives.
** NULL is returned if the result column is an expression or constant or
** anything else which is not an unambiguous reference to a database column.
*/
const char *capdb_column_origin_name(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 0, COLNAME_COLUMN);
}
#ifndef CAPDB_OMIT_UTF16
const void *capdb_column_origin_name16(capdb_stmt *pStmt, int N){
  return columnName(pStmt, N, 1, COLNAME_COLUMN);
}
#endif /* CAPDB_OMIT_UTF16 */
#endif /* CAPDB_ENABLE_COLUMN_METADATA */


/******************************* capdb_bind_  ***************************
**
** Routines used to attach values to wildcards in a compiled SQL statement.
*/
/*
** Unbind the value bound to variable i in virtual machine p. This is the
** the same as binding a NULL value to the column. If the "i" parameter is
** out of range, then CAPDB_RANGE is returned. Otherwise CAPDB_OK.
**
** A successful evaluation of this routine acquires the mutex on p.
** the mutex is released if any kind of error occurs.
**
** The error code stored in database p->db is overwritten with the return
** value in any case.
**
** (tag-20240917-01) If  vdbeUnbind(p,(u32)(i-1))  returns CAPDB_OK,
** that means all of the the following will be true:
**
**     p!=0
**     p->pVar!=0
**     i>0
**     i<=p->nVar
**
** An assert() is normally added after vdbeUnbind() to help static analyzers
** realize this.
*/
static int vdbeUnbind(Vdbe *p, unsigned int i){
  Mem *pVar;
  if( vdbeSafetyNotNull(p) ){
    return CAPDB_MISUSE_BKPT;
  }
  capdb_mutex_enter(p->db->mutex);
  if( p->eVdbeState!=VDBE_READY_STATE ){
    capdbError(p->db, CAPDB_MISUSE_BKPT);
    capdb_mutex_leave(p->db->mutex);
    capdb_log(CAPDB_MISUSE,
        "bind on a busy prepared statement: [%s]", p->zSql);
    return CAPDB_MISUSE_BKPT;
  }
  if( i>=(unsigned int)p->nVar ){
    capdbError(p->db, CAPDB_RANGE);
    capdb_mutex_leave(p->db->mutex);
    return CAPDB_RANGE;
  }
  pVar = &p->aVar[i];
  capdbVdbeMemRelease(pVar);
  pVar->flags = MEM_Null;
  p->db->errCode = CAPDB_OK;

  /* If the bit corresponding to this variable in Vdbe.expmask is set, then
  ** binding a new value to this variable invalidates the current query plan.
  **
  ** IMPLEMENTATION-OF: R-57496-20354 If the specific value bound to a host
  ** parameter in the WHERE clause might influence the choice of query plan
  ** for a statement, then the statement will be automatically recompiled,
  ** as if there had been a schema change, on the first capdb_step() call
  ** following any change to the bindings of that parameter.
  */
  assert( (p->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 || p->expmask==0 );
  if( p->expmask!=0 && (p->expmask & (i>=31 ? 0x80000000 : (u32)1<<i))!=0 ){
    p->expired = 1;
  }
  return CAPDB_OK;
}

/*
** Bind a text or BLOB value.
*/
static int bindText(
  capdb_stmt *pStmt,   /* The statement to bind against */
  int i,                 /* Index of the parameter to bind */
  const void *zData,     /* Pointer to the data to be bound */
  i64 nData,             /* Number of bytes of data to be bound */
  void (*xDel)(void*),   /* Destructor for the data */
  u8 encoding            /* Encoding for the data */
){
  Vdbe *p = (Vdbe *)pStmt;
  Mem *pVar;
  int rc;

  rc = vdbeUnbind(p, (u32)(i-1));
  if( rc==CAPDB_OK ){
    assert( p!=0 && p->aVar!=0 && i>0 && i<=p->nVar ); /* tag-20240917-01 */
    if( zData!=0 ){
      pVar = &p->aVar[i-1];
      if( encoding==CAPDB_UTF8 ){
        rc = capdbVdbeMemSetText(pVar, zData, nData, xDel);
      }else if( encoding==CAPDB_UTF8_ZT ){
        /* It is usually consider improper to assert() on an input.
        ** However, the following assert() is checking for inputs
        ** that are documented to result in undefined behavior. */
        assert( zData==0
             || nData<0 
             || nData>pVar->db->aLimit[CAPDB_LIMIT_LENGTH]
             || ((u8*)zData)[nData]==0
        );
        rc = capdbVdbeMemSetText(pVar, zData, nData, xDel);
        pVar->flags |= MEM_Term;
      }else{
        rc = capdbVdbeMemSetStr(pVar, zData, nData, encoding, xDel);
        if( encoding==0 ) pVar->enc = ENC(p->db);
      }
      if( rc==CAPDB_OK && encoding!=0 ){
        rc = capdbVdbeChangeEncoding(pVar, ENC(p->db));
      }
      if( rc ){
        capdbError(p->db, rc);
        rc = capdbApiExit(p->db, rc);
      }
    }
    capdb_mutex_leave(p->db->mutex);
  }else if( xDel!=CAPDB_STATIC && xDel!=CAPDB_TRANSIENT ){
    xDel((void*)zData);
  }
  return rc;
}


/*
** Bind a blob value to an SQL statement variable.
*/
int capdb_bind_blob(
  capdb_stmt *pStmt,
  int i,
  const void *zData,
  int nData,
  void (*xDel)(void*)
){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( nData<0 ) return CAPDB_MISUSE_BKPT;
#endif
  return bindText(pStmt, i, zData, nData, xDel, 0);
}
int capdb_bind_blob64(
  capdb_stmt *pStmt,
  int i,
  const void *zData,
  capdb_uint64 nData,
  void (*xDel)(void*)
){
  assert( xDel!=CAPDB_DYNAMIC );
  return bindText(pStmt, i, zData, nData, xDel, 0);
}
int capdb_bind_double(capdb_stmt *pStmt, int i, double rValue){
  int rc;
  Vdbe *p = (Vdbe *)pStmt;
  rc = vdbeUnbind(p, (u32)(i-1));
  if( rc==CAPDB_OK ){
    assert( p!=0 && p->aVar!=0 && i>0 && i<=p->nVar ); /* tag-20240917-01 */
    capdbVdbeMemSetDouble(&p->aVar[i-1], rValue);
    capdb_mutex_leave(p->db->mutex);
  }
  return rc;
}
int capdb_bind_int(capdb_stmt *p, int i, int iValue){
  return capdb_bind_int64(p, i, (i64)iValue);
}
int capdb_bind_int64(capdb_stmt *pStmt, int i, sqlite_int64 iValue){
  int rc;
  Vdbe *p = (Vdbe *)pStmt;
  rc = vdbeUnbind(p, (u32)(i-1));
  if( rc==CAPDB_OK ){
    assert( p!=0 && p->aVar!=0 && i>0 && i<=p->nVar ); /* tag-20240917-01 */
    capdbVdbeMemSetInt64(&p->aVar[i-1], iValue);
    capdb_mutex_leave(p->db->mutex);
  }
  return rc;
}
int capdb_bind_null(capdb_stmt *pStmt, int i){
  int rc;
  Vdbe *p = (Vdbe*)pStmt;
  rc = vdbeUnbind(p, (u32)(i-1));
  if( rc==CAPDB_OK ){
    assert( p!=0 && p->aVar!=0 && i>0 && i<=p->nVar ); /* tag-20240917-01 */
    capdb_mutex_leave(p->db->mutex);
  }
  return rc;
}
int capdb_bind_pointer(
  capdb_stmt *pStmt,
  int i,
  void *pPtr,
  const char *zPTtype,
  void (*xDestructor)(void*)
){
  int rc;
  Vdbe *p = (Vdbe*)pStmt;
  rc = vdbeUnbind(p, (u32)(i-1));
  if( rc==CAPDB_OK ){
    assert( p!=0 && p->aVar!=0 && i>0 && i<=p->nVar ); /* tag-20240917-01 */
    capdbVdbeMemSetPointer(&p->aVar[i-1], pPtr, zPTtype, xDestructor);
    capdb_mutex_leave(p->db->mutex);
  }else if( xDestructor ){
    xDestructor(pPtr);
  }
  return rc;
}
int capdb_bind_text(
  capdb_stmt *pStmt,
  int i,
  const char *zData,
  int nData,
  void (*xDel)(void*)
){
  return bindText(pStmt, i, zData, nData, xDel, CAPDB_UTF8);
}
int capdb_bind_text64(
  capdb_stmt *pStmt,
  int i,
  const char *zData,
  capdb_uint64 nData,
  void (*xDel)(void*),
  unsigned char enc
){
  assert( xDel!=CAPDB_DYNAMIC );
  if( enc!=CAPDB_UTF8 && enc!=CAPDB_UTF8_ZT ){
    if( enc==CAPDB_UTF16 ) enc = CAPDB_UTF16NATIVE;
    nData &= ~(u64)1;
  }
  return bindText(pStmt, i, zData, nData, xDel, enc);
}
#ifndef CAPDB_OMIT_UTF16
int capdb_bind_text16(
  capdb_stmt *pStmt,
  int i,
  const void *zData,
  int n,
  void (*xDel)(void*)
){
  return bindText(pStmt, i, zData, n & ~(u64)1, xDel, CAPDB_UTF16NATIVE);
}
#endif /* CAPDB_OMIT_UTF16 */
int capdb_bind_value(capdb_stmt *pStmt, int i, const capdb_value *pValue){
  int rc;
  switch( capdb_value_type((capdb_value*)pValue) ){
    case CAPDB_INTEGER: {
      rc = capdb_bind_int64(pStmt, i, pValue->u.i);
      break;
    }
    case CAPDB_FLOAT: {
      assert( pValue->flags & (MEM_Real|MEM_IntReal) );
      rc = capdb_bind_double(pStmt, i,
          (pValue->flags & MEM_Real) ? pValue->u.r : (double)pValue->u.i
      );
      break;
    }
    case CAPDB_BLOB: {
      if( pValue->flags & MEM_Zero ){
        rc = capdb_bind_zeroblob(pStmt, i, pValue->u.nZero);
      }else{
        rc = capdb_bind_blob(pStmt, i, pValue->z, pValue->n,CAPDB_TRANSIENT);
      }
      break;
    }
    case CAPDB_TEXT: {
      rc = bindText(pStmt,i,  pValue->z, pValue->n, CAPDB_TRANSIENT,
                              pValue->enc);
      break;
    }
    default: {
      rc = capdb_bind_null(pStmt, i);
      break;
    }
  }
  return rc;
}
int capdb_bind_zeroblob(capdb_stmt *pStmt, int i, int n){
  int rc;
  Vdbe *p = (Vdbe *)pStmt;
  rc = vdbeUnbind(p, (u32)(i-1));
  if( rc==CAPDB_OK ){
    assert( p!=0 && p->aVar!=0 && i>0 && i<=p->nVar ); /* tag-20240917-01 */
#ifndef CAPDB_OMIT_INCRBLOB
    capdbVdbeMemSetZeroBlob(&p->aVar[i-1], n);
#else
    rc = capdbVdbeMemSetZeroBlob(&p->aVar[i-1], n);
#endif
    capdb_mutex_leave(p->db->mutex);
  }
  return rc;
}
int capdb_bind_zeroblob64(capdb_stmt *pStmt, int i, capdb_uint64 n){
  int rc;
  Vdbe *p = (Vdbe *)pStmt;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(p->db->mutex);
  if( n>(u64)p->db->aLimit[CAPDB_LIMIT_LENGTH] ){
    rc = CAPDB_TOOBIG;
  }else{
    assert( (n & 0x7FFFFFFF)==n );
    rc = capdb_bind_zeroblob(pStmt, i, n);
  }
  rc = capdbApiExit(p->db, rc);
  capdb_mutex_leave(p->db->mutex);
  return rc;
}

/*
** Return the number of wildcards that can be potentially bound to.
** This routine is added to support DBD::SQLite. 
*/
int capdb_bind_parameter_count(capdb_stmt *pStmt){
  Vdbe *p = (Vdbe*)pStmt;
  return p ? p->nVar : 0;
}

/*
** Return the name of a wildcard parameter.  Return NULL if the index
** is out of range or if the wildcard is unnamed.
**
** The result is always UTF-8.
*/
const char *capdb_bind_parameter_name(capdb_stmt *pStmt, int i){
  Vdbe *p = (Vdbe*)pStmt;
  if( p==0 ) return 0;
  return capdbVListNumToName(p->pVList, i);
}

/*
** Given a wildcard parameter name, return the index of the variable
** with that name.  If there is no variable with the given name,
** return 0.
*/
int capdbVdbeParameterIndex(Vdbe *p, const char *zName, int nName){
  if( p==0 || zName==0 ) return 0;
  return capdbVListNameToNum(p->pVList, zName, nName);
}
int capdb_bind_parameter_index(capdb_stmt *pStmt, const char *zName){
  return capdbVdbeParameterIndex((Vdbe*)pStmt, zName, capdbStrlen30(zName));
}

/*
** Transfer all bindings from the first statement over to the second.
*/
int capdbTransferBindings(capdb_stmt *pFromStmt, capdb_stmt *pToStmt){
  Vdbe *pFrom = (Vdbe*)pFromStmt;
  Vdbe *pTo = (Vdbe*)pToStmt;
  int i;
  assert( pTo->db==pFrom->db );
  assert( pTo->nVar==pFrom->nVar );
  capdb_mutex_enter(pTo->db->mutex);
  for(i=0; i<pFrom->nVar; i++){
    capdbVdbeMemMove(&pTo->aVar[i], &pFrom->aVar[i]);
  }
  capdb_mutex_leave(pTo->db->mutex);
  return CAPDB_OK;
}

#ifndef CAPDB_OMIT_DEPRECATED
/*
** Deprecated external interface.  Internal/core SQLite code
** should call capdbTransferBindings.
**
** It is misuse to call this routine with statements from different
** database connections.  But as this is a deprecated interface, we
** will not bother to check for that condition.
**
** If the two statements contain a different number of bindings, then
** an CAPDB_ERROR is returned.  Nothing else can go wrong, so otherwise
** CAPDB_OK is returned.
*/
int capdb_transfer_bindings(capdb_stmt *pFromStmt, capdb_stmt *pToStmt){
  Vdbe *pFrom = (Vdbe*)pFromStmt;
  Vdbe *pTo = (Vdbe*)pToStmt;
  if( pFrom->nVar!=pTo->nVar ){
    return CAPDB_ERROR;
  }
  assert( (pTo->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 || pTo->expmask==0 );
  if( pTo->expmask ){
    pTo->expired = 1;
  }
  assert( (pFrom->prepFlags & CAPDB_PREPARE_SAVESQL)!=0 || pFrom->expmask==0 );
  if( pFrom->expmask ){
    pFrom->expired = 1;
  }
  return capdbTransferBindings(pFromStmt, pToStmt);
}
#endif

/*
** Return the capdb* database handle to which the prepared statement given
** in the argument belongs.  This is the same database handle that was
** the first argument to the capdb_prepare() that was used to create
** the statement in the first place.
*/
capdb *capdb_db_handle(capdb_stmt *pStmt){
  return pStmt ? ((Vdbe*)pStmt)->db : 0;
}

/*
** Return true if the prepared statement is guaranteed to not modify the
** database.
*/
int capdb_stmt_readonly(capdb_stmt *pStmt){
  return pStmt ? ((Vdbe*)pStmt)->readOnly : 1;
}

/*
** Return 1 if the statement is an EXPLAIN and return 2 if the
** statement is an EXPLAIN QUERY PLAN
*/
int capdb_stmt_isexplain(capdb_stmt *pStmt){
  return pStmt ? ((Vdbe*)pStmt)->explain : 0;
}

/*
** Set the explain mode for a statement.
*/
int capdb_stmt_explain(capdb_stmt *pStmt, int eMode){
  Vdbe *v = (Vdbe*)pStmt;
  int rc;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( pStmt==0 ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(v->db->mutex);
  if( ((int)v->explain)==eMode ){
    rc = CAPDB_OK;
  }else if( eMode<0 || eMode>2 ){
    rc = CAPDB_ERROR;
  }else if( (v->prepFlags & CAPDB_PREPARE_SAVESQL)==0 ){
    rc = CAPDB_ERROR;
  }else if( v->eVdbeState!=VDBE_READY_STATE ){
    rc = CAPDB_BUSY;
  }else if( v->nMem>=10 && (eMode!=2 || v->haveEqpOps) ){
    /* No reprepare necessary */
    v->explain = eMode;
    rc = CAPDB_OK;
  }else{
    v->explain = eMode;
    rc = capdbReprepare(v);
    v->haveEqpOps = eMode==2;
  }
  if( v->explain ){
    v->nResColumn = 12 - 4*v->explain;
  }else{
    v->nResColumn = v->nResAlloc;
  }
  capdb_mutex_leave(v->db->mutex);
  return rc;
}

/*
** Return true if the prepared statement is in need of being reset.
*/
int capdb_stmt_busy(capdb_stmt *pStmt){
  Vdbe *v = (Vdbe*)pStmt;
  return v!=0 && v->eVdbeState==VDBE_RUN_STATE;
}

/*
** Return a pointer to the next prepared statement after pStmt associated
** with database connection pDb.  If pStmt is NULL, return the first
** prepared statement for the database connection.  Return NULL if there
** are no more.
*/
capdb_stmt *capdb_next_stmt(capdb *pDb, capdb_stmt *pStmt){
  capdb_stmt *pNext;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(pDb) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  capdb_mutex_enter(pDb->mutex);
  if( pStmt==0 ){
    pNext = (capdb_stmt*)pDb->pVdbe;
  }else{
    pNext = (capdb_stmt*)((Vdbe*)pStmt)->pVNext;
  }
  capdb_mutex_leave(pDb->mutex);
  return pNext;
}

/*
** Return the value of a status counter for a prepared statement
*/
int capdb_stmt_status(capdb_stmt *pStmt, int op, int resetFlag){
  Vdbe *pVdbe = (Vdbe*)pStmt;
  u32 v;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !pStmt
   || (op!=CAPDB_STMTSTATUS_MEMUSED && (op<0||op>=ArraySize(pVdbe->aCounter)))
  ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  if( op==CAPDB_STMTSTATUS_MEMUSED ){
    capdb *db = pVdbe->db;
    capdb_mutex_enter(db->mutex);
    v = 0;
    db->pnBytesFreed = (int*)&v;
    assert( db->lookaside.pEnd==db->lookaside.pTrueEnd );
    db->lookaside.pEnd = db->lookaside.pStart;
    capdbVdbeDelete(pVdbe);
    db->pnBytesFreed = 0;
    db->lookaside.pEnd = db->lookaside.pTrueEnd;
    capdb_mutex_leave(db->mutex);
  }else{
    v = pVdbe->aCounter[op];
    if( resetFlag ) pVdbe->aCounter[op] = 0;
  }
  return (int)v;
}

/*
** Return the SQL associated with a prepared statement
*/
const char *capdb_sql(capdb_stmt *pStmt){
  Vdbe *p = (Vdbe *)pStmt;
  return p ? p->zSql : 0;
}

/*
** Return the SQL associated with a prepared statement with
** bound parameters expanded.  Space to hold the returned string is
** obtained from capdb_malloc().  The caller is responsible for
** freeing the returned string by passing it to capdb_free().
**
** The CAPDB_TRACE_SIZE_LIMIT puts an upper bound on the size of
** expanded bound parameters.
*/
char *capdb_expanded_sql(capdb_stmt *pStmt){
#ifdef CAPDB_OMIT_TRACE
  return 0;
#else
  char *z = 0;
  const char *zSql = capdb_sql(pStmt);
  if( zSql ){
    Vdbe *p = (Vdbe *)pStmt;
    capdb_mutex_enter(p->db->mutex);
    z = capdbVdbeExpandSql(p, zSql);
    capdb_mutex_leave(p->db->mutex);
  }
  return z;
#endif
}

#ifdef CAPDB_ENABLE_NORMALIZE
/*
** Return the normalized SQL associated with a prepared statement.
*/
const char *capdb_normalized_sql(capdb_stmt *pStmt){
  Vdbe *p = (Vdbe *)pStmt;
  if( p==0 ) return 0;
  if( p->zNormSql==0 && ALWAYS(p->zSql!=0) ){
    capdb_mutex_enter(p->db->mutex);
    p->zNormSql = capdbNormalize(p, p->zSql);
    capdb_mutex_leave(p->db->mutex);
  }
  return p->zNormSql;
}
#endif /* CAPDB_ENABLE_NORMALIZE */

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** Allocate and populate an UnpackedRecord structure based on the serialized
** record in nKey/pKey. Return a pointer to the new UnpackedRecord structure
** if successful, or a NULL pointer if an OOM error is encountered.
*/
static UnpackedRecord *vdbeUnpackRecord(
  KeyInfo *pKeyInfo,
  int nKey,
  const void *pKey
){
  UnpackedRecord *pRet;           /* Return value */

  pRet = capdbVdbeAllocUnpackedRecord(pKeyInfo);
  if( pRet ){
    memset(pRet->aMem, 0, sizeof(Mem)*(pKeyInfo->nKeyField+1));
    capdbVdbeRecordUnpack(nKey, pKey, pRet);
  }
  return pRet;
}

/*
** This function is called from within a pre-update callback to retrieve
** a field of the row currently being updated or deleted.
*/
int capdb_preupdate_old(capdb *db, int iIdx, capdb_value **ppValue){
  PreUpdate *p;
  Mem *pMem;
  int rc = CAPDB_OK;
  int iStore = 0;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( db==0 || ppValue==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  p = db->pPreUpdate;
  /* Test that this call is being made from within an CAPDB_DELETE or
  ** CAPDB_UPDATE pre-update callback, and that iIdx is within range. */
  if( !p || p->op==CAPDB_INSERT ){
    rc = CAPDB_MISUSE_BKPT;
    goto preupdate_old_out;
  }
  if( p->pPk ){
    iStore = capdbTableColumnToIndex(p->pPk, iIdx);
  }else if( iIdx >= p->pTab->nCol ){
    rc = CAPDB_MISUSE_BKPT;
    goto preupdate_old_out;
  }else{
    iStore = capdbTableColumnToStorage(p->pTab, iIdx);
  }
  if( iStore>=p->pCsr->nField || iStore<0 ){
    rc = CAPDB_RANGE;
    goto preupdate_old_out;
  }

  if( iIdx==p->pTab->iPKey ){
    *ppValue = pMem = &p->oldipk;
    capdbVdbeMemSetInt64(pMem, p->iKey1);
  }else{

    /* If the old.* record has not yet been loaded into memory, do so now. */
    if( p->pUnpacked==0 ){
      u32 nRec;
      u8 *aRec;

      assert( p->pCsr->eCurType==CURTYPE_BTREE );
      nRec = capdbBtreePayloadSize(p->pCsr->uc.pCursor);
      aRec = capdbDbMallocRaw(db, nRec);
      if( !aRec ) goto preupdate_old_out;
      rc = capdbBtreePayload(p->pCsr->uc.pCursor, 0, nRec, aRec);
      if( rc==CAPDB_OK ){
        p->pUnpacked = vdbeUnpackRecord(p->pKeyinfo, nRec, aRec);
        if( !p->pUnpacked ) rc = CAPDB_NOMEM;
      }
      if( rc!=CAPDB_OK ){
        capdbDbFree(db, aRec);
        goto preupdate_old_out;
      }
      p->aRecord = aRec;
    }

    pMem = *ppValue = &p->pUnpacked->aMem[iStore];
    if( iStore>=p->pUnpacked->nField ){
      /* This occurs when the table has been extended using ALTER TABLE
      ** ADD COLUMN. The value to return is the default value of the column. */
      Column *pCol = &p->pTab->aCol[iIdx];
      if( pCol->iDflt>0 ){
        if( p->apDflt==0 ){
          int nByte;
          assert( sizeof(capdb_value*)*UMXV(p->pTab->nCol) < 0x7fffffff );
          nByte = sizeof(capdb_value*)*p->pTab->nCol;
          p->apDflt = (capdb_value**)capdbDbMallocZero(db, nByte);
          if( p->apDflt==0 ) goto preupdate_old_out;
        }
        if( p->apDflt[iIdx]==0 ){
          capdb_value *pVal = 0;
          Expr *pDflt;
          assert( p->pTab!=0 && IsOrdinaryTable(p->pTab) );
          pDflt = p->pTab->u.tab.pDfltList->a[pCol->iDflt-1].pExpr;
          rc = capdbValueFromExpr(db, pDflt, ENC(db), pCol->affinity, &pVal);
          if( rc==CAPDB_OK && pVal==0 ){
            rc = CAPDB_CORRUPT_BKPT;
          }
          p->apDflt[iIdx] = pVal;
        }
        *ppValue = p->apDflt[iIdx];
      }else{
        *ppValue = (capdb_value *)columnNullValue();
      }
    }else if( p->pTab->aCol[iIdx].affinity==CAPDB_AFF_REAL ){
      if( pMem->flags & (MEM_Int|MEM_IntReal) ){
        testcase( pMem->flags & MEM_Int );
        testcase( pMem->flags & MEM_IntReal );
        capdbVdbeMemRealify(pMem);
      }
    }
  }

 preupdate_old_out:
  capdbError(db, rc);
  return capdbApiExit(db, rc);
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** This function is called from within a pre-update callback to retrieve
** the number of columns in the row being updated, deleted or inserted.
*/
int capdb_preupdate_count(capdb *db){
  PreUpdate *p;
#ifdef CAPDB_ENABLE_API_ARMOR
  p = db!=0 ? db->pPreUpdate : 0;
#else
  p = db->pPreUpdate;
#endif
  return (p ? p->pKeyinfo->nKeyField : 0);
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** This function is designed to be called from within a pre-update callback
** only. It returns zero if the change that caused the callback was made
** immediately by a user SQL statement. Or, if the change was made by a
** trigger program, it returns the number of trigger programs currently
** on the stack (1 for a top-level trigger, 2 for a trigger fired by a
** top-level trigger etc.).
**
** For the purposes of the previous paragraph, a foreign key CASCADE, SET NULL
** or SET DEFAULT action is considered a trigger.
*/
int capdb_preupdate_depth(capdb *db){
  PreUpdate *p;
#ifdef CAPDB_ENABLE_API_ARMOR
  p = db!=0 ? db->pPreUpdate : 0;
#else
  p = db->pPreUpdate;
#endif
  return (p ? p->v->nFrame : 0);
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** This function is designed to be called from within a pre-update callback
** only.
*/
int capdb_preupdate_blobwrite(capdb *db){
  PreUpdate *p;
#ifdef CAPDB_ENABLE_API_ARMOR
  p = db!=0 ? db->pPreUpdate : 0;
#else
  p = db->pPreUpdate;
#endif
  return (p ? p->iBlobWrite : -1);
}
#endif

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** This function is called from within a pre-update callback to retrieve
** a field of the row currently being updated or inserted.
*/
int capdb_preupdate_new(capdb *db, int iIdx, capdb_value **ppValue){
  PreUpdate *p;
  int rc = CAPDB_OK;
  Mem *pMem;
  int iStore = 0;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( db==0 || ppValue==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  p = db->pPreUpdate;
  if( !p || p->op==CAPDB_DELETE ){
    rc = CAPDB_MISUSE_BKPT;
    goto preupdate_new_out;
  }
  if( p->pPk && p->op!=CAPDB_UPDATE ){
    iStore = capdbTableColumnToIndex(p->pPk, iIdx);
  }else if( iIdx >= p->pTab->nCol ){
    return CAPDB_MISUSE_BKPT;
  }else{
    iStore = capdbTableColumnToStorage(p->pTab, iIdx);
  }

  if( iStore>=p->pCsr->nField || iStore<0 ){
    rc = CAPDB_RANGE;
    goto preupdate_new_out;
  }

  if( p->op==CAPDB_INSERT ){
    /* For an INSERT, memory cell p->iNewReg contains the serialized record
    ** that is being inserted. Deserialize it. */
    UnpackedRecord *pUnpack = p->pNewUnpacked;
    if( !pUnpack ){
      Mem *pData = &p->v->aMem[p->iNewReg];
      rc = ExpandBlob(pData);
      if( rc!=CAPDB_OK ) goto preupdate_new_out;
      pUnpack = vdbeUnpackRecord(p->pKeyinfo, pData->n, pData->z);
      if( !pUnpack ){
        rc = CAPDB_NOMEM;
        goto preupdate_new_out;
      }
      p->pNewUnpacked = pUnpack;
    }
    pMem = &pUnpack->aMem[iStore];
    if( iIdx==p->pTab->iPKey ){
      capdbVdbeMemSetInt64(pMem, p->iKey2);
    }else if( iStore>=pUnpack->nField ){
      pMem = (capdb_value *)columnNullValue();
    }
  }else{
    /* For an UPDATE, memory cell (p->iNewReg+1+iStore) contains the required
    ** value. Make a copy of the cell contents and return a pointer to it.
    ** It is not safe to return a pointer to the memory cell itself as the
    ** caller may modify the value text encoding.
    */
    assert( p->op==CAPDB_UPDATE );
    if( !p->aNew ){
      assert( sizeof(Mem)*UMXV(p->pCsr->nField) < 0x7fffffff );
      p->aNew = (Mem *)capdbDbMallocZero(db, sizeof(Mem)*p->pCsr->nField);
      if( !p->aNew ){
        rc = CAPDB_NOMEM;
        goto preupdate_new_out;
      }
    }
    assert( iStore>=0 && iStore<p->pCsr->nField );
    pMem = &p->aNew[iStore];
    if( pMem->flags==0 ){
      if( iIdx==p->pTab->iPKey ){
        capdbVdbeMemSetInt64(pMem, p->iKey2);
      }else{
        rc = capdbVdbeMemCopy(pMem, &p->v->aMem[p->iNewReg+1+iStore]);
        if( rc!=CAPDB_OK ) goto preupdate_new_out;
      }
    }
  }
  *ppValue = pMem;

 preupdate_new_out:
  capdbError(db, rc);
  return capdbApiExit(db, rc);
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

#ifdef CAPDB_ENABLE_STMT_SCANSTATUS
/*
** Return status data for a single loop within query pStmt.
*/
int capdb_stmt_scanstatus_v2(
  capdb_stmt *pStmt,            /* Prepared statement being queried */
  int iScan,                      /* Index of loop to report on */
  int iScanStatusOp,              /* Which metric to return */
  int flags,
  void *pOut                      /* OUT: Write the answer here */
){
  Vdbe *p = (Vdbe*)pStmt;
  VdbeOp *aOp;
  int nOp;
  ScanStatus *pScan = 0;
  int idx;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 || pOut==0
      || iScanStatusOp<CAPDB_SCANSTAT_NLOOP
      || iScanStatusOp>CAPDB_SCANSTAT_NCYCLE ){
    return 1;
  }
#endif
  aOp = p->aOp;
  nOp = p->nOp;
  if( p->pFrame ){
    VdbeFrame *pFrame;
    for(pFrame=p->pFrame; pFrame->pParent; pFrame=pFrame->pParent);
    aOp = pFrame->aOp;
    nOp = pFrame->nOp;
  }

  if( iScan<0 ){
    int ii;
    if( iScanStatusOp==CAPDB_SCANSTAT_NCYCLE ){
      i64 res = 0;
      for(ii=0; ii<nOp; ii++){
        res += aOp[ii].nCycle;
      }
      *(i64*)pOut = res;
      return 0;
    }
    return 1;
  }
  if( flags & CAPDB_SCANSTAT_COMPLEX ){
    idx = iScan;
  }else{
    /* If the COMPLEX flag is clear, then this function must ignore any
    ** ScanStatus structures with ScanStatus.addrLoop set to 0. */
    for(idx=0; idx<p->nScan; idx++){
      pScan = &p->aScan[idx];
      if( pScan->zName ){
        iScan--;
        if( iScan<0 ) break;
      }
    }
  }
  if( idx>=p->nScan ) return 1;
  assert( pScan==0 || pScan==&p->aScan[idx] );
  pScan = &p->aScan[idx];

  switch( iScanStatusOp ){
    case CAPDB_SCANSTAT_NLOOP: {
      if( pScan->addrLoop>0 ){
        *(capdb_int64*)pOut = aOp[pScan->addrLoop].nExec;
      }else{
        *(capdb_int64*)pOut = -1;
      }
      break;
    }
    case CAPDB_SCANSTAT_NVISIT: {
      if( pScan->addrVisit>0 ){
        *(capdb_int64*)pOut = aOp[pScan->addrVisit].nExec;
      }else{
        *(capdb_int64*)pOut = -1;
      }
      break;
    }
    case CAPDB_SCANSTAT_EST: {
      double r = 1.0;
      LogEst x = pScan->nEst;
      while( x<100 ){
        x += 10;
        r *= 0.5;
      }
      *(double*)pOut = r*capdbLogEstToInt(x);
      break;
    }
    case CAPDB_SCANSTAT_NAME: {
      *(const char**)pOut = pScan->zName;
      break;
    }
    case CAPDB_SCANSTAT_EXPLAIN: {
      if( pScan->addrExplain ){
        *(const char**)pOut = aOp[ pScan->addrExplain ].p4.z;
      }else{
        *(const char**)pOut = 0;
      }
      break;
    }
    case CAPDB_SCANSTAT_SELECTID: {
      if( pScan->addrExplain ){
        *(int*)pOut = aOp[ pScan->addrExplain ].p1;
      }else{
        *(int*)pOut = -1;
      }
      break;
    }
    case CAPDB_SCANSTAT_PARENTID: {
      if( pScan->addrExplain ){
        *(int*)pOut = aOp[ pScan->addrExplain ].p2;
      }else{
        *(int*)pOut = -1;
      }
      break;
    }
    case CAPDB_SCANSTAT_NCYCLE: {
      i64 res = 0;
      if( pScan->aAddrRange[0]==0 ){
        res = -1;
      }else{
        int ii;
        for(ii=0; ii<ArraySize(pScan->aAddrRange); ii+=2){
          int iIns = pScan->aAddrRange[ii];
          int iEnd = pScan->aAddrRange[ii+1];
          if( iIns==0 ) break;
          if( iIns>0 ){
            while( iIns<=iEnd ){
              res += aOp[iIns].nCycle;
              iIns++;
            }
          }else{
            int iOp;
            for(iOp=0; iOp<nOp; iOp++){
              Op *pOp = &aOp[iOp];
              if( pOp->p1!=iEnd ) continue;
              if( (capdbOpcodeProperty[pOp->opcode] & OPFLG_NCYCLE)==0 ){
                continue;
              }
              res += aOp[iOp].nCycle;
            }
          }
        }
      }
      *(i64*)pOut = res;
      break;
    }
    default: {
      return 1;
    }
  }
  return 0;
}

/*
** Return status data for a single loop within query pStmt.
*/
int capdb_stmt_scanstatus(
  capdb_stmt *pStmt,            /* Prepared statement being queried */
  int iScan,                      /* Index of loop to report on */
  int iScanStatusOp,              /* Which metric to return */
  void *pOut                      /* OUT: Write the answer here */
){
  return capdb_stmt_scanstatus_v2(pStmt, iScan, iScanStatusOp, 0, pOut);
}

/*
** Zero all counters associated with the capdb_stmt_scanstatus() data.
*/
void capdb_stmt_scanstatus_reset(capdb_stmt *pStmt){
  Vdbe *p = (Vdbe*)pStmt;
  int ii;
  for(ii=0; p!=0 && ii<p->nOp; ii++){
    Op *pOp = &p->aOp[ii];
    pOp->nExec = 0;
    pOp->nCycle = 0;
  }
}
#endif /* CAPDB_ENABLE_STMT_SCANSTATUS */
