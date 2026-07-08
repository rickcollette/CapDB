# Operating CapDB

This runbook covers the current production-facing CapDB surfaces: `capdb-server`,
`capdb-ctl`, volume storage, replication, and packaged installs.

## Service

A systemd template is provided at
[`packaging/systemd/capdb.service`](../packaging/systemd/capdb.service).
Install it with an environment file such as:

```bash
CAPDB_LISTEN=0.0.0.0:5432
CAPDB_AUTH_FILE=/etc/capdb/tokens
CAPDB_STORAGE=volume
CAPDB_VOLUME_ROOT=/var/lib/capdb/volumes
CAPDB_CERT_FILE=/etc/capdb/server.pem
CAPDB_KEY_FILE=/etc/capdb/server.key
```

For legacy single-node storage, run `capdb-server` with `--storage legacy` and
`--db-root` instead of `--volume-root`; adjust the unit locally because systemd
does not conditionally omit empty arguments.

## Health Checks

For a SQL listener, use a read-only query through the same auth/TLS path clients
use:

```bash
capdb 'capdb://127.0.0.1:5432/health.db?token=TOKEN&ca=/etc/capdb/ca.pem' \
  'SELECT 1;'
```

For a volume, use:

```bash
capdb-ctl health --volume /var/lib/capdb/volumes/app --json
```

Alert when `lag_bytes` grows unexpectedly, when `applied_lsn` stops advancing on
a replica, or when status cannot open the volume.

For Prometheus-style scraping from a local volume path:

```bash
capdb-ctl metrics --volume /var/lib/capdb/volumes/app
```

Audit logs are structured as one line per connection/auth event:

```text
capdb audit event=auth.ok peer=127.0.0.1:50000 detail=user
```

Use `--quiet` only when a supervisor captures equivalent connection and auth
events elsewhere.

## Backup And Restore

Create an exported database file from a volume:

```bash
capdb-ctl backup --volume /var/lib/capdb/volumes/app --out /backup/app.db
```

Validate the backup before relying on it:

```bash
capdb /backup/app.db 'PRAGMA integrity_check;'
CAPDB=capdb tools/capdb-restore-drill.sh /backup/app.db
```

Restore drills should copy the exported database into a fresh volume or legacy
database root and run representative application reads before the backup policy
is considered healthy.

## Replication And Promotion

Primary servers use `--rep-listen` and replicas use `--role replica` with
`--rep-primary`. Promotion is manual:

```bash
capdb-ctl promote --volume /var/lib/capdb/volumes/app --wait 30000
```

`--wait` waits for replayed/applied LSN to catch up before bumping generation.
External fencing is still required: stop or isolate the old primary before
promoting a replica.

To mark a fenced former primary as demoted:

```bash
capdb-ctl demote --volume /var/lib/capdb/volumes/app
```

## Upgrade Guardrails

Before upgrading:

```bash
capdb --version
capdb-ctl status --volume /var/lib/capdb/volumes/app --json
```

Take and validate a backup, upgrade one replica first when available, then
upgrade the primary during a write window. Promotion is intentionally manual and
requires an external fencing action; record the fencing command in the local
service runbook before enabling automated failover.
