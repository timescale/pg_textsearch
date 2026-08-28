# Background Compaction Remediation Design

## Goal

Resolve the correctness, security, operability, testing, and packaging issues
found in the PR #471 review without expanding the POC into a general
non-blocking compaction redesign.

## Implementation Status

The compaction traversal, request gating, temporary-index handling, read-only
enforcement, cancellation propagation, and prepared-transaction callbacks are
implemented and independently reviewed. The pg_durable role, wrapper, socket
authentication, partition authorization, and backstop candidate hardening are
implemented and undergoing final re-review. Benchmark draining, packaging,
and the final operator-documentation pass remain.

## Request Semantics

Memtable spills and segment merges are physical `GenericXLog` mutations.
PostgreSQL does not undo them when the surrounding SQL transaction aborts.
Background requests therefore follow the physical spill rather than claiming
logical heap-commit atomicity. The index's level counts are the durable marker,
and the backstop is the guarantee path; immediate requests are accelerators.

The pg_durable wrapper will use `transaction_mode => 'new'`. The independent
start:

- survives caller rollback after dispatch has begun, matching the surviving
  physical segment;
- makes the task graph visible before `df.start()` returns, eliminating the
  five-second caller-transaction visibility race;
- remains best-effort: an enqueue failure logs a warning and the backstop
  repairs outstanding compaction later;
- consumes a bounded pg_durable loopback-launch slot, which the operator
  documentation must call out.

The transaction callback will also flush pending requests during
`XACT_EVENT_PRE_PREPARE`, so prepared transactions do not silently discard
physical spill requests. Request state remains deduplicated per index and is
cleared after flushing or transaction cleanup. Savepoint rollback does not
remove a request because it does not undo the corresponding physical spill.
An explicit `ROLLBACK` before PRE_COMMIT dispatches no request; the surviving
level counts remain discoverable by the backstop.

Query cancellation and shutdown errors are not best-effort failures. The
request dispatcher will rethrow those errors after restoring PostgreSQL
subtransaction state and releasing its detached request list. Other enqueue
failures remain warnings.

## Required pg_durable Follow-up

PR #471 uses pg_durable's current primitives but does not make the recurring
backstop production-ready by itself. The following pg_durable work will be
designed in a separate thread:

- **Transient-failure resilience.** A failed backstop execution must not
  permanently terminate its cron schedule. The follow-up must define
  retryable failures, backoff and retry limits, non-overlap behavior, and how
  the next scheduled execution proceeds after retries are exhausted.
- **Monitoring and observability.** Operators need durable visibility into
  the schedule's last start, success, and failure; next expected run; current
  instance and node state; retry count; execution duration and queue lag; and
  the latest error with enough context to diagnose connection, permission,
  and SQL failures.
- **Health and alerting contract.** pg_durable should expose stable queries or
  metrics for stale schedules, stuck nonterminal instances, and repeated
  failures. Immediate per-index tasks must remain correlatable by label and
  target index OID, while the backstop must have a stable schedule identity.
- **Connection diagnostics.** The effective host, port, database, and
  submitted role used by worker connections should be inspectable without
  exposing credentials, so socket/authentication mistakes are distinguishable
  from task failures.
- **Singleton and backpressure semantics.** Repeated spills can submit
  redundant tasks for one index. pg_durable should support a keyed
  single-flight policy, plus per-key and global concurrency limits, so
  duplicate work does not become lock contention or an unbounded queue.
- **Schedule lifecycle.** Backstop registration should be idempotent and
  support inspect, pause, resume, replace, and delete operations. The separate
  design must define missed-run, catch-up, clock-change, and overlapping-tick
  behavior.
- **Per-index failure isolation.** The POC catches failures inside
  `bm25_compact_pending()`, which lets the sweep continue but can make a
  partially failed run look successful to pg_durable. A durable iteration
  primitive should run each index in its own transaction, continue past
  permanent per-index errors, and retain a structured partial-failure result.
- **Crash and failover semantics.** Node leases, abandoned executions, primary
  promotion, and worker restart must not leave work permanently stuck or
  permit multiple schedulers to run the same logical backstop concurrently.
- **Failure classification and operator controls.** Connection loss, resource
  pressure, and serialization failures need different handling from revoked
  privileges or a dropped/reindexed target. Operators need cancel, retry,
  requeue, and force-run controls, with exhausted work retained for diagnosis.
- **Execution environment.** Task connections must receive a predictable set
  of role, database, `search_path`, timeout, and extension GUC settings.
  In particular, the writer and worker must agree on
  `pg_textsearch.segments_per_level`.
- **Structured task metadata and auditability.** Labels alone are insufficient.
  Tasks should record the request kind, database, index OID, original login
  role, effective submitter, and schedule identity so immediate and rescue
  work can be correlated safely.
- **History retention.** Per-spill tasks and recurring executions will grow
  pg_durable metadata continuously. Production use needs documented retention,
  pruning, and capacity behavior that preserves recent failures and audit
  records.
- **Caller-mode handoff.** Although this integration uses
  `transaction_mode => 'new'`, pg_durable should separately replace the
  caller-mode graph-loading visibility timeout with a commit-aware handoff or
  an explicit durable contract.
- **Database scope.** The current worker targets one
  `pg_durable.database`. Deployments with BM25 indexes in multiple databases
  need one independently monitored scheduler per database or future
  multi-database orchestration support.

The exact APIs, retry policy, and storage model are intentionally deferred to
that pg_durable design. Until then, the index level counts remain the durable
record of compaction debt, but operators must monitor the backstop instance
and manually restart a schedule that terminates.

## Compaction Scheduling

After a spill is finalized, background mode will inspect the metapage while
the existing per-index exclusive lock is held. It records a request only when
at least one compactable level is at or above
`pg_textsearch.segments_per_level`.

Temporary indexes cannot be opened by a pg_durable worker in another backend.
They will retain inline compaction even when the cluster-wide mode is
`background`.

Whole-cascade `bm25_compact()` scans every compactable level, merging each
over-threshold level before continuing upward. `bm25_compact_step()` still
merges only the lowest eligible batch. This lets the backstop repair an
interrupted cascade whose L0 is below threshold while L1 or a higher level
still needs work.

Both compaction mutators will call `PreventCommandIfReadOnly()` for permanent
indexes. Local temporary indexes retain PostgreSQL's normal read-only
exception.

The backstop candidate query excludes storage-less partitioned parent indexes
and returns their physical leaf indexes instead. This prevents metapage access
on relations without storage while preserving compaction of every leaf.

## Privilege Model

`01_setup_role.sql` will require an explicit `index_owner`. It will reject
missing roles and roles with `rolsuper`, recursively reject membership in any
superuser role, and grant owner membership with `INHERIT TRUE, SET FALSE`.
After the grant, it will reject any alternate direct or indirect membership
path that still permits `SET ROLE` to the owner.

The script will discover the extension schema and explicitly grant the
compactor `USAGE` plus `EXECUTE` on the compaction functions required by the
worker and backstop. Operation will no longer depend on default `PUBLIC`
function privileges.

`02_wrapper.sql` will replace the function atomically, transfer ownership,
remove every non-owner ACL entry (including named default-privilege grants),
and grant only the configured writer. Before submitting work, the
`SECURITY DEFINER` body will resolve the index's heap relation and require the
login caller (`session_user`) to hold `INSERT` on that table or a partition
ancestor. The fixed DSL and numeric OID construction remain unchanged.

Production documentation will require `PGHOST` to be set to the Unix-socket
directory in PostgreSQL's service environment before recommending peer
authentication. It will treat `trust` as test/demo-only and require untrusted
roles to lack `CREATE` on the wrapper schema.

## Operational Artifacts

The latency benchmark will wait for every nonterminal instance associated
with the measured index, fail on timeout, and verify that no compaction work
remains before starting the next scenario.

Release artifacts will include:

- `scripts/durable_compaction/`;
- the background-compaction operator documentation;
- the demo in source archives;
- setup SQL and documentation at a stable PostgreSQL share path in Debian
  packages and versioned binary archives.

## Test Plan

Tests are added before implementation:

1. A below-threshold spill produces no request; the threshold-crossing spill
   produces exactly one.
2. `bm25_compact()` repairs a higher-level-only backlog.
3. Query cancellation during request dispatch aborts rather than committing.
   The regression self-cancels because PostgreSQL disables
   `statement_timeout` before PRE_COMMIT callbacks.
4. A directly dispatched pg_durable task survives caller rollback, while an
   explicitly rolled-back spill remains discoverable and repairable through
   level counts and the backstop.
5. PREPARE flushes rather than discards a pending request.
6. Background mode compacts temporary indexes inline and creates no durable
   task.
7. A writer cannot request compaction for a table on which it lacks `INSERT`;
   parent-only grants authorize physical indexes on nested partition leaves.
8. Role setup rejects omitted and superuser owners, recursive superuser
   membership, and alternate `SET ROLE` paths.
9. Wrapper replacement is atomic and removes object-specific and named
   default-privilege grants.
10. The permission test requires a completed worker task and actual level
   movement.
11. Benchmark draining fails if any matching task remains nonterminal.
12. Package checks assert the setup scripts and documentation are present.

Existing install/upgrade SQL parity, formatting, regression, pg_durable
end-to-end, and package checks remain required.

## Non-Goals

- Removing the per-index exclusive lock from a merge batch.
- Making segment mutation logically undoable.
- Adding a custom WAL resource manager or physical undo log.
- Implementing the separate non-blocking compaction design tracked outside
  PR #471.
