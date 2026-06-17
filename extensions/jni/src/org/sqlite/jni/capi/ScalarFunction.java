/*
** 2023-08-25
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file is part of the JNI bindings for the capdb C API.
*/
package org.sqlite.jni.capi;


/**
   A SQLFunction implementation for scalar functions.
*/
public abstract class ScalarFunction implements SQLFunction {
  /**
     As for the xFunc() argument of the C API's
     capdb_create_function(). If this function throws, it is
     translated into an capdb_result_error().
  */
  public abstract void xFunc(capdb_context cx, capdb_value[] args);

  /**
     Optionally override to be notified when the UDF is finalized by
     SQLite. This default implementation does nothing.
  */
  public void xDestroy() {}
}
