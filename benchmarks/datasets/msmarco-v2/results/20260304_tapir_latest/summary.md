# MS-MARCO v2 Benchmark Results (138M passages)

Date: 2026-03-10
Commit: main @ fb3b3b1
Machine: c6i.4xlarge (Intel Xeon Platinum 8375C, 8C/16T, 123 GB RAM, NVMe)
Postgres: 17.7, shared_buffers=31 GB

## Index Build

| Metric | pg_textsearch | System X (ParadeDB v0.21.6) |
|--------|---------------|----------------------------|
| Build time | 1,057,461 ms (17:37) | 535,169 ms (8:55) |
| Workers launched | 15 of 15 requested | 14 |
| Index size | 17 GB (18,153,144,320 bytes) | 23 GB (24,876,695,552 bytes) |
| Documents | 138,364,158 | 138,364,158 |
| Unique terms | 17,373,764 | - |
| Avg doc length | 29.34 lexemes | - |
| Segments | 1 (L0, 2,214,872 pages) | - |

## Single-Client Query Latency (top-10, LIMIT 10)

691 queries: 100 per bucket 1-6, 53 for bucket 7, 38 for bucket 8+.

### pg_textsearch

| Tokens | p50 (ms) | p95 (ms) | p99 (ms) | avg (ms) | n |
|--------|----------|----------|----------|----------|---|
| 1 | 5.11 | 6.43 | 7.05 | 5.34 | 100 |
| 2 | 9.14 | 32.63 | 56.22 | 12.94 | 100 |
| 3 | 20.04 | 51.51 | 57.25 | 24.11 | 100 |
| 4 | 41.92 | 124.17 | 153.56 | 49.33 | 100 |
| 5 | 67.76 | 167.05 | 239.87 | 77.08 | 100 |
| 6 | 102.82 | 262.07 | 321.54 | 115.90 | 100 |
| 7 | 159.37 | 311.58 | 439.19 | 163.22 | 53 |
| 8+ | 177.95 | 404.95 | 421.80 | 185.09 | 38 |

Weighted p50: 40.61 ms | Weighted avg: 46.69 ms

### System X (ParadeDB v0.21.6)

| Tokens | p50 (ms) | p95 (ms) | p99 (ms) | avg (ms) | n |
|--------|----------|----------|----------|----------|---|
| 1 | 59.83 | 68.34 | 90.48 | 61.45 | 100 |
| 2 | 59.65 | 103.17 | 141.36 | 66.37 | 100 |
| 3 | 77.62 | 114.79 | 136.13 | 82.86 | 100 |
| 4 | 98.89 | 147.32 | 177.70 | 106.97 | 100 |
| 5 | 125.38 | 190.07 | 250.79 | 135.40 | 100 |
| 6 | 148.78 | 201.76 | 244.19 | 156.34 | 100 |
| 7 | 169.65 | 291.09 | 334.75 | 184.45 | 53 |
| 8+ | 190.47 | 310.68 | 428.87 | 206.74 | 38 |

Weighted p50: 94.36 ms | Weighted avg: 101.66 ms

## Single-Client Throughput (3 iterations, median)

| Metric | pg_textsearch | System X |
|--------|---------------|----------|
| Total (691 queries) | 43,477 ms | 73,613 ms |
| Avg ms/query | 62.92 | 106.53 |
| Min ms/query | 62.84 | 105.50 |
| Max ms/query | 62.92 | 106.89 |

## Concurrent Throughput (pgbench, 16 clients, 60 s)

| Metric | pg_textsearch | System X |
|--------|---------------|----------|
| TPS | 91.43 | 19.43 |
| Avg latency | 174.995 ms | 823.351 ms |
| Transactions | 5,526 | 1,180 |
| Failed | 0 | 0 |

## Comparison Summary

| Metric | pg_textsearch | System X | Ratio |
|--------|---------------|----------|-------|
| Weighted p50 latency | 40.61 ms | 94.36 ms | 2.3x faster |
| Weighted avg latency | 46.69 ms | 101.66 ms | 2.2x faster |
| Single-client throughput | 62.92 ms/q | 106.53 ms/q | 1.7x faster |
| Concurrent TPS (16 clients) | 91.4 | 19.4 | 4.7x higher |
| Index size | 17 GB | 23 GB | 26% smaller |
| Build time | 17:37 | 8:55 | 1.9x slower |
