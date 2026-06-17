#!/usr/bin/env bash
# Remove CapDB build products and ephemeral test artifacts.
# Usage: scripts/clean.sh [clean|distclean]
#   clean     — run `make clean` in each build tree, then remove generated
#               sources, lemon debris, and capsuite/nettest temp dirs
#   distclean — delete entire build/, build-*/, and repo-root artifacts
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-clean}"

ARTIFACT_GLOBS=(
  capdb_data_*
  capdb_data_manual_*
  capdb_idle_*
  capdb_pwd_*
  capdb_reuse_*
)

remove_globs() {
  local dir="$1"
  local pat
  shopt -s nullglob
  for pat in "${ARTIFACT_GLOBS[@]}"; do
    rm -rf "${dir}"/${pat}
  done
  shopt -u nullglob
}

clean_one_build_dir() {
  local d="$1"
  if [[ ! -d "$d" ]]; then
    return 0
  fi
  if [[ -f "$d/Makefile" ]]; then
    make -C "$d" clean
  fi
  rm -rf "$d/generated"
  mkdir -p "$d/generated"
  remove_globs "$d"
}

clean_repo_root_artifacts() {
  rm -rf "${ROOT}/Testing"
  rm -f "${ROOT}/compile_commands.json"
  rm -f "${ROOT}/parse.sql" "${ROOT}/fts5parse.sql"
  remove_globs "${ROOT}"
}

if [[ "$MODE" == "distclean" ]]; then
  for d in "${ROOT}"/build "${ROOT}"/build-*; do
    [[ -e "$d" ]] || continue
    rm -rf "$d"
  done
  clean_repo_root_artifacts
  exit 0
fi

if [[ "$MODE" != "clean" ]]; then
  echo "usage: $0 [clean|distclean]" >&2
  exit 1
fi

for d in "${ROOT}"/build "${ROOT}"/build-*; do
  [[ -e "$d" ]] || continue
  clean_one_build_dir "$d"
done

clean_repo_root_artifacts
