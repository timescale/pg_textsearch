window.BENCHMARK_DATA = {
  "lastUpdate": 1775785353274,
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
      }
    ]
  }
}