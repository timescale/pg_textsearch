# ParadeDB MS-MARCO Benchmark Results

**Date:** 2026-01-02
**Purpose:** Establish baseline performance numbers for ParadeDB BM25 full-text search on MS-MARCO dataset for comparison with pg_textsearch.

## System Configuration

### Hardware
| Component | Specification |
|-----------|---------------|
| Instance Type | AWS EC2 (us-east-2) |
| Architecture | aarch64 (ARM64) |
| CPU | AWS Graviton (Neoverse-N1) |
| CPU Cores | 4 cores, 1 thread/core, 1 socket |
| RAM | 32 GB (32,446,356 KB) |
| Swap | None |
| Storage | 128 GB NVMe EBS (gp3) |
| Filesystem | XFS with noatime |

### Operating System
```
Linux 6.1.158-180.294.amzn2023.aarch64
Amazon Linux 2023 (aarch64)
```

### PostgreSQL Configuration
```
PostgreSQL 17.7 on aarch64-amazon-linux-gnu
Compiled by: gcc (GCC) 11.5.0 20240719 (Red Hat 11.5.0-5), 64-bit
Data directory: /var/lib/pgsql/data
```

#### Key Parameters (postgresql.conf)
| Parameter | Value | Notes |
|-----------|-------|-------|
| shared_buffers | 7,680 MB | 983040 * 8kB |
| work_mem | 128 MB | Per-operation sort/hash memory |
| maintenance_work_mem | 1,024 MB | For index creation |
| effective_cache_size | 22,528 MB | OS cache estimate |
| max_parallel_workers | 4 | Total parallel workers |
| max_parallel_maintenance_workers | 2 | For CREATE INDEX |
| max_parallel_workers_per_gather | 2 | For parallel queries |
| random_page_cost | 1.1 | NVMe-optimized |
| effective_io_concurrency | 200 | NVMe-optimized |
| wal_buffers | 16 MB | |
| checkpoint_completion_target | 0.9 | |
| max_connections | 100 | |
| huge_pages | try | |

### ParadeDB Version
```
pg_search 0.20.6
```

## Dataset

### MS-MARCO Passage Ranking
- **Source:** https://microsoft.github.io/msmarco/
- **Collection:** 8,841,823 passages
- **Queries:** 10,000 dev queries (subset)
- **Relevance Judgments:** 7,437 qrels

### Data Files
```
collection.tsv     2.9 GB  (passage_id, passage_text)
queries.dev.tsv    4.3 MB  (query_id, query_text)
qrels.dev.small.tsv 140 KB (query_id, 0, passage_id, relevance)
```

### Download
```bash
cd benchmarks/datasets/msmarco
./download.sh full
```

## Schema and Index

### Table Definition
```sql
CREATE TABLE msmarco_passages_paradedb (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);
```

### Index Creation
```sql
-- With English tokenizer (stop words + stemming) to match to_tsvector('english', ...)
CREATE INDEX msmarco_paradedb_idx ON msmarco_passages_paradedb
    USING bm25 (passage_id, passage_text)
    WITH (
        key_field = 'passage_id',
        text_fields = '{
            "passage_text": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }'
    );
```

### Query Syntax
```sql
-- Basic search (uses index)
SELECT passage_id
FROM msmarco_passages_paradedb
WHERE passage_text @@@ 'search terms'
ORDER BY paradedb.score(passage_id) DESC
LIMIT 10;

-- With score in output
SELECT passage_id, paradedb.score(passage_id) AS score
FROM msmarco_passages_paradedb
WHERE passage_text @@@ 'search terms'
ORDER BY score DESC
LIMIT 10;
```

## Benchmark Results

### Index Creation
| Metric | Value |
|--------|-------|
| Documents Indexed | 8,841,823 |
| Index Creation Time | **3 min 8.89 sec** (188.89 sec) |
| Index Size | **1,410 MB** |
| Table Size | 3,242 MB |
| Index/Table Ratio | 0.43x |
| Parallel Workers | 4 (max_parallel_maintenance_workers = 4) |

**Note:** Index build time was similar with 2 vs 4 parallel workers, suggesting the build is I/O-bound rather than CPU-bound on NVMe storage.

### Query Latency (Top-10 Results)

Each query was run 10 iterations; median, min, and max times reported.

| Query Type | Query Text | Median (ms) | Min (ms) | Max (ms) |
|------------|------------|-------------|----------|----------|
| Short (1 word) | "coffee" | **10.52** | 10.20 | 12.25 |
| Medium (3 words) | "how to cook" | **17.01** | 16.52 | 20.85 |
| Long (question) | "what is the capital of france" | **20.92** | 20.27 | 21.32 |
| Stop word | "the" | **9.01** | 8.74 | 9.44 |
| Rare term | "cryptocurrency blockchain" | **13.21** | 10.90 | 13.66 |

**Note:** "the" is a stop word and returns 0 results. Multi-word queries benefit from stop word filtering (e.g., "how to cook" only searches for "cook").

### Query Latency WITH SCORE in SELECT

| Query Type | Query Text | Median (ms) | Min (ms) | Max (ms) |
|------------|------------|-------------|----------|----------|
| Short (1 word) | "coffee" | **10.36** | 10.25 | 10.98 |
| Medium (3 words) | "how to cook" | **16.28** | 16.07 | 17.71 |
| Long (question) | "what is the capital of france" | **21.32** | 20.95 | 21.62 |
| Stop word | "the" | **8.93** | 8.72 | 9.56 |
| Rare term | "cryptocurrency blockchain" | **11.19** | 10.84 | 13.29 |

### Query Throughput (20 Sequential Queries)

Mixed query workload:
```
weather forecast, stock market, recipe chicken, python programming,
machine learning, climate change, health benefits, travel destinations,
movie reviews, sports scores, music history, science experiments,
cooking tips, fitness exercises, book recommendations, technology news,
financial advice, home improvement, garden plants, pet care
```

| Metric | Without Score | With Score |
|--------|---------------|------------|
| Total Time (20 queries) | 297.43 ms | 301.08 ms |
| Average per Query | **14.87 ms** | **15.05 ms** |
| Queries per Second | **67.2 QPS** | **66.4 QPS** |

## Key Observations

1. **Single-term queries are fast:** ~10-13ms regardless of term frequency
2. **Stop words are filtered:** "the", "how", "to", "is", etc. are not searched
3. **Stemming is applied:** "cooking" matches "cook", "running" matches "run"
4. **Score retrieval is essentially free:** No measurable overhead
5. **Index size is compact:** 0.43x table size (stop words not indexed)
6. **Warm queries:** After warmup, performance is consistent (low variance)

## Reproduction Steps

### 1. Install ParadeDB
```bash
# Follow ParadeDB installation instructions for your platform
# Ensure pg_search extension is available
```

### 2. Download Dataset
```bash
cd /path/to/pg_textsearch/benchmarks/datasets/msmarco
chmod +x download.sh
./download.sh full
```

### 3. Load Data and Create Index
```bash
cd /path/to/pg_textsearch/benchmarks/datasets/msmarco
DATA_DIR=$PWD/data psql -U postgres postgres -f paradedb/load.sql
```

### 4. Run Benchmarks
```bash
psql -U postgres postgres -f paradedb/queries.sql
```

## Files

| File | Description |
|------|-------------|
| `download.sh` | Downloads MS-MARCO dataset |
| `paradedb/load.sql` | Creates tables, loads data, builds ParadeDB index |
| `paradedb/queries.sql` | Runs query benchmarks |
| `PARADEDB.md` | This results file |

## MS-MARCO Dev Set Evaluation (Full Query Set)

Official MS-MARCO evaluation using all 6,980 dev queries with relevance judgments.

### Query Method
```sql
-- Using paradedb.match() for proper tokenization of natural language queries
SELECT passage_id, paradedb.score(passage_id) AS score
FROM msmarco_passages_paradedb
WHERE passage_text @@@ paradedb.match('passage_text', trim(query_text))
ORDER BY score DESC
LIMIT 10;
```

### Retrieval Quality (MRR@10)

| Metric | Value |
|--------|-------|
| Total Queries | 6,980 |
| Queries with Results | 6,980 (100%) |
| **MRR@10** | **0.1815** |
| **Recall@10** | **0.3875** (38.75%) |

**MRR@10** (Mean Reciprocal Rank at 10): For each query, if a relevant document appears in the top-10 results at rank *r*, the score is 1/*r*. Average across all queries.

**Recall@10**: Percentage of queries where at least one relevant document was found in the top-10 results.

### Query Latency (Real MS-MARCO Queries)

Measured by running 1,000 real MS-MARCO dev queries:

| Metric | Value |
|--------|-------|
| Average Latency | **25.73 ms** |
| Throughput | **38.9 queries/sec** |

Individual query examples (EXPLAIN ANALYZE):
| Query | Latency |
|-------|---------|
| "treating tension headaches without medication" (5 terms) | 23.7 ms |
| "what is paula deens brother" (4 terms) | 27.4 ms |
| "Androgen receptor define" (3 terms) | 17.0 ms |
| "coffee" (1 term) | 10.9 ms |

### Observations

1. **MRR@10 = 0.1815** is competitive with standard BM25 (Anserini reports ~0.187)
2. **Recall@10 = 38.75%** means ~39% of queries found a relevant passage in top 10
3. **Query latency scales with term count:** ~10ms for single terms, ~25ms for multi-term queries
4. **`paradedb.match()` has minimal overhead** compared to direct `@@@` syntax (~1ms difference)

### Reference: MS-MARCO Leaderboard Context

For context, published BM25 baselines on MS-MARCO Passage Ranking:
- **BM25 (Anserini):** MRR@10 ≈ 0.187
- **BM25 (tuned):** MRR@10 ≈ 0.228
- **Neural methods:** MRR@10 ≈ 0.35-0.40+

ParadeDB's MRR@10 of 0.1815 is competitive with standard BM25 implementations.

## Comparison Notes

For fair comparison with pg_textsearch:
- Use identical hardware and PostgreSQL configuration
- Use the same dataset and query workload
- Run warmup queries before benchmarking
- Report median of multiple iterations
- Test both with and without score in SELECT clause
- For MRR evaluation, use `paradedb.match()` / equivalent tokenized query method
