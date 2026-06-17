/*
** 2015-06-08
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
** This file contains C code to implement the TreeView debugging routines.
** These routines print a parse tree to standard output for debugging and
** analysis. 
**
** The interfaces in this file is only available when compiling
** with CAPDB_DEBUG.
*/
#include "capdbInt.h"
#ifdef CAPDB_DEBUG

/*
** Add a new subitem to the tree.  The moreToFollow flag indicates that this
** is not the last item in the tree.
*/
static void capdbTreeViewPush(TreeView **pp, u8 moreToFollow){
  TreeView *p = *pp;
  if( p==0 ){
    *pp = p = capdb_malloc64( sizeof(*p) );
    if( p==0 ) return;
    memset(p, 0, sizeof(*p));
  }else{
    p->iLevel++;
  }
  assert( moreToFollow==0 || moreToFollow==1 );
  if( p->iLevel<(int)sizeof(p->bLine) ) p->bLine[p->iLevel] = moreToFollow;
}

/*
** Finished with one layer of the tree
*/
static void capdbTreeViewPop(TreeView **pp){
  TreeView *p = *pp;
  if( p==0 ) return;
  p->iLevel--;
  if( p->iLevel<0 ){
    capdb_free(p);
    *pp = 0;
  }
}

/*
** Generate a single line of output for the tree, with a prefix that contains
** all the appropriate tree lines
*/
void capdbTreeViewLine(TreeView *p, const char *zFormat, ...){
  va_list ap;
  int i;
  StrAccum acc;
  char zBuf[1000];
  capdbStrAccumInit(&acc, 0, zBuf, sizeof(zBuf), 0);
  if( p ){
    for(i=0; i<p->iLevel && i<(int)sizeof(p->bLine)-1; i++){
      capdb_str_append(&acc, p->bLine[i] ? "|   " : "    ", 4);
    }
    capdb_str_append(&acc, p->bLine[i] ? "|-- " : "'-- ", 4);
  }
  if( zFormat!=0 ){
    va_start(ap, zFormat);
    capdb_str_vappendf(&acc, zFormat, ap);
    va_end(ap);
    assert( acc.nChar>0 || acc.accError );
    capdb_str_append(&acc, "\n", 1);
  }
  capdbStrAccumFinish(&acc);
  fprintf(stdout,"%s", zBuf);
  fflush(stdout);
}

/*
** Shorthand for starting a new tree item that consists of a single label
*/
static void capdbTreeViewItem(TreeView *p, const char *zLabel,u8 moreFollows){
  capdbTreeViewPush(&p, moreFollows);
  capdbTreeViewLine(p, "%s", zLabel);
}

/*
** Show a list of Column objects in tree format.
*/
void capdbTreeViewColumnList(
  TreeView *pView,
  const Column *aCol,
  int nCol,
  u8 moreToFollow
){
  int i;
  capdbTreeViewPush(&pView, moreToFollow);
  capdbTreeViewLine(pView, "COLUMNS");
  for(i=0; i<nCol; i++){
    u16 flg = aCol[i].colFlags;
    int colMoreToFollow = i<(nCol - 1);
    capdbTreeViewPush(&pView, colMoreToFollow);
    capdbTreeViewLine(pView, 0);
    printf(" %s", aCol[i].zCnName);
    switch( aCol[i].eCType ){
      case COLTYPE_ANY:      printf(" ANY");        break;
      case COLTYPE_BLOB:     printf(" BLOB");       break;
      case COLTYPE_INT:      printf(" INT");        break;
      case COLTYPE_INTEGER:  printf(" INTEGER");    break;
      case COLTYPE_REAL:     printf(" REAL");       break;
      case COLTYPE_TEXT:     printf(" TEXT");       break;
      case COLTYPE_CUSTOM: {
        if( flg & COLFLAG_HASTYPE ){
          const char *z = aCol[i].zCnName;
          z += strlen(z)+1;
          printf(" X-%s", z);
          break;
        }
      }
    }
    if( flg & COLFLAG_PRIMKEY ) printf(" PRIMARY KEY");
    if( flg & COLFLAG_HIDDEN ) printf(" HIDDEN");
#ifdef COLFLAG_NOEXPAND
    if( flg & COLFLAG_NOEXPAND ) printf(" NO-EXPAND");
#endif
    if( flg ) printf(" flags=%04x", flg);
    printf("\n");      
    fflush(stdout);
    capdbTreeViewPop(&pView);
  }
  capdbTreeViewPop(&pView);
}

/*
** Generate a human-readable description of a WITH clause.
*/
void capdbTreeViewWith(TreeView *pView, const With *pWith, u8 moreToFollow){
  int i;
  if( pWith==0 ) return;
  if( pWith->nCte==0 ) return;
  if( pWith->pOuter ){
    capdbTreeViewLine(pView, "WITH (0x%p, pOuter=0x%p)",pWith,pWith->pOuter);
  }else{
    capdbTreeViewLine(pView, "WITH (0x%p)", pWith);
  }
  if( pWith->nCte>0 ){
    capdbTreeViewPush(&pView, moreToFollow);
    for(i=0; i<pWith->nCte; i++){
      StrAccum x;
      char zLine[1000];
      const struct Cte *pCte = &pWith->a[i];
      capdbStrAccumInit(&x, 0, zLine, sizeof(zLine), 0);
      capdb_str_appendf(&x, "%s", pCte->zName);
      if( pCte->pCols && pCte->pCols->nExpr>0 ){
        char cSep = '(';
        int j;
        for(j=0; j<pCte->pCols->nExpr; j++){
          capdb_str_appendf(&x, "%c%s", cSep, pCte->pCols->a[j].zEName);
          cSep = ',';
        }
        capdb_str_appendf(&x, ")");
      }
      if( pCte->eM10d!=M10d_Any ){
        capdb_str_appendf(&x, " %sMATERIALIZED", 
           pCte->eM10d==M10d_No ? "NOT " : "");
      }
      if( pCte->pUse ){
        capdb_str_appendf(&x, " (pUse=0x%p, nUse=%d)", pCte->pUse,
                 pCte->pUse->nUse);
      }
      capdbStrAccumFinish(&x);
      capdbTreeViewItem(pView, zLine, i<pWith->nCte-1);
      capdbTreeViewSelect(pView, pCte->pSelect, 0);
      capdbTreeViewPop(&pView);
    }
    capdbTreeViewPop(&pView);
  }
}

/*
** Generate a human-readable description of a SrcList object.
*/
void capdbTreeViewSrcList(TreeView *pView, const SrcList *pSrc){
  int i;
  if( pSrc==0 ) return;
  for(i=0; i<pSrc->nSrc; i++){
    const SrcItem *pItem = &pSrc->a[i];
    StrAccum x;
    int n = 0;
    char zLine[1000];
    capdbStrAccumInit(&x, 0, zLine, sizeof(zLine), 0);
    x.printfFlags |= CAPDB_PRINTF_INTERNAL;
    capdb_str_appendf(&x, "{%d:*} %!S", pItem->iCursor, pItem);
    if( pItem->pSTab ){
      capdb_str_appendf(&x, " tab=%Q nCol=%d ptr=%p used=%llx%s",
           pItem->pSTab->zName, pItem->pSTab->nCol, pItem->pSTab, 
           pItem->colUsed,
           pItem->fg.rowidUsed ? "+rowid" : "");
    }
    if( (pItem->fg.jointype & (JT_LEFT|JT_RIGHT))==(JT_LEFT|JT_RIGHT) ){
      capdb_str_appendf(&x, " FULL-OUTER-JOIN");
    }else if( pItem->fg.jointype & JT_LEFT ){
      capdb_str_appendf(&x, " LEFT-JOIN");
    }else if( pItem->fg.jointype & JT_RIGHT ){
      capdb_str_appendf(&x, " RIGHT-JOIN");
    }else if( pItem->fg.jointype & JT_CROSS ){
      capdb_str_appendf(&x, " CROSS-JOIN");
    }
    if( pItem->fg.jointype & JT_LTORJ ){
      capdb_str_appendf(&x, " LTORJ");
    }
    if( pItem->fg.fromDDL ){
      capdb_str_appendf(&x, " DDL");
    }
    if( pItem->fg.isCte ){
      static const char *aMat[] = {",MAT", "", ",NO-MAT"};
      capdb_str_appendf(&x, " CteUse=%d%s",
                          pItem->u2.pCteUse->nUse,
                          aMat[pItem->u2.pCteUse->eM10d]);
    }
    if( pItem->fg.isOn || (pItem->fg.isUsing==0 && pItem->u3.pOn!=0) ){
      capdb_str_appendf(&x, " isOn");
    }
    if( pItem->fg.isTabFunc )      capdb_str_appendf(&x, " isTabFunc");
    if( pItem->fg.isCorrelated )   capdb_str_appendf(&x, " isCorrelated");
    if( pItem->fg.isMaterialized ) capdb_str_appendf(&x, " isMaterialized");
    if( pItem->fg.viaCoroutine )   capdb_str_appendf(&x, " viaCoroutine");
    if( pItem->fg.notCte )         capdb_str_appendf(&x, " notCte");
    if( pItem->fg.isNestedFrom )   capdb_str_appendf(&x, " isNestedFrom");
    if( pItem->fg.fixedSchema )    capdb_str_appendf(&x, " fixedSchema");
    if( pItem->fg.hadSchema )      capdb_str_appendf(&x, " hadSchema");
    if( pItem->fg.isSubquery )     capdb_str_appendf(&x, " isSubquery");

    capdbStrAccumFinish(&x);
    capdbTreeViewItem(pView, zLine, i<pSrc->nSrc-1);
    n = 0;
    if( pItem->fg.isSubquery ) n++;
    if( pItem->fg.isTabFunc ) n++;
    if( pItem->fg.isUsing || pItem->u3.pOn!=0 ) n++;
    if( pItem->fg.isUsing ){
      capdbTreeViewIdList(pView, pItem->u3.pUsing, (--n)>0, "USING");
    }else if( pItem->u3.pOn!=0 ){
      capdbTreeViewItem(pView, "ON", (--n)>0);
      capdbTreeViewExpr(pView, pItem->u3.pOn, 0);
      capdbTreeViewPop(&pView);
    }
    if( pItem->fg.isSubquery ){
      assert( n==1 );
      if( pItem->pSTab ){
        Table *pTab = pItem->pSTab;
        capdbTreeViewColumnList(pView, pTab->aCol, pTab->nCol, 1);
      }
      assert( (int)pItem->fg.isNestedFrom == IsNestedFrom(pItem) );
      capdbTreeViewSelect(pView, pItem->u4.pSubq->pSelect, 0);
    }
    if( pItem->fg.isTabFunc ){
      capdbTreeViewExprList(pView, pItem->u1.pFuncArg, 0, "func-args:");
    }
    capdbTreeViewPop(&pView);
  }
}

/*
** Generate a human-readable description of a Select object.
*/
void capdbTreeViewSelect(TreeView *pView, const Select *p, u8 moreToFollow){
  int n = 0;
  int cnt = 0;
  if( p==0 ){
    capdbTreeViewLine(pView, "nil-SELECT");
    return;
  } 
  capdbTreeViewPush(&pView, moreToFollow);
  if( p->pWith ){
    capdbTreeViewWith(pView, p->pWith, 1);
    cnt = 1;
    capdbTreeViewPush(&pView, 1);
  }
  do{
    if( p->selFlags & SF_WhereBegin ){
      capdbTreeViewLine(pView, "capdbWhereBegin()");
    }else{
      capdbTreeViewLine(pView,
        "SELECT%s%s (%u/%p) selFlags=0x%x nSelectRow=%d",
        ((p->selFlags & SF_Distinct) ? " DISTINCT" : ""),
        ((p->selFlags & SF_Aggregate) ? " agg_flag" : ""),
        p->selId, p, p->selFlags,
        (int)p->nSelectRow
      );
    }
    if( cnt++ ) capdbTreeViewPop(&pView);
    if( p->pPrior ){
      n = 1000;
    }else{
      n = 0;
      if( p->pSrc && p->pSrc->nSrc && p->pSrc->nAlloc ) n++;
      if( p->pWhere ) n++;
      if( p->pGroupBy ) n++;
      if( p->pHaving ) n++;
      if( p->pOrderBy ) n++;
      if( p->pLimit ) n++;
#ifndef CAPDB_OMIT_WINDOWFUNC
      if( p->pWin ) n++;
      if( p->pWinDefn ) n++;
#endif
    }
    if( p->pEList ){
      capdbTreeViewExprList(pView, p->pEList, n>0, "result-set");
    }
    n--;
#ifndef CAPDB_OMIT_WINDOWFUNC
    if( p->pWin ){
      Window *pX;
      capdbTreeViewPush(&pView, (n--)>0);
      capdbTreeViewLine(pView, "window-functions");
      for(pX=p->pWin; pX; pX=pX->pNextWin){
        capdbTreeViewWinFunc(pView, pX, pX->pNextWin!=0);
      }
      capdbTreeViewPop(&pView);
    }
#endif
    if( p->pSrc && p->pSrc->nSrc && p->pSrc->nAlloc ){
      capdbTreeViewPush(&pView, (n--)>0);
      capdbTreeViewLine(pView, "FROM");
      capdbTreeViewSrcList(pView, p->pSrc);
      capdbTreeViewPop(&pView);
    }
    if( p->pWhere ){
      capdbTreeViewItem(pView, "WHERE", (n--)>0);
      capdbTreeViewExpr(pView, p->pWhere, 0);
      capdbTreeViewPop(&pView);
    }
    if( p->pGroupBy ){
      capdbTreeViewExprList(pView, p->pGroupBy, (n--)>0, "GROUPBY");
    }
    if( p->pHaving ){
      capdbTreeViewItem(pView, "HAVING", (n--)>0);
      capdbTreeViewExpr(pView, p->pHaving, 0);
      capdbTreeViewPop(&pView);
    }
#ifndef CAPDB_OMIT_WINDOWFUNC
    if( p->pWinDefn ){
      Window *pX;
      capdbTreeViewItem(pView, "WINDOW", (n--)>0);
      for(pX=p->pWinDefn; pX; pX=pX->pNextWin){
        capdbTreeViewWindow(pView, pX, pX->pNextWin!=0);
      }
      capdbTreeViewPop(&pView);
    }
#endif
    if( p->pOrderBy ){
      capdbTreeViewExprList(pView, p->pOrderBy, (n--)>0, "ORDERBY");
    }
    if( p->pLimit ){
      capdbTreeViewItem(pView, "LIMIT", (n--)>0);
      capdbTreeViewExpr(pView, p->pLimit->pLeft, p->pLimit->pRight!=0);
      if( p->pLimit->pRight ){
        capdbTreeViewItem(pView, "OFFSET", 0);
        capdbTreeViewExpr(pView, p->pLimit->pRight, 0);
        capdbTreeViewPop(&pView);
      }
      capdbTreeViewPop(&pView);
    }
    if( p->pPrior ){
      const char *zOp = "UNION";
      switch( p->op ){
        case TK_ALL:         zOp = "UNION ALL";  break;
        case TK_INTERSECT:   zOp = "INTERSECT";  break;
        case TK_EXCEPT:      zOp = "EXCEPT";     break;
      }
      capdbTreeViewItem(pView, zOp, 1);
    }
    p = p->pPrior;
  }while( p!=0 );
  capdbTreeViewPop(&pView);
}

#ifndef CAPDB_OMIT_WINDOWFUNC
/*
** Generate a description of starting or stopping bounds
*/
void capdbTreeViewBound(
  TreeView *pView,        /* View context */
  u8 eBound,              /* UNBOUNDED, CURRENT, PRECEDING, FOLLOWING */
  Expr *pExpr,            /* Value for PRECEDING or FOLLOWING */
  u8 moreToFollow         /* True if more to follow */
){
  switch( eBound ){
    case TK_UNBOUNDED: {
      capdbTreeViewItem(pView, "UNBOUNDED", moreToFollow);
      capdbTreeViewPop(&pView);
      break;
    }
    case TK_CURRENT: {
      capdbTreeViewItem(pView, "CURRENT", moreToFollow);
      capdbTreeViewPop(&pView);
      break;
    }
    case TK_PRECEDING: {
      capdbTreeViewItem(pView, "PRECEDING", moreToFollow);
      capdbTreeViewExpr(pView, pExpr, 0);
      capdbTreeViewPop(&pView);
      break;
    }
    case TK_FOLLOWING: {
      capdbTreeViewItem(pView, "FOLLOWING", moreToFollow);
      capdbTreeViewExpr(pView, pExpr, 0);
      capdbTreeViewPop(&pView);
      break;
    }
  }
}
#endif /* CAPDB_OMIT_WINDOWFUNC */

#ifndef CAPDB_OMIT_WINDOWFUNC
/*
** Generate a human-readable explanation for a Window object
*/
void capdbTreeViewWindow(TreeView *pView, const Window *pWin, u8 more){
  int nElement = 0;
  if( pWin==0 ) return;
  if( pWin->pFilter ){
    capdbTreeViewItem(pView, "FILTER", 1);
    capdbTreeViewExpr(pView, pWin->pFilter, 0);
    capdbTreeViewPop(&pView);
    if( pWin->eFrmType==TK_FILTER ) return;
  }
  capdbTreeViewPush(&pView, more);
  if( pWin->zName ){
    capdbTreeViewLine(pView, "OVER %s (%p)", pWin->zName, pWin);
  }else{
    capdbTreeViewLine(pView, "OVER (%p)", pWin);
  }
  if( pWin->zBase )    nElement++;
  if( pWin->pOrderBy ) nElement++;
  if( pWin->eFrmType!=0 && pWin->eFrmType!=TK_FILTER ) nElement++;
  if( pWin->eExclude ) nElement++;
  if( pWin->zBase ){
    capdbTreeViewPush(&pView, (--nElement)>0);
    capdbTreeViewLine(pView, "window: %s", pWin->zBase);
    capdbTreeViewPop(&pView);
  }
  if( pWin->pPartition ){
    capdbTreeViewExprList(pView, pWin->pPartition, nElement>0,"PARTITION-BY");
  }
  if( pWin->pOrderBy ){
    capdbTreeViewExprList(pView, pWin->pOrderBy, (--nElement)>0, "ORDER-BY");
  }
  if( pWin->eFrmType!=0 && pWin->eFrmType!=TK_FILTER ){
    char zBuf[30];
    const char *zFrmType = "ROWS";
    if( pWin->eFrmType==TK_RANGE ) zFrmType = "RANGE";
    if( pWin->eFrmType==TK_GROUPS ) zFrmType = "GROUPS";
    capdb_snprintf(sizeof(zBuf),zBuf,"%s%s",zFrmType,
        pWin->bImplicitFrame ? " (implied)" : "");
    capdbTreeViewItem(pView, zBuf, (--nElement)>0);
    capdbTreeViewBound(pView, pWin->eStart, pWin->pStart, 1);
    capdbTreeViewBound(pView, pWin->eEnd, pWin->pEnd, 0);
    capdbTreeViewPop(&pView);
  }
  if( pWin->eExclude ){
    char zBuf[30];
    const char *zExclude;
    switch( pWin->eExclude ){
      case TK_NO:      zExclude = "NO OTHERS";   break;
      case TK_CURRENT: zExclude = "CURRENT ROW"; break;
      case TK_GROUP:   zExclude = "GROUP";       break;
      case TK_TIES:    zExclude = "TIES";        break;
      default:
        capdb_snprintf(sizeof(zBuf),zBuf,"invalid(%d)", pWin->eExclude);
        zExclude = zBuf;
        break;
    }
    capdbTreeViewPush(&pView, 0);
    capdbTreeViewLine(pView, "EXCLUDE %s", zExclude);
    capdbTreeViewPop(&pView);
  }
  capdbTreeViewPop(&pView);
}
#endif /* CAPDB_OMIT_WINDOWFUNC */

#ifndef CAPDB_OMIT_WINDOWFUNC
/*
** Generate a human-readable explanation for a Window Function object
*/
void capdbTreeViewWinFunc(TreeView *pView, const Window *pWin, u8 more){
  if( pWin==0 ) return;
  capdbTreeViewPush(&pView, more);
  capdbTreeViewLine(pView, "WINFUNC %s(%d)",
                       pWin->pWFunc->zName, pWin->pWFunc->nArg);
  capdbTreeViewWindow(pView, pWin, 0);
  capdbTreeViewPop(&pView);
}
#endif /* CAPDB_OMIT_WINDOWFUNC */

/*
** Generate a human-readable explanation of an expression tree.
*/
void capdbTreeViewExpr(TreeView *pView, const Expr *pExpr, u8 moreToFollow){
  const char *zBinOp = 0;   /* Binary operator */
  const char *zUniOp = 0;   /* Unary operator */
  char zFlgs[200];
  capdbTreeViewPush(&pView, moreToFollow);
  if( pExpr==0 ){
    capdbTreeViewLine(pView, "nil");
    capdbTreeViewPop(&pView);
    return;
  }
  if( pExpr->flags || pExpr->affExpr || pExpr->vvaFlags || pExpr->pAggInfo ){
    StrAccum x;
    capdbStrAccumInit(&x, 0, zFlgs, sizeof(zFlgs), 0);
    capdb_str_appendf(&x, " fg.af=%x.%c",
      pExpr->flags, pExpr->affExpr ? pExpr->affExpr : 'n');
    if( ExprHasProperty(pExpr, EP_OuterON) ){
      capdb_str_appendf(&x, " outer.iJoin=%d", pExpr->w.iJoin);
    }
    if( ExprHasProperty(pExpr, EP_InnerON) ){
      capdb_str_appendf(&x, " inner.iJoin=%d", pExpr->w.iJoin);
    }
    if( ExprHasProperty(pExpr, EP_FromDDL) ){
      capdb_str_appendf(&x, " DDL");
    }
    if( ExprHasVVAProperty(pExpr, EP_Immutable) ){
      capdb_str_appendf(&x, " IMMUTABLE");
    }
    if( pExpr->pAggInfo!=0 ){
      capdb_str_appendf(&x, " agg-column[%d]", pExpr->iAgg);
    }
    capdbStrAccumFinish(&x);
  }else{
    zFlgs[0] = 0;
  }
  switch( pExpr->op ){
    case TK_AGG_COLUMN: {
      capdbTreeViewLine(pView, "AGG{%d:%d}%s",
            pExpr->iTable, pExpr->iColumn, zFlgs);
      break;
    }
    case TK_COLUMN: {
      if( pExpr->iTable<0 ){
        /* This only happens when coding check constraints */
        char zOp2[16];
        if( pExpr->op2 ){
          capdb_snprintf(sizeof(zOp2),zOp2," op2=0x%02x",pExpr->op2);
        }else{
          zOp2[0] = 0;
        }
        capdbTreeViewLine(pView, "COLUMN(%d)%s%s",
                                    pExpr->iColumn, zFlgs, zOp2);
      }else{
        assert( ExprUseYTab(pExpr) );
        capdbTreeViewLine(pView, "{%d:%d} pTab=%p%s",
                        pExpr->iTable, pExpr->iColumn,
                        pExpr->y.pTab, zFlgs);
      }
      if( ExprHasProperty(pExpr, EP_FixedCol) ){
        capdbTreeViewExpr(pView, pExpr->pLeft, 0);
      }
      break;
    }
    case TK_INTEGER: {
      if( pExpr->flags & EP_IntValue ){
        capdbTreeViewLine(pView, "%d", pExpr->u.iValue);
      }else{
        capdbTreeViewLine(pView, "%s", pExpr->u.zToken);
      }
      break;
    }
#ifndef CAPDB_OMIT_FLOATING_POINT
    case TK_FLOAT: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView,"%s", pExpr->u.zToken);
      break;
    }
#endif
    case TK_STRING: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView,"%Q", pExpr->u.zToken);
      break;
    }
    case TK_NULL: {
      capdbTreeViewLine(pView,"NULL");
      break;
    }
    case TK_TRUEFALSE: {
      capdbTreeViewLine(pView,"%s%s",
         capdbExprTruthValue(pExpr) ? "TRUE" : "FALSE", zFlgs);
      break;
    }
#ifndef CAPDB_OMIT_BLOB_LITERAL
    case TK_BLOB: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView,"%s", pExpr->u.zToken);
      break;
    }
#endif
    case TK_VARIABLE: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView,"VARIABLE(%s,%d)",
                          pExpr->u.zToken, pExpr->iColumn);
      break;
    }
    case TK_REGISTER: {
      capdbTreeViewLine(pView,"REGISTER(%d)", pExpr->iTable);
      break;
    }
    case TK_ID: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView,"ID \"%w\"", pExpr->u.zToken);
      break;
    }
#ifndef CAPDB_OMIT_CAST
    case TK_CAST: {
      /* Expressions of the form:   CAST(pLeft AS token) */
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView,"CAST %Q", pExpr->u.zToken);
      capdbTreeViewExpr(pView, pExpr->pLeft, 0);
      break;
    }
#endif /* CAPDB_OMIT_CAST */
    case TK_LT:      zBinOp = "LT";     break;
    case TK_LE:      zBinOp = "LE";     break;
    case TK_GT:      zBinOp = "GT";     break;
    case TK_GE:      zBinOp = "GE";     break;
    case TK_NE:      zBinOp = "NE";     break;
    case TK_EQ:      zBinOp = "EQ";     break;
    case TK_IS:      zBinOp = "IS";     break;
    case TK_ISNOT:   zBinOp = "ISNOT";  break;
    case TK_AND:     zBinOp = "AND";    break;
    case TK_OR:      zBinOp = "OR";     break;
    case TK_PLUS:    zBinOp = "ADD";    break;
    case TK_STAR:    zBinOp = "MUL";    break;
    case TK_MINUS:   zBinOp = "SUB";    break;
    case TK_REM:     zBinOp = "REM";    break;
    case TK_BITAND:  zBinOp = "BITAND"; break;
    case TK_BITOR:   zBinOp = "BITOR";  break;
    case TK_SLASH:   zBinOp = "DIV";    break;
    case TK_LSHIFT:  zBinOp = "LSHIFT"; break;
    case TK_RSHIFT:  zBinOp = "RSHIFT"; break;
    case TK_CONCAT:  zBinOp = "CONCAT"; break;
    case TK_DOT:     zBinOp = "DOT";    break;
    case TK_LIMIT:   zBinOp = "LIMIT";  break;

    case TK_UMINUS:  zUniOp = "UMINUS"; break;
    case TK_UPLUS:   zUniOp = "UPLUS";  break;
    case TK_BITNOT:  zUniOp = "BITNOT"; break;
    case TK_NOT:     zUniOp = "NOT";    break;
    case TK_ISNULL:  zUniOp = "ISNULL"; break;
    case TK_NOTNULL: zUniOp = "NOTNULL"; break;

    case TK_TRUTH: {
      int x;
      const char *azOp[] = {
         "IS-FALSE", "IS-TRUE", "IS-NOT-FALSE", "IS-NOT-TRUE"
      };
      assert( pExpr->op2==TK_IS || pExpr->op2==TK_ISNOT );
      assert( pExpr->pRight );
      assert( capdbExprSkipCollateAndLikely(pExpr->pRight)->op
                  == TK_TRUEFALSE );
      x = (pExpr->op2==TK_ISNOT)*2 + capdbExprTruthValue(pExpr->pRight);
      zUniOp = azOp[x];
      break;
    }

    case TK_SPAN: {
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView, "SPAN %Q", pExpr->u.zToken);
      capdbTreeViewExpr(pView, pExpr->pLeft, 0);
      break;
    }

    case TK_COLLATE: {
      /* COLLATE operators without the EP_Collate flag are intended to
      ** emulate collation associated with a table column.  These show
      ** up in the treeview output as "SOFT-COLLATE".  Explicit COLLATE
      ** operators that appear in the original SQL always have the
      ** EP_Collate bit set and appear in treeview output as just "COLLATE" */
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView, "%sCOLLATE %Q%s",
        !ExprHasProperty(pExpr, EP_Collate) ? "SOFT-" : "",
        pExpr->u.zToken, zFlgs);
      capdbTreeViewExpr(pView, pExpr->pLeft, 0);
      break;
    }

    case TK_AGG_FUNCTION:
    case TK_FUNCTION: {
      ExprList *pFarg;       /* List of function arguments */
      Window *pWin;
      if( ExprHasProperty(pExpr, EP_TokenOnly) ){
        pFarg = 0;
        pWin = 0;
      }else{
        assert( ExprUseXList(pExpr) );
        pFarg = pExpr->x.pList;
#ifndef CAPDB_OMIT_WINDOWFUNC
        pWin = IsWindowFunc(pExpr) ? pExpr->y.pWin : 0;
#else
        pWin = 0;
#endif 
      }
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      if( pExpr->op==TK_AGG_FUNCTION ){
        capdbTreeViewLine(pView, "AGG_FUNCTION%d %Q%s agg=%d[%d]/%p",
                             pExpr->op2, pExpr->u.zToken, zFlgs,
                             pExpr->pAggInfo ? pExpr->pAggInfo->selId : 0,
                             pExpr->iAgg, pExpr->pAggInfo);
      }else if( pExpr->op2!=0 ){
        const char *zOp2;
        char zBuf[8];
        capdb_snprintf(sizeof(zBuf),zBuf,"0x%02x",pExpr->op2);
        zOp2 = zBuf;
        if( pExpr->op2==NC_IsCheck ) zOp2 = "NC_IsCheck";
        if( pExpr->op2==NC_IdxExpr ) zOp2 = "NC_IdxExpr";
        if( pExpr->op2==NC_PartIdx ) zOp2 = "NC_PartIdx";
        if( pExpr->op2==NC_GenCol ) zOp2 = "NC_GenCol";
        capdbTreeViewLine(pView, "FUNCTION %Q%s op2=%s",
                            pExpr->u.zToken, zFlgs, zOp2);
      }else{
        capdbTreeViewLine(pView, "FUNCTION %Q%s", pExpr->u.zToken, zFlgs);
      }
      if( pFarg ){
        capdbTreeViewExprList(pView, pFarg, pWin!=0 || pExpr->pLeft, 0);
        if( pExpr->pLeft ){
          Expr *pOB = pExpr->pLeft;
          assert( pOB->op==TK_ORDER );
          assert( ExprUseXList(pOB) );
          capdbTreeViewExprList(pView, pOB->x.pList, pWin!=0, "ORDERBY");
        }
      }
#ifndef CAPDB_OMIT_WINDOWFUNC
      if( pWin ){
        capdbTreeViewWindow(pView, pWin, 0);
      }
#endif
      break;
    }
    case TK_ORDER: {
      capdbTreeViewExprList(pView, pExpr->x.pList, 0, "ORDERBY");
      break;
    }
#ifndef CAPDB_OMIT_SUBQUERY
    case TK_EXISTS: {
      assert( ExprUseXSelect(pExpr) );
      capdbTreeViewLine(pView, "EXISTS-expr flags=0x%x", pExpr->flags);
      capdbTreeViewSelect(pView, pExpr->x.pSelect, 0);
      break;
    }
    case TK_SELECT: {
      assert( ExprUseXSelect(pExpr) );
      capdbTreeViewLine(pView, "subquery-expr flags=0x%x", pExpr->flags);
      capdbTreeViewSelect(pView, pExpr->x.pSelect, 0);
      break;
    }
    case TK_IN: {
      capdb_str *pStr = capdb_str_new(0);
      char *z;
      capdb_str_appendf(pStr, "IN flags=0x%x", pExpr->flags);
      if( pExpr->iTable ) capdb_str_appendf(pStr, " iTable=%d",pExpr->iTable);
      if( ExprHasProperty(pExpr, EP_Subrtn) ){
        capdb_str_appendf(pStr, " subrtn(%d,%d)",
            pExpr->y.sub.regReturn, pExpr->y.sub.iAddr);
      }
      z = capdb_str_finish(pStr);
      capdbTreeViewLine(pView, z);
      capdb_free(z);
      capdbTreeViewExpr(pView, pExpr->pLeft, 1);
      if( ExprUseXSelect(pExpr) ){
        capdbTreeViewSelect(pView, pExpr->x.pSelect, 0);
      }else{
        capdbTreeViewExprList(pView, pExpr->x.pList, 0, 0);
      }
      break;
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
      const Expr *pX, *pY, *pZ;
      pX = pExpr->pLeft;
      assert( ExprUseXList(pExpr) );
      assert( pExpr->x.pList->nExpr==2 );
      pY = pExpr->x.pList->a[0].pExpr;
      pZ = pExpr->x.pList->a[1].pExpr;
      capdbTreeViewLine(pView, "BETWEEN%s", zFlgs);
      capdbTreeViewExpr(pView, pX, 1);
      capdbTreeViewExpr(pView, pY, 1);
      capdbTreeViewExpr(pView, pZ, 0);
      break;
    }
    case TK_TRIGGER: {
      /* If the opcode is TK_TRIGGER, then the expression is a reference
      ** to a column in the new.* or old.* pseudo-tables available to
      ** trigger programs. In this case Expr.iTable is set to 1 for the
      ** new.* pseudo-table, or 0 for the old.* pseudo-table. Expr.iColumn
      ** is set to the column of the pseudo-table to read, or to -1 to
      ** read the rowid field.
      */
      capdbTreeViewLine(pView, "%s(%d)", 
          pExpr->iTable ? "NEW" : "OLD", pExpr->iColumn);
      break;
    }
    case TK_CASE: {
      capdbTreeViewLine(pView, "CASE");
      capdbTreeViewExpr(pView, pExpr->pLeft, 1);
      assert( ExprUseXList(pExpr) );
      capdbTreeViewExprList(pView, pExpr->x.pList, 0, 0);
      break;
    }
#ifndef CAPDB_OMIT_TRIGGER
    case TK_RAISE: {
      const char *zType = "unk";
      switch( pExpr->affExpr ){
        case OE_Rollback:   zType = "rollback";  break;
        case OE_Abort:      zType = "abort";     break;
        case OE_Fail:       zType = "fail";      break;
        case OE_Ignore:     zType = "ignore";    break;
      }
      assert( !ExprHasProperty(pExpr, EP_IntValue) );
      capdbTreeViewLine(pView, "RAISE %s", zType);
      capdbTreeViewExpr(pView, pExpr->pLeft, 0);
      break;
    }
#endif
    case TK_MATCH: {
      capdbTreeViewLine(pView, "MATCH {%d:%d}%s",
                          pExpr->iTable, pExpr->iColumn, zFlgs);
      capdbTreeViewExpr(pView, pExpr->pRight, 0);
      break;
    }
    case TK_VECTOR: {
      char *z = capdb_mprintf("VECTOR%s",zFlgs);
      assert( ExprUseXList(pExpr) );
      capdbTreeViewBareExprList(pView, pExpr->x.pList, z);
      capdb_free(z);
      break;
    }
    case TK_SELECT_COLUMN: {
      capdbTreeViewLine(pView, "SELECT-COLUMN %d of [0..%d]%s",
              pExpr->iColumn, pExpr->iTable-1,
              pExpr->pRight==pExpr->pLeft ? " (SELECT-owner)" : "");
      assert( ExprUseXSelect(pExpr->pLeft) );
      capdbTreeViewSelect(pView, pExpr->pLeft->x.pSelect, 0);
      break;
    }
    case TK_IF_NULL_ROW: {
      capdbTreeViewLine(pView, "IF-NULL-ROW %d", pExpr->iTable);
      capdbTreeViewExpr(pView, pExpr->pLeft, 0);
      break;
    }
    case TK_ERROR: {
      Expr tmp;
      capdbTreeViewLine(pView, "ERROR");
      tmp = *pExpr;
      tmp.op = pExpr->op2;
      capdbTreeViewExpr(pView, &tmp, 0);
      break;
    }
    case TK_ROW: {
      if( pExpr->iColumn<=0 ){
        capdbTreeViewLine(pView, "First FROM table rowid");
      }else{
        capdbTreeViewLine(pView, "First FROM table column %d",
            pExpr->iColumn-1);
      }
      break;
    }
    default: {
      capdbTreeViewLine(pView, "op=%d", pExpr->op);
      break;
    }
  }
  if( zBinOp ){
    capdbTreeViewLine(pView, "%s%s", zBinOp, zFlgs);
    capdbTreeViewExpr(pView, pExpr->pLeft, 1);
    capdbTreeViewExpr(pView, pExpr->pRight, 0);
  }else if( zUniOp ){
    capdbTreeViewLine(pView, "%s%s", zUniOp, zFlgs);
   capdbTreeViewExpr(pView, pExpr->pLeft, 0);
  }
  capdbTreeViewPop(&pView);
}


/*
** Generate a human-readable explanation of an expression list.
*/
void capdbTreeViewBareExprList(
  TreeView *pView,
  const ExprList *pList,
  const char *zLabel
){
  if( zLabel==0 || zLabel[0]==0 ) zLabel = "LIST";
  if( pList==0 ){
    capdbTreeViewLine(pView, "%s (empty)", zLabel);
  }else{
    int i;
    capdbTreeViewLine(pView, "%s", zLabel);
    for(i=0; i<pList->nExpr; i++){
      int j = pList->a[i].u.x.iOrderByCol;
      u8 sortFlags = pList->a[i].fg.sortFlags;
      char *zName = pList->a[i].zEName;
      int moreToFollow = i<pList->nExpr - 1;
      if( j || zName || sortFlags ){
        capdbTreeViewPush(&pView, moreToFollow);
        moreToFollow = 0;
        capdbTreeViewLine(pView, 0);
        if( zName ){
          switch( pList->a[i].fg.eEName ){
            default:
              fprintf(stdout, "AS %s ", zName);
              break;
            case ENAME_TAB:
              fprintf(stdout, "TABLE-ALIAS-NAME(\"%s\") ", zName);
              if( pList->a[i].fg.bUsed ) fprintf(stdout, "(used) ");
              if( pList->a[i].fg.bUsingTerm ) fprintf(stdout, "(USING-term) ");
              if( pList->a[i].fg.bNoExpand ) fprintf(stdout, "(NoExpand) ");
              break;
            case ENAME_SPAN:
              fprintf(stdout, "SPAN(\"%s\") ", zName);
              break;
          }
        }
        if( j ){
          fprintf(stdout, "iOrderByCol=%d ", j);
        }
        if( sortFlags & KEYINFO_ORDER_DESC ){
          fprintf(stdout, "DESC ");
        }else if( sortFlags & KEYINFO_ORDER_BIGNULL ){
          fprintf(stdout, "NULLS-LAST");
        }
        fprintf(stdout, "\n");
        fflush(stdout);
      }
      capdbTreeViewExpr(pView, pList->a[i].pExpr, moreToFollow);
      if( j || zName || sortFlags ){
        capdbTreeViewPop(&pView);
      }
    }
  }
}
void capdbTreeViewExprList(
  TreeView *pView,
  const ExprList *pList,
  u8 moreToFollow,
  const char *zLabel
){
  capdbTreeViewPush(&pView, moreToFollow);
  capdbTreeViewBareExprList(pView, pList, zLabel);
  capdbTreeViewPop(&pView);
}

/*
** Generate a human-readable explanation of an id-list.
*/
void capdbTreeViewBareIdList(
  TreeView *pView,
  const IdList *pList,
  const char *zLabel
){
  if( zLabel==0 || zLabel[0]==0 ) zLabel = "LIST";
  if( pList==0 ){
    capdbTreeViewLine(pView, "%s (empty)", zLabel);
  }else{
    int i;
    capdbTreeViewLine(pView, "%s", zLabel);
    for(i=0; i<pList->nId; i++){
      char *zName = pList->a[i].zName;
      int moreToFollow = i<pList->nId - 1;
      if( zName==0 ) zName = "(null)";
      capdbTreeViewPush(&pView, moreToFollow);
      capdbTreeViewLine(pView, 0);
      fprintf(stdout, "%s\n", zName);
      capdbTreeViewPop(&pView);
    }
  }
}
void capdbTreeViewIdList(
  TreeView *pView,
  const IdList *pList,
  u8 moreToFollow,
  const char *zLabel
){
  capdbTreeViewPush(&pView, moreToFollow);
  capdbTreeViewBareIdList(pView, pList, zLabel);
  capdbTreeViewPop(&pView);
}

/*
** Generate a human-readable explanation of a list of Upsert objects
*/
void capdbTreeViewUpsert(
  TreeView *pView,
  const Upsert *pUpsert,
  u8 moreToFollow
){
  if( pUpsert==0 ) return;
  capdbTreeViewPush(&pView, moreToFollow);
  while( pUpsert ){
    int n;
    capdbTreeViewPush(&pView, pUpsert->pNextUpsert!=0 || moreToFollow);
    capdbTreeViewLine(pView, "ON CONFLICT DO %s", 
         pUpsert->isDoUpdate ? "UPDATE" : "NOTHING");
    n = (pUpsert->pUpsertSet!=0) + (pUpsert->pUpsertWhere!=0);
    capdbTreeViewExprList(pView, pUpsert->pUpsertTarget, (n--)>0, "TARGET");
    capdbTreeViewExprList(pView, pUpsert->pUpsertSet, (n--)>0, "SET");
    if( pUpsert->pUpsertWhere ){
      capdbTreeViewItem(pView, "WHERE", (n--)>0);
      capdbTreeViewExpr(pView, pUpsert->pUpsertWhere, 0);
      capdbTreeViewPop(&pView);
    }
    capdbTreeViewPop(&pView);
    pUpsert = pUpsert->pNextUpsert;
  }
  capdbTreeViewPop(&pView);
}

#if TREETRACE_ENABLED
/*
** Generate a human-readable diagram of the data structure that go
** into generating an DELETE statement.
*/
void capdbTreeViewDelete(
  const With *pWith,
  const SrcList *pTabList,
  const Expr *pWhere,
  const ExprList *pOrderBy,
  const Expr *pLimit,
  const Trigger *pTrigger
){
  int n = 0;
  TreeView *pView = 0;
  capdbTreeViewPush(&pView, 0);
  capdbTreeViewLine(pView, "DELETE");
  if( pWith ) n++;
  if( pTabList ) n++;
  if( pWhere ) n++;
  if( pOrderBy ) n++;
  if( pLimit ) n++;
  if( pTrigger ) n++;
  if( pWith ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewWith(pView, pWith, 0);
    capdbTreeViewPop(&pView);
  }
  if( pTabList ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "FROM");
    capdbTreeViewSrcList(pView, pTabList);
    capdbTreeViewPop(&pView);
  }
  if( pWhere ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "WHERE");
    capdbTreeViewExpr(pView, pWhere, 0);
    capdbTreeViewPop(&pView);
  }
  if( pOrderBy ){
    capdbTreeViewExprList(pView, pOrderBy, (--n)>0, "ORDER-BY");
  }
  if( pLimit ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "LIMIT");
    capdbTreeViewExpr(pView, pLimit, 0);
    capdbTreeViewPop(&pView);
  }
  if( pTrigger ){
    capdbTreeViewTrigger(pView, pTrigger, (--n)>0, 1);
  }
  capdbTreeViewPop(&pView);
}
#endif /* TREETRACE_ENABLED */

#if TREETRACE_ENABLED
/*
** Generate a human-readable diagram of the data structure that go
** into generating an INSERT statement.
*/
void capdbTreeViewInsert(
  const With *pWith,
  const SrcList *pTabList,
  const IdList *pColumnList,
  const Select *pSelect,
  const ExprList *pExprList,
  int onError,
  const Upsert *pUpsert,
  const Trigger *pTrigger
){
  TreeView *pView = 0;
  int n = 0;
  const char *zLabel = "INSERT";
  switch( onError ){
    case OE_Replace:  zLabel = "REPLACE";             break;
    case OE_Ignore:   zLabel = "INSERT OR IGNORE";    break;
    case OE_Rollback: zLabel = "INSERT OR ROLLBACK";  break;
    case OE_Abort:    zLabel = "INSERT OR ABORT";     break;
    case OE_Fail:     zLabel = "INSERT OR FAIL";      break;
  }
  capdbTreeViewPush(&pView, 0);
  capdbTreeViewLine(pView, zLabel);
  if( pWith ) n++;
  if( pTabList ) n++;
  if( pColumnList ) n++;
  if( pSelect ) n++;
  if( pExprList ) n++;
  if( pUpsert ) n++;
  if( pTrigger ) n++;
  if( pWith ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewWith(pView, pWith, 0);
    capdbTreeViewPop(&pView);
  }
  if( pTabList ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "INTO");
    capdbTreeViewSrcList(pView, pTabList);
    capdbTreeViewPop(&pView);
  }
  if( pColumnList ){
    capdbTreeViewIdList(pView, pColumnList, (--n)>0, "COLUMNS");
  }
  if( pSelect ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "DATA-SOURCE");
    capdbTreeViewSelect(pView, pSelect, 0);
    capdbTreeViewPop(&pView);
  }
  if( pExprList ){
    capdbTreeViewExprList(pView, pExprList, (--n)>0, "VALUES");
  }
  if( pUpsert ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "UPSERT");
    capdbTreeViewUpsert(pView, pUpsert, 0);
    capdbTreeViewPop(&pView);
  }
  if( pTrigger ){
    capdbTreeViewTrigger(pView, pTrigger, (--n)>0, 1);
  }
  capdbTreeViewPop(&pView);
}
#endif /* TREETRACE_ENABLED */

#if TREETRACE_ENABLED
/*
** Generate a human-readable diagram of the data structure that go
** into generating an UPDATE statement.
*/
void capdbTreeViewUpdate(
  const With *pWith,
  const SrcList *pTabList,
  const ExprList *pChanges,
  const Expr *pWhere,
  int onError,
  const ExprList *pOrderBy,
  const Expr *pLimit,
  const Upsert *pUpsert,
  const Trigger *pTrigger
){
  int n = 0;
  TreeView *pView = 0;
  const char *zLabel = "UPDATE";
  switch( onError ){
    case OE_Replace:  zLabel = "UPDATE OR REPLACE";   break;
    case OE_Ignore:   zLabel = "UPDATE OR IGNORE";    break;
    case OE_Rollback: zLabel = "UPDATE OR ROLLBACK";  break;
    case OE_Abort:    zLabel = "UPDATE OR ABORT";     break;
    case OE_Fail:     zLabel = "UPDATE OR FAIL";      break;
  }
  capdbTreeViewPush(&pView, 0);
  capdbTreeViewLine(pView, zLabel);
  if( pWith ) n++;
  if( pTabList ) n++;
  if( pChanges ) n++;
  if( pWhere ) n++;
  if( pOrderBy ) n++;
  if( pLimit ) n++;
  if( pUpsert ) n++;
  if( pTrigger ) n++;
  if( pWith ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewWith(pView, pWith, 0);
    capdbTreeViewPop(&pView);
  }
  if( pTabList ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "FROM");
    capdbTreeViewSrcList(pView, pTabList);
    capdbTreeViewPop(&pView);
  }
  if( pChanges ){
    capdbTreeViewExprList(pView, pChanges, (--n)>0, "SET");
  }
  if( pWhere ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "WHERE");
    capdbTreeViewExpr(pView, pWhere, 0);
    capdbTreeViewPop(&pView);
  }
  if( pOrderBy ){
    capdbTreeViewExprList(pView, pOrderBy, (--n)>0, "ORDER-BY");
  }
  if( pLimit ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "LIMIT");
    capdbTreeViewExpr(pView, pLimit, 0);
    capdbTreeViewPop(&pView);
  }
  if( pUpsert ){
    capdbTreeViewPush(&pView, (--n)>0);
    capdbTreeViewLine(pView, "UPSERT");
    capdbTreeViewUpsert(pView, pUpsert, 0);
    capdbTreeViewPop(&pView);
  }
  if( pTrigger ){
    capdbTreeViewTrigger(pView, pTrigger, (--n)>0, 1);
  }
  capdbTreeViewPop(&pView);
}
#endif /* TREETRACE_ENABLED */

#ifndef CAPDB_OMIT_TRIGGER
/*
** Show a human-readable graph of a TriggerStep
*/
void capdbTreeViewTriggerStep(
  TreeView *pView,
  const TriggerStep *pStep,
  u8 moreToFollow,
  u8 showFullList
){
  int cnt = 0;
  if( pStep==0 ) return;
  capdbTreeViewPush(&pView, 
      moreToFollow || (showFullList && pStep->pNext!=0));
  do{
    if( cnt++ && pStep->pNext==0 ){
      capdbTreeViewPop(&pView);
      capdbTreeViewPush(&pView, 0);
    }
    capdbTreeViewLine(pView, "%s", pStep->zSpan ? pStep->zSpan : "RETURNING");
  }while( showFullList && (pStep = pStep->pNext)!=0 );
  capdbTreeViewPop(&pView);
}
  
/*
** Show a human-readable graph of a Trigger
*/
void capdbTreeViewTrigger(
  TreeView *pView,
  const Trigger *pTrigger,
  u8 moreToFollow,
  u8 showFullList
){
  int cnt = 0;
  if( pTrigger==0 ) return;
  capdbTreeViewPush(&pView,
     moreToFollow || (showFullList && pTrigger->pNext!=0));
  do{
    if( cnt++ && pTrigger->pNext==0 ){
      capdbTreeViewPop(&pView);
      capdbTreeViewPush(&pView, 0);
    }
    capdbTreeViewLine(pView, "TRIGGER %s", pTrigger->zName);
    capdbTreeViewPush(&pView, 0);
    capdbTreeViewTriggerStep(pView, pTrigger->step_list, 0, 1);
    capdbTreeViewPop(&pView);
  }while( showFullList && (pTrigger = pTrigger->pNext)!=0 );
  capdbTreeViewPop(&pView);
}
#endif /* CAPDB_OMIT_TRIGGER */
  

/*
** These simplified versions of the tree-view routines omit unnecessary
** parameters.  These variants are intended to be used from a symbolic
** debugger, such as "gdb", during interactive debugging sessions.
**
** This routines are given external linkage so that they will always be
** accessible to the debugging, and to avoid warnings about unused
** functions.  But these routines only exist in debugging builds, so they
** do not contaminate the interface.
**
** See Also:
**
**     capdbShowWhereTerm() in where.c
*/
void capdbShowExpr(const Expr *p){ capdbTreeViewExpr(0,p,0); }
void capdbShowExprList(const ExprList *p){ capdbTreeViewExprList(0,p,0,0);}
void capdbShowIdList(const IdList *p){ capdbTreeViewIdList(0,p,0,0); }
void capdbShowSrcList(const SrcList *p){
  TreeView *pView = 0;
  capdbTreeViewPush(&pView, 0);
  capdbTreeViewLine(pView, "SRCLIST");
  capdbTreeViewSrcList(pView,p);
  capdbTreeViewPop(&pView);
}
void capdbShowSelect(const Select *p){ capdbTreeViewSelect(0,p,0); }
void capdbShowWith(const With *p){ capdbTreeViewWith(0,p,0); }
void capdbShowUpsert(const Upsert *p){ capdbTreeViewUpsert(0,p,0); }
#ifndef CAPDB_OMIT_TRIGGER
void capdbShowTriggerStep(const TriggerStep *p){
  capdbTreeViewTriggerStep(0,p,0,0);
}
void capdbShowTriggerStepList(const TriggerStep *p){
  capdbTreeViewTriggerStep(0,p,0,1);
}
void capdbShowTrigger(const Trigger *p){ capdbTreeViewTrigger(0,p,0,0); }
void capdbShowTriggerList(const Trigger *p){ capdbTreeViewTrigger(0,p,0,1);}
#endif
#ifndef CAPDB_OMIT_WINDOWFUNC
void capdbShowWindow(const Window *p){ capdbTreeViewWindow(0,p,0); }
void capdbShowWinFunc(const Window *p){ capdbTreeViewWinFunc(0,p,0); }
#endif

#endif /* CAPDB_DEBUG */
