# Design Document: Index Ergonomics and Partitioned Tables

## Problem Statement

Three interrelated issues need to be addressed before pg_textsearch syntax becomes stable:

1. **Partitioned table support**: Each partition gets its own index with its own
   name, making explicit index references in queries unworkable.

2. **Syntax ergonomics**: Users must explicitly reference the index by name in
   `<@>` and `to_tpquery()` calls, which is verbose and error-prone.

3. **Duplicate scoring**: When scores from `<@>` are returned in the SELECT
   clause, a separate scoring codepath is invoked, causing both performance
   overhead and maintenance burden (two codepaths to keep in sync).

## Current State Analysis

### pg_textsearch (Current)

```sql
-- Current syntax: explicit index name required in ORDER BY
SELECT id, content <@> to_tpquery('documents_idx', 'search terms') AS score
FROM documents
ORDER BY score;
```

The `<@>` operator is used only in ORDER BY clauses. The index scan implicitly
finds matching documents while computing scores - no separate WHERE clause is
needed for BM25 filtering.

**Issues**:
- `tpquery` carries index name as embedded string
- Two scoring paths: `tp_score_documents()` (batch, index scan) and
  `text_tpquery_score()` (per-row, operator in SELECT)
- Index name resolution via `RelnameGetRelid()` - no partition awareness

### ParadeDB Approach

```sql
-- Simple case: index auto-detected, @@@ in WHERE triggers custom scan
SELECT id, pdb.score(id) FROM documents WHERE content @@@ 'search';

-- Complex case: explicit index for joins/subqueries
SELECT * FROM documents
WHERE id @@@ paradedb.with_index('idx', paradedb.term('content', 'search'));
```

Note: ParadeDB uses `@@@` in WHERE to trigger their custom scan node, which is
a different architecture than our ORDER BY-based approach.

**Key mechanisms**:
1. `rel_get_bm25_index()` scans relation for BM25 indexes automatically
2. `with_index()` wrapper for explicit specification when needed
3. PlaceHolderVar wrapping preserves scores across JOINs
4. Full partition support via planner-level `is_partitioned_table_setup()`
5. Single scoring path - scores cached during custom scan, never re-computed

### VectorChord-BM25 Approach

```sql
-- Explicit index always required
SELECT id, embedding <&> to_bm25query('idx', query_vec) AS rank
FROM documents ORDER BY rank;
```

**Key mechanisms**:
1. `bm25query` is a composite type: `(index_oid regclass, query_vector bm25vector)`
2. Index OID stored directly in query type (not name string)
3. No partition support
4. Operator computes score on-the-fly per row (no caching)

## Design Goals

1. Support partitioned tables without requiring users to know partition names
2. Allow index inference from context where possible
3. Eliminate duplicate scoring - compute once, retrieve via function
4. Keep syntax clean and intuitive
5. Resolve syntax before 1.0 release (breaking changes acceptable now)

---

## Alternatives Summary

| Alternative | Partitions | Implicit Index | No Duplicate Scoring |
|-------------|------------|----------------|----------------------|
| A1: Resjunk | Yes | Yes (column-based) | Yes (resjunk column) |
| A2: CTID Cache | Yes | Yes (column-based) | Yes (CTID+query cache) |
| A3: Rewrite | Yes | Yes (column-based) | Partial (same issues as B) |
| B: bm25_score() | Yes | Yes (column-based) | Yes (backend-local var) |
| C: Planner Hook | Yes | Yes (planner injects OID) | Yes (backend-local var) |
| **D: Recommended** | Yes | Yes (column-based via planner) | Yes (CTID cache) |

**Score caching approaches compared:**
- **A1 (Resjunk)**: Most robust, but requires custom scan node
- **A2 (CTID Cache)**: Simple, works with standard index AM, handles common case
- **B/C (Backend-local)**: Brittle if buffering occurs between scan and projection
- **ParadeDB (PlaceHolderVar)**: Most robust, but requires custom scan + complex C

**Recommended approach (Alternative D):** Start with CTID cache for simplicity.
Syntax is finalized; implementation can upgrade to PlaceHolderVar later if users
hit edge cases with complex JOINs.

---

## Alternative A: Column-Based Index Resolution

### Concept

Instead of referencing the index by name, reference the **indexed column**.
The system finds the appropriate BM25 index on that column automatically.

### Syntax

```sql
-- Index inferred from column reference
SELECT id, content <@> 'search terms' AS score
FROM documents
ORDER BY score;

-- Works with partitioned tables automatically
SELECT id, content <@> 'search terms' AS score
FROM partitioned_documents
ORDER BY score;

-- Explicit index for edge cases (e.g., expression indexes)
SELECT id, content <@> to_tpquery('specific_idx', 'search terms') AS score
FROM documents
ORDER BY score;
```

### The Duplicate Scoring Problem

With the above syntax, PostgreSQL evaluates `content <@> 'search terms'` twice:
1. **ORDER BY**: Index scan computes score via `amgettuple` → `xs_orderbyvals`
2. **SELECT**: Projection calls the `<@>` operator function separately

PostgreSQL does not perform common subexpression elimination between ORDER BY
and SELECT. The alias `AS score` only names the result; it doesn't prevent
re-evaluation.

### Implementation

#### Index Resolution (common to all sub-alternatives)

```c
typedef struct TpQuery
{
    int32 vl_len_;
    Oid   index_oid;        /* Resolved index OID (0 if not yet resolved) */
    int32 query_text_len;
    char  data[FLEXIBLE_ARRAY_MEMBER];  /* query text only */
} TpQuery;

Oid
tp_find_bm25_index_for_column(Oid relid, AttrNumber attnum)
{
    List *indexoidlist = RelationGetIndexList(relation);
    foreach(lc, indexoidlist)
    {
        Oid indexoid = lfirst_oid(lc);
        if (is_bm25_index(indexoid) && index_covers_column(indexoid, attnum))
            return indexoid;
    }
    return InvalidOid;
}
```

#### Partition Handling (common to all sub-alternatives)

- Index resolution happens at plan time using parent relation
- Each partition's local index inherits the same structure
- Executor uses partition-specific index OID from inheritance

---

### Sub-Alternative A1: Resjunk Column

Add a hidden resjunk column to carry the score from index scan to projection.

**Implementation:**
1. Planner hook detects `<@>` in SELECT matching ORDER BY
2. Adds resjunk target entry: `___bm25_score___`
3. Index scan stores score in resjunk slot
4. Modify operator to check for resjunk and return cached value

```c
/* In custom scan target list setup */
TargetEntry *score_tle = makeTargetEntry(
    (Expr *)makeVar(...),
    resno,
    "___bm25_score___",
    true);  /* resjunk = true */
```

**Challenges:**
- Requires custom scan node to inject resjunk column
- Operator function needs access to tuple slot to read resjunk
- Standard index AM doesn't support injecting extra columns

---

### Sub-Alternative A2: CTID-Based Score Cache

Cache the score in a backend-local hash table keyed by CTID + query hash.
The operator checks the cache before recomputing.

**Implementation:**

```c
typedef struct ScoreCacheEntry {
    ItemPointerData ctid;
    uint32          query_hash;
    float4          score;
} ScoreCacheEntry;

static ScoreCacheEntry score_cache;

/* Called by index scan after computing score */
void
tp_cache_score(ItemPointer ctid, uint32 query_hash, float4 score)
{
    ItemPointerCopy(ctid, &score_cache.ctid);
    score_cache.query_hash = query_hash;
    score_cache.score = score;
}

/* Called by operator function */
float4
tp_get_cached_score(ItemPointer ctid, uint32 query_hash, bool *found)
{
    if (ItemPointerEquals(ctid, &score_cache.ctid) &&
        query_hash == score_cache.query_hash)
    {
        *found = true;
        return score_cache.score;
    }
    *found = false;
    return 0;
}

/* In operator function */
Datum
text_tpquery_score(PG_FUNCTION_ARGS)
{
    ...
    bool found;
    float4 cached = tp_get_cached_score(&ctid, query_hash, &found);
    if (found)
        PG_RETURN_FLOAT4(cached);

    /* Cache miss - compute score */
    ...
}
```

**Advantages:**
- Works with standard index AM (no custom scan needed)
- Simple implementation
- Handles the common case where SELECT and ORDER BY match

**Limitations:**
- Only caches one score at a time (sufficient for row-by-row processing)
- Assumes projection happens immediately after tuple fetch
- May not work correctly with Sort/Materialize nodes between scan and projection

---

### Sub-Alternative A3: Query Rewriting to bm25_score()

Use a planner hook to rewrite `content <@> 'query'` in SELECT to `bm25_score()`
when it matches the ORDER BY expression.

**Before rewrite:**
```sql
SELECT id, content <@> 'search' AS score FROM docs ORDER BY score;
```

**After rewrite:**
```sql
SELECT id, bm25_score() AS score FROM docs
ORDER BY content <@> 'search';
```

**Challenges:**
- `bm25_score()` still needs a way to retrieve the cached score
- Same brittleness concerns as Alternative B (backend-local variable)
- Adds complexity without solving the fundamental score-passing problem

---

### Pros (Alternative A overall)

- Clean syntax: no index name clutter
- Natural partition support: column reference works across partitions
- Follows Postgres convention (GIN/GiST don't require index names)

### Cons (Alternative A overall)

- Ambiguous if multiple BM25 indexes exist on same column (mainly expression
  indexes - rare in practice)
- Requires planner hooks for index resolution
- Sub-alternatives have varying complexity/robustness tradeoffs

---

## Alternative B: Separate Score Retrieval Function

### Concept

Keep the `<@>` operator for ORDER BY, but add a separate `bm25_score()` function
to retrieve the score computed during the index scan. This eliminates the need
to repeat the operator in the SELECT clause and avoids duplicate scoring.

### Syntax

```sql
-- Index inferred, score retrieved via function
SELECT id, bm25_score() AS score
FROM documents
ORDER BY content <@> 'search terms';

-- Explicit index when needed (e.g., expression indexes)
SELECT id, bm25_score() AS score
FROM documents
ORDER BY content <@> to_tpquery('specific_idx', 'search terms');

-- Partitioned table works automatically
SELECT id, bm25_score() AS score
FROM partitioned_docs
ORDER BY content <@> 'search terms';
```

### Implementation

#### 1. Score Storage During Index Scan

The index scan already computes scores. Store them in a per-query context:

```c
/* In scan state */
typedef struct TpScanOpaqueData
{
    ...
    float4 current_score;  /* Score of current tuple */
} TpScanOpaqueData;

/* Set during amgettuple */
so->current_score = computed_score;
```

#### 2. bm25_score() Function

```c
Datum
tp_bm25_score(PG_FUNCTION_ARGS)
{
    /* Get score from current executor context */
    float4 score = tp_get_current_scan_score();
    if (score == INVALID_SCORE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_score() can only be used with BM25 index scan")));
    PG_RETURN_FLOAT4(-score);  /* Return positive score */
}
```

#### 3. Context Passing

Use a backend-local static variable to pass the current scan's score to the
`bm25_score()` function:

```c
/* Backend-local score context (one per Postgres process) */
static float4 current_bm25_score = INVALID_SCORE;

void tp_set_current_score(float4 score)
{
    current_bm25_score = score;
}

float4 tp_get_current_scan_score(void)
{
    return current_bm25_score;
}
```

### Pros

- Simple syntax: no operator repetition in SELECT
- Single scoring path: score computed once in index scan
- Clear separation: `<@>` for ordering, `bm25_score()` for retrieval
- Easy to implement incrementally

### Cons

- `bm25_score()` only works within index scan context (not in subqueries, CTEs)
- Requires careful context management for nested queries
- Less flexible than ParadeDB's PlaceHolderVar approach for JOINs

---

## Alternative C: OID-Based Query Type with Planner Integration

### Concept

Store index OID (not name) in the query type. Use planner hooks to resolve
index from context and inject OID before execution. Combine with `bm25_score()`
for score retrieval.

### Syntax

```sql
-- Simple syntax: index resolved from context
SELECT id, bm25_score() AS score
FROM documents
ORDER BY content <@> 'search terms';

-- Explicit index syntax (for expression indexes)
SELECT id, bm25_score() AS score
FROM documents
ORDER BY content <@> to_tpquery('specific_idx', 'search terms');
```

### Implementation

#### 1. Modified Query Type

```c
typedef struct TpQuery
{
    int32 vl_len_;
    Oid   index_oid;        /* 0 = unresolved, needs context */
    int32 query_text_len;
    char  data[FLEXIBLE_ARRAY_MEMBER];
} TpQuery;
```

#### 2. Planner Hook for Index Resolution

```c
static PlannedStmt *
tp_planner_hook(Query *parse, const char *query_string,
                int cursorOptions, ParamListInfo boundParams)
{
    /* Walk query tree looking for <@> operators with unresolved index */
    tp_resolve_indexes_in_query(parse);

    /* Call standard planner */
    return standard_planner(parse, query_string, cursorOptions, boundParams);
}

static void
tp_resolve_indexes_in_query(Query *parse)
{
    /* For each RTE (range table entry) */
    foreach(lc, parse->rtable)
    {
        RangeTblEntry *rte = lfirst(lc);
        if (rte->relkind == RELKIND_RELATION ||
            rte->relkind == RELKIND_PARTITIONED_TABLE)
        {
            Oid bm25_index = tp_find_bm25_index(rte->relid);
            /* Inject into any tpquery constants in ORDER BY */
            tp_inject_index_oid(parse, rte->relid, bm25_index);
        }
    }
}
```

#### 3. Partition-Aware Index Mapping

```c
Oid
tp_get_partition_index(Oid parent_index, Oid partition_relid)
{
    /* Get index info from parent */
    HeapTuple parent_idx_tup = SearchSysCache1(INDEXRELID, parent_index);
    Form_pg_index parent_idx = (Form_pg_index)GETSTRUCT(parent_idx_tup);

    /* Find equivalent index on partition */
    List *part_indexes = RelationGetIndexList(partition_rel);
    foreach(lc, part_indexes)
    {
        Oid part_idx = lfirst_oid(lc);
        if (indexes_are_equivalent(parent_index, part_idx))
            return part_idx;
    }
    return InvalidOid;
}
```

#### 4. Score Passing via xs_orderbyvals

Leverage existing ORDER BY infrastructure:

```c
/* In amgettuple - already implemented */
scan->xs_orderbyvals[0] = Float4GetDatum(-score);
scan->xs_orderbynulls[0] = false;
scan->xs_recheckorderby = false;  /* Trust the score */
```

For SELECT clause access, add a `bm25_score()` function that retrieves
from current scan context:

```c
Datum
tp_bm25_score(PG_FUNCTION_ARGS)
{
    /* Get score from current index scan context */
    float4 score = tp_get_current_scan_score();
    if (score == INVALID_SCORE)
        ereport(ERROR, "bm25_score() only valid in BM25 index scan context");
    PG_RETURN_FLOAT4(score);
}
```

### Pros

- OID storage more efficient than name strings
- Reuses existing ORDER BY score mechanism
- Natural partition support via index equivalence
- Planner hook enables implicit index resolution

### Cons

- Planner hook adds implementation complexity
- `bm25_score()` function has limited applicability (only in index scan context)
- Complex queries (JOINs, subqueries) may need additional work

---

## Alternative D: Recommended Approach

Combines the best elements from A and C:

1. **Index Resolution**: Column-based resolution (from A) via planner hook
   (from C)

2. **Query Type**: Store OID in tpquery (from C), allow unresolved state that
   planner fills in

3. **Score Caching**: CTID-based cache (from A2) avoids duplicate scoring

4. **Partition Support**: Planner resolves parent index, executor maps to
   partition index

### Syntax (Finalized)

```sql
-- Standard usage: index inferred from column
SELECT id, content <@> 'search terms' AS score
FROM documents
ORDER BY score;

-- Explicit index when needed (e.g., expression indexes)
SELECT id, content <@> to_tpquery('specific_idx', 'search terms') AS score
FROM documents
ORDER BY score;

-- Partitioned tables work automatically
SELECT id, content <@> 'search terms' AS score
FROM partitioned_docs
ORDER BY score;
```

The `<@>` operator appears once in SELECT. The ORDER BY references the alias.
The operator is evaluated twice by PostgreSQL, but the second evaluation hits
the CTID cache and returns immediately.

### Implementation Strategy

The syntax above is finalized and stable. The underlying implementation can
evolve without changing user-facing behavior:

**Initial Implementation: CTID Cache**
- Simple backend-local cache keyed by CTID + query hash
- Works with standard index AM (no custom scan needed)
- Covers the common case: simple queries with ORDER BY and SELECT

**Known Limitations:**
- Complex JOINs with BM25 scores may see re-scoring if a Sort/Materialize
  node separates the scan from the projection
- CTEs and subqueries that reference scores outside their immediate context
- These edge cases will compute scores twice (correct, but slower)

**Future Option: PlaceHolderVar (if needed)**
- If users hit limitations with CTID cache, we can upgrade to PlaceHolderVar
- Requires implementing a custom scan node
- More complex but handles all edge cases correctly
- No syntax changes required - same SQL, better execution

---

## Implementation Phases for Alternative D

### Phase 1: OID Storage
- Change tpquery to store index OID instead of name string
- Add resolution function to convert name -> OID at parse time
- Maintain backward compatibility with name-based input

### Phase 2: Implicit Resolution
- Add planner hook for index resolution
- Implement column-to-index mapping
- Handle unresolved tpquery by inferring from context

### Phase 3: CTID Score Cache
- Add CTID + query hash based score cache
- Index scan calls `tp_cache_score()` after computing score
- Operator function calls `tp_get_cached_score()` before recomputing
- Cache hit returns immediately; cache miss computes and caches

### Phase 4: Partition Support
- Add partition detection in planner
- Implement parent-to-partition index mapping
- Test with various partition schemes (range, list, hash)

### Future Phase: PlaceHolderVar (Optional)

If users report performance issues with complex JOINs or subqueries due to
re-scoring, consider upgrading to the PlaceHolderVar approach:

1. **Custom Scan Node**: Implement a custom scan provider that intercepts
   queries with `<@>` in ORDER BY
2. **Support Function**: Register a planner support function for the `<@>`
   operator that wraps scores in PlaceHolderVar
3. **Const Injection**: During execution, store computed scores in Const nodes
   within the PlaceHolderVar structure
4. **Testing**: Verify scores survive Sort, Materialize, Hash Join, etc.

This is a significant undertaking (see Appendix for ParadeDB implementation
details) but provides the most robust score preservation. The key point is
that **no syntax changes are needed** - the same SQL works with either
implementation.

---

## Open Questions

1. **Expression indexes**: The main case for multiple BM25 indexes on the same
   column is expression indexes (e.g., `CREATE INDEX ON t USING bm25 (lower(content))`).
   Should we require explicit index specification only for this case?

2. **Cross-table queries**: How to handle JOINs where both tables have BM25
   indexes? Each ORDER BY clause references a specific column, so this may
   resolve naturally.

3. **Subqueries and CTEs**: Index resolution should work via planner hook.
   However, CTID cache may not work correctly if score is referenced outside
   the immediate scan context. Need testing to identify limitations.

4. **Prepared statements**: How does index resolution interact with plan
   caching? If index OID is injected at plan time, this should work correctly.

5. **Boolean filtering with ranked search**: See
   [DESIGN_FEATURE_GAPS.md](DESIGN_FEATURE_GAPS.md) for analysis of this common
   use case and how ParadeDB handles it. May require future syntax changes.

---

## Appendix: Reference Implementation Details

### ParadeDB PlaceHolderVar Mechanism

ParadeDB uses PostgreSQL's PlaceHolderVar mechanism to preserve scores across
JOINs and other plan nodes. This is more robust than backend-local variables.

**What is PlaceHolderVar?**

A PlaceHolderVar is a planner-level node that forces the optimizer to preserve
an expression through JOIN processing. Without it, expressions from a scan node
can be eliminated when placed under a JOIN.

**How ParadeDB implements it:**

1. **Support function registration**: The `pdb.score()` function registers a
   planner support function:
   ```sql
   ALTER FUNCTION pdb.score SUPPORT paradedb.placeholder_support;
   ```

2. **Support function wraps in PlaceHolderVar**: When the planner encounters
   `pdb.score()`, it calls the support function, which returns a PlaceHolderVar
   wrapping the score expression:
   ```rust
   let phv = make_placeholder_expr(
       (*srs).root,
       pg_sys::copyObjectImpl((*srs).fcall.cast()).cast(),
       phrels,
   );
   ```

3. **Const node injection**: During execution, ParadeDB's custom scan stores
   the computed score in a Const node that's part of the PlaceHolderVar:
   ```rust
   (*const_score_node).constvalue = score.into_datum().unwrap();
   ```

4. **Projection evaluates PlaceHolderVar**: When PostgreSQL projects the result,
   it evaluates the PlaceHolderVar, which now contains the Const with the score.

**Why this is robust:**

- Score is embedded in the plan structure, not a global variable
- Works across JOINs, subqueries, and CTEs
- Survives Sort/Materialize nodes between scan and projection

**Why it's hard for pg_textsearch:**

- Requires custom scan node (ParadeDB has one; we use standard index AM)
- Complex C implementation of support functions and Const injection
- Need to intercept execution at the right point to update the Const

**Implications for our approach:**

We start with CTID cache (simpler, works for common cases). If users report
issues with complex JOINs or subqueries causing re-scoring, PlaceHolderVar
becomes the upgrade path. The syntax remains unchanged - only the internal
implementation would change.

**Execution flow:**

```
Planning:
  pdb.score(t) in SELECT
       ↓
  placeholder_support() called
       ↓
  Returns PlaceHolderVar(pdb.score(t))
       ↓
  Planner preserves PHV through JOINs

Execution:
  CustomScan computes BM25 score = 42.5
       ↓
  Stores in Const node: constvalue = 42.5
       ↓
  Projection evaluates PHV → returns 42.5
```

### ParadeDB Code Reference

PlaceHolderVar wrapping (from pg_search/src/postgres/customscan/pdbscan/projections/mod.rs):

```rust
let phv = make_placeholder_expr(
    (*srs).root,
    pg_sys::copyObjectImpl((*srs).fcall.cast()).cast(),
    phrels,
);
```

This ensures Postgres optimizer doesn't lose score columns when they pass
through Hash Join, Merge Join, etc.

### VectorChord-BM25 Composite Type

VectorChord stores index OID directly in the query type as a regclass:

```sql
CREATE TYPE bm25query AS (
    index_oid regclass,
    query_vector bm25vector
);
```

This avoids name resolution at query time but requires explicit index
specification always.

### Postgres Index Inheritance

For partitioned tables, Postgres creates partition-local indexes that mirror
the parent's definition. We can leverage `pg_inherits` and `pg_index` catalogs
to map between them:

```sql
SELECT c.oid AS partition_index
FROM pg_inherits i
JOIN pg_index pi ON pi.indrelid = i.inhrelid
JOIN pg_class c ON c.oid = pi.indexrelid
WHERE i.inhparent = parent_table_oid
  AND index_is_equivalent(c.oid, parent_index_oid);
```
