# CapDB Connection Pool

Optional connection pool API for CapDB (`capdb_pool_*`).

## Building

With CMake (recommended):

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON
cmake --build build
```

The pool is compiled into the `capdb` amalgamation when enabled. Header:
[`capdb_pool.h`](capdb_pool.h).

## API

- `capdb_pool_open()` / `capdb_pool_close()`
- `capdb_pool_acquire()` / `capdb_pool_release()`
- `capdb_pool_stats()`

Checkout modes: `CAPDB_POOL_READ` and `CAPDB_POOL_WRITE`.

## Usage notes

- Each pooled connection uses a **private page cache** (shared-cache is never enabled).
- New connections default to **WAL mode** and a **busy timeout**.
- **Write checkouts are serialized** by default (`CAPDB_POOL_SERIALIZE_WRITES`).
- WRITE checkouts run `BEGIN IMMEDIATE`; call **COMMIT** before `capdb_pool_release()` when `CAPDB_POOL_RESET_ON_RELEASE` is enabled.
- Only the primary database file passed to `capdb_pool_open()` is managed.
- `:memory:` databases are not supported.

## Tests

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_BUILD_TESTS=ON
cmake --build build
./build/capsuite --filter pool
./build/threadtest_pool
```

Pool tests live in `tests/capdb/capsuite/pool_basic.c` and `pool_busy.c`.

## See also

- [docs/CAPDB.md](../../docs/CAPDB.md)
- [BUILD.md](../../BUILD.md)
