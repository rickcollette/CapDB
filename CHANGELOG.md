# Changelog

> **Heritage:** CapDB is a hard fork of [SQLite](https://sqlite.org/) 3.54.x
> (public domain). Lineage: **SQLite 3.54.x → msqlite3 → CapDB**. The SQLite
> engine remains public domain; CapDB's own additions (pool, networking, TLS,
> server, client) are licensed under the MIT License (© 2026 Rick Collette).
> See [LICENSE](LICENSE) / [LICENSE.md](LICENSE.md).

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
