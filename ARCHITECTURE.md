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
pg_textsearch has no custom WAL resource manager. The on-disk chain is
authoritative through crash recovery and physical replication.

Queries compose postings from the memtable and all published segments. Each
live heap TID occurs in at most one published segment. Segment-local numeric
`doc_id` values may repeat across segments. This disjointness permits merge and
scoring paths to avoid cross-segment document deduplication.

## Memtable Cache

The shared-memory memtable cache accelerates reads but is derived and
disposable. Writes update only the on-disk chain. Readers lazily build or catch
up the cache; generation mismatches, spills, eviction, or memory pressure can
drop it without affecting correctness. Standbys read the on-disk chain.

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

The `compaction` index option controls spill-time behavior:

- `inline` compacts threshold debt during the spill;
- `background` dispatches a pre-commit request when possible; temporary
  indexes, `CREATE INDEX`, autovacuum, callback re-entry, and other
  no-dispatch contexts compact inline;
- `off` leaves debt for explicit maintenance.

Prepared transactions do not flush queued background requests. Unconfigured,
unresolvable, or failed callbacks do not fall back inline; the compaction debt
remains for a later spill or explicit maintenance.

`bm25_compact()` drives reducible debt to completion under one per-index lock.
`bm25_compact_step()` runs at most one pass. Drive repeated maintenance from
the return value of `bm25_compact_step()`, not
`bm25_needs_compaction()`, because over-budget segments can leave a level
permanently above its advisory threshold.

A compaction pass publishes its replacement layout in one metapage update.
Published physical changes are not undone by transaction rollback.

## Deferred Reclaim

Spill and compaction unlink old pages before they can be safely reused. Dead
memtable pages and displaced segment pages are parked with a transaction
horizon and returned to the free-space map only after
`GetOldestNonRemovableTransactionId` passes that horizon.

Query-serving hot standbys require `hot_standby_feedback = on` so their oldest
snapshots hold the primary's reclaim horizon back. Use
`bm25_pending_free_pages()` to observe displaced segment pages awaiting reuse.
