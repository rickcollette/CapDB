# CapDB Rebrand — Breaking Migration Guide

This fork is **CapDB**, a hard fork of SQLite with connection pooling and TLS networking.
There are **no compatibility shims** for the old `msqlite3` / `sqlite3` / `msqlite://` names.

## Product and CLI

| Before | After |
|--------|-------|
| `msqlite3` project / repo | `capdb` |
| `sqlite3` CLI binary | **`capdb`** |
| `libsqlite3` | `libcapdb` |
| `sqlite3.c` / `sqlite3.h` | `capdb.c` / `capdb.h` |

## API

All public symbols were renamed:

- `sqlite3_*` → `capdb_*` (e.g. `capdb_open`, `capdb_exec`)
- `SQLITE_*` → `CAPDB_*` (e.g. `CAPDB_OK`, `CAPDB_BUSY`)
- `sqlite3_pool_*` → `capdb_pool_*`
- Headers: `capdb.h`, `capdbext.h`, `capdb_pool.h`, `capdb_session.h`, etc.

## Network

| Before | After |
|--------|-------|
| `msqlite://host:port/db` | **`capdb://host:port/db`** |
| `msqlite-server` | `capdb-server` |
| `libmsqlite_client` | `libcapdb_client` |
| VFS `"msqlitevfs"` | `"capdbvfs"` |
| `SQLITE_ENABLE_MSQLITE_NETWORK` | `CAPDB_ENABLE_NETWORK` |

## CMake options

| Before | After |
|--------|-------|
| `SQLITE_ENABLE_CONNECTION_POOL` | `CAPDB_ENABLE_POOL` |
| `SQLITE_ENABLE_MSQLITE_NETWORK` | `CAPDB_ENABLE_NETWORK` |
| `SQLITE_REGENERATE_SOURCES` | `CAPDB_REGENERATE_SOURCES` |
| `SQLITE_BUILD_TESTS` | `CAPDB_BUILD_TESTS` |

Build example:

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON
cmake --build build
```

## Tests

| Before | After |
|--------|-------|
| `msuite` | `capsuite` |
| `msqlitest` | `capdbtest` |
| `MSUITE_*` env vars | `CAPSUITE_*` |

```bash
cd build
ctest --output-on-failure
CAPSUITE_SERVER=$PWD/capdb-server CAPSUITE_CLIENT_TEST=$PWD/capdbtest ./capsuite
```

## Codegen

| Before | After |
|--------|-------|
| `tool/py/mksqlite3c.py` | `tools/py/mkcapdb.py` |
| `tool/py/mksqlite3h.py` | `tools/py/mkcapdbh.py` |
| `src/sqlite.h.in` | `core/capdb.h.in` |

Regenerate amalgamation:

```bash
python3 tools/py/mkcapdbh.py . -o build/generated/capdb.h
python3 tools/py/stage_sources.py --gen-dir build/generated
python3 tools/py/mkcapdb.py --srcdir build/generated/staging -o build/generated/capdb.c
python3 tools/py/mkshellc.py build/generated/shell.c
```

## Upstream merges

This rebrand is a **hard fork**. Upstream SQLite patches will not apply cleanly.
Merge upstream only with manual conflict resolution across the full `capdb_*` rename.

## Audit

After changes, verify no stale product names remain:

```bash
python3 tool/rebrand/apply_capdb_rename.py --audit
```

Expected zero matches for: `msqlite`, `msuite`, `msqlite://`, `sqlite3_pool`, `sqlite3_cli`.
