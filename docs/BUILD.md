# Building CapDB (CMake)

CapDB uses **CMake** and **Python 3** for builds and code generation. Tcl/JimTCL are not required.

User guide: [CAPDB.md](CAPDB.md) · Layout: [LAYOUT.md](LAYOUT.md) · Migration: [REBRAND.md](REBRAND.md)

## Quick start

```bash
cmake -B build \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_ENABLE_NETWORK=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

No pre-generated `capdb.c` is required — CMake produces `build/generated/capdb.c` automatically.

## Targets

| Target | Output | Description |
|--------|--------|-------------|
| `capdb_cli` | **`capdb`** | Interactive CLI |
| `capdb` | `libcapdb.a` | Core library (amalgamation) |
| `capdb-server` | `capdb-server` | TLS SQL server |
| `capdb_client` | `libcapdb_client.a` | Network client library |
| `capdbtest` | `capdbtest` | Network smoke test |
| `capdb_nettest` | `capdb_nettest` | Network regression cases (subprocess helper) |
| `capsuite` | `capsuite` | C regression suite |
| `threadtest_pool` | `threadtest_pool` | Pool stress test |

## Install

```bash
cmake --install build --prefix /usr/local
```

| Path | Contents |
|------|----------|
| `bin/capdb` | CLI |
| `bin/capdb-server` | Server (if network enabled) |
| `include/capdb.h`, `capdbext.h` | Core headers (from `build/generated/`) |
| `include/capdb_pool.h` | Pool header (if enabled) |
| `include/capdb_client.h` | Client header (if enabled) |
| `lib/libcapdb.a` | Static library |
| `src/capdb.c` | Single-file amalgamation for vendoring |
| `share/man/man1/capdb.1` | CLI man page |

View man pages before install:

```bash
man -l man/capdb.1
```

## Tests

```bash
cd build
CAPSUITE_SERVER=$PWD/capdb-server \
CAPSUITE_CLIENT_TEST=$PWD/capdbtest \
CAPSUITE_NET_TEST=$PWD/capdb_nettest \
./capsuite
```

`capdb_nettest` runs individual network regression cases invoked by capsuite (`capdb-3` through `capdb-13`). Run one case directly:

```bash
CAPSUITE_SERVER=$PWD/capdb-server ./capdb_nettest prepare-step \
  'capdb://127.0.0.1:5432/mnet.db?token=testtoken&insecure=1'
```

Network tests require all three env vars (CTest sets them automatically when network is enabled).

Individual network cases are also registered as CTests (`capdb_net_capdb_*`) and
run serially (`RUN_SERIAL`) to avoid port collisions.

### Compiler warnings

```bash
cmake -B build -DCAPDB_ENABLE_NETWORK=ON -DCAPDB_ENABLE_POOL=ON -DCAPDB_WARNINGS=ON
# Treat warnings as errors (CI):
cmake -B build -DCAPDB_WARNINGS=ON -DCAPDB_WERROR=ON
./tools/warnings.sh
```

Warnings apply to CapDB-owned targets (`capdb_client`, `capdb-server`, tests),
not the full SQLite amalgamation.

`--insecure` disables TLS on the wire; use only for local development. The auth file stores credentials in plaintext.

The server defaults to listening on `127.0.0.1:5432` (not all interfaces).

## Code generation

CMake runs the Python toolchain in `tools/py/` automatically. Manual regeneration:

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON
cmake --build build --target capdb_codegen
```

Key scripts:

| Script | Output |
|--------|--------|
| `tools/py/mkcapdbh.py` | `build/generated/capdb.h` |
| `tools/py/stage_sources.py` | `build/generated/staging/` |
| `tools/py/mkcapdb.py` | `build/generated/capdb.c` |
| `tools/py/mkshellc.py` | `build/generated/shell.c` |

Parity checks: `python3 tools/py/tests/test_codegen_parity.py`

## Clean

`make clean` inside `build/` only removes object files and binaries for that
tree. It does **not** remove generated sources, CTest logs (`Testing/`), lemon
debris, capsuite temp dirs (`capdb_data_*`), or other gitignored artifacts.

From the **repository root**, use the full clean (aligned with [`.gitignore`](../.gitignore)):

```bash
make clean          # sweep all build*/ trees + root gitignored artifacts
make distclean      # delete entire build/, build-*/, cmake-build-*/ (re-run cmake after)
```

`make clean` removes (among other things):

- `build*/generated/`, `build*/Testing/`, `capdb_data_*` test dirs
- Root `compile_commands.json`, `Testing/`, `*.plan.md`, `manifest.tags`
- `parse.sql`, `parse.out`, `__pycache__/`, `.cursor/`

`make distclean` additionally deletes whole `build/`, `build-*/`, `cmake-build-*/`,
`dist/`, and `out/` directories.

From a single build tree:

```bash
cmake --build build --target clean-all
```

## CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `CAPDB_ENABLE_POOL` | ON | Connection pool extension |
| `CAPDB_ENABLE_NETWORK` | ON | TLS network layer (requires pool, OpenSSL) |
| `CAPDB_ENABLE_FTS5` | ON | FTS5 full-text search |
| `CAPDB_ENABLE_MATH_FUNCTIONS` | ON | SQL math functions |
| `CAPDB_ENABLE_PERCENTILE` | ON | Percentile extension |
| `CAPDB_HAVE_ZLIB` | ON | Link zlib |
| `CAPDB_BUILD_TESTS` | ON | Build capsuite, capdbtest, threadtest_pool |

## Extensions

- Network: [../capdb/README.md](../capdb/README.md)
- Pool: [../capdb/pool/README.md](../capdb/pool/README.md)

## Legacy autoconf

The `legacy/autoconf/` tree and `legacy/Makefile.msc` remain for upstream reference but are **not** the CapDB build path.
