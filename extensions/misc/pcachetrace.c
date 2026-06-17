/*
** 2023-06-21
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file implements an extension that uses the CAPDB_CONFIG_PCACHE2
** mechanism to add a tracing layer on top of pluggable page cache of
** SQLite.  If this extension is registered prior to capdb_initialize(),
** it will cause all page cache activities to be logged on standard output,
** or to some other FILE specified by the initializer.
**
** This file needs to be compiled into the application that uses it.
**
** This extension is used to implement the --pcachetrace option of the
** command-line shell.
*/
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* The original page cache routines */
static capdb_pcache_methods2 pcacheBase;
static FILE *pcachetraceOut;

/* Methods that trace pcache activity */
static int pcachetraceInit(void *pArg){
  int nRes;
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xInit(%p)\n", pArg);
  }
  nRes = pcacheBase.xInit(pArg);
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xInit(%p) -> %d\n", pArg, nRes);
  }
  return nRes;
}
static void pcachetraceShutdown(void *pArg){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xShutdown(%p)\n", pArg);
  }
  pcacheBase.xShutdown(pArg);
}
static capdb_pcache *pcachetraceCreate(int szPage, int szExtra, int bPurge){
  capdb_pcache *pRes;
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xCreate(%d,%d,%d)\n",
            szPage, szExtra, bPurge);
  }
  pRes = pcacheBase.xCreate(szPage, szExtra, bPurge);
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xCreate(%d,%d,%d) -> %p\n",
            szPage, szExtra, bPurge, pRes);
  }
  return pRes;
}
static void pcachetraceCachesize(capdb_pcache *p, int nCachesize){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xCachesize(%p, %d)\n", p, nCachesize);
  }
  pcacheBase.xCachesize(p, nCachesize);
}
static int pcachetracePagecount(capdb_pcache *p){
  int nRes;
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xPagecount(%p)\n", p);
  }
  nRes = pcacheBase.xPagecount(p);
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xPagecount(%p) -> %d\n", p, nRes);
  }
  return nRes;
}
static capdb_pcache_page *pcachetraceFetch(
  capdb_pcache *p,
  unsigned key,
  int crFg
){
  capdb_pcache_page *pRes;
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xFetch(%p,%u,%d)\n", p, key, crFg);
  }
  pRes = pcacheBase.xFetch(p, key, crFg);
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xFetch(%p,%u,%d) -> %p\n",
            p, key, crFg, pRes);
  }
  return pRes;
}
static void pcachetraceUnpin(
  capdb_pcache *p,
  capdb_pcache_page *pPg,
  int bDiscard
){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xUnpin(%p, %p, %d)\n",
            p, pPg, bDiscard);
  }
  pcacheBase.xUnpin(p, pPg, bDiscard);
}
static void pcachetraceRekey(
  capdb_pcache *p,
  capdb_pcache_page *pPg,
  unsigned oldKey,
  unsigned newKey
){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xRekey(%p, %p, %u, %u)\n",
        p, pPg, oldKey, newKey);
  }
  pcacheBase.xRekey(p, pPg, oldKey, newKey);
}
static void pcachetraceTruncate(capdb_pcache *p, unsigned n){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xTruncate(%p, %u)\n", p, n);
  }
  pcacheBase.xTruncate(p, n);
}
static void pcachetraceDestroy(capdb_pcache *p){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xDestroy(%p)\n", p);
  }
  pcacheBase.xDestroy(p);
}
static void pcachetraceShrink(capdb_pcache *p){
  if( pcachetraceOut ){
    fprintf(pcachetraceOut, "PCACHETRACE: xShrink(%p)\n", p);
  }
  pcacheBase.xShrink(p);
}

/* The substitute pcache methods */
static capdb_pcache_methods2 ersaztPcacheMethods = {
  0,
  0,
  pcachetraceInit,
  pcachetraceShutdown,
  pcachetraceCreate,
  pcachetraceCachesize,
  pcachetracePagecount,
  pcachetraceFetch,
  pcachetraceUnpin,
  pcachetraceRekey,
  pcachetraceTruncate,
  pcachetraceDestroy,
  pcachetraceShrink
};

/* Begin tracing memory allocations to out. */
int capdbPcacheTraceActivate(FILE *out){
  int rc = CAPDB_OK;
  if( pcacheBase.xFetch==0 ){
    rc = capdb_config(CAPDB_CONFIG_GETPCACHE2, &pcacheBase);
    if( rc==CAPDB_OK ){
      rc = capdb_config(CAPDB_CONFIG_PCACHE2, &ersaztPcacheMethods);
    }
  }
  pcachetraceOut = out;
  return rc;
}

/* Deactivate memory tracing */
int capdbPcacheTraceDeactivate(void){
  int rc = CAPDB_OK;
  if( pcacheBase.xFetch!=0 ){
    rc = capdb_config(CAPDB_CONFIG_PCACHE2, &pcacheBase);
    if( rc==CAPDB_OK ){
      memset(&pcacheBase, 0, sizeof(pcacheBase));
    }
  }
  pcachetraceOut = 0;
  return rc;
}
