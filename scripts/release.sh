#!/usr/bin/env bash
#
# Build, test, and package CapDB release artifacts into ./dist/.
# Produces:
#   - capdb-<ver>-<os>-<arch>.tar.gz   binary dist (CLI, server, libs, headers)
#   - capdb-<ver>-src.tar.gz           source tarball
#   - capdb-amalgamation-<ver>.tar.gz  single-file amalgamation + headers
#
# Usage: scripts/release.sh [build-dir] [--skip-tests]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
SKIP_TESTS=0
if [ "${2:-}" = "--skip-tests" ]; then
  SKIP_TESTS=1
fi
DIST="$ROOT/dist"
VERSION="$(tr -d ' \t\n\r' < "$ROOT/VERSION")"
GEN="$BUILD/generated"

echo ">> CapDB $VERSION — release build into $BUILD"
cmake -B "$BUILD" -S "$ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_ENABLE_NETWORK=ON
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)"

if [ "$SKIP_TESTS" -eq 0 ]; then
  echo ">> Running tests"
  ( cd "$BUILD" && ctest --output-on-failure )
else
  echo ">> Skipping recursive ctest run for artifact smoke"
fi

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

# Language binding source bundle.
BINDINGS="capdb-bindings-$VERSION"
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/$BINDINGS"
cp -R "$ROOT/bindings/go" "$STAGE/$BINDINGS/"
cp -R "$ROOT/bindings/rust" "$STAGE/$BINDINGS/"
cp -R "$ROOT/bindings/python" "$STAGE/$BINDINGS/"
cp -R "$ROOT/bindings/java" "$STAGE/$BINDINGS/"
find "$STAGE/$BINDINGS" -type d \( -name target -o -name __pycache__ \) -prune -exec rm -rf {} +
find "$STAGE/$BINDINGS" -name '*.pyc' -delete
cp "$ROOT/capdb-go-build-env.sh" "$ROOT/capdb-rust-build-env.sh" \
   "$ROOT/capdb-python-build-env.sh" "$STAGE/$BINDINGS/"
cp "$ROOT/LICENSE" "$ROOT/LICENSE.md" "$ROOT/README.md" "$STAGE/$BINDINGS/"
tar -czf "$DIST/$BINDINGS.tar.gz" -C "$STAGE" "$BINDINGS"
rm -rf "$STAGE"

echo ">> Auditing artifacts"
"$ROOT/tools/check-release-artifacts.sh" "$DIST" "$ROOT/VERSION"

echo ">> Artifacts:"
ls -la "$DIST"
