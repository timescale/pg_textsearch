# Expression and Partial Index Support — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add expression index and partial index support to
pg_textsearch, enabling `CREATE INDEX ... USING bm25 ((expr)) WHERE
predicate`.

**Architecture:** Switch the serial build path to
`IndexBuildHeapScan()` (standard Postgres callback API), add
`FormIndexDatum()`/`ExecQual()` to parallel build workers, vacuum, and
crash recovery. Generalize planner hooks from column-only matching to
expression-tree comparison with `equal()`. Partial index predicates
leverage Postgres's built-in `predicate_implied_by()`.

**Tech Stack:** C, Postgres 17/18 AM APIs, `FormIndexDatum`,
`ExecQual`, `predicate_implied_by`, `pull_var_clause`, `ChangeVarNodes`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `src/am/build.c` | Modify | Remove expression block, refactor serial build to IndexBuildHeapScan callback |
| `src/am/build_parallel.c` | Modify | Replace slot_getattr with FormIndexDatum, add predicate check, remove attnum |
| `src/am/build_parallel.h` | Modify | Remove attnum from TpParallelBuildShared |
| `src/am/vacuum.c` | Modify | Replace heap_getattr with FormIndexDatum, add predicate check |
| `src/state/state.c` | Modify | Replace heap_getattr with FormIndexDatum in recovery path |
| `src/am/scan.c` | Modify | Add expression-tree comparison for index matching |
| `src/planner/hooks.c` | Modify | Generalize from Var-only to arbitrary expressions, add partial index predicate matching |
| `test/sql/expression_index.sql` | Create | Expression index tests |
| `test/expected/expression_index.out` | Create | Expected output |
| `test/sql/partial_index.sql` | Create | Partial index tests |
| `test/expected/partial_index.out` | Create | Expected output |
| `test/sql/unsupported.sql` | Modify | Remove expression index limitation |
| `test/expected/unsupported.out` | Modify | Update expected output |
| `Makefile` | Modify | Add new test names to REGRESS |

---

## Task 1: Remove Expression Index Block and Add Basic Test

**Files:**
- Modify: `src/am/build.c:805-816`
- Create: `test/sql/expression_index.sql`
- Modify: `Makefile:73`

This task removes the guard that blocks expression indexes and adds a
basic test that will initially fail (expression evaluation not yet
wired up, so the indexed text will be wrong or crash). This establishes
the test file and confirms the block is gone.

- [ ] **Step 1: Write the basic expression index test**

Create `test/sql/expression_index.sql`:

```sql
-- Expression index tests for pg_textsearch

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- ============================================================
-- JSONB expression index
-- ============================================================

CREATE TABLE expr_jsonb (
    id SERIAL PRIMARY KEY,
    data JSONB
);

INSERT INTO expr_jsonb (data) VALUES
    ('{"content": "postgresql database management system"}'),
    ('{"content": "machine learning algorithms"}'),
    ('{"content": "full text search and retrieval"}');

CREATE INDEX expr_jsonb_idx ON expr_jsonb
    USING bm25 ((data->>'content'))
    WITH (text_config='english');

-- Explicit index naming query
SET enable_seqscan = off;

SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database', 'expr_jsonb_idx'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_jsonb_idx')
LIMIT 5;

-- ============================================================
-- Multi-column concatenation expression index
-- ============================================================

CREATE TABLE expr_concat (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT
);

INSERT INTO expr_concat (title, body) VALUES
    ('Database Systems', 'An introduction to relational databases'),
    ('Machine Learning', 'Algorithms for pattern recognition'),
    ('Search Engines', 'Full text search and information retrieval');

CREATE INDEX expr_concat_idx ON expr_concat
    USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');

SELECT id,
       ROUND(((coalesce(title, '') || ' ' || coalesce(body, '')) <@>
              to_bm25query('database', 'expr_concat_idx'))::numeric, 4)
              AS score
FROM expr_concat
ORDER BY (coalesce(title, '') || ' ' || coalesce(body, '')) <@>
         to_bm25query('database', 'expr_concat_idx')
LIMIT 5;

-- ============================================================
-- Simple transformation expression index
-- ============================================================

CREATE TABLE expr_lower (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO expr_lower (content) VALUES
    ('PostgreSQL Database Management'),
    ('Machine Learning Algorithms'),
    ('Full Text Search and Retrieval');

CREATE INDEX expr_lower_idx ON expr_lower
    USING bm25 ((lower(content)))
    WITH (text_config='simple');

SELECT id,
       ROUND(((lower(content)) <@>
              to_bm25query('database', 'expr_lower_idx'))::numeric, 4)
              AS score
FROM expr_lower
ORDER BY (lower(content)) <@>
         to_bm25query('database', 'expr_lower_idx')
LIMIT 5;

-- ============================================================
-- Error case: non-text expression
-- ============================================================
\set ON_ERROR_STOP off

CREATE INDEX expr_bad_idx ON expr_lower
    USING bm25 ((length(content)))
    WITH (text_config='simple');

\set ON_ERROR_STOP on

-- ============================================================
-- Cleanup
-- ============================================================

DROP TABLE expr_jsonb CASCADE;
DROP TABLE expr_concat CASCADE;
DROP TABLE expr_lower CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
```

- [ ] **Step 2: Add test to Makefile REGRESS list**

In `Makefile:73`, add `expression_index` to the REGRESS list (after
`explicit_index`).

- [ ] **Step 3: Remove the expression index block in build.c**

In `src/am/build.c`, delete lines 805-816 (the
`ii_IndexAttrNumbers[0] == 0` check and error).

- [ ] **Step 4: Build and verify the block is removed**

```bash
make clean && make && make install
```

Expected: compiles without error.

- [ ] **Step 5: Run the test to see current failure mode**

```bash
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test expression_index
```

Expected: test runs but likely crashes or returns wrong results (the
serial build still uses `slot_getattr` with attnum=0 for expressions).
This confirms the block is removed and we need the next task.

- [ ] **Step 6: Commit**

```bash
git add src/am/build.c test/sql/expression_index.sql Makefile
git commit -m "feat: remove expression index block, add expression index test scaffold"
```

---

## Task 2: Refactor Serial Build to IndexBuildHeapScan

**Files:**
- Modify: `src/am/build.c:998-1222`

This is the core refactor. Replace the custom
`table_scan_getnextslot()` loop with `IndexBuildHeapScan()` and a
callback. The callback does tokenization, adds to build context, and
flushes when budget is exceeded.

- [ ] **Step 1: Add callback state struct and callback function**

Add above `tp_build()` in `src/am/build.c`:

```c
/*
 * Callback state for IndexBuildHeapScan.
 */
typedef struct TpBuildCallbackState
{
	TpBuildContext *build_ctx;
	Relation       index;
	Oid            text_config_oid;
	MemoryContext  per_doc_ctx;
	uint64         total_docs;
	uint64         total_len;
	uint64         tuples_done;
} TpBuildCallbackState;

/*
 * Per-document callback for IndexBuildHeapScan.
 *
 * values[0] contains the already-evaluated index expression result
 * (or plain column value). Predicate-failing rows are already
 * filtered by IndexBuildHeapScan and never reach this callback.
 */
static void
tp_build_callback(
        Relation    index,
        ItemPointer ctid,
        Datum      *values,
        bool       *isnull,
        bool        tupleIsAlive,
        void       *state)
{
	TpBuildCallbackState *bs = (TpBuildCallbackState *)state;
	text                 *document_text;
	Datum                 tsvector_datum;
	TSVector              tsvector;
	char                **terms;
	int32                *frequencies;
	int                   term_count;
	int                   doc_length;
	MemoryContext         oldctx;

	(void)tupleIsAlive; /* unused for non-concurrent builds */

	if (isnull[0])
		return;

	if (!ItemPointerIsValid(ctid))
		return;

	document_text = DatumGetTextPP(values[0]);

	/*
	 * Tokenize in temporary context to prevent
	 * to_tsvector_byid memory from accumulating.
	 */
	oldctx = MemoryContextSwitchTo(bs->per_doc_ctx);

	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid,
			ObjectIdGetDatum(bs->text_config_oid),
			PointerGetDatum(document_text));
	tsvector = DatumGetTSVector(tsvector_datum);

	doc_length = tp_extract_terms_from_tsvector(
			tsvector, &terms, &frequencies, &term_count);

	MemoryContextSwitchTo(oldctx);

	if (term_count > 0)
	{
		tp_build_context_add_document(
				bs->build_ctx,
				terms,
				frequencies,
				term_count,
				doc_length,
				ctid);
	}

	/* Reset per-doc context (frees tsvector, terms) */
	MemoryContextReset(bs->per_doc_ctx);

	/* Budget-based flush */
	if (tp_build_context_should_flush(bs->build_ctx))
	{
		bs->total_docs += bs->build_ctx->num_docs;
		bs->total_len += bs->build_ctx->total_len;

		tp_build_flush_and_link(bs->build_ctx, bs->index);
		tp_build_context_reset(bs->build_ctx);

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE,
				TP_PHASE_COMPACTING);
		tp_maybe_compact_level(bs->index, 0);
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);
	}

	bs->tuples_done++;
	if (bs->tuples_done % TP_PROGRESS_REPORT_INTERVAL == 0)
	{
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE,
				bs->tuples_done);
		CHECK_FOR_INTERRUPTS();
	}
}
```

- [ ] **Step 2: Replace the serial build loop with IndexBuildHeapScan**

Replace lines 998-1222 in `src/am/build.c` (the entire serial build
block starting with `/* Serial build using arena-based build context */`
through the closing `return result;`) with:

```c
	/*
	 * Serial build using IndexBuildHeapScan callback.
	 *
	 * IndexBuildHeapScan handles expression evaluation (via
	 * FormIndexDatum), predicate filtering (via ExecQual for
	 * partial indexes), snapshot management, and visibility.
	 * Our callback does tokenization and arena-based batching.
	 */
	{
		TpBuildCallbackState bs;
		Size                 budget;

		/*
		 * Still create build index state for:
		 * - Per-index LWLock infrastructure
		 * - Post-build transition to runtime mode
		 * - Shared state initialization for runtime queries
		 */
		index_state = tp_create_build_index_state(
				RelationGetRelid(index), RelationGetRelid(heap));

		/* Budget: maintenance_work_mem (in KB) -> bytes */
		budget        = (Size)maintenance_work_mem * 1024L;
		bs.build_ctx  = tp_build_context_create(budget);
		bs.index      = index;
		bs.text_config_oid = text_config_oid;
		bs.total_docs = 0;
		bs.total_len  = 0;
		bs.tuples_done = 0;

		/*
		 * Per-document memory context for tokenization temporaries.
		 * Reset after each document to prevent unbounded growth.
		 */
		bs.per_doc_ctx = AllocSetContextCreate(
				CurrentMemoryContext,
				"build per-doc temp",
				ALLOCSET_DEFAULT_SIZES);

		/* Report loading phase */
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

		{
			double reltuples  = heap->rd_rel->reltuples;
			int64  tuples_est = (reltuples > 0) ? (int64)reltuples : 0;

			pgstat_progress_update_param(
					PROGRESS_CREATEIDX_TUPLES_TOTAL, tuples_est);
		}

		/* Scan heap and build index via callback */
		reltuples = IndexBuildHeapScan(
				heap, index, indexInfo, true, false,
				tp_build_callback, &bs, NULL);

		/* Accumulate final batch stats */
		bs.total_docs += bs.build_ctx->num_docs;
		bs.total_len += bs.build_ctx->total_len;
		total_docs = bs.total_docs;
		total_len = bs.total_len;

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE, bs.tuples_done);

		/* Report writing phase */
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

		/* Write final segment if data remains */
		if (bs.build_ctx->num_docs > 0)
			tp_build_flush_and_link(bs.build_ctx, index);

		/* Update metapage with corpus statistics */
		{
			Buffer          metabuf;
			Page            metapage;
			TpIndexMetaPage metap;

			metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
			metapage = BufferGetPage(metabuf);
			metap    = (TpIndexMetaPage)PageGetContents(metapage);

			metap->total_docs = total_docs;
			metap->total_len  = total_len;

			MarkBufferDirty(metabuf);
			if (!BufferIsLocal(metabuf))
				FlushOneBuffer(metabuf);
			UnlockReleaseBuffer(metabuf);
		}

		/* Update shared state for runtime queries */
		index_state->shared->total_docs = total_docs;
		index_state->shared->total_len  = total_len;

		/* Create index build result */
		result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
		result->heap_tuples  = reltuples;
		result->index_tuples = total_docs;

		if (build_progress.active)
		{
			build_progress.total_docs += total_docs;
			build_progress.total_len += total_len;
			build_progress.partition_count++;
		}
		else
		{
			elog(NOTICE,
				 "BM25 index build completed: " UINT64_FORMAT
				 " documents, avg_length=%.2f",
				 total_docs,
				 total_docs > 0
					 ? (float4)(total_len / (double)total_docs)
					 : 0.0);
		}

		tp_release_index_lock(index_state);
		tp_finalize_build_mode(index_state);

		/* Cleanup */
		tp_build_context_destroy(bs.build_ctx);
		MemoryContextDelete(bs.per_doc_ctx);
	}

	return result;
}
```

Note: add a `double reltuples;` declaration alongside the existing
local variables at the top of `tp_build()`. Remove the now-unused
`TableScanDesc scan`, `TupleTableSlot *slot`, and `Snapshot snapshot`
declarations.

- [ ] **Step 3: Add required include**

Add to the includes at the top of `src/am/build.c`:

```c
#include <catalog/index.h>
```

This provides the `IndexBuildHeapScan` declaration.

- [ ] **Step 4: Remove tp_setup_table_scan if no longer used**

Check if `tp_setup_table_scan()` is used anywhere else. If not, remove
it (lines 670-691). If it's used elsewhere, keep it.

- [ ] **Step 5: Build and run existing tests**

```bash
make clean && make && make install && make installcheck
```

Expected: existing tests pass (plain-column indexes still work through
the callback). Check `test/regression.diffs` for failures.

- [ ] **Step 6: Run the expression index test**

```bash
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test expression_index
```

Expected: expression index creation and queries should now work because
`IndexBuildHeapScan` evaluates the expression via `FormIndexDatum()`
and passes the result in `values[0]`.

- [ ] **Step 7: Generate expected output**

```bash
cp test/results/expression_index.out test/expected/expression_index.out
```

Review the output to make sure scores are reasonable and correct.

- [ ] **Step 8: Run test again to confirm pass**

```bash
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test expression_index
```

Expected: PASS.

- [ ] **Step 9: Run full test suite**

```bash
make installcheck
```

Expected: all tests pass. Check `test/regression.diffs`.

- [ ] **Step 10: Format and commit**

```bash
make format
git add src/am/build.c test/sql/expression_index.sql \
        test/expected/expression_index.out
git commit -m "feat: refactor serial build to IndexBuildHeapScan, enable expression indexes"
```

---

## Task 3: Update Parallel Build for Expressions and Predicates

**Files:**
- Modify: `src/am/build_parallel.h:63-99`
- Modify: `src/am/build_parallel.c:121-163,169-370,440-506`

Replace `slot_getattr(slot, shared->attnum, ...)` with
`FormIndexDatum()` in workers. Remove `attnum` from shared state.
Workers call `BuildIndexInfo()` to get expression/predicate info.

- [ ] **Step 1: Remove attnum from TpParallelBuildShared**

In `src/am/build_parallel.h`, remove the `AttrNumber attnum;` field
(line 69) from the struct.

- [ ] **Step 2: Remove attnum from tp_init_parallel_shared**

In `src/am/build_parallel.c`, remove the `AttrNumber attnum` parameter
from `tp_init_parallel_shared()` (line 126) and remove the
`shared->attnum = attnum;` assignment (line 140).

- [ ] **Step 3: Update the caller in tp_build_parallel**

In `src/am/build_parallel.c:498-506`, remove the
`indexInfo->ii_IndexAttrNumbers[0]` argument from the
`tp_init_parallel_shared()` call.

- [ ] **Step 4: Update worker to use FormIndexDatum**

In `tp_parallel_build_worker_main()` (`src/am/build_parallel.c`),
after opening the index relation (line 197), add IndexInfo setup:

```c
	/* Build IndexInfo for expression/predicate evaluation */
	IndexInfo  *indexInfo;
	EState     *estate;
	ExprContext *econtext;
	Datum       values[INDEX_MAX_KEYS];
	bool        index_isnull[INDEX_MAX_KEYS];

	indexInfo = BuildIndexInfo(index);

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
```

Then replace the text extraction in the worker loop (line 273):

```c
	/* Old: text_datum = slot_getattr(slot, shared->attnum, &isnull); */

	/* Evaluate index expression (or extract plain column) */
	econtext->ecxt_scantuple = slot;
	FormIndexDatum(indexInfo, slot, estate, values, index_isnull);

	if (index_isnull[0])
		goto next_tuple;

	/* Check partial index predicate */
	if (indexInfo->ii_Predicate != NIL &&
	    !ExecQual(indexInfo->ii_PredicateState, econtext))
		goto next_tuple;

	document_text = DatumGetTextPP(values[0]);
```

Remove the `isnull` local variable declaration (line 262) and replace
references to `isnull` with `index_isnull[0]` (or just remove since
we check above).

Add `ResetExprContext(econtext);` after `MemoryContextReset(build_tmpctx);`
at the bottom of the loop.

Add cleanup before worker exit:

```c
	FreeExecutorState(estate);
```

- [ ] **Step 5: Add required includes to build_parallel.c**

```c
#include <catalog/index.h>
#include <executor/executor.h>
#include <nodes/execnodes.h>
```

- [ ] **Step 6: Build and run all tests**

```bash
make clean && make && make install && make installcheck
```

Expected: all tests pass including the expression_index test.

- [ ] **Step 7: Add parallel expression index test**

Append to `test/sql/expression_index.sql` before the cleanup section:

```sql
-- ============================================================
-- Parallel build with expression index (100K+ rows)
-- ============================================================

CREATE TABLE expr_parallel (
    id SERIAL PRIMARY KEY,
    data JSONB
);

-- Insert 100K rows to trigger parallel build
INSERT INTO expr_parallel (data)
SELECT jsonb_build_object(
    'content',
    'document number ' || i || ' with words like database search index'
)
FROM generate_series(1, 100000) AS i;

SET max_parallel_maintenance_workers = 2;
SET maintenance_work_mem = '256MB';

CREATE INDEX expr_parallel_idx ON expr_parallel
    USING bm25 ((data->>'content'))
    WITH (text_config='english');

SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database', 'expr_parallel_idx'))::numeric, 4)
              AS score
FROM expr_parallel
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_parallel_idx')
LIMIT 3;

DROP TABLE expr_parallel CASCADE;
```

Also add `DROP TABLE expr_parallel CASCADE;` to the cleanup section (if
not already covered by CASCADE).

- [ ] **Step 8: Run test and update expected output**

```bash
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test expression_index
cp test/results/expression_index.out test/expected/expression_index.out
```

Review output, then re-run to confirm pass.

- [ ] **Step 9: Format and commit**

```bash
make format
git add src/am/build_parallel.c src/am/build_parallel.h \
        test/sql/expression_index.sql test/expected/expression_index.out
git commit -m "feat: support expression/predicate evaluation in parallel build"
```

---

## Task 4: Update Vacuum Rebuild for Expressions and Predicates

**Files:**
- Modify: `src/am/vacuum.c:172-310`

Replace `heap_getattr(tuple, attnum, ...)` with `FormIndexDatum()`.

- [ ] **Step 1: Add includes to vacuum.c**

Add to the top of `src/am/vacuum.c`:

```c
#include <catalog/index.h>
#include <executor/executor.h>
#include <nodes/execnodes.h>
```

- [ ] **Step 2: Refactor tp_vacuum_rebuild_segment**

In `tp_vacuum_rebuild_segment()` (`src/am/vacuum.c:172`), replace the
`AttrNumber attnum` variable and `heap_getattr` call with
`FormIndexDatum`:

After the text_config_oid extraction (line 199), add:

```c
	/* Set up expression evaluation for index */
	IndexInfo   *indexInfo = BuildIndexInfo(index);
	EState      *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(
			RelationGetDescr(heap), &TTSOpsBufferHeapTuple);
	Datum        idx_values[INDEX_MAX_KEYS];
	bool         idx_isnull[INDEX_MAX_KEYS];
```

Remove the `AttrNumber attnum;` declaration (line 187) and the
`attnum = index->rd_index->indkey.values[0];` assignment (line 202).

In the per-doc loop, after `heap_fetch` succeeds (line 252), replace
the `heap_getattr` block (lines 260-268) with:

```c
		/* Evaluate index expression */
		ExecStoreBufferHeapTuple(tuple, slot, heap_buf);
		econtext->ecxt_scantuple = slot;
		FormIndexDatum(indexInfo, slot, estate, idx_values, idx_isnull);

		if (idx_isnull[0])
		{
			ExecClearTuple(slot);
			ReleaseBuffer(heap_buf);
			continue;
		}

		/* Check partial index predicate */
		if (indexInfo->ii_Predicate != NIL &&
		    !ExecQual(indexInfo->ii_PredicateState, econtext))
		{
			ExecClearTuple(slot);
			ReleaseBuffer(heap_buf);
			continue;
		}
```

Replace `document_text = DatumGetTextPP(text_datum);` with
`document_text = DatumGetTextPP(idx_values[0]);`

At the end of each iteration, after `ReleaseBuffer(heap_buf);` add:

```c
		ExecClearTuple(slot);
		ResetExprContext(econtext);
```

After the loop, add cleanup:

```c
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
```

- [ ] **Step 3: Build and run vacuum tests**

```bash
make clean && make && make install
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test vacuum vacuum_extended vacuum_rebuild
```

Expected: all vacuum tests pass.

- [ ] **Step 4: Run full test suite**

```bash
make installcheck
```

Expected: all tests pass.

- [ ] **Step 5: Format and commit**

```bash
make format
git add src/am/vacuum.c
git commit -m "feat: support expression/predicate evaluation in vacuum rebuild"
```

---

## Task 5: Update Crash Recovery for Expressions and Predicates

**Files:**
- Modify: `src/state/state.c:851-1030`

Replace `heap_getattr(tuple, attnum, ...)` with `FormIndexDatum()` in
`tp_rebuild_posting_lists_from_docids()`.

- [ ] **Step 1: Add includes to state.c**

Add to the top of `src/state/state.c`:

```c
#include <catalog/index.h>
#include <executor/executor.h>
#include <nodes/execnodes.h>
```

- [ ] **Step 2: Refactor tp_rebuild_posting_lists_from_docids**

After opening `heap_rel` (line 876), add:

```c
	/* Set up expression evaluation */
	IndexInfo   *indexInfo = BuildIndexInfo(index_rel);
	EState      *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(
			RelationGetDescr(heap_rel), &TTSOpsBufferHeapTuple);
	Datum        idx_values[INDEX_MAX_KEYS];
	bool         idx_isnull[INDEX_MAX_KEYS];
```

In the per-docid loop, after `heap_fetch` succeeds (line 985), replace
the `heap_getattr` block (lines 993-1023) with:

```c
			/* Evaluate index expression */
			ExecStoreBufferHeapTuple(tuple, slot, heap_buf);
			econtext->ecxt_scantuple = slot;
			FormIndexDatum(indexInfo, slot, estate,
			               idx_values, idx_isnull);

			if (!idx_isnull[0])
			{
				text *document_text = DatumGetTextPP(idx_values[0]);

				if (tp_process_document_text(
							document_text,
							ctid,
							metap->text_config_oid,
							local_state,
							NULL,
							&doc_length))
				{
					local_state->shared->total_docs++;
					local_state->shared->total_len += doc_length;
				}
			}

			ExecClearTuple(slot);
			ResetExprContext(econtext);
			ReleaseBuffer(heap_buf);
```

Remove the now-unused `AttrNumber attnum;` declaration (line 949) and
`attnum = index_rel->rd_index->indkey.values[0];` (line 998).

Before `relation_close(heap_rel, ...)` at the end, add:

```c
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
```

- [ ] **Step 3: Build and run recovery test**

```bash
make clean && make && make install
make test-recovery
```

Expected: recovery tests pass.

- [ ] **Step 4: Run full test suite**

```bash
make installcheck
```

Expected: all tests pass.

- [ ] **Step 5: Format and commit**

```bash
make format
git add src/state/state.c
git commit -m "feat: support expression evaluation in crash recovery path"
```

---

## Task 6: Update Scan Index Matching for Expressions

**Files:**
- Modify: `src/am/scan.c:260-318`

The `indexes_match_by_attribute()` function (or similar) compares
indexes by attribute number and column name. For expression indexes
(`indkey.values[0] == 0`), compare expression trees instead.

- [ ] **Step 1: Update index matching logic in scan.c**

In `src/am/scan.c`, find the function that compares indexes (around
lines 260-318). After the existing attnum/column-name comparison, add
an expression comparison branch:

```c
		/*
		 * For expression indexes (attnum == 0), compare the
		 * stored expression trees from pg_index.
		 */
		if (scan_attnum == 0 && query_attnum == 0)
		{
			List *scan_exprs = RelationGetIndexExpressions(
					index_open(scan_index_oid, AccessShareLock));
			List *query_exprs = RelationGetIndexExpressions(
					index_open(query_index_oid, AccessShareLock));

			/* Close immediately — we only needed the expressions */
			index_close(
					index_open(scan_index_oid, AccessShareLock),
					AccessShareLock);
			index_close(
					index_open(query_index_oid, AccessShareLock),
					AccessShareLock);

			if (scan_exprs != NIL && query_exprs != NIL &&
			    equal(linitial(scan_exprs), linitial(query_exprs)))
			{
				result = true;
			}
		}
```

Note: The exact implementation will depend on how the existing function
is structured. The key idea is: when both indexes have attnum=0
(expressions), deserialize and compare expression trees with `equal()`.
You may need to use `RelationGetIndexExpressions()` which requires
opening the index relation, or read `indexprs` from the `pg_index`
catalog directly.

A cleaner approach is to use the already-fetched `pg_index` tuples:

```c
		if (scan_attnum == 0 && query_attnum == 0)
		{
			Datum  scan_expr_datum, query_expr_datum;
			bool   scan_isnull, query_isnull;
			List  *scan_exprs, *query_exprs;

			scan_expr_datum = SysCacheGetAttr(
					INDEXRELID, scan_idx_tuple,
					Anum_pg_index_indexprs, &scan_isnull);
			query_expr_datum = SysCacheGetAttr(
					INDEXRELID, query_idx_tuple,
					Anum_pg_index_indexprs, &query_isnull);

			if (!scan_isnull && !query_isnull)
			{
				char *scan_str =
					TextDatumGetCString(scan_expr_datum);
				char *query_str =
					TextDatumGetCString(query_expr_datum);

				scan_exprs =
					(List *)stringToNode(scan_str);
				query_exprs =
					(List *)stringToNode(query_str);

				pfree(scan_str);
				pfree(query_str);

				if (equal(scan_exprs, query_exprs))
					result = true;
			}
		}
```

This uses the `pg_index` tuples already in scope. Add a branch so this
is tried instead of the column-name comparison when both attnums are 0.

- [ ] **Step 2: Add required includes**

If not already present:

```c
#include <catalog/pg_index.h>
#include <nodes/readfuncs.h>
```

- [ ] **Step 3: Build and run partitioned tests**

```bash
make clean && make && make install
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test partitioned partitioned_many inheritance
```

Expected: existing partitioned tests still pass.

- [ ] **Step 4: Run full test suite**

```bash
make installcheck
```

Expected: all tests pass.

- [ ] **Step 5: Format and commit**

```bash
make format
git add src/am/scan.c
git commit -m "feat: support expression index matching in scan path"
```

---

## Task 7: Generalize Planner Hooks for Expression Indexes

**Files:**
- Modify: `src/planner/hooks.c`

This is the largest single change. Generalize from Var-only matching to
arbitrary expressions.

- [ ] **Step 1: Add a helper to extract relation OID from an expression**

Add in `src/planner/hooks.c` near the existing helpers:

```c
/*
 * Extract relation OID from an arbitrary expression by collecting
 * all Var nodes. Returns InvalidOid if vars reference multiple
 * relations or none.
 */
static Oid
get_expr_relid(Node *expr, Query *query, Index *varno_out)
{
	List     *vars;
	ListCell *lc;
	Oid       relid = InvalidOid;
	Index     varno = 0;

	vars = pull_var_clause(expr, 0);
	if (vars == NIL)
		return InvalidOid;

	foreach (lc, vars)
	{
		Var           *var = (Var *)lfirst(lc);
		RangeTblEntry *rte;

		if (var->varlevelsup != 0)
		{
			list_free(vars);
			return InvalidOid;
		}

		if (var->varno < 1 ||
		    var->varno > (Index)list_length(query->rtable))
		{
			list_free(vars);
			return InvalidOid;
		}

		rte = rt_fetch(var->varno, query->rtable);
		if (rte->rtekind != RTE_RELATION)
		{
			list_free(vars);
			return InvalidOid;
		}

		if (relid == InvalidOid)
		{
			relid = rte->relid;
			varno = var->varno;
		}
		else if (rte->relid != relid)
		{
			/* Expression references multiple relations */
			list_free(vars);
			return InvalidOid;
		}
	}

	list_free(vars);
	if (varno_out)
		*varno_out = varno;
	return relid;
}
```

- [ ] **Step 2: Add find_bm25_index_for_expr**

```c
/*
 * Find BM25 index matching an expression on a relation.
 *
 * For plain Var expressions, uses the fast attnum comparison.
 * For complex expressions, deserializes indexprs and compares
 * expression trees with equal() after varno normalization.
 */
static Oid
find_bm25_index_for_expr(
        Node *expr, Oid relid, Index varno, Oid bm25_am_oid)
{
	Relation    indexRelation;
	SysScanDesc scan;
	HeapTuple   indexTuple;
	Oid         result      = InvalidOid;
	int         index_count = 0;
	ScanKeyData scanKey;

	if (!OidIsValid(bm25_am_oid))
		return InvalidOid;

	/* Fast path: plain column reference */
	if (IsA(expr, Var))
	{
		Var *var = (Var *)expr;
		return find_bm25_index_for_column(
				relid, var->varattno, bm25_am_oid);
	}

	/* Slow path: expression comparison */
	indexRelation = table_open(IndexRelationId, AccessShareLock);

	ScanKeyInit(
			&scanKey,
			Anum_pg_index_indrelid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(relid));

	scan = systable_beginscan(
			indexRelation, IndexIndrelidIndexId, true, NULL, 1,
			&scanKey);

	while ((indexTuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_index indexForm =
				(Form_pg_index)GETSTRUCT(indexTuple);
		Oid       indexOid = indexForm->indexrelid;
		HeapTuple classTuple;
		Oid       indexAmOid;

		/* Check if this is a BM25 index */
		classTuple = SearchSysCache1(
				RELOID, ObjectIdGetDatum(indexOid));
		if (!HeapTupleIsValid(classTuple))
			continue;

		indexAmOid =
				((Form_pg_class)GETSTRUCT(classTuple))->relam;
		ReleaseSysCache(classTuple);

		if (indexAmOid != bm25_am_oid)
			continue;

		/* Must be an expression index (attnum == 0) */
		if (indexForm->indkey.values[0] != 0)
			continue;

		/* Deserialize and compare expression trees */
		{
			Datum exprDatum;
			bool  isnull;
			List *idx_exprs;
			Node *idx_expr;

			exprDatum = SysCacheGetAttr(
					INDEXRELID,
					indexTuple,
					Anum_pg_index_indexprs,
					&isnull);

			if (isnull)
				continue;

			idx_exprs = (List *)stringToNode(
					TextDatumGetCString(exprDatum));
			if (idx_exprs == NIL)
				continue;

			idx_expr = (Node *)linitial(idx_exprs);

			/*
			 * pg_index stores expressions with varno=1.
			 * The query expression uses the actual varno.
			 * Normalize the stored expression to match.
			 */
			ChangeVarNodes(idx_expr, 1, varno, 0);

			if (equal(idx_expr, expr))
			{
				index_count++;
				if (result == InvalidOid)
					result = indexOid;
			}
		}
	}

	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

	if (index_count > 1)
		ereport(WARNING,
				(errmsg("multiple BM25 indexes match the expression"),
				 errhint("Use explicit to_bm25query('query', "
				         "'index_name') to specify which index.")));

	return result;
}
```

- [ ] **Step 3: Update transform_tpquery_opexpr to handle expressions**

In `transform_tpquery_opexpr()` (line 406), replace the Var-only check
(lines 444-452) with:

```c
	/*
	 * The left operand of <@> can be a column reference or an
	 * expression. Extract the relation OID for index lookup.
	 */
	{
		Oid   relid;
		Index expr_varno;

		if (IsA(left, Var))
		{
			Var *var = (Var *)left;
			if (!get_var_relation_and_attnum(
					var, context->query, &relid, &attnum_unused))
				return NULL;
			expr_varno = var->varno;
		}
		else
		{
			relid = get_expr_relid(left, context->query,
			                       &expr_varno);
			if (!OidIsValid(relid))
				return NULL;
		}
```

For the explicit index validation block (lines 459-490), replace the
`index_is_on_column` check with a more general check that handles both
columns and expressions. For the `IsA(left, Var)` case, keep the
existing `index_is_on_column` check. For expressions, compare the
expression tree against the index's stored expression.

For the implicit resolution (line 492), replace
`find_index_for_var((Var *)left, context)` with:

```c
		index_oid = find_bm25_index_for_expr(
				left, relid, expr_varno,
				context->oid_cache->bm25_am_oid);
```

- [ ] **Step 4: Update transform_text_text_opexpr similarly**

In `transform_text_text_opexpr()` (line 510), apply the same pattern:
remove the `IsA(left, Var)` error (lines 533-540), extract relid from
expression, and use `find_bm25_index_for_expr()`.

- [ ] **Step 5: Update collect_explicit_indexes_walker for expressions**

In `collect_explicit_indexes_walker()` (line 1180), the left-arg
`IsA(left, Var)` check (line 1197) needs to be relaxed. For
expressions, extract the relid via `get_expr_relid()` and store it in
the `ExplicitIndexRequirement`. The `attnum` field of
`ExplicitIndexRequirement` should be set to 0 for expression indexes,
and `find_explicit_requirement_for_column()` should be updated to also
match by expression tree comparison (or the struct should store the
expression node for matching).

This is the trickiest part — the `ExplicitIndexRequirement` struct
(line 86) and `find_explicit_requirement_for_column` (line 1282) need
to be generalized. Add an `expr` field:

```c
typedef struct ExplicitIndexRequirement
{
	Oid        relid;
	AttrNumber attnum;              /* 0 for expression indexes */
	Node      *expr;                /* Expression node (if attnum==0) */
	Oid        required_index_oid;
} ExplicitIndexRequirement;
```

Update `find_explicit_requirement_for_column` to also accept an
expression and match by `equal()` when attnum is 0.

- [ ] **Step 6: Add required includes**

```c
#include <optimizer/clauses.h>   /* for pull_var_clause */
#include <nodes/readfuncs.h>     /* for stringToNode */
#include <catalog/pg_index.h>    /* for Anum_pg_index_indexprs */
#include <rewrite/rewriteManip.h> /* for ChangeVarNodes */
```

- [ ] **Step 7: Build and run tests**

```bash
make clean && make && make install && make installcheck
```

Expected: all tests pass including expression_index. Check that the
implicit resolution tests in expression_index.sql also work.

- [ ] **Step 8: Add implicit resolution tests to expression_index.sql**

Append before cleanup:

```sql
-- ============================================================
-- Implicit index resolution for expressions
-- ============================================================

CREATE TABLE expr_implicit (
    id SERIAL PRIMARY KEY,
    data JSONB
);

INSERT INTO expr_implicit (data) VALUES
    ('{"content": "postgresql database management"}'),
    ('{"content": "machine learning algorithms"}'),
    ('{"content": "full text search and retrieval"}');

CREATE INDEX expr_implicit_idx ON expr_implicit
    USING bm25 ((data->>'content'))
    WITH (text_config='english');

-- text <@> text form (implicit resolution)
EXPLAIN (COSTS OFF)
SELECT id
FROM expr_implicit
ORDER BY (data->>'content') <@> 'database'
LIMIT 3;

-- to_bm25query without index name (implicit)
EXPLAIN (COSTS OFF)
SELECT id
FROM expr_implicit
ORDER BY (data->>'content') <@> to_bm25query('database')
LIMIT 3;

-- Verify JOIN with expression (varno normalization)
CREATE TABLE expr_other (id INT);
INSERT INTO expr_other VALUES (1), (2), (3);

SELECT e.id,
       ROUND(((e.data->>'content') <@>
              to_bm25query('database',
                           'expr_implicit_idx'))::numeric, 4)
              AS score
FROM expr_other o
JOIN expr_implicit e ON o.id = e.id
ORDER BY (e.data->>'content') <@>
         to_bm25query('database', 'expr_implicit_idx')
LIMIT 3;

DROP TABLE expr_other;
DROP TABLE expr_implicit CASCADE;
```

- [ ] **Step 9: Run test and update expected output**

```bash
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test expression_index
cp test/results/expression_index.out test/expected/expression_index.out
```

- [ ] **Step 10: Format and commit**

```bash
make format
git add src/planner/hooks.c test/sql/expression_index.sql \
        test/expected/expression_index.out
git commit -m "feat: generalize planner hooks for expression index resolution"
```

---

## Task 8: Add Partial Index Support and Tests

**Files:**
- Create: `test/sql/partial_index.sql`
- Modify: `Makefile:73`

The build, vacuum, and recovery paths already have predicate checks
from Tasks 2-5. The planner hook needs partial index awareness for
implicit resolution. This task adds the predicate check to the planner
and creates the test file.

- [ ] **Step 1: Add predicate check to find_bm25_index_for_column**

In `src/planner/hooks.c`, update `find_bm25_index_for_column()` to
skip partial indexes whose predicate is not implied by the query
context. This requires passing the query's restriction clauses.

However, the simpler approach for implicit resolution is: during index
scanning, check if the index has a predicate. If it does, it should
only be used when the user explicitly names it or when the standard
planner picks it (which already does `predicate_implied_by`). For
implicit resolution in the hook, skip partial indexes:

```c
		/* Skip partial indexes for implicit resolution —
		 * the standard planner handles predicate matching.
		 * Users can still reference them explicitly. */
		if (indexForm->indpred != 0)
		{
			Datum predDatum;
			bool  predNull;

			predDatum = SysCacheGetAttr(
					INDEXRELID, indexTuple,
					Anum_pg_index_indpred, &predNull);
			if (!predNull)
				continue;
		}
```

Wait — this is too restrictive. We want implicit resolution to work
when the query WHERE clause implies the predicate. But accessing query
restrictions from inside `find_bm25_index_for_column` requires
threading more context through.

A pragmatic first approach: for implicit resolution, prefer non-partial
indexes. If no non-partial BM25 index exists and partial ones do, skip
them (user must use explicit naming). This is safe and can be enhanced
later with `predicate_implied_by` integration.

Add a check in `find_bm25_index_for_column()` inside the matching
loop, after the attnum comparison succeeds:

```c
				/* Skip partial indexes for implicit resolution */
				{
					bool has_predicate = false;
					Datum predDatum;
					bool  predNull;

					predDatum = SysCacheGetAttr(
							INDEXRELID, indexTuple,
							Anum_pg_index_indpred, &predNull);
					has_predicate = !predNull;

					if (has_predicate)
						continue;
				}
```

Apply the same check in `find_bm25_index_for_expr()`.

- [ ] **Step 2: Write partial index test file**

Create `test/sql/partial_index.sql`:

```sql
-- Partial index tests for pg_textsearch

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- ============================================================
-- Basic partial index
-- ============================================================

CREATE TABLE partial_docs (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT
);

INSERT INTO partial_docs (content, category) VALUES
    ('postgresql database management', 'tech'),
    ('machine learning algorithms', 'tech'),
    ('cooking recipes for beginners', 'lifestyle'),
    ('full text search and retrieval', 'tech'),
    ('gardening tips and tricks', 'lifestyle');

CREATE INDEX partial_tech_idx ON partial_docs
    USING bm25 (content)
    WITH (text_config='english')
    WHERE category = 'tech';

SET enable_seqscan = off;

-- Explicit query against partial index
SELECT id, content,
       ROUND((content <@>
              to_bm25query('database', 'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@> to_bm25query('database', 'partial_tech_idx')
LIMIT 5;

-- Non-matching rows should not appear
SELECT id, content,
       ROUND((content <@>
              to_bm25query('cooking', 'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@> to_bm25query('cooking', 'partial_tech_idx')
LIMIT 5;

-- ============================================================
-- Multi-language use case
-- ============================================================

CREATE TABLE multilang (
    id SERIAL PRIMARY KEY,
    content TEXT,
    language TEXT
);

INSERT INTO multilang (content, language) VALUES
    ('the quick brown fox jumps over the lazy dog', 'english'),
    ('postgresql database management system', 'english'),
    ('full text searching and information retrieval', 'english'),
    ('simple words without stemming needed', 'simple'),
    ('another simple document for testing', 'simple');

CREATE INDEX multilang_english_idx ON multilang
    USING bm25 (content)
    WITH (text_config='english')
    WHERE language = 'english';

CREATE INDEX multilang_simple_idx ON multilang
    USING bm25 (content)
    WITH (text_config='simple')
    WHERE language = 'simple';

-- Query English documents with English stemming
SELECT id, content,
       ROUND((content <@>
              to_bm25query('databases',
                           'multilang_english_idx'))::numeric, 4)
              AS score
FROM multilang
WHERE language = 'english'
ORDER BY content <@>
         to_bm25query('databases', 'multilang_english_idx')
LIMIT 5;

-- Query simple documents (no stemming)
SELECT id, content,
       ROUND((content <@>
              to_bm25query('simple',
                           'multilang_simple_idx'))::numeric, 4)
              AS score
FROM multilang
WHERE language = 'simple'
ORDER BY content <@>
         to_bm25query('simple', 'multilang_simple_idx')
LIMIT 5;

-- ============================================================
-- Expression + partial combined
-- ============================================================

CREATE TABLE expr_partial (
    id SERIAL PRIMARY KEY,
    data JSONB
);

INSERT INTO expr_partial (data) VALUES
    ('{"text": "postgresql database management", "type": "article"}'),
    ('{"text": "machine learning algorithms", "type": "article"}'),
    ('{"text": "random user comment", "type": "comment"}'),
    ('{"text": "full text search retrieval", "type": "article"}');

CREATE INDEX expr_partial_idx ON expr_partial
    USING bm25 ((data->>'text'))
    WITH (text_config='english')
    WHERE (data->>'type') = 'article';

SELECT id,
       ROUND(((data->>'text') <@>
              to_bm25query('database',
                           'expr_partial_idx'))::numeric, 4)
              AS score
FROM expr_partial
WHERE (data->>'type') = 'article'
ORDER BY (data->>'text') <@>
         to_bm25query('database', 'expr_partial_idx')
LIMIT 5;

-- ============================================================
-- Insert into partial index — predicate filtering
-- ============================================================

-- Insert a row that does NOT match the predicate
INSERT INTO partial_docs (content, category) VALUES
    ('database administration basics', 'lifestyle');

-- It should not appear in partial index results
SELECT id, content,
       ROUND((content <@>
              to_bm25query('database', 'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@> to_bm25query('database', 'partial_tech_idx')
LIMIT 5;

-- ============================================================
-- Cleanup
-- ============================================================

DROP TABLE partial_docs CASCADE;
DROP TABLE multilang CASCADE;
DROP TABLE expr_partial CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
```

- [ ] **Step 3: Add partial_index to Makefile REGRESS list**

Add `partial_index` to the REGRESS line in `Makefile:73`.

- [ ] **Step 4: Build and run the test**

```bash
make clean && make && make install
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test partial_index
```

- [ ] **Step 5: Generate expected output**

```bash
cp test/results/partial_index.out test/expected/partial_index.out
```

Review output for correctness.

- [ ] **Step 6: Run full test suite**

```bash
make installcheck
```

Expected: all tests pass.

- [ ] **Step 7: Format and commit**

```bash
make format
git add src/planner/hooks.c test/sql/partial_index.sql \
        test/expected/partial_index.out Makefile
git commit -m "feat: add partial index support with predicate filtering"
```

---

## Task 9: Update unsupported.sql and Shell Tests

**Files:**
- Modify: `test/sql/unsupported.sql:84-91`
- Modify: `test/expected/unsupported.out`

- [ ] **Step 1: Remove expression index limitation from unsupported.sql**

In `test/sql/unsupported.sql`, remove lines 84-91 (LIMITATION 3:
Expression indexes) and renumber remaining limitations.

The section to remove:

```sql
-- =============================================================================
-- LIMITATION 3: Expression indexes are not supported
-- =============================================================================

\echo 'Test: Expression index - should fail with error'

CREATE INDEX docs_lower_idx ON docs USING bm25(lower(content))
    WITH (text_config='simple');
```

Renumber LIMITATION 4 → LIMITATION 3, LIMITATION 5 → LIMITATION 4.

- [ ] **Step 2: Update expected output**

```bash
make clean && make && make install
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test unsupported
cp test/results/unsupported.out test/expected/unsupported.out
```

- [ ] **Step 3: Run full test suite**

```bash
make installcheck
```

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/sql/unsupported.sql test/expected/unsupported.out
git commit -m "test: remove expression index limitation, renumber unsupported tests"
```

---

## Task 10: Final Validation and Format Check

- [ ] **Step 1: Clean build and full test suite**

```bash
make clean && make && make install && make installcheck
```

Expected: all tests pass. Check `test/regression.diffs` is empty.

- [ ] **Step 2: Run shell tests**

```bash
make test-shell
```

Expected: all shell tests pass.

- [ ] **Step 3: Format check**

```bash
make format-check
```

Expected: no formatting issues. If there are, run `make format` and
commit.

- [ ] **Step 4: Review all changes**

```bash
git log --oneline main..HEAD
git diff main --stat
```

Verify the commit history is clean and all expected files are modified.

- [ ] **Step 5: Final commit if needed**

If format check required changes:

```bash
make format
git add -u
git commit -m "style: fix formatting"
```
