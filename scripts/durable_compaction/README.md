# Background BM25 compaction with pg_durable

These operator scripts move eligible BM25 segment merges off the write path
and into [pg_durable](https://github.com/timescale/pg_durable). pg_textsearch
itself has no pg_durable dependency: it calls a configured SQL function, and
the index level counts remain the durable record of outstanding work.

See [`docs/background_compaction.md`](../../docs/background_compaction.md) for
the architecture, transaction and lock boundaries, security model, lifecycle
rules, limitations, and required pg_durable follow-up.

## Requirements

- Preload both libraries and restart PostgreSQL:

  ```conf
  shared_preload_libraries = 'pg_durable,pg_textsearch'
  pg_durable.database = 'application_database'
  ```

- Install both extensions, the wrapper, and the BM25 indexes in
  `pg_durable.database`.
- Set `PGHOST` to the PostgreSQL Unix-socket directory in the PostgreSQL
  service environment before server start.
- Provide a passwordless authenticated mapping for
  `textsearch_compactor`. Production should use peer/ident or an equivalent
  scoped mechanism, not `trust`.
- Revoke `CREATE` on the wrapper schema from `PUBLIC` and every other
  untrusted role.
- Choose a non-superuser role that owns the BM25 indexes. If indexes have
  multiple owners, run `01_setup_role.sql` once for each owner.
- Size `pg_durable.max_new_transaction_starts` for concurrent immediate
  requests and set `pg_durable.new_transaction_start_timeout` to an
  acceptable upper bound on writer wait time.

An example production socket mapping is:

```conf
# PostgreSQL service environment
PGHOST=<socket-directory>
```

```conf
# pg_ident.conf
pg_durable_compactor  <server-os-user>  textsearch_compactor

# pg_hba.conf, before general local rules
local  all  textsearch_compactor  peer map=pg_durable_compactor
```

The integration test and demo use `trust` only inside disposable local
clusters.

## Install

Run the scripts in `pg_durable.database`:

```sh
# As a database superuser: create and grant the compactor role.
psql -d application_database -f 01_setup_role.sql \
  -v index_owner=app_owner

# As a database superuser: install the restricted request wrapper.
psql -d application_database -f 02_wrapper.sql \
  -v writer_role=app_writer

# As the compactor: start the hourly rescue schedule once.
psql -U textsearch_compactor -d application_database \
  -f 03_backstop.sql
```

`index_owner` is mandatory and must be an existing non-superuser. The setup
grants `textsearch_compactor` inherited membership with `SET FALSE` and
rejects unsafe direct or transitive role paths.

The wrapper authorizes `session_user` by `INSERT` on the indexed table or a
partition ancestor. The operator setup grants a writer only wrapper `EXECUTE`;
it does not grant access to `df.*` or the private physical-target helpers.
Public compaction mutators separately enforce index ownership. Re-running
`02_wrapper.sql` transactionally normalizes the wrapper ACL to the newly
configured writer.

Omitting `-v writer_role=...` grants wrapper execution to nobody. Grant
additional approved writers explicitly:

```sql
GRANT EXECUTE ON FUNCTION public.bm25_request_compaction(regclass)
TO another_writer;
```

pg_durable initializes its worker asynchronously after extension creation and
server restart. `03_backstop.sql` waits for its readiness sentinel and retries
the initial `df.start()`. Other automation should also wait before submitting
work.

## Configure pg_textsearch

Use settings visible to both writers and `textsearch_compactor`:

```conf
pg_textsearch.compaction_mode = 'background'
pg_textsearch.compaction_request_function = \
    'public.bm25_request_compaction'
pg_textsearch.segments_per_level = 8
```

All three settings are `PGC_SUSET`. Apply them through `postgresql.conf`,
`ALTER SYSTEM`, `ALTER DATABASE ... SET`, or compatible role settings, then
reload as appropriate. Schema-qualify the request function because the writer
resolves it at PRE_COMMIT or PRE_PREPARE.

The threshold is evaluated twice: the writer decides whether to request work,
and the worker decides whether to keep stepping. A writer-only `SET` can
create a completed task that performs no merge because the worker sees a
different threshold.

`compaction_mode` values are:

| Value | Behavior |
|---|---|
| `inline` | Compact in the spilling backend; this is the default. |
| `background` | Request a durable task only when a non-temporary index reaches the threshold. Temporary indexes and in-progress index builds compact inline. |
| `off` | Disable write-path compaction and requests; the backstop can still repair debt. |

The wrapper uses `transaction_mode => 'new'`. A task submitted from
PRE_COMMIT or PRE_PREPARE is persisted in an independent loopback transaction.
An explicit rollback before those callbacks submits nothing, while its
physical spill remains visible to the backstop.

## Operate

`03_backstop.sql` starts a single long-lived schedule with an hourly default.
Override the cadence when invoking psql:

```sh
psql -U textsearch_compactor -d application_database \
  -f 03_backstop.sql -v cron='*/15 * * * *'
```

Run the setup exactly once per database. It is not idempotent and a second
invocation creates a second schedule.

Inspect orchestration as `textsearch_compactor` or a superuser because
`df.instances` is protected by submitter-based row-level security:

```sql
SELECT id, label, status, created_at, completed_at
FROM df.instances
WHERE label LIKE 'bm25-%'
ORDER BY created_at DESC
LIMIT 20;

SELECT id, node_type, status, status_details, left(query, 80)
FROM df.nodes
WHERE instance_id = '<instance-id>';
```

Always compare orchestration state with physical debt:

```sql
SELECT * FROM bm25_indexes_needing_compaction();
SELECT bm25_level_counts('my_index'::regclass);
SELECT bm25_needs_compaction('my_index'::regclass);
```

An immediate request failure emits a warning and leaves level counts for the
next spill or backstop. Resolve the connection, ownership, privilege, or
resource error and let the next spill resubmit, run
`bm25_compact_pending()` as the compactor, or replace a terminated backstop
schedule.

In pg_durable 0.2.6, `df.nodes.error` is empty; the worker error is in the
PostgreSQL server log. Keep logging available. Also inspect server warnings:
`bm25_compact_pending()` continues after ordinary per-index failures, so a
partially failed sweep can look successful to pg_durable.

## Packaged locations

- Source checkout/archive: `scripts/durable_compaction/`.
- Versioned PostgreSQL binary archive: `durable_compaction/`.
- Debian SQL:
  `$(pg_config --sharedir)/extension/pg_textsearch/durable_compaction/`.
- Debian operator README:
  `/usr/share/doc/pg-textsearch-postgresql-<major>/durable_compaction/`.

The canonical design is included beside these artifacts as
`background_compaction.md` or in the package's parent documentation
directory.
