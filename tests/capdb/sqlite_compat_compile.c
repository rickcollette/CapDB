#define SQLITE_ENABLE_SESSION 1
#include <sqlite3_capdb.h>
#include <string.h>

int main(void) {
  sqlite3 *database = (capdb *)0;
  sqlite3_stmt *statement = (capdb_stmt *)0;
  sqlite3_context *context = (capdb_context *)0;
  sqlite3_value *value = (capdb_value *)0;
  sqlite3_session *session = (capdb_session *)0;
  sqlite3_changeset_iter *changeset = (capdb_changeset_iter *)0;
  int (*enable_extension)(capdb *, int) = sqlite3_enable_load_extension;
  int (*backup_remaining)(capdb_backup *) = sqlite3_backup_remaining;
  void (*result_null)(capdb_context *) = sqlite3_result_null;
  int (*extended_error)(capdb *) = sqlite3_extended_errcode;
  const char *(*library_version)(void) = sqlite3_libversion;
  int (*library_version_number)(void) = sqlite3_libversion_number;
  int (*volatile session_create)(sqlite3 *, const char *, sqlite3_session **) =
      sqlite3session_create;
  void (*volatile session_delete)(sqlite3_session *) = sqlite3session_delete;
  int (*volatile session_changeset)(sqlite3_session *, int *, void **) =
      sqlite3session_changeset;
  int (*volatile changeset_apply)(
      sqlite3 *, int, void *, int (*)(void *, const char *),
      int (*)(void *, int, sqlite3_changeset_iter *), void *) =
      sqlite3changeset_apply;
  (void)enable_extension;
  (void)backup_remaining;
  (void)result_null;
  (void)extended_error;
  (void)library_version;
  (void)library_version_number;
  (void)session_create;
  (void)session_delete;
  (void)session_changeset;
  (void)changeset_apply;
  (void)database;
  (void)statement;
  (void)context;
  (void)value;
  (void)session;
  (void)changeset;

#define REQUIRE_SQLITE_COMPAT_SYMBOL(name) (void)sizeof(&(name))
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_bind_parameter_name);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_column_database_name);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_column_origin_name);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_column_table_name);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_create_function_v2);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_create_window_function);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_deserialize);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_limit);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_malloc64);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_serialize);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3_stmt_status);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3session_attach);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3session_changeset);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3session_create);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3session_delete);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3session_patchset);
  REQUIRE_SQLITE_COMPAT_SYMBOL(sqlite3changeset_apply);
#undef REQUIRE_SQLITE_COMPAT_SYMBOL

  if (SQLITE_COPY != CAPDB_COPY || SQLITE_LIMIT_COLUMN != CAPDB_LIMIT_COLUMN) {
    return 1;
  }
  if (strcmp(SQLITE_VERSION, CAPDB_VERSION) != 0 ||
      SQLITE_VERSION_NUMBER != CAPDB_VERSION_NUMBER ||
      strcmp(SQLITE_SOURCE_ID, CAPDB_SOURCE_ID) != 0) {
    return 2;
  }
  if (SQLITE_DBCONFIG_DEFENSIVE != CAPDB_DBCONFIG_DEFENSIVE ||
      SQLITE_DBCONFIG_DQS_DDL != CAPDB_DBCONFIG_DQS_DDL ||
      SQLITE_DBCONFIG_DQS_DML != CAPDB_DBCONFIG_DQS_DML ||
      SQLITE_DBCONFIG_ENABLE_FKEY != CAPDB_DBCONFIG_ENABLE_FKEY ||
      SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION != CAPDB_DBCONFIG_ENABLE_LOAD_EXTENSION ||
      SQLITE_DESERIALIZE_FREEONCLOSE != CAPDB_DESERIALIZE_FREEONCLOSE ||
      SQLITE_DESERIALIZE_RESIZEABLE != CAPDB_DESERIALIZE_RESIZEABLE ||
      SQLITE_DIRECTONLY != CAPDB_DIRECTONLY ||
      SQLITE_RECURSIVE != CAPDB_RECURSIVE ||
      SQLITE_STMTSTATUS_REPREPARE != CAPDB_STMTSTATUS_REPREPARE ||
      SQLITE_CHANGESET_DATA != CAPDB_CHANGESET_DATA ||
      SQLITE_CHANGESET_ABORT != CAPDB_CHANGESET_ABORT) {
    return 3;
  }
  return 0;
}
