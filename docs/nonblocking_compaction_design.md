# Fair spill admission and non-blocking compaction

Status: implemented design for #495

## Summary

Before this change, continuous ranked scans could indefinitely starve a spill
because PostgreSQL LWLocks allow new shared holders to acquire ahead of an
already-waiting exclusive holder. Once a spill did acquire the lock, the
inline compaction path held it through the entire segment merge, producing a
second pathology: all scans and inserts stopped for the duration of a large
merge.

The implemented design addresses the two problems independently:

1. Added writer-preference admission in front of the per-index LWLock. Once an
   exclusive acquisition is pending, new shared acquisitions wait until the
   exclusive waiter acquires the lock.
2. Separated spill publication from compaction policy so no path acquires a
   maintenance lock while holding the per-index lock.
3. Serialized compaction and VACUUM segment mutation with an interruptible
   private per-index heavyweight object lock.
4. Split compaction into short selection, long unlocked build, and short
   exclusive publication phases.
5. Preserved L0 segments published by concurrent spills and attached displaced
   source pages to the existing standby-safe deferred-free chain in the same
   WAL-logged publication.

The managed background compaction implementation from #478 decides *when*
background compaction runs. This implementation changes *how* every invocation
of `bm25_compact_step()` coordinates with foreground work. Together, inline
compaction stops blocking readers and background mode also avoids making the
foreground writer perform the merge.

## Evidence

The mixed MS MARCO workload in #494 uses 32 ranked-query clients and one
writer targeting 1,000 indexed updates per second.

| Development build, 60-second workload | Mixed query QPS | Updates | Maximum query latency |
|---|---:|---:|---:|
| Current locking | 2,270 | 1,888 | 283 ms |
| Writer-preference gate | 636 | 13,237 | 111.4 s |
| Gate plus shorter reader lifetime | 609 | 13,213 | 114.9 s |
| Gate, compaction disabled | 2,151 | 17,858 | 279 ms |
| Gate, merge built outside the lock | 2,181 | 13,245 | 285 ms |

The baseline keeps queries fast only because the writer is starved. Fair
admission lets the writer reach compaction, revealing a minute-scale reader
outage. Building the same inline compaction outside the per-index lock retains
94% of read-only query throughput and eliminates query latencies above one
second.

Shortening the pre-change reader lock lifetime did not improve throughput. It
also moves primary page-reuse safety from a simple lock invariant to
transaction-horizon reasoning. The implementation therefore keeps the existing
reader lock lifetime and relies on fair admission to bound exclusive waits.

## Goals

- Prevent indefinite spill, publication, drain, and truncation starvation
  under continuous ranked scans.
- Hold no per-index LWLock while constructing merged segment output.
- Allow scans and inserts to continue throughout the merge build phase.
- Allow a spill to prepend new L0 segments while compaction builds.
- Keep compaction cancellable during its long build phase.
- Preserve alive-bit correctness across VACUUM and compaction.
- Preserve stock PostgreSQL physical replication without a custom resource
  manager.
- Preserve standby-safe deferred reclaim from #380.
- Keep the compaction engine compatible with inline, manual, callback-driven,
  and #478 managed-background invocation.

## Non-goals

- Changing the pg_durable workflow lifecycle introduced by #478.
- Changing the default compaction mode.
- Making memtable spill itself generation-swapped or lock-free. Measurements
  show ordinary spill is not the long exclusion window.
- Running multiple merges concurrently for one physical index.
- Eliminating every possible pre-publication orphan after backend crash. Such
  pages remain unreachable and can be reclaimed by `REINDEX`.
- Removing the `hot_standby_feedback = on` requirement for query-serving hot
  standbys.

## Historical motivation (pre-change)

### Shared LWLock barging

Ranked scans and ordinary inserts acquired the per-index lock in `LW_SHARED`.
Spill, tombstone drain, force merge, and compaction acquired it in
`LW_EXCLUSIVE`.

PostgreSQL's shared LWLock acquisition checks whether an exclusive holder
currently owns the lock; it does not reject a new reader merely because an
exclusive waiter is queued. A saturated stream of scans can therefore keep at
least one shared holder active indefinitely without the admission gate.

### Merge-duration exclusion

Before the change, `tp_do_spill()` applied compaction policy before its caller
released `LW_EXCLUSIVE`. `tp_compact_once()` then selected sources, merged
postings, wrote output, built tombstones, flushed buffers, and published while
the same lock remained held.

The merge is copy-on-write. Its output is unreachable until publication, so
the long exclusive lifetime is not required for reader safety.

## Concurrency primitives

### Fair per-index LWLock admission

`TpSharedIndexState` gains:

- an atomic count of pending exclusive acquisitions;
- a condition variable used to wake gated shared acquirers.

An exclusive acquirer increments the count before calling `LWLockAcquire()`.
New shared acquirers sleep while the count is nonzero. The exclusive acquirer
decrements the count immediately after acquisition and broadcasts when the
last pending exclusive acquirer has acquired.

A reader can pass the admission check immediately before an exclusive waiter
increments the count. This bounded race is acceptable: only readers already
past the gate can barge, so a continuous stream of later readers cannot cause
starvation.

The count covers acquisition only, not the exclusive critical section. This
avoids leaking the gate through errors in spill or publication. PostgreSQL
LWLock acquisition holds interrupts until it returns, so the increment and
matching decrement cannot be separated by ordinary query cancellation.

### Per-index maintenance lock

Compaction, VACUUM segment mutation, and force merge acquire
an exclusive heavyweight object lock keyed by the physical index OID. It uses
pg_textsearch's private `pg_am` subobject namespace with a discriminator that
is distinct from #478's managed-admission and lineage locks.

This lock:

- conflicts with itself, serializing segment-derived maintenance for one
  physical index;
- does not conflict with relation locks held by ordinary queries, DML, or the
  pre-commit managed dispatcher;
- is interruptible and automatically released on error or backend exit;
- requires no shared-memory owner recovery protocol;
- permits compaction of different indexes concurrently.

Compaction callers retain the normal relation lock appropriate to their SQL or
index-AM operation. The private object lock supplies only the same-index
maintenance serialization. Keeping it distinct from #478's admission lock is
required because the dispatcher deliberately holds its target admission while
signaling a worker that may compact before the spilling transaction commits.

The maintenance lock protects source segment payload, alive bitmaps,
`alive_count`, and chain relationships while a replacement is derived. It
does not serialize ordinary memtable appends or L0 spill publication.

### Lock order

Any path needing more than one lock class follows this order:

1. per-index maintenance object lock;
2. per-index LWLock;
3. metapage buffer lock;
4. segment or tombstone buffer lock.

No path may acquire the maintenance lock while holding the per-index LWLock.
Spill must therefore release `LW_EXCLUSIVE` before applying inline or
background compaction policy.

## Operation matrix

| Operation | Maintenance lock | Per-index lock |
|---|---|---|
| ranked scan | none | `LW_SHARED`, existing lifetime |
| normal insert | none | `LW_SHARED` during append |
| spill | none | `LW_EXCLUSIVE` through L0 publication |
| compaction selection | per-index maintenance object lock | `LW_SHARED` |
| compaction build | per-index maintenance object lock | none |
| compaction publication | per-index maintenance object lock | `LW_EXCLUSIVE` |
| VACUUM segment mutation | per-index maintenance object lock | existing shared/exclusive sections |
| tombstone drain | none | `LW_EXCLUSIVE` |
| force merge | per-index maintenance object lock | phase-specific; exclusive for truncate |

Publication remains exclusive even though it is short. A concurrent L0 spill
can prepend segments while compaction builds. Preserving that prefix requires
changing the `next_segment` pointer of its last segment. Current scans capture
level heads but traverse segment links lazily; allowing publication under a
shared lock could make one scan observe an old metapage head and a new
predecessor link, omitting documents. Brief exclusive publication avoids this
mixed graph.

## Spill and compaction policy

The durable spill primitive only:

1. materializes the current memtable;
2. writes and publishes the new L0 segment;
3. disconnects and parks the old memtable chain;
4. resets cache and spill accounting;
5. reports whether compaction may now be needed.

Its caller releases the per-index lock before applying policy:

- `inline`: invoke the compaction engine synchronously;
- `background`: record the existing transaction-local compaction request;
- `manual` or `off`, depending on the branch version: do nothing.

Build-private compaction during `CREATE INDEX` may retain its current direct
path because the index is not visible to concurrent sessions.

This boundary is required to avoid a lock inversion where compaction holds
the maintenance lock and waits to publish while another spill holds the
per-index lock and waits to start inline compaction.

## Compaction lifecycle

### Phase 0: maintenance admission

The caller acquires the private per-index maintenance object lock and
rechecks whether a reducible level remains above threshold. Competing
same-index compactions and VACUUM wait here without blocking normal scans or
inserts.

The public functions `bm25_compact()`, `bm25_compact_step()`, and the
generation-checked step used by #478 all enter the same engine. A step remains
one transaction and at most one publication. The #478 helper must first check
its captured physical generation before waiting, then acquire maintenance and
recheck the database, tablespace, relfilenumber, owner, and background mode
before selection. It must not wrap the step in a coarse per-index LWLock;
`tp_compact_step()` owns the phase-specific shared and exclusive acquisitions.

### Phase 1: select

With the maintenance lock held, compaction briefly acquires the per-index lock
in `LW_SHARED` and builds an immutable plan containing:

- every selected source root, level, and chain position;
- each selected contiguous run's first root and remainder root;
- retained heads and counts;
- output batches and destination levels;
- source statistics needed to calculate dead-document shrinkage.

Selection releases the per-index lock before collecting complete page lists
or reading postings.

`LW_SHARED` excludes spill publication while the snapshot is taken but remains
compatible with scans and ordinary inserts.

### Phase 2: build

Compaction holds only the heavyweight maintenance lock while it:

1. reads source dictionaries, postings, document maps, and alive bitmaps;
2. writes complete, WAL-logged output segments that are not reachable from
   the metapage;
3. records output roots, counts, statistics, and exact allocation ownership;
4. collects all displaced source pages;
5. builds a detached tombstone batch whose tail initially points to
   `InvalidBlockNumber` and whose reclaim stamp is provisionally invalid;
6. flushes output WAL and dirty relation buffers needed before publication;
7. validates every prepared output segment and detached tombstone link while
   the structures remain unreachable.

The maintenance lock prevents VACUUM or another compaction from changing the
selected sources. Concurrent scans read the old graph. Concurrent inserts
append to the memtable. A concurrent spill may prepend new L0 segments.

The build phase contains regular interrupt checks and holds no LWLock across
CPU or I/O work.

### Allocation and output ownership

Unlocked build can extend the index concurrently with memtable growth. Every
runtime allocator that falls back from the FSM must therefore use
`ExtendBufferedRel(..., EB_LOCK_FIRST)`: memtable, segment, and tombstone
allocation may not mix this with `ReadBufferExtended(P_NEW)`. PostgreSQL 17
does not coordinate those two extension APIs strongly enough to prevent both
paths from reserving the same block.

Handled-error cleanup is based on explicit ownership rather than discovering
pages from partially initialized links:

- the segment writer records each data page immediately on allocation;
- page-index construction records each page-index page immediately;
- a completed output segment transfers ownership to the compaction output as
  an exact root before later validation or accounting can fail;
- detached tombstone construction records each container page before page
  initialization or WAL work.

Low-level catches return partially allocated data, page-index, and tombstone
container pages to the FSM. The outer compaction catch discards each completed
owned output root and detached container page. None of these paths frees a
selected source page or follows an untrusted output link to infer ownership.

### Phase 3: stamp reclaim

After the unlocked build, compaction assigns its full transaction ID and
restamps every detached tombstone container with it while the batch remains
unreachable. The assigned transaction remains in progress through graph
publication, pinning primary and standby horizons even when a standby ranked
cursor begins on the old graph after restamping.

Runtime restamping holds the per-index maintenance lock but no per-index
LWLock. Its work scales with the number of tombstone containers without
turning that work into reader exclusion.

### Phase 4: validate

Compaction requests the fair per-index lock in `LW_EXCLUSIVE`. Once acquired,
no primary reader, insert, spill, drain, or truncation operation can observe a
partially modified graph.

Validation reads the current metapage and level chains. It requires:

- every selected source root still exists, is contiguous, and appears in the
  expected order;
- non-L0 selected runs retain their expected predecessor and remainder;
- an L0 selected run may have only a newly prepended prefix before it;
- destination chains remain compatible with the prepared output.

Complete prepared-output validation already ran before XID assignment,
restamping, and reader exclusion. Publication retains only constant-time
detached-tail checks before attachment.

Concurrent memtable head/tail changes and a changed deferred-free head are
expected and do not invalidate the plan. Current metapage values, not the
phase-1 snapshot, are the base for publication.

With the maintenance lock held, validation failure indicates corruption, an
implementation error, or an operation not yet participating in the
maintenance protocol. It fails closed without changing the published graph.

### Phase 5: publish

One final `GenericXLog` publication:

1. splices a concurrent L0 prefix around the selected run when necessary;
2. removes every selected run from its source level;
3. links prepared outputs into their destination levels;
4. updates level heads and counts from the current metapage values;
5. subtracts dead-document shrinkage from current corpus totals;
6. links the detached tombstone tail to the current `pending_free_head`;
7. makes the detached tombstone batch the new pending-free head.

The final record includes the metapage, the optional L0 predecessor page, and
the detached tombstone tail. Output root links are finalized and WAL-logged
while the output is still unreachable.

The per-index lock is released immediately after publication. The maintenance
lock is then released or retained for the caller's next explicitly bounded
step.

## Reader and reclaim behavior

A primary scan that starts before publication holds `LW_SHARED`, so exclusive
publication waits for it to finish traversing the old graph. A scan that
starts afterward sees the new graph. No primary scan observes both layouts.

Displaced source pages are still parked rather than immediately returned to
the FSM. This remains necessary for:

- hot-standby readers that can still be traversing the old graph;
- crash and replay ordering;
- other no-extension-load replay contexts.

Tombstone drain keeps its existing exclusive lock and horizon check.
`hot_standby_feedback = on` remains required on query-serving standbys so
connected readers normally hold the primary reclaim horizon and complete
without cancellation. Before a reclaimable tombstone batch enters the FSM,
drain also emits PostgreSQL's stock `XLOG_BTREE_REUSE_PAGE` conflict-only WAL
record. Its redo path does not inspect btree storage; it cancels old standby
snapshots before subsequent WAL can reuse those pages. This protects a query
that remains active while its standby disconnects and later resumes replay,
without adding a pg_textsearch resource manager.

## VACUUM

Segments are immutable except for their alive bitmaps and chain metadata.
VACUUM must acquire the per-index maintenance lock before identifying segment
document IDs and retain it through:

- alive-bit mutation;
- legacy segment replacement;
- metapage corpus-statistic adjustment;
- any segment unlink or replacement.

The possible orderings are:

- VACUUM first: compaction waits, then reads the updated alive bits.
- Compaction first: VACUUM waits, then discovers the published output and
  applies deaths using that segment's document IDs.

This prevents deleted documents from being resurrected and prevents VACUUM
from applying stale source document IDs to a renumbered output segment.
Legacy segment replacement assigns VACUUM's current full transaction ID before
building its tombstone batch and retains that in-progress XID through the
atomic replacement publication. A standby snapshot that can still see the old
segment graph therefore cannot be newer than its reclaim stamp.

PostgreSQL may call index bulk-delete in a parallel worker or in a leader that
is already in parallel mode. Those contexts cannot assign an XID. A V5 segment
that becomes empty therefore keeps its zeroed alive bitmap and remains
physically linked until later serial compaction; it is immediately logically
empty and does not return dead TIDs. Spill remains durable, but spill-time
compaction is likewise deferred. An affected legacy segment cannot represent
deletions without replacement, so parallel VACUUM fails closed with a request
to retry using `VACUUM (PARALLEL 0)`.

Pure counting that does not retain source document IDs may remain outside the
maintenance lock. Spill invoked by VACUUM follows the normal lock order.

## Force merge and truncation

`bm25_force_merge()` acquires the maintenance lock before selecting sources
and retains it through its bounded merge work. Each output build drops the
per-index lock exactly like ordinary compaction.

Relation truncation remains an exclusive phase. It acquires the per-index
lock in `LW_EXCLUSIVE`, computes the high-water mark, and truncates before
releasing it. The maintenance lock ensures no unreachable compaction output
is being constructed concurrently.

Normal compaction never truncates the relation.

## Background compaction compatibility

#478's managed workflow invokes a generation-checked
`bm25_compact_step_if_current()` once per transaction. The integrated entry
point preserves that contract:

- target identity, owner checks, lifecycle locks, signaling, and scheduling
  remain unchanged;
- the helper performs its cheap captured-generation check before waiting;
- after acquiring per-index maintenance, it rechecks the captured physical
  identity and background mode before entering the common phase engine;
- the helper does not acquire the per-index LWLock around the whole step;
- one step performs at most one select/build/publish pass;
- the long build holds neither the per-index LWLock nor #478-specific
  lifecycle locks beyond those already required by its target validation;
- spills on a `background` index can continue and signal additional work
  while a worker builds;
- a worker rechecks compaction debt after maintenance admission.

The helper performs a cheap physical-generation check before maintenance
admission, repeats the full database, tablespace, relfilenumber, owner, and
background-mode check after admission, and then calls the common phase engine
without a coarse per-index LWLock. The post-spill policy boundary remains
outside the spill's per-index exclusive section.

## Failure, cancellation, and crash behavior

### Before publication

The old graph remains authoritative. Partial or complete outputs and detached
tombstones are unreachable.

On a handled validation failure or ordinary pre-publication error path, exact
allocation tracking returns partially built segment data pages, page-index
pages, completed output segments, and detached tombstone container pages to
the FSM. It never frees the selected source pages listed inside the detached
tombstones.

If an error occurs after `GenericXLogStart()` but before
`GenericXLogFinish()`, publication aborts the unfinished GenericXLog state,
releases its buffers, and discards the still-unreachable prepared output.
Ownership transfers only after `GenericXLogFinish()` succeeds.

A backend crash bypasses those catches and can leave unreachable output pages.
This is an accepted leak until `REINDEX`; it cannot produce wrong query
results or unsafe page reuse. A durable scratch-allocation manifest is a
separate future enhancement.

### During publication

`GenericXLog` makes graph replacement and deferred-free attachment one atomic
WAL action. Recovery sees either:

- the old graph with no reachable output, or
- the new graph with all displaced pages reachable from the pending-free
  chain.

Interrupts are held only for the brief publication section. Cancellation
during that section may be reported after the physical publication completes,
matching existing index-maintenance semantics.

### After publication

The output is authoritative. Source pages remain parked until the reclaim
horizon permits reuse.

## Observability

Debug timing distinguishes:

- writer-admission wait;
- maintenance-lock wait;
- source selection;
- output build;
- exclusive publication;
- total step duration.

The benchmark continues to sample `pg_stat_activity` and report per-operation
latency distributions. No permanent feature flag selects the old locking
model.

## Test strategy

### Deterministic concurrency tests

Test-only, superuser-only pause points allow a merge to stop after selection
and before publication.

Required cases:

1. A scan completes while a merge is paused in the build phase.
2. An insert completes while a merge is paused in the build phase.
3. A spill prepends L0 while a merge is paused; publication preserves the
   prefix and all documents.
4. Continuous scans cannot starve an exclusive spill request.
5. A second same-index compaction waits on the maintenance lock and rechecks
   debt after admission.
6. Compactions of different indexes can reach the build pause concurrently.
7. VACUUM waits behind a paused merge and applies deletions to the published
   output.
8. Force merge and truncation wait for an active ordinary compaction.

### Failure and recovery tests

1. Cancel during source reading and output writing; the old graph remains
   queryable.
2. Inject publication validation failure; output pages are reclaimed and
   source pages remain live.
3. Crash before, during, and after publication; recovery yields a complete
   old or new graph.
4. Pause after the unlocked build, advance primary XIDs, then start a
   hot-standby query on the old graph before publication. Verify its pages are
   not reused before feedback releases the publication-time horizon.
5. Disconnect a standby while an old-graph cursor is open, reclaim on the
   primary, reconnect, and verify stock WAL replay cancels the cursor before
   replay can expose page reuse.

### Performance tests

Use the mixed MS MARCO driver from #494 and a deterministic compaction-overlap
harness. Report:

- query and update throughput;
- p50, p95, p99, p99.9, and maximum latency;
- operations above 100 ms and one second;
- maintenance build and publication durations;
- per-application wait events.

The performance acceptance target is no zero-QPS interval during compaction,
no merge-duration `tapir_index_lock` waits, and mixed query throughput within
10% of its no-maintenance control on the same machine.

## Delivery boundaries

The implementation PR includes:

1. fair per-index lock admission;
2. spill/compaction policy separation;
3. per-index maintenance locking;
4. select/build/validate/publish compaction;
5. detached tombstone publication and failure cleanup;
6. VACUUM and force-merge integration;
7. deterministic concurrency, recovery, and benchmark coverage;
8. architecture and operator documentation.

With #478 now on the base branch, `compaction = 'background'` has both desired
properties: foreground writers do not perform merges, and background merges
do not stall foreground readers or memtable inserts.

## Acceptance criteria

- Continuous ranked scans cannot indefinitely starve a spill.
- No per-index LWLock is held while merged output is constructed or flushed.
- A scan and insert finish while a merge is paused indefinitely in build.
- A concurrent L0 spill remains reachable after compaction publication.
- Publication blocks readers only for its bounded exclusive section.
- VACUUM cannot mutate selected source alive bits during a merge.
- Same-index maintenance serializes; different indexes can overlap.
- Publication and deferred reclaim remain crash-safe and standby-safe.
- Inline, manual, callback-driven, and #478 managed compaction use the same
  engine and retain their existing external contracts.
- Existing regression, concurrency, recovery, replication, sanitizer, and
  formatting checks pass.
