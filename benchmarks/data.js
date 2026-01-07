window.BENCHMARK_DATA = {
  "lastUpdate": 1767828864239,
  "repoUrl": "https://github.com/timescale/pg_textsearch",
  "entries": {
    "msmarco Benchmarks": [
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@tigerdata.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@tigerdata.com"
          },
          "id": "e59eecc100b3ed695d90f71de5837183b836ba75",
          "message": "Update step summaries for new token-bucketed metrics\n\nBoth ParadeDB and pg_textsearch job summaries now show:\n- Latency by token count (1-8+ tokens, p50)\n- Throughput (avg ms/query)\n\nEasy side-by-side comparison in GitHub Actions run summary.\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-05T05:20:02Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e59eecc100b3ed695d90f71de5837183b836ba75"
        },
        "date": 1767590768669,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 536942.638,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 5.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 29.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 40.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 57.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 112.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 133.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 212.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 380.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (avg ms/query)",
            "value": 155.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.12,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@tigerdata.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@tigerdata.com"
          },
          "id": "e59eecc100b3ed695d90f71de5837183b836ba75",
          "message": "Update step summaries for new token-bucketed metrics\n\nBoth ParadeDB and pg_textsearch job summaries now show:\n- Latency by token count (1-8+ tokens, p50)\n- Throughput (avg ms/query)\n\nEasy side-by-side comparison in GitHub Actions run summary.\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-05T05:20:02Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e59eecc100b3ed695d90f71de5837183b836ba75"
        },
        "date": 1767592377065,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 533194.991,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 5.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 27.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 41.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 89.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 108.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 146.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 317.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (avg ms/query)",
            "value": 126.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "9aea7f2a200fda30b653e4dd6d4f25d03f52c937",
          "message": "Add competitive benchmarks (#106)\n\n## Summary\n\n- Add System X benchmark job to run in parallel with pg_textsearch\nbenchmarks\n- Add `dry_run` option to skip publishing to GitHub Pages (for testing)\n- Set parallel workers dynamically based on CPU count for fair\ncomparison\n- Improve throughput benchmark methodology (warmup, multiple iterations,\nconsistent SQL)\n\n## Changes\n\n### Workflow\n- `dry_run` input parameter to test without publishing\n\n### Benchmark methodology\n- Throughput tests now include warmup pass\n- 10 iterations with median reporting (matching individual query tests)\n- Use `EXECUTE` instead of `PERFORM` for consistency\n- Fixes #95 by using subset of real MS-MARCO dev queries",
          "timestamp": "2026-01-06T00:49:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9aea7f2a200fda30b653e4dd6d4f25d03f52c937"
        },
        "date": 1767661928645,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 524011.123,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 4.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 28.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 40.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 50.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 99.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 113.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 172.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 305.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 130.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "a0eb255abc6bc57552bd3d53b9f738b234493c29",
          "message": "Fix expected output to match updated bmw.sql comment\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T00:28:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a0eb255abc6bc57552bd3d53b9f738b234493c29"
        },
        "date": 1767665002403,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 513293.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 10.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 11.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 13.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 19.27,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 26.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 35.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 48.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 117.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 48.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "8828e633e464ddd754f6afe188862ded05d706ff",
          "message": "Remove BMW_MAX_TERMS limit - always use BMW for multi-term queries\n\nBenchmarks show BMW outperforms exhaustive scoring even for 8+ term\nqueries. The previous assumption that \"exhaustive wins beyond 8 terms\"\nwas incorrect - bucket 8 queries were 2.7x slower than System X due\nto falling back to exhaustive scanning.\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T02:24:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8828e633e464ddd754f6afe188862ded05d706ff"
        },
        "date": 1767667589014,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 547286.175,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 9.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 11.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 13.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 17.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 26.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 37.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 50.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 121.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 53.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "9aea7f2a200fda30b653e4dd6d4f25d03f52c937",
          "message": "Add competitive benchmarks (#106)\n\n## Summary\n\n- Add System X benchmark job to run in parallel with pg_textsearch\nbenchmarks\n- Add `dry_run` option to skip publishing to GitHub Pages (for testing)\n- Set parallel workers dynamically based on CPU count for fair\ncomparison\n- Improve throughput benchmark methodology (warmup, multiple iterations,\nconsistent SQL)\n\n## Changes\n\n### Workflow\n- `dry_run` input parameter to test without publishing\n\n### Benchmark methodology\n- Throughput tests now include warmup pass\n- 10 iterations with median reporting (matching individual query tests)\n- Use `EXECUTE` instead of `PERFORM` for consistency\n- Fixes #95 by using subset of real MS-MARCO dev queries",
          "timestamp": "2026-01-06T00:49:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9aea7f2a200fda30b653e4dd6d4f25d03f52c937"
        },
        "date": 1767680961127,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 523119.843,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 4.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 28.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 40.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 50.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 85.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 109.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 162.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 277.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 132.2,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "c3c4fe35ea13da8feb79a71ae846b18059e89f77",
          "message": "Fix wand test: use to_bm25query for explicit index binding\n\nBare bm25query casts don't work reliably when BMW is disabled.",
          "timestamp": "2026-01-06T19:12:27Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c3c4fe35ea13da8feb79a71ae846b18059e89f77"
        },
        "date": 1767731840197,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 535181.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 10.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 12.54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 15.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 19.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 25.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 32.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 42.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 63.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 30.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "3ba39721d1e1e26938e1ff9efc7e5958a49dedd8",
          "message": "Add tokenizer_catalog and bm25_catalog to search_path",
          "timestamp": "2026-01-07T00:17:46Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3ba39721d1e1e26938e1ff9efc7e5958a49dedd8"
        },
        "date": 1767748003712,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 528030.288,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 5.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 34.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 49.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 67.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 125.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 160.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 234.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 358.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 137.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "fa9f4cdd2130d551952e99092990ac6d92249ec8",
          "message": "Add aggressive disk cleanup for System Y benchmark",
          "timestamp": "2026-01-07T02:01:49Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fa9f4cdd2130d551952e99092990ac6d92249ec8"
        },
        "date": 1767752805897,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 550620.576,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 5.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 33.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 66.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 75.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 163.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 190.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 228.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 392.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 185.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.14,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "c98d408124b045eb066c88c78a696a96cbcc2872",
          "message": "Delete source data after loading to free disk space for tokenization",
          "timestamp": "2026-01-07T03:22:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c98d408124b045eb066c88c78a696a96cbcc2872"
        },
        "date": 1767757472109,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 531259.266,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 5.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 28.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 39.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 53.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 104.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 137.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 183.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 319.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 133.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "87f6ae4508cd1f1d0037ca92ff012ff1e4eecd47",
          "message": "Add Block-Max WAND (BMW) optimization for top-k queries (#102)\n\n## Summary\n\n- Implement Block-Max WAND (BMW) for top-k retrieval using block-level\nupper bounds in V2 segments\n- Add top-k min-heap with O(1) threshold access and O(log k) updates for\nefficient result tracking\n- BMW fast path for single-term queries (skip blocks that can't\ncontribute to top-k)\n- BMW fast path for multi-term queries with WAND-style doc-ID ordered\ntraversal\n- Batch doc_freq lookups to reduce segment open/close overhead for\nmulti-term queries\n- GUC variables `pg_textsearch.enable_bmw` and `log_bmw_stats` for\ndebugging/benchmarking\n\n## Performance (MS MARCO 8.8M docs, p50 latency)\n\n| Query Length | pg_textsearch | System X | Result |\n|--------------|---------------|----------|--------|\n| 1 token | 10.14ms | 18.05ms | **1.8x faster** |\n| 2 tokens | 12.54ms | 17.24ms | **1.4x faster** |\n| 3 tokens | 15.14ms | 22.94ms | **1.5x faster** |\n| 4 tokens | 19.72ms | 24.01ms | **1.2x faster** |\n| 5 tokens | 25.79ms | 26.21ms | ~same |\n| 6 tokens | 32.88ms | 33.47ms | ~same |\n| 7 tokens | 42.09ms | 32.23ms | 1.3x slower |\n| 8+ tokens | 63.41ms | 39.17ms | 1.6x slower |\n\n**Cranfield:** 225 queries in 57ms (0.26 ms/query avg)\n\n*Note: These benchmarks establish baselines for pg_textsearch\u2014not a\nhead-to-head comparison. System X has different defaults and tuning\noptions; further iteration on configurations required.*\n\n## Implementation Details\n\n**New files:**\n- `src/query/bmw.h` - Top-k heap, BMW stats, and scoring function\ninterfaces\n- `src/query/bmw.c` - Min-heap implementation, block max score\ncomputation, single-term and multi-term BMW scoring\n\n**Key algorithm:**\n1. Compute block max BM25 score from skip entry metadata\n(`block_max_tf`, `block_max_norm`)\n2. Only score blocks where `block_max_score >= current_threshold`\n3. Update threshold as better results are found\n4. Memtable scored exhaustively (no skip index)\n\n**Multi-term optimization:**\n- Sort terms by IDF (highest first) for faster threshold convergence\n- WAND-style doc-ID ordered traversal across terms' posting lists\n- Batch doc_freq lookups: opens each segment once instead of once per\nterm\n- Reduces segment opens from O(terms \u00d7 segments) to O(segments)\n\n## Testing\n\n- All regression tests pass\n- Shell-based tests pass (concurrency, recovery, segment)\n- Results match exhaustive scoring path with correct tie-breaking\n\n---------\n\nCo-authored-by: Claude <noreply@anthropic.com>",
          "timestamp": "2026-01-07T04:07:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/87f6ae4508cd1f1d0037ca92ff012ff1e4eecd47"
        },
        "date": 1767760058252,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 554541.187,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 10.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 12.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 15.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 21.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 26.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 32.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 44.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 64.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 32.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "67160fc91c9b962dc729a65e65fd65703f485b02",
          "message": "Add CLAUDE.md to repository (#112)\n\n## Summary\n\n- Adds CLAUDE.md to the repository (previously gitignored)\n- Updates all sections to reflect current codebase state at v0.3.0-dev\n- Documents reorganized source structure with subdirectories\n- Updates GUC parameters and configuration options\n- Documents Block-Max WAND (BMW) query optimization\n- Adds all current test files and CI workflows\n\n## Key Changes\n\n**Source Structure**: Documented the reorganized codebase with:\n- `src/am/` - Access method implementation\n- `src/memtable/` - In-memory index structures\n- `src/segment/` - Disk-based segment storage\n- `src/query/` - Query execution and BMW optimization\n- `src/types/` - SQL data types\n- `src/state/` - Index state management\n- `src/planner/` - Query planner integration\n- `src/debug/` - Debugging utilities\n\n**Configuration**: Updated GUC parameters:\n- `pg_textsearch.default_limit` (was index_memory_limit)\n- `pg_textsearch.enable_bmw` (new)\n- `pg_textsearch.log_bmw_stats` (new)\n- `pg_textsearch.bulk_load_threshold` (new)\n- `pg_textsearch.memtable_spill_threshold` (new)\n- `pg_textsearch.segments_per_level` (new)\n\n**Documentation**: Added sections for:\n- Block-Max WAND optimization\n- All CI workflows (benchmark.yml, nightly-stress.yml,\nupgrade-tests.yml, etc.)\n- New benchmark datasets (MS MARCO, Wikipedia)\n- Debug functions (bm25_dump_index, bm25_summarize_index)\n\n## Testing\n\nN/A - documentation only",
          "timestamp": "2026-01-07T05:22:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/67160fc91c9b962dc729a65e65fd65703f485b02"
        },
        "date": 1767766975855,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 542047.914,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 9.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 16.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 20.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 27.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 31.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 44.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 63.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 30.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "fe487553ce8f11c1ffe0c7876259de05bb29b4da",
          "message": "Fix local memtable issues from code review\n\n- Use separate key_len variable in get_or_create_posting() to preserve\n  original term_len for the posting while truncating for hash lookup\n- Fix total_len accumulation in tp_local_memtable_store_doc_length() to\n  handle document length updates correctly",
          "timestamp": "2026-01-07T23:29:43Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fe487553ce8f11c1ffe0c7876259de05bb29b4da"
        },
        "date": 1767828863680,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 538304.216,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 15.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 19.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 23.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 29.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 34.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 47.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 69.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 34.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      }
    ],
    "cranfield Benchmarks": [
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "a0eb255abc6bc57552bd3d53b9f738b234493c29",
          "message": "Fix expected output to match updated bmw.sql comment\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T00:28:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a0eb255abc6bc57552bd3d53b9f738b234493c29"
        },
        "date": 1767664999972,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 234.215,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.02,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "8828e633e464ddd754f6afe188862ded05d706ff",
          "message": "Remove BMW_MAX_TERMS limit - always use BMW for multi-term queries\n\nBenchmarks show BMW outperforms exhaustive scoring even for 8+ term\nqueries. The previous assumption that \"exhaustive wins beyond 8 terms\"\nwas incorrect - bucket 8 queries were 2.7x slower than System X due\nto falling back to exhaustive scanning.\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T02:24:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8828e633e464ddd754f6afe188862ded05d706ff"
        },
        "date": 1767667586419,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 262.269,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.02,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "9aea7f2a200fda30b653e4dd6d4f25d03f52c937",
          "message": "Add competitive benchmarks (#106)\n\n## Summary\n\n- Add System X benchmark job to run in parallel with pg_textsearch\nbenchmarks\n- Add `dry_run` option to skip publishing to GitHub Pages (for testing)\n- Set parallel workers dynamically based on CPU count for fair\ncomparison\n- Improve throughput benchmark methodology (warmup, multiple iterations,\nconsistent SQL)\n\n## Changes\n\n### Workflow\n- `dry_run` input parameter to test without publishing\n\n### Benchmark methodology\n- Throughput tests now include warmup pass\n- 10 iterations with median reporting (matching individual query tests)\n- Use `EXECUTE` instead of `PERFORM` for consistency\n- Fixes #95 by using subset of real MS-MARCO dev queries",
          "timestamp": "2026-01-06T00:49:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9aea7f2a200fda30b653e4dd6d4f25d03f52c937"
        },
        "date": 1767680958431,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 234.713,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.02,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "c3c4fe35ea13da8feb79a71ae846b18059e89f77",
          "message": "Fix wand test: use to_bm25query for explicit index binding\n\nBare bm25query casts don't work reliably when BMW is disabled.",
          "timestamp": "2026-01-06T19:12:27Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c3c4fe35ea13da8feb79a71ae846b18059e89f77"
        },
        "date": 1767731838545,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.368,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.02,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "87f6ae4508cd1f1d0037ca92ff012ff1e4eecd47",
          "message": "Add Block-Max WAND (BMW) optimization for top-k queries (#102)\n\n## Summary\n\n- Implement Block-Max WAND (BMW) for top-k retrieval using block-level\nupper bounds in V2 segments\n- Add top-k min-heap with O(1) threshold access and O(log k) updates for\nefficient result tracking\n- BMW fast path for single-term queries (skip blocks that can't\ncontribute to top-k)\n- BMW fast path for multi-term queries with WAND-style doc-ID ordered\ntraversal\n- Batch doc_freq lookups to reduce segment open/close overhead for\nmulti-term queries\n- GUC variables `pg_textsearch.enable_bmw` and `log_bmw_stats` for\ndebugging/benchmarking\n\n## Performance (MS MARCO 8.8M docs, p50 latency)\n\n| Query Length | pg_textsearch | System X | Result |\n|--------------|---------------|----------|--------|\n| 1 token | 10.14ms | 18.05ms | **1.8x faster** |\n| 2 tokens | 12.54ms | 17.24ms | **1.4x faster** |\n| 3 tokens | 15.14ms | 22.94ms | **1.5x faster** |\n| 4 tokens | 19.72ms | 24.01ms | **1.2x faster** |\n| 5 tokens | 25.79ms | 26.21ms | ~same |\n| 6 tokens | 32.88ms | 33.47ms | ~same |\n| 7 tokens | 42.09ms | 32.23ms | 1.3x slower |\n| 8+ tokens | 63.41ms | 39.17ms | 1.6x slower |\n\n**Cranfield:** 225 queries in 57ms (0.26 ms/query avg)\n\n*Note: These benchmarks establish baselines for pg_textsearch\u2014not a\nhead-to-head comparison. System X has different defaults and tuning\noptions; further iteration on configurations required.*\n\n## Implementation Details\n\n**New files:**\n- `src/query/bmw.h` - Top-k heap, BMW stats, and scoring function\ninterfaces\n- `src/query/bmw.c` - Min-heap implementation, block max score\ncomputation, single-term and multi-term BMW scoring\n\n**Key algorithm:**\n1. Compute block max BM25 score from skip entry metadata\n(`block_max_tf`, `block_max_norm`)\n2. Only score blocks where `block_max_score >= current_threshold`\n3. Update threshold as better results are found\n4. Memtable scored exhaustively (no skip index)\n\n**Multi-term optimization:**\n- Sort terms by IDF (highest first) for faster threshold convergence\n- WAND-style doc-ID ordered traversal across terms' posting lists\n- Batch doc_freq lookups: opens each segment once instead of once per\nterm\n- Reduces segment opens from O(terms \u00d7 segments) to O(segments)\n\n## Testing\n\n- All regression tests pass\n- Shell-based tests pass (concurrency, recovery, segment)\n- Results match exhaustive scoring path with correct tie-breaking\n\n---------\n\nCo-authored-by: Claude <noreply@anthropic.com>",
          "timestamp": "2026-01-07T04:07:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/87f6ae4508cd1f1d0037ca92ff012ff1e4eecd47"
        },
        "date": 1767760056781,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 236.223,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.02,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "67160fc91c9b962dc729a65e65fd65703f485b02",
          "message": "Add CLAUDE.md to repository (#112)\n\n## Summary\n\n- Adds CLAUDE.md to the repository (previously gitignored)\n- Updates all sections to reflect current codebase state at v0.3.0-dev\n- Documents reorganized source structure with subdirectories\n- Updates GUC parameters and configuration options\n- Documents Block-Max WAND (BMW) query optimization\n- Adds all current test files and CI workflows\n\n## Key Changes\n\n**Source Structure**: Documented the reorganized codebase with:\n- `src/am/` - Access method implementation\n- `src/memtable/` - In-memory index structures\n- `src/segment/` - Disk-based segment storage\n- `src/query/` - Query execution and BMW optimization\n- `src/types/` - SQL data types\n- `src/state/` - Index state management\n- `src/planner/` - Query planner integration\n- `src/debug/` - Debugging utilities\n\n**Configuration**: Updated GUC parameters:\n- `pg_textsearch.default_limit` (was index_memory_limit)\n- `pg_textsearch.enable_bmw` (new)\n- `pg_textsearch.log_bmw_stats` (new)\n- `pg_textsearch.bulk_load_threshold` (new)\n- `pg_textsearch.memtable_spill_threshold` (new)\n- `pg_textsearch.segments_per_level` (new)\n\n**Documentation**: Added sections for:\n- Block-Max WAND optimization\n- All CI workflows (benchmark.yml, nightly-stress.yml,\nupgrade-tests.yml, etc.)\n- New benchmark datasets (MS MARCO, Wikipedia)\n- Debug functions (bm25_dump_index, bm25_summarize_index)\n\n## Testing\n\nN/A - documentation only",
          "timestamp": "2026-01-07T05:22:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/67160fc91c9b962dc729a65e65fd65703f485b02"
        },
        "date": 1767766974379,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 237.564,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.02,
            "unit": "MB"
          }
        ]
      }
    ],
    "wikipedia Benchmarks": [
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "a0eb255abc6bc57552bd3d53b9f738b234493c29",
          "message": "Fix expected output to match updated bmw.sql comment\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T00:28:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a0eb255abc6bc57552bd3d53b9f738b234493c29"
        },
        "date": 1767665004784,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18206.761,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.75,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "8828e633e464ddd754f6afe188862ded05d706ff",
          "message": "Remove BMW_MAX_TERMS limit - always use BMW for multi-term queries\n\nBenchmarks show BMW outperforms exhaustive scoring even for 8+ term\nqueries. The previous assumption that \"exhaustive wins beyond 8 terms\"\nwas incorrect - bucket 8 queries were 2.7x slower than System X due\nto falling back to exhaustive scanning.\n\n\ud83e\udd16 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T02:24:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8828e633e464ddd754f6afe188862ded05d706ff"
        },
        "date": 1767667590975,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18996.624,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.75,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "9aea7f2a200fda30b653e4dd6d4f25d03f52c937",
          "message": "Add competitive benchmarks (#106)\n\n## Summary\n\n- Add System X benchmark job to run in parallel with pg_textsearch\nbenchmarks\n- Add `dry_run` option to skip publishing to GitHub Pages (for testing)\n- Set parallel workers dynamically based on CPU count for fair\ncomparison\n- Improve throughput benchmark methodology (warmup, multiple iterations,\nconsistent SQL)\n\n## Changes\n\n### Workflow\n- `dry_run` input parameter to test without publishing\n\n### Benchmark methodology\n- Throughput tests now include warmup pass\n- 10 iterations with median reporting (matching individual query tests)\n- Use `EXECUTE` instead of `PERFORM` for consistency\n- Fixes #95 by using subset of real MS-MARCO dev queries",
          "timestamp": "2026-01-06T00:49:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9aea7f2a200fda30b653e4dd6d4f25d03f52c937"
        },
        "date": 1767680963824,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18689.723,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.75,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "c3c4fe35ea13da8feb79a71ae846b18059e89f77",
          "message": "Fix wand test: use to_bm25query for explicit index binding\n\nBare bm25query casts don't work reliably when BMW is disabled.",
          "timestamp": "2026-01-06T19:12:27Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c3c4fe35ea13da8feb79a71ae846b18059e89f77"
        },
        "date": 1767731842163,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18522.035,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.75,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "87f6ae4508cd1f1d0037ca92ff012ff1e4eecd47",
          "message": "Add Block-Max WAND (BMW) optimization for top-k queries (#102)\n\n## Summary\n\n- Implement Block-Max WAND (BMW) for top-k retrieval using block-level\nupper bounds in V2 segments\n- Add top-k min-heap with O(1) threshold access and O(log k) updates for\nefficient result tracking\n- BMW fast path for single-term queries (skip blocks that can't\ncontribute to top-k)\n- BMW fast path for multi-term queries with WAND-style doc-ID ordered\ntraversal\n- Batch doc_freq lookups to reduce segment open/close overhead for\nmulti-term queries\n- GUC variables `pg_textsearch.enable_bmw` and `log_bmw_stats` for\ndebugging/benchmarking\n\n## Performance (MS MARCO 8.8M docs, p50 latency)\n\n| Query Length | pg_textsearch | System X | Result |\n|--------------|---------------|----------|--------|\n| 1 token | 10.14ms | 18.05ms | **1.8x faster** |\n| 2 tokens | 12.54ms | 17.24ms | **1.4x faster** |\n| 3 tokens | 15.14ms | 22.94ms | **1.5x faster** |\n| 4 tokens | 19.72ms | 24.01ms | **1.2x faster** |\n| 5 tokens | 25.79ms | 26.21ms | ~same |\n| 6 tokens | 32.88ms | 33.47ms | ~same |\n| 7 tokens | 42.09ms | 32.23ms | 1.3x slower |\n| 8+ tokens | 63.41ms | 39.17ms | 1.6x slower |\n\n**Cranfield:** 225 queries in 57ms (0.26 ms/query avg)\n\n*Note: These benchmarks establish baselines for pg_textsearch\u2014not a\nhead-to-head comparison. System X has different defaults and tuning\noptions; further iteration on configurations required.*\n\n## Implementation Details\n\n**New files:**\n- `src/query/bmw.h` - Top-k heap, BMW stats, and scoring function\ninterfaces\n- `src/query/bmw.c` - Min-heap implementation, block max score\ncomputation, single-term and multi-term BMW scoring\n\n**Key algorithm:**\n1. Compute block max BM25 score from skip entry metadata\n(`block_max_tf`, `block_max_norm`)\n2. Only score blocks where `block_max_score >= current_threshold`\n3. Update threshold as better results are found\n4. Memtable scored exhaustively (no skip index)\n\n**Multi-term optimization:**\n- Sort terms by IDF (highest first) for faster threshold convergence\n- WAND-style doc-ID ordered traversal across terms' posting lists\n- Batch doc_freq lookups: opens each segment once instead of once per\nterm\n- Reduces segment opens from O(terms \u00d7 segments) to O(segments)\n\n## Testing\n\n- All regression tests pass\n- Shell-based tests pass (concurrency, recovery, segment)\n- Results match exhaustive scoring path with correct tie-breaking\n\n---------\n\nCo-authored-by: Claude <noreply@anthropic.com>",
          "timestamp": "2026-01-07T04:07:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/87f6ae4508cd1f1d0037ca92ff012ff1e4eecd47"
        },
        "date": 1767760059958,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19301.022,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.75,
            "unit": "MB"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "67160fc91c9b962dc729a65e65fd65703f485b02",
          "message": "Add CLAUDE.md to repository (#112)\n\n## Summary\n\n- Adds CLAUDE.md to the repository (previously gitignored)\n- Updates all sections to reflect current codebase state at v0.3.0-dev\n- Documents reorganized source structure with subdirectories\n- Updates GUC parameters and configuration options\n- Documents Block-Max WAND (BMW) query optimization\n- Adds all current test files and CI workflows\n\n## Key Changes\n\n**Source Structure**: Documented the reorganized codebase with:\n- `src/am/` - Access method implementation\n- `src/memtable/` - In-memory index structures\n- `src/segment/` - Disk-based segment storage\n- `src/query/` - Query execution and BMW optimization\n- `src/types/` - SQL data types\n- `src/state/` - Index state management\n- `src/planner/` - Query planner integration\n- `src/debug/` - Debugging utilities\n\n**Configuration**: Updated GUC parameters:\n- `pg_textsearch.default_limit` (was index_memory_limit)\n- `pg_textsearch.enable_bmw` (new)\n- `pg_textsearch.log_bmw_stats` (new)\n- `pg_textsearch.bulk_load_threshold` (new)\n- `pg_textsearch.memtable_spill_threshold` (new)\n- `pg_textsearch.segments_per_level` (new)\n\n**Documentation**: Added sections for:\n- Block-Max WAND optimization\n- All CI workflows (benchmark.yml, nightly-stress.yml,\nupgrade-tests.yml, etc.)\n- New benchmark datasets (MS MARCO, Wikipedia)\n- Debug functions (bm25_dump_index, bm25_summarize_index)\n\n## Testing\n\nN/A - documentation only",
          "timestamp": "2026-01-07T05:22:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/67160fc91c9b962dc729a65e65fd65703f485b02"
        },
        "date": 1767766977010,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19071.839,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.75,
            "unit": "MB"
          }
        ]
      }
    ]
  }
};
