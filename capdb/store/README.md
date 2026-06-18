# CapDB Store — Volume Format (v1)

Each database is a **CapDB Volume** directory under `--volume-root`:

```
volumes/<db-id>/
  manifest.json
  meta/state
  data/main.db
  data/main.db-wal      # SQLite WAL sidecar (colocated)
  data/main.db-shm      # SQLite SHM sidecar (colocated)
  wal/                  # framed replication segments (*.wal)
  pages/
  snapshots/
```

SQLite sidecars live beside `data/main.db`. Framed replication segments remain
under `wal/` for ordered replay and streaming.

## Server usage

```bash
capdb-server --storage volume --volume-root /var/capdb/volumes \
  --auth-file auth.txt --insecure
```

Clients open `capdb://host:PORT/<db-id>?token=…` — the server resolves `<db-id>` to
`$volume-root/<db-id>/` and opens it through `capdbstorevfs`.

## API

See [`capdb_store.h`](capdb_store.h). Key entry points:

- `capdb_volume_open` / `capdb_volume_close`
- `capdb_store_vfs_register` — register `capdbstorevfs` for pool use
- `capdb_volume_append_wal` — framed WAL segments for replication
- `capdb_volume_export_db` — export classic `.db` file

## Replication

WAL segments written under `wal/` use the envelope in `capdb_store_format.h`
(format v2 includes `generation` for split-brain fencing). Primary streams
segments via `storeWrapWrite` → `capdb_volume_append_wal` → `capdb_rep_primary_on_wal`.
Replicas apply incremental payloads to `data/main.db-wal` (or snapshot bootstrap
via `CAPDB_STORE_WAL_OFFSET_MAIN_DB`). Recovery replays ordered `wal/*.wal` with
`capdb_rep_recovery_replay_dir`.
