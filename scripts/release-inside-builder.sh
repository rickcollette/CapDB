#!/usr/bin/env bash
# Runs inside a Capper release builder image. The source tree is mounted at
# /host-src (read-only). Archives are written to /out.
set -euo pipefail

: "${VERSION:?VERSION is required}"
: "${PLATFORM_SUFFIX:?PLATFORM_SUFFIX is required}"

if [ ! -d /host-src/scripts ]; then
  echo "error: /host-src is not a CapDB checkout" >&2
  exit 1
fi

echo "Builder OS:"
cat /etc/os-release || true
echo "glibc: $(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)"

if command -v apt-get >/dev/null 2>&1; then
  apt-get update
  apt-get install -y --no-install-recommends zlib1g-dev pkg-config default-jdk-headless
  rm -rf /var/lib/apt/lists/*
elif command -v dnf >/dev/null 2>&1; then
  dnf install -y zlib-devel pkgconf-pkg-config
  dnf install -y java-21-openjdk-devel \
    || dnf install -y java-17-openjdk-devel \
    || echo "warning: JDK not available; JNI will be skipped"
  dnf clean all
else
  echo "error: builder has neither apt-get nor dnf" >&2
  exit 1
fi

rm -rf /tmp/capdb-src /tmp/capdb-build
mkdir -p /tmp/capdb-src
tar -C /host-src \
  --exclude .git \
  --exclude build \
  --exclude dist \
  --exclude 'build-*' \
  --exclude compile_commands.json \
  -cf - . | tar -C /tmp/capdb-src -xf -
printf '%s\n' "$VERSION" > /tmp/capdb-src/VERSION

# Keep the process home out of the archives. release.sh rejects an archive
# that contains $HOME.
export HOME=/capdb-release-home
mkdir -p "$HOME"

cd /tmp/capdb-src
scripts/release.sh /tmp/capdb-build --skip-tests

mkdir -p /out
find /out -mindepth 1 -maxdepth 1 -exec rm -rf {} +
cp -a /tmp/capdb-src/dist/. /out/
