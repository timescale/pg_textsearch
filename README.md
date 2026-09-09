![pg_textsearch](images/banner.png)

[![CI](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/pg_textsearch/actions/workflows/ci.yml)
[![Benchmarks](https://github.com/timescale/pg_textsearch/actions/workflows/benchmark.yml/badge.svg)](https://timescale.github.io/pg_textsearch/benchmarks/)
[![Coverity Scan](https://scan.coverity.com/projects/32822/badge.svg)](https://scan.coverity.com/projects/pg_textsearch)

Modern ranked text search for Postgres.

- Simple syntax: `ORDER BY content <@> 'search terms'`
- BM25 ranking with configurable `k1` and `b`
- PostgreSQL text search configurations
- Expression, partial, and partitioned indexes
- Fast top-k queries with Block-Max WAND
- Parallel index builds for large tables

## Installation

pg_textsearch supports PostgreSQL 17 and 18.

### Pre-built Binaries

Download pre-built binaries from the
[Releases page](https://github.com/timescale/pg_textsearch/releases).
Available for Linux and macOS (amd64 and arm64).

### Build from Source

```sh
cd /tmp
git clone https://github.com/timescale/pg_textsearch
cd pg_textsearch
make
make install # may need sudo
```

## Getting Started

Add pg_textsearch to `shared_preload_libraries` in `postgresql.conf`, then
restart the server:

```
shared_preload_libraries = 'pg_textsearch'  # add to existing list if needed
```

Enable the extension in each database:

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

Get the most relevant documents

```sql
SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

`<@>` returns negative BM25 scores for ascending index scans, so lower scores
rank first.

The index is detected from the column. Specify it explicitly when needed:

```sql
SELECT * FROM documents
ORDER BY content <@> to_bm25query('database system', 'docs_idx')
LIMIT 5;
```

### Verifying Index Usage

```sql
EXPLAIN SELECT * FROM documents
ORDER BY content <@> 'database system'
LIMIT 5;
```

PostgreSQL may prefer a sequential scan for small tables. To test the index
plan:

```sql
SET enable_seqscan = off;
```

Standalone scoring still uses index corpus statistics even when the table is
read sequentially.

### Filtering with WHERE Clauses

PostgreSQL can use a separate index to filter rows before BM25 scoring:

```sql
CREATE INDEX ON documents (category_id);

SELECT * FROM documents
WHERE category_id = 123
ORDER BY content <@> 'search terms'
LIMIT 10;
```

Conditions without a usable index are applied after the BM25 scan:

```sql
SELECT * FROM documents
WHERE length(content) > 100
ORDER BY content <@> 'search terms'
LIMIT 10;
```

Post-filtering can return fewer rows than the requested `LIMIT`. Over-fetch
and re-limit when the condition removes many results.

## Indexing

Create a BM25 index on a text column:

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english');
```

### Index Options

Option | Default | Description
--- | --- | ---
`text_config` | required | PostgreSQL text search configuration
`k1` | 1.2 | Term frequency saturation
`b` | 0.75 | Length normalization
`compaction` | inline | Spill-time compaction: `inline`, `background`, or `off`; see [Background Compaction](#background-compaction)

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english', k1=1.5, b=0.8);
```

Use any installed PostgreSQL text search configuration:

```sql
CREATE INDEX docs_en_idx ON documents USING bm25(content) WITH (text_config='english');
```

### Expression Indexes

Index expressions for JSONB fields, multiple columns, or text transformations:

```sql
-- JSONB field extraction
CREATE INDEX events_expr_idx ON events USING bm25 ((data->>'description'))
    WITH (text_config='english');

SELECT * FROM events
ORDER BY (data->>'description') <@> to_bm25query('network error', 'events_expr_idx')
LIMIT 10;

-- Multi-column search
CREATE INDEX ON articles USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');
```

The expression must evaluate to `text` and use only IMMUTABLE functions.
Queries must repeat the same expression in the `ORDER BY` clause.

### Partial Indexes

Add a `WHERE` clause to index a subset of rows:

```sql
CREATE INDEX docs_content_idx ON docs USING bm25 (content)
    WITH (text_config='english')
    WHERE status = 'published';

SELECT * FROM docs
WHERE status = 'published'
ORDER BY content <@> to_bm25query('search terms', 'docs_content_idx')
LIMIT 10;
```

Partial indexes require explicit index naming via `to_bm25query()` — the
implicit `text <@> 'query'` syntax skips them.

### Multilingual Tables

Create one partial index per language:

```sql
ALTER TABLE docs ADD COLUMN lang CHAR(2) NOT NULL DEFAULT 'en';

CREATE INDEX docs_en_idx ON docs USING bm25 (content)
    WITH (text_config='english') WHERE lang = 'en';
CREATE INDEX docs_de_idx ON docs USING bm25 (content)
    WITH (text_config='german')  WHERE lang = 'de';
CREATE INDEX docs_fr_idx ON docs USING bm25 (content)
    WITH (text_config='french')  WHERE lang = 'fr';
```

Query with the matching predicate and index name:

```sql
SELECT * FROM docs
WHERE lang = 'en'
ORDER BY content <@> to_bm25query('databases', 'docs_en_idx')
LIMIT 10;
```

## Explicit Queries

`bm25query` can carry an explicit index name:

```sql
SELECT to_bm25query('search query text', 'docs_idx');

SELECT 'docs_idx:search query text'::bm25query;
```

Explicit index names are required for partial indexes, PL/pgSQL, and
standalone scoring. Standalone scoring requires `SELECT` on the indexed table
or columns.

### Functions

Function | Description
--- | ---
to_bm25query(text) → bm25query | Create bm25query without index name (for ORDER BY only)
to_bm25query(text, text) → bm25query | Create bm25query with query text and index name
text <@> bm25query → double precision | BM25 scoring operator (returns negative scores)
bm25query = bm25query → boolean | Equality comparison

## Performance

For initial loads, create the index after loading data.

### Parallel Index Builds

PostgreSQL uses parallel workers automatically for sufficiently large tables.

```sql
SET max_parallel_maintenance_workers = 4;
SET maintenance_work_mem = '256MB';
```

Parallel builds require at least 64MB of `maintenance_work_mem`; otherwise they
fall back to serial builds. Partitioned tables build each partition separately.

### Query Performance

Use `ORDER BY ... LIMIT n` to enable Block-Max WAND. Without `LIMIT`, the index
scores up to `pg_textsearch.default_limit` matching documents.

```sql
SELECT * FROM documents ORDER BY content <@> 'search terms' LIMIT 10;
```

Segment compression is enabled by default. Disable it only when decompression
is a measured bottleneck:

```sql
SET pg_textsearch.compress_segments = off;
```

Update-heavy workloads can fragment index pages. Use `REINDEX` during a
low-traffic window if cold-cache latency degrades:

```sql
REINDEX INDEX docs_idx;
```

### Compaction

Compaction runs automatically during memtable spills. These functions provide
manual and scheduled control:

```sql
SELECT bm25_force_merge('docs_idx');
SELECT bm25_compact('docs_idx'::regclass);
SELECT bm25_compact_step('docs_idx'::regclass);
SELECT bm25_needs_compaction('docs_idx'::regclass);
SELECT bm25_level_counts('docs_idx'::regclass);
```

`bm25_force_merge()` reduces the index to the fewest segments allowed by
`pg_textsearch.max_segment_size`. `bm25_compact()` processes all eligible
levels; `bm25_compact_step()` processes at most one pass.

- Published passes are not undone by `ROLLBACK`.
- Mutating compaction functions are not cancellable while a pass runs.
- Drive maintenance loops from `bm25_compact_step()`'s return value, not
  `bm25_needs_compaction()`, which is advisory.
- Mutating functions require index ownership and do not operate on partitioned
  parent indexes or during recovery.

See [docs/background_compaction.md](docs/background_compaction.md) for sizing,
publication, locking, and page-reclaim details.

### Settings

Setting | Default | Description
--- | --- | ---
`pg_textsearch.default_limit` | 1000 | Max documents scored when no LIMIT clause is present
`pg_textsearch.compress_segments` | on | Compress posting blocks in new segments
`pg_textsearch.segments_per_level` | 8 | Segments per level before automatic compaction (2-64)
`pg_textsearch.max_segment_size` | 4095MB | Conservative size budget for newly merged multi-source segments (1-4095MB)
`pg_textsearch.compaction_request_function` | (empty) | Schema-qualified name of a function taking one `regclass`, invoked for indexes set to `compaction = 'background'`
`pg_textsearch.bulk_load_threshold` | 100000 | Terms per transaction before auto-spill (0 = disable)
`pg_textsearch.memtable_pages_threshold` | 64 | Chain pages before auto-spill (0 = disable)
`pg_textsearch.memtable_cache_enabled` | on | Cache memtable data in shared memory for faster queries
`pg_textsearch.memory_limit` | 2GB | Shared memory limit for memtable caches across all indexes (0 = no limit)

### Memtable Architecture

The L0 memtable is stored in the index as a WAL-logged chain of pages. It is
the durable source of truth and can be restored by PostgreSQL without loading
`pg_textsearch.so`. See [`docs/memtable_v2.md`](docs/memtable_v2.md).

Queries use a shared-memory cache when enabled. The cache is rebuilt from the
chain when missing or stale, while standbys read the chain directly. See
[`docs/memtable_cache.md`](docs/memtable_cache.md).

Memtables spill automatically based on `memtable_pages_threshold` and
`bulk_load_threshold`, and during VACUUM.

```sql
-- Manual spill
SELECT bm25_spill_index('docs_idx');
```

## Monitoring

```sql
-- Check index usage
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes
WHERE indexrelid::regclass::text ~ 'pg_textsearch';
```

## Limitations

### Phrase Queries

The BM25 index stores term frequencies but not term positions, so it cannot
evaluate phrases directly. Over-fetch ranked candidates and apply a
post-filter:

```sql
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

### Background Compaction

pg_textsearch does not include a background worker. Compaction defaults to
`inline`; two per-index alternatives are available:

- `background` calls `pg_textsearch.compaction_request_function` at
  pre-commit. The callback must hand work to something that survives its
  rolled-back internal subtransaction; a plain table insert does not.
- `off` requires an external job to call `bm25_compact()` or
  `bm25_compact_step()`; without one, segments accumulate and spills
  eventually fail.

Change the policy with `ALTER INDEX ... SET (compaction = ...)`.
`background` falls back to inline compaction for temporary indexes,
autovacuum, callback-triggered spills, and `CREATE INDEX`. Prepared
transactions do not dispatch requests.

See [docs/background_compaction.md](docs/background_compaction.md).

### Partitioned Tables

BM25 statistics are local to each partition. Scores are comparable within a
partition but may use different IDF scales across partitions. Query individual
partitions when cross-row score comparability matters.

### Token and Document Limits

PostgreSQL ignores tokens beyond its 2047-character text-search limit. This
mainly affects base64 data, long URLs, and concatenated identifiers.

Documents that exceed PostgreSQL's 1MB `tsvector` lexeme limit are tokenized
in 256KB chunks. For large non-whitespace-delimited documents, use a `text[]`
column to control chunk boundaries and a language-aware text search
configuration such as [zhparser](https://github.com/amutu/zhparser).

### PL/pgSQL and Stored Procedures

Planner hooks do not resolve the implicit query syntax inside PL/pgSQL. Use an
explicit index name:

```sql
SELECT * FROM docs
ORDER BY content <@> to_bm25query('search terms', 'docs_idx')
LIMIT 10;
```

## Troubleshooting

List installed text search configurations:

```sql
SELECT cfgname FROM pg_ts_config;
```

List BM25 indexes:

```sql
SELECT indexname FROM pg_indexes WHERE indexdef LIKE '%USING bm25%';
```

For multiple PostgreSQL installations, set `PG_CONFIG` before building:

```sh
export PG_CONFIG=/Library/PostgreSQL/18/bin/pg_config  # or 17
make clean && make && make install
```

Compilation requires PostgreSQL development files:

```sh
sudo apt install postgresql-server-dev-18  # use 17 for PostgreSQL 17
```

## Reference

### Chinese Full-Text Search

Use a Chinese-aware PostgreSQL text search configuration such as
[zhparser](https://github.com/amutu/zhparser). The same `text_config` tokenizes
documents and queries.

```sql
CREATE EXTENSION zhparser;

CREATE TEXT SEARCH CONFIGURATION public.chinese (PARSER = zhparser);
ALTER TEXT SEARCH CONFIGURATION public.chinese
    ADD MAPPING FOR n, v, a, i, e, l WITH simple;

CREATE INDEX docs_bm25 ON docs USING bm25 (content)
    WITH (text_config='public.chinese');

SELECT id FROM docs
ORDER BY content <@> to_bm25query('机器学习', 'docs_bm25')
LIMIT 10;
```

### Compaction Functions

These functions take an index `regclass`. Mutating functions require index
ownership. See [Compaction](#compaction) before scripting them.

Function | Description
--- | ---
bm25_level_counts(index) → int4[] | Segments held at each of the eight LSM levels
bm25_needs_compaction(index) → bool | Whether any level reached `segments_per_level` (advisory)
bm25_compact(index) → void | Run eligible compaction passes
bm25_compact_step(index) → bool | Run at most one pass

### Development Functions

These interfaces may change without notice. Functions marked with † require
superuser privileges.

Function | Description
--- | ---
bm25_force_merge(index_name) → void | Merge into the fewest size-bounded segments
bm25_spill_index(index_name) → int4 | Force memtable spill to disk segment
bm25_pending_free_pages(index_name) † → int8 | Count pages awaiting standby-safe reclaim
bm25_dump_index(index_name) † → text | Dump internal index structure (truncated)
bm25_summarize_index(index_name) † → text | Show index statistics without content

Additional file-writing debug functions (`bm25_dump_index(text, text)` and
`bm25_debug_pageviz`) are available in debug builds only (compile with
`-DDEBUG_DUMP_INDEX`).

## Extension Compatibility

pg_textsearch uses LWLock tranche IDs 1001-1008. Another extension using the
same IDs can cause incorrect wait-event names in `pg_stat_activity`. If you
encounter a conflict,
[open an issue](https://github.com/timescale/pg_textsearch/issues).

## Project History

pg_textsearch was originally named Tapir (Textual Analysis for Postgres
Information Retrieval), which remains the project mascot and appears in some
source names.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, code style, and
how to submit pull requests.

- **Bug Reports**: [Create an issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **Feature Requests**: [Request a feature](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **General Discussion**: [Start a discussion](https://github.com/timescale/pg_textsearch/discussions)
