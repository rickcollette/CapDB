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
   A wrapper for communicating C-level (capdb*) instances with
   Java. These wrappers do not own their associated pointer, they
   simply provide a type-safe way to communicate it between Java
   and C via JNI.
*/
public final class capdb extends NativePointerHolder<capdb>
 implements AutoCloseable {

  // Only invoked from JNI
  private capdb(){}

  public String toString(){
    final long ptr = getNativePointer();
    if( 0==ptr ){
      return capdb.class.getSimpleName()+"@null";
    }
    final String fn = CApi.capdb_db_filename(this, "main");
    return capdb.class.getSimpleName()
      +"@"+String.format("0x%08x",ptr)
      +"["+((null == fn) ? "<unnamed>" : fn)+"]"
      ;
  }

  @Override public void close(){
    CApi.capdb_close_v2(this);
  }
}
