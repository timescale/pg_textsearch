import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from benchmarks.scale.retain_originals import (
    require_idle,
    restore,
    validate_image_layout,
    validate_index_proof,
    validate_plan,
    validate_runtime,
)


class RetainOriginalsTests(unittest.TestCase):
    def setUp(self):
        self.plan = {
            "idle_units": ["pgts-tin-fleet-pg-textsearch-high"],
            "databases": [{
                "volume": "original-index",
                "engine": "pg_textsearch",
                "image": "sha256:" + "a" * 64,
                "port": 16435,
                "labels": {
                    "managed-by": "pg_textsearch-tin",
                    "run-id": "historical-run",
                    "engine": "pg_textsearch",
                    "kind": "analyzer",
                },
                "expect_search_index": True,
            }],
        }

    def test_refuses_foreign_and_disposable_volumes(self):
        for change in (
            {"managed-by": "somebody-else"},
            {"kind": "write-attempt"},
            {"engine": "paradedb"},
        ):
            with self.subTest(change=change):
                plan = copy.deepcopy(self.plan)
                plan["databases"][0]["labels"].update(change)
                with self.assertRaises(ValueError):
                    validate_plan(plan)

    def test_refuses_ambiguous_volume_or_port_assignments(self):
        for field in ("port", "volume"):
            with self.subTest(field=field):
                plan = copy.deepcopy(self.plan)
                other = copy.deepcopy(plan["databases"][0])
                other.update(volume="another-index", port=16436)
                other[field] = plan["databases"][0][field]
                plan["databases"].append(other)
                with self.assertRaises(ValueError):
                    validate_plan(plan)

    def test_requires_pinned_images_and_explicit_index_expectations(self):
        for field, value in (
            ("image", "postgres:latest"),
            ("port", 0),
            ("expect_search_index", "yes"),
        ):
            with self.subTest(field=field):
                plan = copy.deepcopy(self.plan)
                plan["databases"][0][field] = value
                with self.assertRaises(ValueError):
                    validate_plan(plan)
        validate_plan(self.plan)

    def test_running_worker_blocks_retention(self):
        with patch(
            "benchmarks.scale.retain_originals.subprocess.run",
            return_value=subprocess.CompletedProcess([], 0),
        ):
            with self.assertRaisesRegex(ValueError, "active"):
                require_idle(self.plan["idle_units"])

    def test_incompatible_image_cannot_initialize_another_data_directory(self):
        for env in (
            ["PG_MAJOR=17", "PGDATA=/var/lib/postgresql/data"],
            ["PG_MAJOR=18", "PGDATA=/var/lib/postgresql/data"],
            [],
        ):
            with self.subTest(env=env):
                with self.assertRaises(ValueError):
                    validate_image_layout({"Config": {"Env": env}})
        validate_image_layout({"Config": {"Env": [
            "PG_MAJOR=18", "PGDATA=/var/lib/postgresql/18/docker",
        ]}})

    def test_wrong_name_or_body_column_is_not_the_original_search_index(self):
        proof = {
            "count": 1, "valid": True,
            "expected_name": True, "body_indexed": True,
        }
        for field in ("valid", "expected_name", "body_indexed"):
            with self.subTest(field=field):
                with self.assertRaises(ValueError):
                    validate_index_proof({**proof, field: False}, True)
        validate_index_proof(proof, True)
        with self.assertRaises(ValueError):
            validate_index_proof({**proof, "count": 2}, True)
        validate_index_proof({"count": 0, "valid": True}, False)

    def runtime_fixture(self):
        image = {"Config": {
            "Env": ["PG_MAJOR=18", "PGDATA=/var/lib/postgresql/18/docker"],
            "Entrypoint": ["docker-entrypoint.sh"],
            "User": "",
        }}
        cmd = [
            "-c", "shared_buffers=1GB",
            "-c", "maintenance_work_mem=256MB",
            "-c", "random_page_cost=1.1",
            "-c", "shared_preload_libraries=pg_textsearch",
        ]
        container = {
            "Config": {
                **copy.deepcopy(image["Config"]),
                "Cmd": cmd,
            },
            "Path": "docker-entrypoint.sh",
            "Args": list(cmd),
            "Mounts": [{
                "Type": "volume", "Name": "original-index",
                "Destination": "/var/lib/postgresql", "RW": True,
            }],
        }
        container["Config"]["Env"].append("POSTGRES_PASSWORD=postgres")
        return container, image

    def test_existing_runtime_cannot_redirect_or_initialize_data(self):
        for field, value in (
            ("Env", ["PG_MAJOR=18", "PGDATA=/different"]),
            ("Cmd", ["postgres", "-D", "/different"]),
            ("Entrypoint", ["sh"]),
        ):
            with self.subTest(field=field):
                container, image = self.runtime_fixture()
                container["Config"][field] = value
                with self.assertRaises(ValueError):
                    validate_runtime(
                        container, image, self.plan["databases"][0]
                    )
        container, image = self.runtime_fixture()
        container["Mounts"][0]["RW"] = False
        with self.assertRaises(ValueError):
            validate_runtime(container, image, self.plan["databases"][0])
        container, image = self.runtime_fixture()
        validate_runtime(container, image, self.plan["databases"][0])

    def test_existing_stopped_container_cannot_initialize_missing_data(self):
        row = self.plan["databases"][0]
        container, image = self.runtime_fixture()
        container.update({
            "Name": "/pgts-retained-pg-textsearch-16435",
            "Image": row["image"], "State": {"Running": False},
            "HostConfig": {"PortBindings": {"5432/tcp": [{
                "HostIp": "127.0.0.1", "HostPort": "16435",
            }]}},
        })
        container["Config"].update({
            "Image": row["image"],
            "Labels": {
                "managed-by": "pg_textsearch-tin-retained",
                "source-volume": row["volume"],
            },
        })
        image["Id"] = row["image"]
        volume = {"Name": row["volume"], "Labels": row["labels"]}

        def run(args, **kwargs):
            if args[0] != "systemctl":
                raise AssertionError("database must not start without data")
            return subprocess.CompletedProcess(args, 3)

        with tempfile.TemporaryDirectory() as directory:
            with (
                patch(
                    "benchmarks.scale.retain_originals.subprocess.run",
                    side_effect=run,
                ),
                patch(
                    "benchmarks.scale.retain_originals."
                    "subprocess.check_output",
                    side_effect=[
                        "existing-id", json.dumps([container]),
                        json.dumps([volume]), json.dumps([image]), "",
                    ],
                ),
                self.assertRaisesRegex(ValueError, "initialized"),
            ):
                restore(self.plan, Path(directory) / "report.json")

    def test_unexpected_service_failure_is_not_treated_as_idle(self):
        with patch(
            "benchmarks.scale.retain_originals.subprocess.run",
            return_value=subprocess.CompletedProcess([], 1),
        ):
            with self.assertRaisesRegex(ValueError, "verify"):
                require_idle(self.plan["idle_units"])

    def test_running_non_retained_database_blocks_all_restoration(self):
        container = {
            "State": {"Running": True},
            "Config": {"Labels": {"managed-by": "pg_textsearch-tin"}},
        }
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            with (
                patch(
                    "benchmarks.scale.retain_originals.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 3),
                ),
                patch(
                    "benchmarks.scale.retain_originals."
                    "subprocess.check_output",
                    side_effect=["running-id", json.dumps([container])],
                ),
                self.assertRaisesRegex(ValueError, "still running"),
            ):
                restore(self.plan, report)
            self.assertFalse(report.exists())

    def test_changed_volume_labels_block_before_any_database_start(self):
        volume = {
            "Name": "original-index",
            "Labels": {"managed-by": "somebody-else"},
        }
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            with (
                patch(
                    "benchmarks.scale.retain_originals.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 3),
                ),
                patch(
                    "benchmarks.scale.retain_originals."
                    "subprocess.check_output",
                    side_effect=["", json.dumps([volume])],
                ),
                self.assertRaisesRegex(ValueError, "ownership changed"),
            ):
                restore(self.plan, report)
            self.assertFalse(report.exists())
