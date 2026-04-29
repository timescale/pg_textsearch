# pg_textsearch

[![CI](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml)
[![Benchmarks](https://github.com/timescale/pg_textsearch/actions/workflows/benchmark.yml/badge.svg)](https://timescale.github.io/pg_textsearch/benchmarks/)
[![Coverity Scan](https://scan.coverity.com/projects/32822/badge.svg)](https://scan.coverity.com/projects/pg_textsearch)

Modern ranked text search for Postgres.

- Simple syntax: `ORDER BY content <@> 'search terms'`
- BM25 ranking with configurable parameters (k1, b)
- Works with Postgres text search configurations (english, french, german, etc.)
- Expression indexes for JSONB fields, multi-column search, and text transformations
- Partial indexes for scoped search and multilingual tables
- Fast top-k queries via Block-Max WAND optimization
- Parallel index builds for large tables
- Supports partitioned tables
- Best in class performance and scalability

🚀 **Status**: v1.2.0-dev - Production ready.

![Tapir and Friends](images/tapir_and_friends_v1.1.0.png)

## Historical note

The original name of the project was Tapir - **T**extual **A**nalysis for **P**ostgres **I**nformation **R**etrieval.  We still use the tapir as our
mascot and the name occurs in various places in the source code.

## PostgreSQL Version Compatibility

pg_textsearch supports PostgreSQL 17 and 18.

## Installation

### Pre-built Binaries

Download pre-built binaries from the
[Releases page](https://github.com/timescale/pg_textsearch/releases).
Available for Linux and macOS (amd64 and arm64), PostgreSQL 17 and 18.

### Build from Source

```sh
cd /tmp
git clone https://github.com/timescale/pg_textsearch
cd pg_textsearch
make
make install # may need sudo
```

## Getting Started

pg_textsearch must be loaded via `shared_preload_libraries`. Add the following
to `postgresql.conf` and restart the server:

```
shared_preload_libraries = 'pg_textsearch'  # add to existing list if needed
```

Then enable the extension (once per database):

```sql
CREATE EXTENSION pg_textsearch;
```

Create a table with text content

```sql
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
INSERT INTO documents (content) VALUES
    ('PostgreSQL is a powerful database system'),
    ('BM25 is an effective ranking function'),
    ('Full text search with custom scoring');
```

Create a pg_textsearch index on the text column

```sql
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

## Querying

Get the most relevant documents using the `<@>` operator

```sql
SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

Note: `<@>` returns the negative BM25 score since Postgres only supports `ASC` order index scans on operators. Lower scores indicate better matches.

The index is automatically detected from the column. For explicit index specification:
```sql
SELECT * FROM documents
ORDER BY content <@> to_bm25query('database system', 'docs_idx')
LIMIT 5;
```

Supported operations:
- `text <@> 'query'` - Score text against a query (index auto-detected)
- `text <@> bm25query` - Score text with explicit index specification

### Verifying Index Usage

Check query plan with EXPLAIN:
```sql
EXPLAIN SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

For small datasets, PostgreSQL may prefer sequential scans. Force index usage:
```sql
SET enable_seqscan = off;
```

Note: Even if EXPLAIN shows a sequential scan, `<@>` and `to_bm25query` always use the index for corpus statistics (document counts, average length) required for BM25 scoring.

### Filtering with WHERE Clauses

There are two ways filtering interacts with BM25 index scans:

**Pre-filtering** uses a separate index (B-tree, etc.) to reduce rows before scoring:
```sql
-- Create index on filter column
CREATE INDEX ON documents (category_id);

-- Query filters first, then scores matching rows
SELECT * FROM documents
WHERE category_id = 123
ORDER BY content <@> 'search terms'
LIMIT 10;
```

**Post-filtering** applies the BM25 index scan first, then filters
results. Columns without their own index are filtered after the BM25
scan:
```sql
SELECT * FROM documents
WHERE length(content) > 100
ORDER BY content <@> 'search terms'
LIMIT 10;
```

**Performance considerations**:

- **Pre-filtering tradeoff**: If the filter matches many rows (e.g., 100K+), scoring
  all of them can be expensive. The BM25 index is most efficient when it can use
  top-k optimization (ORDER BY + LIMIT) to avoid scoring every matching document.

- **Post-filtering tradeoff**: The index returns top-k results *before* filtering.
  If your WHERE clause eliminates most results, you may get fewer rows than
  requested. Increase LIMIT to compensate, then re-limit in application code.

- **Best case**: Pre-filter with a selective condition (matches <10% of rows), then
  let BM25 score the reduced set with ORDER BY + LIMIT.

This is similar to the [filtering behavior in pgvector](https://github.com/pgvector/pgvector?tab=readme-ov-file#filtering),
where approximate indexes also apply filtering after the index scan.

## Indexing

Create a BM25 index on your text columns:

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english');
```

### Index Options

- `text_config` - PostgreSQL text search configuration to use (required)
- `k1` - term frequency saturation parameter (1.2 by default)
- `b` - length normalization parameter (0.75 by default)

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english', k1=1.5, b=0.8);
```

Also supports different text search configurations:

```sql
-- English documents with stemming
CREATE INDEX docs_en_idx ON documents USING bm25(content) WITH (text_config='english');

-- Simple text processing without stemming
CREATE INDEX docs_simple_idx ON documents USING bm25(content) WITH (text_config='simple');

-- Language-specific configurations
CREATE INDEX docs_fr_idx ON french_docs USING bm25(content) WITH (text_config='french');
CREATE INDEX docs_de_idx ON german_docs USING bm25(content) WITH (text_config='german');
```

### Expression Indexes

Index expressions instead of plain columns — useful for JSONB fields,
multi-column concatenation, and text transformations:

```sql
-- JSONB field extraction
CREATE INDEX ON events USING bm25 ((data->>'description'))
    WITH (text_config='english');

SELECT * FROM events
ORDER BY (data->>'description') <@> to_bm25query('network error', 'events_expr_idx')
LIMIT 10;

-- Multi-column search
CREATE INDEX ON articles USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');

-- Text transformation
CREATE INDEX ON docs USING bm25 ((lower(content)))
    WITH (text_config='simple');
```

The expression must evaluate to `text` and use only IMMUTABLE functions.
Queries must repeat the same expression in the `ORDER BY` clause.

### Partial Indexes

Index a subset of rows by adding a `WHERE` clause. Partial indexes are
smaller and faster when queries always target a specific subset:

```sql
CREATE INDEX ON docs USING bm25 (content)
    WITH (text_config='english')
    WHERE status = 'published';

SELECT * FROM docs
WHERE status = 'published'
ORDER BY content <@> to_bm25query('search terms', 'docs_content_idx')
LIMIT 10;
```

Partial indexes require explicit index naming via `to_bm25query()` — the
implicit `text <@> 'query'` syntax skips them.

Expression and partial indexes can be combined:

```sql
CREATE INDEX ON events USING bm25 ((data->>'message'))
    WITH (text_config='english')
    WHERE (data->>'severity') = 'error';
```

### Multilingual Tables

For tables with documents in multiple languages, create one partial index
per language, each with the appropriate text search configuration:

```sql
ALTER TABLE docs ADD COLUMN lang CHAR(2) NOT NULL DEFAULT 'en';

CREATE INDEX docs_en_idx ON docs USING bm25 (content)
    WITH (text_config='english') WHERE lang = 'en';
CREATE INDEX docs_de_idx ON docs USING bm25 (content)
    WITH (text_config='german')  WHERE lang = 'de';
CREATE INDEX docs_fr_idx ON docs USING bm25 (content)
    WITH (text_config='french')  WHERE lang = 'fr';
```

Each index applies language-appropriate stemming and stop words. Query
with the matching predicate and index name:

```sql
SELECT * FROM docs
WHERE lang = 'en'
ORDER BY content <@> to_bm25query('databases', 'docs_en_idx')
LIMIT 10;
```

## Data Types

### bm25query

The `bm25query` type represents queries for BM25 scoring with optional index context:

```sql
-- Create a bm25query with index name (required for WHERE clause and standalone scoring)
SELECT to_bm25query('search query text', 'docs_idx');
-- Returns: docs_idx:search query text

-- Embedded index name syntax (alternative form using cast)
SELECT 'docs_idx:search query text'::bm25query;
-- Returns: docs_idx:search query text

-- Create a bm25query without index name (only works in ORDER BY with index scan)
SELECT to_bm25query('search query text');
-- Returns: search query text
```

**Note**: In PostgreSQL 18, the embedded index name syntax using single colon (`:`) allows the
query planner to determine the index name even when evaluating SELECT clause expressions early.
This ensures compatibility across different query evaluation strategies.

#### bm25query Functions

Function | Description
--- | ---
to_bm25query(text) → bm25query | Create bm25query without index name (for ORDER BY only)
to_bm25query(text, text) → bm25query | Create bm25query with query text and index name
text <@> bm25query → double precision | BM25 scoring operator (returns negative scores)
bm25query = bm25query → boolean | Equality comparison

## Performance

pg_textsearch indexes use a memtable architecture for efficient writes. Like other index types, it's faster to create an index after loading your data.

```sql
-- Load data first
INSERT INTO documents (content) VALUES (...);

-- Then create index
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

### Parallel Index Builds

pg_textsearch supports parallel index builds for faster indexing of large tables.
Postgres automatically uses parallel workers based on table size and configuration.

```sql
-- Configure parallel workers (optional, uses server defaults otherwise)
SET max_parallel_maintenance_workers = 4;
SET maintenance_work_mem = '256MB';  -- At least 64MB required for parallel builds

-- Create index (parallel workers used automatically for large tables)
CREATE INDEX docs_idx ON documents USING bm25(content) WITH (text_config='english');
```

**Note:** The planner requires `maintenance_work_mem >= 64MB` to enable parallel index
builds. With insufficient memory, builds fall back to serial mode silently.

You'll see a notice when parallel build is used:
```
NOTICE:  parallel index build: launched 4 of 4 requested workers
```

For partitioned tables, each partition builds its index independently with parallel
workers if the partition is large enough. This allows efficient indexing of very
large partitioned datasets.

### Performance Tuning

#### Force-merging segments

The index stores data in multiple segments across levels (similar to an LSM
tree). After bulk loads or sustained incremental inserts, multiple segments
may accumulate; consolidating them into one improves query speed by reducing
the number of segments scanned:

```sql
SELECT bm25_force_merge('docs_idx');
```

This is analogous to Lucene's `forceMerge(1)`. It rewrites all segments into
a single segment and reclaims the freed pages. Best used after large batch
inserts, not during ongoing write traffic.

#### Use LIMIT with ORDER BY

Top-k queries (`ORDER BY ... LIMIT n`) enable Block-Max WAND optimization,
which skips blocks of postings that cannot contribute to the top results.
Without a LIMIT clause, the index falls back to scoring all matching
documents up to `pg_textsearch.default_limit`.

```sql
-- Fast: BMW skips non-competitive blocks
SELECT * FROM documents ORDER BY content <@> 'search terms' LIMIT 10;

-- Slower: scores up to default_limit documents
SELECT * FROM documents ORDER BY content <@> 'search terms';
```

#### Segment compression

Compression is on by default and generally improves both index size and query
performance (fewer pages to read). Disable only if you observe that
decompression overhead is a bottleneck for your workload:

```sql
SET pg_textsearch.compress_segments = off;
```

#### Postgres settings that affect index builds

Setting | Effect
--- | ---
`max_parallel_maintenance_workers` | Number of parallel workers for CREATE INDEX (default 2)
`maintenance_work_mem` | Memory per worker; must be >= 64MB for parallel builds

#### pg_textsearch GUCs

Setting | Default | Description
--- | --- | ---
`pg_textsearch.default_limit` | 1000 | Max documents scored when no LIMIT clause is present
`pg_textsearch.compress_segments` | on | Compress posting blocks in new segments
`pg_textsearch.segments_per_level` | 8 | Segments per level before automatic compaction (2-64)
`pg_textsearch.memory_limit` | 2GB | Cap on shared memory used by memtables (0 = disable)
`pg_textsearch.bulk_load_threshold` | 100000 | Terms per transaction before auto-spill (0 = disable)
`pg_textsearch.memtable_spill_threshold` | 32000000 | **Deprecated.** Posting entries before auto-spill (0 = disable)

> **`memtable_spill_threshold` is deprecated.** It was the original
> mechanism for bounding memtable growth, triggering a spill when an
> index accumulated a fixed number of posting entries. The newer
> `memory_limit` GUC replaces it with byte-level estimation that
> accounts for term overhead and works across indexes. Both checks
> are evaluated (OR'd), so existing configurations continue to work.
> New deployments should use `memory_limit` only.

#### Memory management

pg_textsearch keeps in-memory inverted indexes (memtables) in Postgres
dynamic shared memory (DSA). Without a limit, heavy write workloads can
grow DSA until the OS OOM killer terminates the server.

`memory_limit` caps this growth. It is the maximum amount of DSA memory
the extension will use for memtables. When usage approaches this cap,
the extension automatically spills memtables to on-disk segments. If
usage still exceeds the cap, inserts fail with an ERROR rather than
risking an OOM kill.

Internally, three thresholds are derived from `memory_limit`:

| Threshold | Value | What happens |
| --- | --- | --- |
| Per-index soft limit | `memory_limit / 8` | Spills that index's memtable to a disk segment |
| Global soft limit | `memory_limit / 2` | Evicts the largest memtable across all indexes |
| Hard limit | `memory_limit` | Rejects the insert with an ERROR |

During normal operation, the soft limits keep usage well below the hard
cap. You only need to set `memory_limit`; the internal ratios are tuned
for typical workloads.

```sql
-- Tune for a smaller instance (e.g., 4 GB RAM)
ALTER SYSTEM SET pg_textsearch.memory_limit = '512MB';
SELECT pg_reload_conf();

-- Tune for a larger instance (e.g., 64 GB RAM)
ALTER SYSTEM SET pg_textsearch.memory_limit = '16GB';
SELECT pg_reload_conf();
```

To check current memory usage:

```sql
SELECT * FROM bm25_memory_usage();
```

VACUUM (including autovacuum's insert-threshold path) also spills the
memtable when it runs, so the amount of un-spilled state between
`CREATE INDEX` and the next server restart stays bounded.

**Crash recovery**: The memtable is rebuilt from the heap on startup, so no
data is lost if Postgres crashes before spilling to disk.

## Monitoring

```sql
-- Check index usage
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';
```

## Examples

### Basic Search

```sql
CREATE TABLE articles (id serial PRIMARY KEY, title text, content text);
CREATE INDEX articles_idx ON articles USING bm25(content) WITH (text_config='english');

INSERT INTO articles (title, content) VALUES
    ('Database Systems', 'PostgreSQL is a powerful relational database system'),
    ('Search Technology', 'Full text search enables finding relevant documents quickly'),
    ('Information Retrieval', 'BM25 is a ranking function used in search engines');

-- Find relevant documents
SELECT title, content <@> 'database search' as score
FROM articles
ORDER BY score;
```

Also supports different languages and custom parameters:

```sql
-- Different languages
CREATE INDEX fr_idx ON french_articles USING bm25(content) WITH (text_config='french');
CREATE INDEX de_idx ON german_articles USING bm25(content) WITH (text_config='german');

-- Custom parameters
CREATE INDEX custom_idx ON documents USING bm25(content)
    WITH (text_config='english', k1=2.0, b=0.9);
```


## Limitations

### No Phrase Queries

The BM25 index stores term frequencies but not term positions, so it cannot
natively evaluate phrase queries like `"database system"`. You can emulate
phrase matching by combining BM25 ranking with a post-filter:

```sql
-- BM25 ranks candidates; subquery over-fetches to account for
-- post-filter eliminating non-phrase matches
SELECT * FROM (
    SELECT *, content <@> 'database system' AS score
    FROM documents
    ORDER BY score
    LIMIT 100  -- over-fetch
) sub
WHERE content ILIKE '%database system%'
ORDER BY score
LIMIT 10;
```

Because the post-filter eliminates some results, the inner LIMIT should
be larger than the desired result count.

### No Built-in Faceted Search

pg_textsearch does not provide dedicated faceting operators, but standard
Postgres query machinery handles common faceting patterns:

```sql
-- Filter by category (assumes a B-tree index on category)
SELECT * FROM documents
WHERE category = 'engineering'
ORDER BY content <@> 'search terms'
LIMIT 10;

-- Compute facet counts over top search results
SELECT category, count(*)
FROM (
    SELECT category FROM documents
    ORDER BY content <@> 'search terms'
    LIMIT 100
) matches
GROUP BY category;
```

### Insert/Update Performance

The memtable architecture is designed to support efficient writes, but
sustained write-heavy workloads are not yet fully optimized. For initial
data loading, creating the index after loading data is faster than
incremental inserts. This is an active area of development.

### No Background Compaction

Segment compaction currently runs synchronously during memtable spill
operations. Write-heavy workloads may observe compaction latency during
spills. Background compaction is planned for a future release.

### Partitioned Tables

BM25 indexes on partitioned tables use **partition-local statistics**. Each
partition maintains its own:
- Document count (`total_docs`)
- Average document length (`avg_doc_len`)
- Per-term document frequencies for IDF calculation

This means:
- Queries targeting a single partition compute accurate BM25 scores using that
  partition's statistics
- Queries spanning multiple partitions return scores computed independently per
  partition, which may not be directly comparable across partitions

**Example**: If partition A has 1000 documents and partition B has 10 documents,
the term "database" would have different IDF values in each partition. Results
from both partitions would have scores on different scales.

**Recommendations**:
- For time-partitioned data, query individual partitions when score comparability
  matters
- Use partitioning schemes where queries naturally target single partitions
- Consider this behavior when designing partition strategies for search workloads

```sql
-- Query single partition (scores are accurate within partition)
SELECT * FROM docs
WHERE created_at >= '2024-01-01' AND created_at < '2025-01-01'
ORDER BY content <@> 'search terms'
LIMIT 10;

-- Cross-partition query (scores computed per-partition)
SELECT * FROM docs
ORDER BY content <@> 'search terms'
LIMIT 10;
```

### Word Length Limit

pg_textsearch inherits PostgreSQL's tsvector word length limit of 2047 characters.
Words exceeding this limit are ignored during tokenization (with an INFO message).
This is defined by `MAXSTRLEN` in PostgreSQL's text search implementation.

For typical natural language text, this limit is never encountered. It may affect
documents containing very long tokens such as base64-encoded data, long URLs, or
concatenated identifiers.

This behavior is similar to other search engines:
- Elasticsearch: Truncates tokens (configurable via `truncate` filter, default 10 chars)
- Tantivy: Truncates to 255 bytes by default

### PL/pgSQL and Stored Procedures

The implicit `text <@> 'query'` syntax relies on planner hooks to automatically
detect the BM25 index. These hooks don't run inside PL/pgSQL DO blocks, functions,
or stored procedures.

**Inside PL/pgSQL**, use explicit index names with `to_bm25query()`:

```sql
-- This won't work in PL/pgSQL:
-- SELECT * FROM docs ORDER BY content <@> 'search terms' LIMIT 10;

-- Use explicit index name instead:
SELECT * FROM docs
ORDER BY content <@> to_bm25query('search terms', 'docs_idx')
LIMIT 10;
```

Regular SQL queries (outside PL/pgSQL) support both forms.

## Troubleshooting

```sql
-- List available text search configurations
SELECT cfgname FROM pg_ts_config;

-- List BM25 indexes
SELECT indexname FROM pg_indexes WHERE indexdef LIKE '%USING bm25%';
```


## Installation Notes

If your machine has multiple Postgres installations, specify the path to `pg_config`:

```sh
export PG_CONFIG=/Library/PostgreSQL/18/bin/pg_config  # or 17
make clean && make && make install
```

If you get compilation errors, install Postgres development files:

```sh
# Ubuntu/Debian
sudo apt install postgresql-server-dev-17  # for PostgreSQL 17
sudo apt install postgresql-server-dev-18  # for PostgreSQL 18
```

## Reference

### Index Options

Option | Type | Default | Description
--- | --- | --- | ---
text_config | string | required | PostgreSQL text search configuration to use
k1 | real | 1.2 | Term frequency saturation parameter (0.1 to 10.0)
b | real | 0.75 | Length normalization parameter (0.0 to 1.0)

### Text Search Configurations

Available configurations depend on your Postgres installation:
```
# SELECT cfgname FROM pg_ts_config;
  cfgname
------------
 simple
 arabic
 armenian
 basque
 catalan
 danish
 dutch
 english
 finnish
 french
 german
 greek
 hindi
 hungarian
 indonesian
 irish
 italian
 lithuanian
 nepali
 norwegian
 portuguese
 romanian
 russian
 serbian
 spanish
 swedish
 tamil
 turkish
 yiddish
(29 rows)
```
Further language support is available via extensions such as [zhparser](https://github.com/amutu/zhparser).

### Development Functions

These functions are for debugging and development use only. Their interface may
change in future releases without notice. Functions marked with † require
superuser privileges.

Function | Description
--- | ---
bm25_force_merge(index_name) → void | Merge all segments into one (improves query speed)
bm25_spill_index(index_name) → int4 | Force memtable spill to disk segment
bm25_dump_index(index_name) † → text | Dump internal index structure (truncated)
bm25_summarize_index(index_name) † → text | Show index statistics without content

Additional file-writing debug functions (`bm25_dump_index(text, text)` and
`bm25_debug_pageviz`) are available in debug builds only (compile with
`-DDEBUG_DUMP_INDEX`).

```sql
-- Merge all segments into one (best after bulk loads)
SELECT bm25_force_merge('docs_idx');

-- Force spill to disk (returns number of entries spilled)
SELECT bm25_spill_index('docs_idx');

-- Quick overview of index statistics
SELECT bm25_summarize_index('docs_idx');

-- Detailed dump for debugging (truncated output)
SELECT bm25_dump_index('docs_idx');
```

## Extension Compatibility

pg_textsearch uses fixed LWLock tranche IDs 1001-1008 to support large numbers
of indexes (e.g., partitioned tables with hundreds of partitions). If you use
another Postgres extension that also registers fixed tranche IDs in this range,
wait event names in `pg_stat_activity` may be incorrect. Core Postgres tranches
use IDs below 100. If you encounter a conflict, please
[open an issue](https://github.com/timescale/pg_textsearch/issues).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, code style, and
how to submit pull requests.

- **Bug Reports**: [Create an issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **Feature Requests**: [Request a feature](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **General Discussion**: [Start a discussion](https://github.com/timescale/pg_textsearch/discussions)
