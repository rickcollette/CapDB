# CapDB Language Drivers

Language bindings for CapDB in Go, Rust, and Python.

## Overview

| Language | Status | Features | Location |
|----------|--------|----------|----------|
| **Go** | Network + embedded SQL ready | database/sql drivers, context support, pooling via database/sql, timestamp parsing | `bindings/go/` |
| **Rust** | Network + embedded SQL ready | FFI-backed connections, execution, JSON query results, errors | `bindings/rust/` |
| **Python** | Network + embedded SQL ready | DB API 2.0, ctypes over `libcapdb`, thread-safe | `bindings/python/` |

## Coverage Target

The language drivers cover both CapDB SQL execution modes. Lower-level administrative surfaces are tracked separately from SQL-driver conformance:

| Area | Go | Rust | Python |
|------|----|------|--------|
| Embedded SQL (`capdb_open_v2`, prepare/bind/step/finalize) | Covered | Covered | Covered |
| Network SQL (`capdb_net_connect`, exec, prepare/step/finalize, values) | Covered | Covered | Covered |
| SQL transactions and result metadata | Covered | Covered | Covered |
| Driver-level pooling | Covered by `database/sql` | Use application pool over `CapDbConnection` | Use application pool over DB API connections |
| Remote VFS registration and raw VFS RPC | C API only | C API only | C API only |
| Store, replication, and cluster administration | C API / tools only | C API / tools only | C API / tools only |

The loadable library for dynamic bindings is the full `libcapdb` shared library, because it exports the embedded API and the network API together.

## Quick Start

### Go Driver

```bash
# Build
cd bindings/go
eval $(../../capdb-go-build-env.sh ../../build)
go build -tags capdb ./...

# Test
go test -tags capdb ./capdb

# Use
import _ "capdb/capdb"

db, _ := sql.Open("capdb", "capdb://localhost:5432/db.capdb?token=secret&insecure=1")
embedded, _ := sql.Open("capdb-embedded", "local.capdb")
rows, _ := db.Query("SELECT * FROM users")
```

### Rust Driver

```bash
# Build
cd bindings/rust
cargo build

# Test
cargo test

# Use
use capdb::{CapDbConnection, CapDbMode};

let mode = CapDbMode::Network {
    uri: "capdb://localhost:5432/db.capdb?token=secret".to_string(),
};
let conn = CapDbConnection::open(mode)?;
```

### Python Driver

```bash
# Install
cd bindings/python
pip install -e .

# Use
import capdb

conn = capdb.connect("capdb://localhost:5432/test.capdb?token=secret&insecure=1")
embedded = capdb.connect("local.capdb")
cursor = conn.cursor()
cursor.execute("SELECT * FROM users")
rows = cursor.fetchall()
```

## Go Driver (`bindings/go/`)

### Features

- **Full database/sql support** — Use with any Go ORM (sqlc, gorm, ent)
- **Context cancellation** — Abort queries with context timeouts
- **Connection pooling** — Automatic pool management via database/sql
- **Transaction support** — Full ACID transactions with Rollback/Commit
- **Type safety** — Native Go time.Time, int64, float64, []byte support
- **Prepared statements** — Efficient query parameter binding
- **Error handling** — Clear, actionable error messages

### API Example

```go
import (
    "database/sql"
    _ "capdb/capdb"
)

// Open connection
db, err := sql.Open("capdb", "capdb://localhost:5432/db.capdb?token=secret&insecure=1")
defer db.Close()

// Query with parameters
var name string
err = db.QueryRow("SELECT name FROM users WHERE id = ?", 1).Scan(&name)

// Insert
result, err := db.Exec("INSERT INTO users (name) VALUES (?)", "Alice")
id, _ := result.LastInsertId()

// Transaction
tx, err := db.Begin()
tx.Exec("INSERT INTO users (name) VALUES (?)", "Bob")
tx.Commit()

// Scan timestamps
var createdAt time.Time
err = db.QueryRow("SELECT created_at FROM users WHERE id = 1").Scan(&createdAt)
```

### Build Requirements

```bash
CGO_ENABLED=1
CGO_CFLAGS=-I/path/to/CapDB/capdb/client -DCAPDB_ENABLE_NETWORK=1
CGO_LDFLAGS=-L/path/to/build -Wl,-rpath,/path/to/build -lcapdb -lcapdb_store -lssl -lcrypto -lpthread -lm
```

### Tests

```bash
make test
```

## Rust Driver (`bindings/rust/`)

### Features

- **Safe FFI** — Zero-cost bindings with Rust safety guarantees
- **Error handling** — Custom error types with thiserror
- **Connection modes** — Embedded and Network
- **Type safety** — Strong types for SQL values

### API Example

```rust
use capdb::{CapDbMode, CapDbConnection, Result};

fn main() -> Result<()> {
    let mode = CapDbMode::Network {
        uri: "capdb://localhost:5432/test.capdb?token=secret".to_string(),
    };
    
    let conn = CapDbConnection::open(mode)?;
    
    // Execute DDL
    conn.execute("CREATE TABLE users (id INTEGER, name TEXT)")?;
    
    // Apply schema
    conn.apply_schema(r#"
        CREATE TABLE IF NOT EXISTS posts (
            id INTEGER PRIMARY KEY,
            user_id INTEGER,
            title TEXT
        );
    "#)?;
    
    // Query as JSON
    let results = conn.query_json("SELECT * FROM users")?;
    println!("{}", results);
    
    Ok(())
}
```

### Build Requirements

```bash
CAPDB_INCLUDE_DIR=/path/to/CapDB/capdb/client
CAPDB_LIB_DIR=/path/to/build
```

### Tests

```bash
cargo test
```

## Python Driver (`bindings/python/`)

### Features

- **DB API 2.0 compliant** — Works with sqlalchemy, pandas, etc.
- **Context managers** — Clean resource management
- **Thread-safe** — Automatic locking for concurrent access
- **Embedded and network support** — Local files and CapDB server connections through `libcapdb`
- **Type hints** — Full type annotation for IDE support
- **Error handling** — DB API 2.0 exception hierarchy

### API Example

```python
import capdb

# Network connection
conn = capdb.connect("capdb://localhost:5432/test.capdb?token=secret&insecure=1")

# Embedded connection
embedded = capdb.connect("local.capdb")

# Context manager (auto-commit/rollback)
with capdb.connect("local.capdb") as conn:
    cursor = conn.cursor()
    
    # Execute DDL
    cursor.execute("CREATE TABLE users (id INTEGER, name TEXT)")
    
    # Insert
    cursor.execute("INSERT INTO users VALUES (?, ?)", (1, "Alice"))
    
    # Query
    cursor.execute("SELECT * FROM users")
    rows = cursor.fetchall()
    for row in rows:
        print(row)
    
    # Fetch one
    cursor.execute("SELECT name FROM users WHERE id = ?", (1,))
    name = cursor.fetchone()
    
    # Fetch many
    cursor.execute("SELECT * FROM users")
    batch = cursor.fetchmany(10)

conn.commit()
conn.close()
```

### SQLAlchemy Integration

```python
from sqlalchemy import create_engine

engine = create_engine("capdb://localhost:5432/test.capdb?token=secret&insecure=1")
```

### Tests

```bash
pip install -e ".[dev]"
pytest -v
```

## DSN Format

All drivers support the same DSN format:

```
capdb://[user:password@]host:port/database.capdb?param1=value1&param2=value2
```

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `token` | string | - | Authentication token |
| `token_file` | path | - | Path to file containing token (one line) |
| `password` | string | - | Username/password authentication |
| `insecure` | bool | false | Disable TLS verification |
| `ca` | path | - | CA certificate bundle |
| `connect_timeout` | int | 5 | Connection timeout (seconds) |

### Examples

```
# Network with token auth
capdb://localhost:5432/app.capdb?token=secret&insecure=1

# Network with token file
capdb://db.example.com:5432/prod.capdb?token_file=/etc/capdb-token&ca=/etc/ssl/certs/ca.pem

# High availability (Rust only)
capdb+ha://primary:5432/db.capdb?replicas=replica1:5432,replica2:5432
```

## Error Handling

### Go

```go
import (
    "database/sql"
    "capdb/capdb"
)

db, err := sql.Open("capdb", dsn)
if err != nil {
    // Connection error
}

rows, err := db.Query("SELECT ...")
if err != nil {
    // Query error (wrapped as sql.Error)
}
```

### Rust

```rust
use capdb::{CapDbError, Result};

match conn.execute("SELECT ...") {
    Ok(_) => {},
    Err(CapDbError::Query(msg)) => println!("Query error: {}", msg),
    Err(CapDbError::Connection(msg)) => println!("Connection error: {}", msg),
    Err(e) => println!("Other error: {}", e),
}
```

### Python

```python
from capdb.errors import (
    OperationalError,
    ProgrammingError,
    IntegrityError,
)

try:
    cursor.execute("SELECT ...")
except IntegrityError as e:
    print(f"Constraint violation: {e}")
except ProgrammingError as e:
    print(f"SQL error: {e}")
except OperationalError as e:
    print(f"Connection error: {e}")
```

## Performance Considerations

### Connection Pooling

**Go**: database/sql automatically manages pooling
```go
db.SetMaxOpenConns(25)
db.SetMaxIdleConns(5)
db.SetConnMaxLifetime(5 * time.Minute)
```

**Rust**: Use an application-level pool of `CapDbConnection` objects.

**Python**: SQLAlchemy pool (when using ORM)
```python
from sqlalchemy.pool import QueuePool
engine = create_engine(..., poolclass=QueuePool, pool_size=10)
```

### Query Optimization

1. Use prepared statements for repeated queries
2. Batch insert operations with transactions
3. Limit result sets with LIMIT clause
4. Use indexes on frequently queried columns

## Concurrency

**Go**: Thread-safe via database/sql; use connection pool
**Rust**: `CapDbConnection` wraps one native handle; use one connection per concurrent worker.
**Python**: Thread-safe with automatic locking; use connection pool

## CapDB-Only Driver Names

```go
import _ "capdb/capdb"
db, _ := sql.Open("capdb", "capdb://localhost:5432/test.capdb?token=secret")
```

## Testing

All drivers include comprehensive test suites:

```bash
# Go
cd bindings/go
make test

# Rust
cd bindings/rust
cargo test

# Python
cd bindings/python
pytest -v
```

## Status & Improvements

### Recent Improvements (Code Review Rounds 1-2)

**Round 1 - Critical Fixes:**
- Go: Added nil checks in rows.Next() to prevent panics on closed rows
- Python: Fixed cursor.description to comply with DB API 2.0 specification
- Python: Added proper error handling in connection cleanup

**Round 2 - Robustness:**
- Go: Added DSN validation to catch invalid inputs early
- Python: Added connection state tracking with _closed flag
- Python: Implemented idempotent close() to prevent double-close errors
- Python: Better parameter validation with informative error messages

### Known Limitations

1. **Embedded mode** is not exposed until it is backed by CapDB's own C API
2. Async Rust APIs are not exposed; use blocking `CapDbConnection` from a worker thread
3. **Python** requires `libcapdb` at runtime
4. **Prepared statement caching** not yet implemented
5. **Rust** FFI bindings are framework-only (C layer connections TBD)

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for guidelines.

## License

MIT License - see [LICENSE](../LICENSE) for details.

## Support

- Report issues: [GitHub Issues](https://github.com/rickcollette/CapDB/issues)
- Discussions: [GitHub Discussions](https://github.com/rickcollette/CapDB/discussions)
- Documentation: [CapDB Docs](https://github.com/rickcollette/CapDB/tree/main/docs)
