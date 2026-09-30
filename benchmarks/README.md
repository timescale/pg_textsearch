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

# Measure the filtered-seed top-K optimization (synthetic, no download)
./run_filtered_seed.sh

# Measure index-build memory (synthetic, Linux, no download)
python3 run_build_memory.py
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

### Filtered-Seed Top-K (Synthetic, Self-Contained)
- **Size:** Configurable, default 200K synthetic documents
- **Purpose:** Measure `pg_textsearch.filtered_seed` on filtered top-k
  queries (`WHERE <filter> ORDER BY <score> LIMIT k`), where an
  unseeded scan pays executor backoff re-drives
- **Script:** `./run_filtered_seed.sh [ndocs]`
- **Time:** ~10 seconds at the default size; no download needed
- **Method:** Toggles the GUC over identical data and queries. Reports
  median latency and, where `bm25_debug_scoring_passes` exists, scoring
  passes. Drops cells unless every arm uses the BM25 index; result
  parity is covered separately by the `filtered_seed` regression
- **Note:** Compare only the *seed on* columns across versions. The
  *seed off* baseline is not stable across the #435 change, because the
  `LIMIT` binding became per-scan even with seeding disabled

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

## Build Memory

`run_build_memory.py` reproduces vocabulary-driven build-memory growth
with 100,000 synthetic documents. It starts a disposable local cluster
using `PG_CONFIG` (default: `pg_config`), with the **installed** extension.
It never installs binaries or connects to an existing server.
Requires Linux, Python 3, and PostgreSQL server tools; run as a non-root user.

```bash
PG_CONFIG=/path/to/pg_config python3 run_build_memory.py

# Run only the one-million-term case, or change the corpus/workers
python3 run_build_memory.py --case unique
python3 run_build_memory.py --case unique --rows 200000 --workers 4
```

The default matrix compares 1,000 repeated terms, 500K/1M/2M unique terms,
and the 1M case with a lower memory budget and with parallelism disabled.
Every document also contains one shared term for a result-count check.
Defaults are two workers and `maintenance_work_mem=64MB`; the low-budget
and serial controls use 16MB. Builds verify the actual worker count and
query results. Allow about a minute and 1.5 GiB of available memory for
the unfixed implementation. Parallel cases require at least 100K rows.

Results go into a new `results/build_memory_<timestamp>/` directory:
`summary.json` records versions, settings, timings, index sizes, sampled
memory peaks by phase, and failures; per-case CSV files contain leader
RSS/private memory, worker private memory, aggregate build PSS, and
temporary-file sizes. Logs are retained; the cluster data is removed.
PSS avoids counting shared pages multiple times. Sampling can miss brief
peaks; `--interval` controls the delay between samples (default 0.1s).

For an **intentional OOM experiment**, use a private cgroup v2 scope:

```bash
systemd-run --user --scope -p MemoryMax=384M -p MemorySwapMax=0 \
  -p OOMPolicy=continue \
  python3 run_build_memory.py --case unique --cgroup
```

This requires a systemd user manager with memory-controller delegation.
Use 512M for a higher-limit comparison. `--cgroup` records the current
scope's memory and OOM counters; these include the runner and client
processes, not just PostgreSQL. An OOM/failure returns nonzero and records
the error rather than publishing a successful result. Keep results/data
on a **disk-backed filesystem**, not tmpfs: `--output /disk/new-directory`
selects another location. Tmpfs data would itself consume the memory cap.
Benchmark failures are expected on unfixed builds under tight limits.

## Running Benchmarks

### Prerequisites

1. PostgreSQL with pg_textsearch installed
2. For Wikipedia: `pip install 'wikiextractor==3.0.6'`
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

### Run Concurrent Queries and Updates

The mixed update/query benchmark reproduces a workload with continuous
top-10 disjunction queries and one client targeting 1,000 indexed updates per
second. It runs a read-only phase followed by an identical query phase with
updates, records reader and writer throughput, and samples PostgreSQL wait
events once per second.

The defaults target the MS MARCO v2 schema and run each measured phase for ten
minutes with 32 query clients:

```bash
PGPORT=5433 \
./datasets/msmarco/mixed-update-query/run.sh
```

To run against the original 8.8 million-passage MS MARCO schema:

```bash
PGPORT=5433 \
TABLE=msmarco_passages \
INDEX=msmarco_bm25_idx \
QUERY_TABLE=msmarco_queries \
./datasets/msmarco/mixed-update-query/run.sh
```

Relation settings can be schema-qualified, for example
`TABLE=benchmarks.msmarco_passages` and
`INDEX=benchmarks.msmarco_bm25_idx`.

Use shorter phases for a smoke run:

```bash
PGPORT=5433 DURATION=30 WARMUP=10 READ_CLIENTS=4 \
UPDATE_TARGETS=1000 \
./datasets/msmarco/mixed-update-query/run.sh
```

The writer toggles a trailing `pgtsupdate` token in the selected source rows,
so run against a disposable database or restore the corpus after benchmarking.

Results are written below
`benchmarks/results/mixed-update-query/<timestamp>/`. `metadata.env` records
the resolved non-secret connection and workload settings. `summary.tsv`
contains read-only and mixed query QPS, average and p99 latency, completed
updates, update TPS, and update latency. `waits.tsv` contains sampled reader
and writer wait events, and `wait_summary.tsv` aggregates them. `warmup.log`,
`read_only.out`, `mixed_reader.out`, and `mixed_writer.out` preserve pgbench
output. The `read_only_pgbench.*`, `mixed_reader_pgbench.*`, and
`mixed_writer_pgbench.*` files are per-transaction pgbench logs.

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
- Other PostgreSQL full-text search extensions
- External systems (Elasticsearch, Meilisearch)

See `datasets/[name]/queries_native.sql` for native Postgres equivalents.
