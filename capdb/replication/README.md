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

## Sync modes

- `CAPDB_REP_SYNC_OFF` — async (primary does not wait for replica ACK)
- `CAPDB_REP_SYNC_ON` — primary waits for `capdb_rep_sender_wait_ack` before COMMIT result

Transport reuses `capdb_stream_*` framing from [`capdb_io.c`](../capdb_io.c).
