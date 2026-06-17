
#include "capdb.h"
#include <stdio.h>

int main(void) {
  void *p = 0;
#ifdef CAPDB_OMIT_AUTOINIT
  capdb_initialize();
#endif
  p = capdb_malloc(32);
  if( !p ) return 1;
  capdb_free(p);
  return 0;
}
