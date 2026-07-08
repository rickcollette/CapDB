"""Repo-root import shim for the CapDB Python binding.

The C source tree also uses the top-level ``capdb/`` directory name. When
running from the repository root, this shim forwards ``import capdb`` to the
actual Python package under ``bindings/python/capdb``.
"""

from pathlib import Path

_binding_dir = Path(__file__).resolve().parent.parent / "bindings" / "python" / "capdb"
__path__ = [str(_binding_dir)]
exec(compile((_binding_dir / "__init__.py").read_text(), str(_binding_dir / "__init__.py"), "exec"))
