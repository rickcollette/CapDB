#!/usr/bin/env python3
"""Port of tool/mkopcodeh.tcl — generate opcodes.h from parse.h + vdbe.c."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


RP2V_OPS = [
    "OP_Transaction",
    "OP_AutoCommit",
    "OP_Savepoint",
    "OP_Checkpoint",
    "OP_Vacuum",
    "OP_JournalMode",
    "OP_VUpdate",
    "OP_VFilter",
    "OP_Init",
]

TAIL_OPS = ["OP_Noop", "OP_Explain", "OP_Abortable"]


def generate_opcodes_h(text: str) -> str:
    tk: dict[str, int] = {}
    op: dict[str, int] = {}
    group: dict[str, int] = {}
    jump: dict[str, int] = {}
    jump0: dict[str, int] = {}
    in1: dict[str, int] = {}
    in2: dict[str, int] = {}
    in3: dict[str, int] = {}
    out2: dict[str, int] = {}
    out3: dict[str, int] = {}
    ncycle: dict[str, int] = {}
    synopsis: dict[str, str] = {}
    paramused: dict[str, int] = {}
    order: list[str] = []
    groups: dict[int, list[str]] = {}
    used: dict[int, str] = {}
    sameas: dict[int, str] = {}
    defn: dict[int, str] = {}

    current_op = ""
    prev_name = ""
    n_group = 0

    for raw in text.splitlines():
        line = raw.rstrip()

        if line.startswith("#define TK_"):
            parts = line.split()
            if len(parts) >= 3:
                tk[parts[1]] = int(parts[2], 0)
            continue

        if re.match(r"^.. Opcode: ", line):
            current_op = "OP_" + line.split()[2]
            m = 0
            for term in line.split():
                if term == "P1":
                    m |= 1
                elif term == "P2":
                    m |= 2
                elif term == "P3":
                    m |= 4
                elif term == "P4":
                    m |= 8
                elif term == "P5":
                    m |= 16
            paramused[current_op] = m
            continue

        sm = re.match(r"^.. Synopsis: (.*)", line)
        if sm and current_op:
            synopsis[current_op] = sm.group(1).strip()
            continue

        if not line.startswith("case OP_"):
            continue

        parts = line.split()
        name = parts[1].rstrip(":")
        if name == "OP_Abortable":
            continue

        op[name] = -1
        group[name] = 0
        jump[name] = 0
        jump0[name] = 0
        in1[name] = 0
        in2[name] = 0
        in3[name] = 0
        out2[name] = 0
        out3[name] = 0
        ncycle[name] = 0

        i = 3
        while i < len(parts) - 1:
            term = parts[i].rstrip(",")
            if term == "same" and i + 2 < len(parts) and parts[i + 1] == "as":
                sym = parts[i + 2].rstrip(",")
                val = tk[sym]
                op[name] = val
                used[val] = name
                sameas[val] = sym
                defn[val] = name
                i += 3
                continue
            if term == "group":
                group[name] = 1
            elif term == "jump":
                jump[name] = 1
            elif term == "in1":
                in1[name] = 1
            elif term == "in2":
                in2[name] = 1
            elif term == "in3":
                in3[name] = 1
            elif term == "out2":
                out2[name] = 1
            elif term == "out3":
                out3[name] = 1
            elif term == "ncycle":
                ncycle[name] = 1
            elif term == "jump0":
                jump[name] = 1
                jump0[name] = 1
            i += 1

        if group[name]:
            new_group = False
            if n_group in groups:
                if not prev_name or not group.get(prev_name):
                    new_group = True
            groups.setdefault(n_group, []).append(name)
            if new_group:
                n_group += 1
        else:
            if prev_name and group.get(prev_name):
                n_group += 1

        order.append(name)
        prev_name = name

    for name in TAIL_OPS:
        jump[name] = 0
        jump0[name] = 0
        in1[name] = 0
        in2[name] = 0
        in3[name] = 0
        out2[name] = 0
        out3[name] = 0
        ncycle[name] = 0
        op[name] = -1
        order.append(name)

    cnt = -1
    for name in order:
        if name in RP2V_OPS:
            cnt += 1
            while cnt in used:
                cnt += 1
            op[name] = cnt
            used[cnt] = name
            defn[cnt] = name
    mx_case1 = cnt

    for name in order:
        if op[name] >= 0 or not jump[name]:
            continue
        cnt += 1
        while cnt in used:
            cnt += 1
        op[name] = cnt
        used[cnt] = name
        defn[cnt] = name

    mx_jump = -1
    for name in order:
        if jump[name] and op[name] > mx_jump:
            mx_jump = op[name]

    for g in range(n_group):
        g_list = groups.get(g, [])
        g_len = len(g_list)
        ok = False
        start = -1
        seek = cnt
        while not ok:
            seek += 1
            while seek in used:
                seek += 1
            ok = True
            start = seek
            for _ in range(g_len - 1):
                seek += 1
                if seek in used:
                    ok = False
                    break
        if not ok:
            raise RuntimeError(f"cannot find opcodes for group: {g_list}")
        nxt = start
        for name in g_list:
            if op[name] >= 0:
                continue
            op[name] = nxt
            used[nxt] = name
            defn[nxt] = name
            nxt += 1

    for name in order:
        if op[name] < 0:
            cnt += 1
            while cnt in used:
                cnt += 1
            op[name] = cnt
            used[cnt] = name
            defn[cnt] = name

    max_val = max(used.keys()) if used else 0
    for i in range(max_val + 1):
        if i not in used:
            defn[i] = f"OP_NotUsed_{i}"
        max_val = max(max_val, i)

    out: list[str] = [
        "/* Automatically generated.  Do not edit */",
        "/* See the tools/py/mkopcode.py script for details */",
    ]

    for i in range(max_val + 1):
        name = defn[i]
        com: list[str] = []
        if jump0.get(name):
            com.append("jump0")
        elif jump.get(name):
            com.append("jump")
        if i in sameas:
            com.append(f"same as {sameas[i]}")
        if name in synopsis:
            com.append(f"synopsis: {synopsis[name]}")
        line = f"#define {name:<16s} {i:3d}"
        if com:
            line += f" /* {', '.join(com):<42s} */"
        out.append(line)

    if max_val > 255:
        raise RuntimeError(
            "More than 255 opcodes - VdbeOp.opcode is of type u8!"
        )

    bv: dict[int, int] = {0: 0}
    for i in range(max_val + 1):
        x = 0
        name = defn[i]
        if not name.startswith("OP_NotUsed"):
            if jump.get(name):
                x |= 1
            if in1.get(name):
                x |= 2
            if in2.get(name):
                x |= 4
            if in3.get(name):
                x |= 8
            if out2.get(name):
                x |= 16
            if out3.get(name):
                x |= 32
            if ncycle.get(name):
                x |= 64
            if jump0.get(name):
                x |= 128
        bv[i] = x

    out.extend(
        [
            "",
            '/* Properties such as "out2" or "jump" that are specified in',
            '** comments following the "case" for each opcode in the vdbe.c',
            "** are encoded into bitvectors as follows:",
            "*/",
            "#define OPFLG_JUMP        0x01  /* jump:  P2 holds jmp target */",
            "#define OPFLG_IN1         0x02  /* in1:   P1 is an input */",
            "#define OPFLG_IN2         0x04  /* in2:   P2 is an input */",
            "#define OPFLG_IN3         0x08  /* in3:   P3 is an input */",
            "#define OPFLG_OUT2        0x10  /* out2:  P2 is an output */",
            "#define OPFLG_OUT3        0x20  /* out3:  P3 is an output */",
            "#define OPFLG_NCYCLE      0x40  /* ncycle:Cycles count against P1 */",
            "#define OPFLG_JUMP0       0x80  /* jump0:  P2 might be zero */",
            "#define OPFLG_INITIALIZER {\\",
        ]
    )
    for i in range(max_val + 1):
        if i % 8 == 0:
            out[-1] += f"\n/* {i:3d} */"
        out[-1] += f" 0x{bv[i]:02x},"
        if i % 8 == 7 or i == max_val:
            out[-1] += "\\"
    out[-1] += "\n}"
    out.extend(
        [
            "",
            "/* The resolve3P2Values() routine is able to run faster if it knows",
            "** the value of the largest JUMP opcode.  The smaller the maximum",
            "** JUMP opcode the better, so the mkopcode.py script that",
            "** generated this include file strives to group all JUMP opcodes",
            "** together near the beginning of the list.",
            "*/",
            f"#define CAPDB_MX_JUMP_OPCODE  {mx_jump}  /* Maximum JUMP opcode */",
        ]
    )
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "inputs",
        nargs="*",
        type=Path,
        help="parse.h and vdbe.c (default: stdin)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output file (default: stdout)",
    )
    args = p.parse_args(argv)

    if args.inputs:
        text = "\n".join(
            f.read_text(encoding="utf-8", errors="surrogateescape")
            for f in args.inputs
        )
    else:
        text = sys.stdin.read()

    result = generate_opcodes_h(text)
    if args.output:
        args.output.write_text(result, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
