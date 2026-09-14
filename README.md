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

### Pre-built Packages

Download pre-built binaries from the
[Releases page](https://github.com/timescale/pg_textsearch/releases).
Available for Linux on amd64 and arm64.

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
CREATE TABLE documents (
    id bigserial PRIMARY KEY,
    content text,
    category_id integer
);
INSERT INTO documents (content, category_id) VALUES
    ('PostgreSQL is a powerful database system', 1),
    ('BM25 is an effective ranking function', 1),
    ('Full text search with custom scoring', 2);
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

`<@>` returns negative BM25 scores for ascending index scans, so lower scores
rank first.

`<@>` can also score rows outside a BM25 index scan. This is standalone
scoring; it still uses corpus statistics from the selected BM25 index.

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

PostgreSQL may prefer standalone scoring with a sequential scan for small
tables. To test the index plan:

```sql
SET enable_seqscan = off;
```

### Pre-filtering and Post-filtering

PostgreSQL can use a separate index to pre-filter rows before standalone
scoring:

```sql
CREATE INDEX ON documents (category_id);

SELECT * FROM documents
WHERE category_id = 1
ORDER BY content <@> 'search terms'
LIMIT 10;
```

When PostgreSQL chooses the ordered BM25 index scan, other conditions are
post-filters applied after BM25 scoring:

```sql
SELECT * FROM documents
WHERE length(content) > 100
ORDER BY content <@> 'search terms'
LIMIT 10;
```

Post-filtered scans automatically grow their internal scoring batch until the
`LIMIT` is filled, matches are exhausted, or the 100,000-result scan cap is
reached.

## Indexing

Create a BM25 index on a text column:

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english');
```

### Index Options

Option | Default | Description
--- | --- | ---
[`text_config`](https://www.postgresql.org/docs/current/textsearch-configuration.html) | required | PostgreSQL text search configuration
`k1` | 1.2 | Term frequency saturation (0.1-10.0)
`b` | 0.75 | Length normalization (0.0-1.0)
`compaction` | inline | Spill-time compaction: `inline`, `background`, or `off`; see [Background Compaction](#background-compaction)

```sql
CREATE INDEX ON documents USING bm25(content) WITH (text_config='english', k1=1.5, b=0.8);
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

-- Text transformation
CREATE INDEX ON documents USING bm25 ((lower(content)))
    WITH (text_config='simple');

-- Multi-column search
CREATE INDEX ON articles USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');
```

The expression must evaluate to `text` and use only IMMUTABLE functions.
Queries must repeat the same expression in the `ORDER BY` clause.

### Partial Indexes

Add a `WHERE` clause to index a subset of rows:

```sql
CREATE INDEX documents_category_bm25_idx ON documents USING bm25 (content)
    WITH (text_config='english')
    WHERE category_id = 1;

SELECT * FROM documents
WHERE category_id = 1
ORDER BY content <@> to_bm25query('search terms', 'documents_category_bm25_idx')
LIMIT 10;
```

Partial indexes require explicit index naming via `to_bm25query()` — the
implicit `text <@> 'query'` syntax skips them.

### Multilingual Tables

Create one partial index per language:

```sql
ALTER TABLE documents ADD COLUMN lang CHAR(2) NOT NULL DEFAULT 'en';

CREATE INDEX docs_en_idx ON documents USING bm25 (content)
    WITH (text_config='english') WHERE lang = 'en';
CREATE INDEX docs_de_idx ON documents USING bm25 (content)
    WITH (text_config='german')  WHERE lang = 'de';
CREATE INDEX docs_fr_idx ON documents USING bm25 (content)
    WITH (text_config='french')  WHERE lang = 'fr';
```

Query with the matching predicate and index name:

```sql
SELECT * FROM documents
WHERE lang = 'en'
ORDER BY content <@> to_bm25query('databases', 'docs_en_idx')
LIMIT 10;
```

#### Chinese Full-Text Search

Use a Chinese-aware PostgreSQL text search configuration such as
[zhparser](https://github.com/amutu/zhparser). The same `text_config` tokenizes
documents and queries.

```sql
CREATE EXTENSION zhparser;

CREATE TEXT SEARCH CONFIGURATION public.chinese (PARSER = zhparser);
ALTER TEXT SEARCH CONFIGURATION public.chinese
    ADD MAPPING FOR n, v, a, i, e, l WITH simple;

CREATE TABLE chinese_documents (id bigserial PRIMARY KEY, content text);
INSERT INTO chinese_documents (content) VALUES ('机器学习');

CREATE INDEX chinese_documents_bm25 ON chinese_documents USING bm25 (content)
    WITH (text_config='public.chinese');

SELECT id FROM chinese_documents
ORDER BY content <@> to_bm25query('机器学习', 'chinese_documents_bm25')
LIMIT 10;
```

## Explicit Queries

`bm25query` can carry an explicit index name:

```sql
SELECT to_bm25query('search query text', 'docs_idx');

SELECT 'docs_idx:search query text'::bm25query;
```

Explicit index names are required when the planner cannot infer an index,
including partial indexes, PL/pgSQL, prepared or dynamic query text, and
ambiguous expressions. Standalone scoring requires `SELECT` on the indexed
table or columns.

### Functions

Function | Description
--- | ---
to_bm25query(text) → bm25query | Create bm25query without explicit index context
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

Each parallel worker receives at least a 64MB internal build budget, so size
memory for the worker count. Partitioned tables build each partition
separately.

### Query Performance

When PostgreSQL chooses a BM25 index scan for `ORDER BY`, scoring uses
[Block-Max WAND](https://research.engineering.nyu.edu/~suel/papers/bmw.pdf).
`LIMIT n` requests the top-k result count. For filtered queries, selectivity
seeding may start with a deeper internal scoring batch. Without a pushed-down
SQL `LIMIT`, `pg_textsearch.default_limit` sets the initial scoring batch,
which can grow as more rows are requested.

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

With the default `inline` policy, compaction of levels that reach the configured
threshold occurs as part of the write transaction that triggers the spill.
These functions provide manual and scheduled control:

```sql
SELECT bm25_force_merge('docs_idx');
SELECT bm25_compact('docs_idx'::regclass);
SELECT bm25_compact_step('docs_idx'::regclass);
SELECT bm25_needs_compaction('docs_idx'::regclass);
SELECT bm25_level_counts('docs_idx'::regclass);
```

`bm25_force_merge()` runs one bounded best-effort pass over eligible adjacent
segments; oversized or otherwise uncombinable segments may remain.
`bm25_compact()` processes all eligible levels, while `bm25_compact_step()`
processes at most one pass.

- Long merge work checks for cancellation, but published replacements remain
  physical and are not undone by `ROLLBACK`.
- Drive maintenance loops from `bm25_compact_step()`'s return value, not
  `bm25_needs_compaction()`, which is advisory.
- Mutating functions require index ownership and do not operate on partitioned
  parent indexes or during recovery.

See [ARCHITECTURE.md](ARCHITECTURE.md#spill-and-compaction) for sizing,
publication, locking, and page-reclaim details.

Hot standbys serving queries must set `hot_standby_feedback = on` so active
snapshots delay physical page reuse on the primary.

### Settings

Setting | Default | Description
--- | --- | ---
`pg_textsearch.default_limit` | 1000 | Initial scoring batch when no SQL LIMIT is available
`pg_textsearch.compress_segments` | on | Compress posting blocks in new segments
`pg_textsearch.segments_per_level` | 8 | Segments per level before automatic compaction (2-64)
`pg_textsearch.max_segment_size` | 4095MB | Conservative size budget for newly merged multi-source segments (1-4095MB)
`pg_textsearch.compaction_request_function` | (empty) | Schema-qualified name of a function taking one `regclass`, invoked for indexes set to `compaction = 'background'`
`pg_textsearch.bulk_load_threshold` | 100000 | Terms per transaction before auto-spill (0 = disable)
`pg_textsearch.memtable_pages_threshold` | 64 | Chain pages before auto-spill (0 = disable)
`pg_textsearch.memtable_cache_enabled` | on | Cache memtable data in shared memory for faster queries
`pg_textsearch.memory_limit` | 2GB | Approximate shared-memory budget for the memtable cache across all indexes; changes take effect after a configuration reload without a restart (0 = no limit)

### Memtable Architecture

The L0 memtable is stored in the index as a WAL-logged chain of pages. It is
the durable source of truth and can be restored by PostgreSQL without loading
`pg_textsearch.so`. See [ARCHITECTURE.md](ARCHITECTURE.md#storage-and-wal).

Queries use a shared-memory cache when enabled. An index falls back to the
chain when the next record's estimated growth would cross
`memory_limit / 8`; global pressure triggers best-effort eviction at
`memory_limit / 2`. The global `memory_limit` is an approximate admission
threshold: incremental catch-up and cold builds fall back when the entry-time
estimate is already at the limit, but admitted or concurrent work may take
estimated usage above it. The cache is rebuilt from the chain when missing or
stale, while standbys always read the chain directly.

Memtables spill automatically based on `memtable_pages_threshold` and
`bulk_load_threshold`, and during VACUUM.

```sql
-- Manual spill
SELECT bm25_spill_index('docs_idx');
```

## Monitoring

```sql
-- Check index usage
SELECT s.schemaname, s.relname AS tablename,
       s.indexrelname AS indexname,
       s.idx_scan, s.idx_tup_read, s.idx_tup_fetch
FROM pg_stat_user_indexes AS s
JOIN pg_class AS i ON i.oid = s.indexrelid
JOIN pg_am AS am ON am.oid = i.relam
WHERE am.amname = 'bm25';
```

## Limitations

### Phrase Queries

<!-- TODO: Revisit this workaround after https://github.com/timescale/pg_textsearch/pull/480 merges. -->

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

pg_textsearch does not include a background worker. `background` dispatches
threshold debt at pre-commit, while `off` performs no automatic compaction:

Guidance for scheduling background compaction with `pg_durable` will be added
in a future update.

- `background` calls `pg_textsearch.compaction_request_function` at
  pre-commit. The callback must hand work to something that survives its
  rolled-back internal subtransaction; a plain table insert does not.
- `off` requires an external job to call `bm25_compact()` or
  `bm25_compact_step()`; without one, segments accumulate and spills
  eventually fail.

Change the policy with `ALTER INDEX ... SET (compaction = ...)`.
`background` falls back to inline compaction for temporary indexes,
autovacuum, callback-triggered spills, and `CREATE INDEX`. Prepared
transactions do not flush queued requests. Unconfigured, unresolvable, or
failed callbacks do not fall back inline; the compaction debt remains for a
later spill or explicit maintenance.

See [ARCHITECTURE.md](ARCHITECTURE.md#spill-and-compaction).

### Partitioned Tables

BM25 statistics are local to each partition. Scores are comparable within a
partition but may use different IDF scales across partitions. Query individual
partitions when cross-row score comparability matters.

### Token and Document Limits

PostgreSQL ignores lexemes longer than 2047 bytes. This mainly affects base64
data, long URLs, and concatenated identifiers.

pg_textsearch splits raw inputs larger than 256KB at whitespace or
multibyte-character boundaries before tokenization, then merges the term
frequencies. Text arrays are flattened with spaces before this processing and
do not preserve element boundaries.

### PL/pgSQL and Stored Procedures

Planner hooks do not resolve the implicit query syntax inside PL/pgSQL. Use an
explicit index name:

```sql
SELECT * FROM documents
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
bm25_force_merge(index_name) → void | Run one bounded best-effort merge pass
bm25_spill_index(index_name) → int4 | Force memtable spill to disk segment
bm25_pending_free_pages(index_name) † → int8 | Count pages awaiting standby-safe reclaim
bm25_dump_index(index_name) † → text | Dump internal index structure (truncated)
bm25_summarize_index(index_name) † → text | Show index statistics without content

## Extension Compatibility

pg_textsearch uses LWLock tranche IDs 1001-1012. Another extension using the
same IDs can cause incorrect wait-event names in `pg_stat_activity`. If you
encounter a conflict,
[open an issue](https://github.com/timescale/pg_textsearch/issues).

## Version History

Version | Highlights
--- | ---
[`v1.4.0`](https://github.com/timescale/pg_textsearch/releases/tag/v1.4.0) | Faster filtered top-k queries, Chinese search, and large-corpus improvements
[`v1.3.1`](https://github.com/timescale/pg_textsearch/releases/tag/v1.3.1) | Standby-safe page reclaim and concurrency, VACUUM, and parallel-build fixes
[`v1.3.0`](https://github.com/timescale/pg_textsearch/releases/tag/v1.3.0) | On-disk memtable with a shared-memory read cache and stateless WAL replay
[`v1.2.0`](https://github.com/timescale/pg_textsearch/releases/tag/v1.2.0) | Physical replication and update-heavy workload correctness
[`v1.1.0`](https://github.com/timescale/pg_textsearch/releases/tag/v1.1.0) | Concurrent writes, alive-bitset VACUUM, memory controls, arrays, expression indexes, and partial indexes
[`v1.0.0`](https://github.com/timescale/pg_textsearch/releases/tag/v1.0.0) | First generally available release
[`v0.6.1`](https://github.com/timescale/pg_textsearch/releases/tag/v0.6.1) | Transaction-abort, VACUUM, large-posting, and BMW stability
[`v0.6.0`](https://github.com/timescale/pg_textsearch/releases/tag/v0.6.0) | Large-scale performance, lower memory use, and required shared preloading
[`v0.5.1`](https://github.com/timescale/pg_textsearch/releases/tag/v0.5.1) | Concurrent index builds, iterative scans, progress reporting, and scan statistics
[`v0.5.0`](https://github.com/timescale/pg_textsearch/releases/tag/v0.5.0) | Parallel index builds and improved hypertable support
[`v0.4.2`](https://github.com/timescale/pg_textsearch/releases/tag/v0.4.2) | Hypertable scoring fix
[`v0.4.1`](https://github.com/timescale/pg_textsearch/releases/tag/v0.4.1) | Hypertable scan locking fix
[`v0.4.0`](https://github.com/timescale/pg_textsearch/releases/tag/v0.4.0) | Compressed posting lists, ordered-scan planner support, and partitioned-table fixes
[`v0.3.0`](https://github.com/timescale/pg_textsearch/releases/tag/v0.3.0) | Block-Max WAND, bounded memtable growth, and expanded performance testing
[`v0.2.0`](https://github.com/timescale/pg_textsearch/releases/tag/v0.2.0) | Public benchmarks, block-based segment storage, and a dynamic index registry
[`v0.1.0`](https://github.com/timescale/pg_textsearch/releases/tag/v0.1.0) | First open-source release, simpler query syntax, and partitioned tables
[`v0.0.5`](https://github.com/timescale/pg_textsearch/releases/tag/v0.0.5) | Pre-release
[`v0.0.4`](https://github.com/timescale/pg_textsearch/releases/tag/v0.0.4) | BM25 indexing, PostgreSQL 17/18 support, crash recovery, and memory budgets
[`v0.0.3`](https://github.com/timescale/pg_textsearch/releases/tag/v0.0.3) | PostgreSQL 18 support
[`v0.0.2`](https://github.com/timescale/pg_textsearch/releases/tag/v0.0.2) | Schema insertion fixes and an inheritance safety check
[`v0.0.1`](https://github.com/timescale/pg_textsearch/releases/tag/v0.0.1) | Initial release

See [GitHub Releases](https://github.com/timescale/pg_textsearch/releases) for
complete release notes.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and contribution
guidelines. See [ARCHITECTURE.md](ARCHITECTURE.md) for implementation details
and storage invariants.

- **Bug Reports**: [Create an issue](https://github.com/timescale/pg_textsearch/issues/new?labels=bug&template=bug_report.md)
- **Feature Requests**: [Request a feature](https://github.com/timescale/pg_textsearch/issues/new?labels=enhancement&template=feature_request.md)
- **General Discussion**: [Start a discussion](https://github.com/timescale/pg_textsearch/discussions)
