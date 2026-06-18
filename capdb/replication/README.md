# CapDB Replication

Primary/replica WAL streaming uses framed segments from `capdb_store` (`wal/*.wal`).

## Message types (`capdb_rep.h`)

| Type | Purpose |
|------|---------|
| `REP_HELLO` / `REP_HELLO_ACK` | Protocol version |
| `REP_AUTH` / `REP_AUTH_OK` | Token auth |
| `REP_STREAM_START` | Begin streaming from LSN |
| `REP_WAL_CHUNK` | WAL segment envelope + payload |
| `REP_ACK` | Replica persisted LSN |
| `REP_PING` / `REP_PONG` | Keepalive |

## Apply semantics

- **Incremental** (normal path): payload is pwritten to `data/main.db-wal` at
  `wal_offset` from the segment header.
- **Snapshot bootstrap**: `wal_offset == CAPDB_STORE_WAL_OFFSET_MAIN_DB` replaces
  `data/main.db` (recovery / initial catch-up). The primary also emits a snapshot
  after autocommit writes until incremental apply alone is sufficient for replica reads.
- **Replica read path**: before opening a volume, replicas replay `wal/*.wal` via
  `capdb_rep_recovery_replay_dir` and checkpoint so `capdbstorevfs` open sees merged state.
- **Generation**: format v2 headers carry `generation`; replicas reject chunks with
  `generation < local.generation` after promote.

## Sync modes

- `CAPDB_REP_SYNC_OFF` — async (primary does not wait for replica ACK)
- `CAPDB_REP_SYNC_ON` — primary waits for min replica ACK (`repMinReplicaAck`)
  before returning autocommit writes and explicit `COMMIT`

## Read routing

Writes: connect to the **primary** URI (`capdb://primary:PORT/vol?token=…`).
Reads: connect to a **replica** URI explicitly (`capdb://replica:PORT/vol?token=…`).
There is no automatic read routing in the protocol yet.

Transport reuses `capdb_stream_*` framing from [`capdb_io.c`](../capdb_io.c).
