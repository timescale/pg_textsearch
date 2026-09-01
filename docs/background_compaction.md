# Native compaction controls

pg_textsearch provides scheduler-independent SQL functions for inspecting and
compacting one BM25 index:

- `bm25_level_counts(regclass)` returns the persisted segment count for each
  of the eight LSM levels.
- `bm25_needs_compaction(regclass)` reports whether any level, L0 through L7,
  has reached `pg_textsearch.segments_per_level`.
- `bm25_compact(regclass)` runs compaction passes until no level is left with
  debt this engine can reduce.
- `bm25_compact_step(regclass)` runs at most one compaction pass and returns
  whether a pass ran.

Both mutating functions drive the same size-bounded compaction engine that
threshold compaction and `bm25_force_merge()` use, so they share its
capacity, sizing, and reclaim behavior.

## Passes

A pass is the engine's unit of work and its unit of publication. The planner
selects the lowest triggered level, groups its segments into batches that fit
`pg_textsearch.max_segment_size`, and recursively plans any additional
compaction needed to make room at the destination levels. The whole plan is
built and validated against the per-level segment capacity before any output
is merged, and every output a pass produces becomes visible in one metapage
update.

The guarantee is about the visible segment layout, not about every physical
page:

* Until that metapage update, the existing segments stay authoritative, so a
  pass that fails leaves the layout it started from.
* A pass that fails can still have written pages. It drains the deferred-free
  tombstone chain before planning, and it merges its outputs before
  publishing, so a failure can leave reclaimed pages reclaimed and unpublished
  output pages unreachable until the next `VACUUM` or `REINDEX`.

A published pass is **not undone by `ROLLBACK`**. The metapage update is a
physical change, so it survives abort of the surrounding transaction:

```sql
BEGIN;
SELECT bm25_compact_step('idx'::regclass);  -- t, layout now changed
ROLLBACK;                                   -- layout stays changed
```

Because each pass is independently complete, `bm25_compact()` and repeated
`bm25_compact_step()` calls converge on the same layout. They differ only in
how long the lock is held.

A pass is not a bounded amount of work. It runs only when at least one batch
merges two or more segments, but the prefix it selects may hold up to
`pg_textsearch.segments_per_level` segments, may span several batches, and may
pull in further batches to unblock a destination level. A segment that is
already over `pg_textsearch.max_segment_size` is never *combined* with
another, but it is still rewritten when it falls inside a running pass's
prefix. Treat `bm25_compact_step()` as one publication, not as a latency
bound.

Both functions act on segments only. They are the same operation the storage
layer runs at the tail of a spill, exposed for manual and scheduled use, so
neither flushes the memtable first. Use `bm25_spill_index()` for that, or
`bm25_force_merge()`, which spills and then compacts as aggressively as the
size budget allows.

## Locking

The two mutating functions require ownership of the index. They open the
relation with `RowExclusiveLock` and hold the index's `LW_EXCLUSIVE` lock
while merging. `bm25_compact()` holds that lock for the entire cascade.
`bm25_compact_step()` releases it after one pass, allowing callers to split a
cascade across transactions.

Holding an `LWLock` holds off interrupts, so neither function is cancellable
by `pg_cancel_backend()` or `statement_timeout` while a pass is running. This
is the main reason to prefer `bm25_compact_step()` for scheduled work:
`bm25_compact()` is uninterruptible for as long as the whole cascade takes.

`pg_textsearch.segments_per_level` is `PGC_SUSET`. The planner and
`bm25_needs_compaction()` both read the *calling* session's value, so a
scheduler session with a different setting than the writers will disagree with
them about which levels carry debt.

## Restrictions

Permanent and unlogged indexes cannot be compacted in a read-only transaction,
and no index can be compacted during recovery. A local temporary index remains
available to its owning backend and may be compacted in a read-only
transaction.

Partitioned BM25 parent indexes have no physical storage and are rejected by
all four per-index functions. Call the functions on the physical indexes of
individual partitions instead.

## Debt that cannot be reduced

`bm25_needs_compaction()` is a cheap advisory signal derived from the level
counts alone. A level can hold `segments_per_level` segments that the engine
still cannot compact, because a pass runs only when at least one batch merges
two or more segments, and every candidate group here is a single over-budget
segment. In that case `bm25_needs_compaction()` reports true while
`bm25_compact_step()` returns false.

**`bm25_needs_compaction()` must not be used on its own as a retry or loop
condition.** It does not go false when the debt it reports is unreducible, so
a scheduler that loops `WHILE bm25_needs_compaction(...) DO
bm25_compact_step(...)` never terminates on such an index. Drive the loop from
`bm25_compact_step()`'s return value instead, and stop as soon as it returns
false; use `bm25_needs_compaction()` only to decide whether it is worth
starting, and for observability.

The top level (L7) is where the ladder ends, but it is not a wall: a run that
would promote past it stays there instead, and the level compacts into itself
under the same size budget. It is therefore reducible like any other level,
carries no special count ceiling, and `bm25_needs_compaction()` considers
every level including L7.

Note that levels are cascade generations, not achievable size classes. A
level's nominal size is `8MB * 8^level`, but no merge may exceed
`pg_textsearch.max_segment_size`, whose default *and maximum* is 4095MB —
just under L3's 4096MB boundary. So no merge can ever emit a segment whose
size class is above L3; levels L4 through L7 are reached only by the
one-level promotion that each pass applies to its output. A segment sitting
at L7 is at most 4095MB, not the 2TB its level nominally denotes.
