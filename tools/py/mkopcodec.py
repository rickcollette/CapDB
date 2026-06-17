#!/usr/bin/env python3
"""Port of tool/mkopcodec.tcl — generate opcodes.c from opcodes.h."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def generate_opcodes_c(opcodes_h: str) -> str:
    label: dict[int, str] = {}
    synopsis: dict[int, str] = {}
    mx = 0

    define_re = re.compile(r"^#define (OP_\w+)\s+(\d+)")
    syn_re = re.compile(r"synopsis: (.*) \*/")

    for line in opcodes_h.splitlines():
        m = define_re.match(line)
        if not m:
            continue
        name = m.group(1)
        name = re.sub(r"^OP_", "", name)
        i = int(m.group(2))
        label[i] = name
        mx = max(mx, i)
        sm = syn_re.search(line)
        synopsis[i] = sm.group(1).strip() if sm else ""

    out: list[str] = [
        "/* Automatically generated.  Do not edit */",
        "/* See the tools/py/mkopcodec.py script for details. */",
        "#if !defined(CAPDB_OMIT_EXPLAIN) \\",
        " || defined(VDBE_PROFILE) \\",
        " || defined(CAPDB_DEBUG)",
        "#if defined(CAPDB_ENABLE_EXPLAIN_COMMENTS) || defined(CAPDB_DEBUG)",
        '# define OpHelp(X) "\\0" X',
        "#else",
        "# define OpHelp(X)",
        "#endif",
        "const char *capdbOpcodeName(int i){",
        " static const char *const azName[] = {",
    ]
    for i in range(mx + 1):
        nm = label.get(i, "")
        syn = synopsis.get(i, "")
        out.append(f'    /* {i:3d} */ "{nm}" OpHelp("{syn}"),')
    out.extend(
        [
            "  };",
            "  return azName[i];",
            "}",
            "#endif",
        ]
    )
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "opcodes_h",
        nargs="?",
        type=Path,
        help="opcodes.h path (default: stdin)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output file (default: stdout)",
    )
    args = p.parse_args(argv)

    if args.opcodes_h:
        text = args.opcodes_h.read_text(encoding="utf-8", errors="surrogateescape")
    else:
        text = sys.stdin.read()

    result = generate_opcodes_c(text)
    if args.output:
        args.output.write_text(result, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
