# pg_textsearch: Storage and Query Optimization Design

**Status**: Mostly implemented (v0.5.0)
**Author**: Todd J. Green @ Tiger Data
**Last updated**: 2026-01-30

---

## Implementation Status

Most high-impact query and indexing optimizations are now implemented:

| Optimization | Status | Impact |
|--------------|--------|--------|
| Block-Max WAND query executor | ✅ Done (v0.3.0) | Sub-linear top-k queries |
| Block-based posting storage | ✅ Done (v0.2.0) | Enables BMW block skipping |
| Skip index with block-max scores | ✅ Done (v0.2.0) | O(log n) block seeking |
| Delta + bitpack compression | ✅ Done (v0.4.0) | 41% smaller indexes |
| Fieldnorm quantization | ✅ Done (v0.2.0) | 1 byte vs 2-4 bytes per doc |
| Doc ID mapping | ✅ Done (v0.2.0) | Compact u32 IDs, better compression |
| Parallel index builds | ✅ Done (v0.5.0) | Faster bulk indexing |
| SIMD decoding | 🔲 Not started | Minor speedup (~6% of query time) |
| Dictionary compression | 🔲 Not started | Further size reduction |
| Write concurrency | 🔲 Not started | Higher concurrent insert throughput |
| Many-token query optimization | 🔲 Not started | Faster 8+ term queries (see BMW limitations) |

**Current performance**: Query performance is competitive with other leading
Postgres-based solutions. See [benchmarks](https://timescale.github.io/pg_textsearch/benchmarks/comparison.html).

**Main remaining work**: Write concurrency is currently limited—inserts are
essentially serialized through a single memtable lock. Relaxing this would
improve concurrent insert throughput. The query path is well-optimized;
further gains would come from SIMD decoding (minor) or dictionary compression
(size only).

---

## Background

### The Problem (Now Solved)

Naive top-k retrieval has $O(n)$ complexity where $n$ = matching documents.
Production search engines solve this with Block-Max WAND, which skips blocks
that cannot contribute to top-k results.

**Empirical results** from Ding & Suel on TREC GOV2 (25.2M documents):
- Exhaustive OR: 3.8M docs evaluated, 225ms/query
- BMW: 22K docs evaluated (0.6%), 28ms/query (8x faster)

pg_textsearch now implements BMW (v0.3.0) with compressed block storage (v0.4.0).

### Non-Goals

- **Faceted search**: Expressible in SQL; not optimized
- **Query rewriting**: Belongs in application layer
- **Result highlighting**: Separate from scoring/retrieval

---

## Block-Based Query Optimization

### Why Block-Based Query Optimization?

Block-based algorithms (BMW, MAXSCORE) are the industry standard for top-$k$:
- **Lucene**: Block-Max MAXSCORE (switched from BMW in July 2022)
- **Tantivy/PISA**: Block-Max WAND
- **Elasticsearch/OpenSearch**: Built on Lucene's implementation

Both algorithms maintain a score threshold (the $k$-th best score seen so far)
and skip blocks where the maximum possible score is below the threshold. They
require the same underlying data structures (block storage, per-block max
scores), differing only in the query executor logic.

**Complexity**: Naive exhaustive evaluation is $O(mn)$ where $m$ = query terms,
$n$ = matching docs. Block-based algorithms achieve sub-linear time in practice
by skipping blocks whose max scores fall below the evolving threshold. Formal
complexity bounds remain an open question (Ding & Suel [1], Section 8).

**Empirical results** from Ding & Suel [1] on TREC GOV2 (25.2M documents):
- Exhaustive OR: 3.8M docs evaluated, 225ms/query
- WAND: 178K docs evaluated (4.7%), 77ms/query (2.9x faster)
- BMW: 22K docs evaluated (0.6%), 28ms/query (8x faster than exhaustive)
- With docID reassignment: BMW achieves 8.9ms/query (25x faster)

**Note on effectiveness**: Speedup depends on score distribution skew. Web
corpora exhibit power-law term frequencies, causing the threshold to rise
quickly. For uniform score distributions, fewer blocks are skipped.

### WAND vs MAXSCORE

Two algorithms share the same block storage but differ in query execution:

| Algorithm | Docs evaluated | Per-doc overhead | Best for |
|-----------|---------------|------------------|----------|
| Block-Max WAND | Fewer | Higher | Few terms, low k |
| Block-Max MAXSCORE | More | Lower | Many terms, high k |

**WAND** re-evaluates which terms are "essential" per document, enabling more
aggressive skipping but with O(m log m) overhead per iteration.

**MAXSCORE** partitions terms into essential/non-essential per block, with
lower overhead but less fine-grained skipping.

Lucene switched to MAXSCORE in 2022 because WAND's overhead exceeds its benefit
on complex queries. From [Elastic's analysis][7]: "WAND typically evaluates
fewer hits than MAXSCORE but with a higher per-hit overhead." For queries with
8+ terms, exhaustive evaluation can outperform both.

**Our approach**: Implement block storage first (Phase 1), then choose between
WAND and MAXSCORE based on our workload characteristics (Phase 2). The storage
format supports either algorithm.

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

### Multi-Segment Query Execution

The BMW algorithm above assumes a unified doc ID space, but we have segment-local
doc IDs (each segment's IDs start at 0). Following Lucene/Tantivy's design, we
run BMW **independently per segment** and merge results:

```
Query Execution:
1. For each segment: run BMW with segment-local doc IDs, collect top-k
2. For memtable: exhaustive scan (small, no blocks), collect top-k
3. Merge all results by score, return global top-k
```

This avoids doc ID unification complexity. The minor downside is that each
segment computes a full top-k even when the global threshold would have pruned
earlier, but this is acceptable given the simplicity benefit.

**IDF computation**: BM25 needs global IDF = log(N / df). Term dictionaries
store per-segment df, so we sum df across all segments at query time to compute
global df. This matches Lucene's approach and ensures scores are comparable
across segments.

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

**Solution: On-disk hash table**

Since segments are immutable, we know the exact term count at creation time.
This enables a statically-sized hash table stored in Postgres pages with O(1)
lookup (1-2 page accesses with linear probing at 75% load factor).

Why on-disk hash table over alternatives:
- **vs. sorted array + binary search**: O(1) vs O(log t) page accesses
- **vs. shared memory cache**: No cache management complexity; Postgres buffer
  manager already caches pages effectively
- **vs. auxiliary files (FST/mmap)**: Maintains transactional consistency;
  works with Postgres backup/restore, replication, pg_upgrade

The ~5x buffer manager overhead vs in-memory hash (see benchmark below) is
acceptable: a 10-term query adds only ~3μs total lookup overhead, negligible
compared to posting list traversal.

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
│ Slot 0: term_hash, term_offset, term_len,       │
│         doc_freq, skip_index_offset, block_count│
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

Each term's posting list is divided into blocks of up to 128 documents. For
cache efficiency, block **headers** (skip index) are stored separately from
block **data** (posting blocks):

```
Skip Index Entry (16 bytes per block):
┌────────────────────────────────────────────────────────┐
│ last_doc_id: u32     - Last doc ID in block            │
│ doc_count: u8        - Number of docs (1-128)          │
│ block_max_tf: u16    - Max term frequency in block     │
│ block_max_norm: u8   - Fieldnorm ID of max-scoring doc │
│ posting_offset: u32  - Byte offset from segment start  │
│ flags: u8            - Compression type, etc.          │
│ reserved: 3 bytes    - Future use                      │
└────────────────────────────────────────────────────────┘

Posting Block Data - Uncompressed (8 bytes per posting):
┌────────────────────────────────────────────────────────┐
│ TpBlockPosting[doc_count]:                             │
│   doc_id: u32      - Segment-local document ID         │
│   frequency: u16   - Term frequency in document        │
│   fieldnorm: u8    - Quantized document length         │
│   reserved: u8     - Padding for alignment             │
└────────────────────────────────────────────────────────┘

Posting Block Data - Compressed (variable size):
┌────────────────────────────────────────────────────────┐
│ [doc_id deltas...][frequencies...]                     │
│ (compressed with FOR/PFOR, see Phase 3)                │
│ (fieldnorms in separate table, not inline)             │
└────────────────────────────────────────────────────────┘
```

During BMW, we scan skip index entries (small, cacheable) to find candidate
blocks, then only decompress posting data for blocks that pass the threshold.

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
│ Term Dictionary                                         │
│   - String pool (term text)                             │
│   - Dict entries: skip_index_offset, block_count, df    │
├─────────────────────────────────────────────────────────┤
│ Posting Blocks (written first for streaming)            │
│   - 8 bytes per posting (uncompressed)                  │
│   - See "Fieldnorm Storage" below for format details    │
├─────────────────────────────────────────────────────────┤
│ Skip Index (per-term arrays of block headers)           │
│   - 16 bytes per block, enables binary search           │
│   - Written after postings (offsets now known)          │
├─────────────────────────────────────────────────────────┤
│ Fieldnorm Table                                         │
│   - 1 byte per doc, quantized document lengths          │
├─────────────────────────────────────────────────────────┤
│ Doc ID → CTID Mapping (6 bytes per doc)                 │
│   - Maps segment-local doc IDs back to heap tuples      │
└─────────────────────────────────────────────────────────┘
```

**Key insight**: The layout `[postings] → [skip index]` enables single-pass
streaming writes. We write postings while tracking offsets and block stats,
then write skip entries with known offsets. This eliminates multiple passes
over the data that were required when skip index preceded postings.

**Cache behavior**: During BMW queries, we scan skip entries to find candidate
blocks. Since skip entries are small (16 bytes per 128 docs) and accessed
sequentially, they cache well regardless of physical position.

#### 1.3 Fieldnorm Quantization

Lucene/Tantivy quantize document lengths to 1 byte (256 buckets) using a
log-scale mapping. This:
- Reduces storage (1 byte vs 2-4 bytes per doc per term)
- Enables compact block-max metadata
- Has negligible impact on ranking quality

We use Lucene's quantization scheme (SmallFloat.intToByte4). Both Lucene and
Tantivy independently converged on this same approach, which is reasonable
evidence for its effectiveness.

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

#### 1.4 Fieldnorm Storage Strategy

Fieldnorm storage differs between uncompressed and compressed posting formats
due to a space/performance tradeoff:

**Uncompressed format (V2)**: Fieldnorms are stored **inline** in each posting
entry. The `TpBlockPosting` structure has 2 bytes of padding for alignment,
which can hold the 1-byte fieldnorm at zero additional cost:

```c
typedef struct TpBlockPosting {
    uint32 doc_id;      // 4 bytes - segment-local document ID
    uint16 frequency;   // 2 bytes - term frequency
    uint8  fieldnorm;   // 1 byte  - quantized document length
    uint8  reserved;    // 1 byte  - padding for alignment
} TpBlockPosting;       // 8 bytes total
```

This eliminates per-posting fieldnorm lookups entirely, which is important
because we have observed high overhead from fieldnorm lookups in a separate
table. Each lookup requires a buffer manager round-trip (hash lookup, pin/unpin,
LWLock acquire/release), adding ~300-500ns per posting even when pages are
cached in shared_buffers. For queries touching millions of postings, this
overhead dominates query time.

**Compressed format (future)**: Inline storage is not viable because fieldnorms
are per-document, not per-posting. A document appearing in 100 posting lists
would have its fieldnorm stored 100 times, inflating posting data by 40-80%.
Instead, compressed formats use a **separate fieldnorm table** (1 byte per
document) with a caching scheme to amortize buffer manager overhead.

The skip index already stores `block_max_norm` (the fieldnorm of the
highest-scoring document in each block), so BMW block-skipping decisions don't
require per-posting fieldnorm access—only scoring of candidate documents does.

---
### Phase 2: Block-Based Query Executor

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

#### 3.1 Compression Strategy (Implemented in v0.4.0)

| Data | Encoding | Status |
|------|----------|--------|
| Doc IDs | Delta + Bitpacking | ✅ Implemented |
| Frequencies | Bitpacking | ✅ Implemented |
| Fieldnorms | Stored separately | ✅ Implemented |

**What we implemented**: Simple bitpacking where all values in a 128-doc block
use the same bit width (determined by the max value in that block). This is
similar to Tantivy's approach and provides good compression with minimal decode
overhead (~6% of query time per profiling).

**What we didn't implement**: FOR/PFOR (Frame-of-Reference with Patching).
FOR stores a base value plus small offsets; PFOR adds exception handling for
outliers. These provide slightly better compression for blocks with outliers
but add complexity. The simpler bitpacking approach was chosen for v0.4.0.

#### 3.2 Bitpacking (Implemented)

```c
// Compress a block of 128 postings
void tp_compress_block(TpBlockPosting *postings, uint32 doc_count,
                       uint32 first_doc_id, uint8 *out, uint32 *out_len) {
    // Delta encode doc IDs
    uint32 deltas[TP_POSTING_BLOCK_SIZE];
    uint32 prev_doc = first_doc_id;
    for (int i = 0; i < doc_count; i++) {
        deltas[i] = postings[i].doc_id - prev_doc;
        prev_doc = postings[i].doc_id;
    }

    // Find bits needed for max delta and max frequency
    uint8 doc_id_bits = bits_needed(max_delta);
    uint8 freq_bits = bits_needed(max_freq);

    // Write 2-byte header + bitpacked values
    out[0] = doc_id_bits;
    out[1] = freq_bits;
    bitpack_write(out + 2, deltas, doc_count, doc_id_bits);
    bitpack_write(out + 2 + packed_size, freqs, doc_count, freq_bits);
}
```

**Compression results (MS MARCO)**:

| Scale | Uncompressed | Compressed | System X | vs Uncompressed | vs System X |
|-------|-------------|------------|----------|-----------------|-------------|
| 100K docs | 27 MB | 16 MB | 14 MB | **-41%** | +14% |
| 8.8M docs | 2.4 GB | 1.4 GB | 1.5 GB | **-42%** | **-7%** |

At full scale, we're now **smaller than System X** (1396 MB vs 1500 MB).  However, System X is also storing positional information for phrase queries, so numbers are not entirely comparable.

#### 3.3 FOR/PFOR (Not Implemented - Future Option)

FOR (Frame of Reference) and PFOR (Patched FOR) are more sophisticated
compression schemes that could provide additional compression for blocks
with outlier values:

- **FOR**: Store base value + small offsets from base
- **PFOR**: FOR with separate storage for exceptions that don't fit

These were not implemented because:
1. Simple bitpacking already achieves competitive compression
2. Decode overhead is minimal (~6% of query time)
3. We're already smaller than System X at scale
4. Simpler code is easier to maintain

If compression needs to be improved further, PFOR would be the next step.

#### 3.4 SIMD Decoding (Future Optimization)

Modern implementations use SIMD instructions (SSE2/AVX2) for decoding, achieving
4+ billion integers/second per core. Key techniques from Lemire et al. [5]:

- **SIMD-BP128**: Bit-packing with 128-bit SIMD registers, ~2x faster than
  scalar
- **Vectorized delta decoding**: SIMD prefix sum is 2x faster than scalar

The current implementation uses scalar code for portability. SIMD paths could
be added with runtime detection for x86-64 if decode becomes a bottleneck
(currently only ~6% of query time).

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

Format changes require `REINDEX`. At this stage of development, we don't
support reading old segment formats—users must rebuild indexes after upgrades
that change the on-disk format. This keeps the implementation simple and avoids
carrying compatibility code for formats that may never see production use.

---

## Implementation Roadmap

### v0.2.0: Block Storage Foundation (released Dec 2025)
- [x] Fixed-size posting blocks (128 docs)
- [x] Block headers with last_doc_id, doc_count, block_max_tf, block_max_norm
- [x] Skip index structure (TpSkipEntry, 16 bytes per block)
- [x] Segment format V2 with block-based posting storage
- [x] Fieldnorm quantization table (Lucene SmallFloat encoding)
- [x] Doc ID mapping (segment-local u32 IDs, CTID map for heap lookup)
- [x] Index build optimization: direct mapping arrays for merge path
- [x] Index build optimization: binary search for initial segment writes
- [ ] Query-time block-aware seek operation

### v0.3.0: Block-Based Query Executor (released Jan 2026)
- [x] Block max score computation at query time
- [x] Query executor (initial BMW implementation)
- [x] Single-term optimization path
- [x] Threshold-based block skipping
- [x] Benchmarks comparing old vs new query path
- [x] GUC variables for BMW enable/disable and stats logging
- [ ] **Doc-ID ordered traversal** (see note below)

**Note on current BMW limitations**:

The v0.3.0 BMW implementation has two related limitations:

1. **Block-index iteration instead of doc-ID iteration**: The multi-term BMW
   iterates by block index (0, 1, 2, ...) rather than by doc ID. This assumes
   blocks across different terms are aligned, which they are not—each term's
   posting list has its own doc ID ranges. For short queries (1-4 terms), this
   works because block skipping still helps. For long queries (8+ terms), terms
   often have non-overlapping doc ID ranges, making block-index iteration
   ineffective.

2. **Single-block skipping only**: Even for single-term queries, we iterate
   through blocks sequentially and skip one block at a time:
   ```c
   for (block_idx = 0; block_idx < block_count; block_idx++) {
       if (block_max_scores[block_idx] < threshold)
           continue;  // Skip THIS block, check next
       // ... score block
   }
   ```
   We never use binary search on `last_doc_id` to jump over multiple blocks.
   The skip entry infrastructure supports O(log n) seeking, but we only use it
   for O(n) sequential iteration with single-block skips.

**Where multi-block seeking matters**:
- **WAND pivot advancement**: When advancing cursors to a pivot doc_id, binary
  search could skip hundreds of blocks instead of checking each one
- **Sparse term intersection**: Terms with non-overlapping ranges waste time
  scanning blocks that can't possibly match
- **Long posting lists**: A term with 10,000 blocks does 10,000 comparisons
  instead of ~13 (log2) to find a target doc_id

**The fix** requires WAND-style cursor-based traversal:
1. Track each term's current doc ID position (not block index)
2. Find minimum doc ID across all cursors (the "pivot")
3. Binary search `last_doc_id` in skip entries to seek directly to target blocks
4. Only load/score blocks that could contain documents at the pivot

This is the standard BMW algorithm described in Phase 2 above; the current
implementation is a simplified approximation that works well for common
short-query workloads but degrades for long queries.

### v0.4.0: Compression (released Jan 2026)

#### Posting Compression
- [x] Delta encoding for doc IDs (gaps between sorted IDs)
- [x] Bitpacking for deltas and frequencies (min bits per block)
- [x] GUC `pg_textsearch.compress_segments` (default: **on**)
- [x] Compression in both spill and merge paths
- [x] Mixed compression support (toggle GUC between spills)
- [ ] SIMD-accelerated decoding (future optimization)
- [ ] FOR/PFOR encoding (not needed - bitpacking is sufficient)

**Results (MS MARCO)**:

| Scale | Uncompressed | Compressed | System X | Change |
|-------|-------------|------------|----------|--------|
| 100K docs | 27 MB | 16 MB | 14 MB | -41% |
| 8.8M docs | ~2.4 GB | 1396 MB | 1500 MB | **-7% vs System X** |

**Query performance (100K)**:
- 1-token: 11% faster
- 2-token: 19% faster
- 3-token: 21% faster
- 8-token: 2% slower (negligible)

**Profiling (8.8M scale)**:
- Decode overhead: ~6% of query time (`bitpack_decode` + `tp_decompress_block`)
- Compression improves cache efficiency, offsetting decode cost

#### Dictionary Compression (planned)
Goal: Close the remaining gap to System X.

**Current dictionary overhead**:
- Term strings: ~10 bytes overhead + term length per term
- TpDictEntry: 12 bytes per term (skip_index_offset, block_count, doc_freq)
- For 100K terms: ~2-3 MB dictionary overhead

**Planned approach (incremental)**:

1. **Front-coding for term strings**
   - Sorted terms share prefixes: "search", "searching", "searchable"
   - Store: prefix_len (1 byte) + suffix bytes
   - Expected: 30-50% reduction in string pool size
   - Simple to implement, good ROI

2. **TpDictEntry bitpacking**
   - Block 256 terms together (like Tantivy's TermInfoStore)
   - First term stores full values, rest use bitpacked deltas
   - Expected: ~80% reduction in dict entry overhead

3. **FST (Finite State Transducer)** (optional)
   - Compressed trie mapping terms → ordinals
   - Maximum compression (~10% of raw string size)
   - More complex, requires careful C implementation
   - Consider only if front-coding insufficient

**Tantivy's approach for reference**:
- Uses `tantivy-fst` crate for term → ordinal mapping
- TermInfoStore with 256-term blocks and bitpacking
- Achieves ~2-4 bytes per term (vs our current ~20 bytes)

### v1.0.0: Production Ready (Target: Feb 2026)
- [ ] Performance tuning based on benchmarks
- [ ] Multi-level skip list (optional, for very long lists)
- [ ] Roaring bitmaps for deleted docs (optional)

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

Before committing to the on-disk hash table, we validated that buffer manager
overhead is acceptable by implementing C benchmark functions directly in
pg_textsearch.

### Benchmark Functions

```sql
-- Buffer manager: random page accesses (ReadBuffer/LockBuffer/ReleaseBuffer)
SELECT bm25_bench_buffer_lookup('table_name', iterations);

-- Baseline: in-memory hash table lookup (Postgres dynahash)
SELECT bm25_bench_hash_lookup(num_entries, iterations);
```

### Results (PG18, Apple M1, all data in buffer cache)

| Method | 1M ops (ms) | ns/op | Overhead ratio |
|--------|-------------|-------|----------------|
| Buffer manager | 346 | 330 | 4.7x |
| In-memory hash (100k entries) | 74 | 75 | 1.0x (baseline) |

**Variance**: Buffer manager: 32-34ms per 100k ops (consistent).
In-memory hash: 7-10ms per 100k ops.

**Scaling with hash size**:
- 10k entries: 6.6ms/100k lookups
- 100k entries: 8.4ms/100k lookups
- 500k entries: 18.7ms/100k lookups

### Analysis

The ~5x overhead is **acceptable** for the on-disk hash table design:

1. **Per-query impact is small**: A 10-term query adds ~3.3μs of buffer manager
   overhead (10 × 330ns). This is negligible compared to posting list traversal.

2. **Best case measured**: These results are with all pages in shared buffers.
   Real workloads will see similar performance for warm caches.

3. **Transactional consistency is worth it**: The alternative (auxiliary files
   or shared memory caches) would save ~250ns per lookup but add significant
   complexity and break ACID guarantees.

4. **Linear probing averages 1-2 page accesses**: With 75% load factor,
   expected probe length is ~2. So typical lookups: 1 page for slot +
   possibly 1 page for string pool verification = 2 buffer manager calls
   = ~660ns per term. Still fast.

**Conclusion**: Proceed with on-disk hash table. The buffer manager overhead is
a constant factor that doesn't affect our asymptotic improvements from BMW.

---

## DELETE Handling

Currently, we punt on explicit DELETE tracking in the index:

1. **Visibility**: Postgres handles visibility checking for rows returned by
   queries, so we never return a DELETEd row to the user.

2. **Statistics skew**: IDF and document frequency statistics can become stale
   after deletes, but this only affects ranking quality in pathological cases
   (mass deletion of documents containing specific terms).

3. **Result count**: We may return fewer tuples than LIMIT specifies after
   filtering deleted tuples. This is a known limitation.

4. **Future work**: Roaring bitmaps for deleted doc IDs (Phase 4) will enable
   skipping deleted docs during BMW iteration, improving both correctness of
   result counts and query performance.

---

## Open Questions for Discussion

1. **Block size**: 128 (Tantivy) vs 256 (Lucene)? Should it be configurable?

2. **Compression library**: Implement FOR/PFOR ourselves, or use existing
   library (e.g., simdcomp, TurboPFor)?

3. **Memtable block structure**: The memtable stays linear (no blocks) since
   it's small and write-optimized. BMW only applies to segments.

4. **Roaring scope**: Just deleted docs, or also filter caching?

5. **Benchmark suite**: What queries/datasets best represent our target
   workload?

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

7. Elastic, "MAXSCORE & block-max MAXSCORE: Improving the algorithm"
   https://www.elastic.co/search-labs/blog/more-skipping-with-bm-maxscore

8. Grand et al., "From MAXSCORE to Block-Max WAND: The Story of How Lucene
   Significantly Improved Query Evaluation Performance" ECIR 2020.
   https://cs.uwaterloo.ca/~jimmylin/publications/Grand_etal_ECIR2020_preprint.pdf
