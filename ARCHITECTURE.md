# Architecture

pg_textsearch is a PostgreSQL index access method for BM25-ranked full-text
search. It uses an LSM-like layout: an on-disk memtable receives writes and
spills into immutable segments that are compacted across levels.

## Source Layout

- `src/access/` — access-method build, scan, vacuum, and SQL maintenance entry
  points
- `src/types/` — `bm25query`, `bm25vector`, and SQL operators
- `src/planner/` — planner hooks and cost estimation
- `src/scoring/` — BM25 and Block-Max WAND
- `src/index/` — index state, metapage, registry, limits, and posting sources
- `src/memtable/` — on-disk write buffer and derived query cache
- `src/segment/` — immutable segments, compression, merge, and deferred reclaim
- `src/debug/` — optional diagnostic functions

These directories are an organizational model, not a strictly enforced
dependency graph. Some storage coordination crosses layers; for example,
segment compaction uses reclaim-horizon logic from the access layer. Project
includes use paths relative to `src/`.

## Storage and WAL

Block 0 is the metapage. It points to the on-disk memtable chain, immutable
segment chains for each LSM level, and the deferred-free tombstone chain.

Writes append document records to the memtable chain under buffer locks.
In-place and multi-page publication mutations use `GenericXLog`. Newly written
segment pages use `log_newpage_buffer()` when `RelationNeedsWAL()` is true.
Before reclaimed segment pages enter the FSM, pg_textsearch emits PostgreSQL's
stock btree page-reuse conflict record; its redo path only resolves old standby
snapshots and does not inspect btree storage. pg_textsearch has no custom WAL
resource manager. The on-disk chain is authoritative through crash recovery
and physical replication.

Queries compose postings from the memtable and all published segments. Each
live heap TID occurs in at most one published segment. Segment-local numeric
`doc_id` values may repeat across segments. This disjointness permits merge and
scoring paths to avoid cross-segment document deduplication.

## Memtable Cache

The shared-memory memtable cache accelerates reads but is derived and
disposable. Writes update only the on-disk chain. Readers lazily build or catch
up the cache; generation mismatches, spills, eviction, or memory pressure can
drop it without affecting correctness. Standbys read the on-disk chain.

`pg_textsearch.memory_limit` has three budget tiers:

- per-index per-record growth guard (`limit / 8`): reject a record whose
  estimated growth would cross the guard and fall back to the chain;
- global soft cap (`limit / 2`): attempt best-effort eviction of the largest
  non-caller cache; eviction may find nothing or a busy target;
- global `limit` is an approximate admission threshold: catch-up or cold build
  falls back when the entry-time estimate is already at the limit. Admitted or
  concurrent work may increase estimated usage past it.

`0` disables the limit. The setting is applied on SIGHUP.

Cache lock order is:

1. per-index LWLock;
2. cache apply lock;
3. cache lifetime lock;
4. dshash bucket locks;
5. posting-list lock.

The global eviction mutex is acquired before another index's per-index lock.

## Spill and Compaction

A spill builds an immutable L0 segment from the memtable, publishes it through
the metapage, and marks the old chain dead for deferred reclaim. Compaction
combines adjacent immutable segments within `pg_textsearch.max_segment_size`.

### Admission and lock order

The per-index LWLock uses writer-preference admission. An exclusive acquirer
increments `exclusive_waiters` before waiting for the LWLock. New shared
acquirers sleep while that count is nonzero. The exclusive acquirer decrements
the count immediately after acquisition and wakes shared waiters when the last
exclusive waiter has acquired. A shared acquirer can pass the gate just before
the count changes, but only that bounded set can barge; a continuing stream of
new readers cannot starve the exclusive waiter.

Compaction, force merge, and VACUUM segment mutation serialize with
`ShareUpdateExclusiveLock` on the physical index relation. This maintenance
lock is compatible with ordinary query and DML relation locks, so it does not
exclude scans or memtable appends. Operations that need multiple lock classes
must acquire them in this order:

1. relation maintenance lock;
2. per-index LWLock;
3. metapage buffer lock;
4. segment or tombstone buffer lock.

No path may request maintenance while holding the per-index lock. A spill
therefore completes L0 publication and releases `LW_EXCLUSIVE` before applying
the configured compaction policy.

The `compaction` index option controls spill-time behavior:

- `inline` compacts threshold debt during spills and index builds;
- `background` dispatches a pre-commit request when possible. Runtime
  no-dispatch contexts such as autovacuum and callback re-entry compact
  inline. Index builds leave compaction to the managed workflow after
  activation, and temporary indexes do not support this mode;
- `manual` leaves debt for explicit maintenance. The legacy `off` value is
  accepted as an alias for `manual`.

Prepared transactions do not flush queued background requests. Unconfigured,
unresolvable, or failed callbacks do not fall back inline; the compaction debt
remains for a later spill or explicit maintenance.

`bm25_compact()` holds one relation maintenance lock while it drives reducible
debt to completion. Each pass uses brief per-index `LW_SHARED` selection, no
per-index lock during output build, and fair `LW_EXCLUSIVE` validation and
publication. `bm25_compact_step()` runs at most one pass. Drive repeated
maintenance from the return value of `bm25_compact_step()`, not
`bm25_needs_compaction()`, because over-budget segments can leave a level
permanently above its advisory threshold.

### Compaction phases

Each runtime pass uses the same phase engine:

1. **Maintenance admission.** Acquire the relation maintenance lock. A caller
   that waited rechecks compaction debt after admission.
2. **Select.** Briefly take the per-index lock in `LW_SHARED`, snapshot the
   metapage, and record exact contiguous source runs and retained remainders.
   Release the per-index lock before reading complete sources.
3. **Build.** Hold only the maintenance lock while reading immutable sources,
   constructing complete WAL-logged but unreachable output segments,
   collecting displaced source pages, and building a detached tombstone batch
   with a provisional invalid reclaim stamp.
   Segment data pages, page-index pages, completed output roots, and tombstone
   container pages have explicit ownership records for handled-error cleanup.
   Validate the complete prepared segment and tombstone chains while they
   remain unreachable.
4. **Stamp reclaim.** Assign the compactor's full transaction ID and restamp
   every detached tombstone container with it while the batch remains
   unreachable and no runtime per-index lock is held. The in-progress
   transaction pins primary and standby horizons through publication.
5. **Validate.** Acquire fair `LW_EXCLUSIVE` and validate the selected runs,
   against the current graph. L0 may have only a newly prepended spill prefix;
   non-L0 chains must be unchanged. Prepared output attachment performs only
   constant-time endpoint checks in this reader-excluding section.
6. **Publish.** In one `GenericXLog` action, splice around any accepted L0
   prefix, replace the selected runs, rebase counts and corpus shrinkage from
   current metapage values, and attach the detached tombstone batch to the
   current pending-free head.

Published physical changes are not undone by transaction rollback.

Runtime segment and tombstone allocation must use the same
`ExtendBufferedRel(..., EB_LOCK_FIRST)` fallback as memtable growth when the
FSM has no reusable page. Mixing it with `ReadBufferExtended(P_NEW)` would let
unlocked compaction and concurrent memtable extension reserve the same block
on PostgreSQL 17.

Publication remains exclusive even though it is bounded. Scans capture level
heads but follow segment links lazily. Publishing under a shared lock could
let a scan combine an old metapage head with a rewritten L0 predecessor link
and omit documents. Under `LW_EXCLUSIVE`, scans that started before
publication finish on the old graph and later scans see the new graph.

The existing `LW_SHARED` lifetime of ranked index scans was not shortened.
Readers and inserts continue during the long build phase, but fair admission
can briefly gate new shared acquirers while publication waits. Inline
compaction is still foreground work: the write transaction that triggers it
waits for selection, build, validation, and publication to complete.

## Managed Background Compaction

Background mode requires pg_durable 0.2.8 or newer. pg_durable must be
preloaded, installed and initialized in the current database, and usable by
the index owner. The owner must have `LOGIN`; a superuser owner also requires
`pg_durable.enable_superuser_instances = on`. pg_textsearch discovers the SQL
API through extension metadata and records a normal extension dependency
after the first successful activation.

Each physical background index has one owner-scoped workflow identified by
its database, physical relation identity, owner, schedule, and protocol
version. The workflow runs a stepped cascade immediately, then waits for a
spill signal or its cron schedule. Every step calls the private
physical-target helper in a separate SQL node and transaction, releasing
PostgreSQL locks between published passes.

The startup wrapper and scheduled loop continue after SQL activity failures,
so the same workflow can retry or handle a later wake. Graph, protocol,
runtime, and infrastructure failures remain terminal. A later spill recovers
a terminal workflow for the current managed generation.

The helper revalidates the captured physical identity while holding the
relation lock. Dropped, replaced, reindexed, re-owned, or reconfigured targets
return false without touching another relation. Utility hooks reconcile
workflows after relevant `CREATE INDEX` and `ALTER INDEX` operations.

Spill requests are transaction-local and deduplicated by index. At pre-commit,
after the compaction lock is released, pg_textsearch revalidates the target,
finds or recovers its workflow, and signals that exact instance. Ordinary
signaling failures warn without aborting the writer; cancellation and shutdown
errors retain PostgreSQL's normal behavior. The periodic schedule repairs a
signal lost during a loop transition.

## Deferred Reclaim

Spill and compaction unlink old pages before they can be safely reused. Dead
memtable pages and displaced segment pages are parked with a transaction
horizon and returned to the free-space map only after
`GetOldestNonRemovableTransactionId` passes that horizon.

Query-serving hot standbys require `hot_standby_feedback = on` so their oldest
snapshots hold the primary's reclaim horizon back. Use
`bm25_pending_free_pages()` to observe displaced segment pages awaiting reuse.
As a safety fallback for a standby that disconnects while an old-graph query
remains active, tombstone drain emits the stock `XLOG_BTREE_REUSE_PAGE`
conflict-only WAL record before unlinking a reclaimable batch. Replay cancels
any conflicting standby snapshot before later WAL can reuse those pages.
Feedback therefore preserves query continuity; the conflict record preserves
storage correctness when feedback is temporarily unavailable.

Compaction constructs its tombstone containers as a detached chain whose tail
initially points to `InvalidBlockNumber`. Publication links that tail to the
then-current `pending_free_head` in the same WAL record that replaces the
segment graph. The detached pages are restamped after the long unlocked build
but before requesting the runtime publication lock. The stamp is the
compactor's assigned, still-in-progress full transaction ID, so primary and
standby snapshots that start on the old graph before publication cannot
advance the reclaim horizon past it. Restamping scales with the number of
tombstone containers but does not extend runtime reader exclusion. Selected
source pages are never returned directly to the FSM.

VACUUM segment replacement likewise assigns its current full transaction ID
before building replacement tombstones. That transaction remains in progress
through the replacement `GenericXLog` publication, preventing a later standby
snapshot from observing the old graph with a reclaim stamp that is already in
its past.

A handled error before publication returns every explicitly tracked output and
tombstone allocation to the FSM without freeing selected source pages. A
backend crash can leave unreachable pre-publication output pages; they cannot
affect queries or be mistaken for live pages and are reclaimed by `REINDEX`.
An unfinished publication `GenericXLog` state is aborted on handled errors,
and the still-unreachable prepared pages are discarded. Once
`GenericXLogFinish()` succeeds, cleanup does not recycle pages whose ownership
has transferred. Recovery exposes either the old graph with no attached batch
or the complete new graph with displaced pages reachable from the deferred-free
chain.

## VACUUM Coordination

VACUUM acquires the relation maintenance lock before identifying segment
document IDs and retains it through alive-bit mutation, legacy segment
replacement, corpus-statistic adjustment, and any segment unlink. It then
takes per-index and buffer locks in the normal order.

If VACUUM is admitted first, compaction waits and later builds from the updated
alive bits. If compaction is admitted first, VACUUM waits and then discovers
the published output before applying deaths. This prevents both resurrection
of deleted documents and mutation through stale source document IDs.
