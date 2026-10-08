#!/usr/bin/env bash
# Cross-compile the embedded CapDB library and CLI for 64-bit Windows.
# Networking, the volume store, and replication stay off: those sources are
# POSIX and are not part of this archive.
#
# Requires x86_64-w64-mingw32-gcc, the MinGW static zlib, and wine.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="$(tr -d ' \t\n\r' < "$ROOT/VERSION")"
BUILD="${1:-$ROOT/build-mingw}"
OUT="$ROOT/dist/windows-x86_64"
ZIP="capdb-${VERSION}-windows-x86_64.zip"

cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPDB_ENABLE_NETWORK=OFF \
  -DCAPDB_ENABLE_STORE=OFF \
  -DCAPDB_ENABLE_REPLICATION=OFF \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_BUILD_TESTS=OFF \
  -DCAPDB_HAVE_ZLIB=ON

cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" --target capdb_cli capdb_shared

cli="$(find "$BUILD" -type f -name 'capdb.exe' ! -path '*/CMakeFiles/*' | head -n 1)"
if [ -f "$BUILD/capdb.dll" ]; then
  dll="$BUILD/capdb.dll"
else
  dll="$(find "$BUILD" -type f -name 'libcapdb.dll' ! -path '*/CMakeFiles/*' | head -n 1)"
fi
implib="$(find "$BUILD" -type f -name 'libcapdb.dll.a' ! -path '*/CMakeFiles/*' | head -n 1)"
header="$BUILD/generated/capdb.h"
for f in "$cli" "$dll" "$implib" "$header"; do
  if [ ! -f "$f" ]; then
    echo "error: missing Windows build output" >&2
    exit 1
  fi
done

if [ -x /usr/lib/wine/wine64 ]; then
  wine_bin=/usr/lib/wine/wine64
elif command -v wine64 >/dev/null 2>&1; then
  wine_bin=wine64
else
  wine_bin=wine
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cp "$cli" "$dll" "$implib" "$header" "$stage/"
# A fresh prefix avoids a broken 32-bit ~/.wine. /tmp may not be owned by
# the runner, so the prefix lives next to the build.
mkdir -p "$BUILD/wine-prefix"
WINEPREFIX="$BUILD/wine-prefix" WINEDEBUG=-all \
  "$ROOT/scripts/sql-release-smoke.sh" --embedded-only --wine "$wine_bin" "$cli"

mkdir -p "$OUT"
rm -f "$OUT/$ZIP" "$OUT/$ZIP.sha256"
(
  cd "$stage"
  zip -q -X "$OUT/$ZIP" capdb.exe capdb.dll libcapdb.dll.a capdb.h 2>/dev/null \
    || zip -q -X "$OUT/$ZIP" capdb.exe libcapdb.dll libcapdb.dll.a capdb.h
)
(
  cd "$OUT"
  sha256sum "$ZIP" > "$ZIP.sha256"
)
echo ">> $OUT/$ZIP"
ls -la "$OUT"
