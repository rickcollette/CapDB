#!/usr/bin/env python3
"""Apply CapDB full rebrand: file renames + ordered content replacements.

Usage:
  python3 tool/rebrand/apply_capdb_rename.py --rename   # git mv files/dirs only
  python3 tool/rebrand/apply_capdb_rename.py --content  # text replacements only
  python3 tool/rebrand/apply_capdb_rename.py --apply    # rename + content
  python3 tool/rebrand/apply_capdb_rename.py --audit    # grep audit for stale names
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SKIP_DIRS = {
    ".git",
    "build",
    "__pycache__",
    ".cursor",
    "tool/rebrand/__pycache__",
    "tool/py/__pycache__",
    "ext/wasm",
    "ext/jni",
}

BINARY_SUFFIXES = {
    ".o", ".a", ".so", ".dylib", ".dll", ".exe", ".db", ".zip", ".gz",
    ".png", ".jpg", ".gif", ".ico", ".wasm", ".pc",
}

# (old_path, new_path) relative to ROOT — applied in order; dirs before children.
FILE_RENAMES: list[tuple[str, str]] = [
    # Headers / codegen templates
    ("src/sqlite.h.in", "src/capdb.h.in"),
    ("src/sqliteInt.h", "src/capdbInt.h"),
    ("src/sqliteLimit.h", "src/capdbLimit.h"),
    ("src/sqlite3ext.h", "src/capdbext.h"),
    ("src/sqlite3session.h", "src/capdb_session.h"),
    ("src/sqlite3rbu.h", "src/capdb_rbu.h"),
    ("src/sqlite3recover.h", "src/capdb_recover.h"),
    ("src/sqlite3.h", "src/capdb.h"),
    ("sqlite_cfg.h", "capdb_cfg.h"),
    # Pool
    ("ext/pool/sqlite3pool.c", "ext/pool/capdb_pool.c"),
    ("ext/pool/sqlite3pool.h", "ext/pool/capdb_pool.h"),
    # Network → capdb (files before directory move)
    ("ext/network/msqlite_network.h", "ext/network/capdb_network.h"),
    ("ext/network/msqlite_io.c", "ext/network/capdb_io.c"),
    ("ext/network/msqlite_io.h", "ext/network/capdb_io.h"),
    ("ext/network/proto/msqlite_proto.c", "ext/network/proto/capdb_proto.c"),
    ("ext/network/proto/msqlite_proto.h", "ext/network/proto/capdb_proto.h"),
    ("ext/network/tls/msqlite_tls.c", "ext/network/tls/capdb_tls.c"),
    ("ext/network/tls/msqlite_tls.h", "ext/network/tls/capdb_tls.h"),
    ("ext/network/client/msqlite_client.c", "ext/network/client/capdb_client.c"),
    ("ext/network/client/msqlite_client.h", "ext/network/client/capdb_client.h"),
    ("ext/network/server/msqlite_server.c", "ext/network/server/capdb_server.c"),
    ("ext/network/server/msqlite_server.h", "ext/network/server/capdb_server.h"),
    ("ext/network/server/msqlite_auth.c", "ext/network/server/capdb_auth.c"),
    ("ext/network/shell/msqlite_shell.c", "ext/network/shell/capdb_shell.c"),
    ("ext/network/shell/msqlite_shell.h", "ext/network/shell/capdb_shell.h"),
    ("ext/network/vfs/msqlite_vfs.c", "ext/network/vfs/capdb_vfs.c"),
    ("ext/network/jni/msqlite_jni.c", "ext/network/jni/capdb_jni.c"),
    ("ext/network", "ext/capdb"),
    # Tools / tests
    ("tool/msqlite-server.c", "tool/capdb-server.c"),
    ("tool/py/mksqlite3c.py", "tool/py/mkcapdb.py"),
    ("tool/py/mksqlite3h.py", "tool/py/mkcapdbh.py"),
    ("test/msqlitest.c", "tests/capdb/capdbtest.c"),
    ("test/msuite", "tests/capdb/capsuite"),
    ("test/capsuite/msuite.h", "tests/capdb/capsuite/capsuite.h"),
    ("test/capsuite/msuite_main.c", "tests/capdb/capsuite/capsuite_main.c"),
    ("test/capsuite/msqlite_harness.c", "tests/capdb/capsuite/capdb_harness.c"),
    ("test/capsuite/msqlite_harness.h", "tests/capdb/capsuite/capdb_harness.h"),
    ("test/capsuite/msqlite_loopback.c", "tests/capdb/capsuite/capdb_loopback.c"),
    ("test/capdb_nettest.c", "tests/capdb/capdb_nettest.c"),
    # Amalgamation (regenerated; rename if present)
    ("sqlite3.1", "capdb.1"),
    ("sqlite3.h", "capdb.h"),
    ("sqlite3ext.h", "capdbext.h"),
    ("src/sqlite3session.c", "src/capdb_session.c"),
    ("src/sqlite3rbu.c", "src/capdb_rbu.c"),
    ("src/tclsqlite.c", "src/tclcapdb.c"),
    ("tclsqlite-ex.c", "tclcapdb-ex.c"),
]

# Longest match first. Each (old, new) applied globally in file text.
CONTENT_REPLACEMENTS: list[tuple[str, str]] = [
    ("SQLITE_ENABLE_MSQLITE_NETWORK", "CAPDB_ENABLE_NETWORK"),
    ("SQLITE_ENABLE_CONNECTION_POOL", "CAPDB_ENABLE_POOL"),
    ("MSUITE_ENABLE_MSQLITE", "CAPSUITE_ENABLE_NETWORK"),
    ("MSUITE_MSQLITE_SERVER", "CAPSUITE_SERVER"),
    ("MSUITE_MSQLITEST", "CAPSUITE_CLIENT_TEST"),
    ("MSUITE_", "CAPSUITE_"),
    ("MSQLITE_", "CAPDB_"),
    ("msqlite://", "capdb://"),
    ("msqlitevfs", "capdbvfs"),
    ("libmsqlite_client", "libcapdb_client"),
    ("msqlite-server", "capdb-server"),
    ("msqlite_server", "capdb_server"),
    ("msqlite_client", "capdb_client"),
    ("msqlite_shell", "capdb_shell"),
    ("msqlite_auth", "capdb_auth"),
    ("msqlite_proto", "capdb_proto"),
    ("msqlite_network", "capdb_network"),
    ("msqlite_tls", "capdb_tls"),
    ("msqlite_vfs", "capdb_vfs"),
    ("msqlite_io", "capdb_io"),
    ("msqlite_jni", "capdb_jni"),
    ("msqlitest", "capdbtest"),
    ("msuite", "capsuite"),
    ("msqlite", "capdb"),
    ("sqlite3_cli", "capdb_cli"),
    ("sqlite3pool", "capdb_pool"),
    ("sqlite3_pool", "capdb_pool"),
    ("SQLITE_POOL_", "CAPDB_POOL_"),
    ("sqlite3session", "capdb_session"),
    ("sqlite3changeset", "capdb_changeset"),
    ("sqlite3changegroup", "capdb_changegroup"),
    ("sqlite3rebaser", "capdb_rebaser"),
    ("sqlite3recover", "capdb_recover"),
    ("sqlite3rbu", "capdb_rbu"),
    ("sqlite3ext", "capdbext"),
    ("sqliteInt", "capdbInt"),
    ("sqliteLimit", "capdbLimit"),
    ("sqlite_cfg", "capdb_cfg"),
    ("_HAVE_SQLITE_CONFIG_H", "_HAVE_CAPDB_CONFIG_H"),
    ("BUILD_sqlite", "BUILD_capdb"),
    ("libsqlite3", "libcapdb"),
    ("mksqlite3h", "mkcapdbh"),
    ("mksqlite3c", "mkcapdb"),
    ("project(msqlite3)", "project(capdb)"),
    ("SQLITECONFIG_H", "CAPDBCONFIG_H"),
    ("SQLITE_H", "CAPDB_H"),
    ("sqlite3", "capdb"),
    ("SQLITE_", "CAPDB_"),
]

AUDIT_PATTERNS = [
    r"msqlite",
    r"msuite",
    r"msqlite://",
    r"sqlite3_pool",
    r"sqlite3_cli",
    r"SQLITE_ENABLE_MSQLITE",
    r"SQLITE_ENABLE_CONNECTION_POOL",
    r"libmsqlite",
]

AUDIT_SKIP = {
    "docs/REBRAND.md",
    "CHANGELOG.md",
    "tool/rebrand/apply_capdb_rename.py",
    "README.md",
    "LICENSE.md",
}

# Legacy upstream tool scripts (not part of CapDB product)
AUDIT_SKIP_PREFIXES = (
    "tool/mksourceid",
    "tool/lemon",
    "tool/mkkeywordhash",
    "tool/src-verify",
    "tool/msqlite-server",
    "tool/msqlitest",
    "sqlite3",
    "tsrc/",
)

CONTENT_SKIP = AUDIT_SKIP


def should_skip_path(path: Path) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    for skip in SKIP_DIRS:
        if rel == skip or rel.startswith(skip + "/"):
            return True
    if rel in AUDIT_SKIP:
        return True
    for prefix in AUDIT_SKIP_PREFIXES:
        if rel == prefix or rel.startswith(prefix):
            return True
    if path.suffix.lower() in BINARY_SUFFIXES:
        return True
    return False


def iter_text_files() -> list[Path]:
    out: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or should_skip_path(path):
            continue
        out.append(path)
    return out


def apply_content_replacements(text: str, path: Path) -> str:
    rel = path.relative_to(ROOT).as_posix()
    if rel in CONTENT_SKIP:
        return text
    for old, new in CONTENT_REPLACEMENTS:
        text = text.replace(old, new)
    return text


def git_mv(old: Path, new: Path) -> None:
    new.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(
        ["git", "mv", str(old), str(new)], cwd=ROOT, capture_output=True
    )
    if r.returncode == 0:
        return
    shutil.move(str(old), str(new))


def run_renames() -> None:
    for old_rel, new_rel in FILE_RENAMES:
        old = ROOT / old_rel
        new = ROOT / new_rel
        if not old.exists():
            continue
        new.parent.mkdir(parents=True, exist_ok=True)
        if new.exists():
            print(f"skip rename (dest exists): {old_rel} -> {new_rel}", file=sys.stderr)
            continue
        print(f"rename: {old_rel} -> {new_rel}")
        git_mv(old, new)


def run_content() -> None:
    for path in iter_text_files():
        try:
            raw = path.read_bytes()
        except OSError:
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        new_text = apply_content_replacements(text, path)
        if new_text != text:
            try:
                path.write_text(new_text, encoding="utf-8")
            except OSError as e:
                print(f"skip write {path.relative_to(ROOT)}: {e}", file=sys.stderr)
                continue
            print(f"content: {path.relative_to(ROOT)}")


def run_audit() -> int:
    failures = 0
    for path in iter_text_files():
        if should_skip_path(path):
            continue
        rel = path.relative_to(ROOT).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for pat in AUDIT_PATTERNS:
            if re.search(pat, text, re.IGNORECASE):
                print(f"AUDIT FAIL [{pat}]: {rel}")
                failures += 1
                break
    if failures:
        print(f"\n{failures} file(s) failed audit", file=sys.stderr)
        return 1
    print("Audit passed: no stale product names found.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rename", action="store_true", help="Apply file renames only")
    ap.add_argument("--content", action="store_true", help="Apply content replacements only")
    ap.add_argument("--apply", action="store_true", help="Rename + content")
    ap.add_argument("--audit", action="store_true", help="Grep audit for stale names")
    args = ap.parse_args()

    if args.audit:
        return run_audit()
    if args.apply:
        run_renames()
        run_content()
        return 0
    if args.rename:
        run_renames()
        return 0
    if args.content:
        run_content()
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
