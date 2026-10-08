# Changelog

> **Heritage:** CapDB is a hard fork of [SQLite](https://sqlite.org/) 3.54.x
> (public domain). Lineage: **SQLite 3.54.x → msqlite3 → CapDB**. The SQLite
> engine remains public domain; CapDB's own additions (pool, networking, TLS,
> server, client) are licensed under the MIT License (© 2026 Rick Collette).
> See [LICENSE](LICENSE) / [LICENSE.md](LICENSE.md).

## CapDB 3.7.2 — Release SQL smoke on minimal images

- The release SQL comparison no longer calls `diff`. UBI9 does not install diffutils, and a missing `diff` was reported as a SQL mismatch.

## CapDB 3.7.1 — Windows embedded archive and release SQL smoke

### Security

- A Unix socket is mode `0600` via `fchmod` on the bound descriptor, so a path swapped in after `bind` does not receive the mode change.

### Releases

- The Ubuntu 24.04 release job cross-compiles an x86_64 MinGW archive, `capdb-<version>-windows-x86_64.zip`, containing `capdb.exe`, `capdb.dll`, `libcapdb.dll.a`, and `capdb.h`. Networking, the volume store, and replication are not in that archive. A `.sha256` file is published beside the zip.
- Release builds run `tests/sql/basic.sql` through the embedded `capdb` shell and through `capdb-server` before packaging. The Windows job runs the same queries embedded under Wine. Output is checked against `tests/sql/basic.expected`.

### SQL coverage

- The shared script covers literals and types, table create, insert, update, delete, `COUNT`/`SUM`, a filter, a transaction rollback, a join, and a unique index.

## CapDB 3.7.0 — Network hardening and platform releases

### Security

- The path jail opens and deletes through a directory descriptor of the canonical root, with `O_NOFOLLOW` on every component. A symlink swapped in after the path check cannot redirect the open outside that root.
- TLS setup fails closed when configuration is missing. A TLS server context is refused unless it has both a certificate and a key.
- Password authentication always checks the secret, including when the username does not match.
- A failed TLS handshake owns and closes the accepted socket, so it cannot close a descriptor the process has already reused.

### Network

- `--listen` accepts `host:port`, `[IPv6]:port`, and an absolute Unix socket path created mode `0600`. A regular file at that path is left untouched.
- A `replicas=` entry keeps its own host when the port is omitted, and bracketed IPv6 endpoints parse as addresses.
- A malformed `capdb://` URI frees query strings it already allocated.
- JNI rejects a null URI, and a failed connect copies the error text before the connection is closed.

### Storage and replication

- WAL segment growth frees the previous allocation if the new one cannot be obtained.
- The store VFS keeps the resolved path for the life of the wrapper and preserves the filename layout.

### SQL compatibility

- Production builds enable the session and changeset APIs.
- Production builds enable database, table, and origin column metadata.
- Embedded DSNs accept the production option set.
- Public headers use the canonical SQLite compatibility types.

### Releases

- A version tag builds five platform archives with the Capper release builders: Ubuntu 24.04, Debian 12, RHEL 9, Rocky Linux 10, and Ubuntu 18.04.
- Binary archives are named `capdb-<version>-<platform>.tar.gz`. Every archive has a matching `.sha256` file.
- Release builds drop inherited sanitizer flags so the package is a normal Release build.

## CapDB 3.6.1 — Pure CapDB Language Drivers

### Language Drivers

- Go, Rust, and Python drivers now use CapDB APIs and CapDB libraries only.
- Embedded SQL is backed by `capdb_open_v2`, prepared statements, parameter binding, row stepping, and finalization.
- Network SQL is backed by `capdb_net_connect`, `capdb_net_exec`, `capdb_net_prepare`, `capdb_net_step`, and `capdb_net_finalize`.
- Go provides `database/sql` drivers for `capdb://` network DSNs and local embedded database paths.
- Rust provides FFI-backed `CapDbConnection` modes for embedded and network SQL, including execution and JSON row results.
- Python provides a DB API 2.0 driver over `libcapdb` for embedded and network SQL.

### Build

- Added the shared `libcapdb` target used by dynamic language bindings.
- Installed language build environment helpers as `capdb-go-env`, `capdb-rust-env`, and `capdb-python-env`.
- Kept binding documentation in the repository README and removed standalone binding guide files.

### Network & Client Improvements

- Added Go network-driver liveness checks before blocking row reads.
- Added `token_file=<path>` DSN support for file-backed authentication tokens.
- Added `--quiet` server mode for suppressed audit output in controlled test and service environments.

### Testing

- Added embedded-driver conformance coverage for Go.
- Added embedded and network driver coverage for Rust.
- Verified Python embedded and network smoke paths against `libcapdb`.
- Fixed the CapDB extension loader export-name warning in the generated amalgamation source path.

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
Upstream SQLite merges will not apply cleanly after this rebrand.

### Documentation

- User guide: [docs/CAPDB.md](docs/CAPDB.md)
- Man pages: `capdb(1)`, `capdb-server(1)`
- Pool / network: [capdb/pool/README.md](capdb/pool/README.md), [capdb/README.md](capdb/README.md)

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
