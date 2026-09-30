#!/usr/bin/env python3
"""Tests for build-memory measurement and failure reporting."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "benchmarks/run_build_memory.py"
benchmark = None
if RUNNER.exists():
    spec = importlib.util.spec_from_file_location("build_memory", RUNNER)
    benchmark = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(benchmark)


class BuildMemoryTest(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(benchmark, "build-memory runner not implemented")

    def test_process_memory_uses_pss_not_shared_rss(self):
        with tempfile.TemporaryDirectory() as directory:
            process = Path(directory) / "123"
            process.mkdir()
            (process / "smaps_rollup").write_text(
                "00400000-00500000 ---p 00000000 00:00 0 [rollup]\n"
                "Rss: 100 kB\nPss: 60 kB\n"
                "Private_Clean: 4 kB\nPrivate_Dirty: 16 kB\n"
                "Shared_Clean: 80 kB\n"
            )
            self.assertEqual(
                benchmark.read_memory(123, Path(directory)),
                {"rss": 102400, "pss": 61440, "private": 20480},
            )

    def test_exited_process_is_not_reported_as_zero_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(
                benchmark.read_memory(123, Path(directory))
            )

    def test_malformed_memory_sample_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            process = Path(directory) / "123"
            process.mkdir()
            (process / "smaps_rollup").write_text("Rss: 100 kB\n")
            with self.assertRaises(ValueError):
                benchmark.read_memory(123, Path(directory))

    def test_peak_is_maximum_concurrent_sum_not_sum_of_peaks(self):
        samples = [
            {"phase": "loading", "leader_private": 10,
             "worker_private": 90, "build_pss": 100},
            {"phase": "writing", "leader_private": 80,
             "worker_private": 5, "build_pss": 85},
            {"phase": "writing", "leader_private": 0,
             "worker_private": 0, "build_pss": None},
        ]
        summary = benchmark.summarize_samples(samples)
        self.assertEqual(summary["peak_build_pss_bytes"], 100)
        self.assertEqual(
            summary["phases"]["writing"]["peak_build_pss_bytes"], 85
        )
        self.assertEqual(summary["incomplete_samples"], 1)

    def test_empty_measurements_are_not_published_as_zero(self):
        summary = benchmark.summarize_samples([])
        self.assertIsNone(summary["peak_build_pss_bytes"])
        self.assertEqual(summary["samples"], 0)

    def test_cgroup_snapshot_preserves_limits_and_oom_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for name, value in {
                "memory.current": "4096",
                "memory.max": "max",
                "memory.stat": "anon 1024\nfile 2048\nshmem 512\n",
                "memory.events": "low 0\nhigh 0\nmax 3\noom 1\n"
                                 "oom_kill 1\noom_group_kill 0\n",
            }.items():
                (path / name).write_text(value)
            result = benchmark.read_cgroup(path)
            self.assertEqual(result["current"], 4096)
            self.assertEqual(result["max"], "max")
            self.assertEqual(result["anon"], 1024)
            self.assertEqual(result["events"]["oom_kill"], 1)

    def test_invalid_options_fail_before_creating_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results"
            for arguments in (
                ["--rows", "0"],
                ["--workers", "-1"],
                ["--interval", "nan"],
                ["--rows", "100", "--workers", "2"],
                ["--terms", "0"],
                ["--maintenance-work-mem", "64MB'; SELECT 1; --"],
            ):
                with self.subTest(arguments=arguments):
                    result = subprocess.run(
                        [sys.executable, str(RUNNER),
                         "--output", str(output), *arguments],
                        capture_output=True, text=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(output.exists())

    @unittest.skipUnless(sys.platform == "linux", "Linux runner")
    def test_missing_postgres_tools_record_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results"
            result = subprocess.run(
                [sys.executable, str(RUNNER), "--output", str(output),
                 "--pg-config", str(Path(directory) / "missing")],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["status"], "error")
            self.assertIn("missing", summary["error"])
            self.assertFalse((output / "data").exists())


@unittest.skipUnless(os.environ.get("BUILD_MEMORY_PG_CONFIG"),
                     "set BUILD_MEMORY_PG_CONFIG for cluster tests")
class ClusterIntegrationTest(unittest.TestCase):
    def test_success_records_metrics_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results"
            result = subprocess.run(
                [sys.executable, str(RUNNER), "--output", str(output),
                 "--pg-config", os.environ["BUILD_MEMORY_PG_CONFIG"],
                 "--case", "unique", "--workers", "0", "--terms", "1"],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["status"], "ok")
            case = summary["cases"][0]
            self.assertEqual(case["matching_docs"], 100000)
            self.assertEqual(case["workers_launched"], 0)
            self.assertEqual(case["text_bytes"], 1700000)
            self.assertGreater(case["peak_build_pss_bytes"], 0)
            self.assertGreater(case["build_seconds"], 0)
            self.assertTrue((output / "unique.csv").exists())
            self.assertFalse((output / "data").exists())

    def test_timeout_records_failure_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results"
            result = subprocess.run(
                [sys.executable, str(RUNNER), "--output", str(output),
                 "--pg-config", os.environ["BUILD_MEMORY_PG_CONFIG"],
                 "--case", "unique", "--timeout", "0.001"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["status"], "error")
            self.assertIn("timed out", summary["error"])
            self.assertNotIn("cleanup_error", summary)
            self.assertFalse((output / "data").exists())


if __name__ == "__main__":
    unittest.main()
