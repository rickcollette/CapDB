# Upstream Sync Strategy

CapDB is a hard fork of SQLite. Upstream merges must preserve CapDB product
names, generated-source rules, network/storage additions, and the CMake/Python
build pipeline.

## Source Mapping

| Upstream SQLite Path | CapDB Path |
|----------------------|------------|
| `src/` | `core/` |
| `ext/` | `extensions/` |
| `tool/` | `tools/` |
| `test/` | `tests/archive/` for retained upstream tests, `tests/capdb/` for active CapDB tests |
| generated amalgamation | `build/generated/capdb.c`, `build/generated/capdb.h` |

`docs/upstream/README-LEGACY.md` keeps additional historical mapping notes.

## Merge Rules

1. Import upstream changes into a staging branch, preserving original upstream
   file boundaries before moving changes into CapDB layout paths.
2. Never reintroduce public `sqlite3_*` or `msqlite://` compatibility shims into
   CapDB APIs.
3. Keep upstream reference docs under `docs/upstream/`; CapDB product docs live
   in top-level docs, `capdb/`, and `man/`.
4. Do not edit generated `build/generated/` artifacts by hand. Change source
   inputs under `core/`, `extensions/`, or `tools/py/`, then regenerate.
5. Treat `capdb/`, bindings, packaging, and `tests/capdb/` as CapDB-owned code;
   upstream changes should not overwrite them.

## Required Verification

Run these before accepting an upstream sync:

```bash
tools/upstream-sync-check.sh build
```

When bindings or install artifacts are touched, also run:

```bash
eval "$(./capdb-go-build-env.sh ./build)" && (cd bindings/go && go test -tags capdb ./capdb)
(cd bindings/rust && CAPDB_LIB_DIR="$PWD/../../build" CAPDB_SERVER="$PWD/../../build/capdb-server" cargo test)
CAPDB_LIBRARY="$PWD/build/libcapdb.so" LD_LIBRARY_PATH="$PWD/build:${LD_LIBRARY_PATH:-}" \
  PYTHONPATH=bindings/python python3 -c 'import capdb; c=capdb.connect(":memory:"); c.close()'
cmake --install build --prefix /tmp/capdb-upstream-sync-install
```

The compatibility ledger lives at
[`docs/upstream/COMPATIBILITY.md`](upstream/COMPATIBILITY.md). Update it for
each upstream intake before merge.

## Compatibility Matrix

Track each upstream intake with:

| Field | Required |
|-------|----------|
| Upstream SQLite version/source id | Yes |
| CapDB version | Yes |
| Parser/codegen changes | Yes/no plus notes |
| Public API changes | Yes/no plus notes |
| File-format or WAL behavior changes | Yes/no plus notes |
| Extension behavior changes | Yes/no plus notes |
| Active CapDB tests passed | Exact command list |
| Archived upstream tests run | Exact command list or explicit skip reason |

## Conflict Policy

Prefer upstream correctness fixes in the core SQL engine unless they conflict
with CapDB naming, build, or server/storage contracts. For conflicts:

- Keep upstream algorithmic changes.
- Reapply CapDB naming and compile-time feature gates.
- Add or update an active CapDB regression when the conflict touches network,
  pool, store, replication, packaging, or bindings.
- Document unresolved upstream drift in the compatibility matrix before merge.
