#include <sqlite3_capdb.h>

int main(void) {
  int (*enable_extension)(capdb *, int) = sqlite3_enable_load_extension;
  int (*backup_remaining)(capdb_backup *) = sqlite3_backup_remaining;
  void (*result_null)(capdb_context *) = sqlite3_result_null;
  int (*extended_error)(capdb *) = sqlite3_extended_errcode;
  (void)enable_extension;
  (void)backup_remaining;
  (void)result_null;
  (void)extended_error;
  if (SQLITE_COPY != CAPDB_COPY || SQLITE_LIMIT_COLUMN != CAPDB_LIMIT_COLUMN) {
    return 1;
  }
  return 0;
}
