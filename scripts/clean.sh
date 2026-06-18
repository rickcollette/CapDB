#!/usr/bin/env bash
# Remove CapDB build products and ephemeral artifacts listed in .gitignore.
# Usage: scripts/clean.sh [clean|distclean]
#   clean     — sweep gitignored artifacts inside build trees; keep build dirs
#   distclean — delete build/, build-*/, cmake-build-*/, dist/, out/ entirely
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-clean}"

# Mirrors .gitignore — capsuite/nettest temp dirs
TEST_DIR_GLOBS=(
  capdb_data_*
  capdb_data_manual_*
  capdb_idle_*
  capdb_pwd_*
  capdb_reuse_*
)

# CMake / packaging output trees (gitignored at repo root)
BUILD_TREE_GLOBS=(build build-* cmake-build-* dist out)

remove_globs_in() {
  local dir="$1"
  local pat
  shopt -s nullglob
  for pat in "${TEST_DIR_GLOBS[@]}"; do
    rm -rf "${dir}"/${pat}
  done
  shopt -u nullglob
}

remove_python_cache() {
  find "${ROOT}/tools" "${ROOT}/tests" -type d -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true
}

clean_repo_root_artifacts() {
  local f
  rm -rf "${ROOT}/Testing"
  rm -f "${ROOT}/compile_commands.json"
  rm -f "${ROOT}/parse.sql" "${ROOT}/fts5parse.sql"
  rm -f "${ROOT}/parse.out" "${ROOT}/fts5parse.out"
  rm -f "${ROOT}/manifest.tags"
  rm -f "${ROOT}"/*.plan.md
  rm -f "${ROOT}/auth.txt"
  remove_globs_in "${ROOT}"
  remove_python_cache
  # Editor / local-only files (gitignored; safe to delete before publish)
  rm -rf "${ROOT}/.cursor"
}

clean_one_build_dir() {
  local d="$1"
  if [[ ! -d "$d" ]]; then
    return 0
  fi
  if [[ -f "$d/Makefile" ]]; then
    make -C "$d" clean
  fi
  rm -rf "${d}/Testing" "${d}/generated"
  mkdir -p "${d}/generated"
  rm -f "${d}/parse.out" "${d}/fts5parse.out"
  rm -f "${d}/compile_commands.json"
  remove_globs_in "$d"
  # CMake's `make clean` can leave stale objects under CMakeFiles/
  find "$d" \( -name '*.o' -o -name '*.a' -o -name '*.d' -o -name '*.gcno' -o -name '*.gcda' \) \
    -delete 2>/dev/null || true
}

remove_build_trees() {
  local pat d
  shopt -s nullglob
  for pat in "${BUILD_TREE_GLOBS[@]}"; do
    for d in "${ROOT}"/${pat}; do
      [[ -e "$d" ]] || continue
      rm -rf "$d"
    done
  done
  shopt -u nullglob
}

if [[ "$MODE" == "distclean" ]]; then
  remove_build_trees
  clean_repo_root_artifacts
  exit 0
fi

if [[ "$MODE" != "clean" ]]; then
  echo "usage: $0 [clean|distclean]" >&2
  exit 1
fi

shopt -s nullglob
for pat in "${BUILD_TREE_GLOBS[@]}"; do
  for d in "${ROOT}"/${pat}; do
    [[ -d "$d" ]] || continue
    clean_one_build_dir "$d"
  done
done
shopt -u nullglob

clean_repo_root_artifacts
