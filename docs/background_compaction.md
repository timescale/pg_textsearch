# Background compaction with pg_durable

Status: proof of concept.

## Scope and goals

By default, a memtable spill and every resulting segment merge run in the
writing backend. Background mode keeps the spill on that path but moves
eligible segment merges to
[pg_durable](https://github.com/timescale/pg_durable). Its goals are to:

- remove a whole compaction cascade from the transaction that triggered it;
- release the per-index lock between merge batches;
- recover missed or failed immediate requests from durable index state; and
- keep pg_textsearch independent of pg_durable through a configurable SQL
  callback.

This POC does not make an individual merge batch non-blocking, make physical
index changes transactional, or embed pg_durable in the extension. The
pg_durable integration is operator-installed SQL under
`scripts/durable_compaction/`.

## Durable state and request flow

Spills, segment merges, level heads, and level counts are physical page
mutations made with `GenericXLog`. For permanent relations, they are
WAL-logged and survive crash recovery. Unlogged indexes are also eligible for
background compaction, but retain PostgreSQL's normal crash-reset semantics
and do not emit these mutations to WAL. In both cases, the metapage level
counts are the authoritative record of compaction debt. A pg_durable instance
is only a best-effort accelerator for work described by those counts.

The flow is:

1. `tp_do_spill()` finalizes the physical L0 segment while holding the
   per-index `LW_EXCLUSIVE` lock.
2. `pg_textsearch.compaction_mode` selects the spill-time action:
   - `inline` runs the whole compaction cascade immediately;
   - `background` checks all compactable level counts and records a request
     only if one has reached `pg_textsearch.segments_per_level`;
   - `off` records no request and performs no automatic compaction.
3. Background mode still compacts temporary indexes inline because another
   backend cannot open them. Index builds also compact inline unless mode is
   `off`, because the new index is not yet visible to a worker.
4. A non-temporary index request is appended to a deduplicated, backend-local
   OID list. No SPI, catalog access, or relation open occurs while the
   per-index lock is held.
5. After the lock has been released,
   `XACT_EVENT_PRE_COMMIT` or `XACT_EVENT_PRE_PREPARE` calls the configured
   request function once per pending index. The callback runs in a sandboxed
   internal subtransaction whose local writes, portals, and deferred triggers
   are always rolled back. Parallel workers, recovery, and autovacuum workers
   do not dispatch requests.
6. The operator wrapper starts a fixed `df.loop` with
   `transaction_mode => 'new'`. The independent loopback transaction persists
   the task before returning to the writer.
7. Each loop body calls the private physical-target form of
   `bm25_compact_step()`. Each condition calls the matching
   `bm25_needs_compaction()` helper. pg_durable runs node executions on
   separate connections and transactions, so the per-index lock is released
   between merge batches.
8. The periodic backstop calls `bm25_compact_pending()` to find any remaining
   debt directly from level counts.

Physical spills and completed merge batches are not undone by SQL rollback.
A request started at PRE_COMMIT or PRE_PREPARE is independent and survives a
later caller failure. An explicit rollback before either callback submits no
request, but the physical level counts still expose the surviving debt to the
next spill and the backstop. Savepoint rollback likewise does not remove a
request for a physical spill that remains in the index.

Because dispatch runs late in PRE_COMMIT or PRE_PREPARE processing, callback
SQL cannot safely add effects to the caller transaction. The dispatcher
therefore discards every local transactional effect after a successful or
failed call. A callback must externalize durable work independently; the
shipped pg_durable wrapper does so with `transaction_mode => 'new'`.

The independent worker may run before the writer finishes committing. It may
therefore observe no work, and multiple spills may create redundant tasks.
Every step re-reads the physical target and level counts, so early or
duplicate work is a safe no-op rather than a correctness dependency.

## Compaction interfaces and boundaries

The extension installs these public interfaces:

| Function | Purpose and boundary |
|---|---|
| `bm25_level_counts(regclass)` | Reads the eight metapage counts under an `AccessShareLock` and shared buffer lock. |
| `bm25_needs_compaction(regclass)` | Tests compactable levels against the current session's threshold. |
| `bm25_compact(regclass)` | Owner-only whole cascade. For a non-temporary index it rejects read-only transactions, opens with `RowExclusiveLock`, and holds the per-index `LW_EXCLUSIVE` lock until every eligible level is below threshold. |
| `bm25_compact_step(regclass)` | Owner-only single batch, normally at the lowest eligible level. If that level's destination is full, the step compacts one batch from the blocking destination first. Uses the same relation and per-index lock boundaries and returns whether it merged a batch. |
| `bm25_indexes_needing_compaction()` | Candidate enumeration: lists valid, ready, non-temporary physical BM25 indexes whose owners the caller can use. Storage-less partitioned parents are excluded. |
| `bm25_compact_pending()` | Enumerates all physical candidates, checks their debt, and runs whole-cascade compaction. Ordinary per-index errors, including ownership drift, become warnings so the sweep can continue; the return value counts successful indexes. |

Whole-cascade compaction visits every compactable level, including higher
levels when a lower one is already below threshold. Both traversal forms
normally select the lowest eligible level; when its destination is full, they
compact the blocking destination first so the lower level can advance later.
A step still merges exactly one `segments_per_level` batch and promotes one
segment upward. The top level is not compactable, so a full top level still
causes promotion into it to fail closed.

Two physical-target helpers,
`bm25_compact_step_if_current(oid, oid, oid, oid)` and
`bm25_needs_compaction_if_current(oid, oid, oid, oid)`, are private worker
interfaces. `PUBLIC` has no `EXECUTE`; the setup script grants them only to
`textsearch_compactor`. They validate the database OID, relation OID,
tablespace OID, relfilenumber, access method, and relation persistence while
holding a relation lock. A missing or replaced target returns `false`.

`public.bm25_request_compaction(regclass)` is operator glue, not an extension
API. It validates the caller and target, captures the physical identity, and
constructs only the two fixed helper calls from numeric OIDs.

## Cancellation and errors

Immediate dispatch is best-effort for ordinary errors. An unset or
unresolvable request function, a pg_durable start failure, or a wrapper error
emits a warning and lets the writer commit. Query cancellation, administrative
shutdown, and crash shutdown are rethrown and abort the writer transaction.
The dispatcher uses nested internal subtransactions and an explicit snapshot
because it runs from the transaction callback after normal portals are gone.
Successful callback SQL is also rolled back locally; independently committed
external work is unaffected.

A worker SQL error fails that pg_durable instance. The current pg_durable POC
does not retry failed nodes. A later spill can submit another accelerator, and
the backstop remains the repair path for an index that receives no more
writes.

Cancellation does not bound a merge batch. The caller holds the per-index
`LW_EXCLUSIVE` lock throughout the merge, and PostgreSQL holds interrupts off
while that lock is held. `statement_timeout` or client cancellation is
observed only after the batch releases the lock. Already applied
`GenericXLog` page mutations remain even when the SQL transaction then aborts.
A writer waiting behind the same lock is subject to the same delayed
cancellation.

`bm25_compact_pending()` catches ordinary errors separately for each index,
warns, and continues. Consequently, a backstop node can appear successful to
pg_durable even when one or more indexes failed. Operators must inspect server
warnings and verify remaining debt, not rely only on instance status.

## Index lifecycle

The target lifecycle avoids sending old work to a new physical index:

- Dropping an index removes its OID from the current backend's
  not-yet-dispatched list. This also applies to indexes removed by
  `DROP TABLE ... CASCADE` and partition-tree drops.
- A drop that commits removes the compaction debt with the physical index.
  Storage-less partitioned parents are never backstop candidates; their
  physical leaf indexes are.
- A drop that rolls back has already removed the pending accelerator, but its
  surviving physical index remains discoverable by the next spill or
  backstop.
- A queued task carries the database OID, relation OID, tablespace OID, and
  relfilenumber captured at submission. Drop/recreate, `REINDEX`, replacement
  by another access method, and OID reuse fail this identity check and
  terminate as a successful obsolete-target no-op.
- A running step holds a PostgreSQL relation lock, so `DROP` or `REINDEX`
  serializes with that step. No relation pointer survives across worker
  transactions.

Current pg_durable cannot cancel all queued tasks by stable target key.
Physical identity validation prevents corruption; target tombstoning and
queue cleanup are follow-up requirements below.

## Security model

The operator setup uses three identities:

- Each BM25 index has an explicit, non-superuser `index_owner`.
- `textsearch_compactor` is a `LOGIN NOSUPERUSER INHERIT` role. It receives
  inherited membership in each configured owner with `SET FALSE`, so worker
  sessions pass the index owner check without being able to `SET ROLE` to the
  owner.
- Application writers receive only `EXECUTE` on
  `public.bm25_request_compaction(regclass)` from the operator setup, not
  access to `df.*` or the private physical-target helpers. The public
  compaction mutators still enforce index ownership.

`01_setup_role.sql` rejects missing or superuser owners, direct or transitive
superuser membership, any inbound member of the compactor role, and any
alternate membership path that lets the compactor set the owner role. It
does not revoke operator-managed memberships. It discovers the extension
schema and grants only the schema and function privileges needed for
immediate and backstop compaction.

The `SECURITY DEFINER` wrapper has a fixed safe `search_path`, rejects
non-BM25 and temporary targets, and authorizes the login identity
(`session_user`) by `INSERT` privilege on the indexed table or a partition
ancestor. Its task DSL contains only fixed function names and numeric target
identity. Reinstallation is transactional, recreates the function, removes
all non-owner ACL entries including default-privilege grants, and grants only
the configured writer role. Untrusted roles must not have `CREATE` on the
wrapper schema.

pg_durable connects as the task submitter without supplying a password.
Production must use a scoped, authenticated passwordless route such as
peer/ident mapping. Set `PGHOST` to the Unix-socket directory in the
PostgreSQL service environment before server start. `trust` is acceptable
only in the disposable test and demo clusters.

## Operations

### Requirements and configuration

Both libraries must be preloaded and the server restarted:

```conf
shared_preload_libraries = 'pg_durable,pg_textsearch'
pg_durable.database = 'application_database'
```

The operator scripts require pg_durable 0.2.6 or newer. Before each script's
first persistent side effect, it verifies the exact pg_durable extension
members, defaults, operator signature, and diagnostic columns that script
uses. An unsupported version or incomplete installation fails with an
upgrade/reinstall hint rather than leaving partial role, wrapper, canary, or
schedule state.

The `df` schema, wrapper, and BM25 indexes must be in that database.
`transaction_mode => 'new'` opens a loopback connection to persist every
immediate task. Size `pg_durable.max_new_transaction_starts` for expected
concurrency and set `pg_durable.new_transaction_start_timeout` to the maximum
time a writer may wait for a slot. Exhaustion or timeout is an ordinary
best-effort dispatch failure: the writer gets a warning and the backstop must
repair the debt.

Configure pg_textsearch at a scope visible to both writer and worker sessions:

| GUC | Required behavior |
|---|---|
| `pg_textsearch.compaction_mode` | `inline` by default; set `background` for immediate tasks or `off` for backstop-only operation. |
| `pg_textsearch.compaction_request_function` | Set to the schema-qualified wrapper name, normally `public.bm25_request_compaction`. |
| `pg_textsearch.segments_per_level` | Must have the same effective value in writer and compactor sessions. |

These pg_textsearch GUCs are `PGC_SUSET`. Use `postgresql.conf`,
`ALTER SYSTEM`, `ALTER DATABASE ... SET`, or a compatible role setting.
Setting the threshold only in a writer session can enqueue a task whose worker
uses a different threshold and completes without merging.

Run the operator scripts in order as described in
[`scripts/durable_compaction/README.md`](../scripts/durable_compaction/README.md).
The pg_durable worker initializes asynchronously after extension creation and
server restart; setup must wait for readiness before starting tasks.

### Backstop and failure handling

`03_backstop.sql` first commits a trivial `SELECT 1` task as
`textsearch_compactor`, waits on that exact instance, and verifies the
terminal node result. This proves the worker can open an execution connection
as the compactor; worker readiness or catalog access alone does not.

After the canary succeeds, the script resolves the extension's exact
`bm25_compact_pending()` member and takes a transaction-scoped advisory lock.
It reuses the canonical pending/running
`df.loop(df.wait_for_schedule(...) ~> bm25_compact_pending())` graph submitted
by `textsearch_compactor`, or creates one replacement if only terminal
instances remain. Its default cron expression is hourly and can be overridden
with `-v cron=...`; rerunning with a different value does not alter an
already-live schedule. The label is observability metadata, not authorization
or a security boundary. Executions within one instance are sequential.

The backstop body is one worker transaction. It calls whole-cascade
compaction for each index, so one index holds its per-index exclusive lock for
its full cascade. Per-index ordinary failures are warnings and the sweep
continues. An uncaught node or connection failure terminates the long-lived
instance because the current pg_durable release has no retry.

Monitor both orchestration state and physical debt:

```sql
SELECT id, label, status, created_at, completed_at
FROM df.instances
WHERE label LIKE 'bm25-%'
ORDER BY created_at DESC;

SELECT id, node_type, status, status_details, result, left(query, 80)
FROM df.nodes
WHERE instance_id = '<instance-id>';

-- Run as a superuser for fleet-wide physical debt.
SELECT candidate.idx,
       bm25_level_counts(candidate.idx) AS level_counts
FROM bm25_indexes_needing_compaction() AS candidate(idx)
WHERE bm25_needs_compaction(candidate.idx);

SELECT bm25_level_counts('my_index'::regclass);
SELECT bm25_needs_compaction('my_index'::regclass);
```

In pg_durable 0.2.6, a failed node's worker diagnostic is persisted in
`df.nodes.result`; `df.nodes.error` is empty. Keep server logging available for
connection and worker context. Monitor `bm25_compact_pending()` warnings
separately for ownership or authorization drift: those per-index errors are
caught so the node result is successful. If an immediate task fails, restore
the underlying permission, connection, or resource condition and either let
the next spill resubmit or run the backstop sweep. If the long-lived backstop
terminates, resolve the cause and rerun `03_backstop.sql` to create its single
replacement.

### Packaged files

Repository and source archives contain:

- `scripts/durable_compaction/{01_setup_role.sql,02_wrapper.sql,03_backstop.sql}`;
- `scripts/durable_compaction/README.md`; and
- `docs/background_compaction.md`.

Versioned PostgreSQL binary archives place the scripts and operator README in
`durable_compaction/` and this document at the archive root. Debian packages
install the SQL scripts under
`$(pg_config --sharedir)/extension/pg_textsearch/durable_compaction/`, the
operator README under
`/usr/share/doc/pg-textsearch-postgresql-<major>/durable_compaction/`, and
this document in the parent package documentation directory.

## Current POC limitations

- One merge batch still holds the per-index `LW_EXCLUSIVE` lock for its full
  CPU and I/O duration. Background mode releases writers between batches, not
  during a batch.
- Persisted level counts remain 16-bit. The extension fails closed at 65,535
  segments in one level; compaction or `REINDEX` is required before another
  segment can be published. Widening the on-disk format is follow-up work.
- The backstop uses whole-cascade compaction per index in one node
  transaction, so its lock window is longer than an immediate stepped task.
- Immediate tasks can overlap for one index. Rechecks make them safe, but
  duplicate work consumes connections and adds lock contention.
- pg_durable 0.2.6 has no node retry or backoff. A transient uncaught failure
  terminates the instance, including the long-lived schedule.
- Backstop per-index errors are swallowed after a warning, so a partially
  failed sweep can appear successful.
- Independent task start adds loopback connection and metadata overhead to a
  threshold-crossing spill. Compaction still performs the same total I/O.
- `df.loop` has a one-second minimum iteration duration, and `df.nodes` rows
  are reused across loop generations, limiting progress diagnostics.
- This setup operates in one configured database and uses one global backstop
  singleton. It does not implement the future per-index metapage schedule
  identity.

## pg_durable follow-up

The integration works as a proof of concept, but one upstream capability is
required to make recurring compaction reliable. The gap below is confirmed in
the pg_durable 0.2.6 development tree.

1. **Failure-resilient recurring execution.** The backstop exists to recover
   debt after an immediate request fails and no later spill resubmits it. One
   transient connection, resource, or SQL failure therefore must not disable
   all future sweeps. Each scheduled tick should have a schedule-aware durable
   outcome, use bounded retry and backoff where appropriate, and record an
   exhausted or skipped run before advancing to the next tick. pg_durable
   currently propagates a failed SQL body through `df.loop`, fails the
   long-lived instance, and runs no later ticks. It exposes generic execution
   history, but has no node retry or schedule-aware failure-continuation
   policy, and a loop terminates after 100,000 iterations.

Schedule identity and lifecycle do not require an upstream pg_durable
facility. A production integration can run one recurring instance per BM25
index, store its pg_durable instance ID in the index metapage, and serialize
create, cancel, and replacement bookkeeping with the index's existing locks.
pg_durable already supports cancellation by instance ID, sequential
iterations within one `df.loop`, global admission limits, persisted workflow
restart, generic execution history, metrics, heartbeat monitoring, and
terminal-instance retention.

Until failure-resilient recurrence exists, level counts remain the recovery
source of truth and operators must rerun `03_backstop.sql` after resolving a
failed recurring compaction instance.

## Verification boundaries

The PostgreSQL 17 and 18 regression suites cover threshold gating,
deduplication, ordinary request failures, cancellation propagation, whole and
stepped traversal, read-only enforcement, temporary-index inline behavior,
pending-request removal on drop, and the public SQL helpers.

`make test-durable` is an opt-in integration suite that requires pg_durable.
It starts a disposable cluster and covers independent request persistence,
explicit rollback plus backstop repair, PREPARE, real low-privilege worker
execution, ACL normalization, partition authorization, stale physical
targets, cascade-drop cleanup, stepped progress, and the scheduled backstop.
It also covers pg_durable capability preflight, a failed worker-connection
canary, singleton registration, failed-node diagnostics, pre-COMMIT physical
L0 sampling, and explicit `PG_CONFIG` propagation. It is not part of
`make test-all`.

The demo is a manual, resource-intensive validation of write-path latency,
physical rollback semantics, ranking stability, repeatable reads, and
concurrent writer stalls. Package workflows verify file presence. These tests
do not validate production authentication integration, high availability,
multi-database orchestration, retry policy, or long-term metadata retention.
