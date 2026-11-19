import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "summarize_tin.py"
PYTHON = Path(sys.executable)


class TinSummaryTests(unittest.TestCase):
    def setUp(self):
        test_dir = Path(__file__).resolve().parent
        self.temporary = tempfile.TemporaryDirectory(dir=test_dir)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "tin"
        self.root.mkdir()
        (self.root / "plan.json").write_text(json.dumps({
            "dataset": "synthetic-smoke",
            "measurement_duration": "0.04s",
        }))
        self.engine = "pg_textsearch"
        self.cell_id = (
            "constrained16.english-no-stopwords.natural."
            "concurrent-writes.interleaved-all.shape-filter_first"
        )
        self.cell = self.root / "results" / self.engine / self.cell_id
        self.cell.mkdir(parents=True)
        scenario = self.root / "scenarios" / self.engine
        scenario.mkdir(parents=True)
        (scenario / f"{self.cell_id}.json").write_text(json.dumps({
            "kinds": ["disjunction", "conjunction", "phrase"],
        }))

    def write_json(self, name, value, cell=None):
        ((cell or self.cell) / name).write_text(json.dumps(value))

    def fingerprint(self, operation="concurrent-writes"):
        return {
            "cell": {
                "cell_id": self.cell_id,
                "profile": "constrained16",
                "analyzer": "english-no-stopwords",
                "analyzer_role": "primary",
                "pack": "natural",
                "operation": operation,
                "query_family": "interleaved-all",
                "sql_shape": "filter_first",
                "vus": 8,
                "limit": 10,
                "update_rate": 50 if operation == "concurrent-writes" else 0,
            },
            "engine": self.engine,
        }

    def completed(self):
        self.write_json("complete.json", self.fingerprint())
        self.write_json("dashboard.json", {
            "runs": {
                "database": {
                    "startTime": 1000,
                    "endTime": 1020,
                    "queries": {
                        "queries": {
                            "latencies": [30, 10, 20],
                            "timestamps": [1040, 1010, 1030],
                        },
                        "updates": {
                            "latencies": [1000, 2000],
                            "timestamps": [1020, 1040],
                        },
                    },
                },
                "collector": {
                    "startTime": 1000,
                    "endTime": 9000,
                    "queries": {
                        "metrics": {
                            "latencies": [999] * 100,
                            "timestamps": [9000] * 100,
                        },
                    },
                },
            },
        })
        self.metrics = {
            "benchmark_errors": {"passes": 0, "fails": 5, "value": 0},
            "benchmark_timeouts": {"passes": 0, "fails": 5, "value": 0},
            "update_failures": {"passes": 0, "fails": 2, "value": 0},
            "achieved_updates": {"count": 2},
        }
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        self.write_json("load-quality.json", {
            "target_rate": 50,
            "duration": "0.04s",
            "target_updates": 2,
            "actual_writes": 2,
            "attempted_writes": 2,
            "failed_writes": 0,
            "dropped_arrivals": 0,
            "target_met": True,
            "unequal_pressure": False,
            "engine": self.engine,
            "cell_id": self.cell_id,
            "sql_status": "ok",
        })
        rows = [
            ("q1:disjunction", 1, 10),
            ("q2:conjunction", 1, 20),
            ("q3:phrase", 1, 30),
        ]
        with (self.cell / "queries.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(
                stream, fieldnames=("query_id", "count", "mean_ms")
            )
            writer.writeheader()
            for query_id, count, mean in rows:
                writer.writerow({
                    "query_id": query_id,
                    "count": count,
                    "mean_ms": mean,
                })

    def run_summary(self, *args):
        return subprocess.run(
            [str(PYTHON), str(SCRIPT), str(self.root), *args],
            text=True,
            capture_output=True,
        )

    def rows(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        return list(csv.DictReader(io.StringIO(result.stdout)))

    def test_real_schema_reconciles_stale_clock_and_excludes_non_search(self):
        self.completed()
        row, = self.rows(self.run_summary())
        self.assertEqual(row["dataset"], "synthetic-smoke")
        self.assertEqual(row["operation"], "concurrent-writes")
        self.assertEqual(row["query_family"], "interleaved-all")
        self.assertEqual(row["sql_shape"], "filter_first")
        self.assertEqual(int(row["search_count"]), 3)
        self.assertEqual(float(row["search_qps"]), 75)
        self.assertEqual(float(row["mean_ms"]), 20)
        self.assertEqual(int(row["actual_writes"]), 2)
        self.assertEqual(int(row["attempted_writes"]), 2)
        self.assertEqual(float(row["achieved_write_rate"]), 50)
        self.assertEqual(
            row["write_rate_basis"],
            "successful_writes_per_dashboard_elapsed",
        )
        self.assertEqual(
            json.loads(row["per_kind_counts"]),
            {"conjunction": 1, "disjunction": 1, "phrase": 1},
        )
        self.assertEqual(
            row["per_kind_latency_status"],
            "unavailable_aggregated_query_csv",
        )

    def test_failed_and_plan_gated_cells_never_publish_timings(self):
        failed = self.fingerprint()
        self.write_json("failed.json", {
            "status": "failed",
            "exit_code": 99,
            "fingerprint": failed,
        })
        gated_id = self.cell_id.replace(
            "concurrent-writes", "count"
        )
        gated = self.root / "results" / self.engine / gated_id
        gated.mkdir()
        gated_fingerprint = self.fingerprint("count")
        gated_fingerprint["cell"]["cell_id"] = gated_id
        self.write_json("plan-gate-failed.json", {
            "kind": "non_indexed_count_plan",
            "fingerprint": gated_fingerprint,
        }, gated)

        rows = self.rows(self.run_summary())
        self.assertEqual(
            {row["status"] for row in rows}, {"failed", "plan_gated"}
        )
        failed_row = next(row for row in rows if row["status"] == "failed")
        self.assertEqual(failed_row["failure_phase"], "warmup")
        self.assertEqual(failed_row["search_qps"], "")
        gated_row = next(
            row for row in rows if row["status"] == "plan_gated"
        )
        self.assertEqual(gated_row["search_qps"], "")
        self.assertEqual(gated_row["p95_ms"], "")

    def test_errors_and_timeouts_suppress_competitive_timings(self):
        self.completed()
        self.metrics["benchmark_errors"].update(
            passes=1, fails=4, value=0.2
        )
        self.metrics["benchmark_timeouts"].update(
            passes=1, fails=4, value=0.2
        )
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        row, = self.rows(self.run_summary())
        self.assertEqual(row["status"], "failed")
        self.assertEqual(int(row["error_count"]), 1)
        self.assertEqual(int(row["timeout_count"]), 1)
        self.assertEqual(row["search_qps"], "")
        self.assertEqual(row["p95_ms"], "")

    def test_counter_mismatch_is_rejected(self):
        self.completed()
        self.metrics["benchmark_errors"]["fails"] = 6
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        result = self.run_summary()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sample count", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_load_quality_must_match_raw_counters(self):
        self.completed()
        quality = json.loads(
            (self.cell / "load-quality.json").read_text()
        )
        quality["actual_writes"] = 3
        self.write_json("load-quality.json", quality)
        result = self.run_summary()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("load quality", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_degraded_load_preserves_valid_read_timings(self):
        self.completed()
        self.metrics["dropped_iterations"] = {"count": 1}
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        quality = json.loads(
            (self.cell / "load-quality.json").read_text()
        )
        quality.update(
            dropped_arrivals=1,
            target_met=False,
            unequal_pressure=True,
        )
        self.write_json("load-quality.json", quality)

        row, = self.rows(self.run_summary())
        self.assertEqual(row["status"], "ok")
        self.assertEqual(row["load_quality"], "degraded")
        self.assertEqual(row["target_met"], "False")
        self.assertEqual(float(row["search_qps"]), 75)
        self.assertEqual(float(row["p95_ms"]), 29)

    def test_measured_failure_preserves_validated_write_counts(self):
        self.completed()
        fingerprint = json.loads(
            (self.cell / "complete.json").read_text()
        )
        (self.cell / "complete.json").unlink()
        self.write_json("failed.json", {
            "status": "failed",
            "exit_code": 99,
            "fingerprint": fingerprint,
        })
        (self.cell / "k6.log").touch()
        self.metrics["benchmark_errors"].update(
            passes=1, fails=4, value=0.2
        )
        self.metrics["update_failures"].update(passes=1, fails=1)
        self.metrics["achieved_updates"]["count"] = 1
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        dashboard = json.loads(
            (self.cell / "dashboard.json").read_text()
        )
        dashboard["runs"]["database"]["queries"]["updates"] = {
            "latencies": [1000],
            "timestamps": [1040],
        }
        self.write_json("dashboard.json", dashboard)
        quality = json.loads(
            (self.cell / "load-quality.json").read_text()
        )
        quality.update(
            actual_writes=1,
            failed_writes=1,
            target_met=False,
            unequal_pressure=True,
            sql_status="failed",
        )
        self.write_json("load-quality.json", quality)

        row, = self.rows(self.run_summary())
        self.assertEqual(row["status"], "failed")
        self.assertEqual(row["failure_phase"], "measured")
        self.assertEqual(row["search_qps"], "")
        self.assertEqual(int(row["actual_writes"]), 1)
        self.assertEqual(int(row["attempted_writes"]), 2)
        self.assertEqual(int(row["failed_writes"]), 1)
        self.assertEqual(float(row["achieved_write_rate"]), 25)
        self.assertEqual(
            row["write_rate_basis"],
            "successful_writes_per_dashboard_elapsed",
        )

    def test_all_failed_writes_allow_missing_counter_and_latencies(self):
        self.completed()
        fingerprint = json.loads(
            (self.cell / "complete.json").read_text()
        )
        (self.cell / "complete.json").unlink()
        self.write_json("failed.json", {
            "status": "failed",
            "exit_code": 99,
            "fingerprint": fingerprint,
        })
        (self.cell / "k6.log").touch()
        self.metrics["benchmark_errors"].update(
            passes=2, fails=3, value=0.4
        )
        self.metrics["benchmark_timeouts"].update(
            passes=2, fails=3, value=0.4
        )
        self.metrics["update_failures"].update(passes=2, fails=0)
        del self.metrics["achieved_updates"]
        self.write_json("k6-summary.json", {"metrics": self.metrics})
        dashboard = json.loads(
            (self.cell / "dashboard.json").read_text()
        )
        del dashboard["runs"]["database"]["queries"]["updates"]
        self.write_json("dashboard.json", dashboard)
        quality = json.loads(
            (self.cell / "load-quality.json").read_text()
        )
        quality.update(
            actual_writes=0,
            failed_writes=2,
            target_met=False,
            unequal_pressure=True,
            sql_status="failed",
        )
        self.write_json("load-quality.json", quality)

        row, = self.rows(self.run_summary())
        self.assertEqual(row["status"], "failed")
        self.assertEqual(int(row["actual_writes"]), 0)
        self.assertEqual(int(row["attempted_writes"]), 2)
        self.assertEqual(int(row["failed_writes"]), 2)
        self.assertEqual(row["write_seconds"], "")
        self.assertEqual(row["achieved_write_rate"], "")
        self.assertEqual(
            row["write_rate_basis"],
            "unavailable_no_success_latency_samples",
        )

    def test_target_updates_come_from_campaign_duration(self):
        self.completed()
        quality = json.loads(
            (self.cell / "load-quality.json").read_text()
        )
        quality.update(
            duration="0.02s",
            target_updates=1,
            target_met=True,
            unequal_pressure=False,
        )
        self.write_json("load-quality.json", quality)
        result = self.run_summary()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("measurement duration", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_exporter_is_importable_as_a_repository_module(self):
        result = subprocess.run(
            [
                str(PYTHON), "-c",
                "from benchmarks.scale.summarize_tin import summarize",
            ],
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
