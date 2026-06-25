# CapDB Language Bindings and Build Guide

This guide explains how to build and use CapDB with Go, Rust, Python, and C/C++ applications.

## Prerequisites

- CMake 3.18+
- OpenSSL development headers and libraries
- C compiler (gcc, clang, or MSVC)
- Language-specific tools (Go, Rust, Python as needed)

## Building CapDB

```bash
cd CapDB
mkdir build
cd build
cmake -DCAPDB_ENABLE_NETWORK=ON -DCAPDB_ENABLE_POOL=ON ..
make -j4
```

This produces:
- `build/libcapdb.a` — Core CapDB library
- `build/libcapdb_client.a` — Network client library (for remote access)
- `build/capdb-server` — Network server binary

## Go Language Binding

### Quick Start

```bash
eval $(./capdb-go-build-env.sh ./build)
go build -tags capdb ./cmd/myapp
```

### Manual Setup

If not using the build script, set CGO flags manually:

```bash
export CGO_ENABLED=1
export CGO_CFLAGS="-I$(pwd)/capdb/client -I$(pwd)/build/generated -DCAPDB_ENABLE_NETWORK=1"
export CGO_LDFLAGS="-L$(pwd)/build -Wl,--start-group -lcapdb -lcapdb_store -Wl,--end-group -lssl -lcrypto -lpthread -lm"
export CAPDB_SERVER="$(pwd)/build/capdb-server"
go build -tags capdb ./...
```

### Usage Example

```go
import "database/sql"
import _ "capguard/internal/capdbdriver"

// Network mode
db, _ := sql.Open("capdb", "capdb://localhost:5432/mydb.capdb?token=secret&insecure=1")

// Embedded mode (if configured)
db, _ := sql.Open("capdb-embedded", "file:mydb.capdb")
```

### DSN Parameters

| Parameter | Description | Example |
|-----------|-------------|---------|
| `token` | Authentication token | `token=my-secret-token` |
| `token_file` | Path to file containing token | `token_file=/etc/capdb-token` |
| `password` | Username/password auth | `password=mypass` |
| `insecure` | Disable TLS verification | `insecure=1` |
| `ca` | CA certificate path | `ca=/etc/ssl/certs/ca.pem` |
| `connect_timeout` | Connection timeout in seconds | `connect_timeout=5` |

## Rust Language Binding

### Quick Start

```bash
source ./capdb-rust-build-env.sh ./build
cargo build --features capdb
```

### Setup in Cargo.toml

```toml
[dependencies]
capdb = { path = "../CapDB/bindings/rust", features = ["network"] }
```

### Build Script (build.rs)

```rust
use std::env;
use std::path::PathBuf;

fn main() {
    let capdb_build = env::var("CAPDB_LIB_DIR")
        .unwrap_or_else(|_| "./build".to_string());
    
    println!("cargo:rustc-link-search=native={}", capdb_build);
    println!("cargo:rustc-link-lib=capdb_client");
    println!("cargo:rustc-link-lib=capdb_store");
    println!("cargo:rustc-link-lib=ssl");
    println!("cargo:rustc-link-lib=crypto");
    println!("cargo:rustc-link-lib=pthread");
}
```

## Python Language Binding

### Quick Start

```bash
source ./capdb-python-build-env.sh ./build
python3 -m pip install cffi
python3 setup.py build_ext --inplace
```

### Using ctypes (No Build Required)

```python
import ctypes
import os

# Load the shared library
capdb = ctypes.CDLL(os.path.join(CAPDB_BUILD, "libcapdb_client.so"))

# Define function signatures
capdb.capdb_net_connect.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
capdb.capdb_net_connect.restype = ctypes.c_int

# Use the library
conn = ctypes.c_void_p()
rc = capdb.capdb_net_connect(b"capdb://localhost:5432/db.capdb", conn)
```

## C/C++ Language Binding

### Minimal Example

```c
#include "capdb_client.h"
#include <stdio.h>

int main() {
    capdb_conn *conn = NULL;
    int rc = capdb_net_connect("capdb://localhost:5432/db.capdb?token=secret", &conn);
    if (rc != 0) {
        fprintf(stderr, "Connection failed: %d\n", rc);
        return 1;
    }
    
    // Use conn...
    
    capdb_net_close(conn);
    return 0;
}
```

### Compile with pkg-config (if installed)

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs libcapdb_client)
```

### Manual Compilation

```bash
gcc -o myapp myapp.c \
    -I/path/to/CapDB/capdb/client \
    -I/path/to/CapDB/build/generated \
    -L/path/to/CapDB/build \
    -lcapdb_client -lcapdb_store \
    -lssl -lcrypto -lpthread
```

## Installation

To install libraries and headers for system-wide use:

```bash
cd build
make install  # Installs to /usr/local by default

# Or specify a custom prefix:
cmake -DCMAKE_INSTALL_PREFIX=/opt/capdb ..
make install
```

Files installed:
- `/usr/local/lib/libcapdb.a`
- `/usr/local/lib/libcapdb_client.a`
- `/usr/local/lib/libcapdb_store.a`
- `/usr/local/include/capdb_client.h`
- `/usr/local/share/pkgconfig/libcapdb_client.pc`
- `/usr/local/bin/capdb-server`

## pkg-config Integration

After installation, use pkg-config to get compiler flags:

```bash
pkg-config --cflags capdb_client
pkg-config --libs capdb_client
pkg-config --modversion capdb_client
```

## Testing

Run the CapDB test suite:

```bash
cd build
ctest -V
```

Language-specific integration tests:

```bash
# Go
cd ../bindings/go
make test

# Rust
cd ../bindings/rust
cargo test

# Python
cd ../bindings/python
python -m pytest
```

## Troubleshooting

### "libcapdb_client.a not found"

Build CapDB first:
```bash
cd build && cmake .. && make
```

### "capdb_client.h: No such file or directory"

Ensure `CGO_CFLAGS` or compiler flags include the correct path:
```bash
# Should include: -I/path/to/CapDB/capdb/client
```

### "undefined reference to 'capdb_net_connect'"

Ensure you're linking against `libcapdb_client`:
```bash
# gcc: add -lcapdb_client
# CMake: target_link_libraries(myapp capdb_client)
```

### Connection timeouts or hangs

- Ensure `capdb-server` is running
- Check firewall rules for the listen port (default 5432)
- Use `connect_timeout` parameter to set a timeout
- Check server logs for errors

## Development

### Adding a New Language Binding

1. Create `bindings/<language>/` directory
2. Implement FFI bindings to `capdb_client.h`
3. Add examples and tests
4. Document DSN format and API usage
5. Update this guide

### Updating DSN Parameters

To add a new DSN parameter:

1. Edit `capdb/client/capdb_client.c` (URI parsing)
2. Update language bindings to document it
3. Add conformance tests in each binding
4. Update `BINDINGS_BUILD.md`
