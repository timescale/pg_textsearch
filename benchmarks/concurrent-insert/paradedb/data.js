window.BENCHMARK_DATA = {
  "lastUpdate": 1775772979023,
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
      }
    ]
  }
}