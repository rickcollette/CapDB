/*
** 2013 Jan 11
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the virtual table interfaces.  This code
** is not included in the SQLite library.  It is used for automated
** testing of the SQLite library.
**
** The FS virtual table is created as follows:
**
**   CREATE VIRTUAL TABLE tbl USING fs(idx);
**
** where idx is the name of a table in the db with 2 columns.  The virtual
** table also has two columns - file path and file contents.
**
** The first column of table idx must be an IPK, and the second contains file
** paths. For example:
**
**   CREATE TABLE idx(id INTEGER PRIMARY KEY, path TEXT);
**   INSERT INTO idx VALUES(4, '/etc/passwd');
**
** Adding the row to the idx table automatically creates a row in the 
** virtual table with rowid=4, path=/etc/passwd and a text field that 
** contains data read from file /etc/passwd on disk.
**
*************************************************************************
** Virtual table module "fsdir"
**
** This module is designed to be used as a read-only eponymous virtual table.
** Its schema is as follows:
**
**   CREATE TABLE fsdir(dir TEXT, name TEXT);
**
** When queried, a WHERE term of the form "dir = $dir" must be provided. The
** virtual table then appears to have one row for each entry in file-system
** directory $dir. Column dir contains a copy of $dir, and column "name"
** contains the name of the directory entry.
**
** If the specified $dir cannot be opened or is not a directory, it is not
** an error. The virtual table appears to be empty in this case.
**
*************************************************************************
** Virtual table module "fstree"
**
** This module is also a read-only eponymous virtual table with the 
** following schema:
**
**   CREATE TABLE fstree(path TEXT, size INT, data BLOB);
**
** Running a "SELECT * FROM fstree" query on this table returns the entire
** contents of the file-system, starting at "/". To restrict the search
** space, the virtual table supports LIKE and GLOB constraints on the
** 'path' column. For example:
**
**   SELECT * FROM fstree WHERE path LIKE '/home/dan/sqlite/%'
*/
#include "capdbInt.h"
#include "tclsqlite.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if !defined(_WIN32) || defined(__MSVCRT__)
# include <unistd.h>
# include <dirent.h>
#else
# include "windirent.h"
# ifndef S_ISREG
#  define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
# endif
#endif

#ifndef CAPDB_OMIT_VIRTUALTABLE

typedef struct fs_vtab fs_vtab;
typedef struct fs_cursor fs_cursor;

/* 
** A fs virtual-table object 
*/
struct fs_vtab {
  capdb_vtab base;
  capdb *db;
  char *zDb;                      /* Name of db containing zTbl */
  char *zTbl;                     /* Name of docid->file map table */
};

/* A fs cursor object */
struct fs_cursor {
  capdb_vtab_cursor base;
  capdb_stmt *pStmt;
  char *zBuf;
  int nBuf;
  int nAlloc;
};

/*************************************************************************
** Start of fsdir implementation.
*/
typedef struct FsdirVtab FsdirVtab;
typedef struct FsdirCsr FsdirCsr;
struct FsdirVtab {
  capdb_vtab base;
};

struct FsdirCsr {
  capdb_vtab_cursor base;
  char *zDir;                     /* Buffer containing directory scanned */
  DIR *pDir;                      /* Open directory */
  capdb_int64 iRowid;
  struct dirent *pEntry;
};

/*
** This function is the implementation of both the xConnect and xCreate
** methods of the fsdir virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int fsdirConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  FsdirVtab *pTab;

  if( argc!=3 ){
    *pzErr = capdb_mprintf("wrong number of arguments");
    return CAPDB_ERROR;
  }

  pTab = (FsdirVtab *)capdb_malloc(sizeof(FsdirVtab));
  if( !pTab ) return CAPDB_NOMEM;
  memset(pTab, 0, sizeof(FsdirVtab));

  *ppVtab = &pTab->base;
  capdb_declare_vtab(db, "CREATE TABLE xyz(dir, name);");

  return CAPDB_OK;
}

/*
** xDestroy/xDisconnect implementation.
*/
static int fsdirDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** xBestIndex implementation. The only constraint supported is:
**
**   (dir = ?)
*/
static int fsdirBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  int ii;

  pIdxInfo->estimatedCost = 1000000000.0;

  for(ii=0; ii<pIdxInfo->nConstraint; ii++){
    struct capdb_index_constraint const *p = &pIdxInfo->aConstraint[ii];
    if( p->iColumn==0 && p->usable && p->op==CAPDB_INDEX_CONSTRAINT_EQ ){
      struct capdb_index_constraint_usage *pUsage;
      pUsage = &pIdxInfo->aConstraintUsage[ii];
      pUsage->omit = 1;
      pUsage->argvIndex = 1;
      pIdxInfo->idxNum = 1;
      pIdxInfo->estimatedCost = 1.0;
      break;
    }
  }

  return CAPDB_OK;
}

/*
** xOpen implementation.
**
** Open a new fsdir cursor.
*/
static int fsdirOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  FsdirCsr *pCur;
  /* Allocate an extra 256 bytes because it is undefined how big dirent.d_name
  ** is and we need enough space.  Linux provides plenty already, but
  ** Solaris only provides one byte. */
  pCur = (FsdirCsr*)capdb_malloc(sizeof(FsdirCsr)+256);
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(FsdirCsr));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Close a fsdir cursor.
*/
static int fsdirClose(capdb_vtab_cursor *cur){
  FsdirCsr *pCur = (FsdirCsr*)cur;
  if( pCur->pDir ) closedir(pCur->pDir);
  capdb_free(pCur->zDir);
  capdb_free(pCur);
  return CAPDB_OK;
}

/*
** Skip the cursor to the next entry.
*/
static int fsdirNext(capdb_vtab_cursor *cur){
  FsdirCsr *pCsr = (FsdirCsr*)cur;

  if( pCsr->pDir ){
    pCsr->pEntry = readdir(pCsr->pDir);
    if( pCsr->pEntry==0 ){
      closedir(pCsr->pDir);
      pCsr->pDir = 0;
    }
    pCsr->iRowid++;
  }

  return CAPDB_OK;
}

/*
** xFilter method implementation.
*/
static int fsdirFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  FsdirCsr *pCsr = (FsdirCsr*)pVtabCursor;
  const char *zDir;
  int nDir;


  if( idxNum!=1 || argc!=1 ){
    return CAPDB_ERROR;
  }

  pCsr->iRowid = 0;
  capdb_free(pCsr->zDir);
  if( pCsr->pDir ){
    closedir(pCsr->pDir);
    pCsr->pDir = 0;
  }

  zDir = (const char*)capdb_value_text(argv[0]);
  nDir = capdb_value_bytes(argv[0]);
  pCsr->zDir = capdb_malloc(nDir+1);
  if( pCsr->zDir==0 ) return CAPDB_NOMEM;
  memcpy(pCsr->zDir, zDir, nDir+1);

  pCsr->pDir = opendir(pCsr->zDir);
  return fsdirNext(pVtabCursor); 
}

/*
** xEof method implementation.
*/
static int fsdirEof(capdb_vtab_cursor *cur){
  FsdirCsr *pCsr = (FsdirCsr*)cur;
  return pCsr->pDir==0;
}

/*
** xColumn method implementation.
*/
static int fsdirColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  FsdirCsr *pCsr = (FsdirCsr*)cur;
  switch( i ){
    case 0: /* dir */
      capdb_result_text(ctx, pCsr->zDir, -1, CAPDB_STATIC);
      break;

    case 1: /* name */
      capdb_result_text(ctx, pCsr->pEntry->d_name, -1, CAPDB_TRANSIENT);
      break;

    default:
      assert( 0 );
  }

  return CAPDB_OK;
}

/*
** xRowid method implementation.
*/
static int fsdirRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  FsdirCsr *pCsr = (FsdirCsr*)cur;
  *pRowid = pCsr->iRowid;
  return CAPDB_OK;
}
/*
** End of fsdir implementation.
*************************************************************************/

/*************************************************************************
** Start of fstree implementation.
*/
typedef struct FstreeVtab FstreeVtab;
typedef struct FstreeCsr FstreeCsr;
struct FstreeVtab {
  capdb_vtab base;
  capdb *db;
};

struct FstreeCsr {
  capdb_vtab_cursor base;
  capdb_stmt *pStmt;            /* Statement to list paths */
  int fd;                         /* File descriptor open on current path */
};

/*
** This function is the implementation of both the xConnect and xCreate
** methods of the fstree virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int fstreeConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  FstreeVtab *pTab;

  if( argc!=3 ){
    *pzErr = capdb_mprintf("wrong number of arguments");
    return CAPDB_ERROR;
  }

  pTab = (FstreeVtab *)capdb_malloc(sizeof(FstreeVtab));
  if( !pTab ) return CAPDB_NOMEM;
  memset(pTab, 0, sizeof(FstreeVtab));
  pTab->db = db;

  *ppVtab = &pTab->base;
  capdb_declare_vtab(db, "CREATE TABLE xyz(path, size, data);");

  return CAPDB_OK;
}

/*
** xDestroy/xDisconnect implementation.
*/
static int fstreeDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}

/*
** xBestIndex implementation. The only constraint supported is:
**
**   (dir = ?)
*/
static int fstreeBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  int ii;

  for(ii=0; ii<pIdxInfo->nConstraint; ii++){
    struct capdb_index_constraint const *p = &pIdxInfo->aConstraint[ii];
    if( p->iColumn==0 && p->usable && (
          p->op==CAPDB_INDEX_CONSTRAINT_GLOB
       || p->op==CAPDB_INDEX_CONSTRAINT_LIKE
       || p->op==CAPDB_INDEX_CONSTRAINT_EQ
    )){
      struct capdb_index_constraint_usage *pUsage;
      pUsage = &pIdxInfo->aConstraintUsage[ii];
      pIdxInfo->idxNum = p->op;
      pUsage->argvIndex = 1;
      pIdxInfo->estimatedCost = 100000.0;
      return CAPDB_OK;
    }
  }

  pIdxInfo->estimatedCost = 1000000000.0;
  return CAPDB_OK;
}

/*
** xOpen implementation.
**
** Open a new fstree cursor.
*/
static int fstreeOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  FstreeCsr *pCur;
  pCur = (FstreeCsr*)capdb_malloc(sizeof(FstreeCsr));
  if( pCur==0 ) return CAPDB_NOMEM;
  memset(pCur, 0, sizeof(FstreeCsr));
  pCur->fd = -1;
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

static void fstreeCloseFd(FstreeCsr *pCsr){
  if( pCsr->fd>=0 ){
    close(pCsr->fd);
    pCsr->fd = -1;
  }
}

/*
** Close a fstree cursor.
*/
static int fstreeClose(capdb_vtab_cursor *cur){
  FstreeCsr *pCsr = (FstreeCsr*)cur;
  capdb_finalize(pCsr->pStmt);
  fstreeCloseFd(pCsr);
  capdb_free(pCsr);
  return CAPDB_OK;
}

/*
** Skip the cursor to the next entry.
*/
static int fstreeNext(capdb_vtab_cursor *cur){
  FstreeCsr *pCsr = (FstreeCsr*)cur;
  int rc;

  fstreeCloseFd(pCsr);
  rc = capdb_step(pCsr->pStmt);
  if( rc!=CAPDB_ROW ){
    rc = capdb_finalize(pCsr->pStmt);
    pCsr->pStmt = 0;
  }else{
    rc = CAPDB_OK;
    pCsr->fd = open((const char*)capdb_column_text(pCsr->pStmt, 0), O_RDONLY);
  }

  return rc;
}

/*
** xFilter method implementation.
*/
static int fstreeFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  FstreeCsr *pCsr = (FstreeCsr*)pVtabCursor;
  FstreeVtab *pTab = (FstreeVtab*)(pCsr->base.pVtab);
  int rc;
  const char *zSql = 
"WITH r(d) AS ("
"  SELECT CASE WHEN dir=?2 THEN ?3 ELSE dir END || '/' || name "
"    FROM fsdir WHERE dir=?1 AND name NOT LIKE '.%'"
"  UNION ALL"
"  SELECT dir || '/' || name FROM r, fsdir WHERE dir=d AND name NOT LIKE '.%'"
") SELECT d FROM r;";

  char *zRoot;
  int nRoot;
  char *zPrefix;
  int nPrefix;
  const char *zDir;
  int nDir;
  char aWild[2] = { '\0', '\0' };

#ifdef _WIN32
  const char *zDrive = getenv("fstreeDrive");
  if( zDrive==0 ){
    zDrive = getenv("SystemDrive");
  }
  zRoot = capdb_mprintf("%s%c", zDrive, '/');
  nRoot = capdbStrlen30(zRoot);
  zPrefix = capdb_mprintf("%s", zDrive);
  nPrefix = capdbStrlen30(zPrefix);
#else
  zRoot = "/";
  nRoot = 1;
  zPrefix = "";
  nPrefix = 0;
#endif

  zDir = zRoot;
  nDir = nRoot;

  fstreeCloseFd(pCsr);
  capdb_finalize(pCsr->pStmt);
  pCsr->pStmt = 0;
  rc = capdb_prepare_v2(pTab->db, zSql, -1, &pCsr->pStmt, 0);
  if( rc!=CAPDB_OK ) return rc;

  if( idxNum ){
    const char *zQuery = (const char*)capdb_value_text(argv[0]);
    switch( idxNum ){
      case CAPDB_INDEX_CONSTRAINT_GLOB:
        aWild[0] = '*';
        aWild[1] = '?';
        break;
      case CAPDB_INDEX_CONSTRAINT_LIKE:
        aWild[0] = '_';
        aWild[1] = '%';
        break;
    }

    if( capdb_strnicmp(zQuery, zPrefix, nPrefix)==0 ){
      int i;
      for(i=nPrefix; zQuery[i]; i++){
        if( zQuery[i]==aWild[0] || zQuery[i]==aWild[1] ) break;
        if( zQuery[i]=='/' ) nDir = i;
      }
      zDir = zQuery;
    }
  }
  if( nDir==0 ) nDir = 1;

  capdb_bind_text(pCsr->pStmt, 1, zDir, nDir, CAPDB_TRANSIENT);
  capdb_bind_text(pCsr->pStmt, 2, zRoot, nRoot, CAPDB_TRANSIENT);
  capdb_bind_text(pCsr->pStmt, 3, zPrefix, nPrefix, CAPDB_TRANSIENT);

#ifdef _WIN32
  capdb_free(zPrefix);
  capdb_free(zRoot);
#endif

  return fstreeNext(pVtabCursor); 
}

/*
** xEof method implementation.
*/
static int fstreeEof(capdb_vtab_cursor *cur){
  FstreeCsr *pCsr = (FstreeCsr*)cur;
  return pCsr->pStmt==0;
}

/*
** xColumn method implementation.
*/
static int fstreeColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  FstreeCsr *pCsr = (FstreeCsr*)cur;
  if( i==0 ){      /* path */
    capdb_result_value(ctx, capdb_column_value(pCsr->pStmt, 0));
  }else{
    struct stat sBuf;
    fstat(pCsr->fd, &sBuf);

    if( S_ISREG(sBuf.st_mode) ){
      if( i==1 ){
        capdb_result_int64(ctx, sBuf.st_size);
      }else{
        int nRead;
        char *aBuf = capdb_malloc(sBuf.st_mode+1);
        if( !aBuf ) return CAPDB_NOMEM;
        nRead = read(pCsr->fd, aBuf, sBuf.st_mode);
        if( nRead!=sBuf.st_mode ){
          return CAPDB_IOERR;
        }
        capdb_result_blob(ctx, aBuf, nRead, CAPDB_TRANSIENT);
        capdb_free(aBuf);
      }
    }
  }

  return CAPDB_OK;
}

/*
** xRowid method implementation.
*/
static int fstreeRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  *pRowid = 0;
  return CAPDB_OK;
}
/*
** End of fstree implementation.
*************************************************************************/




/*
** This function is the implementation of both the xConnect and xCreate
** methods of the fs virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fs")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> other module argument fields.
*/
static int fsConnect(
  capdb *db,
  void *pAux,
  int argc, const char *const*argv,
  capdb_vtab **ppVtab,
  char **pzErr
){
  fs_vtab *pVtab;
  int nByte;
  const char *zTbl;
  const char *zDb = argv[1];

  if( argc!=4 ){
    *pzErr = capdb_mprintf("wrong number of arguments");
    return CAPDB_ERROR;
  }
  zTbl = argv[3];

  nByte = sizeof(fs_vtab) + (int)strlen(zTbl) + 1 + (int)strlen(zDb) + 1;
  pVtab = (fs_vtab *)capdbMallocZero( nByte );
  if( !pVtab ) return CAPDB_NOMEM;

  pVtab->zTbl = (char *)&pVtab[1];
  pVtab->zDb = &pVtab->zTbl[strlen(zTbl)+1];
  pVtab->db = db;
  memcpy(pVtab->zTbl, zTbl, strlen(zTbl));
  memcpy(pVtab->zDb, zDb, strlen(zDb));
  *ppVtab = &pVtab->base;
  capdb_declare_vtab(db, "CREATE TABLE x(path TEXT, data TEXT)");

  return CAPDB_OK;
}
/* Note that for this virtual table, the xCreate and xConnect
** methods are identical. */

static int fsDisconnect(capdb_vtab *pVtab){
  capdb_free(pVtab);
  return CAPDB_OK;
}
/* The xDisconnect and xDestroy methods are also the same */

/*
** Open a new fs cursor.
*/
static int fsOpen(capdb_vtab *pVTab, capdb_vtab_cursor **ppCursor){
  fs_cursor *pCur;
  pCur = capdbMallocZero(sizeof(fs_cursor));
  *ppCursor = &pCur->base;
  return CAPDB_OK;
}

/*
** Close a fs cursor.
*/
static int fsClose(capdb_vtab_cursor *cur){
  fs_cursor *pCur = (fs_cursor *)cur;
  capdb_finalize(pCur->pStmt);
  capdb_free(pCur->zBuf);
  capdb_free(pCur);
  return CAPDB_OK;
}

static int fsNext(capdb_vtab_cursor *cur){
  fs_cursor *pCur = (fs_cursor *)cur;
  int rc;

  rc = capdb_step(pCur->pStmt);
  if( rc==CAPDB_ROW || rc==CAPDB_DONE ) rc = CAPDB_OK;

  return rc;
}

static int fsFilter(
  capdb_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, capdb_value **argv
){
  int rc;
  fs_cursor *pCur = (fs_cursor *)pVtabCursor;
  fs_vtab *p = (fs_vtab *)(pVtabCursor->pVtab);

  assert( (idxNum==0 && argc==0) || (idxNum==1 && argc==1) );
  if( idxNum==1 ){
    char *zStmt = capdb_mprintf(
        "SELECT * FROM %Q.%Q WHERE rowid=?", p->zDb, p->zTbl);
    if( !zStmt ) return CAPDB_NOMEM;
    rc = capdb_prepare_v2(p->db, zStmt, -1, &pCur->pStmt, 0);
    capdb_free(zStmt);
    if( rc==CAPDB_OK ){
      capdb_bind_value(pCur->pStmt, 1, argv[0]);
    }
  }else{
    char *zStmt = capdb_mprintf("SELECT * FROM %Q.%Q", p->zDb, p->zTbl);
    if( !zStmt ) return CAPDB_NOMEM;
    rc = capdb_prepare_v2(p->db, zStmt, -1, &pCur->pStmt, 0);
    capdb_free(zStmt);
  }

  if( rc==CAPDB_OK ){
    rc = fsNext(pVtabCursor); 
  }
  return rc;
}

static int fsColumn(capdb_vtab_cursor *cur, capdb_context *ctx, int i){
  fs_cursor *pCur = (fs_cursor*)cur;

  assert( i==0 || i==1 || i==2 );
  if( i==0 ){
    capdb_result_value(ctx, capdb_column_value(pCur->pStmt, 0));
  }else{
    const char *zFile = (const char *)capdb_column_text(pCur->pStmt, 1);
    struct stat sbuf;
    int fd;

    int n;
    fd = open(zFile, O_RDONLY);
    if( fd<0 ) return CAPDB_IOERR;
    fstat(fd, &sbuf);

    if( sbuf.st_size>=pCur->nAlloc ){
      capdb_int64 nNew = sbuf.st_size*2;
      char *zNew;
      if( nNew<1024 ) nNew = 1024;

      zNew = capdbRealloc(pCur->zBuf, nNew);
      if( zNew==0 ){
        close(fd);
        return CAPDB_NOMEM;
      }
      pCur->zBuf = zNew;
      pCur->nAlloc = nNew;
    }

    n = (int)read(fd, pCur->zBuf, sbuf.st_size);
    close(fd);
    if( n!=sbuf.st_size ) return CAPDB_ERROR;
    pCur->nBuf = sbuf.st_size;
    pCur->zBuf[pCur->nBuf] = '\0';

    capdb_result_text(ctx, pCur->zBuf, -1, CAPDB_TRANSIENT);
  }
  return CAPDB_OK;
}

static int fsRowid(capdb_vtab_cursor *cur, sqlite_int64 *pRowid){
  fs_cursor *pCur = (fs_cursor*)cur;
  *pRowid = capdb_column_int64(pCur->pStmt, 0);
  return CAPDB_OK;
}

static int fsEof(capdb_vtab_cursor *cur){
  fs_cursor *pCur = (fs_cursor*)cur;
  return (capdb_data_count(pCur->pStmt)==0);
}

static int fsBestIndex(capdb_vtab *tab, capdb_index_info *pIdxInfo){
  int ii;

  for(ii=0; ii<pIdxInfo->nConstraint; ii++){
    struct capdb_index_constraint const *pCons = &pIdxInfo->aConstraint[ii];
    if( pCons->iColumn<0 && pCons->usable
           && pCons->op==CAPDB_INDEX_CONSTRAINT_EQ ){
      struct capdb_index_constraint_usage *pUsage;
      pUsage = &pIdxInfo->aConstraintUsage[ii];
      pUsage->omit = 0;
      pUsage->argvIndex = 1;
      pIdxInfo->idxNum = 1;
      pIdxInfo->estimatedCost = 1.0;
      break;
    }
  }

  return CAPDB_OK;
}

/*
** A virtual table module that provides read-only access to a
** Tcl global variable namespace.
*/
static capdb_module fsModule = {
  0,                         /* iVersion */
  fsConnect,
  fsConnect,
  fsBestIndex,
  fsDisconnect, 
  fsDisconnect,
  fsOpen,                      /* xOpen - open a cursor */
  fsClose,                     /* xClose - close a cursor */
  fsFilter,                    /* xFilter - configure scan constraints */
  fsNext,                      /* xNext - advance a cursor */
  fsEof,                       /* xEof - check for end of scan */
  fsColumn,                    /* xColumn - read data */
  fsRowid,                     /* xRowid - read data */
  0,                           /* xUpdate */
  0,                           /* xBegin */
  0,                           /* xSync */
  0,                           /* xCommit */
  0,                           /* xRollback */
  0,                           /* xFindMethod */
  0,                           /* xRename */
  0,                           /* xSavepoint */
  0,                           /* xRelease */
  0,                           /* xRollbackTo */
  0,                           /* xShadowName */
  0                            /* xIntegrity */
};

static capdb_module fsdirModule = {
  0,                              /* iVersion */
  fsdirConnect,                   /* xCreate */
  fsdirConnect,                   /* xConnect */
  fsdirBestIndex,                 /* xBestIndex */
  fsdirDisconnect,                /* xDisconnect */
  fsdirDisconnect,                /* xDestroy */
  fsdirOpen,                      /* xOpen - open a cursor */
  fsdirClose,                     /* xClose - close a cursor */
  fsdirFilter,                    /* xFilter - configure scan constraints */
  fsdirNext,                      /* xNext - advance a cursor */
  fsdirEof,                       /* xEof - check for end of scan */
  fsdirColumn,                    /* xColumn - read data */
  fsdirRowid,                     /* xRowid - read data */
  0,                              /* xUpdate */
  0,                              /* xBegin */
  0,                              /* xSync */
  0,                              /* xCommit */
  0,                              /* xRollback */
  0,                              /* xFindMethod */
  0,                              /* xRename */
  0,                              /* xSavepoint */
  0,                              /* xRelease */
  0,                              /* xRollbackTo */
  0,                              /* xShadowName */
  0                               /* xIntegrity */
};

static capdb_module fstreeModule = {
  0,                              /* iVersion */
  fstreeConnect,                  /* xCreate */
  fstreeConnect,                  /* xConnect */
  fstreeBestIndex,                /* xBestIndex */
  fstreeDisconnect,               /* xDisconnect */
  fstreeDisconnect,               /* xDestroy */
  fstreeOpen,                     /* xOpen - open a cursor */
  fstreeClose,                    /* xClose - close a cursor */
  fstreeFilter,                   /* xFilter - configure scan constraints */
  fstreeNext,                     /* xNext - advance a cursor */
  fstreeEof,                      /* xEof - check for end of scan */
  fstreeColumn,                   /* xColumn - read data */
  fstreeRowid,                    /* xRowid - read data */
  0,                              /* xUpdate */
  0,                              /* xBegin */
  0,                              /* xSync */
  0,                              /* xCommit */
  0,                              /* xRollback */
  0,                              /* xFindMethod */
  0,                              /* xRename */
  0,                              /* xSavepoint */
  0,                              /* xRelease */
  0,                              /* xRollbackTo */
  0,                              /* xShadowName */
  0                               /* xIntegrity */
};

/*
** Decode a pointer to an capdb object.
*/
extern int getDbPointer(Tcl_Interp *interp, const char *zA, capdb **ppDb);

/*
** Register the echo virtual table module.
*/
static int CAPDB_TCLAPI register_fs_module(
  ClientData clientData, /* Pointer to capdb_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  capdb *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdb_create_module(db, "fs", &fsModule, (void *)interp);
  capdb_create_module(db, "fsdir", &fsdirModule, 0);
  capdb_create_module(db, "fstree", &fstreeModule, 0);
#endif
  return TCL_OK;
}

#endif


/*
** Register commands with the TCL interpreter.
*/
int Sqlitetestfs_Init(Tcl_Interp *interp){
#ifndef CAPDB_OMIT_VIRTUALTABLE
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
     void *clientData;
  } aObjCmd[] = {
     { "register_fs_module",   register_fs_module, 0 },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, 
        aObjCmd[i].xProc, aObjCmd[i].clientData, 0);
  }
#endif
  return TCL_OK;
}
