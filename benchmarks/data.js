window.BENCHMARK_DATA = {
  "lastUpdate": 1767680961809,
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
          "message": "Update step summaries for new token-bucketed metrics\n\nBoth ParadeDB and pg_textsearch job summaries now show:\n- Latency by token count (1-8+ tokens, p50)\n- Throughput (avg ms/query)\n\nEasy side-by-side comparison in GitHub Actions run summary.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
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
          "message": "Update step summaries for new token-bucketed metrics\n\nBoth ParadeDB and pg_textsearch job summaries now show:\n- Latency by token count (1-8+ tokens, p50)\n- Throughput (avg ms/query)\n\nEasy side-by-side comparison in GitHub Actions run summary.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
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
            "name": "Mateusz Paluszkiewicz",
            "username": "TheAifam5",
            "email": "theaifam5@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220",
          "message": "Add missing math.h include in build.c (#105)\n\ngcc 14.2.0 reports an error about implicit declaration of 'log' function\nin build.c. Adding the math.h include resolves this issue.\n\nSigned-off-by: Mateusz Paluszkiewicz <theaifam5@gmail.com>",
          "timestamp": "2026-01-05T05:14:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220"
        },
        "date": 1767594141357,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 530966.706,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 6.979,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 16.674,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 14.764,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.046,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 4.965,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 34.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query with Score",
            "value": 12.794,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query with Score",
            "value": 23.736,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query with Score",
            "value": 26.877,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query with Score",
            "value": 16.009,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency with Score",
            "value": 42.38,
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
            "name": "Mateusz Paluszkiewicz",
            "username": "TheAifam5",
            "email": "theaifam5@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220",
          "message": "Add missing math.h include in build.c (#105)\n\ngcc 14.2.0 reports an error about implicit declaration of 'log' function\nin build.c. Adding the math.h include resolves this issue.\n\nSigned-off-by: Mateusz Paluszkiewicz <theaifam5@gmail.com>",
          "timestamp": "2026-01-05T05:14:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220"
        },
        "date": 1767656915847,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (99.9K docs) - Index Build Time",
            "value": 5260.761,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Short Query (1 word)",
            "value": 0.118,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Medium Query (3 words)",
            "value": 0.155,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Long Query (question)",
            "value": 0.169,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Common Term Query",
            "value": 0.036,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Rare Term Query",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Avg Query Latency (20 queries)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Short Query with Score",
            "value": 0.532,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Medium Query with Score",
            "value": 0.701,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Long Query with Score",
            "value": 0.642,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Rare Term Query with Score",
            "value": 0.623,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Avg Query Latency with Score",
            "value": 1.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Index Size",
            "value": 27.07,
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
          "id": "61561e2c3e7e1ebc240d78703213126fb67bc2f9",
          "message": "Rename paradedb references to systemx for anonymization\n\n- Rename paradedb/ directory to systemx/\n- Rename job from paradedb-benchmark to system-x-benchmark\n- Rename artifact from paradedb-benchmark to system-x-benchmark\n- Rename internal file names (paradedb_metrics.json -> systemx_metrics.json)\n- Rename PostgreSQL objects (tables, indexes, functions) to use systemx suffix\n- Update all workflow step names and comments\n\nThe only remaining \"paradedb\" references are:\n- GitHub download URL (actual package location)\n- paradedb.match() and paradedb.score() SQL API functions (extension schema)\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-05T22:21:24Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/61561e2c3e7e1ebc240d78703213126fb67bc2f9"
        },
        "date": 1767657844821,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (99.9K docs) - Index Build Time",
            "value": 5844.975,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 1 Token Query (p50)",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 2 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 3 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 4 Token Query (p50)",
            "value": 0.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 5 Token Query (p50)",
            "value": 0.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 6 Token Query (p50)",
            "value": 0.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 7 Token Query (p50)",
            "value": 1.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 8+ Token Query (p50)",
            "value": 1.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Index Size",
            "value": 27.07,
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
          "id": "bf3d3f505226d6cd33101541473f608e9ff46537",
          "message": "Add result count validation to benchmark queries\n\nBoth pg_textsearch and System X benchmarks now report total_results\nto verify that queries are actually returning data, not just executing.\n\nThis helps validate that the benchmark methodology is sound.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-06T00:19:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bf3d3f505226d6cd33101541473f608e9ff46537"
        },
        "date": 1767659049139,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (99.9K docs) - Index Build Time",
            "value": 5679.626,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 1 Token Query (p50)",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 2 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 3 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 4 Token Query (p50)",
            "value": 0.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 5 Token Query (p50)",
            "value": 0.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 6 Token Query (p50)",
            "value": 1,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 7 Token Query (p50)",
            "value": 1.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 8+ Token Query (p50)",
            "value": 1.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Index Size",
            "value": 27.07,
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
          "id": "2a1a8490035f0914e4d2f4afc44258540e2b946b",
          "message": "Fix expected output to match updated bmw.sql comment\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T00:28:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2a1a8490035f0914e4d2f4afc44258540e2b946b"
        },
        "date": 1767661747594,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 522045.617,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 9.022,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 8.925,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 14.607,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.005,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 22.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query with Score",
            "value": 13.527,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query with Score",
            "value": 15.501,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query with Score",
            "value": 16.549,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query with Score",
            "value": 20.296,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency with Score",
            "value": 28.76,
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
          "message": "Fix expected output to match updated bmw.sql comment\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
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
          "message": "Remove BMW_MAX_TERMS limit - always use BMW for multi-term queries\n\nBenchmarks show BMW outperforms exhaustive scoring even for 8+ term\nqueries. The previous assumption that \"exhaustive wins beyond 8 terms\"\nwas incorrect - bucket 8 queries were 2.7x slower than System X due\nto falling back to exhaustive scanning.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
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
      }
    ],
    "cranfield Benchmarks": [
      {
        "commit": {
          "author": {
            "name": "Mateusz Paluszkiewicz",
            "username": "TheAifam5",
            "email": "theaifam5@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220",
          "message": "Add missing math.h include in build.c (#105)\n\ngcc 14.2.0 reports an error about implicit declaration of 'log' function\nin build.c. Adding the math.h include resolves this issue.\n\nSigned-off-by: Mateusz Paluszkiewicz <theaifam5@gmail.com>",
          "timestamp": "2026-01-05T05:14:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220"
        },
        "date": 1767594139577,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 246.461,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 0.124,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 0.184,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 0.184,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 0.154,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 0.051,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 0.27,
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
          "id": "2a1a8490035f0914e4d2f4afc44258540e2b946b",
          "message": "Fix expected output to match updated bmw.sql comment\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T00:28:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2a1a8490035f0914e4d2f4afc44258540e2b946b"
        },
        "date": 1767661746131,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 240.686,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 0.125,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 0.176,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 0.181,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 0.053,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
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
          "id": "a0eb255abc6bc57552bd3d53b9f738b234493c29",
          "message": "Fix expected output to match updated bmw.sql comment\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
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
          "message": "Remove BMW_MAX_TERMS limit - always use BMW for multi-term queries\n\nBenchmarks show BMW outperforms exhaustive scoring even for 8+ term\nqueries. The previous assumption that \"exhaustive wins beyond 8 terms\"\nwas incorrect - bucket 8 queries were 2.7x slower than System X due\nto falling back to exhaustive scanning.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
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
      }
    ],
    "wikipedia Benchmarks": [
      {
        "commit": {
          "author": {
            "name": "Mateusz Paluszkiewicz",
            "username": "TheAifam5",
            "email": "theaifam5@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220",
          "message": "Add missing math.h include in build.c (#105)\n\ngcc 14.2.0 reports an error about implicit declaration of 'log' function\nin build.c. Adding the math.h include resolves this issue.\n\nSigned-off-by: Mateusz Paluszkiewicz <theaifam5@gmail.com>",
          "timestamp": "2026-01-05T05:14:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ee054e9d8a2b8279b8c3a7e85b4eaa74d79c6220"
        },
        "date": 1767594142791,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18753.694,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.806,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 3.588,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.195,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.165,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 5.528,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query with Score",
            "value": 3.748,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query with Score",
            "value": 4.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query with Score",
            "value": 3.524,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query with Score",
            "value": 4.054,
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
          "id": "2a1a8490035f0914e4d2f4afc44258540e2b946b",
          "message": "Fix expected output to match updated bmw.sql comment\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
          "timestamp": "2026-01-06T00:28:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2a1a8490035f0914e4d2f4afc44258540e2b946b"
        },
        "date": 1767661748830,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18856.162,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.657,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 2.001,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 4.833,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 7.403,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 12.675,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query with Score",
            "value": 13.609,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query with Score",
            "value": 2.876,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query with Score",
            "value": 2.921,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query with Score",
            "value": 4.183,
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
          "id": "a0eb255abc6bc57552bd3d53b9f738b234493c29",
          "message": "Fix expected output to match updated bmw.sql comment\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
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
          "message": "Remove BMW_MAX_TERMS limit - always use BMW for multi-term queries\n\nBenchmarks show BMW outperforms exhaustive scoring even for 8+ term\nqueries. The previous assumption that \"exhaustive wins beyond 8 terms\"\nwas incorrect - bucket 8 queries were 2.7x slower than System X due\nto falling back to exhaustive scanning.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)",
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
      }
    ]
  }
}