# Releasing CapDB

CapDB is developed inside the [Capper](https://github.com/) tree under `capdb/`
but is published as a standalone repository at
**github.com/rickcollette/capdb** and consumed by downstreams either as the
single-file amalgamation (`capdb.c` + `capdb.h`) or as a vendored subtree.

## Versioning

The version lives in the [`VERSION`](../VERSION) file and is read by CMake
(`project(capdb VERSION …)`) and the codegen. Release tags are `v<VERSION>`
(e.g. `v3.6.1`).

## One-time: create the standalone repo

```bash
scripts/init-standalone-repo.sh      # git init + remote + first commit (no push)
git -C . push -u origin main         # review, then push
```

`CAPDB_REMOTE` overrides the default remote
(`git@github.com:rickcollette/capdb.git`).

## Cutting a release

1. Bump [`VERSION`](../VERSION) and update [`CHANGELOG.md`](../CHANGELOG.md).
2. Verify locally:
   ```bash
   scripts/release.sh        # configure (Release) + build + ctest + package -> dist/
   ```
3. Commit, tag, and push:
   ```bash
   git commit -am "CapDB <version>"
   git tag v<version>
   git push origin main --tags
   ```

Create the GitHub Release from the pushed tag and attach the artifacts generated
by `scripts/release.sh`.

## Release artifacts

`scripts/release.sh` writes to `dist/`:

| Artifact | Contents |
|----------|----------|
| `capdb-<ver>-<os>-<arch>.tar.gz` | binary dist: `capdb` CLI, `capdb-server`, `libcapdb.a`, `libcapdb_client.a`, headers, man pages |
| `capdb-<ver>-src.tar.gz` | full source tree (CMake) |
| `capdb-amalgamation-<ver>.tar.gz` | single-file `capdb.c` + public headers (`capdb.h`, `capdbext.h`, `capdb_client.h`, `capdb_pool.h`) + license/readme |

## CI

[`../.github/workflows/ci.yml`](../.github/workflows/ci.yml) builds and runs `ctest`
on every push/PR (Ubuntu, OpenSSL + zlib).

## Building

```bash
cmake -B build -DCAPDB_ENABLE_POOL=ON -DCAPDB_ENABLE_NETWORK=ON
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure
```

See [BUILD.md](BUILD.md) for the full target list and options.

## Relationship to Capper

Capper vendors CapDB at `capdb/` and links the network client (`libcapdb_client.a`)
from its Go driver (`internal/capdbdriver`, build tag `capdb`). When CapDB changes
land here, rebuild via `make capdb` from the Capper root. Capper is the upstream
working tree; releases are cut from it into the standalone repo.
