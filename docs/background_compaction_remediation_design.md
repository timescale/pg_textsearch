# Background Compaction Remediation Design

## Goal

Resolve the correctness, security, operability, testing, and packaging issues
found in the PR #471 review without expanding the POC into a general
non-blocking compaction redesign.

## Implementation Status

The compaction traversal, request gating, temporary-index handling, read-only
enforcement, cancellation propagation, and prepared-transaction callbacks are
implemented and independently reviewed. The pg_durable role and wrapper
hardening is implemented and under review. Benchmark draining, packaging, and
the final operator-documentation pass remain.

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

## Privilege Model

`01_setup_role.sql` will require an explicit `index_owner`. It will reject
missing roles and roles with `rolsuper`, reject existing superuser
memberships for `textsearch_compactor`, and grant owner membership with
`INHERIT TRUE, SET FALSE`.

The script will discover the extension schema and explicitly grant the
compactor `USAGE` plus `EXECUTE` on the compaction functions required by the
worker and backstop. Operation will no longer depend on default `PUBLIC`
function privileges.

`02_wrapper.sql` will drop and recreate its function so previous named ACLs
cannot survive writer-role rotation. Before submitting work, the
`SECURITY DEFINER` body will resolve the index's heap relation and require the
login caller (`session_user`) to hold `INSERT` on that table. The fixed DSL
and numeric OID construction remain unchanged.

Production documentation will recommend peer/ident authentication and treat
`trust` as test/demo-only.

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
7. A writer cannot request compaction for a table on which it lacks `INSERT`.
8. Role setup rejects omitted and superuser owners, prevents `SET ROLE`, and
   works when `PUBLIC EXECUTE` is revoked.
9. The permission test requires a completed worker task and actual level
   movement.
10. Benchmark draining fails if any matching task remains nonterminal.
11. Package checks assert the setup scripts and documentation are present.

Existing install/upgrade SQL parity, formatting, regression, pg_durable
end-to-end, and package checks remain required.

## Non-Goals

- Removing the per-index exclusive lock from a merge batch.
- Making segment mutation logically undoable.
- Adding a custom WAL resource manager or physical undo log.
- Implementing the separate non-blocking compaction design tracked outside
  PR #471.
