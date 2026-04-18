window.BENCHMARK_DATA = {
  "lastUpdate": 1776498230752,
  "repoUrl": "https://github.com/timescale/pg_textsearch",
  "entries": {
    "Concurrent INSERT (pg_textsearch)": [
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
        "date": 1775773019940,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2546.416843,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.393,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4314.029852,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.464,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4196.182927,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.953,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4253.848806,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.881,
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
        "date": 1775779953142,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2536.604189,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.394,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4181.756825,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.478,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4062.786288,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.985,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4218.044165,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.897,
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
        "date": 1775785586033,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2442.674639,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.409,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4050.264832,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.494,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4104.38632,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.975,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4114.942463,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.944,
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
        "date": 1775807507081,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2467.245467,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.405,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4053.820488,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.493,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 3934.22093,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 1.017,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 3873.993689,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 2.065,
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
        "date": 1775859639458,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2508.930358,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.399,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4573.060642,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.437,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7525.07532,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.532,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 36407.355794,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.22,
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
        "date": 1775893053364,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2596.827043,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.385,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4343.946116,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4298.491544,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.931,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4225.361596,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.893,
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
        "date": 1775979938460,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2521.055856,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.397,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4390.697527,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.456,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4227.237509,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.946,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4182.402426,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.913,
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
        "date": 1776045002155,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2608.048448,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.383,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4863.467921,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.411,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7951.52942,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.503,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 12332.063385,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.649,
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
        "date": 1776066533328,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2642.62639,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.378,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4456.46425,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.449,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4521.862528,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.885,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4391.218749,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.822,
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
        "date": 1776153095424,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2698.399732,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.371,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4421.823671,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.452,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 4592.621903,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.871,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 4210.942287,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.9,
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
        "date": 1776194201366,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2819.578995,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.355,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5305.738223,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.377,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 8652.328391,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.462,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 13058.300603,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.613,
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
        "date": 1776239299563,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2336.047185,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.428,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4437.329617,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.451,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7233.610174,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.553,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11317.853968,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.707,
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
        "date": 1776325970928,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2539.191954,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.394,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4706.201461,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.425,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7664.495623,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.522,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11865.625852,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.674,
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
        "date": 1776412217842,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2614.754585,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.382,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4955.728288,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.404,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7939.154897,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.504,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 12371.644989,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.647,
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
        "date": 1776498226919,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2089.327105,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.479,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 3786.665425,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.528,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6899.744865,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.58,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11095.497615,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.721,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}