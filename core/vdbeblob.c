/*
** 2007 May 1
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
** This file contains code used to implement incremental BLOB I/O.
*/

#include "capdbInt.h"
#include "vdbeInt.h"

#ifndef CAPDB_OMIT_INCRBLOB

/*
** Valid capdb_blob* handles point to Incrblob structures.
*/
typedef struct Incrblob Incrblob;
struct Incrblob {
  int nByte;              /* Size of open blob, in bytes */
  int iOffset;            /* Byte offset of blob in cursor data */
  u16 iCol;               /* Table column this handle is open on */
  BtCursor *pCsr;         /* Cursor pointing at blob row */
  capdb_stmt *pStmt;    /* Statement holding cursor open */
  capdb *db;            /* The associated database */
  char *zDb;              /* Database name */
  Table *pTab;            /* Table object */
};


/*
** This function is used by both blob_open() and blob_reopen(). It seeks
** the b-tree cursor associated with blob handle p to point to row iRow.
** If successful, CAPDB_OK is returned and subsequent calls to
** capdb_blob_read() or capdb_blob_write() access the specified row.
**
** If an error occurs, or if the specified row does not exist or does not
** contain a value of type TEXT or BLOB in the column nominated when the
** blob handle was opened, then an error code is returned and *pzErr may
** be set to point to a buffer containing an error message. It is the
** responsibility of the caller to free the error message buffer using
** capdbDbFree().
**
** If an error does occur, then the b-tree cursor is closed. All subsequent
** calls to capdb_blob_read(), blob_write() or blob_reopen() will 
** immediately return CAPDB_ABORT.
*/
static int blobSeekToRow(Incrblob *p, capdb_int64 iRow, char **pzErr){
  int rc;                         /* Error code */
  char *zErr = 0;                 /* Error message */
  Vdbe *v = (Vdbe *)p->pStmt;

  /* Set the value of register r[1] in the SQL statement to integer iRow. 
  ** This is done directly as a performance optimization
  */
  capdbVdbeMemSetInt64(&v->aMem[1], iRow);

  /* If the statement has been run before (and is paused at the OP_ResultRow)
  ** then back it up to the point where it does the OP_NotExists.  This could
  ** have been down with an extra OP_Goto, but simply setting the program
  ** counter is faster. */
  if( v->pc>4 ){
    v->pc = 4;
    assert( v->aOp[v->pc].opcode==OP_NotExists );
    rc = capdbVdbeExec(v);
  }else{
    rc = capdb_step(p->pStmt);
  }
  if( rc==CAPDB_ROW ){
    VdbeCursor *pC = v->apCsr[0];
    u32 type;
    assert( pC!=0 );
    assert( pC->eCurType==CURTYPE_BTREE );
    type = pC->nHdrParsed>p->iCol ? pC->aType[p->iCol] : 0;
    testcase( pC->nHdrParsed==p->iCol );
    testcase( pC->nHdrParsed==p->iCol+1 );
    if( type<12 ){
      zErr = capdbMPrintf(p->db, "cannot open value of type %s",
          type==0?"null": type==7?"real": "integer"
      );
      rc = CAPDB_ERROR;
      capdb_finalize(p->pStmt);
      p->pStmt = 0;
    }else{
      p->iOffset = pC->aType[p->iCol + pC->nField];
      p->nByte = capdbVdbeSerialTypeLen(type);
      p->pCsr =  pC->uc.pCursor;
      capdbBtreeIncrblobCursor(p->pCsr);
    }
  }

  if( rc==CAPDB_ROW ){
    rc = CAPDB_OK;
  }else if( p->pStmt ){
    rc = capdb_finalize(p->pStmt);
    p->pStmt = 0;
    if( rc==CAPDB_OK ){
      zErr = capdbMPrintf(p->db, "no such rowid: %lld", iRow);
      rc = CAPDB_ERROR;
    }else{
      zErr = capdbMPrintf(p->db, "%s", capdb_errmsg(p->db));
    }
  }

  assert( rc!=CAPDB_OK || zErr==0 );
  assert( rc!=CAPDB_ROW && rc!=CAPDB_DONE );

  *pzErr = zErr;
  return rc;
}

/*
** Open a blob handle.
*/
int capdb_blob_open(
  capdb* db,            /* The database connection */
  const char *zDb,        /* The attached database containing the blob */
  const char *zTable,     /* The table containing the blob */
  const char *zColumn,    /* The column containing the blob */
  sqlite_int64 iRow,      /* The row containing the glob */
  int wrFlag,             /* True -> read/write access, false -> read-only */
  capdb_blob **ppBlob   /* Handle for accessing the blob returned here */
){
  int nAttempt = 0;
  int iCol;               /* Index of zColumn in row-record */
  int rc = CAPDB_OK;
  char *zErr = 0;
  Table *pTab;
  Incrblob *pBlob = 0;
  int iDb;
  Parse sParse;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( ppBlob==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  *ppBlob = 0;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) || zTable==0 || zColumn==0 ){
    return CAPDB_MISUSE_BKPT;
  }
#endif
  wrFlag = !!wrFlag;                /* wrFlag = (wrFlag ? 1 : 0); */

  capdb_mutex_enter(db->mutex);

  pBlob = (Incrblob *)capdbDbMallocZero(db, sizeof(Incrblob));
  while(1){
    capdbParseObjectInit(&sParse,db);
    if( !pBlob ) goto blob_open_out;
    capdbDbFree(db, zErr);
    zErr = 0;

    capdbBtreeEnterAll(db);
    pTab = capdbLocateTable(&sParse, 0, zTable, zDb);
    if( pTab && IsVirtual(pTab) ){
      pTab = 0;
      capdbErrorMsg(&sParse, "cannot open virtual table: %s", zTable);
    }
    if( pTab && !HasRowid(pTab) ){
      pTab = 0;
      capdbErrorMsg(&sParse, "cannot open table without rowid: %s", zTable);
    }
    if( pTab && (pTab->tabFlags&TF_HasGenerated)!=0 ){
      pTab = 0;
      capdbErrorMsg(&sParse, "cannot open table with generated columns: %s",
                      zTable);
    }
#ifndef CAPDB_OMIT_VIEW
    if( pTab && IsView(pTab) ){
      pTab = 0;
      capdbErrorMsg(&sParse, "cannot open view: %s", zTable);
    }
#endif
    if( pTab==0
     || ((iDb = capdbSchemaToIndex(db, pTab->pSchema))==1 &&
         capdbOpenTempDatabase(&sParse))
    ){
      if( sParse.zErrMsg ){
        capdbDbFree(db, zErr);
        zErr = sParse.zErrMsg;
        sParse.zErrMsg = 0;
      }
      rc = CAPDB_ERROR;
      capdbBtreeLeaveAll(db);
      goto blob_open_out;
    }
    pBlob->pTab = pTab;
    pBlob->zDb = db->aDb[iDb].zDbSName;

    /* Now search pTab for the exact column. */
    iCol = capdbColumnIndex(pTab, zColumn);
    if( iCol<0 ){
      capdbDbFree(db, zErr);
      zErr = capdbMPrintf(db, "no such column: \"%s\"", zColumn);
      rc = CAPDB_ERROR;
      capdbBtreeLeaveAll(db);
      goto blob_open_out;
    }

    /* If the value is being opened for writing, check that the
    ** column is not indexed, and that it is not part of a foreign key. 
    */
    if( wrFlag ){
      const char *zFault = 0;
      Index *pIdx;
#ifndef CAPDB_OMIT_FOREIGN_KEY
      if( db->flags&CAPDB_ForeignKeys ){
        /* Check that the column is not part of an FK child key definition. It
        ** is not necessary to check if it is part of a parent key, as parent
        ** key columns must be indexed. The check below will pick up this 
        ** case.  */
        FKey *pFKey;
        assert( IsOrdinaryTable(pTab) );
        for(pFKey=pTab->u.tab.pFKey; pFKey; pFKey=pFKey->pNextFrom){
          int j;
          for(j=0; j<pFKey->nCol; j++){
            if( pFKey->aCol[j].iFrom==iCol ){
              zFault = "foreign key";
            }
          }
        }
      }
#endif
      for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
        int j;
        for(j=0; j<pIdx->nKeyCol; j++){
          /* FIXME: Be smarter about indexes that use expressions */
          if( pIdx->aiColumn[j]==iCol || pIdx->aiColumn[j]==XN_EXPR ){
            zFault = "indexed";
          }
        }
      }
      if( zFault ){
        capdbDbFree(db, zErr);
        zErr = capdbMPrintf(db, "cannot open %s column for writing", zFault);
        rc = CAPDB_ERROR;
        capdbBtreeLeaveAll(db);
        goto blob_open_out;
      }
    }

    pBlob->pStmt = (capdb_stmt *)capdbVdbeCreate(&sParse);
    assert( pBlob->pStmt || db->mallocFailed );
    if( pBlob->pStmt ){
      
      /* This VDBE program seeks a btree cursor to the identified 
      ** db/table/row entry. The reason for using a vdbe program instead
      ** of writing code to use the b-tree layer directly is that the
      ** vdbe program will take advantage of the various transaction,
      ** locking and error handling infrastructure built into the vdbe.
      **
      ** After seeking the cursor, the vdbe executes an OP_ResultRow.
      ** Code external to the Vdbe then "borrows" the b-tree cursor and
      ** uses it to implement the blob_read(), blob_write() and 
      ** blob_bytes() functions.
      **
      ** The capdb_blob_close() function finalizes the vdbe program,
      ** which closes the b-tree cursor and (possibly) commits the 
      ** transaction.
      */
      static const int iLn = VDBE_OFFSET_LINENO(2);
      static const VdbeOpList openBlob[] = {
        {OP_TableLock,      0, 0, 0},  /* 0: Acquire a read or write lock */
        {OP_OpenRead,       0, 0, 0},  /* 1: Open a cursor */
        /* blobSeekToRow() will initialize r[1] to the desired rowid */
        {OP_NotExists,      0, 5, 1},  /* 2: Seek the cursor to rowid=r[1] */
        {OP_Column,         0, 0, 1},  /* 3  */
        {OP_ResultRow,      1, 0, 0},  /* 4  */
        {OP_Halt,           0, 0, 0},  /* 5  */
      };
      Vdbe *v = (Vdbe *)pBlob->pStmt;
      VdbeOp *aOp;

      capdbVdbeAddOp4Int(v, OP_Transaction, iDb, wrFlag, 
                           pTab->pSchema->schema_cookie,
                           pTab->pSchema->iGeneration);
      capdbVdbeChangeP5(v, 1);
      assert( capdbVdbeCurrentAddr(v)==2 || db->mallocFailed );
      aOp = capdbVdbeAddOpList(v, ArraySize(openBlob), openBlob, iLn);

      /* Make sure a mutex is held on the table to be accessed */
      capdbVdbeUsesBtree(v, iDb); 

      if( db->mallocFailed==0 ){
        assert( aOp!=0 );
        /* Configure the OP_TableLock instruction */
#ifdef CAPDB_OMIT_SHARED_CACHE
        aOp[0].opcode = OP_Noop;
#else
        aOp[0].p1 = iDb;
        aOp[0].p2 = pTab->tnum;
        aOp[0].p3 = wrFlag;
        capdbVdbeChangeP4(v, 2, pTab->zName, P4_TRANSIENT);
      }
      if( db->mallocFailed==0 ){
#endif

        /* Remove either the OP_OpenWrite or OpenRead. Set the P2 
        ** parameter of the other to pTab->tnum.  */
        if( wrFlag ) aOp[1].opcode = OP_OpenWrite;
        aOp[1].p2 = pTab->tnum;
        aOp[1].p3 = iDb;   

        /* Configure the number of columns. Configure the cursor to
        ** think that the table has one more column than it really
        ** does. An OP_Column to retrieve this imaginary column will
        ** always return an SQL NULL. This is useful because it means
        ** we can invoke OP_Column to fill in the vdbe cursors type 
        ** and offset cache without causing any IO.
        */
        aOp[1].p4type = P4_INT32;
        aOp[1].p4.i = pTab->nCol+1;
        aOp[3].p2 = pTab->nCol;

        sParse.nVar = 0;
        sParse.nMem = 1;
        sParse.nTab = 1;
        capdbVdbeMakeReady(v, &sParse);
      }
    }
   
    pBlob->iCol = iCol;
    pBlob->db = db;
    capdbBtreeLeaveAll(db);
    if( db->mallocFailed ){
      goto blob_open_out;
    }
    rc = blobSeekToRow(pBlob, iRow, &zErr);
    if( (++nAttempt)>=CAPDB_MAX_SCHEMA_RETRY || rc!=CAPDB_SCHEMA ) break;
    capdbParseObjectReset(&sParse);
  }

blob_open_out:
  if( rc==CAPDB_OK && db->mallocFailed==0 ){
    *ppBlob = (capdb_blob *)pBlob;
  }else{
    if( pBlob && pBlob->pStmt ) capdbVdbeFinalize((Vdbe *)pBlob->pStmt);
    capdbDbFree(db, pBlob);
  }
  capdbErrorWithMsg(db, rc, (zErr ? "%s" : (char*)0), zErr);
  capdbDbFree(db, zErr);
  capdbParseObjectReset(&sParse);
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Close a blob handle that was previously created using
** capdb_blob_open().
*/
int capdb_blob_close(capdb_blob *pBlob){
  Incrblob *p = (Incrblob *)pBlob;
  int rc;
  capdb *db;

  if( p ){
    capdb_stmt *pStmt = p->pStmt;
    db = p->db;
    capdb_mutex_enter(db->mutex);
    capdbDbFree(db, p);
    capdb_mutex_leave(db->mutex);
    rc = capdb_finalize(pStmt);
  }else{
    rc = CAPDB_OK;
  }
  return rc;
}

/*
** Perform a read or write operation on a blob
*/
static int blobReadWrite(
  capdb_blob *pBlob, 
  void *z, 
  int n, 
  int iOffset, 
  int (*xCall)(BtCursor*, u32, u32, void*)
){
  int rc = CAPDB_OK;
  Incrblob *p = (Incrblob *)pBlob;
  Vdbe *v;
  capdb *db;

  if( p==0 ) return CAPDB_MISUSE_BKPT;
  db = p->db;
  capdb_mutex_enter(db->mutex);
  v = (Vdbe*)p->pStmt;

  if( n<0 || iOffset<0 || ((capdb_int64)iOffset+n)>p->nByte ){
    /* Request is out of range. Return a transient error. */
    rc = CAPDB_ERROR;
  }else if( v==0 ){
    /* If there is no statement handle, then the blob-handle has
    ** already been invalidated. Return CAPDB_ABORT in this case.
    */
    rc = CAPDB_ABORT;
  }else{
    /* Call either BtreeData() or BtreePutData(). If CAPDB_ABORT is
    ** returned, clean-up the statement handle.
    */
    assert( db == v->db );
    capdbBtreeEnterCursor(p->pCsr);

#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
    if( xCall==capdbBtreePutData && db->xPreUpdateCallback ){
      /* If a pre-update hook is registered and this is a write cursor, 
      ** invoke it here. 
      ** 
      ** TODO: The preupdate-hook is passed CAPDB_DELETE, even though this
      ** operation should really be an CAPDB_UPDATE. This is probably
      ** incorrect, but is convenient because at this point the new.* values 
      ** are not easily obtainable. And for the sessions module, an 
      ** CAPDB_UPDATE where the PK columns do not change is handled in the 
      ** same way as an CAPDB_DELETE (the CAPDB_DELETE code is actually
      ** slightly more efficient). Since you cannot write to a PK column
      ** using the incremental-blob API, this works. For the sessions module
      ** anyhow.
      */
      if( capdbBtreeCursorIsValidNN(p->pCsr)==0 ){
        /* If the cursor is not currently valid, try to reseek it. This 
        ** always either fails or finds the correct row - the cursor will
        ** have been marked permanently CURSOR_INVALID if the open row has
        ** been deleted.  */
        int bDiff = 0;
        rc = capdbBtreeCursorRestore(p->pCsr, &bDiff);
        assert( bDiff==0 || capdbBtreeCursorIsValidNN(p->pCsr)==0 );
      }
      if( capdbBtreeCursorIsValidNN(p->pCsr) ){
        capdb_int64 iKey;
        iKey = capdbBtreeIntegerKey(p->pCsr);
        assert( v->apCsr[0]!=0 );
        assert( v->apCsr[0]->eCurType==CURTYPE_BTREE );
        capdbVdbePreUpdateHook(
            v, v->apCsr[0], CAPDB_DELETE, p->zDb, p->pTab, iKey, -1, p->iCol
        );
      }
    }
    if( rc==CAPDB_OK ){
      rc = xCall(p->pCsr, iOffset+p->iOffset, n, z);
    }
#else
    rc = xCall(p->pCsr, iOffset+p->iOffset, n, z);
#endif

    capdbBtreeLeaveCursor(p->pCsr);
    if( rc==CAPDB_ABORT ){
      capdbVdbeFinalize(v);
      p->pStmt = 0;
    }else{
      v->rc = rc;
    }
  }
  capdbError(db, rc);
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Read data from a blob handle.
*/
int capdb_blob_read(capdb_blob *pBlob, void *z, int n, int iOffset){
  return blobReadWrite(pBlob, z, n, iOffset, capdbBtreePayloadChecked);
}

/*
** Write data to a blob handle.
*/
int capdb_blob_write(capdb_blob *pBlob, const void *z, int n, int iOffset){
  return blobReadWrite(pBlob, (void *)z, n, iOffset, capdbBtreePutData);
}

/*
** Query a blob handle for the size of the data.
**
** The Incrblob.nByte field is fixed for the lifetime of the Incrblob
** so no mutex is required for access.
*/
int capdb_blob_bytes(capdb_blob *pBlob){
  Incrblob *p = (Incrblob *)pBlob;
  return (p && p->pStmt) ? p->nByte : 0;
}

/*
** Move an existing blob handle to point to a different row of the same
** database table.
**
** If an error occurs, or if the specified row does not exist or does not
** contain a blob or text value, then an error code is returned and the
** database handle error code and message set. If this happens, then all 
** subsequent calls to capdb_blob_xxx() functions (except blob_close()) 
** immediately return CAPDB_ABORT.
*/
int capdb_blob_reopen(capdb_blob *pBlob, capdb_int64 iRow){
  int rc;
  Incrblob *p = (Incrblob *)pBlob;
  capdb *db;

  if( p==0 ) return CAPDB_MISUSE_BKPT;
  db = p->db;
  capdb_mutex_enter(db->mutex);

  if( p->pStmt==0 ){
    /* If there is no statement handle, then the blob-handle has
    ** already been invalidated. Return CAPDB_ABORT in this case.
    */
    rc = CAPDB_ABORT;
  }else{
    char *zErr;
    ((Vdbe*)p->pStmt)->rc = CAPDB_OK;
    rc = blobSeekToRow(p, iRow, &zErr);
    if( rc!=CAPDB_OK ){
      capdbErrorWithMsg(db, rc, (zErr ? "%s" : (char*)0), zErr);
      capdbDbFree(db, zErr);
    }
    assert( rc!=CAPDB_SCHEMA );
  }

  rc = capdbApiExit(db, rc);
  assert( rc==CAPDB_OK || p->pStmt==0 );
  capdb_mutex_leave(db->mutex);
  return rc;
}

#endif /* #ifndef CAPDB_OMIT_INCRBLOB */
