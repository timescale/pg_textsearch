import os
import json
from pathlib import Path
import sys
import subprocess
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

    def test_custom_root_preserves_foreign_volume_rejection(self):
        root = Path("/srv/searchbench/custom-benchmarks")
        with (
            patch(
                "benchmarks.scale.cache_reset.validate_storage"
            ) as validate,
            patch(
                "benchmarks.scale.cache_reset.docker_output",
                side_effect=[
                    "/srv/searchbench/docker", "",
                    json.dumps([{
                        "Labels": {"managed-by": "another-owner"},
                    }]),
                ],
            ),
        ):
            with self.assertRaisesRegex(ConfigError, "foreign"):
                reset_volume("phase", root / "runs/test", "foreign-volume")
        validate.assert_called_once_with(root, smoke=True)

    def test_shell_hook_uses_system_python_not_artifacts_interpreter(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = root / "arguments.json"
            sudo = root / "sudo"
            sudo.write_text(
                "#!/usr/bin/python3\n"
                "import json, os, sys\n"
                "with open(os.environ['CAPTURE'], 'w') as stream:\n"
                "    json.dump(sys.argv[1:], stream)\n"
            )
            sudo.chmod(0o700)
            script = Path(__file__).resolve().parents[1] / "cache_reset.sh"
            result = subprocess.run(
                ["bash", str(script), "phase", "run-directory", "volume"],
                env={
                    **os.environ,
                    "PATH": f"{root}:{os.environ['PATH']}",
                    "CAPTURE": str(capture),
                },
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            arguments = json.loads(capture.read_text())
        self.assertEqual(arguments[:2], ["-A", "/usr/bin/python3"])
        self.assertEqual(arguments[3:], [
            "phase", "run-directory", "volume",
        ])


if __name__ == "__main__":
    unittest.main()
