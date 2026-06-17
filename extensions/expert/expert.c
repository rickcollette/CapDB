/*
** 2017 April 07
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/


#include <capdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capdbexpert.h"


static void option_requires_argument(const char *zOpt){
  fprintf(stderr, "Option requires an argument: %s\n", zOpt);
  exit(-3);
}

static int option_integer_arg(const char *zVal){
  return atoi(zVal);
}

static void usage(char **argv){
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage %s ?OPTIONS? DATABASE\n", argv[0]);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options are:\n");
  fprintf(stderr, "  -sql SQL   (analyze SQL statements passed as argument)\n");
  fprintf(stderr, "  -file FILE (read SQL statements from file FILE)\n");
  fprintf(stderr, "  -verbose LEVEL (integer verbosity level. default 1)\n");
  fprintf(stderr, "  -sample PERCENT (percent of db to sample. default 100)\n");
  exit(-1);
}

static int readSqlFromFile(capdbexpert *p, const char *zFile, char **pzErr){
  FILE *in = fopen(zFile, "rb");
  long nIn;
  size_t nRead;
  char *pBuf;
  int rc;
  if( in==0 ){
    *pzErr = capdb_mprintf("failed to open file %s\n", zFile);
    return CAPDB_ERROR;
  }
  fseek(in, 0, SEEK_END);
  nIn = ftell(in);
  rewind(in);
  pBuf = capdb_malloc64( nIn+1 );
  nRead = fread(pBuf, nIn, 1, in);
  fclose(in);
  if( nRead!=1 ){
    capdb_free(pBuf);
    *pzErr = capdb_mprintf("failed to read file %s\n", zFile);
    return CAPDB_ERROR;
  }
  pBuf[nIn] = 0;
  rc = capdb_expert_sql(p, pBuf, pzErr);
  capdb_free(pBuf);
  return rc;
}

int main(int argc, char **argv){
  const char *zDb;
  int rc = 0;
  char *zErr = 0;
  int i;
  int iVerbose = 1;               /* -verbose option */

  capdb *db = 0;
  capdbexpert *p = 0;

  if( argc<2 ) usage(argv);
  zDb = argv[argc-1];
  if( zDb[0]=='-' ) usage(argv);
  rc = capdb_open(zDb, &db);
  if( rc!=CAPDB_OK ){
    fprintf(stderr, "Cannot open db file: %s - %s\n", zDb, capdb_errmsg(db));
    exit(-2);
  }

  p = capdb_expert_new(db, &zErr);
  if( p==0 ){
    fprintf(stderr, "Cannot run analysis: %s\n", zErr);
    rc = 1;
  }else{
    for(i=1; i<(argc-1); i++){
      char *zArg = argv[i];
      int nArg;
      if( zArg[0]=='-' && zArg[1]=='-' && zArg[2]!=0 ) zArg++;
      nArg = (int)strlen(zArg);
      if( nArg>=2 && 0==capdb_strnicmp(zArg, "-file", nArg) ){
        if( ++i==(argc-1) ) option_requires_argument("-file");
        rc = readSqlFromFile(p, argv[i], &zErr);
      }

      else if( nArg>=3 && 0==capdb_strnicmp(zArg, "-sql", nArg) ){
        if( ++i==(argc-1) ) option_requires_argument("-sql");
        rc = capdb_expert_sql(p, argv[i], &zErr);
      }

      else if( nArg>=3 && 0==capdb_strnicmp(zArg, "-sample", nArg) ){
        int iSample;
        if( ++i==(argc-1) ) option_requires_argument("-sample");
        iSample = option_integer_arg(argv[i]);
        capdb_expert_config(p, EXPERT_CONFIG_SAMPLE, iSample);
      }

      else if( nArg>=2 && 0==capdb_strnicmp(zArg, "-verbose", nArg) ){
        if( ++i==(argc-1) ) option_requires_argument("-verbose");
        iVerbose = option_integer_arg(argv[i]);
      }

      else{
        usage(argv);
      }
    }
  }

  if( rc==CAPDB_OK ){
    rc = capdb_expert_analyze(p, &zErr);
  }

  if( rc==CAPDB_OK ){
    int nQuery = capdb_expert_count(p);
    if( iVerbose>0 ){
      const char *zCand = capdb_expert_report(p,0,EXPERT_REPORT_CANDIDATES);
      fprintf(stdout, "-- Candidates -------------------------------\n");
      fprintf(stdout, "%s\n", zCand);
    }
    for(i=0; i<nQuery; i++){
      const char *zSql = capdb_expert_report(p, i, EXPERT_REPORT_SQL);
      const char *zIdx = capdb_expert_report(p, i, EXPERT_REPORT_INDEXES);
      const char *zEQP = capdb_expert_report(p, i, EXPERT_REPORT_PLAN);
      if( zIdx==0 ) zIdx = "(no new indexes)\n";
      if( iVerbose>0 ){
        fprintf(stdout, "-- Query %d ----------------------------------\n",i+1);
        fprintf(stdout, "%s\n\n", zSql);
      }
      fprintf(stdout, "%s\n%s\n", zIdx, zEQP);
    }
  }else{
    fprintf(stderr, "Error: %s\n", zErr ? zErr : "?");
  }

  capdb_expert_destroy(p);
  capdb_free(zErr);
  return rc;
}
