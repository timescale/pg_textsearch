import json
from pathlib import Path
import tempfile
import unittest

from benchmarks.scale.queries import (
    QueryError,
    build_corpus_span_queries,
    build_span_queries,
    build_workload_matrix,
    load_queries,
    lead_disjunction,
    write_query_pack,
)


ROOT = Path(__file__).resolve().parents[3]
class QueriesTest(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=ROOT / "benchmarks" / "scale"
        )
        self.scratch = Path(self._temporary.name)

    def tearDown(self):
        self._temporary.cleanup()

    def test_query_selection_is_input_order_independent(self):
        first = self.scratch / "first.tsv"
        second = self.scratch / "second.tsv"
        rows = ["9\tred blue\n", "2\tgreen yellow\n", "5\tblack white\n"]
        first.write_text("".join(rows), encoding="utf-8")
        second.write_text("".join(reversed(rows)), encoding="utf-8")

        a = load_queries([first], source="dev", seed=17, limit=2)
        b = load_queries([second], source="dev", seed=17, limit=2)

        self.assertEqual(a, b)

    def test_invalid_tsv_reports_path_and_line_without_query_text(self):
        path = self.scratch / "queries.tsv"
        path.write_text("missing-tab-and-private-text\n", encoding="utf-8")

        with self.assertRaises(QueryError) as raised:
            load_queries([path], source="dev", seed=1)

        message = str(raised.exception)
        self.assertIn("queries.tsv:1", message)
        self.assertNotIn("private-text", message)

    def test_lead_disjunction_rejects_queries_without_terms(self):
        with self.assertRaisesRegex(QueryError, "no searchable terms"):
            lead_disjunction("?!")

    def test_span_queries_have_exact_requested_term_count_and_stable_ids(self):
        natural = [
            {"id": "dev:1", "text": "one two three", "source": "dev"},
            {"id": "dev:2", "text": "four five six", "source": "dev"},
        ]

        spans = build_span_queries(natural, spans=[2, 5], per_span=2, seed=3)

        self.assertEqual([row["id"] for row in spans], [
            "span02-0000",
            "span05-0000",
            "span02-0001",
            "span05-0001",
        ])
        self.assertEqual(
            [len(row["text"].split()) for row in spans], [2, 5, 2, 5]
        )

    def test_corpus_spans_never_cross_document_boundaries(self):
        documents = [
            {"source_id": "a", "body": "alpha beta gamma delta"},
            {"source_id": "b", "body": "one two three four"},
        ]
        spans = build_corpus_span_queries(
            documents, spans=[3], per_span=8, seed=9
        )
        bodies = {
            "a": documents[0]["body"].split(),
            "b": documents[1]["body"].split(),
        }
        for query in spans:
            terms = query["text"].split()
            source = bodies[query["source_doc_id"]]
            offset = query["offset"]
            self.assertEqual(terms, source[offset:offset + 3])

    def test_slow_query_prefix_covers_every_span_length(self):
        body = (
            "one two three four five six seven eight nine ten "
            "eleven twelve thirteen fourteen fifteen sixteen"
        )
        packs = [
            build_span_queries(
                [{"text": body}], per_span=100, seed=3,
            ),
            build_corpus_span_queries(
                [{"source_id": "a", "body": body}],
                per_span=100, seed=3,
            ),
        ]
        for queries in packs:
            with self.subTest(source=queries[0]["source"]):
                for start in (0, 14, 28):
                    self.assertEqual(
                        [
                            query["span"]
                            for query in queries[start:start + 14]
                        ],
                        list(range(2, 16)),
                    )

    def test_matrix_marks_pg_textsearch_boolean_modes_supported(self):
        matrix = build_workload_matrix()
        cells = {
            (cell["engine"], cell["mode"], cell.get("limit")): cell
            for cell in matrix
        }

        self.assertEqual(
            cells[("pg_textsearch", "disjunction", 10)]["support"], "exact"
        )
        for mode in ("phrase", "conjunction", "count"):
            cell = cells[("pg_textsearch", mode, None)]
            self.assertEqual(cell["support"], "exact")

    def test_query_pack_contains_no_score_predicate_template(self):
        natural = [{"id": "dev:1", "text": "alpha beta", "source": "dev"}]
        write_query_pack(
            self.scratch, natural, spans=[2], per_span=1, seed=1
        )

        matrix = (self.scratch / "matrix.json").read_text(encoding="utf-8")
        self.assertNotIn("< 0", matrix)
        self.assertNotIn("score_predicate", matrix)
        self.assertEqual(
            json.loads((self.scratch / "natural.json").read_text())[0]["id"],
            "dev:1",
        )


if __name__ == "__main__":
    unittest.main()
