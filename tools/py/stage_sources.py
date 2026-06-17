#!/usr/bin/env python3
"""Stage amalgamation sources into a flat directory for mkcapdb.py."""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PY_DIR = Path(__file__).resolve().parent


def load_manifest(path: Path) -> dict[str, str]:
    return json.loads(path.read_text(encoding="utf-8"))


def resolve_path(spec: str, *, repo_root: Path, gen_dir: Path) -> Path:
    if "{gen}" in spec:
        return gen_dir / Path(spec.replace("{gen}/", "").replace("{gen}", ""))
    return repo_root / spec


def stage_sources(
    *,
    repo_root: Path,
    gen_dir: Path,
    staging_dir: Path,
    manifest: dict[str, str],
    cflags: list[str] | None = None,
    small_stack: bool = False,
) -> None:
    staging_dir.mkdir(parents=True, exist_ok=True)
    cflags = list(cflags or [])
    if small_stack:
        cflags.append("-DCAPDB_SMALL_STACK")

    for dest_name, src_spec in manifest.items():
        src = resolve_path(src_spec, repo_root=repo_root, gen_dir=gen_dir)
        dst = staging_dir / dest_name
        if not src.is_file():
            raise FileNotFoundError(f"staging source missing: {src} ({dest_name})")
        if dest_name == "vdbe.c":
            continue
        shutil.copy2(src, dst)

    vdbe_src = resolve_path(manifest["vdbe.c"], repo_root=repo_root, gen_dir=gen_dir)
    vdbe_dst = staging_dir / "vdbe.c"
    compress = PY_DIR / "vdbe_compress.py"
    need_small = small_stack or any(
        "CAPDB_SMALL_STACK" in f or "SQLITE_SMALL_STACK" in f for f in cflags
    )
    cmd = [sys.executable, str(compress)]
    if need_small:
        cmd.append("--small-stack")
    cmd.extend([str(vdbe_src), str(vdbe_dst), *cflags])
    subprocess.run(cmd, check=True)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="Repository root",
    )
    p.add_argument(
        "--gen-dir",
        type=Path,
        default=REPO_ROOT / "build" / "generated",
        help="Generated sources directory ({gen} placeholder)",
    )
    p.add_argument(
        "--staging-dir",
        type=Path,
        default=REPO_ROOT / "build" / "generated" / "staging",
        help="Flat staging output directory",
    )
    p.add_argument(
        "--manifest",
        type=Path,
        default=PY_DIR / "staging_manifest.json",
        help="staging_manifest.json path",
    )
    p.add_argument(
        "--small-stack",
        action="store_true",
        help="Run vdbe_compress with CAPDB_SMALL_STACK",
    )
    p.add_argument(
        "cflags",
        nargs="*",
        help="CFLAGS passed to vdbe_compress (e.g. -DCAPDB_SMALL_STACK)",
    )
    args = p.parse_args(argv)

    manifest = load_manifest(args.manifest)
    stage_sources(
        repo_root=args.repo_root.resolve(),
        gen_dir=args.gen_dir.resolve(),
        staging_dir=args.staging_dir.resolve(),
        manifest=manifest,
        cflags=args.cflags,
        small_stack=args.small_stack,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
