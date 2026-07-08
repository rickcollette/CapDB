#!/usr/bin/env bash
set -euo pipefail

CAPDB="${CAPDB:-capdb}"
BACKUP="${1:?backup database path required}"
TMP="${TMPDIR:-/tmp}/capdb-restore-drill-$$.db"
trap 'rm -f "$TMP"' EXIT

cp "$BACKUP" "$TMP"
result="$("$CAPDB" -batch "$TMP" 'PRAGMA integrity_check;')"
if [ "$result" != "ok" ]; then
  echo "restore drill failed integrity_check: $result" >&2
  exit 1
fi

echo "capdb_restore_drill_ok backup=$BACKUP"
