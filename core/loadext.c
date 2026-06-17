/*
** 2006 June 7
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to dynamically load extensions into
** the SQLite library.
*/

#ifndef CAPDB_CORE
  #define CAPDB_CORE 1  /* Disable the API redefinition in capdbext.h */
#endif
#include "capdbext.h"
#include "capdbInt.h"

#ifndef CAPDB_OMIT_LOAD_EXTENSION
/*
** Some API routines are omitted when various features are
** excluded from a build of SQLite.  Substitute a NULL pointer
** for any missing APIs.
*/
#ifndef CAPDB_ENABLE_COLUMN_METADATA
# define capdb_column_database_name   0
# define capdb_column_database_name16 0
# define capdb_column_table_name      0
# define capdb_column_table_name16    0
# define capdb_column_origin_name     0
# define capdb_column_origin_name16   0
#endif

#ifdef CAPDB_OMIT_AUTHORIZATION
# define capdb_set_authorizer         0
#endif

#ifdef CAPDB_OMIT_UTF16
# define capdb_bind_text16            0
# define capdb_collation_needed16     0
# define capdb_column_decltype16      0
# define capdb_column_name16          0
# define capdb_column_text16          0
# define capdb_complete16             0
# define capdb_create_collation16     0
# define capdb_create_function16      0
# define capdb_errmsg16               0
# define capdb_open16                 0
# define capdb_prepare16              0
# define capdb_prepare16_v2           0
# define capdb_prepare16_v3           0
# define capdb_result_error16         0
# define capdb_result_text16          0
# define capdb_result_text16be        0
# define capdb_result_text16le        0
# define capdb_value_text16           0
# define capdb_value_text16be         0
# define capdb_value_text16le         0
# define capdb_column_database_name16 0
# define capdb_column_table_name16    0
# define capdb_column_origin_name16   0
#endif

#ifdef CAPDB_OMIT_COMPLETE
# define capdb_complete 0
# define capdb_complete16 0
#endif

#ifdef CAPDB_OMIT_DECLTYPE
# define capdb_column_decltype16      0
# define capdb_column_decltype        0
#endif

#ifdef CAPDB_OMIT_PROGRESS_CALLBACK
# define capdb_progress_handler 0
#endif

#ifdef CAPDB_OMIT_VIRTUALTABLE
# define capdb_create_module 0
# define capdb_create_module_v2 0
# define capdb_declare_vtab 0
# define capdb_vtab_config 0
# define capdb_vtab_on_conflict 0
# define capdb_vtab_collation 0
#endif

#ifdef CAPDB_OMIT_SHARED_CACHE
# define capdb_enable_shared_cache 0
#endif

#if defined(CAPDB_OMIT_TRACE) || defined(CAPDB_OMIT_DEPRECATED)
# define capdb_profile       0
# define capdb_trace         0
#endif

#ifdef CAPDB_OMIT_GET_TABLE
# define capdb_free_table    0
# define capdb_get_table     0
#endif

#ifdef CAPDB_OMIT_INCRBLOB
#define capdb_bind_zeroblob  0
#define capdb_blob_bytes     0
#define capdb_blob_close     0
#define capdb_blob_open      0
#define capdb_blob_read      0
#define capdb_blob_write     0
#define capdb_blob_reopen    0
#endif

#if defined(CAPDB_OMIT_TRACE)
# define capdb_trace_v2      0
#endif

/*
** The following structure contains pointers to all SQLite API routines.
** A pointer to this structure is passed into extensions when they are
** loaded so that the extension can make calls back into the SQLite
** library.
**
** When adding new APIs, add them to the bottom of this structure
** in order to preserve backwards compatibility.
**
** Extensions that use newer APIs should first call the
** capdb_libversion_number() to make sure that the API they
** intend to use is supported by the library.  Extensions should
** also check to make sure that the pointer to the function is
** not NULL before calling it.
*/
static const capdb_api_routines capdbApis = {
  capdb_aggregate_context,
#ifndef CAPDB_OMIT_DEPRECATED
  capdb_aggregate_count,
#else
  0,
#endif
  capdb_bind_blob,
  capdb_bind_double,
  capdb_bind_int,
  capdb_bind_int64,
  capdb_bind_null,
  capdb_bind_parameter_count,
  capdb_bind_parameter_index,
  capdb_bind_parameter_name,
  capdb_bind_text,
  capdb_bind_text16,
  capdb_bind_value,
  capdb_busy_handler,
  capdb_busy_timeout,
  capdb_changes,
  capdb_close,
  capdb_collation_needed,
  capdb_collation_needed16,
  capdb_column_blob,
  capdb_column_bytes,
  capdb_column_bytes16,
  capdb_column_count,
  capdb_column_database_name,
  capdb_column_database_name16,
  capdb_column_decltype,
  capdb_column_decltype16,
  capdb_column_double,
  capdb_column_int,
  capdb_column_int64,
  capdb_column_name,
  capdb_column_name16,
  capdb_column_origin_name,
  capdb_column_origin_name16,
  capdb_column_table_name,
  capdb_column_table_name16,
  capdb_column_text,
  capdb_column_text16,
  capdb_column_type,
  capdb_column_value,
  capdb_commit_hook,
  capdb_complete,
  capdb_complete16,
  capdb_create_collation,
  capdb_create_collation16,
  capdb_create_function,
  capdb_create_function16,
  capdb_create_module,
  capdb_data_count,
  capdb_db_handle,
  capdb_declare_vtab,
  capdb_enable_shared_cache,
  capdb_errcode,
  capdb_errmsg,
  capdb_errmsg16,
  capdb_exec,
#ifndef CAPDB_OMIT_DEPRECATED
  capdb_expired,
#else
  0,
#endif
  capdb_finalize,
  capdb_free,
  capdb_free_table,
  capdb_get_autocommit,
  capdb_get_auxdata,
  capdb_get_table,
  0,     /* Was capdb_global_recover(), but that function is deprecated */
  capdb_interrupt,
  capdb_last_insert_rowid,
  capdb_libversion,
  capdb_libversion_number,
  capdb_malloc,
  capdb_mprintf,
  capdb_open,
  capdb_open16,
  capdb_prepare,
  capdb_prepare16,
  capdb_profile,
  capdb_progress_handler,
  capdb_realloc,
  capdb_reset,
  capdb_result_blob,
  capdb_result_double,
  capdb_result_error,
  capdb_result_error16,
  capdb_result_int,
  capdb_result_int64,
  capdb_result_null,
  capdb_result_text,
  capdb_result_text16,
  capdb_result_text16be,
  capdb_result_text16le,
  capdb_result_value,
  capdb_rollback_hook,
  capdb_set_authorizer,
  capdb_set_auxdata,
  capdb_snprintf,
  capdb_step,
  capdb_table_column_metadata,
#ifndef CAPDB_OMIT_DEPRECATED
  capdb_thread_cleanup,
#else
  0,
#endif
  capdb_total_changes,
  capdb_trace,
#ifndef CAPDB_OMIT_DEPRECATED
  capdb_transfer_bindings,
#else
  0,
#endif
  capdb_update_hook,
  capdb_user_data,
  capdb_value_blob,
  capdb_value_bytes,
  capdb_value_bytes16,
  capdb_value_double,
  capdb_value_int,
  capdb_value_int64,
  capdb_value_numeric_type,
  capdb_value_text,
  capdb_value_text16,
  capdb_value_text16be,
  capdb_value_text16le,
  capdb_value_type,
  capdb_vmprintf,
  /*
  ** The original API set ends here.  All extensions can call any
  ** of the APIs above provided that the pointer is not NULL.  But
  ** before calling APIs that follow, extension should check the
  ** capdb_libversion_number() to make sure they are dealing with
  ** a library that is new enough to support that API.
  *************************************************************************
  */
  capdb_overload_function,

  /*
  ** Added after 3.3.13
  */
  capdb_prepare_v2,
  capdb_prepare16_v2,
  capdb_clear_bindings,

  /*
  ** Added for 3.4.1
  */
  capdb_create_module_v2,

  /*
  ** Added for 3.5.0
  */
  capdb_bind_zeroblob,
  capdb_blob_bytes,
  capdb_blob_close,
  capdb_blob_open,
  capdb_blob_read,
  capdb_blob_write,
  capdb_create_collation_v2,
  capdb_file_control,
  capdb_memory_highwater,
  capdb_memory_used,
#ifdef CAPDB_MUTEX_OMIT
  0, 
  0, 
  0,
  0,
  0,
#else
  capdb_mutex_alloc,
  capdb_mutex_enter,
  capdb_mutex_free,
  capdb_mutex_leave,
  capdb_mutex_try,
#endif
  capdb_open_v2,
  capdb_release_memory,
  capdb_result_error_nomem,
  capdb_result_error_toobig,
  capdb_sleep,
  capdb_soft_heap_limit,
  capdb_vfs_find,
  capdb_vfs_register,
  capdb_vfs_unregister,

  /*
  ** Added for 3.5.8
  */
  capdb_threadsafe,
  capdb_result_zeroblob,
  capdb_result_error_code,
  capdb_test_control,
  capdb_randomness,
  capdb_context_db_handle,

  /*
  ** Added for 3.6.0
  */
  capdb_extended_result_codes,
  capdb_limit,
  capdb_next_stmt,
  capdb_sql,
  capdb_status,

  /*
  ** Added for 3.7.4
  */
  capdb_backup_finish,
  capdb_backup_init,
  capdb_backup_pagecount,
  capdb_backup_remaining,
  capdb_backup_step,
#ifndef CAPDB_OMIT_COMPILEOPTION_DIAGS
  capdb_compileoption_get,
  capdb_compileoption_used,
#else
  0,
  0,
#endif
  capdb_create_function_v2,
  capdb_db_config,
  capdb_db_mutex,
  capdb_db_status,
  capdb_extended_errcode,
  capdb_log,
  capdb_soft_heap_limit64,
  capdb_sourceid,
  capdb_stmt_status,
  capdb_strnicmp,
#ifdef CAPDB_ENABLE_UNLOCK_NOTIFY
  capdb_unlock_notify,
#else
  0,
#endif
#ifndef CAPDB_OMIT_WAL
  capdb_wal_autocheckpoint,
  capdb_wal_checkpoint,
  capdb_wal_hook,
#else
  0,
  0,
  0,
#endif
  capdb_blob_reopen,
  capdb_vtab_config,
  capdb_vtab_on_conflict,
  capdb_close_v2,
  capdb_db_filename,
  capdb_db_readonly,
  capdb_db_release_memory,
  capdb_errstr,
  capdb_stmt_busy,
  capdb_stmt_readonly,
  capdb_stricmp,
  capdb_uri_boolean,
  capdb_uri_int64,
  capdb_uri_parameter,
  capdb_vsnprintf,
  capdb_wal_checkpoint_v2,
  /* Version 3.8.7 and later */
  capdb_auto_extension,
  capdb_bind_blob64,
  capdb_bind_text64,
  capdb_cancel_auto_extension,
  capdb_load_extension,
  capdb_malloc64,
  capdb_msize,
  capdb_realloc64,
  capdb_reset_auto_extension,
  capdb_result_blob64,
  capdb_result_text64,
  capdb_strglob,
  /* Version 3.8.11 and later */
  (capdb_value*(*)(const capdb_value*))capdb_value_dup,
  capdb_value_free,
  capdb_result_zeroblob64,
  capdb_bind_zeroblob64,
  /* Version 3.9.0 and later */
  capdb_value_subtype,
  capdb_result_subtype,
  /* Version 3.10.0 and later */
  capdb_status64,
  capdb_strlike,
  capdb_db_cacheflush,
  /* Version 3.12.0 and later */
  capdb_system_errno,
  /* Version 3.14.0 and later */
  capdb_trace_v2,
  capdb_expanded_sql,
  /* Version 3.18.0 and later */
  capdb_set_last_insert_rowid,
  /* Version 3.20.0 and later */
  capdb_prepare_v3,
  capdb_prepare16_v3,
  capdb_bind_pointer,
  capdb_result_pointer,
  capdb_value_pointer,
  /* Version 3.22.0 and later */
  capdb_vtab_nochange,
  capdb_value_nochange,
  capdb_vtab_collation,
  /* Version 3.24.0 and later */
  capdb_keyword_count,
  capdb_keyword_name,
  capdb_keyword_check,
  capdb_str_new,
  capdb_str_finish,
  capdb_str_appendf,
  capdb_str_vappendf,
  capdb_str_append,
  capdb_str_appendall,
  capdb_str_appendchar,
  capdb_str_reset,
  capdb_str_errcode,
  capdb_str_length,
  capdb_str_value,
  /* Version 3.25.0 and later */
  capdb_create_window_function,
  /* Version 3.26.0 and later */
#ifdef CAPDB_ENABLE_NORMALIZE
  capdb_normalized_sql,
#else
  0,
#endif
  /* Version 3.28.0 and later */
  capdb_stmt_isexplain,
  capdb_value_frombind,
  /* Version 3.30.0 and later */
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdb_drop_modules,
#else
  0,
#endif
  /* Version 3.31.0 and later */
  capdb_hard_heap_limit64,
  capdb_uri_key,
  capdb_filename_database,
  capdb_filename_journal,
  capdb_filename_wal,
  /* Version 3.32.0 and later */
  capdb_create_filename,
  capdb_free_filename,
  capdb_database_file_object,
  /* Version 3.34.0 and later */
  capdb_txn_state,
  /* Version 3.36.1 and later */
  capdb_changes64,
  capdb_total_changes64,
  /* Version 3.37.0 and later */
  capdb_autovacuum_pages,
  /* Version 3.38.0 and later */
  capdb_error_offset,
#ifndef CAPDB_OMIT_VIRTUALTABLE
  capdb_vtab_rhs_value,
  capdb_vtab_distinct,
  capdb_vtab_in,
  capdb_vtab_in_first,
  capdb_vtab_in_next,
#else
  0,
  0,
  0,
  0,
  0,
#endif
  /* Version 3.39.0 and later */
#ifndef CAPDB_OMIT_DESERIALIZE
  capdb_deserialize,
  capdb_serialize,
#else
  0,
  0,
#endif
  capdb_db_name,
  /* Version 3.40.0 and later */
  capdb_value_encoding,
  /* Version 3.41.0 and later */
  capdb_is_interrupted,
  /* Version 3.43.0 and later */
  capdb_stmt_explain,
  /* Version 3.44.0 and later */
  capdb_get_clientdata,
  capdb_set_clientdata,
  /* Version 3.50.0 and later */
  capdb_setlk_timeout,
  /* Version 3.51.0 and later */
  capdb_set_errmsg,
  capdb_db_status64,
  /* Version 3.52.0 and later */
  capdb_str_truncate,
  capdb_str_free,
#ifdef CAPDB_ENABLE_CARRAY
  capdb_carray_bind,
  capdb_carray_bind_v2,
#else
  0,
  0,
#endif
  capdb_incomplete
};

/* True if x is the directory separator character
*/
#if CAPDB_OS_WIN
# define DirSep(X)  ((X)=='/'||(X)=='\\')
#else
# define DirSep(X)  ((X)=='/')
#endif

/*
** Attempt to load an SQLite extension library contained in the file
** zFile.  The entry point is zProc.  zProc may be 0 in which case a
** default entry point name (capdb_extension_init) is used.  Use
** of the default name is recommended.
**
** Return CAPDB_OK on success and CAPDB_ERROR if something goes wrong.
**
** If an error occurs and pzErrMsg is not 0, then fill *pzErrMsg with 
** error message text.  The calling function should free this memory
** by calling capdbDbFree(db, ).
*/
static int capdbLoadExtension(
  capdb *db,          /* Load the extension into this database connection */
  const char *zFile,    /* Name of the shared library containing extension */
  const char *zProc,    /* Entry point.  Use "capdb_extension_init" if 0 */
  char **pzErrMsg       /* Put error message here if not 0 */
){
  capdb_vfs *pVfs = db->pVfs;
  void *handle;
  capdb_loadext_entry xInit;
  char *zErrmsg = 0;
  const char *zEntry;
  char *zAltEntry = 0;
  void **aHandle;
  u64 nMsg = strlen(zFile);
  int ii;
  int rc;

  /* Shared library endings to try if zFile cannot be loaded as written */
  static const char *azEndings[] = {
#if CAPDB_OS_WIN
     "dll"   
#elif defined(__APPLE__)
     "dylib"
#else
     "so"
#endif
  };


  if( pzErrMsg ) *pzErrMsg = 0;

  /* Ticket #1863.  To avoid a creating security problems for older
  ** applications that relink against newer versions of SQLite, the
  ** ability to run load_extension is turned off by default.  One
  ** must call either capdb_enable_load_extension(db) or
  ** capdb_db_config(db, CAPDB_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, 0)
  ** to turn on extension loading.
  */
  if( (db->flags & CAPDB_LoadExtension)==0 ){
    if( pzErrMsg ){
      *pzErrMsg = capdb_mprintf("not authorized");
    }
    return CAPDB_ERROR;
  }

  zEntry = zProc ? zProc : "capdb_extension_init";

  /* tag-20210611-1.  Some dlopen() implementations will segfault if given
  ** an oversize filename.  Most filesystems have a pathname limit of 4K,
  ** so limit the extension filename length to about twice that.
  ** https://sqlite.org/forum/forumpost/08a0d6d9bf
  **
  ** Later (2023-03-25): Save an extra 6 bytes for the filename suffix.
  ** See https://sqlite.org/forum/forumpost/24083b579d.
  */
  if( nMsg>CAPDB_MAX_PATHLEN ) goto extension_not_found;

  /* Do not allow capdb_load_extension() to link to a copy of the
  ** running application, by passing in an empty filename. */
  if( nMsg==0 ) goto extension_not_found;
    
  handle = capdbOsDlOpen(pVfs, zFile);
#if CAPDB_OS_UNIX || CAPDB_OS_WIN
  for(ii=0; ii<ArraySize(azEndings) && handle==0; ii++){
    char *zAltFile = capdb_mprintf("%s.%s", zFile, azEndings[ii]);
    if( zAltFile==0 ) return CAPDB_NOMEM_BKPT;
    if( nMsg+strlen(azEndings[ii])+1<=CAPDB_MAX_PATHLEN ){
      handle = capdbOsDlOpen(pVfs, zAltFile);
    }
    capdb_free(zAltFile);
  }
#endif
  if( handle==0 ) goto extension_not_found;
  xInit = (capdb_loadext_entry)capdbOsDlSym(pVfs, handle, zEntry);

  /* If no entry point was specified and the default legacy
  ** entry point name "capdb_extension_init" was not found, then
  ** construct an entry point name "capdb_X_init" where the X is
  ** replaced by the lowercase value of every ASCII alphabetic 
  ** character in the filename after the last "/" up to the first ".",
  ** and skipping the first three characters if they are "lib".  
  ** Examples:
  **
  **    /usr/local/lib/libExample5.4.3.so ==>  capdb_example_init
  **    C:/lib/mathfuncs.dll              ==>  capdb_mathfuncs_init
  **
  ** If that still finds no entry point, repeat a second time but this
  ** time include both alphabetic and numeric characters up to the first
  ** ".".  Example:
  **
  **    /usr/local/lib/libExample5.4.3.so ==>  capdb_example5_init
  */
  if( xInit==0 && zProc==0 ){
    int iFile, iEntry, c;
    int ncFile = capdbStrlen30(zFile);
    int cnt = 0;
    zAltEntry = capdb_malloc64(ncFile+30);
    if( zAltEntry==0 ){
      capdbOsDlClose(pVfs, handle);
      return CAPDB_NOMEM_BKPT;
    }
    do{
      memcpy(zAltEntry, "capdb_", 8);
      for(iFile=ncFile-1; iFile>=0 && !DirSep(zFile[iFile]); iFile--){}
      iFile++;
      if( capdb_strnicmp(zFile+iFile, "lib", 3)==0 ) iFile += 3;
      for(iEntry=8; (c = zFile[iFile])!=0 && c!='.'; iFile++){
        if( capdbIsalpha(c) || (cnt && capdbIsdigit(c)) ){
          zAltEntry[iEntry++] = (char)capdbUpperToLower[(unsigned)c];
        }
      }
      memcpy(zAltEntry+iEntry, "_init", 6);
      zEntry = zAltEntry;
      xInit = (capdb_loadext_entry)capdbOsDlSym(pVfs, handle, zEntry);
    }while( xInit==0 && (++cnt)<2 );
  }
  if( xInit==0 ){
    if( pzErrMsg ){
      nMsg += strlen(zEntry) + 300;
      *pzErrMsg = zErrmsg = capdb_malloc64(nMsg);
      if( zErrmsg ){
        assert( nMsg<0x7fffffff );  /* zErrmsg would be NULL if not so */
        capdb_snprintf((int)nMsg, zErrmsg,
            "no entry point [%s] in shared library [%s]", zEntry, zFile);
        capdbOsDlError(pVfs, nMsg-1, zErrmsg);
      }
    }
    capdbOsDlClose(pVfs, handle);
    capdb_free(zAltEntry);
    return CAPDB_ERROR;
  }
  capdb_free(zAltEntry);
  rc = xInit(db, &zErrmsg, &capdbApis);
  if( rc ){
    if( rc==CAPDB_OK_LOAD_PERMANENTLY ) return CAPDB_OK;
    if( pzErrMsg ){
      *pzErrMsg = capdb_mprintf("error during initialization: %s", zErrmsg);
    }
    capdb_free(zErrmsg);
    capdbOsDlClose(pVfs, handle);
    return CAPDB_ERROR;
  }

  /* Append the new shared library handle to the db->aExtension array. */
  aHandle = capdbDbMallocZero(db, sizeof(handle)*(db->nExtension+1));
  if( aHandle==0 ){
    return CAPDB_NOMEM_BKPT;
  }
  if( db->nExtension>0 ){
    memcpy(aHandle, db->aExtension, sizeof(handle)*db->nExtension);
  }
  capdbDbFree(db, db->aExtension);
  db->aExtension = aHandle;

  db->aExtension[db->nExtension++] = handle;
  return CAPDB_OK;

extension_not_found:
  if( pzErrMsg ){
    nMsg += 300;
    *pzErrMsg = zErrmsg = capdb_malloc64(nMsg);
    if( zErrmsg ){
      assert( nMsg<0x7fffffff );  /* zErrmsg would be NULL if not so */
      capdb_snprintf((int)nMsg, zErrmsg,
          "unable to open shared library [%.*s]", CAPDB_MAX_PATHLEN, zFile);
      capdbOsDlError(pVfs, nMsg-1, zErrmsg);
    }
  }
  return CAPDB_ERROR;
}
int capdb_load_extension(
  capdb *db,          /* Load the extension into this database connection */
  const char *zFile,    /* Name of the shared library containing extension */
  const char *zProc,    /* Entry point.  Use "capdb_extension_init" if 0 */
  char **pzErrMsg       /* Put error message here if not 0 */
){
  int rc;
  capdb_mutex_enter(db->mutex);
  rc = capdbLoadExtension(db, zFile, zProc, pzErrMsg);
  rc = capdbApiExit(db, rc);
  capdb_mutex_leave(db->mutex);
  return rc;
}

/*
** Call this routine when the database connection is closing in order
** to clean up loaded extensions
*/
void capdbCloseExtensions(capdb *db){
  int i;
  assert( capdb_mutex_held(db->mutex) );
  for(i=0; i<db->nExtension; i++){
    capdbOsDlClose(db->pVfs, db->aExtension[i]);
  }
  capdbDbFree(db, db->aExtension);
}

/*
** Enable or disable extension loading.  Extension loading is disabled by
** default so as not to open security holes in older applications.
*/
int capdb_enable_load_extension(capdb *db, int onoff){
#ifdef CAPDB_ENABLE_API_ARMOR
  if( !capdbSafetyCheckOk(db) ) return CAPDB_MISUSE_BKPT;
#endif
  capdb_mutex_enter(db->mutex);
  if( onoff ){
    db->flags |= CAPDB_LoadExtension|CAPDB_LoadExtFunc;
  }else{
    db->flags &= ~(u64)(CAPDB_LoadExtension|CAPDB_LoadExtFunc);
  }
  capdb_mutex_leave(db->mutex);
  return CAPDB_OK;
}

#endif /* !defined(CAPDB_OMIT_LOAD_EXTENSION) */

/*
** The following object holds the list of automatically loaded
** extensions.
**
** This list is shared across threads.  The CAPDB_MUTEX_STATIC_MAIN
** mutex must be held while accessing this list.
*/
typedef struct capdbAutoExtList capdbAutoExtList;
static CAPDB_WSD struct capdbAutoExtList {
  u32 nExt;              /* Number of entries in aExt[] */
  void (**aExt)(void);   /* Pointers to the extension init functions */
} capdbAutoext = { 0, 0 };

/* The "wsdAutoext" macro will resolve to the autoextension
** state vector.  If writable static data is unsupported on the target,
** we have to locate the state vector at run-time.  In the more common
** case where writable static data is supported, wsdStat can refer directly
** to the "capdbAutoext" state vector declared above.
*/
#ifdef CAPDB_OMIT_WSD
# define wsdAutoextInit \
  capdbAutoExtList *x = &GLOBAL(capdbAutoExtList,capdbAutoext)
# define wsdAutoext x[0]
#else
# define wsdAutoextInit
# define wsdAutoext capdbAutoext
#endif


/*
** Register a statically linked extension that is automatically
** loaded by every new database connection.
*/
int capdb_auto_extension(
  void (*xInit)(void)
){
  int rc = CAPDB_OK;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( xInit==0 ) return CAPDB_MISUSE_BKPT;
#endif
#ifndef CAPDB_OMIT_AUTOINIT
  rc = capdb_initialize();
  if( rc ){
    return rc;
  }else
#endif
  {
    u32 i;
#if CAPDB_THREADSAFE
    capdb_mutex *mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN);
#endif
    wsdAutoextInit;
    capdb_mutex_enter(mutex);
    for(i=0; i<wsdAutoext.nExt; i++){
      if( wsdAutoext.aExt[i]==xInit ) break;
    }
    if( i==wsdAutoext.nExt ){
      u64 nByte = (wsdAutoext.nExt+1)*sizeof(wsdAutoext.aExt[0]);
      void (**aNew)(void);
      aNew = capdb_realloc64(wsdAutoext.aExt, nByte);
      if( aNew==0 ){
        rc = CAPDB_NOMEM_BKPT;
      }else{
        wsdAutoext.aExt = aNew;
        wsdAutoext.aExt[wsdAutoext.nExt] = xInit;
        wsdAutoext.nExt++;
      }
    }
    capdb_mutex_leave(mutex);
    assert( (rc&0xff)==rc );
    return rc;
  }
}

/*
** Cancel a prior call to capdb_auto_extension.  Remove xInit from the
** set of routines that is invoked for each new database connection, if it
** is currently on the list.  If xInit is not on the list, then this
** routine is a no-op.
**
** Return 1 if xInit was found on the list and removed.  Return 0 if xInit
** was not on the list.
*/
int capdb_cancel_auto_extension(
  void (*xInit)(void)
){
#if CAPDB_THREADSAFE
  capdb_mutex *mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN);
#endif
  int i;
  int n = 0;
  wsdAutoextInit;
#ifdef CAPDB_ENABLE_API_ARMOR
  if( xInit==0 ) return 0;
#endif
  capdb_mutex_enter(mutex);
  for(i=(int)wsdAutoext.nExt-1; i>=0; i--){
    if( wsdAutoext.aExt[i]==xInit ){
      wsdAutoext.nExt--;
      wsdAutoext.aExt[i] = wsdAutoext.aExt[wsdAutoext.nExt];
      n++;
      break;
    }
  }
  capdb_mutex_leave(mutex);
  return n;
}

/*
** Reset the automatic extension loading mechanism.
*/
void capdb_reset_auto_extension(void){
#ifndef CAPDB_OMIT_AUTOINIT
  if( capdb_initialize()==CAPDB_OK )
#endif
  {
#if CAPDB_THREADSAFE
    capdb_mutex *mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN);
#endif
    wsdAutoextInit;
    capdb_mutex_enter(mutex);
    capdb_free(wsdAutoext.aExt);
    wsdAutoext.aExt = 0;
    wsdAutoext.nExt = 0;
    capdb_mutex_leave(mutex);
  }
}

/*
** Load all automatic extensions.
**
** If anything goes wrong, set an error in the database connection.
*/
void capdbAutoLoadExtensions(capdb *db){
  u32 i;
  int go = 1;
  int rc;
  capdb_loadext_entry xInit;

  wsdAutoextInit;
  if( wsdAutoext.nExt==0 ){
    /* Common case: early out without every having to acquire a mutex */
    return;
  }
  for(i=0; go; i++){
    char *zErrmsg;
#if CAPDB_THREADSAFE
    capdb_mutex *mutex = capdbMutexAlloc(CAPDB_MUTEX_STATIC_MAIN);
#endif
#ifdef CAPDB_OMIT_LOAD_EXTENSION
    const capdb_api_routines *pThunk = 0;
#else
    const capdb_api_routines *pThunk = &capdbApis;
#endif
    capdb_mutex_enter(mutex);
    if( i>=wsdAutoext.nExt ){
      xInit = 0;
      go = 0;
    }else{
      xInit = (capdb_loadext_entry)wsdAutoext.aExt[i];
    }
    capdb_mutex_leave(mutex);
    zErrmsg = 0;
    if( xInit && (rc = xInit(db, &zErrmsg, pThunk))!=0 ){
      capdbErrorWithMsg(db, rc,
            "automatic extension loading failed: %s", zErrmsg);
      go = 0;
    }
    capdb_free(zErrmsg);
  }
}
