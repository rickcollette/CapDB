/*
** 2023-10-09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file is part of the wrapper1 interface for capdb.
*/
package org.sqlite.jni.wrapper1;
import org.sqlite.jni.capi.CApi;
import org.sqlite.jni.capi.capdb;

/**
   A wrapper for communicating C-level (capdb*) instances with
   Java. These wrappers do not own their associated pointer, they
   simply provide a type-safe way to communicate it between Java
   and C via JNI.
*/
public final class SqliteException extends java.lang.RuntimeException {
  private int errCode = CApi.CAPDB_ERROR;
  private int xerrCode = CApi.CAPDB_ERROR;
  private int errOffset = -1;
  private int sysErrno = 0;

  /**
     Records the given error string and uses CAPDB_ERROR for both the
     error code and extended error code.
  */
  public SqliteException(String msg){
    super(msg);
  }

  /**
     Uses capdb_errstr(capdbResultCode) for the error string and
     sets both the error code and extended error code to the given
     value. This approach includes no database-level information and
     systemErrno() will be 0, so is intended only for use with capdb
     APIs for which a result code is not an error but which the
     higher-level wrapper should treat as one.
  */
  public SqliteException(int capdbResultCode){
    super(CApi.capdb_errstr(capdbResultCode));
    errCode = xerrCode = capdbResultCode;
  }

  /**
     Records the current error state of db (which must not be null and
     must refer to an opened db object). Note that this does not close
     the db.

     Design note: closing the db on error is really only useful during
     a failed db-open operation, and the place(s) where that can
     happen are inside this library, not client-level code.
  */
  SqliteException(capdb db){
    super(CApi.capdb_errmsg(db));
    errCode = CApi.capdb_errcode(db);
    xerrCode = CApi.capdb_extended_errcode(db);
    errOffset = CApi.capdb_error_offset(db);
    sysErrno = CApi.capdb_system_errno(db);
  }

  /**
     Records the current error state of db (which must not be null and
     must refer to an open database).
  */
  public SqliteException(Sqlite db){
    this(db.nativeHandle());
  }

  public SqliteException(Sqlite.Stmt stmt){
    this(stmt.getDb());
  }

  public int errcode(){ return errCode; }
  public int extendedErrcode(){ return xerrCode; }
  public int errorOffset(){ return errOffset; }
  public int systemErrno(){ return sysErrno; }

}
