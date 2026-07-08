#!/usr/bin/env bash
set -euo pipefail

BUILD="${1:?build directory required}"
PREFIX="${2:?install prefix required}"

rm -rf "$PREFIX"
cmake --install "$BUILD" --prefix "$PREFIX" >/dev/null

test -x "$PREFIX/bin/capdb"
test -x "$PREFIX/bin/capdb-server"
test -x "$PREFIX/bin/capdb-ctl"
test -f "$PREFIX/include/capdb.h"
test -f "$PREFIX/include/capdb_client.h"
test -f "$PREFIX/include/capdb_pool.h"
test -f "$PREFIX/include/capdb/store/capdb_store.h"
test -f "$PREFIX/include/capdb/cluster/capdb_cluster.h"
test -f "$PREFIX/include/capdb/replication/capdb_rep.h"
test -f "$PREFIX/share/man/man1/capdb.1"
test -f "$PREFIX/share/man/man1/capdb-server.1"
test -f "$PREFIX/share/man/man1/capdb-ctl.1"
test -f "$PREFIX/share/capdb/systemd/capdb.service"
test -f "$PREFIX/share/capdb/java/org/capdb/jni/wrapper1/Capdb.java"
test -f "$PREFIX/lib/pkgconfig/capdb.pc"
test -f "$PREFIX/lib/pkgconfig/libcapdb_client.pc"

"$PREFIX/bin/capdb" -batch :memory: 'SELECT 1;' >/dev/null
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --exists capdb libcapdb_client

if ls "$PREFIX"/lib/libcapdb_jni.* >/dev/null 2>&1; then
  :
else
  echo "missing installed capdb_jni library" >&2
  exit 1
fi

echo "capdb install smoke ok"
