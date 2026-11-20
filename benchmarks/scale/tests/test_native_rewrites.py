import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from benchmarks.scale.native_rewrites import (
    enrich_queries,
    query_sql,
    RewriteError,
)


class QueryPreparationTests(unittest.TestCase):
    def test_partial_server_results_cannot_drop_queries_silently(self):
        response = subprocess.CompletedProcess(
            [], 0, stdout=json.dumps({
                "ordinal": 0, "disjunction": "'alpha'",
                "conjunction": "'alpha'", "phrase": "'alpha'",
            }), stderr="",
        )
        with patch(
            "benchmarks.scale.native_rewrites.subprocess.run",
            return_value=response,
        ):
            with self.assertRaisesRegex(RewriteError, "incomplete"):
                enrich_queries("fixture", "english", [
                    {"id": "a", "text": "alpha"},
                    {"id": "b", "text": "beta"},
                ])


@unittest.skipUnless(
    os.environ.get("PGTS_REWRITE_TEST_CONTAINER"),
    "requires the isolated native rewrite validation container",
)
class NativeRewriteIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.container = os.environ["PGTS_REWRITE_TEST_CONTAINER"]
        info = json.loads(subprocess.check_output(
            ["docker", "inspect", cls.container], text=True,
        ))[0]
        if info["Config"]["Labels"].get("purpose") != (
            "native-rewrite-validation"
        ):
            raise RuntimeError("refusing a non-fixture database")
        cls.config = os.environ.get(
            "PGTS_REWRITE_TEST_CONFIG", "pg_catalog.english"
        )
        cls.queries = enrich_queries(cls.container, cls.config, [
            {"id": "adjacent", "text": "alpha beta"},
            {"id": "gap", "text": "alpha the beta"},
            {"id": "missing", "text": "absentword"},
            {"id": "empty", "text": "?!..."},
            {"id": "stopwords", "text": "the is"},
            {"id": "escaped", "text": 'alpha "the" beta\'s \\ delta\n\\.\n世界'},
        ])
        output = os.environ.get("PGTS_REWRITE_PLAN_OUTPUT")
        cls.plan_output = Path(output) if output else None
        if cls.plan_output:
            cls.plan_output.mkdir(parents=True, exist_ok=True)

    @classmethod
    def sql(cls, statement):
        return subprocess.check_output(
            [
                "docker", "exec", cls.container, "psql", "-U", "postgres",
                "-XqAt", "-v", "ON_ERROR_STOP=1", "-c",
                "SET client_min_messages=warning; "
                "SET default_text_search_config="
                + cls.literal(cls.config) + "; "
                + statement,
            ],
            text=True,
        ).splitlines()

    @staticmethod
    def literal(text):
        return "'" + text.replace("'", "''") + "'"

    def execute(self, query, kind, operation, shape, limit=1, explain=False,
                generic=False, cache=True):
        statement = query_sql(kind, operation, shape, limit)
        args = (
            [query["pg_tsquery"][kind]] if operation == "count"
            else [query["text"]] if kind == "disjunction"
            else [query["text"], query["pg_tsquery"][kind]]
        )
        prefix = (
            "SET plan_cache_mode="
            + ("force_generic_plan" if generic else "force_custom_plan")
            + "; SET pg_textsearch.memtable_cache_enabled="
            + ("on" if cache else "off")
            + "; "
        )
        sql = (
            prefix + "PREPARE checked AS " + statement + "; "
            + ("EXPLAIN (FORMAT JSON, COSTS OFF) " if explain else "")
            + "EXECUTE checked("
            + ",".join(self.literal(arg) for arg in args) + ");"
        )
        return self.sql(sql)

    def test_rewrites_equal_native_full_scan_oracle(self):
        for query in self.queries:
            for kind in ("conjunction", "phrase"):
                predicate = self.literal(query["pg_tsquery"][kind])
                text = self.literal(query["text"])
                for limit in (1, 10):
                    expected = self.sql(
                        "SELECT id FROM documents WHERE "
                        f"to_tsvector({self.literal(self.config)},body) @@ "
                        f"{predicate}::tsquery ORDER BY body <@> "
                        f"to_bm25query({text},'documents_bm25') LIMIT {limit}"
                    )
                    for shape in ("rank_first", "filter_first"):
                        for generic in (False, True):
                            for cache in (False, True):
                                with self.subTest(
                                    query=query["id"], kind=kind,
                                    limit=limit, shape=shape,
                                    generic=generic, cache=cache,
                                ):
                                    self.assertEqual(self.execute(
                                        query, kind, "search", shape, limit,
                                        generic=generic, cache=cache,
                                    ), expected)

    def test_typed_counts_equal_postgres_oracle(self):
        for query in self.queries:
            for kind in ("disjunction", "conjunction", "phrase"):
                predicate = self.literal(query["pg_tsquery"][kind])
                expected = self.sql(
                    "SELECT count(*) FROM documents WHERE "
                    f"to_tsvector({self.literal(self.config)},body) @@ "
                    f"{predicate}::tsquery"
                )
                self.assertEqual(self.execute(
                    query, kind, "count", "rank_first",
                ), expected)

    def test_empty_boolean_queries_do_not_open_document_scans(self):
        query = next(query for query in self.queries if query["id"] == "empty")

        def scan_nodes(plan):
            found = []
            if "Scan" in plan["Node Type"]:
                found.append(plan["Node Type"])
            for child in plan.get("Plans", []):
                found.extend(scan_nodes(child))
            return found

        for kind in ("disjunction", "conjunction", "phrase"):
            cases = [("count", "filter_first")]
            if kind != "disjunction":
                cases += [
                    ("search", "rank_first"),
                    ("search", "filter_first"),
                ]
            for operation, shape in cases:
                with self.subTest(kind=kind, operation=operation, shape=shape):
                    plan = json.loads("\n".join(self.execute(
                        query, kind, operation, shape, explain=True,
                    )))
                    scans = [
                        node for node in scan_nodes(plan[0]["Plan"])
                        if node != "CTE Scan"
                    ]
                    self.assertEqual(scans, [])

    def test_rank_first_is_indexed_and_grows_past_first_1000(self):
        query = self.queries[0]
        for kind in ("conjunction", "phrase"):
            for shape in ("original", "rank_first", "filter_first"):
                plan = json.loads("\n".join(self.execute(
                    query, kind, "search", shape, explain=True,
                )))
                if self.plan_output:
                    (self.plan_output / f"{kind}-{shape}.json").write_text(
                        json.dumps(plan, indent=2) + "\n"
                    )
                encoded = json.dumps(plan)
                self.assertEqual(
                    '"Index Scan"' in encoded, shape != "original", encoded,
                )
        statement = query_sql("conjunction", "search", "rank_first", 1)
        statement = statement.replace(
            "$1", self.literal(query["text"])
        ).replace("$2", self.literal(query["pg_tsquery"]["conjunction"]))
        output = self.sql(
            "SELECT bm25_debug_scoring_passes(true); "
            + statement
            + "; SELECT bm25_debug_scoring_passes(true);"
        )
        self.assertEqual(output[1], "2001")
        self.assertGreaterEqual(int(output[2]), 3)


if __name__ == "__main__":
    unittest.main()
