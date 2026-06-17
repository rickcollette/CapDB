/*
** 2022-08-27
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
** This file contains the public interface to the "recover" extension -
** an SQLite extension designed to recover data from corrupted database
** files.
*/

/*
** OVERVIEW:
**
** To use the API to recover data from a corrupted database, an
** application:
**
**   1) Creates an capdb_recover handle by calling either
**      capdb_recover_init() or capdb_recover_init_sql().
**
**   2) Configures the new handle using one or more calls to
**      capdb_recover_config().
**
**   3) Executes the recovery by repeatedly calling capdb_recover_step() on
**      the handle until it returns something other than CAPDB_OK. If it
**      returns CAPDB_DONE, then the recovery operation completed without 
**      error. If it returns some other non-CAPDB_OK value, then an error 
**      has occurred.
**
**   4) Retrieves any error code and English language error message using the
**      capdb_recover_errcode() and capdb_recover_errmsg() APIs,
**      respectively.
**
**   5) Destroys the capdb_recover handle and frees all resources
**      using capdb_recover_finish().
**
** The application may abandon the recovery operation at any point 
** before it is finished by passing the capdb_recover handle to
** capdb_recover_finish(). This is not an error, but the final state
** of the output database, or the results of running the partial script
** delivered to the SQL callback, are undefined.
*/

#ifndef _CAPDB_RECOVER_H
#define _CAPDB_RECOVER_H

#include "capdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
** An instance of the capdb_recover object represents a recovery
** operation in progress.
**
** Constructors:
**
**    capdb_recover_init()
**    capdb_recover_init_sql()
**
** Destructor:
**
**    capdb_recover_finish()
**
** Methods:
**
**    capdb_recover_config()
**    capdb_recover_errcode()
**    capdb_recover_errmsg()
**    capdb_recover_run()
**    capdb_recover_step()
*/
typedef struct capdb_recover capdb_recover;

/* 
** These two APIs attempt to create and return a new capdb_recover object.
** In both cases the first two arguments identify the (possibly
** corrupt) database to recover data from. The first argument is an open
** database handle and the second the name of a database attached to that
** handle (i.e. "main", "temp" or the name of an attached database).
**
** If capdb_recover_init() is used to create the new capdb_recover
** handle, then data is recovered into a new database, identified by
** string parameter zUri. zUri may be an absolute or relative file path,
** or may be an SQLite URI. If the identified database file already exists,
** it is overwritten.
**
** If capdb_recover_init_sql() is invoked, then any recovered data will
** be returned to the user as a series of SQL statements. Executing these
** SQL statements results in the same database as would have been created
** had capdb_recover_init() been used. For each SQL statement in the
** output, the callback function passed as the third argument (xSql) is 
** invoked once. The first parameter is a passed a copy of the fourth argument
** to this function (pCtx) as its first parameter, and a pointer to a
** nul-terminated buffer containing the SQL statement formated as UTF-8 as 
** the second. If the xSql callback returns any value other than CAPDB_OK,
** then processing is immediately abandoned and the value returned used as
** the recover handle error code (see below).
**
** If an out-of-memory error occurs, NULL may be returned instead of
** a valid handle. In all other cases, it is the responsibility of the
** application to avoid resource leaks by ensuring that
** capdb_recover_finish() is called on all allocated handles.
*/
capdb_recover *capdb_recover_init(
  capdb* db, 
  const char *zDb, 
  const char *zUri
);
capdb_recover *capdb_recover_init_sql(
  capdb* db, 
  const char *zDb, 
  int (*xSql)(void*, const char*),
  void *pCtx
);

/*
** Configure an capdb_recover object that has just been created using
** capdb_recover_init() or capdb_recover_init_sql(). This function
** may only be called before the first call to capdb_recover_step()
** or capdb_recover_run() on the object.
**
** The second argument passed to this function must be one of the
** CAPDB_RECOVER_* symbols defined below. Valid values for the third argument
** depend on the specific CAPDB_RECOVER_* symbol in use.
**
** CAPDB_OK is returned if the configuration operation was successful,
** or an SQLite error code otherwise.
*/
int capdb_recover_config(capdb_recover*, int op, void *pArg);

/*
** CAPDB_RECOVER_LOST_AND_FOUND:
**   The pArg argument points to a string buffer containing the name
**   of a "lost-and-found" table in the output database, or NULL. If
**   the argument is non-NULL and the database contains seemingly
**   valid pages that cannot be associated with any table in the
**   recovered part of the schema, data is extracted from these
**   pages to add to the lost-and-found table.
**
** CAPDB_RECOVER_FREELIST_CORRUPT:
**   The pArg value must actually be a pointer to a value of type
**   int containing value 0 or 1 cast as a (void*). If this option is set
**   (argument is 1) and a lost-and-found table has been configured using
**   CAPDB_RECOVER_LOST_AND_FOUND, then is assumed that the freelist is 
**   corrupt and an attempt is made to recover records from pages that
**   appear to be linked into the freelist. Otherwise, pages on the freelist
**   are ignored. Setting this option can recover more data from the
**   database, but often ends up "recovering" deleted records. The default 
**   value is 0 (clear).
**
** CAPDB_RECOVER_ROWIDS:
**   The pArg value must actually be a pointer to a value of type
**   int containing value 0 or 1 cast as a (void*). If this option is set
**   (argument is 1), then an attempt is made to recover rowid values
**   that are not also INTEGER PRIMARY KEY values. If this option is
**   clear, then new rowids are assigned to all recovered rows. The
**   default value is 1 (set).
**
** CAPDB_RECOVER_SLOWINDEXES:
**   The pArg value must actually be a pointer to a value of type
**   int containing value 0 or 1 cast as a (void*). If this option is clear
**   (argument is 0), then when creating an output database, the recover 
**   module creates and populates non-UNIQUE indexes right at the end of the
**   recovery operation - after all recoverable data has been inserted
**   into the new database. This is faster overall, but means that the
**   final call to capdb_recover_step() for a recovery operation may
**   be need to create a large number of indexes, which may be very slow.
**
**   Or, if this option is set (argument is 1), then non-UNIQUE indexes
**   are created in the output database before it is populated with 
**   recovered data. This is slower overall, but avoids the slow call
**   to capdb_recover_step() at the end of the recovery operation.
**
**   The default option value is 0.
*/
#define CAPDB_RECOVER_LOST_AND_FOUND   1
#define CAPDB_RECOVER_FREELIST_CORRUPT 2
#define CAPDB_RECOVER_ROWIDS           3
#define CAPDB_RECOVER_SLOWINDEXES      4

/*
** Perform a unit of work towards the recovery operation. This function 
** must normally be called multiple times to complete database recovery.
**
** If no error occurs but the recovery operation is not completed, this
** function returns CAPDB_OK. If recovery has been completed successfully
** then CAPDB_DONE is returned. If an error has occurred, then an SQLite
** error code (e.g. CAPDB_IOERR or CAPDB_NOMEM) is returned. It is not
** considered an error if some or all of the data cannot be recovered
** due to database corruption.
**
** Once capdb_recover_step() has returned a value other than CAPDB_OK,
** all further such calls on the same recover handle are no-ops that return
** the same non-CAPDB_OK value.
*/
int capdb_recover_step(capdb_recover*);

/* 
** Run the recovery operation to completion. Return CAPDB_OK if successful,
** or an SQLite error code otherwise. Calling this function is the same
** as executing:
**
**     while( CAPDB_OK==capdb_recover_step(p) );
**     return capdb_recover_errcode(p);
*/
int capdb_recover_run(capdb_recover*);

/*
** If an error has been encountered during a prior call to
** capdb_recover_step(), then this function attempts to return a 
** pointer to a buffer containing an English language explanation of 
** the error. If no error message is available, or if an out-of memory 
** error occurs while attempting to allocate a buffer in which to format
** the error message, NULL is returned.
**
** The returned buffer remains valid until the capdb_recover handle is
** destroyed using capdb_recover_finish().
*/
const char *capdb_recover_errmsg(capdb_recover*);

/*
** If this function is called on an capdb_recover handle after
** an error occurs, an SQLite error code is returned. Otherwise, CAPDB_OK.
*/
int capdb_recover_errcode(capdb_recover*);

/* 
** Clean up a recovery object created by a call to capdb_recover_init().
** The results of using a recovery object with any API after it has been
** passed to this function are undefined.
**
** This function returns the same value as capdb_recover_errcode().
*/
int capdb_recover_finish(capdb_recover*);


#ifdef __cplusplus
}  /* end of the 'extern "C"' block */
#endif

#endif /* ifndef _CAPDB_RECOVER_H */
