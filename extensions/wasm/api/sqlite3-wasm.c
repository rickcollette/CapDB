/*
** This file requires access to capdb.c static state in order to
** implement certain WASM-specific features, and thus directly
** includes that file. Unlike the rest of capdb.c, this file
** requires compiling with -std=c99 (or equivalent, or a later C
** version) because it makes use of features not available in C89.
**
** At its simplest, to build capdb.wasm either place this file
** in the same directory as capdb.c/h before compilation or use the
** -I/path flag to tell the compiler where to find both of those
** files, then compile this file. For example:
**
** emcc -o capdb.wasm ... -I/path/to/capdb-c-and-h capdb-wasm.c
*/
#define CAPDB_WASM
#ifdef CAPDB_WASM_ENABLE_C_TESTS
#  undef CAPDB_WASM_ENABLE_C_TESTS
#  define CAPDB_WASM_ENABLE_C_TESTS 1
/*
** Code blocked off by CAPDB_WASM_ENABLE_C_TESTS is intended solely
** for use in unit/regression testing. They may be safely omitted from
** client-side builds. The main unit test script, tester1.js, will
** skip related tests if it doesn't find the corresponding functions
** in the WASM exports.
*/
#else
#  define CAPDB_WASM_ENABLE_C_TESTS 0
#endif

/*
** Threading and file locking: JS is single-threaded. Each Worker
** thread is a separate instance of the JS engine so can never access
** the same db handle as another thread, thus multi-threading support
** is unnecessary in the library. Because the filesystems are virtual
** and local to a given wasm runtime instance, two Workers can never
** access the same db file at once, with the exception of OPFS.
**
** Summary: except for the case of OPFS, which supports locking using
** its own API, threading and file locking support are unnecessary in
** the wasm build.
*/

/*
** Undefine any CAPDB_... config flags which we specifically do not
** want defined. Please keep these alphabetized.
*/
#undef CAPDB_OMIT_DESERIALIZE
#undef CAPDB_OMIT_MEMORYDB

/*
** Define any CAPDB_... config defaults we want if they aren't
** overridden by the builder. Please keep these alphabetized.
*/

/**********************************************************************/
/* CAPDB_D... */
#ifndef CAPDB_DEFAULT_CACHE_SIZE
/*
** The OPFS impls benefit tremendously from an increased cache size
** when working on large workloads, e.g. speedtest1 --size 50 or
** higher. On smaller workloads, e.g. speedtest1 --size 25, they
** clearly benefit from having 4mb of cache, but not as much as a
** larger cache benefits the larger workloads. Speed differences
** between 2x and nearly 3x have been measured with ample page cache.
*/
# define CAPDB_DEFAULT_CACHE_SIZE -16384
#endif
#if !defined(CAPDB_DEFAULT_PAGE_SIZE)
/*
** OPFS performance is improved by approx. 12% with a page size of 8kb
** instead of 4kb. Performance with 16kb is equivalent to 8kb.
**
** Performance difference of kvvfs with a page size of 8kb compared to
** 4kb, as measured by speedtest1 --size 4, is indeterminate:
** measurements are all over the place either way and not
** significantly different.
*/
# define CAPDB_DEFAULT_PAGE_SIZE 8192
#endif
#ifndef CAPDB_DEFAULT_UNIX_VFS
# define CAPDB_DEFAULT_UNIX_VFS "unix-none"
#endif
#undef CAPDB_DQS
#define CAPDB_DQS 0

/**********************************************************************/
/* CAPDB_ENABLE_... */
/*
** Unconditionally enable API_ARMOR in the WASM build. It ensures that
** public APIs behave predictable in the face of passing illegal NULLs
** or ranges which might otherwise invoke undefined behavior.
*/
#undef CAPDB_ENABLE_API_ARMOR
#define CAPDB_ENABLE_API_ARMOR 1

/**********************************************************************/
/* CAPDB_EXPERIMENTAL_PRAGMA_20251114 */
/*
** See:
** https://sqlite.org/src/info/e2b3f1a9480a9be3
** https://github.com/rhashimoto/wa-sqlite/discussions/301
**
** It is enabled here for the sake of VFS experimentors.
*/
#undef CAPDB_EXPERIMENTAL_PRAGMA_20251114
#define CAPDB_EXPERIMENTAL_PRAGMA_20251114

/**********************************************************************/
/* CAPDB_O... */
#undef CAPDB_OMIT_DEPRECATED
#define CAPDB_OMIT_DEPRECATED 1
#undef CAPDB_OMIT_LOAD_EXTENSION
#define CAPDB_OMIT_LOAD_EXTENSION 1
#undef CAPDB_OMIT_SHARED_CACHE
#define CAPDB_OMIT_SHARED_CACHE 1
#undef CAPDB_OMIT_UTF16
#define CAPDB_OMIT_UTF16 1
#undef CAPDB_OS_KV_OPTIONAL
#define CAPDB_OS_KV_OPTIONAL 1

/**********************************************************************/
/* CAPDB_S... */
#ifndef CAPDB_STRICT_SUBTYPE
# define CAPDB_STRICT_SUBTYPE 1
#endif

/**********************************************************************/
/* CAPDB_T... */
#ifndef CAPDB_TEMP_STORE
# define CAPDB_TEMP_STORE 2
#endif
#ifndef CAPDB_THREADSAFE
# define CAPDB_THREADSAFE 0
#endif

/**********************************************************************/
/* CAPDB_USE_... */
#ifndef CAPDB_USE_URI
#  define CAPDB_USE_URI 1
#endif

#ifdef CAPDB_WASM_EXTRA_INIT
/* CAPDB_EXTRA_INIT vs CAPDB_EXTRA_INIT_MUTEXED:
** see https://sqlite.org/forum/forumpost/14183b98fc0b1dea */
#  define CAPDB_EXTRA_INIT_MUTEXED capdb_wasm_extra_init
#endif

/*
** If CAPDB_WASM_BARE_BONES is defined, undefine most of the ENABLE
** macros. This will, when using the canonical makefile, also elide
** any C functions from the WASM exports: see
** ./EXPORTED_FUNCTIONS.c-pp.
*/
#ifdef CAPDB_WASM_BARE_BONES
#  undef  CAPDB_ENABLE_COLUMN_METADATA
#  undef  CAPDB_ENABLE_DBPAGE_VTAB
#  undef  CAPDB_ENABLE_DBSTAT_VTAB
#  undef  CAPDB_ENABLE_EXPLAIN_COMMENTS
#  undef  CAPDB_ENABLE_FTS5
#  undef  CAPDB_ENABLE_OFFSET_SQL_FUNC
#  undef  CAPDB_ENABLE_PERCENTILE
#  undef  CAPDB_ENABLE_PREUPDATE_HOOK
#  undef  CAPDB_ENABLE_RTREE
#  undef  CAPDB_ENABLE_SESSION
#  undef  CAPDB_ENABLE_STMTVTAB
#  undef  CAPDB_OMIT_AUTHORIZATION
#  define CAPDB_OMIT_AUTHORIZATION
#  undef  CAPDB_OMIT_GET_TABLE
#  define CAPDB_OMIT_GET_TABLE
#  undef  CAPDB_OMIT_INCRBLOB
#  define CAPDB_OMIT_INCRBLOB
#  undef  CAPDB_OMIT_INTROSPECTION_PRAGMAS
#  define CAPDB_OMIT_INTROSPECTION_PRAGMAS
#  undef  CAPDB_OMIT_JSON
#  define CAPDB_OMIT_JSON
#  undef  CAPDB_OMIT_PROGRESS_CALLBACK
#  define CAPDB_OMIT_PROGRESS_CALLBACK
#  undef  CAPDB_OMIT_WAL
#  define CAPDB_OMIT_WAL
/*
  The following OMITs do not work with the standard amalgamation, so
  require a custom build:

  fossil clean -x
  ./configure
  OPTS='...' make -e capdb

  where ... has a -D... for each of the following OMIT flags:

#  undef CAPDB_OMIT_EXPLAIN
#  define CAPDB_OMIT_EXPLAIN

#  undef CAPDB_OMIT_TRIGGER
#  define CAPDB_OMIT_TRIGGER

#  undef CAPDB_OMIT_VIRTUALTABLE
#  define CAPDB_OMIT_VIRTUALTABLE

#  undef CAPDB_OMIT_WINDOWFUNC
#  define CAPDB_OMIT_WINDOWFUNC

  As of this writing (2024-07-25), such a build fails in various ways
  for as-yet-unknown reasons.
*/
#endif

#if !defined(CAPDB_OMIT_VIRTUALTABLE) && !defined(CAPDB_WASM_BARE_BONES)
#  define CAPDB_WASM_HAS_VTAB 1
#else
#  define CAPDB_WASM_HAS_VTAB 0
#endif

#include <assert.h>

/*
** CAPDB_WASM_EXPORT is functionally identical to EMSCRIPTEN_KEEPALIVE
** but is not Emscripten-specific. It explicitly marks functions for
** export into the target wasm file without requiring explicit listing
** of those functions in Emscripten's -sEXPORTED_FUNCTIONS=... list
** (or equivalent in other build platforms). Any function with neither
** this attribute nor which is listed as an explicit export will not
** be exported from the wasm file (but may still be used internally
** within the wasm file).
**
** The functions in this file (capdb-wasm.c) which require exporting
** are marked with this flag. They may also be added to any explicit
** build-time export list but need not be. All of these APIs are
** intended for use only within the project's own JS/WASM code, and
** not by client code, so an argument can be made for reducing their
** visibility by not including them in any build-time export lists.
**
** 2025-12-01: for use in non-Emscripten builds, we need a more
** invasive macro which explicitly names the export:
** CAPDB_WASM_EXPORT2.
*/
#define CAPDB_WASM_EXPORT __attribute__((used,visibility("default")))
#define CAPDB_WASM_EXPORT_NAMED(X) __attribute__((export_name(#X),used,visibility("default")))
#define CAPDB_WASM_EXPORT2(RETTYPE,NAME,SIG) CAPDB_WASM_EXPORT_NAMED(NAME) RETTYPE NAME SIG

#if 1
/** Increase the kvvfs key size limit from 32. */
#define KVRECORD_KEY_SZ 128
#endif

/*
** Which capdb.c we're using needs to be configurable to enable
** building against a custom copy, e.g. the SEE variant. We #include
** the .c file, rather than the header, so that the WASM extensions
** have access to private API internals (namely for kvvfs and
** SQLTester pieces).
**
** The caveat here is that custom variants need to account for
** exporting any necessary symbols (e.g. capdb_activate_see()).  We
** cannot export them from here using CAPDB_WASM_EXPORT because that
** attribute (apparently) has to be part of the function definition.
*/
#ifndef CAPDB_C
# define CAPDB_C capdb.c /* yes, .c instead of .h. */
#endif
#define INC__STRINGIFY_(f) #f
#define INC__STRINGIFY(f) INC__STRINGIFY_(f)
#include INC__STRINGIFY(CAPDB_C)
#undef INC__STRINGIFY_
#undef INC__STRINGIFY
#undef CAPDB_C

/*
** State for the "pseudo-stack" allocator implemented in
** capdb__wasm_pstack_xyz(). In order to avoid colliding with
** Emscripten-controlled stack space, it carves out a bit of stack
** memory to use for that purpose. This memory ends up in the
** WASM-managed memory, such that routines which manipulate the wasm
** heap can also be used to manipulate this memory.
**
** This particular allocator is intended for small allocations such as
** storage for output pointers. We cannot reasonably size it large
** enough for general-purpose string conversions because some of our
** tests use input files (strings) of 16MB+.
*/
static unsigned char PStack_mem[
  1024 * 4 /* API docs guaranty at least 2kb and it's been set at 4kb
              since it was introduced. */
] = {0};
static struct {
  unsigned const char * const pBegin;/* Start (inclusive) of memory */
  unsigned const char * const pEnd;  /* One-after-the-end of memory */
  unsigned char * pPos;              /* Current stack pointer */
} PStack = {
  &PStack_mem[0],
  &PStack_mem[0] + sizeof(PStack_mem),
  &PStack_mem[0] + sizeof(PStack_mem)
};
/*
** Returns the current pstack position.
*/
CAPDB_WASM_EXPORT void * capdb__wasm_pstack_ptr(void){
  return PStack.pPos;
}
/*
** Sets the pstack position pointer to p. Results are undefined if the
** given value did not come from capdb__wasm_pstack_ptr().
*/
CAPDB_WASM_EXPORT void capdb__wasm_pstack_restore(unsigned char * p){
  assert(p>=PStack.pBegin && p<=PStack.pEnd && p>=PStack.pPos);
  assert(0==((unsigned long long)p & 0x7) /* 8-byte aligned */);
  if(p>=PStack.pBegin && p<=PStack.pEnd /*&& p>=PStack.pPos*/){
    PStack.pPos = p;
  }
}
/*
** Allocate and zero out n bytes from the pstack. Returns a pointer to
** the memory on success, 0 on error (including a negative n value). n
** is always adjusted to be a multiple of 8 and returned memory is
** always zeroed out before returning (because this keeps the client
** JS code from having to do so, and most uses of the pstack call for
** doing so).
*/
CAPDB_WASM_EXPORT2(void *,capdb__wasm_pstack_alloc,(int n)){
  if( n<=0 ) return 0;
  n = (n + 7) & ~7 /* align to 8-byte boundary */;
  if( PStack.pBegin + n > PStack.pPos /*not enough space left*/
      || PStack.pBegin + n <= PStack.pBegin /*overflow*/ ) return 0;
  memset((PStack.pPos = PStack.pPos - n), 0, (unsigned int)n);
  return PStack.pPos;
}
/*
** Return the number of bytes left which can be
** capdb__wasm_pstack_alloc()'d.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_pstack_remaining,(void)){
  assert(PStack.pPos >= PStack.pBegin);
  assert(PStack.pPos <= PStack.pEnd);
  return (int)(PStack.pPos - PStack.pBegin);
}

/*
** Return the total number of bytes available in the pstack, including
** any space which is currently allocated. This value is a
** compile-time constant.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_pstack_quota,(void)){
  return (int)(PStack.pEnd - PStack.pBegin);
}

#if CAPDB_WASM_ENABLE_C_TESTS
struct WasmTestStruct {
  int v4;
  void * ppV;
  const char * cstr;
  int64_t v8;
  void (*xFunc)(void*);
};
typedef struct WasmTestStruct WasmTestStruct;
CAPDB_WASM_EXPORT2(void,capdb__wasm_test_struct,(WasmTestStruct * s)){
  if(s){
    if( 0 ){
      /* Do not be alarmed by the small (and odd) pointer values.
         Function pointers in WASM are their index into the
         indirect function table, not their address. */
      fprintf(stderr,"%s:%s()@%p s=@%p xFunc=@%p\n",
              __FILE__, __func__,
              (void*)capdb__wasm_test_struct,
              s, (void*)s->xFunc);
    }
    s->v4 *= 2;
    s->v8 = s->v4 * 2;
    s->ppV = s;
    s->cstr = __FILE__;
    if(s->xFunc) s->xFunc(s);
  }
}
#endif /* CAPDB_WASM_ENABLE_C_TESTS */

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings. Unlike the
** rest of the capdb API, this part requires C99 for snprintf() and
** variadic macros.
**
** Returns a string containing a JSON-format "enum" of C-level
** constants and struct-related metadata intended to be imported into
** the JS environment. The JSON is initialized the first time this
** function is called and that result is reused for all future calls.
**
** If this function returns NULL then it means that the internal
** buffer is not large enough for the generated JSON and needs to be
** increased. In debug builds that will trigger an assert().
**
** 2025-09-19: for reasons entirely not understood, building with emcc
** -sMEMORY64=2 causes this function to fail (return 0). -sMEMORY64=1
** fails to compile with "tables may not be 64-bit" but does not tell
** us where it's happening.
*/
CAPDB_WASM_EXPORT2(const char *,capdb__wasm_enum_json,(void)){
  static char aBuffer[1024 * 20] =
    {0} /* where the JSON goes. 2025-09-19: output size=19295, but
           that can vary slightly from build to build, so a little
           leeway is needed here. */;
  int n = 0, nChildren = 0, nStruct = 0
    /* output counters for figuring out where commas go */;
  char * zPos = &aBuffer[1] /* skip first byte for now to help protect
                            ** against a small race condition */;
  char const * const zEnd = &aBuffer[0] + sizeof(aBuffer) /* one-past-the-end */;
  if(aBuffer[0]) return aBuffer;
  /* Leave aBuffer[0] at 0 until the end to help guard against a tiny
  ** race condition. If this is called twice concurrently, they might
  ** end up both writing to aBuffer, but they'll both write the same
  ** thing, so that's okay. If we set byte 0 up front then the 2nd
  ** instance might return and use the string before the 1st instance
  ** is done filling it. */

/* Core output macros... */
#define lenCheck assert(zPos < zEnd - 128 \
  && "capdb__wasm_enum_json() buffer is too small."); \
  if( zPos >= zEnd - 128 ) return 0
#define outf(format,...) \
  zPos += snprintf(zPos, ((size_t)(zEnd - zPos)), format, __VA_ARGS__); \
  lenCheck
#define out(TXT) outf("%s",TXT)
#define CloseBrace(LEVEL) \
  assert(LEVEL<5); memset(zPos, '}', LEVEL); zPos+=LEVEL; lenCheck

/* Macros for emitting maps of integer- and string-type macros to
** their values. */
#define DefGroup(KEY) n = 0; \
  outf("%s\"" #KEY "\": {",(nChildren++ ? "," : ""));
#define DefInt(KEY)                                     \
  outf("%s\"%s\": %d", (n++ ? ", " : ""), #KEY, (int)KEY)
#define DefStr(KEY)                                     \
  outf("%s\"%s\": \"%s\"", (n++ ? ", " : ""), #KEY, KEY)
#define _DefGroup CloseBrace(1)

  /* The following groups are sorted alphabetic by group name. */
  DefGroup(access){
    DefInt(CAPDB_ACCESS_EXISTS);
    DefInt(CAPDB_ACCESS_READWRITE);
    DefInt(CAPDB_ACCESS_READ)/*docs say this is unused*/;
  } _DefGroup;

  DefGroup(authorizer){
    DefInt(CAPDB_DENY);
    DefInt(CAPDB_IGNORE);
    DefInt(CAPDB_CREATE_INDEX);
    DefInt(CAPDB_CREATE_TABLE);
    DefInt(CAPDB_CREATE_TEMP_INDEX);
    DefInt(CAPDB_CREATE_TEMP_TABLE);
    DefInt(CAPDB_CREATE_TEMP_TRIGGER);
    DefInt(CAPDB_CREATE_TEMP_VIEW);
    DefInt(CAPDB_CREATE_TRIGGER);
    DefInt(CAPDB_CREATE_VIEW);
    DefInt(CAPDB_DELETE);
    DefInt(CAPDB_DROP_INDEX);
    DefInt(CAPDB_DROP_TABLE);
    DefInt(CAPDB_DROP_TEMP_INDEX);
    DefInt(CAPDB_DROP_TEMP_TABLE);
    DefInt(CAPDB_DROP_TEMP_TRIGGER);
    DefInt(CAPDB_DROP_TEMP_VIEW);
    DefInt(CAPDB_DROP_TRIGGER);
    DefInt(CAPDB_DROP_VIEW);
    DefInt(CAPDB_INSERT);
    DefInt(CAPDB_PRAGMA);
    DefInt(CAPDB_READ);
    DefInt(CAPDB_SELECT);
    DefInt(CAPDB_TRANSACTION);
    DefInt(CAPDB_UPDATE);
    DefInt(CAPDB_ATTACH);
    DefInt(CAPDB_DETACH);
    DefInt(CAPDB_ALTER_TABLE);
    DefInt(CAPDB_REINDEX);
    DefInt(CAPDB_ANALYZE);
    DefInt(CAPDB_CREATE_VTABLE);
    DefInt(CAPDB_DROP_VTABLE);
    DefInt(CAPDB_FUNCTION);
    DefInt(CAPDB_SAVEPOINT);
    //DefInt(CAPDB_COPY) /* No longer used */;
    DefInt(CAPDB_RECURSIVE);
  } _DefGroup;

  DefGroup(blobFinalizers) {
    /* CAPDB_STATIC/TRANSIENT need to be handled explicitly as
    ** integers to avoid casting-related warnings. */
    out("\"CAPDB_STATIC\":0, \"CAPDB_TRANSIENT\":-1");
    outf(",\"CAPDB_WASM_DEALLOC\": %lld",
         (capdb_int64)(capdb_free));
  } _DefGroup;

  DefGroup(changeset){
#ifdef CAPDB_CHANGESETSTART_INVERT
    DefInt(CAPDB_CHANGESETSTART_INVERT);
    DefInt(CAPDB_CHANGESETAPPLY_NOSAVEPOINT);
    DefInt(CAPDB_CHANGESETAPPLY_INVERT);
    DefInt(CAPDB_CHANGESETAPPLY_IGNORENOOP);

    DefInt(CAPDB_CHANGESET_DATA);
    DefInt(CAPDB_CHANGESET_NOTFOUND);
    DefInt(CAPDB_CHANGESET_CONFLICT);
    DefInt(CAPDB_CHANGESET_CONSTRAINT);
    DefInt(CAPDB_CHANGESET_FOREIGN_KEY);

    DefInt(CAPDB_CHANGESET_OMIT);
    DefInt(CAPDB_CHANGESET_REPLACE);
    DefInt(CAPDB_CHANGESET_ABORT);
#endif
  } _DefGroup;

  DefGroup(config){
    DefInt(CAPDB_CONFIG_SINGLETHREAD);
    DefInt(CAPDB_CONFIG_MULTITHREAD);
    DefInt(CAPDB_CONFIG_SERIALIZED);
    DefInt(CAPDB_CONFIG_MALLOC);
    DefInt(CAPDB_CONFIG_GETMALLOC);
    DefInt(CAPDB_CONFIG_SCRATCH);
    DefInt(CAPDB_CONFIG_PAGECACHE);
    DefInt(CAPDB_CONFIG_HEAP);
    DefInt(CAPDB_CONFIG_MEMSTATUS);
    DefInt(CAPDB_CONFIG_MUTEX);
    DefInt(CAPDB_CONFIG_GETMUTEX);
/* previously CAPDB_CONFIG_CHUNKALLOC 12 which is now unused. */
    DefInt(CAPDB_CONFIG_LOOKASIDE);
    DefInt(CAPDB_CONFIG_PCACHE);
    DefInt(CAPDB_CONFIG_GETPCACHE);
    DefInt(CAPDB_CONFIG_LOG);
    DefInt(CAPDB_CONFIG_URI);
    DefInt(CAPDB_CONFIG_PCACHE2);
    DefInt(CAPDB_CONFIG_GETPCACHE2);
    DefInt(CAPDB_CONFIG_COVERING_INDEX_SCAN);
    DefInt(CAPDB_CONFIG_SQLLOG);
    DefInt(CAPDB_CONFIG_MMAP_SIZE);
    DefInt(CAPDB_CONFIG_WIN32_HEAPSIZE);
    DefInt(CAPDB_CONFIG_PCACHE_HDRSZ);
    DefInt(CAPDB_CONFIG_PMASZ);
    DefInt(CAPDB_CONFIG_STMTJRNL_SPILL);
    DefInt(CAPDB_CONFIG_SMALL_MALLOC);
    DefInt(CAPDB_CONFIG_SORTERREF_SIZE);
    DefInt(CAPDB_CONFIG_MEMDB_MAXSIZE);
    /* maintenance note: we specifically do not include
       CAPDB_CONFIG_ROWID_IN_VIEW here, on the grounds that
       it's only for legacy support and no apps written with
       this API require that. */
  } _DefGroup;

  DefGroup(dataTypes) {
    DefInt(CAPDB_INTEGER);
    DefInt(CAPDB_FLOAT);
    DefInt(CAPDB_TEXT);
    DefInt(CAPDB_BLOB);
    DefInt(CAPDB_NULL);
  } _DefGroup;

  DefGroup(dbConfig){
    DefInt(CAPDB_DBCONFIG_MAINDBNAME);
    DefInt(CAPDB_DBCONFIG_LOOKASIDE);
    DefInt(CAPDB_DBCONFIG_ENABLE_FKEY);
    DefInt(CAPDB_DBCONFIG_ENABLE_TRIGGER);
    DefInt(CAPDB_DBCONFIG_ENABLE_LOAD_EXTENSION);
    DefInt(CAPDB_DBCONFIG_NO_CKPT_ON_CLOSE);
    DefInt(CAPDB_DBCONFIG_ENABLE_QPSG);
    DefInt(CAPDB_DBCONFIG_TRIGGER_EQP);
    DefInt(CAPDB_DBCONFIG_RESET_DATABASE);
    DefInt(CAPDB_DBCONFIG_DEFENSIVE);
    DefInt(CAPDB_DBCONFIG_WRITABLE_SCHEMA);
    DefInt(CAPDB_DBCONFIG_LEGACY_ALTER_TABLE);
    DefInt(CAPDB_DBCONFIG_DQS_DML);
    DefInt(CAPDB_DBCONFIG_DQS_DDL);
    DefInt(CAPDB_DBCONFIG_ENABLE_VIEW);
    DefInt(CAPDB_DBCONFIG_LEGACY_FILE_FORMAT);
    DefInt(CAPDB_DBCONFIG_TRUSTED_SCHEMA);
    DefInt(CAPDB_DBCONFIG_STMT_SCANSTATUS);
    DefInt(CAPDB_DBCONFIG_REVERSE_SCANORDER);
    DefInt(CAPDB_DBCONFIG_ENABLE_ATTACH_CREATE);
    DefInt(CAPDB_DBCONFIG_ENABLE_ATTACH_WRITE);
    DefInt(CAPDB_DBCONFIG_ENABLE_COMMENTS);
    DefInt(CAPDB_DBCONFIG_MAX);
    DefInt(CAPDB_DBCONFIG_FP_DIGITS);
  } _DefGroup;

  DefGroup(dbStatus){
    DefInt(CAPDB_DBSTATUS_LOOKASIDE_USED);
    DefInt(CAPDB_DBSTATUS_CACHE_USED);
    DefInt(CAPDB_DBSTATUS_SCHEMA_USED);
    DefInt(CAPDB_DBSTATUS_STMT_USED);
    DefInt(CAPDB_DBSTATUS_LOOKASIDE_HIT);
    DefInt(CAPDB_DBSTATUS_LOOKASIDE_MISS_SIZE);
    DefInt(CAPDB_DBSTATUS_LOOKASIDE_MISS_FULL);
    DefInt(CAPDB_DBSTATUS_CACHE_HIT);
    DefInt(CAPDB_DBSTATUS_CACHE_MISS);
    DefInt(CAPDB_DBSTATUS_CACHE_WRITE);
    DefInt(CAPDB_DBSTATUS_DEFERRED_FKS);
    DefInt(CAPDB_DBSTATUS_CACHE_USED_SHARED);
    DefInt(CAPDB_DBSTATUS_CACHE_SPILL);
    DefInt(CAPDB_DBSTATUS_TEMPBUF_SPILL);
    DefInt(CAPDB_DBSTATUS_MAX);
  } _DefGroup;

  DefGroup(encodings) {
    /* Noting that the wasm binding only aims to support UTF-8. */
    DefInt(CAPDB_UTF8);
    DefInt(CAPDB_UTF8_ZT);
    DefInt(CAPDB_UTF16LE);
    DefInt(CAPDB_UTF16BE);
    DefInt(CAPDB_UTF16);
    /*deprecated DefInt(CAPDB_ANY); */
    DefInt(CAPDB_UTF16_ALIGNED);
  } _DefGroup;

  DefGroup(fcntl) {
    DefInt(CAPDB_FCNTL_LOCKSTATE);
    DefInt(CAPDB_FCNTL_GET_LOCKPROXYFILE);
    DefInt(CAPDB_FCNTL_SET_LOCKPROXYFILE);
    DefInt(CAPDB_FCNTL_LAST_ERRNO);
    DefInt(CAPDB_FCNTL_SIZE_HINT);
    DefInt(CAPDB_FCNTL_CHUNK_SIZE);
    DefInt(CAPDB_FCNTL_FILE_POINTER);
    DefInt(CAPDB_FCNTL_SYNC_OMITTED);
    DefInt(CAPDB_FCNTL_WIN32_AV_RETRY);
    DefInt(CAPDB_FCNTL_PERSIST_WAL);
    DefInt(CAPDB_FCNTL_OVERWRITE);
    DefInt(CAPDB_FCNTL_VFSNAME);
    DefInt(CAPDB_FCNTL_POWERSAFE_OVERWRITE);
    DefInt(CAPDB_FCNTL_PRAGMA);
    DefInt(CAPDB_FCNTL_BUSYHANDLER);
    DefInt(CAPDB_FCNTL_TEMPFILENAME);
    DefInt(CAPDB_FCNTL_MMAP_SIZE);
    DefInt(CAPDB_FCNTL_TRACE);
    DefInt(CAPDB_FCNTL_HAS_MOVED);
    DefInt(CAPDB_FCNTL_SYNC);
    DefInt(CAPDB_FCNTL_COMMIT_PHASETWO);
    DefInt(CAPDB_FCNTL_WIN32_SET_HANDLE);
    DefInt(CAPDB_FCNTL_WAL_BLOCK);
    DefInt(CAPDB_FCNTL_ZIPVFS);
    DefInt(CAPDB_FCNTL_RBU);
    DefInt(CAPDB_FCNTL_VFS_POINTER);
    DefInt(CAPDB_FCNTL_JOURNAL_POINTER);
    DefInt(CAPDB_FCNTL_WIN32_GET_HANDLE);
    DefInt(CAPDB_FCNTL_PDB);
    DefInt(CAPDB_FCNTL_BEGIN_ATOMIC_WRITE);
    DefInt(CAPDB_FCNTL_COMMIT_ATOMIC_WRITE);
    DefInt(CAPDB_FCNTL_ROLLBACK_ATOMIC_WRITE);
    DefInt(CAPDB_FCNTL_LOCK_TIMEOUT);
    DefInt(CAPDB_FCNTL_DATA_VERSION);
    DefInt(CAPDB_FCNTL_SIZE_LIMIT);
    DefInt(CAPDB_FCNTL_CKPT_DONE);
    DefInt(CAPDB_FCNTL_RESERVE_BYTES);
    DefInt(CAPDB_FCNTL_CKPT_START);
    DefInt(CAPDB_FCNTL_EXTERNAL_READER);
    DefInt(CAPDB_FCNTL_CKSM_FILE);
    DefInt(CAPDB_FCNTL_RESET_CACHE);
  } _DefGroup;

  DefGroup(flock) {
    DefInt(CAPDB_LOCK_NONE);
    DefInt(CAPDB_LOCK_SHARED);
    DefInt(CAPDB_LOCK_RESERVED);
    DefInt(CAPDB_LOCK_PENDING);
    DefInt(CAPDB_LOCK_EXCLUSIVE);
  } _DefGroup;

  DefGroup(ioCap) {
    DefInt(CAPDB_IOCAP_ATOMIC);
    DefInt(CAPDB_IOCAP_ATOMIC512);
    DefInt(CAPDB_IOCAP_ATOMIC1K);
    DefInt(CAPDB_IOCAP_ATOMIC2K);
    DefInt(CAPDB_IOCAP_ATOMIC4K);
    DefInt(CAPDB_IOCAP_ATOMIC8K);
    DefInt(CAPDB_IOCAP_ATOMIC16K);
    DefInt(CAPDB_IOCAP_ATOMIC32K);
    DefInt(CAPDB_IOCAP_ATOMIC64K);
    DefInt(CAPDB_IOCAP_SAFE_APPEND);
    DefInt(CAPDB_IOCAP_SEQUENTIAL);
    DefInt(CAPDB_IOCAP_UNDELETABLE_WHEN_OPEN);
    DefInt(CAPDB_IOCAP_POWERSAFE_OVERWRITE);
    DefInt(CAPDB_IOCAP_IMMUTABLE);
    DefInt(CAPDB_IOCAP_BATCH_ATOMIC);
  } _DefGroup;

  DefGroup(limits) {
    DefInt(CAPDB_MAX_ALLOCATION_SIZE);
    DefInt(CAPDB_LIMIT_LENGTH);
    DefInt(CAPDB_MAX_LENGTH);
    DefInt(CAPDB_LIMIT_SQL_LENGTH);
    DefInt(CAPDB_MAX_SQL_LENGTH);
    DefInt(CAPDB_LIMIT_COLUMN);
    DefInt(CAPDB_MAX_COLUMN);
    DefInt(CAPDB_LIMIT_EXPR_DEPTH);
    DefInt(CAPDB_MAX_EXPR_DEPTH);
    DefInt(CAPDB_LIMIT_COMPOUND_SELECT);
    DefInt(CAPDB_MAX_COMPOUND_SELECT);
    DefInt(CAPDB_LIMIT_VDBE_OP);
    DefInt(CAPDB_MAX_VDBE_OP);
    DefInt(CAPDB_LIMIT_FUNCTION_ARG);
    DefInt(CAPDB_MAX_FUNCTION_ARG);
    DefInt(CAPDB_LIMIT_ATTACHED);
    DefInt(CAPDB_MAX_ATTACHED);
    DefInt(CAPDB_LIMIT_LIKE_PATTERN_LENGTH);
    DefInt(CAPDB_MAX_LIKE_PATTERN_LENGTH);
    DefInt(CAPDB_LIMIT_VARIABLE_NUMBER);
    DefInt(CAPDB_MAX_VARIABLE_NUMBER);
    DefInt(CAPDB_LIMIT_TRIGGER_DEPTH);
    DefInt(CAPDB_MAX_TRIGGER_DEPTH);
    DefInt(CAPDB_LIMIT_WORKER_THREADS);
    DefInt(CAPDB_MAX_WORKER_THREADS);
    DefInt(CAPDB_LIMIT_PARSER_DEPTH);
    DefInt(CAPDB_MAX_PARSER_DEPTH);
  } _DefGroup;

  DefGroup(openFlags) {
    /* Noting that not all of these will have any effect in
    ** WASM-space. */
    DefInt(CAPDB_OPEN_READONLY);
    DefInt(CAPDB_OPEN_READWRITE);
    DefInt(CAPDB_OPEN_CREATE);
    DefInt(CAPDB_OPEN_URI);
    DefInt(CAPDB_OPEN_MEMORY);
    DefInt(CAPDB_OPEN_NOMUTEX);
    DefInt(CAPDB_OPEN_FULLMUTEX);
    DefInt(CAPDB_OPEN_SHAREDCACHE);
    DefInt(CAPDB_OPEN_PRIVATECACHE);
    DefInt(CAPDB_OPEN_EXRESCODE);
    DefInt(CAPDB_OPEN_NOFOLLOW);
    /* OPEN flags for use with VFSes... */
    DefInt(CAPDB_OPEN_MAIN_DB);
    DefInt(CAPDB_OPEN_MAIN_JOURNAL);
    DefInt(CAPDB_OPEN_TEMP_DB);
    DefInt(CAPDB_OPEN_TEMP_JOURNAL);
    DefInt(CAPDB_OPEN_TRANSIENT_DB);
    DefInt(CAPDB_OPEN_SUBJOURNAL);
    DefInt(CAPDB_OPEN_SUPER_JOURNAL);
    DefInt(CAPDB_OPEN_WAL);
    DefInt(CAPDB_OPEN_DELETEONCLOSE);
    DefInt(CAPDB_OPEN_EXCLUSIVE);
  } _DefGroup;

  DefGroup(prepareFlags) {
    DefInt(CAPDB_PREPARE_PERSISTENT);
    DefInt(CAPDB_PREPARE_NORMALIZE);
    DefInt(CAPDB_PREPARE_NO_VTAB);
    DefInt(CAPDB_PREPARE_FROM_DDL);
  } _DefGroup;

  DefGroup(resultCodes) {
    DefInt(CAPDB_OK);
    DefInt(CAPDB_ERROR);
    DefInt(CAPDB_INTERNAL);
    DefInt(CAPDB_PERM);
    DefInt(CAPDB_ABORT);
    DefInt(CAPDB_BUSY);
    DefInt(CAPDB_LOCKED);
    DefInt(CAPDB_NOMEM);
    DefInt(CAPDB_READONLY);
    DefInt(CAPDB_INTERRUPT);
    DefInt(CAPDB_IOERR);
    DefInt(CAPDB_CORRUPT);
    DefInt(CAPDB_NOTFOUND);
    DefInt(CAPDB_FULL);
    DefInt(CAPDB_CANTOPEN);
    DefInt(CAPDB_PROTOCOL);
    DefInt(CAPDB_EMPTY);
    DefInt(CAPDB_SCHEMA);
    DefInt(CAPDB_TOOBIG);
    DefInt(CAPDB_CONSTRAINT);
    DefInt(CAPDB_MISMATCH);
    DefInt(CAPDB_MISUSE);
    DefInt(CAPDB_NOLFS);
    DefInt(CAPDB_AUTH);
    DefInt(CAPDB_FORMAT);
    DefInt(CAPDB_RANGE);
    DefInt(CAPDB_NOTADB);
    DefInt(CAPDB_NOTICE);
    DefInt(CAPDB_WARNING);
    DefInt(CAPDB_ROW);
    DefInt(CAPDB_DONE);
    // Extended Result Codes
    DefInt(CAPDB_ERROR_MISSING_COLLSEQ);
    DefInt(CAPDB_ERROR_RETRY);
    DefInt(CAPDB_ERROR_SNAPSHOT);
    DefInt(CAPDB_IOERR_READ);
    DefInt(CAPDB_IOERR_SHORT_READ);
    DefInt(CAPDB_IOERR_WRITE);
    DefInt(CAPDB_IOERR_FSYNC);
    DefInt(CAPDB_IOERR_DIR_FSYNC);
    DefInt(CAPDB_IOERR_TRUNCATE);
    DefInt(CAPDB_IOERR_FSTAT);
    DefInt(CAPDB_IOERR_UNLOCK);
    DefInt(CAPDB_IOERR_RDLOCK);
    DefInt(CAPDB_IOERR_DELETE);
    DefInt(CAPDB_IOERR_BLOCKED);
    DefInt(CAPDB_IOERR_NOMEM);
    DefInt(CAPDB_IOERR_ACCESS);
    DefInt(CAPDB_IOERR_CHECKRESERVEDLOCK);
    DefInt(CAPDB_IOERR_LOCK);
    DefInt(CAPDB_IOERR_CLOSE);
    DefInt(CAPDB_IOERR_DIR_CLOSE);
    DefInt(CAPDB_IOERR_SHMOPEN);
    DefInt(CAPDB_IOERR_SHMSIZE);
    DefInt(CAPDB_IOERR_SHMLOCK);
    DefInt(CAPDB_IOERR_SHMMAP);
    DefInt(CAPDB_IOERR_SEEK);
    DefInt(CAPDB_IOERR_DELETE_NOENT);
    DefInt(CAPDB_IOERR_MMAP);
    DefInt(CAPDB_IOERR_GETTEMPPATH);
    DefInt(CAPDB_IOERR_CONVPATH);
    DefInt(CAPDB_IOERR_VNODE);
    DefInt(CAPDB_IOERR_AUTH);
    DefInt(CAPDB_IOERR_BEGIN_ATOMIC);
    DefInt(CAPDB_IOERR_COMMIT_ATOMIC);
    DefInt(CAPDB_IOERR_ROLLBACK_ATOMIC);
    DefInt(CAPDB_IOERR_DATA);
    DefInt(CAPDB_IOERR_CORRUPTFS);
    DefInt(CAPDB_LOCKED_SHAREDCACHE);
    DefInt(CAPDB_LOCKED_VTAB);
    DefInt(CAPDB_BUSY_RECOVERY);
    DefInt(CAPDB_BUSY_SNAPSHOT);
    DefInt(CAPDB_BUSY_TIMEOUT);
    DefInt(CAPDB_CANTOPEN_NOTEMPDIR);
    DefInt(CAPDB_CANTOPEN_ISDIR);
    DefInt(CAPDB_CANTOPEN_FULLPATH);
    DefInt(CAPDB_CANTOPEN_CONVPATH);
    //DefInt(CAPDB_CANTOPEN_DIRTYWAL)/*docs say not used*/;
    DefInt(CAPDB_CANTOPEN_SYMLINK);
    DefInt(CAPDB_CORRUPT_VTAB);
    DefInt(CAPDB_CORRUPT_SEQUENCE);
    DefInt(CAPDB_CORRUPT_INDEX);
    DefInt(CAPDB_READONLY_RECOVERY);
    DefInt(CAPDB_READONLY_CANTLOCK);
    DefInt(CAPDB_READONLY_ROLLBACK);
    DefInt(CAPDB_READONLY_DBMOVED);
    DefInt(CAPDB_READONLY_CANTINIT);
    DefInt(CAPDB_READONLY_DIRECTORY);
    DefInt(CAPDB_ABORT_ROLLBACK);
    DefInt(CAPDB_CONSTRAINT_CHECK);
    DefInt(CAPDB_CONSTRAINT_COMMITHOOK);
    DefInt(CAPDB_CONSTRAINT_FOREIGNKEY);
    DefInt(CAPDB_CONSTRAINT_FUNCTION);
    DefInt(CAPDB_CONSTRAINT_NOTNULL);
    DefInt(CAPDB_CONSTRAINT_PRIMARYKEY);
    DefInt(CAPDB_CONSTRAINT_TRIGGER);
    DefInt(CAPDB_CONSTRAINT_UNIQUE);
    DefInt(CAPDB_CONSTRAINT_VTAB);
    DefInt(CAPDB_CONSTRAINT_ROWID);
    DefInt(CAPDB_CONSTRAINT_PINNED);
    DefInt(CAPDB_CONSTRAINT_DATATYPE);
    DefInt(CAPDB_NOTICE_RECOVER_WAL);
    DefInt(CAPDB_NOTICE_RECOVER_ROLLBACK);
    DefInt(CAPDB_WARNING_AUTOINDEX);
    DefInt(CAPDB_AUTH_USER);
    DefInt(CAPDB_OK_LOAD_PERMANENTLY);
    //DefInt(CAPDB_OK_SYMLINK) /* internal use only */;
  } _DefGroup;

  DefGroup(serialize){
    DefInt(CAPDB_SERIALIZE_NOCOPY);
    DefInt(CAPDB_DESERIALIZE_FREEONCLOSE);
    DefInt(CAPDB_DESERIALIZE_READONLY);
    DefInt(CAPDB_DESERIALIZE_RESIZEABLE);
  } _DefGroup;

  DefGroup(session){
#ifdef CAPDB_SESSION_CONFIG_STRMSIZE
    DefInt(CAPDB_SESSION_CONFIG_STRMSIZE);
    DefInt(CAPDB_SESSION_OBJCONFIG_SIZE);
#endif
  } _DefGroup;

  DefGroup(capdbStatus){
    DefInt(CAPDB_STATUS_MEMORY_USED);
    DefInt(CAPDB_STATUS_PAGECACHE_USED);
    DefInt(CAPDB_STATUS_PAGECACHE_OVERFLOW);
    //DefInt(CAPDB_STATUS_SCRATCH_USED) /* NOT USED */;
    //DefInt(CAPDB_STATUS_SCRATCH_OVERFLOW) /* NOT USED */;
    DefInt(CAPDB_STATUS_MALLOC_SIZE);
    DefInt(CAPDB_STATUS_PARSER_STACK);
    DefInt(CAPDB_STATUS_PAGECACHE_SIZE);
    //DefInt(CAPDB_STATUS_SCRATCH_SIZE) /* NOT USED */;
    DefInt(CAPDB_STATUS_MALLOC_COUNT);
  } _DefGroup;

  DefGroup(stmtStatus){
    DefInt(CAPDB_STMTSTATUS_FULLSCAN_STEP);
    DefInt(CAPDB_STMTSTATUS_SORT);
    DefInt(CAPDB_STMTSTATUS_AUTOINDEX);
    DefInt(CAPDB_STMTSTATUS_VM_STEP);
    DefInt(CAPDB_STMTSTATUS_REPREPARE);
    DefInt(CAPDB_STMTSTATUS_RUN);
    DefInt(CAPDB_STMTSTATUS_FILTER_MISS);
    DefInt(CAPDB_STMTSTATUS_FILTER_HIT);
    DefInt(CAPDB_STMTSTATUS_MEMUSED);
  } _DefGroup;

  DefGroup(syncFlags) {
    DefInt(CAPDB_SYNC_NORMAL);
    DefInt(CAPDB_SYNC_FULL);
    DefInt(CAPDB_SYNC_DATAONLY);
  } _DefGroup;

  DefGroup(trace) {
    DefInt(CAPDB_TRACE_STMT);
    DefInt(CAPDB_TRACE_PROFILE);
    DefInt(CAPDB_TRACE_ROW);
    DefInt(CAPDB_TRACE_CLOSE);
  } _DefGroup;

  DefGroup(txnState){
    DefInt(CAPDB_TXN_NONE);
    DefInt(CAPDB_TXN_READ);
    DefInt(CAPDB_TXN_WRITE);
  } _DefGroup;

  DefGroup(udfFlags) {
    DefInt(CAPDB_DETERMINISTIC);
    DefInt(CAPDB_DIRECTONLY);
    DefInt(CAPDB_INNOCUOUS);
    DefInt(CAPDB_SUBTYPE);
    DefInt(CAPDB_RESULT_SUBTYPE);
    DefInt(CAPDB_SELFORDER1);
  } _DefGroup;

  DefGroup(version) {
    DefInt(CAPDB_VERSION_NUMBER);
    DefStr(CAPDB_VERSION);
    DefStr(CAPDB_SOURCE_ID);
  } _DefGroup;

  DefGroup(vtab) {
#if CAPDB_WASM_HAS_VTAB
    DefInt(CAPDB_INDEX_SCAN_UNIQUE);
    DefInt(CAPDB_INDEX_CONSTRAINT_EQ);
    DefInt(CAPDB_INDEX_CONSTRAINT_GT);
    DefInt(CAPDB_INDEX_CONSTRAINT_LE);
    DefInt(CAPDB_INDEX_CONSTRAINT_LT);
    DefInt(CAPDB_INDEX_CONSTRAINT_GE);
    DefInt(CAPDB_INDEX_CONSTRAINT_MATCH);
    DefInt(CAPDB_INDEX_CONSTRAINT_LIKE);
    DefInt(CAPDB_INDEX_CONSTRAINT_GLOB);
    DefInt(CAPDB_INDEX_CONSTRAINT_REGEXP);
    DefInt(CAPDB_INDEX_CONSTRAINT_NE);
    DefInt(CAPDB_INDEX_CONSTRAINT_ISNOT);
    DefInt(CAPDB_INDEX_CONSTRAINT_ISNOTNULL);
    DefInt(CAPDB_INDEX_CONSTRAINT_ISNULL);
    DefInt(CAPDB_INDEX_CONSTRAINT_IS);
    DefInt(CAPDB_INDEX_CONSTRAINT_LIMIT);
    DefInt(CAPDB_INDEX_CONSTRAINT_OFFSET);
    DefInt(CAPDB_INDEX_CONSTRAINT_FUNCTION);
    DefInt(CAPDB_VTAB_CONSTRAINT_SUPPORT);
    DefInt(CAPDB_VTAB_INNOCUOUS);
    DefInt(CAPDB_VTAB_DIRECTONLY);
    DefInt(CAPDB_VTAB_USES_ALL_SCHEMAS);
    DefInt(CAPDB_ROLLBACK);
    //DefInt(CAPDB_IGNORE); // Also used by capdb_authorizer() callback
    DefInt(CAPDB_FAIL);
    //DefInt(CAPDB_ABORT); // Also an error code
    DefInt(CAPDB_REPLACE);
#endif /*CAPDB_WASM_HAS_VTAB*/
  } _DefGroup;

#undef DefGroup
#undef DefStr
#undef DefInt
#undef _DefGroup

  /*
  ** Emit an array of "StructBinder" struct descriptions, which look
  ** like:
  **
  ** {
  **   "name": "MyStruct",
  **   "sizeof": 16,
  **   "members": {
  **     "member1": {"offset": 0,"sizeof": 4,"signature": "i"},
  **     "member2": {"offset": 4,"sizeof": 4,"signature": "p"},
  **     "member3": {"offset": 8,"sizeof": 8,"signature": "j"}
  **   }
  ** }
  **
  ** Detailed documentation for those bits are in the Jaccwabyt
  ** JS-side component.
  */

  /** Macros for emitting StructBinder description. */
#define StructBinder__(TYPE)                 \
  n = 0;                                     \
  outf("%s{", (nStruct++ ? ", " : ""));      \
  out("\"name\": \"" # TYPE "\",");          \
  outf("\"sizeof\": %d", (int)sizeof(TYPE)); \
  out(",\"members\": {");
#define StructBinder_(T) StructBinder__(T)
  /** ^^^ indirection needed to expand CurrentStruct */
#define StructBinder StructBinder_(CurrentStruct)
#define _StructBinder CloseBrace(2)
#define M3(MEMBER,SIG,READONLY)                                \
  outf("%s\"%s\": "                                            \
       "{\"offset\":%d,\"sizeof\":%d,\"signature\":\"%s\"%s}", \
       (n++ ? ", " : ""), #MEMBER,                             \
       (int)offsetof(CurrentStruct,MEMBER),                    \
       (int)sizeof(((CurrentStruct*)0)->MEMBER),               \
       SIG, (READONLY ? ",\"readOnly\":true" : ""))
#define M(MEMBER,SIG) M3(MEMBER,SIG,0)
#define MRO(MEMBER,SIG) M3(MEMBER,SIG,1)

  nStruct = 0;
  out(", \"structs\": ["); {

#define CurrentStruct capdb_vfs
    StructBinder {
      M(iVersion,          "i");
      M(szOsFile,          "i");
      M(mxPathname,        "i");
      M(pNext,             "p");
      M(zName,             "s");
      M(pAppData,          "p");
      M(xOpen,             "i(pppip)");
      M(xDelete,           "i(ppi)");
      M(xAccess,           "i(ppip)");
      M(xFullPathname,     "i(ppip)");
      M(xDlOpen,           "v(pp)");
      M(xDlError,          "v(pip)");
      M(xDlSym,            "p()");
      M(xDlClose,          "v(pp)");
      M(xRandomness,       "i(pip)");
      M(xSleep,            "i(pi)");
      M(xCurrentTime,      "i(pp)");
      M(xGetLastError,     "i(pip)");
      M(xCurrentTimeInt64, "i(pp)");
      M(xSetSystemCall,    "i(ppp)");
      M(xGetSystemCall,    "p(pp)");
      M(xNextSystemCall,   "p(pp)");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_io_methods
    StructBinder {
      M(iVersion,               "i");
      M(xClose,                 "i(p)");
      M(xRead,                  "i(ppij)");
      M(xWrite,                 "i(ppij)");
      M(xTruncate,              "i(pj)");
      M(xSync,                  "i(pi)");
      M(xFileSize,              "i(pp)");
      M(xLock,                  "i(pi)");
      M(xUnlock,                "i(pi)");
      M(xCheckReservedLock,     "i(pp)");
      M(xFileControl,           "i(pip)");
      M(xSectorSize,            "i(p)");
      M(xDeviceCharacteristics, "i(p)");
      M(xShmMap,                "i(piiip)");
      M(xShmLock,               "i(piii)");
      M(xShmBarrier,            "v(p)");
      M(xShmUnmap,              "i(pi)");
      M(xFetch,                 "i(pjip)");
      M(xUnfetch,               "i(pjp)");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_file
    StructBinder {
      M(pMethods, "p");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_kvvfs_methods
    /* From os_kv.c */
    StructBinder {
      M(xRcrdRead,         "i(sspi)");
      M(xRcrdWrite,        "i(sss)");
      M(xRcrdDelete,       "i(ss)");
      MRO(nKeySize,        "i");
      MRO(nBufferSize,     "i");
      M(pVfs,              "p");
      M(pIoDb,             "p");
      M(pIoJrnl,           "p");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct KVVfsFile
    /* From os_kv.c */
    StructBinder {
      M(base,               "p")/*capdb_file base*/;
      M(zClass,             "s");
      M(isJournal,          "i");
      M(nJrnl,              "i")/*actually unsigned!*/;
      M(aJrnl,              "p");
      M(szPage,             "i");
      M(szDb,               "j");
      M(aData,              "p");
    } _StructBinder;
#undef CurrentStruct

#if CAPDB_WASM_HAS_VTAB
#define CurrentStruct capdb_vtab
    StructBinder {
      M(pModule, "p");
      M(nRef,    "i");
      M(zErrMsg, "p");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_vtab_cursor
    StructBinder {
      M(pVtab, "p");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_module
    StructBinder {
      M(iVersion,       "i");
      M(xCreate,        "i(ppippp)");
      M(xConnect,       "i(ppippp)");
      M(xBestIndex,     "i(pp)");
      M(xDisconnect,    "i(p)");
      M(xDestroy,       "i(p)");
      M(xOpen,          "i(pp)");
      M(xClose,         "i(p)");
      M(xFilter,        "i(pisip)");
      M(xNext,          "i(p)");
      M(xEof,           "i(p)");
      M(xColumn,        "i(ppi)");
      M(xRowid,         "i(pp)");
      M(xUpdate,        "i(pipp)");
      M(xBegin,         "i(p)");
      M(xSync,          "i(p)");
      M(xCommit,        "i(p)");
      M(xRollback,      "i(p)");
      M(xFindFunction,  "i(pispp)");
      M(xRename,        "i(ps)");
      // ^^^ v1. v2+ follows...
      M(xSavepoint,     "i(pi)");
      M(xRelease,       "i(pi)");
      M(xRollbackTo,    "i(pi)");
      // ^^^ v2. v3+ follows...
      M(xShadowName,    "i(s)");
      // ^^^ v3. v4+ follows...
      M(xIntegrity,     "i(pppip)");
    } _StructBinder;
#undef CurrentStruct

    /**
     ** Workaround: in order to map the various inner structs from
     ** capdb_index_info, we have to uplift those into constructs we
     ** can access by type name. These structs _must_ match their
     ** in-capdb_index_info counterparts byte for byte.
     **
     ** 2025-11-21: this uplifing is no longer necessary, as Jaccwabyt
     ** can now handle nested structs, but "it ain't broke" so there's
     ** no pressing need to rewire this. Also, it's conceivable that
     ** rewiring it might break downstream vtab impls, so it shouldn't
     ** be rewired.
     */
    typedef struct {
      int iColumn;
      unsigned char op;
      unsigned char usable;
      int iTermOffset;
    } capdb_index_constraint;
    typedef struct {
      int iColumn;
      unsigned char desc;
    } capdb_index_orderby;
    typedef struct {
      int argvIndex;
      unsigned char omit;
    } capdb_index_constraint_usage;
    { /* Validate that the above struct sizeof()s match
      ** expectations. We could improve upon this by
      ** checking the offsetof() for each member. */
      const capdb_index_info siiCheck = {0};
#define IndexSzCheck(T,M)           \
      (sizeof(T) == sizeof(*siiCheck.M))
      if(!IndexSzCheck(capdb_index_constraint,aConstraint)
         || !IndexSzCheck(capdb_index_orderby,aOrderBy)
         || !IndexSzCheck(capdb_index_constraint_usage,aConstraintUsage)){
        assert(!"sizeof mismatch in capdb_index_... struct(s)");
        return 0;
      }
#undef IndexSzCheck
    }

#define CurrentStruct capdb_index_constraint
    StructBinder {
      M(iColumn,        "i");
      M(op,             "C");
      M(usable,         "C");
      M(iTermOffset,    "i");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_index_orderby
    StructBinder {
      M(iColumn,   "i");
      M(desc,      "C");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_index_constraint_usage
    StructBinder {
      M(argvIndex,  "i");
      M(omit,       "C");
    } _StructBinder;
#undef CurrentStruct

#define CurrentStruct capdb_index_info
    StructBinder {
      M(nConstraint,        "i");
      M(aConstraint,        "p");
      M(nOrderBy,           "i");
      M(aOrderBy,           "p");
      M(aConstraintUsage,   "p");
      M(idxNum,             "i");
      M(idxStr,             "p");
      M(needToFreeIdxStr,   "i");
      M(orderByConsumed,    "i");
      M(estimatedCost,      "d");
      M(estimatedRows,      "j");
      M(idxFlags,           "i");
      M(colUsed,            "j");
    } _StructBinder;
#undef CurrentStruct

#endif /*CAPDB_WASM_HAS_VTAB*/

#if CAPDB_WASM_ENABLE_C_TESTS
#define CurrentStruct WasmTestStruct
    StructBinder {
      M(v4,    "i");
      M(cstr,  "s");
      M(ppV,   "p");
      M(v8,    "j");
      M(xFunc, "v(p)");
    } _StructBinder;
#undef CurrentStruct
#endif /*CAPDB_WASM_ENABLE_C_TESTS*/

  } out( "]"/*structs*/);

  out("}"/*top-level object*/);
  *zPos = 0;
  aBuffer[0] = '{'/*end of the race-condition workaround*/;
  return aBuffer;
#undef StructBinder
#undef StructBinder_
#undef StructBinder__
#undef M
#undef MRO
#undef M3
#undef _StructBinder
#undef CloseBrace
#undef out
#undef outf
#undef lenCheck
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** This function invokes the xDelete method of the given VFS (or the
** default VFS if pVfs is NULL), passing on the given filename. If
** zName is NULL, no default VFS is found, or it has no xDelete
** method, CAPDB_MISUSE is returned, else the result of the xDelete()
** call is returned.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_vfs_unlink,(capdb_vfs *pVfs, const char *zName)){
  int rc = CAPDB_MISUSE /* ??? */;
  if( 0==pVfs && 0!=zName ) pVfs = capdb_vfs_find(0);
  if( zName && pVfs && pVfs->xDelete ){
    rc = pVfs->xDelete(pVfs, zName, 1);
  }
  return rc;
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Returns a pointer to the given DB's VFS for the given DB name,
** defaulting to "main" if zDbName is 0. Returns 0 if no db with the
** given name is open.
*/
CAPDB_WASM_EXPORT2(capdb_vfs *,capdb__wasm_db_vfs,(capdb *pDb, const char *zDbName)){
  capdb_vfs * pVfs = 0;
  capdb_file_control(pDb, zDbName ? zDbName : "main",
                       CAPDB_FCNTL_VFS_POINTER, &pVfs);
  return pVfs;
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** This function resets the given db pointer's database as described at
**
** https://sqlite.org/c3ref/c_dbconfig_defensive.html#sqlitedbconfigresetdatabase
**
** But beware: virtual tables destroyed that way do not have their
** xDestroy() called, so will leak if they require that function for
** proper cleanup.
**
** Returns 0 on success, an CAPDB_xxx code on error. Returns
** CAPDB_MISUSE if pDb is NULL.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_db_reset,(capdb *pDb)){
  int rc = CAPDB_MISUSE;
  if( pDb ){
    capdb_table_column_metadata(pDb, "main", 0, 0, 0, 0, 0, 0, 0);
    rc = capdb_db_config(pDb, CAPDB_DBCONFIG_RESET_DATABASE, 1, 0);
    if( 0==rc ){
      rc = capdb_exec(pDb, "VACUUM", 0, 0, 0);
      capdb_db_config(pDb, CAPDB_DBCONFIG_RESET_DATABASE, 0, 0);
    }
  }
  return rc;
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Uses the given database's VFS xRead to stream the db file's
** contents out to the given callback. The callback gets a single
** chunk of size n (its 2nd argument) on each call and must return 0
** on success, non-0 on error. This function returns 0 on success,
** CAPDB_NOTFOUND if no db is open, or propagates any other non-0
** code from the callback. Note that this is not thread-friendly: it
** expects that it will be the only thread reading the db file and
** takes no measures to ensure that is the case.
**
** This implementation appears to work fine, but
** capdb__wasm_db_serialize() is arguably the better way to achieve
** this.
*/
CAPDB_WASM_EXPORT
int capdb__wasm_db_export_chunked( capdb* pDb,
                                    int (*xCallback)(unsigned const char *zOut, int n) ){
  capdb_int64 nSize = 0;
  capdb_int64 nPos = 0;
  capdb_file * pFile = 0;
  unsigned char buf[1024 * 8];
  int nBuf = (int)sizeof(buf);
  int rc = pDb
    ? capdb_file_control(pDb, "main",
                           CAPDB_FCNTL_FILE_POINTER, &pFile)
    : CAPDB_NOTFOUND;
  if( rc ) return rc;
  rc = pFile->pMethods->xFileSize(pFile, &nSize);
  if( rc ) return rc;
  if(nSize % nBuf){
    /* DB size is not an even multiple of the buffer size. Reduce
    ** buffer size so that we do not unduly inflate the db size
    ** with zero-padding when exporting. */
    if(0 == nSize % 4096) nBuf = 4096;
    else if(0 == nSize % 2048) nBuf = 2048;
    else if(0 == nSize % 1024) nBuf = 1024;
    else nBuf = 512;
  }
  for( ; 0==rc && nPos<nSize; nPos += nBuf ){
    rc = pFile->pMethods->xRead(pFile, buf, nBuf, nPos);
    if( CAPDB_IOERR_SHORT_READ == rc ){
      rc = (nPos + nBuf) < nSize ? rc : 0/*assume EOF*/;
    }
    if( 0==rc ) rc = xCallback(buf, nBuf);
  }
  return rc;
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** A proxy for capdb_serialize() which serializes the schema zSchema
** of pDb, placing the serialized output in pOut and nOut. nOut may be
** NULL. If zSchema is NULL then "main" is assumed. If pDb or pOut are
** NULL then CAPDB_MISUSE is returned. If allocation of the
** serialized copy fails, CAPDB_NOMEM is returned.  On success, 0 is
** returned and `*pOut` will contain a pointer to the memory unless
** mFlags includes CAPDB_SERIALIZE_NOCOPY and the database has no
** contiguous memory representation, in which case `*pOut` will be
** NULL but 0 will be returned.
**
** If `*pOut` is not NULL, the caller is responsible for passing it to
** capdb_free() to free it.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_db_serialize,
                   (capdb *pDb, const char *zSchema,
                    unsigned char **pOut,
                    capdb_int64 *nOut, unsigned int mFlags)){
  unsigned char * z;
  if( !pDb || !pOut ) return CAPDB_MISUSE;
  if( nOut ) *nOut = 0;
  z = capdb_serialize(pDb, zSchema ? zSchema : "main", nOut, mFlags);
  if( z || (CAPDB_SERIALIZE_NOCOPY & mFlags) ){
    *pOut = z;
    return 0;
  }else{
    return CAPDB_NOMEM;
  }
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** ACHTUNG: it was discovered on 2023-08-11 that, with CAPDB_DEBUG,
** this function's out-of-scope use of the capdb_vfs/file/io_methods
** APIs leads to triggering of assertions in the core library. Its use
** is now deprecated and VFS-specific APIs for importing files need to
** be found to replace it. capdb__wasm_posix_create_file() is
** suitable for the "unix" family of VFSes.
**
** Creates a new file using the I/O API of the given VFS, containing
** the given number of bytes of the given data. If the file exists, it
** is truncated to the given length and populated with the given
** data.
**
** This function exists so that we can implement the equivalent of
** Emscripten's FS.createDataFile() in a VFS-agnostic way. This
** functionality is intended for use in uploading database files.
**
** Not all VFSes support this functionality, e.g. the "kvvfs" does
** not.
**
** If pVfs is NULL, capdb_vfs_find(0) is used.
**
** If zFile is NULL, pVfs is NULL (and capdb_vfs_find(0) returns
** NULL), or nData is negative, CAPDB_MISUSE are returned.
**
** On success, it creates a new file with the given name, populated
** with the first nData bytes of pData. If pData is NULL, the file is
** created and/or truncated to nData bytes.
**
** Whether or not directory components of zFilename are created
** automatically or not is unspecified: that detail is left to the
** VFS. The "opfs" VFS, for example, creates them.
**
** If an error happens while populating or truncating the file, the
** target file will be deleted (if needed) if this function created
** it. If this function did not create it, it is not deleted but may
** be left in an undefined state.
**
** Returns 0 on success. On error, it returns a code described above
** or propagates a code from one of the I/O methods.
**
** Design note: nData is an integer, instead of int64, for WASM
** portability, so that the API can still work in builds where BigInt
** support is disabled or unavailable.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_vfs_create_file,
                   (capdb_vfs *pVfs, const char *zFilename,
                    const unsigned char * pData, int nData)){
  int rc;
  capdb_file *pFile = 0;
  capdb_io_methods const *pIo;
  const int openFlags = CAPDB_OPEN_READWRITE | CAPDB_OPEN_CREATE
#if 0 && defined(CAPDB_DEBUG)
    | CAPDB_OPEN_MAIN_JOURNAL
    /* ^^^^ This is for testing a horrible workaround to avoid
       triggering a specific assert() in os_unix.c:unixOpen(). Please
       do not enable this in real builds. */
#endif
    ;
  int flagsOut = 0;
  int fileExisted = 0;
  int doUnlock = 0;
  const unsigned char *pPos = pData;
  const int blockSize = 512
    /* Because we are using pFile->pMethods->xWrite() for writing, and
    ** it may have a buffer limit related to capdb's pager size, we
    ** conservatively write in 512-byte blocks (smallest page
    ** size). */;
  //fprintf(stderr, "pVfs=%p, zFilename=%s, nData=%d\n", pVfs, zFilename, nData);
  if( !pVfs ) pVfs = capdb_vfs_find(0);
  if( !pVfs || !zFilename || nData<0 ) return CAPDB_MISUSE;
  pVfs->xAccess(pVfs, zFilename, CAPDB_ACCESS_EXISTS, &fileExisted);
  rc = capdbOsOpenMalloc(pVfs, zFilename, &pFile, openFlags, &flagsOut);
#if 0
# define RC fprintf(stderr,"create_file(%s,%s) @%d rc=%d\n", \
                    pVfs->zName, zFilename, __LINE__, rc);
#else
# define RC
#endif
  RC;
  if(rc) return rc;
  pIo = pFile->pMethods;
  if( pIo->xLock ) {
    /* We need xLock() in order to accommodate the OPFS VFS, as it
    ** obtains a writeable handle via the lock operation and releases
    ** it in xUnlock(). If we don't do those here, we have to add code
    ** to the VFS to account check whether it was locked before
    ** xFileSize(), xTruncate(), and the like, and release the lock
    ** only if it was unlocked when the op was started. */
    rc = pIo->xLock(pFile, CAPDB_LOCK_EXCLUSIVE);
    RC;
    doUnlock = 0==rc;
  }
  if( 0==rc ){
    rc = pIo->xTruncate(pFile, nData);
    RC;
  }
  if( 0==rc && 0!=pData && nData>0 ){
    while( 0==rc && nData>0 ){
      const int n = nData>=blockSize ? blockSize : nData;
      rc = pIo->xWrite(pFile, pPos, n, (capdb_int64)(pPos - pData));
      RC;
      nData -= n;
      pPos += n;
    }
    if( 0==rc && nData>0 ){
      assert( nData<blockSize );
      rc = pIo->xWrite(pFile, pPos, nData,
                       (capdb_int64)(pPos - pData));
      RC;
    }
  }
  if( pIo->xUnlock && doUnlock!=0 ){
    pIo->xUnlock(pFile, CAPDB_LOCK_NONE);
  }
  pIo->xClose(pFile);
  if( rc!=0 && 0==fileExisted ){
    pVfs->xDelete(pVfs, zFilename, 1);
  }
  RC;
#undef RC
  return rc;
}

/**
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Creates or overwrites a file using the POSIX file API,
** i.e. Emscripten's virtual filesystem. Creates or truncates
** zFilename, appends pData bytes to it, and returns 0 on success or
** CAPDB_IOERR on error.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_posix_create_file,
                   (const char *zFilename, const unsigned char * pData,
                    int nData)){
  int rc;
  FILE * pFile = 0;
  int fileExisted = 0;
  size_t nWrote = 1;

  if( !zFilename || nData<0 || (pData==0 && nData>0) ) return CAPDB_MISUSE;
  pFile = fopen(zFilename, "w");
  if( 0==pFile ) return CAPDB_IOERR;
  if( nData>0 ){
    nWrote = fwrite(pData, (size_t)nData, 1, pFile);
  }
  fclose(pFile);
  return 1==nWrote ? 0 : CAPDB_IOERR;
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** This returns either a pointer to a static buffer or zKeyIn directly
** (if zClass is NULL or empty).
*/
CAPDB_WASM_EXPORT2(const char *,capdb__wasm_kvvfsMakeKey,
                    (const char *zClass, const char *zKeyIn)){
  static char buf[CAPDB_KVOS_SZ+1] = {0};
  assert(capdbKvvfsMethods.nKeySize>24);
  if( zClass && *zClass ){
    kvrecordMakeKey(zClass, zKeyIn, buf);
    return buf;
  }else{
#if 1
    /* We can return zKeyIn here only because the JS API takes special
    ** care with its lifetime.*/
    return zKeyIn;
#else
    /* It would be nice to be able to return zKeyIn directly here, but
    ** it may have been allocated as part of the automated JS-to-WASM
    ** conversions, in which case it will be freed before reaching the
    ** caller. */
    capdb_snprintf(KVRECORD_KEY_SZ, buf, "%s", zKeyIn);
    return buf;
#endif
  }
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Returns the pointer to the singleton object which holds the kvvfs
** I/O methods and associated state.
*/
CAPDB_WASM_EXPORT2(capdb_kvvfs_methods *,capdb__wasm_kvvfs_methods,(void)){
  return &capdbKvvfsMethods;
}

#if CAPDB_WASM_HAS_VTAB
/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** This is a proxy for the variadic capdb_vtab_config() which passes
** its argument on, or not, to capdb_vtab_config(), depending on the
** value of its 2nd argument. Returns the result of
** capdb_vtab_config(), or CAPDB_MISUSE if the 2nd arg is not a
** valid value.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_vtab_config,
                   (capdb *pDb, int op, int arg)){
  switch(op){
  case CAPDB_VTAB_DIRECTONLY:
  case CAPDB_VTAB_INNOCUOUS:
    return capdb_vtab_config(pDb, op);
  case CAPDB_VTAB_CONSTRAINT_SUPPORT:
    return capdb_vtab_config(pDb, op, arg);
  default:
    return CAPDB_MISUSE;
  }
}
#endif /*CAPDB_WASM_HAS_VTAB*/

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Wrapper for the variants of capdb_db_config() which take
** (int,int*) variadic args.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_db_config_ip,
                   (capdb *pDb, int op, int arg1, int* pArg2)){
  switch(op){
    case CAPDB_DBCONFIG_ENABLE_FKEY:
    case CAPDB_DBCONFIG_ENABLE_TRIGGER:
    case CAPDB_DBCONFIG_ENABLE_FTS3_TOKENIZER:
    case CAPDB_DBCONFIG_ENABLE_LOAD_EXTENSION:
    case CAPDB_DBCONFIG_NO_CKPT_ON_CLOSE:
    case CAPDB_DBCONFIG_ENABLE_QPSG:
    case CAPDB_DBCONFIG_TRIGGER_EQP:
    case CAPDB_DBCONFIG_RESET_DATABASE:
    case CAPDB_DBCONFIG_DEFENSIVE:
    case CAPDB_DBCONFIG_WRITABLE_SCHEMA:
    case CAPDB_DBCONFIG_LEGACY_ALTER_TABLE:
    case CAPDB_DBCONFIG_DQS_DML:
    case CAPDB_DBCONFIG_DQS_DDL:
    case CAPDB_DBCONFIG_ENABLE_VIEW:
    case CAPDB_DBCONFIG_LEGACY_FILE_FORMAT:
    case CAPDB_DBCONFIG_TRUSTED_SCHEMA:
    case CAPDB_DBCONFIG_STMT_SCANSTATUS:
    case CAPDB_DBCONFIG_REVERSE_SCANORDER:
    case CAPDB_DBCONFIG_ENABLE_ATTACH_CREATE:
    case CAPDB_DBCONFIG_ENABLE_ATTACH_WRITE:
    case CAPDB_DBCONFIG_ENABLE_COMMENTS:
    case CAPDB_DBCONFIG_FP_DIGITS:
      return capdb_db_config(pDb, op, arg1, pArg2);
    default: return CAPDB_MISUSE;
  }
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Wrapper for the variants of capdb_db_config() which take
** (void*,int,int) variadic args.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_db_config_pii,
                   (capdb *pDb, int op, void * pArg1, int arg2,
                    int arg3)){
  switch(op){
    case CAPDB_DBCONFIG_LOOKASIDE:
      return capdb_db_config(pDb, op, pArg1, arg2, arg3);
    default: return CAPDB_MISUSE;
  }
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Wrapper for the variants of capdb_db_config() which take
** (const char *) variadic args.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_db_config_s,(capdb *pDb, int op,
                                                  const char *zArg)){
  switch(op){
    case CAPDB_DBCONFIG_MAINDBNAME:
      return capdb_db_config(pDb, op, zArg);
    default: return CAPDB_MISUSE;
  }
}


/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Binding for combinations of capdb_config() arguments which take
** a single integer argument.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_config_i,(int op, int arg)){
  return capdb_config(op, arg);
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Binding for combinations of capdb_config() arguments which take
** two int arguments.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_config_ii,(int op, int arg1, int arg2)){
  return capdb_config(op, arg1, arg2);
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** Binding for combinations of capdb_config() arguments which take
** a single i64 argument.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_config_j,(int op, capdb_int64 arg)){
  return capdb_config(op, arg);
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** If z is not NULL, returns the result of passing z to
** capdb_mprintf()'s %Q modifier (if addQuotes is true) or %q (if
** addQuotes is 0). Returns NULL if z is NULL or on OOM.
*/
CAPDB_WASM_EXPORT2(char *,capdb__wasm_qfmt_token,(char *z, int addQuotes)){
  char * rc = 0;
  if( z ){
    rc = addQuotes
      ? capdb_mprintf("%Q", z)
      : capdb_mprintf("%q", z);
  }
  return rc;
}

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings.
**
** A WASM wrapper for the interal os_kv.c:kvvfsDecode() for internal
** use by the kvvfs v2 API.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_kvvfs_decode,(const char *a, char *aOut, int nOut)){
  return kvvfsDecode(a, aOut, nOut);
}
CAPDB_WASM_EXPORT2(int,capdb__wasm_kvvfs_encode,(const char *a, int nA, char *aOut)){
  return kvvfsEncode(a, nA, aOut);
}


#if defined(__EMSCRIPTEN__) && defined(CAPDB_ENABLE_WASMFS)
#include <emscripten/console.h>
#include <emscripten/wasmfs.h>

/*
** This function is NOT part of the capdb public API. It is strictly
** for use by the sqlite project's own JS/WASM bindings, specifically
** only when building with Emscripten's WASMFS support.
**
** This function should only be called if the JS side detects the
** existence of the Origin-Private FileSystem (OPFS) APIs in the
** client. The first time it is called, this function instantiates a
** WASMFS backend impl for OPFS. On success, subsequent calls are
** no-ops.
**
** This function may be passed a "mount point" name, which must have a
** leading "/" and is currently restricted to a single path component,
** e.g. "/foo" is legal but "/foo/" and "/foo/bar" are not. If it is
** NULL or empty, it defaults to "/opfs".
**
** Returns 0 on success, CAPDB_NOMEM if instantiation of the backend
** object fails, CAPDB_IOERR if mkdir() of the zMountPoint dir in
** the virtual FS fails. In builds compiled without CAPDB_ENABLE_WASMFS
** defined, CAPDB_NOTFOUND is returned without side effects.
*/
CAPDB_WASM_EXPORT2(int,capdb__wasm_init_wasmfs,(const char *zMountPoint)){
  static backend_t pOpfs = 0;
  if( !zMountPoint || !*zMountPoint ) zMountPoint = "/opfs";
  if( !pOpfs ){
    pOpfs = wasmfs_create_opfs_backend();
  }
  /** It's not enough to instantiate the backend. We have to create a
      mountpoint in the VFS and attach the backend to it. */
  if( pOpfs && 0!=access(zMountPoint, F_OK) ){
    /* Note that this check and is not robust but it will
       hypothetically suffice for the transient wasm-based virtual
       filesystem we're currently running in. */
    const int rc = wasmfs_create_directory(zMountPoint, 0777, pOpfs);
    /*emscripten_console_logf("OPFS mkdir(%s) rc=%d", zMountPoint, rc);*/
    if(rc) return CAPDB_IOERR;
  }
  return pOpfs ? 0 : CAPDB_NOMEM;
}
#else
CAPDB_WASM_EXPORT2(int,capdb__wasm_init_wasmfs,(const char *zUnused)){
  //emscripten_console_warn("WASMFS OPFS is not compiled in.");
  (void)zUnused;
  return CAPDB_NOTFOUND;
}
#endif /* __EMSCRIPTEN__ && CAPDB_ENABLE_WASMFS */

#if CAPDB_WASM_ENABLE_C_TESTS

CAPDB_WASM_EXPORT2(int,capdb__wasm_test_intptr,(int * p)){
  return *p = *p * 2;
}

CAPDB_WASM_EXPORT2(void *,capdb__wasm_test_voidptr,(void * p)){
  return p;
}

CAPDB_WASM_EXPORT2(int64_t,capdb__wasm_test_int64_max,(void)){
  return (int64_t)0x7fffffffffffffff;
}

CAPDB_WASM_EXPORT2(int64_t,capdb__wasm_test_int64_min,(void)){
  return ~capdb__wasm_test_int64_max();
}

CAPDB_WASM_EXPORT2(int64_t,capdb__wasm_test_int64_times2,(int64_t x)){
  return x * 2;
}

CAPDB_WASM_EXPORT2(void,capdb__wasm_test_int64_minmax,(int64_t * min, int64_t *max)){
  *max = capdb__wasm_test_int64_max();
  *min = capdb__wasm_test_int64_min();
  /*printf("minmax: min=%lld, max=%lld\n", *min, *max);*/
}

CAPDB_WASM_EXPORT2(int64_t,capdb__wasm_test_int64ptr,(int64_t * p)){
  /*printf("capdb__wasm_test_int64ptr( @%lld = 0x%llx )\n", (int64_t)p, *p);*/
  return *p = *p * 2;
}

CAPDB_WASM_EXPORT2(void,capdb__wasm_test_stack_overflow,(int recurse)){
  if(recurse) capdb__wasm_test_stack_overflow(recurse);
}

/* For testing the 'string:dealloc' whwasmutil.xWrap() conversion. */
CAPDB_WASM_EXPORT2(char *,capdb__wasm_test_str_hello,(int fail)){
  char * s = fail ? 0 : (char *)capdb_malloc(6);
  if(s){
    memcpy(s, "hello", 5);
    s[5] = 0;
  }
  return s;
}

/*
** For testing using SQLTester scripts.
**
** Return non-zero if string z matches glob pattern zGlob and zero if the
** pattern does not match.
**
** To repeat:
**
**         zero == no match
**     non-zero == match
**
** Globbing rules:
**
**      '*'       Matches any sequence of zero or more characters.
**
**      '?'       Matches exactly one character.
**
**     [...]      Matches one character from the enclosed list of
**                characters.
**
**     [^...]     Matches one character not in the enclosed list.
**
**      '#'       Matches any sequence of one or more digits with an
**                optional + or - sign in front, or a hexadecimal
**                literal of the form 0x...
*/
static int capdb__wasm_SQLTester_strnotglob(const char *zGlob, const char *z){
  int c, c2;
  int invert;
  int seen;
  typedef int (*recurse_f)(const char *,const char *);
  static const recurse_f recurse = capdb__wasm_SQLTester_strnotglob;

  while( (c = (*(zGlob++)))!=0 ){
    if( c=='*' ){
      while( (c=(*(zGlob++))) == '*' || c=='?' ){
        if( c=='?' && (*(z++))==0 ) return 0;
      }
      if( c==0 ){
        return 1;
      }else if( c=='[' ){
        while( *z && recurse(zGlob-1,z)==0 ){
          z++;
        }
        return (*z)!=0;
      }
      while( (c2 = (*(z++)))!=0 ){
        while( c2!=c ){
          c2 = *(z++);
          if( c2==0 ) return 0;
        }
        if( recurse(zGlob,z) ) return 1;
      }
      return 0;
    }else if( c=='?' ){
      if( (*(z++))==0 ) return 0;
    }else if( c=='[' ){
      int prior_c = 0;
      seen = 0;
      invert = 0;
      c = *(z++);
      if( c==0 ) return 0;
      c2 = *(zGlob++);
      if( c2=='^' ){
        invert = 1;
        c2 = *(zGlob++);
      }
      if( c2==']' ){
        if( c==']' ) seen = 1;
        c2 = *(zGlob++);
      }
      while( c2 && c2!=']' ){
        if( c2=='-' && zGlob[0]!=']' && zGlob[0]!=0 && prior_c>0 ){
          c2 = *(zGlob++);
          if( c>=prior_c && c<=c2 ) seen = 1;
          prior_c = 0;
        }else{
          if( c==c2 ){
            seen = 1;
          }
          prior_c = c2;
        }
        c2 = *(zGlob++);
      }
      if( c2==0 || (seen ^ invert)==0 ) return 0;
    }else if( c=='#' ){
      if( z[0]=='0'
       && (z[1]=='x' || z[1]=='X')
       && capdbIsxdigit(z[2])
      ){
        z += 3;
        while( capdbIsxdigit(z[0]) ){ z++; }
      }else{
        if( (z[0]=='-' || z[0]=='+') && capdbIsdigit(z[1]) ) z++;
        if( !capdbIsdigit(z[0]) ) return 0;
        z++;
        while( capdbIsdigit(z[0]) ){ z++; }
      }
    }else{
      if( c!=(*(z++)) ) return 0;
    }
  }
  return *z==0;
}

CAPDB_WASM_EXPORT2(int,capdb__wasm_SQLTester_strglob,
                    (const char *zGlob, const char *z)){
 return !capdb__wasm_SQLTester_strnotglob(zGlob, z);
}

#endif /* CAPDB_WASM_ENABLE_C_TESTS */

#undef CAPDB_WASM_EXPORT
#undef CAPDB_WASM_HAS_VTAB
#undef CAPDB_WASM_BARE_BONES
#undef CAPDB_WASM_ENABLE_C_TESTS
