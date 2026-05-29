# Memtable v2 — on-disk paged design

Status: implemented, ships in 1.3.0 (issue #374).

This document specifies the on-disk format and write/read/spill
flow of the v2 memtable. It replaces the shared-memory dshash
memtable used through 1.2.x.

## Why

The v1 memtable lived in DSA shared memory (per-index dshash
tables for the term dictionary and doc-length map). To keep it
consistent under streaming replication and crash recovery,
pg_textsearch shipped a custom rmgr (`TP_RMGR_ID = 149`) with
three record types — `INSERT_TERMS`, `SPILL`, `MERGE_LINKAGE` —
plus a chain of on-disk "docid pages" used by the first backend
after restart to re-tokenize the in-memory part from heap.

That design is fundamentally incompatible with PostgreSQL's
single-page WAL-redo helper (used by online-page-fix tooling
and any caller of `XLogReadBufferForRedoExtended` in a
no-extension-load context). Single-page redo runs without
loading `pg_textsearch.so`, so it can't replay records into the
shared-memory memtable.

v2 sidesteps the problem by removing the shared-memory memtable
entirely. The L0 layer is now a chain of pages stored in the
index relation itself, mutated under standard buffer locks and
WAL-logged via `GenericXLog`. No custom rmgr. No docid pages.
No bootstrap path. Stock PostgreSQL replay reconstructs every
page.

## Architecture

```
                ┌────────────────────────────────────┐
                │ metapage (block 0)                 │
                │   level_heads[0..N] (segments)     │
                │   memtable_head_blkno              │
                │   memtable_tail_blkno              │
                │   total_docs, total_len            │
                │   (segment-only after Phase 4)     │
                └────────────────────────────────────┘
                              │
                              ▼  (chain of memtable pages)
                  ┌──────────┬──────────┬──────────┐
                  │ block A  │ block B  │ block C  │
                  │  head    │          │  tail    │
                  └──────────┴──────────┴──────────┘
                              │
                              ▼  (spill)
                          L0 segments
                              │
                              ▼
                          L1, L2, …
```

L0+ segments below the memtable are unchanged from v1.

## Page format

### Memtable page header

```c
typedef struct TpMemtablePageHeader
{
    uint32          magic;        /* TP_MEMTABLE_PAGE_MAGIC */
    uint16          version;      /* 1 */
    uint16          flags;        /* TP_MEMTABLE_PAGE_FLAG_* */
    uint16          n_records;
    uint16          free_offset;  /* tail-of-records, MAXALIGN'd */
    BlockNumber     next_block;   /* chain link; Invalid at tail */
    FullTransactionId dead_fxid;  /* when DEAD: xact horizon for
                                   * deferred FSM recycle */
} TpMemtablePageHeader;  /* 24 bytes */
```

`flags` carries:
- `TP_MEMTABLE_PAGE_FLAG_DEAD` (0x0001) — set on each page of a
  memtable chain when `tp_do_spill` unlinks it from the metapage.
  `dead_fxid` records the spill transaction; `amvacuumcleanup`
  (Step 2) will recycle the block once the horizon is older than
  every active snapshot.  Not used on live chain pages.
- `TP_MEMTABLE_PAGE_FLAG_CONTINUATION` (0x0002) — page is a
  continuation of a multi-page record (see *Multi-page records*
  below); `n_records == 0` and the bytes between
  `TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET` and `free_offset` are
  opaque payload, not a record stream.

The standard `PageHeader` lives in the first 24 bytes of the
page; the `TpMemtablePageHeader` immediately follows.
`tp_memtable_page_init()` sets `pd_lower = BLCKSZ` so that
`GenericXLog`'s page-hole zeroing pass leaves the record area
intact. This is the same convention used by other GenericXLog
page formats in PostgreSQL that don't use line pointers.

### Memtable record

```c
typedef struct TpMemtableRecord
{
    ItemPointerData ctid;          /* 6 bytes */
    uint16          flags;         /* TP_MEMTABLE_RECORD_FLAG_* */
    int32           doc_length;
    uint32          vector_len;    /* FULL payload length, even
                                    * when split across pages */
    char            vector_bytes[FLEXIBLE_ARRAY_MEMBER];
                                   /* opaque v2 bm25vector */
} TpMemtableRecord;
```

Records are appended back-to-back from
`TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET` upward, each `MAXALIGN`'d.
`vector_bytes` is the v2 bm25vector wire format
(see `src/types/vector.h`); the memtable layer doesn't decode it.

### Multi-page (FRAGMENT) records

bm25vectors can exceed a single page (a long document tokenizes
to a vector larger than `BLCKSZ - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET
- sizeof(TpMemtableRecord)`, roughly 8 KB minus header overhead).
The memtable supports arbitrarily large records via a FRAGMENT
encoding:

```
   Thead (regular page, n_records=1, flags on record = FRAGMENT)
     │   ← inline prefix bytes fill the page
     ▼
   C1 (continuation page, flags = CONTINUATION, no records,
     │  payload chunk #1 fills the page)
     ▼
   C2 ─► … ─► Cn ─► Tnew (regular memtable page; the new chain tail)
```

- **Fragment head record.** `TP_MEMTABLE_RECORD_FLAG_FRAGMENT` is
  set on the record's `flags`. `vector_len` carries the **full**
  payload length; the inline prefix on the head page is
  `page.free_offset - record_offset - sizeof(TpMemtableRecord)` bytes
  (always sized to fill the head page, so `free_offset == BLCKSZ`
  after the write). The head page's `n_records` is always 1.
- **Continuation pages.** Carry the `TP_MEMTABLE_PAGE_FLAG_CONTINUATION`
  page-header flag, have `n_records = 0`, and store an opaque
  payload chunk between the header and `free_offset`. They're
  reached only via the fragment head's `next_block → C1 → C2 → …`
  and skipped by the outer chain walk (which resumes at the last
  continuation's `next_block`).
- **Per-continuation chunk size** is bounded by
  `BLCKSZ - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET`. There is no
  cap on the number of continuations per record beyond the
  varlena limit on `bm25vector` itself (~1 GB).

See `tp_memtable_fragment_inline_capacity_max()` and
`tp_memtable_continuation_max_chunk()` in `src/memtable/page.h`
for the exact per-page byte budgets.

## Write path (`tp_memtable_append`)

Lock order is fixed and documented in `src/memtable/log.h`:

```
per-index LWLock SHARED → tail buf EXCL
                          → new buf EXCL (just-extended)
                          → metapage buf EXCL
```

The append takes four forms, dispatched on `vector_len` vs
`TP_MEMTABLE_PAGE_MAX_VECTOR_LEN` and on whether the chain is empty:

1. **Bootstrap.** Metapage `tail_blkno == Invalid` and the record
   fits on a single page. Acquire meta `EXCL`, re-check
   (lost-race), `ExtendBufferedRel(EB_LOCK_FIRST)`, single
   `GenericXLog` over {meta, new}: init new page, append record,
   set `head = tail = new_blkno`.

2. **Fast append.** Read meta `SHARED` for `tail_blkno`;
   release; lock tail `EXCL`; stale-tail check; if `can_fit`:
   single `GenericXLog` over tail, append.

3. **Extend.** Tail full but record fits on a single page.
   `ExtendBufferedRel(EB_LOCK_FIRST)`; acquire meta `EXCL`;
   single `GenericXLog` over {tail, new, meta}: init new page,
   set `tail.next_block = new_blkno`, append record to new,
   update `metap.tail_blkno = new_blkno`.

4. **Fragment (oversized record).** `vector_len >
   TP_MEMTABLE_PAGE_MAX_VECTOR_LEN`. Implemented in
   `memtable_append_fragment` (`src/memtable/log.c`):

   1. Extend `Tnew` (the post-fragment tail) and init it empty;
      own `GenericXLog`.
   2. Extend `Cn`, `Cn-1`, …, `C1` in reverse order (so each
      knows its successor block at init time); each is its own
      `GenericXLog` with `tp_memtable_continuation_page_init`
      copying the relevant chunk of payload bytes.
   3. Extend `Thead` and write the FRAGMENT head record with
      the inline prefix; own `GenericXLog`; set
      `Thead.next_block = C1`.
   4. **Publish.** A final `GenericXLog` over {old_tail, meta}
      (extend case) or {meta} (bootstrap case): set
      `old_tail.next_block = Thead` and `metap.tail_blkno = Tnew`.

   Up to step 4 the new pages are orphans — they have valid
   magic but no `meta.head → … → Thead` path reaches them.
   A crash at any point before step 4 leaves orphan pages that
   the outer chain walk never traverses.

Crash safety: if we crash between any `ExtendBufferedRel` and
its `GenericXLogFinish`, the relation has unreachable extra
blocks whose contents are uninitialized or have valid magic
but no metapage pointer. That's an acceptable leak; no code
sequentially scans the relation without `tp_memtable_page_is_valid`
gating, and the outer chain walk never reaches them. Orphan
reclaim is deferred to a future `amvacuumcleanup` pass.

## Read path (`tp_memtable_chain_source_create`)

The chain source is one of the `TpDataSource`s consumed by
BMW / sequential scoring. It:

- Acquires per-index `LW_SHARED` for the source's lifetime
  (excludes spill; coexists with concurrent inserts which
  also hold SHARED + per-page EXCL).
- Walks the chain head→tail under SHARED buffer lock one page
  at a time. Continuation pages are never visited by the outer
  walk — only by `reassemble_fragment`.
- For each record: bounds-checks against the page's
  `free_offset`; if `flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT`,
  calls `reassemble_fragment` to walk the head page's
  continuation chain under per-page SHARED locks and palloc a
  single contiguous `vector_bytes` buffer in the source's
  memory context; otherwise uses the inline `vector_bytes`
  directly. Either way, validates the assembled bytes via
  `tpvector_validate_v2_buffer`; for each unique term in the
  vector, accumulates `(ctid, freq)` into a per-term posting
  buffer in a private memory context.
- After a fragment, the outer walk resumes at the last
  continuation's `next_block`, not at the head page's
  `next_block` (which points at C1).
- Builds a doc-length lookup HTAB from the records.

All accumulators live in a child memory context that
`close()` drops in one shot. The TpVector bytes are not
copied out of the page buffer until they're decoded;
per-term accumulators are palloc'd in query-local memory.

## Spill (`tp_do_spill` → `tp_spill_finalize`)

Spill runs under per-index `LW_EXCLUSIVE`:

1. Construct a chain source (mode-compatible reuse of the
   already-held EXCL lock).
2. Extract sorted `TermInfo[]` + finalized `TpDocMapBuilder`.
3. Build the new L0 segment via `tp_write_segment`.
4. **`tp_spill_finalize`**: one `GenericXLog` over up to 4
   buffers (metapage + new segment header + optionally the
   old chain head; typically 2 buffers in steady state):
   - `level_heads[0] = root`, `level_counts[0]++`
   - `total_docs += docs_delta`, `total_len += len_delta`
   - `memtable_head_blkno = memtable_tail_blkno = Invalid`
   - If old `level_heads[0]` was valid, set
     `new_header->next_segment = old_head`.
5. **`tp_memtable_mark_chain_dead`**: walk the now-unlinked chain
   (including fragment continuation pages) and stamp each page
   `DEAD` + `dead_fxid` in separate `GenericXLog` records (one
   page per record; up to four buffers could be batched later).
   Uses the spill transaction's `FullTransactionId` as the
   recycle horizon.
6. Reset auto-spill counter heuristic.

**Crash-safe ordering**: Steps 4 and 5 are ordered finalize-first,
then mark-dead. If we crash between step 4 and step 5, the chain
pages are unreachable (metapage head cleared) but not stamped
`DEAD`, so `tp_reclaim_dead_memtable_pages` won't free them — they
leak until REINDEX. The reverse order (mark-dead before finalize)
would be unsafe: a crash after marking but before finalize leaves
pages stamped `DEAD` while still reachable via `metap.head`; once
global xmin advances, vacuum would free pages still in the live
chain, causing corruption on FSM reuse.

After step 4 the old chain blocks are **orphans**: no metapage
pointer reaches them. After step 5 each page has valid
`TP_MEMTABLE_PAGE_MAGIC` and a `DEAD` stamp.
In-flight scans that already latched a chain head may still walk
those blocks against the metapage snapshot they started with;
new scans use the post-spill metapage and skip them.
**FSM recycle** of `DEAD` pages happens in `amvacuumcleanup` (see
below); until then the relation file retains those block numbers.

## FSM reclaim and reuse (`tp_vacuumcleanup` / `tp_memtable_alloc_page`)

After spill, orphaned chain blocks remain on the main fork with
valid memtable magic and a `DEAD` stamp.  They are not reachable
from the metapage.  Reclaim and reuse mirror segment compaction:

1. **`tp_reclaim_dead_memtable_pages`** (in `tp_vacuumcleanup`,
   under per-index `LW_SHARED`): scan main-fork blocks after
   the metapage; for each page that is valid memtable format and
   `DEAD`, compare `dead_fxid` to the global horizon via
   `FullTransactionIdPrecedes` (wraparound-safe).  Before freeing,
   check that the block is **not** in the current live chain
   (defense-in-depth against the crash window between finalize
   and mark-dead; see `tp_collect_reachable_chain_blocks`).
   When the spill horizon is old enough and the block is not
   reachable, call `RecordFreeIndexPage` (index FSM — **not**
   WAL-logged, same as `tp_segment_free_pages`).  Bump
   `IndexBulkDeleteResult.pages_free` and run
   `IndexFreeSpaceMapVacuum` when any pages were freed.
2. **`tp_memtable_alloc_page`** (insert / extend / fragment paths
   in `log.c`): `GetFreeIndexPage` first; on success,
   `ReadBufferExtended(..., block, RBM_ZERO_AND_LOCK)` and full
   re-init via `tp_memtable_page_init` or
   `tp_memtable_continuation_page_init` plus
   `GENERIC_XLOG_FULL_IMAGE`.  Otherwise `ExtendBufferedRel`.

Page bodies are not WAL-cleared at reclaim time; reuse overwrites
them on the next append.  `DEAD` flags may remain visible to
`bm25_memtable_dead_pages` until a block is reallocated.

Regression: `test/sql/memtable_spill_dead.sql` (DEAD stamping),
`test/sql/memtable_reclaim.sql` (VACUUM reclaim + FSM reuse).

## Corpus statistics

After v2, `metap.total_docs` / `metap.total_len` mean
**persisted segment data only** (Σ segment.num_docs across all
levels). Active chain stats come from the chain source's
accumulator. Query-time totals are
`metap.totals + chain_source.totals`.

The Phase 1.2.x shmem atomics
(`shared->total_docs` / `total_len`) are no longer authoritative
and will be removed in a follow-up cleanup.

## WAL replay

Every page mutation goes through `GenericXLog`. Stock
PostgreSQL replay reconstructs every page on a standby or
during crash recovery. No custom rmgr is registered.
`pg_waldump` of a `CREATE INDEX + INSERT + SPILL` workload
shows only `Heap`, `Generic`, `Btree`, `XLOG`, `Heap2`,
`Standby`, `Storage`, and `Transaction` records — no rmgr
ID 149.

First backend access after restart or on a fresh standby has
nothing to bootstrap: the chain pages on disk + the metapage
are the durable record.

## Concurrency contract (summary)

| Operation | Per-index LWLock | Page locks                            |
|-----------|------------------|---------------------------------------|
| Insert    | `LW_SHARED`      | tail buf EXCL during append           |
| Scan      | `LW_SHARED`      | chain pages SHARED one at a time      |
| Spill     | `LW_EXCLUSIVE`   | chain pages DEAD via GenericXLog; then metapage + new seg header |
| Vacuum    | `LW_SHARED`      | reclaim: DEAD pages SHARED read; FSM update not WAL-logged; spill takes `LW_EXCLUSIVE` separately |
| Merge     | (segment-only)   | metapage + new seg header via GenericXLog |

Insert vs scan: both SHARED at LWLock; the tail buffer's EXCL
lock serializes the per-page race, the same way any heap or
btree scan tolerates concurrent writers.

## Upgrade

`TP_METAPAGE_VERSION` was bumped 6 → 7 to record the on-disk
memtable chain head/tail.  As of issue #383, v6 metapages from
1.2.x and earlier are read in place (no REINDEX required):

- `tp_get_metapage` accepts version 6 OR 7.  When the on-disk
  metapage is v6 it normalizes `memtable_head_blkno` and
  `memtable_tail_blkno` to `InvalidBlockNumber` in the returned
  palloc'd copy (the v7-only bytes are zero on a v6 page, which
  is block 0 — the metapage itself — NOT `InvalidBlockNumber`,
  so naive direct reads would be catastrophic).
- Direct-buffer readers in `src/memtable/log.c` and
  `src/memtable/chain_source.c` use the version-tolerant helpers
  `tp_metapage_read_memtable_head` / `_tail` instead of touching
  the raw struct fields.
- The first write to a v6 metapage (any GenericXLog mutation
  reaching it — insert, spill finalize, vacuum stats update,
  merge linkage) calls `tp_metapage_upgrade_to_current` on the
  page returned by `GenericXLogRegisterBuffer`.  That helper
  initializes the new chain fields to `InvalidBlockNumber`, bumps
  `pd_lower` so the new 8 bytes land in the WAL image, and stamps
  `version = TP_METAPAGE_VERSION`.  The upgrade piggybacks on the
  surrounding WAL record — no extra WAL traffic.

The v1.2.x layout kept a docid-chain head in `first_docid_page`
(same offset as v7's `_unused_docid_page`).  v1.1.0+ drained that
chain on clean shutdown via a `before_shmem_exit` callback;
v0.5.1–v1.0.0 did not.  Real upgrade workflows call
`bm25_spill_index()` before the binary swap (the documented
procedure), which moves every memtable record into segments but,
in the older releases, leaves the `first_docid_page` pointer
non-Invalid because the helper never cleared it — the on-disk
bytes survive into the v6 metapage that v1.3 inherits.

`tp_get_metapage` accepts a v6 metapage with a non-Invalid
`_unused_docid_page` and proceeds without complaint — the read
path stays silent.  `tp_metapage_upgrade_to_current` is where
the policy is enforced: on the first metapage mutation, if the
orphan pointer is set, it emits a one-time LOG-level forensic
message ("upgrading index … orphaning a non-empty docid chain
pointer") and clears the field as part of the same WAL record
that bumps the version to 7.  Because the upgrade fires at most
once per index lifetime on the primary, the LOG fires at most
once per upgraded index too — even on read-heavy workloads and
on hot standbys (the standby never reaches the upgrade helper;
it merely replays the WAL record the primary emitted).

If an operator suspects in-flight documents were lost (e.g. a
SIGKILL'd cluster from a release without shutdown spill, with
no prior `bm25_spill_index()` call), the LOG `errhint`
recommends `REINDEX INDEX <name>` to rebuild from heap.  Indexes
that never get a metapage mutation (a read-only-since-upgrade
index on the primary, or any index on a never-written standby)
stay v6 indefinitely; this is by design — the read path tolerates
either layout, so the lazy strategy is safe at rest.

To confirm an in-place upgrade has landed, call
`bm25_dump_index('<name>')` and inspect the `Metapage Info`
block: a post-upgrade, pre-insert resting state shows
`version: 7` with `memtable_head_blkno: 4294967295` and
`memtable_tail_blkno: 4294967295` (= `InvalidBlockNumber`).

Pre-v6 metapages (v5, from v0.5.0 and earlier) still REINDEX —
those releases predate the BMW correctness fix and we don't
maintain a multi-version read path past one major.

Validation lives in `.github/workflows/upgrade-tests.yml`.  The
compat-tier matrix downloads every v0.5.1–v1.2.0 release tarball,
builds and runs each release on a real cluster, ingests data,
swaps to the current binary, runs `ALTER EXTENSION UPDATE` *without*
`REINDEX`, and asserts (a) pre-upgrade query results match
post-upgrade results, (b) read-only queries leave the metapage
at v6 (the read path is non-mutating), (c) a single INSERT
triggers v6 → v7 in isolation (before any VACUUM / merge /
additional inserts), (d) VACUUM + alive-bitset work on old-format
segments, (e) `bm25_force_merge` rewrites them, (f) post-upgrade
UPDATE + sentinel-term query proves new content lands in the
upgraded chain and MVCC stays consistent.  That's the source of
truth for the contract documented above — the synthetic
regression test that mutated a v7 metapage back to v6 in process
was removed in favor of testing against real v1.2.x bytes.

## Removed in 1.3.0

- `src/replication/rmgr.c`, `src/replication/replication.h`,
  custom rmgr ID 149, `INSERT_TERMS` / `SPILL` /
  `MERGE_LINKAGE` records.
- `tp_add_docid_to_pages`, `tp_clear_docid_pages`,
  `tp_invalidate_docid_cache`, `first_docid_page` (now
  `_unused_docid_page` to preserve metapage offsets),
  `TpDocidPageHeader`, `TP_DOCID_PAGE_MAGIC`,
  `TP_DOCID_PAGE_VERSION`.
- The LSN-drain bootstrap dance.  The
  `tp_rebuild_index_from_disk` symbol still exists in
  `src/index/state.c` but is now a thin path that opens the
  index, reads the metapage, and (if the on-disk chain is
  non-empty) seeds `shared->chain_page_count` from the chain
  walk so the auto-spill heuristic sees pages that pre-existed
  the current shmem segment.  No docid replay; no waiting on
  WAL replay LSN.
- `spill_generation` and the in-process docid-page cache.
- The `docs/replication/CORRECTNESS.md` invariants doc (this
  doc replaces it).
- `pg_textsearch.memory_limit` and
  `pg_textsearch.memtable_spill_threshold` GUCs along with the
  `bm25_memory_usage` SRF and the soft-limit cross-index byte
  accounting they fed (Phase 7B).  Replaced by the
  chain-page-count trigger `pg_textsearch.memtable_pages_threshold`
  (default 64).  Postgres' shared-buffer pressure is the real
  cap on memtable size now; an oversized chain just gets paged
  out by the buffer manager like any other relation.

## Performance characteristics

A smoke benchmark on a single-node Postgres 18 with default GUCs
(`shared_buffers=128MB`), measuring 50k bulk INSERT + spill + 10k
"hot" INSERT + three ORDER BY queries against a `bm25` index over
synthetic text:

|                          | main (1.2.x) | v2 (1.3.0)    | delta       |
|--------------------------|--------------|---------------|-------------|
| 50k bulk INSERT          | ~2.48s       | ~2.20s        | **−11%**    |
| 10k INSERT (post-spill)  | ~232ms       | ~204ms        | **−12%**    |
| Query, 10k-doc memtable  | ~1.5ms       | ~15ms         | **+10×**    |
| Query, 1k-doc memtable   | ~1.5ms       | ~1.8ms        | parity      |

Average of 3 runs each.

**Inserts are faster** because the v2 write path avoids the DSA
dshash term-lookup and per-term posting-list lock dance on the hot
insert path. Each insert is one buffer-lock + one GenericXLog
record.

**Query latency is linear in memtable size** because the chain
source walks every record at scan time (the v1 dshash had O(1)
term lookup against an in-memory hash). At small memtable sizes
(default auto-spill thresholds) this is parity. At large memtable
sizes — e.g. a manually disabled auto-spill or an oversized
`pg_textsearch.memtable_pages_threshold` — the cost can dominate.

The mitigation is the spill threshold itself: tighter spill
thresholds keep the chain short. Pending follow-ups:

- Per-term skip lists at the memtable layer (an A3-style hybrid
  per `plan.md`) if smaller spill thresholds prove insufficient
  for high-cardinality query workloads.

## Auto-spill triggers

| GUC | Default | Trigger point | Effect |
|-----|---------|---------------|--------|
| `pg_textsearch.memtable_pages_threshold` | 64 | After each successful chain extend | Spill the chain at the next `tp_auto_spill_if_needed` check |
| `pg_textsearch.bulk_load_threshold` | 100000 | PRE_COMMIT, when `terms_added_this_xact` exceeds the threshold | Spill the chain in `tp_bulk_load_spill_check` |

Both default to non-zero. Setting either to 0 disables that trigger;
manual spills via `bm25_spill_index('idx_name')` still work.

The chain-page counter is a per-index `pg_atomic_uint32`
(`TpSharedIndexState.chain_page_count`). Incremented after each
successful page-publish `GenericXLogFinish` in `src/memtable/log.c`,
reset to 0 after `tp_spill_finalize`. The counter is not WAL-logged;
on crash recovery it starts at 0 even if the chain survived. Worst
case the heuristic overshoots by ~one threshold's worth of pages
between restart and the next normal merge — acceptable since the
counter only governs when to spill, not correctness.

## Future cleanups (tracked separately)

- Stamp/recycle bootstrap and crash orphans (never linked from the
  metapage, no `DEAD` stamp) — block-range scan + horizon or
  stricter rules.
- Batch multiple DEAD pages into one `GenericXLog` record at spill
  (up to four buffers).
- Removal of the DSA-backed registry in favor of a fixed-size
  shmem hash sized at `shmem_request_hook` time
  (`pg_textsearch.max_indexes` GUC). The vestigial `TpMemtable`
  DSA stub allocation goes away with this.
