# Filtered-seed approach comparison — 2026-09-17

Compared committed branch HEAD `d20852ed` (executor hook and scan-identity registry) with the current uncommitted query-carried hint implementation. PostgreSQL 18.4, 200,000 documents, identical data and ANALYZE statistics, dedicated local server. Existing `benchmarks/sql/filtered_seed.sql` workload; setup executed once and measurements repeated without dropping data. Each cell is the median of seven timed executions after warmup. Order: hint, original, original, hint, with server restarts when switching binaries. Seeding enabled in the table.

| Shape | Selectivity | LIMIT | Original run 1 / 2 (ms) | Hint run 1 / 2 (ms) | Passes, both |
| --- | --- | --- | --- | --- | --- |
| single | 0.001 | 10 | 9.30 / 9.38 | 9.27 / 9.32 | 1 |
| single | 0.001 | 100 | 27.47 / 27.28 | 27.90 / 27.31 | 1 |
| single | 0.01 | 10 | 2.41 / 2.43 | 2.46 / 2.43 | 1 |
| single | 0.1 | 10 | 1.74 / 1.74 | 1.67 / 1.65 | 1 |
| union2 | 0.001 | 10 | 18.81 / 19.08 | 18.99 / 18.81 | 2 |
| union3 | 0.001 | 10 | 28.07 / 28.04 | 28.03 / 29.78 | 3 |
| union3 | mixed | 10 | 61.38 / 61.66 | 62.20 / 64.87 | 5 |

All seven cases retained identical scoring-pass counts. These sequential local measurements show similar performance for the tested constant-query, constant-LIMIT shapes; they do not establish a speed advantage. The second hint run slowed on the largest multi-scan cases. This benchmark does not exercise generic prepared parameters, plan copying, or intervening plan nodes, and does not resolve the two outstanding static-review findings.

The dedicated server was stopped after completion and the working-tree query-hint binary restored to the local PostgreSQL installation. No source fixes, commits, pushes, or PR replies were made.

Raw benchmark outputs:

## base-1

```text
FSB_RESULT: shape=single sel=0.001 limit=10 scans=1 off_ms=18.82 off_passes=10 on_ms=9.30 on_passes=1 speedup=2.02
FSB_RESULT: shape=single sel=0.001 limit=100 scans=1 off_ms=46.30 off_passes=10 on_ms=27.47 on_passes=1 speedup=1.69
FSB_RESULT: shape=single sel=0.01 limit=10 scans=1 off_ms=13.49 off_passes=8 on_ms=2.41 on_passes=1 speedup=5.60
FSB_RESULT: shape=single sel=0.1 limit=10 scans=1 off_ms=1.69 off_passes=1 on_ms=1.74 on_passes=1 speedup=0.97
FSB_RESULT: shape=union2 sel=0.001 limit=10 scans=2 off_ms=37.96 off_passes=20 on_ms=18.81 on_passes=2 speedup=2.02
FSB_RESULT: shape=union3 sel=0.001 limit=10 scans=3 off_ms=57.55 off_passes=30 on_ms=28.07 on_passes=3 speedup=2.05
FSB_RESULT: shape=union3 sel=mixed limit=10 scans=3 off_ms=85.35 off_passes=23 on_ms=61.38 on_passes=5 speedup=1.39
```

## base-2

```text
FSB_RESULT: shape=single sel=0.001 limit=10 scans=1 off_ms=19.08 off_passes=10 on_ms=9.38 on_passes=1 speedup=2.03
FSB_RESULT: shape=single sel=0.001 limit=100 scans=1 off_ms=46.78 off_passes=10 on_ms=27.28 on_passes=1 speedup=1.71
FSB_RESULT: shape=single sel=0.01 limit=10 scans=1 off_ms=13.82 off_passes=8 on_ms=2.43 on_passes=1 speedup=5.69
FSB_RESULT: shape=single sel=0.1 limit=10 scans=1 off_ms=1.68 off_passes=1 on_ms=1.74 on_passes=1 speedup=0.97
FSB_RESULT: shape=union2 sel=0.001 limit=10 scans=2 off_ms=38.02 off_passes=20 on_ms=19.08 on_passes=2 speedup=1.99
FSB_RESULT: shape=union3 sel=0.001 limit=10 scans=3 off_ms=57.55 off_passes=30 on_ms=28.04 on_passes=3 speedup=2.05
FSB_RESULT: shape=union3 sel=mixed limit=10 scans=3 off_ms=85.28 off_passes=23 on_ms=61.66 on_passes=5 speedup=1.38
```

## hint-1

```text
FSB_RESULT: shape=single sel=0.001 limit=10 scans=1 off_ms=18.92 off_passes=10 on_ms=9.27 on_passes=1 speedup=2.04
FSB_RESULT: shape=single sel=0.001 limit=100 scans=1 off_ms=46.84 off_passes=10 on_ms=27.90 on_passes=1 speedup=1.68
FSB_RESULT: shape=single sel=0.01 limit=10 scans=1 off_ms=13.83 off_passes=8 on_ms=2.46 on_passes=1 speedup=5.62
FSB_RESULT: shape=single sel=0.1 limit=10 scans=1 off_ms=1.62 off_passes=1 on_ms=1.67 on_passes=1 speedup=0.97
FSB_RESULT: shape=union2 sel=0.001 limit=10 scans=2 off_ms=38.17 off_passes=20 on_ms=18.99 on_passes=2 speedup=2.01
FSB_RESULT: shape=union3 sel=0.001 limit=10 scans=3 off_ms=57.29 off_passes=30 on_ms=28.03 on_passes=3 speedup=2.04
FSB_RESULT: shape=union3 sel=mixed limit=10 scans=3 off_ms=85.35 off_passes=23 on_ms=62.20 on_passes=5 speedup=1.37
```

## hint-2

```text
FSB_RESULT: shape=single sel=0.001 limit=10 scans=1 off_ms=19.11 off_passes=10 on_ms=9.32 on_passes=1 speedup=2.05
FSB_RESULT: shape=single sel=0.001 limit=100 scans=1 off_ms=47.07 off_passes=10 on_ms=27.31 on_passes=1 speedup=1.72
FSB_RESULT: shape=single sel=0.01 limit=10 scans=1 off_ms=13.65 off_passes=8 on_ms=2.43 on_passes=1 speedup=5.62
FSB_RESULT: shape=single sel=0.1 limit=10 scans=1 off_ms=1.58 off_passes=1 on_ms=1.65 on_passes=1 speedup=0.96
FSB_RESULT: shape=union2 sel=0.001 limit=10 scans=2 off_ms=38.10 off_passes=20 on_ms=18.81 on_passes=2 speedup=2.03
FSB_RESULT: shape=union3 sel=0.001 limit=10 scans=3 off_ms=57.64 off_passes=30 on_ms=29.78 on_passes=3 speedup=1.94
FSB_RESULT: shape=union3 sel=mixed limit=10 scans=3 off_ms=89.09 off_passes=23 on_ms=64.87 on_passes=5 speedup=1.37
```
