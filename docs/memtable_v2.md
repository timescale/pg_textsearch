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
    uint16          flags;        /* reserved */
    uint32          n_records;
    uint32          free_offset;  /* tail-of-records */
    BlockNumber     next_block;   /* chain link; Invalid at tail */
    FullTransactionId dead_fxid;  /* reserved for future
                                   * deferred reclaim */
} TpMemtablePageHeader;  /* 24 bytes */
```

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
    uint16          _pad;          /* alignment */
    int32           doc_length;
    uint32          vector_len;
    char            vector_bytes[FLEXIBLE_ARRAY_MEMBER];
                                   /* opaque v2 bm25vector */
} TpMemtableRecord;
```

Records are appended back-to-back from
`TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET` upward, each `MAXALIGN`'d.
`vector_bytes` is the v2 bm25vector wire format
(see `src/types/vector.h`); the memtable layer doesn't decode it.

## Write path (`tp_memtable_append`)

Lock order is fixed and documented in `src/memtable/log.h`:

```
per-index LWLock SHARED → tail buf EXCL
                          → new buf EXCL (just-extended)
                          → metapage buf EXCL
```

The append takes three forms:

1. **Bootstrap.** Metapage `tail_blkno == Invalid`. Acquire meta
   `EXCL`, re-check (lost-race),
   `ExtendBufferedRel(EB_LOCK_FIRST)`, single `GenericXLog`
   over {meta, new}: init new page, append record, set
   `head=tail=new_blkno`.

2. **Fast append.** Read meta `SHARED` for `tail_blkno`;
   release; lock tail `EXCL`; stale-tail check; if `can_fit`:
   single `GenericXLog` over tail, append.

3. **Extend.** Tail full.
   `ExtendBufferedRel(EB_LOCK_FIRST)`; acquire meta `EXCL`;
   single `GenericXLog` over {tail, new, meta}: init new page,
   set `tail.next_block = new_blkno`, append record to new,
   update `metap.tail_blkno = new_blkno`.

Reject up front: any `vec_len` whose `record_size` exceeds
`BLCKSZ - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET`.

Crash safety: if we crash between `ExtendBufferedRel` and
`GenericXLogFinish`, the relation has an unreachable extra
block whose contents are uninitialized. That's an acceptable
leak; no code sequentially scans the relation without
`tp_memtable_page_is_valid` gating.

## Read path (`tp_memtable_chain_source_create`)

The chain source is one of the `TpDataSource`s consumed by
BMW / sequential scoring. It:

- Acquires per-index `LW_SHARED` for the source's lifetime
  (excludes spill; coexists with concurrent inserts which
  also hold SHARED + per-page EXCL).
- Walks the chain head→tail under SHARED buffer lock one page
  at a time.
- For each record: bounds-checks against the page's
  `free_offset`; validates `vector_bytes` via
  `tpvector_validate_v2_buffer`; for each unique term in the
  vector, accumulates `(ctid, freq)` into a per-term posting
  buffer in a private memory context.
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
4. `tp_spill_finalize`: one `GenericXLog` over up to 4
   buffers (metapage + new segment header + optionally the
   old chain head; typically 2 buffers in steady state):
   - `level_heads[0] = root`, `level_counts[0]++`
   - `total_docs += docs_delta`, `total_len += len_delta`
   - `memtable_head_blkno = memtable_tail_blkno = Invalid`
   - If old `level_heads[0]` was valid, set
     `new_header->next_segment = old_head`.
5. Reset auto-spill counter heuristic.

Old memtable chain pages are **not** WAL-touched. They become
orphans reachable only via direct block reads — they still
have intact `TP_MEMTABLE_PAGE_MAGIC` headers but no metapage
pointer to them. In-flight standby/replay scans that already
latched a chain head walk them to completion against the
metapage snapshot they started with; new scans see the
post-spill metapage and skip the orphans entirely.
FSM-recycling orphans is deferred to a future
`amvacuumcleanup` pass.

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
| Spill     | `LW_EXCLUSIVE`   | metapage + new seg header via GenericXLog |
| Merge     | (segment-only)   | metapage + new seg header via GenericXLog |

Insert vs scan: both SHARED at LWLock; the tail buffer's EXCL
lock serializes the per-page race, the same way any heap or
btree scan tolerates concurrent writers.

## Upgrade

`TP_METAPAGE_VERSION` was bumped 6 → 7. v6 metapages from
1.2.x and earlier fail at open with a clear error pointing to
REINDEX. There is no dual-format support.

## Removed in 1.3.0

- `src/replication/rmgr.c`, `src/replication/replication.h`,
  custom rmgr ID 149, `INSERT_TERMS` / `SPILL` /
  `MERGE_LINKAGE` records.
- `tp_add_docid_to_pages`, `tp_clear_docid_pages`,
  `tp_invalidate_docid_cache`, `first_docid_page` (now
  `_unused_docid_page` to preserve metapage offsets),
  `TpDocidPageHeader`, `TP_DOCID_PAGE_MAGIC`,
  `TP_DOCID_PAGE_VERSION`.
- `tp_rebuild_index_from_disk` and the LSN-drain bootstrap
  dance.
- `spill_generation` and the in-process docid-page cache.
- The `docs/replication/CORRECTNESS.md` invariants doc (this
  doc replaces it).

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
`pg_textsearch.memtable_spill_threshold` — the cost can dominate.

The mitigation is the spill threshold itself: tighter spill
thresholds keep the chain short. The Phase-10 tuning of the
default thresholds is tracked separately. Pending follow-ups:

- Replace `pg_textsearch.memory_limit` (DSA-bytes semantics) with
  a chain-page-count threshold (`pg_textsearch.memtable_max_pages`
  or similar).
- Per-term skip lists at the memtable layer (an A3-style hybrid
  per `plan.md`) if smaller spill thresholds prove insufficient
  for high-cardinality query workloads.

## Future cleanups (tracked separately)

- Lazy reclaim of orphan memtable chain pages from spill via
  `amvacuumcleanup` (B-tree's `merged_at_xid` pattern).
- Removal of the DSA-backed registry in favor of a fixed-size
  shmem hash sized at `shmem_request_hook` time
  (`pg_textsearch.max_indexes` GUC).
- Removal of dead DSA helpers in `src/memtable/posting.c` and
  `src/memtable/stringtable.c` (most callers are gone since
  Phase 4 but the helpers remain compiled).
- Reduction of `pg_textsearch.memory_limit` to something
  meaningful for the page-backed layer (e.g. chain-page count
  threshold).
