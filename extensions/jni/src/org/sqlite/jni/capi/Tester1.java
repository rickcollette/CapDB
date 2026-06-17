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
** This file contains a set of tests for the capdb JNI bindings.
*/
package org.sqlite.jni.capi;
import static org.sqlite.jni.capi.CApi.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
   An annotation for Tester1 tests which we do not want to run in
   reflection-driven test mode because either they are not suitable
   for multi-threaded threaded mode or we have to control their execution
   order.
*/
@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
@java.lang.annotation.Target({java.lang.annotation.ElementType.METHOD})
@interface ManualTest{}
/**
   Annotation for Tester1 tests which mark those which must be skipped
   in multi-threaded mode.
*/
@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
@java.lang.annotation.Target({java.lang.annotation.ElementType.METHOD})
@interface SingleThreadOnly{}

/**
   Annotation for Tester1 tests which must only be run if
   capdb_jni_supports_nio() is true.
*/
@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
@java.lang.annotation.Target({java.lang.annotation.ElementType.METHOD})
@interface RequiresJniNio{}

public class Tester1 implements Runnable {
  //! True when running in multi-threaded mode.
  private static boolean mtMode = false;
  //! True to sleep briefly between tests.
  private static boolean takeNaps = false;
  //! True to shuffle the order of the tests.
  private static boolean shuffle = false;
  //! True to dump the list of to-run tests to stdout.
  private static int listRunTests = 0;
  //! True to squelch all out() and outln() output.
  private static boolean quietMode = false;
  //! Total number of runTests() calls.
  private static int nTestRuns = 0;
  //! List of test*() methods to run.
  private static List<java.lang.reflect.Method> testMethods = null;
  //! List of exceptions collected by run()
  private static final List<Exception> listErrors = new ArrayList<>();
  private static final class Metrics {
    //! Number of times createNewDb() (or equivalent) is invoked.
    volatile int dbOpen = 0;
  }

  private final Integer tId;

  Tester1(Integer id){
    tId = id;
  }

  static final Metrics metrics = new Metrics();

  public static synchronized void outln(){
    if( !quietMode ){
      System.out.println();
    }
  }

  public static synchronized void outPrefix(){
    if( !quietMode ){
      System.out.print(Thread.currentThread().getName()+": ");
    }
  }

  public static synchronized void outln(Object val){
    if( !quietMode ){
      outPrefix();
      System.out.println(val);
    }
  }

  public static synchronized void out(Object val){
    if( !quietMode ){
      System.out.print(val);
    }
  }

  @SuppressWarnings("unchecked")
  public static synchronized void out(Object... vals){
    if( !quietMode ){
      outPrefix();
      for(Object v : vals) out(v);
    }
  }

  @SuppressWarnings("unchecked")
  public static synchronized void outln(Object... vals){
    if( !quietMode ){
      out(vals); out("\n");
    }
  }

  static volatile int affirmCount = 0;
  public static synchronized int affirm(Boolean v, String comment){
    ++affirmCount;
    if( false ) assert( v /* prefer assert over exception if it's enabled because
                 the JNI layer sometimes has to suppress exceptions,
                 so they might be squelched on their way back to the
                 top. */);
    if( !v ) throw new RuntimeException(comment);
    return affirmCount;
  }

  public static void affirm(Boolean v){
    affirm(v, "Affirmation failed.");
  }

  @SingleThreadOnly /* because it's thread-agnostic */
  private void test1(){
    affirm(capdb_libversion_number() == CAPDB_VERSION_NUMBER);
  }

  public static capdb createNewDb(){
    final OutputPointer.capdb out = new OutputPointer.capdb();
    int rc = capdb_open(":memory:", out);
    ++metrics.dbOpen;
    capdb db = out.take();
    if( 0!=rc ){
      final String msg =
        null==db ? capdb_errstr(rc) : capdb_errmsg(db);
      capdb_close(db);
      throw new RuntimeException("Opening db failed: "+msg);
    }
    affirm( null == out.get() );
    affirm( 0 != db.getNativePointer() );
    rc = capdb_busy_timeout(db, 2000);
    affirm( 0 == rc );
    return db;
  }

  public static void execSql(capdb db, String[] sql){
    execSql(db, String.join("", sql));
  }

  public static int execSql(capdb db, boolean throwOnError, String sql){
    OutputPointer.Int32 oTail = new OutputPointer.Int32();
    final byte[] sqlUtf8 = sql.getBytes(StandardCharsets.UTF_8);
    int pos = 0, n = 1;
    byte[] sqlChunk = sqlUtf8;
    int rc = 0;
    capdb_stmt stmt = null;
    final OutputPointer.capdb_stmt outStmt = new OutputPointer.capdb_stmt();
    while(pos < sqlChunk.length){
      if(pos > 0){
        sqlChunk = Arrays.copyOfRange(sqlChunk, pos,
                                      sqlChunk.length);
      }
      if( 0==sqlChunk.length ) break;
      rc = capdb_prepare_v2(db, sqlChunk, outStmt, oTail);
      if(throwOnError) affirm(0 == rc);
      else if( 0!=rc ) break;
      pos = oTail.value;
      stmt = outStmt.take();
      if( null == stmt ){
        // empty statement was parsed.
        continue;
      }
      affirm(0 != stmt.getNativePointer());
      while( CAPDB_ROW == (rc = capdb_step(stmt)) ){
      }
      capdb_finalize(stmt);
      affirm(0 == stmt.getNativePointer());
      if(0!=rc && CAPDB_ROW!=rc && CAPDB_DONE!=rc){
        break;
      }
    }
    capdb_finalize(stmt);
    if(CAPDB_ROW==rc || CAPDB_DONE==rc) rc = 0;
    if( 0!=rc && throwOnError){
      throw new RuntimeException("db op failed with rc="
                                 +rc+": "+capdb_errmsg(db));
    }
    return rc;
  }

  public static void execSql(capdb db, String sql){
    execSql(db, true, sql);
  }

  public static capdb_stmt prepare(capdb db, boolean throwOnError, String sql){
    final OutputPointer.capdb_stmt outStmt = new OutputPointer.capdb_stmt();
    int rc = capdb_prepare_v2(db, sql, outStmt);
    if( throwOnError ){
      affirm( 0 == rc );
    }
    final capdb_stmt rv = outStmt.take();
    affirm( null == outStmt.get() );
    if( throwOnError ){
      affirm( 0 != rv.getNativePointer() );
    }
    return rv;
  }

  public static capdb_stmt prepare(capdb db, String sql){
    return prepare(db, true, sql);
  }

  private void showCompileOption(){
    int i = 0;
    String optName;
    outln("compile options:");
    for( ; null != (optName = capdb_compileoption_get(i)); ++i){
      outln("\t"+optName+"\t (used="+
            capdb_compileoption_used(optName)+")");
    }
  }

  private void testCompileOption(){
    int i = 0;
    String optName;
    for( ; null != (optName = capdb_compileoption_get(i)); ++i){
    }
    affirm( i > 10 );
    affirm( null==capdb_compileoption_get(-1) );
  }

  private void testOpenDb1(){
    final OutputPointer.capdb out = new OutputPointer.capdb();
    int rc = capdb_open(":memory:", out);
    ++metrics.dbOpen;
    capdb db = out.get();
    affirm(0 == rc);
    affirm(db.getNativePointer()!=0);
    capdb_db_config(db, CAPDB_DBCONFIG_DEFENSIVE, 1, null)
      /* This function has different mangled names in jdk8 vs jdk19,
         and this call is here to ensure that the build fails
         if it cannot find both names. */;

    affirm( 0==capdb_db_readonly(db,"main") );
    affirm( 0==capdb_db_readonly(db,null) );
    affirm( 0>capdb_db_readonly(db,"nope") );
    affirm( 0>capdb_db_readonly(null,null) );
    affirm( 0==capdb_last_insert_rowid(null) );

    // These interrupt checks are only to make sure that the JNI binding
    // has the proper exported symbol names. They don't actually test
    // anything useful.
    affirm( !capdb_is_interrupted(db) );
    capdb_interrupt(db);
    affirm( capdb_is_interrupted(db) );
    capdb_close_v2(db);
    affirm(0 == db.getNativePointer());
  }

  private void testOpenDb2(){
    final OutputPointer.capdb out = new OutputPointer.capdb();
    int rc = capdb_open_v2(":memory:", out,
                             CAPDB_OPEN_READWRITE
                             | CAPDB_OPEN_CREATE, null);
    ++metrics.dbOpen;
    affirm(0 == rc);
    capdb db = out.get();
    affirm(0 != db.getNativePointer());
    capdb_close_v2(db);
    affirm(0 == db.getNativePointer());
  }

  private void testPrepare123(){
    capdb db = createNewDb();
    int rc;
    final OutputPointer.capdb_stmt outStmt = new OutputPointer.capdb_stmt();
    rc = capdb_prepare(db, "CREATE TABLE t1(a);", outStmt);
    affirm(0 == rc);
    capdb_stmt stmt = outStmt.take();
    affirm(0 != stmt.getNativePointer());
    affirm( !capdb_stmt_readonly(stmt) );
    affirm( db == capdb_db_handle(stmt) );
    rc = capdb_step(stmt);
    affirm(CAPDB_DONE == rc);
    capdb_finalize(stmt);
    affirm( null == capdb_db_handle(stmt) );
    affirm(0 == stmt.getNativePointer());

    { /* Demonstrate how to use the "zTail" option of
         capdb_prepare() family of functions. */
      OutputPointer.Int32 oTail = new OutputPointer.Int32();
      final byte[] sqlUtf8 =
        "CREATE TABLE t2(a); INSERT INTO t2(a) VALUES(1),(2),(3)"
        .getBytes(StandardCharsets.UTF_8);
      int pos = 0, n = 1;
      byte[] sqlChunk = sqlUtf8;
      while(pos < sqlChunk.length){
        if(pos > 0){
          sqlChunk = Arrays.copyOfRange(sqlChunk, pos, sqlChunk.length);
        }
        //outln("SQL chunk #"+n+" length = "+sqlChunk.length+", pos = "+pos);
        if( 0==sqlChunk.length ) break;
        rc = capdb_prepare_v2(db, sqlChunk, outStmt, oTail);
        affirm(0 == rc);
        stmt = outStmt.get();
        pos = oTail.value;
        /*outln("SQL tail pos = "+pos+". Chunk = "+
              (new String(Arrays.copyOfRange(sqlChunk,0,pos),
              StandardCharsets.UTF_8)));*/
        switch(n){
          case 1: affirm(19 == pos); break;
          case 2: affirm(36 == pos); break;
          default: affirm( false /* can't happen */ );

        }
        ++n;
        affirm(0 != stmt.getNativePointer());
        rc = capdb_step(stmt);
        affirm(CAPDB_DONE == rc);
        capdb_finalize(stmt);
        affirm(0 == stmt.getNativePointer());
      }
    }


    rc = capdb_prepare_v3(db, "INSERT INTO t2(a) VALUES(1),(2),(3)",
                            0, outStmt);
    affirm(0 == rc);
    stmt = outStmt.get();
    affirm(0 != stmt.getNativePointer());
    capdb_finalize(stmt);
    affirm(0 == stmt.getNativePointer() );

    affirm( 0==capdb_errcode(db) );
    stmt = capdb_prepare(db, "intentional error");
    affirm( null==stmt );
    affirm( 0!=capdb_errcode(db) );
    affirm( 0==capdb_errmsg(db).indexOf("near \"intentional\"") );
    capdb_finalize(stmt);
    stmt = capdb_prepare(db, "/* empty input*/\n-- comments only");
    affirm( null==stmt );
    affirm( 0==capdb_errcode(db) );
    capdb_close_v2(db);
  }

  private void testBindFetchInt(){
    capdb db = createNewDb();
    execSql(db, "CREATE TABLE t(a)");

    capdb_stmt stmt = prepare(db, "INSERT INTO t(a) VALUES(:a);");
    affirm(1 == capdb_bind_parameter_count(stmt));
    final int paramNdx = capdb_bind_parameter_index(stmt, ":a");
    affirm(1 == paramNdx);
    affirm( ":a".equals(capdb_bind_parameter_name(stmt, paramNdx)));
    int total1 = 0;
    long rowid = -1;
    int changes = capdb_changes(db);
    int changesT = capdb_total_changes(db);
    long changes64 = capdb_changes64(db);
    long changesT64 = capdb_total_changes64(db);
    int rc;
    for(int i = 99; i < 102; ++i ){
      total1 += i;
      rc = capdb_bind_int(stmt, paramNdx, i);
      affirm(0 == rc);
      rc = capdb_step(stmt);
      capdb_reset(stmt);
      affirm(CAPDB_DONE == rc);
      long x = capdb_last_insert_rowid(db);
      affirm(x > rowid);
      rowid = x;
    }
    capdb_finalize(stmt);
    affirm(300 == total1);
    affirm(capdb_changes(db) > changes);
    affirm(capdb_total_changes(db) > changesT);
    affirm(capdb_changes64(db) > changes64);
    affirm(capdb_total_changes64(db) > changesT64);
    stmt = prepare(db, "SELECT a FROM t ORDER BY a DESC;");
    affirm( capdb_stmt_readonly(stmt) );
    affirm( !capdb_stmt_busy(stmt) );
    if( capdb_compileoption_used("ENABLE_COLUMN_METADATA") ){
      /* Unlike in native C code, JNI won't trigger an
         UnsatisfiedLinkError until these are called (on Linux, at
         least). */
      affirm("t".equals(capdb_column_table_name(stmt,0)));
      affirm("main".equals(capdb_column_database_name(stmt,0)));
      affirm("a".equals(capdb_column_origin_name(stmt,0)));
    }

    int total2 = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      affirm( capdb_stmt_busy(stmt) );
      total2 += capdb_column_int(stmt, 0);
      capdb_value sv = capdb_column_value(stmt, 0);
      affirm( null != sv );
      affirm( 0 != sv.getNativePointer() );
      affirm( CAPDB_INTEGER == capdb_value_type(sv) );
    }
    affirm( !capdb_stmt_busy(stmt) );
    capdb_finalize(stmt);
    affirm(total1 == total2);

    // capdb_value_frombind() checks...
    stmt = prepare(db, "SELECT 1, ?");
    capdb_bind_int(stmt, 1, 2);
    rc = capdb_step(stmt);
    affirm( CAPDB_ROW==rc );
    affirm( !capdb_value_frombind(capdb_column_value(stmt, 0)) );
    affirm( capdb_value_frombind(capdb_column_value(stmt, 1)) );
    capdb_finalize(stmt);

    capdb_close_v2(db);
    affirm(0 == db.getNativePointer());
  }

  private void testBindFetchInt64(){
    try (capdb db = createNewDb()){
      execSql(db, "CREATE TABLE t(a)");
      capdb_stmt stmt = prepare(db, "INSERT INTO t(a) VALUES(?);");
      long total1 = 0;
      for(long i = 0xffffffff; i < 0xffffffff + 3; ++i ){
        total1 += i;
        capdb_bind_int64(stmt, 1, i);
        capdb_step(stmt);
        capdb_reset(stmt);
      }
      capdb_finalize(stmt);
      stmt = prepare(db, "SELECT a FROM t ORDER BY a DESC;");
      long total2 = 0;
      while( CAPDB_ROW == capdb_step(stmt) ){
        total2 += capdb_column_int64(stmt, 0);
      }
      capdb_finalize(stmt);
      affirm(total1 == total2);
      //capdb_close_v2(db);
    }
  }

  private void testBindFetchDouble(){
    try (capdb db = createNewDb()){
      execSql(db, "CREATE TABLE t(a)");
      capdb_stmt stmt = prepare(db, "INSERT INTO t(a) VALUES(?);");
      double total1 = 0;
      for(double i = 1.5; i < 5.0; i = i + 1.0 ){
        total1 += i;
        capdb_bind_double(stmt, 1, i);
        capdb_step(stmt);
        capdb_reset(stmt);
      }
      capdb_finalize(stmt);
      stmt = prepare(db, "SELECT a FROM t ORDER BY a DESC;");
      double total2 = 0;
      int counter = 0;
      while( CAPDB_ROW == capdb_step(stmt) ){
        ++counter;
        total2 += capdb_column_double(stmt, 0);
      }
      affirm(4 == counter);
      capdb_finalize(stmt);
      affirm(total2<=total1+0.01 && total2>=total1-0.01);
      //capdb_close_v2(db);
    }
  }

  private void testBindFetchText(){
    capdb db = createNewDb();
    execSql(db, "CREATE TABLE t(a)");
    capdb_stmt stmt = prepare(db, "INSERT INTO t(a) VALUES(?);");
    String[] list1 = { "hell🤩", "w😃rld", "!🤩" };
    int rc;
    int n = 0;
    for( String e : list1 ){
      rc = (0==n)
        ? capdb_bind_text(stmt, 1, e)
        : capdb_bind_text16(stmt, 1, e);
      affirm(0 == rc);
      rc = capdb_step(stmt);
      affirm(CAPDB_DONE==rc);
      capdb_reset(stmt);
    }
    capdb_finalize(stmt);
    stmt = prepare(db, "SELECT a FROM t ORDER BY a DESC;");
    StringBuilder sbuf = new StringBuilder();
    n = 0;
    final boolean tryNio = capdb_jni_supports_nio();
    while( CAPDB_ROW == capdb_step(stmt) ){
      final capdb_value sv = capdb_value_dup(capdb_column_value(stmt,0));
      final String txt = capdb_column_text16(stmt, 0);
      sbuf.append( txt );
      affirm( txt.equals(new String(
                           capdb_column_text(stmt, 0),
                           StandardCharsets.UTF_8
                         )) );
      affirm( txt.length() < capdb_value_bytes(sv) );
      affirm( txt.equals(new String(
                           capdb_value_text(sv),
                           StandardCharsets.UTF_8)) );
      affirm( txt.length() == capdb_value_bytes16(sv)/2 );
      affirm( txt.equals(capdb_value_text16(sv)) );
      if( tryNio ){
        java.nio.ByteBuffer bu = capdb_value_nio_buffer(sv);
        byte ba[] = capdb_value_blob(sv);
        affirm( ba.length == bu.capacity() );
        int i = 0;
        for( byte b : ba ){
          affirm( b == bu.get(i++) );
        }
      }
      capdb_value_free(sv);
      ++n;
    }
    capdb_finalize(stmt);
    affirm(3 == n);
    affirm("w😃rldhell🤩!🤩".contentEquals(sbuf));

    try( capdb_stmt stmt2 = prepare(db, "SELECT ?, ?") ){
      rc = capdb_bind_text(stmt2, 1, "");
      affirm( 0==rc );
      rc = capdb_bind_text(stmt2, 2, (String)null);
      affirm( 0==rc );
      rc = capdb_step(stmt2);
      affirm( CAPDB_ROW==rc );
      byte[] colBa = capdb_column_text(stmt2, 0);
      affirm( 0==colBa.length );
      colBa = capdb_column_text(stmt2, 1);
      affirm( null==colBa );
      //capdb_finalize(stmt);
    }

    if(true){
      capdb_close_v2(db);
    }else{
      // Let the Object.finalize() override deal with it.
    }
  }

  private void testBindFetchBlob(){
    capdb db = createNewDb();
    execSql(db, "CREATE TABLE t(a)");
    capdb_stmt stmt = prepare(db, "INSERT INTO t(a) VALUES(?);");
    byte[] list1 = { 0x32, 0x33, 0x34 };
    int rc = capdb_bind_blob(stmt, 1, list1);
    affirm( 0==rc );
    rc = capdb_step(stmt);
    affirm(CAPDB_DONE == rc);
    capdb_finalize(stmt);
    stmt = prepare(db, "SELECT a FROM t ORDER BY a DESC;");
    int n = 0;
    int total = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      byte[] blob = capdb_column_blob(stmt, 0);
      affirm(3 == blob.length);
      int i = 0;
      for(byte b : blob){
        affirm(b == list1[i++]);
        total += b;
      }
      ++n;
    }
    capdb_finalize(stmt);
    affirm(1 == n);
    affirm(total == 0x32 + 0x33 + 0x34);
    capdb_close_v2(db);
  }

  @RequiresJniNio
  private void testBindByteBuffer(){
    /* TODO: these tests need to be much more extensive to check the
       begin/end range handling. */

    java.nio.ByteBuffer zeroCheck =
      java.nio.ByteBuffer.allocateDirect(0);
    affirm( null != zeroCheck );
    zeroCheck = null;
    capdb db = createNewDb();
    execSql(db, "CREATE TABLE t(a)");

    final java.nio.ByteBuffer buf = java.nio.ByteBuffer.allocateDirect(10);
    buf.put((byte)0x31)/*note that we'll skip this one*/
      .put((byte)0x32)
      .put((byte)0x33)
      .put((byte)0x34)
      .put((byte)0x35)/*we'll skip this one too*/;

    final int expectTotal = buf.get(1) + buf.get(2) + buf.get(3);
    capdb_stmt stmt = prepare(db, "INSERT INTO t(a) VALUES(?);");
    affirm( CAPDB_ERROR == capdb_bind_blob(stmt, 1, buf, -1, 0),
            "Buffer offset may not be negative." );
    affirm( 0 == capdb_bind_blob(stmt, 1, buf, 1, 3) );
    affirm( CAPDB_DONE == capdb_step(stmt) );
    capdb_finalize(stmt);
    stmt = prepare(db, "SELECT a FROM t;");
    int total = 0;
    affirm( CAPDB_ROW == capdb_step(stmt) );
    byte blob[] = capdb_column_blob(stmt, 0);
    java.nio.ByteBuffer nioBlob =
      capdb_column_nio_buffer(stmt, 0);
    affirm(3 == blob.length);
    affirm(blob.length == nioBlob.capacity());
    affirm(blob.length == nioBlob.limit());
    int i = 0;
    for(byte b : blob){
      affirm( i<=3 );
      affirm(b == buf.get(1 + i));
      affirm(b == nioBlob.get(i));
      ++i;
      total += b;
    }
    affirm( CAPDB_DONE == capdb_step(stmt) );
    capdb_finalize(stmt);
    affirm(total == expectTotal);

    SQLFunction func =
      new ScalarFunction(){
        public void xFunc(capdb_context cx, capdb_value[] args){
          capdb_result_blob(cx, buf, 1, 3);
        }
      };

    affirm( 0 == capdb_create_function(db, "myfunc", -1, CAPDB_UTF8, func) );
    stmt = prepare(db, "SELECT myfunc()");
    affirm( CAPDB_ROW == capdb_step(stmt) );
    blob = capdb_column_blob(stmt, 0);
    affirm(3 == blob.length);
    i = 0;
    total = 0;
    for(byte b : blob){
      affirm( i<=3 );
      affirm(b == buf.get(1 + i++));
      total += b;
    }
    affirm( CAPDB_DONE == capdb_step(stmt) );
    capdb_finalize(stmt);
    affirm(total == expectTotal);

    capdb_close_v2(db);
  }

  private void testSql(){
    capdb db = createNewDb();
    capdb_stmt stmt = prepare(db, "SELECT 1");
    affirm( "SELECT 1".equals(capdb_sql(stmt)) );
    capdb_finalize(stmt);
    stmt = prepare(db, "SELECT ?");
    capdb_bind_text(stmt, 1, "hell😃");
    final String expect = "SELECT 'hell😃'";
    affirm( expect.equals(capdb_expanded_sql(stmt)) );
    String n = capdb_normalized_sql(stmt);
    affirm( null==n || "SELECT?;".equals(n) );
    capdb_finalize(stmt);
    capdb_close(db);
  }

  private void testCollation(){
    final capdb db = createNewDb();
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES('a'),('b'),('c')");
    final ValueHolder<Integer> xDestroyCalled = new ValueHolder<>(0);
    final CollationCallback myCollation = new CollationCallback() {
        private final String myState =
          "this is local state. There is much like it, but this is mine.";
        @Override
        // Reverse-sorts its inputs...
        public int call(byte[] lhs, byte[] rhs){
          int len = lhs.length > rhs.length ? rhs.length : lhs.length;
          int c = 0, i = 0;
          for(i = 0; i < len; ++i){
            c = lhs[i] - rhs[i];
            if(0 != c) break;
          }
          if(0==c){
            if(i < lhs.length) c = 1;
            else if(i < rhs.length) c = -1;
          }
          return -c;
        }
        @Override
        public void xDestroy() {
          // Just demonstrates that xDestroy is called.
          ++xDestroyCalled.value;
        }
      };
    final CollationNeededCallback collLoader = new CollationNeededCallback(){
        @Override
        public void call(capdb dbArg, int eTextRep, String collationName){
          affirm(dbArg == db/* as opposed to a temporary object*/);
          capdb_create_collation(dbArg, "reversi", eTextRep, myCollation);
        }
      };
    int rc = capdb_collation_needed(db, collLoader);
    affirm( 0 == rc );
    rc = capdb_collation_needed(db, collLoader);
    affirm( 0 == rc /* Installing the same object again is a no-op */);
    capdb_stmt stmt = prepare(db, "SELECT a FROM t ORDER BY a COLLATE reversi");
    int counter = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      final String val = capdb_column_text16(stmt, 0);
      ++counter;
      //outln("REVERSI'd row#"+counter+": "+val);
      switch(counter){
        case 1: affirm("c".equals(val)); break;
        case 2: affirm("b".equals(val)); break;
        case 3: affirm("a".equals(val)); break;
      }
    }
    affirm(3 == counter);
    capdb_finalize(stmt);
    stmt = prepare(db, "SELECT a FROM t ORDER BY a");
    counter = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      final String val = capdb_column_text16(stmt, 0);
      ++counter;
      //outln("Non-REVERSI'd row#"+counter+": "+val);
      switch(counter){
        case 3: affirm("c".equals(val)); break;
        case 2: affirm("b".equals(val)); break;
        case 1: affirm("a".equals(val)); break;
      }
    }
    affirm(3 == counter);
    capdb_finalize(stmt);
    affirm( 0 == xDestroyCalled.value );
    rc = capdb_collation_needed(db, null);
    affirm( 0 == rc );
    capdb_close_v2(db);
    affirm( 0 == db.getNativePointer() );
    affirm( 1 == xDestroyCalled.value );
  }

  @SingleThreadOnly /* because it's thread-agnostic */
  private void testToUtf8(){
    /**
       https://docs.oracle.com/javase/8/docs/api/java/nio/charset/Charset.html

       Let's ensure that we can convert to standard UTF-8 in Java code
       (noting that the JNI native API has no way to do this).
    */
    final byte[] ba = "a \0 b".getBytes(StandardCharsets.UTF_8);
    affirm( 5 == ba.length /* as opposed to 6 in modified utf-8 */);
  }

  private void testStatus(){
    final OutputPointer.Int64 cur64 = new OutputPointer.Int64();
    final OutputPointer.Int64 high64 = new OutputPointer.Int64();
    final OutputPointer.Int32 cur32 = new OutputPointer.Int32();
    final OutputPointer.Int32 high32 = new OutputPointer.Int32();
    final capdb db = createNewDb();
    execSql(db, "create table t(a); insert into t values(1),(2),(3)");

    int rc = capdb_status(CAPDB_STATUS_MEMORY_USED, cur32, high32, false);
    affirm( 0 == rc );
    affirm( cur32.value > 0 );
    affirm( high32.value >= cur32.value );

    rc = capdb_status64(CAPDB_STATUS_MEMORY_USED, cur64, high64, false);
    affirm( 0 == rc );
    affirm( cur64.value > 0 );
    affirm( high64.value >= cur64.value );

    cur32.value = 0;
    high32.value = 1;
    rc = capdb_db_status(db, CAPDB_DBSTATUS_SCHEMA_USED, cur32, high32, false);
    affirm( 0 == rc );
    affirm( cur32.value > 0 );
    affirm( high32.value == 0 /* always 0 for SCHEMA_USED */ );

    capdb_close_v2(db);
  }

  private void testUdf1(){
    final capdb db = createNewDb();
    // These ValueHolders are just to confirm that the func did what we want...
    final ValueHolder<Boolean> xDestroyCalled = new ValueHolder<>(false);
    final ValueHolder<Integer> xFuncAccum = new ValueHolder<>(0);
    final ValueHolder<capdb_value[]> neverEverDoThisInClientCode = new ValueHolder<>(null);
    final ValueHolder<capdb_context> neverEverDoThisInClientCode2 = new ValueHolder<>(null);

    // Create an SQLFunction instance using one of its 3 subclasses:
    // Scalar, Aggregate, or Window:
    SQLFunction func =
      // Each of the 3 subclasses requires a different set of
      // functions, all of which must be implemented.  Anonymous
      // classes are a convenient way to implement these.
      new ScalarFunction(){
        public void xFunc(capdb_context cx, capdb_value[] args){
          affirm(db == capdb_context_db_handle(cx));
          if( null==neverEverDoThisInClientCode.value ){
            /* !!!NEVER!!! hold a reference to an capdb_value or
               capdb_context object like this in client code! They
               are ONLY legal for the duration of their single
               call. We do it here ONLY to test that the defenses
               against clients doing this are working. */
            neverEverDoThisInClientCode2.value = cx;
            neverEverDoThisInClientCode.value = args;
          }
          int result = 0;
          for( capdb_value v : args ) result += capdb_value_int(v);
          xFuncAccum.value += result;// just for post-run testing
          capdb_result_int(cx, result);
        }
        /* OPTIONALLY override xDestroy... */
        public void xDestroy(){
          xDestroyCalled.value = true;
        }
      };

    // Register and use the function...
    int rc = capdb_create_function(db, "myfunc", -1,
                                     CAPDB_UTF8 | CAPDB_INNOCUOUS,
                                     func);
    affirm(0 == rc);
    affirm(0 == xFuncAccum.value);
    final capdb_stmt stmt = prepare(db, "SELECT myfunc(1,2,3)");
    int n = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      affirm( 6 == capdb_column_int(stmt, 0) );
      ++n;
    }
    capdb_finalize(stmt);
    affirm(1 == n);
    affirm(6 == xFuncAccum.value);
    affirm( !xDestroyCalled.value );
    affirm( null!=neverEverDoThisInClientCode.value );
    affirm( null!=neverEverDoThisInClientCode2.value );
    affirm( 0<neverEverDoThisInClientCode.value.length );
    affirm( 0==neverEverDoThisInClientCode2.value.getNativePointer() );
    for( capdb_value sv : neverEverDoThisInClientCode.value ){
      affirm( 0==sv.getNativePointer() );
    }
    capdb_close_v2(db);
    affirm( xDestroyCalled.value );
  }

  private void testUdfThrows(){
    final capdb db = createNewDb();
    final ValueHolder<Integer> xFuncAccum = new ValueHolder<>(0);

    SQLFunction funcAgg = new AggregateFunction<Integer>(){
        @Override public void xStep(capdb_context cx, capdb_value[] args){
          /** Throwing from here should emit loud noise on stdout or stderr
              but the exception is suppressed because we have no way to inform
              sqlite about it from these callbacks. */
          //throw new RuntimeException("Throwing from an xStep");
        }
        @Override public void xFinal(capdb_context cx){
          throw new RuntimeException("Throwing from an xFinal");
        }
      };
    int rc = capdb_create_function(db, "myagg", 1, CAPDB_UTF8, funcAgg);
    affirm(0 == rc);
    affirm(0 == xFuncAccum.value);
    capdb_stmt stmt = prepare(db, "SELECT myagg(1)");
    rc = capdb_step(stmt);
    capdb_finalize(stmt);
    affirm( 0 != rc );
    affirm( capdb_errmsg(db).indexOf("an xFinal") > 0 );

    SQLFunction funcSc = new ScalarFunction(){
        @Override public void xFunc(capdb_context cx, capdb_value[] args){
          throw new RuntimeException("Throwing from an xFunc");
        }
      };
    rc = capdb_create_function(db, "mysca", 0, CAPDB_UTF8, funcSc);
    affirm(0 == rc);
    affirm(0 == xFuncAccum.value);
    stmt = prepare(db, "SELECT mysca()");
    rc = capdb_step(stmt);
    capdb_finalize(stmt);
    affirm( 0 != rc );
    affirm( capdb_errmsg(db).indexOf("an xFunc") > 0 );
    rc = capdb_create_function(db, "mysca", 1, -1, funcSc);
    affirm( CAPDB_FORMAT==rc, "invalid encoding value." );
    capdb_close_v2(db);
  }

  @SingleThreadOnly
  private void testUdfJavaObject(){
    affirm( !mtMode );
    final capdb db = createNewDb();
    final ValueHolder<capdb> testResult = new ValueHolder<>(db);
    final ValueHolder<Integer> boundObj = new ValueHolder<>(42);
    final SQLFunction func = new ScalarFunction(){
        public void xFunc(capdb_context cx, capdb_value args[]){
          capdb_result_java_object(cx, testResult.value);
          affirm( capdb_value_java_object(args[0]) == boundObj );
        }
      };
    int rc = capdb_create_function(db, "myfunc", -1, CAPDB_UTF8, func);
    affirm(0 == rc);
    capdb_stmt stmt = prepare(db, "select myfunc(?)");
    affirm( 0 != stmt.getNativePointer() );
    affirm( testResult.value == db );
    rc = capdb_bind_java_object(stmt, 1, boundObj);
    affirm( 0==rc );
    int n = 0;
    if( CAPDB_ROW == capdb_step(stmt) ){
      affirm( testResult.value == capdb_column_java_object(stmt, 0) );
      affirm( testResult.value == capdb_column_java_object(stmt, 0, capdb.class) );
      affirm( null == capdb_column_java_object(stmt, 0, capdb_stmt.class) );
      affirm( null == capdb_column_java_object(stmt,1) );
      final capdb_value v = capdb_column_value(stmt, 0);
      affirm( testResult.value == capdb_value_java_object(v) );
      affirm( testResult.value == capdb_value_java_object(v, capdb.class) );
      affirm( testResult.value ==
              capdb_value_java_object(v, testResult.value.getClass()) );
      affirm( testResult.value == capdb_value_java_object(v, Object.class) );
      affirm( null == capdb_value_java_object(v, String.class) );
      ++n;
    }
    capdb_finalize(stmt);
    affirm( 1 == n );
    affirm( 0==capdb_db_release_memory(db) );
    capdb_close_v2(db);
  }

  private void testUdfAggregate(){
    final capdb db = createNewDb();
    final ValueHolder<Boolean> xFinalNull =
      // To confirm that xFinal() is called with no aggregate state
      // when the corresponding result set is empty.
      new ValueHolder<>(false);
    final ValueHolder<capdb_value[]> neverEverDoThisInClientCode = new ValueHolder<>(null);
    final ValueHolder<capdb_context> neverEverDoThisInClientCode2 = new ValueHolder<>(null);
    SQLFunction func = new AggregateFunction<Integer>(){
        @Override
        public void xStep(capdb_context cx, capdb_value[] args){
          if( null==neverEverDoThisInClientCode.value ){
            /* !!!NEVER!!! hold a reference to an capdb_value or
               capdb_context object like this in client code! They
               are ONLY legal for the duration of their single
               call. We do it here ONLY to test that the defenses
               against clients doing this are working. */
            neverEverDoThisInClientCode.value = args;
          }
          final ValueHolder<Integer> agg = this.getAggregateState(cx, 0);
          agg.value += capdb_value_int(args[0]);
          affirm( agg == this.getAggregateState(cx, 0) );
        }
        @Override
        public void xFinal(capdb_context cx){
          if( null==neverEverDoThisInClientCode2.value ){
            neverEverDoThisInClientCode2.value = cx;
          }
          final Integer v = this.takeAggregateState(cx);
          if(null == v){
            xFinalNull.value = true;
            capdb_result_null(cx);
          }else{
            capdb_result_int(cx, v);
          }
        }
      };
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES(1),(2),(3)");
    int rc = capdb_create_function(db, "myfunc", 1, CAPDB_UTF8, func);
    affirm(0 == rc);
    capdb_stmt stmt = prepare(db, "select myfunc(a), myfunc(a+10) from t");
    affirm( 0==capdb_stmt_status(stmt, CAPDB_STMTSTATUS_RUN, false) );
    int n = 0;
    if( CAPDB_ROW == capdb_step(stmt) ){
      int v = capdb_column_int(stmt, 0);
      affirm( 6 == v );
      int v2 = capdb_column_int(stmt, 1);
      affirm( 30+v == v2 );
      ++n;
    }
    affirm( 1==n );
    affirm(!xFinalNull.value);
    affirm( null!=neverEverDoThisInClientCode.value );
    affirm( null!=neverEverDoThisInClientCode2.value );
    affirm( 0<neverEverDoThisInClientCode.value.length );
    affirm( 0==neverEverDoThisInClientCode2.value.getNativePointer() );
    capdb_reset(stmt);
    affirm( 1==capdb_stmt_status(stmt, CAPDB_STMTSTATUS_RUN, false) );
    // Ensure that the accumulator is reset on subsequent calls...
    n = 0;
    if( CAPDB_ROW == capdb_step(stmt) ){
      final int v = capdb_column_int(stmt, 0);
      affirm( 6 == v );
      ++n;
    }
    capdb_finalize(stmt);
    affirm( 1==n );

    stmt = prepare(db, "select myfunc(a), myfunc(a+a) from t order by a");
    n = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      final int c0 = capdb_column_int(stmt, 0);
      final int c1 = capdb_column_int(stmt, 1);
      ++n;
      affirm( 6 == c0 );
      affirm( 12 == c1 );
    }
    capdb_finalize(stmt);
    affirm( 1 == n );
    affirm(!xFinalNull.value);

    execSql(db, "SELECT myfunc(1) WHERE 0");
    affirm(xFinalNull.value);
    capdb_close_v2(db);
  }

  private void testUdfWindow(){
    final capdb db = createNewDb();
    /* Example window function, table, and results taken from:
       https://sqlite.org/windowfunctions.html#udfwinfunc */
    final SQLFunction func = new WindowFunction<Integer>(){

        private void xStepInverse(capdb_context cx, int v){
          this.getAggregateState(cx,0).value += v;
        }
        @Override public void xStep(capdb_context cx, capdb_value[] args){
          this.xStepInverse(cx, capdb_value_int(args[0]));
        }
        @Override public void xInverse(capdb_context cx, capdb_value[] args){
          this.xStepInverse(cx, -capdb_value_int(args[0]));
        }

        private void xFinalValue(capdb_context cx, Integer v){
          if(null == v) capdb_result_null(cx);
          else capdb_result_int(cx, v);
        }
        @Override public void xFinal(capdb_context cx){
          xFinalValue(cx, this.takeAggregateState(cx));
        }
        @Override public void xValue(capdb_context cx){
          xFinalValue(cx, this.getAggregateState(cx,null).value);
        }
      };
    int rc = capdb_create_function(db, "winsumint", 1, CAPDB_UTF8, func);
    affirm( 0 == rc );
    execSql(db, new String[] {
        "CREATE TEMP TABLE twin(x, y); INSERT INTO twin VALUES",
        "('a', 4),('b', 5),('c', 3),('d', 8),('e', 1)"
      });
    final capdb_stmt stmt = prepare(db,
                         "SELECT x, winsumint(y) OVER ("+
                         "ORDER BY x ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING"+
                         ") AS sum_y "+
                         "FROM twin ORDER BY x;");
    int n = 0;
    while( CAPDB_ROW == capdb_step(stmt) ){
      final String s = capdb_column_text16(stmt, 0);
      final int i = capdb_column_int(stmt, 1);
      switch(++n){
        case 1: affirm( "a".equals(s) && 9==i ); break;
        case 2: affirm( "b".equals(s) && 12==i ); break;
        case 3: affirm( "c".equals(s) && 16==i ); break;
        case 4: affirm( "d".equals(s) && 12==i ); break;
        case 5: affirm( "e".equals(s) && 9==i ); break;
        default: affirm( false /* cannot happen */ );
      }
    }
    capdb_finalize(stmt);
    affirm( 5 == n );
    capdb_close_v2(db);
  }

  private void listBoundMethods(){
    if(false){
      final java.lang.reflect.Field[] declaredFields =
        CApi.class.getDeclaredFields();
      outln("Bound constants:\n");
      for(java.lang.reflect.Field field : declaredFields) {
        if(java.lang.reflect.Modifier.isStatic(field.getModifiers())) {
          outln("\t",field.getName());
        }
      }
    }
    final java.lang.reflect.Method[] declaredMethods =
      CApi.class.getDeclaredMethods();
    final java.util.List<String> funcList = new java.util.ArrayList<>();
    for(java.lang.reflect.Method m : declaredMethods){
      if((m.getModifiers() & java.lang.reflect.Modifier.STATIC) != 0){
        final String name = m.getName();
        if(name.startsWith("capdb_")){
          funcList.add(name);
        }
      }
    }
    int count = 0;
    java.util.Collections.sort(funcList);
    for(String n : funcList){
      ++count;
      outln("\t",n,"()");
    }
    outln(count," functions named capdb_*.");
  }

  private void testTrace(){
    final capdb db = createNewDb();
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    /* Ensure that characters outside of the UTF BMP survive the trip
       from Java to capdb and back to Java. (At no small efficiency
       penalty.) */
    final String nonBmpChar = "😃";
    int rc = capdb_trace_v2(
      db, CAPDB_TRACE_STMT | CAPDB_TRACE_PROFILE
          | CAPDB_TRACE_ROW | CAPDB_TRACE_CLOSE,
      new TraceV2Callback(){
        @Override public int call(int traceFlag, Object pNative, Object x){
          ++counter.value;
          //outln("TRACE "+traceFlag+" pNative = "+pNative.getClass().getName());
          switch(traceFlag){
            case CAPDB_TRACE_STMT:
              affirm(pNative instanceof capdb_stmt);
              //outln("TRACE_STMT sql = "+x);
              affirm(x instanceof String);
              affirm( ((String)x).indexOf(nonBmpChar) > 0 );
              break;
            case CAPDB_TRACE_PROFILE:
              affirm(pNative instanceof capdb_stmt);
              affirm(x instanceof Long);
              //outln("TRACE_PROFILE time = "+x);
              break;
            case CAPDB_TRACE_ROW:
              affirm(pNative instanceof capdb_stmt);
              affirm(null == x);
              //outln("TRACE_ROW = "+capdb_column_text16((capdb_stmt)pNative, 0));
              break;
            case CAPDB_TRACE_CLOSE:
              affirm(pNative instanceof capdb);
              affirm(null == x);
              break;
            default:
              affirm(false /*cannot happen*/);
              break;
          }
          return 0;
        }
      });
    affirm( 0==rc );
    execSql(db, "SELECT coalesce(null,null,'"+nonBmpChar+"'); "+
            "SELECT 'w"+nonBmpChar+"orld'");
    affirm( 6 == counter.value );
    capdb_close_v2(db);
    affirm( 7 == counter.value );
  }

  @SingleThreadOnly /* because threads inherently break this test */
  private static void testBusy(){
    final String dbName = "_busy-handler.db";
    try{
      final OutputPointer.capdb outDb = new OutputPointer.capdb();
      final OutputPointer.capdb_stmt outStmt = new OutputPointer.capdb_stmt();

      int rc = capdb_open(dbName, outDb);
      ++metrics.dbOpen;
      affirm( 0 == rc );
      final capdb db1 = outDb.get();
      execSql(db1, "CREATE TABLE IF NOT EXISTS t(a)");
      rc = capdb_open(dbName, outDb);
      ++metrics.dbOpen;
      affirm( 0 == rc );
      affirm( outDb.get() != db1 );
      final capdb db2 = outDb.get();

      affirm( "main".equals( capdb_db_name(db1, 0) ) );
      rc = capdb_db_config(db1, CAPDB_DBCONFIG_MAINDBNAME, "foo");
      affirm( capdb_db_filename(db1, "foo").endsWith(dbName) );
      affirm( "foo".equals( capdb_db_name(db1, 0) ) );
      affirm( CAPDB_MISUSE == capdb_db_config(db1, 0, 0, null) );

      final ValueHolder<Integer> xBusyCalled = new ValueHolder<>(0);
      BusyHandlerCallback handler = new BusyHandlerCallback(){
          @Override public int call(int n){
            //outln("busy handler #"+n);
            return n > 2 ? 0 : ++xBusyCalled.value;
          }
        };
      rc = capdb_busy_handler(db2, handler);
      affirm(0 == rc);

      // Force a locked condition...
      execSql(db1, "BEGIN EXCLUSIVE");
      rc = capdb_prepare_v2(db2, "SELECT * from t", outStmt);
      affirm( CAPDB_BUSY == rc);
      affirm( null == outStmt.get() );
      affirm( 3 == xBusyCalled.value );
      capdb_close_v2(db1);
      capdb_close_v2(db2);
    }finally{
      try{(new java.io.File(dbName)).delete();}
      catch(Exception e){/* ignore */}
    }
  }

  private void testProgress(){
    final capdb db = createNewDb();
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    capdb_progress_handler(db, 1, new ProgressHandlerCallback(){
        @Override public int call(){
          ++counter.value;
          return 0;
        }
      });
    execSql(db, "SELECT 1; SELECT 2;");
    affirm( counter.value > 0 );
    int nOld = counter.value;
    capdb_progress_handler(db, 0, null);
    execSql(db, "SELECT 1; SELECT 2;");
    affirm( nOld == counter.value );
    capdb_close_v2(db);
  }

  private void testCommitHook(){
    final capdb db = createNewDb();
    capdb_extended_result_codes(db, true);
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    final ValueHolder<Integer> hookResult = new ValueHolder<>(0);
    final CommitHookCallback theHook = new CommitHookCallback(){
        @Override public int call(){
          ++counter.value;
          return hookResult.value;
        }
      };
    CommitHookCallback oldHook = capdb_commit_hook(db, theHook);
    affirm( null == oldHook );
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES('a'),('b'),('c')");
    affirm( 2 == counter.value );
    execSql(db, "BEGIN; SELECT 1; SELECT 2; COMMIT;");
    affirm( 2 == counter.value /* NOT invoked if no changes are made */ );
    execSql(db, "BEGIN; update t set a='d' where a='c'; COMMIT;");
    affirm( 3 == counter.value );
    oldHook = capdb_commit_hook(db, theHook);
    affirm( theHook == oldHook );
    execSql(db, "BEGIN; update t set a='e' where a='d'; COMMIT;");
    affirm( 4 == counter.value );
    oldHook = capdb_commit_hook(db, null);
    affirm( theHook == oldHook );
    execSql(db, "BEGIN; update t set a='f' where a='e'; COMMIT;");
    affirm( 4 == counter.value );
    oldHook = capdb_commit_hook(db, null);
    affirm( null == oldHook );
    execSql(db, "BEGIN; update t set a='g' where a='f'; COMMIT;");
    affirm( 4 == counter.value );

    final CommitHookCallback newHook = new CommitHookCallback(){
        @Override public int call(){return 0;}
      };
    oldHook = capdb_commit_hook(db, newHook);
    affirm( null == oldHook );
    execSql(db, "BEGIN; update t set a='h' where a='g'; COMMIT;");
    affirm( 4 == counter.value );
    oldHook = capdb_commit_hook(db, theHook);
    affirm( newHook == oldHook );
    execSql(db, "BEGIN; update t set a='i' where a='h'; COMMIT;");
    affirm( 5 == counter.value );
    hookResult.value = CAPDB_ERROR;
    int rc = execSql(db, false, "BEGIN; update t set a='j' where a='i'; COMMIT;");
    affirm( CAPDB_CONSTRAINT_COMMITHOOK == rc );
    affirm( 6 == counter.value );
    capdb_close_v2(db);
  }

  private void testUpdateHook(){
    final capdb db = createNewDb();
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    final ValueHolder<Integer> expectedOp = new ValueHolder<>(0);
    final UpdateHookCallback theHook = new UpdateHookCallback(){
        @Override
        public void call(int opId, String dbName, String tableName, long rowId){
          ++counter.value;
          if( 0!=expectedOp.value ){
            affirm( expectedOp.value == opId );
          }
        }
      };
    UpdateHookCallback oldHook = capdb_update_hook(db, theHook);
    affirm( null == oldHook );
    expectedOp.value = CAPDB_INSERT;
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES('a'),('b'),('c')");
    affirm( 3 == counter.value );
    expectedOp.value = CAPDB_UPDATE;
    execSql(db, "update t set a='d' where a='c';");
    affirm( 4 == counter.value );
    oldHook = capdb_update_hook(db, theHook);
    affirm( theHook == oldHook );
    expectedOp.value = CAPDB_DELETE;
    execSql(db, "DELETE FROM t where a='d'");
    affirm( 5 == counter.value );
    oldHook = capdb_update_hook(db, null);
    affirm( theHook == oldHook );
    execSql(db, "update t set a='e' where a='b';");
    affirm( 5 == counter.value );
    oldHook = capdb_update_hook(db, null);
    affirm( null == oldHook );

    final UpdateHookCallback newHook = new UpdateHookCallback(){
        @Override public void call(int opId, String dbName, String tableName, long rowId){
        }
      };
    oldHook = capdb_update_hook(db, newHook);
    affirm( null == oldHook );
    execSql(db, "update t set a='h' where a='a'");
    affirm( 5 == counter.value );
    oldHook = capdb_update_hook(db, theHook);
    affirm( newHook == oldHook );
    expectedOp.value = CAPDB_UPDATE;
    execSql(db, "update t set a='i' where a='h'");
    affirm( 6 == counter.value );
    capdb_close_v2(db);
  }

  /**
     This test is functionally identical to testUpdateHook(), only with a
     different callback type.
  */
  private void testPreUpdateHook(){
    if( !capdb_compileoption_used("ENABLE_PREUPDATE_HOOK") ){
      //outln("Skipping testPreUpdateHook(): no pre-update hook support.");
      return;
    }
    final capdb db = createNewDb();
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    final ValueHolder<Integer> expectedOp = new ValueHolder<>(0);
    final PreupdateHookCallback theHook = new PreupdateHookCallback(){
        @Override
        public void call(capdb db, int opId, String dbName, String dbTable,
                         long iKey1, long iKey2 ){
          ++counter.value;
          switch( opId ){
            case CAPDB_UPDATE:
              affirm( 0 < capdb_preupdate_count(db) );
              affirm( null != capdb_preupdate_new(db, 0) );
              affirm( null != capdb_preupdate_old(db, 0) );
              break;
            case CAPDB_INSERT:
              affirm( null != capdb_preupdate_new(db, 0) );
              break;
            case CAPDB_DELETE:
              affirm( null != capdb_preupdate_old(db, 0) );
              break;
            default:
              break;
          }
          if( 0!=expectedOp.value ){
            affirm( expectedOp.value == opId );
          }
        }
      };
    PreupdateHookCallback oldHook = capdb_preupdate_hook(db, theHook);
    affirm( null == oldHook );
    expectedOp.value = CAPDB_INSERT;
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES('a'),('b'),('c')");
    affirm( 3 == counter.value );
    expectedOp.value = CAPDB_UPDATE;
    execSql(db, "update t set a='d' where a='c';");
    affirm( 4 == counter.value );
    oldHook = capdb_preupdate_hook(db, theHook);
    affirm( theHook == oldHook );
    expectedOp.value = CAPDB_DELETE;
    execSql(db, "DELETE FROM t where a='d'");
    affirm( 5 == counter.value );
    oldHook = capdb_preupdate_hook(db, null);
    affirm( theHook == oldHook );
    execSql(db, "update t set a='e' where a='b';");
    affirm( 5 == counter.value );
    oldHook = capdb_preupdate_hook(db, null);
    affirm( null == oldHook );

    final PreupdateHookCallback newHook = new PreupdateHookCallback(){
        @Override
        public void call(capdb db, int opId, String dbName,
                         String tableName, long iKey1, long iKey2){
        }
      };
    oldHook = capdb_preupdate_hook(db, newHook);
    affirm( null == oldHook );
    execSql(db, "update t set a='h' where a='a'");
    affirm( 5 == counter.value );
    oldHook = capdb_preupdate_hook(db, theHook);
    affirm( newHook == oldHook );
    expectedOp.value = CAPDB_UPDATE;
    execSql(db, "update t set a='i' where a='h'");
    affirm( 6 == counter.value );

    capdb_close_v2(db);
  }

  private void testRollbackHook(){
    final capdb db = createNewDb();
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    final RollbackHookCallback theHook = new RollbackHookCallback(){
        @Override public void call(){
          ++counter.value;
        }
      };
    RollbackHookCallback oldHook = capdb_rollback_hook(db, theHook);
    affirm( null == oldHook );
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES('a'),('b'),('c')");
    affirm( 0 == counter.value );
    execSql(db, false, "BEGIN; SELECT 1; SELECT 2; ROLLBACK;");
    affirm( 1 == counter.value /* contra to commit hook, is invoked if no changes are made */ );

    final RollbackHookCallback newHook = new RollbackHookCallback(){
        @Override public void call(){return;}
      };
    oldHook = capdb_rollback_hook(db, newHook);
    affirm( theHook == oldHook );
    execSql(db, false, "BEGIN; SELECT 1; ROLLBACK;");
    affirm( 1 == counter.value );
    oldHook = capdb_rollback_hook(db, theHook);
    affirm( newHook == oldHook );
    execSql(db, false, "BEGIN; SELECT 1; ROLLBACK;");
    affirm( 2 == counter.value );
    int rc = execSql(db, false, "BEGIN; SELECT 1; ROLLBACK;");
    affirm( 0 == rc );
    affirm( 3 == counter.value );
    capdb_close_v2(db);
  }

  /**
     If FTS5 is available, runs FTS5 tests, else returns with no side
     effects. If it is available but loading of the FTS5 bits fails,
     it throws.
  */
  @SuppressWarnings("unchecked")
  @SingleThreadOnly /* because the Fts5 parts are not yet known to be
                       thread-safe */
  private void testFts5() throws Exception {
    if( !capdb_compileoption_used("ENABLE_FTS5") ){
      //outln("CAPDB_ENABLE_FTS5 is not set. Skipping FTS5 tests.");
      return;
    }
    Exception err = null;
    try {
      Class t = Class.forName("org.sqlite.jni.fts5.TesterFts5");
      java.lang.reflect.Constructor ctor = t.getConstructor();
      ctor.setAccessible(true);
      final long timeStart = System.currentTimeMillis();
      ctor.newInstance() /* will run all tests */;
      final long timeEnd = System.currentTimeMillis();
      outln("FTS5 Tests done in ",(timeEnd - timeStart),"ms");
    }catch(ClassNotFoundException e){
      outln("FTS5 classes not loaded.");
      err = e;
    }catch(NoSuchMethodException e){
      outln("FTS5 tester ctor not found.");
      err = e;
    }catch(Exception e){
      outln("Instantiation of FTS5 tester threw.");
      err = e;
    }
    if( null != err ){
      outln("Exception: "+err);
      err.printStackTrace();
      throw err;
    }
  }

  private void testAuthorizer(){
    final capdb db = createNewDb();
    final ValueHolder<Integer> counter = new ValueHolder<>(0);
    final ValueHolder<Integer> authRc = new ValueHolder<>(0);
    final AuthorizerCallback auth = new AuthorizerCallback(){
        public int call(int op, String s0, String s1, String s2, String s3){
          ++counter.value;
          //outln("xAuth(): "+s0+" "+s1+" "+s2+" "+s3);
          return authRc.value;
        }
      };
    execSql(db, "CREATE TABLE t(a); INSERT INTO t(a) VALUES('a'),('b'),('c')");
    capdb_set_authorizer(db, auth);
    execSql(db, "UPDATE t SET a=1");
    affirm( 1 == counter.value );
    authRc.value = CAPDB_DENY;
    int rc = execSql(db, false, "UPDATE t SET a=2");
    affirm( CAPDB_AUTH==rc );
    capdb_set_authorizer(db, null);
    rc = execSql(db, false, "UPDATE t SET a=2");
    affirm( 0==rc );
    // TODO: expand these tests considerably
    capdb_close(db);
  }

  @SingleThreadOnly /* because multiple threads legitimately make these
                       results unpredictable */
  private synchronized void testAutoExtension(){
    final ValueHolder<Integer> val = new ValueHolder<>(0);
    final ValueHolder<String> toss = new ValueHolder<>(null);
    final AutoExtensionCallback ax = new AutoExtensionCallback(){
        @Override public int call(capdb db){
          ++val.value;
          if( null!=toss.value ){
            throw new RuntimeException(toss.value);
          }
          return 0;
        }
      };
    int rc = capdb_auto_extension( ax );
    affirm( 0==rc );
    capdb_close(createNewDb());
    affirm( 1==val.value );
    capdb_close(createNewDb());
    affirm( 2==val.value );
    capdb_reset_auto_extension();
    capdb_close(createNewDb());
    affirm( 2==val.value );
    rc = capdb_auto_extension( ax );
    affirm( 0==rc );
    // Must not add a new entry
    rc = capdb_auto_extension( ax );
    affirm( 0==rc );
    capdb_close( createNewDb() );
    affirm( 3==val.value );

    capdb db = createNewDb();
    affirm( 4==val.value );
    execSql(db, "ATTACH ':memory:' as foo");
    affirm( 4==val.value, "ATTACH uses the same connection, not sub-connections." );
    capdb_close(db);
    db = null;

    affirm( capdb_cancel_auto_extension(ax) );
    affirm( !capdb_cancel_auto_extension(ax) );
    capdb_close(createNewDb());
    affirm( 4==val.value );
    rc = capdb_auto_extension( ax );
    affirm( 0==rc );
    Exception err = null;
    toss.value = "Throwing from auto_extension.";
    try{
      capdb_close(createNewDb());
    }catch(Exception e){
      err = e;
    }
    affirm( err!=null );
    affirm( err.getMessage().indexOf(toss.value)>0 );
    toss.value = null;

    val.value = 0;
    final AutoExtensionCallback ax2 = new AutoExtensionCallback(){
        @Override public int call(capdb db){
          ++val.value;
          return 0;
        }
      };
    rc = capdb_auto_extension( ax2 );
    affirm( 0 == rc );
    capdb_close(createNewDb());
    affirm( 2 == val.value );
    affirm( capdb_cancel_auto_extension(ax) );
    affirm( !capdb_cancel_auto_extension(ax) );
    capdb_close(createNewDb());
    affirm( 3 == val.value );
    rc = capdb_auto_extension( ax );
    affirm( 0 == rc );
    capdb_close(createNewDb());
    affirm( 5 == val.value );
    affirm( capdb_cancel_auto_extension(ax2) );
    affirm( !capdb_cancel_auto_extension(ax2) );
    capdb_close(createNewDb());
    affirm( 6 == val.value );
    rc = capdb_auto_extension( ax2 );
    affirm( 0 == rc );
    capdb_close(createNewDb());
    affirm( 8 == val.value );

    capdb_reset_auto_extension();
    capdb_close(createNewDb());
    affirm( 8 == val.value );
    affirm( !capdb_cancel_auto_extension(ax) );
    affirm( !capdb_cancel_auto_extension(ax2) );
    capdb_close(createNewDb());
    affirm( 8 == val.value );
  }


  private void testColumnMetadata(){
    final capdb db = createNewDb();
    execSql(db, new String[] {
        "CREATE TABLE t(a duck primary key not null collate noCase); ",
        "INSERT INTO t(a) VALUES(1),(2),(3);"
      });
    OutputPointer.Bool bNotNull = new OutputPointer.Bool();
    OutputPointer.Bool bPrimaryKey = new OutputPointer.Bool();
    OutputPointer.Bool bAutoinc = new OutputPointer.Bool();
    OutputPointer.String zCollSeq = new OutputPointer.String();
    OutputPointer.String zDataType = new OutputPointer.String();
    int rc = capdb_table_column_metadata(
      db, "main", "t", "a", zDataType, zCollSeq,
      bNotNull, bPrimaryKey, bAutoinc);
    affirm( 0==rc );
    affirm( bPrimaryKey.value );
    affirm( !bAutoinc.value );
    affirm( bNotNull.value );
    affirm( "noCase".equals(zCollSeq.value) );
    affirm( "duck".equals(zDataType.value) );

    TableColumnMetadata m =
      capdb_table_column_metadata(db, "main", "t", "a");
    affirm( null != m );
    affirm( bPrimaryKey.value == m.isPrimaryKey() );
    affirm( bAutoinc.value == m.isAutoincrement() );
    affirm( bNotNull.value == m.isNotNull() );
    affirm( zCollSeq.value.equals(m.getCollation()) );
    affirm( zDataType.value.equals(m.getDataType()) );

    affirm( null == capdb_table_column_metadata(db, "nope", "t", "a") );
    affirm( null == capdb_table_column_metadata(db, "main", "nope", "a") );

    m = capdb_table_column_metadata(db, "main", "t", null)
      /* Check only for existence of table */;
    affirm( null != m );
    affirm( m.isPrimaryKey() );
    affirm( !m.isAutoincrement() );
    affirm( !m.isNotNull() );
    affirm( "BINARY".equalsIgnoreCase(m.getCollation()) );
    affirm( "INTEGER".equalsIgnoreCase(m.getDataType()) );

    capdb_close_v2(db);
  }

  private void testTxnState(){
    final capdb db = createNewDb();
    affirm( CAPDB_TXN_NONE == capdb_txn_state(db, null) );
    affirm( capdb_get_autocommit(db) );
    execSql(db, "BEGIN;");
    affirm( !capdb_get_autocommit(db) );
    affirm( CAPDB_TXN_NONE == capdb_txn_state(db, null) );
    execSql(db, "SELECT * FROM sqlite_schema;");
    affirm( CAPDB_TXN_READ == capdb_txn_state(db, "main") );
    execSql(db, "CREATE TABLE t(a);");
    affirm( CAPDB_TXN_WRITE ==  capdb_txn_state(db, null) );
    execSql(db, "ROLLBACK;");
    affirm( CAPDB_TXN_NONE == capdb_txn_state(db, null) );
    capdb_close_v2(db);
  }


  private void testExplain(){
    final capdb db = createNewDb();
    capdb_stmt stmt = prepare(db,"SELECT 1");

    affirm( 0 == capdb_stmt_isexplain(stmt) );
    int rc = capdb_stmt_explain(stmt, 1);
    affirm( 1 == capdb_stmt_isexplain(stmt) );
    rc = capdb_stmt_explain(stmt, 2);
    affirm( 2 == capdb_stmt_isexplain(stmt) );
    capdb_finalize(stmt);
    capdb_close_v2(db);
  }

  private void testLimit(){
    final capdb db = createNewDb();
    int v;

    v = capdb_limit(db, CAPDB_LIMIT_LENGTH, -1);
    affirm( v > 0 );
    affirm( v == capdb_limit(db, CAPDB_LIMIT_LENGTH, v-1) );
    affirm( v-1 == capdb_limit(db, CAPDB_LIMIT_LENGTH, -1) );
    capdb_close_v2(db);
  }

  private void testComplete(){
    affirm( 0==capdb_complete("select 1") );
    affirm( 0!=capdb_complete("select 1;") );
    affirm( 0!=capdb_complete("nope 'nope' 'nope' 1;"), "Yup" );
  }

  private void testKeyword(){
    final int n = capdb_keyword_count();
    affirm( n>0 );
    affirm( !capdb_keyword_check("_nope_") );
    affirm( capdb_keyword_check("seLect") );
    affirm( null!=capdb_keyword_name(0) );
    affirm( null!=capdb_keyword_name(n-1) );
    affirm( null==capdb_keyword_name(n) );
  }

  private void testBackup(){
    final capdb dbDest = createNewDb();

    try (capdb dbSrc = createNewDb()) {
      execSql(dbSrc, new String[]{
          "pragma page_size=512; VACUUM;",
          "create table t(a);",
          "insert into t(a) values(1),(2),(3);"
        });
      affirm( null==capdb_backup_init(dbSrc,"main",dbSrc,"main") );
      try (capdb_backup b = capdb_backup_init(dbDest,"main",dbSrc,"main")) {
        affirm( null!=b );
        affirm( b.getNativePointer()!=0 );
        int rc;
        while( CAPDB_DONE!=(rc = capdb_backup_step(b, 1)) ){
          affirm( 0==rc );
        }
        affirm( capdb_backup_pagecount(b) > 0 );
        rc = capdb_backup_finish(b);
        affirm( 0==rc );
        affirm( b.getNativePointer()==0 );
      }
    }

    try (capdb_stmt stmt = prepare(dbDest,"SELECT sum(a) from t")) {
      capdb_step(stmt);
      affirm( capdb_column_int(stmt,0) == 6 );
    }
    capdb_close_v2(dbDest);
  }

  private void testRandomness(){
    byte[] foo = new byte[20];
    int i = 0;
    for( byte b : foo ){
      i += b;
    }
    affirm( i==0 );
    capdb_randomness(foo);
    for( byte b : foo ){
      if(b!=0) ++i;
    }
    affirm( i!=0, "There's a very slight chance that 0 is actually correct." );
  }

  private void testBlobOpen(){
    final capdb db = createNewDb();

    execSql(db, "CREATE TABLE T(a BLOB);"
            +"INSERT INTO t(rowid,a) VALUES(1, 'def'),(2, 'XYZ');"
    );
    final OutputPointer.capdb_blob pOut = new OutputPointer.capdb_blob();
    int rc = capdb_blob_open(db, "main", "t", "a",
                               capdb_last_insert_rowid(db), 1, pOut);
    affirm( 0==rc );
    capdb_blob b = pOut.take();
    affirm( null!=b );
    affirm( 0!=b.getNativePointer() );
    affirm( 3==capdb_blob_bytes(b) );
    rc = capdb_blob_write( b, new byte[] {100, 101, 102 /*"DEF"*/}, 0);
    affirm( 0==rc );
    rc = capdb_blob_close(b);
    affirm( 0==rc );
    rc = capdb_blob_close(b);
    affirm( 0!=rc );
    affirm( 0==b.getNativePointer() );
    capdb_stmt stmt = prepare(db,"SELECT length(a), a FROM t ORDER BY a");
    affirm( CAPDB_ROW == capdb_step(stmt) );
    affirm( 3 == capdb_column_int(stmt,0) );
    affirm( "def".equals(capdb_column_text16(stmt,1)) );
    capdb_finalize(stmt);

    b = capdb_blob_open(db, "main", "t", "a",
                          capdb_last_insert_rowid(db), 0);
    affirm( null!=b );
    rc = capdb_blob_reopen(b, 2);
    affirm( 0==rc );
    final byte[] tgt = new byte[3];
    rc = capdb_blob_read(b, tgt, 0);
    affirm( 0==rc );
    affirm( 100==tgt[0] && 101==tgt[1] && 102==tgt[2], "DEF" );
    rc = capdb_blob_close(b);
    affirm( 0==rc );

    if( !capdb_jni_supports_nio() ){
      outln("WARNING: skipping tests for ByteBuffer-using capdb_blob APIs ",
            "because this platform lacks that support.");
      capdb_close_v2(db);
      return;
    }
    /* Sanity checks for the java.nio.ByteBuffer-taking overloads of
       capdb_blob_read/write(). */
    execSql(db, "UPDATE t SET a=zeroblob(10)");
    b = capdb_blob_open(db, "main", "t", "a", 1, 1);
    affirm( null!=b );
    java.nio.ByteBuffer bb = java.nio.ByteBuffer.allocateDirect(10);
    for( byte i = 0; i < 10; ++i ){
      bb.put((int)i, (byte)(48+i & 0xff));
    }
    rc = capdb_blob_write(b, 1, bb, 1, 10);
    affirm( rc==CAPDB_ERROR, "b length < (srcOffset + bb length)" );
    rc = capdb_blob_write(b, -1, bb);
    affirm( rc==CAPDB_ERROR, "Target offset may not be negative" );
    rc = capdb_blob_write(b, 0, bb, -1, -1);
    affirm( rc==CAPDB_ERROR, "Source offset may not be negative" );
    rc = capdb_blob_write(b, 1, bb, 1, 8);
    affirm( rc==0 );
    // b's contents: 0 49  50  51  52  53  54  55  56  0
    //        ascii: 0 '1' '2' '3' '4' '5' '6' '7' '8' 0
    byte br[] = new byte[10];
    java.nio.ByteBuffer bbr =
      java.nio.ByteBuffer.allocateDirect(bb.limit());
    rc = capdb_blob_read( b, br, 0 );
    affirm( rc==0 );
    rc = capdb_blob_read( b, bbr );
    affirm( rc==0 );
    java.nio.ByteBuffer bbr2 = capdb_blob_read_nio_buffer(b, 0, 12);
    affirm( null==bbr2, "Read size is too big");
    bbr2 = capdb_blob_read_nio_buffer(b, -1, 3);
    affirm( null==bbr2, "Source offset is negative");
    bbr2 = capdb_blob_read_nio_buffer(b, 5, 6);
    affirm( null==bbr2, "Read pos+size is too big");
    bbr2 = capdb_blob_read_nio_buffer(b, 4, 7);
    affirm( null==bbr2, "Read pos+size is too big");
    bbr2 = capdb_blob_read_nio_buffer(b, 4, 6);
    affirm( null!=bbr2 );
    java.nio.ByteBuffer bbr3 =
      java.nio.ByteBuffer.allocateDirect(2 * bb.limit());
    java.nio.ByteBuffer bbr4 =
      java.nio.ByteBuffer.allocateDirect(5);
    rc = capdb_blob_read( b, bbr3 );
    affirm( rc==0 );
    rc = capdb_blob_read( b, bbr4 );
    affirm( rc==0 );
    affirm( capdb_blob_bytes(b)==bbr3.limit() );
    affirm( 5==bbr4.limit() );
    capdb_blob_close(b);
    affirm( 0==br[0] );
    affirm( 0==br[9] );
    affirm( 0==bbr.get(0) );
    affirm( 0==bbr.get(9) );
    affirm( bbr2.limit() == 6 );
    affirm( 0==bbr3.get(0) );
    {
      Exception ex = null;
      try{ bbr3.get(11); }
      catch(Exception e){ex = e;}
      affirm( ex instanceof IndexOutOfBoundsException,
              "bbr3.limit() was reset by read()" );
      ex = null;
    }
    affirm( 0==bbr4.get(0) );
    for( int i = 1; i < 9; ++i ){
      affirm( br[i] == 48 + i );
      affirm( br[i] == bbr.get(i) );
      affirm( br[i] == bbr3.get(i) );
      if( i>3 ){
        affirm( br[i] == bbr2.get(i-4) );
      }
      if( i < bbr4.limit() ){
        affirm( br[i] == bbr4.get(i) );
      }
    }
    capdb_close_v2(db);
  }

  private void testPrepareMulti(){
    final capdb db = createNewDb();
    final String[] sql = {
      "create table t(","a)",
      "; insert into t(a) values(1),(2),(3);",
      "select a from t;"
    };
    final List<capdb_stmt> liStmt = new ArrayList<>();
    final PrepareMultiCallback proxy = new PrepareMultiCallback.StepAll();
    final ValueHolder<String> toss = new ValueHolder<>(null);
    PrepareMultiCallback m = new PrepareMultiCallback() {
        @Override public int call(capdb_stmt st){
          liStmt.add(st);
          if( null!=toss.value ){
            throw new RuntimeException(toss.value);
          }
          return proxy.call(st);
        }
      };
    int rc = capdb_prepare_multi(db, sql, m);
    affirm( 0==rc );
    affirm( liStmt.size() == 3 );
    for( capdb_stmt st : liStmt ){
      capdb_finalize(st);
    }
    toss.value = "This is an exception.";
    rc = capdb_prepare_multi(db, "SELECT 1", m);
    affirm( CAPDB_ERROR==rc );
    affirm( capdb_errmsg(db).indexOf(toss.value)>0 );
    capdb_close_v2(db);
  }

  private void testSetErrmsg(){
    final capdb db = createNewDb();

    int rc = capdb_set_errmsg(db, CAPDB_RANGE, "nope");
    affirm( 0==rc );
    affirm( CAPDB_MISUSE == capdb_set_errmsg(null, 0, null) );
    affirm( "nope".equals(capdb_errmsg(db)) );
    affirm( CAPDB_RANGE == capdb_errcode(db) );
    rc = capdb_set_errmsg(db, 0, null);
    affirm( "not an error".equals(capdb_errmsg(db)) );
    affirm( 0 == capdb_errcode(db) );
    capdb_close_v2(db);
  }

  /* Copy/paste/rename this to add new tests. */
  private void _testTemplate(){
    final capdb db = createNewDb();
    capdb_stmt stmt = prepare(db,"SELECT 1");
    capdb_finalize(stmt);
    capdb_close_v2(db);
  }


  @ManualTest /* we really only want to run this test manually */
  private void testSleep(){
    out("Sleeping briefly... ");
    capdb_sleep(600);
    outln("Woke up.");
  }

  private void nap() throws InterruptedException {
    if( takeNaps ){
      Thread.sleep(java.util.concurrent.ThreadLocalRandom.current().nextInt(3, 17), 0);
    }
  }

  @ManualTest /* because we only want to run this test on demand */
  private void testFail(){
    affirm( false, "Intentional failure." );
  }

  private void runTests(boolean fromThread) throws Exception {
    if(false) showCompileOption();
    List<java.lang.reflect.Method> mlist = testMethods;
    affirm( null!=mlist );
    if( shuffle ){
      mlist = new ArrayList<>( testMethods.subList(0, testMethods.size()) );
      java.util.Collections.shuffle(mlist);
    }
    if( (!fromThread && listRunTests>0) || listRunTests>1 ){
      synchronized(this.getClass()){
        if( !fromThread ){
          out("Initial test"," list: ");
          for(java.lang.reflect.Method m : testMethods){
            out(m.getName()+" ");
          }
          outln();
          outln("(That list excludes some which are hard-coded to run.)");
        }
        out("Running"," tests: ");
        for(java.lang.reflect.Method m : mlist){
          out(m.getName()+" ");
        }
        outln();
      }
    }
    for(java.lang.reflect.Method m : mlist){
      nap();
      try{
        m.invoke(this);
      }catch(java.lang.reflect.InvocationTargetException e){
        outln("FAILURE: ",m.getName(),"(): ", e.getCause());
        throw e;
      }
    }
    synchronized( this.getClass() ){
      ++nTestRuns;
    }
  }

  public void run() {
    try {
      runTests(0!=this.tId);
    }catch(Exception e){
      synchronized( listErrors ){
        listErrors.add(e);
      }
    }finally{
      affirm( capdb_java_uncache_thread() );
      affirm( !capdb_java_uncache_thread() );
    }
  }

  /**
     Runs the basic capdb JNI binding sanity-check suite.

     CLI flags:

     -q|-quiet: disables most test output.

     -t|-thread N: runs the tests in N threads
      concurrently. Default=1.

     -r|-repeat N: repeats the tests in a loop N times, each one
      consisting of the -thread value's threads.

     -shuffle: randomizes the order of most of the test functions.

     -naps: sleep small random intervals between tests in order to add
     some chaos for cross-thread contention.


     -list-tests: outputs the list of tests being run, minus some
      which are hard-coded. In multi-threaded mode, use this twice to
      to emit the list run by each thread (which may differ from the initial
      list, in particular if -shuffle is used).

     -fail: forces an exception to be thrown during the test run.  Use
     with -shuffle to make its appearance unpredictable.

     -v: emit some developer-mode info at the end.
  */
  public static void main(String[] args) throws Exception {
    int nThread = 1;
    boolean doSomethingForDev = false;
    int nRepeat = 1;
    boolean forceFail = false;
    boolean sqlLog = false;
    boolean configLog = false;
    boolean squelchTestOutput = false;
    for( int i = 0; i < args.length; ){
      String arg = args[i++];
      if(arg.startsWith("-")){
        arg = arg.replaceFirst("-+","");
        if(arg.equals("v")){
          doSomethingForDev = true;
          //listBoundMethods();
        }else if(arg.equals("t") || arg.equals("thread")){
          nThread = Integer.parseInt(args[i++]);
        }else if(arg.equals("r") || arg.equals("repeat")){
          nRepeat = Integer.parseInt(args[i++]);
        }else if(arg.equals("shuffle")){
          shuffle = true;
        }else if(arg.equals("list-tests")){
          ++listRunTests;
        }else if(arg.equals("fail")){
          forceFail = true;
        }else if(arg.equals("sqllog")){
          sqlLog = true;
        }else if(arg.equals("configlog")){
          configLog = true;
        }else if(arg.equals("naps")){
          takeNaps = true;
        }else if(arg.equals("q") || arg.equals("quiet")){
          squelchTestOutput = true;
        }else{
          throw new IllegalArgumentException("Unhandled flag:"+arg);
        }
      }
    }

    if( sqlLog ){
      if( capdb_compileoption_used("ENABLE_SQLLOG") ){
        final ConfigSqlLogCallback log = new ConfigSqlLogCallback() {
            @Override public void call(capdb db, String msg, int op){
              switch(op){
                case 0: outln("Opening db: ",db); break;
                case 1: outln("SQL ",db,": ",msg); break;
                case 2: outln("Closing db: ",db); break;
              }
            }
          };
        int rc = capdb_config( log );
        affirm( 0==rc );
        rc = capdb_config( (ConfigSqlLogCallback)null );
        affirm( 0==rc );
        rc = capdb_config( log );
        affirm( 0==rc );
      }else{
        outln("WARNING: -sqllog is not active because library was built ",
              "without CAPDB_ENABLE_SQLLOG.");
      }
    }
    if( configLog ){
      final ConfigLogCallback log = new ConfigLogCallback() {
          @Override public void call(int code, String msg){
            outln("ConfigLogCallback: ",ResultCode.getEntryForInt(code),": ", msg);
          }
        };
      int rc = capdb_config( log );
      affirm( 0==rc );
      rc = capdb_config( (ConfigLogCallback)null );
      affirm( 0==rc );
      rc = capdb_config( log );
      affirm( 0==rc );
    }

    quietMode = squelchTestOutput;
    outln("If you just saw warning messages regarding CallStaticObjectMethod, ",
          "you are very likely seeing the side effects of a known openjdk8 ",
          "bug. It is unsightly but does not affect the library.");

    {
      // Build list of tests to run from the methods named test*().
      testMethods = new ArrayList<>();
      int nSkipped = 0;
      for(final java.lang.reflect.Method m : Tester1.class.getDeclaredMethods()){
        final String name = m.getName();
        if( name.equals("testFail") ){
          if( forceFail ){
            testMethods.add(m);
          }
        }else if( m.isAnnotationPresent( RequiresJniNio.class )
                  && !capdb_jni_supports_nio() ){
          outln("Skipping test for lack of JNI java.nio.ByteBuffer support: ",
                name,"()\n");
          ++nSkipped;
        }else if( !m.isAnnotationPresent( ManualTest.class ) ){
          if( nThread>1 && m.isAnnotationPresent( SingleThreadOnly.class ) ){
            out("Skipping test in multi-thread mode: ",name,"()\n");
            ++nSkipped;
          }else if( name.startsWith("test") ){
            testMethods.add(m);
          }
        }
      }
    }

    final long timeStart = System.currentTimeMillis();
    int nLoop = 0;
    switch( capdb_threadsafe() ){ /* Sanity checking */
      case 0:
        affirm( CAPDB_ERROR==capdb_config( CAPDB_CONFIG_SINGLETHREAD ),
                "Could not switch to single-thread mode." );
        affirm( CAPDB_ERROR==capdb_config( CAPDB_CONFIG_MULTITHREAD ),
                "Could switch to multithread mode."  );
        affirm( CAPDB_ERROR==capdb_config( CAPDB_CONFIG_SERIALIZED ),
                "Could not switch to serialized threading mode."  );
        outln("This is a single-threaded build. Not using threads.");
        nThread = 1;
        break;
      case 1:
      case 2:
        affirm( 0==capdb_config( CAPDB_CONFIG_SINGLETHREAD ),
                "Could not switch to single-thread mode." );
        affirm( 0==capdb_config( CAPDB_CONFIG_MULTITHREAD ),
                "Could not switch to multithread mode."  );
        affirm( 0==capdb_config( CAPDB_CONFIG_SERIALIZED ),
                "Could not switch to serialized threading mode."  );
        break;
      default:
        affirm( false, "Unhandled CAPDB_THREADSAFE value." );
    }
    outln("libversion_number: ",
          capdb_libversion_number(),"\n",
          capdb_libversion(),"\n",CAPDB_SOURCE_ID,"\n",
          "CAPDB_THREADSAFE=",capdb_threadsafe());
    outln("JVM NIO support? ",capdb_jni_supports_nio() ? "YES" : "NO");
    final boolean showLoopCount = (nRepeat>1 && nThread>1);
    if( showLoopCount ){
      outln("Running ",nRepeat," loop(s) with ",nThread," thread(s) each.");
    }
    if( takeNaps ) outln("Napping between tests is enabled.");
    for( int n = 0; n < nRepeat; ++n ){
      ++nLoop;
      if( showLoopCount ) out((1==nLoop ? "" : " ")+nLoop);
      if( nThread<=1 ){
        new Tester1(0).runTests(false);
        continue;
      }
      Tester1.mtMode = true;
      final ExecutorService ex = Executors.newFixedThreadPool( nThread );
      for( int i = 0; i < nThread; ++i ){
        ex.submit( new Tester1(i), i );
      }
      ex.shutdown();
      try{
        ex.awaitTermination(nThread*200, java.util.concurrent.TimeUnit.MILLISECONDS);
        ex.shutdownNow();
      }catch (InterruptedException ie){
        ex.shutdownNow();
        Thread.currentThread().interrupt();
      }
      if( !listErrors.isEmpty() ){
        quietMode = false;
        outln("TEST ERRORS:");
        Exception err = null;
        for( Exception e : listErrors ){
          e.printStackTrace();
          if( null==err ) err = e;
        }
        if( null!=err ) throw err;
      }
    }
    if( showLoopCount ) outln();
    quietMode = false;

    final long timeEnd = System.currentTimeMillis();
    outln("Tests done. Metrics across ",nTestRuns," total iteration(s):");
    outln("\tAssertions checked: ",affirmCount);
    outln("\tDatabases opened: ",metrics.dbOpen);
    if( doSomethingForDev ){
      capdb_jni_internal_details();
    }
    affirm( 0==capdb_release_memory(1) );
    capdb_shutdown();
    int nMethods = 0;
    int nNatives = 0;
    final java.lang.reflect.Method[] declaredMethods =
      CApi.class.getDeclaredMethods();
    for(java.lang.reflect.Method m : declaredMethods){
      final int mod = m.getModifiers();
      if( 0!=(mod & java.lang.reflect.Modifier.STATIC) ){
        final String name = m.getName();
        if(name.startsWith("capdb_")){
          ++nMethods;
          if( 0!=(mod & java.lang.reflect.Modifier.NATIVE) ){
            ++nNatives;
          }
        }
      }
    }
    outln("\tCApi.capdb_*() methods: "+
          nMethods+" total, with "+
          nNatives+" native, "+
          (nMethods - nNatives)+" Java"
    );
    outln("\tTotal test time = "
          +(timeEnd - timeStart)+"ms");
  }
}
