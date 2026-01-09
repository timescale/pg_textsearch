# Posting List Compression Design

This document analyzes compression strategies for pg_textsearch posting lists,
examining approaches used by Tantivy and Lucene/Elasticsearch, and recommends
an implementation path.

## TL;DR

**Algorithm choice**: Delta encoding + bitpacking (like Tantivy)
- Delta encoding exploits sorted doc IDs to produce small gaps
- Bitpacking packs gaps using minimum bits needed
- Skip VByte (too slow without SIMD)

**The honest tradeoff**:
- Compression ratio: 2-3× smaller (8 bytes → 3-4 bytes per posting)
- Decode overhead: 20-50% slower for cached data (scalar implementation)
- Helps: slow storage, memory-constrained systems, large indexes
- Hurts: hot queries on fast NVMe with data in buffer cache

**Recommendation**: Ship as opt-in (`pg_textsearch.compress_segments = off`),
benchmark on real workloads, add SIMD later to make it worthwhile by default.

## Current State

pg_textsearch uses uncompressed posting blocks with the following format:

```c
typedef struct TpBlockPosting {
    uint32 doc_id;    // 4 bytes - segment-local document ID
    uint16 frequency; // 2 bytes - term frequency
    uint8  fieldnorm; // 1 byte  - quantized document length
    uint8  reserved;  // 1 byte  - padding
} TpBlockPosting;     // 8 bytes total
```

- **Block size**: 128 documents (matching Tantivy)
- **Max block size**: 1024 bytes (128 × 8)
- **Skip entries**: 16 bytes per block (stored separately, always uncompressed)

The `TpSkipEntry.flags` field already reserves space for compression type
indicators:

```c
#define TP_BLOCK_FLAG_UNCOMPRESSED 0x00
#define TP_BLOCK_FLAG_DELTA        0x01
#define TP_BLOCK_FLAG_FOR          0x02
#define TP_BLOCK_FLAG_PFOR         0x03
```

## Compression Fundamentals

### Why Compress Posting Lists?

1. **Reduced I/O**: Smaller data means fewer disk reads
2. **Better cache utilization**: More postings fit in CPU cache
3. **Memory bandwidth**: Modern CPUs decompress faster than memory/disk can
   deliver uncompressed data

### The Core Tradeoff

```
Compression Ratio  ←→  Decompression Speed
     (storage)              (query latency)
```

Highly compressed formats save disk space but require more CPU cycles to
decode. The optimal choice depends on whether the system is I/O-bound or
CPU-bound.

### System Bandwidth Reality

To understand when compression helps, compare these bandwidths:

| Component           | Bandwidth      |
|---------------------|----------------|
| DDR4 memory         | ~50 GB/s       |
| DDR5 memory         | ~80 GB/s       |
| NVMe SSD            | 3-7 GB/s       |
| SATA SSD            | 0.5 GB/s       |
| Spinning disk       | 0.1-0.2 GB/s   |
| SIMD bitpack decode | 10-15 GB/s     |
| Scalar bitpack      | 4-6 GB/s       |
| Scalar VByte        | 1-2 GB/s       |

**Key insight**: Compression only helps when decompression is faster than the
data source. If data comes from memory at 50 GB/s but decompression runs at
2 GB/s, compression adds 25× latency for cached data.

### When Compression Helps

1. **Cold queries (data not in buffer cache)**
   - Reading from SSD at 5 GB/s, decompressing at 5 GB/s = net 2.5 GB/s
   - But transferring 2-3× less data, so effective rate improves
   - Clear win for I/O-bound workloads

2. **Memory-constrained systems**
   - More postings fit in shared_buffers and OS page cache
   - Fewer evictions, better hit rates
   - Indirect benefit even if decode adds CPU cost

3. **Large indexes exceeding available RAM**
   - Most queries hit disk
   - Compression reduces I/O wait time

### When Compression Hurts

1. **Hot queries (data in buffer cache)**
   - Uncompressed: read from memory at ~50 GB/s
   - Compressed: read at ~50 GB/s + decode at 1-5 GB/s
   - Pure overhead when data is already cached

2. **High query throughput on fast storage**
   - NVMe at 7 GB/s vs scalar VByte at 2 GB/s
   - Decompression becomes the bottleneck

3. **CPU-bound scoring workloads**
   - BM25 scoring is already CPU-intensive
   - Adding decode overhead competes for CPU cycles

### The SIMD Difference

SIMD changes the calculus significantly:

| Scenario              | Scalar       | SIMD          |
|-----------------------|--------------|---------------|
| NVMe (5 GB/s)         | Hurts        | Helps         |
| SATA SSD (0.5 GB/s)   | Helps        | Helps a lot   |
| Cached in RAM         | Hurts a lot  | Slight hurt   |

Without SIMD, compression is only clearly beneficial for slow storage or
memory-constrained systems. With SIMD (10-15 GB/s decode), compression helps
in most scenarios except pure in-memory workloads.

## Compression Algorithms

### 1. Variable Byte Encoding (VByte)

**How it works**: Each byte contains 7 bits of payload and 1 continuation bit.
The continuation bit indicates whether the number continues to the next byte.

```
Value: 824
Binary: 1100111000
VByte:  [0|0000110] [1|0111000]
         ^           ^
         continues   terminates
```

**Characteristics**:
- Simple implementation (~50 lines of C)
- Byte-aligned (no bit manipulation needed for basic decode)
- Compression ratio: ~2-3 bytes per doc ID (for typical gap sizes)
- Decode speed: ~1-2 GB/s (scalar), ~4-8 GB/s (SIMD-optimized)

**Used by**: Lucene (for low-frequency terms), many legacy systems

### 2. Delta Encoding

**How it works**: Instead of storing absolute doc IDs, store the difference
(gap) from the previous doc ID. Combined with other encodings.

```
Doc IDs:  [100, 105, 108, 200]
Deltas:   [100,   5,   3,  92]  ← smaller numbers, compress better
```

**Prerequisite**: Posting lists must be sorted by doc ID (ascending). pg_textsearch
already maintains this invariant - doc IDs within a block are always sorted.

**Why it works so well**: For high-frequency terms appearing in many documents,
gaps between consecutive doc IDs are small (often single digits). Instead of
storing 32-bit absolute IDs, you encode gaps that typically fit in 4-8 bits.

**Characteristics**:
- Not a compression scheme itself, but a transform that improves others
- Requires sequential decoding (can't random-access within block)
- Always used as a preprocessing step before VByte, FOR, or bitpacking
- First doc ID in block stored absolutely (or as delta from previous block's
  `last_doc_id` in skip entry)

### 3. Frame of Reference (FOR)

**How it works**: Store a reference value (minimum in block), then store all
values as fixed-width offsets from that reference.

```
Doc IDs:  [1000, 1003, 1005, 1010]
Min:      1000
Offsets:  [0, 3, 5, 10]  ← all fit in 4 bits
Stored:   [min=1000][4-bit packed: 0,3,5,10]
```

**Characteristics**:
- Excellent compression when values cluster tightly
- Fixed bit-width enables SIMD vectorization
- Decode speed: 4-8 GB/s (scalar), 10-15 GB/s (SIMD)
- Struggles with outliers (one large gap wastes bits for entire block)

### 4. Patched Frame of Reference (PFOR)

**How it works**: Like FOR, but outliers ("exceptions") that don't fit in the
common bit-width are stored separately.

```
Doc IDs:  [1000, 1003, 1005, 2000]  ← 2000 is an outlier
Base width: 4 bits (covers 0-15)
Main:     [0, 3, 5, EXCEPTION_MARKER]
Patches:  [(index=3, value=1000)]
```

**Characteristics**:
- Best compression for real-world posting lists with occasional large gaps
- More complex implementation (~300-500 lines of C)
- Decode speed: 3-6 GB/s (depends on exception rate)
- Used by Lucene for high-frequency terms

### 5. Bitpacking (BP128)

**How it works**: Pack N integers using exactly B bits each, where B is chosen
to fit the largest value in the block.

```
Values:   [5, 3, 7, 2]  ← max is 7, needs 3 bits
Packed:   [101][011][111][010] = 0xAF2 (12 bits for 4 values)
```

**Characteristics**:
- Extremely fast decode with SIMD (15+ GB/s)
- Block size of 128 chosen to match SIMD register widths
- Compression depends heavily on value distribution
- Simpler than PFOR (no exception handling)

### 6. Simple-8b

**How it works**: Pack as many integers as possible into a 64-bit word using
one of 16 predefined selectors.

```
Selector 0: 240 × 0-bit integers (all zeros)
Selector 1: 120 × 0-bit + 1 × 60-bit
Selector 7:   8 × 7-bit integers
...etc
```

**Characteristics**:
- Self-describing (selector embedded in each word)
- Good balance of simplicity and compression
- Decode speed: 2-4 GB/s
- Used by some search engines as a simpler PFOR alternative

## What Production Systems Use

### Tantivy (Rust)

Tantivy uses a hybrid approach:

1. **Delta encoding** as preprocessing for all doc IDs
2. **Bitpacking** (BP128) for blocks of 128 doc IDs
3. **VInt** for term frequencies (called "minus-one encoded" for freq ≥ 1)
4. **SIMD acceleration** via the `bitpacking` crate

Key design decisions:
- Block size: 128 (matches SIMD lane width × 4)
- Separate streams for doc IDs and frequencies
- Skip data stored separately for BMW optimization

**Compression ratio**: ~2.5-4 bytes per posting (vs 8 uncompressed)

### Lucene / Elasticsearch

Lucene uses adaptive compression:

1. **Terms appearing once**: Inlined directly into term dictionary
2. **Low-frequency terms**: Delta + VByte
3. **High-frequency terms**: Delta + PFOR (called "FOR" in Lucene, but with
   patching for exceptions)

Block structure:
- Block size: 128 doc IDs
- Separate `.doc`, `.pos`, `.pay` files for different data streams
- Block-max scores stored in skip data (like pg_textsearch)

Recent optimizations (Lucene 8.4+):
- Vectorized encoding/decoding
- Adaptive selection between FOR and VByte per block

**Compression ratio**: ~2-3 bytes per posting

### DuckDB

DuckDB (derived from FastPFor) uses:

1. **Constant encoding**: All same value → store once
2. **Dictionary encoding**: Few unique values → dictionary + indices
3. **FOR**: Tight range → frame of reference
4. **Bitpacking**: Otherwise → pack to minimum bits

Achieves 15 GB/s decompression with SIMD on modern CPUs.

## Benchmarks and Tradeoffs

### Compression Ratio Comparison

| Method          | Bits/Int | Bytes/Posting* | Ratio |
|-----------------|----------|----------------|-------|
| Uncompressed    | 32       | 8.0            | 1.0×  |
| VByte           | 16-24    | 4.0-5.0        | 1.6-2× |
| Bitpacking      | 8-16     | 2.5-4.0        | 2-3×  |
| FOR             | 8-12     | 2.0-3.5        | 2-4×  |
| PFOR            | 6-10     | 2.0-3.0        | 2.5-4× |
| Gamma codes     | 5-8      | 1.5-2.5        | 3-5×  |

*Including frequency (typically 1-2 bytes with VByte)

### Decompression Speed

| Method          | Scalar    | SIMD       |
|-----------------|-----------|------------|
| VByte           | 1-2 GB/s  | 4-8 GB/s   |
| Bitpacking      | 4-6 GB/s  | 10-15 GB/s |
| FOR             | 4-8 GB/s  | 10-15 GB/s |
| PFOR            | 2-4 GB/s  | 6-10 GB/s  |
| Gamma codes     | 0.5-1 GB/s| N/A        |

### When Each Method Wins

| Scenario                    | Best Method    | Why                           |
|-----------------------------|----------------|-------------------------------|
| I/O-bound (spinning disk)   | PFOR/Gamma     | Max compression helps most    |
| SSD, moderate query load    | FOR/Bitpacking | Good balance                  |
| NVMe, high query throughput | Bitpacking     | Decode speed dominates        |
| Memory-constrained          | PFOR           | Smallest footprint            |
| Simple implementation       | VByte          | Easy to get right             |
| SIMD available              | Bitpacking/FOR | Vectorizes cleanly            |

## Recommendation for pg_textsearch

### Honest Assessment

Given the tradeoff analysis above, compression is **not a clear win** for
pg_textsearch without SIMD optimization:

- Scalar VByte (1-2 GB/s) is slower than NVMe SSDs (3-7 GB/s)
- Scalar bitpacking (4-6 GB/s) is comparable to fast NVMe
- Both are much slower than cached memory access (~50 GB/s)

Compression primarily benefits:
- Systems with slow storage (SATA SSD, spinning disk)
- Memory-constrained deployments where cache efficiency matters
- Large indexes that don't fit in RAM

For modern NVMe-based systems with adequate RAM, uncompressed may actually
be faster for query latency until we add SIMD.

### Proposed Approach: Conservative Rollout

Start with compression as **opt-in**, gather benchmarks, then decide on
defaults.

#### Phase 1: Delta + Bitpacking (Optional, Off by Default)

Implement bitpacking (not VByte) from the start because:
- Scalar bitpacking (4-6 GB/s) is 2-3× faster than VByte (1-2 GB/s)
- Simpler upgrade path to SIMD (same algorithm, just vectorized)
- Fixed bit-width is easier to reason about

```
Block format (predictable size):
[1 byte: bit_width for doc_id deltas (1-32)]
[1 byte: bit_width for frequencies (1-16)]
[ceil(128 * doc_bits / 8) bytes: packed doc_id deltas]
[ceil(128 * freq_bits / 8) bytes: packed frequencies]
[128 bytes: fieldnorms (uncompressed)]
```

New GUC parameter:
```sql
SET pg_textsearch.compress_segments = off;  -- default: off
```

When enabled, new segments are written compressed. Existing uncompressed
segments continue to work (flag-based detection).

Expected compression: **~60-70% reduction**
Expected decode overhead: **~20-50% slower** for cached data (scalar)

#### Phase 2: Benchmark and Evaluate

Before changing defaults, benchmark on realistic workloads:

1. **Cranfield** (small, fits in RAM) - expect compression to hurt
2. **MS MARCO** (medium) - mixed results expected
3. **Wikipedia** (large, exceeds RAM) - expect compression to help

Metrics to collect:
- Query latency (p50, p95, p99)
- Index size reduction
- Buffer cache hit rate impact

#### Phase 3: SIMD Optimization

Add SIMD-accelerated decode paths:
- SSE2/SSSE3 for x86_64
- NEON for ARM64
- Scalar fallback for other platforms

Expected speedup: 2-4× decode throughput (10-15 GB/s)

After SIMD, compression becomes beneficial in most scenarios. Consider
changing the default to `compress_segments = on`.

#### Phase 4: Adaptive Compression (Future)

Per-block decision based on data characteristics:
- Skip compression if block is nearly full-width anyway
- Use RLE for highly repetitive frequencies
- Consider term frequency when choosing strategy

### What NOT to Compress

1. **Skip entries**: Keep 16-byte `TpSkipEntry` uncompressed
   - Random access needed for BMW
   - Only 16 bytes per 128 docs (0.125 bytes/doc overhead)

2. **Fieldnorms**: Keep as 1-byte SmallFloat
   - Already compact (1 byte vs 4-byte float)
   - Random access within block needed for scoring

3. **CTID map**: Consider separate optimization later
   - Different access pattern (lookup by doc_id)
   - Could use delta encoding on page numbers

### Block Format Versioning

Use the `flags` field in `TpSkipEntry` to indicate compression:

```c
#define TP_BLOCK_FLAG_UNCOMPRESSED 0x00  // Current format
#define TP_BLOCK_FLAG_DELTA_VBYTE  0x01  // Phase 1: Delta + VByte
#define TP_BLOCK_FLAG_BITPACK      0x02  // Phase 2: Delta + Bitpacking
```

This allows:
- Mixed blocks within a segment during transition
- Segment merge to recompress with newer format
- Fallback to uncompressed if compression doesn't help

### Compatibility

- Old segments with `flags=0x00` continue to work unchanged
- New segments use compressed blocks
- Segment merge automatically upgrades compression
- No `pg_upgrade` or reindex required

## Code Changes Summary

### Files to Modify

| File | Changes |
|------|---------|
| `src/mod.c` | Add `pg_textsearch.compress_segments` GUC |
| `src/segment/segment.h` | Add compression constants, declare encode/decode functions |
| `src/segment/segment.c` | Add bitpack encode, modify `tp_write_posting_block()` |
| `src/segment/scan.c` | Add decode logic in `tp_segment_posting_iter_load_block()` |
| `src/segment/merge.c` | Handle mixed compression during merge |
| `test/sql/compression.sql` | New test file for compression correctness |

### New GUC

```c
/* in mod.c */
bool pg_textsearch_compress_segments = false;  /* off by default */

DefineCustomBoolVariable(
    "pg_textsearch.compress_segments",
    "Compress posting blocks in new segments",
    NULL,
    &pg_textsearch_compress_segments,
    false,  /* default off until benchmarked */
    PGC_USERSET,
    0,
    NULL, NULL, NULL
);
```

### New Functions

```c
// Bitpacking (Phase 1)
static uint8 compute_bit_width(uint32 max_value);
static int bitpack_encode(uint32 *values, int count, uint8 bits, uint8 *out);
static void bitpack_decode(uint8 *in, int count, uint8 bits, uint32 *out);

// Delta transform
static void delta_encode(uint32 *values, int count);  /* in-place */
static void delta_decode(uint32 *values, int count);  /* in-place */
```

## Testing Strategy

1. **Correctness**: Round-trip tests encoding → decoding
2. **Edge cases**: Empty blocks, single doc, max values, 128-doc full blocks
3. **Mixed segments**: Queries spanning compressed and uncompressed blocks
4. **Regression**: Existing test suite must pass with compression on and off
5. **Benchmarks**: Compare with `pg_textsearch.compress_segments = on/off`:
   - `benchmarks/run_cranfield.sh` (small, cached)
   - `benchmarks/run_memory_stress.sh` (large, memory pressure)
   - Index size comparison via `bm25_summarize_index()`
   - Query latency percentiles (p50, p95, p99)

## Future Considerations

### Adaptive Per-Block Selection

Could analyze each block and choose the best method:
- Tight clustering → FOR
- Sparse with outliers → PFOR
- Very small deltas → run-length encoding

### Dictionary Compression for Terms

The term dictionary (string pool) could benefit from:
- Front-coding (shared prefixes)
- FST (Finite State Transducer) like Lucene

### Position List Compression

If we add phrase search, position lists need compression too:
- Similar techniques apply (delta + VByte)
- Positions are typically small gaps within a document

## References

- [Stanford IR Book: Postings Compression](https://nlp.stanford.edu/IR-book/html/htmledition/postings-file-compression-1.html)
- [FastPFor Library](https://github.com/lemire/FastPFor) - SIMD-optimized implementations
- [Tantivy Source](https://github.com/quickwit-oss/tantivy) - Rust search engine
- [Lucene Codec Documentation](https://lucene.apache.org/core/9_0_0/core/org/apache/lucene/codecs/lucene90/package-summary.html)
