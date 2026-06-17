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
import java.nio.charset.StandardCharsets;
import org.sqlite.jni.capi.CApi;
import org.sqlite.jni.capi.capdb;
import org.sqlite.jni.capi.capdb_stmt;
import org.sqlite.jni.capi.capdb_backup;
import org.sqlite.jni.capi.capdb_blob;
import org.sqlite.jni.capi.OutputPointer;

/**
   This class represents a database connection, analog to the C-side
   capdb class but with added argument validation, exceptions, and
   similar "smoothing of sharp edges" to make the API safe to use from
   Java. It also acts as a namespace for other types for which
   individual instances are tied to a specific database connection.
*/
public final class Sqlite implements AutoCloseable  {
  private capdb db;
  private static final boolean JNI_SUPPORTS_NIO =
    CApi.capdb_jni_supports_nio();

  // Result codes
  public static final int OK = CApi.CAPDB_OK;
  public static final int ERROR = CApi.CAPDB_ERROR;
  public static final int INTERNAL = CApi.CAPDB_INTERNAL;
  public static final int PERM = CApi.CAPDB_PERM;
  public static final int ABORT = CApi.CAPDB_ABORT;
  public static final int BUSY = CApi.CAPDB_BUSY;
  public static final int LOCKED = CApi.CAPDB_LOCKED;
  public static final int NOMEM = CApi.CAPDB_NOMEM;
  public static final int READONLY = CApi.CAPDB_READONLY;
  public static final int INTERRUPT = CApi.CAPDB_INTERRUPT;
  public static final int IOERR = CApi.CAPDB_IOERR;
  public static final int CORRUPT = CApi.CAPDB_CORRUPT;
  public static final int NOTFOUND = CApi.CAPDB_NOTFOUND;
  public static final int FULL = CApi.CAPDB_FULL;
  public static final int CANTOPEN = CApi.CAPDB_CANTOPEN;
  public static final int PROTOCOL = CApi.CAPDB_PROTOCOL;
  public static final int EMPTY = CApi.CAPDB_EMPTY;
  public static final int SCHEMA = CApi.CAPDB_SCHEMA;
  public static final int TOOBIG = CApi.CAPDB_TOOBIG;
  public static final int CONSTRAINT = CApi. CAPDB_CONSTRAINT;
  public static final int MISMATCH = CApi.CAPDB_MISMATCH;
  public static final int MISUSE = CApi.CAPDB_MISUSE;
  public static final int NOLFS = CApi.CAPDB_NOLFS;
  public static final int AUTH = CApi.CAPDB_AUTH;
  public static final int FORMAT = CApi.CAPDB_FORMAT;
  public static final int RANGE = CApi.CAPDB_RANGE;
  public static final int NOTADB = CApi.CAPDB_NOTADB;
  public static final int NOTICE = CApi.CAPDB_NOTICE;
  public static final int WARNING = CApi.CAPDB_WARNING;
  public static final int ROW = CApi.CAPDB_ROW;
  public static final int DONE = CApi.CAPDB_DONE;
  public static final int ERROR_MISSING_COLLSEQ = CApi.CAPDB_ERROR_MISSING_COLLSEQ;
  public static final int ERROR_RETRY = CApi.CAPDB_ERROR_RETRY;
  public static final int ERROR_SNAPSHOT = CApi.CAPDB_ERROR_SNAPSHOT;
  public static final int IOERR_READ = CApi.CAPDB_IOERR_READ;
  public static final int IOERR_SHORT_READ = CApi.CAPDB_IOERR_SHORT_READ;
  public static final int IOERR_WRITE = CApi.CAPDB_IOERR_WRITE;
  public static final int IOERR_FSYNC = CApi.CAPDB_IOERR_FSYNC;
  public static final int IOERR_DIR_FSYNC = CApi.CAPDB_IOERR_DIR_FSYNC;
  public static final int IOERR_TRUNCATE = CApi.CAPDB_IOERR_TRUNCATE;
  public static final int IOERR_FSTAT = CApi.CAPDB_IOERR_FSTAT;
  public static final int IOERR_UNLOCK = CApi.CAPDB_IOERR_UNLOCK;
  public static final int IOERR_RDLOCK = CApi.CAPDB_IOERR_RDLOCK;
  public static final int IOERR_DELETE = CApi.CAPDB_IOERR_DELETE;
  public static final int IOERR_BLOCKED = CApi.CAPDB_IOERR_BLOCKED;
  public static final int IOERR_NOMEM = CApi.CAPDB_IOERR_NOMEM;
  public static final int IOERR_ACCESS = CApi.CAPDB_IOERR_ACCESS;
  public static final int IOERR_CHECKRESERVEDLOCK = CApi.CAPDB_IOERR_CHECKRESERVEDLOCK;
  public static final int IOERR_LOCK = CApi.CAPDB_IOERR_LOCK;
  public static final int IOERR_CLOSE = CApi.CAPDB_IOERR_CLOSE;
  public static final int IOERR_DIR_CLOSE = CApi.CAPDB_IOERR_DIR_CLOSE;
  public static final int IOERR_SHMOPEN = CApi.CAPDB_IOERR_SHMOPEN;
  public static final int IOERR_SHMSIZE = CApi.CAPDB_IOERR_SHMSIZE;
  public static final int IOERR_SHMLOCK = CApi.CAPDB_IOERR_SHMLOCK;
  public static final int IOERR_SHMMAP = CApi.CAPDB_IOERR_SHMMAP;
  public static final int IOERR_SEEK = CApi.CAPDB_IOERR_SEEK;
  public static final int IOERR_DELETE_NOENT = CApi.CAPDB_IOERR_DELETE_NOENT;
  public static final int IOERR_MMAP = CApi.CAPDB_IOERR_MMAP;
  public static final int IOERR_GETTEMPPATH = CApi.CAPDB_IOERR_GETTEMPPATH;
  public static final int IOERR_CONVPATH = CApi.CAPDB_IOERR_CONVPATH;
  public static final int IOERR_VNODE = CApi.CAPDB_IOERR_VNODE;
  public static final int IOERR_AUTH = CApi.CAPDB_IOERR_AUTH;
  public static final int IOERR_BEGIN_ATOMIC = CApi.CAPDB_IOERR_BEGIN_ATOMIC;
  public static final int IOERR_COMMIT_ATOMIC = CApi.CAPDB_IOERR_COMMIT_ATOMIC;
  public static final int IOERR_ROLLBACK_ATOMIC = CApi.CAPDB_IOERR_ROLLBACK_ATOMIC;
  public static final int IOERR_DATA = CApi.CAPDB_IOERR_DATA;
  public static final int IOERR_CORRUPTFS = CApi.CAPDB_IOERR_CORRUPTFS;
  public static final int LOCKED_SHAREDCACHE = CApi.CAPDB_LOCKED_SHAREDCACHE;
  public static final int LOCKED_VTAB = CApi.CAPDB_LOCKED_VTAB;
  public static final int BUSY_RECOVERY = CApi.CAPDB_BUSY_RECOVERY;
  public static final int BUSY_SNAPSHOT = CApi.CAPDB_BUSY_SNAPSHOT;
  public static final int BUSY_TIMEOUT = CApi.CAPDB_BUSY_TIMEOUT;
  public static final int CANTOPEN_NOTEMPDIR = CApi.CAPDB_CANTOPEN_NOTEMPDIR;
  public static final int CANTOPEN_ISDIR = CApi.CAPDB_CANTOPEN_ISDIR;
  public static final int CANTOPEN_FULLPATH = CApi.CAPDB_CANTOPEN_FULLPATH;
  public static final int CANTOPEN_CONVPATH = CApi.CAPDB_CANTOPEN_CONVPATH;
  public static final int CANTOPEN_SYMLINK = CApi.CAPDB_CANTOPEN_SYMLINK;
  public static final int CORRUPT_VTAB = CApi.CAPDB_CORRUPT_VTAB;
  public static final int CORRUPT_SEQUENCE = CApi.CAPDB_CORRUPT_SEQUENCE;
  public static final int CORRUPT_INDEX = CApi.CAPDB_CORRUPT_INDEX;
  public static final int READONLY_RECOVERY = CApi.CAPDB_READONLY_RECOVERY;
  public static final int READONLY_CANTLOCK = CApi.CAPDB_READONLY_CANTLOCK;
  public static final int READONLY_ROLLBACK = CApi.CAPDB_READONLY_ROLLBACK;
  public static final int READONLY_DBMOVED = CApi.CAPDB_READONLY_DBMOVED;
  public static final int READONLY_CANTINIT = CApi.CAPDB_READONLY_CANTINIT;
  public static final int READONLY_DIRECTORY = CApi.CAPDB_READONLY_DIRECTORY;
  public static final int ABORT_ROLLBACK = CApi.CAPDB_ABORT_ROLLBACK;
  public static final int CONSTRAINT_CHECK = CApi.CAPDB_CONSTRAINT_CHECK;
  public static final int CONSTRAINT_COMMITHOOK = CApi.CAPDB_CONSTRAINT_COMMITHOOK;
  public static final int CONSTRAINT_FOREIGNKEY = CApi.CAPDB_CONSTRAINT_FOREIGNKEY;
  public static final int CONSTRAINT_FUNCTION = CApi.CAPDB_CONSTRAINT_FUNCTION;
  public static final int CONSTRAINT_NOTNULL = CApi.CAPDB_CONSTRAINT_NOTNULL;
  public static final int CONSTRAINT_PRIMARYKEY = CApi.CAPDB_CONSTRAINT_PRIMARYKEY;
  public static final int CONSTRAINT_TRIGGER = CApi.CAPDB_CONSTRAINT_TRIGGER;
  public static final int CONSTRAINT_UNIQUE = CApi.CAPDB_CONSTRAINT_UNIQUE;
  public static final int CONSTRAINT_VTAB = CApi.CAPDB_CONSTRAINT_VTAB;
  public static final int CONSTRAINT_ROWID = CApi.CAPDB_CONSTRAINT_ROWID;
  public static final int CONSTRAINT_PINNED = CApi.CAPDB_CONSTRAINT_PINNED;
  public static final int CONSTRAINT_DATATYPE = CApi.CAPDB_CONSTRAINT_DATATYPE;
  public static final int NOTICE_RECOVER_WAL = CApi.CAPDB_NOTICE_RECOVER_WAL;
  public static final int NOTICE_RECOVER_ROLLBACK = CApi.CAPDB_NOTICE_RECOVER_ROLLBACK;
  public static final int WARNING_AUTOINDEX = CApi.CAPDB_WARNING_AUTOINDEX;
  public static final int AUTH_USER = CApi.CAPDB_AUTH_USER;
  public static final int OK_LOAD_PERMANENTLY = CApi.CAPDB_OK_LOAD_PERMANENTLY;

  // capdb_open() flags
  public static final int OPEN_READWRITE = CApi.CAPDB_OPEN_READWRITE;
  public static final int OPEN_CREATE = CApi.CAPDB_OPEN_CREATE;
  public static final int OPEN_EXRESCODE = CApi.CAPDB_OPEN_EXRESCODE;

  // transaction state
  public static final int TXN_NONE = CApi.CAPDB_TXN_NONE;
  public static final int TXN_READ = CApi.CAPDB_TXN_READ;
  public static final int TXN_WRITE = CApi.CAPDB_TXN_WRITE;

  // capdb_status() ops
  public static final int STATUS_MEMORY_USED = CApi.CAPDB_STATUS_MEMORY_USED;
  public static final int STATUS_PAGECACHE_USED = CApi.CAPDB_STATUS_PAGECACHE_USED;
  public static final int STATUS_PAGECACHE_OVERFLOW = CApi.CAPDB_STATUS_PAGECACHE_OVERFLOW;
  public static final int STATUS_MALLOC_SIZE = CApi.CAPDB_STATUS_MALLOC_SIZE;
  public static final int STATUS_PARSER_STACK = CApi.CAPDB_STATUS_PARSER_STACK;
  public static final int STATUS_PAGECACHE_SIZE = CApi.CAPDB_STATUS_PAGECACHE_SIZE;
  public static final int STATUS_MALLOC_COUNT = CApi.CAPDB_STATUS_MALLOC_COUNT;

  // capdb_db_status() ops
  public static final int DBSTATUS_LOOKASIDE_USED = CApi.CAPDB_DBSTATUS_LOOKASIDE_USED;
  public static final int DBSTATUS_CACHE_USED = CApi.CAPDB_DBSTATUS_CACHE_USED;
  public static final int DBSTATUS_SCHEMA_USED = CApi.CAPDB_DBSTATUS_SCHEMA_USED;
  public static final int DBSTATUS_STMT_USED = CApi.CAPDB_DBSTATUS_STMT_USED;
  public static final int DBSTATUS_LOOKASIDE_HIT = CApi.CAPDB_DBSTATUS_LOOKASIDE_HIT;
  public static final int DBSTATUS_LOOKASIDE_MISS_SIZE = CApi.CAPDB_DBSTATUS_LOOKASIDE_MISS_SIZE;
  public static final int DBSTATUS_LOOKASIDE_MISS_FULL = CApi.CAPDB_DBSTATUS_LOOKASIDE_MISS_FULL;
  public static final int DBSTATUS_CACHE_HIT = CApi.CAPDB_DBSTATUS_CACHE_HIT;
  public static final int DBSTATUS_CACHE_MISS = CApi.CAPDB_DBSTATUS_CACHE_MISS;
  public static final int DBSTATUS_CACHE_WRITE = CApi.CAPDB_DBSTATUS_CACHE_WRITE;
  public static final int DBSTATUS_DEFERRED_FKS = CApi.CAPDB_DBSTATUS_DEFERRED_FKS;
  public static final int DBSTATUS_CACHE_USED_SHARED = CApi.CAPDB_DBSTATUS_CACHE_USED_SHARED;
  public static final int DBSTATUS_CACHE_SPILL = CApi.CAPDB_DBSTATUS_CACHE_SPILL;
  public static final int DBSTATUS_TEMPBUF_SPILL = CApi.CAPDB_DBSTATUS_TEMPBUF_SPILL;

  // Limits
  public static final int LIMIT_LENGTH = CApi.CAPDB_LIMIT_LENGTH;
  public static final int LIMIT_SQL_LENGTH = CApi.CAPDB_LIMIT_SQL_LENGTH;
  public static final int LIMIT_COLUMN = CApi.CAPDB_LIMIT_COLUMN;
  public static final int LIMIT_EXPR_DEPTH = CApi.CAPDB_LIMIT_EXPR_DEPTH;
  public static final int LIMIT_COMPOUND_SELECT = CApi.CAPDB_LIMIT_COMPOUND_SELECT;
  public static final int LIMIT_VDBE_OP = CApi.CAPDB_LIMIT_VDBE_OP;
  public static final int LIMIT_FUNCTION_ARG = CApi.CAPDB_LIMIT_FUNCTION_ARG;
  public static final int LIMIT_ATTACHED = CApi.CAPDB_LIMIT_ATTACHED;
  public static final int LIMIT_LIKE_PATTERN_LENGTH = CApi.CAPDB_LIMIT_LIKE_PATTERN_LENGTH;
  public static final int LIMIT_VARIABLE_NUMBER = CApi.CAPDB_LIMIT_VARIABLE_NUMBER;
  public static final int LIMIT_TRIGGER_DEPTH = CApi.CAPDB_LIMIT_TRIGGER_DEPTH;
  public static final int LIMIT_WORKER_THREADS = CApi.CAPDB_LIMIT_WORKER_THREADS;

  // capdb_prepare_v3() flags
  public static final int PREPARE_PERSISTENT = CApi.CAPDB_PREPARE_PERSISTENT;
  public static final int PREPARE_NO_VTAB = CApi.CAPDB_PREPARE_NO_VTAB;

  // capdb_trace_v2() flags
  public static final int TRACE_STMT = CApi.CAPDB_TRACE_STMT;
  public static final int TRACE_PROFILE = CApi.CAPDB_TRACE_PROFILE;
  public static final int TRACE_ROW = CApi.CAPDB_TRACE_ROW;
  public static final int TRACE_CLOSE = CApi.CAPDB_TRACE_CLOSE;
  public static final int TRACE_ALL = TRACE_STMT | TRACE_PROFILE | TRACE_ROW | TRACE_CLOSE;

  // capdb_db_config() ops
  public static final int DBCONFIG_ENABLE_FKEY = CApi.CAPDB_DBCONFIG_ENABLE_FKEY;
  public static final int DBCONFIG_ENABLE_TRIGGER = CApi.CAPDB_DBCONFIG_ENABLE_TRIGGER;
  public static final int DBCONFIG_ENABLE_FTS3_TOKENIZER = CApi.CAPDB_DBCONFIG_ENABLE_FTS3_TOKENIZER;
  public static final int DBCONFIG_ENABLE_LOAD_EXTENSION = CApi.CAPDB_DBCONFIG_ENABLE_LOAD_EXTENSION;
  public static final int DBCONFIG_NO_CKPT_ON_CLOSE = CApi.CAPDB_DBCONFIG_NO_CKPT_ON_CLOSE;
  public static final int DBCONFIG_ENABLE_QPSG = CApi.CAPDB_DBCONFIG_ENABLE_QPSG;
  public static final int DBCONFIG_TRIGGER_EQP = CApi.CAPDB_DBCONFIG_TRIGGER_EQP;
  public static final int DBCONFIG_RESET_DATABASE = CApi.CAPDB_DBCONFIG_RESET_DATABASE;
  public static final int DBCONFIG_DEFENSIVE = CApi.CAPDB_DBCONFIG_DEFENSIVE;
  public static final int DBCONFIG_WRITABLE_SCHEMA = CApi.CAPDB_DBCONFIG_WRITABLE_SCHEMA;
  public static final int DBCONFIG_LEGACY_ALTER_TABLE = CApi.CAPDB_DBCONFIG_LEGACY_ALTER_TABLE;
  public static final int DBCONFIG_DQS_DML = CApi.CAPDB_DBCONFIG_DQS_DML;
  public static final int DBCONFIG_DQS_DDL = CApi.CAPDB_DBCONFIG_DQS_DDL;
  public static final int DBCONFIG_ENABLE_VIEW = CApi.CAPDB_DBCONFIG_ENABLE_VIEW;
  public static final int DBCONFIG_LEGACY_FILE_FORMAT = CApi.CAPDB_DBCONFIG_LEGACY_FILE_FORMAT;
  public static final int DBCONFIG_TRUSTED_SCHEMA = CApi.CAPDB_DBCONFIG_TRUSTED_SCHEMA;
  public static final int DBCONFIG_STMT_SCANSTATUS = CApi.CAPDB_DBCONFIG_STMT_SCANSTATUS;
  public static final int DBCONFIG_REVERSE_SCANORDER = CApi.CAPDB_DBCONFIG_REVERSE_SCANORDER;

  // capdb_config() ops
  public static final int CONFIG_SINGLETHREAD = CApi.CAPDB_CONFIG_SINGLETHREAD;
  public static final int CONFIG_MULTITHREAD = CApi.CAPDB_CONFIG_MULTITHREAD;
  public static final int CONFIG_SERIALIZED = CApi.CAPDB_CONFIG_SERIALIZED;

  // Encodings
  public static final int UTF8 = CApi.CAPDB_UTF8;
  public static final int UTF16 = CApi.CAPDB_UTF16;
  public static final int UTF16LE = CApi.CAPDB_UTF16LE;
  public static final int UTF16BE = CApi.CAPDB_UTF16BE;
  /* We elide the UTF16_ALIGNED from this interface because it
     is irrelevant for the Java interface. */

  // SQL data type IDs
  public static final int INTEGER = CApi.CAPDB_INTEGER;
  public static final int FLOAT = CApi.CAPDB_FLOAT;
  public static final int TEXT = CApi.CAPDB_TEXT;
  public static final int BLOB = CApi.CAPDB_BLOB;
  public static final int NULL = CApi.CAPDB_NULL;

  // Authorizer codes.
  public static final int DENY = CApi.CAPDB_DENY;
  public static final int IGNORE = CApi.CAPDB_IGNORE;
  public static final int CREATE_INDEX = CApi.CAPDB_CREATE_INDEX;
  public static final int CREATE_TABLE = CApi.CAPDB_CREATE_TABLE;
  public static final int CREATE_TEMP_INDEX = CApi.CAPDB_CREATE_TEMP_INDEX;
  public static final int CREATE_TEMP_TABLE = CApi.CAPDB_CREATE_TEMP_TABLE;
  public static final int CREATE_TEMP_TRIGGER = CApi.CAPDB_CREATE_TEMP_TRIGGER;
  public static final int CREATE_TEMP_VIEW = CApi.CAPDB_CREATE_TEMP_VIEW;
  public static final int CREATE_TRIGGER = CApi.CAPDB_CREATE_TRIGGER;
  public static final int CREATE_VIEW = CApi.CAPDB_CREATE_VIEW;
  public static final int DELETE = CApi.CAPDB_DELETE;
  public static final int DROP_INDEX = CApi.CAPDB_DROP_INDEX;
  public static final int DROP_TABLE = CApi.CAPDB_DROP_TABLE;
  public static final int DROP_TEMP_INDEX = CApi.CAPDB_DROP_TEMP_INDEX;
  public static final int DROP_TEMP_TABLE = CApi.CAPDB_DROP_TEMP_TABLE;
  public static final int DROP_TEMP_TRIGGER = CApi.CAPDB_DROP_TEMP_TRIGGER;
  public static final int DROP_TEMP_VIEW = CApi.CAPDB_DROP_TEMP_VIEW;
  public static final int DROP_TRIGGER = CApi.CAPDB_DROP_TRIGGER;
  public static final int DROP_VIEW = CApi.CAPDB_DROP_VIEW;
  public static final int INSERT = CApi.CAPDB_INSERT;
  public static final int PRAGMA = CApi.CAPDB_PRAGMA;
  public static final int READ = CApi.CAPDB_READ;
  public static final int SELECT = CApi.CAPDB_SELECT;
  public static final int TRANSACTION = CApi.CAPDB_TRANSACTION;
  public static final int UPDATE = CApi.CAPDB_UPDATE;
  public static final int ATTACH = CApi.CAPDB_ATTACH;
  public static final int DETACH = CApi.CAPDB_DETACH;
  public static final int ALTER_TABLE = CApi.CAPDB_ALTER_TABLE;
  public static final int REINDEX = CApi.CAPDB_REINDEX;
  public static final int ANALYZE = CApi.CAPDB_ANALYZE;
  public static final int CREATE_VTABLE = CApi.CAPDB_CREATE_VTABLE;
  public static final int DROP_VTABLE = CApi.CAPDB_DROP_VTABLE;
  public static final int FUNCTION = CApi.CAPDB_FUNCTION;
  public static final int SAVEPOINT = CApi.CAPDB_SAVEPOINT;
  public static final int RECURSIVE = CApi.CAPDB_RECURSIVE;

  //! Used only by the open() factory functions.
  private Sqlite(capdb db){
    this.db = db;
  }

  /** Maps org.sqlite.jni.capi.capdb to Sqlite instances. */
  private static final java.util.Map<org.sqlite.jni.capi.capdb, Sqlite> nativeToWrapper
    = new java.util.HashMap<>();


  /**
     When any given thread is done using the SQLite library, calling
     this will free up any native-side resources which may be
     associated specifically with that thread. This is not strictly
     necessary, in particular in applications which only use SQLite
     from a single thread, but may help free some otherwise errant
     resources.

     Calling into SQLite from a given thread after this has been
     called in that thread is harmless. The library will simply start
     to re-cache certain state for that thread.

     Contrariwise, failing to call this will effectively leak a small
     amount of cached state for the thread, which may add up to
     significant amounts if the application uses SQLite from many
     threads.

     This must never be called while actively using SQLite from this
     thread, e.g. from within a query loop or a callback which is
     operating on behalf of the library.
  */
  static void uncacheThread(){
    CApi.capdb_java_uncache_thread();
  }

  /**
     Returns the Sqlite object associated with the given capdb
     object, or null if there is no such mapping.
  */
  static Sqlite fromNative(capdb low){
    synchronized(nativeToWrapper){
      return nativeToWrapper.get(low);
    }
  }

  /**
     Returns a newly-opened db connection or throws SqliteException if
     opening fails. All arguments are as documented for
     capdb_open_v2().

     Design question: do we want static factory functions or should
     this be reformulated as a constructor?
  */
  public static Sqlite open(String filename, int flags, String vfsName){
    final OutputPointer.capdb out = new OutputPointer.capdb();
    final int rc = CApi.capdb_open_v2(filename, out, flags, vfsName);
    final capdb n = out.take();
    if( 0!=rc ){
      if( null==n ) throw new SqliteException(rc);
      final SqliteException ex = new SqliteException(n);
      n.close();
      throw ex;
    }
    final Sqlite rv = new Sqlite(n);
    synchronized(nativeToWrapper){
      nativeToWrapper.put(n, rv);
    }
    runAutoExtensions(rv);
    return rv;
  }

  public static Sqlite open(String filename, int flags){
    return open(filename, flags, null);
  }

  public static Sqlite open(String filename){
    return open(filename, OPEN_READWRITE|OPEN_CREATE, null);
  }

  /**
     Opens a remote database via capdb:// URI (requires native build
     with CAPDB_ENABLE_NETWORK). Returns null on error.
  */
  private static native long connectRemoteNative(String uri);
  private static native void closeRemoteNative(long ptr);
  private static native int execRemoteNative(long ptr, String sql);

  public static RemoteConnection connectRemote(String uri){
    final long p = connectRemoteNative(uri);
    if( p==0 ) return null;
    return new RemoteConnection(p);
  }

  /** Remote SQL connection via capdb network protocol. */
  public static final class RemoteConnection implements AutoCloseable {
    private long ptr;
    RemoteConnection(long p){ ptr = p; }
    public int exec(String sql){ return execRemoteNative(ptr, sql); }
    @Override public void close(){
      if( ptr!=0 ){
        closeRemoteNative(ptr);
        ptr = 0;
      }
    }
  }

  public static String libVersion(){
    return CApi.capdb_libversion();
  }

  public static int libVersionNumber(){
    return CApi.capdb_libversion_number();
  }

  public static String libSourceId(){
    return CApi.capdb_sourceid();
  }

  /**
     Returns the value of the native library's build-time value of the
     CAPDB_THREADSAFE build option.
  */
  public static int libThreadsafe(){
    return CApi.capdb_threadsafe();
  }

  /**
     Analog to capdb_compileoption_get().
  */
  public static String compileOptionGet(int n){
    return CApi.capdb_compileoption_get(n);
  }

  /**
     Analog to capdb_compileoption_used().
  */
  public static boolean compileOptionUsed(String optName){
    return CApi.capdb_compileoption_used(optName);
  }

  private static final boolean hasNormalizeSql =
    compileOptionUsed("ENABLE_NORMALIZE");

  private static final boolean hasSqlLog =
    compileOptionUsed("ENABLE_SQLLOG");

  /**
     Throws UnsupportedOperationException if check is false.
     flag is expected to be the name of an CAPDB_ENABLE_...
     build flag.
  */
  private static void checkSupported(boolean check, String flag){
    if( !check ){
      throw new UnsupportedOperationException(
        "Library was built without "+flag
      );
    }
  }

  /**
     Analog to capdb_complete().
  */
  public static boolean isCompleteStatement(String sql){
    switch(CApi.capdb_complete(sql)){
      case 0: return false;
      case CApi.CAPDB_MISUSE:
        throw new IllegalArgumentException("Input may not be null.");
      case CApi.CAPDB_NOMEM:
        throw new OutOfMemoryError();
      default:
        return true;
    }
  }

  public static int keywordCount(){
    return CApi.capdb_keyword_count();
  }

  public static boolean keywordCheck(String word){
    return CApi.capdb_keyword_check(word);
  }

  public static String keywordName(int index){
    return CApi.capdb_keyword_name(index);
  }

  public static boolean strglob(String glob, String txt){
    return 0==CApi.capdb_strglob(glob, txt);
  }

  public static boolean strlike(String glob, String txt, char escChar){
    return 0==CApi.capdb_strlike(glob, txt, escChar);
  }

  /**
     Output object for use with status() and libStatus().
  */
  public static final class Status {
    /** The current value for the requested status() or libStatus() metric. */
    long current;
    /** The peak value for the requested status() or libStatus() metric. */
    long peak;
  }

  /**
     As per capdb_status64(), but returns its current and high-water
     results as a Status object. Throws if the first argument is
     not one of the STATUS_... constants.
  */
  public static Status libStatus(int op, boolean resetStats){
    org.sqlite.jni.capi.OutputPointer.Int64 pCurrent =
      new org.sqlite.jni.capi.OutputPointer.Int64();
    org.sqlite.jni.capi.OutputPointer.Int64 pHighwater =
      new org.sqlite.jni.capi.OutputPointer.Int64();
    checkRcStatic( CApi.capdb_status64(op, pCurrent, pHighwater, resetStats) );
    final Status s = new Status();
    s.current = pCurrent.value;
    s.peak = pHighwater.value;
    return s;
  }

  /**
     As per capdb_db_status(), but returns its current and
     high-water results as a Status object. Throws if the first
     argument is not one of the DBSTATUS_... constants or on any other
     misuse.
  */
  public Status status(int op, boolean resetStats){
    org.sqlite.jni.capi.OutputPointer.Int32 pCurrent =
      new org.sqlite.jni.capi.OutputPointer.Int32();
    org.sqlite.jni.capi.OutputPointer.Int32 pHighwater =
      new org.sqlite.jni.capi.OutputPointer.Int32();
    checkRc( CApi.capdb_db_status(thisDb(), op, pCurrent, pHighwater, resetStats) );
    final Status s = new Status();
    s.current = pCurrent.value;
    s.peak = pHighwater.value;
    return s;
  }

  @Override public void close(){
    if(null!=this.db){
      synchronized(nativeToWrapper){
        nativeToWrapper.remove(this.db);
      }
      this.db.close();
      this.db = null;
    }
  }

  /**
     Returns this object's underlying native db handle, or null if
     this instance has been closed. This is very specifically not
     public.
  */
  capdb nativeHandle(){ return this.db; }

  private capdb thisDb(){
    if( null==db || 0==db.getNativePointer() ){
      throw new IllegalArgumentException("This database instance is closed.");
    }
    return this.db;
  }

  // private byte[] stringToUtf8(String s){
  //   return s==null ? null : s.getBytes(StandardCharsets.UTF_8);
  // }

  /**
     If rc!=0, throws an SqliteException. If this db is currently
     opened and has non-0 capdb_errcode(), the error state is
     extracted from it, else only the string form of rc is used. It is
     the caller's responsibility to filter out non-error codes such as
     CAPDB_ROW and CAPDB_DONE before calling this.

     As a special case, if rc is CAPDB_NOMEM, an OutOfMemoryError is
     thrown.
  */
  private void checkRc(int rc){
    if( 0!=rc ){
      if( CApi.CAPDB_NOMEM==rc ){
        throw new OutOfMemoryError();
      }else if( null==db || 0==CApi.capdb_errcode(db) ){
        throw new SqliteException(rc);
      }else{
        throw new SqliteException(db);
      }
    }
  }

  /**
     Like checkRc() but behaves as if that function were
     called with a null db object.
  */
  private static void checkRcStatic(int rc){
    if( 0!=rc ){
      if( CApi.CAPDB_NOMEM==rc ){
        throw new OutOfMemoryError();
      }else{
        throw new SqliteException(rc);
      }
    }
  }

  /**
     Toggles the use of extended result codes on or off. By default
     they are turned off, but they can be enabled by default by
     including the OPEN_EXRESCODE flag when opening a database.

     Because this API reports db-side errors using exceptions,
     enabling this may change the values returned by
     SqliteException.errcode().
  */
  public void useExtendedResultCodes(boolean on){
    checkRc( CApi.capdb_extended_result_codes(thisDb(), on) );
  }

  /**
     Analog to capdb_prepare_v3(), this prepares the first SQL
     statement from the given input string and returns it as a
     Stmt. It throws an SqliteException if preparation fails or an
     IllegalArgumentException if the input is empty (e.g. contains
     only comments or whitespace).

     The first argument must be SQL input in UTF-8 encoding.

     prepFlags must be 0 or a bitmask of the PREPARE_... constants.

     For processing multiple statements from a single input, use
     prepareMulti().

     Design note: though the C-level API succeeds with a null
     statement object for empty inputs, that approach is cumbersome to
     use in higher-level APIs because every prepared statement has to
     be checked for null before using it.
  */
  public Stmt prepare(byte utf8Sql[], int prepFlags){
    final OutputPointer.capdb_stmt out = new OutputPointer.capdb_stmt();
    final int rc = CApi.capdb_prepare_v3(thisDb(), utf8Sql, prepFlags, out);
    checkRc(rc);
    final capdb_stmt q = out.take();
    if( null==q ){
      /* The C-level API treats input which is devoid of SQL
         statements (e.g. all comments or an empty string) as success
         but returns a NULL capdb_stmt object. In higher-level APIs,
         wrapping a "successful NULL" object that way is tedious to
         use because it forces clients and/or wrapper-level code to
         check for that unusual case. In practice, higher-level
         bindings are generally better-served by treating empty SQL
         input as an error. */
      throw new IllegalArgumentException("Input contains no SQL statements.");
    }
    return new Stmt(this, q);
  }

  /**
     Equivalent to prepare(X, prepFlags), where X is
     sql.getBytes(StandardCharsets.UTF_8).
  */
  public Stmt prepare(String sql, int prepFlags){
    return prepare( sql.getBytes(StandardCharsets.UTF_8), prepFlags );
  }

  /**
     Equivalent to prepare(sql, 0).
  */
  public Stmt prepare(String sql){
    return prepare(sql, 0);
  }


  /**
     Callback type for use with prepareMulti().
  */
  public interface PrepareMulti {
    /**
       Gets passed a Stmt which it may handle in arbitrary ways.
       Ownership of st is passed to this function. It must throw on
       error.
    */
    void call(Sqlite.Stmt st);
  }

  /**
     A PrepareMulti implementation which calls another PrepareMulti
     object and then finalizes its statement.
  */
  public static class PrepareMultiFinalize implements PrepareMulti {
    private final PrepareMulti pm;
    /**
       Proxies the given PrepareMulti via this object's call() method.
    */
    public PrepareMultiFinalize(PrepareMulti proxy){
      this.pm = proxy;
    }
    /**
       Passes st to the call() method of the object this one proxies,
       then finalizes st, propagating any exceptions from call() after
       finalizing st.
    */
    @Override public void call(Stmt st){
      try{ pm.call(st); }
      finally{ st.finalizeStmt(); }
    }
  }

  /**
     Equivalent to prepareMulti(sql,0,visitor).
  */
  public void prepareMulti(String sql, PrepareMulti visitor){
    prepareMulti( sql, 0, visitor );
  }

  /**
     Equivalent to prepareMulti(X,prepFlags,visitor), where X is
     sql.getBytes(StandardCharsets.UTF_8).
  */
  public void prepareMulti(String sql, int prepFlags, PrepareMulti visitor){
    prepareMulti(sql.getBytes(StandardCharsets.UTF_8), prepFlags, visitor);
  }

  /**
     A variant of prepare() which can handle multiple SQL statements
     in a single input string. For each statement in the given string,
     the statement is passed to visitor.call() a single time, passing
     ownership of the statement to that function. This function does
     not step() or close() statements - those operations are left to
     caller or the visitor function.

     Unlike prepare(), this function does not fail if the input
     contains only whitespace or SQL comments. In that case it is up
     to the caller to arrange for that to be an error (if desired).

     PrepareMultiFinalize offers a proxy which finalizes each
     statement after it is passed to another client-defined visitor.

     Be aware that certain legal SQL constructs may fail in the
     preparation phase, before the corresponding statement can be
     stepped. Most notably, authorizer checks which disallow access to
     something in a statement behave that way.
  */
  public void prepareMulti(byte sqlUtf8[], int prepFlags, PrepareMulti visitor){
    int pos = 0, n = 1;
    byte[] sqlChunk = sqlUtf8;
    final org.sqlite.jni.capi.OutputPointer.capdb_stmt outStmt =
      new org.sqlite.jni.capi.OutputPointer.capdb_stmt();
    final org.sqlite.jni.capi.OutputPointer.Int32 oTail =
      new org.sqlite.jni.capi.OutputPointer.Int32();
    while( pos < sqlChunk.length ){
      capdb_stmt stmt;
      if( pos>0 ){
        sqlChunk = java.util.Arrays.copyOfRange(sqlChunk, pos, sqlChunk.length);
      }
      if( 0==sqlChunk.length ) break;
      checkRc(
        CApi.capdb_prepare_v3(db, sqlChunk, prepFlags, outStmt, oTail)
      );
      pos = oTail.value;
      stmt = outStmt.take();
      if( null==stmt ){
        /* empty statement, e.g. only comments or whitespace, was parsed. */
        continue;
      }
      visitor.call(new Stmt(this, stmt));
    }
  }

  public void createFunction(String name, int nArg, int eTextRep, ScalarFunction f){
    int rc = CApi.capdb_create_function(thisDb(), name, nArg, eTextRep,
                                           new SqlFunction.ScalarAdapter(f));
    if( 0!=rc ) throw new SqliteException(db);
  }

  public void createFunction(String name, int nArg, ScalarFunction f){
    this.createFunction(name, nArg, CApi.CAPDB_UTF8, f);
  }

  public void createFunction(String name, int nArg, int eTextRep, AggregateFunction f){
    int rc = CApi.capdb_create_function(thisDb(), name, nArg, eTextRep,
                                           new SqlFunction.AggregateAdapter(f));
    if( 0!=rc ) throw new SqliteException(db);
  }

  public void createFunction(String name, int nArg, AggregateFunction f){
    this.createFunction(name, nArg, CApi.CAPDB_UTF8, f);
  }

  public void createFunction(String name, int nArg, int eTextRep, WindowFunction f){
    int rc = CApi.capdb_create_function(thisDb(), name, nArg, eTextRep,
                                          new SqlFunction.WindowAdapter(f));
    if( 0!=rc ) throw new SqliteException(db);
  }

  public void createFunction(String name, int nArg, WindowFunction f){
    this.createFunction(name, nArg, CApi.CAPDB_UTF8, f);
  }

  public long changes(){
    return CApi.capdb_changes64(thisDb());
  }

  public long totalChanges(){
    return CApi.capdb_total_changes64(thisDb());
  }

  public long lastInsertRowId(){
    return CApi.capdb_last_insert_rowid(thisDb());
  }

  public void setLastInsertRowId(long rowId){
    CApi.capdb_set_last_insert_rowid(thisDb(), rowId);
  }

  public void interrupt(){
    CApi.capdb_interrupt(thisDb());
  }

  public boolean isInterrupted(){
    return CApi.capdb_is_interrupted(thisDb());
  }

  public boolean isAutoCommit(){
    return CApi.capdb_get_autocommit(thisDb());
  }

  /**
     Analog to capdb_txn_state(). Returns one of TXN_NONE, TXN_READ,
     or TXN_WRITE to denote this database's current transaction state
     for the given schema name (or the most restrictive state of any
     schema if zSchema is null).
  */
  public int transactionState(String zSchema){
    return CApi.capdb_txn_state(thisDb(), zSchema);
  }

  /**
     Analog to capdb_db_name(). Returns null if passed an unknown
     index.
  */
  public String dbName(int dbNdx){
    return CApi.capdb_db_name(thisDb(), dbNdx);
  }

  /**
     Analog to capdb_db_filename(). Returns null if passed an
     unknown db name.
  */
  public String dbFileName(String dbName){
    return CApi.capdb_db_filename(thisDb(), dbName);
  }

  /**
     Analog to capdb_db_config() for the call forms which take one
     of the boolean-type db configuration flags (namely the
     DBCONFIG_... constants defined in this class). On success it
     returns the result of that underlying call. Throws on error.
  */
  public boolean dbConfig(int op, boolean on){
    org.sqlite.jni.capi.OutputPointer.Int32 pOut =
      new org.sqlite.jni.capi.OutputPointer.Int32();
    checkRc( CApi.capdb_db_config(thisDb(), op, on ? 1 : 0, pOut) );
    return pOut.get()!=0;
  }

  /**
     Analog to the variant of capdb_db_config() for configuring the
     CAPDB_DBCONFIG_MAINDBNAME option. Throws on error.
  */
  public void setMainDbName(String name){
    checkRc(
      CApi.capdb_db_config(thisDb(), CApi.CAPDB_DBCONFIG_MAINDBNAME,
                             name)
    );
  }

  /**
     Analog to capdb_db_readonly() but throws an SqliteException
     with result code CAPDB_NOTFOUND if given an unknown database
     name.
  */
  public boolean readOnly(String dbName){
    final int rc = CApi.capdb_db_readonly(thisDb(), dbName);
    if( 0==rc ) return false;
    else if( rc>0 ) return true;
    throw new SqliteException(CApi.CAPDB_NOTFOUND);
  }

  /**
     Analog to capdb_db_release_memory().
  */
  public void releaseMemory(){
    CApi.capdb_db_release_memory(thisDb());
  }

  /**
     Analog to capdb_release_memory().
  */
  public static int libReleaseMemory(int n){
    return CApi.capdb_release_memory(n);
  }

  /**
     Analog to capdb_limit(). limitId must be one of the
     LIMIT_... constants.

     Returns the old limit for the given option. If newLimit is
     negative, it returns the old limit without modifying the limit.

     If capdb_limit() returns a negative value, this function throws
     an SqliteException with the CAPDB_RANGE result code but no
     further error info (because that case does not qualify as a
     db-level error). Such errors may indicate an invalid argument
     value or an invalid range for newLimit (the underlying function
     does not differentiate between those).
  */
  public int limit(int limitId, int newLimit){
    final int rc = CApi.capdb_limit(thisDb(), limitId, newLimit);
    if( rc<0 ){
      throw new SqliteException(CApi.CAPDB_RANGE);
    }
    return rc;
  }

  /**
     Analog to capdb_errstr().
  */
  static String errstr(int resultCode){
    return CApi.capdb_errstr(resultCode);
  }

  /**
     A wrapper object for use with tableColumnMetadata().  They are
     created and populated only via that interface.
  */
  public final class TableColumnMetadata {
    Boolean pNotNull = null;
    Boolean pPrimaryKey = null;
    Boolean pAutoinc = null;
    String pzCollSeq = null;
    String pzDataType = null;

    private TableColumnMetadata(){}

    public String getDataType(){ return pzDataType; }
    public String getCollation(){ return pzCollSeq; }
    public boolean isNotNull(){ return pNotNull; }
    public boolean isPrimaryKey(){ return pPrimaryKey; }
    public boolean isAutoincrement(){ return pAutoinc; }
  }

  /**
     Returns data about a database, table, and (optionally) column
     (which may be null), as per capdb_table_column_metadata().
     Throws if passed invalid arguments, else returns the result as a
     new TableColumnMetadata object.
  */
  TableColumnMetadata tableColumnMetadata(
    String zDbName, String zTableName, String zColumnName
  ){
    org.sqlite.jni.capi.OutputPointer.String pzDataType
      = new org.sqlite.jni.capi.OutputPointer.String();
    org.sqlite.jni.capi.OutputPointer.String pzCollSeq
      = new org.sqlite.jni.capi.OutputPointer.String();
    org.sqlite.jni.capi.OutputPointer.Bool pNotNull
      = new org.sqlite.jni.capi.OutputPointer.Bool();
    org.sqlite.jni.capi.OutputPointer.Bool pPrimaryKey
      = new org.sqlite.jni.capi.OutputPointer.Bool();
    org.sqlite.jni.capi.OutputPointer.Bool pAutoinc
      = new org.sqlite.jni.capi.OutputPointer.Bool();
    final int rc = CApi.capdb_table_column_metadata(
      thisDb(), zDbName, zTableName, zColumnName,
      pzDataType, pzCollSeq, pNotNull, pPrimaryKey, pAutoinc
    );
    checkRc(rc);
    TableColumnMetadata rv = new TableColumnMetadata();
    rv.pzDataType = pzDataType.value;
    rv.pzCollSeq = pzCollSeq.value;
    rv.pNotNull = pNotNull.value;
    rv.pPrimaryKey = pPrimaryKey.value;
    rv.pAutoinc = pAutoinc.value;
    return rv;
  }

  public interface TraceCallback {
    /**
       Called by capdb for various tracing operations, as per
       capdb_trace_v2(). Note that this interface elides the 2nd
       argument to the native trace callback, as that role is better
       filled by instance-local state.

       <p>These callbacks may throw, in which case their exceptions are
       converted to C-level error information.

       <p>The 2nd argument to this function, if non-null, will be a an
       Sqlite or Sqlite.Stmt object, depending on the first argument
       (see below).

       <p>The final argument to this function is the "X" argument
       documented for capdb_trace() and capdb_trace_v2(). Its type
       depends on value of the first argument:

       <p>- CAPDB_TRACE_STMT: pNative is a Sqlite.Stmt. pX is a String
       containing the prepared SQL.

       <p>- CAPDB_TRACE_PROFILE: pNative is a capdb_stmt. pX is a Long
       holding an approximate number of nanoseconds the statement took
       to run.

       <p>- CAPDB_TRACE_ROW: pNative is a capdb_stmt. pX is null.

       <p>- CAPDB_TRACE_CLOSE: pNative is a capdb. pX is null.
    */
    void call(int traceFlag, Object pNative, Object pX);
  }

  /**
     Analog to capdb_trace_v2(). traceMask must be a mask of the
     TRACE_...  constants. Pass a null callback to remove tracing.

     Throws on error.
  */
  public void trace(int traceMask, TraceCallback callback){
    final Sqlite self = this;
    final org.sqlite.jni.capi.TraceV2Callback tc =
      (null==callback) ? null : new org.sqlite.jni.capi.TraceV2Callback(){
          @SuppressWarnings("unchecked")
          @Override public int call(int flag, Object pNative, Object pX){
            switch(flag){
              case TRACE_ROW:
              case TRACE_PROFILE:
              case TRACE_STMT:
                callback.call(flag, Sqlite.Stmt.fromNative((capdb_stmt)pNative), pX);
                break;
              case TRACE_CLOSE:
                callback.call(flag, self, pX);
                break;
            }
            return 0;
          }
        };
    checkRc( CApi.capdb_trace_v2(thisDb(), traceMask, tc) );
  }

  /**
     Corresponds to the capdb_stmt class. Use Sqlite.prepare() to
     create new instances.
  */
  public static final class Stmt implements AutoCloseable {
    private Sqlite _db;
    private capdb_stmt stmt;

    /** Only called by the prepare() factory functions. */
    Stmt(Sqlite db, capdb_stmt stmt){
      this._db = db;
      this.stmt = stmt;
      synchronized(nativeToWrapper){
        nativeToWrapper.put(this.stmt, this);
      }
    }

    capdb_stmt nativeHandle(){
      return stmt;
    }

    /** Maps org.sqlite.jni.capi.capdb_stmt to Stmt instances. */
    private static final java.util.Map<org.sqlite.jni.capi.capdb_stmt, Stmt> nativeToWrapper
      = new java.util.HashMap<>();

    /**
       Returns the Stmt object associated with the given capdb_stmt
       object, or null if there is no such mapping.
    */
    static Stmt fromNative(capdb_stmt low){
      synchronized(nativeToWrapper){
        return nativeToWrapper.get(low);
      }
    }

    /**
       If this statement is still opened, its low-level handle is
       returned, else an IllegalArgumentException is thrown.
    */
    private capdb_stmt thisStmt(){
      if( null==stmt || 0==stmt.getNativePointer() ){
        throw new IllegalArgumentException("This Stmt has been finalized.");
      }
      return stmt;
    }

    /** Throws if n is out of range of this statement's result column
        count. Intended to be used by the columnXyz() methods. */
    private capdb_stmt checkColIndex(int n){
      if(n<0 || n>=columnCount()){
        throw new IllegalArgumentException("Column index "+n+" is out of range.");
      }
      return thisStmt();
    }

    /**
       Corresponds to capdb_finalize(), but we cannot override the
       name finalize() here because this one requires a different
       signature. It does not throw on error here because "destructors
       do not throw." If it returns non-0, the object is still
       finalized, but the result code is an indication that something
       went wrong in a prior call into the statement's API, as
       documented for capdb_finalize().
    */
    public int finalizeStmt(){
      int rc = 0;
      if( null!=stmt ){
        synchronized(nativeToWrapper){
          nativeToWrapper.remove(this.stmt);
        }
        CApi.capdb_finalize(stmt);
        stmt = null;
        _db = null;
      }
      return rc;
    }

    @Override public void close(){
      finalizeStmt();
    }

    /**
       Throws if rc is any value other than 0, CAPDB_ROW, or
       CAPDB_DONE, else returns rc. Error state for the exception is
       extracted from this statement object (if it's opened) or the
       string form of rc.
    */
    private int checkRc(int rc){
      switch(rc){
        case 0:
        case CApi.CAPDB_ROW:
        case CApi.CAPDB_DONE: return rc;
        default:
          if( null==stmt ) throw new SqliteException(rc);
          else throw new SqliteException(this);
      }
    }

    /**
       Works like capdb_step() but returns true for CAPDB_ROW,
       false for CAPDB_DONE, and throws SqliteException for any other
       result.
    */
    public boolean step(){
      switch(checkRc(CApi.capdb_step(thisStmt()))){
        case CApi.CAPDB_ROW: return true;
        case CApi.CAPDB_DONE: return false;
        default:
          throw new IllegalStateException(
            "This \"cannot happen\": all possible result codes were checked already."
          );
      }
    }

    /**
       Works like capdb_step(), returning the same result codes as
       that function unless throwOnError is true, in which case it
       will throw an SqliteException for any result codes other than
       Sqlite.ROW or Sqlite.DONE.

       The utility of this overload over the no-argument one is the
       ability to handle BUSY and LOCKED errors more easily.
    */
    public int step(boolean throwOnError){
      final int rc = (null==stmt)
              ? Sqlite.MISUSE
              : CApi.capdb_step(stmt);
      return throwOnError ? checkRc(rc) : rc;
    }

    /**
       Returns the Sqlite which prepared this statement, or null if
       this statement has been finalized.
    */
    public Sqlite getDb(){ return this._db; }

    /**
       Works like capdb_reset() but throws on error.
    */
    public void reset(){
      checkRc(CApi.capdb_reset(thisStmt()));
    }

    public boolean isBusy(){
      return CApi.capdb_stmt_busy(thisStmt());
    }

    public boolean isReadOnly(){
      return CApi.capdb_stmt_readonly(thisStmt());
    }

    public String sql(){
      return CApi.capdb_sql(thisStmt());
    }

    public String expandedSql(){
      return CApi.capdb_expanded_sql(thisStmt());
    }

    /**
       Analog to capdb_stmt_explain() but throws if op is invalid.
    */
    public void explain(int op){
      checkRc(CApi.capdb_stmt_explain(thisStmt(), op));
    }

    /**
       Analog to capdb_stmt_isexplain().
    */
    public int isExplain(){
      return CApi.capdb_stmt_isexplain(thisStmt());
    }

    /**
       Analog to capdb_normalized_sql(), but throws
       UnsupportedOperationException if the library was built without
       the CAPDB_ENABLE_NORMALIZE flag.
    */
    public String normalizedSql(){
      Sqlite.checkSupported(hasNormalizeSql, "CAPDB_ENABLE_NORMALIZE");
      return CApi.capdb_normalized_sql(thisStmt());
    }

    public void clearBindings(){
      CApi.capdb_clear_bindings( thisStmt() );
    }
    public void bindInt(int ndx, int val){
      checkRc(CApi.capdb_bind_int(thisStmt(), ndx, val));
    }
    public void bindInt64(int ndx, long val){
      checkRc(CApi.capdb_bind_int64(thisStmt(), ndx, val));
    }
    public void bindDouble(int ndx, double val){
      checkRc(CApi.capdb_bind_double(thisStmt(), ndx, val));
    }
    public void bindObject(int ndx, Object o){
      checkRc(CApi.capdb_bind_java_object(thisStmt(), ndx, o));
    }
    public void bindNull(int ndx){
      checkRc(CApi.capdb_bind_null(thisStmt(), ndx));
    }
    public int bindParameterCount(){
      return CApi.capdb_bind_parameter_count(thisStmt());
    }
    public int bindParameterIndex(String paramName){
      return CApi.capdb_bind_parameter_index(thisStmt(), paramName);
    }
    public String bindParameterName(int ndx){
      return CApi.capdb_bind_parameter_name(thisStmt(), ndx);
    }
    public void bindText(int ndx, byte[] utf8){
      checkRc(CApi.capdb_bind_text(thisStmt(), ndx, utf8));
    }
    public void bindText(int ndx, String asUtf8){
      checkRc(CApi.capdb_bind_text(thisStmt(), ndx, asUtf8));
    }
    public void bindText16(int ndx, byte[] utf16){
      checkRc(CApi.capdb_bind_text16(thisStmt(), ndx, utf16));
    }
    public void bindText16(int ndx, String asUtf16){
      checkRc(CApi.capdb_bind_text16(thisStmt(), ndx, asUtf16));
    }
    public void bindZeroBlob(int ndx, int n){
      checkRc(CApi.capdb_bind_zeroblob(thisStmt(), ndx, n));
    }
    public void bindBlob(int ndx, byte[] bytes){
      checkRc(CApi.capdb_bind_blob(thisStmt(), ndx, bytes));
    }

    public byte[] columnBlob(int ndx){
      return CApi.capdb_column_blob( checkColIndex(ndx), ndx );
    }
    public byte[] columnText(int ndx){
      return CApi.capdb_column_text( checkColIndex(ndx), ndx );
    }
    public String columnText16(int ndx){
      return CApi.capdb_column_text16( checkColIndex(ndx), ndx );
    }
    public int columnBytes(int ndx){
      return CApi.capdb_column_bytes( checkColIndex(ndx), ndx );
    }
    public int columnBytes16(int ndx){
      return CApi.capdb_column_bytes16( checkColIndex(ndx), ndx );
    }
    public int columnInt(int ndx){
      return CApi.capdb_column_int( checkColIndex(ndx), ndx );
    }
    public long columnInt64(int ndx){
      return CApi.capdb_column_int64( checkColIndex(ndx), ndx );
    }
    public double columnDouble(int ndx){
      return CApi.capdb_column_double( checkColIndex(ndx), ndx );
    }
    public int columnType(int ndx){
      return CApi.capdb_column_type( checkColIndex(ndx), ndx );
    }
    public String columnDeclType(int ndx){
      return CApi.capdb_column_decltype( checkColIndex(ndx), ndx );
    }
    /**
       Analog to capdb_column_count() but throws if this statement
       has been finalized.
    */
    public int columnCount(){
      /* We cannot reliably cache the column count in a class
         member because an ALTER TABLE from a separate statement
         can invalidate that count and we have no way, short of
         installing a COMMIT handler or the like, of knowing when
         to re-read it. We cannot install such a handler without
         interfering with a client's ability to do so. */
      return CApi.capdb_column_count(thisStmt());
    }
    public int columnDataCount(){
      return CApi.capdb_data_count( thisStmt() );
    }
    public Object columnObject(int ndx){
      return CApi.capdb_column_java_object( checkColIndex(ndx), ndx );
    }
    public <T> T columnObject(int ndx, Class<T> type){
      return CApi.capdb_column_java_object( checkColIndex(ndx), ndx, type );
    }
    public String columnName(int ndx){
      return CApi.capdb_column_name( checkColIndex(ndx), ndx );
    }
    public String columnDatabaseName(int ndx){
      return CApi.capdb_column_database_name( checkColIndex(ndx), ndx );
    }
    public String columnOriginName(int ndx){
      return CApi.capdb_column_origin_name( checkColIndex(ndx), ndx );
    }
    public String columnTableName(int ndx){
      return CApi.capdb_column_table_name( checkColIndex(ndx), ndx );
    }
  } /* Stmt class */

  /**
     Interface for auto-extensions, as per the
     capdb_auto_extension() API.

     Design note: the chicken/egg timing of auto-extension execution
     requires that this feature be entirely re-implemented in Java
     because the C-level API has no access to the Sqlite type so
     cannot pass on an object of that type while the database is being
     opened.  One side effect of this reimplementation is that this
     class's list of auto-extensions is 100% independent of the
     C-level list so, e.g., clearAutoExtensions() will have no effect
     on auto-extensions added via the C-level API and databases opened
     from that level of API will not be passed to this level's
     AutoExtension instances.
  */
  public interface AutoExtension {
    public void call(Sqlite db);
  }

  private static final java.util.Set<AutoExtension> autoExtensions =
    new java.util.LinkedHashSet<>();

  /**
     Passes db to all auto-extensions. If any one of them throws,
     db.close() is called before the exception is propagated.
  */
  private static void runAutoExtensions(Sqlite db){
    AutoExtension list[];
    synchronized(autoExtensions){
      /* Avoid that modifications to the AutoExtension list from within
         auto-extensions affect this execution of this list. */
      list = autoExtensions.toArray(new AutoExtension[0]);
    }
    try {
      for( AutoExtension ax : list ) ax.call(db);
    }catch(Exception e){
      db.close();
      throw e;
    }
  }

  /**
     Analog to capdb_auto_extension(), adds the given object to the
     list of auto-extensions if it is not already in that list. The
     given object will be run as part of Sqlite.open(), and passed the
     being-opened database. If the extension throws then open() will
     fail.

     This API does not guaranty whether or not manipulations made to
     the auto-extension list from within auto-extension callbacks will
     affect the current traversal of the auto-extension list.  Whether
     or not they do is unspecified and subject to change between
     versions. e.g. if an AutoExtension calls addAutoExtension(),
     whether or not the new extension will be run on the being-opened
     database is undefined.

     Note that calling Sqlite.open() from an auto-extension will
     necessarily result in recursion loop and (eventually) a stack
     overflow.
  */
  public static void addAutoExtension( AutoExtension e ){
    if( null==e ){
      throw new IllegalArgumentException("AutoExtension may not be null.");
    }
    synchronized(autoExtensions){
      autoExtensions.add(e);
    }
  }

  /**
     Removes the given object from the auto-extension list if it is in
     that list, otherwise this has no side-effects beyond briefly
     locking that list.
  */
  public static void removeAutoExtension( AutoExtension e ){
    synchronized(autoExtensions){
      autoExtensions.remove(e);
    }
  }

  /**
     Removes all auto-extensions which were added via addAutoExtension().
  */
  public static void clearAutoExtensions(){
    synchronized(autoExtensions){
      autoExtensions.clear();
    }
  }

  /**
     Encapsulates state related to the capdb backup API. Use
     Sqlite.initBackup() to create new instances.
  */
  public static final class Backup implements AutoCloseable {
    private capdb_backup b;
    private Sqlite dbTo;
    private Sqlite dbFrom;

    Backup(Sqlite dbDest, String schemaDest,Sqlite dbSrc, String schemaSrc){
      this.dbTo = dbDest;
      this.dbFrom = dbSrc;
      b = CApi.capdb_backup_init(dbDest.nativeHandle(), schemaDest,
                                   dbSrc.nativeHandle(), schemaSrc);
      if(null==b) toss();
    }

    private void toss(){
      int rc = CApi.capdb_errcode(dbTo.nativeHandle());
      if(0!=rc) throw new SqliteException(dbTo);
      rc = CApi.capdb_errcode(dbFrom.nativeHandle());
      if(0!=rc) throw new SqliteException(dbFrom);
      throw new SqliteException(CApi.CAPDB_ERROR);
    }

    private capdb_backup getNative(){
      if( null==b ) throw new IllegalStateException("This Backup is already closed.");
      return b;
    }
    /**
       If this backup is still active, this completes the backup and
       frees its native resources, otherwise it this is a no-op.
    */
    public void finish(){
      if( null!=b ){
        CApi.capdb_backup_finish(b);
        b = null;
        dbTo = null;
        dbFrom = null;
      }
    }

    /** Equivalent to finish(). */
    @Override public void close(){
      this.finish();
    }

    /**
       Analog to capdb_backup_step(). Returns 0 if stepping succeeds
       or, Sqlite.DONE if the end is reached, Sqlite.BUSY if one of
       the databases is busy, Sqlite.LOCKED if one of the databases is
       locked, and throws for any other result code or if this object
       has been closed. Note that BUSY and LOCKED are not necessarily
       permanent errors, so do not trigger an exception.
    */
    public int step(int pageCount){
      final int rc = CApi.capdb_backup_step(getNative(), pageCount);
      switch(rc){
        case 0:
        case Sqlite.DONE:
        case Sqlite.BUSY:
        case Sqlite.LOCKED:
          return rc;
        default:
          toss();
          return CApi.CAPDB_ERROR/*not reached*/;
      }
    }

    /**
       Analog to capdb_backup_pagecount().
    */
    public int pageCount(){
      return CApi.capdb_backup_pagecount(getNative());
    }

    /**
       Analog to capdb_backup_remaining().
    */
    public int remaining(){
      return CApi.capdb_backup_remaining(getNative());
    }
  }

  /**
     Analog to capdb_backup_init(). If schemaSrc is null, "main" is
     assumed. Throws if either this db or dbSrc (the source db) are
     not opened, if either of schemaDest or schemaSrc are null, or if
     the underlying call to capdb_backup_init() fails.

     The returned object must eventually be cleaned up by either
     arranging for it to be auto-closed (e.g. using
     try-with-resources) or by calling its finish() method.
  */
  public Backup initBackup(String schemaDest, Sqlite dbSrc, String schemaSrc){
    thisDb();
    dbSrc.thisDb();
    if( null==schemaSrc || null==schemaDest ){
      throw new IllegalArgumentException(
        "Neither the source nor destination schema name may be null."
      );
    }
    return new Backup(this, schemaDest, dbSrc, schemaSrc);
  }


  /**
     Callback type for use with createCollation().
   */
  public interface Collation {
    /**
       Called by the SQLite core to compare inputs. Implementations
       must compare its two arguments using memcmp(3) semantics.

       Warning: the SQLite core has no mechanism for reporting errors
       from custom collations and its workflow does not accommodate
       propagation of exceptions from callbacks. Any exceptions thrown
       from collations will be silently suppressed and sorting results
       will be unpredictable.
    */
    int call(byte[] lhs, byte[] rhs);
  }

  /**
     Analog to capdb_create_collation().

     Throws if name is null or empty, c is null, or the encoding flag
     is invalid. The encoding must be one of the UTF8, UTF16, UTF16LE,
     or UTF16BE constants.
  */
  public void createCollation(String name, int encoding, Collation c){
    thisDb();
    if( null==name || name.isEmpty()){
      throw new IllegalArgumentException("Collation name may not be null or empty.");
    }
    if( null==c ){
      throw new IllegalArgumentException("Collation may not be null.");
    }
    switch(encoding){
      case UTF8:
      case UTF16:
      case UTF16LE:
      case UTF16BE:
        break;
      default:
        throw new IllegalArgumentException("Invalid Collation encoding.");
    }
    checkRc(
      CApi.capdb_create_collation(
        thisDb(), name, encoding, new org.sqlite.jni.capi.CollationCallback(){
            @Override public int call(byte[] lhs, byte[] rhs){
              try{return c.call(lhs, rhs);}
              catch(Exception e){return 0;}
            }
            @Override public void xDestroy(){}
          }
      )
    );
  }

  /**
     Callback for use with onCollationNeeded().
  */
  public interface CollationNeeded {
    /**
       Must behave as documented for the callback for
       capdb_collation_needed().

       Warning: the C API has no mechanism for reporting or
       propagating errors from this callback, so any exceptions it
       throws are suppressed.
    */
    void call(Sqlite db, int encoding, String collationName);
  }

  /**
     Sets up the given object to be called by the SQLite core when it
     encounters a collation name which it does not know. Pass a null
     object to disconnect the object from the core. This replaces any
     existing collation-needed loader, or is a no-op if the given
     object is already registered. Throws if registering the loader
     fails.
  */
  public void onCollationNeeded( CollationNeeded cn ){
    org.sqlite.jni.capi.CollationNeededCallback cnc = null;
    if( null!=cn ){
      cnc = new org.sqlite.jni.capi.CollationNeededCallback(){
          @Override public void call(capdb db, int encoding, String collationName){
            final Sqlite xdb = Sqlite.fromNative(db);
            if(null!=xdb) cn.call(xdb, encoding, collationName);
          }
        };
    }
    checkRc( CApi.capdb_collation_needed(thisDb(), cnc) );
  }

  /**
     Callback for use with busyHandler().
  */
  public interface BusyHandler {
    /**
       Must function as documented for the C-level
       capdb_busy_handler() callback argument, minus the (void*)
       argument the C-level function requires.

       If this function throws, it is translated to a database-level
       error.
    */
    int call(int n);
  }

  /**
     Analog to capdb_busy_timeout().
  */
  public void setBusyTimeout(int ms){
    checkRc(CApi.capdb_busy_timeout(thisDb(), ms));
  }

  /**
     Analog to capdb_busy_handler(). If b is null then any
     current handler is cleared.
  */
  public void setBusyHandler( BusyHandler b ){
    org.sqlite.jni.capi.BusyHandlerCallback bhc = null;
    if( null!=b ){
      /*bhc = new org.sqlite.jni.capi.BusyHandlerCallback(){
          @Override public int call(int n){
            return b.call(n);
          }
        };*/
      bhc = b::call;
    }
    checkRc( CApi.capdb_busy_handler(thisDb(), bhc) );
  }

  public interface CommitHook {
    /**
       Must behave as documented for the C-level capdb_commit_hook()
       callback. If it throws, the exception is translated into
       a db-level error.
    */
    int call();
  }

  /**
     A level of indirection to permit setCommitHook() to have similar
     semantics as the C API, returning the previous hook. The caveat
     is that if the low-level API is used to install a hook, it will
     have a different hook type than Sqlite.CommitHook so
     setCommitHook() will return null instead of that object.
  */
  private static class CommitHookProxy
    implements org.sqlite.jni.capi.CommitHookCallback {
    final CommitHook commitHook;
    CommitHookProxy(CommitHook ch){
      this.commitHook = ch;
    }
    @Override public int call(){
      return commitHook.call();
    }
  }

  /**
     Analog to capdb_commit_hook(). Returns the previous hook, if
     any (else null). Throws if this db is closed.

     Minor caveat: if a commit hook is set on this object's underlying
     db handle using the lower-level SQLite API, this function may
     return null when replacing it, despite there being a hook,
     because it will have a different callback type. So long as the
     handle is only manipulated via the high-level API, this caveat
     does not apply.
  */
  public CommitHook setCommitHook( CommitHook c ){
    CommitHookProxy chp = null;
    if( null!=c ){
      chp = new CommitHookProxy(c);
    }
    final org.sqlite.jni.capi.CommitHookCallback rv =
      CApi.capdb_commit_hook(thisDb(), chp);
    return (rv instanceof CommitHookProxy)
      ? ((CommitHookProxy)rv).commitHook
      : null;
  }


  public interface RollbackHook {
    /**
       Must behave as documented for the C-level capdb_rollback_hook()
       callback. If it throws, the exception is translated into
       a db-level error.
    */
    void call();
  }

  /**
     A level of indirection to permit setRollbackHook() to have similar
     semantics as the C API, returning the previous hook. The caveat
     is that if the low-level API is used to install a hook, it will
     have a different hook type than Sqlite.RollbackHook so
     setRollbackHook() will return null instead of that object.
  */
  private static class RollbackHookProxy
    implements org.sqlite.jni.capi.RollbackHookCallback {
    final RollbackHook rollbackHook;
    RollbackHookProxy(RollbackHook ch){
      this.rollbackHook = ch;
    }
    @Override public void call(){rollbackHook.call();}
  }

  /**
     Analog to capdb_rollback_hook(). Returns the previous hook, if
     any (else null). Throws if this db is closed.

     Minor caveat: if a rollback hook is set on this object's underlying
     db handle using the lower-level SQLite API, this function may
     return null when replacing it, despite there being a hook,
     because it will have a different callback type. So long as the
     handle is only manipulated via the high-level API, this caveat
     does not apply.
  */
  public RollbackHook setRollbackHook( RollbackHook c ){
    RollbackHookProxy chp = null;
    if( null!=c ){
      chp = new RollbackHookProxy(c);
    }
    final org.sqlite.jni.capi.RollbackHookCallback rv =
      CApi.capdb_rollback_hook(thisDb(), chp);
    return (rv instanceof RollbackHookProxy)
      ? ((RollbackHookProxy)rv).rollbackHook
      : null;
  }

  public interface UpdateHook {
    /**
       Must function as described for the C-level capdb_update_hook()
       callback.
    */
    void call(int opId, String dbName, String tableName, long rowId);
  }

  /**
     A level of indirection to permit setUpdateHook() to have similar
     semantics as the C API, returning the previous hook. The caveat
     is that if the low-level API is used to install a hook, it will
     have a different hook type than Sqlite.UpdateHook so
     setUpdateHook() will return null instead of that object.
  */
  private static class UpdateHookProxy
    implements org.sqlite.jni.capi.UpdateHookCallback {
    final UpdateHook updateHook;
    UpdateHookProxy(UpdateHook ch){
      this.updateHook = ch;
    }
    @Override public void call(int opId, String dbName, String tableName, long rowId){
      updateHook.call(opId, dbName, tableName, rowId);
    }
  }

  /**
     Analog to capdb_update_hook(). Returns the previous hook, if
     any (else null). Throws if this db is closed.

     Minor caveat: if a update hook is set on this object's underlying
     db handle using the lower-level SQLite API, this function may
     return null when replacing it, despite there being a hook,
     because it will have a different callback type. So long as the
     handle is only manipulated via the high-level API, this caveat
     does not apply.
  */
  public UpdateHook setUpdateHook( UpdateHook c ){
    UpdateHookProxy chp = null;
    if( null!=c ){
      chp = new UpdateHookProxy(c);
    }
    final org.sqlite.jni.capi.UpdateHookCallback rv =
      CApi.capdb_update_hook(thisDb(), chp);
    return (rv instanceof UpdateHookProxy)
      ? ((UpdateHookProxy)rv).updateHook
      : null;
  }


  /**
     Callback interface for use with setProgressHandler().
  */
  public interface ProgressHandler {
    /**
       Must behave as documented for the C-level capdb_progress_handler()
       callback. If it throws, the exception is translated into
       a db-level error.
    */
    int call();
  }

  /**
     Analog to capdb_progress_handler(), sets the current progress
     handler or clears it if p is null.

     Note that this API, in contrast to setUpdateHook(),
     setRollbackHook(), and setCommitHook(), cannot return the
     previous handler. That inconsistency is part of the lower-level C
     API.
  */
  public void setProgressHandler( int n, ProgressHandler p ){
    org.sqlite.jni.capi.ProgressHandlerCallback phc = null;
    if( null!=p ){
      /*phc = new org.sqlite.jni.capi.ProgressHandlerCallback(){
          @Override public int call(){ return p.call(); }
          };*/
      phc = p::call;
    }
    CApi.capdb_progress_handler( thisDb(), n, phc );
  }


  /**
     Callback for use with setAuthorizer().
  */
  public interface Authorizer {
    /**
       Must function as described for the C-level
       capdb_set_authorizer() callback. If it throws, the error is
       converted to a db-level error and the exception is suppressed.
    */
    int call(int opId, String s1, String s2, String s3, String s4);
  }

  /**
     Analog to capdb_set_authorizer(), this sets the current
     authorizer callback, or clears if it passed null.
  */
  public void setAuthorizer( Authorizer a ) {
    org.sqlite.jni.capi.AuthorizerCallback ac = null;
    if( null!=a ){
      /*ac = new org.sqlite.jni.capi.AuthorizerCallback(){
          @Override public int call(int opId, String s1, String s2, String s3, String s4){
            return a.call(opId, s1, s2, s3, s4);
          }
          };*/
      ac = a::call;
    }
    checkRc( CApi.capdb_set_authorizer( thisDb(), ac ) );
  }

  /**
     Object type for use with blobOpen()
  */
  public final class Blob implements AutoCloseable {
    private Sqlite db;
    private capdb_blob b;
    Blob(Sqlite db, capdb_blob b){
      this.db = db;
      this.b = b;
    }

    /**
       If this blob is still opened, its low-level handle is
       returned, else an IllegalArgumentException is thrown.
    */
    private capdb_blob thisBlob(){
      if( null==b || 0==b.getNativePointer() ){
        throw new IllegalArgumentException("This Blob has been finalized.");
      }
      return b;
    }

    /**
       Analog to capdb_blob_close().
    */
    @Override public void close(){
      if( null!=b ){
        CApi.capdb_blob_close(b);
        b = null;
        db = null;
      }
    }

    /**
       Throws if the JVM does not have JNI-level support for
       ByteBuffer.
    */
    private void checkNio(){
      if( !Sqlite.JNI_SUPPORTS_NIO ){
        throw new UnsupportedOperationException(
          "This JVM does not support JNI access to ByteBuffer."
        );
      }
    }
    /**
       Analog to capdb_blob_reopen() but throws on error.
    */
    public void reopen(long newRowId){
      db.checkRc( CApi.capdb_blob_reopen(thisBlob(), newRowId) );
    }

    /**
       Analog to capdb_blob_write() but throws on error.
    */
    public void write( byte[] bytes, int atOffset ){
      db.checkRc( CApi.capdb_blob_write(thisBlob(), bytes, atOffset) );
    }

    /**
       Analog to capdb_blob_read() but throws on error.
    */
    public void read( byte[] dest, int atOffset ){
      db.checkRc( CApi.capdb_blob_read(thisBlob(), dest, atOffset) );
    }

    /**
       Analog to capdb_blob_bytes().
    */
    public int bytes(){
      return CApi.capdb_blob_bytes(thisBlob());
    }
  }

  /**
     Analog to capdb_blob_open(). Returns a Blob object for the
     given database, table, column, and rowid. The blob is opened for
     read-write mode if writeable is true, else it is read-only.

     The returned object must eventually be freed, before this
     database is closed, by either arranging for it to be auto-closed
     or calling its close() method.

     Throws on error.
  */
  public Blob blobOpen(String dbName, String tableName, String columnName,
                       long iRow, boolean writeable){
    final OutputPointer.capdb_blob out = new OutputPointer.capdb_blob();
    checkRc(
      CApi.capdb_blob_open(thisDb(), dbName, tableName, columnName,
                             iRow, writeable ? 1 : 0, out)
    );
    return new Blob(this, out.take());
  }

  /**
     Callback for use with libConfigLog().
  */
  public interface ConfigLog {
    /**
     Must function as described for a C-level callback for
     capdb_config()'s CAPDB_CONFIG_LOG callback, with the slight
     signature change. Any exceptions thrown from this callback are
     necessarily suppressed.
    */
    void call(int errCode, String msg);
  }

  /**
     Analog to capdb_config() with the CAPDB_CONFIG_LOG option,
     this sets or (if log is null) clears the current logger.
  */
  public static void libConfigLog(ConfigLog log){
    final org.sqlite.jni.capi.ConfigLogCallback l =
      null==log
      ? null
      /*: new org.sqlite.jni.capi.ConfigLogCallback() {
          @Override public void call(int errCode, String msg){
            log.call(errCode, msg);
          }
          };*/
      : log::call;
      checkRcStatic(CApi.capdb_config(l));
  }

  /**
     Callback for use with libConfigSqlLog().
  */
  public interface ConfigSqlLog {
    /**
       Must function as described for a C-level callback for
       capdb_config()'s CAPDB_CONFIG_SQLLOG callback, with the
       slight signature change. Any exceptions thrown from this
       callback are necessarily suppressed.
     */
    void call(Sqlite db, String msg, int msgType);
  }

  /**
     Analog to capdb_config() with the CAPDB_CONFIG_SQLLOG option,
     this sets or (if log is null) clears the current logger.

     If SQLite is built without CAPDB_ENABLE_SQLLOG defined then this
     will throw an UnsupportedOperationException.
  */
  public static void libConfigSqlLog(ConfigSqlLog log){
    Sqlite.checkSupported(hasNormalizeSql, "CAPDB_ENABLE_SQLLOG");
    final org.sqlite.jni.capi.ConfigSqlLogCallback l =
      null==log
      ? null
      : new org.sqlite.jni.capi.ConfigSqlLogCallback() {
          @Override public void call(capdb db, String msg, int msgType){
            try{
              log.call(fromNative(db), msg, msgType);
            }catch(Exception e){
              /* Suppressed */
            }
          }
        };
      checkRcStatic(CApi.capdb_config(l));
  }

  /**
     Analog to the C-level capdb_config() with one of the
     CAPDB_CONFIG_... constants defined as CONFIG_... in this
     class. Throws on error, including passing of an unknown option or
     if a specified option is not supported by the underlying build of
     the SQLite library.
   */
  public static void libConfigOp( int op ){
    checkRcStatic(CApi.capdb_config(op));
  }

}
