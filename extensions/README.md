## CapDB extensions

Fork-specific extensions (built via CMake, not loadable at runtime unless noted):

| Directory | Description | Docs |
|-----------|-------------|------|
| [pool/](pool/) | Connection pool API (`capdb_pool_*`) | [pool/README.md](pool/README.md) |
| [capdb/](capdb/) | TLS network layer (`capdb://`, server, client) | [capdb/README.md](capdb/README.md) |

## Loadable extensions

Various [loadable extensions](https://sqlite.org/loadext.html) for
the core SQL engine are found in subfolders.

Most subfolders are dedicated to a single loadable extension (for
example FTS5, or RTREE).  But the misc/ subfolder contains a collection
of smaller single-file extensions.
