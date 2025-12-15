# Changelog

## v0.1.1-dev (unreleased)

### New Features

- **Benchmark tracking**: Historical performance graphs published to GitHub
  Pages with regression alerts on PRs and release gates

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
