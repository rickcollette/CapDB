/*
** If the canonical build process finds the file
** capdb_wasm_extra_init.c in the main wasm build directory, it
** arranges to include that file in the build of capdb.wasm and
** defines CAPDB_EXTRA_INIT=capdb_wasm_extra_init.
**
** The C file must define the function capdb_wasm_extra_init() with
** this signature:
**
**  int capdb_wasm_extra_init(const char *)
**
** and the capdb library will call it with an argument of NULL one
** time during capdb_initialize(). If it returns non-0,
** initialization of the library will fail.
*/

#include "capdb.h"
#include <stdio.h>

int capdb_wasm_extra_init(const char *z){
  fprintf(stderr,"%s: %s()\n", __FILE__, __func__);
  return 0;
}
