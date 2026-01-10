# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## Project Overview

pg_textsearch is a Postgres extension providing full-text search with BM25
ranking. (Internal name: Tapir) It implements a memtable-based architecture
similar to LSM trees, with in-memory structures that spill to disk segments
for scalability.

**Current Version**: 0.4.0-dev

**Postgres Version Support**: 17, 18 (tested in CI)

**Schema Organization**: Currently uses public schema. Future versions may
consider a dedicated `pg_textsearch` schema for cleaner namespace management.

## Important Notes for Development

- **DO NOT add pg_textsearch to shared_preload_libraries** - The extension
  loads via CREATE EXTENSION without needing shared_preload_libraries. If you
  get "access method bm25 does not exist", the issue is something else (e.g.,
  extension not installed, wrong database, etc.)

## Core Architecture

### Storage Architecture

The index uses a hybrid storage approach:
- **Memtable**: In-memory inverted index with term dictionary and posting
  lists, stored in shared memory for concurrent access
- **Segments**: Immutable disk-based structures using V2 block storage format
  with skip lists for efficient top-k queries

### Query Optimization

- **Block-Max WAND (BMW)**: Top-k query optimization that uses block-level
  upper bounds to skip non-contributing blocks
- **Skip Lists**: Segment format includes skip entries for fast block seeking

### Source Code Structure

The code is organized into logical directories:

```
src/
├── mod.c              # Extension init, GUC registration
├── source.c/h         # Abstract data source interface
├── constants.h        # Shared constants and configuration
├── am/                # Access method implementation
│   ├── handler.c      # Index handler (amhandler)
│   ├── build.c        # Index build and insert
│   ├── scan.c         # Index scan operations
│   └── vacuum.c       # Vacuum support
├── memtable/          # In-memory index structures
│   ├── memtable.c     # Memtable management
│   ├── posting.c      # Posting list data structures
│   ├── stringtable.c  # String interning hash table
│   ├── scan.c         # Memtable scan operations
│   └── source.c       # Data source interface for memtable
├── segment/           # Disk-based segment storage
│   ├── segment.c      # Segment read/write operations
│   ├── dictionary.c   # Term dictionary encoding
│   ├── docmap.c       # Document ID mapping
│   ├── scan.c         # Segment scan operations
│   ├── merge.c        # Segment compaction/merge
│   └── source.c       # Data source interface for segments
├── query/             # Query execution
│   ├── score.c        # BM25 scoring implementation
│   └── bmw.c          # Block-Max WAND optimization
├── types/             # SQL data types
│   ├── vector.c       # bm25vector type
│   └── query.c        # bm25query type
├── state/             # Index state management
│   ├── state.c        # Index state coordination
│   ├── registry.c     # Global registry for index mapping
│   ├── metapage.c     # Index metadata pages
│   └── limit.c        # Query result limits
├── planner/           # Query planner integration
│   ├── hooks.c        # Planner hooks for implicit resolution
│   └── cost.c         # Cost estimation
└── debug/             # Debugging utilities
    └── dump.c         # Index dump functions
```

### Data Types

- `bm25vector` - Stores term frequencies with index context for BM25 scoring
  - Format: `"index_name:{lexeme1:freq1,lexeme2:freq2,...}"`
- `bm25query` - Represents queries for BM25 scoring with optional index context
  - Format: `"query terms"` or `"index_name:{query terms}"` (via to_bm25query)

## Development Commands

### Building and Testing
```bash
make                   # build extension
make install           # install to Postgres
make test              # run SQL regression tests only
make installcheck      # run all tests (SQL + shell scripts)
make test-all          # same as installcheck

# Run a single test
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test basic  # runs basic.sql only

# Individual test types
make test-concurrency  # multi-session concurrency tests
make test-recovery     # crash recovery tests
make test-segment      # multi-backend segment tests
make test-stress       # long-running stress tests
make test-shell        # run all shell-based tests
make test-local        # run tests with dedicated Postgres instance
make expected          # generate expected output from test results
```

### Debug Builds
```bash
# Enable debug index dumps (add to Makefile or command line)
make PG_CPPFLAGS="-I$(pwd)/src -g -O2 -DDEBUG_DUMP_INDEX"
```

### Code Quality
```bash
make format            # auto-format C code with clang-format
make format-check      # check C code formatting (alias: lint-format)
make format-diff       # show formatting differences
make format-single FILE=path/to/file.c  # format specific file
```

## Configuration

### GUC Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `pg_textsearch.default_limit` | Default limit for queries without LIMIT | 1000 |
| `pg_textsearch.enable_bmw` | Enable Block-Max WAND optimization | true |
| `pg_textsearch.log_scores` | Log BM25 scores during scans | false |
| `pg_textsearch.log_bmw_stats` | Log BMW blocks scanned/skipped | false |
| `pg_textsearch.bulk_load_threshold` | Terms/xact to trigger spill | 100000 |
| `pg_textsearch.memtable_spill_threshold` | Posting entries to trigger spill | 32000000 |
| `pg_textsearch.segments_per_level` | Segments before compaction | 8 |

### Index Options

| Option | Description | Default |
|--------|-------------|---------|
| `text_config` | Postgres text search configuration | (required) |
| `k1` | BM25 term frequency saturation | 1.2 |
| `b` | BM25 length normalization | 0.75 |

## Test Structure

- **SQL tests** (`test/sql/`): Core functionality, BM25 scoring (scoring1-6),
  storage/segments, query optimization (bmw, wand), table features
  (partitioned, inheritance), and edge cases
- **Shell tests** (`test/scripts/`): concurrency.sh, recovery.sh, segment.sh,
  stress.sh for multi-session, crash recovery, and load testing
- **Expected output** (`test/expected/`): Reference outputs for regression tests

## BM25 Implementation

### Query Processing

- `text <@> bm25query` - primary operator, works in index scans and standalone
- `text <@> text` - implicit form, planner rewrites to `text <@> bm25query`
- Returns negative BM25 scores for Postgres ASC ordering
- bm25query with index name required for WHERE clause queries

### Block-Max WAND Optimization

When `pg_textsearch.enable_bmw` is enabled (default), top-k queries use:
- Block-level upper bounds computed during index build
- Early termination when blocks cannot contribute to results
- Skip lists for fast block seeking in segments

## Benchmarking

The `benchmarks/` directory contains performance testing tools:

### Datasets
- `sql/cranfield/` - Classic IR benchmark (Cranfield collection)
- `datasets/msmarco/` - MS MARCO passage ranking (download required)
- `datasets/wikipedia/` - Wikipedia articles (download required)

### Scripts
- `run_cranfield.sh` - Classic IR benchmark using Cranfield collection
- `run_memory_stress.sh` - Memory usage and capacity testing
- `sql/memory_stress*.sql` - Memory stress test configurations
- `runner/run_benchmark.sh` - Full benchmark runner with metrics extraction

### Comparative Benchmarks
- `engines/tantivy/` - Tantivy comparison for validation

## Continuous Integration

GitHub Actions workflows:
- `ci.yml` - Main CI pipeline testing Postgres 17, 18
- `sanitizer-build-and-test.yml` - Address sanitizer builds
- `formatting.yml` - Code formatting validation
- `release.yml` - Automated release workflow
- `package-release.yml` - Release packaging
- `upgrade-tests.yml` - Version upgrade testing
- `benchmark.yml` - Performance benchmarks
- `nightly-stress.yml` - Nightly stress tests with memory leak detection
- `claude-code-review.yml` - AI code review integration
- `claude.yml` - Claude Code automation

## Debug Functions

- `bm25_dump_index(index_name)` - Shows internal index structure with term
  dictionary, posting lists, and document entries
- `bm25_dump_index(index_name, filepath)` - Dumps full index to file
- `bm25_summarize_index(index_name)` - Shows high-level index statistics
- `bm25_spill_index(index_name)` - Forces memtable spill to disk segment,
  returns number of entries spilled

## Important Notes

### Concurrency Safety

- All shared memory structures protected by appropriate locks
- String interning is thread-safe via hash table locks
- Transaction isolation maintained through proper cleanup

### Testing Requirements

- Always run `make installcheck` before committing changes
- Concurrency tests verify thread safety under load
- Shell tests verify crash recovery and multi-backend behavior

### Code Style

- Uses clang-format for consistent C formatting
- Postgres coding conventions followed
- Memory allocation patterns follow Postgres standards
- Wrap all lines at 79 characters
- A memtable is stored in shared memory, but there is one memtable per index.
  Memtables are not shared across indexes, only across backend processes via
  shared memory.
- Postgres is installed at ~/pgsql

### Pre-Commit Checklist

**ALWAYS complete these steps before committing changes:**
1. `make` - Compile the extension
2. `make installcheck` - Run all tests
3. **Check test/regression.diffs** - Examine diffs to ensure your changes
   didn't introduce failures
4. If you modified error messages, update corresponding test/expected/*.out
5. `make format-check` - Verify code formatting
6. Re-run `make installcheck` after fixing any test expectation files

### Git Workflow

- **After PRs are merged**: Always check if main has been updated, even if you
  didn't do the merge yourself. Run `git fetch && git rebase origin/main` on
  active branches to avoid merge conflicts.
- **Before pushing**: Verify your branch is up-to-date with main to prevent
  conflicts from divergent histories.
