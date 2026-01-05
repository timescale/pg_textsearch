window.BENCHMARK_DATA = {
  "lastUpdate": 1767656917137,
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
      }
    ]
  }
}