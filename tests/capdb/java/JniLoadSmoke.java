package org.capdb.jni.wrapper1;

public final class JniLoadSmoke {
  private JniLoadSmoke() {}

  public static void main(String[] args) throws Exception {
    Class.forName("org.capdb.jni.wrapper1.Capdb");
    System.out.println("capdb_jni load smoke ok");
  }
}
