window.BENCHMARK_DATA = {
  "lastUpdate": 1767899322659,
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
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "ffb20994d4373419d57b848c7b9659105c13c62a",
          "message": "Fix parallel build crashes with mid-scan memtable spills\n\nThree bugs were causing parallel builds to crash or produce incorrect\nresults with 500K+ row tables:\n\n1. Use-after-free in tp_local_memtable_clear: The TpLocalMemtable struct\n   was allocated inside its own memory context. When MemoryContextReset\n   was called, it freed the struct itself. Fixed by allocating the struct\n   in the parent context.\n\n2. Worker info initialization off-by-one: The initialization loops used\n   nworkers (background workers only) instead of worker_count (bg + leader).\n   This left worker_info[1] uninitialized when only 1 bg worker launched,\n   causing segment_head to be 0 instead of InvalidBlockNumber.\n\n3. Metapage stats not loaded for segment-only indexes: Parallel builds\n   write directly to segments without docid pages, so recovery didn't\n   load total_docs/total_len from the metapage. Fixed by loading stats\n   from metapage after tp_rebuild_posting_lists_from_docids.\n\nAlso adds block validation in segment writer flush and cleans up\nexcessive debug logging.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-08T02:31:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ffb20994d4373419d57b848c7b9659105c13c62a"
        },
        "date": 1767840758459,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 257.368,
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
          "id": "d05f3592745e14cf24e517041db12baa7647bf9c",
          "message": "Reduce memtable spill threshold (#117)\n\nReduces `memtable_spill_threshold` default from 32M to 8M posting\nentries.\n\nSmaller threshold reduces peak DSA memory during index builds,\npreventing OOM on large datasets.\n\nTesting\n- Verified 50M document benchmark completes without OOM",
          "timestamp": "2026-01-08T05:29:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d05f3592745e14cf24e517041db12baa7647bf9c"
        },
        "date": 1767853368618,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 237.647,
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
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "8bbe202cc4873cfcf6860fd6c5fc5d69cd6704fe",
          "message": "Fix document count extraction for all benchmark datasets\n\nThe metrics extraction script only looked for \"Passages loaded\" as\na fallback pattern, missing Cranfield (\"Documents loaded\") and\nWikipedia (\"Articles loaded\") benchmarks.\n\nNow handles all three patterns with flexible spacing to match psql\noutput formatting.",
          "timestamp": "2026-01-08T05:26:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8bbe202cc4873cfcf6860fd6c5fc5d69cd6704fe"
        },
        "date": 1767899316813,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 215.08,
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
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "ffb20994d4373419d57b848c7b9659105c13c62a",
          "message": "Fix parallel build crashes with mid-scan memtable spills\n\nThree bugs were causing parallel builds to crash or produce incorrect\nresults with 500K+ row tables:\n\n1. Use-after-free in tp_local_memtable_clear: The TpLocalMemtable struct\n   was allocated inside its own memory context. When MemoryContextReset\n   was called, it freed the struct itself. Fixed by allocating the struct\n   in the parent context.\n\n2. Worker info initialization off-by-one: The initialization loops used\n   nworkers (background workers only) instead of worker_count (bg + leader).\n   This left worker_info[1] uninitialized when only 1 bg worker launched,\n   causing segment_head to be 0 instead of InvalidBlockNumber.\n\n3. Metapage stats not loaded for segment-only indexes: Parallel builds\n   write directly to segments without docid pages, so recovery didn't\n   load total_docs/total_len from the metapage. Fixed by loading stats\n   from metapage after tp_rebuild_posting_lists_from_docids.\n\nAlso adds block validation in segment writer flush and cleans up\nexcessive debug logging.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-08T02:31:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ffb20994d4373419d57b848c7b9659105c13c62a"
        },
        "date": 1767840761064,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 327022.306,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 20.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 22.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 25.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 28.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 35.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 40.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 50.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 72.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 37.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 10154.22,
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
          "id": "d05f3592745e14cf24e517041db12baa7647bf9c",
          "message": "Reduce memtable spill threshold (#117)\n\nReduces `memtable_spill_threshold` default from 32M to 8M posting\nentries.\n\nSmaller threshold reduces peak DSA memory during index builds,\npreventing OOM on large datasets.\n\nTesting\n- Verified 50M document benchmark completes without OOM",
          "timestamp": "2026-01-08T05:29:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d05f3592745e14cf24e517041db12baa7647bf9c"
        },
        "date": 1767853370416,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 541670.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 11.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 14.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 17.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 20.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 27.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 33.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 44.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 66.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 30.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2422.72,
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
          "id": "8bbe202cc4873cfcf6860fd6c5fc5d69cd6704fe",
          "message": "Fix document count extraction for all benchmark datasets\n\nThe metrics extraction script only looked for \"Passages loaded\" as\na fallback pattern, missing Cranfield (\"Documents loaded\") and\nWikipedia (\"Articles loaded\") benchmarks.\n\nNow handles all three patterns with flexible spacing to match psql\noutput formatting.",
          "timestamp": "2026-01-08T05:26:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8bbe202cc4873cfcf6860fd6c5fc5d69cd6704fe"
        },
        "date": 1767899319378,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 329719.428,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 32.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 35.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 38.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 41.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 50.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 54.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 66.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 90.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 52.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 10154.22,
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
          "id": "ffb20994d4373419d57b848c7b9659105c13c62a",
          "message": "Fix parallel build crashes with mid-scan memtable spills\n\nThree bugs were causing parallel builds to crash or produce incorrect\nresults with 500K+ row tables:\n\n1. Use-after-free in tp_local_memtable_clear: The TpLocalMemtable struct\n   was allocated inside its own memory context. When MemoryContextReset\n   was called, it freed the struct itself. Fixed by allocating the struct\n   in the parent context.\n\n2. Worker info initialization off-by-one: The initialization loops used\n   nworkers (background workers only) instead of worker_count (bg + leader).\n   This left worker_info[1] uninitialized when only 1 bg worker launched,\n   causing segment_head to be 0 instead of InvalidBlockNumber.\n\n3. Metapage stats not loaded for segment-only indexes: Parallel builds\n   write directly to segments without docid pages, so recovery didn't\n   load total_docs/total_len from the metapage. Fixed by loading stats\n   from metapage after tp_rebuild_posting_lists_from_docids.\n\nAlso adds block validation in segment writer flush and cleans up\nexcessive debug logging.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-08T02:31:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ffb20994d4373419d57b848c7b9659105c13c62a"
        },
        "date": 1767840763290,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia - Index Build Time",
            "value": 8261.723,
            "unit": "ms"
          },
          {
            "name": "wikipedia - Index Size",
            "value": 195.8,
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
          "id": "d05f3592745e14cf24e517041db12baa7647bf9c",
          "message": "Reduce memtable spill threshold (#117)\n\nReduces `memtable_spill_threshold` default from 32M to 8M posting\nentries.\n\nSmaller threshold reduces peak DSA memory during index builds,\npreventing OOM on large datasets.\n\nTesting\n- Verified 50M document benchmark completes without OOM",
          "timestamp": "2026-01-08T05:29:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d05f3592745e14cf24e517041db12baa7647bf9c"
        },
        "date": 1767853372328,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18646.843,
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
          "id": "8bbe202cc4873cfcf6860fd6c5fc5d69cd6704fe",
          "message": "Fix document count extraction for all benchmark datasets\n\nThe metrics extraction script only looked for \"Passages loaded\" as\na fallback pattern, missing Cranfield (\"Documents loaded\") and\nWikipedia (\"Articles loaded\") benchmarks.\n\nNow handles all three patterns with flexible spacing to match psql\noutput formatting.",
          "timestamp": "2026-01-08T05:26:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8bbe202cc4873cfcf6860fd6c5fc5d69cd6704fe"
        },
        "date": 1767899322047,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7979.057,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 195.8,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}