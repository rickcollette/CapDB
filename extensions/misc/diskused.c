/*
** 2026-04-13
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
** This extension implements an SQL function:
**
**     diskused(X)
**
** Where X is the schema name (typically 'main').  The output is text
** that describes how much filesystem space the various tables and indexes
** of the database consume.
**
** This function is a replacement for the (now deprecated)
** "capdb_analyzer" utility program.  This function is built
** into the CLI and is used to implement the ".diskused" command there.
*/
#include "capdbext.h"
CAPDB_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/*
** State information for the analysis
*/
typedef struct DiskUsed DiskUsed;
struct DiskUsed {
  capdb *db;               /* Database connection */
  capdb_context *context;  /* SQL function context */
  capdb_str *pOut;         /* Write output here */
  char *zSU;                 /* Name of the temp.space_used table */
  const char *zSchema;       /* Schema to be analyzed */
};

/*
** Free all resources that the DiskUsed object references and
** reset the DiskUsed object.
**
** Call this routine multiple times on the same DiskUsed object
** is a harmless no-op, as long as the memory for the object itself
** has not been freed.
*/
static void diskusedReset(DiskUsed *p){
  if( p->zSU ){
    char *zSql = capdb_mprintf("DROP TABLE temp.%s;", p->zSU);
    if( zSql ){
      capdb_exec(p->db, zSql, 0, 0, 0);
      capdb_free(zSql);
    }
  }
  capdb_str_free(p->pOut);
  capdb_free(p->zSU);
  memset(p, 0, sizeof(*p));
}

/*
** Report an error using formatted text.  If zFormat==NULL then report
** an OOM error.
*/
static void diskusedError(DiskUsed *p, const char *zFormat, ...){
  char *zErr;
  if( zFormat ){
    va_list ap;
    va_start(ap, zFormat);
    zErr = capdb_vmprintf(zFormat, ap);
    va_end(ap);
  }else{
    zErr = 0;
  }
  if( zErr==0 ){
    capdb_result_error_nomem(p->context);
  }else{
    capdb_result_error(p->context, zErr, -1);
    capdb_free(zErr);
  }
  diskusedReset(p);
}

/*
** Prepare and return an SQL statement.
*/
static capdb_stmt *diskusedVPrep(DiskUsed *p, const char *zFmt, va_list ap){
  char *zSql;
  int rc;
  capdb_stmt *pStmt = 0;
  zSql = capdb_vmprintf(zFmt, ap);
  if( zSql==0 ){ diskusedError(p,0); return 0; }
  rc = capdb_prepare_v2(p->db, zSql, -1, &pStmt, 0);
  if( rc ){
    diskusedError(p, "SQL parse error: %s\nOriginal SQL: %s",
                                 capdb_errmsg(p->db), zSql);
    capdb_finalize(pStmt);
    diskusedReset(p);
    pStmt = 0;
  }
  capdb_free(zSql);
  return pStmt;
}
static capdb_stmt *diskusedPrepare(DiskUsed *p, const char *zFormat, ...){
  va_list ap;
  capdb_stmt *pStmt = 0;
  va_start(ap, zFormat);
  pStmt = diskusedVPrep(p,zFormat,ap);
  va_end(ap);
  return pStmt;
}

/*
** If rc is something other than CAPDB_DONE or CAPDB_OK, then report
** an error and return true.
**
** If rc is CAPDB_DONE or CAPDB_OK, then return false.
**
** The prepared statement is closed in either case.
*/
static int diskusedStmtFinish(DiskUsed *p, int rc, capdb_stmt *pStmt){
  if( rc==CAPDB_DONE ){
    rc = CAPDB_OK;
  }
  if( rc!=CAPDB_OK || (rc = capdb_reset(pStmt))!=CAPDB_OK ){
    diskusedError(p, "SQL run-time error: %s\nOriginal SQL: %s",
                  capdb_errmsg(p->db), capdb_sql(pStmt));
    diskusedReset(p);
  }
  capdb_finalize(pStmt);
  return rc;
}

/*
** Run SQL.  Return the number of errors. 
*/
static int diskusedSql(DiskUsed *p, const char *zFormat, ...){
  va_list ap;
  int rc;
  capdb_stmt *pStmt = 0;
  va_start(ap, zFormat);
  pStmt = diskusedVPrep(p,zFormat,ap);
  va_end(ap);
  if( pStmt==0 ) return 1;
  while( (rc = capdb_step(pStmt))==CAPDB_ROW ){}
  if( rc==CAPDB_DONE ){
    rc = CAPDB_OK;
  }else{
    diskusedError(p, "SQL run-time error: %s\nOriginal SQL: %s",
                  capdb_errmsg(p->db), capdb_sql(pStmt));
    diskusedReset(p);
  }
  capdb_finalize(pStmt);
  return rc;
}

/*
** Run an SQL query that returns an integer.  Write that integer
** into *piRes.  Return the number of errors. 
*/
static int diskusedSqlInt(
  DiskUsed *p,
  capdb_int64 *piRes,
  const char *zFormat, ...
){
  va_list ap;
  int rc;
  capdb_stmt *pStmt = 0;
  va_start(ap, zFormat);
  pStmt = diskusedVPrep(p,zFormat,ap);
  va_end(ap);
  if( pStmt==0 ) return 1;
  rc = capdb_step(pStmt);
  if( rc==CAPDB_ROW ){
    *piRes = capdb_column_int64(pStmt, 0);
    rc = CAPDB_OK;
  }else if( rc==CAPDB_DONE ){
    rc = CAPDB_OK;
  }else{
    if( p->db ){
      /* p->db is NULL if there was some prior error */
      diskusedError(p, "SQL run-time error: %s\nOriginal SQL: %s",
                    capdb_errmsg(p->db), capdb_sql(pStmt));
    }
    diskusedReset(p);
  }
  capdb_finalize(pStmt);
  return rc;
}

/*
** Add to the output a title line that contains the text determined
** by the format string.  If the output is initially empty, begin
** the title line with "/" so that it forms the beginning of a C-style
** comment.  Otherwise begin with a new-line.  Always finish with a
** newline.
*/
static void diskusedTitle(DiskUsed *p, const char *zFormat, ...){
  char *zFirst;
  char *zTitle;
  size_t nTitle;
  va_list ap;
  va_start(ap, zFormat);
  zTitle = capdb_vmprintf(zFormat, ap);
  va_end(ap);
  if( zTitle==0 ){
    diskusedError(p, 0);
    return;
  }
  zFirst = capdb_str_length(p->pOut)==0 ? "/" : "\n*";
  nTitle = strlen(zTitle);
  if( nTitle>=75 ){
    capdb_str_appendf(p->pOut, "%s** %z\n\n", zFirst, zTitle);
  }else{
    int nExtra = 74 - (int)nTitle;
    capdb_str_appendf(p->pOut, "%s** %z %.*c\n\n", zFirst, zTitle,
                        nExtra, '*');
  }
}

/*
** Add an output line that begins with the zDesc text extended out to
** 50 columns with "." characters, and followed by whatever text is
** described by zFormat.
*/
static void diskusedLine(
  DiskUsed *p,             /* DiskUsed context */
  const char *zDesc,       /* Description */
  const char *zFormat,     /* Argument to the description */
  ...
){
  char *zTxt;
  size_t nDesc;
  va_list ap;
  va_start(ap, zFormat);
  zTxt = capdb_vmprintf(zFormat, ap);
  va_end(ap);
  if( zTxt==0 ){
    diskusedError(p, 0);
    return;
  }
  nDesc = zDesc ? strlen(zDesc) : 0;
  if( nDesc>=50 ){
    capdb_str_appendf(p->pOut, "%s %z", zDesc, zTxt);
  }else{
    int nExtra = 50 - (int)nDesc;
    capdb_str_appendf(p->pOut, "%s%.*c %z", zDesc, nExtra, '.', zTxt);
  }
}

/*
** Write a percentage into the output.  The number written should show
** two or three significant digits, with the decimal point being the fourth
** character.  
*/
static void diskusedPercent(DiskUsed *p, double r){
  char zNum[100];
  char *zDP;
  int nLeadingDigit;
  int sz;
  capdb_snprintf(sizeof(zNum)-5, zNum, r>=10.0 ? "%.3g" :"%.2g", r);
  sz = (int)strlen(zNum);
  zDP = strchr(zNum, '.');
  if( zDP==0 ){
    memcpy(zNum+sz,".0",3);
    nLeadingDigit = sz;
    sz += 2;
  }else{
    nLeadingDigit = (int)(zDP - zNum);
  }
  if( nLeadingDigit<3 ){
    capdb_str_appendchar(p->pOut, 3-nLeadingDigit, ' ');
  }
  capdb_str_append(p->pOut, zNum, sz);
  capdb_str_append(p->pOut, "%\n", 2);
}

/*
** Create a subreport on a subset of tables and/or indexes.
**
** The title if the subreport is given by zTitle.  zWhere is
** a boolean expression that can go in the WHERE clause to select
** the relevant rows of the s.zSU table.
*/
static int diskusedSubreport(
  DiskUsed *p,                  /* DiskUsed context */
  char *zTitle,                 /* Title for this subreport */
  char *zWhere,                 /* WHERE clause for this subreport */
  capdb_int64 pgsz,           /* Database page size */
  capdb_int64 nPage           /* Number of pages in entire database */
){
  capdb_stmt *pStmt;          /* Statement to query p->zSU */
  capdb_int64 nentry;         /* Number of btree entires */
  capdb_int64 payload;        /* Payload in bytes */
  capdb_int64 ovfl_payload;   /* overflow payload in bytes */
  capdb_int64 mx_payload;     /* largest individual payload */
  capdb_int64 ovfl_cnt;       /* Number entries using overflow */
  capdb_int64 leaf_pages;     /* Leaf pages */
  capdb_int64 int_pages;      /* internal pages */
  capdb_int64 ovfl_pages;     /* overflow pages */
  capdb_int64 leaf_unused;    /* unused bytes on leaf pages */
  capdb_int64 int_unused;     /* unused bytes on internal pages */
  capdb_int64 ovfl_unused;    /* unused bytes on overflow pages */
  capdb_int64 int_cell;       /* B-tree entries on internal pages */
  capdb_int64 depth;          /* btree depth */
  capdb_int64 cnt;            /* Number of s.zSU entries that match */
  capdb_int64 storage;        /* Total bytes */
  capdb_int64 total_pages;    /* Total page count */
  capdb_int64 total_unused;   /* Total unused bytes */
  capdb_int64 total_meta;     /* Total metadata */
  int rc;

  if( zTitle==0 || zWhere==0 ){
    diskusedError(p, 0);
    return CAPDB_NOMEM;
  }
  pStmt = diskusedPrepare(p,
    "SELECT\n"
    "  sum(if(is_without_rowid OR is_index,nentry,leaf_entries)),\n" /* 0 */
    "  sum(payload),\n"            /* 1 */
    "  sum(ovfl_payload),\n"       /* 2 */
    "  max(mx_payload),\n"         /* 3 */
    "  sum(ovfl_cnt),\n"           /* 4 */
    "  sum(leaf_pages),\n"         /* 5 */
    "  sum(int_pages),\n"          /* 6 */
    "  sum(ovfl_pages),\n"         /* 7 */
    "  sum(leaf_unused),\n"        /* 8 */
    "  sum(int_unused),\n"         /* 9 */
    "  sum(ovfl_unused),\n"        /* 10 */
    "  max(depth),\n"              /* 11 */
    "  count(*),\n"                /* 12 */
    "  sum(int_entries)\n"         /* 13 */
    " FROM temp.%s WHERE %s",
    p->zSU, zWhere);
  if( pStmt==0 ) return 1;
  rc = capdb_step(pStmt);
  if( rc==CAPDB_ROW ){
    diskusedTitle(p, "%s", zTitle);

    nentry = capdb_column_int64(pStmt, 0);
    payload = capdb_column_int64(pStmt, 1);
    ovfl_payload = capdb_column_int64(pStmt, 2);
    mx_payload = capdb_column_int64(pStmt, 3);
    ovfl_cnt = capdb_column_int64(pStmt, 4);
    leaf_pages = capdb_column_int64(pStmt, 5);
    int_pages = capdb_column_int64(pStmt, 6);
    ovfl_pages = capdb_column_int64(pStmt, 7);
    leaf_unused = capdb_column_int64(pStmt, 8);
    int_unused = capdb_column_int64(pStmt, 9);
    ovfl_unused = capdb_column_int64(pStmt, 10);
    depth = capdb_column_int64(pStmt, 11);
    cnt = capdb_column_int64(pStmt, 12);
    int_cell = capdb_column_int64(pStmt, 13);
    rc = CAPDB_DONE;

    total_pages = leaf_pages + int_pages + ovfl_pages;
    diskusedLine(p, "Percentage of total database", "%.3g%%\n",
                 (total_pages*100.0)/(double)nPage);
    diskusedLine(p, "Number of entries", "%lld\n", nentry);
    storage = total_pages*pgsz;
    diskusedLine(p, "Bytes of storage consumed", "%lld\n", storage);
    diskusedLine(p, "Bytes of payload", "%-11lld ", payload);
    diskusedPercent(p, payload*100.0/(double)storage);
    if( ovfl_cnt>0 ){
      diskusedLine(p, "Bytes of payload in overflow","%-11lld ",ovfl_payload);
      diskusedPercent(p, ovfl_payload*100.0/(double)payload);
    }
    total_unused = leaf_unused + int_unused + ovfl_unused;
    total_meta = storage - payload - total_unused;
    diskusedLine(p, "Bytes of metadata","%-11lld ", total_meta);
    diskusedPercent(p, total_meta*100.0/(double)storage);
    if( cnt==1 ){
      diskusedLine(p, "B-tree depth", "%lld\n", depth);
      if( int_cell>1 ){
        diskusedLine(p, "Average fanout", "%.1f\n",
                     (double)(int_cell+int_pages)/(double)int_pages);
      }
    }
    if( nentry>0 ){
      diskusedLine(p, "Average payload per entry", "%.1f\n",
                   (double)payload/(double)nentry);
      diskusedLine(p, "Average unused bytes per entry", "%.1f\n",
                   (double)total_unused/(double)nentry);
      diskusedLine(p, "Average metadata per entry", "%.1f\n",
                   (double)total_meta/(double)nentry);
    }
    diskusedLine(p, "Maximum single-entry payload", "%lld\n", mx_payload);
    if( nentry>0 ){
      diskusedLine(p, "Entries that use overflow", "%-11lld ", ovfl_cnt);
      diskusedPercent(p, ovfl_cnt*100.0/(double)nentry);
    }
    if( int_pages>0 ){
      diskusedLine(p, "Index pages used", "%lld\n", int_pages);
    }
    diskusedLine(p, "Primary pages used", "%lld\n", leaf_pages);
    if( ovfl_cnt ){
      diskusedLine(p, "Overflow pages used", "%lld\n", ovfl_pages);
    }
    diskusedLine(p, "Total pages used", "%lld\n", total_pages);
    if( int_pages>0 ){
      diskusedLine(p, "Unused bytes on index pages", "%lld\n", int_unused);
    }
    diskusedLine(p, "Unused bytes on primary pages", "%lld\n", leaf_unused);
    if( ovfl_cnt ){
      diskusedLine(p, "Unused bytes on overflow pages", "%lld\n", ovfl_unused);
    }
    diskusedLine(p, "Unused bytes on all pages", "%-11lld ", total_unused);
    diskusedPercent(p, total_unused*100.0/(double)storage);
  }
  return diskusedStmtFinish(p, rc, pStmt);
}

/*
** SQL Function:   diskused(SCHEMA)
**
** Analyze the database schema named in the argument.  Return text
** containing the space utilization stats.
*/
static void diskusedFunc(
  capdb_context *context,
  int argc,
  capdb_value **argv
){
  int rc;
  capdb_stmt *pStmt;
  int n;
  capdb_int64 ii;
  capdb_int64 pgsz;
  capdb_int64 nPage;
  capdb_int64 nPageInUse;
  capdb_int64 nFreeList;
  capdb_int64 nIndex;
  capdb_int64 nWORowid;
  DiskUsed s;
  capdb_uint64 r[2];

  (void)argc;
  memset(&s, 0, sizeof(s));
  s.db = capdb_context_db_handle(context);
  s.context = context;
  s.pOut = capdb_str_new(0);
  if( capdb_str_errcode(s.pOut) ){
    diskusedError(&s, 0);
    return;
  }
  s.zSchema = (const char*)capdb_value_text(argv[0]);
  if( s.zSchema==0 ){
    s.zSchema = "main";
  }else if( capdb_strlike("temp",s.zSchema,0)==0 ){
    diskusedReset(&s);
    capdb_result_text(context, "cannot analyze \"temp\"",-1,CAPDB_STATIC);
    return;
  }
  ii = 0;
  rc = diskusedSqlInt(&s,&ii,"SELECT 1 FROM pragma_database_list"
                             " WHERE name=%Q COLLATE nocase",s.zSchema);
  if( rc || ii==0 ){
    diskusedReset(&s);
    capdb_result_text(context,"no such database",-1,CAPDB_STATIC);
    return;
  }
  capdb_randomness(sizeof(r), &r);
  s.zSU = capdb_mprintf("diskused%016llx%016llx", r[0], r[1]);
  if( s.zSU==0 ){ diskusedError(&s, 0); return; }

  /* The s.zSU table contains the data used for the analysis.
  ** The table name contains 128-bits of randomness to avoid
  ** collisions with preexisting tables in temp.
  */
  rc = diskusedSql(&s,
    "CREATE TABLE temp.%s(\n"
    "   name text,                -- A table or index\n"
    "   tblname text,             -- Table that owns name\n"
    "   is_index boolean,         -- TRUE if it is an index\n"
    "   is_without_rowid boolean, -- TRUE if WITHOUT ROWID table\n"
    "   nentry int,               -- Number of entries in the BTree\n"
    "   leaf_entries int,         -- Number of leaf entries\n"
    "   depth int,                -- Depth of the b-tree\n"
    "   payload int,              -- Total data stored in this table/index\n"
    "   ovfl_payload int,         -- Total data stored on overflow pages\n"
    "   ovfl_cnt int,             -- Number of entries that use overflow\n"
    "   mx_payload int,           -- Maximum payload size\n"
    "   int_pages int,            -- Interior pages used\n"
    "   leaf_pages int,           -- Leaf pages used\n"
    "   ovfl_pages int,           -- Overflow pages used\n"
    "   int_unused int,           -- Unused bytes on interior pages\n"
    "   leaf_unused int,          -- Unused bytes on primary pages\n"
    "   ovfl_unused int,          -- Unused bytes on overflow pages\n"
    "   int_entries int           -- Btree cells on internal pages\n"
    ");",
    s.zSU
  );
  if( rc ) return;

  /* Populate the s.zSU table
  */
  rc = diskusedSql(&s,
    "WITH\n"
    "  allidx(idxname) AS (\n"
    "    SELECT name FROM \"%w\".sqlite_schema WHERE type='index'\n"
    "  ),\n"
    "  allobj(allname,tblname,isidx,isworowid) AS (\n"
    "    SELECT 'sqlite_schema',\n"
    "           'sqlite_schema',\n"
    "           0,\n"
    "           0\n"
    "    UNION ALL\n"
    "    SELECT name,\n"
    "           tbl_name,\n"
    "           type='index',\n"
    "           EXISTS(SELECT 1\n"
    "                    FROM pragma_index_list(sqlite_schema.name,%Q)\n"
    "                   WHERE pragma_index_list.origin='pk'\n"
    "                     AND pragma_index_list.name NOT IN allidx)\n"
    "      FROM \"%w\".sqlite_schema\n"
    "  )\n"
    "INSERT INTO temp.%s\n"
    "  SELECT\n"
    "    allname,\n"
    "    tblname,\n"
    "    isidx,\n"
    "    isworowid,\n"
    "    sum(ncell),\n"
    "    sum((pagetype='leaf')*ncell),\n"
    "    max((length(if(path GLOB '*+*','',path))+3)/4),\n"
    "    sum(payload),\n"
    "    sum((pagetype='overflow')*payload),\n"
    "    sum(path GLOB '*+000000'),\n"
    "    max(mx_payload),\n"
    "    sum(pagetype='internal'),\n"
    "    sum(pagetype='leaf'),\n"
    "    sum(pagetype='overflow'),\n"
    "    sum((pagetype='internal')*unused),\n"
    "    sum((pagetype='leaf')*unused),\n"
    "    sum((pagetype='overflow')*unused),\n"
    "    sum(if(pagetype='internal',ncell))\n"
    "  FROM allobj CROSS JOIN dbstat(%Q) \n"
    "  WHERE dbstat.name=allobj.allname\n"
    "  GROUP BY allname;\n",
    s.zSchema,   /* %w.sqlite_schema -- in allidx */
    s.zSchema,   /* pragma_index_list(...,%Q) */
    s.zSchema,   /* %w.sqlite_schema */
    s.zSU,       /* INTO temp.%s */
    s.zSchema    /* JOIN dbstat(%Q) */
  );
  if( rc ) return;

  nPage = 0;
  rc = diskusedSqlInt(&s, &nPage, "PRAGMA \"%w\".page_count", s.zSchema);
  if( rc ) return;
  if( nPage<=0 ){
    /* Very brief reply for an empty database */
    diskusedReset(&s);
    capdb_result_text(context, "empty database", -1, CAPDB_STATIC);
    return;
  }

  /* Begin generating the report */
  diskusedTitle(&s, "Database storage utilization report");
  pgsz = 0;
  rc = diskusedSqlInt(&s, &pgsz, "PRAGMA \"%w\".page_size", s.zSchema);
  if( rc ) return;
  diskusedLine(&s, "Page size in bytes","%lld\n",pgsz);
  diskusedLine(&s, "Pages in the database", "%lld\n", nPage);

  nPageInUse = 0;
  rc = diskusedSqlInt(&s, &nPageInUse, 
       "SELECT sum(leaf_pages+int_pages+ovfl_pages) FROM temp.%s", s.zSU);
  if( rc ) return;
  diskusedLine(&s, "Pages that store data", "%-11lld ", nPageInUse);
  diskusedPercent(&s, (nPageInUse*100.0)/(double)nPage);

  nFreeList = 0;
  rc = diskusedSqlInt(&s, &nFreeList, "PRAGMA \"%w\".freelist_count",s.zSchema);
  if( rc ) return;
  diskusedLine(&s, "Pages on the freelist", "%-11lld ", nFreeList);
  diskusedPercent(&s, (nFreeList*100.0)/(double)nPage);

  ii = 0;
  rc = diskusedSqlInt(&s, &ii, "PRAGMA \"%w\".auto_vacuum", s.zSchema);
  if( rc ) return;
  if( ii==0 || nPage<=1 ){
    ii = 0;
  }else{
    double rPtrsPerPage = pgsz/5;
    double rAvPage = (nPage-1.0)/(rPtrsPerPage+1.0);
    ii = (capdb_int64)ceil(rAvPage);
  }
  diskusedLine(&s, "Pages of auto-vacuum overhead", "%-11lld ", ii);
  diskusedPercent(&s, (ii*100.0)/(double)nPage);

  ii = 0;
  rc = diskusedSqlInt(&s, &ii, 
       "SELECT count(*)+1 FROM \"%w\".sqlite_schema WHERE type='table'",
       s.zSchema);
  if( rc ) return;
  diskusedLine(&s, "Number of tables", "%lld\n", ii);
  nWORowid = 0;
  rc = diskusedSqlInt(&s, &nWORowid,
       "SELECT count(*) FROM \"%w\".pragma_table_list WHERE wr",
       s.zSchema);
  if( rc ) return;
  if( nWORowid>0 ){
    diskusedLine(&s, "Number of WITHOUT ROWID tables", "%lld\n", nWORowid);
    diskusedLine(&s, "Number of rowid tables", "%lld\n", ii - nWORowid);
  }
  nIndex = 0;
  rc = diskusedSqlInt(&s, &nIndex, 
       "SELECT count(*) FROM \"%w\".sqlite_schema WHERE type='index'",
       s.zSchema);
  if( rc ) return;
  diskusedLine(&s, "Number of indexes", "%lld\n", nIndex);
  ii = 0;
  rc = diskusedSqlInt(&s, &ii, 
       "SELECT count(*) FROM \"%w\".sqlite_schema"
       " WHERE name GLOB 'sqlite_autoindex_*' AND type='index'",
       s.zSchema);
  if( rc ) return;
  diskusedLine(&s, "Number of defined indexes", "%lld\n", nIndex - ii);
  diskusedLine(&s, "Number of implied indexes", "%lld\n", ii);
  diskusedLine(&s, "Size of the database in bytes", "%lld\n", pgsz*nPage);
  ii = 0;
  rc = diskusedSqlInt(&s, &ii, 
       "SELECT sum(payload) FROM temp.%s"
       " WHERE NOT is_index AND name NOT LIKE 'sqlite_schema'",
       s.zSU);
  if( rc ) return;
  diskusedLine(&s, "Bytes of payload", "%-11lld ", ii);
  diskusedPercent(&s, ii*100.0/(double)(pgsz*nPage));

  diskusedTitle(&s, "Page counts for all tables with their indexes");
  pStmt = diskusedPrepare(&s,
    "SELECT upper(tblname),\n"
    "       sum(int_pages+leaf_pages+ovfl_pages)\n"
    "  FROM temp.%s\n"
    " WHERE tblname IS NOT NULL\n"
    " GROUP BY 1\n"
    " ORDER BY 2 DESC, 1;",
    s.zSU);
  if( pStmt==0 ) return;
  while( (rc = capdb_step(pStmt))==CAPDB_ROW ){
    capdb_int64 nn = capdb_column_int64(pStmt,1);
    diskusedLine(&s, (const char*)capdb_column_text(pStmt,0), "%-11lld ", nn);
    diskusedPercent(&s, (nn*100.0)/(double)nPage);
  }
  if( diskusedStmtFinish(&s, rc, pStmt) ) return;

  diskusedTitle(&s, "Page counts for all tables and indexes separately");
  pStmt = diskusedPrepare(&s,
    "SELECT upper(name),\n"
    "       sum(int_pages+leaf_pages+ovfl_pages)\n"
    "  FROM temp.%s\n"
    " WHERE name IS NOT NULL\n"
    " GROUP BY 1\n"
    " ORDER BY 2 DESC, 1;",
    s.zSU);
  if( pStmt==0 ) return;
  while( (rc = capdb_step(pStmt))==CAPDB_ROW ){
    capdb_int64 nn = capdb_column_int64(pStmt,1);
    diskusedLine(&s, (const char*)capdb_column_text(pStmt,0), "%-11lld ", nn);
    diskusedPercent(&s, (nn*100.0)/(double)nPage);
  }
  if( diskusedStmtFinish(&s, rc, pStmt) ) return;

  rc = diskusedSubreport(&s, "All tables and indexes", "1", pgsz, nPage);
  if( rc ) return;
  rc = diskusedSubreport(&s, "All tables", "NOT is_index", pgsz, nPage);
  if( rc ) return;
  if( nWORowid>0 ){
    rc = diskusedSubreport(&s, "All WITHOUT ROWID tables", "is_without_rowid",
                           pgsz, nPage);
    if( rc ) return;
    rc = diskusedSubreport(&s, "All rowid tables",
                           "NOT is_without_rowid AND NOT is_index",
                           pgsz, nPage);
    if( rc ) return;
  }
  rc = diskusedSubreport(&s, "All indexes", "is_index", pgsz, nPage);
  if( rc ) return;

  pStmt = diskusedPrepare(&s,
    "SELECT upper(tblname), tblname, sum(is_index) FROM temp.%s"
    " GROUP BY 1 ORDER BY 1",
    s.zSU);
  if( pStmt==0 ) return;
  while( (rc = capdb_step(pStmt))==CAPDB_ROW ){
    const char *zUpper = (const char*)capdb_column_text(pStmt, 0);
    const char *zName = (const char*)capdb_column_text(pStmt, 1);
    int nSubIndex = capdb_column_int(pStmt, 2);
    if( nSubIndex==0 ){
      char *zTitle = capdb_mprintf("Table %s", zUpper);
      char *zWhere = capdb_mprintf("name=%Q", zName);
      rc = diskusedSubreport(&s, zTitle, zWhere, pgsz, nPage);
      capdb_free(zTitle);
      capdb_free(zWhere);
      if( rc ) break;
    }else{
      capdb_stmt *pS2;
      char *zTitle = capdb_mprintf("Table %s and all its indexes", zUpper);
      char *zWhere = capdb_mprintf("tblname=%Q", zName);
      rc = diskusedSubreport(&s, zTitle, zWhere, pgsz, nPage);
      capdb_free(zTitle);
      capdb_free(zWhere);
      if( rc ) break;
      zTitle = capdb_mprintf("Table %s w/o any indexes", zUpper);
      zWhere = capdb_mprintf("name=%Q", zName);
      rc = diskusedSubreport(&s, zTitle, zWhere, pgsz, nPage);
      capdb_free(zTitle);
      capdb_free(zWhere);
      if( rc ) break;
      if( nSubIndex>1 ){
        zTitle = capdb_mprintf("All indexes of table %s", zUpper);
        zWhere = capdb_mprintf("tblname=%Q AND is_index", zName);
        rc = diskusedSubreport(&s, zTitle, zWhere, pgsz, nPage);
        capdb_free(zTitle);
        capdb_free(zWhere);
        if( rc ) break;
      }
      pS2 = diskusedPrepare(&s,
              "SELECT name, upper(name) FROM temp.%s"
              " WHERE is_index AND tblname=%Q",
              s.zSU, zName);
      if( pS2==0 ){
        rc = CAPDB_NOMEM;
        break;
      }
      while( (rc = capdb_step(pS2))==CAPDB_ROW ){
        const char *zU = (const char*)capdb_column_text(pS2, 1);
        const char *zN = (const char*)capdb_column_text(pS2, 0);
        zTitle = capdb_mprintf("Index %s", zU);
        zWhere = capdb_mprintf("name=%Q", zN);
        rc = diskusedSubreport(&s, zTitle, zWhere, pgsz, nPage);
        capdb_free(zTitle);
        capdb_free(zWhere);
        if( rc ) break;
      }
      rc = diskusedStmtFinish(&s, rc, pS2);
      if( rc ) break;
    }
  }
  if( diskusedStmtFinish(&s, rc, pStmt) ) return;

  /* Append SQL statements that will recreate the raw data used for
  ** the analysis.
  */
  diskusedTitle(&s, "Raw data used to generate this report");
  capdb_str_appendf(s.pOut,
    "The following SQL will create a table named \"space_used\" which\n"
    "contains most of the information used to generate the report above.\n"
    "*/\n"
  );
  capdb_str_appendf(s.pOut,
    "BEGIN;\n"
    "CREATE TABLE space_used(\n"
    "   name text,                -- A table or index\n"                /* 0 */
    "   tblname text,             -- Table that owns name\n"            /* 1 */
    "   is_index boolean,         -- TRUE if it is an index\n"          /* 2 */
    "   is_without_rowid boolean, -- TRUE if WITHOUT ROWID table\n"     /* 3 */
    "   nentry int,               -- Number of entries in the BTree\n"  /* 4 */
    "   leaf_entries int,         -- Number of leaf entries\n"          /* 5 */
    "   depth int,                -- Depth of the b-tree\n"             /* 6 */
    "   payload int,              -- Total data in this table/index\n"  /* 7 */
    "   ovfl_payload int,         -- Total data on overflow pages\n"    /* 8 */
    "   ovfl_cnt int,             -- Entries that use overflow\n"       /* 9 */
    "   mx_payload int,           -- Maximum payload size\n"            /* 10 */
    "   int_pages int,            -- Interior pages used\n"             /* 11 */
    "   leaf_pages int,           -- Leaf pages used\n"                 /* 12 */
    "   ovfl_pages int,           -- Overflow pages used\n"             /* 13 */
    "   int_unused int,           -- Unused bytes on interior pages\n"  /* 14 */
    "   leaf_unused int,          -- Unused bytes on primary pages\n"   /* 15 */
    "   ovfl_unused int,          -- Unused bytes on overflow pages\n"  /* 16 */
    "   int_entries int           -- B-tree entries on internal pages\n"/* 17 */
    ");\n"
    "INSERT INTO space_used VALUES\n"
  );
  pStmt = diskusedPrepare(&s,
     "SELECT quote(name), quote(tblname),\n"                        /* 0..1 */
     "       is_index, is_without_rowid, nentry, leaf_entries,\n"   /* 2..5 */
     "       depth, payload, ovfl_payload, ovfl_cnt, mx_payload,\n" /* 6..10 */
     "       int_pages, leaf_pages, ovfl_pages, int_unused,\n"      /* 11..14 */
     "       leaf_unused, ovfl_unused, int_entries\n"               /* 15..17 */
     "  FROM temp.%s;",
     s.zSU);
  if( pStmt==0 ) return;
  n = 0;
  while( (rc = capdb_step(pStmt))==CAPDB_ROW ){
    if( n++ ) capdb_str_appendf(s.pOut,",\n");
    capdb_str_appendf(s.pOut,
      " (%s,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
      "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld)",
      capdb_column_text(pStmt, 0),
      capdb_column_text(pStmt, 1),
      capdb_column_int64(pStmt, 2),
      capdb_column_int64(pStmt, 3),
      capdb_column_int64(pStmt, 4),
      capdb_column_int64(pStmt, 5),
      capdb_column_int64(pStmt, 6),
      capdb_column_int64(pStmt, 7),
      capdb_column_int64(pStmt, 8),
      capdb_column_int64(pStmt, 9),
      capdb_column_int64(pStmt, 10),
      capdb_column_int64(pStmt, 11),
      capdb_column_int64(pStmt, 12),
      capdb_column_int64(pStmt, 13),
      capdb_column_int64(pStmt, 14),
      capdb_column_int64(pStmt, 15),
      capdb_column_int64(pStmt, 16),
      capdb_column_int64(pStmt, 17));
  }
  if( rc!=CAPDB_DONE ){
    diskusedError(&s, "SQL run-time error: %s\nSQL: %s",
                  capdb_errmsg(s.db), capdb_sql(pStmt));
    capdb_finalize(pStmt);
    return;
  }
  capdb_str_appendf(s.pOut,";\nCOMMIT;");
  capdb_finalize(pStmt);

  if( capdb_str_length(s.pOut) ){
    capdb_result_text(context, capdb_str_finish(s.pOut), -1,
                        capdb_free);
    s.pOut = 0;
  }
  diskusedReset(&s);
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int capdb_diskused_init(
  capdb *db, 
  char **pzErrMsg, 
  const capdb_api_routines *pApi
){
  int rc = CAPDB_OK;
  CAPDB_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;  /* Unused parameter */
  rc = capdb_create_function(db, "diskused", 1,
                   CAPDB_UTF8|CAPDB_INNOCUOUS,
                   0, diskusedFunc, 0, 0);
  return rc;
}
