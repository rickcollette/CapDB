/*
** 2014 May 31
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
*/
#ifndef _FTS5INT_H
#define _FTS5INT_H

#include "fts5.h"
#include "capdbext.h"
CAPDB_EXTENSION_INIT1

#include <string.h>
#include <assert.h>
#include <stddef.h>

#ifndef CAPDB_AMALGAMATION

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef short i16;
typedef capdb_int64 i64;
typedef capdb_uint64 u64;

#ifndef ArraySize
# define ArraySize(x) ((int)(sizeof(x) / sizeof(x[0])))
#endif

#define testcase(x)

#if defined(CAPDB_COVERAGE_TEST) || defined(CAPDB_MUTATION_TEST)
# define CAPDB_OMIT_AUXILIARY_SAFETY_CHECKS 1
#endif
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

#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#define MAX(x,y) (((x) > (y)) ? (x) : (y))

/*
** Constants for the largest and smallest possible 64-bit signed integers.
*/
# define LARGEST_INT64  (0xffffffff|(((i64)0x7fffffff)<<32))
# define SMALLEST_INT64 (((i64)-1) - LARGEST_INT64)

/*
** This macro is used in a single assert() within fts5 to check that an
** allocation is aligned to an 8-byte boundary. But it is a complicated
** macro to get right for multiple platforms without generating warnings.
** So instead of reproducing the entire definition from capdbInt.h, we
** just do without this assert() for the rare non-amalgamation builds.
*/
#define EIGHT_BYTE_ALIGNMENT(x) 1

/*
** Macros needed to provide flexible arrays in a portable way
*/
#ifndef offsetof
# define offsetof(ST,M) ((size_t)((char*)&((ST*)0)->M - (char*)0))
#endif
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEXARRAY
#else
# define FLEXARRAY 1
#endif

#endif /* CAPDB_AMALGAMATION */

/*
** Constants for the largest and smallest possible 32-bit signed integers.
*/
# define LARGEST_INT32  ((int)(0x7fffffff))
# define SMALLEST_INT32 ((int)((-1) - LARGEST_INT32))

/* Truncate very long tokens to this many bytes. Hard limit is 
** (65536-1-1-4-9)==65521 bytes. The limiting factor is the 16-bit offset
** field that occurs at the start of each leaf page (see fts5_index.c). */
#define FTS5_MAX_TOKEN_SIZE 32768

/*
** Maximum number of prefix indexes on single FTS5 table. This must be
** less than 32. If it is set to anything large than that, an #error
** directive in fts5_index.c will cause the build to fail.
*/
#define FTS5_MAX_PREFIX_INDEXES 31

/*
** Maximum segments permitted in a single index 
*/
#define FTS5_MAX_SEGMENT 2000

#define FTS5_DEFAULT_NEARDIST 10
#define FTS5_DEFAULT_RANK     "bm25"

/* Name of rank and rowid columns */
#define FTS5_RANK_NAME "rank"
#define FTS5_ROWID_NAME "rowid"

#ifdef CAPDB_DEBUG
# define FTS5_CORRUPT capdbFts5Corrupt()
int capdbFts5Corrupt(void);
#else
# define FTS5_CORRUPT CAPDB_CORRUPT_VTAB
#endif

/*
** The assert_nc() macro is similar to the assert() macro, except that it
** is used for assert() conditions that are true only if it can be 
** guranteed that the database is not corrupt.
*/
#ifdef CAPDB_DEBUG
extern int capdb_fts5_may_be_corrupt;
# define assert_nc(x) assert(capdb_fts5_may_be_corrupt || (x))
#else
# define assert_nc(x) assert(x)
#endif

/*
** A version of memcmp() that does not cause asan errors if one of the pointer
** parameters is NULL and the number of bytes to compare is zero.
*/
#define fts5Memcmp(s1, s2, n) ((n)<=0 ? 0 : memcmp((s1), (s2), (n)))

/* Mark a function parameter as unused, to suppress nuisance compiler
** warnings. */
#ifndef UNUSED_PARAM
# define UNUSED_PARAM(X)  (void)(X)
#endif

#ifndef UNUSED_PARAM2
# define UNUSED_PARAM2(X, Y)  (void)(X), (void)(Y)
#endif

typedef struct Fts5Global Fts5Global;
typedef struct Fts5Colset Fts5Colset;

/* If a NEAR() clump or phrase may only match a specific set of columns, 
** then an object of the following type is used to record the set of columns.
** Each entry in the aiCol[] array is a column that may be matched.
**
** This object is used by fts5_expr.c and fts5_index.c.
*/
struct Fts5Colset {
  int nCol;
  int aiCol[FLEXARRAY];
};

/* Size (int bytes) of a complete Fts5Colset object with N columns. */
#define SZ_FTS5COLSET(N) (sizeof(i64)*((N+2)/2))

/**************************************************************************
** Interface to code in fts5_config.c. fts5_config.c contains contains code
** to parse the arguments passed to the CREATE VIRTUAL TABLE statement.
*/

typedef struct Fts5Config Fts5Config;
typedef struct Fts5TokenizerConfig Fts5TokenizerConfig;

struct Fts5TokenizerConfig {
  Fts5Tokenizer *pTok;
  fts5_tokenizer_v2 *pApi2;
  fts5_tokenizer *pApi1;
  const char **azArg;
  int nArg;
  int ePattern;                   /* FTS_PATTERN_XXX constant */
  const char *pLocale;            /* Current locale to use */
  int nLocale;                    /* Size of pLocale in bytes */
};

/*
** An instance of the following structure encodes all information that can
** be gleaned from the CREATE VIRTUAL TABLE statement.
**
** And all information loaded from the %_config table.
**
** nAutomerge:
**   The minimum number of segments that an auto-merge operation should
**   attempt to merge together. A value of 1 sets the object to use the 
**   compile time default. Zero disables auto-merge altogether.
**
** bContentlessDelete:
**   True if the contentless_delete option was present in the CREATE 
**   VIRTUAL TABLE statement.
**
** zContent:
**
** zContentRowid:
**   The value of the content_rowid= option, if one was specified. Or 
**   the string "rowid" otherwise. This text is not quoted - if it is
**   used as part of an SQL statement it needs to be quoted appropriately.
**
** zContentExprlist:
**
** pzErrmsg:
**   This exists in order to allow the fts5_index.c module to return a 
**   decent error message if it encounters a file-format version it does
**   not understand.
**
** bColumnsize:
**   True if the %_docsize table is created.
**
** bPrefixIndex:
**   This is only used for debugging. If set to false, any prefix indexes
**   are ignored. This value is configured using:
**
**       INSERT INTO tbl(tbl, rank) VALUES('prefix-index', $bPrefixIndex);
**
** bLocale:
**   Set to true if locale=1 was specified when the table was created.
*/
struct Fts5Config {
  capdb *db;                    /* Database handle */
  Fts5Global *pGlobal;            /* Global fts5 object for handle db */
  char *zDb;                      /* Database holding FTS index (e.g. "main") */
  char *zName;                    /* Name of FTS index */
  int nCol;                       /* Number of columns */
  char **azCol;                   /* Column names */
  u8 *abUnindexed;                /* True for unindexed columns */
  int nPrefix;                    /* Number of prefix indexes */
  int *aPrefix;                   /* Sizes in bytes of nPrefix prefix indexes */
  int eContent;                   /* An FTS5_CONTENT value */
  int bContentlessDelete;         /* "contentless_delete=" option (dflt==0) */
  int bContentlessUnindexed;      /* "contentless_unindexed=" option (dflt=0) */
  char *zContent;                 /* content table */ 
  char *zContentRowid;            /* "content_rowid=" option value */ 
  int bColumnsize;                /* "columnsize=" option value (dflt==1) */
  int bTokendata;                 /* "tokendata=" option value (dflt==0) */
  int bLocale;                    /* "locale=" option value (dflt==0) */
  int eDetail;                    /* FTS5_DETAIL_XXX value */
  char *zContentExprlist;
  Fts5TokenizerConfig t;
  int bLock;                      /* True when table is preparing statement */
  

  /* Values loaded from the %_config table */
  int iVersion;                   /* fts5 file format 'version' */
  int iCookie;                    /* Incremented when %_config is modified */
  int pgsz;                       /* Approximate page size used in %_data */
  int nAutomerge;                 /* 'automerge' setting */
  int nCrisisMerge;               /* Maximum allowed segments per level */
  int nUsermerge;                 /* 'usermerge' setting */
  int nHashSize;                  /* Bytes of memory for in-memory hash */
  char *zRank;                    /* Name of rank function */
  char *zRankArgs;                /* Arguments to rank function */
  int bSecureDelete;              /* 'secure-delete' */
  int nDeleteMerge;               /* 'deletemerge' */
  int bPrefixInsttoken;           /* 'prefix-insttoken' */

  /* If non-NULL, points to capdb_vtab.base.zErrmsg. Often NULL. */
  char **pzErrmsg;

#ifdef CAPDB_DEBUG
  int bPrefixIndex;               /* True to use prefix-indexes */
#endif
};

/* Current expected value of %_config table 'version' field. And
** the expected version if the 'secure-delete' option has ever been
** set on the table.  */
#define FTS5_CURRENT_VERSION               4
#define FTS5_CURRENT_VERSION_SECUREDELETE  5

#define FTS5_CONTENT_NORMAL    0
#define FTS5_CONTENT_NONE      1
#define FTS5_CONTENT_EXTERNAL  2
#define FTS5_CONTENT_UNINDEXED 3

#define FTS5_DETAIL_FULL      0
#define FTS5_DETAIL_NONE      1
#define FTS5_DETAIL_COLUMNS   2

#define FTS5_PATTERN_NONE     0
#define FTS5_PATTERN_LIKE     65  /* matches CAPDB_INDEX_CONSTRAINT_LIKE */
#define FTS5_PATTERN_GLOB     66  /* matches CAPDB_INDEX_CONSTRAINT_GLOB */

int capdbFts5ConfigParse(
    Fts5Global*, capdb*, int, const char **, Fts5Config**, char**
);
void capdbFts5ConfigFree(Fts5Config*);

int capdbFts5ConfigDeclareVtab(Fts5Config *pConfig);

int capdbFts5Tokenize(
  Fts5Config *pConfig,            /* FTS5 Configuration object */
  int flags,                      /* FTS5_TOKENIZE_* flags */
  const char *pText, int nText,   /* Text to tokenize */
  void *pCtx,                     /* Context passed to xToken() */
  int (*xToken)(void*, int, const char*, int, int, int)    /* Callback */
);

void capdbFts5Dequote(char *z);

/* Load the contents of the %_config table */
int capdbFts5ConfigLoad(Fts5Config*, int);

/* Set the value of a single config attribute */
int capdbFts5ConfigSetValue(Fts5Config*, const char*, capdb_value*, int*);

int capdbFts5ConfigParseRank(const char*, char**, char**);

void capdbFts5ConfigErrmsg(Fts5Config *pConfig, const char *zFmt, ...);

/*
** End of interface to code in fts5_config.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_buffer.c.
*/

/*
** Buffer object for the incremental building of string data.
*/
typedef struct Fts5Buffer Fts5Buffer;
struct Fts5Buffer {
  u8 *p;
  int n;
  int nSpace;
};

int capdbFts5BufferSize(int*, Fts5Buffer*, u32);
void capdbFts5BufferAppendVarint(int*, Fts5Buffer*, i64);
void capdbFts5BufferAppendBlob(int*, Fts5Buffer*, u32, const u8*);
void capdbFts5BufferAppendString(int *, Fts5Buffer*, const char*);
void capdbFts5BufferFree(Fts5Buffer*);
void capdbFts5BufferZero(Fts5Buffer*);
void capdbFts5BufferSet(int*, Fts5Buffer*, int, const u8*);
void capdbFts5BufferAppendPrintf(int *, Fts5Buffer*, char *zFmt, ...);

char *capdbFts5Mprintf(int *pRc, const char *zFmt, ...);

#define fts5BufferZero(x)             capdbFts5BufferZero(x)
#define fts5BufferAppendVarint(a,b,c) capdbFts5BufferAppendVarint(a,b,(i64)c)
#define fts5BufferFree(a)             capdbFts5BufferFree(a)
#define fts5BufferAppendBlob(a,b,c,d) capdbFts5BufferAppendBlob(a,b,c,d)
#define fts5BufferSet(a,b,c,d)        capdbFts5BufferSet(a,b,c,d)

#define fts5BufferGrow(pRc,pBuf,nn) ( \
  (u32)((pBuf)->n) + (u32)(nn) <= (u32)((pBuf)->nSpace) ? 0 : \
    capdbFts5BufferSize((pRc),(pBuf),(nn)+(pBuf)->n) \
)

/* Write and decode big-endian 32-bit integer values */
void capdbFts5Put32(u8*, int);
int capdbFts5Get32(const u8*);

#define FTS5_POS2COLUMN(iPos) (int)((iPos >> 32) & 0x7FFFFFFF)
#define FTS5_POS2OFFSET(iPos) (int)(iPos & 0x7FFFFFFF)

typedef struct Fts5PoslistReader Fts5PoslistReader;
struct Fts5PoslistReader {
  /* Variables used only by capdbFts5PoslistIterXXX() functions. */
  const u8 *a;                    /* Position list to iterate through */
  int n;                          /* Size of buffer at a[] in bytes */
  int i;                          /* Current offset in a[] */

  u8 bFlag;                       /* For client use (any custom purpose) */

  /* Output variables */
  u8 bEof;                        /* Set to true at EOF */
  i64 iPos;                       /* (iCol<<32) + iPos */
};
int capdbFts5PoslistReaderInit(
  const u8 *a, int n,             /* Poslist buffer to iterate through */
  Fts5PoslistReader *pIter        /* Iterator object to initialize */
);
int capdbFts5PoslistReaderNext(Fts5PoslistReader*);

typedef struct Fts5PoslistWriter Fts5PoslistWriter;
struct Fts5PoslistWriter {
  i64 iPrev;
};
int capdbFts5PoslistWriterAppend(Fts5Buffer*, Fts5PoslistWriter*, i64);
void capdbFts5PoslistSafeAppend(Fts5Buffer*, i64*, i64);

int capdbFts5PoslistNext64(
  const u8 *a, int n,             /* Buffer containing poslist */
  int *pi,                        /* IN/OUT: Offset within a[] */
  i64 *piOff                      /* IN/OUT: Current offset */
);

/* Malloc utility */
void *capdbFts5MallocZero(int *pRc, capdb_int64 nByte);
char *capdbFts5Strndup(int *pRc, const char *pIn, int nIn);

/* Character set tests (like isspace(), isalpha() etc.) */
int capdbFts5IsBareword(char t);


/* Bucket of terms object used by the integrity-check in offsets=0 mode. */
typedef struct Fts5Termset Fts5Termset;
int capdbFts5TermsetNew(Fts5Termset**);
int capdbFts5TermsetAdd(Fts5Termset*, int, const char*, int, int *pbPresent);
void capdbFts5TermsetFree(Fts5Termset*);

/*
** End of interface to code in fts5_buffer.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_index.c. fts5_index.c contains contains code
** to access the data stored in the %_data table.
*/

typedef struct Fts5Index Fts5Index;
typedef struct Fts5IndexIter Fts5IndexIter;

struct Fts5IndexIter {
  i64 iRowid;
  const u8 *pData;
  int nData;
  u8 bEof;
};

#define capdbFts5IterEof(x) ((x)->bEof)

/*
** Values used as part of the flags argument passed to IndexQuery().
*/
#define FTS5INDEX_QUERY_PREFIX      0x0001  /* Prefix query */
#define FTS5INDEX_QUERY_DESC        0x0002  /* Docs in descending rowid order */
#define FTS5INDEX_QUERY_TEST_NOIDX  0x0004  /* Do not use prefix index */
#define FTS5INDEX_QUERY_SCAN        0x0008  /* Scan query (fts5vocab) */

/* The following are used internally by the fts5_index.c module. They are
** defined here only to make it easier to avoid clashes with the flags
** above. */
#define FTS5INDEX_QUERY_SKIPEMPTY   0x0010
#define FTS5INDEX_QUERY_NOOUTPUT    0x0020
#define FTS5INDEX_QUERY_SKIPHASH    0x0040
#define FTS5INDEX_QUERY_NOTOKENDATA 0x0080
#define FTS5INDEX_QUERY_SCANONETERM 0x0100

/*
** Create/destroy an Fts5Index object.
*/
int capdbFts5IndexOpen(Fts5Config *pConfig, int bCreate, Fts5Index**, char**);
int capdbFts5IndexClose(Fts5Index *p);

/*
** Return a simple checksum value based on the arguments.
*/
u64 capdbFts5IndexEntryCksum(
  i64 iRowid, 
  int iCol, 
  int iPos, 
  int iIdx,
  const char *pTerm,
  int nTerm
);

/*
** Argument p points to a buffer containing utf-8 text that is n bytes in 
** size. Return the number of bytes in the nChar character prefix of the
** buffer, or 0 if there are less than nChar characters in total.
*/
int capdbFts5IndexCharlenToBytelen(
  const char *p, 
  int nByte, 
  int nChar
);

/*
** Open a new iterator to iterate though all rowids that match the 
** specified token or token prefix.
*/
int capdbFts5IndexQuery(
  Fts5Index *p,                   /* FTS index to query */
  const char *pToken, int nToken, /* Token (or prefix) to query for */
  int flags,                      /* Mask of FTS5INDEX_QUERY_X flags */
  Fts5Colset *pColset,            /* Match these columns only */
  Fts5IndexIter **ppIter          /* OUT: New iterator object */
);

/*
** The various operations on open token or token prefix iterators opened
** using capdbFts5IndexQuery().
*/
int capdbFts5IterNext(Fts5IndexIter*);
int capdbFts5IterNextFrom(Fts5IndexIter*, i64 iMatch);

/*
** Close an iterator opened by capdbFts5IndexQuery().
*/
void capdbFts5IterClose(Fts5IndexIter*);

/*
** Close the reader blob handle, if it is open.
*/
void capdbFts5IndexCloseReader(Fts5Index*);

/*
** This interface is used by the fts5vocab module.
*/
const char *capdbFts5IterTerm(Fts5IndexIter*, int*);
int capdbFts5IterNextScan(Fts5IndexIter*);
void *capdbFts5StructureRef(Fts5Index*);
void capdbFts5StructureRelease(void*);
int capdbFts5StructureTest(Fts5Index*, void*);

/*
** Used by xInstToken():
*/
int capdbFts5IterToken(
  Fts5IndexIter *pIndexIter, 
  const char *pToken, int nToken,
  i64 iRowid,
  int iCol, 
  int iOff, 
  const char **ppOut, int *pnOut
);

/*
** Insert or remove data to or from the index. Each time a document is 
** added to or removed from the index, this function is called one or more
** times.
**
** For an insert, it must be called once for each token in the new document.
** If the operation is a delete, it must be called (at least) once for each
** unique token in the document with an iCol value less than zero. The iPos
** argument is ignored for a delete.
*/
int capdbFts5IndexWrite(
  Fts5Index *p,                   /* Index to write to */
  int iCol,                       /* Column token appears in (-ve -> delete) */
  int iPos,                       /* Position of token within column */
  const char *pToken, int nToken  /* Token to add or remove to or from index */
);

/*
** Indicate that subsequent calls to capdbFts5IndexWrite() pertain to
** document iDocid.
*/
int capdbFts5IndexBeginWrite(
  Fts5Index *p,                   /* Index to write to */
  int bDelete,                    /* True if current operation is a delete */
  i64 iDocid                      /* Docid to add or remove data from */
);

/*
** Flush any data stored in the in-memory hash tables to the database.
** Also close any open blob handles.
*/
int capdbFts5IndexSync(Fts5Index *p);

/*
** Discard any data stored in the in-memory hash tables. Do not write it
** to the database. Additionally, assume that the contents of the %_data
** table may have changed on disk. So any in-memory caches of %_data 
** records must be invalidated.
*/
int capdbFts5IndexRollback(Fts5Index *p);

/*
** Get or set the "averages" values.
*/
int capdbFts5IndexGetAverages(Fts5Index *p, i64 *pnRow, i64 *anSize);
int capdbFts5IndexSetAverages(Fts5Index *p, const u8*, int);

/*
** Functions called by the storage module as part of integrity-check.
*/
int capdbFts5IndexIntegrityCheck(Fts5Index*, u64 cksum, int bUseCksum);

/* 
** Called during virtual module initialization to register UDF 
** fts5_decode() with SQLite 
*/
int capdbFts5IndexInit(capdb*);

int capdbFts5IndexSetCookie(Fts5Index*, int);

/*
** Return the total number of entries read from the %_data table by 
** this connection since it was created.
*/
int capdbFts5IndexReads(Fts5Index *p);

int capdbFts5IndexReinit(Fts5Index *p);
int capdbFts5IndexOptimize(Fts5Index *p);
int capdbFts5IndexMerge(Fts5Index *p, int nMerge);
int capdbFts5IndexReset(Fts5Index *p);

int capdbFts5IndexLoadConfig(Fts5Index *p);

int capdbFts5IndexGetOrigin(Fts5Index *p, i64 *piOrigin);
int capdbFts5IndexContentlessDelete(Fts5Index *p, i64 iOrigin, i64 iRowid);

void capdbFts5IndexIterClearTokendata(Fts5IndexIter*);

/* Used to populate hash tables for xInstToken in detail=none/column mode. */
int capdbFts5IndexIterWriteTokendata(
    Fts5IndexIter*, const char*, int, i64 iRowid, int iCol, int iOff
);

/*
** End of interface to code in fts5_index.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_varint.c. 
*/
int capdbFts5GetVarint32(const unsigned char *p, u32 *v);
int capdbFts5GetVarintLen(u32 iVal);
u8 capdbFts5GetVarint(const unsigned char*, u64*);
int capdbFts5PutVarint(unsigned char *p, u64 v);

#define fts5GetVarint32(a,b) capdbFts5GetVarint32(a,(u32*)&(b))
#define fts5GetVarint    capdbFts5GetVarint

#define fts5FastGetVarint32(a, iOff, nVal) {      \
  nVal = (a)[iOff++];                             \
  if( nVal & 0x80 ){                              \
    iOff--;                                       \
    iOff += fts5GetVarint32(&(a)[iOff], nVal);    \
  }                                               \
}


/*
** End of interface to code in fts5_varint.c.
**************************************************************************/


/**************************************************************************
** Interface to code in fts5_main.c. 
*/

/*
** Virtual-table object.
*/
typedef struct Fts5Table Fts5Table;
struct Fts5Table {
  capdb_vtab base;              /* Base class used by SQLite core */
  Fts5Config *pConfig;            /* Virtual table configuration */
  Fts5Index *pIndex;              /* Full-text index */
};

int capdbFts5LoadTokenizer(Fts5Config *pConfig);

Fts5Table *capdbFts5TableFromCsrid(Fts5Global*, i64);

int capdbFts5FlushToDisk(Fts5Table*);

void capdbFts5ClearLocale(Fts5Config *pConfig);
void capdbFts5SetLocale(Fts5Config *pConfig, const char *pLoc, int nLoc);

int capdbFts5IsLocaleValue(Fts5Config *pConfig, capdb_value *pVal);
int capdbFts5DecodeLocaleValue(capdb_value *pVal, 
    const char **ppText, int *pnText, const char **ppLoc, int *pnLoc
);

/*
** End of interface to code in fts5.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_hash.c. 
*/
typedef struct Fts5Hash Fts5Hash;

/*
** Create a hash table, free a hash table.
*/
int capdbFts5HashNew(Fts5Config*, Fts5Hash**, int *pnSize);
void capdbFts5HashFree(Fts5Hash*);

int capdbFts5HashWrite(
  Fts5Hash*,
  i64 iRowid,                     /* Rowid for this entry */
  int iCol,                       /* Column token appears in (-ve -> delete) */
  int iPos,                       /* Position of token within column */
  char bByte,
  const char *pToken, int nToken  /* Token to add or remove to or from index */
);

/*
** Empty (but do not delete) a hash table.
*/
void capdbFts5HashClear(Fts5Hash*);

/*
** Return true if the hash is empty, false otherwise.
*/
int capdbFts5HashIsEmpty(Fts5Hash*);

int capdbFts5HashQuery(
  Fts5Hash*,                      /* Hash table to query */
  int nPre,
  const char *pTerm, int nTerm,   /* Query term */
  void **ppObj,                   /* OUT: Pointer to doclist for pTerm */
  int *pnDoclist                  /* OUT: Size of doclist in bytes */
);

int capdbFts5HashScanInit(
  Fts5Hash*,                      /* Hash table to query */
  const char *pTerm, int nTerm    /* Query prefix */
);
void capdbFts5HashScanNext(Fts5Hash*);
int capdbFts5HashScanEof(Fts5Hash*);
void capdbFts5HashScanEntry(Fts5Hash *,
  const char **pzTerm,            /* OUT: term (nul-terminated) */
  int *pnTerm,                    /* OUT: Size of term in bytes */
  const u8 **ppDoclist,           /* OUT: pointer to doclist */
  int *pnDoclist                  /* OUT: size of doclist in bytes */
);



/*
** End of interface to code in fts5_hash.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_storage.c. fts5_storage.c contains contains 
** code to access the data stored in the %_content and %_docsize tables.
*/

#define FTS5_STMT_SCAN_ASC  0     /* SELECT rowid, * FROM ... ORDER BY 1 ASC */
#define FTS5_STMT_SCAN_DESC 1     /* SELECT rowid, * FROM ... ORDER BY 1 DESC */
#define FTS5_STMT_LOOKUP    2     /* SELECT rowid, * FROM ... WHERE rowid=? */

typedef struct Fts5Storage Fts5Storage;

int capdbFts5StorageOpen(Fts5Config*, Fts5Index*, int, Fts5Storage**, char**);
int capdbFts5StorageClose(Fts5Storage *p);
int capdbFts5StorageRename(Fts5Storage*, const char *zName);

int capdbFts5DropAll(Fts5Config*);
int capdbFts5CreateTable(Fts5Config*, const char*, const char*, int, char **);

int capdbFts5StorageDelete(Fts5Storage *p, i64, capdb_value**, int);
int capdbFts5StorageContentInsert(Fts5Storage *p, int, capdb_value**, i64*);
int capdbFts5StorageIndexInsert(Fts5Storage *p, capdb_value**, i64);

int capdbFts5StorageIntegrity(Fts5Storage *p, int iArg);

int capdbFts5StorageStmt(Fts5Storage *p, int eStmt, capdb_stmt**, char**);
void capdbFts5StorageStmtRelease(Fts5Storage *p, int eStmt, capdb_stmt*);

int capdbFts5StorageDocsize(Fts5Storage *p, i64 iRowid, int *aCol);
int capdbFts5StorageSize(Fts5Storage *p, int iCol, i64 *pnAvg);
int capdbFts5StorageRowCount(Fts5Storage *p, i64 *pnRow);

int capdbFts5StorageSync(Fts5Storage *p);
int capdbFts5StorageRollback(Fts5Storage *p);

int capdbFts5StorageConfigValue(
    Fts5Storage *p, const char*, capdb_value*, int
);

int capdbFts5StorageDeleteAll(Fts5Storage *p);
int capdbFts5StorageRebuild(Fts5Storage *p);
int capdbFts5StorageOptimize(Fts5Storage *p);
int capdbFts5StorageMerge(Fts5Storage *p, int nMerge);
int capdbFts5StorageReset(Fts5Storage *p);

void capdbFts5StorageReleaseDeleteRow(Fts5Storage*);
int capdbFts5StorageFindDeleteRow(Fts5Storage *p, i64 iDel);

/*
** End of interface to code in fts5_storage.c.
**************************************************************************/


/**************************************************************************
** Interface to code in fts5_expr.c. 
*/
typedef struct Fts5Expr Fts5Expr;
typedef struct Fts5ExprNode Fts5ExprNode;
typedef struct Fts5Parse Fts5Parse;
typedef struct Fts5Token Fts5Token;
typedef struct Fts5ExprPhrase Fts5ExprPhrase;
typedef struct Fts5ExprNearset Fts5ExprNearset;

struct Fts5Token {
  const char *p;                  /* Token text (not NULL terminated) */
  int n;                          /* Size of buffer p in bytes */
};

/* Parse a MATCH expression. */
int capdbFts5ExprNew(
  Fts5Config *pConfig, 
  int bPhraseToAnd,
  int iCol,                       /* Column on LHS of MATCH operator */
  const char *zExpr,
  Fts5Expr **ppNew, 
  char **pzErr
);
int capdbFts5ExprPattern(
  Fts5Config *pConfig, 
  int bGlob, 
  int iCol, 
  const char *zText, 
  Fts5Expr **pp
);

/*
** for(rc = capdbFts5ExprFirst(pExpr, pIdx, bDesc);
**     rc==CAPDB_OK && 0==capdbFts5ExprEof(pExpr);
**     rc = capdbFts5ExprNext(pExpr)
** ){
**   // The document with rowid iRowid matches the expression!
**   i64 iRowid = capdbFts5ExprRowid(pExpr);
** }
*/
int capdbFts5ExprFirst(Fts5Expr*, Fts5Index *pIdx, i64 iMin, i64, int bDesc);
int capdbFts5ExprNext(Fts5Expr*, i64 iMax);
int capdbFts5ExprEof(Fts5Expr*);
i64 capdbFts5ExprRowid(Fts5Expr*);

void capdbFts5ExprFree(Fts5Expr*);
int capdbFts5ExprAnd(Fts5Expr **pp1, Fts5Expr *p2);

/* Called during startup to register a UDF with SQLite */
int capdbFts5ExprInit(Fts5Global*, capdb*);

int capdbFts5ExprPhraseCount(Fts5Expr*);
int capdbFts5ExprPhraseSize(Fts5Expr*, int iPhrase);
int capdbFts5ExprPoslist(Fts5Expr*, int, const u8 **);

typedef struct Fts5PoslistPopulator Fts5PoslistPopulator;
Fts5PoslistPopulator *capdbFts5ExprClearPoslists(Fts5Expr*, int);
int capdbFts5ExprPopulatePoslists(
    Fts5Config*, Fts5Expr*, Fts5PoslistPopulator*, int, const char*, int
);
void capdbFts5ExprCheckPoslists(Fts5Expr*, i64);

int capdbFts5ExprClonePhrase(Fts5Expr*, int, Fts5Expr**);

int capdbFts5ExprPhraseCollist(Fts5Expr *, int, const u8 **, int *);

int capdbFts5ExprQueryToken(Fts5Expr*, int, int, const char**, int*);
int capdbFts5ExprInstToken(Fts5Expr*, i64, int, int, int, int, const char**, int*);
void capdbFts5ExprClearTokens(Fts5Expr*);

/*******************************************
** The fts5_expr.c API above this point is used by the other hand-written
** C code in this module. The interfaces below this point are called by
** the parser code in fts5parse.y.  */

void capdbFts5ParseError(Fts5Parse *pParse, const char *zFmt, ...);

Fts5ExprNode *capdbFts5ParseNode(
  Fts5Parse *pParse,
  int eType,
  Fts5ExprNode *pLeft,
  Fts5ExprNode *pRight,
  Fts5ExprNearset *pNear
);

Fts5ExprNode *capdbFts5ParseImplicitAnd(
  Fts5Parse *pParse,
  Fts5ExprNode *pLeft,
  Fts5ExprNode *pRight
);

Fts5ExprPhrase *capdbFts5ParseTerm(
  Fts5Parse *pParse, 
  Fts5ExprPhrase *pPhrase, 
  Fts5Token *pToken,
  int bPrefix
);

void capdbFts5ParseSetCaret(Fts5ExprPhrase*);

Fts5ExprNearset *capdbFts5ParseNearset(
  Fts5Parse*, 
  Fts5ExprNearset*,
  Fts5ExprPhrase* 
);

Fts5Colset *capdbFts5ParseColset(
  Fts5Parse*, 
  Fts5Colset*, 
  Fts5Token *
);

void capdbFts5ParsePhraseFree(Fts5ExprPhrase*);
void capdbFts5ParseNearsetFree(Fts5ExprNearset*);
void capdbFts5ParseNodeFree(Fts5ExprNode*);

void capdbFts5ParseSetDistance(Fts5Parse*, Fts5ExprNearset*, Fts5Token*);
void capdbFts5ParseSetColset(Fts5Parse*, Fts5ExprNode*, Fts5Colset*);
Fts5Colset *capdbFts5ParseColsetInvert(Fts5Parse*, Fts5Colset*);
void capdbFts5ParseFinished(Fts5Parse *pParse, Fts5ExprNode *p);
void capdbFts5ParseNear(Fts5Parse *pParse, Fts5Token*);

/*
** End of interface to code in fts5_expr.c.
**************************************************************************/



/**************************************************************************
** Interface to code in fts5_aux.c. 
*/

int capdbFts5AuxInit(fts5_api*);
/*
** End of interface to code in fts5_aux.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_tokenizer.c. 
*/

int capdbFts5TokenizerInit(fts5_api*);
int capdbFts5TokenizerPattern(
    int (*xCreate)(void*, const char**, int, Fts5Tokenizer**),
    Fts5Tokenizer *pTok
);
int capdbFts5TokenizerPreload(Fts5TokenizerConfig*);
/*
** End of interface to code in fts5_tokenizer.c.
**************************************************************************/

/**************************************************************************
** Interface to code in fts5_vocab.c. 
*/

int capdbFts5VocabInit(Fts5Global*, capdb*);

/*
** End of interface to code in fts5_vocab.c.
**************************************************************************/


/**************************************************************************
** Interface to automatically generated code in fts5_unicode2.c. 
*/
int capdbFts5UnicodeIsdiacritic(int c);
int capdbFts5UnicodeFold(int c, int bRemoveDiacritic);

int capdbFts5UnicodeCatParse(const char*, u8*);
int capdbFts5UnicodeCategory(u32 iCode);
void capdbFts5UnicodeAscii(u8*, u8*);
/*
** End of interface to code in fts5_unicode2.c.
**************************************************************************/

#endif
