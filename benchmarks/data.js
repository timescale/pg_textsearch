window.BENCHMARK_DATA = {
  "lastUpdate": 1772522819613,
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
        "date": 1770618908936,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 256.053,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.36,
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
          "id": "74d725b6bdf5e4edae71f20b305ad05133819dca",
          "message": "feat: implement WAND pivot selection for multi-term queries (#210)\n\n## Summary\n\n- Replace min-doc-id pivot with classic WAND pivot selection for\nmulti-term BMW queries\n- Terms sorted by current doc_id; walk accumulating max_scores until\nthreshold exceeded, skipping large doc_id ranges that can't contribute\nto top-k\n- Block-max refinement with Tantivy-style skip advancement (advance\nhighest-value scorer past block boundary)\n- Linear insertion maintains sorted order — O(1) typical for low term\ncounts, preserving existing performance\n\n## Motivation\n\n8+ term queries (MS-MARCO p50) are 18% slower than System X. The old\napproach found the minimum doc_id and checked all terms, evaluating many\ndocuments that couldn't beat the threshold. Classic WAND skips these\nentirely.\n\n## Changes\n\n- `src/query/bmw.c`: Add `max_score` to TpTermState, sort\ninfrastructure, `find_wand_pivot`, block-max refinement, rewritten main\nloop\n- `test/sql/wand.sql`: 8-term validation test (BMW vs exhaustive + BM25\nreference)\n\n## Testing\n\n- All regression tests pass\n- All shell tests pass (concurrency, recovery, CIC)\n- BM25 scores verified identical to reference implementation\n- MS-MARCO benchmark indicates query improvement across the board\n\nAddresses #192.",
          "timestamp": "2026-02-09T23:41:48Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/74d725b6bdf5e4edae71f20b305ad05133819dca"
        },
        "date": 1770681444670,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 250.276,
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
          "id": "23831b5decf5e26f0d5813e985bc5f2d36b178c3",
          "message": "docs: update benchmark comparison with Feb 9 results (#217)\n\n## Summary\n- Update comparison page with results after WAND pivot selection merge\n(PR #210)\n- pg_textsearch now faster than System X across all 8 token buckets\n(previously lost on 8+ tokens)\n- Overall throughput improved from 1.8x to 2.8x faster\n- System X version updated from 0.20.6 to 0.21.6\n\n## Key changes in numbers\n- 8+ token p50: 49.03 ms → 27.95 ms (43% faster than previous, now beats\nSystem X's 41.23 ms)\n- 7 token p50: 30.06 ms → 18.02 ms (40% faster)\n- Throughput: 16.65 ms/q → 10.48 ms/q (vs System X 29.18 ms/q)\n\n## Testing\n- Numbers extracted from benchmark run\n[#21845385796](https://github.com/timescale/pg_textsearch/actions/runs/21845385796)\n- gh-pages branch still needs a matching update after this merges",
          "timestamp": "2026-02-10T03:01:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/23831b5decf5e26f0d5813e985bc5f2d36b178c3"
        },
        "date": 1770705756769,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.637,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.45,
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
          "id": "db94e895c547b5558cc2989336f09ea0ad130343",
          "message": "ci: run coverage on PRs and enable branch coverage (#221)\n\n## Summary\n- Run the coverage workflow on `pull_request` events (same path filters\nas push) so Codecov can post diff coverage comments on PRs\n- Enable lcov branch coverage (`--rc branch_coverage=1`) in CI and\nMakefile targets\n\n## Testing\n- This PR itself will trigger the coverage workflow, confirming it works\non PRs\n- Branch coverage data will appear in the lcov summary and HTML report",
          "timestamp": "2026-02-11T03:49:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db94e895c547b5558cc2989336f09ea0ad130343"
        },
        "date": 1770791663318,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 248.205,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.58,
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
          "id": "db94e895c547b5558cc2989336f09ea0ad130343",
          "message": "ci: run coverage on PRs and enable branch coverage (#221)\n\n## Summary\n- Run the coverage workflow on `pull_request` events (same path filters\nas push) so Codecov can post diff coverage comments on PRs\n- Enable lcov branch coverage (`--rc branch_coverage=1`) in CI and\nMakefile targets\n\n## Testing\n- This PR itself will trigger the coverage workflow, confirming it works\non PRs\n- Branch coverage data will appear in the lcov summary and HTML report",
          "timestamp": "2026-02-11T03:49:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db94e895c547b5558cc2989336f09ea0ad130343"
        },
        "date": 1770878054278,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 248.054,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1770964339847,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 261.416,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.41,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771050257628,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 288.164,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.44,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771136889204,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 254.299,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.38,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771223618298,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 246.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.33,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771310491944,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.874,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (800 queries, avg ms/query)",
            "value": 2.35,
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
          "id": "957c07c15894f2711b1b9645a439bbb8f8a72fa3",
          "message": "chore: remove DEBUG1 elog from parallel build DSA limit",
          "timestamp": "2026-02-17T19:50:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/957c07c15894f2711b1b9645a439bbb8f8a72fa3"
        },
        "date": 1771358490918,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 263.263,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.34,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "0f9564cb42f873ad58264c58e6b10aedeabacb6c",
          "message": "bench: add MS MARCO v2 dataset and weighted-average latency metric (#226)\n\n## Summary\n\n- Add benchmark infrastructure for the **MS MARCO v2** passage ranking\ndataset (138M passages) with download, load, query, and System X\ncomparison scripts\n- Introduce **weighted-average query latency** as a new tracked metric\nacross all benchmark datasets (MS MARCO v1, v2, Wikipedia)\n- Per-bucket p50 latencies are weighted by the observed MS-MARCO v1\nlexeme distribution (1M queries: 3-token mode at 30%, mean 3.7 tokens)\nto produce a single summary number that reflects realistic workload\nperformance\n\n### Files changed\n\n**New (MS MARCO v2 benchmark infrastructure):**\n- `benchmarks/datasets/msmarco-v2/` — download, load, query scripts +\nbenchmark queries TSV\n- `benchmarks/datasets/msmarco-v2/systemx/` — System X (ParadeDB)\ncomparison scripts\n\n**Modified (weighted-average metric):**\n- `benchmarks/datasets/msmarco/queries.sql` — add weighted-average\ncomputation\n- `benchmarks/datasets/wikipedia/queries.sql` — add weighted-average\ncomputation\n- `benchmarks/runner/extract_metrics.sh` — extract\n`WEIGHTED_LATENCY_RESULT` from logs\n- `benchmarks/runner/format_for_action.sh` — report weighted latency in\ngithub-action-benchmark format\n- `benchmarks/runner/run_benchmark.sh` — add msmarco-v2 as a dataset\noption\n- `benchmarks/gh-pages/methodology.html` — document weighted methodology\n+ MS MARCO v2 dataset\n\n### Distribution weights (MS-MARCO v1, 1,010,905 queries)\n\n| Tokens | Count | Weight |\n|--------|-------|--------|\n| 1 | 35,638 | 3.5% |\n| 2 | 165,033 | 16.3% |\n| 3 | 304,887 | 30.2% |\n| 4 | 264,177 | 26.1% |\n| 5 | 143,765 | 14.2% |\n| 6 | 59,558 | 5.9% |\n| 7 | 22,595 | 2.2% |\n| 8+ | 15,252 | 1.5% |",
          "timestamp": "2026-02-17T18:51:22Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0f9564cb42f873ad58264c58e6b10aedeabacb6c"
        },
        "date": 1771358657717,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 266.413,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.43,
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
          "id": "ef25897405c9392a9c24da1345530a861639cedd",
          "message": "fix: remove DSA size limit from parallel build\n\nThe limit was too restrictive: workers ignore maintenance_work_mem\nand each buffer can hold up to tp_memtable_spill_threshold postings\n(~384 MB), so with 4 workers the 1 GB floor was easily exceeded.\nProper memory bounding requires reworking the spill threshold to\nrespect maintenance_work_mem, which is a larger follow-up.",
          "timestamp": "2026-02-17T20:24:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ef25897405c9392a9c24da1345530a861639cedd"
        },
        "date": 1771360728833,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 255.692,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "b37eaabf08782a40680bdb28ea7549f97608a229",
          "message": "fix: use min fieldnorm for BMW skip entries in parallel build (#230)\n\n## Summary\n- Fix `write_posting_blocks()` in parallel build to compute MIN\nfieldnorm (shortest doc) instead of MAX (longest doc) per block\n- The `block_max_norm` skip entry field must store the minimum fieldnorm\nso BMW computes valid score upper bounds; using maximum caused BMW to\nincorrectly skip blocks containing high-scoring short documents\n- The serial build (`segment.c`) and merge (`merge.c`) paths already\nused `min_norm` correctly — this aligns the parallel build path\n- Add `parallel_bmw` regression test that deterministically reproduces\nthe bug: medium-length docs set the BMW threshold in early blocks, then\nmixed short+long doc blocks follow where the wrong upper bound causes\nBMW to skip them\n\n## Test plan\n- [x] `parallel_bmw` test fails deterministically without fix (0 short\ndocs in top-10), passes with fix (10 short docs)\n- [x] Verified 5/5 stable passes for `bmw` + `parallel_build` tests\n- [x] Full regression suite passes (only pre-existing `binary_io`\nfailure)\n- [x] `make format-check` passes",
          "timestamp": "2026-02-18T01:49:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b37eaabf08782a40680bdb28ea7549f97608a229"
        },
        "date": 1771397047478,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 259.799,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "206692f75f86c089bd1b804a742ea6af69f6337a",
          "message": "fix: prevent buffer overflow in batch merge when merge_source_init fails\n\nThe segment-walking loop used num_sources (successful inits) as its\ntermination condition, but when merge_source_init fails, num_sources\ndoesn't increment while the loop continues walking the chain. This\ncould walk past segment_count segments, overflowing the segment_pages\narray and corrupting the level count in the metapage.\n\nFix: track segments_walked separately and cap on that. Update\nsegment_count to reflect actual segments consumed so the metapage\nremainder calculation is correct.\n\nAlso zero out num_docs for sources with missing CTID arrays to prevent\na potential NULL dereference in the N-way merge.",
          "timestamp": "2026-02-18T19:34:26Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/206692f75f86c089bd1b804a742ea6af69f6337a"
        },
        "date": 1771443612540,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 237.582,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.28,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "7bd2c53318e928db5e7a64ce4fa4db9c3ea77d7a",
          "message": "bench: disable parallel build in MS MARCO benchmark for deterministic segments\n\nThe benchmark should produce a deterministic segment layout regardless\nof the machine's parallelism settings, so query performance results\nare comparable across runs and branches.",
          "timestamp": "2026-02-18T19:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bd2c53318e928db5e7a64ce4fa4db9c3ea77d7a"
        },
        "date": 1771445671877,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 211.68,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "3588dc0de18d8811d44c599edc15e614cff94108",
          "message": "feat: add bm25_compact_index() and always compact after parallel build\n\nAdd tp_compact_all() which merges ALL segments at each level in a\nsingle batch, ignoring the segments_per_level threshold. This is\nused in two places:\n\n1. After parallel index build, to produce a fully compacted index\n   instead of leaving N loose L0 segments from N workers.\n\n2. Exposed as SQL function bm25_compact_index(index_name) so\n   benchmarks (or users) can force full compaction on demand.\n\nThe tp_merge_level_segments() function now accepts a max_merge\nparameter so callers can control batch size independently of the\nsegments_per_level GUC.",
          "timestamp": "2026-02-18T21:29:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3588dc0de18d8811d44c599edc15e614cff94108"
        },
        "date": 1771451067632,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 269.996,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.36,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "6ad4ffb8f501035ede8687cafa098f8b04f38008",
          "message": "fix: detoast bm25vector in send and equality functions\n\ntpvector_send and tpvector_eq used PG_GETARG_POINTER which does not\ndetoast varlena datums. When PostgreSQL stores small bm25vectors with\n1-byte short varlena headers, VARSIZE() reads garbage (interpreting the\n1B header + data bytes as a 4B header), causing out-of-bounds reads.\n\nUnder ASAN sanitizer builds this crashes immediately when the garbage\nsize causes reads past the buffer page boundary into poisoned memory.\nWithout sanitizer, the bug is latent but produces corrupt binary output.\n\nFix by using PG_DETOAST_DATUM which ensures proper 4-byte varlena\nheaders. This matches what tpvector_out already does correctly.",
          "timestamp": "2026-02-19T01:28:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/6ad4ffb8f501035ede8687cafa098f8b04f38008"
        },
        "date": 1771466551242,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 242.204,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.33,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "8fe9814a5cd20fdf2600b248a21c2ec36d8eb624",
          "message": "feat: add worker-side compaction to parallel index build\n\nWorkers now perform level-aware compaction within their BufFile during\nparallel index build, mirroring tp_maybe_compact_level from serial\nbuild. This produces a proper LSM level structure instead of dumping\nall segments at L0.\n\n- Extract merge types and helpers into merge_internal.h for reuse\n- Add merge_source_init_from_reader() for BufFile-backed segments\n- Workers track segments per level and compact when threshold reached\n- write_merged_segment_to_buffile() performs N-way merge within BufFile\n- Leader writes compacted segments at correct levels, then runs final\n  compaction on combined per-level counts",
          "timestamp": "2026-02-19T03:45:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8fe9814a5cd20fdf2600b248a21c2ec36d8eb624"
        },
        "date": 1771474282581,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 240.906,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.43,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "b37eaabf08782a40680bdb28ea7549f97608a229",
          "message": "fix: use min fieldnorm for BMW skip entries in parallel build (#230)\n\n## Summary\n- Fix `write_posting_blocks()` in parallel build to compute MIN\nfieldnorm (shortest doc) instead of MAX (longest doc) per block\n- The `block_max_norm` skip entry field must store the minimum fieldnorm\nso BMW computes valid score upper bounds; using maximum caused BMW to\nincorrectly skip blocks containing high-scoring short documents\n- The serial build (`segment.c`) and merge (`merge.c`) paths already\nused `min_norm` correctly — this aligns the parallel build path\n- Add `parallel_bmw` regression test that deterministically reproduces\nthe bug: medium-length docs set the BMW threshold in early blocks, then\nmixed short+long doc blocks follow where the wrong upper bound causes\nBMW to skip them\n\n## Test plan\n- [x] `parallel_bmw` test fails deterministically without fix (0 short\ndocs in top-10), passes with fix (10 short docs)\n- [x] Verified 5/5 stable passes for `bmw` + `parallel_build` tests\n- [x] Full regression suite passes (only pre-existing `binary_io`\nfailure)\n- [x] `make format-check` passes",
          "timestamp": "2026-02-18T01:49:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b37eaabf08782a40680bdb28ea7549f97608a229"
        },
        "date": 1771482774407,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 298.295,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "4639bf8baedd19b412fc720d7639d33927d8e726",
          "message": "feat: workers write index pages directly in two-phase parallel build\n\nEliminate single-threaded leader transcription bottleneck by having\nworkers write their segments directly to pre-allocated index pages.\n\nPhase 1: Workers scan heap, flush/compact in BufFile, report total\npages needed, signal phase1_done.\n\nLeader barrier: Sum page counts, pre-extend relation with batched\nExtendBufferedRelBy (8192 pages/batch), set atomic next_page counter,\nsignal phase2_ready.\n\nPhase 2: Workers reopen BufFile read-only, write segments to index\npages using lock-free atomic page counter, report seg_roots[].\n\nLeader finalization: Read seg_roots[] from shared memory (no BufFile\nI/O), chain segments into level lists, update metapage, compact.",
          "timestamp": "2026-02-19T19:23:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4639bf8baedd19b412fc720d7639d33927d8e726"
        },
        "date": 1771531978440,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 234.996,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771568965901,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 286.793,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.24,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "2b60e589d50a7c1f08ef44604dedb8d1821af874",
          "message": "fix: fix BufFile cursor handling in worker-side compaction\n\nThree interrelated bugs in worker-side BufFile compaction caused\ncorruption when worker BufFiles exceeded 1GB (the physical segment\nsize for Postgres BufFile):\n\n1. BufFileTell ordering: write_merged_segment_to_buffile() called\n   BufFileTell AFTER build_merged_docmap(), but that function reads\n   from BufFile-backed segment readers which moves the file cursor.\n   Fix: record the base position BEFORE build_merged_docmap and\n   re-seek after.\n\n2. Interleaved reads and writes: merge source reads (via\n   posting_source_advance -> tp_segment_read) share the same BufFile\n   handle as the output writes, moving the cursor between writes.\n   Fix: introduce buffile_write_at() that seeks to the absolute\n   output position before every write.\n\n3. SEEK_END race: BufFileSeek(SEEK_END) calls FileSize() before\n   flushing the dirty write buffer, returning the on-disk size which\n   is less than the logical size. Writing there overwrites unflushed\n   data. Fix: track buffile_end explicitly in WorkerSegmentTracker\n   and use it instead of SEEK_END.\n\nAlso decomposes BufFile composite offsets (fileno, offset) before all\nBufFileSeek calls to handle multi-segment BufFiles correctly.\n\nVerified with 138M row MS MARCO v2 parallel index build (8 workers).",
          "timestamp": "2026-02-20T08:29:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2b60e589d50a7c1f08ef44604dedb8d1821af874"
        },
        "date": 1771605119581,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 266.72,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "0541b9a41a685d05a817c764e9a3f7a373b1f001",
          "message": "fix: import worker segments as L0 and truncate unused pool pages\n\nWorker-side compaction produces segments at various levels (L1, L2, etc.)\nbut these levels are meaningless globally since each worker only sees 1/Nth\nof the data. Importing them at their worker-local levels inflates segment\ncounts (e.g. 7 L1 + 4 L0 = 11 segments vs ~4 L1 for serial build).\n\nImport all worker segments as L0 so leader-side compaction can cascade\nproperly through L0→L1→L2, matching serial build behavior.\n\nAlso truncate pre-allocated pool pages that workers never used, reducing\nindex size bloat from over-estimation.",
          "timestamp": "2026-02-20T22:38:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0541b9a41a685d05a817c764e9a3f7a373b1f001"
        },
        "date": 1771635971735,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 254.229,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.24,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "4723f0e5956dbae9483260c61abf3c7accd119ca",
          "message": "feat: unified merge sink eliminates parallel build fragmentation\n\nReplace the two-phase parallel build (workers write to BufFile then\ncopy to pre-allocated pages; leader compacts via FSM/P_NEW) with a\nsingle-phase design: workers scan+flush+compact to BufFile and exit;\nleader reopens all worker BufFiles, performs one N-way merge, and\nwrites a single contiguous segment via TpMergeSink.\n\nKey changes:\n- Add TpMergeSink abstraction (pages or BufFile backend) with\n  merge_sink_write() and merge_sink_write_at() for backpatching\n- Unify write_merged_segment() and write_merged_segment_to_buffile()\n  into write_merged_segment_to_sink() (~430 lines of duplication removed)\n- Remove Phase 2 from workers (no more phase2_cv, page pool,\n  atomic counter, pool truncation, segment linking)\n- Leader does BufFile-through merge directly to index pages,\n  producing one contiguous L1 segment with zero fragmentation\n\nNet: -509 lines. Index size should now match serial build.",
          "timestamp": "2026-02-21T03:50:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4723f0e5956dbae9483260c61abf3c7accd119ca"
        },
        "date": 1771646866939,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 218.299,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.26,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771654972847,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 271.775,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.31,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771741567131,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 247.226,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.3,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "b15d8ad5c1b22366f4e019d4bec1f61937e46cc4",
          "message": "fix: restore parallel page writes, drop leader compaction\n\nThe previous commit moved ALL page writing to the leader via a\nsingle-threaded N-way merge, which nearly doubled build time\n(4:29 → 8:26 on CI benchmark). The fragmentation was never from\nworkers writing to pre-allocated pages — it was from the leader's\ntp_maybe_compact_level() using FSM/P_NEW which scattered pages.\n\nFix: restore two-phase parallel build where workers write BufFile\nsegments to pre-allocated contiguous pages in parallel, but skip\nthe compaction step entirely. Worker segments stay as L0 in the\ncontiguous pool and compact naturally on subsequent inserts or\nvia bm25_compact_index().\n\nKeep TpMergeSink abstraction in merge.c for worker BufFile\ncompaction and normal compaction paths.",
          "timestamp": "2026-02-23T01:48:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b15d8ad5c1b22366f4e019d4bec1f61937e46cc4"
        },
        "date": 1771812136178,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 235.324,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.25,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771828451000,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 262.293,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.37,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771915157864,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 254.089,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.35,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "9ad4271373e91371e28c2afa0e1077972af65291",
          "message": "feat: disjoint TID range scans + sequential drain merge\n\nReplace shared parallel heap scan with per-worker TID range scans.\nEach worker scans a contiguous, non-overlapping range of heap blocks,\nensuring disjoint CTIDs across workers.\n\nThis enables a sequential drain fast path in cross-worker merge:\ninstead of N-way CTID comparison per posting, drain sources in order\n(source 0 fully, then source 1, etc.). This eliminates:\n- posting_source_convert_current (2 pread calls per posting)\n- find_min_posting_source (CTID comparison per posting)\n\nThese two functions accounted for ~47% of CPU time in Phase 2 of\nthe 138M-row MS-MARCO v2 parallel build.\n\nChanges:\n- Leader divides heap blocks evenly across launched workers, stores\n  ranges in shared memory, signals scan_ready atomic flag\n- Workers spin-wait on scan_ready, then open table_beginscan_tidrange\n  limited to their assigned block range\n- build_merged_docmap: concatenates sources sequentially when disjoint\n  instead of N-way CTID merge\n- write_merged_segment_to_sink: new disjoint_sources parameter enables\n  sequential drain using posting_source_advance_fast (no CTID lookup)\n- Extract FLUSH_BLOCK macro to deduplicate block write logic\n- Remove parallel table scan descriptor (no longer needed)",
          "timestamp": "2026-02-24T22:03:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9ad4271373e91371e28c2afa0e1077972af65291"
        },
        "date": 1771971517830,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 233.06,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.26,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "afdca64b8adf628c526b18433f8b07315d180ce8",
          "message": "fix: register snapshot in worker for PG18 tidrange scan\n\nPG18 added an assertion in heap_prepare_pagescan requiring any snapshot\nused in a heap scan to be registered or active. The leader already\nregistered the snapshot, but the worker passed an unregistered snapshot\nfrom GetTransactionSnapshot() to table_beginscan_tidrange(), tripping\nthe assertion under sanitizer builds.\n\nMirror the leader's pattern: RegisterSnapshot before scan,\nUnregisterSnapshot after table_endscan.",
          "timestamp": "2026-02-24T22:33:25Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/afdca64b8adf628c526b18433f8b07315d180ce8"
        },
        "date": 1771975251673,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 249.496,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "a1cfff5b14cf371eae6301cab89d1e871e6fb662",
          "message": "Revert versioned shared library filename (#238)\n\n## Summary\n\n- Reverts #232 which added version numbers to the shared library\nfilename\n- The versioned binary naming scheme (e.g.,\n`pg_textsearch-1.0.0-dev.so`) caused problems in practice\n- Returns to the standard unversioned `pg_textsearch.so` naming\n\n## Testing\n\n- `make installcheck` passes (all SQL regression + shell tests)\n- `make format-check` passes",
          "timestamp": "2026-02-24T23:36:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a1cfff5b14cf371eae6301cab89d1e871e6fb662"
        },
        "date": 1772001584834,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 273.548,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.31,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "e0e10f33e0630e639749510d0003333d41bbfecf",
          "message": "fix: merge remainder segments during parallel build to eliminate fragmentation\n\nThe parallel build's plan_merge_groups() only formed merge groups of\nexactly segments_per_level (8) segments. With typical 2-4 workers\nproducing 1 segment each, all segments became COPY-only at L0, leaving\nmultiple uncompacted segments. When bm25_compact_index() later merged\nthem, the new segment was written via P_NEW while freed pages sat in\nthe middle of the file — dead space that truncation couldn't reclaim.\nThis caused a 2x index size regression vs main.\n\nFix:\n- Merge leftover segments (count >= 2) at each level during\n  plan_merge_groups, producing a fully compacted index so\n  bm25_compact_index() becomes a no-op\n- Move truncation after link_chains and walk actual segment pages\n  via tp_segment_collect_pages() instead of using the atomic page\n  counter (which over-estimates due to 5% merge margin)\n- Add post-compaction truncation to bm25_compact_index() for cases\n  where compaction does real work (non-parallel builds, etc.)",
          "timestamp": "2026-02-26T01:37:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e0e10f33e0630e639749510d0003333d41bbfecf"
        },
        "date": 1772070498003,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 238.655,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.36,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "44deea684c705c68e2832796e146ccf1d8b3784b",
          "message": "fix: prevent cascading merge group segfault + dedup build helpers\n\nThree changes:\n\n1. Fix segfault in parallel build: remainder merge groups no longer\n   cascade to higher levels. worker_execute_merge_group() only\n   searches worker result arrays for source segments, not outputs\n   of earlier merge groups. When cascading groups were planned,\n   they found no sources and accessed invalid block numbers,\n   causing SIGSEGV on large tables (8.8M+ rows, 4+ workers).\n\n2. Extract tp_truncate_dead_pages() shared helper from duplicated\n   code in build.c (bm25_compact_index) and build_parallel.c\n   (post-build truncation). Walks all segment chains to find the\n   highest used block and truncates everything beyond it.\n\n3. Extract tp_link_l0_chain_head() helper from 4x duplicated\n   pattern in build.c (auto-spill, flush-and-link, spill-memtable,\n   final-spill). Also removes dead final memtable spill block\n   from serial build path (arena build never populates memtable).",
          "timestamp": "2026-02-26T01:59:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/44deea684c705c68e2832796e146ccf1d8b3784b"
        },
        "date": 1772071953411,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.017,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.24,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "3dc4a8fc8f6a0c63fa0b1cee190ca80207d0e27f",
          "message": "fix: compact remaining segments after parallel build for query perf\n\nWithout cascading merge groups, the parallel build can leave\na few segments at each level (e.g., 3 L1 segments from 2 full\nmerge groups + 1 remainder). This hurts query performance since\nBMW must merge across all segments.\n\nAdd tp_compact_all() after linking chains to merge remaining\nsegments into one, followed by tp_truncate_dead_pages() to\nreclaim any dead pages left by the compaction.",
          "timestamp": "2026-02-26T02:14:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3dc4a8fc8f6a0c63fa0b1cee190ca80207d0e27f"
        },
        "date": 1772072883662,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 238.737,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.23,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "04f4387e2ca42e382e29105d101b8b6909cab8fe",
          "message": "fix: tp_compact_all must check all levels, not just bottom-up\n\ntp_compact_all() used break when it found a level with < 2\nsegments, assuming higher levels would also be sparse. This\nassumption is wrong after parallel build where merge groups\noutput directly at L1+, leaving L0 empty. Change break to\ncontinue so all levels are checked.\n\nThis was preventing post-build compaction from merging the\n2 L1 segments (from full + remainder merge groups) into a\nsingle L2 segment, hurting query performance.",
          "timestamp": "2026-02-26T02:31:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/04f4387e2ca42e382e29105d101b8b6909cab8fe"
        },
        "date": 1772073954785,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 219.253,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.16,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "44deea684c705c68e2832796e146ccf1d8b3784b",
          "message": "fix: prevent cascading merge group segfault + dedup build helpers\n\nThree changes:\n\n1. Fix segfault in parallel build: remainder merge groups no longer\n   cascade to higher levels. worker_execute_merge_group() only\n   searches worker result arrays for source segments, not outputs\n   of earlier merge groups. When cascading groups were planned,\n   they found no sources and accessed invalid block numbers,\n   causing SIGSEGV on large tables (8.8M+ rows, 4+ workers).\n\n2. Extract tp_truncate_dead_pages() shared helper from duplicated\n   code in build.c (bm25_compact_index) and build_parallel.c\n   (post-build truncation). Walks all segment chains to find the\n   highest used block and truncates everything beyond it.\n\n3. Extract tp_link_l0_chain_head() helper from 4x duplicated\n   pattern in build.c (auto-spill, flush-and-link, spill-memtable,\n   final-spill). Also removes dead final memtable spill block\n   from serial build path (arena build never populates memtable).",
          "timestamp": "2026-02-26T01:59:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/44deea684c705c68e2832796e146ccf1d8b3784b"
        },
        "date": 1772075832960,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 251.476,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.33,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "4a9aaf2a430249d4b20d0c4045b4ddf6c5a8bebb",
          "message": "feat: require shared_preload_libraries for pg_textsearch (#235)\n\n## Summary\n\n- Enforce loading via `shared_preload_libraries` to prevent fatal\n`ShmemIndex entry size is wrong` errors when the .so is replaced during\ndeployment without a server restart\n- Remove lazy init fallbacks in registry.c that were the previous\nworkaround for not requiring preloading\n- Update all test infrastructure (Makefile, shell scripts, CI workflows)\nto configure `shared_preload_libraries` with the versioned library name\n\n## Context\n\npg_textsearch uses Postgres shared memory (`ShmemInitStruct`) for its\nindex registry. Without `shared_preload_libraries`, each backend loads\nthe .so independently via `dlopen`. If the .so file is replaced on disk\n(e.g., during a deployment) without restarting Postgres, different\nbackends get different code, causing a fatal `ShmemIndex entry size is\nwrong` error. Requiring `shared_preload_libraries` ensures all backends\nuse the same .so (inherited from the postmaster via fork), eliminating\nthis class of bugs.\n\n## Testing\n\n- All 47 SQL regression tests pass\n- `make format-check` passes\n- Error message verified when extension not in shared_preload_libraries",
          "timestamp": "2026-02-25T23:21:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4a9aaf2a430249d4b20d0c4045b4ddf6c5a8bebb"
        },
        "date": 1772087737834,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 270.731,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.41,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "f1384f93b557bdea96e1de131c56195afb2508f3",
          "message": "feat: detect stale binary after upgrade via library version check (#241)\n\n## Summary\n\n- Add `pg_textsearch.library_version` read-only GUC set at library load\ntime from a compile-time version define\n- Check the GUC in `CREATE EXTENSION` and `ALTER EXTENSION UPDATE` SQL\nscripts, failing with a clear error if the loaded library is missing or\ndoesn't match the expected version\n- Catches the case where a user runs `make install` but forgets to\nrestart the server\n\n## Changes\n\n- **Makefile**: Extract `EXTVERSION` from `pg_textsearch.control`, pass\nas `-DPG_TEXTSEARCH_VERSION`\n- **src/mod.c**: Register `PGC_INTERNAL` string GUC in `_PG_init`\n- **sql/pg_textsearch--1.0.0-dev.sql**: Version check DO block before\nany CREATE statements\n- **sql/pg_textsearch--0.5.0--1.0.0-dev.sql**: Same version check in\nupgrade script\n- **test/sql/basic.sql**: Regression test for the GUC\n\n## Testing\n\n- `SHOW pg_textsearch.library_version` returns `1.0.0-dev` after restart\n- All 47 regression tests pass",
          "timestamp": "2026-02-26T19:19:22Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f1384f93b557bdea96e1de131c56195afb2508f3"
        },
        "date": 1772173769603,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 295.629,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.39,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772243895100,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 246.408,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.35,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "86ec2fd148ae2d2bc22a6be526183bb45476ea92",
          "message": "feat: rewrite index build with arena allocator and parallel page pool (#231)\n\n## Summary\n\n- **Arena allocator + EXPULL posting lists**: Replace DSA/dshash shared\nmemory with process-local arena (1MB slab bump allocator, 32-bit\naddresses) and exponential unrolled linked lists. Eliminates DSA\nfragmentation, dshash lock overhead, and posting list reallocation\ncopies.\n- **TpBuildContext**: New per-build context with arena, HTAB term\ndictionary, and flat doc arrays. Budget-controlled flushing using\n`maintenance_work_mem`. Used by both serial and parallel CREATE INDEX.\n- **Two-phase parallel build**: Workers scan disjoint TID ranges, flush\nto BufFile temp segments, compact within BufFile. Leader plans\ncross-worker merge groups. Workers then COPY non-merged segments to\npre-allocated index pages and work-steal merge groups via atomic\ncounter.\n- **Unified merge sink**: `TpMergeSink` abstraction writes merged\nsegments to either index pages or BufFile, eliminating separate code\npaths. Disjoint source fast path enables sequential drain merge without\nCTID comparison.\n- **bm25_force_merge()**: New SQL function to force-merge all segments\ninto one (à la Lucene's `forceMerge(1)`).\n- **TOAST fix**: `tpvector_send()` and `tpvector_eq()` now properly\ndetoast values.\n\n## Motivation\n\nThe previous parallel build used DSA shared memory with dshash for term\ndictionaries and double-buffered memtable swapping. This caused:\n- DSA fragmentation under large workloads (100GB+ OOM on MS MARCO v2)\n- dshash lock contention between workers\n- Complex N-way merge in the leader\n- ~2200 lines of intricate coordination code\n\nThe new approach is simpler and more memory-efficient: each worker gets\na local arena with a budget, flushes segments to BufFile temp files, and\nthe leader coordinates a two-phase write to pre-allocated index pages.\nCross-worker compaction merges segments that span worker boundaries.\n\n## Benchmark results (MS MARCO, 8.8M passages)\n\n| Metric | main | System X | This PR | vs main |\n|--------|------|----------|---------|---------|\n| Build Time | 278.5s | 146.2s | 228.1s | **19% faster** |\n| Index Size | 1240 MB | 1497 MB | 1360 MB | +10% (9% smaller than\nSystem X) |\n| Query P50 | 3.74ms | — | 6.08ms | +63% |\n| Query Avg | 4.49ms | — | 7.55ms | +68% |\n\n**Note on query latency**: The parallel build produces 2-3 L1 segments\ninstead of a single fully-compacted segment (as on main). The BMW query\noptimizer must scan multiple segments, which increases latency. A\nfollow-up will add post-build `bm25_force_merge()` that merges into\npre-allocated pages (using the existing BufFile→index-page COPY path) to\neliminate this regression.\n\n## Key changes\n\n### New files\n- `src/memtable/arena.c/h` — 1MB page-based bump allocator with 32-bit\naddresses (12-bit page + 20-bit offset), max 4GB\n- `src/memtable/expull.c/h` — Exponential unrolled linked list for\nposting lists (blocks grow 32B→32KB, 7-byte packed entries)\n- `src/am/build_context.c/h` — Arena-based build context replacing DSA\nmemtable during builds\n- `src/segment/merge_internal.h` — Exposes merge internals for\nbuild_parallel.c\n\n### Major modifications\n- `src/am/build.c` — Serial build uses TpBuildContext; added\n`bm25_force_merge()` SQL function; extracted `tp_link_l0_chain_head()`\nand `tp_truncate_dead_pages()` shared helpers\n- `src/am/build_parallel.c` — Two-phase parallel build with disjoint TID\nranges, BufFile worker segments, cross-worker merge groups, and atomic\npage pool\n- `src/segment/merge.c` — Unified TpMergeSink abstraction; disjoint\nsource fast path; streaming docmap merge\n- `src/segment/segment.c` — Added parallel writer init and atomic page\nallocation\n\n### Bug fixes\n- `src/types/vector.c` — Use `PG_DETOAST_DATUM` for TOAST safety in\nsend/eq functions\n- `src/am/scan.c` — Removed `TP_MAX_BLOCK_NUMBER` guard (large\nparallel-built indexes can exceed 1M blocks)\n- `sql/pg_textsearch--0.5.0--1.0.0-dev.sql` — Added `bm25_force_merge`\nto upgrade path\n\n## Test plan\n\n- [x] All 47 regression tests pass (PG17 + PG18)\n- [x] `parallel_build` test validates serial, 1-worker, 2-worker,\n4-worker builds with query correctness\n- [x] `parallel_bmw` test validates Block-Max WAND with parallel-built\nindexes\n- [x] `make format-check` passes\n- [x] CI green: gcc/clang × PG17/PG18, sanitizers\n- [x] MS MARCO benchmark passes (8.8M passages, build + query\nvalidation)\n- [x] Rebased onto current main (shared_preload_libraries, TOCTOU fix,\nvalidation)",
          "timestamp": "2026-02-27T21:46:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/86ec2fd148ae2d2bc22a6be526183bb45476ea92"
        },
        "date": 1772259435143,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 239.141,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.29,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "86ec2fd148ae2d2bc22a6be526183bb45476ea92",
          "message": "feat: rewrite index build with arena allocator and parallel page pool (#231)\n\n## Summary\n\n- **Arena allocator + EXPULL posting lists**: Replace DSA/dshash shared\nmemory with process-local arena (1MB slab bump allocator, 32-bit\naddresses) and exponential unrolled linked lists. Eliminates DSA\nfragmentation, dshash lock overhead, and posting list reallocation\ncopies.\n- **TpBuildContext**: New per-build context with arena, HTAB term\ndictionary, and flat doc arrays. Budget-controlled flushing using\n`maintenance_work_mem`. Used by both serial and parallel CREATE INDEX.\n- **Two-phase parallel build**: Workers scan disjoint TID ranges, flush\nto BufFile temp segments, compact within BufFile. Leader plans\ncross-worker merge groups. Workers then COPY non-merged segments to\npre-allocated index pages and work-steal merge groups via atomic\ncounter.\n- **Unified merge sink**: `TpMergeSink` abstraction writes merged\nsegments to either index pages or BufFile, eliminating separate code\npaths. Disjoint source fast path enables sequential drain merge without\nCTID comparison.\n- **bm25_force_merge()**: New SQL function to force-merge all segments\ninto one (à la Lucene's `forceMerge(1)`).\n- **TOAST fix**: `tpvector_send()` and `tpvector_eq()` now properly\ndetoast values.\n\n## Motivation\n\nThe previous parallel build used DSA shared memory with dshash for term\ndictionaries and double-buffered memtable swapping. This caused:\n- DSA fragmentation under large workloads (100GB+ OOM on MS MARCO v2)\n- dshash lock contention between workers\n- Complex N-way merge in the leader\n- ~2200 lines of intricate coordination code\n\nThe new approach is simpler and more memory-efficient: each worker gets\na local arena with a budget, flushes segments to BufFile temp files, and\nthe leader coordinates a two-phase write to pre-allocated index pages.\nCross-worker compaction merges segments that span worker boundaries.\n\n## Benchmark results (MS MARCO, 8.8M passages)\n\n| Metric | main | System X | This PR | vs main |\n|--------|------|----------|---------|---------|\n| Build Time | 278.5s | 146.2s | 228.1s | **19% faster** |\n| Index Size | 1240 MB | 1497 MB | 1360 MB | +10% (9% smaller than\nSystem X) |\n| Query P50 | 3.74ms | — | 6.08ms | +63% |\n| Query Avg | 4.49ms | — | 7.55ms | +68% |\n\n**Note on query latency**: The parallel build produces 2-3 L1 segments\ninstead of a single fully-compacted segment (as on main). The BMW query\noptimizer must scan multiple segments, which increases latency. A\nfollow-up will add post-build `bm25_force_merge()` that merges into\npre-allocated pages (using the existing BufFile→index-page COPY path) to\neliminate this regression.\n\n## Key changes\n\n### New files\n- `src/memtable/arena.c/h` — 1MB page-based bump allocator with 32-bit\naddresses (12-bit page + 20-bit offset), max 4GB\n- `src/memtable/expull.c/h` — Exponential unrolled linked list for\nposting lists (blocks grow 32B→32KB, 7-byte packed entries)\n- `src/am/build_context.c/h` — Arena-based build context replacing DSA\nmemtable during builds\n- `src/segment/merge_internal.h` — Exposes merge internals for\nbuild_parallel.c\n\n### Major modifications\n- `src/am/build.c` — Serial build uses TpBuildContext; added\n`bm25_force_merge()` SQL function; extracted `tp_link_l0_chain_head()`\nand `tp_truncate_dead_pages()` shared helpers\n- `src/am/build_parallel.c` — Two-phase parallel build with disjoint TID\nranges, BufFile worker segments, cross-worker merge groups, and atomic\npage pool\n- `src/segment/merge.c` — Unified TpMergeSink abstraction; disjoint\nsource fast path; streaming docmap merge\n- `src/segment/segment.c` — Added parallel writer init and atomic page\nallocation\n\n### Bug fixes\n- `src/types/vector.c` — Use `PG_DETOAST_DATUM` for TOAST safety in\nsend/eq functions\n- `src/am/scan.c` — Removed `TP_MAX_BLOCK_NUMBER` guard (large\nparallel-built indexes can exceed 1M blocks)\n- `sql/pg_textsearch--0.5.0--1.0.0-dev.sql` — Added `bm25_force_merge`\nto upgrade path\n\n## Test plan\n\n- [x] All 47 regression tests pass (PG17 + PG18)\n- [x] `parallel_build` test validates serial, 1-worker, 2-worker,\n4-worker builds with query correctness\n- [x] `parallel_bmw` test validates Block-Max WAND with parallel-built\nindexes\n- [x] `make format-check` passes\n- [x] CI green: gcc/clang × PG17/PG18, sanitizers\n- [x] MS MARCO benchmark passes (8.8M passages, build + query\nvalidation)\n- [x] Rebased onto current main (shared_preload_libraries, TOCTOU fix,\nvalidation)",
          "timestamp": "2026-02-27T21:46:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/86ec2fd148ae2d2bc22a6be526183bb45476ea92"
        },
        "date": 1772346296319,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 253.403,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.36,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "93170a2d07de18d890efa12b0c79489e0e6669dc",
          "message": "feat: leader-only merge for parallel index build\n\nReplace the two-phase parallel build (worker compaction + work-stealing\nmerge groups) with a simpler architecture: workers flush L0 segments to\nBufFiles without compaction, then the leader performs a single N-way\nmerge of all segments directly to paged storage.\n\nThis produces a single segment per index build, which is optimal for\nquery performance (no multi-segment scanning needed).\n\nRemoves ~800 lines of complexity: worker_maybe_compact_level,\nplan_merge_groups, compute_total_pages_needed,\nworker_execute_merge_group, write_temp_segment_to_index_parallel,\nand all Phase 2 worker code (COPY + work-steal).",
          "timestamp": "2026-03-01T17:46:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/93170a2d07de18d890efa12b0c79489e0e6669dc"
        },
        "date": 1772388119498,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 236.78,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.14,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "a72f481e17e0c684b7bd37be3da4067674ddec35",
          "message": "fix: address code review issues in leader-only merge\n\n- Move FlushRelationBuffers before metapage update for consistency\n  with merge.c pattern (segment data durable before metadata)\n- Use merge_source_close() instead of manual pfree for cleanup\n- NULL readers[] slot when source takes ownership to prevent\n  double-close\n- Remove dead segment_count field from TpParallelWorkerResult",
          "timestamp": "2026-03-01T18:44:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a72f481e17e0c684b7bd37be3da4067674ddec35"
        },
        "date": 1772394615059,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 278.348,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.33,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "e8601e2c78ac4f757e3255744f2a83360ed6b20b",
          "message": "fix: add coverage gate to block PRs on coverage reduction (#245)\n\n## Summary\n- The coverage job collected data and uploaded to Codecov but never\nenforced\n  thresholds — it always succeeded regardless of coverage values\n- Codecov status checks (`codecov/project`, `codecov/patch`) weren't\nappearing\n  on PRs, likely due to a missing/invalid `CODECOV_TOKEN`, and\n  `fail_ci_if_error` was `false` so the upload failure was silent\n- Branch protection had an empty required checks list\n(`all-tests-passed` was\n  not listed), so CI failures couldn't block merges — now fixed\n\n### Changes\n- Add in-workflow coverage gate to both `ci.yml` and `coverage.yml`\nthat:\n  - Enforces a minimum 85% line coverage threshold\n  - Caches the main branch baseline via `actions/cache`\n- Compares PR coverage against baseline, fails if it drops by more than\n1%\n- Change `fail_ci_if_error` from `false` to `true` on Codecov upload\n- Added `all-tests-passed` to branch protection required status checks\n\n### How the baseline works\n- On pushes to `main`, the coverage percentage is saved to GitHub\nActions\n  cache with key `coverage-baseline-<sha>`\n- On PRs, the most recent main branch cache is restored and compared\nagainst\n  the PR's coverage\n- First run after this merges will skip the reduction check (no baseline\nyet)\n  and establish the initial baseline\n\n## Testing\n- CI on this PR validates the gate runs (no baseline exists yet, so the\nreduction check was skipped, but the minimum threshold check ran and\npassed)\n- After merging, the next PR that reduces coverage by >1% will fail",
          "timestamp": "2026-03-02T04:13:08Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e8601e2c78ac4f757e3255744f2a83360ed6b20b"
        },
        "date": 1772432871598,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 263.985,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.32,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772519934164,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield (1.3K docs) - Index Build Time",
            "value": 240.995,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Throughput (avg ms/query)",
            "value": 2.3,
            "unit": "ms"
          },
          {
            "name": "cranfield (1.3K docs) - Index Size",
            "value": 0.68,
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
        "date": 1770532138825,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 271829.394,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 23.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 36.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 12.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1190.16,
            "unit": "MB"
          }
        ]
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
        "date": 1770618911148,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 274855.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.27,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 13.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 18.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 30.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 16.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.49,
            "unit": "MB"
          }
        ]
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
          "id": "74d725b6bdf5e4edae71f20b305ad05133819dca",
          "message": "feat: implement WAND pivot selection for multi-term queries (#210)\n\n## Summary\n\n- Replace min-doc-id pivot with classic WAND pivot selection for\nmulti-term BMW queries\n- Terms sorted by current doc_id; walk accumulating max_scores until\nthreshold exceeded, skipping large doc_id ranges that can't contribute\nto top-k\n- Block-max refinement with Tantivy-style skip advancement (advance\nhighest-value scorer past block boundary)\n- Linear insertion maintains sorted order — O(1) typical for low term\ncounts, preserving existing performance\n\n## Motivation\n\n8+ term queries (MS-MARCO p50) are 18% slower than System X. The old\napproach found the minimum doc_id and checked all terms, evaluating many\ndocuments that couldn't beat the threshold. Classic WAND skips these\nentirely.\n\n## Changes\n\n- `src/query/bmw.c`: Add `max_score` to TpTermState, sort\ninfrastructure, `find_wand_pivot`, block-max refinement, rewritten main\nloop\n- `test/sql/wand.sql`: 8-term validation test (BMW vs exhaustive + BM25\nreference)\n\n## Testing\n\n- All regression tests pass\n- All shell tests pass (concurrency, recovery, CIC)\n- BM25 scores verified identical to reference implementation\n- MS-MARCO benchmark indicates query improvement across the board\n\nAddresses #192.",
          "timestamp": "2026-02-09T23:41:48Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/74d725b6bdf5e4edae71f20b305ad05133819dca"
        },
        "date": 1770681446811,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 269496.382,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.54,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 8.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 12.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 27.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 10.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1189.49,
            "unit": "MB"
          }
        ]
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
          "id": "23831b5decf5e26f0d5813e985bc5f2d36b178c3",
          "message": "docs: update benchmark comparison with Feb 9 results (#217)\n\n## Summary\n- Update comparison page with results after WAND pivot selection merge\n(PR #210)\n- pg_textsearch now faster than System X across all 8 token buckets\n(previously lost on 8+ tokens)\n- Overall throughput improved from 1.8x to 2.8x faster\n- System X version updated from 0.20.6 to 0.21.6\n\n## Key changes in numbers\n- 8+ token p50: 49.03 ms → 27.95 ms (43% faster than previous, now beats\nSystem X's 41.23 ms)\n- 7 token p50: 30.06 ms → 18.02 ms (40% faster)\n- Throughput: 16.65 ms/q → 10.48 ms/q (vs System X 29.18 ms/q)\n\n## Testing\n- Numbers extracted from benchmark run\n[#21845385796](https://github.com/timescale/pg_textsearch/actions/runs/21845385796)\n- gh-pages branch still needs a matching update after this merges",
          "timestamp": "2026-02-10T03:01:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/23831b5decf5e26f0d5813e985bc5f2d36b178c3"
        },
        "date": 1770705758937,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 291815.756,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 8.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 12.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 28.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 10.63,
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
          "id": "db94e895c547b5558cc2989336f09ea0ad130343",
          "message": "ci: run coverage on PRs and enable branch coverage (#221)\n\n## Summary\n- Run the coverage workflow on `pull_request` events (same path filters\nas push) so Codecov can post diff coverage comments on PRs\n- Enable lcov branch coverage (`--rc branch_coverage=1`) in CI and\nMakefile targets\n\n## Testing\n- This PR itself will trigger the coverage workflow, confirming it works\non PRs\n- Branch coverage data will appear in the lcov summary and HTML report",
          "timestamp": "2026-02-11T03:49:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db94e895c547b5558cc2989336f09ea0ad130343"
        },
        "date": 1770791665052,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 276022.095,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 3.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 4.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 6.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 9.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 14.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 5.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1202.54,
            "unit": "MB"
          }
        ]
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
          "id": "db94e895c547b5558cc2989336f09ea0ad130343",
          "message": "ci: run coverage on PRs and enable branch coverage (#221)\n\n## Summary\n- Run the coverage workflow on `pull_request` events (same path filters\nas push) so Codecov can post diff coverage comments on PRs\n- Enable lcov branch coverage (`--rc branch_coverage=1`) in CI and\nMakefile targets\n\n## Testing\n- This PR itself will trigger the coverage workflow, confirming it works\non PRs\n- Branch coverage data will appear in the lcov summary and HTML report",
          "timestamp": "2026-02-11T03:49:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db94e895c547b5558cc2989336f09ea0ad130343"
        },
        "date": 1770878056291,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 255102.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 4.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 7.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 10.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 14.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 22.29,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 8.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 921.15,
            "unit": "MB"
          }
        ]
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1770964343157,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 281182.535,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 12.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 28.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 10.39,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771050260439,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 278091.841,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 28.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 10.34,
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
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771136891238,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 270483.419,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.63,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 3.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 5.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 6.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 9.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 14.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 5.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1204.11,
            "unit": "MB"
          }
        ]
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771223621042,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 277235.128,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 8.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 12.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 28.26,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 10.58,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771310494205,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 277617.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 8.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 27.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Throughput (800 queries, avg ms/query)",
            "value": 10.42,
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
          "id": "0f9564cb42f873ad58264c58e6b10aedeabacb6c",
          "message": "bench: add MS MARCO v2 dataset and weighted-average latency metric (#226)\n\n## Summary\n\n- Add benchmark infrastructure for the **MS MARCO v2** passage ranking\ndataset (138M passages) with download, load, query, and System X\ncomparison scripts\n- Introduce **weighted-average query latency** as a new tracked metric\nacross all benchmark datasets (MS MARCO v1, v2, Wikipedia)\n- Per-bucket p50 latencies are weighted by the observed MS-MARCO v1\nlexeme distribution (1M queries: 3-token mode at 30%, mean 3.7 tokens)\nto produce a single summary number that reflects realistic workload\nperformance\n\n### Files changed\n\n**New (MS MARCO v2 benchmark infrastructure):**\n- `benchmarks/datasets/msmarco-v2/` — download, load, query scripts +\nbenchmark queries TSV\n- `benchmarks/datasets/msmarco-v2/systemx/` — System X (ParadeDB)\ncomparison scripts\n\n**Modified (weighted-average metric):**\n- `benchmarks/datasets/msmarco/queries.sql` — add weighted-average\ncomputation\n- `benchmarks/datasets/wikipedia/queries.sql` — add weighted-average\ncomputation\n- `benchmarks/runner/extract_metrics.sh` — extract\n`WEIGHTED_LATENCY_RESULT` from logs\n- `benchmarks/runner/format_for_action.sh` — report weighted latency in\ngithub-action-benchmark format\n- `benchmarks/runner/run_benchmark.sh` — add msmarco-v2 as a dataset\noption\n- `benchmarks/gh-pages/methodology.html` — document weighted methodology\n+ MS MARCO v2 dataset\n\n### Distribution weights (MS-MARCO v1, 1,010,905 queries)\n\n| Tokens | Count | Weight |\n|--------|-------|--------|\n| 1 | 35,638 | 3.5% |\n| 2 | 165,033 | 16.3% |\n| 3 | 304,887 | 30.2% |\n| 4 | 264,177 | 26.1% |\n| 5 | 143,765 | 14.2% |\n| 6 | 59,558 | 5.9% |\n| 7 | 22,595 | 2.2% |\n| 8+ | 15,252 | 1.5% |",
          "timestamp": "2026-02-17T18:51:22Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0f9564cb42f873ad58264c58e6b10aedeabacb6c"
        },
        "date": 1771358660009,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 277623.426,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.2,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 28.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.44,
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
          "id": "ef25897405c9392a9c24da1345530a861639cedd",
          "message": "fix: remove DSA size limit from parallel build\n\nThe limit was too restrictive: workers ignore maintenance_work_mem\nand each buffer can hold up to tp_memtable_spill_threshold postings\n(~384 MB), so with 4 workers the 1 GB floor was easily exceeded.\nProper memory bounding requires reworking the spill threshold to\nrespect maintenance_work_mem, which is a larger follow-up.",
          "timestamp": "2026-02-17T20:24:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ef25897405c9392a9c24da1345530a861639cedd"
        },
        "date": 1771360730667,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 283496.798,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 12.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 18.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 28.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "b37eaabf08782a40680bdb28ea7549f97608a229",
          "message": "fix: use min fieldnorm for BMW skip entries in parallel build (#230)\n\n## Summary\n- Fix `write_posting_blocks()` in parallel build to compute MIN\nfieldnorm (shortest doc) instead of MAX (longest doc) per block\n- The `block_max_norm` skip entry field must store the minimum fieldnorm\nso BMW computes valid score upper bounds; using maximum caused BMW to\nincorrectly skip blocks containing high-scoring short documents\n- The serial build (`segment.c`) and merge (`merge.c`) paths already\nused `min_norm` correctly — this aligns the parallel build path\n- Add `parallel_bmw` regression test that deterministically reproduces\nthe bug: medium-length docs set the BMW threshold in early blocks, then\nmixed short+long doc blocks follow where the wrong upper bound causes\nBMW to skip them\n\n## Test plan\n- [x] `parallel_bmw` test fails deterministically without fix (0 short\ndocs in top-10), passes with fix (10 short docs)\n- [x] Verified 5/5 stable passes for `bmw` + `parallel_build` tests\n- [x] Full regression suite passes (only pre-existing `binary_io`\nfailure)\n- [x] `make format-check` passes",
          "timestamp": "2026-02-18T01:49:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b37eaabf08782a40680bdb28ea7549f97608a229"
        },
        "date": 1771397049986,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 293134.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 6.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 8.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 12.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 17.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 5.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1241.96,
            "unit": "MB"
          }
        ]
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
          "id": "206692f75f86c089bd1b804a742ea6af69f6337a",
          "message": "fix: prevent buffer overflow in batch merge when merge_source_init fails\n\nThe segment-walking loop used num_sources (successful inits) as its\ntermination condition, but when merge_source_init fails, num_sources\ndoesn't increment while the loop continues walking the chain. This\ncould walk past segment_count segments, overflowing the segment_pages\narray and corrupting the level count in the metapage.\n\nFix: track segments_walked separately and cap on that. Update\nsegment_count to reflect actual segments consumed so the metapage\nremainder calculation is correct.\n\nAlso zero out num_docs for sources with missing CTID arrays to prevent\na potential NULL dereference in the N-way merge.",
          "timestamp": "2026-02-18T19:34:26Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/206692f75f86c089bd1b804a742ea6af69f6337a"
        },
        "date": 1771443614922,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 278020.441,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.53,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 4202.16,
            "unit": "MB"
          }
        ]
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
          "id": "7bd2c53318e928db5e7a64ce4fa4db9c3ea77d7a",
          "message": "bench: disable parallel build in MS MARCO benchmark for deterministic segments\n\nThe benchmark should produce a deterministic segment layout regardless\nof the machine's parallelism settings, so query performance results\nare comparable across runs and branches.",
          "timestamp": "2026-02-18T19:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bd2c53318e928db5e7a64ce4fa4db9c3ea77d7a"
        },
        "date": 1771445674807,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 406714.833,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1246.83,
            "unit": "MB"
          }
        ]
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
          "id": "3588dc0de18d8811d44c599edc15e614cff94108",
          "message": "feat: add bm25_compact_index() and always compact after parallel build\n\nAdd tp_compact_all() which merges ALL segments at each level in a\nsingle batch, ignoring the segments_per_level threshold. This is\nused in two places:\n\n1. After parallel index build, to produce a fully compacted index\n   instead of leaving N loose L0 segments from N workers.\n\n2. Exposed as SQL function bm25_compact_index(index_name) so\n   benchmarks (or users) can force full compaction on demand.\n\nThe tp_merge_level_segments() function now accepts a max_merge\nparameter so callers can control batch size independently of the\nsegments_per_level GUC.",
          "timestamp": "2026-02-18T21:29:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3588dc0de18d8811d44c599edc15e614cff94108"
        },
        "date": 1771451070621,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 311076.151,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 3.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 4479.28,
            "unit": "MB"
          }
        ]
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
          "id": "6ad4ffb8f501035ede8687cafa098f8b04f38008",
          "message": "fix: detoast bm25vector in send and equality functions\n\ntpvector_send and tpvector_eq used PG_GETARG_POINTER which does not\ndetoast varlena datums. When PostgreSQL stores small bm25vectors with\n1-byte short varlena headers, VARSIZE() reads garbage (interpreting the\n1B header + data bytes as a 4B header), causing out-of-bounds reads.\n\nUnder ASAN sanitizer builds this crashes immediately when the garbage\nsize causes reads past the buffer page boundary into poisoned memory.\nWithout sanitizer, the bug is latent but produces corrupt binary output.\n\nFix by using PG_DETOAST_DATUM which ensures proper 4-byte varlena\nheaders. This matches what tpvector_out already does correctly.",
          "timestamp": "2026-02-19T01:28:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/6ad4ffb8f501035ede8687cafa098f8b04f38008"
        },
        "date": 1771466552903,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 177688.756,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.32,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2536.52,
            "unit": "MB"
          }
        ]
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
          "id": "8fe9814a5cd20fdf2600b248a21c2ec36d8eb624",
          "message": "feat: add worker-side compaction to parallel index build\n\nWorkers now perform level-aware compaction within their BufFile during\nparallel index build, mirroring tp_maybe_compact_level from serial\nbuild. This produces a proper LSM level structure instead of dumping\nall segments at L0.\n\n- Extract merge types and helpers into merge_internal.h for reuse\n- Add merge_source_init_from_reader() for BufFile-backed segments\n- Workers track segments per level and compact when threshold reached\n- write_merged_segment_to_buffile() performs N-way merge within BufFile\n- Leader writes compacted segments at correct levels, then runs final\n  compaction on combined per-level counts",
          "timestamp": "2026-02-19T03:45:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8fe9814a5cd20fdf2600b248a21c2ec36d8eb624"
        },
        "date": 1771474285474,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 273643.032,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2441.03,
            "unit": "MB"
          }
        ]
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
          "id": "b37eaabf08782a40680bdb28ea7549f97608a229",
          "message": "fix: use min fieldnorm for BMW skip entries in parallel build (#230)\n\n## Summary\n- Fix `write_posting_blocks()` in parallel build to compute MIN\nfieldnorm (shortest doc) instead of MAX (longest doc) per block\n- The `block_max_norm` skip entry field must store the minimum fieldnorm\nso BMW computes valid score upper bounds; using maximum caused BMW to\nincorrectly skip blocks containing high-scoring short documents\n- The serial build (`segment.c`) and merge (`merge.c`) paths already\nused `min_norm` correctly — this aligns the parallel build path\n- Add `parallel_bmw` regression test that deterministically reproduces\nthe bug: medium-length docs set the BMW threshold in early blocks, then\nmixed short+long doc blocks follow where the wrong upper bound causes\nBMW to skip them\n\n## Test plan\n- [x] `parallel_bmw` test fails deterministically without fix (0 short\ndocs in top-10), passes with fix (10 short docs)\n- [x] Verified 5/5 stable passes for `bmw` + `parallel_build` tests\n- [x] Full regression suite passes (only pre-existing `binary_io`\nfailure)\n- [x] `make format-check` passes",
          "timestamp": "2026-02-18T01:49:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b37eaabf08782a40680bdb28ea7549f97608a229"
        },
        "date": 1771482777321,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 277122.298,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 15.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "4639bf8baedd19b412fc720d7639d33927d8e726",
          "message": "feat: workers write index pages directly in two-phase parallel build\n\nEliminate single-threaded leader transcription bottleneck by having\nworkers write their segments directly to pre-allocated index pages.\n\nPhase 1: Workers scan heap, flush/compact in BufFile, report total\npages needed, signal phase1_done.\n\nLeader barrier: Sum page counts, pre-extend relation with batched\nExtendBufferedRelBy (8192 pages/batch), set atomic next_page counter,\nsignal phase2_ready.\n\nPhase 2: Workers reopen BufFile read-only, write segments to index\npages using lock-free atomic page counter, report seg_roots[].\n\nLeader finalization: Read seg_roots[] from shared memory (no BufFile\nI/O), chain segments into level lists, update metapage, compact.",
          "timestamp": "2026-02-19T19:23:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4639bf8baedd19b412fc720d7639d33927d8e726"
        },
        "date": 1771531981459,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 263387.172,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2441.08,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771568968667,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 270272.856,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "2b60e589d50a7c1f08ef44604dedb8d1821af874",
          "message": "fix: fix BufFile cursor handling in worker-side compaction\n\nThree interrelated bugs in worker-side BufFile compaction caused\ncorruption when worker BufFiles exceeded 1GB (the physical segment\nsize for Postgres BufFile):\n\n1. BufFileTell ordering: write_merged_segment_to_buffile() called\n   BufFileTell AFTER build_merged_docmap(), but that function reads\n   from BufFile-backed segment readers which moves the file cursor.\n   Fix: record the base position BEFORE build_merged_docmap and\n   re-seek after.\n\n2. Interleaved reads and writes: merge source reads (via\n   posting_source_advance -> tp_segment_read) share the same BufFile\n   handle as the output writes, moving the cursor between writes.\n   Fix: introduce buffile_write_at() that seeks to the absolute\n   output position before every write.\n\n3. SEEK_END race: BufFileSeek(SEEK_END) calls FileSize() before\n   flushing the dirty write buffer, returning the on-disk size which\n   is less than the logical size. Writing there overwrites unflushed\n   data. Fix: track buffile_end explicitly in WorkerSegmentTracker\n   and use it instead of SEEK_END.\n\nAlso decomposes BufFile composite offsets (fileno, offset) before all\nBufFileSeek calls to handle multi-segment BufFiles correctly.\n\nVerified with 138M row MS MARCO v2 parallel index build (8 workers).",
          "timestamp": "2026-02-20T08:29:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2b60e589d50a7c1f08ef44604dedb8d1821af874"
        },
        "date": 1771605122633,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 276087.987,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2441.01,
            "unit": "MB"
          }
        ]
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
          "id": "0541b9a41a685d05a817c764e9a3f7a373b1f001",
          "message": "fix: import worker segments as L0 and truncate unused pool pages\n\nWorker-side compaction produces segments at various levels (L1, L2, etc.)\nbut these levels are meaningless globally since each worker only sees 1/Nth\nof the data. Importing them at their worker-local levels inflates segment\ncounts (e.g. 7 L1 + 4 L0 = 11 segments vs ~4 L1 for serial build).\n\nImport all worker segments as L0 so leader-side compaction can cascade\nproperly through L0→L1→L2, matching serial build behavior.\n\nAlso truncate pre-allocated pool pages that workers never used, reducing\nindex size bloat from over-estimation.",
          "timestamp": "2026-02-20T22:38:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0541b9a41a685d05a817c764e9a3f7a373b1f001"
        },
        "date": 1771635974558,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 269050.469,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.94,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.2,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2441.06,
            "unit": "MB"
          }
        ]
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
          "id": "4723f0e5956dbae9483260c61abf3c7accd119ca",
          "message": "feat: unified merge sink eliminates parallel build fragmentation\n\nReplace the two-phase parallel build (workers write to BufFile then\ncopy to pre-allocated pages; leader compacts via FSM/P_NEW) with a\nsingle-phase design: workers scan+flush+compact to BufFile and exit;\nleader reopens all worker BufFiles, performs one N-way merge, and\nwrites a single contiguous segment via TpMergeSink.\n\nKey changes:\n- Add TpMergeSink abstraction (pages or BufFile backend) with\n  merge_sink_write() and merge_sink_write_at() for backpatching\n- Unify write_merged_segment() and write_merged_segment_to_buffile()\n  into write_merged_segment_to_sink() (~430 lines of duplication removed)\n- Remove Phase 2 from workers (no more phase2_cv, page pool,\n  atomic counter, pool truncation, segment linking)\n- Leader does BufFile-through merge directly to index pages,\n  producing one contiguous L1 segment with zero fragmentation\n\nNet: -509 lines. Index size should now match serial build.",
          "timestamp": "2026-02-21T03:50:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4723f0e5956dbae9483260c61abf3c7accd119ca"
        },
        "date": 1771646869887,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 505781.732,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.65,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.19,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1214.94,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771654974587,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 272593.993,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.23,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.3,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 4.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 7.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 10.15,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 14.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 23.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 5.1,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 6.17,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1093.95,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771741569186,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 275607.804,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.4,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.21,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.24,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "b15d8ad5c1b22366f4e019d4bec1f61937e46cc4",
          "message": "fix: restore parallel page writes, drop leader compaction\n\nThe previous commit moved ALL page writing to the leader via a\nsingle-threaded N-way merge, which nearly doubled build time\n(4:29 → 8:26 on CI benchmark). The fragmentation was never from\nworkers writing to pre-allocated pages — it was from the leader's\ntp_maybe_compact_level() using FSM/P_NEW which scattered pages.\n\nFix: restore two-phase parallel build where workers write BufFile\nsegments to pre-allocated contiguous pages in parallel, but skip\nthe compaction step entirely. Worker segments stay as L0 in the\ncontiguous pool and compact naturally on subsequent inserts or\nvia bm25_compact_index().\n\nKeep TpMergeSink abstraction in merge.c for worker BufFile\ncompaction and normal compaction paths.",
          "timestamp": "2026-02-23T01:48:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b15d8ad5c1b22366f4e019d4bec1f61937e46cc4"
        },
        "date": 1771812138168,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 175621.967,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 3.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.56,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2560.57,
            "unit": "MB"
          }
        ]
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
          "id": "3c81cc349ab563fb95f03dbd04c3d3aaae6dbfe7",
          "message": "feat: cross-worker compaction in parallel build\n\nAfter Phase 1, the leader plans merge groups by collecting all worker\nsegments by level and forming groups of segments_per_level. In Phase 2,\nworkers first COPY their non-merged segments to pages, then work-steal\nmerge groups via atomic counter. Each task (COPY or MERGE) bulk-claims\na contiguous page range from the shared counter. Merge outputs go\ndirectly to pages via merge_sink_init_pages_parallel() -- no\nintermediate BufFile.\n\nExample: 28 L0 segments (4 workers x 7 each, spl=8) -> 3 merge groups\nof 8 + 4 remaining copies. Phase 2 produces 4 L0 + 3 L1 = 7 segments\non contiguous pages, vs 28 L0 segments previously.",
          "timestamp": "2026-02-23T05:49:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3c81cc349ab563fb95f03dbd04c3d3aaae6dbfe7"
        },
        "date": 1771828084955,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco - Index Build Time",
            "value": 439332.356,
            "unit": "ms"
          },
          {
            "name": "msmarco - 1 Token Query (p50)",
            "value": 0.6,
            "unit": "ms"
          },
          {
            "name": "msmarco - 2 Token Query (p50)",
            "value": 1.79,
            "unit": "ms"
          },
          {
            "name": "msmarco - 3 Token Query (p50)",
            "value": 4.03,
            "unit": "ms"
          },
          {
            "name": "msmarco - 4 Token Query (p50)",
            "value": 6.18,
            "unit": "ms"
          },
          {
            "name": "msmarco - 5 Token Query (p50)",
            "value": 9.96,
            "unit": "ms"
          },
          {
            "name": "msmarco - 6 Token Query (p50)",
            "value": 14.16,
            "unit": "ms"
          },
          {
            "name": "msmarco - 7 Token Query (p50)",
            "value": 21.15,
            "unit": "ms"
          },
          {
            "name": "msmarco - 8+ Token Query (p50)",
            "value": 32.63,
            "unit": "ms"
          },
          {
            "name": "msmarco - Weighted Latency (p50, ms)",
            "value": 6.36,
            "unit": "ms"
          },
          {
            "name": "msmarco - Weighted Throughput (avg ms/query)",
            "value": 7.88,
            "unit": "ms"
          },
          {
            "name": "msmarco - Index Size",
            "value": 2522.9,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771828453401,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 281788.255,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.34,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 3.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 5.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 7.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 10.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 16.18,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 3.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 4.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1240.16,
            "unit": "MB"
          }
        ]
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
          "id": "db39fb20a2b6dca7463bdb80e905398ead69445c",
          "message": "fix: include root page in segment page estimation\n\ncompute_segment_pages() only counted data pages + page index pages,\nbut tp_segment_writer_init_parallel() also allocates a root/header\npage. This caused the page pool to be too small for large builds\n(138M rows, 4 workers), leading to reads past end of file.\n\nAdd +1 for the root page per segment.",
          "timestamp": "2026-02-23T07:00:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db39fb20a2b6dca7463bdb80e905398ead69445c"
        },
        "date": 1771832280913,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco - Index Build Time",
            "value": 439369.937,
            "unit": "ms"
          },
          {
            "name": "msmarco - 1 Token Query (p50)",
            "value": 0.62,
            "unit": "ms"
          },
          {
            "name": "msmarco - 2 Token Query (p50)",
            "value": 1.79,
            "unit": "ms"
          },
          {
            "name": "msmarco - 3 Token Query (p50)",
            "value": 3.82,
            "unit": "ms"
          },
          {
            "name": "msmarco - 4 Token Query (p50)",
            "value": 6.04,
            "unit": "ms"
          },
          {
            "name": "msmarco - 5 Token Query (p50)",
            "value": 10.28,
            "unit": "ms"
          },
          {
            "name": "msmarco - 6 Token Query (p50)",
            "value": 14.42,
            "unit": "ms"
          },
          {
            "name": "msmarco - 7 Token Query (p50)",
            "value": 21.66,
            "unit": "ms"
          },
          {
            "name": "msmarco - 8+ Token Query (p50)",
            "value": 32.91,
            "unit": "ms"
          },
          {
            "name": "msmarco - Weighted Latency (p50, ms)",
            "value": 6.34,
            "unit": "ms"
          },
          {
            "name": "msmarco - Weighted Throughput (avg ms/query)",
            "value": 7.92,
            "unit": "ms"
          },
          {
            "name": "msmarco - Index Size",
            "value": 2522.88,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771915159795,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 276450.821,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "9ad4271373e91371e28c2afa0e1077972af65291",
          "message": "feat: disjoint TID range scans + sequential drain merge\n\nReplace shared parallel heap scan with per-worker TID range scans.\nEach worker scans a contiguous, non-overlapping range of heap blocks,\nensuring disjoint CTIDs across workers.\n\nThis enables a sequential drain fast path in cross-worker merge:\ninstead of N-way CTID comparison per posting, drain sources in order\n(source 0 fully, then source 1, etc.). This eliminates:\n- posting_source_convert_current (2 pread calls per posting)\n- find_min_posting_source (CTID comparison per posting)\n\nThese two functions accounted for ~47% of CPU time in Phase 2 of\nthe 138M-row MS-MARCO v2 parallel build.\n\nChanges:\n- Leader divides heap blocks evenly across launched workers, stores\n  ranges in shared memory, signals scan_ready atomic flag\n- Workers spin-wait on scan_ready, then open table_beginscan_tidrange\n  limited to their assigned block range\n- build_merged_docmap: concatenates sources sequentially when disjoint\n  instead of N-way CTID merge\n- write_merged_segment_to_sink: new disjoint_sources parameter enables\n  sequential drain using posting_source_advance_fast (no CTID lookup)\n- Extract FLUSH_BLOCK macro to deduplicate block write logic\n- Remove parallel table scan descriptor (no longer needed)",
          "timestamp": "2026-02-24T22:03:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9ad4271373e91371e28c2afa0e1077972af65291"
        },
        "date": 1771971519770,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 211861.758,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.43,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.35,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.11,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2574.94,
            "unit": "MB"
          }
        ]
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
          "id": "afdca64b8adf628c526b18433f8b07315d180ce8",
          "message": "fix: register snapshot in worker for PG18 tidrange scan\n\nPG18 added an assertion in heap_prepare_pagescan requiring any snapshot\nused in a heap scan to be registered or active. The leader already\nregistered the snapshot, but the worker passed an unregistered snapshot\nfrom GetTransactionSnapshot() to table_beginscan_tidrange(), tripping\nthe assertion under sanitizer builds.\n\nMirror the leader's pattern: RegisterSnapshot before scan,\nUnregisterSnapshot after table_endscan.",
          "timestamp": "2026-02-24T22:33:25Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/afdca64b8adf628c526b18433f8b07315d180ce8"
        },
        "date": 1771975254251,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 218053.842,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.67,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.31,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.71,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2574.94,
            "unit": "MB"
          }
        ]
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
          "id": "a1cfff5b14cf371eae6301cab89d1e871e6fb662",
          "message": "Revert versioned shared library filename (#238)\n\n## Summary\n\n- Reverts #232 which added version numbers to the shared library\nfilename\n- The versioned binary naming scheme (e.g.,\n`pg_textsearch-1.0.0-dev.so`) caused problems in practice\n- Returns to the standard unversioned `pg_textsearch.so` naming\n\n## Testing\n\n- `make installcheck` passes (all SQL regression + shell tests)\n- `make format-check` passes",
          "timestamp": "2026-02-24T23:36:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a1cfff5b14cf371eae6301cab89d1e871e6fb662"
        },
        "date": 1772001587130,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 278515.607,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 2.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 3.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 5.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 7.41,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 10.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 15.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 3.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 4.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1239.95,
            "unit": "MB"
          }
        ]
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
          "id": "40f07e03865c931bfb8bcfec0d23aecae3620e87",
          "message": "fix: recompute ground truth for PG17 stemmer differences\n\nPG18 updated the English Snowball stemmer: \"adding\"/\"added\" now stem\nto \"add\" instead of \"ad\" (PG17). Since CI runs PG17, the ground truth\nmust use PG17 df values. This affected query 158 (the only validation\nquery containing \"add\").\n\nAlso improve validation tolerance: use absolute threshold (0.001)\ninstead of 4-decimal-place rounding, and handle tied-score rank swaps\nat the top-10 boundary.",
          "timestamp": "2026-02-25T20:33:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40f07e03865c931bfb8bcfec0d23aecae3620e87"
        },
        "date": 1772052171362,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 271105.868,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 3.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 5.25,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 7.53,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 11.6,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 15.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 22.13,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 33.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 7.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 9.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.18,
            "unit": "MB"
          }
        ]
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
          "id": "0857970d2c659793a40b097fad150bc5f0fb7b94",
          "message": "fix: version ground truth files per Postgres major version\n\nPG17 and PG18 have different Snowball stemmers (\"adding\"/\"added\" stem\nto \"ad\" on PG17, \"add\" on PG18), which changes df values and BM25\nscores for queries containing \"add\". Store separate ground truth files\nper PG version and select the right one in CI based on pg_config.",
          "timestamp": "2026-02-25T20:46:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0857970d2c659793a40b097fad150bc5f0fb7b94"
        },
        "date": 1772052961700,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 263328.178,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 7.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 15.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 23.12,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 34.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 7.45,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 12.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "44deea684c705c68e2832796e146ccf1d8b3784b",
          "message": "fix: prevent cascading merge group segfault + dedup build helpers\n\nThree changes:\n\n1. Fix segfault in parallel build: remainder merge groups no longer\n   cascade to higher levels. worker_execute_merge_group() only\n   searches worker result arrays for source segments, not outputs\n   of earlier merge groups. When cascading groups were planned,\n   they found no sources and accessed invalid block numbers,\n   causing SIGSEGV on large tables (8.8M+ rows, 4+ workers).\n\n2. Extract tp_truncate_dead_pages() shared helper from duplicated\n   code in build.c (bm25_compact_index) and build_parallel.c\n   (post-build truncation). Walks all segment chains to find the\n   highest used block and truncates everything beyond it.\n\n3. Extract tp_link_l0_chain_head() helper from 4x duplicated\n   pattern in build.c (auto-spill, flush-and-link, spill-memtable,\n   final-spill). Also removes dead final memtable spill block\n   from serial build path (arena build never populates memtable).",
          "timestamp": "2026-02-26T01:59:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/44deea684c705c68e2832796e146ccf1d8b3784b"
        },
        "date": 1772071955356,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 225111.934,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.64,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "3dc4a8fc8f6a0c63fa0b1cee190ca80207d0e27f",
          "message": "fix: compact remaining segments after parallel build for query perf\n\nWithout cascading merge groups, the parallel build can leave\na few segments at each level (e.g., 3 L1 segments from 2 full\nmerge groups + 1 remainder). This hurts query performance since\nBMW must merge across all segments.\n\nAdd tp_compact_all() after linking chains to merge remaining\nsegments into one, followed by tp_truncate_dead_pages() to\nreclaim any dead pages left by the compaction.",
          "timestamp": "2026-02-26T02:14:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3dc4a8fc8f6a0c63fa0b1cee190ca80207d0e27f"
        },
        "date": 1772072885573,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 222022.731,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.37,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 5.99,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.44,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "04f4387e2ca42e382e29105d101b8b6909cab8fe",
          "message": "fix: tp_compact_all must check all levels, not just bottom-up\n\ntp_compact_all() used break when it found a level with < 2\nsegments, assuming higher levels would also be sparse. This\nassumption is wrong after parallel build where merge groups\noutput directly at L1+, leaving L0 empty. Change break to\ncontinue so all levels are checked.\n\nThis was preventing post-build compaction from merging the\n2 L1 segments (from full + remainder merge groups) into a\nsingle L2 segment, hurting query performance.",
          "timestamp": "2026-02-26T02:31:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/04f4387e2ca42e382e29105d101b8b6909cab8fe"
        },
        "date": 1772073957591,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 298274.632,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.87,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.39,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 2601.29,
            "unit": "MB"
          }
        ]
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
          "id": "44deea684c705c68e2832796e146ccf1d8b3784b",
          "message": "fix: prevent cascading merge group segfault + dedup build helpers\n\nThree changes:\n\n1. Fix segfault in parallel build: remainder merge groups no longer\n   cascade to higher levels. worker_execute_merge_group() only\n   searches worker result arrays for source segments, not outputs\n   of earlier merge groups. When cascading groups were planned,\n   they found no sources and accessed invalid block numbers,\n   causing SIGSEGV on large tables (8.8M+ rows, 4+ workers).\n\n2. Extract tp_truncate_dead_pages() shared helper from duplicated\n   code in build.c (bm25_compact_index) and build_parallel.c\n   (post-build truncation). Walks all segment chains to find the\n   highest used block and truncates everything beyond it.\n\n3. Extract tp_link_l0_chain_head() helper from 4x duplicated\n   pattern in build.c (auto-spill, flush-and-link, spill-memtable,\n   final-spill). Also removes dead final memtable spill block\n   from serial build path (arena build never populates memtable).",
          "timestamp": "2026-02-26T01:59:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/44deea684c705c68e2832796e146ccf1d8b3784b"
        },
        "date": 1772075835376,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 228064.568,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.8,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.08,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "4a9aaf2a430249d4b20d0c4045b4ddf6c5a8bebb",
          "message": "feat: require shared_preload_libraries for pg_textsearch (#235)\n\n## Summary\n\n- Enforce loading via `shared_preload_libraries` to prevent fatal\n`ShmemIndex entry size is wrong` errors when the .so is replaced during\ndeployment without a server restart\n- Remove lazy init fallbacks in registry.c that were the previous\nworkaround for not requiring preloading\n- Update all test infrastructure (Makefile, shell scripts, CI workflows)\nto configure `shared_preload_libraries` with the versioned library name\n\n## Context\n\npg_textsearch uses Postgres shared memory (`ShmemInitStruct`) for its\nindex registry. Without `shared_preload_libraries`, each backend loads\nthe .so independently via `dlopen`. If the .so file is replaced on disk\n(e.g., during a deployment) without restarting Postgres, different\nbackends get different code, causing a fatal `ShmemIndex entry size is\nwrong` error. Requiring `shared_preload_libraries` ensures all backends\nuse the same .so (inherited from the postmaster via fork), eliminating\nthis class of bugs.\n\n## Testing\n\n- All 47 SQL regression tests pass\n- `make format-check` passes\n- Error message verified when extension not in shared_preload_libraries",
          "timestamp": "2026-02-25T23:21:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4a9aaf2a430249d4b20d0c4045b4ddf6c5a8bebb"
        },
        "date": 1772087740960,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 281488.667,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 1.55,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 2.59,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 4.48,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 6.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.93,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 15.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.62,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 33.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 7.01,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.18,
            "unit": "MB"
          }
        ]
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
          "id": "f1384f93b557bdea96e1de131c56195afb2508f3",
          "message": "feat: detect stale binary after upgrade via library version check (#241)\n\n## Summary\n\n- Add `pg_textsearch.library_version` read-only GUC set at library load\ntime from a compile-time version define\n- Check the GUC in `CREATE EXTENSION` and `ALTER EXTENSION UPDATE` SQL\nscripts, failing with a clear error if the loaded library is missing or\ndoesn't match the expected version\n- Catches the case where a user runs `make install` but forgets to\nrestart the server\n\n## Changes\n\n- **Makefile**: Extract `EXTVERSION` from `pg_textsearch.control`, pass\nas `-DPG_TEXTSEARCH_VERSION`\n- **src/mod.c**: Register `PGC_INTERNAL` string GUC in `_PG_init`\n- **sql/pg_textsearch--1.0.0-dev.sql**: Version check DO block before\nany CREATE statements\n- **sql/pg_textsearch--0.5.0--1.0.0-dev.sql**: Same version check in\nupgrade script\n- **test/sql/basic.sql**: Regression test for the GUC\n\n## Testing\n\n- `SHOW pg_textsearch.library_version` returns `1.0.0-dev` after restart\n- All 47 regression tests pass",
          "timestamp": "2026-02-26T19:19:22Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f1384f93b557bdea96e1de131c56195afb2508f3"
        },
        "date": 1772173771606,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 266681.429,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 2.06,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 3.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 10.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 15.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 32.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 7.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 8.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1227.17,
            "unit": "MB"
          }
        ]
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772243896924,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 224704.001,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.83,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.58,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.95,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.96,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.09,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "86ec2fd148ae2d2bc22a6be526183bb45476ea92",
          "message": "feat: rewrite index build with arena allocator and parallel page pool (#231)\n\n## Summary\n\n- **Arena allocator + EXPULL posting lists**: Replace DSA/dshash shared\nmemory with process-local arena (1MB slab bump allocator, 32-bit\naddresses) and exponential unrolled linked lists. Eliminates DSA\nfragmentation, dshash lock overhead, and posting list reallocation\ncopies.\n- **TpBuildContext**: New per-build context with arena, HTAB term\ndictionary, and flat doc arrays. Budget-controlled flushing using\n`maintenance_work_mem`. Used by both serial and parallel CREATE INDEX.\n- **Two-phase parallel build**: Workers scan disjoint TID ranges, flush\nto BufFile temp segments, compact within BufFile. Leader plans\ncross-worker merge groups. Workers then COPY non-merged segments to\npre-allocated index pages and work-steal merge groups via atomic\ncounter.\n- **Unified merge sink**: `TpMergeSink` abstraction writes merged\nsegments to either index pages or BufFile, eliminating separate code\npaths. Disjoint source fast path enables sequential drain merge without\nCTID comparison.\n- **bm25_force_merge()**: New SQL function to force-merge all segments\ninto one (à la Lucene's `forceMerge(1)`).\n- **TOAST fix**: `tpvector_send()` and `tpvector_eq()` now properly\ndetoast values.\n\n## Motivation\n\nThe previous parallel build used DSA shared memory with dshash for term\ndictionaries and double-buffered memtable swapping. This caused:\n- DSA fragmentation under large workloads (100GB+ OOM on MS MARCO v2)\n- dshash lock contention between workers\n- Complex N-way merge in the leader\n- ~2200 lines of intricate coordination code\n\nThe new approach is simpler and more memory-efficient: each worker gets\na local arena with a budget, flushes segments to BufFile temp files, and\nthe leader coordinates a two-phase write to pre-allocated index pages.\nCross-worker compaction merges segments that span worker boundaries.\n\n## Benchmark results (MS MARCO, 8.8M passages)\n\n| Metric | main | System X | This PR | vs main |\n|--------|------|----------|---------|---------|\n| Build Time | 278.5s | 146.2s | 228.1s | **19% faster** |\n| Index Size | 1240 MB | 1497 MB | 1360 MB | +10% (9% smaller than\nSystem X) |\n| Query P50 | 3.74ms | — | 6.08ms | +63% |\n| Query Avg | 4.49ms | — | 7.55ms | +68% |\n\n**Note on query latency**: The parallel build produces 2-3 L1 segments\ninstead of a single fully-compacted segment (as on main). The BMW query\noptimizer must scan multiple segments, which increases latency. A\nfollow-up will add post-build `bm25_force_merge()` that merges into\npre-allocated pages (using the existing BufFile→index-page COPY path) to\neliminate this regression.\n\n## Key changes\n\n### New files\n- `src/memtable/arena.c/h` — 1MB page-based bump allocator with 32-bit\naddresses (12-bit page + 20-bit offset), max 4GB\n- `src/memtable/expull.c/h` — Exponential unrolled linked list for\nposting lists (blocks grow 32B→32KB, 7-byte packed entries)\n- `src/am/build_context.c/h` — Arena-based build context replacing DSA\nmemtable during builds\n- `src/segment/merge_internal.h` — Exposes merge internals for\nbuild_parallel.c\n\n### Major modifications\n- `src/am/build.c` — Serial build uses TpBuildContext; added\n`bm25_force_merge()` SQL function; extracted `tp_link_l0_chain_head()`\nand `tp_truncate_dead_pages()` shared helpers\n- `src/am/build_parallel.c` — Two-phase parallel build with disjoint TID\nranges, BufFile worker segments, cross-worker merge groups, and atomic\npage pool\n- `src/segment/merge.c` — Unified TpMergeSink abstraction; disjoint\nsource fast path; streaming docmap merge\n- `src/segment/segment.c` — Added parallel writer init and atomic page\nallocation\n\n### Bug fixes\n- `src/types/vector.c` — Use `PG_DETOAST_DATUM` for TOAST safety in\nsend/eq functions\n- `src/am/scan.c` — Removed `TP_MAX_BLOCK_NUMBER` guard (large\nparallel-built indexes can exceed 1M blocks)\n- `sql/pg_textsearch--0.5.0--1.0.0-dev.sql` — Added `bm25_force_merge`\nto upgrade path\n\n## Test plan\n\n- [x] All 47 regression tests pass (PG17 + PG18)\n- [x] `parallel_build` test validates serial, 1-worker, 2-worker,\n4-worker builds with query correctness\n- [x] `parallel_bmw` test validates Block-Max WAND with parallel-built\nindexes\n- [x] `make format-check` passes\n- [x] CI green: gcc/clang × PG17/PG18, sanitizers\n- [x] MS MARCO benchmark passes (8.8M passages, build + query\nvalidation)\n- [x] Rebased onto current main (shared_preload_libraries, TOCTOU fix,\nvalidation)",
          "timestamp": "2026-02-27T21:46:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/86ec2fd148ae2d2bc22a6be526183bb45476ea92"
        },
        "date": 1772259437333,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 217828.935,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.7,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.36,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.33,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.89,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 30.73,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 5.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "86ec2fd148ae2d2bc22a6be526183bb45476ea92",
          "message": "feat: rewrite index build with arena allocator and parallel page pool (#231)\n\n## Summary\n\n- **Arena allocator + EXPULL posting lists**: Replace DSA/dshash shared\nmemory with process-local arena (1MB slab bump allocator, 32-bit\naddresses) and exponential unrolled linked lists. Eliminates DSA\nfragmentation, dshash lock overhead, and posting list reallocation\ncopies.\n- **TpBuildContext**: New per-build context with arena, HTAB term\ndictionary, and flat doc arrays. Budget-controlled flushing using\n`maintenance_work_mem`. Used by both serial and parallel CREATE INDEX.\n- **Two-phase parallel build**: Workers scan disjoint TID ranges, flush\nto BufFile temp segments, compact within BufFile. Leader plans\ncross-worker merge groups. Workers then COPY non-merged segments to\npre-allocated index pages and work-steal merge groups via atomic\ncounter.\n- **Unified merge sink**: `TpMergeSink` abstraction writes merged\nsegments to either index pages or BufFile, eliminating separate code\npaths. Disjoint source fast path enables sequential drain merge without\nCTID comparison.\n- **bm25_force_merge()**: New SQL function to force-merge all segments\ninto one (à la Lucene's `forceMerge(1)`).\n- **TOAST fix**: `tpvector_send()` and `tpvector_eq()` now properly\ndetoast values.\n\n## Motivation\n\nThe previous parallel build used DSA shared memory with dshash for term\ndictionaries and double-buffered memtable swapping. This caused:\n- DSA fragmentation under large workloads (100GB+ OOM on MS MARCO v2)\n- dshash lock contention between workers\n- Complex N-way merge in the leader\n- ~2200 lines of intricate coordination code\n\nThe new approach is simpler and more memory-efficient: each worker gets\na local arena with a budget, flushes segments to BufFile temp files, and\nthe leader coordinates a two-phase write to pre-allocated index pages.\nCross-worker compaction merges segments that span worker boundaries.\n\n## Benchmark results (MS MARCO, 8.8M passages)\n\n| Metric | main | System X | This PR | vs main |\n|--------|------|----------|---------|---------|\n| Build Time | 278.5s | 146.2s | 228.1s | **19% faster** |\n| Index Size | 1240 MB | 1497 MB | 1360 MB | +10% (9% smaller than\nSystem X) |\n| Query P50 | 3.74ms | — | 6.08ms | +63% |\n| Query Avg | 4.49ms | — | 7.55ms | +68% |\n\n**Note on query latency**: The parallel build produces 2-3 L1 segments\ninstead of a single fully-compacted segment (as on main). The BMW query\noptimizer must scan multiple segments, which increases latency. A\nfollow-up will add post-build `bm25_force_merge()` that merges into\npre-allocated pages (using the existing BufFile→index-page COPY path) to\neliminate this regression.\n\n## Key changes\n\n### New files\n- `src/memtable/arena.c/h` — 1MB page-based bump allocator with 32-bit\naddresses (12-bit page + 20-bit offset), max 4GB\n- `src/memtable/expull.c/h` — Exponential unrolled linked list for\nposting lists (blocks grow 32B→32KB, 7-byte packed entries)\n- `src/am/build_context.c/h` — Arena-based build context replacing DSA\nmemtable during builds\n- `src/segment/merge_internal.h` — Exposes merge internals for\nbuild_parallel.c\n\n### Major modifications\n- `src/am/build.c` — Serial build uses TpBuildContext; added\n`bm25_force_merge()` SQL function; extracted `tp_link_l0_chain_head()`\nand `tp_truncate_dead_pages()` shared helpers\n- `src/am/build_parallel.c` — Two-phase parallel build with disjoint TID\nranges, BufFile worker segments, cross-worker merge groups, and atomic\npage pool\n- `src/segment/merge.c` — Unified TpMergeSink abstraction; disjoint\nsource fast path; streaming docmap merge\n- `src/segment/segment.c` — Added parallel writer init and atomic page\nallocation\n\n### Bug fixes\n- `src/types/vector.c` — Use `PG_DETOAST_DATUM` for TOAST safety in\nsend/eq functions\n- `src/am/scan.c` — Removed `TP_MAX_BLOCK_NUMBER` guard (large\nparallel-built indexes can exceed 1M blocks)\n- `sql/pg_textsearch--0.5.0--1.0.0-dev.sql` — Added `bm25_force_merge`\nto upgrade path\n\n## Test plan\n\n- [x] All 47 regression tests pass (PG17 + PG18)\n- [x] `parallel_build` test validates serial, 1-worker, 2-worker,\n4-worker builds with query correctness\n- [x] `parallel_bmw` test validates Block-Max WAND with parallel-built\nindexes\n- [x] `make format-check` passes\n- [x] CI green: gcc/clang × PG17/PG18, sanitizers\n- [x] MS MARCO benchmark passes (8.8M passages, build + query\nvalidation)\n- [x] Rebased onto current main (shared_preload_libraries, TOCTOU fix,\nvalidation)",
          "timestamp": "2026-02-27T21:46:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/86ec2fd148ae2d2bc22a6be526183bb45476ea92"
        },
        "date": 1772346298767,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 224901.915,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.75,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.52,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.42,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.02,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.49,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "93170a2d07de18d890efa12b0c79489e0e6669dc",
          "message": "feat: leader-only merge for parallel index build\n\nReplace the two-phase parallel build (worker compaction + work-stealing\nmerge groups) with a simpler architecture: workers flush L0 segments to\nBufFiles without compaction, then the leader performs a single N-way\nmerge of all segments directly to paged storage.\n\nThis produces a single segment per index build, which is optimal for\nquery performance (no multi-segment scanning needed).\n\nRemoves ~800 lines of complexity: worker_maybe_compact_level,\nplan_merge_groups, compute_total_pages_needed,\nworker_execute_merge_group, write_temp_segment_to_index_parallel,\nand all Phase 2 worker code (COPY + work-steal).",
          "timestamp": "2026-03-01T17:46:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/93170a2d07de18d890efa12b0c79489e0e6669dc"
        },
        "date": 1772388122596,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 217953.409,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.57,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.77,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.79,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.85,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.97,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 14.28,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 21.46,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 33.14,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.22,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1214.94,
            "unit": "MB"
          }
        ]
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
          "id": "a72f481e17e0c684b7bd37be3da4067674ddec35",
          "message": "fix: address code review issues in leader-only merge\n\n- Move FlushRelationBuffers before metapage update for consistency\n  with merge.c pattern (segment data durable before metadata)\n- Use merge_source_close() instead of manual pfree for cleanup\n- NULL readers[] slot when source takes ownership to prevent\n  double-close\n- Remove dead segment_count field from TpParallelWorkerResult",
          "timestamp": "2026-03-01T18:44:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a72f481e17e0c684b7bd37be3da4067674ddec35"
        },
        "date": 1772394616984,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 237207.904,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.81,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.88,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.78,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.91,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 20.47,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.98,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.16,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.69,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1214.94,
            "unit": "MB"
          }
        ]
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
          "id": "e8601e2c78ac4f757e3255744f2a83360ed6b20b",
          "message": "fix: add coverage gate to block PRs on coverage reduction (#245)\n\n## Summary\n- The coverage job collected data and uploaded to Codecov but never\nenforced\n  thresholds — it always succeeded regardless of coverage values\n- Codecov status checks (`codecov/project`, `codecov/patch`) weren't\nappearing\n  on PRs, likely due to a missing/invalid `CODECOV_TOKEN`, and\n  `fail_ci_if_error` was `false` so the upload failure was silent\n- Branch protection had an empty required checks list\n(`all-tests-passed` was\n  not listed), so CI failures couldn't block merges — now fixed\n\n### Changes\n- Add in-workflow coverage gate to both `ci.yml` and `coverage.yml`\nthat:\n  - Enforces a minimum 85% line coverage threshold\n  - Caches the main branch baseline via `actions/cache`\n- Compares PR coverage against baseline, fails if it drops by more than\n1%\n- Change `fail_ci_if_error` from `false` to `true` on Codecov upload\n- Added `all-tests-passed` to branch protection required status checks\n\n### How the baseline works\n- On pushes to `main`, the coverage percentage is saved to GitHub\nActions\n  cache with key `coverage-baseline-<sha>`\n- On PRs, the most recent main branch cache is restored and compared\nagainst\n  the PR's coverage\n- First run after this merges will skip the reduction check (no baseline\nyet)\n  and establish the initial baseline\n\n## Testing\n- CI on this PR validates the gate runs (no baseline exists yet, so the\nreduction check was skipped, but the minimum threshold check ran and\npassed)\n- After merging, the next PR that reduces coverage by >1% will fail",
          "timestamp": "2026-03-02T04:13:08Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e8601e2c78ac4f757e3255744f2a83360ed6b20b"
        },
        "date": 1772432876272,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 220663.278,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.76,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.82,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.74,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.92,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.86,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 6.05,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.51,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1359.97,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772519936296,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco (8.8M docs) - Index Build Time",
            "value": 234169.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 0.72,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 1.84,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 3.61,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 5.66,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 9.07,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 13.5,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 19.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 31.68,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Latency (p50, ms)",
            "value": 5.9,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Weighted Throughput (avg ms/query)",
            "value": 7.38,
            "unit": "ms"
          },
          {
            "name": "msmarco (8.8M docs) - Index Size",
            "value": 1214.94,
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
        "date": 1770532140111,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11622.318,
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
            "value": 0.21,
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
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.71,
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
          "id": "348671dc23717a8278b564a3e1d5b6ad7fb02199",
          "message": "feat: iterative index scan with exponential backoff (#209)\n\nCloses #143\n\n## Summary\n\n- When a WHERE clause post-filters BM25 index results, the one-shot\ntop-k scan could return fewer rows than LIMIT requests (or zero rows),\neven when qualifying rows exist in the table\n- Fix: `tp_gettuple` now doubles the internal limit and re-executes the\nscoring query when results are exhausted, skipping already-returned rows\n- Terminates when `result_count < max_results_used` (all matching docs\nfound) or the limit reaches `TP_MAX_QUERY_LIMIT`\n\n## Testing\n\nNew `rescan.sql` regression test with 9 scenarios:\n- Complete miss (all top-k filtered out by WHERE)\n- Default limit + WHERE (no explicit LIMIT)\n- Partial miss (some results pass filter)\n- Result ordering preserved after rescan\n- Large table with aggressive filtering (500 rows, 2% match rate)\n- WHERE on non-indexed column\n- LIMIT + OFFSET + WHERE\n- Multiple backoffs required (8 doublings with default_limit=5)\n- Backoff terminates when all matching rows exhausted\n\nUpdated expected output for `limits` and `mixed` tests which now\ncorrectly return additional rows that the rescan finds.",
          "timestamp": "2026-02-07T15:53:42Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/348671dc23717a8278b564a3e1d5b6ad7fb02199"
        },
        "date": 1770618912702,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14862.516,
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
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.48,
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
          "id": "74d725b6bdf5e4edae71f20b305ad05133819dca",
          "message": "feat: implement WAND pivot selection for multi-term queries (#210)\n\n## Summary\n\n- Replace min-doc-id pivot with classic WAND pivot selection for\nmulti-term BMW queries\n- Terms sorted by current doc_id; walk accumulating max_scores until\nthreshold exceeded, skipping large doc_id ranges that can't contribute\nto top-k\n- Block-max refinement with Tantivy-style skip advancement (advance\nhighest-value scorer past block boundary)\n- Linear insertion maintains sorted order — O(1) typical for low term\ncounts, preserving existing performance\n\n## Motivation\n\n8+ term queries (MS-MARCO p50) are 18% slower than System X. The old\napproach found the minimum doc_id and checked all terms, evaluating many\ndocuments that couldn't beat the threshold. Classic WAND skips these\nentirely.\n\n## Changes\n\n- `src/query/bmw.c`: Add `max_score` to TpTermState, sort\ninfrastructure, `find_wand_pivot`, block-max refinement, rewritten main\nloop\n- `test/sql/wand.sql`: 8-term validation test (BMW vs exhaustive + BM25\nreference)\n\n## Testing\n\n- All regression tests pass\n- All shell tests pass (concurrency, recovery, CIC)\n- BM25 scores verified identical to reference implementation\n- MS-MARCO benchmark indicates query improvement across the board\n\nAddresses #192.",
          "timestamp": "2026-02-09T23:41:48Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/74d725b6bdf5e4edae71f20b305ad05133819dca"
        },
        "date": 1770681448408,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14272.436,
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
            "value": 0.21,
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
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.55,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.28,
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
          "id": "23831b5decf5e26f0d5813e985bc5f2d36b178c3",
          "message": "docs: update benchmark comparison with Feb 9 results (#217)\n\n## Summary\n- Update comparison page with results after WAND pivot selection merge\n(PR #210)\n- pg_textsearch now faster than System X across all 8 token buckets\n(previously lost on 8+ tokens)\n- Overall throughput improved from 1.8x to 2.8x faster\n- System X version updated from 0.20.6 to 0.21.6\n\n## Key changes in numbers\n- 8+ token p50: 49.03 ms → 27.95 ms (43% faster than previous, now beats\nSystem X's 41.23 ms)\n- 7 token p50: 30.06 ms → 18.02 ms (40% faster)\n- Throughput: 16.65 ms/q → 10.48 ms/q (vs System X 29.18 ms/q)\n\n## Testing\n- Numbers extracted from benchmark run\n[#21845385796](https://github.com/timescale/pg_textsearch/actions/runs/21845385796)\n- gh-pages branch still needs a matching update after this merges",
          "timestamp": "2026-02-10T03:01:14Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/23831b5decf5e26f0d5813e985bc5f2d36b178c3"
        },
        "date": 1770705760565,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12354.724,
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
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.6,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
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
          "id": "db94e895c547b5558cc2989336f09ea0ad130343",
          "message": "ci: run coverage on PRs and enable branch coverage (#221)\n\n## Summary\n- Run the coverage workflow on `pull_request` events (same path filters\nas push) so Codecov can post diff coverage comments on PRs\n- Enable lcov branch coverage (`--rc branch_coverage=1`) in CI and\nMakefile targets\n\n## Testing\n- This PR itself will trigger the coverage workflow, confirming it works\non PRs\n- Branch coverage data will appear in the lcov summary and HTML report",
          "timestamp": "2026-02-11T03:49:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db94e895c547b5558cc2989336f09ea0ad130343"
        },
        "date": 1770791666468,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12957.083,
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
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
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
          "id": "db94e895c547b5558cc2989336f09ea0ad130343",
          "message": "ci: run coverage on PRs and enable branch coverage (#221)\n\n## Summary\n- Run the coverage workflow on `pull_request` events (same path filters\nas push) so Codecov can post diff coverage comments on PRs\n- Enable lcov branch coverage (`--rc branch_coverage=1`) in CI and\nMakefile targets\n\n## Testing\n- This PR itself will trigger the coverage workflow, confirming it works\non PRs\n- Branch coverage data will appear in the lcov summary and HTML report",
          "timestamp": "2026-02-11T03:49:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/db94e895c547b5558cc2989336f09ea0ad130343"
        },
        "date": 1770878057932,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14363.156,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.16,
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
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.55,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.28,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1770964345441,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12252.706,
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
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.28,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771050262453,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12572.387,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.59,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.29,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771136892823,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13161.251,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.28,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771223623246,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13630.737,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.21,
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
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.57,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.28,
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
          "id": "40fb42f8d40623496cc1bf363b990e7968dc6596",
          "message": "fix: report index scan statistics via pgstat counters (#224)\n\n## Summary\n\n- Add `pgstat_count_index_scan()` call in `tp_gettuple()` so that\n`pg_stat_user_indexes.idx_scan` is incremented for BM25 index scans\n- Add `IndexScanInstrumentation` support (`nsearches++`) for PG18+\nEXPLAIN ANALYZE reporting\n- Add `pgstats` regression test verifying `idx_scan` and `idx_tup_read`\ncounters\n\nThe BM25 access method was not calling `pgstat_count_index_scan()`,\nwhich is each AM's responsibility (btree does it in `_bt_first()`,\npgvector in its `gettuple`). The core only handles\n`pgstat_count_index_tuples()` (for `idx_tup_read`) and\n`pgstat_count_heap_fetch()` (for `idx_tup_fetch`).\n\nCloses #223\n\n## Testing\n\nNew regression test (`test/sql/pgstats.sql`) that:\n1. Creates a table with a BM25 index\n2. Runs queries and verifies `idx_scan > 0` and `idx_tup_read > 0`\n3. Runs a second query and verifies `idx_scan > 1`",
          "timestamp": "2026-02-12T18:17:13Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/40fb42f8d40623496cc1bf363b990e7968dc6596"
        },
        "date": 1771310495954,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11962.552,
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
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.58,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Throughput (800 queries, avg ms/query)",
            "value": 0.28,
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
          "id": "957c07c15894f2711b1b9645a439bbb8f8a72fa3",
          "message": "chore: remove DEBUG1 elog from parallel build DSA limit",
          "timestamp": "2026-02-17T19:50:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/957c07c15894f2711b1b9645a439bbb8f8a72fa3"
        },
        "date": 1771358492993,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11736.804,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.1,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.54,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "0f9564cb42f873ad58264c58e6b10aedeabacb6c",
          "message": "bench: add MS MARCO v2 dataset and weighted-average latency metric (#226)\n\n## Summary\n\n- Add benchmark infrastructure for the **MS MARCO v2** passage ranking\ndataset (138M passages) with download, load, query, and System X\ncomparison scripts\n- Introduce **weighted-average query latency** as a new tracked metric\nacross all benchmark datasets (MS MARCO v1, v2, Wikipedia)\n- Per-bucket p50 latencies are weighted by the observed MS-MARCO v1\nlexeme distribution (1M queries: 3-token mode at 30%, mean 3.7 tokens)\nto produce a single summary number that reflects realistic workload\nperformance\n\n### Files changed\n\n**New (MS MARCO v2 benchmark infrastructure):**\n- `benchmarks/datasets/msmarco-v2/` — download, load, query scripts +\nbenchmark queries TSV\n- `benchmarks/datasets/msmarco-v2/systemx/` — System X (ParadeDB)\ncomparison scripts\n\n**Modified (weighted-average metric):**\n- `benchmarks/datasets/msmarco/queries.sql` — add weighted-average\ncomputation\n- `benchmarks/datasets/wikipedia/queries.sql` — add weighted-average\ncomputation\n- `benchmarks/runner/extract_metrics.sh` — extract\n`WEIGHTED_LATENCY_RESULT` from logs\n- `benchmarks/runner/format_for_action.sh` — report weighted latency in\ngithub-action-benchmark format\n- `benchmarks/runner/run_benchmark.sh` — add msmarco-v2 as a dataset\noption\n- `benchmarks/gh-pages/methodology.html` — document weighted methodology\n+ MS MARCO v2 dataset\n\n### Distribution weights (MS-MARCO v1, 1,010,905 queries)\n\n| Tokens | Count | Weight |\n|--------|-------|--------|\n| 1 | 35,638 | 3.5% |\n| 2 | 165,033 | 16.3% |\n| 3 | 304,887 | 30.2% |\n| 4 | 264,177 | 26.1% |\n| 5 | 143,765 | 14.2% |\n| 6 | 59,558 | 5.9% |\n| 7 | 22,595 | 2.2% |\n| 8+ | 15,252 | 1.5% |",
          "timestamp": "2026-02-17T18:51:22Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0f9564cb42f873ad58264c58e6b10aedeabacb6c"
        },
        "date": 1771358661858,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 15007.931,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.16,
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
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.25,
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
          "id": "ef25897405c9392a9c24da1345530a861639cedd",
          "message": "fix: remove DSA size limit from parallel build\n\nThe limit was too restrictive: workers ignore maintenance_work_mem\nand each buffer can hold up to tp_memtable_spill_threshold postings\n(~384 MB), so with 4 workers the 1 GB floor was easily exceeded.\nProper memory bounding requires reworking the spill threshold to\nrespect maintenance_work_mem, which is a larger follow-up.",
          "timestamp": "2026-02-17T20:24:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/ef25897405c9392a9c24da1345530a861639cedd"
        },
        "date": 1771360731996,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12165.662,
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
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "b37eaabf08782a40680bdb28ea7549f97608a229",
          "message": "fix: use min fieldnorm for BMW skip entries in parallel build (#230)\n\n## Summary\n- Fix `write_posting_blocks()` in parallel build to compute MIN\nfieldnorm (shortest doc) instead of MAX (longest doc) per block\n- The `block_max_norm` skip entry field must store the minimum fieldnorm\nso BMW computes valid score upper bounds; using maximum caused BMW to\nincorrectly skip blocks containing high-scoring short documents\n- The serial build (`segment.c`) and merge (`merge.c`) paths already\nused `min_norm` correctly — this aligns the parallel build path\n- Add `parallel_bmw` regression test that deterministically reproduces\nthe bug: medium-length docs set the BMW threshold in early blocks, then\nmixed short+long doc blocks follow where the wrong upper bound causes\nBMW to skip them\n\n## Test plan\n- [x] `parallel_bmw` test fails deterministically without fix (0 short\ndocs in top-10), passes with fix (10 short docs)\n- [x] Verified 5/5 stable passes for `bmw` + `parallel_build` tests\n- [x] Full regression suite passes (only pre-existing `binary_io`\nfailure)\n- [x] `make format-check` passes",
          "timestamp": "2026-02-18T01:49:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b37eaabf08782a40680bdb28ea7549f97608a229"
        },
        "date": 1771397051833,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12462.45,
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
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.67,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "206692f75f86c089bd1b804a742ea6af69f6337a",
          "message": "fix: prevent buffer overflow in batch merge when merge_source_init fails\n\nThe segment-walking loop used num_sources (successful inits) as its\ntermination condition, but when merge_source_init fails, num_sources\ndoesn't increment while the loop continues walking the chain. This\ncould walk past segment_count segments, overflowing the segment_pages\narray and corrupting the level count in the metapage.\n\nFix: track segments_walked separately and cap on that. Update\nsegment_count to reflect actual segments consumed so the metapage\nremainder calculation is correct.\n\nAlso zero out num_docs for sources with missing CTID arrays to prevent\na potential NULL dereference in the N-way merge.",
          "timestamp": "2026-02-18T19:34:26Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/206692f75f86c089bd1b804a742ea6af69f6337a"
        },
        "date": 1771443617053,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8902.304,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.49,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.59,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.79,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.14,
            "unit": "MB"
          }
        ]
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
          "id": "7bd2c53318e928db5e7a64ce4fa4db9c3ea77d7a",
          "message": "bench: disable parallel build in MS MARCO benchmark for deterministic segments\n\nThe benchmark should produce a deterministic segment layout regardless\nof the machine's parallelism settings, so query performance results\nare comparable across runs and branches.",
          "timestamp": "2026-02-18T19:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7bd2c53318e928db5e7a64ce4fa4db9c3ea77d7a"
        },
        "date": 1771445676980,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 7696.961,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.21,
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
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.54,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.77,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.14,
            "unit": "MB"
          }
        ]
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
          "id": "3588dc0de18d8811d44c599edc15e614cff94108",
          "message": "feat: add bm25_compact_index() and always compact after parallel build\n\nAdd tp_compact_all() which merges ALL segments at each level in a\nsingle batch, ignoring the segments_per_level threshold. This is\nused in two places:\n\n1. After parallel index build, to produce a fully compacted index\n   instead of leaving N loose L0 segments from N workers.\n\n2. Exposed as SQL function bm25_compact_index(index_name) so\n   benchmarks (or users) can force full compaction on demand.\n\nThe tp_merge_level_segments() function now accepts a max_merge\nparameter so callers can control batch size independently of the\nsegments_per_level GUC.",
          "timestamp": "2026-02-18T21:29:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3588dc0de18d8811d44c599edc15e614cff94108"
        },
        "date": 1771451073596,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 10780.361,
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
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.68,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 107.41,
            "unit": "MB"
          }
        ]
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
          "id": "6ad4ffb8f501035ede8687cafa098f8b04f38008",
          "message": "fix: detoast bm25vector in send and equality functions\n\ntpvector_send and tpvector_eq used PG_GETARG_POINTER which does not\ndetoast varlena datums. When PostgreSQL stores small bm25vectors with\n1-byte short varlena headers, VARSIZE() reads garbage (interpreting the\n1B header + data bytes as a 4B header), causing out-of-bounds reads.\n\nUnder ASAN sanitizer builds this crashes immediately when the garbage\nsize causes reads past the buffer page boundary into poisoned memory.\nWithout sanitizer, the bug is latent but produces corrupt binary output.\n\nFix by using PG_DETOAST_DATUM which ensures proper 4-byte varlena\nheaders. This matches what tpvector_out already does correctly.",
          "timestamp": "2026-02-19T01:28:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/6ad4ffb8f501035ede8687cafa098f8b04f38008"
        },
        "date": 1771466554271,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8445.085,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.54,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.8,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.13,
            "unit": "MB"
          }
        ]
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
          "id": "8fe9814a5cd20fdf2600b248a21c2ec36d8eb624",
          "message": "feat: add worker-side compaction to parallel index build\n\nWorkers now perform level-aware compaction within their BufFile during\nparallel index build, mirroring tp_maybe_compact_level from serial\nbuild. This produces a proper LSM level structure instead of dumping\nall segments at L0.\n\n- Extract merge types and helpers into merge_internal.h for reuse\n- Add merge_source_init_from_reader() for BufFile-backed segments\n- Workers track segments per level and compact when threshold reached\n- write_merged_segment_to_buffile() performs N-way merge within BufFile\n- Leader writes compacted segments at correct levels, then runs final\n  compaction on combined per-level counts",
          "timestamp": "2026-02-19T03:45:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8fe9814a5cd20fdf2600b248a21c2ec36d8eb624"
        },
        "date": 1771474288023,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8989.321,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.5,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.57,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.8,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.13,
            "unit": "MB"
          }
        ]
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
          "id": "b37eaabf08782a40680bdb28ea7549f97608a229",
          "message": "fix: use min fieldnorm for BMW skip entries in parallel build (#230)\n\n## Summary\n- Fix `write_posting_blocks()` in parallel build to compute MIN\nfieldnorm (shortest doc) instead of MAX (longest doc) per block\n- The `block_max_norm` skip entry field must store the minimum fieldnorm\nso BMW computes valid score upper bounds; using maximum caused BMW to\nincorrectly skip blocks containing high-scoring short documents\n- The serial build (`segment.c`) and merge (`merge.c`) paths already\nused `min_norm` correctly — this aligns the parallel build path\n- Add `parallel_bmw` regression test that deterministically reproduces\nthe bug: medium-length docs set the BMW threshold in early blocks, then\nmixed short+long doc blocks follow where the wrong upper bound causes\nBMW to skip them\n\n## Test plan\n- [x] `parallel_bmw` test fails deterministically without fix (0 short\ndocs in top-10), passes with fix (10 short docs)\n- [x] Verified 5/5 stable passes for `bmw` + `parallel_build` tests\n- [x] Full regression suite passes (only pre-existing `binary_io`\nfailure)\n- [x] `make format-check` passes",
          "timestamp": "2026-02-18T01:49:18Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b37eaabf08782a40680bdb28ea7549f97608a229"
        },
        "date": 1771482780055,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13164.762,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.13,
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
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.67,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "4639bf8baedd19b412fc720d7639d33927d8e726",
          "message": "feat: workers write index pages directly in two-phase parallel build\n\nEliminate single-threaded leader transcription bottleneck by having\nworkers write their segments directly to pre-allocated index pages.\n\nPhase 1: Workers scan heap, flush/compact in BufFile, report total\npages needed, signal phase1_done.\n\nLeader barrier: Sum page counts, pre-extend relation with batched\nExtendBufferedRelBy (8192 pages/batch), set atomic next_page counter,\nsignal phase2_ready.\n\nPhase 2: Workers reopen BufFile read-only, write segments to index\npages using lock-free atomic page counter, report seg_roots[].\n\nLeader finalization: Read seg_roots[] from shared memory (no BufFile\nI/O), chain segments into level lists, update metapage, compact.",
          "timestamp": "2026-02-19T19:23:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4639bf8baedd19b412fc720d7639d33927d8e726"
        },
        "date": 1771531984404,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8253.604,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.36,
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
            "value": 0.53,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.75,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.14,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771568971341,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11824.283,
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
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.62,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "2b60e589d50a7c1f08ef44604dedb8d1821af874",
          "message": "fix: fix BufFile cursor handling in worker-side compaction\n\nThree interrelated bugs in worker-side BufFile compaction caused\ncorruption when worker BufFiles exceeded 1GB (the physical segment\nsize for Postgres BufFile):\n\n1. BufFileTell ordering: write_merged_segment_to_buffile() called\n   BufFileTell AFTER build_merged_docmap(), but that function reads\n   from BufFile-backed segment readers which moves the file cursor.\n   Fix: record the base position BEFORE build_merged_docmap and\n   re-seek after.\n\n2. Interleaved reads and writes: merge source reads (via\n   posting_source_advance -> tp_segment_read) share the same BufFile\n   handle as the output writes, moving the cursor between writes.\n   Fix: introduce buffile_write_at() that seeks to the absolute\n   output position before every write.\n\n3. SEEK_END race: BufFileSeek(SEEK_END) calls FileSize() before\n   flushing the dirty write buffer, returning the on-disk size which\n   is less than the logical size. Writing there overwrites unflushed\n   data. Fix: track buffile_end explicitly in WorkerSegmentTracker\n   and use it instead of SEEK_END.\n\nAlso decomposes BufFile composite offsets (fileno, offset) before all\nBufFileSeek calls to handle multi-segment BufFiles correctly.\n\nVerified with 138M row MS MARCO v2 parallel index build (8 workers).",
          "timestamp": "2026-02-20T08:29:17Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2b60e589d50a7c1f08ef44604dedb8d1821af874"
        },
        "date": 1771605125492,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8906.804,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.24,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.35,
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
            "value": 0.53,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.13,
            "unit": "MB"
          }
        ]
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
          "id": "0541b9a41a685d05a817c764e9a3f7a373b1f001",
          "message": "fix: import worker segments as L0 and truncate unused pool pages\n\nWorker-side compaction produces segments at various levels (L1, L2, etc.)\nbut these levels are meaningless globally since each worker only sees 1/Nth\nof the data. Importing them at their worker-local levels inflates segment\ncounts (e.g. 7 L1 + 4 L0 = 11 segments vs ~4 L1 for serial build).\n\nImport all worker segments as L0 so leader-side compaction can cascade\nproperly through L0→L1→L2, matching serial build behavior.\n\nAlso truncate pre-allocated pool pages that workers never used, reducing\nindex size bloat from over-estimation.",
          "timestamp": "2026-02-20T22:38:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/0541b9a41a685d05a817c764e9a3f7a373b1f001"
        },
        "date": 1771635976589,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8583.281,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.16,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.49,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.82,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.14,
            "unit": "MB"
          }
        ]
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
          "id": "4723f0e5956dbae9483260c61abf3c7accd119ca",
          "message": "feat: unified merge sink eliminates parallel build fragmentation\n\nReplace the two-phase parallel build (workers write to BufFile then\ncopy to pre-allocated pages; leader compacts via FSM/P_NEW) with a\nsingle-phase design: workers scan+flush+compact to BufFile and exit;\nleader reopens all worker BufFiles, performs one N-way merge, and\nwrites a single contiguous segment via TpMergeSink.\n\nKey changes:\n- Add TpMergeSink abstraction (pages or BufFile backend) with\n  merge_sink_write() and merge_sink_write_at() for backpatching\n- Unify write_merged_segment() and write_merged_segment_to_buffile()\n  into write_merged_segment_to_sink() (~430 lines of duplication removed)\n- Remove Phase 2 from workers (no more phase2_cv, page pool,\n  atomic counter, pool truncation, segment linking)\n- Leader does BufFile-through merge directly to index pages,\n  producing one contiguous L1 segment with zero fragmentation\n\nNet: -509 lines. Index size should now match serial build.",
          "timestamp": "2026-02-21T03:50:01Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4723f0e5956dbae9483260c61abf3c7accd119ca"
        },
        "date": 1771646872656,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 15500.682,
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
            "value": 0.25,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.29,
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
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.63,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771654976343,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12907.415,
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
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.64,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771741571095,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13584.772,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.63,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "b15d8ad5c1b22366f4e019d4bec1f61937e46cc4",
          "message": "fix: restore parallel page writes, drop leader compaction\n\nThe previous commit moved ALL page writing to the leader via a\nsingle-threaded N-way merge, which nearly doubled build time\n(4:29 → 8:26 on CI benchmark). The fragmentation was never from\nworkers writing to pre-allocated pages — it was from the leader's\ntp_maybe_compact_level() using FSM/P_NEW which scattered pages.\n\nFix: restore two-phase parallel build where workers write BufFile\nsegments to pre-allocated contiguous pages in parallel, but skip\nthe compaction step entirely. Worker segments stay as L0 in the\ncontiguous pool and compact naturally on subsequent inserts or\nvia bm25_compact_index().\n\nKeep TpMergeSink abstraction in merge.c for worker BufFile\ncompaction and normal compaction paths.",
          "timestamp": "2026-02-23T01:48:19Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b15d8ad5c1b22366f4e019d4bec1f61937e46cc4"
        },
        "date": 1771812140068,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 8370.507,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.54,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.73,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 42.12,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771828455105,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 14837.011,
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
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.63,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "fc9553c84450fcfd14fa00623f0a23b778dc87ba",
          "message": "fix: resolve test failures on PG17 and /tmp environments (#234)\n\n## Summary\n\n- **binary_io**: Switch `COPY` to `\\copy` (client-side) so temp files\nare owned by the test runner user, avoiding `/tmp` sticky-bit permission\nissues\n- **coverage**: Use shared temp dir with 777 permissions for server-side\nfile writes; add `id` tiebreaker to queries with tied BM25 scores for\ndeterministic ordering\n- **inheritance**: Extend `is_child_partition_index()` in `hooks.c` to\nhandle regular table inheritance — PG17's planner may choose a child\ntable's index over the parent's\n\n## Test plan\n\n- [x] All 47 SQL regression tests pass locally on PG17\n- [x] `make format-check` passes\n- [x] CI passes (PG17, PG18, sanitizer builds)\n- [x] Verify inheritance test still passes on PG18 (planner behavior\ndiffers)",
          "timestamp": "2026-02-19T23:41:12Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fc9553c84450fcfd14fa00623f0a23b778dc87ba"
        },
        "date": 1771915161107,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13434.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.62,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "9ad4271373e91371e28c2afa0e1077972af65291",
          "message": "feat: disjoint TID range scans + sequential drain merge\n\nReplace shared parallel heap scan with per-worker TID range scans.\nEach worker scans a contiguous, non-overlapping range of heap blocks,\nensuring disjoint CTIDs across workers.\n\nThis enables a sequential drain fast path in cross-worker merge:\ninstead of N-way CTID comparison per posting, drain sources in order\n(source 0 fully, then source 1, etc.). This eliminates:\n- posting_source_convert_current (2 pread calls per posting)\n- find_min_posting_source (CTID comparison per posting)\n\nThese two functions accounted for ~47% of CPU time in Phase 2 of\nthe 138M-row MS-MARCO v2 parallel build.\n\nChanges:\n- Leader divides heap blocks evenly across launched workers, stores\n  ranges in shared memory, signals scan_ready atomic flag\n- Workers spin-wait on scan_ready, then open table_beginscan_tidrange\n  limited to their assigned block range\n- build_merged_docmap: concatenates sources sequentially when disjoint\n  instead of N-way CTID merge\n- write_merged_segment_to_sink: new disjoint_sources parameter enables\n  sequential drain using posting_source_advance_fast (no CTID lookup)\n- Extract FLUSH_BLOCK macro to deduplicate block write logic\n- Remove parallel table scan descriptor (no longer needed)",
          "timestamp": "2026-02-24T22:03:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/9ad4271373e91371e28c2afa0e1077972af65291"
        },
        "date": 1771971521980,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 9762.914,
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
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.53,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.76,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 41.76,
            "unit": "MB"
          }
        ]
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
          "id": "afdca64b8adf628c526b18433f8b07315d180ce8",
          "message": "fix: register snapshot in worker for PG18 tidrange scan\n\nPG18 added an assertion in heap_prepare_pagescan requiring any snapshot\nused in a heap scan to be registered or active. The leader already\nregistered the snapshot, but the worker passed an unregistered snapshot\nfrom GetTransactionSnapshot() to table_beginscan_tidrange(), tripping\nthe assertion under sanitizer builds.\n\nMirror the leader's pattern: RegisterSnapshot before scan,\nUnregisterSnapshot after table_endscan.",
          "timestamp": "2026-02-24T22:33:25Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/afdca64b8adf628c526b18433f8b07315d180ce8"
        },
        "date": 1771975256018,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 10187.164,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.23,
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
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.52,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.76,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 41.76,
            "unit": "MB"
          }
        ]
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
          "id": "a1cfff5b14cf371eae6301cab89d1e871e6fb662",
          "message": "Revert versioned shared library filename (#238)\n\n## Summary\n\n- Reverts #232 which added version numbers to the shared library\nfilename\n- The versioned binary naming scheme (e.g.,\n`pg_textsearch-1.0.0-dev.so`) caused problems in practice\n- Returns to the standard unversioned `pg_textsearch.so` naming\n\n## Testing\n\n- `make installcheck` passes (all SQL regression + shell tests)\n- `make format-check` passes",
          "timestamp": "2026-02-24T23:36:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a1cfff5b14cf371eae6301cab89d1e871e6fb662"
        },
        "date": 1772001589401,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11990.714,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.64,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "44deea684c705c68e2832796e146ccf1d8b3784b",
          "message": "fix: prevent cascading merge group segfault + dedup build helpers\n\nThree changes:\n\n1. Fix segfault in parallel build: remainder merge groups no longer\n   cascade to higher levels. worker_execute_merge_group() only\n   searches worker result arrays for source segments, not outputs\n   of earlier merge groups. When cascading groups were planned,\n   they found no sources and accessed invalid block numbers,\n   causing SIGSEGV on large tables (8.8M+ rows, 4+ workers).\n\n2. Extract tp_truncate_dead_pages() shared helper from duplicated\n   code in build.c (bm25_compact_index) and build_parallel.c\n   (post-build truncation). Walks all segment chains to find the\n   highest used block and truncates everything beyond it.\n\n3. Extract tp_link_l0_chain_head() helper from 4x duplicated\n   pattern in build.c (auto-spill, flush-and-link, spill-memtable,\n   final-spill). Also removes dead final memtable spill block\n   from serial build path (arena build never populates memtable).",
          "timestamp": "2026-02-26T01:59:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/44deea684c705c68e2832796e146ccf1d8b3784b"
        },
        "date": 1772071956851,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 12159.434,
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
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.65,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "3dc4a8fc8f6a0c63fa0b1cee190ca80207d0e27f",
          "message": "fix: compact remaining segments after parallel build for query perf\n\nWithout cascading merge groups, the parallel build can leave\na few segments at each level (e.g., 3 L1 segments from 2 full\nmerge groups + 1 remainder). This hurts query performance since\nBMW must merge across all segments.\n\nAdd tp_compact_all() after linking chains to merge remaining\nsegments into one, followed by tp_truncate_dead_pages() to\nreclaim any dead pages left by the compaction.",
          "timestamp": "2026-02-26T02:14:50Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/3dc4a8fc8f6a0c63fa0b1cee190ca80207d0e27f"
        },
        "date": 1772072886912,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 11986.335,
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
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.63,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "04f4387e2ca42e382e29105d101b8b6909cab8fe",
          "message": "fix: tp_compact_all must check all levels, not just bottom-up\n\ntp_compact_all() used break when it found a level with < 2\nsegments, assuming higher levels would also be sparse. This\nassumption is wrong after parallel build where merge groups\noutput directly at L1+, leaving L0 empty. Change break to\ncontinue so all levels are checked.\n\nThis was preventing post-build compaction from merging the\n2 L1 segments (from full + remainder merge groups) into a\nsingle L2 segment, hurting query performance.",
          "timestamp": "2026-02-26T02:31:39Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/04f4387e2ca42e382e29105d101b8b6909cab8fe"
        },
        "date": 1772073960056,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 10198.891,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 1 Token Query (p50)",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.66,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "44deea684c705c68e2832796e146ccf1d8b3784b",
          "message": "fix: prevent cascading merge group segfault + dedup build helpers\n\nThree changes:\n\n1. Fix segfault in parallel build: remainder merge groups no longer\n   cascade to higher levels. worker_execute_merge_group() only\n   searches worker result arrays for source segments, not outputs\n   of earlier merge groups. When cascading groups were planned,\n   they found no sources and accessed invalid block numbers,\n   causing SIGSEGV on large tables (8.8M+ rows, 4+ workers).\n\n2. Extract tp_truncate_dead_pages() shared helper from duplicated\n   code in build.c (bm25_compact_index) and build_parallel.c\n   (post-build truncation). Walks all segment chains to find the\n   highest used block and truncates everything beyond it.\n\n3. Extract tp_link_l0_chain_head() helper from 4x duplicated\n   pattern in build.c (auto-spill, flush-and-link, spill-memtable,\n   final-spill). Also removes dead final memtable spill block\n   from serial build path (arena build never populates memtable).",
          "timestamp": "2026-02-26T01:59:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/44deea684c705c68e2832796e146ccf1d8b3784b"
        },
        "date": 1772075837157,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (100.0K docs) - Index Build Time",
            "value": 13530.433,
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
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 4 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 5 Token Query (p50)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 7 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - 8+ Token Query (p50)",
            "value": 0.7,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Latency (p50, ms)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (100.0K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "4a9aaf2a430249d4b20d0c4045b4ddf6c5a8bebb",
          "message": "feat: require shared_preload_libraries for pg_textsearch (#235)\n\n## Summary\n\n- Enforce loading via `shared_preload_libraries` to prevent fatal\n`ShmemIndex entry size is wrong` errors when the .so is replaced during\ndeployment without a server restart\n- Remove lazy init fallbacks in registry.c that were the previous\nworkaround for not requiring preloading\n- Update all test infrastructure (Makefile, shell scripts, CI workflows)\nto configure `shared_preload_libraries` with the versioned library name\n\n## Context\n\npg_textsearch uses Postgres shared memory (`ShmemInitStruct`) for its\nindex registry. Without `shared_preload_libraries`, each backend loads\nthe .so independently via `dlopen`. If the .so file is replaced on disk\n(e.g., during a deployment) without restarting Postgres, different\nbackends get different code, causing a fatal `ShmemIndex entry size is\nwrong` error. Requiring `shared_preload_libraries` ensures all backends\nuse the same .so (inherited from the postmaster via fork), eliminating\nthis class of bugs.\n\n## Testing\n\n- All 47 SQL regression tests pass\n- `make format-check` passes\n- Error message verified when extension not in shared_preload_libraries",
          "timestamp": "2026-02-25T23:21:16Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4a9aaf2a430249d4b20d0c4045b4ddf6c5a8bebb"
        },
        "date": 1772087743706,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 14461.655,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.66,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "f1384f93b557bdea96e1de131c56195afb2508f3",
          "message": "feat: detect stale binary after upgrade via library version check (#241)\n\n## Summary\n\n- Add `pg_textsearch.library_version` read-only GUC set at library load\ntime from a compile-time version define\n- Check the GUC in `CREATE EXTENSION` and `ALTER EXTENSION UPDATE` SQL\nscripts, failing with a clear error if the loaded library is missing or\ndoesn't match the expected version\n- Catches the case where a user runs `make install` but forgets to\nrestart the server\n\n## Changes\n\n- **Makefile**: Extract `EXTVERSION` from `pg_textsearch.control`, pass\nas `-DPG_TEXTSEARCH_VERSION`\n- **src/mod.c**: Register `PGC_INTERNAL` string GUC in `_PG_init`\n- **sql/pg_textsearch--1.0.0-dev.sql**: Version check DO block before\nany CREATE statements\n- **sql/pg_textsearch--0.5.0--1.0.0-dev.sql**: Same version check in\nupgrade script\n- **test/sql/basic.sql**: Regression test for the GUC\n\n## Testing\n\n- `SHOW pg_textsearch.library_version` returns `1.0.0-dev` after restart\n- All 47 regression tests pass",
          "timestamp": "2026-02-26T19:19:22Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f1384f93b557bdea96e1de131c56195afb2508f3"
        },
        "date": 1772173773007,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 14472.457,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.65,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772243898266,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 12324.783,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.65,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "86ec2fd148ae2d2bc22a6be526183bb45476ea92",
          "message": "feat: rewrite index build with arena allocator and parallel page pool (#231)\n\n## Summary\n\n- **Arena allocator + EXPULL posting lists**: Replace DSA/dshash shared\nmemory with process-local arena (1MB slab bump allocator, 32-bit\naddresses) and exponential unrolled linked lists. Eliminates DSA\nfragmentation, dshash lock overhead, and posting list reallocation\ncopies.\n- **TpBuildContext**: New per-build context with arena, HTAB term\ndictionary, and flat doc arrays. Budget-controlled flushing using\n`maintenance_work_mem`. Used by both serial and parallel CREATE INDEX.\n- **Two-phase parallel build**: Workers scan disjoint TID ranges, flush\nto BufFile temp segments, compact within BufFile. Leader plans\ncross-worker merge groups. Workers then COPY non-merged segments to\npre-allocated index pages and work-steal merge groups via atomic\ncounter.\n- **Unified merge sink**: `TpMergeSink` abstraction writes merged\nsegments to either index pages or BufFile, eliminating separate code\npaths. Disjoint source fast path enables sequential drain merge without\nCTID comparison.\n- **bm25_force_merge()**: New SQL function to force-merge all segments\ninto one (à la Lucene's `forceMerge(1)`).\n- **TOAST fix**: `tpvector_send()` and `tpvector_eq()` now properly\ndetoast values.\n\n## Motivation\n\nThe previous parallel build used DSA shared memory with dshash for term\ndictionaries and double-buffered memtable swapping. This caused:\n- DSA fragmentation under large workloads (100GB+ OOM on MS MARCO v2)\n- dshash lock contention between workers\n- Complex N-way merge in the leader\n- ~2200 lines of intricate coordination code\n\nThe new approach is simpler and more memory-efficient: each worker gets\na local arena with a budget, flushes segments to BufFile temp files, and\nthe leader coordinates a two-phase write to pre-allocated index pages.\nCross-worker compaction merges segments that span worker boundaries.\n\n## Benchmark results (MS MARCO, 8.8M passages)\n\n| Metric | main | System X | This PR | vs main |\n|--------|------|----------|---------|---------|\n| Build Time | 278.5s | 146.2s | 228.1s | **19% faster** |\n| Index Size | 1240 MB | 1497 MB | 1360 MB | +10% (9% smaller than\nSystem X) |\n| Query P50 | 3.74ms | — | 6.08ms | +63% |\n| Query Avg | 4.49ms | — | 7.55ms | +68% |\n\n**Note on query latency**: The parallel build produces 2-3 L1 segments\ninstead of a single fully-compacted segment (as on main). The BMW query\noptimizer must scan multiple segments, which increases latency. A\nfollow-up will add post-build `bm25_force_merge()` that merges into\npre-allocated pages (using the existing BufFile→index-page COPY path) to\neliminate this regression.\n\n## Key changes\n\n### New files\n- `src/memtable/arena.c/h` — 1MB page-based bump allocator with 32-bit\naddresses (12-bit page + 20-bit offset), max 4GB\n- `src/memtable/expull.c/h` — Exponential unrolled linked list for\nposting lists (blocks grow 32B→32KB, 7-byte packed entries)\n- `src/am/build_context.c/h` — Arena-based build context replacing DSA\nmemtable during builds\n- `src/segment/merge_internal.h` — Exposes merge internals for\nbuild_parallel.c\n\n### Major modifications\n- `src/am/build.c` — Serial build uses TpBuildContext; added\n`bm25_force_merge()` SQL function; extracted `tp_link_l0_chain_head()`\nand `tp_truncate_dead_pages()` shared helpers\n- `src/am/build_parallel.c` — Two-phase parallel build with disjoint TID\nranges, BufFile worker segments, cross-worker merge groups, and atomic\npage pool\n- `src/segment/merge.c` — Unified TpMergeSink abstraction; disjoint\nsource fast path; streaming docmap merge\n- `src/segment/segment.c` — Added parallel writer init and atomic page\nallocation\n\n### Bug fixes\n- `src/types/vector.c` — Use `PG_DETOAST_DATUM` for TOAST safety in\nsend/eq functions\n- `src/am/scan.c` — Removed `TP_MAX_BLOCK_NUMBER` guard (large\nparallel-built indexes can exceed 1M blocks)\n- `sql/pg_textsearch--0.5.0--1.0.0-dev.sql` — Added `bm25_force_merge`\nto upgrade path\n\n## Test plan\n\n- [x] All 47 regression tests pass (PG17 + PG18)\n- [x] `parallel_build` test validates serial, 1-worker, 2-worker,\n4-worker builds with query correctness\n- [x] `parallel_bmw` test validates Block-Max WAND with parallel-built\nindexes\n- [x] `make format-check` passes\n- [x] CI green: gcc/clang × PG17/PG18, sanitizers\n- [x] MS MARCO benchmark passes (8.8M passages, build + query\nvalidation)\n- [x] Rebased onto current main (shared_preload_libraries, TOCTOU fix,\nvalidation)",
          "timestamp": "2026-02-27T21:46:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/86ec2fd148ae2d2bc22a6be526183bb45476ea92"
        },
        "date": 1772259438920,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 11868.265,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.12,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.19,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.26,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.65,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "86ec2fd148ae2d2bc22a6be526183bb45476ea92",
          "message": "feat: rewrite index build with arena allocator and parallel page pool (#231)\n\n## Summary\n\n- **Arena allocator + EXPULL posting lists**: Replace DSA/dshash shared\nmemory with process-local arena (1MB slab bump allocator, 32-bit\naddresses) and exponential unrolled linked lists. Eliminates DSA\nfragmentation, dshash lock overhead, and posting list reallocation\ncopies.\n- **TpBuildContext**: New per-build context with arena, HTAB term\ndictionary, and flat doc arrays. Budget-controlled flushing using\n`maintenance_work_mem`. Used by both serial and parallel CREATE INDEX.\n- **Two-phase parallel build**: Workers scan disjoint TID ranges, flush\nto BufFile temp segments, compact within BufFile. Leader plans\ncross-worker merge groups. Workers then COPY non-merged segments to\npre-allocated index pages and work-steal merge groups via atomic\ncounter.\n- **Unified merge sink**: `TpMergeSink` abstraction writes merged\nsegments to either index pages or BufFile, eliminating separate code\npaths. Disjoint source fast path enables sequential drain merge without\nCTID comparison.\n- **bm25_force_merge()**: New SQL function to force-merge all segments\ninto one (à la Lucene's `forceMerge(1)`).\n- **TOAST fix**: `tpvector_send()` and `tpvector_eq()` now properly\ndetoast values.\n\n## Motivation\n\nThe previous parallel build used DSA shared memory with dshash for term\ndictionaries and double-buffered memtable swapping. This caused:\n- DSA fragmentation under large workloads (100GB+ OOM on MS MARCO v2)\n- dshash lock contention between workers\n- Complex N-way merge in the leader\n- ~2200 lines of intricate coordination code\n\nThe new approach is simpler and more memory-efficient: each worker gets\na local arena with a budget, flushes segments to BufFile temp files, and\nthe leader coordinates a two-phase write to pre-allocated index pages.\nCross-worker compaction merges segments that span worker boundaries.\n\n## Benchmark results (MS MARCO, 8.8M passages)\n\n| Metric | main | System X | This PR | vs main |\n|--------|------|----------|---------|---------|\n| Build Time | 278.5s | 146.2s | 228.1s | **19% faster** |\n| Index Size | 1240 MB | 1497 MB | 1360 MB | +10% (9% smaller than\nSystem X) |\n| Query P50 | 3.74ms | — | 6.08ms | +63% |\n| Query Avg | 4.49ms | — | 7.55ms | +68% |\n\n**Note on query latency**: The parallel build produces 2-3 L1 segments\ninstead of a single fully-compacted segment (as on main). The BMW query\noptimizer must scan multiple segments, which increases latency. A\nfollow-up will add post-build `bm25_force_merge()` that merges into\npre-allocated pages (using the existing BufFile→index-page COPY path) to\neliminate this regression.\n\n## Key changes\n\n### New files\n- `src/memtable/arena.c/h` — 1MB page-based bump allocator with 32-bit\naddresses (12-bit page + 20-bit offset), max 4GB\n- `src/memtable/expull.c/h` — Exponential unrolled linked list for\nposting lists (blocks grow 32B→32KB, 7-byte packed entries)\n- `src/am/build_context.c/h` — Arena-based build context replacing DSA\nmemtable during builds\n- `src/segment/merge_internal.h` — Exposes merge internals for\nbuild_parallel.c\n\n### Major modifications\n- `src/am/build.c` — Serial build uses TpBuildContext; added\n`bm25_force_merge()` SQL function; extracted `tp_link_l0_chain_head()`\nand `tp_truncate_dead_pages()` shared helpers\n- `src/am/build_parallel.c` — Two-phase parallel build with disjoint TID\nranges, BufFile worker segments, cross-worker merge groups, and atomic\npage pool\n- `src/segment/merge.c` — Unified TpMergeSink abstraction; disjoint\nsource fast path; streaming docmap merge\n- `src/segment/segment.c` — Added parallel writer init and atomic page\nallocation\n\n### Bug fixes\n- `src/types/vector.c` — Use `PG_DETOAST_DATUM` for TOAST safety in\nsend/eq functions\n- `src/am/scan.c` — Removed `TP_MAX_BLOCK_NUMBER` guard (large\nparallel-built indexes can exceed 1M blocks)\n- `sql/pg_textsearch--0.5.0--1.0.0-dev.sql` — Added `bm25_force_merge`\nto upgrade path\n\n## Test plan\n\n- [x] All 47 regression tests pass (PG17 + PG18)\n- [x] `parallel_build` test validates serial, 1-worker, 2-worker,\n4-worker builds with query correctness\n- [x] `parallel_bmw` test validates Block-Max WAND with parallel-built\nindexes\n- [x] `make format-check` passes\n- [x] CI green: gcc/clang × PG17/PG18, sanitizers\n- [x] MS MARCO benchmark passes (8.8M passages, build + query\nvalidation)\n- [x] Rebased onto current main (shared_preload_libraries, TOCTOU fix,\nvalidation)",
          "timestamp": "2026-02-27T21:46:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/86ec2fd148ae2d2bc22a6be526183bb45476ea92"
        },
        "date": 1772346300400,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 12429.588,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.66,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "93170a2d07de18d890efa12b0c79489e0e6669dc",
          "message": "feat: leader-only merge for parallel index build\n\nReplace the two-phase parallel build (worker compaction + work-stealing\nmerge groups) with a simpler architecture: workers flush L0 segments to\nBufFiles without compaction, then the leader performs a single N-way\nmerge of all segments directly to paged storage.\n\nThis produces a single segment per index build, which is optimal for\nquery performance (no multi-segment scanning needed).\n\nRemoves ~800 lines of complexity: worker_maybe_compact_level,\nplan_merge_groups, compute_total_pages_needed,\nworker_execute_merge_group, write_temp_segment_to_index_parallel,\nand all Phase 2 worker code (COPY + work-steal).",
          "timestamp": "2026-03-01T17:46:57Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/93170a2d07de18d890efa12b0c79489e0e6669dc"
        },
        "date": 1772388124804,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 10085.299,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.11,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.27,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.67,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.29,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "a72f481e17e0c684b7bd37be3da4067674ddec35",
          "message": "fix: address code review issues in leader-only merge\n\n- Move FlushRelationBuffers before metapage update for consistency\n  with merge.c pattern (segment data durable before metadata)\n- Use merge_source_close() instead of manual pfree for cleanup\n- NULL readers[] slot when source takes ownership to prevent\n  double-close\n- Remove dead segment_count field from TpParallelWorkerResult",
          "timestamp": "2026-03-01T18:44:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/a72f481e17e0c684b7bd37be3da4067674ddec35"
        },
        "date": 1772394618412,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 12433.032,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.21,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.44,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.66,
            "unit": "MB"
          }
        ]
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
          "id": "e8601e2c78ac4f757e3255744f2a83360ed6b20b",
          "message": "fix: add coverage gate to block PRs on coverage reduction (#245)\n\n## Summary\n- The coverage job collected data and uploaded to Codecov but never\nenforced\n  thresholds — it always succeeded regardless of coverage values\n- Codecov status checks (`codecov/project`, `codecov/patch`) weren't\nappearing\n  on PRs, likely due to a missing/invalid `CODECOV_TOKEN`, and\n  `fail_ci_if_error` was `false` so the upload failure was silent\n- Branch protection had an empty required checks list\n(`all-tests-passed` was\n  not listed), so CI failures couldn't block merges — now fixed\n\n### Changes\n- Add in-workflow coverage gate to both `ci.yml` and `coverage.yml`\nthat:\n  - Enforces a minimum 85% line coverage threshold\n  - Caches the main branch baseline via `actions/cache`\n- Compares PR coverage against baseline, fails if it drops by more than\n1%\n- Change `fail_ci_if_error` from `false` to `true` on Codecov upload\n- Added `all-tests-passed` to branch protection required status checks\n\n### How the baseline works\n- On pushes to `main`, the coverage percentage is saved to GitHub\nActions\n  cache with key `coverage-baseline-<sha>`\n- On PRs, the most recent main branch cache is restored and compared\nagainst\n  the PR's coverage\n- First run after this merges will skip the reduction check (no baseline\nyet)\n  and establish the initial baseline\n\n## Testing\n- CI on this PR validates the gate runs (no baseline exists yet, so the\nreduction check was skipped, but the minimum threshold check ran and\npassed)\n- After merging, the next PR that reduces coverage by >1% will fail",
          "timestamp": "2026-03-02T04:13:08Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e8601e2c78ac4f757e3255744f2a83360ed6b20b"
        },
        "date": 1772432878382,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 12259.971,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.36,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.68,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.68,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772519937909,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia (99.9K docs) - Index Build Time",
            "value": 12478.594,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 1 Token Query (p50)",
            "value": 0.14,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 2 Token Query (p50)",
            "value": 0.2,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 4 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 5 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 7 Token Query (p50)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - 8+ Token Query (p50)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Latency (p50, ms)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Weighted Throughput (avg ms/query)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia (99.9K docs) - Index Size",
            "value": 38.68,
            "unit": "MB"
          }
        ]
      }
    ],
    "cranfield_gin Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245809571,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_gin - Index Build Time",
            "value": 36.241,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin - Index Size",
            "value": 0.82,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521746251,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_gin - Index Build Time",
            "value": 43.625,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin - Index Size",
            "value": 0.82,
            "unit": "MB"
          }
        ]
      }
    ],
    "msmarco_gin Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245811742,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_gin - Index Build Time",
            "value": 139500.845,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin - Index Size",
            "value": 824.93,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521748216,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_gin - Index Build Time",
            "value": 127369.894,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin - Index Size",
            "value": 824.93,
            "unit": "MB"
          }
        ]
      }
    ],
    "wikipedia_gin Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245813121,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_gin - Index Build Time",
            "value": 3785.511,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin - Index Size",
            "value": 43.12,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521749721,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_gin - Index Build Time",
            "value": 3616.159,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin - Index Size",
            "value": 43.17,
            "unit": "MB"
          }
        ]
      }
    ],
    "cranfield_gin_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245814620,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_gin_insert - Index Build Time",
            "value": 0.465,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin_insert - Insert Time",
            "value": 261.705,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin_insert - Index Size",
            "value": 2.17,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521751144,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_gin_insert - Index Build Time",
            "value": 0.647,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin_insert - Insert Time",
            "value": 295.815,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin_insert - Index Size",
            "value": 2.17,
            "unit": "MB"
          }
        ]
      }
    ],
    "msmarco_gin_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245816017,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_gin_insert - Index Build Time",
            "value": 0.559,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin_insert - Insert Time",
            "value": 662166.181,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin_insert - Index Size",
            "value": 972.11,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521752975,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_gin_insert - Index Build Time",
            "value": 0.644,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin_insert - Insert Time",
            "value": 705494.204,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin_insert - Index Size",
            "value": 972.11,
            "unit": "MB"
          }
        ]
      }
    ],
    "wikipedia_gin_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245817541,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_gin_insert - Index Build Time",
            "value": 0.445,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin_insert - Insert Time",
            "value": 22572.737,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin_insert - Index Size",
            "value": 42.09,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521754460,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_gin_insert - Index Build Time",
            "value": 0.423,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin_insert - Insert Time",
            "value": 24187.436,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin_insert - Index Size",
            "value": 42.08,
            "unit": "MB"
          }
        ]
      }
    ],
    "cranfield_gin_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245819018,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_gin_concurrent - Index Build Time",
            "value": 0.391,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin_concurrent - Concurrent Insert Time",
            "value": 247.108994,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521755902,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_gin_concurrent - Index Build Time",
            "value": 0.481,
            "unit": "ms"
          },
          {
            "name": "cranfield_gin_concurrent - Concurrent Insert Time",
            "value": 297.529928,
            "unit": "ms"
          }
        ]
      }
    ],
    "msmarco_gin_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245820542,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_gin_concurrent - Index Build Time",
            "value": 0.393,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin_concurrent - Concurrent Insert Time",
            "value": 989768.766694,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521757448,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_gin_concurrent - Index Build Time",
            "value": 0.598,
            "unit": "ms"
          },
          {
            "name": "msmarco_gin_concurrent - Concurrent Insert Time",
            "value": 1265984.309568,
            "unit": "ms"
          }
        ]
      }
    ],
    "wikipedia_gin_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772245822114,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_gin_concurrent - Index Build Time",
            "value": 0.443,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin_concurrent - Concurrent Insert Time",
            "value": 16952.450409,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772521759359,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_gin_concurrent - Index Build Time",
            "value": 0.586,
            "unit": "ms"
          },
          {
            "name": "wikipedia_gin_concurrent - Concurrent Insert Time",
            "value": 21862.503338,
            "unit": "ms"
          }
        ]
      }
    ],
    "systemx_cranfield Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246384945,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_cranfield (1.4K docs) - Index Build Time",
            "value": 118.047,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield (1.4K docs) - Throughput (avg ms/query)",
            "value": 0.92,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield (1.4K docs) - Index Size",
            "value": 3.25,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522049040,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_cranfield (1.4K docs) - Index Build Time",
            "value": 116.776,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield (1.4K docs) - Throughput (avg ms/query)",
            "value": 0.89,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield (1.4K docs) - Index Size",
            "value": 3.25,
            "unit": "MB"
          }
        ]
      }
    ],
    "systemx_msmarco Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246387876,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_msmarco (8.8M docs) - Index Build Time",
            "value": 145087.173,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 18.76,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 18.82,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 25.22,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 26.74,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 29.59,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 36.81,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 37.11,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 45,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - Throughput (avg ms/query)",
            "value": 31.61,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - Index Size",
            "value": 1499.17,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522051051,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_msmarco (8.8M docs) - Index Build Time",
            "value": 143165.465,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 1 Token Query (p50)",
            "value": 20.29,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 2 Token Query (p50)",
            "value": 20.35,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 3 Token Query (p50)",
            "value": 25.99,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 4 Token Query (p50)",
            "value": 28.17,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 5 Token Query (p50)",
            "value": 31.07,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 6 Token Query (p50)",
            "value": 38.49,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 7 Token Query (p50)",
            "value": 39.23,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - 8+ Token Query (p50)",
            "value": 48.1,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - Throughput (avg ms/query)",
            "value": 33.55,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco (8.8M docs) - Index Size",
            "value": 1506.38,
            "unit": "MB"
          }
        ]
      }
    ],
    "systemx_wikipedia Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246389980,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_wikipedia (100.0K docs) - Index Build Time",
            "value": 4539.405,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia (100.0K docs) - Index Size",
            "value": 59.5,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522052469,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_wikipedia (100.0K docs) - Index Build Time",
            "value": 4448.415,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia (100.0K docs) - Index Size",
            "value": 58.26,
            "unit": "MB"
          }
        ]
      }
    ],
    "systemx_cranfield_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246392059,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_cranfield_insert - Index Build Time",
            "value": 8.985,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_insert - Insert Time",
            "value": 65.778,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_insert - Throughput (avg ms/query)",
            "value": 95.27,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_insert - Index Size",
            "value": 3.05,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522053899,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_cranfield_insert - Index Build Time",
            "value": 8.325,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_insert - Insert Time",
            "value": 67.871,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_insert - Throughput (avg ms/query)",
            "value": 93.88,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_insert - Index Size",
            "value": 3.05,
            "unit": "MB"
          }
        ]
      }
    ],
    "systemx_msmarco_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246394131,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Index Build Time",
            "value": 4262.689,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Insert Time",
            "value": 276779.438,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 1 Token Query (p50)",
            "value": 117.46,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 2 Token Query (p50)",
            "value": 116.53,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 3 Token Query (p50)",
            "value": 122.2,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 4 Token Query (p50)",
            "value": 121.3,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 5 Token Query (p50)",
            "value": 104.29,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 6 Token Query (p50)",
            "value": 111.28,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 7 Token Query (p50)",
            "value": 111.79,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 8+ Token Query (p50)",
            "value": 117.24,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Throughput (avg ms/query)",
            "value": 103.17,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Index Size",
            "value": 1032.78,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522055369,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Index Build Time",
            "value": 4154.868,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Insert Time",
            "value": 269189.146,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 1 Token Query (p50)",
            "value": 113.15,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 2 Token Query (p50)",
            "value": 114.32,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 3 Token Query (p50)",
            "value": 119.69,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 4 Token Query (p50)",
            "value": 114.07,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 5 Token Query (p50)",
            "value": 101.78,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 6 Token Query (p50)",
            "value": 108.29,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 7 Token Query (p50)",
            "value": 109.5,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - 8+ Token Query (p50)",
            "value": 116.94,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Throughput (avg ms/query)",
            "value": 102.19,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_insert (8.8M docs) - Index Size",
            "value": 1033.28,
            "unit": "MB"
          }
        ]
      }
    ],
    "systemx_wikipedia_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246396247,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_wikipedia_insert (100.0K docs) - Index Build Time",
            "value": 56.342,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_insert (100.0K docs) - Insert Time",
            "value": 10332.038,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_insert (100.0K docs) - Index Size",
            "value": 34.91,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522056805,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_wikipedia_insert (100.0K docs) - Index Build Time",
            "value": 62.052,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_insert (100.0K docs) - Insert Time",
            "value": 10254.886,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_insert (100.0K docs) - Index Size",
            "value": 34.94,
            "unit": "MB"
          }
        ]
      }
    ],
    "systemx_cranfield_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246398261,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_cranfield_concurrent - Index Build Time",
            "value": 5.427,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_concurrent - Concurrent Insert Time",
            "value": 224.855378,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522058234,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_cranfield_concurrent - Index Build Time",
            "value": 5.625,
            "unit": "ms"
          },
          {
            "name": "systemx_cranfield_concurrent - Concurrent Insert Time",
            "value": 205.353645,
            "unit": "ms"
          }
        ]
      }
    ],
    "systemx_msmarco_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246400257,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_msmarco_concurrent - Index Build Time",
            "value": 6.462,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - Concurrent Insert Time",
            "value": 1259279.619291,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 1 Token Query (p50)",
            "value": 82.55,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 2 Token Query (p50)",
            "value": 82.47,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 3 Token Query (p50)",
            "value": 92.11,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 4 Token Query (p50)",
            "value": 95.59,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 5 Token Query (p50)",
            "value": 99.03,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 6 Token Query (p50)",
            "value": 108.11,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 7 Token Query (p50)",
            "value": 109.28,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 8+ Token Query (p50)",
            "value": 120.08,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - Throughput (avg ms/query)",
            "value": 100.16,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522059669,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_msmarco_concurrent - Index Build Time",
            "value": 6.849,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - Insert Time",
            "value": 0.634,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - Concurrent Insert Time",
            "value": 1146252.905451,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 1 Token Query (p50)",
            "value": 81.42,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 2 Token Query (p50)",
            "value": 84.89,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 3 Token Query (p50)",
            "value": 90.02,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 4 Token Query (p50)",
            "value": 92.72,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 5 Token Query (p50)",
            "value": 95.91,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 6 Token Query (p50)",
            "value": 106.41,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 7 Token Query (p50)",
            "value": 106.45,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - 8+ Token Query (p50)",
            "value": 118.02,
            "unit": "ms"
          },
          {
            "name": "systemx_msmarco_concurrent - Throughput (avg ms/query)",
            "value": 100.08,
            "unit": "ms"
          }
        ]
      }
    ],
    "systemx_wikipedia_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246402300,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_wikipedia_concurrent - Index Build Time",
            "value": 6.374,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_concurrent - Concurrent Insert Time",
            "value": 14372.820465,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522060973,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "systemx_wikipedia_concurrent - Index Build Time",
            "value": 6.901,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_concurrent - Insert Time",
            "value": 30.793,
            "unit": "ms"
          },
          {
            "name": "systemx_wikipedia_concurrent - Concurrent Insert Time",
            "value": 13505.940156,
            "unit": "ms"
          }
        ]
      }
    ],
    "cranfield_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246658024,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_insert (0 docs) - Index Build Time",
            "value": 1.83,
            "unit": "ms"
          },
          {
            "name": "cranfield_insert (0 docs) - Insert Time",
            "value": 344.72,
            "unit": "ms"
          },
          {
            "name": "cranfield_insert (0 docs) - Throughput (avg ms/query)",
            "value": 2.35,
            "unit": "ms"
          },
          {
            "name": "cranfield_insert (0 docs) - Index Size",
            "value": 0.9,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522812038,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_insert (0 docs) - Index Build Time",
            "value": 1.771,
            "unit": "ms"
          },
          {
            "name": "cranfield_insert (0 docs) - Insert Time",
            "value": 353.71,
            "unit": "ms"
          },
          {
            "name": "cranfield_insert (0 docs) - Throughput (avg ms/query)",
            "value": 2.32,
            "unit": "ms"
          },
          {
            "name": "cranfield_insert (0 docs) - Index Size",
            "value": 0.9,
            "unit": "MB"
          }
        ]
      }
    ],
    "msmarco_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246660153,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_insert (0 docs) - Index Build Time",
            "value": 1.394,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Insert Time",
            "value": 595546.233,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 1 Token Query (p50)",
            "value": 5.72,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 2 Token Query (p50)",
            "value": 6.37,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 3 Token Query (p50)",
            "value": 11.04,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 4 Token Query (p50)",
            "value": 14.16,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 5 Token Query (p50)",
            "value": 17.41,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 6 Token Query (p50)",
            "value": 23.66,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 7 Token Query (p50)",
            "value": 28.37,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 8+ Token Query (p50)",
            "value": 39.51,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Weighted Latency (p50, ms)",
            "value": 13.37,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 14.68,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Index Size",
            "value": 1491.82,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522814300,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_insert (0 docs) - Index Build Time",
            "value": 1.451,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Insert Time",
            "value": 604122.563,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 1 Token Query (p50)",
            "value": 7.8,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 2 Token Query (p50)",
            "value": 7.65,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 3 Token Query (p50)",
            "value": 15.99,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 4 Token Query (p50)",
            "value": 18.62,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 5 Token Query (p50)",
            "value": 21.78,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 6 Token Query (p50)",
            "value": 27.68,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 7 Token Query (p50)",
            "value": 33.59,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - 8+ Token Query (p50)",
            "value": 43.31,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Weighted Latency (p50, ms)",
            "value": 17.35,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 19.03,
            "unit": "ms"
          },
          {
            "name": "msmarco_insert (0 docs) - Index Size",
            "value": 1491.82,
            "unit": "MB"
          }
        ]
      }
    ],
    "wikipedia_insert Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246661732,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_insert (0 docs) - Index Build Time",
            "value": 1.412,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Insert Time",
            "value": 22047.559,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 1 Token Query (p50)",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 2 Token Query (p50)",
            "value": 0.23,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 3 Token Query (p50)",
            "value": 0.28,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 4 Token Query (p50)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 5 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 6 Token Query (p50)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 7 Token Query (p50)",
            "value": 0.49,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 8+ Token Query (p50)",
            "value": 0.69,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Weighted Latency (p50, ms)",
            "value": 0.31,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Index Size",
            "value": 49.64,
            "unit": "MB"
          }
        ]
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522815906,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_insert (0 docs) - Index Build Time",
            "value": 1.584,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Insert Time",
            "value": 21446.541,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 1 Token Query (p50)",
            "value": 0.15,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 2 Token Query (p50)",
            "value": 0.22,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 3 Token Query (p50)",
            "value": 0.3,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 4 Token Query (p50)",
            "value": 0.34,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 5 Token Query (p50)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 6 Token Query (p50)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 7 Token Query (p50)",
            "value": 0.49,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - 8+ Token Query (p50)",
            "value": 0.67,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Weighted Latency (p50, ms)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "wikipedia_insert (0 docs) - Index Size",
            "value": 49.63,
            "unit": "MB"
          }
        ]
      }
    ],
    "cranfield_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246663280,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_concurrent (0 docs) - Index Build Time",
            "value": 1.464,
            "unit": "ms"
          },
          {
            "name": "cranfield_concurrent (0 docs) - Concurrent Insert Time",
            "value": 585.343171,
            "unit": "ms"
          },
          {
            "name": "cranfield_concurrent (0 docs) - Throughput (avg ms/query)",
            "value": 2.2,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522817592,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "cranfield_concurrent (0 docs) - Index Build Time",
            "value": 1.349,
            "unit": "ms"
          },
          {
            "name": "cranfield_concurrent (0 docs) - Concurrent Insert Time",
            "value": 604.958699,
            "unit": "ms"
          },
          {
            "name": "cranfield_concurrent (0 docs) - Throughput (avg ms/query)",
            "value": 2.17,
            "unit": "ms"
          }
        ]
      }
    ],
    "msmarco_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246664795,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_concurrent (0 docs) - Index Build Time",
            "value": 1.425,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Concurrent Insert Time",
            "value": 2266602.583581,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 1 Token Query (p50)",
            "value": 1.2,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 2 Token Query (p50)",
            "value": 4.01,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 3 Token Query (p50)",
            "value": 7.95,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 4 Token Query (p50)",
            "value": 10.01,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 5 Token Query (p50)",
            "value": 17.06,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 6 Token Query (p50)",
            "value": 22.13,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 7 Token Query (p50)",
            "value": 31.66,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 8+ Token Query (p50)",
            "value": 45.6,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Weighted Latency (p50, ms)",
            "value": 10.84,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 13.62,
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
          "id": "4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b",
          "message": "fix: resolve all compiler warnings in extension source (#246)\n\n## Summary\n\n- Remove unused variables in `build.c` (`metabuf`, `metapage`, `metap`,\n`snapshot`)\n- Add missing prototype for `tp_extract_terms_from_tsvector` to `am.h`\n(removes `-Wmissing-prototypes` and the `extern` in `build_parallel.c`)\n- Silence unused parameter warnings with `(void)` casts in dshash\ncallbacks (`posting.c`, `stringtable.c`) and parallel build estimation\n(`build_parallel.c`)\n- Remove excess initializer elements in `relopt_parse_elt` (`handler.c`)\n— PG17 struct has 3 fields, not 4\n- Fix shadow variable in `score.c` (inner `int i` shadowed\nfunction-scope `i`)\n- Initialize `min_page`/`min_offset` to suppress `-Wmaybe-uninitialized`\n(`merge.c`)\n- Suppress `-Wpacked-not-aligned` for intentionally packed on-disk\nstructs (`segment.h`) with GCC-only diagnostic pragmas\n- Fix `-Waddress-of-packed-member` in `scan.c` by using local\n`ItemPointerData` + `memcpy` instead of taking address of packed member\ndirectly\n- Suppress `-Wclobbered` false positives from GCC inlining functions\ninto PG_TRY callers (`query.c`, `state.c`)\n\nAfter this change, `make -j` produces zero source-level warnings (only\nthe expected pgxs Makefile recipe override notice).\n\n## Test plan\n\n- [x] `make format-check` passes\n- [x] `make clean && make -j$(nproc)` produces no source warnings\n- [x] All 48 SQL regression tests pass",
          "timestamp": "2026-03-02T23:53:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/4f2d2717a3f8da4f87ab14513cc65e7d243e5a2b"
        },
        "date": 1772522819191,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "msmarco_concurrent (0 docs) - Index Build Time",
            "value": 1.367,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Insert Time",
            "value": 3.003,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Concurrent Insert Time",
            "value": 2333941.609438,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 1 Token Query (p50)",
            "value": 1.04,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 2 Token Query (p50)",
            "value": 3.85,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 3 Token Query (p50)",
            "value": 7.36,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 4 Token Query (p50)",
            "value": 9.36,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 5 Token Query (p50)",
            "value": 16.39,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 6 Token Query (p50)",
            "value": 21.75,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 7 Token Query (p50)",
            "value": 31.1,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - 8+ Token Query (p50)",
            "value": 45.72,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Weighted Latency (p50, ms)",
            "value": 10.33,
            "unit": "ms"
          },
          {
            "name": "msmarco_concurrent (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 12.89,
            "unit": "ms"
          }
        ]
      }
    ],
    "wikipedia_concurrent Benchmarks": [
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
          "id": "7d0b7056e70944c45d2e374121b02421cf5afe3d",
          "message": "style: fix clang-format issues in build.c and state.h",
          "timestamp": "2026-02-27T22:45:58Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7d0b7056e70944c45d2e374121b02421cf5afe3d"
        },
        "date": 1772246666322,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "wikipedia_concurrent (0 docs) - Index Build Time",
            "value": 1.266,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - Concurrent Insert Time",
            "value": 38545.25624,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 1 Token Query (p50)",
            "value": 0.18,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 2 Token Query (p50)",
            "value": 0.56,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 3 Token Query (p50)",
            "value": 0.74,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 4 Token Query (p50)",
            "value": 0.91,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 5 Token Query (p50)",
            "value": 0.93,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 6 Token Query (p50)",
            "value": 1.06,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 7 Token Query (p50)",
            "value": 1.13,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - 8+ Token Query (p50)",
            "value": 1.82,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - Weighted Latency (p50, ms)",
            "value": 0.81,
            "unit": "ms"
          },
          {
            "name": "wikipedia_concurrent (0 docs) - Weighted Throughput (avg ms/query)",
            "value": 1.02,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}