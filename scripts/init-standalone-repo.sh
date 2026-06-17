#!/usr/bin/env bash
#
# Initialize this capdb/ directory as a standalone git repository targeting
# github.com/rickcollette/capdb, and make the first commit.
#
# This script does NOT push by itself — review the commit, then push manually:
#     git push -u origin main
#
# Usage: scripts/init-standalone-repo.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE="${CAPDB_REMOTE:-git@github.com:rickcollette/capdb.git}"
VERSION="$(tr -d ' \t\n\r' < "$ROOT/VERSION")"

cd "$ROOT"

if [ -d .git ]; then
  echo "error: $ROOT already contains a .git directory" >&2
  exit 1
fi

git init -b main
git add -A
git commit -m "CapDB ${VERSION}: standalone repository

Hard fork of SQLite 3.54.x (public domain) adding a connection pool, a TLS
client/server wire protocol, and a SQL server. CapDB additions are MIT-licensed
(see LICENSE); the SQLite engine remains public domain (see LICENSE.md)."

git remote add origin "$REMOTE"

cat <<EOF

Initialized standalone CapDB repo at: $ROOT
Remote 'origin' -> $REMOTE

Layout: see docs/LAYOUT.md
Build:   cmake -B build && cmake --build build
Tests:   cd build && ctest --output-on-failure

Generated sources (capdb.c, capdb.h) are produced at build time under build/generated/.
Root .gitignore excludes build/, dist/, and *.db scratch files.

Next steps (review, then push):
    git -C "$ROOT" log --stat -1
    git -C "$ROOT" push -u origin main
    git -C "$ROOT" tag v${VERSION} && git -C "$ROOT" push origin v${VERSION}

Pushing the v${VERSION} tag triggers .github/workflows/release.yml, which builds,
tests, and attaches release artifacts.
EOF
