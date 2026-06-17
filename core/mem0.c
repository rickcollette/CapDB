/*
** 2008 October 28
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
** This file contains a no-op memory allocation drivers for use when
** CAPDB_ZERO_MALLOC is defined.  The allocation drivers implemented
** here always fail.  SQLite will not operate with these drivers.  These
** are merely placeholders.  Real drivers must be substituted using
** capdb_config() before SQLite will operate.
*/
#include "capdbInt.h"

/*
** This version of the memory allocator is the default.  It is
** used when no other memory allocator is specified using compile-time
** macros.
*/
#ifdef CAPDB_ZERO_MALLOC

/*
** No-op versions of all memory allocation routines
*/
static void *capdbMemMalloc(int nByte){ return 0; }
static void capdbMemFree(void *pPrior){ return; }
static void *capdbMemRealloc(void *pPrior, int nByte){ return 0; }
static int capdbMemSize(void *pPrior){ return 0; }
static int capdbMemRoundup(int n){ return n; }
static int capdbMemInit(void *NotUsed){ return CAPDB_OK; }
static void capdbMemShutdown(void *NotUsed){ return; }

/*
** This routine is the only routine in this file with external linkage.
**
** Populate the low-level memory allocation function pointers in
** capdbGlobalConfig.m with pointers to the routines in this file.
*/
void capdbMemSetDefault(void){
  static const capdb_mem_methods defaultMethods = {
     capdbMemMalloc,
     capdbMemFree,
     capdbMemRealloc,
     capdbMemSize,
     capdbMemRoundup,
     capdbMemInit,
     capdbMemShutdown,
     0
  };
  capdb_config(CAPDB_CONFIG_MALLOC, &defaultMethods);
}

#endif /* CAPDB_ZERO_MALLOC */
