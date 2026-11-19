import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest

from benchmarks.scale.dataset import (
    DatasetError,
    iter_archive_members,
    prepare_parquet,
    prepare_synthetic,
    require_terms_acceptance,
    verify_md5,
)


ROOT = Path(__file__).resolve().parents[3]
class DatasetTest(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=ROOT / "benchmarks" / "scale"
        )
        self.scratch = Path(self._temporary.name)

    def tearDown(self):
        self._temporary.cleanup()

    def test_terms_acceptance_requires_explicit_flag_or_environment(self):
        with self.assertRaisesRegex(DatasetError, "MS MARCO terms"):
            require_terms_acceptance(False, {})

        require_terms_acceptance(True, {})
        require_terms_acceptance(False, {"MSMARCO_ACCEPT_TERMS": "1"})

    def test_md5_mismatch_reports_expected_and_actual(self):
        path = self.scratch / "archive.tar"
        path.write_bytes(b"not-the-corpus")
        actual = hashlib.md5(path.read_bytes()).hexdigest()

        with self.assertRaisesRegex(
            DatasetError, rf"expected 0{{32}}.*actual {actual}"
        ):
            verify_md5(path, "0" * 32)

    def test_archive_rows_preserve_source_id_body_and_global_numeric_id(self):
        archive = self.scratch / "passages.tar"
        self._write_archive(
            archive,
            {
                "part-00.jsonl.gz": [
                    {"pid": "msmarco_passage_00_1", "passage": "alpha\tbeta"},
                    {"pid": "msmarco_passage_00_2", "passage": "second\nline"},
                ],
                "part-01.jsonl.gz": [
                    {"pid": "msmarco_passage_01_1", "passage": "third"}
                ],
            },
        )

        members = [
            (member.name, list(member.rows))
            for member in iter_archive_members(archive)
        ]

        self.assertEqual([member[0] for member in members], [
            "part-00.jsonl.gz",
            "part-01.jsonl.gz",
        ])
        self.assertEqual(
            members[0][1],
            [
                (1, "msmarco_passage_00_1", "alpha\tbeta"),
                (2, "msmarco_passage_00_2", "second\nline"),
            ],
        )
        self.assertEqual(
            members[1][1],
            [(3, "msmarco_passage_01_1", "third")],
        )

    def test_malformed_row_reports_member_and_line_without_row_text(self):
        archive = self.scratch / "passages.tar"
        self._write_raw_archive(
            archive,
            {"part-00.jsonl.gz": b'{"pid":"safe","passage":"ok"}\nnot-json\n'},
        )

        with self.assertRaises(DatasetError) as raised:
            for member in iter_archive_members(archive):
                list(member.rows)

        message = str(raised.exception)
        self.assertIn("part-00.jsonl.gz:2", message)
        self.assertNotIn("not-json", message)

    def test_completed_members_can_be_skipped_without_decoding_on_resume(self):
        archive = self.scratch / "passages.tar"
        self._write_raw_archive(
            archive,
            {
                "part-00.jsonl.gz": b"not-json\nnot-json\n",
                "part-01.jsonl.gz": b'{"pid":"p3","passage":"third"}\n',
            },
        )

        members = [
            (member.name, list(member.rows))
            for member in iter_archive_members(
                archive, skip_members={"part-00.jsonl.gz": 2}
            )
        ]

        self.assertEqual(members[0][1], [])
        self.assertEqual(members[1][1], [(3, "p3", "third")])

    def test_parquet_preparation_checkpoints_members_and_resumes(self):
        archive = self.scratch / "passages.tar"
        self._write_archive(
            archive,
            {
                "part-00.jsonl.gz": [{"pid": "p1", "passage": "alpha"}],
                "part-01.jsonl.gz": [{"pid": "p2", "passage": "beta"}],
            },
        )
        expected_md5 = hashlib.md5(archive.read_bytes()).hexdigest()
        calls = []

        class FakeWriter:
            def __init__(self, path, fail=False):
                self.path = path
                self.fail = fail
                self.rows = []

            def write_rows(self, rows):
                if self.fail:
                    raise RuntimeError("interrupted")
                self.rows.extend(rows)

            def close(self):
                self.path.write_text(
                    json.dumps(self.rows), encoding="utf-8"
                )

        def interrupted_factory(path):
            calls.append(path.name)
            return FakeWriter(path, fail=len(calls) == 2)

        with self.assertRaisesRegex(RuntimeError, "interrupted"):
            prepare_parquet(
                archive,
                self.scratch / "prepared",
                expected_md5=expected_md5,
                expected_rows=2,
                writer_factory=interrupted_factory,
                batch_rows=1,
            )

        manifest_path = self.scratch / "prepared" / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        self.assertEqual(len(manifest["members"]), 1)

        resumed_calls = []

        def resumed_factory(path):
            resumed_calls.append(path.name)
            return FakeWriter(path)

        result = prepare_parquet(
            archive,
            self.scratch / "prepared",
            expected_md5=expected_md5,
            expected_rows=2,
            writer_factory=resumed_factory,
            batch_rows=1,
        )

        self.assertEqual(resumed_calls, ["part-00001.parquet.partial"])
        self.assertEqual(result["rows"], 2)
        self.assertEqual(len(result["members"]), 2)

    def test_parquet_preparation_rejects_unexpected_row_count(self):
        archive = self.scratch / "passages.tar"
        self._write_archive(
            archive,
            {"part-00.jsonl.gz": [{"pid": "p1", "passage": "alpha"}]},
        )
        expected_md5 = hashlib.md5(archive.read_bytes()).hexdigest()

        class FakeWriter:
            def __init__(self, path):
                self.path = path

            def write_rows(self, rows):
                self.path.write_bytes(repr(rows).encode())

            def close(self):
                pass

        with self.assertRaisesRegex(DatasetError, "expected 2 rows, converted 1"):
            prepare_parquet(
                archive,
                self.scratch / "prepared",
                expected_md5=expected_md5,
                expected_rows=2,
                writer_factory=FakeWriter,
            )

    @unittest.skipUnless(
        importlib.util.find_spec("pyarrow"), "pyarrow is not installed"
    )
    def test_default_writer_creates_benchmarker_compatible_parquet(self):
        import pyarrow.parquet as parquet

        archive = self.scratch / "passages.tar"
        self._write_archive(
            archive,
            {"part-00.jsonl.gz": [{"pid": "p1", "passage": "alpha"}]},
        )
        expected_md5 = hashlib.md5(archive.read_bytes()).hexdigest()

        prepare_parquet(
            archive,
            self.scratch / "prepared",
            expected_md5=expected_md5,
            expected_rows=1,
        )
        table = parquet.read_table(
            self.scratch / "prepared" / "data" / "part-00000.parquet"
        )

        self.assertEqual(table.column_names, ["id", "source_id", "body"])
        self.assertEqual(table.to_pylist(), [
            {"id": 1, "source_id": "p1", "body": "alpha"}
        ])

    @unittest.skipUnless(
        importlib.util.find_spec("pyarrow"), "pyarrow is not installed"
    )
    def test_synthetic_smoke_is_labeled_and_deterministic(self):
        import pyarrow.parquet as parquet

        first = prepare_synthetic(
            self.scratch / "first", rows=10, seed=7
        )
        second = prepare_synthetic(
            self.scratch / "second", rows=10, seed=7
        )
        first_rows = parquet.read_table(
            self.scratch / "first" / "data" / "part-00000.parquet"
        ).to_pylist()
        second_rows = parquet.read_table(
            self.scratch / "second" / "data" / "part-00000.parquet"
        ).to_pylist()

        self.assertEqual(first_rows, second_rows)
        self.assertEqual(first["dataset"], "synthetic-smoke")
        self.assertFalse(first["comparable_to_full_scale"])

    @staticmethod
    def _write_archive(path, members):
        encoded = {
            name: b"".join(
                json.dumps(row).encode() + b"\n" for row in rows
            )
            for name, rows in members.items()
        }
        DatasetTest._write_raw_archive(path, encoded)

    @staticmethod
    def _write_raw_archive(path, members):
        with tarfile.open(path, "w") as archive:
            for name, raw in members.items():
                payload = io.BytesIO()
                with gzip.GzipFile(fileobj=payload, mode="wb", mtime=0) as out:
                    out.write(raw)
                data = payload.getvalue()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))


if __name__ == "__main__":
    unittest.main()
