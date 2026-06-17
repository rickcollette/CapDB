/*
** 2004 May 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code that is specific to Windows.
*/
#include "capdbInt.h"
#if CAPDB_OS_WIN               /* This file is used for Windows only */

/*
** Include code that is common to all os_*.c files
*/
#include "os_common.h"

/*
** Include the header file for the Windows VFS.
*/
#include "os_win.h"
#define CAPDB_WIN32_HAS_WIDE 1

/*
** This constant should already be defined (in the "WinDef.h" SDK file).
*/
#ifndef MAX_PATH
#  define MAX_PATH                      (260)
#endif

/*
** Maximum pathname length (in chars) for Win32.  This should normally be
** MAX_PATH.
*/
#ifndef CAPDB_WIN32_MAX_PATH_CHARS
#  define CAPDB_WIN32_MAX_PATH_CHARS   (MAX_PATH)
#endif

/*
** This constant should already be defined (in the "WinNT.h" SDK file).
*/
#ifndef UNICODE_STRING_MAX_CHARS
#  define UNICODE_STRING_MAX_CHARS      (32767)
#endif

/*
** Maximum pathname length (in chars) for WinNT.  This should normally be
** UNICODE_STRING_MAX_CHARS.
*/
#ifndef CAPDB_WINNT_MAX_PATH_CHARS
#  define CAPDB_WINNT_MAX_PATH_CHARS   (UNICODE_STRING_MAX_CHARS)
#endif

/*
** Maximum pathname length (in bytes) for Win32.  The MAX_PATH macro is in
** characters, so we allocate 4 bytes per character assuming worst-case of
** 4-bytes-per-character for UTF8.
*/
#ifndef CAPDB_WIN32_MAX_PATH_BYTES
#  define CAPDB_WIN32_MAX_PATH_BYTES   (CAPDB_WIN32_MAX_PATH_CHARS*4)
#endif

/*
** Maximum pathname length (in bytes) for WinNT.  This should normally be
** UNICODE_STRING_MAX_CHARS * sizeof(WCHAR).
*/
#ifndef CAPDB_WINNT_MAX_PATH_BYTES
#  define CAPDB_WINNT_MAX_PATH_BYTES   \
                            (sizeof(WCHAR) * CAPDB_WINNT_MAX_PATH_CHARS)
#endif

/*
** Maximum error message length (in chars) for WinRT.
*/
#ifndef CAPDB_WIN32_MAX_ERRMSG_CHARS
#  define CAPDB_WIN32_MAX_ERRMSG_CHARS (1024)
#endif

/*
** Returns non-zero if the character should be treated as a directory
** separator.
*/
#ifndef winIsDirSep
#  define winIsDirSep(a)                (((a) == '/') || ((a) == '\\'))
#endif

/*
** This macro is used when a local variable is set to a value that is
** [sometimes] not used by the code (e.g. via conditional compilation).
*/
#ifndef UNUSED_VARIABLE_VALUE
#  define UNUSED_VARIABLE_VALUE(x)      (void)(x)
#endif

/*
** Returns the character that should be used as the directory separator.
*/
#ifndef winGetDirSep
#  define winGetDirSep()                '\\'
#endif

/*
** Some older Microsoft compilers lack the following definition.
*/
#ifndef INVALID_FILE_ATTRIBUTES
# define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

#ifndef FILE_FLAG_MASK
# define FILE_FLAG_MASK          (0xFF3C0000)
#endif

#ifndef FILE_ATTRIBUTE_MASK
# define FILE_ATTRIBUTE_MASK     (0x0003FFF7)
#endif

#ifndef CAPDB_OMIT_WAL
/* Forward references to structures used for WAL */
typedef struct winShm winShm;           /* A connection to shared-memory */
typedef struct winShmNode winShmNode;   /* A region of shared-memory */
#endif

/*
** The winFile structure is a subclass of capdb_file* specific to the win32
** portability layer.
*/
typedef struct winFile winFile;
struct winFile {
  const capdb_io_methods *pMethod; /*** Must be first ***/
  capdb_vfs *pVfs;      /* The VFS used to open this file */
  HANDLE h;               /* Handle for accessing the file */
  u8 locktype;            /* Type of lock currently held on this file */
  short sharedLockByte;   /* Randomly chosen byte used as a shared lock */
  u8 ctrlFlags;           /* Flags.  See WINFILE_* below */
  DWORD lastErrno;        /* The Windows errno from the last I/O error */
#ifndef CAPDB_OMIT_WAL
  winShm *pShm;           /* Instance of shared memory on this file */
#endif
  const char *zPath;      /* Full pathname of this file */
  int szChunk;            /* Chunk size configured by FCNTL_CHUNK_SIZE */
#if CAPDB_MAX_MMAP_SIZE>0
  int nFetchOut;                /* Number of outstanding xFetch references */
  HANDLE hMap;                  /* Handle for accessing memory mapping */
  void *pMapRegion;             /* Area memory mapped */
  capdb_int64 mmapSize;       /* Size of mapped region */
  capdb_int64 mmapSizeMax;    /* Configured FCNTL_MMAP_SIZE value */
#endif
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  DWORD iBusyTimeout;        /* Wait this many millisec on locks */
  int bBlockOnConnect;
#endif
};

#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
# define winFileBusyTimeout(pDbFd) pDbFd->iBusyTimeout
#else
# define winFileBusyTimeout(pDbFd) 0
#endif

/*
** The winVfsAppData structure is used for the pAppData member for all of the
** Win32 VFS variants.
*/
typedef struct winVfsAppData winVfsAppData;
struct winVfsAppData {
  const capdb_io_methods *pMethod; /* The file I/O methods to use. */
  void *pAppData;                    /* The extra pAppData, if any. */
  BOOL bNoLock;                      /* Non-zero if locking is disabled. */
};

/*
** Allowed values for winFile.ctrlFlags
*/
#define WINFILE_RDONLY          0x02   /* Connection is read only */
#define WINFILE_PERSIST_WAL     0x04   /* Persistent WAL mode */
#define WINFILE_PSOW            0x10   /* CAPDB_IOCAP_POWERSAFE_OVERWRITE */

/*
 * The size of the buffer used by capdb_win32_write_debug().
 */
#ifndef CAPDB_WIN32_DBG_BUF_SIZE
#  define CAPDB_WIN32_DBG_BUF_SIZE   ((int)(4096-sizeof(DWORD)))
#endif

/*
 * If compiled with CAPDB_WIN32_MALLOC on Windows, we will use the
 * various Win32 API heap functions instead of our own.
 */
#ifdef CAPDB_WIN32_MALLOC

/*
 * If this is non-zero, an isolated heap will be created by the native Win32
 * allocator subsystem; otherwise, the default process heap will be used.  This
 * setting has no effect when compiling for WinRT.  By default, this is enabled
 * and an isolated heap will be created to store all allocated data.
 *
 ******************************************************************************
 * WARNING: It is important to note that when this setting is non-zero and the
 *          winMemShutdown function is called (e.g. by the capdb_shutdown
 *          function), all data that was allocated using the isolated heap will
 *          be freed immediately and any attempt to access any of that freed
 *          data will almost certainly result in an immediate access violation.
 ******************************************************************************
 */
#ifndef CAPDB_WIN32_HEAP_CREATE
#  define CAPDB_WIN32_HEAP_CREATE        (TRUE)
#endif

/*
 * This is the maximum possible initial size of the Win32-specific heap, in
 * bytes.
 */
#ifndef CAPDB_WIN32_HEAP_MAX_INIT_SIZE
#  define CAPDB_WIN32_HEAP_MAX_INIT_SIZE (4294967295U)
#endif

/*
 * This is the extra space for the initial size of the Win32-specific heap,
 * in bytes.  This value may be zero.
 */
#ifndef CAPDB_WIN32_HEAP_INIT_EXTRA
#  define CAPDB_WIN32_HEAP_INIT_EXTRA  (4194304)
#endif

/*
 * Calculate the maximum legal cache size, in pages, based on the maximum
 * possible initial heap size and the default page size, setting aside the
 * needed extra space.
 */
#ifndef CAPDB_WIN32_MAX_CACHE_SIZE
#  define CAPDB_WIN32_MAX_CACHE_SIZE   (((CAPDB_WIN32_HEAP_MAX_INIT_SIZE) - \
                                          (CAPDB_WIN32_HEAP_INIT_EXTRA)) / \
                                         (CAPDB_DEFAULT_PAGE_SIZE))
#endif

/*
 * This is cache size used in the calculation of the initial size of the
 * Win32-specific heap.  It cannot be negative.
 */
#ifndef CAPDB_WIN32_CACHE_SIZE
#  if CAPDB_DEFAULT_CACHE_SIZE>=0
#    define CAPDB_WIN32_CACHE_SIZE     (CAPDB_DEFAULT_CACHE_SIZE)
#  else
#    define CAPDB_WIN32_CACHE_SIZE     (-(CAPDB_DEFAULT_CACHE_SIZE))
#  endif
#endif

/*
 * Make sure that the calculated cache size, in pages, cannot cause the
 * initial size of the Win32-specific heap to exceed the maximum amount
 * of memory that can be specified in the call to HeapCreate.
 */
#if CAPDB_WIN32_CACHE_SIZE>CAPDB_WIN32_MAX_CACHE_SIZE
#  undef CAPDB_WIN32_CACHE_SIZE
#  define CAPDB_WIN32_CACHE_SIZE       (2000)
#endif

/*
 * The initial size of the Win32-specific heap.  This value may be zero.
 */
#ifndef CAPDB_WIN32_HEAP_INIT_SIZE
#  define CAPDB_WIN32_HEAP_INIT_SIZE   ((CAPDB_WIN32_CACHE_SIZE) * \
                                         (CAPDB_DEFAULT_PAGE_SIZE) + \
                                         (CAPDB_WIN32_HEAP_INIT_EXTRA))
#endif

/*
 * The maximum size of the Win32-specific heap.  This value may be zero.
 */
#ifndef CAPDB_WIN32_HEAP_MAX_SIZE
#  define CAPDB_WIN32_HEAP_MAX_SIZE    (0)
#endif

/*
 * The extra flags to use in calls to the Win32 heap APIs.  This value may be
 * zero for the default behavior.
 */
#ifndef CAPDB_WIN32_HEAP_FLAGS
#  define CAPDB_WIN32_HEAP_FLAGS       (0)
#endif


/*
** The winMemData structure stores information required by the Win32-specific
** capdb_mem_methods implementation.
*/
typedef struct winMemData winMemData;
struct winMemData {
#ifndef NDEBUG
  u32 magic1;   /* Magic number to detect structure corruption. */
#endif
  HANDLE hHeap; /* The handle to our heap. */
  BOOL bOwned;  /* Do we own the heap (i.e. destroy it on shutdown)? */
#ifndef NDEBUG
  u32 magic2;   /* Magic number to detect structure corruption. */
#endif
};

#ifndef NDEBUG
#define WINMEM_MAGIC1     0x42b2830b
#define WINMEM_MAGIC2     0xbd4d7cf4
#endif

static struct winMemData win_mem_data = {
#ifndef NDEBUG
  WINMEM_MAGIC1,
#endif
  NULL, FALSE
#ifndef NDEBUG
  ,WINMEM_MAGIC2
#endif
};

#ifndef NDEBUG
#define winMemAssertMagic1() assert( win_mem_data.magic1==WINMEM_MAGIC1 )
#define winMemAssertMagic2() assert( win_mem_data.magic2==WINMEM_MAGIC2 )
#define winMemAssertMagic()  winMemAssertMagic1(); winMemAssertMagic2();
#else
#define winMemAssertMagic()
#endif

#define winMemGetDataPtr()  &win_mem_data
#define winMemGetHeap()     win_mem_data.hHeap
#define winMemGetOwned()    win_mem_data.bOwned

static void *winMemMalloc(int nBytes);
static void winMemFree(void *pPrior);
static void *winMemRealloc(void *pPrior, int nBytes);
static int winMemSize(void *p);
static int winMemRoundup(int n);
static int winMemInit(void *pAppData);
static void winMemShutdown(void *pAppData);

const capdb_mem_methods *capdbMemGetWin32(void);
#endif /* CAPDB_WIN32_MALLOC */

/*
** The following variable is (normally) set once and never changes
** thereafter.  It records whether the operating system is Win9x
** or WinNT.
**
** 0:   Operating system unknown.
** 1:   Operating system is Win9x.
** 2:   Operating system is WinNT.
**
** In order to facilitate testing on a WinNT system, the test fixture
** can manually set this value to 1 to emulate Win98 behavior.
*/
#ifdef CAPDB_TEST
LONG volatile capdb_os_type = 0;
#else
static LONG volatile capdb_os_type = 0;
#endif

#ifndef SYSCALL
#  define SYSCALL capdb_syscall_ptr
#endif

/*
** Many system calls are accessed through pointer-to-functions so that
** they may be overridden at runtime to facilitate fault injection during
** testing and sandboxing.  The following array holds the names and pointers
** to all overrideable system calls.
*/
static struct win_syscall {
  const char *zName;            /* Name of the system call */
  capdb_syscall_ptr pCurrent; /* Current value of the system call */
  capdb_syscall_ptr pDefault; /* Default value */
} aSyscall[] = {
  { "AreFileApisANSI",         (SYSCALL)AreFileApisANSI,         0 },
#define osAreFileApisANSI ((BOOL(WINAPI*)(VOID))aSyscall[0].pCurrent)

  { "CloseHandle",             (SYSCALL)CloseHandle,             0 },
#define osCloseHandle ((BOOL(WINAPI*)(HANDLE))aSyscall[1].pCurrent)


  { "CreateFileW",             (SYSCALL)CreateFileW,             0 },
#define osCreateFileW ((HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD, \
        LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE))aSyscall[2].pCurrent)

#if defined(CAPDB_UWP)
  { "CreateFileMappingFromApp",(SYSCALL)CreateFileMappingFromApp,0 },
#else
  { "CreateFileMappingFromApp",(SYSCALL)0,                       0 },
#endif
#define osCreateFileMappingFromApp ((HANDLE(WINAPI*)(HANDLE,\
        PSECURITY_ATTRIBUTES, \
        ULONG,ULONG64,PCWSTR))aSyscall[3].pCurrent)

#if !defined(CAPDB_UWP)
  { "CreateFileMappingW",      (SYSCALL)CreateFileMappingW,      0 },
#else
  { "CreateFileMappingW",      (SYSCALL)0,                       0 },
#endif
#define osCreateFileMappingW ((HANDLE(WINAPI*)(HANDLE,LPSECURITY_ATTRIBUTES, \
        DWORD,DWORD,DWORD,LPCWSTR))aSyscall[4].pCurrent)

  { "DeleteFileW",             (SYSCALL)DeleteFileW,             0 },
#define osDeleteFileW ((BOOL(WINAPI*)(LPCWSTR))aSyscall[5].pCurrent)

  { "FlushFileBuffers",        (SYSCALL)FlushFileBuffers,        0 },
#define osFlushFileBuffers ((BOOL(WINAPI*)(HANDLE))aSyscall[6].pCurrent)

  { "FormatMessageW",          (SYSCALL)FormatMessageW,          0 },
#define osFormatMessageW ((DWORD(WINAPI*)(DWORD,LPCVOID,DWORD,DWORD,LPWSTR, \
        DWORD,va_list*))aSyscall[7].pCurrent)

#if !defined(CAPDB_OMIT_LOAD_EXTENSION) && !defined(CAPDB_UWP)
  { "FreeLibrary",             (SYSCALL)FreeLibrary,             0 },
#else
  { "FreeLibrary",             (SYSCALL)0,                       0 },
#endif

#define osFreeLibrary ((BOOL(WINAPI*)(HMODULE))aSyscall[8].pCurrent)

  { "GetCurrentProcessId",     (SYSCALL)GetCurrentProcessId,     0 },
#define osGetCurrentProcessId ((DWORD(WINAPI*)(VOID))aSyscall[9].pCurrent)

  { "GetFileAttributesW",      (SYSCALL)GetFileAttributesW,      0 },
#define osGetFileAttributesW ((DWORD(WINAPI*)(LPCWSTR))aSyscall[10].pCurrent)

  { "GetFileAttributesExW",    (SYSCALL)GetFileAttributesExW,    0 },
#define osGetFileAttributesExW ((BOOL(WINAPI*)(LPCWSTR,GET_FILEEX_INFO_LEVELS, \
        LPVOID))aSyscall[11].pCurrent)

  { "GetFileSizeEx",           (SYSCALL)GetFileSizeEx,           0 },
#define osGetFileSizeEx ((BOOL(WINAPI*)(HANDLE, \
                        PLARGE_INTEGER))aSyscall[12].pCurrent)

  { "GetFullPathNameW",        (SYSCALL)GetFullPathNameW,        0 },
#define osGetFullPathNameW ((DWORD(WINAPI*)(LPCWSTR,DWORD,LPWSTR, \
        LPWSTR*))aSyscall[13].pCurrent)

  { "GetLastError",            (SYSCALL)GetLastError,            0 },
#define osGetLastError ((DWORD(WINAPI*)(VOID))aSyscall[14].pCurrent)

#if !defined(CAPDB_OMIT_LOAD_EXTENSION) && !defined(CAPDB_UWP)
  { "GetProcAddressA",         (SYSCALL)GetProcAddress,          0 },
#else
  { "GetProcAddressA",         (SYSCALL)0,                       0 },
#endif
#define osGetProcAddressA ((FARPROC(WINAPI*)(HMODULE, \
        LPCSTR))aSyscall[15].pCurrent)

  { "GetSystemInfo",           (SYSCALL)GetSystemInfo,           0 },
#define osGetSystemInfo ((VOID(WINAPI*)(LPSYSTEM_INFO))aSyscall[16].pCurrent)

  { "GetSystemTimeAsFileTime", (SYSCALL)GetSystemTimeAsFileTime, 0 },
#define osGetSystemTimeAsFileTime ((VOID(WINAPI*)( \
        LPFILETIME))aSyscall[17].pCurrent)

#ifdef CAPDB_UWP
  { "GetTempPathW",            (SYSCALL)0,                       0 },
#else
  { "GetTempPathW",            (SYSCALL)GetTempPathW,            0 },
#endif
#define osGetTempPathW ((DWORD(WINAPI*)(DWORD,LPWSTR))aSyscall[18].pCurrent)

  { "GetTickCount64",          (SYSCALL)GetTickCount64,          0 },
#define osGetTickCount64 ((ULONGLONG(WINAPI*)(VOID))aSyscall[19].pCurrent)

#ifdef CAPDB_UWP
  { "HeapAlloc",               (SYSCALL)0,                       0 },
#else
  { "HeapAlloc",               (SYSCALL)HeapAlloc,               0 },
#endif
#define osHeapAlloc ((LPVOID(WINAPI*)(HANDLE,DWORD, \
        SIZE_T))aSyscall[20].pCurrent)

#ifdef CAPDB_UWP
  { "HeapCreate",              (SYSCALL)0,                       0 },
#else
  { "HeapCreate",              (SYSCALL)HeapCreate,              0 },
#endif
#define osHeapCreate ((HANDLE(WINAPI*)(DWORD,SIZE_T, \
        SIZE_T))aSyscall[21].pCurrent)

#ifdef CAPDB_UWP
  { "HeapDestroy",             (SYSCALL)0,                       0 },
#else
  { "HeapDestroy",             (SYSCALL)HeapDestroy,             0 },
#endif
#define osHeapDestroy ((BOOL(WINAPI*)(HANDLE))aSyscall[22].pCurrent)

#ifdef CAPDB_UWP
  { "HeapFree",                (SYSCALL)0,                       0 },
#else
  { "HeapFree",                (SYSCALL)HeapFree,                0 },
#endif
#define osHeapFree ((BOOL(WINAPI*)(HANDLE,DWORD,LPVOID))aSyscall[23].pCurrent)

#ifdef CAPDB_UWP
  { "HeapReAlloc",             (SYSCALL)0,                       0 },
#else
  { "HeapReAlloc",             (SYSCALL)HeapReAlloc,             0 },
#endif
#define osHeapReAlloc ((LPVOID(WINAPI*)(HANDLE,DWORD,LPVOID, \
        SIZE_T))aSyscall[24].pCurrent)

#ifdef CAPDB_UWP
  { "HeapSize",                (SYSCALL)0,                       0 },
#else
  { "HeapSize",                (SYSCALL)HeapSize,                0 },
#endif
#define osHeapSize ((SIZE_T(WINAPI*)(HANDLE,DWORD, \
        LPCVOID))aSyscall[25].pCurrent)

#ifdef CAPDB_UWP
  { "HeapValidate",            (SYSCALL)0,                       0 },
#else
  { "HeapValidate",            (SYSCALL)HeapValidate,            0 },
#endif
#define osHeapValidate ((BOOL(WINAPI*)(HANDLE,DWORD, \
        LPCVOID))aSyscall[26].pCurrent)

#ifdef CAPDB_UWP
  { "HeapCompact",             (SYSCALL)0,                       0 },
#else
  { "HeapCompact",             (SYSCALL)HeapCompact,             0 },
#endif
#define osHeapCompact ((UINT(WINAPI*)(HANDLE,DWORD))aSyscall[27].pCurrent)


#if !defined(CAPDB_OMIT_LOAD_EXTENSION) && !defined(CAPDB_UWP)
  { "LoadLibraryW",            (SYSCALL)LoadLibraryW,            0 },
#else
  { "LoadLibraryW",            (SYSCALL)0,                       0 },
#endif
#define osLoadLibraryW ((HMODULE(WINAPI*)(LPCWSTR))aSyscall[28].pCurrent)

  { "LocalFree",               (SYSCALL)LocalFree,               0 },
#define osLocalFree ((HLOCAL(WINAPI*)(HLOCAL))aSyscall[29].pCurrent)

  { "LockFileEx",              (SYSCALL)LockFileEx,              0 },
#define osLockFileEx ((BOOL(WINAPI*)(HANDLE,DWORD,DWORD,DWORD,DWORD, \
        LPOVERLAPPED))aSyscall[30].pCurrent)

#if (!defined(CAPDB_OMIT_WAL) || CAPDB_MAX_MMAP_SIZE>0) \
 && !defined(CAPDB_UWP)
  { "MapViewOfFile",           (SYSCALL)MapViewOfFile,           0 },
#else
  { "MapViewOfFile",           (SYSCALL)0,                       0 },
#endif
#define osMapViewOfFile ((LPVOID(WINAPI*)(HANDLE,DWORD,DWORD,DWORD, \
        SIZE_T))aSyscall[31].pCurrent)

#if (!defined(CAPDB_OMIT_WAL) || CAPDB_MAX_MMAP_SIZE>0) \
 && defined(CAPDB_UWP)
  { "MapViewOfFileFromApp",    (SYSCALL)MapViewOfFileFromApp,   0 },
#else
  { "MapViewOfFileFromApp",    (SYSCALL)0,                      0 },
#endif
#define osMapViewOfFileFromApp ((LPVOID(WINAPI*)(HANDLE,ULONG,ULONG64, \
        SIZE_T))aSyscall[32].pCurrent)

  { "MultiByteToWideChar",     (SYSCALL)MultiByteToWideChar,     0 },
#define osMultiByteToWideChar ((int(WINAPI*)(UINT,DWORD,LPCSTR,int,LPWSTR, \
        int))aSyscall[33].pCurrent)

  { "QueryPerformanceCounter", (SYSCALL)QueryPerformanceCounter, 0 },
#define osQueryPerformanceCounter ((BOOL(WINAPI*)( \
        LARGE_INTEGER*))aSyscall[34].pCurrent)

  { "ReadFile",                (SYSCALL)ReadFile,                0 },
#define osReadFile ((BOOL(WINAPI*)(HANDLE,LPVOID,DWORD,LPDWORD, \
        LPOVERLAPPED))aSyscall[35].pCurrent)

  { "SetEndOfFile",            (SYSCALL)SetEndOfFile,            0 },
#define osSetEndOfFile ((BOOL(WINAPI*)(HANDLE))aSyscall[36].pCurrent)

  { "SetFilePointerEx",        (SYSCALL)SetFilePointerEx,        0 },
#define osSetFilePointerEx ((BOOL(WINAPI*)(HANDLE,LARGE_INTEGER,\
        PLARGE_INTEGER,DWORD))aSyscall[37].pCurrent)

  { "Sleep",                   (SYSCALL)Sleep,                   0 },
#define osSleep ((VOID(WINAPI*)(DWORD))aSyscall[38].pCurrent)

  { "UnlockFileEx",            (SYSCALL)UnlockFileEx,            0 },
#define osUnlockFileEx ((BOOL(WINAPI*)(HANDLE,DWORD,DWORD,DWORD, \
        LPOVERLAPPED))aSyscall[39].pCurrent)

#if !defined(CAPDB_OMIT_WAL) || CAPDB_MAX_MMAP_SIZE>0
  { "UnmapViewOfFile",         (SYSCALL)UnmapViewOfFile,         0 },
#else
  { "UnmapViewOfFile",         (SYSCALL)0,                       0 },
#endif
#define osUnmapViewOfFile ((BOOL(WINAPI*)(LPCVOID))aSyscall[40].pCurrent)

  { "WideCharToMultiByte",     (SYSCALL)WideCharToMultiByte,     0 },
#define osWideCharToMultiByte ((int(WINAPI*)(UINT,DWORD,LPCWSTR,int,LPSTR,int, \
        LPCSTR,LPBOOL))aSyscall[41].pCurrent)

  { "WriteFile",               (SYSCALL)WriteFile,               0 },
#define osWriteFile ((BOOL(WINAPI*)(HANDLE,LPCVOID,DWORD,LPDWORD, \
        LPOVERLAPPED))aSyscall[42].pCurrent)

  { "WaitForSingleObject",     (SYSCALL)WaitForSingleObject,     0 },
#define osWaitForSingleObject ((DWORD(WINAPI*)(HANDLE, \
        DWORD))aSyscall[43].pCurrent)

  { "WaitForSingleObjectEx",   (SYSCALL)WaitForSingleObjectEx,   0 },
#define osWaitForSingleObjectEx ((DWORD(WINAPI*)(HANDLE,DWORD, \
        BOOL))aSyscall[44].pCurrent)

  { "OutputDebugStringA",      (SYSCALL)OutputDebugStringA,      0 },
#define osOutputDebugStringA ((VOID(WINAPI*)(LPCSTR))aSyscall[45].pCurrent)

  { "GetProcessHeap",          (SYSCALL)GetProcessHeap,          0 },
#define osGetProcessHeap ((HANDLE(WINAPI*)(VOID))aSyscall[46].pCurrent)

/*
** NOTE: On some sub-platforms, the InterlockedCompareExchange "function"
**       is really just a macro that uses a compiler intrinsic (e.g. x64).
**       So do not try to make this is into a redefinable interface.
*/
#if defined(InterlockedCompareExchange)
  { "InterlockedCompareExchange", (SYSCALL)0,                    0 },
#define osInterlockedCompareExchange InterlockedCompareExchange
#else
  { "InterlockedCompareExchange", (SYSCALL)InterlockedCompareExchange, 0 },
#define osInterlockedCompareExchange ((LONG(WINAPI*)(LONG volatile*,\
        LONG,LONG))aSyscall[47].pCurrent)
#endif /* defined(InterlockedCompareExchange) */

#if CAPDB_WIN32_USE_UUID
  { "UuidCreate",               (SYSCALL)UuidCreate,             0 },
#else
  { "UuidCreate",               (SYSCALL)0,                      0 },
#endif
#define osUuidCreate ((RPC_STATUS(RPC_ENTRY*)(UUID*))aSyscall[48].pCurrent)

#if CAPDB_WIN32_USE_UUID
  { "UuidCreateSequential",     (SYSCALL)UuidCreateSequential,   0 },
#else
  { "UuidCreateSequential",     (SYSCALL)0,                      0 },
#endif
#define osUuidCreateSequential \
        ((RPC_STATUS(RPC_ENTRY*)(UUID*))aSyscall[49].pCurrent)

#if !defined(CAPDB_NO_SYNC) && CAPDB_MAX_MMAP_SIZE>0
  { "FlushViewOfFile",          (SYSCALL)FlushViewOfFile,        0 },
#else
  { "FlushViewOfFile",          (SYSCALL)0,                      0 },
#endif
#define osFlushViewOfFile \
        ((BOOL(WINAPI*)(LPCVOID,SIZE_T))aSyscall[50].pCurrent)

#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  { "CreateEvent",              (SYSCALL)CreateEvent,            0 },
#else
  { "CreateEvent",              (SYSCALL)0,                      0 },
#endif
#define osCreateEvent ((HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES,BOOL, \
                        BOOL,LPCSTR))aSyscall[51].pCurrent)

#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  { "CancelIo",                 (SYSCALL)CancelIo,               0 },
#else
  { "CancelIo",                 (SYSCALL)0,                      0 },
#endif
#define osCancelIo ((BOOL(WINAPI*)(HANDLE))aSyscall[52].pCurrent)

#ifndef _WIN32
  { "getenv",                   (SYSCALL)getenv,                 0 },
#else
  { "getenv",                   (SYSCALL)0,                      0 },
#endif
#define osGetenv ((const char *(*)(const char *))aSyscall[53].pCurrent)

#ifndef _WIN32
  { "getcwd",                   (SYSCALL)getcwd,                 0 },
#else
  { "getcwd",                   (SYSCALL)0,                      0 },
#endif
#define osGetcwd ((char*(*)(char*,size_t))aSyscall[54].pCurrent)

#ifndef _WIN32
  { "readlink",                 (SYSCALL)readlink,               0 },
#else
  { "readlink",                 (SYSCALL)0,                      0 },
#endif
#define osReadlink ((ssize_t(*)(const char*,char*,size_t))aSyscall[55].pCurrent)

#ifndef _WIN32
  { "lstat",                    (SYSCALL)lstat,                  0 },
#else
  { "lstat",                    (SYSCALL)0,                      0 },
#endif
#define osLstat ((int(*)(const char*,struct stat*))aSyscall[56].pCurrent)

#ifndef _WIN32
  { "__errno",                  (SYSCALL)__errno,                0 },
#else
  { "__errno",                  (SYSCALL)0,                      0 },
#endif
#define osErrno (*((int*(*)(void))aSyscall[57].pCurrent)())

#ifndef _WIN32
  { "cygwin_conv_path",         (SYSCALL)cygwin_conv_path,       0 },
#else
  { "cygwin_conv_path",         (SYSCALL)0,                      0 },
#endif
#define osCygwin_conv_path ((size_t(*)(unsigned int, \
    const void *, void *, size_t))aSyscall[58].pCurrent)

}; /* End of the overrideable system calls */

/*
** This is the xSetSystemCall() method of capdb_vfs for all of the
** "win32" VFSes.  Return CAPDB_OK upon successfully updating the
** system call pointer, or CAPDB_NOTFOUND if there is no configurable
** system call named zName.
*/
static int winSetSystemCall(
  capdb_vfs *pNotUsed,        /* The VFS pointer.  Not used */
  const char *zName,            /* Name of system call to override */
  capdb_syscall_ptr pNewFunc  /* Pointer to new system call value */
){
  unsigned int i;
  int rc = CAPDB_NOTFOUND;

  UNUSED_PARAMETER(pNotUsed);
  if( zName==0 ){
    /* If no zName is given, restore all system calls to their default
    ** settings and return NULL
    */
    rc = CAPDB_OK;
    for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
      if( aSyscall[i].pDefault ){
        aSyscall[i].pCurrent = aSyscall[i].pDefault;
      }
    }
  }else{
    /* If zName is specified, operate on only the one system call
    ** specified.
    */
    for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
      if( strcmp(zName, aSyscall[i].zName)==0 ){
        if( aSyscall[i].pDefault==0 ){
          aSyscall[i].pDefault = aSyscall[i].pCurrent;
        }
        rc = CAPDB_OK;
        if( pNewFunc==0 ) pNewFunc = aSyscall[i].pDefault;
        aSyscall[i].pCurrent = pNewFunc;
        break;
      }
    }
  }
  return rc;
}

/*
** Return the value of a system call.  Return NULL if zName is not a
** recognized system call name.  NULL is also returned if the system call
** is currently undefined.
*/
static capdb_syscall_ptr winGetSystemCall(
  capdb_vfs *pNotUsed,
  const char *zName
){
  unsigned int i;

  UNUSED_PARAMETER(pNotUsed);
  for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
    if( strcmp(zName, aSyscall[i].zName)==0 ) return aSyscall[i].pCurrent;
  }
  return 0;
}

/*
** Return the name of the first system call after zName.  If zName==NULL
** then return the name of the first system call.  Return NULL if zName
** is the last system call or if zName is not the name of a valid
** system call.
*/
static const char *winNextSystemCall(capdb_vfs *p, const char *zName){
  int i = -1;

  UNUSED_PARAMETER(p);
  if( zName ){
    for(i=0; i<ArraySize(aSyscall)-1; i++){
      if( strcmp(zName, aSyscall[i].zName)==0 ) break;
    }
  }
  for(i++; i<ArraySize(aSyscall); i++){
    if( aSyscall[i].pCurrent!=0 ) return aSyscall[i].zName;
  }
  return 0;
}

#ifdef CAPDB_WIN32_MALLOC
#ifdef CAPDB_UWP
# error CAPDB_WIN32_MALLOC is incompatible with CAPDB_UWP
#endif
/*
** If a Win32 native heap has been configured, this function will attempt to
** compact it.  Upon success, CAPDB_OK will be returned.  Upon failure, one
** of CAPDB_NOMEM, CAPDB_ERROR, or CAPDB_NOTFOUND will be returned.  The
** "pnLargest" argument, if non-zero, will be used to return the size of the
** largest committed free block in the heap, in bytes.
*/
int capdb_win32_compact_heap(LPUINT pnLargest){
  int rc = CAPDB_OK;
  UINT nLargest = 0;
  HANDLE hHeap;

  winMemAssertMagic();
  hHeap = winMemGetHeap();
  assert( hHeap!=0 );
  assert( hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
  assert( osHeapValidate(hHeap, CAPDB_WIN32_HEAP_FLAGS, NULL) );
#endif
  if( (nLargest=osHeapCompact(hHeap, CAPDB_WIN32_HEAP_FLAGS))==0 ){
    DWORD lastErrno = osGetLastError();
    if( lastErrno==NO_ERROR ){
      capdb_log(CAPDB_NOMEM, "failed to HeapCompact (no space), heap=%p",
                  (void*)hHeap);
      rc = CAPDB_NOMEM_BKPT;
    }else{
      capdb_log(CAPDB_ERROR, "failed to HeapCompact (%lu), heap=%p",
                  osGetLastError(), (void*)hHeap);
      rc = CAPDB_ERROR;
    }
  }
  if( pnLargest ) *pnLargest = nLargest;
  return rc;
}

/*
** If a Win32 native heap has been configured, this function will attempt to
** destroy and recreate it.  If the Win32 native heap is not isolated and/or
** the capdb_memory_used() function does not return zero, CAPDB_BUSY will
** be returned and no changes will be made to the Win32 native heap.
*/
int capdb_win32_reset_heap(){
  int rc;
  MUTEX_LOGIC( capdb_mutex *pMainMtx; ) /* The main static mutex */
  MUTEX_LOGIC( capdb_mutex *pMem; )    /* The memsys static mutex */
  MUTEX_LOGIC( pMainMtx = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN); )
  MUTEX_LOGIC( pMem = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MEM); )
  capdb_mutex_enter(pMainMtx);
  capdb_mutex_enter(pMem);
  winMemAssertMagic();
  if( winMemGetHeap()!=NULL && winMemGetOwned() && capdb_memory_used()==0 ){
    /*
    ** At this point, there should be no outstanding memory allocations on
    ** the heap.  Also, since both the main and memsys locks are currently
    ** being held by us, no other function (i.e. from another thread) should
    ** be able to even access the heap.  Attempt to destroy and recreate our
    ** isolated Win32 native heap now.
    */
    assert( winMemGetHeap()!=NULL );
    assert( winMemGetOwned() );
    assert( capdb_memory_used()==0 );
    winMemShutdown(winMemGetDataPtr());
    assert( winMemGetHeap()==NULL );
    assert( !winMemGetOwned() );
    assert( capdb_memory_used()==0 );
    rc = winMemInit(winMemGetDataPtr());
    assert( rc!=CAPDB_OK || winMemGetHeap()!=NULL );
    assert( rc!=CAPDB_OK || winMemGetOwned() );
    assert( rc!=CAPDB_OK || capdb_memory_used()==0 );
  }else{
    /*
    ** The Win32 native heap cannot be modified because it may be in use.
    */
    rc = CAPDB_BUSY;
  }
  capdb_mutex_leave(pMem);
  capdb_mutex_leave(pMainMtx);
  return rc;
}
#endif /* CAPDB_WIN32_MALLOC */

/*
** This function outputs the specified (ANSI) string to the Win32 debugger
** (if available).  Undocumented.  Might go away at any moment.
*/
void capdb_win32_write_debug(const char *zBuf, int nBuf){
  char zDbgBuf[CAPDB_WIN32_DBG_BUF_SIZE];
  int nMin = MIN(nBuf, (CAPDB_WIN32_DBG_BUF_SIZE - 1)); /* may be negative. */
  if( nMin<-1 ) nMin = -1; /* all negative values become -1. */
  assert( nMin==-1 || nMin==0 || nMin<CAPDB_WIN32_DBG_BUF_SIZE );
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zBuf ){
    (void)CAPDB_MISUSE_BKPT;
    return;
  }
#endif
  if( nMin>0 ){
    memset(zDbgBuf, 0, CAPDB_WIN32_DBG_BUF_SIZE);
    memcpy(zDbgBuf, zBuf, nMin);
    osOutputDebugStringA(zDbgBuf);
  }else{
    osOutputDebugStringA(zBuf);
  }
}

void capdb_win32_sleep(DWORD milliseconds){
  osSleep(milliseconds);
}

#if CAPDB_MAX_WORKER_THREADS>0 && CAPDB_THREADSAFE>0
DWORD capdbWin32Wait(HANDLE hObject){
  DWORD rc;
  while( (rc = osWaitForSingleObjectEx(hObject, INFINITE,
                                       TRUE))==WAIT_IO_COMPLETION ){}
  return rc;
}
#endif

#ifdef CAPDB_WIN32_MALLOC
/*
** Allocate nBytes of memory.
*/
static void *winMemMalloc(int nBytes){
  HANDLE hHeap;
  void *p;

  winMemAssertMagic();
  hHeap = winMemGetHeap();
  assert( hHeap!=0 );
  assert( hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
  assert( osHeapValidate(hHeap, CAPDB_WIN32_HEAP_FLAGS, NULL) );
#endif
  assert( nBytes>=0 );
  p = osHeapAlloc(hHeap, CAPDB_WIN32_HEAP_FLAGS, (SIZE_T)nBytes);
  if( !p ){
    capdb_log(CAPDB_NOMEM, "failed to HeapAlloc %u bytes (%lu), heap=%p",
                nBytes, osGetLastError(), (void*)hHeap);
  }
  return p;
}

/*
** Free memory.
*/
static void winMemFree(void *pPrior){
  HANDLE hHeap;

  winMemAssertMagic();
  hHeap = winMemGetHeap();
  assert( hHeap!=0 );
  assert( hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
  assert( osHeapValidate(hHeap, CAPDB_WIN32_HEAP_FLAGS, pPrior) );
#endif
  if( !pPrior ) return; /* Passing NULL to HeapFree is undefined. */
  if( !osHeapFree(hHeap, CAPDB_WIN32_HEAP_FLAGS, pPrior) ){
    capdb_log(CAPDB_NOMEM, "failed to HeapFree block %p (%lu), heap=%p",
                pPrior, osGetLastError(), (void*)hHeap);
  }
}

/*
** Change the size of an existing memory allocation
*/
static void *winMemRealloc(void *pPrior, int nBytes){
  HANDLE hHeap;
  void *p;

  winMemAssertMagic();
  hHeap = winMemGetHeap();
  assert( hHeap!=0 );
  assert( hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
  assert( osHeapValidate(hHeap, CAPDB_WIN32_HEAP_FLAGS, pPrior) );
#endif
  assert( nBytes>=0 );
  if( !pPrior ){
    p = osHeapAlloc(hHeap, CAPDB_WIN32_HEAP_FLAGS, (SIZE_T)nBytes);
  }else{
    p = osHeapReAlloc(hHeap, CAPDB_WIN32_HEAP_FLAGS, pPrior, (SIZE_T)nBytes);
  }
  if( !p ){
    capdb_log(CAPDB_NOMEM, "failed to %s %u bytes (%lu), heap=%p",
                pPrior ? "HeapReAlloc" : "HeapAlloc", nBytes, osGetLastError(),
                (void*)hHeap);
  }
  return p;
}

/*
** Return the size of an outstanding allocation, in bytes.
*/
static int winMemSize(void *p){
  HANDLE hHeap;
  SIZE_T n;

  winMemAssertMagic();
  hHeap = winMemGetHeap();
  assert( hHeap!=0 );
  assert( hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
  assert( osHeapValidate(hHeap, CAPDB_WIN32_HEAP_FLAGS, p) );
#endif
  if( !p ) return 0;
  n = osHeapSize(hHeap, CAPDB_WIN32_HEAP_FLAGS, p);
  if( n==(SIZE_T)-1 ){
    capdb_log(CAPDB_NOMEM, "failed to HeapSize block %p (%lu), heap=%p",
                p, osGetLastError(), (void*)hHeap);
    return 0;
  }
  return (int)n;
}

/*
** Round up a request size to the next valid allocation size.
*/
static int winMemRoundup(int n){
  return n;
}

/*
** Initialize this module.
*/
static int winMemInit(void *pAppData){
  winMemData *pWinMemData = (winMemData *)pAppData;

  if( !pWinMemData ) return CAPDB_ERROR;
  assert( pWinMemData->magic1==WINMEM_MAGIC1 );
  assert( pWinMemData->magic2==WINMEM_MAGIC2 );

#if CAPDB_WIN32_HEAP_CREATE
  if( !pWinMemData->hHeap ){
    DWORD dwInitialSize = CAPDB_WIN32_HEAP_INIT_SIZE;
    DWORD dwMaximumSize = (DWORD)capdbGlobalConfig.nHeap;
    if( dwMaximumSize==0 ){
      dwMaximumSize = CAPDB_WIN32_HEAP_MAX_SIZE;
    }else if( dwInitialSize>dwMaximumSize ){
      dwInitialSize = dwMaximumSize;
    }
    pWinMemData->hHeap = osHeapCreate(CAPDB_WIN32_HEAP_FLAGS,
                                      dwInitialSize, dwMaximumSize);
    if( !pWinMemData->hHeap ){
      capdb_log(CAPDB_NOMEM,
          "failed to HeapCreate (%lu), flags=%u, initSize=%lu, maxSize=%lu",
          osGetLastError(), CAPDB_WIN32_HEAP_FLAGS, dwInitialSize,
          dwMaximumSize);
      return CAPDB_NOMEM_BKPT;
    }
    pWinMemData->bOwned = TRUE;
    assert( pWinMemData->bOwned );
  }
#else
  pWinMemData->hHeap = osGetProcessHeap();
  if( !pWinMemData->hHeap ){
    capdb_log(CAPDB_NOMEM,
        "failed to GetProcessHeap (%lu)", osGetLastError());
    return CAPDB_NOMEM_BKPT;
  }
  pWinMemData->bOwned = FALSE;
  assert( !pWinMemData->bOwned );
#endif
  assert( pWinMemData->hHeap!=0 );
  assert( pWinMemData->hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
  assert( osHeapValidate(pWinMemData->hHeap, CAPDB_WIN32_HEAP_FLAGS, NULL) );
#endif
  return CAPDB_OK;
}

/*
** Deinitialize this module.
*/
static void winMemShutdown(void *pAppData){
  winMemData *pWinMemData = (winMemData *)pAppData;

  if( !pWinMemData ) return;
  assert( pWinMemData->magic1==WINMEM_MAGIC1 );
  assert( pWinMemData->magic2==WINMEM_MAGIC2 );

  if( pWinMemData->hHeap ){
    assert( pWinMemData->hHeap!=INVALID_HANDLE_VALUE );
#if defined(CAPDB_WIN32_MALLOC_VALIDATE)
    assert( osHeapValidate(pWinMemData->hHeap, CAPDB_WIN32_HEAP_FLAGS, NULL) );
#endif
    if( pWinMemData->bOwned ){
      if( !osHeapDestroy(pWinMemData->hHeap) ){
        capdb_log(CAPDB_NOMEM, "failed to HeapDestroy (%lu), heap=%p",
                    osGetLastError(), (void*)pWinMemData->hHeap);
      }
      pWinMemData->bOwned = FALSE;
    }
    pWinMemData->hHeap = NULL;
  }
}

/*
** Populate the low-level memory allocation function pointers in
** capdbGlobalConfig.m with pointers to the routines in this file. The
** arguments specify the block of memory to manage.
**
** This routine is only called by capdb_config(), and therefore
** is not required to be threadsafe (it is not).
*/
const capdb_mem_methods *capdbMemGetWin32(void){
  static const capdb_mem_methods winMemMethods = {
    winMemMalloc,
    winMemFree,
    winMemRealloc,
    winMemSize,
    winMemRoundup,
    winMemInit,
    winMemShutdown,
    &win_mem_data
  };
  return &winMemMethods;
}

void capdbMemSetDefault(void){
  capdb_config(CAPDB_CONFIG_MALLOC, capdbMemGetWin32());
}
#endif /* CAPDB_WIN32_MALLOC */

#ifdef _WIN32
/*
** Convert a UTF-8 string to Microsoft Unicode.
**
** Space to hold the returned string is obtained from capdb_malloc().
*/
static LPWSTR winUtf8ToUnicode(const char *zText){
  int nChar;
  LPWSTR zWideText;

  nChar = osMultiByteToWideChar(CP_UTF8, 0, zText, -1, NULL, 0);
  if( nChar==0 ){
    return 0;
  }
  zWideText = capdbMallocZero( nChar*sizeof(WCHAR) );
  if( zWideText==0 ){
    return 0;
  }
  nChar = osMultiByteToWideChar(CP_UTF8, 0, zText, -1, zWideText,
                                nChar);
  if( nChar==0 ){
    capdb_free(zWideText);
    zWideText = 0;
  }
  return zWideText;
}
#endif /* _WIN32 */

/*
** Convert a Microsoft Unicode string to UTF-8.
**
** Space to hold the returned string is obtained from capdb_malloc().
*/
static char *winUnicodeToUtf8(LPCWSTR zWideText){
  int nByte;
  char *zText;

  nByte = osWideCharToMultiByte(CP_UTF8, 0, zWideText, -1, 0, 0, 0, 0);
  if( nByte == 0 ){
    return 0;
  }
  zText = capdbMallocZero( nByte );
  if( zText==0 ){
    return 0;
  }
  nByte = osWideCharToMultiByte(CP_UTF8, 0, zWideText, -1, zText, nByte,
                                0, 0);
  if( nByte == 0 ){
    capdb_free(zText);
    zText = 0;
  }
  return zText;
}

/*
** Convert an ANSI string to Microsoft Unicode, using the ANSI or OEM
** code page.
**
** Space to hold the returned string is obtained from capdb_malloc().
*/
static LPWSTR winMbcsToUnicode(const char *zText, int useAnsi){
  int nWideChar;
  LPWSTR zMbcsText;
  int codepage = useAnsi ? CP_ACP : CP_OEMCP;

  nWideChar = osMultiByteToWideChar(codepage, 0, zText, -1, NULL,
                                0);
  if( nWideChar==0 ){
    return 0;
  }
  zMbcsText = capdbMallocZero( nWideChar*sizeof(WCHAR) );
  if( zMbcsText==0 ){
    return 0;
  }
  nWideChar = osMultiByteToWideChar(codepage, 0, zText, -1, zMbcsText,
                                nWideChar);
  if( nWideChar==0 ){
    capdb_free(zMbcsText);
    zMbcsText = 0;
  }
  return zMbcsText;
}

#ifdef _WIN32
/*
** Convert a Microsoft Unicode string to a multi-byte character string,
** using the ANSI or OEM code page.
**
** Space to hold the returned string is obtained from capdb_malloc().
*/
static char *winUnicodeToMbcs(LPCWSTR zWideText, int useAnsi){
  int nByte;
  char *zText;
  int codepage = useAnsi ? CP_ACP : CP_OEMCP;

  nByte = osWideCharToMultiByte(codepage, 0, zWideText, -1, 0, 0, 0, 0);
  if( nByte == 0 ){
    return 0;
  }
  zText = capdbMallocZero( nByte );
  if( zText==0 ){
    return 0;
  }
  nByte = osWideCharToMultiByte(codepage, 0, zWideText, -1, zText,
                                nByte, 0, 0);
  if( nByte == 0 ){
    capdb_free(zText);
    zText = 0;
  }
  return zText;
}
#endif /* _WIN32 */

/*
** Convert a multi-byte character string to UTF-8.
**
** Space to hold the returned string is obtained from capdb_malloc().
*/
static char *winMbcsToUtf8(const char *zText, int useAnsi){
  char *zTextUtf8;
  LPWSTR zTmpWide;

  zTmpWide = winMbcsToUnicode(zText, useAnsi);
  if( zTmpWide==0 ){
    return 0;
  }
  zTextUtf8 = winUnicodeToUtf8(zTmpWide);
  capdb_free(zTmpWide);
  return zTextUtf8;
}

#ifdef _WIN32
/*
** Convert a UTF-8 string to a multi-byte character string.
**
** Space to hold the returned string is obtained from capdb_malloc().
*/
static char *winUtf8ToMbcs(const char *zText, int useAnsi){
  char *zTextMbcs;
  LPWSTR zTmpWide;

  zTmpWide = winUtf8ToUnicode(zText);
  if( zTmpWide==0 ){
    return 0;
  }
  zTextMbcs = winUnicodeToMbcs(zTmpWide, useAnsi);
  capdb_free(zTmpWide);
  return zTextMbcs;
}

/*
** This is a public wrapper for the winUtf8ToUnicode() function.
*/
LPWSTR capdb_win32_utf8_to_unicode(const char *zText){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zText ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return winUtf8ToUnicode(zText);
}

/*
** This is a public wrapper for the winUnicodeToUtf8() function.
*/
char *capdb_win32_unicode_to_utf8(LPCWSTR zWideText){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zWideText ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return winUnicodeToUtf8(zWideText);
}
#endif /* _WIN32 */

/*
** This is a public wrapper for the winMbcsToUtf8() function.
*/
char *capdb_win32_mbcs_to_utf8(const char *zText){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zText ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return winMbcsToUtf8(zText, osAreFileApisANSI());
}

#ifdef _WIN32
/*
** This is a public wrapper for the winMbcsToUtf8() function.
*/
char *capdb_win32_mbcs_to_utf8_v2(const char *zText, int useAnsi){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zText ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return winMbcsToUtf8(zText, useAnsi);
}

/*
** This is a public wrapper for the winUtf8ToMbcs() function.
*/
char *capdb_win32_utf8_to_mbcs(const char *zText){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zText ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return winUtf8ToMbcs(zText, osAreFileApisANSI());
}

/*
** This is a public wrapper for the winUtf8ToMbcs() function.
*/
char *capdb_win32_utf8_to_mbcs_v2(const char *zText, int useAnsi){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !zText ){
    (void)CAPDB_MISUSE_BKPT;
    return 0;
  }
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize() ) return 0;
#endif
  return winUtf8ToMbcs(zText, useAnsi);
}

/*
** This function is the same as capdb_win32_set_directory (below); however,
** it accepts a UTF-8 string.
*/
int capdb_win32_set_directory8(
  unsigned long type, /* Identifier for directory being set or reset */
  const char *zValue  /* New value for directory being set or reset */
){
  char **ppDirectory = 0;
  int rc;
#ifndef CAPDB_OMIT_AUTOINIT
  rc = capdb_initialize();
  if( rc ) return rc;
#endif
  capdb_mutex_enter(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
  if( type==CAPDB_WIN32_DATA_DIRECTORY_TYPE ){
    ppDirectory = &capdb_data_directory;
  }else if( type==CAPDB_WIN32_TEMP_DIRECTORY_TYPE ){
    ppDirectory = &capdb_temp_directory;
  }
  assert( !ppDirectory || type==CAPDB_WIN32_DATA_DIRECTORY_TYPE
          || type==CAPDB_WIN32_TEMP_DIRECTORY_TYPE
  );
  assert( !ppDirectory || capdbMemdebugHasType(*ppDirectory, MEMTYPE_HEAP) );
  if( ppDirectory ){
    char *zCopy = 0;
    if( zValue && zValue[0] ){
      zCopy = capdb_mprintf("%s", zValue);
      if ( zCopy==0 ){
        rc = CAPDB_NOMEM_BKPT;
        goto set_directory8_done;
      }
    }
    capdb_free(*ppDirectory);
    *ppDirectory = zCopy;
    rc = CAPDB_OK;
  }else{
    rc = CAPDB_ERROR;
  }
set_directory8_done:
  capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
  return rc;
}

/*
** This function is the same as capdb_win32_set_directory (below); however,
** it accepts a UTF-16 string.
*/
int capdb_win32_set_directory16(
  unsigned long type, /* Identifier for directory being set or reset */
  const void *zValue  /* New value for directory being set or reset */
){
  int rc;
  char *zUtf8 = 0;
  if( zValue ){
    zUtf8 = capdb_win32_unicode_to_utf8(zValue);
    if( zUtf8==0 ) return CAPDB_NOMEM_BKPT;
  }
  rc = capdb_win32_set_directory8(type, zUtf8);
  if( zUtf8 ) capdb_free(zUtf8);
  return rc;
}

/*
** This function sets the data directory or the temporary directory based on
** the provided arguments.  The type argument must be 1 in order to set the
** data directory or 2 in order to set the temporary directory.  The zValue
** argument is the name of the directory to use.  The return value will be
** CAPDB_OK if successful.
*/
int capdb_win32_set_directory(
  unsigned long type, /* Identifier for directory being set or reset */
  void *zValue        /* New value for directory being set or reset */
){
  return capdb_win32_set_directory16(type, zValue);
}
#endif /* _WIN32 */

/*
** The return value of winGetLastErrorMsg
** is zero if the error message fits in the buffer, or non-zero
** otherwise (if the message was truncated).
*/
static int winGetLastErrorMsg(DWORD lastErrno, int nBuf, char *zBuf){
  /* FormatMessage returns 0 on failure.  Otherwise it
  ** returns the number of TCHARs written to the output
  ** buffer, excluding the terminating null char.
  */
  DWORD dwLen = 0;
  char *zOut = 0;
  LPWSTR zTempWide = NULL;
  dwLen = osFormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL,
                           lastErrno,
                           0,
                           (LPWSTR) &zTempWide,
                           0,
                           0);
  if( dwLen > 0 ){
    /* allocate a buffer and convert to UTF8 */
    capdbBeginBenignMalloc();
    zOut = winUnicodeToUtf8(zTempWide);
    capdbEndBenignMalloc();
    /* free the system buffer allocated by FormatMessage */
    osLocalFree(zTempWide);
  }
  if( 0 == dwLen ){
    capdb_snprintf(nBuf, zBuf, "OsError 0x%lx (%lu)", lastErrno, lastErrno);
  }else{
    /* copy a maximum of nBuf chars to output buffer */
    capdb_snprintf(nBuf, zBuf, "%s", zOut);
    /* free the UTF8 buffer */
    capdb_free(zOut);
  }
  return 0;
}

/*
**
** This function - winLogErrorAtLine() - is only ever called via the macro
** winLogError().
**
** This routine is invoked after an error occurs in an OS function.
** It logs a message using capdb_log() containing the current value of
** error code and, if possible, the human-readable equivalent from
** FormatMessage.
**
** The first argument passed to the macro should be the error code that
** will be returned to SQLite (e.g. CAPDB_IOERR_DELETE, CAPDB_CANTOPEN).
** The two subsequent arguments should be the name of the OS function that
** failed and the associated file-system path, if any.
*/
#define winLogError(a,b,c,d)   winLogErrorAtLine(a,b,c,d,__LINE__)
static int winLogErrorAtLine(
  int errcode,                    /* SQLite error code */
  DWORD lastErrno,                /* Win32 last error */
  const char *zFunc,              /* Name of OS function that failed */
  const char *zPath,              /* File path associated with error */
  int iLine                       /* Source line number where error occurred */
){
  char zMsg[500];                 /* Human readable error text */
  int i;                          /* Loop counter */

  zMsg[0] = 0;
  winGetLastErrorMsg(lastErrno, sizeof(zMsg), zMsg);
  assert( errcode!=CAPDB_OK );
  if( zPath==0 ) zPath = "";
  for(i=0; zMsg[i] && zMsg[i]!='\r' && zMsg[i]!='\n'; i++){}
  zMsg[i] = 0;
  capdb_log(errcode,
      "os_win.c:%d: (%lu) %s(%s) - %s",
      iLine, lastErrno, zFunc, zPath, zMsg
  );

  return errcode;
}

/*
** The number of times that a ReadFile(), WriteFile(), and DeleteFile()
** will be retried following a locking error - probably caused by
** antivirus software.  Also the initial delay before the first retry.
** The delay increases linearly with each retry.
*/
#ifndef CAPDB_WIN32_IOERR_RETRY
# define CAPDB_WIN32_IOERR_RETRY 10
#endif
#ifndef CAPDB_WIN32_IOERR_RETRY_DELAY
# define CAPDB_WIN32_IOERR_RETRY_DELAY 25
#endif
static int winIoerrRetry = CAPDB_WIN32_IOERR_RETRY;
static int winIoerrRetryDelay = CAPDB_WIN32_IOERR_RETRY_DELAY;

/*
** The "winIoerrCanRetry1" macro is used to determine if a particular I/O
** error code obtained via GetLastError() is eligible to be retried.  It
** must accept the error code DWORD as its only argument and should return
** non-zero if the error code is transient in nature and the operation
** responsible for generating the original error might succeed upon being
** retried.  The argument to this macro should be a variable.
**
** Additionally, a macro named "winIoerrCanRetry2" may be defined.  If it
** is defined, it will be consulted only when the macro "winIoerrCanRetry1"
** returns zero.  The "winIoerrCanRetry2" macro is completely optional and
** may be used to include additional error codes in the set that should
** result in the failing I/O operation being retried by the caller.  If
** defined, the "winIoerrCanRetry2" macro must exhibit external semantics
** identical to those of the "winIoerrCanRetry1" macro.
*/
#if !defined(winIoerrCanRetry1)
#define winIoerrCanRetry1(a) (((a)==ERROR_ACCESS_DENIED)        || \
                              ((a)==ERROR_SHARING_VIOLATION)    || \
                              ((a)==ERROR_LOCK_VIOLATION)       || \
                              ((a)==ERROR_DEV_NOT_EXIST)        || \
                              ((a)==ERROR_NETNAME_DELETED)      || \
                              ((a)==ERROR_SEM_TIMEOUT)          || \
                              ((a)==ERROR_NETWORK_UNREACHABLE))
#endif

/*
** If a ReadFile() or WriteFile() error occurs, invoke this routine
** to see if it should be retried.  Return TRUE to retry.  Return FALSE
** to give up with an error.
*/
static int winRetryIoerr(int *pnRetry, DWORD *pError){
  DWORD e = osGetLastError();
  if( *pnRetry>=winIoerrRetry ){
    if( pError ){
      *pError = e;
    }
    return 0;
  }
  if( winIoerrCanRetry1(e) ){
    capdb_win32_sleep(winIoerrRetryDelay*(1+*pnRetry));
    ++*pnRetry;
    return 1;
  }
#if defined(winIoerrCanRetry2)
  else if( winIoerrCanRetry2(e) ){
    capdb_win32_sleep(winIoerrRetryDelay*(1+*pnRetry));
    ++*pnRetry;
    return 1;
  }
#endif
  if( pError ){
    *pError = e;
  }
  return 0;
}

/*
** Log a I/O error retry episode.
*/
static void winLogIoerr(int nRetry, int lineno){
  if( nRetry ){
    capdb_log(CAPDB_NOTICE,
      "delayed %dms for lock/sharing conflict at line %d",
      winIoerrRetryDelay*nRetry*(nRetry+1)/2, lineno
    );
  }
}


/*
** Lock a file region.
*/
static BOOL winLockFile(
  LPHANDLE phFile,
  DWORD flags,
  DWORD offsetLow,
  DWORD offsetHigh,
  DWORD numBytesLow,
  DWORD numBytesHigh
){
  OVERLAPPED ovlp;
  memset(&ovlp, 0, sizeof(OVERLAPPED));
  ovlp.Offset = offsetLow;
  ovlp.OffsetHigh = offsetHigh;
  return osLockFileEx(*phFile, flags, 0, numBytesLow, numBytesHigh, &ovlp);
}

#ifndef CAPDB_OMIT_WAL
/*
** Lock a region of nByte bytes starting at offset offset of file hFile.
** Take an EXCLUSIVE lock if parameter bExclusive is true, or a SHARED lock
** otherwise. If nMs is greater than zero and the lock cannot be obtained
** immediately, block for that many ms before giving up.
**
** This function returns CAPDB_OK if the lock is obtained successfully. If
** some other process holds the lock, CAPDB_BUSY is returned if nMs==0, or
** CAPDB_BUSY_TIMEOUT otherwise. Or, if an error occurs, CAPDB_IOERR.
*/
static int winHandleLockTimeout(
  HANDLE hFile,
  DWORD offset,
  DWORD nByte,
  int bExcl,
  DWORD nMs
){
  DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (bExcl?LOCKFILE_EXCLUSIVE_LOCK:0);
  int rc = CAPDB_OK;
  BOOL ret;
  OVERLAPPED ovlp;
  memset(&ovlp, 0, sizeof(OVERLAPPED));
  ovlp.Offset = offset;

#if defined(CAPDB_ENABLE_SETLK_TIMEOUT)
  if( nMs!=0 ){
    flags &= ~LOCKFILE_FAIL_IMMEDIATELY;
  }
  ovlp.hEvent = osCreateEvent(NULL, TRUE, FALSE, NULL);
  if( ovlp.hEvent==NULL ){
    return CAPDB_IOERR_LOCK;
  }
#endif
  ret = osLockFileEx(hFile, flags, 0, nByte, 0, &ovlp);
#if defined(CAPDB_ENABLE_SETLK_TIMEOUT)
  /* If CAPDB_ENABLE_SETLK_TIMEOUT is defined, then the file-handle was
  ** opened with FILE_FLAG_OVERHEAD specified. In this case, the call to
  ** LockFileEx() may fail because the request is still pending. This can
  ** happen even if LOCKFILE_FAIL_IMMEDIATELY was specified.  
  **
  ** If nMs is 0, then LOCKFILE_FAIL_IMMEDIATELY was set in the flags 
  ** passed to LockFileEx(). In this case, if the operation is pending,
  ** block indefinitely until it is finished.
  **
  ** Otherwise, wait for up to nMs ms for the operation to finish. nMs
  ** may be set to INFINITE.
  */
  if( !ret && GetLastError()==ERROR_IO_PENDING ){
    DWORD nDelay = (nMs==0 ? INFINITE : nMs);
    DWORD res = osWaitForSingleObject(ovlp.hEvent, nDelay);
    if( res==WAIT_OBJECT_0 ){
      ret = TRUE;
    }else if( res==WAIT_TIMEOUT ){
#if CAPDB_ENABLE_SETLK_TIMEOUT==1
      rc = CAPDB_BUSY_TIMEOUT;
#else
      rc = CAPDB_BUSY;
#endif
    }else{
      /* Some other error has occurred */
      rc = CAPDB_IOERR_LOCK;
    }

    /* If it is still pending, cancel the LockFileEx() call. */
    osCancelIo(hFile);
  }

  osCloseHandle(ovlp.hEvent);
#endif /* defined(CAPDB_ENABLE_SETLK_TIMEOUT) */

  if( rc==CAPDB_OK && !ret ){
    rc = CAPDB_BUSY;
  }
  return rc;
}
#endif /* #ifndef CAPDB_OMIT_WAL */

/*
** Unlock a file region.
 */
static BOOL winUnlockFile(
  LPHANDLE phFile,
  DWORD offsetLow,
  DWORD offsetHigh,
  DWORD numBytesLow,
  DWORD numBytesHigh
){
  OVERLAPPED ovlp;
  memset(&ovlp, 0, sizeof(OVERLAPPED));
  ovlp.Offset = offsetLow;
  ovlp.OffsetHigh = offsetHigh;
  return osUnlockFileEx(*phFile, 0, numBytesLow, numBytesHigh, &ovlp);
}

#ifndef CAPDB_OMIT_WAL
/*
** Remove an nByte lock starting at offset iOff from HANDLE h.
*/
static int winHandleUnlock(HANDLE h, int iOff, int nByte){
  BOOL ret = winUnlockFile(&h, iOff, 0, nByte, 0);
  return (ret ? CAPDB_OK : CAPDB_IOERR_UNLOCK);
}
#endif

/*****************************************************************************
** The next group of routines implement the I/O methods specified
** by the capdb_io_methods object.
******************************************************************************/

/*
** Some Microsoft compilers lack this definition.
*/
#ifndef INVALID_SET_FILE_POINTER
# define INVALID_SET_FILE_POINTER ((DWORD)-1)
#endif

/*
** Seek the file handle h to offset nByte of the file.
**
** If successful, return CAPDB_OK. Or, if an error occurs, return an SQLite
** error code.
*/
static int winHandleSeek(HANDLE h, capdb_int64 iOffset){
  int rc = CAPDB_OK;             /* Return value */
  LARGE_INTEGER x;                /* The offset */

  x.QuadPart = iOffset;
  if( osSetFilePointerEx(h, x, 0, FILE_BEGIN)==0 ){
    rc = CAPDB_IOERR_SEEK;
  }
  OSTRACE(("SEEK file=%p, offset=%lld rc=%s\n", h, iOffset,capdbErrName(rc)));
  return rc;
}

/*
** Move the current position of the file handle passed as the first
** argument to offset iOffset within the file. If successful, return 0.
** Otherwise, set pFile->lastErrno and return non-zero.
*/
static int winSeekFile(winFile *pFile, capdb_int64 iOffset){
  int rc;

  rc = winHandleSeek(pFile->h, iOffset);
  if( rc!=CAPDB_OK ){
    pFile->lastErrno = osGetLastError();
    winLogError(rc, pFile->lastErrno, "winSeekFile", pFile->zPath);
  }
  return rc;
}


#if CAPDB_MAX_MMAP_SIZE>0
/* Forward references to VFS helper methods used for memory mapped files */
static int winMapfile(winFile*, capdb_int64);
static int winUnmapfile(winFile*);
#endif

/*
** Close a file.
**
** It is reported that an attempt to close a handle might sometimes
** fail.  This is a very unreasonable result, but Windows is notorious
** for being unreasonable so I do not doubt that it might happen.  If
** the close fails, we pause for 100 milliseconds and try again.  As
** many as MX_CLOSE_ATTEMPT attempts to close the handle are made before
** giving up and returning an error.
*/
#define MX_CLOSE_ATTEMPT 3
static int winClose(capdb_file *id){
  int rc, cnt = 0;
  winFile *pFile = (winFile*)id;

  assert( id!=0 );
#ifndef CAPDB_OMIT_WAL
  assert( pFile->pShm==0 );
#endif
  assert( pFile->h!=NULL && pFile->h!=INVALID_HANDLE_VALUE );
  OSTRACE(("CLOSE pid=%lu, pFile=%p, file=%p\n",
           osGetCurrentProcessId(), pFile, pFile->h));

#if CAPDB_MAX_MMAP_SIZE>0
  winUnmapfile(pFile);
#endif

  do{
    rc = osCloseHandle(pFile->h);
    /* SimulateIOError( rc=0; cnt=MX_CLOSE_ATTEMPT; ); */
  }while( rc==0 && ++cnt < MX_CLOSE_ATTEMPT && (capdb_win32_sleep(100), 1) );
  if( rc ){
    pFile->h = NULL;
  }
  OpenCounter(-1);
  OSTRACE(("CLOSE pid=%lu, pFile=%p, file=%p, rc=%s\n",
           osGetCurrentProcessId(), pFile, pFile->h, rc ? "ok" : "failed"));
  return rc ? CAPDB_OK
            : winLogError(CAPDB_IOERR_CLOSE, osGetLastError(),
                          "winClose", pFile->zPath);
}

/*
** Read data from a file into a buffer.  Return CAPDB_OK if all
** bytes were read successfully and CAPDB_IOERR if anything goes
** wrong.
*/
static int winRead(
  capdb_file *id,          /* File to read from */
  void *pBuf,                /* Write content into this buffer */
  int amt,                   /* Number of bytes to read */
  capdb_int64 offset       /* Begin reading at this offset */
){
#if !defined(CAPDB_WIN32_NO_OVERLAPPED)
  OVERLAPPED overlapped;          /* The offset for ReadFile. */
#endif
  winFile *pFile = (winFile*)id;  /* file handle */
  DWORD nRead;                    /* Number of bytes actually read from file */
  int nRetry = 0;                 /* Number of retrys */

  assert( id!=0 );
  assert( amt>0 );
  assert( offset>=0 );
  SimulateIOError(return CAPDB_IOERR_READ);
  OSTRACE(("READ pid=%lu, pFile=%p, file=%p, buffer=%p, amount=%d, "
           "offset=%lld, lock=%d\n", osGetCurrentProcessId(), pFile,
           pFile->h, pBuf, amt, offset, pFile->locktype));

#if CAPDB_MAX_MMAP_SIZE>0
  /* Deal with as much of this read request as possible by transferring
  ** data from the memory mapping using memcpy().  */
  if( offset<pFile->mmapSize ){
    if( offset+amt <= pFile->mmapSize ){
      memcpy(pBuf, &((u8 *)(pFile->pMapRegion))[offset], amt);
      OSTRACE(("READ-MMAP pid=%lu, pFile=%p, file=%p, rc=CAPDB_OK\n",
               osGetCurrentProcessId(), pFile, pFile->h));
      return CAPDB_OK;
    }else{
      int nCopy = (int)(pFile->mmapSize - offset);
      memcpy(pBuf, &((u8 *)(pFile->pMapRegion))[offset], nCopy);
      pBuf = &((u8 *)pBuf)[nCopy];
      amt -= nCopy;
      offset += nCopy;
    }
  }
#endif

#if defined(CAPDB_WIN32_NO_OVERLAPPED)
  if( winSeekFile(pFile, offset) ){
    OSTRACE(("READ pid=%lu, pFile=%p, file=%p, rc=CAPDB_FULL\n",
             osGetCurrentProcessId(), pFile, pFile->h));
    return CAPDB_FULL;
  }
  while( !osReadFile(pFile->h, pBuf, amt, &nRead, 0) ){
#else
  memset(&overlapped, 0, sizeof(OVERLAPPED));
  overlapped.Offset = (LONG)(offset & 0xffffffff);
  overlapped.OffsetHigh = (LONG)((offset>>32) & 0x7fffffff);
  while( !osReadFile(pFile->h, pBuf, amt, &nRead, &overlapped) &&
         osGetLastError()!=ERROR_HANDLE_EOF ){
#endif
    DWORD lastErrno;
    if( winRetryIoerr(&nRetry, &lastErrno) ) continue;
    pFile->lastErrno = lastErrno;
    OSTRACE(("READ pid=%lu, pFile=%p, file=%p, rc=CAPDB_IOERR_READ\n",
             osGetCurrentProcessId(), pFile, pFile->h));
    return winLogError(CAPDB_IOERR_READ, pFile->lastErrno,
                       "winRead", pFile->zPath);
  }
  winLogIoerr(nRetry, __LINE__);
  if( nRead<(DWORD)amt ){
    /* Unread parts of the buffer must be zero-filled */
    memset(&((char*)pBuf)[nRead], 0, amt-nRead);
    OSTRACE(("READ pid=%lu, pFile=%p, file=%p, rc=CAPDB_IOERR_SHORT_READ\n",
             osGetCurrentProcessId(), pFile, pFile->h));
    return CAPDB_IOERR_SHORT_READ;
  }

  OSTRACE(("READ pid=%lu, pFile=%p, file=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), pFile, pFile->h));
  return CAPDB_OK;
}

/*
** Write data from a buffer into a file.  Return CAPDB_OK on success
** or some other error code on failure.
*/
static int winWrite(
  capdb_file *id,               /* File to write into */
  const void *pBuf,               /* The bytes to be written */
  int amt,                        /* Number of bytes to write */
  capdb_int64 offset            /* Offset into the file to begin writing at */
){
  int rc = 0;                     /* True if error has occurred, else false */
  winFile *pFile = (winFile*)id;  /* File handle */
  int nRetry = 0;                 /* Number of retries */

  assert( amt>0 );
  assert( pFile );
  SimulateIOError(return CAPDB_IOERR_WRITE);
  SimulateDiskfullError(return CAPDB_FULL);

  OSTRACE(("WRITE pid=%lu, pFile=%p, file=%p, buffer=%p, amount=%d, "
           "offset=%lld, lock=%d\n", osGetCurrentProcessId(), pFile,
           pFile->h, pBuf, amt, offset, pFile->locktype));

#if defined(CAPDB_MMAP_READWRITE) && CAPDB_MAX_MMAP_SIZE>0
  /* Deal with as much of this write request as possible by transferring
  ** data from the memory mapping using memcpy().  */
  if( offset<pFile->mmapSize ){
    if( offset+amt <= pFile->mmapSize ){
      memcpy(&((u8 *)(pFile->pMapRegion))[offset], pBuf, amt);
      OSTRACE(("WRITE-MMAP pid=%lu, pFile=%p, file=%p, rc=CAPDB_OK\n",
               osGetCurrentProcessId(), pFile, pFile->h));
      return CAPDB_OK;
    }else{
      int nCopy = (int)(pFile->mmapSize - offset);
      memcpy(&((u8 *)(pFile->pMapRegion))[offset], pBuf, nCopy);
      pBuf = &((u8 *)pBuf)[nCopy];
      amt -= nCopy;
      offset += nCopy;
    }
  }
#endif

#if defined(CAPDB_WIN32_NO_OVERLAPPED)
  rc = winSeekFile(pFile, offset);
  if( rc==0 ){
#else
  {
#endif
#if !defined(CAPDB_WIN32_NO_OVERLAPPED)
    OVERLAPPED overlapped;        /* The offset for WriteFile. */
#endif
    u8 *aRem = (u8 *)pBuf;        /* Data yet to be written */
    int nRem = amt;               /* Number of bytes yet to be written */
    DWORD nWrite;                 /* Bytes written by each WriteFile() call */
    DWORD lastErrno = NO_ERROR;   /* Value returned by GetLastError() */

#if !defined(CAPDB_WIN32_NO_OVERLAPPED)
    memset(&overlapped, 0, sizeof(OVERLAPPED));
    overlapped.Offset = (LONG)(offset & 0xffffffff);
    overlapped.OffsetHigh = (LONG)((offset>>32) & 0x7fffffff);
#endif

    while( nRem>0 ){
#if defined(CAPDB_WIN32_NO_OVERLAPPED)
      if( !osWriteFile(pFile->h, aRem, nRem, &nWrite, 0) ){
#else
      if( !osWriteFile(pFile->h, aRem, nRem, &nWrite, &overlapped) ){
#endif
        if( winRetryIoerr(&nRetry, &lastErrno) ) continue;
        break;
      }
      assert( nWrite==0 || nWrite<=(DWORD)nRem );
      if( nWrite==0 || nWrite>(DWORD)nRem ){
        lastErrno = osGetLastError();
        break;
      }
#if !defined(CAPDB_WIN32_NO_OVERLAPPED)
      offset += nWrite;
      overlapped.Offset = (LONG)(offset & 0xffffffff);
      overlapped.OffsetHigh = (LONG)((offset>>32) & 0x7fffffff);
#endif
      aRem += nWrite;
      nRem -= nWrite;
    }
    if( nRem>0 ){
      pFile->lastErrno = lastErrno;
      rc = 1;
    }
  }

  if( rc ){
    if(   ( pFile->lastErrno==ERROR_HANDLE_DISK_FULL )
       || ( pFile->lastErrno==ERROR_DISK_FULL )){
      OSTRACE(("WRITE pid=%lu, pFile=%p, file=%p, rc=CAPDB_FULL\n",
               osGetCurrentProcessId(), pFile, pFile->h));
      return winLogError(CAPDB_FULL, pFile->lastErrno,
                         "winWrite1", pFile->zPath);
    }
    OSTRACE(("WRITE pid=%lu, pFile=%p, file=%p, rc=CAPDB_IOERR_WRITE\n",
             osGetCurrentProcessId(), pFile, pFile->h));
    return winLogError(CAPDB_IOERR_WRITE, pFile->lastErrno,
                       "winWrite2", pFile->zPath);
  }else{
    winLogIoerr(nRetry, __LINE__);
  }
  OSTRACE(("WRITE pid=%lu, pFile=%p, file=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), pFile, pFile->h));
  return CAPDB_OK;
}

#ifndef CAPDB_OMIT_WAL
/*
** Truncate the file opened by handle h to nByte bytes in size.
*/
static int winHandleTruncate(HANDLE h, capdb_int64 nByte){ 
  int rc = CAPDB_OK;             /* Return code */
  rc = winHandleSeek(h, nByte);
  if( rc==CAPDB_OK ){
    if( 0==osSetEndOfFile(h) ){
      rc = CAPDB_IOERR_TRUNCATE;
    }
  }
  return rc;
}

/*
** Determine the size in bytes of the file opened by the handle passed as 
** the first argument.
*/
static int winHandleSize(HANDLE h, capdb_int64 *pnByte){ 
  int rc = CAPDB_OK;
  LARGE_INTEGER x;
  assert( pnByte );
  if( osGetFileSizeEx(h, &x)==0 ){
    rc = CAPDB_IOERR_FSTAT;
    *pnByte = 0;
  }else{
    *pnByte = x.QuadPart;
  }
  return rc;
}

/*
** Close the handle passed as the only argument.
*/
static void winHandleClose(HANDLE h){
  if( h!=INVALID_HANDLE_VALUE ){
    osCloseHandle(h);
  }
}
#endif /* #ifndef CAPDB_OMIT_WAL */

/*
** Truncate an open file to a specified size
*/
static int winTruncate(capdb_file *id, capdb_int64 nByte){
  winFile *pFile = (winFile*)id;  /* File handle object */
  int rc = CAPDB_OK;             /* Return code for this function */
  DWORD lastErrno;
#if CAPDB_MAX_MMAP_SIZE>0
  capdb_int64 oldMmapSize;
  if( pFile->nFetchOut>0 ){
    /* File truncation is a no-op if there are outstanding memory mapped
    ** pages.  This is because truncating the file means temporarily unmapping
    ** the file, and that might delete memory out from under existing cursors.
    **
    ** This can result in incremental vacuum not truncating the file,
    ** if there is an active read cursor when the incremental vacuum occurs.
    ** No real harm comes of this - the database file is not corrupted,
    ** though some folks might complain that the file is bigger than it
    ** needs to be.
    **
    ** The only feasible work-around is to defer the truncation until after
    ** all references to memory-mapped content are closed.  That is doable,
    ** but involves adding a few branches in the common write code path which
    ** could slow down normal operations slightly.  Hence, we have decided for
    ** now to simply make transactions a no-op if there are pending reads.  We
    ** can maybe revisit this decision in the future.
    */
    return CAPDB_OK;
  }
#endif

  assert( pFile );
  SimulateIOError(return CAPDB_IOERR_TRUNCATE);
  OSTRACE(("TRUNCATE pid=%lu, pFile=%p, file=%p, size=%lld, lock=%d\n",
           osGetCurrentProcessId(), pFile, pFile->h, nByte, pFile->locktype));

  /* If the user has configured a chunk-size for this file, truncate the
  ** file so that it consists of an integer number of chunks (i.e. the
  ** actual file size after the operation may be larger than the requested
  ** size).
  */
  if( pFile->szChunk>0 ){
    nByte = ((nByte + pFile->szChunk - 1)/pFile->szChunk) * pFile->szChunk;
  }

#if CAPDB_MAX_MMAP_SIZE>0
  if( pFile->pMapRegion ){
    oldMmapSize = pFile->mmapSize;
  }else{
    oldMmapSize = 0;
  }
  winUnmapfile(pFile);
#endif

  /* SetEndOfFile() returns non-zero when successful, or zero when it fails. */
  if( winSeekFile(pFile, nByte) ){
    rc = winLogError(CAPDB_IOERR_TRUNCATE, pFile->lastErrno,
                     "winTruncate1", pFile->zPath);
  }else if( 0==osSetEndOfFile(pFile->h) &&
            ((lastErrno = osGetLastError())!=ERROR_USER_MAPPED_FILE) ){
    pFile->lastErrno = lastErrno;
    rc = winLogError(CAPDB_IOERR_TRUNCATE, pFile->lastErrno,
                     "winTruncate2", pFile->zPath);
  }

#if CAPDB_MAX_MMAP_SIZE>0
  if( rc==CAPDB_OK && oldMmapSize>0 ){
    if( oldMmapSize>nByte ){
      winMapfile(pFile, -1);
    }else{
      winMapfile(pFile, oldMmapSize);
    }
  }
#endif

  OSTRACE(("TRUNCATE pid=%lu, pFile=%p, file=%p, rc=%s\n",
           osGetCurrentProcessId(), pFile, pFile->h, capdbErrName(rc)));
  return rc;
}

#ifdef CAPDB_TEST
/*
** Count the number of fullsyncs and normal syncs.  This is used to test
** that syncs and fullsyncs are occurring at the right times.
*/
int capdb_sync_count = 0;
int capdb_fullsync_count = 0;
#endif

/*
** Make sure all writes to a particular file are committed to disk.
*/
static int winSync(capdb_file *id, int flags){
#ifndef CAPDB_NO_SYNC
  /*
  ** Used only when CAPDB_NO_SYNC is not defined.
   */
  BOOL rc;
#endif
#if !defined(NDEBUG) || !defined(CAPDB_NO_SYNC) || \
    defined(CAPDB_HAVE_OS_TRACE)
  /*
  ** Used when CAPDB_NO_SYNC is not defined and by the assert() and/or
  ** OSTRACE() macros.
   */
  winFile *pFile = (winFile*)id;
#else
  UNUSED_PARAMETER(id);
#endif

  assert( pFile );
  /* Check that one of CAPDB_SYNC_NORMAL or FULL was passed */
  assert((flags&0x0F)==CAPDB_SYNC_NORMAL
      || (flags&0x0F)==CAPDB_SYNC_FULL
  );

  /* Unix cannot, but some systems may return CAPDB_FULL from here. This
  ** line is to test that doing so does not cause any problems.
  */
  SimulateDiskfullError( return CAPDB_FULL );

  OSTRACE(("SYNC pid=%lu, pFile=%p, file=%p, flags=%x, lock=%d\n",
           osGetCurrentProcessId(), pFile, pFile->h, flags,
           pFile->locktype));

#ifndef CAPDB_TEST
  UNUSED_PARAMETER(flags);
#else
  if( (flags&0x0F)==CAPDB_SYNC_FULL ){
    capdb_fullsync_count++;
  }
  capdb_sync_count++;
#endif

  /* If we compiled with the CAPDB_NO_SYNC flag, then syncing is a
  ** no-op
  */
#ifdef CAPDB_NO_SYNC
  OSTRACE(("SYNC-NOP pid=%lu, pFile=%p, file=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), pFile, pFile->h));
  return CAPDB_OK;
#else
#if CAPDB_MAX_MMAP_SIZE>0
  if( pFile->pMapRegion ){
    if( osFlushViewOfFile(pFile->pMapRegion, 0) ){
      OSTRACE(("SYNC-MMAP pid=%lu, pFile=%p, pMapRegion=%p, "
               "rc=CAPDB_OK\n", osGetCurrentProcessId(),
               pFile, pFile->pMapRegion));
    }else{
      pFile->lastErrno = osGetLastError();
      OSTRACE(("SYNC-MMAP pid=%lu, pFile=%p, pMapRegion=%p, "
               "rc=CAPDB_IOERR_MMAP\n", osGetCurrentProcessId(),
               pFile, pFile->pMapRegion));
      return winLogError(CAPDB_IOERR_MMAP, pFile->lastErrno,
                         "winSync1", pFile->zPath);
    }
  }
#endif
  rc = osFlushFileBuffers(pFile->h);
  SimulateIOError( rc=FALSE );
  if( rc ){
    OSTRACE(("SYNC pid=%lu, pFile=%p, file=%p, rc=CAPDB_OK\n",
             osGetCurrentProcessId(), pFile, pFile->h));
    return CAPDB_OK;
  }else{
    pFile->lastErrno = osGetLastError();
    OSTRACE(("SYNC pid=%lu, pFile=%p, file=%p, rc=CAPDB_IOERR_FSYNC\n",
             osGetCurrentProcessId(), pFile, pFile->h));
    return winLogError(CAPDB_IOERR_FSYNC, pFile->lastErrno,
                       "winSync2", pFile->zPath);
  }
#endif
}

/*
** Determine the current size of a file in bytes
*/
static int winFileSize(capdb_file *id, capdb_int64 *pSize){
  winFile *pFile = (winFile*)id;
  int rc = CAPDB_OK;

  assert( id!=0 );
  assert( pSize!=0 );
  SimulateIOError(return CAPDB_IOERR_FSTAT);
  OSTRACE(("SIZE file=%p, pSize=%p\n", pFile->h, pSize));
  {
    LARGE_INTEGER x;
    if( osGetFileSizeEx(pFile->h, &x)==0 ){
      *pSize = 0;
      pFile->lastErrno = osGetLastError();
      rc = winLogError(CAPDB_IOERR_FSTAT, pFile->lastErrno,
                       "winFileSize", pFile->zPath);
    }else{
      *pSize = x.QuadPart;
    }
  }
  OSTRACE(("SIZE file=%p, pSize=%p, *pSize=%lld, rc=%s\n",
           pFile->h, pSize, *pSize, capdbErrName(rc)));
  return rc;
}

/*
** LOCKFILE_FAIL_IMMEDIATELY is undefined on some Windows systems.
*/
#ifndef LOCKFILE_FAIL_IMMEDIATELY
# define LOCKFILE_FAIL_IMMEDIATELY 1
#endif

#ifndef LOCKFILE_EXCLUSIVE_LOCK
# define LOCKFILE_EXCLUSIVE_LOCK 2
#endif

/*
** Historically, SQLite has used both the LockFile and LockFileEx functions.
** When the LockFile function was used, it was always expected to fail
** immediately if the lock could not be obtained.  Also, it always expected to
** obtain an exclusive lock.  These flags are used with the LockFileEx function
** and reflect those expectations; therefore, they should not be changed.
*/
#ifndef CAPDB_LOCKFILE_FLAGS
# define CAPDB_LOCKFILE_FLAGS   (LOCKFILE_FAIL_IMMEDIATELY | \
                                  LOCKFILE_EXCLUSIVE_LOCK)
#endif

/*
** Currently, SQLite never calls the LockFileEx function without wanting the
** call to fail immediately if the lock cannot be obtained.
*/
#ifndef CAPDB_LOCKFILEEX_FLAGS
# define CAPDB_LOCKFILEEX_FLAGS (LOCKFILE_FAIL_IMMEDIATELY)
#endif

/*
** Acquire a reader lock.
** Different API routines are called depending on whether or not this
** is Win9x or WinNT.
*/
static int winGetReadLock(winFile *pFile, int bBlock){
  int res;
  DWORD mask = ~(bBlock ? LOCKFILE_FAIL_IMMEDIATELY : 0);
  OSTRACE(("READ-LOCK file=%p, lock=%d\n", pFile->h, pFile->locktype));
  res = winLockFile(&pFile->h, CAPDB_LOCKFILEEX_FLAGS&mask, SHARED_FIRST, 0,
                    SHARED_SIZE, 0);
  if( res == 0 ){
    pFile->lastErrno = osGetLastError();
    /* No need to log a failure to lock */
  }
  OSTRACE(("READ-LOCK file=%p, result=%d\n", pFile->h, res));
  return res;
}

/*
** Undo a readlock
*/
static int winUnlockReadLock(winFile *pFile){
  int res;
  DWORD lastErrno;
  OSTRACE(("READ-UNLOCK file=%p, lock=%d\n", pFile->h, pFile->locktype));
  res = winUnlockFile(&pFile->h, SHARED_FIRST, 0, SHARED_SIZE, 0);
  if( res==0 && ((lastErrno = osGetLastError())!=ERROR_NOT_LOCKED) ){
    pFile->lastErrno = lastErrno;
    winLogError(CAPDB_IOERR_UNLOCK, pFile->lastErrno,
                "winUnlockReadLock", pFile->zPath);
  }
  OSTRACE(("READ-UNLOCK file=%p, result=%d\n", pFile->h, res));
  return res;
}

/*
** Lock the file with the lock specified by parameter locktype - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  The winUnlock() routine
** erases all locks at once and returns us immediately to locking level 0.
** It is not possible to lower the locking level one step at a time.  You
** must go straight to locking level 0.
*/
static int winLock(capdb_file *id, int locktype){
  int rc = CAPDB_OK;    /* Return code from subroutines */
  int res = 1;           /* Result of a Windows lock call */
  int newLocktype;       /* Set pFile->locktype to this value before exiting */
  int gotPendingLock = 0;/* True if we acquired a PENDING lock this time */
  winFile *pFile = (winFile*)id;
  DWORD lastErrno = NO_ERROR;

  assert( id!=0 );
  OSTRACE(("LOCK file=%p, oldLock=%d(%d), newLock=%d\n",
           pFile->h, pFile->locktype, pFile->sharedLockByte, locktype));

  /* If there is already a lock of this type or more restrictive on the
  ** OsFile, do nothing. Don't use the end_lock: exit path, as
  ** capdbOsEnterMutex() hasn't been called yet.
  */
  if( pFile->locktype>=locktype ){
    OSTRACE(("LOCK-HELD file=%p, rc=CAPDB_OK\n", pFile->h));
    return CAPDB_OK;
  }

  /* Do not allow any kind of write-lock on a read-only database
  */
  if( (pFile->ctrlFlags & WINFILE_RDONLY)!=0 && locktype>=RESERVED_LOCK ){
    return CAPDB_IOERR_LOCK;
  }

  /* Make sure the locking sequence is correct
  */
  assert( pFile->locktype!=NO_LOCK || locktype==SHARED_LOCK );
  assert( locktype!=PENDING_LOCK );
  assert( locktype!=RESERVED_LOCK || pFile->locktype==SHARED_LOCK );

  /* Lock the PENDING_LOCK byte if we need to acquire an EXCLUSIVE lock or
  ** a SHARED lock.  If we are acquiring a SHARED lock, the acquisition of
  ** the PENDING_LOCK byte is temporary.
  */
  newLocktype = pFile->locktype;
  if( locktype==SHARED_LOCK
   || (locktype==EXCLUSIVE_LOCK && pFile->locktype==RESERVED_LOCK)
  ){
    int cnt = 3;

    /* Flags for the LockFileEx() call. This should be an exclusive lock if
    ** this call is to obtain EXCLUSIVE, or a shared lock if this call is to
    ** obtain SHARED.  */
    int flags = LOCKFILE_FAIL_IMMEDIATELY;
    if( locktype==EXCLUSIVE_LOCK ){
      flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    while( cnt>0 ){
      /* Try 3 times to get the pending lock.  This is needed to work
      ** around problems caused by indexing and/or anti-virus software on
      ** Windows systems.
      **
      ** If you are using this code as a model for alternative VFSes, do not
      ** copy this retry logic.  It is a hack intended for Windows only.  */
      res = winLockFile(&pFile->h, flags, PENDING_BYTE, 0, 1, 0);
      if( res ) break;

      lastErrno = osGetLastError();
      OSTRACE(("LOCK-PENDING-FAIL file=%p, count=%d, result=%d\n", 
            pFile->h, cnt, res
      ));

      if( lastErrno==ERROR_INVALID_HANDLE ){
        pFile->lastErrno = lastErrno;
        rc = CAPDB_IOERR_LOCK;
        OSTRACE(("LOCK-FAIL file=%p, count=%d, rc=%s\n", 
              pFile->h, cnt, capdbErrName(rc)
        ));
        return rc;
      }

      cnt--;
      if( cnt>0 ) capdb_win32_sleep(1);
    }
    gotPendingLock = res;
  }

  /* Acquire a shared lock
  */
  if( locktype==SHARED_LOCK && res ){
    assert( pFile->locktype==NO_LOCK );
#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
    res = winGetReadLock(pFile, pFile->bBlockOnConnect);
#else
    res = winGetReadLock(pFile, 0);
#endif
    if( res ){
      newLocktype = SHARED_LOCK;
    }else{
      lastErrno = osGetLastError();
    }
  }

  /* Acquire a RESERVED lock
  */
  if( locktype==RESERVED_LOCK && res ){
    assert( pFile->locktype==SHARED_LOCK );
    res = winLockFile(&pFile->h, CAPDB_LOCKFILE_FLAGS, RESERVED_BYTE, 0, 1, 0);
    if( res ){
      newLocktype = RESERVED_LOCK;
    }else{
      lastErrno = osGetLastError();
    }
  }

  /* Acquire a PENDING lock
  */
  if( locktype==EXCLUSIVE_LOCK && res ){
    newLocktype = PENDING_LOCK;
    gotPendingLock = 0;
  }

  /* Acquire an EXCLUSIVE lock
  */
  if( locktype==EXCLUSIVE_LOCK && res ){
    assert( pFile->locktype>=SHARED_LOCK );
    (void)winUnlockReadLock(pFile);
    res = winLockFile(&pFile->h, CAPDB_LOCKFILE_FLAGS, SHARED_FIRST, 0,
                      SHARED_SIZE, 0);
    if( res ){
      newLocktype = EXCLUSIVE_LOCK;
    }else{
      lastErrno = osGetLastError();
      winGetReadLock(pFile, 0);
    }
  }

  /* If we are holding a PENDING lock that ought to be released, then
  ** release it now.
  */
  if( gotPendingLock && locktype==SHARED_LOCK ){
    winUnlockFile(&pFile->h, PENDING_BYTE, 0, 1, 0);
  }

  /* Update the state of the lock has held in the file descriptor then
  ** return the appropriate result code.
  */
  if( res ){
    rc = CAPDB_OK;
  }else{
    pFile->lastErrno = lastErrno;
    rc = CAPDB_BUSY;
    OSTRACE(("LOCK-FAIL file=%p, wanted=%d, got=%d\n",
             pFile->h, locktype, newLocktype));
  }
  pFile->locktype = (u8)newLocktype;
  OSTRACE(("LOCK file=%p, lock=%d, rc=%s\n",
           pFile->h, pFile->locktype, capdbErrName(rc)));
  return rc;
}

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, return
** non-zero, otherwise zero.
*/
static int winCheckReservedLock(capdb_file *id, int *pResOut){
  int res;
  winFile *pFile = (winFile*)id;

  SimulateIOError( return CAPDB_IOERR_CHECKRESERVEDLOCK; );
  OSTRACE(("TEST-WR-LOCK file=%p, pResOut=%p\n", pFile->h, pResOut));

  assert( id!=0 );
  if( pFile->locktype>=RESERVED_LOCK ){
    res = 1;
    OSTRACE(("TEST-WR-LOCK file=%p, result=%d (local)\n", pFile->h, res));
  }else{
    res = winLockFile(&pFile->h, CAPDB_LOCKFILEEX_FLAGS,RESERVED_BYTE,0,1,0);
    if( res ){
      winUnlockFile(&pFile->h, RESERVED_BYTE, 0, 1, 0);
    }
    res = !res;
    OSTRACE(("TEST-WR-LOCK file=%p, result=%d (remote)\n", pFile->h, res));
  }
  *pResOut = res;
  OSTRACE(("TEST-WR-LOCK file=%p, pResOut=%p, *pResOut=%d, rc=CAPDB_OK\n",
           pFile->h, pResOut, *pResOut));
  return CAPDB_OK;
}

/*
** Lower the locking level on file descriptor id to locktype.  locktype
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
**
** It is not possible for this routine to fail if the second argument
** is NO_LOCK.  If the second argument is SHARED_LOCK then this routine
** might return CAPDB_IOERR;
*/
static int winUnlock(capdb_file *id, int locktype){
  int type;
  winFile *pFile = (winFile*)id;
  int rc = CAPDB_OK;
  assert( pFile!=0 );
  assert( locktype<=SHARED_LOCK );
  OSTRACE(("UNLOCK file=%p, oldLock=%d(%d), newLock=%d\n",
           pFile->h, pFile->locktype, pFile->sharedLockByte, locktype));
  type = pFile->locktype;
  if( type>=EXCLUSIVE_LOCK ){
    winUnlockFile(&pFile->h, SHARED_FIRST, 0, SHARED_SIZE, 0);
    if( locktype==SHARED_LOCK && !winGetReadLock(pFile, 0) ){
      /* This should never happen.  We should always be able to
      ** reacquire the read lock */
      rc = winLogError(CAPDB_IOERR_UNLOCK, osGetLastError(),
                       "winUnlock", pFile->zPath);
    }
  }
  if( type>=RESERVED_LOCK ){
    winUnlockFile(&pFile->h, RESERVED_BYTE, 0, 1, 0);
  }
  if( locktype==NO_LOCK && type>=SHARED_LOCK ){
    winUnlockReadLock(pFile);
  }
  if( type>=PENDING_LOCK ){
    winUnlockFile(&pFile->h, PENDING_BYTE, 0, 1, 0);
  }
  pFile->locktype = (u8)locktype;
  OSTRACE(("UNLOCK file=%p, lock=%d, rc=%s\n",
           pFile->h, pFile->locktype, capdbErrName(rc)));
  return rc;
}

/******************************************************************************
****************************** No-op Locking **********************************
**
** Of the various locking implementations available, this is by far the
** simplest:  locking is ignored.  No attempt is made to lock the database
** file for reading or writing.
**
** This locking mode is appropriate for use on read-only databases
** (ex: databases that are burned into CD-ROM, for example.)  It can
** also be used if the application employs some external mechanism to
** prevent simultaneous access of the same database by two or more
** database connections.  But there is a serious risk of database
** corruption if this locking mode is used in situations where multiple
** database connections are accessing the same database file at the same
** time and one or more of those connections are writing.
*/

static int winNolockLock(capdb_file *id, int locktype){
  UNUSED_PARAMETER(id);
  UNUSED_PARAMETER(locktype);
  return CAPDB_OK;
}

static int winNolockCheckReservedLock(capdb_file *id, int *pResOut){
  UNUSED_PARAMETER(id);
  UNUSED_PARAMETER(pResOut);
  return CAPDB_OK;
}

static int winNolockUnlock(capdb_file *id, int locktype){
  UNUSED_PARAMETER(id);
  UNUSED_PARAMETER(locktype);
  return CAPDB_OK;
}

/******************* End of the no-op lock implementation *********************
******************************************************************************/

/*
** If *pArg is initially negative then this is a query.  Set *pArg to
** 1 or 0 depending on whether or not bit mask of pFile->ctrlFlags is set.
**
** If *pArg is 0 or 1, then clear or set the mask bit of pFile->ctrlFlags.
*/
static void winModeBit(winFile *pFile, unsigned char mask, int *pArg){
  if( *pArg<0 ){
    *pArg = (pFile->ctrlFlags & mask)!=0;
  }else if( (*pArg)==0 ){
    pFile->ctrlFlags &= ~mask;
  }else{
    pFile->ctrlFlags |= mask;
  }
}

/* Forward references to VFS helper methods used for temporary files */
static int winGetTempname(capdb_vfs *, char **);
static int winIsDir(const void *);
static BOOL winIsLongPathPrefix(const char *);
static BOOL winIsDriveLetterAndColon(const char *);

/*
** Control and query of the open file handle.
*/
static int winFileControl(capdb_file *id, int op, void *pArg){
  winFile *pFile = (winFile*)id;
  OSTRACE(("FCNTL file=%p, op=%d, pArg=%p\n", pFile->h, op, pArg));
  switch( op ){
    case CAPDB_FCNTL_LOCKSTATE: {
      *(int*)pArg = pFile->locktype;
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_LAST_ERRNO: {
      *(int*)pArg = (int)pFile->lastErrno;
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_CHUNK_SIZE: {
      pFile->szChunk = *(int *)pArg;
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_SIZE_HINT: {
      if( pFile->szChunk>0 ){
        capdb_int64 oldSz;
        int rc = winFileSize(id, &oldSz);
        if( rc==CAPDB_OK ){
          capdb_int64 newSz = *(capdb_int64*)pArg;
          if( newSz>oldSz ){
            SimulateIOErrorBenign(1);
            rc = winTruncate(id, newSz);
            SimulateIOErrorBenign(0);
          }
        }
        OSTRACE(("FCNTL file=%p, rc=%s\n", pFile->h, capdbErrName(rc)));
        return rc;
      }
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_PERSIST_WAL: {
      winModeBit(pFile, WINFILE_PERSIST_WAL, (int*)pArg);
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_POWERSAFE_OVERWRITE: {
      winModeBit(pFile, WINFILE_PSOW, (int*)pArg);
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_VFSNAME: {
      *(char**)pArg = capdb_mprintf("%s", pFile->pVfs->zName);
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_WIN32_AV_RETRY: {
      int *a = (int*)pArg;
      if( a[0]>0 ){
        winIoerrRetry = a[0];
      }else{
        a[0] = winIoerrRetry;
      }
      if( a[1]>0 ){
        winIoerrRetryDelay = a[1];
      }else{
        a[1] = winIoerrRetryDelay;
      }
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_WIN32_GET_HANDLE: {
      LPHANDLE phFile = (LPHANDLE)pArg;
      *phFile = pFile->h;
      OSTRACE(("FCNTL file=%p, rc=CAPDB_OK\n", pFile->h));
      return CAPDB_OK;
    }
#ifdef CAPDB_TEST
    case CAPDB_FCNTL_WIN32_SET_HANDLE: {
      LPHANDLE phFile = (LPHANDLE)pArg;
      HANDLE hOldFile = pFile->h;
      pFile->h = *phFile;
      *phFile = hOldFile;
      OSTRACE(("FCNTL oldFile=%p, newFile=%p, rc=CAPDB_OK\n",
               hOldFile, pFile->h));
      return CAPDB_OK;
    }
#endif
    case CAPDB_FCNTL_NULL_IO: {
      (void)osCloseHandle(pFile->h);
      pFile->h = NULL;
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_TEMPFILENAME: {
      char *zTFile = 0;
      int rc = winGetTempname(pFile->pVfs, &zTFile);
      if( rc==CAPDB_OK ){
        *(char**)pArg = zTFile;
      }
      OSTRACE(("FCNTL file=%p, rc=%s\n", pFile->h, capdbErrName(rc)));
      return rc;
    }
#if CAPDB_MAX_MMAP_SIZE>0
    case CAPDB_FCNTL_MMAP_SIZE: {
      i64 newLimit = *(i64*)pArg;
      int rc = CAPDB_OK;
      if( newLimit>capdbGlobalConfig.mxMmap ){
        newLimit = capdbGlobalConfig.mxMmap;
      }

      /* The value of newLimit may be eventually cast to (SIZE_T) and passed
      ** to MapViewOfFile(). Restrict its value to 2GB if (SIZE_T) is not at
      ** least a 64-bit type. */
      if( newLimit>0 && sizeof(SIZE_T)<8 ){
        newLimit = (newLimit & 0x7FFFFFFF);
      }

      *(i64*)pArg = pFile->mmapSizeMax;
      if( newLimit>=0 && newLimit!=pFile->mmapSizeMax && pFile->nFetchOut==0 ){
        pFile->mmapSizeMax = newLimit;
        if( pFile->mmapSize>0 ){
          winUnmapfile(pFile);
          rc = winMapfile(pFile, -1);
        }
      }
      OSTRACE(("FCNTL file=%p, rc=%s\n", pFile->h, capdbErrName(rc)));
      return rc;
    }
#endif

#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
    case CAPDB_FCNTL_LOCK_TIMEOUT: {
      int iOld = pFile->iBusyTimeout;
      int iNew = *(int*)pArg;
#if CAPDB_ENABLE_SETLK_TIMEOUT==1
      pFile->iBusyTimeout = (iNew < 0) ? INFINITE : (DWORD)iNew;
#elif CAPDB_ENABLE_SETLK_TIMEOUT==2
      pFile->iBusyTimeout = (DWORD)(!!iNew);
#else
# error "CAPDB_ENABLE_SETLK_TIMEOUT must be set to 1 or 2"
#endif
      *(int*)pArg = iOld;
      return CAPDB_OK;
    }
    case CAPDB_FCNTL_BLOCK_ON_CONNECT: {
      int iNew = *(int*)pArg;
      pFile->bBlockOnConnect = iNew;
      return CAPDB_OK;
    }
#endif /* CAPDB_ENABLE_SETLK_TIMEOUT */

#if defined(CAPDB_DEBUG) || defined(CAPDB_ENABLE_FILESTAT)
    case CAPDB_FCNTL_FILESTAT: {
      capdb_str *pStr = (capdb_str*)pArg;
      capdb_str_appendf(pStr, "{\"h\":%llu", (capdb_uint64)pFile->h);
      capdb_str_appendf(pStr, ",\"vfs\":\"%s\"", pFile->pVfs->zName);
      if( pFile->locktype ){
        static const char *azLock[] = { "SHARED", "RESERVED",
                                      "PENDING", "EXCLUSIVE" };
        capdb_str_appendf(pStr, ",\"locktype\":\"%s\"", 
                                  azLock[pFile->locktype-1]);
      }
#if CAPDB_MAX_MMAP_SIZE>0
      if( pFile->mmapSize ){
        capdb_str_appendf(pStr, ",\"mmapSize\":%lld", pFile->mmapSize);
        capdb_str_appendf(pStr, ",\"nFetchOut\":%d", pFile->nFetchOut);
      }
#endif
      capdb_str_append(pStr, "}", 1);
      return CAPDB_OK;
    }
#endif /* CAPDB_DEBUG || CAPDB_ENABLE_FILESTAT */

  }
  OSTRACE(("FCNTL file=%p, rc=CAPDB_NOTFOUND\n", pFile->h));
  return CAPDB_NOTFOUND;
}

/*
** Return the sector size in bytes of the underlying block device for
** the specified file. This is almost always 512 bytes, but may be
** larger for some devices.
**
** SQLite code assumes this function cannot fail. It also assumes that
** if two files are created in the same file-system directory (i.e.
** a database and its journal file) that the sector size will be the
** same for both.
*/
static int winSectorSize(capdb_file *id){
  (void)id;
  return CAPDB_DEFAULT_SECTOR_SIZE;
}

/*
** Return a vector of device characteristics.
*/
static int winDeviceCharacteristics(capdb_file *id){
  winFile *p = (winFile*)id;
  return CAPDB_IOCAP_UNDELETABLE_WHEN_OPEN | CAPDB_IOCAP_SUBPAGE_READ |
         ((p->ctrlFlags & WINFILE_PSOW)?CAPDB_IOCAP_POWERSAFE_OVERWRITE:0);
}

/*
** Windows will only let you create file view mappings
** on allocation size granularity boundaries.
** During capdb_os_init() we do a GetSystemInfo()
** to get the granularity size.
*/
static SYSTEM_INFO winSysInfo;

/*
** Convert a UTF-8 filename into whatever form the underlying
** operating system wants filenames in.  Space to hold the result
** is obtained from malloc and must be freed by the calling
** function
**
** On Cygwin, 3 possible input forms are accepted:
** - If the filename starts with "<drive>:/" or "<drive>:\",
**   it is converted to UTF-16 as-is.
** - If the filename contains '/', it is assumed to be a
**   Cygwin absolute path, it is converted to a win32
**   absolute path in UTF-16.
** - Otherwise it must be a filename only, the win32 filename
**   is returned in UTF-16.
** Note: If the function cygwin_conv_path() fails, only
**   UTF-8 -> UTF-16 conversion will be done. This can only
**   happen when the file path >32k, in which case winUtf8ToUnicode()
**   will fail too.
*/
static void *winConvertFromUtf8Filename(const char *zFilename){
  void *zConverted = 0;
#ifdef __CYGWIN__
  int nChar;
  LPWSTR zWideFilename;

  if( osCygwin_conv_path && !(winIsDriveLetterAndColon(zFilename)
      && winIsDirSep(zFilename[2])) ){
    i64 nByte;
    int convertflag = CCP_POSIX_TO_WIN_W;
    if( !strchr(zFilename, '/') ) convertflag |= CCP_RELATIVE;
    nByte = (i64)osCygwin_conv_path(convertflag,
        zFilename, 0, 0);
    if( nByte>0 ){
      zConverted = capdbMallocZero(12+(u64)nByte);
      if ( zConverted==0 ){
        return zConverted;
      }
      zWideFilename = zConverted;
      /* Filenames should be prefixed, except when converted
       * full path already starts with "\\?\". */
      if( osCygwin_conv_path(convertflag, zFilename,
                           zWideFilename+4, nByte)==0 ){
        if( (convertflag&CCP_RELATIVE) ){
          memmove(zWideFilename, zWideFilename+4, nByte);
        }else if( memcmp(zWideFilename+4, L"\\\\", 4) ){
          memcpy(zWideFilename, L"\\\\?\\", 8);
        }else if( zWideFilename[6]!='?' ){
          memmove(zWideFilename+6, zWideFilename+4, nByte);
          memcpy(zWideFilename, L"\\\\?\\UNC", 14);
        }else{
          memmove(zWideFilename, zWideFilename+4, nByte);
        }
        return zConverted;
      }
      capdb_free(zConverted);
    }
  }
  nChar = osMultiByteToWideChar(CP_UTF8, 0, zFilename, -1, NULL, 0);
  if( nChar==0 ){
    return 0;
  }
  zWideFilename = capdbMallocZero( nChar*sizeof(WCHAR)+12 );
  if( zWideFilename==0 ){
    return 0;
  }
  nChar = osMultiByteToWideChar(CP_UTF8, 0, zFilename, -1,
                                zWideFilename, nChar);
  if( nChar==0 ){
    capdb_free(zWideFilename);
    zWideFilename = 0;
  }else if( nChar>MAX_PATH
      && winIsDriveLetterAndColon(zFilename)
      && winIsDirSep(zFilename[2]) ){
    memmove(zWideFilename+4, zWideFilename, nChar*sizeof(WCHAR));
    zWideFilename[2] = '\\';
    memcpy(zWideFilename, L"\\\\?\\", 8);
  }else if( nChar>MAX_PATH
      && winIsDirSep(zFilename[0]) && winIsDirSep(zFilename[1])
      && zFilename[2] != '?' ){
    memmove(zWideFilename+6, zWideFilename, nChar*sizeof(WCHAR));
    memcpy(zWideFilename, L"\\\\?\\UNC", 14);
  }
  zConverted = zWideFilename;
#else /* if !defined(__CYGWIN__) */
  zConverted = winUtf8ToUnicode(zFilename);
#endif /* __CYGWIN__ */
  /* caller will handle out of memory */
  return zConverted;
}

#ifndef CAPDB_OMIT_WAL

/*
** Helper functions to obtain and relinquish the global mutex. The
** global mutex is used to protect the winLockInfo objects used by
** this file, all of which may be shared by multiple threads.
**
** Function winShmMutexHeld() is used to assert() that the global mutex
** is held when required. This function is only used as part of assert()
** statements. e.g.
**
**   winShmEnterMutex()
**     assert( winShmMutexHeld() );
**   winShmLeaveMutex()
*/
static capdb_mutex *winBigLock = 0;
static void winShmEnterMutex(void){
  capdb_mutex_enter(winBigLock);
}
static void winShmLeaveMutex(void){
  capdb_mutex_leave(winBigLock);
}
#ifndef NDEBUG
static int winShmMutexHeld(void) {
  return capdb_mutex_held(winBigLock);
}
#endif

/*
** Object used to represent a single file opened and mmapped to provide
** shared memory.  When multiple threads all reference the same
** log-summary, each thread has its own winFile object, but they all
** point to a single instance of this object.  In other words, each
** log-summary is opened only once per process.
**
** winShmMutexHeld() must be true when creating or destroying
** this object, or while editing the global linked list that starts
** at winShmNodeList.
**
** When reading or writing the linked list starting at winShmNode.pWinShmList,
** pShmNode->mutex must be held.
**
** The following fields are constant after the object is created:
**
**      zFilename
**      hSharedShm
**      mutex
**      bUseSharedLockHandle
**
** Either winShmNode.mutex must be held or winShmNode.pWinShmList==0 and
** winShmMutexHeld() is true when reading or writing any other field
** in this structure.
**
** File-handle hSharedShm is always used to (a) take the DMS lock, (b) 
** truncate the *-shm file if the DMS-locking protocol demands it, and 
** (c) map regions of the *-shm file into memory using MapViewOfFile() 
** or similar. If bUseSharedLockHandle is true, then other locks are also 
** taken on hSharedShm. Or, if bUseSharedLockHandle is false, then other 
** locks are taken using each connection's winShm.hShm handles.
*/
struct winShmNode {
  capdb_mutex *mutex;      /* Mutex to access this object */
  char *zFilename;           /* Name of the file */
  HANDLE hSharedShm;         /* File handle open on zFilename */
  int bUseSharedLockHandle;  /* True to use hSharedShm for everything */

  int isUnlocked;            /* DMS lock has not yet been obtained */
  int isReadonly;            /* True if read-only */
  int szRegion;              /* Size of shared-memory regions */
  int nRegion;               /* Size of array apRegion */

  struct ShmRegion {
    HANDLE hMap;             /* File handle from CreateFileMapping */
    void *pMap;
  } *aRegion;
  DWORD lastErrno;           /* The Windows errno from the last I/O error */

  winShm *pWinShmList;       /* List of winShm objects with ptrs to this */

  winShmNode *pNext;         /* Next in list of all winShmNode objects */
#if defined(CAPDB_DEBUG) || defined(CAPDB_HAVE_OS_TRACE)
  u8 nextShmId;              /* Next available winShm.id value */
#endif
};

/*
** A global array of all winShmNode objects.
**
** The winShmMutexHeld() must be true while reading or writing this list.
*/
static winShmNode *winShmNodeList = 0;

/*
** Structure used internally by this VFS to record the state of an
** open shared memory connection. There is one such structure for each
** winFile open on a wal mode database.
*/
struct winShm {
  winShmNode *pShmNode;      /* The underlying winShmNode object */
  u16 sharedMask;            /* Mask of shared locks held */
  u16 exclMask;              /* Mask of exclusive locks held */
  HANDLE hShm;               /* File-handle on *-shm file. For locking. */
  int bReadonly;             /* True if hShm is opened read-only */
#if defined(CAPDB_DEBUG) || defined(CAPDB_HAVE_OS_TRACE)
  u8 id;                     /* Id of this connection with its winShmNode */
#endif
  winShm *pWinShmNext;       /* Next winShm object on same winShmNode */
};

/*
** Constants used for locking
*/
#define WIN_SHM_BASE   ((22+CAPDB_SHM_NLOCK)*4)        /* first lock byte */
#define WIN_SHM_DMS    (WIN_SHM_BASE+CAPDB_SHM_NLOCK)  /* deadman switch */

/* Forward references to VFS methods */
static int winOpen(capdb_vfs*,const char*,capdb_file*,int,int*);
static int winDelete(capdb_vfs *,const char*,int);

/*
** Purge the winShmNodeList list of all entries with winShmNode.pWinShmList==0.
**
** This is not a VFS shared-memory method; it is a utility function called
** by VFS shared-memory methods.
*/
static void winShmPurge(capdb_vfs *pVfs, int deleteFlag){
  winShmNode **pp;
  winShmNode *p;
  assert( winShmMutexHeld() );
  OSTRACE(("SHM-PURGE pid=%lu, deleteFlag=%d\n",
           osGetCurrentProcessId(), deleteFlag));
  pp = &winShmNodeList;
  while( (p = *pp)!=0 ){
    if( p->pWinShmList==0 ){
      int i;
      if( p->mutex ){ capdb_mutex_free(p->mutex); }
      for(i=0; i<p->nRegion; i++){
        BOOL bRc = osUnmapViewOfFile(p->aRegion[i].pMap);
        OSTRACE(("SHM-PURGE-UNMAP pid=%lu, region=%d, rc=%s\n",
                 osGetCurrentProcessId(), i, bRc ? "ok" : "failed"));
        UNUSED_VARIABLE_VALUE(bRc);
        bRc = osCloseHandle(p->aRegion[i].hMap);
        OSTRACE(("SHM-PURGE-CLOSE pid=%lu, region=%d, rc=%s\n",
                 osGetCurrentProcessId(), i, bRc ? "ok" : "failed"));
        UNUSED_VARIABLE_VALUE(bRc);
      }
      winHandleClose(p->hSharedShm);
      if( deleteFlag ){
        SimulateIOErrorBenign(1);
        capdbBeginBenignMalloc();
        winDelete(pVfs, p->zFilename, 0);
        capdbEndBenignMalloc();
        SimulateIOErrorBenign(0);
      }
      *pp = p->pNext;
      capdb_free(p->aRegion);
      capdb_free(p);
    }else{
      pp = &p->pNext;
    }
  }
}

/*
** The DMS lock has not yet been taken on the shm file associated with
** pShmNode. Take the lock. Truncate the *-shm file if required.
** Return CAPDB_OK if successful, or an SQLite error code otherwise.
*/
static int winLockSharedMemory(winShmNode *pShmNode, DWORD nMs){
  HANDLE h = pShmNode->hSharedShm;
  int rc = CAPDB_OK;

  assert( capdb_mutex_held(pShmNode->mutex) );
  rc = winHandleLockTimeout(h, WIN_SHM_DMS, 1, 1, 0);
  if( rc==CAPDB_OK ){
    /* We have an EXCLUSIVE lock on the DMS byte. This means that this
    ** is the first process to open the file. Truncate it to zero bytes
    ** in this case.  */
    if( pShmNode->isReadonly ){
      rc = CAPDB_READONLY_CANTINIT;
    }else{
      rc = winHandleTruncate(h, 0);
    }

    /* Release the EXCLUSIVE lock acquired above. */
    winUnlockFile(&h, WIN_SHM_DMS, 0, 1, 0);
  }else if( (rc & 0xFF)==CAPDB_BUSY ){
    rc = CAPDB_OK;
  }

  if( rc==CAPDB_OK ){
    /* Take a SHARED lock on the DMS byte. */
    rc = winHandleLockTimeout(h, WIN_SHM_DMS, 1, 0, nMs);
    if( rc==CAPDB_OK ){
      pShmNode->isUnlocked = 0;
    }
  }

  return rc;
}


/*
** This function is used to open a handle on a *-shm file.
**
** If CAPDB_ENABLE_SETLK_TIMEOUT is defined at build time, then the file
** is opened with FILE_FLAG_OVERLAPPED specified. If not, it is not.
*/
static int winHandleOpen(
  const char *zUtf8,              /* File to open */
  int *pbReadonly,                /* IN/OUT: True for readonly handle */
  HANDLE *ph                      /* OUT: New HANDLE for file */
){
  int rc = CAPDB_OK;
  void *zConverted = 0;
  int bReadonly = *pbReadonly;
  HANDLE h = INVALID_HANDLE_VALUE;

#ifdef CAPDB_ENABLE_SETLK_TIMEOUT
  const DWORD flag_overlapped = FILE_FLAG_OVERLAPPED;
#else
  const DWORD flag_overlapped = 0;
#endif

  /* Convert the filename to the system encoding. */
  zConverted = winConvertFromUtf8Filename(zUtf8);
  if( zConverted==0 ){
    OSTRACE(("OPEN name=%s, rc=CAPDB_IOERR_NOMEM", zUtf8));
    rc = CAPDB_IOERR_NOMEM_BKPT;
    goto winopenfile_out;
  }

  /* Ensure the file we are trying to open is not actually a directory. */
  if( winIsDir(zConverted) ){
    OSTRACE(("OPEN name=%s, rc=CAPDB_CANTOPEN_ISDIR", zUtf8));
    rc = CAPDB_CANTOPEN_ISDIR;
    goto winopenfile_out;
  }

  /* TODO: platforms.
  ** TODO: retry-on-ioerr.
  */
  h = osCreateFileW((LPCWSTR)zConverted,         /* lpFileName */
      (GENERIC_READ | (bReadonly ? 0 : GENERIC_WRITE)),  /* dwDesiredAccess */
      FILE_SHARE_READ | FILE_SHARE_WRITE,        /* dwShareMode */
      NULL,                                      /* lpSecurityAttributes */
      OPEN_ALWAYS,                               /* dwCreationDisposition */
      FILE_ATTRIBUTE_NORMAL|flag_overlapped,
      NULL
  );

  if( h==INVALID_HANDLE_VALUE ){
    if( bReadonly==0 ){
      bReadonly = 1;
      rc = winHandleOpen(zUtf8, &bReadonly, &h);
    }else{
      rc = CAPDB_CANTOPEN_BKPT;
    }
  }

 winopenfile_out:
  capdb_free(zConverted);
  *pbReadonly = bReadonly;
  *ph = h;
  return rc;
}
 
/*
** Close pDbFd's connection to shared-memory.  Delete the underlying
** *-shm file if deleteFlag is true.
*/
static int winCloseSharedMemory(winFile *pDbFd, int deleteFlag){
  winShm *p;            /* The connection to be closed */
  winShm **pp;          /* Iterator for pShmNode->pWinShmList */
  winShmNode *pShmNode; /* The underlying shared-memory file */

  p = pDbFd->pShm;
  if( p==0 ) return CAPDB_OK;
  if( p->hShm!=INVALID_HANDLE_VALUE ){
    osCloseHandle(p->hShm);
  }

  winShmEnterMutex();
  pShmNode = p->pShmNode;

  /* Remove this connection from the winShmNode.pWinShmList list */
  capdb_mutex_enter(pShmNode->mutex);
  for(pp=&pShmNode->pWinShmList; *pp!=p; pp=&(*pp)->pWinShmNext){}
  *pp = p->pWinShmNext;
  capdb_mutex_leave(pShmNode->mutex);

  winShmPurge(pDbFd->pVfs, deleteFlag);
  winShmLeaveMutex();

  /* Free the connection p */
  capdb_free(p);
  pDbFd->pShm = 0;
  return CAPDB_OK;
}

/*
** testfixture builds may set this global variable to true via a
** Tcl interface. This forces the VFS to use the locking normally
** only used for UNC paths for all files.
*/
#ifdef CAPDB_TEST
int capdb_win_test_unc_locking = 0;
#else
# define capdb_win_test_unc_locking 0
#endif

/*
** Return true if the string passed as the only argument is likely
** to be a UNC path. In other words, if it starts with "\\".
*/
static int winIsUNCPath(const char *zFile){
  if( zFile[0]=='\\' && zFile[1]=='\\' ){
    return 1;
  }
  return capdb_win_test_unc_locking;
}

/*
** Open the shared-memory area associated with database file pDbFd.
*/
static int winOpenSharedMemory(winFile *pDbFd){
  struct winShm *p;                  /* The connection to be opened */
  winShmNode *pShmNode = 0;          /* The underlying mmapped file */
  int rc = CAPDB_OK;                /* Result code */
  winShmNode *pNew;                  /* Newly allocated winShmNode */
  int nName;                         /* Size of zName in bytes */

  assert( pDbFd->pShm==0 );    /* Not previously opened */

  /* Allocate space for the new capdb_shm object.  Also speculatively
  ** allocate space for a new winShmNode and filename.  */
  p = capdbMallocZero( sizeof(*p) );
  if( p==0 ) return CAPDB_IOERR_NOMEM_BKPT;
  nName = capdbStrlen30(pDbFd->zPath);
  pNew = capdbMallocZero( sizeof(*pShmNode) + (i64)nName + 17 );
  if( pNew==0 ){
    capdb_free(p);
    return CAPDB_IOERR_NOMEM_BKPT;
  }
  pNew->zFilename = (char*)&pNew[1];
  pNew->hSharedShm = INVALID_HANDLE_VALUE;
  pNew->isUnlocked = 1;
  pNew->bUseSharedLockHandle = winIsUNCPath(pDbFd->zPath);
  capdb_snprintf(nName+15, pNew->zFilename, "%s-shm", pDbFd->zPath);
  capdbFileSuffix3(pDbFd->zPath, pNew->zFilename);

  /* Look to see if there is an existing winShmNode that can be used.
  ** If no matching winShmNode currently exists, then create a new one.  */
  winShmEnterMutex();
  for(pShmNode = winShmNodeList; pShmNode; pShmNode=pShmNode->pNext){
    /* TBD need to come up with better match here.  Perhaps
    ** use FILE_ID_BOTH_DIR_INFO Structure.  */
    if( capdbStrICmp(pShmNode->zFilename, pNew->zFilename)==0 ) break;
  }
  if( pShmNode==0 ){
    pShmNode = pNew;

    /* Allocate a mutex for this winShmNode object, if one is required. */
    if( capdbGlobalConfig.bCoreMutex ){
      pShmNode->mutex = capdb_mutex_alloc(CAPDB_MUTEX_FAST);
      if( pShmNode->mutex==0 ) rc = CAPDB_IOERR_NOMEM_BKPT;
    }

    /* Open a file-handle to use for mappings, and for the DMS lock. */
    if( rc==CAPDB_OK ){
      HANDLE h = INVALID_HANDLE_VALUE;
      pShmNode->isReadonly = capdb_uri_boolean(pDbFd->zPath,"readonly_shm",0);
      rc = winHandleOpen(pNew->zFilename, &pShmNode->isReadonly, &h);
      pShmNode->hSharedShm = h;
    }

    /* If successful, link the new winShmNode into the global list. If an 
    ** error occurred, free the object. */
    if( rc==CAPDB_OK ){
      pShmNode->pNext = winShmNodeList;
      winShmNodeList = pShmNode;
      pNew = 0;
    }else{
      capdb_mutex_free(pShmNode->mutex);
      if( pShmNode->hSharedShm!=INVALID_HANDLE_VALUE ){
        osCloseHandle(pShmNode->hSharedShm);
      }
    }
  }

  /* If no error has occurred, link the winShm object to the winShmNode and
  ** the winShm to pDbFd.  */
  if( rc==CAPDB_OK ){
    capdb_mutex_enter(pShmNode->mutex);
    p->pShmNode = pShmNode;
    p->pWinShmNext = pShmNode->pWinShmList;
    pShmNode->pWinShmList = p;
#if defined(CAPDB_DEBUG) || defined(CAPDB_HAVE_OS_TRACE)
    p->id = pShmNode->nextShmId++;
#endif
    pDbFd->pShm = p;
    capdb_mutex_leave(pShmNode->mutex);
  }else if( p ){
    capdb_free(p);
  }

  assert( rc!=CAPDB_OK || pShmNode->isUnlocked==0 || pShmNode->nRegion==0 );
  winShmLeaveMutex();
  capdb_free(pNew);

  /* Open a file-handle on the *-shm file for this connection. This file-handle
  ** is only used for locking. The mapping of the *-shm file is created using
  ** the shared file handle in winShmNode.hSharedShm.  */
  if( rc==CAPDB_OK && pShmNode->bUseSharedLockHandle==0 ){
    p->bReadonly = capdb_uri_boolean(pDbFd->zPath, "readonly_shm", 0);
    rc = winHandleOpen(pShmNode->zFilename, &p->bReadonly, &p->hShm);
    if( rc!=CAPDB_OK ){
      assert( p->hShm==INVALID_HANDLE_VALUE );
      winCloseSharedMemory(pDbFd, 0);
    }
  }

  return rc;
}

/*
** Close a connection to shared-memory.  Delete the underlying
** storage if deleteFlag is true.
*/
static int winShmUnmap(
  capdb_file *fd,          /* Database holding shared memory */
  int deleteFlag             /* Delete after closing if true */
){
  return winCloseSharedMemory((winFile*)fd, deleteFlag);
}

/*
** Change the lock state for a shared-memory segment.
*/
static int winShmLock(
  capdb_file *fd,          /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  winFile *pDbFd = (winFile*)fd;        /* Connection holding shared memory */
  winShm *p = pDbFd->pShm;              /* The shared memory being locked */
  winShmNode *pShmNode;
  int rc = CAPDB_OK;                   /* Result code */
  u16 mask = (u16)((1U<<(ofst+n)) - (1U<<ofst)); /* Mask of locks to [un]take */

  if( p==0 ) return CAPDB_IOERR_SHMLOCK;
  pShmNode = p->pShmNode;
  if( NEVER(pShmNode==0) ) return CAPDB_IOERR_SHMLOCK;

  assert( ofst>=0 && ofst+n<=CAPDB_SHM_NLOCK );
  assert( n>=1 );
  assert( flags==(CAPDB_SHM_LOCK | CAPDB_SHM_SHARED)
       || flags==(CAPDB_SHM_LOCK | CAPDB_SHM_EXCLUSIVE)
       || flags==(CAPDB_SHM_UNLOCK | CAPDB_SHM_SHARED)
       || flags==(CAPDB_SHM_UNLOCK | CAPDB_SHM_EXCLUSIVE) );
  assert( n==1 || (flags & CAPDB_SHM_EXCLUSIVE)!=0 );

  /* Check that, if this to be a blocking lock, no locks that occur later
  ** in the following list than the lock being obtained are already held:
  **
  **   1. Recovery lock (ofst==2).
  **   2. Checkpointer lock (ofst==1).
  **   3. Write lock (ofst==0).
  **   4. Read locks (ofst>=3 && ofst<CAPDB_SHM_NLOCK).
  **
  ** In other words, if this is a blocking lock, none of the locks that
  ** occur later in the above list than the lock being obtained may be
  ** held.
  */
#if defined(CAPDB_ENABLE_SETLK_TIMEOUT) && defined(CAPDB_DEBUG)
  {
    u16 lockMask = (p->exclMask|p->sharedMask);
    assert( (flags & CAPDB_SHM_UNLOCK) || pDbFd->iBusyTimeout==0 || (
          (ofst!=2 || lockMask==0)
       && (ofst!=1 || lockMask==0 || lockMask==2)
       && (ofst!=0 || lockMask<3)
       && (ofst<3  || lockMask<(1<<ofst))
    ));
  }
#endif

  /* Check if there is any work to do. There are three cases:
  **
  **    a) An unlock operation where there are locks to unlock,
  **    b) An shared lock where the requested lock is not already held
  **    c) An exclusive lock where the requested lock is not already held
  **
  ** The SQLite core never requests an exclusive lock that it already holds.
  ** This is assert()ed immediately below.  */
  assert( flags!=(CAPDB_SHM_EXCLUSIVE|CAPDB_SHM_LOCK) 
       || 0==(p->exclMask & mask)
  );
  if( ((flags & CAPDB_SHM_UNLOCK) && ((p->exclMask|p->sharedMask) & mask))
   || (flags==(CAPDB_SHM_SHARED|CAPDB_SHM_LOCK) && 0==(p->sharedMask & mask))
   || (flags==(CAPDB_SHM_EXCLUSIVE|CAPDB_SHM_LOCK))
  ){
    HANDLE h = p->hShm;

    if( flags & CAPDB_SHM_UNLOCK ){
      /* Case (a) - unlock.  */

      assert( (p->exclMask & p->sharedMask)==0 );
      assert( !(flags & CAPDB_SHM_EXCLUSIVE) || (p->exclMask & mask)==mask );
      assert( !(flags & CAPDB_SHM_SHARED) || (p->sharedMask & mask)==mask );

      assert( !(flags & CAPDB_SHM_SHARED) || n==1 );
      if( pShmNode->bUseSharedLockHandle ){
        h = pShmNode->hSharedShm;
        if( flags & CAPDB_SHM_SHARED ){
          winShm *pShm;
          capdb_mutex_enter(pShmNode->mutex);
          for(pShm=pShmNode->pWinShmList; pShm; pShm=pShm->pWinShmNext){
            if( pShm!=p && (pShm->sharedMask & mask) ){
              /* Another connection within this process is also holding this
              ** SHARED lock. So do not actually release the OS lock.  */
              h = INVALID_HANDLE_VALUE;
              break;
            }
          }
          capdb_mutex_leave(pShmNode->mutex);
        }
      }

      if( h!=INVALID_HANDLE_VALUE ){
        rc = winHandleUnlock(h, ofst+WIN_SHM_BASE, n);
      }

      /* If successful, also clear the bits in sharedMask/exclMask */
      if( rc==CAPDB_OK ){
        p->exclMask = (p->exclMask & ~mask);
        p->sharedMask = (p->sharedMask & ~mask);
      }
    }else{
      int bExcl = ((flags & CAPDB_SHM_EXCLUSIVE) ? 1 : 0);
      DWORD nMs = winFileBusyTimeout(pDbFd);

      if( pShmNode->bUseSharedLockHandle ){
        winShm *pShm;
        h = pShmNode->hSharedShm;
        capdb_mutex_enter(pShmNode->mutex);
        for(pShm=pShmNode->pWinShmList; pShm; pShm=pShm->pWinShmNext){
          if( bExcl ){
            if( (pShm->sharedMask|pShm->exclMask) & mask ){
              rc = CAPDB_BUSY;
              h = INVALID_HANDLE_VALUE;
            }
          }else{
            if( pShm->sharedMask & mask ){
              h = INVALID_HANDLE_VALUE;
            }else if( pShm->exclMask & mask ){
              rc = CAPDB_BUSY;
              h = INVALID_HANDLE_VALUE;
            }
          }
        }
        capdb_mutex_leave(pShmNode->mutex);
      }

      if( h!=INVALID_HANDLE_VALUE ){
        rc = winHandleLockTimeout(h, ofst+WIN_SHM_BASE, n, bExcl, nMs);
      }
      if( rc==CAPDB_OK ){
        if( bExcl ){
          p->exclMask = (p->exclMask | mask);
        }else{
          p->sharedMask = (p->sharedMask | mask);
        }
      }
    }
  }

  OSTRACE((
      "SHM-LOCK(%d,%d,%d) pid=%lu, id=%d, sharedMask=%03x, exclMask=%03x,"
      " rc=%s\n",
      ofst, n, flags, 
      osGetCurrentProcessId(), p->id, p->sharedMask, p->exclMask,
      capdbErrName(rc))
  );
  return rc;
}

/*
** Implement a memory barrier or memory fence on shared memory.
**
** All loads and stores begun before the barrier must complete before
** any load or store begun after the barrier.
*/
static void winShmBarrier(
  capdb_file *fd          /* Database holding the shared memory */
){
  UNUSED_PARAMETER(fd);
  capdbMemoryBarrier();   /* compiler-defined memory barrier */
  winShmEnterMutex();       /* Also mutex, for redundancy */
  winShmLeaveMutex();
}

/*
** This function is called to obtain a pointer to region iRegion of the
** shared-memory associated with the database file fd. Shared-memory regions
** are numbered starting from zero. Each shared-memory region is szRegion
** bytes in size.
**
** If an error occurs, an error code is returned and *pp is set to NULL.
**
** Otherwise, if the isWrite parameter is 0 and the requested shared-memory
** region has not been allocated (by any client, including one running in a
** separate process), then *pp is set to NULL and CAPDB_OK returned. If
** isWrite is non-zero and the requested shared-memory region has not yet
** been allocated, it is allocated by this function.
**
** If the shared-memory region has already been allocated or is allocated by
** this call as described above, then it is mapped into this processes
** address space (if it is not already), *pp is set to point to the mapped
** memory and CAPDB_OK returned.
*/
static int winShmMap(
  capdb_file *fd,               /* Handle open on database file */
  int iRegion,                    /* Region to retrieve */
  int szRegion,                   /* Size of regions */
  int isWrite,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  winFile *pDbFd = (winFile*)fd;
  winShm *pShm = pDbFd->pShm;
  winShmNode *pShmNode;
  DWORD protect = PAGE_READWRITE;
  DWORD flags = FILE_MAP_WRITE | FILE_MAP_READ;
  int rc = CAPDB_OK;

  if( !pShm ){
    rc = winOpenSharedMemory(pDbFd);
    if( rc!=CAPDB_OK ) return rc;
    pShm = pDbFd->pShm;
    assert( pShm!=0 );
  }
  pShmNode = pShm->pShmNode;

  capdb_mutex_enter(pShmNode->mutex);
  if( pShmNode->isUnlocked ){
    /* Take the DMS lock. */
    assert( pShmNode->nRegion==0 );
    rc = winLockSharedMemory(pShmNode, winFileBusyTimeout(pDbFd));
    if( rc!=CAPDB_OK ) goto shmpage_out;
  }

  assert( szRegion==pShmNode->szRegion || pShmNode->nRegion==0 );
  if( pShmNode->nRegion<=iRegion ){
    HANDLE hShared = pShmNode->hSharedShm;
    struct ShmRegion *apNew;           /* New aRegion[] array */
    i64 nByte = ((i64)iRegion+1)*(i64)szRegion;  /* Minimum file size */
    capdb_int64 sz;                  /* Current size of wal-index file */

    pShmNode->szRegion = szRegion;

    /* The requested region is not mapped into this processes address space.
    ** Check to see if it has been allocated (i.e. if the wal-index file is
    ** large enough to contain the requested region).
    */
    rc = winHandleSize(hShared, &sz);
    if( rc!=CAPDB_OK ){
      rc = winLogError(rc, osGetLastError(), "winShmMap1", pDbFd->zPath);
      goto shmpage_out;
    }

    if( sz<nByte ){
      /* The requested memory region does not exist. If isWrite is set to
      ** zero, exit early. *pp will be set to NULL and CAPDB_OK returned.
      **
      ** Alternatively, if isWrite is non-zero, use ftruncate() to allocate
      ** the requested memory region.  */
      if( !isWrite ) goto shmpage_out;
      rc = winHandleTruncate(hShared, nByte);
      if( rc!=CAPDB_OK ){
        rc = winLogError(rc, osGetLastError(), "winShmMap2", pDbFd->zPath);
        goto shmpage_out;
      }
    }

    /* Map the requested memory region into this processes address space. */
    apNew = (struct ShmRegion*)capdb_realloc64(
        pShmNode->aRegion, ((i64)iRegion+1)*sizeof(apNew[0])
    );
    if( !apNew ){
      rc = CAPDB_IOERR_NOMEM_BKPT;
      goto shmpage_out;
    }
    pShmNode->aRegion = apNew;

    if( pShmNode->isReadonly ){
      protect = PAGE_READONLY;
      flags = FILE_MAP_READ;
    }

    while( pShmNode->nRegion<=iRegion ){
      HANDLE hMap = NULL;         /* file-mapping handle */
      void *pMap = 0;             /* Mapped memory region */

#ifdef CAPDB_UWP
      hMap = osCreateFileMappingFromApp(hShared, NULL, protect, nByte, NULL);
#else
      hMap = osCreateFileMappingW(hShared, NULL, protect, 0, nByte, NULL);
#endif
      OSTRACE(("SHM-MAP-CREATE pid=%lu, region=%d, size=%lld, rc=%s\n",
               osGetCurrentProcessId(), pShmNode->nRegion, nByte,
               hMap ? "ok" : "failed"));
      if( hMap ){
        i64 iOffset = pShmNode->nRegion*szRegion;
        int iOffsetShift = iOffset % winSysInfo.dwAllocationGranularity;
#ifdef CAPDB_UWP
        pMap = osMapViewOfFileFromApp(hMap, flags,
            iOffset - iOffsetShift, (i64)szRegion + iOffsetShift
        );
#else
        pMap = osMapViewOfFile(hMap, flags,
            0, iOffset - iOffsetShift, (i64)szRegion + iOffsetShift
        );
#endif
        OSTRACE(("SHM-MAP-MAP pid=%lu, region=%d, offset=%d, size=%d, rc=%s\n",
                 osGetCurrentProcessId(), pShmNode->nRegion, iOffset,
                 szRegion, pMap ? "ok" : "failed"));
      }
      if( !pMap ){
        pShmNode->lastErrno = osGetLastError();
        rc = winLogError(CAPDB_IOERR_SHMMAP, pShmNode->lastErrno,
                         "winShmMap3", pDbFd->zPath);
        if( hMap ) osCloseHandle(hMap);
        goto shmpage_out;
      }

      pShmNode->aRegion[pShmNode->nRegion].pMap = pMap;
      pShmNode->aRegion[pShmNode->nRegion].hMap = hMap;
      pShmNode->nRegion++;
    }
  }

shmpage_out:
  if( pShmNode->nRegion>iRegion ){
    i64 iOffset = (i64)iRegion*(i64)szRegion;
    int iOffsetShift = iOffset % winSysInfo.dwAllocationGranularity;
    char *p = (char *)pShmNode->aRegion[iRegion].pMap;
    *pp = (void *)&p[iOffsetShift];
  }else{
    *pp = 0;
  }
  if( pShmNode->isReadonly && rc==CAPDB_OK ){
    rc = CAPDB_READONLY;
  }
  capdb_mutex_leave(pShmNode->mutex);
  return rc;
}

#else
# define winShmMap     0
# define winShmLock    0
# define winShmBarrier 0
# define winShmUnmap   0
#endif /* #ifndef CAPDB_OMIT_WAL */

/*
** Cleans up the mapped region of the specified file, if any.
*/
#if CAPDB_MAX_MMAP_SIZE>0
static int winUnmapfile(winFile *pFile){
  assert( pFile!=0 );
  OSTRACE(("UNMAP-FILE pid=%lu, pFile=%p, hMap=%p, pMapRegion=%p, "
           "mmapSize=%lld, mmapSizeMax=%lld\n",
           osGetCurrentProcessId(), pFile, pFile->hMap, pFile->pMapRegion,
           pFile->mmapSize, pFile->mmapSizeMax));
  if( pFile->pMapRegion ){
    if( !osUnmapViewOfFile(pFile->pMapRegion) ){
      pFile->lastErrno = osGetLastError();
      OSTRACE(("UNMAP-FILE pid=%lu, pFile=%p, pMapRegion=%p, "
               "rc=CAPDB_IOERR_MMAP\n", osGetCurrentProcessId(), pFile,
               pFile->pMapRegion));
      return winLogError(CAPDB_IOERR_MMAP, pFile->lastErrno,
                         "winUnmapfile1", pFile->zPath);
    }
    pFile->pMapRegion = 0;
    pFile->mmapSize = 0;
  }
  if( pFile->hMap!=NULL ){
    if( !osCloseHandle(pFile->hMap) ){
      pFile->lastErrno = osGetLastError();
      OSTRACE(("UNMAP-FILE pid=%lu, pFile=%p, hMap=%p, rc=CAPDB_IOERR_MMAP\n",
               osGetCurrentProcessId(), pFile, pFile->hMap));
      return winLogError(CAPDB_IOERR_MMAP, pFile->lastErrno,
                         "winUnmapfile2", pFile->zPath);
    }
    pFile->hMap = NULL;
  }
  OSTRACE(("UNMAP-FILE pid=%lu, pFile=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), pFile));
  return CAPDB_OK;
}

/*
** Memory map or remap the file opened by file-descriptor pFd (if the file
** is already mapped, the existing mapping is replaced by the new). Or, if
** there already exists a mapping for this file, and there are still
** outstanding xFetch() references to it, this function is a no-op.
**
** If parameter nByte is non-negative, then it is the requested size of
** the mapping to create. Otherwise, if nByte is less than zero, then the
** requested size is the size of the file on disk. The actual size of the
** created mapping is either the requested size or the value configured
** using CAPDB_FCNTL_MMAP_SIZE, whichever is smaller.
**
** CAPDB_OK is returned if no error occurs (even if the mapping is not
** recreated as a result of outstanding references) or an SQLite error
** code otherwise.
*/
static int winMapfile(winFile *pFd, capdb_int64 nByte){
  capdb_int64 nMap = nByte;
  int rc;

  assert( nMap>=0 || pFd->nFetchOut==0 );
  OSTRACE(("MAP-FILE pid=%lu, pFile=%p, size=%lld\n",
           osGetCurrentProcessId(), pFd, nByte));

  if( pFd->nFetchOut>0 ) return CAPDB_OK;

  if( nMap<0 ){
    rc = winFileSize((capdb_file*)pFd, &nMap);
    if( rc ){
      OSTRACE(("MAP-FILE pid=%lu, pFile=%p, rc=CAPDB_IOERR_FSTAT\n",
               osGetCurrentProcessId(), pFd));
      return CAPDB_IOERR_FSTAT;
    }
  }
  if( nMap>pFd->mmapSizeMax ){
    nMap = pFd->mmapSizeMax;
  }
  nMap &= ~(capdb_int64)(winSysInfo.dwPageSize - 1);

  if( nMap==0 && pFd->mmapSize>0 ){
    winUnmapfile(pFd);
  }
  if( nMap!=pFd->mmapSize ){
    void *pNew = 0;
    DWORD protect = PAGE_READONLY;
    DWORD flags = FILE_MAP_READ;

    winUnmapfile(pFd);
#ifdef CAPDB_MMAP_READWRITE
    if( (pFd->ctrlFlags & WINFILE_RDONLY)==0 ){
      protect = PAGE_READWRITE;
      flags |= FILE_MAP_WRITE;
    }
#endif
#ifdef CAPDB_UWP
    pFd->hMap = osCreateFileMappingFromApp(pFd->h, NULL, protect, nMap, NULL);
#else
    pFd->hMap = osCreateFileMappingW(pFd->h, NULL, protect,
                                (DWORD)((nMap>>32) & 0xffffffff),
                                (DWORD)(nMap & 0xffffffff), NULL);
#endif
    if( pFd->hMap==NULL ){
      pFd->lastErrno = osGetLastError();
      rc = winLogError(CAPDB_IOERR_MMAP, pFd->lastErrno,
                       "winMapfile1", pFd->zPath);
      /* Log the error, but continue normal operation using xRead/xWrite */
      OSTRACE(("MAP-FILE-CREATE pid=%lu, pFile=%p, rc=%s\n",
               osGetCurrentProcessId(), pFd, capdbErrName(rc)));
      return CAPDB_OK;
    }
    assert( (nMap % winSysInfo.dwPageSize)==0 );
    assert( sizeof(SIZE_T)==sizeof(capdb_int64) || nMap<=0xffffffff );
#ifdef CAPDB_UWP
    pNew = osMapViewOfFileFromApp(pFd->hMap, flags, 0, (SIZE_T)nMap);
#else
    pNew = osMapViewOfFile(pFd->hMap, flags, 0, 0, (SIZE_T)nMap);
#endif
    if( pNew==NULL ){
      osCloseHandle(pFd->hMap);
      pFd->hMap = NULL;
      pFd->lastErrno = osGetLastError();
      rc = winLogError(CAPDB_IOERR_MMAP, pFd->lastErrno,
                       "winMapfile2", pFd->zPath);
      /* Log the error, but continue normal operation using xRead/xWrite */
      OSTRACE(("MAP-FILE-MAP pid=%lu, pFile=%p, rc=%s\n",
               osGetCurrentProcessId(), pFd, capdbErrName(rc)));
      return CAPDB_OK;
    }
    pFd->pMapRegion = pNew;
    pFd->mmapSize = nMap;
  }

  OSTRACE(("MAP-FILE pid=%lu, pFile=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), pFd));
  return CAPDB_OK;
}
#endif /* CAPDB_MAX_MMAP_SIZE>0 */

/*
** If possible, return a pointer to a mapping of file fd starting at offset
** iOff. The mapping must be valid for at least nAmt bytes.
**
** If such a pointer can be obtained, store it in *pp and return CAPDB_OK.
** Or, if one cannot but no error occurs, set *pp to 0 and return CAPDB_OK.
** Finally, if an error does occur, return an SQLite error code. The final
** value of *pp is undefined in this case.
**
** If this function does return a pointer, the caller must eventually
** release the reference by calling winUnfetch().
*/
static int winFetch(capdb_file *fd, i64 iOff, int nAmt, void **pp){
#if CAPDB_MAX_MMAP_SIZE>0
  winFile *pFd = (winFile*)fd;   /* The underlying database file */
#endif
  *pp = 0;

  OSTRACE(("FETCH pid=%lu, pFile=%p, offset=%lld, amount=%d, pp=%p\n",
           osGetCurrentProcessId(), fd, iOff, nAmt, pp));

#if CAPDB_MAX_MMAP_SIZE>0
  if( pFd->mmapSizeMax>0 ){
    /* Ensure that there is always at least a 256 byte buffer of addressable
    ** memory following the returned page. If the database is corrupt,
    ** SQLite may overread the page slightly (in practice only a few bytes,
    ** but 256 is safe, round, number).  */
    const int nEofBuffer = 256;
    if( pFd->pMapRegion==0 ){
      int rc = winMapfile(pFd, -1);
      if( rc!=CAPDB_OK ){
        OSTRACE(("FETCH pid=%lu, pFile=%p, rc=%s\n",
                 osGetCurrentProcessId(), pFd, capdbErrName(rc)));
        return rc;
      }
    }
    if( pFd->mmapSize >= (iOff+nAmt+nEofBuffer) ){
      assert( pFd->pMapRegion!=0 );
      *pp = &((u8 *)pFd->pMapRegion)[iOff];
      pFd->nFetchOut++;
    }
  }
#endif

  OSTRACE(("FETCH pid=%lu, pFile=%p, pp=%p, *pp=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), fd, pp, *pp));
  return CAPDB_OK;
}

/*
** If the third argument is non-NULL, then this function releases a
** reference obtained by an earlier call to winFetch(). The second
** argument passed to this function must be the same as the corresponding
** argument that was passed to the winFetch() invocation.
**
** Or, if the third argument is NULL, then this function is being called
** to inform the VFS layer that, according to POSIX, any existing mapping
** may now be invalid and should be unmapped.
*/
static int winUnfetch(capdb_file *fd, i64 iOff, void *p){
#if CAPDB_MAX_MMAP_SIZE>0
  winFile *pFd = (winFile*)fd;   /* The underlying database file */

  /* If p==0 (unmap the entire file) then there must be no outstanding
  ** xFetch references. Or, if p!=0 (meaning it is an xFetch reference),
  ** then there must be at least one outstanding.  */
  assert( (p==0)==(pFd->nFetchOut==0) );

  /* If p!=0, it must match the iOff value. */
  assert( p==0 || p==&((u8 *)pFd->pMapRegion)[iOff] );

  OSTRACE(("UNFETCH pid=%lu, pFile=%p, offset=%lld, p=%p\n",
           osGetCurrentProcessId(), pFd, iOff, p));

  if( p ){
    pFd->nFetchOut--;
  }else{
    /* FIXME:  If Windows truly always prevents truncating or deleting a
    ** file while a mapping is held, then the following winUnmapfile() call
    ** is unnecessary can be omitted - potentially improving
    ** performance.  */
    winUnmapfile(pFd);
  }

  assert( pFd->nFetchOut>=0 );
#endif

  OSTRACE(("UNFETCH pid=%lu, pFile=%p, rc=CAPDB_OK\n",
           osGetCurrentProcessId(), fd));
  return CAPDB_OK;
}

/*
** Here ends the implementation of all capdb_file methods.
**
********************** End capdb_file Methods *******************************
******************************************************************************/

/*
** This vector defines all the methods that can operate on an
** capdb_file for win32.
*/
static const capdb_io_methods winIoMethod = {
  3,                              /* iVersion */
  winClose,                       /* xClose */
  winRead,                        /* xRead */
  winWrite,                       /* xWrite */
  winTruncate,                    /* xTruncate */
  winSync,                        /* xSync */
  winFileSize,                    /* xFileSize */
  winLock,                        /* xLock */
  winUnlock,                      /* xUnlock */
  winCheckReservedLock,           /* xCheckReservedLock */
  winFileControl,                 /* xFileControl */
  winSectorSize,                  /* xSectorSize */
  winDeviceCharacteristics,       /* xDeviceCharacteristics */
  winShmMap,                      /* xShmMap */
  winShmLock,                     /* xShmLock */
  winShmBarrier,                  /* xShmBarrier */
  winShmUnmap,                    /* xShmUnmap */
  winFetch,                       /* xFetch */
  winUnfetch                      /* xUnfetch */
};

/*
** This vector defines all the methods that can operate on an
** capdb_file for win32 without performing any locking.
*/
static const capdb_io_methods winIoNolockMethod = {
  3,                              /* iVersion */
  winClose,                       /* xClose */
  winRead,                        /* xRead */
  winWrite,                       /* xWrite */
  winTruncate,                    /* xTruncate */
  winSync,                        /* xSync */
  winFileSize,                    /* xFileSize */
  winNolockLock,                  /* xLock */
  winNolockUnlock,                /* xUnlock */
  winNolockCheckReservedLock,     /* xCheckReservedLock */
  winFileControl,                 /* xFileControl */
  winSectorSize,                  /* xSectorSize */
  winDeviceCharacteristics,       /* xDeviceCharacteristics */
  winShmMap,                      /* xShmMap */
  winShmLock,                     /* xShmLock */
  winShmBarrier,                  /* xShmBarrier */
  winShmUnmap,                    /* xShmUnmap */
  winFetch,                       /* xFetch */
  winUnfetch                      /* xUnfetch */
};

static winVfsAppData winAppData = {
  &winIoMethod,       /* pMethod */
  0,                  /* pAppData */
  0                   /* bNoLock */
};

static winVfsAppData winNolockAppData = {
  &winIoNolockMethod, /* pMethod */
  0,                  /* pAppData */
  1                   /* bNoLock */
};

/****************************************************************************
**************************** capdb_vfs methods ****************************
**
** This division contains the implementation of methods on the
** capdb_vfs object.
*/

/*
** This function returns non-zero if the specified UTF-8 string buffer
** ends with a directory separator character or one was successfully
** added to it.
*/
static int winMakeEndInDirSep(int nBuf, char *zBuf){
  if( zBuf ){
    int nLen = capdbStrlen30(zBuf);
    if( nLen>0 ){
      if( winIsDirSep(zBuf[nLen-1]) ){
        return 1;
      }else if( nLen+1<nBuf ){
        if( !osGetenv ){
          zBuf[nLen] = winGetDirSep();
        }else if( winIsDriveLetterAndColon(zBuf) && winIsDirSep(zBuf[2]) ){
          zBuf[nLen] = '\\';
          zBuf[2]='\\';
        }else{
          zBuf[nLen] = '/';
        }
        zBuf[nLen+1] = '\0';
        return 1;
      }
    }
  }
  return 0;
}

/*
** If capdb_temp_directory is defined, take the mutex and return true.
**
** If capdb_temp_directory is NULL (undefined), omit the mutex and
** return false.
*/
static int winTempDirDefined(void){
  capdb_mutex_enter(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
  if( capdb_temp_directory!=0 ) return 1;
  capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
  return 0;
}

/*
** Create a temporary file name and store the resulting pointer into pzBuf.
** The pointer returned in pzBuf must be freed via capdb_free().
*/
static int winGetTempname(capdb_vfs *pVfs, char **pzBuf){
  static const char zChars[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789";
  size_t i, j;
  DWORD pid;
  int nPre = capdbStrlen30(CAPDB_TEMP_FILE_PREFIX);
  i64 nMax, nBuf, nDir, nLen;
  char *zBuf;

  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that SQLite handles this
  ** function failing.
  */
  SimulateIOError( return CAPDB_IOERR );

  /* Allocate a temporary buffer to store the fully qualified file
  ** name for the temporary file.  If this fails, we cannot continue.
  */
  nMax = pVfs->mxPathname;
  nBuf = 2 + (i64)nMax;
  zBuf = capdbMallocZero( nBuf );
  if( !zBuf ){
    OSTRACE(("TEMP-FILENAME rc=CAPDB_IOERR_NOMEM\n"));
    return CAPDB_IOERR_NOMEM_BKPT;
  }

  /* Figure out the effective temporary directory.  First, check if one
  ** has been explicitly set by the application; otherwise, use the one
  ** configured by the operating system.
  */
  nDir = nMax - (nPre + 15);
  assert( nDir>0 );
  if( winTempDirDefined() ){
    int nDirLen = capdbStrlen30(capdb_temp_directory);
    if( nDirLen>0 ){
      if( !winIsDirSep(capdb_temp_directory[nDirLen-1]) ){
        nDirLen++;
      }
      if( nDirLen>nDir ){
        capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
        capdb_free(zBuf);
        OSTRACE(("TEMP-FILENAME rc=CAPDB_ERROR\n"));
        return winLogError(CAPDB_ERROR, 0, "winGetTempname1", 0);
      }
      capdb_snprintf(nMax, zBuf, "%s", capdb_temp_directory);
    }
    capdb_mutex_leave(capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR));
  }

#if defined(__CYGWIN__)
  else if( osGetenv!=NULL ){
    static const char *azDirs[] = {
       0, /* getenv("CAPDB_TMPDIR") */
       0, /* getenv("TMPDIR") */
       0, /* getenv("TMP") */
       0, /* getenv("TEMP") */
       0, /* getenv("USERPROFILE") */
       "/var/tmp",
       "/usr/tmp",
       "/tmp",
       ".",
       0        /* List terminator */
    };
    unsigned int i;
    const char *zDir = 0;

    if( !azDirs[0] ) azDirs[0] = osGetenv("CAPDB_TMPDIR");
    if( !azDirs[1] ) azDirs[1] = osGetenv("TMPDIR");
    if( !azDirs[2] ) azDirs[2] = osGetenv("TMP");
    if( !azDirs[3] ) azDirs[3] = osGetenv("TEMP");
    if( !azDirs[4] ) azDirs[4] = osGetenv("USERPROFILE");
    for(i=0; i<sizeof(azDirs)/sizeof(azDirs[0]); zDir=azDirs[i++]){
      void *zConverted;
      if( zDir==0 ) continue;
      /* If the path starts with a drive letter followed by the colon
      ** character, assume it is already a native Win32 path; otherwise,
      ** it must be converted to a native Win32 path via the Cygwin API
      ** prior to using it.
      */
      {
        zConverted = winConvertFromUtf8Filename(zDir);
        if( !zConverted ){
          capdb_free(zBuf);
          OSTRACE(("TEMP-FILENAME rc=CAPDB_IOERR_NOMEM\n"));
          return CAPDB_IOERR_NOMEM_BKPT;
        }
        if( winIsDir(zConverted) ){
          capdb_snprintf(nMax, zBuf, "%s", zDir);
          capdb_free(zConverted);
          break;
        }
        capdb_free(zConverted);
      }
    }
  }
#endif

  else{
    char *zMulti;
    LPWSTR zWidePath = capdbMallocZero( nMax*sizeof(WCHAR) );
    if( !zWidePath ){
      capdb_free(zBuf);
      OSTRACE(("TEMP-FILENAME rc=CAPDB_IOERR_NOMEM\n"));
      return CAPDB_IOERR_NOMEM_BKPT;
    }
    if( osGetTempPathW==0 || osGetTempPathW(nMax, zWidePath)==0 ){
      capdb_free(zWidePath);
      capdb_free(zBuf);
      OSTRACE(("TEMP-FILENAME rc=CAPDB_IOERR_GETTEMPPATH\n"));
      return winLogError(CAPDB_IOERR_GETTEMPPATH, osGetLastError(),
                         "winGetTempname2", 0);
    }
    zMulti = winUnicodeToUtf8(zWidePath);
    if( zMulti ){
      capdb_snprintf(nMax, zBuf, "%s", zMulti);
      capdb_free(zMulti);
      capdb_free(zWidePath);
    }else{
      capdb_free(zWidePath);
      capdb_free(zBuf);
      OSTRACE(("TEMP-FILENAME rc=CAPDB_IOERR_NOMEM\n"));
      return CAPDB_IOERR_NOMEM_BKPT;
    }
  }

  /*
  ** Check to make sure the temporary directory ends with an appropriate
  ** separator.  If it does not and there is not enough space left to add
  ** one, fail.
  */
  if( !winMakeEndInDirSep(nDir+1, zBuf) ){
    capdb_free(zBuf);
    OSTRACE(("TEMP-FILENAME rc=CAPDB_ERROR\n"));
    return winLogError(CAPDB_ERROR, 0, "winGetTempname4", 0);
  }

  /*
  ** Check that the output buffer is large enough for the temporary file
  ** name in the following format:
  **
  **   "<temporary_directory>/etilqs_XXXXXXXXXXXXXXX\0\0"
  **
  ** If not, return CAPDB_ERROR.  The number 17 is used here in order to
  ** account for the space used by the 15 character random suffix and the
  ** two trailing NUL characters.  The final directory separator character
  ** has already added if it was not already present.
  */
  nLen = capdbStrlen30(zBuf);
  if( (nLen + nPre + 17) > nBuf ){
    capdb_free(zBuf);
    OSTRACE(("TEMP-FILENAME rc=CAPDB_ERROR\n"));
    return winLogError(CAPDB_ERROR, 0, "winGetTempname5", 0);
  }

  capdb_snprintf(nBuf-16-nLen, zBuf+nLen, CAPDB_TEMP_FILE_PREFIX);

  j = capdbStrlen30(zBuf);
  capdb_randomness(15, &zBuf[j]);
  pid = osGetCurrentProcessId();
  for(i=0; i<15; i++, j++){
    zBuf[j] += pid & 0xff;
    pid >>= 8;
    zBuf[j] = (char)zChars[ ((unsigned char)zBuf[j])%(sizeof(zChars)-1) ];
  }
  zBuf[j] = 0;
  zBuf[j+1] = 0;
  *pzBuf = zBuf;

  OSTRACE(("TEMP-FILENAME name=%s, rc=CAPDB_OK\n", zBuf));
  return CAPDB_OK;
}

/*
** Return TRUE if the named file is really a directory.  Return false if
** it is something other than a directory, or if there is any kind of memory
** allocation failure.
*/
static int winIsDir(const void *zConverted){
  DWORD attr;
  int rc = 0;
  DWORD lastErrno;
  int cnt = 0;
  WIN32_FILE_ATTRIBUTE_DATA sAttrData;
  memset(&sAttrData, 0, sizeof(sAttrData));
  while( !(rc = osGetFileAttributesExW((LPCWSTR)zConverted,
                           GetFileExInfoStandard,
                           &sAttrData)) && winRetryIoerr(&cnt, &lastErrno) ){}
  if( !rc ){
    return 0; /* Invalid name? */
  }
  attr = sAttrData.dwFileAttributes;
  return (attr!=INVALID_FILE_ATTRIBUTES) && (attr&FILE_ATTRIBUTE_DIRECTORY);
}

/* forward reference */
static int winAccess(
  capdb_vfs *pVfs,         /* Not used on win32 */
  const char *zFilename,     /* Name of file to check */
  int flags,                 /* Type of test to make on this file */
  int *pResOut               /* OUT: Result */
);

/*
** The Windows version of xAccess() accepts an extra bit in the flags
** parameter that prevents an anti-virus retry loop.
*/
#define NORETRY 0x4000

/*
** Open a file.
*/
static int winOpen(
  capdb_vfs *pVfs,        /* Used to get maximum path length and AppData */
  const char *zName,        /* Name of the file (UTF-8) */
  capdb_file *id,         /* Write the SQLite file handle here */
  int flags,                /* Open mode flags */
  int *pOutFlags            /* Status return flags */
){
  HANDLE h;
  DWORD lastErrno = 0;
  DWORD dwDesiredAccess;
  DWORD dwShareMode;
  DWORD dwCreationDisposition;
  DWORD dwFlagsAndAttributes = 0;
  winVfsAppData *pAppData;
  winFile *pFile = (winFile*)id;
  void *zConverted;              /* Filename in OS encoding */
  const char *zUtf8Name = zName; /* Filename in UTF-8 encoding */
  int cnt = 0;
  int isRO = 0;              /* file is known to be accessible readonly */

  /* If argument zPath is a NULL pointer, this function is required to open
  ** a temporary file. Use this buffer to store the file name in.
  */
  char *zTmpname = 0; /* For temporary filename, if necessary. */

  int rc = CAPDB_OK;            /* Function Return Code */
#if !defined(NDEBUG)
  int eType = flags&0x0FFF00;  /* Type of file to open */
#endif

  int isExclusive  = (flags & CAPDB_OPEN_EXCLUSIVE);
  int isDelete     = (flags & CAPDB_OPEN_DELETEONCLOSE);
  int isCreate     = (flags & CAPDB_OPEN_CREATE);
  int isReadonly   = (flags & CAPDB_OPEN_READONLY);
  int isReadWrite  = (flags & CAPDB_OPEN_READWRITE);

#ifndef NDEBUG
  int isOpenJournal = (isCreate && (
        eType==CAPDB_OPEN_SUPER_JOURNAL
     || eType==CAPDB_OPEN_MAIN_JOURNAL
     || eType==CAPDB_OPEN_WAL
  ));
#endif

  OSTRACE(("OPEN name=%s, pFile=%p, flags=%x, pOutFlags=%p\n",
           zUtf8Name, id, flags, pOutFlags));

  /* Check the following statements are true:
  **
  **   (a) Exactly one of the READWRITE and READONLY flags must be set, and
  **   (b) if CREATE is set, then READWRITE must also be set, and
  **   (c) if EXCLUSIVE is set, then CREATE must also be set.
  **   (d) if DELETEONCLOSE is set, then CREATE must also be set.
  */
  assert((isReadonly==0 || isReadWrite==0) && (isReadWrite || isReadonly));
  assert(isCreate==0 || isReadWrite);
  assert(isExclusive==0 || isCreate);
  assert(isDelete==0 || isCreate);

  /* The main DB, main journal, WAL file and super-journal are never
  ** automatically deleted. Nor are they ever temporary files.  */
  assert( (!isDelete && zName) || eType!=CAPDB_OPEN_MAIN_DB );
  assert( (!isDelete && zName) || eType!=CAPDB_OPEN_MAIN_JOURNAL );
  assert( (!isDelete && zName) || eType!=CAPDB_OPEN_SUPER_JOURNAL );
  assert( (!isDelete && zName) || eType!=CAPDB_OPEN_WAL );

  /* Assert that the upper layer has set one of the "file-type" flags. */
  assert( eType==CAPDB_OPEN_MAIN_DB      || eType==CAPDB_OPEN_TEMP_DB
       || eType==CAPDB_OPEN_MAIN_JOURNAL || eType==CAPDB_OPEN_TEMP_JOURNAL
       || eType==CAPDB_OPEN_SUBJOURNAL   || eType==CAPDB_OPEN_SUPER_JOURNAL
       || eType==CAPDB_OPEN_TRANSIENT_DB || eType==CAPDB_OPEN_WAL
  );

  assert( pFile!=0 );
  memset(pFile, 0, sizeof(winFile));
  pFile->h = INVALID_HANDLE_VALUE;

  /* If the second argument to this function is NULL, generate a
  ** temporary file name to use
  */
  if( !zUtf8Name ){
    assert( isDelete && !isOpenJournal );
    rc = winGetTempname(pVfs, &zTmpname);
    if( rc!=CAPDB_OK ){
      OSTRACE(("OPEN name=%s, rc=%s", zUtf8Name, capdbErrName(rc)));
      return rc;
    }
    zUtf8Name = zTmpname;
  }

  /* Database filenames are double-zero terminated if they are not
  ** URIs with parameters.  Hence, they can always be passed into
  ** capdb_uri_parameter().
  */
  assert( (eType!=CAPDB_OPEN_MAIN_DB) || (flags & CAPDB_OPEN_URI) ||
       zUtf8Name[capdbStrlen30(zUtf8Name)+1]==0 );

  /* Convert the filename to the system encoding. */
  zConverted = winConvertFromUtf8Filename(zUtf8Name);
  if( zConverted==0 ){
    capdb_free(zTmpname);
    OSTRACE(("OPEN name=%s, rc=CAPDB_IOERR_NOMEM", zUtf8Name));
    return CAPDB_IOERR_NOMEM_BKPT;
  }

  if( winIsDir(zConverted) ){
    capdb_free(zConverted);
    capdb_free(zTmpname);
    OSTRACE(("OPEN name=%s, rc=CAPDB_CANTOPEN_ISDIR", zUtf8Name));
    return CAPDB_CANTOPEN_ISDIR;
  }

  if( isReadWrite ){
    dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
  }else{
    dwDesiredAccess = GENERIC_READ;
  }

  /* CAPDB_OPEN_EXCLUSIVE is used to make sure that a new file is
  ** created. SQLite doesn't use it to indicate "exclusive access"
  ** as it is usually understood.
  */
  if( isExclusive ){
    /* Creates a new file, only if it does not already exist. */
    /* If the file exists, it fails. */
    dwCreationDisposition = CREATE_NEW;
  }else if( isCreate ){
    /* Open existing file, or create if it doesn't exist */
    dwCreationDisposition = OPEN_ALWAYS;
  }else{
    /* Opens a file, only if it exists. */
    dwCreationDisposition = OPEN_EXISTING;
  }

  if( 0==capdb_uri_boolean(zName, "exclusive", 0) ){
    dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
  }else{
    dwShareMode = 0;
  }

  if( isDelete ){
    dwFlagsAndAttributes = FILE_ATTRIBUTE_TEMPORARY
                               | FILE_ATTRIBUTE_HIDDEN
                               | FILE_FLAG_DELETE_ON_CLOSE;
  }else{
    dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL;
  }
  do{
    h = osCreateFileW((LPCWSTR)zConverted,
                      dwDesiredAccess,
                      dwShareMode, NULL,
                      dwCreationDisposition,
                      dwFlagsAndAttributes,
                      NULL);
    if( h!=INVALID_HANDLE_VALUE ) break;
    if( isReadWrite ){
      int rc2;
      capdbBeginBenignMalloc();
      rc2 = winAccess(pVfs, zUtf8Name, CAPDB_ACCESS_READ|NORETRY, &isRO);
      capdbEndBenignMalloc();
      if( rc2==CAPDB_OK && isRO ) break;
    }
  }while( winRetryIoerr(&cnt, &lastErrno) );
  winLogIoerr(cnt, __LINE__);

  OSTRACE(("OPEN file=%p, name=%s, access=%lx, rc=%s\n", h, zUtf8Name,
           dwDesiredAccess, (h==INVALID_HANDLE_VALUE) ? "failed" : "ok"));

  if( h==INVALID_HANDLE_VALUE ){
    capdb_free(zConverted);
    capdb_free(zTmpname);
    if( isReadWrite && isRO && !isExclusive ){
      return winOpen(pVfs, zName, id,
         ((flags|CAPDB_OPEN_READONLY) &
                     ~(CAPDB_OPEN_CREATE|CAPDB_OPEN_READWRITE)),
         pOutFlags);
    }else{
      pFile->lastErrno = lastErrno;
      winLogError(CAPDB_CANTOPEN, pFile->lastErrno, "winOpen", zUtf8Name);
      return CAPDB_CANTOPEN_BKPT;
    }
  }

  if( pOutFlags ){
    if( isReadWrite ){
      *pOutFlags = CAPDB_OPEN_READWRITE;
    }else{
      *pOutFlags = CAPDB_OPEN_READONLY;
    }
  }

  OSTRACE(("OPEN file=%p, name=%s, access=%lx, pOutFlags=%p, *pOutFlags=%d, "
           "rc=%s\n", h, zUtf8Name, dwDesiredAccess, pOutFlags, pOutFlags ?
           *pOutFlags : 0, (h==INVALID_HANDLE_VALUE) ? "failed" : "ok"));

  pAppData = (winVfsAppData*)pVfs->pAppData;

  capdb_free(zConverted);
  capdb_free(zTmpname);
  id->pMethods = pAppData ? pAppData->pMethod : &winIoMethod;
  pFile->pVfs = pVfs;
  pFile->h = h;
  if( isReadonly ){
    pFile->ctrlFlags |= WINFILE_RDONLY;
  }
  if( (flags & CAPDB_OPEN_MAIN_DB)
   && capdb_uri_boolean(zName, "psow", CAPDB_POWERSAFE_OVERWRITE) 
  ){
    pFile->ctrlFlags |= WINFILE_PSOW;
  }
  pFile->lastErrno = NO_ERROR;
  pFile->zPath = zName;
#if CAPDB_MAX_MMAP_SIZE>0
  pFile->hMap = NULL;
  pFile->pMapRegion = 0;
  pFile->mmapSize = 0;
  pFile->mmapSizeMax = capdbGlobalConfig.szMmap;
#endif

  OpenCounter(+1);
  return rc;
}

/*
** Delete the named file.
**
** Note that Windows does not allow a file to be deleted if some other
** process has it open.  Sometimes a virus scanner or indexing program
** will open a journal file shortly after it is created in order to do
** whatever it does.  While this other process is holding the
** file open, we will be unable to delete it.  To work around this
** problem, we delay 100 milliseconds and try to delete again.  Up
** to MX_DELETION_ATTEMPTs deletion attempts are run before giving
** up and returning an error.
*/
static int winDelete(
  capdb_vfs *pVfs,          /* Not used on win32 */
  const char *zFilename,      /* Name of file to delete */
  int syncDir                 /* Not used on win32 */
){
  int cnt = 0;
  int rc;
  DWORD attr;
  DWORD lastErrno = 0;
  void *zConverted;
  UNUSED_PARAMETER(pVfs);
  UNUSED_PARAMETER(syncDir);

  SimulateIOError(return CAPDB_IOERR_DELETE);
  OSTRACE(("DELETE name=%s, syncDir=%d\n", zFilename, syncDir));

  zConverted = winConvertFromUtf8Filename(zFilename);
  if( zConverted==0 ){
    OSTRACE(("DELETE name=%s, rc=CAPDB_IOERR_NOMEM\n", zFilename));
    return CAPDB_IOERR_NOMEM_BKPT;
  }
  do {
    attr = osGetFileAttributesW(zConverted);
    if ( attr==INVALID_FILE_ATTRIBUTES ){
      lastErrno = osGetLastError();
      if( lastErrno==ERROR_FILE_NOT_FOUND
       || lastErrno==ERROR_PATH_NOT_FOUND ){
        rc = CAPDB_IOERR_DELETE_NOENT; /* Already gone? */
      }else{
        rc = CAPDB_ERROR;
      }
      break;
    }
    if ( attr&FILE_ATTRIBUTE_DIRECTORY ){
      rc = CAPDB_ERROR; /* Files only. */
      break;
    }
    if ( osDeleteFileW(zConverted) ){
      rc = CAPDB_OK; /* Deleted OK. */
      break;
    }
    if ( !winRetryIoerr(&cnt, &lastErrno) ){
      rc = CAPDB_ERROR; /* No more retries. */
      break;
    }
  } while(1);
  if( rc && rc!=CAPDB_IOERR_DELETE_NOENT ){
    rc = winLogError(CAPDB_IOERR_DELETE, lastErrno, "winDelete", zFilename);
  }else{
    winLogIoerr(cnt, __LINE__);
  }
  capdb_free(zConverted);
  OSTRACE(("DELETE name=%s, rc=%s\n", zFilename, capdbErrName(rc)));
  return rc;
}

/*
** Check the existence and status of a file.
*/
static int winAccess(
  capdb_vfs *pVfs,         /* Not used on win32 */
  const char *zFilename,     /* Name of file to check */
  int flags,                 /* Type of test to make on this file */
  int *pResOut               /* OUT: Result */
){
  DWORD attr;
  int rc = 0;
  int cnt = 0;
  DWORD lastErrno = 0;
  void *zConverted;
  int noRetry = 0;           /* Do not use winRetryIoerr() */
  WIN32_FILE_ATTRIBUTE_DATA sAttrData;
  UNUSED_PARAMETER(pVfs);

  if( (flags & NORETRY)!=0 ){
    noRetry = 1;
    flags &= ~NORETRY;
  }  

  SimulateIOError( return CAPDB_IOERR_ACCESS; );
  OSTRACE(("ACCESS name=%s, flags=%x, pResOut=%p\n",
           zFilename, flags, pResOut));

  if( zFilename==0 ){
    *pResOut = 0;
    OSTRACE(("ACCESS name=%s, pResOut=%p, *pResOut=%d, rc=CAPDB_OK\n",
             zFilename, pResOut, *pResOut));
    return CAPDB_OK;
  }

  zConverted = winConvertFromUtf8Filename(zFilename);
  if( zConverted==0 ){
    OSTRACE(("ACCESS name=%s, rc=CAPDB_IOERR_NOMEM\n", zFilename));
    return CAPDB_IOERR_NOMEM_BKPT;
  }
  memset(&sAttrData, 0, sizeof(sAttrData));
  while( !(rc = osGetFileAttributesExW((LPCWSTR)zConverted,
                           GetFileExInfoStandard,
                           &sAttrData))
     && !noRetry
     && winRetryIoerr(&cnt, &lastErrno)
  ){ /* Loop until true */}
  if( rc ){
    /* For an CAPDB_ACCESS_EXISTS query, treat a zero-length file
    ** as if it does not exist.
    */
    if(    flags==CAPDB_ACCESS_EXISTS
        && sAttrData.nFileSizeHigh==0
        && sAttrData.nFileSizeLow==0 ){
      attr = INVALID_FILE_ATTRIBUTES;
    }else{
      attr = sAttrData.dwFileAttributes;
    }
  }else{
    if( noRetry ) lastErrno = osGetLastError();
    winLogIoerr(cnt, __LINE__);
    if( lastErrno!=ERROR_FILE_NOT_FOUND && lastErrno!=ERROR_PATH_NOT_FOUND ){
      capdb_free(zConverted);
      return winLogError(CAPDB_IOERR_ACCESS, lastErrno, "winAccess",
                         zFilename);
    }else{
      attr = INVALID_FILE_ATTRIBUTES;
    }
  }
  capdb_free(zConverted);
  switch( flags ){
    case CAPDB_ACCESS_READ:
    case CAPDB_ACCESS_EXISTS:
      rc = attr!=INVALID_FILE_ATTRIBUTES;
      break;
    case CAPDB_ACCESS_READWRITE:
      rc = attr!=INVALID_FILE_ATTRIBUTES &&
             (attr & FILE_ATTRIBUTE_READONLY)==0;
      break;
    default:
      assert(!"Invalid flags argument");
  }
  *pResOut = rc;
  OSTRACE(("ACCESS name=%s, pResOut=%p, *pResOut=%d, rc=CAPDB_OK\n",
           zFilename, pResOut, *pResOut));
  return CAPDB_OK;
}

/*
** Returns non-zero if the specified path name starts with the "long path"
** prefix.
*/
static BOOL winIsLongPathPrefix(
  const char *zPathname
){
  return ( zPathname[0]=='\\' && zPathname[1]=='\\'
        && zPathname[2]=='?'  && zPathname[3]=='\\' );
}

/*
** Returns non-zero if the specified path name starts with a drive letter
** followed by a colon character.
*/
static BOOL winIsDriveLetterAndColon(
  const char *zPathname
){
  return ( capdbIsalpha(zPathname[0]) && zPathname[1]==':' );
}

#ifdef _WIN32
/*
** Returns non-zero if the specified path name should be used verbatim.  If
** non-zero is returned from this function, the calling function must simply
** use the provided path name verbatim -OR- resolve it into a full path name
** using the GetFullPathName Win32 API function (if available).
*/
static BOOL winIsVerbatimPathname(
  const char *zPathname
){
  /*
  ** If the path name starts with a forward slash or a backslash, it is either
  ** a legal UNC name, a volume relative path, or an absolute path name in the
  ** "Unix" format on Windows.  There is no easy way to differentiate between
  ** the final two cases; therefore, we return the safer return value of TRUE
  ** so that callers of this function will simply use it verbatim.
  */
  if ( winIsDirSep(zPathname[0]) ){
    return TRUE;
  }

  /*
  ** If the path name starts with a letter and a colon it is either a volume
  ** relative path or an absolute path.  Callers of this function must not
  ** attempt to treat it as a relative path name (i.e. they should simply use
  ** it verbatim).
  */
  if ( winIsDriveLetterAndColon(zPathname) ){
    return TRUE;
  }

  /*
  ** If we get to this point, the path name should almost certainly be a purely
  ** relative one (i.e. not a UNC name, not absolute, and not volume relative).
  */
  return FALSE;
}
#endif /* _WIN32 */

#ifdef __CYGWIN__
/*
** Simplify a filename into its canonical form
** by making the following changes:
**
**  * convert any '/' to '\' (win32) or reverse (Cygwin)
**  * removing any trailing and duplicate / (except for UNC paths)
**  * convert /./ into just /
**
** Changes are made in-place.  Return the new name length.
**
** The original filename is in z[0..]. If the path is shortened,
** no-longer used bytes will be written by '\0'.
*/
static void winSimplifyName(char *z){
  int i, j;
  for(i=j=0; z[i]; ++i){
    if( winIsDirSep(z[i]) ){
#if !defined(CAPDB_TEST)
      /* Some test-cases assume that "./foo" and "foo" are different */
      if( z[i+1]=='.' && winIsDirSep(z[i+2]) ){
        ++i;
        continue;
      }
#endif
      if( !z[i+1] || (winIsDirSep(z[i+1]) && (i!=0)) ){
        continue;
      }
      z[j++] = osGetenv?'/':'\\';
    }else{
      z[j++] = z[i];
    }
  }
  while(j<i) z[j++] = '\0';
}

#define CAPDB_MAX_SYMLINKS 100

static int mkFullPathname(
  const char *zPath,              /* Input path */
  char *zOut,                     /* Output buffer */
  int nOut                        /* Allocated size of buffer zOut */
){
  int nPath = capdbStrlen30(zPath);
  int iOff = 0;
  if( zPath[0]!='/' ){
    if( osGetcwd(zOut, nOut-2)==0 ){
      return winLogError(CAPDB_CANTOPEN_BKPT, (DWORD)osErrno, "getcwd", zPath);
    }
    iOff = capdbStrlen30(zOut);
    zOut[iOff++] = '/';
  }
  if( (iOff+nPath+1)>nOut ){
    /* SQLite assumes that xFullPathname() nul-terminates the output buffer
    ** even if it returns an error.  */
    zOut[iOff] = '\0';
    return CAPDB_CANTOPEN_BKPT;
  }
  capdb_snprintf(nOut-iOff, &zOut[iOff], "%s", zPath);
  return CAPDB_OK;
}
#endif /* __CYGWIN__ */

/*
** Turn a relative pathname into a full pathname.  Write the full
** pathname into zOut[].  zOut[] will be at least pVfs->mxPathname
** bytes in size.
*/
static int winFullPathnameNoMutex(
  capdb_vfs *pVfs,            /* Pointer to vfs object */
  const char *zRelative,        /* Possibly relative input path */
  int nFull,                    /* Size of output buffer in bytes */
  char *zFull                   /* Output buffer */
){
  int nByte;
  void *zConverted;
  char *zOut;

  /* If this path name begins with "/X:" or "\\?\", where "X" is any
  ** alphabetic character, discard the initial "/" from the pathname.
  */
  if( zRelative[0]=='/' && (winIsDriveLetterAndColon(zRelative+1)
       || winIsLongPathPrefix(zRelative+1)) ){
    zRelative++;
  }

  SimulateIOError( return CAPDB_ERROR );

#ifdef __CYGWIN__
  if( osGetcwd ){
    zFull[nFull-1] = '\0';
    if( !winIsDriveLetterAndColon(zRelative) || !winIsDirSep(zRelative[2]) ){
      int rc = CAPDB_OK;
      int nLink = 1;              /* Number of symbolic links followed so far */
      const char *zIn = zRelative; /* Input path for each iteration of loop */
      char *zDel = 0;
      struct stat buf;

      UNUSED_PARAMETER(pVfs);

      do {
        /* Call lstat() on path zIn. Set bLink to true if the path
        ** is a symbolic link, or false otherwise.  */
        int bLink = 0;
        if( osLstat && osReadlink ) {
          if( osLstat(zIn, &buf)!=0 ){
            int myErrno = osErrno;
            if( myErrno!=ENOENT ){
              rc = winLogError(CAPDB_CANTOPEN_BKPT, (DWORD)myErrno,
                               "lstat", zIn);
            }
          }else{
            bLink = ((buf.st_mode & 0170000) == 0120000);
          }

          if( bLink ){
            if( zDel==0 ){
              zDel = capdbMallocZero(nFull);
              if( zDel==0 ) rc = CAPDB_NOMEM;
            }else if( ++nLink>CAPDB_MAX_SYMLINKS ){
              rc = CAPDB_CANTOPEN_BKPT;
            }

            if( rc==CAPDB_OK ){
              nByte = osReadlink(zIn, zDel, nFull-1);
              if( nByte ==(DWORD)-1 ){
                rc = winLogError(CAPDB_CANTOPEN_BKPT, (DWORD)osErrno,
                                 "readlink", zIn);
              }else{
                if( zDel[0]!='/' ){
                  int n;
                  for(n = capdbStrlen30(zIn); n>0 && zIn[n-1]!='/'; n--);
                  if( nByte+n+1>nFull ){
                    rc = CAPDB_CANTOPEN_BKPT;
                  }else{
                    memmove(&zDel[n], zDel, nByte+1);
                    memcpy(zDel, zIn, n);
                    nByte += n;
                  }
                }
                zDel[nByte] = '\0';
              }
            }

            zIn = zDel;
          }
        }

        assert( rc!=CAPDB_OK || zIn!=zFull || zIn[0]=='/' );
        if( rc==CAPDB_OK && zIn!=zFull ){
          rc = mkFullPathname(zIn, zFull, nFull);
        }
        if( bLink==0 ) break;
        zIn = zFull;
      }while( rc==CAPDB_OK );

      capdb_free(zDel);
      winSimplifyName(zFull);
      return rc;
    }
  }
#endif /* __CYGWIN__ */

#if defined(_WIN32)
  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that SQLite handles this
  ** function failing. This function could fail if, for example, the
  ** current working directory has been unlinked.
  */
  SimulateIOError( return CAPDB_ERROR );
  if ( capdb_data_directory && !winIsVerbatimPathname(zRelative) ){
    /*
    ** NOTE: We are dealing with a relative path name and the data
    **       directory has been set.  Therefore, use it as the basis
    **       for converting the relative path name to an absolute
    **       one by prepending the data directory and a backslash.
    */
    capdb_snprintf(MIN(nFull, pVfs->mxPathname), zFull, "%s%c%s",
                     capdb_data_directory, winGetDirSep(), zRelative);
    return CAPDB_OK;
  }
#endif
  zConverted = winConvertFromUtf8Filename(zRelative);
  if( zConverted==0 ){
    return CAPDB_IOERR_NOMEM_BKPT;
  }
  {
    LPWSTR zTemp;
    nByte = osGetFullPathNameW((LPCWSTR)zConverted, 0, 0, 0);
    if( nByte==0 ){
      capdb_free(zConverted);
      return winLogError(CAPDB_CANTOPEN_FULLPATH, osGetLastError(),
                         "winFullPathname1", zRelative);
    }
    nByte += 3;
    zTemp = capdbMallocZero( nByte*sizeof(zTemp[0]) );
    if( zTemp==0 ){
      capdb_free(zConverted);
      return CAPDB_IOERR_NOMEM_BKPT;
    }
    nByte = osGetFullPathNameW((LPCWSTR)zConverted, nByte, zTemp, 0);
    if( nByte==0 ){
      capdb_free(zConverted);
      capdb_free(zTemp);
      return winLogError(CAPDB_CANTOPEN_FULLPATH, osGetLastError(),
                         "winFullPathname2", zRelative);
    }
    capdb_free(zConverted);
    zOut = winUnicodeToUtf8(zTemp);
    capdb_free(zTemp);
  }
  if( zOut ){
#ifdef __CYGWIN__
    if( memcmp(zOut, "\\\\?\\", 4) ){
      capdb_snprintf(MIN(nFull, pVfs->mxPathname), zFull, "%s", zOut);
    }else if( memcmp(zOut+4, "UNC\\", 4) ){
      capdb_snprintf(MIN(nFull, pVfs->mxPathname), zFull, "%s", zOut+4);
    }else{
      char *p = zOut+6;
      *p = '\\';
      if( osGetcwd ){
        /* On Cygwin, UNC paths use forward slashes */
        while( *p ){
          if( *p=='\\' ) *p = '/';
          ++p;
        }
      }
      capdb_snprintf(MIN(nFull, pVfs->mxPathname), zFull, "%s", zOut+6);
    }
#else
    capdb_snprintf(MIN(nFull, pVfs->mxPathname), zFull, "%s", zOut);
#endif /* __CYGWIN__ */
    capdb_free(zOut);
    return CAPDB_OK;
  }else{
    return CAPDB_IOERR_NOMEM_BKPT;
  }
}
static int winFullPathname(
  capdb_vfs *pVfs,            /* Pointer to vfs object */
  const char *zRelative,        /* Possibly relative input path */
  int nFull,                    /* Size of output buffer in bytes */
  char *zFull                   /* Output buffer */
){
  int rc;
  MUTEX_LOGIC( capdb_mutex *pMutex; )
  MUTEX_LOGIC( pMutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_TEMPDIR); )
  capdb_mutex_enter(pMutex);
  rc = winFullPathnameNoMutex(pVfs, zRelative, nFull, zFull);
  capdb_mutex_leave(pMutex);
  return rc;
}

#ifndef CAPDB_OMIT_LOAD_EXTENSION
/*
** Interfaces for opening a shared library, finding entry points
** within the shared library, and closing the shared library.
*/
static void *winDlOpen(capdb_vfs *pVfs, const char *zFilename){
  HANDLE h = 0;
  void *zConverted = winConvertFromUtf8Filename(zFilename);
  UNUSED_PARAMETER(pVfs);
  if( zConverted==0 ){
    OSTRACE(("DLOPEN name=%s, handle=%p\n", zFilename, (void*)0));
    return 0;
  }
  h = osLoadLibraryW ? osLoadLibraryW((LPCWSTR)zConverted) : 0;
  OSTRACE(("DLOPEN name=%s, handle=%p\n", zFilename, (void*)h));
  capdb_free(zConverted);
  return (void*)h;
}
static void winDlError(capdb_vfs *pVfs, int nBuf, char *zBufOut){
  UNUSED_PARAMETER(pVfs);
  winGetLastErrorMsg(osGetLastError(), nBuf, zBufOut);
}
static void (*winDlSym(capdb_vfs *pVfs,void *pH,const char *zSym))(void){
  FARPROC proc = 0;
  UNUSED_PARAMETER(pVfs);
  proc = osGetProcAddressA ? osGetProcAddressA((HANDLE)pH, zSym) : 0;
  OSTRACE(("DLSYM handle=%p, symbol=%s, address=%p\n",
           (void*)pH, zSym, (void*)proc));
  return (void(*)(void))proc;
}
static void winDlClose(capdb_vfs *pVfs, void *pHandle){
  UNUSED_PARAMETER(pVfs);
  if( osFreeLibrary!=0 ) osFreeLibrary((HANDLE)pHandle);
  OSTRACE(("DLCLOSE handle=%p\n", (void*)pHandle));
}
#else /* if CAPDB_OMIT_LOAD_EXTENSION is defined: */
  #define winDlOpen  0
  #define winDlError 0
  #define winDlSym   0
  #define winDlClose 0
#endif

/* State information for the randomness gatherer. */
typedef struct EntropyGatherer EntropyGatherer;
struct EntropyGatherer {
  unsigned char *a;   /* Gather entropy into this buffer */
  int na;             /* Size of a[] in bytes */
  int i;              /* XOR next input into a[i] */
  int nXor;           /* Number of XOR operations done */
};

#if !defined(CAPDB_TEST) && !defined(CAPDB_OMIT_RANDOMNESS)
/* Mix sz bytes of entropy into p. */
static void xorMemory(EntropyGatherer *p, unsigned char *x, int sz){
  int j, k;
  for(j=0, k=p->i; j<sz; j++){
    p->a[k++] ^= x[j];
    if( k>=p->na ) k = 0;
  }
  p->i = k;
  p->nXor += sz;
}
#endif /* !defined(CAPDB_TEST) && !defined(CAPDB_OMIT_RANDOMNESS) */

/*
** Write up to nBuf bytes of randomness into zBuf.
*/
static int winRandomness(capdb_vfs *pVfs, int nBuf, char *zBuf){
#if defined(CAPDB_TEST) || defined(CAPDB_OMIT_RANDOMNESS)
  UNUSED_PARAMETER(pVfs);
  memset(zBuf, 0, nBuf);
  return nBuf;
#else
  EntropyGatherer e;
  UNUSED_PARAMETER(pVfs);
  memset(zBuf, 0, nBuf);
  e.a = (unsigned char*)zBuf;
  e.na = nBuf;
  e.nXor = 0;
  e.i = 0;
  {
    FILETIME x;
    osGetSystemTimeAsFileTime(&x);
    xorMemory(&e, (unsigned char*)&x, sizeof(x));
  }
  {
    DWORD pid = osGetCurrentProcessId();
    xorMemory(&e, (unsigned char*)&pid, sizeof(pid));
  }
  {
    ULONGLONG cnt = osGetTickCount64();
    xorMemory(&e, (unsigned char*)&cnt, sizeof(cnt));
  }
  {
    LARGE_INTEGER i;
    osQueryPerformanceCounter(&i);
    xorMemory(&e, (unsigned char*)&i, sizeof(i));
  }
#if CAPDB_WIN32_USE_UUID
#ifdef CAPDB_UWP
# error CAPDB_WIN32_USE_UUID is incompatible with CAPDB_UWP
#endif
  {
    UUID id;
    memset(&id, 0, sizeof(id));
    osUuidCreate(&id);
    xorMemory(&e, (unsigned char*)&id, sizeof(id));
    memset(&id, 0, sizeof(UUID));
    osUuidCreateSequential(&id);
    xorMemory(&e, (unsigned char*)&id, sizeof(id));
  }
#endif /* CAPDB_WIN32_USE_UUID */
  return e.nXor>nBuf ? nBuf : e.nXor;
#endif /* defined(CAPDB_TEST) || defined(CAPDB_OMIT_RANDOMNESS) */
}


/*
** Sleep for a little while.  Return the amount of time slept.
*/
static int winSleep(capdb_vfs *pVfs, int microsec){
  capdb_win32_sleep((microsec+999)/1000);
  UNUSED_PARAMETER(pVfs);
  return ((microsec+999)/1000)*1000;
}

/*
** The following variable, if set to a non-zero value, is interpreted as
** the number of seconds since 1970 and is used to set the result of
** capdbOsCurrentTime() during testing.
*/
#ifdef CAPDB_TEST
int capdb_current_time = 0;  /* Fake system time in seconds since 1970. */
#endif

/*
** Find the current time (in Universal Coordinated Time).  Write into *piNow
** the current time and date as a Julian Day number times 86_400_000.  In
** other words, write into *piNow the number of milliseconds since the Julian
** epoch of noon in Greenwich on November 24, 4714 B.C according to the
** proleptic Gregorian calendar.
**
** On success, return CAPDB_OK.  Return CAPDB_ERROR if the time and date
** cannot be found.
*/
static int winCurrentTimeInt64(capdb_vfs *pVfs, capdb_int64 *piNow){
  /* FILETIME structure is a 64-bit value representing the number of
     100-nanosecond intervals since January 1, 1601 (= JD 2305813.5).
  */
  FILETIME ft;
  static const capdb_int64 winFiletimeEpoch = 23058135*(capdb_int64)8640000;
#ifdef CAPDB_TEST
  static const capdb_int64 unixEpoch = 24405875*(capdb_int64)8640000;
#endif
  /* 2^32 - to avoid use of LL and warnings in gcc */
  static const capdb_int64 max32BitValue =
      (capdb_int64)2000000000 + (capdb_int64)2000000000 +
      (capdb_int64)294967296;

  osGetSystemTimeAsFileTime( &ft );
  *piNow = winFiletimeEpoch +
            ((((capdb_int64)ft.dwHighDateTime)*max32BitValue) +
               (capdb_int64)ft.dwLowDateTime)/(capdb_int64)10000;

#ifdef CAPDB_TEST
  if( capdb_current_time ){
    *piNow = 1000*(capdb_int64)capdb_current_time + unixEpoch;
  }
#endif
  UNUSED_PARAMETER(pVfs);
  return CAPDB_OK;
}

/*
** Find the current time (in Universal Coordinated Time).  Write the
** current time and date as a Julian Day number into *prNow and
** return 0.  Return 1 if the time and date cannot be found.
*/
static int winCurrentTime(capdb_vfs *pVfs, double *prNow){
  int rc;
  capdb_int64 i;
  rc = winCurrentTimeInt64(pVfs, &i);
  if( !rc ){
    *prNow = i/86400000.0;
  }
  return rc;
}

/*
** The idea is that this function works like a combination of
** GetLastError() and FormatMessage() on Windows (or errno and
** strerror_r() on Unix). After an error is returned by an OS
** function, SQLite calls this function with zBuf pointing to
** a buffer of nBuf bytes. The OS layer should populate the
** buffer with a nul-terminated UTF-8 encoded error message
** describing the last IO error to have occurred within the calling
** thread.
**
** If the error message is too large for the supplied buffer,
** it should be truncated. The return value of xGetLastError
** is zero if the error message fits in the buffer, or non-zero
** otherwise (if the message was truncated). If non-zero is returned,
** then it is not necessary to include the nul-terminator character
** in the output buffer.
**
** Not supplying an error message will have no adverse effect
** on SQLite. It is fine to have an implementation that never
** returns an error message:
**
**   int xGetLastError(capdb_vfs *pVfs, int nBuf, char *zBuf){
**     assert(zBuf[0]=='\0');
**     return 0;
**   }
**
** However if an error message is supplied, it will be incorporated
** by sqlite into the error message available to the user using
** capdb_errmsg(), possibly making IO errors easier to debug.
*/
static int winGetLastError(capdb_vfs *pVfs, int nBuf, char *zBuf){
  DWORD e = osGetLastError();
  UNUSED_PARAMETER(pVfs);
  if( nBuf>0 ) winGetLastErrorMsg(e, nBuf, zBuf);
  return e;
}

/*
** Initialize and deinitialize the operating system interface.
*/
int capdb_os_init(void){
  static capdb_vfs winVfs = {
    3,                     /* iVersion */
    sizeof(winFile),       /* szOsFile */
    CAPDB_WIN32_MAX_PATH_BYTES, /* mxPathname */
    0,                     /* pNext */
    "win32",               /* zName */
    &winAppData,           /* pAppData */
    winOpen,               /* xOpen */
    winDelete,             /* xDelete */
    winAccess,             /* xAccess */
    winFullPathname,       /* xFullPathname */
    winDlOpen,             /* xDlOpen */
    winDlError,            /* xDlError */
    winDlSym,              /* xDlSym */
    winDlClose,            /* xDlClose */
    winRandomness,         /* xRandomness */
    winSleep,              /* xSleep */
    winCurrentTime,        /* xCurrentTime */
    winGetLastError,       /* xGetLastError */
    winCurrentTimeInt64,   /* xCurrentTimeInt64 */
    winSetSystemCall,      /* xSetSystemCall */
    winGetSystemCall,      /* xGetSystemCall */
    winNextSystemCall,     /* xNextSystemCall */
  };
  static capdb_vfs winLongPathVfs = {
    3,                     /* iVersion */
    sizeof(winFile),       /* szOsFile */
    CAPDB_WINNT_MAX_PATH_BYTES, /* mxPathname */
    0,                     /* pNext */
    "win32-longpath",      /* zName */
    &winAppData,           /* pAppData */
    winOpen,               /* xOpen */
    winDelete,             /* xDelete */
    winAccess,             /* xAccess */
    winFullPathname,       /* xFullPathname */
    winDlOpen,             /* xDlOpen */
    winDlError,            /* xDlError */
    winDlSym,              /* xDlSym */
    winDlClose,            /* xDlClose */
    winRandomness,         /* xRandomness */
    winSleep,              /* xSleep */
    winCurrentTime,        /* xCurrentTime */
    winGetLastError,       /* xGetLastError */
    winCurrentTimeInt64,   /* xCurrentTimeInt64 */
    winSetSystemCall,      /* xSetSystemCall */
    winGetSystemCall,      /* xGetSystemCall */
    winNextSystemCall,     /* xNextSystemCall */
  };
  static capdb_vfs winNolockVfs = {
    3,                     /* iVersion */
    sizeof(winFile),       /* szOsFile */
    CAPDB_WIN32_MAX_PATH_BYTES, /* mxPathname */
    0,                     /* pNext */
    "win32-none",          /* zName */
    &winNolockAppData,     /* pAppData */
    winOpen,               /* xOpen */
    winDelete,             /* xDelete */
    winAccess,             /* xAccess */
    winFullPathname,       /* xFullPathname */
    winDlOpen,             /* xDlOpen */
    winDlError,            /* xDlError */
    winDlSym,              /* xDlSym */
    winDlClose,            /* xDlClose */
    winRandomness,         /* xRandomness */
    winSleep,              /* xSleep */
    winCurrentTime,        /* xCurrentTime */
    winGetLastError,       /* xGetLastError */
    winCurrentTimeInt64,   /* xCurrentTimeInt64 */
    winSetSystemCall,      /* xSetSystemCall */
    winGetSystemCall,      /* xGetSystemCall */
    winNextSystemCall,     /* xNextSystemCall */
  };
  static capdb_vfs winLongPathNolockVfs = {
    3,                     /* iVersion */
    sizeof(winFile),       /* szOsFile */
    CAPDB_WINNT_MAX_PATH_BYTES, /* mxPathname */
    0,                     /* pNext */
    "win32-longpath-none", /* zName */
    &winNolockAppData,     /* pAppData */
    winOpen,               /* xOpen */
    winDelete,             /* xDelete */
    winAccess,             /* xAccess */
    winFullPathname,       /* xFullPathname */
    winDlOpen,             /* xDlOpen */
    winDlError,            /* xDlError */
    winDlSym,              /* xDlSym */
    winDlClose,            /* xDlClose */
    winRandomness,         /* xRandomness */
    winSleep,              /* xSleep */
    winCurrentTime,        /* xCurrentTime */
    winGetLastError,       /* xGetLastError */
    winCurrentTimeInt64,   /* xCurrentTimeInt64 */
    winSetSystemCall,      /* xSetSystemCall */
    winGetSystemCall,      /* xGetSystemCall */
    winNextSystemCall,     /* xNextSystemCall */
  };

  /* Double-check that the aSyscall[] array has been constructed
  ** correctly.  See ticket [bb3a86e890c8e96ab] */
  assert( ArraySize(aSyscall)==59 );
  assert( strcmp(aSyscall[0].zName,"AreFileApisANSI")==0 );
  assert( strcmp(aSyscall[8].zName,"FreeLibrary")==0 );
  assert( strcmp(aSyscall[16].zName,"GetSystemInfo")==0 );
  assert( strcmp(aSyscall[24].zName,"HeapReAlloc")==0 );
  assert( strcmp(aSyscall[32].zName,"MapViewOfFileFromApp")==0 );
  assert( strcmp(aSyscall[40].zName,"UnmapViewOfFile")==0 );
  assert( strcmp(aSyscall[48].zName,"UuidCreate")==0 );
  assert( strcmp(aSyscall[56].zName,"lstat")==0 );

  /* get memory map allocation granularity */
  memset(&winSysInfo, 0, sizeof(SYSTEM_INFO));
  osGetSystemInfo(&winSysInfo);
  assert( winSysInfo.dwAllocationGranularity>0 );
  assert( winSysInfo.dwPageSize>0 );

  capdb_vfs_register(&winVfs, 1);
  capdb_vfs_register(&winLongPathVfs, 0);
  capdb_vfs_register(&winNolockVfs, 0);
  capdb_vfs_register(&winLongPathNolockVfs, 0);
#ifndef CAPDB_OMIT_WAL
  winBigLock = capdbMutexAlloc(CAPDB_MUTEX_STATIC_VFS1);
#endif

  return CAPDB_OK;
}

int capdb_os_end(void){
#ifndef CAPDB_OMIT_WAL
  winBigLock = 0;
#endif
  return CAPDB_OK;
}

#endif /* CAPDB_OS_WIN */
