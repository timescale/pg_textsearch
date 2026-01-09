window.BENCHMARK_DATA = {
  "lastUpdate": 1767999911181,
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
          "id": "29f2ac79eeeed7c627d7f8d77eb94899bb19e381",
          "message": "Use threshold-based compaction for parallel builds\n\nParallel builds were forcing compaction with 2+ segments, bypassing the\nsegments_per_level threshold. This caused parallel builds to produce\ndifferent index structures than serial builds with the same settings.\n\nBenchmark showed serial builds with 8 unmerged segments at L0 had better\nquery performance (17.18ms) than parallel builds forced to 1 merged\nsegment at L1 (20.03ms) due to Block-Max WAND skip index efficiency.\n\nChange parallel builds to use tp_maybe_compact_level() which respects\nthe segments_per_level threshold, ensuring consistent index structure\nregardless of build method.",
          "timestamp": "2026-01-09T03:14:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/29f2ac79eeeed7c627d7f8d77eb94899bb19e381"
        },
        "date": 1767930948991,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 216.184,
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
          "id": "7bfb0f5fcc97cefceb692de66f6bc6381ff07518",
          "message": "Use private DSA for index builds to eliminate memory leaks (#118)\n\n## Summary\n\nEliminates memory leaks during CREATE INDEX by using a private DSA that\nis destroyed and recreated on each spill, providing perfect memory\nreclamation.\n\n### Problem\n\nAs identified in PR #117, index builds leak ~110-400MB per spill cycle\ndue to DSA fragmentation. Even with the threshold reduction in #117, a\n50M document build still leaks ~17GB cumulative memory.\n\n### Solution: Private DSA with Destroy/Recreate\n\n**Key Insight:** During CREATE INDEX, only one backend is building. We\ndon't need a shared DSA - we can use a private one and destroy it\ncompletely between spills.\n\n**Implementation:**\n```c\n// Create private DSA for build (not in global registry)\nprivate_dsa = dsa_create(tranche_id);  \n\n// After spill:\ndsa_detach(private_dsa);  // Destroys DSA + ALL memory → OS\nprivate_dsa = dsa_create(tranche_id);  // Fresh DSA for next batch\n```\n\n**Architecture:**\n- **BUILD MODE**: Private DSA, destroyed/recreated per spill → 0% memory\nleak\n- **RUNTIME MODE**: Shared DSA for concurrent inserts (unchanged)\n- **Same data structures**: dshash, posting lists work identically in\nboth modes\n- **Minimal changes**: ~100 lines of code\n\n### Changes\n\n**New functions:**\n- `tp_create_build_index_state()`: Creates private DSA instead of using\nglobal\n- `tp_recreate_build_dsa()`: Destroys old DSA and creates fresh one\n- Updated `tp_clear_memtable()`: Calls recreation in build mode\n\n**Modified files:**\n- `src/state/state.h`: Added `is_build_mode` flag\n- `src/state/state.c`: Implemented private DSA lifecycle  \n- `src/am/build.c`: Use build mode during CREATE INDEX\n\n### Expected Results\n\n**Memory profile with private DSA:**\n```\nBUILD_START: 23 MB\n1M docs: 428 MB (8M postings)\nBEFORE_SPILL: 428 MB\nAFTER_SPILL: 25 MB  ← Perfect reclamation!\n2M docs: 428 MB (no growth!)\nBEFORE_SPILL: 428 MB\nAFTER_SPILL: 25 MB  ← Still 25 MB!\n```\n\n**For any dataset size:** Peak stays at ~430MB\n\n### Comparison\n\n| Approach | 50M Docs Peak | Memory Leak | Code Changes |\n|----------|---------------|-------------|--------------|\n| Original (32M threshold) | 26GB (OOM) | 400MB/spill | 0 |\n| PR #117 (8M threshold) | ~18GB | 110MB/spill | ~150 lines |\n| **This PR (Private DSA)** | **~430MB** | **0MB/spill** | **~100\nlines** |\n\n### Testing Plan\n\n- [ ] Build compiles successfully\n- [ ] Existing regression tests pass\n- [ ] 1M document build with memory instrumentation shows perfect\nreclamation\n- [ ] 50M document build completes with constant ~430MB peak\n- [ ] Concurrent inserts still work (runtime mode validation)\n\n### Relationship to PR #117\n\nPR #117 provides immediate mitigation and enables large-scale\nbenchmarks.\nThis PR provides the complete architectural fix for unlimited\nscalability.\n\nBoth can be merged independently - #117 helps immediately, this PR\neliminates the issue entirely.",
          "timestamp": "2026-01-09T03:58:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bfb0f5fcc97cefceb692de66f6bc6381ff07518"
        },
        "date": 1767939760949,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 275.596,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 1.12,
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
          "id": "95b2dba0df539c6cee323bc052e28add8b593206",
          "message": "Fix index size regression from parallel build over-allocation\n\nThe parallel build was pre-allocating pages using a 2.0x expansion factor\nfrom heap size, resulting in massive over-allocation (8.5GB for an index\nthat only needs 2GB of actual data).\n\nTwo fixes:\n1. Reduce expansion factor from 2.0 to 0.6 - text search indexes are\n   typically smaller than the heap, not larger\n2. Add tp_reclaim_unused_pool_pages() to return unused pre-allocated\n   pages to FSM before compaction, so they can be reused\n\nThis should reduce index size from ~8.5GB back to ~2.2GB for MS MARCO.",
          "timestamp": "2026-01-09T16:54:15Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/95b2dba0df539c6cee323bc052e28add8b593206"
        },
        "date": 1767979173475,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 254.934,
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
          "id": "151fefa781727d8f7664936d9be4d5f693c10a20",
          "message": "Add unused pool pages to FSM before compaction\n\nMove the FSM reclamation into tp_link_all_worker_segments, right before\ncompaction is called. This ensures unused pre-allocated pages are\navailable for compaction to reuse, reducing index bloat.\n\nOnly call RecordFreeIndexPage here - compaction will call\nIndexFreeSpaceMapVacuum after freeing its own segment pages, which\nupdates the FSM upper levels for all free pages including ours.",
          "timestamp": "2026-01-09T17:27:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/151fefa781727d8f7664936d9be4d5f693c10a20"
        },
        "date": 1767981136136,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 272.191,
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
          "id": "df06537a723c9ad7226870966b05489341636a28",
          "message": "Revert FSM reclamation, increase expansion factor\n\nRecordFreeIndexPage during parallel build causes buffer ownership errors\n(\"buffer io not owned by resource owner\"). This happens because FSM\noperations conflict with the parallel context's buffer management.\n\nFor now:\n- Increase expansion factor from 0.6 to 1.2 to provide enough pages\n  for parallel builds without needing mid-build FSM reclamation\n- Remove the RecordFreeIndexPage calls that caused crashes\n\nThe index will be somewhat over-allocated, but this can be addressed\nin a follow-up with proper VACUUM-based reclamation after the parallel\ncontext is fully destroyed.",
          "timestamp": "2026-01-09T17:55:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/df06537a723c9ad7226870966b05489341636a28"
        },
        "date": 1767983690997,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 234.315,
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
          "id": "fc562102ff994e2e02b9eab162f95a731abfa38a",
          "message": "Fix parallel build query performance regression\n\nChanges:\n- Force compaction when 2+ segments exist after parallel build, ensuring\n  the same single-segment structure as serial builds\n- Switch from per-worker page pools to a single shared pool for better\n  space efficiency\n- Add debug logging for parallel build decision tracing\n- Track max block used for potential future truncation optimization\n\nThe forced compaction eliminates the query overhead from multiple\nsegment lookups that was causing the 70% regression on MS MARCO.",
          "timestamp": "2026-01-08T22:50:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc562102ff994e2e02b9eab162f95a731abfa38a"
        },
        "date": 1767913638491,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 329003.135,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 23.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 25.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 28.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 32.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 40.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 43.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 54.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 75.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 40.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 8533.16,
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
          "id": "f302e837a9f5f51706d46a89d729c4a41e6c9a79",
          "message": "Add parallel build scaling benchmark\n\nNew benchmark that tests index build performance with varying numbers\nof parallel workers (0, 1, 2, 4). Uses MS MARCO dataset and measures\nboth build time and query latency for each configuration.\n\nFiles:\n- benchmarks/datasets/msmarco/parallel_scaling.sql: SQL benchmark script\n- benchmarks/datasets/msmarco/load_data_only.sql: Data loading without index\n- benchmarks/run_parallel_scaling.sh: Shell runner for local testing\n- .github/workflows/benchmark.yml: Added parallel_scaling input option\n\nTo run in CI: trigger benchmark workflow with parallel_scaling=true",
          "timestamp": "2026-01-09T02:28:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f302e837a9f5f51706d46a89d729c4a41e6c9a79"
        },
        "date": 1767927934878,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 320565.509,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 20.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 22.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 25.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 28.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 36.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 39.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 49.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 71.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 37.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 8533.17,
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
          "id": "29f2ac79eeeed7c627d7f8d77eb94899bb19e381",
          "message": "Use threshold-based compaction for parallel builds\n\nParallel builds were forcing compaction with 2+ segments, bypassing the\nsegments_per_level threshold. This caused parallel builds to produce\ndifferent index structures than serial builds with the same settings.\n\nBenchmark showed serial builds with 8 unmerged segments at L0 had better\nquery performance (17.18ms) than parallel builds forced to 1 merged\nsegment at L1 (20.03ms) due to Block-Max WAND skip index efficiency.\n\nChange parallel builds to use tp_maybe_compact_level() which respects\nthe segments_per_level threshold, ensuring consistent index structure\nregardless of build method.",
          "timestamp": "2026-01-09T03:14:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/29f2ac79eeeed7c627d7f8d77eb94899bb19e381"
        },
        "date": 1767930951480,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 331239.767,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 34.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 36.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 39.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 42.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 50.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 55.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 66.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 89.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 52.53,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 8533.17,
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
          "id": "7bfb0f5fcc97cefceb692de66f6bc6381ff07518",
          "message": "Use private DSA for index builds to eliminate memory leaks (#118)\n\n## Summary\n\nEliminates memory leaks during CREATE INDEX by using a private DSA that\nis destroyed and recreated on each spill, providing perfect memory\nreclamation.\n\n### Problem\n\nAs identified in PR #117, index builds leak ~110-400MB per spill cycle\ndue to DSA fragmentation. Even with the threshold reduction in #117, a\n50M document build still leaks ~17GB cumulative memory.\n\n### Solution: Private DSA with Destroy/Recreate\n\n**Key Insight:** During CREATE INDEX, only one backend is building. We\ndon't need a shared DSA - we can use a private one and destroy it\ncompletely between spills.\n\n**Implementation:**\n```c\n// Create private DSA for build (not in global registry)\nprivate_dsa = dsa_create(tranche_id);  \n\n// After spill:\ndsa_detach(private_dsa);  // Destroys DSA + ALL memory → OS\nprivate_dsa = dsa_create(tranche_id);  // Fresh DSA for next batch\n```\n\n**Architecture:**\n- **BUILD MODE**: Private DSA, destroyed/recreated per spill → 0% memory\nleak\n- **RUNTIME MODE**: Shared DSA for concurrent inserts (unchanged)\n- **Same data structures**: dshash, posting lists work identically in\nboth modes\n- **Minimal changes**: ~100 lines of code\n\n### Changes\n\n**New functions:**\n- `tp_create_build_index_state()`: Creates private DSA instead of using\nglobal\n- `tp_recreate_build_dsa()`: Destroys old DSA and creates fresh one\n- Updated `tp_clear_memtable()`: Calls recreation in build mode\n\n**Modified files:**\n- `src/state/state.h`: Added `is_build_mode` flag\n- `src/state/state.c`: Implemented private DSA lifecycle  \n- `src/am/build.c`: Use build mode during CREATE INDEX\n\n### Expected Results\n\n**Memory profile with private DSA:**\n```\nBUILD_START: 23 MB\n1M docs: 428 MB (8M postings)\nBEFORE_SPILL: 428 MB\nAFTER_SPILL: 25 MB  ← Perfect reclamation!\n2M docs: 428 MB (no growth!)\nBEFORE_SPILL: 428 MB\nAFTER_SPILL: 25 MB  ← Still 25 MB!\n```\n\n**For any dataset size:** Peak stays at ~430MB\n\n### Comparison\n\n| Approach | 50M Docs Peak | Memory Leak | Code Changes |\n|----------|---------------|-------------|--------------|\n| Original (32M threshold) | 26GB (OOM) | 400MB/spill | 0 |\n| PR #117 (8M threshold) | ~18GB | 110MB/spill | ~150 lines |\n| **This PR (Private DSA)** | **~430MB** | **0MB/spill** | **~100\nlines** |\n\n### Testing Plan\n\n- [ ] Build compiles successfully\n- [ ] Existing regression tests pass\n- [ ] 1M document build with memory instrumentation shows perfect\nreclamation\n- [ ] 50M document build completes with constant ~430MB peak\n- [ ] Concurrent inserts still work (runtime mode validation)\n\n### Relationship to PR #117\n\nPR #117 provides immediate mitigation and enables large-scale\nbenchmarks.\nThis PR provides the complete architectural fix for unlimited\nscalability.\n\nBoth can be merged independently - #117 helps immediately, this PR\neliminates the issue entirely.",
          "timestamp": "2026-01-09T03:58:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bfb0f5fcc97cefceb692de66f6bc6381ff07518"
        },
        "date": 1767939763360,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 535191.188,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 9.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 11.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 15.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 20.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 25.54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 32.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 43.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 63.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 30.98,
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
          "id": "243f47ae58eb723b4853421057e05af6f71250e5",
          "message": "Add optional segment block compression\n\nImplement delta encoding + bitpacking for posting list compression.\nWhen enabled via `pg_textsearch.compress_segments = on`, new segments\nuse compressed blocks that are ~60-70% smaller than uncompressed.\n\nKey changes:\n- New GUC `pg_textsearch.compress_segments` (default: off)\n- Add compression.c/h with bitpacking encode/decode functions\n- Modify segment write path to optionally compress blocks\n- Modify segment scan path to decompress based on skip entry flags\n- Use TP_BLOCK_FLAG_DELTA flag to indicate compressed blocks\n\nCompression is opt-in and off by default because without SIMD\noptimization, scalar decode (~4-6 GB/s) may not outpace NVMe storage.\nMixed compressed/uncompressed segments are fully supported.\n\nDesign document at docs/compression-design.md covers algorithm choices,\ntradeoff analysis, and future SIMD optimization plans.",
          "timestamp": "2026-01-09T03:37:45Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/243f47ae58eb723b4853421057e05af6f71250e5"
        },
        "date": 1767982433362,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (99.9K docs) - Index Build Time",
            "value": 5655.451,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 1 Token Query (p50)",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 3 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 4 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 5 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 6 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 7 Token Query (p50)",
            "value": 0.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (99.9K docs) - Index Size",
            "value": 15.52,
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
          "id": "df06537a723c9ad7226870966b05489341636a28",
          "message": "Revert FSM reclamation, increase expansion factor\n\nRecordFreeIndexPage during parallel build causes buffer ownership errors\n(\"buffer io not owned by resource owner\"). This happens because FSM\noperations conflict with the parallel context's buffer management.\n\nFor now:\n- Increase expansion factor from 0.6 to 1.2 to provide enough pages\n  for parallel builds without needing mid-build FSM reclamation\n- Remove the RecordFreeIndexPage calls that caused crashes\n\nThe index will be somewhat over-allocated, but this can be addressed\nin a follow-up with proper VACUUM-based reclamation after the parallel\ncontext is fully destroyed.",
          "timestamp": "2026-01-09T17:55:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/df06537a723c9ad7226870966b05489341636a28"
        },
        "date": 1767983692541,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 302603.516,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 20.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 22.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 25.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 28.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 35.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 39.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 49.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 71.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 37.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 5939.45,
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
          "id": "76132145fa55f1431bbb84f05c8ee065cdd59fd2",
          "message": "Add tests for mixed compression scenarios\n\nTest that queries work correctly when an index has segments with\ndifferent compression states (compressed vs uncompressed) due to\ntoggling the compress_segments GUC between spills.",
          "timestamp": "2026-01-09T18:35:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/76132145fa55f1431bbb84f05c8ee065cdd59fd2"
        },
        "date": 1767985339797,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 560928.389,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 14.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 17.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 22.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 25.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 34.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 41.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 52.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 78.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 37.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1396.44,
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
          "id": "2c6e602d415b1eeaf2af0a39704a49faf11f0eab",
          "message": "Add tests for mixed compression scenarios\n\nTest that queries work correctly when an index has segments with\ndifferent compression states (compressed vs uncompressed) due to\ntoggling the compress_segments GUC between spills.",
          "timestamp": "2026-01-09T18:35:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2c6e602d415b1eeaf2af0a39704a49faf11f0eab"
        },
        "date": 1767999910645,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 529898.329,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 16.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 22.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 27.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 34.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 45.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 69.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 33.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "29f2ac79eeeed7c627d7f8d77eb94899bb19e381",
          "message": "Use threshold-based compaction for parallel builds\n\nParallel builds were forcing compaction with 2+ segments, bypassing the\nsegments_per_level threshold. This caused parallel builds to produce\ndifferent index structures than serial builds with the same settings.\n\nBenchmark showed serial builds with 8 unmerged segments at L0 had better\nquery performance (17.18ms) than parallel builds forced to 1 merged\nsegment at L1 (20.03ms) due to Block-Max WAND skip index efficiency.\n\nChange parallel builds to use tp_maybe_compact_level() which respects\nthe segments_per_level threshold, ensuring consistent index structure\nregardless of build method.",
          "timestamp": "2026-01-09T03:14:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/29f2ac79eeeed7c627d7f8d77eb94899bb19e381"
        },
        "date": 1767930954088,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7980.837,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 131.05,
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
          "id": "7bfb0f5fcc97cefceb692de66f6bc6381ff07518",
          "message": "Use private DSA for index builds to eliminate memory leaks (#118)\n\n## Summary\n\nEliminates memory leaks during CREATE INDEX by using a private DSA that\nis destroyed and recreated on each spill, providing perfect memory\nreclamation.\n\n### Problem\n\nAs identified in PR #117, index builds leak ~110-400MB per spill cycle\ndue to DSA fragmentation. Even with the threshold reduction in #117, a\n50M document build still leaks ~17GB cumulative memory.\n\n### Solution: Private DSA with Destroy/Recreate\n\n**Key Insight:** During CREATE INDEX, only one backend is building. We\ndon't need a shared DSA - we can use a private one and destroy it\ncompletely between spills.\n\n**Implementation:**\n```c\n// Create private DSA for build (not in global registry)\nprivate_dsa = dsa_create(tranche_id);  \n\n// After spill:\ndsa_detach(private_dsa);  // Destroys DSA + ALL memory → OS\nprivate_dsa = dsa_create(tranche_id);  // Fresh DSA for next batch\n```\n\n**Architecture:**\n- **BUILD MODE**: Private DSA, destroyed/recreated per spill → 0% memory\nleak\n- **RUNTIME MODE**: Shared DSA for concurrent inserts (unchanged)\n- **Same data structures**: dshash, posting lists work identically in\nboth modes\n- **Minimal changes**: ~100 lines of code\n\n### Changes\n\n**New functions:**\n- `tp_create_build_index_state()`: Creates private DSA instead of using\nglobal\n- `tp_recreate_build_dsa()`: Destroys old DSA and creates fresh one\n- Updated `tp_clear_memtable()`: Calls recreation in build mode\n\n**Modified files:**\n- `src/state/state.h`: Added `is_build_mode` flag\n- `src/state/state.c`: Implemented private DSA lifecycle  \n- `src/am/build.c`: Use build mode during CREATE INDEX\n\n### Expected Results\n\n**Memory profile with private DSA:**\n```\nBUILD_START: 23 MB\n1M docs: 428 MB (8M postings)\nBEFORE_SPILL: 428 MB\nAFTER_SPILL: 25 MB  ← Perfect reclamation!\n2M docs: 428 MB (no growth!)\nBEFORE_SPILL: 428 MB\nAFTER_SPILL: 25 MB  ← Still 25 MB!\n```\n\n**For any dataset size:** Peak stays at ~430MB\n\n### Comparison\n\n| Approach | 50M Docs Peak | Memory Leak | Code Changes |\n|----------|---------------|-------------|--------------|\n| Original (32M threshold) | 26GB (OOM) | 400MB/spill | 0 |\n| PR #117 (8M threshold) | ~18GB | 110MB/spill | ~150 lines |\n| **This PR (Private DSA)** | **~430MB** | **0MB/spill** | **~100\nlines** |\n\n### Testing Plan\n\n- [ ] Build compiles successfully\n- [ ] Existing regression tests pass\n- [ ] 1M document build with memory instrumentation shows perfect\nreclamation\n- [ ] 50M document build completes with constant ~430MB peak\n- [ ] Concurrent inserts still work (runtime mode validation)\n\n### Relationship to PR #117\n\nPR #117 provides immediate mitigation and enables large-scale\nbenchmarks.\nThis PR provides the complete architectural fix for unlimited\nscalability.\n\nBoth can be merged independently - #117 helps immediately, this PR\neliminates the issue entirely.",
          "timestamp": "2026-01-09T03:58:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bfb0f5fcc97cefceb692de66f6bc6381ff07518"
        },
        "date": 1767939765257,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18942.407,
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
          "id": "95b2dba0df539c6cee323bc052e28add8b593206",
          "message": "Fix index size regression from parallel build over-allocation\n\nThe parallel build was pre-allocating pages using a 2.0x expansion factor\nfrom heap size, resulting in massive over-allocation (8.5GB for an index\nthat only needs 2GB of actual data).\n\nTwo fixes:\n1. Reduce expansion factor from 2.0 to 0.6 - text search indexes are\n   typically smaller than the heap, not larger\n2. Add tp_reclaim_unused_pool_pages() to return unused pre-allocated\n   pages to FSM before compaction, so they can be reused\n\nThis should reduce index size from ~8.5GB back to ~2.2GB for MS MARCO.",
          "timestamp": "2026-01-09T16:54:15Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/95b2dba0df539c6cee323bc052e28add8b593206"
        },
        "date": 1767979175238,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7775.749,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 67.75,
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
          "id": "df06537a723c9ad7226870966b05489341636a28",
          "message": "Revert FSM reclamation, increase expansion factor\n\nRecordFreeIndexPage during parallel build causes buffer ownership errors\n(\"buffer io not owned by resource owner\"). This happens because FSM\noperations conflict with the parallel context's buffer management.\n\nFor now:\n- Increase expansion factor from 0.6 to 1.2 to provide enough pages\n  for parallel builds without needing mid-build FSM reclamation\n- Remove the RecordFreeIndexPage calls that caused crashes\n\nThe index will be somewhat over-allocated, but this can be addressed\nin a follow-up with proper VACUUM-based reclamation after the parallel\ncontext is fully destroyed.",
          "timestamp": "2026-01-09T17:55:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/df06537a723c9ad7226870966b05489341636a28"
        },
        "date": 1767983693771,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7764.813,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 79.25,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}