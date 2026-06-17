/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Internal interface definitions for SQLite.
**
*/
#ifndef SQLITEINT_H
#define SQLITEINT_H

/* Special Comments:
**
** Some comments have special meaning to the tools that measure test
** coverage:
**
**    NO_TEST                     - The branches on this line are not
**                                  measured by branch coverage.  This is
**                                  used on lines of code that actually
**                                  implement parts of coverage testing.
**
**    OPTIMIZATION-IF-TRUE        - This branch is allowed to always be false
**                                  and the correct answer is still obtained,
**                                  though perhaps more slowly.
**
**    OPTIMIZATION-IF-FALSE       - This branch is allowed to always be true
**                                  and the correct answer is still obtained,
**                                  though perhaps more slowly.
**
**    PREVENTS-HARMLESS-OVERREAD  - This branch prevents a buffer overread
**                                  that would be harmless and undetectable
**                                  if it did occur.
**
** In all cases, the special comment must be enclosed in the usual
** slash-asterisk...asterisk-slash comment marks, with no spaces between the
** asterisks and the comment text.
*/

/*
** Make sure the Tcl calling convention macro is defined.  This macro is
** only used by test code and Tcl integration code.
*/
#ifndef CAPDB_TCLAPI
#  define CAPDB_TCLAPI
#endif

/*
** Include the header file used to customize the compiler options for MSVC.
** This should be done first so that it can successfully prevent spurious
** compiler warnings due to subsequent content in this file and other files
** that are included by this file.
*/
#include "msvc.h"

/*
** Special setup for VxWorks
*/
#include "vxworks.h"

/*
** These #defines should enable >2GB file support on POSIX if the
** underlying operating system supports it.  If the OS lacks
** large file support, or if the OS is windows, these should be no-ops.
**
** Ticket #2739:  The _LARGEFILE_SOURCE macro must appear before any
** system #includes.  Hence, this block of code must be the very first
** code in all source files.
**
** Large file support can be disabled using the -DCAPDB_DISABLE_LFS switch
** on the compiler command line.  This is necessary if you are compiling
** on a recent machine (ex: Red Hat 7.2) but you want your code to work
** on an older machine (ex: Red Hat 6.0).  If you compile on Red Hat 7.2
** without this option, LFS is enable.  But LFS does not exist in the kernel
** in Red Hat 6.0, so the code won't work.  Hence, for maximum binary
** portability you should omit LFS.
**
** The previous paragraph was written in 2005.  (This paragraph is written
** on 2008-11-28.) These days, all Linux kernels support large files, so
** you should probably leave LFS enabled.  But some embedded platforms might
** lack LFS in which case the CAPDB_DISABLE_LFS macro might still be useful.
**
** Similar is true for Mac OS X.  LFS is only supported on Mac OS X 9 and later.
*/
#ifndef CAPDB_DISABLE_LFS
# define _LARGE_FILE       1
# ifndef _FILE_OFFSET_BITS
#   define _FILE_OFFSET_BITS 64
# endif
# define _LARGEFILE_SOURCE 1
#endif

/* The GCC_VERSION and MSVC_VERSION macros are used to
** conditionally include optimizations for each of these compilers.  A
** value of 0 means that compiler is not being used.  The
** CAPDB_DISABLE_INTRINSIC macro means do not use any compiler-specific
** optimizations, and hence set all compiler macros to 0
**
** There was once also a CLANG_VERSION macro.  However, we learn that the
** version numbers in clang are for "marketing" only and are inconsistent
** and unreliable.  Fortunately, all versions of clang also recognize the
** gcc version numbers and have reasonable settings for gcc version numbers,
** so the GCC_VERSION macro will be set to a correct non-zero value even
** when compiling with clang.
*/
#if defined(__GNUC__) && !defined(CAPDB_DISABLE_INTRINSIC)
# define GCC_VERSION (__GNUC__*1000000+__GNUC_MINOR__*1000+__GNUC_PATCHLEVEL__)
#else
# define GCC_VERSION 0
#endif
#if defined(_MSC_VER) && !defined(CAPDB_DISABLE_INTRINSIC)
# define MSVC_VERSION _MSC_VER
#else
# define MSVC_VERSION 0
#endif

/*
** Some C99 functions in "math.h" are only present for MSVC when its version
** is associated with Visual Studio 2013 or higher.
*/
#ifndef CAPDB_HAVE_C99_MATH_FUNCS
# if MSVC_VERSION==0 || MSVC_VERSION>=1800
#  define CAPDB_HAVE_C99_MATH_FUNCS (1)
# else
#  define CAPDB_HAVE_C99_MATH_FUNCS (0)
# endif
#endif

/* Needed for various definitions... */
#if defined(__GNUC__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

#if defined(__OpenBSD__) && !defined(_BSD_SOURCE)
# define _BSD_SOURCE
#endif

/*
** Macro to disable warnings about missing "break" at the end of a "case".
*/
#if defined(__has_attribute)
#  if __has_attribute(fallthrough)
#    define deliberate_fall_through __attribute__((fallthrough));
#  endif
#endif
#if !defined(deliberate_fall_through)
#  define deliberate_fall_through
#endif

/*
** For MinGW, check to see if we can include the header file containing its
** version information, among other things.  Normally, this internal MinGW
** header file would [only] be included automatically by other MinGW header
** files; however, the contained version information is now required by this
** header file to work around binary compatibility issues (see below) and
** this is the only known way to reliably obtain it.  This entire #if block
** would be completely unnecessary if there was any other way of detecting
** MinGW via their preprocessor (e.g. if they customized their GCC to define
** some MinGW-specific macros).  When compiling for MinGW, either the
** _HAVE_MINGW_H or _HAVE__MINGW_H (note the extra underscore) macro must be
** defined; otherwise, detection of conditions specific to MinGW will be
** disabled.
*/
#if defined(_HAVE_MINGW_H)
# include "mingw.h"
#elif defined(_HAVE__MINGW_H)
# include "_mingw.h"
#endif

/*
** For MinGW version 4.x (and higher), check to see if the _USE_32BIT_TIME_T
** define is required to maintain binary compatibility with the MSVC runtime
** library in use (e.g. for Windows XP).
*/
#if !defined(_USE_32BIT_TIME_T) && !defined(_USE_64BIT_TIME_T) && \
    defined(_WIN32) && !defined(_WIN64) && \
    defined(__MINGW_MAJOR_VERSION) && __MINGW_MAJOR_VERSION >= 4 && \
    defined(__MSVCRT__)
# define _USE_32BIT_TIME_T
#endif

/* Optionally #include a user-defined header, whereby compilation options
** may be set prior to where they take effect, but after platform setup.
** If CAPDB_CUSTOM_INCLUDE=? is defined, its value names the #include
** file.
*/
#ifdef CAPDB_CUSTOM_INCLUDE
# define INC_STRINGIFY_(f) #f
# define INC_STRINGIFY(f) INC_STRINGIFY_(f)
# include INC_STRINGIFY(CAPDB_CUSTOM_INCLUDE)
#endif

/* The public SQLite interface.  The _FILE_OFFSET_BITS macro must appear
** first in QNX.  Also, the _USE_32BIT_TIME_T macro must appear first for
** MinGW.
*/
#include "capdb.h"

/*
** Reuse the STATIC_VFS1 for mutex access to capdb_temp_directory.
*/
#define CAPDB_MUTEX_STATIC_TEMPDIR CAPDB_MUTEX_STATIC_VFS1

/*
** Include the configuration header output by 'configure' if we're using the
** autoconf-based build
*/
#if defined(_HAVE_CAPDB_CONFIG_H) && !defined(CAPDBCONFIG_H)
#include "capdb_cfg.h"
#define CAPDBCONFIG_H 1
#endif

#include "capdbLimit.h"

/* Disable nuisance warnings on Borland compilers */
#if defined(__BORLANDC__)
#pragma warn -rch /* unreachable code */
#pragma warn -ccc /* Condition is always true or false */
#pragma warn -aus /* Assigned value is never used */
#pragma warn -csu /* Comparing signed and unsigned */
#pragma warn -spa /* Suspicious pointer arithmetic */
#endif

/*
** A few places in the code require atomic load/store of aligned
** integer values.
*/
#ifndef __has_extension
# define __has_extension(x) 0     /* compatibility with non-clang compilers */
#endif
#if GCC_VERSION>=4007000 || __has_extension(c_atomic)
# define CAPDB_ATOMIC_INTRINSICS 1
# define AtomicLoad(PTR)       __atomic_load_n((PTR),__ATOMIC_RELAXED)
# define AtomicStore(PTR,VAL)  __atomic_store_n((PTR),(VAL),__ATOMIC_RELAXED)
#else
# define CAPDB_ATOMIC_INTRINSICS 0
# define AtomicLoad(PTR)       (*(PTR))
# define AtomicStore(PTR,VAL)  (*(PTR) = (VAL))
#endif

/*
** Include standard header files as necessary
*/
#include <stdint.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

/*
** The following macros are used to cast pointers to integers and
** integers to pointers.  The way you do this varies from one compiler
** to the next, so we have developed the following set of #if statements
** to generate appropriate macros for a wide range of compilers.
**
** The correct "ANSI" way to do this is to use the intptr_t type.
** Unfortunately, that typedef is not available on all compilers, or
** if it is available, it requires an #include of specific headers
** that vary from one machine to the next.
**
** Ticket #3860:  The llvm-gcc-4.2 compiler from Apple chokes on
** the ((void*)&((char*)0)[X]) construct.  But MSVC chokes on ((void*)(X)).
** So we have to define the macros in different ways depending on the
** compiler.
*/
#if defined(HAVE_STDINT_H)   /* Use this case if we have ANSI headers */
# define CAPDB_INT_TO_PTR(X)  ((void*)(intptr_t)(X))
# define CAPDB_PTR_TO_INT(X)  ((int)(intptr_t)(X))
#elif defined(__PTRDIFF_TYPE__)  /* This case should work for GCC */
# define CAPDB_INT_TO_PTR(X)  ((void*)(__PTRDIFF_TYPE__)(X))
# define CAPDB_PTR_TO_INT(X)  ((int)(__PTRDIFF_TYPE__)(X))
#elif !defined(__GNUC__)       /* Works for compilers other than LLVM */
# define CAPDB_INT_TO_PTR(X)  ((void*)&((char*)0)[X])
# define CAPDB_PTR_TO_INT(X)  ((int)(((char*)X)-(char*)0))
#else                          /* Generates a warning - but it always works */
# define CAPDB_INT_TO_PTR(X)  ((void*)(X))
# define CAPDB_PTR_TO_INT(X)  ((int)(X))
#endif

/*
** Macros to hint to the compiler that a function should or should not be
** inlined.
*/
#if defined(__GNUC__)
#  define CAPDB_NOINLINE  __attribute__((noinline))
#  define CAPDB_INLINE    __attribute__((always_inline)) inline
#elif defined(_MSC_VER) && _MSC_VER>=1310
#  define CAPDB_NOINLINE  __declspec(noinline)
#  define CAPDB_INLINE    __forceinline
#else
#  define CAPDB_NOINLINE
#  define CAPDB_INLINE
#endif
#if defined(CAPDB_COVERAGE_TEST) || defined(__STRICT_ANSI__)
# undef CAPDB_INLINE
# define CAPDB_INLINE
#endif

/*
** Make sure that the compiler intrinsics we desire are enabled when
** compiling with an appropriate version of MSVC unless prevented by
** the CAPDB_DISABLE_INTRINSIC define.
*/
#if !defined(CAPDB_DISABLE_INTRINSIC)
#  if defined(_MSC_VER) && _MSC_VER>=1400
#    if !defined(_WIN32_WCE)
#      include <intrin.h>
#      pragma intrinsic(_byteswap_ushort)
#      pragma intrinsic(_byteswap_ulong)
#      pragma intrinsic(_byteswap_uint64)
#      pragma intrinsic(_ReadWriteBarrier)
#    else
#      include <cmnintrin.h>
#    endif
#  endif
#endif

/*
** Enable CAPDB_USE_SEH by default on MSVC builds.  Only omit
** SEH support if the -DCAPDB_OMIT_SEH option is given.
*/
#if defined(_MSC_VER) && !defined(CAPDB_OMIT_SEH)
# define CAPDB_USE_SEH 1
#else
# undef CAPDB_USE_SEH
#endif

/*
** Enable CAPDB_DIRECT_OVERFLOW_READ, unless the build explicitly
** disables it using -DCAPDB_DIRECT_OVERFLOW_READ=0
*/
#if defined(CAPDB_DIRECT_OVERFLOW_READ) && CAPDB_DIRECT_OVERFLOW_READ+1==1
  /* Disable if -DCAPDB_DIRECT_OVERFLOW_READ=0 */
# undef CAPDB_DIRECT_OVERFLOW_READ
#else
  /* In all other cases, enable */
# define CAPDB_DIRECT_OVERFLOW_READ 1
#endif


/*
** The CAPDB_THREADSAFE macro must be defined as 0, 1, or 2.
** 0 means mutexes are permanently disable and the library is never
** threadsafe.  1 means the library is serialized which is the highest
** level of threadsafety.  2 means the library is multithreaded - multiple
** threads can use SQLite as long as no two threads try to use the same
** database connection at the same time.
**
** Older versions of SQLite used an optional THREADSAFE macro.
** We support that for legacy.
**
** To ensure that the correct value of "THREADSAFE" is reported when querying
** for compile-time options at runtime (e.g. "PRAGMA compile_options"), this
** logic is partially replicated in ctime.c. If it is updated here, it should
** also be updated there.
*/
#if !defined(CAPDB_THREADSAFE)
# if defined(THREADSAFE)
#   define CAPDB_THREADSAFE THREADSAFE
# else
#   define CAPDB_THREADSAFE 1 /* IMP: R-07272-22309 */
# endif
#endif

/*
** Powersafe overwrite is on by default.  But can be turned off using
** the -DCAPDB_POWERSAFE_OVERWRITE=0 command-line option.
*/
#ifndef CAPDB_POWERSAFE_OVERWRITE
# define CAPDB_POWERSAFE_OVERWRITE 1
#endif

/*
** EVIDENCE-OF: R-25715-37072 Memory allocation statistics are enabled by
** default unless SQLite is compiled with CAPDB_DEFAULT_MEMSTATUS=0 in
** which case memory allocation statistics are disabled by default.
*/
#if !defined(CAPDB_DEFAULT_MEMSTATUS)
# define CAPDB_DEFAULT_MEMSTATUS 1
#endif

/*
** Exactly one of the following macros must be defined in order to
** specify which memory allocation subsystem to use.
**
**     CAPDB_SYSTEM_MALLOC          // Use normal system malloc()
**     CAPDB_WIN32_MALLOC           // Use Win32 native heap API
**     CAPDB_ZERO_MALLOC            // Use a stub allocator that always fails
**     CAPDB_MEMDEBUG               // Debugging version of system malloc()
**
** On Windows, if the CAPDB_WIN32_MALLOC_VALIDATE macro is defined and the
** assert() macro is enabled, each call into the Win32 native heap subsystem
** will cause HeapValidate to be called.  If heap validation should fail, an
** assertion will be triggered.
**
** If none of the above are defined, then set CAPDB_SYSTEM_MALLOC as
** the default.
*/
#if defined(CAPDB_SYSTEM_MALLOC) \
  + defined(CAPDB_WIN32_MALLOC) \
  + defined(CAPDB_ZERO_MALLOC) \
  + defined(CAPDB_MEMDEBUG)>1
# error "Two or more of the following compile-time configuration options\
 are defined but at most one is allowed:\
 CAPDB_SYSTEM_MALLOC, CAPDB_WIN32_MALLOC, CAPDB_MEMDEBUG,\
 CAPDB_ZERO_MALLOC"
#endif
#if defined(CAPDB_SYSTEM_MALLOC) \
  + defined(CAPDB_WIN32_MALLOC) \
  + defined(CAPDB_ZERO_MALLOC) \
  + defined(CAPDB_MEMDEBUG)==0
# define CAPDB_SYSTEM_MALLOC 1
#endif

/*
** If CAPDB_MALLOC_SOFT_LIMIT is not zero, then try to keep the
** sizes of memory allocations below this value where possible.
*/
#if !defined(CAPDB_MALLOC_SOFT_LIMIT)
# define CAPDB_MALLOC_SOFT_LIMIT 1024
#endif

/*
** We need to define _XOPEN_SOURCE as follows in order to enable
** recursive mutexes on most Unix systems and fchmod() on OpenBSD.
** But _XOPEN_SOURCE define causes problems for Mac OS X, so omit
** it.
*/
#if !defined(_XOPEN_SOURCE) && !defined(__DARWIN__) && !defined(__APPLE__)
#  define _XOPEN_SOURCE 600
#endif

/*
** NDEBUG and CAPDB_DEBUG are opposites.  It should always be true that
** defined(NDEBUG)==!defined(CAPDB_DEBUG).  If this is not currently true,
** make it true by defining or undefining NDEBUG.
**
** Setting NDEBUG makes the code smaller and faster by disabling the
** assert() statements in the code.  So we want the default action
** to be for NDEBUG to be set and NDEBUG to be undefined only if CAPDB_DEBUG
** is set.  Thus NDEBUG becomes an opt-in rather than an opt-out
** feature.
*/
#if !defined(NDEBUG) && !defined(CAPDB_DEBUG)
# define NDEBUG 1
#endif
#if defined(NDEBUG) && defined(CAPDB_DEBUG)
# undef NDEBUG
#endif

/*
** Enable CAPDB_ENABLE_EXPLAIN_COMMENTS if CAPDB_DEBUG is turned on.
*/
#if !defined(CAPDB_ENABLE_EXPLAIN_COMMENTS) && defined(CAPDB_DEBUG)
# define CAPDB_ENABLE_EXPLAIN_COMMENTS 1
#endif

/*
** The testcase() macro is used to aid in coverage testing.  When
** doing coverage testing, the condition inside the argument to
** testcase() must be evaluated both true and false in order to
** get full branch coverage.  The testcase() macro is inserted
** to help ensure adequate test coverage in places where simple
** condition/decision coverage is inadequate.  For example, testcase()
** can be used to make sure boundary values are tested.  For
** bitmask tests, testcase() can be used to make sure each bit
** is significant and used at least once.  On switch statements
** where multiple cases go to the same block of code, testcase()
** can insure that all cases are evaluated.
*/
#if defined(CAPDB_COVERAGE_TEST) || defined(CAPDB_DEBUG)
# ifndef CAPDB_AMALGAMATION
    extern unsigned int capdbCoverageCounter;
# endif
# define testcase(X)  if( X ){ capdbCoverageCounter += (unsigned)__LINE__; }
#else
# define testcase(X)
#endif

/*
** The TESTONLY macro is used to enclose variable declarations or
** other bits of code that are needed to support the arguments
** within testcase() and assert() macros.
*/
#if !defined(NDEBUG) || defined(CAPDB_COVERAGE_TEST)
# define TESTONLY(X)  X
#else
# define TESTONLY(X)
#endif

/*
** Sometimes we need a small amount of code such as a variable initialization
** to setup for a later assert() statement.  We do not want this code to
** appear when assert() is disabled.  The following macro is therefore
** used to contain that setup code.  The "VVA" acronym stands for
** "Verification, Validation, and Accreditation".  In other words, the
** code within VVA_ONLY() will only run during verification processes.
*/
#ifndef NDEBUG
# define VVA_ONLY(X)  X
#else
# define VVA_ONLY(X)
#endif

/*
** Disable ALWAYS() and NEVER() (make them pass-throughs) for coverage
** and mutation testing
*/
#if defined(CAPDB_COVERAGE_TEST) || defined(CAPDB_MUTATION_TEST)
# define CAPDB_OMIT_AUXILIARY_SAFETY_CHECKS  1
#endif

/*
** The ALWAYS and NEVER macros surround boolean expressions which
** are intended to always be true or false, respectively.  Such
** expressions could be omitted from the code completely.  But they
** are included in a few cases in order to enhance the resilience
** of SQLite to unexpected behavior - to make the code "self-healing"
** or "ductile" rather than being "brittle" and crashing at the first
** hint of unplanned behavior.
**
** In other words, ALWAYS and NEVER are added for defensive code.
**
** When doing coverage testing ALWAYS and NEVER are hard-coded to
** be true and false so that the unreachable code they specify will
** not be counted as untested code.
*/
#if defined(CAPDB_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif

/*
** Some conditionals are optimizations only.  In other words, if the
** conditionals are replaced with a constant 1 (true) or 0 (false) then
** the correct answer is still obtained, though perhaps not as quickly.
**
** The following macros mark these optimizations conditionals.
*/
#if defined(CAPDB_MUTATION_TEST)
# define OK_IF_ALWAYS_TRUE(X)  (1)
# define OK_IF_ALWAYS_FALSE(X) (0)
#else
# define OK_IF_ALWAYS_TRUE(X)  (X)
# define OK_IF_ALWAYS_FALSE(X) (X)
#endif

/*
** Some malloc failures are only possible if CAPDB_TEST_REALLOC_STRESS is
** defined.  We need to defend against those failures when testing with
** CAPDB_TEST_REALLOC_STRESS, but we don't want the unreachable branches
** during a normal build.  The following macro can be used to disable tests
** that are always false except when CAPDB_TEST_REALLOC_STRESS is set.
*/
#if defined(CAPDB_TEST_REALLOC_STRESS)
# define ONLY_IF_REALLOC_STRESS(X)  (X)
#elif !defined(NDEBUG)
# define ONLY_IF_REALLOC_STRESS(X)  ((X)?(assert(0),1):0)
#else
# define ONLY_IF_REALLOC_STRESS(X)  (0)
#endif

/*
** Declarations used for tracing the operating system interfaces.
*/
#if defined(CAPDB_FORCE_OS_TRACE) || defined(CAPDB_TEST) || \
    (defined(CAPDB_DEBUG) && CAPDB_OS_WIN)
  extern int capdbOSTrace;
# define OSTRACE(X)          if( capdbOSTrace ) capdbDebugPrintf X
# define CAPDB_HAVE_OS_TRACE
#else
# define OSTRACE(X)
# undef  CAPDB_HAVE_OS_TRACE
#endif

/*
** Is the capdbErrName() function needed in the build?  Currently,
** it is needed by "mutex_w32.c" (when debugging), "os_win.c" (when
** OSTRACE is enabled), and by several "test*.c" files (which are
** compiled using CAPDB_TEST).
*/
#if defined(CAPDB_HAVE_OS_TRACE) || defined(CAPDB_TEST) || \
    (defined(CAPDB_DEBUG) && CAPDB_OS_WIN)
# define CAPDB_NEED_ERR_NAME
#else
# undef  CAPDB_NEED_ERR_NAME
#endif

/*
** CAPDB_ENABLE_EXPLAIN_COMMENTS is incompatible with CAPDB_OMIT_EXPLAIN
*/
#ifdef CAPDB_OMIT_EXPLAIN
# undef CAPDB_ENABLE_EXPLAIN_COMMENTS
#endif

/*
** CAPDB_OMIT_VIRTUALTABLE implies CAPDB_OMIT_ALTERTABLE
*/
#if defined(CAPDB_OMIT_VIRTUALTABLE) && !defined(CAPDB_OMIT_ALTERTABLE)
# define CAPDB_OMIT_ALTERTABLE
#endif

#define CAPDB_DIGIT_SEPARATOR '_'

/*
** Return true (non-zero) if the input is an integer that is too large
** to fit in 32-bits.  This macro is used inside of various testcase()
** macros to verify that we have tested SQLite for large-file support.
*/
#define IS_BIG_INT(X)  (((X)&~(i64)0xffffffff)!=0)

/*
** The macro unlikely() is a hint that surrounds a boolean
** expression that is usually false.  Macro likely() surrounds
** a boolean expression that is usually true.  These hints could,
** in theory, be used by the compiler to generate better code, but
** currently they are just comments for human readers.
*/
#define likely(X)    (X)
#define unlikely(X)  (X)

#include "hash.h"
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include <ctype.h>

/*
** Use a macro to replace memcpy() if compiled with CAPDB_INLINE_MEMCPY.
** This allows better measurements of where memcpy() is used when running
** cachegrind.  But this macro version of memcpy() is very slow so it
** should not be used in production.  This is a performance measurement
** hack only.
*/
#ifdef CAPDB_INLINE_MEMCPY
# define memcpy(D,S,N) {char*xxd=(char*)(D);const char*xxs=(const char*)(S);\
                        int xxn=(N);while(xxn-->0)*(xxd++)=*(xxs++);}
#endif

/*
** If compiling for a processor that lacks floating point support,
** substitute integer for floating-point
*/
#ifdef CAPDB_OMIT_FLOATING_POINT
# define double sqlite_int64
# define float sqlite_int64
# define fabs(X) ((X)<0?-(X):(X))
# define capdbIsOverflow(X) 0
# define INFINITY (9223372036854775807LL)
# ifndef CAPDB_BIG_DBL
#   define CAPDB_BIG_DBL (((capdb_int64)1)<<50)
# endif
# define CAPDB_OMIT_DATETIME_FUNCS 1
# define CAPDB_OMIT_TRACE 1
# undef CAPDB_MIXED_ENDIAN_64BIT_FLOAT
# undef CAPDB_HAVE_ISNAN
#endif
#ifndef CAPDB_BIG_DBL
# define CAPDB_BIG_DBL (1e99)
#endif

/*
** OMIT_TEMPDB is set to 1 if CAPDB_OMIT_TEMPDB is defined, or 0
** afterward. Having this macro allows us to cause the C compiler
** to omit code used by TEMP tables without messy #ifndef statements.
*/
#ifdef CAPDB_OMIT_TEMPDB
#define OMIT_TEMPDB 1
#else
#define OMIT_TEMPDB 0
#endif

/*
** The "file format" number is an integer that is incremented whenever
** the VDBE-level file format changes.  The following macros define the
** the default file format for new databases and the maximum file format
** that the library can read.
*/
#define CAPDB_MAX_FILE_FORMAT 4
#ifndef CAPDB_DEFAULT_FILE_FORMAT
# define CAPDB_DEFAULT_FILE_FORMAT 4
#endif

/*
** Determine whether triggers are recursive by default.  This can be
** changed at run-time using a pragma.
*/
#ifndef CAPDB_DEFAULT_RECURSIVE_TRIGGERS
# define CAPDB_DEFAULT_RECURSIVE_TRIGGERS 0
#endif

/*
** Provide a default value for CAPDB_TEMP_STORE in case it is not specified
** on the command-line
*/
#ifndef CAPDB_TEMP_STORE
# define CAPDB_TEMP_STORE 1
#endif

/*
** If no value has been provided for CAPDB_MAX_WORKER_THREADS, or if
** CAPDB_TEMP_STORE is set to 3 (never use temporary files), set it
** to zero.
*/
#if CAPDB_TEMP_STORE==3 || CAPDB_THREADSAFE==0
# undef CAPDB_MAX_WORKER_THREADS
# define CAPDB_MAX_WORKER_THREADS 0
#endif
#ifndef CAPDB_MAX_WORKER_THREADS
# define CAPDB_MAX_WORKER_THREADS 8
#endif
#ifndef CAPDB_DEFAULT_WORKER_THREADS
# define CAPDB_DEFAULT_WORKER_THREADS 0
#endif
#if CAPDB_DEFAULT_WORKER_THREADS>CAPDB_MAX_WORKER_THREADS
# undef CAPDB_MAX_WORKER_THREADS
# define CAPDB_MAX_WORKER_THREADS CAPDB_DEFAULT_WORKER_THREADS
#endif

/*
** The default initial allocation for the pagecache when using separate
** pagecaches for each database connection.  A positive number is the
** number of pages.  A negative number N translations means that a buffer
** of -1024*N bytes is allocated and used for as many pages as it will hold.
**
** The default value of "20" was chosen to minimize the run-time of the
** speedtest1 test program with options: --shrink-memory --reprepare
*/
#ifndef CAPDB_DEFAULT_PCACHE_INITSZ
# define CAPDB_DEFAULT_PCACHE_INITSZ 20
#endif

/*
** Default value for the CAPDB_CONFIG_SORTERREF_SIZE option.
*/
#ifndef CAPDB_DEFAULT_SORTERREF_SIZE
# define CAPDB_DEFAULT_SORTERREF_SIZE 0x7fffffff
#endif

/*
** The compile-time options CAPDB_MMAP_READWRITE and
** CAPDB_ENABLE_BATCH_ATOMIC_WRITE are not compatible with one another.
** You must choose one or the other (or neither) but not both.
*/
#if defined(CAPDB_MMAP_READWRITE) && defined(CAPDB_ENABLE_BATCH_ATOMIC_WRITE)
#error Cannot use both CAPDB_MMAP_READWRITE and CAPDB_ENABLE_BATCH_ATOMIC_WRITE
#endif

/*
** GCC does not define the offsetof() macro so we'll have to do it
** ourselves.
*/
#ifndef offsetof
# define offsetof(ST,M) ((size_t)((char*)&((ST*)0)->M - (char*)0))
#endif

/*
** Work around C99 "flex-array" syntax for pre-C99 compilers, so as
** to avoid complaints from -fsanitize=strict-bounds.
*/
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEXARRAY
#else
# define FLEXARRAY 1
#endif

/*
** Macros to compute minimum and maximum of two numbers.
*/
#ifndef MIN
# define MIN(A,B) ((A)<(B)?(A):(B))
#endif
#ifndef MAX
# define MAX(A,B) ((A)>(B)?(A):(B))
#endif

/*
** Swap two objects of type TYPE.
*/
#define SWAP(TYPE,A,B) {TYPE t=A; A=B; B=t;}

/*
** Check to see if this machine uses EBCDIC.  (Yes, believe it or
** not, there are still machines out there that use EBCDIC.)
*/
#if 'A' == '\301'
# define CAPDB_EBCDIC 1
#else
# define CAPDB_ASCII 1
#endif

/*
** Integers of known sizes.  These typedefs might change for architectures
** where the sizes very.  Preprocessor macros are available so that the
** types can be conveniently redefined at compile-type.  Like this:
**
**         cc '-DUINTPTR_TYPE=long long int' ...
*/
#ifndef UINT32_TYPE
# ifdef HAVE_UINT32_T
#  define UINT32_TYPE uint32_t
# else
#  define UINT32_TYPE unsigned int
# endif
#endif
#ifndef UINT16_TYPE
# ifdef HAVE_UINT16_T
#  define UINT16_TYPE uint16_t
# else
#  define UINT16_TYPE unsigned short int
# endif
#endif
#ifndef INT16_TYPE
# ifdef HAVE_INT16_T
#  define INT16_TYPE int16_t
# else
#  define INT16_TYPE short int
# endif
#endif
#ifndef UINT8_TYPE
# ifdef HAVE_UINT8_T
#  define UINT8_TYPE uint8_t
# else
#  define UINT8_TYPE unsigned char
# endif
#endif
#ifndef INT8_TYPE
# ifdef HAVE_INT8_T
#  define INT8_TYPE int8_t
# else
#  define INT8_TYPE signed char
# endif
#endif
typedef sqlite_int64 i64;          /* 8-byte signed integer */
typedef sqlite_uint64 u64;         /* 8-byte unsigned integer */
typedef UINT32_TYPE u32;           /* 4-byte unsigned integer */
typedef UINT16_TYPE u16;           /* 2-byte unsigned integer */
typedef INT16_TYPE i16;            /* 2-byte signed integer */
typedef UINT8_TYPE u8;             /* 1-byte unsigned integer */
typedef INT8_TYPE i8;              /* 1-byte signed integer */

/* A bitfield type for use inside of structures.  Always follow with :N where
** N is the number of bits.
*/
typedef unsigned bft;  /* Bit Field Type */

/*
** CAPDB_MAX_U32 is a u64 constant that is the maximum u64 value
** that can be stored in a u32 without loss of data.  The value
** is 0x00000000ffffffff.  But because of quirks of some compilers, we
** have to specify the value in the less intuitive manner shown:
*/
#define CAPDB_MAX_U32  ((((u64)1)<<32)-1)

/*
** The datatype used to store estimates of the number of rows in a
** table or index.
*/
typedef u64 tRowcnt;

/*
** Estimated quantities used for query planning are stored as 16-bit
** logarithms.  For quantity X, the value stored is 10*log2(X).  This
** gives a possible range of values of approximately 1.0e986 to 1e-986.
** But the allowed values are "grainy".  Not every value is representable.
** For example, quantities 16 and 17 are both represented by a LogEst
** of 40.  However, since LogEst quantities are suppose to be estimates,
** not exact values, this imprecision is not a problem.
**
** "LogEst" is short for "Logarithmic Estimate".
**
** Examples:
**      1 -> 0              20 -> 43          10000 -> 132
**      2 -> 10             25 -> 46          25000 -> 146
**      3 -> 16            100 -> 66        1000000 -> 199
**      4 -> 20           1000 -> 99        1048576 -> 200
**     10 -> 33           1024 -> 100    4294967296 -> 320
**
** The LogEst can be negative to indicate fractional values.
** Examples:
**
**    0.5 -> -10           0.1 -> -33        0.0625 -> -40
*/
typedef INT16_TYPE LogEst;
#define LOGEST_MIN (-32768)
#define LOGEST_MAX (32767)

/*
** Set the CAPDB_PTRSIZE macro to the number of bytes in a pointer
*/
#ifndef CAPDB_PTRSIZE
# if defined(__SIZEOF_POINTER__)
#   define CAPDB_PTRSIZE __SIZEOF_POINTER__
# elif defined(i386)     || defined(__i386__)   || defined(_M_IX86) ||    \
       defined(_M_ARM)   || defined(__arm__)    || defined(__x86)   ||    \
      (defined(__APPLE__) && defined(__ppc__)) ||                         \
      (defined(__TOS_AIX__) && !defined(__64BIT__))
#   define CAPDB_PTRSIZE 4
# else
#   define CAPDB_PTRSIZE 8
# endif
#endif

/* The uptr type is an unsigned integer large enough to hold a pointer
*/
#if defined(HAVE_STDINT_H)
  typedef uintptr_t uptr;
#elif CAPDB_PTRSIZE==4
  typedef u32 uptr;
#else
  typedef u64 uptr;
#endif

/*
** The CAPDB_WITHIN(P,S,E) macro checks to see if pointer P points to
** something between S (inclusive) and E (exclusive).
**
** In other words, S is a buffer and E is a pointer to the first byte after
** the end of buffer S.  This macro returns true if P points to something
** contained within the buffer S.
*/
#define CAPDB_WITHIN(P,S,E)   (((uptr)(P)>=(uptr)(S))&&((uptr)(P)<(uptr)(E)))

/*
** P is one byte past the end of a large buffer. Return true if a span of bytes
** between S..E crosses the end of that buffer.  In other words, return true
** if the sub-buffer S..E-1 overflows the buffer whose last byte is P-1.
**
** S is the start of the span.  E is one byte past the end of end of span.
**
**                        P
**     |-----------------|                FALSE
**               |-------|
**               S        E
**
**                        P
**     |-----------------|
**                    |-------|           TRUE
**                    S        E
**
**                        P
**     |-----------------|               
**                        |-------|       FALSE
**                        S        E
*/
#define CAPDB_OVERFLOW(P,S,E) (((uptr)(S)<(uptr)(P))&&((uptr)(E)>(uptr)(P)))

/*
** Macros to determine whether the machine is big or little endian,
** and whether or not that determination is run-time or compile-time.
**
** For best performance, an attempt is made to guess at the byte-order
** using C-preprocessor macros.  If that is unsuccessful, or if
** -DCAPDB_BYTEORDER=0 is set, then byte-order is determined
** at run-time.
**
** If you are building SQLite on some obscure platform for which the
** following ifdef magic does not work, you can always include either:
**
**    -DCAPDB_BYTEORDER=1234
**
** or
**
**    -DCAPDB_BYTEORDER=4321
**
** to cause the build to work for little-endian or big-endian processors,
** respectively.
*/
#ifndef CAPDB_BYTEORDER  /* Replicate changes at tag-20230904a */
# if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
#   define CAPDB_BYTEORDER 4321
# elif defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
#   define CAPDB_BYTEORDER 1234
# elif defined(__BIG_ENDIAN__) && __BIG_ENDIAN__==1
#   define CAPDB_BYTEORDER 4321
# elif defined(i386)    || defined(__i386__)      || defined(_M_IX86) ||    \
     defined(__x86_64)  || defined(__x86_64__)    || defined(_M_X64)  ||    \
     defined(_M_AMD64)  || defined(_M_ARM)        || defined(__x86)   ||    \
     defined(__ARMEL__) || defined(__AARCH64EL__) || defined(_M_ARM64)
#   define CAPDB_BYTEORDER 1234
# elif defined(sparc)   || defined(__ARMEB__)     || defined(__AARCH64EB__)
#   define CAPDB_BYTEORDER 4321
# else
#   define CAPDB_BYTEORDER 0
# endif
#endif
#if CAPDB_BYTEORDER==4321
# define CAPDB_BIGENDIAN    1
# define CAPDB_LITTLEENDIAN 0
# define CAPDB_UTF16NATIVE  CAPDB_UTF16BE
#elif CAPDB_BYTEORDER==1234
# define CAPDB_BIGENDIAN    0
# define CAPDB_LITTLEENDIAN 1
# define CAPDB_UTF16NATIVE  CAPDB_UTF16LE
#else
# ifdef CAPDB_AMALGAMATION
  const int capdbone = 1;
# else
  extern const int capdbone;
# endif
# define CAPDB_BIGENDIAN    (*(char *)(&capdbone)==0)
# define CAPDB_LITTLEENDIAN (*(char *)(&capdbone)==1)
# define CAPDB_UTF16NATIVE  (CAPDB_BIGENDIAN?CAPDB_UTF16BE:CAPDB_UTF16LE)
#endif

/*
** Constants for the largest and smallest possible 64-bit signed integers.
** These macros are designed to work correctly on both 32-bit and 64-bit
** compilers.
*/
#define LARGEST_INT64  (0xffffffff|(((i64)0x7fffffff)<<32))
#define LARGEST_UINT64 (0xffffffff|(((u64)0xffffffff)<<32))
#define SMALLEST_INT64 (((i64)-1) - LARGEST_INT64)

/*
** Macro SMXV(n) return the maximum value that can be held in variable n,
** assuming n is a signed integer type.  UMXV(n) is similar for unsigned
** integer types.
*/
#define SMXV(n) ((((i64)1)<<(sizeof(n)*8-1))-1)
#define UMXV(n) ((((i64)1)<<(sizeof(n)*8))-1)

/*
** Round up a number to the next larger multiple of 8.  This is used
** to force 8-byte alignment on 64-bit architectures.
**
** ROUND8() always does the rounding, for any argument.
**
** ROUND8P() assumes that the argument is already an integer number of
** pointers in size, and so it is a no-op on systems where the pointer
** size is 8.
*/
#define ROUND8(x)     (((x)+7)&~7)
#if CAPDB_PTRSIZE==8
# define ROUND8P(x)   (x)
#else
# define ROUND8P(x)   (((x)+7)&~7)
#endif

/*
** Round down to the nearest multiple of 8
*/
#define ROUNDDOWN8(x) ((x)&~7)

/*
** Assert that the pointer X is aligned to an 8-byte boundary.  This
** macro is used only within assert() to verify that the code gets
** all alignment restrictions correct.
**
** Except, if CAPDB_4_BYTE_ALIGNED_MALLOC is defined, then the
** underlying malloc() implementation might return us 4-byte aligned
** pointers.  In that case, only verify 4-byte alignment.
*/
#ifdef CAPDB_4_BYTE_ALIGNED_MALLOC
# define EIGHT_BYTE_ALIGNMENT(X)   ((((uptr)(X) - (uptr)0)&3)==0)
#else
# define EIGHT_BYTE_ALIGNMENT(X)   ((((uptr)(X) - (uptr)0)&7)==0)
#endif
#define TWO_BYTE_ALIGNMENT(X)      ((((uptr)(X) - (uptr)0)&1)==0)

/*
** Disable MMAP on platforms where it is known to not work
*/
#if defined(__OpenBSD__) || defined(__QNXNTO__)
# undef CAPDB_MAX_MMAP_SIZE
# define CAPDB_MAX_MMAP_SIZE 0
#endif

/*
** Default maximum size of memory used by memory-mapped I/O in the VFS
*/
#ifdef __APPLE__
# include <TargetConditionals.h>
#endif
#ifndef CAPDB_MAX_MMAP_SIZE
# if defined(__linux__) \
  || defined(_WIN32) \
  || (defined(__APPLE__) && defined(__MACH__)) \
  || defined(__sun) \
  || defined(__FreeBSD__) \
  || defined(__DragonFly__)
#   define CAPDB_MAX_MMAP_SIZE 0x7fff0000  /* 2147418112 */
# else
#   define CAPDB_MAX_MMAP_SIZE 0
# endif
#endif

/*
** The default MMAP_SIZE is zero on all platforms.  Or, even if a larger
** default MMAP_SIZE is specified at compile-time, make sure that it does
** not exceed the maximum mmap size.
*/
#ifndef CAPDB_DEFAULT_MMAP_SIZE
# define CAPDB_DEFAULT_MMAP_SIZE 0
#endif
#if CAPDB_DEFAULT_MMAP_SIZE>CAPDB_MAX_MMAP_SIZE
# undef CAPDB_DEFAULT_MMAP_SIZE
# define CAPDB_DEFAULT_MMAP_SIZE CAPDB_MAX_MMAP_SIZE
#endif

/*
** TREETRACE_ENABLED will be either 1 or 0 depending on whether or not
** the Abstract Syntax Tree tracing logic is turned on.
*/
#if !defined(CAPDB_AMALGAMATION)
extern u32 capdbTreeTrace;
#endif
#if defined(CAPDB_DEBUG) \
    && (defined(CAPDB_TEST) || defined(CAPDB_ENABLE_SELECTTRACE) \
                             || defined(CAPDB_ENABLE_TREETRACE))
# define TREETRACE_ENABLED 1
# define TREETRACE(K,P,S,X)  \
  if(capdbTreeTrace&(K))   \
    capdbDebugPrintf("%u/%d/%p: ",(S)->selId,(P)->addrExplain,(S)),\
    capdbDebugPrintf X
#else
# define TREETRACE(K,P,S,X)
# define TREETRACE_ENABLED 0
#endif

/* TREETRACE flag meanings:
**
**   0x00000001     Beginning and end of SELECT processing
**   0x00000002     WHERE clause processing
**   0x00000004     Query flattener
**   0x00000008     Result-set wildcard expansion
**   0x00000010     Query name resolution
**   0x00000020     Aggregate analysis
**   0x00000040     Window functions
**   0x00000080     Generated column names
**   0x00000100     Move HAVING terms into WHERE
**   0x00000200     Count-of-view optimization
**   0x00000400     Compound SELECT processing
**   0x00000800     Drop superfluous ORDER BY
**   0x00001000     LEFT JOIN simplifies to JOIN
**   0x00002000     Constant propagation
**   0x00004000     Push-down optimization
**   0x00008000     After all FROM-clause analysis
**   0x00010000     Beginning of DELETE/INSERT/UPDATE processing
**   0x00020000     Transform DISTINCT into GROUP BY
**   0x00040000     SELECT tree dump after all code has been generated
**   0x00080000     NOT NULL strength reduction
**   0x00100000     Pointers are all shown as zero
**   0x00200000     EXISTS-to-JOIN optimization
*/

/*
** Macros for "wheretrace"
*/
extern u32 capdbWhereTrace;
#if defined(CAPDB_DEBUG) \
    && (defined(CAPDB_TEST) || defined(CAPDB_ENABLE_WHERETRACE))
# define WHERETRACE(K,X)  if(capdbWhereTrace&(K)) capdbDebugPrintf X
# define WHERETRACE_ENABLED 1
#else
# define WHERETRACE(K,X)
#endif

/*
** Bits for the capdbWhereTrace mask:
**
** (---any--)   Top-level block structure
** 0x-------F   High-level debug messages
** 0x----FFF-   More detail
** 0xFFFF----   Low-level debug messages
**
** 0x00000001   Code generation
** 0x00000002   Solver (Use 0x40000 for less detail)
** 0x00000004   Solver costs
** 0x00000008   WhereLoop inserts
**
** 0x00000010   Display capdb_index_info xBestIndex calls
** 0x00000020   Range an equality scan metrics
** 0x00000040   IN operator decisions
** 0x00000080   WhereLoop cost adjustments
** 0x00000100
** 0x00000200   Covering index decisions
** 0x00000400   OR optimization
** 0x00000800   Index scanner
** 0x00001000   More details associated with code generation
** 0x00002000
** 0x00004000   Show all WHERE terms at key points
** 0x00008000   Show the full SELECT statement at key places
**
** 0x00010000   Show more detail when printing WHERE terms
** 0x00020000   Show WHERE terms returned from whereScanNext()
** 0x00040000   Solver overview messages
** 0x00080000   Star-query heuristic
** 0x00100000   Pointers are all shown as zero
*/


/*
** An instance of the following structure is used to store the busy-handler
** callback for a given sqlite handle.
**
** The sqlite.busyHandler member of the sqlite struct contains the busy
** callback for the database handle. Each pager opened via the sqlite
** handle is passed a pointer to sqlite.busyHandler. The busy-handler
** callback is currently invoked only from within pager.c.
*/
typedef struct BusyHandler BusyHandler;
struct BusyHandler {
  int (*xBusyHandler)(void *,int);  /* The busy callback */
  void *pBusyArg;                   /* First arg to busy callback */
  int nBusy;                        /* Incremented with each busy call */
};

/*
** Name of table that holds the database schema.
**
** The PREFERRED names are used wherever possible.  But LEGACY is also
** used for backwards compatibility.
**
**  1.  Queries can use either the PREFERRED or the LEGACY names
**  2.  The capdb_set_authorizer() callback uses the LEGACY name
**  3.  The PRAGMA table_list statement uses the PREFERRED name
**
** The LEGACY names are stored in the internal symbol hash table
** in support of (2).  Names are translated using capdbPreferredTableName()
** for (3).  The capdbFindTable() function takes care of translating
** names for (1).
**
** Note that "sqlite_temp_schema" can also be called "temp.sqlite_schema".
*/
#define LEGACY_SCHEMA_TABLE          "sqlite_master"
#define LEGACY_TEMP_SCHEMA_TABLE     "sqlite_temp_master"
#define PREFERRED_SCHEMA_TABLE       "sqlite_schema"
#define PREFERRED_TEMP_SCHEMA_TABLE  "sqlite_temp_schema"


/*
** The root-page of the schema table.
*/
#define SCHEMA_ROOT    1

/*
** The name of the schema table.  The name is different for TEMP.
*/
#define SCHEMA_TABLE(x) \
    ((!OMIT_TEMPDB)&&(x==1)?LEGACY_TEMP_SCHEMA_TABLE:LEGACY_SCHEMA_TABLE)

/*
** A convenience macro that returns the number of elements in
** an array.
*/
#define ArraySize(X)    ((int)(sizeof(X)/sizeof(X[0])))

/*
** Determine if the argument is a power of two
*/
#define IsPowerOfTwo(X) (((X)&((X)-1))==0)

/*
** The following value as a destructor means to use capdbDbFree().
** The capdbDbFree() routine requires two parameters instead of the
** one parameter that destructors normally want.  So we have to introduce
** this magic value that the code knows to handle differently.  Any
** pointer will work here as long as it is distinct from CAPDB_STATIC
** and CAPDB_TRANSIENT.
*/
#define CAPDB_DYNAMIC   ((capdb_destructor_type)capdbRowSetClear)

/*
** When CAPDB_OMIT_WSD is defined, it means that the target platform does
** not support Writable Static Data (WSD) such as global and static variables.
** All variables must either be on the stack or dynamically allocated from
** the heap.  When WSD is unsupported, the variable declarations scattered
** throughout the SQLite code must become constants instead.  The CAPDB_WSD
** macro is used for this purpose.  And instead of referencing the variable
** directly, we use its constant as a key to lookup the run-time allocated
** buffer that holds real variable.  The constant is also the initializer
** for the run-time allocated buffer.
**
** In the usual case where WSD is supported, the CAPDB_WSD and GLOBAL
** macros become no-ops and have zero performance impact.
*/
#ifdef CAPDB_OMIT_WSD
  #define CAPDB_WSD const
  #define GLOBAL(t,v) (*(t*)capdb_wsd_find((void*)&(v), sizeof(v)))
  #define capdbGlobalConfig GLOBAL(struct Sqlite3Config, capdbConfig)
  int capdb_wsd_init(int N, int J);
  void *capdb_wsd_find(void *K, int L);
#else
  #define CAPDB_WSD
  #define GLOBAL(t,v) v
  #define capdbGlobalConfig capdbConfig
#endif

/*
** The following macros are used to suppress compiler warnings and to
** make it clear to human readers when a function parameter is deliberately
** left unused within the body of a function. This usually happens when
** a function is called via a function pointer. For example the
** implementation of an SQL aggregate step callback may not use the
** parameter indicating the number of arguments passed to the aggregate,
** if it knows that this is enforced elsewhere.
**
** When a function parameter is not used at all within the body of a function,
** it is generally named "NotUsed" or "NotUsed2" to make things even clearer.
** However, these macros may also be used to suppress warnings related to
** parameters that may or may not be used depending on compilation options.
** For example those parameters only used in assert() statements. In these
** cases the parameters are named as per the usual conventions.
*/
#define UNUSED_PARAMETER(x) (void)(x)
#define UNUSED_PARAMETER2(x,y) UNUSED_PARAMETER(x),UNUSED_PARAMETER(y)

/*
** Forward references to structures
*/
typedef struct AggInfo AggInfo;
typedef struct AuthContext AuthContext;
typedef struct AutoincInfo AutoincInfo;
typedef struct Bitvec Bitvec;
typedef struct CollSeq CollSeq;
typedef struct Column Column;
typedef struct Cte Cte;
typedef struct CteUse CteUse;
typedef struct Db Db;
typedef struct DbClientData DbClientData;
typedef struct DbFixer DbFixer;
typedef struct Schema Schema;
typedef struct Expr Expr;
typedef struct ExprList ExprList;
typedef struct FKey FKey;
typedef struct FpDecode FpDecode;
typedef struct FuncDestructor FuncDestructor;
typedef struct FuncDef FuncDef;
typedef struct FuncDefHash FuncDefHash;
typedef struct IdList IdList;
typedef struct Index Index;
typedef struct IndexedExpr IndexedExpr;
typedef struct IndexSample IndexSample;
typedef struct KeyClass KeyClass;
typedef struct KeyInfo KeyInfo;
typedef struct Lookaside Lookaside;
typedef struct LookasideSlot LookasideSlot;
typedef struct Module Module;
typedef struct NameContext NameContext;
typedef struct OnOrUsing OnOrUsing;
typedef struct Parse Parse;
typedef struct ParseCleanup ParseCleanup;
typedef struct PreUpdate PreUpdate;
typedef struct PrintfArguments PrintfArguments;
typedef struct RCStr RCStr;
typedef struct RenameToken RenameToken;
typedef struct Returning Returning;
typedef struct RowSet RowSet;
typedef struct Savepoint Savepoint;
typedef struct Select Select;
typedef struct SQLiteThread SQLiteThread;
typedef struct SelectDest SelectDest;
typedef struct Subquery Subquery;
typedef struct SrcItem SrcItem;
typedef struct SrcList SrcList;
typedef struct capdb_str StrAccum; /* Internal alias for capdb_str */
typedef struct Table Table;
typedef struct TableLock TableLock;
typedef struct Token Token;
typedef struct TreeView TreeView;
typedef struct Trigger Trigger;
typedef struct TriggerPrg TriggerPrg;
typedef struct TriggerStep TriggerStep;
typedef struct UnpackedRecord UnpackedRecord;
typedef struct Upsert Upsert;
typedef struct VTable VTable;
typedef struct VtabCtx VtabCtx;
typedef struct Walker Walker;
typedef struct WhereInfo WhereInfo;
typedef struct Window Window;
typedef struct With With;


/*
** The bitmask datatype defined below is used for various optimizations.
**
** Changing this from a 64-bit to a 32-bit type limits the number of
** tables in a join to 32 instead of 64.  But it also reduces the size
** of the library by 738 bytes on ix86.
*/
#ifdef CAPDB_BITMASK_TYPE
  typedef CAPDB_BITMASK_TYPE Bitmask;
#else
  typedef u64 Bitmask;
#endif

/*
** The number of bits in a Bitmask.  "BMS" means "BitMask Size".
*/
#define BMS  ((int)(sizeof(Bitmask)*8))

/*
** A bit in a Bitmask
*/
#define MASKBIT(n)    (((Bitmask)1)<<(n))
#define MASKBIT64(n)  (((u64)1)<<(n))
#define MASKBIT32(n)  (((unsigned int)1)<<(n))
#define SMASKBIT32(n) ((n)<=31?((unsigned int)1)<<(n):0)
#define ALLBITS       ((Bitmask)-1)
#define TOPBIT        (((Bitmask)1)<<(BMS-1))

/* A VList object records a mapping between parameters/variables/wildcards
** in the SQL statement (such as $abc, @pqr, or :xyz) and the integer
** variable number associated with that parameter.  See the format description
** on the capdbVListAdd() routine for more information.  A VList is really
** just an array of integers.
*/
typedef int VList;

/*
** Defer sourcing vdbe.h and btree.h until after the "u8" and
** "BusyHandler" typedefs. vdbe.h also requires a few of the opaque
** pointer types (i.e. FuncDef) defined above.
*/
#include "os.h"
#include "pager.h"
#include "btree.h"
#include "vdbe.h"
#include "pcache.h"
#include "mutex.h"

/* The CAPDB_EXTRA_DURABLE compile-time option used to set the default
** synchronous setting to EXTRA.  It is no longer supported.
*/
#ifdef CAPDB_EXTRA_DURABLE
# warning Use CAPDB_DEFAULT_SYNCHRONOUS=3 instead of CAPDB_EXTRA_DURABLE
# define CAPDB_DEFAULT_SYNCHRONOUS 3
#endif

/*
** Default synchronous levels.
**
** Note that (for historical reasons) the PAGER_SYNCHRONOUS_* macros differ
** from the CAPDB_DEFAULT_SYNCHRONOUS value by 1.
**
**           PAGER_SYNCHRONOUS       DEFAULT_SYNCHRONOUS
**   OFF           1                         0
**   NORMAL        2                         1
**   FULL          3                         2
**   EXTRA         4                         3
**
** The "PRAGMA synchronous" statement also uses the zero-based numbers.
** In other words, the zero-based numbers are used for all external interfaces
** and the one-based values are used internally.
*/
#ifndef CAPDB_DEFAULT_SYNCHRONOUS
# define CAPDB_DEFAULT_SYNCHRONOUS 2
#endif
#ifndef CAPDB_DEFAULT_WAL_SYNCHRONOUS
# define CAPDB_DEFAULT_WAL_SYNCHRONOUS CAPDB_DEFAULT_SYNCHRONOUS
#endif

/*
** Each database file to be accessed by the system is an instance
** of the following structure.  There are normally two of these structures
** in the sqlite.aDb[] array.  aDb[0] is the main database file and
** aDb[1] is the database file used to hold temporary tables.  Additional
** databases may be attached.
*/
struct Db {
  char *zDbSName;      /* Name of this database. (schema name, not filename) */
  Btree *pBt;          /* The B*Tree structure for this database file */
  u8 safety_level;     /* How aggressive at syncing data to disk */
  u8 bSyncSet;         /* True if "PRAGMA synchronous=N" has been run */
  Schema *pSchema;     /* Pointer to database schema (possibly shared) */
};

/*
** An instance of the following structure stores a database schema.
**
** Most Schema objects are associated with a Btree.  The exception is
** the Schema for the TEMP database (capdb.aDb[1]) which is free-standing.
** In shared cache mode, a single Schema object can be shared by multiple
** Btrees that refer to the same underlying BtShared object.
**
** Schema objects are automatically deallocated when the last Btree that
** references them is destroyed.   The TEMP Schema is manually freed by
** capdb_close().
*
** A thread must be holding a mutex on the corresponding Btree in order
** to access Schema content.  This implies that the thread must also be
** holding a mutex on the capdb connection pointer that owns the Btree.
** For a TEMP Schema, only the connection mutex is required.
*/
struct Schema {
  int schema_cookie;   /* Database schema version number for this file */
  int iGeneration;     /* Generation counter.  Incremented with each change */
  Hash tblHash;        /* All tables indexed by name */
  Hash idxHash;        /* All (named) indices indexed by name */
  Hash trigHash;       /* All triggers indexed by name */
  Hash fkeyHash;       /* All foreign keys by referenced table name */
  Table *pSeqTab;      /* The sqlite_sequence table used by AUTOINCREMENT */
  u8 file_format;      /* Schema format version for this file */
  u8 enc;              /* Text encoding used by this database */
  u16 schemaFlags;     /* Flags associated with this schema */
  int cache_size;      /* Number of pages to use in the cache */
};

/*
** These macros can be used to test, set, or clear bits in the
** Db.pSchema->flags field.
*/
#define DbHasProperty(D,I,P)     (((D)->aDb[I].pSchema->schemaFlags&(P))==(P))
#define DbHasAnyProperty(D,I,P)  (((D)->aDb[I].pSchema->schemaFlags&(P))!=0)
#define DbSetProperty(D,I,P)     (D)->aDb[I].pSchema->schemaFlags|=(P)
#define DbClearProperty(D,I,P)   (D)->aDb[I].pSchema->schemaFlags&=~(P)

/*
** Allowed values for the DB.pSchema->flags field.
**
** The DB_SchemaLoaded flag is set after the database schema has been
** read into internal hash tables.
**
** DB_UnresetViews means that one or more views have column names that
** have been filled out.  If the schema changes, these column names might
** changes and so the view will need to be reset.
*/
#define DB_SchemaLoaded    0x0001  /* The schema has been loaded */
#define DB_UnresetViews    0x0002  /* Some views have defined column names */
#define DB_ResetWanted     0x0008  /* Reset the schema when nSchemaLock==0 */

/*
** The number of different kinds of things that can be limited
** using the capdb_limit() interface.
*/
#define CAPDB_N_LIMIT (CAPDB_LIMIT_PARSER_DEPTH+1)

/*
** Lookaside malloc is a set of fixed-size buffers that can be used
** to satisfy small transient memory allocation requests for objects
** associated with a particular database connection.  The use of
** lookaside malloc provides a significant performance enhancement
** (approx 10%) by avoiding numerous malloc/free requests while parsing
** SQL statements.
**
** The Lookaside structure holds configuration information about the
** lookaside malloc subsystem.  Each available memory allocation in
** the lookaside subsystem is stored on a linked list of LookasideSlot
** objects.
**
** Lookaside allocations are only allowed for objects that are associated
** with a particular database connection.  Hence, schema information cannot
** be stored in lookaside because in shared cache mode the schema information
** is shared by multiple database connections.  Therefore, while parsing
** schema information, the Lookaside.bEnabled flag is cleared so that
** lookaside allocations are not used to construct the schema objects.
**
** New lookaside allocations are only allowed if bDisable==0.  When
** bDisable is greater than zero, sz is set to zero which effectively
** disables lookaside without adding a new test for the bDisable flag
** in a performance-critical path.  sz should be set by to szTrue whenever
** bDisable changes back to zero.
**
** Lookaside buffers are initially held on the pInit list.  As they are
** used and freed, they are added back to the pFree list.  New allocations
** come off of pFree first, then pInit as a fallback.  This dual-list
** allows use to compute a high-water mark - the maximum number of allocations
** outstanding at any point in the past - by subtracting the number of
** allocations on the pInit list from the total number of allocations.
**
** Enhancement on 2019-12-12:  Two-size-lookaside
** The default lookaside configuration is 100 slots of 1200 bytes each.
** The larger slot sizes are important for performance, but they waste
** a lot of space, as most lookaside allocations are less than 128 bytes.
** The two-size-lookaside enhancement breaks up the lookaside allocation
** into two pools:  One of 128-byte slots and the other of the default size
** (1200-byte) slots.   Allocations are filled from the small-pool first,
** failing over to the full-size pool if that does not work.  Thus more
** lookaside slots are available while also using less memory.
** This enhancement can be omitted by compiling with
** CAPDB_OMIT_TWOSIZE_LOOKASIDE.
*/
struct Lookaside {
  u32 bDisable;           /* Only operate the lookaside when zero */
  u16 sz;                 /* Size of each buffer in bytes */
  u16 szTrue;             /* True value of sz, even if disabled */
  u8 bMalloced;           /* True if pStart obtained from capdb_malloc() */
  u32 nSlot;              /* Number of lookaside slots allocated */
  u32 anStat[3];          /* 0: hits.  1: size misses.  2: full misses */
  LookasideSlot *pInit;   /* List of buffers not previously used */
  LookasideSlot *pFree;   /* List of available buffers */
#ifndef CAPDB_OMIT_TWOSIZE_LOOKASIDE
  LookasideSlot *pSmallInit; /* List of small buffers not previously used */
  LookasideSlot *pSmallFree; /* List of available small buffers */
  void *pMiddle;          /* First byte past end of full-size buffers and
                          ** the first byte of LOOKASIDE_SMALL buffers */
#endif /* CAPDB_OMIT_TWOSIZE_LOOKASIDE */
  void *pStart;           /* First byte of available memory space */
  void *pEnd;             /* First byte past end of available space */
  void *pTrueEnd;         /* True value of pEnd, when db->pnBytesFreed!=0 */
};
struct LookasideSlot {
  LookasideSlot *pNext;    /* Next buffer in the list of free buffers */
};

#define DisableLookaside  db->lookaside.bDisable++;db->lookaside.sz=0
#define EnableLookaside   db->lookaside.bDisable--;\
   db->lookaside.sz=db->lookaside.bDisable?0:db->lookaside.szTrue

/* Size of the smaller allocations in two-size lookaside */
#ifdef CAPDB_OMIT_TWOSIZE_LOOKASIDE
#  define LOOKASIDE_SMALL           0
#else
#  define LOOKASIDE_SMALL         128
#endif

/*
** A hash table for built-in function definitions.  (Application-defined
** functions use a regular table table from hash.h.)
**
** Hash each FuncDef structure into one of the FuncDefHash.a[] slots.
** Collisions are on the FuncDef.u.pHash chain.  Use the CAPDB_FUNC_HASH()
** macro to compute a hash on the function name.
*/
#define CAPDB_FUNC_HASH_SZ 23
struct FuncDefHash {
  FuncDef *a[CAPDB_FUNC_HASH_SZ];       /* Hash table for functions */
};
#define CAPDB_FUNC_HASH(C,L) (((C)+(L))%CAPDB_FUNC_HASH_SZ)

/*
** typedef for the authorization callback function.
*/
typedef int (*capdb_xauth)(void*,int,const char*,const char*,const char*,
                             const char*);

#ifndef CAPDB_OMIT_DEPRECATED
/* This is an extra CAPDB_TRACE macro that indicates "legacy" tracing
** in the style of capdb_trace()
*/
#define CAPDB_TRACE_LEGACY          0x40     /* Use the legacy xTrace */
#define CAPDB_TRACE_XPROFILE        0x80     /* Use the legacy xProfile */
#else
#define CAPDB_TRACE_LEGACY          0
#define CAPDB_TRACE_XPROFILE        0
#endif /* CAPDB_OMIT_DEPRECATED */
#define CAPDB_TRACE_NONLEGACY_MASK  0x0f     /* Normal flags */

/*
** Maximum number of capdb.aDb[] entries.  This is the number of attached
** databases plus 2 for "main" and "temp".
*/
#define CAPDB_MAX_DB (CAPDB_MAX_ATTACHED+2)

/*
** Each database connection is an instance of the following structure.
*/
struct capdb {
  capdb_vfs *pVfs;            /* OS Interface */
  struct Vdbe *pVdbe;           /* List of active virtual machines */
  CollSeq *pDfltColl;           /* BINARY collseq for the database encoding */
  capdb_mutex *mutex;         /* Connection mutex */
  Db *aDb;                      /* All backends */
  int nDb;                      /* Number of backends currently in use */
  u32 mDbFlags;                 /* flags recording internal state */
  u64 flags;                    /* flags settable by pragmas. See below */
  i64 lastRowid;                /* ROWID of most recent insert (see above) */
  i64 szMmap;                   /* Default mmap_size setting */
  u32 nSchemaLock;              /* Do not reset the schema when non-zero */
  unsigned int openFlags;       /* Flags passed to capdb_vfs.xOpen() */
  int errCode;                  /* Most recent error code (CAPDB_*) */
  int errByteOffset;            /* Byte offset of error in SQL statement */
  int errMask;                  /* & result codes with this before returning */
  int iSysErrno;                /* Errno value from last system error */
  u32 dbOptFlags;               /* Flags to enable/disable optimizations */
  u8 enc;                       /* Text encoding */
  u8 autoCommit;                /* The auto-commit flag. */
  u8 temp_store;                /* 1: file 2: memory 0: default */
  u8 mallocFailed;              /* True if we have seen a malloc failure */
  u8 bBenignMalloc;             /* Do not require OOMs if true */
  u8 dfltLockMode;              /* Default locking-mode for attached dbs */
  signed char nextAutovac;      /* Autovac setting after VACUUM if >=0 */
  u8 suppressErr;               /* Do not issue error messages if true */
  u8 vtabOnConflict;            /* Value to return for s3_vtab_on_conflict() */
  u8 isTransactionSavepoint;    /* True if the outermost savepoint is a TS */
  u8 mTrace;                    /* zero or more CAPDB_TRACE flags */
  u8 noSharedCache;             /* True if no shared-cache backends */
  u8 nSqlExec;                  /* Number of pending OP_SqlExec opcodes */
  u8 eOpenState;                /* Current condition of the connection */
  u8 nFpDigit;                  /* Significant digits to keep on double->text */
  int nextPagesize;             /* Pagesize after VACUUM if >0 */
  i64 nChange;                  /* Value returned by capdb_changes() */
  i64 nTotalChange;             /* Value returned by capdb_total_changes() */
  int aLimit[CAPDB_N_LIMIT];   /* Limits */
  int nMaxSorterMmap;           /* Maximum size of regions mapped by sorter */
  struct capdbInitInfo {      /* Information used during initialization */
    Pgno newTnum;               /* Rootpage of table being initialized */
    u8 iDb;                     /* Which db file is being initialized */
    u8 busy;                    /* TRUE if initing. 2 if due to ADD COLUMN */
    unsigned orphanTrigger : 1; /* Last statement is orphaned TEMP trigger */
    unsigned imposterTable : 2; /* Building an imposter table */
    unsigned reopenMemdb : 1;   /* ATTACH is really a reopen using MemDB */
    const char **azInit;        /* "type", "name", and "tbl_name" columns */
  } init;
  int nVdbeActive;              /* Number of VDBEs currently running */
  int nVdbeRead;                /* Number of active VDBEs that read or write */
  int nVdbeWrite;               /* Number of active VDBEs that read and write */
  int nVdbeExec;                /* Number of nested calls to VdbeExec() */
  int nVDestroy;                /* Number of active OP_VDestroy operations */
  int nExtension;               /* Number of loaded extensions */
  void **aExtension;            /* Array of shared library handles */
  union {
    void (*xLegacy)(void*,const char*);   /* mTrace==CAPDB_TRACE_LEGACY */
    int (*xV2)(u32,void*,void*,void*);    /* All other mTrace values */
  } trace;
  void *pTraceArg;                        /* Argument to the trace function */
#ifndef CAPDB_OMIT_DEPRECATED
  void (*xProfile)(void*,const char*,u64);  /* Profiling function */
  void *pProfileArg;                        /* Argument to profile function */
#endif
  void *pCommitArg;                 /* Argument to xCommitCallback() */
  int (*xCommitCallback)(void*);    /* Invoked at every commit. */
  void *pRollbackArg;               /* Argument to xRollbackCallback() */
  void (*xRollbackCallback)(void*); /* Invoked at every commit. */
  void *pUpdateArg;
  void (*xUpdateCallback)(void*,int, const char*,const char*,sqlite_int64);
  void *pAutovacPagesArg;           /* Client argument to autovac_pages */
  void (*xAutovacDestr)(void*);     /* Destructor for pAutovacPAgesArg */
  unsigned int (*xAutovacPages)(void*,const char*,u32,u32,u32);
  Parse *pParse;                /* Current parse */
#ifdef CAPDB_ENABLE_PREUPDATE_HOOK
  void *pPreUpdateArg;          /* First argument to xPreUpdateCallback */
  void (*xPreUpdateCallback)(   /* Registered using capdb_preupdate_hook() */
    void*,capdb*,int,char const*,char const*,capdb_int64,capdb_int64
  );
  PreUpdate *pPreUpdate;        /* Context for active pre-update callback */
#endif /* CAPDB_ENABLE_PREUPDATE_HOOK */
#ifndef CAPDB_OMIT_WAL
  int (*xWalCallback)(void *, capdb *, const char *, int);
  void *pWalArg;
#endif
  void(*xCollNeeded)(void*,capdb*,int eTextRep,const char*);
  void(*xCollNeeded16)(void*,capdb*,int eTextRep,const void*);
  void *pCollNeededArg;
  capdb_value *pErr;          /* Most recent error message */
  union {
    volatile int isInterrupted; /* True if capdb_interrupt has been called */
    double notUsed1;            /* Spacer */
  } u1;
  Lookaside lookaside;          /* Lookaside malloc configuration */
#ifndef CAPDB_OMIT_AUTHORIZATION
  capdb_xauth xAuth;          /* Access authorization function */
  void *pAuthArg;               /* 1st argument to the access auth function */
#endif
#ifndef CAPDB_OMIT_PROGRESS_CALLBACK
  int (*xProgress)(void *);     /* The progress callback */
  void *pProgressArg;           /* Argument to the progress callback */
  unsigned nProgressOps;        /* Number of opcodes for progress callback */
#endif
#ifndef CAPDB_OMIT_VIRTUALTABLE
  int nVTrans;                  /* Allocated size of aVTrans */
  Hash aModule;                 /* populated by capdb_create_module() */
  VtabCtx *pVtabCtx;            /* Context for active vtab connect/create */
  VTable **aVTrans;             /* Virtual tables with open transactions */
  VTable *pDisconnect;          /* Disconnect these in next capdb_prepare() */
#endif
  Hash aFunc;                   /* Hash table of connection functions */
  Hash aCollSeq;                /* All collating sequences */
  BusyHandler busyHandler;      /* Busy callback */
  Db aDbStatic[2];              /* Static space for the 2 default backends */
  Savepoint *pSavepoint;        /* List of active savepoints */
  int nAnalysisLimit;           /* Number of index rows to ANALYZE */
  int busyTimeout;              /* Busy handler timeout, in msec */
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  int setlkTimeout;             /* Blocking lock timeout, in msec. -1 -> inf. */
  int setlkFlags;               /* Flags passed to setlk_timeout() */
#endif
  int nSavepoint;               /* Number of non-transaction savepoints */
  int nStatement;               /* Number of nested statement-transactions  */
  i64 nDeferredCons;            /* Net deferred constraints this transaction. */
  i64 nDeferredImmCons;         /* Net deferred immediate constraints */
  int *pnBytesFreed;            /* If not NULL, increment this in DbFree() */
  DbClientData *pDbData;        /* capdb_set_clientdata() content */
  u64 nSpill;                   /* TEMP content spilled to disk */
#ifdef CAPDB_ENABLE_UNLOCK_NOTIFY
  /* The following variables are all protected by the STATIC_MAIN
  ** mutex, not by capdb.mutex. They are used by code in notify.c.
  **
  ** When X.pUnlockConnection==Y, that means that X is waiting for Y to
  ** unlock so that it can proceed.
  **
  ** When X.pBlockingConnection==Y, that means that something that X tried
  ** tried to do recently failed with an CAPDB_LOCKED error due to locks
  ** held by Y.
  */
  capdb *pBlockingConnection; /* Connection that caused CAPDB_LOCKED */
  capdb *pUnlockConnection;           /* Connection to watch for unlock */
  void *pUnlockArg;                     /* Argument to xUnlockNotify */
  void (*xUnlockNotify)(void **, int);  /* Unlock notify callback */
  capdb *pNextBlocked;        /* Next in list of all blocked connections */
#endif
};

/*
** A macro to discover the encoding of a database.
*/
#define SCHEMA_ENC(db) ((db)->aDb[0].pSchema->enc)
#define ENC(db)        ((db)->enc)

/*
** A u64 constant where the lower 32 bits are all zeros.  Only the
** upper 32 bits are included in the argument.  Necessary because some
** C-compilers still do not accept LL integer literals.
*/
#define HI(X)  ((u64)(X)<<32)

/*
** Possible values for the capdb.flags.
**
** Value constraints (enforced via assert()):
**      CAPDB_FullFSync     == PAGER_FULLFSYNC
**      CAPDB_CkptFullFSync == PAGER_CKPT_FULLFSYNC
**      CAPDB_CacheSpill    == PAGER_CACHE_SPILL
*/
#define CAPDB_WriteSchema    0x00000001  /* OK to update CAPDB_SCHEMA */
#define CAPDB_LegacyFileFmt  0x00000002  /* Create new databases in format 1 */
#define CAPDB_FullColNames   0x00000004  /* Show full column names on SELECT */
#define CAPDB_FullFSync      0x00000008  /* Use full fsync on the backend */
#define CAPDB_CkptFullFSync  0x00000010  /* Use full fsync for checkpoint */
#define CAPDB_CacheSpill     0x00000020  /* OK to spill pager cache */
#define CAPDB_ShortColNames  0x00000040  /* Show short columns names */
#define CAPDB_TrustedSchema  0x00000080  /* Allow unsafe functions and
                                          ** vtabs in the schema definition */
#define CAPDB_NullCallback   0x00000100  /* Invoke the callback once if the */
                                          /*   result set is empty */
#define CAPDB_IgnoreChecks   0x00000200  /* Do not enforce check constraints */
#define CAPDB_StmtScanStatus 0x00000400  /* Enable stmt_scanstats() counters */
#define CAPDB_NoCkptOnClose  0x00000800  /* No checkpoint on close()/DETACH */
#define CAPDB_ReverseOrder   0x00001000  /* Reverse unordered SELECTs */
#define CAPDB_RecTriggers    0x00002000  /* Enable recursive triggers */
#define CAPDB_ForeignKeys    0x00004000  /* Enforce foreign key constraints  */
#define CAPDB_AutoIndex      0x00008000  /* Enable automatic indexes */
#define CAPDB_LoadExtension  0x00010000  /* Enable load_extension */
#define CAPDB_LoadExtFunc    0x00020000  /* Enable load_extension() SQL func */
#define CAPDB_EnableTrigger  0x00040000  /* True to enable triggers */
#define CAPDB_DeferFKs       0x00080000  /* Defer all FK constraints */
#define CAPDB_QueryOnly      0x00100000  /* Disable database changes */
#define CAPDB_CellSizeCk     0x00200000  /* Check btree cell sizes on load */
#define CAPDB_Fts3Tokenizer  0x00400000  /* Enable fts3_tokenizer(2) */
#define CAPDB_EnableQPSG     0x00800000  /* Query Planner Stability Guarantee*/
#define CAPDB_TriggerEQP     0x01000000  /* Show trigger EXPLAIN QUERY PLAN */
#define CAPDB_ResetDatabase  0x02000000  /* Reset the database */
#define CAPDB_LegacyAlter    0x04000000  /* Legacy ALTER TABLE behaviour */
#define CAPDB_NoSchemaError  0x08000000  /* Do not report schema parse errors*/
#define CAPDB_Defensive      0x10000000  /* Input SQL is likely hostile */
#define CAPDB_DqsDDL         0x20000000  /* dbl-quoted strings allowed in DDL*/
#define CAPDB_DqsDML         0x40000000  /* dbl-quoted strings allowed in DML*/
#define CAPDB_EnableView     0x80000000  /* Enable the use of views */
#define CAPDB_CountRows      HI(0x00001) /* Count rows changed by INSERT, */
                                          /*   DELETE, or UPDATE and return */
                                          /*   the count using a callback. */
#define CAPDB_CorruptRdOnly  HI(0x00002) /* Prohibit writes due to error */
#define CAPDB_ReadUncommit   HI(0x00004) /* READ UNCOMMITTED in shared-cache */
#define CAPDB_FkNoAction     HI(0x00008) /* Treat all FK as NO ACTION */
#define CAPDB_AttachCreate   HI(0x00010) /* ATTACH allowed to create new dbs */
#define CAPDB_AttachWrite    HI(0x00020) /* ATTACH allowed to open for write */
#define CAPDB_Comments       HI(0x00040) /* Enable SQL comments */

/* Flags used only if debugging */
#ifdef CAPDB_DEBUG
#define CAPDB_SqlTrace       HI(0x0100000) /* Debug print SQL as it executes */
#define CAPDB_VdbeListing    HI(0x0200000) /* Debug listings of VDBE progs */
#define CAPDB_VdbeTrace      HI(0x0400000) /* True to trace VDBE execution */
#define CAPDB_VdbeAddopTrace HI(0x0800000) /* Trace capdbVdbeAddOp() calls */
#define CAPDB_VdbeEQP        HI(0x1000000) /* Debug EXPLAIN QUERY PLAN */
#define CAPDB_ParserTrace    HI(0x2000000) /* PRAGMA parser_trace=ON */
#endif

/*
** Allowed values for capdb.mDbFlags
*/
#define DBFLAG_SchemaChange   0x0001  /* Uncommitted Hash table changes */
#define DBFLAG_PreferBuiltin  0x0002  /* Preference to built-in funcs */
#define DBFLAG_Vacuum         0x0004  /* Currently in a VACUUM */
#define DBFLAG_VacuumInto     0x0008  /* Currently running VACUUM INTO */
#define DBFLAG_SchemaKnownOk  0x0010  /* Schema is known to be valid */
#define DBFLAG_InternalFunc   0x0020  /* Allow use of internal functions */
#define DBFLAG_EncodingFixed  0x0040  /* No longer possible to change enc. */

/*
** Bits of the capdb.dbOptFlags field that are used by the
** capdb_test_control(CAPDB_TESTCTRL_OPTIMIZATIONS,...) interface to
** selectively disable various optimizations.
*/
#define CAPDB_QueryFlattener 0x00000001 /* Query flattening */
#define CAPDB_WindowFunc     0x00000002 /* Use xInverse for window functions */
#define CAPDB_GroupByOrder   0x00000004 /* GROUPBY cover of ORDERBY */
#define CAPDB_FactorOutConst 0x00000008 /* Constant factoring */
#define CAPDB_DistinctOpt    0x00000010 /* DISTINCT using indexes */
#define CAPDB_CoverIdxScan   0x00000020 /* Covering index scans */
#define CAPDB_OrderByIdxJoin 0x00000040 /* ORDER BY of joins via index */
#define CAPDB_Transitive     0x00000080 /* Transitive constraints */
#define CAPDB_OmitNoopJoin   0x00000100 /* Omit unused tables in joins */
#define CAPDB_CountOfView    0x00000200 /* The count-of-view optimization */
#define CAPDB_CursorHints    0x00000400 /* Add OP_CursorHint opcodes */
#define CAPDB_Stat4          0x00000800 /* Use STAT4 data */
   /* TH3 expects this value  ^^^^^^^^^^ to be 0x0000800. Don't change it */
#define CAPDB_PushDown       0x00001000 /* WHERE-clause push-down opt */
#define CAPDB_SimplifyJoin   0x00002000 /* Convert LEFT JOIN to JOIN */
#define CAPDB_SkipScan       0x00004000 /* Skip-scans */
#define CAPDB_PropagateConst 0x00008000 /* The constant propagation opt */
#define CAPDB_MinMaxOpt      0x00010000 /* The min/max optimization */
#define CAPDB_SeekScan       0x00020000 /* The OP_SeekScan optimization */
#define CAPDB_OmitOrderBy    0x00040000 /* Omit pointless ORDER BY */
   /* TH3 expects this value  ^^^^^^^^^^ to be 0x40000. Coordinate any change */
#define CAPDB_BloomFilter    0x00080000 /* Use a Bloom filter on searches */
#define CAPDB_BloomPulldown  0x00100000 /* Run Bloom filters early */
#define CAPDB_BalancedMerge  0x00200000 /* Balance multi-way merges */
#define CAPDB_ReleaseReg     0x00400000 /* Use OP_ReleaseReg for testing */
#define CAPDB_FlttnUnionAll  0x00800000 /* Disable the UNION ALL flattener */
   /* TH3 expects this value  ^^^^^^^^^^ See flatten04.test */
#define CAPDB_IndexedExpr    0x01000000 /* Pull exprs from index when able */
#define CAPDB_Coroutines     0x02000000 /* Co-routines for subqueries */
#define CAPDB_NullUnusedCols 0x04000000 /* NULL unused columns in subqueries */
#define CAPDB_OnePass        0x08000000 /* Single-pass DELETE and UPDATE */
#define CAPDB_OrderBySubq    0x10000000 /* ORDER BY in subquery helps outer */
#define CAPDB_StarQuery      0x20000000 /* Heurists for star queries */
#define CAPDB_ExistsToJoin   0x40000000 /* The EXISTS-to-JOIN optimization */
#define CAPDB_AllOpts        0xffffffff /* All optimizations */

/*
** Macros for testing whether or not optimizations are enabled or disabled.
*/
#define OptimizationDisabled(db, mask)  (((db)->dbOptFlags&(mask))!=0)
#define OptimizationEnabled(db, mask)   (((db)->dbOptFlags&(mask))==0)

/*
** Return true if it OK to factor constant expressions into the initialization
** code. The argument is a Parse object for the code generator.
*/
#define ConstFactorOk(P) ((P)->okConstFactor)

/* Possible values for the capdb.eOpenState field.
** The numbers are randomly selected such that a minimum of three bits must
** change to convert any number to another or to zero
*/
#define CAPDB_STATE_OPEN     0x76  /* Database is open */
#define CAPDB_STATE_CLOSED   0xce  /* Database is closed */
#define CAPDB_STATE_SICK     0xba  /* Error and awaiting close */
#define CAPDB_STATE_BUSY     0x6d  /* Database currently in use */
#define CAPDB_STATE_ERROR    0xd5  /* An CAPDB_MISUSE error occurred */
#define CAPDB_STATE_ZOMBIE   0xa7  /* Close with last statement close */

/*
** Each SQL function is defined by an instance of the following
** structure.  For global built-in functions (ex: substr(), max(), count())
** a pointer to this structure is held in the capdbBuiltinFunctions object.
** For per-connection application-defined functions, a pointer to this
** structure is held in the db->aHash hash table.
**
** The u.pHash field is used by the global built-ins.  The u.pDestructor
** field is used by per-connection app-def functions.
*/
struct FuncDef {
  i16 nArg;            /* Number of arguments.  -1 means unlimited */
  u32 funcFlags;       /* Some combination of CAPDB_FUNC_* */
  void *pUserData;     /* User data parameter */
  FuncDef *pNext;      /* Next function with same name */
  void (*xSFunc)(capdb_context*,int,capdb_value**); /* func or agg-step */
  void (*xFinalize)(capdb_context*);                  /* Agg finalizer */
  void (*xValue)(capdb_context*);                     /* Current agg value */
  void (*xInverse)(capdb_context*,int,capdb_value**); /* inverse agg-step */
  const char *zName;   /* SQL name of the function. */
  union {
    FuncDef *pHash;      /* Next with a different name but the same hash */
    FuncDestructor *pDestructor;   /* Reference counted destructor function */
  } u; /* pHash if CAPDB_FUNC_BUILTIN, pDestructor otherwise */
};

/*
** This structure encapsulates a user-function destructor callback (as
** configured using create_function_v2()) and a reference counter. When
** create_function_v2() is called to create a function with a destructor,
** a single object of this type is allocated. FuncDestructor.nRef is set to
** the number of FuncDef objects created (either 1 or 3, depending on whether
** or not the specified encoding is CAPDB_ANY). The FuncDef.pDestructor
** member of each of the new FuncDef objects is set to point to the allocated
** FuncDestructor.
**
** Thereafter, when one of the FuncDef objects is deleted, the reference
** count on this object is decremented. When it reaches 0, the destructor
** is invoked and the FuncDestructor structure freed.
*/
struct FuncDestructor {
  int nRef;
  void (*xDestroy)(void *);
  void *pUserData;
};

/*
** Possible values for FuncDef.flags.  Note that the _LENGTH and _TYPEOF
** values must correspond to OPFLAG_LENGTHARG and OPFLAG_TYPEOFARG.  And
** CAPDB_FUNC_CONSTANT must be the same as CAPDB_DETERMINISTIC.  There
** are assert() statements in the code to verify this.
**
** Value constraints (enforced via assert()):
**     CAPDB_FUNC_MINMAX      ==  NC_MinMaxAgg      == SF_MinMaxAgg
**     CAPDB_FUNC_ANYORDER    ==  NC_OrderAgg       == SF_OrderByReqd
**     CAPDB_FUNC_LENGTH      ==  OPFLAG_LENGTHARG
**     CAPDB_FUNC_TYPEOF      ==  OPFLAG_TYPEOFARG
**     CAPDB_FUNC_BYTELEN     ==  OPFLAG_BYTELENARG
**     CAPDB_FUNC_CONSTANT    ==  CAPDB_DETERMINISTIC from the API
**     CAPDB_FUNC_DIRECT      ==  CAPDB_DIRECTONLY from the API
**     CAPDB_FUNC_UNSAFE      ==  CAPDB_INNOCUOUS  -- opposite meanings!!!
**     CAPDB_FUNC_ENCMASK   depends on CAPDB_UTF* macros in the API
**
** Note that even though CAPDB_FUNC_UNSAFE and CAPDB_INNOCUOUS have the
** same bit value, their meanings are inverted.  CAPDB_FUNC_UNSAFE is
** used internally and if set means that the function has side effects.
** CAPDB_INNOCUOUS is used by application code and means "not unsafe".
** See multiple instances of tag-20230109-1.
*/
#define CAPDB_FUNC_ENCMASK  0x0003 /* CAPDB_UTF8, CAPDB_UTF16BE or UTF16LE */
#define CAPDB_FUNC_LIKE     0x0004 /* Candidate for the LIKE optimization */
#define CAPDB_FUNC_CASE     0x0008 /* Case-sensitive LIKE-type function */
#define CAPDB_FUNC_EPHEM    0x0010 /* Ephemeral.  Delete with VDBE */
#define CAPDB_FUNC_NEEDCOLL 0x0020 /* capdbGetFuncCollSeq() might be called*/
#define CAPDB_FUNC_LENGTH   0x0040 /* Built-in length() function */
#define CAPDB_FUNC_TYPEOF   0x0080 /* Built-in typeof() function */
#define CAPDB_FUNC_BYTELEN  0x00c0 /* Built-in octet_length() function */
#define CAPDB_FUNC_COUNT    0x0100 /* Built-in count(*) aggregate */
/*                           0x0200 -- available for reuse */
#define CAPDB_FUNC_UNLIKELY 0x0400 /* Built-in unlikely() function */
#define CAPDB_FUNC_CONSTANT 0x0800 /* Constant inputs give a constant output */
#define CAPDB_FUNC_MINMAX   0x1000 /* True for min() and max() aggregates */
#define CAPDB_FUNC_SLOCHNG  0x2000 /* "Slow Change". Value constant during a
                                    ** single query - might change over time */
#define CAPDB_FUNC_TEST     0x4000 /* Built-in testing functions */
#define CAPDB_FUNC_RUNONLY  0x8000 /* Cannot be used by valueFromFunction */
#define CAPDB_FUNC_WINDOW   0x00010000 /* Built-in window-only function */
#define CAPDB_FUNC_INTERNAL 0x00040000 /* For use by NestedParse() only */
#define CAPDB_FUNC_DIRECT   0x00080000 /* Not for use in TRIGGERs or VIEWs */
/* CAPDB_SUBTYPE            0x00100000 // Consumer of subtypes */
#define CAPDB_FUNC_UNSAFE   0x00200000 /* Function has side effects */
#define CAPDB_FUNC_INLINE   0x00400000 /* Functions implemented in-line */
#define CAPDB_FUNC_BUILTIN  0x00800000 /* This is a built-in function */
/*  CAPDB_RESULT_SUBTYPE    0x01000000 // Generator of subtypes */
#define CAPDB_FUNC_ANYORDER 0x08000000 /* count/min/max aggregate */

/* Identifier numbers for each in-line function */
#define INLINEFUNC_coalesce             0
#define INLINEFUNC_implies_nonnull_row  1
#define INLINEFUNC_expr_implies_expr    2
#define INLINEFUNC_expr_compare         3
#define INLINEFUNC_affinity             4
#define INLINEFUNC_iif                  5
#define INLINEFUNC_sqlite_offset        6
#define INLINEFUNC_unlikely            99  /* Default case */

/*
** The following three macros, FUNCTION(), LIKEFUNC() and AGGREGATE() are
** used to create the initializers for the FuncDef structures.
**
**   FUNCTION(zName, nArg, iArg, bNC, xFunc)
**     Used to create a scalar function definition of a function zName
**     implemented by C function xFunc that accepts nArg arguments. The
**     value passed as iArg is cast to a (void*) and made available
**     as the user-data (capdb_user_data()) for the function. If
**     argument bNC is true, then the CAPDB_FUNC_NEEDCOLL flag is set.
**
**   VFUNCTION(zName, nArg, iArg, bNC, xFunc)
**     Like FUNCTION except it omits the CAPDB_FUNC_CONSTANT flag.
**
**   SFUNCTION(zName, nArg, iArg, bNC, xFunc)
**     Like FUNCTION except it omits the CAPDB_FUNC_CONSTANT flag and
**     adds the CAPDB_DIRECTONLY flag.
**
**   INLINE_FUNC(zName, nArg, iFuncId, mFlags)
**     zName is the name of a function that is implemented by in-line
**     byte code rather than by the usual callbacks. The iFuncId
**     parameter determines the function id.  The mFlags parameter is
**     optional CAPDB_FUNC_ flags for this function.
**
**   TEST_FUNC(zName, nArg, iFuncId, mFlags)
**     zName is the name of a test-only function implemented by in-line
**     byte code rather than by the usual callbacks. The iFuncId
**     parameter determines the function id.  The mFlags parameter is
**     optional CAPDB_FUNC_ flags for this function.
**
**   DFUNCTION(zName, nArg, iArg, bNC, xFunc)
**     Like FUNCTION except it omits the CAPDB_FUNC_CONSTANT flag and
**     adds the CAPDB_FUNC_SLOCHNG flag.  Used for date & time functions
**     and functions like sqlite_version() that can change, but not during
**     a single query.  The iArg is ignored.  The user-data is always set
**     to a NULL pointer.  The bNC parameter is not used.
**
**   MFUNCTION(zName, nArg, xPtr, xFunc)
**     For math-library functions.  xPtr is an arbitrary pointer.
**
**   PURE_DATE(zName, nArg, iArg, bNC, xFunc)
**     Used for "pure" date/time functions, this macro is like DFUNCTION
**     except that it does set the CAPDB_FUNC_CONSTANT flags.  iArg is
**     ignored and the user-data for these functions is set to an
**     arbitrary non-NULL pointer.  The bNC parameter is not used.
**
**   AGGREGATE(zName, nArg, iArg, bNC, xStep, xFinal)
**     Used to create an aggregate function definition implemented by
**     the C functions xStep and xFinal. The first four parameters
**     are interpreted in the same way as the first 4 parameters to
**     FUNCTION().
**
**   WAGGREGATE(zName, nArg, iArg, xStep, xFinal, xValue, xInverse)
**     Used to create an aggregate function definition implemented by
**     the C functions xStep and xFinal. The first four parameters
**     are interpreted in the same way as the first 4 parameters to
**     FUNCTION().
**
**   LIKEFUNC(zName, nArg, pArg, flags)
**     Used to create a scalar function definition of a function zName
**     that accepts nArg arguments and is implemented by a call to C
**     function likeFunc. Argument pArg is cast to a (void *) and made
**     available as the function user-data (capdb_user_data()). The
**     FuncDef.flags variable is set to the value passed as the flags
**     parameter.
*/
#define FUNCTION(zName, nArg, iArg, bNC, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|\
   CAPDB_FUNC_CONSTANT|CAPDB_UTF8|(bNC*CAPDB_FUNC_NEEDCOLL), \
   CAPDB_INT_TO_PTR(iArg), 0, xFunc, 0, 0, 0, #zName, {0} }
#define VFUNCTION(zName, nArg, iArg, bNC, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_UTF8|(bNC*CAPDB_FUNC_NEEDCOLL), \
   CAPDB_INT_TO_PTR(iArg), 0, xFunc, 0, 0, 0, #zName, {0} }
#define SFUNCTION(zName, nArg, iArg, bNC, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_UTF8|CAPDB_DIRECTONLY|CAPDB_FUNC_UNSAFE, \
   CAPDB_INT_TO_PTR(iArg), 0, xFunc, 0, 0, 0, #zName, {0} }
#define MFUNCTION(zName, nArg, xPtr, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_FUNC_CONSTANT|CAPDB_UTF8, \
   xPtr, 0, xFunc, 0, 0, 0, #zName, {0} }
#define JFUNCTION(zName, nArg, bUseCache, bWS, bRS, bJsonB, iArg, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_DETERMINISTIC|CAPDB_FUNC_CONSTANT|\
   CAPDB_UTF8|((bUseCache)*CAPDB_FUNC_RUNONLY)|\
   ((bRS)*CAPDB_SUBTYPE)|((bWS)*CAPDB_RESULT_SUBTYPE), \
   CAPDB_INT_TO_PTR(iArg|((bJsonB)*JSON_BLOB)),0,xFunc,0, 0, 0, #zName, {0} }
#define INLINE_FUNC(zName, nArg, iArg, mFlags) \
  {nArg, CAPDB_FUNC_BUILTIN|\
   CAPDB_UTF8|CAPDB_FUNC_INLINE|CAPDB_FUNC_CONSTANT|(mFlags), \
   CAPDB_INT_TO_PTR(iArg), 0, noopFunc, 0, 0, 0, #zName, {0} }
#define TEST_FUNC(zName, nArg, iArg, mFlags) \
  {nArg, CAPDB_FUNC_BUILTIN|\
         CAPDB_UTF8|CAPDB_FUNC_INTERNAL|CAPDB_FUNC_TEST| \
         CAPDB_FUNC_INLINE|CAPDB_FUNC_CONSTANT|(mFlags), \
   CAPDB_INT_TO_PTR(iArg), 0, noopFunc, 0, 0, 0, #zName, {0} }
#define DFUNCTION(zName, nArg, iArg, bNC, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_FUNC_SLOCHNG|CAPDB_UTF8, \
   0, 0, xFunc, 0, 0, 0, #zName, {0} }
#define PURE_DATE(zName, nArg, iArg, bNC, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|\
         CAPDB_FUNC_SLOCHNG|CAPDB_UTF8|CAPDB_FUNC_CONSTANT, \
   (void*)&capdbConfig, 0, xFunc, 0, 0, 0, #zName, {0} }
#define FUNCTION2(zName, nArg, iArg, bNC, xFunc, extraFlags) \
  {nArg, CAPDB_FUNC_BUILTIN|\
   CAPDB_FUNC_CONSTANT|CAPDB_UTF8|(bNC*CAPDB_FUNC_NEEDCOLL)|extraFlags,\
   CAPDB_INT_TO_PTR(iArg), 0, xFunc, 0, 0, 0, #zName, {0} }
#define STR_FUNCTION(zName, nArg, pArg, bNC, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|\
   CAPDB_FUNC_SLOCHNG|CAPDB_UTF8|(bNC*CAPDB_FUNC_NEEDCOLL), \
   pArg, 0, xFunc, 0, 0, 0, #zName, {0} }
#define LIKEFUNC(zName, nArg, arg, flags) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_FUNC_CONSTANT|CAPDB_UTF8|flags, \
   (void *)arg, 0, likeFunc, 0, 0, 0, #zName, {0} }
#define WAGGREGATE(zName, nArg, arg, nc, xStep, xFinal, xValue, xInverse, f) \
  {nArg, CAPDB_FUNC_BUILTIN|CAPDB_UTF8|(nc*CAPDB_FUNC_NEEDCOLL)|f, \
   CAPDB_INT_TO_PTR(arg), 0, xStep,xFinal,xValue,xInverse,#zName, {0}}
#define INTERNAL_FUNCTION(zName, nArg, xFunc) \
  {nArg, CAPDB_FUNC_BUILTIN|\
   CAPDB_FUNC_INTERNAL|CAPDB_UTF8|CAPDB_FUNC_CONSTANT, \
   0, 0, xFunc, 0, 0, 0, #zName, {0} }


/*
** All current savepoints are stored in a linked list starting at
** capdb.pSavepoint. The first element in the list is the most recently
** opened savepoint. Savepoints are added to the list by the vdbe
** OP_Savepoint instruction.
*/
struct Savepoint {
  char *zName;                        /* Savepoint name (nul-terminated) */
  i64 nDeferredCons;                  /* Number of deferred fk violations */
  i64 nDeferredImmCons;               /* Number of deferred imm fk. */
  Savepoint *pNext;                   /* Parent savepoint (if any) */
};

/*
** The following are used as the second parameter to capdbSavepoint(),
** and as the P1 argument to the OP_Savepoint instruction.
*/
#define SAVEPOINT_BEGIN      0
#define SAVEPOINT_RELEASE    1
#define SAVEPOINT_ROLLBACK   2


/*
** Each SQLite module (virtual table definition) is defined by an
** instance of the following structure, stored in the capdb.aModule
** hash table.
*/
struct Module {
  const capdb_module *pModule;       /* Callback pointers */
  const char *zName;                   /* Name passed to create_module() */
  int nRefModule;                      /* Number of pointers to this object */
  void *pAux;                          /* pAux passed to create_module() */
  void (*xDestroy)(void *);            /* Module destructor function */
  Table *pEpoTab;                      /* Eponymous table for this module */
};

/*
** Information about each column of an SQL table is held in an instance
** of the Column structure, in the Table.aCol[] array.
**
** Definitions:
**
**   "table column index"     This is the index of the column in the
**                            Table.aCol[] array, and also the index of
**                            the column in the original CREATE TABLE stmt.
**
**   "storage column index"   This is the index of the column in the
**                            record BLOB generated by the OP_MakeRecord
**                            opcode.  The storage column index is less than
**                            or equal to the table column index.  It is
**                            equal if and only if there are no VIRTUAL
**                            columns to the left.
**
** Notes on zCnName:
** The zCnName field stores the name of the column, the datatype of the
** column, and the collating sequence for the column, in that order, all in
** a single allocation.  Each string is 0x00 terminated.  The datatype
** is only included if the COLFLAG_HASTYPE bit of colFlags is set and the
** collating sequence name is only included if the COLFLAG_HASCOLL bit is
** set.
*/
struct Column {
  char *zCnName;        /* Name of this column */
  unsigned notNull :4;  /* An OE_ code for handling a NOT NULL constraint */
  unsigned eCType :4;   /* One of the standard types */
  char affinity;        /* One of the CAPDB_AFF_... values */
  u8 szEst;             /* Est size of value in this column. sizeof(INT)==1 */
  u8 hName;             /* Column name hash for faster lookup */
  u16 iDflt;            /* 1-based index of DEFAULT.  0 means "none" */
  u16 colFlags;         /* Boolean properties.  See COLFLAG_ defines below */
};

/* Allowed values for Column.eCType.
**
** Values must match entries in the global constant arrays
** capdbStdTypeLen[] and capdbStdType[].  Each value is one more
** than the offset into these arrays for the corresponding name.
** Adjust the CAPDB_N_STDTYPE value if adding or removing entries.
*/
#define COLTYPE_CUSTOM      0   /* Type appended to zName */
#define COLTYPE_ANY         1
#define COLTYPE_BLOB        2
#define COLTYPE_INT         3
#define COLTYPE_INTEGER     4
#define COLTYPE_REAL        5
#define COLTYPE_TEXT        6
#define CAPDB_N_STDTYPE    6  /* Number of standard types */

/* Allowed values for Column.colFlags.
**
** Constraints:
**         TF_HasVirtual == COLFLAG_VIRTUAL
**         TF_HasStored  == COLFLAG_STORED
**         TF_HasHidden  == COLFLAG_HIDDEN
*/
#define COLFLAG_PRIMKEY   0x0001   /* Column is part of the primary key */
#define COLFLAG_HIDDEN    0x0002   /* A hidden column in a virtual table */
#define COLFLAG_HASTYPE   0x0004   /* Type name follows column name */
#define COLFLAG_UNIQUE    0x0008   /* Column def contains "UNIQUE" or "PK" */
#define COLFLAG_SORTERREF 0x0010   /* Use sorter-refs with this column */
#define COLFLAG_VIRTUAL   0x0020   /* GENERATED ALWAYS AS ... VIRTUAL */
#define COLFLAG_STORED    0x0040   /* GENERATED ALWAYS AS ... STORED */
#define COLFLAG_NOTAVAIL  0x0080   /* STORED column not yet calculated */
#define COLFLAG_BUSY      0x0100   /* Blocks recursion on GENERATED columns */
#define COLFLAG_HASCOLL   0x0200   /* Has collating sequence name in zCnName */
#define COLFLAG_NOEXPAND  0x0400   /* Omit this column when expanding "*" */
#define COLFLAG_GENERATED 0x0060   /* Combo: _STORED, _VIRTUAL */
#define COLFLAG_NOINSERT  0x0062   /* Combo: _HIDDEN, _STORED, _VIRTUAL */

/*
** A "Collating Sequence" is defined by an instance of the following
** structure. Conceptually, a collating sequence consists of a name and
** a comparison routine that defines the order of that sequence.
**
** If CollSeq.xCmp is NULL, it means that the
** collating sequence is undefined.  Indices built on an undefined
** collating sequence may not be read or written.
*/
struct CollSeq {
  char *zName;          /* Name of the collating sequence, UTF-8 encoded */
  u8 enc;               /* Text encoding handled by xCmp() */
  void *pUser;          /* First argument to xCmp() */
  int (*xCmp)(void*,int, const void*, int, const void*);
  void (*xDel)(void*);  /* Destructor for pUser */
};

/*
** A sort order can be either ASC or DESC.
*/
#define CAPDB_SO_ASC       0  /* Sort in ascending order */
#define CAPDB_SO_DESC      1  /* Sort in ascending order */
#define CAPDB_SO_UNDEFINED -1 /* No sort order specified */

/*
** Column affinity types.
**
** These used to have mnemonic name like 'i' for CAPDB_AFF_INTEGER and
** 't' for CAPDB_AFF_TEXT.  But we can save a little space and improve
** the speed a little by numbering the values consecutively.
**
** But rather than start with 0 or 1, we begin with 'A'.  That way,
** when multiple affinity types are concatenated into a string and
** used as the P4 operand, they will be more readable.
**
** Note also that the numeric types are grouped together so that testing
** for a numeric type is a single comparison.  And the BLOB type is first.
*/
#define CAPDB_AFF_NONE     0x40  /* '@' */
#define CAPDB_AFF_BLOB     0x41  /* 'A' */
#define CAPDB_AFF_TEXT     0x42  /* 'B' */
#define CAPDB_AFF_NUMERIC  0x43  /* 'C' */
#define CAPDB_AFF_INTEGER  0x44  /* 'D' */
#define CAPDB_AFF_REAL     0x45  /* 'E' */
#define CAPDB_AFF_FLEXNUM  0x46  /* 'F' */
#define CAPDB_AFF_DEFER    0x58  /* 'X'  - defer computation until later */

#define capdbIsNumericAffinity(X)  ((X)>=CAPDB_AFF_NUMERIC)

/*
** The CAPDB_AFF_MASK values masks off the significant bits of an
** affinity value.
*/
#define CAPDB_AFF_MASK     0x47

/*
** Additional bit values that can be ORed with an affinity without
** changing the affinity.
**
** The CAPDB_NOTNULL flag is a combination of NULLEQ and JUMPIFNULL.
** It causes an assert() to fire if either operand to a comparison
** operator is NULL.  It is added to certain comparison operators to
** prove that the operands are always NOT NULL.
*/
#define CAPDB_JUMPIFNULL   0x10  /* jumps if either operand is NULL */
#define CAPDB_NULLEQ       0x80  /* NULL=NULL */
#define CAPDB_NOTNULL      0x90  /* Assert that operands are never NULL */

/*
** An object of this type is created for each virtual table present in
** the database schema.
**
** If the database schema is shared, then there is one instance of this
** structure for each database connection (capdb*) that uses the shared
** schema. This is because each database connection requires its own unique
** instance of the capdb_vtab* handle used to access the virtual table
** implementation. capdb_vtab* handles can not be shared between
** database connections, even when the rest of the in-memory database
** schema is shared, as the implementation often stores the database
** connection handle passed to it via the xConnect() or xCreate() method
** during initialization internally. This database connection handle may
** then be used by the virtual table implementation to access real tables
** within the database. So that they appear as part of the callers
** transaction, these accesses need to be made via the same database
** connection as that used to execute SQL operations on the virtual table.
**
** All VTable objects that correspond to a single table in a shared
** database schema are initially stored in a linked-list pointed to by
** the Table.pVTable member variable of the corresponding Table object.
** When an capdb_prepare() operation is required to access the virtual
** table, it searches the list for the VTable that corresponds to the
** database connection doing the preparing so as to use the correct
** capdb_vtab* handle in the compiled query.
**
** When an in-memory Table object is deleted (for example when the
** schema is being reloaded for some reason), the VTable objects are not
** deleted and the capdb_vtab* handles are not xDisconnect()ed
** immediately. Instead, they are moved from the Table.pVTable list to
** another linked list headed by the capdb.pDisconnect member of the
** corresponding capdb structure. They are then deleted/xDisconnected
** next time a statement is prepared using said capdb*. This is done
** to avoid deadlock issues involving multiple capdb.mutex mutexes.
** Refer to comments above function capdbVtabUnlockList() for an
** explanation as to why it is safe to add an entry to an capdb.pDisconnect
** list without holding the corresponding capdb.mutex mutex.
**
** The memory for objects of this type is always allocated by
** capdbDbMalloc(), using the connection handle stored in VTable.db as
** the first argument.
*/
struct VTable {
  capdb *db;              /* Database connection associated with this table */
  Module *pMod;             /* Pointer to module implementation */
  capdb_vtab *pVtab;      /* Pointer to vtab instance */
  int nRef;                 /* Number of pointers to this structure */
  u8 bConstraint;           /* True if constraints are supported */
  u8 bAllSchemas;           /* True if might use any attached schema */
  u8 eVtabRisk;             /* Riskiness of allowing hacker access */
  int iSavepoint;           /* Depth of the SAVEPOINT stack */
  VTable *pNext;            /* Next in linked list (see above) */
};

/* Allowed values for VTable.eVtabRisk
*/
#define CAPDB_VTABRISK_Low          0
#define CAPDB_VTABRISK_Normal       1
#define CAPDB_VTABRISK_High         2

/*
** The schema for each SQL table, virtual table, and view is represented
** in memory by an instance of the following structure.
*/
struct Table {
  char *zName;         /* Name of the table or view */
  Column *aCol;        /* Information about each column */
  Index *pIndex;       /* List of SQL indexes on this table. */
  char *zColAff;       /* String defining the affinity of each column */
  ExprList *pCheck;    /* All CHECK constraints */
                       /*   ... also used as column name list in a VIEW */
  Pgno tnum;           /* Root BTree page for this table */
  u32 nTabRef;         /* Number of pointers to this Table */
  u32 tabFlags;        /* Mask of TF_* values */
  i16 iPKey;           /* If not negative, use aCol[iPKey] as the rowid */
  i16 nCol;            /* Number of columns in this table */
  i16 nNVCol;          /* Number of columns that are not VIRTUAL */
  LogEst nRowLogEst;   /* Estimated rows in table - from sqlite_stat1 table */
  LogEst szTabRow;     /* Estimated size of each table row in bytes */
#ifdef CAPDB_ENABLE_COSTMULT
  LogEst costMult;     /* Cost multiplier for using this table */
#endif
  u8 keyConf;          /* What to do in case of uniqueness conflict on iPKey */
  u8 eTabType;         /* 0: normal, 1: virtual, 2: view */
  union {
    struct {             /* Used by ordinary tables: */
      int addColOffset;    /* Offset in CREATE TABLE stmt to add a new column */
      FKey *pFKey;         /* Linked list of all foreign keys in this table */
      ExprList *pDfltList; /* DEFAULT clauses on various columns.
                           ** Or the AS clause for generated columns. */
    } tab;
    struct {             /* Used by views: */
      Select *pSelect;     /* View definition */
    } view;
    struct {             /* Used by virtual tables only: */
      int nArg;            /* Number of arguments to the module */
      char **azArg;        /* 0: module 1: schema 2: vtab name 3...: args */
      VTable *p;           /* List of VTable objects. */
    } vtab;
  } u;
  Trigger *pTrigger;   /* List of triggers on this object */
  Schema *pSchema;     /* Schema that contains this table */
  u8 aHx[16];          /* Column aHt[K%sizeof(aHt)] might have hash K */
};

/*
** Allowed values for Table.tabFlags.
**
** TF_OOOHidden applies to tables or view that have hidden columns that are
** followed by non-hidden columns.  Example:  "CREATE VIRTUAL TABLE x USING
** vtab1(a HIDDEN, b);".  Since "b" is a non-hidden column but "a" is hidden,
** the TF_OOOHidden attribute would apply in this case.  Such tables require
** special handling during INSERT processing. The "OOO" means "Out Of Order".
**
** Constraints:
**
**         TF_HasVirtual == COLFLAG_VIRTUAL
**         TF_HasStored  == COLFLAG_STORED
**         TF_HasHidden  == COLFLAG_HIDDEN
*/
#define TF_Readonly       0x00000001 /* Read-only system table */
#define TF_HasHidden      0x00000002 /* Has one or more hidden columns */
#define TF_HasPrimaryKey  0x00000004 /* Table has a primary key */
#define TF_Autoincrement  0x00000008 /* Integer primary key is autoincrement */
#define TF_HasStat1       0x00000010 /* nRowLogEst set from sqlite_stat1 */
#define TF_HasVirtual     0x00000020 /* Has one or more VIRTUAL columns */
#define TF_HasStored      0x00000040 /* Has one or more STORED columns */
#define TF_HasGenerated   0x00000060 /* Combo: HasVirtual + HasStored */
#define TF_WithoutRowid   0x00000080 /* No rowid.  PRIMARY KEY is the key */
#define TF_MaybeReanalyze 0x00000100 /* Maybe run ANALYZE on this table */
#define TF_NoVisibleRowid 0x00000200 /* No user-visible "rowid" column */
#define TF_OOOHidden      0x00000400 /* Out-of-Order hidden columns */
#define TF_HasNotNull     0x00000800 /* Contains NOT NULL constraints */
#define TF_Shadow         0x00001000 /* True for a shadow table */
#define TF_HasStat4       0x00002000 /* STAT4 info available for this table */
#define TF_Ephemeral      0x00004000 /* An ephemeral table */
#define TF_Eponymous      0x00008000 /* An eponymous virtual table */
#define TF_Strict         0x00010000 /* STRICT mode */
#define TF_Imposter       0x00020000 /* An imposter table */

/*
** Allowed values for Table.eTabType
*/
#define TABTYP_NORM      0     /* Ordinary table */
#define TABTYP_VTAB      1     /* Virtual table */
#define TABTYP_VIEW      2     /* A view */

#define IsView(X)           ((X)->eTabType==TABTYP_VIEW)
#define IsOrdinaryTable(X)  ((X)->eTabType==TABTYP_NORM)

/*
** Test to see whether or not a table is a virtual table.  This is
** done as a macro so that it will be optimized out when virtual
** table support is omitted from the build.
*/
#ifndef CAPDB_OMIT_VIRTUALTABLE
#  define IsVirtual(X)      ((X)->eTabType==TABTYP_VTAB)
#  define ExprIsVtab(X)  \
   ((X)->op==TK_COLUMN && (X)->y.pTab->eTabType==TABTYP_VTAB)
#else
#  define IsVirtual(X)      0
#  define ExprIsVtab(X)     0
#endif

/*
** Macros to determine if a column is hidden.  IsOrdinaryHiddenColumn()
** only works for non-virtual tables (ordinary tables and views) and is
** always false unless CAPDB_ENABLE_HIDDEN_COLUMNS is defined.  The
** IsHiddenColumn() macro is general purpose.
*/
#if defined(CAPDB_ENABLE_HIDDEN_COLUMNS)
#  define IsHiddenColumn(X)         (((X)->colFlags & COLFLAG_HIDDEN)!=0)
#  define IsOrdinaryHiddenColumn(X) (((X)->colFlags & COLFLAG_HIDDEN)!=0)
#elif !defined(CAPDB_OMIT_VIRTUALTABLE)
#  define IsHiddenColumn(X)         (((X)->colFlags & COLFLAG_HIDDEN)!=0)
#  define IsOrdinaryHiddenColumn(X) 0
#else
#  define IsHiddenColumn(X)         0
#  define IsOrdinaryHiddenColumn(X) 0
#endif


/* Does the table have a rowid */
#define HasRowid(X)     (((X)->tabFlags & TF_WithoutRowid)==0)
#define VisibleRowid(X) (((X)->tabFlags & TF_NoVisibleRowid)==0)

/* Macro is true if the CAPDB_ALLOW_ROWID_IN_VIEW (mis-)feature is
** available.  By default, this macro is false
*/
#ifndef CAPDB_ALLOW_ROWID_IN_VIEW
# define ViewCanHaveRowid     0
#else
# define ViewCanHaveRowid     (capdbConfig.mNoVisibleRowid==0)
#endif

/*
** Each foreign key constraint is an instance of the following structure.
**
** A foreign key is associated with two tables.  The "from" table is
** the table that contains the REFERENCES clause that creates the foreign
** key.  The "to" table is the table that is named in the REFERENCES clause.
** Consider this example:
**
**     CREATE TABLE ex1(
**       a INTEGER PRIMARY KEY,
**       b INTEGER CONSTRAINT fk1 REFERENCES ex2(x)
**     );
**
** For foreign key "fk1", the from-table is "ex1" and the to-table is "ex2".
** Equivalent names:
**
**     from-table == child-table
**       to-table == parent-table
**
** Each REFERENCES clause generates an instance of the following structure
** which is attached to the from-table.  The to-table need not exist when
** the from-table is created.  The existence of the to-table is not checked.
**
** The list of all parents for child Table X is held at X.pFKey.
**
** A list of all children for a table named Z (which might not even exist)
** is held in Schema.fkeyHash with a hash key of Z.
*/
struct FKey {
  Table *pFrom;     /* Table containing the REFERENCES clause (aka: Child) */
  FKey *pNextFrom;  /* Next FKey with the same in pFrom. Next parent of pFrom */
  char *zTo;        /* Name of table that the key points to (aka: Parent) */
  FKey *pNextTo;    /* Next with the same zTo. Next child of zTo. */
  FKey *pPrevTo;    /* Previous with the same zTo */
  int nCol;         /* Number of columns in this key */
  /* EV: R-30323-21917 */
  u8 isDeferred;       /* True if constraint checking is deferred till COMMIT */
  u8 aAction[2];        /* ON DELETE and ON UPDATE actions, respectively */
  Trigger *apTrigger[2];/* Triggers for aAction[] actions */
  struct sColMap {      /* Mapping of columns in pFrom to columns in zTo */
    int iFrom;            /* Index of column in pFrom */
    char *zCol;           /* Name of column in zTo.  If NULL use PRIMARY KEY */
  } aCol[FLEXARRAY];      /* One entry for each of nCol columns */
};

/* The size (in bytes) of an FKey object holding N columns.  The answer
** does NOT include space to hold the zTo name. */
#define SZ_FKEY(N)  (offsetof(FKey,aCol)+(N)*sizeof(struct sColMap))

/*
** SQLite supports many different ways to resolve a constraint
** error.  ROLLBACK processing means that a constraint violation
** causes the operation in process to fail and for the current transaction
** to be rolled back.  ABORT processing means the operation in process
** fails and any prior changes from that one operation are backed out,
** but the transaction is not rolled back.  FAIL processing means that
** the operation in progress stops and returns an error code.  But prior
** changes due to the same operation are not backed out and no rollback
** occurs.  IGNORE means that the particular row that caused the constraint
** error is not inserted or updated.  Processing continues and no error
** is returned.  REPLACE means that preexisting database rows that caused
** a UNIQUE constraint violation are removed so that the new insert or
** update can proceed.  Processing continues and no error is reported.
** UPDATE applies to insert operations only and means that the insert
** is omitted and the DO UPDATE clause of an upsert is run instead.
**
** RESTRICT, SETNULL, SETDFLT, and CASCADE actions apply only to foreign keys.
** RESTRICT is the same as ABORT for IMMEDIATE foreign keys and the
** same as ROLLBACK for DEFERRED keys.  SETNULL means that the foreign
** key is set to NULL.  SETDFLT means that the foreign key is set
** to its default value.  CASCADE means that a DELETE or UPDATE of the
** referenced table row is propagated into the row that holds the
** foreign key.
**
** The OE_Default value is a place holder that means to use whatever
** conflict resolution algorithm is required from context.
**
** The following symbolic values are used to record which type
** of conflict resolution action to take.
*/
#define OE_None     0   /* There is no constraint to check */
#define OE_Rollback 1   /* Fail the operation and rollback the transaction */
#define OE_Abort    2   /* Back out changes but do no rollback transaction */
#define OE_Fail     3   /* Stop the operation but leave all prior changes */
#define OE_Ignore   4   /* Ignore the error. Do not do the INSERT or UPDATE */
#define OE_Replace  5   /* Delete existing record, then do INSERT or UPDATE */
#define OE_Update   6   /* Process as a DO UPDATE in an upsert */
#define OE_Restrict 7   /* OE_Abort for IMMEDIATE, OE_Rollback for DEFERRED */
#define OE_SetNull  8   /* Set the foreign key value to NULL */
#define OE_SetDflt  9   /* Set the foreign key value to its default */
#define OE_Cascade  10  /* Cascade the changes */
#define OE_Default  11  /* Do whatever the default action is */


/*
** An instance of the following structure is passed as the first
** argument to capdbVdbeKeyCompare and is used to control the
** comparison of the two index keys.
**
** The aSortOrder[] and aColl[] arrays have nAllField slots each. There
** are nKeyField slots for the columns of an index then extra slots
** for the rowid or key at the end.  The aSortOrder array is located after
** the aColl[] array.
**
** If CAPDB_ENABLE_PREUPDATE_HOOK is defined, then aSortFlags might be NULL
** to indicate that this object is for use by a preupdate hook.  When aSortFlags
** is NULL, then nAllField is uninitialized and no space is allocated for
** aColl[], so those fields may not be used.
*/
struct KeyInfo {
  u32 nRef;           /* Number of references to this KeyInfo object */
  u8 enc;             /* Text encoding - one of the CAPDB_UTF* values */
  u16 nKeyField;      /* Number of key columns in the index */
  u16 nAllField;      /* Total columns, including key plus others */
  capdb *db;        /* The database connection */
  u8 *aSortFlags;     /* Sort order for each column. */
  CollSeq *aColl[FLEXARRAY]; /* Collating sequence for each term of the key */
};

/* The size (in bytes) of a KeyInfo object with up to N fields.  This includes
** the main body of the KeyInfo object and the aColl[] array of N elements,
** but does not count the memory used to hold aSortFlags[]. */
#define SZ_KEYINFO(N)  (offsetof(KeyInfo,aColl) + (N)*sizeof(CollSeq*))

/* The size of a bare KeyInfo with no aColl[] entries */
#if FLEXARRAY+1 > 1
# define SZ_KEYINFO_0   offsetof(KeyInfo,aColl)
#else
# define SZ_KEYINFO_0   sizeof(KeyInfo)
#endif

/*
** Allowed bit values for entries in the KeyInfo.aSortFlags[] array.
*/
#define KEYINFO_ORDER_DESC    0x01    /* DESC sort order */
#define KEYINFO_ORDER_BIGNULL 0x02    /* NULL is larger than any other value */

/*
** This object holds a record which has been parsed out into individual
** fields, for the purposes of doing a comparison.
**
** A record is an object that contains one or more fields of data.
** Records are used to store the content of a table row and to store
** the key of an index.  A blob encoding of a record is created by
** the OP_MakeRecord opcode of the VDBE and is disassembled by the
** OP_Column opcode.
**
** An instance of this object serves as a "key" for doing a search on
** an index b+tree. The goal of the search is to find the entry that
** is closest to the key described by this object.  This object might hold
** just a prefix of the key.  The number of fields is given by nField.
**
** The r1 and r2 fields are the values to return if this key is less than
** or greater than a key in the btree, respectively.  These are normally
** -1 and +1 respectively, but might be inverted to +1 and -1 if the b-tree
** is in DESC order.
**
** The key comparison functions actually return default_rc when they find
** an equals comparison.  default_rc can be -1, 0, or +1.  If there are
** multiple entries in the b-tree with the same key (when only looking
** at the first nField elements) then default_rc can be set to -1 to
** cause the search to find the last match, or +1 to cause the search to
** find the first match.
**
** The key comparison functions will set eqSeen to true if they ever
** get and equal results when comparing this structure to a b-tree record.
** When default_rc!=0, the search might end up on the record immediately
** before the first match or immediately after the last match.  The
** eqSeen field will indicate whether or not an exact match exists in the
** b-tree.
*/
struct UnpackedRecord {
  KeyInfo *pKeyInfo;  /* Comparison info for the index that is unpacked */
  Mem *aMem;          /* Values for columns of the index */
  union {
    char *z;            /* Cache of aMem[0].z for vdbeRecordCompareString() */
    i64 i;              /* Cache of aMem[0].u.i for vdbeRecordCompareInt() */
  } u;
  int n;              /* Cache of aMem[0].n used by vdbeRecordCompareString() */
  u16 nField;         /* Number of entries in apMem[] */
  i8 default_rc;      /* Comparison result if keys are equal */
  u8 errCode;         /* Error detected by xRecordCompare (CORRUPT or NOMEM) */
  i8 r1;              /* Value to return if (lhs < rhs) */
  i8 r2;              /* Value to return if (lhs > rhs) */
  u8 eqSeen;          /* True if an equality comparison has been seen */
};


/*
** Each SQL index is represented in memory by an
** instance of the following structure.
**
** The columns of the table that are to be indexed are described
** by the aiColumn[] field of this structure.  For example, suppose
** we have the following table and index:
**
**     CREATE TABLE Ex1(c1 int, c2 int, c3 text);
**     CREATE INDEX Ex2 ON Ex1(c3,c1);
**
** In the Table structure describing Ex1, nCol==3 because there are
** three columns in the table.  In the Index structure describing
** Ex2, nColumn==2 since 2 of the 3 columns of Ex1 are indexed.
** The value of aiColumn is {2, 0}.  aiColumn[0]==2 because the
** first column to be indexed (c3) has an index of 2 in Ex1.aCol[].
** The second column to be indexed (c1) has an index of 0 in
** Ex1.aCol[], hence Ex2.aiColumn[1]==0.
**
** The Index.onError field determines whether or not the indexed columns
** must be unique and what to do if they are not.  When Index.onError=OE_None,
** it means this is not a unique index.  Otherwise it is a unique index
** and the value of Index.onError indicates which conflict resolution
** algorithm to employ when an attempt is made to insert a non-unique
** element.
**
** The colNotIdxed bitmask is used in combination with SrcItem.colUsed
** for a fast test to see if an index can serve as a covering index.
** colNotIdxed has a 1 bit for every column of the original table that
** is *not* available in the index.  Thus the expression
** "colUsed & colNotIdxed" will be non-zero if the index is not a
** covering index.  The most significant bit of of colNotIdxed will always
** be true (note-20221022-a).  If a column beyond the 63rd column of the
** table is used, the "colUsed & colNotIdxed" test will always be non-zero
** and we have to assume either that the index is not covering, or use
** an alternative (slower) algorithm to determine whether or not
** the index is covering.
**
** While parsing a CREATE TABLE or CREATE INDEX statement in order to
** generate VDBE code (as opposed to parsing one read from an sqlite_schema
** table as part of parsing an existing database schema), transient instances
** of this structure may be created. In this case the Index.tnum variable is
** used to store the address of a VDBE instruction, not a database page
** number (it cannot - the database page is not allocated until the VDBE
** program is executed). See convertToWithoutRowidTable() for details.
*/
struct Index {
  char *zName;             /* Name of this index */
  i16 *aiColumn;           /* Which columns are used by this index.  1st is 0 */
  LogEst *aiRowLogEst;     /* From ANALYZE: Est. rows selected by each column */
  Table *pTable;           /* The SQL table being indexed */
  char *zColAff;           /* String defining the affinity of each column */
  Index *pNext;            /* The next index associated with the same table */
  Schema *pSchema;         /* Schema containing this index */
  u8 *aSortOrder;          /* for each column: True==DESC, False==ASC */
  const char **azColl;     /* Array of collation sequence names for index */
  Expr *pPartIdxWhere;     /* WHERE clause for partial indices */
  ExprList *aColExpr;      /* Column expressions */
  Pgno tnum;               /* DB Page containing root of this index */
  LogEst szIdxRow;         /* Estimated average row size in bytes */
  u16 nKeyCol;             /* Number of columns forming the key */
  u16 nColumn;             /* Nr columns in btree. Can be 2*Table.nCol */
  u8 onError;              /* OE_Abort, OE_Ignore, OE_Replace, or OE_None */
  unsigned idxType:2;      /* 0:Normal 1:UNIQUE, 2:PRIMARY KEY, 3:IPK */
  unsigned bUnordered:1;   /* Use this index for == or IN queries only */
  unsigned uniqNotNull:1;  /* True if UNIQUE and NOT NULL for all columns */
  unsigned isResized:1;    /* True if resizeIndexObject() has been called */
  unsigned isCovering:1;   /* True if this is a covering index */
  unsigned noSkipScan:1;   /* Do not try to use skip-scan if true */
  unsigned hasStat1:1;     /* aiRowLogEst values come from sqlite_stat1 */
  unsigned bNoQuery:1;     /* Do not use this index to optimize queries */
  unsigned bAscKeyBug:1;   /* True if the bba7b69f9849b5bf bug applies */
  unsigned bHasVCol:1;     /* Index references one or more VIRTUAL columns */
  unsigned bHasExpr:1;     /* Index contains an expression, either a literal
                           ** expression, or a reference to a VIRTUAL column */
#ifdef CAPDB_ENABLE_STAT4
  int nSample;             /* Number of elements in aSample[] */
  int mxSample;            /* Number of slots allocated to aSample[] */
  int nSampleCol;          /* Size of IndexSample.anEq[] and so on */
  tRowcnt *aAvgEq;         /* Average nEq values for keys not in aSample */
  IndexSample *aSample;    /* Samples of the left-most key */
  tRowcnt *aiRowEst;       /* Non-logarithmic stat1 data for this index */
  tRowcnt nRowEst0;        /* Non-logarithmic number of rows in the index */
#endif
  Bitmask colNotIdxed;     /* Unindexed columns in pTab */
};

/*
** Allowed values for Index.idxType
*/
#define CAPDB_IDXTYPE_APPDEF      0   /* Created using CREATE INDEX */
#define CAPDB_IDXTYPE_UNIQUE      1   /* Implements a UNIQUE constraint */
#define CAPDB_IDXTYPE_PRIMARYKEY  2   /* Is the PRIMARY KEY for the table */
#define CAPDB_IDXTYPE_IPK         3   /* INTEGER PRIMARY KEY index */

/* Return true if index X is a PRIMARY KEY index */
#define IsPrimaryKeyIndex(X)  ((X)->idxType==CAPDB_IDXTYPE_PRIMARYKEY)

/* Return true if index X is a UNIQUE index */
#define IsUniqueIndex(X)      ((X)->onError!=OE_None)

/* The Index.aiColumn[] values are normally positive integer.  But
** there are some negative values that have special meaning:
*/
#define XN_ROWID     (-1)     /* Indexed column is the rowid */
#define XN_EXPR      (-2)     /* Indexed column is an expression */

/*
** Each sample stored in the sqlite_stat4 table is represented in memory
** using a structure of this type.  See documentation at the top of the
** analyze.c source file for additional information.
*/
struct IndexSample {
  void *p;          /* Pointer to sampled record */
  int n;            /* Size of record in bytes */
  tRowcnt *anEq;    /* Est. number of rows where the key equals this sample */
  tRowcnt *anLt;    /* Est. number of rows where key is less than this sample */
  tRowcnt *anDLt;   /* Est. number of distinct keys less than this sample */
};

/*
** Possible values to use within the flags argument to capdbGetToken().
*/
#define CAPDB_TOKEN_QUOTED    0x1 /* Token is a quoted identifier. */
#define CAPDB_TOKEN_KEYWORD   0x2 /* Token is a keyword. */

/*
** Each token coming out of the lexer is an instance of
** this structure.  Tokens are also used as part of an expression.
**
** The memory that "z" points to is owned by other objects.  Take care
** that the owner of the "z" string does not deallocate the string before
** the Token goes out of scope!  Very often, the "z" points to some place
** in the middle of the Parse.zSql text.  But it might also point to a
** static string.
*/
struct Token {
  const char *z;     /* Text of the token.  Not NULL-terminated! */
  unsigned int n;    /* Number of characters in this token */
};

/*
** An instance of this structure contains information needed to generate
** code for a SELECT that contains aggregate functions.
**
** If Expr.op==TK_AGG_COLUMN or TK_AGG_FUNCTION then Expr.pAggInfo is a
** pointer to this structure.  The Expr.iAgg field is the index in
** AggInfo.aCol[] or AggInfo.aFunc[] of information needed to generate
** code for that node.
**
** AggInfo.pGroupBy and AggInfo.aFunc.pExpr point to fields within the
** original Select structure that describes the SELECT statement.  These
** fields do not need to be freed when deallocating the AggInfo structure.
*/
struct AggInfo {
  u8 directMode;          /* Direct rendering mode means take data directly
                          ** from source tables rather than from accumulators */
  u8 useSortingIdx;       /* In direct mode, reference the sorting index rather
                          ** than the source table */
  u32 nSortingColumn;     /* Number of columns in the sorting index */
  int sortingIdx;         /* Cursor number of the sorting index */
  int sortingIdxPTab;     /* Cursor number of pseudo-table */
  int iFirstReg;          /* First register in range for aCol[] and aFunc[] */
  ExprList *pGroupBy;     /* The group by clause */
  struct AggInfo_col {    /* For each column used in source tables */
    Table *pTab;             /* Source table */
    Expr *pCExpr;            /* The original expression */
    int iTable;              /* Cursor number of the source table */
    int iColumn;             /* Column number within the source table */
    int iSorterColumn;       /* Column number in the sorting index */
  } *aCol;
  int nColumn;            /* Number of used entries in aCol[] */
  int nAccumulator;       /* Number of columns that show through to the output.
                          ** Additional columns are used only as parameters to
                          ** aggregate functions */
  struct AggInfo_func {   /* For each aggregate function */
    Expr *pFExpr;            /* Expression encoding the function */
    FuncDef *pFunc;          /* The aggregate function implementation */
    int iDistinct;           /* Ephemeral table used to enforce DISTINCT */
    int iDistAddr;           /* Address of OP_OpenEphemeral */
    int iOBTab;              /* Ephemeral table to implement ORDER BY */
    u8 bOBPayload;           /* iOBTab has payload columns separate from key */
    u8 bOBUnique;            /* Enforce uniqueness on iOBTab keys */
    u8 bUseSubtype;          /* Transfer subtype info through sorter */
  } *aFunc;
  int nFunc;              /* Number of entries in aFunc[] */
  u32 selId;              /* Select to which this AggInfo belongs */
#ifdef CAPDB_DEBUG
  Select *pSelect;        /* SELECT statement that this AggInfo supports */
#endif
};

/*
** Macros to compute aCol[] and aFunc[] register numbers.
**
** These macros should not be used prior to the call to
** assignAggregateRegisters() that computes the value of pAggInfo->iFirstReg.
** The assert()s that are part of this macro verify that constraint.
*/
#ifndef NDEBUG
#define AggInfoColumnReg(A,I)  (assert((A)->iFirstReg),(A)->iFirstReg+(I))
#define AggInfoFuncReg(A,I)    \
                      (assert((A)->iFirstReg),(A)->iFirstReg+(A)->nColumn+(I))
#else
#define AggInfoColumnReg(A,I)  ((A)->iFirstReg+(I))
#define AggInfoFuncReg(A,I)    \
                      ((A)->iFirstReg+(A)->nColumn+(I))
#endif

/*
** The datatype ynVar is a signed integer, either 16-bit or 32-bit.
** Usually it is 16-bits.  But if CAPDB_MAX_VARIABLE_NUMBER is greater
** than 32767 we have to make it 32-bit.  16-bit is preferred because
** it uses less memory in the Expr object, which is a big memory user
** in systems with lots of prepared statements.  And few applications
** need more than about 10 or 20 variables.  But some extreme users want
** to have prepared statements with over 32766 variables, and for them
** the option is available (at compile-time).
*/
#if CAPDB_MAX_VARIABLE_NUMBER<32767
typedef i16 ynVar;
#else
typedef int ynVar;
#endif

/*
** Each node of an expression in the parse tree is an instance
** of this structure.
**
** Expr.op is the opcode. The integer parser token codes are reused
** as opcodes here. For example, the parser defines TK_GE to be an integer
** code representing the ">=" operator. This same integer code is reused
** to represent the greater-than-or-equal-to operator in the expression
** tree.
**
** If the expression is an SQL literal (TK_INTEGER, TK_FLOAT, TK_BLOB,
** or TK_STRING), then Expr.u.zToken contains the text of the SQL literal. If
** the expression is a variable (TK_VARIABLE), then Expr.u.zToken contains the
** variable name. Finally, if the expression is an SQL function (TK_FUNCTION),
** then Expr.u.zToken contains the name of the function.
**
** Expr.pRight and Expr.pLeft are the left and right subexpressions of a
** binary operator. Either or both may be NULL.
**
** Expr.x.pList is a list of arguments if the expression is an SQL function,
** a CASE expression or an IN expression of the form "<lhs> IN (<y>, <z>...)".
** Expr.x.pSelect is used if the expression is a sub-select or an expression of
** the form "<lhs> IN (SELECT ...)". If the EP_xIsSelect bit is set in the
** Expr.flags mask, then Expr.x.pSelect is valid. Otherwise, Expr.x.pList is
** valid.
**
** An expression of the form ID or ID.ID refers to a column in a table.
** For such expressions, Expr.op is set to TK_COLUMN and Expr.iTable is
** the integer cursor number of a VDBE cursor pointing to that table and
** Expr.iColumn is the column number for the specific column.  If the
** expression is used as a result in an aggregate SELECT, then the
** value is also stored in the Expr.iAgg column in the aggregate so that
** it can be accessed after all aggregates are computed.
**
** If the expression is an unbound variable marker (a question mark
** character '?' in the original SQL) then the Expr.iTable holds the index
** number for that variable.
**
** If the expression is a subquery then Expr.iColumn holds an integer
** register number containing the result of the subquery.  If the
** subquery gives a constant result, then iTable is -1.  If the subquery
** gives a different answer at different times during statement processing
** then iTable is the address of a subroutine that computes the subquery.
**
** If the Expr is of type OP_Column, and the table it is selecting from
** is a disk table or the "old.*" pseudo-table, then pTab points to the
** corresponding table definition.
**
** ALLOCATION NOTES:
**
** Expr objects can use a lot of memory space in database schema.  To
** help reduce memory requirements, sometimes an Expr object will be
** truncated.  And to reduce the number of memory allocations, sometimes
** two or more Expr objects will be stored in a single memory allocation,
** together with Expr.u.zToken strings.
**
** If the EP_Reduced and EP_TokenOnly flags are set when
** an Expr object is truncated.  When EP_Reduced is set, then all
** the child Expr objects in the Expr.pLeft and Expr.pRight subtrees
** are contained within the same memory allocation.  Note, however, that
** the subtrees in Expr.x.pList or Expr.x.pSelect are always separately
** allocated, regardless of whether or not EP_Reduced is set.
*/
struct Expr {
  u8 op;                 /* Operation performed by this node */
  char affExpr;          /* affinity, or RAISE type */
  u8 op2;                /* TK_REGISTER/TK_TRUTH: original value of Expr.op
                         ** TK_COLUMN: the value of p5 for OP_Column
                         ** TK_AGG_FUNCTION: nesting depth
                         ** TK_FUNCTION: NC_SelfRef flag if needs OP_PureFunc */
#ifdef CAPDB_DEBUG
  u8 vvaFlags;           /* Verification flags. */
#endif
  u32 flags;             /* Various flags.  EP_* See below */
  union {
    char *zToken;          /* Token value. Zero terminated and dequoted */
    int iValue;            /* Non-negative integer value if EP_IntValue */
  } u;

  /* If the EP_TokenOnly flag is set in the Expr.flags mask, then no
  ** space is allocated for the fields below this point. An attempt to
  ** access them will result in a segfault or malfunction.
  *********************************************************************/

  Expr *pLeft;           /* Left subnode */
  Expr *pRight;          /* Right subnode */
  union {
    ExprList *pList;     /* op = IN, EXISTS, SELECT, CASE, FUNCTION, BETWEEN */
    Select *pSelect;     /* EP_xIsSelect and op = IN, EXISTS, SELECT */
  } x;

  /* If the EP_Reduced flag is set in the Expr.flags mask, then no
  ** space is allocated for the fields below this point. An attempt to
  ** access them will result in a segfault or malfunction.
  *********************************************************************/

#if CAPDB_MAX_EXPR_DEPTH>0
  int nHeight;           /* Height of the tree headed by this node */
#endif
  int iTable;            /* TK_COLUMN: cursor number of table holding column
                         ** TK_REGISTER: register number
                         ** TK_TRIGGER: 1 -> new, 0 -> old
                         ** EP_Unlikely:  134217728 times likelihood
                         ** TK_IN: ephemeral table holding RHS
                         ** TK_SELECT_COLUMN: Number of columns on the LHS
                         ** TK_SELECT: 1st register of result vector */
  ynVar iColumn;         /* TK_COLUMN: column index.  -1 for rowid.
                         ** TK_VARIABLE: variable number (always >= 1).
                         ** TK_SELECT_COLUMN: column of the result vector */
  i16 iAgg;              /* Which entry in pAggInfo->aCol[] or ->aFunc[] */
  union {
    int iJoin;             /* If EP_OuterON or EP_InnerON, the right table */
    int iOfst;             /* else: start of token from start of statement */
  } w;
  AggInfo *pAggInfo;     /* Used by TK_AGG_COLUMN and TK_AGG_FUNCTION */
  union {
    Table *pTab;           /* TK_COLUMN: Table containing column. Can be NULL
                           ** for a column of an index on an expression */
    Window *pWin;          /* EP_WinFunc: Window/Filter defn for a function */
    int nReg;              /* TK_NULLS: Number of registers to NULL out */
    struct {               /* TK_IN, TK_SELECT, and TK_EXISTS */
      int iAddr;             /* Subroutine entry address */
      int regReturn;         /* Register used to hold return address */
    } sub;
  } y;
};

/* The following are the meanings of bits in the Expr.flags field.
** Value restrictions:
**
**          EP_Agg == NC_HasAgg == SF_HasAgg
**          EP_Win == NC_HasWin
*/
#define EP_OuterON    0x000001 /* Originates in ON/USING clause of outer join */
#define EP_InnerON    0x000002 /* Originates in ON/USING of an inner join */
#define EP_Distinct   0x000004 /* Aggregate function with DISTINCT keyword */
#define EP_HasFunc    0x000008 /* Contains one or more functions of any kind */
#define EP_Agg        0x000010 /* Contains one or more aggregate functions */
#define EP_FixedCol   0x000020 /* TK_Column with a known fixed value */
#define EP_VarSelect  0x000040 /* pSelect is correlated, not constant */
#define EP_DblQuoted  0x000080 /* token.z was originally in "..." */
#define EP_InfixFunc  0x000100 /* True for an infix function: LIKE, GLOB, etc */
#define EP_Collate    0x000200 /* Tree contains a TK_COLLATE operator */
#define EP_Commuted   0x000400 /* Comparison operator has been commuted */
#define EP_IntValue   0x000800 /* Integer value contained in u.iValue */
#define EP_xIsSelect  0x001000 /* x.pSelect is valid (otherwise x.pList is) */
#define EP_Skip       0x002000 /* Operator does not contribute to affinity */
#define EP_Reduced    0x004000 /* Expr struct EXPR_REDUCEDSIZE bytes only */
#define EP_Win        0x008000 /* Contains window functions */
#define EP_TokenOnly  0x010000 /* Expr struct EXPR_TOKENONLYSIZE bytes only */
#define EP_FullSize   0x020000 /* Expr structure must remain full sized */
#define EP_IfNullRow  0x040000 /* The TK_IF_NULL_ROW opcode */
#define EP_Unlikely   0x080000 /* unlikely() or likelihood() function */
#define EP_ConstFunc  0x100000 /* A CAPDB_FUNC_CONSTANT or _SLOCHNG function */
#define EP_CanBeNull  0x200000 /* Can be null despite NOT NULL constraint */
#define EP_Subquery   0x400000 /* Tree contains a TK_SELECT operator */
#define EP_Leaf       0x800000 /* Expr.pLeft, .pRight, .u.pSelect all NULL */
#define EP_WinFunc   0x1000000 /* TK_FUNCTION with Expr.y.pWin set */
#define EP_Subrtn    0x2000000 /* Uses Expr.y.sub. TK_IN, _SELECT, or _EXISTS */
#define EP_Quoted    0x4000000 /* TK_ID was originally quoted */
#define EP_Static    0x8000000 /* Held in memory not obtained from malloc() */
#define EP_IsTrue   0x10000000 /* Always has boolean value of TRUE */
#define EP_IsFalse  0x20000000 /* Always has boolean value of FALSE */
#define EP_FromDDL  0x40000000 /* Originates from sqlite_schema */
#define EP_SubtArg  0x80000000 /* Is argument to CAPDB_SUBTYPE function */

/* The EP_Propagate mask is a set of properties that automatically propagate
** upwards into parent nodes.
*/
#define EP_Propagate (EP_Collate|EP_Subquery|EP_HasFunc)

/* Macros can be used to test, set, or clear bits in the
** Expr.flags field.
*/
#define ExprHasProperty(E,P)     (((E)->flags&(u32)(P))!=0)
#define ExprHasAllProperty(E,P)  (((E)->flags&(u32)(P))==(u32)(P))
#define ExprSetProperty(E,P)     (E)->flags|=(u32)(P)
#define ExprClearProperty(E,P)   (E)->flags&=~(u32)(P)
#define ExprAlwaysTrue(E)   (((E)->flags&(EP_OuterON|EP_IsTrue))==EP_IsTrue)
#define ExprAlwaysFalse(E)  (((E)->flags&(EP_OuterON|EP_IsFalse))==EP_IsFalse)
#define ExprIsFullSize(E)   (((E)->flags&(EP_Reduced|EP_TokenOnly))==0)

/* Macros used to ensure that the correct members of unions are accessed
** in Expr.
*/
#define ExprUseUToken(E)    (((E)->flags&EP_IntValue)==0)
#define ExprUseUValue(E)    (((E)->flags&EP_IntValue)!=0)
#define ExprUseWOfst(E)     (((E)->flags&(EP_InnerON|EP_OuterON))==0)
#define ExprUseWJoin(E)     (((E)->flags&(EP_InnerON|EP_OuterON))!=0)
#define ExprUseXList(E)     (((E)->flags&EP_xIsSelect)==0)
#define ExprUseXSelect(E)   (((E)->flags&EP_xIsSelect)!=0)
#define ExprUseYTab(E)      (((E)->flags&(EP_WinFunc|EP_Subrtn))==0)
#define ExprUseYWin(E)      (((E)->flags&EP_WinFunc)!=0)
#define ExprUseYSub(E)      (((E)->flags&EP_Subrtn)!=0)

/* Flags for use with Expr.vvaFlags
*/
#define EP_NoReduce   0x01  /* Cannot EXPRDUP_REDUCE this Expr */
#define EP_Immutable  0x02  /* Do not change this Expr node */

/* The ExprSetVVAProperty() macro is used for Verification, Validation,
** and Accreditation only.  It works like ExprSetProperty() during VVA
** processes but is a no-op for delivery.
*/
#ifdef CAPDB_DEBUG
# define ExprSetVVAProperty(E,P)   (E)->vvaFlags|=(P)
# define ExprHasVVAProperty(E,P)   (((E)->vvaFlags&(P))!=0)
# define ExprClearVVAProperties(E) (E)->vvaFlags = 0
#else
# define ExprSetVVAProperty(E,P)
# define ExprHasVVAProperty(E,P)   0
# define ExprClearVVAProperties(E)
#endif

/*
** Macros to determine the number of bytes required by a normal Expr
** struct, an Expr struct with the EP_Reduced flag set in Expr.flags
** and an Expr struct with the EP_TokenOnly flag set.
*/
#define EXPR_FULLSIZE           sizeof(Expr)           /* Full size */
#define EXPR_REDUCEDSIZE        offsetof(Expr,iTable)  /* Common features */
#define EXPR_TOKENONLYSIZE      offsetof(Expr,pLeft)   /* Fewer features */

/*
** Flags passed to the capdbExprDup() function. See the header comment
** above capdbExprDup() for details.
*/
#define EXPRDUP_REDUCE         0x0001  /* Used reduced-size Expr nodes */

/*
** True if the expression passed as an argument was a function with
** an OVER() clause (a window function).
*/
#ifdef CAPDB_OMIT_WINDOWFUNC
# define IsWindowFunc(p) 0
#else
# define IsWindowFunc(p) ( \
    ExprHasProperty((p), EP_WinFunc) && p->y.pWin->eFrmType!=TK_FILTER \
 )
#endif

/*
** A list of expressions.  Each expression may optionally have a
** name.  An expr/name combination can be used in several ways, such
** as the list of "expr AS ID" fields following a "SELECT" or in the
** list of "ID = expr" items in an UPDATE.  A list of expressions can
** also be used as the argument to a function, in which case the a.zName
** field is not used.
**
** In order to try to keep memory usage down, the Expr.a.zEName field
** is used for multiple purposes:
**
**     eEName          Usage
**    ----------       -------------------------
**    ENAME_NAME       (1) the AS of result set column
**                     (2) COLUMN= of an UPDATE
**
**    ENAME_TAB        DB.TABLE.NAME used to resolve names
**                     of subqueries
**
**    ENAME_SPAN       Text of the original result set
**                     expression.
*/
struct ExprList {
  int nExpr;             /* Number of expressions on the list */
  int nAlloc;            /* Number of a[] slots allocated */
  struct ExprList_item { /* For each expression in the list */
    Expr *pExpr;            /* The parse tree for this expression */
    char *zEName;           /* Token associated with this expression */
    struct {
      u8 sortFlags;           /* Mask of KEYINFO_ORDER_* flags */
      unsigned eEName :2;     /* Meaning of zEName */
      unsigned done :1;       /* Indicates when processing is finished */
      unsigned reusable :1;   /* Constant expression is reusable */
      unsigned bSorterRef :1; /* Defer evaluation until after sorting */
      unsigned bNulls :1;     /* True if explicit "NULLS FIRST/LAST" */
      unsigned bUsed :1;      /* This column used in a SF_NestedFrom subquery */
      unsigned bUsingTerm:1;  /* Term from the USING clause of a NestedFrom */
      unsigned bNoExpand: 1;  /* Term is an auxiliary in NestedFrom and should
                              ** not be expanded by "*" in parent queries */
    } fg;
    union {
      struct {             /* Used by any ExprList other than Parse.pConsExpr */
        u16 iOrderByCol;      /* For ORDER BY, column number in result set */
        u16 iAlias;           /* Index into Parse.aAlias[] for zName */
      } x;
      int iConstExprReg;   /* Register in which Expr value is cached. Used only
                           ** by Parse.pConstExpr */
    } u;
  } a[FLEXARRAY];          /* One slot for each expression in the list */
};

/* The size (in bytes) of an ExprList object that is big enough to hold
** as many as N expressions. */
#define SZ_EXPRLIST(N)  \
             (offsetof(ExprList,a) + (N)*sizeof(struct ExprList_item))

/*
** Allowed values for Expr.a.eEName
*/
#define ENAME_NAME  0       /* The AS clause of a result set */
#define ENAME_SPAN  1       /* Complete text of the result set expression */
#define ENAME_TAB   2       /* "DB.TABLE.NAME" for the result set */
#define ENAME_ROWID 3       /* "DB.TABLE._rowid_" for * expansion of rowid */

/*
** An instance of this structure can hold a simple list of identifiers,
** such as the list "a,b,c" in the following statements:
**
**      INSERT INTO t(a,b,c) VALUES ...;
**      CREATE INDEX idx ON t(a,b,c);
**      CREATE TRIGGER trig BEFORE UPDATE ON t(a,b,c) ...;
**
** The IdList.a.idx field is used when the IdList represents the list of
** column names after a table name in an INSERT statement.  In the statement
**
**     INSERT INTO t(a,b,c) ...
**
** If "a" is the k-th column of table "t", then IdList.a[0].idx==k.
*/
struct IdList {
  int nId;         /* Number of identifiers on the list */
  struct IdList_item {
    char *zName;      /* Name of the identifier */
  } a[FLEXARRAY];
};

/* The size (in bytes) of an IdList object that can hold up to N IDs. */
#define SZ_IDLIST(N)  (offsetof(IdList,a)+(N)*sizeof(struct IdList_item))

/*
** Allowed values for IdList.eType, which determines which value of the a.u4
** is valid.
*/
#define EU4_NONE   0   /* Does not use IdList.a.u4 */
#define EU4_IDX    1   /* Uses IdList.a.u4.idx */
#define EU4_EXPR   2   /* Uses IdList.a.u4.pExpr -- NOT CURRENTLY USED */

/*
** Details of the implementation of a subquery.
*/
struct Subquery {
  Select *pSelect;  /* A SELECT statement used in place of a table name */
  int addrFillSub;  /* Address of subroutine to initialize a subquery */
  int regReturn;    /* Register holding return address of addrFillSub */
  int regResult;    /* Registers holding results of a co-routine */
};

/*
** The SrcItem object represents a single term in the FROM clause of a query.
** The SrcList object is mostly an array of SrcItems.
**
** The jointype starts out showing the join type between the current table
** and the next table on the list.  The parser builds the list this way.
** But capdbSrcListShiftJoinType() later shifts the jointypes so that each
** jointype expresses the join between the table and the previous table.
**
** In the colUsed field, the high-order bit (bit 63) is set if the table
** contains more than 63 columns and the 64-th or later column is used.
**
** Aggressive use of "union" helps keep the size of the object small.  This
** has been shown to boost performance, in addition to saving memory.
** Access to union elements is gated by the following rules which should
** always be checked, either by an if-statement or by an assert().
**
**    Field              Only access if this is true
**    ---------------    -----------------------------------
**    u1.zIndexedBy      fg.isIndexedBy
**    u1.pFuncArg        fg.isTabFunc
**    u1.nRow            !fg.isTabFunc  && !fg.isIndexedBy
**
**    u2.pIBIndex        fg.isIndexedBy
**    u2.pCteUse         fg.isCte
**
**    u3.pOn             !fg.isUsing
**    u3.pUsing          fg.isUsing
**
**    u4.zDatabase       !fg.fixedSchema && !fg.isSubquery
**    u4.pSchema         fg.fixedSchema
**    u4.pSubq           fg.isSubquery
**
** See also the capdbSrcListDelete() routine for assert() statements that
** check invariants on the fields of this object, especially the flags
** inside the fg struct.
*/
struct SrcItem {
  char *zName;      /* Name of the table */
  char *zAlias;     /* The "B" part of a "A AS B" phrase.  zName is the "A" */
  Table *pSTab;     /* Table object for zName. Mnemonic: Srcitem-TABle */
  struct {
    u8 jointype;      /* Type of join between this table and the previous */
    unsigned notIndexed :1;    /* True if there is a NOT INDEXED clause */
    unsigned isIndexedBy :1;   /* True if there is an INDEXED BY clause */
    unsigned isSubquery :1;    /* True if this term is a subquery */
    unsigned isTabFunc :1;     /* True if table-valued-function syntax */
    unsigned isCorrelated :1;  /* True if sub-query is correlated */
    unsigned isMaterialized:1; /* This is a materialized view */
    unsigned viaCoroutine :1;  /* Implemented as a co-routine */
    unsigned isRecursive :1;   /* True for recursive reference in WITH */
    unsigned fromDDL :1;       /* Comes from sqlite_schema */
    unsigned isCte :1;         /* This is a CTE */
    unsigned notCte :1;        /* This item may not match a CTE */
    unsigned isUsing :1;       /* u3.pUsing is valid */
    unsigned isOn :1;          /* u3.pOn was once valid and non-NULL */
    unsigned isSynthUsing :1;  /* u3.pUsing is synthesized from NATURAL */
    unsigned isNestedFrom :1;  /* pSelect is a SF_NestedFrom subquery */
    unsigned rowidUsed :1;     /* The ROWID of this table is referenced */
    unsigned fixedSchema :1;   /* Uses u4.pSchema, not u4.zDatabase */
    unsigned hadSchema :1;     /* Had u4.zDatabase before u4.pSchema */
    unsigned fromExists :1;    /* Comes from WHERE EXISTS(...) */
  } fg;
  int iCursor;      /* The VDBE cursor number used to access this table */
  Bitmask colUsed;  /* Bit N set if column N used. Details above for N>62 */
  union {
    char *zIndexedBy;    /* Identifier from "INDEXED BY <zIndex>" clause */
    ExprList *pFuncArg;  /* Arguments to table-valued-function */
    u32 nRow;            /* Number of rows in a VALUES clause */
  } u1;
  union {
    Index *pIBIndex;  /* Index structure corresponding to u1.zIndexedBy */
    CteUse *pCteUse;  /* CTE Usage info when fg.isCte is true */
  } u2;
  union {
    Expr *pOn;        /* fg.isUsing==0 =>  The ON clause of a join */
    IdList *pUsing;   /* fg.isUsing==1 =>  The USING clause of a join */
  } u3;
  union {
    Schema *pSchema;  /* Schema to which this item is fixed */
    char *zDatabase;  /* Name of database holding this table */
    Subquery *pSubq;  /* Description of a subquery */
  } u4;
};

/*
** The OnOrUsing object represents either an ON clause or a USING clause.
** It can never be both at the same time, but it can be neither.
*/
struct OnOrUsing {
  Expr *pOn;         /* The ON clause of a join */
  IdList *pUsing;    /* The USING clause of a join */
};

/*
** This object represents one or more tables that are the source of
** content for an SQL statement.  For example, a single SrcList object
** is used to hold the FROM clause of a SELECT statement.  SrcList also
** represents the target tables for DELETE, INSERT, and UPDATE statements.
**
*/
struct SrcList {
  int nSrc;             /* Number of tables or subqueries in the FROM clause */
  u32 nAlloc;           /* Number of entries allocated in a[] below */
  SrcItem a[FLEXARRAY]; /* One entry for each identifier on the list */
};

/* Size (in bytes) of a SrcList object that can hold as many as N
** SrcItem objects. */
#define SZ_SRCLIST(N) (offsetof(SrcList,a)+(N)*sizeof(SrcItem))

/* Size (in bytes( of a SrcList object that holds 1 SrcItem.  This is a
** special case of SZ_SRCITEM(1) that comes up often. */
#define SZ_SRCLIST_1  (offsetof(SrcList,a)+sizeof(SrcItem))

/*
** Permitted values of the SrcList.a.jointype field
*/
#define JT_INNER     0x01    /* Any kind of inner or cross join */
#define JT_CROSS     0x02    /* Explicit use of the CROSS keyword */
#define JT_NATURAL   0x04    /* True for a "natural" join */
#define JT_LEFT      0x08    /* Left outer join */
#define JT_RIGHT     0x10    /* Right outer join */
#define JT_OUTER     0x20    /* The "OUTER" keyword is present */
#define JT_LTORJ     0x40    /* One of the LEFT operands of a RIGHT JOIN
                             ** Mnemonic: Left Table Of Right Join */
#define JT_ERROR     0x80    /* unknown or unsupported join type */

/*
** Flags appropriate for the wctrlFlags parameter of capdbWhereBegin()
** and the WhereInfo.wctrlFlags member.
**
** Value constraints (enforced via assert()):
**     WHERE_USE_LIMIT  == SF_FixedLimit
*/
#define WHERE_ORDERBY_NORMAL   0x0000 /* No-op */
#define WHERE_ORDERBY_MIN      0x0001 /* ORDER BY processing for min() func */
#define WHERE_ORDERBY_MAX      0x0002 /* ORDER BY processing for max() func */
#define WHERE_ONEPASS_DESIRED  0x0004 /* Want to do one-pass UPDATE/DELETE */
#define WHERE_ONEPASS_MULTIROW 0x0008 /* ONEPASS is ok with multiple rows */
#define WHERE_DUPLICATES_OK    0x0010 /* Ok to return a row more than once */
#define WHERE_OR_SUBCLAUSE     0x0020 /* Processing a sub-WHERE as part of
                                      ** the OR optimization  */
#define WHERE_GROUPBY          0x0040 /* pOrderBy is really a GROUP BY */
#define WHERE_DISTINCTBY       0x0080 /* pOrderby is really a DISTINCT clause */
#define WHERE_WANT_DISTINCT    0x0100 /* All output needs to be distinct */
#define WHERE_SORTBYGROUP      0x0200 /* Support capdbWhereIsSorted() */
#define WHERE_AGG_DISTINCT     0x0400 /* Query is "SELECT agg(DISTINCT ...)" */
#define WHERE_ORDERBY_LIMIT    0x0800 /* ORDERBY+LIMIT on the inner loop */
#define WHERE_RIGHT_JOIN       0x1000 /* Processing a RIGHT JOIN */
#define WHERE_KEEP_ALL_JOINS   0x2000 /* Do not do the omit-noop-join opt */
#define WHERE_USE_LIMIT        0x4000 /* Use the LIMIT in cost estimates */
                        /*     0x8000    not currently used */

/* Allowed return values from capdbWhereIsDistinct()
*/
#define WHERE_DISTINCT_NOOP      0  /* DISTINCT keyword not used */
#define WHERE_DISTINCT_UNIQUE    1  /* No duplicates */
#define WHERE_DISTINCT_ORDERED   2  /* All duplicates are adjacent */
#define WHERE_DISTINCT_UNORDERED 3  /* Duplicates are scattered */

/*
** A NameContext defines a context in which to resolve table and column
** names.  The context consists of a list of tables (the pSrcList) field and
** a list of named expression (pEList).  The named expression list may
** be NULL.  The pSrc corresponds to the FROM clause of a SELECT or
** to the table being operated on by INSERT, UPDATE, or DELETE.  The
** pEList corresponds to the result set of a SELECT and is NULL for
** other statements.
**
** NameContexts can be nested.  When resolving names, the inner-most
** context is searched first.  If no match is found, the next outer
** context is checked.  If there is still no match, the next context
** is checked.  This process continues until either a match is found
** or all contexts are check.  When a match is found, the nRef member of
** the context containing the match is incremented.
**
** Each subquery gets a new NameContext.  The pNext field points to the
** NameContext in the parent query.  Thus the process of scanning the
** NameContext list corresponds to searching through successively outer
** subqueries looking for a match.
*/
struct NameContext {
  Parse *pParse;       /* The parser */
  SrcList *pSrcList;   /* One or more tables used to resolve names */
  union {
    ExprList *pEList;    /* Optional list of result-set columns */
    AggInfo *pAggInfo;   /* Information about aggregates at this level */
    Upsert *pUpsert;     /* ON CONFLICT clause information from an upsert */
    int iBaseReg;        /* For TK_REGISTER when parsing RETURNING */
  } uNC;
  NameContext *pNext;  /* Next outer name context.  NULL for outermost */
  int nRef;            /* Number of names resolved by this context */
  int nNcErr;          /* Number of errors encountered while resolving names */
  int ncFlags;         /* Zero or more NC_* flags defined below */
  u32 nNestedSelect;   /* Number of nested selects using this NC */
  Select *pWinSelect;  /* SELECT statement for any window functions */
};

/*
** Allowed values for the NameContext, ncFlags field.
**
** Value constraints (all checked via assert()):
**    NC_HasAgg    == SF_HasAgg       == EP_Agg
**    NC_MinMaxAgg == SF_MinMaxAgg    == CAPDB_FUNC_MINMAX
**    NC_OrderAgg  == SF_OrderByReqd  == CAPDB_FUNC_ANYORDER
**    NC_HasWin    == EP_Win
**
*/
#define NC_AllowAgg  0x000001 /* Aggregate functions are allowed here */
#define NC_PartIdx   0x000002 /* True if resolving a partial index WHERE */
#define NC_IsCheck   0x000004 /* True if resolving a CHECK constraint */
#define NC_GenCol    0x000008 /* True for a GENERATED ALWAYS AS clause */
#define NC_HasAgg    0x000010 /* One or more aggregate functions seen */
#define NC_IdxExpr   0x000020 /* True if resolving columns of CREATE INDEX */
#define NC_SelfRef   0x00002e /* Combo: PartIdx, isCheck, GenCol, and IdxExpr */
#define NC_Subquery  0x000040 /* A subquery has been seen */
#define NC_UEList    0x000080 /* True if uNC.pEList is used */
#define NC_UAggInfo  0x000100 /* True if uNC.pAggInfo is used */
#define NC_UUpsert   0x000200 /* True if uNC.pUpsert is used */
#define NC_UBaseReg  0x000400 /* True if uNC.iBaseReg is used */
#define NC_MinMaxAgg 0x001000 /* min/max aggregates seen.  See note above */
/*                   0x002000 // available for reuse */
#define NC_AllowWin  0x004000 /* Window functions are allowed here */
#define NC_HasWin    0x008000 /* One or more window functions seen */
#define NC_IsDDL     0x010000 /* Resolving names in a CREATE statement */
#define NC_InAggFunc 0x020000 /* True if analyzing arguments to an agg func */
#define NC_FromDDL   0x040000 /* SQL text comes from sqlite_schema */
#define NC_NoSelect  0x080000 /* Do not descend into sub-selects */
#define NC_Where     0x100000 /* Processing WHERE clause of a SELECT */
#define NC_OrderAgg 0x8000000 /* Has an aggregate other than count/min/max */

/*
** An instance of the following object describes a single ON CONFLICT
** clause in an upsert.
**
** The pUpsertTarget field is only set if the ON CONFLICT clause includes
** conflict-target clause.  (In "ON CONFLICT(a,b)" the "(a,b)" is the
** conflict-target clause.)  The pUpsertTargetWhere is the optional
** WHERE clause used to identify partial unique indexes.
**
** pUpsertSet is the list of column=expr terms of the UPDATE statement.
** The pUpsertSet field is NULL for a ON CONFLICT DO NOTHING.  The
** pUpsertWhere is the WHERE clause for the UPDATE and is NULL if the
** WHERE clause is omitted.
*/
struct Upsert {
  ExprList *pUpsertTarget;  /* Optional description of conflict target */
  Expr *pUpsertTargetWhere; /* WHERE clause for partial index targets */
  ExprList *pUpsertSet;     /* The SET clause from an ON CONFLICT UPDATE */
  Expr *pUpsertWhere;       /* WHERE clause for the ON CONFLICT UPDATE */
  Upsert *pNextUpsert;      /* Next ON CONFLICT clause in the list */
  u8 isDoUpdate;            /* True for DO UPDATE.  False for DO NOTHING */
  u8 isDup;                 /* True if 2nd or later with same pUpsertIdx */
  /* Above this point is the parse tree for the ON CONFLICT clauses.
  ** The next group of fields stores intermediate data. */
  void *pToFree;            /* Free memory when deleting the Upsert object */
  /* All fields above are owned by the Upsert object and must be freed
  ** when the Upsert is destroyed.  The fields below are used to transfer
  ** information from the INSERT processing down into the UPDATE processing
  ** while generating code.  The fields below are owned by the INSERT
  ** statement and will be freed by INSERT processing. */
  Index *pUpsertIdx;        /* UNIQUE constraint specified by pUpsertTarget */
  SrcList *pUpsertSrc;      /* Table to be updated */
  int regData;              /* First register holding array of VALUES */
  int iDataCur;             /* Index of the data cursor */
  int iIdxCur;              /* Index of the first index cursor */
};

/*
** An instance of the following structure contains all information
** needed to generate code for a single SELECT statement.
*/
struct Select {
  u8 op;                 /* One of: TK_UNION TK_ALL TK_INTERSECT TK_EXCEPT */
  LogEst nSelectRow;     /* Estimated number of result rows */
  u32 selFlags;          /* Various SF_* values */
  int iLimit, iOffset;   /* Memory registers holding LIMIT & OFFSET counters */
  u32 selId;             /* Unique identifier number for this SELECT */
  ExprList *pEList;      /* The fields of the result */
  SrcList *pSrc;         /* The FROM clause */
  Expr *pWhere;          /* The WHERE clause */
  ExprList *pGroupBy;    /* The GROUP BY clause */
  Expr *pHaving;         /* The HAVING clause */
  ExprList *pOrderBy;    /* The ORDER BY clause */
  Select *pPrior;        /* Prior select in a compound select statement */
  Select *pNext;         /* Next select to the left in a compound */
  Expr *pLimit;          /* LIMIT expression. NULL means not used. */
  With *pWith;           /* WITH clause attached to this select. Or NULL. */
#ifndef CAPDB_OMIT_WINDOWFUNC
  Window *pWin;          /* List of window functions */
  Window *pWinDefn;      /* List of named window definitions */
#endif
};

/*
** Allowed values for Select.selFlags.  The "SF" prefix stands for
** "Select Flag".
**
** Value constraints (all checked via assert())
**     SF_HasAgg      == NC_HasAgg
**     SF_MinMaxAgg   == NC_MinMaxAgg     == CAPDB_FUNC_MINMAX
**     SF_OrderByReqd == NC_OrderAgg      == CAPDB_FUNC_ANYORDER
**     SF_FixedLimit  == WHERE_USE_LIMIT
*/
#define SF_Distinct      0x0000001 /* Output should be DISTINCT */
#define SF_All           0x0000002 /* Includes the ALL keyword */
#define SF_Resolved      0x0000004 /* Identifiers have been resolved */
#define SF_Aggregate     0x0000008 /* Contains agg functions or a GROUP BY */
#define SF_HasAgg        0x0000010 /* Contains aggregate functions */
#define SF_ClonedRhsIn   0x0000020 /* Cloned RHS of an IN operator */
#define SF_Expanded      0x0000040 /* capdbSelectExpand() called on this */
#define SF_HasTypeInfo   0x0000080 /* FROM subqueries have Table metadata */
#define SF_Compound      0x0000100 /* Part of a compound query */
#define SF_Values        0x0000200 /* Synthesized from VALUES clause */
#define SF_MultiValue    0x0000400 /* Single VALUES term with multiple rows */
#define SF_NestedFrom    0x0000800 /* Part of a parenthesized FROM clause */
#define SF_MinMaxAgg     0x0001000 /* Aggregate containing min() or max() */
#define SF_Recursive     0x0002000 /* The recursive part of a recursive CTE */
#define SF_FixedLimit    0x0004000 /* nSelectRow set by a constant LIMIT */
/*                       0x0008000 // available for reuse */
#define SF_Converted     0x0010000 /* By convertCompoundSelectToSubquery() */
#define SF_IncludeHidden 0x0020000 /* Include hidden columns in output */
#define SF_ComplexResult 0x0040000 /* Result contains subquery or function */
#define SF_WhereBegin    0x0080000 /* Really a WhereBegin() call.  Debug Only */
#define SF_WinRewrite    0x0100000 /* Window function rewrite accomplished */
#define SF_View          0x0200000 /* SELECT statement is a view */
/*                       0x0400000 // available for reuse */
#define SF_UFSrcCheck    0x0800000 /* Check pSrc as required by UPDATE...FROM */
#define SF_PushDown      0x1000000 /* Modified by WHERE-clause push-down opt */
#define SF_MultiPart     0x2000000 /* Has multiple incompatible PARTITIONs */
#define SF_CopyCte       0x4000000 /* SELECT statement is a copy of a CTE */
#define SF_OrderByReqd   0x8000000 /* The ORDER BY clause may not be omitted */
#define SF_UpdateFrom   0x10000000 /* Query originates with UPDATE FROM */
#define SF_Correlated   0x20000000 /* True if references the outer context */
#define SF_OnToWhere    0x40000000 /* One or more ON clauses moved to WHERE */

/* True if SrcItem X is a subquery that has SF_NestedFrom */
#define IsNestedFrom(X) \
   ((X)->fg.isSubquery && \
    ((X)->u4.pSubq->pSelect->selFlags&SF_NestedFrom)!=0)

/*
** The results of a SELECT can be distributed in several ways, as defined
** by one of the following macros.  The "SRT" prefix means "SELECT Result
** Type".
**
**     SRT_Exists      Store a 1 in memory cell pDest->iSDParm if the result
**                     set is not empty.
**
**     SRT_Discard     Throw the results away.  This is used by SELECT
**                     statements within triggers whose only purpose is
**                     the side-effects of functions.
**
**     SRT_Output      Generate a row of output (using the OP_ResultRow
**                     opcode) for each row in the result set.
**
**     SRT_Mem         Only valid if the result is a single column.
**                     Store the first column of the first result row
**                     in register pDest->iSDParm then abandon the rest
**                     of the query.  This destination implies "LIMIT 1".
**
**     SRT_Set         The result must be a single column.  Store each
**                     row of result as the key in table pDest->iSDParm.
**                     Apply the affinity pDest->affSdst before storing
**                     results.  if pDest->iSDParm2 is positive, then it is
**                     a register holding a Bloom filter for the IN operator
**                     that should be populated in addition to the
**                     pDest->iSDParm table.  This SRT is used to
**                     implement "IN (SELECT ...)".
**
**     SRT_EphemTab    Create an temporary table pDest->iSDParm and store
**                     the result there. The cursor is left open after
**                     returning.  This is like SRT_Table except that
**                     this destination uses OP_OpenEphemeral to create
**                     the table first.
**
**     SRT_Coroutine   Generate a co-routine that returns a new row of
**                     results each time it is invoked.  The entry point
**                     of the co-routine is stored in register pDest->iSDParm
**                     and the result row is stored in pDest->nDest registers
**                     starting with pDest->iSdst.
**
**     SRT_Table       Store results in temporary table pDest->iSDParm.
**     SRT_Fifo        This is like SRT_EphemTab except that the table
**                     is assumed to already be open.  SRT_Fifo has
**                     the additional property of being able to ignore
**                     the ORDER BY clause.
**
**     SRT_DistFifo    Store results in a temporary table pDest->iSDParm.
**                     But also use temporary table pDest->iSDParm+1 as
**                     a record of all prior results and ignore any duplicate
**                     rows.  Name means:  "Distinct Fifo".
**
**     SRT_Queue       Store results in priority queue pDest->iSDParm (really
**                     an index).  Append a sequence number so that all entries
**                     are distinct.
**
**     SRT_DistQueue   Store results in priority queue pDest->iSDParm only if
**                     the same record has never been stored before.  The
**                     index at pDest->iSDParm+1 hold all prior stores.
**
**     SRT_Upfrom      Store results in the temporary table already opened by
**                     pDest->iSDParm. If (pDest->iSDParm<0), then the temp
**                     table is an intkey table - in this case the first
**                     column returned by the SELECT is used as the integer
**                     key. If (pDest->iSDParm>0), then the table is an index
**                     table. (pDest->iSDParm) is the number of key columns in
**                     each index record in this case.
*/
#define SRT_Exists       1  /* Store 1 if the result is not empty */
#define SRT_Discard      2  /* Do not save the results anywhere */
#define SRT_DistFifo     3  /* Like SRT_Fifo, but unique results only */
#define SRT_DistQueue    4  /* Like SRT_Queue, but unique results only */

/* The DISTINCT clause is ignored for all of the above.  Not that
** IgnorableDistinct() implies IgnorableOrderby() */
#define IgnorableDistinct(X) ((X->eDest)<=SRT_DistQueue)

#define SRT_Queue        5  /* Store result in an queue */
#define SRT_Fifo         6  /* Store result as data with an automatic rowid */

/* The ORDER BY clause is ignored for all of the above */
#define IgnorableOrderby(X) ((X->eDest)<=SRT_Fifo)

#define SRT_Output       7  /* Output each row of result */
#define SRT_Mem          8  /* Store result in a memory cell */
#define SRT_Set          9  /* Store results as keys in an index */
#define SRT_EphemTab    10  /* Create transient tab and store like SRT_Table */
#define SRT_Coroutine   11  /* Generate a single row of result */
#define SRT_Table       12  /* Store result as data with an automatic rowid */
#define SRT_Upfrom      13  /* Store result as data with rowid */

/*
** An instance of this object describes where to put of the results of
** a SELECT statement.
*/
struct SelectDest {
  u8 eDest;            /* How to dispose of the results.  One of SRT_* above. */
  int iSDParm;         /* A parameter used by the eDest disposal method */
  int iSDParm2;        /* A second parameter for the eDest disposal method */
  int iSdst;           /* Base register where results are written */
  int nSdst;           /* Number of registers allocated */
  char *zAffSdst;      /* Affinity used for SRT_Set */
  ExprList *pOrderBy;  /* Key columns for SRT_Queue and SRT_DistQueue */
};

/*
** During code generation of statements that do inserts into AUTOINCREMENT
** tables, the following information is attached to the Table.u.autoInc.p
** pointer of each autoincrement table to record some side information that
** the code generator needs.  We have to keep per-table autoincrement
** information in case inserts are done within triggers.  Triggers do not
** normally coordinate their activities, but we do need to coordinate the
** loading and saving of autoincrement information.
*/
struct AutoincInfo {
  AutoincInfo *pNext;   /* Next info block in a list of them all */
  Table *pTab;          /* Table this info block refers to */
  int iDb;              /* Index in capdb.aDb[] of database holding pTab */
  int regCtr;           /* Memory register holding the rowid counter */
};

/*
** At least one instance of the following structure is created for each
** trigger that may be fired while parsing an INSERT, UPDATE or DELETE
** statement. All such objects are stored in the linked list headed at
** Parse.pTriggerPrg and deleted once statement compilation has been
** completed.
**
** A Vdbe sub-program that implements the body and WHEN clause of trigger
** TriggerPrg.pTrigger, assuming a default ON CONFLICT clause of
** TriggerPrg.orconf, is stored in the TriggerPrg.pProgram variable.
** The Parse.pTriggerPrg list never contains two entries with the same
** values for both pTrigger and orconf.
**
** The TriggerPrg.aColmask[0] variable is set to a mask of old.* columns
** accessed (or set to 0 for triggers fired as a result of INSERT
** statements). Similarly, the TriggerPrg.aColmask[1] variable is set to
** a mask of new.* columns used by the program.
*/
struct TriggerPrg {
  Trigger *pTrigger;      /* Trigger this program was coded from */
  TriggerPrg *pNext;      /* Next entry in Parse.pTriggerPrg list */
  SubProgram *pProgram;   /* Program implementing pTrigger/orconf */
  int orconf;             /* Default ON CONFLICT policy */
  u32 aColmask[2];        /* Masks of old.*, new.* columns accessed */
};

/*
** The yDbMask datatype for the bitmask of all attached databases.
*/
#if CAPDB_MAX_ATTACHED>30
  typedef unsigned char yDbMask[(CAPDB_MAX_ATTACHED+9)/8];
# define DbMaskTest(M,I)    (((M)[(I)/8]&(1<<((I)&7)))!=0)
# define DbMaskZero(M)      memset((M),0,sizeof(M))
# define DbMaskSet(M,I)     (M)[(I)/8]|=(1<<((I)&7))
# define DbMaskAllZero(M)   capdbDbMaskAllZero(M)
# define DbMaskNonZero(M)   (capdbDbMaskAllZero(M)==0)
#else
  typedef unsigned int yDbMask;
# define DbMaskTest(M,I)    (((M)&(((yDbMask)1)<<(I)))!=0)
# define DbMaskZero(M)      ((M)=0)
# define DbMaskSet(M,I)     ((M)|=(((yDbMask)1)<<(I)))
# define DbMaskAllZero(M)   ((M)==0)
# define DbMaskNonZero(M)   ((M)!=0)
#endif

/*
** For each index X that has as one of its arguments either an expression
** or the name of a virtual generated column, and if X is in scope such that
** the value of the expression can simply be read from the index, then
** there is an instance of this object on the Parse.pIdxExpr list.
**
** During code generation, while generating code to evaluate expressions,
** this list is consulted and if a matching expression is found, the value
** is read from the index rather than being recomputed.
*/
struct IndexedExpr {
  Expr *pExpr;            /* The expression contained in the index */
  int iDataCur;           /* The data cursor associated with the index */
  int iIdxCur;            /* The index cursor */
  int iIdxCol;            /* The index column that contains value of pExpr */
  u8 bMaybeNullRow;       /* True if we need an OP_IfNullRow check */
  u8 aff;                 /* Affinity of the pExpr expression */
  IndexedExpr *pIENext;   /* Next in a list of all indexed expressions */
#ifdef CAPDB_ENABLE_EXPLAIN_COMMENTS
  const char *zIdxName;   /* Name of index, used only for bytecode comments */
#endif
};

/*
** An instance of the ParseCleanup object specifies an operation that
** should be performed after parsing to deallocation resources obtained
** during the parse and which are no longer needed.
*/
struct ParseCleanup {
  ParseCleanup *pNext;               /* Next cleanup task */
  void *pPtr;                        /* Pointer to object to deallocate */
  void (*xCleanup)(capdb*,void*);  /* Deallocation routine */
};

/*
** An SQL parser context.  A copy of this structure is passed through
** the parser and down into all the parser action routine in order to
** carry around information that is global to the entire parse.
**
** The structure is divided into two parts.  When the parser and code
** generate call themselves recursively, the first part of the structure
** is constant but the second part is reset at the beginning and end of
** each recursion.
**
** The nTableLock and aTableLock variables are only used if the shared-cache
** feature is enabled (if capdbTsd()->useSharedData is true). They are
** used to store the set of table-locks required by the statement being
** compiled. Function capdbTableLock() is used to add entries to the
** list.
*/
struct Parse {
  capdb *db;         /* The main database structure */
  char *zErrMsg;       /* An error message */
  Vdbe *pVdbe;         /* An engine for executing database bytecode */
  int rc;              /* Return code from execution */
  LogEst nQueryLoop;   /* Est number of iterations of a query (10*log2(N)) */
  u8 nested;           /* Number of nested calls to the parser/code generator */
  u8 nTempReg;         /* Number of temporary registers in aTempReg[] */
  u8 isMultiWrite;     /* True if statement may modify/insert multiple rows */
  u8 disableLookaside; /* Number of times lookaside has been disabled */
  u8 prepFlags;        /* CAPDB_PREPARE_* flags */
  u8 withinRJSubrtn;   /* Nesting level for RIGHT JOIN body subroutines */
  u8 mSubrtnSig;       /* mini Bloom filter on available SubrtnSig.selId */
  u8 eTriggerOp;       /* TK_UPDATE, TK_INSERT or TK_DELETE */
  u8 eOrconf;          /* Default ON CONFLICT policy for trigger steps */
#if defined(CAPDB_DEBUG) || defined(CAPDB_COVERAGE_TEST)
  u8 earlyCleanup;     /* OOM inside capdbParserAddCleanup() */
#endif
#ifdef CAPDB_DEBUG
  u8 ifNotExists;      /* Might be true if IF NOT EXISTS.  Assert()s only */
  u8 isCreate;         /* CREATE TABLE, INDEX, or VIEW (but not TRIGGER)
                       ** and ALTER TABLE ADD COLUMN. */
#endif
  bft disableTriggers:1; /* True to disable triggers */
  bft mayAbort :1;     /* True if statement may throw an ABORT exception */
  bft hasCompound :1;  /* Need to invoke convertCompoundSelectToSubquery() */
  bft bReturning :1;   /* Coding a RETURNING trigger */
  bft bHasExists :1;   /* Has a correlated "EXISTS (SELECT ....)" expression */
  bft colNamesSet :1;  /* TRUE after OP_ColumnName has been issued to pVdbe */
  bft bHasWith :1;     /* True if statement contains WITH */
  bft okConstFactor:1; /* OK to factor out constants */
  bft checkSchema :1;  /* Causes schema cookie check after an error */
  int nRangeReg;       /* Size of the temporary register block */
  int iRangeReg;       /* First register in temporary register block */
  int nErr;            /* Number of errors seen */
  int nTab;            /* Number of previously allocated VDBE cursors */
  int nMem;            /* Number of memory cells used so far */
  int szOpAlloc;       /* Bytes of memory space allocated for Vdbe.aOp[] */
  int iSelfTab;        /* Table associated with an index on expr, or negative
                       ** of the base register during check-constraint eval */
  int nLabel;          /* The *negative* of the number of labels used */
  int nLabelAlloc;     /* Number of slots in aLabel */
  int *aLabel;         /* Space to hold the labels */
  ExprList *pConstExpr;/* Constant expressions */
  IndexedExpr *pIdxEpr;/* List of expressions used by active indexes */
  IndexedExpr *pIdxPartExpr; /* Exprs constrained by index WHERE clauses */
  yDbMask writeMask;   /* Start a write transaction on these databases */
  yDbMask cookieMask;  /* Bitmask of schema verified databases */
  int nMaxArg;         /* Max args to xUpdate and xFilter vtab methods */
  int nSelect;         /* Number of SELECT stmts. Counter for Select.selId */
#ifndef CAPDB_OMIT_PROGRESS_CALLBACK
  u32 nProgressSteps;  /* xProgress steps taken during capdb_prepare() */
#endif
#ifndef CAPDB_OMIT_SHARED_CACHE
  int nTableLock;        /* Number of locks in aTableLock */
  TableLock *aTableLock; /* Required table locks for shared-cache mode */
#endif
  AutoincInfo *pAinc;  /* Information about AUTOINCREMENT counters */
  Parse *pToplevel;    /* Parse structure for main program (or NULL) */
  Table *pTriggerTab;  /* Table triggers are being coded for */
  TriggerPrg *pTriggerPrg;  /* Linked list of coded triggers */
  ParseCleanup *pCleanup;   /* List of cleanup operations to run after parse */

  /**************************************************************************
  ** Fields above must be initialized to zero.  The fields that follow,
  ** down to the beginning of the recursive section, do not need to be
  ** initialized as they will be set before being used.  The boundary is
  ** determined by offsetof(Parse,aTempReg).
  **************************************************************************/

  int aTempReg[8];        /* Holding area for temporary registers */
  Parse *pOuterParse;     /* Outer Parse object when nested */
  Token sNameToken;       /* Token with unqualified schema object name */
  u32 oldmask;            /* Mask of old.* columns referenced */
  u32 newmask;            /* Mask of new.* columns referenced */
  union {
    struct {  /* These fields available when isCreate is true */
      int addrCrTab;        /* Address of OP_CreateBtree on CREATE TABLE */
      int regRowid;         /* Register holding rowid of CREATE TABLE entry */
      int regRoot;          /* Register holding root page for new objects */
      Token constraintName; /* Name of the constraint currently being parsed */
    } cr;
    struct {  /* These fields available to all other statements */
      Returning *pReturning; /* The RETURNING clause */
    } d;
  } u1;

  /************************************************************************
  ** Above is constant between recursions.  Below is reset before and after
  ** each recursion.  The boundary between these two regions is determined
  ** using offsetof(Parse,sLastToken) so the sLastToken field must be the
  ** first field in the recursive region.
  ************************************************************************/

  Token sLastToken;       /* The last token parsed */
  ynVar nVar;               /* Number of '?' variables seen in the SQL so far */
  u8 iPkSortOrder;          /* ASC or DESC for INTEGER PRIMARY KEY */
  u8 explain;               /* True if the EXPLAIN flag is found on the query */
  u8 eParseMode;            /* PARSE_MODE_XXX constant */
#ifndef CAPDB_OMIT_VIRTUALTABLE
  int nVtabLock;            /* Number of virtual tables to lock */
#endif
  int nHeight;              /* Expression tree height of current sub-select */
  int addrExplain;          /* Address of current OP_Explain opcode */
  VList *pVList;            /* Mapping between variable names and numbers */
  Vdbe *pReprepare;         /* VM being reprepared (capdbReprepare()) */
  const char *zTail;        /* All SQL text past the last semicolon parsed */
  Table *pNewTable;         /* A table being constructed by CREATE TABLE */
  Index *pNewIndex;         /* An index being constructed by CREATE INDEX.
                            ** Also used to hold redundant UNIQUE constraints
                            ** during a RENAME COLUMN */
  Trigger *pNewTrigger;     /* Trigger under construct by a CREATE TRIGGER */
  const char *zAuthContext; /* The 6th parameter to db->xAuth callbacks */
#ifndef CAPDB_OMIT_VIRTUALTABLE
  Token sArg;               /* Complete text of a module argument */
  Table **apVtabLock;       /* Pointer to virtual tables needing locking */
#endif
  With *pWith;              /* Current WITH clause, or NULL */
#ifndef CAPDB_OMIT_ALTERTABLE
  RenameToken *pRename;     /* Tokens subject to renaming by ALTER TABLE */
#endif
};

/* Allowed values for Parse.eParseMode
*/
#define PARSE_MODE_NORMAL        0
#define PARSE_MODE_DECLARE_VTAB  1
#define PARSE_MODE_RENAME        2
#define PARSE_MODE_UNMAP         3

/*
** Sizes and pointers of various parts of the Parse object.
*/
#define PARSE_HDR(X)  (((char*)(X))+offsetof(Parse,zErrMsg))
#define PARSE_HDR_SZ (offsetof(Parse,aTempReg)-offsetof(Parse,zErrMsg)) /* Recursive part w/o aColCache*/
#define PARSE_RECURSE_SZ offsetof(Parse,sLastToken)    /* Recursive part */
#define PARSE_TAIL_SZ (sizeof(Parse)-PARSE_RECURSE_SZ) /* Non-recursive part */
#define PARSE_TAIL(X) (((char*)(X))+PARSE_RECURSE_SZ)  /* Pointer to tail */

/*
** Return true if currently inside an capdb_declare_vtab() call.
*/
#ifdef CAPDB_OMIT_VIRTUALTABLE
  #define IN_DECLARE_VTAB 0
#else
  #define IN_DECLARE_VTAB (pParse->eParseMode==PARSE_MODE_DECLARE_VTAB)
#endif

#if defined(CAPDB_OMIT_ALTERTABLE)
  #define IN_RENAME_OBJECT 0
#else
  #define IN_RENAME_OBJECT (pParse->eParseMode>=PARSE_MODE_RENAME)
#endif

#if defined(CAPDB_OMIT_VIRTUALTABLE) && defined(CAPDB_OMIT_ALTERTABLE)
  #define IN_SPECIAL_PARSE 0
#else
  #define IN_SPECIAL_PARSE (pParse->eParseMode!=PARSE_MODE_NORMAL)
#endif

/*
** An instance of the following structure can be declared on a stack and used
** to save the Parse.zAuthContext value so that it can be restored later.
*/
struct AuthContext {
  const char *zAuthContext;   /* Put saved Parse.zAuthContext here */
  Parse *pParse;              /* The Parse structure */
};

/*
** Bitfield flags for P5 value in various opcodes.
**
** Value constraints (enforced via assert()):
**    OPFLAG_LENGTHARG    == CAPDB_FUNC_LENGTH
**    OPFLAG_TYPEOFARG    == CAPDB_FUNC_TYPEOF
**    OPFLAG_BULKCSR      == BTREE_BULKLOAD
**    OPFLAG_SEEKEQ       == BTREE_SEEK_EQ
**    OPFLAG_FORDELETE    == BTREE_FORDELETE
**    OPFLAG_SAVEPOSITION == BTREE_SAVEPOSITION
**    OPFLAG_AUXDELETE    == BTREE_AUXDELETE
*/
#define OPFLAG_NCHANGE       0x01    /* OP_Insert: Set to update db->nChange */
                                     /* Also used in P2 (not P5) of OP_Delete */
#define OPFLAG_NOCHNG        0x01    /* OP_VColumn nochange for UPDATE */
#define OPFLAG_EPHEM         0x01    /* OP_Column: Ephemeral output is ok */
#define OPFLAG_LASTROWID     0x20    /* Set to update db->lastRowid */
#define OPFLAG_ISUPDATE      0x04    /* This OP_Insert is an sql UPDATE */
#define OPFLAG_APPEND        0x08    /* This is likely to be an append */
#define OPFLAG_USESEEKRESULT 0x10    /* Try to avoid a seek in BtreeInsert() */
#define OPFLAG_ISNOOP        0x40    /* OP_Delete does pre-update-hook only */
#define OPFLAG_LENGTHARG     0x40    /* OP_Column only used for length() */
#define OPFLAG_TYPEOFARG     0x80    /* OP_Column only used for typeof() */
#define OPFLAG_BYTELENARG    0xc0    /* OP_Column only for octet_length() */
#define OPFLAG_BULKCSR       0x01    /* OP_Open** used to open bulk cursor */
#define OPFLAG_SEEKEQ        0x02    /* OP_Open** cursor uses EQ seek only */
#define OPFLAG_FORDELETE     0x08    /* OP_Open should use BTREE_FORDELETE */
#define OPFLAG_P2ISREG       0x10    /* P2 to OP_Open** is a register number */
#define OPFLAG_PERMUTE       0x01    /* OP_Compare: use the permutation */
#define OPFLAG_SAVEPOSITION  0x02    /* OP_Delete/Insert: save cursor pos */
#define OPFLAG_AUXDELETE     0x04    /* OP_Delete: index in a DELETE op */
#define OPFLAG_NOCHNG_MAGIC  0x6d    /* OP_MakeRecord: serialtype 10 is ok */
#define OPFLAG_PREFORMAT     0x80    /* OP_Insert uses preformatted cell */

/*
** Each trigger present in the database schema is stored as an instance of
** struct Trigger.
**
** Pointers to instances of struct Trigger are stored in two ways.
** 1. In the "trigHash" hash table (part of the capdb* that represents the
**    database). This allows Trigger structures to be retrieved by name.
** 2. All triggers associated with a single table form a linked list, using the
**    pNext member of struct Trigger. A pointer to the first element of the
**    linked list is stored as the "pTrigger" member of the associated
**    struct Table.
**
** The "step_list" member points to the first element of a linked list
** containing the SQL statements specified as the trigger program.
*/
struct Trigger {
  char *zName;            /* The name of the trigger                        */
  char *table;            /* The table or view to which the trigger applies */
  u8 op;                  /* One of TK_DELETE, TK_UPDATE, TK_INSERT         */
  u8 tr_tm;               /* One of TRIGGER_BEFORE, TRIGGER_AFTER */
  u8 bReturning;          /* This trigger implements a RETURNING clause */
  Expr *pWhen;            /* The WHEN clause of the expression (may be NULL) */
  IdList *pColumns;       /* If this is an UPDATE OF <column-list> trigger,
                             the <column-list> is stored here */
  Schema *pSchema;        /* Schema containing the trigger */
  Schema *pTabSchema;     /* Schema containing the table */
  TriggerStep *step_list; /* Link list of trigger program steps             */
  Trigger *pNext;         /* Next trigger associated with the table */
};

/*
** A trigger is either a BEFORE or an AFTER trigger.  The following constants
** determine which.
**
** If there are multiple triggers, you might of some BEFORE and some AFTER.
** In that cases, the constants below can be ORed together.
*/
#define TRIGGER_BEFORE  1
#define TRIGGER_AFTER   2

/*
** An instance of struct TriggerStep is used to store a single SQL statement
** that is a part of a trigger-program.
**
** Instances of struct TriggerStep are stored in a singly linked list (linked
** using the "pNext" member) referenced by the "step_list" member of the
** associated struct Trigger instance. The first element of the linked list is
** the first step of the trigger-program.
**
** The "op" member indicates whether this is a "DELETE", "INSERT", "UPDATE" or
** "SELECT" statement. The meanings of the other members is determined by the
** value of "op" as follows:
**
** (op == TK_INSERT)
** orconf    -> stores the ON CONFLICT algorithm
** pSelect   -> The content to be inserted - either a SELECT statement or
**              a VALUES clause.
** pSrc      -> Table to insert into.
** pIdList   -> If this is an INSERT INTO ... (<column-names>) VALUES ...
**              statement, then this stores the column-names to be
**              inserted into.
** pUpsert   -> The ON CONFLICT clauses for an Upsert
**
** (op == TK_DELETE)
** pSrc      -> Table to delete from
** pWhere    -> The WHERE clause of the DELETE statement if one is specified.
**              Otherwise NULL.
**
** (op == TK_UPDATE)
** pSrc      -> Table to update, followed by any FROM clause tables.
** pWhere    -> The WHERE clause of the UPDATE statement if one is specified.
**              Otherwise NULL.
** pExprList -> A list of the columns to update and the expressions to update
**              them to. See capdbUpdate() documentation of "pChanges"
**              argument.
**
** (op == TK_SELECT)
** pSelect   -> The SELECT statement
**
** (op == TK_RETURNING)
** pExprList -> The list of expressions that follow the RETURNING keyword.
**
*/
struct TriggerStep {
  u8 op;               /* One of TK_DELETE, TK_UPDATE, TK_INSERT, TK_SELECT,
                       ** or TK_RETURNING */
  u8 orconf;           /* OE_Rollback etc. */
  Trigger *pTrig;      /* The trigger that this step is a part of */
  Select *pSelect;     /* SELECT statement or RHS of INSERT INTO SELECT ... */
  SrcList *pSrc;       /* Table to insert/update/delete */
  Expr *pWhere;        /* The WHERE clause for DELETE or UPDATE steps */
  ExprList *pExprList; /* SET clause for UPDATE, or RETURNING clause */
  IdList *pIdList;     /* Column names for INSERT */
  Upsert *pUpsert;     /* Upsert clauses on an INSERT */
  char *zSpan;         /* Original SQL text of this command */
  TriggerStep *pNext;  /* Next in the link-list */
  TriggerStep *pLast;  /* Last element in link-list. Valid for 1st elem only */
};

/*
** Information about a RETURNING clause
*/
struct Returning {
  Parse *pParse;        /* The parse that includes the RETURNING clause */
  ExprList *pReturnEL;  /* List of expressions to return */
  Trigger retTrig;      /* The transient trigger that implements RETURNING */
  TriggerStep retTStep; /* The trigger step */
  int iRetCur;          /* Transient table holding RETURNING results */
  int nRetCol;          /* Number of in pReturnEL after expansion */
  int iRetReg;          /* Register array for holding a row of RETURNING */
  char zName[40];       /* Name of trigger: "sqlite_returning_%p" */
};

/*
** An object used to accumulate the text of a string where we
** do not necessarily know how big the string will be in the end.
*/
struct capdb_str {
  capdb *db;         /* Optional database for lookaside.  Can be NULL */
  char *zText;         /* The string collected so far */
  u32  nAlloc;         /* Amount of space allocated in zText */
  u32  mxAlloc;        /* Maximum allowed allocation.  0 for no malloc usage */
  u32  nChar;          /* Length of the string so far */
  u8   accError;       /* CAPDB_NOMEM or CAPDB_TOOBIG */
  u8   printfFlags;    /* CAPDB_PRINTF flags below */
};
#define CAPDB_PRINTF_INTERNAL 0x01  /* Internal-use-only converters allowed */
#define CAPDB_PRINTF_SQLFUNC  0x02  /* SQL function arguments to VXPrintf */
#define CAPDB_PRINTF_MALLOCED 0x04  /* True if zText is allocated space */

#define isMalloced(X)  (((X)->printfFlags & CAPDB_PRINTF_MALLOCED)!=0)

/*
** The following object is the header for an "RCStr" or "reference-counted
** string".  An RCStr is passed around and used like any other char*
** that has been dynamically allocated.  The important interface
** differences:
**
**   1.  RCStr strings are reference counted.  They are deallocated
**       when the reference count reaches zero.
**
**   2.  Use capdbRCStrUnref() to free an RCStr string rather than
**       capdb_free()
**
**   3.  Make a (read-only) copy of a read-only RCStr string using
**       capdbRCStrRef().
**
** "String" is in the name, but an RCStr object can also be used to hold
** binary data.
*/
struct RCStr {
  u64 nRCRef;            /* Number of references */
  /* Total structure size should be a multiple of 8 bytes for alignment */
};

/*
** A pointer to this structure is used to communicate information
** from capdbInit and OP_ParseSchema into the capdbInitCallback.
*/
typedef struct {
  capdb *db;        /* The database being initialized */
  char **pzErrMsg;    /* Error message stored here */
  int iDb;            /* 0 for main database.  1 for TEMP, 2.. for ATTACHed */
  int rc;             /* Result code stored here */
  u32 mInitFlags;     /* Flags controlling error messages */
  u32 nInitRow;       /* Number of rows processed */
  Pgno mxPage;        /* Maximum page number.  0 for no limit. */
} InitData;

/*
** Allowed values for mInitFlags
*/
#define INITFLAG_AlterMask     0x0007  /* Types of ALTER */
#define INITFLAG_AlterRename   0x0001  /* Reparse after a RENAME */
#define INITFLAG_AlterDrop     0x0002  /* Reparse after a DROP COLUMN */
#define INITFLAG_AlterAdd      0x0003  /* Reparse after an ADD COLUMN */
#define INITFLAG_AlterDropCons 0x0004  /* Reparse after a DROP CONSTRAINT */

/* Tuning parameters are set using CAPDB_TESTCTRL_TUNE and are controlled
** on debug-builds of the CLI using ".testctrl tune ID VALUE".  Tuning
** parameters are for temporary use during development, to help find
** optimal values for parameters in the query planner.  The should not
** be used on trunk check-ins.  They are a temporary mechanism available
** for transient development builds only.
**
** Tuning parameters are numbered starting with 1.
*/
#define CAPDB_NTUNE  6             /* Should be zero for all trunk check-ins */
#ifdef CAPDB_DEBUG
# define Tuning(X)  (capdbConfig.aTune[(X)-1])
#else
# define Tuning(X)  0
#endif

/*
** Structure containing global configuration data for the SQLite library.
**
** This structure also contains some state information.
*/
struct Sqlite3Config {
  int bMemstat;                     /* True to enable memory status */
  u8 bCoreMutex;                    /* True to enable core mutexing */
  u8 bFullMutex;                    /* True to enable full mutexing */
  u8 bOpenUri;                      /* True to interpret filenames as URIs */
  u8 bUseCis;                       /* Use covering indices for full-scans */
  u8 bSmallMalloc;                  /* Avoid large memory allocations if true */
  u8 bExtraSchemaChecks;            /* Verify type,name,tbl_name in schema */
#ifdef CAPDB_DEBUG
  u8 bJsonSelfcheck;                /* Double-check JSON parsing */
#endif
  int mxStrlen;                     /* Maximum string length */
  int neverCorrupt;                 /* Database is always well-formed */
  int szLookaside;                  /* Default lookaside buffer size */
  int nLookaside;                   /* Default lookaside buffer count */
  int nStmtSpill;                   /* Stmt-journal spill-to-disk threshold */
  capdb_mem_methods m;            /* Low-level memory allocation interface */
  capdb_mutex_methods mutex;      /* Low-level mutex interface */
  capdb_pcache_methods2 pcache2;  /* Low-level page-cache interface */
  void *pHeap;                      /* Heap storage space */
  int nHeap;                        /* Size of pHeap[] */
  int mnReq, mxReq;                 /* Min and max heap requests sizes */
  capdb_int64 szMmap;             /* mmap() space per open file */
  capdb_int64 mxMmap;             /* Maximum value for szMmap */
  void *pPage;                      /* Page cache memory */
  int szPage;                       /* Size of each page in pPage[] */
  int nPage;                        /* Number of pages in pPage[] */
  int mxParserStack;                /* maximum depth of the parser stack */
  int sharedCacheEnabled;           /* true if shared-cache mode enabled */
  u32 szPma;                        /* Maximum Sorter PMA size */
  /* The above might be initialized to non-zero.  The following need to always
  ** initially be zero, however. */
  int isInit;                       /* True after initialization has finished */
  int inProgress;                   /* True while initialization in progress */
  int isMutexInit;                  /* True after mutexes are initialized */
  int isMallocInit;                 /* True after malloc is initialized */
  int isPCacheInit;                 /* True after malloc is initialized */
  int nRefInitMutex;                /* Number of users of pInitMutex */
  capdb_mutex *pInitMutex;        /* Mutex used by capdb_initialize() */
  void (*xLog)(void*,int,const char*); /* Function for logging */
  void *pLogArg;                       /* First argument to xLog() */
#ifdef CAPDB_ENABLE_SQLLOG
  void(*xSqllog)(void*,capdb*,const char*, int);
  void *pSqllogArg;
#endif
#ifdef CAPDB_VDBE_COVERAGE
  /* The following callback (if not NULL) is invoked on every VDBE branch
  ** operation.  Set the callback using CAPDB_TESTCTRL_VDBE_COVERAGE.
  */
  void (*xVdbeBranch)(void*,unsigned iSrcLine,u8 eThis,u8 eMx);  /* Callback */
  void *pVdbeBranchArg;                                     /* 1st argument */
#endif
#ifndef CAPDB_OMIT_DESERIALIZE
  capdb_int64 mxMemdbSize;        /* Default max memdb size */
#endif
#ifndef CAPDB_UNTESTABLE
  int (*xTestCallback)(int);        /* Invoked by capdbFaultSim() */
#endif
#ifdef CAPDB_ALLOW_ROWID_IN_VIEW
  u32 mNoVisibleRowid;              /* TF_NoVisibleRowid if the ROWID_IN_VIEW
                                    ** feature is disabled.  0 if rowids can
                                    ** occur in views. */
#endif
  int bLocaltimeFault;              /* True to fail localtime() calls */
  int (*xAltLocaltime)(const void*,void*); /* Alternative localtime() routine */
  int iOnceResetThreshold;          /* When to reset OP_Once counters */
  u32 szSorterRef;                  /* Min size in bytes to use sorter-refs */
  unsigned int iPrngSeed;           /* Alternative fixed seed for the PRNG */
  /* vvvv--- must be last ---vvv */
#ifdef CAPDB_DEBUG
  capdb_int64 aTune[CAPDB_NTUNE]; /* Tuning parameters */
#endif
};

/*
** This macro is used inside of assert() statements to indicate that
** the assert is only valid on a well-formed database.  Instead of:
**
**     assert( X );
**
** One writes:
**
**     assert( X || CORRUPT_DB );
**
** CORRUPT_DB is true during normal operation.  CORRUPT_DB does not indicate
** that the database is definitely corrupt, only that it might be corrupt.
** For most test cases, CORRUPT_DB is set to false using a special
** capdb_test_control().  This enables assert() statements to prove
** things that are always true for well-formed databases.
*/
#define CORRUPT_DB  (capdbConfig.neverCorrupt==0)

/*
** Context pointer passed down through the tree-walk.
*/
struct Walker {
  Parse *pParse;                            /* Parser context.  */
  int (*xExprCallback)(Walker*, Expr*);     /* Callback for expressions */
  int (*xSelectCallback)(Walker*,Select*);  /* Callback for SELECTs */
  void (*xSelectCallback2)(Walker*,Select*);/* Second callback for SELECTs */
  int walkerDepth;                          /* Number of subqueries */
  u16 eCode;                                /* A small processing code */
  u16 mWFlags;                              /* Use-dependent flags */
  union {                                   /* Extra data for callback */
    NameContext *pNC;                         /* Naming context */
    int n;                                    /* A counter */
    int iCur;                                 /* A cursor number */
    int sz;                                   /* String literal length */
    SrcList *pSrcList;                        /* FROM clause */
    struct CCurHint *pCCurHint;               /* Used by codeCursorHint() */
    struct RefSrcList *pRefSrcList;           /* capdbReferencesSrcList() */
    int *aiCol;                               /* array of column indexes */
    struct IdxCover *pIdxCover;               /* Check for index coverage */
    ExprList *pGroupBy;                       /* GROUP BY clause */
    Select *pSelect;                          /* HAVING to WHERE clause ctx */
    struct WindowRewrite *pRewrite;           /* Window rewrite context */
    struct WhereConst *pConst;                /* WHERE clause constants */
    struct RenameCtx *pRename;                /* RENAME COLUMN context */
    struct Table *pTab;                       /* Table of generated column */
    struct CoveringIndexCheck *pCovIdxCk;     /* Check for covering index */
    SrcItem *pSrcItem;                        /* A single FROM clause item */
    DbFixer *pFix;                            /* See capdbFixSelect() */
    Mem *aMem;                                /* See capdbBtreeCursorHint() */
    struct CheckOnCtx *pCheckOnCtx;           /* See selectCheckOnClauses() */
  } u;
};

/*
** The following structure contains information used by the sqliteFix...
** routines as they walk the parse tree to make database references
** explicit.
*/
struct DbFixer {
  Parse *pParse;      /* The parsing context.  Error messages written here */
  Walker w;           /* Walker object */
  Schema *pSchema;    /* Fix items to this schema */
  u8 bTemp;           /* True for TEMP schema entries */
  const char *zDb;    /* Make sure all objects are contained in this database */
  const char *zType;  /* Type of the container - used for error messages */
  const Token *pName; /* Name of the container - used for error messages */
};

/* Forward declarations */
int capdbWalkExpr(Walker*, Expr*);
int capdbWalkExprNN(Walker*, Expr*);
int capdbWalkExprList(Walker*, ExprList*);
int capdbWalkSelect(Walker*, Select*);
int capdbWalkSelectExpr(Walker*, Select*);
int capdbWalkSelectFrom(Walker*, Select*);
int capdbExprWalkNoop(Walker*, Expr*);
int capdbSelectWalkNoop(Walker*, Select*);
int capdbSelectWalkFail(Walker*, Select*);
int capdbWalkerDepthIncrease(Walker*,Select*);
void capdbWalkerDepthDecrease(Walker*,Select*);
void capdbWalkWinDefnDummyCallback(Walker*,Select*);

#ifdef CAPDB_DEBUG
void capdbSelectWalkAssert2(Walker*, Select*);
#endif

#ifndef CAPDB_OMIT_CTE
void capdbSelectPopWith(Walker*, Select*);
#else
# define capdbSelectPopWith 0
#endif

/*
** Return code from the parse-tree walking primitives and their
** callbacks.
*/
#define WRC_Continue    0   /* Continue down into children */
#define WRC_Prune       1   /* Omit children but continue walking siblings */
#define WRC_Abort       2   /* Abandon the tree walk */

/*
** A single common table expression
*/
struct Cte {
  char *zName;            /* Name of this CTE */
  ExprList *pCols;        /* List of explicit column names, or NULL */
  Select *pSelect;        /* The definition of this CTE */
  const char *zCteErr;    /* Error message for circular references */
  CteUse *pUse;           /* Usage information for this CTE */
  u8 eM10d;               /* The MATERIALIZED flag */
};

/*
** Allowed values for the materialized flag (eM10d):
*/
#define M10d_Yes       0  /* AS MATERIALIZED */
#define M10d_Any       1  /* Not specified.  Query planner's choice */
#define M10d_No        2  /* AS NOT MATERIALIZED */

/*
** An instance of the With object represents a WITH clause containing
** one or more CTEs (common table expressions).
*/
struct With {
  int nCte;               /* Number of CTEs in the WITH clause */
  int bView;              /* Belongs to the outermost Select of a view */
  With *pOuter;           /* Containing WITH clause, or NULL */
  Cte a[FLEXARRAY];       /* For each CTE in the WITH clause.... */
};

/* The size (in bytes) of a With object that can hold as many
** as N different CTEs. */
#define SZ_WITH(N)  (offsetof(With,a) + (N)*sizeof(Cte))

/*
** The Cte object is not guaranteed to persist for the entire duration
** of code generation.  (The query flattener or other parser tree
** edits might delete it.)  The following object records information
** about each Common Table Expression that must be preserved for the
** duration of the parse.
**
** The CteUse objects are freed using capdbParserAddCleanup() rather
** than capdbSelectDelete(), which is what enables them to persist
** until the end of code generation.
*/
struct CteUse {
  int nUse;              /* Number of users of this CTE */
  int addrM9e;           /* Start of subroutine to compute materialization */
  int regRtn;            /* Return address register for addrM9e subroutine */
  int iCur;              /* Ephemeral table holding the materialization */
  LogEst nRowEst;        /* Estimated number of rows in the table */
  u8 eM10d;              /* The MATERIALIZED flag */
};


/* Client data associated with capdb_set_clientdata() and
** capdb_get_clientdata().
*/
struct DbClientData {
  DbClientData *pNext;        /* Next in a linked list */
  void *pData;                /* The data */
  void (*xDestructor)(void*); /* Destructor.  Might be NULL */
  char zName[FLEXARRAY];      /* Name of this client data. MUST BE LAST */
};

/* The size (in bytes) of a DbClientData object that can has a name
** that is N bytes long, including the zero-terminator. */
#define SZ_DBCLIENTDATA(N) (offsetof(DbClientData,zName)+(N))

#ifdef CAPDB_DEBUG
/*
** An instance of the TreeView object is used for printing the content of
** data structures on capdbDebugPrintf() using a tree-like view.
*/
struct TreeView {
  int iLevel;             /* Which level of the tree we are on */
  u8  bLine[100];         /* Draw vertical in column i if bLine[i] is true */
};
#endif /* CAPDB_DEBUG */

/*
** This object is used in various ways, most (but not all) related to window
** functions.
**
**   (1) A single instance of this structure is attached to the
**       the Expr.y.pWin field for each window function in an expression tree.
**       This object holds the information contained in the OVER clause,
**       plus additional fields used during code generation.
**
**   (2) All window functions in a single SELECT form a linked-list
**       attached to Select.pWin.  The Window.pFunc and Window.pExpr
**       fields point back to the expression that is the window function.
**
**   (3) The terms of the WINDOW clause of a SELECT are instances of this
**       object on a linked list attached to Select.pWinDefn.
**
**   (4) For an aggregate function with a FILTER clause, an instance
**       of this object is stored in Expr.y.pWin with eFrmType set to
**       TK_FILTER. In this case the only field used is Window.pFilter.
**
** The uses (1) and (2) are really the same Window object that just happens
** to be accessible in two different ways.  Use case (3) are separate objects.
*/
struct Window {
  char *zName;            /* Name of window (may be NULL) */
  char *zBase;            /* Name of base window for chaining (may be NULL) */
  ExprList *pPartition;   /* PARTITION BY clause */
  ExprList *pOrderBy;     /* ORDER BY clause */
  u8 eFrmType;            /* TK_RANGE, TK_GROUPS, TK_ROWS, or 0 */
  u8 eStart;              /* UNBOUNDED, CURRENT, PRECEDING or FOLLOWING */
  u8 eEnd;                /* UNBOUNDED, CURRENT, PRECEDING or FOLLOWING */
  u8 bImplicitFrame;      /* True if frame was implicitly specified */
  u8 eExclude;            /* TK_NO, TK_CURRENT, TK_TIES, TK_GROUP, or 0 */
  Expr *pStart;           /* Expression for "<expr> PRECEDING" */
  Expr *pEnd;             /* Expression for "<expr> FOLLOWING" */
  Window **ppThis;        /* Pointer to this object in Select.pWin list */
  Window *pNextWin;       /* Next window function belonging to this SELECT */
  Expr *pFilter;          /* The FILTER expression */
  FuncDef *pWFunc;        /* The function */
  int iEphCsr;            /* Partition buffer or Peer buffer */
  int regAccum;           /* Accumulator */
  int regResult;          /* Interim result */
  int csrApp;             /* Function cursor (used by min/max) */
  int regApp;             /* Function register (also used by min/max) */
  int regPart;            /* Array of registers for PARTITION BY values */
  Expr *pOwner;           /* Expression object this window is attached to */
  int nBufferCol;         /* Number of columns in buffer table */
  int iArgCol;            /* Offset of first argument for this function */
  int regOne;             /* Register containing constant value 1 */
  int regStartRowid;
  int regEndRowid;
  u8 bExprArgs;           /* Defer evaluation of window function arguments
                          ** due to the CAPDB_SUBTYPE flag */
};

Select *capdbMultiValues(Parse *pParse, Select *pLeft, ExprList *pRow);
void capdbMultiValuesEnd(Parse *pParse, Select *pVal);

#ifndef CAPDB_OMIT_WINDOWFUNC
void capdbWindowDelete(capdb*, Window*);
void capdbWindowUnlinkFromSelect(Window*);
void capdbWindowListDelete(capdb *db, Window *p);
Window *capdbWindowAlloc(Parse*, int, int, Expr*, int , Expr*, u8);
void capdbWindowAttach(Parse*, Expr*, Window*);
void capdbWindowLink(Select *pSel, Window *pWin);
int capdbWindowCompare(const Parse*, const Window*, const Window*, int);
void capdbWindowCodeInit(Parse*, Select*);
void capdbWindowCodeStep(Parse*, Select*, WhereInfo*, int, int);
int capdbWindowRewrite(Parse*, Select*);
void capdbWindowUpdate(Parse*, Window*, Window*, FuncDef*);
Window *capdbWindowDup(capdb *db, Expr *pOwner, Window *p);
Window *capdbWindowListDup(capdb *db, Window *p);
void capdbWindowFunctions(void);
void capdbWindowChain(Parse*, Window*, Window*);
Window *capdbWindowAssemble(Parse*, Window*, ExprList*, ExprList*, Token*);
#else
# define capdbWindowDelete(a,b)
# define capdbWindowFunctions()
# define capdbWindowAttach(a,b,c)
#endif

/*
** Assuming zIn points to the first byte of a UTF-8 character,
** advance zIn to point to the first byte of the next UTF-8 character.
**
** # Dividing malformed UTF-8 into characters (tag-20260418-01)
**
** If a text input is malformed UTF-8, SQLite does not make any guarantees
** about how the bytes are divided up into characters.  The system promises
** to not overflow an array or cause other memory errors when presented
** with malformed UTF-8.  And it promises to preserve the specific
** sequence of bytes as long as no conversion occur.  But beyond that,
** there are no guarantees.  Results can vary from one version to the
** next.
**
** The CAPDB_SKIP_UTF8 macro below is one technique for dividing UTF-8
** into characters.  The length() and substr() SQL functions use a
** different technique when searching across multiple characters, a
** technique that exchanges a subtraction for comparison of z and results
** in faster machine code on some compilers and architectures.  The code
** in substr() to skip over p1 characters goes something like this:
**
**    for( ; p1>0; p1--){
**                     // vvvv--- tag-20260418-01
**      if( (u8)(z[0]-1)<(0x80-1) ){
**        z++;
**      }else if( z[0]==0 ){
**        break;
**      }else{
**        do{ z++; }while( (z[0]&0xc0)==0x80 );
**      }
**    }
**
** In valid UTF-8, multibyte characters always begin with a byte with the
** two most significant bits set and that is followed by one or more bytes
** for which the two most significant bits are 10.  In other words:
**
**     First byte:        (BYTE & 0xc0)==0xc0
**     Following bytes:   (BYTE & 0xc0)==0x80
**
** What to do if the input byte sequence contain a "following byte" that
** is not preceded by a "first byte"?  How many characters are in the
** byte sequence:  0x61 0x81 0x82 0x7a?  3 or 4 or something else?
** 
** If you use the macro below, the answer will be 4.  If you use the code
** snippet demonstrated at tag-20260418-01, then answer is 3.  If you
** use a variant of tag-20260418-01 where the constant of comparison is
** 0xc0-1 instead of 0x80-1 then the answer is again 4.  The key point is
** that because the input is malformed UTF-8, so is no "correct" answer.
** SQLite is free to use either value.
**
** It turns out that GCC 13.3.0 is able to generate faster code (at least
** on x86-64) if the constant at tag-20260418-01 is (0x80-1).  If you make
** that constant (0xc0-1) instead, gcc 13.3.0 generates code that runs slower.
** So the (0x80-1) constant is used for substr() and length().
*/
#define CAPDB_SKIP_UTF8(zIn) {                        \
  if( (*(zIn++))>=0xc0 ){                              \
    while( (*zIn & 0xc0)==0x80 ){ zIn++; }             \
  }                                                    \
}

/*
** The CAPDB_*_BKPT macros are substitutes for the error codes with
** the same name but without the _BKPT suffix.  These macros invoke
** routines that report the line-number on which the error originated
** using capdb_log().  The routines also provide a convenient place
** to set a debugger breakpoint.
*/
int capdbReportError(int iErr, int lineno, const char *zType);
int capdbCorruptError(int);
int capdbMisuseError(int);
int capdbCantopenError(int);
#define CAPDB_CORRUPT_BKPT capdbCorruptError(__LINE__)
#define CAPDB_MISUSE_BKPT capdbMisuseError(__LINE__)
#define CAPDB_CANTOPEN_BKPT capdbCantopenError(__LINE__)
#ifdef CAPDB_DEBUG
  int capdbNomemError(int);
  int capdbIoerrnomemError(int);
# define CAPDB_NOMEM_BKPT capdbNomemError(__LINE__)
# define CAPDB_IOERR_NOMEM_BKPT capdbIoerrnomemError(__LINE__)
#else
# define CAPDB_NOMEM_BKPT CAPDB_NOMEM
# define CAPDB_IOERR_NOMEM_BKPT CAPDB_IOERR_NOMEM
#endif
#if defined(CAPDB_DEBUG) || defined(CAPDB_ENABLE_CORRUPT_PGNO)
  int capdbCorruptPgnoError(int,Pgno);
# define CAPDB_CORRUPT_PGNO(P) capdbCorruptPgnoError(__LINE__,(P))
#else
# define CAPDB_CORRUPT_PGNO(P) capdbCorruptError(__LINE__)
#endif

/*
** FTS3 and FTS4 both require virtual table support
*/
#if defined(CAPDB_OMIT_VIRTUALTABLE)
# undef CAPDB_ENABLE_FTS3
# undef CAPDB_ENABLE_FTS4
#endif

/*
** FTS4 is really an extension for FTS3.  It is enabled using the
** CAPDB_ENABLE_FTS3 macro.  But to avoid confusion we also call
** the CAPDB_ENABLE_FTS4 macro to serve as an alias for CAPDB_ENABLE_FTS3.
*/
#if defined(CAPDB_ENABLE_FTS4) && !defined(CAPDB_ENABLE_FTS3)
# define CAPDB_ENABLE_FTS3 1
#endif

/*
** The following macros mimic the standard library functions toupper(),
** isspace(), isalnum(), isdigit() and isxdigit(), respectively. The
** sqlite versions only work for ASCII characters, regardless of locale.
*/
#ifdef CAPDB_ASCII
# define capdbToupper(x)  ((x)&~(capdbCtypeMap[(unsigned char)(x)]&0x20))
# define capdbIsspace(x)   (capdbCtypeMap[(unsigned char)(x)]&0x01)
# define capdbIsalnum(x)   (capdbCtypeMap[(unsigned char)(x)]&0x06)
# define capdbIsalpha(x)   (capdbCtypeMap[(unsigned char)(x)]&0x02)
# define capdbIsdigit(x)   (capdbCtypeMap[(unsigned char)(x)]&0x04)
# define capdbIsxdigit(x)  (capdbCtypeMap[(unsigned char)(x)]&0x08)
# define capdbTolower(x)   (capdbUpperToLower[(unsigned char)(x)])
# define capdbIsquote(x)   (capdbCtypeMap[(unsigned char)(x)]&0x80)
# define capdbJsonId1(x)   (capdbCtypeMap[(unsigned char)(x)]&0x42)
# define capdbJsonId2(x)   (capdbCtypeMap[(unsigned char)(x)]&0x46)
#else
# define capdbToupper(x)   toupper((unsigned char)(x))
# define capdbIsspace(x)   isspace((unsigned char)(x))
# define capdbIsalnum(x)   isalnum((unsigned char)(x))
# define capdbIsalpha(x)   isalpha((unsigned char)(x))
# define capdbIsdigit(x)   isdigit((unsigned char)(x))
# define capdbIsxdigit(x)  isxdigit((unsigned char)(x))
# define capdbTolower(x)   tolower((unsigned char)(x))
# define capdbIsquote(x)   ((x)=='"'||(x)=='\''||(x)=='['||(x)=='`')
# define capdbJsonId1(x)   (capdbIsIdChar(x)&&(x)<'0')
# define capdbJsonId2(x)   capdbIsIdChar(x)
#endif
int capdbIsIdChar(u8);

/*
** Internal function prototypes
*/
int capdbStrICmp(const char*,const char*);
int capdbStrlen30(const char*);
#define capdbStrlen30NN(C) (strlen(C)&0x3fffffff)
char *capdbColumnType(Column*,char*);
#define capdbStrNICmp capdb_strnicmp

int capdbMallocInit(void);
void capdbMallocEnd(void);
void *capdbMalloc(u64);
void *capdbMallocZero(u64);
void *capdbDbMallocZero(capdb*, u64);
void *capdbDbMallocRaw(capdb*, u64);
void *capdbDbMallocRawNN(capdb*, u64);
char *capdbDbStrDup(capdb*,const char*);
char *capdbDbStrNDup(capdb*,const char*, u64);
char *capdbDbSpanDup(capdb*,const char*,const char*);
void *capdbRealloc(void*, u64);
void *capdbDbReallocOrFree(capdb *, void *, u64);
void *capdbDbRealloc(capdb *, void *, u64);
void capdbDbFree(capdb*, void*);
void capdbDbFreeNN(capdb*, void*);
void capdbDbNNFreeNN(capdb*, void*);
int capdbMallocSize(const void*);
int capdbDbMallocSize(capdb*, const void*);
void *capdbPageMalloc(int);
void capdbPageFree(void*);
void capdbMemSetDefault(void);
#ifndef CAPDB_UNTESTABLE
void capdbBenignMallocHooks(void (*)(void), void (*)(void));
#endif
int capdbHeapNearlyFull(void);

/*
** On systems with ample stack space and that support alloca(), make
** use of alloca() to obtain space for large automatic objects.  By default,
** obtain space from malloc().
**
** The alloca() routine never returns NULL.  This will cause code paths
** that deal with capdbStackAlloc() failures to be unreachable.
*/
#ifdef CAPDB_USE_ALLOCA
# define capdbStackAllocRaw(D,N)   alloca(N)
# define capdbStackAllocRawNN(D,N) alloca(N)
# define capdbStackFree(D,P)
# define capdbStackFreeNN(D,P)
#else
# define capdbStackAllocRaw(D,N)   capdbDbMallocRaw(D,N)
# define capdbStackAllocRawNN(D,N) capdbDbMallocRawNN(D,N)
# define capdbStackFree(D,P)       capdbDbFree(D,P)
# define capdbStackFreeNN(D,P)     capdbDbFreeNN(D,P)
#endif

/* Do not allow both MEMSYS5 and MEMSYS3 to be defined together.  If they
** are, disable MEMSYS3
*/
#ifdef CAPDB_ENABLE_MEMSYS5
const capdb_mem_methods *capdbMemGetMemsys5(void);
#undef CAPDB_ENABLE_MEMSYS3
#endif
#ifdef CAPDB_ENABLE_MEMSYS3
const capdb_mem_methods *capdbMemGetMemsys3(void);
#endif


#ifndef CAPDB_MUTEX_OMIT
  capdb_mutex_methods const *capdbDefaultMutex(void);
  capdb_mutex_methods const *capdbNoopMutex(void);
  capdb_mutex *capdbMutexAlloc(int);
  int capdbMutexInit(void);
  int capdbMutexEnd(void);
#endif
#if !defined(CAPDB_MUTEX_OMIT) && !defined(CAPDB_MUTEX_NOOP)
  void capdbMemoryBarrier(void);
#else
# define capdbMemoryBarrier()
#endif

capdb_int64 capdbStatusValue(int);
void capdbStatusUp(int, int);
void capdbStatusDown(int, int);
void capdbStatusHighwater(int, int);
int capdbLookasideUsed(capdb*,int*);

/* Access to mutexes used by capdb_status() */
capdb_mutex *capdbPcache1Mutex(void);
capdb_mutex *capdbMallocMutex(void);


/* The CAPDB_THREAD_MISUSE_WARNINGS compile-time option used to be called
** CAPDB_ENABLE_MULTITHREADED_CHECKS.  Keep that older macro for backwards
** compatibility, at least for a while... */
#ifdef CAPDB_ENABLE_MULTITHREADED_CHECKS
# define CAPDB_THREAD_MISUSE_WARNINGS 1
#endif

/* CAPDB_THREAD_MISUSE_ABORT implies CAPDB_THREAD_MISUSE_WARNINGS */
#ifdef CAPDB_THREAD_MISUSE_ABORT
# define CAPDB_THREAD_MISUSE_WARNINGS 1
#endif

#if defined(CAPDB_THREAD_MISUSE_WARNINGS) && !defined(CAPDB_MUTEX_OMIT)
void capdbMutexWarnOnContention(capdb_mutex*);
#else
# define capdbMutexWarnOnContention(x)
#endif

#ifndef CAPDB_OMIT_FLOATING_POINT
# define EXP754 (((u64)0x7ff)<<52)
# define MAN754 ((((u64)1)<<52)-1)
# define IsNaN(X) (((X)&EXP754)==EXP754 && ((X)&MAN754)!=0)
# define IsOvfl(X) (((X)&EXP754)==EXP754)
  int capdbIsNaN(double);
  int capdbIsOverflow(double);
#else
# define IsNaN(X)             0
# define capdbIsNaN(X)      0
# define capdbIsOVerflow(X) 0
#endif

/*
** An instance of the following structure holds information about SQL
** functions arguments that are the parameters to the printf() function.
*/
struct PrintfArguments {
  int nArg;                /* Total number of arguments */
  int nUsed;               /* Number of arguments used so far */
  capdb_value **apArg;   /* The argument values */
};

/*
** Maxium number of base-10 digits in an unsigned 64-bit integer
*/
#define CAPDB_U64_DIGITS 20

/*
** An instance of this object receives the decoding of a floating point
** value into an approximate decimal representation.
*/
struct FpDecode {
  int n;                           /* Significant digits in the decode */
  int iDP;                         /* Location of the decimal point */
  char *z;                         /* Start of significant digits */
  char zBuf[CAPDB_U64_DIGITS+1];  /* Storage for significant digits */
  char sign;                       /* '+' or '-' */
  char isSpecial;                  /* 1: Infinity  2: NaN */
};

void capdbFpDecode(FpDecode*,double,int,int);
char *capdbMPrintf(capdb*,const char*, ...);
char *capdbVMPrintf(capdb*,const char*, va_list);
#if defined(CAPDB_DEBUG) || defined(CAPDB_HAVE_OS_TRACE)
  void capdbDebugPrintf(const char*, ...);
#endif
#if defined(CAPDB_TEST)
  void *capdbTestTextToPtr(const char*);
#endif

#if defined(CAPDB_DEBUG)
  void capdbTreeViewLine(TreeView*, const char *zFormat, ...);
  void capdbTreeViewExpr(TreeView*, const Expr*, u8);
  void capdbTreeViewBareExprList(TreeView*, const ExprList*, const char*);
  void capdbTreeViewExprList(TreeView*, const ExprList*, u8, const char*);
  void capdbTreeViewBareIdList(TreeView*, const IdList*, const char*);
  void capdbTreeViewIdList(TreeView*, const IdList*, u8, const char*);
  void capdbTreeViewColumnList(TreeView*, const Column*, int, u8);
  void capdbTreeViewSrcList(TreeView*, const SrcList*);
  void capdbTreeViewSelect(TreeView*, const Select*, u8);
  void capdbTreeViewWith(TreeView*, const With*, u8);
  void capdbTreeViewUpsert(TreeView*, const Upsert*, u8);
#if TREETRACE_ENABLED
  void capdbTreeViewDelete(const With*, const SrcList*, const Expr*,
                             const ExprList*,const Expr*, const Trigger*);
  void capdbTreeViewInsert(const With*, const SrcList*,
                             const IdList*, const Select*, const ExprList*,
                             int, const Upsert*, const Trigger*);
  void capdbTreeViewUpdate(const With*, const SrcList*, const ExprList*,
                             const Expr*, int, const ExprList*, const Expr*,
                             const Upsert*, const Trigger*);
#endif
#ifndef CAPDB_OMIT_TRIGGER
  void capdbTreeViewTriggerStep(TreeView*, const TriggerStep*, u8, u8);
  void capdbTreeViewTrigger(TreeView*, const Trigger*, u8, u8);
#endif
#ifndef CAPDB_OMIT_WINDOWFUNC
  void capdbTreeViewWindow(TreeView*, const Window*, u8);
  void capdbTreeViewWinFunc(TreeView*, const Window*, u8);
#endif
  void capdbShowExpr(const Expr*);
  void capdbShowExprList(const ExprList*);
  void capdbShowIdList(const IdList*);
  void capdbShowSrcList(const SrcList*);
  void capdbShowSelect(const Select*);
  void capdbShowWith(const With*);
  void capdbShowUpsert(const Upsert*);
#ifndef CAPDB_OMIT_TRIGGER
  void capdbShowTriggerStep(const TriggerStep*);
  void capdbShowTriggerStepList(const TriggerStep*);
  void capdbShowTrigger(const Trigger*);
  void capdbShowTriggerList(const Trigger*);
#endif
#ifndef CAPDB_OMIT_WINDOWFUNC
  void capdbShowWindow(const Window*);
  void capdbShowWinFunc(const Window*);
#endif
  void capdbShowBitvec(Bitvec*);
#endif

void capdbSetString(char **, capdb*, const char*);
void capdbProgressCheck(Parse*);
void capdbErrorMsg(Parse*, const char*, ...);
int capdbErrorToParser(capdb*,int);
void capdbDequote(char*);
void capdbDequoteExpr(Expr*);
void capdbDequoteToken(Token*);
void capdbDequoteNumber(Parse*, Expr*);
void capdbTokenInit(Token*,char*);
int capdbKeywordCode(const unsigned char*, int);
int capdbRunParser(Parse*, const char*);
void capdbFinishCoding(Parse*);
int capdbGetTempReg(Parse*);
void capdbReleaseTempReg(Parse*,int);
int capdbGetTempRange(Parse*,int);
void capdbReleaseTempRange(Parse*,int,int);
void capdbClearTempRegCache(Parse*);
void capdbTouchRegister(Parse*,int);
#if defined(CAPDB_ENABLE_STAT4) || defined(CAPDB_DEBUG)
int capdbFirstAvailableRegister(Parse*,int);
#endif
#ifdef CAPDB_DEBUG
int capdbNoTempsInRange(Parse*,int,int);
#endif
Expr *capdbExprAlloc(capdb*,int,const Token*,int);
Expr *capdbExpr(capdb*,int,const char*);
Expr *capdbExprInt32(capdb*,int);
void capdbExprAttachSubtrees(capdb*,Expr*,Expr*,Expr*);
Expr *capdbPExpr(Parse*, int, Expr*, Expr*);
void capdbPExprAddSelect(Parse*, Expr*, Select*);
Expr *capdbExprAnd(Parse*,Expr*, Expr*);
Expr *capdbExprSimplifiedAndOr(Expr*);
Expr *capdbExprFunction(Parse*,ExprList*, const Token*, int);
void capdbExprAddFunctionOrderBy(Parse*,Expr*,ExprList*);
void capdbExprOrderByAggregateError(Parse*,Expr*);
void capdbExprFunctionUsable(Parse*,const Expr*,const FuncDef*);
void capdbExprAssignVarNumber(Parse*, Expr*, u32);
void capdbExprDelete(capdb*, Expr*);
void capdbExprDeleteGeneric(capdb*,void*);
int capdbExprDeferredDelete(Parse*, Expr*);
void capdbExprUnmapAndDelete(Parse*, Expr*);
ExprList *capdbExprListAppend(Parse*,ExprList*,Expr*);
ExprList *capdbExprListAppendVector(Parse*,ExprList*,IdList*,Expr*);
Select *capdbExprListToValues(Parse*, int, ExprList*);
void capdbExprListSetSortOrder(ExprList*,int,int);
void capdbExprListSetName(Parse*,ExprList*,const Token*,int);
void capdbExprListSetSpan(Parse*,ExprList*,const char*,const char*);
void capdbExprListDelete(capdb*, ExprList*);
void capdbExprListDeleteGeneric(capdb*,void*);
u32 capdbExprListFlags(const ExprList*);
int capdbIndexHasDuplicateRootPage(Index*);
int capdbInit(capdb*, char**);
int capdbInitCallback(void*, int, char**, char**);
int capdbInitOne(capdb*, int, char**, u32);
void capdbPragma(Parse*,Token*,Token*,Token*,int);
#ifndef CAPDB_OMIT_VIRTUALTABLE
Module *capdbPragmaVtabRegister(capdb*,const char *zName);
#endif
void capdbResetAllSchemasOfConnection(capdb*);
void capdbResetOneSchema(capdb*,int);
void capdbCollapseDatabaseArray(capdb*);
void capdbCommitInternalChanges(capdb*);
void capdbColumnSetExpr(Parse*,Table*,Column*,Expr*);
Expr *capdbColumnExpr(Table*,Column*);
void capdbColumnSetColl(capdb*,Column*,const char*zColl);
const char *capdbColumnColl(Column*);
void capdbDeleteColumnNames(capdb*,Table*);
void capdbGenerateColumnNames(Parse *pParse, Select *pSelect);
int capdbColumnsFromExprList(Parse*,ExprList*,i16*,Column**);
void capdbSubqueryColumnTypes(Parse*,Table*,Select*,char);
Table *capdbResultSetOfSelect(Parse*,Select*,char);
void capdbOpenSchemaTable(Parse *, int);
Index *capdbPrimaryKeyIndex(Table*);
int capdbTableColumnToIndex(Index*, int);
#ifdef CAPDB_OMIT_GENERATED_COLUMNS
# define capdbTableColumnToStorage(T,X) (X)  /* No-op pass-through */
# define capdbStorageColumnToTable(T,X) (X)  /* No-op pass-through */
#else
  i16 capdbTableColumnToStorage(Table*, i16);
  i16 capdbStorageColumnToTable(Table*, i16);
#endif
void capdbStartTable(Parse*,Token*,Token*,int,int,int,int);
#if CAPDB_ENABLE_HIDDEN_COLUMNS
  void capdbColumnPropertiesFromName(Table*, Column*);
#else
# define capdbColumnPropertiesFromName(T,C) /* no-op */
#endif
void capdbAddColumn(Parse*,Token,Token);
void capdbAddNotNull(Parse*, int);
void capdbAddPrimaryKey(Parse*, ExprList*, int, int, int);
void capdbAddCheckConstraint(Parse*, Expr*, const char*, const char*);
void capdbAddDefaultValue(Parse*,Expr*,const char*,const char*);
void capdbAddCollateType(Parse*, Token*);
void capdbAddGenerated(Parse*,Expr*,Token*);
void capdbEndTable(Parse*,Token*,Token*,u32,Select*);
void capdbAddReturning(Parse*,ExprList*);
int capdbParseUri(const char*,const char*,unsigned int*,
                    capdb_vfs**,char**,char **);
#define capdbCodecQueryParameters(A,B,C) 0
Btree *capdbDbNameToBtree(capdb*,const char*);

#ifdef CAPDB_UNTESTABLE
# define capdbFaultSim(X) CAPDB_OK
#else
  int capdbFaultSim(int);
#endif

Bitvec *capdbBitvecCreate(u32);
int capdbBitvecTest(Bitvec*, u32);
int capdbBitvecTestNotNull(Bitvec*, u32);
int capdbBitvecSet(Bitvec*, u32);
void capdbBitvecClear(Bitvec*, u32, void*);
void capdbBitvecDestroy(Bitvec*);
u32 capdbBitvecSize(Bitvec*);
#ifndef CAPDB_UNTESTABLE
int capdbBitvecBuiltinTest(int,int*);
#endif

RowSet *capdbRowSetInit(capdb*);
void capdbRowSetDelete(void*);
void capdbRowSetClear(void*);
void capdbRowSetInsert(RowSet*, i64);
int capdbRowSetTest(RowSet*, int iBatch, i64);
int capdbRowSetNext(RowSet*, i64*);

void capdbCreateView(Parse*,Token*,Token*,Token*,ExprList*,Select*,int,int);

#if !defined(CAPDB_OMIT_VIEW) || !defined(CAPDB_OMIT_VIRTUALTABLE)
  int capdbViewGetColumnNames(Parse*,Table*);
#else
# define capdbViewGetColumnNames(A,B) 0
#endif

#if CAPDB_MAX_ATTACHED>30
  int capdbDbMaskAllZero(yDbMask);
#endif
void capdbDropTable(Parse*, SrcList*, int, int);
void capdbCodeDropTable(Parse*, Table*, int, int);
void capdbDeleteTable(capdb*, Table*);
void capdbDeleteTableGeneric(capdb*, void*);
void capdbFreeIndex(capdb*, Index*);
#ifndef CAPDB_OMIT_AUTOINCREMENT
  void capdbAutoincrementBegin(Parse *pParse);
  void capdbAutoincrementEnd(Parse *pParse);
#else
# define capdbAutoincrementBegin(X)
# define capdbAutoincrementEnd(X)
#endif
void capdbInsert(Parse*, SrcList*, Select*, IdList*, int, Upsert*);
#ifndef CAPDB_OMIT_GENERATED_COLUMNS
  void capdbComputeGeneratedColumns(Parse*, int, Table*);
#endif
void *capdbArrayAllocate(capdb*,void*,int,int*,int*);
IdList *capdbIdListAppend(Parse*, IdList*, Token*);
int capdbIdListIndex(IdList*,const char*);
SrcList *capdbSrcListEnlarge(Parse*, SrcList*, int, int);
SrcList *capdbSrcListAppendList(Parse *pParse, SrcList *p1, SrcList *p2);
SrcList *capdbSrcListAppend(Parse*, SrcList*, Token*, Token*);
void capdbSubqueryDelete(capdb*,Subquery*);
Select *capdbSubqueryDetach(capdb*,SrcItem*);
int capdbSrcItemAttachSubquery(Parse*, SrcItem*, Select*, int);
SrcList *capdbSrcListAppendFromTerm(Parse*, SrcList*, Token*, Token*,
                                      Token*, Select*, OnOrUsing*);
void capdbSrcListIndexedBy(Parse *, SrcList *, Token *);
void capdbSrcListFuncArgs(Parse*, SrcList*, ExprList*);
int capdbIndexedByLookup(Parse *, SrcItem *);
void capdbSrcListShiftJoinType(Parse*,SrcList*);
void capdbSrcListAssignCursors(Parse*, SrcList*);
void capdbIdListDelete(capdb*, IdList*);
void capdbClearOnOrUsing(capdb*, OnOrUsing*);
void capdbSrcListDelete(capdb*, SrcList*);
Index *capdbAllocateIndexObject(capdb*,int,int,char**);
void capdbCreateIndex(Parse*,Token*,Token*,SrcList*,ExprList*,int,Token*,
                          Expr*, int, int, u8);
void capdbDropIndex(Parse*, SrcList*, int);
int capdbSelect(Parse*, Select*, SelectDest*);
Select *capdbSelectNew(Parse*,ExprList*,SrcList*,Expr*,ExprList*,
                         Expr*,ExprList*,u32,Expr*);
void capdbSelectDelete(capdb*, Select*);
void capdbSelectDeleteGeneric(capdb*,void*);
void capdbSelectCheckOnClauses(Parse *pParse, Select *pSelect);
Table *capdbSrcListLookup(Parse*, SrcList*);
int capdbIsReadOnly(Parse*, Table*, Trigger*);
void capdbOpenTable(Parse*, int iCur, int iDb, Table*, int);
#if defined(CAPDB_ENABLE_UPDATE_DELETE_LIMIT) && !defined(CAPDB_OMIT_SUBQUERY)
Expr *capdbLimitWhere(Parse*,SrcList*,Expr*,ExprList*,Expr*,char*);
#endif
void capdbCodeChangeCount(Vdbe*,int,const char*);
void capdbDeleteFrom(Parse*, SrcList*, Expr*, ExprList*, Expr*);
void capdbUpdate(Parse*, SrcList*, ExprList*,Expr*,int,ExprList*,Expr*,
                   Upsert*);
WhereInfo *capdbWhereBegin(Parse*,SrcList*,Expr*,ExprList*,
                             ExprList*,Select*,u16,int);
void capdbWhereEnd(WhereInfo*);
LogEst capdbWhereOutputRowCount(WhereInfo*);
int capdbWhereIsDistinct(WhereInfo*);
int capdbWhereIsOrdered(WhereInfo*);
int capdbWhereOrderByLimitOptLabel(WhereInfo*);
void capdbWhereMinMaxOptEarlyOut(Vdbe*,WhereInfo*);
int capdbWhereIsSorted(WhereInfo*);
int capdbWhereContinueLabel(WhereInfo*);
int capdbWhereBreakLabel(WhereInfo*);
int capdbWhereOkOnePass(WhereInfo*, int*);
#define ONEPASS_OFF      0        /* Use of ONEPASS not allowed */
#define ONEPASS_SINGLE   1        /* ONEPASS valid for a single row update */
#define ONEPASS_MULTI    2        /* ONEPASS is valid for multiple rows */
int capdbWhereUsesDeferredSeek(WhereInfo*);
void capdbExprCodeLoadIndexColumn(Parse*, Index*, int, int, int);
int capdbExprCodeGetColumn(Parse*, Table*, int, int, int, u8);
void capdbExprCodeGetColumnOfTable(Vdbe*, Table*, int, int, int);
void capdbExprCodeMove(Parse*, int, int, int);
void capdbExprToRegister(Expr *pExpr, int iReg);
void capdbExprCode(Parse*, Expr*, int);
#ifndef CAPDB_OMIT_GENERATED_COLUMNS
void capdbExprCodeGeneratedColumn(Parse*, Table*, Column*, int);
#endif
void capdbExprCodeCopy(Parse*, Expr*, int);
void capdbExprCodeFactorable(Parse*, Expr*, int);
int capdbExprCodeRunJustOnce(Parse*, Expr*, int);
void capdbExprNullRegisterRange(Parse*, int, int);
int capdbExprCodeTemp(Parse*, Expr*, int*);
int capdbExprCodeTarget(Parse*, Expr*, int);
int capdbExprCodeExprList(Parse*, ExprList*, int, int, u8);
#define CAPDB_ECEL_DUP      0x01  /* Deep, not shallow copies */
#define CAPDB_ECEL_FACTOR   0x02  /* Factor out constant terms */
#define CAPDB_ECEL_REF      0x04  /* Use ExprList.u.x.iOrderByCol */
#define CAPDB_ECEL_OMITREF  0x08  /* Omit if ExprList.u.x.iOrderByCol */
void capdbExprIfTrue(Parse*, Expr*, int, int);
void capdbExprIfFalse(Parse*, Expr*, int, int);
void capdbExprIfFalseDup(Parse*, Expr*, int, int);
Table *capdbFindTable(capdb*,const char*, const char*);
#define LOCATE_VIEW    0x01
#define LOCATE_NOERR   0x02
Table *capdbLocateTable(Parse*,u32 flags,const char*, const char*);
const char *capdbPreferredTableName(const char*);
Table *capdbLocateTableItem(Parse*,u32 flags,SrcItem *);
Index *capdbFindIndex(capdb*,const char*, const char*);
void capdbUnlinkAndDeleteTable(capdb*,int,const char*);
void capdbUnlinkAndDeleteIndex(capdb*,int,const char*);
void capdbVacuum(Parse*,Token*,Expr*);
int capdbRunVacuum(char**, capdb*, int, capdb_value*);
char *capdbNameFromToken(capdb*, const Token*);
int capdbExprCompare(const Parse*,const Expr*,const Expr*, int);
int capdbExprCompareSkip(Expr*,Expr*,int);
int capdbExprListCompare(const ExprList*,const ExprList*, int);
int capdbExprImpliesExpr(const Parse*,const Expr*,const Expr*, int);
int capdbExprImpliesNonNullRow(Expr*,int,int);
void capdbAggInfoPersistWalkerInit(Walker*,Parse*);
void capdbExprAnalyzeAggregates(NameContext*, Expr*);
void capdbExprAnalyzeAggList(NameContext*,ExprList*);
int capdbExprCoveredByIndex(Expr*, int iCur, Index *pIdx);
int capdbReferencesSrcList(Parse*, Expr*, SrcList*);
Vdbe *capdbGetVdbe(Parse*);
#ifndef CAPDB_UNTESTABLE
void capdbPrngSaveState(void);
void capdbPrngRestoreState(void);
#endif
void capdbRollbackAll(capdb*,int);
void capdbCodeVerifySchema(Parse*, int);
void capdbCodeVerifyNamedSchema(Parse*, const char *zDb);
void capdbBeginTransaction(Parse*, int);
void capdbEndTransaction(Parse*,int);
void capdbSavepoint(Parse*, int, Token*);
void capdbCloseSavepoints(capdb *);
void capdbLeaveMutexAndCloseZombie(capdb*);
u32 capdbIsTrueOrFalse(const char*);
int capdbExprIdToTrueFalse(Expr*);
int capdbExprTruthValue(const Expr*);
int capdbExprIsConstant(Parse*,Expr*);
int capdbExprIsConstantOrFunction(Expr*, u8);
int capdbExprIsConstantOrGroupBy(Parse*, Expr*, ExprList*);
int capdbExprIsSingleTableConstraint(Expr*,const SrcList*,int,int);
#ifdef CAPDB_ENABLE_CURSOR_HINTS
int capdbExprContainsSubquery(Expr*);
#endif
int capdbExprIsInteger(const Expr*, int*, Parse*);
int capdbExprCanBeNull(const Expr*);
int capdbExprNeedsNoAffinityChange(const Expr*, char);
int capdbExprIsLikeOperator(const Expr*);
int capdbIsRowid(const char*);
const char *capdbRowidAlias(Table *pTab);
void capdbGenerateRowDelete(
    Parse*,Table*,Trigger*,int,int,int,i16,u8,u8,u8,int);
void capdbGenerateRowIndexDelete(Parse*, Table*, int, int, int*, int);
int capdbGenerateIndexKey(Parse*, Index*, int, int, int, int*,Index*,int);
void capdbResolvePartIdxLabel(Parse*,int);
int capdbExprReferencesUpdatedColumn(Expr*,int*,int);
void capdbGenerateConstraintChecks(Parse*,Table*,int*,int,int,int,int,
                                     u8,u8,int,int*,int*,Upsert*);
#ifdef CAPDB_ENABLE_NULL_TRIM
  void capdbSetMakeRecordP5(Vdbe*,Table*);
#else
# define capdbSetMakeRecordP5(A,B)
#endif
void capdbCompleteInsertion(Parse*,Table*,int,int,int,int*,int,int,int);
int capdbOpenTableAndIndices(Parse*, Table*, int, u8, int, u8*, int*, int*);
void capdbBeginWriteOperation(Parse*, int, int);
void capdbMultiWrite(Parse*);
void capdbMayAbort(Parse*);
void capdbHaltConstraint(Parse*, int, int, char*, i8, u8);
void capdbUniqueConstraint(Parse*, int, Index*);
void capdbRowidConstraint(Parse*, int, Table*);
Expr *capdbExprDup(capdb*,const Expr*,int);
ExprList *capdbExprListDup(capdb*,const ExprList*,int);
SrcList *capdbSrcListDup(capdb*,const SrcList*,int);
IdList *capdbIdListDup(capdb*,const IdList*);
Select *capdbSelectDup(capdb*,const Select*,int);
FuncDef *capdbFunctionSearch(int,const char*);
void capdbInsertBuiltinFuncs(FuncDef*,int);
FuncDef *capdbFindFunction(capdb*,const char*,int,u8,u8);
void capdbQuoteValue(StrAccum*,capdb_value*,int);
int capdbAppendOneUtf8Character(char*, u32);
void capdbRegisterBuiltinFunctions(void);
void capdbRegisterDateTimeFunctions(void);
void capdbRegisterJsonFunctions(void);
void capdbRegisterPerConnectionBuiltinFunctions(capdb*);
#if !defined(CAPDB_OMIT_VIRTUALTABLE) && !defined(CAPDB_OMIT_JSON)
  Module *capdbJsonVtabRegister(capdb*,const char*);
#endif
int capdbSafetyCheckOk(capdb*);
int capdbSafetyCheckSickOrOk(capdb*);
void capdbChangeCookie(Parse*, int);
With *capdbWithDup(capdb *db, With *p);

#if !defined(CAPDB_OMIT_VIRTUALTABLE) && defined(CAPDB_ENABLE_CARRAY)
  Module *capdbCarrayRegister(capdb*);
#endif

#if !defined(CAPDB_OMIT_VIEW) && !defined(CAPDB_OMIT_TRIGGER)
void capdbMaterializeView(Parse*, Table*, Expr*, ExprList*,Expr*,int);
#endif

#ifndef CAPDB_OMIT_TRIGGER
  void capdbBeginTrigger(Parse*, Token*,Token*,int,int,IdList*,SrcList*,
                           Expr*,int, int);
  void capdbFinishTrigger(Parse*, TriggerStep*, Token*);
  void capdbDropTrigger(Parse*, SrcList*, int);
  void capdbDropTriggerPtr(Parse*, Trigger*);
  Trigger *capdbTriggersExist(Parse *, Table*, int, ExprList*, int *pMask);
  Trigger *capdbTriggerList(Parse *, Table *);
  void capdbCodeRowTrigger(Parse*, Trigger *, int, ExprList*, int, Table *,
                            int, int, int);
  void capdbCodeRowTriggerDirect(Parse *, Trigger *, Table *, int, int, int);
  void sqliteViewTriggers(Parse*, Table*, Expr*, int, ExprList*);
  void capdbDeleteTriggerStep(capdb*, TriggerStep*);
  TriggerStep *capdbTriggerSelectStep(capdb*,Select*,
                                        const char*,const char*);
  TriggerStep *capdbTriggerInsertStep(Parse*,SrcList*, IdList*,
                                        Select*,u8,Upsert*,
                                        const char*,const char*);
  TriggerStep *capdbTriggerUpdateStep(Parse*,SrcList*,SrcList*,ExprList*,
                                        Expr*, u8, const char*,const char*);
  TriggerStep *capdbTriggerDeleteStep(Parse*,SrcList*, Expr*,
                                        const char*,const char*);
  void capdbDeleteTrigger(capdb*, Trigger*);
  void capdbUnlinkAndDeleteTrigger(capdb*,int,const char*);
  u32 capdbTriggerColmask(Parse*,Trigger*,ExprList*,int,int,Table*,int);
# define capdbParseToplevel(p) ((p)->pToplevel ? (p)->pToplevel : (p))
# define capdbIsToplevel(p) ((p)->pToplevel==0)
#else
# define capdbTriggersExist(B,C,D,E,F) 0
# define capdbDeleteTrigger(A,B)
# define capdbDropTriggerPtr(A,B)
# define capdbUnlinkAndDeleteTrigger(A,B,C)
# define capdbCodeRowTrigger(A,B,C,D,E,F,G,H,I)
# define capdbCodeRowTriggerDirect(A,B,C,D,E,F)
# define capdbTriggerList(X, Y) 0
# define capdbParseToplevel(p) p
# define capdbIsToplevel(p) 1
# define capdbTriggerColmask(A,B,C,D,E,F,G) 0
#endif

int capdbJoinType(Parse*, Token*, Token*, Token*);
int capdbColumnIndex(Table *pTab, const char *zCol);
void capdbSrcItemColumnUsed(SrcItem*,int);
void capdbSetJoinExpr(Expr*,int,u32);
void capdbCreateForeignKey(Parse*, ExprList*, Token*, ExprList*, int);
void capdbDeferForeignKey(Parse*, int);
#ifndef CAPDB_OMIT_AUTHORIZATION
  void capdbAuthRead(Parse*,Expr*,Schema*,SrcList*);
  int capdbAuthCheck(Parse*,int, const char*, const char*, const char*);
  void capdbAuthContextPush(Parse*, AuthContext*, const char*);
  void capdbAuthContextPop(AuthContext*);
  int capdbAuthReadCol(Parse*, const char *, const char *, int);
#else
# define capdbAuthRead(a,b,c,d)
# define capdbAuthCheck(a,b,c,d,e)    CAPDB_OK
# define capdbAuthContextPush(a,b,c)
# define capdbAuthContextPop(a)  ((void)(a))
#endif
int capdbDbIsNamed(capdb *db, int iDb, const char *zName);
void capdbAttach(Parse*, Expr*, Expr*, Expr*);
void capdbDetach(Parse*, Expr*);
void capdbFixInit(DbFixer*, Parse*, int, const char*, const Token*);
int capdbFixSrcList(DbFixer*, SrcList*);
int capdbFixSelect(DbFixer*, Select*);
int capdbFixExpr(DbFixer*, Expr*);
int capdbFixTriggerStep(DbFixer*, TriggerStep*);

int capdbRealSameAsInt(double,capdb_int64);
i64 capdbRealToI64(double);
int capdbInt64ToText(i64,char*);
int capdbAtoF(const char *z, double*);
int capdbGetInt32(const char *, int*);
int capdbGetUInt32(const char*, u32*);
int capdbAtoi(const char*);
#ifndef CAPDB_OMIT_UTF16
int capdbUtf16ByteLen(const void *pData, int nByte, int nChar);
#endif
int capdbUtf8CharLen(const char *pData, int nByte);
u32 capdbUtf8Read(const u8**);
int capdbUtf8ReadLimited(const u8*, int, u32*);
LogEst capdbLogEst(u64);
LogEst capdbLogEstAdd(LogEst,LogEst);
LogEst capdbLogEstFromDouble(double);
u64 capdbLogEstToInt(LogEst);
VList *capdbVListAdd(capdb*,VList*,const char*,int,int);
const char *capdbVListNumToName(VList*,int);
int capdbVListNameToNum(VList*,const char*,int);

/*
** Routines to read and write variable-length integers.  These used to
** be defined locally, but now we use the varint routines in the util.c
** file.
*/
int capdbPutVarint(unsigned char*, u64);
u8 capdbGetVarint(const unsigned char *, u64 *);
u8 capdbGetVarint32(const unsigned char *, u32 *);
int capdbVarintLen(u64 v);

/*
** The common case is for a varint to be a single byte.  They following
** macros handle the common case without a procedure call, but then call
** the procedure for larger varints.
*/
#define getVarint32(A,B)  \
  (u8)((*(A)<(u8)0x80)?((B)=(u32)*(A)),1:capdbGetVarint32((A),(u32 *)&(B)))
#define getVarint32NR(A,B) \
  B=(u32)*(A);if(B>=0x80)capdbGetVarint32((A),(u32*)&(B))
#define putVarint32(A,B)  \
  (u8)(((u32)(B)<(u32)0x80)?(*(A)=(unsigned char)(B)),1:\
  capdbPutVarint((A),(B)))
#define getVarint    capdbGetVarint
#define putVarint    capdbPutVarint


const char *capdbIndexAffinityStr(capdb*, Index*);
char *capdbTableAffinityStr(capdb*,const Table*);
void capdbTableAffinity(Vdbe*, Table*, int);
char capdbCompareAffinity(const Expr *pExpr, char aff2);
int capdbIndexAffinityOk(const Expr *pExpr, char idx_affinity);
char capdbTableColumnAffinity(const Table*,int);
char capdbExprAffinity(const Expr *pExpr);
int capdbExprDataType(const Expr *pExpr);
int capdbAtoi64(const char*, i64*, int, u8);
int capdbDecOrHexToI64(const char*, i64*);
void capdbErrorWithMsg(capdb*, int, const char*,...);
void capdbError(capdb*,int);
void capdbErrorClear(capdb*);
void capdbSystemError(capdb*,int);
#if !defined(CAPDB_OMIT_BLOB_LITERAL)
void *capdbHexToBlob(capdb*, const char *z, int n);
#endif
u8 capdbHexToInt(int h);
int capdbTwoPartName(Parse *, Token *, Token *, Token **);

#if defined(CAPDB_NEED_ERR_NAME)
const char *capdbErrName(int);
#endif

#ifndef CAPDB_OMIT_DESERIALIZE
int capdbMemdbInit(void);
int capdbIsMemdb(const capdb_vfs*);
#else
# define capdbIsMemdb(X) 0
#endif

const char *capdbErrStr(int);
int capdbReadSchema(Parse *pParse);
CollSeq *capdbFindCollSeq(capdb*,u8 enc, const char*,int);
int capdbIsBinary(const CollSeq*);
CollSeq *capdbLocateCollSeq(Parse *pParse, const char*zName);
void capdbSetTextEncoding(capdb *db, u8);
CollSeq *capdbExprCollSeq(Parse *pParse, const Expr *pExpr);
CollSeq *capdbExprNNCollSeq(Parse *pParse, const Expr *pExpr);
int capdbExprCollSeqMatch(Parse*,const Expr*,const Expr*);
Expr *capdbExprAddCollateToken(const Parse *pParse, Expr*, const Token*, int);
Expr *capdbExprAddCollateString(const Parse*,Expr*,const char*);
Expr *capdbExprSkipCollate(Expr*);
Expr *capdbExprSkipCollateAndLikely(Expr*);
int capdbCheckCollSeq(Parse *, CollSeq *);
int capdbWritableSchema(capdb*);
int capdbCheckObjectName(Parse*, const char*,const char*,const char*);
void capdbVdbeSetChanges(capdb *, i64);
int capdbAddInt64(i64*,i64);
int capdbSubInt64(i64*,i64);
int capdbMulInt64(i64*,i64);
int capdbAbsInt32(int);
#ifdef CAPDB_ENABLE_8_3_NAMES
void capdbFileSuffix3(const char*, char*);
#else
# define capdbFileSuffix3(X,Y)
#endif
u8 capdbGetBoolean(const char *z,u8);

const void *capdbValueText(capdb_value*, u8);
int capdbValueIsOfClass(const capdb_value*, void(*)(void*));
int capdbValueBytes(capdb_value*, u8);
void capdbValueSetStr(capdb_value*, int, const void *,u8,
                        void(*)(void*));
void capdbValueSetNull(capdb_value*);
void capdbValueFree(capdb_value*);
#ifndef CAPDB_UNTESTABLE
void capdbResultIntReal(capdb_context*);
#endif
capdb_value *capdbValueNew(capdb *);
#ifndef CAPDB_OMIT_UTF16
char *capdbUtf16to8(capdb *, const void*, int, u8);
#endif
int capdbValueFromExpr(capdb *, const Expr *, u8, u8, capdb_value **);
void capdbValueApplyAffinity(capdb_value *, u8, u8);
#ifndef CAPDB_AMALGAMATION
extern const unsigned char capdbOpcodeProperty[];
extern const char capdbStrBINARY[];
extern const unsigned char capdbStdTypeLen[];
extern const char capdbStdTypeAffinity[];
extern const char *capdbStdType[];
extern const unsigned char capdbUpperToLower[];
extern const unsigned char *capdbaLTb;
extern const unsigned char *capdbaEQb;
extern const unsigned char *capdbaGTb;
extern const unsigned char capdbCtypeMap[];
extern CAPDB_WSD struct Sqlite3Config capdbConfig;
extern FuncDefHash capdbBuiltinFunctions;
#ifndef CAPDB_OMIT_WSD
extern int capdbPendingByte;
#endif
#endif /* CAPDB_AMALGAMATION */
#ifdef VDBE_PROFILE
extern capdb_uint64 capdbNProfileCnt;
#endif
void capdbRootPageMoved(capdb*, int, Pgno, Pgno);
void capdbReindex(Parse*, Token*, Token*);
void capdbAlterFunctions(void);
void capdbAlterRenameTable(Parse*, SrcList*, Token*);
void capdbAlterRenameColumn(Parse*, SrcList*, Token*, Token*);
void capdbAlterDropConstraint(Parse*,SrcList*,Token*,Token*);
void capdbAlterAddConstraint(
  Parse *pParse,           /* Parse context */
  SrcList *pSrc,           /* Table to add constraint to */
  Token *pFirst,           /* First token of new constraint */
  Token *pName,            /* Name of new constraint. NULL if name omitted. */
  const char *zExpr,       /* Text of CHECK expression */
  int nExpr,               /* Size of pExpr in bytes */
  Expr *pExpr              /* The parsed CHECK expression */
);
void capdbAlterSetNotNull(Parse*, SrcList*, Token*, Token*);
i64 capdbGetToken(const unsigned char *, int *);
void capdbNestedParse(Parse*, const char*, ...);
void capdbExpirePreparedStatements(capdb*, int);
void capdbCodeRhsOfIN(Parse*, Expr*, int, int);
int capdbCodeSubselect(Parse*, Expr*);
void capdbSelectPrep(Parse*, Select*, NameContext*);
int capdbExpandSubquery(Parse*, SrcItem*);
void capdbSelectWrongNumTermsError(Parse *pParse, Select *p);
int capdbMatchEName(
  const struct ExprList_item*,
  const char*,
  const char*,
  const char*,
  int*
);
Bitmask capdbExprColUsed(Expr*);
u8 capdbStrIHash(const char*);
int capdbResolveExprNames(NameContext*, Expr*);
int capdbResolveExprListNames(NameContext*, ExprList*);
void capdbResolveSelectNames(Parse*, Select*, NameContext*);
int capdbResolveSelfReference(Parse*,Table*,int,Expr*,ExprList*);
int capdbResolveOrderGroupBy(Parse*, Select*, ExprList*, const char*);
void capdbColumnDefault(Vdbe *, Table *, int, int);
void capdbAlterFinishAddColumn(Parse *, Token *);
void capdbAlterBeginAddColumn(Parse *, SrcList *);
void capdbAlterDropColumn(Parse*, SrcList*, const Token*);
const void *capdbRenameTokenMap(Parse*, const void*, const Token*);
void capdbRenameTokenRemap(Parse*, const void *pTo, const void *pFrom);
void capdbRenameExprUnmap(Parse*, Expr*);
void capdbRenameExprlistUnmap(Parse*, ExprList*);
CollSeq *capdbGetCollSeq(Parse*, u8, CollSeq *, const char*);
char capdbAffinityType(const char*, Column*);
void capdbAnalyze(Parse*, Token*, Token*);
int capdbInvokeBusyHandler(BusyHandler*);
int capdbFindDb(capdb*, Token*);
int capdbFindDbName(capdb *, const char *);
int capdbAnalysisLoad(capdb*,int iDB);
void capdbDeleteIndexSamples(capdb*,Index*);
void capdbDefaultRowEst(Index*);
void capdbRegisterLikeFunctions(capdb*, int);
int capdbIsLikeFunction(capdb*,Expr*,int*,char*);
void capdbSchemaClear(void *);
Schema *capdbSchemaGet(capdb *, Btree *);
int capdbSchemaToIndex(capdb *db, Schema *);
KeyInfo *capdbKeyInfoAlloc(capdb*,int,int);
void capdbKeyInfoUnref(KeyInfo*);
KeyInfo *capdbKeyInfoRef(KeyInfo*);
KeyInfo *capdbKeyInfoOfIndex(Parse*, Index*);
KeyInfo *capdbKeyInfoFromExprList(Parse*, ExprList*, int, int);
const char *capdbSelectOpName(int);
int capdbHasExplicitNulls(Parse*, ExprList*);

#ifdef CAPDB_DEBUG
int capdbKeyInfoIsWriteable(KeyInfo*);
#endif
int capdbCreateFunc(capdb *, const char *, int, int, void *,
  void (*)(capdb_context*,int,capdb_value **),
  void (*)(capdb_context*,int,capdb_value **),
  void (*)(capdb_context*),
  void (*)(capdb_context*),
  void (*)(capdb_context*,int,capdb_value **),
  FuncDestructor *pDestructor
);
void capdbNoopDestructor(void*);
void *capdbOomFault(capdb*);
void capdbOomClear(capdb*);
int capdbApiExit(capdb *db, int);
int capdbOpenTempDatabase(Parse *);

char *capdbRCStrRef(char*);
void capdbRCStrUnref(void*);
char *capdbRCStrNew(u64);
char *capdbRCStrResize(char*,u64);

void capdbStrAccumInit(StrAccum*, capdb*, char*, int, int);
int capdbStrAccumEnlarge(StrAccum*, i64);
int capdbStrAccumEnlargeIfNeeded(StrAccum*, i64);
char *capdbStrAccumFinish(StrAccum*);
void capdbStrAccumSetError(StrAccum*, u8);
void capdbResultStrAccum(capdb_context*,StrAccum*);
void capdbSelectDestInit(SelectDest*,int,int);
Expr *capdbCreateColumnExpr(capdb *, SrcList *, int, int);
void capdbRecordErrorByteOffset(capdb*,const char*);
void capdbRecordErrorOffsetOfExpr(capdb*,const Expr*);

void capdbBackupRestart(capdb_backup *);
void capdbBackupUpdate(capdb_backup *, Pgno, const u8 *);

#ifndef CAPDB_OMIT_SUBQUERY
int capdbExprCheckIN(Parse*, Expr*);
#else
# define capdbExprCheckIN(x,y) CAPDB_OK
#endif

#ifdef CAPDB_ENABLE_STAT4
int capdbStat4ProbeSetValue(
    Parse*,Index*,UnpackedRecord**,Expr*,int,int,int*);
int capdbStat4ValueFromExpr(Parse*, Expr*, u8, capdb_value**);
void capdbStat4ProbeFree(UnpackedRecord*);
int capdbStat4Column(capdb*, const void*, int, int, capdb_value**);
char capdbIndexColumnAffinity(capdb*, Index*, int);
#endif

/*
** The interface to the LEMON-generated parser
*/
#ifndef CAPDB_AMALGAMATION
  void *capdbParserAlloc(void*(*)(u64), Parse*);
  void capdbParserFree(void*, void(*)(void*));
#endif
void capdbParser(void*, int, Token);
int capdbParserFallback(int);
#ifdef YYTRACKMAXSTACKDEPTH
  int capdbParserStackPeak(void*);
#endif

void capdbAutoLoadExtensions(capdb*);
#ifndef CAPDB_OMIT_LOAD_EXTENSION
  void capdbCloseExtensions(capdb*);
#else
# define capdbCloseExtensions(X)
#endif

#ifndef CAPDB_OMIT_SHARED_CACHE
  void capdbTableLock(Parse *, int, Pgno, u8, const char *);
#else
  #define capdbTableLock(v,w,x,y,z)
#endif

#ifdef CAPDB_TEST
  int capdbUtf8To8(unsigned char*);
#endif

#ifdef CAPDB_OMIT_VIRTUALTABLE
#  define capdbVtabClear(D,T)
#  define capdbVtabSync(X,Y) CAPDB_OK
#  define capdbVtabRollback(X)
#  define capdbVtabCommit(X)
#  define capdbVtabInSync(db) 0
#  define capdbVtabLock(X)
#  define capdbVtabUnlock(X)
#  define capdbVtabModuleUnref(D,X)
#  define capdbVtabUnlockList(X)
#  define capdbVtabSavepoint(X, Y, Z) CAPDB_OK
#  define capdbGetVTable(X,Y)  ((VTable*)0)
#else
   void capdbVtabClear(capdb *db, Table*);
   void capdbVtabDisconnect(capdb *db, Table *p);
   int capdbVtabSync(capdb *db, Vdbe*);
   int capdbVtabRollback(capdb *db);
   int capdbVtabCommit(capdb *db);
   void capdbVtabLock(VTable *);
   void capdbVtabUnlock(VTable *);
   void capdbVtabModuleUnref(capdb*,Module*);
   void capdbVtabUnlockList(capdb*);
   int capdbVtabSavepoint(capdb *, int, int);
   void capdbVtabImportErrmsg(Vdbe*, capdb_vtab*);
   VTable *capdbGetVTable(capdb*, Table*);
   Module *capdbVtabCreateModule(
     capdb*,
     const char*,
     const capdb_module*,
     void*,
     void(*)(void*)
   );
#  define capdbVtabInSync(db) ((db)->nVTrans>0 && (db)->aVTrans==0)
#endif
int capdbReadOnlyShadowTables(capdb *db);
#ifndef CAPDB_OMIT_VIRTUALTABLE
  int capdbShadowTableName(capdb *db, const char *zName);
  int capdbIsShadowTableOf(capdb*,Table*,const char*);
  void capdbMarkAllShadowTablesOf(capdb*, Table*);
#else
# define capdbShadowTableName(A,B) 0
# define capdbIsShadowTableOf(A,B,C) 0
# define capdbMarkAllShadowTablesOf(A,B)
#endif
int capdbVtabEponymousTableInit(Parse*,Module*);
void capdbVtabEponymousTableClear(capdb*,Module*);
void capdbVtabMakeWritable(Parse*,Table*);
void capdbVtabBeginParse(Parse*, Token*, Token*, Token*, int);
void capdbVtabFinishParse(Parse*, Token*);
void capdbVtabArgInit(Parse*);
void capdbVtabArgExtend(Parse*, Token*);
int capdbVtabCallCreate(capdb*, int, const char *, char **);
int capdbVtabCallConnect(Parse*, Table*);
int capdbVtabCallDestroy(capdb*, int, const char *);
int capdbVtabBegin(capdb *, VTable *);

FuncDef *capdbVtabOverloadFunction(capdb *,FuncDef*, int nArg, Expr*);
void capdbVtabUsesAllSchemas(Parse*);
capdb_int64 capdbStmtCurrentTime(capdb_context*);
int capdbVdbeParameterIndex(Vdbe*, const char*, int);
int capdbTransferBindings(capdb_stmt *, capdb_stmt *);
void capdbParseObjectInit(Parse*,capdb*);
void capdbParseObjectReset(Parse*);
void *capdbParserAddCleanup(Parse*,void(*)(capdb*,void*),void*);
#ifdef CAPDB_ENABLE_NORMALIZE
char *capdbNormalize(Vdbe*, const char*);
#endif
int capdbReprepare(Vdbe*);
void capdbExprListCheckLength(Parse*, ExprList*, const char*);
CollSeq *capdbExprCompareCollSeq(Parse*,const Expr*);
CollSeq *capdbBinaryCompareCollSeq(Parse *, const Expr*, const Expr*);
int capdbTempInMemory(const capdb*);
const char *capdbJournalModename(int);
#ifndef CAPDB_OMIT_WAL
  int capdbCheckpoint(capdb*, int, int, int*, int*);
  int capdbWalDefaultHook(void*,capdb*,const char*,int);
#endif
#ifndef CAPDB_OMIT_CTE
  Cte *capdbCteNew(Parse*,Token*,ExprList*,Select*,u8);
  void capdbCteDelete(capdb*,Cte*);
  With *capdbWithAdd(Parse*,With*,Cte*);
  void capdbWithDelete(capdb*,With*);
  void capdbWithDeleteGeneric(capdb*,void*);
  With *capdbWithPush(Parse*, With*, u8);
#else
# define capdbCteNew(P,T,E,S)   ((void*)0)
# define capdbCteDelete(D,C)
# define capdbCteWithAdd(P,W,C) ((void*)0)
# define capdbWithDelete(x,y)
# define capdbWithPush(x,y,z) ((void*)0)
#endif
#ifndef CAPDB_OMIT_UPSERT
  Upsert *capdbUpsertNew(capdb*,ExprList*,Expr*,ExprList*,Expr*,Upsert*);
  void capdbUpsertDelete(capdb*,Upsert*);
  Upsert *capdbUpsertDup(capdb*,Upsert*);
  int capdbUpsertAnalyzeTarget(Parse*,SrcList*,Upsert*,Upsert*);
  void capdbUpsertDoUpdate(Parse*,Upsert*,Table*,Index*,int);
  Upsert *capdbUpsertOfIndex(Upsert*,Index*);
  int capdbUpsertNextIsIPK(Upsert*);
#else
#define capdbUpsertNew(u,v,w,x,y,z) ((Upsert*)0)
#define capdbUpsertDelete(x,y)
#define capdbUpsertDup(x,y)         ((Upsert*)0)
#define capdbUpsertOfIndex(x,y)     ((Upsert*)0)
#define capdbUpsertNextIsIPK(x)     0
#endif


/* Declarations for functions in fkey.c. All of these are replaced by
** no-op macros if OMIT_FOREIGN_KEY is defined. In this case no foreign
** key functionality is available. If OMIT_TRIGGER is defined but
** OMIT_FOREIGN_KEY is not, only some of the functions are no-oped. In
** this case foreign keys are parsed, but no other functionality is
** provided (enforcement of FK constraints requires the triggers sub-system).
*/
#if !defined(CAPDB_OMIT_FOREIGN_KEY) && !defined(CAPDB_OMIT_TRIGGER)
  void capdbFkCheck(Parse*, Table*, int, int, int*, int);
  void capdbFkDropTable(Parse*, SrcList *, Table*);
  void capdbFkActions(Parse*, Table*, ExprList*, int, int*, int);
  int capdbFkRequired(Parse*, Table*, int*, int);
  u32 capdbFkOldmask(Parse*, Table*);
  FKey *capdbFkReferences(Table *);
  void capdbFkClearTriggerCache(capdb*,int);
#else
  #define capdbFkActions(a,b,c,d,e,f)
  #define capdbFkCheck(a,b,c,d,e,f)
  #define capdbFkDropTable(a,b,c)
  #define capdbFkOldmask(a,b)         0
  #define capdbFkRequired(a,b,c,d)    0
  #define capdbFkReferences(a)        0
  #define capdbFkClearTriggerCache(a,b)
#endif
#ifndef CAPDB_OMIT_FOREIGN_KEY
  void capdbFkDelete(capdb *, Table*);
  int capdbFkLocateIndex(Parse*,Table*,FKey*,Index**,int**);
#else
  #define capdbFkDelete(a,b)
  #define capdbFkLocateIndex(a,b,c,d,e)
#endif


/*
** Available fault injectors.  Should be numbered beginning with 0.
*/
#define CAPDB_FAULTINJECTOR_MALLOC     0
#define CAPDB_FAULTINJECTOR_COUNT      1

/*
** The interface to the code in fault.c used for identifying "benign"
** malloc failures. This is only present if CAPDB_UNTESTABLE
** is not defined.
*/
#ifndef CAPDB_UNTESTABLE
  void capdbBeginBenignMalloc(void);
  void capdbEndBenignMalloc(void);
#else
  #define capdbBeginBenignMalloc()
  #define capdbEndBenignMalloc()
#endif

/*
** Allowed return values from capdbFindInIndex()
*/
#define IN_INDEX_ROWID        1   /* Search the rowid of the table */
#define IN_INDEX_EPH          2   /* Search an ephemeral b-tree */
#define IN_INDEX_INDEX_ASC    3   /* Existing index ASCENDING */
#define IN_INDEX_INDEX_DESC   4   /* Existing index DESCENDING */
#define IN_INDEX_NOOP         5   /* No table available. Use comparisons */
/*
** Allowed flags for the 3rd parameter to capdbFindInIndex().
*/
#define IN_INDEX_NOOP_OK     0x0001  /* OK to return IN_INDEX_NOOP */
#define IN_INDEX_MEMBERSHIP  0x0002  /* IN operator used for membership test */
#define IN_INDEX_LOOP        0x0004  /* IN operator used as a loop */
int capdbFindInIndex(Parse *, Expr *, u32, int*, int*, int*);

int capdbJournalOpen(capdb_vfs *, const char *, capdb_file *, int, int);
int capdbJournalSize(capdb_vfs *);
#if defined(CAPDB_ENABLE_ATOMIC_WRITE) \
 || defined(CAPDB_ENABLE_BATCH_ATOMIC_WRITE)
  int capdbJournalCreate(capdb_file *);
#endif

int capdbJournalIsInMemory(capdb_file *p);
void capdbMemJournalOpen(capdb_file *);

void capdbExprSetHeightAndFlags(Parse *pParse, Expr *p);
#if CAPDB_MAX_EXPR_DEPTH>0
  int capdbSelectExprHeight(const Select *);
  int capdbExprCheckHeight(Parse*, int);
#else
  #define capdbSelectExprHeight(x) 0
  #define capdbExprCheckHeight(x,y)
#endif
void capdbExprSetErrorOffset(Expr*,int);

u32 capdbGet4byte(const u8*);
void capdbPut4byte(u8*, u32);

#ifdef CAPDB_ENABLE_UNLOCK_NOTIFY
  void capdbConnectionBlocked(capdb *, capdb *);
  void capdbConnectionUnlocked(capdb *db);
  void capdbConnectionClosed(capdb *db);
#else
  #define capdbConnectionBlocked(x,y)
  #define capdbConnectionUnlocked(x)
  #define capdbConnectionClosed(x)
#endif

#ifdef CAPDB_DEBUG
  void capdbParserTrace(FILE*, char *);
#endif
#if defined(YYCOVERAGE)
  int capdbParserCoverage(FILE*);
#endif

/*
** If the CAPDB_ENABLE IOTRACE exists then the global variable
** capdbIoTrace is a pointer to a printf-like routine used to
** print I/O tracing messages.
*/
#ifdef CAPDB_ENABLE_IOTRACE
# define IOTRACE(A)  if( capdbIoTrace ){ capdbIoTrace A; }
  void capdbVdbeIOTraceSql(Vdbe*);
CAPDB_API CAPDB_EXTERN void (CAPDB_CDECL *capdbIoTrace)(const char*,...);
#else
# define IOTRACE(A)
# define capdbVdbeIOTraceSql(X)
#endif

/*
** These routines are available for the mem2.c debugging memory allocator
** only.  They are used to verify that different "types" of memory
** allocations are properly tracked by the system.
**
** capdbMemdebugSetType() sets the "type" of an allocation to one of
** the MEMTYPE_* macros defined below.  The type must be a bitmask with
** a single bit set.
**
** capdbMemdebugHasType() returns true if any of the bits in its second
** argument match the type set by the previous capdbMemdebugSetType().
** capdbMemdebugHasType() is intended for use inside assert() statements.
**
** capdbMemdebugNoType() returns true if none of the bits in its second
** argument match the type set by the previous capdbMemdebugSetType().
**
** Perhaps the most important point is the difference between MEMTYPE_HEAP
** and MEMTYPE_LOOKASIDE.  If an allocation is MEMTYPE_LOOKASIDE, that means
** it might have been allocated by lookaside, except the allocation was
** too large or lookaside was already full.  It is important to verify
** that allocations that might have been satisfied by lookaside are not
** passed back to non-lookaside free() routines.  Asserts such as the
** example above are placed on the non-lookaside free() routines to verify
** this constraint.
**
** All of this is no-op for a production build.  It only comes into
** play when the CAPDB_MEMDEBUG compile-time option is used.
*/
#ifdef CAPDB_MEMDEBUG
  void capdbMemdebugSetType(void*,u8);
  int capdbMemdebugHasType(const void*,u8);
  int capdbMemdebugNoType(const void*,u8);
#else
# define capdbMemdebugSetType(X,Y)  /* no-op */
# define capdbMemdebugHasType(X,Y)  1
# define capdbMemdebugNoType(X,Y)   1
#endif
#define MEMTYPE_HEAP       0x01  /* General heap allocations */
#define MEMTYPE_LOOKASIDE  0x02  /* Heap that might have been lookaside */
#define MEMTYPE_PCACHE     0x04  /* Page cache allocations */

/*
** Threading interface
*/
#if CAPDB_MAX_WORKER_THREADS>0
int capdbThreadCreate(SQLiteThread**,void*(*)(void*),void*);
int capdbThreadJoin(SQLiteThread*, void**);
#endif

#if defined(CAPDB_ENABLE_DBPAGE_VTAB) || defined(CAPDB_TEST)
int capdbDbpageRegister(capdb*);
#endif
#if defined(CAPDB_ENABLE_DBSTAT_VTAB) || defined(CAPDB_TEST)
int capdbDbstatRegister(capdb*);
#endif

int capdbExprVectorSize(const Expr *pExpr);
int capdbExprIsVector(const Expr *pExpr);
Expr *capdbVectorFieldSubexpr(Expr*, int);
Expr *capdbExprForVectorField(Parse*,Expr*,int,int);
void capdbVectorErrorMsg(Parse*, Expr*);

#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
const char **capdbCompileOptions(int *pnOpt);
#endif

#if CAPDB_OS_UNIX && defined(CAPDB_OS_KV_OPTIONAL)
int capdbKvvfsInit(void);
#endif

#if defined(VDBE_PROFILE) \
 || defined(CAPDB_PERFORMANCE_TRACE) \
 || defined(CAPDB_ENABLE_STMT_SCANSTATUS)
capdb_uint64 capdbHwtime(void);
#endif

#ifdef CAPDB_ENABLE_STMT_SCANSTATUS
# define IS_STMT_SCANSTATUS(db) (db->flags & CAPDB_StmtScanStatus)
#else
# define IS_STMT_SCANSTATUS(db) 0
#endif

#endif /* SQLITEINT_H */
