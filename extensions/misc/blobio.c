/*
** 2019-03-30
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** An SQL function that uses the incremental BLOB I/O mechanism of SQLite
** to read or write part of a blob.  This is intended for debugging use
** in the CLI.
**
**      readblob(SCHEMA,TABLE,COLUMN,ROWID,OFFSET,N)
**
** Returns N bytes of the blob starting at OFFSET.
**
**      writeblob(SCHEMA,TABLE,COLUMN,ROWID,OFFSET,NEWDATA)
**
** NEWDATA must be a blob.  The content of NEWDATA overwrites the
** existing BLOB data at SCHEMA.TABLE.COLUMN for row ROWID beginning
** at OFFSET bytes into the blob.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

static void readblobFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_blob *pBlob = 0;
  const char *zSchema;
  const char *zTable;
  const char *zColumn;
  capdb_int64 iRowid;
  int iOfst;
  unsigned char *aData;
  int nData;
  capdb *db;
  int rc;

  zSchema = (const char*)capdb_value_text(argv[0]);
  zTable = (const char*)capdb_value_text(argv[1]);
  if( zTable==0 ){
    capdb_result_error(context, "bad table name", -1);
    return;
  }
  zColumn = (const char*)capdb_value_text(argv[2]);
  if( zTable==0 ){
    capdb_result_error(context, "bad column name", -1);
    return;
  }
  iRowid = capdb_value_int64(argv[3]);
  iOfst = capdb_value_int(argv[4]);
  nData = capdb_value_int(argv[5]);
  if( nData<=0 ) return;
  aData = capdb_malloc64( nData+1 );
  if( aData==0 ){
    capdb_result_error_nomem(context);
    return;
  }
  db = capdb_context_db_handle(context);
  rc = capdb_blob_open(db, zSchema, zTable, zColumn, iRowid, 0, &pBlob);
  if( rc ){
    capdb_free(aData);
    capdb_result_error(context, "cannot open BLOB pointer", -1);
    return;
  }
  rc = capdb_blob_read(pBlob, aData, nData, iOfst);
  capdb_blob_close(pBlob);
  if( rc ){
    capdb_free(aData);
    capdb_result_error(context, "BLOB read failed", -1);
  }else{
    capdb_result_blob(context, aData, nData, capdb_free);
  }
}    

static void writeblobFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  capdb_blob *pBlob = 0;
  const char *zSchema;
  const char *zTable;
  const char *zColumn;
  capdb_int64 iRowid;
  int iOfst;
  unsigned char *aData;
  int nData;
  capdb *db;
  int rc;

  zSchema = (const char*)capdb_value_text(argv[0]);
  zTable = (const char*)capdb_value_text(argv[1]);
  if( zTable==0 ){
    capdb_result_error(context, "bad table name", -1);
    return;
  }
  zColumn = (const char*)capdb_value_text(argv[2]);
  if( zTable==0 ){
    capdb_result_error(context, "bad column name", -1);
    return;
  }
  iRowid = capdb_value_int64(argv[3]);
  iOfst = capdb_value_int(argv[4]);
  if( capdb_value_type(argv[5])!=CAPDB_BLOB ){
    capdb_result_error(context, "6th argument must be a BLOB", -1);
    return;
  }
  nData = capdb_value_bytes(argv[5]);
  aData = (unsigned char *)capdb_value_blob(argv[5]);
  db = capdb_context_db_handle(context);
  rc = capdb_blob_open(db, zSchema, zTable, zColumn, iRowid, 1, &pBlob);
  if( rc ){
    capdb_result_error(context, "cannot open BLOB pointer", -1);
    return;
  }
  rc = capdb_blob_write(pBlob, aData, nData, iOfst);
  capdb_blob_close(pBlob);
  if( rc ){
    capdb_result_error(context, "BLOB write failed", -1);
  }
}    


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_blobio_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "readblob", 6, CAPDB_UTF8, 0,
                               readblobFunc, 0, 0);
  if( rc==CAPDB_OK ){
    rc = capdb_create_function(db, "writeblob", 6, CAPDB_UTF8, 0,
                               writeblobFunc, 0, 0);
  }
  return rc;
}
