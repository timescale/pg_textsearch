import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from benchmarks.scale.fleet import (
    _is_measured_run_error,
    _write_quality,
    build_shard_summary,
    execute_shard,
    fleet_plan,
    run_pending_cells,
    scan_cell_results,
    shard_cells,
    validate_imported_inputs,
)
from benchmarks.scale.tin import (
    ANALYZERS,
    BUILD_RESOURCES,
    DATABASE_PROOF_PROTOCOL,
)
from benchmarks.scale.run import ConfigError, Runner


class FleetPlanTest(unittest.TestCase):
    def test_selected_sql_shape_reaches_every_shard_cell(self):
        with patch.dict("os.environ", {"PGTS_SQL_SHAPE": "original"}):
            plan = fleet_plan("msmarco-v2")
        self.assertTrue(all(
            cell["sql_shape"] == "original"
            and cell["cell_id"].endswith(".shape-original")
            for cell in plan["selected_cells"]
        ))

    def test_six_shards_cover_ms_matrix_and_wikipedia_controls(self):
        ms_plan = fleet_plan("msmarco-v2")
        wiki_plan = fleet_plan("wikipedia")

        ms_cells = [
            shard_cells(ms_plan, engine, profile)
            for engine in ("pg_textsearch", "paradedb")
            for profile in ("high", "constrained32", "constrained16")
        ]
        wiki_cells = [
            shard_cells(wiki_plan, engine, profile)
            for engine in ("pg_textsearch", "paradedb")
            for profile in ("high", "constrained32", "constrained16")
        ]

        self.assertEqual([len(cells) for cells in ms_cells], [28] * 6)
        self.assertEqual([len(cells) for cells in wiki_cells], [1] * 6)
        self.assertEqual(sum(map(len, ms_cells)), 168)
        self.assertEqual(sum(map(len, wiki_cells)), 6)
        self.assertEqual(
            len({cell["cell_id"] for cells in ms_cells for cell in cells}),
            84,
        )
        self.assertTrue(all(
            cell["sql_shape"] == "filter_first"
            for cell in ms_plan["selected_cells"]
        ))


class FleetResultTest(unittest.TestCase):
    def result_dir(self, root, engine, cell):
        path = root / "results" / engine / cell["cell_id"]
        path.mkdir(parents=True)
        return path

    def marker(self, path, name, engine, cell):
        fingerprint = {"engine": engine, "cell": cell}
        payload = fingerprint if name == "complete.json" else {
            "fingerprint": fingerprint,
        }
        (path / name).write_text(json.dumps(payload))
        if name == "complete.json":
            (path / "k6-summary.json").write_text('{"metrics": {}}')
            if cell["operation"] == "concurrent-writes":
                (path / "write-cleanup.json").write_text(
                    '{"removed": true}'
                )

    def execute_with_run_error(self, error, *, legitimate_summary):
        root = Path(self._testMethodName)
        self.addCleanup(
            lambda: __import__("shutil").rmtree(
                root, ignore_errors=True
            )
        )
        tin_dir = root / "tin"
        plan = fleet_plan("msmarco-v2")
        cells = shard_cells(plan, "pg_textsearch", "high")
        first = self.result_dir(
            tin_dir, "pg_textsearch", cells[0]
        )
        self.marker(
            first, "failed.json", "pg_textsearch", cells[0]
        )
        runner = SimpleNamespace(
            run_id="tin-msmarco-20261119-final-v1",
            tin_dir=tin_dir,
            dataset_kind="msmarco-v2",
            dataset=root / "dataset",
            root=root,
        )

        runner._write_or_validate_json = Runner._write_or_validate_json

        def run_cells(_engines, pending):
            failed = pending[0] if legitimate_summary else None
            for cell in pending:
                result = (
                    tin_dir / "results" / "pg_textsearch"
                    / cell["cell_id"]
                )
                result.mkdir(parents=True, exist_ok=True)
                marker = (
                    "failed.json"
                    if cell is failed
                    else "complete.json"
                )
                self.marker(
                    result, marker, "pg_textsearch", cell
                )
            if legitimate_summary:
                (tin_dir / "summary.json").write_text(json.dumps({
                    "status": "failed",
                    "failures": [{
                        "engine": "pg_textsearch",
                        "cell_id": failed["cell_id"],
                    }],
                    "selected_cells": len(pending),
                    "engines": ["pg_textsearch"],
                }))
            raise error

        runner.run_cells = run_cells
        with (
            patch(
                "benchmarks.scale.fleet.validate_imported_inputs",
                return_value={"images": "pinned"},
            ),
            patch(
                "benchmarks.scale.fleet._source_manifest",
                return_value={"host": "worker-1"},
            ),
            patch(
                "benchmarks.scale.fleet._write_quality",
                return_value=[],
            ),
        ):
            return execute_shard(
                runner,
                engine="pg_textsearch",
                profile="high",
                dataset="msmarco-v2",
                host="worker-1",
                attempt_id="classification-1",
            )

    def test_finished_complete_and_failed_cells_are_never_rerun(self):
        cells = fleet_plan("wikipedia")["selected_cells"][:2]
        with tempfile.TemporaryDirectory() as directory:
            tin_dir = Path(directory)
            complete = self.result_dir(
                tin_dir, "pg_textsearch", cells[0]
            )
            failed = self.result_dir(
                tin_dir, "pg_textsearch", cells[1]
            )
            self.marker(
                complete, "complete.json", "pg_textsearch", cells[0]
            )
            self.marker(
                failed, "failed.json", "pg_textsearch", cells[1]
            )
            statuses = scan_cell_results(
                tin_dir, "pg_textsearch", cells
            )
            runner = SimpleNamespace(run_cells=Mock())

            run_pending_cells(runner, "pg_textsearch", cells, statuses)

            runner.run_cells.assert_not_called()
            self.assertEqual(
                [status["status"] for status in statuses],
                ["completed", "failed"],
            )

    def test_fleet_retries_only_unfinished_completed_write_cleanup(self):
        for cleanup_succeeds in (True, False):
            with (
                self.subTest(cleanup_succeeds=cleanup_succeeds),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                tin_dir = root / "tin"
                plan = fleet_plan("msmarco-v2")
                cells = shard_cells(plan, "pg_textsearch", "high")
                write_cell = next(
                    cell for cell in cells
                    if cell["operation"] == "concurrent-writes"
                )
                for cell in cells:
                    result = self.result_dir(
                        tin_dir, "pg_textsearch", cell
                    )
                    self.marker(
                        result, "complete.json", "pg_textsearch", cell
                    )
                result = (
                    tin_dir / "results/pg_textsearch"
                    / write_cell["cell_id"]
                )
                cleanup = result / "write-cleanup.json"
                cleanup.unlink()
                resumed = []

                def run_cells(_engines, selected):
                    resumed.extend(selected)
                    if not cleanup_succeeds:
                        raise ConfigError("cleanup still unavailable")
                    cleanup.write_text('{"removed": true}')

                runner = SimpleNamespace(
                    run_id="cleanup-retry",
                    tin_dir=tin_dir,
                    dataset_kind="msmarco-v2",
                    dataset=root / "dataset",
                    root=root,
                    run_cells=run_cells,
                    _write_or_validate_json=(
                        Runner._write_or_validate_json
                    ),
                )
                with (
                    patch(
                        "benchmarks.scale.fleet.validate_imported_inputs",
                        return_value={"images": "pinned"},
                    ),
                    patch(
                        "benchmarks.scale.fleet._source_manifest",
                        return_value={"host": "worker-1"},
                    ),
                    patch(
                        "benchmarks.scale.fleet._write_quality",
                        return_value=[],
                    ),
                ):
                    summary, exit_code = execute_shard(
                        runner,
                        engine="pg_textsearch",
                        profile="high",
                        dataset="msmarco-v2",
                        host="worker-1",
                        attempt_id="cleanup-1",
                    )
                self.assertEqual(resumed, [write_cell])
                self.assertEqual(
                    exit_code, 0 if cleanup_succeeds else 3
                )
                self.assertEqual(
                    summary["status"],
                    "ok" if cleanup_succeeds else "infrastructure_error",
                )
                self.assertEqual(cleanup.is_file(), cleanup_succeeds)

    def test_invalid_or_incomplete_terminal_artifacts_are_rejected(self):
        cell = fleet_plan("wikipedia")["selected_cells"][0]
        with tempfile.TemporaryDirectory() as directory:
            tin_dir = Path(directory)
            result = self.result_dir(tin_dir, "paradedb", cell)
            self.marker(result, "complete.json", "paradedb", cell)
            (result / "failed.json").write_text(json.dumps({
                "fingerprint": {"engine": "paradedb", "cell": cell},
            }))

            with self.assertRaisesRegex(ConfigError, "exactly one"):
                scan_cell_results(tin_dir, "paradedb", [cell])

            (result / "failed.json").unlink()
            (result / "k6-summary.json").unlink()
            with self.assertRaisesRegex(ConfigError, "k6-summary"):
                scan_cell_results(tin_dir, "paradedb", [cell])

            (result / "complete.json").unlink()
            (result / "plan-gate-failed.json").write_text(json.dumps({
                "fingerprint": {"engine": "paradedb", "cell": cell},
                "probe": "probes/summary.json",
            }))
            with self.assertRaisesRegex(ConfigError, "referenced artifact"):
                scan_cell_results(tin_dir, "paradedb", [cell])

    def test_partial_results_are_archived_before_retry(self):
        cell = fleet_plan("wikipedia")["selected_cells"][0]
        with tempfile.TemporaryDirectory() as directory:
            tin_dir = Path(directory)
            result = self.result_dir(tin_dir, "paradedb", cell)
            (result / "raw.json").write_text('{"partial": true}')

            statuses = scan_cell_results(
                tin_dir,
                "paradedb",
                [cell],
                archive_partials=True,
                attempt_id="attempt-7",
            )

            archived = (
                tin_dir / "fleet/forensics" / "paradedb"
                / cell["cell_id"] / "attempt-7/raw.json"
            )
            self.assertTrue(archived.is_file())
            self.assertTrue(result.is_dir())
            self.assertEqual(list(result.iterdir()), [])
            self.assertEqual(statuses[0]["status"], "pending")
            self.assertEqual(
                statuses[0]["archived_partial"],
                str(archived.parent),
            )

    def test_summary_reports_preserved_failures_and_full_coverage(self):
        cells = fleet_plan("wikipedia")["selected_cells"]
        statuses = [
            {
                "cell_id": cell["cell_id"],
                "status": "failed" if index == 0 else "completed",
                "marker": "failed.json" if index == 0 else "complete.json",
            }
            for index, cell in enumerate(cells)
        ]

        summary, exit_code = build_shard_summary(
            engine="pg_textsearch",
            profile="high",
            dataset="wikipedia",
            run_id="tin-wikipedia-20261119-final-v1",
            cells=cells,
            statuses=statuses,
            write_quality=[],
            host="worker-1",
        )

        self.assertEqual(exit_code, 2)
        self.assertEqual(summary["status"], "measured_failures")
        self.assertEqual(summary["terminal_counts"]["failed"], 1)
        self.assertEqual(summary["expected_cells"], len(cells))
        self.assertEqual(summary["terminal_cells"], len(cells))

    def test_execute_shard_preserves_all_existing_terminal_cells(self):
        plan = fleet_plan("msmarco-v2")
        cells = shard_cells(plan, "pg_textsearch", "high")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tin_dir = root / "tin"
            for index, cell in enumerate(cells):
                result = self.result_dir(
                    tin_dir, "pg_textsearch", cell
                )
                marker = (
                    "failed.json" if index == 0 else "complete.json"
                )
                self.marker(
                    result, marker, "pg_textsearch", cell
                )
            runner = SimpleNamespace(
                run_id="fleet-retry_2027",
                tin_dir=tin_dir,
                dataset_kind="msmarco-v2",
                dataset=root / "dataset",
                root=root,
                run_cells=Mock(),
            )

            def write_or_validate(path, value):
                if path.is_file():
                    self.assertEqual(json.loads(path.read_text()), value)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(json.dumps(value))

            runner._write_or_validate_json = write_or_validate
            with (
                patch(
                    "benchmarks.scale.fleet.validate_imported_inputs",
                    return_value={"images": "pinned"},
                ),
                patch(
                    "benchmarks.scale.fleet._source_manifest",
                    return_value={"host": "worker-1"},
                ),
                patch(
                    "benchmarks.scale.fleet._write_quality",
                    return_value=[],
                ),
            ):
                summary, exit_code = execute_shard(
                    runner,
                    engine="pg_textsearch",
                    profile="high",
                    dataset="msmarco-v2",
                    host="worker-1",
                    attempt_id="resume-1",
                )

            runner.run_cells.assert_not_called()
            self.assertEqual(exit_code, 2)
            self.assertEqual(summary["expected_cells"], 28)
            self.assertEqual(summary["terminal_counts"]["failed"], 1)

    def test_run_id_is_caller_selected_but_must_be_one_safe_component(self):
        runner = SimpleNamespace(
            run_id="../other-run",
            tin_dir=Path("unused"),
        )

        with self.assertRaisesRegex(ConfigError, "safe path component"):
            execute_shard(
                runner,
                engine="pg_textsearch",
                profile="high",
                dataset="msmarco-v2",
            )

    def test_paradedb_build_flag_never_triggers_native_build_side_effect(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = SimpleNamespace(
                run_id="tin-wikipedia-20261119-final-v1",
                tin_dir=root / "tin",
                build_selected=Mock(),
            )
            runner.tin_dir.mkdir()
            runner._write_or_validate_json = (
                lambda path, value: path.write_text(json.dumps(value))
            )

            with self.assertRaisesRegex(
                ConfigError, "only safe on a native worker"
            ):
                execute_shard(
                    runner,
                    engine="paradedb",
                    profile="high",
                    dataset="wikipedia",
                    build=True,
                )

            runner.build_selected.assert_not_called()

    def test_unrelated_run_error_overrides_existing_measured_failure(self):
        summary, exit_code = self.execute_with_run_error(
            ConfigError("docker stop failed"),
            legitimate_summary=False,
        )

        self.assertEqual(exit_code, 3)
        self.assertEqual(summary["status"], "infrastructure_error")

    def test_matching_run_summary_preserves_measured_failure_exit(self):
        summary, exit_code = self.execute_with_run_error(
            ConfigError("TIN campaign has 1 failed cells"),
            legitimate_summary=True,
        )

        self.assertEqual(exit_code, 2)
        self.assertEqual(summary["status"], "measured_failures")

    def test_malformed_failed_summary_is_not_a_measured_failure_signal(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.json"
            path.write_text(json.dumps({
                "status": "failed",
                "failures": [None],
                "selected_cells": 1,
                "engines": ["pg_textsearch"],
            }))

            self.assertFalse(_is_measured_run_error(
                ConfigError("TIN campaign has 1 failed cells"),
                path,
                "pg_textsearch",
                {"cell-1"},
                [{"cell_id": "cell-1", "status": "failed"}],
            ))

    def test_write_quality_is_derived_from_raw_completed_metrics(self):
        cell = {
            "cell_id": "write-cell",
            "operation": "concurrent-writes",
        }
        derived = {"actual_writes": 300000}
        runner = SimpleNamespace(
            tin_dir=Path("/artifacts/tin"),
            _record_write_quality=Mock(return_value=derived),
        )

        quality = _write_quality(
            runner,
            "paradedb",
            [cell],
            [{
                "cell_id": "write-cell",
                "status": "completed",
                "marker": "complete.json",
            }],
        )

        runner._record_write_quality.assert_called_once_with(
            result_dir=(
                Path("/artifacts/tin/results/paradedb/write-cell")
            ),
            cell=cell,
            engine="paradedb",
            duration="5m",
            sql_status="ok",
        )
        self.assertEqual(quality[0]["quality"], derived)

    def test_failed_write_quality_is_cross_checked_against_raw_metrics(self):
        cell = {
            "cell_id": "write-cell",
            "operation": "concurrent-writes",
            "update_rate": 1000,
        }
        with tempfile.TemporaryDirectory() as directory:
            tin_dir = Path(directory)
            result = tin_dir / "results/paradedb/write-cell"
            result.mkdir(parents=True)
            (result / "k6-summary.json").write_text(json.dumps({
                "metrics": {
                    "update_failures": {"passes": 1, "fails": 2999},
                    "achieved_updates": {"count": 2999},
                    "dropped_iterations": {"count": 0},
                },
            }))
            (result / "load-quality.json").write_text("{}")
            runner = SimpleNamespace(
                tin_dir=tin_dir,
                _write_or_validate_json=Mock(
                    side_effect=ConfigError("quality mismatch")
                ),
            )

            with self.assertRaisesRegex(ConfigError, "quality mismatch"):
                _write_quality(
                    runner,
                    "paradedb",
                    [cell],
                    [{
                        "cell_id": "write-cell",
                        "status": "failed",
                        "marker": "failed.json",
                    }],
                )


class FleetImportTest(unittest.TestCase):
    def test_imported_build_rejects_stale_analyzer_source_fingerprint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / "dataset"
            tin_dir = root / "tin"
            (dataset / "k6").mkdir(parents=True)
            (dataset / "backends").mkdir()
            (dataset / "manifest.json").write_text(json.dumps({
                "rows": 6_407_814,
            }))
            (dataset / "k6/update_targets.json").write_text("[]")
            (dataset / "k6/natural.json").write_text("[]")
            build_dir = (
                tin_dir
                / "build/pg_textsearch/english-no-stopwords"
            )
            build_dir.mkdir(parents=True)
            base = {"prepared": "fingerprint"}
            base_dir = tin_dir / "build/pg_textsearch/prepared-corpus"
            base_dir.mkdir(parents=True)
            (base_dir / "complete.json").write_text(json.dumps(base))
            (build_dir / "complete.json").write_text(json.dumps({
                "format": 1,
                "engine": "pg_textsearch",
                "analyzer": ANALYZERS["english-no-stopwords"],
                "image": "sha256:native",
                "sql_sha256": "stale",
                "build_resources": BUILD_RESOURCES,
                "base": base,
                "database_proof_protocol": DATABASE_PROOF_PROTOCOL,
            }))
            proof = {
                "rows": 6_407_814,
                "index_bytes": 100,
                "table_total_bytes": 200,
            }
            (build_dir / "build.json").write_text(json.dumps({
                "database_proof_protocol": DATABASE_PROOF_PROTOCOL,
                "database": proof,
            }))
            (build_dir / "rowcount-and-sizes.tsv").write_text(
                "6407814\n100\t200\n"
            )
            runner = SimpleNamespace(
                dataset=dataset,
                dataset_kind="wikipedia",
                tin_dir=tin_dir,
                _images=Mock(return_value=(
                    {"pg_textsearch": "sha256:native"},
                    {"lock": "fingerprint"},
                )),
                _volume_name=Mock(return_value="analyzer-volume"),
                _require_owned_volume=Mock(),
            )
            cell = {
                "analyzer": "english-no-stopwords",
                "pack": "natural",
            }

            with self.assertRaisesRegex(
                ConfigError, "analyzer proof"
            ):
                validate_imported_inputs(
                    runner, "pg_textsearch", [cell]
                )


if __name__ == "__main__":
    unittest.main()
