// Test data for local comparison chart testing
// Copy this to data.js to test the comparison dashboard locally

window.BENCHMARK_DATA = {
  "lastUpdate": 1735900000000,
  "repoUrl": "https://github.com/timescale/pg_textsearch",
  "entries": {
    "msmarco Benchmarks": [
      {
        "commit": {
          "id": "abc1234567890",
          "message": "Test commit 1",
          "timestamp": "2026-01-01T12:00:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/abc1234567890"
        },
        "date": 1735689600000,
        "tool": "customSmallerIsBetter",
        "benches": [
          { "name": "msmarco (8.8M docs) - Index Build Time", "value": 250000, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Short Query (1 word)", "value": 8.5, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Medium Query (3 words)", "value": 12.3, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Long Query (question)", "value": 15.8, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Common Term Query", "value": 5.2, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Rare Term Query", "value": 6.1, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)", "value": 11.5, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Index Size", "value": 1800, "unit": "MB" }
        ]
      },
      {
        "commit": {
          "id": "def2345678901",
          "message": "Test commit 2",
          "timestamp": "2026-01-02T12:00:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/def2345678901"
        },
        "date": 1735776000000,
        "tool": "customSmallerIsBetter",
        "benches": [
          { "name": "msmarco (8.8M docs) - Index Build Time", "value": 245000, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Short Query (1 word)", "value": 8.2, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Medium Query (3 words)", "value": 11.9, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Long Query (question)", "value": 15.2, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Common Term Query", "value": 5.0, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Rare Term Query", "value": 5.9, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Avg Query Latency (20 queries)", "value": 11.0, "unit": "ms" },
          { "name": "msmarco (8.8M docs) - Index Size", "value": 1800, "unit": "MB" }
        ]
      }
    ],
    "paradedb_msmarco Benchmarks": [
      {
        "commit": {
          "id": "abc1234567890",
          "message": "Test commit 1",
          "timestamp": "2026-01-01T12:00:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/abc1234567890"
        },
        "date": 1735689600000,
        "tool": "customSmallerIsBetter",
        "benches": [
          { "name": "paradedb_msmarco (8.8M docs) - Index Build Time", "value": 188890, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Short Query (1 word)", "value": 10.5, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Medium Query (3 words)", "value": 17.0, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Long Query (question)", "value": 20.9, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Common Term Query", "value": 9.0, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Rare Term Query", "value": 13.2, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Avg Query Latency (20 queries)", "value": 14.9, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Index Size", "value": 1410, "unit": "MB" }
        ]
      },
      {
        "commit": {
          "id": "def2345678901",
          "message": "Test commit 2",
          "timestamp": "2026-01-02T12:00:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/def2345678901"
        },
        "date": 1735776000000,
        "tool": "customSmallerIsBetter",
        "benches": [
          { "name": "paradedb_msmarco (8.8M docs) - Index Build Time", "value": 185000, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Short Query (1 word)", "value": 10.3, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Medium Query (3 words)", "value": 16.5, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Long Query (question)", "value": 20.5, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Common Term Query", "value": 8.8, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Rare Term Query", "value": 12.8, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Avg Query Latency (20 queries)", "value": 14.5, "unit": "ms" },
          { "name": "paradedb_msmarco (8.8M docs) - Index Size", "value": 1410, "unit": "MB" }
        ]
      }
    ],
    "cranfield Benchmarks": [
      {
        "commit": {
          "id": "abc1234567890",
          "message": "Test commit 1",
          "timestamp": "2026-01-01T12:00:00Z",
          "url": "https://github.com/timescale/pg_textsearch/commit/abc1234567890"
        },
        "date": 1735689600000,
        "tool": "customSmallerIsBetter",
        "benches": [
          { "name": "cranfield (1.4K docs) - Index Build Time", "value": 250, "unit": "ms" },
          { "name": "cranfield (1.4K docs) - Short Query (1 word)", "value": 3.5, "unit": "ms" }
        ]
      }
    ]
  }
};

window.BRANCH_INFO = {
  "abc1234567890": "main",
  "def2345678901": "main"
};
