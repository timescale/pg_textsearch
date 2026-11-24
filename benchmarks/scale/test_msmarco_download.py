import hashlib
import importlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


class DownloadTests(unittest.TestCase):
    def module(self):
        path = Path(__file__).with_name("msmarco_download.py")
        self.assertTrue(
            path.exists(), "MS MARCO asset download is not implemented"
        )
        return importlib.import_module("benchmarks.scale.msmarco_download")

    def test_terms_gate_precedes_filesystem_and_network_access(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "not-created"
            with patch.dict(os.environ, {"MSMARCO_ACCEPT_TERMS": "0"}):
                with self.assertRaisesRegex(module.DatasetError, "terms"):
                    module.download_assets(output)
            self.assertFalse(output.exists())

    def test_download_verifies_bytes_and_records_source_provenance(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.tsv"
            source.write_bytes(b"abc")
            output = Path(directory) / "downloads"
            assets = {"dev1": {
                "filename": "queries.tsv", "url": source.as_uri(),
                "md5": "900150983cd24fb0d6963f7d28e17f72", "bytes": 3,
            }}
            with patch.object(module, "ASSETS", assets):
                paths = module.download_assets(output, accepted=True)
                self.assertEqual(paths["dev1"].read_bytes(), b"abc")
                source.unlink()
                module.download_assets(output, accepted=True)
            record = json.loads((output / "downloads.json").read_text())
            self.assertEqual(record["assets"]["dev1"]["bytes"], 3)
            self.assertEqual(record["assets"]["dev1"]["url"], source.as_uri())
            self.assertTrue(record["terms_acceptance"]["accepted"])

    def test_bad_checksum_never_publishes_a_complete_asset(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.tsv"
            source.write_bytes(b"corrupted")
            output = Path(directory) / "downloads"
            assets = {"dev1": {
                "filename": "queries.tsv", "url": source.as_uri(),
                "md5": hashlib.md5(b"abc").hexdigest(), "bytes": 9,
            }}
            with patch.object(module, "ASSETS", assets):
                with self.assertRaisesRegex(module.DatasetError, "checksum"):
                    module.download_assets(output, accepted=True)
            self.assertFalse((output / "queries.tsv").exists())
            self.assertFalse((output / "downloads.json").exists())

    def test_query_only_download_skips_archive_and_retains_provenance(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.tsv"
            source.write_bytes(b"abc")
            output = root / "downloads"
            output.mkdir()
            prior = {
                "dataset": "msmarco-passage-v2",
                "assets": {"archive": {
                    "md5": hashlib.md5(b"previous archive").hexdigest(),
                }},
            }
            (output / "downloads.json").write_text(json.dumps(prior))
            query = {
                "filename": "queries.tsv", "url": source.as_uri(),
                "md5": hashlib.md5(b"abc").hexdigest(), "bytes": 3,
            }
            assets = {
                "archive": {
                    **query,
                    "filename": "archive.tar",
                    "url": (root / "unavailable-archive").as_uri(),
                },
                "dev1": query,
                "dev2": {**query, "filename": "queries2.tsv"},
            }
            with patch.object(module, "ASSETS", assets):
                paths = module.download_assets(
                    output, accepted=True, names=("dev1", "dev2")
                )
            self.assertEqual(set(paths), {"dev1", "dev2"})
            self.assertTrue(all(
                path.read_bytes() == b"abc" for path in paths.values()
            ))
            self.assertFalse((output / "archive.tar").exists())
            record = json.loads((output / "downloads.json").read_text())
            self.assertEqual(
                record["assets"]["archive"], prior["assets"]["archive"]
            )


if __name__ == "__main__":
    unittest.main()
