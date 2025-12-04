# pg_textsearch Roadmap

## Released

| Version | Date | Highlights |
|---------|------|------------|
| v0.0.1 | Oct 2025 | Initial memtable implementation, basic BM25 indexing |
| v0.0.2 | Oct 2025 | Bug fixes, stability improvements |
| v0.0.3 | Nov 2025 | Crash recovery, concurrency fixes, string-based architecture |
| v0.0.4 | Nov 2025 | BM25 score validation, PostgreSQL 18 support |

## Current (v0.0.5)

Naive segment implementation - scalable beyond main memory.

| Feature | Status |
|---------|--------|
| Segment infrastructure (disk storage) | Done |
| Segment query execution | Done |
| Auto-spill memtable to segments | Done |
| Segment chain traversal | Done |
| Crash recovery for segments | Done |
| Segment compaction | Not started |
| DELETE/UPDATE propagation to segments | Not started |

## Future

### v0.0.6 - Optimized Segments

Performance and space efficiency improvements.

- **Compression**: Delta encoding for posting lists, bitpacking
- **Skip lists**: Fast intersection for multi-term queries
- **Tombstones**: Proper DELETE support with compaction cleanup
- **Block-Max WAND**: Early termination for top-k queries

### v0.0.7 - Background Compaction

Move compaction off the critical path.

- **Background worker**: Async segment merging via pg_cron
- **Level-based compaction**: 8 segments at level N merge to 1 at level N+1
- **GUCs**: `max_segments_per_level`, compaction thresholds
- **Observability**: Progress tracking, statistics

### v0.1 - Production Ready

First production-quality release.

- Bug fixes, performance tuning, polish
- Benchmarks validated (Slack corpus, IR datasets)
- Documentation and blog post
- Backwards compatibility commitments begin

### Future (Post v0.1)

- **Boolean queries**: AND/OR/NOT via `@@` operator
- **Phrase queries**: Position-aware matching
- **Faceted search**: Term aggregations
- **Hybrid search**: Integration with pgvector

---

*See [DESIGN.md](DESIGN.md) for technical details.*
