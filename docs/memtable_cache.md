# Memtable cache — restoring v1 query performance on top of v2 storage

Status: draft (issue: TBD).

## Why

The memtable v2 cutover ([`docs/memtable_v2.md`](memtable_v2.md))
replaced the v1 shared-memory inverted index with an on-disk
paged chain of opaque doc records. That change was necessary for
streaming replication, crash recovery via stock PG replay, and
single-page WAL-redo compatibility — but it has a real query-time
cost. Every index scan now opens a `chain_source` that walks
**every chain page** from `meta.head` to `meta.tail`, decoding
every doc record, just to materialize the few posting lists the
query actually needs. With a chain of ~64 pages (the auto-spill
threshold) that's hundreds of `ReadBuffer` calls per query
regardless of keyword frequency.

The v1 in-memory memtable did not pay that cost: it kept a DSA-
backed inverted index (`term → posting list`) and a doc-length
hash table, so a query's posting-list reads were O(query terms)
not O(chain pages). The cache restores this property without
changing v2's source of truth.

## Design summary

- The on-disk page chain remains the **source of truth**. All
  WAL, replication, crash recovery, and standby behavior is
  unchanged.
- On top of that chain we maintain a **per-index, DSA-resident
  cache** of the same content, organized exactly like the v1
  memtable: dshash `term → TpPostingList`, dshash `ctid →
  doc_length`, plus rolling corpus totals.
- The cache is **derived state**: lazily built on first query,
  incrementally caught up by subsequent queries, dropped +
  rebuilt on spill or invalidation. Crashing or losing it is
  harmless — the next query rebuilds from the chain.
- **The write path does not touch the cache.**
  `tp_add_document_terms` is unchanged; writes only mutate the
  on-disk chain. This keeps the v2 contract untouched and
  eliminates any spill-during-apply race.
- **Reads — with spill as a special case of read — are the only
  cache mutators.** A reader that opens a `TpDataSource` first
  catches the cache up to the current chain tail, then serves
  from it. Spill reuses the same "catch up, then act" path; it
  just writes a segment from the cache instead of returning
  posting lists.
- An **apply cursor** tracks the next chain record the cache
  expects to consume. Both apply callers (reader catchup and
  spill catchup) go through a single shared helper
  `apply_to_tail()` under `cache.apply_lock` EXCL. The cursor
  IS the commit point; every record is applied exactly once
  per generation.

The cache is the v1 memtable. The chain is the v2 memtable. The
only conceptual addition is the apply cursor + the read-side
catchup protocol that keeps the cache in sync with the chain.

### Lazy vs eager updates

Writers in this design do not touch the cache; only readers and
spill mutate it. This is a real trade-off, not a free lunch, so
it's worth stating precisely:

- **Total work is the same either way.** Over a chain
  generation producing N records, either eager or lazy
  ultimately performs N record-applies plus the structural
  cost of one inverted index. Lazy splits that into a
  cold-build (or incremental catchups) plus the spill catchup;
  eager spreads it across N write transactions. The
  arithmetic balances.

- **Lazy preserves writer throughput.** v1 had ONE structure,
  so writers updating the in-memory inverted index WAS the
  memtable write. v2 has two structures: the WAL-logged chain
  (source of truth) and the cache (derived). Eager means each
  writer does `chain_append + cache_apply`, ~2x v1's per-write
  cost. Lazy keeps writers at `chain_append`, ~v1's per-write
  cost. For a write-dominated workload this is the largest
  practical difference.

- **Lazy preserves the v2 lock contract.** Writers currently
  hold per-index LWLock SHARED and never touch any DSA dshash.
  Adding eager cache mutation either (a) widens that contract
  to include cache-side locks (creating writer/writer cache
  contention on a previously concurrent path) or (b) splits
  apply into "writer-eager when cache exists, reader-lazy
  otherwise" (just lazy with extra branches). Both undo the
  "writers don't touch the cache" simplification on which the
  whole apply protocol rests.

- **Bulk-load + spill before any query.** Lazy pays the
  cold-build cost on the first query after a spill, via the
  existing chain_source HTAB path.  The spill path itself does
  catch the cache up to the chain tail (so it can write the
  segment from the cache), but then calls `tp_cache_clear` —
  so any catchup work performed during spill is discarded and
  the next query starts from an empty cache regardless.  Eager
  would pay the per-record apply cost distributed across N
  writes, which would let the next-query-after-spill skip the
  cold-build only if eager-mode writers also re-applied
  post-clear (additional protocol complexity, see next bullet).
  In the current lazy design the "where do we pay it" question
  collapses to "on the first cold read after spill", which is
  bounded by `memtable_pages_threshold`.

- **Eager wins on first-query latency** for low-read workloads.
  A query that lands after a long write burst pays a catchup
  walk bounded by `memtable_pages_threshold` (default 64 pages
  ≈ a few ms). Eager would have pre-paid this. So for query-
  latency-sensitive workloads with infrequent queries, eager
  has a measurable advantage.

This PR picks lazy: write paths kept at v2 cost, smaller
apply-protocol surface area, and the spill path reuses the
shared cache for its segment-write pass (then immediately
invalidates the cache via `tp_cache_clear`).  Benchmarks
validate that the bounded first-query catchup cost is in fact
tolerable.  Promoting select hot terms to eager update later
is a refinement we can layer in without restructuring the
cache.

## What we resurrect

The v1 cutover commit (`023c3841`) deleted seven files we want
back, with surgical adjustments for v2:

| Path                               | LOC | Reuse |
|------------------------------------|----:|-------|
| `src/memtable/posting.h`           |  86 | unchanged surface |
| `src/memtable/posting.c`           | 458 | unchanged |
| `src/memtable/posting_entry.h`     |  24 | unchanged |
| `src/memtable/stringtable.h`       |  97 | unchanged surface |
| `src/memtable/stringtable.c`       | 561 | mostly unchanged; drop primary-write paths that now live in `tp_add_document_terms` |
| `src/memtable/source.h`            |  18 | rename `tp_memtable_source_create` → `tp_memtable_cache_source_create`; semantics unchanged |
| `src/memtable/source.c`            | 167 | as above |

> Post-implementation note (PR #395): the original v1 cutover plan
> also listed `src/memtable/memtable.{c,h}` (55 LOC combined) as
> files to resurrect.  Both were dissolved during cleanup:
> `memtable.c` (a tiny stub holding only `tp_cache_clear`) folded
> into `cache.c`, and `memtable.h` (a 49-line header holding only
> `TpDocLengthEntry` + a posting-list helper decl) dissolved into
> `posting.h`.  The table above reflects the surviving v1 files.

What we do **NOT** resurrect:

- `src/replication/replication.h`, `src/replication/rmgr.c`
  (custom rmgr, 1000+ LOC). The cache is non-WAL'd derived
  state; replication is handled at the chain layer.
- `src/index/registry.{c,h}` (484 LOC). The current `state.c`
  already provides registry-equivalent lifecycle.
- `src/index/memory.{c,h}` (87 LOC). Memory accounting will be
  smaller and lives next to the cache.

Net: ~1450 LOC restored. The existing `TpMemtable` scaffold in
`src/index/state.h` (`string_hash_handle`, `doc_lengths_handle`,
counters) is already wired through `state.c`; we just fill in the
handles instead of leaving them invalid.

**No changes to `tp_memtable_append`'s signature** or any other
v2 write-side API. Reviewer-requested simplification: the cache
does not attach a write-side hook, so writers need not surface
any extra metadata (bootstrap flag, post-apply cursor, etc.) to
the cache.

## Data structures

The existing `TpMemtable` struct (`src/index/state.h`) was
effectively dead after the on-disk-memtable cutover:
`string_hash_handle` and `doc_lengths_handle` were only ever set
to `DSHASH_HANDLE_INVALID`, and the three counters
(`total_postings`, `num_terms`, `total_term_len`) were only ever
initialized to 0 and never read.  The cache wires them back up:
`string_hash_handle` and `doc_lengths_handle` are populated with
real values, and the dead counters are removed:

```c
/* src/index/state.h, rewritten for the cache */
typedef struct TpMemtable
{
    /* dshash inverted index: term → TpStringHashEntry.  Set by
     * cold_build, reset to DSHASH_HANDLE_INVALID by
     * tp_cache_clear.  Identical schema to the original
     * shared-memory memtable's term table. */
    dshash_table_handle string_hash_handle;

    /* dshash doc-length map: ctid → TpDocLengthEntry.  Same
     * lifecycle as string_hash_handle. */
    dshash_table_handle doc_lengths_handle;

    /* The single serialization point for cache mutation: every
     * path that advances the cursor or inserts records into the
     * cache acquires this EXCL.  Acquired AFTER per-index
     * LWLock. */
    LWLock              apply_lock;

    /* Lifetime lock for served TpDataSources: SHARED for the
     * lifetime of a source (open through close), EXCL only for
     * full drop / evict.  apply_lock holders may take SHARED
     * here concurrently with served readers. */
    LWLock              lock;

    /* Apply cursor (see "The apply cursor" section).  Mutated
     * only under apply_lock EXCL. */
    uint64              cursor_gen_spill_count; /* generation
                                                 * token: equals
                                                 * TpSharedIndexState
                                                 * .spill_generation
                                                 * at cold-build
                                                 * time. */
    BlockNumber         cursor_next_blkno;     /* logical next
                                                * record position */
    uint16              cursor_next_off;
    pg_atomic_uint64    cursor_seq;            /* monotonic
                                                * apply counter,
                                                * lock-free read */

    /* Memory accounting (cache footprint, used by the 3-tier
     * memory cap).  Updated by apply paths under apply_lock. */
    pg_atomic_uint64    estimated_bytes;
} TpMemtable;
```

`TpStringHashEntry`, `TpPostingList`, `TpPostingEntry`,
`TpDocLengthEntry` come back verbatim from `62308b82`. They're
already designed for the dshash + DSA pattern.

**Corpus stats for query evaluation come from
`TpSharedIndexState`, not from `TpMemtable`.** The shared
struct already carries `total_docs` (u32 atomic) and
`total_len` (u64 atomic) representing the **whole-index**
totals (segments + memtable). These are maintained by the
existing v2 paths: `log.c` increments per insert, `merge.c`
decrements when dead docs are merged out, `vacuum.c` adjusts
on VACUUM, `build.c` initializes from the metapage. The cache
source mirrors v1 (`src/memtable/source.c` @ `62308b82` lines
139-140) and simply reads `shared->total_docs` / `total_len`
into `TpDataSource.total_docs` / `total_len` at source-create
time. Per-cache contribution to the corpus is not separately
tracked — the cache reflects the chain, which has already
been counted into `shared`.

**Spill detection.** The cache generation token is
`cursor.gen_spill_count`, captured at cold-build time from
`TpSharedIndexState.spill_generation` (a pre-existing atomic
added in #374, otherwise unused since the v2 chain landed).
`tp_spill_finalize` bumps `spill_generation` under per-index
EXCL before clearing the metapage, so any apply path acquiring
per-index SHARED after a spill observes the mismatch and drops
+ rebuilds. We use this counter rather than `meta.head_blkno`
because `meta.head_blkno` admits an ABA: spill does **not** FSM-
free the orphaned chain pages (`tp_spill_finalize` in
`src/memtable/log.c:647-698` only updates the metapage), but
`bm25_force_merge` → `tp_truncate_dead_pages`
(`src/access/build.c:336-429`) calls `RelationTruncate` past
the orphan head, after which a fresh `ExtendBufferedRel` can
land at the same physical block number — a head-blkno check
would pass spuriously. `spill_generation` is monotonic within
a backend lifetime, which is sufficient since the cache also
does not survive restart.

## Lock order

Building on the v2 contract in `chain_source.h`:

```
caller per-index LWLock   (SHARED for readers, EXCL for spill)
  ├─► caller cache.apply_lock
  │     │  (EXCL for any path that advances the cursor: reader
  │     │   catchup, cold lazy build, spill catchup.  Held only
  │     │   for the duration of the apply itself.)
  │     └─► caller cache.lock
  │           │  (SHARED for any path serving a TpDataSource —
  │           │   held for the lifetime of the source.  EXCL for
  │           │   drop/evict only; apply_lock holders may take
  │           │   SHARED.)
  │           └─► caller dshash buckets
  │                 └─► caller TpPostingList.lock  (per-term,
  │                                                 leaf)
  ├─► global_eviction_mutex
  │     │  (EXCL, held only inside evict_largest.  Serializes
  │     │   evictions across all backends so cross-index AB-BA
  │     │   between victim selections cannot occur; see "Memory
  │     │   cap" below for the full argument.)
  │     └─► target per-index LWLock  (EXCL, target ≠ caller)
  │           └─► target cache.apply_lock  (EXCL)
  │                 └─► target cache.lock  (EXCL)
  └─► chain page buffers  (existing v2 contract; SHARED for
                            readers via metapage → head → next
                            walks, EXCL only inside log.c)
```

The `global_eviction_mutex` sits above any **other** index's
per-index LWLock. evict_largest **must** skip the caller's own
index when choosing a victim: otherwise the caller (which
already holds its own per-index LWLock in SHARED or EXCL mode
incompatible with same-backend EXCL re-acquire) would deadlock
on itself. See "Memory cap" for both the self-eviction and
cross-index AB-BA arguments.

Two separate cache locks because they have different lifetimes:

- `cache.apply_lock` (EXCL only) is held briefly while mutating
  the cache structure or advancing the cursor. It serializes
  the only two apply callers — reader catchup and spill
  catchup — against each other so the cursor advances by
  exactly one record per apply.
- `cache.lock` (SHARED for readers, EXCL only for full drop or
  evict) covers the lifetime of a served `TpDataSource`. A
  reader holds it SHARED from open through close; an apply path
  holds it SHARED concurrently while it mutates — this is fine
  because the per-posting-list locks and dshash bucket locks
  serialize the actual data mutation, and the only operation
  that can't tolerate concurrent applies is **structural** (drop
  / evict).

Reverse acquisitions are forbidden. The cache locks are never
held across chain page buffer EXCL acquisitions; readers walking
the chain for catchup may hold both cache locks SHARED while
taking chain page buffer SHARED (reader-on-reader is fine).

Note: because the write path doesn't touch the cache, a writer
holding per-index SHARED cannot block on either cache lock. The
write path is invisible to the cache.

## The apply cursor

The cache is updated lazily by readers. The apply cursor is the
authoritative "next chain record to consume" pointer, advanced
under `cache.apply_lock` EXCL by reader catchup, cold lazy build,
and spill catchup.

### What the cursor stores

```c
typedef struct TpCacheCursor
{
    /* Identifies the chain generation the cache was built
     * from.  Captured from TpSharedIndexState.spill_generation
     * at cold-build time, under per-index SHARED+ (which
     * excludes spill, the only writer of spill_generation).
     * Compared on every apply / serve to detect a spill that
     * may have raced through under per-index EXCL between
     * captures. */
    uint64 gen_spill_count;

    /* Logical position of the NEXT chain record to apply.
     * "Logical" means: this is the (page, off) the next record
     * will live at IF the next append is a normal append onto
     * the current tail.  For fragment appends it's the (page, 0)
     * of the first fresh page produced by the fragment.  The
     * meaning is "where is my read head in the chain". */
    BlockNumber next_blkno;
    uint16      next_off;

    /* Optional: a monotonic sequence number incremented on every
     * successful apply.  Used by debug + by readers that want a
     * cheap "did anything change" check that's robust to
     * fragment-page games.  Not strictly required for correctness
     * because (gen_spill_count, next_blkno, next_off) is sufficient,
     * but it makes invariants easier to assert. */
    pg_atomic_uint64 seq;
} TpCacheCursor;
```

The cursor lives in `TpMemtable` (shared, DSA). Mutations of
`gen_spill_count`, `next_blkno`, `next_off` happen under
`apply_lock` EXCL. `seq` is a `pg_atomic_uint64` for lock-free
read.

### Why a logical "next position" rather than `<` comparison

V2 fragment append allocates pages in reverse physical order
(see [`docs/memtable_v2.md`](memtable_v2.md), the "fragment
append" section): a single oversized doc record produces

```
        Thead → C1 → C2 → … → Cn → Tnew
        ^^^^^                       ^^^^
        head fragment               new tail
```

with `BlockNumber(Tnew) < BlockNumber(Thead)` quite possible
because `Tnew` may be allocated from FSM before `Thead`. Any
comparison `(cur.blkno, cur.off) < (meta.tail_blkno, count)` is
wrong as soon as fragment append participates. The cursor sidesteps
this entirely: there is no "before/after" comparison. There is
only "is the cursor at the current logical tail?", answered by
asking the chain walker: starting from `next_blkno @ next_off`,
is there a next record? If not, we're caught up.

### Apply protocol (catchup)

The catchup protocol below — used by **reader catchup** and
**spill catchup** — advances the cursor over already-applied
state. Cold lazy build is a **separate** bootstrap path:
because it must allocate the dshash tables structurally, it
takes `cache.lock` EXCL rather than SHARED, and is documented
in "Read path" under `cold_build`. The two paths share the same
high-level invariants (apply_lock EXCL serialization, monotonic
cursor advance, generation-token check) but the cache.lock
modes differ.

Catchup callers (reader catchup, spill catchup) **MUST** follow
this protocol:

```
/*
 * Advance the cache cursor to the current logical tail,
 * applying every chain record encountered along the way.
 *
 * The walker stops as soon as has_next() returns false.  Because
 * the only callers want "catch up to whatever's currently on
 * disk", there's no per-record target; the chain walker's
 * link-following loop IS the stop condition and is robust
 * against fragment-page allocation order.
 */
apply_to_tail():
    Assert(holding per-index LWLock SHARED or stronger)
    LWLockAcquire(cache.apply_lock, LW_EXCLUSIVE)
    LWLockAcquire(cache.lock,       LW_SHARED)

    if (cursor.gen_spill_count != atomic_read(shared->spill_generation)):
        # Spill completed before our per-index SHARED acquire (a
        # spill needs per-index EXCL, so it can't have completed
        # while we were holding SHARED).  The cache is stale.
        # Drop it:
        #   - Release cache.lock SHARED, re-acquire EXCL.
        #     Because spill held per-index EXCL until it finished,
        #     no readers' source could be alive across the gap
        #     where the spill happened, so cache.lock has no
        #     SHARED holders other than us — EXCL acquire is
        #     immediate.
        #   - tp_cache_clear() under cache.lock EXCL.
        # Return DROPPED to the caller, which decides whether to
        # cold-build (read path) or fall back to chain_source
        # (spill path under budget pressure).
        release cache.lock SHARED
        acquire cache.lock EXCL
        if (cursor.gen_spill_count != atomic_read(shared->spill_generation)):
            tp_cache_clear()
        release cache.lock EXCL
        release apply_lock
        return DROPPED

    walker = chain_walker_open_at(cursor.next_blkno, cursor.next_off)
    while walker.has_next():
        rec = walker.read_record()        # under chain page SHARED

        # Memory-cap check on every apply (read path is the only
        # caller that allocates into the cache).
        if (estimated_bytes + size(rec) > per_index_soft_cap):
            release locks
            return BUDGET_EXCEEDED

        apply_record_to_cache(rec)        # under cache.lock SHARED,
                                          # mutates posting lists +
                                          # doclen hash under their
                                          # own leaf locks
        cursor.next_blkno = walker.next_pos.blkno
        cursor.next_off   = walker.next_pos.off
        pg_atomic_fetch_add_u64(&cursor.seq, 1)

    LWLockRelease(cache.lock)
    LWLockRelease(apply_lock)
    return OK
```

Key properties:

- **`apply_lock` EXCL serializes ALL appliers**. Reader catchup,
  cold build, and spill catchup do not race each other.
- **Idempotent across concurrent callers**. Two readers that
  both want to catch up serialize on apply_lock. The second
  reader finds the cursor at the position the first one left it,
  walks until `has_next()` is false (which is "now"), and
  applies whatever has accumulated since.
- **No double-apply**. The cursor advances strictly per-record
  under apply_lock, so every record is applied exactly once per
  generation.
- **Walker uses logical link traversal**, not block-number
  comparison, so fragment-page allocation order can't desync the
  apply order.

The walker reuses the existing chain-walker logic in
`chain_source.c`, factored into a reusable helper.

## Write path

**Unchanged.** `tp_add_document_terms` and `tp_memtable_append`
have no cache interaction. Writes mutate only the v2 on-disk
chain, exactly as today.

This is a deliberate simplification (reviewer-requested): the
cache is **lazily** caught up by readers, not eagerly maintained
by writers. The cost is that the first reader after a burst of
writes pays a catchup walk; the benefit is the v2 write path is
entirely untouched and no spill-during-apply race can exist.

Catchup cost is bounded by `pg_textsearch.memtable_pages_
threshold` (default 64) — once that many pages accumulate, an
auto-spill fires and the cache is dropped + rebuilt fresh on the
next query. In typical workloads the cache is caught up after
the first query in a transaction, and all subsequent queries
within that transaction pay only the O(query terms) posting-list
lookup.

## Read path

`tp_memtable_cache_source_create(state, rel, query_terms,
query_term_count)` mirrors the chain source contract:

```
tp_memtable_cache_source_create(state, rel, terms, nterms):
    Assert(holding per-index LWLock SHARED)

    /* Step 1: catchup or build, under apply_lock EXCL. */
    for retry in [first, second]:
        result = apply_to_tail()
        if result == OK:
            break
        if result == BUDGET_EXCEEDED:
            /* Cache won't fit; serve from chain_source instead.
             * Cache is left in whatever caught-up-then-aborted
             * state it was in; the next query will retry or
             * eviction will reclaim it. */
            return tp_memtable_chain_source_create(state, rel,
                                                   terms, nterms)
        Assert(result == DROPPED)  /* spill detected */
        /* Step 1a: cold lazy build into the post-spill chain.
         * Done once; second DROPPED in a row is a bug or a
         * legitimate burst of spills, in which case we just
         * fall back to chain_source. */
        if first iteration:
            cold_build_result = cold_build(rel)
            if cold_build_result == ABORT:
                return tp_memtable_chain_source_create(state, rel,
                                                       terms, nterms)
            continue
        return tp_memtable_chain_source_create(state, rel,
                                               terms, nterms)

    /* Step 2: serve from cache.  Acquire cache.lock SHARED for
     * the lifetime of the source. */
    LWLockAcquire(cache.lock, LW_SHARED)
    return cache_source_new(state, terms, nterms)
        /* close op releases cache.lock SHARED */

cold_build(rel):
    LWLockAcquire(cache.apply_lock, LW_EXCLUSIVE)
    LWLockAcquire(cache.lock,       LW_EXCLUSIVE)
        /* EXCL because we're about to allocate dshash tables
         * and reset handles; concurrent SHARED readers can't
         * tolerate that.  Other readers' open() will wait
         * here. */

    if (cache not empty):
        /* someone else cold-built while we waited */
        downgrade cache.lock to SHARED
        release apply_lock
        return RETRY

    /* allocate dshash tables, set handles */
    cursor.gen_spill_count = atomic_read(shared->spill_generation)
    cursor.next_blkno      = meta.head_blkno
    cursor.next_off        = 0

    /* Walk the chain head→tail, applying each record.  Monitor
     * memory cap on each insert; if exceeded, abort: free
     * partial state, set handles back to INVALID, release
     * locks, return ABORT.  Caller falls back to chain_source. */
    while walker has next record:
        rec = walker.read_record()
        if (estimated_bytes + size(rec) > per_index_soft_cap):
            free_partial_cache()
            release locks
            return ABORT
        apply_record_to_cache(rec)
        cursor.next_(blkno,off) = walker.next_pos
        pg_atomic_fetch_add_u64(&cursor.seq, 1)

    downgrade cache.lock to SHARED
    release apply_lock
    return OK
```

Notes:

- **Hot path = `apply_to_tail` returning OK on the first call
  with the walker finding 0 records**. In steady state (no
  writes since the last query), the cache is caught up,
  `apply_lock` is briefly acquired, walker finds nothing, lock
  released — single LWLock acquire + release per query open.
  This is the fast path we're tuning for.
- **First read after a write burst** pays the catchup walk:
  `apply_to_tail` walks every chain record published since the
  last catchup, decoding each and applying to the cache. Cost
  is O(records since last catchup), bounded by the auto-spill
  threshold.
- **`get_postings(term)`** on the returned source does
  `tp_string_table_lookup` under dshash bucket lock, acquires
  `TpPostingList.lock` SHARED, copies entries into heap-
  allocated `TpPostingData`, releases. Identical to v1. The
  returned `TpPostingData` holds POD copies
  (`ItemPointerData`, `int32`) and has no surviving references
  into the DSA arena.
- **Hold `cache.lock` SHARED across the entire scan**. This
  excludes drop/evict for the source's lifetime. The reason is
  **not** the heap-allocated `TpPostingData` (which would
  survive a drop on its own), but the source's cached
  `dshash_table` attachments (`string_table`, `doclength_
  table`): `get_postings` performs a fresh dshash lookup per
  query term, and BM25 scoring calls `get_doc_length` per
  posting hit. A concurrent `tp_cache_clear` that reset
  handles to `DSHASH_HANDLE_INVALID` and freed the underlying
  dshash tables would dangle the cached attachments and
  corrupt the next lookup. Per-query LIMIT (default 1000)
  bounds the lifetime.
- **Cold-build ABORT** and **catchup BUDGET_EXCEEDED** are
  symmetric memory-cap behaviors: when the budget can't
  accommodate the build, we serve via chain_source instead.
  Eviction (see "Memory cap") may later reclaim the partially-
  populated cache.

## Spill path

Spill becomes a **special case of catchup**: bring the cache up
to the current chain tail, then write the segment from the
cache. The v2 chain_source path remains as fallback when the
cache can't be (re)built within budget.

```
tp_spill(state, rel) {
    LWLockAcquire(per-index LWLock, LW_EXCLUSIVE);  /* existing */

    /* per-index EXCL excludes:
     *   - all read-path apply protocols (cold build, catchup)
     *   - the cache's TpDataSource lifetime locks
     * No cache mutator can be running concurrently.  (Writes
     * don't touch the cache at all, so there's no writer-side
     * exclusion to worry about.) */

    LWLockAcquire(cache.apply_lock, LW_EXCLUSIVE);
    LWLockAcquire(cache.lock,       LW_EXCLUSIVE);

    /* Catch the cache up to the current chain tail.  If the
     * cache doesn't exist yet, build it cold.  Either way, on
     * return the cache reflects the entire chain. */
    if (cache exists):
        catchup_result = apply_to_tail_under_existing_excl_locks();
    else:
        catchup_result = cold_build_under_existing_excl_locks(rel);

    if (catchup_result == OK):
        tp_write_segment_from_cache(state, &totals);
    else:
        /* Budget exceeded or DROPPED somehow — fall back to the
         * v2 chain-source spill path, which is the existing
         * implementation. */
        src = tp_memtable_chain_source_create(state, rel, NULL, 0);
        tp_write_segment_from_source(src, &totals);
        tp_source_close(src);

    tp_spill_finalize(rel, new_segment_root,
                      totals.docs, totals.len);
    /* spill_finalize bumps shared->spill_generation atomically
     * under per-index EXCL, then zeroes meta.head_blkno /
     * meta.tail_blkno.  The cache below is now stale by
     * generation; any future apply will observe the mismatch. */

    tp_cache_clear(state);   /* deallocates dshash tables,
                              * sets handles to DSHASH_HANDLE_
                              * INVALID, cursor.gen_spill_count =
                              * 0 (will mismatch shared->
                              * spill_generation on next apply) */

    LWLockRelease(cache.lock);
    LWLockRelease(cache.apply_lock);
    LWLockRelease(per-index LWLock);
}
```

After spill, `tp_cache_clear` deallocates the dshash tables.
The next query lazy-builds. Because writes don't touch the
cache, a spill that lands between two queries simply means the
second query pays a cold-build instead of a catchup walk.

## Standby / replication

WAL replay does not load `pg_textsearch.so`, so no extension
code runs on the standby — including the code that bumps
`shared->spill_generation` inside `tp_spill_finalize`. The
generation token is therefore unreliable across replay, and
chain page links in orphan pages (which a stale cursor would
chase) still exist on disk after spill replay. Rather than
invent a separate standby-side detection path, **the cache is
disabled on standbys**:

```c
TpDataSource *
tp_memtable_cache_source_create(...)
{
    if (RecoveryInProgress())
        return NULL;  /* caller falls through to chain_source */
    ...
}
```

Consequences:

- **Read on standby**: falls through to `chain_source`,
  identical to today's behavior. The perf-recovery goal
  targets the primary, which is where writes (and most read
  traffic) live.
- **Spill replay on standby**: replay zeros `meta.head_blkno`/
  `meta.tail_blkno` via standard `GenericXLog` redo. The
  cache was never built on the standby, so there's nothing
  to invalidate.
- **Promotion**: `RecoveryInProgress()` flips to false. The
  first query after promotion does a cold-build from the
  current chain (whatever replay left). All subsequent
  queries take the catchup path normally. Promotion does not
  bump `spill_generation` because the DSA atomic was already
  consistent (the standby simply never touched the cache);
  the very first cold-build captures whatever
  `spill_generation` is at that point.

No new cross-recovery invariants are introduced. The cost is
giving up the perf win on standby read replicas — acceptable
for the simplicity, and we can revisit by promoting
`spill_generation` to a WAL-replicated metapage field if
profiling shows standby reads are a bottleneck.

## Memory cap (3 tiers)

`pg_textsearch.memory_limit` (KB) is the global ceiling across
all per-index caches in this backend's shared memory view.
Three thresholds:

1. **Per-index soft cap = `memory_limit / 8`**. Enforced on
   **every** allocation path (which is now only the read
   side):
   - **Cold lazy build**: check on every record-apply. If the
     **next** apply would exceed the cap, **abort the partial
     build**: free in-flight dshash entries, reset handles to
     `DSHASH_HANDLE_INVALID`, release locks, return ABORT to
     caller. Caller falls back to `chain_source`.
   - **Reader catchup**: same check on every record. On exceed,
     return BUDGET_EXCEEDED to the caller, which serves via
     `chain_source` for this query. The cache is left in the
     state it was in (caught up through the last successful
     apply); subsequent queries will either succeed (if more
     budget is available) or also fall back to chain_source.
     Eviction will eventually reclaim it.
   - **Spill catchup**: same as cold build / reader catchup.
     On exceed, spill falls back to the v2 chain_source spill
     path.

2. **Global soft cap = `memory_limit / 2`**. When the sum of
   all per-index caches' `estimated_bytes` crosses this, the
   **largest cache other than the caller's own** is **evicted**
   (`tp_cache_clear`, NOT spilled — the chain is still source
   of truth). Eviction protocol:

   ```
   evict_largest():
     /* Pick a victim WITHOUT the caller's own index in the
      * argmax.  Skipping self is what prevents the
      * self-eviction deadlock: the caller already holds its
      * own per-index LWLock (SHARED for read-path eviction;
      * EXCL for spill-path eviction), and LWLocks are
      * non-recursive — a same-backend EXCL re-acquire of a
      * lock the backend already holds would hang with no
      * deadlock detection. */
     target = argmax_{idx ≠ caller_index} estimated_bytes(idx)
     if target == none:
         return NOTHING_TO_EVICT

     /* Acquire the global eviction mutex BEFORE the target's
      * per-index lock.  This serializes evictions across the
      * cluster.  Cross-index AB-BA cannot occur because only
      * one backend is inside evict_largest at a time: if A
      * picks Y as victim and B picks X, the global mutex
      * forces them to run sequentially, and by the time the
      * second one runs its argmax the situation has changed
      * (the first eviction freed its target's bytes; the
      * second may now find a different victim, or none). */
     LWLockAcquire(global_eviction_mutex, LW_EXCLUSIVE)

     LWLockAcquire(target's per-index LWLock, LW_EXCLUSIVE)
     LWLockAcquire(target's cache.apply_lock, LW_EXCLUSIVE)
     LWLockAcquire(target's cache.lock,       LW_EXCLUSIVE)

     /* Re-check after lock acquire: another backend may have
      * already evicted this target between the argmax and
      * the lock acquire. */
     if cache still present and estimated_bytes >= threshold:
         tp_cache_clear(target)
         atomic_sub(global estimated_total_bytes,
                    target.estimated_bytes)

     /* release in reverse order */
     LWLockRelease(target's cache.lock)
     LWLockRelease(target's cache.apply_lock)
     LWLockRelease(target's per-index LWLock)
     LWLockRelease(global_eviction_mutex)
   ```

   **Why deadlock is impossible**:
   - *Self-eviction*: `argmax_{idx ≠ caller_index}` syntactically
     excludes the caller's index, so we never try to acquire a
     per-index LWLock the caller already holds.
   - *Cross-index AB-BA between victim picks*: only one backend
     holds `global_eviction_mutex` at a time, so two concurrent
     evictions cannot interleave their target-lock acquires.
     The second backend's `evict_largest` invocation runs after
     the first releases all target locks; by then `target` may
     have changed.

   **What if the caller's own index is the largest?** The caller
   has no way to evict its own cache from inside the apply
   protocol (LWLock self-upgrade is impossible). evict_largest
   returns NOTHING_TO_EVICT (no other-index target exists or
   total caller-excluded bytes are under threshold), the caller
   returns BUDGET_EXCEEDED, and the read falls back to
   `chain_source` for this query. A different backend, on a
   later query against a different index, will see this index
   in its argmax and evict it.

3. **Global hard cap = `memory_limit`**. Reached only if soft
   cap eviction failed to make room (rare race). The
   triggering read **falls back to chain_source** rather than
   silently exceeding the cap. (There is no analogous write-
   side error path because writes don't touch the cache.)

On standby (`RecoveryInProgress`), spilling is replay-driven so
only eviction is used. The chain source becomes the fallback for
any served query when the cache has been evicted.

Counters used:

- `TpMemtable.estimated_bytes` (per index, atomic) — sum of
  `sizeof(TpPostingEntry) * doc_count + key bytes` etc., updated
  on every apply.
- A backend-global `estimated_total_bytes` in `mod.c` — atomic
  sum across all per-index caches in shared memory. Cheap to
  read at check time.

The global-cap check runs **at apply-protocol entry**, before
the caller acquires `cache.apply_lock` (and therefore before
holding any of its own cache locks). Triggering eviction at
this point keeps the caller's cache locks out of the
acquisition chain entirely: evict_largest needs only
`global_eviction_mutex` + the target's per-index/apply/cache
locks, all on a different index. There is **no mid-walk
eviction trigger**: the per-record check inside `apply_to_tail`
is for the per-index soft cap only (returns BUDGET_EXCEEDED on
exceed, no eviction). This is deliberate — invoking
evict_largest while holding the caller's own apply_lock and
cache.lock would stall other readers on the caller's index for
the duration of the target's per-index EXCL acquire.

Reads of an already-caught-up cache pay only the per-index
`estimated_bytes` touch in the apply protocol header.

## Code layout

```
src/memtable/
    arena.{c,h}             unchanged
    expull.{c,h}            unchanged
    chain_source.{c,h}      unchanged interface; spill falls back
                            here when cache catchup exceeds budget
    log.{c,h}               UNCHANGED — no cache hook in the
                            write path
    page.{c,h}              unchanged
    scan.{c,h}              unchanged
    memtable.{c,h}          restored from 62308b82; thin stub
    posting.{c,h}           restored from 62308b82
    posting_entry.h         restored from 62308b82
    stringtable.{c,h}       restored from 62308b82
    cache.{c,h}             NEW — cursor, apply protocol, cold
                            build, eviction, spill-helper, and the
                            tp_memtable_cache_source_create
                            TpDataSource implementation
                            (subsumes old source.{c,h})

src/index/
    state.{c,h}             TpMemtable already in place; we fill
                            in handles and add the watermark
                            fields + cache.lock
    metapage.{c,h}          unchanged (no format change)

src/scoring/
    bm25.c                  scoring chooses cache source when
                            present + caught up, else chain
                            source (already the source-of-truth
                            chooser; no behavioral change for
                            chain users)
```

No metapage version bump. No new WAL record types. No on-disk
format change.

## Testing

### Correctness (via existing scaffolding from #391)

- `validate_bm25_scoring` runs the index-scan top-k twice for
  LIMIT 10 and LIMIT 100, compares both runs to SQL-computed
  ground truth and to each other (covers 13 BM25 tests).
- `validate_index_vs_standalone` runs twice and compares cold,
  warm, and standalone.

Once the cache lands, both helpers exercise the cold/lazy-build
path (first run) and warm-cache path (second run) against
ground truth — automatically, with no test-file changes.

Additional correctness tests this PR adds:

1. **`test/sql/cache_spill.sql`**: explicit spill-boundary
   coverage. Writes a controlled batch, lets the cache build,
   forces a spill via `bm25_spill_index`, writes more, queries
   after each step. Verifies cache invalidation across the
   spill boundary and rebuild from new chain head.

2. **`test/scripts/replication_cache.sh`**: streaming-
   replication coverage with explicit cache-state assertions.
   Three test cases:

   - **C1: standby disables cache at runtime.**
     Start primary + standby. Set
     `pg_textsearch.memtable_cache_enabled = on` on both nodes.
     Drive a write+query workload on the primary, then query
     the standby. Assert via the `log_cache_state` debug GUC
     that the standby logs "cache disabled
     by recovery" and falls through to chain_source. Compare
     standby results to primary results: must match. This
     guards against a subtle failure mode where the
     `RecoveryInProgress()` check is missed and the cache
     silently builds on the standby, which would serve stale
     data after a primary spill since `spill_generation` is
     DSA-only and not bumped by replay.
   - **C2: standby long-lived backend across primary spill.**
     Reuses the existing `long_lived_open` /
     `long_lived_query` infra from replication_lib.sh. Holds
     an open psql session on the standby across a primary
     spill cycle (write → spill → write); the standby's
     answers must converge on the post-replay primary state,
     same as today. The cache GUC is on; if any cache code
     accidentally ran on the standby, this would fail with
     stale results.
   - **C3: post-promotion cold-build.**
     Builds on the existing D2 pattern in
     replication_failover.sh but adds explicit assertions
     against `log_cache_state`: the first query against the
     newly-promoted primary must log "cold-build", return the
     correct count, and a subsequent query must log "catchup
     no-op" (cursor already at tail).

   The other replication scripts (`replication.sh`,
   `replication_correctness.sh`, `replication_concurrency.sh`,
   `replication_failover.sh`, `replication_cascading.sh`,
   `replication_compat.sh`, `replication_spill_paths.sh`,
   `replication_parallel_build.sh`, `replication_issue_342.sh`)
   continue to run unchanged. With the cache GUC defaulting to
   `on`, each test exercises the cache lifecycle on the
   primary and the chain_source fallback on the standby
   automatically. No new bug class is introduced — the cache
   is derived state, so any divergence between cache results
   and chain_source results would show up immediately as a
   regression in `validate_bm25_scoring` /
   `validate_index_vs_standalone`.

3. **`test/sql/cache_memory_cap.sql`**: explicit memory-cap
   coverage. Configures a small `pg_textsearch.memory_limit`,
   triggers per-index soft cap (cold-build ABORT path),
   triggers global soft cap (eviction of largest), verifies
   chain_source fallback returns correct scores.

CI: existing `make installcheck` covers correctness via the
helpers above; the three new test files plug into the same
runner.

### A/B benchmarking (via existing workflows)

Reviewer-requested: benchmarking is part of testing, and we
reuse the existing GitHub Actions workflows rather than building
a parallel harness. Two workflows already exist:

- `.github/workflows/benchmark.yml` — daily ranking-quality
  benchmarks against Cranfield / MS MARCO / Wikipedia.
- `.github/workflows/benchmark-concurrent-insert.yml` — daily
  concurrent-INSERT throughput against MS MARCO.

Both are extended with a matrix dimension over
`pg_textsearch.memtable_cache_enabled`:

```yaml
strategy:
  matrix:
    memtable_cache: [off, on]
```

Each matrix leg runs the **same workload** through the same
runner script, exports the same metrics, and tags them with the
cache state (e.g., `bench_msmarco_query_p50_ms{cache=off}` vs
`bench_msmarco_query_p50_ms{cache=on}`). The existing metrics
publisher in `benchmarks/runner/run_benchmark.sh` already emits
`METRIC <name> <value>` lines; we add a `_cache_on` / `_cache_
off` suffix or a labels parameter.

A/B targets:

| Workload | Metric of interest | Expected effect |
|----------|--------------------|------------------|
| MS MARCO concurrent INSERT | inserts/sec at each concurrency | **No regression** with cache on — writes don't touch the cache, so the only delta is the once-per-spill-cycle cache-clear cost |
| MS MARCO mixed read/write | query p50/p95 latency | **Improvement** with cache on — the goal of this PR |
| Cranfield ranking quality | NDCG@10, MAP | **No change** — scoring is identical, cache only changes how posting lists are sourced |
| Wikipedia query throughput | queries/sec at each concurrency | **Improvement** with cache on; concurrent queries benefit from a shared cache |

Soft-fail thresholds initially (publish numbers, no CI gating).
After two weeks of nightly runs we'll have baselines; then
harden the gate as "cache=on must not regress more than X%
against cache=off on the INSERT workload, and must improve by
at least Y% on the query workload".

CI matrix overhead: both workflows already take 1-2h per run;
the matrix doubles that. Acceptable for nightly runs. The
manual `workflow_dispatch` interface gets a `cache_only` input
so developers can run just one leg during iteration.

The same A/B approach applies to local benchmarking via
`benchmarks/sql/memtable_resident.sql` (new): a SQL script that
runs a fixed mixed workload with the cache toggled on and off
and prints both numbers side by side.

## Terminology going forward

The "v1 / v2" labels in this doc refer to the historical
in-memory and on-disk implementations of the memtable; they're
unavoidable here because the doc explains a migration. The
**code and going-forward documentation should not perpetuate
these labels**. Once the cache lands, the descriptive names are:

| Use this                                      | Not this   |
|-----------------------------------------------|------------|
| "on-disk memtable" / "page chain" / "chain"   | "v2"       |
| "in-memory cache" / "memtable cache" / "cache"| "v1"       |
| "chain source" / "cache source"               | "v1/v2 source" |
| "memtable v2 redesign (#374)" → just "#374" or "the on-disk memtable redesign" | "memtable v2 redesign" |

The cleanup pass through `src/index/state.{c,h}`,
`src/index/metapage.{c,h}`, `src/access/build.c`, `src/mod.c`,
and `src/constants.h` rewrites the existing `v1`/`v2` comments
to the descriptive form, so the resurrected code lands with
the same convention.

Issue numbers (#374, #345, #349, #350, #377) remain — those
are persistent references; only the "vN" labels are dropped.

## What's out of scope (this PR)

- Backward compatibility for pre-1.3.0-dev indexes. The cache
  has no on-disk presence, so loading old indexes works as-is;
  no upgrade path needed.
- Per-query budget for incremental catchup. If catchup falls
  badly behind (e.g., a multi-million-row backfill without any
  intervening query), the first query post-backfill walks the
  whole tail. Solving this would require a background worker;
  parking until we see it in benchmarks.
- Selectively building only query-term posting lists in the
  cache. The chain source already does this; the cache builds
  the full term dictionary on cold build for simplicity. May
  revisit if cold-build cost becomes a problem.
- Custom rmgr. Explicitly out — the cache is volatile derived
  state.

## Open questions

1. **Cache + chain_tail compound source**: this PR serves
   queries either entirely from the cache (if catchup succeeds
   under the per-index soft cap) or entirely from chain_source
   (if catchup aborts mid-way under memory pressure).
   An alternative is a compound source that serves the cached
   posting lists for already-applied records and lazily walks
   the chain tail for the remainder. Compound is cheaper at
   open time (no eager catchup walk) but pays the chain-walk
   cost in `get_postings` per query term. Defer to a follow-up;
   not required for the perf-recovery goal.

2. **Cursor seq as authoritative vs debug-only**: the current
   design uses `(cursor.gen_spill_count, cursor.next_blkno,
   cursor.next_off)` as the authoritative "where are we" tuple
   and `cursor.seq` as a lock-free monotonic counter for "did
   anything change" cheap checks + debug asserts. Could promote
   seq to authoritative (use it as the apply-order index, with
   a side table mapping seq → (blk, off)). The tuple-based
   approach is simpler and avoids the side table; revisit only
   if profiling shows the cursor check costs us under
   contention.

## Scope delivered

The work lands as a set of self-contained, independently
reviewable changes (each leaves `make installcheck` green via
the helpers from #391):

- **Resurrected in-memory pieces** + **terminology cleanup**.
  `memtable/{memtable,posting,stringtable}.{c,h}` +
  `posting_entry.h` from `62308b82`, with comment headers
  rewritten to use "in-memory memtable cache" rather than
  "v1". Wired into the Makefile. The same pass sweeps
  `state.{c,h}`, `metapage.{c,h}`, `build.c`, `mod.c`,
  `constants.h` for the v1/v2 → on-disk/in-memory rename
  (see "Terminology going forward").
- **Cache lifecycle scaffolding**. Added `apply_lock`, `lock`,
  cursor fields, `estimated_bytes` to `TpMemtable`. Updated
  `state.c` init/teardown. Added `tp_cache_clear`. No
  mutation paths yet at this layer.
- **Apply protocol**. `apply_to_tail` (catchup) and the
  `cold_build` bootstrap path in `cache.{c,h}`. Both advance
  the cursor and share the same invariants (apply_lock EXCL
  serialization, generation-token check), but take
  `cache.lock` in different modes (SHARED for catchup, EXCL
  for cold_build). The chain walker is factored out of
  `chain_source.c` for reuse. Unit tests against synthetic
  cursors.
- **Read path**. `tp_memtable_cache_source_create` with cold
  lazy build + catchup. Scoring prefers the cache source when
  `pg_textsearch.memtable_cache_enabled` (default **on**) is
  true; `cache_source_create` returns NULL when
  `RecoveryInProgress()` is true, falling through to
  chain_source so standbys preserve today's behavior exactly.
  `pg_textsearch.log_cache_state` (debug GUC, default off)
  NOTICEs the per-query cache outcome (`cold_build` /
  `catchup_applied N records` / `catchup_noop` /
  `disabled_by_recovery` / `aborted_budget` / `disabled_by_
  guc`), used by the replication and memory-cap tests to make
  state-coverage assertions explicit. Both helpers from #391
  (`validate_bm25_scoring`, `validate_index_vs_standalone`)
  exercise cold + warm via the cache.
- **Spill consumption from cache**.
  `tp_write_segment_from_cache` runs after catchup, with a
  chain_source fallback when the budget is exceeded.
  `tp_spill_finalize` bumps `shared->spill_generation` under
  per-index EXCL **before** zeroing
  `meta.head_blkno`/`meta.tail_blkno`, so any reader acquiring
  per-index SHARED after the spill sees the new generation.
  `tp_spill_finalize` also clears the cache.
- **3-tier memory cap**. Per-index soft, global soft
  (`evict_largest` with `global_eviction_mutex` +
  skip-caller's-own-index), global hard. `cache_memory_cap.sql`
  exercises both same-index and cross-index eviction paths.
- **Replication and benchmark coverage** to follow as
  separate work items: `replication_cache.sh` (C1
  standby-runtime-disabled, C2 long-lived backend across
  spill, C3 post-promotion cold-build), and a
  `memtable_cache: [off, on]` matrix dimension in
  `benchmark.yml` / `benchmark-concurrent-insert.yml` for
  ongoing perf tracking.
