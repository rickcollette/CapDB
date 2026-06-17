#!/usr/bin/env python3
"""Port of tool/mksqlite3h.tcl — generate capdb.h from core/capdb.h.in."""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

VAR_PATTERN = re.compile(r"^[a-zA-Z][a-zA-Z_0-9 *]+capdb_[_a-zA-Z0-9]+(\[|;| =)")
DECL_PATTERNS = [
    re.compile(r"^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb_[_a-zA-Z0-9]+)(\(.*)$"),
    re.compile(r"^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb_session_[_a-zA-Z0-9]+)(\(.*)$"),
    re.compile(r"^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb_changeset_[_a-zA-Z0-9]+)(\(.*)$"),
    re.compile(r"^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb_changegroup_[_a-zA-Z0-9]+)(\(.*)$"),
    re.compile(r"^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb_rebaser_[_a-zA-Z0-9]+)(\(.*)$"),
]

CDECL_LIST = {
    "capdb_config",
    "capdb_db_config",
    "capdb_log",
    "capdb_mprintf",
    "capdb_snprintf",
    "capdb_test_control",
    "capdb_vtab_config",
}


def find_mksourceid(repo_root: Path) -> Path:
    for name in ("mksourceid", "mksourceid.exe"):
        for sub in ("build", "tools", "tool"):
            candidate = repo_root / sub / name
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
    src = repo_root / "tools" / "mksourceid.c"
    if not src.is_file():
        src = repo_root / "tool" / "mksourceid.c"
    if not src.is_file():
        raise FileNotFoundError("mksourceid not found; build tools/mksourceid first")
    out = repo_root / "build" / "mksourceid"
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["cc", "-O2", "-o", str(out), str(src)],
        check=True,
    )
    return out


def file_content(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="surrogateescape").strip()


def read_source_id(repo_root: Path) -> str:
    mksourceid = find_mksourceid(repo_root)
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        manifest = repo_root / "manifest"
        if manifest.is_file():
            subprocess.run(
                [str(mksourceid), "manifest"],
                cwd=repo_root,
                stdout=tmp_path.open("w"),
                check=True,
            )
            return file_content(tmp_path)
        return "unknown"
    finally:
        tmp_path.unlink(missing_ok=True)


def scm_metadata(repo_root: Path, source_id: str) -> tuple[str, str, str, str]:
    manifest = repo_root / "manifest"
    tags_path = repo_root / "manifest.tags"
    z_ver_time = "unknown"
    if manifest.is_file():
        lines = manifest.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            if line.startswith("D "):
                z_ver_time = line[2:].rstrip("Z") + "Z"
                break
    if not tags_path.is_file():
        print(
            "WARNING: building capdb.h without manifest.tags.\n"
            "  fossil set manifest urt",
            file=sys.stderr,
        )
        sid = source_id[:-13] + "-experimental" if len(source_id) > 13 else source_id
        return sid, "unknown", "unknown", z_ver_time

    content = tags_path.read_text(encoding="utf-8").split()
    z_branch = content[1] if len(content) > 1 else "unknown"
    z_tags = " ".join(t for t in content[2:] if t != z_branch)
    return source_id, z_branch, z_tags, z_ver_time


def transform_line(
    line: str,
    *,
    z_version: str,
    n_version: str,
    z_source_id: str,
    z_branch: str,
    z_tags: str,
    z_ver_time: str,
    use_apicall: bool,
) -> str:
    line = (
        line.replace("--VERS--", z_version)
        .replace("--VERSION-NUMBER--", n_version)
        .replace("--SOURCE-ID--", z_source_id)
        .replace("--SCM-BRANCH--", z_branch)
        .replace("--SCM-TAGS--", z_tags)
        .replace("--SCM-DATETIME--", z_ver_time)
    )

    if VAR_PATTERN.match(line) and not line.lstrip().startswith("typedef"):
        return f"CAPDB_API {line}"

    for pat in DECL_PATTERNS:
        m = pat.match(line)
        if m:
            rettype, funcname, rest = m.group(1), m.group(2), m.group(3)
            out = "CAPDB_API " + rettype.rstrip()
            if not rettype.endswith("*"):
                out += " "
            if use_apicall:
                conv = (
                    "CAPDB_CDECL "
                    if funcname in CDECL_LIST
                    else "CAPDB_APICALL "
                )
                out += conv
            return out + funcname + rest

    if use_apicall:
        line = line.replace(
            "(*capdb_syscall_ptr)",
            "(CAPDB_SYSAPI *capdb_syscall_ptr)",
        )
        line = re.sub(r"\(\*", "(CAPDB_CALLBACK *", line)
    return line


def generate_capdb_h(
    repo_root: Path,
    *,
    enable_recover: bool = False,
    use_apicall: bool = False,
) -> str:
    version = file_content(repo_root / "VERSION")
    parts = version.split(".")
    n_version = "".join(f"{int(p):03d}" for p in parts)

    source_id = read_source_id(repo_root)
    z_source_id, z_branch, z_tags, z_ver_time = scm_metadata(repo_root, source_id)

    filelist = [
        repo_root / "core" / "capdb.h.in",
        repo_root / "extensions" / "rtree" / "capdbrtree.h",
        repo_root / "extensions" / "session" / "capdb_session.h",
        repo_root / "extensions" / "fts5" / "fts5.h",
    ]
    if enable_recover:
        filelist.append(repo_root / "extensions" / "recover" / "capdb_recover.h")

    out: list[str] = []
    for path in filelist:
        is_main = path.name == "capdb.h.in"
        if not is_main:
            out.append(f"/******** Begin file {path.name} *********/")
        for raw in path.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
            line = raw.rstrip()
            if re.search(r'#include\s*[<"]capdb\.h[>"]', line):
                continue
            line = transform_line(
                line,
                z_version=version,
                n_version=n_version,
                z_source_id=z_source_id,
                z_branch=z_branch,
                z_tags=z_tags,
                z_ver_time=z_ver_time,
                use_apicall=use_apicall,
            )
            out.append(line)
        if not is_main:
            out.append(f"/******** End of {path.name} *********/")
    out.append("#endif /* CAPDB3_H */")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "top",
        nargs="?",
        type=Path,
        default=REPO_ROOT,
        help="Repository root",
    )
    p.add_argument("-o", "--output", type=Path, help="Output file (default: stdout)")
    p.add_argument("--enable-recover", action="store_true")
    p.add_argument("--useapicall", action="store_true")
    args = p.parse_args(argv)

    result = generate_capdb_h(
        args.top.resolve(),
        enable_recover=args.enable_recover,
        use_apicall=args.useapicall,
    )
    if args.output:
        args.output.write_text(result, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
