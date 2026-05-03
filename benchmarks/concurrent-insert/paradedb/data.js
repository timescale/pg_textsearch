window.BENCHMARK_DATA = {
  "lastUpdate": 1777794556103,
  "repoUrl": "https://github.com/timescale/pg_textsearch",
  "entries": {
    "Concurrent INSERT (ParadeDB)": [
      {
        "commit": {
          "author": {
            "email": "tj@timescale.com",
            "name": "Todd J. Green",
            "username": "tjgreen42"
          },
          "committer": {
            "email": "tj@timescale.com",
            "name": "Todd J. Green",
            "username": "tjgreen42"
          },
          "distinct": true,
          "id": "c3caec35d362856bc5a5494bc3fd523020880916",
          "message": "bench: try ubuntu-latest-8-cores-TJ again",
          "timestamp": "2026-04-09T15:06:48-07:00",
          "tree_id": "62c899d63d61478e397f05e0e373affdaa54fc33",
          "url": "https://github.com/timescale/pg_textsearch/commit/c3caec35d362856bc5a5494bc3fd523020880916"
        },
        "date": 1775772976642,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 3028.883648,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.33,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5857.023738,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.341,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 9751.649107,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.41,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 14802.470444,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.54,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "tj@timescale.com",
            "name": "Todd J. Green",
            "username": "tjgreen42"
          },
          "committer": {
            "email": "tj@timescale.com",
            "name": "Todd J. Green",
            "username": "tjgreen42"
          },
          "distinct": true,
          "id": "9ccf215ce11017abcf7934dc2d4fa18b06c4b582",
          "message": "bench: fix remaining-rows guard and spurious failure warning\n\n- Use `(SELECT last_value FROM insert_seq)` instead of `currval()`\n  which fails in a fresh psql session that never called nextval()\n- Hard-code staging table names per engine instead of deriving via\n  string substitution (ParadeDB name was wrong: msmarco_staging_paradedb\n  vs actual msmarco_paradedb_staging)\n- Use ${failed:-0} to avoid spurious warnings when pgbench omits\n  the failed transactions line on success",
          "timestamp": "2026-04-09T17:05:45-07:00",
          "tree_id": "222d2f801152ca258542b2e78b57f650a10cd6dc",
          "url": "https://github.com/timescale/pg_textsearch/commit/9ccf215ce11017abcf7934dc2d4fa18b06c4b582"
        },
        "date": 1775780091886,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2846.964472,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.351,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5401.378168,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8938.415859,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.448,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 13699.654599,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.584,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "tj@timescale.com",
            "name": "Todd J. Green",
            "username": "tjgreen42"
          },
          "committer": {
            "email": "tj@timescale.com",
            "name": "Todd J. Green",
            "username": "tjgreen42"
          },
          "distinct": true,
          "id": "05414a352926fbce22a75420712ea861ba70cc9c",
          "message": "bench: add concurrent INSERT links to benchmarks index page",
          "timestamp": "2026-04-09T18:35:50-07:00",
          "tree_id": "18d06406f983fd8fa21b02b42754207d66109091",
          "url": "https://github.com/timescale/pg_textsearch/commit/05414a352926fbce22a75420712ea861ba70cc9c"
        },
        "date": 1775785350110,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2849.836678,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.351,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5410.478111,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8886.558477,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 13620.363494,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.587,
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
          "id": "b622d909d9a12186814b72baa49037048a9ef6ac",
          "message": "docs: improve README WHERE clause examples (#306)\n\n## Summary\n- Replace score-threshold-only examples with more intuitive patterns\n- Explicit index specification now uses ORDER BY + LIMIT instead of a\nscore comparison\n- Post-filtering section leads with a price filter (common case), then\nshows score thresholds as a secondary option with caveats about\ncorpus-dependent interpretation\n- Faceted search uses a top-k subquery instead of a score cutoff\n\n## Testing\nDocs-only change, no code affected.",
          "timestamp": "2026-04-10T03:24:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b622d909d9a12186814b72baa49037048a9ef6ac"
        },
        "date": 1775807296177,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2692.709268,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.371,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5033.391325,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.397,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8262.279221,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.484,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12785.909514,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.626,
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
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "d94be32c3c919ab7c946df0942385d7999240c72",
          "message": "refactor: add debug warning for stale locks at xact end\n\nWith per-operation locking, locks should be released by each\noperation. The xact callback is now a safety net — log when it\nfires to flag bugs in lock release paths.",
          "timestamp": "2026-04-10T19:57:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/d94be32c3c919ab7c946df0942385d7999240c72"
        },
        "date": 1775859660421,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2863.501819,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.349,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5280.160483,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.379,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8473.025637,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.472,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 38054.899892,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.21,
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
          "id": "b622d909d9a12186814b72baa49037048a9ef6ac",
          "message": "docs: improve README WHERE clause examples (#306)\n\n## Summary\n- Replace score-threshold-only examples with more intuitive patterns\n- Explicit index specification now uses ORDER BY + LIMIT instead of a\nscore comparison\n- Post-filtering section leads with a price filter (common case), then\nshows score thresholds as a secondary option with caveats about\ncorpus-dependent interpretation\n- Faceted search uses a top-k subquery instead of a score cutoff\n\n## Testing\nDocs-only change, no code affected.",
          "timestamp": "2026-04-10T03:24:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b622d909d9a12186814b72baa49037048a9ef6ac"
        },
        "date": 1775892875083,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2740.011058,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.365,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5205.790106,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.384,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8462.3679,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.473,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12951.851833,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.618,
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
          "id": "b622d909d9a12186814b72baa49037048a9ef6ac",
          "message": "docs: improve README WHERE clause examples (#306)\n\n## Summary\n- Replace score-threshold-only examples with more intuitive patterns\n- Explicit index specification now uses ORDER BY + LIMIT instead of a\nscore comparison\n- Post-filtering section leads with a price filter (common case), then\nshows score thresholds as a secondary option with caveats about\ncorpus-dependent interpretation\n- Faceted search uses a top-k subquery instead of a score cutoff\n\n## Testing\nDocs-only change, no code affected.",
          "timestamp": "2026-04-10T03:24:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b622d909d9a12186814b72baa49037048a9ef6ac"
        },
        "date": 1775979819077,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2721.349078,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.367,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5208.155723,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.384,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8464.899505,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.473,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12878.476186,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.621,
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
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "fb627c342c4019f1970b2660bc505892356fed6a",
          "message": "test: update expected outputs for bm25_spill_index block numbers\n\nAdding tp_clear_docid_pages to tp_spill_memtable shifts segment\nroot block numbers by 1 because the docid page is now properly\nfreed before the next segment allocation.",
          "timestamp": "2026-04-11T22:31:54Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/fb627c342c4019f1970b2660bc505892356fed6a"
        },
        "date": 1776044751092,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2720.523001,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.368,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5063.662756,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.395,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8338.369986,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12815.001138,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.624,
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
          "id": "b622d909d9a12186814b72baa49037048a9ef6ac",
          "message": "docs: improve README WHERE clause examples (#306)\n\n## Summary\n- Replace score-threshold-only examples with more intuitive patterns\n- Explicit index specification now uses ORDER BY + LIMIT instead of a\nscore comparison\n- Post-filtering section leads with a price filter (common case), then\nshows score thresholds as a secondary option with caveats about\ncorpus-dependent interpretation\n- Faceted search uses a top-k subquery instead of a score cutoff\n\n## Testing\nDocs-only change, no code affected.",
          "timestamp": "2026-04-10T03:24:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b622d909d9a12186814b72baa49037048a9ef6ac"
        },
        "date": 1776066745817,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2788.98746,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.359,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5263.431061,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.38,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8576.743673,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.466,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 13259.979158,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.603,
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
          "id": "b622d909d9a12186814b72baa49037048a9ef6ac",
          "message": "docs: improve README WHERE clause examples (#306)\n\n## Summary\n- Replace score-threshold-only examples with more intuitive patterns\n- Explicit index specification now uses ORDER BY + LIMIT instead of a\nscore comparison\n- Post-filtering section leads with a price filter (common case), then\nshows score thresholds as a secondary option with caveats about\ncorpus-dependent interpretation\n- Faceted search uses a top-k subquery instead of a score cutoff\n\n## Testing\nDocs-only change, no code affected.",
          "timestamp": "2026-04-10T03:24:20Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/b622d909d9a12186814b72baa49037048a9ef6ac"
        },
        "date": 1776152879722,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2849.074266,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.351,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5337.546453,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.375,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8811.42038,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.454,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 13556.630077,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.59,
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
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tj@timescale.com"
          },
          "id": "dd08790abc9c1cd93e7b943ed2ef9f9e6aeaed7f",
          "message": "fix: address code review feedback\n\n- Expand comment on lockless threshold checks explaining why\n  approximate reads are safe (false positive → unnecessary lock,\n  false negative → deferred to next insert)\n- Document how the global soft limit warning state is reached\n  (all eviction candidates locked or empty)\n- Document spill lock duration and deadlock safety\n- Clarify tp_check_hard_limit is a read-only check, no lock held\n- Document vacuum spill preconditions (AccessExclusiveLock held,\n  DSA pinned, pre-lock read is fast bailout)\n- Fix user-facing messages: \"tapir index\" → \"pg_textsearch index\",\n  \"Tapir shared DSA\" → \"pg_textsearch shared DSA\"\n- Remove all DEBUG traces from state.c (stale-lock safety net,\n  bulk-load spill, build-mode lifecycle)",
          "timestamp": "2026-04-14T00:52:27Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/dd08790abc9c1cd93e7b943ed2ef9f9e6aeaed7f"
        },
        "date": 1776194380336,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2696.964682,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.371,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5088.945162,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.393,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8421.155163,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.475,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12814.403693,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.624,
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
          "id": "e8f54c3b2c776b8f09ab29d5b5a093873cbfafd8",
          "message": "fix: create shared index state after parallel build (#312)\n\n## Summary\n\n- After a parallel index build, `tp_build()` returned without creating\nshared index state in the registry, forcing subsequent accesses through\nthe crash-recovery path (`tp_rebuild_index_from_disk`)\n- This created a race window where concurrent backends could recreate\nthe shared state independently, leaving the inserting backend's memtable\ninvisible to scans — `tp_memtable_source_create()` would see\n`total_postings == 0` and skip the memtable\n- Fix calls `tp_create_shared_index_state()` after `tp_build_parallel()`\nreturns and populates `total_docs`/`total_len` from the metapage,\nmatching the serial build path's behavior\n\nCloses #310\n\n## Test plan\n\n- [ ] `parallel_build` regression test passes consistently (was flaky\nbefore)\n- [ ] Full regression suite (53 tests) passes\n- [ ] `make format-check` passes",
          "timestamp": "2026-04-15T02:36:24Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e8f54c3b2c776b8f09ab29d5b5a093873cbfafd8"
        },
        "date": 1776239445175,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2652.440581,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.377,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5005.850131,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8349.791671,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.479,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12731.608308,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.628,
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
          "id": "8b8d5bdaf4b658b2a10d330659083f9d41940740",
          "message": "feat: VACUUM performance benchmark (partial vs full) (#319)\n\n## Summary\n\n- Add a VACUUM benchmark that measures partial vs full VACUUM\nperformance against a multi-segment BM25 index\n- The standard load produces a single merged segment, so the benchmark\nfirst inserts 4 batches of ~100K rows with controlled memtable spills\n(`bm25_spill_index`) to create distinct segments (1 large original + 4\nsmall L0)\n- Three scenarios, each affecting ~100K rows:\n- **Partial VACUUM**: delete from one batch — only its segment is\naffected, VACUUM skips the rest\n- **Full VACUUM (delete)**: delete uniformly via `hashint4()` — dead\ntuples in all segments\n- **Full VACUUM (update)**: update uniformly — old tuple versions create\ndead entries across all segments\n- Tracks per-scenario: VACUUM wall-clock time, post-vacuum index size,\nquery latency\n- Integrated into CI with metrics extraction, GitHub Actions historical\ntracking, and job summary tables\n- Runs as part of the nightly benchmark schedule\n- Available locally via `./benchmarks/runner/run_benchmark.sh msmarco\n--vacuum`\n\nMotivation: establish a baseline before the partial vacuum optimization\nPR lands.\n\n## Local results (pg18-release, 8.8M MS MARCO passages)\n\n| Scenario | VACUUM Time | Query Latency |\n|----------|------------|---------------|\n| Partial (concentrated delete, 1 segment) | 3.7s | 11.1 ms/query |\n| Full (uniform delete, all segments) | 234s | 10.0 ms/query |\n| Full (uniform update, all segments) | 230s | 7.6 ms/query |\n\nPartial-to-full ratio is ~62x. The full VACUUM is dominated by\nrebuilding the large 8.7M-doc segment even though only ~1% of its docs\nare dead.\n\n## Testing\n\n- Ran full benchmark locally against pg18-release with 8.8M MS MARCO\npassages\n- Validated metric extraction and GitHub Actions formatting pipelines\nend-to-end",
          "timestamp": "2026-04-16T01:21:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/8b8d5bdaf4b658b2a10d330659083f9d41940740"
        },
        "date": 1776325680170,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2652.006002,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.377,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5037.952482,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.397,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8094.950817,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.494,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12551.737455,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.637,
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
          "id": "199bfcb9dae8b46417f6551fd683edd6f5a8c4c0",
          "message": "Remove WHERE clauses comparing against BM25 scores in tests (#322)\n\n## Summary\n\n- Remove `WHERE content <@> query < 0` and similar score-comparison\n  WHERE clauses from 22 SQL test files\n- Replace with idiomatic `ORDER BY ... LIMIT` patterns that use the\n  BM25 index scan instead of standalone scoring\n- For COUNT queries, wrap in `ORDER BY` subqueries\n- Add `SET enable_seqscan = off` where needed for small-table tests\n- Add query style guidelines to CLAUDE.md\n\nBM25 scores are for ranking, not filtering. The numeric thresholds\n(`< 0`, `< -0.001`, `> -5`) were opaque, and the WHERE clause\ntriggered standalone scoring (seq scan) instead of the index scan\nthat ORDER BY provides.\n\nTwo intentional exceptions left in place:\n- `validation.sql`: deliberately uses standalone scoring as a\n  reference to validate index scan correctness\n- `aerodocs.sql`: CROSS JOIN evaluating all doc-query pairs where\n  an index scan isn't applicable\n\n## Testing\n\nAll 57 SQL regression tests pass + shell tests pass on PG17.",
          "timestamp": "2026-04-16T16:57:30Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/199bfcb9dae8b46417f6551fd683edd6f5a8c4c0"
        },
        "date": 1776412351802,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2785.239269,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.359,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5212.635542,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.384,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8608.889409,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.465,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12999.052135,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.615,
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
          "id": "54ab691bfad94b016bdeb5dbd22ed7680e12e393",
          "message": "refactor: deduplicate read_term_at_index into dictionary.c (#324)\n\n## Summary\n\n- Extract shared term-reading logic from `segment.c` and `merge.c`\n  into `dictionary.c` as `tp_segment_read_term_at_index()`\n- Both files had nearly identical static functions for reading a term\n  string from the segment string pool\n\n## Testing\n\n- Compiles cleanly on PG17 and PG18\n- All regression tests pass (no behavioral change)",
          "timestamp": "2026-04-17T20:33:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/54ab691bfad94b016bdeb5dbd22ed7680e12e393"
        },
        "date": 1776498122730,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 3052.853571,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.328,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5942.209406,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.337,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 9911.562646,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.404,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 14858.370317,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.538,
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
          "id": "54ab691bfad94b016bdeb5dbd22ed7680e12e393",
          "message": "refactor: deduplicate read_term_at_index into dictionary.c (#324)\n\n## Summary\n\n- Extract shared term-reading logic from `segment.c` and `merge.c`\n  into `dictionary.c` as `tp_segment_read_term_at_index()`\n- Both files had nearly identical static functions for reading a term\n  string from the segment string pool\n\n## Testing\n\n- Compiles cleanly on PG17 and PG18\n- All regression tests pass (no behavioral change)",
          "timestamp": "2026-04-17T20:33:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/54ab691bfad94b016bdeb5dbd22ed7680e12e393"
        },
        "date": 1776584695505,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2728.560279,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.366,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5118.895256,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.391,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8371.976303,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.478,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12880.501618,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.621,
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
          "id": "54ab691bfad94b016bdeb5dbd22ed7680e12e393",
          "message": "refactor: deduplicate read_term_at_index into dictionary.c (#324)\n\n## Summary\n\n- Extract shared term-reading logic from `segment.c` and `merge.c`\n  into `dictionary.c` as `tp_segment_read_term_at_index()`\n- Both files had nearly identical static functions for reading a term\n  string from the segment string pool\n\n## Testing\n\n- Compiles cleanly on PG17 and PG18\n- All regression tests pass (no behavioral change)",
          "timestamp": "2026-04-17T20:33:31Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/54ab691bfad94b016bdeb5dbd22ed7680e12e393"
        },
        "date": 1776671471196,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2802.565449,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.357,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5091.493851,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.393,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8481.281132,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.472,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12902.148414,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.62,
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
          "id": "da18d486a5c5a09f75d4c441821d2488e7a7d4b2",
          "message": "chore: bump version to 1.1.0-dev (#332)\n\n## Summary\nPost-1.0.0 release bump — we forgot to do this after the 1.0.0 release.\n\n- Rename `pg_textsearch--1.0.0-dev.sql` → `pg_textsearch--1.1.0-dev.sql`\n- Rename upgrade script `pg_textsearch--1.0.0--1.0.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0-dev.sql`\n- Update version strings in `pg_textsearch.control`, `src/mod.c`,\n`CLAUDE.md`, `Makefile`, test scripts, and expected outputs\n- Drop the `timescaledb-docker-ha` step from `RELEASING.md`; that part\nof the release procedure now lives in internal docs\n\n## Testing\n- `make test` passes\n- `make format-check` clean",
          "timestamp": "2026-04-21T02:51:41Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/da18d486a5c5a09f75d4c441821d2488e7a7d4b2"
        },
        "date": 1776758037022,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2606.12649,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.384,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5033.501648,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.397,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8335.780682,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.48,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12498.923307,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.64,
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
          "id": "da18d486a5c5a09f75d4c441821d2488e7a7d4b2",
          "message": "chore: bump version to 1.1.0-dev (#332)\n\n## Summary\nPost-1.0.0 release bump — we forgot to do this after the 1.0.0 release.\n\n- Rename `pg_textsearch--1.0.0-dev.sql` → `pg_textsearch--1.1.0-dev.sql`\n- Rename upgrade script `pg_textsearch--1.0.0--1.0.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0-dev.sql`\n- Update version strings in `pg_textsearch.control`, `src/mod.c`,\n`CLAUDE.md`, `Makefile`, test scripts, and expected outputs\n- Drop the `timescaledb-docker-ha` step from `RELEASING.md`; that part\nof the release procedure now lives in internal docs\n\n## Testing\n- `make test` passes\n- `make format-check` clean",
          "timestamp": "2026-04-21T02:51:41Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/da18d486a5c5a09f75d4c441821d2488e7a7d4b2"
        },
        "date": 1776844265069,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2589.808913,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.386,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 4963.930853,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.403,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8254.925599,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.485,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12619.678836,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.634,
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
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1776930702417,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 3038.119515,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.329,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5910.043908,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.338,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 9785.585722,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.409,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 14698.426398,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.544,
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
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1777017263372,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2683.292545,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.373,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 4821.45173,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.415,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8208.92577,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.487,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12385.824878,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.646,
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
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1777103169710,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2704.616218,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.37,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5162.217047,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.387,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8521.473525,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.469,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12877.829271,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.621,
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
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1777189582831,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2721.377015,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.367,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5092.258168,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.393,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8399.977959,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.476,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12807.132524,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.625,
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
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1777276868306,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2662.926515,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.376,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5003.568108,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8234.418232,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.486,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12466.911761,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.642,
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
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1777363400881,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2251.59255,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.444,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 4280.03903,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.467,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 7316.922797,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.547,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 11862.063315,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.674,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "email": "tj@timescale.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "35a2dc6345805037e8d48071adb7d746a2e193da",
          "message": "Release v1.1.0 (#334)\n\n## Summary\n- Bump version from `1.1.0-dev` to `1.1.0` across control, `mod.c`,\n`Makefile`,\n`README.md`, `CLAUDE.md`, `RELEASING.md`, and hardcoded version strings\nin\n  `test/scripts/*.sh`\n- Rename SQL files (`pg_textsearch--1.1.0-dev.sql` →\n`pg_textsearch--1.1.0.sql`\nand `pg_textsearch--1.0.0--1.1.0-dev.sql` →\n`pg_textsearch--1.0.0--1.1.0.sql`)\n  and update internal version strings / `RAISE INFO` messages\n- Swap in new v1.1.0 banner image; drop the v1.0.0 banner\n- Update README status line to \"v1.1.0 - Production ready\"\n- Regenerate expected test output for the new version string (including\n  alternative `*_1.out` files)\n\n## Notes\n- Segment format was bumped v4 → v5 during the dev cycle (alive bitset,\n#317). Read paths remain backward-compatible with v3/v4 segments;\nupgrade\ntests (#327) cover this. No additional version constants bumped for the\n  release itself.\n\n## Testing\n- `make test` — 58/58 passed against pg17\n- `make format-check` — passed",
          "timestamp": "2026-04-22T22:58:52Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/35a2dc6345805037e8d48071adb7d746a2e193da"
        },
        "date": 1777449457969,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2709.580116,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.369,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5103.354636,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.392,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8377.462502,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.477,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12759.756678,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.627,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tjgreen@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "142b32d81dedac7fbb17af0506c66707219066c2",
          "message": "chore: bump version to 1.2.0-dev (+ automate version bumps) (#341)\n\n## Summary\n\nPost-1.1.0 dev bump, plus tooling so future bumps are mechanical.\n\nThree commits:\n\n1. **Drop CREATE EXTENSION load banner; add bump-version.sh** — Remove\n   the `RAISE INFO 'pg_textsearch vX.Y.Z'` block that fired on every\n   \\`CREATE EXTENSION\\`. It produced one INFO line that 62 expected\n   outputs had to absorb, so every release regenerated 62 files for\n   the same one-line diff. Survey of nearby extensions (pgvector,\n   pgvectorscale, pg_search, timescaledb) and standard contribs\n   (pg_stat_statements, hstore, pg_trgm, pgcrypto, postgis) confirms\n   none emit a load banner; pg_textsearch was the outlier. The\n   library-version mismatch \\`RAISE EXCEPTION\\` (the actual\n   correctness check) stays. Version remains discoverable via\n   \\`SHOW pg_textsearch.library_version\\`, \\`\\\\dx\\`, and\n   \\`pg_extension.extversion\\`.\n\n   Also adds \\`scripts/bump-version.sh\\`, which automates the text\n   substitutions across SQL files, Makefile DATA, control file,\n   mod.c, README, CLAUDE.md, test scripts, and the msmarco benchmark\n   version check. Auto-detects dev-bump vs release mode from the\n   version pattern; refuses to run on a dirty tree; reports\n   stragglers after substituting; prints next-steps for the human.\n\n2. **docs: simplify RELEASING.md around bump-version.sh** — Collapse\n   the per-file substitution checklist (the script does it now) and\n   replace with a step-driven sequence. Add a section on auditing\n   the upgrade SQL script — the one piece the script can't automate\n   and the most likely place to ship a regression.\n\n3. **chore: bump version to 1.2.0-dev** — Generated by running\n   \\`scripts/bump-version.sh 1.1.0 1.2.0-dev\\` (the dogfood proves\n   the script works). Renames the base SQL file, creates the\n   upgrade-script stub, updates default_version /\n   PG_MODULE_MAGIC_EXT / Makefile / README / CLAUDE.md / test\n   scripts / msmarco benchmark.\n\n## Testing\n\n- \\`make test\\` — 58/58 pass on pg18-release after the bump\n- \\`make format-check\\` — clean\n- The script itself is the integration test (commit 3 is its output)\n\n## Follow-up for the next release\n\nReplace the banner image and add 1.1.0 to the upgrade-tests matrix\nwhen cutting v1.2.0; \\`scripts/bump-version.sh\\` reminds you of both\nin its next-steps output.",
          "timestamp": "2026-04-29T16:57:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/142b32d81dedac7fbb17af0506c66707219066c2"
        },
        "date": 1777536049322,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 3126.01076,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.32,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 6046.162926,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.331,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 10068.946974,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.397,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 15091.846653,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.53,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tjgreen@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "142b32d81dedac7fbb17af0506c66707219066c2",
          "message": "chore: bump version to 1.2.0-dev (+ automate version bumps) (#341)\n\n## Summary\n\nPost-1.1.0 dev bump, plus tooling so future bumps are mechanical.\n\nThree commits:\n\n1. **Drop CREATE EXTENSION load banner; add bump-version.sh** — Remove\n   the `RAISE INFO 'pg_textsearch vX.Y.Z'` block that fired on every\n   \\`CREATE EXTENSION\\`. It produced one INFO line that 62 expected\n   outputs had to absorb, so every release regenerated 62 files for\n   the same one-line diff. Survey of nearby extensions (pgvector,\n   pgvectorscale, pg_search, timescaledb) and standard contribs\n   (pg_stat_statements, hstore, pg_trgm, pgcrypto, postgis) confirms\n   none emit a load banner; pg_textsearch was the outlier. The\n   library-version mismatch \\`RAISE EXCEPTION\\` (the actual\n   correctness check) stays. Version remains discoverable via\n   \\`SHOW pg_textsearch.library_version\\`, \\`\\\\dx\\`, and\n   \\`pg_extension.extversion\\`.\n\n   Also adds \\`scripts/bump-version.sh\\`, which automates the text\n   substitutions across SQL files, Makefile DATA, control file,\n   mod.c, README, CLAUDE.md, test scripts, and the msmarco benchmark\n   version check. Auto-detects dev-bump vs release mode from the\n   version pattern; refuses to run on a dirty tree; reports\n   stragglers after substituting; prints next-steps for the human.\n\n2. **docs: simplify RELEASING.md around bump-version.sh** — Collapse\n   the per-file substitution checklist (the script does it now) and\n   replace with a step-driven sequence. Add a section on auditing\n   the upgrade SQL script — the one piece the script can't automate\n   and the most likely place to ship a regression.\n\n3. **chore: bump version to 1.2.0-dev** — Generated by running\n   \\`scripts/bump-version.sh 1.1.0 1.2.0-dev\\` (the dogfood proves\n   the script works). Renames the base SQL file, creates the\n   upgrade-script stub, updates default_version /\n   PG_MODULE_MAGIC_EXT / Makefile / README / CLAUDE.md / test\n   scripts / msmarco benchmark.\n\n## Testing\n\n- \\`make test\\` — 58/58 pass on pg18-release after the bump\n- \\`make format-check\\` — clean\n- The script itself is the integration test (commit 3 is its output)\n\n## Follow-up for the next release\n\nReplace the banner image and add 1.1.0 to the upgrade-tests matrix\nwhen cutting v1.2.0; \\`scripts/bump-version.sh\\` reminds you of both\nin its next-steps output.",
          "timestamp": "2026-04-29T16:57:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/142b32d81dedac7fbb17af0506c66707219066c2"
        },
        "date": 1777622251208,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2687.0717,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.372,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5005.683189,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.4,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8265.752818,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.484,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12677.957837,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.631,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tjgreen@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "7f7bd41a8914b480dc30c0e275e6a1ab04facc33",
          "message": "fix(replication): WAL-log segment writes (#342) + 24-test physical replication suite (#343)\n\n## Summary\n\nFixes #342 — pg_textsearch's segment writes weren't reaching streaming\nstandbys, so replica queries errored with `invalid page index at block\nN`. Three code paths needed to emit WAL; this PR adds it. Bundled with a\nphysical-replication test suite that pinned down the bug, surfaced two\nof the three affected paths, and verifies the fix end-to-end.\n\n## The fix\n\nThree previously WAL-silent paths now emit WAL:\n\n| File / function | WAL added |\n|---|---|\n| `src/segment/segment.c` `write_page_index_internal` (covers serial\nCREATE INDEX, parallel-build leader, memtable spill, level merge — all\nflow through `write_page_index`) | `log_newpage_buffer(buf, false)`\ninside `START_CRIT_SECTION()`. `page_std=false` because page-index\nentries live in the pd_lower/pd_upper \"hole\" — `REGBUF_STANDARD` would\nzero them on replay. |\n| `src/access/build_context.c` `tp_write_segment_from_build_ctx` (serial\nCREATE INDEX data pages) | Post-hoc `GenericXLog FULL_IMAGE` pass over\n`writer.pages`, mirroring the existing pass in `tp_write_segment`. |\n| `src/access/build_parallel.c` `tp_build_parallel` (parallel CREATE\nINDEX merged data pages — caught by @upirr's review) | Same `GenericXLog\nFULL_IMAGE` pass over `sink.writer.pages`. |\n\n## Verification\n\n| Script | Path exercised | Before fix → After fix |\n|---|---|---|\n| `test/scripts/replication_issue_342.sh` | 3000-doc memtable spill →\n`write_page_index_internal` | standby `could not read blocks` →\n3000/3000 |\n| `test/scripts/replication_parallel_build.sh` | 150K rows + 4 parallel\nworkers → `tp_build_parallel` | standby `invalid segment header at block\n1` → 150000/150000 |\n\nBoth verified locally by reverting the corresponding fix and re-running.\n\n## Suite\n\n30 tests across 9 scripts (`make test-replication-extended`). After this\nPR: **18 pass, 11 fail, 1 skip**. All 11 failures map to a separate bug\n— long-lived standby backend staleness — tracked in **#345**; the PITR\ntest fails on a third issue (corpus stats not WAL-replayed). Out of\nscope here.",
          "timestamp": "2026-05-02T03:02:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7f7bd41a8914b480dc30c0e275e6a1ab04facc33"
        },
        "date": 1777708102337,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2821.699549,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.354,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5314.909523,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.376,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8976.346812,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.446,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 13467.848207,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.594,
            "unit": "ms"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Todd J. Green",
            "username": "tjgreen42",
            "email": "tjgreen@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "7f7bd41a8914b480dc30c0e275e6a1ab04facc33",
          "message": "fix(replication): WAL-log segment writes (#342) + 24-test physical replication suite (#343)\n\n## Summary\n\nFixes #342 — pg_textsearch's segment writes weren't reaching streaming\nstandbys, so replica queries errored with `invalid page index at block\nN`. Three code paths needed to emit WAL; this PR adds it. Bundled with a\nphysical-replication test suite that pinned down the bug, surfaced two\nof the three affected paths, and verifies the fix end-to-end.\n\n## The fix\n\nThree previously WAL-silent paths now emit WAL:\n\n| File / function | WAL added |\n|---|---|\n| `src/segment/segment.c` `write_page_index_internal` (covers serial\nCREATE INDEX, parallel-build leader, memtable spill, level merge — all\nflow through `write_page_index`) | `log_newpage_buffer(buf, false)`\ninside `START_CRIT_SECTION()`. `page_std=false` because page-index\nentries live in the pd_lower/pd_upper \"hole\" — `REGBUF_STANDARD` would\nzero them on replay. |\n| `src/access/build_context.c` `tp_write_segment_from_build_ctx` (serial\nCREATE INDEX data pages) | Post-hoc `GenericXLog FULL_IMAGE` pass over\n`writer.pages`, mirroring the existing pass in `tp_write_segment`. |\n| `src/access/build_parallel.c` `tp_build_parallel` (parallel CREATE\nINDEX merged data pages — caught by @upirr's review) | Same `GenericXLog\nFULL_IMAGE` pass over `sink.writer.pages`. |\n\n## Verification\n\n| Script | Path exercised | Before fix → After fix |\n|---|---|---|\n| `test/scripts/replication_issue_342.sh` | 3000-doc memtable spill →\n`write_page_index_internal` | standby `could not read blocks` →\n3000/3000 |\n| `test/scripts/replication_parallel_build.sh` | 150K rows + 4 parallel\nworkers → `tp_build_parallel` | standby `invalid segment header at block\n1` → 150000/150000 |\n\nBoth verified locally by reverting the corresponding fix and re-running.\n\n## Suite\n\n30 tests across 9 scripts (`make test-replication-extended`). After this\nPR: **18 pass, 11 fail, 1 skip**. All 11 failures map to a separate bug\n— long-lived standby backend staleness — tracked in **#345**; the PITR\ntest fails on a third issue (corpus stats not WAL-replayed). Out of\nscope here.",
          "timestamp": "2026-05-02T03:02:47Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/7f7bd41a8914b480dc30c0e275e6a1ab04facc33"
        },
        "date": 1777794550826,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "ParadeDB INSERT TPS (c=1)",
            "value": 2737.520902,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=1)",
            "value": 0.365,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=2)",
            "value": 5167.18572,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=2)",
            "value": 0.387,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=4)",
            "value": 8455.072126,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=4)",
            "value": 0.473,
            "unit": "ms"
          },
          {
            "name": "ParadeDB INSERT TPS (c=8)",
            "value": 12981.109083,
            "unit": "tps"
          },
          {
            "name": "ParadeDB INSERT latency (c=8)",
            "value": 0.616,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}