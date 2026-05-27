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
regardless of query selectivity.

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
  incrementally updated by writers, dropped + rebuilt on spill
  or invalidation. Crashing or losing it is harmless — the next
  query rebuilds from the chain.
- An **apply cursor** tracks the next chain record the cache
  expects to consume. Three paths can advance the cursor —
  writer hook, reader catchup, cold lazy build — and all three
  share a single `apply_lock`-serialized helper
  (`apply_records_up_to`). The cursor IS the commit point;
  every record is applied to the cache exactly once per
  generation.
- The writer hook attaches at the tail of `tp_add_document_
  terms` while the writer still holds per-index LWLock SHARED.
  Spill (which needs per-index LWLock EXCL) cannot fire
  concurrently with cache application, so spill-during-apply is
  excluded by the existing v2 lock contract.

The cache is the v1 memtable. The chain is the v2 memtable. The
only conceptual additions are the apply cursor + the shared
apply protocol that keeps the cache in sync with the chain.

## What we resurrect

The v1 cutover commit (`023c3841`) deleted seven files we want
back, with surgical adjustments for v2:

| Path                               | LOC | Reuse |
|------------------------------------|----:|-------|
| `src/memtable/memtable.h`          |  44 | unchanged surface |
| `src/memtable/memtable.c`          |  11 | stub, was always tiny |
| `src/memtable/posting.h`           |  86 | unchanged surface |
| `src/memtable/posting.c`           | 458 | unchanged |
| `src/memtable/posting_entry.h`     |  24 | unchanged |
| `src/memtable/stringtable.h`       |  97 | unchanged surface |
| `src/memtable/stringtable.c`       | 561 | mostly unchanged; drop primary-write paths that now live in `tp_add_document_terms` |
| `src/memtable/source.h`            |  18 | rename `tp_memtable_source_create` → `tp_memtable_cache_source_create`; semantics unchanged |
| `src/memtable/source.c`            | 167 | as above |

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

### Required API change to `tp_memtable_append`

`tp_memtable_append` currently returns `BlockNumber` — the page
of the just-written record. That's insufficient: the cache hook
needs (a) the explicit bootstrap signal, (b) the within-page
offset, (c) the logical "next position" after this record (which
differs from `(record_blkno, off+1)` for fragment appends because
the fragment chain ends on a fresh tail page), and (d) the
`meta.head_blkno` observed at publish time as the generation
token. This PR changes the return type to
`TpMemtableAppendResult` (defined in "The apply cursor"
section). All callers (currently only `tp_add_document_terms`)
are updated in lockstep; the on-disk format is unchanged.

## Data structures

```c
/* in src/index/state.h, extending the existing TpMemtable stub */
typedef struct TpMemtable
{
    /* --- inherited v1 fields, currently dead but still allocated --- */
    dshash_table_handle string_hash_handle;  /* term  → TpStringHashEntry */
    dshash_table_handle doc_lengths_handle;  /* ctid  → TpDocLengthEntry  */
    pg_atomic_uint64    total_postings;
    pg_atomic_uint64    num_terms;
    pg_atomic_uint64    total_term_len;

    /* --- new in cache design --- */

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
    BlockNumber         cursor_gen_head_blkno; /* generation
                                                * token: equals
                                                * meta.head_blkno
                                                * the cache was
                                                * built against */
    BlockNumber         cursor_next_blkno;     /* logical next
                                                * record position */
    uint16              cursor_next_off;
    pg_atomic_uint64    cursor_seq;            /* monotonic
                                                * apply counter,
                                                * lock-free read */

    /* memory accounting */
    pg_atomic_uint64    estimated_bytes;       /* cached cache
                                                * footprint */
} TpMemtable;
```

`TpStringHashEntry`, `TpPostingList`, `TpPostingEntry`,
`TpDocLengthEntry` come back verbatim from `62308b82`. They're
already designed for the dshash + DSA pattern.

The vestigial `spill_generation` atomic in `TpSharedIndexState`
(see `state.h:108`) is repurposed: bumped under per-index EXCL
at the end of `tp_spill_finalize`, read by any cache path that
took per-index SHARED to check "did a spill complete between my
acquire and now". Combined with the `cursor_gen_head_blkno`
check inside the apply_lock, we have a two-tier spill detector
(cheap atomic read first, authoritative metapage compare on
mismatch).

## Lock order

Building on the v2 contract in `chain_source.h`:

```
per-index LWLock          (SHARED for readers, EXCL for spill)
  ├─► cache.apply_lock    (EXCL for any path that mutates the
  │                         cache or advances the watermark:
  │                         writer hook, reader catchup, cold
  │                         lazy build, evict.  Held only for
  │                         the duration of the apply itself.)
  │     └─► cache.lock    (SHARED for any path serving a
  │                         TpDataSource — held for the lifetime
  │                         of the source.  EXCL for drop/evict
  │                         only; apply_lock holders may take
  │                         SHARED.)
  │           └─► dshash buckets
  │                 └─► TpPostingList.lock     (per-term, leaf)
  └─► chain page buffers  (existing v2 contract; SHARED for
                            readers via metapage → head → next
                            walks, EXCL only inside log.c)
```

Two separate cache locks because they have different lifetimes:

- `cache.apply_lock` (EXCL only) is held briefly while mutating
  the cache structure or advancing the watermark. It's the
  serialization point for "who's writing into the cache right
  now". This is the lock that prevents the writer-hook /
  cold-build / reader-catchup double-apply races.
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

## The apply cursor

The watermark is not a passive metadata field. It is the
**authoritative apply cursor** for the cache, and every path
that mutates the cache must consult and advance it under
`cache.apply_lock` EXCL.

### What the cursor stores

```c
typedef struct TpCacheCursor
{
    /* Identifies the chain generation the cache was built from.
     * Compared against meta.head_blkno on every apply / serve to
     * detect spills. */
    BlockNumber gen_head_blkno;

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
     * because (gen_head_blkno, next_blkno, next_off) is sufficient,
     * but it makes invariants easier to assert. */
    pg_atomic_uint64 seq;
} TpCacheCursor;
```

The cursor lives in `TpMemtable` (shared, DSA). Mutations of
`gen_head_blkno`, `next_blkno`, `next_off` happen under
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

### Apply protocol (single shared protocol for all mutators)

Every path that wants to insert a record into the cache —
whether it's a writer's just-completed append, a reader doing
incremental catchup, or a cold lazy build from scratch —
**MUST** follow this protocol:

```
/*
 * Advance the cache cursor to `target_next`, applying every
 * chain record encountered along the way.  `target_next` is
 * itself a cursor value — the (blkno, off) the cursor should
 * hold once we've applied the record(s) the caller cares about.
 *
 * `target_next == NULL` means "apply everything currently
 * visible at meta.tail".
 */
apply_records_up_to(target_next):
    Assert(holding per-index LWLock SHARED or stronger)
    LWLockAcquire(cache.apply_lock, LW_EXCLUSIVE)
    LWLockAcquire(cache.lock,       LW_SHARED)

    if (cursor.gen_head_blkno != meta.head_blkno):
        # Spill happened between caller's perception of the chain
        # and now.  Drop the cache (under cache.lock EXCL upgrade)
        # and return DROPPED to caller.  Caller decides whether
        # to retry (cold-build) or skip (writer hook for a record
        # that's now orphaned by the spill).
        promote_cache_lock_to_excl_and_drop()
        release apply_lock
        return DROPPED

    walker = chain_walker_open_at(cursor.next_blkno, cursor.next_off)
    while True:
        # Stop condition.  Two equivalent forms:
        #
        #   (a) If target_next was provided and the cursor has
        #       reached it, stop.  Comparison is EQUALITY on
        #       (blkno, off) — no ordering arithmetic, robust
        #       against fragment-page allocation order.
        #   (b) If walker has no next record (cursor at logical
        #       tail), stop.
        if target_next != NULL
           AND cursor.next_blkno == target_next.blkno
           AND cursor.next_off   == target_next.off:
            break
        if NOT walker.has_next():
            break

        rec = walker.read_record()        # under chain page SHARED
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

Stop-condition note: when callers know exactly which record they
care about (writer hook with its `result.post_apply_next_*`),
form (a) lets them avoid pulling extra chain pages into the
buffer LRU. Reader catchup uses `target_next == NULL` and
relies on form (b).

Key properties:

- **`apply_lock` EXCL serializes ALL appliers**. No writer-hook
  can race with reader-catchup or cold-build. No two writers can
  step on each other's watermark advancement.
- **Idempotent for re-entrant callers**. A writer that just
  appended record at position `P` calls `apply_records_up_to(P)`.
  If catchup already happened to apply `P`, the walker starts at
  `P+1`, finds nothing, and returns immediately. The writer's
  contribution is the call itself, not the act of inserting; the
  cursor decides who actually does the insert.
- **Bridges write+read paths**. A writer who finds `cursor.next`
  is far behind its just-appended record will catch the cache
  up across that gap. Inversely, a reader who arrives between
  two writers' chain publishes catches the cache up to whatever
  was most recently published.
- **No double-apply**. Because the cursor advances strictly
  per-record under the apply_lock, every record is applied
  exactly once across the lifetime of the cache (within one
  `gen_head_blkno` generation).

The walker reuses the existing chain-walker logic in
`chain_source.c`, factored into a reusable helper.

### `TpMemtableAppendResult`

`tp_memtable_append` is updated to return:

```c
typedef struct TpMemtableAppendResult
{
    bool         bootstrapped;        /* this append created the
                                       * chain (meta.head was
                                       * InvalidBlockNumber on
                                       * entry, valid on return) */
    bool         fragmented;          /* used the fragment path */
    BlockNumber  record_blkno;        /* where the head record
                                       * lives (Thead for fragments,
                                       * tail page for normal) */
    uint16       record_off;          /* slot within record_blkno */
    BlockNumber  post_apply_next_blkno; /* logical next position
                                         * after this record */
    uint16       post_apply_next_off;
    BlockNumber  observed_head_blkno;  /* meta.head_blkno seen by
                                        * the publish step */
} TpMemtableAppendResult;
```

The cache hook consumes this struct, not just a `BlockNumber`.
`bootstrapped` is the explicit bootstrap signal. The
`post_apply_next_*` fields tell the hook the cursor value it
should reach. `observed_head_blkno` is the generation token the
hook compares against `cursor.gen_head_blkno`.

## Write path

`tp_add_document_terms()` is the single primary-side ingest
entry point. Per `log.h:70-73`, it **leaves the per-index
LWLock held SHARED on return**. That guarantee is what makes
the cache hook safe against spill:

```
tp_add_document_terms(state, rel, ctid, vec_bytes, vec_len, ...) {
    /* existing v2 code (abridged): */
    Assert(holding per-index LWLock SHARED);
    result = tp_memtable_append(rel, ctid, doc_length,
                                vec_bytes, vec_len);
    atomic_add(state->shared->total_docs, 1);
    atomic_add(state->shared->total_len, doc_length);
    /* per-index LWLock SHARED is STILL held here */

    /* new: cache update hook */
    if (!RecoveryInProgress()
        && tp_memtable_cache_enabled
        && !state->is_build_mode) {
        tp_cache_apply_local_append(state, &result,
                                    ctid, doc_length,
                                    vec_bytes, vec_len);
    }
    /* caller releases per-index LWLock SHARED */
}
```

Inside `tp_cache_apply_local_append`:

```
tp_cache_apply_local_append(state, result, ctid, len, vec, n):
    Assert(holding per-index LWLock SHARED)

    /* Step 1: quick generation check, no locks beyond per-index.
     * If meta.head_blkno != result->observed_head_blkno, the
     * record we just published is now orphaned by a spill we
     * raced against.  Skip the cache.  The next query will
     * lazy-build. */
    if (read meta.head_blkno != result.observed_head_blkno):
        return

    /* Step 2: bootstrap the cache on the chain's first-ever
     * append.  Cheap to detect via result.bootstrapped.  Without
     * this, every cache would need at least one query before
     * existing, which masks write-side regressions. */
    if (result.bootstrapped):
        LWLockAcquire(cache.apply_lock, LW_EXCLUSIVE)
        if (cache still empty):
            initialize_empty_cache()
            cursor.gen_head_blkno = result.observed_head_blkno
            cursor.next_blkno    = result.record_blkno
            cursor.next_off      = result.record_off
            /* fall through to apply our own record */
        LWLockRelease(cache.apply_lock)

    /* Step 3: standard apply path.  If the cache is empty
     * (string_hash_handle == DSHASH_HANDLE_INVALID) and we
     * didn't just bootstrap, skip — no need to lazy-build on
     * the write side. */
    if (cache empty): return

    /* Step 4: apply up to and including our record.  This may
     * also catch up earlier records published by other backends
     * that haven't been applied yet.  Idempotent: if catchup
     * already raced ahead and applied our record, the walker
     * sees nothing to do. */
    apply_records_up_to(
        target_next = {result.post_apply_next_blkno,
                       result.post_apply_next_off})

    /* Step 5: memory-cap check (cheap atomic read).  If we
     * crossed the per-index soft cap by this insert, request a
     * spill at xact end (don't spill mid-statement). */
    if (estimated_bytes > per_index_soft_cap):
        request_xact_end_spill(state)
```

Key safety properties:

- **Spill cannot fire while the hook is running**. The hook runs
  under per-index SHARED, which excludes the EXCL needed by
  `tp_spill`. Generation mismatch in Step 1 only happens if a
  concurrent writer raced us between our chain publish and our
  generation read — a window so narrow it's almost theoretical,
  but the explicit check costs one atomic load. The `cursor.
  gen_head_blkno` check inside `apply_lock` is the
  authoritative one.
- **No double-apply with concurrent catchup readers**. Both go
  through `apply_records_up_to` under `apply_lock` EXCL. The
  cursor advances exactly once per record.
- **No double-apply with cold lazy build**. Cold build acquires
  `apply_lock` EXCL and walks head→tail; if a writer's hook
  fires while build is in progress, it queues on `apply_lock`,
  then when it runs finds the cursor already at or past its
  record and applies nothing.
- **Bootstrap is the only path that creates a cache from the
  write side**. All other "no cache yet" appends defer cache
  creation to the next query's lazy build.

## Read path

`tp_memtable_cache_source_create(state, rel, query_terms,
query_term_count)` mirrors the chain source contract:

```
tp_memtable_cache_source_create(state, rel, terms, nterms):
    Assert(holding per-index LWLock SHARED)

    /* Step 1: catchup or build, under apply_lock EXCL.  Passing
     * NULL means "apply to current logical tail". */
    for retry in [first, second]:
        result = apply_records_up_to(target_next = NULL)
        if result == OK:
            break
        Assert(result == DROPPED)  /* spill detected */
        /* Step 1a: cold lazy build into the post-spill chain.
         * Done once; second DROPPED in a row is a bug or a
         * legitimate burst of spills, in which case we just
         * fall back to chain_source. */
        if first iteration:
            cold_build(rel)
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
    cursor.gen_head_blkno = meta.head_blkno
    cursor.next_blkno    = meta.head_blkno
    cursor.next_off      = 0

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

- **Hot path = `apply_records_up_to` returning OK on the first
  call with the walker finding 0 records**. In steady state
  (writes have already applied themselves via the write hook),
  the cache is caught up, `apply_lock` is briefly acquired,
  walker finds nothing, lock released — single LWLock acquire +
  release per query open. This is the fast path we're tuning for.
- **`get_postings(term)`** on the returned source does
  `tp_string_table_lookup` under dshash bucket lock, acquires
  `TpPostingList.lock` SHARED, copies entries into heap-
  allocated `TpPostingData`, releases. Identical to v1.
- **Hold `cache.lock` SHARED across the entire scan**. This
  excludes drop/evict for the source's lifetime — necessary
  because the source's served `TpPostingData` references DSA
  pointers that drop would invalidate. Per-query LIMIT (default
  1000) bounds the lifetime.
- **Cold-build ABORT path** is the read-side equivalent of the
  per-index soft cap on the write side. A cache that won't fit
  in budget isn't worth building; chain_source remains correct
  if slower.

## Spill path

Spill currently constructs a `chain_source` and feeds it into
the segment writer. With the cache, spill can consume the cache
directly when caught up:

```
tp_spill(state, rel) {
    LWLockAcquire(per-index LWLock, LW_EXCLUSIVE);  /* existing */

    /* per-index EXCL excludes:
     *   - all writer hooks (they hold per-index SHARED)
     *   - all read-path apply protocols (cold build, catchup)
     *   - the cache's TpDataSource lifetime locks
     * No cache mutator can be running concurrently. */

    LWLockAcquire(cache.apply_lock, LW_EXCLUSIVE);
    LWLockAcquire(cache.lock,       LW_EXCLUSIVE);

    if (cache exists AND cursor caught up to (tail_blkno, tail_count)):
        tp_write_segment_from_cache(state, &totals);
    else:
        /* cache stale or absent — fall back to chain walk */
        src = tp_memtable_chain_source_create(state, rel, NULL, 0);
        tp_write_segment_from_source(src, &totals);
        tp_source_close(src);

    tp_spill_finalize(rel, new_segment_root,
                      totals.docs, totals.len);
    /* spill_finalize zeroes meta.head_blkno/meta.tail_blkno
     * and increments state->shared->spill_generation atomically. */

    tp_cache_clear(state);   /* deallocates dshash tables,
                              * sets handles to DSHASH_HANDLE_
                              * INVALID, cursor.gen_head_blkno =
                              * InvalidBlockNumber */

    LWLockRelease(cache.lock);
    LWLockRelease(cache.apply_lock);
    LWLockRelease(per-index LWLock);
}
```

The `cursor caught up` check is `cursor.gen_head_blkno ==
meta.head_blkno && cursor.next_blkno == meta.tail_blkno &&
cursor.next_off == current_tail_line_pointer_count`. If a
partial catchup would be needed, we fall back to the chain
walk rather than running catchup under per-index EXCL — it's
the same I/O either way and chain_source is well-tested.

After spill, `tp_cache_clear` deallocates the dshash tables.
The next write hook bootstraps a new cache; or the next query
lazy-builds. Both go through the apply protocol so neither
double-applies.

## Standby / replication

WAL replay does not load `pg_textsearch.so`, so no extension code
runs. The cache cannot be maintained from replay. Behaviors:

- **Read on standby**: `RecoveryInProgress() == true`, but the
  read hook does not care — it builds from whatever the chain
  says. Lazy build works identically to the primary. Each
  read-path apply checks `cursor.gen_head_blkno == meta.
  head_blkno`; if replay applied a spill record, the check
  fails and the cache is dropped + rebuilt.

- **Spill replay on standby**: replay zeros `meta.head_blkno` /
  `meta.tail_blkno`. The next query observes the generation
  mismatch and drops + rebuilds. No code runs on the standby
  during replay, so no cache mutation happens between the
  metapage change and the next query.

- **Promotion**: the standby's last-built cache reflects the
  chain it was built from. Two cases:

  1. **No write activity between promotion and the first read**:
     reads keep using the cache. The apply protocol's
     `cursor.gen_head_blkno == meta.head_blkno` check holds (no
     spill happened) and there's nothing new on the chain.
  2. **A primary-style write fires first**: the write hook calls
     `apply_records_up_to(result.post_apply_next_*)`. This
     **catches the cache up across any records that the
     standby's chain accumulated between cache-build and
     promotion** (e.g., records replicated during the gap)
     before applying its own record. The hook is the same path
     a steady-state primary write takes; it is correct by
     construction.

No new cross-recovery invariants beyond the `cursor.
gen_head_blkno` check are required. The cursor IS the standby-
promotion safety net.

## Memory cap (3 tiers)

`pg_textsearch.memory_limit` (KB) is the global ceiling across
all per-index caches in this backend's shared memory view.
Three thresholds:

1. **Per-index soft cap = `memory_limit / 8`**. Enforced on
   **every** allocation path:
   - **Write hook**: after apply, if `estimated_bytes` crossed
     the cap, request an xact-end spill via
     `request_xact_end_spill(state)` (the existing v2
     auto-spill mechanism on terms-added or pages-added
     threshold). Don't spill mid-statement.
   - **Cold lazy build**: check on every record-apply. If the
     **next** apply would exceed the cap, **abort the partial
     build**: free in-flight dshash entries, reset handles to
     `DSHASH_HANDLE_INVALID`, release locks, return ABORT to
     caller. Caller falls back to `chain_source`.
   - **Reader catchup**: same check on every record. On
     exceed, abort catchup (cache stays valid through the
     last successfully applied record, but cursor doesn't
     advance) and the read source falls back to chain_source
     for the remaining gap. (Cache + chain_source compound is
     not implemented in this PR — see open question.)

2. **Global soft cap = `memory_limit / 2`**. When the sum of
   all per-index caches' `estimated_bytes` crosses this, the
   **largest** cache is **evicted** (`tp_cache_clear`, NOT
   spilled — the chain is still source of truth). Eviction
   protocol:

   ```
   evict_largest():
     /* select target without holding any cache lock */
     target = argmax over all indexes of estimated_bytes

     /* acquire in canonical order to avoid deadlock */
     LWLockAcquire(target's per-index LWLock, LW_EXCLUSIVE)
     LWLockAcquire(target's cache.apply_lock, LW_EXCLUSIVE)
     LWLockAcquire(target's cache.lock,       LW_EXCLUSIVE)

     /* re-check after lock acquire: another backend may have
      * evicted the same target. */
     if cache still present and estimated_bytes >= threshold:
         tp_cache_clear(target)
         atomic_sub(global estimated_total_bytes,
                    target.estimated_bytes)

     release in reverse order
   ```

   The **target per-index LWLock must be acquired BEFORE
   cache.apply_lock and cache.lock** to maintain the global
   lock order. This means eviction waits on any active writer
   or reader of the target index. That's correct: we can't
   yank the dshash tables out from under a concurrent
   `apply_records_up_to` even though it holds only apply_lock
   EXCL — the chain page buffer lock and posting list locks it
   transitively holds need a clean shutdown.

3. **Global hard cap = `memory_limit`**. Reached only if soft
   cap eviction failed to make room (rare race). The triggering
   write **ERRORs** with `out_of_shared_memory` rather than
   silently exceeding the cap. The write to the on-disk chain
   has already succeeded by this point (this is the cache
   hook); the only effect is leaving the cache partially behind.

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

The global-cap check runs on every Nth write (amortized via
`local_state->docs_since_global_check`, same pattern v1 used)
and on every cold-build / catchup apply. Reads of an already-
caught-up cache pay only the per-index estimated_bytes touch.

## Code layout

```
src/memtable/
    arena.{c,h}             unchanged
    expull.{c,h}            unchanged
    chain_source.{c,h}      unchanged interface; tp_spill consumes
                            cache when present, else chain_source
    log.{c,h}               tp_add_document_terms gains the
                            cache-update hook (one new call)
    page.{c,h}              unchanged
    scan.{c,h}              unchanged
    memtable.{c,h}          restored from 62308b82; thin stub
    posting.{c,h}           restored from 62308b82
    posting_entry.h         restored from 62308b82
    stringtable.{c,h}       restored from 62308b82
    cache.{c,h}             NEW — watermark, build, sync, evict,
                            spill-helper, and the
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

Test scaffolding already landed in #391:
- `validate_bm25_scoring` runs the index-scan top-k twice for
  LIMIT 10 and LIMIT 100, compares both runs to SQL-computed
  ground truth and to each other (covers 13 BM25 tests).
- `validate_index_vs_standalone` runs twice and compares cold,
  warm, and standalone.

Once the cache lands, both helpers exercise the cold/lazy-build
path (first run) and warm-cache path (second run) against
ground truth — automatically, with no test-file changes.

Additional tests this PR adds:

1. **`test/sql/cache_spill.sql`**: explicit spill-boundary
   coverage. Writes a controlled batch, lets the cache build,
   forces a spill via `bm25_spill_index`, writes more, queries
   after each step. Verifies cache invalidation across the
   spill boundary and rebuild from new chain head.

2. **`test/scripts/replication_cache.sh`**: streaming-
   replication coverage. Primary builds cache, replica lazy-
   builds from replicated chain, scoring matches across both.
   Spill replay on replica invalidates and rebuilds.

3. **`benchmarks/sql/memtable_resident.sql` +
   `run_memtable_resident.sh`**: warm-cache perf goal. Emits
   `METRIC memtable_resident_p50_ms <ms>`. CI threshold TBD
   from baseline run.

CI: existing `make installcheck` covers correctness via the
helpers above. The benchmark wiring is added to `benchmark.yml`
with a soft-fail threshold initially, hardened once we have a
stable baseline.

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

2. **Watermark seq as authoritative vs debug-only**: the
   current design uses `(cursor.gen_head_blkno,
   cursor.next_blkno, cursor.next_off)` as the authoritative
   "where are we" tuple and `cursor.seq` as a lock-free
   monotonic counter for "did anything change" cheap checks +
   debug asserts. Could promote seq to authoritative (use it
   as the apply-order index, with a side table mapping seq →
   (blk, off)). The tuple-based approach is simpler and avoids
   the side table; revisit only if profiling shows the cursor
   check costs us under contention.

3. **Where the request_xact_end_spill check fires**: the v2
   auto-spill already runs from
   `pg_textsearch.memtable_pages_threshold` and
   `pg_textsearch.bulk_load_threshold`. The cache adds a third
   trigger (per-index soft cap on estimated_bytes). Whether
   these should all share a single "request spill" mechanism
   or remain three independent checks is a code-org question;
   default to extending the v2 mechanism with a new reason
   field.

## Phases

Implementation order (each independently reviewable; each leaves
`make installcheck` green via the helpers from #391):

0. **`tp_memtable_append` API change**. Introduce
   `TpMemtableAppendResult`; update all callers. No cache code
   yet. Smallest possible diff.
1. **Resurrect v1 in-memory pieces**.
   `memtable/{memtable,posting,stringtable}.{c,h}` +
   `posting_entry.h` from `62308b82`. Wire into Makefile.
   Compile-clean, no callers.
2. **Cache lifecycle scaffolding**. Add `apply_lock`, `lock`,
   cursor fields, `estimated_bytes` to `TpMemtable`. Update
   `state.c` init/teardown. Add `tp_cache_clear`. No mutation
   paths yet.
3. **Apply protocol**. Implement `apply_records_up_to` in
   `cache.{c,h}` — the single shared helper used by all three
   apply paths. Factor the chain walker out of `chain_source.c`
   for reuse. Unit tests covering the protocol against synthetic
   cursors.
4. **Read path**. Implement `tp_memtable_cache_source_create`
   with cold lazy build via `apply_records_up_to(meta.tail)`.
   Wire scoring to prefer the cache source when
   `tp_memtable_cache_enabled` is on. At this point queries on
   warm cache hit the apply protocol; new writes are still
   chain-only and the cache is dropped + rebuilt across every
   write batch.
5. **Write hook + bootstrap**. Add `tp_cache_apply_local_append`
   to `tp_add_document_terms`. Cache stays caught up across
   writes; readers stop seeing the drop/rebuild path.
6. **Spill consumption from cache**. `tp_write_segment_from_
   cache` when caught up; chain fallback otherwise. `tp_spill_
   finalize` clears the cache.
7. **3-tier memory cap**. Per-index soft, global soft (eviction
   with canonical lock order), global hard. Wire benchmark.
   Decide threshold from baseline run. Ship.
