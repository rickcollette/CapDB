# CapDB Repository Layout

This document describes the reorganized CapDB tree (v3.54+). **Edit source files only in the paths below** — never hand-edit build outputs.

## Directory map

```
CapDB/
├── core/              SQLite engine sources (was src/)
├── capdb/             Fork additions: pool, network, server, client, VFS
├── extensions/        Optional SQLite extensions (fts3, fts5, rtree, session, …)
├── tools/             Build tools, codegen (tools/py/), capdb-server.c
├── tests/
│   ├── capdb/         Active CapDB tests (capsuite, capdbtest, threadtest_pool)
│   ├── fixtures/      Certs, JSON fixtures, seed data
│   └── archive/       Legacy upstream C tests (not wired to CMake)
├── docs/              CapDB documentation
├── man/               capdb(1), capdb-server(1)
├── packaging/         capdb.pc.in
├── scripts/           release.sh, init-standalone-repo.sh
└── legacy/            autoconf/, Makefile.msc (upstream compatibility)
```

## Edit-here rules

| Edit here | Never hand-edit |
|-----------|-----------------|
| `core/`, `capdb/`, `extensions/` | `build/`, `build/generated/` |
| `tools/py/*.py`, `staging_manifest.json` | Flat staging dir (was `tsrc/`) |
| `tests/capdb/` | Generated `capdb.c` amalgamation |

## Build-time codegen

CMake generates all derived artifacts under `build/generated/`:

1. `capdb.h` ← `core/capdb.h.in` via `tools/py/mkcapdbh.py`
2. `parse.c/h` ← `core/parse.y` via `lemon`
3. `opcodes.h/c` ← `parse.h` + `core/vdbe.c`
4. `keywordhash.h` ← `tools/mkkeywordhash`
5. `ctime.c`, `fts5.c`, `shell.c` via Python scripts
6. `staging/` ← `tools/py/stage_sources.py` + `staging_manifest.json`
7. `capdb.c` ← `tools/py/mkcapdb.py --srcdir build/generated/staging`

Fresh clone build:

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

Requires: CMake 3.16+, Python 3, OpenSSL (for network), zlib (optional).

## Upstream SQLite merge mapping

When porting upstream SQLite patches:

| Upstream path | CapDB path |
|---------------|------------|
| `src/foo.c` | `core/foo.c` |
| `ext/fts5/` | `extensions/fts5/` |
| `ext/session/` | `extensions/session/` |
| `tool/lemon.c` | `tools/lemon.c` |

After applying patches, rebuild — the amalgamation regenerates automatically.

## CapDB fork code locations

| Component | Path |
|-----------|------|
| Connection pool | `capdb/pool/` |
| Wire protocol | `capdb/proto/` |
| TLS transport | `capdb/tls/` |
| Network client | `capdb/client/` |
| SQL server | `capdb/server/` |
| Remote VFS | `capdb/vfs/` |
| Shell remote SQL | `capdb/shell/` |

## Vendoring the amalgamation

`capdb.c` is **not** checked into git. Obtain it via:

- `cmake --install build` (installs to `share/.../src/capdb.c`)
- `scripts/release.sh` → `dist/capdb-amalgamation-<ver>.tar.gz`
- Build locally: `build/generated/capdb.c`
