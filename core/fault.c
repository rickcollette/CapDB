/*
** 2008 Jan 22
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
** This file contains code to support the concept of "benign" 
** malloc failures (when the xMalloc() or xRealloc() method of the
** capdb_mem_methods structure fails to allocate a block of memory
** and returns 0). 
**
** Most malloc failures are non-benign. After they occur, SQLite
** abandons the current operation and returns an error code (usually
** CAPDB_NOMEM) to the user. However, sometimes a fault is not necessarily
** fatal. For example, if a malloc fails while resizing a hash table, this 
** is completely recoverable simply by not carrying out the resize. The 
** hash table will continue to function normally.  So a malloc failure 
** during a hash table resize is a benign fault.
*/

#include "capdbInt.h"

#ifndef CAPDB_UNTESTABLE

/*
** Global variables.
*/
typedef struct BenignMallocHooks BenignMallocHooks;
static CAPDB_WSD struct BenignMallocHooks {
  void (*xBenignBegin)(void);
  void (*xBenignEnd)(void);
} capdbHooks = { 0, 0 };

/* The "wsdHooks" macro will resolve to the appropriate BenignMallocHooks
** structure.  If writable static data is unsupported on the target,
** we have to locate the state vector at run-time.  In the more common
** case where writable static data is supported, wsdHooks can refer directly
** to the "capdbHooks" state vector declared above.
*/
#ifdef CAPDB_OMIT_WSD
# define wsdHooksInit \
  BenignMallocHooks *x = &GLOBAL(BenignMallocHooks,capdbHooks)
# define wsdHooks x[0]
#else
# define wsdHooksInit
# define wsdHooks capdbHooks
#endif


/*
** Register hooks to call when capdbBeginBenignMalloc() and
** capdbEndBenignMalloc() are called, respectively.
*/
void capdbBenignMallocHooks(
  void (*xBenignBegin)(void),
  void (*xBenignEnd)(void)
){
  wsdHooksInit;
  wsdHooks.xBenignBegin = xBenignBegin;
  wsdHooks.xBenignEnd = xBenignEnd;
}

/*
** This (capdbEndBenignMalloc()) is called by SQLite code to indicate that
** subsequent malloc failures are benign. A call to capdbEndBenignMalloc()
** indicates that subsequent malloc failures are non-benign.
*/
void capdbBeginBenignMalloc(void){
  wsdHooksInit;
  if( wsdHooks.xBenignBegin ){
    wsdHooks.xBenignBegin();
  }
}
void capdbEndBenignMalloc(void){
  wsdHooksInit;
  if( wsdHooks.xBenignEnd ){
    wsdHooks.xBenignEnd();
  }
}

#endif   /* #ifndef CAPDB_UNTESTABLE */
