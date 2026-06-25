#!/usr/bin/env python3
"""Structural parity checks for tools/py codegen scripts."""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PY_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = PY_DIR.parents[1]

SCRIPTS = [
    "mkcapdb.py",
    "mkcapdbh.py",
    "mkshellc.py",
    "mkctimec.py",
    "mkopcode.py",
    "mkopcodec.py",
    "mkfts5c.py",
    "vdbe_compress.py",
    "stage_sources.py",
]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestCodegenParity(unittest.TestCase):
    def test_scripts_exist(self):
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.assertTrue((PY_DIR / script).is_file())

    def test_staging_manifest_covers_flist(self):
        mkcapdb = load_module("mkcapdb", PY_DIR / "mkcapdb.py")
        manifest = json.loads((PY_DIR / "staging_manifest.json").read_text())
        for entry in mkcapdb.FLIST:
            with self.subTest(entry=entry):
                self.assertIn(entry, manifest)

    def test_staging_manifest_headers_available_to_mkcapdb(self):
        mkcapdb = load_module("mkcapdb", PY_DIR / "mkcapdb.py")
        manifest = json.loads((PY_DIR / "staging_manifest.json").read_text())
        for hdr in mkcapdb.AVAILABLE_HDR:
            with self.subTest(hdr=hdr):
                self.assertIn(hdr, manifest)

    def test_mkcapdbh_generates_version_markers(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "capdb.h"
            subprocess.run(
                [sys.executable, str(PY_DIR / "mkcapdbh.py"), str(REPO_ROOT), "-o", str(out)],
                check=True,
            )
            text = out.read_text(encoding="utf-8")
            self.assertIn('#define CAPDB_VERSION        "3.6.1"', text)
            self.assertIn("#define CAPDB_VERSION_NUMBER", text)
            self.assertIn("#define CAPDB_SOURCE_ID", text)
            self.assertTrue(text.rstrip().endswith("#endif /* CAPDB3_H */"))

    def test_mkcapdbh_api_decorations(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "capdb.h"
            subprocess.run(
                [sys.executable, str(PY_DIR / "mkcapdbh.py"), str(REPO_ROOT), "-o", str(out)],
                check=True,
            )
            text = out.read_text(encoding="utf-8")
            self.assertRegex(text, r"CAPDB_API\s+int\s+capdb_open\(")

    def test_mkctimec_emits_capdb_symbols(self):
        mkctimec = load_module("mkctimec", PY_DIR / "mkctimec.py")
        text = mkctimec.generate_ctime_c()
        self.assertIn("capdbazCompileOpt", text)
        self.assertIn("capdbCompileOptions", text)
        self.assertIn("CAPDB_ENABLE_NETWORK", text)
        self.assertIn("CAPDB_ENABLE_POOL", text)

    def test_mkopcode_on_core_vdbe(self):
        mkopcode = load_module("mkopcode", PY_DIR / "mkopcode.py")
        parse_h = (REPO_ROOT / "build" / "generated" / "parse.h")
        if not parse_h.is_file():
            parse_h = REPO_ROOT / "core" / "parse.h"
        vdbe_c = REPO_ROOT / "core" / "vdbe.c"
        if not parse_h.is_file():
            self.skipTest("parse.h not generated yet")
        text = mkopcode.generate_opcodes_h(
            parse_h.read_text(encoding="utf-8", errors="surrogateescape")
            + "\n"
            + vdbe_c.read_text(encoding="utf-8", errors="surrogateescape")
        )
        self.assertIn("#define OP_Noop", text)
        self.assertIn("CAPDB_MX_JUMP_OPCODE", text)
        self.assertNotIn("SQLITE_MX_JUMP_OPCODE", text)

    def test_mkopcodec_roundtrip_structure(self):
        mkopcode = load_module("mkopcode", PY_DIR / "mkopcode.py")
        mkopcodec = load_module("mkopcodec", PY_DIR / "mkopcodec.py")
        parse_h = REPO_ROOT / "build" / "generated" / "parse.h"
        vdbe_c = REPO_ROOT / "core" / "vdbe.c"
        if not parse_h.is_file():
            self.skipTest("parse.h not generated yet")
        text_h = mkopcode.generate_opcodes_h(
            parse_h.read_text(encoding="utf-8", errors="surrogateescape")
            + "\n"
            + vdbe_c.read_text(encoding="utf-8", errors="surrogateescape")
        )
        text_c = mkopcodec.generate_opcodes_c(text_h)
        self.assertIn("capdbOpcodeName", text_c)
        self.assertIn("CAPDB_OMIT_EXPLAIN", text_c)

    def test_vdbe_compress_noop_without_small_stack(self):
        vdbe = load_module("vdbe_compress", PY_DIR / "vdbe_compress.py")
        src = (REPO_ROOT / "core" / "vdbe.c").read_text(encoding="utf-8", errors="surrogateescape")
        out = vdbe.compress_vdbe(src, small_stack=False)
        self.assertEqual(out, src)

    def test_vdbe_compress_transforms_with_small_stack(self):
        vdbe = load_module("vdbe_compress", PY_DIR / "vdbe_compress.py")
        src = (REPO_ROOT / "core" / "vdbe.c").read_text(encoding="utf-8", errors="surrogateescape")
        out = vdbe.compress_vdbe(src, small_stack=True)
        self.assertIn("union vdbeExecUnion", out)
        self.assertIn("INSERT STACK UNION HERE", src)
        self.assertNotIn("INSERT STACK UNION HERE", out)

    def test_mkshellc_includes_capdb_shell_sources(self):
        mkshellc = load_module("mkshellc", PY_DIR / "mkshellc.py")
        text = mkshellc.generate_shell_c(repo_root=REPO_ROOT)
        self.assertIn("core/shell.c.in", text)
        self.assertIn("tools/py/mkshellc.py", text)
        self.assertIn("capdb", text)

    def test_mkcapdb_repo_root_default_srcdir(self):
        mkcapdb = load_module("mkcapdb", PY_DIR / "mkcapdb.py")
        self.assertEqual(mkcapdb.REPO_ROOT, REPO_ROOT)
        self.assertEqual(
            mkcapdb.REPO_ROOT / "build" / "generated" / "staging",
            REPO_ROOT / "build" / "generated" / "staging",
        )

    def test_flist_includes_capdb_fork_files(self):
        mkcapdb = load_module("mkcapdb", PY_DIR / "mkcapdb.py")
        fork_files = [
            "capdb_pool.c",
            "capdb_proto.c",
            "capdb_io.c",
            "capdb_tls.c",
            "capdb_client.c",
            "capdb_server.c",
            "capdb_auth.c",
            "capdb_shell.c",
            "capdb_vfs.c",
        ]
        for name in fork_files:
            with self.subTest(name=name):
                self.assertIn(name, mkcapdb.FLIST)


if __name__ == "__main__":
    unittest.main()
