# Parallel Build PR Revisions

Design notes for improvements to the parallel index build implementation.

## Goals

1. **Equivalence with serial builds**: Parallel indexing should produce results
   comparable to serial indexing:
   - Same query results (correctness)
   - Similar query performance (segment count, structure)
   - Similar index size (within reasonable tolerance)

   The only difference should be build time. Users shouldn't need to reason
   about whether their index was built serially or in parallel.

2. **Uniform segment sizes**: Segments at a given level should be roughly
   uniform in size. This improves query performance (predictable merge costs,
   balanced skip lists) and simplifies capacity planning. Small "runt"
   segments should be merged rather than left as stragglers.

## 1. Memory-based spill threshold

**Scope**: Parallel build only (worker memtables). The main shared memtable
continues using posting-count threshold since DSA memory tracking is
complicated by multi-index, multi-backend sharing.

**Current behavior**: Workers estimate memory using a heuristic:
```c
#define TP_BYTES_PER_POSTING_ESTIMATE 25
threshold_postings = memory_budget / 25;
```

This is inaccurate - actual memory per posting varies with term lengths,
terms-per-document ratio, and allocator overhead.

**Proposed change**: Use actual memory context tracking:
```c
if (MemoryContextMemAllocated(memtable->mcxt) >= memory_budget_per_worker)
    spill();
```

**Details**:
- Check memory before ingesting each document
- If over limit, spill and clear memtable first, then ingest
- Add ~10% slop to budget to avoid thrashing near boundary
- Remove `TP_BYTES_PER_POSTING_ESTIMATE` constant

**Benefits**:
- Accurate memory accounting regardless of document characteristics
- Respects `maintenance_work_mem` as users expect
- Simpler code (no heuristic tuning)

## 2. Parallel segment compaction

**Current behavior**: After workers finish building segments:
1. All worker segment chains linked into one L0 chain
2. Single-threaded compaction merges if count exceeds threshold

With 4 workers each spilling 3 times, this creates 12 segments merged
single-threaded - the merge phase can dominate build time.

**Proposed change**: Parallel compaction until LSM invariants restored.

```
Phase 1 (parallel): Workers build L0 segments
  Worker 0: [s0] → [s1] → [s2]
  Worker 1: [s3] → [s4]
  Worker 2: [s5] → [s6] → [s7]
  Worker 3: [s8]
  Total: 9 segments

Phase 2 (parallel): Each worker merges its own chain
  Worker 0: [s0,s1,s2] → [S0]
  Worker 1: [s3,s4]    → [S1]
  Worker 2: [s5,s6,s7] → [S2]
  Worker 3: [s8]       → [S3] (no merge needed, single segment)
  Result: 4 segments

Phase 3: Check invariants
  If segment_count ≤ segments_per_level: Done
  Otherwise: Redistribute segments among workers and repeat phase 2
```

For typical configs (4-8 workers, segments_per_level=8), completes in one
merge round.

**Implementation notes**:
- Reuse existing `tp_merge_segments()` infrastructure
- Barrier synchronization between build and merge phases
- Leader coordinates segment distribution between rounds
- Merge phase uses same page pool as build phase

## 3. Small segment handling

**Problem**: At end of build, each worker flushes its memtable regardless of
size. Workers that processed fewer docs produce small "runt" segments,
violating the uniform size goal.

```
Worker 0: 1M docs  → full segment
Worker 1: 1M docs  → full segment
Worker 2: 1M docs  → full segment
Worker 3: 50K docs → runt (5% of normal)
```

**Proposed approach**: During compaction, merge runts with adjacent segments:

1. Define runt threshold as fraction of target size (e.g., 25%)
2. After build phase, identify runt segments
3. Assign runts to workers merging adjacent segments

```
Before: [S0: 1M] [S1: 1M] [S2: 1M] [S3: 50K]

After redistribution:
  Worker 0: [S0]      → [T0: 1M]
  Worker 1: [S1]      → [T1: 1M]
  Worker 2: [S2, S3]  → [T2: 1.05M]
```

If multiple runts exist, they can be combined together before merging with
a full segment, or merged into a single segment if their combined size is
reasonable.

## Implementation Order

Suggested order based on dependencies and impact:

1. **Memory-based spill threshold** (Section 1)
   - Self-contained change to worker memtable spill logic
   - No dependencies on other changes
   - Immediate benefit: accurate memory control

2. **Parallel compaction - basic** (Section 2, phase 2 only)
   - Each worker merges its own chain after build
   - Simple barrier synchronization
   - Reduces segment count from `workers * spills` to `workers`

3. **Small segment handling** (Section 3)
   - Integrate runt detection into compaction phase
   - Modify segment distribution to pair runts with neighbors
   - Achieves uniform segment size goal

4. **Parallel compaction - full** (Section 2, multi-round)
   - Add redistribution logic for additional merge rounds
   - Only needed when workers > segments_per_level
   - Achieves full LSM invariant restoration
