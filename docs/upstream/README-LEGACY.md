# Upstream SQLite — Legacy Notes

CapDB reorganized the upstream SQLite tree. The directories below map to the original layout:

| Original | CapDB |
|----------|-------|
| `src/` | `core/` |
| `ext/` | `extensions/` + `capdb/` |
| `tool/` | `tools/` |
| `test/` (Tcl) | `tests/archive/` (C only; Tcl suite not shipped) |
| `doc/` | `docs/upstream/` |

## Historical build system

Upstream SQLite uses [Fossil](https://fossil-scm.org/) for version control and Tcl/JimTCL for code generation and the `testfixture` test harness. CapDB replaced this with:

- **CMake** for builds
- **Python 3** for codegen (`tools/py/`)
- **capsuite** for regression tests

The amalgamation workflow originally used a flat `tsrc/` staging directory populated by `make target_source`. CapDB now uses `tools/py/stage_sources.py` and `build/generated/staging/` instead.

## Upstream documentation

Internal SQLite design documents live in `docs/upstream/` (formerly `doc/`). They describe the engine architecture and remain accurate for the `core/` tree unless noted otherwise.

For current CapDB build and layout instructions, see [../LAYOUT.md](../LAYOUT.md) and [../BUILD.md](../BUILD.md).
