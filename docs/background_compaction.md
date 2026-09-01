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

Batching is greedy over a head prefix of the level, and a batch that ends up
holding a single segment combines nothing: merging it rewrites the whole
segment to produce a segment of the same size. Because spills prepend, a
level reads newest-first, so its oldest and largest runs — the ones already
past `max_segment_size` — sit at the tail. A pass therefore hands the
trailing run of single-segment batches back to the level before planning
anything further, and leaves those segments where they are, at the level
they were already on. Without that, one pairable pair at the head of a level
would authorize rewriting every over-budget segment behind it.

This applies to the level a pass chose to compact. When a pass instead
compacts a level to make room for its own output, promoting a
single-segment batch is what makes that room, so those are not handed
back.

Handing back a *trailing* run is what keeps this cheap. The selection is a
head prefix, so the chain behind it is untouched and returning segments is
just moving the level's head pointer back; nothing on disk changes. A
single-segment batch in the middle of a prefix is still rewritten, because
excising one would mean repointing its predecessor, and those writes do not
fit alongside the metapage update that publishes the pass.

The guarantee is about the visible segment layout, not about every physical
page:

* Until that metapage update, the existing segments stay authoritative, so a
  pass that fails leaves the layout it started from.
* A pass that fails can still have written pages. It drains the deferred-free
  tombstone chain before planning, and it merges its outputs before
  publishing, so a failure can leave reclaimed pages reclaimed and unpublished
  output pages allocated but unreachable. `VACUUM` does not recover those:
  it reclaims dead memtable pages and past-horizon tombstones, not pages a
  failed pass never linked. `REINDEX` is what returns them.

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
them about which levels are full.

## Restrictions

Permanent and unlogged indexes cannot be compacted in a read-only transaction,
and no index can be compacted during recovery. A local temporary index remains
available to its owning backend and may be compacted in a read-only
transaction.

Partitioned BM25 parent indexes have no physical storage and are rejected by
all four per-index functions. Call the functions on the physical indexes of
individual partitions instead.

## Full levels with nothing to compact

`bm25_needs_compaction()` is a cheap advisory signal derived from the level
counts alone. It answers "is any level at its segment threshold", not "is
there work to do". A level can sit at `segments_per_level` and still have
nothing to compact, because a pass runs only when at least one batch merges
two or more segments, and every candidate group here is a single over-budget
segment. Such a level is not carrying compaction debt — no pass would reduce
it — but a count-only signal cannot tell the two apart, so it reports true
while `bm25_compact_step()` returns false.

**`bm25_needs_compaction()` must not be used on its own as a retry or loop
condition.** It stays true on a full level that has nothing to reduce, so
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

Note that a level is a hybrid rank rather than a pure size class. A pass
places its output at whichever is higher: the output's size class, or one
level above the level the sources came from. A level's nominal ceiling is
`8MB * segments_per_level^level`, so the ladder's shape follows
`pg_textsearch.segments_per_level` (2 to 64, default 8) and is not fixed.

At the default fanout of 8 that ceiling reaches 4096MB at L3, while
`pg_textsearch.max_segment_size` caps a newly combined segment at 4095MB, so
under default settings no merge emits a segment above size class 3 and L4
through L7 are reached only by the one-level promotion. That is a property of
the default, not of the design: at `segments_per_level = 2` the L6 ceiling is
512MB and a 4095MB output lands at L7 on size alone.

`max_segment_size` also bounds only the segments a pass *combines*. An
existing segment larger than the current setting stays a valid uncombinable
singleton, so a segment at any level may exceed it.

## Spill-time dispatch

`pg_textsearch.compaction_mode` controls what happens when a spill leaves a
level at the compaction threshold:

- `inline` (the default) preserves synchronous compaction.
- `background` records one request per affected index and invokes the
  configured callback at transaction pre-commit.
- `off` leaves compaction debt for an explicit caller.

Set `pg_textsearch.compaction_request_function` to an unqualified or
schema-qualified function name. The function must accept one `regclass`
argument; its return type is ignored. Name validation is syntax-only so an
external scheduler can be installed independently of pg_textsearch.

Requests are backend-local, transaction-local, and deduplicated by index.
Aborted transactions dispatch nothing, pending index OIDs are revalidated
against the transaction's final catalog state, and prepared transactions
dispatch before prepare and then clear backend-local state. Temporary indexes
always compact inline because another backend cannot open them.

Each callback runs in a protected internal subtransaction. Its local effects
are rolled back after invocation, so the callback should hand work to a
facility whose submission survives that rollback. Ordinary lookup or callback
errors produce a warning and do not abort the writer transaction. Query
cancellation and server shutdown errors are rethrown.
This interface is scheduler-neutral and has no pg_durable dependency.
