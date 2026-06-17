#!/usr/bin/env python3
"""Port of tool/mksqlite3c.tcl — amalgamate staged sources into capdb.c."""
from __future__ import annotations

import argparse
import datetime
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
S78 = "*" * 78

FLIST = [
    "capdbInt.h",
    "os_common.h",
    "ctime.c",
    "global.c",
    "status.c",
    "date.c",
    "os.c",
    "fault.c",
    "mem0.c",
    "mem1.c",
    "mem2.c",
    "mem3.c",
    "mem5.c",
    "mutex.c",
    "mutex_noop.c",
    "mutex_unix.c",
    "mutex_w32.c",
    "malloc.c",
    "printf.c",
    "treeview.c",
    "random.c",
    "threads.c",
    "utf.c",
    "util.c",
    "hash.c",
    "opcodes.c",
    "os_kv.c",
    "os_unix.c",
    "os_win.c",
    "memdb.c",
    "bitvec.c",
    "pcache.c",
    "pcache1.c",
    "rowset.c",
    "pager.c",
    "wal.c",
    "btmutex.c",
    "btree.c",
    "backup.c",
    "vdbemem.c",
    "vdbeaux.c",
    "vdbeapi.c",
    "vdbetrace.c",
    "vdbe.c",
    "vdbeblob.c",
    "vdbesort.c",
    "vdbevtab.c",
    "memjournal.c",
    "walker.c",
    "resolve.c",
    "expr.c",
    "alter.c",
    "analyze.c",
    "attach.c",
    "auth.c",
    "build.c",
    "callback.c",
    "delete.c",
    "func.c",
    "fkey.c",
    "insert.c",
    "legacy.c",
    "loadext.c",
    "pragma.c",
    "prepare.c",
    "select.c",
    "table.c",
    "trigger.c",
    "update.c",
    "upsert.c",
    "vacuum.c",
    "vtab.c",
    "wherecode.c",
    "whereexpr.c",
    "where.c",
    "window.c",
    "parse.c",
    "tokenize.c",
    "complete.c",
    "main.c",
    "notify.c",
    "capdb_pool.c",
    "capdb_proto.c",
    "capdb_io.c",
    "capdb_tls.c",
    "capdb_client.c",
    "capdb_server.c",
    "capdb_auth.c",
    "capdb_shell.c",
    "capdb_vfs.c",
    "fts3.c",
    "fts3_aux.c",
    "fts3_expr.c",
    "fts3_hash.c",
    "fts3_porter.c",
    "fts3_tokenizer.c",
    "fts3_tokenizer1.c",
    "fts3_tokenize_vtab.c",
    "fts3_write.c",
    "fts3_snippet.c",
    "fts3_unicode.c",
    "fts3_unicode2.c",
    "json.c",
    "rtree.c",
    "icu.c",
    "fts3_icu.c",
    "capdb_rbu.c",
    "dbstat.c",
    "dbpage.c",
    "carray.c",
    "capdb_session.c",
    "fts5.c",
    "stmt.c",
]

AVAILABLE_HDR = {
    "btree.h": True,
    "btreeInt.h": True,
    "fts3.h": True,
    "fts3Int.h": True,
    "fts3_hash.h": True,
    "fts3_tokenizer.h": True,
    "geopoly.c": True,
    "hash.h": True,
    "hwtime.h": True,
    "keywordhash.h": True,
    "msvc.h": True,
    "mutex.h": True,
    "opcodes.h": True,
    "os_common.h": False,
    "os_setup.h": True,
    "os_win.h": True,
    "os.h": True,
    "pager.h": True,
    "parse.h": True,
    "pcache.h": True,
    "pragma.h": True,
    "capdbrtree.h": True,
    "capdb_session.h": False,
    "capdb.h": True,
    "capdbext.h": True,
    "capdb_rbu.h": True,
    "sqliteicu.h": True,
    "capdbInt.h": False,
    "capdbLimit.h": True,
    "vdbe.h": True,
    "vdbeInt.h": True,
    "vxworks.h": True,
    "wal.h": True,
    "whereInt.h": True,
    "capdb_recover.h": True,
}

VARONLY_HDR = {"capdb.h": True}

CDECL_LIST = {
    "capdb_config",
    "capdb_db_config",
    "capdb_log",
    "capdb_mprintf",
    "capdb_snprintf",
    "capdb_test_control",
    "capdb_vtab_config",
}


def section_comment(out: list[str], text: str) -> None:
    nstar = max(1, 60 - len(text))
    out.append(f"/************** {text} {S78[:nstar]}/")


def comment_include_line(line: str) -> str:
    return "/* " + line.replace("/*", "**").replace("*/", "**") + " */"


def transform_c_line(
    line: str,
    *,
    tail: str,
    add_static: bool,
    linemacros: bool,
    use_apicall: bool,
    is_header: bool,
) -> str:
    if not add_static:
        return line
    if tail in VARONLY_HDR:
        return line

    decl_pat = (
        r"^ *([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb[_a-zA-Z0-9]+)(\(.*)$"
        if is_header
        else r"^([a-zA-Z][a-zA-Z_0-9 ]+ \**)(capdb[_a-zA-Z0-9]+)(\(.*)$"
    )
    var_pat = r"^[a-zA-Z][a-zA-Z_0-9 *]+(capdb[_a-zA-Z0-9]+)(\[|;| =)"

    dm = re.match(decl_pat, line)
    if dm:
        rettype, funcname, rest = dm.group(1), dm.group(2), dm.group(3)
        line = re.sub(r"^CAPDB_API ", "", line)
        rettype = re.sub(r"^CAPDB_API ", "", rettype)
        if re.match(r"^capdb[a-z]*_", funcname):
            out = "CAPDB_API " + rettype.rstrip()
            if not rettype.endswith("*"):
                out += " "
            if use_apicall:
                out += (
                    "CAPDB_CDECL "
                    if funcname in CDECL_LIST
                    else "CAPDB_APICALL "
                )
            out += funcname + rest
            if funcname == "capdb_sourceid":
                return f"/* {out} */"
            return out
        return f"CAPDB_PRIVATE {line}"

    vm = re.match(var_pat, line)
    if vm:
        varname = vm.group(1)
        line = re.sub(r"^CAPDB_API ", "", line)
        if not re.match(r"^capdb_", varname) and not re.match(
            r"^capdbShow[A-Z]", varname
        ):
            line = re.sub(r"^extern ", "", line)
            return f"CAPDB_PRIVATE {line}"
        if "const char capdb_version[];" in line:
            line = "const char capdb_version[] = CAPDB_VERSION;"
        line = re.sub(r"^CAPDB_EXTERN ", "", line)
        return f"CAPDB_API {line}"

    if re.match(r"^(CAPDB_EXTERN )?void \(\*capdbIoTrace\)", line):
        line = re.sub(r"^CAPDB_API ", "", line)
        line = re.sub(r"^CAPDB_EXTERN ", "", line)
        return line
    if re.match(r"^void \(\*capdbOs", line):
        line = re.sub(r"^CAPDB_API ", "", line)
        return f"CAPDB_PRIVATE {line}"

    return line


def copy_file(
    filename: Path,
    *,
    srcdir: Path,
    out: list[str],
    available_hdr: dict[str, bool],
    seen_hdr: dict[str, bool],
    add_static: bool,
    linemacros: bool,
    use_apicall: bool,
) -> None:
    ln = 0
    tail = filename.name
    section_comment(out, f"Begin file {tail}")
    if linemacros:
        out.append(f'#line 1 "{filename}"')

    text = filename.read_text(encoding="utf-8", errors="surrogateescape")
    is_header = filename.suffix == ".h"
    inc_re = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]')

    for raw in text.splitlines():
        line = raw.rstrip()
        ln += 1
        m = inc_re.match(line)
        if m:
            hdr = m.group(1)
            if hdr in available_hdr:
                if available_hdr[hdr]:
                    available_hdr[hdr] = False
                    section_comment(out, f"Include {hdr} in the middle of {tail}")
                    copy_file(
                        srcdir / hdr,
                        srcdir=srcdir,
                        out=out,
                        available_hdr=available_hdr,
                        seen_hdr=seen_hdr,
                        add_static=add_static,
                        linemacros=linemacros,
                        use_apicall=use_apicall,
                    )
                    section_comment(out, f"Continuing where we left off in {tail}")
                    if linemacros:
                        out.append(f'#line {ln + 1} "{filename}"')
                else:
                    out.append(comment_include_line(line))
            elif hdr not in seen_hdr:
                if "amalgamator: dontcache" not in line:
                    seen_hdr[hdr] = True
                out.append(line)
            elif "amalgamator: keep" in line:
                out.append(line)
            else:
                out.append(comment_include_line(line))
        elif line == "#ifdef __cplusplus":
            out.append("#if 0")
        elif not linemacros and line.startswith("#line"):
            continue
        elif add_static and not re.match(
            r"^(static|typedef|CAPDB_PRIVATE)", line
        ):
            out.append(
                transform_c_line(
                    line,
                    tail=tail,
                    add_static=add_static,
                    linemacros=linemacros,
                    use_apicall=use_apicall,
                    is_header=is_header,
                )
            )
        else:
            out.append(line)

    section_comment(out, f"End of {tail}")


def copy_file_verbatim(filename: Path, out: list[str]) -> None:
    tail = filename.name
    section_comment(out, f"Begin EXTRA_SRC file {tail}")
    for raw in filename.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
        out.append(raw.rstrip())
    section_comment(out, f"End of EXTRA_SRC {tail}")


def read_version(srcdir: Path) -> str:
    capdb_h = srcdir / "capdb.h"
    for line in capdb_h.read_text(encoding="utf-8", errors="surrogateescape").splitlines():
        m = re.search(r'#define\s+CAPDB_VERSION\s+"(.*)"', line)
        if m:
            return m.group(1)
    return "?????"


def fossil_provenance(repo_root: Path) -> list[str]:
    lines: list[str] = []
    if os.name == "nt":
        vsrcprog = repo_root / "tools" / "src-verify.exe"
    else:
        vsrcprog = repo_root / "tools" / "src-verify"
        if not vsrcprog.is_file():
            vsrcprog = Path("./src-verify")
    manifest = repo_root / "manifest"
    if vsrcprog.is_file() and os.access(vsrcprog, os.X_OK) and manifest.is_file():
        try:
            res = subprocess.run(
                [str(vsrcprog), "-x", str(repo_root)],
                capture_output=True,
                text=True,
                check=True,
            )
            chunks = [x.strip() for x in res.stdout.splitlines() if x.strip()]
            lines.append(
                "** The content in this amalgamation comes from Fossil check-in"
            )
            if chunks:
                lines.append(f"** {chunks[0][:36]}")
                for extra in chunks[1:]:
                    lines.append(f"**    {extra}")
        except (subprocess.CalledProcessError, OSError):
            pass
    if not lines:
        lines.append("** The origin of the sources used to build this amalgamation")
        lines.append("** is unknown.")
    return lines


def generate_capdb_c(
    *,
    srcdir: Path,
    repo_root: Path,
    add_static: bool = True,
    linemacros: bool = False,
    use_apicall: bool = False,
    enable_recover: bool = False,
    extra_src: list[Path] | None = None,
) -> str:
    version = read_version(srcdir)
    today = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%d %H:%M:%S UTC"
    )

    out: list[str] = [
        "/******************************************************************************",
        "** This file is an amalgamation of many separate C source files from CapDB",
        f"** version {version}.  By combining all the individual C code files into this",
        "** single large file, the entire code can be compiled as a single translation",
        "** unit.  This allows many compilers to do optimizations that would not be",
        "** possible if the files were compiled separately.  Performance improvements",
        "** of 5% or more are commonly seen when CapDB is compiled as a single",
        "** translation unit.",
        "**",
        "** This file is all you need to compile CapDB.  To use CapDB in other",
        '** programs, you need this file and the "capdb.h" header file that defines',
        "** the programming interface to the CapDB library.  (If you do not have",
        '** the "capdb.h" header file at hand, you will find a copy embedded within',
        '** the text of this file.  Search for "Begin file capdb.h" to find the start',
        "** of the embedded capdb.h header file.) Additional code files may be needed",
        "** if you want a wrapper to interface CapDB with your choice of programming",
        '** language. The code for the "capdb" command-line shell is also in a',
        "** separate file. This file contains only code for the core CapDB library.",
        "**",
    ]
    out.extend(fossil_provenance(repo_root))
    out.extend(
        [
            "*/",
            "#ifndef CAPDB_AMALGAMATION",
            "#define CAPDB_CORE 1",
            "#define CAPDB_AMALGAMATION 1",
        ]
    )
    if add_static:
        out.extend(
            [
                "#ifndef CAPDB_PRIVATE",
                "# define CAPDB_PRIVATE static",
                "#endif",
            ]
        )

    parse_c = srcdir / "parse.c"
    if parse_c.is_file():
        if "ifndef CAPDB_ENABLE_UPDATE_DELETE_LIMIT" in parse_c.read_text(
            encoding="utf-8", errors="surrogateescape"
        ):
            out.append("#define CAPDB_UDL_CAPABLE_PARSER 1")

    available_hdr = dict(AVAILABLE_HDR)
    seen_hdr: dict[str, bool] = {}
    flist = list(FLIST)
    if enable_recover:
        flist.extend(["capdb_recover.c", "dbdata.c"])

    for name in flist:
        path = srcdir / name
        if not path.is_file():
            raise FileNotFoundError(f"missing staged source: {path}")
        copy_file(
            path,
            srcdir=srcdir,
            out=out,
            available_hdr=available_hdr,
            seen_hdr=seen_hdr,
            add_static=add_static,
            linemacros=linemacros,
            use_apicall=use_apicall,
        )

    for extra in extra_src or []:
        copy_file_verbatim(extra, out)

    out.extend(
        [
            "/* Amalgamation provides capdb_version (skipped in #ifndef CAPDB_AMALGAMATION blocks). */",
            "CAPDB_API const char capdb_version[] = CAPDB_VERSION;",
            "/* Return the source-id for this library */",
            "CAPDB_API const char *capdb_sourceid(void){ return CAPDB_SOURCE_ID; }",
            "#endif /* CAPDB_AMALGAMATION */",
            "/************************** End of capdb.c ******************************/",
        ]
    )
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""options:
  --nostatic       Do not emit CAPDB_PRIVATE linkage transforms
  --linemacros=0|1 Emit #line directives (default: 0)
  --useapicall     Use CAPDB_APICALL / CAPDB_CDECL
  --enable-recover Include recover extension sources
  --srcdir DIR     Staging directory (default: build/generated/staging)
""",
    )
    p.add_argument("--nostatic", action="store_true")
    p.add_argument("--linemacros", type=int, choices=(0, 1), default=0)
    p.add_argument("--useapicall", action="store_true")
    p.add_argument("--enable-recover", action="store_true")
    p.add_argument(
        "--srcdir",
        type=Path,
        default=REPO_ROOT / "build" / "generated" / "staging",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("capdb.c"),
    )
    p.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
    )
    p.add_argument("extra_src", nargs="*", type=Path)
    args = p.parse_args(argv)

    result = generate_capdb_c(
        srcdir=args.srcdir,
        repo_root=args.repo_root.resolve(),
        add_static=not args.nostatic,
        linemacros=bool(args.linemacros),
        use_apicall=args.useapicall,
        enable_recover=args.enable_recover,
        extra_src=args.extra_src,
    )
    args.output.write_text(result, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
