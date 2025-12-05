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

## Alternatives Considered

### Alternative A: Column-Based Index Resolution

Infer the BM25 index from the column reference rather than requiring explicit
index names. Explored three sub-approaches for avoiding duplicate scoring:

- **A1 (Resjunk)**: Add a hidden "resjunk" target entry to carry the score from
  index scan to projection. Resjunk columns are PostgreSQL's mechanism for
  passing data through plan nodes without exposing it in results (used
  internally for ORDER BY, UPDATE targets, etc.). Most robust approach, but
  requires a custom scan node to inject the resjunk column - standard index AM
  can't add extra columns to the scan output.
- **A2 (CTID Cache)**: Cache score keyed by CTID + query hash. Simple, works
  with standard index AM. **Adopted in Alternative D.**
- **A3 (Query Rewrite)**: Rewrite SELECT to use `bm25_score()`. Doesn't solve
  the fundamental score-passing problem.

### Alternative B: Separate bm25_score() Function

Use `bm25_score()` in SELECT instead of repeating the `<@>` operator:
```sql
SELECT id, bm25_score() FROM docs ORDER BY content <@> 'search';
```

**Why not chosen**: Backend-local variable for passing scores is brittle if
Sort/Materialize nodes appear between scan and projection. Also creates two
different syntaxes for accessing scores.

### Alternative C: OID-Based Query with Planner Hook

Store index OID in tpquery and use planner hooks to resolve index from context.
Similar to B but with OID storage instead of name strings.

**Why not chosen**: Same brittleness issues as B for score passing. The OID
storage and planner hook ideas were incorporated into Alternative D.

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

### Key Implementation Details

**CTID Score Cache:**
```c
typedef struct ScoreCacheEntry {
    ItemPointerData ctid;
    uint32          query_hash;
    float4          score;
} ScoreCacheEntry;

static ScoreCacheEntry score_cache;

/* Called by index scan after computing score */
void tp_cache_score(ItemPointer ctid, uint32 query_hash, float4 score);

/* Called by operator - returns cached score or computes fresh */
float4 tp_get_cached_score(ItemPointer ctid, uint32 query_hash, bool *found);
```

**Index Resolution:**
```c
Oid tp_find_bm25_index_for_column(Oid relid, AttrNumber attnum)
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
