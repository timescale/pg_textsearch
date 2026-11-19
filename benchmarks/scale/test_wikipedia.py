import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE = Path(__file__).with_name("wikipedia.py")


class WikipediaTests(unittest.TestCase):
    def module(self):
        self.assertTrue(MODULE.exists(), "Wikipedia preparation is missing")
        spec = importlib.util.spec_from_file_location("wikipedia_prepare", MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_conversion_preserves_ids_and_unmodified_article_text(self):
        import pyarrow as pa
        import pyarrow.parquet as pq

        module = self.module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.parquet"
            body = "alpha beta gamma delta epsilon zeta eta theta iota kappa"
            pq.write_table(pa.table({
                "id": ["123", "456"], "title": ["Alpha", "Beta"],
                "url": ["https://en.wikipedia.org/wiki/Alpha",
                        "https://en.wikipedia.org/wiki/Beta"],
                "text": [body, body + " lambda mu nu xi omicron"],
            }), source)
            info, queries = module.convert_shard(
                source, root / "converted.parquet", seed=17,
            )
            table = pq.read_table(root / "converted.parquet").to_pydict()
            self.assertEqual(table["id"], [123, 456])
            self.assertEqual(table["source_id"], ["123", "456"])
            self.assertEqual(table["body"][0], body)
            self.assertEqual(info["rows"], 2)
            self.assertTrue(queries)
            for query in queries:
                self.assertIn(query["text"], table["body"][
                    table["source_id"].index(query["source_doc_id"])].lower())
                self.assertEqual(query["span"], len(query["text"].split()))

    def test_download_rejects_bad_hash_without_publishing_file(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.write_bytes(b"corrupt")
            output = root / "downloaded"
            with self.assertRaises(ValueError):
                module.download(source.as_uri(), output,
                                hashlib.sha256(b"correct").hexdigest())
            self.assertFalse(output.exists())

    def test_query_selection_is_deterministic_and_doc_bounded(self):
        module = self.module()
        body = " ".join(f"word{i}" for i in range(50))
        first = module.article_queries("11", body, 21)
        self.assertEqual(first, module.article_queries("11", body, 21))
        for query in first:
            self.assertIn(query["text"], body)
            self.assertEqual(query["source_doc_id"], "11")
        self.assertNotEqual(first, module.article_queries("11", body, 22))


if __name__ == "__main__":
    unittest.main()
