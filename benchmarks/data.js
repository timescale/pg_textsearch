window.BENCHMARK_DATA = {
  "lastUpdate": 1767590769857,
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
      }
    ]
  }
}