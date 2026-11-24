import datetime as dt
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from benchmarks.scale.fleet_azure import (
    ConfigError,
    FleetAzure,
    ROLES,
    config_from_manifest,
    compute_deadline,
    hardware_from_probe,
    region_capacity,
    remote_command,
)


class FleetAzureTest(unittest.TestCase):
    def setUp(self):
        self.root = (
            Path("benchmarks/scale/tests")
            / f".fleet-azure-{self._testMethodName}"
        )
        shutil.rmtree(self.root, ignore_errors=True)
        self.root.mkdir()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.config = {
            "subscription": "sub",
            "group_prefix": "searchbench-fleet",
            "regions": ["region-a", "region-b"],
            "size": "Standard_L32as_v3",
            "lifetime_days": 7,
            "ssh_private_key": str(self.root / "key"),
            "ssh_public_key": str(self.root / "key.pub"),
            "ssh_cidr": "203.0.113.10/32",
        }
        (self.root / "key").write_text("private")
        (self.root / "key.pub").write_text("ssh-ed25519 AAAAtest\n")

    def test_role_matrix_is_fixed_and_complete(self):
        self.assertEqual(
            ROLES,
            (
                (1, "pg_textsearch", "high"),
                (2, "pg_textsearch", "constrained32"),
                (3, "pg_textsearch", "constrained16"),
                (4, "paradedb", "high"),
                (5, "paradedb", "constrained32"),
                (6, "paradedb", "constrained16"),
            ),
        )

    def test_deadline_persists_and_incompatible_resume_is_rejected(self):
        now = dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        fleet = FleetAzure(self.config, self.root)
        first = fleet.prepare_manifest(now=now)
        second = fleet.prepare_manifest(now=now + dt.timedelta(days=1))

        self.assertEqual(first["common_deadline"], second["common_deadline"])
        self.assertEqual(
            first["common_deadline"],
            compute_deadline(now, 7),
        )
        changed = dict(self.config, size="Standard_L32s_v3")
        with self.assertRaisesRegex(ConfigError, "incompatible"):
            FleetAzure(changed, self.root).prepare_manifest(now=now)

    def test_expired_deadline_blocks_all_azure_mutation(self):
        now = dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        config = dict(
            self.config, existing_group="searchbench-existing"
        )
        FleetAzure(config, self.root).prepare_manifest(now=now)
        calls = []
        fleet = FleetAzure(
            config,
            self.root,
            azure=lambda *args: calls.append(args),
        )

        with self.assertRaisesRegex(ConfigError, "expired"):
            fleet.provision(now=now + dt.timedelta(days=8))

        self.assertEqual(calls, [])

    def test_capacity_uses_exact_sku_family_and_regional_quota(self):
        skus = [{
            "name": "Standard_L32as_v3",
            "family": "standardLASv3Family",
            "restrictions": [],
        }]
        usage = [
            {"name": {"value": "standardLASv3Family"},
             "currentValue": 32, "limit": 160},
            {"name": {"value": "cores"},
             "currentValue": 190, "limit": 250},
        ]

        capacity = region_capacity(skus, usage, "Standard_L32as_v3")

        self.assertEqual(capacity["family"], "standardLASv3Family")
        self.assertEqual(capacity["family_remaining"], 128)
        self.assertEqual(capacity["regional_remaining"], 60)
        self.assertEqual(capacity["available_hosts"], 1)

    def test_plan_only_uses_read_only_azure_queries(self):
        calls = []

        def azure(*args):
            calls.append(args)
            if args[:2] == ("vm", "list-skus"):
                return [{
                    "name": "Standard_L32as_v3",
                    "family": "standardLASv3Family",
                    "restrictions": [],
                }]
            if args[:2] == ("vm", "list-usage"):
                return [
                    {"name": {"value": "standardLASv3Family"},
                     "currentValue": 0, "limit": 200},
                    {"name": {"value": "cores"},
                     "currentValue": 0, "limit": 200},
                ]
            self.fail(f"unexpected Azure call: {args}")

        plan = FleetAzure(
            self.config, self.root, azure=azure
        ).plan(now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc))

        self.assertEqual(len(plan["hosts"]), 6)
        self.assertTrue(all(
            call[:2] in (("vm", "list-skus"), ("vm", "list-usage"))
            for call in calls
        ))

    def test_capacity_failure_falls_back_without_losing_failed_attempt(self):
        class FakeFleet(FleetAzure):
            def _region_capacities(self):
                return {
                    "region-a": {
                        "available_hosts": 6,
                        "family": "standardLASv3Family",
                    },
                    "region-b": {
                        "available_hosts": 6,
                        "family": "standardLASv3Family",
                    },
                }

            def _create_host(self, host, region, manifest):
                if region == "region-a":
                    return {
                        "status": "failed",
                        "error_code": "SkuNotAvailable",
                        "deployment_id": "failed-deployment",
                    }
                return {
                    "status": "provisioned",
                    "group": "searchbench-fleet-02-region-b",
                    "location": region,
                    "size": self.config["size"],
                    "expiration": manifest["common_deadline"],
                    "compute_id": "compute-2",
                    "ip": "203.0.113.2",
                }

            def _bootstrap_host(self, _host):
                return {
                    "cpu_model": "AMD EPYC",
                    "kernel": "test",
                    "nvme_count": 4,
                    "nvme_raid_bytes": 1,
                }

        fleet = FakeFleet(self.config, self.root)
        manifest = fleet.provision(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc),
            only_hosts={2},
        )
        host = manifest["hosts"][1]

        self.assertEqual(host["location"], "region-b")
        self.assertEqual(host["sku_family"], "standardLASv3Family")
        self.assertTrue(host["bootstrap_ready"])
        self.assertEqual(
            [attempt["error_code"] for attempt in host["attempts"]],
            ["SkuNotAvailable", None],
        )
        self.assertEqual(
            host["attempts"][0]["deployment_id"],
            "failed-deployment",
        )
        self.assertEqual(manifest["status"], "cleanup_required")
        self.assertEqual(len(manifest["cleanup_required"]), 1)
        obligation = manifest["cleanup_required"][0]
        self.assertEqual(
            obligation["group"], "searchbench-fleet-02-region-a"
        )
        self.assertEqual(
            obligation["destroy_command"][-4:],
            ["--subscription", "sub", "--group",
             "searchbench-fleet-02-region-a"],
        )

    def test_cleanup_obligation_blocks_resume_without_deleting_resources(self):
        class FakeFleet(FleetAzure):
            def _region_capacities(self):
                self.fail("capacity mutation must be blocked")

        fleet = FleetAzure(self.config, self.root)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        manifest["cleanup_required"] = [{
            "group": "searchbench-fleet-02-region-a",
            "subscription": "sub",
            "error_code": "SkuNotAvailable",
            "destroy_command": [
                sys.executable,
                str(Path("benchmarks/scale/azure.py").resolve()),
                "destroy", "--subscription", "sub", "--group",
                "searchbench-fleet-02-region-a",
            ],
        }]
        fleet._save(manifest)
        calls = []

        def azure(*args):
            calls.append(args)
            if args[:2] == ("group", "exists"):
                return True
            self.fail(f"cleanup reconciliation must be read-only: {args}")

        blocked = FleetAzure(self.config, self.root, azure=azure)
        with self.assertRaisesRegex(
            ConfigError, "azure.py.*destroy.*searchbench-fleet-02-region-a"
        ):
            blocked.provision(
                now=dt.datetime(2026, 11, 21, tzinfo=dt.timezone.utc)
            )

        self.assertEqual(
            calls,
            [("group", "exists", "-n",
              "searchbench-fleet-02-region-a")],
        )

    def test_absent_failed_group_is_reconciled_without_delete(self):
        fleet = FleetAzure(self.config, self.root)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        for host in manifest["hosts"]:
            host["bootstrap_ready"] = True
        manifest["cleanup_required"] = [{
            "group": "searchbench-fleet-02-region-a",
            "subscription": "sub",
            "error_code": "SkuNotAvailable",
            "destroy_command": ["python3", "azure.py", "destroy"],
        }]
        fleet._save(manifest)
        calls = []

        def azure(*args):
            calls.append(args)
            if args[:2] == ("group", "exists"):
                return False
            if args[:2] == ("vm", "list-skus"):
                return [{
                    "name": self.config["size"],
                    "family": "standardLASv3Family",
                    "restrictions": [],
                }]
            if args[:2] == ("vm", "list-usage"):
                return [
                    {"name": {"value": "standardLASv3Family"},
                     "currentValue": 0, "limit": 200},
                    {"name": {"value": "cores"},
                     "currentValue": 0, "limit": 200},
                ]
            self.fail(f"unexpected Azure call: {args}")

        result = FleetAzure(
            self.config, self.root, azure=azure
        ).provision(now=dt.datetime(
            2026, 11, 21, tzinfo=dt.timezone.utc
        ))

        self.assertEqual(result["cleanup_required"], [])
        self.assertEqual(result["status"], "ready")
        self.assertFalse(any("delete" in call for call in calls))

    def test_existing_host_updates_and_reads_back_native_expiration(self):
        calls = []
        current = {
            "location": "region-a",
            "tags": {"managed-by": "pg_textsearch-scale"},
            "properties": {
                "computeId": (
                    "/subscriptions/sub/resourceGroups/compute-group/"
                    "providers/Microsoft.Compute/virtualMachines/searchbench"
                ),
                "expirationDate": "2026-12-01T00:00:00+00:00",
                "galleryImageReference": {"version": "image"},
                "isAuthenticationWithSshKey": True,
                "labSubnetName": "default",
                "labVirtualNetworkId": "network",
                "provisioningState": "Succeeded",
                "size": "Standard_L32as_v3",
                "storageType": "Premium",
                "userName": "bench",
            },
        }

        def azure(*args):
            calls.append(args)
            if args[:3] == ("group", "show", "-n"):
                return {"tags": {"managed-by": "pg_textsearch-scale"}}
            if args[:3] == ("rest", "--method", "get"):
                return current
            if args[:3] == ("rest", "--method", "put"):
                body = json.loads(args[args.index("--body") + 1])
                current["properties"]["expirationDate"] = (
                    body["properties"]["expirationDate"]
                )
                return current
            if args[:2] == ("resource", "show"):
                return {"tags": {
                    "managed-by": "pg_textsearch-scale"
                }}
            if args[:2] == ("vm", "list-ip-addresses"):
                return [{"virtualMachine": {"network": {
                    "publicIpAddresses": [{"ipAddress": "203.0.113.1"}],
                }}}]
            self.fail(f"unexpected Azure call: {args}")

        config = dict(self.config, existing_group="searchbench-existing")
        fleet = FleetAzure(config, self.root, azure=azure)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        host = fleet._adopt_existing(manifest)

        put = next(call for call in calls if call[:3] == (
            "rest", "--method", "put"
        ))
        body = json.loads(put[put.index("--body") + 1])
        self.assertEqual(
            body["properties"]["expirationDate"],
            manifest["common_deadline"],
        )
        self.assertEqual(
            body["properties"]["scheduleParameters"][0]["properties"][
                "status"
            ],
            "Disabled",
        )
        self.assertEqual(host["expiration"], manifest["common_deadline"])

    def test_existing_host_requires_managed_vm_resource(self):
        current = {
            "location": "region-a",
            "tags": {},
            "properties": {
                "computeId": None,
                "provisioningState": "Succeeded",
                "size": "Standard_L32as_v3",
            },
        }

        def azure(*args):
            if args[:3] == ("group", "show", "-n"):
                return {"tags": {"managed-by": "pg_textsearch-scale"}}
            if args[:3] == ("rest", "--method", "get"):
                return current
            self.fail(f"unexpected Azure call: {args}")

        config = dict(self.config, existing_group="searchbench-existing")
        fleet = FleetAzure(config, self.root, azure=azure)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )

        with self.assertRaisesRegex(ConfigError, "managed"):
            fleet._adopt_existing(manifest)

    def test_existing_host_requires_owned_compute_before_expiration_put(self):
        calls = []
        current = {
            "location": "region-a",
            "tags": {"managed-by": "pg_textsearch-scale"},
            "properties": {
                "computeId": (
                    "/subscriptions/sub/resourceGroups/compute-group/"
                    "providers/Microsoft.Compute/virtualMachines/searchbench"
                ),
                "provisioningState": "Succeeded",
                "size": "Standard_L32as_v3",
            },
        }

        def azure(*args):
            calls.append(args)
            if args[:2] == ("group", "show"):
                return {"tags": {"managed-by": "pg_textsearch-scale"}}
            if args[:3] == ("rest", "--method", "get"):
                return current
            if args[:2] == ("resource", "show"):
                return {"tags": {}}
            self.fail(f"unexpected Azure call: {args}")

        config = dict(self.config, existing_group="searchbench-existing")
        fleet = FleetAzure(config, self.root, azure=azure)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )

        with self.assertRaisesRegex(ConfigError, "compute resource"):
            fleet._adopt_existing(manifest)

        self.assertFalse(any(
            call[:3] == ("rest", "--method", "put") for call in calls
        ))

    def test_status_validates_existing_manifest_with_read_only_calls(self):
        calls = []
        verified = []

        class StatusFleet(FleetAzure):
            def _verify_host(self, host):
                verified.append(host["host"])
                return {
                    "cpu_model": "AMD EPYC",
                    "kernel": "test",
                    "nvme_count": 4,
                    "nvme_raid_bytes": 1,
                }

        fleet = FleetAzure(self.config, self.root)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        manifest["hosts"][0].update({
            "group": "searchbench-existing",
            "bootstrap_ready": True,
        })
        fleet._save(manifest)
        Path(manifest["hosts"][0]["known_hosts"]).write_text(
            "ssh-ed25519 test\n"
        )

        def azure(*args):
            calls.append(args)
            if args[:2] == ("group", "show"):
                return {"tags": {
                    "managed-by": "pg_textsearch-scale"
                }}
            if args[:3] == ("rest", "--method", "get"):
                return {
                    "location": "region-a",
                    "tags": {"managed-by": "pg_textsearch-scale"},
                    "properties": {
                        "computeId": (
                            "/subscriptions/sub/resourceGroups/compute/"
                            "providers/Microsoft.Compute/virtualMachines/"
                            "searchbench"
                        ),
                        "expirationDate": manifest["common_deadline"],
                        "provisioningState": "Succeeded",
                        "size": "Standard_L32as_v3",
                    },
                }
            if args[:2] == ("resource", "show"):
                return {"tags": {
                    "managed-by": "pg_textsearch-scale"
                }}
            if args[:2] == ("vm", "list-ip-addresses"):
                return [{"virtualMachine": {"network": {
                    "publicIpAddresses": [{"ipAddress": "203.0.113.1"}],
                }}}]
            self.fail(f"unexpected Azure call: {args}")

        status = StatusFleet(
            self.config, self.root, azure=azure
        ).status()

        self.assertEqual(status["hosts"][0]["ip"], "203.0.113.1")
        self.assertEqual(verified, [1])
        self.assertTrue(all(
            call[:2] in (
                ("group", "show"),
                ("resource", "show"),
                ("vm", "list-ip-addresses"),
            )
            or call[:3] == ("rest", "--method", "get")
            for call in calls
        ))

    def test_status_config_can_be_recovered_from_current_manifest(self):
        manifest = {
            "common_deadline": "2026-11-27T00:00:00Z",
            "hosts": [{
                "host": 1,
                "subscription": "sub",
                "group": "searchbench-existing",
                "location": "region-a",
                "size": "Standard_L32as_v3",
            }],
        }

        config = config_from_manifest(manifest)

        self.assertEqual(config["subscription"], "sub")
        self.assertEqual(config["regions"], ["region-a"])
        self.assertEqual(config["size"], "Standard_L32as_v3")

    def test_status_never_reports_ready_for_host_without_resource_group(self):
        fleet = FleetAzure(self.config, self.root, azure=self.fail)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        for host in manifest["hosts"]:
            host["bootstrap_ready"] = True
        fleet._save(manifest)

        status = fleet.status()

        self.assertEqual(status["status"], "partial")
        self.assertFalse(status["hosts"][0]["bootstrap_ready"])

    def test_status_never_reports_ready_while_cleanup_is_required(self):
        fleet = FleetAzure(self.config, self.root)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        manifest["cleanup_required"] = [{
            "group": "searchbench-fleet-02-region-a",
            "subscription": "sub",
            "error_code": "SkuNotAvailable",
            "destroy_command": ["python3", "azure.py", "destroy"],
        }]
        fleet._save(manifest)

        status = FleetAzure(
            self.config,
            self.root,
            azure=lambda *args: (
                True if args[:2] == ("group", "exists") else self.fail(args)
            ),
        ).status()

        self.assertEqual(status["status"], "cleanup_required")

    def test_read_vm_rejects_wrong_ownership_before_ip_lookup(self):
        calls = []

        def azure(*args):
            calls.append(args)
            if args[:2] == ("group", "show"):
                return {"tags": {}}
            self.fail(f"ownership failure should stop reads: {args}")

        fleet = FleetAzure(self.config, self.root, azure=azure)
        with self.assertRaisesRegex(ConfigError, "resource group"):
            fleet._read_vm("searchbench-fleet-02-region-a")

        self.assertFalse(any(
            call[:2] == ("vm", "list-ip-addresses") for call in calls
        ))

    def test_bootstrap_ready_is_false_when_verification_fails(self):
        class FakeFleet(FleetAzure):
            def _region_capacities(self):
                return {"region-a": {"available_hosts": 6}}

            def _create_host(self, host, region, manifest):
                return {
                    "status": "provisioned",
                    "group": f"group-{host['host']}",
                    "location": region,
                    "size": self.config["size"],
                    "expiration": manifest["common_deadline"],
                    "compute_id": f"compute-{host['host']}",
                    "ip": f"203.0.113.{host['host']}",
                }

            def _bootstrap_host(self, _host):
                raise RuntimeError("SSH verification failed")

        fleet = FakeFleet(self.config, self.root)
        with self.assertRaisesRegex(RuntimeError, "SSH verification"):
            fleet.provision(
                now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc),
                only_hosts={2},
            )

        manifest = json.loads(
            (self.root / "fleet-manifest.json").read_text()
        )
        self.assertFalse(manifest["hosts"][1]["bootstrap_ready"])

    def test_in_flight_attempt_is_persisted_before_cloud_create(self):
        test = self

        class FakeFleet(FleetAzure):
            def _region_capacities(self):
                return {"region-a": {"available_hosts": 6}}

            def _create_host(self, host, region, manifest):
                saved = json.loads(
                    self.manifest_path.read_text()
                )["hosts"][host["host"] - 1]["attempts"]
                test.assertEqual(saved[-1]["status"], "deploying")
                test.assertIn(
                    "/deployments/benchmark",
                    saved[-1]["deployment_id"],
                )
                return {
                    "status": "provisioned",
                    "group": "group-2",
                    "location": region,
                    "size": self.config["size"],
                    "expiration": manifest["common_deadline"],
                    "compute_id": "compute-2",
                    "ip": "203.0.113.2",
                }

            def _bootstrap_host(self, _host):
                return {
                    "cpu_model": "AMD EPYC",
                    "kernel": "test",
                    "nvme_count": 4,
                    "nvme_raid_bytes": 1,
                }

        fleet = FakeFleet(self.config, self.root)

        fleet.provision(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc),
            only_hosts={2},
        )

    def test_ssh_host_key_capture_retries_until_vm_is_ready(self):
        fleet = FleetAzure(self.config, self.root)
        host = {
            "ip": "203.0.113.2",
            "known_hosts": str(self.root / "host2.known_hosts"),
        }
        failure = subprocess.CalledProcessError(1, ["ssh-keyscan"])
        success = SimpleNamespace(stdout="ssh-ed25519 test\n")

        with (
            patch(
                "benchmarks.scale.fleet_azure.subprocess.run",
                side_effect=[failure, success],
            ) as run,
            patch("benchmarks.scale.fleet_azure.time.sleep"),
        ):
            fleet._capture_host_key(host)

        self.assertEqual(run.call_count, 2)
        self.assertEqual(
            Path(host["known_hosts"]).read_text(),
            "ssh-ed25519 test\n",
        )

    def test_bootstrap_syncs_go_helper_and_passes_exact_compute_id(self):
        class BootstrapFleet(FleetAzure):
            def _capture_host_key(self, _host):
                return None

            def _verify_host(self, _host):
                return {"verified": True}

            def _ssh(self, host, *remote, capture=False):
                ssh_calls.append((host, remote, capture))
                return SimpleNamespace()

        host = {
            "ip": "203.0.113.2",
            "known_hosts": str(self.root / "host2.known_hosts"),
            "compute_id": (
                "/subscriptions/sub/resourceGroups/compute/providers/"
                "Microsoft.Compute/virtualMachines/searchbench"
            ),
        }
        ssh_calls = []
        fleet = BootstrapFleet(self.config, self.root)
        with patch(
            "benchmarks.scale.fleet_azure.subprocess.run",
            return_value=SimpleNamespace(),
        ) as run:
            fleet._bootstrap_host(host)

        copied = run.call_args.args[0]
        self.assertTrue(any(
            str(path).endswith("/install_go.sh") for path in copied
        ))
        bootstrap = ssh_calls[-1][1]
        self.assertEqual(bootstrap[-1], host["compute_id"])

    def test_provisioned_state_is_reused_without_duplicate_cloud_create(self):
        calls = []

        def azure(*args):
            calls.append(args)
            if args[:2] == ("group", "show"):
                return {"tags": {
                    "managed-by": "pg_textsearch-scale"
                }}
            if args[:3] == ("rest", "--method", "get"):
                return {
                    "location": "region-a",
                    "tags": {"managed-by": "pg_textsearch-scale"},
                    "properties": {
                        "computeId": (
                            "/subscriptions/sub/resourceGroups/compute/"
                            "providers/Microsoft.Compute/virtualMachines/"
                            "searchbench"
                        ),
                        "expirationDate": manifest["common_deadline"],
                        "provisioningState": "Succeeded",
                        "size": "Standard_L32as_v3",
                    },
                }
            if args[:2] == ("resource", "show"):
                return {"tags": {
                    "managed-by": "pg_textsearch-scale"
                }}
            if args[:2] == ("vm", "list-ip-addresses"):
                return [{"virtualMachine": {"network": {
                    "publicIpAddresses": [{"ipAddress": "203.0.113.2"}],
                }}}]
            self.fail(f"unexpected Azure call: {args}")

        fleet = FleetAzure(self.config, self.root, azure=azure)
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        state = fleet._state_path(2, "region-a")
        state.write_text(json.dumps({
            "subscription": "sub",
            "group": "searchbench-fleet-02-region-a",
            "expires": manifest["common_deadline"],
            "location": "region-a",
            "size": "Standard_L32as_v3",
            "status": "provisioned",
            "vm": {
                "computeId": "compute-2",
                "expirationDate": manifest["common_deadline"],
                "provisioningState": "Succeeded",
                "size": "Standard_L32as_v3",
                "networkInterface": {
                    "publicIpAddress": "203.0.113.2",
                },
            },
        }))

        with patch(
            "benchmarks.scale.fleet_azure.subprocess.run"
        ) as run:
            result = fleet._create_host(
                manifest["hosts"][1], "region-a", manifest
            )

        run.assert_not_called()
        self.assertTrue(any(
            call[:3] == ("rest", "--method", "get") for call in calls
        ))
        self.assertEqual(result["status"], "provisioned")
        self.assertEqual(
            result["compute_id"],
            (
                "/subscriptions/sub/resourceGroups/compute/providers/"
                "Microsoft.Compute/virtualMachines/searchbench"
            ),
        )

    def test_stale_saved_vm_is_rejected_by_fresh_azure_readback(self):
        def azure(*args):
            if args[:2] == ("group", "show"):
                return {"tags": {
                    "managed-by": "pg_textsearch-scale"
                }}
            if args[:3] == ("rest", "--method", "get"):
                return {
                    "location": "region-a",
                    "tags": {"managed-by": "pg_textsearch-scale"},
                    "properties": {
                        "computeId": None,
                        "expirationDate": "2026-12-01T00:00:00Z",
                        "provisioningState": "Failed",
                        "size": "Standard_L32as_v3",
                    },
                }
            self.fail(f"unexpected Azure call: {args}")

        fleet = FleetAzure(
            self.config, self.root, azure=azure
        )
        manifest = fleet.prepare_manifest(
            now=dt.datetime(2026, 11, 20, tzinfo=dt.timezone.utc)
        )
        state = fleet._state_path(2, "region-a")
        state.write_text(json.dumps({
            "subscription": "sub",
            "group": "searchbench-fleet-02-region-a",
            "expires": manifest["common_deadline"],
            "location": "region-a",
            "size": "Standard_L32as_v3",
            "status": "provisioned",
            "vm": {
                "computeId": "stale-compute",
                "expirationDate": manifest["common_deadline"],
                "provisioningState": "Succeeded",
                "size": "Standard_L32as_v3",
                "networkInterface": {},
            },
        }))

        with self.assertRaisesRegex(ConfigError, "not provisioned"):
            fleet._create_host(
                manifest["hosts"][1], "region-a", manifest
            )

    def test_remote_python_command_survives_ssh_shell_boundary(self):
        fleet = FleetAzure(self.config, self.root)
        host = {
            "ip": "203.0.113.2",
            "known_hosts": str(self.root / "host2.known_hosts"),
        }
        with patch(
            "benchmarks.scale.fleet_azure.subprocess.run",
            return_value=SimpleNamespace(),
        ) as run:
            fleet._ssh(
                host, "python3", "-c", "print('two words')"
            )
        command = run.call_args.args[0]
        remote_index = command.index("bench@203.0.113.2") + 1
        self.assertEqual(len(command[remote_index:]), 1)
        emitted = command[remote_index]
        self.assertEqual(
            emitted,
            remote_command(("python3", "-c", "print('two words')")),
        )

        result = subprocess.run(
            ["/bin/sh", "-c", emitted],
            check=True,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.stdout, "two words\n")

    def test_nested_raid_topology_is_walked_and_paths_are_deduplicated(self):
        raid = {
            "path": "/dev/md127",
            "type": "raid0",
            "size": 7680,
            "mountpoints": ["/srv/searchbench"],
        }
        topology = {"blockdevices": [
            {
                "path": f"/dev/nvme{i}n1",
                "model": "Microsoft NVMe Direct Disk",
                "type": "disk",
                "size": 1920,
                "children": [raid],
            }
            for i in range(4)
        ]}
        probe = {
            "bootstrap_ready": True,
            "cpu_model": "AMD EPYC",
            "docker_root": "/srv/searchbench/docker",
            "kernel": "test",
            "topology": topology,
        }

        hardware = hardware_from_probe(probe)

        self.assertEqual(hardware["nvme_count"], 4)
        self.assertEqual(hardware["nvme_raid_bytes"], 7680)
        self.assertEqual(hardware["cpu_model"], "AMD EPYC")


if __name__ == "__main__":
    unittest.main()
