# Native compaction controls

pg_textsearch provides scheduler-independent SQL functions for inspecting and
compacting one BM25 index:

- `bm25_level_counts(regclass)` returns the persisted segment count for each
  of the eight LSM levels.
- `bm25_needs_compaction(regclass)` reports whether any compactable level,
  L0 through L6, has reached `pg_textsearch.segments_per_level`.
- `bm25_compact(regclass)` runs compaction passes until no compactable level
  is left with debt this engine can reduce.
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
built and validated against the per-level segment capacity before a single
page is written, so a pass either publishes completely or leaves the index
exactly as it found it.

Because each pass is independently complete, `bm25_compact()` and repeated
`bm25_compact_step()` calls converge on the same layout. They differ only in
how long the lock is held.

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
still cannot compact, because every candidate group already exceeds
`max_segment_size` and an over-budget segment is an indivisible singleton. In
that case `bm25_needs_compaction()` reports true while `bm25_compact_step()`
returns false. A scheduler should treat a false return as "nothing further to
do for now" rather than retrying immediately.

L7 is the terminal level and is not itself compactable, so
`bm25_needs_compaction()` considers only L0 through L6. If L7 has no room for
a promotion out of L6, planning that pass raises
`bm25 segment count limit reached at level 7` before writing anything. Passes
that can legally run still run: `bm25_compact()` reduces the lower-level debt
it can and then reports the terminal conflict. Operators must resolve such a
layout rather than retrying it as ordinary compaction debt.
