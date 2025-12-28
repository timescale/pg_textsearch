window.BENCHMARK_DATA = {
  "lastUpdate": 1766950899925,
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
          "id": "227818c762af2137ec92daf47c591c04b48fe196",
          "message": "Cache fieldnorm and CTID tables at segment open for faster iteration\n\nV2 segment queries were slow because the iterator performed per-posting\nI/O to look up CTIDs and fieldnorms. This change preloads both tables\ninto memory when opening a V2 segment reader.\n\nPerformance improvement on 50K document benchmark:\n- Common term queries: 2.1-2.6x faster\n- Buffer hits reduced by 23-34x (e.g., 30K → 900)\n\nMemory overhead per segment reader:\n- Fieldnorm: 1 byte per document\n- CTID map: 6 bytes per document\n- Total: ~7 bytes per document (e.g., 700KB for 100K docs)\n\nThe caching remains beneficial with BMW optimization since we still\nneed CTIDs for result output and fieldnorms for scoring on blocks\nthat aren't skipped.",
          "timestamp": "2025-12-17T22:23:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/227818c762af2137ec92daf47c591c04b48fe196"
        },
        "date": 1766011818087,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 272.074,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.202,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.261,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.404,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.204,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.319,
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
          "id": "8a1344956d99e719a1d323e0754b1343625bf175",
          "message": "Limit V2 cache to segments with <=100K docs\n\nThe unconditional caching caused severe regression on large segments\n(MS MARCO 8.8M docs was 5-7x slower) because:\n- Loading 60MB of cache data upfront is expensive\n- Top-k queries only access a small fraction of documents\n- PostgreSQL's buffer cache efficiently handles sparse access patterns\n\nWith 100K threshold:\n- Small segments (flush, early compaction): cached for fast iteration\n- Large segments (late compaction): use per-posting reads via buffer cache",
          "timestamp": "2025-12-17T23:16:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8a1344956d99e719a1d323e0754b1343625bf175"
        },
        "date": 1766014874771,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 243.626,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.143,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.213,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.383,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.144,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.325,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.02,
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
          "id": "aa2ed0705d742a148e5d62226bc20dcabdc9eca2",
          "message": "Store fieldnorm inline in TpBlockPosting for V2 segments\n\nEliminates per-posting fieldnorm lookups during query execution by\nstoring the encoded fieldnorm directly in each TpBlockPosting entry.\nThis uses existing padding bytes, so there's zero storage overhead.\n\nChanges:\n- TpBlockPosting: uint8 fieldnorm replaces part of reserved padding\n- V2 segment writer: populates fieldnorm from docmap during write\n- V2 segment reader: reads fieldnorm directly from posting\n- Segment merge: reads/writes inline fieldnorm during merge\n- Removed cached_fieldnorms from TpSegmentReader (no longer needed)\n\nThe separate fieldnorm table is still written for backward compatibility\nand potential future use with compressed formats.",
          "timestamp": "2025-12-18T00:34:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/aa2ed0705d742a148e5d62226bc20dcabdc9eca2"
        },
        "date": 1766023468613,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 244.274,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.077,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.165,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.186,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.366,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.03,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766039068812,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 246.414,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 4.178,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.204,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.415,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.196,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.351,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.11,
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
          "id": "1080101c3bf0a2e34ccd3c7b457da118e8253885",
          "message": "Change docmap lookup failure from WARNING to ERROR\n\nA missing CTID in the docmap indicates a data integrity bug that should\nnot be silently handled. Fail fast with a clear error message instead.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-18T15:12:49Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1080101c3bf0a2e34ccd3c7b457da118e8253885"
        },
        "date": 1766072522491,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 247.89,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.122,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.321,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.44,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.212,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.337,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.14,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766125236072,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 217.369,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 2.804,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 3.779,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.08,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 2.843,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.093,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.72,
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
          "id": "c39a2afa819a5b978710d67971ff2af798a2453e",
          "message": "Clarify tp_docmap_create is constructor",
          "timestamp": "2025-12-19T21:10:41Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c39a2afa819a5b978710d67971ff2af798a2453e"
        },
        "date": 1766180584757,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.289,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.192,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.236,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.604,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.181,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.371,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4,
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
          "id": "e24ccf3f0de3086bbff751979cbc7938179e0aa8",
          "message": "Optimize merge: replace hash lookups with direct mapping arrays\n\nDuring segment merge, each posting required a hash lookup to convert\nCTID → new_doc_id. This showed up as ~10% of CPU time in profiling.\n\nReplace with O(1) array lookups by building per-source mapping arrays\n(old_doc_id → new_doc_id) during docmap construction. Also eliminate\nunnecessary fieldnorm decode/re-encode by passing through the raw byte.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-19T22:31:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e24ccf3f0de3086bbff751979cbc7938179e0aa8"
        },
        "date": 1766184768501,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 252.795,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.113,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.166,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.426,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.14,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.312,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.07,
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
          "id": "bb55c7b0b7a448d93bad0b518b62f3701e798036",
          "message": "Add v0.4.0 for compression",
          "timestamp": "2025-12-20T01:24:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bb55c7b0b7a448d93bad0b518b62f3701e798036"
        },
        "date": 1766194065015,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 243.389,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.089,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.414,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.183,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 3.164,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766211667860,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 270.91,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.139,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.2,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.149,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.335,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.98,
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
          "id": "4a3fe86f16b7ba2ec78317f38debaa376b7e70a1",
          "message": "Switch V2 segment format to streaming layout\n\nChange format from [dict] -> [skip index] -> [postings] to\n[dict] -> [postings] -> [skip index]. This enables single-pass\nstreaming writes instead of 9 passes during segment creation.\n\nThe new format writes postings immediately after dict entries,\naccumulating skip index entries in memory, then writes the skip\nindex at the end. Dict entries are updated with correct offsets\nafter all postings are written.\n\nThis addresses ~30% performance regression seen in segment merge\nbenchmarks by eliminating multiple passes over the data.",
          "timestamp": "2025-12-21T02:14:15Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4a3fe86f16b7ba2ec78317f38debaa376b7e70a1"
        },
        "date": 1766283734578,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 227.461,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 2.779,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 3.771,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.077,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 2.849,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.102,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.72,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766289617826,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 240.673,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.073,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.194,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.393,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.169,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.326,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.08,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766293711575,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 246.529,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.149,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.275,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.441,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.213,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.35,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.09,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766293764370,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 244.88,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.127,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.247,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.492,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.229,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.373,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.3,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766298086305,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 246.473,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.522,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 7.043,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.385,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.162,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.347,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766370351875,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 261.713,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.355,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.56,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.745,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.448,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.542,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.41,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766371107529,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 247.739,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.219,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.463,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.256,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.393,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.08,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766384445277,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 219.967,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 5.096,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 3.807,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.105,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 2.863,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.099,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.69,
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
          "id": "15b8df351172d0321c2ca4035afcf211b4861f96",
          "message": "Implement V2 segment format with block storage for BMW optimization (#81)\n\n## Summary\n- Implements Phase 1 of block storage optimization with V2 segment\nformat\n- Block-based posting storage (128 docs/block) with skip index for\nfuture BMW optimization\n- Fieldnorm table using Lucene SmallFloat encoding (1 byte per doc)\n- Segment-local document IDs with CTID map for heap tuple lookup\n- All segment writes switched to V2 format (breaking change, metapage\nversion 5)\n\n## Compatibility\n- **Read**: Supports both V1 and V2 segments (query code branches on\n`header->version`)\n- **Write**: V2 only - all new segments use V2 format\n- **Upgrade path**: Existing indexes must be rebuilt (standard for\npre-1.0 releases)\n\n## Key Changes\n- **New files**: `docmap.c/h` (CTID mapping), `fieldnorm.h` (length\nquantization)\n- **segment_merge.c**: Complete rewrite to read V2 sources and write V2\noutput\n- **segment_query.c**: V2 query iterator with block-based iteration\n- **segment.c**: V2 writer and dump function updates\n\n## Format Details\n- `TpBlockPosting` (8 bytes): doc_id + frequency (down from 14-byte\nTpSegmentPosting)\n- `TpSkipEntry` (16 bytes): per-block statistics for BMW block skipping\n- `TpDictEntryV2` (12 bytes): skip_index_offset + block_count + doc_freq\n\n## Configuration Changes\n- **Memtable spill threshold**: Bumped from 800K to 32M posting entries\n(~1M docs/segment)\n- **Benchmark config**: `segments_per_level = 16` for fewer merges\nduring benchmarking\n- **Segment statistics**: Added `bm25_summarize_index()` calls to\nbenchmark load scripts\n\n## Testing\n- All 29 regression tests pass\n- Merge, segment, and scoring tests validate V2 format correctness",
          "timestamp": "2025-12-22T08:47:53Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/15b8df351172d0321c2ca4035afcf211b4861f96"
        },
        "date": 1766431550165,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 250.28,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.159,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.225,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.613,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.283,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.357,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.05,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766471005540,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 215.789,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 2.766,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 3.762,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.084,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 2.868,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.1,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.6,
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
          "id": "8c86fccb2939836185fc141daf2fa762a09fb9fb",
          "message": "Implement top-k partial sort for BM25 result extraction\n\nReplace qsort with a custom partial quicksort that only sorts the\ntop-k elements needed for results. This is O(n + k log k) instead\nof O(n log n), providing significant speedup when k << n.\n\nKey optimizations:\n- Partial quicksort only recurses into partitions containing top-k\n- Inlined score comparison eliminates function call overhead\n- Insertion sort for small subarrays (< 16 elements)\n- Median-of-three pivot selection for better partitioning\n\nPerformance improvement: ~40% faster for multi-term queries on\nMS-MARCO (8.8M docs): 565ms -> 340ms for 5-term query.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T03:44:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8c86fccb2939836185fc141daf2fa762a09fb9fb"
        },
        "date": 1766548623680,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.99,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.298,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.567,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.604,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.33,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.552,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.2,
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
          "id": "2b9fe6f74c1bcc93bb69800729a8e1a207e7e60a",
          "message": "Add CTID tiebreaker for deterministic sort ordering\n\nWhen documents have equal BM25 scores, sort by CTID ascending to ensure\ndeterministic, reproducible query results. This makes the partial\nquicksort stable for equal-scored documents.\n\nUpdate limits test expected output to reflect the new deterministic\nordering.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T04:02:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2b9fe6f74c1bcc93bb69800729a8e1a207e7e60a"
        },
        "date": 1766549139194,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 270.387,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.232,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.077,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.524,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.025,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.348,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.88,
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
          "id": "2e60e1e605ae9ff733796267fc162d62f3d07117",
          "message": "Add CTID tiebreaker for deterministic sort ordering\n\nWhen documents have equal BM25 scores, sort by CTID ascending to ensure\ndeterministic, reproducible query results. This makes the partial\nquicksort stable for equal-scored documents.\n\nUpdate limits test expected output to reflect the new deterministic\nordering.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T04:02:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2e60e1e605ae9ff733796267fc162d62f3d07117"
        },
        "date": 1766550388313,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 248.478,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 2.995,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 3.997,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 2.996,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.31,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.88,
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
          "id": "a2d302bf43d00d0472adb696472575edcdb38318",
          "message": "Fix expected test output for deterministic CTID ordering",
          "timestamp": "2025-12-24T04:45:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a2d302bf43d00d0472adb696472575edcdb38318"
        },
        "date": 1766552226222,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 241.339,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 2.98,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.045,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.222,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.047,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.317,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.82,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766557307981,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 245.684,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.137,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.309,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.496,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.249,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.08,
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
          "id": "014207fbe339343c2329b04fa32a7527c051a985",
          "message": "Fix hash function to use all 8 bytes of CTID key\n\nThe FNV-1a hash was only processing bytes 0-5 (bits 0-47), missing\nthe high 16 bits of block number (bits 48-63). For tables > 512MB,\nthis would cause hash collisions for CTIDs differing only in those\nbits.\n\nAlso fixed incorrect comment claiming 24-byte entry size (it's 16).",
          "timestamp": "2025-12-24T07:33:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/014207fbe339343c2329b04fa32a7527c051a985"
        },
        "date": 1766632077290,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 238.64,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.058,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.076,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.327,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.106,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.384,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.09,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766643732174,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 262.517,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.088,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.22,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.429,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.146,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.349,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.12,
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
          "id": "53594e02f7d43fb420652bc403d6a440294f5ea0",
          "message": "Eliminate duplicate score computation with planner hook\n\nAdd a planner hook that replaces resjunk ORDER BY score expressions\nwith a cheap stub function that returns the cached score from the\nindex scan. This avoids computing BM25 scores twice (once in the\nindex scan, once in expression evaluation).\n\n- Add tp_cached_score infrastructure in index.c\n- Add bm25_get_current_score() stub function in query.c\n- Add planner hook to detect BM25 IndexScan and replace expressions\n- Only replace when BM25 IndexScan is present (safe for SeqScan fallback)\n\nAlso add fieldnorm_discrepancy test documenting score corruption\nafter L0->L1 merge (tracked in issue #93).",
          "timestamp": "2025-12-26T01:50:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/53594e02f7d43fb420652bc403d6a440294f5ea0"
        },
        "date": 1766715023393,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 234.821,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 1.611,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 2.175,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 1.781,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 1.619,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 1.226,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 2.03,
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
          "id": "979749163401fa76b29a83e8cb49953d5f1f2076",
          "message": "Revert custom hash table, use PostgreSQL dynahash\n\nRevert commits 014207f and a422e57 that introduced a custom\nlinear-probing hash table. Profiling showed tp_doc_score_table_insert\ntaking ~50% of query time, and the custom FNV-1a hash was doing\n8 multiplies per insert.\n\nPostgreSQL's built-in dynahash is well-tested and optimized.\nStick with it unless we have compelling evidence it's a bottleneck.",
          "timestamp": "2025-12-26T02:51:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/979749163401fa76b29a83e8cb49953d5f1f2076"
        },
        "date": 1766722383028,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 237.235,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 1.595,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 2.147,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 1.764,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 1.637,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 1.229,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 2.11,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766730114479,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 250.188,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.17,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.245,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.486,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.184,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.37,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766816362230,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 240.411,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 2.782,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 3.792,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.087,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 2.848,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.092,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 3.61,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766902888598,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 244.911,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Short Query (1 word)",
            "value": 3.118,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Medium Query (3 words)",
            "value": 4.323,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Long Query (question)",
            "value": 3.58,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Common Term Query",
            "value": 3.244,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Rare Term Query",
            "value": 2.349,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Avg Query Latency (20 queries)",
            "value": 4.03,
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
          "id": "227818c762af2137ec92daf47c591c04b48fe196",
          "message": "Cache fieldnorm and CTID tables at segment open for faster iteration\n\nV2 segment queries were slow because the iterator performed per-posting\nI/O to look up CTIDs and fieldnorms. This change preloads both tables\ninto memory when opening a V2 segment reader.\n\nPerformance improvement on 50K document benchmark:\n- Common term queries: 2.1-2.6x faster\n- Buffer hits reduced by 23-34x (e.g., 30K → 900)\n\nMemory overhead per segment reader:\n- Fieldnorm: 1 byte per document\n- CTID map: 6 bytes per document\n- Total: ~7 bytes per document (e.g., 700KB for 100K docs)\n\nThe caching remains beneficial with BMW optimization since we still\nneed CTIDs for result output and fieldnorms for scoring on blocks\nthat aren't skipped.",
          "timestamp": "2025-12-17T22:23:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/227818c762af2137ec92daf47c591c04b48fe196"
        },
        "date": 1766011820405,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 995733.065,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 27.662,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 36.254,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 64.646,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 61.796,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 65.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "8a1344956d99e719a1d323e0754b1343625bf175",
          "message": "Limit V2 cache to segments with <=100K docs\n\nThe unconditional caching caused severe regression on large segments\n(MS MARCO 8.8M docs was 5-7x slower) because:\n- Loading 60MB of cache data upfront is expensive\n- Top-k queries only access a small fraction of documents\n- PostgreSQL's buffer cache efficiently handles sparse access patterns\n\nWith 100K threshold:\n- Small segments (flush, early compaction): cached for fast iteration\n- Large segments (late compaction): use per-posting reads via buffer cache",
          "timestamp": "2025-12-17T23:16:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8a1344956d99e719a1d323e0754b1343625bf175"
        },
        "date": 1766014876334,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 906908.842,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.354,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.946,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 13.115,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.054,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.973,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 16.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "aa2ed0705d742a148e5d62226bc20dcabdc9eca2",
          "message": "Store fieldnorm inline in TpBlockPosting for V2 segments\n\nEliminates per-posting fieldnorm lookups during query execution by\nstoring the encoded fieldnorm directly in each TpBlockPosting entry.\nThis uses existing padding bytes, so there's zero storage overhead.\n\nChanges:\n- TpBlockPosting: uint8 fieldnorm replaces part of reserved padding\n- V2 segment writer: populates fieldnorm from docmap during write\n- V2 segment reader: reads fieldnorm directly from posting\n- Segment merge: reads/writes inline fieldnorm during merge\n- Removed cached_fieldnorms from TpSegmentReader (no longer needed)\n\nThe separate fieldnorm table is still written for backward compatibility\nand potential future use with compressed formats.",
          "timestamp": "2025-12-18T00:34:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/aa2ed0705d742a148e5d62226bc20dcabdc9eca2"
        },
        "date": 1766023469771,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 868556.606,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.234,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.223,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.062,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.043,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.872,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "a8f481717908a9cf5461e1debe3d44d27a60fef5",
          "message": "Simplify graph click menu - single 'Find Profile' link to index",
          "timestamp": "2025-12-18T04:09:55Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a8f481717908a9cf5461e1debe3d44d27a60fef5"
        },
        "date": 1766031299100,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 901533.994,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.201,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.154,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.341,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.046,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.888,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766039071114,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 594857.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.602,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.206,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.124,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.585,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 14.06,
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
          "id": "1080101c3bf0a2e34ccd3c7b457da118e8253885",
          "message": "Change docmap lookup failure from WARNING to ERROR\n\nA missing CTID in the docmap indicates a data integrity bug that should\nnot be silently handled. Fail fast with a clear error message instead.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-18T15:12:49Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1080101c3bf0a2e34ccd3c7b457da118e8253885"
        },
        "date": 1766072524833,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 841366.111,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.501,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.912,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 13.599,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.066,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.162,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 16.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "1080101c3bf0a2e34ccd3c7b457da118e8253885",
          "message": "Change docmap lookup failure from WARNING to ERROR\n\nA missing CTID in the docmap indicates a data integrity bug that should\nnot be silently handled. Fail fast with a clear error message instead.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-18T15:12:49Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1080101c3bf0a2e34ccd3c7b457da118e8253885"
        },
        "date": 1766075728499,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 762151.056,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.201,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.607,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.941,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766125238389,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 543532.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.265,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 6.759,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.468,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.033,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.166,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 13.01,
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
          "id": "c39a2afa819a5b978710d67971ff2af798a2453e",
          "message": "Clarify tp_docmap_create is constructor",
          "timestamp": "2025-12-19T21:10:41Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c39a2afa819a5b978710d67971ff2af798a2453e"
        },
        "date": 1766180585946,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 776214.815,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.293,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.364,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.556,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.053,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.974,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "e24ccf3f0de3086bbff751979cbc7938179e0aa8",
          "message": "Optimize merge: replace hash lookups with direct mapping arrays\n\nDuring segment merge, each posting required a hash lookup to convert\nCTID → new_doc_id. This showed up as ~10% of CPU time in profiling.\n\nReplace with O(1) array lookups by building per-source mapping arrays\n(old_doc_id → new_doc_id) during docmap construction. Also eliminate\nunnecessary fieldnorm decode/re-encode by passing through the raw byte.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-19T22:31:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e24ccf3f0de3086bbff751979cbc7938179e0aa8"
        },
        "date": 1766184770063,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 725277.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.334,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.494,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.03,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 14.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.47,
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
          "id": "e37f6971667a00f11373b266ad15648bbe04db38",
          "message": "Fix chart fill: use rgba() for proper transparency",
          "timestamp": "2025-12-20T00:12:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e37f6971667a00f11373b266ad15648bbe04db38"
        },
        "date": 1766189778391,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 671125.618,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.323,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.342,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.181,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.049,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.982,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 14.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "8fd6589c4c289b15d7a4a5046e8e9d7f4a47650d",
          "message": "Bump version to 0.2.0-dev, target 1.0.0 GA in Feb 2025\n\n- Skip 0.1.1-dev, go directly from 0.1.0 to 0.2.0-dev\n- Update roadmaps with bigger version jumps: 0.2.0 -> 0.5.0 -> 1.0.0\n- Block storage foundation (current) targets v0.2.0\n- Query optimizations (BMW/MAXSCORE) target v0.5.0\n- Production ready GA targets v1.0.0 (Feb 2025)",
          "timestamp": "2025-12-20T01:19:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8fd6589c4c289b15d7a4a5046e8e9d7f4a47650d"
        },
        "date": 1766193701236,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 747825.846,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.467,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.491,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.476,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.043,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "bb55c7b0b7a448d93bad0b518b62f3701e798036",
          "message": "Add v0.4.0 for compression",
          "timestamp": "2025-12-20T01:24:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bb55c7b0b7a448d93bad0b518b62f3701e798036"
        },
        "date": 1766194066374,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 725833.687,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.406,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.537,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.351,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.103,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "99ff043f148ad1d05f2b39d20d8b676a8f8c3ae2",
          "message": "Revert binary search optimization for CTID lookups\n\nThe binary search optimization intended to eliminate hash lookup overhead\nduring segment writing actually caused an 8% slowdown in index build time\n(671s -> 726s for MS MARCO 8.8M docs).\n\nRoot cause: For high-volume lookups (8.8M docs * 30 terms = 264M lookups),\nO(log n) binary search (23 comparisons per lookup) is slower than O(1)\nhash lookup, even with hash overhead.\n\nThis reverts the sorted CTID array and tp_docmap_lookup_fast() function,\nkeeping the simpler and faster hash-based tp_docmap_lookup().\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-20T02:15:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/99ff043f148ad1d05f2b39d20d8b676a8f8c3ae2"
        },
        "date": 1766197856511,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 716661.176,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.342,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.577,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.879,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "99ff043f148ad1d05f2b39d20d8b676a8f8c3ae2",
          "message": "Revert binary search optimization for CTID lookups\n\nThe binary search optimization intended to eliminate hash lookup overhead\nduring segment writing actually caused an 8% slowdown in index build time\n(671s -> 726s for MS MARCO 8.8M docs).\n\nRoot cause: For high-volume lookups (8.8M docs * 30 terms = 264M lookups),\nO(log n) binary search (23 comparisons per lookup) is slower than O(1)\nhash lookup, even with hash overhead.\n\nThis reverts the sorted CTID array and tp_docmap_lookup_fast() function,\nkeeping the simpler and faster hash-based tp_docmap_lookup().\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-20T02:15:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/99ff043f148ad1d05f2b39d20d8b676a8f8c3ae2"
        },
        "date": 1766207467541,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 713937.535,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.306,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.674,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.134,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.847,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 14.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766211669628,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 586630.396,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.515,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.113,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.975,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.044,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.509,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 13.51,
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
          "id": "d560f1e5da28b15ce5e7e4186f57b0608aec1457",
          "message": "Add profiling support for V1 baseline comparison",
          "timestamp": "2025-12-20T10:06:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d560f1e5da28b15ce5e7e4186f57b0608aec1457"
        },
        "date": 1766225925327,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 549310.227,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.348,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 6.848,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.573,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.203,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 13.18,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766289619574,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 693698.357,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.287,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.404,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.188,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.071,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.868,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 14.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766293713944,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 724388.554,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.283,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.388,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 12.293,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.055,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.963,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766293766711,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 718633.988,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.333,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.565,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 13.021,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.963,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 15.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2551.58,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766298089264,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 582336.571,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.501,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 7.119,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.964,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.064,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.534,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 13.59,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766370354490,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 559309.752,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 13.049,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 41.778,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 40.362,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.043,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.511,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 98.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2203.43,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766371109765,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 570582.934,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 14.524,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 50.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 46.568,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.048,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.538,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 102.86,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766377168879,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 581327.693,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 14.268,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 46.282,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 41.647,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.549,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 100.03,
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
          "id": "b992ef444110bd0d3ee963882a7f0b9f945bd157",
          "message": "Fix query profiling: disable autovacuum, filter background workers",
          "timestamp": "2025-12-22T04:31:24Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b992ef444110bd0d3ee963882a7f0b9f945bd157"
        },
        "date": 1766378707362,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 579239.517,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.607,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 50.811,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 43.389,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 106.33,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766384448310,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 541794.414,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.275,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 6.753,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 11.442,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.034,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.076,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 12.94,
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
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "15b8df351172d0321c2ca4035afcf211b4861f96",
          "message": "Implement V2 segment format with block storage for BMW optimization (#81)\n\n## Summary\n- Implements Phase 1 of block storage optimization with V2 segment\nformat\n- Block-based posting storage (128 docs/block) with skip index for\nfuture BMW optimization\n- Fieldnorm table using Lucene SmallFloat encoding (1 byte per doc)\n- Segment-local document IDs with CTID map for heap tuple lookup\n- All segment writes switched to V2 format (breaking change, metapage\nversion 5)\n\n## Compatibility\n- **Read**: Supports both V1 and V2 segments (query code branches on\n`header->version`)\n- **Write**: V2 only - all new segments use V2 format\n- **Upgrade path**: Existing indexes must be rebuilt (standard for\npre-1.0 releases)\n\n## Key Changes\n- **New files**: `docmap.c/h` (CTID mapping), `fieldnorm.h` (length\nquantization)\n- **segment_merge.c**: Complete rewrite to read V2 sources and write V2\noutput\n- **segment_query.c**: V2 query iterator with block-based iteration\n- **segment.c**: V2 writer and dump function updates\n\n## Format Details\n- `TpBlockPosting` (8 bytes): doc_id + frequency (down from 14-byte\nTpSegmentPosting)\n- `TpSkipEntry` (16 bytes): per-block statistics for BMW block skipping\n- `TpDictEntryV2` (12 bytes): skip_index_offset + block_count + doc_freq\n\n## Configuration Changes\n- **Memtable spill threshold**: Bumped from 800K to 32M posting entries\n(~1M docs/segment)\n- **Benchmark config**: `segments_per_level = 16` for fewer merges\nduring benchmarking\n- **Segment statistics**: Added `bm25_summarize_index()` calls to\nbenchmark load scripts\n\n## Testing\n- All 29 regression tests pass\n- Merge, segment, and scoring tests validate V2 format correctness",
          "timestamp": "2025-12-22T08:47:53Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/15b8df351172d0321c2ca4035afcf211b4861f96"
        },
        "date": 1766431551614,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 622143.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 16.484,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 52.968,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 52.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.646,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 106.08,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766434012796,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 598742.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.873,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 53.798,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 51.542,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.699,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 110.12,
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
          "id": "2c5a2746f9aa6c32601e8ad95b46781615c48bb8",
          "message": "Fix iterator field names",
          "timestamp": "2025-12-22T22:14:04Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2c5a2746f9aa6c32601e8ad95b46781615c48bb8"
        },
        "date": 1766443971374,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 575100.513,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.777,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 53.735,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 48.294,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.361,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 110.88,
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
          "id": "2c5a2746f9aa6c32601e8ad95b46781615c48bb8",
          "message": "Fix iterator field names",
          "timestamp": "2025-12-22T22:14:04Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2c5a2746f9aa6c32601e8ad95b46781615c48bb8"
        },
        "date": 1766443983777,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 567705.198,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.352,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 55.272,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 49.851,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.043,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.274,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 102.49,
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
          "id": "ba0d92672d812e132a586b6d64147acbe82a16c4",
          "message": "Add benchmark query variants: with and without scores\n\n- Split query benchmarks into two modes:\n  - Without scores: ORDER BY only (index scan)\n  - With scores: SELECT score column (per-row operator call)\n- Increase profiling iterations from 100 to 300 per query type\n- Add 600 iterations total (300 no-score + 300 with-score)\n\nThis helps distinguish between index scan performance and\nper-row scoring operator performance.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-22T23:55:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ba0d92672d812e132a586b6d64147acbe82a16c4"
        },
        "date": 1766448675898,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 577230.657,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 16.616,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 54.934,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 48.892,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.044,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.426,
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
          "id": "136cfe536bf7b68682c905dead9bf2d9bc5f2002",
          "message": "Add profiling job with improved query coverage\n\nBackports profiling job from work2 branch to enable baseline profiling\non V1 segment format. Includes:\n- Profiling job with 600 query iterations (300 no-score + 300 with-score)\n- Score variant benchmarks to distinguish index scan vs operator perf\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-22T23:57:11Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/136cfe536bf7b68682c905dead9bf2d9bc5f2002"
        },
        "date": 1766448690848,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 612797.197,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 5.228,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 5.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 10.155,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 9.001,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 4442.1,
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
          "id": "646d6a73316422fd8cbd844aac3d847c4489fb7f",
          "message": "Optimize V2 block iteration with zero-copy access\n\nChange V2 segment posting iterator to use tp_segment_get_direct for\nblock data instead of tp_segment_read. This eliminates costly memcpy\noperations when block data fits within a single page (the common case).\n\nKey changes:\n- Add TpSegmentDirectAccess to V2 iterator for zero-copy block access\n- Try direct access first, fall back to copy when block spans pages\n- Properly release direct access when advancing blocks or finishing\n\nProfile showed V2 was spending ~27% CPU in tp_segment_read path\n(13.96% memcpy + 9.27% buffer manager + 3.5% read overhead) vs V1's\n~2.5% using zero-copy access. This should significantly reduce that.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-23T04:20:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/646d6a73316422fd8cbd844aac3d847c4489fb7f"
        },
        "date": 1766464428446,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 555325.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 14.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 55.729,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 45.531,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.297,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766471007200,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 514923.691,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 13.206,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 47.574,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 43.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.032,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.395,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 98.34,
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
          "id": "646d6a73316422fd8cbd844aac3d847c4489fb7f",
          "message": "Optimize V2 block iteration with zero-copy access\n\nChange V2 segment posting iterator to use tp_segment_get_direct for\nblock data instead of tp_segment_read. This eliminates costly memcpy\noperations when block data fits within a single page (the common case).\n\nKey changes:\n- Add TpSegmentDirectAccess to V2 iterator for zero-copy block access\n- Try direct access first, fall back to copy when block spans pages\n- Properly release direct access when advancing blocks or finishing\n\nProfile showed V2 was spending ~27% CPU in tp_segment_read path\n(13.96% memcpy + 9.27% buffer manager + 3.5% read overhead) vs V1's\n~2.5% using zero-copy access. This should significantly reduce that.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-23T04:20:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/646d6a73316422fd8cbd844aac3d847c4489fb7f"
        },
        "date": 1766504490687,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 570894.187,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 14.179,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 50.695,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 46.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.043,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.272,
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
          "id": "36a3da2fec00a48fa61e27f869e62d49e8304174",
          "message": "Use zero-copy for per-posting CTID lookups\n\nThe previous commit added zero-copy for block loading, but CTID lookups\nwere still using tp_segment_read for every posting (when segment has\n>100K docs). Profile showed this was still ~12% of CPU time.\n\nNow use tp_segment_get_direct for CTID lookups too, with fallback to\ncopy only when CTID spans page boundary (rare - 6 byte entries).\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-23T16:05:15Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/36a3da2fec00a48fa61e27f869e62d49e8304174"
        },
        "date": 1766506758609,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 586914.126,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 13.768,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 52.936,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 48.345,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.316,
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
          "id": "36a3da2fec00a48fa61e27f869e62d49e8304174",
          "message": "Use zero-copy for per-posting CTID lookups\n\nThe previous commit added zero-copy for block loading, but CTID lookups\nwere still using tp_segment_read for every posting (when segment has\n>100K docs). Profile showed this was still ~12% of CPU time.\n\nNow use tp_segment_get_direct for CTID lookups too, with fallback to\ncopy only when CTID spans page boundary (rare - 6 byte entries).\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-23T16:05:15Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/36a3da2fec00a48fa61e27f869e62d49e8304174"
        },
        "date": 1766506758286,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 591254.336,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 16.686,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 55.681,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 50.243,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.272,
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
          "id": "01701eb0bca20c232aca483c691e0512e9a266ef",
          "message": "Fix benchmark extraction for new query format\n\n- Support both old (5 queries) and new (10 queries) format\n- Extract no-score and with-score metrics separately\n- Fix throughput extraction for new THROUGHPUT_RESULT_NO_SCORE format\n- Add with-score metrics to benchmark action output\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-23T18:44:27Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/01701eb0bca20c232aca483c691e0512e9a266ef"
        },
        "date": 1766516034120,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 562987.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 13.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 50.492,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 49.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.327,
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
          "id": "01701eb0bca20c232aca483c691e0512e9a266ef",
          "message": "Fix benchmark extraction for new query format\n\n- Support both old (5 queries) and new (10 queries) format\n- Extract no-score and with-score metrics separately\n- Fix throughput extraction for new THROUGHPUT_RESULT_NO_SCORE format\n- Add with-score metrics to benchmark action output\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-23T18:44:27Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/01701eb0bca20c232aca483c691e0512e9a266ef"
        },
        "date": 1766516249561,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 571327.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.829,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 55.677,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 50.293,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.049,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 6.242,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 106.09,
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
          "id": "8c86fccb2939836185fc141daf2fa762a09fb9fb",
          "message": "Implement top-k partial sort for BM25 result extraction\n\nReplace qsort with a custom partial quicksort that only sorts the\ntop-k elements needed for results. This is O(n + k log k) instead\nof O(n log n), providing significant speedup when k << n.\n\nKey optimizations:\n- Partial quicksort only recurses into partitions containing top-k\n- Inlined score comparison eliminates function call overhead\n- Insertion sort for small subarrays (< 16 elements)\n- Median-of-three pivot selection for better partitioning\n\nPerformance improvement: ~40% faster for multi-term queries on\nMS-MARCO (8.8M docs): 565ms -> 340ms for 5-term query.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T03:44:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8c86fccb2939836185fc141daf2fa762a09fb9fb"
        },
        "date": 1766548626256,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 543001.889,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 20.779,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 39.371,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 43.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 26.142,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 75.69,
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
          "id": "2b9fe6f74c1bcc93bb69800729a8e1a207e7e60a",
          "message": "Add CTID tiebreaker for deterministic sort ordering\n\nWhen documents have equal BM25 scores, sort by CTID ascending to ensure\ndeterministic, reproducible query results. This makes the partial\nquicksort stable for equal-scored documents.\n\nUpdate limits test expected output to reflect the new deterministic\nordering.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T04:02:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2b9fe6f74c1bcc93bb69800729a8e1a207e7e60a"
        },
        "date": 1766549140611,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 567824.928,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 20.836,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 31.665,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 43.053,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 32.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 80.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2211.13,
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
          "id": "2e60e1e605ae9ff733796267fc162d62f3d07117",
          "message": "Add CTID tiebreaker for deterministic sort ordering\n\nWhen documents have equal BM25 scores, sort by CTID ascending to ensure\ndeterministic, reproducible query results. This makes the partial\nquicksort stable for equal-scored documents.\n\nUpdate limits test expected output to reflect the new deterministic\nordering.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T04:02:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2e60e1e605ae9ff733796267fc162d62f3d07117"
        },
        "date": 1766550389727,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 520924.906,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 17.224,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 28.168,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 33.343,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.041,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 23.183,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 53.4,
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
          "id": "a2d302bf43d00d0472adb696472575edcdb38318",
          "message": "Fix expected test output for deterministic CTID ordering",
          "timestamp": "2025-12-24T04:45:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a2d302bf43d00d0472adb696472575edcdb38318"
        },
        "date": 1766552227905,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 527986.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 16.167,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 22.533,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 29.432,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.046,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 23.451,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 43.24,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766557310438,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 588971.927,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.431,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 52.234,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 49.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.764,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 105.66,
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
          "id": "014207fbe339343c2329b04fa32a7527c051a985",
          "message": "Fix hash function to use all 8 bytes of CTID key\n\nThe FNV-1a hash was only processing bytes 0-5 (bits 0-47), missing\nthe high 16 bits of block number (bits 48-63). For tables > 512MB,\nthis would cause hash collisions for CTIDs differing only in those\nbits.\n\nAlso fixed incorrect comment claiming 24-byte entry size (it's 16).",
          "timestamp": "2025-12-24T07:33:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/014207fbe339343c2329b04fa32a7527c051a985"
        },
        "date": 1766632079732,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 532411.258,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 17.878,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 24.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 34.094,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.038,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 26.049,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 47.46,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766643734688,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 609057.359,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 15.762,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 54.211,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 52.022,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.047,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.837,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 106.05,
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
          "id": "53594e02f7d43fb420652bc403d6a440294f5ea0",
          "message": "Eliminate duplicate score computation with planner hook\n\nAdd a planner hook that replaces resjunk ORDER BY score expressions\nwith a cheap stub function that returns the cached score from the\nindex scan. This avoids computing BM25 scores twice (once in the\nindex scan, once in expression evaluation).\n\n- Add tp_cached_score infrastructure in index.c\n- Add bm25_get_current_score() stub function in query.c\n- Add planner hook to detect BM25 IndexScan and replace expressions\n- Only replace when BM25 IndexScan is present (safe for SeqScan fallback)\n\nAlso add fieldnorm_discrepancy test documenting score corruption\nafter L0->L1 merge (tracked in issue #93).",
          "timestamp": "2025-12-26T01:50:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/53594e02f7d43fb420652bc403d6a440294f5ea0"
        },
        "date": 1766715024829,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 517897.548,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 12.582,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 18.134,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 23.362,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.077,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 15.616,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 37.8,
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
          "id": "979749163401fa76b29a83e8cb49953d5f1f2076",
          "message": "Revert custom hash table, use PostgreSQL dynahash\n\nRevert commits 014207f and a422e57 that introduced a custom\nlinear-probing hash table. Profiling showed tp_doc_score_table_insert\ntaking ~50% of query time, and the custom FNV-1a hash was doing\n8 multiplies per insert.\n\nPostgreSQL's built-in dynahash is well-tested and optimized.\nStick with it unless we have compelling evidence it's a bottleneck.",
          "timestamp": "2025-12-26T02:51:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/979749163401fa76b29a83e8cb49953d5f1f2076"
        },
        "date": 1766722385568,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 549695.986,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 14.166,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 26.863,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 29.104,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.042,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 17.386,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 51.97,
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
          "id": "36c23457ed5af1901065da7fe5889ceac299b646",
          "message": "Add with-score query benchmarks to measure SELECT clause optimization\n\nAdds parallel benchmarks that include score in the SELECT clause:\n- Benchmark 1b: Query latency with score (5 query types × 10 iterations)\n- Benchmark 2b: Query throughput with score (20 queries)\n\nThe extraction script already supported with-score metrics. Updated\nformat_for_action.sh to include all with-score query latencies in\nthe graphs.\n\nThis lets us measure the impact of the stub optimization that replaces\nduplicate score expressions in SELECT with a cached value lookup.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-26T04:47:25Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/36c23457ed5af1901065da7fe5889ceac299b646"
        },
        "date": 1766725192828,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 527354.889,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 7.311,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 17.624,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 15.047,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.054,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 4.753,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 33.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query with Score",
            "value": 12.145,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query with Score",
            "value": 22.265,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query with Score",
            "value": 24.732,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query with Score",
            "value": 14.328,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency with Score",
            "value": 45.55,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766730116220,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 576987.952,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 17.433,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 54.652,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 50.827,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.725,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 106.84,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766816365194,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 516927.976,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 14.538,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 50.108,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 48.855,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.039,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.601,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 104.25,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766902890478,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 572596.558,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 13.657,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 45.078,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 43.346,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.046,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 8.561,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 98.4,
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
            "name": "SteveLauC",
            "username": "SteveLauC",
            "email": "stevelauc@outlook.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "8b9ee25614bb2d1969ac7723128ea37975c8acc6",
          "message": "fix: buildempty() should write init fork (#89)\n\nAn index's `buildempty()` should modify the init fork, not the main\nfork. This commit replaces `ReadBuffer()` with\n`ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL)` to\nfix the issue\n\nHere is how to verify the bug using commit\n20313076f2f43cab3f1a36be0f2805fea22bcf83:\n\n```sh\n$ cd pg_textsearch && git checkout 20313076f2f43cab\n$ make install\n$ psql\nsteve=# DROP EXTENSION IF EXISTS pg_textsearch;\nsteve=# create unlogged table test (content text);\nsteve=# create index idx_content on test using bm25 (content) with (text_config='english');\nsteve=# SELECT oid\nFROM pg_class\nWHERE relname = 'idx_content' AND relkind = 'i';\n  oid  \n-------\n 36129\n(1 row)\n\n#-- exit psql\n\n$ cd $PGDATA # cd to the PG data dir\n$ find . -name '36129*'\n./base/16384/36129_init\n./base/16384/36129\n\n$ ls -l ./base/16384/36129_init\n.rw------- 0 steve 24 Dec 12:25 ./base/16384/36129_init\n```\n\nYou can see that the init fork is empty, i.e., does not contain a meta\npage",
          "timestamp": "2025-12-28T19:33:45Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8b9ee25614bb2d1969ac7723128ea37975c8acc6"
        },
        "date": 1766950899412,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 537177.944,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query (1 word)",
            "value": 7.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query (3 words)",
            "value": 18.576,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query (question)",
            "value": 15.659,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Common Term Query",
            "value": 0.035,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query",
            "value": 4.725,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)",
            "value": 40.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Short Query with Score",
            "value": 12.086,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Medium Query with Score",
            "value": 22.079,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Long Query with Score",
            "value": 24.414,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Rare Term Query with Score",
            "value": 14.337,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Avg Query Latency with Score",
            "value": 53.66,
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
          "id": "227818c762af2137ec92daf47c591c04b48fe196",
          "message": "Cache fieldnorm and CTID tables at segment open for faster iteration\n\nV2 segment queries were slow because the iterator performed per-posting\nI/O to look up CTIDs and fieldnorms. This change preloads both tables\ninto memory when opening a V2 segment reader.\n\nPerformance improvement on 50K document benchmark:\n- Common term queries: 2.1-2.6x faster\n- Buffer hits reduced by 23-34x (e.g., 30K → 900)\n\nMemory overhead per segment reader:\n- Fieldnorm: 1 byte per document\n- CTID map: 6 bytes per document\n- Total: ~7 bytes per document (e.g., 700KB for 100K docs)\n\nThe caching remains beneficial with BMW optimization since we still\nneed CTIDs for result output and fieldnorms for scoring on blocks\nthat aren't skipped.",
          "timestamp": "2025-12-17T22:23:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/227818c762af2137ec92daf47c591c04b48fe196"
        },
        "date": 1766011823453,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 24856.113,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 7.064,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 27.791,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 37.277,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 15.752,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 18.051,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "8a1344956d99e719a1d323e0754b1343625bf175",
          "message": "Limit V2 cache to segments with <=100K docs\n\nThe unconditional caching caused severe regression on large segments\n(MS MARCO 8.8M docs was 5-7x slower) because:\n- Loading 60MB of cache data upfront is expensive\n- Top-k queries only access a small fraction of documents\n- PostgreSQL's buffer cache efficiently handles sparse access patterns\n\nWith 100K threshold:\n- Small segments (flush, early compaction): cached for fast iteration\n- Large segments (late compaction): use per-posting reads via buffer cache",
          "timestamp": "2025-12-17T23:16:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8a1344956d99e719a1d323e0754b1343625bf175"
        },
        "date": 1766014877595,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 23576.432,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 6.811,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 27.733,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 37.028,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 14.664,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 17.762,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "aa2ed0705d742a148e5d62226bc20dcabdc9eca2",
          "message": "Store fieldnorm inline in TpBlockPosting for V2 segments\n\nEliminates per-posting fieldnorm lookups during query execution by\nstoring the encoded fieldnorm directly in each TpBlockPosting entry.\nThis uses existing padding bytes, so there's zero storage overhead.\n\nChanges:\n- TpBlockPosting: uint8 fieldnorm replaces part of reserved padding\n- V2 segment writer: populates fieldnorm from docmap during write\n- V2 segment reader: reads fieldnorm directly from posting\n- Segment merge: reads/writes inline fieldnorm during merge\n- Removed cached_fieldnorms from TpSegmentReader (no longer needed)\n\nThe separate fieldnorm table is still written for backward compatibility\nand potential future use with compressed formats.",
          "timestamp": "2025-12-18T00:34:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/aa2ed0705d742a148e5d62226bc20dcabdc9eca2"
        },
        "date": 1766023470781,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 23125.674,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 6.806,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 27.939,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 36.856,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 14.653,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 18.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766039073067,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21196.929,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.636,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.511,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.336,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 3.054,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.834,
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
          "id": "1080101c3bf0a2e34ccd3c7b457da118e8253885",
          "message": "Change docmap lookup failure from WARNING to ERROR\n\nA missing CTID in the docmap indicates a data integrity bug that should\nnot be silently handled. Fail fast with a clear error message instead.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-18T15:12:49Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1080101c3bf0a2e34ccd3c7b457da118e8253885"
        },
        "date": 1766072562986,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 22869.536,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 7.322,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 25.979,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 37.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 14.89,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 18.405,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766125240477,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18735.398,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.223,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.04,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.536,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.365,
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
          "id": "c39a2afa819a5b978710d67971ff2af798a2453e",
          "message": "Clarify tp_docmap_create is constructor",
          "timestamp": "2025-12-19T21:10:41Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c39a2afa819a5b978710d67971ff2af798a2453e"
        },
        "date": 1766180587512,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21972.373,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.947,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.495,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.242,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.928,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.626,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "e24ccf3f0de3086bbff751979cbc7938179e0aa8",
          "message": "Optimize merge: replace hash lookups with direct mapping arrays\n\nDuring segment merge, each posting required a hash lookup to convert\nCTID → new_doc_id. This showed up as ~10% of CPU time in profiling.\n\nReplace with O(1) array lookups by building per-source mapping arrays\n(old_doc_id → new_doc_id) during docmap construction. Also eliminate\nunnecessary fieldnorm decode/re-encode by passing through the raw byte.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-19T22:31:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e24ccf3f0de3086bbff751979cbc7938179e0aa8"
        },
        "date": 1766184771352,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21366.845,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 4.813,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.842,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.776,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 3.975,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 4.824,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "bb55c7b0b7a448d93bad0b518b62f3701e798036",
          "message": "Add v0.4.0 for compression",
          "timestamp": "2025-12-20T01:24:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bb55c7b0b7a448d93bad0b518b62f3701e798036"
        },
        "date": 1766194067466,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21971.213,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.941,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.368,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.306,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.933,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.693,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.35,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766211671192,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 20010.245,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 3.541,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.536,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.317,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.983,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.588,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766289621506,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21147.287,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 1.55,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 2.322,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.641,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 3.444,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.602,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.36,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766293715844,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21955.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 1.55,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 2.264,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.681,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 3.432,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.625,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.36,
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
          "id": "be9e9979fb6f0287d7876e902d565648bc07f655",
          "message": "Fix dict entry page boundary overflow in V2 segment format\n\nThe V2 segment format's dict entry update code was writing 12-byte\nTpDictEntryV2 entries without handling the case where an entry spans\ntwo pages. When page_offset > 8156 (SEGMENT_DATA_PER_PAGE - sizeof),\nthe write would overflow into the next page's header, corrupting the\nLSN field and causing 'xlog flush request not satisfied' errors.\n\nFixed by detecting when an entry would cross a page boundary and\nsplitting the write across two pages. Applied to both tp_write_segment_v2()\nand the merge segment creation code.",
          "timestamp": "2025-12-21T03:40:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be9e9979fb6f0287d7876e902d565648bc07f655"
        },
        "date": 1766293768660,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 21565.741,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 1.468,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 2.032,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.629,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 3.398,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.765,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 144.36,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766298091686,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 20202.836,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 1.131,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 2.273,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.556,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 3.186,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.472,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 153.11,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766370357068,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19341.669,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.717,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.585,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.263,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.664,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.089,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "3380f6ffe36eb525bff41498fddc06ac15e7b90b",
          "message": "Tune segment configuration for better benchmarking\n\n- Default memtable spill threshold now 32M posting entries (~1M docs/segment)\n- Benchmark workflow uses segments_per_level=16 for fewer merges\n- Add segment statistics collection to MS MARCO and Wikipedia benchmarks",
          "timestamp": "2025-12-22T02:20:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3380f6ffe36eb525bff41498fddc06ac15e7b90b"
        },
        "date": 1766371112296,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19538.585,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.804,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.515,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.51,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.693,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "0ca3751e0f0a001e6ffb6c729890aabfd1152ae9",
          "message": "Add storage and query optimization roadmap (#79)\n\n## Summary\n\n- Adds design doc (`OPTIMIZATION_ROADMAP.md`) covering planned\noptimizations:\n- Block-Max WAND algorithm for O(k log n) top-k queries (vs current\nO(n))\n  - Block-aligned posting storage with skip lists\n  - FOR/PFOR compression targeting 50%+ space reduction\n  - Fieldnorm quantization using Lucene's SmallFloat scheme\n  - Phased implementation plan (v0.0.4 through v0.0.7)\n\n- Updates README to clarify \"not yet recommended\" production status\n\nThe roadmap draws from analysis of Tantivy and Lucene implementations,\nprioritizing asymptotic gains (BMW) over constant-factor gains\n(compression).",
          "timestamp": "2025-12-17T20:06:29Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0ca3751e0f0a001e6ffb6c729890aabfd1152ae9"
        },
        "date": 1766384450317,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18401.065,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 1.107,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 1.703,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.581,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 2.976,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 3.476,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 153.11,
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
          "id": "15b8df351172d0321c2ca4035afcf211b4861f96",
          "message": "Implement V2 segment format with block storage for BMW optimization (#81)\n\n## Summary\n- Implements Phase 1 of block storage optimization with V2 segment\nformat\n- Block-based posting storage (128 docs/block) with skip index for\nfuture BMW optimization\n- Fieldnorm table using Lucene SmallFloat encoding (1 byte per doc)\n- Segment-local document IDs with CTID map for heap tuple lookup\n- All segment writes switched to V2 format (breaking change, metapage\nversion 5)\n\n## Compatibility\n- **Read**: Supports both V1 and V2 segments (query code branches on\n`header->version`)\n- **Write**: V2 only - all new segments use V2 format\n- **Upgrade path**: Existing indexes must be rebuilt (standard for\npre-1.0 releases)\n\n## Key Changes\n- **New files**: `docmap.c/h` (CTID mapping), `fieldnorm.h` (length\nquantization)\n- **segment_merge.c**: Complete rewrite to read V2 sources and write V2\noutput\n- **segment_query.c**: V2 query iterator with block-based iteration\n- **segment.c**: V2 writer and dump function updates\n\n## Format Details\n- `TpBlockPosting` (8 bytes): doc_id + frequency (down from 14-byte\nTpSegmentPosting)\n- `TpSkipEntry` (16 bytes): per-block statistics for BMW block skipping\n- `TpDictEntryV2` (12 bytes): skip_index_offset + block_count + doc_freq\n\n## Configuration Changes\n- **Memtable spill threshold**: Bumped from 800K to 32M posting entries\n(~1M docs/segment)\n- **Benchmark config**: `segments_per_level = 16` for fewer merges\nduring benchmarking\n- **Segment statistics**: Added `bm25_summarize_index()` calls to\nbenchmark load scripts\n\n## Testing\n- All 29 regression tests pass\n- Merge, segment, and scoring tests validate V2 format correctness",
          "timestamp": "2025-12-22T08:47:53Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/15b8df351172d0321c2ca4035afcf211b4861f96"
        },
        "date": 1766431553219,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 20197.543,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.784,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.695,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.397,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.612,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.281,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766471008574,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 17930,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.561,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.031,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.042,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 4.903,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 8.116,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "8c86fccb2939836185fc141daf2fa762a09fb9fb",
          "message": "Implement top-k partial sort for BM25 result extraction\n\nReplace qsort with a custom partial quicksort that only sorts the\ntop-k elements needed for results. This is O(n + k log k) instead\nof O(n log n), providing significant speedup when k << n.\n\nKey optimizations:\n- Partial quicksort only recurses into partitions containing top-k\n- Inlined score comparison eliminates function call overhead\n- Insertion sort for small subarrays (< 16 elements)\n- Median-of-three pivot selection for better partitioning\n\nPerformance improvement: ~40% faster for multi-term queries on\nMS-MARCO (8.8M docs): 565ms -> 340ms for 5-term query.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T03:44:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8c86fccb2939836185fc141daf2fa762a09fb9fb"
        },
        "date": 1766548628747,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18575.546,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.815,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.127,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.15,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.517,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 7.817,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.49,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "2b9fe6f74c1bcc93bb69800729a8e1a207e7e60a",
          "message": "Add CTID tiebreaker for deterministic sort ordering\n\nWhen documents have equal BM25 scores, sort by CTID ascending to ensure\ndeterministic, reproducible query results. This makes the partial\nquicksort stable for equal-scored documents.\n\nUpdate limits test expected output to reflect the new deterministic\nordering.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T04:02:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2b9fe6f74c1bcc93bb69800729a8e1a207e7e60a"
        },
        "date": 1766549141859,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19805.027,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.822,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 3.703,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.863,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.156,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 5.664,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.436,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "2e60e1e605ae9ff733796267fc162d62f3d07117",
          "message": "Add CTID tiebreaker for deterministic sort ordering\n\nWhen documents have equal BM25 scores, sort by CTID ascending to ensure\ndeterministic, reproducible query results. This makes the partial\nquicksort stable for equal-scored documents.\n\nUpdate limits test expected output to reflect the new deterministic\nordering.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2025-12-24T04:02:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2e60e1e605ae9ff733796267fc162d62f3d07117"
        },
        "date": 1766550390871,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18697.987,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.54,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 3.287,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.829,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.661,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 5.403,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.566,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "a2d302bf43d00d0472adb696472575edcdb38318",
          "message": "Fix expected test output for deterministic CTID ordering",
          "timestamp": "2025-12-24T04:45:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a2d302bf43d00d0472adb696472575edcdb38318"
        },
        "date": 1766552229236,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18399.145,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.53,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 3.164,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.982,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.171,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 4.421,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.467,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766557312963,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19995.887,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.785,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.587,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.222,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.305,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.156,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "014207fbe339343c2329b04fa32a7527c051a985",
          "message": "Fix hash function to use all 8 bytes of CTID key\n\nThe FNV-1a hash was only processing bytes 0-5 (bits 0-47), missing\nthe high 16 bits of block number (bits 48-63). For tables > 512MB,\nthis would cause hash collisions for CTIDs differing only in those\nbits.\n\nAlso fixed incorrect comment claiming 24-byte entry size (it's 16).",
          "timestamp": "2025-12-24T07:33:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/014207fbe339343c2329b04fa32a7527c051a985"
        },
        "date": 1766632082190,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19269.883,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.595,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 3.056,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.899,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.082,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 4.459,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.362,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766643737351,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 20053.665,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.803,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.534,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.584,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.784,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "53594e02f7d43fb420652bc403d6a440294f5ea0",
          "message": "Eliminate duplicate score computation with planner hook\n\nAdd a planner hook that replaces resjunk ORDER BY score expressions\nwith a cheap stub function that returns the cached score from the\nindex scan. This avoids computing BM25 scores twice (once in the\nindex scan, once in expression evaluation).\n\n- Add tp_cached_score infrastructure in index.c\n- Add bm25_get_current_score() stub function in query.c\n- Add planner hook to detect BM25 IndexScan and replace expressions\n- Only replace when BM25 IndexScan is present (safe for SeqScan fallback)\n\nAlso add fieldnorm_discrepancy test documenting score corruption\nafter L0->L1 merge (tracked in issue #93).",
          "timestamp": "2025-12-26T01:50:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/53594e02f7d43fb420652bc403d6a440294f5ea0"
        },
        "date": 1766715026335,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18841.418,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.561,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 3.016,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.756,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 4.89,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 4.317,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.278,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "979749163401fa76b29a83e8cb49953d5f1f2076",
          "message": "Revert custom hash table, use PostgreSQL dynahash\n\nRevert commits 014207f and a422e57 that introduced a custom\nlinear-probing hash table. Profiling showed tp_doc_score_table_insert\ntaking ~50% of query time, and the custom FNV-1a hash was doing\n8 multiplies per insert.\n\nPostgreSQL's built-in dynahash is well-tested and optimized.\nStick with it unless we have compelling evidence it's a bottleneck.",
          "timestamp": "2025-12-26T02:51:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/979749163401fa76b29a83e8cb49953d5f1f2076"
        },
        "date": 1766722387521,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19071.963,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.649,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.136,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 1.892,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 4.904,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 5.207,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query (with score)",
            "value": 2.407,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766730117799,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19616.069,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.839,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.564,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.463,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.623,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.498,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766816367570,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18042.111,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.658,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.068,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.071,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 4.976,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 8.257,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
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
          "id": "20313076f2f43cab3f1a36be0f2805fea22bcf83",
          "message": "Style benchmark graph points by branch type (#84)\n\n## Summary\n- Main branch benchmark runs shown as filled circles (●)\n- Feature branch runs shown as X markers (✕) with white fill\n- Tooltip displays branch name alongside commit and date info\n- Added visual legend to dashboard header\n- Workflow now tracks branch info in `branch_info.js` on gh-pages (keeps\nlast 500 entries)\n\n## Testing\n- Run benchmark workflow on main and a feature branch\n- Verify points display with different styles on the dashboard\n- Hover over points to confirm branch name appears in tooltip",
          "timestamp": "2025-12-22T19:49:33Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/20313076f2f43cab3f1a36be0f2805fea22bcf83"
        },
        "date": 1766902892082,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19292.884,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Short Query (1 word)",
            "value": 2.748,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Medium Query (3 words)",
            "value": 4.477,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Long Query (question)",
            "value": 2.175,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Common Term Query",
            "value": 5.338,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Rare Term Query",
            "value": 9.062,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 66.67,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}