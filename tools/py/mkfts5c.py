#!/usr/bin/env python3
"""Port of ext/fts5/tool/mkfts5c.tcl — assemble extensions/fts5 into fts5.c."""
from __future__ import annotations

import argparse
import re
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FTS5_DIR = REPO_ROOT / "extensions" / "fts5"

HEADER = """/*
** This, the "fts5.c" source file, is a composite file that is itself
** assembled from the following files:
**
**    fts5.h
**    fts5Int.h
**    fts5parse.h          <--- Generated from fts5parse.y by Lemon
**    fts5parse.c          <--- Generated from fts5parse.y by Lemon
**    fts5_aux.c
**    fts5_buffer.c
**    fts5_config.c
**    fts5_expr.c
**    fts5_hash.c
**    fts5_index.c
**    fts5_main.c
**    fts5_storage.c
**    fts5_tokenize.c
**    fts5_unicode2.c
**    fts5_varint.c
**    fts5_vocab.c
*/
#if !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS5)

#if !defined(NDEBUG) && !defined(CAPDB_DEBUG)
# define NDEBUG 1
#endif
#if defined(NDEBUG) && defined(CAPDB_DEBUG)
# undef NDEBUG
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
"""

FOOTER = """
/* Here ends the fts5.c composite file. */
#endif /* !defined(CAPDB_CORE) || defined(CAPDB_ENABLE_FTS5) */
"""

SOURCE_FILES = [
    "fts5.h",
    "fts5Int.h",
    "fts5parse.h",
    "fts5parse.c",
    "fts5_aux.c",
    "fts5_buffer.c",
    "fts5_config.c",
    "fts5_expr.c",
    "fts5_hash.c",
    "fts5_index.c",
    "fts5_main.c",
    "fts5_storage.c",
    "fts5_tokenize.c",
    "fts5_unicode2.c",
    "fts5_varint.c",
    "fts5_vocab.c",
]


def fts5_source_id(repo_root: Path) -> str:
    uuid_path = repo_root / "manifest.uuid"
    manifest_path = repo_root / "manifest"
    uuid = uuid_path.read_text(encoding="utf-8").strip() if uuid_path.is_file() else "unknown"
    date = "unknown"
    if manifest_path.is_file():
        for line in manifest_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("D "):
                date = line[2:].replace("T", " ")
                dot = date.rfind(".")
                if dot > 0:
                    date = date[:dot]
                break
    return f"fts5: {date} {uuid}"


def resolve_source(gen_dir: Path, name: str) -> Path:
    if name in ("fts5parse.h", "fts5parse.c"):
        return gen_dir / name
    return FTS5_DIR / name


def print_file(
    path: Path,
    *,
    source_id: str,
    out: list[str],
) -> None:
    data = path.read_text(encoding="utf-8", errors="surrogateescape")
    tail = path.name
    out.append(f'#line 1 "{tail}"')

    sub_map = {"--FTS5-SOURCE-ID--": source_id}
    if tail == "fts5parse.c":
        sub_map.update(
            {
                "yy": "fts5yy",
                "YY": "fts5YY",
                "TOKEN": "FTS5TOKEN",
            }
        )

    fts5_inc = re.compile(r"^#include.*fts5")
    fts5_api = re.compile(r"^(const )?[a-zA-Z][a-zA-Z0-9]* [*]?capdbFts5")

    for line in data.splitlines():
        if fts5_inc.match(line):
            line = f"/* {line} */"
        elif "capdbFts5Init(" not in line and fts5_api.match(line):
            line = f"static {line}"
        for old, new in sub_map.items():
            line = line.replace(old, new)
        out.append(line)


def generate_fts5_c(*, gen_dir: Path, repo_root: Path) -> str:
    sid = fts5_source_id(repo_root)
    out: list[str] = [HEADER]
    for name in SOURCE_FILES:
        print_file(resolve_source(gen_dir, name), source_id=sid, out=out)
    out.append(FOOTER)
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=Path("fts5.c"),
        help="Output path (default: fts5.c)",
    )
    p.add_argument(
        "--gen-dir",
        type=Path,
        default=REPO_ROOT / "build" / "generated",
        help="Directory containing fts5parse.c/h",
    )
    p.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="Repository root",
    )
    args = p.parse_args(argv)
    result = generate_fts5_c(gen_dir=args.gen_dir, repo_root=args.repo_root)
    args.output.write_text(result, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
