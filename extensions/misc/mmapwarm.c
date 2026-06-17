/*
** 2017-09-18
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
*/

#include "capdb.h"


/*
** This function is used to touch each page of a mapping of a memory
** mapped SQLite database. Assuming that the system has sufficient free
** memory and supports sufficiently large mappings, this causes the OS 
** to cache the entire database in main memory, making subsequent 
** database accesses faster.
**
** If the second parameter to this function is not NULL, it is the name of
** the specific database to operate on (i.e. "main" or the name of an
** attached database).
**
** CAPDB_OK is returned if successful, or an SQLite error code otherwise.
** It is not considered an error if the file is not memory-mapped, or if
** the mapping does not span the entire file. If an error does occur, a
** transaction may be left open on the database file.
**
** It is illegal to call this function when the database handle has an 
** open transaction. CAPDB_MISUSE is returned in this case.
*/
int capdb_mmap_warm(capdb *db, const char *zDb){
  int rc = CAPDB_OK;
  char *zSql = 0;
  int pgsz = 0;
  unsigned int nTotal = 0;

  if( 0==capdb_get_autocommit(db) ) return CAPDB_MISUSE;

  /* Open a read-only transaction on the file in question */
  zSql = capdb_mprintf("BEGIN; SELECT * FROM %s%q%ssqlite_schema", 
      (zDb ? "'" : ""), (zDb ? zDb : ""), (zDb ? "'." : "")
  );
  if( zSql==0 ) return CAPDB_NOMEM;
  rc = capdb_exec(db, zSql, 0, 0, 0);
  capdb_free(zSql);

  /* Find the SQLite page size of the file */
  if( rc==CAPDB_OK ){
    zSql = capdb_mprintf("PRAGMA %s%q%spage_size", 
        (zDb ? "'" : ""), (zDb ? zDb : ""), (zDb ? "'." : "")
    );
    if( zSql==0 ){
      rc = CAPDB_NOMEM;
    }else{
      capdb_stmt *pPgsz = 0;
      rc = capdb_prepare_v2(db, zSql, -1, &pPgsz, 0);
      capdb_free(zSql);
      if( rc==CAPDB_OK ){
        if( capdb_step(pPgsz)==CAPDB_ROW ){
          pgsz = capdb_column_int(pPgsz, 0);
        }
        rc = capdb_finalize(pPgsz);
      }
      if( rc==CAPDB_OK && pgsz==0 ){
        rc = CAPDB_ERROR;
      }
    }
  }

  /* Touch each mmap'd page of the file */
  if( rc==CAPDB_OK ){
    int rc2;
    capdb_file *pFd = 0;
    rc = capdb_file_control(db, zDb, CAPDB_FCNTL_FILE_POINTER, &pFd);
    if( rc==CAPDB_OK && pFd->pMethods->iVersion>=3 ){
      capdb_int64 iPg = 1;
      capdb_io_methods const *p = pFd->pMethods;
      while( 1 ){
        unsigned char *pMap;
        rc = p->xFetch(pFd, pgsz*iPg, pgsz, (void**)&pMap);
        if( rc!=CAPDB_OK || pMap==0 ) break;

        nTotal += (unsigned int)pMap[0];
        nTotal += (unsigned int)pMap[pgsz-1];

        rc = p->xUnfetch(pFd, pgsz*iPg, (void*)pMap);
        if( rc!=CAPDB_OK ) break;
        iPg++;
      }
      capdb_log(CAPDB_OK, 
          "capdb_mmap_warm_cache: Warmed up %d pages of %s", iPg==1?0:iPg,
          capdb_db_filename(db, zDb)
      );
    }

    rc2 = capdb_exec(db, "END", 0, 0, 0);
    if( rc==CAPDB_OK ) rc = rc2;
  }

  (void)nTotal;
  return rc;
}
