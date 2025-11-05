# pg_textsearch (Tapir) Memory Architecture

## Key Finding: Index Lives in Shared Memory, Not On Disk

Our investigation revealed that pg_textsearch stores its BM25 inverted index entirely in PostgreSQL's Dynamic Shared Areas (DSA), not on disk. The on-disk "index" only contains metadata for crash recovery.

## Data Storage Locations

### DSA Shared Memory (Actual Index)
The complete inverted index is stored in PostgreSQL shared memory via DSA:
- **Term dictionary** - Hash table mapping terms to posting lists
- **Posting lists** - Document IDs and term frequencies for each term
- **Document lengths** - Array of document lengths for BM25 normalization
- **Corpus statistics** - Total documents, average document length
- **String interning table** - Deduplicated term storage

Size: ~46MB for our test indexes with thousands of documents

### On-Disk Storage (Metadata Only)
The on-disk index pages contain only:
- **Block 0 (Metapage)** - Index configuration, corpus statistics, pointer to first docid page
- **Document ID pages** - Linked list of pages storing ItemPointerData entries for crash recovery

Size: Typically 16-56KB for thousands of documents

## Memory Usage Calculation

### DSA Memory (src/index.c)
```c
size_t dsa_total_size = dsa_get_total_size(index_state->dsa);
```
This returns the total allocated DSA memory for the index.

### On-Disk Size
```sql
SELECT pg_relation_size('index_name');
```
This returns only the metadata pages size, not the actual index.

## Implications

1. **Memory Requirements**: The entire index must fit in PostgreSQL shared memory
2. **Performance**: Very fast queries since all data is memory-resident
3. **Scalability**: Limited by available shared memory, not disk space
4. **Configuration**: The `tapir.shared_memory_size` parameter (default 16MB) controls per-index memory limit
5. **Restart Behavior**: Index must be rebuilt from document ID pages after PostgreSQL restart

## Benchmark Reporting

Benchmarks now correctly report both metrics:
```
DSA memory (actual index): 46.00 MB
On-disk size (metadata): 16 kB
```

This explains why our initial "index size" reports of 56KB for 6,800 documents were misleading - we were only measuring the on-disk metadata, not the actual 46MB inverted index in shared memory.

## Future Work

The current v0.0.3-dev is a memtable-only implementation. Future versions will add:
- Disk-based segments for overflow data
- Compression for better memory efficiency
- Background compaction workers
- True hybrid memory/disk architecture

Until then, pg_textsearch operates as a pure in-memory index with minimal on-disk persistence for crash recovery.
