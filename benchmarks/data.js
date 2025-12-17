window.BENCHMARK_DATA = {
  "lastUpdate": 1766008363022,
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
        "date": 1765926060457,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield - Index Build Time",
            "value": 243.511,
            "unit": "ms"
          },
          {
            "name": "cranfield - Short Query (1 word)",
            "value": 3.105,
            "unit": "ms"
          },
          {
            "name": "cranfield - Medium Query (3 words)",
            "value": 4.189,
            "unit": "ms"
          },
          {
            "name": "cranfield - Long Query (question)",
            "value": 3.432,
            "unit": "ms"
          },
          {
            "name": "cranfield - Common Term Query",
            "value": 3.149,
            "unit": "ms"
          },
          {
            "name": "cranfield - Rare Term Query",
            "value": 2.324,
            "unit": "ms"
          },
          {
            "name": "cranfield - Avg Query Latency (20 queries)",
            "value": 4,
            "unit": "ms"
          },
          {
            "name": "cranfield - Index Size",
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
          "id": "09e726021418ac873b9d6878f9ddd6dae68230ae",
          "message": "Improve benchmark dashboard: dataset sizes and compact layout (#77)\n\n## Summary\n\n- Include document count in benchmark metric names (e.g., \"msmarco (8.8M\ndocs) - Index Build Time\")\n- Create 3-column responsive grid layout for benchmark dashboard\n(responsive down to 1 column on mobile)\n- Auto-deploy updated index.html to gh-pages after benchmarks run\n\n## Changes\n\n**format_for_action.sh**: Format document counts (1.4K, 8.8M, etc.) and\ninclude in metric names\n\n**benchmarks/gh-pages/index.html**: New compact layout with:\n- 3-column grid (responsive)\n- Smaller chart sizes\n- Cleaner styling\n- Dataset sections with headers\n\n**benchmark.yml**: Add step to deploy updated index.html to gh-pages",
          "timestamp": "2025-12-16T23:30:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/09e726021418ac873b9d6878f9ddd6dae68230ae"
        },
        "date": 1765928997911,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 243.876,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.117,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.191,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.404,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.154,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.325,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.04,
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
          "id": "36502f82bb9a840caebdb974afa425352d7d0956",
          "message": "Reclaim pages after segment compaction (#78)\n\n## Summary\n- Add page reclamation during segment compaction to prevent index bloat\n- Pages from merged source segments are now freed to PostgreSQL's FSM\n- New allocations check FSM first, reusing freed pages when available\n- Demote spill/merge notices to DEBUG1 to reduce build noise\n\n## Results (internal benchmark, 2.1M slack messages)\n\n| Metric | Before | After | Improvement |\n|--------|--------|-------|-------------|\n| Index Size | 831 MB | 473 MB | **43% smaller** |\n| Pages Reused | 0 | 45,026 | ✓ |",
          "timestamp": "2025-12-17T02:09:26Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/36502f82bb9a840caebdb974afa425352d7d0956"
        },
        "date": 1765952506273,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 257.613,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.182,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.233,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.448,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.207,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.354,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.04,
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
          "id": "b82dea59ef6d9c20c88742ce904acb4a94388246",
          "message": "Implement V2 segment format with block storage for BMW optimization\n\nThis implements Phase 1 of the block storage optimization plan:\n\nV2 segment format changes:\n- Block-based posting storage: 128 docs per block with TpBlockPosting (8 bytes)\n- Skip index with TpSkipEntry (16 bytes) enabling BMW block skipping\n- Fieldnorm table using Lucene SmallFloat encoding (1 byte per doc)\n- CTID map for segment-local doc_id → heap tuple lookup\n- TpDictEntryV2 (12 bytes) with skip_index_offset + block_count\n\nNew infrastructure:\n- docmap.c/h: Document ID mapping with CTID hash table\n- fieldnorm.h: Lucene SmallFloat encode/decode with precomputed table\n- segment_query.c: V2 query iterator with block-based iteration\n\nKey changes:\n- All segment writers switched from V1 to V2 (index build, spill, merge)\n- Merge completely rewritten to read V2 sources and write V2 output\n- Dump function updated to display V2 format details\n- Metapage version bumped to 5 (breaking change)\n\nThe V2 format reduces storage (posting: 14→8 bytes) and enables\nfuture BMW optimization by providing block-level statistics for\nearly termination during top-k queries.\n\nV1 code retained but unused - will be removed in future cleanup.",
          "timestamp": "2025-12-17T21:26:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b82dea59ef6d9c20c88742ce904acb4a94388246"
        },
        "date": 1766008355045,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 243.425,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.098,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.173,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.433,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.209,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.378,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.01,
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
        "date": 1765926061959,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco - Index Build Time",
            "value": 578715.942,
            "unit": "ms"
          },
          {
            "name": "msmarco - Short Query (1 word)",
            "value": 5.454,
            "unit": "ms"
          },
          {
            "name": "msmarco - Medium Query (3 words)",
            "value": 7.062,
            "unit": "ms"
          },
          {
            "name": "msmarco - Long Query (question)",
            "value": 11.707,
            "unit": "ms"
          },
          {
            "name": "msmarco - Common Term Query",
            "value": 0.052,
            "unit": "ms"
          },
          {
            "name": "msmarco - Rare Term Query",
            "value": 9.24,
            "unit": "ms"
          },
          {
            "name": "msmarco - Avg Query Latency (20 queries)",
            "value": 19.4,
            "unit": "ms"
          },
          {
            "name": "msmarco - Index Size",
            "value": 7697.38,
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
          "id": "09e726021418ac873b9d6878f9ddd6dae68230ae",
          "message": "Improve benchmark dashboard: dataset sizes and compact layout (#77)\n\n## Summary\n\n- Include document count in benchmark metric names (e.g., \"msmarco (8.8M\ndocs) - Index Build Time\")\n- Create 3-column responsive grid layout for benchmark dashboard\n(responsive down to 1 column on mobile)\n- Auto-deploy updated index.html to gh-pages after benchmarks run\n\n## Changes\n\n**format_for_action.sh**: Format document counts (1.4K, 8.8M, etc.) and\ninclude in metric names\n\n**benchmarks/gh-pages/index.html**: New compact layout with:\n- 3-column grid (responsive)\n- Smaller chart sizes\n- Cleaner styling\n- Dataset sections with headers\n\n**benchmark.yml**: Add step to deploy updated index.html to gh-pages",
          "timestamp": "2025-12-16T23:30:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/09e726021418ac873b9d6878f9ddd6dae68230ae"
        },
        "date": 1765928999292,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 579773.838,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.439,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 6.988,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.701,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.051,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.289,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 20.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 7697.38,
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
          "id": "36502f82bb9a840caebdb974afa425352d7d0956",
          "message": "Reclaim pages after segment compaction (#78)\n\n## Summary\n- Add page reclamation during segment compaction to prevent index bloat\n- Pages from merged source segments are now freed to PostgreSQL's FSM\n- New allocations check FSM first, reusing freed pages when available\n- Demote spill/merge notices to DEBUG1 to reduce build noise\n\n## Results (internal benchmark, 2.1M slack messages)\n\n| Metric | Before | After | Improvement |\n|--------|--------|-------|-------------|\n| Index Size | 831 MB | 473 MB | **43% smaller** |\n| Pages Reused | 0 | 45,026 | ✓ |",
          "timestamp": "2025-12-17T02:09:26Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/36502f82bb9a840caebdb974afa425352d7d0956"
        },
        "date": 1765952507704,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 585244.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.548,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.131,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.065,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.506,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 13.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2807.35,
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
          "id": "b82dea59ef6d9c20c88742ce904acb4a94388246",
          "message": "Implement V2 segment format with block storage for BMW optimization\n\nThis implements Phase 1 of the block storage optimization plan:\n\nV2 segment format changes:\n- Block-based posting storage: 128 docs per block with TpBlockPosting (8 bytes)\n- Skip index with TpSkipEntry (16 bytes) enabling BMW block skipping\n- Fieldnorm table using Lucene SmallFloat encoding (1 byte per doc)\n- CTID map for segment-local doc_id → heap tuple lookup\n- TpDictEntryV2 (12 bytes) with skip_index_offset + block_count\n\nNew infrastructure:\n- docmap.c/h: Document ID mapping with CTID hash table\n- fieldnorm.h: Lucene SmallFloat encode/decode with precomputed table\n- segment_query.c: V2 query iterator with block-based iteration\n\nKey changes:\n- All segment writers switched from V1 to V2 (index build, spill, merge)\n- Merge completely rewritten to read V2 sources and write V2 output\n- Dump function updated to display V2 format details\n- Metapage version bumped to 5 (breaking change)\n\nThe V2 format reduces storage (posting: 14→8 bytes) and enables\nfuture BMW optimization by providing block-level statistics for\nearly termination during top-k queries.\n\nV1 code retained but unused - will be removed in future cleanup.",
          "timestamp": "2025-12-17T21:26:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b82dea59ef6d9c20c88742ce904acb4a94388246"
        },
        "date": 1766008357664,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 928284.074,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.215,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 8.037,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.953,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.053,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.495,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 17.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
        "date": 1765926063229,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia - Index Build Time",
            "value": 19545.088,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Short Query (1 word)",
            "value": 3.623,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Medium Query (3 words)",
            "value": 1.259,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Long Query (question)",
            "value": 1.152,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Common Term Query",
            "value": 2.822,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Rare Term Query",
            "value": 3.512,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Index Size",
            "value": 158.62,
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
          "id": "09e726021418ac873b9d6878f9ddd6dae68230ae",
          "message": "Improve benchmark dashboard: dataset sizes and compact layout (#77)\n\n## Summary\n\n- Include document count in benchmark metric names (e.g., \"msmarco (8.8M\ndocs) - Index Build Time\")\n- Create 3-column responsive grid layout for benchmark dashboard\n(responsive down to 1 column on mobile)\n- Auto-deploy updated index.html to gh-pages after benchmarks run\n\n## Changes\n\n**format_for_action.sh**: Format document counts (1.4K, 8.8M, etc.) and\ninclude in metric names\n\n**benchmarks/gh-pages/index.html**: New compact layout with:\n- 3-column grid (responsive)\n- Smaller chart sizes\n- Cleaner styling\n- Dataset sections with headers\n\n**benchmark.yml**: Add step to deploy updated index.html to gh-pages",
          "timestamp": "2025-12-16T23:30:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/09e726021418ac873b9d6878f9ddd6dae68230ae"
        },
        "date": 1765929000531,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19905.079,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.609,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.226,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.201,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.769,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.561,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 158.62,
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
          "id": "36502f82bb9a840caebdb974afa425352d7d0956",
          "message": "Reclaim pages after segment compaction (#78)\n\n## Summary\n- Add page reclamation during segment compaction to prevent index bloat\n- Pages from merged source segments are now freed to PostgreSQL's FSM\n- New allocations check FSM first, reusing freed pages when available\n- Demote spill/merge notices to DEBUG1 to reduce build noise\n\n## Results (internal benchmark, 2.1M slack messages)\n\n| Metric | Before | After | Improvement |\n|--------|--------|-------|-------------|\n| Index Size | 831 MB | 473 MB | **43% smaller** |\n| Pages Reused | 0 | 45,026 | ✓ |",
          "timestamp": "2025-12-17T02:09:26Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/36502f82bb9a840caebdb974afa425352d7d0956"
        },
        "date": 1765952508967,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 20062.195,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.589,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.261,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.127,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.754,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.58,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 153.1,
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
          "id": "b82dea59ef6d9c20c88742ce904acb4a94388246",
          "message": "Implement V2 segment format with block storage for BMW optimization\n\nThis implements Phase 1 of the block storage optimization plan:\n\nV2 segment format changes:\n- Block-based posting storage: 128 docs per block with TpBlockPosting (8 bytes)\n- Skip index with TpSkipEntry (16 bytes) enabling BMW block skipping\n- Fieldnorm table using Lucene SmallFloat encoding (1 byte per doc)\n- CTID map for segment-local doc_id → heap tuple lookup\n- TpDictEntryV2 (12 bytes) with skip_index_offset + block_count\n\nNew infrastructure:\n- docmap.c/h: Document ID mapping with CTID hash table\n- fieldnorm.h: Lucene SmallFloat encode/decode with precomputed table\n- segment_query.c: V2 query iterator with block-based iteration\n\nKey changes:\n- All segment writers switched from V1 to V2 (index build, spill, merge)\n- Merge completely rewritten to read V2 sources and write V2 output\n- Dump function updated to display V2 format details\n- Metapage version bumped to 5 (breaking change)\n\nThe V2 format reduces storage (posting: 14→8 bytes) and enables\nfuture BMW optimization by providing block-level statistics for\nearly termination during top-k queries.\n\nV1 code retained but unused - will be removed in future cleanup.",
          "timestamp": "2025-12-17T21:26:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b82dea59ef6d9c20c88742ce904acb4a94388246"
        },
        "date": 1766008362156,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 24273.554,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 6.475,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 28.081,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 36.746,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 14.583,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 18.87,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}