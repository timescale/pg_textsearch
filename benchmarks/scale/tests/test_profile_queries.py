import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from benchmarks.scale import profile_queries as profile


class ProfileTests(unittest.TestCase):
    def test_pid_mapping_is_container_scoped_and_unambiguous(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for pid, namespace in ((101, 7), (102, 8)):
                (root / str(pid)).mkdir()
                (root / str(pid) / "status").write_text(
                    f"Name:\tpostgres\nNSpid:\t{pid}\t{namespace}\n"
                )
            with patch.object(
                subprocess, "check_output", return_value="PID\n101\n102\n"
            ) as command:
                self.assertEqual(
                    profile.host_backend_pid("owned", 8, root), 102
                )
                self.assertEqual(
                    command.call_args.args[0],
                    ["docker", "top", "owned", "-eo", "pid"],
                )
                with self.assertRaisesRegex(RuntimeError, "ambiguous"):
                    profile.host_backend_pid("owned", 9, root)
                (root / "101" / "status").write_text("NSpid:\t101\t8\n")
                with self.assertRaisesRegex(RuntimeError, "ambiguous"):
                    profile.host_backend_pid("owned", 8, root)

    def test_mapped_symbols_reject_deleted_binaries(self):
        maps = (
            "1-2 r-xp 0 00:00 1 /usr/bin/postgres\n"
            "3-4 r-xp 0 00:00 2 /usr/lib/pg_textsearch.so\n"
            "5-6 r-xp 0 00:00 0 [vdso]\n"
        )
        self.assertEqual(profile.mapped_paths(maps), {
            "/usr/bin/postgres", "/usr/lib/pg_textsearch.so",
        })
        with self.assertRaisesRegex(RuntimeError, "deleted"):
            profile.mapped_paths(
                maps + "7-8 r-xp 0 00:00 3 /lib/libc.so (deleted)\n"
            )

    def test_receipt_must_identify_the_ready_selected_container(self):
        receipt = {
            "status": "ready", "container": "owned",
            "binary_sha256": "a" * 64, "source_sha": "b" * 40,
            "after": [10, 10, 8192],
        }
        self.assertEqual(profile.validate_receipt(receipt, "owned"), receipt)
        self.assertEqual(
            profile.validate_receipt(receipt, "owned", [10, 10, 8192]), receipt
        )
        with self.assertRaises(ValueError):
            profile.validate_receipt(receipt, "owned", [11, 11, 8192])
        for invalid in (
            receipt | {"container": "foreign"},
            receipt | {"status": "preparing"},
            receipt | {"binary_sha256": "bad"},
        ):
            with self.assertRaises(ValueError):
                profile.validate_receipt(invalid, "owned")

    def test_recorder_cleanup_signals_only_its_owned_child(self):
        with subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ) as child:
            self.assertIn(
                profile.stop_recorder(child), (0, 130, -signal.SIGINT)
            )
            self.assertIsNotNone(child.poll())

    def test_foreign_container_fails_before_backend_or_perf(self):
        with patch.object(
            subprocess, "check_output", return_value=json.dumps({
                "managed-by": "someone-else",
            })
        ) as command:
            with self.assertRaisesRegex(RuntimeError, "ownership"):
                profile.validate_container("foreign")
            self.assertEqual(command.call_count, 1)

    def test_profile_output_is_exclusive_and_restores_sudo_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "profile"
            with (
                patch("os.geteuid", return_value=0),
                patch.dict(
                    "os.environ", {"SUDO_UID": "123", "SUDO_GID": "456"}
                ),
                patch("os.chown") as chown,
            ):
                with profile.profile_output(output):
                    (output / "result.json").write_text("{}")
                self.assertEqual(chown.call_count, 2)
                self.assertEqual(chown.call_args.args, (output, 123, 456))
                with self.assertRaises(FileExistsError):
                    with profile.profile_output(output):
                        self.fail("existing output must not be entered")
