import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import unittest
from unittest.mock import patch


MODULE = pathlib.Path(__file__).with_name("bootstrap_disks.py")


class EphemeralDiskTests(unittest.TestCase):
    def module(self):
        spec = importlib.util.spec_from_file_location(
            "bootstrap_disks", MODULE
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def select(self, devices):
        self.assertTrue(MODULE.exists(), "Safe disk selection is not implemented")
        return self.module().select_disks({"blockdevices": devices})

    def disk(self, path, **changes):
        disk = {"path": path, "type": "disk",
                "model": "Microsoft NVMe Direct Disk", "mountpoints": [None],
                "size": 1_920_000_000_000}
        disk.update(changes)
        return disk

    def test_selects_only_unmounted_ephemeral_nvme(self):
        self.assertEqual(self.select([
            self.disk("/dev/nvme0n1", model="Virtual Disk",
                      children=[{"mountpoints": ["/"]}]),
            self.disk("/dev/nvme1n1"),
            self.disk("/dev/nvme2n1"),
        ]), ["/dev/nvme1n1", "/dev/nvme2n1"])

    def test_refuses_any_mounted_or_partitioned_ephemeral_disk(self):
        for disk in (
            self.disk("/dev/nvme1n1", mountpoints=["/data"]),
            self.disk("/dev/nvme1n1", children=[{"mountpoints": [None]}]),
        ):
            with self.assertRaises(ValueError):
                self.select([disk])

    def test_refuses_to_fall_back_to_os_disk(self):
        with self.assertRaises(ValueError):
            self.select([self.disk("/dev/sda", model="Virtual Disk")])

    def test_wrong_compute_identity_stops_before_disk_inspection(self):
        module = self.module()
        expected = (
            "/subscriptions/sub/resourceGroups/owned/providers/"
            "Microsoft.Compute/virtualMachines/searchbench"
        )
        metadata = io.BytesIO(json.dumps({
            "resourceId": expected.replace("/owned/", "/foreign/"),
            "tagsList": [{"name": "managed-by", "value": module.MARKER}],
        }).encode())
        response = unittest.mock.MagicMock()
        response.__enter__.return_value = metadata
        opener = unittest.mock.MagicMock()
        opener.open.return_value = response

        with (
            patch.object(module.urllib.request, "build_opener",
                         return_value=opener),
            patch.object(module.subprocess, "run") as run,
            patch.object(
                sys, "argv",
                ["bootstrap_disks.py", "--expected-compute-id", expected,
                 "--initialize-ephemeral-storage"],
            ),
        ):
            with self.assertRaisesRegex(ValueError, "compute resource"):
                module.main()

        run.assert_not_called()

    def test_missing_guest_ownership_marker_stops_before_disk_inspection(self):
        module = self.module()
        expected = (
            "/subscriptions/sub/resourceGroups/owned/providers/"
            "Microsoft.Compute/virtualMachines/searchbench"
        )
        metadata = io.BytesIO(json.dumps({
            "resourceId": expected,
            "tagsList": [{"name": "purpose", "value": "benchmark"}],
        }).encode())
        response = unittest.mock.MagicMock()
        response.__enter__.return_value = metadata
        opener = unittest.mock.MagicMock()
        opener.open.return_value = response

        with (
            patch.object(module.urllib.request, "build_opener",
                         return_value=opener),
            patch.object(module.subprocess, "run") as run,
            patch.object(
                sys, "argv",
                ["bootstrap_disks.py", "--expected-compute-id", expected,
                 "--initialize-ephemeral-storage"],
            ),
        ):
            with self.assertRaisesRegex(ValueError, "ownership marker"):
                module.main()

        run.assert_not_called()


class PinnedGoInstallerTests(unittest.TestCase):
    def setUp(self):
        self.root = (
            pathlib.Path("benchmarks/scale")
            / f".test-go-{self._testMethodName}"
        )
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root, True)

    def test_installs_exact_go_without_replacing_foreign_install(self):
        helper = MODULE.with_name("install_go.sh")
        tools = self.root / "fake-bin"
        tools.mkdir()
        foreign = self.root / "foreign-go"
        foreign.mkdir()
        (foreign / "sentinel").write_text("keep\n")
        archive = self.root / "go.tar.gz"
        archive.write_text("fixture\n")
        fake_tar = tools / "tar"
        fake_tar.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            'dest=""\n'
            'while [ "$#" -gt 0 ]; do\n'
            '  if [ "$1" = "-C" ]; then dest="$2"; shift 2; else shift; fi\n'
            "done\n"
            'mkdir -p "$dest/go/bin"\n'
            "cat >\"$dest/go/bin/go\" <<'EOF'\n"
            "#!/bin/sh\n"
            "echo 'go version go1.26.11 linux/amd64'\n"
            "EOF\n"
            'cp "$dest/go/bin/go" "$dest/go/bin/gofmt"\n'
            'chmod +x "$dest/go/bin/go" "$dest/go/bin/gofmt"\n'
        )
        fake_tar.chmod(0o755)
        fake_sha = tools / "sha256sum"
        fake_sha.write_text("#!/bin/sh\ncat >/dev/null\nexit 0\n")
        fake_sha.chmod(0o755)
        install_root = (self.root / "toolchains").resolve()
        bin_dir = (self.root / "bin").resolve()
        environment = os.environ | {
            "PATH": f"{tools}:{os.environ['PATH']}",
            "GO_TOOLCHAINS_DIR": str(install_root),
            "GO_BIN_DIR": str(bin_dir),
            "GO_ARCHIVE_FILE": str(archive.resolve()),
        }

        for _ in range(2):
            result = subprocess.run(
                ["bash", "-c", f"source {helper}; install_pinned_go"],
                env=environment, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

        self.assertEqual((foreign / "sentinel").read_text(), "keep\n")
        go = bin_dir / "go"
        self.assertTrue(go.is_symlink())
        self.assertEqual(
            go.resolve(),
            (install_root / "go1.26.11" / "bin" / "go").resolve(),
        )
        version = subprocess.run(
            [str(go), "version"], check=True, capture_output=True, text=True
        )
        self.assertEqual(
            version.stdout.strip(), "go version go1.26.11 linux/amd64"
        )

    def test_refuses_to_replace_foreign_go_shim(self):
        helper = MODULE.with_name("install_go.sh")
        bin_dir = (self.root / "bin").resolve()
        bin_dir.mkdir()
        foreign = bin_dir / "go"
        foreign.write_text("#!/bin/sh\necho foreign\n")
        foreign.chmod(0o755)
        owned = self.root / "toolchains" / "go1.26.11" / "bin"
        owned.mkdir(parents=True)
        for name in ("go", "gofmt"):
            path = owned / name
            path.write_text(
                "#!/bin/sh\necho 'go version go1.26.11 linux/amd64'\n"
            )
            path.chmod(0o755)

        result = subprocess.run(
            ["bash", "-c", f"source {helper}; install_pinned_go"],
            env=os.environ | {
                "GO_TOOLCHAINS_DIR": str(
                    (self.root / "toolchains").resolve()
                ),
                "GO_BIN_DIR": str(bin_dir),
            },
            capture_output=True, text=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Refusing to replace foreign", result.stderr)
        self.assertEqual(foreign.read_text(), "#!/bin/sh\necho foreign\n")


if __name__ == "__main__":
    unittest.main()
