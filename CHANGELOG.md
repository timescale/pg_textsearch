# Changelog

## v0.4.0

### New Features

- **Segment block compression**: LZ4 compression for posting list blocks,
  reducing index size

### Improvements

- Coverity static analysis integration for detecting code quality issues
- Implement AMPROP_DISTANCE_ORDERABLE in amproperty for better planner support

### Bug Fixes

- Fixed 'too many LWLocks taken' error on partitioned tables
- Fixed Coverity static analysis issues

### Breaking Changes

- **Segment format v3**: Indexes created with v0.3.0 or earlier must be
  recreated after upgrading to v0.4.0. Use `REINDEX INDEX index_name` or
  drop and recreate the index.

## v0.3.0

### New Features

- **Block-Max WAND optimization**: 4x faster top-k queries using block-level
  upper bounds to skip non-contributing blocks

### Improvements

- Added code coverage workflow with Codecov integration
- Added segment integrity tests
- Added nightly stress tests with memory leak detection
- Added competitive benchmarks comparing pg_textsearch to other search engines
- Code consolidation and refactoring for better maintainability

### Bug Fixes

- Fixed memory leaks by using private DSA for index builds
- Fixed bm25query varlena detoasting for binary I/O and scoring

## v0.2.0

### New Features

- **V2 segment format**: Block-based posting storage (128 docs/block) with
  skip index metadata for future Block-Max WAND optimization

- **Unlimited indexes**: Replaced fixed-size registry with dshash for
  unlimited concurrent BM25 indexes

- **Benchmark suite**: MS MARCO and Wikipedia benchmarks with historical
  performance tracking on GitHub Pages

### Improvements

- Major code refactoring: organized source into am/, memtable/, segment/,
  types/, state/, planner/, and debug/ directories
- Page reclamation after segment compaction
- Better cost estimation for query planning

### Bug Fixes

- Fixed excessive memory allocation in document scoring
- Fixed buildempty() to write init fork correctly

## v0.1.0

First open-source release!

### New Features

- **Implicit index resolution**: Queries can now use simpler syntax with
  automatic index lookup:
  ```sql
  SELECT * FROM docs ORDER BY content <@> 'search terms' LIMIT 10;
  ```
  Instead of the explicit form:
  ```sql
  SELECT * FROM docs ORDER BY content <@> to_bm25query('search terms', 'docs_idx') LIMIT 10;
  ```

- **Partitioned table support**: BM25 indexes now work on partitioned tables.
  Each partition maintains its own index, with parent index OID automatically
  mapping to partition indexes.

### Improvements

- Added `bm25_summarize_index()` function for fast index statistics without
  content dump
- Added `COST 1000` to scoring functions to help the query planner prefer
  index scans over sequential scans
- Improved page versioning with distinct magic numbers for different page
  types
- PostgreSQL 18 support verified in CI

### Bug Fixes

- Fixed extension upgrade paths from 0.0.4 and 0.0.5
- Fixed compiler warnings and standardized header guards

### Breaking Changes

- Removed `to_bm25vector()` function (use `to_bm25query()` instead)
- Renamed scoring functions to `bm25_` prefix (`bm25_text_bm25query_score`,
  `bm25_text_text_score`)
- Existing BM25 indexes must be rebuilt after upgrading

### Housekeeping

- Added PostgreSQL License and NOTICE file
- Added license headers to source files
