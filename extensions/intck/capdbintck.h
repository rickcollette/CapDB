/*
** 2024-02-08
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/

/*
** Incremental Integrity-Check Extension
** -------------------------------------
**
** This module contains code to check whether or not an SQLite database
** is well-formed or corrupt. This is the same task as performed by SQLite's
** built-in "PRAGMA integrity_check" command. This module differs from
** "PRAGMA integrity_check" in that:
**
**   +  It is less thorough - this module does not detect certain types
**      of corruption that are detected by the PRAGMA command. However,
**      it does detect all kinds of corruption that are likely to cause
**      errors in SQLite applications.
**
**   +  It is slower. Sometimes up to three times slower.
**
**   +  It allows integrity-check operations to be split into multiple
**      transactions, so that the database does not need to be read-locked
**      for the duration of the integrity-check.
**
** One way to use the API to run integrity-check on the "main" database
** of handle db is:
**
**   int rc = CAPDB_OK;
**   capdb_intck *p = 0;
**
**   capdb_intck_open(db, "main", &p);
**   while( CAPDB_OK==capdb_intck_step(p) ){
**     const char *zMsg = capdb_intck_message(p);
**     if( zMsg ) printf("corruption: %s\n", zMsg);
**   }
**   rc = capdb_intck_error(p, &zErr);
**   if( rc!=CAPDB_OK ){
**     printf("error occured (rc=%d), (errmsg=%s)\n", rc, zErr);
**   }
**   capdb_intck_close(p);
**
** Usually, the capdb_intck object opens a read transaction within the
** first call to capdb_intck_step() and holds it open until the 
** integrity-check is complete. However, if capdb_intck_unlock() is
** called, the read transaction is ended and a new read transaction opened
** by the subsequent call to capdb_intck_step().
*/

#ifndef _CAPDB_INTCK_H
#define _CAPDB_INTCK_H

#include "capdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
** An ongoing incremental integrity-check operation is represented by an
** opaque pointer of the following type.
*/
typedef struct capdb_intck capdb_intck;

/*
** Open a new incremental integrity-check object. If successful, populate
** output variable (*ppOut) with the new object handle and return CAPDB_OK.
** Or, if an error occurs, set (*ppOut) to NULL and return an SQLite error
** code (e.g. CAPDB_NOMEM).
**
** The integrity-check will be conducted on database zDb (which must be "main",
** "temp", or the name of an attached database) of database handle db. Once
** this function has been called successfully, the caller should not use 
** database handle db until the integrity-check object has been destroyed
** using capdb_intck_close().
*/
int capdb_intck_open(
  capdb *db,                    /* Database handle */
  const char *zDb,                /* Database name ("main", "temp" etc.) */
  capdb_intck **ppOut           /* OUT: New capdb_intck handle */
);

/*
** Close and release all resources associated with a handle opened by an
** earlier call to capdb_intck_open(). The results of using an
** integrity-check handle after it has been passed to this function are
** undefined.
*/
void capdb_intck_close(capdb_intck *pCk);

/*
** Do the next step of the integrity-check operation specified by the handle
** passed as the only argument. This function returns CAPDB_DONE if the 
** integrity-check operation is finished, or an SQLite error code if
** an error occurs, or CAPDB_OK if no error occurs but the integrity-check
** is not finished. It is not considered an error if database corruption
** is encountered.
**
** Following a successful call to capdb_intck_step() (one that returns
** CAPDB_OK), capdb_intck_message() returns a non-NULL value if 
** corruption was detected in the db.
**
** If an error occurs and a value other than CAPDB_OK or CAPDB_DONE is
** returned, then the integrity-check handle is placed in an error state.
** In this state all subsequent calls to capdb_intck_step() or 
** capdb_intck_unlock() will immediately return the same error. The 
** capdb_intck_error() method may be used to obtain an English language 
** error message in this case.
*/
int capdb_intck_step(capdb_intck *pCk);

/*
** If the previous call to capdb_intck_step() encountered corruption 
** within the database, then this function returns a pointer to a buffer
** containing a nul-terminated string describing the corruption in 
** English. If the previous call to capdb_intck_step() did not encounter
** corruption, or if there was no previous call, this function returns 
** NULL.
*/
const char *capdb_intck_message(capdb_intck *pCk);

/*
** Close any read-transaction opened by an earlier call to 
** capdb_intck_step(). Any subsequent call to capdb_intck_step() will
** open a new transaction. Return CAPDB_OK if successful, or an SQLite error
** code otherwise.
**
** If an error occurs, then the integrity-check handle is placed in an error
** state. In this state all subsequent calls to capdb_intck_step() or 
** capdb_intck_unlock() will immediately return the same error. The 
** capdb_intck_error() method may be used to obtain an English language 
** error message in this case.
*/
int capdb_intck_unlock(capdb_intck *pCk);

/*
** If an error has occurred in an earlier call to capdb_intck_step()
** or capdb_intck_unlock(), then this method returns the associated 
** SQLite error code. Additionally, if pzErr is not NULL, then (*pzErr)
** may be set to point to a nul-terminated string containing an English
** language error message. Or, if no error message is available, to
** NULL.
**
** If no error has occurred within capdb_intck_step() or
** sqlite_intck_unlock() calls on the handle passed as the first argument, 
** then CAPDB_OK is returned and (*pzErr) set to NULL.
*/
int capdb_intck_error(capdb_intck *pCk, const char **pzErr);

/*
** This API is used for testing only. It returns the full-text of an SQL
** statement used to test object zObj, which may be a table or index.
** The returned buffer is valid until the next call to either this function
** or capdb_intck_close() on the same capdb_intck handle.
*/
const char *capdb_intck_test_sql(capdb_intck *pCk, const char *zObj);


#ifdef __cplusplus
}  /* end of the 'extern "C"' block */
#endif

#endif /* ifndef _CAPDB_INTCK_H */
