window.BENCHMARK_DATA = {
  "lastUpdate": 1767584672814,
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
          "id": "32f6bfa8072f662af9a60d8c8aa261928cbe4355",
          "message": "Remove ParadeDB benchmark publishing to public dashboard\n\nKeep competitive benchmark results in workflow logs and artifacts only,\nwithout publishing to the GitHub Pages dashboard. Results are still\ncollected but not publicly graphed.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-05T02:16:32Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/32f6bfa8072f662af9a60d8c8aa261928cbe4355"
        },
        "date": 1767584672098,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 521752.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 137.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.21,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}