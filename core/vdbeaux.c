/*
** 2003 September 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used for creating, destroying, and populating
** a VDBE (or an "capdb_stmt" as it is known to the outside world.)
*/
#include "capdbInt.h"
#include "vdbeInt.h"

/* Forward references */
static void freeEphemeralFunction(capdb *db, FuncDef *pDef);
static void vdbeFreeOpArray(capdb *, Op *, int);

/*
** Create a new virtual database engine.
*/
Vdbe *capdbVdbeCreate(Parse *pParse){
  capdb *db = pParse->db;
  Vdbe *p;
  p = capdbDbMallocRawNN(db, sizeof(Vdbe) );
  if( p==0 ) return 0;
  memset(&p->aOp, 0, sizeof(Vdbe)-offsetof(Vdbe,aOp));
  p->db = db;
  if( db->pVdbe ){
    db->pVdbe->ppVPrev = &p->pVNext;
  }
  p->pVNext = db->pVdbe;
  p->ppVPrev = &db->pVdbe;
  db->pVdbe = p;
  assert( p->eVdbeState==VDBE_INIT_STATE );
  p->pParse = pParse;
  pParse->pVdbe = p;
  assert( pParse->aLabel==0 );
  assert( pParse->nLabel==0 );
  assert( p->nOpAlloc==0 );
  assert( pParse->szOpAlloc==0 );
  capdbVdbeAddOp2(p, OP_Init, 0, 1);
  return p;
}

/*
** Return the Parse object that owns a Vdbe object.
*/
Parse *capdbVdbeParser(Vdbe *p){
  return p->pParse;
}

/*
** Change the error string stored in Vdbe.zErrMsg
*/
void capdbVdbeError(Vdbe *p, const char *zFormat, ...){
  va_list ap;
  capdbDbFree(p->db, p->zErrMsg);
  va_start(ap, zFormat);
  p->zErrMsg = capdbVMPrintf(p->db, zFormat, ap);
  va_end(ap);
}

/*
** Remember the SQL string for a prepared statement.
*/
void capdbVdbeSetSql(Vdbe *p, const char *z, int n, u8 prepFlags){
  if( p==0 ) return;
  p->prepFlags = prepFlags;
  if( (prepFlags & CAPDB_PREPARE_SAVESQL)==0 ){
    p->expmask = 0;
  }
  assert( p->zSql==0 );
  p->zSql = capdbDbStrNDup(p->db, z, n);
}

#ifdef CAPDB_ENABLE_NORMALIZE
/*
** Add a new element to the Vdbe->pDblStr list.
*/
void capdbVdbeAddDblquoteStr(capdb *db, Vdbe *p, const char *z){
  if( p ){
    int n = capdbStrlen30(z);
    DblquoteStr *pStr = capdbDbMallocRawNN(db,
                            sizeof(*pStr)+n+1-sizeof(pStr->z));
    if( pStr ){
      pStr->pNextStr = p->pDblStr;
      p->pDblStr = pStr;
      memcpy(pStr->z, z, n+1);
    }
  }
}
#endif

#ifdef CAPDB_ENABLE_NORMALIZE
/*
** zId of length nId is a double-quoted identifier.  Check to see if
** that identifier is really used as a string literal.
*/
int capdbVdbeUsesDoubleQuotedString(
  Vdbe *pVdbe,            /* The prepared statement */
  const char *zId         /* The double-quoted identifier, already dequoted */
){
  DblquoteStr *pStr;
  assert( zId!=0 );
  if( pVdbe->pDblStr==0 ) return 0;
  for(pStr=pVdbe->pDblStr; pStr; pStr=pStr->pNextStr){
    if( strcmp(zId, pStr->z)==0 ) return 1;
  }
  return 0;
}
#endif

/*
** Swap byte-code between two VDBE structures.
**
** This happens after pB was previously run and returned
** CAPDB_SCHEMA.  The statement was then reprepared in pA.
** This routine transfers the new bytecode in pA over to pB
** so that pB can be run again.  The old pB byte code is
** moved back to pA so that it will be cleaned up when pA is
** finalized.
*/
void capdbVdbeSwap(Vdbe *pA, Vdbe *pB){
  Vdbe tmp, *pTmp, **ppTmp;
  char *zTmp;
  assert( pA->db==pB->db );
  tmp = *pA;
  *pA = *pB;
  *pB = tmp;
  pTmp = pA->pVNext;
  pA->pVNext = pB->pVNext;
  pB->pVNext = pTmp;
  ppTmp = pA->ppVPrev;
  pA->ppVPrev = pB->ppVPrev;
  pB->ppVPrev = ppTmp;
  zTmp = pA->zSql;
  pA->zSql = pB->zSql;
  pB->zSql = zTmp;
#ifdef CAPDB_ENABLE_NORMALIZE
  zTmp = pA->zNormSql;
  pA->zNormSql = pB->zNormSql;
  pB->zNormSql = zTmp;
#endif
  pB->expmask = pA->expmask;
  pB->prepFlags = pA->prepFlags;
  memcpy(pB->aCounter, pA->aCounter, sizeof(pB->aCounter));
  pB->aCounter[CAPDB_STMTSTATUS_REPREPARE]++;
}

/*
** Resize the Vdbe.aOp array so that it is at least nOp elements larger
** than its current size. nOp is guaranteed to be less than or equal
** to 1024/sizeof(Op).
**
** If an out-of-memory error occurs while resizing the array, return
** CAPDB_NOMEM. In this case Vdbe.aOp and Vdbe.nOpAlloc remain
** unchanged (this is so that any opcodes already allocated can be
** correctly deallocated along with the rest of the Vdbe).
*/
static int growOpArray(Vdbe *v, int nOp){
  VdbeOp *pNew;
  Parse *p = v->pParse;

  /* The CAPDB_TEST_REALLOC_STRESS compile-time option is designed to force
  ** more frequent reallocs and hence provide more opportunities for
  ** simulated OOM faults.  CAPDB_TEST_REALLOC_STRESS is generally used
  ** during testing only.  With CAPDB_TEST_REALLOC_STRESS grow the op array
  ** by the minimum* amount required until the size reaches 512.  Normal
  ** operation (without CAPDB_TEST_REALLOC_STRESS) is to double the current
  ** size of the op array or add 1KB of space, whichever is smaller. */
#ifdef CAPDB_TEST_REALLOC_STRESS
  capdb_int64 nNew = (v->nOpAlloc>=512 ? 2*(capdb_int64)v->nOpAlloc
                        : (capdb_int64)v->nOpAlloc+nOp);
#else
  capdb_int64 nNew = (v->nOpAlloc ? 2*(capdb_int64)v->nOpAlloc
                        : (capdb_int64)(1024/sizeof(Op)));
  UNUSED_PARAMETER(nOp);
#endif

  /* Ensure that the size of a VDBE does not grow too large */
  if( nNew > p->db->aLimit[CAPDB_LIMIT_VDBE_OP] ){
    capdbOomFault(p->db);
    return CAPDB_NOMEM;
  }

  assert( nOp<=(int)(1024/sizeof(Op)) );
  assert( nNew>=(v->nOpAlloc+nOp) );
  pNew = capdbDbRealloc(p->db, v->aOp, nNew*sizeof(Op));
  if( pNew ){
    p->szOpAlloc = capdbDbMallocSize(p->db, pNew);
    v->nOpAlloc = p->szOpAlloc/sizeof(Op);
    v->aOp = pNew;
  }
  return (pNew ? CAPDB_OK : CAPDB_NOMEM_BKPT);
}

#ifdef CAPDB_DEBUG
/* This routine is just a convenient place to set a breakpoint that will
** fire after each opcode is inserted and displayed using
** "PRAGMA vdbe_addoptrace=on".  Parameters "pc" (program counter) and
** pOp are available to make the breakpoint conditional.
**
** Other useful labels for breakpoints include:
**   test_trace_breakpoint(pc,pOp)
**   capdbCorruptError(lineno)
**   capdbMisuseError(lineno)
**   capdbCantopenError(lineno)
*/
static void test_addop_breakpoint(int pc, Op *pOp){
  static u64 n = 0;
  (void)pc;
  (void)pOp;
  n++;
  if( n==LARGEST_UINT64 ) abort(); /* so that n is used, preventing a warning */
}
#endif

/*
** Slow paths for capdbVdbeAddOp3() and capdbVdbeAddOp4Int() for the
** unusual case when we need to increase the size of the Vdbe.aOp[] array
** before adding the new opcode.
*/
static CAPDB_NOINLINE int growOp3(Vdbe *p, int op, int p1, int p2, int p3){
  assert( p->nOpAlloc<=p->nOp );
  if( growOpArray(p, 1) ) return 1;
  assert( p->nOpAlloc>p->nOp );
  return capdbVdbeAddOp3(p, op, p1, p2, p3);
}
static CAPDB_NOINLINE int addOp4IntSlow(
  Vdbe *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  int p4              /* The P4 operand as an integer */
){
  int addr = capdbVdbeAddOp3(p, op, p1, p2, p3);
  if( p->db->mallocFailed==0 ){
    VdbeOp *pOp = &p->aOp[addr];
    pOp->p4type = P4_INT32;
    pOp->p4.i = p4;
  }
  return addr;
}


/*
** Add a new instruction to the list of instructions current in the
** VDBE.  Return the address of the new instruction.
**
** Parameters:
**
**    p               Pointer to the VDBE
**
**    op              The opcode for this instruction
**
**    p1, p2, p3, p4  Operands
*/
int capdbVdbeAddOp0(Vdbe *p, int op){
  return capdbVdbeAddOp3(p, op, 0, 0, 0);
}
int capdbVdbeAddOp1(Vdbe *p, int op, int p1){
  return capdbVdbeAddOp3(p, op, p1, 0, 0);
}
int capdbVdbeAddOp2(Vdbe *p, int op, int p1, int p2){
  return capdbVdbeAddOp3(p, op, p1, p2, 0);
}
int capdbVdbeAddOp3(Vdbe *p, int op, int p1, int p2, int p3){
  int i;
  VdbeOp *pOp;

  i = p->nOp;
  assert( p->eVdbeState==VDBE_INIT_STATE );
  assert( op>=0 && op<0xff );
  if( p->nOpAlloc<=i ){
    return growOp3(p, op, p1, p2, p3);
  }
  assert( p->aOp!=0 );
  p->nOp++;
  pOp = &p->aOp[i];
  assert( pOp!=0 );
  pOp->opcode = (u8)op;
  pOp->p5 = 0;
  pOp->p1 = p1;
  pOp->p2 = p2;
  pOp->p3 = p3;
  pOp->p4.p = 0;
  pOp->p4type = P4_NOTUSED;

  /* Replicate this logic in capdbVdbeAddOp4Int()
  ** vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv   */
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
  pOp->zComment = 0;
#endif
#if defined(CAPDB_ENABLE_STMT_SCANSTATUS) || defined(VDBE_PROFILE)
  pOp->nExec = 0;
  pOp->nCycle = 0;
#endif
#ifdef CAPDB_DEBUG
  if( p->db->flags & CAPDB_VdbeAddopTrace ){
    capdbVdbePrintOp(0, i, &p->aOp[i]);
    test_addop_breakpoint(i, &p->aOp[i]);
  }
#endif
#ifdef CAPDB_VDBE_COVERAGE
  pOp->iSrcLine = 0;
#endif
  /* ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  ** Replicate in capdbVdbeAddOp4Int() */

  return i;
}
int capdbVdbeAddOp4Int(
  Vdbe *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  int p4              /* The P4 operand as an integer */
){
  int i;
  VdbeOp *pOp;

  i = p->nOp;
  if( p->nOpAlloc<=i ){
    return addOp4IntSlow(p, op, p1, p2, p3, p4);
  }
  p->nOp++;
  pOp = &p->aOp[i];
  assert( pOp!=0 );
  pOp->opcode = (u8)op;
  pOp->p5 = 0;
  pOp->p1 = p1;
  pOp->p2 = p2;
  pOp->p3 = p3;
  pOp->p4.i = p4;
  pOp->p4type = P4_INT32;

  /* Replicate this logic in capdbVdbeAddOp3()
  ** vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv   */
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
  pOp->zComment = 0;
#endif
#if defined(CAPDB_ENABLE_STMT_SCANSTATUS) || defined(VDBE_PROFILE)
  pOp->nExec = 0;
  pOp->nCycle = 0;
#endif
#ifdef CAPDB_DEBUG
  if( p->db->flags & CAPDB_VdbeAddopTrace ){
    capdbVdbePrintOp(0, i, &p->aOp[i]);
    test_addop_breakpoint(i, &p->aOp[i]);
  }
#endif
#ifdef CAPDB_VDBE_COVERAGE
  pOp->iSrcLine = 0;
#endif
  /* ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  ** Replicate in capdbVdbeAddOp3() */

  return i;
}

/* Generate code for an unconditional jump to instruction iDest
*/
int capdbVdbeGoto(Vdbe *p, int iDest){
  return capdbVdbeAddOp3(p, OP_Goto, 0, iDest, 0);
}

/* Generate code to cause the string zStr to be loaded into
** register iDest
*/
int capdbVdbeLoadString(Vdbe *p, int iDest, const char *zStr){
  return capdbVdbeAddOp4(p, OP_String8, 0, iDest, 0, zStr, 0);
}

/*
** Generate code that initializes multiple registers to string or integer
** constants.  The registers begin with iDest and increase consecutively.
** One register is initialized for each characgter in zTypes[].  For each
** "s" character in zTypes[], the register is a string if the argument is
** not NULL, or OP_Null if the value is a null pointer.  For each "i" character
** in zTypes[], the register is initialized to an integer.
**
** If the input string does not end with "X" then an OP_ResultRow instruction
** is generated for the values inserted.
*/
void capdbVdbeMultiLoad(Vdbe *p, int iDest, const char *zTypes, ...){
  va_list ap;
  int i;
  char c;
  va_start(ap, zTypes);
  for(i=0; (c = zTypes[i])!=0; i++){
    if( c=='s' ){
      const char *z = va_arg(ap, const char*);
      capdbVdbeAddOp4(p, z==0 ? OP_Null : OP_String8, 0, iDest+i, 0, z, 0);
    }else if( c=='i' ){
      capdbVdbeAddOp2(p, OP_Integer, va_arg(ap, int), iDest+i);
    }else{
      goto skip_op_resultrow;
    }
  }
  capdbVdbeAddOp2(p, OP_ResultRow, iDest, i);
skip_op_resultrow:
  va_end(ap);
}

/*
** Add an opcode that includes the p4 value as a pointer.
*/
int capdbVdbeAddOp4(
  Vdbe *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  const char *zP4,    /* The P4 operand */
  int p4type          /* P4 operand type */
){
  int addr = capdbVdbeAddOp3(p, op, p1, p2, p3);
  capdbVdbeChangeP4(p, addr, zP4, p4type);
  return addr;
}

/*
** Add an OP_Function or OP_PureFunc opcode.
**
** The eCallCtx argument is information (typically taken from Expr.op2)
** that describes the calling context of the function.  0 means a general
** function call.  NC_IsCheck means called by a check constraint,
** NC_IdxExpr means called as part of an index expression.  NC_PartIdx
** means in the WHERE clause of a partial index.  NC_GenCol means called
** while computing a generated column value.  0 is the usual case.
*/
int capdbVdbeAddFunctionCall(
  Parse *pParse,        /* Parsing context */
  int p1,               /* Constant argument mask */
  int p2,               /* First argument register */
  int p3,               /* Register into which results are written */
  int nArg,             /* Number of argument */
  const FuncDef *pFunc, /* The function to be invoked */
  int eCallCtx          /* Calling context */
){
  Vdbe *v = pParse->pVdbe;
  int addr;
  capdb_context *pCtx;
  assert( v );
  pCtx = capdbDbMallocRawNN(pParse->db, SZ_CONTEXT(nArg));
  if( pCtx==0 ){
    assert( pParse->db->mallocFailed );
    freeEphemeralFunction(pParse->db, (FuncDef*)pFunc);
    return 0;
  }
  pCtx->pOut = 0;
  pCtx->pFunc = (FuncDef*)pFunc;
  pCtx->pVdbe = 0;
  pCtx->isError = 0;
  pCtx->argc = nArg;
  pCtx->iOp = capdbVdbeCurrentAddr(v);
  addr = capdbVdbeAddOp4(v, eCallCtx ? OP_PureFunc : OP_Function,
                           p1, p2, p3, (char*)pCtx, P4_FUNCCTX);
  capdbVdbeChangeP5(v, eCallCtx & NC_SelfRef);
  capdbMayAbort(pParse);
  return addr;
}

/*
** Add an opcode that includes the p4 value with a P4_INT64 or
** P4_REAL type.
*/
int capdbVdbeAddOp4Dup8(
  Vdbe *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  const u8 *zP4,      /* The P4 operand */
  int p4type          /* P4 operand type */
){
  char *p4copy = capdbDbMallocRawNN(capdbVdbeDb(p), 8);
  if( p4copy ) memcpy(p4copy, zP4, 8);
  return capdbVdbeAddOp4(p, op, p1, p2, p3, p4copy, p4type);
}

#ifndef CAPDB_OMIT_EXPLAIN
/*
** Return the address of the current EXPLAIN QUERY PLAN baseline.
** 0 means "none".
*/
int capdbVdbeExplainParent(Parse *pParse){
  VdbeOp *pOp;
  if( pParse->addrExplain==0 ) return 0;
  pOp = capdbVdbeGetOp(pParse->pVdbe, pParse->addrExplain);
  return pOp->p2;
}

/*
** Set a debugger breakpoint on the following routine in order to
** monitor the EXPLAIN QUERY PLAN code generation.
*/
#if defined(CAPDB_DEBUG)
void capdbExplainBreakpoint(const char *z1, const char *z2){
  (void)z1;
  (void)z2;
}
#endif

/*
** Add a new OP_Explain opcode.
**
** If the bPush flag is true, then make this opcode the parent for
** subsequent Explains until capdbVdbeExplainPop() is called.
*/
int capdbVdbeExplain(Parse *pParse, u8 bPush, const char *zFmt, ...){
  int addr = 0;
#if !defined(CAPDB_DEBUG)
  /* Always include the OP_Explain opcodes if CAPDB_DEBUG is defined.
  ** But omit them (for performance) during production builds */
  if( pParse->explain==2 || IS_STMT_SCANSTATUS(pParse->db) )
#endif
  {
    char *zMsg;
    Vdbe *v;
    va_list ap;
    int iThis;
    va_start(ap, zFmt);
    zMsg = capdbVMPrintf(pParse->db, zFmt, ap);
    va_end(ap);
    v = pParse->pVdbe;
    iThis = v->nOp;
    addr = capdbVdbeAddOp4(v, OP_Explain, iThis, pParse->addrExplain, 0,
                      zMsg, P4_DYNAMIC);
    capdbExplainBreakpoint(bPush?"PUSH":"", capdbVdbeGetLastOp(v)->p4.z);
    if( bPush){
      pParse->addrExplain = iThis;
    }
    capdbVdbeScanStatus(v, iThis, -1, -1, 0, 0);
  }
  return addr;
}

/*
** Pop the EXPLAIN QUERY PLAN stack one level.
*/
void capdbVdbeExplainPop(Parse *pParse){
  capdbExplainBreakpoint("POP", 0);
  pParse->addrExplain = capdbVdbeExplainParent(pParse);
}
#endif /* CAPDB_OMIT_EXPLAIN */

/*
** Add an OP_ParseSchema opcode.  This routine is broken out from
** capdbVdbeAddOp4() since it needs to also needs to mark all btrees
** as having been used.
**
** zWhere is a WHERE clause that defines which entries of the schema
** to reparse.  If zWhere==0, that means all entries.  p5 is a mask
** of INITFLAG_* values for the parse.
**
** In the current usage, the following are always true:
**
**     ALTER TABLE:     zWhere==0,  p5!=0
**     Otherwise:       zWhere!=0,  p5==0
**
** The zWhere string must have been obtained from capdbDbMalloc().
** This routine will take ownership of the allocated memory.
*/
void capdbVdbeAddParseSchemaOp(Vdbe *p, int iDb, char *zWhere, u16 p5){
  int j;
  assert( p5==0 || zWhere==0 );
  capdbVdbeAddOp4(p, OP_ParseSchema, iDb, 0, 0, zWhere, P4_DYNAMIC);
  capdbVdbeChangeP5(p, p5);
  for(j=0; j<p->db->nDb; j++) capdbVdbeUsesBtree(p, j);
  capdbMayAbort(p->pParse);
}

/* Insert the end of a co-routine
*/
void capdbVdbeEndCoroutine(Vdbe *v, int regYield){
  capdbVdbeAddOp1(v, OP_EndCoroutine, regYield);

  /* Clear the temporary register cache, thereby ensuring that each
  ** co-routine has its own independent set of registers, because co-routines
  ** might expect their registers to be preserved across an OP_Yield, and
  ** that could cause problems if two or more co-routines are using the same
  ** temporary register.
  */
  v->pParse->nTempReg = 0;
  v->pParse->nRangeReg = 0;
}

/*
** Create a new symbolic label for an instruction that has yet to be
** coded.  The symbolic label is really just a negative number.  The
** label can be used as the P2 value of an operation.  Later, when
** the label is resolved to a specific address, the VDBE will scan
** through its operation list and change all values of P2 which match
** the label into the resolved address.
**
** The VDBE knows that a P2 value is a label because labels are
** always negative and P2 values are suppose to be non-negative.
** Hence, a negative P2 value is a label that has yet to be resolved.
** (Later:) This is only true for opcodes that have the OPFLG_JUMP
** property.
**
** Variable usage notes:
**
**     Parse.aLabel[x]     Stores the address that the x-th label resolves
**                         into.  For testing (CAPDB_DEBUG), unresolved
**                         labels stores -1, but that is not required.
**     Parse.nLabelAlloc   Number of slots allocated to Parse.aLabel[]
**     Parse.nLabel        The *negative* of the number of labels that have
**                         been issued.  The negative is stored because
**                         that gives a performance improvement over storing
**                         the equivalent positive value.
*/
int capdbVdbeMakeLabel(Parse *pParse){
  return --pParse->nLabel;
}

/*
** Resolve label "x" to be the address of the next instruction to
** be inserted.  The parameter "x" must have been obtained from
** a prior call to capdbVdbeMakeLabel().
*/
static CAPDB_NOINLINE void resizeResolveLabel(Parse *p, Vdbe *v, int j){
  int nNewSize = 10 - p->nLabel;
  p->aLabel = capdbDbReallocOrFree(p->db, p->aLabel,
                     nNewSize*sizeof(p->aLabel[0]));
  if( p->aLabel==0 ){
    p->nLabelAlloc = 0;
  }else{
#ifdef CAPDB_DEBUG
    int i;
    for(i=p->nLabelAlloc; i<nNewSize; i++) p->aLabel[i] = -1;
#endif
    if( nNewSize>=100 && (nNewSize/100)>(p->nLabelAlloc/100) ){
      capdbProgressCheck(p);
    }
    p->nLabelAlloc = nNewSize;
    p->aLabel[j] = v->nOp;
  }
}
void capdbVdbeResolveLabel(Vdbe *v, int x){
  Parse *p = v->pParse;
  int j = ADDR(x);
  assert( v->eVdbeState==VDBE_INIT_STATE );
  assert( j<-p->nLabel );
  assert( j>=0 );
#ifdef CAPDB_DEBUG
  if( p->db->flags & CAPDB_VdbeAddopTrace ){
    printf("RESOLVE LABEL %d to %d\n", x, v->nOp);
  }
#endif
  if( p->nLabelAlloc + p->nLabel < 0 ){
    resizeResolveLabel(p,v,j);
  }else{
    assert( p->aLabel[j]==(-1) ); /* Labels may only be resolved once */
    p->aLabel[j] = v->nOp;
  }
}

/*
** Mark the VDBE as one that can only be run one time.
*/
void capdbVdbeRunOnlyOnce(Vdbe *p){
  capdbVdbeAddOp2(p, OP_Expire, 1, 1);
}

/*
** Mark the VDBE as one that can be run multiple times.
*/
void capdbVdbeReusable(Vdbe *p){
  int i;
  for(i=1; ALWAYS(i<p->nOp); i++){
    if( ALWAYS(p->aOp[i].opcode==OP_Expire) ){
      p->aOp[1].opcode = OP_Noop;
      break;
    }
  }
}

#ifdef CAPDB_DEBUG /* capdbAssertMayAbort() logic */

/*
** The following type and function are used to iterate through all opcodes
** in a Vdbe main program and each of the sub-programs (triggers) it may
** invoke directly or indirectly. It should be used as follows:
**
**   Op *pOp;
**   VdbeOpIter sIter;
**
**   memset(&sIter, 0, sizeof(sIter));
**   sIter.v = v;                            // v is of type Vdbe*
**   while( (pOp = opIterNext(&sIter)) ){
**     // Do something with pOp
**   }
**   capdbDbFree(v->db, sIter.apSub);
**
*/
typedef struct VdbeOpIter VdbeOpIter;
struct VdbeOpIter {
  Vdbe *v;                   /* Vdbe to iterate through the opcodes of */
  SubProgram **apSub;        /* Array of subprograms */
  int nSub;                  /* Number of entries in apSub */
  int iAddr;                 /* Address of next instruction to return */
  int iSub;                  /* 0 = main program, 1 = first sub-program etc. */
};
static Op *opIterNext(VdbeOpIter *p){
  Vdbe *v = p->v;
  Op *pRet = 0;
  Op *aOp;
  int nOp;

  if( p->iSub<=p->nSub ){

    if( p->iSub==0 ){
      aOp = v->aOp;
      nOp = v->nOp;
    }else{
      aOp = p->apSub[p->iSub-1]->aOp;
      nOp = p->apSub[p->iSub-1]->nOp;
    }
    assert( p->iAddr<nOp );

    pRet = &aOp[p->iAddr];
    p->iAddr++;
    if( p->iAddr==nOp ){
      p->iSub++;
      p->iAddr = 0;
    }
 
    if( pRet->p4type==P4_SUBPROGRAM ){
      i64 nByte = (1+(u64)p->nSub)*sizeof(SubProgram*);
      int j;
      for(j=0; j<p->nSub; j++){
        if( p->apSub[j]==pRet->p4.pProgram ) break;
      }
      if( j==p->nSub ){
        p->apSub = capdbDbReallocOrFree(v->db, p->apSub, nByte);
        if( !p->apSub ){
          pRet = 0;
        }else{
          p->apSub[p->nSub++] = pRet->p4.pProgram;
        }
      }
    }
  }

  return pRet;
}

/*
** Check if the program stored in the VM associated with pParse may
** throw an ABORT exception (causing the statement, but not entire transaction
** to be rolled back). This condition is true if the main program or any
** sub-programs contains any of the following:
**
**   *  OP_Halt with P1=CAPDB_CONSTRAINT and P2=OE_Abort.
**   *  OP_HaltIfNull with P1=CAPDB_CONSTRAINT and P2=OE_Abort.
**   *  OP_Destroy
**   *  OP_VUpdate
**   *  OP_VCreate
**   *  OP_VRename
**   *  OP_FkCounter with P2==0 (immediate foreign key constraint)
**   *  OP_CreateBtree/BTREE_INTKEY and OP_InitCoroutine
**      (for CREATE TABLE AS SELECT ...)
**
** Then check that the value of Parse.mayAbort is true if an
** ABORT may be thrown, or false otherwise. Return true if it does
** match, or false otherwise. This function is intended to be used as
** part of an assert statement in the compiler. Similar to:
**
**   assert( capdbVdbeAssertMayAbort(pParse->pVdbe, pParse->mayAbort) );
*/
int capdbVdbeAssertMayAbort(Vdbe *v, int mayAbort){
  int hasAbort = 0;
  int hasFkCounter = 0;
  int hasCreateTable = 0;
  int hasCreateIndex = 0;
  int hasInitCoroutine = 0;
  Op *pOp;
  VdbeOpIter sIter;

  if( v==0 ) return 0;
  memset(&sIter, 0, sizeof(sIter));
  sIter.v = v;

  while( (pOp = opIterNext(&sIter))!=0 ){
    int opcode = pOp->opcode;
    if( opcode==OP_Destroy || opcode==OP_VUpdate || opcode==OP_VRename
     || opcode==OP_VDestroy
     || opcode==OP_VCreate
     || opcode==OP_ParseSchema
     || opcode==OP_Function || opcode==OP_PureFunc
     || ((opcode==OP_Halt || opcode==OP_HaltIfNull)
      && ((pOp->p1)!=CAPDB_OK && pOp->p2==OE_Abort))
    ){
      hasAbort = 1;
      break;
    }
    if( opcode==OP_CreateBtree && pOp->p3==BTREE_INTKEY ) hasCreateTable = 1;
    if( mayAbort ){
      /* hasCreateIndex may also be set for some DELETE statements that use
      ** OP_Clear. So this routine may end up returning true in the case
      ** where a "DELETE FROM tbl" has a statement-journal but does not
      ** require one. This is not so bad - it is an inefficiency, not a bug. */
      if( opcode==OP_CreateBtree && pOp->p3==BTREE_BLOBKEY ) hasCreateIndex = 1;
      if( opcode==OP_Clear ) hasCreateIndex = 1;
    }
    if( opcode==OP_InitCoroutine ) hasInitCoroutine = 1;
#ifndef CAPDB_OMIT_FOREIGN_KEY
    if( opcode==OP_FkCounter && pOp->p1==0 && pOp->p2==1 ){
      hasFkCounter = 1;
    }
#endif
  }
  capdbDbFree(v->db, sIter.apSub);

  /* Return true if hasAbort==mayAbort. Or if a malloc failure occurred.
  ** If malloc failed, then the while() loop above may not have iterated
  ** through all opcodes and hasAbort may be set incorrectly. Return
  ** true for this case to prevent the assert() in the callers frame
  ** from failing.  */
  return ( v->db->mallocFailed || hasAbort==mayAbort || hasFkCounter
        || (hasCreateTable && hasInitCoroutine) || hasCreateIndex
  );
}
#endif /* CAPDB_DEBUG - the capdbAssertMayAbort() function */

#ifdef CAPDB_DEBUG
/*
** Increment the nWrite counter in the VDBE if the cursor is not an
** ephemeral cursor, or if the cursor argument is NULL.
*/
void capdbVdbeIncrWriteCounter(Vdbe *p, VdbeCursor *pC){
  if( pC==0
   || (pC->eCurType!=CURTYPE_SORTER
       && pC->eCurType!=CURTYPE_PSEUDO
       && !pC->isEphemeral)
  ){
    p->nWrite++;
  }
}
#endif

#ifdef CAPDB_DEBUG
/*
** Assert if an Abort at this point in time might result in a corrupt
** database.
*/
void capdbVdbeAssertAbortable(Vdbe *p){
  assert( p->nWrite==0 || p->usesStmtJournal );
}
#endif

/*
** This routine is called after all opcodes have been inserted.  It loops
** through all the opcodes and fixes up some details.
**
** (1) For each jump instruction with a negative P2 value (a label)
**     resolve the P2 value to an actual address.
**
** (2) Compute the maximum number of arguments used by the xUpdate/xFilter
**     methods of any virtual table and store that value in *pMaxVtabArgs.
**
** (3) Update the Vdbe.readOnly and Vdbe.bIsReader flags to accurately
**     indicate what the prepared statement actually does.
**
** (4) (discontinued)
**
** (5) Reclaim the memory allocated for storing labels.
**
** This routine will only function correctly if the mkopcodeh.tcl generator
** script numbers the opcodes correctly.  Changes to this routine must be
** coordinated with changes to mkopcodeh.tcl.
*/
static void resolveP2Values(Vdbe *p, int *pMaxVtabArgs){
  int nMaxVtabArgs = *pMaxVtabArgs;
  Op *pOp;
  Parse *pParse = p->pParse;
  int *aLabel = pParse->aLabel;

  assert( pParse->db->mallocFailed==0 ); /* tag-20230419-1 */
  p->readOnly = 1;
  p->bIsReader = 0;
  pOp = &p->aOp[p->nOp-1];
  assert( p->aOp[0].opcode==OP_Init );
  while( 1 /* Loop terminates when it reaches the OP_Init opcode */ ){
    /* Only JUMP opcodes and the short list of special opcodes in the switch
    ** below need to be considered.  The mkopcodeh.tcl generator script groups
    ** all these opcodes together near the front of the opcode list.  Skip
    ** any opcode that does not need processing by virtual of the fact that
    ** it is larger than CAPDB_MX_JUMP_OPCODE, as a performance optimization.
    */
    if( pOp->opcode<=CAPDB_MX_JUMP_OPCODE ){
      /* NOTE: Be sure to update mkopcodeh.tcl when adding or removing
      ** cases from this switch! */
      switch( pOp->opcode ){
        case OP_Transaction: {
          if( pOp->p2!=0 ) p->readOnly = 0;
          /* no break */ deliberate_fall_through
        }
        case OP_AutoCommit:
        case OP_Savepoint: {
          p->bIsReader = 1;
          break;
        }
#ifndef CAPDB_OMIT_WAL
        case OP_Checkpoint:
#endif
        case OP_Vacuum:
        case OP_JournalMode: {
          p->readOnly = 0;
          p->bIsReader = 1;
          break;
        }
        case OP_Init: {
          assert( pOp->p2>=0 );
          goto resolve_p2_values_loop_exit;
        }
#ifndef CAPDB_OMIT_VIRTUALTABLE
        case OP_VUpdate: {
          if( pOp->p2>nMaxVtabArgs ) nMaxVtabArgs = pOp->p2;
          break;
        }
        case OP_VFilter: {
          int n;
          /* The instruction immediately prior to VFilter will be an
          ** OP_Integer that sets the "argc" value for the VFilter.  See
          ** the code where OP_VFilter is generated at tag-20250207a. */
          assert( (pOp - p->aOp) >= 3 );
          assert( pOp[-1].opcode==OP_Integer );
          assert( pOp[-1].p2==pOp->p3+1 );
          n = pOp[-1].p1;
          if( n>nMaxVtabArgs ) nMaxVtabArgs = n;
          /* Fall through into the default case */
          /* no break */ deliberate_fall_through
        }
#endif
        default: {
          if( pOp->p2<0 ){
            /* The mkopcodeh.tcl script has so arranged things that the only
            ** non-jump opcodes less than CAPDB_MX_JUMP_CODE are guaranteed to
            ** have non-negative values for P2. */
            assert( (capdbOpcodeProperty[pOp->opcode] & OPFLG_JUMP)!=0 );
            assert( ADDR(pOp->p2)<-pParse->nLabel );
            assert( aLabel!=0 );  /* True because of tag-20230419-1 */
            pOp->p2 = aLabel[ADDR(pOp->p2)];
          }

          /* OPFLG_JUMP opcodes never have P2==0, though OPFLG_JUMP0 opcodes
          ** might */
          assert( pOp->p2>0 
                  || (capdbOpcodeProperty[pOp->opcode] & OPFLG_JUMP0)!=0 );

          /* Jumps never go off the end of the bytecode array */
          assert( pOp->p2<p->nOp
                  || (capdbOpcodeProperty[pOp->opcode] & OPFLG_JUMP)==0 );
          break;
        }
      }
      /* The mkopcodeh.tcl script has so arranged things that the only
      ** non-jump opcodes less than CAPDB_MX_JUMP_CODE are guaranteed to
      ** have non-negative values for P2. */
      assert( (capdbOpcodeProperty[pOp->opcode]&OPFLG_JUMP)==0 || pOp->p2>=0);
    }
    assert( pOp>p->aOp );   
    pOp--;
  }
resolve_p2_values_loop_exit:
  if( aLabel ){
    capdbDbNNFreeNN(p->db, pParse->aLabel);
    pParse->aLabel = 0;
  }
  pParse->nLabel = 0;
  *pMaxVtabArgs = nMaxVtabArgs;
  assert( p->bIsReader!=0 || DbMaskAllZero(p->btreeMask) );
}

#ifdef CAPDB_DEBUG
/*
** Check to see if a subroutine contains a jump to a location outside of
** the subroutine.  If a jump outside the subroutine is detected, add code
** that will cause the program to halt with an error message.
**
** The subroutine consists of opcodes between iFirst and iLast.  Jumps to
** locations within the subroutine are acceptable.  iRetReg is a register
** that contains the return address.  Jumps to outside the range of iFirst
** through iLast are also acceptable as long as the jump destination is
** an OP_Return to iReturnAddr.
**
** A jump to an unresolved label means that the jump destination will be
** beyond the current address.  That is normally a jump to an early
** termination and is consider acceptable.
**
** This routine only runs during debug builds.  The purpose is (of course)
** to detect invalid escapes out of a subroutine.  The OP_Halt opcode
** is generated rather than an assert() or other error, so that ".eqp full"
** will still work to show the original bytecode, to aid in debugging.
*/
void capdbVdbeNoJumpsOutsideSubrtn(
  Vdbe *v,          /* The byte-code program under construction */
  int iFirst,       /* First opcode of the subroutine */
  int iLast,        /* Last opcode of the subroutine */
  int iRetReg       /* Subroutine return address register */
){
  VdbeOp *pOp;
  Parse *pParse;
  int i;
  capdb_str *pErr = 0;
  assert( v!=0 );
  pParse = v->pParse;
  assert( pParse!=0 );
  if( pParse->nErr ) return;
  assert( iLast>=iFirst );
  assert( iLast<v->nOp );
  pOp = &v->aOp[iFirst];
  for(i=iFirst; i<=iLast; i++, pOp++){
    if( (capdbOpcodeProperty[pOp->opcode] & OPFLG_JUMP)!=0 ){
      int iDest = pOp->p2;   /* Jump destination */
      if( iDest==0 ) continue;
      if( pOp->opcode==OP_Gosub ) continue;
      if( pOp->p3==20230325 && pOp->opcode==OP_NotNull ){
        /* This is a deliberately taken illegal branch.  tag-20230325-2 */
        continue;
      }
      if( iDest<0 ){
        int j = ADDR(iDest);
        assert( j>=0 );
        if( j>=-pParse->nLabel || pParse->aLabel[j]<0 ){
          continue;
        }
        iDest = pParse->aLabel[j];
      }
      if( iDest<iFirst || iDest>iLast ){
        int j = iDest;
        for(; j<v->nOp; j++){
          VdbeOp *pX = &v->aOp[j];
          if( pX->opcode==OP_Return ){
            if( pX->p1==iRetReg ) break;
            continue;
          }
          if( pX->opcode==OP_Noop ) continue;
          if( pX->opcode==OP_Explain ) continue;
          if( pErr==0 ){
            pErr = capdb_str_new(0);
          }else{
            capdb_str_appendchar(pErr, 1, '\n');
          }
          capdb_str_appendf(pErr,
              "Opcode at %d jumps to %d which is outside the "
              "subroutine at %d..%d",
              i, iDest, iFirst, iLast);
          break;
        }
      }
    }
  }
  if( pErr ){
    char *zErr = capdb_str_finish(pErr);
    capdbVdbeAddOp4(v, OP_Halt, CAPDB_INTERNAL, OE_Abort, 0, zErr, 0);
    capdb_free(zErr);
    capdbMayAbort(pParse);
  }
}
#endif /* CAPDB_DEBUG */

/*
** Return the address of the next instruction to be inserted.
*/
int capdbVdbeCurrentAddr(Vdbe *p){
  assert( p->eVdbeState==VDBE_INIT_STATE );
  return p->nOp;
}

/*
** Verify that at least N opcode slots are available in p without
** having to malloc for more space (except when compiled using
** CAPDB_TEST_REALLOC_STRESS).  This interface is used during testing
** to verify that certain calls to capdbVdbeAddOpList() can never
** fail due to a OOM fault and hence that the return value from
** capdbVdbeAddOpList() will always be non-NULL.
*/
#if defined(CAPDB_DEBUG) && !defined(CAPDB_TEST_REALLOC_STRESS)
void capdbVdbeVerifyNoMallocRequired(Vdbe *p, int N){
  assert( p->nOp + N <= p->nOpAlloc );
}
#endif

/*
** Verify that the VM passed as the only argument does not contain
** an OP_ResultRow opcode. Fail an assert() if it does. This is used
** by code in pragma.c to ensure that the implementation of certain
** pragmas comports with the flags specified in the mkpragmatab.tcl
** script.
*/
#if defined(CAPDB_DEBUG) && !defined(CAPDB_TEST_REALLOC_STRESS)
void capdbVdbeVerifyNoResultRow(Vdbe *p){
  int i;
  for(i=0; i<p->nOp; i++){
    assert( p->aOp[i].opcode!=OP_ResultRow );
  }
}
#endif

/*
** Generate code (a single OP_Abortable opcode) that will
** verify that the VDBE program can safely call Abort in the current
** context.
*/
#if defined(CAPDB_DEBUG)
void capdbVdbeVerifyAbortable(Vdbe *p, int onError){
  if( onError==OE_Abort ) capdbVdbeAddOp0(p, OP_Abortable);
}
#endif

/*
** This function returns a pointer to the array of opcodes associated with
** the Vdbe passed as the first argument. It is the callers responsibility
** to arrange for the returned array to be eventually freed using the
** vdbeFreeOpArray() function.
**
** Before returning, *pnOp is set to the number of entries in the returned
** array. Also, *pnMaxArg is set to the larger of its current value and
** the number of entries in the Vdbe.apArg[] array required to execute the
** returned program.
*/
VdbeOp *capdbVdbeTakeOpArray(Vdbe *p, int *pnOp, int *pnMaxArg){
  VdbeOp *aOp = p->aOp;
  assert( aOp && !p->db->mallocFailed );

  /* Check that capdbVdbeUsesBtree() was not called on this VM */
  assert( DbMaskAllZero(p->btreeMask) );

  resolveP2Values(p, pnMaxArg);
  *pnOp = p->nOp;
  p->aOp = 0;
  return aOp;
}

/*
** Add a whole list of operations to the operation stack.  Return a
** pointer to the first operation inserted.
**
** Non-zero P2 arguments to jump instructions are automatically adjusted
** so that the jump target is relative to the first operation inserted.
*/
VdbeOp *capdbVdbeAddOpList(
  Vdbe *p,                     /* Add opcodes to the prepared statement */
  int nOp,                     /* Number of opcodes to add */
  VdbeOpList const *aOp,       /* The opcodes to be added */
  int iLineno                  /* Source-file line number of first opcode */
){
  int i;
  VdbeOp *pOut, *pFirst;
  assert( nOp>0 );
  assert( p->eVdbeState==VDBE_INIT_STATE );
  if( p->nOp + nOp > p->nOpAlloc && growOpArray(p, nOp) ){
    return 0;
  }
  pFirst = pOut = &p->aOp[p->nOp];
  for(i=0; i<nOp; i++, aOp++, pOut++){
    pOut->opcode = aOp->opcode;
    pOut->p1 = aOp->p1;
    pOut->p2 = aOp->p2;
    assert( aOp->p2>=0 );
    if( (capdbOpcodeProperty[aOp->opcode] & OPFLG_JUMP)!=0 && aOp->p2>0 ){
      pOut->p2 += p->nOp;
    }
    pOut->p3 = aOp->p3;
    pOut->p4type = P4_NOTUSED;
    pOut->p4.p = 0;
    pOut->p5 = 0;
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
    pOut->zComment = 0;
#endif
#ifdef CAPDB_VDBE_COVERAGE
    pOut->iSrcLine = iLineno+i;
#else
    (void)iLineno;
#endif
#ifdef CAPDB_DEBUG
    if( p->db->flags & CAPDB_VdbeAddopTrace ){
      capdbVdbePrintOp(0, i+p->nOp, &p->aOp[i+p->nOp]);
    }
#endif
  }
  p->nOp += nOp;
  return pFirst;
}

#if defined(CAPDB_ENABLE_STMT_SCANSTATUS)
/*
** Add an entry to the array of counters managed by capdb_stmt_scanstatus().
*/
void capdbVdbeScanStatus(
  Vdbe *p,                        /* VM to add scanstatus() to */
  int addrExplain,                /* Address of OP_Explain (or 0) */
  int addrLoop,                   /* Address of loop counter */
  int addrVisit,                  /* Address of rows visited counter */
  LogEst nEst,                    /* Estimated number of output rows */
  const char *zName               /* Name of table or index being scanned */
){
  if( IS_STMT_SCANSTATUS(p->db) ){
    i64 nByte = (1+(i64)p->nScan) * sizeof(ScanStatus);
    ScanStatus *aNew;
    aNew = (ScanStatus*)capdbDbRealloc(p->db, p->aScan, nByte);
    if( aNew ){
      ScanStatus *pNew = &aNew[p->nScan++];
      memset(pNew, 0, sizeof(ScanStatus));
      pNew->addrExplain = addrExplain;
      pNew->addrLoop = addrLoop;
      pNew->addrVisit = addrVisit;
      pNew->nEst = nEst;
      pNew->zName = capdbDbStrDup(p->db, zName);
      p->aScan = aNew;
    }
  }
}

/*
** Add the range of instructions from addrStart to addrEnd (inclusive) to
** the set of those corresponding to the capdb_stmt_scanstatus() counters
** associated with the OP_Explain instruction at addrExplain. The
** sum of the capdbHwtime() values for each of these instructions
** will be returned for CAPDB_SCANSTAT_NCYCLE requests.
*/
void capdbVdbeScanStatusRange(
  Vdbe *p,
  int addrExplain,
  int addrStart,
  int addrEnd
){
  if( IS_STMT_SCANSTATUS(p->db) ){
    ScanStatus *pScan = 0;
    int ii;
    for(ii=p->nScan-1; ii>=0; ii--){
      pScan = &p->aScan[ii];
      if( pScan->addrExplain==addrExplain ) break;
      pScan = 0;
    }
    if( pScan ){
      if( addrEnd<0 ) addrEnd = capdbVdbeCurrentAddr(p)-1;
      for(ii=0; ii<ArraySize(pScan->aAddrRange); ii+=2){
        if( pScan->aAddrRange[ii]==0 ){
          pScan->aAddrRange[ii] = addrStart;
          pScan->aAddrRange[ii+1] = addrEnd;
          break;
        }
      }
    }
  }
}

/*
** Set the addresses for the CAPDB_SCANSTAT_NLOOP and CAPDB_SCANSTAT_NROW
** counters for the query element associated with the OP_Explain at
** addrExplain.
*/
void capdbVdbeScanStatusCounters(
  Vdbe *p,
  int addrExplain,
  int addrLoop,
  int addrVisit
){
  if( IS_STMT_SCANSTATUS(p->db) ){
    ScanStatus *pScan = 0;
    int ii;
    for(ii=p->nScan-1; ii>=0; ii--){
      pScan = &p->aScan[ii];
      if( pScan->addrExplain==addrExplain ) break;
      pScan = 0;
    }
    if( pScan ){
      if( addrLoop>0 ) pScan->addrLoop = addrLoop;
      if( addrVisit>0 ) pScan->addrVisit = addrVisit;
    }
  }
}
#endif /* defined(CAPDB_ENABLE_STMT_SCANSTATUS) */


/*
** Change the value of the opcode, or P1, P2, P3, or P5 operands
** for a specific instruction.
*/
void capdbVdbeChangeOpcode(Vdbe *p, int addr, u8 iNewOpcode){
  assert( addr>=0 );
  capdbVdbeGetOp(p,addr)->opcode = iNewOpcode;
}
void capdbVdbeChangeP1(Vdbe *p, int addr, int val){
  assert( addr>=0 );
  capdbVdbeGetOp(p,addr)->p1 = val;
}
void capdbVdbeChangeP2(Vdbe *p, int addr, int val){
  assert( addr>=0 || p->db->mallocFailed );
  capdbVdbeGetOp(p,addr)->p2 = val;
}
void capdbVdbeChangeP3(Vdbe *p, int addr, int val){
  assert( addr>=0 );
  capdbVdbeGetOp(p,addr)->p3 = val;
}
void capdbVdbeChangeP5(Vdbe *p, u16 p5){
  assert( p->nOp>0 || p->db->mallocFailed );
  if( p->nOp>0 ) p->aOp[p->nOp-1].p5 = p5;
}

/*
** If the previous opcode is an OP_Column that delivers results
** into register iDest, then add the OPFLAG_TYPEOFARG flag to that
** opcode.
*/
void capdbVdbeTypeofColumn(Vdbe *p, int iDest){
  VdbeOp *pOp = capdbVdbeGetLastOp(p);
#ifdef CAPDB_DEBUG
  while( pOp->opcode==OP_ReleaseReg ) pOp--;
#endif
  if( pOp->p3==iDest && pOp->opcode==OP_Column ){
    pOp->p5 |= OPFLAG_TYPEOFARG;
  }
}

/*
** Change the P2 operand of instruction addr so that it points to
** the address of the next instruction to be coded.
*/
void capdbVdbeJumpHere(Vdbe *p, int addr){
  capdbVdbeChangeP2(p, addr, p->nOp);
}

/*
** Change the P2 operand of the jump instruction at addr so that
** the jump lands on the next opcode.  Or if the jump instruction was
** the previous opcode (and is thus a no-op) then simply back up
** the next instruction counter by one slot so that the jump is
** overwritten by the next inserted opcode.
**
** This routine is an optimization of capdbVdbeJumpHere() that
** strives to omit useless byte-code like this:
**
**        7   Once 0 8 0
**        8   ...
*/
void capdbVdbeJumpHereOrPopInst(Vdbe *p, int addr){
  if( addr==p->nOp-1 ){
    assert( p->aOp[addr].opcode==OP_Once
         || p->aOp[addr].opcode==OP_If
         || p->aOp[addr].opcode==OP_FkIfZero );
    assert( p->aOp[addr].p4type==0 );
#ifdef CAPDB_VDBE_COVERAGE
    capdbVdbeGetLastOp(p)->iSrcLine = 0;  /* Erase VdbeCoverage() macros */
#endif
    p->nOp--;
  }else{
    capdbVdbeChangeP2(p, addr, p->nOp);
  }
}


/*
** If the input FuncDef structure is ephemeral, then free it.  If
** the FuncDef is not ephemeral, then do nothing.
*/
static void freeEphemeralFunction(capdb *db, FuncDef *pDef){
  assert( db!=0 );
  if( (pDef->funcFlags & CAPDB_FUNC_EPHEM)!=0 ){
    capdbDbNNFreeNN(db, pDef);
  }
}

/*
** Delete a P4 value if necessary.
*/
static CAPDB_NOINLINE void freeP4Mem(capdb *db, Mem *p){
  if( p->szMalloc ) capdbDbFree(db, p->zMalloc);
  capdbDbNNFreeNN(db, p);
}
static CAPDB_NOINLINE void freeP4FuncCtx(capdb *db, capdb_context *p){
  assert( db!=0 );
  freeEphemeralFunction(db, p->pFunc);
  capdbDbNNFreeNN(db, p);
}
static void freeP4(capdb *db, int p4type, void *p4){
  assert( db );
  switch( p4type ){
    case P4_FUNCCTX: {
      freeP4FuncCtx(db, (capdb_context*)p4);
      break;
    }
    case P4_REAL:
    case P4_INT64:
    case P4_DYNAMIC:
    case P4_INTARRAY: {
      if( p4 ) capdbDbNNFreeNN(db, p4);
      break;
    }
    case P4_KEYINFO: {
      if( db->pnBytesFreed==0 ) capdbKeyInfoUnref((KeyInfo*)p4);
      break;
    }
#ifdef CAPDB_ENABLE_CURSOR_HINTS
    case P4_EXPR: {
      capdbExprDelete(db, (Expr*)p4);
      break;
    }
#endif
    case P4_FUNCDEF: {
      freeEphemeralFunction(db, (FuncDef*)p4);
      break;
    }
    case P4_MEM: {
      if( db->pnBytesFreed==0 ){
        capdbValueFree((capdb_value*)p4);
      }else{
        freeP4Mem(db, (Mem*)p4);
      }
      break;
    }
    case P4_VTAB : {
      if( db->pnBytesFreed==0 ) capdbVtabUnlock((VTable *)p4);
      break;
    }
    case P4_TABLEREF: {
      if( db->pnBytesFreed==0 ) capdbDeleteTable(db, (Table*)p4);
      break;
    }
    case P4_SUBRTNSIG: {
      SubrtnSig *pSig = (SubrtnSig*)p4;
      capdbDbFree(db, pSig->zAff);
      capdbDbFree(db, pSig);
      break;
    }
  }
}

/*
** Free the space allocated for aOp and any p4 values allocated for the
** opcodes contained within. If aOp is not NULL it is assumed to contain
** nOp entries.
*/
static void vdbeFreeOpArray(capdb *db, Op *aOp, int nOp){
  assert( nOp>=0 );
  assert( db!=0 );
  if( aOp ){
    Op *pOp = &aOp[nOp-1];
    while(1){  /* Exit via break */
      if( pOp->p4type <= P4_FREE_IF_LE ) freeP4(db, pOp->p4type, pOp->p4.p);
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
      capdbDbFree(db, pOp->zComment);
#endif    
      if( pOp==aOp ) break;
      pOp--;
    }
    capdbDbNNFreeNN(db, aOp);
  }
}

/*
** Link the SubProgram object passed as the second argument into the linked
** list at Vdbe.pSubProgram. This list is used to delete all sub-program
** objects when the VM is no longer required.
*/
void capdbVdbeLinkSubProgram(Vdbe *pVdbe, SubProgram *p){
  p->pNext = pVdbe->pProgram;
  pVdbe->pProgram = p;
}

/*
** Return true if the given Vdbe has any SubPrograms.
*/
int capdbVdbeHasSubProgram(Vdbe *pVdbe){
  return pVdbe->pProgram!=0;
}

/*
** Change the opcode at addr into OP_Noop
*/
int capdbVdbeChangeToNoop(Vdbe *p, int addr){
  VdbeOp *pOp;
  if( p->db->mallocFailed ) return 0;
  assert( addr>=0 && addr<p->nOp );
  pOp = &p->aOp[addr];
  freeP4(p->db, pOp->p4type, pOp->p4.p);
  pOp->p4type = P4_NOTUSED;
  pOp->p4.z = 0;
  pOp->opcode = OP_Noop;
  return 1;
}

/*
** If the last opcode is "op" and it is not a jump destination,
** then remove it.  Return true if and only if an opcode was removed.
*/
int capdbVdbeDeletePriorOpcode(Vdbe *p, u8 op){
  if( p->nOp>0 && p->aOp[p->nOp-1].opcode==op ){
    return capdbVdbeChangeToNoop(p, p->nOp-1);
  }else{
    return 0;
  }
}

#ifdef CAPDB_DEBUG
/*
** Generate an OP_ReleaseReg opcode to indicate that a range of
** registers, except any identified by mask, are no longer in use.
*/
void capdbVdbeReleaseRegisters(
  Parse *pParse,       /* Parsing context */
  int iFirst,          /* Index of first register to be released */
  int N,               /* Number of registers to release */
  u32 mask,            /* Mask of registers to NOT release */
  int bUndefine        /* If true, mark registers as undefined */
){
  if( N==0 || OptimizationDisabled(pParse->db, CAPDB_ReleaseReg) ) return;
  assert( pParse->pVdbe );
  assert( iFirst>=1 );
  assert( iFirst+N-1<=pParse->nMem );
  if( N<=31 && mask!=0 ){
    while( N>0 && (mask&1)!=0 ){
      mask >>= 1;
      iFirst++;
      N--;
    }
    while( N>0 && N<=32 && (mask & MASKBIT32(N-1))!=0 ){
      mask &= ~MASKBIT32(N-1);
      N--;
    }
  }
  if( N>0 ){
    capdbVdbeAddOp3(pParse->pVdbe, OP_ReleaseReg, iFirst, N, *(int*)&mask);
    if( bUndefine ) capdbVdbeChangeP5(pParse->pVdbe, 1);
  }
}
#endif /* CAPDB_DEBUG */

/*
** Change the value of the P4 operand for a specific instruction.
** This routine is useful when a large program is loaded from a
** static array using capdbVdbeAddOpList but we want to make a
** few minor changes to the program.
**
** If n>=0 then the P4 operand is dynamic, meaning that a copy of
** the string is made into memory obtained from capdb_malloc().
** A value of n==0 means copy bytes of zP4 up to and including the
** first null byte.  If n>0 then copy n+1 bytes of zP4.
**
** Other values of n (P4_STATIC, P4_COLLSEQ etc.) indicate that zP4 points
** to a string or structure that is guaranteed to exist for the lifetime of
** the Vdbe. In these cases we can just copy the pointer.
**
** If addr<0 then change P4 on the most recently inserted instruction.
*/
static void CAPDB_NOINLINE vdbeChangeP4Full(
  Vdbe *p,
  Op *pOp,
  const char *zP4,
  int n
){
  if( pOp->p4type ){
    assert( pOp->p4type > P4_FREE_IF_LE );
    pOp->p4type = 0;
    pOp->p4.p = 0;
  }
  if( n<0 ){
    capdbVdbeChangeP4(p, (int)(pOp - p->aOp), zP4, n);
  }else{
    if( n==0 ) n = capdbStrlen30(zP4);
    pOp->p4.z = capdbDbStrNDup(p->db, zP4, n);
    pOp->p4type = P4_DYNAMIC;
  }
}
void capdbVdbeChangeP4(Vdbe *p, int addr, const char *zP4, int n){
  Op *pOp;
  capdb *db;
  assert( p!=0 );
  db = p->db;
  assert( p->eVdbeState==VDBE_INIT_STATE );
  assert( p->aOp!=0 || db->mallocFailed );
  if( db->mallocFailed ){
    if( n!=P4_VTAB ) freeP4(db, n, (void*)*(char**)&zP4);
    return;
  }
  assert( p->nOp>0 );
  assert( addr<p->nOp );
  if( addr<0 ){
    addr = p->nOp - 1;
  }
  pOp = &p->aOp[addr];
  if( n>=0 || pOp->p4type ){
    vdbeChangeP4Full(p, pOp, zP4, n);
    return;
  }
  if( n==P4_INT32 ){
    /* Note: this cast is safe, because the origin data point was an int
    ** that was cast to a (const char *). */
    pOp->p4.i = CAPDB_PTR_TO_INT(zP4);
    pOp->p4type = P4_INT32;
  }else if( zP4!=0 ){
    assert( n<0 );
    pOp->p4.p = (void*)zP4;
    pOp->p4type = (signed char)n;
    if( n==P4_VTAB ) capdbVtabLock((VTable*)zP4);
  }
}

/*
** Change the P4 operand of the most recently coded instruction
** to the value defined by the arguments.  This is a high-speed
** version of capdbVdbeChangeP4().
**
** The P4 operand must not have been previously defined.  And the new
** P4 must not be P4_INT32.  Use capdbVdbeChangeP4() in either of
** those cases.
*/
void capdbVdbeAppendP4(Vdbe *p, void *pP4, int n){
  VdbeOp *pOp;
  assert( n!=P4_INT32 && n!=P4_VTAB );
  assert( n<=0 );
  if( p->db->mallocFailed ){
    freeP4(p->db, n, pP4);
  }else{
    assert( pP4!=0 || n==P4_DYNAMIC );
    assert( p->nOp>0 );
    pOp = &p->aOp[p->nOp-1];
    assert( pOp->p4type==P4_NOTUSED );
    pOp->p4type = n;
    pOp->p4.p = pP4;
  }
}

/*
** Set the P4 on the most recently added opcode to the KeyInfo for the
** index given.
*/
void capdbVdbeSetP4KeyInfo(Parse *pParse, Index *pIdx){
  Vdbe *v = pParse->pVdbe;
  KeyInfo *pKeyInfo;
  assert( v!=0 );
  assert( pIdx!=0 );
  pKeyInfo = capdbKeyInfoOfIndex(pParse, pIdx);
  if( pKeyInfo ) capdbVdbeAppendP4(v, pKeyInfo, P4_KEYINFO);
}

#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
/*
** Change the comment on the most recently coded instruction.  Or
** insert a No-op and add the comment to that new instruction.  This
** makes the code easier to read during debugging.  None of this happens
** in a production build.
*/
static void vdbeVComment(Vdbe *p, const char *zFormat, va_list ap){
  assert( p->nOp>0 || p->aOp==0 );
  assert( p->aOp==0 || p->aOp[p->nOp-1].zComment==0 || p->pParse->nErr>0 );
  if( p->nOp ){
    assert( p->aOp );
    capdbDbFree(p->db, p->aOp[p->nOp-1].zComment);
    p->aOp[p->nOp-1].zComment = capdbVMPrintf(p->db, zFormat, ap);
  }
}
void capdbVdbeComment(Vdbe *p, const char *zFormat, ...){
  va_list ap;
  if( p ){
    va_start(ap, zFormat);
    vdbeVComment(p, zFormat, ap);
    va_end(ap);
  }
}
void capdbVdbeNoopComment(Vdbe *p, const char *zFormat, ...){
  va_list ap;
  if( p ){
    capdbVdbeAddOp0(p, OP_Noop);
    va_start(ap, zFormat);
    vdbeVComment(p, zFormat, ap);
    va_end(ap);
  }
}
#endif  /* NDEBUG */

#ifdef CAPDB_VDBE_COVERAGE
/*
** Set the value if the iSrcLine field for the previously coded instruction.
*/
void capdbVdbeSetLineNumber(Vdbe *v, int iLine){
  capdbVdbeGetLastOp(v)->iSrcLine = iLine;
}
#endif /* CAPDB_VDBE_COVERAGE */

/*
** Return the opcode for a given address.  The address must be non-negative.
** See capdbVdbeGetLastOp() to get the most recently added opcode.
**
** If a memory allocation error has occurred prior to the calling of this
** routine, then a pointer to a dummy VdbeOp will be returned.  That opcode
** is readable but not writable, though it is cast to a writable value.
** The return of a dummy opcode allows the call to continue functioning
** after an OOM fault without having to check to see if the return from
** this routine is a valid pointer.  But because the dummy.opcode is 0,
** dummy will never be written to.  This is verified by code inspection and
** by running with Valgrind.
*/
VdbeOp *capdbVdbeGetOp(Vdbe *p, int addr){
  /* C89 specifies that the constant "dummy" will be initialized to all
  ** zeros, which is correct.  MSVC generates a warning, nevertheless. */
  static VdbeOp dummy;  /* Ignore the MSVC warning about no initializer */
  assert( p->eVdbeState==VDBE_INIT_STATE );
  assert( (addr>=0 && addr<p->nOp) || p->db->mallocFailed );
  if( p->db->mallocFailed ){
    return (VdbeOp*)&dummy;
  }else{
    return &p->aOp[addr];
  }
}

/* Return the most recently added opcode
*/
VdbeOp *capdbVdbeGetLastOp(Vdbe *p){
  return capdbVdbeGetOp(p, p->nOp - 1);
}

#if defined(CAPDB_ENABLE_EXPLAIN_COMMENTS)
/*
** Return an integer value for one of the parameters to the opcode pOp
** determined by character c.
*/
static int translateP(char c, const Op *pOp){
  if( c=='1' ) return pOp->p1;
  if( c=='2' ) return pOp->p2;
  if( c=='3' ) return pOp->p3;
  if( c=='4' ) return pOp->p4.i;
  return pOp->p5;
}

/*
** Compute a string for the "comment" field of a VDBE opcode listing.
**
** The Synopsis: field in comments in the vdbe.c source file gets converted
** to an extra string that is appended to the capdbOpcodeName().  In the
** absence of other comments, this synopsis becomes the comment on the opcode.
** Some translation occurs:
**
**       "PX"      ->  "r[X]"
**       "PX@PY"   ->  "r[X..X+Y-1]"  or "r[x]" if y is 0 or 1
**       "PX@PY+1" ->  "r[X..X+Y]"    or "r[x]" if y is 0
**       "PY..PY"  ->  "r[X..Y]"      or "r[x]" if y<=x
*/
char *capdbVdbeDisplayComment(
  capdb *db,       /* Optional - Oom error reporting only */
  const Op *pOp,     /* The opcode to be commented */
  const char *zP4    /* Previously obtained value for P4 */
){
  const char *zOpName;
  const char *zSynopsis;
  int nOpName;
  int ii;
  char zAlt[50];
  StrAccum x;

  capdbStrAccumInit(&x, 0, 0, 0, CAPDB_MAX_LENGTH);
  zOpName = capdbOpcodeName(pOp->opcode);
  nOpName = capdbStrlen30(zOpName);
  if( zOpName[nOpName+1] ){
    int seenCom = 0;
    char c;
    zSynopsis = zOpName + nOpName + 1;
    if( strncmp(zSynopsis,"IF ",3)==0 ){
      capdb_snprintf(sizeof(zAlt), zAlt, "if %s goto P2", zSynopsis+3);
      zSynopsis = zAlt;
    }
    for(ii=0; (c = zSynopsis[ii])!=0; ii++){
      if( c=='P' ){
        c = zSynopsis[++ii];
        if( c=='4' ){
          capdb_str_appendall(&x, zP4);
        }else if( c=='X' ){
          if( pOp->zComment && pOp->zComment[0] ){
            capdb_str_appendall(&x, pOp->zComment);
            seenCom = 1;
            break;
          }
        }else{
          int v1 = translateP(c, pOp);
          int v2;
          if( strncmp(zSynopsis+ii+1, "@P", 2)==0 ){
            ii += 3;
            v2 = translateP(zSynopsis[ii], pOp);
            if( strncmp(zSynopsis+ii+1,"+1",2)==0 ){
              ii += 2;
              v2++;
            }
            if( v2<2 ){
              capdb_str_appendf(&x, "%d", v1);
            }else{
              capdb_str_appendf(&x, "%d..%d", v1, v1+v2-1);
            }
          }else if( strncmp(zSynopsis+ii+1, "@NP", 3)==0 ){
            capdb_context *pCtx = pOp->p4.pCtx;
            if( pOp->p4type!=P4_FUNCCTX || pCtx->argc==1 ){
              capdb_str_appendf(&x, "%d", v1);
            }else if( pCtx->argc>1 ){
              capdb_str_appendf(&x, "%d..%d", v1, v1+pCtx->argc-1);
            }else if( x.accError==0 ){
              assert( x.nChar>2 );
              x.nChar -= 2;
              ii++;
            }
            ii += 3;
          }else{
            capdb_str_appendf(&x, "%d", v1);
            if( strncmp(zSynopsis+ii+1, "..P3", 4)==0 && pOp->p3==0 ){
              ii += 4;
            }
          }
        }
      }else{
        capdb_str_appendchar(&x, 1, c);
      }
    }
    if( !seenCom && pOp->zComment ){
      capdb_str_appendf(&x, "; %s", pOp->zComment);
    }
  }else if( pOp->zComment ){
    capdb_str_appendall(&x, pOp->zComment);
  }
  if( (x.accError & CAPDB_NOMEM)!=0 && db!=0 ){
    capdbOomFault(db);
  }
  return capdbStrAccumFinish(&x);
}
#endif /* CAPDB_ENABLE_EXPLAIN_COMMENTS */

#if VDBE_DISPLAY_P4 && defined(CAPDB_ENABLE_CURSOR_HINTS)
/*
** Translate the P4.pExpr value for an OP_CursorHint opcode into text
** that can be displayed in the P4 column of EXPLAIN output.
*/
static void displayP4Expr(StrAccum *p, Expr *pExpr){
  const char *zOp = 0;
  switch( pExpr->op ){
    case TK_STRING:
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdb_str_appendf(p, "%Q", pExpr->u.zToken);
      break;
    case TK_INTEGER:
      capdb_str_appendf(p, "%d", pExpr->u.iValue);
      break;
    case TK_NULL:
      capdb_str_appendf(p, "NULL");
      break;
    case TK_REGISTER: {
      capdb_str_appendf(p, "r[%d]", pExpr->iTable);
      break;
    }
    case TK_COLUMN: {
      if( pExpr->iColumn<0 ){
        capdb_str_appendf(p, "rowid");
      }else{
        capdb_str_appendf(p, "c%d", (int)pExpr->iColumn);
      }
      break;
    }
    case TK_LT:      zOp = "LT";      break;
    case TK_LE:      zOp = "LE";      break;
    case TK_GT:      zOp = "GT";      break;
    case TK_GE:      zOp = "GE";      break;
    case TK_NE:      zOp = "NE";      break;
    case TK_EQ:      zOp = "EQ";      break;
    case TK_IS:      zOp = "IS";      break;
    case TK_ISNOT:   zOp = "ISNOT";   break;
    case TK_AND:     zOp = "AND";     break;
    case TK_OR:      zOp = "OR";      break;
    case TK_PLUS:    zOp = "ADD";     break;
    case TK_STAR:    zOp = "MUL";     break;
    case TK_MINUS:   zOp = "SUB";     break;
    case TK_REM:     zOp = "REM";     break;
    case TK_BITAND:  zOp = "BITAND";  break;
    case TK_BITOR:   zOp = "BITOR";   break;
    case TK_SLASH:   zOp = "DIV";     break;
    case TK_LSHIFT:  zOp = "LSHIFT";  break;
    case TK_RSHIFT:  zOp = "RSHIFT";  break;
    case TK_CONCAT:  zOp = "CONCAT";  break;
    case TK_UMINUS:  zOp = "MINUS";   break;
    case TK_UPLUS:   zOp = "PLUS";    break;
    case TK_BITNOT:  zOp = "BITNOT";  break;
    case TK_NOT:     zOp = "NOT";     break;
    case TK_ISNULL:  zOp = "ISNULL";  break;
    case TK_NOTNULL: zOp = "NOTNULL"; break;

    default:
      capdb_str_appendf(p, "%s", "expr");
      break;
  }

  if( zOp ){
    capdb_str_appendf(p, "%s(", zOp);
    displayP4Expr(p, pExpr->pLeft);
    if( pExpr->pRight ){
      capdb_str_append(p, ",", 1);
      displayP4Expr(p, pExpr->pRight);
    }
    capdb_str_append(p, ")", 1);
  }
}
#endif /* VDBE_DISPLAY_P4 && defined(CAPDB_ENABLE_CURSOR_HINTS) */


#if VDBE_DISPLAY_P4
/*
** Compute a string that describes the P4 parameter for an opcode.
*/
char *capdbVdbeDisplayP4(capdb *db, Op *pOp){
  char *zP4 = 0;
  StrAccum x;

  capdbStrAccumInit(&x, 0, 0, 0, CAPDB_MAX_LENGTH);
  switch( pOp->p4type ){
    case P4_KEYINFO: {
      int j;
      KeyInfo *pKeyInfo = pOp->p4.pKeyInfo;
      assert( pKeyInfo->aSortFlags!=0 );
      capdb_str_appendf(&x, "k(%d", pKeyInfo->nKeyField);
      for(j=0; j<pKeyInfo->nKeyField; j++){
        CollSeq *pColl = pKeyInfo->aColl[j];
        const char *zColl = pColl ? pColl->zName : "";
        if( strcmp(zColl, "BINARY")==0 ) zColl = "B";
        capdb_str_appendf(&x, ",%s%s%s",
               (pKeyInfo->aSortFlags[j] & KEYINFO_ORDER_DESC) ? "-" : "",
               (pKeyInfo->aSortFlags[j] & KEYINFO_ORDER_BIGNULL)? "N." : "",
               zColl);
      }
      capdb_str_append(&x, ")", 1);
      break;
    }
#ifdef CAPDB_ENABLE_CURSOR_HINTS
    case P4_EXPR: {
      displayP4Expr(&x, pOp->p4.pExpr);
      break;
    }
#endif
    case P4_COLLSEQ: {
      static const char *const encnames[] = {"?", "8", "16LE", "16BE"};
      CollSeq *pColl = pOp->p4.pColl;
      assert( pColl->enc<4 );
      capdb_str_appendf(&x, "%.18s-%s", pColl->zName,
                          encnames[pColl->enc]);
      break;
    }
    case P4_FUNCDEF: {
      FuncDef *pDef = pOp->p4.pFunc;
      capdb_str_appendf(&x, "%s(%d)", pDef->zName, pDef->nArg);
      break;
    }
    case P4_FUNCCTX: {
      FuncDef *pDef = pOp->p4.pCtx->pFunc;
      capdb_str_appendf(&x, "%s(%d)", pDef->zName, pDef->nArg);
      break;
    }
    case P4_INT64: {
      capdb_str_appendf(&x, "%lld", *pOp->p4.pI64);
      break;
    }
    case P4_INT32: {
      capdb_str_appendf(&x, "%d", pOp->p4.i);
      break;
    }
    case P4_REAL: {
      capdb_str_appendf(&x, "%.16g", *pOp->p4.pReal);
      break;
    }
    case P4_MEM: {
      Mem *pMem = pOp->p4.pMem;
      if( pMem->flags & MEM_Str ){
        zP4 = pMem->z;
      }else if( pMem->flags & (MEM_Int|MEM_IntReal) ){
        capdb_str_appendf(&x, "%lld", pMem->u.i);
      }else if( pMem->flags & MEM_Real ){
        capdb_str_appendf(&x, "%.16g", pMem->u.r);
      }else if( pMem->flags & MEM_Null ){
        zP4 = "NULL";
      }else{
        assert( pMem->flags & MEM_Blob );
        zP4 = "(blob)";
      }
      break;
    }
#ifndef CAPDB_OMIT_VIRTUALTABLE
    case P4_VTAB: {
      capdb_vtab *pVtab = pOp->p4.pVtab->pVtab;
      capdb_str_appendf(&x, "vtab:%p", pVtab);
      break;
    }
#endif
    case P4_INTARRAY: {
      u32 i;
      u32 *ai = pOp->p4.ai;
      u32 n = ai[0];   /* The first element of an INTARRAY is always the
                       ** count of the number of elements to follow */
      for(i=1; i<=n; i++){
        capdb_str_appendf(&x, "%c%u", (i==1 ? '[' : ','), ai[i]);
      }
      capdb_str_append(&x, "]", 1);
      break;
    }
    case P4_SUBPROGRAM: {
      zP4 = "program";
      break;
    }
    case P4_TABLE: {
      zP4 = pOp->p4.pTab->zName;
      break;
    }
    case P4_INDEX: {
      zP4 = pOp->p4.pIdx->zName;
      break;
    }
    case P4_SUBRTNSIG: {
      SubrtnSig *pSig = pOp->p4.pSubrtnSig;
      capdb_str_appendf(&x, "subrtnsig:%d,%s", pSig->selId, pSig->zAff);
      break;
    }
    default: {
      zP4 = pOp->p4.z;
    }
  }
  if( zP4 ) capdb_str_appendall(&x, zP4);
  if( (x.accError & CAPDB_NOMEM)!=0 ){
    capdbOomFault(db);
  }
  return capdbStrAccumFinish(&x);
}
#endif /* VDBE_DISPLAY_P4 */

/*
** Declare to the Vdbe that the BTree object at db->aDb[i] is used.
**
** The prepared statements need to know in advance the complete set of
** attached databases that will be use.  A mask of these databases
** is maintained in p->btreeMask.  The p->lockMask value is the subset of
** p->btreeMask of databases that will require a lock.
*/
void capdbVdbeUsesBtree(Vdbe *p, int i){
  assert( i>=0 && i<p->db->nDb && i<(int)sizeof(yDbMask)*8 );
  assert( i<(int)sizeof(p->btreeMask)*8 );
  DbMaskSet(p->btreeMask, i);
  if( i!=1 && capdbBtreeSharable(p->db->aDb[i].pBt) ){
    DbMaskSet(p->lockMask, i);
  }
}

#if !defined(CAPDB_OMIT_SHARED_CACHE)
/*
** If SQLite is compiled to support shared-cache mode and to be threadsafe,
** this routine obtains the mutex associated with each BtShared structure
** that may be accessed by the VM passed as an argument. In doing so it also
** sets the BtShared.db member of each of the BtShared structures, ensuring
** that the correct busy-handler callback is invoked if required.
**
** If SQLite is not threadsafe but does support shared-cache mode, then
** capdbBtreeEnter() is invoked to set the BtShared.db variables
** of all of BtShared structures accessible via the database handle
** associated with the VM.
**
** If SQLite is not threadsafe and does not support shared-cache mode, this
** function is a no-op.
**
** The p->btreeMask field is a bitmask of all btrees that the prepared
** statement p will ever use.  Let N be the number of bits in p->btreeMask
** corresponding to btrees that use shared cache.  Then the runtime of
** this routine is N*N.  But as N is rarely more than 1, this should not
** be a problem.
*/
void capdbVdbeEnter(Vdbe *p){
  int i;
  capdb *db;
  Db *aDb;
  int nDb;
  if( DbMaskAllZero(p->lockMask) ) return;  /* The common case */
  db = p->db;
  aDb = db->aDb;
  nDb = db->nDb;
  for(i=0; i<nDb; i++){
    if( i!=1 && DbMaskTest(p->lockMask,i) && ALWAYS(aDb[i].pBt!=0) ){
      capdbBtreeEnter(aDb[i].pBt);
    }
  }
}
#endif

#if !defined(CAPDB_OMIT_SHARED_CACHE) && CAPDB_THREADSAFE>0
/*
** Unlock all of the btrees previously locked by a call to capdbVdbeEnter().
*/
static CAPDB_NOINLINE void vdbeLeave(Vdbe *p){
  int i;
  capdb *db;
  Db *aDb;
  int nDb;
  db = p->db;
  aDb = db->aDb;
  nDb = db->nDb;
  for(i=0; i<nDb; i++){
    if( i!=1 && DbMaskTest(p->lockMask,i) && ALWAYS(aDb[i].pBt!=0) ){
      capdbBtreeLeave(aDb[i].pBt);
    }
  }
}
void capdbVdbeLeave(Vdbe *p){
  if( DbMaskAllZero(p->lockMask) ) return;  /* The common case */
  vdbeLeave(p);
}
#endif

#if defined(VDBE_PROFILE) || defined(CAPDB_DEBUG)
/*
** Print a single opcode.  This routine is used for debugging only.
*/
void capdbVdbePrintOp(FILE *pOut, int pc, VdbeOp *pOp){
  char *zP4;
  char *zCom;
  capdb dummyDb;
  static const char *zFormat1 = "%4d %-13s %4d %4d %4d %-13s %.2X %s\n";
  if( pOut==0 ) pOut = stdout;
  capdbBeginBenignMalloc();
  dummyDb.mallocFailed = 1;
  zP4 = capdbVdbeDisplayP4(&dummyDb, pOp);
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
  zCom = capdbVdbeDisplayComment(0, pOp, zP4);
#else
  zCom = 0;
#endif
  /* NB:  The capdbOpcodeName() function is implemented by code created
  ** by the mkopcodeh.awk and mkopcodec.awk scripts which extract the
  ** information from the vdbe.c source text */
  fprintf(pOut, zFormat1, pc,
      capdbOpcodeName(pOp->opcode), pOp->p1, pOp->p2, pOp->p3,
      zP4 ? zP4 : "", pOp->p5,
      zCom ? zCom : ""
  );
  fflush(pOut);
  capdb_free(zP4);
  capdb_free(zCom);
  capdbEndBenignMalloc();
}
#endif

/*
** Initialize an array of N Mem element.
**
** This is a high-runner, so only those fields that really do need to
** be initialized are set.  The Mem structure is organized so that
** the fields that get initialized are nearby and hopefully on the same
** cache line.
**
**    Mem.flags = flags
**    Mem.db = db
**    Mem.szMalloc = 0
**
** All other fields of Mem can safely remain uninitialized for now.  They
** will be initialized before use.
*/
static void initMemArray(Mem *p, int N, capdb *db, u16 flags){
  assert( db!=0 );
  if( N>0 ){
    do{
      p->flags = flags;
      p->db = db;
      p->szMalloc = 0;
#ifdef CAPDB_DEBUG
      p->pScopyFrom = 0;
      p->bScopy = 0;
#endif
      p++;
    }while( (--N)>0 );
  }
}

/*
** Release auxiliary memory held in an array of N Mem elements.
**
** After this routine returns, all Mem elements in the array will still
** be valid.  Those Mem elements that were not holding auxiliary resources
** will be unchanged.  Mem elements which had something freed will be
** set to MEM_Undefined.
*/
static void releaseMemArray(Mem *p, int N){
  if( p && N ){
    Mem *pEnd = &p[N];
    capdb *db = p->db;
    assert( db!=0 );
    if( db->pnBytesFreed ){
      do{
        if( p->szMalloc ) capdbDbFree(db, p->zMalloc);
      }while( (++p)<pEnd );
      return;
    }
    do{
      assert( (&p[1])==pEnd || p[0].db==p[1].db );
      assert( capdbVdbeCheckMemInvariants(p) );

      /* This block is really an inlined version of capdbVdbeMemRelease()
      ** that takes advantage of the fact that the memory cell value is
      ** being set to NULL after releasing any dynamic resources.
      **
      ** The justification for duplicating code is that according to
      ** callgrind, this causes a certain test case to hit the CPU 4.7
      ** percent less (x86 linux, gcc version 4.1.2, -O6) than if
      ** capdbMemRelease() were called from here. With -O2, this jumps
      ** to 6.6 percent. The test case is inserting 1000 rows into a table
      ** with no indexes using a single prepared INSERT statement, bind()
      ** and reset(). Inserts are grouped into a transaction.
      */
      testcase( p->flags & MEM_Agg );
      testcase( p->flags & MEM_Dyn );
      if( p->flags&(MEM_Agg|MEM_Dyn) ){
        testcase( (p->flags & MEM_Dyn)!=0 && p->xDel==capdbVdbeFrameMemDel );
        capdbVdbeMemRelease(p);
        p->flags = MEM_Undefined;
      }else if( p->szMalloc ){
        capdbDbNNFreeNN(db, p->zMalloc);
        p->szMalloc = 0;
        p->flags = MEM_Undefined;
      }
#ifdef CAPDB_DEBUG
      else{
        p->flags = MEM_Undefined;
      }
#endif
    }while( (++p)<pEnd );
  }
}

#ifdef CAPDB_DEBUG
/*
** Verify that pFrame is a valid VdbeFrame pointer.  Return true if it is
** and false if something is wrong.
**
** This routine is intended for use inside of assert() statements only.
*/
int capdbVdbeFrameIsValid(VdbeFrame *pFrame){
  if( pFrame->iFrameMagic!=CAPDB_FRAME_MAGIC ) return 0;
  return 1;
}
#endif


/*
** This is a destructor on a Mem object (which is really an capdb_value)
** that deletes the Frame object that is attached to it as a blob.
**
** This routine does not delete the Frame right away.  It merely adds the
** frame to a list of frames to be deleted when the Vdbe halts.
*/
void capdbVdbeFrameMemDel(void *pArg){
  VdbeFrame *pFrame = (VdbeFrame*)pArg;
  assert( capdbVdbeFrameIsValid(pFrame) );
  pFrame->pParent = pFrame->v->pDelFrame;
  pFrame->v->pDelFrame = pFrame;
}

#if defined(CAPDB_ENABLE_BYTECODE_VTAB) || !defined(CAPDB_OMIT_EXPLAIN)
/*
** Locate the next opcode to be displayed in EXPLAIN or EXPLAIN
** QUERY PLAN output.
**
** Return CAPDB_ROW on success.  Return CAPDB_DONE if there are no
** more opcodes to be displayed.
*/
int capdbVdbeNextOpcode(
  Vdbe *p,         /* The statement being explained */
  Mem *pSub,       /* Storage for keeping track of subprogram nesting */
  int eMode,       /* 0: normal.  1: EQP.  2:  TablesUsed */
  int *piPc,       /* IN/OUT: Current rowid.  Overwritten with next rowid */
  int *piAddr,     /* OUT: Write index into (*paOp)[] here */
  Op **paOp        /* OUT: Write the opcode array here */
){
  int nRow;                            /* Stop when row count reaches this */
  int nSub = 0;                        /* Number of sub-vdbes seen so far */
  SubProgram **apSub = 0;              /* Array of sub-vdbes */
  int i;                               /* Next instruction address */
  int rc = CAPDB_OK;                  /* Result code */
  Op *aOp = 0;                         /* Opcode array */
  int iPc;                             /* Rowid.  Copy of value in *piPc */

  /* When the number of output rows reaches nRow, that means the
  ** listing has finished and capdb_step() should return CAPDB_DONE.
  ** nRow is the sum of the number of rows in the main program, plus
  ** the sum of the number of rows in all trigger subprograms encountered
  ** so far.  The nRow value will increase as new trigger subprograms are
  ** encountered, but p->pc will eventually catch up to nRow.
  */
  nRow = p->nOp;
  if( pSub!=0 ){
    if( pSub->flags&MEM_Blob ){
      /* pSub is initiallly NULL.  It is initialized to a BLOB by
      ** the P4_SUBPROGRAM processing logic below */
      nSub = pSub->n/sizeof(Vdbe*);
      apSub = (SubProgram **)pSub->z;
    }
    for(i=0; i<nSub; i++){
      nRow += apSub[i]->nOp;
    }
  }
  iPc = *piPc;
  while(1){  /* Loop exits via break */
    i = iPc++;
    if( i>=nRow ){
      p->rc = CAPDB_OK;
      rc = CAPDB_DONE;
      break;
    }
    if( i<p->nOp ){
      /* The rowid is small enough that we are still in the
      ** main program. */
      aOp = p->aOp;
    }else{
      /* We are currently listing subprograms.  Figure out which one and
      ** pick up the appropriate opcode. */
      int j;
      i -= p->nOp;
      assert( apSub!=0 );
      assert( nSub>0 );
      for(j=0; i>=apSub[j]->nOp; j++){
        i -= apSub[j]->nOp;
        assert( i<apSub[j]->nOp || j+1<nSub );
      }
      aOp = apSub[j]->aOp;
    }

    /* When an OP_Program opcode is encounter (the only opcode that has
    ** a P4_SUBPROGRAM argument), expand the size of the array of subprograms
    ** kept in p->aMem[9].z to hold the new program - assuming this subprogram
    ** has not already been seen.
    */
    if( pSub!=0 && aOp[i].p4type==P4_SUBPROGRAM ){
      int nByte = (nSub+1)*sizeof(SubProgram*);
      int j;
      for(j=0; j<nSub; j++){
        if( apSub[j]==aOp[i].p4.pProgram ) break;
      }
      if( j==nSub ){
        p->rc = capdbVdbeMemGrow(pSub, nByte, nSub!=0);
        if( p->rc!=CAPDB_OK ){
          rc = CAPDB_ERROR;
          break;
        }
        apSub = (SubProgram **)pSub->z;
        apSub[nSub++] = aOp[i].p4.pProgram;
        MemSetTypeFlag(pSub, MEM_Blob);
        pSub->n = nSub*sizeof(SubProgram*);
        nRow += aOp[i].p4.pProgram->nOp;
      }
    }
    if( eMode==0 ) break;
#ifdef CAPDB_ENABLE_BYTECODE_VTAB
    if( eMode==2 ){
      Op *pOp = aOp + i;
      if( pOp->opcode==OP_OpenRead ) break;
      if( pOp->opcode==OP_OpenWrite && (pOp->p5 & OPFLAG_P2ISREG)==0 ) break;
      if( pOp->opcode==OP_ReopenIdx ) break;     
    }else
#endif
    {
      assert( eMode==1 );
      if( aOp[i].opcode==OP_Explain ) break;
      if( aOp[i].opcode==OP_Init && iPc>1 ) break;
    }
  }
  *piPc = iPc;
  *piAddr = i;
  *paOp = aOp;
  return rc;
}
#endif /* CAPDB_ENABLE_BYTECODE_VTAB || !CAPDB_OMIT_EXPLAIN */


/*
** Delete a VdbeFrame object and its contents. VdbeFrame objects are
** allocated by the OP_Program opcode in capdbVdbeExec().
*/
void capdbVdbeFrameDelete(VdbeFrame *p){
  int i;
  Mem *aMem = VdbeFrameMem(p);
  VdbeCursor **apCsr = (VdbeCursor **)&aMem[p->nChildMem];
  assert( capdbVdbeFrameIsValid(p) );
  for(i=0; i<p->nChildCsr; i++){
    if( apCsr[i] ) capdbVdbeFreeCursorNN(p->v, apCsr[i]);
  }
  releaseMemArray(aMem, p->nChildMem);
  capdbVdbeDeleteAuxData(p->v->db, &p->pAuxData, -1, 0);
  capdbDbFree(p->v->db, p);
}

#ifndef CAPDB_OMIT_EXPLAIN
/*
** Give a listing of the program in the virtual machine.
**
** The interface is the same as capdbVdbeExec().  But instead of
** running the code, it invokes the callback once for each instruction.
** This feature is used to implement "EXPLAIN".
**
** When p->explain==1, each instruction is listed.  When
** p->explain==2, only OP_Explain instructions are listed and these
** are shown in a different format.  p->explain==2 is used to implement
** EXPLAIN QUERY PLAN.
** 2018-04-24:  In p->explain==2 mode, the OP_Init opcodes of triggers
** are also shown, so that the boundaries between the main program and
** each trigger are clear.
**
** When p->explain==1, first the main program is listed, then each of
** the trigger subprograms are listed one by one.
*/
int capdbVdbeList(
  Vdbe *p                   /* The VDBE */
){
  Mem *pSub = 0;                       /* Memory cell hold array of subprogs */
  capdb *db = p->db;                 /* The database connection */
  int i;                               /* Loop counter */
  int rc = CAPDB_OK;                  /* Return code */
  Mem *pMem = &p->aMem[1];             /* First Mem of result set */
  int bListSubprogs = (p->explain==1 || (db->flags & CAPDB_TriggerEQP)!=0);
  Op *aOp;                             /* Array of opcodes */
  Op *pOp;                             /* Current opcode */

  assert( p->explain );
  assert( p->eVdbeState==VDBE_RUN_STATE );
  assert( p->rc==CAPDB_OK || p->rc==CAPDB_BUSY || p->rc==CAPDB_NOMEM );

  /* Even though this opcode does not use dynamic strings for
  ** the result, result columns may become dynamic if the user calls
  ** capdb_column_text16(), causing a translation to UTF-16 encoding.
  */
  releaseMemArray(pMem, 8);

  if( p->rc==CAPDB_NOMEM ){
    /* This happens if a malloc() inside a call to capdb_column_text() or
    ** capdb_column_text16() failed.  */
    capdbOomFault(db);
    return CAPDB_ERROR;
  }

  if( bListSubprogs ){
    /* The first 8 memory cells are used for the result set.  So we will
    ** commandeer the 9th cell to use as storage for an array of pointers
    ** to trigger subprograms.  The VDBE is guaranteed to have at least 9
    ** cells.  */
    assert( p->nMem>9 );
    pSub = &p->aMem[9];
  }else{
    pSub = 0;
  }

  /* Figure out which opcode is next to display */
  rc = capdbVdbeNextOpcode(p, pSub, p->explain==2, &p->pc, &i, &aOp);

  if( rc==CAPDB_OK ){
    pOp = aOp + i;
    if( AtomicLoad(&db->u1.isInterrupted) ){
      p->rc = CAPDB_INTERRUPT;
      rc = CAPDB_ERROR;
      capdbVdbeError(p, capdbErrStr(p->rc));
    }else{
      char *zP4 = capdbVdbeDisplayP4(db, pOp);
      if( p->explain==2 ){
        capdbVdbeMemSetInt64(pMem, pOp->p1);
        capdbVdbeMemSetInt64(pMem+1, pOp->p2);
        capdbVdbeMemSetInt64(pMem+2, pOp->p3);
        capdbVdbeMemSetStr(pMem+3, zP4, -1, CAPDB_UTF8, capdb_free);
        assert( p->nResColumn==4 );
      }else{
        capdbVdbeMemSetInt64(pMem+0, i);
        capdbVdbeMemSetStr(pMem+1, (char*)capdbOpcodeName(pOp->opcode),
                             -1, CAPDB_UTF8, CAPDB_STATIC);
        capdbVdbeMemSetInt64(pMem+2, pOp->p1);
        capdbVdbeMemSetInt64(pMem+3, pOp->p2);
        capdbVdbeMemSetInt64(pMem+4, pOp->p3);
        /* pMem+5 for p4 is done last */
        capdbVdbeMemSetInt64(pMem+6, pOp->p5);
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
        {
          char *zCom = capdbVdbeDisplayComment(db, pOp, zP4);
          capdbVdbeMemSetStr(pMem+7, zCom, -1, CAPDB_UTF8, capdb_free);
        }
#else
        capdbVdbeMemSetNull(pMem+7);
#endif
        capdbVdbeMemSetStr(pMem+5, zP4, -1, CAPDB_UTF8, capdb_free);
        assert( p->nResColumn==8 );
      }
      p->pResultRow = pMem;
      if( db->mallocFailed ){
        p->rc = CAPDB_NOMEM;
        rc = CAPDB_ERROR;
      }else{
        p->rc = CAPDB_OK;
        rc = CAPDB_ROW;
      }
    }
  }
  return rc;
}
#endif /* CAPDB_OMIT_EXPLAIN */

#ifdef CAPDB_DEBUG
/*
** Print the SQL that was used to generate a VDBE program.
*/
void capdbVdbePrintSql(Vdbe *p){
  const char *z = 0;
  if( p->zSql ){
    z = p->zSql;
  }else if( p->nOp>=1 ){
    const VdbeOp *pOp = &p->aOp[0];
    if( pOp->opcode==OP_Init && pOp->p4.z!=0 ){
      z = pOp->p4.z;
      while( capdbIsspace(*z) ) z++;
    }
  }
  if( z ) printf("SQL: [%s]\n", z);
}
#endif

#if !defined(CAPDB_OMIT_TRACE) && defined(CAPDB_ENABLE_IOTRACE)
/*
** Print an IOTRACE message showing SQL content.
*/
void capdbVdbeIOTraceSql(Vdbe *p){
  int nOp = p->nOp;
  VdbeOp *pOp;
  if( capdbIoTrace==0 ) return;
  if( nOp<1 ) return;
  pOp = &p->aOp[0];
  if( pOp->opcode==OP_Init && pOp->p4.z!=0 ){
    int i, j;
    char z[1000];
    capdb_snprintf(sizeof(z), z, "%s", pOp->p4.z);
    for(i=0; capdbIsspace(z[i]); i++){}
    for(j=0; z[i]; i++){
      if( capdbIsspace(z[i]) ){
        if( z[i-1]!=' ' ){
          z[j++] = ' ';
        }
      }else{
        z[j++] = z[i];
      }
    }
    z[j] = 0;
    capdbIoTrace("SQL %s\n", z);
  }
}
#endif /* !CAPDB_OMIT_TRACE && CAPDB_ENABLE_IOTRACE */

/* An instance of this object describes bulk memory available for use
** by subcomponents of a prepared statement.  Space is allocated out
** of a ReusableSpace object by the allocSpace() routine below.
*/
struct ReusableSpace {
  u8 *pSpace;            /* Available memory */
  capdb_int64 nFree;   /* Bytes of available memory */
  capdb_int64 nNeeded; /* Total bytes that could not be allocated */
};

/* Try to allocate nByte bytes of 8-byte aligned bulk memory for pBuf
** from the ReusableSpace object.  Return a pointer to the allocated
** memory on success.  If insufficient memory is available in the
** ReusableSpace object, increase the ReusableSpace.nNeeded
** value by the amount needed and return NULL.
**
** If pBuf is not initially NULL, that means that the memory has already
** been allocated by a prior call to this routine, so just return a copy
** of pBuf and leave ReusableSpace unchanged.
**
** This allocator is employed to repurpose unused slots at the end of the
** opcode array of prepared state for other memory needs of the prepared
** statement.
*/
static void *allocSpace(
  struct ReusableSpace *p,  /* Bulk memory available for allocation */
  void *pBuf,               /* Pointer to a prior allocation */
  capdb_int64 nByte       /* Bytes of memory needed. */
){
  assert( EIGHT_BYTE_ALIGNMENT(p->pSpace) );
  if( pBuf==0 ){
    nByte = ROUND8P(nByte);
    if( nByte <= p->nFree ){
      p->nFree -= nByte;
      pBuf = &p->pSpace[p->nFree];
    }else{
      p->nNeeded += nByte;
    }
  }
  assert( EIGHT_BYTE_ALIGNMENT(pBuf) );
  return pBuf;
}

/*
** Rewind the VDBE back to the beginning in preparation for
** running it.
*/
void capdbVdbeRewind(Vdbe *p){
#if defined(CAPDB_DEBUG)
  int i;
#endif
  assert( p!=0 );
  assert( p->eVdbeState==VDBE_INIT_STATE
       || p->eVdbeState==VDBE_READY_STATE
       || p->eVdbeState==VDBE_HALT_STATE );

  /* There should be at least one opcode.
  */
  assert( p->nOp>0 );

  p->eVdbeState = VDBE_READY_STATE;

#ifdef CAPDB_DEBUG
  for(i=0; i<p->nMem; i++){
    assert( p->aMem[i].db==p->db );
  }
#endif
  p->pc = -1;
  p->rc = CAPDB_OK;
  p->errorAction = OE_Abort;
  p->nChange = 0;
  p->cacheCtr = 1;
  p->minWriteFileFormat = 255;
  p->iStatement = 0;
  p->nFkConstraint = 0;
#ifdef VDBE_PROFILE
  for(i=0; i<p->nOp; i++){
    p->aOp[i].nExec = 0;
    p->aOp[i].nCycle = 0;
  }
#endif
}

/*
** Prepare a virtual machine for execution for the first time after
** creating the virtual machine.  This involves things such
** as allocating registers and initializing the program counter.
** After the VDBE has be prepped, it can be executed by one or more
** calls to capdbVdbeExec(). 
**
** This function may be called exactly once on each virtual machine.
** After this routine is called the VM has been "packaged" and is ready
** to run.  After this routine is called, further calls to
** capdbVdbeAddOp() functions are prohibited.  This routine disconnects
** the Vdbe from the Parse object that helped generate it so that the
** the Vdbe becomes an independent entity and the Parse object can be
** destroyed.
**
** Use the capdbVdbeRewind() procedure to restore a virtual machine back
** to its initial state after it has been run.
*/
void capdbVdbeMakeReady(
  Vdbe *p,                       /* The VDBE */
  Parse *pParse                  /* Parsing context */
){
  capdb *db;                   /* The database connection */
  int nVar;                      /* Number of parameters */
  int nMem;                      /* Number of VM memory registers */
  int nCursor;                   /* Number of cursors required */
  int nArg;                      /* Max number args to xFilter or xUpdate */
  int n;                         /* Loop counter */
  struct ReusableSpace x;        /* Reusable bulk memory */

  assert( p!=0 );
  assert( p->nOp>0 );
  assert( pParse!=0 );
  assert( p->eVdbeState==VDBE_INIT_STATE );
  assert( pParse==p->pParse );
  assert( pParse->db==p->db );
  p->pVList = pParse->pVList;
  pParse->pVList =  0;
  db = p->db;
  assert( db->mallocFailed==0 );
  nVar = pParse->nVar;
  nMem = pParse->nMem;
  nCursor = pParse->nTab;
  nArg = pParse->nMaxArg;
 
  /* Each cursor uses a memory cell.  The first cursor (cursor 0) can
  ** use aMem[0] which is not otherwise used by the VDBE program.  Allocate
  ** space at the end of aMem[] for cursors 1 and greater.
  ** See also: allocateCursor().
  */
  nMem += nCursor;
  if( nCursor==0 && nMem>0 ) nMem++;  /* Space for aMem[0] even if not used */

  /* Figure out how much reusable memory is available at the end of the
  ** opcode array.  This extra memory will be reallocated for other elements
  ** of the prepared statement.
  */
  n = ROUND8P(sizeof(Op)*p->nOp);             /* Bytes of opcode memory used */
  x.pSpace = &((u8*)p->aOp)[n];               /* Unused opcode memory */
  assert( EIGHT_BYTE_ALIGNMENT(x.pSpace) );
  x.nFree = ROUNDDOWN8(pParse->szOpAlloc - n);  /* Bytes of unused memory */
  assert( x.nFree>=0 );
  assert( EIGHT_BYTE_ALIGNMENT(&x.pSpace[x.nFree]) );

  resolveP2Values(p, &nArg);
  p->usesStmtJournal = (u8)(pParse->isMultiWrite && pParse->mayAbort);
  if( pParse->explain ){
    if( nMem<10 ) nMem = 10;
    p->explain = pParse->explain;
    p->nResColumn = 12 - 4*p->explain;
  }
  p->expired = 0;

  /* Memory for registers, parameters, cursor, etc, is allocated in one or two
  ** passes.  On the first pass, we try to reuse unused memory at the
  ** end of the opcode array.  If we are unable to satisfy all memory
  ** requirements by reusing the opcode array tail, then the second
  ** pass will fill in the remainder using a fresh memory allocation. 
  **
  ** This two-pass approach that reuses as much memory as possible from
  ** the leftover memory at the end of the opcode array.  This can significantly
  ** reduce the amount of memory held by a prepared statement.
  */
  x.nNeeded = 0;
  p->aMem = allocSpace(&x, 0, nMem*sizeof(Mem));
  p->aVar = allocSpace(&x, 0, nVar*sizeof(Mem));
  p->apArg = allocSpace(&x, 0, nArg*sizeof(Mem*));
  p->apCsr = allocSpace(&x, 0, nCursor*sizeof(VdbeCursor*));
  if( x.nNeeded ){
    x.pSpace = p->pFree = capdbDbMallocRawNN(db, x.nNeeded);
    x.nFree = x.nNeeded;
    if( !db->mallocFailed ){
      p->aMem = allocSpace(&x, p->aMem, nMem*sizeof(Mem));
      p->aVar = allocSpace(&x, p->aVar, nVar*sizeof(Mem));
      p->apArg = allocSpace(&x, p->apArg, nArg*sizeof(Mem*));
      p->apCsr = allocSpace(&x, p->apCsr, nCursor*sizeof(VdbeCursor*));
    }
  }
#ifdef CAPDB_DEBUG
  p->napArg = nArg;
#endif

  if( db->mallocFailed ){
    p->nVar = 0;
    p->nCursor = 0;
    p->nMem = 0;
  }else{
    p->nCursor = nCursor;
    p->nVar = (ynVar)nVar;
    initMemArray(p->aVar, nVar, db, MEM_Null);
    p->nMem = nMem;
    initMemArray(p->aMem, nMem, db, MEM_Undefined);
    memset(p->apCsr, 0, nCursor*sizeof(VdbeCursor*));
  }
  capdbVdbeRewind(p);
}

/*
** Close a VDBE cursor and release all the resources that cursor
** happens to hold.
*/
void capdbVdbeFreeCursor(Vdbe *p, VdbeCursor *pCx){
  if( pCx ) capdbVdbeFreeCursorNN(p,pCx);
}
static CAPDB_NOINLINE void freeCursorWithCache(Vdbe *p, VdbeCursor *pCx){
  VdbeTxtBlbCache *pCache = pCx->pCache;
  assert( pCx->colCache );
  pCx->colCache = 0;
  pCx->pCache = 0;
  if( pCache->pCValue ){
    capdbRCStrUnref(pCache->pCValue);
    pCache->pCValue = 0;
  }
  capdbDbFree(p->db, pCache);
  capdbVdbeFreeCursorNN(p, pCx);
}
void capdbVdbeFreeCursorNN(Vdbe *p, VdbeCursor *pCx){
  if( pCx->colCache ){
    freeCursorWithCache(p, pCx);
    return;
  }
  switch( pCx->eCurType ){
    case CURTYPE_SORTER: {
      capdbVdbeSorterClose(p->db, pCx);
      break;
    }
    case CURTYPE_BTREE: {
      assert( pCx->uc.pCursor!=0 );
      capdbBtreeCloseCursor(pCx->uc.pCursor);
      break;
    }
#ifndef CAPDB_OMIT_VIRTUALTABLE
    case CURTYPE_VTAB: {
      capdb_vtab_cursor *pVCur = pCx->uc.pVCur;
      const capdb_module *pModule = pVCur->pVtab->pModule;
      assert( pVCur->pVtab->nRef>0 );
      pVCur->pVtab->nRef--;
      pModule->xClose(pVCur);
      break;
    }
#endif
  }
}

/*
** Close all cursors in the current frame.
*/
static void closeCursorsInFrame(Vdbe *p){
  int i;
  for(i=0; i<p->nCursor; i++){
    VdbeCursor *pC = p->apCsr[i];
    if( pC ){
      capdbVdbeFreeCursorNN(p, pC);
      p->apCsr[i] = 0;
    }
  }
}

/*
** Copy the values stored in the VdbeFrame structure to its Vdbe. This
** is used, for example, when a trigger sub-program is halted to restore
** control to the main program.
*/
int capdbVdbeFrameRestore(VdbeFrame *pFrame){
  Vdbe *v = pFrame->v;
  closeCursorsInFrame(v);
  v->aOp = pFrame->aOp;
  v->nOp = pFrame->nOp;
  v->aMem = pFrame->aMem;
  v->nMem = pFrame->nMem;
  v->apCsr = pFrame->apCsr;
  v->nCursor = pFrame->nCursor;
  v->db->lastRowid = pFrame->lastRowid;
  v->nChange = pFrame->nChange;
  v->db->nChange = pFrame->nDbChange;
  capdbVdbeDeleteAuxData(v->db, &v->pAuxData, -1, 0);
  v->pAuxData = pFrame->pAuxData;
  pFrame->pAuxData = 0;
  return pFrame->pc;
}

/*
** Close all cursors.
**
** Also release any dynamic memory held by the VM in the Vdbe.aMem memory
** cell array. This is necessary as the memory cell array may contain
** pointers to VdbeFrame objects, which may in turn contain pointers to
** open cursors.
*/
static void closeAllCursors(Vdbe *p){
  if( p->pFrame ){
    VdbeFrame *pFrame;
    for(pFrame=p->pFrame; pFrame->pParent; pFrame=pFrame->pParent);
    capdbVdbeFrameRestore(pFrame);
    p->pFrame = 0;
    p->nFrame = 0;
  }
  assert( p->nFrame==0 );
  closeCursorsInFrame(p);
  releaseMemArray(p->aMem, p->nMem);
  while( p->pDelFrame ){
    VdbeFrame *pDel = p->pDelFrame;
    p->pDelFrame = pDel->pParent;
    capdbVdbeFrameDelete(pDel);
  }

  /* Delete any auxdata allocations made by the VM */
  if( p->pAuxData ) capdbVdbeDeleteAuxData(p->db, &p->pAuxData, -1, 0);
  assert( p->pAuxData==0 );
}

/*
** Set the number of result columns that will be returned by this SQL
** statement. This is now set at compile time, rather than during
** execution of the vdbe program so that capdb_column_count() can
** be called on an SQL statement before capdb_step().
*/
void capdbVdbeSetNumCols(Vdbe *p, int nResColumn){
  int n;
  capdb *db = p->db;

  if( p->nResAlloc ){
    releaseMemArray(p->aColName, p->nResAlloc*COLNAME_N);
    capdbDbFree(db, p->aColName);
  }
  n = nResColumn*COLNAME_N;
  p->nResColumn = p->nResAlloc = (u16)nResColumn;
  p->aColName = (Mem*)capdbDbMallocRawNN(db, sizeof(Mem)*n );
  if( p->aColName==0 ) return;
  initMemArray(p->aColName, n, db, MEM_Null);
}

/*
** Set the name of the idx'th column to be returned by the SQL statement.
** zName must be a pointer to a nul terminated string.
**
** This call must be made after a call to capdbVdbeSetNumCols().
**
** The final parameter, xDel, must be one of CAPDB_DYNAMIC, CAPDB_STATIC
** or CAPDB_TRANSIENT. If it is CAPDB_DYNAMIC, then the buffer pointed
** to by zName will be freed by capdbDbFree() when the vdbe is destroyed.
*/
int capdbVdbeSetColName(
  Vdbe *p,                         /* Vdbe being configured */
  int idx,                         /* Index of column zName applies to */
  int var,                         /* One of the COLNAME_* constants */
  const char *zName,               /* Pointer to buffer containing name */
  void (*xDel)(void*)              /* Memory management strategy for zName */
){
  int rc;
  Mem *pColName;
  assert( idx<p->nResAlloc );
  assert( var<COLNAME_N );
  if( p->db->mallocFailed ){
    assert( !zName || xDel!=CAPDB_DYNAMIC );
    return CAPDB_NOMEM_BKPT;
  }
  assert( p->aColName!=0 );
  pColName = &(p->aColName[idx+var*p->nResAlloc]);
  rc = capdbVdbeMemSetText(pColName, zName, -1, xDel);
  assert( rc!=0 || !zName || (pColName->flags&MEM_Term)!=0 );
  return rc;
}

/*
** A read or write transaction may or may not be active on database handle
** db. If a transaction is active, commit it. If there is a
** write-transaction spanning more than one database file, this routine
** takes care of the super-journal trickery.
*/
static int vdbeCommit(capdb *db, Vdbe *p){
  int i;
  int nTrans = 0;  /* Number of databases with an active write-transaction
                   ** that are candidates for a two-phase commit using a
                   ** super-journal */
  int rc = CAPDB_OK;
  int needXcommit = 0;

#ifdef CAPDB_OMIT_VIRTUALTABLE
  /* With this option, capdbVtabSync() is defined to be simply
  ** CAPDB_OK so p is not used.
  */
  UNUSED_PARAMETER(p);
#endif

  /* Before doing anything else, call the xSync() callback for any
  ** virtual module tables written in this transaction. This has to
  ** be done before determining whether a super-journal file is
  ** required, as an xSync() callback may add an attached database
  ** to the transaction.
  */
  rc = capdbVtabSync(db, p);

  /* This loop determines (a) if the commit hook should be invoked and
  ** (b) how many database files have open write transactions, not
  ** including the temp database. (b) is important because if more than
  ** one database file has an open write transaction, a super-journal
  ** file is required for an atomic commit.
  */
  for(i=0; rc==CAPDB_OK && i<db->nDb; i++){
    Btree *pBt = db->aDb[i].pBt;
    if( capdbBtreeTxnState(pBt)==CAPDB_TXN_WRITE ){
      /* Whether or not a database might need a super-journal depends upon
      ** its journal mode (among other things).  This matrix determines which
      ** journal modes use a super-journal and which do not */
      static const u8 aMJNeeded[] = {
        /* DELETE   */  1,
        /* PERSIST   */ 1,
        /* OFF       */ 0,
        /* TRUNCATE  */ 1,
        /* MEMORY    */ 0,
        /* WAL       */ 0
      };
      Pager *pPager;   /* Pager associated with pBt */
      needXcommit = 1;
      capdbBtreeEnter(pBt);
      pPager = capdbBtreePager(pBt);
      if( db->aDb[i].safety_level!=PAGER_SYNCHRONOUS_OFF
       && aMJNeeded[capdbPagerGetJournalMode(pPager)]
       && capdbPagerIsMemdb(pPager)==0
      ){
        assert( i!=1 );
        nTrans++;
      }
      rc = capdbPagerExclusiveLock(pPager);
      capdbBtreeLeave(pBt);
    }
  }
  if( rc!=CAPDB_OK ){
    return rc;
  }

  /* If there are any write-transactions at all, invoke the commit hook */
  if( needXcommit && db->xCommitCallback ){
    rc = db->xCommitCallback(db->pCommitArg);
    if( rc ){
      return CAPDB_CONSTRAINT_COMMITHOOK;
    }
  }

  /* The simple case - no more than one database file (not counting the
  ** TEMP database) has a transaction active.   There is no need for the
  ** super-journal.
  **
  ** If the return value of capdbBtreeGetFilename() is a zero length
  ** string, it means the main database is :memory: or a temp file.  In
  ** that case we do not support atomic multi-file commits, so use the
  ** simple case then too.
  */
  if( 0==capdbStrlen30(capdbBtreeGetFilename(db->aDb[0].pBt))
   || nTrans<=1
  ){
    if( needXcommit ){
      for(i=0; rc==CAPDB_OK && i<db->nDb; i++){
        Btree *pBt = db->aDb[i].pBt;
        if( capdbBtreeTxnState(pBt)>=CAPDB_TXN_WRITE ){
          rc = capdbBtreeCommitPhaseOne(pBt, 0);
        }
      }
    }

    /* Do the commit only if all databases successfully complete phase 1.
    ** If one of the BtreeCommitPhaseOne() calls fails, this indicates an
    ** IO error while deleting or truncating a journal file. It is unlikely,
    ** but could happen. In this case abandon processing and return the error.
    */
    for(i=0; rc==CAPDB_OK && i<db->nDb; i++){
      Btree *pBt = db->aDb[i].pBt;
      int txn = capdbBtreeTxnState(pBt);
      if( txn!=CAPDB_TXN_NONE ){
        assert( needXcommit || txn==CAPDB_TXN_READ );
        rc = capdbBtreeCommitPhaseTwo(pBt, 0);
      }
    }
    if( rc==CAPDB_OK ){
      capdbVtabCommit(db);
    }
  }

  /* The complex case - There is a multi-file write-transaction active.
  ** This requires a super-journal file to ensure the transaction is
  ** committed atomically.
  */
#ifndef CAPDB_OMIT_DISKIO
  else{
    capdb_vfs *pVfs = db->pVfs;
    char *zSuper = 0;   /* File-name for the super-journal */
    char const *zMainFile = capdbBtreeGetFilename(db->aDb[0].pBt);
    capdb_file *pSuperJrnl = 0;
    i64 offset = 0;
    int res;
    int retryCount = 0;
    int nMainFile;

    /* Select a super-journal file name */
    nMainFile = capdbStrlen30(zMainFile);
    zSuper = capdbMPrintf(db, "%.4c%s%.16c", 0,zMainFile,0);
    if( zSuper==0 ) return CAPDB_NOMEM_BKPT;
    zSuper += 4;
    do {
      u32 iRandom;
      if( retryCount ){
        if( retryCount>100 ){
          capdb_log(CAPDB_FULL, "MJ delete: %s", zSuper);
          capdbOsDelete(pVfs, zSuper, 0);
          break;
        }else if( retryCount==1 ){
          capdb_log(CAPDB_FULL, "MJ collide: %s", zSuper);
        }
      }
      retryCount++;
      capdb_randomness(sizeof(iRandom), &iRandom);
      capdb_snprintf(13, &zSuper[nMainFile], "-mj%06X9%02X",
                               (iRandom>>8)&0xffffff, iRandom&0xff);
      /* The antipenultimate character of the super-journal name must
      ** be "9" to avoid name collisions when using 8+3 filenames. */
      assert( zSuper[capdbStrlen30(zSuper)-3]=='9' );
      capdbFileSuffix3(zMainFile, zSuper);
      rc = capdbOsAccess(pVfs, zSuper, CAPDB_ACCESS_EXISTS, &res);
    }while( rc==CAPDB_OK && res );
    if( rc==CAPDB_OK ){
      /* Open the super-journal. */
      rc = capdbOsOpenMalloc(pVfs, zSuper, &pSuperJrnl,
          CAPDB_OPEN_READWRITE|CAPDB_OPEN_CREATE|
          CAPDB_OPEN_EXCLUSIVE|CAPDB_OPEN_SUPER_JOURNAL, 0
      );
    }
    if( rc!=CAPDB_OK ){
      capdbDbFree(db, zSuper-4);
      return rc;
    }

    /* Write the name of each database file in the transaction into the new
    ** super-journal file. If an error occurs at this point close
    ** and delete the super-journal file. All the individual journal files
    ** still have 'null' as the super-journal pointer, so they will roll
    ** back independently if a failure occurs.
    */
    for(i=0; i<db->nDb; i++){
      Btree *pBt = db->aDb[i].pBt;
      if( capdbBtreeTxnState(pBt)==CAPDB_TXN_WRITE ){
        char const *zFile = capdbBtreeGetJournalname(pBt);
        if( zFile==0 ){
          continue;  /* Ignore TEMP and :memory: databases */
        }
        assert( zFile[0]!=0 );
        rc = capdbOsWrite(pSuperJrnl, zFile, capdbStrlen30(zFile)+1,offset);
        offset += capdbStrlen30(zFile)+1;
        if( rc!=CAPDB_OK ){
          capdbOsCloseFree(pSuperJrnl);
          capdbOsDelete(pVfs, zSuper, 0);
          capdbDbFree(db, zSuper-4);
          return rc;
        }
      }
    }

    /* Sync the super-journal file. If the IOCAP_SEQUENTIAL device
    ** flag is set this is not required.
    */
    if( 0==(capdbOsDeviceCharacteristics(pSuperJrnl)&CAPDB_IOCAP_SEQUENTIAL)
     && CAPDB_OK!=(rc = capdbOsSync(pSuperJrnl, CAPDB_SYNC_NORMAL))
    ){
      capdbOsCloseFree(pSuperJrnl);
      capdbOsDelete(pVfs, zSuper, 0);
      capdbDbFree(db, zSuper-4);
      return rc;
    }

    /* Sync all the db files involved in the transaction. The same call
    ** sets the super-journal pointer in each individual journal. If
    ** an error occurs here, do not delete the super-journal file.
    **
    ** If the error occurs during the first call to
    ** capdbBtreeCommitPhaseOne(), then there is a chance that the
    ** super-journal file will be orphaned. But we cannot delete it,
    ** in case the super-journal file name was written into the journal
    ** file before the failure occurred.
    */
    for(i=0; rc==CAPDB_OK && i<db->nDb; i++){
      Btree *pBt = db->aDb[i].pBt;
      if( pBt ){
        rc = capdbBtreeCommitPhaseOne(pBt, zSuper);
      }
    }
    capdbOsCloseFree(pSuperJrnl);
    assert( rc!=CAPDB_BUSY );
    if( rc!=CAPDB_OK ){
      capdbDbFree(db, zSuper-4);
      return rc;
    }

    /* Delete the super-journal file. This commits the transaction. After
    ** doing this the directory is synced again before any individual
    ** transaction files are deleted.
    */
    rc = capdbOsDelete(pVfs, zSuper, 1);
    capdbDbFree(db, zSuper-4);
    zSuper = 0;
    if( rc ){
      return rc;
    }

    /* All files and directories have already been synced, so the following
    ** calls to capdbBtreeCommitPhaseTwo() are only closing files and
    ** deleting or truncating journals. If something goes wrong while
    ** this is happening we don't really care. The integrity of the
    ** transaction is already guaranteed, but some stray 'cold' journals
    ** may be lying around. Returning an error code won't help matters.
    */
    disable_simulated_io_errors();
    capdbBeginBenignMalloc();
    for(i=0; i<db->nDb; i++){
      Btree *pBt = db->aDb[i].pBt;
      if( pBt ){
        capdbBtreeCommitPhaseTwo(pBt, 1);
      }
    }
    capdbEndBenignMalloc();
    enable_simulated_io_errors();

    capdbVtabCommit(db);
  }
#endif

  return rc;
}

/*
** This routine checks that the capdb.nVdbeActive count variable
** matches the number of vdbe's in the list capdb.pVdbe that are
** currently active. An assertion fails if the two counts do not match.
** This is an internal self-check only - it is not an essential processing
** step.
**
** This is a no-op if NDEBUG is defined.
*/
#ifndef NDEBUG
static void checkActiveVdbeCnt(capdb *db){
  Vdbe *p;
  int cnt = 0;
  int nWrite = 0;
  int nRead = 0;
  p = db->pVdbe;
  while( p ){
    if( capdb_stmt_busy((capdb_stmt*)p) ){
      cnt++;
      if( p->readOnly==0 ) nWrite++;
      if( p->bIsReader ) nRead++;
    }
    p = p->pVNext;
  }
  assert( cnt==db->nVdbeActive );
  assert( nWrite==db->nVdbeWrite );
  assert( nRead==db->nVdbeRead );
}
#else
#define checkActiveVdbeCnt(x)
#endif

/*
** If the Vdbe passed as the first argument opened a statement-transaction,
** close it now. Argument eOp must be either SAVEPOINT_ROLLBACK or
** SAVEPOINT_RELEASE. If it is SAVEPOINT_ROLLBACK, then the statement
** transaction is rolled back. If eOp is SAVEPOINT_RELEASE, then the
** statement transaction is committed.
**
** If an IO error occurs, an CAPDB_IOERR_XXX error code is returned.
** Otherwise CAPDB_OK.
*/
static CAPDB_NOINLINE int vdbeCloseStatement(Vdbe *p, int eOp){
  capdb *const db = p->db;
  int rc = CAPDB_OK;
  int i;
  const int iSavepoint = p->iStatement-1;

  assert( eOp==SAVEPOINT_ROLLBACK || eOp==SAVEPOINT_RELEASE);
  assert( db->nStatement>0 );
  assert( p->iStatement==(db->nStatement+db->nSavepoint) );

  for(i=0; i<db->nDb; i++){
    int rc2 = CAPDB_OK;
    Btree *pBt = db->aDb[i].pBt;
    if( pBt ){
      if( eOp==SAVEPOINT_ROLLBACK ){
        rc2 = capdbBtreeSavepoint(pBt, SAVEPOINT_ROLLBACK, iSavepoint);
      }
      if( rc2==CAPDB_OK ){
        rc2 = capdbBtreeSavepoint(pBt, SAVEPOINT_RELEASE, iSavepoint);
      }
      if( rc==CAPDB_OK ){
        rc = rc2;
      }
    }
  }
  db->nStatement--;
  p->iStatement = 0;

  if( rc==CAPDB_OK ){
    if( eOp==SAVEPOINT_ROLLBACK ){
      rc = capdbVtabSavepoint(db, SAVEPOINT_ROLLBACK, iSavepoint);
    }
    if( rc==CAPDB_OK ){
      rc = capdbVtabSavepoint(db, SAVEPOINT_RELEASE, iSavepoint);
    }
  }

  /* If the statement transaction is being rolled back, also restore the
  ** database handles deferred constraint counter to the value it had when
  ** the statement transaction was opened.  */
  if( eOp==SAVEPOINT_ROLLBACK ){
    db->nDeferredCons = p->nStmtDefCons;
    db->nDeferredImmCons = p->nStmtDefImmCons;
  }
  return rc;
}
int capdbVdbeCloseStatement(Vdbe *p, int eOp){
  if( p->db->nStatement && p->iStatement ){
    return vdbeCloseStatement(p, eOp);
  }
  return CAPDB_OK;
}


/*
** These functions are called when a transaction opened by the database
** handle associated with the VM passed as an argument is about to be
** committed. If there are outstanding foreign key constraint violations
** return an error code. Otherwise, CAPDB_OK.
**
** If there are outstanding FK violations and this function returns
** non-zero, set the result of the VM to CAPDB_CONSTRAINT_FOREIGNKEY
** and write an error message to it.
*/
#ifndef CAPDB_OMIT_FOREIGN_KEY
static CAPDB_NOINLINE int vdbeFkError(Vdbe *p){
  p->rc = CAPDB_CONSTRAINT_FOREIGNKEY;
  p->errorAction = OE_Abort;
  capdbVdbeError(p, "FOREIGN KEY constraint failed");
  if( (p->prepFlags & CAPDB_PREPARE_SAVESQL)==0 ) return CAPDB_ERROR;
  return CAPDB_CONSTRAINT_FOREIGNKEY;
}
int capdbVdbeCheckFkImmediate(Vdbe *p){
  if( p->nFkConstraint==0 ) return CAPDB_OK;
  return vdbeFkError(p);
}
int capdbVdbeCheckFkDeferred(Vdbe *p){
  capdb *db = p->db;
  if( (db->nDeferredCons+db->nDeferredImmCons)==0 ) return CAPDB_OK;
  return vdbeFkError(p);
}
#endif

/*
** This routine is called the when a VDBE tries to halt.  If the VDBE
** has made changes and is in autocommit mode, then commit those
** changes.  If a rollback is needed, then do the rollback.
**
** This routine is the only way to move the capdbeOpenState of a VM from
** CAPDB_STATE_RUN to CAPDB_STATE_HALT.  It is harmless to
** call this on a VM that is in the CAPDB_STATE_HALT state.
**
** Return an error code.  If the commit could not complete because of
** lock contention, return CAPDB_BUSY.  If CAPDB_BUSY is returned, it
** means the close did not happen and needs to be repeated.
*/
int capdbVdbeHalt(Vdbe *p){
  int rc;                         /* Used to store transient return codes */
  capdb *db = p->db;

  /* This function contains the logic that determines if a statement or
  ** transaction will be committed or rolled back as a result of the
  ** execution of this virtual machine.
  **
  ** If any of the following errors occur:
  **
  **     CAPDB_NOMEM
  **     CAPDB_IOERR
  **     CAPDB_FULL
  **     CAPDB_INTERRUPT
  **
  ** Then the internal cache might have been left in an inconsistent
  ** state.  We need to rollback the statement transaction, if there is
  ** one, or the complete transaction if there is no statement transaction.
  */

  assert( p->eVdbeState==VDBE_RUN_STATE );
  if( db->mallocFailed ){
    p->rc = CAPDB_NOMEM_BKPT;
  }
  closeAllCursors(p);
  checkActiveVdbeCnt(db);

  /* No commit or rollback needed if the program never started or if the
  ** SQL statement does not read or write a database file.  */
  if( p->bIsReader ){
    int mrc;   /* Primary error code from p->rc */
    int eStatementOp = 0;
    int isSpecialError;            /* Set to true if a 'special' error */

    /* Lock all btrees used by the statement */
    capdbVdbeEnter(p);

    /* Check for one of the special errors */
    if( p->rc ){
      mrc = p->rc & 0xff;
      isSpecialError = mrc==CAPDB_NOMEM
                    || mrc==CAPDB_IOERR
                    || mrc==CAPDB_INTERRUPT
                    || mrc==CAPDB_FULL;
    }else{
      mrc = isSpecialError = 0;
    }
    if( isSpecialError ){
      /* If the query was read-only and the error code is CAPDB_INTERRUPT,
      ** no rollback is necessary. Otherwise, at least a savepoint
      ** transaction must be rolled back to restore the database to a
      ** consistent state.
      **
      ** Even if the statement is read-only, it is important to perform
      ** a statement or transaction rollback operation. If the error
      ** occurred while writing to the journal, sub-journal or database
      ** file as part of an effort to free up cache space (see function
      ** pagerStress() in pager.c), the rollback is required to restore
      ** the pager to a consistent state.
      */
      if( !p->readOnly || mrc!=CAPDB_INTERRUPT ){
        if( (mrc==CAPDB_NOMEM || mrc==CAPDB_FULL) && p->usesStmtJournal ){
          eStatementOp = SAVEPOINT_ROLLBACK;
        }else{
          /* We are forced to roll back the active transaction. Before doing
          ** so, abort any other statements this handle currently has active.
          */
          capdbRollbackAll(db, CAPDB_ABORT_ROLLBACK);
          capdbCloseSavepoints(db);
          db->autoCommit = 1;
          p->nChange = 0;
        }
      }
    }

    /* Check for immediate foreign key violations. */
    if( p->rc==CAPDB_OK || (p->errorAction==OE_Fail && !isSpecialError) ){
      (void)capdbVdbeCheckFkImmediate(p);
    }

    /* If the auto-commit flag is set and this is the only active writer
    ** VM, then we do either a commit or rollback of the current transaction.
    **
    ** Note: This block also runs if one of the special errors handled
    ** above has occurred.
    */
    if( !capdbVtabInSync(db)
     && db->autoCommit
     && db->nVdbeWrite==(p->readOnly==0)
    ){
      if( p->rc==CAPDB_OK || (p->errorAction==OE_Fail && !isSpecialError) ){
        rc = capdbVdbeCheckFkDeferred(p);
        if( rc!=CAPDB_OK ){
          if( NEVER(p->readOnly) ){
            capdbVdbeLeave(p);
            return CAPDB_ERROR;
          }
          rc = CAPDB_CONSTRAINT_FOREIGNKEY;
        }else if( db->flags & CAPDB_CorruptRdOnly ){
          rc = CAPDB_CORRUPT;
          db->flags &= ~CAPDB_CorruptRdOnly;
        }else{
          /* The auto-commit flag is true, the vdbe program was successful
          ** or hit an 'OR FAIL' constraint and there are no deferred foreign
          ** key constraints to hold up the transaction. This means a commit
          ** is required. */
          rc = vdbeCommit(db, p);
        }
        if( rc==CAPDB_BUSY && p->readOnly ){
          capdbVdbeLeave(p);
          return CAPDB_BUSY;
        }else if( rc!=CAPDB_OK ){
          capdbSystemError(db, rc);
          p->rc = rc;
          capdbRollbackAll(db, CAPDB_OK);
          p->nChange = 0;
        }else{
          db->nDeferredCons = 0;
          db->nDeferredImmCons = 0;
          db->flags &= ~(u64)CAPDB_DeferFKs;
          capdbCommitInternalChanges(db);
        }
      }else if( p->rc==CAPDB_SCHEMA && db->nVdbeActive>1 ){
        p->nChange = 0;
      }else{
        capdbRollbackAll(db, CAPDB_OK);
        p->nChange = 0;
      }
      db->nStatement = 0;
    }else if( eStatementOp==0 ){
      if( p->rc==CAPDB_OK || p->errorAction==OE_Fail ){
        eStatementOp = SAVEPOINT_RELEASE;
      }else if( p->errorAction==OE_Abort ){
        eStatementOp = SAVEPOINT_ROLLBACK;
      }else{
        capdbRollbackAll(db, CAPDB_ABORT_ROLLBACK);
        capdbCloseSavepoints(db);
        db->autoCommit = 1;
        p->nChange = 0;
      }
    }
 
    /* If eStatementOp is non-zero, then a statement transaction needs to
    ** be committed or rolled back. Call capdbVdbeCloseStatement() to
    ** do so. If this operation returns an error, and the current statement
    ** error code is CAPDB_OK or CAPDB_CONSTRAINT, then promote the
    ** current statement error code.
    */
    if( eStatementOp ){
      rc = capdbVdbeCloseStatement(p, eStatementOp);
      if( rc ){
        if( p->rc==CAPDB_OK || (p->rc&0xff)==CAPDB_CONSTRAINT ){
          p->rc = rc;
          capdbDbFree(db, p->zErrMsg);
          p->zErrMsg = 0;
        }
        capdbRollbackAll(db, CAPDB_ABORT_ROLLBACK);
        capdbCloseSavepoints(db);
        db->autoCommit = 1;
        p->nChange = 0;
      }
    }
 
    /* If this was an INSERT, UPDATE or DELETE and no statement transaction
    ** has been rolled back, update the database connection change-counter.
    */
    if( p->changeCntOn ){
      if( eStatementOp!=SAVEPOINT_ROLLBACK ){
        capdbVdbeSetChanges(db, p->nChange);
      }else{
        capdbVdbeSetChanges(db, 0);
      }
      p->nChange = 0;
    }

    /* Release the locks */
    capdbVdbeLeave(p);
  }

  /* We have successfully halted and closed the VM.  Record this fact. */
  db->nVdbeActive--;
  if( !p->readOnly ) db->nVdbeWrite--;
  if( p->bIsReader ) db->nVdbeRead--;
  assert( db->nVdbeActive>=db->nVdbeRead );
  assert( db->nVdbeRead>=db->nVdbeWrite );
  assert( db->nVdbeWrite>=0 );
  p->eVdbeState = VDBE_HALT_STATE;
  checkActiveVdbeCnt(db);
  if( db->mallocFailed ){
    p->rc = CAPDB_NOMEM_BKPT;
  }

  /* If the auto-commit flag is set to true, then any locks that were held
  ** by connection db have now been released. Call capdbConnectionUnlocked()
  ** to invoke any required unlock-notify callbacks.
  */
  if( db->autoCommit ){
    capdbConnectionUnlocked(db);
  }

  assert( db->nVdbeActive>0 || db->autoCommit==0 || db->nStatement==0 );
  return (p->rc==CAPDB_BUSY ? CAPDB_BUSY : CAPDB_OK);
}


/*
** Each VDBE holds the result of the most recent capdb_step() call
** in p->rc.  This routine sets that result back to CAPDB_OK.
*/
void capdbVdbeResetStepResult(Vdbe *p){
  p->rc = CAPDB_OK;
}

/*
** Copy the error code and error message belonging to the VDBE passed
** as the first argument to its database handle (so that they will be
** returned by calls to capdb_errcode() and capdb_errmsg()).
**
** This function does not clear the VDBE error code or message, just
** copies them to the database handle.
*/
int capdbVdbeTransferError(Vdbe *p){
  capdb *db = p->db;
  int rc = p->rc;
  if( p->zErrMsg ){
    db->bBenignMalloc++;
    capdbBeginBenignMalloc();
    if( db->pErr==0 ) db->pErr = capdbValueNew(db);
    capdbValueSetStr(db->pErr, -1, p->zErrMsg, CAPDB_UTF8, CAPDB_TRANSIENT);
    capdbEndBenignMalloc();
    db->bBenignMalloc--;
  }else if( db->pErr ){
    capdbValueSetNull(db->pErr);
  }
  db->errCode = rc;
  db->errByteOffset = -1;
  return rc;
}

#ifdef CAPDB_ENABLE_SQLLOG
/*
** If an CAPDB_CONFIG_SQLLOG hook is registered and the VM has been run,
** invoke it.
*/
static void vdbeInvokeSqllog(Vdbe *v){
  if( capdbGlobalConfig.xSqllog && v->rc==CAPDB_OK && v->zSql && v->pc>=0 ){
    char *zExpanded = capdbVdbeExpandSql(v, v->zSql);
    assert( v->db->init.busy==0 );
    if( zExpanded ){
      capdbGlobalConfig.xSqllog(
          capdbGlobalConfig.pSqllogArg, v->db, zExpanded, 1
      );
      capdbDbFree(v->db, zExpanded);
    }
  }
}
#else
# define vdbeInvokeSqllog(x)
#endif

/*
** Clean up a VDBE after execution but do not delete the VDBE just yet.
** Write any error messages into *pzErrMsg.  Return the result code.
**
** After this routine is run, the VDBE should be ready to be executed
** again.
**
** To look at it another way, this routine resets the state of the
** virtual machine from VDBE_RUN_STATE or VDBE_HALT_STATE back to
** VDBE_READY_STATE.
*/
int capdbVdbeReset(Vdbe *p){
#if defined(CAPDB_DEBUG) || defined(VDBE_PROFILE)
  int i;
#endif

  capdb *db;
  db = p->db;

  /* If the VM did not run to completion or if it encountered an
  ** error, then it might not have been halted properly.  So halt
  ** it now.
  */
  if( p->eVdbeState==VDBE_RUN_STATE ) capdbVdbeHalt(p);

  /* If the VDBE has been run even partially, then transfer the error code
  ** and error message from the VDBE into the main database structure.  But
  ** if the VDBE has just been set to run but has not actually executed any
  ** instructions yet, leave the main database error information unchanged.
  */
  if( p->pc>=0 ){
    vdbeInvokeSqllog(p);
    if( db->pErr || p->zErrMsg ){
      capdbVdbeTransferError(p);
    }else{
      db->errCode = p->rc;
    }
  }

  /* Reset register contents and reclaim error message memory.
  */
#ifdef CAPDB_DEBUG
  /* Execute assert() statements to ensure that the Vdbe.apCsr[] and
  ** Vdbe.aMem[] arrays have already been cleaned up.  */
  if( p->apCsr ) for(i=0; i<p->nCursor; i++) assert( p->apCsr[i]==0 );
  if( p->aMem ){
    for(i=0; i<p->nMem; i++) assert( p->aMem[i].flags==MEM_Undefined );
  }
#endif
  if( p->zErrMsg ){
    capdbDbFree(db, p->zErrMsg);
    p->zErrMsg = 0;
  }
  p->pResultRow = 0;
#ifdef CAPDB_DEBUG
  p->nWrite = 0;
#endif

  /* Save profiling information from this VDBE run.
  */
#ifdef VDBE_PROFILE
  {
    FILE *out = fopen("vdbe_profile.out", "a");
    if( out ){
      fprintf(out, "---- ");
      for(i=0; i<p->nOp; i++){
        fprintf(out, "%02x", p->aOp[i].opcode);
      }
      fprintf(out, "\n");
      if( p->zSql ){
        char c, pc = 0;
        fprintf(out, "-- ");
        for(i=0; (c = p->zSql[i])!=0; i++){
          if( pc=='\n' ) fprintf(out, "-- ");
          putc(c, out);
          pc = c;
        }
        if( pc!='\n' ) fprintf(out, "\n");
      }
      for(i=0; i<p->nOp; i++){
        char zHdr[100];
        i64 cnt = p->aOp[i].nExec;
        i64 cycles = p->aOp[i].nCycle;
        capdb_snprintf(sizeof(zHdr), zHdr, "%6u %12llu %8llu ",
           cnt,
           cycles,
           cnt>0 ? cycles/cnt : 0
        );
        fprintf(out, "%s", zHdr);
        capdbVdbePrintOp(out, i, &p->aOp[i]);
      }
      fclose(out);
    }
  }
#endif
  return p->rc & db->errMask;
}

/*
** Clean up and delete a VDBE after execution.  Return an integer which is
** the result code.  Write any error message text into *pzErrMsg.
*/
int capdbVdbeFinalize(Vdbe *p){
  int rc = CAPDB_OK;
  assert( VDBE_RUN_STATE>VDBE_READY_STATE );
  assert( VDBE_HALT_STATE>VDBE_READY_STATE );
  assert( VDBE_INIT_STATE<VDBE_READY_STATE );
  if( p->eVdbeState>=VDBE_READY_STATE ){
    rc = capdbVdbeReset(p);
    assert( (rc & p->db->errMask)==rc );
  }
  capdbVdbeDelete(p);
  return rc;
}

/*
** If parameter iOp is less than zero, then invoke the destructor for
** all auxiliary data pointers currently cached by the VM passed as
** the first argument.
**
** Or, if iOp is greater than or equal to zero, then the destructor is
** only invoked for those auxiliary data pointers created by the user
** function invoked by the OP_Function opcode at instruction iOp of
** VM pVdbe, and only then if:
**
**    * the associated function parameter is the 32nd or later (counting
**      from left to right), or
**
**    * the corresponding bit in argument mask is clear (where the first
**      function parameter corresponds to bit 0 etc.).
*/
void capdbVdbeDeleteAuxData(capdb *db, AuxData **pp, int iOp, int mask){
  while( *pp ){
    AuxData *pAux = *pp;
    if( (iOp<0)
     || (pAux->iAuxOp==iOp
          && pAux->iAuxArg>=0
          && (pAux->iAuxArg>31 || !(mask & MASKBIT32(pAux->iAuxArg))))
    ){
      testcase( pAux->iAuxArg==31 );
      if( pAux->xDeleteAux ){
        pAux->xDeleteAux(pAux->pAux);
      }
      *pp = pAux->pNextAux;
      capdbDbFree(db, pAux);
    }else{
      pp= &pAux->pNextAux;
    }
  }
}

/*
** Free all memory associated with the Vdbe passed as the second argument,
** except for object itself, which is preserved.
**
** The difference between this function and capdbVdbeDelete() is that
** VdbeDelete() also unlinks the Vdbe from the list of VMs associated with
** the database connection and frees the object itself.
*/
static void capdbVdbeClearObject(capdb *db, Vdbe *p){
  SubProgram *pSub, *pNext;
  assert( db!=0 );
  assert( p->db==0 || p->db==db );
  if( p->aColName ){
    releaseMemArray(p->aColName, p->nResAlloc*COLNAME_N);
    capdbDbNNFreeNN(db, p->aColName);
  }
  for(pSub=p->pProgram; pSub; pSub=pNext){
    pNext = pSub->pNext;
    vdbeFreeOpArray(db, pSub->aOp, pSub->nOp);
    capdbDbFree(db, pSub);
  }
  if( p->eVdbeState!=VDBE_INIT_STATE ){
    releaseMemArray(p->aVar, p->nVar);
    if( p->pVList ) capdbDbNNFreeNN(db, p->pVList);
    if( p->pFree ) capdbDbNNFreeNN(db, p->pFree);
  }
  vdbeFreeOpArray(db, p->aOp, p->nOp);
  if( p->zSql ) capdbDbNNFreeNN(db, p->zSql);
#ifdef CAPDB_ENABLE_NORMALIZE
  capdbDbFree(db, p->zNormSql);
  {
    DblquoteStr *pThis, *pNxt;
    for(pThis=p->pDblStr; pThis; pThis=pNxt){
      pNxt = pThis->pNextStr;
      capdbDbFree(db, pThis);
    }
  }
#endif
#ifdef CAPDB_ENABLE_STMT_SCANSTATUS
  {
    int i;
    for(i=0; i<p->nScan; i++){
      capdbDbFree(db, p->aScan[i].zName);
    }
    capdbDbFree(db, p->aScan);
  }
#endif
}

/*
** Delete an entire VDBE.
*/
void capdbVdbeDelete(Vdbe *p){
  capdb *db;

  assert( p!=0 );
  db = p->db;
  assert( db!=0 );
  assert( capdb_mutex_held(db->mutex) );
  capdbVdbeClearObject(db, p);
  if( db->pnBytesFreed==0 ){
    assert( p->ppVPrev!=0 );
    *p->ppVPrev = p->pVNext;
    if( p->pVNext ){
      p->pVNext->ppVPrev = p->ppVPrev;
    }
  }
  capdbDbNNFreeNN(db, p);
}

/*
** The cursor "p" has a pending seek operation that has not yet been
** carried out.  Seek the cursor now.  If an error occurs, return
** the appropriate error code.
*/
int CAPDB_NOINLINE capdbVdbeFinishMoveto(VdbeCursor *p){
  int res, rc;
#ifdef CAPDB_TEST
  extern int capdb_search_count;
#endif
  assert( p->deferredMoveto );
  assert( p->isTable );
  assert( p->eCurType==CURTYPE_BTREE );
  rc = capdbBtreeTableMoveto(p->uc.pCursor, p->movetoTarget, 0, &res);
  if( rc ) return rc;
  if( res!=0 ) return CAPDB_CORRUPT_BKPT;
#ifdef CAPDB_TEST
  capdb_search_count++;
#endif
  p->deferredMoveto = 0;
  p->cacheStatus = CACHE_STALE;
  return CAPDB_OK;
}

/*
** Something has moved cursor "p" out of place.  Maybe the row it was
** pointed to was deleted out from under it.  Or maybe the btree was
** rebalanced.  Whatever the cause, try to restore "p" to the place it
** is supposed to be pointing.  If the row was deleted out from under the
** cursor, set the cursor to point to a NULL row.
*/
int CAPDB_NOINLINE capdbVdbeHandleMovedCursor(VdbeCursor *p){
  int isDifferentRow, rc;
  assert( p->eCurType==CURTYPE_BTREE );
  assert( p->uc.pCursor!=0 );
  assert( capdbBtreeCursorHasMoved(p->uc.pCursor) );
  rc = capdbBtreeCursorRestore(p->uc.pCursor, &isDifferentRow);
  p->cacheStatus = CACHE_STALE;
  if( isDifferentRow ) p->nullRow = 1;
  return rc;
}

/*
** Check to ensure that the cursor is valid.  Restore the cursor
** if need be.  Return any I/O error from the restore operation.
*/
int capdbVdbeCursorRestore(VdbeCursor *p){
  assert( p->eCurType==CURTYPE_BTREE || IsNullCursor(p) );
  if( capdbBtreeCursorHasMoved(p->uc.pCursor) ){
    return capdbVdbeHandleMovedCursor(p);
  }
  return CAPDB_OK;
}

/*
** The following functions:
**
** capdbVdbeSerialType()
** capdbVdbeSerialTypeLen()
** capdbVdbeSerialLen()
** capdbVdbeSerialPut()  <--- in-lined into OP_MakeRecord as of 2022-04-02
** capdbVdbeSerialGet()
**
** encapsulate the code that serializes values for storage in SQLite
** data and index records. Each serialized value consists of a
** 'serial-type' and a blob of data. The serial type is an 8-byte unsigned
** integer, stored as a varint.
**
** In an SQLite index record, the serial type is stored directly before
** the blob of data that it corresponds to. In a table record, all serial
** types are stored at the start of the record, and the blobs of data at
** the end. Hence these functions allow the caller to handle the
** serial-type and data blob separately.
**
** The following table describes the various storage classes for data:
**
**   serial type        bytes of data      type
**   --------------     ---------------    ---------------
**      0                     0            NULL
**      1                     1            signed integer
**      2                     2            signed integer
**      3                     3            signed integer
**      4                     4            signed integer
**      5                     6            signed integer
**      6                     8            signed integer
**      7                     8            IEEE float
**      8                     0            Integer constant 0
**      9                     0            Integer constant 1
**     10,11                               reserved for expansion
**    N>=12 and even       (N-12)/2        BLOB
**    N>=13 and odd        (N-13)/2        text
**
** The 8 and 9 types were added in 3.3.0, file format 4.  Prior versions
** of SQLite will not understand those serial types.
*/

#if 0 /* Inlined into the OP_MakeRecord opcode */
/*
** Return the serial-type for the value stored in pMem.
**
** This routine might convert a large MEM_IntReal value into MEM_Real.
**
** 2019-07-11:  The primary user of this subroutine was the OP_MakeRecord
** opcode in the byte-code engine.  But by moving this routine in-line, we
** can omit some redundant tests and make that opcode a lot faster.  So
** this routine is now only used by the STAT3 logic and STAT3 support has
** ended.  The code is kept here for historical reference only.
*/
u32 capdbVdbeSerialType(Mem *pMem, int file_format, u32 *pLen){
  int flags = pMem->flags;
  u32 n;

  assert( pLen!=0 );
  if( flags&MEM_Null ){
    *pLen = 0;
    return 0;
  }
  if( flags&(MEM_Int|MEM_IntReal) ){
    /* Figure out whether to use 1, 2, 4, 6 or 8 bytes. */
#   define MAX_6BYTE ((((i64)0x00008000)<<32)-1)
    i64 i = pMem->u.i;
    u64 u;
    testcase( flags & MEM_Int );
    testcase( flags & MEM_IntReal );
    if( i<0 ){
      u = ~i;
    }else{
      u = i;
    }
    if( u<=127 ){
      if( (i&1)==i && file_format>=4 ){
        *pLen = 0;
        return 8+(u32)u;
      }else{
        *pLen = 1;
        return 1;
      }
    }
    if( u<=32767 ){ *pLen = 2; return 2; }
    if( u<=8388607 ){ *pLen = 3; return 3; }
    if( u<=2147483647 ){ *pLen = 4; return 4; }
    if( u<=MAX_6BYTE ){ *pLen = 6; return 5; }
    *pLen = 8;
    if( flags&MEM_IntReal ){
      /* If the value is IntReal and is going to take up 8 bytes to store
      ** as an integer, then we might as well make it an 8-byte floating
      ** point value */
      pMem->u.r = (double)pMem->u.i;
      pMem->flags &= ~MEM_IntReal;
      pMem->flags |= MEM_Real;
      return 7;
    }
    return 6;
  }
  if( flags&MEM_Real ){
    *pLen = 8;
    return 7;
  }
  assert( pMem->db->mallocFailed || flags&(MEM_Str|MEM_Blob) );
  assert( pMem->n>=0 );
  n = (u32)pMem->n;
  if( flags & MEM_Zero ){
    n += pMem->u.nZero;
  }
  *pLen = n;
  return ((n*2) + 12 + ((flags&MEM_Str)!=0));
}
#endif /* inlined into OP_MakeRecord */

/*
** The sizes for serial types less than 128
*/
const u8 capdbSmallTypeSizes[128] = {
        /*  0   1   2   3   4   5   6   7   8   9 */  
/*   0 */   0,  1,  2,  3,  4,  6,  8,  8,  0,  0,
/*  10 */   0,  0,  0,  0,  1,  1,  2,  2,  3,  3,
/*  20 */   4,  4,  5,  5,  6,  6,  7,  7,  8,  8,
/*  30 */   9,  9, 10, 10, 11, 11, 12, 12, 13, 13,
/*  40 */  14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
/*  50 */  19, 19, 20, 20, 21, 21, 22, 22, 23, 23,
/*  60 */  24, 24, 25, 25, 26, 26, 27, 27, 28, 28,
/*  70 */  29, 29, 30, 30, 31, 31, 32, 32, 33, 33,
/*  80 */  34, 34, 35, 35, 36, 36, 37, 37, 38, 38,
/*  90 */  39, 39, 40, 40, 41, 41, 42, 42, 43, 43,
/* 100 */  44, 44, 45, 45, 46, 46, 47, 47, 48, 48,
/* 110 */  49, 49, 50, 50, 51, 51, 52, 52, 53, 53,
/* 120 */  54, 54, 55, 55, 56, 56, 57, 57
};

/*
** Return the length of the data corresponding to the supplied serial-type.
*/
u32 capdbVdbeSerialTypeLen(u32 serial_type){
  if( serial_type>=128 ){
    return (serial_type-12)/2;
  }else{
    assert( serial_type<12
            || capdbSmallTypeSizes[serial_type]==(serial_type - 12)/2 );
    return capdbSmallTypeSizes[serial_type];
  }
}
u8 capdbVdbeOneByteSerialTypeLen(u8 serial_type){
  assert( serial_type<128 );
  return capdbSmallTypeSizes[serial_type]; 
}

/*
** If we are on an architecture with mixed-endian floating
** points (ex: ARM7) then swap the lower 4 bytes with the
** upper 4 bytes.  Return the result.
**
** For most architectures, this is a no-op.
**
** (later):  It is reported to me that the mixed-endian problem
** on ARM7 is an issue with GCC, not with the ARM7 chip.  It seems
** that early versions of GCC stored the two words of a 64-bit
** float in the wrong order.  And that error has been propagated
** ever since.  The blame is not necessarily with GCC, though.
** GCC might have just copying the problem from a prior compiler.
** I am also told that newer versions of GCC that follow a different
** ABI get the byte order right.
**
** Developers using SQLite on an ARM7 should compile and run their
** application using -DCAPDB_DEBUG=1 at least once.  With DEBUG
** enabled, some asserts below will ensure that the byte order of
** floating point values is correct.
**
** (2007-08-30)  Frank van Vugt has studied this problem closely
** and has send his findings to the SQLite developers.  Frank
** writes that some Linux kernels offer floating point hardware
** emulation that uses only 32-bit mantissas instead of a full
** 48-bits as required by the IEEE standard.  (This is the
** CONFIG_FPE_FASTFPE option.)  On such systems, floating point
** byte swapping becomes very complicated.  To avoid problems,
** the necessary byte swapping is carried out using a 64-bit integer
** rather than a 64-bit float.  Frank assures us that the code here
** works for him.  We, the developers, have no way to independently
** verify this, but Frank seems to know what he is talking about
** so we trust him.
*/
#ifdef CAPDB_MIXED_ENDIAN_64BIT_FLOAT
u64 capdbFloatSwap(u64 in){
  union {
    u64 r;
    u32 i[2];
  } u;
  u32 t;

  u.r = in;
  t = u.i[0];
  u.i[0] = u.i[1];
  u.i[1] = t;
  return u.r;
}
#endif /* CAPDB_MIXED_ENDIAN_64BIT_FLOAT */


/* Input "x" is a sequence of unsigned characters that represent a
** big-endian integer.  Return the equivalent native integer
*/
#define ONE_BYTE_INT(x)    ((i8)(x)[0])
#define TWO_BYTE_INT(x)    (256*(i8)((x)[0])|(x)[1])
#define THREE_BYTE_INT(x)  (65536*(i8)((x)[0])|((x)[1]<<8)|(x)[2])
#define FOUR_BYTE_UINT(x)  (((u32)(x)[0]<<24)|((x)[1]<<16)|((x)[2]<<8)|(x)[3])
#define FOUR_BYTE_INT(x) (16777216*(i8)((x)[0])|((x)[1]<<16)|((x)[2]<<8)|(x)[3])

/*
** Deserialize the data blob pointed to by buf as serial type serial_type
** and store the result in pMem.
**
** This function is implemented as two separate routines for performance.
** The few cases that require local variables are broken out into a separate
** routine so that in most cases the overhead of moving the stack pointer
** is avoided.
*/
static void serialGet(
  const unsigned char *buf,     /* Buffer to deserialize from */
  u32 serial_type,              /* Serial type to deserialize */
  Mem *pMem                     /* Memory cell to write value into */
){
  u64 x = FOUR_BYTE_UINT(buf);
  u32 y = FOUR_BYTE_UINT(buf+4);
  x = (x<<32) + y;
  if( serial_type==6 ){
    /* EVIDENCE-OF: R-29851-52272 Value is a big-endian 64-bit
    ** twos-complement integer. */
    pMem->u.i = *(i64*)&x;
    pMem->flags = MEM_Int;
    testcase( pMem->u.i<0 );
  }else{
    /* EVIDENCE-OF: R-57343-49114 Value is a big-endian IEEE 754-2008 64-bit
    ** floating point number. */
#if !defined(NDEBUG) && !defined(CAPDB_OMIT_FLOATING_POINT)
    /* Verify that integers and floating point values use the same
    ** byte order.  Or, that if CAPDB_MIXED_ENDIAN_64BIT_FLOAT is
    ** defined that 64-bit floating point values really are mixed
    ** endian.
    */
    static const u64 t1 = ((u64)0x3ff00000)<<32;
    static const double r1 = 1.0;
    u64 t2 = t1;
    swapMixedEndianFloat(t2);
    assert( sizeof(r1)==sizeof(t2) && memcmp(&r1, &t2, sizeof(r1))==0 );
#endif
    assert( sizeof(x)==8 && sizeof(pMem->u.r)==8 );
    swapMixedEndianFloat(x);
    memcpy(&pMem->u.r, &x, sizeof(x));
    pMem->flags = IsNaN(x) ? MEM_Null : MEM_Real;
  }
}
static int serialGet7(
  const unsigned char *buf,     /* Buffer to deserialize from */
  Mem *pMem                     /* Memory cell to write value into */
){
  u64 x = FOUR_BYTE_UINT(buf);
  u32 y = FOUR_BYTE_UINT(buf+4);
  x = (x<<32) + y;
  assert( sizeof(x)==8 && sizeof(pMem->u.r)==8 );
  swapMixedEndianFloat(x);
  memcpy(&pMem->u.r, &x, sizeof(x));
  if( IsNaN(x) ){
    pMem->flags = MEM_Null;
    return 1;
  }
  pMem->flags = MEM_Real;
  return 0;
}
void capdbVdbeSerialGet(
  const unsigned char *buf,     /* Buffer to deserialize from */
  u32 serial_type,              /* Serial type to deserialize */
  Mem *pMem                     /* Memory cell to write value into */
){
  switch( serial_type ){
    case 10: { /* Internal use only: NULL with virtual table
               ** UPDATE no-change flag set */
      pMem->flags = MEM_Null|MEM_Zero;
      pMem->n = 0;
      pMem->u.nZero = 0;
      return;
    }
    case 11:   /* Reserved for future use */
    case 0: {  /* Null */
      /* EVIDENCE-OF: R-24078-09375 Value is a NULL. */
      pMem->flags = MEM_Null;
      return;
    }
    case 1: {
      /* EVIDENCE-OF: R-44885-25196 Value is an 8-bit twos-complement
      ** integer. */
      pMem->u.i = ONE_BYTE_INT(buf);
      pMem->flags = MEM_Int;
      testcase( pMem->u.i<0 );
      return;
    }
    case 2: { /* 2-byte signed integer */
      /* EVIDENCE-OF: R-49794-35026 Value is a big-endian 16-bit
      ** twos-complement integer. */
      pMem->u.i = TWO_BYTE_INT(buf);
      pMem->flags = MEM_Int;
      testcase( pMem->u.i<0 );
      return;
    }
    case 3: { /* 3-byte signed integer */
      /* EVIDENCE-OF: R-37839-54301 Value is a big-endian 24-bit
      ** twos-complement integer. */
      pMem->u.i = THREE_BYTE_INT(buf);
      pMem->flags = MEM_Int;
      testcase( pMem->u.i<0 );
      return;
    }
    case 4: { /* 4-byte signed integer */
      /* EVIDENCE-OF: R-01849-26079 Value is a big-endian 32-bit
      ** twos-complement integer. */
      pMem->u.i = FOUR_BYTE_INT(buf);
#ifdef __HP_cc
      /* Work around a sign-extension bug in the HP compiler for HP/UX */
      if( buf[0]&0x80 ) pMem->u.i |= 0xffffffff80000000LL;
#endif
      pMem->flags = MEM_Int;
      testcase( pMem->u.i<0 );
      return;
    }
    case 5: { /* 6-byte signed integer */
      /* EVIDENCE-OF: R-50385-09674 Value is a big-endian 48-bit
      ** twos-complement integer. */
      pMem->u.i = FOUR_BYTE_UINT(buf+2) + (((i64)1)<<32)*TWO_BYTE_INT(buf);
      pMem->flags = MEM_Int;
      testcase( pMem->u.i<0 );
      return;
    }
    case 6:   /* 8-byte signed integer */
    case 7: { /* IEEE floating point */
      /* These use local variables, so do them in a separate routine
      ** to avoid having to move the frame pointer in the common case */
      serialGet(buf,serial_type,pMem);
      return;
    }
    case 8:    /* Integer 0 */
    case 9: {  /* Integer 1 */
      /* EVIDENCE-OF: R-12976-22893 Value is the integer 0. */
      /* EVIDENCE-OF: R-18143-12121 Value is the integer 1. */
      pMem->u.i = serial_type-8;
      pMem->flags = MEM_Int;
      return;
    }
    default: {
      /* EVIDENCE-OF: R-14606-31564 Value is a BLOB that is (N-12)/2 bytes in
      ** length.
      ** EVIDENCE-OF: R-28401-00140 Value is a string in the text encoding and
      ** (N-13)/2 bytes in length. */
      static const u16 aFlag[] = { MEM_Blob|MEM_Ephem, MEM_Str|MEM_Ephem };
      pMem->z = (char *)buf;
      pMem->n = (serial_type-12)/2;
      pMem->flags = aFlag[serial_type&1];
      return;
    }
  }
  return;
}
/*
** Allocate sufficient space for an UnpackedRecord structure large enough
** to hold a decoded index record for pKeyInfo.
**
** The space is allocated using capdbDbMallocRaw().  If an OOM error
** occurs, NULL is returned.
*/
UnpackedRecord *capdbVdbeAllocUnpackedRecord(
  KeyInfo *pKeyInfo               /* Description of the record */
){
  UnpackedRecord *p;              /* Unpacked record to return */
  u64 nByte;                      /* Number of bytes required for *p */
  assert( sizeof(UnpackedRecord) + sizeof(Mem)*65536 < 0x7fffffff );
  nByte = ROUND8P(sizeof(UnpackedRecord)) + sizeof(Mem)*(pKeyInfo->nKeyField+1);
  p = (UnpackedRecord *)capdbDbMallocRaw(pKeyInfo->db, nByte);
  if( !p ) return 0;
  p->aMem = (Mem*)&((char*)p)[ROUND8P(sizeof(UnpackedRecord))];
  p->pKeyInfo = pKeyInfo;
  p->nField = pKeyInfo->nKeyField + 1;
  return p;
}

/*
** Given the nKey-byte encoding of a record in pKey[], populate the
** UnpackedRecord structure indicated by the fourth argument with the
** contents of the decoded record.
*/
void capdbVdbeRecordUnpack(
  int nKey,              /* Size of the binary record */
  const void *pKey,      /* The binary record */
  UnpackedRecord *p      /* Populate this structure before returning. */
){
  const unsigned char *aKey = (const unsigned char *)pKey;
  u32 d;
  u32 idx;                        /* Offset in aKey[] to read from */
  u16 u;                          /* Unsigned loop counter */
  u32 szHdr;
  Mem *pMem = p->aMem;
  KeyInfo *pKeyInfo = p->pKeyInfo;

  p->default_rc = 0;
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  idx = getVarint32(aKey, szHdr);
  d = szHdr;
  u = 0;
  while( idx<szHdr && d<=(u32)nKey ){
    u32 serial_type;

    idx += getVarint32(&aKey[idx], serial_type);
    pMem->enc = pKeyInfo->enc;
    pMem->db = pKeyInfo->db;
    /* pMem->flags = 0; // capdbVdbeSerialGet() will set this for us */
    pMem->szMalloc = 0;
    pMem->z = 0;
    capdbVdbeSerialGet(&aKey[d], serial_type, pMem);
    d += capdbVdbeSerialTypeLen(serial_type);
    if( (++u)>=p->nField ) break;
    pMem++;
  }
  if( d>(u32)nKey && u ){
    assert( CORRUPT_DB );
    /* In a corrupt record entry, the last pMem might have been set up using
    ** uninitialized memory. Overwrite its value with NULL, to prevent
    ** warnings from MSAN. */
    capdbVdbeMemSetNull(pMem-(u<p->nField));
  }
  testcase( u == pKeyInfo->nKeyField + 1 );
  testcase( u < pKeyInfo->nKeyField + 1 );
  assert( u<=pKeyInfo->nKeyField + 1 );
  p->nField = u;
}

#ifdef CAPDB_DEBUG
/*
** This function compares two index or table record keys in the same way
** as the capdbVdbeRecordCompare() routine. Unlike VdbeRecordCompare(),
** this function deserializes and compares values using the
** capdbVdbeSerialGet() and capdbMemCompare() functions. It is used
** in assert() statements to ensure that the optimized code in
** capdbVdbeRecordCompare() returns results with these two primitives.
**
** Return true if the result of comparison is equivalent to desiredResult.
** Return false if there is a disagreement.
*/
static int vdbeRecordCompareDebug(
  int nKey1, const void *pKey1, /* Left key */
  const UnpackedRecord *pPKey2, /* Right key */
  int desiredResult             /* Correct answer */
){
  u32 d1;            /* Offset into aKey[] of next data element */
  u32 idx1;          /* Offset into aKey[] of next header element */
  u32 szHdr1;        /* Number of bytes in header */
  int i = 0;
  int rc = 0;
  const unsigned char *aKey1 = (const unsigned char *)pKey1;
  KeyInfo *pKeyInfo;
  Mem mem1;

  pKeyInfo = pPKey2->pKeyInfo;
  if( pKeyInfo->db==0 ) return 1;
  mem1.enc = pKeyInfo->enc;
  mem1.db = pKeyInfo->db;
  /* mem1.flags = 0;  // Will be initialized by capdbVdbeSerialGet() */
  VVA_ONLY( mem1.szMalloc = 0; ) /* Only needed by assert() statements */

  /* Compilers may complain that mem1.u.i is potentially uninitialized.
  ** We could initialize it, as shown here, to silence those complaints.
  ** But in fact, mem1.u.i will never actually be used uninitialized, and doing
  ** the unnecessary initialization has a measurable negative performance
  ** impact, since this routine is a very high runner.  And so, we choose
  ** to ignore the compiler warnings and leave this variable uninitialized.
  */
  /*  mem1.u.i = 0;  // not needed, here to silence compiler warning */
 
  idx1 = getVarint32(aKey1, szHdr1);
  if( szHdr1>98307 ) return CAPDB_CORRUPT;
  d1 = szHdr1;
  assert( pKeyInfo->nAllField>=pPKey2->nField || CORRUPT_DB );
  assert( pKeyInfo->aSortFlags!=0 );
  assert( pKeyInfo->nKeyField>0 );
  assert( idx1<=szHdr1 || CORRUPT_DB );
  do{
    u32 serial_type1;

    /* Read the serial types for the next element in each key. */
    idx1 += getVarint32( aKey1+idx1, serial_type1 );

    /* Verify that there is enough key space remaining to avoid
    ** a buffer overread.  The "d1+serial_type1+2" subexpression will
    ** always be greater than or equal to the amount of required key space.
    ** Use that approximation to avoid the more expensive call to
    ** capdbVdbeSerialTypeLen() in the common case.
    */
    if( d1+(u64)serial_type1+2>(u64)nKey1
     && d1+(u64)capdbVdbeSerialTypeLen(serial_type1)>(u64)nKey1
    ){
      if( serial_type1>=1
       && serial_type1<=7
       && d1+(u64)capdbVdbeSerialTypeLen(serial_type1)<=(u64)nKey1+8
       && CORRUPT_DB
      ){
        return 1;  /* corrupt record not detected by
                   ** capdbVdbeRecordCompareWithSkip().  Return true
                   ** to avoid firing the assert() */
      }
      break;
    }

    /* Extract the values to be compared.
    */
    capdbVdbeSerialGet(&aKey1[d1], serial_type1, &mem1);
    d1 += capdbVdbeSerialTypeLen(serial_type1);

    /* Do the comparison
    */
    rc = capdbMemCompare(&mem1, &pPKey2->aMem[i],
                           pKeyInfo->nAllField>i ? pKeyInfo->aColl[i] : 0);
    if( rc!=0 ){
      assert( mem1.szMalloc==0 );  /* See comment below */
      if( (pKeyInfo->aSortFlags[i] & KEYINFO_ORDER_BIGNULL)
       && ((mem1.flags & MEM_Null) || (pPKey2->aMem[i].flags & MEM_Null))
      ){
        rc = -rc;
      }
      if( pKeyInfo->aSortFlags[i] & KEYINFO_ORDER_DESC ){
        rc = -rc;  /* Invert the result for DESC sort order. */
      }
      goto debugCompareEnd;
    }
    i++;
  }while( idx1<szHdr1 && i<pPKey2->nField );

  /* No memory allocation is ever used on mem1.  Prove this using
  ** the following assert().  If the assert() fails, it indicates a
  ** memory leak and a need to call capdbVdbeMemRelease(&mem1).
  */
  assert( mem1.szMalloc==0 );

  /* rc==0 here means that one of the keys ran out of fields and
  ** all the fields up to that point were equal. Return the default_rc
  ** value.  */
  rc = pPKey2->default_rc;

debugCompareEnd:
  if( desiredResult==0 && rc==0 ) return 1;
  if( desiredResult<0 && rc<0 ) return 1;
  if( desiredResult>0 && rc>0 ) return 1;
  if( CORRUPT_DB ) return 1;
  if( pKeyInfo->db->mallocFailed ) return 1;
  return 0;
}
#endif

#ifdef CAPDB_DEBUG
/*
** Count the number of fields (a.k.a. columns) in the record given by
** pKey,nKey.  The verify that this count is less than or equal to the
** limit given by pKeyInfo->nAllField.
**
** If this constraint is not satisfied, it means that the high-speed
** vdbeRecordCompareInt() and vdbeRecordCompareString() routines will
** not work correctly.  If this assert() ever fires, it probably means
** that the KeyInfo.nKeyField or KeyInfo.nAllField values were computed
** incorrectly.
*/
static void vdbeAssertFieldCountWithinLimits(
  int nKey, const void *pKey,   /* The record to verify */
  const KeyInfo *pKeyInfo       /* Compare size with this KeyInfo */
){
  int nField = 0;
  u32 szHdr;
  u32 idx;
  u32 notUsed;
  const unsigned char *aKey = (const unsigned char*)pKey;

  if( CORRUPT_DB ) return;
  idx = getVarint32(aKey, szHdr);
  assert( nKey>=0 );
  assert( szHdr<=(u32)nKey );
  while( idx<szHdr ){
    idx += getVarint32(aKey+idx, notUsed);
    nField++;
  }
  assert( nField <= pKeyInfo->nAllField );
}
#else
# define vdbeAssertFieldCountWithinLimits(A,B,C)
#endif

/*
** Both *pMem1 and *pMem2 contain string values. Compare the two values
** using the collation sequence pColl. As usual, return a negative , zero
** or positive value if *pMem1 is less than, equal to or greater than
** *pMem2, respectively. Similar in spirit to "rc = (*pMem1) - (*pMem2);".
*/
static CAPDB_NOINLINE int vdbeCompareMemStringWithEncodingChange(
  const Mem *pMem1,
  const Mem *pMem2,
  const CollSeq *pColl,
  u8 *prcErr                      /* If an OOM occurs, set to CAPDB_NOMEM */
){
  int rc;
  const void *v1, *v2;
  Mem c1;
  Mem c2;
  capdbVdbeMemInit(&c1, pMem1->db, MEM_Null);
  capdbVdbeMemInit(&c2, pMem1->db, MEM_Null);
  capdbVdbeMemShallowCopy(&c1, pMem1, MEM_Ephem);
  capdbVdbeMemShallowCopy(&c2, pMem2, MEM_Ephem);
  v1 = capdbValueText((capdb_value*)&c1, pColl->enc);
  v2 = capdbValueText((capdb_value*)&c2, pColl->enc);
  if( (v1==0 || v2==0) ){
    if( prcErr ) *prcErr = CAPDB_NOMEM_BKPT;
    rc = 0;
  }else{
    rc = pColl->xCmp(pColl->pUser, c1.n, v1, c2.n, v2);
  }
  capdbVdbeMemReleaseMalloc(&c1);
  capdbVdbeMemReleaseMalloc(&c2);
  return rc;
}
static int vdbeCompareMemString(
  const Mem *pMem1,
  const Mem *pMem2,
  const CollSeq *pColl,
  u8 *prcErr                      /* If an OOM occurs, set to CAPDB_NOMEM */
){
  if( pMem1->enc==pColl->enc ){
    /* The strings are already in the correct encoding.  Call the
     ** comparison function directly */
    return pColl->xCmp(pColl->pUser,pMem1->n,pMem1->z,pMem2->n,pMem2->z);
  }else{
    return vdbeCompareMemStringWithEncodingChange(pMem1,pMem2,pColl,prcErr);
  }
}

/*
** The input pBlob is guaranteed to be a Blob that is not marked
** with MEM_Zero.  Return true if it could be a zero-blob.
*/
static int isAllZero(const char *z, int n){
  int i;
  for(i=0; i<n; i++){
    if( z[i] ) return 0;
  }
  return 1;
}

/*
** Compare two blobs.  Return negative, zero, or positive if the first
** is less than, equal to, or greater than the second, respectively.
** If one blob is a prefix of the other, then the shorter is the lessor.
*/
CAPDB_NOINLINE int capdbBlobCompare(const Mem *pB1, const Mem *pB2){
  int c;
  int n1 = pB1->n;
  int n2 = pB2->n;

  /* It is possible to have a Blob value that has some non-zero content
  ** followed by zero content.  But that only comes up for Blobs formed
  ** by the OP_MakeRecord opcode, and such Blobs never get passed into
  ** capdbMemCompare(). */
  assert( (pB1->flags & MEM_Zero)==0 || n1==0 );
  assert( (pB2->flags & MEM_Zero)==0 || n2==0 );

  if( (pB1->flags|pB2->flags) & MEM_Zero ){
    if( pB1->flags & pB2->flags & MEM_Zero ){
      return pB1->u.nZero - pB2->u.nZero;
    }else if( pB1->flags & MEM_Zero ){
      if( !isAllZero(pB2->z, pB2->n) ) return -1;
      return pB1->u.nZero - n2;
    }else{
      if( !isAllZero(pB1->z, pB1->n) ) return +1;
      return n1 - pB2->u.nZero;
    }
  }
  c = memcmp(pB1->z, pB2->z, n1>n2 ? n2 : n1);
  if( c ) return c;
  return n1 - n2;
}

/* The following two functions are used only within testcase() to prove
** test coverage.  These functions do no exist for production builds.
** We must use separate CAPDB_NOINLINE functions here, since otherwise
** optimizer code movement causes gcov to become very confused.
*/
#if defined(CAPDB_COVERAGE_TEST) || defined(CAPDB_DEBUG)
static int CAPDB_NOINLINE doubleLt(double a, double b){ return a<b; }
static int CAPDB_NOINLINE doubleEq(double a, double b){ return a==b; }
#endif

/*
** Do a comparison between a 64-bit signed integer and a 64-bit floating-point
** number.  Return negative, zero, or positive if the first (i64) is less than,
** equal to, or greater than the second (double).
*/
int capdbIntFloatCompare(i64 i, double r){
  if( capdbIsNaN(r) ){
    /* SQLite considers NaN to be a NULL. And all integer values are greater
    ** than NULL */
    return 1;
  }else{
    i64 y;
    if( r<-9223372036854775808.0 ) return +1;
    if( r>=9223372036854775808.0 ) return -1;
    y = (i64)r;
    if( i<y ) return -1;
    if( i>y ) return +1;
    testcase( doubleLt(((double)i),r) );
    testcase( doubleLt(r,((double)i)) );
    testcase( doubleEq(r,((double)i)) );
    return (((double)i)<r) ? -1 : (((double)i)>r);
  }
}

/*
** Compare the values contained by the two memory cells, returning
** negative, zero or positive if pMem1 is less than, equal to, or greater
** than pMem2. Sorting order is NULL's first, followed by numbers (integers
** and reals) sorted numerically, followed by text ordered by the collating
** sequence pColl and finally blob's ordered by memcmp().
**
** Two NULL values are considered equal by this function.
*/
int capdbMemCompare(const Mem *pMem1, const Mem *pMem2, const CollSeq *pColl){
  int f1, f2;
  int combined_flags;

  f1 = pMem1->flags;
  f2 = pMem2->flags;
  combined_flags = f1|f2;
  assert( !capdbVdbeMemIsRowSet(pMem1) && !capdbVdbeMemIsRowSet(pMem2) );

  /* If one value is NULL, it is less than the other. If both values
  ** are NULL, return 0.
  */
  if( combined_flags&MEM_Null ){
    return (f2&MEM_Null) - (f1&MEM_Null);
  }

  /* At least one of the two values is a number
  */
  if( combined_flags&(MEM_Int|MEM_Real|MEM_IntReal) ){
    testcase( combined_flags & MEM_Int );
    testcase( combined_flags & MEM_Real );
    testcase( combined_flags & MEM_IntReal );
    if( (f1 & f2 & (MEM_Int|MEM_IntReal))!=0 ){
      testcase( f1 & f2 & MEM_Int );
      testcase( f1 & f2 & MEM_IntReal );
      if( pMem1->u.i < pMem2->u.i ) return -1;
      if( pMem1->u.i > pMem2->u.i ) return +1;
      return 0;
    }
    if( (f1 & f2 & MEM_Real)!=0 ){
      if( pMem1->u.r < pMem2->u.r ) return -1;
      if( pMem1->u.r > pMem2->u.r ) return +1;
      return 0;
    }
    if( (f1&(MEM_Int|MEM_IntReal))!=0 ){
      testcase( f1 & MEM_Int );
      testcase( f1 & MEM_IntReal );
      if( (f2&MEM_Real)!=0 ){
        return capdbIntFloatCompare(pMem1->u.i, pMem2->u.r);
      }else if( (f2&(MEM_Int|MEM_IntReal))!=0 ){
        if( pMem1->u.i < pMem2->u.i ) return -1;
        if( pMem1->u.i > pMem2->u.i ) return +1;
        return 0;
      }else{
        return -1;
      }
    }
    if( (f1&MEM_Real)!=0 ){
      if( (f2&(MEM_Int|MEM_IntReal))!=0 ){
        testcase( f2 & MEM_Int );
        testcase( f2 & MEM_IntReal );
        return -capdbIntFloatCompare(pMem2->u.i, pMem1->u.r);
      }else{
        return -1;
      }
    }
    return +1;
  }

  /* If one value is a string and the other is a blob, the string is less.
  ** If both are strings, compare using the collating functions.
  */
  if( combined_flags&MEM_Str ){
    if( (f1 & MEM_Str)==0 ){
      return 1;
    }
    if( (f2 & MEM_Str)==0 ){
      return -1;
    }

    assert( pMem1->enc==pMem2->enc || pMem1->db->mallocFailed );
    assert( pMem1->enc==CAPDB_UTF8 ||
            pMem1->enc==CAPDB_UTF16LE || pMem1->enc==CAPDB_UTF16BE );

    /* The collation sequence must be defined at this point, even if
    ** the user deletes the collation sequence after the vdbe program is
    ** compiled (this was not always the case).
    */
    assert( !pColl || pColl->xCmp );

    if( pColl ){
      return vdbeCompareMemString(pMem1, pMem2, pColl, 0);
    }
    /* If a NULL pointer was passed as the collate function, fall through
    ** to the blob case and use memcmp().  */
  }

  /* Both values must be blobs.  Compare using memcmp().  */
  return capdbBlobCompare(pMem1, pMem2);
}


/*
** The first argument passed to this function is a serial-type that
** corresponds to an integer - all values between 1 and 9 inclusive
** except 7. The second points to a buffer containing an integer value
** serialized according to serial_type. This function deserializes
** and returns the value.
*/
static i64 vdbeRecordDecodeInt(u32 serial_type, const u8 *aKey){
  u32 y;
  assert( CORRUPT_DB || (serial_type>=1 && serial_type<=9 && serial_type!=7) );
  switch( serial_type ){
    case 0:
    case 1:
      testcase( aKey[0]&0x80 );
      return ONE_BYTE_INT(aKey);
    case 2:
      testcase( aKey[0]&0x80 );
      return TWO_BYTE_INT(aKey);
    case 3:
      testcase( aKey[0]&0x80 );
      return THREE_BYTE_INT(aKey);
    case 4: {
      testcase( aKey[0]&0x80 );
      y = FOUR_BYTE_UINT(aKey);
      return (i64)*(int*)&y;
    }
    case 5: {
      testcase( aKey[0]&0x80 );
      return FOUR_BYTE_UINT(aKey+2) + (((i64)1)<<32)*TWO_BYTE_INT(aKey);
    }
    case 6: {
      u64 x = FOUR_BYTE_UINT(aKey);
      testcase( aKey[0]&0x80 );
      x = (x<<32) | FOUR_BYTE_UINT(aKey+4);
      return (i64)*(i64*)&x;
    }
  }

  return (serial_type - 8);
}

/*
** This function compares the two table rows or index records
** specified by {nKey1, pKey1} and pPKey2.  It returns a negative, zero
** or positive integer if key1 is less than, equal to or
** greater than key2.  The {nKey1, pKey1} key must be a blob
** created by the OP_MakeRecord opcode of the VDBE.  The pPKey2
** key must be a parsed key such as obtained from
** capdbVdbeParseRecord.
**
** If argument bSkip is non-zero, it is assumed that the caller has already
** determined that the first fields of the keys are equal.
**
** Key1 and Key2 do not have to contain the same number of fields. If all
** fields that appear in both keys are equal, then pPKey2->default_rc is
** returned.
**
** If database corruption is discovered, set pPKey2->errCode to
** CAPDB_CORRUPT and return 0. If an OOM error is encountered,
** pPKey2->errCode is set to CAPDB_NOMEM and, if it is not NULL, the
** malloc-failed flag set on database handle (pPKey2->pKeyInfo->db).
*/
int capdbVdbeRecordCompareWithSkip(
  int nKey1, const void *pKey1,   /* Left key */
  UnpackedRecord *pPKey2,         /* Right key */
  int bSkip                       /* If true, skip the first field */
){
  u32 d1;                         /* Offset into aKey[] of next data element */
  int i;                          /* Index of next field to compare */
  u32 szHdr1;                     /* Size of record header in bytes */
  u32 idx1;                       /* Offset of first type in header */
  int rc = 0;                     /* Return value */
  Mem *pRhs = pPKey2->aMem;       /* Next field of pPKey2 to compare */
  KeyInfo *pKeyInfo;
  const unsigned char *aKey1 = (const unsigned char *)pKey1;
  Mem mem1;

  /* If bSkip is true, then the caller has already determined that the first
  ** two elements in the keys are equal. Fix the various stack variables so
  ** that this routine begins comparing at the second field. */
  if( bSkip ){
    u32 s1 = aKey1[1];
    if( s1<0x80 ){
      idx1 = 2;
    }else{
      idx1 = 1 + capdbGetVarint32(&aKey1[1], &s1);
    }
    szHdr1 = aKey1[0];
    d1 = szHdr1 + capdbVdbeSerialTypeLen(s1);
    i = 1;
    pRhs++;
  }else{
    if( (szHdr1 = aKey1[0])<0x80 ){
      idx1 = 1;
    }else{
      idx1 = capdbGetVarint32(aKey1, &szHdr1);
    }
    d1 = szHdr1;
    i = 0;
  }
  if( d1>(unsigned)nKey1 ){
    pPKey2->errCode = (u8)CAPDB_CORRUPT_BKPT;
    return 0;  /* Corruption */
  }

  VVA_ONLY( mem1.szMalloc = 0; ) /* Only needed by assert() statements */
  assert( pPKey2->pKeyInfo->nAllField>=pPKey2->nField
       || CORRUPT_DB );
  assert( pPKey2->pKeyInfo->aSortFlags!=0 );
  assert( pPKey2->pKeyInfo->nKeyField>0 );
  assert( idx1<=szHdr1 || CORRUPT_DB );
  while( 1 /*exit-by-break*/ ){
    u32 serial_type;

    /* RHS is an integer */
    if( pRhs->flags & (MEM_Int|MEM_IntReal) ){
      testcase( pRhs->flags & MEM_Int );
      testcase( pRhs->flags & MEM_IntReal );
      serial_type = aKey1[idx1];
      testcase( serial_type==12 );
      if( serial_type>=10 ){
        rc = serial_type==10 ? -1 : +1;
      }else if( serial_type==0 ){
        rc = -1;
      }else if( serial_type==7 ){
        serialGet7(&aKey1[d1], &mem1);
        rc = -capdbIntFloatCompare(pRhs->u.i, mem1.u.r);
      }else{
        i64 lhs = vdbeRecordDecodeInt(serial_type, &aKey1[d1]);
        i64 rhs = pRhs->u.i;
        if( lhs<rhs ){
          rc = -1;
        }else if( lhs>rhs ){
          rc = +1;
        }
      }
    }

    /* RHS is real */
    else if( pRhs->flags & MEM_Real ){
      serial_type = aKey1[idx1];
      if( serial_type>=10 ){
        /* Serial types 12 or greater are strings and blobs (greater than
        ** numbers). Types 10 and 11 are currently "reserved for future
        ** use", so it doesn't really matter what the results of comparing
        ** them to numeric values are.  */
        rc = serial_type==10 ? -1 : +1;
      }else if( serial_type==0 ){
        rc = -1;
      }else{
        if( serial_type==7 ){
          if( serialGet7(&aKey1[d1], &mem1) ){
            rc = -1;  /* mem1 is a NaN */
          }else if( mem1.u.r<pRhs->u.r ){
            rc = -1;
          }else if( mem1.u.r>pRhs->u.r ){
            rc = +1;
          }else{
            assert( rc==0 );
          }
        }else{
          capdbVdbeSerialGet(&aKey1[d1], serial_type, &mem1);
          rc = capdbIntFloatCompare(mem1.u.i, pRhs->u.r);
        }
      }
    }

    /* RHS is a string */
    else if( pRhs->flags & MEM_Str ){
      getVarint32NR(&aKey1[idx1], serial_type);
      testcase( serial_type==12 );
      if( serial_type<12 ){
        rc = -1;
      }else if( !(serial_type & 0x01) ){
        rc = +1;
      }else{
        mem1.n = (serial_type - 12) / 2;
        testcase( (d1+mem1.n)==(unsigned)nKey1 );
        testcase( (d1+mem1.n+1)==(unsigned)nKey1 );
        if( (d1+mem1.n) > (unsigned)nKey1
         || (pKeyInfo = pPKey2->pKeyInfo)->nAllField<=i
        ){
          pPKey2->errCode = (u8)CAPDB_CORRUPT_BKPT;
          return 0;                /* Corruption */
        }else if( pKeyInfo->aColl[i] ){
          mem1.enc = pKeyInfo->enc;
          mem1.db = pKeyInfo->db;
          mem1.flags = MEM_Str;
          mem1.z = (char*)&aKey1[d1];
          rc = vdbeCompareMemString(
              &mem1, pRhs, pKeyInfo->aColl[i], &pPKey2->errCode
          );
        }else{
          int nCmp = MIN(mem1.n, pRhs->n);
          rc = memcmp(&aKey1[d1], pRhs->z, nCmp);
          if( rc==0 ) rc = mem1.n - pRhs->n;
        }
      }
    }

    /* RHS is a blob */
    else if( pRhs->flags & MEM_Blob ){
      assert( (pRhs->flags & MEM_Zero)==0 || pRhs->n==0 );
      getVarint32NR(&aKey1[idx1], serial_type);
      testcase( serial_type==12 );
      if( serial_type<12 || (serial_type & 0x01) ){
        rc = -1;
      }else{
        int nStr = (serial_type - 12) / 2;
        testcase( (d1+nStr)==(unsigned)nKey1 );
        testcase( (d1+nStr+1)==(unsigned)nKey1 );
        if( (d1+nStr) > (unsigned)nKey1 ){
          pPKey2->errCode = (u8)CAPDB_CORRUPT_BKPT;
          return 0;                /* Corruption */
        }else if( pRhs->flags & MEM_Zero ){
          if( !isAllZero((const char*)&aKey1[d1],nStr) ){
            rc = 1;
          }else{
            rc = nStr - pRhs->u.nZero;
          }
        }else{
          int nCmp = MIN(nStr, pRhs->n);
          rc = memcmp(&aKey1[d1], pRhs->z, nCmp);
          if( rc==0 ) rc = nStr - pRhs->n;
        }
      }
    }

    /* RHS is null */
    else{
      serial_type = aKey1[idx1];
      if( serial_type==0
       || serial_type==10
       || (serial_type==7 && serialGet7(&aKey1[d1], &mem1)!=0)
      ){
        assert( rc==0 );
      }else{
        rc = 1;
      }
    }

    if( rc!=0 ){
      int sortFlags = pPKey2->pKeyInfo->aSortFlags[i];
      if( sortFlags ){
        if( (sortFlags & KEYINFO_ORDER_BIGNULL)==0
         || ((sortFlags & KEYINFO_ORDER_DESC)
           !=(serial_type==0 || (pRhs->flags&MEM_Null)))
        ){
          rc = -rc;
        }
      }
      assert( vdbeRecordCompareDebug(nKey1, pKey1, pPKey2, rc) );
      assert( mem1.szMalloc==0 );  /* See comment below */
      return rc;
    }

    i++;
    if( i==pPKey2->nField ) break;
    pRhs++;
    d1 += capdbVdbeSerialTypeLen(serial_type);
    if( d1>(unsigned)nKey1 ) break;
    idx1 += capdbVarintLen(serial_type);
    if( idx1>=(unsigned)szHdr1 ){
      pPKey2->errCode = (u8)CAPDB_CORRUPT_BKPT;
      return 0;  /* Corrupt index */
    }
  }

  /* No memory allocation is ever used on mem1.  Prove this using
  ** the following assert().  If the assert() fails, it indicates a
  ** memory leak and a need to call capdbVdbeMemRelease(&mem1).  */
  assert( mem1.szMalloc==0 );

  /* rc==0 here means that one or both of the keys ran out of fields and
  ** all the fields up to that point were equal. Return the default_rc
  ** value.  */
  assert( CORRUPT_DB
       || vdbeRecordCompareDebug(nKey1, pKey1, pPKey2, pPKey2->default_rc)
       || pPKey2->pKeyInfo->db->mallocFailed
  );
  pPKey2->eqSeen = 1;
  return pPKey2->default_rc;
}
int capdbVdbeRecordCompare(
  int nKey1, const void *pKey1,   /* Left key */
  UnpackedRecord *pPKey2          /* Right key */
){
  return capdbVdbeRecordCompareWithSkip(nKey1, pKey1, pPKey2, 0);
}


/*
** This function is an optimized version of capdbVdbeRecordCompare()
** that (a) the first field of pPKey2 is an integer, and (b) the
** size-of-header varint at the start of (pKey1/nKey1) fits in a single
** byte (i.e. is less than 128).
**
** To avoid concerns about buffer overreads, this routine is only used
** on schemas where the maximum valid header size is 63 bytes or less.
*/
static int vdbeRecordCompareInt(
  int nKey1, const void *pKey1, /* Left key */
  UnpackedRecord *pPKey2        /* Right key */
){
  const u8 *aKey = &((const u8*)pKey1)[*(const u8*)pKey1 & 0x3F];
  int serial_type = ((const u8*)pKey1)[1];
  int res;
  u32 y;
  u64 x;
  i64 v;
  i64 lhs;

  vdbeAssertFieldCountWithinLimits(nKey1, pKey1, pPKey2->pKeyInfo);
  assert( (*(u8*)pKey1)<=0x3F || CORRUPT_DB );
  switch( serial_type ){
    case 1: { /* 1-byte signed integer */
      lhs = ONE_BYTE_INT(aKey);
      testcase( lhs<0 );
      break;
    }
    case 2: { /* 2-byte signed integer */
      lhs = TWO_BYTE_INT(aKey);
      testcase( lhs<0 );
      break;
    }
    case 3: { /* 3-byte signed integer */
      lhs = THREE_BYTE_INT(aKey);
      testcase( lhs<0 );
      break;
    }
    case 4: { /* 4-byte signed integer */
      y = FOUR_BYTE_UINT(aKey);
      lhs = (i64)*(int*)&y;
      testcase( lhs<0 );
      break;
    }
    case 5: { /* 6-byte signed integer */
      lhs = FOUR_BYTE_UINT(aKey+2) + (((i64)1)<<32)*TWO_BYTE_INT(aKey);
      testcase( lhs<0 );
      break;
    }
    case 6: { /* 8-byte signed integer */
      x = FOUR_BYTE_UINT(aKey);
      x = (x<<32) | FOUR_BYTE_UINT(aKey+4);
      lhs = *(i64*)&x;
      testcase( lhs<0 );
      break;
    }
    case 8:
      lhs = 0;
      break;
    case 9:
      lhs = 1;
      break;

    /* This case could be removed without changing the results of running
    ** this code. Including it causes gcc to generate a faster switch
    ** statement (since the range of switch targets now starts at zero and
    ** is contiguous) but does not cause any duplicate code to be generated
    ** (as gcc is clever enough to combine the two like cases). Other
    ** compilers might be similar.  */
    case 0: case 7:
      return capdbVdbeRecordCompare(nKey1, pKey1, pPKey2);

    default:
      return capdbVdbeRecordCompare(nKey1, pKey1, pPKey2);
  }

  assert( pPKey2->u.i == pPKey2->aMem[0].u.i );
  v = pPKey2->u.i;
  if( v>lhs ){
    res = pPKey2->r1;
  }else if( v<lhs ){
    res = pPKey2->r2;
  }else if( pPKey2->nField>1 ){
    /* The first fields of the two keys are equal. Compare the trailing
    ** fields.  */
    res = capdbVdbeRecordCompareWithSkip(nKey1, pKey1, pPKey2, 1);
  }else{
    /* The first fields of the two keys are equal and there are no trailing
    ** fields. Return pPKey2->default_rc in this case. */
    res = pPKey2->default_rc;
    pPKey2->eqSeen = 1;
  }

  assert( vdbeRecordCompareDebug(nKey1, pKey1, pPKey2, res) );
  return res;
}

/*
** This function is an optimized version of capdbVdbeRecordCompare()
** that (a) the first field of pPKey2 is a string, that (b) the first field
** uses the collation sequence BINARY and (c) that the size-of-header varint
** at the start of (pKey1/nKey1) fits in a single byte.
*/
static int vdbeRecordCompareString(
  int nKey1, const void *pKey1, /* Left key */
  UnpackedRecord *pPKey2        /* Right key */
){
  const u8 *aKey1 = (const u8*)pKey1;
  int serial_type;
  int res;

  assert( pPKey2->aMem[0].flags & MEM_Str );
  assert( pPKey2->aMem[0].n == pPKey2->n );
  assert( pPKey2->aMem[0].z == pPKey2->u.z );
  vdbeAssertFieldCountWithinLimits(nKey1, pKey1, pPKey2->pKeyInfo);
  serial_type = (signed char)(aKey1[1]);

vrcs_restart:
  if( serial_type<12 ){
    if( serial_type<0 ){
      capdbGetVarint32(&aKey1[1], (u32*)&serial_type);
      if( serial_type>=12 ) goto vrcs_restart;
      assert( CORRUPT_DB );
    }
    res = pPKey2->r1;      /* (pKey1/nKey1) is a number or a null */
  }else if( !(serial_type & 0x01) ){
    res = pPKey2->r2;      /* (pKey1/nKey1) is a blob */
  }else{
    int nCmp;
    int nStr;
    int szHdr = aKey1[0];

    nStr = (serial_type-12) / 2;
    if( (szHdr + nStr) > nKey1 ){
      pPKey2->errCode = (u8)CAPDB_CORRUPT_BKPT;
      return 0;    /* Corruption */
    }
    nCmp = MIN( pPKey2->n, nStr );
    res = memcmp(&aKey1[szHdr], pPKey2->u.z, nCmp);

    if( res>0 ){
      res = pPKey2->r2;
    }else if( res<0 ){
      res = pPKey2->r1;
    }else{
      res = nStr - pPKey2->n;
      if( res==0 ){
        if( pPKey2->nField>1 ){
          res = capdbVdbeRecordCompareWithSkip(nKey1, pKey1, pPKey2, 1);
        }else{
          res = pPKey2->default_rc;
          pPKey2->eqSeen = 1;
        }
      }else if( res>0 ){
        res = pPKey2->r2;
      }else{
        res = pPKey2->r1;
      }
    }
  }

  assert( vdbeRecordCompareDebug(nKey1, pKey1, pPKey2, res)
       || CORRUPT_DB
       || pPKey2->pKeyInfo->db->mallocFailed
  );
  return res;
}

/*
** Return a pointer to an capdbVdbeRecordCompare() compatible function
** suitable for comparing serialized records to the unpacked record passed
** as the only argument.
*/
RecordCompare capdbVdbeFindCompare(UnpackedRecord *p){
  /* varintRecordCompareInt() and varintRecordCompareString() both assume
  ** that the size-of-header varint that occurs at the start of each record
  ** fits in a single byte (i.e. is 127 or less). varintRecordCompareInt()
  ** also assumes that it is safe to overread a buffer by at least the
  ** maximum possible legal header size plus 8 bytes. Because there is
  ** guaranteed to be at least 74 (but not 136) bytes of padding following each
  ** buffer passed to varintRecordCompareInt() this makes it convenient to
  ** limit the size of the header to 64 bytes in cases where the first field
  ** is an integer.
  **
  ** The easiest way to enforce this limit is to consider only records with
  ** 13 fields or less. If the first field is an integer, the maximum legal
  ** header size is (12*5 + 1 + 1) bytes.  */
  assert( p->pKeyInfo->aSortFlags!=0 );
  if( p->pKeyInfo->nAllField<=13 ){
    int flags = p->aMem[0].flags;
    if( p->pKeyInfo->aSortFlags[0] ){
      if( p->pKeyInfo->aSortFlags[0] & KEYINFO_ORDER_BIGNULL ){
        return capdbVdbeRecordCompare;
      }
      p->r1 = 1;
      p->r2 = -1;
    }else{
      p->r1 = -1;
      p->r2 = 1;
    }
    if( (flags & MEM_Int) ){
      p->u.i = p->aMem[0].u.i;
      return vdbeRecordCompareInt;
    }
    testcase( flags & MEM_Real );
    testcase( flags & MEM_Null );
    testcase( flags & MEM_Blob );
    if( (flags & (MEM_Real|MEM_IntReal|MEM_Null|MEM_Blob))==0
     && p->pKeyInfo->aColl[0]==0
    ){
      assert( flags & MEM_Str );
      p->u.z = p->aMem[0].z;
      p->n = p->aMem[0].n;
      return vdbeRecordCompareString;
    }
  }

  return capdbVdbeRecordCompare;
}

/*
** pCur points at an index entry created using the OP_MakeRecord opcode.
** Read the rowid (the last field in the record) and store it in *rowid.
** Return CAPDB_OK if everything works, or an error code otherwise.
**
** pCur might be pointing to text obtained from a corrupt database file.
** So the content cannot be trusted.  Do appropriate checks on the content.
*/
int capdbVdbeIdxRowid(capdb *db, BtCursor *pCur, i64 *rowid){
  i64 nCellKey = 0;
  int rc;
  u32 szHdr;        /* Size of the header */
  u32 typeRowid;    /* Serial type of the rowid */
  u32 lenRowid;     /* Size of the rowid */
  Mem m, v;

  /* Get the size of the index entry.  Only indices entries of less
  ** than 2GiB are support - anything large must be database corruption.
  ** Any corruption is detected in capdbBtreeParseCellPtr(), though, so
  ** this code can safely assume that nCellKey is 32-bits 
  */
  assert( capdbBtreeCursorIsValid(pCur) );
  nCellKey = capdbBtreePayloadSize(pCur);
  assert( (nCellKey & CAPDB_MAX_U32)==(u64)nCellKey );

  /* Read in the complete content of the index entry */
  capdbVdbeMemInit(&m, db, 0);
  rc = capdbVdbeMemFromBtreeZeroOffset(pCur, (u32)nCellKey, &m);
  if( rc ){
    return rc;
  }

  /* The index entry must begin with a header size */
  getVarint32NR((u8*)m.z, szHdr);
  testcase( szHdr==3 );
  testcase( szHdr==(u32)m.n );
  testcase( szHdr>0x7fffffff );
  assert( m.n>=0 );
  if( unlikely(szHdr<3 || szHdr>(unsigned)m.n) ){
    goto idx_rowid_corruption;
  }

  /* The last field of the index should be an integer - the ROWID.
  ** Verify that the last entry really is an integer. */
  getVarint32NR((u8*)&m.z[szHdr-1], typeRowid);
  testcase( typeRowid==1 );
  testcase( typeRowid==2 );
  testcase( typeRowid==3 );
  testcase( typeRowid==4 );
  testcase( typeRowid==5 );
  testcase( typeRowid==6 );
  testcase( typeRowid==8 );
  testcase( typeRowid==9 );
  if( unlikely(typeRowid<1 || typeRowid>9 || typeRowid==7) ){
    goto idx_rowid_corruption;
  }
  lenRowid = capdbSmallTypeSizes[typeRowid];
  testcase( (u32)m.n==szHdr+lenRowid );
  if( unlikely((u32)m.n<szHdr+lenRowid) ){
    goto idx_rowid_corruption;
  }

  /* Fetch the integer off the end of the index record */
  capdbVdbeSerialGet((u8*)&m.z[m.n-lenRowid], typeRowid, &v);
  *rowid = v.u.i;
  capdbVdbeMemReleaseMalloc(&m);
  return CAPDB_OK;

  /* Jump here if database corruption is detected after m has been
  ** allocated.  Free the m object and return CAPDB_CORRUPT. */
idx_rowid_corruption:
  testcase( m.szMalloc!=0 );
  capdbVdbeMemReleaseMalloc(&m);
  return CAPDB_CORRUPT_BKPT;
}

/*
** Compare the key of the index entry that cursor pC is pointing to against
** the key string in pUnpacked.  Write into *pRes a number
** that is negative, zero, or positive if pC is less than, equal to,
** or greater than pUnpacked.  Return CAPDB_OK on success.
**
** pUnpacked is either created without a rowid or is truncated so that it
** omits the rowid at the end.  The rowid at the end of the index entry
** is ignored as well.  Hence, this routine only compares the prefixes
** of the keys prior to the final rowid, not the entire key.
*/
int capdbVdbeIdxKeyCompare(
  capdb *db,                     /* Database connection */
  VdbeCursor *pC,                  /* The cursor to compare against */
  UnpackedRecord *pUnpacked,       /* Unpacked version of key */
  int *res                         /* Write the comparison result here */
){
  i64 nCellKey = 0;
  int rc;
  BtCursor *pCur;
  Mem m;

  assert( pC->eCurType==CURTYPE_BTREE );
  pCur = pC->uc.pCursor;
  assert( capdbBtreeCursorIsValid(pCur) );
  nCellKey = capdbBtreePayloadSize(pCur);
  /* nCellKey will always be between 0 and 0xffffffff because of the way
  ** that btreeParseCellPtr() and capdbGetVarint32() are implemented */
  if( nCellKey<=0 || nCellKey>0x7fffffff ){
    *res = 0;
    return CAPDB_CORRUPT_BKPT;
  }
  capdbVdbeMemInit(&m, db, 0);
  rc = capdbVdbeMemFromBtreeZeroOffset(pCur, (u32)nCellKey, &m);
  if( rc ){
    return rc;
  }
  *res = capdbVdbeRecordCompareWithSkip(m.n, m.z, pUnpacked, 0);
  capdbVdbeMemReleaseMalloc(&m);
  return CAPDB_OK;
}

/*
** This routine sets the value to be returned by subsequent calls to
** capdb_changes() on the database handle 'db'.
*/
void capdbVdbeSetChanges(capdb *db, i64 nChange){
  assert( capdb_mutex_held(db->mutex) );
  db->nChange = nChange;
  db->nTotalChange += nChange;
}

/*
** Set a flag in the vdbe to update the change counter when it is finalised
** or reset.
*/
void capdbVdbeCountChanges(Vdbe *v){
  v->changeCntOn = 1;
}

/*
** Mark every prepared statement associated with a database connection
** as expired.
**
** An expired statement means that recompilation of the statement is
** recommend.  Statements expire when things happen that make their
** programs obsolete.  Removing user-defined functions or collating
** sequences, or changing an authorization function are the types of
** things that make prepared statements obsolete.
**
** If iCode is 1, then expiration is advisory.  The statement should
** be reprepared before being restarted, but if it is already running
** it is allowed to run to completion.
**
** Internally, this function just sets the Vdbe.expired flag on all
** prepared statements.  The flag is set to 1 for an immediate expiration
** and set to 2 for an advisory expiration.
*/
void capdbExpirePreparedStatements(capdb *db, int iCode){
  Vdbe *p;
  for(p = db->pVdbe; p; p=p->pVNext){
    p->expired = iCode+1;
  }
}

/*
** Return the database associated with the Vdbe.
*/
capdb *capdbVdbeDb(Vdbe *v){
  return v->db;
}

/*
** Return the CAPDB_PREPARE flags for a Vdbe.
*/
u8 capdbVdbePrepareFlags(Vdbe *v){
  return v->prepFlags;
}

/*
** Return a pointer to an capdb_value structure containing the value bound
** parameter iVar of VM v. Except, if the value is an SQL NULL, return
** 0 instead. Unless it is NULL, apply affinity aff (one of the CAPDB_AFF_*
** constants) to the value before returning it.
**
** The returned value must be freed by the caller using capdbValueFree().
*/
capdb_value *capdbVdbeGetBoundValue(Vdbe *v, int iVar, u8 aff){
  assert( iVar>0 );
  if( v ){
    Mem *pMem = &v->aVar[iVar-1];
    assert( (v->db->flags & CAPDB_EnableQPSG)==0 
         || (v->db->mDbFlags & DBFLAG_InternalFunc)!=0 );
    if( 0==(pMem->flags & MEM_Null) ){
      capdb_value *pRet = capdbValueNew(v->db);
      if( pRet ){
        capdbVdbeMemCopy((Mem *)pRet, pMem);
        capdbValueApplyAffinity(pRet, aff, CAPDB_UTF8);
      }
      return pRet;
    }
  }
  return 0;
}

/*
** Configure SQL variable iVar so that binding a new value to it signals
** to capdb_reoptimize() that re-preparing the statement may result
** in a better query plan.
*/
void capdbVdbeSetVarmask(Vdbe *v, int iVar){
  assert( iVar>0 );
  assert( (v->db->flags & CAPDB_EnableQPSG)==0 
       || (v->db->mDbFlags & DBFLAG_InternalFunc)!=0 );
  if( iVar>=32 ){
    v->expmask |= 0x80000000;
  }else{
    v->expmask |= ((u32)1 << (iVar-1));
  }
}

/*
** Helper function for vdbeIsMatchingIndexKey(). Return true if column
** iCol should be ignored when comparing a record with a record from 
** an index on disk. The field should be ignored if:
**
**   * the corresponding bit in mask is set, and
**   * either:
**       - bIntegrity is false, or
**       - the two Mem values are both real values that differ by 
**         BTREE_ULPDISTORTION or fewer ULPs.
*/
static int vdbeSkipField(
  Bitmask mask,                   /* Mask of indexed expression fields */
  int iCol,                       /* Column of index being considered */
  Mem *pMem1,                     /* Expected index value */
  Mem *pMem2,                     /* Actual indexed value */
  int bIntegrity                  /* True if running PRAGMA integrity_check */
){
#define BTREE_ULPDISTORTION 2
  if( iCol>=BMS || (mask & MASKBIT(iCol))==0 ) return 0;
  if( bIntegrity==0 ) return 1;
  if( (pMem1->flags & MEM_Real) && (pMem2->flags & MEM_Real) ){
    u64 m1, m2;
    memcpy(&m1,&pMem1->u.r,8);
    memcpy(&m2,&pMem2->u.r,8);
    if( (m1<m2 ? m2-m1 : m1-m2) <= BTREE_ULPDISTORTION ){
      return 1;
    }
  }
  return 0;
}

/*
** This function compares the unpacked record with the current key that
** cursor pCur points to. If bInt is false, all fields for which the 
** corresponding bit in parameter "mask" is set are ignored. Or, if
** bInt is true, then a difference of BTREE_ULPDISTORTION or fewer ULPs
** in real values is overlooked for fields with the corresponding bit
** set in mask.  
**
** Return the usual less than zero, zero, or greater than zero if the 
** remaining fields of the cursor cursor key are less than, equal to or 
** greater than those in (*p).
*/
static int vdbeIsMatchingIndexKey(
  BtCursor *pCur,            /* Cursor open on index */
  int bInt,                  /* True for integrity_check-style search */
  Bitmask mask,              /* Mask of columns to skip */
  UnpackedRecord *p,         /* Index key being deleted */
  int *piRes                 /* 0 for a match, non-zero for not a match */
){
  u8 *aRec = 0;
  u32 nRec = 0;
  Mem m;
  int rc = CAPDB_OK;

  memset(&m, 0, sizeof(m));
  m.enc = p->pKeyInfo->enc;
  m.db = p->pKeyInfo->db;
  nRec = capdbBtreePayloadSize(pCur);
  if( nRec>0x7fffffff ){
    return CAPDB_CORRUPT_BKPT;
  }

  /* Allocate 5 extra bytes at the end of the buffer. This allows the
  ** getVarint32() call below to read slightly past the end of the buffer 
  ** if the record is corrupt. */
  aRec = capdbMallocZero(nRec+5);
  if( aRec==0 ){
    rc = CAPDB_NOMEM_BKPT;
  }else{
    rc = capdbBtreePayload(pCur, 0, nRec, aRec);
  }

  if( rc==CAPDB_OK ){
    u32 szHdr = 0;                /* Size of record header in bytes */
    u32 idxHdr = 0;               /* Current index in header */

    idxHdr = getVarint32(aRec, szHdr);
    if( szHdr>98307 ){
      rc = CAPDB_CORRUPT;
    }else{
      int res = 0;                /* Result of this function call */
      u32 idxRec = szHdr;         /* Index of next field in record body */
      int ii = 0;                 /* Iterator variable */

      int nCol = p->pKeyInfo->nAllField;
      for(ii=0; ii<nCol && rc==CAPDB_OK; ii++){
        u32 iSerial = 0;
        int nSerial = 0;

        if( idxHdr>=szHdr ){
          rc = CAPDB_CORRUPT_BKPT;
          break;
        }
        idxHdr += getVarint32(&aRec[idxHdr], iSerial);
        nSerial = capdbVdbeSerialTypeLen(iSerial);
        if( (idxRec+nSerial)>nRec ){
          rc = CAPDB_CORRUPT_BKPT;
        }else{
          capdbVdbeSerialGet(&aRec[idxRec], iSerial, &m);
          if( vdbeSkipField(mask, ii, &p->aMem[ii], &m, bInt)==0 ){
            res = capdbMemCompare(&m, &p->aMem[ii], p->pKeyInfo->aColl[ii]);
            if( res!=0 ) break;
          }
        }
        idxRec += capdbVdbeSerialTypeLen(iSerial);
      }

      *piRes = res;
    }
  }

  capdb_free(aRec);
  return rc;
}

/*
** This is called when the record in (*p) should be found in the index 
** opened by cursor pCur, but was not. This may happen as part of a DELETE
** operation or an integrity check.
**
** One reason that an exact match was not found may be the EIIB bug - that
** a text-to-float conversion may have caused a real value in record (*p)
** to be slightly different from its counterpart on disk. This function
** attempts to find the right index record. If it does find the right
** record, it leaves *pCur pointing to it and sets (*pRes) to 0 before
** returning. Otherwise, (*pRes) is set to non-zero and an SQLite error
** code returned.
**
** The algorithm used to find the correct record is:
**
**   * Scan up to BTREE_FDK_RANGE entries either side of the current entry.
**     If parameter bIntegrity is false, then all fields that are indexed
**     expressions or virtual table columns are omitted from the comparison.
**     If bIntegrity is true, then small differences in real values in
**     such fields are overlooked, but they are not omitted from the comparison
**     altogether.
**
**   * If the above fails to find an entry and bIntegrity is false, search 
**     the entire index.
*/
int capdbVdbeFindIndexKey(
  BtCursor *pCur, 
  Index *pIdx,
  UnpackedRecord *p, 
  int *pRes,
  int bIntegrity
){
#define BTREE_FDK_RANGE 10
  int nStep = 0;
  int res = 1;
  int rc = CAPDB_OK;
  int ii = 0;

  /* Calculate a mask based on the first 64 columns of the index. The mask
  ** bit is set if the corresponding index field is either an expression
  ** or a virtual column of the table.  */
  Bitmask mask = 0;
  for(ii=0; ii<MIN(pIdx->nColumn, BMS); ii++){
    int iCol = pIdx->aiColumn[ii];
    if( (iCol==XN_EXPR)
     || (iCol>=0 && (pIdx->pTable->aCol[iCol].colFlags & COLFLAG_VIRTUAL))
    ){
      mask |= MASKBIT(ii);
    }
  }

  /* If the mask is 0 at this point, then the index contains no expressions
  ** or virtual columns. So do not search for a match - return so that the
  ** caller may declare the db corrupt immediately. Or, if mask is non-zero,
  ** proceed.  */
  if( mask!=0 ){

    /* Move the cursor back BTREE_FDK_RANGE entries. If this hits an EOF, 
    ** position the cursor at the first entry in the index and set nStep
    ** to -1 so that the first loop below scans the entire index. Otherwise,
    ** set nStep to BTREE_FDK_RANGE*2 so that the first loop below scans
    ** just that many entries.  */
    for(ii=0; capdbBtreeEof(pCur)==0 && ii<BTREE_FDK_RANGE; ii++){
      rc = capdbBtreePrevious(pCur, 0);
    }
    if( rc==CAPDB_DONE ){
      rc = capdbBtreeFirst(pCur, &res);
      nStep = -1;
    }else{
      nStep = BTREE_FDK_RANGE*2;
    }
  
    /* This loop runs at most twice to search for a key with matching PK 
    ** fields in the index. The second iteration always searches the entire 
    ** index. The first iteration searches nStep entries starting with the
    ** current cursor entry if (nStep>=0), or the entire index if (nStep<0).  */
    while( capdbBtreeCursorIsValidNN(pCur) ){
      for(ii=0; rc==CAPDB_OK && (ii<nStep || nStep<0); ii++){
        rc = vdbeIsMatchingIndexKey(pCur, bIntegrity, mask, p, &res);
        if( res==0 || rc!=CAPDB_OK ) break;
        rc = capdbBtreeNext(pCur, 0);
      }
      if( rc==CAPDB_DONE ){
        rc = CAPDB_OK;
        assert( res!=0 );
      }
      if( nStep<0 || rc!=CAPDB_OK || res==0 || bIntegrity ) break;
  
      /* The first, non-exhaustive, search failed to find an entry with 
      ** matching PK fields. So restart for an exhaustive search of the 
      ** entire index.  */
      nStep = -1;
      rc = capdbBtreeFirst(pCur, &res);
    }
  }

  *pRes = res;
  return rc;
}

#ifndef CAPDB_OMIT_DATETIME_FUNCS
/*
** Cause a function to throw an error if it was call from OP_PureFunc
** rather than OP_Function.
**
** OP_PureFunc means that the function must be deterministic, and should
** throw an error if it is given inputs that would make it non-deterministic.
** This routine is invoked by date/time functions that use non-deterministic
** features such as 'now'.
*/
int capdbNotPureFunc(capdb_context *pCtx){
  const VdbeOp *pOp;
#ifdef CAPDB_ENABLE_STAT4
  if( pCtx->pVdbe==0 ) return 1;
#endif
  pOp = pCtx->pVdbe->aOp + pCtx->iOp;
  if( pOp->opcode==OP_PureFunc ){
    const char *zContext;
    char *zMsg;
    if( pOp->p5 & NC_IsCheck ){
      zContext = "a CHECK constraint";
    }else if( pOp->p5 & NC_GenCol ){
      zContext = "a generated column";
    }else{
      zContext = "an index";
    }
    zMsg = capdb_mprintf("non-deterministic use of %s() in %s",
                           pCtx->pFunc->zName, zContext);
    capdb_result_error(pCtx, zMsg, -1);
    capdb_free(zMsg);
    return 0;
  }
  return 1;
}
#endif /* CAPDB_OMIT_DATETIME_FUNCS */

#if defined(CAPDB_ENABLE_CURSOR_HINTS) && defined(CAPDB_DEBUG)
/*
** This Walker callback is used to help verify that calls to
** capdbBtreeCursorHint() with opcode BTREE_HINT_RANGE have
** byte-code register values correctly initialized.
*/
int capdbCursorRangeHintExprCheck(Walker *pWalker, Expr *pExpr){
  if( pExpr->op==TK_REGISTER ){
    assert( (pWalker->u.aMem[pExpr->iTable].flags & MEM_Undefined)==0 );
  }
  return WRC_Continue;
}
#endif /* CAPDB_ENABLE_CURSOR_HINTS && CAPDB_DEBUG */

#ifndef CAPDB_OMIT_VIRTUALTABLE
/*
** Transfer error message text from an capdb_vtab.zErrMsg (text stored
** in memory obtained from capdb_malloc) into a Vdbe.zErrMsg (text stored
** in memory obtained from capdbDbMalloc).
*/
void capdbVtabImportErrmsg(Vdbe *p, capdb_vtab *pVtab){
  if( pVtab->zErrMsg ){
    capdb *db = p->db;
    capdbDbFree(db, p->zErrMsg);
    p->zErrMsg = capdbDbStrDup(db, pVtab->zErrMsg);
    capdb_free(pVtab->zErrMsg);
    pVtab->zErrMsg = 0;
  }
}
#endif /* CAPDB_OMIT_VIRTUALTABLE */

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK

/*
** If the second argument is not NULL, release any allocations associated
** with the memory cells in the p->aMem[] array. Also free the UnpackedRecord
** structure itself, using capdbDbFree().
**
** This function is used to free UnpackedRecord structures allocated by
** the vdbeUnpackRecord() function found in vdbeapi.c.
*/
static void vdbeFreeUnpacked(capdb *db, int nField, UnpackedRecord *p){
  assert( db!=0 );
  if( p ){
    int i;
    for(i=0; i<nField; i++){
      Mem *pMem = &p->aMem[i];
      if( pMem->zMalloc ) capdbVdbeMemReleaseMalloc(pMem);
    }
    capdbDbNNFreeNN(db, p);
  }
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
/*
** Invoke the pre-update hook. If this is an UPDATE or DELETE pre-update call,
** then cursor passed as the second argument should point to the row about
** to be update or deleted. If the application calls capdb_preupdate_old(),
** the required value will be read from the row the cursor points to.
*/
void capdbVdbePreUpdateHook(
  Vdbe *v,                        /* Vdbe pre-update hook is invoked by */
  VdbeCursor *pCsr,               /* Cursor to grab old.* values from */
  int op,                         /* CAPDB_INSERT, UPDATE or DELETE */
  const char *zDb,                /* Database name */
  Table *pTab,                    /* Modified table */
  i64 iKey1,                      /* Initial key value */
  int iReg,                       /* Register for new.* record */
  int iBlobWrite
){
  capdb *db = v->db;
  i64 iKey2;
  PreUpdate preupdate;
  const char *zTbl = pTab->zName;
#ifdef CAPDB_DEBUG
  int nRealCol;
  if( pTab->tabFlags & TF_WithoutRowid ){
    nRealCol = capdbPrimaryKeyIndex(pTab)->nColumn;
  }else if( pTab->tabFlags & TF_HasVirtual ){
    nRealCol = pTab->nNVCol;
  }else{
    nRealCol = pTab->nCol;
  }
#endif

  assert( db->pPreUpdate==0 );
  memset(&preupdate, 0, sizeof(PreUpdate));
  if( HasRowid(pTab)==0 ){
    iKey1 = iKey2 = 0;
    preupdate.pPk = capdbPrimaryKeyIndex(pTab);
  }else{
    if( op==CAPDB_UPDATE ){
      iKey2 = v->aMem[iReg].u.i;
    }else{
      iKey2 = iKey1;
    }
  }

  assert( pCsr!=0 );
  assert( pCsr->eCurType==CURTYPE_BTREE );
  assert( pCsr->nField==nRealCol
       || (pCsr->nField==nRealCol+1 && op==CAPDB_DELETE && iReg==-1)
  );

  preupdate.v = v;
  preupdate.pCsr = pCsr;
  preupdate.op = op;
  preupdate.iNewReg = iReg;
  preupdate.pKeyinfo = (KeyInfo*)&preupdate.uKey;
  preupdate.pKeyinfo->db = db;
  preupdate.pKeyinfo->enc = ENC(db);
  preupdate.pKeyinfo->nKeyField = pTab->nCol;
  preupdate.pKeyinfo->aSortFlags = 0; /* Indicate .aColl, .nAllField uninit */
  preupdate.iKey1 = iKey1;
  preupdate.iKey2 = iKey2;
  preupdate.pTab = pTab;
  preupdate.iBlobWrite = iBlobWrite;

  db->pPreUpdate = &preupdate;
  db->xPreUpdateCallback(db->pPreUpdateArg, db, op, zDb, zTbl, iKey1, iKey2);
  db->pPreUpdate = 0;
  capdbDbFree(db, preupdate.aRecord);
  vdbeFreeUnpacked(db, preupdate.pKeyinfo->nKeyField+1,preupdate.pUnpacked);
  vdbeFreeUnpacked(db, preupdate.pKeyinfo->nKeyField+1,preupdate.pNewUnpacked);
  capdbVdbeMemRelease(&preupdate.oldipk);
  if( preupdate.aNew ){
    int i;
    for(i=0; i<pCsr->nField; i++){
      capdbVdbeMemRelease(&preupdate.aNew[i]);
    }
    capdbDbNNFreeNN(db, preupdate.aNew);
  }
  if( preupdate.apDflt ){
    int i;
    for(i=0; i<pTab->nCol; i++){
      capdbValueFree(preupdate.apDflt[i]);
    }
    capdbDbFree(db, preupdate.apDflt);
  }
}
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */

#ifdef CAPDB_ENABLE_PERCENTILE
/*
** Return the name of an SQL function associated with the capdb_context.
*/
const char *capdbVdbeFuncName(const capdb_context *pCtx){
  assert( pCtx!=0 );
  assert( pCtx->pFunc!=0 );
  return pCtx->pFunc->zName;
}
#endif /* CAPDB_ENABLE_PERCENTILE */
