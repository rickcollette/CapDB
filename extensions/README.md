# CapDB Extensions

CapDB keeps upstream extension source trees for reference and future
productization, but only a smaller set is currently first-class in the CMake
build.

## First-Class Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CAPDB_ENABLE_FTS5` | ON | Builds FTS5 into the generated amalgamation. |
| `CAPDB_ENABLE_MATH_FUNCTIONS` | ON | Enables SQL math functions. |
| `CAPDB_ENABLE_PERCENTILE` | ON | Enables the percentile extension. |

The shell also compiles the expert, integrity-check, and recover helper sources
for shell workflows.

## Reference / Not Yet Productized

Directories such as `fts3/`, `rtree/`, `icu/`, `session/`, `rbu/`, `misc/`,
`wasm/`, and `jni/` are retained from upstream or experimental work. They are
not all installed, documented as supported CapDB deliverables, or exercised by
CI. Treat them as reference material unless a CMake option, install rule, and
test explicitly says otherwise.
