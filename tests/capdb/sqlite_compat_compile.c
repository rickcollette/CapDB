#include <sqlite3_capdb.h>
#include <string.h>

int main(void) {
  sqlite3 *database = (capdb *)0;
  sqlite3_stmt *statement = (capdb_stmt *)0;
  sqlite3_context *context = (capdb_context *)0;
  sqlite3_value *value = (capdb_value *)0;
  int (*enable_extension)(capdb *, int) = sqlite3_enable_load_extension;
  int (*backup_remaining)(capdb_backup *) = sqlite3_backup_remaining;
  void (*result_null)(capdb_context *) = sqlite3_result_null;
  int (*extended_error)(capdb *) = sqlite3_extended_errcode;
  const char *(*library_version)(void) = sqlite3_libversion;
  int (*library_version_number)(void) = sqlite3_libversion_number;
  (void)enable_extension;
  (void)backup_remaining;
  (void)result_null;
  (void)extended_error;
  (void)library_version;
  (void)library_version_number;
  (void)database;
  (void)statement;
  (void)context;
  (void)value;
  if (SQLITE_COPY != CAPDB_COPY || SQLITE_LIMIT_COLUMN != CAPDB_LIMIT_COLUMN) {
    return 1;
  }
  if (strcmp(SQLITE_VERSION, CAPDB_VERSION) != 0 ||
      SQLITE_VERSION_NUMBER != CAPDB_VERSION_NUMBER ||
      strcmp(SQLITE_SOURCE_ID, CAPDB_SOURCE_ID) != 0) {
    return 2;
  }
  return 0;
}
