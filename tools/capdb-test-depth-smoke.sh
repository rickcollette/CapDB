#!/usr/bin/env bash
set -euo pipefail

CAPDB="${1:?path to capdb CLI required}"
ROOT="${2:?repo root required}"
TMP="${TMPDIR:-/tmp}/capdb-test-depth-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

"$CAPDB" -batch :memory: ".read $ROOT/tests/capdb/upstream_archive_smoke.sql" >/dev/null

for i in $(seq 1 40); do
  "$CAPDB" -batch "$TMP/soak.db" \
    "CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v TEXT);
     INSERT INTO t(v) VALUES('row-$i');
     SELECT count(*) FROM t;" >/dev/null
done

for sql in \
  "SELECT randomblob(-9223372036854775808);" \
  "CREATE TABLE x(a); INSERT INTO x VALUES(zeroblob(32)); SELECT quote(a) FROM x;" \
  "WITH RECURSIVE c(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<32) SELECT sum(x) FROM c;" \
  "SELECT json_extract('{\"a\":[1,2,3]}', '$.a[2]');"
do
  printf '%s\n' "$sql" | timeout 10s "$CAPDB" -batch :memory: >/dev/null
done

for sql in \
  "SELECT (;" \
  "CREATE TABLE bad(x UNIQUE, x UNIQUE);" \
  "SELECT json_extract('{', '$.bad');"
do
  set +e
  printf '%s\n' "$sql" | timeout 10s "$CAPDB" -batch :memory: >/dev/null 2>"$TMP/fuzz.err"
  rc=$?
  set -e
  if [ "$rc" -gt 128 ]; then
    cat "$TMP/fuzz.err" >&2
    echo "capdb crashed or was signaled during fuzz smoke: rc=$rc" >&2
    exit 1
  fi
done

echo "capdb test-depth smoke ok"
