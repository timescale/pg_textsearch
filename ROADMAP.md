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

## Current (v0.2.0-dev)

Block storage foundation for query optimizations.

- **V2 segment format**: Block-based posting storage (128 docs/block)
- **Skip index**: Per-block metadata (last_doc_id, max_tf, max_fieldnorm)
- **Doc ID mapping**: Compact 4-byte segment-local IDs instead of 6-byte CTIDs
- **Fieldnorm quantization**: 1-byte encoded document lengths
- **Index build optimizations**: Binary search and direct mapping for CTID lookups

## Future

### v0.3.0 - Query Optimizations

Query-time performance improvements building on block storage.

- **Block-Max WAND/MAXSCORE**: Early termination for top-k queries
- **Threshold-based block skipping**: Skip blocks that can't contribute to top-k

### v1.0.0 - Production Ready (Target: Feb 2025)

First production-quality release.

- Performance tuning and polish
- Compression (if time permits): Delta + FOR/PFOR encoding
- Benchmark validation on large datasets
- Backwards compatibility commitments begin

### Future (Post v1.0)

- **Boolean queries**: AND/OR/NOT via `@@` operator
- YOUR IDEA HERE
