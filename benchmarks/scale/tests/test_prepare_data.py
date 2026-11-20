import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

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
