package org.capdb.jni.wrapper1;

import java.sql.SQLException;

public final class Capdb implements AutoCloseable {
  private long nativeHandle;

  static {
    System.loadLibrary("capdb_jni");
  }

  private Capdb(long nativeHandle) {
    this.nativeHandle = nativeHandle;
  }

  public static Capdb connectRemote(String uri) throws SQLException {
    long handle = connectRemoteNative(uri);
    if (handle == 0) {
      throw new SQLException("capdb remote connect returned null handle");
    }
    return new Capdb(handle);
  }

  public int exec(String sql) throws SQLException {
    if (nativeHandle == 0) {
      throw new SQLException("capdb connection is closed");
    }
    return execRemoteNative(nativeHandle, sql);
  }

  @Override
  public void close() {
    if (nativeHandle != 0) {
      closeRemoteNative(nativeHandle);
      nativeHandle = 0;
    }
  }

  private static native long connectRemoteNative(String uri) throws SQLException;
  private static native void closeRemoteNative(long nativeHandle);
  private static native int execRemoteNative(long nativeHandle, String sql)
      throws SQLException;
}
