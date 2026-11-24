import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "summarize.py"


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cell = self.root / "paradedb/mixed/top10/natural-8vu"
        self.cell.mkdir(parents=True)

    def write_json(self, name, value):
        (self.cell / name).write_text(json.dumps(value))

    def completed(self):
        self.write_json("complete.json", {})
        self.write_json("paradedb_test.json", {
            "runs": {
                "search": {
                    "startTime": 1000, "endTime": 1020,
                    "queries": {"search": {
                        "latencies": [30, 10, 20],
                        "timestamps": [1040, 1010, 1030],
                    }},
                },
                "updates": {
                    "startTime": 1000, "endTime": 1040,
                    "queries": {"updates": {
                        "latencies": [1000, 2000],
                        "timestamps": [1020, 1040],
                    }},
                },
            },
        })
        self.metrics = {
            "benchmark_errors": {"passes": 0, "fails": 5, "value": 0},
            "benchmark_timeouts": {"value": 0},
            "update_failures": {"passes": 0, "fails": 2, "value": 0},
            "achieved_updates": {"count": 2},
            "dropped_iterations": {"count": 1},
        }
        self.write_json("k6-summary.json", {"metrics": self.metrics})

    def run_summary(self, *args):
        if not args:
            args = ("--engine", "paradedb")
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(self.root), *args],
            text=True, capture_output=True,
        )

    def test_stale_end_time_and_writes_do_not_inflate_search_qps(self):
        self.completed()
        result = self.run_summary()
        self.assertEqual(result.returncode, 0, result.stderr)
        row, = csv.DictReader(io.StringIO(result.stdout))
        self.assertEqual(int(row["search_count"]), 3)
        self.assertEqual(float(row["search_qps"]), 75)
        self.assertEqual(float(row["mean_ms"]), 20)
        self.assertEqual(float(row["p50_ms"]), 20)
        self.assertAlmostEqual(float(row["p95_ms"]), 29)
        self.assertEqual(float(row["achieved_updates_per_second"]), 50)
        self.assertEqual(int(row["dropped_updates"]), 1)

    def test_failed_cells_do_not_publish_successful_subset_latency(self):
        self.write_json("failed.json", {"exit_code": 99})
        self.write_json("paradedb_test.json", {"not": "usable"})
        (self.cell / "k6.log").touch()
        result = self.run_summary()
        self.assertEqual(result.returncode, 0, result.stderr)
        row, = csv.DictReader(io.StringIO(result.stdout))
        self.assertEqual(row["status"], "failed")
        self.assertEqual(row["failure_phase"], "measured")
        self.assertEqual(row["search_qps"], "")
        self.assertEqual(row["p95_ms"], "")

    def test_unverified_phrase_protocol_cannot_publish_latency(self):
        destination = self.root / "paradedb/phrase/top10/natural-8vu"
        destination.parent.mkdir(parents=True)
        self.cell.rename(destination)
        self.cell = destination
        self.write_json("complete.json", {})
        self.write_json("paradedb_test.json", {"not": "usable"})
        result = self.run_summary()
        self.assertEqual(result.returncode, 0, result.stderr)
        row, = csv.DictReader(io.StringIO(result.stdout))
        self.assertEqual(row["status"], "invalid_semantics")
        self.assertEqual(
            row["exclusion_reason"], "unverified_phrase_protocol"
        )
        self.assertEqual(row["search_qps"], "")
        self.assertEqual(row["p95_ms"], "")

    def test_inconsistent_raw_and_independent_k6_counts_are_rejected(self):
        self.completed()
        self.metrics["benchmark_errors"]["fails"] = 6
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        result = self.run_summary()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sample count", result.stderr)

    def test_selecting_headline_engine_ignores_unfinished_lead_stage(self):
        self.completed()
        (self.root / "lead/disjunction/top10/spans-8vu").mkdir(parents=True)
        result = self.run_summary("--engine", "paradedb")
        self.assertEqual(result.returncode, 0, result.stderr)
        row, = csv.DictReader(io.StringIO(result.stdout))
        self.assertEqual(row["engine"], "paradedb")

    def test_requested_missing_engine_cannot_be_silently_omitted(self):
        self.completed()
        result = self.run_summary(
            "--engine", "paradedb", "--engine", "pg_textsearch",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pg_textsearch", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
