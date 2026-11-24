import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from benchmarks.scale.prepare_data import main


ROOT = Path(__file__).resolve().parents[3]
class PrepareDataCliTest(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=ROOT / "benchmarks" / "scale"
        )
        self.scratch = Path(self._temporary.name)

    def tearDown(self):
        self._temporary.cleanup()

    def test_queries_command_refuses_to_read_without_terms_acceptance(self):
        query_file = self.scratch / "queries.tsv"
        query_file.write_text("1\tprivate query text\n", encoding="utf-8")
        stderr = io.StringIO()

        with contextlib.redirect_stderr(stderr):
            status = main(
                [
                    "queries",
                    "--query",
                    f"dev={query_file}",
                    "--output",
                    str(self.scratch / "out"),
                ],
                environment={},
            )

        self.assertEqual(status, 2)
        self.assertIn("MS MARCO terms", stderr.getvalue())
        self.assertNotIn("private query text", stderr.getvalue())

    def test_acceptance_command_is_a_download_gate(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            refused = main(["acceptance"], environment={})

        with contextlib.redirect_stdout(io.StringIO()):
            accepted = main(
                ["acceptance"], environment={"MSMARCO_ACCEPT_TERMS": "1"}
            )

        self.assertEqual(refused, 2)
        self.assertEqual(accepted, 0)

    def test_queries_command_writes_natural_and_span_inputs(self):
        query_file = self.scratch / "queries.tsv"
        query_file.write_text(
            "1\tone two three\n2\tfour five six\n", encoding="utf-8"
        )

        with contextlib.redirect_stdout(io.StringIO()):
            status = main(
                [
                    "queries",
                    "--accept-msmarco-terms",
                    "--query",
                    f"dev={query_file}",
                    "--output",
                    str(self.scratch / "out"),
                    "--spans",
                    "2,3",
                    "--per-span",
                    "1",
                ],
                environment={},
            )

        self.assertEqual(status, 0)
        self.assertTrue((self.scratch / "out" / "natural.json").is_file())
        self.assertTrue((self.scratch / "out" / "spans.json").is_file())
        self.assertTrue((self.scratch / "out" / "matrix.json").is_file())

    def corpus_query_args(self):
        query_file = self.scratch / "queries.tsv"
        query_file.write_text("1\tone two three\n", encoding="utf-8")
        return [
            "queries", "--accept-msmarco-terms",
            "--query", f"dev={query_file}",
            "--output", str(self.scratch / "out"),
            "--corpus-parquet", str(self.scratch / "corpus"),
            "--spans", "2", "--per-span", "1",
        ]

    def test_interrupted_corpus_sampling_publishes_no_query_pack(self):
        with (
            patch(
                "benchmarks.scale.prepare_data.sample_parquet_spans",
                side_effect=OSError("interrupted corpus sampling"),
            ),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            status = main(self.corpus_query_args(), environment={})

        self.assertEqual(status, 2)
        for name in ("natural.json", "spans.json", "matrix.json"):
            self.assertFalse((self.scratch / "out" / name).exists())

    def test_corpus_spans_use_atomic_publication_without_token_pool_pack(self):
        corpus = [{
            "id": "corpus-span-2-0", "text": "document words",
            "source": "corpus-span", "source_doc_id": "document-1",
            "span": 2, "offset": 0, "seed": 1,
        }]
        replace = Path.replace

        def interrupt_span_publication(source, destination):
            if Path(destination).name == "spans.json":
                raise OSError("interrupted atomic publication")
            return replace(source, destination)

        with (
            patch(
                "benchmarks.scale.prepare_data.sample_parquet_spans",
                return_value=corpus,
            ),
            patch.object(Path, "replace", interrupt_span_publication),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            status = main(self.corpus_query_args(), environment={})

        self.assertEqual(status, 2)
        self.assertFalse((self.scratch / "out" / "spans.json").exists())
        temporary = self.scratch / "out" / "spans.json.partial"
        self.assertEqual(json.loads(temporary.read_text()), corpus)
        with (
            patch(
                "benchmarks.scale.prepare_data.sample_parquet_spans",
                return_value=corpus,
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            resumed = main(self.corpus_query_args(), environment={})
        self.assertEqual(resumed, 0)
        self.assertEqual(json.loads(
            (self.scratch / "out" / "spans.json").read_text()
        ), corpus)
        self.assertFalse(temporary.exists())

    @unittest.skipUnless(
        importlib.util.find_spec("pyarrow"), "pyarrow is not installed"
    )
    def test_synthetic_command_does_not_require_msmarco_acceptance(self):
        with contextlib.redirect_stdout(io.StringIO()):
            status = main(
                [
                    "synthetic",
                    "--output",
                    str(self.scratch / "out"),
                    "--rows",
                    "10",
                ],
                environment={},
            )

        self.assertEqual(status, 0)
        self.assertTrue((self.scratch / "out" / "manifest.json").is_file())
        self.assertTrue(
            (self.scratch / "out" / "k6" / "natural.json").is_file()
        )


if __name__ == "__main__":
    unittest.main()
