#!/bin/bash
# capdb-rust-build-env.sh — Emit build flags for Rust FFI with CapDB
#
# Usage:
#   source ./capdb-rust-build-env.sh /path/to/capdb/build
#   cargo build --features capdb

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <capdb-build-dir>" >&2
    echo "" >&2
    echo "Example:" >&2
    echo "  source $0 ../CapDB/build" >&2
    echo "  cargo build --features capdb" >&2
    exit 1
fi

CAPDB_BUILD="$(cd "$1" && pwd)"
CAPDB_CLIENT="$(cd "$(dirname "$CAPDB_BUILD")/capdb/client" && pwd)"
CAPDB_GENERATED="$CAPDB_BUILD/generated"

if [ ! -d "$CAPDB_BUILD" ]; then
    echo "Error: $CAPDB_BUILD does not exist" >&2
    exit 1
fi

if [ ! -f "$CAPDB_BUILD/libcapdb.a" ] && [ ! -f "$CAPDB_BUILD/libcapdb_store.a" ]; then
    echo "Error: CapDB libraries not found in $CAPDB_BUILD" >&2
    echo "Run: cd $CAPDB_BUILD && cmake .. && make" >&2
    exit 1
fi

cat <<EOF
export CAPDB_INCLUDE_DIR="$CAPDB_CLIENT:$CAPDB_GENERATED"
export CAPDB_LIB_DIR="$CAPDB_BUILD"
export CAPDB_RUSTFLAGS="-lcapdb_client -lcapdb_store -lssl -lcrypto -lpthread -lm"
EOF
