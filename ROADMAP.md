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

## v0.2.0 - Block Storage Foundation

Block storage foundation for query optimizations.

- **V2 segment format**: Block-based posting storage (128 docs/block)
- **Skip index**: Per-block metadata (last_doc_id, max_tf, max_fieldnorm)
- **Doc ID mapping**: Compact 4-byte segment-local IDs instead of 6-byte CTIDs
- **Fieldnorm quantization**: 1-byte encoded document lengths
- **Index build optimizations**: Binary search and direct mapping for CTID lookups
- **Unlimited indexes**: dshash registry removes fixed limit on concurrent indexes
- **Benchmark suite**: MS MARCO and Wikipedia benchmarks with public dashboard

## In Progress

### v0.3.0 - Query Optimizations

Query-time performance improvements building on block storage.

**Completed:**
- **Block-Max WAND (BMW) initial implementation**: Single-term and multi-term paths
- **Threshold-based block skipping**: Skip blocks where max score < threshold
- **Block max score precomputation**: Per-term block upper bounds for pruning
- **GUC variables**: `pg_textsearch.enable_bmw` and `log_bmw_stats` for debugging
- **Benchmark results**: 1.3-1.8x faster than System X on 1-4 term queries

**Remaining:**
- **Doc-ID ordered traversal**: Current multi-term BMW iterates by block index,
  not doc ID. This prevents efficient skipping for long queries (8+ terms) where
  different terms have non-overlapping doc ID ranges. Need WAND-style cursor-based
  traversal that seeks by doc ID using skip entry `last_doc_id` fields.

## Future

### v0.4.0 - Compression

Reduce storage footprint via posting list compression.

- **Delta encoding**: Compact doc ID storage
- **FOR/PFOR**: Frame-of-reference encoding for posting blocks

### v1.0.0 - Production Ready (Target: Feb 2026)

First production-quality release.

- Performance tuning and polish
- Benchmark validation on large datasets
- Backwards compatibility commitments begin

### Future (Post v1.0)

- **Boolean queries**: AND/OR/NOT via `@@` operator
- YOUR IDEA HERE
