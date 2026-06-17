# ADR-001: CapDB Store Engine vs Remote VFS for HA

## Status

Accepted

## Context

CapDB provides two single-node remote access paths:

1. **Remote SQL** (`capdb-server` + `capdb_pool`) — SQL executes on the server against local files.
2. **Remote VFS** (`capdbvfs`) — the SQLite pager runs on the client; the server exposes byte-level page I/O.

Both assume one authoritative server and POSIX files under `--db-root`. There is no replication, failover, or cross-node coordination. The path registry prevents concurrent pool and VFS access to the same file on one host.

Goals for HA storage:

- Multi-connection, server-authoritative SQL
- Primary + replica replication with failover (Postgres streaming style)
- New on-disk format optimized for replication and operations
- Keep the CapDB/SQLite SQL engine

## Decision

Introduce **`capdb_store`** — a volume manager and server-side VFS (`capdbstorevfs`) below the connection pool — as the foundation for HA. Do **not** extend `capdbvfs` for clustering.

| Layer | Role |
|-------|------|
| `capdb_net_*` client | SQL over TLS (unchanged wire protocol v1) |
| `capdb-server` | Session + pool + optional replication |
| `capdb_pool` | Checkout; uses `capdbstorevfs` in volume mode |
| `capdb_store` | Volume layout, durability, WAL segments, LSN |
| `capdb_replication` | Primary → replica WAL stream |
| `capdb_cluster` | Roles, generation, promotion |

**Remote VFS (`capdbvfs`)** remains for embedded “remote file” use cases and is documented as **non-HA**.

## Volume format (v1)

Each database is a directory (CapDB Volume):

```
volumes/<db-id>/
  manifest.json
  meta/state
  data/main.db          # SQLite database file (page-compatible)
  wal/                  # WAL segments + SQLite -wal/-shm redirected here
  pages/                # optional chunk files (future)
  snapshots/            # checkpoint snapshots (phase 5)
```

## Non-goals

- **Multi-writer / active-active** across nodes
- **Shared NFS/POSIX** multi-primary on one file
- **Replacing the SQLite pager** or B-tree in phase 1
- **Client-side store access** — clients never read volume format directly

## Consequences

- New CMake option `CAPDB_ENABLE_STORE` (implies pool)
- Server flags: `--storage legacy|volume`, `--volume-root`
- Legacy `--db-root` flat files remain supported for dev/single-node
- Replication uses a separate `REP_*` protocol on the existing TLS/frame transport
