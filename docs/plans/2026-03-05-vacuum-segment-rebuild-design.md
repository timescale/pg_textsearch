# VACUUM Dead Tuple Handling via Segment Rebuild

**Date**: 2026-03-05
**Status**: Approved
**Tracks**: GitHub issue #264 (future bitmap optimization)

## Problem

`tp_bulkdelete` calls the VACUUM callback for each indexed CTID but
discards the return value. Dead index entries are never removed. After
VACUUM frees heap line pointers and CTIDs are reused by new inserts,
the index returns wrong rows.

### The CTID reuse bug

1. Row A at CTID (42,5) is indexed with term "database"
2. Row A is deleted, plain VACUUM runs
3. `ambulkdelete` is supposed to remove dead index entries before
   VACUUM frees heap line pointers — but we don't
4. New row B gets CTID (42,5), contains "networking"
5. Index says (42,5) matches "database" — Postgres fetches row B,
   visibility check passes (row B is live), wrong row returned

For `ORDER BY ... LIMIT` queries (our primary pattern), Postgres
trusts the index ordering and does not re-evaluate the `<@>` operator
against the heap tuple.

## Design

### Algorithm

```
tp_bulkdelete(info, stats, callback, callback_state):

  Phase 1: Spill memtable
    If memtable is non-empty, spill to L0 segment.
    All index data is now in segments.

  Phase 2: Identify affected segments
    For each level, walk segment chain:
      For each segment, read docmap (CTID arrays).
      For each CTID, call callback.
      If any callback returns true: mark segment "affected".
      Track dead count per segment for stats.
    Memory: O(segments) -- one boolean + count per segment.

  Phase 3: Rebuild affected segments
    For each affected segment:
      Read docmap again.
      For each CTID, call callback again:
        If dead: skip.
        If live: fetch heap tuple, tokenize, add to TpBuildContext.
      Write new segment via tp_write_segment_from_build_ctx.
      Replace in chain (update prev->next_segment or level_head).
      Free old segment pages.
      If all docs dead: remove from chain entirely.

  Phase 4: Update metapage statistics
    Recount total_docs and total_len from all segments.
    Report accurate tuples_removed in stats.
```

### Key properties

- **No dead CTID storage.** O(segments) memory, not O(dead_CTIDs).
  We mark segments as "affected" and call the callback a second time
  during rebuild to filter.
- **Callback called twice** for affected segments (once to identify,
  once to filter during rebuild). The callback is O(1) per call.
- **Unaffected segments untouched** beyond reading their docmap in
  Phase 2.
- **Reuses existing build infrastructure.** TpBuildContext and
  tp_write_segment_from_build_ctx handle tokenization and segment
  writing.
- **Heap access via `info->heaprel`**, already opened by VACUUM.
- **Text config OID from metapage**, needed for tokenization.

### Chain management

Segments form a singly-linked list per level via `next_segment`
pointers.

- **Head replacement**: update `metap->level_heads[level]`.
- **Mid-chain replacement**: update `prev->next_segment`.
- **New segment**: set `new->next_segment = old->next_segment`,
  same level.
- **Empty segment** (all docs dead): unlink from chain, decrement
  `level_counts[level]`.

### Merge path safety

Normal compaction already passes `disjoint_sources = false`
(`merge.c:1587`), using N-way CTID-comparison merge. No code change
needed. The only caller using `disjoint_sources = true` is parallel
build (`build_parallel.c:751`), where workers have genuinely disjoint
heap block ranges. Add a comment documenting that this invariant is
load-bearing after VACUUM rebuilds.

### CREATE INDEX CONCURRENTLY

The CIC validation path also calls `ambulkdelete` with a callback that
collects TIDs (returns false for all). This path is unaffected: no
dead CTIDs means no segments are marked affected, so we skip directly
to returning stats.

### Statistics

After rebuilding, total_docs and total_len are recounted by summing
across all segments. Per-term doc_freq in rebuilt segments is naturally
correct (computed from live docs only). Non-rebuilt segments retain
their original stats — a minor inaccuracy that #264 will address.

## Limitations

- **Expensive for large segments with few deletes.** Rebuilding a
  100K-doc segment to remove 1 dead tuple re-tokenizes 99,999
  documents. The future bitmap approach (#264) avoids this.
- **BM25 statistics in non-rebuilt segments may be slightly stale.**
  IDF calculations use doc_freq values that include deleted documents
  in segments that weren't rebuilt.
- **Double callback iteration for affected segments.** Acceptable
  since the callback is O(1).

## Testing

1. Insert docs, delete some, VACUUM, insert new rows that reuse
   CTIDs, verify correct results (no false positives).
2. Force spill to create segments, delete, VACUUM, verify segment
   docmap is clean.
3. Delete all docs in a segment, verify it is removed from the chain.
4. Multi-level test: segments at L0 and L1, delete from both, VACUUM,
   verify chain integrity.
5. VACUUM with no dead tuples (no-op path).
6. Existing vacuum and vacuum_extended tests continue to pass.

## Future work

- **Deleted-document bitmap** (#264): Per-segment bitmap of deleted
  doc IDs. O(dead_docs) work during VACUUM instead of
  O(all_docs_in_segment). Enables scan-time filtering and fixes the
  LIMIT shortfall.
- **Background compaction with dead tuple filtering**: Drop dead
  entries during merge without a separate VACUUM step.
