<h1 align="center">CapDB</h1>

**CapDB** is a hard fork of SQLite 3.54 with connection pooling and TLS networking.
Build with [CMake](docs/BUILD.md) — no Tcl required. Tests run via `capsuite` and native C binaries.

| Topic | Document |
|-------|----------|
| User guide | [docs/CAPDB.md](docs/CAPDB.md) |
| Repository layout | [docs/LAYOUT.md](docs/LAYOUT.md) |
| Build & install | [docs/BUILD.md](docs/BUILD.md) |
| CLI man page | [man/capdb.1](man/capdb.1) |
| Server man page | [man/capdb-server.1](man/capdb-server.1) |
| Network layer | [capdb/README.md](capdb/README.md) |
| Connection pool | [capdb/pool/README.md](capdb/pool/README.md) |
| Breaking rename | [docs/REBRAND.md](docs/REBRAND.md) |

After building: `./build/capdb my.db` · remote: `./build/capdb 'capdb://host:5432/db?token=x&insecure=1'`

## Quick build

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

Generated sources (`capdb.c`, `capdb.h`, `shell.c`) are produced at build time under `build/generated/`. See [docs/LAYOUT.md](docs/LAYOUT.md).

## History & licensing

CapDB keeps the SQLite SQL dialect, file format, and engine (public domain), and adds a connection pool, TLS client/server protocol, and SQL server. CapDB additions are [MIT licensed](LICENSE); see [LICENSE.md](LICENSE.md) for per-path scope.

Upstream SQLite heritage and legacy Fossil/Tcl build notes: [docs/upstream/README-LEGACY.md](docs/upstream/README-LEGACY.md).
