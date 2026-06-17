#!/usr/bin/env bash
#
# Build, test, and package CapDB release artifacts into ./dist/.
# Produces:
#   - capdb-<ver>-<os>-<arch>.tar.gz   binary dist (CLI, server, libs, headers)
#   - capdb-<ver>-src.tar.gz           source tarball
#   - capdb-amalgamation-<ver>.tar.gz  single-file amalgamation + headers
#
# Usage: scripts/release.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
DIST="$ROOT/dist"
VERSION="$(tr -d ' \t\n\r' < "$ROOT/VERSION")"
GEN="$BUILD/generated"

echo ">> CapDB $VERSION — release build into $BUILD"
cmake -B "$BUILD" -S "$ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_ENABLE_NETWORK=ON
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)"

echo ">> Running tests"
( cd "$BUILD" && ctest --output-on-failure )

echo ">> Codegen parity"
python3 "$ROOT/tools/py/tests/test_codegen_parity.py"

echo ">> Packaging into $DIST"
rm -rf "$DIST"
mkdir -p "$DIST"

# Binary + source tarballs via CPack.
( cd "$BUILD" && cpack -G TGZ )
( cd "$BUILD" && cpack --config CPackSourceConfig.cmake -G TGZ )
mv "$BUILD"/capdb-*.tar.gz "$DIST"/ 2>/dev/null || true

# Amalgamation bundle: the easy-to-vendor single .c plus public headers.
AMALG="capdb-amalgamation-$VERSION"
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/$AMALG"
cp "$GEN/capdb.c" "$GEN/capdb.h" "$ROOT/core/capdbext.h" "$STAGE/$AMALG/"
cp "$ROOT/capdb/client/capdb_client.h" "$STAGE/$AMALG/" 2>/dev/null || true
cp "$ROOT/capdb/pool/capdb_pool.h" "$STAGE/$AMALG/" 2>/dev/null || true
cp "$ROOT/LICENSE" "$ROOT/LICENSE.md" "$ROOT/README.md" "$STAGE/$AMALG/"
tar -czf "$DIST/$AMALG.tar.gz" -C "$STAGE" "$AMALG"
rm -rf "$STAGE"

echo ">> Artifacts:"
ls -la "$DIST"
