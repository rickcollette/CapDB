#include "capdb.h"
#include <stdio.h>

int main(void) {
  const char *zExpect = "2023.000";
  char szBuffer[32];
  double val = 2023.0;
#ifdef CAPDB_OMIT_AUTOINIT
  capdb_initialize();
#endif
  capdb_snprintf(17, szBuffer, "%.3f", val);
  printf("size 17: '%s'\n", szBuffer);
  if( capdb_stricmp(zExpect, szBuffer) ) return 1;
  capdb_snprintf(16, szBuffer, "%.3f", val);
  printf("size 16: '%s'\n", szBuffer);
  if( capdb_stricmp(zExpect, szBuffer) ) return 1;
  return 0;
}
