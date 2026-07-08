#!/usr/bin/env bash
set -euo pipefail

BUILD="${1:-build}"
MODE="${2:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cmake -S "$ROOT" -B "$BUILD" -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON >/dev/null
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
python3 "$ROOT/tools/py/tests/test_codegen_parity.py" >/dev/null
"$ROOT/tools/check-api-surface.sh" >/dev/null

if [ "$MODE" != "--quick" ]; then
  ctest --test-dir "$BUILD" --output-on-failure
  CAPDB_WERROR=ON "$ROOT/tools/warnings.sh"
fi

test -f "$ROOT/docs/upstream/COMPATIBILITY.md"
grep -q "CapDB compatibility ledger" "$ROOT/docs/upstream/COMPATIBILITY.md"
grep -q "Archived upstream coverage" "$ROOT/docs/upstream/COMPATIBILITY.md"

echo "capdb upstream sync check ok"
