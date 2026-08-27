# Non-blocking segment compaction

Status: draft design for work stream 2

This document is intentionally separate from the pg_durable background
compaction proof of concept in PR #471. PR #471 changes when compaction runs.
This work changes how a merge coordinates with concurrent readers, writers,
spills, VACUUM, and page reclaim. The concurrency design must be correct and
useful without pg_durable.

Implementation starts after PR #471 lands and this branch is rebased onto the
updated `main`.

## Summary

Today a segment merge holds the per-index LWLock in `LW_EXCLUSIVE` mode from
source selection through output construction and publication. A large merge
therefore blocks scans, inserts, and spills for the full merge duration.
Background execution moves that wait to another backend but does not remove
it.

The proposed design treats segment replacement as an RCU-style operation:

1. Select an exact run of source segments while briefly holding the existing
   per-index lock in `LW_SHARED` mode.
2. Build an unreachable output segment without holding the per-index lock.
3. Publish the output with one WAL-logged pointer swap while again briefly
   holding `LW_SHARED`.
4. Keep the displaced source pages in the existing `pending_free_head`
   tombstone chain until the standby-safe reclaim horizon passes.

Normal scans and inserts also use `LW_SHARED`, so they continue during source
selection and publication as well as during the long build phase. Spill and
physical reclaim use `LW_EXCLUSIVE`; they wait only for the two short shared
sections.

PostgreSQL's heavyweight `ShareUpdateExclusiveLock` on the index relation acts
as the maintenance gate. Merge and VACUUM both take it, so a source segment's
alive bitset cannot change during a merge. The lock is compatible with the
relation locks used by ordinary reads and writes, is interruptible, and is
automatically cleaned up after an error or backend exit.

## Goals

- Remove merge-duration stalls from normal index scans and inserts.
- Permit memtable writes, and therefore most writer work, throughout a merge.
- Permit a spill to publish a new L0 segment while a merge builds.
- Prevent VACUUM from changing source alive bitsets during a merge.
- Preserve stock PostgreSQL physical replication through `GenericXLog`.
- Preserve the standby-safe deferred reclaim protocol from issue #380.
- Make merge cancellation effective during the build phase.
- Establish deterministic tests for the known merge, spill, VACUUM, scan, and
  reclaim races before optimizing them.

## Non-goals

- Moving compaction into the background. PR #471 owns that work.
- Making spill itself non-blocking. Spill remains a short `LW_EXCLUSIVE`
  operation in this work stream.
- Running more than one merge concurrently for the same index. Different
  indexes may merge concurrently.
- Replacing the existing segment or metapage format.
- Eliminating the existing crash-orphan behavior for pages written before
  publication. A durable scratch-page manifest is a later hardening option.
- Removing the `hot_standby_feedback = on` requirement for hot standbys that
  serve queries.

## Current concurrency model

The index has one per-index LWLock in `TpSharedIndexState`.

| Operation | Mode | Relevant lifetime |
|---|---|---|
| normal insert | `LW_SHARED` | append to the on-disk memtable |
| index scan | `LW_SHARED` | metapage snapshot and scoring |
| spill | `LW_EXCLUSIVE` | read/reset memtable and link an L0 segment |
| merge | `LW_EXCLUSIVE` | select, build, publish, and park sources |
| tombstone drain | `LW_EXCLUSIVE` | unlink and recycle one tombstone batch |
| VACUUM bulk delete | `LW_SHARED` | identify and mutate/replace segments |
| force merge/truncate | `LW_EXCLUSIVE` | whole administrative operation |

The segment payload is immutable after publication, but a segment is not
literally immutable:

- VACUUM clears bits in the per-segment alive bitset and updates
  `alive_count`.
- Chain publication can update a root segment's `next_segment`.
- VACUUM can replace or unlink a segment.

The design must therefore exclude those mutations while an output is derived
from source segments. It cannot rely on "segments are immutable" without
stating the exceptions.

VACUUM already demonstrates the safe publication pattern this design extends.
It can replace a segment while scans hold the same shared index lock because
the old pages are parked rather than immediately returned to the FSM. New
scans read the new metapage linkage; scans that already captured the old
linkage keep valid pages until they release the shared lock. On a standby,
the transaction horizon stored in the tombstone and hot-standby feedback
provide the corresponding protection.

## Correctness invariants

The implementation must preserve all of these invariants:

1. Every published segment is reachable from exactly one
   `metapage.level_heads[]` chain.
2. A segment under construction is unreachable from the metapage.
3. A selected source run remains linked until the output and deferred-free
   chain are published atomically.
4. Source payload, alive bits, `alive_count`, and chain links do not change
   while the output is built. A concurrent spill may only prepend new L0
   segments ahead of the selected run.
5. Publication never overwrites or drops an L0 prefix created by concurrent
   spills.
6. A displaced page is linked into `pending_free_head` in the same
   `GenericXLog` record that removes its source segment from the level chain.
7. A page is returned to the FSM only after it is unreachable and its
   reclaim horizon precedes `GetOldestNonRemovableTransactionId`.
8. `metapage.total_docs` remains the sum of published segment `num_docs`;
   `total_len` receives the corresponding dead-document shrinkage.
9. No per-index LWLock is held during output construction.
10. All relation-page mutations remain WAL-logged with `GenericXLog`; replay
    never requires loading `pg_textsearch.so`.

## Maintenance coordination

### Alternatives considered

**A second per-index LWLock** would separate maintenance from the existing
index lock, but holding any LWLock across output construction keeps
`InterruptHoldoffCount` nonzero. The merge would remain uncancellable, and
every error path would need explicit stale-owner recovery. This repeats the
problem the redesign is meant to remove.

**Bitset snapshot and delta replay** could let VACUUM run during a merge.
However, merge removes dead documents and renumbers surviving `doc_id`
values. Publication would need a complete old-to-new mapping for every
source, a versioned bitset snapshot, and an atomic replay of deaths that
arrived during the build. This adds substantial memory and correctness
surface before measurements show that merge-versus-VACUUM serialization is
important.

**Optimistic concurrent same-index merges** could let workers build without a
maintenance gate and allow only one matching source snapshot to publish.
Workers that selected the same sources would duplicate the full CPU and I/O
cost, while durable source reservations would require their own crash-recovery
protocol. Storage bandwidth is likely the limiting resource during merge, so
speculative duplication is a poor default.

The chosen heavyweight relation lock removes reader/writer contention while
keeping source stability and failure cleanup in PostgreSQL's existing lock
manager.

### Lock choice

Merge and VACUUM acquire `ShareUpdateExclusiveLock` on the index relation for
the portion of the operation that can mutate or derive segment state.

This is preferable to a second LWLock:

- `ShareUpdateExclusiveLock` is compatible with the `AccessShareLock` and
  `RowExclusiveLock` modes used by normal queries and DML.
- It conflicts with itself, serializing merge against merge and merge against
  VACUUM for one index.
- Waiting for and holding a heavyweight lock does not suppress interrupts for
  the duration of the merge.
- PostgreSQL releases the lock on error, subtransaction abort, or backend
  exit.
- No shared-memory layout change or stale-owner recovery protocol is needed.

The lock is acquired explicitly and released when the maintenance operation
finishes rather than retained until transaction end. Overlapping durable tasks
therefore serialize only their merge steps. After a waiter acquires the lock,
it rechecks the compaction predicate before selecting sources.

### Global lock order

Code that needs more than one class of lock follows this order:

1. relation maintenance lock (`ShareUpdateExclusiveLock`);
2. per-index LWLock (`LW_SHARED` or `LW_EXCLUSIVE`);
3. metapage buffer lock;
4. segment or tombstone buffer locks.

No path may acquire the maintenance lock while holding the per-index LWLock.
This rule is especially important for inline compaction after spill.

Tombstone drain does not need the maintenance lock. It continues to take the
per-index lock in `LW_EXCLUSIVE` mode while unlinking and recycling pages.

## Separate spill from compaction policy

`tp_do_spill()` currently performs the spill and then, while its caller still
holds `LW_EXCLUSIVE`, applies the configured compaction policy. That structure
would deadlock:

1. a merge holds the maintenance lock and waits briefly for the index lock to
   publish;
2. a spill holds the index lock and tries to acquire the maintenance lock for
   inline compaction.

The spill primitive will only publish the L0 segment and return whether it
wrote work that may require compaction. Its caller will:

1. release `LW_EXCLUSIVE`;
2. apply the configured policy:
   - inline: invoke compaction, which acquires the maintenance lock first;
   - background: record the durable request in the same top-level
     transaction;
   - off: do nothing.

Recording a background request after releasing the LWLock does not weaken the
atomicity established by PR #471. The request is still dispatched from the
same top-level transaction at PRE_COMMIT.

Serial and parallel `CREATE INDEX` are different: the new index is not visible
to concurrent sessions. Their private build-time compaction may retain the
existing direct path.

## Merge lifecycle

### Phase 0: select

The merge:

1. acquires the relation maintenance lock;
2. rechecks that a level is above the compaction threshold;
3. acquires the per-index lock in `LW_SHARED`;
4. selects an exact contiguous source run and records:
   - source level and target level;
   - source root block numbers in chain order;
   - the first block after the run;
   - source header counts needed to calculate shrinkage;
5. releases the per-index lock.

Selection walks only the small number of source roots in one merge batch.
Page collection and all term/posting work happen after releasing the index
lock.

`LW_SHARED` is sufficient: it excludes spill, truncate, and tombstone drain
while taking the snapshot, while remaining compatible with scans and ordinary
inserts.

### Phase 1: build

The merge keeps only the heavyweight maintenance lock.

It opens the selected roots directly, collects their page lists, reads their
payload and alive bitsets, and writes a complete output segment into pages
that are not referenced by the metapage.

This is safe because:

- the maintenance lock excludes VACUUM and other merges, so source alive bits
  and chain links cannot change;
- a concurrent spill can prepend an L0 segment but does not modify a selected
  source;
- DDL retains its existing heavyweight-lock exclusion;
- the FSM claim protocol rejects live or double-offered pages;
- relation extension is already safe across backends.

No per-index LWLock is held, so scans, inserts, and spills can proceed. The
existing `CHECK_FOR_INTERRUPTS()` calls become effective except during brief
buffer-locked writes.

After the output is complete, the merge constructs the displaced-page
tombstones as a detached batch:

- the batch head is not yet reachable from `pending_free_head`;
- the batch tail initially points to `InvalidBlockNumber`;
- the API returns both head and tail so publication can attach the current
  deferred-free chain atomically.

The tombstone horizon is sampled immediately before publication, not at
source selection.

### Phase 2: validate and publish

The merge reacquires the per-index lock in `LW_SHARED`. This blocks spill,
tombstone drain, and truncation for the short publication section but remains
compatible with scans and ordinary inserts.

Under that lock it walks the current source-level chain and finds the selected
run. A long-running L0 merge may now have a new prefix from concurrent spills.
The merge records the selected run's current predecessor rather than assuming
that the selected first block is still the level head.

It validates:

- every selected root is still present, contiguous, and in the expected
  order;
- the selected last root still points to the recorded remainder;
- the source and target levels remain valid;
- the completed output header is internally consistent.

With the maintenance lock held, validation should fail only for corruption or
an implementation bug. It must fail closed and leave the published graph
unchanged.

Publication uses one `GenericXLog` record over at most four buffers:

1. metapage;
2. source-run predecessor, when a concurrent L0 prefix exists;
3. output root;
4. detached tombstone tail.

The record:

- points the predecessor, or the source level head, at the recorded
  remainder;
- points the output root at the current target-level head;
- makes the output the target-level head;
- subtracts the selected count from the current source count and increments
  the target count;
- applies dead-document shrinkage to current metapage totals;
- points the detached tombstone tail at the current `pending_free_head`;
- makes the detached batch the new `pending_free_head`.

Using the current metapage counts is essential when L0 spills occurred during
the build. Assigning values derived from the phase-0 snapshot would lose those
concurrent updates.

After the record finishes, the merge releases the shared index lock and the
maintenance lock.

### Reader view

A scan that snapshots the level chain before publication may continue through
the displaced sources. It holds `LW_SHARED`, so tombstone drain cannot recycle
those pages until the scan finishes.

A scan that snapshots the chain after publication sees the output segment.

The publisher does not wait for either class of scan. Its metapage buffer lock
is the only unavoidable short reader contention point.

## VACUUM and alive-bitset correctness

VACUUM acquires the relation maintenance lock before spilling and before
identifying affected segments. It retains the lock through bitset mutation,
legacy-segment rebuild, chain replacement, and metapage shrinkage.

The possible orderings are therefore:

- VACUUM first: merge waits, then copies the updated alive bits.
- Merge first: VACUUM waits, then discovers the published output and marks
  dead heap tuples in that segment.

There is no ordering in which VACUUM produces source `doc_id` values, merge
renumbers the documents, and VACUUM then applies the stale IDs to the output.
This prevents both dead-document resurrection and out-of-bounds bitset
writes.

`tp_vacuumcleanup()` operations that only count live documents or drain
already-parked pages do not need the maintenance lock. They retain their
existing per-index locking.

## Force merge and truncation

`bm25_force_merge()` must participate in the maintenance protocol because
relation truncation cannot run while an unreachable output is being built.
It acquires the maintenance lock before spilling, merging, or calculating the
truncation high-water mark.

Normal compaction never truncates the relation. It parks displaced pages for
later FSM reuse.

The force-merge implementation may keep one maintenance lock across its
multi-level operation, but each individual merge still drops the per-index
lock during output construction. Its final truncate retains
`LW_EXCLUSIVE`, as it must exclude relation extension while determining and
applying the high-water mark.

## Overlapping compaction requests

Only one merge runs per index because the maintenance relation lock is
self-conflicting. Different indexes remain independent and can merge in
parallel.

When overlapping requests target one index:

1. one acquires the maintenance lock and performs a merge step;
2. waiters remain interruptible in PostgreSQL's heavyweight lock manager;
3. each waiter rechecks the level threshold after acquiring the lock;
4. a waiter exits without building if the earlier merge removed the need.

This deliberately avoids source reservations and duplicate speculative
output. Same-index parallel merges can be reconsidered only after measurement
shows that merge serialization, rather than reader/writer blocking or storage
bandwidth, is the limiting factor.

## Error, cancellation, and crash behavior

### Before publication

The existing level chains remain authoritative. Any complete or partial
output and detached tombstone pages are unreachable, so they cannot affect
query results or replay.

If validation fails in normal control flow, the implementation reclaims:

- output segment and page-index pages;
- detached tombstone container pages.

It must not reclaim the source pages listed inside those tombstones.

If the backend errors, is cancelled, or crashes during the build, unreachable
pages can leak until REINDEX. This matches existing accepted crash windows in
segment writing and fragmented memtable append. Removing the long-held
LWLock makes query cancellation effective, so it can make this leak easier to
trigger deliberately; the implementation must document and log the condition
when the backend survives.

A future hardening change may add a WAL-logged scratch allocation manifest.
That is intentionally separate because it changes the metapage format and
adds WAL work to every allocation. It is not required for index correctness
or for proving the concurrency design.

### Publication

`GenericXLog` makes the graph swap and deferred-free linkage one WAL record:

- recovery before the record sees the old graph;
- recovery after the record sees the new graph and its pending-free batch;
- recovery never sees sources unlinked without their pages parked.

Interrupts are held off only during the brief LWLock/buffer-locked publication
section. A timeout arriving in that window can be reported after publication;
as with other physical index maintenance, the merge may be applied even if
the calling statement reports cancellation. The long build phase is
cancellable.

### After publication

The output is authoritative. Source pages remain parked until the existing
reclaim horizon permits FSM reuse.

## WAL and standby behavior

Every output page is WAL-logged before publication. Replay may reconstruct
unreachable output pages before replaying the publication record; no scan can
discover them.

The publication record updates the level graph and deferred-free graph
atomically. It uses the same `FullTransactionId` horizon discipline as the
current issue #380 implementation. A standby query that began against the old
graph keeps the primary reclaim horizon back through
`hot_standby_feedback = on`.

No custom resource manager or standby-side extension code is introduced.

## Observability

Debug-level timing should distinguish:

- maintenance-lock wait;
- source selection;
- output build;
- publication;
- total merge duration.

Tests need a way to pause a merge deterministically after selection and before
publication. The pause mechanism is test-only and superuser-only, following
the existing debug GUC pattern. It must not be enabled in normal operation.

The production-facing pending-free page count remains
`bm25_pending_free_pages()`. No new production GUC is required to enable the
locking protocol; replacing a long exclusive section with the new protocol
should not create two permanent correctness modes.

## Test design

Timing-only stress tests remain useful but are not sufficient. The primary
tests use deterministic phase barriers.

### Deterministic concurrency tests

1. **Scan during build:** pause a large merge in phase 1. Start a top-k scan
   and require it to finish before releasing the merge. Verify results before
   and after publication.
2. **Insert during build:** with the merge paused, insert documents and
   require the statement to finish. Verify all documents remain searchable.
3. **Spill during L0 build:** publish a new L0 segment while the merge is
   paused. After publication, verify the new prefix remains linked, level
   counts are correct, and both old and new documents are searchable.
4. **VACUUM ordering:** pause the merge, start VACUUM, and prove VACUUM waits
   on the maintenance lock. After releasing the merge, verify VACUUM marks
   dead documents in the output and no deleted tuple is resurrected.
5. **Same-index serialization:** pause one merge, start a second, and prove
   the second waits without consuming source pages or writing output. After
   release it rechecks the threshold.
6. **Different-index overlap:** pause merges for two indexes in phase 1 and
   prove both reach the barrier concurrently.
7. **Tombstone drain ordering:** begin a scan against the old graph, publish
   while it runs, and prove drain cannot recycle the source pages until the
   scan releases `LW_SHARED`.
8. **Force-merge/truncate ordering:** pause normal compaction output and prove
   force merge cannot compute or apply a truncation high-water mark until the
   maintenance lock is available.

### Failure and recovery tests

1. Cancel during term enumeration and during output writing. Verify the old
   graph remains queryable and no partial output is published.
2. Inject a validation failure after a complete build. Verify output and
   tombstone container pages are reclaimed without freeing source pages.
3. Crash before publication, during publication WAL, and immediately after
   publication. Verify recovery returns either the complete old graph or the
   complete new graph.
4. Repeat publication and reclaim with a hot-standby query held open. Verify
   feedback prevents source-page reuse until the standby query ends.

### Stress and performance tests

- Extend the existing writer + force-merge + deleter + VACUUM stress test to
  include normal stepped compaction and phase-aware diagnostics.
- Run sanitizer jobs for the complete suite.
- Benchmark scan and insert latency during a large merge before and after the
  change.
- Record publication duration separately from build duration.

The deterministic acceptance criterion is stronger than a machine-dependent
latency threshold: while a merge is paused indefinitely in phase 1, an
independent scan and insert must complete within the test timeout. Performance
numbers are reported for regression tracking rather than used as flaky CI
gates.

## Staged delivery

### Stage 1: audit and race harness

- Document and assert the lock order.
- Add deterministic select/build/publish pause points.
- Extend concurrent merge/VACUUM/spill/scan/recovery coverage.
- Add diagnostics that identify which merge phase was active on failure.

### Stage 2: spill/maintenance boundary

- Remove compaction policy execution from the spill critical section.
- Add the relation maintenance lock to merge, VACUUM mutation, and force
  merge.
- Recheck predicates after maintenance-lock acquisition.

### Stage 3: non-blocking merge

- Introduce explicit merge-plan, merge-output, and detached-tombstone
  structures.
- Split selection, build, validation, and publication.
- Preserve concurrent L0 prefixes during publication.
- Publish the level swap and deferred-free linkage atomically.

### Stage 4: administrative and failure hardening

- Adapt force merge and truncation to the new protocol.
- Reclaim completed unpublished output on validation failure.
- Document cancellation's pre-publication orphan behavior.
- Verify crash recovery and standby reclaim.

### Stage 5: measure and decide

- Compare reader/writer latency and lock waits with the baseline.
- Audit any remaining long `LW_EXCLUSIVE` sections.
- Consider non-blocking spill only if spill is now material.
- Consider same-index parallel merges only if serialized merge throughput is
  the measured bottleneck.

## Acceptance criteria

- No per-index LWLock is held while constructing merged output.
- A scan and insert complete while a merge is deterministically paused in its
  build phase.
- A concurrent L0 spill remains reachable after merge publication.
- VACUUM cannot mutate source alive bits during a merge and does not
  resurrect deleted documents.
- Same-index maintenance serializes without deadlock; different indexes can
  merge concurrently.
- Publication and deferred reclaim remain crash-safe and standby-safe.
- Existing SQL, concurrency, crash-recovery, replication, sanitizer, and
  formatting tests pass.
- The implementation introduces no custom WAL resource manager and no
  persistent feature flag.
