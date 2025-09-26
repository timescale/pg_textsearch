# Tapir

![Tapir and Friends](images/tapir_and_friends.png)

[![CI](https://github.com/timescale/tapir/actions/workflows/ci.yml/badge.svg)](https://github.com/timescale/tapir/actions/workflows/ci.yml)

**T**extual **A**nalysis for **P**ostgres **I**nformation **R**etrieval

Open-source full-text search for Postgres.  Supports:

- BM25 ranking with configurable parameters (k1, b)
- PostgreSQL text search configurations (english, french, german, etc.)
- tpquery data type for scoring operations

🚀 **Development Status**: Pre-alpha release.  Memtable-based implementation is
in place and working.

## Installation

### Linux and Mac

Compile and install the extension (requires PostgreSQL 17)

```sh
cd /tmp
git clone https://github.com/timescale/tapir
cd tapir
make
make install # may need sudo
```

## Getting Started

Enable the extension (do this once in each database where you want to use it)

```sql
CREATE EXTENSION tapir;
```

Create a table with text content

```sql
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
INSERT INTO documents (content) VALUES
    ('PostgreSQL is a powerful database system'),
    ('BM25 is an effective ranking function'),
    ('Full text search with custom scoring');
```

Create a Tapir index on the text column

```sql
CREATE INDEX docs_tapir_idx ON documents USING tapir(content) WITH (text_config='english');
```

## Querying

Get the most relevant documents using the `<@>` operator

```sql
SELECT * FROM documents ORDER BY content <@> to_tpquery('docs_tapir_idx:database system') LIMIT 5;
```

Note: `<@>` returns the negative BM25 score since Postgres only supports `ASC` order index scans on operators. Lower scores indicate better matches.

For WHERE clause queries, use tpquery with index name:
```sql
SELECT * FROM documents WHERE content <@> to_tpquery('docs_tapir_idx:database system') < -1.0;
```

Supported operations:
- `text <@> tpquery` - Score text against a query

## Indexing

Create a BM25 index on your text columns:

```sql
CREATE INDEX ON documents USING tapir(content) WITH (text_config='english');
```

### Index Options

- `text_config` - PostgreSQL text search configuration to use (required)
- `k1` - term frequency saturation parameter (1.2 by default)
- `b` - length normalization parameter (0.75 by default)

```sql
CREATE INDEX ON documents USING tapir(content) WITH (text_config='english', k1=1.5, b=0.8);
```

Also supports different text search configurations:

```sql
-- English documents with stemming
CREATE INDEX docs_en_idx ON documents USING tapir(content) WITH (text_config='english');

-- Simple text processing without stemming
CREATE INDEX docs_simple_idx ON documents USING tapir(content) WITH (text_config='simple');

-- Language-specific configurations
CREATE INDEX docs_fr_idx ON french_docs USING tapir(content) WITH (text_config='french');
CREATE INDEX docs_de_idx ON german_docs USING tapir(content) WITH (text_config='german');
```

## Data Types

### tpquery

The `tpquery` type represents queries for BM25 scoring with optional index context:

```sql
-- Create a tpquery with index name for WHERE clause queries
SELECT to_tpquery('docs_tapir_idx:search query text');
-- Returns: docs_tapir_idx:search query text

-- Create a tpquery without index name (for ORDER BY)
SELECT to_tpquery('search query text');
-- Returns: search query text
```

#### tpquery Functions

Function | Description
--- | ---
to_tpquery(text) → tpquery | Create tpquery from text
text <@> tpquery → double precision | BM25 scoring operator
tpquery = tpquery → boolean | Equality comparison


## Functions

Function | Description
--- | ---
to_tpquery(text) → tpquery | Create tpquery from text
text <@> tpquery → double precision | BM25 scoring operator

## Performance

Tapir indexes use a memtable architecture for efficient writes. Like other index types, it's faster to create an index after loading your data.

```sql
-- Load data first
INSERT INTO documents (content) VALUES (...);

-- Then create index
CREATE INDEX docs_tapir_idx ON documents USING tapir(content) WITH (text_config='english');
```

## Monitoring

```sql
-- Check index usage
SELECT schemaname, tablename, indexname, idx_scan, idx_tup_read, idx_tup_fetch
FROM pg_stat_user_indexes WHERE indexname LIKE '%tapir%';

-- Memory pool statistics
SELECT * FROM tp_pool_stats();

-- Per-index memory usage
SELECT indexname, tp_index_memory_usage(indexrelid) as memory_usage_mb
FROM pg_stat_user_indexes
WHERE indexname LIKE '%tapir%';

-- Debug index internal structure
SELECT tp_debug_dump_index('index_name');
```

### Configuration

Optional settings in `postgresql.conf`:

```bash
# Per-index memory limit (can be changed without restart)
tapir.shared_memory_size = 64MB      # Memory limit per index, default 64MB

# Query limit when no LIMIT clause detected
tapir.default_limit = 1000           # default 1000
```

## Examples

### Basic Search

```sql
CREATE TABLE articles (id serial PRIMARY KEY, title text, content text);
CREATE INDEX articles_tapir_idx ON articles USING tapir(content) WITH (text_config='english');

INSERT INTO articles (title, content) VALUES
    ('Database Systems', 'PostgreSQL is a powerful relational database system'),
    ('Search Technology', 'Full text search enables finding relevant documents quickly'),
    ('Information Retrieval', 'BM25 is a ranking function used in search engines');

-- Find relevant documents
SELECT title, content <@> to_tpquery('articles_tapir_idx:database search') as score
FROM articles
ORDER BY content <@> to_tpquery('articles_tapir_idx:database search');
```

Also supports different languages and custom parameters:

```sql
-- Different languages
CREATE INDEX fr_idx ON french_articles USING tapir(content) WITH (text_config='french');
CREATE INDEX de_idx ON german_articles USING tapir(content) WITH (text_config='german');

-- Custom parameters
CREATE INDEX custom_idx ON documents USING tapir(content)
    WITH (text_config='english', k1=2.0, b=0.9);
```


## Troubleshooting

```sql
-- List available text search configurations
SELECT cfgname FROM pg_ts_config;

-- List BM25 indexes
SELECT indexname FROM pg_indexes WHERE indexdef LIKE '%USING tapir%';
```


## Installation Notes

If your machine has multiple Postgres installations, specify the path to `pg_config`:

```sh
export PG_CONFIG=/Library/PostgreSQL/17/bin/pg_config
make clean && make && make install
```

If you get compilation errors, install Postgres development files:

```sh
# Ubuntu/Debian
sudo apt install postgresql-server-dev-17
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

## Development

### Building and Testing

```sh
make                   # build extension
make install           # install to PostgreSQL
make installcheck      # run regression tests
make test-concurrency  # run concurrency stress tests
test/scripts/recovery.sh    # run crash recovery tests

# Code formatting
make format            # format all source files with clang-format
make lint-format       # check formatting without changing files

# Install pre-commit hooks (optional)
brew install pre-commit && pre-commit install  # macOS
# pip install pre-commit && pre-commit install  # Linux
```

### Code Style

- 79-character line limit, tab indentation
- Opening braces on new lines (Allman style)
- Consecutive alignment for variables and macros
- 2 spaces before trailing comments
- Multiline function prototypes followed by blank line
- PostgreSQL naming conventions (snake_case)
- Format with: `make format`

## Bug Reports & Feature Requests

- **Bug Reports**: [Create an issue](https://github.com/timescale/tapir/issues/new?labels=bug&template=bug_report.md)
- **Feature Requests**: [Request a feature](https://github.com/timescale/tapir/issues/new?labels=enhancement&template=feature_request.md)
- **General Discussion**: [Start a discussion](https://github.com/timescale/tapir/discussions)

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make changes with tests
4. Ensure `make installcheck` and `make test-concurrency` pass
5. Submit a pull request

All pull requests are automatically tested against PostgreSQL 17.

