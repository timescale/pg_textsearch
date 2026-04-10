# Expression and Partial Index Support

**Date**: 2026-04-10
**Status**: Draft
**Scope**: Single PR covering both expression indexes and partial indexes

## Overview

pg_textsearch currently requires BM25 indexes on plain column references.
This design adds support for:

1. **Expression indexes** -- index arbitrary expressions that evaluate to
   text (JSONB extraction, multi-column concatenation, transformations)
2. **Partial indexes** -- index a subset of rows matching a WHERE predicate
   (multi-language indexing, category-scoped search)

These can be combined:

```sql
CREATE INDEX ON t USING bm25 ((data->>'text'))
  WITH (text_config='english') WHERE (data->>'type') = 'article';
```

## Motivation

Expression indexes enable indexing computed text (JSONB fields,
concatenated columns, transformations) without requiring a materialized
column. Partial indexes enable per-language text search configurations
and scoped indexes that only cover relevant rows, reducing index size
and improving query performance.

## Current Limitations

The codebase assumes plain-column indexes in ~7 locations:

| Component         | File                  | Assumption                          |
|-------------------|-----------------------|-------------------------------------|
| Build validation  | `build.c:809`         | Rejects `ii_IndexAttrNumbers[0]==0` |
| Serial build      | `build.c:1065`        | `slot_getattr(slot, attnum)`        |
| Parallel build    | `build_parallel.c`    | Passes attnum to workers            |
| Vacuum rebuild    | `vacuum.c:201-262`    | `heap_getattr(tuple, attnum)`       |
| Crash recovery    | `state.c:998-1000`    | `heap_getattr(tuple, attnum)`       |
| Scan matching     | `scan.c:272-301`      | Compares attnum and column names    |
| Planner hooks     | `hooks.c:220-540`     | Var-only matching, attnum lookups   |

## Design

### 1. Serial Build: Switch to IndexBuildHeapScan

Replace the custom `table_scan_getnextslot()` loop with
`IndexBuildHeapScan()`, the standard Postgres API used by GIN, GiST,
and other AMs.

`IndexBuildHeapScan` automatically:
- Evaluates index expressions via `FormIndexDatum()`
- Filters rows failing the partial index predicate via `ExecQual()`
- Handles snapshot management and visibility checking
- Calls our callback once per qualifying tuple with pre-evaluated values

The callback signature:

```c
typedef struct TpBuildCallbackState {
    TpBuildContext *build_ctx;
    Relation        index;
    Oid             text_config_oid;
    MemoryContext   per_doc_ctx;
    uint64          total_docs;
    uint64          total_len;
} TpBuildCallbackState;

static void
tp_build_callback(Relation index, ItemPointer ctid,
                  Datum *values, bool *isnull,
                  bool tupleIsAlive, void *state)
{
    /* values[0] is the already-evaluated expression result.
     * Predicate-failing rows never reach this callback.
     * Tokenize, add to build context, flush if budget exceeded. */
}
```

The arena-based batching and spill-to-disk logic moves into the
callback. Finalization (writing the last segment, updating metapage,
transitioning to runtime) happens after `IndexBuildHeapScan()` returns.

`tp_setup_table_scan()` is no longer needed for the serial build path
and can be removed or kept for other uses.

### 2. Parallel Build: Add Expression/Predicate Evaluation

Keep the existing custom parallel architecture (TID-range scanning,
per-worker BufFile segments, leader N-way merge). Add expression and
predicate evaluation to the worker scan loop:

1. Workers call `BuildIndexInfo()` after opening the index relation to
   get expression and predicate state.
2. Replace `slot_getattr(slot, shared->attnum, &isnull)` with
   `FormIndexDatum(indexInfo, slot, estate, values, isnull)`.
3. Add `ExecQual(indexInfo->ii_PredicateState, econtext)` to skip rows
   not satisfying the partial index predicate.
4. Remove `attnum` from `TpParallelBuildShared` -- workers get
   column/expression info from `BuildIndexInfo()`.

Visibility is already handled by the snapshot-based scan under the
ShareLock held during non-concurrent CREATE INDEX.

The rest of the parallel pipeline (BufFile segments, N-way merge,
leader finalization) is unchanged.

### 3. Insert Path: No Changes

Postgres's executor already evaluates the index expression and passes
the result in `values[0]`, and checks the partial index predicate
before calling `aminsert`. `tp_insert()` works with `values[0]` and
`isnull[0]` -- no changes needed.

### 4. Vacuum Rebuild: FormIndexDatum

Replace `heap_getattr(tuple, attnum, ...)` with `FormIndexDatum()`:

1. Build `IndexInfo` via `BuildIndexInfo(index)` at the start of vacuum
   rebuild.
2. Set up `EState` and `ExprContext` for expression evaluation.
3. For each tuple, store in a `TupleTableSlot` and call
   `FormIndexDatum()` to get the text value.
4. For partial indexes, check `ExecQual()` on the predicate -- tuples
   no longer satisfying the predicate are skipped during rebuild.

A shared helper reduces duplication with the recovery path:

```c
Datum
tp_eval_indexed_text(IndexInfo *indexInfo,
                     TupleTableSlot *slot,
                     ExprContext *econtext,
                     bool *isnull)
```

For plain-column indexes, `FormIndexDatum` is equivalent to
`slot_getattr()` -- both cases are handled uniformly.

### 5. Crash Recovery: FormIndexDatum

Same pattern as vacuum. In `tp_rebuild_posting_lists_from_docids()`:

1. Build `IndexInfo` and set up `EState`/`ExprContext` at the start.
2. For each fetched tuple, use `ExecStoreBufferHeapTuple()` to put the
   `HeapTupleData` into a slot (currently uses raw tuple with
   `heap_getattr`).
3. Call `tp_eval_indexed_text()` instead of `heap_getattr()`.
4. For partial indexes, check predicate for consistency (docid pages
   should only contain rows that originally satisfied the predicate).

### 6. Planner Hooks

Two changes: expression matching and partial index awareness.

#### Expression matching

**Remove the Var-only restriction** in `transform_tpquery_opexpr()` and
`transform_text_text_opexpr()`. The left side of `<@>` can be any
expression evaluating to text.

**Replace `find_bm25_index_for_column()`** with
`find_bm25_index_for_expr()`:
- If the expression is a plain `Var`: check `indkey.values[0] ==
  attnum` (fast path, same as today)
- If the expression is complex: deserialize `indexprs` from `pg_index`,
  normalize varnos with `ChangeVarNodes()`, compare with `equal()`

**Extract relation info from expressions**: Replace
`get_var_relation_and_attnum()` with a helper using
`pull_var_clause()` to find all Var nodes, verify they reference the
same relation, and return the relation OID.

**Explicit index validation** for expressions: When the user specifies
an index via `to_bm25query('query', 'idx')`, validate that the index
expression matches the query expression.

#### Partial index awareness

For implicit resolution in our hook:
1. When scanning candidate BM25 indexes, check if the index has a
   predicate (`indpred` is non-null).
2. If it does, check `predicate_implied_by(index_predicate,
   query_restrictions)` -- if the query WHERE clause implies the
   predicate, the index is usable.
3. Non-partial indexes are always usable.
4. Multiple matching partial indexes trigger the existing multi-index
   warning.

For the standard planner path, Postgres's `indxpath.c` already calls
`predicate_implied_by()` when considering index paths. The AM just
needs to properly expose predicate info, which happens automatically
via `pg_index.indpred`.

### 7. Scan Path (Index Matching)

For partitioned/hypertable queries, `scan.c` validates that a scan
index matches the query index by comparing attribute numbers and column
names.

For expression indexes (`indkey.values[0] == 0`):
- Deserialize `indexprs` from both indexes
- Compare with `equal()` after normalizing catalog-level differences

For partial indexes: no change needed in scan matching -- the predicate
is per-partition and the planner handles partition pruning.

### 8. Handler

`amcanmulticol` stays false (single key column). The existing
`tp_validate()` checks the opclass input type, which works for both
plain columns and expressions since both must evaluate to text.

Remove the expression-index rejection in `tp_build()` (line 809).

## Test Plan

### Expression Index Tests (`test/sql/expression_index.sql`)

**Basic functionality:**
- JSONB field extraction: `(data->>'content')`
- Multi-column concat: `(coalesce(title,'') || ' ' || coalesce(body,''))`
- Simple transformation: `(lower(content))`
- Verify correct search results for each

**Query forms (all three):**
- `expr <@> 'query text'` (implicit text <@> text)
- `expr <@> to_bm25query('query')` (implicit index resolution)
- `expr <@> to_bm25query('query', 'idx_name')` (explicit)

**Planner verification:**
- EXPLAIN shows index scan for expression queries
- Varno normalization: indexed table as second relation in JOIN

**Error cases:**
- Non-text expression type rejected (e.g., `(length(content))`)
- Explicit index name not matching expression errors clearly

### Partial Index Tests (`test/sql/partial_index.sql`)

**Basic functionality:**
- `WHERE category = 'tech'` partial index
- Search returns only predicate-matching rows
- Non-matching rows not indexed

**Multi-language use case:**
- Two partial indexes with different `text_config` values by language
- Verify correct index chosen when query WHERE implies predicate
- Verify correct per-language stemming

**Query forms:**
- Implicit: `WHERE language = 'english' ORDER BY content <@> 'query'`
- Explicit: `to_bm25query('query', 'docs_english_idx')`

**Planner verification:**
- EXPLAIN shows correct partial index chosen based on WHERE clause

**Edge cases:**
- Query without WHERE implying predicate: no partial index used
- Falls back to non-partial index or seq scan

### Combined Tests

- Expression + partial: `(data->>'text') WHERE (data->>'type') = 'article'`
- Parallel build with expression index (100K+ rows)

### Shell Tests

- Extend `recovery.sh` with expression index recovery
- Extend `segment.sh` with expression index segment operations

### Existing Test Updates

- Remove "LIMITATION 3: Expression indexes" from `unsupported.sql`
- Update `expected/unsupported.out`

## Prior Art

Community PR #154 by SteveLauC attempted expression indexes using a
similar approach (FormIndexDatum, expression tree matching in planner).
This design differs by:
- Switching the serial build path to `IndexBuildHeapScan` (PR #154
  kept the custom scan loop)
- Adding partial index support (not in PR #154)
- Using a shared helper for expression evaluation in vacuum/recovery

## Out of Scope

- `CREATE INDEX CONCURRENTLY` -- not supported today, unrelated
- Multi-column indexes (`amcanmulticol` remains false)
- Bitmap scans on partial indexes
