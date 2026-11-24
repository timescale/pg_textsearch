import json
import hashlib
import io
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from contextlib import redirect_stdout

from benchmarks.scale.fleet_control import (
    ControlError,
    FleetControl,
    build_partition,
    load_private_config,
    main,
    validate_retained_container,
    verify_frozen_tree,
)
from benchmarks.scale.fleet import fleet_plan
from benchmarks.scale.run import Runner


ENGINES = ("pg_textsearch", "paradedb")
PROFILES = ("high", "constrained32", "constrained16")


def fleet_manifest():
    hosts = []
    number = 1
    for engine in ENGINES:
        for profile in PROFILES:
            hosts.append({
                "host": f"host{number}",
                "engine": engine,
                "profile": profile,
                "ip": f"192.0.2.{number}",
                "known_hosts": f"/private/known-hosts-{number}",
                "bootstrap_ready": True,
                "expiration": "2026-11-27T00:00:00Z",
            })
            number += 1
    return {
        "status": "ready",
        "hosts": hosts,
        "common_deadline": "2026-11-27T00:00:00Z",
    }


def private_config(root):
    return {
        "subscription": "private",
        "group_prefix": "private",
        "regions": ["region-a", "region-b"],
        "size": "Standard_L32as_v3",
        "lifetime_days": 7,
        "ssh_private_key": "/private/key",
        "ssh_public_key": "/private/key.pub",
        "ssh_cidr": "192.0.2.0/24",
        "root": "/srv/searchbench/pg_textsearch-scale",
        "remote_repo": "/opt/searchbench/campaign",
        "source_ref": "94e7f5c1",
        "campaign_id": "repeatable-v1",
        "sql_shape": "filter_first",
        "state_dir": str(root),
    }


class FakeBackend:
    def __init__(self):
        self.calls = []
        self.hashes = {}
        self.fail_transfer = None

    def azure(self, action, config_path, state_dir):
        self.calls.append(("azure", action))

    def deploy_source(self, host, source_ref, remote_repo):
        self.calls.append((
            "deploy_source", host["host"], source_ref, remote_repo,
        ))

    def run(self, host, command):
        self.calls.append(("run", host["host"], command))
        return ""

    def sha256(self, host, path):
        return self.hashes.get((host["host"], path))

    def stream(self, source, target, source_path, target_path, digest):
        self.calls.append((
            "stream", source["host"], target["host"], source_path,
        ))
        if self.fail_transfer == (target["host"], source_path):
            raise ControlError("injected transfer failure")
        self.hashes[(target["host"], target_path)] = digest

    def read_json(self, host, path):
        raise AssertionError(f"unexpected read: {host['host']} {path}")


class FleetControlTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = self.root / "state"
        self.state.mkdir()
        self.config_path = self.root / "private.json"
        self.config_path.write_text(json.dumps(
            private_config(self.state)
        ))
        (self.state / "fleet-manifest.json").write_text(
            json.dumps(fleet_manifest())
        )

    def control(self, backend=None):
        return FleetControl(
            self.config_path,
            backend=backend or FakeBackend(),
        )

    def run_native_import(
        self, *, tag_id="", list_status=0, proof_id="sha256:locked",
        include_volume=False,
    ):
        repository = self.root / "repository"
        install = repository / "benchmarks/scale/install.sh"
        install.parent.mkdir(parents=True)
        install.write_text("#!/bin/sh\nexit 0\n")
        install.chmod(0o755)
        artifacts = self.root / "artifacts"
        artifacts.mkdir()
        source = artifacts / "frozen-source.json"
        source.write_text("{}\n")
        image = artifacts / "native-image.tar.zst"
        image.write_bytes(b"image")
        config = private_config(self.state) | {
            "root": str(self.root / "worker"),
            "remote_repo": str(repository),
        }
        self.config_path.write_text(json.dumps(config))
        control = self.control()
        proof = {
            "id": "sha256:locked",
            "architecture": "amd64",
            "os": "linux",
            "created": "2026-01-01T00:00:00Z",
            "rootfs_layers": ["sha256:layer"],
        }
        imported = [
            {
                "kind": "source_manifest",
                "path": str(source),
                "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            },
            {
                "kind": "native_image",
                "path": str(image),
                "image_id": "sha256:locked",
                "image_proof": proof,
            },
        ]
        if include_volume:
            volume = artifacts / "volume.tar.zst"
            volume.write_bytes(b"volume")
            imported.append({
                "kind": "volume",
                "path": str(volume),
                "volume": {
                    "name": "native-volume",
                    "labels": {"managed-by": "pg_textsearch-tin"},
                },
            })

        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        log = self.root / "docker.jsonl"
        docker = bin_dir / "docker"
        docker.write_text(
            "#!/usr/bin/env python3\n"
            "import json,os,sys\n"
            "args=sys.argv[1:]\n"
            "with open(os.environ['DOCKER_LOG'],'a') as stream:\n"
            " stream.write(json.dumps(args)+'\\n')\n"
            "if args[:2]==['ps','-q']:\n"
            " raise SystemExit\n"
            "if args[:2]==['load']:\n"
            " sys.stdin.buffer.read()\n"
            " print('Loaded image')\n"
            " raise SystemExit\n"
            "if args[:2]==['image','inspect']:\n"
            " print(json.dumps([json.loads(os.environ['IMAGE_PROOF'])]))\n"
            " raise SystemExit\n"
            "if args[:2]==['image','ls']:\n"
            " status=int(os.environ['LIST_STATUS'])\n"
            " if status:\n"
            "  raise SystemExit(status)\n"
            " if os.environ.get('TAG_ID'):\n"
            "  print(os.environ['TAG_ID'])\n"
            " raise SystemExit\n"
            "if args[:2]==['image','tag']:\n"
            " raise SystemExit\n"
            "if args[:2]==['volume','inspect']:\n"
            " raise SystemExit(1)\n"
            "if args[:2]==['volume','create']:\n"
            " print(args[-1])\n"
            " raise SystemExit\n"
            "raise SystemExit('unexpected docker command: '+repr(args))\n"
        )
        sudo = bin_dir / "sudo"
        sudo.write_text("#!/bin/sh\nshift\nexec \"$@\"\n")
        zstd = bin_dir / "zstd"
        zstd.write_text("#!/bin/sh\ncat \"$2\"\n")
        python3 = bin_dir / "python3"
        python3.write_text(
            "#!/bin/sh\n"
            "if test \"$1\" = -m && test \"$2\" = venv; then\n"
            " mkdir -p \"$3/bin\"\n"
            " printf '#!/bin/sh\\nexit 0\\n' > \"$3/bin/pip\"\n"
            " chmod +x \"$3/bin/pip\"\n"
            " exit 0\n"
            "fi\n"
            "exec " + shlex.quote(sys.executable) + " \"$@\"\n"
        )
        for executable in (docker, sudo, zstd, python3):
            executable.chmod(0o755)
        actual_proof = proof | {"id": proof_id}
        environment = os.environ | {
            "PATH": str(bin_dir) + os.pathsep + os.environ["PATH"],
            "DOCKER_LOG": str(log),
            "IMAGE_PROOF": json.dumps({
                "Id": actual_proof["id"],
                "Architecture": actual_proof["architecture"],
                "Os": actual_proof["os"],
                "Created": actual_proof["created"],
                "RootFS": {"Layers": actual_proof["rootfs_layers"]},
            }),
            "TAG_ID": tag_id,
            "LIST_STATUS": str(list_status),
        }
        result = subprocess.run(
            ["bash", "-c", control._import_command(
                control.host("host1"), imported
            )],
            env=environment,
            capture_output=True,
            text=True,
        )
        calls = [
            json.loads(line) for line in log.read_text().splitlines()
        ]
        return control, result, calls

    def test_native_import_tags_verified_image_when_tag_is_absent(self):
        control, result, calls = self.run_native_import()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn([
            "image", "tag", "sha256:locked",
            f"pg_textsearch-scale:{control.config['native_source']}",
        ], calls)

    def test_native_import_accepts_existing_tag_for_locked_image(self):
        _, result, calls = self.run_native_import(
            tag_id="sha256:locked"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(
            call[:2] == ["image", "tag"] for call in calls
        ))

    def test_native_import_rejects_foreign_tag_before_volume_mutation(self):
        _, result, calls = self.run_native_import(
            tag_id="sha256:foreign", include_volume=True
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(
            call[:2] == ["image", "tag"]
            or call[:2] == ["volume", "create"]
            for call in calls
        ))

    def test_native_import_aborts_when_tag_listing_fails(self):
        _, result, calls = self.run_native_import(
            list_status=42, include_volume=True
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(
            call[:2] == ["image", "tag"]
            or call[:2] == ["volume", "create"]
            for call in calls
        ))

    def test_native_import_rejects_proof_mismatch_before_tag_review(self):
        _, result, calls = self.run_native_import(
            proof_id="sha256:different"
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(
            call[:2] in (["image", "ls"], ["image", "tag"])
            for call in calls
        ))

    def test_native_selection_reaches_build_export_and_workers(self):
        candidate = "0123456789abcdef0123456789abcdef01234567"
        config = private_config(self.state) | {"native_source": candidate}
        self.config_path.write_text(json.dumps(config))
        backend = FakeBackend()
        control = self.control(backend)
        control.write_stage("provision", {"status": "complete"})
        control.prepare()
        setup = next(call[2] for call in backend.calls if call[0] == "run")
        self.assertIn(f"PGTS_NATIVE_SOURCE={candidate}", setup)
        self.assertIn(candidate, control._export_command())
        self.assertIn(
            f"PGTS_NATIVE_SOURCE={candidate}",
            control._worker_command(control.host("host2"), "run"),
        )
        self.assertIn(
            candidate,
            control._worker_command(control.host("host2"), "preflight"),
        )

    def test_changed_native_selection_cannot_resume_completed_stages(self):
        control = self.control()
        control.write_stage("prepare", {"status": "complete"})
        config = private_config(self.state) | {"native_source": "a" * 40}
        self.config_path.write_text(json.dumps(config))
        changed = self.control()
        with self.assertRaisesRegex(ControlError, "selection.*resume"):
            changed.prepare()
        self.assertEqual(changed.backend.calls, [])

    def test_explicit_null_native_source_is_not_a_default_selection(self):
        config = private_config(self.state) | {"native_source": None}
        self.config_path.write_text(json.dumps(config))
        with self.assertRaisesRegex(ControlError, "PGTS_NATIVE_SOURCE"):
            self.control()

    def test_schema_can_pin_current_harness_native_and_sql_shape(self):
        output = io.StringIO()
        with redirect_stdout(output):
            result = main([
                "schema", "--source-ref", "a" * 40,
                "--native-source", "b" * 40, "--sql-shape", "original",
            ])
        self.assertEqual(result, 0)
        config = json.loads(output.getvalue())
        self.assertEqual(config["source_ref"], "a" * 40)
        self.assertEqual(config["native_source"], "b" * 40)
        self.assertEqual(config["sql_shape"], "original")

    def test_original_sql_shape_reaches_partition_and_workers(self):
        config = private_config(self.state) | {"sql_shape": "original"}
        self.config_path.write_text(json.dumps(config))
        control = self.control()
        partition = build_partition(control.config, fleet_manifest())
        self.assertTrue(all(
            row["cell_id"].endswith(".shape-original") for row in partition
        ))
        self.assertIn(
            "PGTS_SQL_SHAPE=original",
            control._worker_command(control.host("host1"), "run"),
        )

    def test_source_preparation_writes_the_same_plan_as_workers(self):
        backend = FakeBackend()
        control = self.control(backend)
        control.write_stage("provision", {"status": "complete"})
        control.prepare()
        command = next(call[2] for call in backend.calls if call[0] == "run")
        script = command.split("<<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
        captured = []

        def make_runner(repo, root, smoke, run_id, dataset, dataset_path):
            runner = Mock()
            runner.dataset = self.root / dataset
            runner.dataset.mkdir(exist_ok=True)
            (runner.dataset / "manifest.json").write_text("{}")
            runner.tin_dir = self.root / run_id
            runner.tin_dir.mkdir(exist_ok=True)
            runner._write_or_validate_json = Runner._write_or_validate_json
            captured.append((dataset, runner.tin_dir / "plan.json"))
            return runner

        with (
            patch("benchmarks.scale.tin.TinRunner", side_effect=make_runner),
            patch("benchmarks.scale.tin.Runner"),
            patch.dict(os.environ, {
                "TIN_ROOT": str(self.root),
                "TIN_REPO": str(self.root),
                "MSMARCO_RUN_ID": "ms-plan",
                "WIKIPEDIA_RUN_ID": "wiki-plan",
                "PGTS_SQL_SHAPE": "filter_first",
            }),
        ):
            exec(compile(script, "<source-prepare>", "exec"), {})
        for dataset, path in captured:
            if path.is_file():
                self.assertEqual(
                    json.loads(path.read_text()), fleet_plan(dataset)
                )

    def test_preflight_accepts_the_selected_released_native_version(self):
        config = private_config(self.state) | {"root": str(self.root)}
        self.config_path.write_text(json.dumps(config))
        control = self.control()
        command = control._worker_command(control.host("host1"), "preflight")
        script = command.split("<<'PY'\n", 1)[1].split("\nPY", 1)[0]

        def make_runner(repo, root, smoke, run_id, dataset, dataset_path):
            runner = Mock()
            runner.tin_dir = self.root / run_id / "tin"
            runner.tin_dir.mkdir(parents=True, exist_ok=True)
            runner._images.return_value = ({"pg_textsearch": "image"}, {})
            runner._volume_name.return_value = "volume"
            return runner

        def output(argv, **kwargs):
            if argv[:3] == ["docker", "ps", "-q"]:
                return ""
            if argv[:2] == ["docker", "exec"]:
                return "pg_textsearch\t1.6.0\nt\n"
            if argv[0] in ("lscpu", "lsblk"):
                return "{}"
            return "owned"

        with (
            patch("benchmarks.scale.tin.TinRunner", side_effect=make_runner),
            patch("benchmarks.scale.fleet.validate_imported_inputs"),
            patch("subprocess.check_output", side_effect=output),
            patch("os.cpu_count", return_value=32),
            patch.dict(os.environ, {
                "PGTS_NATIVE_SOURCE": control.config["native_source"],
                "PGTS_SQL_SHAPE": control.config["sql_shape"],
            }),
            redirect_stdout(io.StringIO()),
        ):
            exec(compile(script, "<preflight>", "exec"), {})
        result = self.root / "fleet-control/preflight-pg_textsearch-high.json"
        self.assertEqual(json.loads(result.read_text())["status"], "ok")

    def test_resume_relaunches_only_hosts_with_missing_cells(self):
        backend = FakeBackend()
        control = self.control(backend)
        control.write_stage("preflight", {"status": "complete"})
        control.write_stage("run", {"status": "complete", "hosts": ["host2"]})
        missing = next(
            row for row in build_partition(control.config, fleet_manifest())
            if row["host"] == "host2"
        )
        control.status = Mock(return_value={"missing": [[
            missing["dataset"], missing["engine"], missing["cell_id"],
        ]]})
        control.run()
        self.assertEqual(
            [call[1] for call in backend.calls if call[0] == "run"], ["host2"]
        )
        backend.calls.clear()
        control.status.return_value = {"missing": []}
        control.run()
        self.assertEqual(backend.calls, [])

    def test_loaded_failed_worker_is_restarted_without_starting_another_unit(
        self,
    ):
        config = private_config(self.state) | {
            "root": str(self.root / "artifacts")
        }
        self.config_path.write_text(json.dumps(config))
        control = self.control()
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        log = self.root / "systemctl.jsonl"
        systemctl = bin_dir / "systemctl"
        systemctl.write_text(
            "#!/usr/bin/env python3\n"
            "import json,os,sys\n"
            "with open(os.environ['SYSTEMCTL_LOG'],'a') as f:\n"
            " f.write(json.dumps(sys.argv[1:])+'\\n')\n"
            "if sys.argv[1]=='show':\n"
            " print(os.environ['SYSTEMCTL_DESCRIPTION'] "
            "if 'Description' in sys.argv else "
            "('failed' if '--value' in sys.argv else 'ActiveState=failed'))\n"
        )
        sudo = bin_dir / "sudo"
        sudo.write_text("#!/bin/sh\nshift\nexec \"$@\"\n")
        systemctl.chmod(0o755)
        sudo.chmod(0o755)
        command = control._worker_command(control.host("host2"), "run")
        match = re.search(r"--description=([A-Za-z0-9_:-]+)", command)
        environment = os.environ | {
            "PATH": str(bin_dir) + os.pathsep + os.environ["PATH"],
            "SYSTEMCTL_LOG": str(log),
            "SYSTEMCTL_DESCRIPTION": match[1] if match else "legacy",
        }
        subprocess.run(
            ["bash", "-c", command], env=environment,
            check=True, capture_output=True, text=True,
        )
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(
            [call for call in calls if call[0] == "restart"],
            [["restart", "pgts-tin-fleet-pg-textsearch-constrained32"]],
        )
        log.write_text("")
        foreign = subprocess.run(
            ["bash", "-c", command],
            env=environment | {"SYSTEMCTL_DESCRIPTION": "foreign"},
            capture_output=True, text=True,
        )
        self.assertNotEqual(foreign.returncode, 0)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertFalse(any(call[0] == "restart" for call in calls))

    def test_partition_is_disjoint_complete_and_bound_to_six_hosts(self):
        config = load_private_config(self.config_path)
        partition = build_partition(config, fleet_manifest())

        self.assertEqual(len(partition), 174)
        keys = {
            (row["dataset"], row["engine"], row["cell_id"])
            for row in partition
        }
        self.assertEqual(len(keys), 174)
        self.assertEqual(
            {row["host"] for row in partition},
            {f"host{number}" for number in range(1, 7)},
        )
        for host in fleet_manifest()["hosts"]:
            assigned = [
                row for row in partition if row["host"] == host["host"]
            ]
            self.assertEqual(len(assigned), 29)
            self.assertTrue(all(
                row["engine"] == host["engine"]
                and row["profile"] == host["profile"]
                for row in assigned
            ))

    def test_prepare_deploys_frozen_source_before_fresh_setup(self):
        backend = FakeBackend()
        control = self.control(backend)
        control.write_stage("provision", {
            "status": "complete",
            "hosts": [f"host{number}" for number in range(1, 7)],
        })

        control.prepare()

        self.assertEqual(backend.calls[0][0], "deploy_source")
        self.assertEqual(backend.calls[1][0], "run")
        command = backend.calls[1][2]
        self.assertIn("benchmarks/scale/install.sh", command)
        self.assertIn("python3 -m venv", command)
        self.assertIn("requirements.txt", command)
        subprocess.run(
            ["bash", "-n"],
            input=command,
            text=True,
            check=True,
        )
        script = re.search(
            r"<<'PY'\n(.*)\nPY\n", command, re.DOTALL
        ).group(1)
        compile(script, "<fresh-source-setup>", "exec")

    def test_generated_prepare_uses_canonical_tin_apis(self):
        repository = Path(__file__).resolve().parents[3]
        artifacts = self.root / "artifacts"
        config = private_config(self.state)
        config["root"] = str(artifacts)
        config["remote_repo"] = str(repository)
        self.config_path.write_text(json.dumps(config))
        backend = FakeBackend()
        control = self.control(backend)
        run_ids = (
            control.config["msmarco_run_id"],
            control.config["wikipedia_run_id"],
        )
        control.write_stage("provision", {
            "status": "complete",
            "hosts": [f"host{number}" for number in range(1, 7)],
        })
        control.prepare()
        command = backend.calls[1][2]
        script = re.search(
            r"<<'PY'\n(.*)\nPY\n", command, re.DOTALL
        ).group(1)

        datasets = (
            (
                "msmarco-v2",
                138_364_198,
                {
                    "dataset": "msmarco-v2",
                    "source_md5": "05946bac48a8ffee62e160213eab3fda",
                },
            ),
            (
                "wikipedia-20231101-en",
                6_407_814,
                {
                    "dataset": "wikipedia-20231101-en",
                    "revision": (
                        "b04c8d1ceb2f5cd4588862100d08de323dccfbaa"
                    ),
                    "source": (
                        "https://huggingface.co/datasets/"
                        "wikimedia/wikipedia"
                    ),
                },
            ),
        )
        for name, rows, provenance in datasets:
            dataset = artifacts / name
            data = dataset / "data"
            queries = dataset / "k6"
            data.mkdir(parents=True)
            queries.mkdir()
            shard = data / "fixture"
            shard.write_bytes(b"fixture")
            manifest = {
                "complete": True,
                "rows": rows,
                "columns": {
                    "id": "int64",
                    "source_id": "string",
                    "body": "string",
                },
                "shards": [{
                    "file": shard.name,
                    "rows": rows,
                    "sha256": hashlib.sha256(
                        shard.read_bytes()
                    ).hexdigest(),
                }],
                **provenance,
            }
            (dataset / "manifest.json").write_text(
                json.dumps(manifest)
            )
            for pack in ("natural", "spans"):
                (queries / f"{pack}.json").write_text(json.dumps([
                    {"id": f"{pack}-1", "text": "search terms"},
                ]))
            (queries / "update_targets.json").write_text("[1]\n")

        import benchmarks.scale.tin as tin_module
        from benchmarks.scale.run import Runner
        from benchmarks.scale.tin import TinRunner

        environment = {
            "TIN_ROOT": str(artifacts),
            "TIN_REPO": str(repository),
            "MSMARCO_RUN_ID": run_ids[0],
            "WIKIPEDIA_RUN_ID": run_ids[1],
            "MSMARCO_ACCEPT_TERMS": "1",
        }
        with (
            patch.dict(os.environ, environment, clear=False),
            patch.object(tin_module, "Runner", Runner),
            patch.object(TinRunner, "install"),
            patch.object(TinRunner, "build_selected"),
        ):
            exec(compile(
                script, "<generated-fresh-prepare>", "exec"
            ), {"__name__": "__main__"})

        for run_id in run_ids:
            self.assertTrue(
                (
                    artifacts / "runs" / run_id
                    / "tin" / "plan.json"
                ).is_file()
            )

    def test_frozen_tree_rejects_same_commit_with_modified_overlay(self):
        tree = self.root / "tree"
        tree.mkdir()
        (tree / "fleet.py").write_text("expected")
        expected = {
            "fleet.py": __import__("hashlib").sha256(
                b"expected"
            ).hexdigest(),
        }
        verify_frozen_tree(tree, expected)

        (tree / "fleet.py").write_text("modified")
        with self.assertRaisesRegex(ControlError, "frozen source"):
            verify_frozen_tree(tree, expected)

    def test_partition_rejects_manifest_without_common_expiry(self):
        config = load_private_config(self.config_path)
        manifest = fleet_manifest()
        del manifest["hosts"][0]["expiration"]

        with self.assertRaisesRegex(ControlError, "expiration"):
            build_partition(config, manifest)

    def test_partition_accepts_equivalent_utc_expiration_spellings(self):
        config = load_private_config(self.config_path)
        manifest = fleet_manifest()
        for host in manifest["hosts"]:
            host["expiration"] = "2026-11-27T00:00:00+00:00"

        partition = build_partition(config, manifest)

        self.assertEqual(len(partition), 174)

    def test_partition_rejects_different_expiration_instant(self):
        config = load_private_config(self.config_path)
        manifest = fleet_manifest()
        manifest["hosts"][0]["expiration"] = (
            "2026-11-27T00:00:01+00:00"
        )

        with self.assertRaisesRegex(ControlError, "does not match"):
            build_partition(config, manifest)

    def test_partition_rejects_naive_expiration_timestamps(self):
        config = load_private_config(self.config_path)
        for field in ("common_deadline", "host"):
            manifest = fleet_manifest()
            if field == "common_deadline":
                manifest["common_deadline"] = "2026-11-27T00:00:00"
            else:
                manifest["hosts"][0]["expiration"] = (
                    "2026-11-27T00:00:00"
                )
            with self.subTest(field=field):
                with self.assertRaisesRegex(
                    ControlError, "expiration.*invalid"
                ):
                    build_partition(config, manifest)

    def test_completed_transfer_is_reused_and_corruption_is_restreamed(self):
        backend = FakeBackend()
        control = self.control(backend)
        source = fleet_manifest()["hosts"][0]
        target = fleet_manifest()["hosts"][1]
        artifact = {
            "name": "database.tar.zst",
            "path": "/dist/database.tar.zst",
            "sha256": "abc123",
        }
        backend.hashes[(target["host"], artifact["path"])] = "abc123"

        self.assertEqual(
            control.transfer(source, target, artifact), "reused"
        )
        self.assertEqual(backend.calls, [])

        backend.hashes[(target["host"], artifact["path"])] = "corrupt"
        self.assertEqual(
            control.transfer(source, target, artifact), "transferred"
        )
        self.assertEqual(backend.calls[0][0], "stream")

    def test_staging_host_import_waits_for_every_fanout_reader(self):
        backend = FakeBackend()
        control = self.control(backend)
        control.write_stage("stage", {
            "status": "complete",
            "staging": "host4",
            "artifacts": [{
                "name": "metadata.tar.zst",
                "path": "/dist/metadata.tar.zst",
                "sha256": "digest",
                "kind": "metadata",
                "engines": list(ENGINES),
            }, {
                "name": "frozen-source.json",
                "path": "/dist/frozen-source.json",
                "sha256": "source-digest",
                "kind": "source_manifest",
                "engines": list(ENGINES),
            }],
        })

        control.distribute()

        events = backend.calls
        host4_import = next(
            index for index, event in enumerate(events)
            if event[:2] == ("run", "host4")
        )
        fanout_streams = [
            index for index, event in enumerate(events)
            if event[0] == "stream" and event[2] != "host4"
        ]
        self.assertTrue(fanout_streams)
        self.assertGreater(host4_import, max(fanout_streams))

    def test_failed_transfer_blocks_distribution_checkpoint_and_resumes(self):
        backend = FakeBackend()
        control = self.control(backend)
        artifact = {
            "name": "metadata.tar.zst",
            "path": "/dist/metadata.tar.zst",
            "sha256": "digest",
            "kind": "metadata",
            "engines": list(ENGINES),
        }
        source_manifest = {
            "name": "frozen-source.json",
            "path": "/dist/frozen-source.json",
            "sha256": "source-digest",
            "kind": "source_manifest",
            "engines": list(ENGINES),
        }
        control.write_stage("stage", {
            "status": "complete",
            "staging": "host4",
            "artifacts": [artifact, source_manifest],
        })
        backend.fail_transfer = ("host2", artifact["path"])

        with self.assertRaisesRegex(ControlError, "transfer failure"):
            control.distribute()
        self.assertFalse(control.stage_path("distribute").is_file())
        self.assertTrue(
            (
                control.state_dir
                / "control/distribute-error.json"
            ).is_file()
        )

        backend.fail_transfer = None
        control.distribute()
        state = json.loads(
            control.stage_path("distribute").read_text()
        )
        self.assertEqual(state["status"], "complete")
        call_count = len(backend.calls)
        control.distribute()
        self.assertEqual(len(backend.calls), call_count)

    def test_paradedb_workers_never_receive_native_image_archive(self):
        control = self.control()
        host = next(
            host for host in fleet_manifest()["hosts"]
            if host["engine"] == "paradedb"
        )
        artifacts = [
            {
                "name": "native-image.tar.zst",
                "kind": "native_image",
                "engines": ["pg_textsearch"],
            },
            {
                "name": "metadata.tar.zst",
                "kind": "metadata",
                "engines": list(ENGINES),
            },
        ]

        selected = control._artifacts_for_host(host, artifacts)

        self.assertEqual(
            [artifact["name"] for artifact in selected],
            ["metadata.tar.zst"],
        )

    def test_native_import_requires_exported_lock_metadata(self):
        control = self.control()
        host = fleet_manifest()["hosts"][0]
        source_manifest = {
            "kind": "source_manifest",
            "path": "/dist/frozen-source.json",
            "sha256": "source",
        }
        image = {
            "kind": "native_image",
            "path": "/dist/native-image.tar.zst",
        }
        with self.assertRaisesRegex(ControlError, "locked image ID"):
            control._import_command(
                host, [source_manifest, image]
            )

        image["image_id"] = "sha256:locked"
        image["image_proof"] = {
            "id": "sha256:locked",
            "architecture": "amd64",
            "os": "linux",
            "created": "2026-01-01T00:00:00Z",
            "rootfs_layers": ["sha256:layer"],
        }
        command = control._import_command(
            host, [source_manifest, image]
        )
        self.assertIn("docker load", command)

    def test_collection_rejects_duplicate_or_missing_assignments(self):
        control = self.control()
        partition = control.plan()["partition"]
        rows = [
            {
                "dataset": row["dataset"],
                "engine": row["engine"],
                "cell_id": row["cell_id"],
                "host": row["host"],
                "status": "ok",
            }
            for row in partition
        ]

        summary = control.reconcile(rows)
        self.assertEqual(summary["observed"], 174)
        self.assertEqual(summary["missing"], [])
        self.assertEqual(summary["duplicates"], [])

        with self.assertRaisesRegex(ControlError, "partition mismatch"):
            control.reconcile(rows[:-1] + [rows[0]])

    def test_wait_polls_until_full_terminal_coverage(self):
        control = self.control()
        control.status = Mock(side_effect=[
            {
                "missing": [("msmarco-v2", "pg_textsearch", "cell")],
                "services": {
                    "host1": {
                        "ActiveState": "active",
                        "ExecMainStatus": "0",
                    },
                },
            },
            {
                "missing": [],
                "observed": 174,
                "services": {},
            },
        ])

        with patch(
            "benchmarks.scale.fleet_control.time.sleep"
        ) as sleep:
            result = control.wait()

        self.assertEqual(result["observed"], 174)
        sleep.assert_called_once_with(60)

    def test_wait_fails_immediately_when_required_host_is_unreachable(self):
        control = self.control()
        missing = control.plan()["partition"][0]
        control.status = Mock(return_value={
            "missing": [(
                missing["dataset"],
                missing["engine"],
                missing["cell_id"],
            )],
            "observed": 173,
            "services": {
                missing["host"]: {"error": "ssh unreachable"},
            },
            "host_errors": {
                missing["host"]: "ssh unreachable",
            },
        })

        with patch(
            "benchmarks.scale.fleet_control.time.sleep",
            side_effect=AssertionError("must not sleep"),
        ):
            with self.assertRaisesRegex(
                ControlError, "unreachable"
            ):
                control.wait()

    def test_retained_container_reuse_rejects_foreign_ownership(self):
        info = {
            "Config": {
                "Image": "sha256:image",
                "Labels": {
                    "managed-by": "foreign",
                    "source-volume": "volume-a",
                },
            },
            "Mounts": [{"Name": "volume-a"}],
            "HostConfig": {
                "PortBindings": {
                    "5432/tcp": [{
                        "HostIp": "127.0.0.1",
                        "HostPort": "16432",
                    }],
                },
            },
        }

        with self.assertRaisesRegex(ControlError, "retained container"):
            validate_retained_container(
                info,
                image="sha256:image",
                volume="volume-a",
                port=16432,
            )

    def test_generated_remote_programs_parse(self):
        control = self.control()
        host = fleet_manifest()["hosts"][0]
        commands = [
            control._export_command(),
            control._worker_command(host, "preflight"),
            control._worker_command(host, "retain"),
            control._status_command(host),
            control._import_command(host, [{
                "kind": "source_manifest",
                "path": "/dist/frozen-source.json",
                "sha256": "source",
            }]),
        ]
        for command in commands:
            subprocess.run(
                ["bash", "-n"],
                input=command,
                text=True,
                check=True,
            )
            for number, script in enumerate(re.findall(
                r"<<'PY'\n(.*?)\nPY", command, re.DOTALL
            )):
                compile(
                    script,
                    f"<remote-program-{number}>",
                    "exec",
                )


if __name__ == "__main__":
    unittest.main()
