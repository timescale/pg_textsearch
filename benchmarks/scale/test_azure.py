import contextlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


MODULE = pathlib.Path(__file__).with_name("azure.py")


class AzureTemplateTests(unittest.TestCase):
    def module(self):
        self.assertTrue(MODULE.exists(), "Azure provisioning is not implemented")
        spec = importlib.util.spec_from_file_location("azure_benchmark", MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def template(self):
        return self.module().template(
            "westus2", "Standard_L32as_v3", "ssh-ed25519 AAAAtest",
            "203.0.113.1/32", "2026-12-03T02:00:00Z",
        )

    def test_native_cloud_expiry_without_role_grants(self):
        resources = self.template()["resources"]
        vm = next(r for r in resources
                  if r["type"] == "Microsoft.DevTestLab/labs/virtualMachines")
        self.assertEqual(vm["properties"]["expirationDate"],
                         "2026-12-03T02:00:00Z")
        self.assertTrue(vm["properties"]["isAuthenticationWithSshKey"])
        self.assertFalse(any("roleAssignments" in r["type"] for r in resources))

    def test_network_is_closed_before_vm_exists(self):
        resources = self.template()["resources"]
        nsg = next(r for r in resources
                   if r["type"] == "Microsoft.Network/networkSecurityGroups")
        rules = nsg["properties"]["securityRules"]
        allow = [r["properties"] for r in rules
                 if r["properties"]["access"] == "Allow"]
        self.assertEqual(len(allow), 1)
        self.assertEqual(allow[0]["destinationPortRange"], "22")
        self.assertEqual(allow[0]["sourceAddressPrefix"], "203.0.113.1/32")
        vnet = next(r for r in resources
                    if r["type"] == "Microsoft.Network/virtualNetworks")
        self.assertIn("networkSecurityGroup", json.dumps(vnet))
        self.assertIn("benchmark-nsg", str(vnet["dependsOn"]))
        labnet = next(r for r in resources
                      if r["type"] == "Microsoft.DevTestLab/labs/virtualNetworks")
        self.assertIn("benchmark-vnet", str(labnet["dependsOn"]))

    def test_refuses_unbounded_or_public_ssh(self):
        for cidr in ("0.0.0.0/0", "*", "Internet"):
            with self.assertRaises(ValueError):
                self.module().template("westus2", "Standard_L32as_v3",
                                       "ssh-ed25519 AAAAtest", cidr,
                                       "2026-12-03T02:00:00Z")

    def test_destroy_releases_lab_owned_locks_before_deleting_group(self):
        lab = ("/subscriptions/test/resourceGroups/searchbench-test/providers/"
               "Microsoft.DevTestLab/labs/searchbench")
        for existing in ((True, True), (True, False), (False, False)):
            with self.subTest(lab_and_vm=existing):
                module = self.module()
                state = {"lab": existing[0], "vm": existing[1], "group": True}

                def azure(subscription, *args):
                    if args[:2] == ("group", "show"):
                        return {"tags": {"managed-by": "pg_textsearch-scale"}}
                    if args[:2] == ("resource", "list"):
                        return [
                            {"id": identifier}
                            for present, identifier in (
                                (state["lab"], lab),
                                (state["vm"], lab + "/virtualMachines/searchbench"),
                            ) if present
                        ]
                    if args[:2] == ("resource", "delete"):
                        target = args[args.index("--ids") + 1]
                        if target == lab:
                            self.assertFalse(state["vm"])
                            state["lab"] = False
                        else:
                            self.assertEqual(target,
                                             lab + "/virtualMachines/searchbench")
                            state["vm"] = False
                        return None
                    if args[:2] == ("group", "delete"):
                        if state["lab"]:
                            raise subprocess.CalledProcessError(
                                1, args, stderr="ScopeLocked: DevTestLabs Lock",
                            )
                        state["group"] = False
                        return None
                    self.fail(f"Unexpected Azure operation: {args}")

                arguments = ["azure.py", "destroy", "--subscription", "test",
                             "--group", "searchbench-test"]
                with patch.object(module, "az", side_effect=azure):
                    with patch.object(sys, "argv", arguments):
                        module.main()
                self.assertEqual(state,
                                 {"lab": False, "vm": False, "group": False})

    def test_azure_failure_exposes_service_error_without_hiding_failure(self):
        module = self.module()
        failure = subprocess.CalledProcessError(
            1, ["az"], stderr="ScopeLocked: DevTestLabs Lock",
        )
        error_output = io.StringIO()
        with patch.object(module.subprocess, "run", side_effect=failure):
            with contextlib.redirect_stderr(error_output):
                with self.assertRaises(subprocess.CalledProcessError):
                    module.az("test", "group", "delete", "-n", "searchbench-test")
        self.assertIn("ScopeLocked: DevTestLabs Lock", error_output.getvalue())

    def test_describe_vm_refuses_unowned_resources_before_reading_ip(self):
        module = self.module()
        calls = []

        def azure(_subscription, *args):
            calls.append(args)
            if args[:2] == ("group", "show"):
                return {"tags": {}}
            self.fail(f"ownership failure should stop Azure reads: {args}")

        with patch.object(module, "az", side_effect=azure):
            with self.assertRaisesRegex(ValueError, "[Rr]esource group"):
                module.describe_vm("test", "searchbench-test")

        self.assertFalse(any(call[:2] == ("vm", "list-ip-addresses")
                             for call in calls))

    def test_describe_vm_requires_owned_dtl_and_compute_resources(self):
        module = self.module()
        compute_id = (
            "/subscriptions/test/resourceGroups/searchbench-compute/"
            "providers/Microsoft.Compute/virtualMachines/searchbench"
        )
        cases = (
            ({"managed-by": module.MARKER}, {}, None, "DevTest Labs VM"),
            ({"managed-by": module.MARKER},
             {"managed-by": module.MARKER}, {}, "compute resource"),
        )
        for group_tags, vm_tags, compute_tags, message in cases:
            with self.subTest(message=message):
                calls = []

                def azure(_subscription, *args):
                    calls.append(args)
                    if args[:2] == ("group", "show"):
                        return {"tags": group_tags}
                    if args[:3] == ("rest", "--method", "get"):
                        return {
                            "tags": vm_tags,
                            "properties": {
                                "computeId": compute_id,
                                "provisioningState": "Succeeded",
                            },
                        }
                    if args[:2] == ("resource", "show"):
                        return {"tags": compute_tags}
                    self.fail(f"unexpected Azure read: {args}")

                with patch.object(module, "az", side_effect=azure):
                    with self.assertRaisesRegex(ValueError, message):
                        module.describe_vm("test", "searchbench-test")
                self.assertFalse(any(
                    call[:2] == ("vm", "list-ip-addresses")
                    for call in calls
                ))

    def test_create_establishes_marker_on_dtl_underlying_compute(self):
        module = self.module()
        compute_id = (
            "/subscriptions/test/resourceGroups/searchbench-compute/"
            "providers/Microsoft.Compute/virtualMachines/searchbench"
        )
        calls = []

        def azure(_subscription, *args):
            calls.append(args)
            if args[:2] == ("group", "show"):
                return {"tags": {"managed-by": module.MARKER}}
            if args[:3] == ("rest", "--method", "get"):
                return {
                    "tags": {"managed-by": module.MARKER},
                    "properties": {"computeId": compute_id},
                }
            if args[:2] == ("tag", "update"):
                return {"properties": {"tags": {
                    "managed-by": module.MARKER,
                    "expires": "2026-12-03T02:00:00Z",
                }}}
            self.fail(f"unexpected Azure operation: {args}")

        with patch.object(module, "az", side_effect=azure):
            module.establish_compute_ownership(
                "test", "searchbench-test", "2026-12-03T02:00:00Z"
            )

        update = next(call for call in calls if call[:2] == (
            "tag", "update"
        ))
        self.assertIn(compute_id, update)
        self.assertIn("managed-by=pg_textsearch-scale", update)

    def test_resume_refuses_unmarked_existing_guest_without_repairing_it(self):
        module = self.module()
        vm_id = (
            "/subscriptions/test/resourceGroups/searchbench-test/providers/"
            "Microsoft.DevTestLab/labs/searchbench/virtualMachines/searchbench"
        )
        calls = []

        def azure(_subscription, *args):
            calls.append(args)
            if args[:2] == ("resource", "list"):
                return [{"id": vm_id}]
            if args[:2] == ("group", "show"):
                return {"tags": {"managed-by": module.MARKER}}
            if args[:3] == ("rest", "--method", "get"):
                return {"tags": {}, "properties": {}}
            self.fail(f"unexpected Azure operation: {args}")

        with patch.object(module, "az", side_effect=azure):
            with self.assertRaisesRegex(ValueError, "DevTest Labs VM"):
                module.validate_existing_guest_ownership(
                    "test", "searchbench-test"
                )

        self.assertFalse(any(
            call[:2] in (("tag", "update"), ("rest", "--method"))
            and "put" in call
            for call in calls
        ))

    def test_launcher_refuses_unaccepted_corpus_before_cloud_allocation(self):
        environment = os.environ | {
            "AZURE_SUBSCRIPTION_ID": "not-a-subscription",
            "AZURE_RESOURCE_GROUP": "searchbench-not-created",
            "CONTROLLER_IPV4": "203.0.113.1",
            "SSH_PRIVATE_KEY": "/nonexistent",
            "MSMARCO_ACCEPT_TERMS": "0",
            "DATASET": "msmarco-v2",
        }
        result = subprocess.run(
            ["bash", str(MODULE.with_name("launch.sh")), "all"],
            env=environment, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("terms acceptance is required before allocating",
                      result.stderr)

    def test_collection_copies_only_the_selected_run_without_provisioning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tools = root / "bin"
            tools.mkdir()
            fixture = root / "remote"
            fixture.mkdir()
            (fixture / "summary.json").write_text('{"complete":true}\n')
            scripts = {
                "python3": (
                    "#!/bin/bash\n"
                    'if [[ "$1" == *azure.py ]]; then\n'
                    '  [[ "$2" == status ]] || exit 76\n'
                    '  echo \'{"networkInterface":{"publicIpAddress":'
                    '"203.0.113.1"},"computeId":"'
                    '/subscriptions/test/resourceGroups/compute/providers/'
                    'Microsoft.Compute/virtualMachines/searchbench"}\'\n'
                    f"else exec {sys.executable} \"$@\"; fi\n"
                ),
                "ssh": "#!/bin/sh\nexit 0\n",
                "rsync": (
                    f"#!{sys.executable}\n"
                    "import os,pathlib,shutil,sys\n"
                    "assert sys.argv[-2].endswith('/runs/test-run/')\n"
                    "shutil.copytree(os.environ['FIXTURE'],sys.argv[-1],"
                    "dirs_exist_ok=True)\n"
                ),
            }
            for name, body in scripts.items():
                path = tools / name
                path.write_text(body)
                path.chmod(0o755)
            result = subprocess.run(
                ["bash", str(MODULE.with_name("launch.sh")), "collect"],
                env=os.environ | {
                    "PATH": f"{tools}:{os.environ['PATH']}",
                    "AZURE_SUBSCRIPTION_ID": "test-subscription",
                    "AZURE_RESOURCE_GROUP": "searchbench-test",
                    "CONTROLLER_IPV4": "203.0.113.1",
                    "SSH_PRIVATE_KEY": str(root / "test-key"),
                    "STATE": str(root / "state.json"),
                    "RESULTS_DIR": str(root / "results"),
                    "FIXTURE": str(fixture), "RUN_ID": "test-run",
                    "DATASET": "smoke",
                }, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            copied = root / "results" / "test-run" / "summary.json"
            self.assertEqual(json.loads(copied.read_text()), {"complete": True})

    def test_setup_syncs_go_helper_and_passes_validated_compute_id(self):
        root = pathlib.Path(
            "benchmarks/scale/.test-launch-bootstrap"
        ).resolve()
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir()
        self.addCleanup(shutil.rmtree, root, True)
        tools = root / "bin"
        tools.mkdir()
        log = root / "calls.jsonl"
        compute_id = (
            "/subscriptions/test/resourceGroups/compute/providers/"
            "Microsoft.Compute/virtualMachines/searchbench"
        )
        scripts = {
            "python3": (
                "#!/bin/bash\n"
                'if [[ "$1" == *azure.py ]]; then\n'
                '  [[ "$2" == status ]] || exit 76\n'
                "  cat <<'EOF'\n"
                '{"networkInterface":{"publicIpAddress":"203.0.113.1"},'
                f'"computeId":"{compute_id}"'
                "}\nEOF\n"
                f"else exec {sys.executable} \"$@\"; fi\n"
            ),
            "ssh": (
                f"#!{sys.executable}\n"
                "import json,os,sys\n"
                "with open(os.environ['CALL_LOG'],'a') as f:\n"
                " f.write(json.dumps(['ssh',*sys.argv[1:]])+'\\n')\n"
            ),
            "scp": (
                f"#!{sys.executable}\n"
                "import json,os,sys\n"
                "with open(os.environ['CALL_LOG'],'a') as f:\n"
                " f.write(json.dumps(['scp',*sys.argv[1:]])+'\\n')\n"
            ),
            "rsync": "#!/bin/sh\nexit 0\n",
        }
        for name, body in scripts.items():
            path = tools / name
            path.write_text(body)
            path.chmod(0o755)

        result = subprocess.run(
            ["bash", str(MODULE.with_name("launch.sh")), "setup"],
            env=os.environ | {
                "PATH": f"{tools}:{os.environ['PATH']}",
                "AZURE_SUBSCRIPTION_ID": "test-subscription",
                "AZURE_RESOURCE_GROUP": "searchbench-test",
                "CONTROLLER_IPV4": "203.0.113.1",
                "SSH_PRIVATE_KEY": str(root / "test-key"),
                "STATE": str(root / "state.json"),
                "CALL_LOG": str(log),
                "DATASET": "smoke",
            },
            capture_output=True, text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        scp = next(call for call in calls if call[0] == "scp")
        self.assertTrue(any(
            argument.endswith("/install_go.sh") for argument in scp
        ))
        bootstrap = next(
            call for call in calls
            if call[0] == "ssh"
            and any("bootstrap.sh" in argument for argument in call)
        )
        self.assertTrue(any(compute_id in argument for argument in bootstrap))

    def test_run_forwards_selected_native_source_to_systemd_worker(self):
        root = pathlib.Path(
            "benchmarks/scale/.test-launch-native-source"
        ).resolve()
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir()
        self.addCleanup(shutil.rmtree, root, True)
        tools = root / "bin"
        tools.mkdir()
        log = root / "ssh.jsonl"
        selected = "a" * 40
        scripts = {
            "python3": (
                "#!/bin/bash\n"
                'if [[ "$1" == *azure.py ]]; then\n'
                '  [[ "$2" == status ]] || exit 76\n'
                "  cat <<'EOF'\n"
                '{"networkInterface":{"publicIpAddress":"203.0.113.1"},'
                '"computeId":"/subscriptions/test/resourceGroups/compute/'
                'providers/Microsoft.Compute/virtualMachines/searchbench"}\n'
                "EOF\n"
                f"else exec {sys.executable} \"$@\"; fi\n"
            ),
            "ssh": (
                f"#!{sys.executable}\n"
                "import json,os,sys\n"
                "with open(os.environ['SSH_LOG'],'a') as f:\n"
                " f.write(json.dumps(sys.argv[1:])+'\\n')\n"
            ),
        }
        for name, body in scripts.items():
            path = tools / name
            path.write_text(body)
            path.chmod(0o755)

        result = subprocess.run(
            ["bash", str(MODULE.with_name("launch.sh")), "run"],
            env=os.environ | {
                "PATH": f"{tools}:{os.environ['PATH']}",
                "AZURE_SUBSCRIPTION_ID": "test-subscription",
                "AZURE_RESOURCE_GROUP": "searchbench-test",
                "CONTROLLER_IPV4": "203.0.113.1",
                "SSH_PRIVATE_KEY": str(root / "test-key"),
                "STATE": str(root / "state.json"),
                "SSH_LOG": str(log),
                "DATASET": "smoke",
                "RUN_ID": "native-source-test",
                "PGTS_NATIVE_SOURCE": selected,
            },
            capture_output=True, text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        worker = next(
            call for call in calls
            if any("systemd-run" in argument for argument in call)
        )
        self.assertTrue(any(
            f"PGTS_NATIVE_SOURCE={selected}" in argument
            for argument in worker
        ))

    def test_invalid_native_source_never_reaches_ssh_or_worker(self):
        root = pathlib.Path(
            "benchmarks/scale/.test-launch-invalid-native-source"
        ).resolve()
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir()
        self.addCleanup(shutil.rmtree, root, True)
        tools = root / "bin"
        tools.mkdir()
        ssh = tools / "ssh"
        ssh.write_text(
            "#!/bin/sh\n"
            f"touch {root / 'ssh-called'}\n"
            "exit 99\n"
        )
        ssh.chmod(0o755)

        result = subprocess.run(
            ["bash", str(MODULE.with_name("launch.sh")), "run"],
            env=os.environ | {
                "PATH": f"{tools}:{os.environ['PATH']}",
                "AZURE_SUBSCRIPTION_ID": "test-subscription",
                "AZURE_RESOURCE_GROUP": "searchbench-test",
                "CONTROLLER_IPV4": "203.0.113.1",
                "SSH_PRIVATE_KEY": str(root / "test-key"),
                "STATE": str(root / "state.json"),
                "DATASET": "smoke",
                "PGTS_NATIVE_SOURCE": "ABC123",
            },
            capture_output=True, text=True,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("40 lowercase hexadecimal", result.stderr)
        self.assertFalse((root / "ssh-called").exists())


if __name__ == "__main__":
    unittest.main()
