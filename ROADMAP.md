# pg_textsearch Roadmap

## Released

| Version | Date | Highlights |
|---------|------|------------|
| v0.0.1 | Oct 2025 | Initial memtable implementation, basic BM25 indexing |
| v0.0.2 | Oct 2025 | Bug fixes, stability improvements |
| v0.0.3 | Nov 2025 | Crash recovery, concurrency fixes, string-based architecture |
| v0.0.4 | Nov 2025 | BM25 score validation, PostgreSQL 18 support |
| v0.0.5 | Dec 2025 | Segment infrastructure, auto-spill, hierarchical merging |
| v0.0.6 | Dec 2025 | Implicit index resolution, partitioned table support |

## Current (v0.0.7-dev)

Development version targeting segment optimizations.

## Future

### v0.0.7 - Optimized Segments

Performance and space efficiency improvements.

- **Compression**: Delta encoding for posting lists, bitpacking
- **Skip lists**: Fast intersection for multi-term queries
- **Block-Max WAND**: Early termination for top-k queries

### v0.0.8 - Background Compaction

Move compaction off the critical path.

- **Background worker**: Async segment merging
- **Observability**: Progress tracking, statistics

### v0.1 - Production Ready

First production-quality release.

- Bug fixes, performance tuning, polish
- Benchmark validation
- Backwards compatibility commitments begin

### Future (Post v0.1)

- **Boolean queries**: AND/OR/NOT via `@@` operator
- YOUR IDEA HERE
