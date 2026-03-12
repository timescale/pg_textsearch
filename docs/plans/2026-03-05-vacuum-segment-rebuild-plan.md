# VACUUM Segment Rebuild Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans
> to implement this plan task-by-task.

**Goal:** Fix the CTID reuse correctness bug by rebuilding affected
segments during `tp_bulkdelete`.

**Architecture:** During `ambulkdelete`, spill the memtable, walk all
segment docmaps to identify which segments contain dead CTIDs, then
rebuild those segments from the heap using the existing
`TpBuildContext` infrastructure. No dead CTID storage needed — the
callback is called twice for affected segments.

**Tech Stack:** C, Postgres AM API (`ambulkdelete`), existing
`TpBuildContext` / segment writer infrastructure.

**Design doc:**
`docs/plans/2026-03-05-vacuum-segment-rebuild-design.md`

---

### Task 1: Create feature branch

**Step 1: Create and switch to branch**

```bash
git checkout -b fix/vacuum-dead-tuple-rebuild
```

**Step 2: Verify clean state**

```bash
git status
```

Expected: clean working tree on `fix/vacuum-dead-tuple-rebuild`.

---

### Task 2: Write failing regression test for CTID reuse bug

This test reproduces the core bug: after DELETE + VACUUM + INSERT
(reusing CTIDs), the index must not return stale results.

**Files:**
- Create: `test/sql/vacuum_rebuild.sql`
- Create: `test/expected/vacuum_rebuild.out`

**Step 1: Write the test**

Create `test/sql/vacuum_rebuild.sql`:

```sql
-- Test: VACUUM correctly removes dead index entries so CTID reuse
-- does not produce false positives.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- ================================================================
-- Test 1: Basic CTID reuse after VACUUM
-- ================================================================

CREATE TABLE vacuum_rebuild_test (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_rebuild_idx
    ON vacuum_rebuild_test USING bm25(content)
    WITH (text_config = 'english');

-- Insert documents with distinctive terms
INSERT INTO vacuum_rebuild_test (content) VALUES
    ('alpha bravo charlie'),
    ('delta echo foxtrot'),
    ('golf hotel india');

-- Force to segments so VACUUM has something to rebuild
SELECT bm25_spill_index('vacuum_rebuild_idx');

-- Verify baseline search works
SELECT id, content FROM vacuum_rebuild_test
WHERE content <@> to_bm25query('alpha', 'vacuum_rebuild_idx') < 0
ORDER BY content <@> to_bm25query('alpha', 'vacuum_rebuild_idx');

-- Delete and VACUUM to free CTIDs
DELETE FROM vacuum_rebuild_test WHERE id <= 2;
VACUUM vacuum_rebuild_test;

-- Insert new rows (may reuse CTIDs from deleted rows)
INSERT INTO vacuum_rebuild_test (content) VALUES
    ('juliet kilo lima'),
    ('mike november oscar');

-- Critical: search for 'alpha' must NOT return new rows
-- (old index entry for 'alpha' pointed to a CTID now reused)
SELECT count(*) AS alpha_matches FROM vacuum_rebuild_test
WHERE content <@> to_bm25query('alpha', 'vacuum_rebuild_idx') < 0;

-- Search for new terms must work
SELECT count(*) AS juliet_matches FROM vacuum_rebuild_test
WHERE content <@> to_bm25query('juliet', 'vacuum_rebuild_idx') < 0;

DROP TABLE vacuum_rebuild_test;

-- ================================================================
-- Test 2: All docs in segment deleted (segment removed from chain)
-- ================================================================

CREATE TABLE vacuum_allgone (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_allgone_idx
    ON vacuum_allgone USING bm25(content)
    WITH (text_config = 'english');

INSERT INTO vacuum_allgone (content) VALUES
    ('papaya mango'),
    ('papaya kiwi');

SELECT bm25_spill_index('vacuum_allgone_idx');

-- Delete everything, vacuum
DELETE FROM vacuum_allgone;
VACUUM vacuum_allgone;

-- Insert fresh data
INSERT INTO vacuum_allgone (content) VALUES
    ('strawberry banana');

-- Must find only the new row, not phantom results
SELECT count(*) AS papaya_gone FROM vacuum_allgone
WHERE content <@> to_bm25query('papaya', 'vacuum_allgone_idx') < 0;

SELECT count(*) AS strawberry_found FROM vacuum_allgone
WHERE content <@> to_bm25query('strawberry', 'vacuum_allgone_idx') < 0;

DROP TABLE vacuum_allgone;

-- ================================================================
-- Test 3: Multi-level segments with deletions
-- ================================================================

SET pg_textsearch.memtable_spill_threshold = 200;

CREATE TABLE vacuum_multilevel (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_multilevel_idx
    ON vacuum_multilevel USING bm25(content)
    WITH (text_config = 'english');

-- Insert enough to create L0 segments and trigger compaction to L1
INSERT INTO vacuum_multilevel (content)
SELECT 'multilevel test document number ' || i ||
       ' with keyword searchterm'
FROM generate_series(1, 2000) AS i;

-- Verify we have segments at multiple levels
SELECT bm25_summarize_index('vacuum_multilevel_idx') ~ 'L1'
    AS has_l1_segments;

-- Delete half
DELETE FROM vacuum_multilevel WHERE id <= 1000;
VACUUM vacuum_multilevel;

-- Verify remaining docs are searchable
SELECT count(*) AS remaining FROM vacuum_multilevel
WHERE content <@> to_bm25query('searchterm',
    'vacuum_multilevel_idx') < 0;

RESET pg_textsearch.memtable_spill_threshold;
DROP TABLE vacuum_multilevel;

-- ================================================================
-- Test 4: VACUUM with no dead tuples (no-op)
-- ================================================================

CREATE TABLE vacuum_noop (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_noop_idx
    ON vacuum_noop USING bm25(content)
    WITH (text_config = 'english');

INSERT INTO vacuum_noop (content) VALUES
    ('nothing deleted here');

SELECT bm25_spill_index('vacuum_noop_idx');
VACUUM vacuum_noop;

SELECT count(*) AS still_here FROM vacuum_noop
WHERE content <@> to_bm25query('deleted', 'vacuum_noop_idx') < 0;

DROP TABLE vacuum_noop;

DROP EXTENSION pg_textsearch CASCADE;
```

**Step 2: Generate expected output**

```bash
make install && make test
# The test will fail (expected). Copy the result to create the
# expected file as a baseline, then we'll update it after
# implementation.
```

We expect test 1's `alpha_matches` to show incorrect results (> 0)
before the fix. After the fix it should be 0.

**Step 3: Commit test file**

```bash
git add test/sql/vacuum_rebuild.sql
git commit -m "test: add regression test for VACUUM CTID reuse bug

Reproduces the correctness issue where plain VACUUM does not remove
dead index entries, leading to false positives when CTIDs are reused
by new inserts."
```

---

### Task 3: Implement Phase 1 — memtable spill in tp_bulkdelete

**Files:**
- Modify: `src/am/vacuum.c`

**Step 1: Add includes and spill helper**

Add to the top of `vacuum.c` (after existing includes):

```c
#include "am/build_context.h"
```

Add a static helper function before `tp_bulkdelete`:

```c
/*
 * Spill memtable to L0 segment if non-empty.
 *
 * During VACUUM, we want all index data in segments for uniform
 * processing. This forces any in-memory data to disk.
 */
static void
tp_vacuum_spill_memtable(
        Relation           index,
        TpLocalIndexState *index_state)
{
    TpMemtable *memtable;
    BlockNumber segment_root;

    if (!index_state || !index_state->shared)
        return;

    memtable = get_memtable(index_state);
    if (!memtable || memtable->total_postings == 0)
        return;

    tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

    segment_root = tp_write_segment(index_state, index);
    if (segment_root != InvalidBlockNumber)
    {
        tp_clear_memtable(index_state);
        tp_clear_docid_pages(index);
        tp_link_l0_chain_head(index, segment_root);
        tp_maybe_compact_level(index, 0);
    }

    tp_release_index_lock(index_state);
}
```

**Step 2: Wire Phase 1 into tp_bulkdelete**

In `tp_bulkdelete`, after the existing stats initialization and
metapage read, add Phase 1 before the callback iteration:

```c
    /* Phase 1: Spill memtable so all data is in segments */
    if (callback != NULL)
    {
        index_state =
            tp_get_local_index_state(RelationGetRelid(info->index));
        if (index_state != NULL)
            tp_vacuum_spill_memtable(info->index, index_state);

        /* Re-read metapage after spill (may have changed) */
        pfree(metap);
        metap = tp_get_metapage(info->index);
        if (!metap)
        {
            stats->num_pages        = 1;
            stats->num_index_tuples = 0;
            stats->tuples_removed   = 0;
            return stats;
        }
    }
```

**Step 3: Build and verify compilation**

```bash
make
```

Expected: compiles without errors.

**Step 4: Commit**

```bash
git add src/am/vacuum.c
git commit -m "fix(vacuum): Phase 1 — spill memtable before bulk delete

Forces all memtable data to disk segments at the start of
tp_bulkdelete, so dead tuple processing only needs to handle
segments."
```

---

### Task 4: Implement Phase 2 — identify affected segments

**Files:**
- Modify: `src/am/vacuum.c`

**Step 1: Define per-segment tracking struct**

Add above `tp_bulkdelete`:

```c
/*
 * Per-segment state for VACUUM dead tuple tracking.
 * Tracks which segments contain dead CTIDs without storing the
 * CTIDs themselves.
 */
typedef struct TpVacuumSegmentInfo
{
    BlockNumber root_block;
    BlockNumber next_segment;
    uint32      level;
    uint32      num_docs;
    int64       dead_count;
    bool        affected;
} TpVacuumSegmentInfo;
```

**Step 2: Add Phase 2 helper — scan segments for dead CTIDs**

```c
/*
 * Walk all segment docmaps and call the callback for each CTID.
 * Returns an array of TpVacuumSegmentInfo with affected flags set.
 * *num_segments_out receives the total segment count.
 */
static TpVacuumSegmentInfo *
tp_vacuum_identify_affected(
        Relation                index,
        TpIndexMetaPage         metap,
        IndexBulkDeleteCallback callback,
        void                   *callback_state,
        int                    *num_segments_out,
        int64                  *total_dead_out)
{
    TpVacuumSegmentInfo *segments;
    int                  capacity = 32;
    int                  count    = 0;
    int64                total_dead = 0;

    segments = palloc(capacity * sizeof(TpVacuumSegmentInfo));

    for (int level = 0; level < TP_MAX_LEVELS; level++)
    {
        BlockNumber seg = metap->level_heads[level];

        while (seg != InvalidBlockNumber)
        {
            TpSegmentReader *reader;
            int64            seg_dead = 0;

            reader = tp_segment_open_ex(index, seg, true);
            if (!reader || !reader->header)
            {
                if (reader)
                    tp_segment_close(reader);
                break;
            }

            /* Check each CTID against the callback */
            for (uint32 i = 0; i < reader->header->num_docs; i++)
            {
                ItemPointerData ctid;

                tp_segment_lookup_ctid(reader, i, &ctid);
                if (ItemPointerIsValid(&ctid) &&
                    callback(&ctid, callback_state))
                {
                    seg_dead++;
                }
            }

            /* Record segment info */
            if (count >= capacity)
            {
                capacity *= 2;
                segments = repalloc(segments,
                    capacity * sizeof(TpVacuumSegmentInfo));
            }

            segments[count].root_block    = seg;
            segments[count].next_segment  =
                reader->header->next_segment;
            segments[count].level         = level;
            segments[count].num_docs      = reader->header->num_docs;
            segments[count].dead_count    = seg_dead;
            segments[count].affected      = (seg_dead > 0);

            total_dead += seg_dead;
            count++;

            seg = reader->header->next_segment;
            tp_segment_close(reader);
        }
    }

    *num_segments_out = count;
    *total_dead_out   = total_dead;
    return segments;
}
```

**Step 3: Build**

```bash
make
```

Expected: compiles.

**Step 4: Commit**

```bash
git add src/am/vacuum.c
git commit -m "fix(vacuum): Phase 2 — identify segments with dead CTIDs

Walks all segment docmaps and calls the VACUUM callback for each CTID.
Tracks which segments are affected using O(segments) memory, without
storing individual dead CTIDs."
```

---

### Task 5: Implement Phase 3 — rebuild affected segments

This is the core of the fix. For each affected segment, re-read the
docmap, call the callback to filter dead CTIDs, fetch live heap tuples,
tokenize, and write a new segment.

**Files:**
- Modify: `src/am/vacuum.c`

**Step 1: Add the rebuild helper**

```c
/*
 * Rebuild a single segment, excluding dead CTIDs.
 *
 * Reads the segment's docmap, calls the callback for each CTID,
 * fetches live heap tuples, tokenizes them, and writes a new
 * segment via TpBuildContext.
 *
 * Returns the new segment's root block, or InvalidBlockNumber if
 * all docs were dead (segment should be removed from chain).
 */
static BlockNumber
tp_vacuum_rebuild_segment(
        Relation                index,
        Relation                heap,
        BlockNumber             old_root,
        uint32                  level,
        IndexBulkDeleteCallback callback,
        void                   *callback_state,
        uint64                 *new_total_docs,
        uint64                 *new_total_len)
{
    TpSegmentReader *reader;
    TpBuildContext   *build_ctx;
    BlockNumber      new_root;
    Oid              text_config_oid;
    AttrNumber       attnum;
    MemoryContext     per_doc_ctx;
    MemoryContext     old_ctx;
    uint64           docs_added = 0;
    uint64           len_added  = 0;

    /* Get text config from metapage */
    {
        TpIndexMetaPage mp = tp_get_metapage(index);

        text_config_oid = mp->text_config_oid;
        pfree(mp);
    }

    /* Get the indexed column number */
    attnum = index->rd_index->indkey.values[0];

    /* Open segment with CTID preloading */
    reader = tp_segment_open_ex(index, old_root, true);
    if (!reader || !reader->header)
    {
        if (reader)
            tp_segment_close(reader);
        *new_total_docs = 0;
        *new_total_len  = 0;
        return InvalidBlockNumber;
    }

    /* Create build context (no budget limit for VACUUM rebuild) */
    build_ctx = tp_build_context_create(0);

    per_doc_ctx = AllocSetContextCreate(
            CurrentMemoryContext,
            "VACUUM rebuild per-doc",
            ALLOCSET_DEFAULT_SIZES);

    /* Iterate docmap, skip dead, fetch+tokenize live */
    for (uint32 i = 0; i < reader->header->num_docs; i++)
    {
        ItemPointerData ctid;
        HeapTupleData   tuple;
        Buffer          heap_buf = InvalidBuffer;
        bool            valid;
        Datum           text_datum;
        bool            isnull;
        text           *document_text;
        Datum           tsvector_datum;
        TSVector        tsvector;
        char          **terms;
        int32          *frequencies;
        int             term_count;
        int             doc_length;

        tp_segment_lookup_ctid(reader, i, &ctid);
        if (!ItemPointerIsValid(&ctid))
            continue;

        /* Skip dead CTIDs */
        if (callback(&ctid, callback_state))
            continue;

        /* Fetch heap tuple */
        tuple.t_self = ctid;
        valid = heap_fetch(heap, SnapshotAny, &tuple,
                           &heap_buf, true);
        if (!valid)
        {
            if (heap_buf != InvalidBuffer)
                ReleaseBuffer(heap_buf);
            continue;
        }

        /* Extract text from indexed column */
        text_datum = heap_getattr(
                &tuple, attnum, RelationGetDescr(heap), &isnull);

        if (isnull)
        {
            ReleaseBuffer(heap_buf);
            continue;
        }

        document_text = DatumGetTextPP(text_datum);

        /* Tokenize in per-doc context */
        old_ctx = MemoryContextSwitchTo(per_doc_ctx);

        tsvector_datum = DirectFunctionCall2Coll(
                to_tsvector_byid,
                InvalidOid,
                ObjectIdGetDatum(text_config_oid),
                PointerGetDatum(document_text));
        tsvector = DatumGetTSVector(tsvector_datum);

        doc_length = tp_extract_terms_from_tsvector(
                tsvector, &terms, &frequencies, &term_count);

        MemoryContextSwitchTo(old_ctx);

        if (term_count > 0)
        {
            tp_build_context_add_document(
                    build_ctx,
                    terms,
                    frequencies,
                    term_count,
                    doc_length,
                    &ctid);
            docs_added++;
            len_added += doc_length;
        }

        MemoryContextReset(per_doc_ctx);
        ReleaseBuffer(heap_buf);
    }

    tp_segment_close(reader);

    /* Write new segment if any docs survived */
    if (build_ctx->num_docs > 0)
        new_root = tp_write_segment_from_build_ctx(build_ctx, index);
    else
        new_root = InvalidBlockNumber;

    tp_build_context_destroy(build_ctx);
    MemoryContextDelete(per_doc_ctx);

    *new_total_docs = docs_added;
    *new_total_len  = len_added;
    return new_root;
}
```

**Step 2: Add the chain-replacement helper**

```c
/*
 * Replace or remove a segment in a level's chain.
 *
 * If new_root is valid, replaces old_root with new_root in the
 * chain. If new_root is InvalidBlockNumber, removes old_root from
 * the chain entirely.
 *
 * Updates the metapage level_heads/level_counts as needed.
 * Frees old segment pages.
 */
static void
tp_vacuum_replace_segment(
        Relation    index,
        uint32      level,
        BlockNumber old_root,
        BlockNumber new_root,
        BlockNumber prev_root)
{
    Buffer           metabuf;
    Page             metapage;
    TpIndexMetaPage  metap;
    BlockNumber     *old_pages;
    uint32           old_page_count;

    /* Collect old segment pages for freeing */
    old_page_count =
        tp_segment_collect_pages(index, old_root, &old_pages);

    /* Set new segment's next_segment pointer */
    if (new_root != InvalidBlockNumber)
    {
        TpSegmentReader *old_reader;
        BlockNumber      old_next;
        Buffer           new_buf;
        Page             new_page;
        TpSegmentHeader *new_header;

        old_reader = tp_segment_open(index, old_root);
        old_next = old_reader->header->next_segment;
        tp_segment_close(old_reader);

        new_buf = ReadBuffer(index, new_root);
        LockBuffer(new_buf, BUFFER_LOCK_EXCLUSIVE);
        new_page   = BufferGetPage(new_buf);
        new_header = (TpSegmentHeader *)PageGetContents(new_page);
        new_header->next_segment = old_next;
        new_header->level        = level;
        MarkBufferDirty(new_buf);
        UnlockReleaseBuffer(new_buf);
    }

    /* Update chain pointers */
    metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    metapage = BufferGetPage(metabuf);
    metap    = (TpIndexMetaPage)PageGetContents(metapage);

    if (prev_root == InvalidBlockNumber)
    {
        /* old_root was the head */
        if (new_root != InvalidBlockNumber)
            metap->level_heads[level] = new_root;
        else
        {
            /* Read old next before freeing */
            TpSegmentReader *r = tp_segment_open(index, old_root);
            BlockNumber      nx = r->header->next_segment;

            tp_segment_close(r);
            metap->level_heads[level] = nx;
            metap->level_counts[level]--;
        }
    }
    else
    {
        /* Update prev's next_segment */
        Buffer           prev_buf;
        Page             prev_page;
        TpSegmentHeader *prev_header;

        prev_buf = ReadBuffer(index, prev_root);
        LockBuffer(prev_buf, BUFFER_LOCK_EXCLUSIVE);
        prev_page   = BufferGetPage(prev_buf);
        prev_header =
            (TpSegmentHeader *)PageGetContents(prev_page);

        if (new_root != InvalidBlockNumber)
            prev_header->next_segment = new_root;
        else
        {
            TpSegmentReader *r = tp_segment_open(index, old_root);
            BlockNumber      nx = r->header->next_segment;

            tp_segment_close(r);
            prev_header->next_segment = nx;
            metap->level_counts[level]--;
        }

        MarkBufferDirty(prev_buf);
        UnlockReleaseBuffer(prev_buf);
    }

    MarkBufferDirty(metabuf);
    UnlockReleaseBuffer(metabuf);

    /* Free old segment pages */
    if (old_pages && old_page_count > 0)
        tp_segment_free_pages(index, old_pages, old_page_count);
    if (old_pages)
        pfree(old_pages);

    IndexFreeSpaceMapVacuum(index);
}
```

**Step 3: Build**

```bash
make
```

**Step 4: Commit**

```bash
git add src/am/vacuum.c
git commit -m "fix(vacuum): Phase 3 — rebuild affected segments from heap

For each segment containing dead CTIDs, re-reads the docmap, calls the
callback to filter, fetches live heap tuples, tokenizes them, and
writes a new segment via TpBuildContext. Replaces old segments in the
chain and frees their pages."
```

---

### Task 6: Wire everything together in tp_bulkdelete

**Files:**
- Modify: `src/am/vacuum.c`

**Step 1: Rewrite tp_bulkdelete**

Replace the body of `tp_bulkdelete` (keep the signature):

```c
IndexBulkDeleteResult *
tp_bulkdelete(
        IndexVacuumInfo        *info,
        IndexBulkDeleteResult  *stats,
        IndexBulkDeleteCallback callback,
        void                   *callback_state)
{
    TpIndexMetaPage        metap;
    TpLocalIndexState     *index_state;
    TpVacuumSegmentInfo   *segments;
    int                    num_segments;
    int64                  total_dead;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *)palloc0(
                sizeof(IndexBulkDeleteResult));

    metap = tp_get_metapage(info->index);
    if (!metap)
    {
        stats->num_pages        = 1;
        stats->num_index_tuples = 0;
        stats->tuples_removed   = 0;
        stats->pages_deleted    = 0;
        elog(WARNING,
             "Tapir bulkdelete: couldn't read metapage for "
             "index %s",
             RelationGetRelationName(info->index));
        return stats;
    }

    if (callback == NULL)
    {
        /* No callback — just report stats (shouldn't happen) */
        stats->num_pages        = 1;
        stats->num_index_tuples = (double)metap->total_docs;
        stats->tuples_removed   = 0;
        stats->pages_deleted    = 0;
        pfree(metap);
        return stats;
    }

    /* Phase 1: Spill memtable so all data is in segments */
    index_state =
        tp_get_local_index_state(RelationGetRelid(info->index));
    if (index_state != NULL)
        tp_vacuum_spill_memtable(info->index, index_state);

    /* Re-read metapage after spill */
    pfree(metap);
    metap = tp_get_metapage(info->index);
    if (!metap)
    {
        stats->num_pages        = 1;
        stats->num_index_tuples = 0;
        stats->tuples_removed   = 0;
        return stats;
    }

    /* Phase 2: Identify affected segments */
    segments = tp_vacuum_identify_affected(
            info->index, metap, callback, callback_state,
            &num_segments, &total_dead);

    if (total_dead == 0)
    {
        /* No dead tuples — nothing to rebuild */
        stats->num_pages        = 1;
        stats->num_index_tuples = (double)metap->total_docs;
        stats->tuples_removed   = 0;
        stats->pages_deleted    = 0;
        pfree(metap);
        pfree(segments);
        return stats;
    }

    elog(DEBUG1,
         "Tapir VACUUM: %lld dead tuples across %d segments, "
         "rebuilding affected segments",
         (long long)total_dead, num_segments);

    /* Phase 3: Rebuild affected segments */
    {
        uint64 new_total_docs = 0;
        uint64 new_total_len  = 0;

        /*
         * Walk segments per-level to track prev pointers for
         * chain replacement.
         */
        for (int level = 0; level < TP_MAX_LEVELS; level++)
        {
            BlockNumber prev = InvalidBlockNumber;

            for (int i = 0; i < num_segments; i++)
            {
                if ((int)segments[i].level != level)
                    continue;

                if (segments[i].affected)
                {
                    BlockNumber new_root;
                    uint64      seg_docs, seg_len;

                    new_root = tp_vacuum_rebuild_segment(
                            info->index,
                            info->heaprel,
                            segments[i].root_block,
                            level,
                            callback,
                            callback_state,
                            &seg_docs,
                            &seg_len);

                    tp_vacuum_replace_segment(
                            info->index,
                            level,
                            segments[i].root_block,
                            new_root,
                            prev);

                    new_total_docs += seg_docs;
                    new_total_len  += seg_len;

                    /*
                     * If we replaced (not removed), the new
                     * segment becomes prev for the next
                     * iteration.
                     */
                    if (new_root != InvalidBlockNumber)
                        prev = new_root;
                    /* else prev stays the same */
                }
                else
                {
                    new_total_docs += segments[i].num_docs;
                    /* Approximate: we don't have total_len
                     * per segment readily, so we'll recount
                     * from the metapage after. */
                    prev = segments[i].root_block;
                }
            }
        }

        /* Phase 4: Update metapage statistics */
        {
            Buffer          mbuf;
            Page            mpage;
            TpIndexMetaPage mp;

            mbuf  = ReadBuffer(info->index, TP_METAPAGE_BLKNO);
            LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
            mpage = BufferGetPage(mbuf);
            mp    = (TpIndexMetaPage)PageGetContents(mpage);

            /*
             * Recount total_docs from level_counts is not
             * accurate (that's segment counts, not doc counts).
             * Subtract dead from prior total as approximation.
             */
            if (mp->total_docs >= (uint64)total_dead)
                mp->total_docs -= total_dead;
            else
                mp->total_docs = new_total_docs;

            MarkBufferDirty(mbuf);
            UnlockReleaseBuffer(mbuf);
        }
    }

    /* Fill in return stats */
    {
        TpIndexMetaPage mp = tp_get_metapage(info->index);

        if (mp)
        {
            stats->num_pages        = 1;
            stats->num_index_tuples = (double)mp->total_docs;
            stats->tuples_removed   = (double)total_dead;
            stats->pages_deleted    = 0;
            pfree(mp);
        }
    }

    pfree(metap);
    pfree(segments);

    return stats;
}
```

**Step 2: Remove the old `tp_bulkdelete_memtable_ctids` and
`tp_bulkdelete_segment_ctids` functions**

These are now superseded by `tp_vacuum_identify_affected` and the
rebuild logic. Delete both functions (lines ~33-122 of the original
file).

**Step 3: Build**

```bash
make
```

**Step 4: Commit**

```bash
git add src/am/vacuum.c
git commit -m "fix(vacuum): wire up all phases in tp_bulkdelete

Replaces the old no-op bulkdelete with the four-phase approach:
spill memtable, identify affected segments, rebuild from heap,
update statistics."
```

---

### Task 7: Add merge path comment

**Files:**
- Modify: `src/segment/merge.c`

**Step 1: Add comment at line 1587**

At `merge.c:1587` where `write_merged_segment_to_sink` is called with
`false` (disjoint_sources), add a comment:

```c
        /*
         * disjoint_sources = false: use N-way CTID-comparison merge.
         * This is load-bearing for VACUUM correctness. After VACUUM
         * rebuilds affected segments, CTID ranges across segments at
         * the same level may interleave. The N-way merge handles
         * this correctly. Only parallel build (build_parallel.c) may
         * pass true, where workers have genuinely disjoint heap
         * block ranges.
         */
        write_merged_segment_to_sink(
                &sink,
                merged_terms,
                num_merged_terms,
                sources,
                num_sources,
                level + 1,
                total_tokens,
                false);
```

**Step 2: Build and commit**

```bash
make && git add src/segment/merge.c
git commit -m "docs: add comment explaining disjoint_sources=false for VACUUM safety"
```

---

### Task 8: Run tests and fix

**Step 1: Install and run all tests**

```bash
make install && make installcheck 2>&1 | tail -40
```

**Step 2: Check the new test**

```bash
cat test/results/vacuum_rebuild.out
```

If `alpha_matches` is now 0 (correct), generate expected output:

```bash
cp test/results/vacuum_rebuild.out test/expected/vacuum_rebuild.out
```

**Step 3: Check existing tests**

```bash
cat test/regression.diffs
```

Fix any regressions in existing vacuum/vacuum_extended tests. If
error messages changed, update `test/expected/*.out` accordingly.

**Step 4: Run format check**

```bash
make format-check
```

If formatting issues, fix with `make format`.

**Step 5: Full test pass**

```bash
make installcheck
```

Expected: all tests pass.

**Step 6: Commit expected output**

```bash
git add test/expected/vacuum_rebuild.out
git add -u test/expected/  # any updated expected files
git commit -m "test: add expected output for vacuum_rebuild test"
```

---

### Task 9: Update documentation

**Files:**
- Modify: `ROADMAP.md`

**Step 1: Add note to ROADMAP.md**

Under the v1.0.0 section, add a bullet:

```markdown
- **VACUUM correctness**: Plain VACUUM now correctly removes dead
  index entries by rebuilding affected segments. This prevents false
  positives when CTIDs are reused. See #264 for the planned bitmap
  optimization.
```

**Step 2: Commit**

```bash
git add ROADMAP.md
git commit -m "docs: document VACUUM correctness fix in roadmap"
```

---

### Task 10: Final verification and PR

**Step 1: Full test suite**

```bash
make clean && make && make install && make installcheck
```

**Step 2: Format check**

```bash
make format-check
```

**Step 3: Review all changes**

```bash
git log --oneline fix/vacuum-dead-tuple-rebuild ^main
git diff main..fix/vacuum-dead-tuple-rebuild --stat
```

**Step 4: Push and create PR**

```bash
git push -u origin fix/vacuum-dead-tuple-rebuild
gh pr create --draft \
    --title "fix: VACUUM correctly removes dead index entries" \
    --body "$(cat <<'EOF'
## Summary

- Fixes correctness bug where plain VACUUM did not remove dead index
  entries, leading to false positives when Postgres reuses CTIDs
- Implements four-phase approach in `tp_bulkdelete`:
  1. Spill memtable to segments
  2. Identify segments containing dead CTIDs (O(segments) memory)
  3. Rebuild affected segments from heap via TpBuildContext
  4. Update metapage statistics
- Adds comment documenting `disjoint_sources=false` invariant in
  merge path

## Limitations

- Rebuilds entire affected segments (expensive for large segments
  with few deletes). #264 tracks the efficient bitmap approach.
- BM25 statistics in non-rebuilt segments may be slightly stale.

## Testing

- New `vacuum_rebuild` regression test covering:
  - CTID reuse after DELETE + VACUUM + INSERT
  - All-docs-deleted segment removal
  - Multi-level segments with deletions
  - No-op VACUUM path
- Existing vacuum and vacuum_extended tests pass

Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
