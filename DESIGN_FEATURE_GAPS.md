# Feature Gap Analysis: pg_textsearch vs ParadeDB

This document analyzes feature gaps between pg_textsearch and ParadeDB/Tantivy,
with a focus on what's needed for hybrid search use cases.

Related: [DESIGN_INDEX_ERGONOMICS.md](DESIGN_INDEX_ERGONOMICS.md) covers syntax
and partitioned table support.

---

## Critical Feature Gap: Boolean Filtering with Ranked Search

### The Problem

The most common full-text search use case is: "find the most relevant documents
matching 'search terms' WHERE category = 'X' AND date > '2024-01-01'". This
combines BM25 ranking with Boolean filtering on other columns.

pg_textsearch currently has no clean way to express this. Users would need to:
```sql
-- Awkward: filter after ranking (inefficient, wrong semantics)
SELECT * FROM (
    SELECT id, bm25_score() AS score
    FROM documents
    ORDER BY content <@> 'search terms'
    LIMIT 100
) sub
WHERE category = 'electronics';
```

This is wrong because it ranks first, then filters - potentially missing
relevant documents that match the filter but weren't in the top 100.

### How ParadeDB Handles This

ParadeDB uses a two-stage approach with their `@@@` operator:

**Stage 1: Indexed search** - The `@@@` operator triggers a custom scan node
that searches the Tantivy index:
```sql
SELECT * FROM products WHERE products @@@ 'description:keyboard';
```

**Stage 2: Heap filtering** - Additional WHERE conditions are evaluated against
heap tuples after the index scan:
```sql
SELECT * FROM products
WHERE products @@@ 'keyboard'    -- indexed search
  AND price > 50                  -- heap filter
  AND in_stock = true;            -- heap filter
```

ParadeDB's custom scan intercepts the query, runs the indexed search, then
evaluates remaining WHERE predicates against the matching tuples. This is
efficient because only matching documents are fetched from heap.

### Design Considerations for pg_textsearch

**Option 1: Custom Scan Node (ParadeDB approach)**
- Implement a custom scan provider that intercepts queries with `<@>`
- Evaluate WHERE predicates against index scan results
- Requires significant planner/executor integration
- Most flexible and efficient long-term

**Option 2: Index-Only Filtering**
- Extend the BM25 index to store filterable columns
- Query syntax specifies both search and filter in ORDER BY
- Similar to how some search engines handle facets
- Limited to columns included in index

**Option 3: Two-Pass with Bitmap**
- First pass: BM25 index returns all matching TIDs
- Second pass: Apply WHERE filters, then rank survivors
- Less efficient but simpler to implement
- May work acceptably for moderate result sets

### Syntax Implications

If we adopt a custom scan approach, the syntax might change:
```sql
-- Possible future syntax with custom scan
SELECT id, bm25_score() AS score
FROM documents
WHERE content @@ 'search terms'   -- triggers custom scan
  AND category = 'electronics'     -- evaluated by custom scan
ORDER BY score;
```

This would be a **breaking change** from the current ORDER BY-based approach.
However, it aligns with how users expect WHERE clauses to work and matches
ParadeDB's proven model.

### Recommendation

Defer this decision until after the ergonomics changes are implemented. The
current ORDER BY-based approach can coexist with a future WHERE-based custom
scan. Priority order:
1. Implement Alternative D from DESIGN_INDEX_ERGONOMICS.md
2. Evaluate custom scan feasibility
3. Design Boolean filter syntax (may require new operator like `@@`)

---

## Feature Gap Analysis

### Context: Hybrid Search Focus

pg_textsearch is not intended to be a standalone full-text search engine
competing with Tantivy or Elasticsearch. Its primary use case is **hybrid
search** in combination with pgvector or pgvectorscale, where:

- **Semantic search** (vector similarity) handles conceptual matching, typo
  tolerance, and synonym expansion
- **Lexical search** (BM25) handles exact keyword matching, rare terms, and
  provides a complementary signal for ranking

This changes the priority calculus. Features like fuzzy matching become less
important because:
- The semantic side of hybrid search already handles typos and variations
- AI agents (a major target use case) do not make typos like humans do

### Feature Comparison

| Feature | ParadeDB | pg_textsearch | Notes |
|---------|----------|---------------|-------|
| Boolean queries (AND/OR/NOT) | Yes | No | Useful for filtering |
| Phrase queries | Yes | No | Nice to have |
| Boolean filter + ranking | Yes | No | Common use case |
| Range queries (time axis) | Yes | No | Synergy with TimescaleDB |
| Fuzzy matching | Yes | No | Semantic search covers this |
| Field-specific search | Yes | No | Multi-column indexes |
| Highlighting/snippets | Yes | No | UX feature |
| Regex patterns | Yes | No | Niche use case |

### Priorities for pg_textsearch

**Near-term (for hybrid search users):**
1. Boolean query support (AND/OR/NOT) - enables filtering within text search
2. Boolean filtering with ranked search - the common "search + filter" pattern
3. Time-range queries - natural fit with TimescaleDB for time-series + search

**Deferred:**
- Phrase queries - useful but not blocking for most hybrid search use cases
- Fuzzy matching - semantic search handles this better
- Highlighting - UX polish, not core functionality
- Regex/proximity/advanced query syntax - niche requirements

### Non-Goals

We are not pursuing feature parity with Tantivy or ParadeDB. The goal is to
provide a solid BM25 implementation that satisfies 90% of hybrid search use
cases with clean ergonomics and good performance.

---

## Appendix: ParadeDB Query DSL Reference

ParadeDB exposes Tantivy's query capabilities through a rich function-based API.
This is provided for reference, not as a target for implementation.

### Boolean Combinators
```sql
paradedb.boolean(
  must => ARRAY[query1, query2],
  should => ARRAY[query3],
  must_not => ARRAY[query4]
)
```

### Text Search
```sql
paradedb.parse('field:value')
paradedb.match(value => 'term', distance => 1)  -- fuzzy
paradedb.phrase(phrases => ARRAY['exact', 'phrase'], slop => 0)
```

### Range Queries
```sql
paradedb.range(field => 'price', range => '[50,150]'::numrange)
paradedb.range(field => 'date', range => '[2024-01-01,2024-12-31]'::daterange)
```

### Scoring Modifiers
```sql
paradedb.boost(query => base_query, factor => 2.5)
paradedb.const_score(query => filter_query, score => 1.0)
```
