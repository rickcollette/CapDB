#ifndef CAPDB_SQLITE3_SOURCE_COMPAT_H
#define CAPDB_SQLITE3_SOURCE_COMPAT_H

#include <capdb.h>

/*
 * Canonical source-level SQLite type spellings.  Consumers must see these as
 * aliases of CapDB's actual public objects; declaring unrelated opaque
 * `struct sqlite3_context` or `struct sqlite3_value` types makes callback
 * prototypes incompatible in C++ and was the source of downstream duplicate
 * typedef failures.
 */
typedef capdb sqlite3;
typedef capdb_stmt sqlite3_stmt;
typedef capdb_context sqlite3_context;
typedef capdb_value sqlite3_value;
typedef capdb_backup sqlite3_backup;
typedef capdb_vfs sqlite3_vfs;
typedef capdb_filename sqlite3_filename;
typedef capdb_int64 sqlite3_int64;
typedef capdb_uint64 sqlite3_uint64;

/*
 * Compile-time SQLite identity spellings are part of sqlite3.h's public
 * source contract.  They describe the compatibility ABI implemented by this
 * CapDB build and must remain coherent with the corresponding CapDB values.
 */
#define SQLITE_VERSION CAPDB_VERSION
#define SQLITE_VERSION_NUMBER CAPDB_VERSION_NUMBER
#define SQLITE_SOURCE_ID CAPDB_SOURCE_ID

/* SQLite spellings supported directly by the CapDB public API. */
#define sqlite3_libversion capdb_libversion
#define sqlite3_libversion_number capdb_libversion_number
#define sqlite3_enable_load_extension capdb_enable_load_extension
#define sqlite3_load_extension capdb_load_extension
#define sqlite3_backup_init capdb_backup_init
#define sqlite3_backup_step capdb_backup_step
#define sqlite3_backup_finish capdb_backup_finish
#define sqlite3_backup_remaining capdb_backup_remaining
#define sqlite3_backup_pagecount capdb_backup_pagecount
#define sqlite3_result_blob capdb_result_blob
#define sqlite3_result_error_toobig capdb_result_error_toobig
#define sqlite3_result_null capdb_result_null
#define sqlite3_result_zeroblob capdb_result_zeroblob
#define sqlite3_aggregate_context capdb_aggregate_context
#define sqlite3_commit_hook capdb_commit_hook
#define sqlite3_create_collation capdb_create_collation
#define sqlite3_extended_errcode capdb_extended_errcode
#define sqlite3_rollback_hook capdb_rollback_hook
#define sqlite3_sourceid capdb_sourceid
#define sqlite3_system_errno capdb_system_errno
#define sqlite3_update_hook capdb_update_hook

#define SQLITE_COPY CAPDB_COPY
#define SQLITE_SAVEPOINT CAPDB_SAVEPOINT
#define SQLITE_IOERR_NOMEM CAPDB_IOERR_NOMEM
#define SQLITE_LIMIT_LENGTH CAPDB_LIMIT_LENGTH
#define SQLITE_LIMIT_SQL_LENGTH CAPDB_LIMIT_SQL_LENGTH
#define SQLITE_LIMIT_COLUMN CAPDB_LIMIT_COLUMN
#define SQLITE_LIMIT_EXPR_DEPTH CAPDB_LIMIT_EXPR_DEPTH
#define SQLITE_LIMIT_COMPOUND_SELECT CAPDB_LIMIT_COMPOUND_SELECT
#define SQLITE_LIMIT_VDBE_OP CAPDB_LIMIT_VDBE_OP
#define SQLITE_LIMIT_FUNCTION_ARG CAPDB_LIMIT_FUNCTION_ARG
#define SQLITE_LIMIT_ATTACHED CAPDB_LIMIT_ATTACHED
#define SQLITE_LIMIT_LIKE_PATTERN_LENGTH CAPDB_LIMIT_LIKE_PATTERN_LENGTH
#define SQLITE_LIMIT_VARIABLE_NUMBER CAPDB_LIMIT_VARIABLE_NUMBER
#define SQLITE_LIMIT_TRIGGER_DEPTH CAPDB_LIMIT_TRIGGER_DEPTH

#endif
