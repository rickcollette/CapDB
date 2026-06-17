/*
** 2009 January 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation of the capdb_backup_XXX() 
** API functions and the related features.
*/
#include "capdbInt.h"
#include "btreeInt.h"

/*
** Structure allocated for each backup operation.
*/
struct capdb_backup {
  capdb* pDestDb;        /* Destination database handle */
  Db *pDest;               /* Destination db file */
  u32 iDestSchema;         /* Original schema cookie in destination */
  int bDestLocked;         /* True once a write-transaction is open on pDest */

  Pgno iNext;              /* Page number of the next source page to copy */
  capdb* pSrcDb;         /* Source database handle */
  Db *pSrc;                /* Source db file */

  int rc;                  /* Backup process error code */

  /* These two variables are set by every call to backup_step(). They are
  ** read by calls to backup_remaining() and backup_pagecount().
  */
  Pgno nRemaining;         /* Number of pages left to copy */
  Pgno nPagecount;         /* Total number of pages to copy */

  int isAttached;          /* True once backup has been registered with pager */
  capdb_backup *pNext;   /* Next backup associated with source pager */
};

/*
** THREAD SAFETY NOTES:
**
**   Once it has been created using backup_init(), a single capdb_backup
**   structure may be accessed via two groups of thread-safe entry points:
**
**     * Via the capdb_backup_XXX() API function backup_step() and 
**       backup_finish(). Both these functions obtain the source database
**       handle mutex and the mutex associated with the source BtShared 
**       structure, in that order.
**
**     * Via the BackupUpdate() and BackupRestart() functions, which are
**       invoked by the pager layer to report various state changes in
**       the page cache associated with the source database. The mutex
**       associated with the source database BtShared structure will always 
**       be held when either of these functions are invoked.
**
**   The other capdb_backup_XXX() API functions, backup_remaining() and
**   backup_pagecount() are not thread-safe functions. If they are called
**   while some other thread is calling backup_step() or backup_finish(),
**   the values returned may be invalid. There is no way for a call to
**   BackupUpdate() or BackupRestart() to interfere with backup_remaining()
**   or backup_pagecount().
**
**   Depending on the SQLite configuration, the database handles and/or
**   the Btree objects may have their own mutexes that require locking.
**   Non-sharable Btrees (in-memory databases for example), do not have
**   associated mutexes.
*/

/*
** Return a pointer corresponding to database zDb (i.e. "main", "temp")
** in connection handle pDb. If such a database cannot be found, return
** a NULL pointer and write an error message to pErrorDb.
**
** If the "temp" database is requested, it may need to be opened by this 
** function. If an error occurs while doing so, return 0 and write an 
** error message to pErrorDb.
*/
static Db *findDatabase(capdb *pErrorDb, capdb *pDb, const char *zDb){
  int i = capdbFindDbName(pDb, zDb);

  if( i==1 ){
    Parse sParse;
    int rc = 0;
    capdbParseObjectInit(&sParse,pDb);
    if( capdbOpenTempDatabase(&sParse) ){
      capdbErrorWithMsg(pErrorDb, sParse.rc, "%s", sParse.zErrMsg);
      rc = CAPDB_ERROR;
    }
    capdbDbFree(pErrorDb, sParse.zErrMsg);
    capdbParseObjectReset(&sParse);
    if( rc ){
      return 0;
    }
  }

  if( i<0 ){
    capdbErrorWithMsg(pErrorDb, CAPDB_ERROR, "unknown database %s", zDb);
    return 0;
  }

  return &pDb->aDb[i];
}

/*
** Attempt to set the page size of the destination to match the page size
** of the source.
*/
static int setDestPgsz(capdb_backup *p){
  return capdbBtreeSetPageSize(p->pDest->pBt,
      capdbBtreeGetPageSize(p->pSrc->pBt), 0, 0
  );
}

/*
** Check that there is no open read-transaction on the b-tree passed as the
** second argument. If there is not, return CAPDB_OK. Otherwise, if there
** is an open read-transaction, return CAPDB_ERROR and leave an error 
** message in database handle db.
*/
static int checkReadTransaction(capdb *db, Btree *p){
  if( capdbBtreeTxnState(p)!=CAPDB_TXN_NONE ){
    capdbErrorWithMsg(db, CAPDB_ERROR, "destination database is in use");
    return CAPDB_ERROR;
  }
  return CAPDB_OK;
}

/*
** Create an capdb_backup process to copy the contents of zSrcDb from
** connection handle pSrcDb to zDestDb in pDestDb. If successful, return
** a pointer to the new capdb_backup object.
**
** If an error occurs, NULL is returned and an error code and error message
** stored in database handle pDestDb.
*/
capdb_backup *capdb_backup_init(
  capdb* pDestDb,                     /* Database to write to */
  const char *zDestDb,                  /* Name of database within pDestDb */
  capdb* pSrcDb,                      /* Database connection to read from */
  const char *zSrcDb                    /* Name of database within pSrcDb */
){
  capdb_backup *p;                    /* Value to return */

#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(pSrcDb)||!capdbSafetyCheckOk(pDestDb) ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif

  /* Lock the source database handle. The destination database
  ** handle is not locked in this routine, but it is locked in
  ** capdb_backup_step(). The user is required to ensure that no
  ** other thread accesses the destination handle for the duration
  ** of the backup operation.  Any attempt to use the destination
  ** database connection while a backup is in progress may cause
  ** a malfunction or a deadlock.
  */
  capdb_mutex_enter(pSrcDb->mutex);
  capdb_mutex_enter(pDestDb->mutex);

  if( pSrcDb==pDestDb ){
    capdbErrorWithMsg(
        pDestDb, CAPDB_ERROR, "source and destination must be distinct"
    );
    p = 0;
  }else {
    /* Allocate space for a new capdb_backup object...
    ** EVIDENCE-OF: R-64852-21591 The capdb_backup object is created by a
    ** call to capdb_backup_init() and is destroyed by a call to
    ** capdb_backup_finish(). */
    p = (capdb_backup *)capdbMallocZero(sizeof(capdb_backup));
    if( !p ){
      capdbError(pDestDb, CAPDB_NOMEM_BKPT);
    }
  }

  /* If the allocation succeeded, populate the new object. */
  if( p ){
    p->pSrc = findDatabase(pDestDb, pSrcDb, zSrcDb);
    p->pDest = findDatabase(pDestDb, pDestDb, zDestDb);
    p->pDestDb = pDestDb;
    p->pSrcDb = pSrcDb;
    p->iNext = 1;
    p->isAttached = 0;

    if( 0==p->pSrc || 0==p->pDest 
     || checkReadTransaction(pDestDb, p->pDest->pBt)!=CAPDB_OK 
     ){
      /* One (or both) of the named databases did not exist or an OOM
      ** error was hit. Or there is a transaction open on the destination
      ** database. The error has already been written into the pDestDb 
      ** handle. All that is left to do here is free the capdb_backup 
      ** structure.  */
      capdb_free(p);
      p = 0;
    }
  }
  if( p ){
    p->pSrc->pBt->nBackup++;
  }

  capdb_mutex_leave(pDestDb->mutex);
  capdb_mutex_leave(pSrcDb->mutex);
  return p;
}

/*
** Argument rc is an SQLite error code. Return true if this error is 
** considered fatal if encountered during a backup operation. All errors
** are considered fatal except for CAPDB_BUSY and CAPDB_LOCKED.
*/
static int isFatalError(int rc){
  return (rc!=CAPDB_OK && rc!=CAPDB_BUSY && ALWAYS(rc!=CAPDB_LOCKED));
}

/*
** Parameter zSrcData points to a buffer containing the data for 
** page iSrcPg from the source database. Copy this data into the 
** destination database.
*/
static int backupOnePage(
  capdb_backup *p,              /* Backup handle */
  Pgno iSrcPg,                    /* Source database page to backup */
  const u8 *zSrcData,             /* Source database page data */
  int bUpdate                     /* True for an update, false otherwise */
){
  Pager * const pDestPager = capdbBtreePager(p->pDest->pBt);
  const int nSrcPgsz = capdbBtreeGetPageSize(p->pSrc->pBt);
  int nDestPgsz = capdbBtreeGetPageSize(p->pDest->pBt);
  const int nCopy = MIN(nSrcPgsz, nDestPgsz);
  const i64 iEnd = (i64)iSrcPg*(i64)nSrcPgsz;
  int rc = CAPDB_OK;
  i64 iOff;

  assert( capdbBtreeGetReserveNoMutex(p->pSrc->pBt)>=0 );
  assert( p->bDestLocked );
  assert( !isFatalError(p->rc) );
  assert( iSrcPg!=PENDING_BYTE_PAGE(p->pSrc->pBt->pBt) );
  assert( zSrcData );
  assert( nSrcPgsz==nDestPgsz || capdbPagerIsMemdb(pDestPager)==0 );

  /* This loop runs once for each destination page spanned by the source 
  ** page. For each iteration, variable iOff is set to the byte offset
  ** of the destination page.
  */
  for(iOff=iEnd-(i64)nSrcPgsz; rc==CAPDB_OK && iOff<iEnd; iOff+=nDestPgsz){
    DbPage *pDestPg = 0;
    Pgno iDest = (Pgno)(iOff/nDestPgsz)+1;
    if( iDest==PENDING_BYTE_PAGE(p->pDest->pBt->pBt) ) continue;
    if( CAPDB_OK==(rc = capdbPagerGet(pDestPager, iDest, &pDestPg, 0))
     && CAPDB_OK==(rc = capdbPagerWrite(pDestPg))
    ){
      const u8 *zIn = &zSrcData[iOff%nSrcPgsz];
      u8 *zDestData = capdbPagerGetData(pDestPg);
      u8 *zOut = &zDestData[iOff%nDestPgsz];

      /* Copy the data from the source page into the destination page.
      ** Then clear the Btree layer MemPage.isInit flag. Both this module
      ** and the pager code use this trick (clearing the first byte
      ** of the page 'extra' space to invalidate the Btree layers
      ** cached parse of the page). MemPage.isInit is marked 
      ** "MUST BE FIRST" for this purpose.
      */
      memcpy(zOut, zIn, nCopy);
      ((u8 *)capdbPagerGetExtra(pDestPg))[0] = 0;
      if( iOff==0 && bUpdate==0 ){
        capdbPut4byte(&zOut[28], capdbBtreeLastPage(p->pSrc->pBt));
      }
    }
    capdbPagerUnref(pDestPg);
  }

  return rc;
}

/*
** If pFile is currently larger than iSize bytes, then truncate it to
** exactly iSize bytes. If pFile is not larger than iSize bytes, then
** this function is a no-op.
**
** Return CAPDB_OK if everything is successful, or an SQLite error 
** code if an error occurs.
*/
static int backupTruncateFile(capdb_file *pFile, i64 iSize){
  i64 iCurrent;
  int rc = capdbOsFileSize(pFile, &iCurrent);
  if( rc==CAPDB_OK && iCurrent>iSize ){
    rc = capdbOsTruncate(pFile, iSize);
  }
  return rc;
}

/*
** Register this backup object with the associated source pager for
** callbacks when pages are changed or the cache invalidated.
*/
static void attachBackupObject(capdb_backup *p){
  capdb_backup **pp;
  assert( capdbBtreeHoldsMutex(p->pSrc->pBt) );
  pp = capdbPagerBackupPtr(capdbBtreePager(p->pSrc->pBt));
  p->pNext = *pp;
  *pp = p;
  p->isAttached = 1;
}

/*
** Copy nPage pages from the source b-tree to the destination.
*/
int capdb_backup_step(capdb_backup *p, int nPage){
  int rc;
  int destMode;       /* Destination journal mode */
  int pgszSrc = 0;    /* Source page size */
  int pgszDest = 0;   /* Destination page size */
  Btree *pDest;
  Btree *pSrc;

#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ) return CAPDB_MISUSE_BKPT;
#endif
  assert( p->pDest );
  assert( p->pSrc );
  pDest = p->pDest->pBt;
  pSrc = p->pSrc->pBt;
  capdb_mutex_enter(p->pSrcDb->mutex);
  capdbBtreeEnter(pSrc);
  if( p->pDestDb ){
    capdb_mutex_enter(p->pDestDb->mutex);
  }

  rc = p->rc;
  if( !isFatalError(rc) ){
    Pager * const pSrcPager = capdbBtreePager(pSrc);     /* Source pager */
    Pager * const pDestPager = capdbBtreePager(pDest);   /* Dest pager */
    int ii;                            /* Iterator variable */
    int nSrcPage = -1;                 /* Size of source db in pages */
    int bCloseTrans = 0;               /* True if src db requires unlocking */

    /* If the source pager is currently in a write-transaction, return
    ** CAPDB_BUSY immediately.
    */
    if( p->pDestDb && pSrc->pBt->inTransaction==TRANS_WRITE ){
      rc = CAPDB_BUSY;
    }else{
      rc = CAPDB_OK;
    }

    /* If there is no open read-transaction on the source database, open
    ** one now. If a transaction is opened here, then it will be closed
    ** before this function exits.
    */
    if( rc==CAPDB_OK && CAPDB_TXN_NONE==capdbBtreeTxnState(pSrc) ){
      rc = capdbBtreeBeginTrans(pSrc, 0, 0);
      bCloseTrans = 1;
    }

    /* If the destination database has not yet been locked (i.e. if this
    ** is the first call to backup_step() for the current backup operation),
    ** try to set its page size to the same as the source database. This
    ** is especially important on ZipVFS systems, as in that case it is
    ** not possible to create a database file that uses one page size by
    ** writing to it with another.  */
    if( p->bDestLocked==0 && rc==CAPDB_OK && setDestPgsz(p)==CAPDB_NOMEM ){
      rc = CAPDB_NOMEM;
    }

    /* Lock the destination database, if it is not locked already. */
    if( CAPDB_OK==rc && p->bDestLocked==0
     && CAPDB_OK==(rc = capdbBtreeBeginTrans(pDest, 2,
                                                (int*)&p->iDestSchema)) 
    ){
      p->bDestLocked = 1;
    }

    /* Do not allow backup if the destination database is in WAL mode
    ** and the page sizes are different between source and destination */
    pgszSrc = capdbBtreeGetPageSize(pSrc);
    pgszDest = capdbBtreeGetPageSize(pDest);
    destMode = capdbPagerGetJournalMode(capdbBtreePager(pDest));
    if( CAPDB_OK==rc 
     && (destMode==PAGER_JOURNALMODE_WAL || capdbPagerIsMemdb(pDestPager))
     && pgszSrc!=pgszDest 
    ){
      rc = CAPDB_READONLY;
    }
  
    /* Now that there is a read-lock on the source database, query the
    ** source pager for the number of pages in the database.
    */
    nSrcPage = (int)capdbBtreeLastPage(pSrc);
    assert( nSrcPage>=0 );
    for(ii=0; (nPage<0 || ii<nPage) && p->iNext<=(Pgno)nSrcPage && !rc; ii++){
      const Pgno iSrcPg = p->iNext;                 /* Source page number */
      if( iSrcPg!=PENDING_BYTE_PAGE(pSrc->pBt) ){
        DbPage *pSrcPg;                             /* Source page object */
        rc = capdbPagerGet(pSrcPager, iSrcPg, &pSrcPg,PAGER_GET_READONLY);
        if( rc==CAPDB_OK ){
          rc = backupOnePage(p, iSrcPg, capdbPagerGetData(pSrcPg), 0);
          capdbPagerUnref(pSrcPg);
        }
      }
      p->iNext++;
    }
    if( rc==CAPDB_OK ){
      p->nPagecount = nSrcPage;
      p->nRemaining = nSrcPage+1-p->iNext;
      if( p->iNext>(Pgno)nSrcPage ){
        rc = CAPDB_DONE;
      }else if( !p->isAttached ){
        attachBackupObject(p);
      }
    }
  
    /* Update the schema version field in the destination database. This
    ** is to make sure that the schema-version really does change in
    ** the case where the source and destination databases have the
    ** same schema version.
    */
    if( rc==CAPDB_DONE ){
      if( nSrcPage==0 ){
        rc = capdbBtreeNewDb(pDest);
        nSrcPage = 1;
      }
      if( rc==CAPDB_OK || rc==CAPDB_DONE ){
        rc = capdbBtreeUpdateMeta(pDest,1,p->iDestSchema+1);
      }
      if( rc==CAPDB_OK ){
        if( p->pDestDb ){
          capdbResetAllSchemasOfConnection(p->pDestDb);
        }
        if( destMode==PAGER_JOURNALMODE_WAL ){
          rc = capdbBtreeSetVersion(pDest, 2);
        }
      }
      if( rc==CAPDB_OK ){
        int nDestTruncate;
        /* Set nDestTruncate to the final number of pages in the destination
        ** database. The complication here is that the destination page
        ** size may be different to the source page size. 
        **
        ** If the source page size is smaller than the destination page size, 
        ** round up. In this case the call to capdbOsTruncate() below will
        ** fix the size of the file. However it is important to call
        ** capdbPagerTruncateImage() here so that any pages in the 
        ** destination file that lie beyond the nDestTruncate page mark are
        ** journalled by PagerCommitPhaseOne() before they are destroyed
        ** by the file truncation.
        */
        assert( pgszSrc==capdbBtreeGetPageSize(pSrc) );
        assert( pgszDest==capdbBtreeGetPageSize(pDest) );
        if( pgszSrc<pgszDest ){
          int ratio = pgszDest/pgszSrc;
          nDestTruncate = (nSrcPage+ratio-1)/ratio;
          if( nDestTruncate==(int)PENDING_BYTE_PAGE(pDest->pBt) ){
            nDestTruncate--;
          }
        }else{
          nDestTruncate = nSrcPage * (pgszSrc/pgszDest);
        }
        assert( nDestTruncate>0 );

        if( pgszSrc<pgszDest ){
          /* If the source page-size is smaller than the destination page-size,
          ** two extra things may need to happen:
          **
          **   * The destination may need to be truncated, and
          **
          **   * Data stored on the pages immediately following the 
          **     pending-byte page in the source database may need to be
          **     copied into the destination database.
          */
          const i64 iSize = (i64)pgszSrc * (i64)nSrcPage;
          capdb_file * const pFile = capdbPagerFile(pDestPager);
          Pgno iPg;
          int nDstPage;
          i64 iOff;
          i64 iEnd;

          assert( pFile );
          assert( nDestTruncate==0 
              || (i64)nDestTruncate*(i64)pgszDest >= iSize || (
                nDestTruncate==(int)(PENDING_BYTE_PAGE(pDest->pBt)-1)
             && iSize>=PENDING_BYTE && iSize<=PENDING_BYTE+pgszDest
          ));

          /* This block ensures that all data required to recreate the original
          ** database has been stored in the journal for pDestPager and the
          ** journal synced to disk. So at this point we may safely modify
          ** the database file in any way, knowing that if a power failure
          ** occurs, the original database will be reconstructed from the 
          ** journal file.  */
          capdbPagerPagecount(pDestPager, &nDstPage);
          for(iPg=nDestTruncate; rc==CAPDB_OK && iPg<=(Pgno)nDstPage; iPg++){
            if( iPg!=PENDING_BYTE_PAGE(pDest->pBt) ){
              DbPage *pPg;
              rc = capdbPagerGet(pDestPager, iPg, &pPg, 0);
              if( rc==CAPDB_OK ){
                rc = capdbPagerWrite(pPg);
                capdbPagerUnref(pPg);
              }
            }
          }
          if( rc==CAPDB_OK ){
            rc = capdbPagerCommitPhaseOne(pDestPager, 0, 1);
          }

          /* Write the extra pages and truncate the database file as required */
          iEnd = MIN(PENDING_BYTE + pgszDest, iSize);
          for(
            iOff=PENDING_BYTE+pgszSrc; 
            rc==CAPDB_OK && iOff<iEnd; 
            iOff+=pgszSrc
          ){
            PgHdr *pSrcPg = 0;
            const Pgno iSrcPg = (Pgno)((iOff/pgszSrc)+1);
            rc = capdbPagerGet(pSrcPager, iSrcPg, &pSrcPg, 0);
            if( rc==CAPDB_OK ){
              u8 *zData = capdbPagerGetData(pSrcPg);
              rc = capdbOsWrite(pFile, zData, pgszSrc, iOff);
            }
            capdbPagerUnref(pSrcPg);
          }
          if( rc==CAPDB_OK ){
            rc = backupTruncateFile(pFile, iSize);
          }

          /* Sync the database file to disk. */
          if( rc==CAPDB_OK ){
            rc = capdbPagerSync(pDestPager, 0);
          }
        }else{
          capdbPagerTruncateImage(pDestPager, nDestTruncate);
          rc = capdbPagerCommitPhaseOne(pDestPager, 0, 0);
        }
    
        /* Finish committing the transaction to the destination database. */
        if( CAPDB_OK==rc
         && CAPDB_OK==(rc = capdbBtreeCommitPhaseTwo(pDest, 0))
        ){
          rc = CAPDB_DONE;
        }
      }
    }
  
    /* If bCloseTrans is true, then this function opened a read transaction
    ** on the source database. Close the read transaction here. There is
    ** no need to check the return values of the btree methods here, as
    ** "committing" a read-only transaction cannot fail.
    */
    if( bCloseTrans ){
      TESTONLY( int rc2 );
      TESTONLY( rc2  = ) capdbBtreeCommitPhaseOne(pSrc, 0);
      TESTONLY( rc2 |= ) capdbBtreeCommitPhaseTwo(pSrc, 0);
      assert( rc2==CAPDB_OK );
    }
  
    if( rc==CAPDB_IOERR_NOMEM ){
      rc = CAPDB_NOMEM_BKPT;
    }
    p->rc = rc;
  }
  if( p->pDestDb ){
    capdb_mutex_leave(p->pDestDb->mutex);
  }
  capdbBtreeLeave(pSrc);
  capdb_mutex_leave(p->pSrcDb->mutex);
  return rc;
}

/*
** Release all resources associated with an capdb_backup* handle.
*/
int capdb_backup_finish(capdb_backup *p){
  capdb_backup **pp;                 /* Ptr to head of pagers backup list */
  capdb *pSrcDb;                     /* Source database connection */
  int rc;                              /* Value to return */

  /* Enter the mutexes */
  if( p==0 ) return CAPDB_OK;
  pSrcDb = p->pSrcDb;
  capdb_mutex_enter(pSrcDb->mutex);
  capdbBtreeEnter(p->pSrc->pBt);
  if( p->pDestDb ){
    capdb_mutex_enter(p->pDestDb->mutex);
  }

  /* Detach this backup from the source pager. */
  if( p->pDestDb ){
    p->pSrc->pBt->nBackup--;
  }
  if( p->isAttached ){
    pp = capdbPagerBackupPtr(capdbBtreePager(p->pSrc->pBt));
    assert( pp!=0 );
    while( *pp!=p ){
      pp = &(*pp)->pNext;
      assert( pp!=0 );
    }
    *pp = p->pNext;
  }

  /* If a transaction is still open on the Btree, roll it back. */
  capdbBtreeRollback(p->pDest->pBt, CAPDB_OK, 0);

  /* Set the error code of the destination database handle. */
  rc = (p->rc==CAPDB_DONE) ? CAPDB_OK : p->rc;
  if( p->pDestDb ){
    capdbError(p->pDestDb, rc);

    /* Exit the mutexes and free the backup context structure. */
    capdbLeaveMutexAndCloseZombie(p->pDestDb);
  }
  capdbBtreeLeave(p->pSrc->pBt);
  if( p->pDestDb ){
    /* EVIDENCE-OF: R-64852-21591 The capdb_backup object is created by a
    ** call to capdb_backup_init() and is destroyed by a call to
    ** capdb_backup_finish(). */
    capdb_free(p);
  }
  capdbLeaveMutexAndCloseZombie(pSrcDb);
  return rc;
}

/*
** Return the number of pages still to be backed up as of the most recent
** call to capdb_backup_step().
*/
int capdb_backup_remaining(capdb_backup *p){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return p->nRemaining;
}

/*
** Return the total number of pages in the source database as of the most 
** recent call to capdb_backup_step().
*/
int capdb_backup_pagecount(capdb_backup *p){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( p==0 ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
  return p->nPagecount;
}

/*
** This function is called after the contents of page iPage of the
** source database have been modified. If page iPage has already been 
** copied into the destination database, then the data written to the
** destination is now invalidated. The destination copy of iPage needs
** to be updated with the new data before the backup operation is
** complete.
**
** It is assumed that the mutex associated with the BtShared object
** corresponding to the source database is held when this function is
** called.
*/
static CAPDB_NOINLINE void backupUpdate(
  capdb_backup *p,
  Pgno iPage,
  const u8 *aData
){
  assert( p!=0 );
  do{
    assert( capdb_mutex_held(p->pSrc->pBt->pBt->mutex) );
    if( !isFatalError(p->rc) && iPage<p->iNext ){
      /* The backup process p has already copied page iPage. But now it
      ** has been modified by a transaction on the source pager. Copy
      ** the new data into the backup.
      */
      int rc;
      assert( p->pDestDb );
      capdb_mutex_enter(p->pDestDb->mutex);
      rc = backupOnePage(p, iPage, aData, 1);
      capdb_mutex_leave(p->pDestDb->mutex);
      assert( rc!=CAPDB_BUSY && rc!=CAPDB_LOCKED );
      if( rc!=CAPDB_OK ){
        p->rc = rc;
      }
    }
  }while( (p = p->pNext)!=0 );
}
void capdbBackupUpdate(capdb_backup *pBackup, Pgno iPage, const u8 *aData){
  if( pBackup ) backupUpdate(pBackup, iPage, aData);
}

/*
** Restart the backup process. This is called when the pager layer
** detects that the database has been modified by an external database
** connection. In this case there is no way of knowing which of the
** pages that have been copied into the destination database are still 
** valid and which are not, so the entire process needs to be restarted.
**
** It is assumed that the mutex associated with the BtShared object
** corresponding to the source database is held when this function is
** called.
*/
void capdbBackupRestart(capdb_backup *pBackup){
  capdb_backup *p;                   /* Iterator variable */
  for(p=pBackup; p; p=p->pNext){
    assert( capdb_mutex_held(p->pSrc->pBt->pBt->mutex) );
    p->iNext = 1;
  }
}

#ifndef CAPDB_OMIT_VACUUM
/*
** Copy the complete content of pBtFrom into pBtTo.  A transaction
** must be active for both files.
**
** The size of file pTo may be reduced by this operation. If anything 
** goes wrong, the transaction on pTo is rolled back. If successful, the 
** transaction is committed before returning.
*/
int capdbBtreeCopyFile(Btree *pTo, Btree *pFrom){
  int rc;
  capdb_file *pFd;              /* File descriptor for database pTo */
  capdb_backup b;
  Db dbDest;
  Db dbSrc;
  capdbBtreeEnter(pTo);
  capdbBtreeEnter(pFrom);

  assert( capdbBtreeTxnState(pTo)==CAPDB_TXN_WRITE );
  pFd = capdbPagerFile(capdbBtreePager(pTo));
  if( pFd->pMethods ){
    i64 nByte = capdbBtreeGetPageSize(pFrom)*(i64)capdbBtreeLastPage(pFrom);
    rc = capdbOsFileControl(pFd, CAPDB_FCNTL_OVERWRITE, &nByte);
    if( rc==CAPDB_NOTFOUND ) rc = CAPDB_OK;
    if( rc ) goto copy_finished;
  }

  /* Set up an capdb_backup object. capdb_backup.pDestDb must be set
  ** to 0. This is used by the implementations of capdb_backup_step()
  ** and capdb_backup_finish() to detect that they are being called
  ** from this function, not directly by the user.
  */
  memset(&b, 0, sizeof(b));
  memset(&dbDest, 0, sizeof(dbDest));
  memset(&dbSrc, 0, sizeof(dbSrc));
  dbDest.pBt = pTo;
  dbSrc.pBt = pFrom;
  b.pSrcDb = pFrom->db;
  b.pSrc = &dbSrc;
  b.pDest = &dbDest;
  b.iNext = 1;

  /* 0x7FFFFFFF is the hard limit for the number of pages in a database
  ** file. By passing this as the number of pages to copy to
  ** capdb_backup_step(), we can guarantee that the copy finishes 
  ** within a single call (unless an error occurs). The assert() statement
  ** checks this assumption - (p->rc) should be set to either CAPDB_DONE 
  ** or an error code.  */
  capdb_backup_step(&b, 0x7FFFFFFF);
  assert( b.rc!=CAPDB_OK );

  rc = capdb_backup_finish(&b);
  if( rc==CAPDB_OK ){
    pTo->pBt->btsFlags &= ~BTS_PAGESIZE_FIXED;
  }else{
    capdbPagerClearCache(capdbBtreePager(pTo));
  }

  assert( capdbBtreeTxnState(pTo)!=CAPDB_TXN_WRITE );
copy_finished:
  capdbBtreeLeave(pFrom);
  capdbBtreeLeave(pTo);
  return rc;
}
#endif /* CAPDB_OMIT_VACUUM */
