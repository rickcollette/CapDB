/*
** 2014-09-21
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
** This SQLite extension adds a debug "authorizer" callback to the database
** connection.  The callback merely writes the authorization request to
** standard output and returns CAPDB_OK.
**
** This extension can be used (for example) in the command-line shell to
** trace the operation of the authorizer.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <stdio.h>

/*
** Display the authorization request
*/
static int authCallback(
  void *pClientData,
  int op,
  const char *z1,
  const char *z2,
  const char *z3,
  const char *z4
){
  const char *zOp;
  char zOpSpace[50];
  switch( op ){
    case CAPDB_CREATE_INDEX:        zOp = "CREATE_INDEX";        break;
    case CAPDB_CREATE_TABLE:        zOp = "CREATE_TABLE";        break;
    case CAPDB_CREATE_TEMP_INDEX:   zOp = "CREATE_TEMP_INDEX";   break;
    case CAPDB_CREATE_TEMP_TABLE:   zOp = "CREATE_TEMP_TABLE";   break;
    case CAPDB_CREATE_TEMP_TRIGGER: zOp = "CREATE_TEMP_TRIGGER"; break;
    case CAPDB_CREATE_TEMP_VIEW:    zOp = "CREATE_TEMP_VIEW";    break;
    case CAPDB_CREATE_TRIGGER:      zOp = "CREATE_TRIGGER";      break;
    case CAPDB_CREATE_VIEW:         zOp = "CREATE_VIEW";         break;
    case CAPDB_DELETE:              zOp = "DELETE";              break;
    case CAPDB_DROP_INDEX:          zOp = "DROP_INDEX";          break;
    case CAPDB_DROP_TABLE:          zOp = "DROP_TABLE";          break;
    case CAPDB_DROP_TEMP_INDEX:     zOp = "DROP_TEMP_INDEX";     break;
    case CAPDB_DROP_TEMP_TABLE:     zOp = "DROP_TEMP_TABLE";     break;
    case CAPDB_DROP_TEMP_TRIGGER:   zOp = "DROP_TEMP_TRIGGER";   break;
    case CAPDB_DROP_TEMP_VIEW:      zOp = "DROP_TEMP_VIEW";      break;
    case CAPDB_DROP_TRIGGER:        zOp = "DROP_TRIGGER";        break;
    case CAPDB_DROP_VIEW:           zOp = "DROP_VIEW";           break;
    case CAPDB_INSERT:              zOp = "INSERT";              break;
    case CAPDB_PRAGMA:              zOp = "PRAGMA";              break;
    case CAPDB_READ:                zOp = "READ";                break;
    case CAPDB_SELECT:              zOp = "SELECT";              break;
    case CAPDB_TRANSACTION:         zOp = "TRANSACTION";         break;
    case CAPDB_UPDATE:              zOp = "UPDATE";              break;
    case CAPDB_ATTACH:              zOp = "ATTACH";              break;
    case CAPDB_DETACH:              zOp = "DETACH";              break;
    case CAPDB_ALTER_TABLE:         zOp = "ALTER_TABLE";         break;
    case CAPDB_REINDEX:             zOp = "REINDEX";             break;
    case CAPDB_ANALYZE:             zOp = "ANALYZE";             break;
    case CAPDB_CREATE_VTABLE:       zOp = "CREATE_VTABLE";       break;
    case CAPDB_DROP_VTABLE:         zOp = "DROP_VTABLE";         break;
    case CAPDB_FUNCTION:            zOp = "FUNCTION";            break;
    case CAPDB_SAVEPOINT:           zOp = "SAVEPOINT";           break;
    case CAPDB_COPY:                zOp = "COPY";                break;
    case CAPDB_RECURSIVE:           zOp = "RECURSIVE";           break;


    default: {
      capdb_snprintf(sizeof(zOpSpace), zOpSpace, "%d", op);
      zOp = zOpSpace;
      break;
    }
  }
  if( z1==0 ) z1 = "NULL";
  if( z2==0 ) z2 = "NULL";
  if( z3==0 ) z3 = "NULL";
  if( z4==0 ) z4 = "NULL";
  printf("AUTH: %s,%s,%s,%s,%s\n", zOp, z1, z2, z3, z4);
  return CAPDB_OK;
}



#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_showauth_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_set_authorizer(db, authCallback, 0);
  return rc;
}
