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

An append retries if another writer has extended its candidate tail. If
rereading the metapage returns that same stale tail, the index is corrupt:
extension publishes the old tail's link and the new tail pointer atomically.
The write raises an index-corruption error with a `REINDEX` hint rather than
spinning while holding the per-index lock.

Queries compose postings from the memtable and all published segments. Each
live heap TID occurs in at most one published segment. Segment-local numeric
`doc_id` values may repeat across segments. This disjointness permits merge and
scoring paths to avoid cross-segment document deduplication.

Published-graph consumers first read and release a candidate memtable
head/tail, lock the candidate tail in share mode, then lock the metapage in
share mode and validate that the candidate is still current. On mismatch they
release metapage then tail and retry. With tail then metapage held, they copy
every segment root and the bounded memtable endpoint, including the tail free
offset. Empty chains validate both pointers without a tail lock. Level counts
bound and validate root discovery.
Query, debug, and maintenance work then use the explicit root arrays rather
than lazily following published `next_segment` links. On recovery, ranked,
standalone, and Boolean scoring consume the bounded chain endpoint instead of
rereading the metapage. Segment and memtable inputs therefore come from one
complete old or new generation while Generic WAL replay changes publication.
Ranked scoring pins recovery mode before acquiring the snapshot, so promotion
during a pause cannot switch that generation to the primary cache path.
Ranked scans and standalone score expressions share one captured generation
per physical index and executor, including larger-batch retries and cursor
fetches. Corpus totals and term frequencies therefore stay consistent within
that execution; a later statement captures fresh statistics. These are physical
index statistics, not MVCC-filtered corpus statistics.
Snapshots also identify the physical relation file, so a same-backend
`REINDEX` or `TRUNCATE` captures a new generation rather than reusing old
block numbers.

## Parallel Index Build

Parallel build workers scan disjoint heap ranges and write current-format
segments as flat temporary `BufFile` streams. Their TID-range scans enable
the table AM's bulk-read strategy, allowing large heap scans to reuse a
buffer ring rather than fill shared buffers alongside the build batches.
Synchronized scanning remains disabled to preserve the disjoint ranges.
These private worker segments contain only live documents; an absent
alive-bitset section represents that all-live state. Empty-token documents
still occupy the fieldnorm and CTID
sections and contribute to corpus totals even when a worker segment has no
dictionary terms.

Each worker appends its segment directory to its temporary stream and reports
only the directory offset and count through shared memory. Small build budgets
can therefore produce more than 64 segments per worker without exhausting a
fixed shared-memory reporting array.

After all workers finish, the leader publishes one final L0 segment. It merges
source dictionaries one term at a time, using bounded windows for worker
string offsets and temporary streams for string metadata, exact source
references, skip entries, and dictionary-entry backpatch data. Posting blocks
are read from worker segments and written directly to the normal WAL-logged
segment writer; complete output postings are never spooled or reread.

Worker inputs are ordered and disjoint, so the leader assigns each source a
document-ID base and concatenates its fieldnorm and split CTID sections in the
same order. It emits the all-live bitmap incrementally. This avoids
vocabulary-sized merged-term state and corpus-sized document remapping arrays;
leader working memory is bounded by active inputs, posting blocks, offset
windows, and fixed-size copy buffers. The output keeps the ordinary segment
layout, compression, page index, dictionary backpatch, and publication order.

Worker batch flushes use the bytes allocated by the batch's dedicated memory
context, including the arena, dynahash, and document arrays. The worker
threshold also reserves conservative serialization scratch for sorted terms,
string offsets, term and dictionary metadata, the geometrically grown skip
array, temporary CTID arrays, and the alive bitmap. Parallel builds divide the
total `maintenance_work_mem` batch budget by the workers actually launched. A
worker budget that cannot hold the initial context, one 1 MiB arena page, and
minimum serialization scratch is rejected explicitly rather than being raised
by a hidden per-worker floor.

Serial builds retain their established arena-payload and document-capacity
flush estimate, preserving their batching and inline-compaction behavior.
Correcting serial dynahash and serialization accounting is deferred until it
can be paired with a serial merge strategy that does not regress build time or
peak memory.

The worker batch estimate is not a hard backend RSS limit. PostgreSQL executor
and backend overhead, temporary-file buffers, and the resettable tokenization
of one document are outside it. A single document can therefore overshoot the
worker threshold before the following flush check.

A primary reader uses the memtable cache only when its physical relation file
and applied endpoint match the captured chain. It holds the cache apply lock
in shared mode for that scoring call to prevent catch-up from changing the
view. Otherwise it reads the bounded on-disk chain. Reads discard caches from
rolled-back relation files even when the restored chain is empty.
Readers of an already matching cache share that lock without exclusive
catch-up admission. The first ranked source also supplies the snapshot's
corpus totals, avoiding a separate memtable walk.
No LWLock is held between rows or cursor fetches; the executor's heap
snapshot protects retired pages from
reclaim. Executor memory-context cleanup releases the captured generations,
including on errors and nested execution.

## Memtable Cache

The shared-memory memtable cache accelerates reads but is derived and
disposable. Writes update only the on-disk chain. Readers lazily build or catch
up the cache; generation mismatches, spills, eviction, or memory pressure can
drop it without affecting correctness. Standbys read the on-disk chain.

Registry entries are keyed by database and index OID. Rebuilds preserve the
shared allocation and its locks so existing backends retain valid wrappers.
A successful build clears the cache in place and advances its generation.
Cache cursors also record the relation's physical file identity: REINDEX,
TRUNCATE, and rollback must never resume a cursor in a replacement file.
Chain-page counts are lazily recounted against that same file identity
before spill threshold checks; shutdown spills skip a busy recount lock.

Index drops free registry state only at commit, before relation locks are
released. Transaction or savepoint rollback preserves allocations referenced
by other backends; releasing a savepoint transfers pending cleanup to its
parent. `PREPARE TRANSACTION` rejects pending index-drop cleanup, as it does
initial index creation, because this ownership is backend-local.
Concurrent drops retain that state across PostgreSQL's intermediate commits
and reader waits. Only successful completion of the utility command schedules
cleanup for its final transaction; cancellation preserves the surviving
index's state.

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

Memtable writers also use a fair per-index writer/spill gate. Appends take it
in shared mode before the per-index LWLock; spill takes it exclusively so the
published chain stays stable while its segment is built. Ranked readers do not
take this gate.

Compaction, force merge, and VACUUM segment mutation serialize with an
exclusive per-index heavyweight object lock in pg_textsearch's private
`pg_am` subobject namespace. Its discriminator is distinct from managed
background-compaction admission, so a signaled worker can compact while the
spilling transaction is still completing pre-commit dispatch. The lock does
not exclude scans, memtable appends, or ordinary relation locks.

Graph publication also uses a private per-index publication barrier. Runtime
compaction and VACUUM replacement acquire maintenance, then the publication
barrier in `ExclusiveLock`, then phase-specific per-index LWLocks. Spill
publication does not use maintenance; it acquires the publication barrier in
`ShareLock`, the writer/spill gate in exclusive mode, and then phase-specific
per-index LWLocks. No path acquires the publication barrier, maintenance, or
writer/spill gate while holding the per-index LWLock.

Buffer ordering then follows the storage operation. Existing-tail memtable
extension is tail -> new page -> metapage. The common read snapshot uses
tail -> metapage after an unlocked candidate read and retry validation; it
never nests metapage -> tail. Segment and tombstone publication retain their
own validated buffer order under the per-index lock. Empty-chain bootstrap is
the only memtable path with no existing tail and locks metapage before its new
unpublished page.

No path may request maintenance while holding the per-index lock. A spill
therefore completes L0 publication and releases `LW_EXCLUSIVE` before applying
the configured compaction policy.

### Spill phases

A runtime spill holds the publication barrier and writer/spill gate across
three phases:

1. **Freeze and extract.** Briefly take the per-index lock in
   `LW_EXCLUSIVE`, recheck the threshold, and extract the stable memtable
   chain.
2. **Build.** Release the per-index lock and write the complete WAL-logged but
   unreachable L0 segment. Readers continue to scan the unchanged chain;
   writers wait on the writer/spill gate.
3. **Publish.** Briefly retake `LW_EXCLUSIVE`, publish the segment, disconnect
   and stamp the old chain for deferred reclaim, and advance the cache spill
   generation.

An error before publication discards the unreachable segment and leaves the
old chain published. Ownership transfers at WAL publication, before cache
cleanup or chain retirement; a later error must not discard the live segment.
Shutdown's no-wait spill skips busy publication, writer-gate, and per-index
locks. If the publication-phase index lock is busy, it discards the unpublished
output. Buffer, WAL, and I/O work may still wait after admission.

The `compaction` index option controls spill-time behavior:

- `inline` compacts eligible debt during spills, serial VACUUM, and index
  builds. It never
  waits for maintenance admission or exclusive index access: when another
  session is using or maintaining the index -- a concurrent `REINDEX INDEX
  CONCURRENTLY`, `VACUUM`, or explicit compaction -- the pass is skipped
  rather than blocking the writer, and the debt is picked up by the next
  spill or by explicit maintenance;
- `background` dispatches a pre-commit request when possible. Runtime
  no-dispatch contexts such as autovacuum and callback re-entry compact
  inline. Index builds leave compaction to the managed workflow after
  activation, and temporary indexes do not support this mode;
- `manual` leaves debt for explicit maintenance. The legacy `off` value is
  accepted as an alias for `manual`.

Prepared transactions do not flush queued background requests. Unconfigured,
unresolvable, or failed callbacks do not fall back inline; the compaction debt
remains for a later spill or explicit maintenance.

A level 0 that is already at `pg_textsearch.segments_per_level` blocks the
spill outright, so `inline` and `background` indexes both compact it in the
spilling process; a background worker's pass would come too late. If that
compaction is skipped because maintenance is busy, the records stay in the
durable memtable chain for a later spill to drain. If it runs but cannot
reduce level 0 -- every segment is over `max_segment_size` -- the spill fails
closed with a segment-count error rather than growing the chain without
bound. `manual` indexes take that error directly.

Each visible automatic invocation performs at most one bounded pass. Each
pass uses brief per-index `LW_SHARED` selection, no per-index lock during
output build, and fair `LW_EXCLUSIVE` validation and publication.
`bm25_compact()` drives reducible debt to completion, taking the per-index
maintenance lock for each pass and releasing it in between so a queued
VACUUM is not starved by a long cascade. Each pass uses brief per-index
`LW_SHARED` selection, no per-index lock during output build, and fair
`LW_EXCLUSIVE` validation and publication. `bm25_compact_step()` runs at most
one pass. A private `CREATE INDEX` build may drain compaction debt before the
index becomes visible. Both, like `bm25_force_merge()`, wait for index
maintenance: the lock is private to this extension, so waiting cannot close a
deadlock cycle with a concurrent `REINDEX INDEX CONCURRENTLY`. A dispatched
background worker instead defers to its next scheduled run.
`bm25_needs_compaction()` reports whether `bm25_compact_step()` would run a
pass: it runs the same selection without publishing, so a level that is full
of over-budget segments reports false and is safe to loop on. It tries the
maintenance lock instead of waiting for it, and reports true when another
session holds it.

### Consolidation policy

Count-triggered compaction remains available at `segments_per_level`.
Merged outputs use their estimated surviving size for placement, with L1 as
the minimum destination; small outputs no longer climb a level on every
merge. Capacity-driven singleton promotion remains an exception.

Below the count threshold, a segment with at least 50% dead documents can
be rewritten alone. This also works inside a level's chain, retaining its
level so publication needs only one predecessor splice. Completely dead
segments retain their existing cleanup path.

Otherwise, compaction considers head prefixes across levels whose estimated
surviving sizes are within a factor of two of the first source. A pass
combines at most `segments_per_level` sources into one bounded output.
Incompatible heads are not rewritten merely to reach smaller segments
behind them. This prevents each tiny spill from rewriting a large output.
Surviving-size estimates guide selection and placement only: the conservative
document, dictionary, and posting estimates still enforce merge-size and
format bounds. An existing over-budget singleton can be rewritten for
deletion cleanup, but cannot join a multi-source merge.

Explicit maintenance, inline policy, and managed workers use the same planner.
Background spills enqueue a request without running selection in the writer;
the worker determines whether any pass is eligible. Policy scans read only
segment headers, loading page maps and dictionaries for selected candidates.
Serial VACUUM invokes the configured policy after cleanup, so partial
deletion needs no subsequent spill to become eligible. Manual mode leaves
nonempty segments for explicit maintenance. A scheduled worker also discovers
below-threshold debt without a new signal.

### Compaction phases

Each runtime pass uses the same phase engine:

1. **Maintenance admission.** Acquire the per-index maintenance lock. A caller
   that waited rechecks compaction debt after admission.
2. **Select.** Briefly take the per-index lock in `LW_SHARED` and copy the
   metapage. Release it before the maintenance-protected source walk records
   exact source roots, contiguous runs, and retained remainders.
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
5. **Prepare and validate.** Acquire the publication barrier in
   `ExclusiveLock`, then under `LW_SHARED` validate the selected runs and
   prepare any L0 prefix added by spills that completed before barrier
   admission. Release the shared lock, then request fair `LW_EXCLUSIVE`.
   Because later spills cannot publish while the barrier is held, an identity
   mismatch fails closed as an internal invariant violation rather than
   retrying preparation.
6. **Publish.** In one `GenericXLog` action, splice around any accepted L0
   prefix, replace the selected runs, rebase counts and corpus shrinkage from
   current metapage values, and attach the detached tombstone batch to the
   current pending-free head. Exclusive work is limited to constant-time
   identity, predecessor, and detached-tail checks plus publication.

Published physical changes are not undone by transaction rollback.

Runtime segment and tombstone allocation must use the same
`ExtendBufferedRel(..., EB_LOCK_FIRST)` fallback as memtable growth when the
FSM has no reusable page. Mixing it with `ReadBufferExtended(P_NEW)` would let
unlocked compaction and concurrent memtable extension reserve the same block
on PostgreSQL 17.

Publication remains exclusive even though it is bounded. Primary scans that
started before publication finish from their copied old roots; later scans
copy the new roots. Standby discovery holds the metapage buffer share lock, so
replay cannot expose a metapage/link mixture while roots are being copied.

The existing `LW_SHARED` lifetime of ranked index scans was not shortened.
Readers and inserts continue during the long build phase, but fair admission
can briefly gate new shared acquirers while publication waits. Inline
compaction is still foreground work: the write transaction that triggers it
waits for one selection, build, validation, and publication pass to complete.
Output and tombstone pages are WAL-logged before the later reachability record;
WAL insertion order provides durability without `FlushRelationBuffers()`.

## Managed Background Compaction

Background mode requires pg_durable 0.2.8 or newer. pg_durable must be
preloaded, installed and initialized in the current database, and usable by
the index owner. The owner must have `LOGIN`; a superuser owner also requires
`pg_durable.enable_superuser_instances = on`. pg_textsearch discovers the SQL
API through extension metadata and records a normal extension dependency
after the first successful activation.

The pg_durable extension and BM25 index must be in the same database.
pg_durable's `database` argument routes SQL activities after submission; it
does not expose `df.start`, `df.signal`, or workflow metadata in another
database. pg_textsearch rejects background mode when `pg_durable.database`
names a different database. Use `manual` mode there.

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
As a safety fallback for a standby that disconnects while an old-generation
query remains active, both tombstone drain and DEAD-memtable reclaim emit the
stock `XLOG_BTREE_REUSE_PAGE` conflict-only WAL record before reuse. Memtable
reclaim uses each page's `dead_fxid`; tombstone drain uses the batch horizon.
Spill samples `dead_fxid` only after the WAL record that unpublishes the old
chain, so every standby snapshot that can still discover that chain is covered.
Replay cancels any conflicting standby snapshot before later WAL can reuse
those pages.
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

Tombstone drain unlinks a reclaimable batch under `LW_EXCLUSIVE`, then releases
the per-index lock before stamping its already-unreachable pages free and
returning them to the FSM. Free-before-unlink is forbidden; unlink-before-free
is safe, and an error during the unlocked free loop can only leak the
unfinished remainder until `REINDEX`.

Tombstone walks reject metapage and past-EOF links before reading them.
The drain warns and detaches the corrupt tail, preserving any valid prefix;
`REINDEX` reclaims the leaked pages. `bm25_pending_free_pages()` instead
raises a corruption error without changing the chain or returning a partial
count.

Metapage V8 already contains `pending_free_head`; compaction preserves that
existing chain when upgrading and publishing. Only older metapage versions
synthesize an empty pending-free head.

Serial VACUUM segment replacement assigns its current full transaction ID
before building replacement tombstones. That transaction remains in progress
through the replacement `GenericXLog` publication, preventing a later standby
snapshot from observing the old graph with a reclaim stamp that is already in
its past.

PostgreSQL can invoke index bulk-delete in a parallel worker or in a leader
that has already entered parallel mode, where assigning an XID is forbidden.
In that context VACUUM still persists V5 alive-bit changes, including an
all-zero bitmap, but leaves an empty segment physically linked for later
serial compaction. A VACUUM-triggered spill remains published while its
compaction policy is deferred. For affected legacy segments, parallel VACUUM
builds detached tombstones with a provisional invalid stamp before
publication. Under the publication barrier and `LW_EXCLUSIVE`, one
`GenericXLog` action replaces the graph and attaches that provisional batch
atomically. Only after the publication WAL record is inserted does VACUUM
sample `ReadNextFullTransactionId()` and WAL-restamp the attached containers.
WAL order protects standby readers that can still see the old graph. Invalid
stamps are unreclaimable; after a crash, a later tombstone drain activates
them with a new post-publication stamp before any reuse.

A handled error before publication returns every explicitly tracked output and
tombstone allocation to the FSM without freeing selected source pages. A
backend crash can leave unreachable pre-publication output pages; they cannot
affect queries or be mistaken for live pages and are reclaimed by `REINDEX`.
Force-merge relation truncation does not reclaim those orphans by inference:
it removes only a contiguous EOF suffix whose pages already carry
`TP_FREE_PAGE_MAGIC`.
An unfinished publication `GenericXLog` state is aborted on handled errors,
and the still-unreachable prepared pages are discarded. Once
`GenericXLogFinish()` succeeds, cleanup does not recycle pages whose ownership
has transferred. Recovery exposes either the old graph with no attached batch
or the complete new graph with displaced pages reachable from the deferred-free
chain.

## VACUUM Coordination

VACUUM uses separate maintenance critical sections for bulk deletion and
cleanup. The bulk-delete section covers segment document identification,
alive-bit mutation, legacy replacement, and corpus-statistic adjustment. The
cleanup section covers segment unlink and the full-fork scan that reclaims
DEAD memtable pages, excluding force-merge truncation during that scan. Other
maintenance may run between the two callbacks. The per-index LWLock is held
only for root snapshots and brief validated publication; document
identification, bitmap work, and dead-memtable reclaim remain unlocked from
readers, inserts, and spills.

When legacy and current segments disagree with the metapage token total,
VACUUM verifies each current segment by summing its posting frequencies before
attributing the residual to legacy headers. A current-format mismatch fails
closed with a REINDEX hint before any replacement is published.

Dead-memtable reclaim first records the currently reachable chain, then
inspects pages under their buffer locks. A racing spill can only make that
reachable set conservative, retaining pages until a later VACUUM. Live and
newly allocated pages are not marked DEAD, while `dead_fxid` prevents a
retired page from entering the FSM before old primary or feedback-protected
standby snapshots are safe. Before each reclaimed page is free-stamped, stock
conflict-only WAL protects disconnected or no-feedback standby readers.

`bm25_force_merge()` may shrink the physical relation only across consecutive
EOF pages already stamped `TP_FREE_PAGE_MAGIC`. The stamp proves normal
reclaim completed: the page is detached from every owning structure and any
required standby conflict WAL was inserted before it entered the FSM. A DEAD
memtable page, a valid structural page, an unknown page, or an unreachable
orphan stops truncation even when no current graph edge references it.

If VACUUM is admitted first, compaction waits and later builds from the updated
alive bits. If compaction is admitted first, VACUUM waits and then discovers
the published output before applying deaths. This prevents both resurrection
of deleted documents and mutation through stale source document IDs.

Parallel VACUUM can persist a zero-alive bitmap but cannot assign the reclaim
XID needed to unlink it. A later serial VACUUM or ordinary compaction therefore
prioritizes removal of zero-alive segments even below the normal compaction
threshold, including maintenance rounds with no newly reported dead TIDs.
