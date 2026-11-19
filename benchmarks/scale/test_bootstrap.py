import importlib.util
import pathlib
import unittest


MODULE = pathlib.Path(__file__).with_name("bootstrap_disks.py")


class EphemeralDiskTests(unittest.TestCase):
    def select(self, devices):
        self.assertTrue(MODULE.exists(), "Safe disk selection is not implemented")
        spec = importlib.util.spec_from_file_location("bootstrap_disks", MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.select_disks({"blockdevices": devices})

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


if __name__ == "__main__":
    unittest.main()
