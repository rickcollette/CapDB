# CapDB Language Drivers - Complete Guide

This guide covers all aspects of using CapDB with Go, Rust, and Python, including installation, configuration, and best practices.

## Table of Contents

1. [Go Driver](#go-driver)
2. [Rust Driver](#rust-driver)
3. [Python Driver](#python-driver)
4. [Common Patterns](#common-patterns)
5. [Performance Tuning](#performance-tuning)
6. [Troubleshooting](#troubleshooting)

## Go Driver

### Installation

The Go driver is available in `bindings/go/` and requires CapDB to be built with network support.

```bash
# Clone/download CapDB
git clone https://github.com/rickcollette/CapDB.git
cd CapDB

# Build CapDB
mkdir build && cd build
cmake -DCAPDB_ENABLE_NETWORK=ON ..
make -j4

# Set up Go environment
cd ../bindings/go
eval $(../../capdb-go-build-env.sh ../../build)
```

### Basic Usage

```go
package main

import (
    "database/sql"
    "fmt"
    "log"
    
    _ "capdb/capdb"
)

func main() {
    // Connect to CapDB server
    db, err := sql.Open("capdb", 
        "capdb://localhost:5432/myapp.capdb?token=secret&insecure=1")
    if err != nil {
        log.Fatal(err)
    }
    defer db.Close()
    
    // Verify connection
    if err := db.Ping(); err != nil {
        log.Fatal(err)
    }
    
    // Execute query
    var count int
    err = db.QueryRow("SELECT COUNT(*) FROM users").Scan(&count)
    if err != nil {
        log.Fatal(err)
    }
    fmt.Printf("User count: %d\n", count)
}
```

### Prepared Statements

```go
// Prepare statement
stmt, err := db.Prepare("INSERT INTO users (name, email) VALUES (?, ?)")
if err != nil {
    log.Fatal(err)
}
defer stmt.Close()

// Execute with different parameters
result, err := stmt.Exec("Alice", "alice@example.com")
if err != nil {
    log.Fatal(err)
}

// Get last insert ID
id, _ := result.LastInsertId()
fmt.Printf("Inserted user with ID: %d\n", id)
```

### Transactions

```go
// Start transaction
tx, err := db.Begin()
if err != nil {
    log.Fatal(err)
}

// Execute statements
_, err = tx.Exec("INSERT INTO users (name) VALUES (?)", "Bob")
if err != nil {
    tx.Rollback()
    log.Fatal(err)
}

_, err = tx.Exec("UPDATE accounts SET balance = balance - 100 WHERE user_id = 1")
if err != nil {
    tx.Rollback()
    log.Fatal(err)
}

// Commit
if err := tx.Commit(); err != nil {
    log.Fatal(err)
}
```

### Context and Timeouts

```go
import "context"

// Create context with 5-second timeout
ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
defer cancel()

// Execute query with context
rows, err := db.QueryContext(ctx, "SELECT * FROM large_table")
if err != nil {
    log.Fatal(err)
}
defer rows.Close()

// Iterate results
for rows.Next() {
    // Process row
}
```

### Working with Timestamps

```go
import "time"

var id int
var createdAt time.Time

// Scan CURRENT_TIMESTAMP into time.Time
err := db.QueryRow(
    "SELECT id, created_at FROM users WHERE id = 1",
).Scan(&id, &createdAt)

if err != nil {
    log.Fatal(err)
}

fmt.Printf("User created at: %s\n", createdAt.Format(time.RFC3339))
```

### Connection Pool Configuration

```go
// Set maximum number of open connections
db.SetMaxOpenConns(25)

// Set maximum number of idle connections
db.SetMaxIdleConns(5)

// Set maximum lifetime for a connection
db.SetConnMaxLifetime(time.Minute * 5)

// Get connection stats
stats := db.Stats()
fmt.Printf("Open connections: %d\n", stats.OpenConnections)
fmt.Printf("In-use: %d\n", stats.InUse)
fmt.Printf("Idle: %d\n", stats.Idle)
```

### Error Handling

```go
import (
    "database/sql"
    "errors"
)

rows, err := db.Query("SELECT * FROM users")
if err != nil {
    if errors.Is(err, sql.ErrNoRows) {
        fmt.Println("No users found")
    } else {
        log.Fatal(err)
    }
}
```

## Rust Driver

### Installation

Add to your `Cargo.toml`:

```toml
[dependencies]
capdb = { path = "../CapDB/bindings/rust", features = ["network"] }
```

Or install from the repository:

```bash
cd bindings/rust
cargo add --path .
```

### Basic Usage

```rust
use capdb::{CapDbMode, CapDbConnection, CapDbError};

fn main() -> Result<(), CapDbError> {
    // Create connection
    let mode = CapDbMode::Embedded {
        path: "mydb.capdb".into(),
    };
    
    let conn = CapDbConnection::open(mode)?;
    
    // Execute DDL
    conn.execute("CREATE TABLE users (id INTEGER, name TEXT)")?;
    
    // Query as JSON
    let users = conn.query_json("SELECT * FROM users")?;
    println!("Users: {}", users);
    
    Ok(())
}
```

### Schema Management

```rust
let schema = r#"
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        email TEXT NOT NULL UNIQUE,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    );
    
    CREATE TABLE IF NOT EXISTS posts (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        user_id INTEGER NOT NULL,
        title TEXT,
        content TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (user_id) REFERENCES users(id)
    );
"#;

conn.apply_schema(schema)?;
```

### Error Handling

```rust
use capdb::{CapDbError, Result};

fn safe_operation() -> Result<()> {
    match conn.execute("INVALID SQL") {
        Ok(_) => println!("Success"),
        Err(CapDbError::Query(msg)) => {
            eprintln!("Query error: {}", msg);
            // Handle query-specific errors
        }
        Err(CapDbError::Connection(msg)) => {
            eprintln!("Connection error: {}", msg);
            // Handle connection errors
        }
        Err(e) => {
            eprintln!("Other error: {}", e);
        }
    }
    Ok(())
}
```

### Connection Modes

```rust
// Embedded mode
let embedded = CapDbMode::Embedded {
    path: "local.capdb".into(),
};

// Network mode
let network = CapDbMode::LocalServer {
    uri: "capdb://db.example.com:5432/prod.capdb?token=secret".to_string(),
};

// High availability mode
let ha = CapDbMode::HighAvailability {
    primary_uri: "capdb://primary.example.com:5432/db.capdb".to_string(),
    replicas: vec![
        "capdb://replica1.example.com:5432/db.capdb".to_string(),
        "capdb://replica2.example.com:5432/db.capdb".to_string(),
    ],
};

let conn = CapDbConnection::open(embedded)?;
```

## Python Driver

### Installation

```bash
cd bindings/python
pip install -e .
```

### Basic Usage

```python
import capdb

# Create connection
conn = capdb.connect("file:test.capdb")

# Create cursor
cursor = conn.cursor()

# Execute DDL
cursor.execute("""
    CREATE TABLE users (
        id INTEGER PRIMARY KEY,
        name TEXT,
        email TEXT
    )
""")

# Insert data
cursor.execute("INSERT INTO users (name, email) VALUES (?, ?)", 
               ("Alice", "alice@example.com"))

# Query data
cursor.execute("SELECT * FROM users")
for row in cursor.fetchall():
    print(row)

# Commit and close
conn.commit()
conn.close()
```

### Context Managers

```python
import capdb

# Automatic commit/rollback and cleanup
with capdb.connect("file:test.capdb") as conn:
    cursor = conn.cursor()
    cursor.execute("INSERT INTO users VALUES (?, ?)", (1, "Bob"))
    # Auto-commits on success, rolls back on exception
```

### Fetch Methods

```python
cursor.execute("SELECT * FROM users")

# Fetch one row
row = cursor.fetchone()
print(row)  # (1, 'Alice', 'alice@example.com')

# Fetch specific number of rows
cursor.execute("SELECT * FROM users")
rows = cursor.fetchmany(10)  # Get up to 10 rows

# Fetch all remaining rows
cursor.execute("SELECT * FROM users")
all_rows = cursor.fetchall()
```

### Error Handling

```python
from capdb.errors import (
    OperationalError,
    ProgrammingError,
    IntegrityError,
    DatabaseError,
)

try:
    cursor.execute("INSERT INTO users (email) VALUES (?)", ("duplicate@example.com",))
    conn.commit()
except IntegrityError as e:
    print(f"Integrity constraint violated: {e}")
    conn.rollback()
except ProgrammingError as e:
    print(f"SQL syntax error: {e}")
    conn.rollback()
except OperationalError as e:
    print(f"Connection or database error: {e}")
    conn.rollback()
except DatabaseError as e:
    print(f"General database error: {e}")
    conn.rollback()
```

### SQLAlchemy Integration

```python
from sqlalchemy import create_engine, Column, Integer, String, DateTime
from sqlalchemy.orm import declarative_base, Session
from datetime import datetime

# Create engine (embedded mode)
engine = create_engine("capdb:///file:mydb.capdb")

# Define model
Base = declarative_base()

class User(Base):
    __tablename__ = "users"
    id = Column(Integer, primary_key=True)
    name = Column(String)
    email = Column(String, unique=True)
    created_at = Column(DateTime, default=datetime.utcnow)

# Create tables
Base.metadata.create_all(engine)

# Use with session
with Session(engine) as session:
    user = User(name="Charlie", email="charlie@example.com")
    session.add(user)
    session.commit()
```

### Pandas Integration

```python
import pandas as pd
import capdb

conn = capdb.connect("file:test.capdb")

# Read entire table into DataFrame
df = pd.read_sql("SELECT * FROM users", conn)

# Read with query
df = pd.read_sql("SELECT name, email FROM users WHERE id > ?", 
                 conn, params=(5,))

# Write DataFrame to database
df.to_sql("exported_users", conn, if_exists="append")

conn.close()
```

## Common Patterns

### Connection String Management

Store connection strings in environment variables:

```bash
# .env file
CAPDB_DSN=capdb://localhost:5432/myapp.capdb?token=secret&insecure=1

# In application
import os
dsn = os.environ.get("CAPDB_DSN")
```

### Retry Logic

```python
import time
from capdb.errors import OperationalError

def execute_with_retry(cursor, sql, params=None, max_retries=3):
    for attempt in range(max_retries):
        try:
            if params:
                cursor.execute(sql, params)
            else:
                cursor.execute(sql)
            return cursor
        except OperationalError as e:
            if attempt == max_retries - 1:
                raise
            time.sleep(2 ** attempt)  # Exponential backoff
```

### Batch Operations

```go
// Go example
tx, _ := db.Begin()
defer tx.Rollback()

stmt, _ := tx.Prepare("INSERT INTO events (user_id, type) VALUES (?, ?)")
defer stmt.Close()

for i := 0; i < 1000; i++ {
    stmt.Exec(userId, "event_type")
}

tx.Commit()
```

## Performance Tuning

### Connection Pool Sizing

**Go:**
- `MaxOpenConns`: Set to 2-4x the number of worker threads
- `MaxIdleConns`: Set to 1/4 of MaxOpenConns
- Typical: MaxOpenConns=25, MaxIdleConns=5

**Python:**
- Use SQLAlchemy's `pool_size` parameter
- Typical: pool_size=10, max_overflow=20

### Query Optimization

1. **Use LIMIT** to avoid fetching unnecessary rows
2. **Create indexes** on frequently queried columns
3. **Batch inserts** within transactions
4. **Use prepared statements** for repeated queries

### Monitoring

```go
stats := db.Stats()
fmt.Printf("Open: %d, InUse: %d, Idle: %d, WaitCount: %d\n",
    stats.OpenConnections,
    stats.InUse,
    stats.Idle,
    stats.WaitCount,
)
```

## Troubleshooting

### Connection Refused

**Problem:** `connection refused` or `dial tcp: lookup no such host`

**Solution:**
1. Verify capdb-server is running: `ps aux | grep capdb-server`
2. Check listen address: `netstat -tlnp | grep capdb-server`
3. Verify firewall allows connection

### Authentication Failure

**Problem:** `authentication failed` or `invalid token`

**Solutions:**
1. Verify token matches auth file: `cat /path/to/auth.txt`
2. Use `token_file` parameter instead of inline token
3. Check permissions on auth file: `ls -la /path/to/auth.txt`

### Slow Queries

**Solutions:**
1. Add indexes: `CREATE INDEX idx_users_email ON users(email)`
2. Analyze query plan: Use `EXPLAIN`
3. Reduce result set size with LIMIT
4. Increase connection pool size

### Memory Leaks in Rust

**Problem:** Connections not properly closed

**Solution:** Ensure Drop is called:
```rust
{
    let conn = CapDbConnection::open(mode)?;
    // Use connection
} // conn dropped here, resources freed
```

### Timeout Issues in Python

**Problem:** `OperationalError: database is locked`

**Solution:**
```python
# Increase timeout
conn = capdb.connect("file:test.capdb", timeout=30.0)
```

## See Also

- [Building CapDB](../BUILD.md)
- [CapDB Architecture](../docs/ARCHITECTURE.md)
- [Language Drivers Overview](../LANGUAGE_DRIVERS.md)
- [Build Helpers Guide](../BINDINGS_BUILD.md)
