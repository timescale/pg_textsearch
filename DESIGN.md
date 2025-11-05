# \[WIP\] pg_textsearch – Technical Design Document

[Todd Green (TJ)](mailto:tj@tigerdata.com) / Aug 27, 2025

*Internal code name: Tapir (**T**extual **A**nalysis for **P**ostgres **I**nformation **R**etrieval)*

# Background

See [PR/FAQ](https://docs.google.com/document/d/1SU-IxhBZqNhnAmliLAkQNTrDcer1RwJ1PW4OU-kNq7s/edit?tab=t.0) for requirements.

# People

Design / implementation: [Todd Green (TJ)](mailto:tj@tigerdata.com)
Review: Search Team \+ [Sven Klemm](mailto:sven@tigerdata.com)

# Proposed Design

Borrows heavily from Lucene / Tantivy / other open-source IR projects (*mutatis mutandis*).  The basic template has been battle-tested and seems to be more or less standard these days.

## Code Architecture

### Component Dependencies
**Respect these dependency relationships to avoid cycles:**

```
operator.c (top-level scoring coordination)
├── index.c (access method implementation)
├── memtable.c (DSA coordination)
├── stringtable.c (string interning + posting list coordination)
├── posting.c (pure posting list data structures)
├── state.c (index state management)
└── registry.c (global state registry)

memtable.c (DSA area management)
├── stringtable.c (string table management)
├── posting.c (posting list structures)
└── state.c (index state access)

stringtable.c (string interning + posting coordination)
└── posting.c (posting list allocation/management)

posting.c (leaf node - no dependencies on other tapir modules)
state.c (index state management)
registry.c (global state registry)
```

**Architectural Rules:**
- `posting.c` should NOT include `memtable.h` or `stringtable.h`
- `stringtable.c` should NOT include `memtable.h`
- Lower-level modules must not depend on higher-level modules
- Each module has distinct responsibilities and clean interfaces

## Terminology

* Document
  * Unit of text being indexed
  * Not necessarily a full “document” per se – could be titles, paragraphs, summaries, chunks, whatever
* Term
  * `to_tsvector` produces a list of terms
    * and positions – ignored by Tapir
  * “The quick brown fox jumps” \-\> \[ “quick”, “brown”, “fox”, “jump” \]
    * Capitalization normalized, punctuation erased, “the” removed ([stop word](https://en.wikipedia.org/wiki/Stop_word)), “jumps” replaced with “jump” ([stemming](https://en.wikipedia.org/wiki/Stemming))
* Inverted index
  * Maps terms back to documents
* Docid
  * Aka “posting entry”
  * In Tapir: (pageid, offset) of row containing document
* Posting list
  * List of docids containing the term

## User Surface

```sql
-- Enable the extension
CREATE EXTENSION pg_textsearch;

-- Create a table with text content
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
INSERT INTO documents (content) VALUES
    ('PostgreSQL is a powerful database system'),
    ('BM25 is an effective ranking function'),
    ('Full text search with custom scoring');

-- Create a BM25 index on the text column
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');-- Query the indexSELECT id, content, content <@> to_bm25query('database system', 'docs_idx') AS score
FROM documents
ORDER BY score
LIMIT 10;
```

### Types

* `bm25vector` - Stores term frequencies with index context for BM25 scoring (legacy, still supported)
* `bm25query` - Represents queries for BM25 scoring with optional index context (primary interface)

### Operators

* `text <@> bm25query` \- Primary BM25 scoring operator (works in index scans and standalone)
* `text <@> bm25vector` \- Legacy BM25 scoring operator
* `bm25vector <@> bm25vector` \- Legacy vector-to-vector scoring

### Built-ins

* `to_bm25query(text)` - Create bm25query without index name (for ORDER BY only)
* `to_bm25query(text, text)` - Create bm25query with query text and index name
* `to_bm25vector(text, text)` - Create bm25vector (legacy)
* `tpvector_eq` - Equality comparison for bm25vector

### Access Methods

* `bm25`
  * Parameters: `text_config` (required); `k1`, `b` (optional)
  * Bindings to `<@>` operator for BM25 ranked matches
  * Post-GA: bindings to `@@` operator and `tsvector` for Boolean matches

## Design Overview

![][image1]

![][image2]

### Memtable

Top-level inverted index stored in shared memory using PostgreSQL Dynamic Shared Areas (DSA).  Makes updates efficient while supporting fast reads too.  Memory budget enforced via `pg_textsearch.index_memory_limit` GUC (default 16MB per index).  Future versions will flush to disk (segment) when full.  For durability, document IDs are stored in index metapages.  Protected by shared memory locks.

Composition of memtable:

* Term dictionary
  * Maps a string token → posting list (DSA pointer)
  * **Current v0.0.3**: String-based architecture using dshash for string interning
  * **Implementation**: Each term string is interned once, posting lists allocated via DSA
  * **Rationale**: Avoids global term_id namespace, compatible with future disk segments
* Posting list
  * DSA-allocated vector of (ItemPointerData, term frequency) pairs
  * Append on write, sort by TID on read for efficient intersection
  * **Implementation**: Dynamic arrays in shared memory, grown via dsa_allocate_extended()
  * Thread-safe via per-posting-list spinlocks
* Document lengths
  * **Implementation**: dshash table mapping ItemPointerData → document length
  * **Persistence**: Document IDs stored in index metapages for crash recovery
  * Enables proper BM25 length normalization
* Corpus statistics
  * total_docs, avg_doc_length stored in shared index state
  * Updated incrementally during index builds and inserts
  * Used for IDF and BM25 calculations

No compression in the memtable.

#### Memtable and transaction abort

**Current v0.0.3 status**: Tombstones not yet implemented. Transaction abort handling deferred to future versions.

**Insertions**   Docs inserted as part of aborted transaction are filtered out by PostgreSQL executor.  They take up space in the index until segments are implemented with compaction.

**Deletions**   Tombstones will be needed for proper DELETE support. Need cleanup after transaction abort or there will be correctness issues during compaction.

**Future implementation**:  Will need a commit protocol inside the memtable with a flag on each docid for committed vs uncommitted.  Use

```c
RegisterXactCallback(XactCallback callback, void *arg);
```

to register hook with Postgres.

## Segments

Hierarchy of levels reminiscent of [LSM](https://en.wikipedia.org/wiki/Log-structured_merge-tree).  Spill of memtable \=\> 1 segment at level 0\. 8 segments at level n \=\> merged to 1 segment at level n \+ 1\.  (Tune with GUC `max_segments_per_level`).  Like in LSMs, segments are immutable.  Unlike in LSMs, segments are self-contained: a docid lives in exactly one segment; dictionary is local to segment; query evaluation can focus on one segment at a time.  This significantly simplifies the implementation.

Composition of a segment:

* Term dictionary
  * Maps a string token \-\> integer id, posting list location
  * Simplest: sorted, paged list of strings
    * O(log n) lookups – probably fine given skip list scan cost
    * Note: binary search requires list of pages to be kept somewhere
  * Fancier: persistent hash table
    * O(1) lookups
    * Not sure if there’s a paged hash table implementation to reuse
    * Again would need list of pages – or better, abstraction of uniform address space over paged memory
  * Fanciest: finite state transducer (cf. Lucene)
    * Most compact option – shares prefixes and suffixes
    * O(1) lookups
* Posting lists
  * Sorted, paged list of docids and term frequencies
    * need to support fast intersection operation
  * Fancier: use [skip list](https://en.wikipedia.org/wiki/Skip_list)
    * Big payoff for queries mixing frequent and infrequent terms
  * And: use [delta compression](https://en.wikipedia.org/wiki/Delta_encoding)
    * Crucial for keeping index size reasonable
    * Blockwise compression scheme (cf. Tantivy)
      * Interleaved blocks of 128 docids and 128 term frequencies
      * Calculate min number of bits needed for all deltas in block
      * Pack all 128 \- 1 deltas using exactly that many bits each
      * Use SIMD-optimized bitpacking
      * Decompress during scans (don’t attempt to work with compressed representation)
    * Potentially: separate fallback scheme for small blocks
* Tombstones
  * Simplest: sorted list of docids
  * Fancier: roaring bitmaps
  * But: maybe punt entirely on tombstones since Postgres filters out dead rows anyway.  Or: store them simply/naively, and only use them during segment merging/compaction to reduce space.
* Document lengths
  * Sorted, paged list of (docid, length) pairs

## Query Evaluation

### Boolean queries

**Status**: Not implemented in v0.0.3. Future work post-GA.

Will describe the simplest case (AND queries) as a warmup to ranked queries.  Extension to OR, NOT queries are all left for followup post-GA work.

Algorithm:

* Within each segment, get posting lists for the N query terms and perform N-way intersection.  Can leapfrog if we have skip lists.  Still fast (one pass) if we only have sorted lists.
* Evaluate query separately on memtable / segments and merge (union all) results.
* No sorting required

### BM25 queries

**v0.0.3 implementation**: Uses naive algorithm with memtable-only evaluation. Query limit detection via SPI to avoid exhaustive scans.

Score of a document D for query Q \= q1…qn:

BM25(D,Q) \= Σ IDF(qᵢ) × (tf(qᵢ,D) × (k₁ \+ 1)) / (tf(qᵢ,D) \+ k₁ × (1 \- b \+ b × |D|/avgdl))

Naive algorithm (current v0.0.3 implementation):

* For each query term
  * Look up the term in the string interning hash table
  * Retrieve the posting list containing (ItemPointerData, term frequency) pairs
  * Sort posting lists by TID if needed
* For each document containing at least one query term:
  * Calculate the BM25 contribution for each query term present in the document
  * Sum the contributions across all query terms to get document score
  * Store in hash table of document → score mappings
* Sort documents by their BM25 scores in descending order (via index scan or standalone)

Optimized algorithm (future work - inspired by Tantivy):

* Block-Max WAND (BMW) Algorithm (src/query/boolean\_query/block\_wand.rs)
  * This is the primary optimization for top-k ranked queries
  * Documents are processed in blocks of 128 docs
  * Each block stores its maximum BM25 score computed at index time
  * The algorithm maintains a dynamic threshold (initially MIN, then the k-th best score)
  * Blocks with max\_score ≤ threshold are entirely skipped without decompression
* Precomputed TF Component Cache (src/query/[bm25.rs:62-69](http://bm25.rs:62-69))
  * Fieldnorm calculation K1 \* (1 \- B \+ B \* fieldnorm/avg\_fieldnorm) is precomputed
  * Stored in a 256-element lookup table (one per fieldnorm ID)
  * Converts the BM25 calculation to simple array lookups at scoring time
* Fieldnorm Compression (src/fieldnorm/[mod.rs:9-16](http://mod.rs:9-16))
  * Document lengths are quantized to 256 values on a log scale
  * Stored as single bytes (fieldnorm IDs)
  * Enables the TF cache optimization above
  * Reduces memory usage while maintaining scoring accuracy
* Skip Lists with Block Metadata (src/postings/[skip.rs](http://skip.rs))
  * For posting lists \> 128 docs
  * Skip entries store: last\_doc\_in\_block, doc/tf bit widths, block\_wand\_fieldnorm\_id, block\_wand\_term\_freq
  * The (fieldnorm\_id, term\_freq) pair that maximizes BM25 for each block is stored at index time
  * Enables shallow seeking without loading block data

  Index Support for These Optimizations (from Tantivy)

* Block-Based Posting Storage (src/postings/block\_segment\_postings.rs)
  * Documents stored in compressed blocks of 128
  * Each block is independently decodable
  * Supports both bitpacking and VInt encoding
* Block Max Score Computation
  * At index time: computes max BM25 score per block (src/postings/[skip.rs:69-73](http://skip.rs:69-73))
  * Stores the (fieldnorm\_id, term\_freq) pair that achieves this maximum
  * Available via block\_max\_score() method without decompressing the block
* Shallow Seeking (src/query/term\_query/term\_scorer.rs:28-30)
  * Can position on a block without loading its data
  * Critical for Block-Max WAND to check block maximums efficiently
* Two-Phase Block Access
  * Phase 1: shallow\_seek() \- positions on block, reads metadata only
  * Phase 2: load\_block() \- decompresses block data when needed

  Query Evaluation Flow for Top-K BM25 (from Tantivy):

1. Initialize with k documents to retrieve
2. Find pivot: Determine minimum doc that could exceed threshold
3. Check block max: Use stored block max scores to test if block is worth processing
4. Skip or process:
   1. If block\_max ≤ threshold: skip entire block
   2. Otherwise: decompress and score documents
5. Update threshold: As better documents found, threshold increases
6. Early termination: More blocks get skipped as threshold rises

OPEN QUESTION: all of this is predicated on knowing what K is (i.e., the LIMIT argument in SQL), do we have to hook the query planner to figure this out?

## Background Compaction Worker

Will use pg\_cron.  Details TBD.

# Benchmarks

* Company Slack database used by agents (Eon etc.)
  * \~6GB
* Standard datasets: [https://ir-datasets.com/](https://ir-datasets.com/)
  * Need to pick some that seem relevant

Details TBD.  Want to start running these (for both performance and correctness) as soon as possible.

# Schedule and Milestones

No backwards compatibility guarantees until v0.1 (production launch).  Plan to start dogfooding with agents teams (hybrid search) as soon as possible – likely v0.0.2.

* v0.0.1: memtable-only implementation ✅ **COMPLETED**
  * Complete user surface in place ✅
  * Including crash recovery ✅ **FIXED: FlushOneBuffer() ensures metapage persistence**
  * Basic benchmarks passing validation ✅
  * String-based architecture for future disk segments ✅
  * All concurrency and recovery tests passing ✅
  * Target: Sep 5, 2025 → **Completed Sep 3, 2025**
* v0.0.2: naive segment implementation
  * Scalable beyond main memory
  * No compression or optimizations
  * No background worker
    * Spill / compaction done on update thread
  * No tombstones
  * Target: Sep 19, 2025
* v0.0.3: optimized segment implementation
  * Compression
  * Skip lists
  * Tombstones
  * Still no background worker
  * Target: Oct 17, 2025
* v0.0.4: background worker
  * GUCs, observability, the works
  * Target: Nov 7, 2025
* v0.1
  * Bugfixes, performance tuning, polish
  * Benchmarks look good, stability looks good
  * Ready for initial production use cases
  * Accompanied by blog post
  * Target: Nov 28, 2025
