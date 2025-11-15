# PR Breakdown Plan for Segment Implementation

This document outlines the strategy for breaking up the large segment implementation PR into smaller, manageable pieces that can be merged independently.

## Overview

The segment implementation is being split into 4 sequential PRs, each branching from main and merging independently. This allows for incremental progress and easier code review.

## PR 1: BM25 IDF Fix + Python Validation ✅
**Branch**: `fix-idf-calculation`
**Status**: In Progress

### Scope
- Switch to Tantivy's IDF formula: `ln(1 + (N - n + 0.5) / (n + 0.5))`
- Implement query-time IDF calculation (no pre-computation)
- Add Python validation infrastructure using custom `BM25Tantivy` class
- Update all test expected outputs to match new scores
- Add CI integration for BM25 validation

### Key Changes
- `src/operator.c` - New IDF formula that never produces negative values
- `src/query.c` - Simplified IDF calculation at query time
- `src/index.c` - Stub out `tp_calculate_idf_sum` functions (kept for compatibility)
- `test/python/bm25_tantivy.py` - Custom BM25 implementation matching Tantivy
- `test/python/validate_bm25.py` - Validation script for test cases
- Update all `test/expected/*.out` files with new scores
- Makefile target: `make validate-bm25`
- CI workflow: `.github/workflows/bm25-validation.yml`

### Expected Score Changes
All BM25 scores will change due to the new IDF formula. For the common test case where all documents contain a term, the IDF will be 0.105361 instead of negative values with epsilon handling.

## PR 2: Segment Infrastructure
**Branch**: `add-segment-infrastructure`
**Status**: Planned (after PR 1 merges)

### Scope
- Basic segment structures and type definitions
- Segment writer/reader implementation
- Segment dictionary and posting list storage formats
- No actual segment creation from memtable yet
- No query execution over segments

### Key Files
- `src/segment/segment.h` - Type definitions and structures
- `src/segment/segment.c` - Basic read/write operations
- `src/segment/dictionary.c` - Dictionary handling
- Unit tests for segment I/O

## PR 3: Memtable Spilling
**Branch**: `add-memtable-spilling`
**Status**: Planned (after PR 2 merges)

### Scope
- Implement spilling logic when memtable reaches capacity
- `tp_spill_memtable` function implementation
- Integration with index build process
- Memory budget enforcement
- Crash recovery for segments

### Key Changes
- `src/index.c` - Spilling triggers during inserts
- `src/state.c` - State management for spilling
- `src/memtable.c` - Clear memtable after spilling
- Tests for spilling behavior and memory limits

## PR 4: Segment Query Execution
**Branch**: `add-segment-queries`
**Status**: Planned (after PR 3 merges)

### Scope
- Query execution over segments
- Document frequency aggregation across segments and memtable
- Term lookup in segment dictionaries
- Performance optimizations for segment access

### Key Files
- `src/segment/segment_query.c` - Query execution logic
- `src/query.c` - Integration with segment queries
- `src/operator.c` - Scoring with segments
- End-to-end tests with large datasets

## Python Validation Infrastructure

### BM25Tantivy Class
The `test/python/bm25_tantivy.py` file implements a custom BM25 class that:
- Subclasses `rank_bm25.BM25Okapi`
- Overrides `_calc_idf` to use Tantivy's formula
- Produces exact scores matching Tantivy (e.g., 0.105361 for uniform term distribution)
- Requires only `pip install rank-bm25` (lightweight dependency)

### Validation Script
The `test/python/validate_bm25.py` script:
- Reads SQL test files
- Extracts expected BM25 scores
- Computes ground truth using BM25Tantivy
- Validates that PostgreSQL scores match
- Can be run via `make validate-bm25`

### CI Integration
- Runs on every PR and push to main
- Validates all BM25 test cases against Python ground truth
- Ensures scoring remains consistent with Tantivy's implementation
- Lightweight and fast (no Rust/Tantivy runtime needed)

## Testing Strategy

Each PR will include:
1. Unit tests for new functionality
2. Integration tests where applicable
3. Update to existing tests to maintain compatibility
4. Performance benchmarks for critical paths

## Migration Notes

For users upgrading:
- PR 1 will change all BM25 scores due to the new IDF formula
- This is a breaking change but provides more accurate and consistent scoring
- Scores will never be negative (excluding PostgreSQL's ordering negation)
- Results will match industry-standard Tantivy implementation
