# Security Policy

## Supported versions

Security fixes are applied to the **`main`** branch. There are no long-term release branches yet.

| Version   | Supported |
|-----------|-----------|
| `main`    | Yes       |
| Tags      | Best effort at tag time |

## Reporting a vulnerability

**Please do not open public GitHub issues for security vulnerabilities.**

Report security issues privately to the maintainer:

- **Email:** [security reports via GitHub private vulnerability reporting](https://github.com/rickcollette/CapDB/security/advisories/new) (preferred)
- Or open a draft security advisory on the repository if you have maintainer access

Include:

- Affected component (`capdb-server`, replication, pool, client, store, etc.)
- Steps to reproduce
- Impact assessment (confidentiality, integrity, availability)
- Suggested fix, if any

We aim to acknowledge reports within **72 hours** and provide a remediation timeline when possible.

## Security-sensitive deployment notes

- **`--insecure` disables TLS** on the SQL and replication ports. Use only on loopback or isolated lab networks.
- **`--rep-token` is required** when `--rep-listen` is set; replication auth uses constant-time comparison.
- **Path jails** (`--db-root` / `--volume-root`) must point at dedicated directories; symlinks inside the jail are resolved and rejected when they escape the root.
- **Volume mode** denies `ATTACH` / `DETACH` so pooled connections cannot open arbitrary files.
- **Replicas are read-only** at the protocol layer (`EXEC`, `PREPARE`, and `STEP` are gated for writes).

See [docs/CAPDB.md](docs/CAPDB.md) and [capdb/README.md](capdb/README.md) for server hardening details.
