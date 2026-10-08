#!/usr/bin/env bash
# Build one CapDB release with the Capper release builder for that platform.
#
# usage: scripts/release-platform.sh TARGET VERSION BASE_IMAGE PLATFORM_SUFFIX [CAPPER_DIR]
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
target="${1:?usage: scripts/release-platform.sh TARGET VERSION BASE_IMAGE PLATFORM_SUFFIX [CAPPER_DIR]}"
version="${2:?usage: scripts/release-platform.sh TARGET VERSION BASE_IMAGE PLATFORM_SUFFIX [CAPPER_DIR]}"
base_image="${3:?usage: scripts/release-platform.sh TARGET VERSION BASE_IMAGE PLATFORM_SUFFIX [CAPPER_DIR]}"
platform_suffix="${4:?usage: scripts/release-platform.sh TARGET VERSION BASE_IMAGE PLATFORM_SUFFIX [CAPPER_DIR]}"
capper_dir="${5:-$ROOT/../}"

if [ ! -f "$capper_dir/packaging/Dockerfile.release" ] || [ ! -f "$capper_dir/packaging/install-deps.sh" ]; then
  echo "error: Capper packaging builders not found in $capper_dir" >&2
  exit 1
fi

builder="capper-release-${target}"
out="$ROOT/dist/${target}"
ctx="$(mktemp -d)"
trap 'rm -rf "$ctx"' EXIT
mkdir -p "$ctx/packaging"
cp "$capper_dir/packaging/Dockerfile.release" "$capper_dir/packaging/install-deps.sh" "$ctx/packaging/"

# Registry and package-mirror failures show up as a fast non-zero exit.
# Retry before giving up on the platform.
retry() {
  local attempt=1
  local max=3
  local delay=20
  while true; do
    if "$@"; then
      return 0
    fi
    if [ "$attempt" -ge "$max" ]; then
      echo "error: command failed after ${max} attempts" >&2
      return 1
    fi
    echo ">> attempt ${attempt} failed; retrying in ${delay}s" >&2
    sleep "$delay"
    attempt=$((attempt + 1))
    delay=$((delay * 2))
  done
}

DOCKER_BUILDKIT="${DOCKER_BUILDKIT:-1}" retry docker build \
  --build-arg "BASE_IMAGE=${base_image}" \
  -f "$ctx/packaging/Dockerfile.release" \
  -t "$builder" \
  "$ctx"

rm -rf "$out"
mkdir -p "$out"
retry docker run --rm \
  -e "VERSION=${version}" \
  -e "PLATFORM_SUFFIX=${platform_suffix}" \
  -v "$ROOT:/host-src:ro" \
  -v "$out:/out" \
  --entrypoint /bin/bash \
  "$builder" \
  /host-src/scripts/release-inside-builder.sh

docker run --rm \
  --entrypoint /bin/sh \
  -v "$out:/out" \
  "$builder" \
  -c "chown -R $(id -u):$(id -g) /out"

# The builder ran the audit against its own home. Repeat it against the
# machine that invoked the builder, so a host path cannot ship.
verfile="$(mktemp)"
printf '%s\n' "$version" > "$verfile"
"$ROOT/tools/check-release-artifacts.sh" "$out" "$verfile"
rm -f "$verfile"
echo ">> $target artifacts:"
ls -la "$out"
