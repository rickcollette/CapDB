# CapDB compatibility ledger

This ledger records upstream SQLite intake state for CapDB releases. It is
checked by `tools/upstream-sync-check.sh` so upstream-sync reviews have a stable
place to record compatibility evidence.

| Field | Current Entry |
|-------|---------------|
| Upstream SQLite version/source id | SQLite 3.54 lineage, source id tracked by generated `capdb_sourceid()` |
| CapDB version | 3.6.1 |
| Parser/codegen changes | CapDB CMake/Python codegen passed parity checks |
| Public API changes | Public `capdb_*`/`CAPDB_*` surface enforced by `tools/check-api-surface.sh` |
| File-format or WAL behavior changes | CapDB volume store adds sidecar WAL segments without changing core database page format |
| Extension behavior changes | Supported product extensions are CMake-backed FTS5, math, percentile, expert/intck/recover shell helpers |
| Active CapDB tests passed | `ctest --test-dir build --output-on-failure`, warning build, codegen parity, API-surface check |
| Archived upstream coverage | `tools/capdb-test-depth-smoke.sh` runs CapDB-native archive smoke SQL plus fuzz and soak probes |

## Intake Template

| Field | Required |
|-------|----------|
| Upstream SQLite version/source id | |
| CapDB version | |
| Parser/codegen changes | |
| Public API changes | |
| File-format or WAL behavior changes | |
| Extension behavior changes | |
| Active CapDB tests passed | |
| Archived upstream coverage | |
| Follow-up regressions added | |
