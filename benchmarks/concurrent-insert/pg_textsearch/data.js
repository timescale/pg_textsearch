window.BENCHMARK_DATA = {
  "lastUpdate": 1775779956199,
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
      }
    ]
  }
}