window.BENCHMARK_DATA = {
  "lastUpdate": 1776153099104,
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
      }
    ]
  }
}