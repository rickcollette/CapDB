# Releasing CapDB

CapDB is developed inside the [Capper](https://github.com/) tree under `capdb/`
but is published as a standalone repository at
**github.com/rickcollette/capdb** and consumed by downstreams either as the
single-file amalgamation (`capdb.c` + `capdb.h`) or as a vendored subtree.

## Versioning

The version lives in the [`VERSION`](../VERSION) file and is read by CMake
(`project(capdb VERSION …)`) and the codegen. Release tags are `v<VERSION>`
(e.g. `v3.7.1`).

## One-time: create the standalone repo

```bash
scripts/init-standalone-repo.sh      # git init + remote + first commit (no push)
git -C . push -u origin main         # review, then push
```

`CAPDB_REMOTE` overrides the default remote
(`git@github.com:rickcollette/capdb.git`).

## Cutting a release

1. Bump [`VERSION`](../VERSION), the CMake `project()` version, and [`CHANGELOG.md`](../CHANGELOG.md).
2. Verify one platform locally. This uses the Capper release builder image for that OS:
   ```bash
   scripts/release-platform.sh ubuntu24.04 "$(cat VERSION)" ubuntu:24.04 ubuntu24.04-glibc2.39-x86_64
   ```
3. The Windows embedded archive is cross-compiled on the Ubuntu 24.04 runner (`scripts/release-windows.sh`). It does not use the Capper builder image.
4. Commit, tag, and push. The tag push runs [`.github/workflows/release.yml`](../.github/workflows/release.yml), which builds every platform and publishes the GitHub Release:
   ```bash
   git tag v<version>
   git push origin main --tags
   ```

## Release artifacts

Each Linux archive is built inside the matching Capper release builder (Ubuntu 24.04, Debian 12, RHEL 9, Rocky Linux 10, Ubuntu 18.04). `scripts/release.sh` names the binary archive with that platform suffix and writes a `.sha256` beside every tarball. The Windows zip is produced by `scripts/release-windows.sh` on the Ubuntu 24.04 runner and has its own `.sha256`.

`scripts/release.sh` runs `scripts/sql-release-smoke.sh` after the build, including when `ctest` is skipped. The same `tests/sql/basic.sql` script is executed with `capdb -bail -batch` on an in-memory database and against a local `capdb-server` started with `--insecure`. Both outputs must match `tests/sql/basic.expected`. The Windows job passes `--embedded-only` and runs that embedded half under Wine.

| Artifact | Contents |
|----------|----------|
| `capdb-<ver>-<platform>.tar.gz` | binary dist for one glibc family: `capdb` CLI, `capdb-server`, libraries, headers, man pages |
| `capdb-<ver>-windows-x86_64.zip` | MinGW x86_64 embedded build: `capdb.exe`, `capdb.dll`, `libcapdb.dll.a`, `capdb.h`. Networking, the volume store, and replication are off |
| `capdb-<ver>-src.tar.gz` | full source tree (CMake) |
| `capdb-amalgamation-<ver>.tar.gz` | single-file `capdb.c` + public headers (`capdb.h`, `capdbext.h`, `capdb_client.h`, `capdb_pool.h`) + license/readme |
| `capdb-bindings-<ver>.tar.gz` | Go, Rust, Python, and Java binding source trees plus helper scripts |

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
