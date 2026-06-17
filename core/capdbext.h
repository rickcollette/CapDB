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
** This header file defines the SQLite interface for use by
** shared libraries that want to be imported as extensions into
** an SQLite instance.  Shared libraries that intend to be loaded
** as extensions by SQLite should #include this file instead of 
** capdb.h.
*/
#ifndef SQLITE3EXT_H
#define SQLITE3EXT_H
#include "capdb.h"

/*
** The following structure holds pointers to all of the SQLite API
** routines.
**
** WARNING:  In order to maintain backwards compatibility, add new
** interfaces to the end of this structure only.  If you insert new
** interfaces in the middle of this structure, then older different
** versions of SQLite will not be able to load each other's shared
** libraries!
*/
struct capdb_api_routines {
  void * (*aggregate_context)(capdb_context*,int nBytes);
  int  (*aggregate_count)(capdb_context*);
  int  (*bind_blob)(capdb_stmt*,int,const void*,int n,void(*)(void*));
  int  (*bind_double)(capdb_stmt*,int,double);
  int  (*bind_int)(capdb_stmt*,int,int);
  int  (*bind_int64)(capdb_stmt*,int,sqlite_int64);
  int  (*bind_null)(capdb_stmt*,int);
  int  (*bind_parameter_count)(capdb_stmt*);
  int  (*bind_parameter_index)(capdb_stmt*,const char*zName);
  const char * (*bind_parameter_name)(capdb_stmt*,int);
  int  (*bind_text)(capdb_stmt*,int,const char*,int n,void(*)(void*));
  int  (*bind_text16)(capdb_stmt*,int,const void*,int,void(*)(void*));
  int  (*bind_value)(capdb_stmt*,int,const capdb_value*);
  int  (*busy_handler)(capdb*,int(*)(void*,int),void*);
  int  (*busy_timeout)(capdb*,int ms);
  int  (*changes)(capdb*);
  int  (*close)(capdb*);
  int  (*collation_needed)(capdb*,void*,void(*)(void*,capdb*,
                           int eTextRep,const char*));
  int  (*collation_needed16)(capdb*,void*,void(*)(void*,capdb*,
                             int eTextRep,const void*));
  const void * (*column_blob)(capdb_stmt*,int iCol);
  int  (*column_bytes)(capdb_stmt*,int iCol);
  int  (*column_bytes16)(capdb_stmt*,int iCol);
  int  (*column_count)(capdb_stmt*pStmt);
  const char * (*column_database_name)(capdb_stmt*,int);
  const void * (*column_database_name16)(capdb_stmt*,int);
  const char * (*column_decltype)(capdb_stmt*,int i);
  const void * (*column_decltype16)(capdb_stmt*,int);
  double  (*column_double)(capdb_stmt*,int iCol);
  int  (*column_int)(capdb_stmt*,int iCol);
  sqlite_int64  (*column_int64)(capdb_stmt*,int iCol);
  const char * (*column_name)(capdb_stmt*,int);
  const void * (*column_name16)(capdb_stmt*,int);
  const char * (*column_origin_name)(capdb_stmt*,int);
  const void * (*column_origin_name16)(capdb_stmt*,int);
  const char * (*column_table_name)(capdb_stmt*,int);
  const void * (*column_table_name16)(capdb_stmt*,int);
  const unsigned char * (*column_text)(capdb_stmt*,int iCol);
  const void * (*column_text16)(capdb_stmt*,int iCol);
  int  (*column_type)(capdb_stmt*,int iCol);
  capdb_value* (*column_value)(capdb_stmt*,int iCol);
  void * (*commit_hook)(capdb*,int(*)(void*),void*);
  int  (*complete)(const char*sql);
  int  (*complete16)(const void*sql);
  int  (*create_collation)(capdb*,const char*,int,void*,
                           int(*)(void*,int,const void*,int,const void*));
  int  (*create_collation16)(capdb*,const void*,int,void*,
                             int(*)(void*,int,const void*,int,const void*));
  int  (*create_function)(capdb*,const char*,int,int,void*,
                          void (*xFunc)(capdb_context*,int,capdb_value**),
                          void (*xStep)(capdb_context*,int,capdb_value**),
                          void (*xFinal)(capdb_context*));
  int  (*create_function16)(capdb*,const void*,int,int,void*,
                            void (*xFunc)(capdb_context*,int,capdb_value**),
                            void (*xStep)(capdb_context*,int,capdb_value**),
                            void (*xFinal)(capdb_context*));
  int (*create_module)(capdb*,const char*,const capdb_module*,void*);
  int  (*data_count)(capdb_stmt*pStmt);
  capdb * (*db_handle)(capdb_stmt*);
  int (*declare_vtab)(capdb*,const char*);
  int  (*enable_shared_cache)(int);
  int  (*errcode)(capdb*db);
  const char * (*errmsg)(capdb*);
  const void * (*errmsg16)(capdb*);
  int  (*exec)(capdb*,const char*,capdb_callback,void*,char**);
  int  (*expired)(capdb_stmt*);
  int  (*finalize)(capdb_stmt*pStmt);
  void  (*free)(void*);
  void  (*free_table)(char**result);
  int  (*get_autocommit)(capdb*);
  void * (*get_auxdata)(capdb_context*,int);
  int  (*get_table)(capdb*,const char*,char***,int*,int*,char**);
  int  (*global_recover)(void);
  void  (*interruptx)(capdb*);
  sqlite_int64  (*last_insert_rowid)(capdb*);
  const char * (*libversion)(void);
  int  (*libversion_number)(void);
  void *(*malloc)(int);
  char * (*mprintf)(const char*,...);
  int  (*open)(const char*,capdb**);
  int  (*open16)(const void*,capdb**);
  int  (*prepare)(capdb*,const char*,int,capdb_stmt**,const char**);
  int  (*prepare16)(capdb*,const void*,int,capdb_stmt**,const void**);
  void * (*profile)(capdb*,void(*)(void*,const char*,sqlite_uint64),void*);
  void  (*progress_handler)(capdb*,int,int(*)(void*),void*);
  void *(*realloc)(void*,int);
  int  (*reset)(capdb_stmt*pStmt);
  void  (*result_blob)(capdb_context*,const void*,int,void(*)(void*));
  void  (*result_double)(capdb_context*,double);
  void  (*result_error)(capdb_context*,const char*,int);
  void  (*result_error16)(capdb_context*,const void*,int);
  void  (*result_int)(capdb_context*,int);
  void  (*result_int64)(capdb_context*,sqlite_int64);
  void  (*result_null)(capdb_context*);
  void  (*result_text)(capdb_context*,const char*,int,void(*)(void*));
  void  (*result_text16)(capdb_context*,const void*,int,void(*)(void*));
  void  (*result_text16be)(capdb_context*,const void*,int,void(*)(void*));
  void  (*result_text16le)(capdb_context*,const void*,int,void(*)(void*));
  void  (*result_value)(capdb_context*,capdb_value*);
  void * (*rollback_hook)(capdb*,void(*)(void*),void*);
  int  (*set_authorizer)(capdb*,int(*)(void*,int,const char*,const char*,
                         const char*,const char*),void*);
  void  (*set_auxdata)(capdb_context*,int,void*,void (*)(void*));
  char * (*xsnprintf)(int,char*,const char*,...);
  int  (*step)(capdb_stmt*);
  int  (*table_column_metadata)(capdb*,const char*,const char*,const char*,
                                char const**,char const**,int*,int*,int*);
  void  (*thread_cleanup)(void);
  int  (*total_changes)(capdb*);
  void * (*trace)(capdb*,void(*xTrace)(void*,const char*),void*);
  int  (*transfer_bindings)(capdb_stmt*,capdb_stmt*);
  void * (*update_hook)(capdb*,void(*)(void*,int ,char const*,char const*,
                                         sqlite_int64),void*);
  void * (*user_data)(capdb_context*);
  const void * (*value_blob)(capdb_value*);
  int  (*value_bytes)(capdb_value*);
  int  (*value_bytes16)(capdb_value*);
  double  (*value_double)(capdb_value*);
  int  (*value_int)(capdb_value*);
  sqlite_int64  (*value_int64)(capdb_value*);
  int  (*value_numeric_type)(capdb_value*);
  const unsigned char * (*value_text)(capdb_value*);
  const void * (*value_text16)(capdb_value*);
  const void * (*value_text16be)(capdb_value*);
  const void * (*value_text16le)(capdb_value*);
  int  (*value_type)(capdb_value*);
  char *(*vmprintf)(const char*,va_list);
  /* Added ??? */
  int (*overload_function)(capdb*, const char *zFuncName, int nArg);
  /* Added by 3.3.13 */
  int (*prepare_v2)(capdb*,const char*,int,capdb_stmt**,const char**);
  int (*prepare16_v2)(capdb*,const void*,int,capdb_stmt**,const void**);
  int (*clear_bindings)(capdb_stmt*);
  /* Added by 3.4.1 */
  int (*create_module_v2)(capdb*,const char*,const capdb_module*,void*,
                          void (*xDestroy)(void *));
  /* Added by 3.5.0 */
  int (*bind_zeroblob)(capdb_stmt*,int,int);
  int (*blob_bytes)(capdb_blob*);
  int (*blob_close)(capdb_blob*);
  int (*blob_open)(capdb*,const char*,const char*,const char*,capdb_int64,
                   int,capdb_blob**);
  int (*blob_read)(capdb_blob*,void*,int,int);
  int (*blob_write)(capdb_blob*,const void*,int,int);
  int (*create_collation_v2)(capdb*,const char*,int,void*,
                             int(*)(void*,int,const void*,int,const void*),
                             void(*)(void*));
  int (*file_control)(capdb*,const char*,int,void*);
  capdb_int64 (*memory_highwater)(int);
  capdb_int64 (*memory_used)(void);
  capdb_mutex *(*mutex_alloc)(int);
  void (*mutex_enter)(capdb_mutex*);
  void (*mutex_free)(capdb_mutex*);
  void (*mutex_leave)(capdb_mutex*);
  int (*mutex_try)(capdb_mutex*);
  int (*open_v2)(const char*,capdb**,int,const char*);
  int (*release_memory)(int);
  void (*result_error_nomem)(capdb_context*);
  void (*result_error_toobig)(capdb_context*);
  int (*sleep)(int);
  void (*soft_heap_limit)(int);
  capdb_vfs *(*vfs_find)(const char*);
  int (*vfs_register)(capdb_vfs*,int);
  int (*vfs_unregister)(capdb_vfs*);
  int (*xthreadsafe)(void);
  void (*result_zeroblob)(capdb_context*,int);
  void (*result_error_code)(capdb_context*,int);
  int (*test_control)(int, ...);
  void (*randomness)(int,void*);
  capdb *(*context_db_handle)(capdb_context*);
  int (*extended_result_codes)(capdb*,int);
  int (*limit)(capdb*,int,int);
  capdb_stmt *(*next_stmt)(capdb*,capdb_stmt*);
  const char *(*sql)(capdb_stmt*);
  int (*status)(int,int*,int*,int);
  int (*backup_finish)(capdb_backup*);
  capdb_backup *(*backup_init)(capdb*,const char*,capdb*,const char*);
  int (*backup_pagecount)(capdb_backup*);
  int (*backup_remaining)(capdb_backup*);
  int (*backup_step)(capdb_backup*,int);
  const char *(*compileoption_get)(int);
  int (*compileoption_used)(const char*);
  int (*create_function_v2)(capdb*,const char*,int,int,void*,
                            void (*xFunc)(capdb_context*,int,capdb_value**),
                            void (*xStep)(capdb_context*,int,capdb_value**),
                            void (*xFinal)(capdb_context*),
                            void(*xDestroy)(void*));
  int (*db_config)(capdb*,int,...);
  capdb_mutex *(*db_mutex)(capdb*);
  int (*db_status)(capdb*,int,int*,int*,int);
  int (*extended_errcode)(capdb*);
  void (*log)(int,const char*,...);
  capdb_int64 (*soft_heap_limit64)(capdb_int64);
  const char *(*sourceid)(void);
  int (*stmt_status)(capdb_stmt*,int,int);
  int (*strnicmp)(const char*,const char*,int);
  int (*unlock_notify)(capdb*,void(*)(void**,int),void*);
  int (*wal_autocheckpoint)(capdb*,int);
  int (*wal_checkpoint)(capdb*,const char*);
  void *(*wal_hook)(capdb*,int(*)(void*,capdb*,const char*,int),void*);
  int (*blob_reopen)(capdb_blob*,capdb_int64);
  int (*vtab_config)(capdb*,int op,...);
  int (*vtab_on_conflict)(capdb*);
  /* Version 3.7.16 and later */
  int (*close_v2)(capdb*);
  const char *(*db_filename)(capdb*,const char*);
  int (*db_readonly)(capdb*,const char*);
  int (*db_release_memory)(capdb*);
  const char *(*errstr)(int);
  int (*stmt_busy)(capdb_stmt*);
  int (*stmt_readonly)(capdb_stmt*);
  int (*stricmp)(const char*,const char*);
  int (*uri_boolean)(const char*,const char*,int);
  capdb_int64 (*uri_int64)(const char*,const char*,capdb_int64);
  const char *(*uri_parameter)(const char*,const char*);
  char *(*xvsnprintf)(int,char*,const char*,va_list);
  int (*wal_checkpoint_v2)(capdb*,const char*,int,int*,int*);
  /* Version 3.8.7 and later */
  int (*auto_extension)(void(*)(void));
  int (*bind_blob64)(capdb_stmt*,int,const void*,capdb_uint64,
                     void(*)(void*));
  int (*bind_text64)(capdb_stmt*,int,const char*,capdb_uint64,
                      void(*)(void*),unsigned char);
  int (*cancel_auto_extension)(void(*)(void));
  int (*load_extension)(capdb*,const char*,const char*,char**);
  void *(*malloc64)(capdb_uint64);
  capdb_uint64 (*msize)(void*);
  void *(*realloc64)(void*,capdb_uint64);
  void (*reset_auto_extension)(void);
  void (*result_blob64)(capdb_context*,const void*,capdb_uint64,
                        void(*)(void*));
  void (*result_text64)(capdb_context*,const char*,capdb_uint64,
                         void(*)(void*), unsigned char);
  int (*strglob)(const char*,const char*);
  /* Version 3.8.11 and later */
  capdb_value *(*value_dup)(const capdb_value*);
  void (*value_free)(capdb_value*);
  int (*result_zeroblob64)(capdb_context*,capdb_uint64);
  int (*bind_zeroblob64)(capdb_stmt*, int, capdb_uint64);
  /* Version 3.9.0 and later */
  unsigned int (*value_subtype)(capdb_value*);
  void (*result_subtype)(capdb_context*,unsigned int);
  /* Version 3.10.0 and later */
  int (*status64)(int,capdb_int64*,capdb_int64*,int);
  int (*strlike)(const char*,const char*,unsigned int);
  int (*db_cacheflush)(capdb*);
  /* Version 3.12.0 and later */
  int (*system_errno)(capdb*);
  /* Version 3.14.0 and later */
  int (*trace_v2)(capdb*,unsigned,int(*)(unsigned,void*,void*,void*),void*);
  char *(*expanded_sql)(capdb_stmt*);
  /* Version 3.18.0 and later */
  void (*set_last_insert_rowid)(capdb*,capdb_int64);
  /* Version 3.20.0 and later */
  int (*prepare_v3)(capdb*,const char*,int,unsigned int,
                    capdb_stmt**,const char**);
  int (*prepare16_v3)(capdb*,const void*,int,unsigned int,
                      capdb_stmt**,const void**);
  int (*bind_pointer)(capdb_stmt*,int,void*,const char*,void(*)(void*));
  void (*result_pointer)(capdb_context*,void*,const char*,void(*)(void*));
  void *(*value_pointer)(capdb_value*,const char*);
  int (*vtab_nochange)(capdb_context*);
  int (*value_nochange)(capdb_value*);
  const char *(*vtab_collation)(capdb_index_info*,int);
  /* Version 3.24.0 and later */
  int (*keyword_count)(void);
  int (*keyword_name)(int,const char**,int*);
  int (*keyword_check)(const char*,int);
  capdb_str *(*str_new)(capdb*);
  char *(*str_finish)(capdb_str*);
  void (*str_appendf)(capdb_str*, const char *zFormat, ...);
  void (*str_vappendf)(capdb_str*, const char *zFormat, va_list);
  void (*str_append)(capdb_str*, const char *zIn, int N);
  void (*str_appendall)(capdb_str*, const char *zIn);
  void (*str_appendchar)(capdb_str*, int N, char C);
  void (*str_reset)(capdb_str*);
  int (*str_errcode)(capdb_str*);
  int (*str_length)(capdb_str*);
  char *(*str_value)(capdb_str*);
  /* Version 3.25.0 and later */
  int (*create_window_function)(capdb*,const char*,int,int,void*,
                            void (*xStep)(capdb_context*,int,capdb_value**),
                            void (*xFinal)(capdb_context*),
                            void (*xValue)(capdb_context*),
                            void (*xInv)(capdb_context*,int,capdb_value**),
                            void(*xDestroy)(void*));
  /* Version 3.26.0 and later */
  const char *(*normalized_sql)(capdb_stmt*);
  /* Version 3.28.0 and later */
  int (*stmt_isexplain)(capdb_stmt*);
  int (*value_frombind)(capdb_value*);
  /* Version 3.30.0 and later */
  int (*drop_modules)(capdb*,const char**);
  /* Version 3.31.0 and later */
  capdb_int64 (*hard_heap_limit64)(capdb_int64);
  const char *(*uri_key)(const char*,int);
  const char *(*filename_database)(const char*);
  const char *(*filename_journal)(const char*);
  const char *(*filename_wal)(const char*);
  /* Version 3.32.0 and later */
  const char *(*create_filename)(const char*,const char*,const char*,
                           int,const char**);
  void (*free_filename)(const char*);
  capdb_file *(*database_file_object)(const char*);
  /* Version 3.34.0 and later */
  int (*txn_state)(capdb*,const char*);
  /* Version 3.36.1 and later */
  capdb_int64 (*changes64)(capdb*);
  capdb_int64 (*total_changes64)(capdb*);
  /* Version 3.37.0 and later */
  int (*autovacuum_pages)(capdb*,
     unsigned int(*)(void*,const char*,unsigned int,unsigned int,unsigned int),
     void*, void(*)(void*));
  /* Version 3.38.0 and later */
  int (*error_offset)(capdb*);
  int (*vtab_rhs_value)(capdb_index_info*,int,capdb_value**);
  int (*vtab_distinct)(capdb_index_info*);
  int (*vtab_in)(capdb_index_info*,int,int);
  int (*vtab_in_first)(capdb_value*,capdb_value**);
  int (*vtab_in_next)(capdb_value*,capdb_value**);
  /* Version 3.39.0 and later */
  int (*deserialize)(capdb*,const char*,unsigned char*,
                     capdb_int64,capdb_int64,unsigned);
  unsigned char *(*serialize)(capdb*,const char *,capdb_int64*,
                              unsigned int);
  const char *(*db_name)(capdb*,int);
  /* Version 3.40.0 and later */
  int (*value_encoding)(capdb_value*);
  /* Version 3.41.0 and later */
  int (*is_interrupted)(capdb*);
  /* Version 3.43.0 and later */
  int (*stmt_explain)(capdb_stmt*,int);
  /* Version 3.44.0 and later */
  void *(*get_clientdata)(capdb*,const char*);
  int (*set_clientdata)(capdb*, const char*, void*, void(*)(void*));
  /* Version 3.50.0 and later */
  int (*setlk_timeout)(capdb*,int,int);
  /* Version 3.51.0 and later */
  int (*set_errmsg)(capdb*,int,const char*);
  int (*db_status64)(capdb*,int,capdb_int64*,capdb_int64*,int);
  /* Version 3.52.0 and later */
  void (*str_truncate)(capdb_str*,int);
  void (*str_free)(capdb_str*);
  int (*carray_bind)(capdb_stmt*,int,void*,int,int,void(*)(void*));
  int (*carray_bind_v2)(capdb_stmt*,int,void*,int,int,void(*)(void*),void*);
  /* Version 3.54.0 and later */
  capdb_int64 (*incomplete)(const char*);
};

/*
** This is the function signature used for all extension entry points.  It
** is also defined in the file "loadext.c".
*/
typedef int (*capdb_loadext_entry)(
  capdb *db,                       /* Handle to the database. */
  char **pzErrMsg,                   /* Used to set error string on failure. */
  const capdb_api_routines *pThunk /* Extension API function pointers. */
);

/*
** The following macros redefine the API routines so that they are
** redirected through the global capdb_api structure.
**
** This header file is also used by the loadext.c source file
** (part of the main SQLite library - not an extension) so that
** it can get access to the capdb_api_routines structure
** definition.  But the main library does not want to redefine
** the API.  So the redefinition macros are only valid if the
** CAPDB_CORE macros is undefined.
*/
#if !defined(CAPDB_CORE) && !defined(CAPDB_OMIT_LOAD_EXTENSION)
#define capdb_aggregate_context      capdb_api->aggregate_context
#ifndef CAPDB_OMIT_DEPRECATED
#define capdb_aggregate_count        capdb_api->aggregate_count
#endif
#define capdb_bind_blob              capdb_api->bind_blob
#define capdb_bind_double            capdb_api->bind_double
#define capdb_bind_int               capdb_api->bind_int
#define capdb_bind_int64             capdb_api->bind_int64
#define capdb_bind_null              capdb_api->bind_null
#define capdb_bind_parameter_count   capdb_api->bind_parameter_count
#define capdb_bind_parameter_index   capdb_api->bind_parameter_index
#define capdb_bind_parameter_name    capdb_api->bind_parameter_name
#define capdb_bind_text              capdb_api->bind_text
#define capdb_bind_text16            capdb_api->bind_text16
#define capdb_bind_value             capdb_api->bind_value
#define capdb_busy_handler           capdb_api->busy_handler
#define capdb_busy_timeout           capdb_api->busy_timeout
#define capdb_changes                capdb_api->changes
#define capdb_close                  capdb_api->close
#define capdb_collation_needed       capdb_api->collation_needed
#define capdb_collation_needed16     capdb_api->collation_needed16
#define capdb_column_blob            capdb_api->column_blob
#define capdb_column_bytes           capdb_api->column_bytes
#define capdb_column_bytes16         capdb_api->column_bytes16
#define capdb_column_count           capdb_api->column_count
#define capdb_column_database_name   capdb_api->column_database_name
#define capdb_column_database_name16 capdb_api->column_database_name16
#define capdb_column_decltype        capdb_api->column_decltype
#define capdb_column_decltype16      capdb_api->column_decltype16
#define capdb_column_double          capdb_api->column_double
#define capdb_column_int             capdb_api->column_int
#define capdb_column_int64           capdb_api->column_int64
#define capdb_column_name            capdb_api->column_name
#define capdb_column_name16          capdb_api->column_name16
#define capdb_column_origin_name     capdb_api->column_origin_name
#define capdb_column_origin_name16   capdb_api->column_origin_name16
#define capdb_column_table_name      capdb_api->column_table_name
#define capdb_column_table_name16    capdb_api->column_table_name16
#define capdb_column_text            capdb_api->column_text
#define capdb_column_text16          capdb_api->column_text16
#define capdb_column_type            capdb_api->column_type
#define capdb_column_value           capdb_api->column_value
#define capdb_commit_hook            capdb_api->commit_hook
#define capdb_complete               capdb_api->complete
#define capdb_complete16             capdb_api->complete16
#define capdb_create_collation       capdb_api->create_collation
#define capdb_create_collation16     capdb_api->create_collation16
#define capdb_create_function        capdb_api->create_function
#define capdb_create_function16      capdb_api->create_function16
#define capdb_create_module          capdb_api->create_module
#define capdb_create_module_v2       capdb_api->create_module_v2
#define capdb_data_count             capdb_api->data_count
#define capdb_db_handle              capdb_api->db_handle
#define capdb_declare_vtab           capdb_api->declare_vtab
#define capdb_enable_shared_cache    capdb_api->enable_shared_cache
#define capdb_errcode                capdb_api->errcode
#define capdb_errmsg                 capdb_api->errmsg
#define capdb_errmsg16               capdb_api->errmsg16
#define capdb_exec                   capdb_api->exec
#ifndef CAPDB_OMIT_DEPRECATED
#define capdb_expired                capdb_api->expired
#endif
#define capdb_finalize               capdb_api->finalize
#define capdb_free                   capdb_api->free
#define capdb_free_table             capdb_api->free_table
#define capdb_get_autocommit         capdb_api->get_autocommit
#define capdb_get_auxdata            capdb_api->get_auxdata
#define capdb_get_table              capdb_api->get_table
#ifndef CAPDB_OMIT_DEPRECATED
#define capdb_global_recover         capdb_api->global_recover
#endif
#define capdb_interrupt              capdb_api->interruptx
#define capdb_last_insert_rowid      capdb_api->last_insert_rowid
#define capdb_libversion             capdb_api->libversion
#define capdb_libversion_number      capdb_api->libversion_number
#define capdb_malloc                 capdb_api->malloc
#define capdb_mprintf                capdb_api->mprintf
#define capdb_open                   capdb_api->open
#define capdb_open16                 capdb_api->open16
#define capdb_prepare                capdb_api->prepare
#define capdb_prepare16              capdb_api->prepare16
#define capdb_prepare_v2             capdb_api->prepare_v2
#define capdb_prepare16_v2           capdb_api->prepare16_v2
#define capdb_profile                capdb_api->profile
#define capdb_progress_handler       capdb_api->progress_handler
#define capdb_realloc                capdb_api->realloc
#define capdb_reset                  capdb_api->reset
#define capdb_result_blob            capdb_api->result_blob
#define capdb_result_double          capdb_api->result_double
#define capdb_result_error           capdb_api->result_error
#define capdb_result_error16         capdb_api->result_error16
#define capdb_result_int             capdb_api->result_int
#define capdb_result_int64           capdb_api->result_int64
#define capdb_result_null            capdb_api->result_null
#define capdb_result_text            capdb_api->result_text
#define capdb_result_text16          capdb_api->result_text16
#define capdb_result_text16be        capdb_api->result_text16be
#define capdb_result_text16le        capdb_api->result_text16le
#define capdb_result_value           capdb_api->result_value
#define capdb_rollback_hook          capdb_api->rollback_hook
#define capdb_set_authorizer         capdb_api->set_authorizer
#define capdb_set_auxdata            capdb_api->set_auxdata
#define capdb_snprintf               capdb_api->xsnprintf
#define capdb_step                   capdb_api->step
#define capdb_table_column_metadata  capdb_api->table_column_metadata
#define capdb_thread_cleanup         capdb_api->thread_cleanup
#define capdb_total_changes          capdb_api->total_changes
#define capdb_trace                  capdb_api->trace
#ifndef CAPDB_OMIT_DEPRECATED
#define capdb_transfer_bindings      capdb_api->transfer_bindings
#endif
#define capdb_update_hook            capdb_api->update_hook
#define capdb_user_data              capdb_api->user_data
#define capdb_value_blob             capdb_api->value_blob
#define capdb_value_bytes            capdb_api->value_bytes
#define capdb_value_bytes16          capdb_api->value_bytes16
#define capdb_value_double           capdb_api->value_double
#define capdb_value_int              capdb_api->value_int
#define capdb_value_int64            capdb_api->value_int64
#define capdb_value_numeric_type     capdb_api->value_numeric_type
#define capdb_value_text             capdb_api->value_text
#define capdb_value_text16           capdb_api->value_text16
#define capdb_value_text16be         capdb_api->value_text16be
#define capdb_value_text16le         capdb_api->value_text16le
#define capdb_value_type             capdb_api->value_type
#define capdb_vmprintf               capdb_api->vmprintf
#define capdb_vsnprintf              capdb_api->xvsnprintf
#define capdb_overload_function      capdb_api->overload_function
#define capdb_prepare_v2             capdb_api->prepare_v2
#define capdb_prepare16_v2           capdb_api->prepare16_v2
#define capdb_clear_bindings         capdb_api->clear_bindings
#define capdb_bind_zeroblob          capdb_api->bind_zeroblob
#define capdb_blob_bytes             capdb_api->blob_bytes
#define capdb_blob_close             capdb_api->blob_close
#define capdb_blob_open              capdb_api->blob_open
#define capdb_blob_read              capdb_api->blob_read
#define capdb_blob_write             capdb_api->blob_write
#define capdb_create_collation_v2    capdb_api->create_collation_v2
#define capdb_file_control           capdb_api->file_control
#define capdb_memory_highwater       capdb_api->memory_highwater
#define capdb_memory_used            capdb_api->memory_used
#define capdb_mutex_alloc            capdb_api->mutex_alloc
#define capdb_mutex_enter            capdb_api->mutex_enter
#define capdb_mutex_free             capdb_api->mutex_free
#define capdb_mutex_leave            capdb_api->mutex_leave
#define capdb_mutex_try              capdb_api->mutex_try
#define capdb_open_v2                capdb_api->open_v2
#define capdb_release_memory         capdb_api->release_memory
#define capdb_result_error_nomem     capdb_api->result_error_nomem
#define capdb_result_error_toobig    capdb_api->result_error_toobig
#define capdb_sleep                  capdb_api->sleep
#define capdb_soft_heap_limit        capdb_api->soft_heap_limit
#define capdb_vfs_find               capdb_api->vfs_find
#define capdb_vfs_register           capdb_api->vfs_register
#define capdb_vfs_unregister         capdb_api->vfs_unregister
#define capdb_threadsafe             capdb_api->xthreadsafe
#define capdb_result_zeroblob        capdb_api->result_zeroblob
#define capdb_result_error_code      capdb_api->result_error_code
#define capdb_test_control           capdb_api->test_control
#define capdb_randomness             capdb_api->randomness
#define capdb_context_db_handle      capdb_api->context_db_handle
#define capdb_extended_result_codes  capdb_api->extended_result_codes
#define capdb_limit                  capdb_api->limit
#define capdb_next_stmt              capdb_api->next_stmt
#define capdb_sql                    capdb_api->sql
#define capdb_status                 capdb_api->status
#define capdb_backup_finish          capdb_api->backup_finish
#define capdb_backup_init            capdb_api->backup_init
#define capdb_backup_pagecount       capdb_api->backup_pagecount
#define capdb_backup_remaining       capdb_api->backup_remaining
#define capdb_backup_step            capdb_api->backup_step
#define capdb_compileoption_get      capdb_api->compileoption_get
#define capdb_compileoption_used     capdb_api->compileoption_used
#define capdb_create_function_v2     capdb_api->create_function_v2
#define capdb_db_config              capdb_api->db_config
#define capdb_db_mutex               capdb_api->db_mutex
#define capdb_db_status              capdb_api->db_status
#define capdb_extended_errcode       capdb_api->extended_errcode
#define capdb_log                    capdb_api->log
#define capdb_soft_heap_limit64      capdb_api->soft_heap_limit64
#define capdb_sourceid               capdb_api->sourceid
#define capdb_stmt_status            capdb_api->stmt_status
#define capdb_strnicmp               capdb_api->strnicmp
#define capdb_unlock_notify          capdb_api->unlock_notify
#define capdb_wal_autocheckpoint     capdb_api->wal_autocheckpoint
#define capdb_wal_checkpoint         capdb_api->wal_checkpoint
#define capdb_wal_hook               capdb_api->wal_hook
#define capdb_blob_reopen            capdb_api->blob_reopen
#define capdb_vtab_config            capdb_api->vtab_config
#define capdb_vtab_on_conflict       capdb_api->vtab_on_conflict
/* Version 3.7.16 and later */
#define capdb_close_v2               capdb_api->close_v2
#define capdb_db_filename            capdb_api->db_filename
#define capdb_db_readonly            capdb_api->db_readonly
#define capdb_db_release_memory      capdb_api->db_release_memory
#define capdb_errstr                 capdb_api->errstr
#define capdb_stmt_busy              capdb_api->stmt_busy
#define capdb_stmt_readonly          capdb_api->stmt_readonly
#define capdb_stricmp                capdb_api->stricmp
#define capdb_uri_boolean            capdb_api->uri_boolean
#define capdb_uri_int64              capdb_api->uri_int64
#define capdb_uri_parameter          capdb_api->uri_parameter
#define capdb_uri_vsnprintf          capdb_api->xvsnprintf
#define capdb_wal_checkpoint_v2      capdb_api->wal_checkpoint_v2
/* Version 3.8.7 and later */
#define capdb_auto_extension         capdb_api->auto_extension
#define capdb_bind_blob64            capdb_api->bind_blob64
#define capdb_bind_text64            capdb_api->bind_text64
#define capdb_cancel_auto_extension  capdb_api->cancel_auto_extension
#define capdb_load_extension         capdb_api->load_extension
#define capdb_malloc64               capdb_api->malloc64
#define capdb_msize                  capdb_api->msize
#define capdb_realloc64              capdb_api->realloc64
#define capdb_reset_auto_extension   capdb_api->reset_auto_extension
#define capdb_result_blob64          capdb_api->result_blob64
#define capdb_result_text64          capdb_api->result_text64
#define capdb_strglob                capdb_api->strglob
/* Version 3.8.11 and later */
#define capdb_value_dup              capdb_api->value_dup
#define capdb_value_free             capdb_api->value_free
#define capdb_result_zeroblob64      capdb_api->result_zeroblob64
#define capdb_bind_zeroblob64        capdb_api->bind_zeroblob64
/* Version 3.9.0 and later */
#define capdb_value_subtype          capdb_api->value_subtype
#define capdb_result_subtype         capdb_api->result_subtype
/* Version 3.10.0 and later */
#define capdb_status64               capdb_api->status64
#define capdb_strlike                capdb_api->strlike
#define capdb_db_cacheflush          capdb_api->db_cacheflush
/* Version 3.12.0 and later */
#define capdb_system_errno           capdb_api->system_errno
/* Version 3.14.0 and later */
#define capdb_trace_v2               capdb_api->trace_v2
#define capdb_expanded_sql           capdb_api->expanded_sql
/* Version 3.18.0 and later */
#define capdb_set_last_insert_rowid  capdb_api->set_last_insert_rowid
/* Version 3.20.0 and later */
#define capdb_prepare_v3             capdb_api->prepare_v3
#define capdb_prepare16_v3           capdb_api->prepare16_v3
#define capdb_bind_pointer           capdb_api->bind_pointer
#define capdb_result_pointer         capdb_api->result_pointer
#define capdb_value_pointer          capdb_api->value_pointer
/* Version 3.22.0 and later */
#define capdb_vtab_nochange          capdb_api->vtab_nochange
#define capdb_value_nochange         capdb_api->value_nochange
#define capdb_vtab_collation         capdb_api->vtab_collation
/* Version 3.24.0 and later */
#define capdb_keyword_count          capdb_api->keyword_count
#define capdb_keyword_name           capdb_api->keyword_name
#define capdb_keyword_check          capdb_api->keyword_check
#define capdb_str_new                capdb_api->str_new
#define capdb_str_finish             capdb_api->str_finish
#define capdb_str_appendf            capdb_api->str_appendf
#define capdb_str_vappendf           capdb_api->str_vappendf
#define capdb_str_append             capdb_api->str_append
#define capdb_str_appendall          capdb_api->str_appendall
#define capdb_str_appendchar         capdb_api->str_appendchar
#define capdb_str_reset              capdb_api->str_reset
#define capdb_str_errcode            capdb_api->str_errcode
#define capdb_str_length             capdb_api->str_length
#define capdb_str_value              capdb_api->str_value
/* Version 3.25.0 and later */
#define capdb_create_window_function capdb_api->create_window_function
/* Version 3.26.0 and later */
#define capdb_normalized_sql         capdb_api->normalized_sql
/* Version 3.28.0 and later */
#define capdb_stmt_isexplain         capdb_api->stmt_isexplain
#define capdb_value_frombind         capdb_api->value_frombind
/* Version 3.30.0 and later */
#define capdb_drop_modules           capdb_api->drop_modules
/* Version 3.31.0 and later */
#define capdb_hard_heap_limit64      capdb_api->hard_heap_limit64
#define capdb_uri_key                capdb_api->uri_key
#define capdb_filename_database      capdb_api->filename_database
#define capdb_filename_journal       capdb_api->filename_journal
#define capdb_filename_wal           capdb_api->filename_wal
/* Version 3.32.0 and later */
#define capdb_create_filename        capdb_api->create_filename
#define capdb_free_filename          capdb_api->free_filename
#define capdb_database_file_object   capdb_api->database_file_object
/* Version 3.34.0 and later */
#define capdb_txn_state              capdb_api->txn_state
/* Version 3.36.1 and later */
#define capdb_changes64              capdb_api->changes64
#define capdb_total_changes64        capdb_api->total_changes64
/* Version 3.37.0 and later */
#define capdb_autovacuum_pages       capdb_api->autovacuum_pages
/* Version 3.38.0 and later */
#define capdb_error_offset           capdb_api->error_offset
#define capdb_vtab_rhs_value         capdb_api->vtab_rhs_value
#define capdb_vtab_distinct          capdb_api->vtab_distinct
#define capdb_vtab_in                capdb_api->vtab_in
#define capdb_vtab_in_first          capdb_api->vtab_in_first
#define capdb_vtab_in_next           capdb_api->vtab_in_next
/* Version 3.39.0 and later */
#ifndef CAPDB_OMIT_DESERIALIZE
#define capdb_deserialize            capdb_api->deserialize
#define capdb_serialize              capdb_api->serialize
#endif
#define capdb_db_name                capdb_api->db_name
/* Version 3.40.0 and later */
#define capdb_value_encoding         capdb_api->value_encoding
/* Version 3.41.0 and later */
#define capdb_is_interrupted         capdb_api->is_interrupted
/* Version 3.43.0 and later */
#define capdb_stmt_explain           capdb_api->stmt_explain
/* Version 3.44.0 and later */
#define capdb_get_clientdata         capdb_api->get_clientdata
#define capdb_set_clientdata         capdb_api->set_clientdata
/* Version 3.50.0 and later */
#define capdb_setlk_timeout          capdb_api->setlk_timeout
/* Version 3.51.0 and later */
#define capdb_set_errmsg             capdb_api->set_errmsg
#define capdb_db_status64            capdb_api->db_status64
/* Version 3.52.0 and later */
#define capdb_str_truncate           capdb_api->str_truncate
#define capdb_str_free               capdb_api->str_free
#define capdb_carray_bind            capdb_api->carray_bind
#define capdb_carray_bind_v2         capdb_api->carray_bind_v2
/* Version 3.54.0 and later */
#define capdb_incomplete             capdb_api->incomplete
#endif /* !defined(CAPDB_CORE) && !defined(CAPDB_OMIT_LOAD_EXTENSION) */

#if !defined(CAPDB_CORE) && !defined(CAPDB_OMIT_LOAD_EXTENSION)
  /* This case when the file really is being compiled as a loadable 
  ** extension */
# define CAPDB_EXTENSION_INIT1     const capdb_api_routines *capdb_api=0;
# define CAPDB_EXTENSION_INIT2(v)  capdb_api=v;
# define CAPDB_EXTENSION_INIT3     \
    extern const capdb_api_routines *capdb_api;
#else
  /* This case when the file is being statically linked into the 
  ** application */
# define CAPDB_EXTENSION_INIT1     /*no-op*/
# define CAPDB_EXTENSION_INIT2(v)  (void)v; /* unused parameter */
# define CAPDB_EXTENSION_INIT3     /*no-op*/
#endif

#endif /* SQLITE3EXT_H */
