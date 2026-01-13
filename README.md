# pg_textsearch

[![CI](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml)
[![Benchmarks](https://github.com/timescale/pg_textsearch/actions/workflows/benchmark.yml/badge.svg)](https://timescale.github.io/pg_textsearch/benchmarks/)
[![Coverity Scan](https://scan.coverity.com/projects/32822/badge.svg)](https://scan.coverity.com/projects/pg_textsearch)

Modern ranked text search for Postgres.

- Simple syntax: `ORDER BY content <@> 'search terms'`
- BM25 ranking with configurable parameters (k1, b)
- Works with Postgres text search configurations (english, french, german, etc.)
- Fast top-k queries via Block-Max WAND optimization

⚠️ **Pre-release**: v0.4.0 - GA expected Feb 2026. Query performance is competitive with other leading Postgres-based solutions; see [benchmarks](https://timescale.github.io/pg_textsearch/benchmarks/comparison.html). This release adds index compression; parallel builds coming soon. See [ROADMAP.md](ROADMAP.md) for details.

![Tapir and Friends](images/tapir_and_friends_v0.4.0.png)

*The theme of v0.4.0 is compression, which the friends are trying to avoid.*

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

Enable the extension (do this once in each database where you want to use it)

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
WHERE content <@> to_bm25query('database system', 'docs_idx') < -1.0;
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

**Post-filtering** applies the BM25 index scan first, then filters results:
```sql
SELECT * FROM documents
WHERE content <@> to_bm25query('search terms', 'docs_idx') < -5.0
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

## Monitoring

```sql
-- Check index usage
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';
```

### Configuration

Optional settings in `postgresql.conf`:

```bash
# Query limit when no LIMIT clause detected
pg_textsearch.default_limit = 1000           # default 1000

# Auto-spill thresholds (set to 0 to disable)
pg_textsearch.bulk_load_threshold = 100000    # terms per transaction
pg_textsearch.memtable_spill_threshold = 32000000  # posting entries (~1M docs/segment)

# Compression (v0.4.0+)
pg_textsearch.compress_segments = on          # default on
```

The `memtable_spill_threshold` controls when the in-memory index spills to
disk segments. When the memtable reaches this many posting entries, it
automatically flushes to a segment at transaction commit. This keeps memory
usage bounded while maintaining good query performance.

**Crash recovery**: The memtable is rebuilt from the heap on startup, so no
data is lost if Postgres crashes before spilling to disk.

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
change in future releases without notice.

Function | Description
--- | ---
bm25_dump_index(index_name) → text | Dump internal index structure (truncated)
bm25_dump_index(index_name, file_path) → text | Dump full index structure to file
bm25_summarize_index(index_name) → text | Show index statistics without content
bm25_spill_index(index_name) → int4 | Force memtable spill to disk segment

```sql
-- Quick overview of index statistics
SELECT bm25_summarize_index('docs_idx');

-- Detailed dump for debugging (truncated output)
SELECT bm25_dump_index('docs_idx');

-- Full dump to file (includes hex data)
SELECT bm25_dump_index('docs_idx', '/tmp/docs_idx_dump.txt');

-- Force spill to disk (returns number of entries spilled)
SELECT bm25_spill_index('docs_idx');
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, code style, and
how to submit pull requests.

- **Bug Reports**: [Create an issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **Feature Requests**: [Request a feature](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **General Discussion**: [Start a discussion](https://github.com/timescale/pg_textsearch/discussions)
