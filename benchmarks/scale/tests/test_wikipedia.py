import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from benchmarks.scale import wikipedia


class WikipediaQueryTests(unittest.TestCase):
    def test_span_pack_interleaves_lengths_without_changing_selection(self):
        samples = [
            {
                "id": f"article-{span}-{sample}",
                "text": " ".join(["word"] * span),
                "span": span,
            }
            for span in range(2, 16)
            for sample in range(101)
        ]
        ordered = sorted(
            samples,
            key=lambda query: hashlib.sha256(
                query["id"].encode()
            ).hexdigest(),
        )
        expected_ids = {
            query["id"]
            for span in range(2, 16)
            for query in [
                item for item in ordered if item["span"] == span
            ][:100]
        }
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=Path(__file__).resolve().parents[1],
        ) as directory:
            root = Path(directory)
            files = [
                {
                    "path": f"20231101.en/train-{number:05d}-of-00041.parquet",
                    "lfs": {"oid": "fixture"},
                }
                for number in range(41)
            ]
            (root / "source-files.json").write_text(json.dumps(files))
            conversions = [
                (
                    {
                        "rows": wikipedia.EXPECTED_ROWS // 41
                        + (number < wikipedia.EXPECTED_ROWS % 41),
                        "text_bytes": 1,
                        "titles": [{"id": "title", "text": "article title"}],
                    },
                    samples if number == 0 else [],
                )
                for number in range(41)
            ]
            with (
                patch("sys.argv", [
                    "wikipedia.py", "--output", str(root),
                ]),
                patch("benchmarks.scale.wikipedia.download"),
                patch(
                    "benchmarks.scale.wikipedia.convert_shard",
                    side_effect=conversions,
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                wikipedia.main()
            queries = json.loads((root / "k6/spans.json").read_text())

        self.assertEqual(
            [query["span"] for query in queries[:28]],
            list(range(2, 16)) * 2,
        )
        self.assertEqual({query["id"] for query in queries}, expected_ids)
        self.assertEqual(len(queries), len(expected_ids))


if __name__ == "__main__":
    unittest.main()
