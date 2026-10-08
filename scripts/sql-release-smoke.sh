#!/usr/bin/env bash
# Run tests/sql/basic.sql against the embedded shell and, unless
# --embedded-only is set, against capdb-server. Release builds run this
# before they publish archives.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SQL="$ROOT/tests/sql/basic.sql"
EXPECT="$ROOT/tests/sql/basic.expected"

WINE_BIN=""
EMBEDDED_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --embedded-only) EMBEDDED_ONLY=1; shift ;;
    --wine)
      WINE_BIN="${2:?--wine requires a wine binary}"
      shift 2
      ;;
    --) shift; break ;;
    -*) echo "error: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done

CAPDB="${1:?path to the capdb shell is required}"
SERVER="${2:-}"
if [ "$EMBEDDED_ONLY" -eq 0 ] && [ -z "$SERVER" ]; then
  echo "error: path to capdb-server is required" >&2
  exit 2
fi
if [ ! -x "$CAPDB" ] && [ ! -f "$CAPDB" ]; then
  echo "error: capdb shell not found: $CAPDB" >&2
  exit 1
fi
if [ "$EMBEDDED_ONLY" -eq 0 ] && [ ! -x "$SERVER" ]; then
  echo "error: capdb-server not found: $SERVER" >&2
  exit 1
fi

work="$(mktemp -d)"
server_pid=""
cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

normalize() {
  tr -d '\r' < "$1" > "$2"
}

run_shell() {
  local db="$1" out="$2"
  if [ -n "$WINE_BIN" ]; then
    "$WINE_BIN" "$CAPDB" -bail -batch "$db" <"$SQL" >"$out"
  else
    "$CAPDB" -bail -batch "$db" <"$SQL" >"$out"
  fi
}

check_against_expected() {
  local label="$1" raw="$2"
  normalize "$raw" "$work/${label}.out"
  if ! diff -u "$EXPECT" "$work/${label}.out"; then
    echo "error: ${label} SQL output does not match tests/sql/basic.expected" >&2
    exit 1
  fi
  echo ">> ${label} SQL smoke ok"
}

run_shell ":memory:" "$work/embedded.raw"
check_against_expected embedded "$work/embedded.raw"

if [ "$EMBEDDED_ONLY" -eq 1 ]; then
  exit 0
fi

auth="$work/auth.txt"
dbroot="$work/root"
log="$work/server.log"
mkdir -p "$dbroot"
umask 077
printf 'capuser:sha256:%s\n' "$(printf '%s' capsecret | sha256sum | awk '{print $1}')" >"$auth"
chmod 600 "$auth"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
"$SERVER" \
  --listen "127.0.0.1:${port}" \
  --auth-file "$auth" \
  --db-root "$dbroot" \
  --insecure \
  --quiet \
  >"$log" 2>&1 &
server_pid=$!

ready=0
for _ in $(seq 1 100); do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    break
  fi
  if grep -q 'listening on' "$log"; then
    ready=1
    break
  fi
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  echo "error: capdb-server did not start listening" >&2
  cat "$log" >&2 || true
  exit 1
fi

uri="capdb://capuser:capsecret@127.0.0.1:${port}/basic.db?insecure=1"
run_shell "$uri" "$work/server.raw"
check_against_expected server "$work/server.raw"
