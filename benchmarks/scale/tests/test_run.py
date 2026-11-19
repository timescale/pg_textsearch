import json
import subprocess
import unittest
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from benchmarks.scale.dataset import prepare_synthetic
from benchmarks.scale.run import (
    BenchmarkCellsFailed,
    ConfigError,
    GO_VERSION,
    K6_VERSION,
    Runner,
    build_plan,
    file_sha256,
    validate_storage,
)


class RunPlanTest(unittest.TestCase):
    def test_smoke_plan_is_sequential_and_uses_actual_benchmarker(self):
        plan = build_plan(smoke=True, accept_terms=False)

        self.assertEqual(
            [cell["engine"] for cell in plan["cells"]],
            ["pg_textsearch", "paradedb", "lead"],
        )
        self.assertEqual(plan["benchmarker_sha"], (
            "4b262c05af138991d3bf28c683a0fc6b7f113df3"
        ))
        self.assertEqual(plan["dataset"]["kind"], "synthetic-smoke")
        self.assertEqual(plan["cells"][2]["row_limit"], 10_000)

    def test_full_plan_requires_explicit_terms_acceptance(self):
        with self.assertRaisesRegex(ConfigError, "MS MARCO terms"):
            build_plan(smoke=False, accept_terms=False)

    def test_wikipedia_plan_does_not_bypass_or_require_msmarco_terms(self):
        plan = build_plan(
            smoke=False,
            accept_terms=False,
            dataset_kind="wikipedia",
        )

        self.assertEqual(plan["dataset"]["kind"], "wikipedia")
        self.assertEqual(plan["dataset"]["rows"], 6_407_814)

    def test_pg_textsearch_records_boolean_and_count_cells(self):
        plan = build_plan(smoke=True, accept_terms=False)
        pgts = plan["cells"][0]

        self.assertEqual(pgts["modes"]["disjunction"], [10, 100])
        self.assertEqual(pgts["modes"]["conjunction"], [10, 100])
        self.assertEqual(pgts["modes"]["phrase"], [10, 100])
        self.assertEqual(pgts["modes"]["count"], [None])
        self.assertEqual(pgts["unsupported"], {})

    def test_remote_storage_must_be_a_ready_mount(self):
        with self.assertRaisesRegex(ConfigError, "not mounted"):
            validate_storage(
                Path("/srv/searchbench/run"),
                smoke=True,
                is_mount=lambda _: False,
                free_bytes=lambda _: 10**12,
            )

        validate_storage(
            Path("/srv/searchbench/run"),
            smoke=True,
            is_mount=lambda _: True,
            free_bytes=lambda _: 10**12,
        )

    def test_msmarco_queries_are_published_where_runner_consumes_them(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            artifacts = Path(directory)
            runner = Runner(
                scale_dir.parents[1], artifacts, False,
                "query-preparation", "msmarco-v2", None,
            )
            prepare_synthetic(runner.dataset, rows=10)
            source = artifacts / "queries.tsv"
            source.write_text("1\tsearch ranking\n")
            with patch.dict("os.environ", {
                "MSMARCO_ACCEPT_TERMS": "1",
                "MSMARCO_QUERY_FILES": f"dev1={source}",
            }):
                runner._prepare_msmarco()
            runner._validate_query_packs(runner.dataset)
            natural = json.loads(
                (runner.dataset / "k6/natural.json").read_text()
            )
            self.assertEqual(natural[0]["text"], "search ranking")

    def test_lead_subset_cannot_reuse_another_corpus(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            artifacts = Path(directory)
            for kind, seed in (("wikipedia", 3), ("msmarco-v2", 7)):
                runner = Runner(
                    scale_dir.parents[1], artifacts, False,
                    "lead-corpus", kind, None,
                )
                prepare_synthetic(runner.dataset, rows=10_000, seed=seed)
                runner._prepare_lead_subset()
                self.assertEqual(
                    json.loads(
                        (runner.lead_dataset / "source-manifest.json")
                        .read_text()
                    ),
                    json.loads(
                        (runner.dataset / "manifest.json").read_text()
                    ),
                )

    def test_lead_subset_refreshes_queries_but_rejects_changed_source(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            runner = Runner(
                scale_dir.parents[1], Path(directory), False,
                "lead-refresh", "msmarco-v2", None,
            )
            prepare_synthetic(runner.dataset, rows=10_000)
            queries = runner.dataset / "k6"
            queries.mkdir()
            source = queries / "spans.json"
            source.write_text('[{"id":"old","text":"old term"}]')
            runner._prepare_lead_subset()
            source.write_text(json.dumps([
                {"id": "new", "text": 'New "OR" wi-fi'},
            ]))
            runner._prepare_lead_subset()
            self.assertEqual(
                json.loads(
                    (runner.lead_dataset / "k6/spans.json").read_text()
                ),
                [{
                    "id": "new",
                    "text": '"new" OR "or" OR "wi" OR "fi"',
                    "source_text": 'New "OR" wi-fi',
                }],
            )
            manifest = runner.dataset / "manifest.json"
            data = json.loads(manifest.read_text())
            data["rows"] += 1
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(ConfigError, "incompatible resume"):
                runner._prepare_lead_subset()

    def test_install_writes_only_tool_and_image_pins(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            artifacts = Path(directory)
            (artifacts / "tools" / "benchmarker").mkdir(parents=True)
            runner = Runner(
                scale_dir.parents[1],
                artifacts,
                True,
                "install-test",
                "msmarco-v2",
                None,
            )

            def fake_run(args, **_kwargs):
                if args[:3] == ["docker", "info", "--format"]:
                    return SimpleNamespace(
                        returncode=0,
                        stdout="/srv/searchbench/docker\n",
                    )
                if args[:3] == ["go", "env", "GOPATH"]:
                    return SimpleNamespace(returncode=0, stdout="/go\n")
                if args[:2] == ["go", "version"]:
                    return SimpleNamespace(
                        returncode=0,
                        stdout=f"go version {GO_VERSION} linux/amd64\n",
                    )
                if args[:4] == [
                    "docker", "image", "inspect", "postgres:18-bookworm"
                ]:
                    return SimpleNamespace(
                        returncode=0,
                        stdout=json.dumps([{
                            "RepoDigests": ["postgres@sha256:base"]
                        }]),
                    )
                if args[:4] == [
                    "docker", "image", "inspect",
                    "paradedb/paradedb:v0.26.8-pg18",
                ]:
                    return SimpleNamespace(
                        returncode=0,
                        stdout=json.dumps([{
                            "RepoDigests": ["paradedb/paradedb@sha256:parade"]
                        }]),
                    )
                if args[:3] == ["docker", "image", "inspect"]:
                    return SimpleNamespace(
                        returncode=0, stdout="sha256:pgts\n"
                    )
                raise AssertionError(args)

            with (
                patch("benchmarks.scale.run.shutil.which",
                      return_value="/usr/bin/tool"),
                patch("benchmarks.scale.run.subprocess.run",
                      side_effect=fake_run),
                patch.object(Runner, "command") as command,
            ):
                runner.install()

            lock = json.loads(
                (artifacts / "images.lock.json").read_text()
            )
            self.assertEqual(lock["k6"], K6_VERSION)
            self.assertEqual(lock["go"], GO_VERSION)
            self.assertNotIn("database", lock)
            build_call = next(
                call for call in command.call_args_list
                if call.args[0][0] == "/go/bin/xk6"
            )
            self.assertEqual(build_call.kwargs["env"]["GOTOOLCHAIN"],
                             "local")
            self.assertEqual(
                build_call.kwargs["env"]["TMPDIR"],
                str(artifacts / "tools/tmp"),
            )

    def test_k6_runs_warmup_separately_from_measured_output(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            runner = Runner(
                scale_dir.parents[1],
                Path(directory),
                True,
                "warmup-test",
                "msmarco-v2",
                None,
            )
            result_dir = Path(directory) / "results"
            result_dir.mkdir()
            with patch.object(Runner, "command") as command:
                runner._run_k6_cell(
                    Path("/benchmarker"),
                    Path("/dataset/k6/search.js"),
                    {
                        "DURATION": "3s",
                        "WARMUP": "2s",
                        "TOTAL_DURATION": "8s",
                    },
                    result_dir,
                )

            warmup, measured = command.call_args_list
            self.assertNotIn("--out", warmup.args[0])
            self.assertNotIn("--summary-export", warmup.args[0])
            self.assertEqual(warmup.kwargs["env"]["PHASE"], "warmup")
            self.assertEqual(warmup.kwargs["env"]["DURATION"], "2s")
            self.assertEqual(warmup.kwargs["output"],
                             result_dir / "warmup.log")
            self.assertIn("--out", measured.args[0])
            summary_index = measured.args[0].index("--summary-export")
            self.assertEqual(
                measured.args[0][summary_index + 1],
                str(result_dir / "k6-summary.json"),
            )
            self.assertEqual(measured.kwargs["env"]["PHASE"], "measured")
            self.assertEqual(measured.kwargs["env"]["DURATION"], "3s")
            self.assertEqual(measured.kwargs["output"],
                             result_dir / "k6.log")

    def test_k6_script_does_not_tag_a_constant_vus_warmup(self):
        script = (
            Path(__file__).resolve().parents[1]
            / "benchmarker/k6/search.js"
        ).read_text()

        self.assertIn('phase === "measured"', script)
        self.assertNotIn('warmup: "true"', script)

    def test_plans_are_captured_without_executing_queries(self):
        source = (
            Path(__file__).resolve().parents[1] / "run.py"
        ).read_text()

        self.assertNotIn("EXPLAIN (ANALYZE", source)
        self.assertIn(
            "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON)",
            source,
        )

    def test_engine_refreshes_dataset_templates_without_prepare(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            temporary = Path(directory)
            root = temporary / "repo"
            template = (
                root / "benchmarks/scale/benchmarker/k6/search.js"
            )
            template.parent.mkdir(parents=True)
            template.write_text("current")
            dataset = temporary / "dataset"
            stale = dataset / "k6/search.js"
            stale.parent.mkdir(parents=True)
            stale.write_text("stale")
            runner = Runner(
                root,
                temporary / "artifacts",
                True,
                "refresh-test",
                "msmarco-v2",
                None,
            )

            runner._refresh_templates(dataset)

            self.assertEqual(stale.read_text(), "current")

    def test_wait_requires_external_postgres_connection(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            runner = Runner(
                scale_dir.parents[1],
                Path(directory),
                True,
                "wait-test",
                "msmarco-v2",
                None,
            )
            results = [
                SimpleNamespace(returncode=0),
                SimpleNamespace(returncode=1),
                SimpleNamespace(returncode=0),
                SimpleNamespace(returncode=0),
            ]
            with (
                patch("benchmarks.scale.run.subprocess.run",
                      side_effect=results) as run,
                patch("benchmarks.scale.run.time.sleep"),
            ):
                runner._wait("database")

            self.assertEqual(run.call_count, 4)
            self.assertEqual(run.call_args_list[1].args[0][0], "psql")

    def test_msmarco_prepare_downloads_and_merges_pinned_assets(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            temporary = Path(directory)
            runner = Runner(
                scale_dir.parents[1],
                temporary / "artifacts",
                False,
                "msmarco-test",
                "msmarco-v2",
                None,
            )
            assets = {
                "archive": temporary / "archive.tar",
                "dev1": temporary / "dev1.tsv",
                "dev2": temporary / "dev2.tsv",
            }
            with (
                patch("benchmarks.scale.run.download_assets",
                      return_value=assets) as download,
                patch("benchmarks.scale.run.prepare_main",
                      return_value=0) as prepare,
                patch.dict(
                    "os.environ",
                    {"MSMARCO_ACCEPT_TERMS": "1"},
                    clear=True,
                ),
            ):
                runner._prepare_msmarco()

            download.assert_called_once_with(
                runner.artifacts / "downloads/msmarco-v2",
                accepted=True,
            )
            corpus_args = prepare.call_args_list[0].args[0]
            query_args = prepare.call_args_list[1].args[0]
            self.assertIn(str(assets["archive"]), corpus_args)
            self.assertIn(f"dev1={assets['dev1']}", query_args)
            self.assertIn(f"dev2={assets['dev2']}", query_args)
            self.assertIn(str(runner.dataset / "data"), query_args)

    def test_install_script_pins_go_toolchain(self):
        script = (
            Path(__file__).resolve().parents[1] / "install.sh"
        ).read_text()

        self.assertIn("go1.26.11.linux-amd64.tar.gz", script)
        self.assertIn(
            "9849f4e04ee35303714e34fbc3ac8ab44ea3e448a95aa182f91e3b9b79c710ec",
            script,
        )

    def test_correctness_is_bounded_checkpointed_and_classified(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            temporary = Path(directory)
            runner = Runner(
                scale_dir.parents[1],
                temporary,
                False,
                "correctness-test",
                "wikipedia",
                None,
            )
            queries = [
                {"id": f"q{number}", "text": f"query {number}"}
                for number in range(10)
            ]
            success = SimpleNamespace(
                returncode=0, stdout="1\n2\n", stderr=""
            )
            timeout = SimpleNamespace(
                returncode=1,
                stdout="",
                stderr="ERROR: canceling statement due to statement timeout",
            )
            sql_error = SimpleNamespace(
                returncode=1, stdout="", stderr="ERROR: operator missing"
            )
            results = [success, timeout] + [sql_error] * 6
            output = temporary / "correctness-v2.jsonl"
            with patch(
                "benchmarks.scale.run.subprocess.run",
                side_effect=results,
            ) as run:
                summary = runner._capture_correctness(
                    "database",
                    "pg_textsearch",
                    queries,
                    output,
                    {"database": "fingerprint"},
                )

            records = [
                json.loads(line) for line in output.read_text().splitlines()
            ]
            self.assertEqual(run.call_count, 8)
            self.assertEqual({record["query_id"] for record in records},
                             {"q0", "q5"})
            self.assertEqual(summary["ok"], 1)
            self.assertEqual(summary["timeout"], 1)
            self.assertEqual(summary["sql_error"], 6)
            self.assertTrue(records[1]["censored"])
            self.assertTrue(
                output.with_suffix(".complete.json").is_file()
            )

            with patch(
                "benchmarks.scale.run.subprocess.run"
            ) as resumed_run:
                resumed = runner._capture_correctness(
                    "database",
                    "pg_textsearch",
                    queries,
                    output,
                    {"database": "fingerprint"},
                )
            resumed_run.assert_not_called()
            self.assertEqual(resumed, summary)

    def test_correctness_uses_natural_and_longest_span_families(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            temporary = Path(directory)
            dataset = temporary / "dataset"
            query_dir = dataset / "k6"
            query_dir.mkdir(parents=True)
            (query_dir / "natural.json").write_text(json.dumps([
                {"id": "natural-1", "text": "natural query"},
                {"id": "natural-2", "text": "other natural"},
            ]))
            (query_dir / "spans.json").write_text(json.dumps([
                {"id": "span-2", "text": "two words", "span": 2},
                {
                    "id": "span-15",
                    "text": "controlled fifteen word span",
                    "span": 15,
                },
                {"id": "span-10", "text": "controlled span", "span": 10},
            ]))
            runner = Runner(
                scale_dir.parents[1],
                temporary / "artifacts",
                False,
                "families-test",
                "wikipedia",
                dataset,
            )

            queries, fingerprint = runner._correctness_inputs(
                dataset, {"database": "fingerprint"}
            )

            self.assertEqual(
                [(query["id"], query["query_family"])
                 for query in queries],
                [("natural-1", "natural"), ("span-15", "controlled_span")],
            )
            self.assertEqual(
                fingerprint,
                {
                    "database": {"database": "fingerprint"},
                    "natural_queries": file_sha256(
                        query_dir / "natural.json"
                    ),
                    "span_queries": file_sha256(
                        query_dir / "spans.json"
                    ),
                },
            )

    def test_failed_k6_cell_is_recorded_without_raising(self):
        scale_dir = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=scale_dir
        ) as directory:
            temporary = Path(directory)
            runner = Runner(
                scale_dir.parents[1],
                temporary,
                True,
                "cell-test",
                "msmarco-v2",
                None,
            )
            result_dir = temporary / "cell"
            result_dir.mkdir()
            error = subprocess.CalledProcessError(99, ["k6", "run"])
            with patch.object(
                runner, "_run_k6_cell", side_effect=error
            ):
                result = runner._execute_k6_cell(
                    Path("/benchmarker"),
                    Path("/dataset/search.js"),
                    {},
                    result_dir,
                    {"mode": "conjunction"},
                )

            self.assertEqual(result["status"], "failed")
            failure = json.loads(
                (result_dir / "failed.json").read_text()
            )
            self.assertEqual(failure["exit_code"], 99)
            self.assertFalse((result_dir / "complete.json").exists())

    def test_engine_failures_do_not_skip_later_engines(self):
        calls = []

        class FakeRunner:
            def engine(self, engine):
                calls.append(engine)
                if engine == "pg_textsearch":
                    raise BenchmarkCellsFailed(engine, ["failed cell"])

        with self.assertRaises(BenchmarkCellsFailed):
            Runner.run_engines(
                FakeRunner(),
                ["pg_textsearch", "paradedb", "lead"],
            )

        self.assertEqual(calls, ["pg_textsearch", "paradedb", "lead"])


if __name__ == "__main__":
    unittest.main()
