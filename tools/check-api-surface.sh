#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

paths=(
  core/capdb.h.in
  core/capdbext.h
  capdb/capdb_network.h
  capdb/client/capdb_client.h
  capdb/pool/capdb_pool.h
  capdb/store/capdb_store.h
  capdb/cluster/capdb_cluster.h
  capdb/replication/capdb_rep.h
)

if rg -n 'sqlite3_|msqlite://' "${paths[@]}"; then
  echo "CapDB public API surface contains legacy sqlite3_/msqlite names" >&2
  exit 1
fi

echo "CapDB public API surface check passed"
