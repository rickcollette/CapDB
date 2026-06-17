/*
** 2020-01-11
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
** This SQLite extension implements various SQL functions used to access
** the following SQLite C-language APIs:
**
**         capdb_uri_parameter()
**         capdb_uri_boolean()
**         capdb_uri_int64()
**         capdb_uri_key()
**         capdb_filename_database()
**         capdb_filename_journal()
**         capdb_filename_wal()
**         capdb_db_filename()
**
** These SQL functions are for testing and demonstration purposes only.
**
**
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

/*
** SQL function:    capdb_db_filename(SCHEMA) 
**
** Return the filename corresponding to SCHEMA.
*/
static void func_db_filename(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zFile = capdb_db_filename(db, zSchema);
  capdb_result_text(context, zFile, -1, CAPDB_TRANSIENT);
}

/*
** SQL function:    capdb_uri_parameter(SCHEMA,NAME) 
**
** Return the value of the NAME query parameter to the database for SCHEMA
*/
static void func_uri_parameter(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zName = (const char*)capdb_value_text(argv[1]);
  const char *zFile = capdb_db_filename(db, zSchema);
  const char *zRes = capdb_uri_parameter(zFile, zName);
  capdb_result_text(context, zRes, -1, CAPDB_TRANSIENT);
}

/*
** SQL function:    capdb_uri_boolean(SCHEMA,NAME,DEFAULT) 
**
** Return the boolean value of the NAME query parameter to
** the database for SCHEMA
*/
static void func_uri_boolean(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zName = (const char*)capdb_value_text(argv[1]);
  const char *zFile = capdb_db_filename(db, zSchema);
  int iDflt = capdb_value_int(argv[2]);
  int iRes = capdb_uri_boolean(zFile, zName, iDflt);
  capdb_result_int(context, iRes);
}

/*
** SQL function:    capdb_uri_key(SCHEMA,N)
**
** Return the name of the Nth query parameter
*/
static void func_uri_key(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  int N = capdb_value_int(argv[1]);
  const char *zFile = capdb_db_filename(db, zSchema);
  const char *zRes = capdb_uri_key(zFile, N);
  capdb_result_text(context, zRes, -1, CAPDB_TRANSIENT);
}

/*
** SQL function:    capdb_uri_int64(SCHEMA,NAME,DEFAULT) 
**
** Return the int64 value of the NAME query parameter to
** the database for SCHEMA
*/
static void func_uri_int64(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zName = (const char*)capdb_value_text(argv[1]);
  const char *zFile = capdb_db_filename(db, zSchema);
  capdb_int64 iDflt = capdb_value_int64(argv[2]);
  capdb_int64 iRes = capdb_uri_int64(zFile, zName, iDflt);
  capdb_result_int64(context, iRes);
}

/*
** SQL function:    capdb_filename_database(SCHEMA)
**
** Return the database filename for SCHEMA
*/
static void func_filename_database(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zFile = capdb_db_filename(db, zSchema);
  const char *zRes = zFile ? capdb_filename_database(zFile) : 0;
  capdb_result_text(context, zRes, -1, CAPDB_TRANSIENT);
}

/*
** SQL function:    capdb_filename_journal(SCHEMA)
**
** Return the rollback journal filename for SCHEMA
*/
static void func_filename_journal(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zFile = capdb_db_filename(db, zSchema);
  const char *zRes = zFile ? capdb_filename_journal(zFile) : 0;
  capdb_result_text(context, zRes, -1, CAPDB_TRANSIENT);
}

/*
** SQL function:    capdb_filename_wal(SCHEMA)
**
** Return the WAL filename for SCHEMA
*/
static void func_filename_wal(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  const char *zSchema = (const char*)capdb_value_text(argv[0]);
  capdb *db = capdb_context_db_handle(context);
  const char *zFile = capdb_db_filename(db, zSchema);
  const char *zRes = zFile ? capdb_filename_wal(zFile) : 0;
  capdb_result_text(context, zRes, -1, CAPDB_TRANSIENT);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_urifuncs_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  static const struct {
    const char *zFuncName;
    int nArg;
    void (*xFunc)(capdb_context*,int,capdb_value**);
  } aFunc[] = {
    { "capdb_db_filename",       1, func_db_filename       },
    { "capdb_uri_parameter",     2, func_uri_parameter     },
    { "capdb_uri_boolean",       3, func_uri_boolean       },
    { "capdb_uri_int64",         3, func_uri_int64         },
    { "capdb_uri_key",           2, func_uri_key           },
    { "capdb_filename_database", 1, func_filename_database },
    { "capdb_filename_journal",  1, func_filename_journal  },
    { "capdb_filename_wal",      1, func_filename_wal      },
  };
  int rc = CAPDB_OK;
  int i;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  for(i=0; rc==CAPDB_OK && i<sizeof(aFunc)/sizeof(aFunc[0]); i++){
    rc = capdb_create_function(db, aFunc[i].zFuncName, aFunc[i].nArg,
                     CAPDB_UTF8, 0,
                     aFunc[i].xFunc, 0, 0);
  }
  return rc;
}
