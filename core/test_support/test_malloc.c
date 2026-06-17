/*
** 2007 August 15
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
** This file contains code used to implement test interfaces to the
** memory allocation subsystem.
*/
#include "capdbInt.h"
#include "tclsqlite.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
** This structure is used to encapsulate the global state variables used 
** by malloc() fault simulation.
*/
static struct MemFault {
  int iCountdown;         /* Number of pending successes before a failure */
  int nRepeat;            /* Number of times to repeat the failure */
  int nBenign;            /* Number of benign failures seen since last config */
  int nFail;              /* Number of failures seen since last config */
  int nOkBefore;          /* Successful allocations prior to the first fault */
  int nOkAfter;           /* Successful allocations after a fault */
  u8 enable;              /* True if enabled */
  int isInstalled;        /* True if the fault simulation layer is installed */
  int isBenignMode;       /* True if malloc failures are considered benign */
  capdb_mem_methods m;  /* 'Real' malloc implementation */
} memfault;

/*
** This routine exists as a place to set a breakpoint that will
** fire on any simulated malloc() failure.
*/
static void capdbFault(void){
  static u64 cnt = 0;
  cnt++;
  if( cnt>((u64)1<<63) ) abort();
}

/*
** This routine exists as a place to set a breakpoint that will
** fire the first time any malloc() fails on a single test case.
** The capdbFault() routine above runs on every malloc() failure.
** This routine only runs on the first such failure.
*/
static void capdbFirstFault(void){
  static u64 cnt2 = 0;
  cnt2++;
  if( cnt2>((u64)1<<63) ) abort();
}

/*
** Check to see if a fault should be simulated.  Return true to simulate
** the fault.  Return false if the fault should not be simulated.
*/
static int faultsimStep(void){
  if( likely(!memfault.enable) ){
    memfault.nOkAfter++;
    return 0;
  }
  if( memfault.iCountdown>0 ){
    memfault.iCountdown--;
    memfault.nOkBefore++;
    return 0;
  }
  if( memfault.nFail==0 ) capdbFirstFault();
  capdbFault();
  memfault.nFail++;
  if( memfault.isBenignMode>0 ){
    memfault.nBenign++;
  }
  memfault.nRepeat--;
  if( memfault.nRepeat<=0 ){
    memfault.enable = 0;
  }
  return 1;  
}

/*
** A version of capdb_mem_methods.xMalloc() that includes fault simulation
** logic.
*/
static void *faultsimMalloc(int n){
  void *p = 0;
  if( !faultsimStep() ){
    p = memfault.m.xMalloc(n);
  }
  return p;
}


/*
** A version of capdb_mem_methods.xRealloc() that includes fault simulation
** logic.
*/
static void *faultsimRealloc(void *pOld, int n){
  void *p = 0;
  if( !faultsimStep() ){
    p = memfault.m.xRealloc(pOld, n);
  }
  return p;
}

/*
** This routine configures the malloc failure simulation.  After
** calling this routine, the next nDelay mallocs will succeed, followed
** by a block of nRepeat failures, after which malloc() calls will begin
** to succeed again.
*/
static void faultsimConfig(int nDelay, int nRepeat){
  memfault.iCountdown = nDelay;
  memfault.nRepeat = nRepeat;
  memfault.nBenign = 0;
  memfault.nFail = 0;
  memfault.nOkBefore = 0;
  memfault.nOkAfter = 0;
  memfault.enable = nDelay>=0;

  /* Sometimes, when running multi-threaded tests, the isBenignMode 
  ** variable is not properly incremented/decremented so that it is
  ** 0 when not inside a benign malloc block. This doesn't affect
  ** the multi-threaded tests, as they do not use this system. But
  ** it does affect OOM tests run later in the same process. So
  ** zero the variable here, just to be sure.
  */
  memfault.isBenignMode = 0;
}

/*
** Return the number of faults (both hard and benign faults) that have
** occurred since the injector was last configured.
*/
static int faultsimFailures(void){
  return memfault.nFail;
}

/*
** Return the number of benign faults that have occurred since the
** injector was last configured.
*/
static int faultsimBenignFailures(void){
  return memfault.nBenign;
}

/*
** Return the number of successes that will occur before the next failure.
** If no failures are scheduled, return -1.
*/
static int faultsimPending(void){
  if( memfault.enable ){
    return memfault.iCountdown;
  }else{
    return -1;
  }
}


static void faultsimBeginBenign(void){
  memfault.isBenignMode++;
}
static void faultsimEndBenign(void){
  memfault.isBenignMode--;
}

/*
** Add or remove the fault-simulation layer using capdb_config(). If
** the argument is non-zero, the 
*/
static int faultsimInstall(int install){
  int rc;

  install = (install ? 1 : 0);
  assert(memfault.isInstalled==1 || memfault.isInstalled==0);

  if( install==memfault.isInstalled ){
    return CAPDB_ERROR;
  }

  if( install ){
    rc = capdb_config(CAPDB_CONFIG_GETMALLOC, &memfault.m);
    assert(memfault.m.xMalloc);
    if( rc==CAPDB_OK ){
      capdb_mem_methods m = memfault.m;
      m.xMalloc = faultsimMalloc;
      m.xRealloc = faultsimRealloc;
      rc = capdb_config(CAPDB_CONFIG_MALLOC, &m);
    }
    capdb_test_control(CAPDB_TESTCTRL_BENIGN_MALLOC_HOOKS, 
        faultsimBeginBenign, faultsimEndBenign
    );
  }else{
    capdb_mem_methods m2;
    assert(memfault.m.xMalloc);

    /* One should be able to reset the default memory allocator by storing
    ** a zeroed allocator then calling GETMALLOC. */
    memset(&m2, 0, sizeof(m2));
    capdb_config(CAPDB_CONFIG_MALLOC, &m2);
    capdb_config(CAPDB_CONFIG_GETMALLOC, &m2);
    assert( memcmp(&m2, &memfault.m, sizeof(m2))==0 );

    rc = capdb_config(CAPDB_CONFIG_MALLOC, &memfault.m);
    capdb_test_control(CAPDB_TESTCTRL_BENIGN_MALLOC_HOOKS,
        (void*)0, (void*)0);
  }

  if( rc==CAPDB_OK ){
    memfault.isInstalled = 1;
  }
  return rc;
}

#ifdef CAPDB_TEST

/*
** This function is implemented in main.c. Returns a pointer to a static
** buffer containing the symbolic SQLite error code that corresponds to
** the least-significant 8-bits of the integer passed as an argument.
** For example:
**
**   capdbErrName(1) -> "CAPDB_ERROR"
*/
extern const char *capdbErrName(int);

/*
** Transform pointers to text and back again
*/
static void pointerToText(void *p, char *z){
  static const char zHex[] = "0123456789abcdef";
  int i, k;
  unsigned int u;
  capdb_uint64 n;
  if( p==0 ){
    strcpy(z, "0");
    return;
  }
  if( sizeof(n)==sizeof(p) ){
    memcpy(&n, &p, sizeof(p));
  }else if( sizeof(u)==sizeof(p) ){
    memcpy(&u, &p, sizeof(u));
    n = u;
  }else{
    assert( 0 );
  }
  for(i=0, k=sizeof(p)*2-1; i<sizeof(p)*2; i++, k--){
    z[k] = zHex[n&0xf];
    n >>= 4;
  }
  z[sizeof(p)*2] = 0;
}
static int hexToInt(int h){
  if( h>='0' && h<='9' ){
    return h - '0';
  }else if( h>='a' && h<='f' ){
    return h - 'a' + 10;
  }else{
    return -1;
  }
}
static int textToPointer(const char *z, void **pp){
  capdb_uint64 n = 0;
  int i;
  unsigned int u;
  for(i=0; i<sizeof(void*)*2 && z[0]; i++){
    int v;
    v = hexToInt(*z++);
    if( v<0 ) return TCL_ERROR;
    n = n*16 + v;
  }
  if( *z!=0 ) return TCL_ERROR;
  if( sizeof(n)==sizeof(*pp) ){
    memcpy(pp, &n, sizeof(n));
  }else if( sizeof(u)==sizeof(*pp) ){
    u = (unsigned int)n;
    memcpy(pp, &u, sizeof(u));
  }else{
    assert( 0 );
  }
  return TCL_OK;
}

/*
** Usage:    capdb_malloc  NBYTES
**
** Raw test interface for capdb_malloc().
*/
static int CAPDB_TCLAPI test_malloc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int nByte;
  void *p;
  char zOut[100];
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NBYTES");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &nByte) ) return TCL_ERROR;
  p = capdb_malloc((unsigned)nByte);
  pointerToText(p, zOut);
  Tcl_AppendResult(interp, zOut, NULL);
  return TCL_OK;
}

/*
** Usage:    capdb_realloc  PRIOR  NBYTES
**
** Raw test interface for capdb_realloc().
*/
static int CAPDB_TCLAPI test_realloc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int nByte;
  void *pPrior, *p;
  char zOut[100];
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PRIOR NBYTES");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &nByte) ) return TCL_ERROR;
  if( textToPointer(Tcl_GetString(objv[1]), &pPrior) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  p = capdb_realloc(pPrior, (unsigned)nByte);
  pointerToText(p, zOut);
  Tcl_AppendResult(interp, zOut, NULL);
  return TCL_OK;
}

/*
** Usage:    capdb_free  PRIOR
**
** Raw test interface for capdb_free().
*/
static int CAPDB_TCLAPI test_free(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *pPrior;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PRIOR");
    return TCL_ERROR;
  }
  if( textToPointer(Tcl_GetString(objv[1]), &pPrior) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  capdb_free(pPrior);
  return TCL_OK;
}

/*
** These routines are in test_hexio.c
*/
int capdbTestHexToBin(const char *, int, char *);
int capdbTestBinToHex(char*,int);

/*
** Usage:    memset  ADDRESS  SIZE  HEX
**
** Set a chunk of memory (obtained from malloc, probably) to a
** specified hex pattern.
*/
static int CAPDB_TCLAPI test_memset(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *p;
  int size, i;
  Tcl_Size n;
  char *zHex;
  char *zOut;
  char zBin[100];

  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "ADDRESS SIZE HEX");
    return TCL_ERROR;
  }
  if( textToPointer(Tcl_GetString(objv[1]), &p) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &size) ){
    return TCL_ERROR;
  }
  if( size<=0 ){
    Tcl_AppendResult(interp, "size must be positive", (char*)0);
    return TCL_ERROR;
  }
  zHex = Tcl_GetStringFromObj(objv[3], &n);
  if( n>sizeof(zBin)*2 ) n = sizeof(zBin)*2;
  n = capdbTestHexToBin(zHex, (int)n, zBin);
  if( n==0 ){
    Tcl_AppendResult(interp, "no data", (char*)0);
    return TCL_ERROR;
  }
  zOut = p;
  for(i=0; i<size; i++){
    zOut[i] = zBin[i%n];
  }
  return TCL_OK;
}

/*
** Usage:    memget  ADDRESS  SIZE
**
** Return memory as hexadecimal text.
*/
static int CAPDB_TCLAPI test_memget(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  void *p;
  int size, n;
  char *zBin;
  char zHex[100];

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "ADDRESS SIZE");
    return TCL_ERROR;
  }
  if( textToPointer(Tcl_GetString(objv[1]), &p) ){
    Tcl_AppendResult(interp, "bad pointer: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[2], &size) ){
    return TCL_ERROR;
  }
  if( size<=0 ){
    Tcl_AppendResult(interp, "size must be positive", (char*)0);
    return TCL_ERROR;
  }
  zBin = p;
  while( size>0 ){
    if( size>(sizeof(zHex)-1)/2 ){
      n = (sizeof(zHex)-1)/2;
    }else{
      n = size;
    }
    memcpy(zHex, zBin, n);
    zBin += n;
    size -= n;
    capdbTestBinToHex(zHex, n);
    Tcl_AppendResult(interp, zHex, (char*)0);
  }
  return TCL_OK;
}

/*
** Usage:    capdb_memory_used
**
** Raw test interface for capdb_memory_used().
*/
static int CAPDB_TCLAPI test_memory_used(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(capdb_memory_used()));
  return TCL_OK;
}

/*
** Usage:    capdb_memory_highwater ?RESETFLAG?
**
** Raw test interface for capdb_memory_highwater().
*/
static int CAPDB_TCLAPI test_memory_highwater(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int resetFlag = 0;
  if( objc!=1 && objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?RESET?");
    return TCL_ERROR;
  }
  if( objc==2 ){
    if( Tcl_GetBooleanFromObj(interp, objv[1], &resetFlag) ) return TCL_ERROR;
  } 
  Tcl_SetObjResult(interp, 
     Tcl_NewWideIntObj(capdb_memory_highwater(resetFlag)));
  return TCL_OK;
}

/*
** Usage:    capdb_memdebug_backtrace DEPTH
**
** Set the depth of backtracing.  If CAPDB_MEMDEBUG is not defined
** then this routine is a no-op.
*/
static int CAPDB_TCLAPI test_memdebug_backtrace(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int depth;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DEPT");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &depth) ) return TCL_ERROR;
#ifdef CAPDB_MEMDEBUG
  {
    extern void capdbMemdebugBacktrace(int);
    capdbMemdebugBacktrace(depth);
  }
#endif
  return TCL_OK;
}

/*
** Usage:    capdb_memdebug_dump  FILENAME
**
** Write a summary of unfreed memory to FILENAME.
*/
static int CAPDB_TCLAPI test_memdebug_dump(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME");
    return TCL_ERROR;
  }
#if defined(CAPDB_MEMDEBUG) || defined(CAPDB_MEMORY_SIZE) \
     || defined(CAPDB_POW2_MEMORY_SIZE)
  {
    extern void capdbMemdebugDump(const char*);
    capdbMemdebugDump(Tcl_GetString(objv[1]));
  }
#endif
  return TCL_OK;
}

/*
** Usage:    capdb_memdebug_malloc_count
**
** Return the total number of times malloc() has been called.
*/
static int CAPDB_TCLAPI test_memdebug_malloc_count(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int nMalloc = -1;
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
#if defined(CAPDB_MEMDEBUG)
  {
    extern int capdbMemdebugMallocCount();
    nMalloc = capdbMemdebugMallocCount();
  }
#endif
  Tcl_SetObjResult(interp, Tcl_NewIntObj(nMalloc));
  return TCL_OK;
}


/*
** Usage:    capdb_memdebug_fail  COUNTER  ?OPTIONS?
**
** where options are:
**
**     -repeat    <count>
**     -benigncnt <varname>
**
** Arrange for a simulated malloc() failure after COUNTER successes.
** If a repeat count is specified, the fault is repeated that many
** times.
**
** Each call to this routine overrides the prior counter value.
** This routine returns the number of simulated failures that have
** happened since the previous call to this routine.
**
** To disable simulated failures, use a COUNTER of -1.
*/
static int CAPDB_TCLAPI test_memdebug_fail(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int ii;
  int iFail;
  int nRepeat = 1;
  Tcl_Obj *pBenignCnt = 0;
  int nBenign;
  int nFail = 0;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "COUNTER ?OPTIONS?");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &iFail) ) return TCL_ERROR;

  for(ii=2; ii<objc; ii+=2){
    Tcl_Size nOption;
    char *zOption = Tcl_GetStringFromObj(objv[ii], &nOption);
    char *zErr = 0;

    if( nOption>1 && strncmp(zOption, "-repeat", nOption)==0 ){
      if( ii==(objc-1) ){
        zErr = "option requires an argument: ";
      }else{
        if( Tcl_GetIntFromObj(interp, objv[ii+1], &nRepeat) ){
          return TCL_ERROR;
        }
      }
    }else if( nOption>1 && strncmp(zOption, "-benigncnt", nOption)==0 ){
      if( ii==(objc-1) ){
        zErr = "option requires an argument: ";
      }else{
        pBenignCnt = objv[ii+1];
      }
    }else{
      zErr = "unknown option: ";
    }

    if( zErr ){
      Tcl_AppendResult(interp, zErr, zOption, NULL);
      return TCL_ERROR;
    }
  }
  
  nBenign = faultsimBenignFailures();
  nFail = faultsimFailures();
  faultsimConfig(iFail, nRepeat);

  if( pBenignCnt ){
    Tcl_ObjSetVar2(interp, pBenignCnt, 0, Tcl_NewIntObj(nBenign), 0);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(nFail));
  return TCL_OK;
}

/*
** Usage:    capdb_memdebug_pending
**
** Return the number of malloc() calls that will succeed before a 
** simulated failure occurs. A negative return value indicates that
** no malloc() failure is scheduled.
*/
static int CAPDB_TCLAPI test_memdebug_pending(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int nPending;
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }
  nPending = faultsimPending();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(nPending));
  return TCL_OK;
}

/*
** The following global variable keeps track of the number of tests
** that have run.  This variable is only useful when running in the
** debugger.
*/
static int capdb_memdebug_title_count = 0;

/*
** Usage:    capdb_memdebug_settitle TITLE
**
** Set a title string stored with each allocation.  The TITLE is
** typically the name of the test that was running when the
** allocation occurred.  The TITLE is stored with the allocation
** and can be used to figure out which tests are leaking memory.
**
** Each title overwrite the previous.
*/
static int CAPDB_TCLAPI test_memdebug_settitle(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb_memdebug_title_count++;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "TITLE");
    return TCL_ERROR;
  }
#ifdef CAPDB_MEMDEBUG
  {
    const char *zTitle;
    extern int capdbMemdebugSettitle(const char*);
    zTitle = Tcl_GetString(objv[1]);
    capdbMemdebugSettitle(zTitle);
  }
#endif
  return TCL_OK;
}

#define MALLOC_LOG_FRAMES  10 
#define MALLOC_LOG_KEYINTS (                                              \
    10 * ((sizeof(int)>=sizeof(void*)) ? 1 : sizeof(void*)/sizeof(int))   \
)
static Tcl_HashTable aMallocLog;
static int mallocLogEnabled = 0;

typedef struct MallocLog MallocLog;
struct MallocLog {
  int nCall;
  int nByte;
};

#ifdef CAPDB_MEMDEBUG
static void test_memdebug_callback(int nByte, int nFrame, void **aFrame){
  if( mallocLogEnabled ){
    MallocLog *pLog;
    Tcl_HashEntry *pEntry;
    int isNew;

    int aKey[MALLOC_LOG_KEYINTS];
    unsigned int nKey = sizeof(int)*MALLOC_LOG_KEYINTS;

    memset(aKey, 0, nKey);
    if( (sizeof(void*)*nFrame)<nKey ){
      nKey = nFrame*sizeof(void*);
    }
    memcpy(aKey, aFrame, nKey);

    pEntry = Tcl_CreateHashEntry(&aMallocLog, (const char *)aKey, &isNew);
    if( isNew ){
      pLog = (MallocLog *)Tcl_Alloc(sizeof(MallocLog));
      memset(pLog, 0, sizeof(MallocLog));
      Tcl_SetHashValue(pEntry, (ClientData)pLog);
    }else{
      pLog = (MallocLog *)Tcl_GetHashValue(pEntry);
    }

    pLog->nCall++;
    pLog->nByte += nByte;
  }
}
#endif /* CAPDB_MEMDEBUG */

static void test_memdebug_log_clear(void){
  Tcl_HashSearch search;
  Tcl_HashEntry *pEntry;
  for(
    pEntry=Tcl_FirstHashEntry(&aMallocLog, &search);
    pEntry;
    pEntry=Tcl_NextHashEntry(&search)
  ){
    MallocLog *pLog = (MallocLog *)Tcl_GetHashValue(pEntry);
    Tcl_Free((char *)pLog);
  }
  Tcl_DeleteHashTable(&aMallocLog);
  Tcl_InitHashTable(&aMallocLog, MALLOC_LOG_KEYINTS);
}

static int CAPDB_TCLAPI test_memdebug_log(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static int isInit = 0;
  int iSub;

  static const char *MB_strs[] = { "start", "stop", "dump", "clear", "sync" };
  enum MB_enum { 
      MB_LOG_START, MB_LOG_STOP, MB_LOG_DUMP, MB_LOG_CLEAR, MB_LOG_SYNC 
  };

  if( !isInit ){
#ifdef CAPDB_MEMDEBUG
    extern void capdbMemdebugBacktraceCallback(
        void (*xBacktrace)(int, int, void **));
    capdbMemdebugBacktraceCallback(test_memdebug_callback);
#endif
    Tcl_InitHashTable(&aMallocLog, MALLOC_LOG_KEYINTS);
    isInit = 1;
  }

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND ...");
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], MB_strs, "sub-command", 0, &iSub) ){
    return TCL_ERROR;
  }

  switch( (enum MB_enum)iSub ){
    case MB_LOG_START:
      mallocLogEnabled = 1;
      break;
    case MB_LOG_STOP:
      mallocLogEnabled = 0;
      break;
    case MB_LOG_DUMP: {
      Tcl_HashSearch search;
      Tcl_HashEntry *pEntry;
      Tcl_Obj *pRet = Tcl_NewObj();

      assert(sizeof(Tcl_WideInt)>=sizeof(void*));

      for(
        pEntry=Tcl_FirstHashEntry(&aMallocLog, &search);
        pEntry;
        pEntry=Tcl_NextHashEntry(&search)
      ){
        Tcl_Obj *apElem[MALLOC_LOG_FRAMES+2];
        MallocLog *pLog = (MallocLog *)Tcl_GetHashValue(pEntry);
        Tcl_WideInt *aKey = (Tcl_WideInt *)Tcl_GetHashKey(&aMallocLog, pEntry);
        int ii;
  
        apElem[0] = Tcl_NewIntObj(pLog->nCall);
        apElem[1] = Tcl_NewIntObj(pLog->nByte);
        for(ii=0; ii<MALLOC_LOG_FRAMES; ii++){
          apElem[ii+2] = Tcl_NewWideIntObj(aKey[ii]);
        }

        Tcl_ListObjAppendElement(interp, pRet,
            Tcl_NewListObj(MALLOC_LOG_FRAMES+2, apElem)
        );
      }

      Tcl_SetObjResult(interp, pRet);
      break;
    }
    case MB_LOG_CLEAR: {
      test_memdebug_log_clear();
      break;
    }

    case MB_LOG_SYNC: {
#ifdef CAPDB_MEMDEBUG
      extern void capdbMemdebugSync();
      test_memdebug_log_clear();
      mallocLogEnabled = 1;
      capdbMemdebugSync();
#endif
      break;
    }
  }

  return TCL_OK;
}

/*
** Usage:    capdb_config_pagecache SIZE N
**
** Set the page-cache memory buffer using CAPDB_CONFIG_PAGECACHE.
** The buffer is static and is of limited size.  N might be
** adjusted downward as needed to accommodate the requested size.
** The revised value of N is returned.
**
** A negative SIZE causes the buffer pointer to be NULL.
*/
static int CAPDB_TCLAPI test_config_pagecache(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int sz, N;
  Tcl_Obj *pRes;
  static char *buf = 0;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SIZE N");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &N) ) return TCL_ERROR;
  free(buf);
  buf = 0;

  /* Set the return value */
  pRes = Tcl_NewObj();
  Tcl_ListObjAppendElement(0, pRes, Tcl_NewIntObj(capdbGlobalConfig.szPage));
  Tcl_ListObjAppendElement(0, pRes, Tcl_NewIntObj(capdbGlobalConfig.nPage));
  Tcl_SetObjResult(interp, pRes);

  if( sz<0 ){
    capdb_config(CAPDB_CONFIG_PAGECACHE, (void*)0, 0, 0);
  }else{
    buf = malloc( sz*N );
    capdb_config(CAPDB_CONFIG_PAGECACHE, buf, sz, N);
  }
  return TCL_OK;
}

/*
** Usage:    capdb_config_alt_pcache INSTALL_FLAG DISCARD_CHANCE PRNG_SEED
**
** Set up the alternative test page cache.  Install if INSTALL_FLAG is
** true and uninstall (reverting to the default page cache) if INSTALL_FLAG
** is false.  DISCARD_CHANGE is an integer between 0 and 100 inclusive
** which determines the chance of discarding a page when unpinned.  100
** is certainty.  0 is never.  PRNG_SEED is the pseudo-random number generator
** seed.
*/
static int CAPDB_TCLAPI test_alt_pcache(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int installFlag;
  int discardChance = 0;
  int prngSeed = 0;
  int highStress = 0;
  extern void installTestPCache(int,unsigned,unsigned,unsigned);
  if( objc<2 || objc>5 ){
    Tcl_WrongNumArgs(interp, 1, objv, 
        "INSTALLFLAG DISCARDCHANCE PRNGSEEED HIGHSTRESS");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &installFlag) ) return TCL_ERROR;
  if( objc>=3 && Tcl_GetIntFromObj(interp, objv[2], &discardChance) ){
     return TCL_ERROR;
  }
  if( objc>=4 && Tcl_GetIntFromObj(interp, objv[3], &prngSeed) ){
     return TCL_ERROR;
  }
  if( objc>=5 && Tcl_GetIntFromObj(interp, objv[4], &highStress) ){
    return TCL_ERROR;
  }
  if( discardChance<0 || discardChance>100 ){
    Tcl_AppendResult(interp, "discard-chance should be between 0 and 100",
                     (char*)0);
    return TCL_ERROR;
  }
  installTestPCache(installFlag, (unsigned)discardChance, (unsigned)prngSeed,
                    (unsigned)highStress);
  return TCL_OK;
}

/*
** Usage:    capdb_config_memstatus BOOLEAN
**
** Enable or disable memory status reporting using CAPDB_CONFIG_MEMSTATUS.
*/
static int CAPDB_TCLAPI test_config_memstatus(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int enable, rc;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &enable) ) return TCL_ERROR;
  rc = capdb_config(CAPDB_CONFIG_MEMSTATUS, enable);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** Usage:    capdb_config_lookaside  SIZE  COUNT
**
*/
static int CAPDB_TCLAPI test_config_lookaside(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int sz, cnt;
  Tcl_Obj *pRet;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SIZE COUNT");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &cnt) ) return TCL_ERROR;
  pRet = Tcl_NewObj();
  Tcl_ListObjAppendElement(
      interp, pRet, Tcl_NewIntObj(capdbGlobalConfig.szLookaside)
  );
  Tcl_ListObjAppendElement(
      interp, pRet, Tcl_NewIntObj(capdbGlobalConfig.nLookaside)
  );
  capdb_config(CAPDB_CONFIG_LOOKASIDE, sz, cnt);
  Tcl_SetObjResult(interp, pRet);
  return TCL_OK;
}


/*
** Usage:    capdb_db_config_lookaside  CONNECTION  BUFID  SIZE  COUNT
**
** There are two static buffers with BUFID 1 and 2.   Each static buffer
** is 10KB in size.  A BUFID of 0 indicates that the buffer should be NULL
** which will cause capdb_db_config() to allocate space on its own.
*/
static int CAPDB_TCLAPI test_db_config_lookaside(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int sz, cnt;
  capdb *db;
  int bufid;
  static char azBuf[2][10000];
  extern int getDbPointer(Tcl_Interp*, const char*, capdb**);
  if( objc!=5 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BUFID SIZE COUNT");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &bufid) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[3], &sz) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[4], &cnt) ) return TCL_ERROR;
  if( bufid==0 ){
    rc = capdb_db_config(db, CAPDB_DBCONFIG_LOOKASIDE, (void*)0, sz, cnt);
  }else if( bufid>=1 && bufid<=2 && sz*cnt<=sizeof(azBuf[0]) ){
    rc = capdb_db_config(db, CAPDB_DBCONFIG_LOOKASIDE, azBuf[bufid], sz,cnt);
  }else{
    Tcl_AppendResult(interp, "illegal arguments - see documentation", (char*)0);
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** Usage:    capdb_config_heap NBYTE NMINALLOC
*/
static int CAPDB_TCLAPI test_config_heap(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static char *zBuf; /* Use this memory */
  int nByte;         /* Size of buffer to pass to capdb_config() */
  int nMinAlloc;     /* Size of minimum allocation */
  int rc;            /* Return code of capdb_config() */

  Tcl_Obj * CONST *aArg = &objv[1];
  int nArg = objc-1;

  if( nArg!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NBYTE NMINALLOC");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, aArg[0], &nByte) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, aArg[1], &nMinAlloc) ) return TCL_ERROR;

  if( nByte==0 ){
    free( zBuf );
    zBuf = 0;
    rc = capdb_config(CAPDB_CONFIG_HEAP, (void*)0, 0, 0);
  }else{
    zBuf = realloc(zBuf, nByte);
    rc = capdb_config(CAPDB_CONFIG_HEAP, zBuf, nByte, nMinAlloc);
  }

  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** Usage:    capdb_config_heap_size NBYTE
*/
static int CAPDB_TCLAPI test_config_heap_size(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int nByte;         /* Size to pass to capdb_config() */
  int rc;            /* Return code of capdb_config() */

  Tcl_Obj * CONST *aArg = &objv[1];
  int nArg = objc-1;

  if( nArg!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NBYTE");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, aArg[0], &nByte) ) return TCL_ERROR;

  rc = capdb_config(CAPDB_CONFIG_WIN32_HEAPSIZE, nByte);

  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** Usage:    capdb_config_error  [DB]
**
** Invoke capdb_config() or capdb_db_config() with invalid
** opcodes and verify that they return errors.
*/
static int CAPDB_TCLAPI test_config_error(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  capdb *db;
  extern int getDbPointer(Tcl_Interp*, const char*, capdb**);

  if( objc!=2 && objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "[DB]");
    return TCL_ERROR;
  }
  if( objc==2 ){
    if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
    if( capdb_db_config(db, 99999)!=CAPDB_ERROR ){
      Tcl_AppendResult(interp, 
            "capdb_db_config(db, 99999) does not return CAPDB_ERROR",
            (char*)0);
      return TCL_ERROR;
    }
  }else{
    if( capdb_config(99999)!=CAPDB_ERROR ){
      Tcl_AppendResult(interp, 
          "capdb_config(99999) does not return CAPDB_ERROR",
          (char*)0);
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

/*
** Usage:    capdb_config_uri  BOOLEAN
**
** Enables or disables interpretation of URI parameters by default using
** CAPDB_CONFIG_URI.
*/
static int CAPDB_TCLAPI test_config_uri(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int bOpenUri;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOL");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &bOpenUri) ){
    return TCL_ERROR;
  }

  rc = capdb_config(CAPDB_CONFIG_URI, bOpenUri);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);

  return TCL_OK;
}

/*
** Usage:    capdb_config_cis  BOOLEAN
**
** Enables or disables the use of the covering-index scan optimization.
** CAPDB_CONFIG_COVERING_INDEX_SCAN.
*/
static int CAPDB_TCLAPI test_config_cis(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int bUseCis;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOL");
    return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[1], &bUseCis) ){
    return TCL_ERROR;
  }

  rc = capdb_config(CAPDB_CONFIG_COVERING_INDEX_SCAN, bUseCis);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);

  return TCL_OK;
}

/*
** Usage:    capdb_config_pmasz  INTEGER
**
** Set the minimum PMA size.
*/
static int CAPDB_TCLAPI test_config_pmasz(
  void * clientData, 
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int iPmaSz;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOL");
    return TCL_ERROR;
  }
  if( Tcl_GetIntFromObj(interp, objv[1], &iPmaSz) ){
    return TCL_ERROR;
  }

  rc = capdb_config(CAPDB_CONFIG_PMASZ, iPmaSz);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);

  return TCL_OK;
}


/*
** Usage:    capdb_dump_memsys3  FILENAME
**           capdb_dump_memsys5  FILENAME
**
** Write a summary of unfreed memsys3 allocations to FILENAME.
*/
static int CAPDB_TCLAPI test_dump_memsys3(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "FILENAME");
    return TCL_ERROR;
  }

  switch( CAPDB_PTR_TO_INT(clientData) ){
    case 3: {
#ifdef CAPDB_ENABLE_MEMSYS3
      extern void capdbMemsys3Dump(const char*);
      capdbMemsys3Dump(Tcl_GetString(objv[1]));
      break;
#endif
    }
    case 5: {
#ifdef CAPDB_ENABLE_MEMSYS5
      extern void capdbMemsys5Dump(const char*);
      capdbMemsys5Dump(Tcl_GetString(objv[1]));
      break;
#endif
    }
  }
  return TCL_OK;
}

/*
** Usage:    capdb_status  OPCODE  RESETFLAG
**
** Return a list of three elements which are the capdb_status() return
** code, the current value, and the high-water mark value.
*/
static int CAPDB_TCLAPI test_status(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc, iValue, mxValue;
  int i, op = 0, resetFlag;
  const char *zOpName;
  static const struct {
    const char *zName;
    int op;
  } aOp[] = {
    { "CAPDB_STATUS_MEMORY_USED",         CAPDB_STATUS_MEMORY_USED         },
    { "CAPDB_STATUS_MALLOC_SIZE",         CAPDB_STATUS_MALLOC_SIZE         },
    { "CAPDB_STATUS_PAGECACHE_USED",      CAPDB_STATUS_PAGECACHE_USED      },
    { "CAPDB_STATUS_PAGECACHE_OVERFLOW",  CAPDB_STATUS_PAGECACHE_OVERFLOW  },
    { "CAPDB_STATUS_PAGECACHE_SIZE",      CAPDB_STATUS_PAGECACHE_SIZE      },
    { "CAPDB_STATUS_SCRATCH_USED",        CAPDB_STATUS_SCRATCH_USED        },
    { "CAPDB_STATUS_SCRATCH_OVERFLOW",    CAPDB_STATUS_SCRATCH_OVERFLOW    },
    { "CAPDB_STATUS_SCRATCH_SIZE",        CAPDB_STATUS_SCRATCH_SIZE        },
    { "CAPDB_STATUS_PARSER_STACK",        CAPDB_STATUS_PARSER_STACK        },
    { "CAPDB_STATUS_MALLOC_COUNT",        CAPDB_STATUS_MALLOC_COUNT        },
  };
  Tcl_Obj *pResult;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "PARAMETER RESETFLAG");
    return TCL_ERROR;
  }
  zOpName = Tcl_GetString(objv[1]);
  for(i=0; i<ArraySize(aOp); i++){
    if( strcmp(aOp[i].zName, zOpName)==0 ){
      op = aOp[i].op;
      break;
    }
  }
  if( i>=ArraySize(aOp) ){
    if( Tcl_GetIntFromObj(interp, objv[1], &op) ) return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[2], &resetFlag) ) return TCL_ERROR;
  iValue = 0;
  mxValue = 0;
  rc = capdb_status(op, &iValue, &mxValue, resetFlag);
  pResult = Tcl_NewObj();
  Tcl_ListObjAppendElement(0, pResult, Tcl_NewIntObj(rc));
  Tcl_ListObjAppendElement(0, pResult, Tcl_NewIntObj(iValue));
  Tcl_ListObjAppendElement(0, pResult, Tcl_NewIntObj(mxValue));
  Tcl_SetObjResult(interp, pResult);
  return TCL_OK;
}

/*
** Usage:    capdb_db_status  DATABASE  OPCODE  RESETFLAG
**
** Return a list of three elements which are the capdb_db_status() return
** code, the current value, and the high-water mark value.
*/
static int CAPDB_TCLAPI test_db_status(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc, iValue, mxValue;
  int i, op = 0, resetFlag;
  const char *zOpName;
  capdb *db;
  extern int getDbPointer(Tcl_Interp*, const char*, capdb**);
  static const struct {
    const char *zName;
    int op;
  } aOp[] = {
    { "LOOKASIDE_USED",      CAPDB_DBSTATUS_LOOKASIDE_USED      },
    { "CACHE_USED",          CAPDB_DBSTATUS_CACHE_USED          },
    { "SCHEMA_USED",         CAPDB_DBSTATUS_SCHEMA_USED         },
    { "STMT_USED",           CAPDB_DBSTATUS_STMT_USED           },
    { "LOOKASIDE_HIT",       CAPDB_DBSTATUS_LOOKASIDE_HIT       },
    { "LOOKASIDE_MISS_SIZE", CAPDB_DBSTATUS_LOOKASIDE_MISS_SIZE },
    { "LOOKASIDE_MISS_FULL", CAPDB_DBSTATUS_LOOKASIDE_MISS_FULL },
    { "CACHE_HIT",           CAPDB_DBSTATUS_CACHE_HIT           },
    { "CACHE_MISS",          CAPDB_DBSTATUS_CACHE_MISS          },
    { "CACHE_WRITE",         CAPDB_DBSTATUS_CACHE_WRITE         },
    { "DEFERRED_FKS",        CAPDB_DBSTATUS_DEFERRED_FKS        },
    { "CACHE_USED_SHARED",   CAPDB_DBSTATUS_CACHE_USED_SHARED   },
    { "CACHE_SPILL",         CAPDB_DBSTATUS_CACHE_SPILL         },
    { "TEMPBUF_SPILL",       CAPDB_DBSTATUS_TEMPBUF_SPILL       },
  };
  Tcl_Obj *pResult;
  if( objc!=4 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB PARAMETER RESETFLAG");
    return TCL_ERROR;
  }
  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  zOpName = Tcl_GetString(objv[2]);
  if( memcmp(zOpName, "CAPDB_", 7)==0 ) zOpName += 7;
  if( memcmp(zOpName, "DBSTATUS_", 9)==0 ) zOpName += 9;
  for(i=0; i<ArraySize(aOp); i++){
    if( strcmp(aOp[i].zName, zOpName)==0 ){
      op = aOp[i].op;
      break;
    }
  }
  if( i>=ArraySize(aOp) ){
    if( Tcl_GetIntFromObj(interp, objv[2], &op) ) return TCL_ERROR;
  }
  if( Tcl_GetBooleanFromObj(interp, objv[3], &resetFlag) ) return TCL_ERROR;
  iValue = 0;
  mxValue = 0;
  rc = capdb_db_status(db, op, &iValue, &mxValue, resetFlag);
  pResult = Tcl_NewObj();
  Tcl_ListObjAppendElement(0, pResult, Tcl_NewIntObj(rc));
  Tcl_ListObjAppendElement(0, pResult, Tcl_NewIntObj(iValue));
  Tcl_ListObjAppendElement(0, pResult, Tcl_NewIntObj(mxValue));
  Tcl_SetObjResult(interp, pResult);
  return TCL_OK;
}

/*
** install_malloc_faultsim BOOLEAN
*/
static int CAPDB_TCLAPI test_install_malloc_faultsim(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;
  int isInstall;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[1], &isInstall) ){
    return TCL_ERROR;
  }
  rc = faultsimInstall(isInstall);
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** capdb_install_memsys3
*/
static int CAPDB_TCLAPI test_install_memsys3(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = CAPDB_MISUSE;
#ifdef CAPDB_ENABLE_MEMSYS3
  const capdb_mem_methods *capdbMemGetMemsys3(void);
  rc = capdb_config(CAPDB_CONFIG_MALLOC, capdbMemGetMemsys3());
#endif
  Tcl_SetResult(interp, (char *)capdbErrName(rc), TCL_VOLATILE);
  return TCL_OK;
}

static int CAPDB_TCLAPI test_vfs_oom_test(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int capdb_memdebug_vfs_oom_test;
  if( objc>2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?INTEGER?");
    return TCL_ERROR;
  }else if( objc==2 ){
    int iNew;
    if( Tcl_GetIntFromObj(interp, objv[1], &iNew) ) return TCL_ERROR;
    capdb_memdebug_vfs_oom_test = iNew;
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(capdb_memdebug_vfs_oom_test));
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest_malloc_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
     int clientData;
  } aObjCmd[] = {
     { "capdb_malloc",             test_malloc                   ,0 },
     { "capdb_realloc",            test_realloc                  ,0 },
     { "capdb_free",               test_free                     ,0 },
     { "memset",                     test_memset                   ,0 },
     { "memget",                     test_memget                   ,0 },
     { "capdb_memory_used",        test_memory_used              ,0 },
     { "capdb_memory_highwater",   test_memory_highwater         ,0 },
     { "capdb_memdebug_backtrace", test_memdebug_backtrace       ,0 },
     { "capdb_memdebug_dump",      test_memdebug_dump            ,0 },
     { "capdb_memdebug_fail",      test_memdebug_fail            ,0 },
     { "capdb_memdebug_pending",   test_memdebug_pending         ,0 },
     { "capdb_memdebug_settitle",  test_memdebug_settitle        ,0 },
     { "capdb_memdebug_malloc_count", test_memdebug_malloc_count ,0 },
     { "capdb_memdebug_log",       test_memdebug_log             ,0 },
     { "capdb_config_pagecache",   test_config_pagecache         ,0 },
     { "capdb_config_alt_pcache",  test_alt_pcache               ,0 },
     { "capdb_status",             test_status                   ,0 },
     { "capdb_db_status",          test_db_status                ,0 },
     { "install_malloc_faultsim",    test_install_malloc_faultsim  ,0 },
     { "capdb_config_heap",        test_config_heap              ,0 },
     { "capdb_config_heap_size",   test_config_heap_size         ,0 },
     { "capdb_config_memstatus",   test_config_memstatus         ,0 },
     { "capdb_config_lookaside",   test_config_lookaside         ,0 },
     { "capdb_config_error",       test_config_error             ,0 },
     { "capdb_config_uri",         test_config_uri               ,0 },
     { "capdb_config_cis",         test_config_cis               ,0 },
     { "capdb_config_pmasz",       test_config_pmasz             ,0 },
     { "capdb_db_config_lookaside",test_db_config_lookaside      ,0 },
     { "capdb_dump_memsys3",       test_dump_memsys3             ,3 },
     { "capdb_dump_memsys5",       test_dump_memsys3             ,5 },
     { "capdb_install_memsys3",    test_install_memsys3          ,0 },
     { "capdb_memdebug_vfs_oom_test", test_vfs_oom_test          ,0 },
  };
  int i;
  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    ClientData c = (ClientData)CAPDB_INT_TO_PTR(aObjCmd[i].clientData);
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, c, 0);
  }
  return TCL_OK;
}
#endif
