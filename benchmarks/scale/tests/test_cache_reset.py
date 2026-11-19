import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from benchmarks.scale.cache_reset import (
    evict_file,
    reset_volume,
    resident_pages,
)
from benchmarks.scale.run import ConfigError


@unittest.skipUnless(sys.platform == "linux", "requires Linux mincore/fadvise")
class CacheResetTests(unittest.TestCase):
    def test_only_requested_file_is_evicted(self):
        # /tmp may be tmpfs, whose pages cannot be evicted to backing storage.
        with tempfile.TemporaryDirectory(
            prefix=".test-tmp-", dir=Path(__file__).resolve().parents[1],
        ) as directory:
            target = Path(directory) / "target"
            other = Path(directory) / "other"
            target.write_bytes(b"a" * (2 * 1024**2))
            other.write_bytes(b"b" * (2 * 1024**2))
            other_fd = os.open(other, os.O_RDONLY)
            try:
                before_other = resident_pages(other_fd, other.stat().st_size)
                self.assertGreater(before_other, 0)
                size, before = evict_file(target)
                self.assertEqual(size, 2 * 1024**2)
                self.assertGreater(before, 0)
                with target.open("rb") as stream:
                    self.assertEqual(resident_pages(stream.fileno(), size), 0)
                self.assertEqual(
                    resident_pages(other_fd, other.stat().st_size),
                    before_other,
                )
            finally:
                os.close(other_fd)

    def test_live_host_is_rejected_before_cache_eviction(self):
        with (
            patch("benchmarks.scale.cache_reset.validate_storage"),
            patch(
                "benchmarks.scale.cache_reset.docker_output",
                side_effect=["/srv/searchbench/docker", "active-container"],
            ),
        ):
            with self.assertRaisesRegex(ConfigError, "idle benchmark host"):
                reset_volume(
                    "high.pg_textsearch.english-no-stopwords",
                    "/srv/searchbench/pg_textsearch-scale/runs/test",
                    "pgts-tin-example",
                )


if __name__ == "__main__":
    unittest.main()
