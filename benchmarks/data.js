window.BENCHMARK_DATA = {
  "lastUpdate": 1770532137710,
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
          "id": "7c35333d63b364452d712aa405e7463691c40d0b",
          "message": "Update banner image for v0.4.0 (#131)\n\n## Summary\n- Add new banner image for v0.4.0\n- Update README to reference the new image",
          "timestamp": "2026-01-10T03:07:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7c35333d63b364452d712aa405e7463691c40d0b"
        },
        "date": 1768026150432,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 251.447,
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
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "7c35333d63b364452d712aa405e7463691c40d0b",
          "message": "Update banner image for v0.4.0 (#131)\n\n## Summary\n- Add new banner image for v0.4.0\n- Update README to reference the new image",
          "timestamp": "2026-01-10T03:07:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7c35333d63b364452d712aa405e7463691c40d0b"
        },
        "date": 1768112565824,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 255.018,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.35,
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
          "id": "bd8e028a841e2ed5919007f573d1ccd96e19bd0e",
          "message": "Fix smgrtruncate call for PG17 compatibility",
          "timestamp": "2026-01-12T04:00:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bd8e028a841e2ed5919007f573d1ccd96e19bd0e"
        },
        "date": 1768195467732,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 255.993,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "564b18a6e2fa542e2e37abbf35be189ecbecd6b3",
          "message": "Implement AMPROP_DISTANCE_ORDERABLE in amproperty (#133)\n\n## Summary\n\nThe `amproperty` callback lets access methods report capabilities that\nPostgres can't infer automatically. For properties like\n`distance_orderable`, Postgres returns NULL if the access method doesn't\nimplement the callback.\n\nSince BM25 indexes support ORDER BY via the `<@>` operator, we should\nreport `distance_orderable = true`. This improves reporting of index\ncapabilities through tools like pgAdmin.\n\nFixes #103\n\n## Testing\nAll existing regression tests pass.",
          "timestamp": "2026-01-12T03:23:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/564b18a6e2fa542e2e37abbf35be189ecbecd6b3"
        },
        "date": 1768199095075,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 285.158,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "28b0b96866b20e1f3851df2737735347cecc5ce6",
          "message": "Fix parallel build pool exhaustion and smgr cache issues\n\n- Increase expansion factor from 0.5 to 0.6 for safety margin\n- Improve page index estimation: account for ~10 segments per worker\n- Add smgrnblocks() call in worker init to refresh stale smgr cache\n  This fixes \"unexpected data beyond EOF\" errors in parallel workers",
          "timestamp": "2026-01-12T20:30:11Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/28b0b96866b20e1f3851df2737735347cecc5ce6"
        },
        "date": 1768250490384,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 258.388,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "4140c83e6bf828cb20e094afdb93f9f8743f4a5c",
          "message": "Increase expansion factor to 0.8 for large datasets",
          "timestamp": "2026-01-12T21:11:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4140c83e6bf828cb20e094afdb93f9f8743f4a5c"
        },
        "date": 1768253312885,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 270.297,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "4140c83e6bf828cb20e094afdb93f9f8743f4a5c",
          "message": "Increase expansion factor to 0.8 for large datasets",
          "timestamp": "2026-01-12T21:11:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4140c83e6bf828cb20e094afdb93f9f8743f4a5c"
        },
        "date": 1768253430596,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.769,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "411527a28812edbf9a2ccb175811f449ce4278b0",
          "message": "Release v0.4.0 (#136)\n\n## Summary\n\n- Update version from 0.4.0-dev to 0.4.0\n- Rename SQL files to drop -dev suffix\n- Add CHANGELOG entries for v0.3.0 and v0.4.0\n\n## v0.4.0 Highlights\n\n- **Posting list compression**: Delta encoding + bitpacking for ~41%\nsmaller indexes\n- Coverity static analysis integration\n- AMPROP_DISTANCE_ORDERABLE implementation\n- Fixed 'too many LWLocks taken' error on partitioned tables\n\n## Testing\n\n- `make test` passes (37 tests)\n- `make format-check` passes",
          "timestamp": "2026-01-13T02:33:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/411527a28812edbf9a2ccb175811f449ce4278b0"
        },
        "date": 1768285311635,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 251.085,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "0b1fde1605f16cc34b195c41209f76e13c1ac303",
          "message": "Use binary search for WAND-style doc ID seeking in BMW\n\nThe multi-term BMW algorithm now uses binary search (via\ntp_segment_posting_iterator_seek) when skipping documents that can't\nbeat the threshold. Previously, iterators advanced one document at a\ntime.\n\nKey changes:\n- Add seek_term_to_doc() that uses binary search on skip entries when\n  the target is beyond the current block\n- Add find_next_candidate_doc_id() to find the minimum doc ID among\n  terms not at the current pivot\n- Modify skip_pivot_document() to seek to the next candidate instead\n  of advancing by 1\n\nThis changes the complexity from O(blocks) to O(log blocks) when\nskipping over documents with sparse term overlap.\n\nCloses #141",
          "timestamp": "2026-01-13T16:57:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0b1fde1605f16cc34b195c41209f76e13c1ac303"
        },
        "date": 1768324908731,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 270.027,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "fbff39af22ea556ca9cab93b45e437d49a35f9fd",
          "message": "Add GUC and stats for WAND seek optimization\n\nAdd pg_textsearch.enable_wand_seek GUC to control whether WAND-style\nbinary search seeking is used (default: on). This allows A/B\ncomparison to measure the optimization's impact.\n\nAdd seek statistics to TpBMWStats:\n- seeks_performed: number of binary search seeks\n- docs_seeked: total doc IDs skipped via seeking\n- linear_advances: single-doc advances (no seek benefit)\n- seek_to_same_doc: seeks that landed on same/next doc\n\nThese stats are logged when pg_textsearch.log_bmw_stats=on.\n\nLocal testing shows the current implementation adds overhead without\nbenefit for typical workloads. The find_next_candidate_doc_id call\non every skip appears to cost more than the seeks save.",
          "timestamp": "2026-01-13T17:35:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fbff39af22ea556ca9cab93b45e437d49a35f9fd"
        },
        "date": 1768326477745,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 289.344,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "7e7e75e7ae39399c61ec85606231226baab39232",
          "message": "Optimize WAND seek by maintaining sorted term array\n\nInspired by Tantivy's WAND implementation, maintain term iterators\nsorted by current doc ID. This makes find_pivot_doc_id O(1) and\nfind_next_candidate_doc_id O(1) typical case instead of O(N).\n\nKey changes:\n- Add term_current_doc_id() helper for consistent doc ID access\n- Add compare_terms_by_doc_id() and sort_terms_by_doc_id() for sorting\n- Sort terms after initialization in init_segment_term_states()\n- Re-sort after advancing terms in score_pivot_document() and\n  skip_pivot_document()\n- Simplify find_pivot_doc_id() to just return first term's doc ID\n- Optimize find_next_candidate_doc_id() to scan from start until\n  first term past pivot (O(1) when only one term at pivot)\n- Add early exit in compute_pivot_max_score() using sorted order\n\nLocal benchmarks show performance parity between seek ON/OFF modes,\nresolving the overhead issue from the previous O(N) iteration.",
          "timestamp": "2026-01-13T18:48:40Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7e7e75e7ae39399c61ec85606231226baab39232"
        },
        "date": 1768333198367,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 265.099,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 1.6,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "12e50c25163c3b715f1f558e6c3111ece9c5cd5a",
          "message": "Use insertion sort instead of qsort for term array\n\nProfiling showed qsort was taking 70% of query time due to its\noverhead on small arrays. Insertion sort is O(N) for nearly-sorted\narrays, which is our case since only 1-2 terms move after advancing.\n\nFor N=2-5 terms (typical), this is essentially free compared to qsort.",
          "timestamp": "2026-01-13T21:55:36Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/12e50c25163c3b715f1f558e6c3111ece9c5cd5a"
        },
        "date": 1768342454187,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 232.233,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768355518760,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 228.235,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768356978810,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 232.731,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "0db7c2097de05330cfbcd89a747e9b04fb9f9d81",
          "message": "Add pgspot CI workflow for SQL security checks (#145)\n\n## Summary\n- Add GitHub workflow to run pgspot on extension SQL files for security\nchecks\n- Fix PS002 warning by changing `CREATE OR REPLACE` to `CREATE` for\n`bm25_spill_index` function\n- Ignore PS017 warnings (false positives for type/operator definitions\nwithin the extension)\n\n## Testing\n- All 37 regression tests pass\n- pgspot runs clean on all SQL files with `--ignore PS017`",
          "timestamp": "2026-01-14T04:37:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0db7c2097de05330cfbcd89a747e9b04fb9f9d81"
        },
        "date": 1768371737057,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 257.485,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "c5a6ea09ab094dd95174f6f7e75d338a680a295d",
          "message": "Lazy CTID loading for segment queries\n\nDefer CTID resolution until final top-k results are known, reducing\nmemory usage from ~53MB to ~60 bytes per query (for k=10).\n\nChanges:\n- Add tp_segment_open_ex() with load_ctids parameter (default: false)\n- Add tp_segment_lookup_ctid() for targeted 6-byte reads\n- Split heap add into tp_topk_add_memtable/tp_topk_add_segment\n- Add tp_topk_resolve_ctids() to resolve CTIDs before extraction\n- Add doc_id field to TpSegmentPosting for deferred resolution\n- Update iterator to conditionally resolve CTIDs\n- Exhaustive path still loads CTIDs (needed for hash table keys)",
          "timestamp": "2026-01-14T06:48:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c5a6ea09ab094dd95174f6f7e75d338a680a295d"
        },
        "date": 1768374447431,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.269,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.88,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "94c66cc59f38a7e4e2d72832432f7825fb29fd3a",
          "message": "Fix formatting",
          "timestamp": "2026-01-14T18:43:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/94c66cc59f38a7e4e2d72832432f7825fb29fd3a"
        },
        "date": 1768416443738,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 247.617,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "d32590214fa6b4fbf6a5f8643b6461926873fdf6",
          "message": "Bump version to v0.5.0-dev (#147)\n\n## Summary\n- Bump version from v0.4.0 to v0.5.0-dev\n- Update README to note parallel indexing as the main feature for v0.5.0\n- Add new SQL extension file and update test expected outputs\n\n## Testing\n- CI will verify the version string updates are consistent",
          "timestamp": "2026-01-15T04:27:11Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d32590214fa6b4fbf6a5f8643b6461926873fdf6"
        },
        "date": 1768458368660,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.018,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "75a05c5003b013a45ca4886ea055a175b0c1c351",
          "message": "Add LSAN suppressions for pg_config false positives\n\nThe nightly stress tests were failing because LSAN detected \"leaks\" in\nPostgreSQL's pg_config utility. These are false positives - pg_config\nis a short-lived command-line tool that allocates memory and exits\nwithout freeing it, which is normal and expected behavior.\n\nAdd suppressions for pg_strdup and get_configdata to fix the failures.",
          "timestamp": "2026-01-16T05:56:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/75a05c5003b013a45ca4886ea055a175b0c1c351"
        },
        "date": 1768544511463,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 257.766,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "72239eee572aba5253e13aeb0bb5a086f81ace7e",
          "message": "Document Python 3.10 pin for wikiextractor (#151)\n\n## Summary\n\n- Add comment explaining why Python 3.10 is pinned in the\ncomparative-benchmark job\n- wikiextractor is unmaintained and breaks on Python 3.11+",
          "timestamp": "2026-01-17T02:20:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/72239eee572aba5253e13aeb0bb5a086f81ace7e"
        },
        "date": 1768630838023,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 247.285,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "72239eee572aba5253e13aeb0bb5a086f81ace7e",
          "message": "Document Python 3.10 pin for wikiextractor (#151)\n\n## Summary\n\n- Add comment explaining why Python 3.10 is pinned in the\ncomparative-benchmark job\n- wikiextractor is unmaintained and breaks on Python 3.11+",
          "timestamp": "2026-01-17T02:20:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/72239eee572aba5253e13aeb0bb5a086f81ace7e"
        },
        "date": 1768717308785,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 257.835,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "efe88321a9fabe17cd60a5adf155d804b1796161",
          "message": "Fix potential integer overflow in BMW stats tracking (#155)\n\n## Summary\n- Fix Coverity-reported integer overflow (CID 641544) in\n`skip_pivot_document()`\n- Rewrite `landed_doc <= pivot_doc_id + 1` as `landed_doc - pivot_doc_id\n<= 1` to avoid overflow when `pivot_doc_id` equals `UINT32_MAX`\n\n## Testing\n- All SQL regression tests pass",
          "timestamp": "2026-01-19T01:31:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/efe88321a9fabe17cd60a5adf155d804b1796161"
        },
        "date": 1768803796578,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.022,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "efe88321a9fabe17cd60a5adf155d804b1796161",
          "message": "Fix potential integer overflow in BMW stats tracking (#155)\n\n## Summary\n- Fix Coverity-reported integer overflow (CID 641544) in\n`skip_pivot_document()`\n- Rewrite `landed_doc <= pivot_doc_id + 1` as `landed_doc - pivot_doc_id\n<= 1` to avoid overflow when `pivot_doc_id` equals `UINT32_MAX`\n\n## Testing\n- All SQL regression tests pass",
          "timestamp": "2026-01-19T01:31:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/efe88321a9fabe17cd60a5adf155d804b1796161"
        },
        "date": 1768890195403,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 269.435,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "c088e94febb15a9b4901fc522dac675f8f9f89fb",
          "message": "Fix nightly stress test failures (#157)\n\n## Summary\n- Fix PostgreSQL cache to include install directory (was only caching\nbuild dir)\n- Add LSAN suppression for `save_ps_display_args` false positive\n- Add `issues: write` permission for failure notification job\n\nThe nightly stress tests have been failing since Jan 9 due to the cache\nissue causing `initdb` to fail with \"postgres binary not found\".\n\n## Testing\n- Trigger manual run: `gh workflow run nightly-stress.yml --field\nduration_minutes=5`",
          "timestamp": "2026-01-20T16:01:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c088e94febb15a9b4901fc522dac675f8f9f89fb"
        },
        "date": 1768976509246,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 255.684,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "bde408ca9e72fca686e47ef360cee27c217d2ab8",
          "message": "Add parallel bulk index build support, part 1/2 (#114)\n\n## Summary\n\n- Implement parallel CREATE INDEX using Postgres parallel infrastructure\n- Each worker scans heap concurrently, builds local memtables, writes L0\nsegments\n- Leader participates as worker 0; links all worker segment chains at\nend\n- Compaction performed on leader node; part 2/2 will parallelize\ncompaction\n\n## Testing\n\n- All 33 regression tests pass\n- Falls back to serial when `max_parallel_maintenance_workers=0`",
          "timestamp": "2026-01-21T19:00:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bde408ca9e72fca686e47ef360cee27c217d2ab8"
        },
        "date": 1769062817376,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 266.615,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "2ae9d52c42fe284f6bcae1850c64e6a769c0ef03",
          "message": "fix: use FSM instead of truncation for parallel build page reclamation\n\nAfter parallel index build completes, unused pre-allocated pool pages\nwere being truncated. However, the subsequent compaction step would then\nextend the relation again, negating the truncation.\n\nInstead of truncating, record unused pool pages in the Free Space Map\n(FSM) so they can be reused by compaction. This prevents the index size\nregression observed in nightly benchmarks since the parallel build PR\nmerged.\n\nKey changes:\n- Rename truncate_unused_pool_pages to reclaim_unused_pool_pages\n- Use RecordFreeIndexPage to add unused pages to FSM\n- Call IndexFreeSpaceMapVacuum to make pages discoverable",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2ae9d52c42fe284f6bcae1850c64e6a769c0ef03"
        },
        "date": 1769108599067,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 248.565,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "95f28c8cd59584b198dd6d0bc50e893dda57b1d6",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nTwo fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build, instead\n   of failing with an error, we now dynamically extend the relation.\n   This ensures builds always succeed even if the initial size estimate\n   was too conservative. A NOTICE is logged on first overflow so users\n   can tune the expansion factor if desired.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/95f28c8cd59584b198dd6d0bc50e893dda57b1d6"
        },
        "date": 1769110704214,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 260.704,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "95f28c8cd59584b198dd6d0bc50e893dda57b1d6",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nTwo fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build, instead\n   of failing with an error, we now dynamically extend the relation.\n   This ensures builds always succeed even if the initial size estimate\n   was too conservative. A NOTICE is logged on first overflow so users\n   can tune the expansion factor if desired.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/95f28c8cd59584b198dd6d0bc50e893dda57b1d6"
        },
        "date": 1769111683256,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 251.935,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "d66f0d8bdeb63aca2c43247243494af89820c199",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nThree fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build (in\n   tp_pool_get_page), instead of failing with an error, we now\n   dynamically extend the relation. A NOTICE is logged on first overflow\n   so users can tune the expansion factor if desired.\n\n3. When writing page index and the pool is exhausted (in\n   write_page_index_from_pool), we now also dynamically extend instead\n   of failing.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d66f0d8bdeb63aca2c43247243494af89820c199"
        },
        "date": 1769113050537,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 282.59,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "24735ab9664f56e47eddfad6e0164c941563108d",
          "message": "Fix 'too many LWLocks taken' error when scanning hypertables (#164)\n\n## Summary\n- Fixes \"too many LWLocks taken\" error when querying BM25 indexes on\nhypertables with many chunks (e.g., 587 chunks)\n- Releases the per-index LWLock after `tp_memtable_search()` completes\n\nThis is the scan-path equivalent of the build-path fix in b7f6539.\n\n## Problem\nWhen scanning a hypertable with many chunks, each chunk's BM25 index\nscan acquires a per-index LWLock but never releases it until transaction\nend. With 587 chunks, this exceeds Postgres's ~200 lock limit.\n\n## Solution\nRelease the lock immediately after `tp_memtable_search()` returns, since\nwe've already extracted all CTIDs into memory at that point.\n\n## Testing\n- Tested with 587-chunk hypertable (2.2M rows) - query now succeeds\n- All regression tests pass\n- All shell-based tests pass",
          "timestamp": "2026-01-22T23:28:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/24735ab9664f56e47eddfad6e0164c941563108d"
        },
        "date": 1769149180781,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 250.295,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "24735ab9664f56e47eddfad6e0164c941563108d",
          "message": "Fix 'too many LWLocks taken' error when scanning hypertables (#164)\n\n## Summary\n- Fixes \"too many LWLocks taken\" error when querying BM25 indexes on\nhypertables with many chunks (e.g., 587 chunks)\n- Releases the per-index LWLock after `tp_memtable_search()` completes\n\nThis is the scan-path equivalent of the build-path fix in b7f6539.\n\n## Problem\nWhen scanning a hypertable with many chunks, each chunk's BM25 index\nscan acquires a per-index LWLock but never releases it until transaction\nend. With 587 chunks, this exceeds Postgres's ~200 lock limit.\n\n## Solution\nRelease the lock immediately after `tp_memtable_search()` returns, since\nwe've already extracted all CTIDs into memory at that point.\n\n## Testing\n- Tested with 587-chunk hypertable (2.2M rows) - query now succeeds\n- All regression tests pass\n- All shell-based tests pass",
          "timestamp": "2026-01-22T23:28:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/24735ab9664f56e47eddfad6e0164c941563108d"
        },
        "date": 1769235492120,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 257.369,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "0bce9b22bedead552eaa38ea1cfc5f0bf8c2a64b",
          "message": "Fix Wikipedia benchmark metric extraction failure\n\nThe Wikipedia benchmark CI was failing because queries.sql didn't produce\nthe structured metric output that extract_metrics.sh expects:\n\n- LATENCY_BUCKET_N: p50=Xms p95=Yms p99=Zms avg=Wms (n=100)\n- THROUGHPUT_RESULT: N queries in X ms (avg Y ms/query)\n\nWithout these patterns, wikipedia_metrics.json had null values for all\nlatency/throughput metrics, causing format_for_action.sh to produce an\nempty array [], which made github-action-benchmark fail.\n\nThis change rewrites queries.sql to match the MS MARCO benchmark format:\n- Creates benchmark_queries table with 800 queries (100 per token bucket)\n- Runs latency benchmarks outputting LATENCY_BUCKET_N metrics\n- Runs throughput benchmarks outputting THROUGHPUT_RESULT metrics\n- Uses representative Wikipedia search queries across 8 token buckets\n\nThis gives Wikipedia the same standardized benchmark format as MS MARCO,\nenabling consistent cross-dataset performance comparison.",
          "timestamp": "2026-01-25T00:49:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0bce9b22bedead552eaa38ea1cfc5f0bf8c2a64b"
        },
        "date": 1769305169115,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 258.567,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "d69525e0963a9960de7d7338c89db7e2391d34db",
          "message": "Fix Coverity static analysis issues (#170)\n\n## Summary\n\n- Remove dead code in `write_term_postings()` where a condition can\nnever be true\n- Remove redundant null checks before `pfree()` in\n`tp_segment_posting_iterator_init()`\n- Initialize `doc_count` variable to satisfy Coverity's inter-procedural\nanalysis\n\n## Testing\n\n- All SQL regression tests pass\n- Concurrency, recovery, and segment shell tests pass",
          "timestamp": "2026-01-25T00:48:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d69525e0963a9960de7d7338c89db7e2391d34db"
        },
        "date": 1769321850591,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 255.757,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "3244d05a86658620ecb2d82253ef7916604f839d",
          "message": "Fix zero scores when querying hypertables with BM25 index (#168)\n\n## Summary\n\nThis PR fixes zero scores when querying hypertables with BM25 indexes.\n\n**Commit 1: Planner hook fix for CustomScan**\n- Add `T_CustomScan` handling in `plan_has_bm25_indexscan()` to detect\nBM25 index scans nested inside custom scans (e.g., TimescaleDB's\nConstraintAwareAppend)\n- Add `T_CustomScan` handling in `replace_scores_in_plan()` to replace\nscore expressions in custom scan children\n\n**Commit 2: Standalone scoring fix for hypertable parent indexes**\n- When using standalone BM25 scoring with a hypertable parent index\nname, the code was falling back to child index stats but NOT switching\nto the child's index relation and segment metadata\n- This caused IDF calculation to fail because it was looking up document\nfrequencies in the parent index's segments (which are empty)\n- The fix switches to the child index for segment access when falling\nback to a child index's state\n\n## Testing\n\n- Verified fix with reproduction case from bug report\n- Added Test 5 in partitioned.sql for MergeAppend score expression\nreplacement\n- Added test/scripts/hypertable.sh for optional TimescaleDB integration\ntesting (runs only if TimescaleDB is installed)\n\n```sql\n-- Both queries now return proper BM25 scores:\n\n-- Query through parent hypertable with ORDER BY\nSELECT content, -(content <@> to_bm25query('database', 'hyper_idx')) as score\nFROM hyper_docs\nORDER BY content <@> to_bm25query('database', 'hyper_idx')\nLIMIT 5;\n\n-- Standalone scoring with parent index name\nSELECT content, (content <@> to_bm25query('database', 'hyper_idx')) as score\nFROM hyper_docs\nWHERE content LIKE '%database%';\n```",
          "timestamp": "2026-01-25T16:35:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3244d05a86658620ecb2d82253ef7916604f839d"
        },
        "date": 1769408375895,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 251.915,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.51,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "70747956a7287acc6da5743f831c4cfe651e8d22",
          "message": "chore: clean up legacy am/extension name and remove unused test script (#177)\n\nIn src/am/build.c, there is still code that refers to the legacy access\nmethod and extension name, tapir. This commit updates those references\nto the current names.\n\nscripts/generate_test_case.py is no longer used (replaced by\nvalidation.sql) and thus gets removed.",
          "timestamp": "2026-01-27T02:08:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/70747956a7287acc6da5743f831c4cfe651e8d22"
        },
        "date": 1769494669804,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 232.08,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "70747956a7287acc6da5743f831c4cfe651e8d22",
          "message": "chore: clean up legacy am/extension name and remove unused test script (#177)\n\nIn src/am/build.c, there is still code that refers to the legacy access\nmethod and extension name, tapir. This commit updates those references\nto the current names.\n\nscripts/generate_test_case.py is no longer used (replaced by\nvalidation.sql) and thus gets removed.",
          "timestamp": "2026-01-27T02:08:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/70747956a7287acc6da5743f831c4cfe651e8d22"
        },
        "date": 1769581193604,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.884,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769668007401,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 255.325,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "276f64750340502e0fbce0f35f23c3cc4a8b93c8",
          "message": "fix: handle page boundaries when writing dictionary entries in parallel build\n\nThe dictionary entry writing code in tp_merge_worker_buffers_to_segment()\nassumed all entries fit on the first page. With large datasets (150K+ docs),\ndictionary entries span multiple pages, causing \"Invalid logical page\" errors.\n\nFixed by:\n1. Flushing segment writer before writing entries\n2. Using page-boundary-aware code that calculates logical/physical pages\n3. Handling entries that span two pages like merge.c does",
          "timestamp": "2026-01-29T15:34:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/276f64750340502e0fbce0f35f23c3cc4a8b93c8"
        },
        "date": 1769710986353,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 229.995,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "ef7051aeffdeb9076de500185859baeb4a1549ba",
          "message": "fix: revert to nworkers > 0 for parallel build, update expected output\n\n1 worker still helps with read/write parallelism (worker scans while\nleader writes). Reverted the nworkers > 1 check back to nworkers > 0.\n\nUpdated test expected output after confirming tests pass with both\ndebug and release builds.",
          "timestamp": "2026-01-29T23:52:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ef7051aeffdeb9076de500185859baeb4a1549ba"
        },
        "date": 1769731123569,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.566,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "a9cf8f4b5aa880608a7dfbc736429b2693a6028e",
          "message": "chore: add max_parallel_maintenance_workers to Cranfield benchmark\n\nAlso add comments explaining the maintenance_work_mem requirement\nfor parallel builds (32MB per worker + leader).\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-30T01:59:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a9cf8f4b5aa880608a7dfbc736429b2693a6028e"
        },
        "date": 1769739675718,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 252.506,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "a9cf8f4b5aa880608a7dfbc736429b2693a6028e",
          "message": "chore: add max_parallel_maintenance_workers to Cranfield benchmark\n\nAlso add comments explaining the maintenance_work_mem requirement\nfor parallel builds (32MB per worker + leader).\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-30T01:59:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a9cf8f4b5aa880608a7dfbc736429b2693a6028e"
        },
        "date": 1769741833864,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 261.247,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769754450069,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.953,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769840627450,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.576,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.64,
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
          "id": "60604649c2cef4b638c318a545bbbe6f72801d93",
          "message": "ci: use standard runners for benchmarks (#193)\n\n## Summary\n- Reverts benchmark workflow from `ubuntu-latest-16-cores` to standard\n`ubuntu-latest` runners\n- Adjusts PostgreSQL memory settings to fit within ~7GB RAM limit\n- Fixes jobs that were queueing indefinitely due to unavailable 16-core\nrunners\n\n## Changes\n| Setting | Before | After |\n|---------|--------|-------|\n| Runner | ubuntu-latest-16-cores | ubuntu-latest |\n| shared_buffers | 4GB | 1GB |\n| effective_cache_size | 12GB | 4GB |\n| maintenance_work_mem | 1GB | 512MB |\n| work_mem | 128MB | 64MB |\n| wal_buffers | 64MB | 16MB |\n\nThe 512MB maintenance_work_mem is still well above the 64MB minimum\nrequired for parallel index builds.\n\n## Testing\nManually triggered workflow succeeded and results look reasonable.",
          "timestamp": "2026-02-01T01:48:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/60604649c2cef4b638c318a545bbbe6f72801d93"
        },
        "date": 1769927255573,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 234.045,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "60604649c2cef4b638c318a545bbbe6f72801d93",
          "message": "ci: use standard runners for benchmarks (#193)\n\n## Summary\n- Reverts benchmark workflow from `ubuntu-latest-16-cores` to standard\n`ubuntu-latest` runners\n- Adjusts PostgreSQL memory settings to fit within ~7GB RAM limit\n- Fixes jobs that were queueing indefinitely due to unavailable 16-core\nrunners\n\n## Changes\n| Setting | Before | After |\n|---------|--------|-------|\n| Runner | ubuntu-latest-16-cores | ubuntu-latest |\n| shared_buffers | 4GB | 1GB |\n| effective_cache_size | 12GB | 4GB |\n| maintenance_work_mem | 1GB | 512MB |\n| work_mem | 128MB | 64MB |\n| wal_buffers | 64MB | 16MB |\n\nThe 512MB maintenance_work_mem is still well above the 64MB minimum\nrequired for parallel index builds.\n\n## Testing\nManually triggered workflow succeeded and results look reasonable.",
          "timestamp": "2026-02-01T01:48:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/60604649c2cef4b638c318a545bbbe6f72801d93"
        },
        "date": 1770014183842,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 277.214,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "1a23bc6bbb06dcec0c7e6efa50b2a51c08882926",
          "message": "Release v0.5.0 (#197)\n\n## Summary\n- Update version from 0.5.0-dev to 0.5.0 across all files\n- Rename SQL files and banner image to remove -dev suffix\n- Update README: this is expected to be the last pre-release before GA\n(v1.0.0)\n- Update test expected outputs for minor ordering/warning differences\n\n## Testing\n- `make test` passes (40/40 tests)\n- `make format-check` passes",
          "timestamp": "2026-02-02T19:09:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1a23bc6bbb06dcec0c7e6efa50b2a51c08882926"
        },
        "date": 1770100113631,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 259.364,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "1a23bc6bbb06dcec0c7e6efa50b2a51c08882926",
          "message": "Release v0.5.0 (#197)\n\n## Summary\n- Update version from 0.5.0-dev to 0.5.0 across all files\n- Rename SQL files and banner image to remove -dev suffix\n- Update README: this is expected to be the last pre-release before GA\n(v1.0.0)\n- Update test expected outputs for minor ordering/warning differences\n\n## Testing\n- `make test` passes (40/40 tests)\n- `make format-check` passes",
          "timestamp": "2026-02-02T19:09:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1a23bc6bbb06dcec0c7e6efa50b2a51c08882926"
        },
        "date": 1770186461092,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 247.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "b0eea09b625cbbca5db98be033582c06e04f9cd7",
          "message": "refactor: remove unused tp_limits_init() (#190)\n\nGUC parameter 'pg_textsearch.default_limit' is initialized in\n_PG_init(), this tp_limits_init() is not used, so remove it.",
          "timestamp": "2026-02-05T05:09:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b0eea09b625cbbca5db98be033582c06e04f9cd7"
        },
        "date": 1770273092906,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 266.186,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "03969c526b74a012e38d5f140b008bf15459ed37",
          "message": "fix: implement CREATE INDEX CONCURRENTLY support (#200)\n\n## Summary\n\n- Implement proper `ambulkdelete` callback to report all indexed CTIDs\nduring CIC validation\n- Add SQL test verifying CIC works correctly without duplicate entries\n- Add shell script tests for concurrent table operations during CIC\n\n## Background\n\nCREATE INDEX CONCURRENTLY requires the `ambulkdelete` callback to report\nall existing TIDs so `validate_index` can determine which rows need to\nbe inserted. Without this, all rows would be re-inserted causing\nduplicates.\n\nThe fix adds two helper functions in `vacuum.c`:\n- `tp_bulkdelete_memtable_ctids`: iterates through document lengths\ndshash to collect memtable CTIDs\n- `tp_bulkdelete_segment_ctids`: iterates through segment document maps\nto collect segment CTIDs\n\n## Testing\n\n- SQL test `test/sql/concurrent_build.sql`:\n  - Basic CIC index creation\n  - Verification that index is marked valid\n  - Verification that no duplicate entries are produced\n  - DROP INDEX CONCURRENTLY\n  - Regular CREATE INDEX for comparison\n\n- Shell test `test/scripts/cic.sh`:\n  - Concurrent INSERTs during CIC (100k documents)\n  - Concurrent UPDATEs during CIC\n  - Concurrent DELETEs during CIC\n  - Mixed concurrent operations during CIC\n  - Multiple concurrent CIC operations\n- Configured with lower spill thresholds to trigger multiple memtable\nspills",
          "timestamp": "2026-02-05T23:57:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/03969c526b74a012e38d5f140b008bf15459ed37"
        },
        "date": 1770359317821,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 259.93,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "07b8656acdb9099d4496a7d4f5a9a3657f28a0aa",
          "message": "data: regenerate MS-MARCO ground truth with exact avgdl\n\nRegenerated ground truth using total_len/total_docs for avg_doc_len\ninstead of the rounded display value. Results now match Tapir to\nfloat precision:\n\n- Before: 1.2% match at 4 decimal places, max diff 0.00066\n- After: 99.4% match at 4 decimal places, max diff 0.0000022\n\nThe remaining 0.6% are at rounding boundaries with diff < 3e-6.\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-02-06T17:55:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/07b8656acdb9099d4496a7d4f5a9a3657f28a0aa"
        },
        "date": 1770401649430,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 256.021,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "753eed5ee0018203154314a9e6a6a6ebc1bd1ac7",
          "message": "docs: update benchmark comparison page with Feb 6 results (#211)\n\n## Summary\n\n- Update source copy of `benchmarks/gh-pages/comparison.html` with the\nFeb 6 benchmark data\n- The benchmark workflow copies this file onto gh-pages on every run, so\nthe source copy must stay in sync with any manual edits made directly on\ngh-pages\n\nThis was overwritten when a benchmark run deployed the stale source\nversion on top of a manual gh-pages edit.",
          "timestamp": "2026-02-06T21:06:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/753eed5ee0018203154314a9e6a6a6ebc1bd1ac7"
        },
        "date": 1770445404706,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 248.457,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "348671dc23717a8278b564a3e1d5b6ad7fb02199",
          "message": "feat: iterative index scan with exponential backoff (#209)\n\nCloses #143\n\n## Summary\n\n- When a WHERE clause post-filters BM25 index results, the one-shot\ntop-k scan could return fewer rows than LIMIT requests (or zero rows),\neven when qualifying rows exist in the table\n- Fix: `tp_gettuple` now doubles the internal limit and re-executes the\nscoring query when results are exhausted, skipping already-returned rows\n- Terminates when `result_count < max_results_used` (all matching docs\nfound) or the limit reaches `TP_MAX_QUERY_LIMIT`\n\n## Testing\n\nNew `rescan.sql` regression test with 9 scenarios:\n- Complete miss (all top-k filtered out by WHERE)\n- Default limit + WHERE (no explicit LIMIT)\n- Partial miss (some results pass filter)\n- Result ordering preserved after rescan\n- Large table with aggressive filtering (500 rows, 2% match rate)\n- WHERE on non-indexed column\n- LIMIT + OFFSET + WHERE\n- Multiple backoffs required (8 doublings with default_limit=5)\n- Backoff terminates when all matching rows exhausted\n\nUpdated expected output for `limits` and `mixed` tests which now\ncorrectly return additional rows that the rescan finds.",
          "timestamp": "2026-02-07T15:53:42Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/348671dc23717a8278b564a3e1d5b6ad7fb02199"
        },
        "date": 1770532137020,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 296.219,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.34,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.62,
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
          "id": "7c35333d63b364452d712aa405e7463691c40d0b",
          "message": "Update banner image for v0.4.0 (#131)\n\n## Summary\n- Add new banner image for v0.4.0\n- Update README to reference the new image",
          "timestamp": "2026-01-10T03:07:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7c35333d63b364452d712aa405e7463691c40d0b"
        },
        "date": 1768026153601,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 540967.797,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 10.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 15.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 18.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 25.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 30.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 41.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 63.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 30.87,
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
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "b5c04a7ab49812e54573104b3c1d178100608963",
          "message": "Add compression validation job to benchmark workflow",
          "timestamp": "2026-01-10T19:48:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b5c04a7ab49812e54573104b3c1d178100608963"
        },
        "date": 1768075449971,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 533821.918,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 12.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 14.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 20.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 26.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 33.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 43.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 70.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 34.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "3eb3a6fb137a2fc8bc01da23ecb767d6f23fa08f",
          "message": "Fix validate-compression job setup\n\nThe Postgres server failed to start because the job was missing:\n- Unix socket directory configuration\n- PGHOST environment variable\n- The -w flag for pg_ctl to wait for startup\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-10T20:20:24Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3eb3a6fb137a2fc8bc01da23ecb767d6f23fa08f"
        },
        "date": 1768077373208,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 517845.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 12.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 15.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 20.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 27.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 33.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 43.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 68.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 33.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "7c35333d63b364452d712aa405e7463691c40d0b",
          "message": "Update banner image for v0.4.0 (#131)\n\n## Summary\n- Add new banner image for v0.4.0\n- Update README to reference the new image",
          "timestamp": "2026-01-10T03:07:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7c35333d63b364452d712aa405e7463691c40d0b"
        },
        "date": 1768112569696,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 539548.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 11.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 16.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 21.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 26.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 32.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 44.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 65.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 31.62,
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
          "id": "564b18a6e2fa542e2e37abbf35be189ecbecd6b3",
          "message": "Implement AMPROP_DISTANCE_ORDERABLE in amproperty (#133)\n\n## Summary\n\nThe `amproperty` callback lets access methods report capabilities that\nPostgres can't infer automatically. For properties like\n`distance_orderable`, Postgres returns NULL if the access method doesn't\nimplement the callback.\n\nSince BM25 indexes support ORDER BY via the `<@>` operator, we should\nreport `distance_orderable = true`. This improves reporting of index\ncapabilities through tools like pgAdmin.\n\nFixes #103\n\n## Testing\nAll existing regression tests pass.",
          "timestamp": "2026-01-12T03:23:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/564b18a6e2fa542e2e37abbf35be189ecbecd6b3"
        },
        "date": 1768199097590,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 572607.906,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 17.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 22.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 29.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 35.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 46.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 71.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 35.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "4140c83e6bf828cb20e094afdb93f9f8743f4a5c",
          "message": "Increase expansion factor to 0.8 for large datasets",
          "timestamp": "2026-01-12T21:11:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4140c83e6bf828cb20e094afdb93f9f8743f4a5c"
        },
        "date": 1768253314784,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 317720.159,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 21.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 24.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 28.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 30.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 37.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 44.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 54.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 78.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 42.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3599.59,
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
          "id": "4140c83e6bf828cb20e094afdb93f9f8743f4a5c",
          "message": "Increase expansion factor to 0.8 for large datasets",
          "timestamp": "2026-01-12T21:11:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4140c83e6bf828cb20e094afdb93f9f8743f4a5c"
        },
        "date": 1768253434185,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 300445.648,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 31.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 34.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 37.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 42.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 49.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 56.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 68.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 91.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 54.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3599.77,
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
          "id": "411527a28812edbf9a2ccb175811f449ce4278b0",
          "message": "Release v0.4.0 (#136)\n\n## Summary\n\n- Update version from 0.4.0-dev to 0.4.0\n- Rename SQL files to drop -dev suffix\n- Add CHANGELOG entries for v0.3.0 and v0.4.0\n\n## v0.4.0 Highlights\n\n- **Posting list compression**: Delta encoding + bitpacking for ~41%\nsmaller indexes\n- Coverity static analysis integration\n- AMPROP_DISTANCE_ORDERABLE implementation\n- Fixed 'too many LWLocks taken' error on partitioned tables\n\n## Testing\n\n- `make test` passes (37 tests)\n- `make format-check` passes",
          "timestamp": "2026-01-13T02:33:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/411527a28812edbf9a2ccb175811f449ce4278b0"
        },
        "date": 1768285314298,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 537804.021,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 14.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 17.53,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 21.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 29.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 33.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 42.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 66.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 33.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1274.71,
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
          "id": "0b1fde1605f16cc34b195c41209f76e13c1ac303",
          "message": "Use binary search for WAND-style doc ID seeking in BMW\n\nThe multi-term BMW algorithm now uses binary search (via\ntp_segment_posting_iterator_seek) when skipping documents that can't\nbeat the threshold. Previously, iterators advanced one document at a\ntime.\n\nKey changes:\n- Add seek_term_to_doc() that uses binary search on skip entries when\n  the target is beyond the current block\n- Add find_next_candidate_doc_id() to find the minimum doc ID among\n  terms not at the current pivot\n- Modify skip_pivot_document() to seek to the next candidate instead\n  of advancing by 1\n\nThis changes the complexity from O(blocks) to O(log blocks) when\nskipping over documents with sparse term overlap.\n\nCloses #141",
          "timestamp": "2026-01-13T16:57:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0b1fde1605f16cc34b195c41209f76e13c1ac303"
        },
        "date": 1768324910436,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 549316.324,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 13.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 18.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 24.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 30.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 43.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 62.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 31.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "fbff39af22ea556ca9cab93b45e437d49a35f9fd",
          "message": "Add GUC and stats for WAND seek optimization\n\nAdd pg_textsearch.enable_wand_seek GUC to control whether WAND-style\nbinary search seeking is used (default: on). This allows A/B\ncomparison to measure the optimization's impact.\n\nAdd seek statistics to TpBMWStats:\n- seeks_performed: number of binary search seeks\n- docs_seeked: total doc IDs skipped via seeking\n- linear_advances: single-doc advances (no seek benefit)\n- seek_to_same_doc: seeks that landed on same/next doc\n\nThese stats are logged when pg_textsearch.log_bmw_stats=on.\n\nLocal testing shows the current implementation adds overhead without\nbenefit for typical workloads. The find_next_candidate_doc_id call\non every skip appears to cost more than the seeks save.",
          "timestamp": "2026-01-13T17:35:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fbff39af22ea556ca9cab93b45e437d49a35f9fd"
        },
        "date": 1768326480489,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 530127.475,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 13.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 11.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 13.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 17.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 22.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 27.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 36.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 54.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 27.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1274.81,
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
          "id": "7e7e75e7ae39399c61ec85606231226baab39232",
          "message": "Optimize WAND seek by maintaining sorted term array\n\nInspired by Tantivy's WAND implementation, maintain term iterators\nsorted by current doc ID. This makes find_pivot_doc_id O(1) and\nfind_next_candidate_doc_id O(1) typical case instead of O(N).\n\nKey changes:\n- Add term_current_doc_id() helper for consistent doc ID access\n- Add compare_terms_by_doc_id() and sort_terms_by_doc_id() for sorting\n- Sort terms after initialization in init_segment_term_states()\n- Re-sort after advancing terms in score_pivot_document() and\n  skip_pivot_document()\n- Simplify find_pivot_doc_id() to just return first term's doc ID\n- Optimize find_next_candidate_doc_id() to scan from start until\n  first term past pivot (O(1) when only one term at pivot)\n- Add early exit in compute_pivot_max_score() using sorted order\n\nLocal benchmarks show performance parity between seek ON/OFF modes,\nresolving the overhead issue from the previous O(N) iteration.",
          "timestamp": "2026-01-13T18:48:40Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7e7e75e7ae39399c61ec85606231226baab39232"
        },
        "date": 1768333200404,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 544590.363,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 13.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 18.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 40.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 80.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 140.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 367.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 671.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 198.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "7e7e75e7ae39399c61ec85606231226baab39232",
          "message": "Optimize WAND seek by maintaining sorted term array\n\nInspired by Tantivy's WAND implementation, maintain term iterators\nsorted by current doc ID. This makes find_pivot_doc_id O(1) and\nfind_next_candidate_doc_id O(1) typical case instead of O(N).\n\nKey changes:\n- Add term_current_doc_id() helper for consistent doc ID access\n- Add compare_terms_by_doc_id() and sort_terms_by_doc_id() for sorting\n- Sort terms after initialization in init_segment_term_states()\n- Re-sort after advancing terms in score_pivot_document() and\n  skip_pivot_document()\n- Simplify find_pivot_doc_id() to just return first term's doc ID\n- Optimize find_next_candidate_doc_id() to scan from start until\n  first term past pivot (O(1) when only one term at pivot)\n- Add early exit in compute_pivot_max_score() using sorted order\n\nLocal benchmarks show performance parity between seek ON/OFF modes,\nresolving the overhead issue from the previous O(N) iteration.",
          "timestamp": "2026-01-13T18:48:40Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7e7e75e7ae39399c61ec85606231226baab39232"
        },
        "date": 1768335449140,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 497189.531,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 19.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 19.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 23.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 35.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 57.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 91.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 197.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 350.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 112.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "12e50c25163c3b715f1f558e6c3111ece9c5cd5a",
          "message": "Use insertion sort instead of qsort for term array\n\nProfiling showed qsort was taking 70% of query time due to its\noverhead on small arrays. Insertion sort is O(N) for nearly-sorted\narrays, which is our case since only 1-2 terms move after advancing.\n\nFor N=2-5 terms (typical), this is essentially free compared to qsort.",
          "timestamp": "2026-01-13T21:55:36Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/12e50c25163c3b715f1f558e6c3111ece9c5cd5a"
        },
        "date": 1768342457027,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 471734.225,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 18.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 17.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 19.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 26.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 34.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 44.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 82.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 41.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "12e50c25163c3b715f1f558e6c3111ece9c5cd5a",
          "message": "Use insertion sort instead of qsort for term array\n\nProfiling showed qsort was taking 70% of query time due to its\noverhead on small arrays. Insertion sort is O(N) for nearly-sorted\narrays, which is our case since only 1-2 terms move after advancing.\n\nFor N=2-5 terms (typical), this is essentially free compared to qsort.",
          "timestamp": "2026-01-13T21:55:36Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/12e50c25163c3b715f1f558e6c3111ece9c5cd5a"
        },
        "date": 1768344946782,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 550485.442,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 13.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 13.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 15.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 21.27,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 27.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 35.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 50.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 78.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 35.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1270.02,
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
          "id": "12e50c25163c3b715f1f558e6c3111ece9c5cd5a",
          "message": "Use insertion sort instead of qsort for term array\n\nProfiling showed qsort was taking 70% of query time due to its\noverhead on small arrays. Insertion sort is O(N) for nearly-sorted\narrays, which is our case since only 1-2 terms move after advancing.\n\nFor N=2-5 terms (typical), this is essentially free compared to qsort.",
          "timestamp": "2026-01-13T21:55:36Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/12e50c25163c3b715f1f558e6c3111ece9c5cd5a"
        },
        "date": 1768344946404,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 525710.429,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 11.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 12.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 13.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 19.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 27.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 37.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 52.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 81.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 36.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "6e9289e3137f0e15329f440f92c8be69ac56a3a4",
          "message": "Remove sorted array overhead from WAND traversal\n\nThe sorted array approach was causing 16-17% overhead in profiling due\nto repeated insertion sorts after each document advance. This commit\nsimplifies to O(N) scans for finding the minimum doc ID, which has the\nsame asymptotic complexity but avoids memory writes.\n\nKey changes:\n- find_pivot_doc_id: now scans all terms for minimum (was O(1) sorted)\n- find_next_candidate_doc_id: now scans all terms for minimum past pivot\n- compute_pivot_max_score: uses continue instead of break\n- score_pivot_document: uses continue instead of break\n- skip_pivot_document: uses continue instead of break\n- Removed sort_terms_by_doc_id entirely\n\nThe WAND-style binary search seeking is retained. This removes the\nsorting overhead while keeping the seek optimization.",
          "timestamp": "2026-01-13T23:03:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/6e9289e3137f0e15329f440f92c8be69ac56a3a4"
        },
        "date": 1768346310395,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 525453.624,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 11.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 12.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 18.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 23.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 28.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 39.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 59.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 30.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "7bf2f81d6d4fb80c4b57f62ac0568647ae2b3910",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bf2f81d6d4fb80c4b57f62ac0568647ae2b3910"
        },
        "date": 1768350936289,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 505580.606,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 11.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 9.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 11.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 16.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 20.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 26.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 37.27,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 58.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 27.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768355521462,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 496589.385,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 19.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 19.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 20.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 24.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 29.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 35.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 45.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 66.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 35.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1275,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768356980666,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 482850.615,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 18.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 18.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 19.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 25.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 30.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 36.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 48.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 72.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 37.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768358647661,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 513134.859,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 10.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 9.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 11.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 15.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 21.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 26.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 37.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 58.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 28.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "0db7c2097de05330cfbcd89a747e9b04fb9f9d81",
          "message": "Add pgspot CI workflow for SQL security checks (#145)\n\n## Summary\n- Add GitHub workflow to run pgspot on extension SQL files for security\nchecks\n- Fix PS002 warning by changing `CREATE OR REPLACE` to `CREATE` for\n`bm25_spill_index` function\n- Ignore PS017 warnings (false positives for type/operator definitions\nwithin the extension)\n\n## Testing\n- All 37 regression tests pass\n- pgspot runs clean on all SQL files with `--ignore PS017`",
          "timestamp": "2026-01-14T04:37:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0db7c2097de05330cfbcd89a747e9b04fb9f9d81"
        },
        "date": 1768371739076,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 525870.634,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 12.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 11.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 12.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 16.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 20.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 25.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 35.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 54.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 27.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1274.75,
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
          "id": "c5a6ea09ab094dd95174f6f7e75d338a680a295d",
          "message": "Lazy CTID loading for segment queries\n\nDefer CTID resolution until final top-k results are known, reducing\nmemory usage from ~53MB to ~60 bytes per query (for k=10).\n\nChanges:\n- Add tp_segment_open_ex() with load_ctids parameter (default: false)\n- Add tp_segment_lookup_ctid() for targeted 6-byte reads\n- Split heap add into tp_topk_add_memtable/tp_topk_add_segment\n- Add tp_topk_resolve_ctids() to resolve CTIDs before extraction\n- Add doc_id field to TpSegmentPosting for deferred resolution\n- Update iterator to conditionally resolve CTIDs\n- Exhaustive path still loads CTIDs (needed for hash table keys)",
          "timestamp": "2026-01-14T06:48:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c5a6ea09ab094dd95174f6f7e75d338a680a295d"
        },
        "date": 1768374450870,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 548079.069,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 27.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 44.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1271.08,
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
          "id": "94c66cc59f38a7e4e2d72832432f7825fb29fd3a",
          "message": "Fix formatting",
          "timestamp": "2026-01-14T18:43:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/94c66cc59f38a7e4e2d72832432f7825fb29fd3a"
        },
        "date": 1768416446553,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 472169.085,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 31.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 51.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 18.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "d32590214fa6b4fbf6a5f8643b6461926873fdf6",
          "message": "Bump version to v0.5.0-dev (#147)\n\n## Summary\n- Bump version from v0.4.0 to v0.5.0-dev\n- Update README to note parallel indexing as the main feature for v0.5.0\n- Add new SQL extension file and update test expected outputs\n\n## Testing\n- CI will verify the version string updates are consistent",
          "timestamp": "2026-01-15T04:27:11Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d32590214fa6b4fbf6a5f8643b6461926873fdf6"
        },
        "date": 1768458371302,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 531380.527,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 10.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 10.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 12.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 17.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 22.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 27.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 38.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 60.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 28.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "75a05c5003b013a45ca4886ea055a175b0c1c351",
          "message": "Add LSAN suppressions for pg_config false positives\n\nThe nightly stress tests were failing because LSAN detected \"leaks\" in\nPostgreSQL's pg_config utility. These are false positives - pg_config\nis a short-lived command-line tool that allocates memory and exits\nwithout freeing it, which is normal and expected behavior.\n\nAdd suppressions for pg_strdup and get_configdata to fix the failures.",
          "timestamp": "2026-01-16T05:56:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/75a05c5003b013a45ca4886ea055a175b0c1c351"
        },
        "date": 1768544513446,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 537169.666,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 17.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "72239eee572aba5253e13aeb0bb5a086f81ace7e",
          "message": "Document Python 3.10 pin for wikiextractor (#151)\n\n## Summary\n\n- Add comment explaining why Python 3.10 is pinned in the\ncomparative-benchmark job\n- wikiextractor is unmaintained and breaks on Python 3.11+",
          "timestamp": "2026-01-17T02:20:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/72239eee572aba5253e13aeb0bb5a086f81ace7e"
        },
        "date": 1768630840844,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 511950.534,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 11.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 46.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "72239eee572aba5253e13aeb0bb5a086f81ace7e",
          "message": "Document Python 3.10 pin for wikiextractor (#151)\n\n## Summary\n\n- Add comment explaining why Python 3.10 is pinned in the\ncomparative-benchmark job\n- wikiextractor is unmaintained and breaks on Python 3.11+",
          "timestamp": "2026-01-17T02:20:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/72239eee572aba5253e13aeb0bb5a086f81ace7e"
        },
        "date": 1768717310698,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 513677.247,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 46.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "efe88321a9fabe17cd60a5adf155d804b1796161",
          "message": "Fix potential integer overflow in BMW stats tracking (#155)\n\n## Summary\n- Fix Coverity-reported integer overflow (CID 641544) in\n`skip_pivot_document()`\n- Rewrite `landed_doc <= pivot_doc_id + 1` as `landed_doc - pivot_doc_id\n<= 1` to avoid overflow when `pivot_doc_id` equals `UINT32_MAX`\n\n## Testing\n- All SQL regression tests pass",
          "timestamp": "2026-01-19T01:31:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/efe88321a9fabe17cd60a5adf155d804b1796161"
        },
        "date": 1768803799159,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 533684.016,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "efe88321a9fabe17cd60a5adf155d804b1796161",
          "message": "Fix potential integer overflow in BMW stats tracking (#155)\n\n## Summary\n- Fix Coverity-reported integer overflow (CID 641544) in\n`skip_pivot_document()`\n- Rewrite `landed_doc <= pivot_doc_id + 1` as `landed_doc - pivot_doc_id\n<= 1` to avoid overflow when `pivot_doc_id` equals `UINT32_MAX`\n\n## Testing\n- All SQL regression tests pass",
          "timestamp": "2026-01-19T01:31:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/efe88321a9fabe17cd60a5adf155d804b1796161"
        },
        "date": 1768890197326,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 585761.265,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 30.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 17.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "c088e94febb15a9b4901fc522dac675f8f9f89fb",
          "message": "Fix nightly stress test failures (#157)\n\n## Summary\n- Fix PostgreSQL cache to include install directory (was only caching\nbuild dir)\n- Add LSAN suppression for `save_ps_display_args` false positive\n- Add `issues: write` permission for failure notification job\n\nThe nightly stress tests have been failing since Jan 9 due to the cache\nissue causing `initdb` to fail with \"postgres binary not found\".\n\n## Testing\n- Trigger manual run: `gh workflow run nightly-stress.yml --field\nduration_minutes=5`",
          "timestamp": "2026-01-20T16:01:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c088e94febb15a9b4901fc522dac675f8f9f89fb"
        },
        "date": 1768976511107,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 513957.993,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1269.35,
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
          "id": "bde408ca9e72fca686e47ef360cee27c217d2ab8",
          "message": "Add parallel bulk index build support, part 1/2 (#114)\n\n## Summary\n\n- Implement parallel CREATE INDEX using Postgres parallel infrastructure\n- Each worker scans heap concurrently, builds local memtables, writes L0\nsegments\n- Leader participates as worker 0; links all worker segment chains at\nend\n- Compaction performed on leader node; part 2/2 will parallelize\ncompaction\n\n## Testing\n\n- All 33 regression tests pass\n- Falls back to serial when `max_parallel_maintenance_workers=0`",
          "timestamp": "2026-01-21T19:00:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bde408ca9e72fca686e47ef360cee27c217d2ab8"
        },
        "date": 1769062818907,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 351795.753,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 20.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3510.82,
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
          "id": "2ae9d52c42fe284f6bcae1850c64e6a769c0ef03",
          "message": "fix: use FSM instead of truncation for parallel build page reclamation\n\nAfter parallel index build completes, unused pre-allocated pool pages\nwere being truncated. However, the subsequent compaction step would then\nextend the relation again, negating the truncation.\n\nInstead of truncating, record unused pool pages in the Free Space Map\n(FSM) so they can be reused by compaction. This prevents the index size\nregression observed in nightly benchmarks since the parallel build PR\nmerged.\n\nKey changes:\n- Rename truncate_unused_pool_pages to reclaim_unused_pool_pages\n- Use RecordFreeIndexPage to add unused pages to FSM\n- Call IndexFreeSpaceMapVacuum to make pages discoverable",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2ae9d52c42fe284f6bcae1850c64e6a769c0ef03"
        },
        "date": 1769108601051,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 340118.054,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 11.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3511.56,
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
          "id": "95f28c8cd59584b198dd6d0bc50e893dda57b1d6",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nTwo fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build, instead\n   of failing with an error, we now dynamically extend the relation.\n   This ensures builds always succeed even if the initial size estimate\n   was too conservative. A NOTICE is logged on first overflow so users\n   can tune the expansion factor if desired.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/95f28c8cd59584b198dd6d0bc50e893dda57b1d6"
        },
        "date": 1769110706118,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 368022.774,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3509.81,
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
          "id": "95f28c8cd59584b198dd6d0bc50e893dda57b1d6",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nTwo fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build, instead\n   of failing with an error, we now dynamically extend the relation.\n   This ensures builds always succeed even if the initial size estimate\n   was too conservative. A NOTICE is logged on first overflow so users\n   can tune the expansion factor if desired.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/95f28c8cd59584b198dd6d0bc50e893dda57b1d6"
        },
        "date": 1769111686131,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 347334.746,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 20.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3511.14,
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
          "id": "d66f0d8bdeb63aca2c43247243494af89820c199",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nThree fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build (in\n   tp_pool_get_page), instead of failing with an error, we now\n   dynamically extend the relation. A NOTICE is logged on first overflow\n   so users can tune the expansion factor if desired.\n\n3. When writing page index and the pool is exhausted (in\n   write_page_index_from_pool), we now also dynamically extend instead\n   of failing.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d66f0d8bdeb63aca2c43247243494af89820c199"
        },
        "date": 1769113053262,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 356085.896,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.03,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3511.99,
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
          "id": "d6ef453701d36c559e9220af54a9013e5e79df78",
          "message": "style: fix formatting",
          "timestamp": "2026-01-23T01:55:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d6ef453701d36c559e9220af54a9013e5e79df78"
        },
        "date": 1769145449298,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 352292.205,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 11.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.53,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3511.78,
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
          "id": "24735ab9664f56e47eddfad6e0164c941563108d",
          "message": "Fix 'too many LWLocks taken' error when scanning hypertables (#164)\n\n## Summary\n- Fixes \"too many LWLocks taken\" error when querying BM25 indexes on\nhypertables with many chunks (e.g., 587 chunks)\n- Releases the per-index LWLock after `tp_memtable_search()` completes\n\nThis is the scan-path equivalent of the build-path fix in b7f6539.\n\n## Problem\nWhen scanning a hypertable with many chunks, each chunk's BM25 index\nscan acquires a per-index LWLock but never releases it until transaction\nend. With 587 chunks, this exceeds Postgres's ~200 lock limit.\n\n## Solution\nRelease the lock immediately after `tp_memtable_search()` returns, since\nwe've already extracted all CTIDs into memory at that point.\n\n## Testing\n- Tested with 587-chunk hypertable (2.2M rows) - query now succeeds\n- All regression tests pass\n- All shell-based tests pass",
          "timestamp": "2026-01-22T23:28:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/24735ab9664f56e47eddfad6e0164c941563108d"
        },
        "date": 1769149182431,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 361308.562,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 11.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 46.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3512.19,
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
          "id": "24735ab9664f56e47eddfad6e0164c941563108d",
          "message": "Fix 'too many LWLocks taken' error when scanning hypertables (#164)\n\n## Summary\n- Fixes \"too many LWLocks taken\" error when querying BM25 indexes on\nhypertables with many chunks (e.g., 587 chunks)\n- Releases the per-index LWLock after `tp_memtable_search()` completes\n\nThis is the scan-path equivalent of the build-path fix in b7f6539.\n\n## Problem\nWhen scanning a hypertable with many chunks, each chunk's BM25 index\nscan acquires a per-index LWLock but never releases it until transaction\nend. With 587 chunks, this exceeds Postgres's ~200 lock limit.\n\n## Solution\nRelease the lock immediately after `tp_memtable_search()` returns, since\nwe've already extracted all CTIDs into memory at that point.\n\n## Testing\n- Tested with 587-chunk hypertable (2.2M rows) - query now succeeds\n- All regression tests pass\n- All shell-based tests pass",
          "timestamp": "2026-01-22T23:28:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/24735ab9664f56e47eddfad6e0164c941563108d"
        },
        "date": 1769235494311,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 355470.846,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 11.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 46.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3512.27,
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
          "id": "d69525e0963a9960de7d7338c89db7e2391d34db",
          "message": "Fix Coverity static analysis issues (#170)\n\n## Summary\n\n- Remove dead code in `write_term_postings()` where a condition can\nnever be true\n- Remove redundant null checks before `pfree()` in\n`tp_segment_posting_iterator_init()`\n- Initialize `doc_count` variable to satisfy Coverity's inter-procedural\nanalysis\n\n## Testing\n\n- All SQL regression tests pass\n- Concurrency, recovery, and segment shell tests pass",
          "timestamp": "2026-01-25T00:48:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d69525e0963a9960de7d7338c89db7e2391d34db"
        },
        "date": 1769321852636,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 348143,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3510.79,
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
          "id": "3244d05a86658620ecb2d82253ef7916604f839d",
          "message": "Fix zero scores when querying hypertables with BM25 index (#168)\n\n## Summary\n\nThis PR fixes zero scores when querying hypertables with BM25 indexes.\n\n**Commit 1: Planner hook fix for CustomScan**\n- Add `T_CustomScan` handling in `plan_has_bm25_indexscan()` to detect\nBM25 index scans nested inside custom scans (e.g., TimescaleDB's\nConstraintAwareAppend)\n- Add `T_CustomScan` handling in `replace_scores_in_plan()` to replace\nscore expressions in custom scan children\n\n**Commit 2: Standalone scoring fix for hypertable parent indexes**\n- When using standalone BM25 scoring with a hypertable parent index\nname, the code was falling back to child index stats but NOT switching\nto the child's index relation and segment metadata\n- This caused IDF calculation to fail because it was looking up document\nfrequencies in the parent index's segments (which are empty)\n- The fix switches to the child index for segment access when falling\nback to a child index's state\n\n## Testing\n\n- Verified fix with reproduction case from bug report\n- Added Test 5 in partitioned.sql for MergeAppend score expression\nreplacement\n- Added test/scripts/hypertable.sh for optional TimescaleDB integration\ntesting (runs only if TimescaleDB is installed)\n\n```sql\n-- Both queries now return proper BM25 scores:\n\n-- Query through parent hypertable with ORDER BY\nSELECT content, -(content <@> to_bm25query('database', 'hyper_idx')) as score\nFROM hyper_docs\nORDER BY content <@> to_bm25query('database', 'hyper_idx')\nLIMIT 5;\n\n-- Standalone scoring with parent index name\nSELECT content, (content <@> to_bm25query('database', 'hyper_idx')) as score\nFROM hyper_docs\nWHERE content LIKE '%database%';\n```",
          "timestamp": "2026-01-25T16:35:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3244d05a86658620ecb2d82253ef7916604f839d"
        },
        "date": 1769408378568,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 379718.741,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 22.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 49.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3510.89,
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
          "id": "70747956a7287acc6da5743f831c4cfe651e8d22",
          "message": "chore: clean up legacy am/extension name and remove unused test script (#177)\n\nIn src/am/build.c, there is still code that refers to the legacy access\nmethod and extension name, tapir. This commit updates those references\nto the current names.\n\nscripts/generate_test_case.py is no longer used (replaced by\nvalidation.sql) and thus gets removed.",
          "timestamp": "2026-01-27T02:08:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/70747956a7287acc6da5743f831c4cfe651e8d22"
        },
        "date": 1769494672456,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 372272.269,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 31.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 50.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 17.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3510.75,
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
          "id": "70747956a7287acc6da5743f831c4cfe651e8d22",
          "message": "chore: clean up legacy am/extension name and remove unused test script (#177)\n\nIn src/am/build.c, there is still code that refers to the legacy access\nmethod and extension name, tapir. This commit updates those references\nto the current names.\n\nscripts/generate_test_case.py is no longer used (replaced by\nvalidation.sql) and thus gets removed.",
          "timestamp": "2026-01-27T02:08:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/70747956a7287acc6da5743f831c4cfe651e8d22"
        },
        "date": 1769581195955,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 385321.545,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 30.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 49.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3510.95,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769668009043,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 369124.265,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.04,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 20.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 46.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3511.09,
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
          "id": "276f64750340502e0fbce0f35f23c3cc4a8b93c8",
          "message": "fix: handle page boundaries when writing dictionary entries in parallel build\n\nThe dictionary entry writing code in tp_merge_worker_buffers_to_segment()\nassumed all entries fit on the first page. With large datasets (150K+ docs),\ndictionary entries span multiple pages, causing \"Invalid logical page\" errors.\n\nFixed by:\n1. Flushing segment writer before writing entries\n2. Using page-boundary-aware code that calculates logical/physical pages\n3. Handling entries that span two pages like merge.c does",
          "timestamp": "2026-01-29T15:34:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/276f64750340502e0fbce0f35f23c3cc4a8b93c8"
        },
        "date": 1769710989214,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 282172.795,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 3.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 7.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 46.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2178.38,
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
          "id": "ef7051aeffdeb9076de500185859baeb4a1549ba",
          "message": "fix: revert to nworkers > 0 for parallel build, update expected output\n\n1 worker still helps with read/write parallelism (worker scans while\nleader writes). Reverted the nworkers > 1 check back to nworkers > 0.\n\nUpdated test expected output after confirming tests pass with both\ndebug and release builds.",
          "timestamp": "2026-01-29T23:52:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ef7051aeffdeb9076de500185859baeb4a1549ba"
        },
        "date": 1769731126272,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 304878.191,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 17.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 31.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.46,
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
          "id": "a9cf8f4b5aa880608a7dfbc736429b2693a6028e",
          "message": "chore: add max_parallel_maintenance_workers to Cranfield benchmark\n\nAlso add comments explaining the maintenance_work_mem requirement\nfor parallel builds (32MB per worker + leader).\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-30T01:59:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a9cf8f4b5aa880608a7dfbc736429b2693a6028e"
        },
        "date": 1769739677689,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 277840.938,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.53,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.47,
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
          "id": "a9cf8f4b5aa880608a7dfbc736429b2693a6028e",
          "message": "chore: add max_parallel_maintenance_workers to Cranfield benchmark\n\nAlso add comments explaining the maintenance_work_mem requirement\nfor parallel builds (32MB per worker + leader).\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-30T01:59:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a9cf8f4b5aa880608a7dfbc736429b2693a6028e"
        },
        "date": 1769741836614,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 266516.318,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.48,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769754452665,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 365904.378,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3511.14,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769840630609,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 373431.948,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 28.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 3512.24,
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
          "id": "60604649c2cef4b638c318a545bbbe6f72801d93",
          "message": "ci: use standard runners for benchmarks (#193)\n\n## Summary\n- Reverts benchmark workflow from `ubuntu-latest-16-cores` to standard\n`ubuntu-latest` runners\n- Adjusts PostgreSQL memory settings to fit within ~7GB RAM limit\n- Fixes jobs that were queueing indefinitely due to unavailable 16-core\nrunners\n\n## Changes\n| Setting | Before | After |\n|---------|--------|-------|\n| Runner | ubuntu-latest-16-cores | ubuntu-latest |\n| shared_buffers | 4GB | 1GB |\n| effective_cache_size | 12GB | 4GB |\n| maintenance_work_mem | 1GB | 512MB |\n| work_mem | 128MB | 64MB |\n| wal_buffers | 64MB | 16MB |\n\nThe 512MB maintenance_work_mem is still well above the 64MB minimum\nrequired for parallel index builds.\n\n## Testing\nManually triggered workflow succeeded and results look reasonable.",
          "timestamp": "2026-02-01T01:48:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/60604649c2cef4b638c318a545bbbe6f72801d93"
        },
        "date": 1769927257489,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 269632.915,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 31.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 51.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 17.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.48,
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
          "id": "60604649c2cef4b638c318a545bbbe6f72801d93",
          "message": "ci: use standard runners for benchmarks (#193)\n\n## Summary\n- Reverts benchmark workflow from `ubuntu-latest-16-cores` to standard\n`ubuntu-latest` runners\n- Adjusts PostgreSQL memory settings to fit within ~7GB RAM limit\n- Fixes jobs that were queueing indefinitely due to unavailable 16-core\nrunners\n\n## Changes\n| Setting | Before | After |\n|---------|--------|-------|\n| Runner | ubuntu-latest-16-cores | ubuntu-latest |\n| shared_buffers | 4GB | 1GB |\n| effective_cache_size | 12GB | 4GB |\n| maintenance_work_mem | 1GB | 512MB |\n| work_mem | 128MB | 64MB |\n| wal_buffers | 64MB | 16MB |\n\nThe 512MB maintenance_work_mem is still well above the 64MB minimum\nrequired for parallel index builds.\n\n## Testing\nManually triggered workflow succeeded and results look reasonable.",
          "timestamp": "2026-02-01T01:48:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/60604649c2cef4b638c318a545bbbe6f72801d93"
        },
        "date": 1770014185494,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 278103.978,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 49.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.48,
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
          "id": "1a23bc6bbb06dcec0c7e6efa50b2a51c08882926",
          "message": "Release v0.5.0 (#197)\n\n## Summary\n- Update version from 0.5.0-dev to 0.5.0 across all files\n- Rename SQL files and banner image to remove -dev suffix\n- Update README: this is expected to be the last pre-release before GA\n(v1.0.0)\n- Update test expected outputs for minor ordering/warning differences\n\n## Testing\n- `make test` passes (40/40 tests)\n- `make format-check` passes",
          "timestamp": "2026-02-02T19:09:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1a23bc6bbb06dcec0c7e6efa50b2a51c08882926"
        },
        "date": 1770100115696,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 293497.746,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 6.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 9.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 15.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 24.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 8.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1202.57,
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
          "id": "1a23bc6bbb06dcec0c7e6efa50b2a51c08882926",
          "message": "Release v0.5.0 (#197)\n\n## Summary\n- Update version from 0.5.0-dev to 0.5.0 across all files\n- Rename SQL files and banner image to remove -dev suffix\n- Update README: this is expected to be the last pre-release before GA\n(v1.0.0)\n- Update test expected outputs for minor ordering/warning differences\n\n## Testing\n- `make test` passes (40/40 tests)\n- `make format-check` passes",
          "timestamp": "2026-02-02T19:09:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1a23bc6bbb06dcec0c7e6efa50b2a51c08882926"
        },
        "date": 1770186462849,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 272628.422,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 4.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 6.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 9.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 14.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 23.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 8.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1202.27,
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
          "id": "b0eea09b625cbbca5db98be033582c06e04f9cd7",
          "message": "refactor: remove unused tp_limits_init() (#190)\n\nGUC parameter 'pg_textsearch.default_limit' is initialized in\n_PG_init(), this tp_limits_init() is not used, so remove it.",
          "timestamp": "2026-02-05T05:09:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b0eea09b625cbbca5db98be033582c06e04f9cd7"
        },
        "date": 1770273094701,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 283047.729,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 29.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 49.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.48,
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
          "id": "03969c526b74a012e38d5f140b008bf15459ed37",
          "message": "fix: implement CREATE INDEX CONCURRENTLY support (#200)\n\n## Summary\n\n- Implement proper `ambulkdelete` callback to report all indexed CTIDs\nduring CIC validation\n- Add SQL test verifying CIC works correctly without duplicate entries\n- Add shell script tests for concurrent table operations during CIC\n\n## Background\n\nCREATE INDEX CONCURRENTLY requires the `ambulkdelete` callback to report\nall existing TIDs so `validate_index` can determine which rows need to\nbe inserted. Without this, all rows would be re-inserted causing\nduplicates.\n\nThe fix adds two helper functions in `vacuum.c`:\n- `tp_bulkdelete_memtable_ctids`: iterates through document lengths\ndshash to collect memtable CTIDs\n- `tp_bulkdelete_segment_ctids`: iterates through segment document maps\nto collect segment CTIDs\n\n## Testing\n\n- SQL test `test/sql/concurrent_build.sql`:\n  - Basic CIC index creation\n  - Verification that index is marked valid\n  - Verification that no duplicate entries are produced\n  - DROP INDEX CONCURRENTLY\n  - Regular CREATE INDEX for comparison\n\n- Shell test `test/scripts/cic.sh`:\n  - Concurrent INSERTs during CIC (100k documents)\n  - Concurrent UPDATEs during CIC\n  - Concurrent DELETEs during CIC\n  - Mixed concurrent operations during CIC\n  - Multiple concurrent CIC operations\n- Configured with lower spill thresholds to trigger multiple memtable\nspills",
          "timestamp": "2026-02-05T23:57:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/03969c526b74a012e38d5f140b008bf15459ed37"
        },
        "date": 1770359320227,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 281326.277,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 30.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 49.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.48,
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
          "id": "07b8656acdb9099d4496a7d4f5a9a3657f28a0aa",
          "message": "data: regenerate MS-MARCO ground truth with exact avgdl\n\nRegenerated ground truth using total_len/total_docs for avg_doc_len\ninstead of the rounded display value. Results now match Tapir to\nfloat precision:\n\n- Before: 1.2% match at 4 decimal places, max diff 0.00066\n- After: 99.4% match at 4 decimal places, max diff 0.0000022\n\nThe remaining 0.6% are at rounding boundaries with diff < 3e-6.\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-02-06T17:55:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/07b8656acdb9099d4496a7d4f5a9a3657f28a0aa"
        },
        "date": 1770401651890,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 276479.621,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.2,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 7.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 19.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 30.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 49.03,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.46,
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
          "id": "d2d715519a1a15cc5ca73eec533120019a8915b0",
          "message": "feat: implement WAND pivot selection for multi-term queries\n\nReplace the min-doc-id pivot approach with classic WAND pivot\nselection. Terms are sorted by current doc_id and max_scores are\naccumulated until the threshold is exceeded, skipping large ranges\nof documents that can't contribute to top-k results.\n\nKey changes:\n- Add max_score (global max across all blocks) to TpTermState\n- Sort terms by doc_id, maintain order via linear insertion\n- find_wand_pivot walks sorted terms to find true WAND pivot\n- Block-max refinement with Tantivy-style skip advancement\n- 8-term validation test (BMW vs exhaustive + BM25 reference)\n\nAddresses #192.",
          "timestamp": "2026-02-06T20:40:45Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d2d715519a1a15cc5ca73eec533120019a8915b0"
        },
        "date": 1770411036179,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 262924.691,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 8.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 29.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 11.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.47,
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
          "id": "753eed5ee0018203154314a9e6a6a6ebc1bd1ac7",
          "message": "docs: update benchmark comparison page with Feb 6 results (#211)\n\n## Summary\n\n- Update source copy of `benchmarks/gh-pages/comparison.html` with the\nFeb 6 benchmark data\n- The benchmark workflow copies this file onto gh-pages on every run, so\nthe source copy must stay in sync with any manual edits made directly on\ngh-pages\n\nThis was overwritten when a benchmark run deployed the stale source\nversion on top of a manual gh-pages edit.",
          "timestamp": "2026-02-06T21:06:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/753eed5ee0018203154314a9e6a6a6ebc1bd1ac7"
        },
        "date": 1770445407441,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 272569.774,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 3.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 12.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 30,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 47.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.48,
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
          "id": "7c35333d63b364452d712aa405e7463691c40d0b",
          "message": "Update banner image for v0.4.0 (#131)\n\n## Summary\n- Add new banner image for v0.4.0\n- Update README to reference the new image",
          "timestamp": "2026-01-10T03:07:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7c35333d63b364452d712aa405e7463691c40d0b"
        },
        "date": 1768026156043,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18815.974,
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
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "7c35333d63b364452d712aa405e7463691c40d0b",
          "message": "Update banner image for v0.4.0 (#131)\n\n## Summary\n- Add new banner image for v0.4.0\n- Update README to reference the new image",
          "timestamp": "2026-01-10T03:07:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7c35333d63b364452d712aa405e7463691c40d0b"
        },
        "date": 1768112572260,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18777.231,
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
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "564b18a6e2fa542e2e37abbf35be189ecbecd6b3",
          "message": "Implement AMPROP_DISTANCE_ORDERABLE in amproperty (#133)\n\n## Summary\n\nThe `amproperty` callback lets access methods report capabilities that\nPostgres can't infer automatically. For properties like\n`distance_orderable`, Postgres returns NULL if the access method doesn't\nimplement the callback.\n\nSince BM25 indexes support ORDER BY via the `<@>` operator, we should\nreport `distance_orderable = true`. This improves reporting of index\ncapabilities through tools like pgAdmin.\n\nFixes #103\n\n## Testing\nAll existing regression tests pass.",
          "timestamp": "2026-01-12T03:23:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/564b18a6e2fa542e2e37abbf35be189ecbecd6b3"
        },
        "date": 1768199099718,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19166.681,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "411527a28812edbf9a2ccb175811f449ce4278b0",
          "message": "Release v0.4.0 (#136)\n\n## Summary\n\n- Update version from 0.4.0-dev to 0.4.0\n- Rename SQL files to drop -dev suffix\n- Add CHANGELOG entries for v0.3.0 and v0.4.0\n\n## v0.4.0 Highlights\n\n- **Posting list compression**: Delta encoding + bitpacking for ~41%\nsmaller indexes\n- Coverity static analysis integration\n- AMPROP_DISTANCE_ORDERABLE implementation\n- Fixed 'too many LWLocks taken' error on partitioned tables\n\n## Testing\n\n- `make test` passes (37 tests)\n- `make format-check` passes",
          "timestamp": "2026-01-13T02:33:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/411527a28812edbf9a2ccb175811f449ce4278b0"
        },
        "date": 1768285316861,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19351.723,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "0b1fde1605f16cc34b195c41209f76e13c1ac303",
          "message": "Use binary search for WAND-style doc ID seeking in BMW\n\nThe multi-term BMW algorithm now uses binary search (via\ntp_segment_posting_iterator_seek) when skipping documents that can't\nbeat the threshold. Previously, iterators advanced one document at a\ntime.\n\nKey changes:\n- Add seek_term_to_doc() that uses binary search on skip entries when\n  the target is beyond the current block\n- Add find_next_candidate_doc_id() to find the minimum doc ID among\n  terms not at the current pivot\n- Modify skip_pivot_document() to seek to the next candidate instead\n  of advancing by 1\n\nThis changes the complexity from O(blocks) to O(log blocks) when\nskipping over documents with sparse term overlap.\n\nCloses #141",
          "timestamp": "2026-01-13T16:57:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0b1fde1605f16cc34b195c41209f76e13c1ac303"
        },
        "date": 1768324911791,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19688.691,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "fbff39af22ea556ca9cab93b45e437d49a35f9fd",
          "message": "Add GUC and stats for WAND seek optimization\n\nAdd pg_textsearch.enable_wand_seek GUC to control whether WAND-style\nbinary search seeking is used (default: on). This allows A/B\ncomparison to measure the optimization's impact.\n\nAdd seek statistics to TpBMWStats:\n- seeks_performed: number of binary search seeks\n- docs_seeked: total doc IDs skipped via seeking\n- linear_advances: single-doc advances (no seek benefit)\n- seek_to_same_doc: seeks that landed on same/next doc\n\nThese stats are logged when pg_textsearch.log_bmw_stats=on.\n\nLocal testing shows the current implementation adds overhead without\nbenefit for typical workloads. The find_next_candidate_doc_id call\non every skip appears to cost more than the seeks save.",
          "timestamp": "2026-01-13T17:35:23Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fbff39af22ea556ca9cab93b45e437d49a35f9fd"
        },
        "date": 1768326482555,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18889.607,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "7e7e75e7ae39399c61ec85606231226baab39232",
          "message": "Optimize WAND seek by maintaining sorted term array\n\nInspired by Tantivy's WAND implementation, maintain term iterators\nsorted by current doc ID. This makes find_pivot_doc_id O(1) and\nfind_next_candidate_doc_id O(1) typical case instead of O(N).\n\nKey changes:\n- Add term_current_doc_id() helper for consistent doc ID access\n- Add compare_terms_by_doc_id() and sort_terms_by_doc_id() for sorting\n- Sort terms after initialization in init_segment_term_states()\n- Re-sort after advancing terms in score_pivot_document() and\n  skip_pivot_document()\n- Simplify find_pivot_doc_id() to just return first term's doc ID\n- Optimize find_next_candidate_doc_id() to scan from start until\n  first term past pivot (O(1) when only one term at pivot)\n- Add early exit in compute_pivot_max_score() using sorted order\n\nLocal benchmarks show performance parity between seek ON/OFF modes,\nresolving the overhead issue from the previous O(N) iteration.",
          "timestamp": "2026-01-13T18:48:40Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7e7e75e7ae39399c61ec85606231226baab39232"
        },
        "date": 1768333202306,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19209.985,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "12e50c25163c3b715f1f558e6c3111ece9c5cd5a",
          "message": "Use insertion sort instead of qsort for term array\n\nProfiling showed qsort was taking 70% of query time due to its\noverhead on small arrays. Insertion sort is O(N) for nearly-sorted\narrays, which is our case since only 1-2 terms move after advancing.\n\nFor N=2-5 terms (typical), this is essentially free compared to qsort.",
          "timestamp": "2026-01-13T21:55:36Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/12e50c25163c3b715f1f558e6c3111ece9c5cd5a"
        },
        "date": 1768342459195,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 17268.505,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768355524049,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 17539.149,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "b35d8984a0c388e1462efbce055a55e7dcac2dd4",
          "message": "Pre-load skip entries for in-memory binary search\n\nInstead of doing I/O for each binary search iteration when seeking,\npre-load all block last_doc_ids during term state initialization.\nThis makes binary search O(log N) in-memory operations instead of\nO(log N) I/O operations.\n\nChanges:\n- Add block_last_doc_ids array to TpTermState\n- Cache last_doc_id alongside block_max_scores during init\n- Rewrite seek_term_to_doc to use cached data for binary search\n- Only do I/O when loading the target block\n\nThis should significantly improve performance for high-token queries\nwhere many seeks are performed across large posting lists.",
          "timestamp": "2026-01-14T00:20:03Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b35d8984a0c388e1462efbce055a55e7dcac2dd4"
        },
        "date": 1768356982461,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 17385.639,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "0db7c2097de05330cfbcd89a747e9b04fb9f9d81",
          "message": "Add pgspot CI workflow for SQL security checks (#145)\n\n## Summary\n- Add GitHub workflow to run pgspot on extension SQL files for security\nchecks\n- Fix PS002 warning by changing `CREATE OR REPLACE` to `CREATE` for\n`bm25_spill_index` function\n- Ignore PS017 warnings (false positives for type/operator definitions\nwithin the extension)\n\n## Testing\n- All 37 regression tests pass\n- pgspot runs clean on all SQL files with `--ignore PS017`",
          "timestamp": "2026-01-14T04:37:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0db7c2097de05330cfbcd89a747e9b04fb9f9d81"
        },
        "date": 1768371740610,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18839.747,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "c5a6ea09ab094dd95174f6f7e75d338a680a295d",
          "message": "Lazy CTID loading for segment queries\n\nDefer CTID resolution until final top-k results are known, reducing\nmemory usage from ~53MB to ~60 bytes per query (for k=10).\n\nChanges:\n- Add tp_segment_open_ex() with load_ctids parameter (default: false)\n- Add tp_segment_lookup_ctid() for targeted 6-byte reads\n- Split heap add into tp_topk_add_memtable/tp_topk_add_segment\n- Add tp_topk_resolve_ctids() to resolve CTIDs before extraction\n- Add doc_id field to TpSegmentPosting for deferred resolution\n- Update iterator to conditionally resolve CTIDs\n- Exhaustive path still loads CTIDs (needed for hash table keys)",
          "timestamp": "2026-01-14T06:48:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c5a6ea09ab094dd95174f6f7e75d338a680a295d"
        },
        "date": 1768374453104,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19526.9,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "94c66cc59f38a7e4e2d72832432f7825fb29fd3a",
          "message": "Fix formatting",
          "timestamp": "2026-01-14T18:43:28Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/94c66cc59f38a7e4e2d72832432f7825fb29fd3a"
        },
        "date": 1768416448588,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 17195.502,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "d32590214fa6b4fbf6a5f8643b6461926873fdf6",
          "message": "Bump version to v0.5.0-dev (#147)\n\n## Summary\n- Bump version from v0.4.0 to v0.5.0-dev\n- Update README to note parallel indexing as the main feature for v0.5.0\n- Add new SQL extension file and update test expected outputs\n\n## Testing\n- CI will verify the version string updates are consistent",
          "timestamp": "2026-01-15T04:27:11Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d32590214fa6b4fbf6a5f8643b6461926873fdf6"
        },
        "date": 1768458373914,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18723.331,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "75a05c5003b013a45ca4886ea055a175b0c1c351",
          "message": "Add LSAN suppressions for pg_config false positives\n\nThe nightly stress tests were failing because LSAN detected \"leaks\" in\nPostgreSQL's pg_config utility. These are false positives - pg_config\nis a short-lived command-line tool that allocates memory and exits\nwithout freeing it, which is normal and expected behavior.\n\nAdd suppressions for pg_strdup and get_configdata to fix the failures.",
          "timestamp": "2026-01-16T05:56:34Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/75a05c5003b013a45ca4886ea055a175b0c1c351"
        },
        "date": 1768544515351,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 19193.876,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "72239eee572aba5253e13aeb0bb5a086f81ace7e",
          "message": "Document Python 3.10 pin for wikiextractor (#151)\n\n## Summary\n\n- Add comment explaining why Python 3.10 is pinned in the\ncomparative-benchmark job\n- wikiextractor is unmaintained and breaks on Python 3.11+",
          "timestamp": "2026-01-17T02:20:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/72239eee572aba5253e13aeb0bb5a086f81ace7e"
        },
        "date": 1768630842972,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18322.973,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "72239eee572aba5253e13aeb0bb5a086f81ace7e",
          "message": "Document Python 3.10 pin for wikiextractor (#151)\n\n## Summary\n\n- Add comment explaining why Python 3.10 is pinned in the\ncomparative-benchmark job\n- wikiextractor is unmaintained and breaks on Python 3.11+",
          "timestamp": "2026-01-17T02:20:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/72239eee572aba5253e13aeb0bb5a086f81ace7e"
        },
        "date": 1768717312082,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18276.378,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "efe88321a9fabe17cd60a5adf155d804b1796161",
          "message": "Fix potential integer overflow in BMW stats tracking (#155)\n\n## Summary\n- Fix Coverity-reported integer overflow (CID 641544) in\n`skip_pivot_document()`\n- Rewrite `landed_doc <= pivot_doc_id + 1` as `landed_doc - pivot_doc_id\n<= 1` to avoid overflow when `pivot_doc_id` equals `UINT32_MAX`\n\n## Testing\n- All SQL regression tests pass",
          "timestamp": "2026-01-19T01:31:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/efe88321a9fabe17cd60a5adf155d804b1796161"
        },
        "date": 1768803801223,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18733.623,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "efe88321a9fabe17cd60a5adf155d804b1796161",
          "message": "Fix potential integer overflow in BMW stats tracking (#155)\n\n## Summary\n- Fix Coverity-reported integer overflow (CID 641544) in\n`skip_pivot_document()`\n- Rewrite `landed_doc <= pivot_doc_id + 1` as `landed_doc - pivot_doc_id\n<= 1` to avoid overflow when `pivot_doc_id` equals `UINT32_MAX`\n\n## Testing\n- All SQL regression tests pass",
          "timestamp": "2026-01-19T01:31:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/efe88321a9fabe17cd60a5adf155d804b1796161"
        },
        "date": 1768890198827,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 20570.837,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "c088e94febb15a9b4901fc522dac675f8f9f89fb",
          "message": "Fix nightly stress test failures (#157)\n\n## Summary\n- Fix PostgreSQL cache to include install directory (was only caching\nbuild dir)\n- Add LSAN suppression for `save_ps_display_args` false positive\n- Add `issues: write` permission for failure notification job\n\nThe nightly stress tests have been failing since Jan 9 due to the cache\nissue causing `initdb` to fail with \"postgres binary not found\".\n\n## Testing\n- Trigger manual run: `gh workflow run nightly-stress.yml --field\nduration_minutes=5`",
          "timestamp": "2026-01-20T16:01:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c088e94febb15a9b4901fc522dac675f8f9f89fb"
        },
        "date": 1768976512654,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 18379.192,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 36.92,
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
          "id": "d66f0d8bdeb63aca2c43247243494af89820c199",
          "message": "fix: use FSM for page reclamation and dynamic pool extension\n\nThree fixes for parallel build page management:\n\n1. After parallel index build completes, unused pre-allocated pool pages\n   were being truncated. However, the subsequent compaction step would\n   then extend the relation again, negating the truncation. Now we record\n   unused pool pages in the Free Space Map (FSM) so they can be reused\n   by compaction.\n\n2. When the pre-allocated page pool is exhausted during build (in\n   tp_pool_get_page), instead of failing with an error, we now\n   dynamically extend the relation. A NOTICE is logged on first overflow\n   so users can tune the expansion factor if desired.\n\n3. When writing page index and the pool is exhausted (in\n   write_page_index_from_pool), we now also dynamically extend instead\n   of failing.\n\nThese changes fix the index size regression observed in nightly\nbenchmarks and make parallel builds more robust.",
          "timestamp": "2026-01-22T18:48:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d66f0d8bdeb63aca2c43247243494af89820c199"
        },
        "date": 1769113055349,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8023.394,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.28,
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
          "id": "f985027719b0a0e7a1f5c8ff08bd836200937220",
          "message": "Fix parallel build page pool exhaustion in benchmarks\n\n- Fix error message to reference correct GUC name\n  (pg_textsearch.parallel_build_expansion_factor, not TP_INDEX_EXPANSION_FACTOR)\n- Set parallel_build_expansion_factor = 2.0 in benchmark workflow config\n  to prevent pool exhaustion on larger datasets like Wikipedia 100K",
          "timestamp": "2026-01-25T02:26:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f985027719b0a0e7a1f5c8ff08bd836200937220"
        },
        "date": 1769308399568,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7987.151,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.91,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.28,
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
          "id": "3244d05a86658620ecb2d82253ef7916604f839d",
          "message": "Fix zero scores when querying hypertables with BM25 index (#168)\n\n## Summary\n\nThis PR fixes zero scores when querying hypertables with BM25 indexes.\n\n**Commit 1: Planner hook fix for CustomScan**\n- Add `T_CustomScan` handling in `plan_has_bm25_indexscan()` to detect\nBM25 index scans nested inside custom scans (e.g., TimescaleDB's\nConstraintAwareAppend)\n- Add `T_CustomScan` handling in `replace_scores_in_plan()` to replace\nscore expressions in custom scan children\n\n**Commit 2: Standalone scoring fix for hypertable parent indexes**\n- When using standalone BM25 scoring with a hypertable parent index\nname, the code was falling back to child index stats but NOT switching\nto the child's index relation and segment metadata\n- This caused IDF calculation to fail because it was looking up document\nfrequencies in the parent index's segments (which are empty)\n- The fix switches to the child index for segment access when falling\nback to a child index's state\n\n## Testing\n\n- Verified fix with reproduction case from bug report\n- Added Test 5 in partitioned.sql for MergeAppend score expression\nreplacement\n- Added test/scripts/hypertable.sh for optional TimescaleDB integration\ntesting (runs only if TimescaleDB is installed)\n\n```sql\n-- Both queries now return proper BM25 scores:\n\n-- Query through parent hypertable with ORDER BY\nSELECT content, -(content <@> to_bm25query('database', 'hyper_idx')) as score\nFROM hyper_docs\nORDER BY content <@> to_bm25query('database', 'hyper_idx')\nLIMIT 5;\n\n-- Standalone scoring with parent index name\nSELECT content, (content <@> to_bm25query('database', 'hyper_idx')) as score\nFROM hyper_docs\nWHERE content LIKE '%database%';\n```",
          "timestamp": "2026-01-25T16:35:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3244d05a86658620ecb2d82253ef7916604f839d"
        },
        "date": 1769408380596,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8135.84,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.51,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.6,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.95,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.28,
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
          "id": "70747956a7287acc6da5743f831c4cfe651e8d22",
          "message": "chore: clean up legacy am/extension name and remove unused test script (#177)\n\nIn src/am/build.c, there is still code that refers to the legacy access\nmethod and extension name, tapir. This commit updates those references\nto the current names.\n\nscripts/generate_test_case.py is no longer used (replaced by\nvalidation.sql) and thus gets removed.",
          "timestamp": "2026-01-27T02:08:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/70747956a7287acc6da5743f831c4cfe651e8d22"
        },
        "date": 1769494674396,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7931.523,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.52,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.6,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.97,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.2,
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
          "id": "70747956a7287acc6da5743f831c4cfe651e8d22",
          "message": "chore: clean up legacy am/extension name and remove unused test script (#177)\n\nIn src/am/build.c, there is still code that refers to the legacy access\nmethod and extension name, tapir. This commit updates those references\nto the current names.\n\nscripts/generate_test_case.py is no longer used (replaced by\nvalidation.sql) and thus gets removed.",
          "timestamp": "2026-01-27T02:08:35Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/70747956a7287acc6da5743f831c4cfe651e8d22"
        },
        "date": 1769581197728,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8180.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.58,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.93,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.29,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769668010449,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8197.201,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.5,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.58,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.95,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.27,
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
          "id": "276f64750340502e0fbce0f35f23c3cc4a8b93c8",
          "message": "fix: handle page boundaries when writing dictionary entries in parallel build\n\nThe dictionary entry writing code in tp_merge_worker_buffers_to_segment()\nassumed all entries fit on the first page. With large datasets (150K+ docs),\ndictionary entries span multiple pages, causing \"Invalid logical page\" errors.\n\nFixed by:\n1. Flushing segment writer before writing entries\n2. Using page-boundary-aware code that calculates logical/physical pages\n3. Handling entries that span two pages like merge.c does",
          "timestamp": "2026-01-29T15:34:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/276f64750340502e0fbce0f35f23c3cc4a8b93c8"
        },
        "date": 1769710991894,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12341.775,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.1,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.17,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.75,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 66.17,
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
          "id": "ef7051aeffdeb9076de500185859baeb4a1549ba",
          "message": "fix: revert to nworkers > 0 for parallel build, update expected output\n\n1 worker still helps with read/write parallelism (worker scans while\nleader writes). Reverted the nworkers > 1 check back to nworkers > 0.\n\nUpdated test expected output after confirming tests pass with both\ndebug and release builds.",
          "timestamp": "2026-01-29T23:52:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ef7051aeffdeb9076de500185859baeb4a1549ba"
        },
        "date": 1769731128338,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13541.186,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.71,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.34,
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
          "id": "a9cf8f4b5aa880608a7dfbc736429b2693a6028e",
          "message": "chore: add max_parallel_maintenance_workers to Cranfield benchmark\n\nAlso add comments explaining the maintenance_work_mem requirement\nfor parallel builds (32MB per worker + leader).\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-30T01:59:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a9cf8f4b5aa880608a7dfbc736429b2693a6028e"
        },
        "date": 1769739679181,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14612.389,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.34,
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
          "id": "a9cf8f4b5aa880608a7dfbc736429b2693a6028e",
          "message": "chore: add max_parallel_maintenance_workers to Cranfield benchmark\n\nAlso add comments explaining the maintenance_work_mem requirement\nfor parallel builds (32MB per worker + leader).\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-01-30T01:59:09Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a9cf8f4b5aa880608a7dfbc736429b2693a6028e"
        },
        "date": 1769741838596,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14275.522,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.1,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.34,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769754454313,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7880.197,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.55,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.89,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.28,
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
          "id": "80800439c2112008d6c4b573d5bb2de00e2efcb7",
          "message": "Add bm25_debug_pageviz() for index page layout visualization (#186)\n\n## Summary\n\nAdds `bm25_debug_pageviz(index_name, filepath)` function to visualize\nindex page layout with ANSI colors. This is useful for debugging index\nsize issues, particularly the page fragmentation in parallel builds.\n\n**Features:**\n- Background colors distinguish different segments (16-color palette)\n- Letter mnemonics for page types: `H`=header, `d`=dictionary, `s`=skip,\n`m`=docmap, `i`=pageindex\n- Blank space with colored background for posting pages (most common\ntype, reduces clutter)\n- Special colors for metapage (white) and recovery pages (blue)\n- Line breaks every 128 characters\n- Legend showing segments by level with color sample, page count, and\nsize\n- Summary with utilization percentage\n\n**Example output header:**\n```\n# Page Visualization: msmarco_bm25_idx\n# Total: 162053 pages (1266.0 MB), 7 segments\n#\n# Segments (background color indicates segment):\n#   L0: S0 (16249 pg, 126.9MB)  S1 (23129 pg, 180.7MB) ...\n#\n# Page counts: empty=5817 header=7 dict=23719 post=109783 ...\n```\n\n**Use case:** Comparing serial vs parallel builds on MS-MARCO 8M:\n- Serial: 162K pages, 5,817 empty (3.6% waste)  \n- Parallel: 512K pages, 360,587 empty (70% waste!)\n\n## Testing\n\n```sql\nSELECT bm25_debug_pageviz('my_index', '/tmp/viz.txt');\n-- View with: less -R /tmp/viz.txt\n```",
          "timestamp": "2026-01-29T01:54:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/80800439c2112008d6c4b573d5bb2de00e2efcb7"
        },
        "date": 1769840633438,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8245.456,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.9,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 72.28,
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
          "id": "60604649c2cef4b638c318a545bbbe6f72801d93",
          "message": "ci: use standard runners for benchmarks (#193)\n\n## Summary\n- Reverts benchmark workflow from `ubuntu-latest-16-cores` to standard\n`ubuntu-latest` runners\n- Adjusts PostgreSQL memory settings to fit within ~7GB RAM limit\n- Fixes jobs that were queueing indefinitely due to unavailable 16-core\nrunners\n\n## Changes\n| Setting | Before | After |\n|---------|--------|-------|\n| Runner | ubuntu-latest-16-cores | ubuntu-latest |\n| shared_buffers | 4GB | 1GB |\n| effective_cache_size | 12GB | 4GB |\n| maintenance_work_mem | 1GB | 512MB |\n| work_mem | 128MB | 64MB |\n| wal_buffers | 64MB | 16MB |\n\nThe 512MB maintenance_work_mem is still well above the 64MB minimum\nrequired for parallel index builds.\n\n## Testing\nManually triggered workflow succeeded and results look reasonable.",
          "timestamp": "2026-02-01T01:48:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/60604649c2cef4b638c318a545bbbe6f72801d93"
        },
        "date": 1769927258976,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 10816.481,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.09,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.17,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.79,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.34,
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
          "id": "60604649c2cef4b638c318a545bbbe6f72801d93",
          "message": "ci: use standard runners for benchmarks (#193)\n\n## Summary\n- Reverts benchmark workflow from `ubuntu-latest-16-cores` to standard\n`ubuntu-latest` runners\n- Adjusts PostgreSQL memory settings to fit within ~7GB RAM limit\n- Fixes jobs that were queueing indefinitely due to unavailable 16-core\nrunners\n\n## Changes\n| Setting | Before | After |\n|---------|--------|-------|\n| Runner | ubuntu-latest-16-cores | ubuntu-latest |\n| shared_buffers | 4GB | 1GB |\n| effective_cache_size | 12GB | 4GB |\n| maintenance_work_mem | 1GB | 512MB |\n| work_mem | 128MB | 64MB |\n| wal_buffers | 64MB | 16MB |\n\nThe 512MB maintenance_work_mem is still well above the 64MB minimum\nrequired for parallel index builds.\n\n## Testing\nManually triggered workflow succeeded and results look reasonable.",
          "timestamp": "2026-02-01T01:48:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/60604649c2cef4b638c318a545bbbe6f72801d93"
        },
        "date": 1770014186814,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12055.176,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.17,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
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
          "id": "1a23bc6bbb06dcec0c7e6efa50b2a51c08882926",
          "message": "Release v0.5.0 (#197)\n\n## Summary\n- Update version from 0.5.0-dev to 0.5.0 across all files\n- Rename SQL files and banner image to remove -dev suffix\n- Update README: this is expected to be the last pre-release before GA\n(v1.0.0)\n- Update test expected outputs for minor ordering/warning differences\n\n## Testing\n- `make test` passes (40/40 tests)\n- `make format-check` passes",
          "timestamp": "2026-02-02T19:09:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1a23bc6bbb06dcec0c7e6efa50b2a51c08882926"
        },
        "date": 1770100117172,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12486.307,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.19,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
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
          "id": "1a23bc6bbb06dcec0c7e6efa50b2a51c08882926",
          "message": "Release v0.5.0 (#197)\n\n## Summary\n- Update version from 0.5.0-dev to 0.5.0 across all files\n- Rename SQL files and banner image to remove -dev suffix\n- Update README: this is expected to be the last pre-release before GA\n(v1.0.0)\n- Update test expected outputs for minor ordering/warning differences\n\n## Testing\n- `make test` passes (40/40 tests)\n- `make format-check` passes",
          "timestamp": "2026-02-02T19:09:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/1a23bc6bbb06dcec0c7e6efa50b2a51c08882926"
        },
        "date": 1770186464151,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11794.723,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
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
          "id": "b0eea09b625cbbca5db98be033582c06e04f9cd7",
          "message": "refactor: remove unused tp_limits_init() (#190)\n\nGUC parameter 'pg_textsearch.default_limit' is initialized in\n_PG_init(), this tp_limits_init() is not used, so remove it.",
          "timestamp": "2026-02-05T05:09:06Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b0eea09b625cbbca5db98be033582c06e04f9cd7"
        },
        "date": 1770273096488,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12195.966,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
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
          "id": "03969c526b74a012e38d5f140b008bf15459ed37",
          "message": "fix: implement CREATE INDEX CONCURRENTLY support (#200)\n\n## Summary\n\n- Implement proper `ambulkdelete` callback to report all indexed CTIDs\nduring CIC validation\n- Add SQL test verifying CIC works correctly without duplicate entries\n- Add shell script tests for concurrent table operations during CIC\n\n## Background\n\nCREATE INDEX CONCURRENTLY requires the `ambulkdelete` callback to report\nall existing TIDs so `validate_index` can determine which rows need to\nbe inserted. Without this, all rows would be re-inserted causing\nduplicates.\n\nThe fix adds two helper functions in `vacuum.c`:\n- `tp_bulkdelete_memtable_ctids`: iterates through document lengths\ndshash to collect memtable CTIDs\n- `tp_bulkdelete_segment_ctids`: iterates through segment document maps\nto collect segment CTIDs\n\n## Testing\n\n- SQL test `test/sql/concurrent_build.sql`:\n  - Basic CIC index creation\n  - Verification that index is marked valid\n  - Verification that no duplicate entries are produced\n  - DROP INDEX CONCURRENTLY\n  - Regular CREATE INDEX for comparison\n\n- Shell test `test/scripts/cic.sh`:\n  - Concurrent INSERTs during CIC (100k documents)\n  - Concurrent UPDATEs during CIC\n  - Concurrent DELETEs during CIC\n  - Mixed concurrent operations during CIC\n  - Multiple concurrent CIC operations\n- Configured with lower spill thresholds to trigger multiple memtable\nspills",
          "timestamp": "2026-02-05T23:57:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/03969c526b74a012e38d5f140b008bf15459ed37"
        },
        "date": 1770359321942,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12692.346,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.72,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
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
          "id": "07b8656acdb9099d4496a7d4f5a9a3657f28a0aa",
          "message": "data: regenerate MS-MARCO ground truth with exact avgdl\n\nRegenerated ground truth using total_len/total_docs for avg_doc_len\ninstead of the rounded display value. Results now match Tapir to\nfloat precision:\n\n- Before: 1.2% match at 4 decimal places, max diff 0.00066\n- After: 99.4% match at 4 decimal places, max diff 0.0000022\n\nThe remaining 0.6% are at rounding boundaries with diff < 3e-6.\n\nCo-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>",
          "timestamp": "2026-02-06T17:55:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/07b8656acdb9099d4496a7d4f5a9a3657f28a0aa"
        },
        "date": 1770401653926,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11956.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.72,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
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
          "id": "753eed5ee0018203154314a9e6a6a6ebc1bd1ac7",
          "message": "docs: update benchmark comparison page with Feb 6 results (#211)\n\n## Summary\n\n- Update source copy of `benchmarks/gh-pages/comparison.html` with the\nFeb 6 benchmark data\n- The benchmark workflow copies this file onto gh-pages on every run, so\nthe source copy must stay in sync with any manual edits made directly on\ngh-pages\n\nThis was overwritten when a benchmark run deployed the stale source\nversion on top of a manual gh-pages edit.",
          "timestamp": "2026-02-06T21:06:05Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/753eed5ee0018203154314a9e6a6a6ebc1bd1ac7"
        },
        "date": 1770445409447,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14487.79,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.1,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 36.4,
            "unit": "MB"
          }
        ]
      }
    ]
  }
}