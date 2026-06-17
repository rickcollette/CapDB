#!/bin/sh
# Compile CapDB-owned targets with strict warnings via CMake.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
WERROR="${CAPDB_WERROR:-OFF}"

cmake -B "$BUILD" \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_ENABLE_NETWORK=ON \
  -DCAPDB_WARNINGS=ON \
  -DCAPDB_WERROR="$WERROR"
cmake --build "$BUILD" --target capdb_client capdb-server capdbtest capdb_nettest capsuite threadtest_pool
if cmake --build "$BUILD" --target capdb_jni 2>/dev/null; then
  :
fi

echo "**** Warning build of CapDB-owned targets succeeded ****"
