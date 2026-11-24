import copy
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
import uuid

from benchmarks.scale.replace_binary import await_ready, validate_target


class ReplaceBinaryTests(unittest.TestCase):
    def test_rejects_ephemeral_port_that_would_change_after_restart(self):
        info = {
            "Image": "image", "State": {"Running": True},
            "Mounts": [{"Type": "volume", "Name": "volume"}],
            "NetworkSettings": {
                "Ports": {"5432/tcp": [{"HostPort": "16432"}]}
            },
            "HostConfig": {
                "PortBindings": {"5432/tcp": [{"HostPort": ""}]}
            },
        }
        with self.assertRaises(RuntimeError):
            validate_target(info, "image", "volume", port=16432)

    def test_rejects_database_port_outside_target_container(self):
        info = {
            "Image": "image", "State": {"Running": True},
            "Mounts": [{"Type": "volume", "Name": "volume"}],
            "NetworkSettings": {
                "Ports": {"5432/tcp": [{"HostPort": "15432"}]}
            },
        }
        with self.assertRaises(RuntimeError):
            validate_target(info, "image", "volume", port=16432)

    def test_rejects_foreign_or_stopped_targets(self):
        expected = {
            "Image": "expected-image", "State": {"Running": True},
            "Mounts": [{"Type": "volume", "Name": "expected-volume"}],
        }
        validate_target(expected, "expected-image", "expected-volume")
        for change in (
            {"Image": "foreign-image"},
            {"State": {"Running": False}},
            {"Mounts": [{"Type": "volume", "Name": "foreign-volume"}]},
            {"Mounts": [{"Type": "bind", "Name": "expected-volume"}]},
            {"Mounts": expected["Mounts"] * 2},
        ):
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                validate_target(
                    copy.deepcopy(expected) | change,
                    "expected-image", "expected-volume",
                )


@unittest.skipUnless(
    os.environ.get("PGTS_REPLACEMENT_TEST_IMAGE")
    and os.environ.get("PGTS_REPLACEMENT_TEST_BINARY"),
    "set PGTS_REPLACEMENT_TEST_IMAGE and PGTS_REPLACEMENT_TEST_BINARY",
)
class ReplaceBinaryIntegrationTests(unittest.TestCase):
    def test_deployment_idempotency_and_startup_rollback_preserve_data(self):
        def command(*args):
            return subprocess.check_output(args, text=True).strip()

        container = "pgts-replacement-test-" + uuid.uuid4().hex[:12]
        image = os.environ["PGTS_REPLACEMENT_TEST_IMAGE"]
        binary = Path(os.environ["PGTS_REPLACEMENT_TEST_BINARY"]).resolve()
        library = os.environ.get(
            "PGTS_REPLACEMENT_TEST_LIBRARY",
            "/usr/lib/postgresql/18/lib/pg_textsearch.so",
        )
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = str(reservation.getsockname()[1])
        command(
            "docker", "create", "--name", container,
            "--cpus", "1", "--memory", "2g", "--shm-size", "1g",
            "-p", f"127.0.0.1:{port}:5432",
            "-e", "POSTGRES_HOST_AUTH_METHOD=trust", image,
            "-c", "shared_preload_libraries=pg_textsearch",
            "-c", "shared_buffers=64MB",
        )
        try:
            command("docker", "start", container)
            await_ready(container)
            command(
                "docker", "exec", container, "psql",
                "-U", "postgres", "-v", "ON_ERROR_STOP=1", "-c",
                "CREATE EXTENSION pg_textsearch;"
                "CREATE TABLE documents (id bigint PRIMARY KEY, body text);"
                "INSERT INTO documents VALUES (1,'alpha'),(2,'alpha beta');"
                "CREATE INDEX documents_bm25 ON documents USING bm25(body)"
                " WITH(text_config='simple');",
            )
            info = json.loads(command("docker", "inspect", container))[0]
            original = command(
                "docker", "exec", container, "sha256sum", library
            ).split()[0]
            checksum = hashlib.sha256(binary.read_bytes()).hexdigest()
            common = [
                sys.executable, "-m", "benchmarks.scale.replace_binary",
                "--container", container, "--port", port,
                "--expected-image", info["Image"],
                "--expected-volume", info["Mounts"][0]["Name"],
                "--worker-unit", "pgts-replacement-test-inactive",
                "--library", library,
            ]
            with tempfile.TemporaryDirectory() as directory:
                successful = common + [
                    "--expected-original-sha256", original,
                    "--binary", str(binary), "--binary-sha256", checksum,
                    "--source-sha", "integration-test",
                    "--backup-directory", str(Path(directory) / "successful"),
                ]
                first = json.loads(command(*successful))
                self.assertEqual(first["status"], "ready")
                self.assertEqual(first["before"][:2], first["after"][:2])
                alternate = library + ".alternate"
                command(
                    "docker", "cp", str(binary), f"{container}:{alternate}"
                )
                for option, value in (
                    ("--index", "public.foreign_index"),
                    ("--expected-original-sha256", "0" * 64),
                    ("--library", alternate),
                ):
                    conflicting = subprocess.run(
                        successful + [option, value],
                        capture_output=True, text=True,
                    )
                    self.assertNotEqual(
                        conflicting.returncode, 0, (option, conflicting.stdout)
                    )
                started = json.loads(command(
                    "docker", "inspect", container
                ))[0]["State"]["StartedAt"]
                self.assertEqual(json.loads(command(*successful)), first)
                self.assertEqual(json.loads(command(
                    "docker", "inspect", container
                ))[0]["State"]["StartedAt"], started)
                command(
                    "docker", "exec", container, "psql",
                    "-U", "postgres", "-v", "ON_ERROR_STOP=1", "-c",
                    "REINDEX INDEX documents_bm25;",
                )
                drifted = subprocess.run(
                    successful, capture_output=True, text=True
                )
                self.assertNotEqual(drifted.returncode, 0, drifted.stdout)
                invalid = Path(directory) / "invalid.so"
                invalid.write_bytes(b"deliberately invalid shared library\n")
                failed = subprocess.run(
                    common + [
                        "--expected-original-sha256", checksum,
                        "--binary", str(invalid), "--binary-sha256",
                        hashlib.sha256(invalid.read_bytes()).hexdigest(),
                        "--source-sha", "readiness-failure-test",
                        "--backup-directory",
                        str(Path(directory) / "rollback"),
                    ],
                    capture_output=True, text=True,
                )
                self.assertNotEqual(failed.returncode, 0)
                receipt = json.loads((
                    Path(directory) / "rollback" / f"{container}.json"
                ).read_text())
                self.assertEqual(receipt["status"], "rolled_back")
                self.assertEqual(command(
                    "docker", "exec", container, "sha256sum", library
                ).split()[0], checksum)
                self.assertEqual(command(
                    "docker", "exec", container, "psql",
                    "-U", "postgres", "-XqAt", "-c",
                    "SELECT count(*) FROM documents;"
                ), "2")
        finally:
            command("docker", "stop", "--time", "10", container)
            command("docker", "rm", "-v", container)


if __name__ == "__main__":
    unittest.main()
