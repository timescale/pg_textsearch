# DSA Memory Investigation Report

## Executive Summary

**CRITICAL FINDING**: pg_textsearch has a severe memory inefficiency. Each BM25 index pre-allocates 46MB of DSA shared memory regardless of actual usage, while the configured memory limit is only 16MB. This wastes 30MB per index.

## Key Discoveries

### 1. Fixed 46MB DSA Allocation

Every BM25 index allocates exactly **48,234,496 bytes (46.00 MB)** of DSA memory, regardless of:
- Number of documents (same for 0, 10, 100, 1000, or even 10000 documents)
- Configured memory limit (16MB by default)
- Actual memory usage

### 2. DSA Segment Allocation Pattern

PostgreSQL's DSA (Dynamic Shared Areas) allocates segments using this formula:
```
segment_size = init_size * 2^(segment_index / 2)
```

With `DSA_NUM_SEGMENTS_AT_EACH_SIZE = 2`, this creates 9 segments:
- Segments 0-1: 1MB each = 2MB total
- Segments 2-3: 2MB each = 4MB total
- Segments 4-5: 4MB each = 8MB total
- Segments 6-7: 8MB each = 16MB total
- Segment 8: 16MB = 16MB total
- **Total: 1+1+2+2+4+4+8+8+16 = 46MB**

### 3. Memory Limit Not Enforced

The index configuration specifies:
- `TP_DEFAULT_INDEX_MEMORY_LIMIT = 16` (16MB)
- But DSA allocates 46MB upfront
- The extra 30MB is wasted shared memory

### 4. Actual Memory Usage vs Allocation

Testing shows:
- 100 Wikipedia articles: Uses < 1MB of actual data, allocates 46MB
- 1000 documents: Still fits within initial allocation, still 46MB
- 10000 documents: Still 46MB (may be hitting 16MB limit internally)

## Impact

### Resource Waste
- **30MB wasted per index** (46MB allocated - 16MB limit = 30MB waste)
- With 10 indexes: 300MB of wasted shared memory
- With 100 indexes: 3GB of wasted shared memory

### Scalability Issues
- Cannot create many indexes due to shared memory exhaustion
- System reports "out of shared memory" while most allocated memory is unused
- DSA segment exhaustion errors under load

## Root Cause

The issue stems from how DSA pre-allocates segments:
1. DSA doesn't know about the 16MB limit when created
2. DSA pre-allocates 9 segments (46MB) for performance
3. The index only uses up to 16MB, leaving 30MB unused
4. This memory cannot be reclaimed or used by other indexes

## Recommendations

### Short-term Fixes

1. **Align DSA allocation with memory limit**: Pass the memory limit to `dsa_create_ext()` to limit initial segment allocation

2. **Reduce initial segment count**: Modify DSA creation to allocate fewer segments initially (e.g., only segments 0-6 = 14MB)

3. **Use lazy segment allocation**: Only allocate segments as needed rather than upfront

### Long-term Solutions

1. **Custom memory allocator**: Implement a memory pool that better matches the index's needs

2. **Shared DSA across indexes**: Use a single DSA for all indexes with per-index quotas

3. **Segment compaction**: Implement ability to release unused DSA segments

## Testing Performed

```sql
-- Test 1: Memory scaling with document count
-- Result: Always 46MB regardless of 0 to 10000 documents

-- Test 2: DSA growth pattern
-- Result: Pre-allocates 9 segments totaling 46MB

-- Test 3: Memory limit enforcement
-- Result: 16MB limit not enforced at DSA level
```

## Conclusion

This is a critical performance bug causing:
- 188% memory overhead (46MB allocated for 16MB limit)
- Prevents scaling to many indexes
- Explains DSA segment exhaustion errors

The fix requires modifying how pg_textsearch initializes DSA to respect the configured memory limit and avoid pre-allocating unused segments.
