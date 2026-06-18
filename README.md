<p align="center">
  <img src="art/logo/CapDB_Logo.png" alt="CapDB" width="520"/>
</p>

<p align="center">
  <strong>SQLite 3.54 — pooled, networked, and replication-ready</strong>
</p>

<p align="center">
  <a href="https://github.com/rickcollette/CapDB/actions/workflows/ci.yml"><img src="https://github.com/rickcollette/CapDB/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="License: MIT"/></a>
  <a href="https://sqlite.org/"><img src="https://img.shields.io/badge/engine-SQLite%203.54-lightgrey" alt="SQLite 3.54"/></a>
  <a href="docs/BUILD.md"><img src="https://img.shields.io/badge/build-CMake%20%2B%20Python3-2ea44f" alt="CMake build"/></a>
  <a href="SECURITY.md"><img src="https://img.shields.io/badge/security-policy-informational" alt="Security policy"/></a>
</p>

---

**CapDB** is a hard fork of [SQLite](https://sqlite.org/) 3.54 that keeps the familiar SQL dialect and on-disk format, and adds production-oriented server features: a **connection pool**, **TLS networking** (`capdb://`), an optional **HA volume store** with **primary/replica replication**, and a remote **VFS** for embedded clients.

Build with **CMake** — no Tcl required. Tests run via `ctest`, `capsuite`, and native C binaries.

## Highlights

| | |
|---|---|
| **Pool** | `capdb_pool_*` checkout/checkin with WAL defaults and busy-timeout handling |
| **Network** | `capdb://` remote SQL over TLS; `capdb-server` executes on server-side files |
| **HA volumes** | `capdbstorevfs` volume layout, WAL segments, LSN tracking, sync replication |
| **Replication** | Primary streams WAL to replicas; generation fencing and path jails |
| **CLI** | Drop-in `capdb` shell (replaces `sqlite3`); same dot-commands you expect |

## Quick start

```bash
git clone https://github.com/rickcollette/CapDB.git
cd CapDB

cmake -B build \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_ENABLE_NETWORK=ON \
  -DCAPDB_ENABLE_STORE=ON \
  -DCAPDB_ENABLE_REPLICATION=ON \
  -DCAPDB_BUILD_TESTS=ON

cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

**Local shell**

```bash
./build/capdb my.db
./build/capdb -batch my.db 'SELECT 1;'
```

**Remote (dev — no TLS)**

```bash
./build/capdb-server --listen 127.0.0.1:5432 \
  --auth-file ./tokens.txt --db-root ./data --insecure

./build/capdb 'capdb://127.0.0.1:5432/app.db?token=SECRET&insecure=1'
```

Generated sources (`capdb.c`, `capdb.h`, `shell.c`) are produced at build time under `build/generated/`. See [docs/LAYOUT.md](docs/LAYOUT.md).

## Architecture

```mermaid
flowchart TB
  subgraph clients["Clients"]
    CLI["capdb CLI"]
    SDK["libcapdb_client"]
    VFS["capdbvfs (remote pages)"]
  end

  subgraph server["capdb-server"]
    NET["TLS + capdb proto"]
    POOL["capdb_pool"]
    STORE["capdb_store / capdbstorevfs"]
    REP["capdb_replication"]
  end

  CLI --> NET
  SDK --> NET
  VFS --> NET
  NET --> POOL
  POOL --> STORE
  STORE --> REP
  REP -->|"WAL stream"| REPLICA["Replica capdb-server"]
```

| Mode | Storage flag | Use case |
|------|----------------|----------|
| Legacy | `--db-root` | Single-node SQL server over POSIX files |
| Volume / HA | `--storage volume --volume-root` | Replication, failover, LSN-backed WAL |
| Remote VFS | `capdbvfs` on client | Embedded engine; server does page I/O (non-HA) |

## Documentation

| Topic | Document |
|-------|----------|
| User guide | [docs/CAPDB.md](docs/CAPDB.md) |
| Build & install | [docs/BUILD.md](docs/BUILD.md) |
| Repository layout | [docs/LAYOUT.md](docs/LAYOUT.md) |
| HA storage ADR | [docs/adr/001-storage-engine.md](docs/adr/001-storage-engine.md) |
| Changelog | [CHANGELOG.md](CHANGELOG.md) |
| CLI man page | [man/capdb.1](man/capdb.1) |
| Server man page | [man/capdb-server.1](man/capdb-server.1) |
| Network layer | [capdb/README.md](capdb/README.md) |
| Connection pool | [capdb/pool/README.md](capdb/pool/README.md) |
| Volume store | [capdb/store/README.md](capdb/store/README.md) |
| Replication | [capdb/replication/README.md](capdb/replication/README.md) |

## HA example (primary + replica)

```bash
# Primary
capdb-server --storage volume --volume-root /var/lib/capdb/volumes \
  --listen 0.0.0.0:5432 --auth-file /etc/capdb/tokens \
  --rep-listen 0.0.0.0:5433 --rep-token SECRET --cert srv.pem --key srv.key

# Replica (read-only SQL; catches up via replication)
capdb-server --storage volume --volume-root /var/lib/capdb/volumes \
  --role replica --rep-primary primary.host:5433 --rep-token SECRET \
  --listen 0.0.0.0:5432 --auth-file /etc/capdb/tokens \
  --cert srv.pem --key srv.key
```

Client read fan-out: `read_preference=replica` and `replicas=` on the `capdb://` URI. See [capdb/README.md](capdb/README.md).

## Security

> [!CAUTION]
> **`--insecure` disables TLS.** Use it only on loopback or isolated lab networks. Credentials and SQL are visible on the wire.

> [!IMPORTANT]
> Report vulnerabilities through [GitHub private security advisories](https://github.com/rickcollette/CapDB/security/advisories/new) — not public issues. See [SECURITY.md](SECURITY.md) for the full policy.

Built-in hardening includes path jails (`realpath` + prefix checks), replica read-only gates on `EXEC`/`PREPARE`/`STEP`, required replication tokens, `ATTACH` denial in volume mode, and generation fencing on replicated WAL.

## History & licensing

CapDB keeps the SQLite SQL dialect, file format, and engine (**public domain**). CapDB additions — pool, networking, TLS, server, store, replication — are **[MIT licensed](LICENSE)** (© 2026 Rick Collette). See [LICENSE.md](LICENSE.md) for per-path scope.

Lineage: **SQLite 3.54 → msqlite3 → CapDB**. Upstream SQLite heritage: [docs/upstream/README-LEGACY.md](docs/upstream/README-LEGACY.md).
