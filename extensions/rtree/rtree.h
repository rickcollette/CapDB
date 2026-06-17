/*
** 2008 May 26
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
** This header file is used by programs that want to link against the
** RTREE library.  All it does is declare the capdbRtreeInit() interface.
*/
#include "capdb.h"

#ifdef CAPDB_OMIT_VIRTUALTABLE
# undef CAPDB_ENABLE_RTREE
#endif

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

int capdbRtreeInit(capdb *db);

#ifdef __cplusplus
}  /* extern "C" */
#endif  /* __cplusplus */
