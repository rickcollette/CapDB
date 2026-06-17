/*
** 2023-09-03
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
   A wrapper for passing C-level (capdb_blob*) instances around in
   Java. These wrappers do not own their associated pointer, they
   simply provide a type-safe way to communicate it between Java and C
   via JNI.
*/
public final class capdb_blob extends NativePointerHolder<capdb_blob>
  implements AutoCloseable {
  // Only invoked from JNI.
  private capdb_blob(){}

  @Override public void close(){
    CApi.capdb_blob_close(this);
  }
}
