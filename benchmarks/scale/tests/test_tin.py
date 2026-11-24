import hashlib
import json
import subprocess
import unittest
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from benchmarks.scale.run import ConfigError
from benchmarks.scale.tin import (
    ANALYZERS,
    BUILD_RESOURCES,
    PARADEDB_IMAGE,
    PARADEDB_TAG,
    PROFILES,
    TinRunner,
    analyzer_sql,
    apply_sql_shape,
    build_tin_plan,
    classify_explain,
    database_size_proof,
    interleaved_selection,
    load_quality,
    ordered_cells,
    parse_plan_audit,
    probe_sql,
    scenario_sql,
    select_cells,
)


class TinPlanTest(unittest.TestCase):
    def test_partial_database_load_is_rejected(self):
        with self.assertRaisesRegex(ConfigError, "row count"):
            database_size_proof("99\n1024\t4096\n", expected_rows=100)
        with self.assertRaisesRegex(ConfigError, "incomplete"):
            database_size_proof("100\n", expected_rows=100)
        self.assertEqual(
            database_size_proof(
                "100\n1024\t4096\n", expected_rows=100
            ),
            {"rows": 100, "index_bytes": 1024, "table_total_bytes": 4096},
        )

    def test_tin_dependency_install_preserves_original_image_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "images.lock.json"
            original.write_text('{"baseline":"preserve"}\n')
            runner = TinRunner(
                Path(__file__).resolve().parents[3], root,
                True, "install-proof", "msmarco-v2", None,
            )
            dependency_lock = {
                "pg_textsearch_source": "source",
                "pg_textsearch_image_id": "sha256:native",
                "postgres_base": "postgres@sha256:base",
            }

            def install_dependencies(installer):
                (installer.artifacts / "images.lock.json").write_text(
                    json.dumps(dependency_lock)
                )
                self.assertEqual(installer.tools, runner.tools)

            inspection = subprocess.CompletedProcess(
                [], 0, stdout=json.dumps([{
                    "RepoDigests": [PARADEDB_IMAGE]
                }]),
            )
            with patch(
                "benchmarks.scale.tin.Runner.install", autospec=True,
                side_effect=install_dependencies,
            ), patch.object(runner, "command"), patch(
                "benchmarks.scale.tin.subprocess.run",
                return_value=inspection,
            ):
                runner.install()
            self.assertEqual(
                original.read_text(), '{"baseline":"preserve"}\n'
            )
            lock = json.loads((root / "tin-images.lock.json").read_text())
            self.assertEqual(lock["pg_textsearch_image_id"], "sha256:native")

    def test_primary_matrix_has_22_cells_per_memory_profile(self):
        plan = build_tin_plan(
            smoke=False,
            accept_terms=True,
            dataset_kind="msmarco-v2",
        )

        primary = [
            cell for cell in plan["cells"]
            if cell["analyzer"] == "english-no-stopwords"
        ]
        self.assertEqual(PARADEDB_TAG, "paradedb/paradedb:v0.27.0-pg18")
        self.assertEqual(
            {profile: sum(
                cell["profile"] == profile for cell in primary
            ) for profile in plan["profiles"]},
            {"high": 22, "constrained32": 22, "constrained16": 22},
        )
        self.assertEqual(
            {
                (cell["pack"], cell["operation"], cell["query_family"])
                for cell in primary
                if cell["profile"] == "high"
            },
            {
                ("natural", "search", "disjunction"),
                ("natural", "search", "conjunction"),
                ("natural", "search", "phrase"),
                ("natural", "search", "interleaved-all"),
                ("natural", "search", "interleaved-conjunction-phrase"),
                ("natural", "count", "disjunction"),
                ("natural", "count", "conjunction"),
                ("natural", "count", "phrase"),
                ("natural", "count", "interleaved-all"),
                ("natural", "concurrent-writes", "disjunction"),
                ("natural", "concurrent-writes", "interleaved-all"),
                ("spans", "search", "disjunction"),
                ("spans", "search", "conjunction"),
                ("spans", "search", "phrase"),
                ("spans", "search", "interleaved-all"),
                ("spans", "search", "interleaved-conjunction-phrase"),
                ("spans", "count", "disjunction"),
                ("spans", "count", "conjunction"),
                ("spans", "count", "phrase"),
                ("spans", "count", "interleaved-all"),
                ("spans", "concurrent-writes", "disjunction"),
                ("spans", "concurrent-writes", "interleaved-all"),
            },
        )
        self.assertTrue(all(cell["vus"] == 8 for cell in primary))
        self.assertTrue(all(cell["limit"] == 10 for cell in primary))

    def test_profiles_and_build_resources_are_explicit_and_separate(self):
        plan = build_tin_plan(
            smoke=False,
            accept_terms=True,
            dataset_kind="msmarco-v2",
        )

        self.assertEqual(plan["database_cpus"], "0-7")
        self.assertEqual(plan["client_cpus"], "24-31")
        self.assertEqual(
            {
                name: (value["memory"], value["shared_buffers"])
                for name, value in plan["profiles"].items()
            },
            {
                "high": ("192g", "32GB"),
                "constrained32": ("32g", "24GB"),
                "constrained16": ("16g", "12GB"),
            },
        )
        self.assertEqual(plan["build_resources"]["memory"], "192g")
        self.assertEqual(
            plan["build_resources"]["random_page_cost"], 1.1
        )
        self.assertTrue(all(
            profile["random_page_cost"] == 1.1
            for profile in plan["profiles"].values()
        ))
        self.assertFalse(plan["build_resources"]["minimum_claimed"])
        self.assertEqual(
            plan["cache_reset"]["status"], "required-before-profile"
        )

    def test_selectors_keep_full_cell_metadata(self):
        plan = build_tin_plan(
            smoke=False,
            accept_terms=True,
            dataset_kind="msmarco-v2",
        )

        selected = select_cells(
            plan["cells"],
            profiles={"constrained16"},
            operations={"count"},
            packs={"spans"},
            analyzers={"english-no-stopwords"},
        )

        self.assertEqual(len(selected), 4)
        self.assertTrue(all(cell["profile"] == "constrained16"
                            for cell in selected))
        self.assertTrue(all(cell["operation"] == "count"
                            for cell in selected))
        self.assertTrue(all(cell["pack"] == "spans" for cell in selected))
        self.assertTrue(all("cell_id" in cell for cell in selected))

    def test_sql_shape_override_uses_distinct_resumable_cell_ids(self):
        plan = build_tin_plan(
            smoke=True,
            accept_terms=False,
            dataset_kind="msmarco-v2",
        )

        original = apply_sql_shape(plan["cells"][:1], "original")[0]

        self.assertEqual(original["sql_shape"], "original")
        self.assertTrue(original["cell_id"].endswith(".shape-original"))
        self.assertNotEqual(original["cell_id"], plan["cells"][0]["cell_id"])

    def test_wikipedia_control_is_a_separate_disjunction_count_cell(self):
        plan = build_tin_plan(
            smoke=False,
            accept_terms=True,
            dataset_kind="msmarco-v2",
        )

        self.assertEqual(
            plan["controls"]["wikipedia"],
            {
                "dataset": "wikipedia",
                "operation": "count",
                "query_family": "disjunction",
                "pack": "natural",
                "separate_campaign": True,
            },
        )

        wikipedia = build_tin_plan(
            smoke=False,
            accept_terms=False,
            dataset_kind="wikipedia",
        )
        self.assertEqual(len(wikipedia["cells"]), 3)
        self.assertTrue(all(
            cell["pack"] == "natural"
            and cell["operation"] == "count"
            and cell["query_family"] == "disjunction"
            and cell["analyzer"] == "english-no-stopwords"
            for cell in wikipedia["cells"]
        ))

    def test_no_stopword_analyzers_do_not_mutate_baselines(self):
        native = analyzer_sql(
            "pg_textsearch", "english-no-stopwords"
        )
        parade = analyzer_sql("paradedb", "english-no-stopwords")

        self.assertIn("CREATE TEXT SEARCH CONFIGURATION", native)
        self.assertIn("tin_english_no_stopwords", native)
        self.assertIn("CREATE TEXT SEARCH DICTIONARY", native)
        self.assertNotIn("ALTER TEXT SEARCH DICTIONARY english_stem", native)
        mapping = native.split("ALTER MAPPING FOR ", 1)[1].split(
            "\n  WITH", 1
        )[0]
        self.assertEqual(
            {token.strip() for token in mapping.replace("\n", "").split(",")},
            {
                "asciiword",
                "asciihword",
                "hword_asciipart",
                "word",
                "hword",
                "hword_part",
            },
        )
        self.assertNotIn("numword", mapping)
        self.assertNotIn("stopwords_language", parade)
        self.assertIn("DROP INDEX IF EXISTS documents_bm25", parade)

    def test_sql_templates_use_rewrites_and_exact_pdb_counts(self):
        calls = []

        def query_sql(kind, operation, shape, limit):
            calls.append((kind, operation, shape, limit))
            return f"{kind}:{operation}:{shape}:{limit}"

        native = scenario_sql(
            "pg_textsearch",
            operation="search",
            family="interleaved-all",
            limit=10,
            native_query_sql=query_sql,
        )
        parade_count = scenario_sql(
            "paradedb",
            operation="count",
            family="interleaved-all",
            limit=10,
        )

        self.assertEqual(
            calls,
            [
                ("disjunction", "search", "rank_first", 10),
                ("conjunction", "search", "rank_first", 10),
                ("phrase", "search", "rank_first", 10),
            ],
        )
        self.assertEqual(
            native,
            {
                "disjunction": "disjunction:search:rank_first:10",
                "conjunction": "conjunction:search:rank_first:10",
                "phrase": "phrase:search:rank_first:10",
            },
        )
        self.assertEqual(
            parade_count["disjunction"],
            "SELECT count(*) FROM documents WHERE body ||| $1",
        )
        self.assertEqual(
            parade_count["conjunction"],
            "SELECT count(*) FROM documents WHERE body &&& $1",
        )
        self.assertEqual(
            parade_count["phrase"],
            "SELECT count(*) FROM documents WHERE body @@@ $1",
        )

    def test_tin_driver_records_operation_family_errors_and_writes(self):
        script = (
            Path(__file__).resolve().parents[1]
            / "benchmarker/k6/tin.js"
        ).read_text()

        self.assertIn("...commonTags", script)
        self.assertIn("query_family: queryFamily", script)
        self.assertIn("cell_id: cellId", script)
        self.assertIn("pack, profile, analyzer", script)
        self.assertIn('new Counter("achieved_updates")', script)
        self.assertIn('new Rate("benchmark_errors")', script)
        self.assertIn('new Rate("update_failures")', script)
        self.assertNotIn('dropped_iterations: ["count==0"]', script)
        self.assertIn("preAllocatedVUs: 32", script)
        self.assertIn("maxVUs: 64", script)
        self.assertIn("JSON.stringify(query.text)", script)
        self.assertIn("Math.floor(iteration / kindCount)", script)
        self.assertIn("kindIndex * Math.floor(queryCount / kindCount)", script)
        self.assertIn("`${query.id}:${kind}`", script)

    def test_interleaving_visits_every_query_for_every_family(self):
        query_count = 6
        kind_count = 3
        seen = {kind: set() for kind in range(kind_count)}

        for iteration in range(query_count * kind_count):
            query, kind = interleaved_selection(
                iteration, query_count, kind_count
            )
            seen[kind].add(query)

        self.assertEqual(
            seen,
            {kind: set(range(query_count)) for kind in range(kind_count)},
        )
        adjacent = [
            interleaved_selection(i, query_count, kind_count)[0]
            for i in range(kind_count)
        ]
        self.assertEqual(len(set(adjacent)), kind_count)

    def test_read_only_cells_are_ordered_before_writes_across_packs(self):
        plan = build_tin_plan(
            smoke=True,
            accept_terms=False,
            dataset_kind="msmarco-v2",
        )
        selected = [
            cell for cell in plan["cells"]
            if cell["profile"] == "high"
            and cell["analyzer"] == "english-no-stopwords"
        ]

        ordered = ordered_cells(reversed(selected))
        first_write = next(
            index for index, cell in enumerate(ordered)
            if cell["operation"] == "concurrent-writes"
        )

        self.assertTrue(all(
            cell["operation"] != "concurrent-writes"
            for cell in ordered[:first_write]
        ))
        self.assertTrue(all(
            cell["operation"] == "concurrent-writes"
            for cell in ordered[first_write:]
        ))

    def test_probe_classification_separates_nonindexed_native_counts(self):
        indexed = json.dumps([{
            "Plan": {"Node Type": "Index Scan", "Index Name": "idx"}
        }])
        sequential = json.dumps([{
            "Plan": {"Node Type": "Aggregate", "Plans": [{
                "Node Type": "Seq Scan"
            }]}
        }])

        self.assertEqual(
            classify_explain(
                indexed,
                engine="pg_textsearch",
                operation="search",
                shape="rank_first",
            ),
            "indexed",
        )
        self.assertEqual(
            classify_explain(
                sequential,
                engine="pg_textsearch",
                operation="count",
                shape="rank_first",
            ),
            "non_indexed_count",
        )
        with self.assertRaisesRegex(ConfigError, "Index Scan"):
            classify_explain(
                sequential,
                engine="pg_textsearch",
                operation="search",
                shape="rank_first",
            )

    def test_count_probe_executes_full_scalar_count(self):
        sql = probe_sql(
            engine="pg_textsearch",
            operation="count",
            kind="conjunction",
            statement="SELECT count(*) FROM documents WHERE body @@ $1",
            query={
                "text": "alpha beta",
                "pg_tsquery": {"conjunction": "'alpha' & 'beta'"},
            },
            explain=False,
        )

        self.assertIn("PREPARE tin_probe(tsquery)", sql)
        self.assertIn(
            "EXECUTE tin_probe('''alpha'' & ''beta'''::tsquery)",
            sql,
        )
        self.assertNotIn("EXPLAIN", sql)


class TinRunnerTest(unittest.TestCase):
    def test_profile_execution_fails_closed_without_cache_reset_hook(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                Path(directory),
                smoke=True,
                run_id="tin-cache-gate",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )

            with patch.dict("os.environ", {}, clear=True):
                with self.assertRaisesRegex(
                    ConfigError, "PGTS_CACHE_RESET_HOOK"
                ):
                    runner.require_cache_reset("high", "profile-volume")

    def test_cache_reset_hook_must_return_verified_dedicated_vm_evidence(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            hook = root / "cache-hook"
            hook.write_text(
                "#!/bin/sh\nprintf '%s\\n' '{\"performed\":false}'\n"
            )
            hook.chmod(0o755)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-cache-evidence",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )

            with patch.dict(
                "os.environ", {"PGTS_CACHE_RESET_HOOK": str(hook)}, clear=True
            ):
                with self.assertRaisesRegex(ConfigError, "verified evidence"):
                    runner.require_cache_reset("high", "profile-volume")

    def test_cache_reset_hook_receives_phase_run_directory_and_volume(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            hook = root / "cache-hook"
            hook.write_text("#!/bin/sh\n")
            hook.chmod(0o755)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-cache-argv",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            evidence = {
                "performed": True,
                "dedicated_vm": True,
                "verified": True,
                "method": "posix_fadvise_dontneed",
                "phase": "phase-id",
                "volume": "profile-volume",
                "files": 10,
                "bytes": 1000,
                "previously_resident_pages": 20,
                "resident_pages_after": 0,
            }
            with (
                patch.dict(
                    "os.environ",
                    {"PGTS_CACHE_RESET_HOOK": str(hook)},
                    clear=True,
                ),
                patch(
                    "benchmarks.scale.tin.subprocess.run",
                    return_value=SimpleNamespace(
                        stdout=json.dumps(evidence) + "\n"
                    ),
                ) as run,
            ):
                runner.require_cache_reset("phase-id", "profile-volume")

            self.assertEqual(
                run.call_args.args[0],
                [
                    str(hook),
                    "phase-id",
                    str(runner.run_dir),
                    "profile-volume",
                ],
            )

    def test_cell_fingerprint_covers_scripts_packs_resources_and_sql(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            script = root / "tin.js"
            queries = root / "queries.json"
            image_lock = root / "images.json"
            script.write_text("script")
            queries.write_text("[]")
            image_lock.write_text(json.dumps({
                "pg_textsearch_image_id": "sha256:native",
                "paradedb": "paradedb@sha256:new",
            }))
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-fingerprint",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            cell = build_tin_plan(
                smoke=True,
                accept_terms=False,
                dataset_kind="msmarco-v2",
            )["cells"][0]

            fingerprint = runner.cell_fingerprint(
                cell=cell,
                engine="pg_textsearch",
                script=script,
                query_pack=queries,
                images_lock=image_lock,
                analyzer={"name": "english-no-stopwords", "sql": "CREATE"},
                sql_templates={"disjunction": "SELECT $1"},
                source_image={"source": "abc", "image": "sha256:native"},
            )

            self.assertEqual(fingerprint["cell"], cell)
            self.assertEqual(
                fingerprint["analyzer"]["name"], "english-no-stopwords"
            )
            self.assertEqual(
                fingerprint["sql_templates"]["disjunction"], "SELECT $1"
            )
            self.assertIn("script_sha256", fingerprint)
            self.assertIn("query_pack_sha256", fingerprint)
            self.assertIn("images_lock_sha256", fingerprint)
            self.assertEqual(fingerprint["source_image"]["source"], "abc")

    def test_profile_server_args_apply_memory_at_start(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                Path(directory),
                smoke=True,
                run_id="tin-profile",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )

            args = runner.server_args(
                name="tin-db",
                volume="tin-volume",
                image="paradedb@sha256:new",
                profile="constrained16",
                engine="paradedb",
                phase="query",
            )

            self.assertIn("--cpuset-cpus=0-7", args)
            self.assertIn("--memory=16g", args)
            self.assertIn("--memory-swap=16g", args)
            self.assertIn("shared_buffers=12GB", args)
            self.assertIn("effective_cache_size=14GB", args)
            self.assertIn("maintenance_work_mem=256MB", args)
            self.assertIn("plan_cache_mode=force_custom_plan", args)
            self.assertIn("random_page_cost=1.1", args)
            self.assertIn("managed-by=pg_textsearch-tin", args)
            self.assertIn(f"run-id={runner.run_id}", args)
            self.assertIn("engine=paradedb", args)

            build = runner.server_args(
                name="tin-build",
                volume="tin-volume",
                image="paradedb@sha256:new",
                profile="high",
                engine="paradedb",
                phase="build",
            )
            self.assertIn("maintenance_work_mem=8GB", build)
            self.assertIn("plan_cache_mode=force_custom_plan", build)
            self.assertIn("random_page_cost=1.1", build)
            self.assertEqual(
                PROFILES["constrained16"]["maintenance_work_mem"],
                "256MB",
            )
            self.assertEqual(
                BUILD_RESOURCES["maintenance_work_mem"], "8GB"
            )

    def test_start_refuses_preexisting_container_without_removing_it(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                Path(directory),
                smoke=True,
                run_id="tin-start-owner",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            existing = SimpleNamespace(
                returncode=0,
                stdout=json.dumps([{
                    "Config": {"Labels": {"managed-by": "someone-else"}}
                }]),
                stderr="",
            )
            with (
                patch(
                    "benchmarks.scale.tin.subprocess.run",
                    return_value=existing,
                ),
                patch.object(runner, "command") as command,
            ):
                with self.assertRaisesRegex(
                    ConfigError, "already exists"
                ):
                    runner._start_database(
                        name="database",
                        volume="volume",
                        image="sha256:image",
                        profile="high",
                        engine="paradedb",
                        output=Path(directory) / "start.log",
                    )

            command.assert_not_called()

    def test_stop_gracefully_removes_only_matching_owned_container(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                Path(directory),
                smoke=True,
                run_id="tin-stop-owner",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            runner._owned_containers = {"database": "paradedb"}
            owned = SimpleNamespace(
                returncode=0,
                stdout=json.dumps([{
                    "Config": {"Labels": {
                        "managed-by": "pg_textsearch-tin",
                        "run-id": runner.run_id,
                        "engine": "paradedb",
                    }}
                }]),
                stderr="",
            )
            log = Path(directory) / "postgres.log"
            with (
                patch(
                    "benchmarks.scale.tin.subprocess.run",
                    return_value=owned,
                ),
                patch.object(runner, "command") as command,
            ):
                runner._stop_database("database", log)

            self.assertEqual(
                [call.args[0] for call in command.call_args_list],
                [
                    ["docker", "stop", "--timeout", "60", "database"],
                    ["docker", "logs", "database"],
                    ["docker", "rm", "database"],
                ],
            )
            self.assertEqual(
                command.call_args_list[1].kwargs["output"], log
            )
            self.assertNotIn(
                "-f",
                command.call_args_list[2].args[0],
            )

    def test_stop_refuses_container_with_mismatched_labels(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                Path(directory),
                smoke=True,
                run_id="tin-stop-foreign",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            runner._owned_containers = {"database": "paradedb"}
            foreign = SimpleNamespace(
                returncode=0,
                stdout=json.dumps([{
                    "Config": {"Labels": {
                        "managed-by": "pg_textsearch-tin",
                        "run-id": "different-run",
                        "engine": "paradedb",
                    }}
                }]),
                stderr="",
            )
            with (
                patch(
                    "benchmarks.scale.tin.subprocess.run",
                    return_value=foreign,
                ),
                patch.object(runner, "command") as command,
            ):
                with self.assertRaisesRegex(ConfigError, "foreign"):
                    runner._stop_database(
                        "database", Path(directory) / "postgres.log"
                    )

            command.assert_not_called()

    def test_volume_name_is_stable_and_includes_profile(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                Path(directory),
                smoke=True,
                run_id="tin-volume-name",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )

            first = runner._volume_name(
                "profile",
                "pg_textsearch",
                "english-no-stopwords-constrained16",
            )
            second = runner._volume_name(
                "profile",
                "pg_textsearch",
                "english-no-stopwords-constrained16",
            )

            self.assertEqual(first, second)
            self.assertIn("constrained16", first)

    def test_clone_uses_pinned_image_and_validates_checkpoint(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-clone",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            checkpoint = root / "clone.json"
            labels = {
                "kind": "profile",
                "engine": "pg_textsearch",
                "profile": "high",
            }
            source_labels = {
                "kind": "analyzer",
                "engine": "pg_textsearch",
                "analyzer": "english-no-stopwords",
            }
            inspect_results = [
                SimpleNamespace(
                    returncode=0,
                    stdout=json.dumps([{"Labels": {
                        "managed-by": "pg_textsearch-tin",
                        "run-id": runner.run_id,
                    } | source_labels}]),
                ),
                SimpleNamespace(returncode=0, stdout=""),
                SimpleNamespace(returncode=1, stdout=""),
            ]
            with (
                patch(
                    "benchmarks.scale.tin.subprocess.run",
                    side_effect=inspect_results,
                ),
                patch.object(runner, "command") as command,
            ):
                runner._clone_volume(
                    "source",
                    "target",
                    labels,
                    checkpoint,
                    image="pg_textsearch@sha256:pinned",
                    source_labels=source_labels,
                )

            clone_command = command.call_args_list[-1].args[0]
            self.assertIn("pg_textsearch@sha256:pinned", clone_command)
            self.assertNotIn("postgres:18-bookworm", clone_command)
            saved = json.loads(checkpoint.read_text())
            self.assertEqual(saved["image"], "pg_textsearch@sha256:pinned")

            saved["source"] = "other"
            checkpoint.write_text(json.dumps(saved))
            target_inspect = SimpleNamespace(
                returncode=0,
                stdout=json.dumps([{"Labels": {
                    "managed-by": "pg_textsearch-tin",
                    "run-id": runner.run_id,
                } | labels}]),
            )
            with self.assertRaisesRegex(ConfigError, "incompatible resume"):
                with patch(
                    "benchmarks.scale.tin.subprocess.run",
                    side_effect=inspect_results[:2] + [target_inspect],
                ):
                    runner._clone_volume(
                        "source",
                        "target",
                        labels,
                        checkpoint,
                        image="pg_textsearch@sha256:pinned",
                        source_labels=source_labels,
                    )

    def test_images_rejects_tag_only_or_wrong_digest_locks(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-images",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            lock = {
                "paradedb_tag": PARADEDB_TAG,
                "paradedb": PARADEDB_TAG,
                "pg_textsearch_source": "wrong",
                "pg_textsearch_image_id": "native:tag",
            }
            (root / "tin-images.lock.json").write_text(json.dumps(lock))

            with self.assertRaisesRegex(ConfigError, "campaign digest"):
                runner._images()

    def test_count_probe_records_actual_value_and_nonindexed_status(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-probe",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            query_pack = root / "queries.json"
            query_pack.write_text(json.dumps([{
                "id": "q1",
                "text": "alpha beta",
                "pg_tsquery": {
                    "disjunction": "'alpha' | 'beta'",
                    "conjunction": "'alpha' & 'beta'",
                    "phrase": "'alpha' <-> 'beta'",
                },
            }]))
            cell = {
                "operation": "count",
                "query_family": "conjunction",
                "sql_shape": "rank_first",
            }
            result_dir = root / "result"
            result_dir.mkdir()
            sequential = json.dumps([{
                "Plan": {"Node Type": "Aggregate", "Plans": [{
                    "Node Type": "Seq Scan"
                }]}
            }])

            def command(_args, *, output, **_kwargs):
                if output.name.endswith(".explain.json"):
                    output.write_text(sequential)
                else:
                    output.write_text("42\n")

            with patch.object(runner, "command", side_effect=command):
                result = runner._probe_cell(
                "database",
                "pg_textsearch",
                cell,
                query_pack,
                {"conjunction": (
                    "SELECT count(*) FROM documents WHERE body @@ $1"
                )},
                result_dir,
            )

            self.assertFalse(result["timing_allowed"])
            self.assertEqual(
                result["records"][0]["classification"],
                "non_indexed_count",
            )
            self.assertEqual(result["records"][0]["actual_count"], 42)

    def test_probe_timeout_is_censored_and_does_not_block_timing(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-probe-timeout",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            query_pack = root / "queries.json"
            query_pack.write_text(json.dumps([{
                "id": "q1",
                "text": "alpha beta",
                "pg_tsquery": {
                    "disjunction": "'alpha' | 'beta'",
                    "conjunction": "'alpha' & 'beta'",
                    "phrase": "'alpha' <-> 'beta'",
                },
            }]))
            cell = {
                "operation": "search",
                "query_family": "conjunction",
                "sql_shape": "rank_first",
            }
            result_dir = root / "result"
            result_dir.mkdir()
            indexed = json.dumps([{
                "Plan": {"Node Type": "Index Scan"}
            }])
            commands = []

            def command(args, *, output, env, **_kwargs):
                commands.append((args, env))
                if output.name.endswith(".explain.json"):
                    output.write_text(indexed)
                    return
                output.write_text(
                    "ERROR: canceling statement due to statement timeout\n"
                )
                raise subprocess.CalledProcessError(1, args)

            with patch.object(runner, "command", side_effect=command):
                result = runner._probe_cell(
                    "database",
                    "pg_textsearch",
                    cell,
                    query_pack,
                    {"conjunction": (
                        "SELECT id FROM documents WHERE body @@ $2 "
                        "ORDER BY body <@> to_bm25query($1) LIMIT 10"
                    )},
                    result_dir,
                )

            self.assertTrue(result["timing_allowed"])
            self.assertEqual(result["records"][0]["status"], "timeout")
            self.assertTrue(result["records"][0]["censored"])
            self.assertTrue(all(
                [
                    "--env",
                    (
                        "PGOPTIONS=-c statement_timeout=30000 "
                        "-c plan_cache_mode=force_custom_plan "
                        "-c random_page_cost=1.1"
                    ),
                ] == args[2:4]
                for args, _env in commands
            ))
            self.assertTrue(all(
                "PGOPTIONS" not in env for _args, env in commands
            ))

    def test_mixed_load_quality_reports_pressure_without_sql_failure(self):
        summary = {
            "metrics": {
                "achieved_updates": {"count": 2900},
                "dropped_iterations": {"count": 100},
                "update_failures": {"passes": 0, "fails": 2900},
            },
        }

        quality = load_quality(
            summary, update_rate=1000, duration="3s"
        )

        self.assertEqual(quality["target_updates"], 3000)
        self.assertEqual(quality["actual_writes"], 2900)
        self.assertEqual(quality["dropped_arrivals"], 100)
        self.assertFalse(quality["target_met"])
        self.assertTrue(quality["unequal_pressure"])

    def test_write_quality_rejects_unreconciled_success_counter(self):
        with self.assertRaisesRegex(ConfigError, "write counter"):
            load_quality(
                {"metrics": {
                    "achieved_updates": {"count": 0},
                    "update_failures": {"passes": 0, "fails": 10},
                }},
                update_rate=1000, duration="3s",
            )

    def test_write_quality_requires_actual_update_attempts(self):
        with self.assertRaisesRegex(ConfigError, "did not exercise"):
            load_quality(
                {"metrics": {
                    "update_failures": {"passes": 0, "fails": 0},
                }},
                update_rate=1000, duration="3s",
            )

    def test_plan_audit_parses_every_query_and_classifies_empty_guard(self):
        indexed = json.dumps([{
            "Plan": {"Node Type": "Index Scan"}
        }])
        cheap_empty = json.dumps([{
            "Plan": {
                "Node Type": "Limit",
                "Plans": [
                    {"Node Type": "Result", "One-Time Filter": "false"},
                    {"Node Type": "CTE Scan"},
                ],
            }
        }])
        output = "\n".join([
            "TIN_AUDIT_BEGIN:0",
            indexed,
            "TIN_AUDIT_END:0",
            "TIN_AUDIT_BEGIN:1",
            cheap_empty,
            "TIN_AUDIT_END:1",
        ])

        result = parse_plan_audit(
            output,
            queries=[
                {"id": "q1", "pg_tsquery": {"conjunction": "'alpha'"}},
                {"id": "q2", "pg_tsquery": {"conjunction": ""}},
            ],
            engine="pg_textsearch",
            operation="search",
            kind="conjunction",
            shape="rank_first",
        )

        self.assertEqual(result["audited_queries"], 2)
        self.assertEqual(result["indexed_queries"], 1)
        self.assertEqual(result["empty_queries"], 1)
        self.assertTrue(result["timing_allowed"])

    def test_empty_query_audit_rejects_a_document_scan(self):
        plan = json.dumps([{
            "Plan": {
                "Node Type": "Result",
                "Plans": [{"Node Type": "Seq Scan"}],
            }
        }])
        result = parse_plan_audit(
            "TIN_AUDIT_BEGIN:0\n" + plan + "\nTIN_AUDIT_END:0\n",
            queries=[{"id": "empty", "pg_tsquery": {"phrase": ""}}],
            engine="pg_textsearch", operation="search", kind="phrase",
            shape="filter_first",
        )
        self.assertFalse(result["timing_allowed"])
        self.assertEqual(
            result["records"][0]["classification"], "non_indexed_empty"
        )

    def test_plan_audit_accepts_psql_prefixed_notice_before_empty_json(self):
        plan = json.dumps([{
            "Plan": {
                "Node Type": "Result",
                "One-Time Filter": "false",
            },
        }])
        output = "\n".join([
            "TIN_AUDIT_BEGIN:0",
            (
                "psql:/tin-plan-audit-6b598c268fe4881f.sql:5: NOTICE:  "
                "text-search query doesn't contain lexemes: \"\""
            ),
            (
                "LINE 1: ... SETTINGS, FORMAT JSON) EXECUTE "
                "tin_probe('in a', ''::tsquer..."
            ),
            "                                                             ^",
            plan,
            "TIN_AUDIT_END:0",
        ])

        result = parse_plan_audit(
            output,
            queries=[{
                "id": "stopwords",
                "pg_tsquery": {"phrase": ""},
            }],
            engine="pg_textsearch",
            operation="search",
            kind="phrase",
            shape="filter_first",
        )

        self.assertTrue(result["timing_allowed"])
        self.assertEqual(
            result["records"][0]["classification"], "empty_query"
        )

    def test_plan_audit_rejects_sql_error_after_notice(self):
        output = "\n".join([
            "TIN_AUDIT_BEGIN:0",
            "NOTICE:  text-search query doesn't contain lexemes",
            "LINE 1: EXPLAIN EXECUTE audit_0('');",
            "                                  ^",
            "ERROR:  relation \"documents\" does not exist",
            "TIN_AUDIT_END:0",
        ])

        with self.assertRaisesRegex(ConfigError, "SQL failed"):
            parse_plan_audit(
                output,
                queries=[{
                    "id": "broken",
                    "pg_tsquery": {"phrase": ""},
                }],
                engine="pg_textsearch",
                operation="search",
                kind="phrase",
                shape="filter_first",
            )

    def test_plan_audit_preserves_timeout_after_notice(self):
        output = "\n".join([
            "TIN_AUDIT_BEGIN:0",
            "NOTICE:  text-search query doesn't contain lexemes",
            "LINE 1: EXPLAIN EXECUTE audit_0('');",
            "                                  ^",
            "ERROR:  canceling statement due to statement timeout",
            "TIN_AUDIT_END:0",
        ])

        result = parse_plan_audit(
            output,
            queries=[{
                "id": "timeout",
                "pg_tsquery": {"phrase": ""},
            }],
            engine="pg_textsearch",
            operation="search",
            kind="phrase",
            shape="filter_first",
        )

        self.assertFalse(result["timing_allowed"])
        self.assertEqual(
            result["records"][0]["classification"], "timeout"
        )
        self.assertTrue(result["records"][0]["censored"])

    def test_plan_audit_rejects_notice_without_valid_json(self):
        for suffix in ("", "[not-json"):
            output = "\n".join([
                "TIN_AUDIT_BEGIN:0",
                "NOTICE:  text-search query doesn't contain lexemes",
                "LINE 1: EXPLAIN EXECUTE audit_0('');",
                "                                  ^",
                suffix,
                "TIN_AUDIT_END:0",
            ])
            with self.subTest(suffix=suffix):
                with self.assertRaisesRegex(
                    ConfigError, "valid JSON plan"
                ):
                    parse_plan_audit(
                        output,
                        queries=[{
                            "id": "invalid",
                            "pg_tsquery": {"phrase": ""},
                        }],
                        engine="pg_textsearch",
                        operation="search",
                        kind="phrase",
                        shape="filter_first",
                    )

    def test_plan_audit_rejects_unrecognized_preamble(self):
        plan = json.dumps([{"Plan": {"Node Type": "Result"}}])
        output = "\n".join([
            "TIN_AUDIT_BEGIN:0",
            "unexpected output",
            plan,
            "TIN_AUDIT_END:0",
        ])

        with self.assertRaisesRegex(ConfigError, "diagnostic preamble"):
            parse_plan_audit(
                output,
                queries=[{
                    "id": "invalid",
                    "pg_tsquery": {"phrase": ""},
                }],
                engine="pg_textsearch",
                operation="search",
                kind="phrase",
                shape="filter_first",
            )

    def test_native_plan_audit_batches_one_session_and_resumes(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-plan-audit",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            query_pack = root / "queries.json"
            queries = [
                {
                    "id": "q1",
                    "text": "alpha",
                    "pg_tsquery": {"conjunction": "'alpha'"},
                },
                {
                    "id": "q2",
                    "text": "the",
                    "pg_tsquery": {"conjunction": ""},
                },
            ]
            query_pack.write_text(json.dumps(queries))
            indexed = json.dumps([{
                "Plan": {"Node Type": "Index Scan"}
            }])
            empty = json.dumps([{
                "Plan": {"Node Type": "Result"}
            }])
            output_text = "\n".join([
                "TIN_AUDIT_BEGIN:0", indexed, "TIN_AUDIT_END:0",
                "TIN_AUDIT_BEGIN:1", empty, "TIN_AUDIT_END:1",
            ])
            commands = []

            def command(args, *, output=None, **_kwargs):
                commands.append(args)
                if output is not None:
                    output.write_text(output_text)

            arguments = {
                "container": "database",
                "phase_dir": root / "phase",
                "phase": "high.pgts.english",
                "profile": "high",
                "analyzer": "english-no-stopwords",
                "pack": "natural",
                "operation": "search",
                "kind": "conjunction",
                "shape": "rank_first",
                "statement": "SELECT id FROM documents WHERE body @@ $2",
                "query_pack": query_pack,
            }
            with patch.object(runner, "command", side_effect=command):
                first = runner._audit_native_queries(**arguments)
                second = runner._audit_native_queries(**arguments)

            psql = [
                args for args in commands
                if "psql" in args
            ]
            self.assertEqual(len(psql), 1)
            pgoptions = psql[0][psql[0].index("--env") + 1]
            self.assertIn("-c random_page_cost=1.1", pgoptions)
            self.assertEqual(first, second)
            self.assertEqual(first["audited_queries"], 2)

    def test_write_retry_preserves_retained_and_archived_volumes(self):
        for location, flag in (
            ("results", "retained"),
            ("fleet/forensics", "retained"),
            ("fleet/forensics", "retention_requested"),
        ):
            with (
                self.subTest(location=location, flag=flag),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                runner = TinRunner(
                    Path(__file__).resolve().parents[3],
                    root, True, "write-retention", "msmarco-v2", None,
                )
                volume = runner._volume_name(
                    "write", "pg_textsearch",
                    "english-high-035d20cc5cda",
                )
                labels = {
                    "kind": "write-attempt",
                    "engine": "pg_textsearch",
                    "analyzer": "english",
                    "profile": "high",
                    "cell": "cell-id",
                }
                record = (
                    runner.tin_dir / location / "old-attempt"
                    / "retention.json"
                )
                record.parent.mkdir(parents=True)
                record.write_text(json.dumps({
                    flag: True, "volume": volume, "labels": labels,
                    "reason": "write-cleanup-failed",
                }))
                volumes = {volume}

                def docker(args, **_kwargs):
                    if args[:3] == ["docker", "volume", "inspect"]:
                        return subprocess.CompletedProcess(
                            args, 0, stdout=json.dumps([{
                                "Labels": {
                                    "managed-by": "pg_textsearch-tin",
                                    "run-id": runner.run_id,
                                } | labels,
                            }]),
                        )
                    return subprocess.CompletedProcess(args, 0, stdout="")

                def command(args, **_kwargs):
                    if args[:3] == ["docker", "volume", "rm"]:
                        volumes.remove(args[-1])

                with (
                    patch(
                        "benchmarks.scale.tin.subprocess.run",
                        side_effect=docker,
                    ),
                    patch.object(runner, "command", side_effect=command),
                    patch.object(runner, "_clone_volume"),
                ):
                    arguments = dict(
                        engine="pg_textsearch",
                        analyzer="english",
                        profile="high",
                        cell_id="cell-id",
                        source_volume="source",
                        image="sha256:pinned",
                        result_dir=runner.tin_dir / "results/current",
                    )
                    with self.assertRaisesRegex(ConfigError, "retained"):
                        runner._prepare_write_volume(**arguments)
                    self.assertEqual(volumes, {volume})
                    self.assertTrue(record.is_file())
                    if flag == "retained":
                        with self.assertRaisesRegex(
                            ConfigError, "retained"
                        ):
                            runner._remove_owned_write_volume(
                                volume, labels, completed=True
                            )
                    record.unlink()
                    runner._prepare_write_volume(**arguments)
                    self.assertEqual(volumes, set())

    def test_write_attempt_volume_is_recreated_from_analyzer_source(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-write-clone",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            result_dir = root / "result"
            result_dir.mkdir()
            calls = []

            with (
                patch.object(
                    runner,
                    "_remove_owned_write_volume",
                    side_effect=lambda *args: calls.append(
                        ("remove", args)
                    ),
                ),
                patch.object(
                    runner,
                    "_clone_volume",
                    side_effect=lambda *args, **kwargs: calls.append(
                        ("clone", args, kwargs)
                    ),
                ),
                patch(
                    "benchmarks.scale.tin.time.time_ns",
                    return_value=123,
                ),
            ):
                volume, attempt = runner._prepare_write_volume(
                    engine="pg_textsearch",
                    analyzer="english-no-stopwords",
                    profile="high",
                    cell_id="cell-id",
                    source_volume="analyzer-source",
                    image="sha256:pinned",
                    result_dir=result_dir,
                )

            self.assertIn("write", volume)
            self.assertEqual(attempt.name, "123")
            self.assertEqual(calls[0][0], "remove")
            self.assertEqual(calls[1][0], "clone")
            self.assertEqual(calls[1][1][0], "analyzer-source")
            self.assertEqual(
                calls[1][2]["source_labels"]["kind"], "analyzer"
            )

    def test_query_enrichment_is_checkpointed_per_analyzer_and_pack(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            dataset = root / "dataset"
            (dataset / "k6").mkdir(parents=True)
            source = [
                {"id": "q1", "text": "alpha the beta", "span": 3},
            ]
            (dataset / "k6/natural.json").write_text(json.dumps(source))
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root / "artifacts",
                smoke=True,
                run_id="tin-enrich",
                dataset_kind="msmarco-v2",
                dataset_path=dataset,
            )
            calls = []

            def enrich(container, text_config, queries):
                calls.append((container, text_config, queries))
                return [
                    query | {"pg_tsquery": {
                        "disjunction": "'alpha' | 'beta'",
                        "conjunction": "'alpha' & 'beta'",
                        "phrase": "'alpha' <2> 'beta'",
                    }}
                    for query in queries
                ]

            output = runner.enrich_query_pack(
                "native-db",
                "english-no-stopwords",
                "natural",
                enrich_queries=enrich,
            )
            resumed = runner.enrich_query_pack(
                "native-db",
                "english-no-stopwords",
                "natural",
                enrich_queries=enrich,
            )

            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][1], "public.tin_english_no_stopwords")
            self.assertEqual(json.loads(output.read_text())[0]["span"], 3)
            self.assertEqual(resumed, output)
            metadata = json.loads(
                output.with_suffix(".fingerprint.json").read_text()
            )
            self.assertEqual(metadata["analyzer"], "english-no-stopwords")
            self.assertEqual(metadata["pack"], "natural")

    def test_prepared_corpus_loader_omits_engine_index_build(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            dataset = root / "dataset"
            (dataset / "data").mkdir(parents=True)
            (dataset / "schema.yaml").write_text("schema")
            (dataset / "pg_textsearch").mkdir()
            (dataset / "pg_textsearch/pre.sql").write_text("CREATE EXTENSION")
            (dataset / "pg_textsearch/post.sql").write_text("CREATE INDEX")
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root / "artifacts",
                smoke=True,
                run_id="tin-prepared",
                dataset_kind="msmarco-v2",
                dataset_path=dataset,
            )

            prepared = runner.prepared_dataset("pg_textsearch")

            self.assertTrue((prepared / "data").is_symlink())
            self.assertEqual(
                (prepared / "pg_textsearch/pre.sql").read_text(),
                "CREATE EXTENSION",
            )
            self.assertEqual(
                (prepared / "pg_textsearch/post.sql").read_text(),
                "",
            )

    def test_analyzer_build_records_vacuum_timing_sizes_and_actual_resources(
        self,
    ):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-build-metadata",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            base_dir = (
                runner.tin_dir / "build/paradedb/prepared-corpus"
            )
            base_dir.mkdir(parents=True)
            (base_dir / "complete.json").write_text(
                json.dumps({"base": "fingerprint"})
            )
            commands = []

            def command(args, **_kwargs):
                commands.append(args)
                output = _kwargs.get("output")
                if output and output.name == "rowcount-and-sizes.tsv":
                    output.write_text("10000\n1024\t4096\n")

            runner.dataset.mkdir(parents=True)
            (runner.dataset / "manifest.json").write_text(
                json.dumps({"rows": 10000})
            )

            with (
                patch.object(
                    runner,
                    "_images",
                    return_value=(
                        {"paradedb": "paradedb@sha256:pinned"},
                        {},
                    ),
                ),
                patch.object(
                    runner, "build_base", return_value="base-volume"
                ),
                patch.object(runner, "_clone_volume"),
                patch.object(runner, "_start_database"),
                patch.object(runner, "_stop_database"),
                patch.object(runner, "command", side_effect=command),
                patch(
                    "benchmarks.scale.tin.time.time",
                    side_effect=[0, 1, 4, 5, 7, 9],
                ),
            ):
                runner.build_analyzer(
                    "paradedb", "english-no-stopwords"
                )

            sql_commands = [
                args[-1] for args in commands if "-c" in args
            ]
            self.assertIn("VACUUM (ANALYZE) documents", sql_commands)
            self.assertTrue(any(
                "SELECT count(*) FROM documents" in sql
                for sql in sql_commands
            ))
            metadata = json.loads(
                (
                    runner.tin_dir
                    / "build/paradedb/english-no-stopwords/build.json"
                ).read_text()
            )
            self.assertEqual(metadata["index_seconds"], 3)
            self.assertEqual(metadata["vacuum_analyze_seconds"], 2)
            self.assertEqual(
                metadata["resources"]["maintenance_work_mem"], "8GB"
            )

    def test_analyzer_resume_revalidates_current_database_proof(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            dataset = root / "dataset"
            dataset.mkdir()
            (dataset / "manifest.json").write_text(json.dumps({"rows": 10}))
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root / "artifacts",
                smoke=True,
                run_id="tin-proof-resume",
                dataset_kind="msmarco-v2",
                dataset_path=dataset,
            )
            base_dir = (
                runner.tin_dir / "build/paradedb/prepared-corpus"
            )
            base_dir.mkdir(parents=True)
            base = {"base": "fingerprint"}
            (base_dir / "complete.json").write_text(json.dumps(base))
            build_dir = (
                runner.tin_dir
                / "build/paradedb/english-no-stopwords"
            )
            build_dir.mkdir(parents=True)
            sql = analyzer_sql("paradedb", "english-no-stopwords")
            fingerprint = {
                "format": 1,
                "engine": "paradedb",
                "analyzer": ANALYZERS["english-no-stopwords"],
                "image": "paradedb@sha256:pinned",
                "sql_sha256": hashlib.sha256(sql.encode()).hexdigest(),
                "build_resources": BUILD_RESOURCES,
                "base": base,
                "database_proof_protocol": "rowcount-sizes-v1",
            }
            (build_dir / "complete.json").write_text(json.dumps(fingerprint))
            (build_dir / "rowcount-and-sizes.tsv").write_text(
                "10\n100\t200\n"
            )
            (build_dir / "build.json").write_text(json.dumps({
                "database": {
                    "rows": 9,
                    "index_bytes": 100,
                    "table_total_bytes": 200,
                },
            }))

            with (
                patch.object(
                    runner,
                    "_images",
                    return_value=(
                        {"paradedb": "paradedb@sha256:pinned"},
                        {},
                    ),
                ),
                patch.object(
                    runner, "build_base", return_value="base-volume"
                ),
                patch.object(runner, "_clone_volume"),
            ):
                with self.assertRaisesRegex(
                    ConfigError, "database proof"
                ):
                    runner.build_analyzer(
                        "paradedb", "english-no-stopwords"
                    )

    def test_run_cells_resume_rebuilds_quality_and_cleans_write_clone(self):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            dataset = root / "dataset"
            (dataset / "k6").mkdir(parents=True)
            (dataset / "k6/natural.json").write_text(json.dumps([{
                "id": "q1",
                "text": "alpha",
                "pg_tsquery": {
                    "disjunction": "'alpha'",
                    "conjunction": "'alpha'",
                    "phrase": "'alpha'",
                },
            }]))
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root / "artifacts",
                smoke=True,
                run_id="tin-write-resume",
                dataset_kind="msmarco-v2",
                dataset_path=dataset,
            )
            cell = next(
                cell for cell in build_tin_plan(
                    smoke=True,
                    accept_terms=False,
                    dataset_kind="msmarco-v2",
                )["cells"]
                if (
                    cell["profile"] == "high"
                    and cell["analyzer"] == "english-no-stopwords"
                    and cell["pack"] == "natural"
                    and cell["operation"] == "concurrent-writes"
                    and cell["query_family"] == "disjunction"
                )
            )
            query_pack = (
                runner.tin_dir
                / "query-packs/english-no-stopwords/natural.json"
            )
            query_pack.parent.mkdir(parents=True)
            query_pack.write_text(
                (dataset / "k6/natural.json").read_text()
            )
            build_dir = (
                runner.tin_dir
                / "build/paradedb/english-no-stopwords"
            )
            build_dir.mkdir(parents=True)
            (build_dir / "complete.json").write_text(
                json.dumps({"build": "fingerprint"})
            )
            phase_dir = (
                runner.tin_dir
                / "phases/high/paradedb/english-no-stopwords"
            )
            result_dir = (
                runner.tin_dir / "results/paradedb" / cell["cell_id"]
            )
            result_dir.mkdir(parents=True)
            image = "paradedb@sha256:pinned"
            source = runner._volume_name(
                "analyzer", "paradedb", "english-no-stopwords"
            )
            cell_hash = hashlib.sha256(
                cell["cell_id"].encode()
            ).hexdigest()[:12]
            target = runner._volume_name(
                "write",
                "paradedb",
                f"english-no-stopwords-high-{cell_hash}",
            )
            labels = {
                "kind": "write-attempt",
                "engine": "paradedb",
                "analyzer": "english-no-stopwords",
                "profile": "high",
                "cell": cell["cell_id"],
            }
            retention = (
                result_dir / "write-attempts/123/retention.json"
            )
            retention.parent.mkdir(parents=True)
            retention.write_text(json.dumps({
                "retention_requested": True,
                "volume": target,
                "labels": labels,
                "reason": "write-cleanup-failed",
            }))
            removed_volumes = []
            fingerprint = {
                "base": "fingerprint",
                "phase_resources": PROFILES["high"],
                "write_clone": {
                    "source": source,
                    "target": target,
                    "labels": labels,
                    "source_labels": {
                        "kind": "analyzer",
                        "engine": "paradedb",
                        "analyzer": "english-no-stopwords",
                    },
                    "image": image,
                },
                "cache_reset_hook_sha256": "hooksha",
                "plan_audit_protocol": "all-query-custom-plan-v1",
                "plan_audits": [],
            }
            (result_dir / "complete.json").write_text(
                json.dumps(fingerprint)
            )
            (result_dir / "k6-summary.json").write_text(json.dumps({
                "metrics": {
                    "update_failures": {"passes": 0, "fails": 3000},
                    "achieved_updates": {"count": 3000},
                    "dropped_iterations": {"count": 0},
                },
            }))

            def clone(
                _source, _target, _labels, checkpoint, **_kwargs
            ):
                checkpoint.parent.mkdir(parents=True, exist_ok=True)
                checkpoint.write_text("{}")

            def command(_args, *, output=None, **_kwargs):
                if output is not None:
                    output.write_text("")
                if _args[:3] == ["docker", "volume", "rm"]:
                    removed_volumes.append(_args[-1])

            def docker(args, **_kwargs):
                if args[:3] == ["docker", "volume", "inspect"]:
                    return subprocess.CompletedProcess(
                        args, 0, stdout=json.dumps([{
                            "Labels": {
                                "managed-by": "pg_textsearch-tin",
                                "run-id": runner.run_id,
                            } | labels,
                        }]),
                    )
                return subprocess.CompletedProcess(args, 0, stdout="")

            with (
                patch.object(
                    runner,
                    "_images",
                    return_value=({"paradedb": image}, {}),
                ),
                patch.object(runner, "_clone_volume", side_effect=clone),
                patch.object(
                    runner,
                    "require_cache_reset",
                    return_value={"hook_sha256": "hooksha"},
                ),
                patch.object(
                    runner,
                    "cache_reset_hook_sha256",
                    return_value="hooksha",
                ),
                patch.object(runner, "_start_database"),
                patch.object(runner, "_stop_database"),
                patch.object(runner, "command", side_effect=command),
                patch.object(
                    runner,
                    "cell_fingerprint",
                    return_value={"base": "fingerprint"},
                ),
                patch.object(runner, "_probe_cell"),
                patch(
                    "benchmarks.scale.tin.subprocess.run",
                    side_effect=docker,
                ),
            ):
                runner.run_cells(["paradedb"], [cell])

            quality = json.loads(
                (result_dir / "load-quality.json").read_text()
            )
            self.assertTrue(quality["target_met"])
            summary = json.loads(
                (runner.tin_dir / "summary.json").read_text()
            )
            self.assertEqual(summary["load_quality"], [quality])
            self.assertEqual(removed_volumes, [target])

    def test_completed_write_quality_requires_metrics_and_matches_existing(
        self,
    ):
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-",
            dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            runner = TinRunner(
                Path(__file__).resolve().parents[3],
                root,
                smoke=True,
                run_id="tin-quality-resume",
                dataset_kind="msmarco-v2",
                dataset_path=None,
            )
            cell = {
                "cell_id": "write-cell",
                "update_rate": 1000,
            }
            with self.assertRaisesRegex(ConfigError, "missing k6 metrics"):
                runner._record_write_quality(
                    result_dir=root,
                    cell=cell,
                    engine="paradedb",
                    duration="3s",
                    sql_status="ok",
                )
            (root / "k6-summary.json").write_text(json.dumps({
                "metrics": {
                    "update_failures": {"passes": 0, "fails": 3000},
                    "achieved_updates": {"count": 3000},
                    "dropped_iterations": {"count": 0},
                },
            }))
            (root / "load-quality.json").write_text(json.dumps({
                "target_met": False,
            }))

            with self.assertRaisesRegex(
                ConfigError, "incompatible resume metadata"
            ):
                runner._record_write_quality(
                    result_dir=root,
                    cell=cell,
                    engine="paradedb",
                    duration="3s",
                    sql_status="ok",
                )


if __name__ == "__main__":
    unittest.main()
