# PR Self-Review: Segment Implementation

## Critical TODOs and Loose Ends

### 1. Hardcoded Segment Size ❌ CRITICAL
**File:** `src/segment/segment.c:485`
```c
uint32 num_pages_needed = 10; /* TODO: Calculate actual size */
```
**Issue:** Segment size is hardcoded to 10 pages instead of calculating based on actual data size. This will cause:
- Wasted space for small segments
- **Crashes** for segments needing >10 pages (as we saw in testing)
- Should use `tp_segment_estimate_size()` function that already exists

### 2. Missing Metadata ❌ IMPORTANT
**File:** `src/segment/segment.c:534-535`
```c
header.num_docs     = 0; /* TODO: Get actual doc count */
header.total_tokens = 0; /* TODO: Get actual token count */
```
**Issue:** Not tracking document count or token count in segments, losing important statistics for BM25 scoring.

### 3. Memory Limit Enforcement ⚠️ INCOMPLETE
**File:** `src/index.c:910`
```c
/* TODO: Call tp_write_segment when we have the index relation. */
```
**Issue:** When memory limit is exceeded during insertions, we still error out instead of automatically spilling to segments.

### 4. Callback Architecture ⚠️ OPTIMIZATION
**File:** `src/operator.c:312-314`
```c
/* TODO: Refactor to use a callback function instead of allocating
 * and returning an array. This would avoid memory allocation and
 * allow the caller to process CTIDs as they're discovered. */
```
**Issue:** `tp_scan_docid_pages` allocates and copies all CTIDs unnecessarily.

## Major Performance Issues

### 1. NO Zero-Copy Operations ❌❌❌
The current implementation copies data at every level instead of operating on page data directly:

#### Query Path Copies:
1. **segment.c:197** - `memcpy` in `tp_segment_read()` - copies ALL data from pages
2. **segment.c:254** - `palloc` for EVERY term during binary search
3. **segment.c:291-297** - Allocates and copies all postings from segment
4. **segment.c:300-307** - ANOTHER allocation and copy to convert posting formats
5. **operator.c:164-166** - `memcpy` to merge posting lists from segments

#### Write Path Copies:
1. **dictionary.c:86** - `pstrdup` copies all terms when building dictionary

**Impact:** For a query matching 1000 documents across 3 segments:
- ~4000 unnecessary allocations
- ~12KB+ of unnecessary copies
- Multiple passes over the same data

### 2. Linear Scans ❌❌
**File:** `src/segment/segment.c:354-367`
```c
/* Linear search through document lengths (could optimize with binary search) */
```
**Issue:** O(n) lookup for EVERY document length retrieval. With 10,000 documents, this is 10,000 comparisons per query!

### 3. Multiple Buffer Acquisitions ⚠️
**File:** `src/segment/segment.c:192-199`
- Acquires and releases buffer lock for EVERY read operation
- Should batch reads or keep buffer locked for duration of operation

## Recommended Fixes

### Priority 1: Critical Bugs
1. **Fix segment size calculation:**
   ```c
   // Replace line 485:
   uint32 num_pages_needed = (tp_segment_estimate_size(state, index) + BLCKSZ - 1) / BLCKSZ;
   ```

2. **Track document metadata:**
   ```c
   // Get from memtable stats
   TpIndexMetaPage metap = tp_get_metapage(index);
   header.num_docs = metap->total_docs;
   header.total_tokens = metap->corpus_tokens;
   ```

### Priority 2: Zero-Copy Architecture
1. **Create zero-copy segment reader:**
   ```c
   typedef struct TpSegmentPostingIterator {
       Buffer buffer;
       Page page;
       TpSegmentPosting *current;
       uint32 remaining;
   } TpSegmentPostingIterator;

   // Return pointer directly to page data:
   TpSegmentPosting *tp_segment_get_posting_direct(reader, offset) {
       Page page = BufferGetPage(buffer);
       return (TpSegmentPosting *)(page + offset);
   }
   ```

2. **Eliminate intermediate copies in query path:**
   - Process postings directly from page buffers
   - Use callback-based iteration instead of materializing arrays
   - Keep buffers pinned during scoring

### Priority 3: Optimize Lookups
1. **Binary search for document lengths:**
   - Store document lengths sorted by CTID
   - Use binary search: O(log n) instead of O(n)

2. **Optimize term lookup:**
   - Keep frequently accessed pages in cache
   - Consider bloom filter for negative lookups

### Priority 4: Reduce Allocations
1. **Use memory contexts efficiently:**
   - Create per-query context
   - Reset instead of individual pfrees

2. **Pre-allocate buffers:**
   - Reuse buffers for term comparisons
   - Use stack allocation for small strings

## Memory Usage Analysis
Current implementation for 1000-doc query:
- Segment reads: ~1000 allocations, ~100KB
- Posting merges: ~3000 allocations, ~50KB
- Document scoring: ~2000 allocations, ~30KB
- **Total: ~6000 allocations, ~180KB overhead**

With zero-copy:
- Segment reads: 0 allocations
- Posting iteration: ~10 allocations, <1KB
- Document scoring: ~1000 allocations, ~20KB
- **Total: ~1010 allocations, ~21KB overhead**

## Conclusion
The segment implementation works functionally but has severe performance issues:
1. **Not production-ready** due to excessive copying
2. **Will not scale** beyond small datasets
3. **Needs major refactoring** for zero-copy architecture

The spilling mechanism works correctly for correctness testing, but performance optimization is critical before any real usage.
