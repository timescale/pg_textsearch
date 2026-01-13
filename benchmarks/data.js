window.BENCHMARK_DATA = {
  "lastUpdate": 1768342455540,
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
      }
    ]
  }
}