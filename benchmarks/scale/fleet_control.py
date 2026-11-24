#!/usr/bin/env python3
"""Resumable controller for the six-worker TIN benchmark fleet."""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import datetime as dt
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile
import time

from benchmarks.scale.fleet import fleet_plan, shard_cells
from benchmarks.scale.run import (
    BASELINE_PG_TEXTSEARCH_SHA, ConfigError, native_source,
)
from benchmarks.scale.tin import PARADEDB_IMAGE, PG_TEXTSEARCH_SHA


ENGINES = ("pg_textsearch", "paradedb")
PROFILES = ("high", "constrained32", "constrained16")
DATASETS = ("msmarco-v2", "wikipedia")
REQUIRED_CONFIG = {
    "subscription",
    "group_prefix",
    "regions",
    "size",
    "lifetime_days",
    "ssh_private_key",
    "ssh_public_key",
    "ssh_cidr",
    "root",
    "remote_repo",
    "source_ref",
    "campaign_id",
    "sql_shape",
}
SCHEMA = {
    "subscription": "private Azure subscription identifier",
    "group_prefix": "private resource-group prefix",
    "regions": ["private-region-a", "private-region-b"],
    "size": "Standard_L32as_v3",
    "lifetime_days": 7,
    "ssh_private_key": "/private/path/key",
    "ssh_public_key": "/private/path/key.pub",
    "ssh_cidr": "private-admin-cidr",
    "existing_group": "optional-source-host-resource-group",
    "root": "/srv/searchbench/pg_textsearch-scale",
    "remote_repo": "/opt/searchbench/campaign",
    "source_ref": "immutable-source-commit",
    "native_source": PG_TEXTSEARCH_SHA,
    "campaign_id": "caller-selected-safe-id",
    "sql_shape": "filter_first",
    "state_dir": "/private/ignored/controller-state",
    "msmarco_run_id": "optional-derived-override",
    "wikipedia_run_id": "optional-derived-override",
    "msmarco_archive": "optional-private-local-path-on-source-host",
    "msmarco_query_files": (
        "optional comma-separated name=/private/path query inputs"
    ),
}


class ControlError(RuntimeError):
    pass


def verify_frozen_tree(root, expected):
    root = Path(root)
    failures = []
    for relative, digest in expected.items():
        path = root / relative
        if not path.is_file():
            failures.append(f"missing:{relative}")
            continue
        value = hashlib.sha256(path.read_bytes()).hexdigest()
        if value != digest:
            failures.append(f"changed:{relative}")
    if failures:
        raise ControlError(
            "frozen source verification failed: "
            + ", ".join(failures[:5])
        )


def validate_retained_container(info, *, image, volume, port):
    config = info.get("Config") or {}
    labels = config.get("Labels") or {}
    mounts = info.get("Mounts") or []
    bindings = (
        (info.get("HostConfig") or {})
        .get("PortBindings", {})
        .get("5432/tcp")
        or []
    )
    expected_binding = {
        "HostIp": "127.0.0.1",
        "HostPort": str(port),
    }
    if (
        labels.get("managed-by")
        != "pg_textsearch-tin-retained"
        or labels.get("source-volume") != volume
        or config.get("Image") != image
        or not any(
            mount.get("Name") == volume
            and mount.get("Destination") == "/var/lib/postgresql"
            for mount in mounts
        )
        or bindings != [expected_binding]
    ):
        raise ControlError(
            "retained container ownership or configuration mismatch"
        )


def _safe_component(value, name):
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z0-9_-]+", value
    ):
        raise ControlError(f"{name} must be one safe path component")
    return value


def _atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def _read_json(path, description):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ControlError(f"invalid {description}: {path}") from error


def load_private_config(path):
    config = _read_json(Path(path), "private fleet configuration")
    missing = sorted(REQUIRED_CONFIG - set(config))
    if missing:
        raise ControlError(
            "private fleet configuration is missing: "
            + ", ".join(missing)
        )
    if config["sql_shape"] not in {"original", "filter_first", "rank_first"}:
        raise ControlError("unknown fleet sql_shape")
    try:
        config["native_source"] = native_source(
            config.get("native_source", PG_TEXTSEARCH_SHA)
        )
    except ConfigError as error:
        raise ControlError(str(error)) from error
    campaign = _safe_component(config["campaign_id"], "campaign_id")
    config["msmarco_run_id"] = config.get(
        "msmarco_run_id", f"tin-msmarco-{campaign}"
    )
    config["wikipedia_run_id"] = config.get(
        "wikipedia_run_id", f"tin-wikipedia-{campaign}"
    )
    _safe_component(config["msmarco_run_id"], "msmarco_run_id")
    _safe_component(config["wikipedia_run_id"], "wikipedia_run_id")
    state = config.get("state_dir")
    if not state:
        raise ControlError("private fleet configuration needs state_dir")
    config["state_dir"] = str(Path(state).resolve())
    return config


def _host_name(value):
    text = str(value)
    if text.startswith("host"):
        suffix = text[4:]
    else:
        suffix = text
    if not suffix.isdigit() or not 1 <= int(suffix) <= 6:
        raise ControlError(f"invalid fleet host identifier: {value}")
    return f"host{int(suffix)}"


def _utc_instant(value, label):
    if not isinstance(value, str):
        raise ControlError(f"{label} expiration is invalid")
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        timestamp = dt.datetime.fromisoformat(normalized)
    except ValueError as error:
        raise ControlError(f"{label} expiration is invalid") from error
    if timestamp.tzinfo is None or timestamp.utcoffset() is None:
        raise ControlError(f"{label} expiration is invalid")
    return timestamp.astimezone(dt.timezone.utc)


def validate_manifest(manifest):
    deadline = manifest.get("common_deadline")
    if not isinstance(deadline, str):
        raise ControlError("fleet manifest common expiration is missing")
    deadline_instant = _utc_instant(deadline, "fleet common")
    hosts = manifest.get("hosts")
    if not isinstance(hosts, list) or len(hosts) != 6:
        raise ControlError("fleet manifest must contain exactly six hosts")
    normalized = []
    seen_hosts = set()
    seen_shards = set()
    for source in hosts:
        host = dict(source)
        host["host"] = _host_name(host.get("host"))
        shard = (host.get("engine"), host.get("profile"))
        if host["host"] in seen_hosts or shard in seen_shards:
            raise ControlError("fleet manifest has duplicate host or shard")
        if shard[0] not in ENGINES or shard[1] not in PROFILES:
            raise ControlError(f"invalid fleet shard: {shard}")
        if not host.get("ip") or not host.get("known_hosts"):
            raise ControlError(
                f"fleet host address is missing: {host['host']}"
            )
        host_expiration = _utc_instant(
            host.get("expiration"), f"fleet host {host['host']}"
        )
        if host_expiration != deadline_instant:
            raise ControlError(
                f"fleet host expiration does not match common deadline: "
                f"{host['host']}"
            )
        seen_hosts.add(host["host"])
        seen_shards.add(shard)
        normalized.append(host)
    expected = {(engine, profile) for engine in ENGINES
                for profile in PROFILES}
    if seen_shards != expected:
        raise ControlError("fleet manifest does not cover all six shards")
    by_name = {host["host"]: host for host in normalized}
    if (
        (by_name["host1"]["engine"], by_name["host1"]["profile"])
        != ("pg_textsearch", "high")
        or (by_name["host4"]["engine"], by_name["host4"]["profile"])
        != ("paradedb", "high")
    ):
        raise ControlError(
            "host1 must be native/high and host4 must be ParadeDB/high"
        )
    return sorted(normalized, key=lambda host: host["host"])


def build_partition(config, manifest):
    hosts = validate_manifest(manifest)
    partition = []
    for host in hosts:
        for dataset in DATASETS:
            plan = fleet_plan(dataset, config["sql_shape"])
            cells = shard_cells(
                plan, host["engine"], host["profile"]
            )
            for cell in cells:
                partition.append({
                    "dataset": dataset,
                    "run_id": (
                        config["msmarco_run_id"]
                        if dataset == "msmarco-v2"
                        else config["wikipedia_run_id"]
                    ),
                    "host": host["host"],
                    "engine": host["engine"],
                    "profile": host["profile"],
                    "cell_id": cell["cell_id"],
                    "query_pack": cell["pack"],
                    "operation": cell["operation"],
                })
    keys = {
        (row["dataset"], row["engine"], row["cell_id"])
        for row in partition
    }
    if len(partition) != 174 or len(keys) != 174:
        raise ControlError("fleet partition is not a disjoint 174-cell union")
    return partition


class SubprocessBackend:
    def __init__(self, config):
        self.config = config

    def _ssh(self, host, command):
        return [
            "ssh",
            "-i",
            self.config["ssh_private_key"],
            "-o",
            "BatchMode=yes",
            "-o",
            "ServerAliveInterval=30",
            "-o",
            "ConnectTimeout=20",
            "-o",
            f"UserKnownHostsFile={host['known_hosts']}",
            f"bench@{host['ip']}",
            command,
        ]

    def azure(self, action, config_path, state_dir):
        subprocess.run(
            [
                sys.executable,
                "-m",
                "benchmarks.scale.fleet_azure",
                "--config",
                str(config_path),
                "--state",
                str(state_dir),
                action,
            ],
            check=True,
        )

    def deploy_source(self, host, source_ref, remote_repo):
        repository = Path(__file__).resolve().parents[2]
        commit = subprocess.check_output(
            ["git", "-C", str(repository), "rev-parse",
             f"{source_ref}^{{commit}}"],
            text=True,
        ).strip()
        names = subprocess.check_output(
            ["git", "-C", str(repository), "ls-tree", "-r",
             "--name-only", commit],
            text=True,
        ).splitlines()
        overrides = set(subprocess.check_output(
            [
                "git", "-C", str(repository), "diff", "--name-only",
                commit, "--", "benchmarks/scale",
            ],
            text=True,
        ).splitlines())
        overrides.update(subprocess.check_output(
            [
                "git", "-C", str(repository), "ls-files", "--others",
                "--exclude-standard", "--", "benchmarks/scale",
            ],
            text=True,
        ).splitlines())
        deleted = {
            name for name in overrides
            if not (repository / name).is_file()
        }
        names = sorted((set(names) - deleted) | (overrides - deleted))
        files = {}
        for name in names:
            path = repository / name
            content = (
                path.read_bytes()
                if name in overrides
                else subprocess.check_output(
                    ["git", "-C", str(repository), "show",
                     f"{commit}:{name}"]
                )
            )
            files[name] = hashlib.sha256(content).hexdigest()
        manifest = {
            "format": 1,
            "source_ref": commit,
            "files": files,
        }
        encoded = base64.b64encode(
            json.dumps(manifest).encode()
        ).decode()
        verifier = r'''
import base64,hashlib,json,pathlib,sys
root=pathlib.Path(sys.argv[1])
manifest=json.loads(base64.b64decode(sys.argv[2]))
failures=[]
for relative,expected in manifest["files"].items():
    path=root/relative
    if not path.is_file():
        failures.append("missing:"+relative)
    elif hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
        failures.append("changed:"+relative)
if failures:
    raise SystemExit("frozen source mismatch: "+",".join(failures[:5]))
(root/".fleet-source.json").write_text(
    json.dumps(manifest,indent=2)+"\n"
)
'''
        probe = subprocess.run(
            self._ssh(
                host,
                "if test -d " + shlex.quote(remote_repo)
                + "; then python3 -c "
                + shlex.quote(verifier) + " "
                + shlex.quote(remote_repo) + " "
                + shlex.quote(encoded)
                + "; else exit 44; fi",
            ),
            check=False,
        )
        if probe.returncode == 0:
            return manifest
        if probe.returncode != 44:
            raise ControlError(
                "existing remote source failed frozen verification"
            )
        temporary = remote_repo + ".partial-" + commit[:12]
        receiver = (
            "set -eu; test ! -e " + shlex.quote(remote_repo)
            + "; if test -e " + shlex.quote(temporary)
            + "; then test -f "
            + shlex.quote(temporary + "/.fleet-source.partial-owned")
            + "; else mkdir -p " + shlex.quote(temporary)
            + "; touch "
            + shlex.quote(temporary + "/.fleet-source.partial-owned")
            + "; fi"
            + "; tar -C " + shlex.quote(temporary)
            + " -xpf -; python3 -c " + shlex.quote(verifier)
            + " " + shlex.quote(temporary) + " "
            + shlex.quote(encoded)
            + "; rm "
            + shlex.quote(temporary + "/.fleet-source.partial-owned")
            + "; mv " + shlex.quote(temporary)
            + " " + shlex.quote(remote_repo)
        )
        consumer = subprocess.Popen(
            self._ssh(host, receiver),
            stdin=subprocess.PIPE,
        )
        try:
            with tarfile.open(
                fileobj=consumer.stdin, mode="w|"
            ) as archive:
                for name in names:
                    path = repository / name
                    content = (
                        path.read_bytes()
                        if name in overrides
                        else subprocess.check_output(
                            ["git", "-C", str(repository), "show",
                             f"{commit}:{name}"]
                        )
                    )
                    info = tarfile.TarInfo(name)
                    info.size = len(content)
                    info.mode = (
                        path.stat().st_mode & 0o777
                        if name in overrides else 0o755
                        if subprocess.check_output(
                            [
                                "git", "-C", str(repository), "ls-tree",
                                commit, name,
                            ],
                            text=True,
                        ).startswith("100755 ")
                        else 0o644
                    )
                    archive.addfile(info, io.BytesIO(content))
            consumer.stdin.close()
            status = consumer.wait()
            if status:
                raise ControlError(
                    "frozen source deployment failed: "
                    f"target={status}"
                )
        finally:
            if consumer.poll() is None:
                consumer.terminate()
                consumer.wait()
        return manifest

    def run(self, host, command):
        return subprocess.check_output(
            self._ssh(host, command), text=True
        )

    def sha256(self, host, path):
        command = (
            "if test -f " + shlex.quote(path)
            + "; then sha256sum " + shlex.quote(path)
            + " | cut -d' ' -f1; fi"
        )
        value = self.run(host, command).strip()
        return value or None

    def stream(self, source, target, source_path, target_path, digest):
        receiver = (
            "set -eu; mkdir -p "
            + shlex.quote(str(Path(target_path).parent))
            + "; cat > " + shlex.quote(target_path + ".partial")
            + "; echo " + shlex.quote(
                f"{digest}  {target_path}.partial"
            )
            + " | sha256sum -c -; mv "
            + shlex.quote(target_path + ".partial")
            + " " + shlex.quote(target_path)
        )
        producer = subprocess.Popen(
            self._ssh(source, "cat " + shlex.quote(source_path)),
            stdout=subprocess.PIPE,
        )
        try:
            consumer = subprocess.run(
                self._ssh(target, receiver),
                stdin=producer.stdout,
                check=False,
            )
            producer.stdout.close()
            source_status = producer.wait()
            if source_status or consumer.returncode:
                raise ControlError(
                    "stream transfer failed: "
                    f"source={source_status}, "
                    f"target={consumer.returncode}"
                )
        finally:
            if producer.poll() is None:
                producer.terminate()
                producer.wait()

    def read_json(self, host, path):
        try:
            return json.loads(
                self.run(host, "cat " + shlex.quote(path))
            )
        except json.JSONDecodeError as error:
            raise ControlError(
                f"invalid remote JSON: {host['host']}:{path}"
            ) from error


class FleetControl:
    def __init__(self, config_path, *, backend=None):
        self.config_path = Path(config_path).resolve()
        self.config = load_private_config(self.config_path)
        self.state_dir = Path(self.config["state_dir"])
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.backend = backend or SubprocessBackend(self.config)

    def stage_path(self, name):
        return self.state_dir / "control" / f"{name}.json"

    def write_stage(self, name, value):
        _atomic_json(self.stage_path(name), value | {
            "source_selection": self.source_selection(),
        })

    def source_selection(self):
        return {
            key: self.config[key]
            for key in ("source_ref", "native_source", "sql_shape")
        }

    def legacy_selection(self, state):
        return {
            "source_ref": state.get("source_ref", self.config["source_ref"]),
            "native_source": BASELINE_PG_TEXTSEARCH_SHA,
            "sql_shape": "filter_first",
        }

    def read_stage(self, name):
        path = self.stage_path(name)
        if not path.is_file():
            raise ControlError(f"required stage is incomplete: {name}")
        state = _read_json(path, f"{name} stage")
        if state.get(
            "source_selection", self.legacy_selection(state)
        ) != self.source_selection():
            raise ControlError("fleet source selection changed during resume")
        if state.get("status") != "complete":
            raise ControlError(f"required stage is not complete: {name}")
        return state

    def manifest(self):
        source = _read_json(
            self.state_dir / "fleet-manifest.json",
            "fleet manifest",
        )
        return source | {"hosts": validate_manifest(source)}

    def hosts(self):
        return self.manifest()["hosts"]

    def host(self, name):
        normalized = _host_name(name)
        matches = [
            host for host in self.hosts()
            if host["host"] == normalized
        ]
        if len(matches) != 1:
            raise ControlError(f"fleet host is missing: {normalized}")
        return matches[0]

    def plan(self):
        manifest_path = self.state_dir / "fleet-manifest.json"
        if not manifest_path.is_file():
            self.backend.azure(
                "plan", self.config_path, self.state_dir
            )
        manifest = self.manifest()
        partition = build_partition(self.config, manifest)
        value = {
            "status": "complete",
            "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "source_ref": self.config["source_ref"],
            "source_selection": self.source_selection(),
            "partition_sha256": hashlib.sha256(
                json.dumps(partition, sort_keys=True).encode()
            ).hexdigest(),
            "partition": partition,
        }
        existing = self.stage_path("plan")
        if existing.is_file():
            prior = _read_json(existing, "fleet plan")
            stable = {
                key: value for key, value in value.items()
                if key != "created_at"
            }
            prior_stable = {
                key: value for key, value in prior.items()
                if key != "created_at"
            }
            prior_stable.setdefault(
                "source_selection", self.legacy_selection(prior)
            )
            if prior_stable != stable:
                raise ControlError("fleet plan changed during resume")
            return prior
        self.write_stage("plan", value)
        return value

    def provision(self):
        if self.stage_path("provision").is_file():
            return self.read_stage("provision")
        self.backend.azure(
            "provision", self.config_path, self.state_dir
        )
        self.plan()
        self.write_stage("provision", {
            "status": "complete",
            "hosts": [host["host"] for host in self.hosts()],
        })
        return self.read_stage("provision")

    def transfer(self, source, target, artifact):
        target_path = artifact.get("target_path", artifact["path"])
        existing = self.backend.sha256(target, target_path)
        if existing == artifact["sha256"]:
            return "reused"
        self.backend.stream(
            source,
            target,
            artifact["path"],
            target_path,
            artifact["sha256"],
        )
        verified = self.backend.sha256(target, target_path)
        if verified != artifact["sha256"]:
            raise ControlError(
                f"transferred artifact checksum mismatch: {target_path}"
            )
        return "transferred"

    def prepare(self):
        if self.stage_path("prepare").is_file():
            return self.read_stage("prepare")
        self.read_stage("provision")
        source = self.host("host1")
        root = self.config["root"]
        repo = self.config["remote_repo"]
        self.backend.deploy_source(
            source, self.config["source_ref"], repo
        )
        setup = r'''
import json,os,pathlib,subprocess
import benchmarks.scale.tin as tin_module
from benchmarks.scale.run import LEAD_IMAGE,Runner
from benchmarks.scale.tin import (
    ANALYZERS,TinRunner,
)
from benchmarks.scale.fleet import fleet_plan
root=pathlib.Path(os.environ["TIN_ROOT"])
repo=pathlib.Path(os.environ["TIN_REPO"])
class NoLeadRunner(Runner):
    def command(self,args,**kwargs):
        if list(args)==["docker","pull",LEAD_IMAGE]:
            return subprocess.CompletedProcess(args,0,"","")
        return super().command(args,**kwargs)
tin_module.Runner=NoLeadRunner
TinRunner(
    repo,root,False,os.environ["MSMARCO_RUN_ID"],
    "msmarco-v2",None,
).install()
runs={
    "msmarco-v2":os.environ["MSMARCO_RUN_ID"],
    "wikipedia":os.environ["WIKIPEDIA_RUN_ID"],
}
for dataset,run_id in runs.items():
    runner=TinRunner(repo,root,False,run_id,dataset,None)
    manifest_path=runner.dataset/"manifest.json"
    if dataset=="msmarco-v2":
        runner._prepare_msmarco()
    elif not manifest_path.is_file():
        runner.command([
            os.sys.executable,
            str(repo/"benchmarks/scale/wikipedia.py"),
            "--output",str(runner.dataset),
        ],output=runner.run_dir/"wikipedia-prepare.log")
    manifest=json.loads(manifest_path.read_text())
    runner._validate_dataset(manifest)
    runner._validate_query_packs(runner.dataset)
    runner._prepare_update_targets()
    runner._copy_templates(
        repo/"benchmarks/scale/benchmarker",runner.dataset
    )
    runner._write_or_validate_json(
        runner.tin_dir/"plan.json",fleet_plan(dataset)
    )
    analyzers=(
        list(ANALYZERS)
        if dataset=="msmarco-v2"
        else ["english-no-stopwords"]
    )
    runner.build_selected(
        ["pg_textsearch","paradedb"],analyzers
    )
'''
        environment = [
            "PYTHONPATH=" + shlex.quote(repo),
            "PGTS_NATIVE_SOURCE=" + self.config["native_source"],
            "PGTS_SQL_SHAPE=" + shlex.quote(self.config["sql_shape"]),
            "TIN_ROOT=" + shlex.quote(root),
            "TIN_REPO=" + shlex.quote(repo),
            "MSMARCO_RUN_ID="
            + shlex.quote(self.config["msmarco_run_id"]),
            "WIKIPEDIA_RUN_ID="
            + shlex.quote(self.config["wikipedia_run_id"]),
            "MSMARCO_ACCEPT_TERMS=1",
        ]
        if self.config.get("msmarco_archive"):
            environment.append(
                "MSMARCO_ARCHIVE="
                + shlex.quote(self.config["msmarco_archive"])
            )
        if self.config.get("msmarco_query_files"):
            environment.append(
                "MSMARCO_QUERY_FILES="
                + shlex.quote(self.config["msmarco_query_files"])
            )
        command = (
            "set -eu; "
            + "sudo -A " + shlex.quote(
                repo + "/benchmarks/scale/install.sh"
            )
            + "; mkdir -p " + shlex.quote(root)
            + "; python3 -m venv " + shlex.quote(root + "/venv")
            + "; " + shlex.quote(root + "/venv/bin/pip")
            + " install -r " + shlex.quote(
                repo + "/benchmarks/scale/requirements.txt"
            )
            + "; sg docker -c "
            + shlex.quote(
                " ".join(environment) + " "
                + shlex.quote(root + "/venv/bin/python") + " -"
            )
            + " <<'PY'\n" + setup + "\nPY\n"
            + "sg docker -c 'test -z \"$(docker ps -q)\"'"
        )
        self.backend.run(source, command)
        self.write_stage("prepare", {
            "status": "complete",
            "source": source["host"],
            "source_ref": self.config["source_ref"],
            "lead_invoked": False,
            "built_analyzer_volumes": 6,
        })
        return self.read_stage("prepare")

    def _export_command(self):
        root = self.config["root"]
        repo = self.config["remote_repo"]
        distribution = (
            f"{root}/fleet-distribution-"
            f"{self.config['campaign_id']}"
        )
        spec = {
            "root": root,
            "repo": repo,
            "distribution": distribution,
            "run_ids": [
                self.config["msmarco_run_id"],
                self.config["wikipedia_run_id"],
            ],
            "native_image": (
                f"pg_textsearch-scale:{self.config['native_source']}"
            ),
            "native_source": self.config["native_source"],
            "source_ref": self.config["source_ref"],
        }
        script = r'''
import hashlib,json,os,pathlib,shlex,subprocess
cfg=json.loads(__SPEC__)
root=pathlib.Path(cfg["root"])
repo=pathlib.Path(cfg["repo"])
out=pathlib.Path(cfg["distribution"])
out.mkdir(parents=True,exist_ok=True)
manifest=out/"export-manifest.json"
if subprocess.check_output(["docker","ps","-q"],text=True).strip():
    raise RuntimeError("containers are running during source export")
def pipe(command,target):
    partial=str(target)+".partial"
    subprocess.run(
        ["bash","-o","pipefail","-c",command+" > "+shlex.quote(partial)],
        check=True,
    )
    pathlib.Path(partial).replace(target)
def digest(path):
    value=hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda:stream.read(8*1024*1024),b""):
            value.update(chunk)
    return value.hexdigest()
lock=json.loads((root/"tin-images.lock.json").read_text())
native_image_id=lock.get("pg_textsearch_image_id","")
if not native_image_id.startswith("sha256:"):
    raise RuntimeError("native image lock has no immutable image ID")
actual_id=subprocess.check_output([
    "docker","image","inspect","--format={{.Id}}",
    cfg["native_image"],
],text=True).strip()
if actual_id!=native_image_id:
    raise RuntimeError(
        "native image tag does not match TIN lock: "
        +actual_id+" != "+native_image_id
    )
def image_proof(reference):
    info=json.loads(subprocess.check_output(
        ["docker","image","inspect",reference],text=True,
    ))[0]
    return {
        "id":info.get("Id"),
        "architecture":info.get("Architecture"),
        "os":info.get("Os"),
        "created":info.get("Created"),
        "rootfs_layers":(info.get("RootFS") or {}).get("Layers"),
    }
native_image_proof=image_proof(native_image_id)
if manifest.is_file():
    prior=json.loads(manifest.read_text())
    valid=prior.get("status")=="complete"
    native_archives=0
    for artifact in prior.get("artifacts",[]):
        path=pathlib.Path(artifact.get("path",""))
        valid=(
            valid and path.is_file()
            and digest(path)==artifact.get("sha256")
        )
        if artifact.get("kind")=="native_image":
            native_archives+=1
    if valid and prior.get("artifacts") and native_archives==1:
        native=next(
            item for item in prior["artifacts"]
            if item.get("kind")=="native_image"
        )
        native_path=pathlib.Path(native["path"])
        pipe(
            "docker image save "+shlex.quote(native_image_id)
            +" | zstd -T8 -1",
            native_path,
        )
        native["sha256"]=digest(native_path)
        native["image_id"]=native_image_id
        native["image_proof"]=native_image_proof
        temporary=manifest.with_suffix(".json.partial")
        temporary.write_text(json.dumps(prior,indent=2)+"\n")
        temporary.replace(manifest)
        print(manifest.read_text(),end="")
        raise SystemExit
artifacts=[]
checkout=out/"frozen-checkout.tar.zst"
pipe(
    "tar -C "+shlex.quote(str(repo.parent))+" -cpf - "
    +shlex.quote(repo.name)+" | zstd -T8 -1",
    checkout,
)
artifacts.append({
    "name":checkout.name,"path":str(checkout),"kind":"checkout",
    "engines":["pg_textsearch","paradedb"],"sha256":digest(checkout),
})
metadata=out/"worker-metadata.tar.zst"
items=["tools/benchmarker","tin-images.lock.json"]
for dataset in ("msmarco-v2","wikipedia-20231101-en"):
    items.append(dataset)
for run_id in cfg["run_ids"]:
    items.extend([
        "runs/"+run_id+"/tin/build",
        "runs/"+run_id+"/tin/query-packs",
        "runs/"+run_id+"/tin/plan.json",
    ])
pipe(
    "tar -C "+shlex.quote(str(root))
    +" --exclude='*/raw' --exclude='*/data'"
    +" --exclude='*/checkpoints' --exclude='*/lead' -cpf - "
    +" ".join(shlex.quote(item) for item in items)
    +" | zstd -T8 -1",
    metadata,
)
artifacts.append({
    "name":metadata.name,"path":str(metadata),"kind":"metadata",
    "engines":["pg_textsearch","paradedb"],"sha256":digest(metadata),
})
image=out/"native-image.tar.zst"
pipe(
    "docker image save "+shlex.quote(native_image_id)
    +" | zstd -T8 -1",
    image,
)
artifacts.append({
    "name":image.name,"path":str(image),"kind":"native_image",
    "engines":["pg_textsearch"],"sha256":digest(image),
    "image_id":native_image_id,
    "image_proof":native_image_proof,
})
volumes=[]
for name in subprocess.check_output(
    ["docker","volume","ls","--format","{{.Name}}"],text=True
).splitlines():
    info,=json.loads(subprocess.check_output(
        ["docker","volume","inspect",name],text=True
    ))
    labels=info.get("Labels") or {}
    if (
        labels.get("managed-by")!="pg_textsearch-tin"
        or labels.get("run-id") not in cfg["run_ids"]
        or labels.get("kind")!="analyzer"
    ):
        continue
    if subprocess.check_output(
        ["docker","ps","-aq","--filter","volume="+name],text=True
    ).strip():
        raise RuntimeError("analyzer volume is attached: "+name)
    proof=subprocess.check_output([
        "docker","run","--rm","--entrypoint","pg_controldata",
        "--mount","type=volume,source="+name+",target=/proof,readonly",
        cfg["native_image"],"/proof/18/docker",
    ],text=True)
    if "Database cluster state:               shut down" not in proof:
        raise RuntimeError("unclean analyzer volume: "+name)
    archive=out/(name+".tar.zst")
    mount=pathlib.Path(info["Mountpoint"])
    pipe(
        "sudo -A tar -C "+shlex.quote(str(mount))
        +" -cpf - . | zstd -T8 -1",
        archive,
    )
    artifact={
        "name":archive.name,"path":str(archive),"kind":"volume",
        "engines":[labels["engine"]],"sha256":digest(archive),
        "volume":{"name":name,"labels":labels},
    }
    artifacts.append(artifact)
    volumes.append(artifact)
if len(volumes)!=6:
    raise RuntimeError(
        "expected six analyzer volumes, found "+str(len(volumes))
    )
source_files={
    str(path.relative_to(repo)):digest(path)
    for path in sorted(repo.rglob("*"))
    if path.is_file() and ".git" not in path.relative_to(repo).parts
}
artifacts[0]["files"]=source_files
frozen=out/"frozen-source.json"
frozen.write_text(json.dumps({
    "format":1,"source_ref":cfg["source_ref"],
    "files":source_files,
},sort_keys=True,indent=2)+"\n")
artifacts.append({
    "name":frozen.name,"path":str(frozen),
    "kind":"source_manifest",
    "engines":["pg_textsearch","paradedb"],
    "sha256":digest(frozen),
})
value={
    "format":1,"status":"complete","source_ref":cfg["source_ref"],
    "native_source":cfg["native_source"],
    "source_files":source_files,
    "artifacts":artifacts,
}
temporary=manifest.with_suffix(".json.partial")
temporary.write_text(json.dumps(value,indent=2)+"\n")
temporary.replace(manifest)
print(json.dumps(value))
'''.replace("__SPEC__", repr(json.dumps(spec)))
        return (
            "set -eu; python3 - <<'PY'\n"
            + script
            + "\nPY"
        )

    def export(self):
        if self.stage_path("export").is_file():
            return self.read_stage("export")
        self.read_stage("prepare")
        source = self.host("host1")
        manifest = json.loads(
            self.backend.run(source, self._export_command())
        )
        artifacts = manifest.get("artifacts")
        if not isinstance(artifacts, list) or not artifacts:
            raise ControlError("source export manifest has no artifacts")
        for artifact in artifacts:
            if (
                not isinstance(artifact.get("path"), str)
                or not re.fullmatch(
                    r"[0-9a-f]{64}", artifact.get("sha256", "")
                )
            ):
                raise ControlError("source export manifest is invalid")
        self.write_stage("export", {
            "status": "complete",
            "source": source["host"],
            "artifacts": artifacts,
            "manifest": manifest,
        })
        return self.read_stage("export")

    def stage(self):
        if self.stage_path("stage").is_file():
            return self.read_stage("stage")
        exported = self.read_stage("export")
        source = self.host("host1")
        staging = self.host("host4")
        results = {}
        try:
            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = {
                    pool.submit(
                        self.transfer, source, staging, artifact
                    ): artifact["name"]
                    for artifact in exported["artifacts"]
                }
                for future in as_completed(futures):
                    results[futures[future]] = future.result()
        except (
            ControlError,
            OSError,
            subprocess.CalledProcessError,
        ) as error:
            _atomic_json(
                self.state_dir / "control/stage-error.json",
                {
                    "status": "failed",
                    "error": str(error),
                    "completed": results,
                },
            )
            raise
        self.write_stage("stage", {
            "status": "complete",
            "source": source["host"],
            "staging": staging["host"],
            "artifacts": exported["artifacts"],
            "transfers": results,
            "source_bulk_io_complete": True,
        })
        return self.read_stage("stage")

    @staticmethod
    def _artifacts_for_host(host, artifacts):
        return [
            artifact for artifact in artifacts
            if (
                "engines" not in artifact
                or host["engine"] in artifact["engines"]
            )
        ]

    def _import_command(self, host, artifacts):
        root = self.config["root"]
        repo = self.config["remote_repo"]
        lines = [
            "set -eu",
            "sudo -A "
            + shlex.quote(repo + "/benchmarks/scale/install.sh"),
            "python3 -m venv " + shlex.quote(root + "/venv"),
            shlex.quote(root + "/venv/bin/pip")
            + " install -r "
            + shlex.quote(
                repo + "/benchmarks/scale/requirements.txt"
            ),
            'test -z "$(sudo -A docker ps -q)"',
        ]
        database_image = (
            f"pg_textsearch-scale:{self.config['native_source']}"
            if host["engine"] == "pg_textsearch"
            else PARADEDB_IMAGE
        )
        if host["engine"] == "paradedb":
            lines.append(
                "sudo -A docker pull "
                + shlex.quote(PARADEDB_IMAGE)
            )
        source_manifests = [
            item for item in artifacts
            if item["kind"] == "source_manifest"
        ]
        if len(source_manifests) != 1:
            raise ControlError(
                "worker import requires one frozen source manifest"
            )
        source_manifest = source_manifests[0]["path"]
        for artifact in artifacts:
            path = shlex.quote(artifact["path"])
            if artifact["kind"] == "checkout":
                marker = shlex.quote(artifact["path"] + ".imported")
                verifier = (
                    "import hashlib,json,pathlib,sys;"
                    "r=pathlib.Path(sys.argv[1]);"
                    "e=json.loads(pathlib.Path(sys.argv[2]).read_text())"
                    "['files'];"
                    "f=[p for p,d in e.items() if not (r/p).is_file() "
                    "or hashlib.sha256((r/p).read_bytes()).hexdigest()!=d];"
                    "f and (_ for _ in ()).throw("
                    "RuntimeError('frozen source mismatch:'+','.join(f[:5])))"
                )
                lines.append(
                    "if ! test -e " + shlex.quote(repo)
                    + "; then zstd -dc " + path
                    + " | sudo -A tar -C "
                    + shlex.quote(str(Path(repo).parent))
                    + " -xpf -; fi; python3 -c "
                    + shlex.quote(verifier) + " "
                    + shlex.quote(repo) + " "
                    + shlex.quote(source_manifest)
                    + "; touch " + marker
                )
            elif artifact["kind"] == "metadata":
                marker = shlex.quote(artifact["path"] + ".imported")
                build_paths = [
                    root + "/runs/" + run_id + "/tin/build"
                    for run_id in (
                        self.config["msmarco_run_id"],
                        self.config["wikipedia_run_id"],
                    )
                ]
                absence = " && test ".join(
                    "! -e " + shlex.quote(path)
                    for path in build_paths
                )
                lines.append(
                    "if ! test -f " + marker + "; then test "
                    + absence + "; zstd -dc " + path
                    + " | sudo -A tar -C "
                    + shlex.quote(root) + " -xpf -; touch "
                    + marker + "; fi"
                )
            elif artifact["kind"] == "native_image":
                image_id = artifact.get("image_id")
                if not (
                    isinstance(image_id, str)
                    and image_id.startswith("sha256:")
                ):
                    raise ControlError(
                        "native image artifact has no locked image ID"
                    )
                proof = artifact.get("image_proof")
                if not isinstance(proof, dict):
                    raise ControlError(
                        "native image artifact has no image proof"
                    )
                expected = base64.b64encode(
                    json.dumps(proof, sort_keys=True).encode()
                ).decode()
                verifier = (
                    "import base64,json,sys;"
                    "e=json.loads(base64.b64decode(sys.argv[1]));"
                    "i=json.load(sys.stdin)[0];"
                    "a={'id':i.get('Id'),"
                    "'architecture':i.get('Architecture'),"
                    "'os':i.get('Os'),'created':i.get('Created'),"
                    "'rootfs_layers':(i.get('RootFS') or {}).get('Layers')};"
                    "assert a==e,(a,e)"
                )
                lines.append(
                    "zstd -dc " + path
                    + " | sudo -A docker load; "
                    + "sudo -A docker image inspect "
                    + shlex.quote(image_id)
                    + " | python3 -c " + shlex.quote(verifier)
                    + " " + shlex.quote(expected)
                    + "; native_tag_id=\"$(sudo -A docker image ls "
                    "--no-trunc --format '{{.ID}}' --filter "
                    + shlex.quote("reference=" + database_image)
                    + ")\"; if test -z \"$native_tag_id\"; then "
                    "sudo -A docker image tag "
                    + shlex.quote(image_id) + " "
                    + shlex.quote(database_image)
                    + "; elif test \"$native_tag_id\" != "
                    + shlex.quote(image_id)
                    + "; then echo 'native image tag points to a "
                    "foreign image' >&2; exit 1; fi"
                )
            elif artifact["kind"] == "source_manifest":
                lines.append(
                    "test -s " + path
                    + "; test \"$(sha256sum " + path
                    + " | awk '{print $1}')\" = "
                    + shlex.quote(artifact["sha256"])
                )
            elif artifact["kind"] == "volume":
                volume = artifact["volume"]
                name = volume["name"]
                labels = " ".join(
                    "--label " + shlex.quote(f"{key}={value}")
                    for key, value in sorted(
                        volume["labels"].items()
                    )
                )
                mount = (
                    "/srv/searchbench/docker/volumes/"
                    + name + "/_data"
                )
                marker = artifact["path"] + ".imported"
                lines.append(
                    "if ! test -f " + shlex.quote(marker) + "; then "
                    "if sudo -A docker volume inspect " + shlex.quote(name)
                    + " >/dev/null 2>&1; then "
                    "echo 'uncheckpointed volume exists' >&2; exit 1; fi; "
                    "sudo -A docker volume create " + labels + " "
                    + shlex.quote(name) + "; "
                    "zstd -dc " + path
                    + " | sudo -A tar --numeric-owner --same-owner -C "
                    + shlex.quote(mount) + " -xpf -; "
                    "sudo -A docker run --rm --entrypoint pg_controldata "
                    "--mount "
                    + shlex.quote(
                        "type=volume,source=" + name
                        + ",target=/proof,readonly"
                    )
                    + " " + shlex.quote(database_image)
                    + " /proof/18/docker > "
                    + shlex.quote(marker + ".control")
                    + "; grep -q 'Database cluster state: *shut down' "
                    + shlex.quote(marker + ".control") + "; "
                    "touch " + shlex.quote(marker) + "; fi"
                )
        return "; ".join(lines)

    def distribute(self):
        if self.stage_path("distribute").is_file():
            return self.read_stage("distribute")
        staged = self.read_stage("stage")
        staging = self.host(staged["staging"])
        artifacts = staged["artifacts"]
        targets = [
            host for host in self.hosts()
            if host["host"] not in {"host1", "host4"}
        ]

        def fanout(target):
            transfers = {}
            selected = self._artifacts_for_host(target, artifacts)
            for artifact in selected:
                transfers[artifact["name"]] = self.transfer(
                    staging, target, artifact
                )
            self.backend.deploy_source(
                target,
                self.config["source_ref"],
                self.config["remote_repo"],
            )
            self.backend.run(
                target, self._import_command(target, selected)
            )
            return transfers

        completed = {}
        try:
            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = {
                    pool.submit(fanout, target): target["host"]
                    for target in targets
                }
                for future in as_completed(futures):
                    completed[futures[future]] = future.result()
        except (
            ControlError,
            OSError,
            subprocess.CalledProcessError,
        ) as error:
            _atomic_json(
                self.state_dir / "control/distribute-error.json",
                {
                    "status": "failed",
                    "error": str(error),
                    "completed": completed,
                },
            )
            raise
        staging_artifacts = self._artifacts_for_host(
            staging, artifacts
        )
        self.backend.deploy_source(
            staging,
            self.config["source_ref"],
            self.config["remote_repo"],
        )
        self.backend.run(
            staging,
            self._import_command(staging, staging_artifacts),
        )
        completed[staging["host"]] = {
            artifact["name"]: "staged"
            for artifact in staging_artifacts
        }
        self.write_stage("distribute", {
            "status": "complete",
            "staging_import_after_fanout": True,
            "hosts": completed,
        })
        return self.read_stage("distribute")

    def _worker_command(self, host, action):
        root = self.config["root"]
        repo = self.config["remote_repo"]
        run_ids = {
            "msmarco-v2": self.config["msmarco_run_id"],
            "wikipedia": self.config["wikipedia_run_id"],
        }
        if action == "run":
            commands = []
            for dataset, run_id in run_ids.items():
                command = shlex.join([
                        f"{root}/venv/bin/python",
                        "-m",
                        "benchmarks.scale.fleet",
                        "--engine",
                        host["engine"],
                        "--profile",
                        host["profile"],
                        "--dataset",
                        dataset,
                        "--run-id",
                        run_id,
                        "--root",
                        repo,
                        "--artifacts",
                        root,
                    ])
                commands.append(
                    "result=0; " + command + " || result=$?; "
                    "case \"$result\" in 0) ;; 2) overall=2 ;; "
                    "*) exit \"$result\" ;; esac"
                )
            worker = (
                "set -eu; export PYTHONPATH=" + shlex.quote(repo)
                + "; export PGTS_NATIVE_SOURCE="
                + self.config["native_source"]
                + "; export PGTS_SQL_SHAPE="
                + shlex.quote(self.config["sql_shape"])
                + "; export PGTS_CACHE_RESET_HOOK="
                + shlex.quote(repo + "/benchmarks/scale/cache_reset.sh")
                + "; overall=0; " + "; ".join(commands)
                + "; exit \"$overall\""
            )
            unit = (
                "pgts-tin-fleet-"
                + host["engine"].replace("_", "-")
                + "-" + host["profile"]
            )
            identity = json.dumps({
                "campaign_id": self.config["campaign_id"],
                "source": self.source_selection(),
                "worker": worker,
            }, sort_keys=True).encode()
            description = (
                "pg_textsearch-scale:" + hashlib.sha256(identity).hexdigest()
            )
            log = root + "/fleet-control/" + unit + ".log"
            return (
                "set -eu; mkdir -p "
                + shlex.quote(root + "/fleet-control")
                + "; if ! systemctl cat " + shlex.quote(unit)
                + " >/dev/null 2>&1; then "
                + "sudo -A systemd-run --unit="
                + shlex.quote(unit)
                + " --description=" + shlex.quote(description)
                + " --uid=bench --gid=bench "
                + "--property=SuccessExitStatus=2 "
                + "--property=StandardOutput=append:"
                + shlex.quote(log)
                + " --property=StandardError=append:"
                + shlex.quote(log)
                + " bash -lc " + shlex.quote(worker)
                + "; else description=$(systemctl show " + shlex.quote(unit)
                + " -p Description --value); "
                + "[ \"$description\" = " + shlex.quote(description)
                + " ] || { echo 'foreign or legacy worker service; "
                + "explicit reconciliation required' >&2; exit 1; }; "
                + "state=$(systemctl show " + shlex.quote(unit)
                + " -p ActiveState --value); "
                + "case \"$state\" in failed|inactive) "
                + "sudo -A systemctl restart " + shlex.quote(unit)
                + " ;; active|activating|deactivating|reloading) ;; "
                + "*) echo 'unexpected worker service state' >&2; exit 1 ;; "
                + "esac; fi; systemctl show " + shlex.quote(unit)
                + " -p LoadState -p ActiveState -p SubState "
                + "-p ExecMainStatus"
            )
        if action == "preflight":
            spec = {
                "root": root,
                "repo": repo,
                "engine": host["engine"],
                "profile": host["profile"],
                "native_source": self.config["native_source"],
                "sql_shape": self.config["sql_shape"],
                "runs": {
                    "msmarco-v2": self.config["msmarco_run_id"],
                    "wikipedia": self.config["wikipedia_run_id"],
                },
            }
            script = r'''
import json,os,pathlib,subprocess
cfg=json.loads(__SPEC__)
os.environ["PGTS_NATIVE_SOURCE"]=cfg["native_source"]
os.environ["PGTS_SQL_SHAPE"]=cfg["sql_shape"]
root=pathlib.Path(cfg["root"])
repo=pathlib.Path(cfg["repo"])
import sys
sys.path.insert(0,str(repo))
from benchmarks.scale.fleet import (
    fleet_plan,shard_cells,validate_imported_inputs,
)
from benchmarks.scale.tin import TinRunner
if subprocess.check_output(["docker","ps","-q"],text=True).strip():
    raise RuntimeError("containers are running before preflight")
proofs=[]
for dataset,run_id in cfg["runs"].items():
    runner=TinRunner(
        repo,root,False,run_id,dataset,None,
    )
    plan=fleet_plan(dataset)
    runner._write_or_validate_json(runner.tin_dir/"plan.json",plan)
    cells=shard_cells(
        plan,cfg["engine"],cfg["profile"],
    )
    validate_imported_inputs(runner,cfg["engine"],cells)
    images,_=runner._images()
    for analyzer in sorted({cell["analyzer"] for cell in cells}):
        source=runner._volume_name(
            "analyzer",cfg["engine"],analyzer,
        )
        target=runner._volume_name(
            "profile",cfg["engine"],
            analyzer+"-"+cfg["profile"],
        )
        phase=(
            runner.tin_dir/"phases"/cfg["profile"]
            /cfg["engine"]/analyzer
        )
        phase.mkdir(parents=True,exist_ok=True)
        runner._clone_volume(
            source,target,{
                "kind":"profile","engine":cfg["engine"],
                "analyzer":analyzer,"profile":cfg["profile"],
            },phase/"clone.json",image=images[cfg["engine"]],
            source_labels={
                "kind":"analyzer","engine":cfg["engine"],
                "analyzer":analyzer,
            },
        )
        name=(
            "pgts-fleet-preflight-"
            +cfg["engine"].replace("_","-")+"-"
            +dataset.replace("_","-")+"-"
            +analyzer.replace("_","-")
        )
        runner._start_database(
            name=name,volume=target,image=images[cfg["engine"]],
            profile=cfg["profile"],engine=cfg["engine"],
            output=phase/"fleet-preflight-start.log",
        )
        try:
            extension=(
                "pg_textsearch"
                if cfg["engine"]=="pg_textsearch"
                else "pg_search"
            )
            sql=(
                "SELECT e.extname,e.extversion FROM pg_extension e "
                "JOIN pg_available_extensions a ON a.name=e.extname "
                "WHERE e.extname='"+extension+"' "
                "AND e.extversion=a.default_version;"
                "SELECT relname,relkind,reltuples::bigint "
                "FROM pg_class WHERE oid IN "
                "('documents'::regclass,'documents_bm25'::regclass) "
                "ORDER BY relname;"
                "SELECT indisvalid AND indisready FROM pg_index "
                "WHERE indexrelid='documents_bm25'::regclass;"
                "SELECT id,length(body) FROM documents "
                "ORDER BY id LIMIT 1;"
                "SELECT system_identifier FROM pg_control_system()"
            )
            text=subprocess.check_output([
                "docker","exec",name,"psql","-U","postgres",
                "-XAt","-F","\t","-v","ON_ERROR_STOP=1",
                "-c",sql,
            ],text=True)
            versions=[
                line.split("\t",1)[1]
                for line in text.splitlines()
                if line.startswith(extension+"\t")
            ]
            if (
                len(versions)!=1 or not versions[0]
                or (cfg["engine"]=="paradedb" and versions[0]!="0.27.0")
            ):
                raise RuntimeError("extension version proof failed")
            if "\nt\n" not in "\n"+text:
                raise RuntimeError("index is not valid and ready")
            proofs.append({
                "dataset":dataset,"analyzer":analyzer,
                "profile_volume":target,"catalog":text,
            })
        finally:
            runner._stop_database(
                name,phase/"fleet-preflight-postgres.log",
            )
hardware={
    "hostname":subprocess.check_output(
        ["hostname","-f"],text=True,
    ).strip(),
    "uname":subprocess.check_output(["uname","-a"],text=True).strip(),
    "cpu":json.loads(subprocess.check_output(
        ["lscpu","--json"],text=True,
    )),
    "block_devices":json.loads(subprocess.check_output(
        ["lsblk","--json","-b"],text=True,
    )),
    "memory_bytes":os.sysconf("SC_PAGE_SIZE")*os.sysconf("SC_PHYS_PAGES"),
}
if os.cpu_count() is None or os.cpu_count()<8:
    raise RuntimeError("worker has fewer than eight logical CPUs")
output=root/"fleet-control"
output.mkdir(parents=True,exist_ok=True)
path=output/(
    "preflight-"+cfg["engine"]+"-"+cfg["profile"]+".json"
)
temporary=path.with_suffix(".json.partial")
temporary.write_text(json.dumps({
    "status":"ok","engine":cfg["engine"],
    "profile":cfg["profile"],"hardware":hardware,
    "proofs":proofs,
},indent=2)+"\n")
temporary.replace(path)
print(path.read_text())
'''.replace("__SPEC__", repr(json.dumps(spec)))
            return (
                "set -eu; test -f /opt/searchbench/bootstrap.ready; "
                + shlex.quote(root + "/venv/bin/python")
                + " - <<'PY'\n" + script + "\nPY"
            )
        if action == "retain":
            spec = {
                "root": root,
                "engine": host["engine"],
                "run_ids": list(run_ids.values()),
                "paradedb": PARADEDB_IMAGE,
            }
            script = r'''
import json,pathlib,subprocess,time
cfg=json.loads(__SPEC__)
root=pathlib.Path(cfg["root"])
lock=json.loads((root/"tin-images.lock.json").read_text())
image=(
    lock["pg_textsearch_image_id"]
    if cfg["engine"]=="pg_textsearch"
    else cfg["paradedb"]
)
selected=[]
for name in subprocess.check_output(
    ["docker","volume","ls","--format","{{.Name}}"],text=True
).splitlines():
    info,=json.loads(subprocess.check_output(
        ["docker","volume","inspect",name],text=True
    ))
    labels=info.get("Labels") or {}
    if (
        labels.get("managed-by")=="pg_textsearch-tin"
        and labels.get("run-id") in cfg["run_ids"]
        and labels.get("kind")=="analyzer"
        and labels.get("engine")==cfg["engine"]
    ):
        selected.append((name,labels))
if len(selected)!=3:
    raise RuntimeError("expected three retained analyzer volumes")
for offset,(volume,labels) in enumerate(sorted(selected),start=0):
    port=16432+offset
    name=(
        "pgts-retained-"+cfg["engine"].replace("_","-")
        +"-"+str(port)
    )
    subprocess.run(
        ["docker","volume","inspect",volume],
        check=True,capture_output=True,text=True,
    )
    existing=subprocess.run(
        ["docker","container","inspect",name],
        capture_output=True,text=True,
    )
    if existing.returncode==0:
        info=json.loads(existing.stdout)[0]
        config=info.get("Config") or {}
        actual_labels=config.get("Labels") or {}
        mounts=info.get("Mounts") or []
        bindings=(
            (info.get("HostConfig") or {})
            .get("PortBindings",{}).get("5432/tcp") or []
        )
        if (
            info.get("Name")!="/"+name
            or
            actual_labels.get("managed-by")
            !="pg_textsearch-tin-retained"
            or actual_labels.get("source-volume")!=volume
            or config.get("Image")!=image
            or not any(
                item.get("Name")==volume
                and item.get("Destination")=="/var/lib/postgresql"
                for item in mounts
            )
            or bindings!=[{
                "HostIp":"127.0.0.1",
                "HostPort":str(port),
            }]
        ):
            raise RuntimeError(
                "refusing incompatible retained container: "+name
            )
        if not (info.get("State") or {}).get("Running"):
            subprocess.run(
                ["docker","start",name],check=True,
            )
    else:
        args=[
            "docker","run","-d","--name",name,
            "--label","managed-by=pg_textsearch-tin-retained",
            "--label","source-volume="+volume,
            "--mount","type=volume,source="+volume
            +",target=/var/lib/postgresql",
            "-e","POSTGRES_PASSWORD=postgres",
            "-p","127.0.0.1:"+str(port)+":5432",
            "--shm-size=2g",image,
            "-c","shared_buffers=1GB",
            "-c","maintenance_work_mem=256MB",
            "-c","random_page_cost=1.1",
        ]
        if cfg["engine"]=="pg_textsearch":
            args.extend([
                "-c","shared_preload_libraries=pg_textsearch",
            ])
        subprocess.run(args,check=True)
    deadline=time.monotonic()+120
    while True:
        probe=subprocess.run([
            "docker","exec",name,"pg_isready","-U","postgres",
        ],stdout=subprocess.DEVNULL)
        if probe.returncode==0:
            break
        if time.monotonic()>=deadline:
            raise RuntimeError("retained database did not become ready")
        time.sleep(1)
print(json.dumps({
    "status":"ok","ports":[16432,16433,16434],
    "engine":cfg["engine"],
}))
'''.replace("__SPEC__", repr(json.dumps(spec)))
            return "set -eu; python3 - <<'PY'\n" + script + "\nPY"
        raise ControlError(f"unknown worker action: {action}")

    def preflight(self, *, source_only=False):
        name = "preflight-source" if source_only else "preflight"
        if self.stage_path(name).is_file():
            return self.read_stage(name)
        self.read_stage("stage")
        if not source_only:
            self.read_stage("distribute")
        hosts = (
            [self.host("host1")]
            if source_only
            else [host for host in self.hosts()
                  if host["host"] != "host1"]
        )
        for host in hosts:
            self.backend.run(
                host, self._worker_command(host, "preflight")
            )
        self.write_stage(name, {
            "status": "complete",
            "hosts": [host["host"] for host in hosts],
        })
        return self.read_stage(name)

    def run(self, *, source_only=False):
        name = "run-source" if source_only else "run"
        prior = None
        if self.stage_path(name).is_file():
            prior = self.read_stage(name)
        self.read_stage(
            "preflight-source" if source_only else "preflight"
        )
        partition = self.plan()["partition"]
        hosts = (
            [self.host("host1")]
            if source_only
            else [host for host in self.hosts()
                  if host["host"] != "host1"]
        )
        if prior is not None:
            missing = {tuple(key) for key in self.status()["missing"]}
            needed = {
                row["host"] for row in partition
                if (row["dataset"], row["engine"], row["cell_id"]) in missing
            }
            hosts = [host for host in hosts if host["host"] in needed]
            if not hosts:
                return prior
        launched = []
        for host in hosts:
            self.backend.run(host, self._worker_command(host, "run"))
            launched.append(host["host"])
        self.write_stage(name, {
            "status": "complete",
            "hosts": launched,
        })
        return self.read_stage(name)

    def status(self):
        partition = self.plan()["partition"]
        rows = []
        services = {}
        host_errors = {}
        for host in self.hosts():
            try:
                records = json.loads(
                    self.backend.run(
                        host, self._status_command(host)
                    )
                )
            except (
                ControlError,
                subprocess.CalledProcessError,
                json.JSONDecodeError,
                OSError,
            ) as error:
                host_errors[host["host"]] = str(error)
                records = []
            for record in records:
                if len(record.get("markers", ())) == 1:
                    rows.append(record | {"host": host["host"]})
            unit = (
                "pgts-tin-fleet-"
                + host["engine"].replace("_", "-")
                + "-" + host["profile"]
            )
            try:
                text = self.backend.run(
                    host,
                    "systemctl show " + shlex.quote(unit)
                    + " -p LoadState -p ActiveState -p SubState "
                    + "-p ExecMainStatus",
                )
                services[host["host"]] = dict(
                    line.split("=", 1)
                    for line in text.splitlines()
                    if "=" in line
                )
            except (
                ControlError,
                subprocess.CalledProcessError,
                OSError,
            ) as error:
                services[host["host"]] = {
                    "error": str(error),
                }
                host_errors.setdefault(host["host"], str(error))
        summary = self.reconcile(rows, allow_missing=True)
        summary["services"] = services
        summary["host_errors"] = host_errors
        self.write_stage("status", {
            "status": "complete",
            "partition_size": len(partition),
            **summary,
        })
        return summary

    def wait(self):
        deadline_text = self.manifest().get("common_deadline")
        deadline = _utc_instant(deadline_text, "fleet common")
        while True:
            status = self.status()
            if not status["missing"]:
                self.write_stage("wait", {
                    "status": "complete",
                    "observed": status["observed"],
                })
                return status
            expected_hosts = {
                (
                    row["dataset"],
                    row["engine"],
                    row["cell_id"],
                ): row["host"]
                for row in self.plan()["partition"]
            }
            missing_hosts = {
                expected_hosts.get(tuple(key))
                for key in status["missing"]
            }
            unreachable = sorted(
                host for host in missing_hosts
                if host in status.get("host_errors", {})
            )
            if unreachable:
                details = {
                    host: status["host_errors"][host]
                    for host in unreachable
                }
                raise ControlError(
                    "required fleet host is unreachable: "
                    + json.dumps(details, sort_keys=True)
                )
            for host, service in status["services"].items():
                if (
                    host in missing_hosts
                    and service.get("ActiveState")
                    in {"failed", "inactive"}
                ):
                    raise ControlError(
                        f"fleet worker failed on {host}: {service}"
                    )
            if dt.datetime.now(dt.timezone.utc) >= deadline:
                raise ControlError(
                    "fleet deadline reached before terminal coverage"
                )
            time.sleep(60)

    def _status_command(self, host):
        root = self.config["root"]
        runs = {
            "msmarco-v2": self.config["msmarco_run_id"],
            "wikipedia": self.config["wikipedia_run_id"],
        }
        operations = {}
        for row in self.plan()["partition"]:
            if row["host"] == host["host"]:
                operations.setdefault(row["dataset"], {})[
                    row["cell_id"]
                ] = row["operation"]
        script = r'''
from pathlib import Path
import json
root=Path(__ROOT__)
runs=json.loads(__RUNS__)
operations=json.loads(__OPERATIONS__)
engine=__ENGINE__
rows=[]
for dataset,run_id in runs.items():
    results=root/"runs"/run_id/"tin"/"results"/engine
    if not results.is_dir():
        continue
    for cell in sorted(results.iterdir()):
        if not cell.is_dir():
            continue
        markers=[
            name for name in (
                "complete.json","failed.json",
                "plan-gate-failed.json","plan-audit-failed.json",
            )
            if (cell/name).is_file()
        ]
        operation=operations.get(dataset,{}).get(cell.name)
        if (
            markers==["complete.json"]
            and operation=="concurrent-writes"
            and not (cell/"write-cleanup.json").is_file()
        ):
            continue
        rows.append({
            "dataset":dataset,"engine":engine,
            "cell_id":cell.name,"operation":operation,
            "markers":markers,
        })
print(json.dumps(rows))
'''
        script = (
            script.replace("__ROOT__", repr(root))
            .replace("__RUNS__", repr(json.dumps(runs)))
            .replace("__OPERATIONS__", repr(json.dumps(operations)))
            .replace("__ENGINE__", repr(host["engine"]))
        )
        return "python3 - <<'PY'\n" + script + "\nPY"

    def reconcile(self, rows, *, allow_missing=False):
        expected = {
            (row["dataset"], row["engine"], row["cell_id"]): row["host"]
            for row in self.plan()["partition"]
        }
        observed = {}
        duplicates = []
        wrong_host = []
        for row in rows:
            key = (
                row.get("dataset"),
                row.get("engine"),
                row.get("cell_id"),
            )
            if key in observed:
                duplicates.append(key)
            observed[key] = row
            if key in expected and row.get("host") != expected[key]:
                wrong_host.append(key)
        missing = sorted(set(expected) - set(observed))
        extras = sorted(set(observed) - set(expected))
        if (
            duplicates or wrong_host or extras
            or (missing and not allow_missing)
        ):
            raise ControlError(
                "fleet partition mismatch in observed results"
            )
        return {
            "expected": len(expected),
            "observed": len(observed),
            "missing": missing,
            "extras": extras,
            "duplicates": duplicates,
            "wrong_host": wrong_host,
        }

    def collect(self):
        if self.stage_path("collect").is_file():
            return self.read_stage("collect")
        status = self.status()
        if status["missing"]:
            raise ControlError("cannot collect an incomplete fleet")
        rows = []
        provenance = {}
        for host in self.hosts():
            proof_path = (
                self.config["root"] + "/fleet-control/preflight-"
                + host["engine"] + "-" + host["profile"] + ".json"
            )
            provenance[host["host"]] = json.loads(
                self.backend.run(
                    host, "cat " + shlex.quote(proof_path)
                )
            )
            for dataset, run_id in (
                ("msmarco-v2", self.config["msmarco_run_id"]),
                ("wikipedia", self.config["wikipedia_run_id"]),
            ):
                run_dir = (
                    self.config["root"] + "/runs/" + run_id + "/tin"
                )
                command = shlex.join([
                    self.config["root"] + "/venv/bin/python",
                    self.config["remote_repo"]
                    + "/benchmarks/scale/summarize_tin.py",
                    run_dir,
                    "--engine",
                    host["engine"],
                ])
                text = self.backend.run(host, command)
                for record in csv.DictReader(io.StringIO(text)):
                    record.update({
                        "dataset": dataset,
                        "host": host["host"],
                    })
                    rows.append(record)
        reconciliation = self.reconcile(rows)
        output = {
            "status": "complete",
            "reconciliation": reconciliation,
            "provenance": provenance,
            "rows": rows,
        }
        self.write_stage("collect", output)
        return output

    def retain(self):
        if self.stage_path("retain").is_file():
            return self.read_stage("retain")
        self.read_stage("collect")
        for host in self.hosts():
            self.backend.run(
                host, self._worker_command(host, "retain")
            )
        self.write_stage("retain", {
            "status": "complete",
            "hosts": [host["host"] for host in self.hosts()],
            "original_volumes_preserved": True,
        })
        return self.read_stage("retain")

    def all(self):
        self.provision()
        self.prepare()
        self.export()
        self.stage()
        self.preflight(source_only=True)
        self.run(source_only=True)
        self.distribute()
        self.preflight()
        self.run()
        self.wait()
        self.collect()
        self.retain()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action",
        choices=(
            "schema",
            "plan",
            "provision",
            "prepare",
            "export",
            "stage",
            "distribute",
            "preflight",
            "run",
            "wait",
            "status",
            "collect",
            "retain",
            "all",
        ),
    )
    parser.add_argument("--config", type=Path)
    parser.add_argument(
        "--source-ref", help="harness revision for schema output"
    )
    parser.add_argument(
        "--native-source", help="native commit for schema output"
    )
    parser.add_argument(
        "--sql-shape", choices=("original", "filter_first", "rank_first"),
        help="native SQL shape for schema output",
    )
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="apply preflight/run only to source host1",
    )
    args = parser.parse_args(argv)
    if args.action == "schema":
        schema = dict(SCHEMA)
        if args.source_ref is not None:
            schema["source_ref"] = args.source_ref
        if args.native_source is not None:
            try:
                schema["native_source"] = native_source(args.native_source)
            except ConfigError as error:
                parser.error(str(error))
        if args.sql_shape is not None:
            schema["sql_shape"] = args.sql_shape
        print(json.dumps(schema, indent=2))
        return 0
    if any(value is not None for value in (
        args.source_ref, args.native_source, args.sql_shape,
    )):
        parser.error(
            "source and SQL selection flags are only valid for schema"
        )
    if args.config is None:
        parser.error("--config is required")
    try:
        control = FleetControl(args.config)
        if args.action in {"preflight", "run"}:
            result = getattr(control, args.action)(
                source_only=args.source_only
            )
        else:
            if args.source_only:
                parser.error(
                    "--source-only is valid only for preflight or run"
                )
            result = getattr(control, args.action)()
        if result is not None:
            print(json.dumps(result, indent=2))
        return 0
    except (
        ControlError,
        OSError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
