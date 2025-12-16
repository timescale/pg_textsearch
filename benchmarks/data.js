window.BENCHMARK_DATA = {
  "lastUpdate": 1765916951734,
  "repoUrl": "https://github.com/timescale/pg_textsearch",
  "entries": {
    "cranfield Benchmarks": [
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
          "id": "55a8b6c71318a0df49fe9dabbc2e8401503fb3a0",
          "message": "Fix JSON generation in extract_metrics.sh (#73)\n\nUse jq for proper JSON formatting instead of heredoc string\ninterpolation. This handles empty/null values correctly and avoids\nmalformed JSON.",
          "timestamp": "2025-12-16T02:54:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/55a8b6c71318a0df49fe9dabbc2e8401503fb3a0"
        },
        "date": 1765860275252,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield - Index Build Time",
            "value": 238.159,
            "unit": "ms"
          },
          {
            "name": "cranfield - Short Query (1 word)",
            "value": 3.379,
            "unit": "ms"
          },
          {
            "name": "cranfield - Medium Query (3 words)",
            "value": 4.656,
            "unit": "ms"
          },
          {
            "name": "cranfield - Long Query (question)",
            "value": 3.867,
            "unit": "ms"
          },
          {
            "name": "cranfield - Common Term Query",
            "value": 3.648,
            "unit": "ms"
          },
          {
            "name": "cranfield - Rare Term Query",
            "value": 2.704,
            "unit": "ms"
          },
          {
            "name": "cranfield - Avg Query Latency (20 queries)",
            "value": 4.06,
            "unit": "ms"
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
          "id": "c80221e1ec07cfcfb80ea12afe6ac77a2776d12f",
          "message": "Extract and publish metrics per-dataset when running all benchmarks (#75)\n\n## Summary\n- When running `dataset=all`, metrics are now extracted separately for\neach dataset\n- Each dataset (Cranfield, MS MARCO, Wikipedia) gets its own benchmark\nchart\n- Previously, only the first dataset's results were captured\n\n## Changes\n- `extract_metrics.sh` now accepts optional section parameter to extract\nfrom log sections\n- Workflow runs extract_metrics.sh once per dataset when running \"all\"\n- Separate benchmark-action publish steps for each dataset\n\n## Testing\nTrigger a benchmark run with `dataset=all` to verify all three datasets\nappear separately in the results.",
          "timestamp": "2025-12-16T17:18:32Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c80221e1ec07cfcfb80ea12afe6ac77a2776d12f"
        },
        "date": 1765916949001,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield - Index Build Time",
            "value": 270.157,
            "unit": "ms"
          },
          {
            "name": "cranfield - Short Query (1 word)",
            "value": 3.13,
            "unit": "ms"
          },
          {
            "name": "cranfield - Medium Query (3 words)",
            "value": 4.225,
            "unit": "ms"
          },
          {
            "name": "cranfield - Long Query (question)",
            "value": 3.436,
            "unit": "ms"
          },
          {
            "name": "cranfield - Common Term Query",
            "value": 3.183,
            "unit": "ms"
          },
          {
            "name": "cranfield - Rare Term Query",
            "value": 2.352,
            "unit": "ms"
          },
          {
            "name": "cranfield - Avg Query Latency (20 queries)",
            "value": 4.05,
            "unit": "ms"
          }
        ]
      }
    ],
    "all Benchmarks": [
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
          "id": "55a8b6c71318a0df49fe9dabbc2e8401503fb3a0",
          "message": "Fix JSON generation in extract_metrics.sh (#73)\n\nUse jq for proper JSON formatting instead of heredoc string\ninterpolation. This handles empty/null values correctly and avoids\nmalformed JSON.",
          "timestamp": "2025-12-16T02:54:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/55a8b6c71318a0df49fe9dabbc2e8401503fb3a0"
        },
        "date": 1765862888763,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "all - Index Build Time",
            "value": 248.508,
            "unit": "ms"
          },
          {
            "name": "all - Short Query (1 word)",
            "value": 3.659,
            "unit": "ms"
          },
          {
            "name": "all - Medium Query (3 words)",
            "value": 4.364,
            "unit": "ms"
          },
          {
            "name": "all - Long Query (question)",
            "value": 3.62,
            "unit": "ms"
          },
          {
            "name": "all - Common Term Query",
            "value": 3.311,
            "unit": "ms"
          },
          {
            "name": "all - Rare Term Query",
            "value": 2.421,
            "unit": "ms"
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
          "id": "866fd1af7979c9077924da9753a8b8181dcfbfc6",
          "message": "Run benchmark queries repeatedly for stable measurements (#74)\n\n## Summary\n- Each benchmark query now runs 10 times, reporting median/min/max times\n- Reduces variance from cache warming and system noise\n- Updated extract_metrics.sh to parse median from new output format\n\n## Testing\nManually verified query function works locally. Benchmark results will\nbe validated on next nightly run.",
          "timestamp": "2025-12-16T06:01:42Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/866fd1af7979c9077924da9753a8b8181dcfbfc6"
        },
        "date": 1765866139666,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "all - Index Build Time",
            "value": 242.301,
            "unit": "ms"
          },
          {
            "name": "all - Short Query (1 word)",
            "value": 3.092,
            "unit": "ms"
          },
          {
            "name": "all - Medium Query (3 words)",
            "value": 4.167,
            "unit": "ms"
          },
          {
            "name": "all - Long Query (question)",
            "value": 3.408,
            "unit": "ms"
          },
          {
            "name": "all - Common Term Query",
            "value": 3.146,
            "unit": "ms"
          },
          {
            "name": "all - Rare Term Query",
            "value": 2.333,
            "unit": "ms"
          }
        ]
      }
    ],
    "msmarco Benchmarks": [
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
          "id": "c80221e1ec07cfcfb80ea12afe6ac77a2776d12f",
          "message": "Extract and publish metrics per-dataset when running all benchmarks (#75)\n\n## Summary\n- When running `dataset=all`, metrics are now extracted separately for\neach dataset\n- Each dataset (Cranfield, MS MARCO, Wikipedia) gets its own benchmark\nchart\n- Previously, only the first dataset's results were captured\n\n## Changes\n- `extract_metrics.sh` now accepts optional section parameter to extract\nfrom log sections\n- Workflow runs extract_metrics.sh once per dataset when running \"all\"\n- Separate benchmark-action publish steps for each dataset\n\n## Testing\nTrigger a benchmark run with `dataset=all` to verify all three datasets\nappear separately in the results.",
          "timestamp": "2025-12-16T17:18:32Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c80221e1ec07cfcfb80ea12afe6ac77a2776d12f"
        },
        "date": 1765916950288,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco - Index Build Time",
            "value": 593036.256,
            "unit": "ms"
          },
          {
            "name": "msmarco - Short Query (1 word)",
            "value": 5.436,
            "unit": "ms"
          },
          {
            "name": "msmarco - Medium Query (3 words)",
            "value": 7.082,
            "unit": "ms"
          },
          {
            "name": "msmarco - Long Query (question)",
            "value": 11.716,
            "unit": "ms"
          },
          {
            "name": "msmarco - Common Term Query",
            "value": 0.045,
            "unit": "ms"
          },
          {
            "name": "msmarco - Rare Term Query",
            "value": 9.326,
            "unit": "ms"
          },
          {
            "name": "msmarco - Avg Query Latency (20 queries)",
            "value": 18.56,
            "unit": "ms"
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
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "c80221e1ec07cfcfb80ea12afe6ac77a2776d12f",
          "message": "Extract and publish metrics per-dataset when running all benchmarks (#75)\n\n## Summary\n- When running `dataset=all`, metrics are now extracted separately for\neach dataset\n- Each dataset (Cranfield, MS MARCO, Wikipedia) gets its own benchmark\nchart\n- Previously, only the first dataset's results were captured\n\n## Changes\n- `extract_metrics.sh` now accepts optional section parameter to extract\nfrom log sections\n- Workflow runs extract_metrics.sh once per dataset when running \"all\"\n- Separate benchmark-action publish steps for each dataset\n\n## Testing\nTrigger a benchmark run with `dataset=all` to verify all three datasets\nappear separately in the results.",
          "timestamp": "2025-12-16T17:18:32Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c80221e1ec07cfcfb80ea12afe6ac77a2776d12f"
        },
        "date": 1765916951504,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia - Index Build Time",
            "value": 20422.556,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Short Query (1 word)",
            "value": 3.578,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Medium Query (3 words)",
            "value": 1.488,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Long Query (question)",
            "value": 1.316,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Common Term Query",
            "value": 2.971,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Rare Term Query",
            "value": 3.492,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}