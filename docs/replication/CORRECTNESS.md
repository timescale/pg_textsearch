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

- **Primary** — accepts writes.
- **Replica** — read-only, follows the primary by streaming WAL.

Each holds the index split into:

- An **in-memory part** — recent docs.
- **On-disk parts** — older docs in immutable segments, plus an
  on-disk list of which heap row pointers are in the in-memory
  part (the "docid pages"). The list lets a fresh backend rebuild
  the in-memory part by walking the list and re-tokenizing the
  heap rows it points at.

The in-memory part and the on-disk list are kept in lockstep:
every insert adds to both. Every spill clears both and writes the
displaced docs into a new segment.

## What the index records

The index records "what was inserted, modulo spills" — *not*
"docs whose heap row is currently live". On the primary, both
the in-memory part and the on-disk list can carry "ghost"
entries — pointers whose heap row is no longer live. Two ways
this happens:

- **Insert-then-abort.** The insert path adds terms to the
  in-memory part and a pointer to the on-disk list, then the
  transaction aborts. Neither is rolled back. The heap row is
  marked aborted; scan-time visibility filtering hides it from
  query output.
- **Delete (and update).** `DELETE` and the old half of an
  indexed-column `UPDATE` mark the heap row dead but do *not*
  touch the in-memory part or the on-disk list — the pointer
  and its term postings stay until VACUUM merges or rebuilds
  the segment.

Spill drops both kinds of ghost at segment-build time (re-tokenizes
from heap, sees the dead row, skips). VACUUM drops them at
segment-merge time via metap/alive-bitset shrinkage.

## Invariants

These describe the **replica after bootstrap completes**, compared
against the primary at the corresponding log position. "Corresponding
log position" = the LSN replay had reached when bootstrap took its
exclusive lock. The replica is always some distance behind the
primary.

**Note on ghost entries.** The bootstrap walks the on-disk list
and re-tokenizes from heap via `heap_fetch(SnapshotAny, …)`.
Aborted-or-dead heap tuples that are still physically present
are returned and re-indexed normally. Tuples that have been
physically removed (HOT-pruned, vacuumed, heap pages truncated)
are not, and bootstrap skips them. Both insert-abort ghosts and
delete/update ghosts can end up in this dropped category once
VACUUM has cleaned the heap. So on the replica:

- `docid_pages_replica` ≡ `docid_pages_primary` (exact bytes via
  GenericXLog replay).
- `memtable_replica` ⊆ `docid_pages_replica` (one-way — entries
  dropped at bootstrap have no memtable presence).
- `memtable_replica` ⊆ `memtable_primary` (same reason — primary
  retains the ghost; replica may not).

These asymmetries don't affect live-query correctness: a dropped
ghost can't yield search results anyway, because the heap row
that backs it is gone and scan-time visibility filtering would
reject it. They do cascade to BM25 corpus statistics: dropped
ghosts mean a smaller `total_docs`, which shifts IDF for terms
that appeared only in those ghosts, which shifts ranking scores
on the replica vs. the primary. Slight, scoped to terms that
appeared only in physically-removed ghost rows, and self-heals
at the next spill (which rewrites segment from heap on both sides
and drops the ghost in either case).

**Inv 1 — Replica state is internally well-formed.** Every row
pointer in `memtable_replica` appears in `docid_pages_replica`.
Every term posting referencing a CTID has a matching entry in
the doc-length table. The reverse direction (every docid-page
entry has a memtable entry) holds *except* for ghost entries
whose heap row was physically removed before bootstrap.

**Inv 2 — Counters reflect what's actually on the replica.**
`shared->total_docs == sum(segment.num_docs) + |memtable_replica|`.
Same for `total_len`. The numbers BM25 ranking reads describe
the replica's actual contents — they may differ from
`shared->total_docs` on the primary by the count of dropped
ghosts.

**Inv 3 — Each row pointer counted once on the replica.** No
pointer is in two segments, or in both a segment and the in-memory
part. Counters don't double-count.

**Inv 4 — Live docs are findable on the replica.** Every doc the
primary's index can return as a live result at the corresponding
log position can also be returned by the replica. (Equivalently:
every CTID whose heap row is visible to a snapshot at the
corresponding log position is in either a segment or
`memtable_replica`. Visibility is the relevant boundary, not
"every CTID the primary has indexed" — dropped ghosts wouldn't
return results from the primary either.)

**Inv 5 — Forward replay works.** After bootstrap, applying
further WAL leaves Inv 1–4 holding at the new log position.

## How the current code achieves this

`tp_rebuild_index_from_disk`, in order:

1. **Open the index relation, read the metapage early, validate
   magic.** Magic is set at index creation and never changes, so
   this check is stable across replay. Done before registering
   shared state so a corrupt metapage doesn't leak a registry
   entry.

2. **Register shared state.** From here, replay of `INSERT_TERMS`
   / `SPILL` / `MERGE_LINKAGE` records hits our entry instead of
   being silently skipped. Atomic counters start at 0; they are
   not authoritative until step 7.

3. **Drain WAL replay.** Block until `GetXLogReplayRecPtr` reaches
   `GetWalRcvFlushRecPtr` at drain start. Past this point, every
   record the primary had emitted before we started draining has
   been applied to on-disk state. (Drain is a no-op when not in
   recovery — e.g., post-promote PITR or post-recovery primary.)

4. **Acquire the per-index `LW_EXCLUSIVE`.** Custom-rmgr replay
   blocks on this lock, as do backend scans (which hold
   `LW_SHARED`). The only redo that can advance during our hold
   is `GenericXLog`, and only on records queued before the next
   custom-rmgr record (sequential WAL replay halts at the first
   blocked record).

5. **Re-read the metapage under the lock.** Snapshot the fields
   we use (`total_docs`, `total_len`, `first_docid_page`,
   `level_heads[0]`) so they come from a self-consistent state.

6. **Branch on the fresh snapshot, not the early one.**
   - Heap was TRUNCATEd post-spill (`heap_blocks == 0 &&
     fresh_metap.total_docs > 0`) → log warning, return registered
     empty state.
   - Empty index (`total_docs == 0 && first_docid_page == Invalid
     && level_heads[0] == Invalid`) → return registered empty
     state.
   - Otherwise → walk the on-disk list, re-tokenize each heap
     row, add to the in-memory part. The add path is idempotent
     by row pointer (`tp_store_document_length` is the
     check-and-add gate), so concurrent INSERT_TERMS replay during
     drain that already populated some entries doesn't get
     double-added by the walk.

7. **Set counters as ground truth.** After the walk:
   ```c
   tp_doclength_summary(local_state, &count, &sum_len);
   pg_atomic_write_u32(&shared->total_docs,
                       fresh_metap.total_docs + count);
   pg_atomic_write_u64(&shared->total_len,
                       fresh_metap.total_len + sum_len);
   ```
   Atomic *write*, not `fetch_add`: under the exclusive lock the
   metapage half (segments) and the in-memory half
   (`tp_doclength_summary` walks the doc-length dshash) are
   disjoint cohorts; their sum is absolute truth at the moment we
   hold the lock. `fetch_add` would double-count any cohort that
   redo applied to the registered state during the drain *and*
   that also ended up in the metapage half via spill's
   `tp_sync_metapage_stats`.

8. **Release the lock.**

## Scenario × invariant matrix

For each scenario: trace the bootstrap, check Inv 1–5. Inv 1's
asymmetry (memtable ⊆ docid-pages) and Inv 2's possible drift
from the primary's `total_docs` are baked into the invariants
themselves — there's no separate "modulo ghosts" footnote, since
both invariants describe the replica's actual state.

| # | Scenario | Behavior | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|---|
| A | Brand-new index, nothing replayed | Register → drain no-op → fresh metapage shows total=0/list=empty/no segments → empty-index branch returns registered empty state | ✓ | ✓ | ✓ | ✓ | ✓ |
| B | Brand-new index, primary did N inserts (no spill) | Register → drain catches up → fresh metapage shows list non-empty → walk fills in-memory part → `total_docs = 0 + N = N` | ✓ | ✓ | ✓ | ✓ | ✓ |
| C | Post-spill, no inserts since | Fresh metapage shows total=N/list=empty/segments present → walk returns 0 → `total_docs = N + 0 = N` | ✓ | ✓ | ✓ | ✓ | ✓ |
| D | Post-spill + M new in-memory inserts | Walk fills remainder → `total_docs = N + M` | ✓ | ✓ | ✓ | ✓ | ✓ |
| E1 | Spill replays fully before bootstrap takes the lock | Indistinguishable from C on entry to the exclusive section | ✓ | ✓ | ✓ | ✓ | ✓ |
| E2 | Spill's GenericXLog pieces (segment data, list clear, stat sync) replay during our drain or hold; SPILL custom record blocks behind our lock | Fresh metapage may reflect any subset of these. atomic_write under the lock uses metapage snapshot + dshash walk, both disjoint cohorts. Result is correct regardless of what intermediate state we caught. | ✓ | ✓ | ✓ | ✓ | ✓ |
| F | Primary crash recovery | No backends during recovery, so bootstrap fires only post-recovery. Drain is no-op. Otherwise C/D. | ✓ | ✓ | ✓ | ✓ | ✓ |
| G | Heap TRUNCATE'd while index has data | Heap-empty branch logs warning, returns registered empty state. Primary is in same stale state — replica matching it is correct. | ✓ | ✓ | ✓ | ✓ | ✓ |
| H | PITR mid-burst (issue #350) | Post-promote, drain is no-op. metapage `total_docs` is from last spill before target (could be 0). Walk recovers M docs. `total_docs = small + M`. | ✓ | ✓ | ✓ | ✓ | ✓ |
| I | Merge replays during bootstrap | MERGE_LINKAGE custom record blocks at our lock. Pre-merge GenericXLog (segment data writes) can replay during drain or hold; doesn't affect the snapshot. After we release, MERGE_LINKAGE applies its atomic shrinkage to whatever value we wrote, yielding `post_merge_metap.total + memtable_count`. | ✓ | ✓ | ✓ | ✓ | ✓ |

## Why GenericXLog redo during the exclusive hold is harmless

While bootstrap holds the per-index exclusive lock, custom-rmgr
records block. GenericXLog records replay freely — but sequential
WAL replay halts at the first blocked custom-rmgr record, so only
GenericXLog records queued *before* it can apply.

GenericXLog records on this index:

| Source | What it modifies | Effect on bootstrap |
|---|---|---|
| Docid-page WAL (in tp_insert) | Appends a row pointer to the on-disk list. | Always queued *after* a paired INSERT_TERMS — if INSERT_TERMS is blocked at our lock, this can't apply. |
| Segment data WAL (during spill) | Writes new segment pages. | New pages aren't referenced by metap until SPILL replays. Invisible to scans, harmless. |
| `tp_clear_docid_pages` (during spill) | Clears `metap.first_docid_page` on disk. | We use the metapage snapshot taken under the lock, so on-disk change doesn't affect us. |
| `tp_sync_metapage_stats` (during spill) | Writes `metap.total_docs/total_len` on disk. | Same — snapshot is what we use. |
| Segment data WAL (during merge) | Writes new segment pages. | Same as spill. |
| Segment alive-bitset updates (VACUUM bulk-delete) | Modifies segment header alive-bitset. | We don't read alive-bitsets at bootstrap time. Harmless. |

## Why atomic_write, not fetch_add

Concrete scenario where `fetch_add` would have double-counted:

- Primary timeline: I1..I50, I51..I100, SPILL, I101..I150.
- Replica registers shared state somewhere between I1..I50 and
  I51..I100. Drain begins.
- During drain on the replica: I51..I100 INSERT_TERMS replay
  against our registered state (atomic += 50). SPILL's
  `tp_sync_metapage_stats` GenericXLog applies (`metap.total_docs`
  set to primary's atomic at that moment = 100). SPILL custom
  record applies (memtable cleared, atomic untouched).
  I101..I150 INSERT_TERMS replay (atomic = 50). Docid pages now
  hold 50 entries (post-spill memtable docs).
- Drain ends. Acquire exclusive. fresh_metap.total_docs = 100.
  Walk the docid-page list — idempotency gate skips all 50 (already
  added by INSERT_TERMS replay). `tp_doclength_summary` returns
  count=50.
- With `fetch_add`: `atomic = 50 + 100 = 150`... but the I51..I100
  cohort was already counted via INSERT_TERMS replay AND included
  in `metap.total_docs=100` via stat sync, so atomic ends up at
  150 + 50 = 200. Wrong by 50.
- With `atomic_write`: `atomic = 100 + 50 = 150`. Matches
  primary's atomic at the corresponding log position.

The disjoint-cohort argument: `metap.total_docs` always counts
*segments only* (set by `tp_sync_metapage_stats` from the primary's
atomic at spill time, post-segment-write). `tp_doclength_summary`
counts *the in-memory part only* (from the doc-length dshash
under our lock). The two sets are disjoint by construction —
spill clears the in-memory part before stat-sync writes the
post-spill total. So sum-of-disjoint-cohorts is the absolute
total, and atomic_write under the exclusive lock is the correct
operation.

## Known limitations

- **Mid-walk error.** If anything in the walk loop errors
  (corruption-driven `heap_fetch` PANIC, expression-eval
  failure, etc.) Postgres's exception handler releases the
  LWLock but the in-memory part is left partially populated.
  The next backend bypasses bootstrap (registry hit) and sees
  the partial state.

The ghost-driven memtable / corpus-stat asymmetries between
primary and replica are *not* a limitation — they're baked into
Inv 1 and Inv 2. Whether the resulting BM25 score drift matters
depends on whether primary↔replica score parity is a contract
or best-effort; the current impression is best-effort.

## Why it works

> Bootstrap takes the exclusive lock, snapshots the metapage and
> the doc-length dshash under that lock, and writes
> `total = metap_snapshot + memtable_count` as ground truth. The
> exclusive lock blocks any custom-rmgr replay that could change
> the index's "inserted" view; the only redo that can happen
> during the hold is GenericXLog records on metapage fields we
> read into the snapshot, on segment data we don't read, or on
> docid pages that can't apply because their paired INSERT_TERMS
> is blocked.

Concurrent bootstrap is also safe: `tp_create_shared_index_state`
uses an atomic register-or-attach (`tp_registry_register_if_absent`)
in its bootstrap mode, so two backends arriving at the same empty
registry resolve to the same shared state instead of unregistering
each other; the second arrival's bootstrap still runs through
drain → exclusive → walk, but the idempotency gate dedups and the
atomic-write yields the same answer.
