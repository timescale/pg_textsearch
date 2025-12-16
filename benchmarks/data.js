window.BENCHMARK_DATA = {
  "lastUpdate": 1765924502219,
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
          "id": "1b423ca362f278eedc868953d940a15a4ee6ea0e",
          "message": "Improve benchmark configuration and add index size tracking (#76)\n\n## Summary\n\n- Tune Postgres settings for ubuntu-latest runner (4 vCPUs, 16GB RAM):\n  - `shared_buffers`: 1GB → 4GB (25% of RAM)\n  - `effective_cache_size`: 2GB → 12GB (75% of RAM)\n  - `maintenance_work_mem`: 256MB → 512MB\n- Add more aggressive disk cleanup (swift, powershell, ghcup) to free\n~10GB additional space\n- Add index and table size reporting to all benchmark datasets with\nstandardized output\n- Track index size over time in benchmark dashboard (displayed in MB)\n- Show index/table sizes in GitHub job summary\n\n## Testing\n\n- [ ] Manually trigger benchmark workflow to verify new metrics are\ncaptured",
          "timestamp": "2025-12-16T22:14:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1b423ca362f278eedc868953d940a15a4ee6ea0e"
        },
        "date": 1765924498779,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield - Index Build Time",
            "value": 245.552,
            "unit": "ms"
          },
          {
            "name": "cranfield - Short Query (1 word)",
            "value": 3.198,
            "unit": "ms"
          },
          {
            "name": "cranfield - Medium Query (3 words)",
            "value": 4.219,
            "unit": "ms"
          },
          {
            "name": "cranfield - Long Query (question)",
            "value": 3.415,
            "unit": "ms"
          },
          {
            "name": "cranfield - Common Term Query",
            "value": 5.681,
            "unit": "ms"
          },
          {
            "name": "cranfield - Rare Term Query",
            "value": 2.359,
            "unit": "ms"
          },
          {
            "name": "cranfield - Avg Query Latency (20 queries)",
            "value": 3.99,
            "unit": "ms"
          },
          {
            "name": "cranfield - Index Size",
            "value": 0.02,
            "unit": "MB"
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
          "id": "1b423ca362f278eedc868953d940a15a4ee6ea0e",
          "message": "Improve benchmark configuration and add index size tracking (#76)\n\n## Summary\n\n- Tune Postgres settings for ubuntu-latest runner (4 vCPUs, 16GB RAM):\n  - `shared_buffers`: 1GB → 4GB (25% of RAM)\n  - `effective_cache_size`: 2GB → 12GB (75% of RAM)\n  - `maintenance_work_mem`: 256MB → 512MB\n- Add more aggressive disk cleanup (swift, powershell, ghcup) to free\n~10GB additional space\n- Add index and table size reporting to all benchmark datasets with\nstandardized output\n- Track index size over time in benchmark dashboard (displayed in MB)\n- Show index/table sizes in GitHub job summary\n\n## Testing\n\n- [ ] Manually trigger benchmark workflow to verify new metrics are\ncaptured",
          "timestamp": "2025-12-16T22:14:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1b423ca362f278eedc868953d940a15a4ee6ea0e"
        },
        "date": 1765924500613,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco - Index Build Time",
            "value": 585316.527,
            "unit": "ms"
          },
          {
            "name": "msmarco - Short Query (1 word)",
            "value": 5.437,
            "unit": "ms"
          },
          {
            "name": "msmarco - Medium Query (3 words)",
            "value": 7.081,
            "unit": "ms"
          },
          {
            "name": "msmarco - Long Query (question)",
            "value": 11.829,
            "unit": "ms"
          },
          {
            "name": "msmarco - Common Term Query",
            "value": 0.057,
            "unit": "ms"
          },
          {
            "name": "msmarco - Rare Term Query",
            "value": 9.285,
            "unit": "ms"
          },
          {
            "name": "msmarco - Avg Query Latency (20 queries)",
            "value": 20.36,
            "unit": "ms"
          },
          {
            "name": "msmarco - Index Size",
            "value": 7697.38,
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
          "id": "1b423ca362f278eedc868953d940a15a4ee6ea0e",
          "message": "Improve benchmark configuration and add index size tracking (#76)\n\n## Summary\n\n- Tune Postgres settings for ubuntu-latest runner (4 vCPUs, 16GB RAM):\n  - `shared_buffers`: 1GB → 4GB (25% of RAM)\n  - `effective_cache_size`: 2GB → 12GB (75% of RAM)\n  - `maintenance_work_mem`: 256MB → 512MB\n- Add more aggressive disk cleanup (swift, powershell, ghcup) to free\n~10GB additional space\n- Add index and table size reporting to all benchmark datasets with\nstandardized output\n- Track index size over time in benchmark dashboard (displayed in MB)\n- Show index/table sizes in GitHub job summary\n\n## Testing\n\n- [ ] Manually trigger benchmark workflow to verify new metrics are\ncaptured",
          "timestamp": "2025-12-16T22:14:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1b423ca362f278eedc868953d940a15a4ee6ea0e"
        },
        "date": 1765924501902,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia - Index Build Time",
            "value": 19767.231,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Short Query (1 word)",
            "value": 3.613,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Medium Query (3 words)",
            "value": 1.479,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Long Query (question)",
            "value": 1.397,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Common Term Query",
            "value": 2.738,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Rare Term Query",
            "value": 3.494,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Index Size",
            "value": 158.62,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}