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
** Code for testing the btree.c module in SQLite.  This code
** is not included in the SQLite library.  It is used for automated
** testing of the SQLite library.
*/
#include "capdbInt.h"
#include "btreeInt.h"
#include "tclsqlite.h"
#include <stdlib.h>
#include <string.h>

extern const char *capdbErrName(int);

/*
** A bogus capdb connection structure for use in the btree
** tests.
*/
static capdb sDb;
static int nRefSqlite3 = 0;

/*
** Usage:   btree_open FILENAME NCACHE
**
** Open a new database
*/
static int CAPDB_TCLAPI btree_open(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  Btree *pBt;
  int rc, nCache;
  char zBuf[100];
  int n;
  char *zFilename;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " FILENAME NCACHE FLAGS\"", NULL);
    return TCL_ERROR;
  }

  if( Tcl_GetInt(interp, argv[2], &nCache) ) return TCL_ERROR;
  nRefSqlite3++;
  if( nRefSqlite3==1 ){
    sDb.pVfs = capdb_vfs_find(0);
    sDb.mutex = capdbMutexAlloc(CAPDB_MUTEX_RECURSIVE);
    capdb_mutex_enter(sDb.mutex);
  }
  n = (int)strlen(argv[1]);
  zFilename = capdb_malloc( n+2 );
  if( zFilename==0 ) return TCL_ERROR;
  memcpy(zFilename, argv[1], n+1);
  zFilename[n+1] = 0;
  rc = capdbBtreeOpen(sDb.pVfs, zFilename, &sDb, &pBt, 0, 
     CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE | CAPDB_OPEN_MAIN_DB);
  capdb_free(zFilename);
  if( rc!=CAPDB_OK ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  capdbBtreeSetCacheSize(pBt, nCache);
  capdb_snprintf(sizeof(zBuf), zBuf,"%p", pBt);
  Tcl_AppendResult(interp, zBuf, NULL);
  return TCL_OK;
}

/*
** Usage:   btree_close ID
**
** Close the given database.
*/
static int CAPDB_TCLAPI btree_close(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  Btree *pBt;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pBt = capdbTestTextToPtr(argv[1]);
  rc = capdbBtreeClose(pBt);
  if( rc!=CAPDB_OK ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  nRefSqlite3--;
  if( nRefSqlite3==0 ){
    capdb_mutex_leave(sDb.mutex);
    capdb_mutex_free(sDb.mutex);
    sDb.mutex = 0;
    sDb.pVfs = 0;
  }
  return TCL_OK;
}


/*
** Usage:   btree_begin_transaction ID
**
** Start a new transaction
*/
static int CAPDB_TCLAPI btree_begin_transaction(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  Btree *pBt;
  int rc;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pBt = capdbTestTextToPtr(argv[1]);
  capdbBtreeEnter(pBt);
  rc = capdbBtreeBeginTrans(pBt, 1, 0);
  capdbBtreeLeave(pBt);
  if( rc!=CAPDB_OK ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:   btree_pager_stats ID
**
** Returns pager statistics
*/
static int CAPDB_TCLAPI btree_pager_stats(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  Btree *pBt;
  int i;
  int *a;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pBt = capdbTestTextToPtr(argv[1]);
 
  /* Normally in this file, with a b-tree handle opened using the 
  ** [btree_open] command it is safe to call capdbBtreeEnter() directly.
  ** But this function is sometimes called with a btree handle obtained
  ** from an open SQLite connection (using [btree_from_db]). In this case
  ** we need to obtain the mutex for the controlling SQLite handle before
  ** it is safe to call capdbBtreeEnter().
  */
  capdb_mutex_enter(pBt->db->mutex);

  capdbBtreeEnter(pBt);
  a = capdbPagerStats(capdbBtreePager(pBt));
  for(i=0; i<11; i++){
    static char *zName[] = {
      "ref", "page", "max", "size", "state", "err",
      "hit", "miss", "ovfl", "read", "write"
    };
    char zBuf[100];
    Tcl_AppendElement(interp, zName[i]);
    capdb_snprintf(sizeof(zBuf), zBuf,"%d",a[i]);
    Tcl_AppendElement(interp, zBuf);
  }
  capdbBtreeLeave(pBt);

  /* Release the mutex on the SQLite handle that controls this b-tree */
  capdb_mutex_leave(pBt->db->mutex);
  return TCL_OK;
}

/*
** Usage:   btree_cursor ID TABLENUM WRITEABLE
**
** Create a new cursor.  Return the ID for the cursor.
*/
static int CAPDB_TCLAPI btree_cursor(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  Btree *pBt;
  int iTable;
  BtCursor *pCur;
  int rc = CAPDB_OK;
  int wrFlag;
  char zBuf[30];

  if( argc!=4 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID TABLENUM WRITEABLE\"", NULL);
    return TCL_ERROR;
  }
  pBt = capdbTestTextToPtr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &iTable) ) return TCL_ERROR;
  if( Tcl_GetBoolean(interp, argv[3], &wrFlag) ) return TCL_ERROR;
  if( wrFlag ) wrFlag = BTREE_WRCSR;
  pCur = (BtCursor *)ckalloc(capdbBtreeCursorSize());
  memset(pCur, 0, capdbBtreeCursorSize());
  capdb_mutex_enter(pBt->db->mutex);
  capdbBtreeEnter(pBt);
#ifndef CAPDB_OMIT_SHARED_CACHE
  rc = capdbBtreeLockTable(pBt, iTable, !!wrFlag);
#endif
  if( rc==CAPDB_OK ){
    rc = capdbBtreeCursor(pBt, iTable, wrFlag, 0, pCur);
  }
  capdbBtreeLeave(pBt);
  capdb_mutex_leave(pBt->db->mutex);
  if( rc ){
    ckfree((char *)pCur);
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  capdb_snprintf(sizeof(zBuf), zBuf,"%p", pCur);
  Tcl_AppendResult(interp, zBuf, NULL);
  return CAPDB_OK;
}

/*
** Usage:   btree_close_cursor ID
**
** Close a cursor opened using btree_cursor.
*/
static int CAPDB_TCLAPI btree_close_cursor(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  BtCursor *pCur;
  int rc;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pCur = capdbTestTextToPtr(argv[1]);
#if CAPDB_THREADSAFE>0
  {
    Btree *pBt = pCur->pBtree;
    capdb_mutex_enter(pBt->db->mutex);
    capdbBtreeEnter(pBt);
    rc = capdbBtreeCloseCursor(pCur);
    capdbBtreeLeave(pBt);
    capdb_mutex_leave(pBt->db->mutex);
  }
#else
  rc = capdbBtreeCloseCursor(pCur);
#endif
  ckfree((char *)pCur);
  if( rc ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  return CAPDB_OK;
}

/*
** Usage:   btree_next ID
**
** Move the cursor to the next entry in the table.  Return 0 on success
** or 1 if the cursor was already on the last entry in the table or if
** the table is empty.
*/
static int CAPDB_TCLAPI btree_next(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  BtCursor *pCur;
  int rc;
  int res = 0;
  char zBuf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pCur = capdbTestTextToPtr(argv[1]);
  capdbBtreeEnter(pCur->pBtree);
  rc = capdbBtreeNext(pCur, 0);
  if( rc==CAPDB_DONE ){
    res = 1;
    rc = CAPDB_OK;
  }
  capdbBtreeLeave(pCur->pBtree);
  if( rc ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  capdb_snprintf(sizeof(zBuf),zBuf,"%d",res);
  Tcl_AppendResult(interp, zBuf, NULL);
  return CAPDB_OK;
}

/*
** Usage:   btree_first ID
**
** Move the cursor to the first entry in the table.  Return 0 if the
** cursor was left point to something and 1 if the table is empty.
*/
static int CAPDB_TCLAPI btree_first(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  BtCursor *pCur;
  int rc;
  int res = 0;
  char zBuf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pCur = capdbTestTextToPtr(argv[1]);
  capdbBtreeEnter(pCur->pBtree);
  rc = capdbBtreeFirst(pCur, &res);
  capdbBtreeLeave(pCur->pBtree);
  if( rc ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  capdb_snprintf(sizeof(zBuf),zBuf,"%d",res);
  Tcl_AppendResult(interp, zBuf, NULL);
  return CAPDB_OK;
}

/*
** Usage:   btree_eof ID
**
** Return TRUE if the given cursor is not pointing at a valid entry.
** Return FALSE if the cursor does point to a valid entry.
*/
static int CAPDB_TCLAPI btree_eof(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  BtCursor *pCur;
  int rc;
  char zBuf[50];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pCur = capdbTestTextToPtr(argv[1]);
  capdbBtreeEnter(pCur->pBtree);
  rc = capdbBtreeEof(pCur);
  capdbBtreeLeave(pCur->pBtree);
  capdb_snprintf(sizeof(zBuf),zBuf, "%d", rc);
  Tcl_AppendResult(interp, zBuf, NULL);
  return CAPDB_OK;
}

/*
** Usage:   btree_payload_size ID
**
** Return the number of bytes of payload
*/
static int CAPDB_TCLAPI btree_payload_size(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  BtCursor *pCur;
  u32 n;
  char zBuf[50];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pCur = capdbTestTextToPtr(argv[1]);
  capdbBtreeEnter(pCur->pBtree);
  n = capdbBtreePayloadSize(pCur);
  capdbBtreeLeave(pCur->pBtree);
  capdb_snprintf(sizeof(zBuf),zBuf, "%u", n);
  Tcl_AppendResult(interp, zBuf, NULL);
  return CAPDB_OK;
}

/*
** usage:   varint_test  START  MULTIPLIER  COUNT  INCREMENT
**
** This command tests the putVarint() and getVarint()
** routines, both for accuracy and for speed.
**
** An integer is written using putVarint() and read back with
** getVarint() and verified to be unchanged.  This repeats COUNT
** times.  The first integer is START*MULTIPLIER.  Each iteration
** increases the integer by INCREMENT.
**
** This command returns nothing if it works.  It returns an error message
** if something goes wrong.
*/
static int CAPDB_TCLAPI btree_varint_test(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  u32 start, mult, count, incr;
  u64 in, out;
  int n1, n2, i, j;
  unsigned char zBuf[100];
  if( argc!=5 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " START MULTIPLIER COUNT INCREMENT\"", NULL);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[1], (int*)&start) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[2], (int*)&mult) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[3], (int*)&count) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[4], (int*)&incr) ) return TCL_ERROR;
  in = start;
  in *= mult;
  for(i=0; i<(int)count; i++){
    char zErr[200];
    n1 = putVarint(zBuf, in);
    if( n1>9 || n1<1 ){
      capdb_snprintf(sizeof(zErr), zErr,
         "putVarint returned %d - should be between 1 and 9", n1);
      Tcl_AppendResult(interp, zErr, NULL);
      return TCL_ERROR;
    }
    n2 = getVarint(zBuf, &out);
    if( n1!=n2 ){
      capdb_snprintf(sizeof(zErr), zErr,
          "putVarint returned %d and getVarint returned %d", n1, n2);
      Tcl_AppendResult(interp, zErr, NULL);
      return TCL_ERROR;
    }
    if( in!=out ){
      capdb_snprintf(sizeof(zErr), zErr,
          "Wrote 0x%016llx and got back 0x%016llx", in, out);
      Tcl_AppendResult(interp, zErr, NULL);
      return TCL_ERROR;
    }
    if( (in & 0xffffffff)==in ){
      u32 out32;
      n2 = getVarint32(zBuf, out32);
      out = out32;
      if( n1!=n2 ){
        capdb_snprintf(sizeof(zErr), zErr,
          "putVarint returned %d and GetVarint32 returned %d", 
                  n1, n2);
        Tcl_AppendResult(interp, zErr, NULL);
        return TCL_ERROR;
      }
      if( in!=out ){
        capdb_snprintf(sizeof(zErr), zErr,
          "Wrote 0x%016llx and got back 0x%016llx from GetVarint32",
            in, out);
        Tcl_AppendResult(interp, zErr, NULL);
        return TCL_ERROR;
      }
    }

    /* In order to get realistic timings, run getVarint 19 more times.
    ** This is because getVarint is called about 20 times more often
    ** than putVarint.
    */
    for(j=0; j<19; j++){
      getVarint(zBuf, &out);
    }
    in += incr;
  }
  return TCL_OK;
}

/*
** usage:   btree_from_db  DB-HANDLE
**
** This command returns the btree handle for the main database associated
** with the database-handle passed as the argument. Example usage:
**
** capdb db test.db
** set bt [btree_from_db db]
*/
static int CAPDB_TCLAPI btree_from_db(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  char zBuf[100];
  Tcl_CmdInfo info;
  capdb *db;
  Btree *pBt;
  int iDb = 0;

  if( argc!=2 && argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " DB-HANDLE ?N?\"", NULL);
    return TCL_ERROR;
  }

  if( 1!=Tcl_GetCommandInfo(interp, argv[1], &info) ){
    Tcl_AppendResult(interp, "No such db-handle: \"", argv[1], "\"", NULL);
    return TCL_ERROR;
  }
  if( argc==3 ){
    iDb = atoi(argv[2]);
  }

  db = *((capdb **)info.objClientData);
  assert( db );

  pBt = db->aDb[iDb].pBt;
  capdb_snprintf(sizeof(zBuf), zBuf, "%p", pBt);
  Tcl_SetResult(interp, zBuf, TCL_VOLATILE);
  return TCL_OK;
}

/*
** Usage:   btree_ismemdb ID
**
** Return true if the B-Tree is currently stored entirely in memory.
*/
static int CAPDB_TCLAPI btree_ismemdb(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  Btree *pBt;
  int res;
  capdb_file *pFile;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID\"", NULL);
    return TCL_ERROR;
  }
  pBt = capdbTestTextToPtr(argv[1]);
  capdb_mutex_enter(pBt->db->mutex);
  capdbBtreeEnter(pBt);
  pFile = capdbPagerFile(capdbBtreePager(pBt));
  res = (pFile->pMethods==0);
  capdbBtreeLeave(pBt);
  capdb_mutex_leave(pBt->db->mutex);
  Tcl_SetObjResult(interp, Tcl_NewBooleanObj(res));
  return CAPDB_OK;
}

/*
** usage:   btree_set_cache_size ID NCACHE
**
** Set the size of the cache used by btree $ID.
*/
static int CAPDB_TCLAPI btree_set_cache_size(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int nCache;
  Btree *pBt;
  
  if( argc!=3 ){
    Tcl_AppendResult(
        interp, "wrong # args: should be \"", argv[0], " BT NCACHE\"", NULL);
    return TCL_ERROR;
  }
  pBt = capdbTestTextToPtr(argv[1]);
  if( Tcl_GetInt(interp, argv[2], &nCache) ) return TCL_ERROR;

  capdb_mutex_enter(pBt->db->mutex);
  capdbBtreeEnter(pBt);
  capdbBtreeSetCacheSize(pBt, nCache);
  capdbBtreeLeave(pBt);
  capdb_mutex_leave(pBt->db->mutex);
  return TCL_OK;
}      

/*
** usage:   btree_insert CSR ?KEY? VALUE
**
** Set the size of the cache used by btree $ID.
*/
static int CAPDB_TCLAPI btree_insert(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *const objv[]
){
  BtCursor *pCur;
  int rc;
  BtreePayload x;
  Tcl_Size n;

  if( objc!=4 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-intkey? CSR KEY VALUE");
    return TCL_ERROR;
  }

  memset(&x, 0, sizeof(x));
  if( objc==4 ){
    if( Tcl_GetIntFromObj(interp, objv[2], &rc) ) return TCL_ERROR;
    x.nKey = rc;
    x.pData = (void*)Tcl_GetByteArrayFromObj(objv[3], &n);
    x.nData = (int)n;
  }else{
    x.pKey = (void*)Tcl_GetByteArrayFromObj(objv[2], &n);
    x.nKey = (int)n;
  }
  pCur = (BtCursor*)capdbTestTextToPtr(Tcl_GetString(objv[1]));

  capdb_mutex_enter(pCur->pBtree->db->mutex);
  capdbBtreeEnter(pCur->pBtree);
  rc = capdbBtreeInsert(pCur, &x, 0, 0);
  capdbBtreeLeave(pCur->pBtree);
  capdb_mutex_leave(pCur->pBtree->db->mutex);

  Tcl_ResetResult(interp);
  if( rc ){
    Tcl_AppendResult(interp, capdbErrName(rc), NULL);
    return TCL_ERROR;
  }
  return TCL_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest3_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_CmdProc *xProc;
  } aCmd[] = {
     { "btree_open",               (Tcl_CmdProc*)btree_open               },
     { "btree_close",              (Tcl_CmdProc*)btree_close              },
     { "btree_begin_transaction",  (Tcl_CmdProc*)btree_begin_transaction  },
     { "btree_pager_stats",        (Tcl_CmdProc*)btree_pager_stats        },
     { "btree_cursor",             (Tcl_CmdProc*)btree_cursor             },
     { "btree_close_cursor",       (Tcl_CmdProc*)btree_close_cursor       },
     { "btree_next",               (Tcl_CmdProc*)btree_next               },
     { "btree_eof",                (Tcl_CmdProc*)btree_eof                },
     { "btree_payload_size",       (Tcl_CmdProc*)btree_payload_size       },
     { "btree_first",              (Tcl_CmdProc*)btree_first              },
     { "btree_varint_test",        (Tcl_CmdProc*)btree_varint_test        },
     { "btree_from_db",            (Tcl_CmdProc*)btree_from_db            },
     { "btree_ismemdb",            (Tcl_CmdProc*)btree_ismemdb            },
     { "btree_set_cache_size",     (Tcl_CmdProc*)btree_set_cache_size     }
  };
  int i;

  for(i=0; i<sizeof(aCmd)/sizeof(aCmd[0]); i++){
    Tcl_CreateCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
  }

  Tcl_CreateObjCommand(interp, "btree_insert", btree_insert, 0, 0);

  return TCL_OK;
}
