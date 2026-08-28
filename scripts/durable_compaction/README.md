# Background BM25 compaction with pg_durable

Glue scripts that move BM25 segment compaction off the write path
and into a [pg_durable](https://github.com/timescale/pg_durable)
durable task.

Nothing here is compiled into the extension. These are operator
scripts; pg_textsearch itself only needs the two GUCs in
[Configuration](#configuration).

## How it works

When `pg_textsearch.compaction_mode = 'background'`, a spill that
would previously have merged segments inline instead records a
request. At `XACT_EVENT_PRE_COMMIT` the extension emits, once per
requesting index, inside a subtransaction:

```sql
SELECT <pg_textsearch.compaction_request_function>(<oid>::oid::regclass)
```

`bm25_request_compaction()` (from `02_wrapper.sql`) turns that into a
pg_durable instance:

```
df.loop(
    'SELECT bm25_compact_step(<oid>::oid::regclass)',   -- body
    'SELECT bm25_needs_compaction(<oid>::oid::regclass)')  -- condition
```

The SECURITY DEFINER wrapper accepts no DSL from the caller. It first
requires the login identity (`session_user`) to have `INSERT` on the
indexed heap or any ancestor returned by `pg_partition_ancestors()`,
then constructs the fixed body and condition using only the numeric
index OID. Re-running `02_wrapper.sql` atomically drops and recreates
the function, transfers ownership, removes every non-owner ACL entry
(including named grants introduced by default privileges), and grants
only the configured writer role.

pg_durable's background worker runs **each node execution on its own
connection, in its own transaction**. So the cascade is split: each
`bm25_compact_step()` merges one batch at the lowest over-threshold
level, commits, drops the per-index `LW_EXCLUSIVE`, and only then is
the next level considered. A writer can commit in between.

The wrapper uses `transaction_mode => 'new'`. pg_durable opens a
loopback session as `textsearch_compactor`, persists the instance in
that session's transaction, and returns its ID to the writer. The
request therefore survives a later failure of the writer transaction.

Requests are accelerators, not durable compaction state. The
GenericXLog-logged level state in the index is the durable truth.
Every `bm25_compact_step()` and `bm25_needs_compaction()` call reads
that state again. An independent request may race ahead of the writer
commit and do nothing, or may become redundant, without affecting
correctness. A later spill requests again, and the scheduled backstop
eventually finds every index whose durable level counts still require
compaction. An explicit writer rollback still queues nothing because
pg_textsearch invokes the wrapper only at `XACT_EVENT_PRE_COMMIT`.

The request is best-effort. It runs in a subtransaction, so if
pg_durable is unavailable the writer gets a `WARNING` and still
commits.

## Requirements

### Both extensions in `shared_preload_libraries`

```
shared_preload_libraries = 'pg_durable,pg_textsearch'
```

Both need it, and it needs a server restart.

### Both extensions in the same database

pg_durable's `df` schema and its metadata tables exist only in the
database named by `pg_durable.database`. The wrapper's loopback
`df.start()` session reconnects to that database, and the BM25 indexes
must live there too. `df.start`'s `database =>` parameter does not
help: it selects where the *nodes* run, not where the instance row is
written.

### Passwordless authenticated connection for the compactor

The worker executes each SQL node on a fresh libpq connection opened
as `df.instances.submitted_by`. pg_durable reads `PGHOST` for both its
management connection and these per-user connections, defaulting to
`127.0.0.1` when it is unset.

For production, map the PostgreSQL server's OS account to the
compactor role with peer authentication. Set `PGHOST` to PostgreSQL's
Unix socket directory in the **PostgreSQL service environment before
starting the server**; setting it only in an interactive shell after
startup does not affect the background worker. Replace
`<socket-directory>` and `<server-os-user>` for your installation:

```
# PostgreSQL service environment
PGHOST=<socket-directory>
```

```
# pg_ident.conf
pg_durable_compactor  <server-os-user>  textsearch_compactor

# pg_hba.conf, above general local rules
local  all  textsearch_compactor  peer map=pg_durable_compactor
```

Restart PostgreSQL after changing its service environment. An HBA-only
reload does not update `PGHOST` in the running server process. The
integration test rejects TCP authentication and uses `trust` only for
local socket connections in its disposable cluster; do not copy that
authentication setup into production.

### The compactor role must have LOGIN and must not be a superuser

The role is called `textsearch_compactor`, without a `pg_` prefix:
PostgreSQL reserves the whole `pg_` role namespace, so
`CREATE ROLE pg_textsearch_compactor` fails with "role name ... is
reserved".

Both restrictions are enforced inside `df.start()`:
`require_login_privilege()` (`src/dsl.rs:873`) rejects a role that
cannot log in, and superuser submission is refused unless
`pg_durable.enable_superuser_instances = on` (`src/dsl.rs:1118`).
`01_setup_role.sql` gets both right. Do not "fix" a permission error
by making the compactor a superuser.

### The background worker initializes asynchronously

After `CREATE EXTENSION pg_durable` — and after every server restart
— the worker takes a moment to connect, write its epoch sentinel and
start the duroxide runtime. Until then `df.start()` cannot hand the
instance to the engine and fails with

```
pg_durable background worker not yet initialized — try again in a moment
```

**Setup scripts and tests must wait and retry.** `03_backstop.sql`
does both: it polls `df._worker_epoch.last_seen_at` for recency, then
retries `df.start()` itself. A readiness probe you can copy:

```sql
SELECT EXISTS (
    SELECT 1 FROM df._worker_epoch
    WHERE last_seen_at > clock_timestamp() - interval '2 minutes');
```

In the release build this failure aborts `df.start()`'s transaction,
so no half-started instance is left behind — but older/test builds
have been seen to leave the instance stuck in `pending` forever. If
you find a `pending` instance that never moves, check the server log
for that message and re-submit.

## Setup

Run in order, in the `pg_durable.database` database.

```sh
# 1. as superuser -- role, grants, membership in the index owner
psql -d postgres -f 01_setup_role.sql -v index_owner=app_owner

# 2. as superuser -- the SECURITY DEFINER request wrapper
psql -d postgres -f 02_wrapper.sql -v writer_role=app_writer

# 3. as the compactor -- the hourly rescue sweep, started once
psql -U textsearch_compactor -d postgres -f 03_backstop.sql
```

`01` and `02` need superuser (or at least membership in
`textsearch_compactor`, since `02` transfers ownership to it).
`03` must run as `textsearch_compactor` itself so the instance is
attributed to that role.

`index_owner` is required, must exist, and must not be a superuser.
The setup grants `textsearch_compactor` inherited membership with
`SET FALSE`; it rejects recursive membership in any superuser role and
any alternate direct or indirect membership path that still permits
`SET ROLE` to the owner. Omitting `-v writer_role=...` grants `EXECUTE`
to nobody; grant it yourself afterwards.

The wrapper schema must not be creatable by untrusted roles. The setup
script rejects `PUBLIC` having `CREATE` on `public`; also revoke it from
any named untrusted roles before installation:

```sql
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
REVOKE CREATE ON SCHEMA public FROM untrusted_role;
```

`02_wrapper.sql` performs replacement and ACL normalization in one
transaction, so a failed owner transfer or grant rolls back to the
previous wrapper without exposing a missing or partially secured
function.

Each writer role granted wrapper `EXECUTE` also needs ordinary
`INSERT` privilege on the indexed heap or one of its partition
ancestors. This permits a writer granted only on a partitioned parent
to authorize the physical leaf index selected by pg_textsearch. The
wrapper checks the login role, not a role selected later with
`SET ROLE`.

## Configuration

```
pg_textsearch.compaction_mode = 'background'
pg_textsearch.compaction_request_function = 'public.bm25_request_compaction'
```

`compaction_request_function` is `PGC_SUSET`, so set it in
`postgresql.conf` (then `SELECT pg_reload_conf();`) or via
`ALTER SYSTEM`. Schema-qualify it: it is resolved with the *writer's*
`search_path` at PRE_COMMIT, which you do not control.

`compaction_mode` values:

| Value        | Behaviour                                        |
|--------------|--------------------------------------------------|
| `inline`     | Merge inside the writing transaction (default).  |
| `background` | Call the request function at PRE_COMMIT.         |
| `off`        | Never compact from the write path at all.        |

Use `off` plus the backstop if you want compaction driven purely on
a schedule.

### Set the compaction threshold where the *worker* can see it

`pg_textsearch.segments_per_level` decides both whether a request is
enqueued and whether the background task actually merges anything --
but the two decisions are made in **two different sessions**:

| Decision                        | Evaluated in                       |
|---------------------------------|------------------------------------|
| enqueue a request at PRE_COMMIT | the **writer's** session           |
| `bm25_needs_compaction()` loop  | the **compactor's** worker session |

The worker connects as `textsearch_compactor` and inherits nothing
from the writer. Setting the threshold only in the writer's session
produces a task that starts, evaluates the condition against the
*default* threshold, finds nothing to do and reports `completed`
while the level counts never change -- a silent no-op that looks
like success:

```sql
-- writer set segments_per_level = 2, worker still sees the default 8
SELECT bm25_level_counts('my_idx');    -- {4,0,0,0,0,0,0,0}
-- ... instance completes ...
SELECT bm25_level_counts('my_idx');    -- {4,0,0,0,0,0,0,0}  (unchanged)
```

Set it at a scope both sessions inherit -- `postgresql.conf`,
`ALTER SYSTEM`, or `ALTER DATABASE ... SET` -- or explicitly on the
compactor role:

```sql
ALTER ROLE textsearch_compactor SET pg_textsearch.segments_per_level = 2;
```

The same applies to any other GUC that `bm25_compact_step()` or
`bm25_needs_compaction()` reads.

## Operating

```sql
-- what is queued / running
SELECT id, label, status, created_at, completed_at
FROM df.instances
WHERE label LIKE 'bm25-compact%'
ORDER BY created_at DESC LIMIT 20;

-- why one failed -- NOTE: n.error is empty in 0.2.6, see
-- "Known limitations"; the message is in the server log
SELECT n.id, n.node_type, n.status, n.status_details, left(n.query, 80)
FROM df.nodes n
WHERE n.instance_id = '<id>';

-- did it help?
SELECT bm25_level_counts('my_idx'::regclass);
SELECT bm25_needs_compaction('my_idx'::regclass);
SELECT * FROM bm25_indexes_needing_compaction();
```

`df.instances` is under RLS keyed on `submitted_by`, so a writer role
cannot see these rows at all. Inspect as `textsearch_compactor` or
as a superuser.

## Known limitations

* **No retry.** pg_durable 0.2.6 has no `max_attempts`, no backoff
  and no `on_failure`; a node that raises kills its instance. This is
  why the two-layer story (next-spill retry plus the hourly backstop)
  exists. Upstream PR #354 would change this.
* **`df.nodes.error` is always empty.** In 0.2.6 a failed node has
  `status = 'failed'` but an empty `error` column, and
  `status_details` carries only `{"execution_id": ...}`. The real
  message is only in the Postgres server log:

  ```
  INFO duroxide::orchestration: Function failed with error:
    SQL execution failed: error returned from database: <message>
    instance_id=<id> execution_id=1
  ```

  Diagnosing a failed compaction therefore requires log access, so
  keep `logging_collector` on.
* **One second minimum per loop iteration.** pg_durable rate-limits
  `df.loop` to `LOOP_MIN_ITER_DURATION = 1 s`, so a stepped cascade
  of *n* levels takes at least *n* seconds. Irrelevant for
  asynchronous compaction, but do not mistake it for the merge cost.
* **Overlapping tasks are possible.** One request is fired per spill
  that needs compaction, and spills are serialized by the per-index
  lock — but nothing stops a second spill from firing a second
  request while the first task is still merging. The tasks do not
  corrupt anything (`bm25_compact_step()` takes the per-index
  `LW_EXCLUSIVE` and re-reads `level_counts` each time, so a
  redundant step is a cheap no-op), but under sustained write load
  you will see several concurrent `bm25-compact-<oid>` instances for
  one index.
* **The backstop sweep is one transaction.** See the long comment at
  the top of `03_backstop.sql`.
* **Compaction still costs the same total I/O.** The work moves off
  the writer, it does not disappear.
