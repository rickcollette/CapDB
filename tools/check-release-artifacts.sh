#!/usr/bin/env bash
set -euo pipefail

DIST="${1:-dist}"
VERSION="$(tr -d ' \t\n\r' < "${2:-VERSION}")"

test -d "$DIST"
test -f "$DIST/capdb-amalgamation-$VERSION.tar.gz"
test -f "$DIST/capdb-bindings-$VERSION.tar.gz"

if ! ls "$DIST"/capdb-"$VERSION"-*.tar.gz >/dev/null 2>&1; then
  echo "missing versioned CapDB binary/source CPack tarball in $DIST" >&2
  exit 1
fi

AMALG_LIST="$(mktemp)"
BIND_LIST="$(mktemp)"
trap 'rm -f "$AMALG_LIST" "$BIND_LIST"' EXIT
tar -tzf "$DIST/capdb-amalgamation-$VERSION.tar.gz" > "$AMALG_LIST"
tar -tzf "$DIST/capdb-bindings-$VERSION.tar.gz" > "$BIND_LIST"
grep -q "/capdb.c$" "$AMALG_LIST"
grep -q "/capdb.h$" "$AMALG_LIST"
grep -q "/go/" "$BIND_LIST"
grep -q "/rust/" "$BIND_LIST"
grep -q "/python/" "$BIND_LIST"
grep -q "/java/" "$BIND_LIST"

echo "capdb release artifact audit ok"
