# \[WIP\] Tapir – Technical Design Document

[Todd Green (TJ)](mailto:tj@tigerdata.com) / Aug 27, 2025

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
CREATE EXTENSION tapir;

-- Create a table with text content
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
INSERT INTO documents (content) VALUES
    ('PostgreSQL is a powerful database system'),
    ('BM25 is an effective ranking function'),
    ('Full text search with custom scoring');

-- Create a BM25 index on the text column
CREATE INDEX docs_tapir_idx ON documents USING tapir(content) WITH (ts_config='english');-- Query the indexSELECT id, content, content <@> to_tpvector('docs_tapir_idx', 'database system') AS score
FROM documents
ORDER BY score
LIMIT 10;
```

### Types

* `tpvector` encapsulates a document or query string along with an index name.  (We need the index’s tokenizer and statistics in order to compute BM25 scores).

### Operators

* `tpvector <@> tpvector` \- Core BM25 similarity scoring
* `text <@> tpvector` \- Mixed text-vector similarity scoring

### Built-ins

* `to_tpvector`
* `tpvector_eq`
* `text_tpvector_score`
* `tpvector_score`

### Access Methods

* `tapir`
  * Parameters: `ts_config` (required); `k1`, `b` (optional)
  * Bindings to `<@>` operator for BM25 ranked matches
  * Post-GA: bindings to `@@` operator and `tsvector` for Boolean matches

## Design Overview

![][image1]

![][image2]

### Memtable

Top-level inverted index stored in main memory.  Makes updates efficient while supporting fast reads too.  Capped at X MB.  Flush to disk (segment) when full.  For durability, also append to a document list stored in DB pages.  Protected by shared memory lock.

Composition of memtable:

* Term dictionary
  * Maps a string token \-\> integer id, posting list
  * **Current v0.0.0a**: String-based posting lists (term_ids are per-index, not global)
  * **Rationale**: Future disk segments require term_ids to be local to segment/memtable
  * Shared-memory hash table for string interning
* Posting list
  * Shared-memory vector of (docid, term frequency) pairs
  * Append on write, sort if needed on read
    * Needs to support fast intersection operation
  * Or: shared-memory search tree
    * If we want worst-case guarantees.
    * Didn’t see one in postgres sources, but easy exercise to build one
* Tombstones
  * Shared-memory hash table of docids
* Document lengths
  * List of (docid, length) – the only part stored on disk, for crash recovery

No compression in the memtable.

#### Memtable and transction abort

**Insertions**   Pretty sure that docs inserted as part of aborted transaction are more or less harmless (will be filtered out of any query results by Postgres executor).  But they will take up space in the index forever if we don’t clean them up.

**Deletions**   Tombstones really do need to get cleaned up after a transaction abort or there will be a correctness issue when they clobber their index items during compaction.

So long as we are punting on tombstones during early versions of this project we can safely ignore transaction aborts.  Once we implement tombstones we will need a little commit protocol inside the memtable, with a flag on each docid to say committed vs not.  Use

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

Will just describe the simplest case (AND queries) as a warmup to ranked queries.  This implementation and the extension to OR, NOT queries are all left for followup post-GA work.

Algorithm:

* Within each segment, get posting lists for the N query terms and perform N-way intersection.  Can leapfrog if we have skip lists.  Still fast (one pass) if we only have sorted lists.
* Evaluate query separately on memtable / segments and merge (union all) results.
* No sorting required

### BM25 queries

Score of a document D for query Q \= q1…qn:

BM25(D,Q) \= Σ IDF(qᵢ) × (tf(qᵢ,D) × (k₁ \+ 1)) / (tf(qᵢ,D) \+ k₁ × (1 \- b \+ b × |D|/avgdl))

Naive algorithm:

* For each query term
  * Look up the term in the inverted index
  * Retrieve the posting list containing (docID, term frequency) pairs.
  * Access freq values for each term
* For each document containing at least one query term:
  * Calculate the BM25 contribution for each query term present in the document
  * Sum the contributions across all query terms
* Sort documents by their BM25 scores in descending order

Optimized algorithm (from Tantivy):

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

No backwards compatibility guarantees until v0.1 (production launch).  Plan to start dogfooding with agents teams (hybrid search) as soon as possible – likely v0.0b.

* v0.0.0a: memtable-only implementation ✅ **COMPLETED**
  * Complete user surface in place ✅
  * Including crash recovery ✅ **FIXED: FlushOneBuffer() ensures metapage persistence**
  * Basic benchmarks passing validation ✅
  * String-based architecture for future disk segments ✅
  * All concurrency and recovery tests passing ✅
  * Target: Sep 5, 2025 → **Completed Sep 3, 2025**
* v0.0b: naive segment implementation
  * Scalable beyond main memory
  * No compression or optimizations
  * No background worker
    * Spill / compaction done on update thread
  * No tombstones
  * Target: Sep 19, 2025
* v0.0c: optimized segment implementation
  * Compression
  * Skip lists
  * Tombstones
  * Still no background worker
  * Target: Oct 17, 2025
* v0.0d: background worker
  * GUCs, observability, the works
  * Target: Nov 7, 2025
* v0.1
  * Bugfixes, performance tuning, polish
  * Benchmarks look good, stability looks good
  * Ready for initial production use cases
  * Accompanied by blog post
  * Target: Nov 28, 2025
