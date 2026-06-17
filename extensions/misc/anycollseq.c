/*
** 2017-04-16
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
** This file implements a run-time loadable extension to SQLite that
** registers a capdb_collation_needed() callback to register a fake
** collating function for any unknown collating sequence.  The fake
** collating function works like BINARY.
**
** This extension can be used to load schemas that contain one or more
** unknown collating sequences.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <string.h>

static int anyCollFunc(
  void *NotUsed,
  int nKey1, const void *pKey1,
  int nKey2, const void *pKey2
){
  int rc, n;
  n = nKey1<nKey2 ? nKey1 : nKey2;
  rc = memcmp(pKey1, pKey2, n);
  if( rc==0 ) rc = nKey1 - nKey2;
  return rc;
}

static void anyCollNeeded(
  void *NotUsed,
  capdb *db,
  int eTextRep,
  const char *zCollName
){
  capdb_create_collation(db, zCollName, eTextRep, 0, anyCollFunc); 
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_anycollseq_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  rc = capdb_collation_needed(db, 0, anyCollNeeded);
  return rc;
}
