# CapDB User Guide

CapDB is a hard fork of SQLite 3.54 with two major extensions:

1. **Connection pool** (`capdb_pool_*`) — checkout/checkin API with WAL and busy-timeout defaults.
2. **TLS network layer** (`capdb://`) — remote SQL and optional remote VFS.

There are **no compatibility shims** for old `sqlite3` / `msqlite://` names.

## Quick build

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON
cmake --build build -j$(nproc)
sudo cmake --install build   # optional
```

See [BUILD.md](../BUILD.md) for all CMake options and codegen.

## Command-line interface

The shipped binary is **`capdb`** (not `sqlite3`):

```bash
./build/capdb my.db
./build/capdb -batch my.db 'SELECT 1;'
./build/capdb 'capdb://localhost:5432/remote.db?token=SECRET&insecure=1'
```

Man page: [capdb(1)](../man/capdb.1)

### Configuration files

| File | Purpose |
|------|---------|
| `~/.config/capdb/sqliterc` | Interactive dot-commands (preferred) |
| `~/.sqliterc` | Legacy fallback |
| `$CAPDB_HISTORY` | Interactive history file path |

## Network server

```bash
./build/capdb-server \
  --listen 127.0.0.1:5432 \
  --auth-file ./tokens.txt \
  --db-root ./data \
  --insecure
```

Man page: [capdb-server(1)](../man/capdb-server.1)

Full URI and client API details: [capdb/README.md](../capdb/README.md)

## Connection pool

C API in `capdb_pool.h`:

```c
capdb_pool *pool;
capdb *db;
capdb_pool_open("/path/app.db", &cfg, &pool);
capdb_pool_acquire(pool, CAPDB_POOL_WRITE, 30000, &db);
/* ... use db ... */
capdb_exec(db, "COMMIT", 0, 0, 0);
capdb_pool_release(pool, db);
capdb_pool_close(pool);
```

See [capdb/pool/README.md](../capdb/pool/README.md).

## C library

| Artifact | Description |
|----------|-------------|
| `capdb.c` / `capdb.h` | Amalgamation and public API |
| `capdbext.h` | Loadable extension interface |
| `libcapdb_client` | Network client (when `CAPDB_ENABLE_NETWORK`) |

All symbols use the `capdb_*` / `CAPDB_*` prefix.

## Tests

```bash
cd build
ctest --output-on-failure
CAPSUITE_SERVER=$PWD/capdb-server CAPSUITE_CLIENT_TEST=$PWD/capdbtest ./capsuite
```

| Binary | Role |
|--------|------|
| `capsuite` | Pool + network regression tests |
| `capdbtest` | Standalone network smoke test |
| `threadtest_pool` | Pool concurrency stress test |

No Tcl / testfixture is required.

## Documentation index

| Document | Contents |
|----------|----------|
| [LAYOUT.md](LAYOUT.md) | Repository layout and codegen |
| [BUILD.md](BUILD.md) | CMake build, install, codegen |
| [CHANGELOG.md](../CHANGELOG.md) | Release notes |
| [capdb(1)](../man/capdb.1) | CLI man page |
| [capdb-server(1)](../man/capdb-server.1) | Server man page |
| [capdb/README.md](../capdb/README.md) | Network layer |
| [capdb/pool/README.md](../capdb/pool/README.md) | Connection pool |

Upstream SQLite design docs under `docs/upstream/` describe core engine behavior; CapDB replaces product naming but retains the same architecture unless noted otherwise.
