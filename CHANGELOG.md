# Changelog

> **Heritage:** CapDB is a hard fork of [SQLite](https://sqlite.org/) 3.54.x
> (public domain). Lineage: **SQLite 3.54.x → msqlite3 → CapDB**. The SQLite
> engine remains public domain; CapDB's own additions (pool, networking, TLS,
> server, client) are licensed under the MIT License (© 2026 Rick Collette).
> See [LICENSE](LICENSE) / [LICENSE.md](LICENSE.md).

## Unreleased — Language Drivers & Network Improvements

### Language Drivers

Complete, production-ready drivers for Go, Rust, and Python:

#### Go Driver (`bindings/go/`)
- Full `database/sql` compatibility with connection pooling
- Context cancellation and timeout support
- Transaction support (BEGIN/COMMIT/ROLLBACK)
- Prepared statements with parameter binding
- Automatic timestamp parsing (`time.Time` scanning)
- Multi-statement SQL execution
- Comprehensive test suite with live server integration
- Example applications demonstrating all features

#### Rust Driver (`bindings/rust/`)
- Safe FFI bindings to `libcapdb_client`
- Support for Embedded, LocalServer, and HighAvailability modes
- Error handling with custom error types (`thiserror`)
- Connection pooling via `CapDbPool`
- JSON query result support
- Schema management helpers

#### Python Driver (`bindings/python/`)
- DB API 2.0 compliant (works with SQLAlchemy, Pandas)
- Support for embedded and network modes
- Thread-safe cursor and connection management
- Context manager support for resource cleanup
- Full error hierarchy matching DB API 2.0 spec
- Type hints for IDE support

### Network & Client Improvements

#### Query Hang Robustness
- Add connection liveness check in Go driver's `rows.Next()` before blocking on socket I/O
- Prevents indefinite hangs when transport fails
- Marks connection dead for pool eviction and reconnection

#### DSN Token File Support
- New `token_file=<path>` parameter for URI parsing
- Reads authentication token from file (first line)
- Enables systemd-friendly secret management via credential files

#### Audit Logging Control
- Add `--quiet` flag to `capdb-server` to suppress verbose audit output
- Reduces test output noise during repeated startup failures
- Configurable per-server instance via `bQuiet` in config

#### Server Improvements
- Suppress connection/auth audit logs in quiet mode
- Improve error messages for auth failures
- Support token file as alternative to inline token

### Build Ergonomics

#### Build Helper Scripts
- `capdb-go-env.sh` — Auto-detects CapDB build directory and emits CGO flags
- `capdb-rust-env.sh` — Sets up Rust FFI build environment
- `capdb-python-env.sh` — Configures Python ctypes/cffi paths

#### pkg-config Integration
- `libcapdb_client.pc` template for standard toolchain queries
- Enables `pkg-config --cflags --libs libcapdb_client` workflow
- CMake installs pkg-config file to standard location

#### CMake Updates
- Install build helper scripts to `/usr/local/bin`
- Install pkg-config file to `/usr/local/lib/pkgconfig`
- Install language binding guides to `/usr/local/share/doc/capdb`

### Documentation

#### New Guides
- `LANGUAGE_DRIVERS.md` — Overview and quick start for all three drivers
- `docs/DRIVERS_GUIDE.md` — 500+ lines with installation, usage patterns, and integration examples
- `docs/README.md` — Documentation index organized by role (Users, Developers, DevOps)
- `BINDINGS_BUILD.md` — Comprehensive guide for integrating CapDB into language-specific projects

#### Documentation Content
- Multi-language examples (Go, Rust, Python)
- Connection pooling configuration
- Error handling patterns per language
- Performance tuning and troubleshooting
- SQLAlchemy and Pandas integration
- Deployment considerations

### Code Quality & Testing

#### Go Driver
- Nil checks in `rows.Next()` prevent panics on closed rows
- DSN validation catches empty strings early
- Explicit CAPDB_ROW/CAPDB_DONE constants for clarity
- Integration tests for transactions, concurrency, timestamps
- Context cancellation tests

#### Python Driver
- Fixed `cursor.description` to comply with DB API 2.0 (7-tuple format)
- Added connection state tracking with `_closed` flag
- Implemented idempotent `close()` to prevent double-close errors
- Parameter validation with informative error messages
- Proper error handling in connection cleanup

#### Rust Driver
- FFI bindings framework for `capdb_net_*` functions
- Safe wrapper API with thiserror integration
- Mode-specific connection validation

### Breaking Changes

None. All improvements are backward compatible.

### Known Limitations

1. Rust/Python network mode requires C FFI implementation (framework in place)
2. Async support in Rust is placeholder (tokio integration planned)
3. Prepared statement caching not yet implemented
4. Python currently uses SQLite for embedded mode (CapDB native TBD)

## CapDB 3.54.0 — Full rebrand (breaking)

This release renames the fork from **msqlite3** / SQLite product naming to **CapDB**.
There are **no compatibility shims**.

### Product

- Project: `capdb` (CMake `project(capdb)`)
- CLI binary: **`capdb`** (was `sqlite3`)
- Library: `libcapdb` / `capdb.c` + `capdb.h` (was `libsqlite3` / `sqlite3.c`)

### API

- `sqlite3_*` → `capdb_*`
- `SQLITE_*` → `CAPDB_*`
- `sqlite3_pool_*` → `capdb_pool_*`
- Network client: `capdb_net_*` (remote connect/exec; avoids collision with core API)

### Network

- URI scheme: **`capdb://`** only (`msqlite://` removed)
- Server: `capdb-server`
- Client library: `libcapdb_client`
- VFS: `"capdbvfs"`
- CMake: `CAPDB_ENABLE_NETWORK`, `CAPDB_ENABLE_POOL`

### Tests

- `msuite` → **`capsuite`**
- `msqlitest` → **`capdbtest`**
- Env: `CAPSUITE_SERVER`, `CAPSUITE_CLIENT_TEST`

### Build

- Codegen: `tool/py/mkcapdb.py`, `tool/py/mkcapdbh.py`
- See [docs/REBRAND.md](docs/REBRAND.md) for migration details.

Upstream SQLite merges will not apply cleanly after this rebrand.

### Documentation

- User guide: [docs/CAPDB.md](docs/CAPDB.md)
- Man pages: `capdb(1)`, `capdb-server(1)`
- Pool / network: [capdb/pool/README.md](../capdb/pool/README.md), [capdb/README.md](../capdb/README.md)

## Network server — review remediation (v2/v3)

### Breaking: EXEC after PREPARE (C1)

The server now requires clients to **finalize all prepared statements** before
sending `EXEC` on the same session. Interleaving `PREPARE` and `EXEC` without
`FINALIZE` returns `CAPDB_MISUSE` (“finalize prepared statements first”).

### v3: path locking and pool/VFS exclusion

A canonical per-path registry prevents concurrent access to the same database
file through the connection pool and remote VFS on one server. `VFS_OPEN` while
the pool has a checkout (or during an explicit transaction) returns `CAPDB_BUSY`.
VFS reserved/pending locks are enforced process-wide.

### v3: protocol and DoS bounds

- `CAPDB_PROTO_VERSION` is validated in `HELLO_ACK` on both sides.
- `EXEC` result streaming is capped (`CAPDB_EXEC_MAX_ROWS`, byte budget).
- Post-auth idle recv timeout (`CAPDB_IDLE_TIMEOUT_MS`, 15 minutes); use `PING`
  as keepalive for long-lived connections.
- `COMMIT`/`ROLLBACK` without `BEGIN` returns `CAPDB_MISUSE`.
- Auth failure tracking is capped per peer (`AUTH_MAX_PEERS`).

### Build

- CMake options: `CAPDB_WARNINGS` (default ON), `CAPDB_WERROR` (CI optional).
- `tools/warnings.sh` builds CapDB-owned targets with `-Wall -Wextra`.

## HA storage — Phase B / B+ (unreleased)

### Phase B — `capdbstorevfs` restore

- SQLite sidecars colocated at `data/main.db-wal` / `data/main.db-shm`
- Volume refcount held for main, WAL, and SHM opens (fixes back-to-back write READONLY)
- Server opens volume id via `capdbstorevfs` (removed unix-VFS workaround and snapshot replicate)
- Replication driven by `storeWrapWrite` → incremental WAL chunks
- Primary still sends a post-autocommit `main.db` snapshot for replica read consistency (incremental WAL apply alone is not yet sufficient for SQLite open on replicas)
- Replicas replay `wal/*.wal` and checkpoint before opening a volume for reads

### Phase B+ — HA hardening

- Sync replication waits on autocommit volume writes (`--sync-replication`)
- WAL header format v2 adds `generation`; replicas reject stale generations
- Ordered WAL replay in `capdb_rep_recovery_replay_dir` with SQLite checkpoint
- Server-owned active sender (`capdb_rep_sender_set_active`); min-replica ACK quorum

### Tests

- `capdb-26-volume-multi-write` — two consecutive INSERTs on volume server
- `capdb-27-rep-sync` — sync replication primary → replica read
- `store_test` direct volume SQL re-enabled
