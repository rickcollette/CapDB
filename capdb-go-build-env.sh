#!/bin/bash
# capdb-go-build-env.sh — Emit build flags for Go CGO with CapDB
#
# Usage:
#   eval $(./capdb-go-build-env.sh /path/to/capdb/build)
#   go build -tags capdb ./...

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <capdb-build-dir>" >&2
    echo "" >&2
    echo "Example:" >&2
    echo "  eval \$($0 ../CapDB/build)" >&2
    echo "  go build -tags capdb ./..." >&2
    exit 1
fi

CAPDB_BUILD="$1"
CAPDB_CLIENT="${CAPDB_BUILD}/../capdb/client"
CAPDB_GENERATED="${CAPDB_BUILD}/generated"

if [ ! -d "$CAPDB_BUILD" ]; then
    echo "Error: $CAPDB_BUILD does not exist" >&2
    exit 1
fi

if [ ! -f "$CAPDB_BUILD/libcapdb.a" ]; then
    echo "Error: libcapdb.a not found in $CAPDB_BUILD" >&2
    echo "Run: cd $CAPDB_BUILD && cmake .. && make" >&2
    exit 1
fi

# Core flags
CFLAGS="-I$(cd "$CAPDB_CLIENT" && pwd)"
CFLAGS="$CFLAGS -I$(cd "$CAPDB_GENERATED" && pwd)"
CFLAGS="$CFLAGS -DCAPDB_ENABLE_NETWORK=1"

LDFLAGS="-L$(cd "$CAPDB_BUILD" && pwd)"
LDFLAGS="$LDFLAGS -Wl,--start-group -lcapdb -lcapdb_store -Wl,--end-group"
LDFLAGS="$LDFLAGS -lssl -lcrypto -lpthread -lm"

cat <<EOF
export CGO_ENABLED=1
export CGO_CFLAGS="$CFLAGS"
export CGO_LDFLAGS="$LDFLAGS"
export CAPDB_SERVER="$CAPDB_BUILD/capdb-server"
EOF
