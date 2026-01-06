# pg_textsearch Roadmap

## Released

| Version | Date | Highlights |
|---------|------|------------|
| v0.0.1 | Oct 2025 | Initial memtable implementation, basic BM25 indexing |
| v0.0.2 | Oct 2025 | Bug fixes, stability improvements |
| v0.0.3 | Nov 2025 | Crash recovery, concurrency fixes, string-based architecture |
| v0.0.4 | Nov 2025 | BM25 score validation, PostgreSQL 18 support |
| v0.0.5 | Dec 2025 | Segment infrastructure, auto-spill, hierarchical merging |
| v0.1.0 | Dec 2025 | First open-source release, implicit index resolution, partitioned tables |
| v0.2.0 | Dec 2025 | V2 segment format, skip index, doc ID mapping, benchmark suite |

## Upcoming

### v0.3.0 - Query Optimizations (Jan 2026)

Query-time performance improvements building on block storage.

- **Block-Max WAND (BMW)**: Single-term and multi-term scoring with block skipping
- **WAND-style doc-ID traversal**: Correct multi-term scoring via doc-ID ordered iteration
- **Threshold-based block skipping**: Skip blocks where max score < threshold
- **Block max score precomputation**: Per-term block upper bounds for pruning
- **GUC variables**: `pg_textsearch.enable_bmw` and `log_bmw_stats` for debugging
- **Benchmark results**: 4.3x faster than exhaustive, competitive with System X

### v0.4.0 - Compression (Jan 2026)

Reduce storage footprint via posting list compression.

- **Delta encoding**: Compact doc ID storage
- **FOR/PFOR**: Frame-of-reference encoding for posting blocks

### v1.0.0 - Production Ready (Feb 2026)

First production-quality release.

- Performance tuning and polish
- Benchmark validation on large datasets
- Backwards compatibility commitments begin

### Future (Post v1.0)

- **Boolean queries**: AND/OR/NOT via `@@` operator
- YOUR IDEA HERE
