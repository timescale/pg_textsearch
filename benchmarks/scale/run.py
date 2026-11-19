#!/usr/bin/env python3
"""Run the pinned, sequential large-scale text-search benchmark."""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.scale.dataset import require_terms_acceptance
from benchmarks.scale.msmarco_download import download_assets
from benchmarks.scale.prepare_data import main as prepare_main


BENCHMARKER_SHA = "4b262c05af138991d3bf28c683a0fc6b7f113df3"
PG_TEXTSEARCH_SHA = "8923a5ba16df2731627dfbde6f6e3b62e5b8273b"
PARADEDB_TAG = "paradedb/paradedb:v0.26.8-pg18"
LEAD_IMAGE = (
    "planetscale/lead@sha256:"
    "ebb795acccd7f7bdabc009ca9490b73c4dbb216f61bc4ebcc4d95e284cc96b76"
)
XK6_VERSION = "v1.4.15"
K6_VERSION = "v1.8.0"
GO_VERSION = "go1.26.11"


class ConfigError(RuntimeError):
    pass


class BenchmarkCellsFailed(ConfigError):
    def __init__(self, engine, failures):
        self.engine = engine
        self.failures = failures
        super().__init__(
            f"{engine} has {len(failures)} unsuccessful benchmark results"
        )


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024**2), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_storage(
    path: Path,
    *,
    smoke: bool,
    is_mount=os.path.ismount,
    free_bytes=lambda value: shutil.disk_usage(value).free,
) -> None:
    mount = Path("/srv/searchbench")
    resolved = path.resolve()
    if mount not in (resolved, *resolved.parents):
        raise ConfigError(
            "artifacts must be under /srv/searchbench to protect the OS disk"
        )
    if not is_mount(mount):
        raise ConfigError(
            "/srv/searchbench is not mounted; wait for bootstrap before running"
        )
    minimum = 10 * 1024**3 if smoke else 500 * 1024**3
    available = free_bytes(mount)
    if available < minimum:
        raise ConfigError(
            f"insufficient free space on /srv/searchbench: {available} bytes"
        )


def build_plan(
    *, smoke: bool, accept_terms: bool, dataset_kind: str = "msmarco-v2"
) -> dict[str, object]:
    if not smoke and dataset_kind == "msmarco-v2" and not accept_terms:
        raise ConfigError(
            "MS MARCO terms acceptance is required for the full benchmark"
        )
    return {
        "format": 1,
        "benchmarker_sha": BENCHMARKER_SHA,
        "pg_textsearch_sha": PG_TEXTSEARCH_SHA,
        "dataset": {
            "kind": (
                "synthetic-smoke" if smoke else dataset_kind
            ),
            "rows": (
                10_000 if smoke else
                6_407_814 if dataset_kind == "wikipedia" else 138_364_198
            ),
        },
        "cells": [
            {
                "engine": "pg_textsearch",
                "row_limit": None,
                "modes": {
                    "disjunction": [10, 100],
                    "conjunction": [10, 100],
                    "phrase": [10, 100],
                    "count": [None],
                    "mixed": [10],
                },
                "unsupported": {},
            },
            {
                "engine": "paradedb",
                "row_limit": None,
                "modes": {
                    "disjunction": [10, 100],
                    "conjunction": [10, 100],
                    "phrase": [10, 100],
                    "count": [None],
                    "mixed": [10],
                },
                "unsupported": {},
            },
            {
                "engine": "lead",
                "row_limit": 10_000,
                "modes": {"compatibility_disjunction": [10, 100]},
                "unsupported": {
                    "performance": (
                        "compatibility-only full-heap rescans are not "
                        "performance-comparable"
                    )
                },
            },
        ],
    }


class Runner:
    def __init__(
        self, root: Path, artifacts: Path, smoke: bool, run_id: str | None,
        dataset_kind: str, dataset_path: Path | None,
    ):
        self.root = root
        self.artifacts = artifacts
        self.smoke = smoke
        self.dataset_kind = "synthetic-smoke" if smoke else dataset_kind
        stamp = run_id or dt.datetime.now(dt.timezone.utc).strftime(
            "%Y%m%dT%H%M%SZ"
        )
        self.run_id = stamp
        self.run_dir = artifacts / "runs" / stamp
        self.tools = artifacts / "tools"
        self.dataset = (
            dataset_path.resolve() if dataset_path else artifacts / (
                "smoke-dataset" if smoke else
                "wikipedia-20231101-en" if dataset_kind == "wikipedia"
                else dataset_kind
            )
        )
        self.lead_dataset = (
            self.dataset if smoke else artifacts / "lead-10k"
        )
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.log = self.run_dir / "commands.jsonl"

    def command(self, args, *, cwd=None, env=None, output=None):
        started = time.time()
        record = {"argv": list(args), "started_at": started}
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record) + "\n")
        target = (
            open(output, "w", encoding="utf-8") if output is not None else None
        )
        try:
            subprocess.run(
                args,
                cwd=cwd,
                env=env,
                check=True,
                text=True,
                stdout=target,
                stderr=subprocess.STDOUT if target else None,
            )
        except subprocess.CalledProcessError as error:
            (self.run_dir / "error.json").write_text(
                json.dumps(
                    {
                        "argv": list(args),
                        "exit_code": error.returncode,
                        "elapsed_seconds": time.time() - started,
                    },
                    indent=2,
                )
                + "\n"
            )
            raise
        finally:
            if target:
                target.close()

    def install(self):
        for tool in ("git", "go", "make", "docker"):
            if shutil.which(tool) is None:
                raise ConfigError(f"required tool is missing: {tool}")
        docker_root = subprocess.run(
            ["docker", "info", "--format", "{{.DockerRootDir}}"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if not Path(docker_root).resolve().is_relative_to(
            Path("/srv/searchbench")
        ):
            raise ConfigError(
                "Docker data-root is not under /srv/searchbench; "
                "wait for bootstrap"
            )
        checkout = self.tools / "benchmarker"
        if not checkout.exists():
            checkout.parent.mkdir(parents=True, exist_ok=True)
            self.command(
                [
                    "git",
                    "clone",
                    "https://github.com/paradedb/benchmarker.git",
                    str(checkout),
                ]
            )
        self.command(["git", "fetch", "--all", "--tags"], cwd=checkout)
        self.command(["git", "checkout", "--detach", BENCHMARKER_SHA], cwd=checkout)
        tool_tmp = self.tools / "tmp"
        tool_tmp.mkdir(parents=True, exist_ok=True)
        go_env = os.environ | {
            "GOTOOLCHAIN": "local",
            "TMPDIR": str(tool_tmp),
        }
        go_version = subprocess.run(
            ["go", "version"],
            check=True,
            capture_output=True,
            text=True,
            env=go_env,
        ).stdout.split()[2]
        if go_version != GO_VERSION:
            raise ConfigError(
                f"Go {GO_VERSION} is required, found {go_version}"
            )
        self.command(
            ["go", "install", f"go.k6.io/xk6/cmd/xk6@{XK6_VERSION}"],
            cwd=checkout,
            env=go_env,
        )
        go_path = subprocess.run(
            ["go", "env", "GOPATH"],
            check=True,
            capture_output=True,
            text=True,
            env=go_env,
        ).stdout.strip()
        self.command(
            [
                str(Path(go_path) / "bin/xk6"),
                "build",
                K6_VERSION,
                "--with",
                "github.com/paradedb/benchmarker=.",
                "--output",
                "./k6",
            ],
            cwd=checkout,
            env=go_env,
        )
        self.command(
            ["make", "loader", "viewer"], cwd=checkout, env=go_env
        )
        self.command(["docker", "pull", "postgres:18-bookworm"])
        base = json.loads(subprocess.run(
            ["docker", "image", "inspect", "postgres:18-bookworm"],
            check=True, capture_output=True, text=True,
        ).stdout)[0]
        postgres_base = next(
            (d for d in (base.get("RepoDigests") or [])
             if d.startswith("postgres@")),
            None,
        )
        if not postgres_base:
            raise ConfigError("Postgres base tag did not resolve to a digest")
        self.command(
            [
                "docker",
                "build",
                "--build-arg",
                f"PG_TEXTSEARCH_SHA={PG_TEXTSEARCH_SHA}",
                "--build-arg",
                f"POSTGRES_BASE={postgres_base}",
                "-t",
                f"pg_textsearch-scale:{PG_TEXTSEARCH_SHA}",
                "-f",
                str(self.root / "benchmarks/scale/docker/pg_textsearch.Dockerfile"),
                str(self.root),
            ]
        )
        self.command(["docker", "pull", PARADEDB_TAG])
        self.command(["docker", "pull", LEAD_IMAGE])
        inspect = subprocess.run(
            ["docker", "image", "inspect", PARADEDB_TAG],
            check=True,
            capture_output=True,
            text=True,
        )
        image = json.loads(inspect.stdout)[0]
        digests = image.get("RepoDigests") or []
        if not digests:
            raise ConfigError("ParadeDB tag did not resolve to a registry digest")
        lock = {
            "benchmarker": BENCHMARKER_SHA,
            "xk6": XK6_VERSION,
            "k6": K6_VERSION,
            "go": GO_VERSION,
            "pg_textsearch_source": PG_TEXTSEARCH_SHA,
            "postgres_base": postgres_base,
            "pg_textsearch_image_id": subprocess.run(
                [
                    "docker",
                    "image",
                    "inspect",
                    f"pg_textsearch-scale:{PG_TEXTSEARCH_SHA}",
                    "--format",
                    "{{.Id}}",
                ],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip(),
            "paradedb": digests[0],
            "lead": LEAD_IMAGE,
        }
        (self.artifacts / "images.lock.json").write_text(
            json.dumps(lock, indent=2) + "\n"
        )

    def prepare(self):
        if self.smoke:
            status = prepare_main(
                [
                    "synthetic",
                    "--output",
                    str(self.dataset),
                    "--rows",
                    "10000",
                ]
            )
            if status:
                raise ConfigError("synthetic dataset preparation failed")
        elif self.dataset_kind == "wikipedia":
            if not (self.dataset / "manifest.json").is_file():
                self.command(
                    [
                        sys.executable,
                        str(self.root / "benchmarks/scale/wikipedia.py"),
                        "--output",
                        str(self.dataset),
                    ],
                    output=self.run_dir / "wikipedia-prepare.log",
                )
        else:
            self._prepare_msmarco()
        manifest = json.loads((self.dataset / "manifest.json").read_text())
        if self.dataset_kind == "wikipedia":
            if (
                manifest.get("dataset") != "wikipedia-20231101-en"
                or not manifest.get("complete")
                or manifest.get("rows") != 6_407_814
                or manifest.get("revision")
                != "b04c8d1ceb2f5cd4588862100d08de323dccfbaa"
                or manifest.get("source")
                != "https://huggingface.co/datasets/wikimedia/wikipedia"
            ):
                raise ConfigError("invalid or incomplete Wikipedia manifest")
        self._validate_dataset(manifest)
        self._validate_query_packs(self.dataset)
        self._prepare_update_targets()
        templates = self.root / "benchmarks/scale/benchmarker"
        self._copy_templates(templates, self.dataset)
        if not self.smoke:
            self._prepare_lead_subset()
            self._copy_templates(templates, self.lead_dataset)
        plan = build_plan(
            smoke=self.smoke,
            dataset_kind=(
                "msmarco-v2" if self.smoke else self.dataset_kind
            ),
            accept_terms=(
                self.smoke
                or os.environ.get("MSMARCO_ACCEPT_TERMS", "").lower()
                in {"1", "true", "yes"}
            ),
        )
        self._write_or_validate_json(self.run_dir / "plan.json", plan)
        provenance = {
            "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "uname": subprocess.run(
                ["uname", "-a"], check=True, capture_output=True, text=True
            ).stdout.strip(),
            "cpu": subprocess.run(
                ["lscpu", "--json"], check=True, capture_output=True, text=True
            ).stdout,
            "block_devices": subprocess.run(
                ["lsblk", "--json", "-b"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout,
            "memory_bytes": os.sysconf("SC_PAGE_SIZE")
            * os.sysconf("SC_PHYS_PAGES"),
            "images_lock": (
                json.loads((self.artifacts / "images.lock.json").read_text())
                if (self.artifacts / "images.lock.json").is_file()
                else None
            ),
        }
        provenance_path = self.run_dir / "provenance.json"
        if not provenance_path.exists():
            provenance_path.write_text(json.dumps(provenance, indent=2) + "\n")

    def _prepare_msmarco(self):
        accepted = os.environ.get(
            "MSMARCO_ACCEPT_TERMS", ""
        ).lower() in {"1", "true", "yes"}
        require_terms_acceptance(accepted)
        assets = None
        if not (self.dataset / "manifest.json").is_file():
            archive = os.environ.get("MSMARCO_ARCHIVE")
            if not archive:
                assets = download_assets(
                    self.artifacts / "downloads/msmarco-v2",
                    accepted=accepted,
                )
                archive = str(assets["archive"])
            status = prepare_main([
                "corpus",
                "--archive", archive,
                "--output", str(self.dataset),
                "--accept-msmarco-terms",
            ])
            if status:
                raise ConfigError("MS MARCO corpus preparation failed")
        query_dir = self.dataset / "k6"
        if not all(
            (query_dir / name).is_file()
            for name in ("natural.json", "spans.json")
        ):
            configured = os.environ.get("MSMARCO_QUERY_FILES")
            if configured:
                query_specs = [
                    value.strip()
                    for value in configured.split(",")
                    if value.strip()
                ]
            else:
                if assets is None:
                    assets = download_assets(
                        self.artifacts / "downloads/msmarco-v2",
                        accepted=accepted,
                    )
                query_specs = [
                    f"dev1={assets['dev1']}",
                    f"dev2={assets['dev2']}",
                ]
            args = ["queries"]
            for spec in query_specs:
                args.extend(["--query", spec])
            args.extend([
                "--output", str(query_dir),
                "--corpus-parquet", str(self.dataset / "data"),
                "--accept-msmarco-terms",
            ])
            if prepare_main(args):
                raise ConfigError("MS MARCO query preparation failed")

    @staticmethod
    def _write_or_validate_json(path: Path, value):
        if path.exists():
            if json.loads(path.read_text()) != value:
                raise ConfigError(f"incompatible resume metadata: {path}")
            return
        temporary = path.with_suffix(path.suffix + ".partial")
        temporary.write_text(json.dumps(value, indent=2) + "\n")
        temporary.replace(path)

    def _validate_dataset(self, manifest):
        if not manifest.get("complete"):
            raise ConfigError("dataset manifest is incomplete")
        expected = 10_000 if self.smoke else (
            6_407_814 if self.dataset_kind == "wikipedia" else 138_364_198
        )
        if manifest.get("rows") != expected:
            raise ConfigError("dataset row count does not match selected corpus")
        if manifest.get("columns") != {
            "id": "int64", "source_id": "string", "body": "string"
        }:
            raise ConfigError("dataset schema is incompatible")
        shards = manifest.get("shards") or manifest.get("members") or []
        if sum(item.get("rows", 0) for item in shards) != expected:
            raise ConfigError("dataset shard row counts do not match manifest")
        for item in shards:
            shard = self.dataset / "data" / item["file"]
            if not shard.is_file() or file_sha256(shard) != item["sha256"]:
                raise ConfigError(f"dataset shard checksum failed: {shard.name}")
        if self.dataset_kind == "msmarco-v2" and not self.smoke:
            if (
                manifest.get("dataset") != "msmarco-v2"
                or manifest.get("source_md5") != (
                "05946bac48a8ffee62e160213eab3fda"
                )
            ):
                raise ConfigError("MS MARCO source checksum is not pinned")
        if self.dataset_kind == "wikipedia" and (
            manifest.get("dataset") != "wikipedia-20231101-en"
            or manifest.get("revision")
            != "b04c8d1ceb2f5cd4588862100d08de323dccfbaa"
            or manifest.get("source")
            != "https://huggingface.co/datasets/wikimedia/wikipedia"
        ):
            raise ConfigError("Wikipedia source provenance is not pinned")

    @staticmethod
    def _validate_query_packs(dataset):
        for name in ("natural.json", "spans.json"):
            path = dataset / "k6" / name
            try:
                values = json.loads(path.read_text())
            except (OSError, json.JSONDecodeError) as error:
                raise ConfigError(f"invalid query pack: {path}") from error
            if not isinstance(values, list) or not values:
                raise ConfigError(f"empty query pack: {path}")
            if any(
                not isinstance(item, dict)
                or not isinstance(item.get("id"), str)
                or not isinstance(item.get("text"), str)
                or not item["text"].strip()
                for item in values
            ):
                raise ConfigError(f"invalid query records: {path}")

    def _prepare_update_targets(self):
        import pyarrow as pa
        import pyarrow.dataset as pads

        output = self.dataset / "k6" / "update_targets.json"
        if output.is_file():
            values = json.loads(output.read_text())
            if values and all(isinstance(value, int) for value in values):
                return
            raise ConfigError("invalid update target pack")
        dataset = pads.dataset(str(self.dataset / "data"), format="parquet")
        count = dataset.count_rows()
        target_count = min(100_000, count)
        indices = [
            (number * count) // target_count
            for number in range(target_count)
        ]
        table = dataset.take(pa.array(indices, type=pa.int64()))
        values = table.column("id").to_pylist()
        temporary = output.with_suffix(".json.partial")
        temporary.write_text(json.dumps(values) + "\n")
        temporary.replace(output)

    @staticmethod
    def _copy_templates(templates: Path, dataset: Path):
        for source in templates.rglob("*"):
            if source.is_file():
                relative = source.relative_to(templates)
                destination = dataset / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)

    def _refresh_templates(self, dataset: Path):
        self._copy_templates(
            self.root / "benchmarks/scale/benchmarker",
            dataset,
        )

    def _prepare_lead_subset(self):
        import pyarrow.dataset as pads
        import pyarrow.parquet as pq

        output = self.lead_dataset / "data" / "part-00000.parquet"
        if output.is_file():
            return
        self.lead_dataset.mkdir(parents=True, exist_ok=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        source = pads.dataset(
            str(self.dataset / "data"), format="parquet"
        )
        table = source.head(10_000)
        if table.num_rows != 10_000:
            raise ConfigError("full corpus has fewer than 10000 Lead rows")
        pq.write_table(table, output, compression="zstd")
        shutil.copyfile(
            self.dataset / "manifest.json",
            self.lead_dataset / "source-manifest.json",
        )
        (self.lead_dataset / "manifest.json").write_text(
            json.dumps(
                {
                    "dataset": "msmarco-v2-lead-compatibility-sample",
                    "rows": 10_000,
                    "selection": "first 10000 rows by Parquet shard order",
                    "performance_comparable": False,
                },
                indent=2,
            )
            + "\n"
        )
        for name in ("natural.json", "spans.json", "matrix.json"):
            source_query = self.dataset / "k6" / name
            if source_query.is_file():
                destination = self.lead_dataset / "k6" / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source_query, destination)

    def _wait(self, name):
        for _ in range(120):
            probe = subprocess.run(
                ["docker", "exec", name, "pg_isready", "-U", "postgres"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if probe.returncode == 0:
                external = subprocess.run(
                    [
                        "psql",
                        "postgresql://postgres@127.0.0.1:15432/"
                        "postgres?sslmode=disable",
                        "-XAt", "-c", "SELECT 1",
                    ],
                    env=os.environ | {"PGPASSWORD": "postgres"},
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if external.returncode == 0:
                    return
            time.sleep(2)
        raise ConfigError(f"database container did not become ready: {name}")

    def engine(self, engine: str):
        manifest_path = self.dataset / "manifest.json"
        if not manifest_path.is_file():
            raise ConfigError("dataset is not prepared")
        self._validate_dataset(json.loads(manifest_path.read_text()))
        self._validate_query_packs(self.dataset)
        lock = json.loads(
            (self.artifacts / "images.lock.json").read_text()
        )
        images = {
            "pg_textsearch": f"pg_textsearch-scale:{PG_TEXTSEARCH_SHA}",
            "paradedb": lock["paradedb"],
            "lead": LEAD_IMAGE,
        }
        name = f"pgts-scale-{engine.replace('_', '-')}"
        cell = self.run_dir / engine
        cell.mkdir(parents=True, exist_ok=True)
        engine_dataset = (
            self.lead_dataset if engine == "lead" else self.dataset
        )
        self._refresh_templates(engine_dataset)
        volume_suffix = hashlib.sha256(
            str(self.run_dir).encode()
        ).hexdigest()[:12]
        volume = f"pgts-scale-{volume_suffix}-{engine.replace('_', '-')}"
        volume_inspect = subprocess.run(
            ["docker", "volume", "inspect", volume],
            capture_output=True,
            text=True,
        )
        volume_existed = volume_inspect.returncode == 0
        if not volume_existed:
            self.command([
                "docker", "volume", "create",
                "--label", "managed-by=pg_textsearch-scale",
                "--label", f"run-id={self.run_id}",
                "--label", f"engine={engine}",
                volume,
            ])
        else:
            labels = json.loads(volume_inspect.stdout)[0].get("Labels") or {}
            if (
                labels.get("managed-by") != "pg_textsearch-scale"
                or labels.get("run-id") != self.run_id
                or labels.get("engine") != engine
            ):
                raise ConfigError(f"refusing foreign Docker volume: {volume}")
        self.command(
            ["docker", "volume", "inspect", volume],
            output=cell / "volume-inspect.json",
        )
        database_fingerprint = {
            "engine": engine,
            "image": images[engine],
            "dataset_manifest": file_sha256(
                engine_dataset / "manifest.json"
            ),
            "schema": file_sha256(engine_dataset / "schema.yaml"),
            "pre_sql": file_sha256(
                engine_dataset / engine / "pre.sql"
            ),
            "post_sql": file_sha256(
                engine_dataset / engine / "post.sql"
            ),
            "settings": {
                "cpus": "0-23",
                "memory": "192g",
                "shared_buffers": "32GB",
                "effective_cache_size": "192GB",
                "maintenance_work_mem": "8GB",
                "max_connections": "128",
            },
            "benchmarker": BENCHMARKER_SHA,
        }
        database_checkpoint = cell / "database.json"
        database_ready = database_checkpoint.is_file()
        if database_ready and not volume_existed:
            raise ConfigError(f"database volume missing for resume: {volume}")
        if database_ready and (
            json.loads(database_checkpoint.read_text())
            != database_fingerprint
        ):
            raise ConfigError(f"incompatible database resume: {cell}")
        subprocess.run(["docker", "rm", "-f", name], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server_args = [
            "docker", "run", "-d", "--name", name, "--shm-size=64g",
            "--cpuset-cpus=0-23", "--memory=192g", "--memory-swap=192g",
            "--mount", f"type=volume,source={volume},target=/var/lib/postgresql",
            "-e", "POSTGRES_PASSWORD=postgres",
            "-p", "127.0.0.1:15432:5432",
            images[engine],
            "-c", "shared_buffers=32GB",
            "-c", "effective_cache_size=192GB",
            "-c", "maintenance_work_mem=8GB",
            "-c", "max_connections=128",
            "-c", "track_io_timing=on",
        ]
        if engine == "pg_textsearch":
            server_args.extend(
                ["-c", "shared_preload_libraries=pg_textsearch"]
            )
        self.command(server_args, output=cell / "container-start.log")
        try:
            self._wait(name)
            self.command(
                ["docker", "inspect", name],
                output=cell / "container-inspect.json",
            )
            self.command(
                [
                    "docker", "exec", name, "psql", "-U", "postgres", "-XAt",
                    "-F", "\t", "-c",
                    "SELECT name, setting, unit, source FROM pg_settings "
                    "ORDER BY name",
                ],
                output=cell / "postgres-settings.tsv",
            )
            self.command(
                [
                    "docker", "exec", name, "psql", "-U", "postgres", "-XAt",
                    "-c", "SELECT version()",
                ],
                output=cell / "postgres-version.txt",
            )
            env = os.environ.copy()
            env[f"{engine.upper()}_URL"] = (
                "postgres://postgres:postgres@127.0.0.1:15432/postgres"
            )
            benchmarker = self.tools / "benchmarker"
            if not database_ready:
                start = time.time()
                self.command(
                    [
                        "taskset", "-c", "24-31",
                        str(benchmarker / "bin/loader"),
                        "load",
                        "--backend",
                        engine,
                        "--backends-dir",
                        str(self.dataset / "backends"),
                        "--workers",
                        "8",
                        "--batch-size",
                        "10000",
                        str(engine_dataset),
                    ],
                    env=env,
                    output=cell / "load.log",
                )
                (cell / "build.json").write_text(
                    json.dumps(
                        {"elapsed_seconds": time.time() - start}, indent=2
                    ) + "\n"
                )
                self._write_or_validate_json(
                    database_checkpoint, database_fingerprint
                )
            self.command(
                [
                    "docker", "exec", name, "psql", "-U", "postgres", "-XAt",
                    "-F", "\t", "-c",
                    "SELECT pg_relation_size(indexrelid), "
                    "pg_total_relation_size(indrelid) "
                    "FROM pg_index WHERE indexrelid::regclass::text IN "
                    "('documents_bm25','documents_tin')",
                ],
                output=cell / "sizes.tsv",
            )
            query = json.loads(
                (engine_dataset / "k6/natural.json").read_text()
            )[0]["text"]
            query_literal = "'" + query.replace("'", "''") + "'"
            sql = {
                "pg_textsearch": (
                    "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
                    "SELECT id FROM documents ORDER BY body <@> "
                    f"to_bm25query({query_literal},'documents_bm25') LIMIT 10;"
                    "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
                    "SELECT id FROM documents WHERE body @@ "
                    f"plainto_tsquery('english',{query_literal}) "
                    "ORDER BY body <@> "
                    f"to_bm25query({query_literal},'documents_bm25') LIMIT 10;"
                    "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
                    "SELECT id FROM documents WHERE body @@ "
                    f"phraseto_tsquery('english',{query_literal}) "
                    "ORDER BY body <@> "
                    f"to_bm25query({query_literal},'documents_bm25') LIMIT 10;"
                    "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
                    "SELECT count(*) FROM documents WHERE body @@ "
                    "(SELECT string_agg(quote_literal(lexeme),' | ')::tsquery "
                    "FROM unnest(tsvector_to_array(to_tsvector("
                    f"'english',{query_literal}))) AS lexeme)"
                ),
                "paradedb": (
                    "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
                    f"SELECT id FROM documents WHERE body ||| {query_literal} "
                    "ORDER BY paradedb.score(id) DESC LIMIT 10"
                ),
                "lead": (
                    "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
                    f"SELECT id FROM documents WHERE body ==> {query_literal} "
                    "ORDER BY tin.full_score(ctid) DESC LIMIT 10"
                ),
            }[engine]
            self.command(
                ["docker", "exec", name, "psql", "-U", "postgres", "-XAt",
                 "-c", sql],
                output=cell / "explain.json",
            )
            dirty_updates = cell / "mixed-updates-dirty.json"
            if dirty_updates.exists():
                self._cleanup_updates(name, cell)
                dirty_updates.unlink()
            failures = []
            correctness_queries, correctness_fingerprint = (
                self._correctness_inputs(
                    engine_dataset, database_fingerprint
                )
            )
            correctness_summary = self._capture_correctness(
                name,
                engine,
                correctness_queries,
                cell / "correctness-v2.jsonl",
                correctness_fingerprint,
            )
            if (
                correctness_summary["timeout"]
                or correctness_summary["sql_error"]
            ):
                failures.append({
                    "kind": "correctness",
                    "summary": correctness_summary,
                })
            modes = (
                ("disjunction", "count", "conjunction", "phrase", "mixed")
                if engine != "lead"
                else ("disjunction",)
            )
            concurrencies = [8, 1]
            if os.environ.get("PGTS_INCLUDE_32VU") == "1":
                concurrencies.append(32)
            for mode in modes:
                limits = (0,) if mode == "count" else (10, 100)
                if mode == "mixed":
                    limits = (10,)
                for limit in limits:
                    for vus in concurrencies:
                        for query_set in ("spans", "natural"):
                            result_dir = (
                                cell / mode / f"top{limit}" /
                                f"{query_set}-{vus}vu"
                            )
                            result_dir.mkdir(parents=True, exist_ok=True)
                            run_env = env | {
                                "ENGINE": engine,
                                "MODE": mode,
                                "QUERY_FILE": f"./{query_set}.json",
                                "LIMIT": str(limit),
                                "DURATION": (
                                    "3s" if self.smoke else "5m"
                                ),
                                "WARMUP": (
                                    "2s" if self.smoke else "60s"
                                ),
                                "TOTAL_DURATION": (
                                    "8s" if self.smoke else "7m"
                                ),
                                "VUS": str(vus),
                                "ROWS": (
                                    "10000" if self.smoke else
                                    "6407814" if self.dataset_kind == "wikipedia"
                                    else "138364198"
                                ),
                                "BACKENDS_DIR": str(
                                    engine_dataset / "backends"
                                ),
                                "UPDATE_RATE": (
                                    "10" if self.smoke else "1000"
                                ),
                                "DASHBOARD_EXPORT_DIR": str(result_dir),
                                "DASHBOARD_EXPORT_PREFIX": (
                                    f"{engine}_{mode}_{query_set}_"
                                    f"{vus}vu_top{limit}"
                                ),
                            }
                            fingerprint = {
                                "engine": engine,
                                "mode": mode,
                                "limit": limit,
                                "vus": vus,
                                "query_set": query_set,
                                "dataset_manifest": file_sha256(
                                    engine_dataset / "manifest.json"
                                ),
                                "queries": file_sha256(
                                    engine_dataset / "k6" /
                                    f"{query_set}.json"
                                ),
                                "images_lock": file_sha256(
                                    self.artifacts / "images.lock.json"
                                ),
                                "script": file_sha256(
                                    engine_dataset / "k6/search.js"
                                ),
                                "benchmarker": BENCHMARKER_SHA,
                                "database": database_fingerprint,
                                "duration": run_env["DURATION"],
                                "warmup": run_env["WARMUP"],
                                "update_rate": run_env["UPDATE_RATE"],
                            }
                            complete = result_dir / "complete.json"
                            if complete.is_file():
                                if json.loads(complete.read_text()) != fingerprint:
                                    raise ConfigError(
                                        f"incompatible cell resume: {result_dir}"
                                    )
                                continue
                            failed = result_dir / "failed.json"
                            if failed.is_file() and (
                                json.loads(failed.read_text()).get(
                                    "fingerprint"
                                )
                                != fingerprint
                            ):
                                raise ConfigError(
                                    f"incompatible failed cell: {result_dir}"
                                )
                            if mode == "mixed":
                                dirty_updates.write_text(
                                    json.dumps(fingerprint, indent=2) + "\n"
                                )
                            result = self._execute_k6_cell(
                                benchmarker,
                                engine_dataset / "k6/search.js",
                                run_env,
                                result_dir,
                                fingerprint,
                            )
                            if mode == "mixed":
                                self._cleanup_updates(name, cell)
                                dirty_updates.unlink(missing_ok=True)
                            if result["status"] != "ok":
                                failures.append({
                                    "kind": "workload",
                                    "path": str(
                                        result_dir.relative_to(cell)
                                    ),
                                    "exit_code": result["exit_code"],
                                })
            summary = {
                "engine": engine,
                "status": "failed" if failures else "ok",
                "correctness": correctness_summary,
                "failures": failures,
            }
            summary_path = cell / "summary.json"
            temporary = summary_path.with_suffix(".json.partial")
            temporary.write_text(json.dumps(summary, indent=2) + "\n")
            temporary.replace(summary_path)
            if failures:
                raise BenchmarkCellsFailed(engine, failures)
            self._write_or_validate_json(
                cell / "complete.json",
                {"complete": True, "engine": engine},
            )
        finally:
            with (cell / "postgres.log").open("w") as stream:
                subprocess.run(
                    ["docker", "logs", name],
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
            subprocess.run(["docker", "rm", "-f", name], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def _run_k6_cell(self, benchmarker, script, run_env, result_dir):
        base_command = [
            "taskset", "-c", "24-31", str(benchmarker / "k6"), "run",
        ]
        warmup_env = run_env | {
            "PHASE": "warmup",
            "DURATION": run_env["WARMUP"],
        }
        self.command(
            base_command + [str(script)],
            env=warmup_env,
            output=result_dir / "warmup.log",
        )
        measured_env = run_env | {"PHASE": "measured"}
        self.command(
            base_command + [
                "--out", "dashboard=html,json,query_csv", str(script),
            ],
            env=measured_env,
            output=result_dir / "k6.log",
        )

    def _execute_k6_cell(
        self, benchmarker, script, run_env, result_dir, fingerprint
    ):
        try:
            self._run_k6_cell(
                benchmarker, script, run_env, result_dir
            )
        except subprocess.CalledProcessError as error:
            failure = {
                "status": "failed",
                "exit_code": error.returncode,
                "fingerprint": fingerprint,
            }
            failed = result_dir / "failed.json"
            temporary = failed.with_suffix(".json.partial")
            temporary.write_text(json.dumps(failure, indent=2) + "\n")
            temporary.replace(failed)
            return failure
        self._write_or_validate_json(
            result_dir / "complete.json", fingerprint
        )
        (result_dir / "failed.json").unlink(missing_ok=True)
        return {"status": "ok", "fingerprint": fingerprint}

    @staticmethod
    def run_engines(runner, engines):
        failures = []
        for engine in engines:
            try:
                runner.engine(engine)
            except BenchmarkCellsFailed as error:
                failures.extend(error.failures)
        if failures:
            raise BenchmarkCellsFailed("aggregate", failures)

    def _cleanup_updates(self, container, cell):
        self.command(
            [
                "docker", "exec", container, "psql", "-U", "postgres",
                "-v", "ON_ERROR_STOP=1", "-c",
                "UPDATE documents SET body = left(body, length(body) - 11) "
                "WHERE right(body, 11) = ' pgtsupdate'",
            ],
            output=cell / "mixed-cleanup.log",
        )
        self.command(
            [
                "docker", "exec", container, "psql", "-U", "postgres",
                "-v", "ON_ERROR_STOP=1", "-c",
                "VACUUM (ANALYZE) documents",
            ],
            output=cell / "mixed-vacuum.log",
        )

    @staticmethod
    def _correctness_inputs(dataset, database_fingerprint):
        natural_path = dataset / "k6/natural.json"
        spans_path = dataset / "k6/spans.json"
        natural = json.loads(natural_path.read_text())
        spans = json.loads(spans_path.read_text())
        longest = max(query.get("span", 0) for query in spans)
        controlled = next(
            query for query in spans if query.get("span", 0) == longest
        )
        queries = [
            natural[0] | {"query_family": "natural"},
            controlled | {"query_family": "controlled_span"},
        ]
        fingerprint = {
            "database": database_fingerprint,
            "natural_queries": file_sha256(natural_path),
            "span_queries": file_sha256(spans_path),
        }
        return queries, fingerprint

    def _capture_correctness(
        self, container, engine, queries, output, fingerprint
    ):
        selected = (
            queries[:1]
            if len(queries) < 2
            else [queries[0], queries[len(queries) // 2]]
        )
        modes = (
            ("disjunction", "conjunction", "phrase", "count")
            if engine != "lead" else ("disjunction",)
        )
        timeout_seconds = 30
        checkpoint = {
            "version": 2,
            "fingerprint": fingerprint,
            "engine": engine,
            "query_ids": [query["id"] for query in selected],
            "modes": list(modes),
            "timeout_seconds": timeout_seconds,
        }
        state = output.with_suffix(".fingerprint.json")
        complete = output.with_suffix(".complete.json")
        self._write_or_validate_json(state, checkpoint)
        if complete.is_file():
            saved = json.loads(complete.read_text())
            if saved.get("fingerprint") != checkpoint:
                raise ConfigError(
                    f"incompatible correctness resume: {output}"
                )
            return saved["summary"]
        records = []
        if output.is_file():
            records = [
                json.loads(line)
                for line in output.read_text().splitlines()
                if line.strip()
            ]
        completed = {
            (record["mode"], record["query_id"]) for record in records
        }
        error_log = output.with_suffix(".stderr.jsonl")
        with (
            output.open("a", encoding="utf-8") as stream,
            error_log.open("a", encoding="utf-8") as errors,
        ):
            for mode in modes:
              for query in selected:
                key = (mode, query["id"])
                if key in completed:
                    continue
                literal = "'" + query["text"].replace("'", "''") + "'"
                pgts = {
                    "disjunction": (
                        "SELECT id FROM documents ORDER BY body <@> "
                        f"to_bm25query({literal},'documents_bm25') LIMIT 100"
                    ),
                    "conjunction": (
                        "SELECT id FROM documents WHERE body @@ "
                        f"plainto_tsquery('english',{literal}) "
                        "ORDER BY body <@> "
                        f"to_bm25query({literal},'documents_bm25') LIMIT 100"
                    ),
                    "phrase": (
                        "SELECT id FROM documents WHERE body @@ "
                        f"phraseto_tsquery('english',{literal}) "
                        "ORDER BY body <@> "
                        f"to_bm25query({literal},'documents_bm25') LIMIT 100"
                    ),
                    "count": (
                        "SELECT count(*) FROM documents WHERE body @@ "
                        "(SELECT string_agg(quote_literal(lexeme),' | ')::tsquery "
                        "FROM unnest(tsvector_to_array(to_tsvector("
                        f"'english',{literal}))) AS lexeme)"
                    ),
                }
                parade_op = {
                    "disjunction": "|||",
                    "conjunction": "&&&",
                    "phrase": "###",
                    "count": "|||",
                }[mode]
                if engine == "pg_textsearch":
                    sql = pgts[mode]
                elif engine == "paradedb":
                    prefix = "SELECT count(*)" if mode == "count" else "SELECT id"
                    suffix = "" if mode == "count" else (
                        " ORDER BY paradedb.score(id) DESC LIMIT 100"
                    )
                    sql = (
                        f"{prefix} FROM documents WHERE body {parade_op} "
                        f"{literal}{suffix}"
                    )
                else:
                    sql = (
                        f"SELECT id FROM documents WHERE body ==> {literal} "
                        "ORDER BY tin.full_score(ctid) DESC LIMIT 100"
                    )
                result = subprocess.run(
                    [
                        "docker", "exec", "-e",
                        (
                            "PGOPTIONS=-c statement_timeout="
                            f"{timeout_seconds * 1000}"
                        ),
                        container, "psql", "-U", "postgres",
                        "-XqAt", "-c", sql,
                    ],
                    capture_output=True,
                    text=True,
                )
                if result.returncode == 0:
                    record = {
                        "query_id": query["id"],
                        "query_family": query.get("query_family"),
                        "mode": mode,
                        "status": "ok",
                        "censored": False,
                        "rows": result.stdout.splitlines(),
                    }
                else:
                    timed_out = "statement timeout" in result.stderr.lower()
                    record = {
                        "query_id": query["id"],
                        "query_family": query.get("query_family"),
                        "mode": mode,
                        "status": "timeout" if timed_out else "sql_error",
                        "censored": timed_out,
                        "returncode": result.returncode,
                    }
                    errors.write(json.dumps({
                        "query_id": query["id"],
                        "query_family": query.get("query_family"),
                        "mode": mode,
                        "status": record["status"],
                        "stderr": result.stderr,
                    }) + "\n")
                    errors.flush()
                    os.fsync(errors.fileno())
                stream.write(json.dumps(record) + "\n")
                stream.flush()
                os.fsync(stream.fileno())
                records.append(record)
        summary = {
            status: sum(
                record["status"] == status for record in records
            )
            for status in ("ok", "timeout", "sql_error")
        }
        summary["total"] = len(records)
        self._write_or_validate_json(
            complete,
            {"fingerprint": checkpoint, "summary": summary},
        )
        return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "stage",
        choices=[
            "install", "prepare", "pg_textsearch", "paradedb", "lead",
            "all", "status",
        ],
    )
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument(
        "--dataset",
        choices=["msmarco-v2", "wikipedia"],
        default="msmarco-v2",
    )
    parser.add_argument("--dataset-path", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument(
        "--artifacts",
        type=Path,
        default=Path(
            os.environ.get(
                "PGTS_SCALE_ROOT",
                "/srv/searchbench/pg_textsearch-scale",
            )
        ),
    )
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[2]
    accepted = os.environ.get("MSMARCO_ACCEPT_TERMS", "").lower() in {
        "1", "true", "yes",
    }
    try:
        build_plan(
            smoke=args.smoke,
            accept_terms=accepted,
            dataset_kind=args.dataset,
        )
        validate_storage(args.artifacts, smoke=args.smoke)
        runner = Runner(
            root, args.artifacts.resolve(), args.smoke, args.run_id,
            args.dataset, args.dataset_path,
        )
        if args.stage == "status":
            print(runner.run_dir)
            return 0
        if args.stage in {"install", "all"}:
            runner.install()
        if args.stage in {"prepare", "all"}:
            runner.prepare()
        engines = (
            ["pg_textsearch", "paradedb", "lead"]
            if args.stage == "all"
            else [args.stage]
        )
        Runner.run_engines(
            runner,
            [
                engine for engine in engines
                if engine in {"pg_textsearch", "paradedb", "lead"}
            ],
        )
        return 0
    except (ConfigError, OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
