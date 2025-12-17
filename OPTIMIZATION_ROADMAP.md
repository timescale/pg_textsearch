# pg_textsearch: Storage and Query Optimization Design

**Status**: Draft for review
**Author**: Todd J. Green @ Tiger Data
**Last updated**: 2025-12-16

---

## Problem Statement

pg_textsearch currently has $O(n)$ query complexity for top-$k$ retrieval, where
$n$ = number of matching documents. For a query matching $10^5$ documents but
requesting only $10$ results, we:

1. Score all $10^5$ documents
2. Sort all $10^5$ scores
3. Return top $10$

This becomes a major bottleneck as index size grows. Production search engines
(Lucene, Tantivy, Elasticsearch) solve this with Block-Max WAND, which skips
blocks that cannot contribute to the top-$k$ results. See "Why Block-Max WAND?"
below for empirical performance data.

### Goals

1. **Primary**: Achieve sub-linear query time for top-$k$ retrieval
2. **Secondary**: Reduce storage footprint by 50%+ via compression

### Non-Goals

- **Faceted search**: Aggregations, facet counts, and drill-down filtering are
  immediately expressible in SQL; we just haven't prioritized optimizing them.
- **Query rewriting**: Spell correction, query expansion, and synonym handling
  belong in the application layer or Postgres text search configurations.
- **Result highlighting**: Snippet generation and term highlighting are
  separate concerns from scoring and retrieval.

---

## Current Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Query Flow                         │
├─────────────────────────────────────────────────────────┤
│  1. Parse query terms                                   │
│  2. For each term: linear scan of posting list          │
│  3. Accumulate scores in hash table (ALL matching docs) │
│  4. qsort() all scores                                  │
│  5. Return top $k$                                      │
└─────────────────────────────────────────────────────────┘
```

**Posting list format** (segments):
```c
typedef struct TpSegmentPosting {
    ItemPointerData ctid;    // 6 bytes - heap location
    uint16 frequency;        // 2 bytes - term frequency
    uint16 doc_length;       // 2 bytes - document length
} TpSegmentPosting;          // 10 bytes total, uncompressed
```

**Key limitations**:
- No skip pointers: Must scan entire posting list sequentially
- No block structure: Can't skip non-competitive regions
- No compression: 10 bytes per posting regardless of content
- No early termination: Must process all matches before knowing top-$k$

---

## Proposed Solution: Block-Max WAND

### Why Block-Max WAND?

Block-Max WAND (BMW) is the industry standard for top-$k$ retrieval:
- **Lucene**: `BlockMaxConjunctionScorer` (since Lucene 8)
- **Tantivy**: `block_wand.rs`
- **Elasticsearch/OpenSearch**: Built on Lucene's implementation

The algorithm maintains a score threshold (the $k$-th best score seen so far)
and skips any block where the maximum possible score is below the threshold.

**Complexity**: Naive exhaustive evaluation is $O(mn)$ where $m$ = query terms,
$n$ = matching docs. BMW achieves sub-linear time in practice by skipping
blocks whose max scores fall below the evolving threshold. Formal complexity
bounds remain an open question (Ding & Suel [1], Section 8: "It would also be
interesting to provide some analysis, say of the optimal number of pointer
movements and document evaluations"). Per-iteration overhead is $O(m \log m)$
for sorting scorers, reducible to $O(m)$ with a heap.

**Empirical results** from Ding & Suel [1] on TREC GOV2 (25.2M documents):
- Exhaustive OR: 3.8M docs evaluated, 225ms/query
- WAND: 178K docs evaluated (4.7%), 77ms/query (2.9x faster)
- BMW: 22K docs evaluated (0.6%), 28ms/query (8x faster than exhaustive)
- With docID reassignment: BMW achieves 8.9ms/query (25x faster)

### Algorithm Overview

```
Input: Query terms T[], result count $k$
Output: Top $k$ documents by BM25 score

threshold = 0
heap = MinHeap(capacity=k)

while any term has remaining postings:
    1. Sort term iterators by current doc_id
    2. Find "pivot" - first position where cumulative block_max >= threshold
    3. If no pivot exists: DONE (no remaining doc can beat threshold)
    4. Advance all iterators before pivot to pivot_doc
    5. If all iterators land on pivot_doc:
         score = compute_bm25(pivot_doc)
         if score > threshold:
             heap.push(pivot_doc, score)
             if heap.size > k:
                 threshold = heap.pop_min()
    6. Advance past pivot_doc
```

### Required Data Structures

To enable BMW, we need:

1. **Block-aligned posting storage** (128 docs per block)
2. **Per-block max score metadata** (max_tf, fieldnorm_id)
3. **Skip pointers** for $O(\log n)$ block seeking
   - Why not a tree? See Zobel & Moffat [6] for analysis of skip pointer
     trade-offs in inverted indexes.

---

## Detailed Design

### Foundational Structures

#### Document IDs

Postgres identifies heap tuples by CTID (`ItemPointerData`, 6 bytes: block number +
offset). For efficient posting list storage and BMW, we use **segment-local
document IDs** (u32):

- Each segment assigns sequential doc IDs (0, 1, 2, ...) to documents as they're
  added
- A separate **doc ID → CTID mapping** table in the segment enables heap lookup
- Posting lists store the compact u32 doc IDs, not 6-byte CTIDs
- Delta encoding of sorted doc IDs yields small values (often 1-10 bits each)

This is standard practice in Lucene/Tantivy. The mapping table adds ~4 bytes per
document but enables much better posting list compression.

**Note**: The BMW paper shows that "docID reassignment" (reordering documents so
similar ones have adjacent IDs) can improve performance by 3x. This is a future
optimization; for now, doc IDs are assigned in insertion order.

#### Term Dictionary

At query time, we need to map query terms to their posting lists. Profiling
shows term lookup is a significant cost, primarily due to Postgres buffer
manager overhead (pin/unpin, reference counting) combined with binary search.

**How Lucene/Tantivy solve this**: They mmap index files directly, bypassing
any buffer manager. They also use FSTs (Finite State Transducers) which are
compact and support prefix/range queries.

However:
- We cannot use auxiliary files outside Postgres (breaks transactional
  consistency)
- We don't need prefix or range queries over terms - just point lookups
- FST lookup is $O(|term|)$ character-by-character traversal (branchy), while
  hash tables are $O(1)$

**Alternative: On-disk hash table**

Since segments are immutable, we know the exact term count at creation time.
This enables a statically-sized hash table stored in Postgres pages:

| Approach | Page accesses | Buffer mgr calls | Shared memory |
|----------|---------------|------------------|---------------|
| Sorted array + binary search | $O(\log t)$ | $O(\log t)$ | None |
| On-disk hash table | 1-2 | 1-2 | None |
| Sorted array + shmem cache | 1 (cache miss) | 1 (cache miss) | Hash table |

**On-disk hash trade-offs**:
- Pro: No shared memory management complexity
- Pro: Works naturally with Postgres buffer manager and page cache
- Pro: Postgres has hash index infrastructure we could study/reuse
- Con: Still 1-2 buffer manager calls per lookup (pin/unpin overhead)
- Con: Hash collisions may cause extra page accesses
- Con: Larger on-disk footprint than sorted array (~50% load factor typical)

**Recommendation**: Use an on-disk hash table. It's simple, works naturally with
Postgres infrastructure, and reduces lookups from $O(\log t)$ to 1-2 page
accesses.

```
On-disk Hash Table Layout (linear probing, no indirection):

Page 0: Header
┌─────────────────────────────────────────┐
│ slot_count: u32    (entries / 0.75 load factor) │
│ entry_count: u32                               │
│ string_pool_start: u32 (page number)           │
└─────────────────────────────────────────┘

Pages 1..S: Slot array (dense, fixed-size entries)
┌─────────────────────────────────────────┐
│ Slot 0: term_hash, term_offset, term_len,     │
│         doc_freq, posting_offset, block_count │
│ Slot 1: ...                                   │
│ ...                                           │
│ (empty slots: term_offset = INVALID)          │
└─────────────────────────────────────────┘

Pages S+1..N: String pool (variable-length term bytes)
```

**Lookup**: hash(term) → slot index → page (slot / slots_per_page) →
check term_hash, compare term bytes from string pool. On collision,
linear probe to next slot.

**Why no bucket directory**: With fixed-size slots and linear probing,
slot location is computed directly: `page = 1 + (slot / slots_per_page)`.
No indirection needed. This is simpler and typically requires just 1-2
page accesses (one for slot, possibly one for string pool to verify term).


---

### Phase 1: Block Storage Foundation

#### 1.1 Block Structure

```
┌────────────────────────────────────────────────────────┐
│ Block Header (16 bytes)                                │
├────────────────────────────────────────────────────────┤
│ last_doc_id: u32     - Last doc ID in block            │
│ doc_count: u8        - Number of docs (1-128)          │
│ block_max_tf: u16    - Max term frequency in block     │
│ block_max_norm: u8   - Fieldnorm ID of max-scoring doc │
│ posting_offset: u32  - Byte offset to posting data     │
│ flags: u8            - Compression type, etc.          │
│ reserved: 3 bytes    - Future use                      │
├────────────────────────────────────────────────────────┤
│ Posting Data (variable size)                           │
│ [doc_id deltas...][frequencies...][doc_lengths...]     │
└────────────────────────────────────────────────────────┘
```

**Block size choice**: 128 documents
- Tantivy uses 128, Lucene uses 256
- Smaller = more blocks = finer-grained skipping
- Larger = better compression ratio, less metadata overhead
- 128 is a good balance; revisit after benchmarking

**Open question**: Should we make block size configurable per index?

#### 1.2 Segment Layout

```
┌─────────────────────────────────────────────────────────┐
│ Segment Header                                          │
├─────────────────────────────────────────────────────────┤
│ Term Dictionary (sorted array)                          │
│   - term -> (posting_offset, doc_freq, block_count)     │
├─────────────────────────────────────────────────────────┤
│ Doc ID → CTID Mapping (6 bytes per doc)                 │
│   - Maps segment-local doc IDs back to heap tuples      │
├─────────────────────────────────────────────────────────┤
│ Skip Index (dense array of block headers)               │
│   - Enables binary search over blocks                   │
│   - Separate from posting data for cache efficiency     │
├─────────────────────────────────────────────────────────┤
│ Posting Blocks                                          │
│   - Block-aligned, potentially compressed               │
├─────────────────────────────────────────────────────────┤
│ Fieldnorm Table (1 byte per doc)                        │
│   - Quantized document lengths for BM25                 │
│   - Shared across all terms                             │
└─────────────────────────────────────────────────────────┘
```

**Key insight**: Separating skip index from posting data improves cache
behavior. During BMW, we frequently access skip entries but rarely decompress
blocks.

#### 1.3 Fieldnorm Quantization

Lucene/Tantivy quantize document lengths to 1 byte (256 buckets) using a
log-scale mapping. This:
- Reduces storage (1 byte vs 2-4 bytes per doc per term)
- Enables compact block-max metadata
- Has negligible impact on ranking quality

We might as well use Lucene's quantization scheme (SmallFloat.intToByte4). Both
Lucene and Tantivy independently converged on this same approach, which is
reasonable evidence for its effectiveness.

Key properties:
- Document lengths 0-39 stored **exactly** (covers most short documents)
- Larger lengths use 4-bit mantissa encoding (~6% relative error max)
- 256 buckets cover lengths from 0 to 2+ billion

Why quantization doesn't hurt BM25 ranking:
- BM25 uses the **ratio** $dl/avgdl$, not absolute length
- Small errors in $dl$ become smaller errors in the ratio
- The $b$ parameter (0.75) further dampens length's influence
- Other factors (IDF, term frequency) dominate scoring

```c
// Precomputed decode table (Lucene SmallFloat.byte4ToInt)
static const uint32 FIELDNORM_TABLE[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 42, 44, 46, 48, 50, 52, 54,
    // ... continues with increasing gaps
};

uint8 encode_fieldnorm(uint32 length);   // See SmallFloat.intToByte4
uint32 decode_fieldnorm(uint8 norm_id) {
    return FIELDNORM_TABLE[norm_id];
}
```

---
### Phase 2: Block-Max WAND Query Executor

#### 2.1 Term Scorer Interface

```c
typedef struct TpTermScorer {
    /* Iterator state */
    uint32          current_doc;
    uint32          current_block;
    bool            exhausted;

    /* Block-level state */
    TpSkipEntry    *skip_entries;
    uint32          num_blocks;
    float           block_max_score;    // Cached for current block

    /* Scoring parameters */
    float           idf;                // Precomputed at query start
    float           weight;             // Query term weight

    /* Methods */
    bool (*advance)(TpTermScorer *self, uint32 target);
    bool (*advance_block)(TpTermScorer *self, uint32 target_block);
    float (*score)(TpTermScorer *self);
    float (*max_score)(TpTermScorer *self);  // Block max
} TpTermScorer;
```

#### 2.2 BMW Executor

```c
typedef struct TpBMWExecutor {
    TpTermScorer  **scorers;
    int             num_scorers;
    float           threshold;
    TpMinHeap      *top_k;
    int             k;
} TpBMWExecutor;

void tp_bmw_execute(TpBMWExecutor *exec) {
    exec->threshold = 0;

    while (!all_exhausted(exec)) {
        // Sort by current doc (cheapest scorer first for ties)
        sort_scorers_by_doc(exec);

        // Find pivot where cumulative max_score >= threshold
        int pivot = find_pivot(exec);
        if (pivot < 0) break;

        uint32 pivot_doc = exec->scorers[pivot]->current_doc;

        // Advance pre-pivot scorers to pivot_doc
        bool aligned = true;
        for (int i = 0; i < pivot; i++) {
            if (!exec->scorers[i]->advance(exec->scorers[i], pivot_doc)) {
                aligned = false;
                break;
            }
            if (exec->scorers[i]->current_doc != pivot_doc) {
                aligned = false;
            }
        }

        if (aligned) {
            // All scorers on pivot_doc - compute actual score
            float score = 0;
            for (int i = 0; i <= pivot; i++) {
                score += exec->scorers[i]->score(exec->scorers[i]);
            }

            if (score > exec->threshold) {
                heap_insert(exec->top_k, pivot_doc, score);
                if (heap_size(exec->top_k) > exec->k) {
                    exec->threshold = heap_pop_min(exec->top_k);
                }
            }
        }

        // Advance past pivot
        advance_min_scorer(exec);
    }
}
```

#### 2.3 Single-Term Fast Path

Single-term queries are common and ~3x faster because they skip the multi-term
overhead: no sorting scorers, no pivot computation, no alignment checks. Just
a tight loop over blocks:

```c
void tp_bmw_single_term(TpTermScorer *scorer, int k, TpMinHeap *results) {
    float threshold = 0;

    while (!scorer->exhausted) {
        // Skip blocks below threshold
        while (scorer->block_max_score < threshold) {
            if (!scorer->advance_block(scorer, scorer->current_block + 1))
                return;
        }

        // Process current block
        while (in_current_block(scorer)) {
            float score = scorer->score(scorer);
            if (score > threshold) {
                heap_insert(results, scorer->current_doc, score);
                if (heap_size(results) >= k) {
                    threshold = heap_min_score(results);
                }
            }
            scorer->advance(scorer, scorer->current_doc + 1);
        }
    }
}
```

---

### Phase 3: Posting List Compression

#### 3.1 Compression Strategy

| Data | Encoding | Rationale |
|------|----------|-----------|
| Doc IDs | Delta + FOR/PFOR | Sorted, small deltas |
| Frequencies | FOR/PFOR | Usually small (1-10) |
| Last block | VInt | < 128 docs, not worth FOR |

#### 3.2 FOR (Frame of Reference)

```c
void for_encode(uint32 *values, int count, uint8 *out, int *out_len) {
    // Find minimum bits needed
    uint32 max_val = 0;
    for (int i = 0; i < count; i++)
        if (values[i] > max_val) max_val = values[i];

    uint8 bits = 32 - __builtin_clz(max_val | 1);

    // Pack values
    *out++ = bits;
    for (int i = 0; i < count; i++) {
        write_bits(out, values[i], bits);
    }
}
```

**Expected compression**: Compression ratio depends heavily on posting list
density. For web-scale collections, Zobel & Moffat [6] report 8-10 bits per
doc ID with variable-byte encoding; FOR can achieve 4-6 bits for dense lists.

#### 3.3 PFOR for Outliers

When a few values don't fit the common bit width:
1. Encode all values at reduced bit width
2. Store exceptions: (index, high_bits) pairs
3. Patch during decode

Tantivy uses this; Lucene uses a variant. See Lemire et al. [5] for detailed
performance analysis of PFOR vs alternatives.

---

### Phase 4: Additional Optimizations

#### 4.1 Roaring Bitmaps

**Use cases** (not for posting lists):

| Component | Format | Why |
|-----------|--------|-----|
| Posting lists | FOR/PFOR blocks | Need frequencies, block-max |
| Deleted docs | Roaring bitmap | Set membership only |
| Filter cache | Roaring bitmap | Fast intersection |
| Segment merge | Roaring bitmap | Doc ID remapping |

Roaring excels at set operations but doesn't store term frequencies, which we
need for BM25 scoring.

#### 4.2 Segment-Level Pruning

Store per-segment max scores in segment header:
```c
if (segment_max_score_for_query < threshold)
    skip_entire_segment();
```

Requires computing segment-level term statistics at compaction time.

#### 4.3 Multi-Level Skip Lists

For very long posting lists ($10^5$+ docs), single-level skip is $O(n/128)$.
Add level-2 skip (every 8 blocks) for $O(\log n)$ seeking:

```
Level 2: [skip 0] ─────────────────────── [skip 1] ────────
Level 1: [0][1][2][3][4][5][6][7] ─ [8][9][10][11][12][13][14][15]
Data:    [blocks 0-7]                [blocks 8-15]
```

Lucene does this; Tantivy uses single-level (simpler, still fast).

---

## Alternatives Considered

### A1: Impact-Ordered Posting Lists

Store postings sorted by score contribution instead of doc ID:
- High-impact docs first -> threshold rises faster
- More blocks skipped overall

**Why not**: Complicates AND queries (need doc ID intersection). Lucene and
Tantivy don't use this despite academic interest. BMW provides most of the
benefit with simpler implementation.

### A2: Tiered Indexes

Maintain separate indexes for high-importance vs. low-importance documents:
- Query high tier first, may satisfy top-$k$ without touching low tier

**Why not**: Adds complexity for incremental updates. Better suited for
static corpora. Consider for future if we see specific workloads that benefit.

### A3: SIMD Intersection

Use SIMD instructions for posting list intersection:
- Galloping intersection with AVX2/AVX-512

**Why not**: Postgres extensions should be portable. Consider as optional
optimization with runtime detection.

### A4: Approximate Top-$k$ (WAND without max scores)

Original WAND algorithm without block-max optimization:
- Simpler implementation
- Still provides early termination

**Why not**: Block-Max WAND is now standard and not significantly harder to
implement. The block storage format we need anyway.

---

## Migration and Compatibility

### Segment Format Versioning

Add format version to segment header:
```c
#define TP_SEGMENT_FORMAT_V1 1  // Current: uncompressed, no blocks
#define TP_SEGMENT_FORMAT_V2 2  // Phase 1: block storage
#define TP_SEGMENT_FORMAT_V3 3  // Phase 3: compressed blocks
```

### Upgrade Path

1. New segments written in new format
2. Old segments readable until compacted
3. Full compaction migrates all data to new format
4. No in-place migration (immutable segments)

### Backward Compatibility

- Readers support all format versions
- Writers always use latest format
- `pg_upgrade` just works (segment files copied as-is)

---

## Implementation Roadmap

### v0.0.4: Block Storage Foundation
- [ ] Fixed-size posting blocks (128 docs)
- [ ] Block headers with last_doc_id, doc_count
- [ ] Skip index structure
- [ ] Segment format v2
- [ ] Basic block-aware seek operation
- [ ] Fieldnorm quantization table

**Estimated scope**: ~1500 LOC, 2-3 weeks

### v0.0.5: Block-Max WAND
- [ ] Block max score storage (max_tf, fieldnorm_id)
- [ ] Block max score computation at query time
- [ ] BMW executor for multi-term queries
- [ ] Single-term BMW optimization
- [ ] Threshold-based block skipping
- [ ] Benchmarks comparing old vs new query path

**Estimated scope**: ~1000 LOC, 2 weeks

### v0.0.6: Compression
- [ ] Delta encoding for doc IDs
- [ ] FOR encoding implementation
- [ ] PFOR encoding for outlier blocks
- [ ] VInt fallback for last block
- [ ] Frequency compression
- [ ] Decode benchmarks

**Estimated scope**: ~800 LOC, 1-2 weeks

### v0.0.7: Polish
- [ ] Multi-level skip list (optional, for very long lists)
- [ ] Segment-level pruning
- [ ] Roaring bitmaps for deleted docs
- [ ] Performance tuning based on benchmarks

**Estimated scope**: ~600 LOC, 1-2 weeks

---

## Success Metrics

| Metric | Current | Target | How to Measure |
|--------|---------|--------|----------------|
| Top-10 latency ($10^5$ matches) | ~50ms | <5ms | Cranfield benchmark |
| Top-10 latency ($10^6$ matches) | ~500ms | <20ms | Synthetic benchmark |
| Storage per posting | 10 bytes | 3-4 bytes | Index size / posting count |
| Index build time | baseline | <2x baseline | Cranfield benchmark |

---

## Prototype: Buffer Manager Overhead Test

Before committing to the on-disk hash table, validate that 1-2 buffer manager
calls per term lookup is acceptable.

**Setup:**
```sql
-- Simulate hash table structure: fixed-size slots with term data
CREATE TABLE term_lookup_bench (
    slot_id int PRIMARY KEY,
    term_hash int,
    term_offset int,
    term_len smallint,
    doc_freq int,
    posting_offset int,
    block_count smallint
);

-- Fill with ~100K entries (realistic term dictionary size)
INSERT INTO term_lookup_bench
SELECT
    i,
    hashtext('term' || i),
    i * 20,  -- simulated string pool offset
    10 + (i % 20),
    1 + (i % 10000),
    i * 100,
    1 + (i % 50)
FROM generate_series(0, 133333) i;  -- 100K terms at 75% load factor

-- Ensure data is in buffer cache
SELECT count(*) FROM term_lookup_bench;
```

**Benchmark:**
```sql
-- Measure: random lookups by slot_id (simulates hash probe)
-- Run 10K lookups, measure total time
\timing on
DO $$
DECLARE
    r record;
    target_slot int;
BEGIN
    FOR i IN 1..10000 LOOP
        target_slot := (hashtext('query_term' || i) % 133333);
        SELECT * INTO r FROM term_lookup_bench WHERE slot_id = target_slot;
    END LOOP;
END $$;
```

**Compare to baseline:**
- Current pg_textsearch term lookup (shared memory string interning)
- Raw hash table lookup in C (no buffer manager)

**Success criteria:** If buffer manager lookups are within 2-3x of shared memory
lookups, on-disk hash is viable. If 10x+ slower, we need shared memory caching.

---

## Open Questions for Discussion

1. **Block size**: 128 (Tantivy) vs 256 (Lucene)? Should it be configurable?

2. **Compression library**: Implement FOR/PFOR ourselves, or use existing
   library (e.g., simdcomp, TurboPFor)?

3. **Memtable integration**: Should memtable also use block structure, or
   keep it simple (linear) since it spills to segments anyway?

4. **Roaring scope**: Just deleted docs, or also filter caching?

5. **Benchmark suite**: What queries/datasets best represent our target
   workload?

6. **Term dictionary caching**: How aggressively should we cache term
   dictionaries in shared memory? Options:
   - Cache entire dictionary on first segment access (simple, uses more memory)
   - LRU cache of recently-used terms (complex, memory-bounded)
   - Hybrid: cache small segments fully, LRU for large segments

---

## References

1. Ding & Suel, "Faster Top-k Document Retrieval Using Block-Max Indexes"
   http://engineering.nyu.edu/~suel/papers/bmw.pdf

2. Tantivy Block-Max WAND implementation
   https://github.com/quickwit-oss/tantivy/blob/main/src/query/boolean_query/block_wand.rs

3. Lucene BlockMaxConjunctionScorer
   https://github.com/apache/lucene/blob/main/lucene/core/src/java/org/apache/lucene/search/BlockMaxConjunctionScorer.java

4. Frame of Reference and Roaring Bitmaps (Elastic blog)
   https://www.elastic.co/blog/frame-of-reference-and-roaring-bitmaps

5. Lemire et al., "Decoding billions of integers per second through
   vectorization" https://arxiv.org/abs/1209.2137

6. Zobel & Moffat, "Inverted Files for Text Search Engines"
   ACM Computing Surveys, 2006. https://doi.org/10.1145/1132956.1132959
