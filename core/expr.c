/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains routines used for analyzing expressions and
** for generating VDBE code that evaluates expressions in SQLite.
*/
#include "capdbInt.h"

/* Forward declarations */
static void exprCodeBetween(Parse*,Expr*,int,void(*)(Parse*,Expr*,int,int),int);
static int exprCodeVector(Parse *pParse, Expr *p, int *piToFree);

/*
** Return the affinity character for a single column of a table.
*/
char capdbTableColumnAffinity(const Table *pTab, int iCol){
  if( iCol<0 || NEVER(iCol>=pTab->nCol) ) return CAPDB_AFF_INTEGER;
  return pTab->aCol[iCol].affinity;
}

/*
** Return the 'affinity' of the expression pExpr if any.
**
** If pExpr is a column, a reference to a column via an 'AS' alias,
** or a sub-select with a column as the return value, then the
** affinity of that column is returned. Otherwise, 0x00 is returned,
** indicating no affinity for the expression.
**
** i.e. the WHERE clause expressions in the following statements all
** have an affinity:
**
** CREATE TABLE t1(a);
** SELECT * FROM t1 WHERE a;
** SELECT a AS b FROM t1 WHERE b;
** SELECT * FROM t1 WHERE (select a from t1);
*/
char capdbExprAffinity(const Expr *pExpr){
  int op;
  op = pExpr->op;
  while( 1 /* exit-by-break */ ){
    if( op==TK_COLUMN || (op==TK_AGG_COLUMN && pExpr->y.pTab!=0) ){
      assert( ExprUseYTab(pExpr) );
      assert( pExpr->y.pTab!=0 );
      return capdbTableColumnAffinity(pExpr->y.pTab, pExpr->iColumn);
    }
    if( op==TK_SELECT ){
      assert( ExprUseXSelect(pExpr) );
      assert( pExpr->x.pSelect!=0 );
      assert( pExpr->x.pSelect->pEList!=0 );
      assert( pExpr->x.pSelect->pEList->a[0].pExpr!=0 );
      return capdbExprAffinity(pExpr->x.pSelect->pEList->a[0].pExpr);
    }
#ifndef CAPDB_OMIT_CAST
    if( op==TK_CAST ){
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      return capdbAffinityType(pExpr->u.zToken, 0);
    }
#endif
    if( op==TK_SELECT_COLUMN ){
      assert( pExpr->pLeft!=0 && ExprUseXSelect(pExpr->pLeft) );
      assert( pExpr->iColumn < pExpr->iTable );
      assert( pExpr->iColumn >= 0 );
      assert( pExpr->iTable==pExpr->pLeft->x.pSelect->pEList->nExpr );
      return capdbExprAffinity(
          pExpr->pLeft->x.pSelect->pEList->a[pExpr->iColumn].pExpr
      );
    }
    if( op==TK_VECTOR
     || (op==TK_FUNCTION && pExpr->affExpr==CAPDB_AFF_DEFER)
    ){
      assert( ExprUseXList(pExpr) );
      return capdbExprAffinity(pExpr->x.pList->a[0].pExpr);
    }
    if( ExprHasProperty(pExpr, EP_Skip|EP_IfNullRow) ){
      assert( pExpr->op==TK_COLLATE
           || pExpr->op==TK_IF_NULL_ROW
           || (pExpr->op==TK_REGISTER && pExpr->op2==TK_IF_NULL_ROW) );
      pExpr = pExpr->pLeft;
      op = pExpr->op;
      continue;
    }
    if( op!=TK_REGISTER ) break;
    op = pExpr->op2;
    if( NEVER( op==TK_REGISTER ) ) break;
  }
  return pExpr->affExpr;
}

/*
** Make a guess at all the possible datatypes of the result that could
** be returned by an expression.  Return a bitmask indicating the answer:
**
**     0x01         Numeric
**     0x02         Text
**     0x04         Blob
**
** If the expression must return NULL, then 0x00 is returned.
*/
int capdbExprDataType(const Expr *pExpr){
  while( pExpr ){
    switch( pExpr->op ){
      case TK_COLLATE:
      case TK_IF_NULL_ROW:
      case TK_UPLUS:  {
        pExpr = pExpr->pLeft;
        break;
      }
      case TK_NULL: {
        pExpr = 0;
        break;
      }
      case TK_STRING: {
        return 0x02;
      }
      case TK_BLOB: {
        return 0x04;
      }
      case TK_CONCAT: {
        return 0x06;
      }
      case TK_VARIABLE:
      case TK_AGG_FUNCTION:
      case TK_FUNCTION: {
        return 0x07;
      }
      case TK_COLUMN:
      case TK_AGG_COLUMN:
      case TK_SELECT:
      case TK_CAST:
      case TK_SELECT_COLUMN:
      case TK_VECTOR:  {
        int aff = capdbExprAffinity(pExpr);
        if( aff>=CAPDB_AFF_NUMERIC ) return 0x05;
        if( aff==CAPDB_AFF_TEXT )    return 0x06;
        return 0x07;
      }
      case TK_CASE: {
        int res = 0;
        int ii;
        ExprList *pList = pExpr->x.pList;
        assert( ExprUseXList(pExpr) && pList!=0 );
        assert( pList->nExpr > 0);
        for(ii=1; ii<pList->nExpr; ii+=2){
          res |= capdbExprDataType(pList->a[ii].pExpr);
        }
        if( pList->nExpr % 2 ){
          res |= capdbExprDataType(pList->a[pList->nExpr-1].pExpr);
        }
        return res;
      }
      default: {
        return 0x01;
      }
    } /* End of switch(op) */
  } /* End of while(pExpr) */
  return 0x00;
}

/*
** Set the collating sequence for expression pExpr to be the collating
** sequence named by pToken.   Return a pointer to a new Expr node that
** implements the COLLATE operator.
**
** If a memory allocation error occurs, that fact is recorded in pParse->db
** and the pExpr parameter is returned unchanged.
*/
Expr *capdbExprAddCollateToken(
  const Parse *pParse,     /* Parsing context */
  Expr *pExpr,             /* Add the "COLLATE" clause to this expression */
  const Token *pCollName,  /* Name of collating sequence */
  int dequote              /* True to dequote pCollName */
){
  if( pCollName->n>0 ){
    Expr *pNew = capdbExprAlloc(pParse->db, TK_COLLATE, pCollName, dequote);
    if( pNew ){
      pNew->pLeft = pExpr;
      pNew->flags |= EP_Collate|EP_Skip;
      pExpr = pNew;
    }
  }
  return pExpr;
}
Expr *capdbExprAddCollateString(
  const Parse *pParse,  /* Parsing context */
  Expr *pExpr,          /* Add the "COLLATE" clause to this expression */
  const char *zC        /* The collating sequence name */
){
  Token s;
  assert( zC!=0 );
  capdbTokenInit(&s, (char*)zC);
  return capdbExprAddCollateToken(pParse, pExpr, &s, 0);
}

/*
** Skip over any TK_COLLATE operators.
*/
Expr *capdbExprSkipCollate(Expr *pExpr){
  while( pExpr && ExprHasProperty(pExpr, EP_Skip) ){
    assert( pExpr->op==TK_COLLATE );
    pExpr = pExpr->pLeft;
  }  
  return pExpr;
}

/*
** Skip over any TK_COLLATE operators and/or any unlikely()
** or likelihood() or likely() functions at the root of an
** expression.
*/
Expr *capdbExprSkipCollateAndLikely(Expr *pExpr){
  while( pExpr && ExprHasProperty(pExpr, EP_Skip|EP_Unlikely) ){
    if( ExprHasProperty(pExpr, EP_Unlikely) ){
      assert( ExprUseXList(pExpr) );
      assert( pExpr->x.pList->nExpr>0 );
      assert( pExpr->op==TK_FUNCTION );
      pExpr = pExpr->x.pList->a[0].pExpr;
    }else if( pExpr->op==TK_COLLATE ){
      pExpr = pExpr->pLeft;
    }else{
      break;
    }
  }  
  return pExpr;
}

/*
** Return the collation sequence for the expression pExpr. If
** there is no defined collating sequence, return NULL.
**
** See also: capdbExprNNCollSeq()
**
** The capdbExprNNCollSeq() works the same exact that it returns the
** default collation if pExpr has no defined collation.
**
** The collating sequence might be determined by a COLLATE operator
** or by the presence of a column with a defined collating sequence.
** COLLATE operators take first precedence.  Left operands take
** precedence over right operands.
*/
CollSeq *capdbExprCollSeq(Parse *pParse, const Expr *pExpr){
  capdb *db = pParse->db;
  CollSeq *pColl = 0;
  const Expr *p = pExpr;
  while( p ){
    int op = p->op;
    if( op==TK_REGISTER ) op = p->op2;
    if( (op==TK_AGG_COLUMN && p->y.pTab!=0)
     || op==TK_COLUMN || op==TK_TRIGGER
    ){
      int j;
      assert( ExprUseYTab(p) );
      assert( p->y.pTab!=0 );
      if( (j = p->iColumn)>=0 ){
        const char *zColl = capdbColumnColl(&p->y.pTab->aCol[j]);
        pColl = capdbFindCollSeq(db, ENC(db), zColl, 0);
      }
      break;
    }
    if( op==TK_CAST || op==TK_UPLUS ){
      p = p->pLeft;
      continue;
    }
    if( op==TK_VECTOR
     || (op==TK_FUNCTION && p->affExpr==CAPDB_AFF_DEFER)
    ){
      assert( ExprUseXList(p) );
      p = p->x.pList->a[0].pExpr;
      continue;
    }
    if( op==TK_COLLATE ){
      assert( !ExprHasProperty(p, EP_IntValue) );
      pColl = capdbGetCollSeq(pParse, ENC(db), 0, p->u.zToken);
      break;
    }
    if( p->flags & EP_Collate ){
      if( p->pLeft && (p->pLeft->flags & EP_Collate)!=0 ){
        p = p->pLeft;
      }else{
        Expr *pNext  = p->pRight;
        /* The Expr.x union is never used at the same time as Expr.pRight */
        assert( !ExprUseXList(p) || p->x.pList==0 || p->pRight==0 );
        if( ExprUseXList(p) && p->x.pList!=0 && !db->mallocFailed ){
          int i;
          for(i=0; i<p->x.pList->nExpr; i++){
            if( ExprHasProperty(p->x.pList->a[i].pExpr, EP_Collate) ){
              pNext = p->x.pList->a[i].pExpr;
              break;
            }
          }
        }
        p = pNext;
      }
    }else{
      break;
    }
  }
  if( capdbCheckCollSeq(pParse, pColl) ){
    pColl = 0;
  }
  return pColl;
}

/*
** Return the collation sequence for the expression pExpr. If
** there is no defined collating sequence, return a pointer to the
** default collation sequence.
**
** See also: capdbExprCollSeq()
**
** The capdbExprCollSeq() routine works the same except that it
** returns NULL if there is no defined collation.
*/
CollSeq *capdbExprNNCollSeq(Parse *pParse, const Expr *pExpr){
  CollSeq *p = capdbExprCollSeq(pParse, pExpr);
  if( p==0 ) p = pParse->db->pDfltColl;
  assert( p!=0 );
  return p;
}

/*
** Return TRUE if the two expressions have equivalent collating sequences.
*/
int capdbExprCollSeqMatch(Parse *pParse, const Expr *pE1, const Expr *pE2){
  CollSeq *pColl1 = capdbExprNNCollSeq(pParse, pE1);
  CollSeq *pColl2 = capdbExprNNCollSeq(pParse, pE2);
  assert( (pColl1==pColl2) ==
          (capdb_stricmp(pColl1->zName,pColl2->zName)==0) );
  return pColl1==pColl2;
}

/*
** pExpr is an operand of a comparison operator.  aff2 is the
** type affinity of the other operand.  This routine returns the
** type affinity that should be used for the comparison operator.
*/
char capdbCompareAffinity(const Expr *pExpr, char aff2){
  char aff1 = capdbExprAffinity(pExpr);
  if( aff1>CAPDB_AFF_NONE && aff2>CAPDB_AFF_NONE ){
    /* Both sides of the comparison are columns. If one has numeric
    ** affinity, use that. Otherwise use no affinity.
    */
    if( capdbIsNumericAffinity(aff1) || capdbIsNumericAffinity(aff2) ){
      return CAPDB_AFF_NUMERIC;
    }else{
      return CAPDB_AFF_BLOB;
    }
  }else{
    /* One side is a column, the other is not. Use the columns affinity. */
    assert( aff1<=CAPDB_AFF_NONE || aff2<=CAPDB_AFF_NONE );
    return (aff1<=CAPDB_AFF_NONE ? aff2 : aff1) | CAPDB_AFF_NONE;
  }
}

/*
** pExpr is a comparison operator.  Return the type affinity that should
** be applied to both operands prior to doing the comparison.
*/
static char comparisonAffinity(const Expr *pExpr){
  char aff;
  assert( pExpr->op==TK_EQ || pExpr->op==TK_IN || pExpr->op==TK_LT ||
          pExpr->op==TK_GT || pExpr->op==TK_GE || pExpr->op==TK_LE ||
          pExpr->op==TK_NE || pExpr->op==TK_IS || pExpr->op==TK_ISNOT );
  assert( pExpr->pLeft );
  aff = capdbExprAffinity(pExpr->pLeft);
  if( pExpr->pRight ){
    aff = capdbCompareAffinity(pExpr->pRight, aff);
  }else if( ExprUseXSelect(pExpr) ){
    aff = capdbCompareAffinity(pExpr->x.pSelect->pEList->a[0].pExpr, aff);
  }else if( aff==0 ){
    aff = CAPDB_AFF_BLOB;
  }
  return aff;
}

/*
** pExpr is a comparison expression, eg. '=', '<', IN(...) etc.
** idx_affinity is the affinity of an indexed column. Return true
** if the index with affinity idx_affinity may be used to implement
** the comparison in pExpr.
*/
int capdbIndexAffinityOk(const Expr *pExpr, char idx_affinity){
  char aff = comparisonAffinity(pExpr);
  if( aff<CAPDB_AFF_TEXT ){
    return 1;
  }
  if( aff==CAPDB_AFF_TEXT ){
    return idx_affinity==CAPDB_AFF_TEXT;
  }
  return capdbIsNumericAffinity(idx_affinity);
}

/*
** Return the P5 value that should be used for a binary comparison
** opcode (OP_Eq, OP_Ge etc.) used to compare pExpr1 and pExpr2.
*/
static u8 binaryCompareP5(
  const Expr *pExpr1,   /* Left operand */
  const Expr *pExpr2,   /* Right operand */
  int jumpIfNull        /* Extra flags added to P5 */
){
  u8 aff = (char)capdbExprAffinity(pExpr2);
  aff = (u8)capdbCompareAffinity(pExpr1, aff) | (u8)jumpIfNull;
  return aff;
}

/*
** Return a pointer to the collation sequence that should be used by
** a binary comparison operator comparing pLeft and pRight.
**
** If the left hand expression has a collating sequence type, then it is
** used. Otherwise the collation sequence for the right hand expression
** is used, or the default (BINARY) if neither expression has a collating
** type.
**
** Argument pRight (but not pLeft) may be a null pointer. In this case,
** it is not considered.
*/
CollSeq *capdbBinaryCompareCollSeq(
  Parse *pParse,
  const Expr *pLeft,
  const Expr *pRight
){
  CollSeq *pColl;
  assert( pLeft );
  if( pLeft->flags & EP_Collate ){
    pColl = capdbExprCollSeq(pParse, pLeft);
  }else if( pRight && (pRight->flags & EP_Collate)!=0 ){
    pColl = capdbExprCollSeq(pParse, pRight);
  }else{
    pColl = capdbExprCollSeq(pParse, pLeft);
    if( !pColl ){
      pColl = capdbExprCollSeq(pParse, pRight);
    }
  }
  return pColl;
}

/* Expression p is a comparison operator.  Return a collation sequence
** appropriate for the comparison operator.
**
** This is normally just a wrapper around capdbBinaryCompareCollSeq().
** However, if the OP_Commuted flag is set, then the order of the operands
** is reversed in the capdbBinaryCompareCollSeq() call so that the
** correct collating sequence is found.
*/
CollSeq *capdbExprCompareCollSeq(Parse *pParse, const Expr *p){
  if( ExprHasProperty(p, EP_Commuted) ){
    return capdbBinaryCompareCollSeq(pParse, p->pRight, p->pLeft);
  }else{
    return capdbBinaryCompareCollSeq(pParse, p->pLeft, p->pRight);
  }
}

/*
** Generate code for a comparison operator.
*/
static int codeCompare(
  Parse *pParse,    /* The parsing (and code generating) context */
  Expr *pLeft,      /* The left operand */
  Expr *pRight,     /* The right operand */
  int opcode,       /* The comparison opcode */
  int in1, int in2, /* Register holding operands */
  int dest,         /* Jump here if true.  */
  int jumpIfNull,   /* If true, jump if either operand is NULL */
  int isCommuted    /* The comparison has been commuted */
){
  int p5;
  int addr;
  CollSeq *p4;

  if( pParse->nErr ) return 0;
  if( isCommuted ){
    p4 = capdbBinaryCompareCollSeq(pParse, pRight, pLeft);
  }else{
    p4 = capdbBinaryCompareCollSeq(pParse, pLeft, pRight);
  }
  p5 = binaryCompareP5(pLeft, pRight, jumpIfNull);
  addr = capdbVdbeAddOp4(pParse->pVdbe, opcode, in2, dest, in1,
                           (void*)p4, P4_COLLSEQ);
  capdbVdbeChangeP5(pParse->pVdbe, (u16)p5);
  return addr;
}

/*
** Return true if expression pExpr is a vector, or false otherwise.
**
** A vector is defined as any expression that results in two or more
** columns of result.  Every TK_VECTOR node is an vector because the
** parser will not generate a TK_VECTOR with fewer than two entries.
** But a TK_SELECT might be either a vector or a scalar. It is only
** considered a vector if it has two or more result columns.
*/
int capdbExprIsVector(const Expr *pExpr){
  return capdbExprVectorSize(pExpr)>1;
}

/*
** If the expression passed as the only argument is of type TK_VECTOR
** return the number of expressions in the vector. Or, if the expression
** is a sub-select, return the number of columns in the sub-select. For
** any other type of expression, return 1.
*/
int capdbExprVectorSize(const Expr *pExpr){
  u8 op = pExpr->op;
  if( op==TK_REGISTER ) op = pExpr->op2;
  if( op==TK_VECTOR ){
    assert( ExprUseXList(pExpr) );
    return pExpr->x.pList->nExpr;
  }else if( op==TK_SELECT ){
    assert( ExprUseXSelect(pExpr) );
    return pExpr->x.pSelect->pEList->nExpr;
  }else{
    return 1;
  }
}

/*
** Return a pointer to a subexpression of pVector that is the i-th
** column of the vector (numbered starting with 0).  The caller must
** ensure that i is within range.
**
** If pVector is really a scalar (and "scalar" here includes subqueries
** that return a single column!) then return pVector unmodified.
**
** pVector retains ownership of the returned subexpression.
**
** If the vector is a (SELECT ...) then the expression returned is
** just the expression for the i-th term of the result set, and may
** not be ready for evaluation because the table cursor has not yet
** been positioned.
*/
Expr *capdbVectorFieldSubexpr(Expr *pVector, int i){
  assert( i<capdbExprVectorSize(pVector) || pVector->op==TK_ERROR );
  if( capdbExprIsVector(pVector) ){
    assert( pVector->op2==0 || pVector->op==TK_REGISTER );
    if( pVector->op==TK_SELECT || pVector->op2==TK_SELECT ){
      assert( ExprUseXSelect(pVector) );
      return pVector->x.pSelect->pEList->a[i].pExpr;
    }else{
      assert( ExprUseXList(pVector) );
      return pVector->x.pList->a[i].pExpr;
    }
  }
  return pVector;
}

/*
** Compute and return a new Expr object which when passed to
** capdbExprCode() will generate all necessary code to compute
** the iField-th column of the vector expression pVector.
**
** It is ok for pVector to be a scalar (as long as iField==0). 
** In that case, this routine works like capdbExprDup().
**
** The caller owns the returned Expr object and is responsible for
** ensuring that the returned value eventually gets freed.
**
** The caller retains ownership of pVector.  If pVector is a TK_SELECT,
** then the returned object will reference pVector and so pVector must remain
** valid for the life of the returned object.  If pVector is a TK_VECTOR
** or a scalar expression, then it can be deleted as soon as this routine
** returns.
**
** A trick to cause a TK_SELECT pVector to be deleted together with
** the returned Expr object is to attach the pVector to the pRight field
** of the returned TK_SELECT_COLUMN Expr object.
*/
Expr *capdbExprForVectorField(
  Parse *pParse,       /* Parsing context */
  Expr *pVector,       /* The vector.  List of expressions or a sub-SELECT */
  int iField,          /* Which column of the vector to return */
  int nField           /* Total number of columns in the vector */
){
  Expr *pRet;
  if( pVector->op==TK_SELECT ){
    assert( ExprUseXSelect(pVector) );
    /* The TK_SELECT_COLUMN Expr node:
    **
    ** pLeft:           pVector containing TK_SELECT.  Not deleted.
    ** pRight:          not used.  But recursively deleted.
    ** iColumn:         Index of a column in pVector
    ** iTable:          0 or the number of columns on the LHS of an assignment
    ** pLeft->iTable:   First in an array of register holding result, or 0
    **                  if the result is not yet computed.
    **
    ** capdbExprDelete() specifically skips the recursive delete of
    ** pLeft on TK_SELECT_COLUMN nodes.  But pRight is followed, so pVector
    ** can be attached to pRight to cause this node to take ownership of
    ** pVector.  Typically there will be multiple TK_SELECT_COLUMN nodes
    ** with the same pLeft pointer to the pVector, but only one of them
    ** will own the pVector.
    */
    pRet = capdbPExpr(pParse, TK_SELECT_COLUMN, 0, 0);
    if( pRet ){
      ExprSetProperty(pRet, EP_FullSize);
      pRet->iTable = nField;
      pRet->iColumn = iField;
      pRet->pLeft = pVector;
    }
  }else{
    if( pVector->op==TK_VECTOR ){
      Expr **ppVector;
      assert( ExprUseXList(pVector) );
      ppVector = &pVector->x.pList->a[iField].pExpr;
      pVector = *ppVector;
      if( IN_RENAME_OBJECT ){
        /* This must be a vector UPDATE inside a trigger */
        *ppVector = 0;
        return pVector;
      }
    }
    pRet = capdbExprDup(pParse->db, pVector, 0);
  }
  return pRet;
}

/*
** If expression pExpr is of type TK_SELECT, generate code to evaluate
** it. Return the register in which the result is stored (or, if the
** sub-select returns more than one column, the first in an array
** of registers in which the result is stored).
**
** If pExpr is not a TK_SELECT expression, return 0.
*/
static int exprCodeSubselect(Parse *pParse, Expr *pExpr){
  int reg = 0;
#ifndef CAPDB_OMIT_SUBQUERY
  if( pExpr->op==TK_SELECT ){
    reg = capdbCodeSubselect(pParse, pExpr);
  }
#endif
  return reg;
}

/*
** Argument pVector points to a vector expression - either a TK_VECTOR
** or TK_SELECT that returns more than one column. This function returns
** the register number of a register that contains the value of
** element iField of the vector.
**
** If pVector is a TK_SELECT expression, then code for it must have
** already been generated using the exprCodeSubselect() routine. In this
** case parameter regSelect should be the first in an array of registers
** containing the results of the sub-select.
**
** If pVector is of type TK_VECTOR, then code for the requested field
** is generated. In this case (*pRegFree) may be set to the number of
** a temporary register to be freed by the caller before returning.
**
** Before returning, output parameter (*ppExpr) is set to point to the
** Expr object corresponding to element iElem of the vector.
*/
static int exprVectorRegister(
  Parse *pParse,                  /* Parse context */
  Expr *pVector,                  /* Vector to extract element from */
  int iField,                     /* Field to extract from pVector */
  int regSelect,                  /* First in array of registers */
  Expr **ppExpr,                  /* OUT: Expression element */
  int *pRegFree                   /* OUT: Temp register to free */
){
  u8 op = pVector->op;
  assert( op==TK_VECTOR || op==TK_REGISTER || op==TK_SELECT || op==TK_ERROR );
  if( op==TK_REGISTER ){
    *ppExpr = capdbVectorFieldSubexpr(pVector, iField);
    return pVector->iTable+iField;
  }
  if( op==TK_SELECT ){
    assert( ExprUseXSelect(pVector) );
    *ppExpr = pVector->x.pSelect->pEList->a[iField].pExpr;
     return regSelect+iField;
  }
  if( op==TK_VECTOR ){
    assert( ExprUseXList(pVector) );
    *ppExpr = pVector->x.pList->a[iField].pExpr;
    return capdbExprCodeTemp(pParse, *ppExpr, pRegFree);
  }
  return 0;
}

/*
** Expression pExpr is a comparison between two vector values. Compute
** the result of the comparison (1, 0, or NULL) and write that
** result into register dest.
**
** The caller must satisfy the following preconditions:
**
**    if pExpr->op==TK_IS:      op==TK_EQ and p5==CAPDB_NULLEQ
**    if pExpr->op==TK_ISNOT:   op==TK_NE and p5==CAPDB_NULLEQ
**    otherwise:                op==pExpr->op and p5==0
*/
static void codeVectorCompare(
  Parse *pParse,        /* Code generator context */
  Expr *pExpr,          /* The comparison operation */
  int dest,             /* Write results into this register */
  u8 op,                /* Comparison operator */
  u8 p5                 /* CAPDB_NULLEQ or zero */
){
  Vdbe *v = pParse->pVdbe;
  Expr *pLeft = pExpr->pLeft;
  Expr *pRight = pExpr->pRight;
  int nLeft = capdbExprVectorSize(pLeft);
  int i;
  int regLeft = 0;
  int regRight = 0;
  u8 opx = op;
  int addrCmp = 0;
  int addrDone = capdbVdbeMakeLabel(pParse);
  int isCommuted = ExprHasProperty(pExpr,EP_Commuted);

  assert( !ExprHasVVAProperty(pExpr,EP_Immutable) );
  if( pParse->nErr ) return;
  if( nLeft!=capdbExprVectorSize(pRight) ){
    capdbErrorMsg(pParse, "row value misused");
    return;
  }
  assert( pExpr->op==TK_EQ || pExpr->op==TK_NE
       || pExpr->op==TK_IS || pExpr->op==TK_ISNOT
       || pExpr->op==TK_LT || pExpr->op==TK_GT
       || pExpr->op==TK_LE || pExpr->op==TK_GE
  );
  assert( pExpr->op==op || (pExpr->op==TK_IS && op==TK_EQ)
            || (pExpr->op==TK_ISNOT && op==TK_NE) );
  assert( p5==0 || pExpr->op!=op );
  assert( p5==CAPDB_NULLEQ || pExpr->op==op );

  if( op==TK_LE ) opx = TK_LT;
  if( op==TK_GE ) opx = TK_GT;
  if( op==TK_NE ) opx = TK_EQ;

  regLeft = exprCodeSubselect(pParse, pLeft);
  regRight = exprCodeSubselect(pParse, pRight);

  capdbVdbeAddOp2(v, OP_Integer, 1, dest);
  for(i=0; 1 /*Loop exits by "break"*/; i++){
    int regFree1 = 0, regFree2 = 0;
    Expr *pL = 0, *pR = 0;
    int r1, r2;
    assert( i>=0 && i<nLeft );
    if( addrCmp ) capdbVdbeJumpHere(v, addrCmp);
    r1 = exprVectorRegister(pParse, pLeft, i, regLeft, &pL, &regFree1);
    r2 = exprVectorRegister(pParse, pRight, i, regRight, &pR, &regFree2);
    addrCmp = capdbVdbeCurrentAddr(v);
    codeCompare(pParse, pL, pR, opx, r1, r2, addrDone, p5, isCommuted);
    testcase(op==OP_Lt); VdbeCoverageIf(v,op==OP_Lt);
    testcase(op==OP_Le); VdbeCoverageIf(v,op==OP_Le);
    testcase(op==OP_Gt); VdbeCoverageIf(v,op==OP_Gt);
    testcase(op==OP_Ge); VdbeCoverageIf(v,op==OP_Ge);
    testcase(op==OP_Eq); VdbeCoverageIf(v,op==OP_Eq);
    testcase(op==OP_Ne); VdbeCoverageIf(v,op==OP_Ne);
    capdbReleaseTempReg(pParse, regFree1);
    capdbReleaseTempReg(pParse, regFree2);
    if( (opx==TK_LT || opx==TK_GT) && i<nLeft-1 ){
      addrCmp = capdbVdbeAddOp0(v, OP_ElseEq);
      testcase(opx==TK_LT); VdbeCoverageIf(v,opx==TK_LT);
      testcase(opx==TK_GT); VdbeCoverageIf(v,opx==TK_GT);
    }
    if( p5==CAPDB_NULLEQ ){
      capdbVdbeAddOp2(v, OP_Integer, 0, dest);
    }else{
      capdbVdbeAddOp3(v, OP_ZeroOrNull, r1, dest, r2);
    }
    if( i==nLeft-1 ){
      break;
    }
    if( opx==TK_EQ ){
      capdbVdbeAddOp2(v, OP_NotNull, dest, addrDone); VdbeCoverage(v);
    }else{
      assert( op==TK_LT || op==TK_GT || op==TK_LE || op==TK_GE );
      capdbVdbeAddOp2(v, OP_Goto, 0, addrDone);
      if( i==nLeft-2 ) opx = op;
    }
  }
  capdbVdbeJumpHere(v, addrCmp);
  capdbVdbeResolveLabel(v, addrDone);
  if( op==TK_NE ){
    capdbVdbeAddOp2(v, OP_Not, dest, dest);
  }
}

#if CAPDB_MAX_EXPR_DEPTH>0
/*
** Check that argument nHeight is less than or equal to the maximum
** expression depth allowed. If it is not, leave an error message in
** pParse.
*/
int capdbExprCheckHeight(Parse *pParse, int nHeight){
  int rc = CAPDB_OK;
  int mxHeight = pParse->db->aLimit[CAPDB_LIMIT_EXPR_DEPTH];
  if( nHeight>mxHeight ){
    capdbErrorMsg(pParse,
       "Expression tree is too large (maximum depth %d)", mxHeight
    );
    rc = CAPDB_ERROR;
  }
  return rc;
}

/* The following three functions, heightOfExpr(), heightOfExprList()
** and heightOfSelect(), are used to determine the maximum height
** of any expression tree referenced by the structure passed as the
** first argument.
**
** If this maximum height is greater than the current value pointed
** to by pnHeight, the second parameter, then set *pnHeight to that
** value.
*/
static void heightOfExpr(const Expr *p, int *pnHeight){
  if( p ){
    if( p->nHeight>*pnHeight ){
      *pnHeight = p->nHeight;
    }
  }
}
static void heightOfExprList(const ExprList *p, int *pnHeight){
  if( p ){
    int i;
    for(i=0; i<p->nExpr; i++){
      heightOfExpr(p->a[i].pExpr, pnHeight);
    }
  }
}
static void heightOfSelect(const Select *pSelect, int *pnHeight){
  const Select *p;
  for(p=pSelect; p; p=p->pPrior){
    heightOfExpr(p->pWhere, pnHeight);
    heightOfExpr(p->pHaving, pnHeight);
    heightOfExpr(p->pLimit, pnHeight);
    heightOfExprList(p->pEList, pnHeight);
    heightOfExprList(p->pGroupBy, pnHeight);
    heightOfExprList(p->pOrderBy, pnHeight);
  }
}

/*
** Set the Expr.nHeight variable in the structure passed as an
** argument. An expression with no children, Expr.pList or
** Expr.pSelect member has a height of 1. Any other expression
** has a height equal to the maximum height of any other
** referenced Expr plus one.
**
** Also propagate EP_Propagate flags up from Expr.x.pList to Expr.flags,
** if appropriate.
*/
static void exprSetHeight(Expr *p){
  int nHeight = p->pLeft ? p->pLeft->nHeight : 0;
  if( NEVER(p->pRight) && p->pRight->nHeight>nHeight ){
    nHeight = p->pRight->nHeight;
  }
  if( ExprUseXSelect(p) ){
    heightOfSelect(p->x.pSelect, &nHeight);
  }else if( p->x.pList ){
    heightOfExprList(p->x.pList, &nHeight);
    p->flags |= EP_Propagate & capdbExprListFlags(p->x.pList);
  }
  p->nHeight = nHeight + 1;
}

/*
** Set the Expr.nHeight variable using the exprSetHeight() function. If
** the height is greater than the maximum allowed expression depth,
** leave an error in pParse.
**
** Also propagate all EP_Propagate flags from the Expr.x.pList into
** Expr.flags.
*/
void capdbExprSetHeightAndFlags(Parse *pParse, Expr *p){
  if( pParse->nErr ) return;
  exprSetHeight(p);
  capdbExprCheckHeight(pParse, p->nHeight);
}

/*
** Return the maximum height of any expression tree referenced
** by the select statement passed as an argument.
*/
int capdbSelectExprHeight(const Select *p){
  int nHeight = 0;
  heightOfSelect(p, &nHeight);
  return nHeight;
}
#else /* ABOVE:  Height enforcement enabled.  BELOW: Height enforcement off */
/*
** Propagate all EP_Propagate flags from the Expr.x.pList into
** Expr.flags.
*/
void capdbExprSetHeightAndFlags(Parse *pParse, Expr *p){
  if( pParse->nErr ) return;
  if( p && ExprUseXList(p) && p->x.pList ){
    p->flags |= EP_Propagate & capdbExprListFlags(p->x.pList);
  }
}
#define exprSetHeight(y)
#endif /* CAPDB_MAX_EXPR_DEPTH>0 */

/*
** Set the error offset for an Expr node, if possible.
*/
void capdbExprSetErrorOffset(Expr *pExpr, int iOfst){
  if( pExpr==0 ) return;
  if( NEVER(ExprUseWJoin(pExpr)) ) return;
  pExpr->w.iOfst = iOfst;
}

/*
** This routine is the core allocator for Expr nodes.
**
** Construct a new expression node and return a pointer to it.  Memory
** for this node and for the pToken argument is a single allocation
** obtained from capdbDbMalloc().  The calling function
** is responsible for making sure the node eventually gets freed.
**
** If dequote is true, then the token (if it exists) is dequoted.
** If dequote is false, no dequoting is performed.  The deQuote
** parameter is ignored if pToken is NULL or if the token does not
** appear to be quoted.  If the quotes were of the form "..." (double-quotes)
** then the EP_DblQuoted flag is set on the expression node.
**
** Special case (tag-20240227-a):  If op==TK_INTEGER and pToken points to
** a string that can be translated into a 32-bit integer, then the token is
** not stored in u.zToken.  Instead, the integer values is written
** into u.iValue and the EP_IntValue flag is set. No extra storage
** is allocated to hold the integer text and the dequote flag is ignored.
** See also tag-20240227-b.
*/
Expr *capdbExprAlloc(
  capdb *db,            /* Handle for capdbDbMallocRawNN() */
  int op,                 /* Expression opcode */
  const Token *pToken,    /* Token argument.  Might be NULL */
  int dequote             /* True to dequote */
){
  Expr *pNew;
  int nExtra = pToken ? pToken->n+1 : 0;

  assert( db!=0 );
  pNew = capdbDbMallocRawNN(db, sizeof(Expr)+nExtra);
  if( pNew ){
    memset(pNew, 0, sizeof(Expr));
    pNew->op = (u8)op;
    pNew->iAgg = -1;
    if( nExtra ){
      assert( pToken!=0 );
      pNew->u.zToken = (char*)&pNew[1];
      assert( pToken->z!=0 || pToken->n==0 );
      if( pToken->n ) memcpy(pNew->u.zToken, pToken->z, pToken->n);
      pNew->u.zToken[pToken->n] = 0;
      if( dequote && capdbIsquote(pNew->u.zToken[0]) ){
        capdbDequoteExpr(pNew);
      }
    }
#if CAPDB_MAX_EXPR_DEPTH>0
    pNew->nHeight = 1;
#endif 
  }
  return pNew;
}

/*
** Allocate a new expression node from a zero-terminated token that has
** already been dequoted.
*/
Expr *capdbExpr(
  capdb *db,            /* Handle for capdbDbMallocZero() (may be null) */
  int op,                 /* Expression opcode */
  const char *zToken      /* Token argument.  Might be NULL */
){
  Token x;
  x.z = zToken;
  x.n = capdbStrlen30(zToken);
  return capdbExprAlloc(db, op, &x, 0);
}

/*
** Allocate an expression for a 32-bit signed integer literal.
*/
Expr *capdbExprInt32(capdb *db, int iVal){
  Expr *pNew = capdbDbMallocRawNN(db, sizeof(Expr));
  if( pNew ){
    memset(pNew, 0, sizeof(Expr));
    pNew->op = TK_INTEGER;
    pNew->iAgg = -1;
    pNew->flags = EP_IntValue|EP_Leaf|(iVal?EP_IsTrue:EP_IsFalse);
    pNew->u.iValue = iVal;
#if CAPDB_MAX_EXPR_DEPTH>0
    pNew->nHeight = 1;
#endif 
  }
  return pNew;
}

/*
** Attach subtrees pLeft and pRight to the Expr node pRoot.
**
** If pRoot==NULL that means that a memory allocation error has occurred.
** In that case, delete the subtrees pLeft and pRight.
*/
void capdbExprAttachSubtrees(
  capdb *db,
  Expr *pRoot,
  Expr *pLeft,
  Expr *pRight
){
  if( pRoot==0 ){
    assert( db->mallocFailed );
    capdbExprDelete(db, pLeft);
    capdbExprDelete(db, pRight);
  }else{
    assert( ExprUseXList(pRoot) );
    assert( pRoot->x.pSelect==0 );
    if( pRight ){
      pRoot->pRight = pRight;
      pRoot->flags |= EP_Propagate & pRight->flags;
#if CAPDB_MAX_EXPR_DEPTH>0
      pRoot->nHeight = pRight->nHeight+1;
    }else{
      pRoot->nHeight = 1;
#endif
    }
    if( pLeft ){
      pRoot->pLeft = pLeft;
      pRoot->flags |= EP_Propagate & pLeft->flags;
#if CAPDB_MAX_EXPR_DEPTH>0
      if( pLeft->nHeight>=pRoot->nHeight ){
        pRoot->nHeight = pLeft->nHeight+1;
      }
#endif
    }
  }
}

/*
** Allocate an Expr node which joins as many as two subtrees.
**
** One or both of the subtrees can be NULL.  Return a pointer to the new
** Expr node.  Or, if an OOM error occurs, set pParse->db->mallocFailed,
** free the subtrees and return NULL.
*/
Expr *capdbPExpr(
  Parse *pParse,          /* Parsing context */
  int op,                 /* Expression opcode */
  Expr *pLeft,            /* Left operand */
  Expr *pRight            /* Right operand */
){
  Expr *p;
  p = capdbDbMallocRawNN(pParse->db, sizeof(Expr));
  if( p ){
    memset(p, 0, sizeof(Expr));
    p->op = op & 0xff;
    p->iAgg = -1;
    capdbExprAttachSubtrees(pParse->db, p, pLeft, pRight);
    capdbExprCheckHeight(pParse, p->nHeight);
  }else{
    capdbExprDelete(pParse->db, pLeft);
    capdbExprDelete(pParse->db, pRight);
  }
  return p;
}

/*
** Add pSelect to the Expr.x.pSelect field.  Or, if pExpr is NULL (due
** do a memory allocation failure) then delete the pSelect object.
*/
void capdbPExprAddSelect(Parse *pParse, Expr *pExpr, Select *pSelect){
  if( pExpr ){
    pExpr->x.pSelect = pSelect;
    ExprSetProperty(pExpr, EP_xIsSelect|EP_Subquery);
    capdbExprSetHeightAndFlags(pParse, pExpr);
  }else{
    assert( pParse->db->mallocFailed );
    capdbSelectDelete(pParse->db, pSelect);
  }
}

/*
** Expression list pEList is a list of vector values. This function
** converts the contents of pEList to a VALUES(...) Select statement
** returning 1 row for each element of the list. For example, the
** expression list:
**
**   ( (1,2), (3,4) (5,6) )
**
** is translated to the equivalent of:
**
**   VALUES(1,2), (3,4), (5,6)
**
** Each of the vector values in pEList must contain exactly nElem terms.
** If a list element that is not a vector or does not contain nElem terms,
** an error message is left in pParse.
**
** This is used as part of processing IN(...) expressions with a list
** of vectors on the RHS. e.g. "... IN ((1,2), (3,4), (5,6))".
*/
Select *capdbExprListToValues(Parse *pParse, int nElem, ExprList *pEList){
  int ii;
  Select *pRet = 0;
  assert( nElem>1 );
  for(ii=0; ii<pEList->nExpr; ii++){
    Select *pSel;
    Expr *pExpr = pEList->a[ii].pExpr;
    int nExprElem;
    if( pExpr->op==TK_VECTOR ){
      assert( ExprUseXList(pExpr) );
      nExprElem = pExpr->x.pList->nExpr;
    }else{
      nExprElem = 1;
    }
    if( nExprElem!=nElem ){
      capdbErrorMsg(pParse, "IN(...) element has %d term%s - expected %d",
          nExprElem, nExprElem>1?"s":"", nElem
      );
      break;
    }
    assert( ExprUseXList(pExpr) );
    pSel = capdbSelectNew(pParse, pExpr->x.pList, 0, 0, 0, 0, 0, SF_Values,0);
    pExpr->x.pList = 0;
    if( pSel ){
      if( pRet ){
        pSel->op = TK_ALL;
        pSel->pPrior = pRet;
      }
      pRet = pSel;
    }
  }

  if( pRet && pRet->pPrior ){
    pRet->selFlags |= SF_MultiValue;
  }
  capdbExprListDelete(pParse->db, pEList);
  return pRet;
}

/*
** Join two expressions using an AND operator.  If either expression is
** NULL, then just return the other expression.
**
** If one side or the other of the AND is known to be false, and neither side
** is part of an ON clause, then instead of returning an AND expression,
** just return a constant expression with a value of false.
*/
Expr *capdbExprAnd(Parse *pParse, Expr *pLeft, Expr *pRight){
  capdb *db = pParse->db;
  if( pLeft==0  ){
    return pRight;
  }else if( pRight==0 ){
    return pLeft;
  }else{
    u32 f = pLeft->flags | pRight->flags;
    if( (f&(EP_OuterON|EP_InnerON|EP_IsFalse|EP_HasFunc))==EP_IsFalse
     && !IN_RENAME_OBJECT
    ){
      capdbExprDeferredDelete(pParse, pLeft);
      capdbExprDeferredDelete(pParse, pRight);
      return capdbExprInt32(db, 0);
    }else{
      return capdbPExpr(pParse, TK_AND, pLeft, pRight);
    }
  }
}

/*
** Construct a new expression node for a function with multiple
** arguments.
*/
Expr *capdbExprFunction(
  Parse *pParse,        /* Parsing context */
  ExprList *pList,      /* Argument list */
  const Token *pToken,  /* Name of the function */
  int eDistinct         /* SF_Distinct or SF_ALL or 0 */
){
  Expr *pNew;
  capdb *db = pParse->db;
  assert( pToken );
  pNew = capdbExprAlloc(db, TK_FUNCTION, pToken, 1);
  if( pNew==0 ){
    capdbExprListDelete(db, pList); /* Avoid memory leak when malloc fails */
    return 0;
  }
  assert( !ExprHasProperty(pNew, EP_InnerON|EP_OuterON) );
  pNew->w.iOfst = (int)(pToken->z - pParse->zTail);
  if( pList
   && pList->nExpr > pParse->db->aLimit[CAPDB_LIMIT_FUNCTION_ARG]
   && !pParse->nested
  ){
    capdbErrorMsg(pParse, "too many arguments on function %T", pToken);
  }
  pNew->x.pList = pList;
  ExprSetProperty(pNew, EP_HasFunc);
  assert( ExprUseXList(pNew) );
  capdbExprSetHeightAndFlags(pParse, pNew);
  if( eDistinct==SF_Distinct ) ExprSetProperty(pNew, EP_Distinct);
  return pNew;
}

/*
** Report an error when attempting to use an ORDER BY clause within
** the arguments of a non-aggregate function.
*/
void capdbExprOrderByAggregateError(Parse *pParse, Expr *p){
  capdbErrorMsg(pParse,
     "ORDER BY may not be used with non-aggregate %#T()", p
  );
}

/*
** Attach an ORDER BY clause to a function call.
**
**     functionname( arguments ORDER BY sortlist )
**     \_____________________/          \______/
**             pExpr                    pOrderBy
**
** The ORDER BY clause is inserted into a new Expr node of type TK_ORDER
** and added to the Expr.pLeft field of the parent TK_FUNCTION node.
*/
void capdbExprAddFunctionOrderBy(
  Parse *pParse,        /* Parsing context */
  Expr *pExpr,          /* The function call to which ORDER BY is to be added */
  ExprList *pOrderBy    /* The ORDER BY clause to add */
){
  Expr *pOB;
  capdb *db = pParse->db;
  if( NEVER(pOrderBy==0) ){
    assert( db->mallocFailed );
    return;
  }
  if( pExpr==0 ){
    assert( db->mallocFailed );
    capdbExprListDelete(db, pOrderBy);
    return;
  }
  assert( pExpr->op==TK_FUNCTION );
  assert( pExpr->pLeft==0 );
  assert( ExprUseXList(pExpr) );
  if( pExpr->x.pList==0 || NEVER(pExpr->x.pList->nExpr==0) ){
    /* Ignore ORDER BY on zero-argument aggregates */
    capdbParserAddCleanup(pParse, capdbExprListDeleteGeneric, pOrderBy);
    return;
  }
  if( IsWindowFunc(pExpr) ){
    capdbExprOrderByAggregateError(pParse, pExpr);
    capdbExprListDelete(db, pOrderBy);
    return;
  }
  if( pOrderBy->nExpr>db->aLimit[CAPDB_LIMIT_COLUMN] ){
    capdbErrorMsg(pParse, "too many terms in ORDER BY clause");
    capdbExprListDelete(db, pOrderBy);
    return;
  }

  pOB = capdbExprAlloc(db, TK_ORDER, 0, 0);
  if( pOB==0 ){
    capdbExprListDelete(db, pOrderBy);
    return;
  }
  pOB->x.pList = pOrderBy;
  assert( ExprUseXList(pOB) );
  pExpr->pLeft = pOB;
  ExprSetProperty(pOB, EP_FullSize);
}

/*
** Check to see if a function is usable according to current access
** rules:
**
**    CAPDB_FUNC_DIRECT    -     Only usable from top-level SQL
**
**    CAPDB_FUNC_UNSAFE    -     Usable if TRUSTED_SCHEMA or from
**                                top-level SQL
**
** If the function is not usable, create an error.
*/
void capdbExprFunctionUsable(
  Parse *pParse,         /* Parsing and code generating context */
  const Expr *pExpr,     /* The function invocation */
  const FuncDef *pDef    /* The function being invoked */
){
  assert( !IN_RENAME_OBJECT );
  assert( (pDef->funcFlags & (CAPDB_FUNC_DIRECT|CAPDB_FUNC_UNSAFE))!=0 );
  if( ExprHasProperty(pExpr, EP_FromDDL) 
   || pParse->prepFlags & CAPDB_PREPARE_FROM_DDL
  ){
    if( (pDef->funcFlags & CAPDB_FUNC_DIRECT)!=0
     || (pParse->db->flags & CAPDB_TrustedSchema)==0
    ){
      /* Functions prohibited in triggers and views if:
      **     (1) tagged with CAPDB_DIRECTONLY
      **     (2) not tagged with CAPDB_INNOCUOUS (which means it
      **         is tagged with CAPDB_FUNC_UNSAFE) and
      **         CAPDB_DBCONFIG_TRUSTED_SCHEMA is off (meaning
      **         that the schema is possibly tainted).
      */
      capdbErrorMsg(pParse, "unsafe use of %#T()", pExpr);
    }
  }
}

/*
** Assign a variable number to an expression that encodes a wildcard
** in the original SQL statement. 
**
** Wildcards consisting of a single "?" are assigned the next sequential
** variable number.
**
** Wildcards of the form "?nnn" are assigned the number "nnn".  We make
** sure "nnn" is not too big to avoid a denial of service attack when
** the SQL statement comes from an external source.
**
** Wildcards of the form ":aaa", "@aaa", or "$aaa" are assigned the same number
** as the previous instance of the same wildcard.  Or if this is the first
** instance of the wildcard, the next sequential variable number is
** assigned.
*/
void capdbExprAssignVarNumber(Parse *pParse, Expr *pExpr, u32 n){
  capdb *db = pParse->db;
  const char *z;
  ynVar x;

  if( pExpr==0 ) return;
  assert( !ExprHasProperty(pExpr, EP_IntValue|EP_Reduced|EP_TokenOnly) );
  z = pExpr->u.zToken;
  assert( z!=0 );
  assert( z[0]!=0 );
  assert( n==(u32)capdbStrlen30(z) );
  if( z[1]==0 ){
    /* Wildcard of the form "?".  Assign the next variable number */
    assert( z[0]=='?' );
    x = (ynVar)(++pParse->nVar);
  }else{
    int doAdd = 0;
    if( z[0]=='?' ){
      /* Wildcard of the form "?nnn".  Convert "nnn" to an integer and
      ** use it as the variable number */
      i64 i;
      int bOk;
      if( n==2 ){ /*OPTIMIZATION-IF-TRUE*/
        i = z[1]-'0';  /* The common case of ?N for a single digit N */
        bOk = 1;
      }else{
        bOk = 0==capdbAtoi64(&z[1], &i, n-1, CAPDB_UTF8);
      }
      testcase( i==0 );
      testcase( i==1 );
      testcase( i==db->aLimit[CAPDB_LIMIT_VARIABLE_NUMBER]-1 );
      testcase( i==db->aLimit[CAPDB_LIMIT_VARIABLE_NUMBER] );
      if( bOk==0 || i<1 || i>db->aLimit[CAPDB_LIMIT_VARIABLE_NUMBER] ){
        capdbErrorMsg(pParse, "variable number must be between ?1 and ?%d",
            db->aLimit[CAPDB_LIMIT_VARIABLE_NUMBER]);
        capdbRecordErrorOffsetOfExpr(pParse->db, pExpr);
        return;
      }
      x = (ynVar)i;
      if( x>pParse->nVar ){
        pParse->nVar = (int)x;
        doAdd = 1;
      }else if( capdbVListNumToName(pParse->pVList, x)==0 ){
        doAdd = 1;
      }
    }else{
      /* Wildcards like ":aaa", "$aaa" or "@aaa".  Reuse the same variable
      ** number as the prior appearance of the same name, or if the name
      ** has never appeared before, reuse the same variable number
      */
      x = (ynVar)capdbVListNameToNum(pParse->pVList, z, n);
      if( x==0 ){
        x = (ynVar)(++pParse->nVar);
        doAdd = 1;
      }
    }
    if( doAdd ){
      pParse->pVList = capdbVListAdd(db, pParse->pVList, z, n, x);
    }
  }
  pExpr->iColumn = x;
  if( x>db->aLimit[CAPDB_LIMIT_VARIABLE_NUMBER] ){
    capdbErrorMsg(pParse, "too many SQL variables");
    capdbRecordErrorOffsetOfExpr(pParse->db, pExpr);
  }
}

/*
** Recursively delete an expression tree.
*/
static CAPDB_NOINLINE void capdbExprDeleteNN(capdb *db, Expr *p){
  assert( p!=0 );
  assert( db!=0 );
exprDeleteRestart:
  assert( !ExprUseUValue(p) || p->u.iValue>=0 );
  assert( !ExprUseYWin(p) || !ExprUseYSub(p) );
  assert( !ExprUseYWin(p) || p->y.pWin!=0 || db->mallocFailed );
  assert( p->op!=TK_FUNCTION || !ExprUseYSub(p) );
#ifdef CAPDB_DEBUG
  if( ExprHasProperty(p, EP_Leaf) && !ExprHasProperty(p, EP_TokenOnly) ){
    assert( p->pLeft==0 );
    assert( p->pRight==0 );
    assert( !ExprUseXSelect(p) || p->x.pSelect==0 );
    assert( !ExprUseXList(p) || p->x.pList==0 );
  }
#endif
  if( !ExprHasProperty(p, (EP_TokenOnly|EP_Leaf)) ){
    /* The Expr.x union is never used at the same time as Expr.pRight */
    assert( (ExprUseXList(p) && p->x.pList==0) || p->pRight==0 );
    if( p->pRight ){
      assert( !ExprHasProperty(p, EP_WinFunc) );
      capdbExprDeleteNN(db, p->pRight);
    }else if( ExprUseXSelect(p) ){
      assert( !ExprHasProperty(p, EP_WinFunc) );
      capdbSelectDelete(db, p->x.pSelect);
    }else{
      capdbExprListDelete(db, p->x.pList);
#ifndef CAPDB_OMIT_WINDOWFUNC
      if( ExprHasProperty(p, EP_WinFunc) ){
        capdbWindowDelete(db, p->y.pWin);
      }
#endif
    }
    if( p->pLeft && p->op!=TK_SELECT_COLUMN ){
      Expr *pLeft = p->pLeft;
      if( !ExprHasProperty(p, EP_Static)
       && !ExprHasProperty(pLeft, EP_Static)
      ){
        /* Avoid unnecessary recursion on unary operators */
        capdbDbNNFreeNN(db, p);
        p = pLeft;
        goto exprDeleteRestart;
      }else{
        capdbExprDeleteNN(db, pLeft);
      }
    }
  }
  if( !ExprHasProperty(p, EP_Static) ){
    capdbDbNNFreeNN(db, p);
  }
}
void capdbExprDelete(capdb *db, Expr *p){
  if( p ) capdbExprDeleteNN(db, p);
}
void capdbExprDeleteGeneric(capdb *db, void *p){
  if( ALWAYS(p) ) capdbExprDeleteNN(db, (Expr*)p);
}

/*
** Clear both elements of an OnOrUsing object
*/
void capdbClearOnOrUsing(capdb *db, OnOrUsing *p){
  if( p==0 ){
    /* Nothing to clear */
  }else if( p->pOn ){
    capdbExprDeleteNN(db, p->pOn);
  }else if( p->pUsing ){
    capdbIdListDelete(db, p->pUsing);
  }
}

/*
** Arrange to cause pExpr to be deleted when the pParse is deleted.
** This is similar to capdbExprDelete() except that the delete is
** deferred until the pParse is deleted.
**
** The pExpr might be deleted immediately on an OOM error.
**
** Return 0 if the delete was successfully deferred.  Return non-zero
** if the delete happened immediately because of an OOM.
*/
int capdbExprDeferredDelete(Parse *pParse, Expr *pExpr){
  return 0==capdbParserAddCleanup(pParse, capdbExprDeleteGeneric, pExpr);
}

/* Invoke capdbRenameExprUnmap() and capdbExprDelete() on the
** expression.
*/
void capdbExprUnmapAndDelete(Parse *pParse, Expr *p){
  if( p ){
    if( IN_RENAME_OBJECT ){
      capdbRenameExprUnmap(pParse, p);
    }
    capdbExprDeleteNN(pParse->db, p);
  }
}

/*
** Return the number of bytes allocated for the expression structure
** passed as the first argument. This is always one of EXPR_FULLSIZE,
** EXPR_REDUCEDSIZE or EXPR_TOKENONLYSIZE.
*/
static int exprStructSize(const Expr *p){
  if( ExprHasProperty(p, EP_TokenOnly) ) return EXPR_TOKENONLYSIZE;
  if( ExprHasProperty(p, EP_Reduced) ) return EXPR_REDUCEDSIZE;
  return EXPR_FULLSIZE;
}

/*
** The dupedExpr*Size() routines each return the number of bytes required
** to store a copy of an expression or expression tree.  They differ in
** how much of the tree is measured.
**
**     dupedExprStructSize()     Size of only the Expr structure
**     dupedExprNodeSize()       Size of Expr + space for token
**     dupedExprSize()           Expr + token + subtree components
**
***************************************************************************
**
** The dupedExprStructSize() function returns two values OR-ed together: 
** (1) the space required for a copy of the Expr structure only and
** (2) the EP_xxx flags that indicate what the structure size should be.
** The return values is always one of:
**
**      EXPR_FULLSIZE
**      EXPR_REDUCEDSIZE   | EP_Reduced
**      EXPR_TOKENONLYSIZE | EP_TokenOnly
**
** The size of the structure can be found by masking the return value
** of this routine with 0xfff.  The flags can be found by masking the
** return value with EP_Reduced|EP_TokenOnly.
**
** Note that with flags==EXPRDUP_REDUCE, this routines works on full-size
** (unreduced) Expr objects as they or originally constructed by the parser.
** During expression analysis, extra information is computed and moved into
** later parts of the Expr object and that extra information might get chopped
** off if the expression is reduced.  Note also that it does not work to
** make an EXPRDUP_REDUCE copy of a reduced expression.  It is only legal
** to reduce a pristine expression tree from the parser.  The implementation
** of dupedExprStructSize() contain multiple assert() statements that attempt
** to enforce this constraint.
*/
static int dupedExprStructSize(const Expr *p, int flags){
  int nSize;
  assert( flags==EXPRDUP_REDUCE || flags==0 ); /* Only one flag value allowed */
  assert( EXPR_FULLSIZE<=0xfff );
  assert( (0xfff & (EP_Reduced|EP_TokenOnly))==0 );
  if( 0==flags || ExprHasProperty(p, EP_FullSize) ){
    nSize = EXPR_FULLSIZE;
  }else{
    assert( !ExprHasProperty(p, EP_TokenOnly|EP_Reduced) );
    assert( !ExprHasProperty(p, EP_OuterON) );
    assert( !ExprHasVVAProperty(p, EP_NoReduce) );
    if( p->pLeft || p->x.pList ){
      nSize = EXPR_REDUCEDSIZE | EP_Reduced;
    }else{
      assert( p->pRight==0 );
      nSize = EXPR_TOKENONLYSIZE | EP_TokenOnly;
    }
  }
  return nSize;
}

/*
** This function returns the space in bytes required to store the copy
** of the Expr structure and a copy of the Expr.u.zToken string (if that
** string is defined.)
*/
static int dupedExprNodeSize(const Expr *p, int flags){
  int nByte = dupedExprStructSize(p, flags) & 0xfff;
  if( !ExprHasProperty(p, EP_IntValue) && p->u.zToken ){
    nByte += capdbStrlen30NN(p->u.zToken)+1;
  }
  return ROUND8(nByte);
}

/*
** Return the number of bytes required to create a duplicate of the
** expression passed as the first argument.
**
** The value returned includes space to create a copy of the Expr struct
** itself and the buffer referred to by Expr.u.zToken, if any.
**
** The return value includes space to duplicate all Expr nodes in the
** tree formed by Expr.pLeft and Expr.pRight, but not any other
** substructure such as Expr.x.pList, Expr.x.pSelect, and Expr.y.pWin.
*/
static int dupedExprSize(const Expr *p){
  int nByte;
  assert( p!=0 );
  nByte = dupedExprNodeSize(p, EXPRDUP_REDUCE);
  if( p->pLeft ) nByte += dupedExprSize(p->pLeft);
  if( p->pRight ) nByte += dupedExprSize(p->pRight);
  assert( nByte==ROUND8(nByte) );
  return nByte;
}

/*
** An EdupBuf is a memory allocation used to stored multiple Expr objects
** together with their Expr.zToken content.  This is used to help implement
** compression while doing capdbExprDup().  The top-level Expr does the
** allocation for itself and many of its decendents, then passes an instance
** of the structure down into exprDup() so that they decendents can have
** access to that memory.
*/
typedef struct EdupBuf EdupBuf;
struct EdupBuf {
  u8 *zAlloc;          /* Memory space available for storage */
#ifdef CAPDB_DEBUG
  u8 *zEnd;            /* First byte past the end of memory */
#endif
};

/*
** This function is similar to capdbExprDup(), except that if pEdupBuf
** is not NULL then it points to memory that can be used to store a copy
** of the input Expr p together with its p->u.zToken (if any).  pEdupBuf
** is updated with the new buffer tail prior to returning.
*/
static Expr *exprDup(
  capdb *db,          /* Database connection (for memory allocation) */
  const Expr *p,        /* Expr tree to be duplicated */
  int dupFlags,         /* EXPRDUP_REDUCE for compression.  0 if not */
  EdupBuf *pEdupBuf     /* Preallocated storage space, or NULL */
){
  Expr *pNew;           /* Value to return */
  EdupBuf sEdupBuf;     /* Memory space from which to build Expr object */
  u32 staticFlag;       /* EP_Static if space not obtained from malloc */
  int nToken = -1;       /* Space needed for p->u.zToken.  -1 means unknown */

  assert( db!=0 );
  assert( p );
  assert( dupFlags==0 || dupFlags==EXPRDUP_REDUCE );
  assert( pEdupBuf==0 || dupFlags==EXPRDUP_REDUCE );

  /* Figure out where to write the new Expr structure. */
  if( pEdupBuf ){
    sEdupBuf.zAlloc = pEdupBuf->zAlloc;
#ifdef CAPDB_DEBUG
    sEdupBuf.zEnd = pEdupBuf->zEnd;
#endif
    staticFlag = EP_Static;
    assert( sEdupBuf.zAlloc!=0 );
    assert( dupFlags==EXPRDUP_REDUCE );
  }else{
    int nAlloc;
    if( dupFlags ){
      nAlloc = dupedExprSize(p);
    }else if( !ExprHasProperty(p, EP_IntValue) && p->u.zToken ){
      nToken = capdbStrlen30NN(p->u.zToken)+1;
      nAlloc = ROUND8(EXPR_FULLSIZE + nToken);
    }else{
      nToken = 0;
      nAlloc = ROUND8(EXPR_FULLSIZE);
    }
    assert( nAlloc==ROUND8(nAlloc) );
    sEdupBuf.zAlloc = capdbDbMallocRawNN(db, nAlloc);
#ifdef CAPDB_DEBUG
    sEdupBuf.zEnd = sEdupBuf.zAlloc ? sEdupBuf.zAlloc+nAlloc : 0;
#endif
    
    staticFlag = 0;
  }
  pNew = (Expr *)sEdupBuf.zAlloc;
  assert( EIGHT_BYTE_ALIGNMENT(pNew) );

  if( pNew ){
    /* Set nNewSize to the size allocated for the structure pointed to
    ** by pNew. This is either EXPR_FULLSIZE, EXPR_REDUCEDSIZE or
    ** EXPR_TOKENONLYSIZE. nToken is set to the number of bytes consumed
    ** by the copy of the p->u.zToken string (if any).
    */
    const unsigned nStructSize = dupedExprStructSize(p, dupFlags);
    int nNewSize = nStructSize & 0xfff;
    if( nToken<0 ){
      if( !ExprHasProperty(p, EP_IntValue) && p->u.zToken ){
        nToken = capdbStrlen30(p->u.zToken) + 1;
      }else{
        nToken = 0;
      }
    }
    if( dupFlags ){
      assert( (int)(sEdupBuf.zEnd - sEdupBuf.zAlloc) >= nNewSize+nToken );
      assert( ExprHasProperty(p, EP_Reduced)==0 );
      memcpy(sEdupBuf.zAlloc, p, nNewSize);
    }else{
      u32 nSize = (u32)exprStructSize(p);
      assert( (int)(sEdupBuf.zEnd - sEdupBuf.zAlloc) >=
                                                   (int)EXPR_FULLSIZE+nToken );
      memcpy(sEdupBuf.zAlloc, p, nSize);
      if( nSize<EXPR_FULLSIZE ){
        memset(&sEdupBuf.zAlloc[nSize], 0, EXPR_FULLSIZE-nSize);
      }
      nNewSize = EXPR_FULLSIZE;
    }

    /* Set the EP_Reduced, EP_TokenOnly, and EP_Static flags appropriately. */
    pNew->flags &= ~(EP_Reduced|EP_TokenOnly|EP_Static);
    pNew->flags |= nStructSize & (EP_Reduced|EP_TokenOnly);
    pNew->flags |= staticFlag;
    ExprClearVVAProperties(pNew);
    if( dupFlags ){
      ExprSetVVAProperty(pNew, EP_Immutable);
    }

    /* Copy the p->u.zToken string, if any. */
    assert( nToken>=0 );
    if( nToken>0 ){
      char *zToken = pNew->u.zToken = (char*)&sEdupBuf.zAlloc[nNewSize];
      memcpy(zToken, p->u.zToken, nToken);
      nNewSize += nToken;
    }
    sEdupBuf.zAlloc += ROUND8(nNewSize);

    if( ((p->flags|pNew->flags)&(EP_TokenOnly|EP_Leaf))==0 ){

      /* Fill in the pNew->x.pSelect or pNew->x.pList member. */
      if( ExprUseXSelect(p) ){
        pNew->x.pSelect = capdbSelectDup(db, p->x.pSelect, dupFlags);
      }else{
        pNew->x.pList = capdbExprListDup(db, p->x.pList,
                           p->op!=TK_ORDER ? dupFlags : 0);
      }

#ifndef CAPDB_OMIT_WINDOWFUNC
      if( ExprHasProperty(p, EP_WinFunc) ){
        pNew->y.pWin = capdbWindowDup(db, pNew, p->y.pWin);
        assert( ExprHasProperty(pNew, EP_WinFunc) );
      }
#endif /* CAPDB_OMIT_WINDOWFUNC */

      /* Fill in pNew->pLeft and pNew->pRight. */
      if( dupFlags ){
        if( p->op==TK_SELECT_COLUMN ){
          pNew->pLeft = p->pLeft;
          assert( p->pRight==0 
               || p->pRight==p->pLeft
               || ExprHasProperty(p->pLeft, EP_Subquery) );
        }else{
          pNew->pLeft = p->pLeft ?
                      exprDup(db, p->pLeft, EXPRDUP_REDUCE, &sEdupBuf) : 0;
        }
        pNew->pRight = p->pRight ?
                       exprDup(db, p->pRight, EXPRDUP_REDUCE, &sEdupBuf) : 0;
      }else{
        if( p->op==TK_SELECT_COLUMN ){
          pNew->pLeft = p->pLeft;
          assert( p->pRight==0 
               || p->pRight==p->pLeft
               || ExprHasProperty(p->pLeft, EP_Subquery) );
        }else{
          pNew->pLeft = capdbExprDup(db, p->pLeft, 0);
        }
        pNew->pRight = capdbExprDup(db, p->pRight, 0);
      }
    }
  }
  if( pEdupBuf ) memcpy(pEdupBuf, &sEdupBuf, sizeof(sEdupBuf));
  assert( sEdupBuf.zAlloc <= sEdupBuf.zEnd );
  return pNew;
}

/*
** Create and return a deep copy of the object passed as the second
** argument. If an OOM condition is encountered, NULL is returned
** and the db->mallocFailed flag set.
*/
#ifndef CAPDB_OMIT_CTE
With *capdbWithDup(capdb *db, With *p){
  With *pRet = 0;
  if( p ){
    capdb_int64 nByte = SZ_WITH(p->nCte);
    pRet = capdbDbMallocZero(db, nByte);
    if( pRet ){
      int i;
      pRet->nCte = p->nCte;
      for(i=0; i<p->nCte; i++){
        pRet->a[i].pSelect = capdbSelectDup(db, p->a[i].pSelect, 0);
        pRet->a[i].pCols = capdbExprListDup(db, p->a[i].pCols, 0);
        pRet->a[i].zName = capdbDbStrDup(db, p->a[i].zName);
        pRet->a[i].eM10d = p->a[i].eM10d;
      }
    }
  }
  return pRet;
}
#else
# define capdbWithDup(x,y) 0
#endif

#ifndef CAPDB_OMIT_WINDOWFUNC
/*
** The gatherSelectWindows() procedure and its helper routine
** gatherSelectWindowsCallback() are used to scan all the expressions
** an a newly duplicated SELECT statement and gather all of the Window
** objects found there, assembling them onto the linked list at Select->pWin.
*/
static int gatherSelectWindowsCallback(Walker *pWalker, Expr *pExpr){
  if( pExpr->op==TK_FUNCTION && ExprHasProperty(pExpr, EP_WinFunc) ){
    Select *pSelect = pWalker->u.pSelect;
    Window *pWin = pExpr->y.pWin;
    assert( pWin );
    assert( IsWindowFunc(pExpr) );
    assert( pWin->ppThis==0 );
    capdbWindowLink(pSelect, pWin);
  }
  return WRC_Continue;
}
static int gatherSelectWindowsSelectCallback(Walker *pWalker, Select *p){
  return p==pWalker->u.pSelect ? WRC_Continue : WRC_Prune;
}
static void gatherSelectWindows(Select *p){
  Walker w;
  w.xExprCallback = gatherSelectWindowsCallback;
  w.xSelectCallback = gatherSelectWindowsSelectCallback;
  w.xSelectCallback2 = 0;
  w.pParse = 0;
  w.u.pSelect = p;
  capdbWalkSelect(&w, p);
}
#endif


/*
** The following group of routines make deep copies of expressions,
** expression lists, ID lists, and select statements.  The copies can
** be deleted (by being passed to their respective ...Delete() routines)
** without effecting the originals.
**
** The expression list, ID, and source lists return by capdbExprListDup(),
** capdbIdListDup(), and capdbSrcListDup() can not be further expanded
** by subsequent calls to sqlite*ListAppend() routines.
**
** Any tables that the SrcList might point to are not duplicated.
**
** The flags parameter contains a combination of the EXPRDUP_XXX flags.
** If the EXPRDUP_REDUCE flag is set, then the structure returned is a
** truncated version of the usual Expr structure that will be stored as
** part of the in-memory representation of the database schema.
*/
Expr *capdbExprDup(capdb *db, const Expr *p, int flags){
  assert( flags==0 || flags==EXPRDUP_REDUCE );
  return p ? exprDup(db, p, flags, 0) : 0;
}
ExprList *capdbExprListDup(capdb *db, const ExprList *p, int flags){
  ExprList *pNew;
  struct ExprList_item *pItem;
  const struct ExprList_item *pOldItem;
  int i;
  Expr *pPriorSelectColOld = 0;
  Expr *pPriorSelectColNew = 0;
  assert( db!=0 );
  if( p==0 ) return 0;
  pNew = capdbDbMallocRawNN(db, capdbDbMallocSize(db, p));
  if( pNew==0 ) return 0;
  pNew->nExpr = p->nExpr;
  pNew->nAlloc = p->nAlloc;
  pItem = pNew->a;
  pOldItem = p->a;
  for(i=0; i<p->nExpr; i++, pItem++, pOldItem++){
    Expr *pOldExpr = pOldItem->pExpr;
    Expr *pNewExpr;
    pItem->pExpr = capdbExprDup(db, pOldExpr, flags);
    if( pOldExpr
     && pOldExpr->op==TK_SELECT_COLUMN
     && (pNewExpr = pItem->pExpr)!=0
    ){
      if( pNewExpr->pRight ){
        pPriorSelectColOld = pOldExpr->pRight;
        pPriorSelectColNew = pNewExpr->pRight;
        pNewExpr->pLeft = pNewExpr->pRight;
      }else{
        if( pOldExpr->pLeft!=pPriorSelectColOld ){
          pPriorSelectColOld = pOldExpr->pLeft;
          pPriorSelectColNew = capdbExprDup(db, pPriorSelectColOld, flags);
          pNewExpr->pRight = pPriorSelectColNew;
        }
        pNewExpr->pLeft = pPriorSelectColNew;
      }
    }
    pItem->zEName = capdbDbStrDup(db, pOldItem->zEName);
    pItem->fg = pOldItem->fg;
    pItem->u = pOldItem->u;
  }
  return pNew;
}

/*
** If cursors, triggers, views and subqueries are all omitted from
** the build, then none of the following routines, except for
** capdbSelectDup(), can be called. capdbSelectDup() is sometimes
** called with a NULL argument.
*/
#if !defined(CAPDB_OMIT_VIEW) || !defined(CAPDB_OMIT_TRIGGER) \
 || !defined(CAPDB_OMIT_SUBQUERY)
SrcList *capdbSrcListDup(capdb *db, const SrcList *p, int flags){
  SrcList *pNew;
  int i;
  assert( db!=0 );
  if( p==0 ) return 0;
  pNew = capdbDbMallocRawNN(db, SZ_SRCLIST(p->nSrc) );
  if( pNew==0 ) return 0;
  pNew->nSrc = pNew->nAlloc = p->nSrc;
  for(i=0; i<p->nSrc; i++){
    SrcItem *pNewItem = &pNew->a[i];
    const SrcItem *pOldItem = &p->a[i];
    Table *pTab;
    pNewItem->fg = pOldItem->fg;
    if( pOldItem->fg.isSubquery ){
      Subquery *pNewSubq = capdbDbMallocRaw(db, sizeof(Subquery));
      if( pNewSubq==0 ){
        assert( db->mallocFailed );
        pNewItem->fg.isSubquery = 0;
      }else{
        memcpy(pNewSubq, pOldItem->u4.pSubq, sizeof(*pNewSubq));
        pNewSubq->pSelect = capdbSelectDup(db, pNewSubq->pSelect, flags);
        if( pNewSubq->pSelect==0 ){
          capdbDbFree(db, pNewSubq);
          pNewSubq = 0;
          pNewItem->fg.isSubquery = 0;
        }
      }
      pNewItem->u4.pSubq = pNewSubq;
    }else if( pOldItem->fg.fixedSchema ){
      pNewItem->u4.pSchema = pOldItem->u4.pSchema;
    }else{
      pNewItem->u4.zDatabase = capdbDbStrDup(db, pOldItem->u4.zDatabase);
    }
    pNewItem->zName = capdbDbStrDup(db, pOldItem->zName);
    pNewItem->zAlias = capdbDbStrDup(db, pOldItem->zAlias);
    pNewItem->iCursor = pOldItem->iCursor;
    if( pNewItem->fg.isIndexedBy ){
      pNewItem->u1.zIndexedBy = capdbDbStrDup(db, pOldItem->u1.zIndexedBy);
    }else if( pNewItem->fg.isTabFunc ){
      pNewItem->u1.pFuncArg =
          capdbExprListDup(db, pOldItem->u1.pFuncArg, flags);
    }else{
      pNewItem->u1.nRow = pOldItem->u1.nRow;
    }
    pNewItem->u2 = pOldItem->u2;
    if( pNewItem->fg.isCte ){
      pNewItem->u2.pCteUse->nUse++;
    }
    pTab = pNewItem->pSTab = pOldItem->pSTab;
    if( pTab ){
      pTab->nTabRef++;
    }
    if( pOldItem->fg.isUsing ){
      assert( pNewItem->fg.isUsing );
      pNewItem->u3.pUsing = capdbIdListDup(db, pOldItem->u3.pUsing);
    }else{
      pNewItem->u3.pOn = capdbExprDup(db, pOldItem->u3.pOn, flags);
    }
    pNewItem->colUsed = pOldItem->colUsed;
  }
  return pNew;
}
IdList *capdbIdListDup(capdb *db, const IdList *p){
  IdList *pNew;
  int i;
  assert( db!=0 );
  if( p==0 ) return 0;
  pNew = capdbDbMallocRawNN(db, SZ_IDLIST(p->nId));
  if( pNew==0 ) return 0;
  pNew->nId = p->nId;
  for(i=0; i<p->nId; i++){
    struct IdList_item *pNewItem = &pNew->a[i];
    const struct IdList_item *pOldItem = &p->a[i];
    pNewItem->zName = capdbDbStrDup(db, pOldItem->zName);
  }
  return pNew;
}
Select *capdbSelectDup(capdb *db, const Select *pDup, int flags){
  Select *pRet = 0;
  Select *pNext = 0;
  Select **pp = &pRet;
  const Select *p;

  assert( db!=0 );
  for(p=pDup; p; p=p->pPrior){
    Select *pNew = capdbDbMallocRawNN(db, sizeof(*p) );
    if( pNew==0 ) break;
    pNew->pEList = capdbExprListDup(db, p->pEList, flags);
    pNew->pSrc = capdbSrcListDup(db, p->pSrc, flags);
    pNew->pWhere = capdbExprDup(db, p->pWhere, flags);
    pNew->pGroupBy = capdbExprListDup(db, p->pGroupBy, flags);
    pNew->pHaving = capdbExprDup(db, p->pHaving, flags);
    pNew->pOrderBy = capdbExprListDup(db, p->pOrderBy, flags);
    pNew->op = p->op;
    pNew->pNext = pNext;
    pNew->pPrior = 0;
    pNew->pLimit = capdbExprDup(db, p->pLimit, flags);
    pNew->iLimit = 0;
    pNew->iOffset = 0;
    pNew->selFlags = p->selFlags;
    pNew->nSelectRow = p->nSelectRow;
    pNew->pWith = capdbWithDup(db, p->pWith);
#ifndef CAPDB_OMIT_WINDOWFUNC
    pNew->pWin = 0;
    pNew->pWinDefn = capdbWindowListDup(db, p->pWinDefn);
    if( p->pWin && db->mallocFailed==0 ) gatherSelectWindows(pNew);
#endif
    pNew->selId = p->selId;
    if( db->mallocFailed ){
      /* Any prior OOM might have left the Select object incomplete.
      ** Delete the whole thing rather than allow an incomplete Select
      ** to be used by the code generator. */
      pNew->pNext = 0;
      capdbSelectDelete(db, pNew);
      break;
    }
    *pp = pNew;
    pp = &pNew->pPrior;
    pNext = pNew;
  }
  return pRet;
}
#else
Select *capdbSelectDup(capdb *db, const Select *p, int flags){
  assert( p==0 );
  return 0;
}
#endif


/*
** Add a new element to the end of an expression list.  If pList is
** initially NULL, then create a new expression list.
**
** The pList argument must be either NULL or a pointer to an ExprList
** obtained from a prior call to capdbExprListAppend().
**
** If a memory allocation error occurs, the entire list is freed and
** NULL is returned.  If non-NULL is returned, then it is guaranteed
** that the new entry was successfully appended.
*/
static const struct ExprList_item zeroItem = {0};
CAPDB_NOINLINE ExprList *capdbExprListAppendNew(
  capdb *db,            /* Database handle.  Used for memory allocation */
  Expr *pExpr             /* Expression to be appended. Might be NULL */
){
  struct ExprList_item *pItem;
  ExprList *pList;

  pList = capdbDbMallocRawNN(db, SZ_EXPRLIST(4));
  if( pList==0 ){
    capdbExprDelete(db, pExpr);
    return 0;
  }
  pList->nAlloc = 4;
  pList->nExpr = 1;
  pItem = &pList->a[0];
  *pItem = zeroItem;
  pItem->pExpr = pExpr;
  return pList;
}
CAPDB_NOINLINE ExprList *capdbExprListAppendGrow(
  capdb *db,            /* Database handle.  Used for memory allocation */
  ExprList *pList,        /* List to which to append. Might be NULL */
  Expr *pExpr             /* Expression to be appended. Might be NULL */
){
  struct ExprList_item *pItem;
  ExprList *pNew;
  pList->nAlloc *= 2;
  pNew = capdbDbRealloc(db, pList, SZ_EXPRLIST(pList->nAlloc));
  if( pNew==0 ){
    capdbExprListDelete(db, pList);
    capdbExprDelete(db, pExpr);
    return 0;
  }else{
    pList = pNew;
  }
  pItem = &pList->a[pList->nExpr++];
  *pItem = zeroItem;
  pItem->pExpr = pExpr;
  return pList;
}
ExprList *capdbExprListAppend(
  Parse *pParse,          /* Parsing context */
  ExprList *pList,        /* List to which to append. Might be NULL */
  Expr *pExpr             /* Expression to be appended. Might be NULL */
){
  struct ExprList_item *pItem;
  if( pList==0 ){
    return capdbExprListAppendNew(pParse->db,pExpr);
  }
  if( pList->nAlloc<pList->nExpr+1 ){
    return capdbExprListAppendGrow(pParse->db,pList,pExpr);
  }
  pItem = &pList->a[pList->nExpr++];
  *pItem = zeroItem;
  pItem->pExpr = pExpr;
  return pList;
}

/*
** pColumns and pExpr form a vector assignment which is part of the SET
** clause of an UPDATE statement.  Like this:
**
**        (a,b,c) = (expr1,expr2,expr3)
** Or:    (a,b,c) = (SELECT x,y,z FROM ....)
**
** For each term of the vector assignment, append new entries to the
** expression list pList.  In the case of a subquery on the RHS, append
** TK_SELECT_COLUMN expressions.
*/
ExprList *capdbExprListAppendVector(
  Parse *pParse,         /* Parsing context */
  ExprList *pList,       /* List to which to append. Might be NULL */
  IdList *pColumns,      /* List of names of LHS of the assignment */
  Expr *pExpr            /* Vector expression to be appended. Might be NULL */
){
  capdb *db = pParse->db;
  int n;
  int i;
  int iFirst = pList ? pList->nExpr : 0;
  /* pColumns can only be NULL due to an OOM but an OOM will cause an
  ** exit prior to this routine being invoked */
  if( NEVER(pColumns==0) ) goto vector_append_error;
  if( pExpr==0 ) goto vector_append_error;

  /* If the RHS is a vector, then we can immediately check to see that
  ** the size of the RHS and LHS match.  But if the RHS is a SELECT,
  ** wildcards ("*") in the result set of the SELECT must be expanded before
  ** we can do the size check, so defer the size check until code generation.
  */
  if( pExpr->op!=TK_SELECT && pColumns->nId!=(n=capdbExprVectorSize(pExpr)) ){
    capdbErrorMsg(pParse, "%d columns assigned %d values",
                    pColumns->nId, n);
    goto vector_append_error;
  }

  for(i=0; i<pColumns->nId; i++){
    Expr *pSubExpr = capdbExprForVectorField(pParse, pExpr, i, pColumns->nId);
    assert( pSubExpr!=0 || db->mallocFailed );
    if( pSubExpr==0 ) continue;
    pList = capdbExprListAppend(pParse, pList, pSubExpr);
    if( pList ){
      assert( pList->nExpr==iFirst+i+1 );
      pList->a[pList->nExpr-1].zEName = pColumns->a[i].zName;
      pColumns->a[i].zName = 0;
    }
  }

  if( !db->mallocFailed && pExpr->op==TK_SELECT && ALWAYS(pList!=0) ){
    Expr *pFirst = pList->a[iFirst].pExpr;
    assert( pFirst!=0 );
    assert( pFirst->op==TK_SELECT_COLUMN );
    
    /* Store the SELECT statement in pRight so it will be deleted when
    ** capdbExprListDelete() is called */
    pFirst->pRight = pExpr;
    pExpr = 0;

    /* Remember the size of the LHS in iTable so that we can check that
    ** the RHS and LHS sizes match during code generation. */
    pFirst->iTable = pColumns->nId;
  }

vector_append_error:
  capdbExprUnmapAndDelete(pParse, pExpr);
  capdbIdListDelete(db, pColumns);
  return pList;
}

/*
** Set the sort order for the last element on the given ExprList.
*/
void capdbExprListSetSortOrder(ExprList *p, int iSortOrder, int eNulls){
  struct ExprList_item *pItem;
  if( p==0 ) return;
  assert( p->nExpr>0 );

  assert( CAPDB_SO_UNDEFINED<0 && CAPDB_SO_ASC==0 && CAPDB_SO_DESC>0 );
  assert( iSortOrder==CAPDB_SO_UNDEFINED
       || iSortOrder==CAPDB_SO_ASC
       || iSortOrder==CAPDB_SO_DESC
  );
  assert( eNulls==CAPDB_SO_UNDEFINED
       || eNulls==CAPDB_SO_ASC
       || eNulls==CAPDB_SO_DESC
  );

  pItem = &p->a[p->nExpr-1];
  assert( pItem->fg.bNulls==0 );
  if( iSortOrder==CAPDB_SO_UNDEFINED ){
    iSortOrder = CAPDB_SO_ASC;
  }
  pItem->fg.sortFlags = (u8)iSortOrder;

  if( eNulls!=CAPDB_SO_UNDEFINED ){
    pItem->fg.bNulls = 1;
    if( iSortOrder!=eNulls ){
      pItem->fg.sortFlags |= KEYINFO_ORDER_BIGNULL;
    }
  }
}

/*
** Set the ExprList.a[].zEName element of the most recently added item
** on the expression list.
**
** pList might be NULL following an OOM error.  But pName should never be
** NULL.  If a memory allocation fails, the pParse->db->mallocFailed flag
** is set.
*/
void capdbExprListSetName(
  Parse *pParse,          /* Parsing context */
  ExprList *pList,        /* List to which to add the span. */
  const Token *pName,     /* Name to be added */
  int dequote             /* True to cause the name to be dequoted */
){
  assert( pList!=0 || pParse->db->mallocFailed!=0 );
  assert( pParse->eParseMode!=PARSE_MODE_UNMAP || dequote==0 );
  if( pList ){
    struct ExprList_item *pItem;
    assert( pList->nExpr>0 );
    pItem = &pList->a[pList->nExpr-1];
    assert( pItem->zEName==0 );
    assert( pItem->fg.eEName==ENAME_NAME );
    pItem->zEName = capdbDbStrNDup(pParse->db, pName->z, pName->n);
    if( dequote ){
      /* If dequote==0, then pName->z does not point to part of a DDL
      ** statement handled by the parser. And so no token need be added
      ** to the token-map.  */
      capdbDequote(pItem->zEName);
      if( IN_RENAME_OBJECT ){
        capdbRenameTokenMap(pParse, (const void*)pItem->zEName, pName);
      }
    }
  }
}

/*
** Set the ExprList.a[].zSpan element of the most recently added item
** on the expression list.
**
** pList might be NULL following an OOM error.  But pSpan should never be
** NULL.  If a memory allocation fails, the pParse->db->mallocFailed flag
** is set.
*/
void capdbExprListSetSpan(
  Parse *pParse,          /* Parsing context */
  ExprList *pList,        /* List to which to add the span. */
  const char *zStart,     /* Start of the span */
  const char *zEnd        /* End of the span */
){
  capdb *db = pParse->db;
  assert( pList!=0 || db->mallocFailed!=0 );
  if( pList ){
    struct ExprList_item *pItem = &pList->a[pList->nExpr-1];
    assert( pList->nExpr>0 );
    if( pItem->zEName==0 ){
      pItem->zEName = capdbDbSpanDup(db, zStart, zEnd);
      pItem->fg.eEName = ENAME_SPAN;
    }
  }
}

/*
** If the expression list pEList contains more than iLimit elements,
** leave an error message in pParse.
*/
void capdbExprListCheckLength(
  Parse *pParse,
  ExprList *pEList,
  const char *zObject
){
  int mx = pParse->db->aLimit[CAPDB_LIMIT_COLUMN];
  testcase( pEList && pEList->nExpr==mx );
  testcase( pEList && pEList->nExpr==mx+1 );
  if( pEList && pEList->nExpr>mx ){
    capdbErrorMsg(pParse, "too many columns in %s", zObject);
  }
}

/*
** Delete an entire expression list.
*/
static CAPDB_NOINLINE void exprListDeleteNN(capdb *db, ExprList *pList){
  int i = pList->nExpr;
  struct ExprList_item *pItem =  pList->a;
  assert( pList->nExpr>0 );
  assert( db!=0 );
  do{
    capdbExprDelete(db, pItem->pExpr);
    if( pItem->zEName ) capdbDbNNFreeNN(db, pItem->zEName);
    pItem++;
  }while( --i>0 );
  capdbDbNNFreeNN(db, pList);
}
void capdbExprListDelete(capdb *db, ExprList *pList){
  if( pList ) exprListDeleteNN(db, pList);
}
void capdbExprListDeleteGeneric(capdb *db, void *pList){
  if( ALWAYS(pList) ) exprListDeleteNN(db, (ExprList*)pList);
}

/*
** Return the bitwise-OR of all Expr.flags fields in the given
** ExprList.
*/
u32 capdbExprListFlags(const ExprList *pList){
  int i;
  u32 m = 0;
  assert( pList!=0 );
  for(i=0; i<pList->nExpr; i++){
     Expr *pExpr = pList->a[i].pExpr;
     assert( pExpr!=0 );
     m |= pExpr->flags;
  }
  return m;
}

/*
** This is a SELECT-node callback for the expression walker that
** always "fails".  By "fail" in this case, we mean set
** pWalker->eCode to zero and abort.
**
** This callback is used by multiple expression walkers.
*/
int capdbSelectWalkFail(Walker *pWalker, Select *NotUsed){
  UNUSED_PARAMETER(NotUsed);
  pWalker->eCode = 0;
  return WRC_Abort;
}

/*
** Check the input string to see if it is "true" or "false" (in any case).
**
**       If the string is....           Return
**         "true"                         EP_IsTrue
**         "false"                        EP_IsFalse
**         anything else                  0
*/
u32 capdbIsTrueOrFalse(const char *zIn){
  if( capdbStrICmp(zIn, "true")==0  ) return EP_IsTrue;
  if( capdbStrICmp(zIn, "false")==0 ) return EP_IsFalse;
  return 0;
}


/*
** If the input expression is an ID with the name "true" or "false"
** then convert it into an TK_TRUEFALSE term.  Return non-zero if
** the conversion happened, and zero if the expression is unaltered.
*/
int capdbExprIdToTrueFalse(Expr *pExpr){
  u32 v;
  assert( pExpr->op==TK_ID || pExpr->op==TK_STRING );
  if( !ExprHasProperty(pExpr, EP_Quoted|EP_IntValue)
   && (v = capdbIsTrueOrFalse(pExpr->u.zToken))!=0
  ){
    pExpr->op = TK_TRUEFALSE;
    ExprSetProperty(pExpr, v);
    return 1;
  }
  return 0;
}

/*
** The argument must be a TK_TRUEFALSE Expr node.  Return 1 if it is TRUE
** and 0 if it is FALSE.
*/
int capdbExprTruthValue(const Expr *pExpr){
  pExpr = capdbExprSkipCollateAndLikely((Expr*)pExpr);
  assert( pExpr->op==TK_TRUEFALSE );
  assert( !ExprHasProperty(pExpr, EP_IntValue) );
  assert( capdbStrICmp(pExpr->u.zToken,"true")==0
       || capdbStrICmp(pExpr->u.zToken,"false")==0 );
  return pExpr->u.zToken[4]==0;
}

/*
** If pExpr is an AND or OR expression, try to simplify it by eliminating
** terms that are always true or false.  Return the simplified expression.
** Or return the original expression if no simplification is possible.
**
** Examples:
**
**     (x<10) AND true                =>   (x<10)
**     (x<10) AND false               =>   false
**     (x<10) AND (y=22 OR false)     =>   (x<10) AND (y=22)
**     (x<10) AND (y=22 OR true)      =>   (x<10)
**     (y=22) OR true                 =>   true
*/
Expr *capdbExprSimplifiedAndOr(Expr *pExpr){
  assert( pExpr!=0 );
  if( pExpr->op==TK_AND || pExpr->op==TK_OR ){
    Expr *pRight = capdbExprSimplifiedAndOr(pExpr->pRight);
    Expr *pLeft = capdbExprSimplifiedAndOr(pExpr->pLeft);
    if( ExprAlwaysTrue(pLeft) || ExprAlwaysFalse(pRight) ){
      pExpr = pExpr->op==TK_AND ? pRight : pLeft;
    }else if( ExprAlwaysTrue(pRight) || ExprAlwaysFalse(pLeft) ){
      pExpr = pExpr->op==TK_AND ? pLeft : pRight;
    }
  }
  return pExpr;
}

/*
** Return true if it might be advantageous to compute the right operand
** of expression pExpr first, before the left operand.
**
** Normally the left operand is computed before the right operand.  But if
** the left operand contains a subquery and the right does not, then it
** might be more efficient to compute the right operand first.
*/
static int exprEvalRhsFirst(Expr *pExpr){
  if( ExprHasProperty(pExpr->pLeft, EP_Subquery)
   && !ExprHasProperty(pExpr->pRight, EP_Subquery)
  ){
    return 1;
  }else{
    return 0;
  }
}

/*
** Compute the two operands of a binary operator.
**
** If either operand contains a subquery, then the code strives to
** compute the operand containing the subquery second.  If the other
** operand evalutes to NULL, then a jump is made.  The address of the
** IsNull operand that does this jump is returned.  The caller can use
** this to optimize the computation so as to avoid doing the potentially
** expensive subquery.
**
** If no optimization opportunities exist, return 0.
*/
static int exprComputeOperands(
  Parse *pParse,     /* Parsing context */
  Expr *pExpr,       /* The comparison expression */
  int *pR1,          /* OUT: Register holding the left operand */
  int *pR2,          /* OUT: Register holding the right operand */
  int *pFree1,       /* OUT: Temp register to free if not zero */
  int *pFree2        /* OUT: Another temp register to free if not zero */
){
  int addrIsNull;
  int r1, r2;
  Vdbe *v = pParse->pVdbe;

  assert( v!=0 );
  /*
  ** If the left operand contains a (possibly expensive) subquery and the
  ** right operand does not and the right operation might be NULL,
  ** then compute the right operand first and do an IsNull jump if the
  ** right operand evalutes to NULL.
  */
  if( exprEvalRhsFirst(pExpr) && capdbExprCanBeNull(pExpr->pRight) ){
    r2 = capdbExprCodeTemp(pParse, pExpr->pRight, pFree2);
    addrIsNull = capdbVdbeAddOp1(v, OP_IsNull, r2);
    VdbeComment((v, "skip left operand"));
    VdbeCoverage(v);
  }else{
    r2 = 0; /* Silence a false-positive uninit-var warning in MSVC */
    addrIsNull = 0;
  }
  r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, pFree1);
  if( addrIsNull==0 ){
    /*
    ** If the right operand contains a subquery and the left operand does not
    ** and the left operand might be NULL, then do an IsNull check
    ** check on the left operand before computing the right operand.
    */
    if( ExprHasProperty(pExpr->pRight, EP_Subquery)
     && capdbExprCanBeNull(pExpr->pLeft)
    ){
      addrIsNull = capdbVdbeAddOp1(v, OP_IsNull, r1);
      VdbeComment((v, "skip right operand"));
      VdbeCoverage(v);
    }
    r2 = capdbExprCodeTemp(pParse, pExpr->pRight, pFree2);
  }
  *pR1 = r1;
  *pR2 = r2;
  return addrIsNull;
}

/*
** pExpr is a TK_FUNCTION node.  Try to determine whether or not the
** function is a constant function.  A function is constant if all of
** the following are true:
**
**    (1)  It is a scalar function (not an aggregate or window function)
**    (2)  It has either the CAPDB_FUNC_CONSTANT or CAPDB_FUNC_SLOCHNG
**         property.
**    (3)  All of its arguments are constants
**
** This routine sets pWalker->eCode to 0 if pExpr is not a constant.
** It makes no changes to pWalker->eCode if pExpr is constant.  In
** every case, it returns WRC_Abort.
**
** Called as a service subroutine from exprNodeIsConstant().
*/
static CAPDB_NOINLINE int exprNodeIsConstantFunction(
  Walker *pWalker,
  Expr *pExpr
){
  int n;             /* Number of arguments */
  ExprList *pList;   /* List of arguments */
  FuncDef *pDef;     /* The function */
  capdb *db;       /* The database */

  assert( pExpr->op==TK_FUNCTION );
  if( ExprHasProperty(pExpr, EP_TokenOnly)
   || (pList = pExpr->x.pList)==0
  ){;
    n = 0;
  }else{
    n = pList->nExpr;
    capdbWalkExprList(pWalker, pList);
    if( pWalker->eCode==0 ) return WRC_Abort;
  }
  db = pWalker->pParse->db;
  pDef = capdbFindFunction(db, pExpr->u.zToken, n, ENC(db), 0);
  if( pDef==0
   || pDef->xFinalize!=0
   || (pDef->funcFlags & (CAPDB_FUNC_CONSTANT|CAPDB_FUNC_SLOCHNG))==0
   || ExprHasProperty(pExpr, EP_WinFunc)
  ){
    pWalker->eCode = 0;
    return WRC_Abort;
  }
  return WRC_Prune;
}


/*
** These routines are Walker callbacks used to check expressions to
** see if they are "constant" for some definition of constant.  The
** Walker.eCode value determines the type of "constant" we are looking
** for.
**
** These callback routines are used to implement the following:
**
**     capdbExprIsConstant()                  pWalker->eCode==1
**     capdbExprIsConstantNotJoin()           pWalker->eCode==2
**     capdbExprIsTableConstant()             pWalker->eCode==3
**     capdbExprIsConstantOrFunction()        pWalker->eCode==4 or 5
**
** In all cases, the callbacks set Walker.eCode=0 and abort if the expression
** is found to not be a constant.
**
** The capdbExprIsConstantOrFunction() is used for evaluating DEFAULT
** expressions in a CREATE TABLE statement.  The Walker.eCode value is 5
** when parsing an existing schema out of the sqlite_schema table and 4
** when processing a new CREATE TABLE statement.  A bound parameter raises
** an error for new statements, but is silently converted
** to NULL for existing schemas.  This allows sqlite_schema tables that
** contain a bound parameter because they were generated by older versions
** of SQLite to be parsed by newer versions of SQLite without raising a
** malformed schema error.
*/
static int exprNodeIsConstant(Walker *pWalker, Expr *pExpr){
  assert( pWalker->eCode>0 );

  /* If pWalker->eCode is 2 then any term of the expression that comes from
  ** the ON or USING clauses of an outer join disqualifies the expression
  ** from being considered constant. */
  if( pWalker->eCode==2 && ExprHasProperty(pExpr, EP_OuterON) ){
    pWalker->eCode = 0;
    return WRC_Abort;
  }

  switch( pExpr->op ){
    /* Consider functions to be constant if all their arguments are constant
    ** and either pWalker->eCode==4 or 5 or the function has the
    ** CAPDB_FUNC_CONST flag. */
    case TK_FUNCTION:
      if( (pWalker->eCode>=4 || ExprHasProperty(pExpr,EP_ConstFunc))
       && !ExprHasProperty(pExpr, EP_WinFunc)
      ){
        if( pWalker->eCode==5 ) ExprSetProperty(pExpr, EP_FromDDL);
        return WRC_Continue;
      }else if( pWalker->pParse ){
        return exprNodeIsConstantFunction(pWalker, pExpr);
      }else{
        pWalker->eCode = 0;
        return WRC_Abort;
      }
    case TK_ID:
      /* Convert "true" or "false" in a DEFAULT clause into the
      ** appropriate TK_TRUEFALSE operator */
      if( capdbExprIdToTrueFalse(pExpr) ){
        return WRC_Prune;
      }
      /* no break */ deliberate_fall_through
    case TK_COLUMN:
    case TK_AGG_FUNCTION:
    case TK_AGG_COLUMN:
      testcase( pExpr->op==TK_ID );
      testcase( pExpr->op==TK_COLUMN );
      testcase( pExpr->op==TK_AGG_FUNCTION );
      testcase( pExpr->op==TK_AGG_COLUMN );
      if( ExprHasProperty(pExpr, EP_FixedCol) && pWalker->eCode!=2 ){
        return WRC_Continue;
      }
      if( pWalker->eCode==3 && pExpr->iTable==pWalker->u.iCur ){
        return WRC_Continue;
      }
      /* no break */ deliberate_fall_through
    case TK_IF_NULL_ROW:
    case TK_REGISTER:
    case TK_DOT:
    case TK_RAISE:
      testcase( pExpr->op==TK_REGISTER );
      testcase( pExpr->op==TK_IF_NULL_ROW );
      testcase( pExpr->op==TK_DOT );
      testcase( pExpr->op==TK_RAISE );
      pWalker->eCode = 0;
      return WRC_Abort;
    case TK_VARIABLE:
      if( pWalker->eCode==5 ){
        /* Silently convert bound parameters that appear inside of CREATE
        ** statements into a NULL when parsing the CREATE statement text out
        ** of the sqlite_schema table */
        pExpr->op = TK_NULL;
      }else if( pWalker->eCode==4 ){
        /* A bound parameter in a CREATE statement that originates from
        ** capdb_prepare() causes an error */
        pWalker->eCode = 0;
        return WRC_Abort;
      }
      /* no break */ deliberate_fall_through
    default:
      testcase( pExpr->op==TK_SELECT ); /* capdbSelectWalkFail() disallows */
      testcase( pExpr->op==TK_EXISTS ); /* capdbSelectWalkFail() disallows */
      return WRC_Continue;
  }
}
static int exprIsConst(Parse *pParse, Expr *p, int initFlag){
  Walker w;
  w.eCode = initFlag;
  w.pParse = pParse;
  w.xExprCallback = exprNodeIsConstant;
  w.xSelectCallback = capdbSelectWalkFail;
#ifdef CAPDB_DEBUG
  w.xSelectCallback2 = capdbSelectWalkAssert2;
#endif
  capdbWalkExpr(&w, p);
  return w.eCode;
}

/*
** Walk an expression tree.  Return non-zero if the expression is constant
** or return zero if the expression involves variables or function calls.
**
** For the purposes of this function, a double-quoted string (ex: "abc")
** is considered a variable but a single-quoted string (ex: 'abc') is
** a constant.
**
** The pParse parameter may be NULL.  But if it is NULL, there is no way
** to determine if function calls are constant or not, and hence all
** function calls will be considered to be non-constant.  If pParse is
** not NULL, then a function call might be constant, depending on the
** function and on its parameters.
*/
int capdbExprIsConstant(Parse *pParse, Expr *p){
  return exprIsConst(pParse, p, 1);
}

/*
** Walk an expression tree.  Return non-zero if
**
**   (1) the expression is constant, and
**   (2) the expression does not originate in the ON or USING clause
**       of a LEFT JOIN, and
**   (3) the expression does not contain any EP_FixedCol TK_COLUMN
**       operands created by the constant propagation optimization.
**
** When this routine returns true, it indicates that the expression
** can be added to the pParse->pConstExpr list and evaluated once when
** the prepared statement starts up.  See capdbExprCodeRunJustOnce().
*/
static int capdbExprIsConstantNotJoin(Parse *pParse, Expr *p){
  return exprIsConst(pParse, p, 2);
}

/*
** This routine examines sub-SELECT statements as an expression is being
** walked as part of capdbExprIsTableConstant().  Sub-SELECTs are considered
** constant as long as they are uncorrelated - meaning that they do not
** contain any terms from outer contexts.
*/
static int exprSelectWalkTableConstant(Walker *pWalker, Select *pSelect){
  assert( pSelect!=0 );
  assert( pWalker->eCode==3 || pWalker->eCode==0 );
  if( (pSelect->selFlags & SF_Correlated)!=0 ){
    pWalker->eCode = 0;
    return WRC_Abort;
  }
  return WRC_Prune;
}

/*
** Walk an expression tree.  Return non-zero if the expression is constant
** for any single row of the table with cursor iCur.  In other words, the
** expression must not refer to any non-deterministic function nor any
** table other than iCur.
**
** Consider uncorrelated subqueries to be constants if the bAllowSubq
** parameter is true.
*/
static int capdbExprIsTableConstant(Expr *p, int iCur, int bAllowSubq){
  Walker w;
  w.eCode = 3;
  w.pParse = 0;
  w.xExprCallback = exprNodeIsConstant;
  if( bAllowSubq ){
    w.xSelectCallback = exprSelectWalkTableConstant;
  }else{
    w.xSelectCallback = capdbSelectWalkFail;
#ifdef CAPDB_DEBUG
    w.xSelectCallback2 = capdbSelectWalkAssert2;
#endif
  }
  w.u.iCur = iCur;
  capdbWalkExpr(&w, p);
  return w.eCode;
}

/*
** Check pExpr to see if it is an constraint on the single data source
** pSrc = &pSrcList->a[iSrc].  In other words, check to see if pExpr
** constrains pSrc but does not depend on any other tables or data
** sources anywhere else in the query.  Return true (non-zero) if pExpr
** is a constraint on pSrc only.
**
** This is an optimization.  False negatives will perhaps cause slower
** queries, but false positives will yield incorrect answers.  So when in
** doubt, return 0.
**
** To be an single-source constraint, the following must be true:
**
**   (1)  pExpr cannot refer to any table other than pSrc->iCursor.
**
**   (2a) pExpr cannot use subqueries unless the bAllowSubq parameter is
**        true and the subquery is non-correlated
**
**   (2b) pExpr cannot use non-deterministic functions.
**
**   (3)  pSrc cannot be part of the left operand for a RIGHT JOIN.
**        (Is there some way to relax this constraint?)
**
**   (4)  If pSrc is the right operand of a LEFT JOIN, then...
**         (4a)  pExpr must come from an ON clause..
**         (4b)  and specifically the ON clause associated with the LEFT JOIN.
**
**   (5)  If pSrc is the right operand of a LEFT JOIN or the left
**        operand of a RIGHT JOIN, then pExpr must be from the WHERE
**        clause, not an ON clause.
**
**   (6) Either:
**
**       (6a) pExpr does not originate in an ON or USING clause, or
**
**       (6b) The ON or USING clause from which pExpr is derived is
**            not to the left of a RIGHT JOIN (or FULL JOIN).
**
**       Without this restriction, accepting pExpr as a single-table
**       constraint might move the the ON/USING filter expression
**       from the left side of a RIGHT JOIN over to the right side,
**       which leads to incorrect answers.  See also restriction (9)
**       on push-down.
*/
int capdbExprIsSingleTableConstraint(
  Expr *pExpr,                 /* The constraint */
  const SrcList *pSrcList,     /* Complete FROM clause */
  int iSrc,                    /* Which element of pSrcList to use */
  int bAllowSubq               /* Allow non-correlated subqueries */
){
  const SrcItem *pSrc = &pSrcList->a[iSrc];
  if( pSrc->fg.jointype & JT_LTORJ ){
    return 0;  /* rule (3) */
  }
  if( pSrc->fg.jointype & JT_LEFT ){
    if( !ExprHasProperty(pExpr, EP_OuterON) ) return 0;   /* rule (4a) */
    if( pExpr->w.iJoin!=pSrc->iCursor ) return 0;         /* rule (4b) */
  }else{
    if( ExprHasProperty(pExpr, EP_OuterON) ) return 0;    /* rule (5) */
  }
  if( ExprHasProperty(pExpr, EP_OuterON|EP_InnerON)  /* (6a) */
   && (pSrcList->a[0].fg.jointype & JT_LTORJ)!=0     /* Fast pre-test of (6b) */
  ){
    int jj;
    for(jj=0; jj<iSrc; jj++){
      if( pExpr->w.iJoin==pSrcList->a[jj].iCursor ){
        if( (pSrcList->a[jj].fg.jointype & JT_LTORJ)!=0 ){
          return 0;  /* restriction (6) */
        }
        break;
      }
    }
  }
  /* Rules (1), (2a), and (2b) handled by the following: */
  return capdbExprIsTableConstant(pExpr, pSrc->iCursor, bAllowSubq);
}


/*
** capdbWalkExpr() callback used by capdbExprIsConstantOrGroupBy().
*/
static int exprNodeIsConstantOrGroupBy(Walker *pWalker, Expr *pExpr){
  ExprList *pGroupBy = pWalker->u.pGroupBy;
  int i;

  /* Check if pExpr is identical to any GROUP BY term. If so, consider
  ** it constant.  */
  for(i=0; i<pGroupBy->nExpr; i++){
    Expr *p = pGroupBy->a[i].pExpr;
    if( capdbExprCompare(0, pExpr, p, -1)<2 ){
      CollSeq *pColl = capdbExprNNCollSeq(pWalker->pParse, p);
      if( capdbIsBinary(pColl) ){
        return WRC_Prune;
      }
    }
  }

  /* Check if pExpr is a sub-select. If so, consider it variable. */
  if( ExprUseXSelect(pExpr) ){
    pWalker->eCode = 0;
    return WRC_Abort;
  }

  return exprNodeIsConstant(pWalker, pExpr);
}

/*
** Walk the expression tree passed as the first argument. Return non-zero
** if the expression consists entirely of constants or copies of terms
** in pGroupBy that sort with the BINARY collation sequence.
**
** This routine is used to determine if a term of the HAVING clause can
** be promoted into the WHERE clause.  In order for such a promotion to work,
** the value of the HAVING clause term must be the same for all members of
** a "group".  The requirement that the GROUP BY term must be BINARY
** assumes that no other collating sequence will have a finer-grained
** grouping than binary.  In other words (A=B COLLATE binary) implies
** A=B in every other collating sequence.  The requirement that the
** GROUP BY be BINARY is stricter than necessary.  It would also work
** to promote HAVING clauses that use the same alternative collating
** sequence as the GROUP BY term, but that is much harder to check,
** alternative collating sequences are uncommon, and this is only an
** optimization, so we take the easy way out and simply require the
** GROUP BY to use the BINARY collating sequence.
*/
int capdbExprIsConstantOrGroupBy(Parse *pParse, Expr *p, ExprList *pGroupBy){
  Walker w;
  w.eCode = 1;
  w.xExprCallback = exprNodeIsConstantOrGroupBy;
  w.xSelectCallback = 0;
  w.u.pGroupBy = pGroupBy;
  w.pParse = pParse;
  capdbWalkExpr(&w, p);
  return w.eCode;
}

/*
** Walk an expression tree for the DEFAULT field of a column definition
** in a CREATE TABLE statement.  Return non-zero if the expression is
** acceptable for use as a DEFAULT.  That is to say, return non-zero if
** the expression is constant or a function call with constant arguments.
** Return and 0 if there are any variables.
**
** isInit is true when parsing from sqlite_schema.  isInit is false when
** processing a new CREATE TABLE statement.  When isInit is true, parameters
** (such as ? or $abc) in the expression are converted into NULL.  When
** isInit is false, parameters raise an error.  Parameters should not be
** allowed in a CREATE TABLE statement, but some legacy versions of SQLite
** allowed it, so we need to support it when reading sqlite_schema for
** backwards compatibility.
**
** If isInit is true, set EP_FromDDL on every TK_FUNCTION node.
**
** For the purposes of this function, a double-quoted string (ex: "abc")
** is considered a variable but a single-quoted string (ex: 'abc') is
** a constant.
*/
int capdbExprIsConstantOrFunction(Expr *p, u8 isInit){
  assert( isInit==0 || isInit==1 );
  return exprIsConst(0, p, 4+isInit);
}

#ifdef CAPDB_ENABLE_CURSOR_HINTS
/*
** Walk an expression tree.  Return 1 if the expression contains a
** subquery of some kind.  Return 0 if there are no subqueries.
*/
int capdbExprContainsSubquery(Expr *p){
  Walker w;
  w.eCode = 1;
  w.xExprCallback = capdbExprWalkNoop;
  w.xSelectCallback = capdbSelectWalkFail;
#ifdef CAPDB_DEBUG
  w.xSelectCallback2 = capdbSelectWalkAssert2;
#endif
  capdbWalkExpr(&w, p);
  return w.eCode==0;
}
#endif

/*
** If the expression p codes a constant integer that is small enough
** to fit in a 32-bit integer, return 1 and put the value of the integer
** in *pValue.  If the expression is not an integer or if it is too big
** to fit in a signed 32-bit integer, return 0 and leave *pValue unchanged.
**
** If the pParse pointer is provided, then allow the expression p to be
** a parameter (TK_VARIABLE) that is bound to an integer.
** But if pParse is NULL, then p must be a pure integer literal.
*/
int capdbExprIsInteger(const Expr *p, int *pValue, Parse *pParse){
  int rc = 0;
  if( NEVER(p==0) ) return 0;  /* Used to only happen following on OOM */

  /* If an expression is an integer literal that fits in a signed 32-bit
  ** integer, then the EP_IntValue flag will have already been set */
  assert( p->op!=TK_INTEGER || (p->flags & EP_IntValue)!=0
           || capdbGetInt32(p->u.zToken, &rc)==0 );

  if( p->flags & EP_IntValue ){
    *pValue = p->u.iValue;
    return 1;
  }
  switch( p->op ){
    case TK_UPLUS: {
      rc = capdbExprIsInteger(p->pLeft, pValue, 0);
      break;
    }
    case TK_UMINUS: {
      int v = 0;
      if( capdbExprIsInteger(p->pLeft, &v, 0) ){
        assert( ((unsigned int)v)!=0x80000000 );
        *pValue = -v;
        rc = 1;
      }
      break;
    }
    case TK_VARIABLE: {
      capdb_value *pVal;
      if( pParse==0 ) break;
      if( NEVER(pParse->pVdbe==0) ) break;
      if( (pParse->db->flags & CAPDB_EnableQPSG)!=0 ) break;
      capdbVdbeSetVarmask(pParse->pVdbe, p->iColumn);
      pVal = capdbVdbeGetBoundValue(pParse->pReprepare, p->iColumn,
                                      CAPDB_AFF_BLOB);
      if( pVal ){
        if( capdb_value_type(pVal)==CAPDB_INTEGER ){
          capdb_int64 vv = capdb_value_int64(pVal);
          if( vv == (vv & 0x7fffffff) ){ /* non-negative numbers only */
            *pValue = (int)vv;
            rc = 1;
          }
        }
        capdbValueFree(pVal);
      }
      break;
    }
    default: break;
  }
  return rc;
}

/*
** Return FALSE if there is no chance that the expression can be NULL.
**
** If the expression might be NULL or if the expression is too complex
** to tell return TRUE. 
**
** This routine is used as an optimization, to skip OP_IsNull opcodes
** when we know that a value cannot be NULL.  Hence, a false positive
** (returning TRUE when in fact the expression can never be NULL) might
** be a small performance hit but is otherwise harmless.  On the other
** hand, a false negative (returning FALSE when the result could be NULL)
** will likely result in an incorrect answer.  So when in doubt, return
** TRUE.
*/
int capdbExprCanBeNull(const Expr *p){
  u8 op;
  assert( p!=0 );
  while( p->op==TK_UPLUS || p->op==TK_UMINUS ){
    p = p->pLeft;
    assert( p!=0 );
  }
  op = p->op;
  if( op==TK_REGISTER ) op = p->op2;
  switch( op ){
    case TK_INTEGER:
    case TK_STRING:
    case TK_FLOAT:
    case TK_BLOB:
      return 0;
    case TK_COLUMN:
      assert( ExprUseYTab(p) );
      return ExprHasProperty(p, EP_CanBeNull)
          || NEVER(p->y.pTab==0) /* Reference to column of index on expr */
#ifdef CAPDB_ALLOW_ROWID_IN_VIEW
          || (p->iColumn==XN_ROWID && IsView(p->y.pTab))
#endif
          || (p->iColumn>=0
              && p->y.pTab->aCol!=0 /* Possible due to prior error */
              && ALWAYS(p->iColumn<p->y.pTab->nCol)
              && p->y.pTab->aCol[p->iColumn].notNull==0);
    default:
      return 1;
  }
}

/*
** Return TRUE if the given expression is a constant which would be
** unchanged by OP_Affinity with the affinity given in the second
** argument.
**
** This routine is used to determine if the OP_Affinity operation
** can be omitted.  When in doubt return FALSE.  A false negative
** is harmless.  A false positive, however, can result in the wrong
** answer.
*/
int capdbExprNeedsNoAffinityChange(const Expr *p, char aff){
  u8 op;
  int unaryMinus = 0;
  if( aff==CAPDB_AFF_BLOB ) return 1;
  while( p->op==TK_UPLUS || p->op==TK_UMINUS ){
    if( p->op==TK_UMINUS ) unaryMinus = 1;
    p = p->pLeft;
  }
  op = p->op;
  if( op==TK_REGISTER ) op = p->op2;
  switch( op ){
    case TK_INTEGER: {
      return aff>=CAPDB_AFF_NUMERIC;
    }
    case TK_FLOAT: {
      return aff>=CAPDB_AFF_NUMERIC;
    }
    case TK_STRING: {
      return !unaryMinus && aff==CAPDB_AFF_TEXT;
    }
    case TK_BLOB: {
      return !unaryMinus;
    }
    case TK_COLUMN: {
      assert( p->iTable>=0 );  /* p cannot be part of a CHECK constraint */
      return aff>=CAPDB_AFF_NUMERIC && p->iColumn<0;
    }
    default: {
      return 0;
    }
  }
}

/*
** Return TRUE if the given string is a row-id column name.
*/
int capdbIsRowid(const char *z){
  if( capdbStrICmp(z, "_ROWID_")==0 ) return 1;
  if( capdbStrICmp(z, "ROWID")==0 ) return 1;
  if( capdbStrICmp(z, "OID")==0 ) return 1;
  return 0;
}

/*
** Return a pointer to a buffer containing a usable rowid alias for table
** pTab. An alias is usable if there is not an explicit user-defined column 
** of the same name.
*/
const char *capdbRowidAlias(Table *pTab){
  const char *azOpt[] = {"_ROWID_", "ROWID", "OID"};
  int ii;
  assert( VisibleRowid(pTab) );
  for(ii=0; ii<ArraySize(azOpt); ii++){
    if( capdbColumnIndex(pTab, azOpt[ii])<0 ) return azOpt[ii];
  }
  return 0;
}

/*
** pX is the RHS of an IN operator.  If pX is a SELECT statement
** that can be simplified to a direct table access, then return
** a pointer to the SELECT statement.  If pX is not a SELECT statement,
** or if the SELECT statement needs to be materialized into a transient
** table, then return NULL.
*/
#ifndef CAPDB_OMIT_SUBQUERY
static Select *isCandidateForInOpt(const Expr *pX){
  Select *p;
  SrcList *pSrc;
  ExprList *pEList;
  Table *pTab;
  int i;
  if( !ExprUseXSelect(pX) ) return 0;                 /* Not a subquery */
  if( ExprHasProperty(pX, EP_VarSelect)  ) return 0;  /* Correlated subq */
  p = pX->x.pSelect;
  if( p->pPrior ) return 0;              /* Not a compound SELECT */
  if( p->selFlags & (SF_Distinct|SF_Aggregate) ){
    testcase( (p->selFlags & (SF_Distinct|SF_Aggregate))==SF_Distinct );
    testcase( (p->selFlags & (SF_Distinct|SF_Aggregate))==SF_Aggregate );
    return 0; /* No DISTINCT keyword and no aggregate functions */
  }
  assert( p->pGroupBy==0 );              /* Has no GROUP BY clause */
  if( p->pLimit ) return 0;              /* Has no LIMIT clause */
  if( p->pWhere ) return 0;              /* Has no WHERE clause */
  pSrc = p->pSrc;
  assert( pSrc!=0 );
  if( pSrc->nSrc!=1 ) return 0;          /* Single term in FROM clause */
  if( pSrc->a[0].fg.isSubquery) return 0;/* FROM is not a subquery or view */
  pTab = pSrc->a[0].pSTab;
  assert( pTab!=0 );
  assert( !IsView(pTab)  );              /* FROM clause is not a view */
  if( IsVirtual(pTab) ) return 0;        /* FROM clause not a virtual table */
  pEList = p->pEList;
  assert( pEList!=0 );
  /* All SELECT results must be columns. */
  for(i=0; i<pEList->nExpr; i++){
    Expr *pRes = pEList->a[i].pExpr;
    if( pRes->op!=TK_COLUMN ) return 0;
    assert( pRes->iTable==pSrc->a[0].iCursor );  /* Not a correlated subquery */
  }
  return p;
}
#endif /* CAPDB_OMIT_SUBQUERY */

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Generate code that checks the left-most column of index table iCur to see if
** it contains any NULL entries.  Cause the register at regHasNull to be set
** to a non-NULL value if iCur contains no NULLs.  Cause register regHasNull
** to be set to NULL if iCur contains one or more NULL values.
*/
static void capdbSetHasNullFlag(Vdbe *v, int iCur, int regHasNull){
  int addr1;
  capdbVdbeAddOp2(v, OP_Integer, 0, regHasNull);
  addr1 = capdbVdbeAddOp1(v, OP_Rewind, iCur); VdbeCoverage(v);
  capdbVdbeAddOp3(v, OP_Column, iCur, 0, regHasNull);
  capdbVdbeChangeP5(v, OPFLAG_TYPEOFARG);
  VdbeComment((v, "first_entry_in(%d)", iCur));
  capdbVdbeJumpHere(v, addr1);
}
#endif


#ifndef CAPDB_OMIT_SUBQUERY
/*
** The argument is an IN operator with a list (not a subquery) on the
** right-hand side.  Return TRUE if that list is constant.
*/
static int capdbInRhsIsConstant(Parse *pParse, Expr *pIn){
  Expr *pLHS;
  int res;
  assert( !ExprHasProperty(pIn, EP_xIsSelect) );
  pLHS = pIn->pLeft;
  pIn->pLeft = 0;
  res = capdbExprIsConstant(pParse, pIn);
  pIn->pLeft = pLHS;
  return res;
}
#endif

/*
** This function is used by the implementation of the IN (...) operator.
** The pX parameter is the expression on the RHS of the IN operator, which
** might be either a list of expressions or a subquery.
**
** The job of this routine is to find or create a b-tree object that can
** be used either to test for membership in the RHS set or to iterate through
** all members of the RHS set, skipping duplicates.
**
** A cursor is opened on the b-tree object that is the RHS of the IN operator
** and the *piTab parameter is set to the index of that cursor.
**
** The returned value of this function indicates the b-tree type, as follows:
**
**   IN_INDEX_ROWID      - The cursor was opened on a database table.
**   IN_INDEX_INDEX_ASC  - The cursor was opened on an ascending index.
**   IN_INDEX_INDEX_DESC - The cursor was opened on a descending index.
**   IN_INDEX_EPH        - The cursor was opened on a specially created and
**                         populated ephemeral table.
**   IN_INDEX_NOOP       - No cursor was allocated.  The IN operator must be
**                         implemented as a sequence of comparisons.
**
** An existing b-tree might be used if the RHS expression pX is a simple
** subquery such as:
**
**     SELECT <column1>, <column2>... FROM <table>
**
** If the RHS of the IN operator is a list or a more complex subquery, then
** an ephemeral table might need to be generated from the RHS and then
** pX->iTable made to point to the ephemeral table instead of an
** existing table.  In this case, the creation and initialization of the
** ephemeral table might be put inside of a subroutine, the EP_Subrtn flag
** will be set on pX and the pX->y.sub fields will be set to show where
** the subroutine is coded.
**
** The inFlags parameter must contain, at a minimum, one of the bits
** IN_INDEX_MEMBERSHIP or IN_INDEX_LOOP but not both.  If inFlags contains
** IN_INDEX_MEMBERSHIP, then the generated table will be used for a fast
** membership test.  When the IN_INDEX_LOOP bit is set, the IN index will
** be used to loop over all values of the RHS of the IN operator.
**
** When IN_INDEX_LOOP is used (and the b-tree will be used to iterate
** through the set members) then the b-tree must not contain duplicates.
** An ephemeral table will be created unless the selected columns are guaranteed
** to be unique - either because it is an INTEGER PRIMARY KEY or due to
** a UNIQUE constraint or index.
**
** When IN_INDEX_MEMBERSHIP is used (and the b-tree will be used
** for fast set membership tests) then an ephemeral table must
** be used unless <columns> is a single INTEGER PRIMARY KEY column or an
** index can be found with the specified <columns> as its left-most.
**
** If the IN_INDEX_NOOP_OK and IN_INDEX_MEMBERSHIP are both set and
** if the RHS of the IN operator is a list (not a subquery) then this
** routine might decide that creating an ephemeral b-tree for membership
** testing is too expensive and return IN_INDEX_NOOP.  In that case, the
** calling routine should implement the IN operator using a sequence
** of Eq or Ne comparison operations.
**
** When the b-tree is being used for membership tests, the calling function
** might need to know whether or not the RHS side of the IN operator
** contains a NULL.  If prRhsHasNull is not a NULL pointer and
** if there is any chance that the (...) might contain a NULL value at
** runtime, then a register is allocated and the register number written
** to *prRhsHasNull. If there is no chance that the (...) contains a
** NULL value, then *prRhsHasNull is left unchanged.
**
** If a register is allocated and its location stored in *prRhsHasNull, then
** the value in that register will be NULL if the b-tree contains one or more
** NULL values, and it will be some non-NULL value if the b-tree contains no
** NULL values.
**
** If the aiMap parameter is not NULL, it must point to an array containing
** one element for each column returned by the SELECT statement on the RHS
** of the IN(...) operator. The i'th entry of the array is populated with the
** offset of the index column that matches the i'th column returned by the
** SELECT. For example, if the expression and selected index are:
**
**   (?,?,?) IN (SELECT a, b, c FROM t1)
**   CREATE INDEX i1 ON t1(b, c, a);
**
** then aiMap[] is populated with {2, 0, 1}.
*/
#ifndef CAPDB_OMIT_SUBQUERY
int capdbFindInIndex(
  Parse *pParse,             /* Parsing context */
  Expr *pX,                  /* The IN expression */
  u32 inFlags,               /* IN_INDEX_LOOP, _MEMBERSHIP, and/or _NOOP_OK */
  int *prRhsHasNull,         /* Register holding NULL status.  See notes */
  int *aiMap,                /* Mapping from Index fields to RHS fields */
  int *piTab                 /* OUT: index to use */
){
  Select *p;                            /* SELECT to the right of IN operator */
  int eType = 0;                        /* Type of RHS table. IN_INDEX_* */
  int iTab;                             /* Cursor of the RHS table */
  int mustBeUnique;                     /* True if RHS must be unique */
  Vdbe *v = capdbGetVdbe(pParse);     /* Virtual machine being coded */

  assert( pX->op==TK_IN );
  mustBeUnique = (inFlags & IN_INDEX_LOOP)!=0;
  iTab = pParse->nTab++;

  /* If the RHS of this IN(...) operator is a SELECT, and if it matters
  ** whether or not the SELECT result contains NULL values, check whether
  ** or not NULL is actually possible (it may not be, for example, due
  ** to NOT NULL constraints in the schema). If no NULL values are possible,
  ** set prRhsHasNull to 0 before continuing.  */
  if( prRhsHasNull && ExprUseXSelect(pX) ){
    int i;
    ExprList *pEList = pX->x.pSelect->pEList;
    for(i=0; i<pEList->nExpr; i++){
      if( capdbExprCanBeNull(pEList->a[i].pExpr) ) break;
    }
    if( i==pEList->nExpr ){
      prRhsHasNull = 0;
    }
  }

  /* Check to see if an existing table or index can be used to
  ** satisfy the query.  This is preferable to generating a new
  ** ephemeral table.  */
  if( pParse->nErr==0 && (p = isCandidateForInOpt(pX))!=0 ){
    capdb *db = pParse->db;              /* Database connection */
    Table *pTab;                           /* Table <table>. */
    int iDb;                               /* Database idx for pTab */
    ExprList *pEList = p->pEList;
    int nExpr = pEList->nExpr;

    assert( p->pEList!=0 );             /* Because of isCandidateForInOpt(p) */
    assert( p->pEList->a[0].pExpr!=0 ); /* Because of isCandidateForInOpt(p) */
    assert( p->pSrc!=0 );               /* Because of isCandidateForInOpt(p) */
    pTab = p->pSrc->a[0].pSTab;

    /* Code an OP_Transaction and OP_TableLock for <table>. */
    iDb = capdbSchemaToIndex(db, pTab->pSchema);
    assert( iDb>=0 && iDb<CAPDB_MAX_DB );
    capdbCodeVerifySchema(pParse, iDb);
    capdbTableLock(pParse, iDb, pTab->tnum, 0, pTab->zName);

    assert(v);  /* capdbGetVdbe() has always been previously called */
    if( nExpr==1 && pEList->a[0].pExpr->iColumn<0 ){
      /* The "x IN (SELECT rowid FROM table)" case */
      int iAddr = capdbVdbeAddOp0(v, OP_Once);
      VdbeCoverage(v);

      capdbOpenTable(pParse, iTab, iDb, pTab, OP_OpenRead);
      eType = IN_INDEX_ROWID;
      ExplainQueryPlan((pParse, 0,
            "USING ROWID SEARCH ON TABLE %s FOR IN-OPERATOR",pTab->zName));
      capdbVdbeJumpHere(v, iAddr);
    }else{
      Index *pIdx;                         /* Iterator variable */
      int affinity_ok = 1;
      int i;

      /* Check that the affinity that will be used to perform each
      ** comparison is the same as the affinity of each column in table
      ** on the RHS of the IN operator.  If it not, it is not possible to
      ** use any index of the RHS table.  */
      for(i=0; i<nExpr && affinity_ok; i++){
        Expr *pLhs = capdbVectorFieldSubexpr(pX->pLeft, i);
        int iCol = pEList->a[i].pExpr->iColumn;
        char idxaff = capdbTableColumnAffinity(pTab,iCol); /* RHS table */
        char cmpaff = capdbCompareAffinity(pLhs, idxaff);
        testcase( cmpaff==CAPDB_AFF_BLOB );
        testcase( cmpaff==CAPDB_AFF_TEXT );
        switch( cmpaff ){
          case CAPDB_AFF_BLOB:
            break;
          case CAPDB_AFF_TEXT:
            /* capdbCompareAffinity() only returns TEXT if one side or the
            ** other has no affinity and the other side is TEXT.  Hence,
            ** the only way for cmpaff to be TEXT is for idxaff to be TEXT
            ** and for the term on the LHS of the IN to have no affinity. */
            assert( idxaff==CAPDB_AFF_TEXT );
            break;
          default:
            affinity_ok = capdbIsNumericAffinity(idxaff);
        }
      }

      if( affinity_ok ){
        /* Search for an existing index that will work for this IN operator */
        for(pIdx=pTab->pIndex; pIdx && eType==0; pIdx=pIdx->pNext){
          Bitmask colUsed;      /* Columns of the index used */
          Bitmask mCol;         /* Mask for the current column */
          if( pIdx->nColumn<nExpr ) continue;
          if( pIdx->pPartIdxWhere!=0 ) continue;
          /* Maximum nColumn is BMS-2, not BMS-1, so that we can compute
          ** BITMASK(nExpr) without overflowing */
          testcase( pIdx->nColumn==BMS-2 );
          testcase( pIdx->nColumn==BMS-1 );
          if( pIdx->nColumn>=BMS-1 ) continue;
          if( mustBeUnique ){
            if( pIdx->nKeyCol>nExpr
             ||(pIdx->nColumn>nExpr && !IsUniqueIndex(pIdx))
            ){
              continue;  /* This index is not unique over the IN RHS columns */
            }
          }
 
          colUsed = 0;   /* Columns of index used so far */
          for(i=0; i<nExpr; i++){
            Expr *pLhs = capdbVectorFieldSubexpr(pX->pLeft, i);
            Expr *pRhs = pEList->a[i].pExpr;
            CollSeq *pReq = capdbBinaryCompareCollSeq(pParse, pLhs, pRhs);
            int j;
 
            for(j=0; j<nExpr; j++){
              if( pIdx->aiColumn[j]!=pRhs->iColumn ) continue;
              assert( pIdx->azColl[j] );
              if( pReq!=0 && capdbStrICmp(pReq->zName, pIdx->azColl[j])!=0 ){
                continue;
              }
              break;
            }
            if( j==nExpr ) break;
            mCol = MASKBIT(j);
            if( mCol & colUsed ) break; /* Each column used only once */
            colUsed |= mCol;
            if( aiMap ) aiMap[i] = j;
          }
 
          assert( nExpr>0 && nExpr<BMS );
          assert( i==nExpr || colUsed!=(MASKBIT(nExpr)-1) );
          if( colUsed==(MASKBIT(nExpr)-1) ){
            /* If we reach this point, that means the index pIdx is usable */
            int iAddr = capdbVdbeAddOp0(v, OP_Once); VdbeCoverage(v);
            ExplainQueryPlan((pParse, 0,
                              "USING INDEX %s FOR IN-OPERATOR",pIdx->zName));
            capdbVdbeAddOp3(v, OP_OpenRead, iTab, pIdx->tnum, iDb);
            capdbVdbeSetP4KeyInfo(pParse, pIdx);
            VdbeComment((v, "%s", pIdx->zName));
            assert( IN_INDEX_INDEX_DESC == IN_INDEX_INDEX_ASC+1 );
            eType = IN_INDEX_INDEX_ASC + pIdx->aSortOrder[0];
 
            if( prRhsHasNull ){
#ifdef CAPDB_ENABLE_COLUMN_USED_MASK
              i64 mask = (1<<nExpr)-1;
              capdbVdbeAddOp4Dup8(v, OP_ColumnsUsed,
                  iTab, 0, 0, (u8*)&mask, P4_INT64);
#endif
              *prRhsHasNull = ++pParse->nMem;
              if( nExpr==1 ){
                capdbSetHasNullFlag(v, iTab, *prRhsHasNull);
              }
            }
            capdbVdbeJumpHere(v, iAddr);
          }
        } /* End loop over indexes */
      } /* End if( affinity_ok ) */
    } /* End if not an rowid index */
  } /* End attempt to optimize using an index */

  /* If no preexisting index is available for the IN clause
  ** and IN_INDEX_NOOP is an allowed reply
  ** and the RHS of the IN operator is a list, not a subquery
  ** and the RHS is not constant or has two or fewer terms,
  ** then it is not worth creating an ephemeral table to evaluate
  ** the IN operator so return IN_INDEX_NOOP.
  */
  if( eType==0
   && (inFlags & IN_INDEX_NOOP_OK)
   && ExprUseXList(pX)
   && (!capdbInRhsIsConstant(pParse,pX) || pX->x.pList->nExpr<=2)
  ){
    pParse->nTab--;  /* Back out the allocation of the unused cursor */
    iTab = -1;       /* Cursor is not allocated */
    eType = IN_INDEX_NOOP;
  }

  if( eType==0 ){
    /* Could not find an existing table or index to use as the RHS b-tree.
    ** We will have to generate an ephemeral table to do the job.
    */
    u32 savedNQueryLoop = pParse->nQueryLoop;
    int rMayHaveNull = 0;
    int bloomOk = (inFlags & IN_INDEX_MEMBERSHIP)!=0;
    eType = IN_INDEX_EPH;
    if( inFlags & IN_INDEX_LOOP ){
      pParse->nQueryLoop = 0;
    }else if( prRhsHasNull ){
      *prRhsHasNull = rMayHaveNull = ++pParse->nMem;
    }
    assert( pX->op==TK_IN );
    if( !bloomOk
     && ExprUseXSelect(pX)
     && (pX->x.pSelect->selFlags & SF_ClonedRhsIn)!=0
    ){
      bloomOk = 1;
    }
    capdbCodeRhsOfIN(pParse, pX, iTab, bloomOk);
    if( rMayHaveNull ){
      capdbSetHasNullFlag(v, iTab, rMayHaveNull);
    }
    pParse->nQueryLoop = savedNQueryLoop;
  }

  if( aiMap && eType!=IN_INDEX_INDEX_ASC && eType!=IN_INDEX_INDEX_DESC ){
    int i, n;
    n = capdbExprVectorSize(pX->pLeft);
    for(i=0; i<n; i++) aiMap[i] = i;
  }
  *piTab = iTab;
  return eType;
}
#endif

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Argument pExpr is an (?, ?...) IN(...) expression. This
** function allocates and returns a nul-terminated string containing
** the affinities to be used for each column of the comparison.
**
** It is the responsibility of the caller to ensure that the returned
** string is eventually freed using capdbDbFree().
*/
static char *exprINAffinity(Parse *pParse, const Expr *pExpr){
  Expr *pLeft = pExpr->pLeft;
  int nVal = capdbExprVectorSize(pLeft);
  Select *pSelect = ExprUseXSelect(pExpr) ? pExpr->x.pSelect : 0;
  char *zRet;

  assert( pExpr->op==TK_IN );
  zRet = capdbDbMallocRaw(pParse->db, 1+(i64)nVal);
  if( zRet ){
    int i;
    for(i=0; i<nVal; i++){
      Expr *pA = capdbVectorFieldSubexpr(pLeft, i);
      char a = capdbExprAffinity(pA);
      if( pSelect ){
        zRet[i] = capdbCompareAffinity(pSelect->pEList->a[i].pExpr, a);
      }else{
        zRet[i] = a;
      }
    }
    zRet[nVal] = '\0';
  }
  return zRet;
}
#endif

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Load the Parse object passed as the first argument with an error
** message of the form:
**
**   "sub-select returns N columns - expected M"
*/  
void capdbSubselectError(Parse *pParse, int nActual, int nExpect){
  if( pParse->nErr==0 ){
    const char *zFmt = "sub-select returns %d columns - expected %d";
    capdbErrorMsg(pParse, zFmt, nActual, nExpect);
  }
}
#endif

/*
** Expression pExpr is a vector that has been used in a context where
** it is not permitted. If pExpr is a sub-select vector, this routine
** loads the Parse object with a message of the form:
**
**   "sub-select returns N columns - expected 1"
**
** Or, if it is a regular scalar vector:
**
**   "row value misused"
*/  
void capdbVectorErrorMsg(Parse *pParse, Expr *pExpr){
#ifndef CAPDB_OMIT_SUBQUERY
  if( ExprUseXSelect(pExpr) ){
    capdbSubselectError(pParse, pExpr->x.pSelect->pEList->nExpr, 1);
  }else
#endif
  {
    capdbErrorMsg(pParse, "row value misused");
  }
}

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Scan all previously generated bytecode looking for an OP_BeginSubrtn
** that is compatible with pExpr.  If found, add the y.sub values
** to pExpr and return true.  If not found, return false.
*/
static int findCompatibleInRhsSubrtn(
  Parse *pParse,          /* Parsing context */
  Expr *pExpr,            /* IN operator with RHS that we want to reuse */
  SubrtnSig *pNewSig      /* Signature for the IN operator */
){
  VdbeOp *pOp, *pEnd;
  SubrtnSig *pSig;
  Vdbe *v;

  if( pNewSig==0 ) return 0;
  if( (pParse->mSubrtnSig & (1<<(pNewSig->selId&7)))==0 ) return 0;
  assert( pExpr->op==TK_IN );
  assert( !ExprUseYSub(pExpr) );
  assert( ExprUseXSelect(pExpr) );
  assert( pExpr->x.pSelect!=0 );
  assert( (pExpr->x.pSelect->selFlags & SF_All)==0 );
  v = pParse->pVdbe;
  assert( v!=0 );
  pOp = capdbVdbeGetOp(v, 1);
  pEnd = capdbVdbeGetLastOp(v);
  for(; pOp<pEnd; pOp++){
    if( pOp->p4type!=P4_SUBRTNSIG ) continue;
    assert( pOp->opcode==OP_BeginSubrtn );
    pSig = pOp->p4.pSubrtnSig;
    assert( pSig!=0 );
    if( !pSig->bComplete ) continue;
    if( pNewSig->selId!=pSig->selId ) continue;
    if( strcmp(pNewSig->zAff,pSig->zAff)!=0 ) continue;
    pExpr->y.sub.iAddr = pSig->iAddr;
    pExpr->y.sub.regReturn = pSig->regReturn;
    pExpr->iTable = pSig->iTable;
    ExprSetProperty(pExpr, EP_Subrtn);
    return 1;
  }
  return 0;
}
#endif /* CAPDB_OMIT_SUBQUERY */

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Generate code that will construct an ephemeral table containing all terms
** in the RHS of an IN operator.  The IN operator can be in either of two
** forms:
**
**     x IN (4,5,11)              -- IN operator with list on right-hand side
**     x IN (SELECT a FROM b)     -- IN operator with subquery on the right
**
** The pExpr parameter is the IN operator.  The cursor number for the
** constructed ephemeral table is returned.  The first time the ephemeral
** table is computed, the cursor number is also stored in pExpr->iTable,
** however the cursor number returned might not be the same, as it might
** have been duplicated using OP_OpenDup.
**
** If the LHS expression ("x" in the examples) is a column value, or
** the SELECT statement returns a column value, then the affinity of that
** column is used to build the index keys. If both 'x' and the
** SELECT... statement are columns, then numeric affinity is used
** if either column has NUMERIC or INTEGER affinity. If neither
** 'x' nor the SELECT... statement are columns, then numeric affinity
** is used.
*/
void capdbCodeRhsOfIN(
  Parse *pParse,          /* Parsing context */
  Expr *pExpr,            /* The IN operator */
  int iTab,               /* Use this cursor number */
  int allowBloom          /* True to allow the use of a Bloom filter */
){
  int addrOnce = 0;           /* Address of the OP_Once instruction at top */
  int addr;                   /* Address of OP_OpenEphemeral instruction */
  Expr *pLeft;                /* the LHS of the IN operator */
  KeyInfo *pKeyInfo = 0;      /* Key information */
  int nVal;                   /* Size of vector pLeft */
  Vdbe *v;                    /* The prepared statement under construction */
  SubrtnSig *pSig = 0;        /* Signature for this subroutine */

  v = pParse->pVdbe;
  assert( v!=0 );

  /* The evaluation of the IN must be repeated every time it
  ** is encountered if any of the following is true:
  **
  **    *  The right-hand side is a correlated subquery
  **    *  The right-hand side is an expression list containing variables
  **    *  We are inside a trigger
  **
  ** If all of the above are false, then we can compute the RHS just once
  ** and reuse it many names.
  */
  if( !ExprHasProperty(pExpr, EP_VarSelect) && pParse->iSelfTab==0 ){
    /* Reuse of the RHS is allowed
    **
    ** Compute a signature for the RHS of the IN operator to facility
    ** finding and reusing prior instances of the same IN operator.
    */
    assert( !ExprUseXSelect(pExpr) || pExpr->x.pSelect!=0 );
    if( ExprUseXSelect(pExpr) && (pExpr->x.pSelect->selFlags & SF_All)==0 ){
      pSig = capdbDbMallocRawNN(pParse->db, sizeof(pSig[0]));
      if( pSig ){
        pSig->selId = pExpr->x.pSelect->selId;
        pSig->zAff = exprINAffinity(pParse, pExpr);
      }
    }

    /* Check to see if there is a prior materialization of the RHS of
    ** this IN operator.  If there is, then make use of that prior
    ** materialization rather than recomputing it.
    */
    if( ExprHasProperty(pExpr, EP_Subrtn) 
     || findCompatibleInRhsSubrtn(pParse, pExpr, pSig)
    ){
      addrOnce = capdbVdbeAddOp0(v, OP_Once); VdbeCoverage(v);
      if( ExprUseXSelect(pExpr) ){
        ExplainQueryPlan((pParse, 0, "REUSE LIST SUBQUERY %d",
              pExpr->x.pSelect->selId));
      }
      assert( ExprUseYSub(pExpr) );
      capdbVdbeAddOp2(v, OP_Gosub, pExpr->y.sub.regReturn,
                        pExpr->y.sub.iAddr);
      assert( iTab!=pExpr->iTable );
      capdbVdbeAddOp2(v, OP_OpenDup, iTab, pExpr->iTable);
      capdbVdbeJumpHere(v, addrOnce);
      if( pSig ){
        capdbDbFree(pParse->db, pSig->zAff);
        capdbDbFree(pParse->db, pSig);
      }
      return;
    }

    /* Begin coding the subroutine */
    assert( !ExprUseYWin(pExpr) );
    ExprSetProperty(pExpr, EP_Subrtn);
    assert( !ExprHasProperty(pExpr, EP_TokenOnly|EP_Reduced) );
    pExpr->y.sub.regReturn = ++pParse->nMem;
    pExpr->y.sub.iAddr =
      capdbVdbeAddOp2(v, OP_BeginSubrtn, 0, pExpr->y.sub.regReturn) + 1;
    if( pSig ){
      pSig->bComplete = 0;
      pSig->iAddr = pExpr->y.sub.iAddr;
      pSig->regReturn = pExpr->y.sub.regReturn;
      pSig->iTable = iTab;
      pParse->mSubrtnSig = 1 << (pSig->selId&7);
      capdbVdbeChangeP4(v, -1, (const char*)pSig, P4_SUBRTNSIG);
    }
    addrOnce = capdbVdbeAddOp0(v, OP_Once); VdbeCoverage(v);
  }

  /* Check to see if this is a vector IN operator */
  pLeft = pExpr->pLeft;
  nVal = capdbExprVectorSize(pLeft);

  /* Construct the ephemeral table that will contain the content of
  ** RHS of the IN operator.
  */
  pExpr->iTable = iTab;
  addr = capdbVdbeAddOp2(v, OP_OpenEphemeral, pExpr->iTable, nVal);
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
  if( ExprUseXSelect(pExpr) ){
    VdbeComment((v, "Result of SELECT %u", pExpr->x.pSelect->selId));
  }else{
    VdbeComment((v, "RHS of IN operator"));
  }
#endif
  pKeyInfo = capdbKeyInfoAlloc(pParse->db, nVal, 1);

  if( ExprUseXSelect(pExpr) ){
    /* Case 1:     expr IN (SELECT ...)
    **
    ** Generate code to write the results of the select into the temporary
    ** table allocated and opened above.
    */
    Select *pSelect = pExpr->x.pSelect;
    ExprList *pEList = pSelect->pEList;

    ExplainQueryPlan((pParse, 1, "%sLIST SUBQUERY %d",
        addrOnce?"":"CORRELATED ", pSelect->selId
    ));
    /* If the LHS and RHS of the IN operator do not match, that
    ** error will have been caught long before we reach this point. */
    if( ALWAYS(pEList->nExpr==nVal) ){
      Select *pCopy;
      SelectDest dest;
      int i;
      int rc;
      int addrBloom = 0;
      capdbSelectDestInit(&dest, SRT_Set, iTab);
      dest.zAffSdst = exprINAffinity(pParse, pExpr);
      pSelect->iLimit = 0;
      if( addrOnce
       && allowBloom
       && OptimizationEnabled(pParse->db, CAPDB_BloomFilter)
      ){
        int regBloom = ++pParse->nMem;
        addrBloom = capdbVdbeAddOp2(v, OP_Blob, 10000, regBloom);
        VdbeComment((v, "Bloom filter"));
        dest.iSDParm2 = regBloom;
      }
      testcase( pSelect->selFlags & SF_Distinct );
      testcase( pKeyInfo==0 ); /* Caused by OOM in capdbKeyInfoAlloc() */
      pCopy = capdbSelectDup(pParse->db, pSelect, 0);
      rc = pParse->db->mallocFailed ? 1 :capdbSelect(pParse, pCopy, &dest);
      capdbSelectDelete(pParse->db, pCopy);
      capdbDbFree(pParse->db, dest.zAffSdst);
      if( addrBloom ){
        /* Remember that location of the Bloom filter in the P3 operand
        ** of the OP_Once that began this subroutine. tag-202407032019 */
        capdbVdbeGetOp(v, addrOnce)->p3 = dest.iSDParm2;
        if( dest.iSDParm2==0 ){
          /* If the Bloom filter won't actually be used, keep it small */
          capdbVdbeGetOp(v, addrBloom)->p1 = 10;
        }
      }
      if( rc ){
        capdbKeyInfoUnref(pKeyInfo);
        return;
      }
      assert( pKeyInfo!=0 ); /* OOM will cause exit after capdbSelect() */
      assert( pEList!=0 );
      assert( pEList->nExpr>0 );
      assert( capdbKeyInfoIsWriteable(pKeyInfo) );
      for(i=0; i<nVal; i++){
        Expr *p = capdbVectorFieldSubexpr(pLeft, i);
        pKeyInfo->aColl[i] = capdbBinaryCompareCollSeq(
            pParse, p, pEList->a[i].pExpr
        );
      }
    }
  }else if( ALWAYS(pExpr->x.pList!=0) ){
    /* Case 2:     expr IN (exprlist)
    **
    ** For each expression, build an index key from the evaluation and
    ** store it in the temporary table. If <expr> is a column, then use
    ** that columns affinity when building index keys. If <expr> is not
    ** a column, use numeric affinity.
    */
    char affinity;            /* Affinity of the LHS of the IN */
    int i;
    ExprList *pList = pExpr->x.pList;
    struct ExprList_item *pItem;
    int r1, r2;
    affinity = capdbExprAffinity(pLeft);
    if( affinity<=CAPDB_AFF_NONE ){
      affinity = CAPDB_AFF_BLOB;
    }else if( affinity==CAPDB_AFF_REAL ){
      affinity = CAPDB_AFF_NUMERIC;
    }
    if( pKeyInfo ){
      assert( capdbKeyInfoIsWriteable(pKeyInfo) );
      pKeyInfo->aColl[0] = capdbExprCollSeq(pParse, pExpr->pLeft);
    }

    /* Loop through each expression in <exprlist>. */
    r1 = capdbGetTempReg(pParse);
    r2 = capdbGetTempReg(pParse);
    for(i=pList->nExpr, pItem=pList->a; i>0; i--, pItem++){
      Expr *pE2 = pItem->pExpr;

      /* If the expression is not constant then we will need to
      ** disable the test that was generated above that makes sure
      ** this code only executes once.  Because for a non-constant
      ** expression we need to rerun this code each time.
      */
      if( addrOnce && !capdbExprIsConstant(pParse, pE2) ){
        capdbVdbeChangeToNoop(v, addrOnce-1);
        capdbVdbeChangeToNoop(v, addrOnce);
        ExprClearProperty(pExpr, EP_Subrtn);
        addrOnce = 0;
      }

      /* Evaluate the expression and insert it into the temp table */
      capdbExprCode(pParse, pE2, r1);
      capdbVdbeAddOp4(v, OP_MakeRecord, r1, 1, r2, &affinity, 1);
      capdbVdbeAddOp4Int(v, OP_IdxInsert, iTab, r2, r1, 1);
    }
    capdbReleaseTempReg(pParse, r1);
    capdbReleaseTempReg(pParse, r2);
  }
  if( pSig ) pSig->bComplete = 1;
  if( pKeyInfo ){
    capdbVdbeChangeP4(v, addr, (void *)pKeyInfo, P4_KEYINFO);
  }
  if( addrOnce ){
    capdbVdbeAddOp1(v, OP_NullRow, iTab);
    capdbVdbeJumpHere(v, addrOnce);
    /* Subroutine return */
    assert( ExprUseYSub(pExpr) );
    assert( capdbVdbeGetOp(v,pExpr->y.sub.iAddr-1)->opcode==OP_BeginSubrtn
            || pParse->nErr );
    capdbVdbeAddOp3(v, OP_Return, pExpr->y.sub.regReturn,
                      pExpr->y.sub.iAddr, 1);
    VdbeCoverage(v);
    capdbClearTempRegCache(pParse);
  }
}
#endif /* CAPDB_OMIT_SUBQUERY */

/*
** Generate code for scalar subqueries used as a subquery expression
** or EXISTS operator:
**
**     (SELECT a FROM b)          -- subquery
**     EXISTS (SELECT a FROM b)   -- EXISTS subquery
**
** The pExpr parameter is the SELECT or EXISTS operator to be coded.
**
** Return the register that holds the result.  For a multi-column SELECT,
** the result is stored in a contiguous array of registers and the
** return value is the register of the left-most result column.
** Return 0 if an error occurs.
*/
#ifndef CAPDB_OMIT_SUBQUERY
int capdbCodeSubselect(Parse *pParse, Expr *pExpr){
  int addrOnce = 0;           /* Address of OP_Once at top of subroutine */
  int rReg = 0;               /* Register storing resulting */
  Select *pSel;               /* SELECT statement to encode */
  SelectDest dest;            /* How to deal with SELECT result */
  int nReg;                   /* Registers to allocate */
  Expr *pLimit;               /* New limit expression */
#ifdef CAPDB_ENABLE_STMT_SCANSTATUS
  int addrExplain;            /* Address of OP_Explain instruction */
#endif

  Vdbe *v = pParse->pVdbe;
  assert( v!=0 );
  if( pParse->nErr ) return 0;
  testcase( pExpr->op==TK_EXISTS );
  testcase( pExpr->op==TK_SELECT );
  assert( pExpr->op==TK_EXISTS || pExpr->op==TK_SELECT );
  assert( ExprUseXSelect(pExpr) );
  pSel = pExpr->x.pSelect;

  /* If this routine has already been coded, then invoke it as a
  ** subroutine. */
  if( ExprHasProperty(pExpr, EP_Subrtn) ){
    ExplainQueryPlan((pParse, 0, "REUSE SUBQUERY %d", pSel->selId));
    assert( ExprUseYSub(pExpr) );
    capdbVdbeAddOp2(v, OP_Gosub, pExpr->y.sub.regReturn,
                      pExpr->y.sub.iAddr);
    return pExpr->iTable;
  }

  /* Begin coding the subroutine */
  assert( !ExprUseYWin(pExpr) );
  assert( !ExprHasProperty(pExpr, EP_Reduced|EP_TokenOnly) );
  ExprSetProperty(pExpr, EP_Subrtn);
  pExpr->y.sub.regReturn = ++pParse->nMem;
  pExpr->y.sub.iAddr =
    capdbVdbeAddOp2(v, OP_BeginSubrtn, 0, pExpr->y.sub.regReturn) + 1;

  /* The evaluation of the EXISTS/SELECT must be repeated every time it
  ** is encountered if any of the following is true:
  **
  **    *  The right-hand side is a correlated subquery
  **    *  The right-hand side is an expression list containing variables
  **    *  We are inside a trigger
  **
  ** If all of the above are false, then we can run this code just once
  ** save the results, and reuse the same result on subsequent invocations.
  */
  if( !ExprHasProperty(pExpr, EP_VarSelect) ){
    addrOnce = capdbVdbeAddOp0(v, OP_Once); VdbeCoverage(v);
  }
 
  /* For a SELECT, generate code to put the values for all columns of
  ** the first row into an array of registers and return the index of
  ** the first register.
  **
  ** If this is an EXISTS, write an integer 0 (not exists) or 1 (exists)
  ** into a register and return that register number.
  **
  ** In both cases, the query is augmented with "LIMIT 1".  Any
  ** preexisting limit is discarded in place of the new LIMIT 1.
  */
  ExplainQueryPlan2(addrExplain, (pParse, 1, "%sSCALAR SUBQUERY %d",
        addrOnce?"":"CORRELATED ", pSel->selId));
  capdbVdbeScanStatusCounters(v, addrExplain, addrExplain, -1);
  nReg = pExpr->op==TK_SELECT ? pSel->pEList->nExpr : 1;
  capdbSelectDestInit(&dest, 0, pParse->nMem+1);
  pParse->nMem += nReg;
  if( pExpr->op==TK_SELECT ){
    dest.eDest = SRT_Mem;
    if( (pSel->selFlags&SF_Distinct) && pSel->pLimit && pSel->pLimit->pRight ){
      /* If there is both a DISTINCT and an OFFSET clause, then allocate
      ** a separate dest.iSdst array for capdbSelect() and other
      ** routines to populate. In this case results will be copied over
      ** into the dest.iSDParm array only after OFFSET processing. This
      ** ensures that in the case where OFFSET excludes all rows, the
      ** dest.iSDParm array is not left populated with the contents of the
      ** last row visited - it should be all NULLs if all rows were
      ** excluded by OFFSET.  */ 
      dest.iSdst = pParse->nMem+1;
      pParse->nMem += nReg;
    }else{
      dest.iSdst = dest.iSDParm;
    }
    dest.nSdst = nReg;
    capdbVdbeAddOp3(v, OP_Null, 0, dest.iSDParm, pParse->nMem);
    VdbeComment((v, "Init subquery result"));
  }else{
    dest.eDest = SRT_Exists;
    capdbVdbeAddOp2(v, OP_Integer, 0, dest.iSDParm);
    VdbeComment((v, "Init EXISTS result"));
  }
  if( pSel->pLimit ){
    /* The subquery already has a limit.  If the pre-existing limit X is 
    ** not already integer value 1 or 0, then make the new limit X<>0 so that
    ** the new limit is either 1 or 0 */
    Expr *pLeft = pSel->pLimit->pLeft;
    if( ExprHasProperty(pLeft, EP_IntValue)==0
     || (pLeft->u.iValue!=1 && pLeft->u.iValue!=0)
    ){
      capdb *db = pParse->db;
      pLimit = capdbExprInt32(db, 0);
      if( pLimit ){
        pLimit->affExpr = CAPDB_AFF_NUMERIC;
        pLimit = capdbPExpr(pParse, TK_NE,
            capdbExprDup(db, pLeft, 0), pLimit);
      }
      capdbExprDeferredDelete(pParse, pLeft);
      pSel->pLimit->pLeft = pLimit;
    }
  }else{
    /* If there is no pre-existing limit add a limit of 1 */
    pLimit = capdbExprInt32(pParse->db, 1);
    pSel->pLimit = capdbPExpr(pParse, TK_LIMIT, pLimit, 0);
  }
  pSel->iLimit = 0;
  if( capdbSelect(pParse, pSel, &dest) ){
    pExpr->op2 = pExpr->op;
    pExpr->op = TK_ERROR;
    return 0;
  }
  pExpr->iTable = rReg = dest.iSDParm;
  ExprSetVVAProperty(pExpr, EP_NoReduce);
  if( addrOnce ){
    capdbVdbeJumpHere(v, addrOnce);
  }
  capdbVdbeScanStatusRange(v, addrExplain, addrExplain, -1);

  /* Subroutine return */
  assert( ExprUseYSub(pExpr) );
  assert( capdbVdbeGetOp(v,pExpr->y.sub.iAddr-1)->opcode==OP_BeginSubrtn
          || pParse->nErr );
  capdbVdbeAddOp3(v, OP_Return, pExpr->y.sub.regReturn,
                    pExpr->y.sub.iAddr, 1);
  VdbeCoverage(v);
  capdbClearTempRegCache(pParse);
  return rReg;
}
#endif /* CAPDB_OMIT_SUBQUERY */

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Expr pIn is an IN(...) expression. This function checks that the
** sub-select on the RHS of the IN() operator has the same number of
** columns as the vector on the LHS. Or, if the RHS of the IN() is not
** a sub-query, that the LHS is a vector of size 1.
*/
int capdbExprCheckIN(Parse *pParse, Expr *pIn){
  int nVector = capdbExprVectorSize(pIn->pLeft);
  if( ExprUseXSelect(pIn) && !pParse->db->mallocFailed ){
    if( nVector!=pIn->x.pSelect->pEList->nExpr ){
      capdbSubselectError(pParse, pIn->x.pSelect->pEList->nExpr, nVector);
      return 1;
    }
  }else if( nVector!=1 ){
    capdbVectorErrorMsg(pParse, pIn->pLeft);
    return 1;
  }
  return 0;
}
#endif

#ifndef CAPDB_OMIT_SUBQUERY
/*
** Generate code for an IN expression.
**
**      x IN (SELECT ...)
**      x IN (value, value, ...)
**
** The left-hand side (LHS) is a scalar or vector expression.  The
** right-hand side (RHS) is an array of zero or more scalar values, or a
** subquery.  If the RHS is a subquery, the number of result columns must
** match the number of columns in the vector on the LHS.  If the RHS is
** a list of values, the LHS must be a scalar.
**
** The IN operator is true if the LHS value is contained within the RHS.
** The result is false if the LHS is definitely not in the RHS.  The
** result is NULL if the presence of the LHS in the RHS cannot be
** determined due to NULLs.
**
** This routine generates code that jumps to destIfFalse if the LHS is not
** contained within the RHS.  If due to NULLs we cannot determine if the LHS
** is contained in the RHS then jump to destIfNull.  If the LHS is contained
** within the RHS then fall through.
**
** See the separate in-operator.md documentation file in the canonical
** SQLite source tree for additional information.
*/
static void capdbExprCodeIN(
  Parse *pParse,        /* Parsing and code generating context */
  Expr *pExpr,          /* The IN expression */
  int destIfFalse,      /* Jump here if LHS is not contained in the RHS */
  int destIfNull        /* Jump here if the results are unknown due to NULLs */
){
  int rRhsHasNull = 0;  /* Register that is true if RHS contains NULL values */
  int eType;            /* Type of the RHS */
  int rLhs;             /* Register(s) holding the LHS values */
  Vdbe *v;              /* Statement under construction */
  int *aiMap = 0;       /* Map from vector field to index column */
  char *zAff = 0;       /* Affinity string for comparisons */
  int nVector;          /* Size of vectors for this IN operator */
  int iDummy;           /* Dummy parameter to exprCodeVector() */
  Expr *pLeft;          /* The LHS of the IN operator */
  int i;                /* loop counter */
  int destStep2;        /* Where to jump when NULLs seen in step 2 */
  int destStep6 = 0;    /* Start of code for Step 6 */
  int addrTruthOp;      /* Address of opcode that determines the IN is true */
  int destNotNull;      /* Jump here if a comparison is not true in step 6 */
  int addrTop;          /* Top of the step-6 loop */
  int iTab = 0;         /* Index to use */
  u8 okConstFactor = pParse->okConstFactor;

  assert( !ExprHasVVAProperty(pExpr,EP_Immutable) );
  pLeft = pExpr->pLeft;
  if( capdbExprCheckIN(pParse, pExpr) ) return;
  zAff = exprINAffinity(pParse, pExpr);
  nVector = capdbExprVectorSize(pExpr->pLeft);
  aiMap = (int*)capdbDbMallocZero(pParse->db, nVector*sizeof(int));
  if( pParse->db->mallocFailed ) goto capdbExprCodeIN_oom_error;

  /* Attempt to compute the RHS. After this step, if anything other than
  ** IN_INDEX_NOOP is returned, the table opened with cursor iTab
  ** contains the values that make up the RHS. If IN_INDEX_NOOP is returned,
  ** the RHS has not yet been coded.  */
  v = pParse->pVdbe;
  assert( v!=0 );       /* OOM detected prior to this routine */
  VdbeNoopComment((v, "begin IN expr"));
  eType = capdbFindInIndex(pParse, pExpr,
                             IN_INDEX_MEMBERSHIP | IN_INDEX_NOOP_OK,
                             destIfFalse==destIfNull ? 0 : &rRhsHasNull,
                             aiMap, &iTab);

  assert( pParse->nErr || nVector==1 || eType==IN_INDEX_EPH
       || eType==IN_INDEX_INDEX_ASC || eType==IN_INDEX_INDEX_DESC
  );
#ifdef CAPDB_DEBUG
  /* Confirm that aiMap[] contains nVector integer values between 0 and
  ** nVector-1. */
  for(i=0; i<nVector; i++){
    int j, cnt;
    for(cnt=j=0; j<nVector; j++) if( aiMap[j]==i ) cnt++;
    assert( cnt==1 );
  }
#endif

  /* Code the LHS, the <expr> from "<expr> IN (...)". If the LHS is a
  ** vector, then it is stored in an array of nVector registers starting
  ** at r1.
  **
  ** capdbFindInIndex() might have reordered the fields of the LHS vector
  ** so that the fields are in the same order as an existing index.   The
  ** aiMap[] array contains a mapping from the original LHS field order to
  ** the field order that matches the RHS index.
  **
  ** Avoid factoring the LHS of the IN(...) expression out of the loop,
  ** even if it is constant, as OP_Affinity may be used on the register
  ** by code generated below.  */
  assert( pParse->okConstFactor==okConstFactor );
  pParse->okConstFactor = 0;
  rLhs = exprCodeVector(pParse, pLeft, &iDummy);
  pParse->okConstFactor = okConstFactor;

  /* If capdbFindInIndex() did not find or create an index that is
  ** suitable for evaluating the IN operator, then evaluate using a
  ** sequence of comparisons.
  **
  ** This is step (1) in the in-operator.md optimized algorithm.
  */
  if( eType==IN_INDEX_NOOP ){
    ExprList *pList;
    CollSeq *pColl;
    int labelOk = capdbVdbeMakeLabel(pParse);
    int r2, regToFree;
    int regCkNull = 0;
    int ii;
    assert( nVector==1 );
    assert( ExprUseXList(pExpr) );
    pList = pExpr->x.pList;
    pColl = capdbExprCollSeq(pParse, pExpr->pLeft);
    if( destIfNull!=destIfFalse ){
      regCkNull = capdbGetTempReg(pParse);
      capdbVdbeAddOp3(v, OP_BitAnd, rLhs, rLhs, regCkNull);
    }
    for(ii=0; ii<pList->nExpr; ii++){
      r2 = capdbExprCodeTemp(pParse, pList->a[ii].pExpr, &regToFree);
      if( regCkNull && capdbExprCanBeNull(pList->a[ii].pExpr) ){
        capdbVdbeAddOp3(v, OP_BitAnd, regCkNull, r2, regCkNull);
      }
      capdbReleaseTempReg(pParse, regToFree);
      if( ii<pList->nExpr-1 || destIfNull!=destIfFalse ){
        int op = rLhs!=r2 ? OP_Eq : OP_NotNull;
        capdbVdbeAddOp4(v, op, rLhs, labelOk, r2,
                          (void*)pColl, P4_COLLSEQ);
        VdbeCoverageIf(v, ii<pList->nExpr-1 && op==OP_Eq);
        VdbeCoverageIf(v, ii==pList->nExpr-1 && op==OP_Eq);
        VdbeCoverageIf(v, ii<pList->nExpr-1 && op==OP_NotNull);
        VdbeCoverageIf(v, ii==pList->nExpr-1 && op==OP_NotNull);
        capdbVdbeChangeP5(v, zAff[0]);
      }else{
        int op = rLhs!=r2 ? OP_Ne : OP_IsNull;
        assert( destIfNull==destIfFalse );
        capdbVdbeAddOp4(v, op, rLhs, destIfFalse, r2,
                          (void*)pColl, P4_COLLSEQ);
        VdbeCoverageIf(v, op==OP_Ne);
        VdbeCoverageIf(v, op==OP_IsNull);
        capdbVdbeChangeP5(v, zAff[0] | CAPDB_JUMPIFNULL);
      }
    }
    if( regCkNull ){
      capdbVdbeAddOp2(v, OP_IsNull, regCkNull, destIfNull); VdbeCoverage(v);
      capdbVdbeGoto(v, destIfFalse);
    }
    capdbVdbeResolveLabel(v, labelOk);
    capdbReleaseTempReg(pParse, regCkNull);
    goto capdbExprCodeIN_finished;
  }

  if( eType!=IN_INDEX_ROWID ){
    /* If this IN operator will use an index, then the order of columns in the
    ** vector might be different from the order in the index.  In that case,
    ** we need to reorder the LHS values to be in index order.  Run Affinity
    ** before reordering the columns, so that the affinity is correct.
    */
    capdbVdbeAddOp4(v, OP_Affinity, rLhs, nVector, 0, zAff, nVector);
    for(i=0; i<nVector && aiMap[i]==i; i++){} /* Are LHS fields reordered? */
    if( i!=nVector ){
      /* Need to reorder the LHS fields according to aiMap */
      int rLhsOrig = rLhs;
      rLhs = capdbGetTempRange(pParse, nVector);
      for(i=0; i<nVector; i++){
        testcase( aiMap[i]!=i );
        capdbVdbeAddOp3(v, OP_Copy, rLhsOrig+i, rLhs+aiMap[i], 0);
      }
      capdbReleaseTempReg(pParse, rLhsOrig);
    }
  }


  /* Step 2: Check to see if the LHS contains any NULL columns.  If the
  ** LHS does contain NULLs then the result must be either FALSE or NULL.
  ** We will then skip the binary search of the RHS.
  */
  if( destIfNull==destIfFalse ){
    destStep2 = destIfFalse;
  }else{
    destStep2 = destStep6 = capdbVdbeMakeLabel(pParse);
  }
  for(i=0; i<nVector; i++){
    Expr *p = capdbVectorFieldSubexpr(pExpr->pLeft, i);
    if( pParse->nErr ) goto capdbExprCodeIN_oom_error;
    if( capdbExprCanBeNull(p) ){
      testcase( aiMap[i]!=i );
      capdbVdbeAddOp2(v, OP_IsNull, rLhs+aiMap[i], destStep2);
      VdbeCoverage(v);
    }
  }

  /* Step 3.  The LHS is now known to be non-NULL.  Do the binary search
  ** of the RHS using the LHS as a probe.  If found, the result is
  ** true.
  */
  if( eType==IN_INDEX_ROWID ){
    /* In this case, the RHS is the ROWID of table b-tree and so we also
    ** know that the RHS is non-NULL.  Hence, we combine steps 3 and 4
    ** into a single opcode. */
    assert( nVector==1 );
    capdbVdbeAddOp3(v, OP_SeekRowid, iTab, destIfFalse, rLhs);
    VdbeCoverage(v);
    addrTruthOp = capdbVdbeAddOp0(v, OP_Goto);  /* Return True */
  }else{
    if( destIfFalse==destIfNull ){
      /* Combine Step 3 and Step 5 into a single opcode */
      if( ExprHasProperty(pExpr, EP_Subrtn) ){
        const VdbeOp *pOp = capdbVdbeGetOp(v, pExpr->y.sub.iAddr);
        assert( pOp->opcode==OP_Once || pParse->nErr );
        if( pOp->p3>0 ){  /* tag-202407032019 */
          assert( OptimizationEnabled(pParse->db, CAPDB_BloomFilter)
                 || pParse->nErr );
          capdbVdbeAddOp4Int(v, OP_Filter, pOp->p3, destIfFalse,
                               rLhs, nVector); VdbeCoverage(v);
        }
      }
      capdbVdbeAddOp4Int(v, OP_NotFound, iTab, destIfFalse,
                           rLhs, nVector); VdbeCoverage(v);
      goto capdbExprCodeIN_finished;
    }
    /* Ordinary Step 3, for the case where FALSE and NULL are distinct */
    addrTruthOp = capdbVdbeAddOp4Int(v, OP_Found, iTab, 0,
                                      rLhs, nVector); VdbeCoverage(v);
  }

  /* Step 4.  If the RHS is known to be non-NULL and we did not find
  ** an match on the search above, then the result must be FALSE.
  */
  if( rRhsHasNull && nVector==1 ){
    capdbVdbeAddOp2(v, OP_NotNull, rRhsHasNull, destIfFalse);
    VdbeCoverage(v);
  }

  /* Step 5.  If we do not care about the difference between NULL and
  ** FALSE, then just return false.
  */
  if( destIfFalse==destIfNull ) capdbVdbeGoto(v, destIfFalse);

  /* Step 6: Loop through rows of the RHS.  Compare each row to the LHS.
  ** If any comparison is NULL, then the result is NULL.  If all
  ** comparisons are FALSE then the final result is FALSE.
  **
  ** For a scalar LHS, it is sufficient to check just the first row
  ** of the RHS.
  */
  if( destStep6 ) capdbVdbeResolveLabel(v, destStep6);
  addrTop = capdbVdbeAddOp2(v, OP_Rewind, iTab, destIfFalse);
  VdbeCoverage(v);
  if( nVector>1 ){
    destNotNull = capdbVdbeMakeLabel(pParse);
  }else{
    /* For nVector==1, combine steps 6 and 7 by immediately returning
    ** FALSE if the first comparison is not NULL */
    destNotNull = destIfFalse;
  }
  for(i=0; i<nVector; i++){
    Expr *p;
    CollSeq *pColl;
    int r3 = capdbGetTempReg(pParse);
    p = capdbVectorFieldSubexpr(pLeft, i);
    if( ExprUseXSelect(pExpr) ){
      Expr *pRhs = pExpr->x.pSelect->pEList->a[i].pExpr;
      pColl = capdbBinaryCompareCollSeq(pParse, p, pRhs);
    }else{
      /* If the RHS of the IN(...) expression are scalar expressions, do
      ** not consider their collation sequences. The documentation says
      ** "The collating sequence used for expressions of the form "x IN (y, z,
      ** ...)" is the collating sequence of x.".  */
      pColl = capdbExprCollSeq(pParse, p);
    }
    testcase( aiMap[i]!=i );
    capdbVdbeAddOp3(v, OP_Column, iTab, aiMap[i], r3);
    capdbVdbeAddOp4(v, OP_Ne, rLhs+aiMap[i], destNotNull, r3,
                      (void*)pColl, P4_COLLSEQ);
    VdbeCoverage(v);
    capdbReleaseTempReg(pParse, r3);
  }
  capdbVdbeAddOp2(v, OP_Goto, 0, destIfNull);
  if( nVector>1 ){
    capdbVdbeResolveLabel(v, destNotNull);
    capdbVdbeAddOp2(v, OP_Next, iTab, addrTop+1);
    VdbeCoverage(v);

    /* Step 7:  If we reach this point, we know that the result must
    ** be false. */
    capdbVdbeAddOp2(v, OP_Goto, 0, destIfFalse);
  }

  /* Jumps here in order to return true. */
  capdbVdbeJumpHere(v, addrTruthOp);

capdbExprCodeIN_finished:
  VdbeComment((v, "end IN expr"));
capdbExprCodeIN_oom_error:
  capdbDbFree(pParse->db, aiMap);
  capdbDbFree(pParse->db, zAff);
}
#endif /* CAPDB_OMIT_SUBQUERY */

#ifndef CAPDB_OMIT_FLOATING_POINT
/*
** Generate an instruction that will put the floating point
** value described by z[0..n-1] into register iMem.
**
** The z[] string will probably not be zero-terminated.  But the
** z[n] character is guaranteed to be something that does not look
** like the continuation of the number.
*/
static void codeReal(Vdbe *v, const char *z, int negateFlag, int iMem){
  if( ALWAYS(z!=0) ){
    double value;
    capdbAtoF(z, &value);
    assert( !capdbIsNaN(value) ); /* The new AtoF never returns NaN */
    if( negateFlag ) value = -value;
    capdbVdbeAddOp4Dup8(v, OP_Real, 0, iMem, 0, (u8*)&value, P4_REAL);
  }
}
#endif


/*
** Generate an instruction that will put the integer describe by
** text z[0..n-1] into register iMem.
**
** Expr.u.zToken is always UTF8 and zero-terminated.
*/
static void codeInteger(Parse *pParse, Expr *pExpr, int negFlag, int iMem){
  Vdbe *v = pParse->pVdbe;
  if( pExpr->flags & EP_IntValue ){
    int i = pExpr->u.iValue;
    assert( i>=0 );
    if( negFlag ) i = -i;
    capdbVdbeAddOp2(v, OP_Integer, i, iMem);
  }else{
    int c;
    i64 value;
    const char *z = pExpr->u.zToken;
    assert( z!=0 );
    c = capdbDecOrHexToI64(z, &value);
    if( (c==3 && !negFlag) || (c==2) || (negFlag && value==SMALLEST_INT64)){
#ifdef CAPDB_OMIT_FLOATING_POINT
      capdbErrorMsg(pParse, "oversized integer: %s%#T", negFlag?"-":"",pExpr);
#else
#ifndef CAPDB_OMIT_HEX_INTEGER
      if( capdb_strnicmp(z,"0x",2)==0 ){
        capdbErrorMsg(pParse, "hex literal too big: %s%#T",
                        negFlag?"-":"",pExpr);
      }else
#endif
      {
        codeReal(v, z, negFlag, iMem);
      }
#endif
    }else{
      if( negFlag ){ value = c==3 ? SMALLEST_INT64 : -value; }
      capdbVdbeAddOp4Dup8(v, OP_Int64, 0, iMem, 0, (u8*)&value, P4_INT64);
    }
  }
}


/* Generate code that will load into register regOut a value that is
** appropriate for the iIdxCol-th column of index pIdx.
*/
void capdbExprCodeLoadIndexColumn(
  Parse *pParse,  /* The parsing context */
  Index *pIdx,    /* The index whose column is to be loaded */
  int iTabCur,    /* Cursor pointing to a table row */
  int iIdxCol,    /* The column of the index to be loaded */
  int regOut      /* Store the index column value in this register */
){
  i16 iTabCol = pIdx->aiColumn[iIdxCol];
  if( iTabCol==XN_EXPR ){
    assert( pIdx->aColExpr );
    assert( pIdx->aColExpr->nExpr>iIdxCol );
    pParse->iSelfTab = iTabCur + 1;
    capdbExprCodeCopy(pParse, pIdx->aColExpr->a[iIdxCol].pExpr, regOut);
    pParse->iSelfTab = 0;
  }else{
    capdbExprCodeGetColumnOfTable(pParse->pVdbe, pIdx->pTable, iTabCur,
                                    iTabCol, regOut);
  }
}

#ifndef CAPDB_OMIT_GENERATED_COLUMNS
/*
** Generate code that will compute the value of generated column pCol
** and store the result in register regOut
*/
void capdbExprCodeGeneratedColumn(
  Parse *pParse,     /* Parsing context */
  Table *pTab,       /* Table containing the generated column */
  Column *pCol,      /* The generated column */
  int regOut         /* Put the result in this register */
){
  int iAddr;
  Vdbe *v = pParse->pVdbe;
  int nErr = pParse->nErr;
  assert( v!=0 );
  assert( pParse->iSelfTab!=0 );
  if( pParse->iSelfTab>0 ){
    iAddr = capdbVdbeAddOp3(v, OP_IfNullRow, pParse->iSelfTab-1, 0, regOut);
  }else{
    iAddr = 0;
  }
  capdbExprCodeCopy(pParse, capdbColumnExpr(pTab,pCol), regOut);
  if( (pCol->colFlags & COLFLAG_VIRTUAL)!=0
   && (pTab->tabFlags & TF_Strict)!=0
  ){
    int p3 = 2+(int)(pCol - pTab->aCol);
    capdbVdbeAddOp4(v, OP_TypeCheck, regOut, 1, p3, (char*)pTab, P4_TABLE);
  }else if( pCol->affinity>=CAPDB_AFF_TEXT ){
    capdbVdbeAddOp4(v, OP_Affinity, regOut, 1, 0, &pCol->affinity, 1);
  }
  if( iAddr ) capdbVdbeJumpHere(v, iAddr);
  if( pParse->nErr>nErr ) pParse->db->errByteOffset = -1;
}
#endif /* CAPDB_OMIT_GENERATED_COLUMNS */

/*
** Generate code to extract the value of the iCol-th column of a table.
*/
void capdbExprCodeGetColumnOfTable(
  Vdbe *v,        /* Parsing context */
  Table *pTab,    /* The table containing the value */
  int iTabCur,    /* The table cursor.  Or the PK cursor for WITHOUT ROWID */
  int iCol,       /* Index of the column to extract */
  int regOut      /* Extract the value into this register */
){
  Column *pCol;
  assert( v!=0 );
  assert( pTab!=0 );
  assert( iCol!=XN_EXPR );
  if( iCol<0 || iCol==pTab->iPKey ){
    capdbVdbeAddOp2(v, OP_Rowid, iTabCur, regOut);
    VdbeComment((v, "%s.rowid", pTab->zName));
  }else{
    int op;
    int x;
    if( IsVirtual(pTab) ){
      op = OP_VColumn;
      x = iCol;
#ifndef CAPDB_OMIT_GENERATED_COLUMNS
    }else if( (pCol = &pTab->aCol[iCol])->colFlags & COLFLAG_VIRTUAL ){
      Parse *pParse = capdbVdbeParser(v);
      if( pCol->colFlags & COLFLAG_BUSY ){
        capdbErrorMsg(pParse, "generated column loop on \"%s\"",
                        pCol->zCnName);
      }else{
        int savedSelfTab = pParse->iSelfTab;
        pCol->colFlags |= COLFLAG_BUSY;
        pParse->iSelfTab = iTabCur+1;
        capdbExprCodeGeneratedColumn(pParse, pTab, pCol, regOut);
        pParse->iSelfTab = savedSelfTab;
        pCol->colFlags &= ~COLFLAG_BUSY;
      }
      return;
#endif
    }else if( !HasRowid(pTab) ){
      testcase( iCol!=capdbTableColumnToStorage(pTab, iCol) );
      x = capdbTableColumnToIndex(capdbPrimaryKeyIndex(pTab), iCol);
      op = OP_Column;
    }else{
      x = capdbTableColumnToStorage(pTab,iCol);
      testcase( x!=iCol );
      op = OP_Column;
    }
    capdbVdbeAddOp3(v, op, iTabCur, x, regOut);
    capdbColumnDefault(v, pTab, iCol, regOut);
  }
}

/*
** Generate code that will extract the iColumn-th column from
** table pTab and store the column value in register iReg.
**
** There must be an open cursor to pTab in iTable when this routine
** is called.  If iColumn<0 then code is generated that extracts the rowid.
*/
int capdbExprCodeGetColumn(
  Parse *pParse,   /* Parsing and code generating context */
  Table *pTab,     /* Description of the table we are reading from */
  int iColumn,     /* Index of the table column */
  int iTable,      /* The cursor pointing to the table */
  int iReg,        /* Store results here */
  u8 p5            /* P5 value for OP_Column + FLAGS */
){
  assert( pParse->pVdbe!=0 );
  assert( (p5 & (OPFLAG_NOCHNG|OPFLAG_TYPEOFARG|OPFLAG_LENGTHARG))==p5 );
  assert( IsVirtual(pTab) || (p5 & OPFLAG_NOCHNG)==0 );
  capdbExprCodeGetColumnOfTable(pParse->pVdbe, pTab, iTable, iColumn, iReg);
  if( p5 ){
    VdbeOp *pOp = capdbVdbeGetLastOp(pParse->pVdbe);
    if( pOp->opcode==OP_Column ) pOp->p5 = p5;
    if( pOp->opcode==OP_VColumn ) pOp->p5 = (p5 & OPFLAG_NOCHNG);
  }
  return iReg;
}

/*
** Generate code to move content from registers iFrom...iFrom+nReg-1
** over to iTo..iTo+nReg-1.
*/
void capdbExprCodeMove(Parse *pParse, int iFrom, int iTo, int nReg){
  capdbVdbeAddOp3(pParse->pVdbe, OP_Move, iFrom, iTo, nReg);
}

/*
** Convert a scalar expression node to a TK_REGISTER referencing
** register iReg.  The caller must ensure that iReg already contains
** the correct value for the expression.
*/
void capdbExprToRegister(Expr *pExpr, int iReg){
  Expr *p = capdbExprSkipCollateAndLikely(pExpr);
  if( NEVER(p==0) ) return;
  if( p->op==TK_REGISTER ){
    assert( p->iTable==iReg );
  }else{
    p->op2 = p->op;
    p->op = TK_REGISTER;
    p->iTable = iReg;
    ExprClearProperty(p, EP_Skip);
  }
}

/*
** Evaluate an expression (either a vector or a scalar expression) and store
** the result in contiguous temporary registers.  Return the index of
** the first register used to store the result.
**
** If the returned result register is a temporary scalar, then also write
** that register number into *piFreeable.  If the returned result register
** is not a temporary or if the expression is a vector set *piFreeable
** to 0.
*/
static int exprCodeVector(Parse *pParse, Expr *p, int *piFreeable){
  int iResult;
  int nResult = capdbExprVectorSize(p);
  if( nResult==1 ){
    iResult = capdbExprCodeTemp(pParse, p, piFreeable);
  }else{
    *piFreeable = 0;
    if( p->op==TK_SELECT ){
#if CAPDB_OMIT_SUBQUERY
      iResult = 0;
#else
      iResult = capdbCodeSubselect(pParse, p);
#endif
    }else{
      int i;
      iResult = pParse->nMem+1;
      pParse->nMem += nResult;
      assert( ExprUseXList(p) );
      for(i=0; i<nResult; i++){
        capdbExprCodeFactorable(pParse, p->x.pList->a[i].pExpr, i+iResult);
      }
    }
  }
  return iResult;
}

/*
** If the last opcode is a OP_Copy, then set the do-not-merge flag (p5)
** so that a subsequent copy will not be merged into this one.
*/
static void setDoNotMergeFlagOnCopy(Vdbe *v){
  if( capdbVdbeGetLastOp(v)->opcode==OP_Copy ){
    capdbVdbeChangeP5(v, 1);  /* Tag trailing OP_Copy as not mergeable */
  }
}

/*
** Generate code to implement special SQL functions that are implemented
** in-line rather than by using the usual callbacks.
*/
static int exprCodeInlineFunction(
  Parse *pParse,        /* Parsing context */
  ExprList *pFarg,      /* List of function arguments */
  int iFuncId,          /* Function ID.  One of the INTFUNC_... values */
  int target            /* Store function result in this register */
){
  int nFarg;
  Vdbe *v = pParse->pVdbe;
  assert( v!=0 );
  assert( pFarg!=0 );
  nFarg = pFarg->nExpr;
  assert( nFarg>0 );  /* All in-line functions have at least one argument */
  switch( iFuncId ){
    case INLINEFUNC_coalesce: {
      /* Attempt a direct implementation of the built-in COALESCE() and
      ** IFNULL() functions.  This avoids unnecessary evaluation of
      ** arguments past the first non-NULL argument.
      */
      int endCoalesce = capdbVdbeMakeLabel(pParse);
      int i;
      assert( nFarg>=2 );
      capdbExprCode(pParse, pFarg->a[0].pExpr, target);
      for(i=1; i<nFarg; i++){
        capdbVdbeAddOp2(v, OP_NotNull, target, endCoalesce);
        VdbeCoverage(v);
        capdbExprCode(pParse, pFarg->a[i].pExpr, target);
      }
      setDoNotMergeFlagOnCopy(v);
      capdbVdbeResolveLabel(v, endCoalesce);
      break;
    }
    case INLINEFUNC_iif: {
      Expr caseExpr;
      memset(&caseExpr, 0, sizeof(caseExpr));
      caseExpr.op = TK_CASE;
      caseExpr.x.pList = pFarg;
      return capdbExprCodeTarget(pParse, &caseExpr, target);
    }
#ifdef CAPDB_ENABLE_OFFSET_SQL_FUNC
    case INLINEFUNC_sqlite_offset: {
      Expr *pArg = pFarg->a[0].pExpr;
      if( pArg->op==TK_COLUMN && pArg->iTable>=0 ){
        capdbVdbeAddOp3(v, OP_Offset, pArg->iTable, pArg->iColumn, target);
      }else{
        capdbVdbeAddOp2(v, OP_Null, 0, target);
      }
      break;
    }
#endif
    default: {  
      /* The UNLIKELY() function is a no-op.  The result is the value
      ** of the first argument.
      */
      assert( nFarg==1 || nFarg==2 );
      target = capdbExprCodeTarget(pParse, pFarg->a[0].pExpr, target);
      break;
    }

  /***********************************************************************
  ** Test-only SQL functions that are only usable if enabled
  ** via CAPDB_TESTCTRL_INTERNAL_FUNCTIONS
  */
#if !defined(CAPDB_UNTESTABLE)
    case INLINEFUNC_expr_compare: {
      /* Compare two expressions using capdbExprCompare() */
      assert( nFarg==2 );
      capdbVdbeAddOp2(v, OP_Integer,
         capdbExprCompare(0,pFarg->a[0].pExpr, pFarg->a[1].pExpr,-1),
         target);
      break;
    }

    case INLINEFUNC_expr_implies_expr: {
      /* Compare two expressions using capdbExprImpliesExpr() */
      assert( nFarg==2 );
      capdbVdbeAddOp2(v, OP_Integer,
         capdbExprImpliesExpr(pParse,pFarg->a[0].pExpr, pFarg->a[1].pExpr,-1),
         target);
      break;
    }

    case INLINEFUNC_implies_nonnull_row: {
      /* Result of capdbExprImpliesNonNullRow() */
      Expr *pA1;
      assert( nFarg==2 );
      pA1 = pFarg->a[1].pExpr;
      if( pA1->op==TK_COLUMN ){
        capdbVdbeAddOp2(v, OP_Integer,
           capdbExprImpliesNonNullRow(pFarg->a[0].pExpr,pA1->iTable,1),
           target);
      }else{
        capdbVdbeAddOp2(v, OP_Null, 0, target);
      }
      break;
    }

    case INLINEFUNC_affinity: {
      /* The AFFINITY() function evaluates to a string that describes
      ** the type affinity of the argument.  This is used for testing of
      ** the SQLite type logic.
      */
      const char *azAff[] = { "blob", "text", "numeric", "integer",
                              "real", "flexnum" };
      char aff;
      assert( nFarg==1 );
      aff = capdbExprAffinity(pFarg->a[0].pExpr);
      assert( aff<=CAPDB_AFF_NONE
           || (aff>=CAPDB_AFF_BLOB && aff<=CAPDB_AFF_FLEXNUM) );
      capdbVdbeLoadString(v, target,
              (aff<=CAPDB_AFF_NONE) ? "none" : azAff[aff-CAPDB_AFF_BLOB]);
      break;
    }
#endif /* !defined(CAPDB_UNTESTABLE) */
  }
  return target;
}

/*
** Expression Node callback for capdbExprCanReturnSubtype().  If
** pExpr is able to return a subtype, set pWalker->eCode and abort
** the search.  If pExpr can never return a subtype, prune search.
**
** The only expressions that can return a subtype are:
**
**    1.  A function
**    2.  The no-op "+" operator
**    3.  A CASE...END expression
**    4.  A CAST() expression
**    5.  A "expr COLLATE colseq" expression.
**
** For any other kind of expression, prune the search.
**
** For case 1, the expression can yield a subtype if the function has
** the CAPDB_RESULT_SUBTYPE property.  Functions can also return
** a subtype (via capdb_result_value()) if any of the arguments can
** return a subtype.
**
** In all cases 1 through 5, the expression might also return a subtype
** if any operand can return a subtype.
*/
static int exprNodeCanReturnSubtype(Walker *pWalker, Expr *pExpr){
  int n;
  FuncDef *pDef;
  capdb *db;
  if( pExpr->op==TK_CASE || pExpr->op==TK_UPLUS 
   || pExpr->op==TK_COLLATE || pExpr->op==TK_CAST 
  ){
    return WRC_Continue;
  }
  if( pExpr->op!=TK_FUNCTION ){
    return WRC_Prune;
  }
  assert( ExprUseXList(pExpr) );
  db = pWalker->pParse->db;
  n = ALWAYS(pExpr->x.pList) ? pExpr->x.pList->nExpr : 0;
  pDef = capdbFindFunction(db, pExpr->u.zToken, n, ENC(db), 0);
  if( NEVER(pDef==0) || (pDef->funcFlags & CAPDB_RESULT_SUBTYPE)!=0 ){
    pWalker->eCode = 1;
    return WRC_Abort;
  }
  return WRC_Continue;
}

/*
** Return TRUE if expression pExpr is able to return a subtype.
**
** A TRUE return does not guarantee that a subtype will be returned.
** It only indicates that a subtype return is possible.  False positives
** are acceptable as they only disable an optimization.  False negatives,
** on the other hand, can lead to incorrect answers.
*/
static int capdbExprCanReturnSubtype(Parse *pParse, Expr *pExpr){
  Walker w;
  memset(&w, 0, sizeof(w));
  w.pParse = pParse;
  w.xExprCallback = exprNodeCanReturnSubtype;
  capdbWalkExpr(&w, pExpr);
  return w.eCode;
}


/*
** Check to see if pExpr is one of the indexed expressions on pParse->pIdxEpr.
** If it is, then resolve the expression by reading from the index and
** return the register into which the value has been read.  If pExpr is
** not an indexed expression, then return negative.
*/
static CAPDB_NOINLINE int capdbIndexedExprLookup(
  Parse *pParse,   /* The parsing context */
  Expr *pExpr,     /* The expression to potentially bypass */
  int target       /* Where to store the result of the expression */
){
  IndexedExpr *p;
  Vdbe *v;
  for(p=pParse->pIdxEpr; p; p=p->pIENext){
    u8 exprAff;
    int iDataCur = p->iDataCur;
    if( iDataCur<0 ) continue;
    if( pParse->iSelfTab ){
      if( p->iDataCur!=pParse->iSelfTab-1 ) continue;
      iDataCur = -1;
    }
    if( capdbExprCompare(0, pExpr, p->pExpr, iDataCur)!=0 ) continue;
    assert( p->aff>=CAPDB_AFF_BLOB && p->aff<=CAPDB_AFF_NUMERIC );
    exprAff = capdbExprAffinity(pExpr);
    if( (exprAff<=CAPDB_AFF_BLOB && p->aff!=CAPDB_AFF_BLOB)
     || (exprAff==CAPDB_AFF_TEXT && p->aff!=CAPDB_AFF_TEXT)
     || (exprAff>=CAPDB_AFF_NUMERIC && p->aff!=CAPDB_AFF_NUMERIC)
    ){
      /* Affinity mismatch on a generated column */
      continue;
    }


    /* Functions that might set a subtype should not be replaced by the
    ** value taken from an expression index if they are themselves an
    ** argument to another scalar function or aggregate. 
    ** https://sqlite.org/forum/forumpost/68d284c86b082c3e */
    if( ExprHasProperty(pExpr, EP_SubtArg)
     && capdbExprCanReturnSubtype(pParse, pExpr) 
    ){
      continue;
    }

    v = pParse->pVdbe;
    assert( v!=0 );
    if( p->bMaybeNullRow ){
      /* If the index is on a NULL row due to an outer join, then we
      ** cannot extract the value from the index.  The value must be
      ** computed using the original expression. */
      int addr = capdbVdbeCurrentAddr(v);
      capdbVdbeAddOp3(v, OP_IfNullRow, p->iIdxCur, addr+3, target);
      VdbeCoverage(v);
      capdbVdbeAddOp3(v, OP_Column, p->iIdxCur, p->iIdxCol, target);
      VdbeComment((v, "%s expr-column %d", p->zIdxName, p->iIdxCol));
      capdbVdbeGoto(v, 0);
      p = pParse->pIdxEpr;
      pParse->pIdxEpr = 0;
      capdbExprCode(pParse, pExpr, target);
      pParse->pIdxEpr = p;
      capdbVdbeJumpHere(v, addr+2);
    }else{
      capdbVdbeAddOp3(v, OP_Column, p->iIdxCur, p->iIdxCol, target);
      VdbeComment((v, "%s expr-column %d", p->zIdxName, p->iIdxCol));
    }
    return target;
  }
  return -1;  /* Not found */
}


/*
** Expression pExpr is guaranteed to be a TK_COLUMN or equivalent. This
** function checks the Parse.pIdxPartExpr list to see if this column
** can be replaced with a constant value. If so, it generates code to
** put the constant value in a register (ideally, but not necessarily, 
** register iTarget) and returns the register number.
**
** Or, if the TK_COLUMN cannot be replaced by a constant, zero is 
** returned.
*/
static int exprPartidxExprLookup(Parse *pParse, Expr *pExpr, int iTarget){
  IndexedExpr *p;
  for(p=pParse->pIdxPartExpr; p; p=p->pIENext){
    if( pExpr->iColumn==p->iIdxCol && pExpr->iTable==p->iDataCur ){
      Vdbe *v = pParse->pVdbe;
      int addr = 0;
      int ret;

      if( p->bMaybeNullRow ){
        addr = capdbVdbeAddOp1(v, OP_IfNullRow, p->iIdxCur);
      }
      ret = capdbExprCodeTarget(pParse, p->pExpr, iTarget);
      capdbVdbeAddOp4(pParse->pVdbe, OP_Affinity, ret, 1, 0,
                        (const char*)&p->aff, 1);
      if( addr ){
        capdbVdbeJumpHere(v, addr);
        capdbVdbeChangeP3(v, addr, ret);
      }
      return ret;
    }
  }
  return 0;
}

/*
** Generate code that evaluates an AND or OR operator leaving a
** boolean result in a register.  pExpr is the AND/OR expression.
** Store the result in the "target" register.  Use short-circuit
** evaluation to avoid computing both operands, if possible.
**
** The code generated might require the use of a temporary register.
** If it does, then write the number of that temporary register
** into *pTmpReg.  If not, leave *pTmpReg unchanged.
*/
static CAPDB_NOINLINE int exprCodeTargetAndOr(
  Parse *pParse,     /* Parsing context */
  Expr *pExpr,       /* AND or OR expression to be coded */
  int target,        /* Put result in this register, guaranteed */
  int *pTmpReg       /* Write a temporary register here */
){
  int op;            /* The opcode.  TK_AND or TK_OR */
  int skipOp;        /* Opcode for the branch that skips one operand */
  int addrSkip;      /* Branch instruction that skips one of the operands */
  int regSS = 0;     /* Register holding computed operand when other omitted */
  int r1, r2;        /* Registers for left and right operands, respectively */
  Expr *pAlt;        /* Alternative, simplified expression */
  Vdbe *v;           /* statement being coded */

  assert( pExpr!=0 );
  op = pExpr->op;
  assert( op==TK_AND || op==TK_OR );
  assert( TK_AND==OP_And );            testcase( op==TK_AND );
  assert( TK_OR==OP_Or );              testcase( op==TK_OR );
  assert( pParse->pVdbe!=0 );
  v = pParse->pVdbe;
  pAlt = capdbExprSimplifiedAndOr(pExpr);
  if( pAlt!=pExpr ){
    r1 = capdbExprCodeTarget(pParse, pAlt, target);
    capdbVdbeAddOp3(v, OP_And, r1, r1, target);
    return target;
  }
  skipOp = op==TK_AND ? OP_IfNot : OP_If;
  if( exprEvalRhsFirst(pExpr) ){
    /* Compute the right operand first.  Skip the computation of the left
    ** operand if the right operand fully determines the result */
    r2 = regSS = capdbExprCodeTarget(pParse, pExpr->pRight, target);
    addrSkip = capdbVdbeAddOp1(v, skipOp, r2);
    VdbeComment((v, "skip left operand"));
    VdbeCoverage(v); 
    r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, pTmpReg);
  }else{
    /* Compute the left operand first */
    r1 = capdbExprCodeTarget(pParse, pExpr->pLeft, target);
    if( ExprHasProperty(pExpr->pRight, EP_Subquery) ){
      /* Skip over the computation of the right operand if the right
      ** operand is a subquery and the left operand completely determines
      ** the result */
      regSS = r1;
      addrSkip = capdbVdbeAddOp1(v, skipOp, r1);
      VdbeComment((v, "skip right operand"));
      VdbeCoverage(v);
    }else{
      addrSkip = regSS = 0;
    }
    r2 = capdbExprCodeTemp(pParse, pExpr->pRight, pTmpReg);
  }
  capdbVdbeAddOp3(v, op, r2, r1, target);
  testcase( (*pTmpReg)==0 );
  if( addrSkip ){
    capdbVdbeAddOp2(v, OP_Goto, 0, capdbVdbeCurrentAddr(v)+2);
    capdbVdbeJumpHere(v, addrSkip);
    capdbVdbeAddOp3(v, OP_Or, regSS, regSS, target);
    VdbeComment((v, "short-circut value"));
  }
  return target;
}



/*
** Generate code into the current Vdbe to evaluate the given
** expression.  Attempt to store the results in register "target".
** Return the register where results are stored.
**
** With this routine, there is no guarantee that results will
** be stored in target.  The result might be stored in some other
** register if it is convenient to do so.  The calling function
** must check the return code and move the results to the desired
** register.
*/
int capdbExprCodeTarget(Parse *pParse, Expr *pExpr, int target){
  Vdbe *v = pParse->pVdbe;  /* The VM under construction */
  int op;                   /* The opcode being coded */
  int inReg = target;       /* Results stored in register inReg */
  int regFree1 = 0;         /* If non-zero free this temporary register */
  int regFree2 = 0;         /* If non-zero free this temporary register */
  int r1, r2;               /* Various register numbers */
  Expr tempX;               /* Temporary expression node */
  int p5 = 0;

  assert( target>0 && target<=pParse->nMem );
  assert( v!=0 );

expr_code_doover:
  if( pExpr==0 ){
    op = TK_NULL;
  }else if( pParse->pIdxEpr!=0
   && !ExprHasProperty(pExpr, EP_Leaf)
   && (r1 = capdbIndexedExprLookup(pParse, pExpr, target))>=0
  ){
    return r1;
  }else{
    assert( !ExprHasVVAProperty(pExpr,EP_Immutable) );
    op = pExpr->op;
  }
  assert( op!=TK_ORDER );
  switch( op ){
    case TK_AGG_COLUMN: {
      AggInfo *pAggInfo = pExpr->pAggInfo;
      struct AggInfo_col *pCol;
      assert( pAggInfo!=0 );
      assert( pExpr->iAgg>=0 );
      if( pExpr->iAgg>=pAggInfo->nColumn ){
        /* Happens when the left table of a RIGHT JOIN is null and
        ** is using an expression index */
        capdbVdbeAddOp2(v, OP_Null, 0, target);
#ifdef CAPDB_VDBE_COVERAGE
        /* Verify that the OP_Null above is exercised by tests
        ** tag-20230325-2 */
        capdbVdbeAddOp3(v, OP_NotNull, target, 1, 20230325);
        VdbeCoverageNeverTaken(v);
#endif
        break;
      }
      pCol = &pAggInfo->aCol[pExpr->iAgg];
      if( !pAggInfo->directMode ){
        return AggInfoColumnReg(pAggInfo, pExpr->iAgg);
      }else if( pAggInfo->useSortingIdx ){
        Table *pTab = pCol->pTab;
        capdbVdbeAddOp3(v, OP_Column, pAggInfo->sortingIdxPTab,
                              pCol->iSorterColumn, target);
        if( pTab==0 ){
          /* No comment added */
        }else if( pCol->iColumn<0 ){
          VdbeComment((v,"%s.rowid",pTab->zName));
        }else{
          VdbeComment((v,"%s.%s",
              pTab->zName, pTab->aCol[pCol->iColumn].zCnName));
          if( pTab->aCol[pCol->iColumn].affinity==CAPDB_AFF_REAL ){
            capdbVdbeAddOp1(v, OP_RealAffinity, target);
          }
        }
        return target;
      }else if( pExpr->y.pTab==0 ){
        /* This case happens when the argument to an aggregate function
        ** is rewritten by aggregateConvertIndexedExprRefToColumn() */
        capdbVdbeAddOp3(v, OP_Column, pExpr->iTable, pExpr->iColumn, target);
        return target;
      }
      /* Otherwise, fall thru into the TK_COLUMN case */
      /* no break */ deliberate_fall_through
    }
    case TK_COLUMN: {
      int iTab = pExpr->iTable;
      int iReg;
      if( ExprHasProperty(pExpr, EP_FixedCol) ){
        /* This COLUMN expression is really a constant due to WHERE clause
        ** constraints, and that constant is coded by the pExpr->pLeft
        ** expression.  However, make sure the constant has the correct
        ** datatype by applying the Affinity of the table column to the
        ** constant.
        */
        int aff;
        iReg = capdbExprCodeTarget(pParse, pExpr->pLeft,target);
        assert( ExprUseYTab(pExpr) );
        assert( pExpr->y.pTab!=0 );
        aff = capdbTableColumnAffinity(pExpr->y.pTab, pExpr->iColumn);
        if( aff>CAPDB_AFF_BLOB ){
          static const char zAff[] = "B\000C\000D\000E\000F";
          assert( CAPDB_AFF_BLOB=='A' );
          assert( CAPDB_AFF_TEXT=='B' );
          capdbVdbeAddOp4(v, OP_Affinity, iReg, 1, 0,
                            &zAff[(aff-'B')*2], P4_STATIC);
        }
        return iReg;
      }
      if( iTab<0 ){
        if( pParse->iSelfTab<0 ){
          /* Other columns in the same row for CHECK constraints or
          ** generated columns or for inserting into partial index.
          ** The row is unpacked into registers beginning at
          ** 0-(pParse->iSelfTab).  The rowid (if any) is in a register
          ** immediately prior to the first column.
          */
          Column *pCol;
          Table *pTab;
          int iSrc;
          int iCol = pExpr->iColumn;
          assert( ExprUseYTab(pExpr) );
          pTab = pExpr->y.pTab;
          assert( pTab!=0 );
          assert( iCol>=XN_ROWID );
          assert( iCol<pTab->nCol );
          if( iCol<0 ){
            return -1-pParse->iSelfTab;
          }
          pCol = pTab->aCol + iCol;
          testcase( iCol!=capdbTableColumnToStorage(pTab,iCol) );
          iSrc = capdbTableColumnToStorage(pTab, iCol) - pParse->iSelfTab;
#ifndef CAPDB_OMIT_GENERATED_COLUMNS
          if( pCol->colFlags & COLFLAG_GENERATED ){
            if( pCol->colFlags & COLFLAG_BUSY ){
              capdbErrorMsg(pParse, "generated column loop on \"%s\"",
                              pCol->zCnName);
              return 0;
            }
            pCol->colFlags |= COLFLAG_BUSY;
            if( pCol->colFlags & COLFLAG_NOTAVAIL ){
              capdbExprCodeGeneratedColumn(pParse, pTab, pCol, iSrc);
            }
            pCol->colFlags &= ~(COLFLAG_BUSY|COLFLAG_NOTAVAIL);
            return iSrc;
          }else
#endif /* CAPDB_OMIT_GENERATED_COLUMNS */
          if( pCol->affinity==CAPDB_AFF_REAL ){
            capdbVdbeAddOp2(v, OP_SCopy, iSrc, target);
            capdbVdbeAddOp1(v, OP_RealAffinity, target);
            return target;
          }else{
            return iSrc;
          }
        }else{
          /* Coding an expression that is part of an index where column names
          ** in the index refer to the table to which the index belongs */
          iTab = pParse->iSelfTab - 1;
        }
      }
      else if( pParse->pIdxPartExpr 
       && 0!=(r1 = exprPartidxExprLookup(pParse, pExpr, target))
      ){
        return r1;
      }
      assert( ExprUseYTab(pExpr) );
      assert( pExpr->y.pTab!=0 );
      iReg = capdbExprCodeGetColumn(pParse, pExpr->y.pTab,
                               pExpr->iColumn, iTab, target,
                               pExpr->op2);
      return iReg;
    }
    case TK_INTEGER: {
      codeInteger(pParse, pExpr, 0, target);
      return target;
    }
    case TK_TRUEFALSE: {
      capdbVdbeAddOp2(v, OP_Integer, capdbExprTruthValue(pExpr), target);
      return target;
    }
#ifndef CAPDB_OMIT_FLOATING_POINT
    case TK_FLOAT: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      codeReal(v, pExpr->u.zToken, 0, target);
      return target;
    }
#endif
    case TK_STRING: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbVdbeLoadString(v, target, pExpr->u.zToken);
      return target;
    }
    case TK_NULLS: {
      /* Set a range of registers to NULL.  pExpr->y.nReg registers starting
      ** with target */
      capdbVdbeAddOp3(v, OP_Null, 0, target, target + pExpr->y.nReg - 1);
      return target;
    }
    default: {
      /* Make NULL the default case so that if a bug causes an illegal
      ** Expr node to be passed into this function, it will be handled
      ** sanely and not crash.  But keep the assert() to bring the problem
      ** to the attention of the developers. */
      assert( op==TK_NULL || op==TK_ERROR || pParse->db->mallocFailed );
      capdbVdbeAddOp2(v, OP_Null, 0, target);
      return target;
    }
#ifndef CAPDB_OMIT_BLOB_LITERAL
    case TK_BLOB: {
      int n;
      const char *z;
      char *zBlob;
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      assert( pExpr->u.zToken[0]=='x' || pExpr->u.zToken[0]=='X' );
      assert( pExpr->u.zToken[1]=='\'' );
      z = &pExpr->u.zToken[2];
      n = capdbStrlen30(z) - 1;
      assert( z[n]=='\'' );
      zBlob = capdbHexToBlob(capdbVdbeDb(v), z, n);
      capdbVdbeAddOp4(v, OP_Blob, n/2, target, 0, zBlob, P4_DYNAMIC);
      return target;
    }
#endif
    case TK_VARIABLE: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      assert( pExpr->u.zToken!=0 );
      assert( pExpr->u.zToken[0]!=0 );
      capdbVdbeAddOp2(v, OP_Variable, pExpr->iColumn, target);
      return target;
    }
    case TK_REGISTER: {
      return pExpr->iTable;
    }
#ifndef CAPDB_OMIT_CAST
    case TK_CAST: {
      /* Expressions of the form:   CAST(pLeft AS token) */
      capdbExprCode(pParse, pExpr->pLeft, target);
      assert( inReg==target );
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbVdbeAddOp2(v, OP_Cast, target,
                        capdbAffinityType(pExpr->u.zToken, 0));
      return inReg;
    }
#endif /* CAPDB_OMIT_CAST */
    case TK_IS:
    case TK_ISNOT:
      op = (op==TK_IS) ? TK_EQ : TK_NE;
      p5 = CAPDB_NULLEQ;
      /* no break */ deliberate_fall_through
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_NE:
    case TK_EQ: {
      Expr *pLeft = pExpr->pLeft;
      int addrIsNull = 0;
      if( capdbExprIsVector(pLeft) ){
        codeVectorCompare(pParse, pExpr, target, op, p5);
      }else{
        if( ExprHasProperty(pExpr, EP_Subquery) && p5!=CAPDB_NULLEQ ){
          addrIsNull = exprComputeOperands(pParse, pExpr,
                                     &r1, &r2, &regFree1, &regFree2);
        }else{
          r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
          r2 = capdbExprCodeTemp(pParse, pExpr->pRight, &regFree2);
        }
        capdbVdbeAddOp2(v, OP_Integer, 1, inReg);
        codeCompare(pParse, pLeft, pExpr->pRight, op, r1, r2,
            capdbVdbeCurrentAddr(v)+2, p5,
            ExprHasProperty(pExpr,EP_Commuted));
        assert(TK_LT==OP_Lt); testcase(op==OP_Lt); VdbeCoverageIf(v,op==OP_Lt);
        assert(TK_LE==OP_Le); testcase(op==OP_Le); VdbeCoverageIf(v,op==OP_Le);
        assert(TK_GT==OP_Gt); testcase(op==OP_Gt); VdbeCoverageIf(v,op==OP_Gt);
        assert(TK_GE==OP_Ge); testcase(op==OP_Ge); VdbeCoverageIf(v,op==OP_Ge);
        assert(TK_EQ==OP_Eq); testcase(op==OP_Eq); VdbeCoverageIf(v,op==OP_Eq);
        assert(TK_NE==OP_Ne); testcase(op==OP_Ne); VdbeCoverageIf(v,op==OP_Ne);
        if( p5==CAPDB_NULLEQ ){
          capdbVdbeAddOp2(v, OP_Integer, 0, inReg);
        }else{
          capdbVdbeAddOp3(v, OP_ZeroOrNull, r1, inReg, r2);
          if( addrIsNull ){
            capdbVdbeAddOp2(v, OP_Goto, 0, capdbVdbeCurrentAddr(v)+2);
            capdbVdbeJumpHere(v, addrIsNull);
            capdbVdbeAddOp2(v, OP_Null, 0, inReg);
          }
        }
        testcase( regFree1==0 );
        testcase( regFree2==0 );
      }
      break;
    }
    case TK_AND:
    case TK_OR: {
      inReg = exprCodeTargetAndOr(pParse, pExpr, target, &regFree1);
      break;
    }
    case TK_PLUS:
    case TK_STAR:
    case TK_MINUS:
    case TK_REM:
    case TK_BITAND:
    case TK_BITOR:
    case TK_SLASH:
    case TK_LSHIFT:
    case TK_RSHIFT:
    case TK_CONCAT: {
      int addrIsNull;
      assert( TK_PLUS==OP_Add );           testcase( op==TK_PLUS );
      assert( TK_MINUS==OP_Subtract );     testcase( op==TK_MINUS );
      assert( TK_REM==OP_Remainder );      testcase( op==TK_REM );
      assert( TK_BITAND==OP_BitAnd );      testcase( op==TK_BITAND );
      assert( TK_BITOR==OP_BitOr );        testcase( op==TK_BITOR );
      assert( TK_SLASH==OP_Divide );       testcase( op==TK_SLASH );
      assert( TK_LSHIFT==OP_ShiftLeft );   testcase( op==TK_LSHIFT );
      assert( TK_RSHIFT==OP_ShiftRight );  testcase( op==TK_RSHIFT );
      assert( TK_CONCAT==OP_Concat );      testcase( op==TK_CONCAT );
      if( ExprHasProperty(pExpr, EP_Subquery) ){
        addrIsNull = exprComputeOperands(pParse, pExpr,
                                   &r1, &r2, &regFree1, &regFree2);
      }else{
        r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
        r2 = capdbExprCodeTemp(pParse, pExpr->pRight, &regFree2);
        addrIsNull = 0;
      }
      capdbVdbeAddOp3(v, op, r2, r1, target);
      testcase( regFree1==0 );
      testcase( regFree2==0 );
      if( addrIsNull ){
        capdbVdbeAddOp2(v, OP_Goto, 0, capdbVdbeCurrentAddr(v)+2);
        capdbVdbeJumpHere(v, addrIsNull);
        capdbVdbeAddOp2(v, OP_Null, 0, target);
        VdbeComment((v, "short-circut value"));
      }
      break;
    }
    case TK_UMINUS: {
      Expr *pLeft = pExpr->pLeft;
      assert( pLeft );
      if( pLeft->op==TK_INTEGER ){
        codeInteger(pParse, pLeft, 1, target);
        return target;
#ifndef CAPDB_OMIT_FLOATING_POINT
      }else if( pLeft->op==TK_FLOAT ){
        assert( !ExprHasProperty(pExpr, EP_IntValue) );
        codeReal(v, pLeft->u.zToken, 1, target);
        return target;
#endif
      }else{
        tempX.op = TK_INTEGER;
        tempX.flags = EP_IntValue|EP_TokenOnly;
        tempX.u.iValue = 0;
        ExprClearVVAProperties(&tempX);
        r1 = capdbExprCodeTemp(pParse, &tempX, &regFree1);
        r2 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree2);
        capdbVdbeAddOp3(v, OP_Subtract, r2, r1, target);
        testcase( regFree2==0 );
      }
      break;
    }
    case TK_BITNOT:
    case TK_NOT: {
      assert( TK_BITNOT==OP_BitNot );   testcase( op==TK_BITNOT );
      assert( TK_NOT==OP_Not );         testcase( op==TK_NOT );
      r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
      testcase( regFree1==0 );
      capdbVdbeAddOp2(v, op, r1, inReg);
      break;
    }
    case TK_TRUTH: {
      int isTrue;    /* IS TRUE or IS NOT TRUE */
      int bNormal;   /* IS TRUE or IS FALSE */
      r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
      testcase( regFree1==0 );
      isTrue = capdbExprTruthValue(pExpr->pRight);
      bNormal = pExpr->op2==TK_IS;
      testcase( isTrue && bNormal);
      testcase( !isTrue && bNormal);
      capdbVdbeAddOp4Int(v, OP_IsTrue, r1, inReg, !isTrue, isTrue ^ bNormal);
      break;
    }
    case TK_ISNULL:
    case TK_NOTNULL: {
      int addr;
      assert( TK_ISNULL==OP_IsNull );   testcase( op==TK_ISNULL );
      assert( TK_NOTNULL==OP_NotNull ); testcase( op==TK_NOTNULL );
      capdbVdbeAddOp2(v, OP_Integer, 1, target);
      r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
      testcase( regFree1==0 );
      addr = capdbVdbeAddOp1(v, op, r1);
      VdbeCoverageIf(v, op==TK_ISNULL);
      VdbeCoverageIf(v, op==TK_NOTNULL);
      capdbVdbeAddOp2(v, OP_Integer, 0, target);
      capdbVdbeJumpHere(v, addr);
      break;
    }
    case TK_AGG_FUNCTION: {
      AggInfo *pInfo = pExpr->pAggInfo;
      if( pInfo==0
       || NEVER(pExpr->iAgg<0)
       || NEVER(pExpr->iAgg>=pInfo->nFunc)
      ){
        assert( !ExprHasProperty(pExpr, EP_IntValue) );
        capdbErrorMsg(pParse, "misuse of aggregate: %#T()", pExpr);
      }else{
        return AggInfoFuncReg(pInfo, pExpr->iAgg);
      }
      break;
    }
    case TK_FUNCTION: {
      ExprList *pFarg;       /* List of function arguments */
      int nFarg;             /* Number of function arguments */
      FuncDef *pDef;         /* The function definition object */
      const char *zId;       /* The function name */
      u32 constMask = 0;     /* Mask of function arguments that are constant */
      int i;                 /* Loop counter */
      capdb *db = pParse->db;  /* The database connection */
      u8 enc = ENC(db);      /* The text encoding used by this database */
      CollSeq *pColl = 0;    /* A collating sequence */

#ifndef CAPDB_OMIT_WINDOWFUNC
      if( ExprHasProperty(pExpr, EP_WinFunc) ){
        return pExpr->y.pWin->regResult;
      }
#endif

      if( ConstFactorOk(pParse)
       && capdbExprIsConstantNotJoin(pParse,pExpr)
      ){
        /* SQL functions can be expensive. So try to avoid running them
        ** multiple times if we know they always give the same result */
        return capdbExprCodeRunJustOnce(pParse, pExpr, -1);
      }
      assert( !ExprHasProperty(pExpr, EP_TokenOnly) );
      assert( ExprUseXList(pExpr) );
      pFarg = pExpr->x.pList;
      nFarg = pFarg ? pFarg->nExpr : 0;
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      zId = pExpr->u.zToken;
      pDef = capdbFindFunction(db, zId, nFarg, enc, 0);
#ifdef CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION
      if( pDef==0 && pParse->explain ){
        pDef = capdbFindFunction(db, "unknown", nFarg, enc, 0);
      }
#endif
      if( pDef==0 || pDef->xFinalize!=0 ){
        capdbErrorMsg(pParse, "unknown function: %#T()", pExpr);
        break;
      }
      if( (pDef->funcFlags & CAPDB_FUNC_INLINE)!=0 && ALWAYS(pFarg!=0) ){
        assert( (pDef->funcFlags & CAPDB_FUNC_UNSAFE)==0 );
        assert( (pDef->funcFlags & CAPDB_FUNC_DIRECT)==0 );
        return exprCodeInlineFunction(pParse, pFarg,
             CAPDB_PTR_TO_INT(pDef->pUserData), target);
      }else if( pDef->funcFlags & (CAPDB_FUNC_DIRECT|CAPDB_FUNC_UNSAFE) ){
        capdbExprFunctionUsable(pParse, pExpr, pDef);
      }

      for(i=0; i<nFarg; i++){
        if( i<32 && capdbExprIsConstant(pParse, pFarg->a[i].pExpr) ){
          testcase( i==31 );
          constMask |= MASKBIT32(i);
        }
        if( (pDef->funcFlags & CAPDB_FUNC_NEEDCOLL)!=0 && !pColl ){
          pColl = capdbExprCollSeq(pParse, pFarg->a[i].pExpr);
        }
      }
      if( pFarg ){
        if( constMask ){
          r1 = pParse->nMem+1;
          pParse->nMem += nFarg;
        }else{
          r1 = capdbGetTempRange(pParse, nFarg);
        }

        /* For length() and typeof() and octet_length() functions,
        ** set the P5 parameter to the OP_Column opcode to OPFLAG_LENGTHARG
        ** or OPFLAG_TYPEOFARG or OPFLAG_BYTELENARG respectively, to avoid
        ** unnecessary data loading.
        */
        if( (pDef->funcFlags & (CAPDB_FUNC_LENGTH|CAPDB_FUNC_TYPEOF))!=0 ){
          u8 exprOp;
          assert( nFarg==1 );
          assert( pFarg->a[0].pExpr!=0 );
          exprOp = pFarg->a[0].pExpr->op;
          if( exprOp==TK_COLUMN || exprOp==TK_AGG_COLUMN ){
            assert( CAPDB_FUNC_LENGTH==OPFLAG_LENGTHARG );
            assert( CAPDB_FUNC_TYPEOF==OPFLAG_TYPEOFARG );
            assert( CAPDB_FUNC_BYTELEN==OPFLAG_BYTELENARG );
            assert( (OPFLAG_LENGTHARG|OPFLAG_TYPEOFARG)==OPFLAG_BYTELENARG );
            testcase( (pDef->funcFlags & OPFLAG_BYTELENARG)==OPFLAG_LENGTHARG );
            testcase( (pDef->funcFlags & OPFLAG_BYTELENARG)==OPFLAG_TYPEOFARG );
            testcase( (pDef->funcFlags & OPFLAG_BYTELENARG)==OPFLAG_BYTELENARG);
            pFarg->a[0].pExpr->op2 = pDef->funcFlags & OPFLAG_BYTELENARG;
          }
        }

        capdbExprCodeExprList(pParse, pFarg, r1, 0, CAPDB_ECEL_FACTOR);
      }else{
        r1 = 0;
      }
#ifndef CAPDB_OMIT_VIRTUALTABLE
      /* Possibly overload the function if the first argument is
      ** a virtual table column.
      **
      ** For infix functions (LIKE, GLOB, REGEXP, and MATCH) use the
      ** second argument, not the first, as the argument to test to
      ** see if it is a column in a virtual table.  This is done because
      ** the left operand of infix functions (the operand we want to
      ** control overloading) ends up as the second argument to the
      ** function.  The expression "A glob B" is equivalent to
      ** "glob(B,A).  We want to use the A in "A glob B" to test
      ** for function overloading.  But we use the B term in "glob(B,A)".
      */
      if( nFarg>=2 && ExprHasProperty(pExpr, EP_InfixFunc) ){
        pDef = capdbVtabOverloadFunction(db, pDef, nFarg, pFarg->a[1].pExpr);
      }else if( nFarg>0 ){
        pDef = capdbVtabOverloadFunction(db, pDef, nFarg, pFarg->a[0].pExpr);
      }
#endif
      if( pDef->funcFlags & CAPDB_FUNC_NEEDCOLL ){
        if( !pColl ) pColl = db->pDfltColl;
        capdbVdbeAddOp4(v, OP_CollSeq, 0, 0, 0, (char *)pColl, P4_COLLSEQ);
      }
      capdbVdbeAddFunctionCall(pParse, constMask, r1, target, nFarg,
                                 pDef, pExpr->op2);
      if( nFarg ){
        if( constMask==0 ){
          capdbReleaseTempRange(pParse, r1, nFarg);
        }else{
          capdbVdbeReleaseRegisters(pParse, r1, nFarg, constMask, 1);
        }
      }
      return target;
    }
#ifndef CAPDB_OMIT_SUBQUERY
    case TK_EXISTS:
    case TK_SELECT: {
      int nCol;
      testcase( op==TK_EXISTS );
      testcase( op==TK_SELECT );
      if( pParse->db->mallocFailed ){
        return 0;
      }else if( op==TK_SELECT
             && ALWAYS( ExprUseXSelect(pExpr) )
             && (nCol = pExpr->x.pSelect->pEList->nExpr)!=1
      ){
        capdbSubselectError(pParse, nCol, 1);
      }else{
        return capdbCodeSubselect(pParse, pExpr);
      }
      break;
    }
    case TK_SELECT_COLUMN: {
      int n;
      Expr *pLeft = pExpr->pLeft;
      if( pLeft->iTable==0 || pParse->withinRJSubrtn > pLeft->op2 ){
        pLeft->iTable = capdbCodeSubselect(pParse, pLeft);
        pLeft->op2 = pParse->withinRJSubrtn;
      }
      assert( pLeft->op==TK_SELECT || pLeft->op==TK_ERROR );
      n = capdbExprVectorSize(pLeft);
      if( pExpr->iTable!=n ){
        capdbErrorMsg(pParse, "%d columns assigned %d values",
                                pExpr->iTable, n);
      }
      return pLeft->iTable + pExpr->iColumn;
    }
    case TK_IN: {
      int destIfFalse = capdbVdbeMakeLabel(pParse);
      int destIfNull = capdbVdbeMakeLabel(pParse);
      capdbVdbeAddOp2(v, OP_Null, 0, target);
      capdbExprCodeIN(pParse, pExpr, destIfFalse, destIfNull);
      capdbVdbeAddOp2(v, OP_Integer, 1, target);
      capdbVdbeResolveLabel(v, destIfFalse);
      capdbVdbeAddOp2(v, OP_AddImm, target, 0);
      capdbVdbeResolveLabel(v, destIfNull);
      return target;
    }
#endif /* CAPDB_OMIT_SUBQUERY */


    /*
    **    x BETWEEN y AND z
    **
    ** This is equivalent to
    **
    **    x>=y AND x<=z
    **
    ** X is stored in pExpr->pLeft.
    ** Y is stored in pExpr->pList->a[0].pExpr.
    ** Z is stored in pExpr->pList->a[1].pExpr.
    */
    case TK_BETWEEN: {
      exprCodeBetween(pParse, pExpr, target, 0, 0);
      return target;
    }
    case TK_COLLATE: {
      if( !ExprHasProperty(pExpr, EP_Collate) ){
        /* A TK_COLLATE Expr node without the EP_Collate tag is a so-called
        ** "SOFT-COLLATE" that is added to constraints that are pushed down
        ** from outer queries into sub-queries by the WHERE-clause push-down
        ** optimization. Clear subtypes as subtypes may not cross a subquery
        ** boundary.
        */
        assert( pExpr->pLeft );
        capdbExprCode(pParse, pExpr->pLeft, target);
        capdbVdbeAddOp1(v, OP_ClrSubtype, target);
        return target;
      }else{
        pExpr = pExpr->pLeft;
        goto expr_code_doover; /* 2018-04-28: Prevent deep recursion. */
      }
    }
    case TK_SPAN:
    case TK_UPLUS: {
      pExpr = pExpr->pLeft;
      goto expr_code_doover; /* 2018-04-28: Prevent deep recursion. OSSFuzz. */
    }

    case TK_TRIGGER: {
      /* If the opcode is TK_TRIGGER, then the expression is a reference
      ** to a column in the new.* or old.* pseudo-tables available to
      ** trigger programs. In this case Expr.iTable is set to 1 for the
      ** new.* pseudo-table, or 0 for the old.* pseudo-table. Expr.iColumn
      ** is set to the column of the pseudo-table to read, or to -1 to
      ** read the rowid field.
      **
      ** The expression is implemented using an OP_Param opcode. The p1
      ** parameter is set to 0 for an old.rowid reference, or to (i+1)
      ** to reference another column of the old.* pseudo-table, where
      ** i is the index of the column. For a new.rowid reference, p1 is
      ** set to (n+1), where n is the number of columns in each pseudo-table.
      ** For a reference to any other column in the new.* pseudo-table, p1
      ** is set to (n+2+i), where n and i are as defined previously. For
      ** example, if the table on which triggers are being fired is
      ** declared as:
      **
      **   CREATE TABLE t1(a, b);
      **
      ** Then p1 is interpreted as follows:
      **
      **   p1==0   ->    old.rowid     p1==3   ->    new.rowid
      **   p1==1   ->    old.a         p1==4   ->    new.a
      **   p1==2   ->    old.b         p1==5   ->    new.b      
      */
      Table *pTab;
      int iCol;
      int p1;

      assert( ExprUseYTab(pExpr) );
      pTab = pExpr->y.pTab;
      iCol = pExpr->iColumn;
      p1 = pExpr->iTable * (pTab->nCol+1) + 1
                     + capdbTableColumnToStorage(pTab, iCol);

      assert( pExpr->iTable==0 || pExpr->iTable==1 );
      assert( iCol>=-1 && iCol<pTab->nCol );
      assert( pTab->iPKey<0 || iCol!=pTab->iPKey );
      assert( p1>=0 && p1<(pTab->nCol*2+2) );

      capdbVdbeAddOp2(v, OP_Param, p1, target);
      VdbeComment((v, "r[%d]=%s.%s", target,
        (pExpr->iTable ? "new" : "old"),
        (pExpr->iColumn<0 ? "rowid" : pExpr->y.pTab->aCol[iCol].zCnName)
      ));

#ifndef CAPDB_OMIT_FLOATING_POINT
      /* If the column has REAL affinity, it may currently be stored as an
      ** integer. Use OP_RealAffinity to make sure it is really real.
      **
      ** EVIDENCE-OF: R-60985-57662 SQLite will convert the value back to
      ** floating point when extracting it from the record.  */
      if( iCol>=0 && pTab->aCol[iCol].affinity==CAPDB_AFF_REAL ){
        capdbVdbeAddOp1(v, OP_RealAffinity, target);
      }
#endif
      break;
    }

    case TK_VECTOR: {
      capdbErrorMsg(pParse, "row value misused");
      break;
    }

    /* TK_IF_NULL_ROW Expr nodes are inserted ahead of expressions
    ** that derive from the right-hand table of a LEFT JOIN.  The
    ** Expr.iTable value is the table number for the right-hand table.
    ** The expression is only evaluated if that table is not currently
    ** on a LEFT JOIN NULL row.
    */
    case TK_IF_NULL_ROW: {
      int addrINR;
      u8 okConstFactor = pParse->okConstFactor;
      AggInfo *pAggInfo = pExpr->pAggInfo;
      if( pAggInfo ){
        assert( pExpr->iAgg>=0 && pExpr->iAgg<pAggInfo->nColumn );
        if( !pAggInfo->directMode ){
          inReg = AggInfoColumnReg(pAggInfo, pExpr->iAgg);
          break;
        }
        if( pExpr->pAggInfo->useSortingIdx ){
          capdbVdbeAddOp3(v, OP_Column, pAggInfo->sortingIdxPTab,
                            pAggInfo->aCol[pExpr->iAgg].iSorterColumn,
                            target);
          inReg = target;
          break;
        }
      }
      addrINR = capdbVdbeAddOp3(v, OP_IfNullRow, pExpr->iTable, 0, target);
      /* The OP_IfNullRow opcode above can overwrite the result register with
      ** NULL.  So we have to ensure that the result register is not a value
      ** that is suppose to be a constant.  Two defenses are needed:
      **   (1)  Temporarily disable factoring of constant expressions
      **   (2)  Make sure the computed value really is stored in register
      **        "target" and not someplace else.
      */
      pParse->okConstFactor = 0;   /* note (1) above */
      capdbExprCode(pParse, pExpr->pLeft, target);
      assert( target==inReg );
      pParse->okConstFactor = okConstFactor;
      capdbVdbeJumpHere(v, addrINR);
      break;
    }

    /*
    ** Form A:
    **   CASE x WHEN e1 THEN r1 WHEN e2 THEN r2 ... WHEN eN THEN rN ELSE y END
    **
    ** Form B:
    **   CASE WHEN e1 THEN r1 WHEN e2 THEN r2 ... WHEN eN THEN rN ELSE y END
    **
    ** Form A is can be transformed into the equivalent form B as follows:
    **   CASE WHEN x=e1 THEN r1 WHEN x=e2 THEN r2 ...
    **        WHEN x=eN THEN rN ELSE y END
    **
    ** X (if it exists) is in pExpr->pLeft.
    ** Y is in the last element of pExpr->x.pList if pExpr->x.pList->nExpr is
    ** odd.  The Y is also optional.  If the number of elements in x.pList
    ** is even, then Y is omitted and the "otherwise" result is NULL.
    ** Ei is in pExpr->pList->a[i*2] and Ri is pExpr->pList->a[i*2+1].
    **
    ** The result of the expression is the Ri for the first matching Ei,
    ** or if there is no matching Ei, the ELSE term Y, or if there is
    ** no ELSE term, NULL.
    */
    case TK_CASE: {
      int endLabel;                     /* GOTO label for end of CASE stmt */
      int nextCase;                     /* GOTO label for next WHEN clause */
      int nExpr;                        /* 2x number of WHEN terms */
      int i;                            /* Loop counter */
      ExprList *pEList;                 /* List of WHEN terms */
      struct ExprList_item *aListelem;  /* Array of WHEN terms */
      Expr opCompare;                   /* The X==Ei expression */
      Expr *pX;                         /* The X expression */
      Expr *pTest = 0;                  /* X==Ei (form A) or just Ei (form B) */
      Expr *pDel = 0;
      capdb *db = pParse->db;

      assert( ExprUseXList(pExpr) && pExpr->x.pList!=0 );
      assert(pExpr->x.pList->nExpr > 0);
      pEList = pExpr->x.pList;
      aListelem = pEList->a;
      nExpr = pEList->nExpr;
      endLabel = capdbVdbeMakeLabel(pParse);
      if( (pX = pExpr->pLeft)!=0 ){
        pDel = capdbExprDup(db, pX, 0);
        if( db->mallocFailed ){
          capdbExprDelete(db, pDel);
          break;
        }
        testcase( pX->op==TK_COLUMN );
        capdbExprToRegister(pDel, exprCodeVector(pParse, pDel, &regFree1));
        testcase( regFree1==0 );
        memset(&opCompare, 0, sizeof(opCompare));
        opCompare.op = TK_EQ;
        opCompare.pLeft = pDel;
        pTest = &opCompare;
        /* Ticket b351d95f9cd5ef17e9d9dbae18f5ca8611190001:
        ** The value in regFree1 might get SCopy-ed into the file result.
        ** So make sure that the regFree1 register is not reused for other
        ** purposes and possibly overwritten.  */
        regFree1 = 0;
      }
      for(i=0; i<nExpr-1; i=i+2){
        if( pX ){
          assert( pTest!=0 );
          opCompare.pRight = aListelem[i].pExpr;
        }else{
          pTest = aListelem[i].pExpr;
        }
        nextCase = capdbVdbeMakeLabel(pParse);
        testcase( pTest->op==TK_COLUMN );
        capdbExprIfFalse(pParse, pTest, nextCase, CAPDB_JUMPIFNULL);
        testcase( aListelem[i+1].pExpr->op==TK_COLUMN );
        capdbExprCode(pParse, aListelem[i+1].pExpr, target);
        capdbVdbeGoto(v, endLabel);
        capdbVdbeResolveLabel(v, nextCase);
      }
      if( (nExpr&1)!=0 ){
        capdbExprCode(pParse, pEList->a[nExpr-1].pExpr, target);
      }else{
        capdbVdbeAddOp2(v, OP_Null, 0, target);
      }
      capdbExprDelete(db, pDel);
      setDoNotMergeFlagOnCopy(v);
      capdbVdbeResolveLabel(v, endLabel);
      break;
    }
#ifndef CAPDB_OMIT_TRIGGER
    case TK_RAISE: {
      assert( pExpr->affExpr==OE_Rollback
           || pExpr->affExpr==OE_Abort
           || pExpr->affExpr==OE_Fail
           || pExpr->affExpr==OE_Ignore
      );
      if( !pParse->pTriggerTab && !pParse->nested ){
        capdbErrorMsg(pParse,
                       "RAISE() may only be used within a trigger-program");
        return 0;
      }
      if( pExpr->affExpr==OE_Abort ){
        capdbMayAbort(pParse);
      }
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      if( pExpr->affExpr==OE_Ignore ){
        capdbVdbeAddOp2(v, OP_Halt, CAPDB_OK, OE_Ignore);
        VdbeCoverage(v);
      }else{
        r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
        capdbVdbeAddOp3(v, OP_Halt, 
             pParse->pTriggerTab ? CAPDB_CONSTRAINT_TRIGGER : CAPDB_ERROR,
             pExpr->affExpr, r1);
      }
      break;
    }
#endif
  }
  capdbReleaseTempReg(pParse, regFree1);
  capdbReleaseTempReg(pParse, regFree2);
  return inReg;
}

/*
** Generate code that will evaluate expression pExpr just one time
** per prepared statement execution.
**
** If the expression uses functions (that might throw an exception) then
** guard them with an OP_Once opcode to ensure that the code is only executed
** once. If no functions are involved, then factor the code out and put it at
** the end of the prepared statement in the initialization section.
**
** If regDest>0 then the result is always stored in that register and the
** result is not reusable.  If regDest<0 then this routine is free to
** store the value wherever it wants.  The register where the expression
** is stored is returned.  When regDest<0, two identical expressions might
** code to the same register, if they do not contain function calls and hence
** are factored out into the initialization section at the end of the
** prepared statement.
*/
int capdbExprCodeRunJustOnce(
  Parse *pParse,    /* Parsing context */
  Expr *pExpr,      /* The expression to code when the VDBE initializes */
  int regDest       /* Store the value in this register */
){
  ExprList *p;
  assert( ConstFactorOk(pParse) );
  assert( regDest!=0 );
  p = pParse->pConstExpr;
  if( regDest<0 && p ){
    struct ExprList_item *pItem;
    int i;
    for(pItem=p->a, i=p->nExpr; i>0; pItem++, i--){
      if( pItem->fg.reusable
       && capdbExprCompare(0,pItem->pExpr,pExpr,-1)==0
      ){
        return pItem->u.iConstExprReg;
      }
    }
  }
  pExpr = capdbExprDup(pParse->db, pExpr, 0);
  if( pExpr!=0 && ExprHasProperty(pExpr, EP_HasFunc) ){
    Vdbe *v = pParse->pVdbe;
    int addr;
    assert( v );
    addr = capdbVdbeAddOp0(v, OP_Once); VdbeCoverage(v);
    pParse->okConstFactor = 0;
    if( !pParse->db->mallocFailed ){
      if( regDest<0 ) regDest = ++pParse->nMem;
      capdbExprCode(pParse, pExpr, regDest);
    }
    pParse->okConstFactor = 1;
    capdbExprDelete(pParse->db, pExpr);
    capdbVdbeJumpHere(v, addr);
  }else{
    p = capdbExprListAppend(pParse, p, pExpr);
    if( p ){
       struct ExprList_item *pItem = &p->a[p->nExpr-1];
       pItem->fg.reusable = regDest<0;
       if( regDest<0 ) regDest = ++pParse->nMem;
       pItem->u.iConstExprReg = regDest;
    }
    pParse->pConstExpr = p;
  }
  return regDest;
}

/*
** Make arrangements to invoke OP_Null on a range of registers
** during initialization.
*/
CAPDB_NOINLINE void capdbExprNullRegisterRange(
  Parse *pParse,   /* Parsing context */
  int iReg,        /* First register to set to NULL */
  int nReg         /* Number of sequential registers to NULL out */
){
  u8 okConstFactor = pParse->okConstFactor;
  Expr t;
  memset(&t, 0, sizeof(t));
  t.op = TK_NULLS;
  t.y.nReg = nReg;
  pParse->okConstFactor = 1;
  capdbExprCodeRunJustOnce(pParse, &t, iReg);
  pParse->okConstFactor = okConstFactor;
}

/*
** Generate code to evaluate an expression and store the results
** into a register.  Return the register number where the results
** are stored.
**
** If the register is a temporary register that can be deallocated,
** then write its number into *pReg.  If the result register is not
** a temporary, then set *pReg to zero.
**
** If pExpr is a constant, then this routine might generate this
** code to fill the register in the initialization section of the
** VDBE program, in order to factor it out of the evaluation loop.
*/
int capdbExprCodeTemp(Parse *pParse, Expr *pExpr, int *pReg){
  int r2;
  pExpr = capdbExprSkipCollateAndLikely(pExpr);
  if( ConstFactorOk(pParse)
   && ALWAYS(pExpr!=0)
   && pExpr->op!=TK_REGISTER
   && capdbExprIsConstantNotJoin(pParse, pExpr)
  ){
    *pReg  = 0;
    r2 = capdbExprCodeRunJustOnce(pParse, pExpr, -1);
  }else{
    int r1 = capdbGetTempReg(pParse);
    r2 = capdbExprCodeTarget(pParse, pExpr, r1);
    if( r2==r1 ){
      *pReg = r1;
    }else{
      capdbReleaseTempReg(pParse, r1);
      *pReg = 0;
    }
  }
  return r2;
}

/*
** Generate code that will evaluate expression pExpr and store the
** results in register target.  The results are guaranteed to appear
** in register target.
*/
void capdbExprCode(Parse *pParse, Expr *pExpr, int target){
  int inReg;

  assert( pExpr==0 || !ExprHasVVAProperty(pExpr,EP_Immutable) );
  assert( target>0 && target<=pParse->nMem );
  assert( pParse->pVdbe!=0 || pParse->db->mallocFailed );
  if( pParse->pVdbe==0 ) return;
  inReg = capdbExprCodeTarget(pParse, pExpr, target);
  if( inReg!=target ){
    u8 op;
    Expr *pX = capdbExprSkipCollateAndLikely(pExpr);
    testcase( pX!=pExpr );
    if( ALWAYS(pX)
     && (ExprHasProperty(pX,EP_Subquery) || pX->op==TK_REGISTER)
    ){
      op = OP_Copy;
    }else{
      op = OP_SCopy;
    }
    capdbVdbeAddOp2(pParse->pVdbe, op, inReg, target);
  }
}

/*
** Make a transient copy of expression pExpr and then code it using
** capdbExprCode().  This routine works just like capdbExprCode()
** except that the input expression is guaranteed to be unchanged.
*/
void capdbExprCodeCopy(Parse *pParse, Expr *pExpr, int target){
  capdb *db = pParse->db;
  pExpr = capdbExprDup(db, pExpr, 0);
  if( !db->mallocFailed ) capdbExprCode(pParse, pExpr, target);
  capdbExprDelete(db, pExpr);
}

/*
** Generate code that will evaluate expression pExpr and store the
** results in register target.  The results are guaranteed to appear
** in register target.  If the expression is constant, then this routine
** might choose to code the expression at initialization time.
*/
void capdbExprCodeFactorable(Parse *pParse, Expr *pExpr, int target){
  if( pParse->okConstFactor && capdbExprIsConstantNotJoin(pParse,pExpr) ){
    capdbExprCodeRunJustOnce(pParse, pExpr, target);
  }else{
    capdbExprCodeCopy(pParse, pExpr, target);
  }
}

/*
** Generate code that pushes the value of every element of the given
** expression list into a sequence of registers beginning at target.
**
** Return the number of elements evaluated.  The number returned will
** usually be pList->nExpr but might be reduced if CAPDB_ECEL_OMITREF
** is defined.
**
** The CAPDB_ECEL_DUP flag prevents the arguments from being
** filled using OP_SCopy.  OP_Copy must be used instead.
**
** The CAPDB_ECEL_FACTOR argument allows constant arguments to be
** factored out into initialization code.
**
** The CAPDB_ECEL_REF flag means that expressions in the list with
** ExprList.a[].u.x.iOrderByCol>0 have already been evaluated and stored
** in registers at srcReg, and so the value can be copied from there.
** If CAPDB_ECEL_OMITREF is also set, then the values with u.x.iOrderByCol>0
** are simply omitted rather than being copied from srcReg.
*/
int capdbExprCodeExprList(
  Parse *pParse,     /* Parsing context */
  ExprList *pList,   /* The expression list to be coded */
  int target,        /* Where to write results */
  int srcReg,        /* Source registers if CAPDB_ECEL_REF */
  u8 flags           /* CAPDB_ECEL_* flags */
){
  struct ExprList_item *pItem;
  int i, j, n;
  u8 copyOp = (flags & CAPDB_ECEL_DUP) ? OP_Copy : OP_SCopy;
  Vdbe *v = pParse->pVdbe;
  assert( pList!=0 );
  assert( target>0 );
  assert( pParse->pVdbe!=0 );  /* Never gets this far otherwise */
  n = pList->nExpr;
  if( !ConstFactorOk(pParse) ) flags &= ~CAPDB_ECEL_FACTOR;
  for(pItem=pList->a, i=0; i<n; i++, pItem++){
    Expr *pExpr = pItem->pExpr;
#ifdef CAPDB_ENABLE_SORTER_REFERENCES
    if( pItem->fg.bSorterRef ){
      i--;
      n--;
    }else
#endif
    if( (flags & CAPDB_ECEL_REF)!=0 && (j = pItem->u.x.iOrderByCol)>0 ){
      if( flags & CAPDB_ECEL_OMITREF ){
        i--;
        n--;
      }else{
        capdbVdbeAddOp2(v, copyOp, j+srcReg-1, target+i);
      }
    }else if( (flags & CAPDB_ECEL_FACTOR)!=0
           && capdbExprIsConstantNotJoin(pParse,pExpr)
    ){
      capdbExprCodeRunJustOnce(pParse, pExpr, target+i);
    }else{
      int inReg = capdbExprCodeTarget(pParse, pExpr, target+i);
      if( inReg!=target+i ){
        VdbeOp *pOp;
        if( copyOp==OP_Copy
         && (pOp=capdbVdbeGetLastOp(v))->opcode==OP_Copy
         && pOp->p1+pOp->p3+1==inReg
         && pOp->p2+pOp->p3+1==target+i
         && pOp->p5==0  /* The do-not-merge flag must be clear */
        ){
          pOp->p3++;
        }else{
          capdbVdbeAddOp2(v, copyOp, inReg, target+i);
        }
      }
    }
  }
  return n;
}

/*
** Generate code for a BETWEEN operator.
**
**    x BETWEEN y AND z
**
** The above is equivalent to
**
**    x>=y AND x<=z
**
** Code it as such, taking care to do the common subexpression
** elimination of x.
**
** The xJumpIf parameter determines details:
**
**    NULL:                   Store the boolean result in reg[dest]
**    capdbExprIfTrue:      Jump to dest if true
**    capdbExprIfFalse:     Jump to dest if false
**
** The jumpIfNull parameter is ignored if xJumpIf is NULL.
*/
static void exprCodeBetween(
  Parse *pParse,    /* Parsing and code generating context */
  Expr *pExpr,      /* The BETWEEN expression */
  int dest,         /* Jump destination or storage location */
  void (*xJump)(Parse*,Expr*,int,int), /* Action to take */
  int jumpIfNull    /* Take the jump if the BETWEEN is NULL */
){
  Expr exprAnd;     /* The AND operator in  x>=y AND x<=z  */
  Expr compLeft;    /* The  x>=y  term */
  Expr compRight;   /* The  x<=z  term */
  int regFree1 = 0; /* Temporary use register */
  Expr *pDel = 0;
  capdb *db = pParse->db;

  memset(&compLeft, 0, sizeof(Expr));
  memset(&compRight, 0, sizeof(Expr));
  memset(&exprAnd, 0, sizeof(Expr));

  assert( ExprUseXList(pExpr) );
  pDel = capdbExprDup(db, pExpr->pLeft, 0);
  if( db->mallocFailed==0 ){
    exprAnd.op = TK_AND;
    exprAnd.pLeft = &compLeft;
    exprAnd.pRight = &compRight;
    compLeft.op = TK_GE;
    compLeft.pLeft = pDel;
    compLeft.pRight = pExpr->x.pList->a[0].pExpr;
    compRight.op = TK_LE;
    compRight.pLeft = pDel;
    compRight.pRight = pExpr->x.pList->a[1].pExpr;
    capdbExprToRegister(pDel, exprCodeVector(pParse, pDel, &regFree1));
    if( xJump ){
      xJump(pParse, &exprAnd, dest, jumpIfNull);
    }else{
      /* Mark the expression is being from the ON or USING clause of a join
      ** so that the capdbExprCodeTarget() routine will not attempt to move
      ** it into the Parse.pConstExpr list.  We should use a new bit for this,
      ** for clarity, but we are out of bits in the Expr.flags field so we
      ** have to reuse the EP_OuterON bit.  Bummer. */
      pDel->flags |= EP_OuterON;
      capdbExprCodeTarget(pParse, &exprAnd, dest);
    }
    capdbReleaseTempReg(pParse, regFree1);
  }
  capdbExprDelete(db, pDel);

  /* Ensure adequate test coverage */
  testcase( xJump==capdbExprIfTrue  && jumpIfNull==0 && regFree1==0 );
  testcase( xJump==capdbExprIfTrue  && jumpIfNull==0 && regFree1!=0 );
  testcase( xJump==capdbExprIfTrue  && jumpIfNull!=0 && regFree1==0 );
  testcase( xJump==capdbExprIfTrue  && jumpIfNull!=0 && regFree1!=0 );
  testcase( xJump==capdbExprIfFalse && jumpIfNull==0 && regFree1==0 );
  testcase( xJump==capdbExprIfFalse && jumpIfNull==0 && regFree1!=0 );
  testcase( xJump==capdbExprIfFalse && jumpIfNull!=0 && regFree1==0 );
  testcase( xJump==capdbExprIfFalse && jumpIfNull!=0 && regFree1!=0 );
  testcase( xJump==0 );
}

/*
** Generate code for a boolean expression such that a jump is made
** to the label "dest" if the expression is true but execution
** continues straight thru if the expression is false.
**
** If the expression evaluates to NULL (neither true nor false), then
** take the jump if the jumpIfNull flag is CAPDB_JUMPIFNULL.
**
** This code depends on the fact that certain token values (ex: TK_EQ)
** are the same as opcode values (ex: OP_Eq) that implement the corresponding
** operation.  Special comments in vdbe.c and the mkopcodeh.awk script in
** the make process cause these values to align.  Assert()s in the code
** below verify that the numbers are aligned correctly.
*/
void capdbExprIfTrue(Parse *pParse, Expr *pExpr, int dest, int jumpIfNull){
  Vdbe *v = pParse->pVdbe;
  int op = 0;
  int regFree1 = 0;
  int regFree2 = 0;
  int r1, r2;

  assert( jumpIfNull==CAPDB_JUMPIFNULL || jumpIfNull==0 );
  if( NEVER(v==0) )     return;  /* Existence of VDBE checked by caller */
  if( NEVER(pExpr==0) ) return;  /* No way this can happen */
  assert( !ExprHasVVAProperty(pExpr, EP_Immutable) );
  op = pExpr->op;
  switch( op ){
    case TK_AND:
    case TK_OR: {
      Expr *pAlt = capdbExprSimplifiedAndOr(pExpr);
      if( pAlt!=pExpr ){
        capdbExprIfTrue(pParse, pAlt, dest, jumpIfNull);
      }else{
        Expr *pFirst, *pSecond;
        if( exprEvalRhsFirst(pExpr) ){
          pFirst = pExpr->pRight;
          pSecond = pExpr->pLeft;
        }else{
          pFirst = pExpr->pLeft;
          pSecond = pExpr->pRight;
        }
        if( op==TK_AND ){
          int d2 = capdbVdbeMakeLabel(pParse);
          testcase( jumpIfNull==0 );
          capdbExprIfFalse(pParse, pFirst, d2,
                             jumpIfNull^CAPDB_JUMPIFNULL);
          capdbExprIfTrue(pParse, pSecond, dest, jumpIfNull);
          capdbVdbeResolveLabel(v, d2);
        }else{
          testcase( jumpIfNull==0 );
          capdbExprIfTrue(pParse, pFirst, dest, jumpIfNull);
          capdbExprIfTrue(pParse, pSecond, dest, jumpIfNull);
        }
      }
      break;
    }
    case TK_NOT: {
      testcase( jumpIfNull==0 );
      capdbExprIfFalse(pParse, pExpr->pLeft, dest, jumpIfNull);
      break;
    }
    case TK_TRUTH: {
      int isNot;      /* IS NOT TRUE or IS NOT FALSE */
      int isTrue;     /* IS TRUE or IS NOT TRUE */
      testcase( jumpIfNull==0 );
      isNot = pExpr->op2==TK_ISNOT;
      isTrue = capdbExprTruthValue(pExpr->pRight);
      testcase( isTrue && isNot );
      testcase( !isTrue && isNot );
      if( isTrue ^ isNot ){
        capdbExprIfTrue(pParse, pExpr->pLeft, dest,
                          isNot ? CAPDB_JUMPIFNULL : 0);
      }else{
        capdbExprIfFalse(pParse, pExpr->pLeft, dest,
                           isNot ? CAPDB_JUMPIFNULL : 0);
      }
      break;
    }
    case TK_IS:
    case TK_ISNOT:
      testcase( op==TK_IS );
      testcase( op==TK_ISNOT );
      op = (op==TK_IS) ? TK_EQ : TK_NE;
      jumpIfNull = CAPDB_NULLEQ;
      /* no break */ deliberate_fall_through
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_NE:
    case TK_EQ: {
      int addrIsNull;
      if( capdbExprIsVector(pExpr->pLeft) ) goto default_expr;
      if( ExprHasProperty(pExpr, EP_Subquery) && jumpIfNull!=CAPDB_NULLEQ ){
        addrIsNull = exprComputeOperands(pParse, pExpr,
                                   &r1, &r2, &regFree1, &regFree2);
      }else{
        r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
        r2 = capdbExprCodeTemp(pParse, pExpr->pRight, &regFree2);
        addrIsNull = 0;
      }
      codeCompare(pParse, pExpr->pLeft, pExpr->pRight, op,
                  r1, r2, dest, jumpIfNull, ExprHasProperty(pExpr,EP_Commuted));
      assert(TK_LT==OP_Lt); testcase(op==OP_Lt); VdbeCoverageIf(v,op==OP_Lt);
      assert(TK_LE==OP_Le); testcase(op==OP_Le); VdbeCoverageIf(v,op==OP_Le);
      assert(TK_GT==OP_Gt); testcase(op==OP_Gt); VdbeCoverageIf(v,op==OP_Gt);
      assert(TK_GE==OP_Ge); testcase(op==OP_Ge); VdbeCoverageIf(v,op==OP_Ge);
      assert(TK_EQ==OP_Eq); testcase(op==OP_Eq);
      VdbeCoverageIf(v, op==OP_Eq && jumpIfNull==CAPDB_NULLEQ);
      VdbeCoverageIf(v, op==OP_Eq && jumpIfNull!=CAPDB_NULLEQ);
      assert(TK_NE==OP_Ne); testcase(op==OP_Ne);
      VdbeCoverageIf(v, op==OP_Ne && jumpIfNull==CAPDB_NULLEQ);
      VdbeCoverageIf(v, op==OP_Ne && jumpIfNull!=CAPDB_NULLEQ);
      testcase( regFree1==0 );
      testcase( regFree2==0 );
      if( addrIsNull ){
        if( jumpIfNull ){
          capdbVdbeChangeP2(v, addrIsNull, dest);
        }else{
          capdbVdbeJumpHere(v, addrIsNull);
        }
      }
      break;
    }
    case TK_ISNULL:
    case TK_NOTNULL: {
      assert( TK_ISNULL==OP_IsNull );   testcase( op==TK_ISNULL );
      assert( TK_NOTNULL==OP_NotNull ); testcase( op==TK_NOTNULL );
      r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
      assert( regFree1==0 || regFree1==r1 );
      if( regFree1 ) capdbVdbeTypeofColumn(v, r1);
      capdbVdbeAddOp2(v, op, r1, dest);
      VdbeCoverageIf(v, op==TK_ISNULL);
      VdbeCoverageIf(v, op==TK_NOTNULL);
      break;
    }
    case TK_BETWEEN: {
      testcase( jumpIfNull==0 );
      exprCodeBetween(pParse, pExpr, dest, capdbExprIfTrue, jumpIfNull);
      break;
    }
#ifndef CAPDB_OMIT_SUBQUERY
    case TK_IN: {
      int destIfFalse = capdbVdbeMakeLabel(pParse);
      int destIfNull = jumpIfNull ? dest : destIfFalse;
      capdbExprCodeIN(pParse, pExpr, destIfFalse, destIfNull);
      capdbVdbeGoto(v, dest);
      capdbVdbeResolveLabel(v, destIfFalse);
      break;
    }
#endif
    default: {
    default_expr:
      if( ExprAlwaysTrue(pExpr) ){
        capdbVdbeGoto(v, dest);
      }else if( ExprAlwaysFalse(pExpr) ){
        /* No-op */
      }else{
        r1 = capdbExprCodeTemp(pParse, pExpr, &regFree1);
        capdbVdbeAddOp3(v, OP_If, r1, dest, jumpIfNull!=0);
        VdbeCoverage(v);
        testcase( regFree1==0 );
        testcase( jumpIfNull==0 );
      }
      break;
    }
  }
  capdbReleaseTempReg(pParse, regFree1);
  capdbReleaseTempReg(pParse, regFree2); 
}

/*
** Generate code for a boolean expression such that a jump is made
** to the label "dest" if the expression is false but execution
** continues straight thru if the expression is true.
**
** If the expression evaluates to NULL (neither true nor false) then
** jump if jumpIfNull is CAPDB_JUMPIFNULL or fall through if jumpIfNull
** is 0.
*/
void capdbExprIfFalse(Parse *pParse, Expr *pExpr, int dest, int jumpIfNull){
  Vdbe *v = pParse->pVdbe;
  int op = 0;
  int regFree1 = 0;
  int regFree2 = 0;
  int r1, r2;

  assert( jumpIfNull==CAPDB_JUMPIFNULL || jumpIfNull==0 );
  if( NEVER(v==0) ) return; /* Existence of VDBE checked by caller */
  if( pExpr==0 )    return;
  assert( !ExprHasVVAProperty(pExpr,EP_Immutable) );

  /* The value of pExpr->op and op are related as follows:
  **
  **       pExpr->op            op
  **       ---------          ----------
  **       TK_ISNULL          OP_NotNull
  **       TK_NOTNULL         OP_IsNull
  **       TK_NE              OP_Eq
  **       TK_EQ              OP_Ne
  **       TK_GT              OP_Le
  **       TK_LE              OP_Gt
  **       TK_GE              OP_Lt
  **       TK_LT              OP_Ge
  **
  ** For other values of pExpr->op, op is undefined and unused.
  ** The value of TK_ and OP_ constants are arranged such that we
  ** can compute the mapping above using the following expression.
  ** Assert()s verify that the computation is correct.
  */
  op = ((pExpr->op+(TK_ISNULL&1))^1)-(TK_ISNULL&1);

  /* Verify correct alignment of TK_ and OP_ constants
  */
  assert( pExpr->op!=TK_ISNULL || op==OP_NotNull );
  assert( pExpr->op!=TK_NOTNULL || op==OP_IsNull );
  assert( pExpr->op!=TK_NE || op==OP_Eq );
  assert( pExpr->op!=TK_EQ || op==OP_Ne );
  assert( pExpr->op!=TK_LT || op==OP_Ge );
  assert( pExpr->op!=TK_LE || op==OP_Gt );
  assert( pExpr->op!=TK_GT || op==OP_Le );
  assert( pExpr->op!=TK_GE || op==OP_Lt );

  switch( pExpr->op ){
    case TK_AND:
    case TK_OR: {
      Expr *pAlt = capdbExprSimplifiedAndOr(pExpr);
      if( pAlt!=pExpr ){
        capdbExprIfFalse(pParse, pAlt, dest, jumpIfNull);
      }else{
        Expr *pFirst, *pSecond;
        if( exprEvalRhsFirst(pExpr) ){
          pFirst = pExpr->pRight;
          pSecond = pExpr->pLeft;
        }else{
          pFirst = pExpr->pLeft;
          pSecond = pExpr->pRight;
        }
        if( pExpr->op==TK_AND ){
          testcase( jumpIfNull==0 );
          capdbExprIfFalse(pParse, pFirst, dest, jumpIfNull);
          capdbExprIfFalse(pParse, pSecond, dest, jumpIfNull);
        }else{
          int d2 = capdbVdbeMakeLabel(pParse);
          testcase( jumpIfNull==0 );
          capdbExprIfTrue(pParse, pFirst, d2,
                            jumpIfNull^CAPDB_JUMPIFNULL);
          capdbExprIfFalse(pParse, pSecond, dest, jumpIfNull);
          capdbVdbeResolveLabel(v, d2);
        }
      }
      break;
    }
    case TK_NOT: {
      testcase( jumpIfNull==0 );
      capdbExprIfTrue(pParse, pExpr->pLeft, dest, jumpIfNull);
      break;
    }
    case TK_TRUTH: {
      int isNot;   /* IS NOT TRUE or IS NOT FALSE */
      int isTrue;  /* IS TRUE or IS NOT TRUE */
      testcase( jumpIfNull==0 );
      isNot = pExpr->op2==TK_ISNOT;
      isTrue = capdbExprTruthValue(pExpr->pRight);
      testcase( isTrue && isNot );
      testcase( !isTrue && isNot );
      if( isTrue ^ isNot ){
        /* IS TRUE and IS NOT FALSE */
        capdbExprIfFalse(pParse, pExpr->pLeft, dest,
                           isNot ? 0 : CAPDB_JUMPIFNULL);

      }else{
        /* IS FALSE and IS NOT TRUE */
        capdbExprIfTrue(pParse, pExpr->pLeft, dest,
                          isNot ? 0 : CAPDB_JUMPIFNULL);
      }
      break;
    }
    case TK_IS:
    case TK_ISNOT:
      testcase( pExpr->op==TK_IS );
      testcase( pExpr->op==TK_ISNOT );
      op = (pExpr->op==TK_IS) ? TK_NE : TK_EQ;
      jumpIfNull = CAPDB_NULLEQ;
      /* no break */ deliberate_fall_through
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_NE:
    case TK_EQ: {
      int addrIsNull;
      if( capdbExprIsVector(pExpr->pLeft) ) goto default_expr;
      if( ExprHasProperty(pExpr, EP_Subquery) && jumpIfNull!=CAPDB_NULLEQ ){
        addrIsNull = exprComputeOperands(pParse, pExpr,
                                   &r1, &r2, &regFree1, &regFree2);
      }else{
        r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
        r2 = capdbExprCodeTemp(pParse, pExpr->pRight, &regFree2);
        addrIsNull = 0;
      }
      codeCompare(pParse, pExpr->pLeft, pExpr->pRight, op,
                  r1, r2, dest, jumpIfNull,ExprHasProperty(pExpr,EP_Commuted));
      assert(TK_LT==OP_Lt); testcase(op==OP_Lt); VdbeCoverageIf(v,op==OP_Lt);
      assert(TK_LE==OP_Le); testcase(op==OP_Le); VdbeCoverageIf(v,op==OP_Le);
      assert(TK_GT==OP_Gt); testcase(op==OP_Gt); VdbeCoverageIf(v,op==OP_Gt);
      assert(TK_GE==OP_Ge); testcase(op==OP_Ge); VdbeCoverageIf(v,op==OP_Ge);
      assert(TK_EQ==OP_Eq); testcase(op==OP_Eq);
      VdbeCoverageIf(v, op==OP_Eq && jumpIfNull!=CAPDB_NULLEQ);
      VdbeCoverageIf(v, op==OP_Eq && jumpIfNull==CAPDB_NULLEQ);
      assert(TK_NE==OP_Ne); testcase(op==OP_Ne);
      VdbeCoverageIf(v, op==OP_Ne && jumpIfNull!=CAPDB_NULLEQ);
      VdbeCoverageIf(v, op==OP_Ne && jumpIfNull==CAPDB_NULLEQ);
      testcase( regFree1==0 );
      testcase( regFree2==0 );
      if( addrIsNull ){
        if( jumpIfNull ){
          capdbVdbeChangeP2(v, addrIsNull, dest);
        }else{
          capdbVdbeJumpHere(v, addrIsNull);
        }
      }
      break;
    }
    case TK_ISNULL:
    case TK_NOTNULL: {
      r1 = capdbExprCodeTemp(pParse, pExpr->pLeft, &regFree1);
      assert( regFree1==0 || regFree1==r1 );
      if( regFree1 ) capdbVdbeTypeofColumn(v, r1);
      capdbVdbeAddOp2(v, op, r1, dest);
      testcase( op==TK_ISNULL );   VdbeCoverageIf(v, op==TK_ISNULL);
      testcase( op==TK_NOTNULL );  VdbeCoverageIf(v, op==TK_NOTNULL);
      break;
    }
    case TK_BETWEEN: {
      testcase( jumpIfNull==0 );
      exprCodeBetween(pParse, pExpr, dest, capdbExprIfFalse, jumpIfNull);
      break;
    }
#ifndef CAPDB_OMIT_SUBQUERY
    case TK_IN: {
      if( jumpIfNull ){
        capdbExprCodeIN(pParse, pExpr, dest, dest);
      }else{
        int destIfNull = capdbVdbeMakeLabel(pParse);
        capdbExprCodeIN(pParse, pExpr, dest, destIfNull);
        capdbVdbeResolveLabel(v, destIfNull);
      }
      break;
    }
#endif
    default: {
    default_expr:
      if( ExprAlwaysFalse(pExpr) ){
        capdbVdbeGoto(v, dest);
      }else if( ExprAlwaysTrue(pExpr) ){
        /* no-op */
      }else{
        r1 = capdbExprCodeTemp(pParse, pExpr, &regFree1);
        capdbVdbeAddOp3(v, OP_IfNot, r1, dest, jumpIfNull!=0);
        VdbeCoverage(v);
        testcase( regFree1==0 );
        testcase( jumpIfNull==0 );
      }
      break;
    }
  }
  capdbReleaseTempReg(pParse, regFree1);
  capdbReleaseTempReg(pParse, regFree2);
}

/*
** Like capdbExprIfFalse() except that a copy is made of pExpr before
** code generation, and that copy is deleted after code generation. This
** ensures that the original pExpr is unchanged.
*/
void capdbExprIfFalseDup(Parse *pParse, Expr *pExpr, int dest,int jumpIfNull){
  capdb *db = pParse->db;
  Expr *pCopy = capdbExprDup(db, pExpr, 0);
  if( db->mallocFailed==0 ){
    capdbExprIfFalse(pParse, pCopy, dest, jumpIfNull);
  }
  capdbExprDelete(db, pCopy);
}

/*
** Expression pVar is guaranteed to be an SQL variable. pExpr may be any
** type of expression.
**
** If pExpr is a simple SQL value - an integer, real, string, blob
** or NULL value - then the VDBE currently being prepared is configured
** to re-prepare each time a new value is bound to variable pVar.
**
** Additionally, if pExpr is a simple SQL value and the value is the
** same as that currently bound to variable pVar, non-zero is returned.
** Otherwise, if the values are not the same or if pExpr is not a simple
** SQL value, zero is returned.
**
** If the CAPDB_EnableQPSG flag is set on the database connection, then
** this routine always returns false.
*/
static CAPDB_NOINLINE int exprCompareVariable(
  const Parse *pParse,
  const Expr *pVar,
  const Expr *pExpr
){
  int res = 2;
  int iVar;
  capdb_value *pL, *pR = 0;
 
  if( pExpr->op==TK_VARIABLE && pVar->iColumn==pExpr->iColumn ){
    return 0;
  }
  if( (pParse->db->flags & CAPDB_EnableQPSG)!=0 ) return 2;
  capdbValueFromExpr(pParse->db, pExpr, CAPDB_UTF8, CAPDB_AFF_BLOB, &pR);
  if( pR ){
    iVar = pVar->iColumn;
    capdbVdbeSetVarmask(pParse->pVdbe, iVar);
    pL = capdbVdbeGetBoundValue(pParse->pReprepare, iVar, CAPDB_AFF_BLOB);
    if( pL ){
      if( capdb_value_type(pL)==CAPDB_TEXT ){
        capdb_value_text(pL); /* Make sure the encoding is UTF-8 */
      }
      res = capdbMemCompare(pL, pR, 0) ? 2 : 0;
    }
    capdbValueFree(pR);
    capdbValueFree(pL);
  }
  return res;
}

/*
** Do a deep comparison of two expression trees.  Return 0 if the two
** expressions are completely identical.  Return 1 if they differ only
** by a COLLATE operator at the top level.  Return 2 if there are differences
** other than the top-level COLLATE operator.
**
** If any subelement of pB has Expr.iTable==(-1) then it is allowed
** to compare equal to an equivalent element in pA with Expr.iTable==iTab.
**
** The pA side might be using TK_REGISTER.  If that is the case and pB is
** not using TK_REGISTER but is otherwise equivalent, then still return 0.
**
** Sometimes this routine will return 2 even if the two expressions
** really are equivalent.  If we cannot prove that the expressions are
** identical, we return 2 just to be safe.  So if this routine
** returns 2, then you do not really know for certain if the two
** expressions are the same.  But if you get a 0 or 1 return, then you
** can be sure the expressions are the same.  In the places where
** this routine is used, it does not hurt to get an extra 2 - that
** just might result in some slightly slower code.  But returning
** an incorrect 0 or 1 could lead to a malfunction.
**
** If pParse is not NULL and CAPDB_EnableQPSG is off then TK_VARIABLE
** terms in pA with bindings in pParse->pReprepare can be matched against
** literals in pB.  The pParse->pVdbe->expmask bitmask is updated for
** each variable referenced.
*/
int capdbExprCompare(
  const Parse *pParse,
  const Expr *pA,
  const Expr *pB,
  int iTab
){
  u32 combinedFlags;
  if( pA==0 || pB==0 ){
    return pB==pA ? 0 : 2;
  }
  if( pParse && pA->op==TK_VARIABLE ){
    return exprCompareVariable(pParse, pA, pB);
  }
  combinedFlags = pA->flags | pB->flags;
  if( combinedFlags & EP_IntValue ){
    if( (pA->flags&pB->flags&EP_IntValue)!=0 && pA->u.iValue==pB->u.iValue ){
      return 0;
    }
    return 2;
  }
  if( pA->op!=pB->op || pA->op==TK_RAISE ){
    if( pA->op==TK_COLLATE && capdbExprCompare(pParse, pA->pLeft,pB,iTab)<2 ){
      return 1;
    }
    if( pB->op==TK_COLLATE && capdbExprCompare(pParse, pA,pB->pLeft,iTab)<2 ){
      return 1;
    }
    if( pA->op==TK_AGG_COLUMN && pB->op==TK_COLUMN
     && pB->iTable<0 && pA->iTable==iTab
    ){
      /* fall through */
    }else{
      return 2;
    }
  }
  assert( !ExprHasProperty(pA, EP_IntValue) );
  assert( !ExprHasProperty(pB, EP_IntValue) );
  if( pA->u.zToken ){
    if( pA->op==TK_FUNCTION || pA->op==TK_AGG_FUNCTION ){
      if( capdbStrICmp(pA->u.zToken,pB->u.zToken)!=0 ) return 2;
#ifndef CAPDB_OMIT_WINDOWFUNC
      assert( pA->op==pB->op );
      if( ExprHasProperty(pA,EP_WinFunc)!=ExprHasProperty(pB,EP_WinFunc) ){
        return 2;
      }
      if( ExprHasProperty(pA,EP_WinFunc) ){
        if( capdbWindowCompare(pParse, pA->y.pWin, pB->y.pWin, 1)!=0 ){
          return 2;
        }
      }
#endif
    }else if( pA->op==TK_NULL ){
      return 0;
    }else if( pA->op==TK_COLLATE ){
      if( capdb_stricmp(pA->u.zToken,pB->u.zToken)!=0 ) return 2;
    }else
    if( pB->u.zToken!=0
     && pA->op!=TK_COLUMN
     && pA->op!=TK_AGG_COLUMN
     && strcmp(pA->u.zToken,pB->u.zToken)!=0
    ){
      return 2;
    }
  }
  if( (pA->flags & (EP_Distinct|EP_Commuted))
     != (pB->flags & (EP_Distinct|EP_Commuted)) ) return 2;
  if( ALWAYS((combinedFlags & EP_TokenOnly)==0) ){
    if( combinedFlags & EP_xIsSelect ) return 2;
    if( (combinedFlags & EP_FixedCol)==0
     && capdbExprCompare(pParse, pA->pLeft, pB->pLeft, iTab) ) return 2;
    if( capdbExprCompare(pParse, pA->pRight, pB->pRight, iTab) ) return 2;
    if( capdbExprListCompare(pA->x.pList, pB->x.pList, iTab) ) return 2;
    if( pA->op!=TK_STRING
     && pA->op!=TK_TRUEFALSE
     && ALWAYS((combinedFlags & EP_Reduced)==0)
    ){
      if( pA->iColumn!=pB->iColumn ) return 2;
      if( pA->op2!=pB->op2 && pA->op==TK_TRUTH ) return 2;
      if( pA->op!=TK_IN && pA->iTable!=pB->iTable && pA->iTable!=iTab ){
        return 2;
      }
    }
  }
  return 0;
}

/*
** Compare two ExprList objects.  Return 0 if they are identical, 1
** if they are certainly different, or 2 if it is not possible to
** determine if they are identical or not.
**
** If any subelement of pB has Expr.iTable==(-1) then it is allowed
** to compare equal to an equivalent element in pA with Expr.iTable==iTab.
**
** This routine might return non-zero for equivalent ExprLists.  The
** only consequence will be disabled optimizations.  But this routine
** must never return 0 if the two ExprList objects are different, or
** a malfunction will result.
**
** Two NULL pointers are considered to be the same.  But a NULL pointer
** always differs from a non-NULL pointer.
*/
int capdbExprListCompare(const ExprList *pA, const ExprList *pB, int iTab){
  int i;
  if( pA==0 && pB==0 ) return 0;
  if( pA==0 || pB==0 ) return 1;
  if( pA->nExpr!=pB->nExpr ) return 1;
  for(i=0; i<pA->nExpr; i++){
    int res;
    Expr *pExprA = pA->a[i].pExpr;
    Expr *pExprB = pB->a[i].pExpr;
    if( pA->a[i].fg.sortFlags!=pB->a[i].fg.sortFlags ) return 1;
    if( (res = capdbExprCompare(0, pExprA, pExprB, iTab)) ) return res;
  }
  return 0;
}

/*
** Like capdbExprCompare() except COLLATE operators at the top-level
** are ignored.
*/
int capdbExprCompareSkip(Expr *pA,Expr *pB, int iTab){
  return capdbExprCompare(0,
             capdbExprSkipCollate(pA),
             capdbExprSkipCollate(pB),
             iTab);
}

/*
** Return non-zero if Expr p can only be true if pNN is not NULL.
**
** Or if seenNot is true, return non-zero if Expr p can only be
** non-NULL if pNN is not NULL
*/
static int exprImpliesNotNull(
  const Parse *pParse,/* Parsing context */
  const Expr *p,      /* The expression to be checked */
  const Expr *pNN,    /* The expression that is NOT NULL */
  int iTab,           /* Table being evaluated */
  int seenNot         /* Return true only if p can be any non-NULL value */
){
  assert( p );
  assert( pNN );
  if( capdbExprCompare(pParse, p, pNN, iTab)==0 ){
    return pNN->op!=TK_NULL;
  }
  switch( p->op ){
    case TK_IN: {
      if( seenNot && ExprHasProperty(p, EP_xIsSelect) ) return 0;
      assert( ExprUseXSelect(p) || (p->x.pList!=0 && p->x.pList->nExpr>0) );
      return exprImpliesNotNull(pParse, p->pLeft, pNN, iTab, 1);
    }
    case TK_BETWEEN: {
      ExprList *pList;
      assert( ExprUseXList(p) );
      pList = p->x.pList;
      assert( pList!=0 );
      assert( pList->nExpr==2 );
      if( seenNot ) return 0;
      if( exprImpliesNotNull(pParse, pList->a[0].pExpr, pNN, iTab, 1)
       || exprImpliesNotNull(pParse, pList->a[1].pExpr, pNN, iTab, 1)
      ){
        return 1;
      }
      return exprImpliesNotNull(pParse, p->pLeft, pNN, iTab, 1);
    }
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE:
    case TK_PLUS:
    case TK_MINUS:
    case TK_BITOR:
    case TK_LSHIFT:
    case TK_RSHIFT:
    case TK_CONCAT:
      seenNot = 1;
      /* no break */ deliberate_fall_through
    case TK_STAR:
    case TK_REM:
    case TK_BITAND:
    case TK_SLASH: {
      if( exprImpliesNotNull(pParse, p->pRight, pNN, iTab, seenNot) ) return 1;
      /* no break */ deliberate_fall_through
    }
    case TK_SPAN:
    case TK_COLLATE:
    case TK_UPLUS:
    case TK_UMINUS: {
      return exprImpliesNotNull(pParse, p->pLeft, pNN, iTab, seenNot);
    }
    case TK_TRUTH: {
      if( seenNot ) return 0;
      if( p->op2!=TK_IS ) return 0;
      return exprImpliesNotNull(pParse, p->pLeft, pNN, iTab, 1);
    }
    case TK_BITNOT:
    case TK_NOT: {
      return exprImpliesNotNull(pParse, p->pLeft, pNN, iTab, 1);
    }
  }
  return 0;
}

/*
** Return true if the boolean value of the expression is always either
** FALSE or NULL.
*/
static int capdbExprIsNotTrue(Expr *pExpr){
  int v;
  if( pExpr->op==TK_NULL ) return 1;
  if( pExpr->op==TK_TRUEFALSE && capdbExprTruthValue(pExpr)==0 ) return 1;
  v = 1;
  if( capdbExprIsInteger(pExpr, &v, 0) && v==0 ) return 1;
  return 0;
}

/*
** Return true if the expression is one of the following:
**
**    CASE WHEN x THEN y END
**    CASE WHEN x THEN y ELSE NULL END
**    CASE WHEN x THEN y ELSE false END
**    iif(x,y)
**    iif(x,y,NULL)
**    iif(x,y,false)
*/
static int capdbExprIsIIF(capdb *db, const Expr *pExpr){
  ExprList *pList;
  if( pExpr->op==TK_FUNCTION ){
    const char *z = pExpr->u.zToken;
    FuncDef *pDef;
    if( (z[0]!='i' && z[0]!='I') ) return 0;
    if( pExpr->x.pList==0 ) return 0;
    pDef = capdbFindFunction(db, z, pExpr->x.pList->nExpr, ENC(db), 0);
#ifdef CAPDB_ENABLE_UNKNOWN_SQL_FUNCTION
    if( pDef==0 ) return 0;
#else
    if( NEVER(pDef==0) ) return 0;
#endif
    if( (pDef->funcFlags & CAPDB_FUNC_INLINE)==0 ) return 0;
    if( CAPDB_PTR_TO_INT(pDef->pUserData)!=INLINEFUNC_iif ) return 0;
  }else if( pExpr->op==TK_CASE ){
    if( pExpr->pLeft!=0 ) return 0;
  }else{
    return 0;
  }
  pList = pExpr->x.pList;
  assert( pList!=0 );
  if( pList->nExpr==2 ) return 1;
  if( pList->nExpr==3 && capdbExprIsNotTrue(pList->a[2].pExpr) ) return 1;
  return 0;
}

/*
** Return true if we can prove the pE2 will always be true if pE1 is
** true.  Return false if we cannot complete the proof or if pE2 might
** be false.  Examples:
**
**     pE1: x==5        pE2: x==5             Result: true
**     pE1: x>0         pE2: x==5             Result: false
**     pE1: x=21        pE2: x=21 OR y=43     Result: true
**     pE1: x!=123      pE2: x IS NOT NULL    Result: true
**     pE1: x!=?1       pE2: x IS NOT NULL    Result: true
**     pE1: x IS NULL   pE2: x IS NOT NULL    Result: false
**     pE1: x IS ?2     pE2: x IS NOT NULL    Result: false
**     pE1: iif(x,y)    pE2: x                Result: true
**     PE1: iif(x,y,0)  pE2: x                Result: true
**
** When comparing TK_COLUMN nodes between pE1 and pE2, if pE2 has
** Expr.iTable<0 then assume a table number given by iTab.
**
** If pParse is not NULL, then the values of bound variables in pE1 are
** compared against literal values in pE2 and pParse->pVdbe->expmask is
** modified to record which bound variables are referenced.  If pParse
** is NULL, then false will be returned if pE1 contains any bound variables.
**
** When in doubt, return false.  Returning true might give a performance
** improvement.  Returning false might cause a performance reduction, but
** it will always give the correct answer and is hence always safe.
*/
int capdbExprImpliesExpr(
  const Parse *pParse,
  const Expr *pE1,
  const Expr *pE2,
  int iTab
){
  if( capdbExprCompare(pParse, pE1, pE2, iTab)==0 ){
    return 1;
  }
  if( pE2->op==TK_OR
   && (capdbExprImpliesExpr(pParse, pE1, pE2->pLeft, iTab)
             || capdbExprImpliesExpr(pParse, pE1, pE2->pRight, iTab) )
  ){
    return 1;
  }
  if( pE2->op==TK_NOTNULL
   && exprImpliesNotNull(pParse, pE1, pE2->pLeft, iTab, 0)
  ){
    return 1;
  }
  if( capdbExprIsIIF(pParse->db, pE1) ){
    return capdbExprImpliesExpr(pParse,pE1->x.pList->a[0].pExpr,pE2,iTab);
  }
  return 0;
}

/* This is a helper function to impliesNotNullRow().  In this routine,
** set pWalker->eCode to one only if *both* of the input expressions
** separately have the implies-not-null-row property.
*/
static void bothImplyNotNullRow(Walker *pWalker, Expr *pE1, Expr *pE2){
  if( pWalker->eCode==0 ){
    capdbWalkExpr(pWalker, pE1);
    if( pWalker->eCode ){
      pWalker->eCode = 0;
      capdbWalkExpr(pWalker, pE2);
    }
  }
}

/*
** This is the Expr node callback for capdbExprImpliesNonNullRow().
** If the expression node requires that the table at pWalker->iCur
** have one or more non-NULL column, then set pWalker->eCode to 1 and abort.
**
** pWalker->mWFlags is non-zero if this inquiry is being undertaking on
** behalf of a RIGHT JOIN (or FULL JOIN).  That makes a difference when
** evaluating terms in the ON clause of an inner join.
**
** This routine controls an optimization.  False positives (setting
** pWalker->eCode to 1 when it should not be) are deadly, but false-negatives
** (never setting pWalker->eCode) is a harmless missed optimization.
*/
static int impliesNotNullRow(Walker *pWalker, Expr *pExpr){
  testcase( pExpr->op==TK_AGG_COLUMN );
  testcase( pExpr->op==TK_AGG_FUNCTION );
  if( ExprHasProperty(pExpr, EP_OuterON) ) return WRC_Prune;
  if( ExprHasProperty(pExpr, EP_InnerON) && pWalker->mWFlags ){
    /* If iCur is used in an inner-join ON clause to the left of a
    ** RIGHT JOIN, that does *not* mean that the table must be non-null.
    ** But it is difficult to check for that condition precisely.
    ** To keep things simple, any use of iCur from any inner-join is
    ** ignored while attempting to simplify a RIGHT JOIN. */
    return WRC_Prune;
  }
  switch( pExpr->op ){
    case TK_ISNOT:
    case TK_ISNULL:
    case TK_NOTNULL:
    case TK_IS:
    case TK_VECTOR:
    case TK_FUNCTION:
    case TK_TRUTH:
    case TK_CASE:
      testcase( pExpr->op==TK_ISNOT );
      testcase( pExpr->op==TK_ISNULL );
      testcase( pExpr->op==TK_NOTNULL );
      testcase( pExpr->op==TK_IS );
      testcase( pExpr->op==TK_VECTOR );
      testcase( pExpr->op==TK_FUNCTION );
      testcase( pExpr->op==TK_TRUTH );
      testcase( pExpr->op==TK_CASE );
      return WRC_Prune;

    case TK_COLUMN:
      if( pWalker->u.iCur==pExpr->iTable ){
        pWalker->eCode = 1;
        return WRC_Abort;
      }
      return WRC_Prune;

    case TK_OR:
    case TK_AND:
      /* Both sides of an AND or OR must separately imply non-null-row.
      ** Consider these cases:
      **    1.  NOT (x AND y)
      **    2.  x OR y
      ** If only one of x or y is non-null-row, then the overall expression
      ** can be true if the other arm is false (case 1) or true (case 2).
      */
      testcase( pExpr->op==TK_OR );
      testcase( pExpr->op==TK_AND );
      bothImplyNotNullRow(pWalker, pExpr->pLeft, pExpr->pRight);
      return WRC_Prune;
       
    case TK_IN:
      /* Beware of "x NOT IN ()" and "x NOT IN (SELECT 1 WHERE false)",
      ** both of which can be true.  But apart from these cases, if
      ** the left-hand side of the IN is NULL then the IN itself will be
      ** NULL. */
      if( ExprUseXList(pExpr) && ALWAYS(pExpr->x.pList->nExpr>0) ){
        capdbWalkExpr(pWalker, pExpr->pLeft);
      }
      return WRC_Prune;

    case TK_BETWEEN:
      /* In "x NOT BETWEEN y AND z" either x must be non-null-row or else
      ** both y and z must be non-null row */
      assert( ExprUseXList(pExpr) );
      assert( pExpr->x.pList->nExpr==2 );
      capdbWalkExpr(pWalker, pExpr->pLeft);
      bothImplyNotNullRow(pWalker, pExpr->x.pList->a[0].pExpr,
                                   pExpr->x.pList->a[1].pExpr);
      return WRC_Prune;

    /* Virtual tables are allowed to use constraints like x=NULL.  So
    ** a term of the form x=y does not prove that y is not null if x
    ** is the column of a virtual table */
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE: {
      Expr *pLeft = pExpr->pLeft;
      Expr *pRight = pExpr->pRight;
      testcase( pExpr->op==TK_EQ );
      testcase( pExpr->op==TK_NE );
      testcase( pExpr->op==TK_LT );
      testcase( pExpr->op==TK_LE );
      testcase( pExpr->op==TK_GT );
      testcase( pExpr->op==TK_GE );
      /* The y.pTab=0 assignment in wherecode.c always happens after the
      ** impliesNotNullRow() test */
      assert( pLeft->op!=TK_COLUMN || ExprUseYTab(pLeft) );
      assert( pRight->op!=TK_COLUMN || ExprUseYTab(pRight) );
      if( (pLeft->op==TK_COLUMN
           && ALWAYS(pLeft->y.pTab!=0)
           && IsVirtual(pLeft->y.pTab))
       || (pRight->op==TK_COLUMN
           && ALWAYS(pRight->y.pTab!=0)
           && IsVirtual(pRight->y.pTab))
      ){
        return WRC_Prune;
      }
      /* no break */ deliberate_fall_through
    }
    default:
      return WRC_Continue;
  }
}

/*
** Return true (non-zero) if expression p can only be true if at least
** one column of table iTab is non-null.  In other words, return true
** if expression p will always be NULL or false if every column of iTab
** is NULL.
**
** False negatives are acceptable.  In other words, it is ok to return
** zero even if expression p will never be true of every column of iTab
** is NULL.  A false negative is merely a missed optimization opportunity.
**
** False positives are not allowed, however.  A false positive may result
** in an incorrect answer.
**
** Terms of p that are marked with EP_OuterON (and hence that come from
** the ON or USING clauses of OUTER JOINS) are excluded from the analysis.
**
** This routine is used to check if a LEFT JOIN can be converted into
** an ordinary JOIN.  The p argument is the WHERE clause.  If the WHERE
** clause requires that some column of the right table of the LEFT JOIN
** be non-NULL, then the LEFT JOIN can be safely converted into an
** ordinary join.
*/
int capdbExprImpliesNonNullRow(Expr *p, int iTab, int isRJ){
  Walker w;
  p = capdbExprSkipCollateAndLikely(p);
  if( p==0 ) return 0;
  if( p->op==TK_NOTNULL ){
    p = p->pLeft;
  }else{
    while( p->op==TK_AND ){
      if( capdbExprImpliesNonNullRow(p->pLeft, iTab, isRJ) ) return 1;
      p = p->pRight;
    }
  }
  w.xExprCallback = impliesNotNullRow;
  w.xSelectCallback = 0;
  w.xSelectCallback2 = 0;
  w.eCode = 0;
  w.mWFlags = isRJ!=0;
  w.u.iCur = iTab;
  capdbWalkExpr(&w, p);
  return w.eCode;
}

/*
** An instance of the following structure is used by the tree walker
** to determine if an expression can be evaluated by reference to the
** index only, without having to do a search for the corresponding
** table entry.  The IdxCover.pIdx field is the index.  IdxCover.iCur
** is the cursor for the table.
*/
struct IdxCover {
  Index *pIdx;     /* The index to be tested for coverage */
  int iCur;        /* Cursor number for the table corresponding to the index */
};

/*
** Check to see if there are references to columns in table
** pWalker->u.pIdxCover->iCur can be satisfied using the index
** pWalker->u.pIdxCover->pIdx.
*/
static int exprIdxCover(Walker *pWalker, Expr *pExpr){
  if( pExpr->op==TK_COLUMN
   && pExpr->iTable==pWalker->u.pIdxCover->iCur
   && capdbTableColumnToIndex(pWalker->u.pIdxCover->pIdx, pExpr->iColumn)<0
  ){
    pWalker->eCode = 1;
    return WRC_Abort;
  }
  return WRC_Continue;
}

/*
** Determine if an index pIdx on table with cursor iCur contains will
** the expression pExpr.  Return true if the index does cover the
** expression and false if the pExpr expression references table columns
** that are not found in the index pIdx.
**
** An index covering an expression means that the expression can be
** evaluated using only the index and without having to lookup the
** corresponding table entry.
*/
int capdbExprCoveredByIndex(
  Expr *pExpr,        /* The index to be tested */
  int iCur,           /* The cursor number for the corresponding table */
  Index *pIdx         /* The index that might be used for coverage */
){
  Walker w;
  struct IdxCover xcov;
  memset(&w, 0, sizeof(w));
  xcov.iCur = iCur;
  xcov.pIdx = pIdx;
  w.xExprCallback = exprIdxCover;
  w.u.pIdxCover = &xcov;
  capdbWalkExpr(&w, pExpr);
  return !w.eCode;
}


/* Structure used to pass information throughout the Walker in order to
** implement capdbReferencesSrcList().
*/
struct RefSrcList {
  capdb *db;         /* Database connection used for capdbDbRealloc() */
  SrcList *pRef;       /* Looking for references to these tables */
  i64 nExclude;        /* Number of tables to exclude from the search */
  int *aiExclude;      /* Cursor IDs for tables to exclude from the search */
};

/*
** Walker SELECT callbacks for capdbReferencesSrcList().
**
** When entering a new subquery on the pExpr argument, add all FROM clause
** entries for that subquery to the exclude list.
**
** When leaving the subquery, remove those entries from the exclude list.
*/
static int selectRefEnter(Walker *pWalker, Select *pSelect){
  struct RefSrcList *p = pWalker->u.pRefSrcList;
  SrcList *pSrc = pSelect->pSrc;
  i64 i, j;
  int *piNew;
  if( pSrc->nSrc==0 ) return WRC_Continue;
  j = p->nExclude;
  p->nExclude += pSrc->nSrc;
  piNew = capdbDbRealloc(p->db, p->aiExclude, p->nExclude*sizeof(int));
  if( piNew==0 ){
    p->nExclude = 0;
    return WRC_Abort;
  }else{
    p->aiExclude = piNew;
  }
  for(i=0; i<pSrc->nSrc; i++, j++){
     p->aiExclude[j] = pSrc->a[i].iCursor;
  }
  return WRC_Continue;
}
static void selectRefLeave(Walker *pWalker, Select *pSelect){
  struct RefSrcList *p = pWalker->u.pRefSrcList;
  SrcList *pSrc = pSelect->pSrc;
  if( p->nExclude ){
    assert( p->nExclude>=pSrc->nSrc );
    p->nExclude -= pSrc->nSrc;
  }
}

/* This is the Walker EXPR callback for capdbReferencesSrcList().
**
** Set the 0x01 bit of pWalker->eCode if there is a reference to any
** of the tables shown in RefSrcList.pRef.
**
** Set the 0x02 bit of pWalker->eCode if there is a reference to a
** table is in neither RefSrcList.pRef nor RefSrcList.aiExclude.
*/
static int exprRefToSrcList(Walker *pWalker, Expr *pExpr){
  if( pExpr->op==TK_COLUMN
   || pExpr->op==TK_AGG_COLUMN
  ){
    int i;
    struct RefSrcList *p = pWalker->u.pRefSrcList;
    SrcList *pSrc = p->pRef;
    int nSrc = pSrc ? pSrc->nSrc : 0;
    for(i=0; i<nSrc; i++){
      if( pExpr->iTable==pSrc->a[i].iCursor ){
        pWalker->eCode |= 1;
        return WRC_Continue;
      }
    }
    for(i=0; i<p->nExclude && p->aiExclude[i]!=pExpr->iTable; i++){}
    if( i>=p->nExclude ){
      pWalker->eCode |= 2;
    }
  }
  return WRC_Continue;
}

/*
** Check to see if pExpr references any tables in pSrcList.
** Possible return values:
**
**    1         pExpr does references a table in pSrcList.
**
**    0         pExpr references some table that is not defined in either
**              pSrcList or in subqueries of pExpr itself.
**
**   -1         pExpr only references no tables at all, or it only
**              references tables defined in subqueries of pExpr itself.
**
** As currently used, pExpr is always an aggregate function call.  That
** fact is exploited for efficiency.
*/
int capdbReferencesSrcList(Parse *pParse, Expr *pExpr, SrcList *pSrcList){
  Walker w;
  struct RefSrcList x;
  assert( pParse->db!=0 );
  memset(&w, 0, sizeof(w));
  memset(&x, 0, sizeof(x));
  w.xExprCallback = exprRefToSrcList;
  w.xSelectCallback = selectRefEnter;
  w.xSelectCallback2 = selectRefLeave;
  w.u.pRefSrcList = &x;
  x.db = pParse->db;
  x.pRef = pSrcList;
  assert( pExpr->op==TK_AGG_FUNCTION );
  assert( ExprUseXList(pExpr) );
  capdbWalkExprList(&w, pExpr->x.pList);
  if( pExpr->pLeft ){
    assert( pExpr->pLeft->op==TK_ORDER );
    assert( ExprUseXList(pExpr->pLeft) );
    assert( pExpr->pLeft->x.pList!=0 );
    capdbWalkExprList(&w, pExpr->pLeft->x.pList);
  }
#ifndef CAPDB_OMIT_WINDOWFUNC
  if( ExprHasProperty(pExpr, EP_WinFunc) ){
    capdbWalkExpr(&w, pExpr->y.pWin->pFilter);
  }
#endif
  if( x.aiExclude ) capdbDbNNFreeNN(pParse->db, x.aiExclude);
  if( w.eCode & 0x01 ){
    return 1;
  }else if( w.eCode ){
    return 0;
  }else{
    return -1;
  }
}

/*
** This is a Walker expression node callback.
**
** For Expr nodes that contain pAggInfo pointers, make sure the AggInfo
** object that is referenced does not refer directly to the Expr.  If
** it does, make a copy.  This is done because the pExpr argument is
** subject to change.
**
** The copy is scheduled for deletion using the capdbExprDeferredDelete()
** which builds on the capdbParserAddCleanup() mechanism.
*/
static int agginfoPersistExprCb(Walker *pWalker, Expr *pExpr){
  if( ALWAYS(!ExprHasProperty(pExpr, EP_TokenOnly|EP_Reduced))
   && pExpr->pAggInfo!=0
  ){
    AggInfo *pAggInfo = pExpr->pAggInfo;
    int iAgg = pExpr->iAgg;
    Parse *pParse = pWalker->pParse;
    capdb *db = pParse->db;
    assert( iAgg>=0 );
    if( pExpr->op!=TK_AGG_FUNCTION ){
      if( iAgg<pAggInfo->nColumn
       && pAggInfo->aCol[iAgg].pCExpr==pExpr
      ){
        pExpr = capdbExprDup(db, pExpr, 0);
        if( pExpr && !capdbExprDeferredDelete(pParse, pExpr) ){
          pAggInfo->aCol[iAgg].pCExpr = pExpr;
        }
      }
    }else{
      assert( pExpr->op==TK_AGG_FUNCTION );
      if( ALWAYS(iAgg<pAggInfo->nFunc)
       && pAggInfo->aFunc[iAgg].pFExpr==pExpr
      ){
        pExpr = capdbExprDup(db, pExpr, 0);
        if( pExpr && !capdbExprDeferredDelete(pParse, pExpr) ){
          pAggInfo->aFunc[iAgg].pFExpr = pExpr;
        }
      }
    }
  }
  return WRC_Continue;
}

/*
** Initialize a Walker object so that will persist AggInfo entries referenced
** by the tree that is walked.
*/
void capdbAggInfoPersistWalkerInit(Walker *pWalker, Parse *pParse){
  memset(pWalker, 0, sizeof(*pWalker));
  pWalker->pParse = pParse;
  pWalker->xExprCallback = agginfoPersistExprCb;
  pWalker->xSelectCallback = capdbSelectWalkNoop;
}

/*
** Add a new element to the pAggInfo->aCol[] array.  Return the index of
** the new element.  Return a negative number if malloc fails.
*/
static int addAggInfoColumn(capdb *db, AggInfo *pInfo){
  int i;
  pInfo->aCol = capdbArrayAllocate(
       db,
       pInfo->aCol,
       sizeof(pInfo->aCol[0]),
       &pInfo->nColumn,
       &i
  );
  return i;
}   

/*
** Add a new element to the pAggInfo->aFunc[] array.  Return the index of
** the new element.  Return a negative number if malloc fails.
*/
static int addAggInfoFunc(capdb *db, AggInfo *pInfo){
  int i;
  pInfo->aFunc = capdbArrayAllocate(
       db,
       pInfo->aFunc,
       sizeof(pInfo->aFunc[0]),
       &pInfo->nFunc,
       &i
  );
  return i;
}

/*
** Search the AggInfo object for an aCol[] entry that has iTable and iColumn.
** Return the index in aCol[] of the entry that describes that column.
**
** If no prior entry is found, create a new one and return -1.  The
** new column will have an index of pAggInfo->nColumn-1.
*/
static void findOrCreateAggInfoColumn(
  Parse *pParse,       /* Parsing context */
  AggInfo *pAggInfo,   /* The AggInfo object to search and/or modify */
  Expr *pExpr          /* Expr describing the column to find or insert */
){
  struct AggInfo_col *pCol;
  int k;
  int mxTerm = pParse->db->aLimit[CAPDB_LIMIT_COLUMN];

  assert( mxTerm <= SMXV(i16) );
  assert( pAggInfo->iFirstReg==0 );
  pCol = pAggInfo->aCol;
  for(k=0; k<pAggInfo->nColumn; k++, pCol++){
    if( pCol->pCExpr==pExpr ) return;
    if( pCol->iTable==pExpr->iTable
     && pCol->iColumn==pExpr->iColumn
     && pExpr->op!=TK_IF_NULL_ROW
    ){
      goto fix_up_expr;
    }
  }
  k = addAggInfoColumn(pParse->db, pAggInfo);
  if( k<0 ){
    /* OOM on resize */
    assert( pParse->db->mallocFailed );
    return;
  }
  if( k>mxTerm ){
    capdbErrorMsg(pParse, "more than %d aggregate terms", mxTerm);
    k = mxTerm;
  }
  pCol = &pAggInfo->aCol[k];
  assert( ExprUseYTab(pExpr) );
  pCol->pTab = pExpr->y.pTab;
  pCol->iTable = pExpr->iTable;
  pCol->iColumn = pExpr->iColumn;
  pCol->iSorterColumn = -1;
  pCol->pCExpr = pExpr;
  if( pAggInfo->pGroupBy && pExpr->op!=TK_IF_NULL_ROW ){
    int j, n;
    ExprList *pGB = pAggInfo->pGroupBy;
    struct ExprList_item *pTerm = pGB->a;
    n = pGB->nExpr;
    for(j=0; j<n; j++, pTerm++){
      Expr *pE = pTerm->pExpr;
      if( pE->op==TK_COLUMN
       && pE->iTable==pExpr->iTable
       && pE->iColumn==pExpr->iColumn
      ){
        pCol->iSorterColumn = j;
        break;
      }
    }
  }
  if( pCol->iSorterColumn<0 ){
    pCol->iSorterColumn = pAggInfo->nSortingColumn++;
  }
fix_up_expr:
  ExprSetVVAProperty(pExpr, EP_NoReduce);
  assert( pExpr->pAggInfo==0 || pExpr->pAggInfo==pAggInfo );
  pExpr->pAggInfo = pAggInfo;
  if( pExpr->op==TK_COLUMN ){
    pExpr->op = TK_AGG_COLUMN;
  }
  assert( k <= SMXV(pExpr->iAgg) );
  pExpr->iAgg = (i16)k;
}

/*
** This is the xExprCallback for a tree walker.  It is used to
** implement capdbExprAnalyzeAggregates().  See capdbExprAnalyzeAggregates
** for additional information.
*/
static int analyzeAggregate(Walker *pWalker, Expr *pExpr){
  int i;
  NameContext *pNC = pWalker->u.pNC;
  Parse *pParse = pNC->pParse;
  SrcList *pSrcList = pNC->pSrcList;
  AggInfo *pAggInfo = pNC->uNC.pAggInfo;

  assert( pNC->ncFlags & NC_UAggInfo );
  assert( pAggInfo->iFirstReg==0 );
  switch( pExpr->op ){
    default: {
      IndexedExpr *pIEpr;
      Expr tmp;
      assert( pParse->iSelfTab==0 );
      if( (pNC->ncFlags & NC_InAggFunc)==0 ) break;
      if( pParse->pIdxEpr==0 ) break;
      for(pIEpr=pParse->pIdxEpr; pIEpr; pIEpr=pIEpr->pIENext){
        int iDataCur = pIEpr->iDataCur;
        if( iDataCur<0 ) continue;
        if( capdbExprCompare(0, pExpr, pIEpr->pExpr, iDataCur)==0 ) break;
      }
      if( pIEpr==0 ) break;
      if( NEVER(!ExprUseYTab(pExpr)) ) break;
      for(i=0; i<pSrcList->nSrc; i++){
         if( pSrcList->a[i].iCursor==pIEpr->iDataCur ){
           testcase( i>0 );
           break;
         }
      }
      if( i>=pSrcList->nSrc ) break;
      if( NEVER(pExpr->pAggInfo!=0) ) break; /* Resolved by outer context */
      if( pParse->nErr ){ return WRC_Abort; }

      /* If we reach this point, it means that expression pExpr can be
      ** translated into a reference to an index column as described by
      ** pIEpr.
      */
      memset(&tmp, 0, sizeof(tmp));
      tmp.op = TK_AGG_COLUMN;
      tmp.iTable = pIEpr->iIdxCur;
      tmp.iColumn = pIEpr->iIdxCol;
      findOrCreateAggInfoColumn(pParse, pAggInfo, &tmp);
      if( pParse->nErr ){ return WRC_Abort; }
      assert( pAggInfo->aCol!=0 );
      assert( tmp.iAgg<pAggInfo->nColumn );
      pAggInfo->aCol[tmp.iAgg].pCExpr = pExpr;
      pExpr->pAggInfo = pAggInfo;
      pExpr->iAgg = tmp.iAgg;
      return WRC_Prune;
    }
    case TK_IF_NULL_ROW:
    case TK_AGG_COLUMN:
    case TK_COLUMN: {
      testcase( pExpr->op==TK_AGG_COLUMN );
      testcase( pExpr->op==TK_COLUMN );
      testcase( pExpr->op==TK_IF_NULL_ROW );
      /* Check to see if the column is in one of the tables in the FROM
      ** clause of the aggregate query */
      if( ALWAYS(pSrcList!=0) ){
        SrcItem *pItem = pSrcList->a;
        for(i=0; i<pSrcList->nSrc; i++, pItem++){
          assert( !ExprHasProperty(pExpr, EP_TokenOnly|EP_Reduced) );
          if( pExpr->iTable==pItem->iCursor ){
            findOrCreateAggInfoColumn(pParse, pAggInfo, pExpr);
            break;
          } /* endif pExpr->iTable==pItem->iCursor */
        } /* end loop over pSrcList */
      }
      return WRC_Continue;
    }
    case TK_AGG_FUNCTION: {
      if( (pNC->ncFlags & NC_InAggFunc)==0
       && pWalker->walkerDepth==pExpr->op2
       && pExpr->pAggInfo==0
      ){
        /* Check to see if pExpr is a duplicate of another aggregate
        ** function that is already in the pAggInfo structure
        */
        struct AggInfo_func *pItem = pAggInfo->aFunc;
        int mxTerm = pParse->db->aLimit[CAPDB_LIMIT_COLUMN];
        assert( mxTerm <= SMXV(i16) );
        for(i=0; i<pAggInfo->nFunc; i++, pItem++){
          if( NEVER(pItem->pFExpr==pExpr) ) break;
          if( capdbExprCompare(0, pItem->pFExpr, pExpr, -1)==0 ){
            break;
          }
        }
        if( i>mxTerm ){
          capdbErrorMsg(pParse, "more than %d aggregate terms", mxTerm);
          i = mxTerm;
          assert( i<pAggInfo->nFunc );
        }else if( i>=pAggInfo->nFunc ){
          /* pExpr is original.  Make a new entry in pAggInfo->aFunc[]
          */
          u8 enc = ENC(pParse->db);
          i = addAggInfoFunc(pParse->db, pAggInfo);
          if( i>=0 ){
            int nArg;
            assert( !ExprHasProperty(pExpr, EP_xIsSelect) );
            pItem = &pAggInfo->aFunc[i];
            pItem->pFExpr = pExpr;
            assert( ExprUseUToken(pExpr) );
            nArg = pExpr->x.pList ? pExpr->x.pList->nExpr : 0;
            pItem->pFunc = capdbFindFunction(pParse->db,
                                         pExpr->u.zToken, nArg, enc, 0);
            assert( pItem->bOBUnique==0 );
            if( pExpr->pLeft
             && (pItem->pFunc->funcFlags & CAPDB_FUNC_NEEDCOLL)==0
            ){
              /* The NEEDCOLL test above causes any ORDER BY clause on
              ** aggregate min() or max() to be ignored. */
              ExprList *pOBList;
              assert( nArg>0 );
              assert( pExpr->pLeft->op==TK_ORDER );
              assert( ExprUseXList(pExpr->pLeft) );
              pItem->iOBTab = pParse->nTab++;
              pOBList = pExpr->pLeft->x.pList;
              assert( pOBList->nExpr>0 );
              assert( pItem->bOBUnique==0 );
              if( pOBList->nExpr==1
               && nArg==1
               && capdbExprCompare(0,pOBList->a[0].pExpr,
                               pExpr->x.pList->a[0].pExpr,0)==0
              ){
                pItem->bOBPayload = 0;
                pItem->bOBUnique = ExprHasProperty(pExpr, EP_Distinct);
              }else{
                pItem->bOBPayload = 1;
              }
              pItem->bUseSubtype =
                    (pItem->pFunc->funcFlags & CAPDB_SUBTYPE)!=0;
            }else{
              pItem->iOBTab = -1;
            }
            if( ExprHasProperty(pExpr, EP_Distinct) && !pItem->bOBUnique ){
              pItem->iDistinct = pParse->nTab++;
            }else{
              pItem->iDistinct = -1;
            }
          }
        }
        /* Make pExpr point to the appropriate pAggInfo->aFunc[] entry
        */
        assert( !ExprHasProperty(pExpr, EP_TokenOnly|EP_Reduced) );
        ExprSetVVAProperty(pExpr, EP_NoReduce);
        assert( i <= SMXV(pExpr->iAgg) );
        pExpr->iAgg = (i16)i;
        pExpr->pAggInfo = pAggInfo;
        return WRC_Prune;
      }else{
        return WRC_Continue;
      }
    }
  }
  return WRC_Continue;
}

/*
** Analyze the pExpr expression looking for aggregate functions and
** for variables that need to be added to AggInfo object that pNC->pAggInfo
** points to.  Additional entries are made on the AggInfo object as
** necessary.
**
** This routine should only be called after the expression has been
** analyzed by capdbResolveExprNames().
*/
void capdbExprAnalyzeAggregates(NameContext *pNC, Expr *pExpr){
  Walker w;
  w.xExprCallback = analyzeAggregate;
  w.xSelectCallback = capdbWalkerDepthIncrease;
  w.xSelectCallback2 = capdbWalkerDepthDecrease;
  w.walkerDepth = 0;
  w.u.pNC = pNC;
  w.pParse = 0;
  assert( pNC->pSrcList!=0 );
  capdbWalkExpr(&w, pExpr);
}

/*
** Call capdbExprAnalyzeAggregates() for every expression in an
** expression list.  Return the number of errors.
**
** If an error is found, the analysis is cut short.
*/
void capdbExprAnalyzeAggList(NameContext *pNC, ExprList *pList){
  struct ExprList_item *pItem;
  int i;
  if( pList ){
    for(pItem=pList->a, i=0; i<pList->nExpr; i++, pItem++){
      capdbExprAnalyzeAggregates(pNC, pItem->pExpr);
    }
  }
}

/*
** Allocate a single new register for use to hold some intermediate result.
*/
int capdbGetTempReg(Parse *pParse){
  if( pParse->nTempReg==0 ){
    return ++pParse->nMem;
  }
  return pParse->aTempReg[--pParse->nTempReg];
}

/*
** Deallocate a register, making available for reuse for some other
** purpose.
*/
void capdbReleaseTempReg(Parse *pParse, int iReg){
  if( iReg ){
    capdbVdbeReleaseRegisters(pParse, iReg, 1, 0, 0);
    if( pParse->nTempReg<ArraySize(pParse->aTempReg) ){
      pParse->aTempReg[pParse->nTempReg++] = iReg;
    }
  }
}

/*
** Allocate or deallocate a block of nReg consecutive registers.
*/
int capdbGetTempRange(Parse *pParse, int nReg){
  int i, n;
  if( nReg==1 ) return capdbGetTempReg(pParse);
  i = pParse->iRangeReg;
  n = pParse->nRangeReg;
  if( nReg<=n ){
    pParse->iRangeReg += nReg;
    pParse->nRangeReg -= nReg;
  }else{
    i = pParse->nMem+1;
    pParse->nMem += nReg;
  }
  return i;
}
void capdbReleaseTempRange(Parse *pParse, int iReg, int nReg){
  if( nReg==1 ){
    capdbReleaseTempReg(pParse, iReg);
    return;
  }
  capdbVdbeReleaseRegisters(pParse, iReg, nReg, 0, 0);
  if( nReg>pParse->nRangeReg ){
    pParse->nRangeReg = nReg;
    pParse->iRangeReg = iReg;
  }
}

/*
** Mark all temporary registers as being unavailable for reuse.
**
** Always invoke this procedure after coding a subroutine or co-routine
** that might be invoked from other parts of the code, to ensure that
** the sub/co-routine does not use registers in common with the code that
** invokes the sub/co-routine.
*/
void capdbClearTempRegCache(Parse *pParse){
  pParse->nTempReg = 0;
  pParse->nRangeReg = 0;
}

/*
** Make sure sufficient registers have been allocated so that
** iReg is a valid register number.
*/
void capdbTouchRegister(Parse *pParse, int iReg){
  if( pParse->nMem<iReg ) pParse->nMem = iReg;
}

#if defined(CAPDB_ENABLE_STAT4) || defined(CAPDB_DEBUG)
/*
** Return the latest reusable register in the set of all registers.
** The value returned is no less than iMin.  If any register iMin or
** greater is in permanent use, then return one more than that last
** permanent register.
*/
int capdbFirstAvailableRegister(Parse *pParse, int iMin){
  const ExprList *pList = pParse->pConstExpr;
  if( pList ){
    int i;
    for(i=0; i<pList->nExpr; i++){
      if( pList->a[i].u.iConstExprReg>=iMin ){
        iMin = pList->a[i].u.iConstExprReg + 1;
      }
    }
  }
  pParse->nTempReg = 0;
  pParse->nRangeReg = 0;
  return iMin;
}
#endif /* CAPDB_ENABLE_STAT4 || CAPDB_DEBUG */

/*
** Validate that no temporary register falls within the range of
** iFirst..iLast, inclusive.  This routine is only call from within assert()
** statements.
*/
#ifdef CAPDB_DEBUG
int capdbNoTempsInRange(Parse *pParse, int iFirst, int iLast){
  int i;
  if( pParse->nRangeReg>0
   && pParse->iRangeReg+pParse->nRangeReg > iFirst
   && pParse->iRangeReg <= iLast
  ){
     return 0;
  }
  for(i=0; i<pParse->nTempReg; i++){
    if( pParse->aTempReg[i]>=iFirst && pParse->aTempReg[i]<=iLast ){
      return 0;
    }
  }
  if( pParse->pConstExpr ){
    ExprList *pList = pParse->pConstExpr;
    for(i=0; i<pList->nExpr; i++){
      int iReg = pList->a[i].u.iConstExprReg;
      if( iReg==0 ) continue;
      if( iReg>=iFirst && iReg<=iLast ) return 0;
    }
  }
  return 1;
}
#endif /* CAPDB_DEBUG */
