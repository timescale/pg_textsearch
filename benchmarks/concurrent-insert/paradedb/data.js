window.BENCHMARK_DATA = {
  "lastUpdate": 1776152882747,
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
      }
    ]
  }
}