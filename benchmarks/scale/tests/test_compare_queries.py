import json
from pathlib import Path
import tempfile
import unittest

from benchmarks.scale import compare_queries as compare


class ComparisonTests(unittest.TestCase):
    def test_sampling_handles_single_query_and_caps_large_requests(self):
        self.assertEqual(compare.sample_ordinals(1, 16), [0])
        self.assertEqual(compare.sample_ordinals(10, 1), [0])
        self.assertEqual(compare.sample_ordinals(10, 4), [0, 3, 6, 9])
        self.assertEqual(compare.sample_ordinals(3, 20), [0, 1, 2])
        for length, samples in ((0, 2), (2, 0), (2, -1)):
            with self.assertRaises(ValueError):
                compare.sample_ordinals(length, samples)

    def test_parade_sql_binds_text_and_rejects_unsupported_kinds(self):
        sql = compare.parade_statement("disjunction", "search", 10)
        self.assertIn("body ||| %(text)s", sql)
        self.assertIn("ORDER BY paradedb.score(id) DESC LIMIT 10", sql)
        self.assertNotIn("ORDER BY", compare.parade_statement(
            "conjunction", "count", 10
        ))
        for kind, operation, limit in (
            ("phrase", "search", 10), ("conjunction", "delete", 10),
            ("disjunction", "search", True),
        ):
            with self.assertRaises(ValueError):
                compare.parade_statement(kind, operation, limit)

    def records(self, native_rows=None, parade_rows=None, native_ms=10):
        return [
            {
                "type": "metadata", "query_pack_sha256": "pack",
                "ordinals": [0], "kinds": ["disjunction"],
                "operation": "search", "limit": 10, "repeats": 3,
            },
            {
                "type": "query", "engine": "pg_textsearch", "ordinal": 0,
                "query_id": "q", "kind": "disjunction", "operation": "search",
                "status": "ok", "median_ms": native_ms,
                "rows": native_rows if native_rows is not None else [[1], [2]],
            },
            {
                "type": "query", "engine": "paradedb", "ordinal": 0,
                "query_id": "q", "kind": "disjunction", "operation": "search",
                "status": "ok", "median_ms": 5,
                "rows": parade_rows if parade_rows is not None else [[2], [1]],
            },
            {
                "type": "database", "engine": "pg_textsearch",
                "index": [10, 10, 8192, "native definition"],
                "index_options": ["text_config=english"],
                "settings": [["plan_cache_mode", "force_custom_plan"]],
                "extension_version": "native",
            },
            {
                "type": "database", "engine": "paradedb",
                "index": [20, 20, 8192, "parade definition"],
                "index_options": None,
                "settings": [["plan_cache_mode", "force_custom_plan"]],
                "extension_version": "parade",
            },
            {"type": "complete", "completed": 2, "cancelled": 0},
        ]

    def test_analysis_requires_unique_complete_database_provenance(self):
        records = self.records()
        variants = [
            [row for row in records if row["type"] != "database"],
            [row for row in records if row.get("engine") != "paradedb"
             or row["type"] != "database"],
            records[:-1] + [records[3], records[-1]],
            records[:3] + [
                {key: value for key, value in records[3].items()
                 if key != "extension_version"},
            ] + records[4:],
        ]
        for rows in variants:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                compare.analyze_runs(rows, rows)

    def test_analysis_separates_native_rank_changes_from_parade_drift(self):
        report = compare.analyze_runs(
            self.records(),
            self.records(parade_rows=[[1], [2]], native_ms=8),
        )
        self.assertEqual(report["native_rows_changed"], 0)
        self.assertEqual(report["parade_rows_changed"], 1)
        self.assertAlmostEqual(
            report["engines"]["pg_textsearch"]["median_after_before"], .8
        )
        self.assertAlmostEqual(
            report["candidate_native_vs_parade"]["median_native_over_parade"],
            1.6,
        )
        changed = compare.analyze_runs(
            self.records(), self.records(native_rows=[[2], [1]])
        )
        self.assertEqual(changed["native_rows_changed"], 1)
        self.assertEqual(changed["differences"][0]["before"], [[1], [2]])

    def test_analysis_rejects_incomplete_duplicate_or_mismatched_work(self):
        before = self.records()
        variants = [
            before[:-1],
            before[:2] + before[1:],
            [before[0] | {"query_pack_sha256": "different"}] + before[1:],
            [before[0]] + before[2:],
        ]
        for after in variants:
            with self.subTest(after=after), self.assertRaises(ValueError):
                compare.analyze_runs(before, after)

    def test_analysis_rejects_empty_workloads_and_boolean_latencies(self):
        records = self.records()
        empty = [
            records[0] | {"ordinals": []},
            {"type": "complete", "completed": 0, "cancelled": 0},
        ]
        with self.assertRaises(ValueError):
            compare.analyze_runs(empty, empty)
        invalid = self.records()
        invalid[1]["median_ms"] = True
        with self.assertRaises(ValueError):
            compare.analyze_runs(records, invalid)

    def test_cancelled_queries_are_not_latency_samples(self):
        after = self.records()
        after[1] = after[1] | {"status": "cancelled"}
        after[-1] = {"type": "complete", "completed": 1, "cancelled": 1}
        report = compare.analyze_runs(self.records(), after)
        self.assertEqual(report["unsuccessful"], 1)
        self.assertEqual(
            report["engines"]["pg_textsearch"]["paired_successes"], 0
        )

    def test_different_count_results_are_not_comparable_performance_wins(self):
        records = self.records(native_rows=[[10]], parade_rows=[[9]])
        records[0]["operation"] = "count"
        for row in records:
            if row["type"] == "query":
                row["operation"] = "count"
        report = compare.analyze_runs(records, records)
        self.assertEqual(len(report["cross_engine_count_differences"]), 1)
        self.assertEqual(
            report["candidate_native_vs_parade"]["paired_successes"], 0
        )

    def test_analysis_cli_never_overwrites_an_existing_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            before = root / "before.jsonl"
            before.write_text("".join(
                json.dumps(row) + "\n" for row in self.records()
            ))
            report = root / "report.json"
            report.write_text("keep")
            with self.assertRaises(FileExistsError):
                compare.main([
                    "analyze", "--before", str(before), "--after", str(before),
                    "--output", str(report),
                ])
            self.assertEqual(report.read_text(), "keep")
