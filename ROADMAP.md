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
| v0.3.0 | Jan 2026 | Block-Max WAND query optimization, 4x faster queries |
| v0.4.0 | Jan 2026 | Posting list compression (delta encoding + bitpacking), 41% smaller indexes |
| v0.5.0 | Jan 2026 | Parallel index builds, faster CREATE INDEX on large tables |
| v1.0.0 | Mar 2026 | First production-ready release |

## Upcoming

### Future (Post v1.0)

Definitely planned:
- **Continued optimizations**: ongoing task, see [optimization roadmap](OPTIMIZATION_ROADMAP.md)
- **Boolean queries**: AND/OR/NOT via `@@` operator
- **Background compaction**: needed to avoid write stalls in update-heavy workloads
- **Expression index** support

Potential future work:

- **Multi-tenant support**: tenant-id column supported, required for queries, under the covers there are many separate indexes, but only one from PG's point of view
- **Positional queries**: major work item
- **Faceted query optimizations**
