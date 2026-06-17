# CapDB Network Layer

Optional remote CapDB access over TLS.

## Build (CMake)

```bash
cmake -B build \
  -DCAPDB_ENABLE_POOL=ON \
  -DCAPDB_ENABLE_NETWORK=ON \
  -DCAPDB_ENABLE_STORE=ON \
  -DCAPDB_ENABLE_REPLICATION=ON
cmake --build build --target capdb-server capdbtest capsuite capdb_cli capdb-ctl
```

Requires OpenSSL (`libssl`, `libcrypto`). No Tcl dependency.

See [docs/BUILD.md](../docs/BUILD.md) for full build and test instructions.

Man pages: [capdb(1)](../man/capdb.1), [capdb-server(1)](../man/capdb-server.1)

## Architecture

- **capdb-server** — TLS SQL server; executes SQL via the connection pool on server-side database files.
- **libcapdb_client** — C SDK (`capdb_net_connect`, `capdb_net_exec`, `capdb_net_prepare`, …).
- **capdbvfs** — Remote VFS: CapDB engine on client, page I/O over the network (DELETE journal mode).

## URI format

```
capdb://HOST:PORT/PATH/TO/DB?token=SECRET
capdb://HOST:PORT/PATH/TO/DB?ca=/path/ca.pem
capdb://HOST:PORT/PATH/TO/DB?user=alice&password=secret
capdb://HOST:PORT/PATH/TO/DB?token=x&insecure=1   # dev only
```

Default port: **5432**.

## Server

```bash
capdb-server --listen 0.0.0.0:5432 \
  --cert server.pem --key server.key \
  --auth-file /etc/capdb/users.db \
  --db-root /var/lib/capdb/databases
```

Auth file: one token per line, or `user:password` lines. Paths in `OPEN` must resolve under `--db-root`.

Development without TLS: add `--insecure` (client must pass `insecure=1` in URI).

**Security note:** In insecure mode the wire has **no TLS** and frames have **no
integrity protection** beyond TCP. Use only on loopback or isolated lab networks;
credentials and SQL are visible on the wire.

## CLI

```bash
capdb 'capdb://localhost:5432/test.db?token=SECRET&insecure=1'
```

## Tests

```bash
cd build
CAPSUITE_SERVER=$PWD/capdb-server \
CAPSUITE_CLIENT_TEST=$PWD/capdbtest \
./capsuite --filter capdb
./capdbtest 'capdb://127.0.0.1:5432/db?token=TOKEN&insecure=1'
```

## Remote VFS

Register the VFS and open with a URI query parameter:

```c
capdb_net_vfs_register("capdb://host:5432/app.db?token=x&insecure=1", 0);
capdb_open_v2("file:local.db?vfs=capdbvfs", &db, ...);
```

See `capdb/vfs/capdb_vfs.c` for RPC details.

## HA volume storage (cluster mode)

Opt-in volume-backed storage for primary/replica replication:

```bash
capdb-server --storage volume --volume-root /var/lib/capdb/volumes \
  --auth-file /etc/capdb/users.db --insecure \
  --rep-listen 0.0.0.0:5433 --rep-token SECRET
```

Replica:

```bash
capdb-server --storage volume --volume-root /var/lib/capdb/volumes \
  --role replica --rep-primary 127.0.0.1:5433 --rep-token SECRET \
  --auth-file /etc/capdb/users.db --insecure
```

Client read fan-out (writes go to primary URI host; `SELECT` uses first replica):

```
capdb://PRIMARY:5432/app.db?token=x&read_preference=replica&replicas=REP1:5432,REP2:5433
```

Admin: `capdb-ctl status|promote|backup|export --volume PATH`

See [docs/adr/001-storage-engine.md](../docs/adr/001-storage-engine.md) and `capdb/store/README.md`.
