window.BENCHMARK_DATA = {
  "lastUpdate": 1781686691452,
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
      },
      {
        "commit": {
          "author": {
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
        "date": 1776584711493,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2473.614355,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.404,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4650.515171,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.43,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7460.523519,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.536,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11807.892937,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.678,
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
        "date": 1776671532340,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2462.905209,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.406,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4605.586546,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.434,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7462.09997,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.536,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11598.933909,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.69,
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
        "date": 1776757856813,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2639.339292,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.379,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4979.531753,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.402,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 8192.918241,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.488,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 12676.968005,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
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
        "date": 1776844175776,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2449.932202,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.408,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4604.615229,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.434,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7426.832482,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.539,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11593.488008,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.69,
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
        "date": 1776930835118,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2532.337994,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.395,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4762.928841,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7766.272004,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.515,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11929.354363,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.671,
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
        "date": 1777017196667,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2582.063045,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.387,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4858.373835,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.412,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7689.199973,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.52,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9453.787711,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.846,
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
        "date": 1777103069108,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2561.691169,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.39,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4821.698316,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.415,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7718.420798,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.518,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 12181.242422,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.657,
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
        "date": 1777189579362,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2463.220201,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.406,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4591.46742,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.436,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7346.700819,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.544,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11214.429088,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.713,
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
        "date": 1777276979625,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2452.751683,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.408,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4616.825916,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.433,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7600.403346,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.526,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11642.483274,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.687,
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
        "date": 1777363376460,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2347.410759,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.426,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4425.603418,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.452,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7230.644468,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.553,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11158.013907,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.717,
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
        "date": 1777449542471,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2409.689319,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.415,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4701.790843,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.425,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7487.721559,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.534,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11776.469355,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.679,
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
        "date": 1777536129737,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2422.249219,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.413,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4666.777904,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.429,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7352.03635,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.544,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11770.649736,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.68,
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
        "date": 1777622406529,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2924.023345,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.342,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5609.360258,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.357,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 8960.882963,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.446,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 10354.281873,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.773,
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
        "date": 1777708123976,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2768.231201,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.361,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5347.278423,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.374,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 8888.732424,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.45,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 13454.276825,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.595,
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
        "date": 1777794666534,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2518.499654,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.397,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4686.99311,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.427,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7602.085817,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.526,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11578.529113,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.691,
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
          "id": "5886b94f61767c0c06b31ec9d5de9b4b4d1094b8",
          "message": "feat: compact bm25vector v2 wire format (#346)\n\nFirst PR of a three-PR series fixing #345 (long-lived standby backend\nstaleness). Design doc lands here as\n`docs/plans/2026-05-01-physical-replication.md`; PR 3 deletes it.\n\n## Summary\n\nTighten the `bm25vector` per-entry encoding before the WAL work for #345\nlands. v1 used `int32 frequency + int32 lexeme_len + MAXALIGN padding`\nper entry — ~16 bytes for a 6-char lexeme. v2 packs entries as varint\n(LEB128) frequency + varint lexeme_len + lexeme bytes — ~8 bytes for the\nsame entry, with no padding waste.\n\nPer-entry size, English-stem typical case (lexeme ≈ 6 chars, freq = 1):\n\n| | Bytes |\n|---|---|\n| v1 | ~16 |\n| v2 (this PR) | **~8** |\n\nFor a 100-token doc with 70 unique stems, ~570 bytes of vector payload\nafter this PR (vs ~1150 in v1). The compaction applies everywhere\n`bm25vector` is materialized — not just in the upcoming #345 WAL\nrecords.\n\n## Format detection\n\nEach v2 value carries a 4-byte ASCII `\"BM25\"` magic immediately after\nthe varlena header, plus a 1-byte version field and 3 reserved bytes.\nThe magic is unambiguously not a valid v1 `index_name_len` value, so\nreaders sniff the first 4 bytes after `vl_len_` and dispatch to v1 or v2\nparsers.\n\n## Backward compatibility\n\n- `tpvector_recv` accepts both v1 and v2 binary input; v1 is converted\nto v2 in palloc'd memory before returning.\n- `tpvector_canonicalize()` runs at every operator boundary\n(`tpvector_eq`, `tpvector_send`, `tpvector_out`) on `PG_DETOAST_DATUM`\noutput, so any persisted v1 value is upgraded on first read.\n- Writers always emit v2; `tpvector_send` always emits v2 wire bytes.\n- Internal walkers and storage paths only ever see v2.\n\n`bm25vector` columns persisted in user tables (uncommon — the type is\nnormally transient) continue to work; the value is upgraded on first\nread.\n\n## API change\n\n`TpVectorEntry` is now opaque (variable-length byte stream). Internal\ncallers (`tp_insert` in `build.c`, the memtable scan path in\n`memtable/scan.c`) decode entries via `tpvector_entry_decode()` into a\n`TpVectorEntryView` struct.\n\n## Files\n\n- `src/types/vector.{c,h}` — v2 layout, varint helpers, v1→v2 converter,\ncanonicalize wrapper.\n- `src/access/build.c`, `src/memtable/scan.c` — internal callers updated\nto use the decoded view.\n- `docs/plans/2026-05-01-physical-replication.md` — three-PR design doc\nanchoring this work; deleted in PR 3.\n\n## Testing\n\n- All 58 SQL regression tests pass on PG17 (debug build).\n- All shell tests pass.\n- Format check passes.\n\n## Lifecycle\n\nThis PR is purely preparatory. PR 2 will use the v2 layout to carry\n`INSERT_TERMS` WAL record payloads. PR 3 removes the now-redundant\ndocid-page machinery.",
          "timestamp": "2026-05-03T16:08:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/5886b94f61767c0c06b31ec9d5de9b4b4d1094b8"
        },
        "date": 1777882320513,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2859.546067,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.35,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5511.280453,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.363,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 8947.928267,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.447,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9992.151818,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.801,
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
          "id": "5886b94f61767c0c06b31ec9d5de9b4b4d1094b8",
          "message": "feat: compact bm25vector v2 wire format (#346)\n\nFirst PR of a three-PR series fixing #345 (long-lived standby backend\nstaleness). Design doc lands here as\n`docs/plans/2026-05-01-physical-replication.md`; PR 3 deletes it.\n\n## Summary\n\nTighten the `bm25vector` per-entry encoding before the WAL work for #345\nlands. v1 used `int32 frequency + int32 lexeme_len + MAXALIGN padding`\nper entry — ~16 bytes for a 6-char lexeme. v2 packs entries as varint\n(LEB128) frequency + varint lexeme_len + lexeme bytes — ~8 bytes for the\nsame entry, with no padding waste.\n\nPer-entry size, English-stem typical case (lexeme ≈ 6 chars, freq = 1):\n\n| | Bytes |\n|---|---|\n| v1 | ~16 |\n| v2 (this PR) | **~8** |\n\nFor a 100-token doc with 70 unique stems, ~570 bytes of vector payload\nafter this PR (vs ~1150 in v1). The compaction applies everywhere\n`bm25vector` is materialized — not just in the upcoming #345 WAL\nrecords.\n\n## Format detection\n\nEach v2 value carries a 4-byte ASCII `\"BM25\"` magic immediately after\nthe varlena header, plus a 1-byte version field and 3 reserved bytes.\nThe magic is unambiguously not a valid v1 `index_name_len` value, so\nreaders sniff the first 4 bytes after `vl_len_` and dispatch to v1 or v2\nparsers.\n\n## Backward compatibility\n\n- `tpvector_recv` accepts both v1 and v2 binary input; v1 is converted\nto v2 in palloc'd memory before returning.\n- `tpvector_canonicalize()` runs at every operator boundary\n(`tpvector_eq`, `tpvector_send`, `tpvector_out`) on `PG_DETOAST_DATUM`\noutput, so any persisted v1 value is upgraded on first read.\n- Writers always emit v2; `tpvector_send` always emits v2 wire bytes.\n- Internal walkers and storage paths only ever see v2.\n\n`bm25vector` columns persisted in user tables (uncommon — the type is\nnormally transient) continue to work; the value is upgraded on first\nread.\n\n## API change\n\n`TpVectorEntry` is now opaque (variable-length byte stream). Internal\ncallers (`tp_insert` in `build.c`, the memtable scan path in\n`memtable/scan.c`) decode entries via `tpvector_entry_decode()` into a\n`TpVectorEntryView` struct.\n\n## Files\n\n- `src/types/vector.{c,h}` — v2 layout, varint helpers, v1→v2 converter,\ncanonicalize wrapper.\n- `src/access/build.c`, `src/memtable/scan.c` — internal callers updated\nto use the decoded view.\n- `docs/plans/2026-05-01-physical-replication.md` — three-PR design doc\nanchoring this work; deleted in PR 3.\n\n## Testing\n\n- All 58 SQL regression tests pass on PG17 (debug build).\n- All shell tests pass.\n- Format check passes.\n\n## Lifecycle\n\nThis PR is purely preparatory. PR 2 will use the v2 layout to carry\n`INSERT_TERMS` WAL record payloads. PR 3 removes the now-redundant\ndocid-page machinery.",
          "timestamp": "2026-05-03T16:08:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/5886b94f61767c0c06b31ec9d5de9b4b4d1094b8"
        },
        "date": 1777967604537,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2377.637742,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.421,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4469.980672,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.447,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7670.153802,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.522,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9279.050613,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.862,
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
          "id": "bdd3b317af4007bfca6d1b43ff8446fb55d0e2ae",
          "message": "fix: replicate memtable mutations via custom rmgr (#347)\n\nCloses #345, #349, #350.\n\n## What was broken\n\n`pg_textsearch` keeps the working set of recent docs in a DSA\nshared-memory memtable (one per index, shared across backends). Pre-PR,\nthe memtable on a streaming standby was populated only by the standby's\nown first-access rebuild. After that, primary writes did not propagate:\nthey updated docid pages on disk via GenericXLog but never touched the\nstandby's in-memory memtable. Three bugs followed.\n\n- **#345**: a long-lived standby backend's BM25 query results were\nfrozen at the time of its first index access. Subsequent primary inserts\nwere invisible until the backend reconnected.\n- **#349**: when the primary compacted L0 segments into L1, the standby\nreceived the metapage unlink/link via GenericXLog (buffer locks only),\nbut a backend mid-walk on the L0 chain followed `next_segment` pointers\ninto pages that the FSM had since recycled. Symptoms ranged from wrong\ncounts to `ERROR: invalid page index at block N / magic=0x00000000`.\n- **#350**: `total_docs` and `total_len` (the BM25 corpus stats that\ndrive IDF and BMW) were never WAL-logged. After PITR to a between-spills\nLSN, the recovered cluster's metapage held the last spill's snapshot,\nthe in-memory atomics started at zero, and the standard query path\nreturned zero results until a `REINDEX`.\n\n## Summary\n\nA custom WAL resource manager (rmgr ID 149,\n[registered](https://wiki.postgresql.org/wiki/CustomWALResourceManagers))\nemits three record types so streaming-replication standbys and primary\ncrash recovery keep their in-shared-memory memtable in sync with the\nsegment chain on disk.\n\n| Record | Standby redo |\n|---|---|\n| `INSERT_TERMS` | Apply the v2 bm25vector to the memtable (idempotent\nby CTID). |\n| `SPILL` | Apply the L0 chain-link, optional new-segment splice, and\nmemtable clear, atomically under per-index `LW_EXCLUSIVE`. |\n| `MERGE_LINKAGE` | Apply the metap unlink/link, optional new-segment\nsplice, and corpus-stat shrinkage, atomically under `LW_EXCLUSIVE`. |\n\n`SPILL` is emitted from all five spill recipes (auto-spill,\n`bm25_spill_index`, VACUUM bulkdelete, PRE_COMMIT bulk-load,\nglobal-memory eviction). Each spill entry point also has a\n`RecoveryInProgress()` guard so a standby never autonomously spills.\nEmission is gated on `RelationNeedsWAL(rel)` so UNLOGGED and TEMP\nindexes do not ship records that cannot apply on a standby.\n\nThe full invariants and scenario-by-scenario verification of the\ncold-start bootstrap path are spec'd at\n[`docs/replication/CORRECTNESS.md`](docs/replication/CORRECTNESS.md).\nRead that before changing `tp_rebuild_index_from_disk`.\n\n## Notes on the tricky parts\n\n- **Corpus stats.** The bootstrap writes `total_docs = metap.total +\nmemtable_count` via `pg_atomic_write` under the per-index exclusive\nlock, not `fetch_add`. The two halves are disjoint by construction\n(segments versus the memtable), so the write is unambiguous; `fetch_add`\nwould double-count any cohort that redo applied during the WAL-replay\ndrain and that a subsequent SPILL also folded into `metap.total_docs`.\nThe PITR \"BM25 returns zero\" symptom in #350 was the pre-fix\nunconditional-overwrite case.\n\n- **Concurrent bootstrap.** Two backends arriving at the same empty\nregistry slot resolve to the same shared state via\n`tp_registry_register_if_absent` (atomic register-or-attach), not to a\nunregister/re-register race.\n\n- **Defensive WAL decoding.** Header-length checks before any field\nderef; bounds check on `MERGE_LINKAGE.level` before mutating\n`metap.level_heads/level_counts`; designated initializers on all\nemission headers so compiler padding is zeroed.\n\n- **`bm25_force_merge` locking.** `tp_xlog_merge_linkage` requires the\ncaller to hold the per-index `LW_EXCLUSIVE`; this serializes the\nstandby's redo against in-flight scans (#349). All other call paths take\nthat lock first, but `bm25_force_merge` was a pre-existing exception: it\nheld only `RowExclusiveLock` on the relation and called the merge helper\ndirectly. The SQL function now wraps the force-merge body in\n`tp_acquire_index_lock(LW_EXCLUSIVE)` / `tp_release_index_lock`.\n\n## Testing\n\n| Suite | New coverage |\n|---|---|\n| `replication_spill_paths.sh` | 4 tests: `tp_xlog_spill` from each\nspill recipe, cold-start corpus stats (#350), `MERGE_LINKAGE` round-trip\n(#349) |\n| `wal_audit.sh` | 3 tests: `RelationNeedsWAL` guard, `tp_rmgr_identify`\nnames, `INSERT_TERMS` records present in WAL window |\n| `recovery.sh` | `test_empty_index_rebuild`: empty index + restart\nfires the bootstrap empty-index branch |\n| `unlogged_index.sql` | UNLOGGED merge through `merge.c` GenericXLog\nfallback; delete + VACUUM + force-merge to drive shrinkage on that path\n|\n\nExisting replication suite (`replication.sh`,\n`replication_correctness.sh`, `replication_concurrency.sh`,\n`replication_failover.sh`, `replication_cascading.sh`,\n`replication_compat.sh`, `replication_parallel_build.sh`,\n`replication_issue_342.sh`) all pass.",
          "timestamp": "2026-05-06T02:20:08Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bdd3b317af4007bfca6d1b43ff8446fb55d0e2ae"
        },
        "date": 1778054658898,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2882.673316,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.347,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5419.257696,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.369,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 8923.426252,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.448,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9723.652014,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.823,
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
          "id": "bdd3b317af4007bfca6d1b43ff8446fb55d0e2ae",
          "message": "fix: replicate memtable mutations via custom rmgr (#347)\n\nCloses #345, #349, #350.\n\n## What was broken\n\n`pg_textsearch` keeps the working set of recent docs in a DSA\nshared-memory memtable (one per index, shared across backends). Pre-PR,\nthe memtable on a streaming standby was populated only by the standby's\nown first-access rebuild. After that, primary writes did not propagate:\nthey updated docid pages on disk via GenericXLog but never touched the\nstandby's in-memory memtable. Three bugs followed.\n\n- **#345**: a long-lived standby backend's BM25 query results were\nfrozen at the time of its first index access. Subsequent primary inserts\nwere invisible until the backend reconnected.\n- **#349**: when the primary compacted L0 segments into L1, the standby\nreceived the metapage unlink/link via GenericXLog (buffer locks only),\nbut a backend mid-walk on the L0 chain followed `next_segment` pointers\ninto pages that the FSM had since recycled. Symptoms ranged from wrong\ncounts to `ERROR: invalid page index at block N / magic=0x00000000`.\n- **#350**: `total_docs` and `total_len` (the BM25 corpus stats that\ndrive IDF and BMW) were never WAL-logged. After PITR to a between-spills\nLSN, the recovered cluster's metapage held the last spill's snapshot,\nthe in-memory atomics started at zero, and the standard query path\nreturned zero results until a `REINDEX`.\n\n## Summary\n\nA custom WAL resource manager (rmgr ID 149,\n[registered](https://wiki.postgresql.org/wiki/CustomWALResourceManagers))\nemits three record types so streaming-replication standbys and primary\ncrash recovery keep their in-shared-memory memtable in sync with the\nsegment chain on disk.\n\n| Record | Standby redo |\n|---|---|\n| `INSERT_TERMS` | Apply the v2 bm25vector to the memtable (idempotent\nby CTID). |\n| `SPILL` | Apply the L0 chain-link, optional new-segment splice, and\nmemtable clear, atomically under per-index `LW_EXCLUSIVE`. |\n| `MERGE_LINKAGE` | Apply the metap unlink/link, optional new-segment\nsplice, and corpus-stat shrinkage, atomically under `LW_EXCLUSIVE`. |\n\n`SPILL` is emitted from all five spill recipes (auto-spill,\n`bm25_spill_index`, VACUUM bulkdelete, PRE_COMMIT bulk-load,\nglobal-memory eviction). Each spill entry point also has a\n`RecoveryInProgress()` guard so a standby never autonomously spills.\nEmission is gated on `RelationNeedsWAL(rel)` so UNLOGGED and TEMP\nindexes do not ship records that cannot apply on a standby.\n\nThe full invariants and scenario-by-scenario verification of the\ncold-start bootstrap path are spec'd at\n[`docs/replication/CORRECTNESS.md`](docs/replication/CORRECTNESS.md).\nRead that before changing `tp_rebuild_index_from_disk`.\n\n## Notes on the tricky parts\n\n- **Corpus stats.** The bootstrap writes `total_docs = metap.total +\nmemtable_count` via `pg_atomic_write` under the per-index exclusive\nlock, not `fetch_add`. The two halves are disjoint by construction\n(segments versus the memtable), so the write is unambiguous; `fetch_add`\nwould double-count any cohort that redo applied during the WAL-replay\ndrain and that a subsequent SPILL also folded into `metap.total_docs`.\nThe PITR \"BM25 returns zero\" symptom in #350 was the pre-fix\nunconditional-overwrite case.\n\n- **Concurrent bootstrap.** Two backends arriving at the same empty\nregistry slot resolve to the same shared state via\n`tp_registry_register_if_absent` (atomic register-or-attach), not to a\nunregister/re-register race.\n\n- **Defensive WAL decoding.** Header-length checks before any field\nderef; bounds check on `MERGE_LINKAGE.level` before mutating\n`metap.level_heads/level_counts`; designated initializers on all\nemission headers so compiler padding is zeroed.\n\n- **`bm25_force_merge` locking.** `tp_xlog_merge_linkage` requires the\ncaller to hold the per-index `LW_EXCLUSIVE`; this serializes the\nstandby's redo against in-flight scans (#349). All other call paths take\nthat lock first, but `bm25_force_merge` was a pre-existing exception: it\nheld only `RowExclusiveLock` on the relation and called the merge helper\ndirectly. The SQL function now wraps the force-merge body in\n`tp_acquire_index_lock(LW_EXCLUSIVE)` / `tp_release_index_lock`.\n\n## Testing\n\n| Suite | New coverage |\n|---|---|\n| `replication_spill_paths.sh` | 4 tests: `tp_xlog_spill` from each\nspill recipe, cold-start corpus stats (#350), `MERGE_LINKAGE` round-trip\n(#349) |\n| `wal_audit.sh` | 3 tests: `RelationNeedsWAL` guard, `tp_rmgr_identify`\nnames, `INSERT_TERMS` records present in WAL window |\n| `recovery.sh` | `test_empty_index_rebuild`: empty index + restart\nfires the bootstrap empty-index branch |\n| `unlogged_index.sql` | UNLOGGED merge through `merge.c` GenericXLog\nfallback; delete + VACUUM + force-merge to drive shrinkage on that path\n|\n\nExisting replication suite (`replication.sh`,\n`replication_correctness.sh`, `replication_concurrency.sh`,\n`replication_failover.sh`, `replication_cascading.sh`,\n`replication_compat.sh`, `replication_parallel_build.sh`,\n`replication_issue_342.sh`) all pass.",
          "timestamp": "2026-05-06T02:20:08Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bdd3b317af4007bfca6d1b43ff8446fb55d0e2ae"
        },
        "date": 1778141387687,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2536.711853,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.394,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4639.402868,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.431,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7711.24332,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.519,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11644.521144,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.687,
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
          "id": "bdd3b317af4007bfca6d1b43ff8446fb55d0e2ae",
          "message": "fix: replicate memtable mutations via custom rmgr (#347)\n\nCloses #345, #349, #350.\n\n## What was broken\n\n`pg_textsearch` keeps the working set of recent docs in a DSA\nshared-memory memtable (one per index, shared across backends). Pre-PR,\nthe memtable on a streaming standby was populated only by the standby's\nown first-access rebuild. After that, primary writes did not propagate:\nthey updated docid pages on disk via GenericXLog but never touched the\nstandby's in-memory memtable. Three bugs followed.\n\n- **#345**: a long-lived standby backend's BM25 query results were\nfrozen at the time of its first index access. Subsequent primary inserts\nwere invisible until the backend reconnected.\n- **#349**: when the primary compacted L0 segments into L1, the standby\nreceived the metapage unlink/link via GenericXLog (buffer locks only),\nbut a backend mid-walk on the L0 chain followed `next_segment` pointers\ninto pages that the FSM had since recycled. Symptoms ranged from wrong\ncounts to `ERROR: invalid page index at block N / magic=0x00000000`.\n- **#350**: `total_docs` and `total_len` (the BM25 corpus stats that\ndrive IDF and BMW) were never WAL-logged. After PITR to a between-spills\nLSN, the recovered cluster's metapage held the last spill's snapshot,\nthe in-memory atomics started at zero, and the standard query path\nreturned zero results until a `REINDEX`.\n\n## Summary\n\nA custom WAL resource manager (rmgr ID 149,\n[registered](https://wiki.postgresql.org/wiki/CustomWALResourceManagers))\nemits three record types so streaming-replication standbys and primary\ncrash recovery keep their in-shared-memory memtable in sync with the\nsegment chain on disk.\n\n| Record | Standby redo |\n|---|---|\n| `INSERT_TERMS` | Apply the v2 bm25vector to the memtable (idempotent\nby CTID). |\n| `SPILL` | Apply the L0 chain-link, optional new-segment splice, and\nmemtable clear, atomically under per-index `LW_EXCLUSIVE`. |\n| `MERGE_LINKAGE` | Apply the metap unlink/link, optional new-segment\nsplice, and corpus-stat shrinkage, atomically under `LW_EXCLUSIVE`. |\n\n`SPILL` is emitted from all five spill recipes (auto-spill,\n`bm25_spill_index`, VACUUM bulkdelete, PRE_COMMIT bulk-load,\nglobal-memory eviction). Each spill entry point also has a\n`RecoveryInProgress()` guard so a standby never autonomously spills.\nEmission is gated on `RelationNeedsWAL(rel)` so UNLOGGED and TEMP\nindexes do not ship records that cannot apply on a standby.\n\nThe full invariants and scenario-by-scenario verification of the\ncold-start bootstrap path are spec'd at\n[`docs/replication/CORRECTNESS.md`](docs/replication/CORRECTNESS.md).\nRead that before changing `tp_rebuild_index_from_disk`.\n\n## Notes on the tricky parts\n\n- **Corpus stats.** The bootstrap writes `total_docs = metap.total +\nmemtable_count` via `pg_atomic_write` under the per-index exclusive\nlock, not `fetch_add`. The two halves are disjoint by construction\n(segments versus the memtable), so the write is unambiguous; `fetch_add`\nwould double-count any cohort that redo applied during the WAL-replay\ndrain and that a subsequent SPILL also folded into `metap.total_docs`.\nThe PITR \"BM25 returns zero\" symptom in #350 was the pre-fix\nunconditional-overwrite case.\n\n- **Concurrent bootstrap.** Two backends arriving at the same empty\nregistry slot resolve to the same shared state via\n`tp_registry_register_if_absent` (atomic register-or-attach), not to a\nunregister/re-register race.\n\n- **Defensive WAL decoding.** Header-length checks before any field\nderef; bounds check on `MERGE_LINKAGE.level` before mutating\n`metap.level_heads/level_counts`; designated initializers on all\nemission headers so compiler padding is zeroed.\n\n- **`bm25_force_merge` locking.** `tp_xlog_merge_linkage` requires the\ncaller to hold the per-index `LW_EXCLUSIVE`; this serializes the\nstandby's redo against in-flight scans (#349). All other call paths take\nthat lock first, but `bm25_force_merge` was a pre-existing exception: it\nheld only `RowExclusiveLock` on the relation and called the merge helper\ndirectly. The SQL function now wraps the force-merge body in\n`tp_acquire_index_lock(LW_EXCLUSIVE)` / `tp_release_index_lock`.\n\n## Testing\n\n| Suite | New coverage |\n|---|---|\n| `replication_spill_paths.sh` | 4 tests: `tp_xlog_spill` from each\nspill recipe, cold-start corpus stats (#350), `MERGE_LINKAGE` round-trip\n(#349) |\n| `wal_audit.sh` | 3 tests: `RelationNeedsWAL` guard, `tp_rmgr_identify`\nnames, `INSERT_TERMS` records present in WAL window |\n| `recovery.sh` | `test_empty_index_rebuild`: empty index + restart\nfires the bootstrap empty-index branch |\n| `unlogged_index.sql` | UNLOGGED merge through `merge.c` GenericXLog\nfallback; delete + VACUUM + force-merge to drive shrinkage on that path\n|\n\nExisting replication suite (`replication.sh`,\n`replication_correctness.sh`, `replication_concurrency.sh`,\n`replication_failover.sh`, `replication_cascading.sh`,\n`replication_compat.sh`, `replication_parallel_build.sh`,\n`replication_issue_342.sh`) all pass.",
          "timestamp": "2026-05-06T02:20:08Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/bdd3b317af4007bfca6d1b43ff8446fb55d0e2ae"
        },
        "date": 1778226448718,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2848.569455,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.351,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5497.31337,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.364,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 9156.745806,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.437,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9674.597808,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.827,
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
          "id": "67af30f909398eb5ce88981b8faa92c59eefbc47",
          "message": "Restore standard installcheck behavior (#353)\n\n## Summary\n- remove the custom `installcheck` override so PGXS owns the target\nagain\n- keep SQL regression coverage available through `make installcheck` /\n`make test`\n- keep shell-script coverage available through `make test-shell` and\n`make test-all`\n- update Makefile help, CLAUDE.md, and test README to describe the\ntarget split\n\n## Motivation\nDownstream consumers expect PGXS `installcheck` to run SQL regression\ntests without triggering extension-specific shell scripts. The previous\noverride made `installcheck` run both SQL regression and shell tests,\nwhich forced downstream harnesses to avoid the standard target.\n\nUpstream shell-script coverage remains explicit through local targets\nand existing GitHub Actions steps.\n\n## Validation\n- `git diff --check`\n- `make -n installcheck` shows PGXS pg_regress only\n- `make -n test-all` shows SQL regression plus shell-script targets\n- searched for stale docs claiming `installcheck` runs all tests\n- code review + re-review",
          "timestamp": "2026-05-09T01:17:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/67af30f909398eb5ce88981b8faa92c59eefbc47"
        },
        "date": 1778312966497,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2505.576963,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.399,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4717.839179,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.424,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7586.840596,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.527,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11991.887467,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.667,
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
          "id": "29d7438717689bbb7796d27e09c1b03e77cfb88a",
          "message": "ci(benchmark): add step timeouts + diagnostic capture so hangs are debuggable (#354)\n\n## Problem\n\nThe nightly **Benchmarks** workflow has been failing intermittently for\nweeks, all hanging on the same step (`Run MS MARCO concurrent insert\nqueries`) on **bucket-7 (7-token) MS MARCO queries**:\n\n| Date | SHA | Result |\n|---|---|---|\n| 2026-04-27 | `35a2dc63` | ❌ cancelled (pre-rmgr) |\n| 2026-05-01 | `142b32d8` | ❌ cancelled (pre-rmgr) |\n| 2026-05-02–03 | `7f7bd41a` | ❌ × 2 |\n| 2026-05-06 | `bdd3b317` | ✅ |\n| 2026-05-07–08 | `bdd3b317` | ❌ × 2 |\n| 2026-05-09 | `67af30f9` | ❌ |\n\nEach run waited the full **6-hour job-level timeout**, was then\ncancelled by GitHub Actions, and because cancellation skips `if:\nalways()` cleanup/upload steps, **no `postgres.log`, `pg_stat_activity`,\nor `pg_locks` was ever uploaded**. We have zero diagnostic data to\nroot-cause the underlying hang.\n\n## What this PR changes\n\nThis PR makes the workflow fail fast on hangs and preserve evidence so\nthe next failure tells us exactly what is stuck.\n\n### 1. Step-level `timeout-minutes` on all long-running insert-benchmark\nsteps\n\nSized at **~2–3× observed durations** from the last successful run (May\n6):\n\n| Step | Observed | Timeout | Headroom |\n|---|---|---|---|\n| Run MS MARCO insert benchmark | 13 min | 45 min | 3.4× |\n| Run MS MARCO concurrent insert | 26 min | 60 min | 2.3× |\n| **Run MS MARCO concurrent insert queries** (the hang-prone step) | 37\ns | 30 min | huge |\n| Run Wikipedia insert / concurrent | <1 min ea. | 30 min | huge |\n| Validate / Cranfield steps | <1 min ea. | 10–15 min | huge |\n\nWhen a step hits its timeout it **FAILS** (rather than the whole job\nbeing cancelled), which lets subsequent `if: always()` steps run.\n\n### 2. New `Capture diagnostics on failure` step (`if: failure()`)\n\nDumps `pg_stat_activity`, `pg_locks`, `tail -n 500\ntmp_bench/postgres.log`, and **gdb stack traces of any still-running\nbackends** to `diagnostics.log`. The gdb stacks are especially important\nhere because pg_textsearch's BMW scan loop currently has no\n`CHECK_FOR_INTERRUPTS`, so `statement_timeout` / `pg_cancel_backend()`\ncannot interrupt a hung scan — gdb is the only way to see where it's\nstuck.\n\n### 3. Reorder `Format insert metrics` before `Upload insert benchmark\nresults`\n\nThe generated `*_action.json` files were previously listed in the upload\n`path:` but produced *after* the upload step ran, so they were never\nactually archived. Pure bug.\n\n### 4. Add `if: always()` to the upload step\n\n**The single most important change for next-time debuggability.**\nWithout it, GitHub Actions skips this step on cancellation/failure and\nwe lose `postgres.log` + `diagnostics.log`.\n\n### 5. Add `SET statement_timeout = 5min` to query SQL files\n\nDefense in depth in\n`benchmarks/datasets/{msmarco,wikipedia}/queries.sql` and\n`benchmarks/sql/cranfield/02-queries.sql`. Note: this **cannot**\ninterrupt a C-loop hang in BMW (no `CHECK_FOR_INTERRUPTS` there) but\ndoes catch plpgsql / planner-side runaways.\n\n## What this PR explicitly does **not** fix\n\nThe underlying bucket-7 BMW hang itself. That bug:\n- **Pre-dates** the rmgr/replication work (pre-rmgr commits `35a2dc63`,\n`7f7bd41a` cancelled in the same step before any WAL/replication PR\nlanded).\n- Is intermittent / data-dependent (same SHA `bdd3b317` succeeded once\nand hung twice on subsequent runs).\n- Cannot be debugged from current run logs because we have no artifact.\n\nI will file a tracking issue documenting the symptom and the missing\n`CHECK_FOR_INTERRUPTS` in `src/access/scan.c` / `src/scoring/bmw.c`\n(which is the reason hangs are uninterruptible — likely worth a separate\nsmall PR independently of root-causing the bucket-7 case).\n\n## Verification\n\n- [x] `actionlint .github/workflows/benchmark.yml` — clean.\n- [x] `python3 -c \"import yaml; yaml.safe_load(...)\"` — parses.\n- [x] All `timeout-minutes` values comfortably fit within the 6h job\ntimeout (sum ≈ 5h 25min worst case).\n- [x] Step ordering verified: extract → format → upload → publish.\n\nCo-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>\n\n---------\n\nCo-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>",
          "timestamp": "2026-05-10T04:43:51Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/29d7438717689bbb7796d27e09c1b03e77cfb88a"
        },
        "date": 1778399765343,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2615.908454,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.382,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4889.912304,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.409,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7794.429926,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.513,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8869.816133,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.902,
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
          "id": "be2f453d1c62bfb3a0c87f8886b5a002758ba9dc",
          "message": "fix: chunked tokenization for oversized documents (#348)\n\n## Summary\n- Documents whose unique-token volume exceeds Postgres's `tsvector` 1 MB\nlexeme-dictionary cap (`MAXSTRPOS`) previously failed `CREATE INDEX`,\n`INSERT` (aminsert), `VACUUM` rebuild, `to_tpvector`, and standalone\n`<@>` scoring with `ERROR: string is too long for tsvector (N bytes, max\n1048575 bytes)`.\n- New `tp_tokenize_text` helper splits inputs >256 KB on ASCII\nwhitespace (UTF-8-safe byte fallback when no whitespace is present),\ntokenizes each chunk via `to_tsvector_byid`, and merges per-chunk\n`(term, freq)` arrays via sort + collapse. Single-chunk fast path is\nunchanged.\n- All five tokenization sites (`build.c` ×2, `build_parallel.c`,\n`vacuum.c`, `types/vector.c::to_tpvector`) route through the helper. The\nstandalone scoring path in `bm25_text_bm25query_score` was also\nrefactored to use the helper plus a sorted-array term lookup, since\n`find_term_frequency` previously walked the tsvector directly and would\nhave re-hit the same 1 MB cap on seq-scan rows.\n- Whitespace splitting is correct for whitespace-delimited scripts\n(Latin, Cyrillic, Greek, Arabic). Non-whitespace-delimited scripts (CJK,\nThai) get byte/codepoint splits — acceptable because Postgres's default\ntext-search parser doesn't emit per-word tokens for those scripts.\nDocumented in README.\n\n## Trigger\nThe bug requires *unique-token volume*, not raw byte size:\n`repeat('hello world ', 100000)` is 1.2 MB but only 2 lexemes and\nindexes fine. The repro and tests use ~250 K distinct tokens (~1.9 MB\nraw text).\n\n## Testing\n- New \\`test/sql/large_documents.sql\\` covers \\`CREATE INDEX\\`,\n\\`INSERT\\`, index-scan query, seq-scan / standalone-scoring query, and\n\\`VACUUM\\` on a 2 MB+ many-unique-token document.\n- Full \\`make installcheck\\` (60 tests) passes locally on PG 17.",
          "timestamp": "2026-05-11T02:44:56Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/be2f453d1c62bfb3a0c87f8886b5a002758ba9dc"
        },
        "date": 1778487974386,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2375.221218,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.421,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4581.750969,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.437,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7744.386187,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.517,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11947.611912,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.67,
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
          "id": "e50c7997d3a6e9403d535c023740b6c8821949e6",
          "message": "fix: sort posting list when spilling memtable (root-cause MS MARCO bucket-8 hang) (#360)\n\n## Summary\n\nRoot-cause and fix the MS MARCO bucket-8 hang in production benchmarks\n([nightly run\n25648758099](https://github.com/timescale/pg_textsearch/actions/runs/25648758099)),\nplus close the watchdog gap that allowed the underlying corruption to\npersist undetected.\n\n## Root cause: unsorted block_postings in spilled segments\n\n`tp_write_segment` (memtable → segment spill path) iterates a term's\nposting-list entries in array order and writes them as\n`block_postings[]` *without sorting*. This is fine under single-writer\nCOPY/CREATE INDEX, because entries are appended in CTID order, which\nmatches `doc_id` order after `tp_docmap_finalize`. Under **concurrent\ninserts**, multiple backends interleave appends to the same posting list\nin arbitrary thread-scheduling order:\n\n```c\n// src/memtable/posting.c:191\nposting_list->is_sorted = false;  /* New entry may break sort order */\n```\n\nThat `is_sorted` flag is set on every insert but **never set back to\n`true`** anywhere — there's no sort step on the read side. The spilled\nsegment's `block_postings` then violate the sorted-block invariant the\nentire BMW machinery relies on (binary search on `block_last_doc_ids`,\n`seek_term_to_doc` finding \"first doc ≥ target\", `find_wand_pivot`'s\nsmallest-first walk), and merges *propagate* the corruption to higher\nlevels.\n\n### Diagnostic evidence\n\nA temporary debug function (`bm25_check_segment_consistency`, removed in\nthe final commit once root cause was fixed) walked every segment / term\n/ block and compared cached skip-data against actual on-disk\n`block_postings`. Run against the failing benchmark's MS MARCO corpus\nbefore the fix:\n\n```\nChecked 2 segments, 4,080,947 terms, 5,776,365 blocks.\nTotal inconsistencies: 904,174\n```\n\nAll examples were \"not strictly ascending within block\", with delta\nmagnitudes up to ~200,000 doc_ids — genuine out-of-order data, not a\none-byte glitch. (Check #1 — `skip.last_doc_id ≠\npostings[doc_count-1].doc_id` — fired zero times, so the skip *metadata*\nwas always consistent with what got written; what got written was the\nbug.)\n\n## Fix\n\n### 1. Sort posting list by doc_id when spilling memtable to segment\n\n`src/segment/segment.c` — `qsort(block_postings, doc_count, ...,\ncmp_by_doc_id)` after building it in `tp_write_segment`, before\nsplitting into `TP_BLOCK_SIZE` blocks. `doc_id` is monotonic with CTID\nafter `tp_docmap_finalize`, so this is equivalent to sorting by CTID —\nthe invariant the segment format documents and the merge / scoring code\nassumes. `build_context.c` is unaffected (EXPULL streams already-sorted\nentries).\n\n### 2. Harden `seek_term_to_doc` to actually scan multiple blocks\n(defense-in-depth)\n\n`src/scoring/bmw.c` — The old code had two unverified fall-through paths\nthat loaded *one* \"next\" block and returned `true` based purely on\n`!iter.finished`, without verifying that the newly loaded block's first\ndoc actually reached `target_doc_id`. Under correct invariants those\nfall-throughs are unreachable, but with the segment-sort bug they fired\nand `seek_to_pivot`'s `i--; continue;` re-entry spun forever (the\ngdb-observed bucket-8 hang at `bmw.c:1329`).\n\nRestructured so the fast path and binary-search path both just\n*position* the iterator at a candidate starting block, then a single\nblock-advancing scan loop keeps advancing until it finds a posting `>=\ntarget` (returns `true`) or exhausts the iterator. The post-condition\n`cur_doc_id >= target_doc_id` is now guaranteed by control flow.\n\nEven with the segment-sort root cause fixed, this is a worthwhile\ncorrectness improvement — the original `seek_term_to_doc` was latently\nincorrect for *any* skip-data inconsistency.\n\n### 3. `CHECK_FOR_INTERRUPTS` in BMW hot loops\n\n`src/scoring/bmw.c` — Added in two places:\n- Inside the new `for(;;)` block-advancing loop in `seek_term_to_doc`\n(per-iter disk I/O + decompression should be cancelable).\n- Inside `seek_to_pivot`'s `for` loop (the prior PR #355 added CFI only\nto the outer WAND main loop; if `seek_to_pivot` itself spins, we never\nreturn there).\n\n### 4. Validation watchdog actually validates\n\nThe corruption above existed for as long as concurrent inserts have, and\nwe had a validation step (`validate_queries.sql` against\n`ground_truth.tsv`) explicitly designed to catch this class of\ncorrectness regression. **It never fired.** Two latent bugs:\n\n- `validate_queries.sql` had an ambiguous-column reference (`WHERE\nquery_id = p_query_id` where `query_id` shadows a plpgsql variable).\npsql errored out partway through with `ON_ERROR_STOP`.\n- Every `Validate ...` workflow step pipes `psql` through `tee` (no `set\n-o pipefail`), then `grep -q \"VALIDATION FAILED\"` to decide pass/fail.\nWhen the SQL errors before reaching the FAILED marker, psql's non-zero\nexit is swallowed by `tee`, the grep finds nothing, and the step claims\nsuccess.\n\nFixed in `benchmarks/datasets/msmarco/validate_queries.sql` (qualify as\n`ground_truth.query_id`) and in `.github/workflows/benchmark.yml` (6\naffected `Validate ...` steps): each now sets `set -o pipefail` and\nrequires an explicit `VALIDATION PASSED` marker — absence of which fails\nthe step. Future SQL-level errors in validation will fail the run loudly\ninstead of silently passing. (Cranfield has no validation step in the\nworkflow at all; out of scope.)\n\n## Verification\n\n- ✅ Built clean on PG 17 / PG 18 (`-O2 -g`, no new warnings)\n- ✅ All **61** regression tests pass via `pg_regress`\n- ✅ `make format-check` clean\n- ✅ Local stress repro: 8-thread pgbench concurrent inserts of 4000 docs\nover 8 terms → spill → consistency check (during development). Before\nthis fix: thousands of inconsistencies. After: 0.\n- ✅ **First successful end-to-end benchmark run**\n[25689967666](https://github.com/timescale/pg_textsearch/actions/runs/25689967666):\n- `Run MS MARCO insert benchmark` completed in seconds (was 32m+ hang) —\nbucket-8 p99=59ms, n=100, results=1000\n- `Run MS MARCO concurrent insert` (8-thread pgbench, 8.8M passages) ran\nthe full query suite + validation — first time these steps have ever\nreached completion",
          "timestamp": "2026-05-12T00:19:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/e50c7997d3a6e9403d535c023740b6c8821949e6"
        },
        "date": 1778574799427,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2997.290668,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.334,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5464.987821,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.366,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 9119.800359,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.439,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9546.339613,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.838,
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
          "id": "62308b82a5e514320d0802beab2ebe4ea33d751b",
          "message": "ci(benchmark): wikipedia validate by per-rank score, not per-doc set (#372)\n\nPort the per-rank score validation approach from msmarco (#366) to the\nWikipedia validator.\n\n## Motivation\n\nBenchmark run\n[25745376667](https://github.com/timescale/pg_textsearch/actions/runs/25745376667)\non `release-1.2.0` failed Wikipedia validation. Of 80 queries:\n- **19** had `docs_match=f, scores_match=t, max_abs_diff=0.000000` —\npure tie-cluster doc-set differences (BMW vs ground-truth tie-break\nordering). All matched on scores.\n- **1** had a real per-rank score divergence beyond 4 decimal places.\n\nThe old script used doc-set comparison plus 4-decimal rounding and\ntreated *any* divergence as fatal. This produces the same tie-cluster\nfalse-positive class that #366 fixed for MS MARCO.\n\n## Change\n\nWikipedia validator now mirrors `msmarco/validate_queries.sql`\nstructurally:\n- Per-rank score comparison (rank 1..10) within **0.001** absolute\ntolerance — same threshold and shape as msmarco.\n- `LIMIT 10` is held inside the inner subquery so it propagates to the\nBM25 index scan (BMW K=10). See #365 for the trap of letting the planner\npark the LIMIT above a WindowAgg.\n- Doc-set match is still reported as supplementary diagnostic info, but\nno longer part of the pass/fail decision.\n- Allowlist mechanism with tracking-issue references, starting empty for\nWikipedia.\n\n## Behavior\n\nRe-running run 25745376667 with this change would have:\n- ✅ Eliminated all 19 tie-cluster false positives.\n- ✅ Still flagged the 1 real divergence (or passed it if its abs diff is\n≤ 0.001 — the same tolerance applied to msmarco, which is the\nestablished standard).\n\n## Notes\n\n`benchmarks/datasets/msmarco-v2/validate_queries.sql` still uses the\nlegacy doc-set logic and should likely receive the same treatment. Out\nof scope for this PR to keep the diff focused on what failed on run\n25745376667.\n\nFollow-up to #366.",
          "timestamp": "2026-05-13T00:37:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/62308b82a5e514320d0802beab2ebe4ea33d751b"
        },
        "date": 1778661173625,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2551.105954,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.392,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4679.875105,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.427,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7732.182658,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.517,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 11535.459956,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.694,
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
          "id": "62308b82a5e514320d0802beab2ebe4ea33d751b",
          "message": "ci(benchmark): wikipedia validate by per-rank score, not per-doc set (#372)\n\nPort the per-rank score validation approach from msmarco (#366) to the\nWikipedia validator.\n\n## Motivation\n\nBenchmark run\n[25745376667](https://github.com/timescale/pg_textsearch/actions/runs/25745376667)\non `release-1.2.0` failed Wikipedia validation. Of 80 queries:\n- **19** had `docs_match=f, scores_match=t, max_abs_diff=0.000000` —\npure tie-cluster doc-set differences (BMW vs ground-truth tie-break\nordering). All matched on scores.\n- **1** had a real per-rank score divergence beyond 4 decimal places.\n\nThe old script used doc-set comparison plus 4-decimal rounding and\ntreated *any* divergence as fatal. This produces the same tie-cluster\nfalse-positive class that #366 fixed for MS MARCO.\n\n## Change\n\nWikipedia validator now mirrors `msmarco/validate_queries.sql`\nstructurally:\n- Per-rank score comparison (rank 1..10) within **0.001** absolute\ntolerance — same threshold and shape as msmarco.\n- `LIMIT 10` is held inside the inner subquery so it propagates to the\nBM25 index scan (BMW K=10). See #365 for the trap of letting the planner\npark the LIMIT above a WindowAgg.\n- Doc-set match is still reported as supplementary diagnostic info, but\nno longer part of the pass/fail decision.\n- Allowlist mechanism with tracking-issue references, starting empty for\nWikipedia.\n\n## Behavior\n\nRe-running run 25745376667 with this change would have:\n- ✅ Eliminated all 19 tie-cluster false positives.\n- ✅ Still flagged the 1 real divergence (or passed it if its abs diff is\n≤ 0.001 — the same tolerance applied to msmarco, which is the\nestablished standard).\n\n## Notes\n\n`benchmarks/datasets/msmarco-v2/validate_queries.sql` still uses the\nlegacy doc-set logic and should likely receive the same treatment. Out\nof scope for this PR to keep the diff focused on what failed on run\n25745376667.\n\nFollow-up to #366.",
          "timestamp": "2026-05-13T00:37:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/62308b82a5e514320d0802beab2ebe4ea33d751b"
        },
        "date": 1778746315106,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2783.92098,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.359,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5392.609233,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.371,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 9132.808239,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.438,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9364.448672,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.854,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1778832879347,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2460.758156,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.406,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4309.733745,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.464,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6670.179977,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.6,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8199.778177,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.976,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1778917938008,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2708.812566,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.369,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4941.450211,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.405,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7354.85458,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.544,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7927.453378,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.009,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779004865815,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2347.72094,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.426,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4284.75368,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.467,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6607.097138,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.605,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7937.580656,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.008,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779093528824,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2362.641809,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.423,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4284.750724,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.467,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6506.21797,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.615,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7788.031896,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.027,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779179158549,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2407.685767,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.415,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4418.8444,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.453,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6649.173926,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.602,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8269.941905,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.967,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779265506245,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2470.951614,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.405,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4418.028882,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.453,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6790.70886,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.589,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8504.462729,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.941,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779352127654,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2395.048558,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.418,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4485.734049,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.446,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6714.379437,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.596,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8137.489264,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.983,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779438171793,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2712.238148,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.369,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 5013.763578,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.399,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7576.611282,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.528,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9281.150242,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.862,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779523276722,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2391.596272,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.418,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4292.455241,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.466,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6485.73495,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.617,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8030.785427,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.996,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779610217351,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2443.672618,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.409,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4466.012488,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.448,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6778.943751,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.59,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8444.087891,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.947,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779698776378,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2361.797511,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.423,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4265.318188,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.469,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6501.265676,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.615,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8190.380155,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.977,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779784010663,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2287.809636,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.437,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4146.120902,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.482,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6464.719936,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.619,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7788.858092,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.027,
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
          "id": "023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b",
          "message": "Replace shared-memory memtable with on-disk paged structure (#374) (#375)\n\nReplaces the shared-memory memtable with an on-disk paged structure,\nremoving the custom WAL resource manager and the docid recovery pages it\nrequired.\n\nCloses #374. (Supersedes the design proposed there, which kept an\nin-memory memtable behind custom-but-page-less WAL records.)\n\n## Motivation\n\nThe original L0 layer was a shared-memory inverted index (DSA-backed\ndshash tables) replayed on standbys via a custom rmgr (ID 149). This\nworked on a regular streaming standby — extension loaded, rmgr\nregistered, redo applies to the in-shmem memtable — but it broke\nPostgreSQL's stateless WAL-redo helper used for single-page\nreconstruction: that environment has no shared memory and cannot load\n`pg_textsearch.so`, so `INSERT_TERMS` redo is undefined and\n`SPILL`/`MERGE_LINKAGE` are brittle.\n\nRather than re-engineer custom WAL records to be page-only, this PR\nremoves the in-memory memtable entirely. The memtable becomes a chain of\npages in the index relation itself, mutated under buffer locks,\nWAL-logged via `GenericXLog`. Walredo and any other stateless replay\nenvironment reconstructs every page without loading the extension.\n\n## What changes\n\n- **Memtable storage.** A chain of pages rooted at the metapage's new\n`(memtable_head_blkno, memtable_tail_blkno)` fields. Each page has a\nmagic header (`TP_MEMTABLE_PAGE_MAGIC`) and a sequence of\nvariable-length doc records (`ctid, doc_length, vector_len,\nvector_bytes`). Oversized `TpVector` payloads are stored as a FRAGMENT\nhead + N CONTINUATION pages, reassembled by the reader.\n- **Write path.** `tp_add_document_terms` builds a `TpVector` and calls\n`tp_memtable_append`, which extends or appends to the tail page under a\nsingle `GenericXLog` record. Per-index `LW_SHARED` is held for the txn;\nthe tail buffer is taken `EXCLUSIVE` only while the record is being\nwritten.\n- **Read path.** `chain_source` walks the chain under the per-index\n`LW_SHARED`. Scoring and query-time `doc_freq` paths take a\n`TpDataSource *` so they consult the chain instead of dshash.\n- **Spill.** `tp_do_spill` extracts the chain via `chain_source`, builds\nan L0 segment via `tp_write_segment`, then one `GenericXLog` over\n`{metapage, new segment header}` publishes the result. Held under\n`LW_EXCLUSIVE`.\n- **Custom rmgr deleted.** `src/replication/rmgr.c` is gone, the call to\n`tp_register_rmgr()` in `_PG_init` is removed, and the `INSERT_TERMS` /\n`SPILL` / `MERGE_LINKAGE` record types no longer exist. Slot 149 is\nintentionally retired.\n- **`pg_textsearch.memory_limit` and `bm25_memory_usage()` removed.**\nThe soft-limit byte-accounting was DSA-specific and not meaningful for\nthe page-backed layer. Replaced by a chain-page-count trigger\n`pg_textsearch.memtable_pages_threshold` (default 64). Postgres'\nshared-buffer pressure is the real cap on memtable size now; an\noversized chain just gets paged out by the buffer manager like any other\nrelation.\n- **`MarkGUCPrefixReserved(\"pg_textsearch\")`** added to `_PG_init` so\nstale `postgresql.conf` entries for the removed GUCs are flagged at\nserver start instead of being silently ignored.\n- **PARALLEL UNSAFE** marker added to `bm25_text_bm25query_score` and\n`bm25_textarray_bm25query_score`: the chain source needs a stable\nper-index lock for the lifetime of the query.\n\n## Spec\n\nSee [`docs/memtable_v2.md`](docs/memtable_v2.md) for the on-disk page\nformat, write/read/spill flow, concurrency contract, and WAL replay\nstory. Replaces the obsolete `docs/replication/CORRECTNESS.md`.\n\n## Performance (this PR vs. main 1.2.x, default GUCs)\n\n### Bulk path\n\n| Path                        | main 1.2.x | this PR  | delta    |\n|-----------------------------|------------|----------|----------|\n| 50k bulk INSERT             | 2.48s      | 2.20s    | **-11%** |\n| 10k INSERT (post-spill)     | 232ms      | 204ms    | **-12%** |\n\n### Read latency vs. memtable chain size\n\nBy design, read latency scales with the size of the in-flight memtable\nchain (see [`docs/memtable_v2.md`](docs/memtable_v2.md)). How full the\nchain is allowed to get is a per-deployment tradeoff governed by\n`pg_textsearch.memtable_pages_threshold` (default 64).\n\n| Memtable size                 | Chain pages | this PR  |\n|-------------------------------|-------------|----------|\n| 0 docs (segments-only)        | 0           | 2–5 ms   |\n| 2k docs (default threshold)   | 64          | 3.2 ms   |\n| 10k docs                      | 255         | 14 ms    |\n| 30k docs                      | 960         | 59 ms    |\n\nGH `Benchmarks` workflow run for full numbers vs. the most recent\nsuccessful `main` scheduled run is in the comments below.\n\n## Upgrade\n\n`TP_METAPAGE_VERSION` is bumped 6 → 7. **v6 metapages fail at open with\na clear `REINDEX INDEX` error**; there is no dual-format support. The\n`pg_textsearch--1.2.0--1.3.0-dev.sql` upgrade script handles the\nSQL-level side (drops the removed `bm25_memory_usage` function, marks\nscoring functions PARALLEL UNSAFE), but operators must `REINDEX` each\n`bm25` index to migrate v6 metapages to v7. The upgrade-tests CI matrix\nexercises this path for v1.2.0 (added in Batch 4 of post-review fixes).\n\n## Out of scope / follow-ups\n\n- #377 — Drop `shared_preload_libraries` requirement (subsumes registry\nteardown / `TpMemtable` deletion)\n- #378 — Release rmgr ID 149 from the PostgreSQL wiki\n- #379 — Reclaim orphaned chain pages after spill\n- #380 — Standby-safe horizon for displaced segment pages during merge\n(predates this PR)\n- #381 — Reduce WAL amplification for FRAGMENT chain-page writes\n- #382 — Tail-page contention on high-concurrency single-term inserts\n- #383 — Re-establish read compatibility for v6 metapages (avoid forced\nREINDEX on upgrade)\n\n\n## Testing\n\n- `make installcheck`: **64/64 green**.\n- `make format-check`: clean (clang-format 21.1.8).\n- `bash test/scripts/wal_audit.sh`: all 3 tests pass — verifies zero\ncustom rmgr records in any LSN window, including across a SIGKILL +\nrestart crash window.\n- `bash test/scripts/shutdown_spill.sh`: pass.\n- `bash test/scripts/docid_chain_recovery.sh`: pass (test now exercises\nthe new memtable chain; file rename deferred to a follow-up).\n- `pg_waldump` on the regression's traffic: zero rmgr ID 149 records.",
          "timestamp": "2026-05-15T01:11:44Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/023c3841b7d41fd3dbdbaa6f48ef3da46c9a362b"
        },
        "date": 1779870846076,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2694.030462,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.371,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4978.689937,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.402,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7594.815226,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.527,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 9164.894231,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.873,
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
          "id": "dfc15f0dcf577b420c098057e92de9e7e2b0c63f",
          "message": "fix(memtable): validate reassembled fragment payloads in release builds (#385)\n\n## What\n\nMake `tpvector_validate_v2_buffer()` unconditional on the\nfragment-reassembly\npath in `src/memtable/chain_source.c`. Inline-record validation stays\ngated by\n`#ifdef USE_ASSERT_CHECKING` (no perf change for the common case).\n\n## Why\n\nAfter `reassemble_fragment()` stitches a multi-page payload together,\n`ingest_terms()` decodes it via `tpvector_entry_decode_advance`, which\nassumes a well-formed v2 buffer. The structural validation was\n`#ifdef USE_ASSERT_CHECKING`-gated — so release builds had no defense\nagainst a malformed reassembled buffer.\n\nA structurally-malformed payload here would:\n1. OOB-read in the varint decoder\n2. Silently poison the per-term accumulators\n3. Get serialised into the next L0 segment at spill, surviving restarts\n\nThe author's perf rationale (page CRC + same-backend write) is sound for\n**inline** records but doesn't transfer to **fragments**:\n- the bytes are reassembled across multiple buffer-locked page reads,\n- a fragment is by definition larger than a single page, so per-record\n  validation cost is amortised over a much larger payload,\n- on a standby, the bytes come from WAL replay rather than our local\n  write.\n\n## How\n\nOne call site change in `chain_source.c` — call\n`tpvector_validate_v2_buffer(full, rec->vector_len)` right after\n`reassemble_fragment()` and before `ingest_terms()`. The validation\nfunction already `ereport`s `ERRCODE_DATA_CORRUPTED` on bad input, so\nthe index becomes detection-first rather than silent-corruption-first.\n\n## Testing\n\n- `make installcheck` — 64/64 pass\n- `make format-check` (clang-format 21.1.8) — clean\n\nA targeted regression test that injects a malformed fragment payload\nand asserts the read path errors out (suggested in the review as\nTEST-5b) is a follow-up; this PR is the minimal correctness fix.\n\n## Context\n\nIdentified by the v2 memtable hardening review of PR #374 (FRAG-1,\nMedium severity).\n\nCo-authored-by: Todd J. Green <1738591+tjgreen42@users.noreply.github.com>\nCo-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>",
          "timestamp": "2026-05-27T22:38:37Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/dfc15f0dcf577b420c098057e92de9e7e2b0c63f"
        },
        "date": 1779957309413,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2401.653662,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.416,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4388.377345,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.456,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6549.159708,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.611,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8175.022811,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.979,
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
          "id": "c9ee76e0fa7de6d4d987fd478011c33664a5fe55",
          "message": "test: add multi-backend reindex regression for issue #390 (#396)\n\nAdds `test/scripts/multi_backend_reindex.sh`, a multi-backend regression\ntest for [#390](https://github.com/timescale/pg_textsearch/issues/390)\n(\"BM25 index returns stale heap TIDs after heap rewrite\").\n\n## Why\n\nThe bug surfaced in v1.2.0 when one backend had already touched the BM25\nindex (priming its private `TpLocalIndexState` cache in\n`src/index/state.c`) and a second backend ran a statement that rewrites\nthe heap and reindexes BM25 — most commonly `ALTER TABLE ... ADD COLUMN\n... GENERATED ALWAYS AS (...) STORED`. v1.2.0 registered no\n`CacheRegisterRelcacheCallback`, so the orphaned in-memory memtable\nstayed alive in DSA, and a later spill from the primed backend wrote a\nsegment of pre-rewrite CTIDs into the new index file. Queries against\nthose stale CTIDs then failed with:\n\n```\nERROR: could not read blocks N..N in file \"base/<db>/<heap_fno>\": read only 0 of 8192 bytes\n```\n\n`main` (1.3.0-dev) is unaffected because the on-disk memtable design\nfrom #374/#375/#392 makes the bug unreachable: the heap rewrite swaps\nthe index relfilenode, the on-disk memtable chain is empty in the new\nfile, and any stale local-state pointer is harmless. This PR pins that\nbehaviour so the bug class cannot be reintroduced (e.g. by any future\nchange that revives shared-memory memtable state without proper\ninvalidation).\n\n## What it asserts\n\n1. Backend A's session reaches every phase marker and produces no `could\nnot read blocks` / `invalid (page|segment)` / `XX001` errors during the\nlate INSERT + forced spill.\n2. `bm25_summarize_index` `docs_persisted` equals the live heap row\ncount (catches a stale segment being added to the metapage).\n3. Top-100 BM25 scan for `'lorem'` (in every pre-ALTER row) completes\nwithout error and returns 100 rows.\n4. BM25 scan for `'moonlight'` (a token unique to post-ALTER late\ninserts, joined against the live heap on id) returns exactly 50 rows —\nproves the late-insert path correctly populated the new index file.\n5. The heap was actually compacted by ALTER (sanity check on the test\nsetup itself).\n\n## Setup notes\n\n- `fillfactor=10` + INSERT 30k + DELETE all but 1000 rows so the old\nheap spans ~4286 pages. Any stale CTID retained by the primed backend\npoints well beyond the new heap's EOF, turning a silent-wrong-data bug\ninto a deterministic read error.\n- Pinned `pg_textsearch.bulk_load_threshold = 0` and\n`pg_textsearch.memtable_pages_threshold = 0` so no implicit spill fires\nbefore the explicit `bm25_spill_index()` call.\n- Drives both backends from a single persistent `psql` session that\nspawns Backend B via `\\!`. This keeps Backend A's connection (and its\nprimed local-state cache) alive across the heap rewrite, exactly\nmatching the in-production race.\n- Unique per-PID data dir (`tmp_reindex_test_<port>_<pid>`) and a port\n(`55458`) not used by any other script; `TEST_PORT` is overridable via\nenv.\n\n## Local verification\n\n| Branch | Result |\n|---|---|\n| `regression-test-issue-390` (this PR, on `main`) | ✅ Pass |\n| `v1.2.0` | ❌ Fail with `ERROR: could not read blocks 638..638 in file\n\"base/16384/16447\": read only 0 of 8192 bytes` — exact issue #390\nsymptom |\n\n`make installcheck` is also clean on this branch (68/68).\n\n## Wiring\n\n- **Makefile:** new `test-reindex` target, added to `test-shell`\naggregate and `.PHONY`, plus a help-text line.\n- **.github/workflows/ci.yml:** appended to the shell-tests block next\nto `multi_index.sh` (one entry per supported PG version in the matrix).\n- **.github/workflows/sanitizer-build-and-test.yml:** appended to the\nsanitizer shell-tests block. Sanitizers are particularly valuable here\nbecause the v1.2.0 bug class was stale-pointer / use-after-realloc.\n\nRefs #390",
          "timestamp": "2026-05-29T02:03:55Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/c9ee76e0fa7de6d4d987fd478011c33664a5fe55"
        },
        "date": 1780043673999,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2689.795298,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.372,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4935.058191,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.405,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7451.286448,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.537,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8445.014849,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.947,
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
          "id": "f50e9831edffd97f4f1977448fe708c4575f7ebb",
          "message": "Release v1.3.0 (#399)\n\nRelease v1.3.0.\n\n## Headlines\n\n- **On-disk memtable** (#374, #375, #385, #389): the L0 memtable now\n  lives in the index relation itself as a chain of WAL-logged pages\n  via `GenericXLog`, replacing the shared-memory structure. Removes\n  the old soft-limit machinery and a long tail of physical-replication\n  edge cases. New metapage version (v7), read-compatible with v6 via\n  lazy upgrade. See [`docs/memtable_v2.md`](docs/memtable_v2.md).\n- **In-memory memtable cache** (#391, #392, #395): query reads can be\n  served from a per-backend cache built over the on-disk chain, with\n  a 3-tier memory cap (per-index / global soft / global hard).\n  Controlled by `pg_textsearch.memtable_cache_enabled` and\n  `pg_textsearch.memory_limit`. See\n  [`docs/memtable_cache.md`](docs/memtable_cache.md).\n- **Multi-backend reindex regression coverage** (#386, #390, #396):\n  fixes a stale-CTIDs class of bug that surfaced under ALTER TABLE\n  heap rewrites concurrent with memtable activity.\n- **CI hardening** (#372, #394).\n\n## Release checklist (from RELEASING.md)\n\n- [x] Audit `sql/pg_textsearch--1.2.0--1.3.0.sql` against the main\n  SQL diff. Covers 11 new CREATE FUNCTIONs (memtable + cache test\n  scaffolds), DROP of `bm25_memory_usage()`, and the two ALTER\n  FUNCTION ... PARALLEL UNSAFE changes (`bm25_text_bm25query_score`,\n  `bm25_textarray_bm25query_score`).\n- [x] Ran `./scripts/bump-version.sh 1.3.0-dev 1.3.0`.\n- [x] Replaced banner image (`images/tapir_and_friends_v1.3.0.png`).\n- [x] `1.2.0` is already present in the upgrade-tests matrix (added\n  during the dev-bump in #373).",
          "timestamp": "2026-05-29T20:35:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f50e9831edffd97f4f1977448fe708c4575f7ebb"
        },
        "date": 1780128302667,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2311.871629,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.433,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4180.039544,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.478,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6343.981724,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.631,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7774.494201,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.029,
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
          "id": "f50e9831edffd97f4f1977448fe708c4575f7ebb",
          "message": "Release v1.3.0 (#399)\n\nRelease v1.3.0.\n\n## Headlines\n\n- **On-disk memtable** (#374, #375, #385, #389): the L0 memtable now\n  lives in the index relation itself as a chain of WAL-logged pages\n  via `GenericXLog`, replacing the shared-memory structure. Removes\n  the old soft-limit machinery and a long tail of physical-replication\n  edge cases. New metapage version (v7), read-compatible with v6 via\n  lazy upgrade. See [`docs/memtable_v2.md`](docs/memtable_v2.md).\n- **In-memory memtable cache** (#391, #392, #395): query reads can be\n  served from a per-backend cache built over the on-disk chain, with\n  a 3-tier memory cap (per-index / global soft / global hard).\n  Controlled by `pg_textsearch.memtable_cache_enabled` and\n  `pg_textsearch.memory_limit`. See\n  [`docs/memtable_cache.md`](docs/memtable_cache.md).\n- **Multi-backend reindex regression coverage** (#386, #390, #396):\n  fixes a stale-CTIDs class of bug that surfaced under ALTER TABLE\n  heap rewrites concurrent with memtable activity.\n- **CI hardening** (#372, #394).\n\n## Release checklist (from RELEASING.md)\n\n- [x] Audit `sql/pg_textsearch--1.2.0--1.3.0.sql` against the main\n  SQL diff. Covers 11 new CREATE FUNCTIONs (memtable + cache test\n  scaffolds), DROP of `bm25_memory_usage()`, and the two ALTER\n  FUNCTION ... PARALLEL UNSAFE changes (`bm25_text_bm25query_score`,\n  `bm25_textarray_bm25query_score`).\n- [x] Ran `./scripts/bump-version.sh 1.3.0-dev 1.3.0`.\n- [x] Replaced banner image (`images/tapir_and_friends_v1.3.0.png`).\n- [x] `1.2.0` is already present in the upgrade-tests matrix (added\n  during the dev-bump in #373).",
          "timestamp": "2026-05-29T20:35:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f50e9831edffd97f4f1977448fe708c4575f7ebb"
        },
        "date": 1780215299120,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2382.63865,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4274.201022,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.468,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6468.523965,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.618,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7857.938886,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.018,
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
          "id": "f50e9831edffd97f4f1977448fe708c4575f7ebb",
          "message": "Release v1.3.0 (#399)\n\nRelease v1.3.0.\n\n## Headlines\n\n- **On-disk memtable** (#374, #375, #385, #389): the L0 memtable now\n  lives in the index relation itself as a chain of WAL-logged pages\n  via `GenericXLog`, replacing the shared-memory structure. Removes\n  the old soft-limit machinery and a long tail of physical-replication\n  edge cases. New metapage version (v7), read-compatible with v6 via\n  lazy upgrade. See [`docs/memtable_v2.md`](docs/memtable_v2.md).\n- **In-memory memtable cache** (#391, #392, #395): query reads can be\n  served from a per-backend cache built over the on-disk chain, with\n  a 3-tier memory cap (per-index / global soft / global hard).\n  Controlled by `pg_textsearch.memtable_cache_enabled` and\n  `pg_textsearch.memory_limit`. See\n  [`docs/memtable_cache.md`](docs/memtable_cache.md).\n- **Multi-backend reindex regression coverage** (#386, #390, #396):\n  fixes a stale-CTIDs class of bug that surfaced under ALTER TABLE\n  heap rewrites concurrent with memtable activity.\n- **CI hardening** (#372, #394).\n\n## Release checklist (from RELEASING.md)\n\n- [x] Audit `sql/pg_textsearch--1.2.0--1.3.0.sql` against the main\n  SQL diff. Covers 11 new CREATE FUNCTIONs (memtable + cache test\n  scaffolds), DROP of `bm25_memory_usage()`, and the two ALTER\n  FUNCTION ... PARALLEL UNSAFE changes (`bm25_text_bm25query_score`,\n  `bm25_textarray_bm25query_score`).\n- [x] Ran `./scripts/bump-version.sh 1.3.0-dev 1.3.0`.\n- [x] Replaced banner image (`images/tapir_and_friends_v1.3.0.png`).\n- [x] `1.2.0` is already present in the upgrade-tests matrix (added\n  during the dev-bump in #373).",
          "timestamp": "2026-05-29T20:35:07Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f50e9831edffd97f4f1977448fe708c4575f7ebb"
        },
        "date": 1780304808394,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2365.09116,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.423,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4343.923338,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6513.629538,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.614,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8092.664232,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.989,
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
          "id": "2f882353d7e85e1fdce905514342c2aea00a9de3",
          "message": "Make PG_CONFIG overridable via the environment (#402)\n\n## Summary\n\nChange `PG_CONFIG = pg_config` to `PG_CONFIG ?= pg_config` in the\nMakefile so the target `pg_config` can be selected via an **environment\nvariable**, in addition to the existing command-line override (`make\nPG_CONFIG=...`) and PATH-based selection.\n\n## Motivation\n\nWith a plain `=` assignment, make precedence means an environment\n`PG_CONFIG` is **ignored** (only a command-line override or PATH\nselection takes effect). Using `?=` (conditional assignment) lets the\nenvironment variable work too, which is convenient when building against\na specific PostgreSQL major version without prepending its bin directory\nto PATH:\n\n```sh\nPG_CONFIG=/usr/lib/postgresql/18/bin/pg_config make\n```\n\n## Behavior\n\n- Default unchanged: when `PG_CONFIG` is set neither in the environment\nnor on the command line, it still resolves to `pg_config`.\n- Command-line override (`make PG_CONFIG=...`) continues to win, as\nbefore.\n- Environment variable is now also honored.\n\nVerified all three selection paths resolve as expected and a clean build\nsucceeds.",
          "timestamp": "2026-06-02T01:22:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2f882353d7e85e1fdce905514342c2aea00a9de3"
        },
        "date": 1780390109269,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2384.305395,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.419,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4317.846706,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.463,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6520.125137,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.613,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7946.634537,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.007,
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
          "id": "2f882353d7e85e1fdce905514342c2aea00a9de3",
          "message": "Make PG_CONFIG overridable via the environment (#402)\n\n## Summary\n\nChange `PG_CONFIG = pg_config` to `PG_CONFIG ?= pg_config` in the\nMakefile so the target `pg_config` can be selected via an **environment\nvariable**, in addition to the existing command-line override (`make\nPG_CONFIG=...`) and PATH-based selection.\n\n## Motivation\n\nWith a plain `=` assignment, make precedence means an environment\n`PG_CONFIG` is **ignored** (only a command-line override or PATH\nselection takes effect). Using `?=` (conditional assignment) lets the\nenvironment variable work too, which is convenient when building against\na specific PostgreSQL major version without prepending its bin directory\nto PATH:\n\n```sh\nPG_CONFIG=/usr/lib/postgresql/18/bin/pg_config make\n```\n\n## Behavior\n\n- Default unchanged: when `PG_CONFIG` is set neither in the environment\nnor on the command line, it still resolves to `pg_config`.\n- Command-line override (`make PG_CONFIG=...`) continues to win, as\nbefore.\n- Environment variable is now also honored.\n\nVerified all three selection paths resolve as expected and a clean build\nsucceeds.",
          "timestamp": "2026-06-02T01:22:38Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/2f882353d7e85e1fdce905514342c2aea00a9de3"
        },
        "date": 1780476780661,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2362.600245,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.423,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4256.729699,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.47,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6474.196145,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.618,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8026.899597,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.997,
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
          "id": "dcbb8063564374c9fdfec73445086694cb2b2703",
          "message": "Fix invalid segment header in standalone scoring during concurrent spill/merge (#404) (#405)\n\nFixes #404. The standalone `<@>` scoring path (used on a seqscan) opened\nsegment pages without the per-index lock, so a concurrent spill/merge\ncould free and recycle those blocks under the reader — surfacing as\n`invalid segment header ... magic=0x5450544D, expected 0x54505347`. It\nnow takes the lock `LW_SHARED` and re-reads the `level_heads` snapshot\nunder it before the segment reads (the unlocked snapshot can already\nname a recycled block), lazily on IDF cache-miss so cache-hit rows stay\nlock-free. The acquire is ownership-aware to coexist with the memtable\nchain source. The same unlocked segment walk in\n`bm25_summarize_index`/`bm25_dump_index` is fixed too.\n\nAdds `test/scripts/partial_concurrent_read.sh` (run by\n`test-concurrency`), which forces the standalone path under concurrent\nspill/merge; it fails before the fix and passes after.",
          "timestamp": "2026-06-04T04:24:43Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/dcbb8063564374c9fdfec73445086694cb2b2703"
        },
        "date": 1780562460365,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2442.790128,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.409,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4468.528472,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.448,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6768.654748,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.591,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8221.880832,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.973,
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
          "id": "dcbb8063564374c9fdfec73445086694cb2b2703",
          "message": "Fix invalid segment header in standalone scoring during concurrent spill/merge (#404) (#405)\n\nFixes #404. The standalone `<@>` scoring path (used on a seqscan) opened\nsegment pages without the per-index lock, so a concurrent spill/merge\ncould free and recycle those blocks under the reader — surfacing as\n`invalid segment header ... magic=0x5450544D, expected 0x54505347`. It\nnow takes the lock `LW_SHARED` and re-reads the `level_heads` snapshot\nunder it before the segment reads (the unlocked snapshot can already\nname a recycled block), lazily on IDF cache-miss so cache-hit rows stay\nlock-free. The acquire is ownership-aware to coexist with the memtable\nchain source. The same unlocked segment walk in\n`bm25_summarize_index`/`bm25_dump_index` is fixed too.\n\nAdds `test/scripts/partial_concurrent_read.sh` (run by\n`test-concurrency`), which forces the standalone path under concurrent\nspill/merge; it fails before the fix and passes after.",
          "timestamp": "2026-06-04T04:24:43Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/dcbb8063564374c9fdfec73445086694cb2b2703"
        },
        "date": 1780648692347,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2426.488779,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.412,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4334.205145,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.461,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6633.194152,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.603,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8290.597309,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.965,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1780733259989,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2390.331126,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.418,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4311.65119,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.464,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6437.571951,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.621,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7729.794581,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.035,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1780820636255,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2425.192629,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.412,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4457.462054,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.449,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6612.461362,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.605,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8100.160972,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.988,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1780908476115,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2363.660037,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.423,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4231.791081,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.473,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6423.999965,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.623,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7926.743342,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.009,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1780993563746,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2314.491697,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.432,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4217.2789,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.474,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6449.706029,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.62,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7843.738703,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.02,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1781080759075,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2449.20534,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.408,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4470.174069,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.447,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6710.042768,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.596,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8294.45616,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.964,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1781167629972,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2403.04373,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.416,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4348.231521,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.46,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6591.922235,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.607,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8187.494468,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.977,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1781253851194,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2387.598117,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.419,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4273.38931,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.468,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6674.642723,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.599,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8249.782997,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.97,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1781338989535,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2379.285122,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.42,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4312.254515,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.464,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6622.468099,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.604,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8096.279654,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.988,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1781426286102,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2371.253709,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.422,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4422.826552,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.452,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6586.239703,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.607,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8213.240583,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.974,
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
          "id": "876497f424c4472cbe440212e64ecb982cb22feb",
          "message": "Restrict debug_panic_after_spill_finalize to superusers (#407)\n\n`pg_textsearch.debug_panic_after_spill_finalize` was `PGC_USERSET` but\nforces an `elog(PANIC)` in the spill path, which crashes the whole\ncluster into recovery. Any role able to trigger a spill (e.g. a bulk\nINSERT past the threshold) could use it as a repeatable full-cluster\nDoS, so this makes it `PGC_SUSET` like the other debug GUCs. The\ncrash-safety shell test connects as the bootstrap superuser, so it's\nunaffected; verified manually that a non-superuser `SET` now gets\npermission denied.",
          "timestamp": "2026-06-05T18:15:59Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/876497f424c4472cbe440212e64ecb982cb22feb"
        },
        "date": 1781515844546,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2663.491882,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.375,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4906.328427,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.408,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 7311.746187,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.547,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8896.57088,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.899,
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
          "id": "07e2bfd7aae8474c89d4c218afaea3f748dd5544",
          "message": "Fix VACUUM-vs-merge race: read segment metapage under the per-index lock (#411) (#412)\n\n## Summary\n\nFixes #411 — a TOCTOU race between `VACUUM`/autovacuum and concurrent\nsegment spill/merge that produced `invalid segment header` errors and,\nin the worst case, an out-of-bounds write in the alive bitset (`TRAP:\nfailed Assert(\"doc_id < bitset->num_docs\")` under cassert, or a SIGSEGV\nin release builds).\n\nThe race affects **both** VACUUM paths in `src/access/vacuum.c`. Both\nread the metapage `level_heads` snapshot and then open segments **by\nblock number**, while a concurrent spill/merge/compaction (`tp_do_spill`\n→ `tp_maybe_compact_level`, or `bm25_force_merge`, all under\n`LW_EXCLUSIVE`) frees and recycles those blocks via the FSM:\n\n- **`tp_bulkdelete`** (`ambulkdelete`, `CONTEXT: while vacuuming index`)\nheld **no** per-index lock across Phase 2\n(`tp_vacuum_identify_affected`) → Phase 3 (`tp_vacuum_mark_dead`). A\nsegment shrunk by a merge between the phases makes a\npreviously-collected `doc_id` exceed the reloaded bitset's `num_docs`,\ndriving the out-of-bounds write in `tp_alive_bitset_mark_dead`.\n- **`tp_vacuumcleanup`** (`amvacuumcleanup`, `CONTEXT: while cleaning up\nindex`) *did* take the per-index lock `LW_SHARED`, but **acquired it\nafter reading the metapage**, so the snapshot walked by\n`tp_count_live_docs` was already stale. Under autovacuum this is the\ndominant manifestation of `invalid segment header`.\n\n## Changes\n\n- **`tp_bulkdelete`**: acquire the per-index lock `LW_SHARED` after\nPhase 1's spill (which itself takes `LW_EXCLUSIVE`, so this avoids a\nshared→exclusive upgrade) and hold it across identify + mark/replace;\nrelease on every exit path.\n- **`tp_vacuumcleanup`**: acquire `LW_SHARED` **before** reading the\nmetapage so the `level_heads` snapshot is taken under the lock.\n- **`tp_alive_bitset_mark_dead`**: skip `doc_id >= num_docs` instead of\nwriting out of bounds — defense in depth for non-assert builds (the\nassert is preserved as a canary).\n\nInserts and scans already hold this lock `LW_SHARED`, so they are\nunaffected; only the exclusive segment-recyclers wait. This matches the\nexisting convention already used by `tp_vacuumcleanup`'s reclaim walk\nand the #404 read-path fix.\n\n## Test\n\nAdds `test/scripts/vacuum_concurrent_merge.sh` (wired into `make\ntest-concurrency`): concurrent writers (spilling many small segments) +\n`bm25_force_merge` loop + monotonic deleter + `VACUUM` loop on one\nshared `bm25` index, with aggressive autovacuum. It checks the server\nand client logs for `invalid segment header` and crash signatures. A\nserial `installcheck` cannot catch this concurrency bug, so a dedicated\nstress script is needed.\n\n## Verification (stock PostgreSQL 17.9, `--enable-cassert`)\n\nA/B with a concurrent harness (6 writers + force-merge loop + monotonic\ndeleter + 2 readers, aggressive autovacuum):\n\n| build | duration | `invalid segment header` (VACUUM ctx) | crash |\n|---|---|---|---|\n| unmodified `main` | 4 min | **18** | — |\n| `tp_bulkdelete` fix only | 10 min | 24 (all cleanup ctx) | — |\n| **both paths fixed (this PR)** | 10 min | **0** | none |\n\n- `vacuum_concurrent_merge.sh`: **fails within ~30 s on `main`**,\n**passes** with this change.\n- `make installcheck`: all 70 tests pass.\n- `make format-check`: clean on the changed files.",
          "timestamp": "2026-06-16T01:51:32Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/07e2bfd7aae8474c89d4c218afaea3f748dd5544"
        },
        "date": 1781601009033,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2370.216994,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.422,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4315.13154,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.463,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6474.899956,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.618,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 7995.180909,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 1.001,
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
          "id": "f69d22d7aebd026c416e09338c26d2018f78095b",
          "message": "Standby-safe deferred reclaim of displaced segment pages (#380) (#406)\n\nCloses #380.\n\n## The problem\n\nA segment merge used to free the displaced source pages straight to the\nFSM, which is fine on the primary but can let a hot standby's in-flight\nquery read a page that's already been reused — we have no custom rmgr to\nresolve recovery conflicts, so there's nothing to stop it.\n\n## The fix\n\nA merge now parks the displaced pages in a WAL-logged tombstone chain\noff the metapage (new v8 metapage field `pending_free_head`, each batch\nstamped with the merge's `FullTransactionId`). Parked pages only go back\nto the FSM once a later VACUUM or merge sees the stamp precede\n`GetOldestNonRemovableTransactionId`. This leans on\n`hot_standby_feedback=on` to hold the primary's horizon back while a\nstandby is still reading.\n\nTwo things worth a reviewer's attention:\n\n- The displaced pages flip from the level chain to the tombstone chain\nin the *same* `GenericXLog` record as the level swap, so a crash can\nnever leave them owned by neither list (lost) or both (double-free).\n- The drain unlinks the tombstone in WAL *before* calling\n`RecordFreeIndexPage`, so a crash mid-drain only leaks, never\ndouble-frees.\n\nTruncation's high-water mark now also folds in the tombstone chain so\nforce-merge can't shrink a parked page away. Everything stays\n`GenericXLog`-only — `wal_audit` confirms no rmgr.\n\n## On-disk format & upgrade\n\nThe metapage bumps to **v8** (adds `pending_free_head`). Existing\nindexes are read-compatible and upgrade lazily, with no REINDEX: a v6\n(≤1.2.x) or v7 (1.3.x) page is normalized on read and rewritten to v8 in\nplace on the first write (atomically, with `pd_lower` bumped so\n`GenericXLog` replays the new field). v5 and earlier still require\nREINDEX as before.\n\n## Observability\n\nNew superuser-only function `bm25_pending_free_pages(index_name)`\nreturns the number of displaced blocks currently parked (REVOKE'd from\nPUBLIC, `STABLE`, walks the tombstone chain under the per-index\n`LW_SHARED` lock).\n\n## Testing & CI\n\n- **`segment_reclaim`** SQL regression test (runs every PR):\npark-after-merge, drain-after-VACUUM, queries still correct.\n- **`standby_reclaim.sh`** crash-recovery test: parks pages, crashes\n(`-m immediate`), asserts stock recovery reconstructs the tombstone\nchain + `pending_free_head`, then VACUUM drains to zero.\n- **`replication_segment_reclaim.sh`** standby acceptance test: a live\nstandby snapshot with `hot_standby_feedback=on` holds the horizon so a\nmerge+VACUUM keeps pages parked; the standby reads cleanly across merge\nreplay; pages drain once the snapshot is released.\n- Both shell tests are wired into the gating CI jobs (`test:` on PG17/18\nand the PG17.2/PG18.1 `sanitizer:` jobs, where a use-after-recycle would\ntrip ASan) plus the non-gating coverage job. The `upgrade-tests` matrix\nnow includes 1.3.0 so the **v7→v8** lazy upgrade is exercised\nend-to-end.",
          "timestamp": "2026-06-17T00:52:21Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/f69d22d7aebd026c416e09338c26d2018f78095b"
        },
        "date": 1781686675903,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "pg_textsearch INSERT TPS (c=1)",
            "value": 2352.990776,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=1)",
            "value": 0.425,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=2)",
            "value": 4339.176815,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=2)",
            "value": 0.461,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=4)",
            "value": 6597.966702,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=4)",
            "value": 0.606,
            "unit": "ms"
          },
          {
            "name": "pg_textsearch INSERT TPS (c=8)",
            "value": 8097.075977,
            "unit": "tps"
          },
          {
            "name": "pg_textsearch INSERT latency (c=8)",
            "value": 0.988,
            "unit": "ms"
          }
        ]
      }
    ]
  }
}