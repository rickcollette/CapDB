/*
** 2023-07-21
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
   A wrapper for communicating C-level (capdb_stmt*) instances with
   Java. These wrappers do not own their associated pointer, they
   simply provide a type-safe way to communicate it between Java and C
   via JNI.
*/
public final class capdb_stmt extends NativePointerHolder<capdb_stmt>
  implements AutoCloseable {
  // Only invoked from JNI.
  private capdb_stmt(){}

  @Override public void close(){
    CApi.capdb_finalize(this);
  }
}
