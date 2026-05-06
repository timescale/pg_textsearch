# Replica bootstrap correctness

This doc specifies what the standby's bootstrap path
(`tp_rebuild_index_from_disk`) is supposed to do, in terms of
invariants over the resulting in-memory state, and verifies the
current code against those invariants across the scenarios it can
encounter.

The aim is for someone reviewing the bootstrap code or evaluating
a proposed change to start here and only then dive into
`src/index/state.c`.

## Setup

Two machines:

- **Primary**: accepts writes.
- **Replica**: read-only, follows the primary by streaming WAL.

Each holds the index split into:

- An **in-memory part**: recent docs. This part is non-durable;
  it is lost on crash or restart and rebuilt by the bootstrap
  path on the first backend access after the restart.
- **On-disk parts**: older docs in immutable segments, plus an
  on-disk list of which heap row pointers belong to the
  in-memory part (the "docid pages"). The list lets the
  bootstrap path rebuild the in-memory part by walking it and
  re-tokenizing the heap rows it points at.

In normal write traffic the in-memory part and the on-disk list
move together: every insert appends to both, and every spill
clears both (writing the displaced docs into a new segment). The
two can diverge in two situations:

- **Between server restart and first backend access.** Server
  restart wipes the in-memory part; the docid pages survive on
  disk. Until the first backend opens the index and bootstrap
  runs, the in-memory part is empty and the docid pages are
  populated.

- **After bootstrap drops ghost entries.** The bootstrap path
  re-tokenizes from heap. If a docid-page entry's heap row has
  been physically removed (HOT-pruned, vacuumed, heap pages
  truncated) the heap fetch returns nothing and bootstrap skips
  the entry. The in-memory part then permanently lacks that
  pointer until the next spill rewrites the segment from heap
  (which would also drop the ghost).

## What the index records

The index is a record of which row pointers have been inserted, not
which heap rows are currently live. On the primary, both the
in-memory part and the on-disk list can carry "ghost" entries:
pointers whose heap row is no longer live. There are two ways
this happens.

- **Insert-then-abort.** The insert path adds terms to the
  in-memory part and a pointer to the on-disk list, then the
  transaction aborts. Neither addition is rolled back. The heap
  row is marked aborted, and scan-time visibility filtering
  hides it from query output.
- **Delete (and update).** `DELETE` and the old half of an
  indexed-column `UPDATE` mark the heap row dead but do not
  touch the in-memory part or the on-disk list. The pointer and
  its term postings stay until a VACUUM-driven segment merge or
  rebuild rewrites the segment from the heap.

Spill drops both kinds of ghost at segment-build time: it
re-tokenizes from the heap, sees the dead row, and skips it.
VACUUM drops them at segment-merge time via metapage and
alive-bitset shrinkage.

## Invariants

The invariants below describe the replica after bootstrap
completes, compared against the primary at the corresponding log
position. By "corresponding log position", the doc means the
LSN that replay had reached when bootstrap took its exclusive
lock. The
replica is always some distance behind the primary; the invariants
are about consistency between the replica's bootstrapped state and
the primary's state at that lower LSN.

### Ghost entries: what the bootstrap drops

The bootstrap walks the on-disk list and re-tokenizes from the
heap via `heap_fetch(SnapshotAny, …)`. Aborted or dead heap
tuples that are still physically present are returned and
re-indexed normally. Tuples that have been physically removed
(HOT-pruned, vacuumed, heap pages truncated) are not returned, so
bootstrap skips them. Both insert-abort ghosts and delete/update
ghosts can end up in this dropped category once VACUUM has
cleaned the heap. The result on the replica:

- `docid_pages_replica` ≡ `docid_pages_primary`. Exact bytes,
  via standard buffer replay of the docid-page WAL.
- `memtable_replica` ⊆ `docid_pages_replica`. Entries dropped
  at bootstrap have no memtable presence.
- `memtable_replica` ⊆ `memtable_primary`. Same reason: the
  primary retains the ghost, while the replica may not.

These asymmetries do not affect live-query correctness: a
dropped ghost cannot yield search results because the heap row
that backs it is gone, and scan-time visibility filtering would
reject it anyway. They do cascade to BM25 corpus statistics.
Dropped ghosts give the replica a smaller `total_docs`, which
shifts IDF for any term that appeared only in those ghost rows,
which in turn shifts ranking scores on the replica relative to
the primary. The drift is bounded to terms that appeared only in
physically-removed ghost rows, and it self-heals at the next
spill, which rewrites the segment from the heap on both machines
and drops the ghost on each.

### The invariants

**Inv 1: replica state is internally well-formed.** Every row
pointer in `memtable_replica` appears in `docid_pages_replica`.
Every term posting referencing a CTID has a matching entry in
the doc-length table. The reverse direction (every docid-page
entry has a memtable entry) holds *except* for ghost entries
whose heap row was physically removed before bootstrap.

**Inv 2: counters reflect what is actually on the replica.**
`shared->total_docs == sum(segment.num_docs) + |memtable_replica|`,
and the same identity holds for `total_len`. The numbers BM25
ranking reads describe the replica's actual contents, and they
may differ from `shared->total_docs` on the primary by the
count of dropped ghosts.

**Inv 3: each row pointer is counted once on the replica.** No
pointer appears in two segments, or in both a segment and the
in-memory part. The counters in Inv 2 do not double-count.

**Inv 4: live docs are findable on the replica.** Every doc the
primary's index can return as a live result at the corresponding
log position can also be returned by the replica. The relevant
boundary is heap visibility, not "every CTID the primary has
indexed". Dropped ghosts cannot return results from the primary
either, so the replica losing them does not violate this
invariant.

**Inv 5: forward replay works.** After bootstrap, applying
further WAL leaves Inv 1–4 holding at the new log position.

## How the current code achieves this

`tp_rebuild_index_from_disk`, in order:

1. **Open the index relation, read the metapage, validate
   magic.** The magic word is set at index creation and never
   changes, so this check is stable across replay. The
   validation runs before the function registers any shared
   state, so a corrupt metapage cannot leak a registry entry.

2. **Register shared state.** Custom-rmgr redo
   (`INSERT_TERMS` / `SPILL` / `MERGE_LINKAGE`) looks up the
   per-index registry entry by OID. If no entry exists, the
   handler returns without applying its in-memory effect: the
   memtable update for INSERT_TERMS, the memtable clear for
   SPILL, or the atomic shrinkage of `shared->total_docs` and
   `shared->total_len` for MERGE_LINKAGE. The on-disk side of
   each record still applies through standard Postgres buffer
   replay (registered buffers for SPILL and MERGE_LINKAGE, and a
   separately-emitted paired `GenericXLog` record for
   INSERT_TERMS's docid-page write), so the on-disk index
   continues to move forward regardless of whether any backend
   has populated this replica's registry.

   Bootstrap registers the entry up front so that subsequent
   custom-rmgr redo applies its in-memory effect to the
   newly-registered state. The two corpus-stat atomics on the
   new entry are `shared->total_docs` and `shared->total_len`
   (the values BM25 ranking reads). Both start at zero. They
   are not authoritative until step 7 writes the ground-truth
   values under the per-index exclusive lock.

3. **Drain WAL replay.** The bootstrap snapshots the
   wal-receiver's flush LSN (the highest LSN flushed to the
   local WAL by the receiver) and blocks until the replay LSN
   catches up to it. Past this point, every record the primary
   had emitted up to that snapshot has been applied to the
   replica's on-disk state. The drain is a no-op when the
   process is not in recovery (for example, on a post-promote
   PITR cluster or on a primary that has finished crash
   recovery).

4. **Acquire the per-index lock in exclusive mode.** This
   acquisition serves three purposes.

   - The custom-rmgr redo handlers (`INSERT_TERMS`, `SPILL`,
     `MERGE_LINKAGE`) take this same lock when they apply their
     in-memory effect, so they block while the bootstrap holds
     the lock exclusive. WAL replay is single-threaded, which
     means that once the first such record after the
     acquisition blocks, every later record in the WAL stream
     stalls behind
     it until the bootstrap releases. The in-memory state
     therefore cannot change between when the bootstrap re-reads
     the metapage in step 5 and when it writes counters in
     step 7.

   - Backend scans take this lock in shared mode and block
     until the bootstrap releases, so no scan ever observes the
     half-rebuilt in-memory state during the walk.

   - The walk in step 6 calls
     `tp_ensure_string_table_initialized`, which creates the
     in-memory dshash tables on first use. Its contract
     requires the per-index exclusive lock so that cold-path
     table creation cannot race with the shared-lock hot path
     in `tp_get_or_create_posting_list`.

   `GenericXLog` redo does not take the per-index lock, so any
   `GenericXLog` records the WAL stream has already reached
   (those queued before the next custom-rmgr record) can still
   apply during the hold. This is harmless: the on-disk fields
   those records touch are either ones the bootstrap uses only
   via the snapshot taken in step 5, or ones the bootstrap does
   not read. The "Why GenericXLog redo during the exclusive
   hold is harmless" section below enumerates the cases.

5. **Re-read the metapage under the lock.** `tp_get_metapage`
   takes the metapage buffer in share mode, copies the entire
   metapage struct into a palloc'd snapshot, and releases the
   buffer. From that snapshot the rest of the function uses
   four fields:

   - `total_docs` and `total_len`, the segment-side corpus
     totals, which contribute to the ground-truth values
     written to the in-memory atomics in step 7.
   - `first_docid_page`, the head of the docid-page list, which
     the walk in step 6 follows from here.
   - `level_heads[0]`, the head of the segment chain at
     level 0, which the empty-index decision in step 6 reads.

   The snapshot is taken by value, so any subsequent on-disk
   changes to the metapage (which are possible during the
   exclusive hold; see step 4) cannot mutate the values the
   rest of the function uses.

6. **Pick a branch.** Three cases:

   - **Heap was TRUNCATEd while the index has data.** The heap
     relation's block count is read fresh under the lock; if
     `heap_blocks == 0 && fresh_metap.total_docs > 0`, log a
     warning and return the registered (empty) shared state.
     The on-disk index references rows that no longer exist;
     the primary is in the same stale state, so matching it is
     correct.

   - **Empty index.** If the snapshot has
     `total_docs == 0`, `first_docid_page == Invalid`, and
     `level_heads[0] == Invalid`, there is nothing to walk.
     Return the registered (empty) shared state.

   - **Otherwise.** Walk the on-disk list, re-tokenize each
     heap row, and add it to the in-memory part. The add path
     is idempotent by row pointer (`tp_store_document_length`
     is the check-and-add gate), so any entries that custom-rmgr
     redo already added during the drain in step 3 are not
     double-added by the walk.

7. **Compute and write the corpus counters.** The bootstrap
   walks the doc-length table to count the docs currently in
   the in-memory part:

   ```c
   tp_doclength_summary(local_state, &count, &sum_len);
   pg_atomic_write_u32(&shared->total_docs,
                       fresh_metap.total_docs + count);
   pg_atomic_write_u64(&shared->total_len,
                       fresh_metap.total_len + sum_len);
   ```

   The two halves are disjoint by construction: a spill moves
   docs out of the in-memory part into a new segment, and it
   clears the in-memory part before persisting the new segment
   totals. So at any moment under the per-index lock, every doc
   is in exactly one of the two halves. The sum of
   `fresh_metap.total_docs` (a snapshot of the segment-side
   total) and `count` (the current in-memory count) is
   therefore the absolute total at the moment the bootstrap
   holds the lock.

   This step uses `pg_atomic_write`, not `fetch_add`. The
   atomic might have been bumped by custom-rmgr redo during
   the drain in step 3, and some of those increments may
   reflect docs that were subsequently promoted into a segment
   by a later spill in the same drain window. Those docs would
   then also be counted in `fresh_metap.total_docs`, so a
   `fetch_add` would count the same cohort twice. Writing the
   absolute value avoids the ambiguity, because each half
   contributes its own disjoint share of the total.

8. **Release the lock and close the index relation.** The
   shared state is now fully populated and visible to other
   backends.

## Scenario × invariant matrix

For each scenario the table traces what bootstrap does and
checks Inv 1–5.

| # | Scenario | Behavior | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|---|
| A | Brand-new index, nothing replayed | Register → drain no-op → fresh metapage shows total=0/list=empty/no segments → empty-index branch returns registered empty state | ✓ | ✓ | ✓ | ✓ | ✓ |
| B | Brand-new index, primary did N inserts (no spill) | Register → drain catches up → fresh metapage shows list non-empty → walk fills in-memory part → `total_docs = 0 + N = N` | ✓ | ✓ | ✓ | ✓ | ✓ |
| C | Post-spill, no inserts since | Fresh metapage shows total=N/list=empty/segments present → walk returns 0 → `total_docs = N + 0 = N` | ✓ | ✓ | ✓ | ✓ | ✓ |
| D | Post-spill + M new in-memory inserts | Walk fills remainder → `total_docs = N + M` | ✓ | ✓ | ✓ | ✓ | ✓ |
| E1 | Spill replays fully before bootstrap takes the lock | Indistinguishable from C on entry to the exclusive section | ✓ | ✓ | ✓ | ✓ | ✓ |
| E2 | Spill's GenericXLog pieces (segment data, list clear, stat sync) replay during the drain or hold; SPILL custom record blocks behind the bootstrap's lock | Fresh metapage may reflect any subset of these. atomic_write under the lock uses metapage snapshot plus dshash walk, both disjoint cohorts. Result is correct regardless of what intermediate state the bootstrap caught. | ✓ | ✓ | ✓ | ✓ | ✓ |
| F | Primary crash recovery | No backends during recovery, so bootstrap fires only post-recovery. Drain is no-op. Otherwise C/D. | ✓ | ✓ | ✓ | ✓ | ✓ |
| G | Heap TRUNCATE'd while index has data | Heap-empty branch logs warning, returns registered empty state. Primary is in the same stale state, so a replica that matches it is correct. | ✓ | ✓ | ✓ | ✓ | ✓ |
| H | PITR mid-burst (issue #350) | Post-promote, drain is no-op. metapage `total_docs` is from last spill before target (could be 0). Walk recovers M docs. `total_docs = small + M`. | ✓ | ✓ | ✓ | ✓ | ✓ |
| I | Merge replays during bootstrap | MERGE_LINKAGE custom record blocks at the bootstrap's lock. Pre-merge GenericXLog (segment data writes) can replay during drain or hold without affecting the snapshot. After the bootstrap releases, MERGE_LINKAGE applies its atomic shrinkage to whatever value step 7 wrote, yielding `post_merge_metap.total + memtable_count`. | ✓ | ✓ | ✓ | ✓ | ✓ |

## Why GenericXLog redo during the exclusive hold is harmless

Step 4 explained the blocking: while bootstrap holds the
exclusive lock, custom-rmgr redo blocks, and sequential WAL
replay halts at the first blocked record, so the only redo that
can advance is GenericXLog records queued before it. The table
below enumerates those records and shows that none of them
disturbs bootstrap's snapshot or its in-memory state.

| Source | What it modifies | Effect on bootstrap |
|---|---|---|
| Docid-page WAL (in tp_insert) | Appends a row pointer to the on-disk list. | Always queued *after* a paired INSERT_TERMS, so if INSERT_TERMS is blocked at the per-index lock, this record cannot apply either. |
| Segment data WAL (during spill) | Writes new segment pages. | New pages aren't referenced by metap until SPILL replays. Invisible to scans, harmless. |
| `tp_clear_docid_pages` (during spill) | Clears `metap.first_docid_page` on disk. | The bootstrap reads from the metapage snapshot taken under the lock, so the on-disk change does not affect it. |
| `tp_sync_metapage_stats` (during spill) | Writes `metap.total_docs/total_len` on disk. | Same as above: the bootstrap reads the snapshot, not the live on-disk metapage. |
| Segment data WAL (during merge) | Writes new segment pages. | Same as spill. |
| Segment alive-bitset updates (VACUUM bulk-delete) | Modifies segment header alive-bitset. | We don't read alive-bitsets at bootstrap time. Harmless. |

## Why atomic_write, not fetch_add

Concrete scenario where `fetch_add` would have double-counted:

- Primary timeline: I1..I50, I51..I100, SPILL, I101..I150.
- Replica registers shared state somewhere between I1..I50 and
  I51..I100. Drain begins.
- During drain on the replica: I51..I100 INSERT_TERMS replay
  against the registered state (atomic += 50). SPILL's
  `tp_sync_metapage_stats` GenericXLog applies (`metap.total_docs`
  set to primary's atomic at that moment = 100). SPILL custom
  record applies (memtable cleared, atomic untouched).
  I101..I150 INSERT_TERMS replay (atomic = 50). Docid pages now
  hold 50 entries (post-spill memtable docs).
- Drain ends. Acquire exclusive. fresh_metap.total_docs = 100.
  Walk the docid-page list. The idempotency gate skips all 50
  entries (already added by INSERT_TERMS replay), and
  `tp_doclength_summary` returns count=50.
- With `fetch_add`: `atomic = 50 + 100 = 150`... but the I51..I100
  cohort was already counted via INSERT_TERMS replay AND included
  in `metap.total_docs=100` via stat sync, so atomic ends up at
  150 + 50 = 200. Wrong by 50.
- With `atomic_write`: `atomic = 100 + 50 = 150`. Matches the
  primary's atomic at the corresponding log position.

## Known limitations

- **Mid-walk error.** If the walk loop errors out (a
  corruption-driven `heap_fetch` PANIC, an expression-eval
  failure, etc.), Postgres's exception handler releases the
  LWLock, but the in-memory part is left partially populated.
  The next backend that opens the index bypasses bootstrap
  (the registry has an entry) and observes the partial state.

The ghost-driven asymmetries between primary and replica
covered by Inv 1 and Inv 2 are not limitations: they are
properties of the design. The resulting BM25 score drift
matters only if primary↔replica score parity is a hard contract
rather than best-effort, which is not currently the case.

## Why it works

The bootstrap takes the per-index exclusive lock, snapshots the
metapage and the doc-length dshash under that lock, and writes
`total = metap_snapshot + memtable_count` as ground truth. The
exclusive lock blocks any custom-rmgr replay that could change
the index's "inserted" view, so the only redo that can advance
during the hold is GenericXLog records. Those records touch
either metapage fields the bootstrap reads only via the
snapshot, or segment data the bootstrap does not read, or
docid pages that cannot apply because their paired INSERT_TERMS
is blocked at the lock.

Concurrent bootstrap from two backends is safe for a related
reason: `tp_create_shared_index_state` uses an atomic
register-or-attach primitive (`tp_registry_register_if_absent`)
in its bootstrap mode, so two backends arriving at the same
empty registry resolve to the same shared state instead of
unregistering each other. The second arrival's bootstrap still
runs through the drain, the exclusive acquisition, and the
walk, but the idempotency gate deduplicates the entries and the
final atomic write produces the same answer in both backends.
