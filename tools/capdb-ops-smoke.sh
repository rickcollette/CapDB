#!/usr/bin/env bash
set -euo pipefail

CTL="${1:?path to capdb-ctl required}"
TMP="${TMPDIR:-/tmp}/capdb-ops-smoke-$$"
VOL="$TMP/volume"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

"$CTL" prepare --volume "$VOL" >/dev/null
"$CTL" health --volume "$VOL" --json | grep -q '"healthy":true'
"$CTL" status --volume "$VOL" --json | grep -q '"role":"primary"'
"$CTL" metrics --volume "$VOL" | grep -q '^capdb_volume_lag_bytes'
"$CTL" backup --volume "$VOL" --out "$TMP/backup.db" >/dev/null
CAPDB="${CAPDB:-capdb}" "$(dirname "$0")/capdb-restore-drill.sh" "$TMP/backup.db" >/dev/null

echo "capdb ops smoke ok"
