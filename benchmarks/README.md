# pg_textsearch Benchmarks

Performance benchmarks for the pg_textsearch BM25 full-text search extension.

## Quick Start

```bash
# Run Cranfield benchmark (quick validation, ~1400 docs)
./runner/run_benchmark.sh cranfield

# Run MS MARCO benchmark (8.8M passages) - requires download
./runner/run_benchmark.sh msmarco --download --load --query

# Run Wikipedia benchmark
./runner/run_benchmark.sh wikipedia --download --load --query

# Run all benchmarks
./runner/run_benchmark.sh all
```

## Datasets

### Cranfield Collection (Quick Validation)
- **Size:** 1,400 aerodynamics abstracts, 225 queries
- **Purpose:** Quick validation of BM25 correctness and basic performance
- **Time:** ~1-2 minutes total

### MS MARCO Passage Ranking (Full Scale)
- **Size:** 8.8 million passages, 6,980 dev queries with relevance judgments
- **Purpose:** Large-scale performance benchmarking, search quality evaluation
- **Source:** [Microsoft MS MARCO](https://microsoft.github.io/msmarco/)
- **Download:** ~2GB compressed
- **Time:** Index build may take 30+ minutes depending on hardware

### Wikipedia (Real-World Content)
- **Size:** Configurable (10K, 100K, 1M, or full ~6M articles)
- **Purpose:** Real-world document lengths and vocabulary
- **Source:** [Wikimedia Dumps](https://dumps.wikimedia.org/)
- **Time:** Varies significantly by size

## Benchmark Runner

The main runner script is `runner/run_benchmark.sh`:

```bash
./runner/run_benchmark.sh [dataset] [options]

# Datasets:
#   msmarco     - MS MARCO Passage Ranking (8.8M passages)
#   wikipedia   - Wikipedia articles
#   cranfield   - Cranfield collection (1,400 docs)
#   all         - Run all benchmarks

# Options:
#   --download  - Download dataset if not present
#   --load      - Load data and create index (drops existing)
#   --query     - Run query benchmarks only
#   --report    - Generate markdown report
#   --port PORT - Postgres port (default: 5433 for release build)
```

## Running Benchmarks

### Prerequisites

1. PostgreSQL with pg_textsearch installed
2. For Wikipedia: `pip install wikiextractor`
3. For best results, use a release build of Postgres (port 5433)

### Download Data

```bash
# MS MARCO
cd datasets/msmarco && ./download.sh

# Wikipedia (full)
cd datasets/wikipedia && ./download.sh full

# Wikipedia (subset for testing)
cd datasets/wikipedia && ./download.sh 100K
```

### Load and Index

```bash
# Using psql directly
psql -p 5433 -v data_dir="'$PWD/datasets/msmarco/data'" \
    -f datasets/msmarco/load.sql

# Or use the runner
./runner/run_benchmark.sh msmarco --load --port 5433
```

### Run Query Benchmarks

```bash
psql -p 5433 -f datasets/msmarco/queries.sql
```

## Metrics Collected

### Index Build
- Total time to build BM25 index
- Memory usage during index build

### Query Performance
- Single query latency (p50, p95, p99)
- Batch query throughput (QPS)
- Query latency by query type:
  - Single-word queries
  - Multi-word queries
  - Question-style queries
  - Rare term queries

### Search Quality (MS MARCO)
- MRR@10 (Mean Reciprocal Rank)
- Comparison with known relevance judgments

## CI Integration

Benchmarks run automatically:
- **On PR:** Cranfield only (quick validation)
- **Nightly:** MS MARCO subset
- **Weekly:** Full benchmark suite

See `.github/workflows/benchmark.yml` for configuration.

## Results

Historical results are stored in `results/` (gitignored).

Each run produces:
- `benchmark_[dataset]_[timestamp].md` - Markdown report
- `[dataset]_load_[timestamp].log` - Load phase logs
- `[dataset]_queries_[timestamp].log` - Query benchmark logs
- `metrics_[timestamp].env` - Machine-readable metrics

## Interpreting Results

### Index Build Time

| Dataset | Expected Time (Release Build) |
|---------|------------------------------|
| Cranfield | < 5 seconds |
| MS MARCO | 15-60 minutes |
| Wikipedia (100K) | 2-10 minutes |
| Wikipedia (full) | 1-4 hours |

### Query Latency

For a well-tuned system with sufficient memory:
- Top-10 queries: < 100ms
- Batch throughput: 10-100 QPS (depending on query complexity)

### Memory Requirements

- Cranfield: < 64MB
- MS MARCO: 1-4GB (depending on index memory limit)
- Wikipedia: 512MB-8GB (depending on size)

## Adding New Benchmarks

To add a new dataset:

1. Create directory: `datasets/[name]/`
2. Add scripts:
   - `download.sh` - Download and prepare data
   - `load.sql` - Load data and create index
   - `queries.sql` - Query benchmarks
3. Update this README

## Comparison with Other Systems

For competitive benchmarks, you can run the same queries against:
- Native Postgres `ts_rank` (built-in full-text search)
- ParadeDB `pg_search` (Tantivy-based)
- External systems (Elasticsearch, Meilisearch)

See `datasets/[name]/queries_native.sql` for native Postgres equivalents.
