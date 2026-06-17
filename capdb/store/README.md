# CapDB Store — Volume Format (v1)

Each database is a **CapDB Volume** directory under `--volume-root`:

```
volumes/<db-id>/
  manifest.json
  meta/state
  data/main.db
  wal/
  pages/
  snapshots/
```

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

WAL segments written under `wal/` use the envelope in `capdb_store_format.h`.
Primary streams segments via `capdb_replication`; replicas apply with
`capdb_rep_replica_apply_chunk`.
