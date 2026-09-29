# Nonblocking Memtable Spill Design

## Goal

Keep ranked queries responsive while a visible index spills its on-disk
memtable to an immutable L0 segment. The spill may pause writers, but segment
construction must not hold the per-index lock that readers need.

This change complements managed pg_durable background compaction. It does not
change the default `compaction = inline` policy.

## Benchmark evidence

The 8.84 million-row MS MARCO mixed workload uses 32 readers, 16 reader
threads, one writer targeting 1,000 updates per second, a 60-second warmup,
600 seconds read-only, and 600 seconds mixed.

| Mode | Read-only QPS | Mixed QPS | Updates/s | Longest zero-update interval |
|---|---:|---:|---:|---:|
| Inline | 2,195 | 1,961 | 103 | 400s |
| pg_durable background | 2,191 | 1,552 | 292 | 30s |

Background compaction removed the long inline writer pause, but readers
recorded 3,487 `tapir_index_lock` samples. The samples included an
approximately 118-second interval in which all 32 readers were blocked.

The spill path explains the result. It takes `tapir_index_lock` exclusive,
extracts the chain, writes the complete L0 segment, publishes it, and retires
the old chain before releasing the lock. Segment construction dominates that
critical section.

## Considered approaches

### Fair memtable writer/spill gate

Add a per-index fair LWLock used only by memtable writers and spill. Appends
take it shared. Spill takes it exclusive, briefly takes the existing index
lock to freeze and extract the chain, releases the index lock while building
the segment, then briefly reacquires it to publish.

This is the selected approach. It keeps the existing chain query-visible
during construction, requires no storage-format change, and prevents
continuous writers from starving spill.

### Optimistic build and retry

Spill could snapshot the chain, release all locks, build a segment, then
discard and retry if writers changed the chain. Continuous updates can force
unbounded retries and waste large amounts of I/O. This approach is rejected.

### Active and frozen memtable chains

The metapage could publish separate active and frozen chains so writers
continue during spill. This provides the most concurrency but changes the
storage format, WAL protocol, scan inputs, recovery, and upgrade behavior. It
is unnecessary for the current goal and is rejected.

## Locking

Add a fair memtable writer/spill gate to `TpSharedIndexState`, with its own
exclusive-waiter counter and condition variable. Register a distinct
`tapir_memtable_write_lock` tranche name.

The gate follows the existing writer-preference protocol used by
`tapir_index_lock`: an exclusive spill registers before waiting, and new
shared appenders wait while any exclusive waiter is registered. This bounds
the appenders that can pass a queued spill.

Runtime lock order is:

1. maintenance admission, for callers such as force merge that already own it;
2. compaction publication barrier, when required;
3. memtable writer/spill gate;
4. per-index lock;
5. page buffer locks.

Readers acquire only the per-index lock and therefore continue while segment
construction holds the writer/spill gate.

Private CREATE INDEX state remains single-backend and does not need the new
gate.

## Spill lifecycle

Split runtime spill into three phases.

### Freeze and extract

The spill caller acquires the publication barrier in `ShareLock`, the
writer/spill gate exclusive, and the per-index lock exclusive. It rechecks the
spill threshold and extracts the stable chain into the existing prepared-spill
structures.

The spill then releases the per-index lock. It retains the publication barrier
and writer/spill gate, so:

- readers can scan the unchanged chain;
- no writer can append to it;
- compaction cannot enter its final publication window.

### Build

Write and validate the complete WAL-logged but unreachable L0 segment without
holding the per-index lock.

### Publish

Reacquire the per-index lock exclusive. Publish the L0 root, disconnect and
stamp the old chain for deferred reclaim, advance `spill_generation`, and
reset `chain_page_count`. Release the index lock, writer/spill gate, and
publication barrier.

The old chain remains the query-visible source of truth until publication.
After publication, the segment replaces it atomically under the existing
per-index lock.

## Other spill callers

Threshold spill, explicit spill, VACUUM's deferred spill, shutdown spill, and
force-merge pre-spill use the same lifecycle.

Shutdown's no-wait path conditionally acquires the publication barrier,
writer/spill gate, and per-index lock in both phases. It skips if any is busy,
discarding unpublished output if publication cannot acquire the index lock.
Buffer, WAL, and I/O work may still wait after admission.

The compaction policy still runs only after spill publication and after all
spill locks have been released.

## Failure behavior

An error or cancellation before publication leaves the original chain
published and queryable. Any completed unpublished segment is discarded using
the existing segment cleanup path.

An error after publication follows the existing physical-maintenance
semantics: the published spill is not undone by transaction rollback.
The cleanup ownership flag is set at WAL publication, before fallible cache
cleanup and chain retirement.

Cleanup paths release each lock only if the current operation acquired it.
They do not depend on transaction-end LWLock cleanup for normal control flow.

## Validation

The identical MS MARCO pg_durable workload is the primary acceptance test.
The change is not successful unless a fresh run meets all of these conditions:

- no sustained interval in which all readers wait on `tapir_index_lock`
  during spill;
- at least 1,900 mixed reader queries per second;
- at least 250 updates per second;
- no writer zero-progress interval longer than 30 seconds.

The full run with spill and reclaim changes reached 1,711 mixed QPS and
277 updates/s, with a maximum writer completion gap of 1.11 seconds. Reader
index-lock samples fell from 3,487 to 74 across four isolated sampled seconds.
The 1,900-QPS target was not met. The run completed 2.7 times as many updates
per second as the inline baseline; equal-write-load performance remains
unmeasured, and resource contention is a hypothesis rather than a proven
explanation for the entire shortfall.

Deterministic injection coverage supports, but does not replace, the
benchmark. It verifies that:

- a ranked reader completes and sees the old chain while spill construction
  is paused;
- a concurrent writer waits on `tapir_memtable_write_lock`;
- cancellation before publication leaves the old chain queryable;
- an error after publication preserves the live segment;
- shutdown spill avoids blocking index-lock acquisition in both phases;
- successful publication exposes all documents exactly once.

Build, SQL regression, spill recovery, concurrency, and formatting checks are
required supporting validation.

## Documentation

Update `ARCHITECTURE.md` with the new lock order and three-phase spill
lifecycle. Update the README's concurrency description to state that runtime
spill construction blocks memtable writers but not ranked readers.

## Out of scope

- changing the default compaction mode;
- changing pg_durable orchestration;
- allowing appends to continue during one index's spill;
- changing the metapage or segment on-disk format;
- changing compaction selection or size policy.
