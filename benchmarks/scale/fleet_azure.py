#!/usr/bin/env python3
"""Provision and verify a six-host Azure benchmark fleet.

The private JSON config has this shape:

    {
      "subscription": "...",
      "group_prefix": "searchbench-fleet",
      "regions": ["region-a", "region-b"],
      "size": "Standard_L32as_v3",
      "lifetime_days": 7,
      "ssh_private_key": "/private/key",
      "ssh_public_key": "/private/key.pub",
      "ssh_cidr": "203.0.113.10/32",
      "existing_group": "searchbench-existing"
    }

``existing_group`` is optional. When present, host 1 is adopted only if
the resource group is already marked as managed by pg_textsearch-scale.
"""

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time


MARKER = "pg_textsearch-scale"
ROLES = (
    (1, "pg_textsearch", "high"),
    (2, "pg_textsearch", "constrained32"),
    (3, "pg_textsearch", "constrained16"),
    (4, "paradedb", "high"),
    (5, "paradedb", "constrained32"),
    (6, "paradedb", "constrained16"),
)
CAPACITY_ERRORS = {
    "OperationNotAllowed",
    "QuotaExceeded",
    "SkuNotAvailable",
}


class ConfigError(ValueError):
    """Fleet configuration or persisted state is unsafe."""


def _utc(value):
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    return dt.datetime.fromisoformat(value).astimezone(dt.timezone.utc)


def compute_deadline(now, days):
    if not 1 <= days <= 14:
        raise ConfigError("lifetime_days must be between 1 and 14")
    return (now + dt.timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")


def _vcpus(size, sku):
    for capability in sku.get("capabilities", []):
        if capability.get("name") == "vCPUs":
            return int(capability["value"])
    match = re.search(r"L(\d+)", size, re.IGNORECASE)
    if not match:
        raise ConfigError(f"cannot determine vCPUs for {size}")
    return int(match.group(1))


def region_capacity(skus, usage, size):
    sku = next((item for item in skus if item.get("name") == size), None)
    if not sku:
        raise ConfigError(f"{size} is not listed in this region")
    family = sku.get("family")
    if not family:
        raise ConfigError(f"{size} has no Azure SKU family")
    location_blocked = any(
        restriction.get("type") == "Location"
        for restriction in sku.get("restrictions", [])
    )
    by_name = {
        item.get("name", {}).get("value", "").lower(): item
        for item in usage
    }
    family_usage = by_name.get(family.lower())
    regional_usage = by_name.get("cores")
    if not family_usage or not regional_usage:
        raise ConfigError(f"quota is unavailable for {family}")
    family_remaining = (
        int(family_usage["limit"]) - int(family_usage["currentValue"])
    )
    regional_remaining = (
        int(regional_usage["limit"]) - int(regional_usage["currentValue"])
    )
    vcpus = _vcpus(size, sku)
    available = min(family_remaining, regional_remaining) // vcpus
    if location_blocked:
        available = 0
    return {
        "family": family,
        "family_remaining": family_remaining,
        "regional_remaining": regional_remaining,
        "available_hosts": max(0, available),
        "restrictions": sku.get("restrictions", []),
        "vcpus": vcpus,
    }


def remote_command(parts):
    return shlex.join(parts)


def _walk_devices(devices):
    seen = set()
    pending = list(devices)
    while pending:
        device = pending.pop()
        pending.extend(device.get("children") or [])
        path = device.get("path")
        if path and path not in seen:
            seen.add(path)
            yield device


def hardware_from_probe(probe):
    if not probe.get("bootstrap_ready"):
        raise ConfigError("bootstrap readiness marker is missing")
    if probe.get("docker_root") != "/srv/searchbench/docker":
        raise ConfigError("Docker is not using benchmark storage")
    devices = list(_walk_devices(
        probe.get("topology", {}).get("blockdevices", [])
    ))
    nvme = [
        device for device in devices
        if device.get("model") == "Microsoft NVMe Direct Disk"
    ]
    raids = [
        device for device in devices
        if device.get("type") == "raid0"
        and str(device.get("path", "")).startswith("/dev/md")
        and "/srv/searchbench" in (device.get("mountpoints") or [])
    ]
    if len(nvme) != 4:
        raise ConfigError("expected four Azure local NVMe disks")
    if len(raids) != 1:
        raise ConfigError("expected one mounted NVMe RAID0 device")
    return {
        "cpu_model": probe["cpu_model"],
        "kernel": probe["kernel"],
        "nvme_count": len(nvme),
        "nvme_raid_bytes": int(raids[0]["size"]),
        "docker_root": probe["docker_root"],
    }


def _azure_cli(subscription, *args):
    result = subprocess.run(
        [
            "az", *args, "--subscription", subscription,
            "-o", "json", "--only-show-errors",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(result.stdout) if result.stdout.strip() else None


class FleetAzure:
    def __init__(self, config, state_dir, *, azure=None):
        self.config = dict(config)
        self.state_dir = Path(state_dir)
        self.manifest_path = self.state_dir / "fleet-manifest.json"
        self.azure = azure or (
            lambda *args: _azure_cli(self.config["subscription"], *args)
        )
        self._validate_config()

    def _validate_config(self):
        required = {
            "subscription",
            "group_prefix",
            "regions",
            "size",
            "lifetime_days",
            "ssh_private_key",
            "ssh_public_key",
            "ssh_cidr",
        }
        missing = sorted(required - self.config.keys())
        if missing:
            raise ConfigError(f"missing config fields: {', '.join(missing)}")
        if not self.config["regions"]:
            raise ConfigError("regions must not be empty")
        if "searchbench-" not in self.config["group_prefix"]:
            raise ConfigError("group_prefix must contain searchbench-")
        compute_deadline(
            dt.datetime.now(dt.timezone.utc),
            int(self.config["lifetime_days"]),
        )

    def _identity(self):
        return {
            "subscription": self.config["subscription"],
            "group_prefix": self.config["group_prefix"],
            "regions": list(self.config["regions"]),
            "size": self.config["size"],
            "existing_group": self.config.get("existing_group"),
        }

    def _compatible(self, saved):
        current = self._identity()
        for key in ("subscription", "group_prefix", "size",
                    "existing_group"):
            if saved.get(key) != current.get(key):
                return False
        return set(saved.get("regions", [])).issubset(current["regions"])

    def _save(self, manifest):
        self.state_dir.mkdir(parents=True, exist_ok=True)
        manifest["updated_at"] = dt.datetime.now(
            dt.timezone.utc
        ).isoformat()
        partial = self.manifest_path.with_suffix(".json.partial")
        partial.write_text(json.dumps(manifest, indent=2) + "\n")
        partial.chmod(0o600)
        partial.replace(self.manifest_path)
        self.manifest_path.chmod(0o600)

    def prepare_manifest(self, *, now=None):
        now = now or dt.datetime.now(dt.timezone.utc)
        if self.manifest_path.exists():
            manifest = json.loads(self.manifest_path.read_text())
            if not self._compatible(manifest.get("config_identity", {})):
                raise ConfigError("incompatible config for existing state")
            return manifest
        deadline = compute_deadline(
            now, int(self.config["lifetime_days"])
        )
        hosts = []
        for number, engine, profile in ROLES:
            hosts.append({
                "host": number,
                "engine": engine,
                "profile": profile,
                "ip": None,
                "known_hosts": str(
                    (self.state_dir / f"host{number}.known_hosts").resolve()
                ),
                "subscription": self.config["subscription"],
                "group": None,
                "location": None,
                "size": self.config["size"],
                "expiration": None,
                "bootstrap_ready": False,
                "compute_id": None,
                "cpu_model": None,
                "attempts": [],
            })
        manifest = {
            "status": "planned",
            "common_deadline": deadline,
            "config_identity": self._identity(),
            "private_config": {
                key: value for key, value in self.config.items()
                if key != "ssh_public_key"
            } | {"ssh_public_key": self.config["ssh_public_key"]},
            "hosts": hosts,
            "regional_capacity": {},
        }
        self._save(manifest)
        return manifest

    def _region_capacities(self):
        result = {}
        for region in self.config["regions"]:
            skus = self.azure(
                "vm", "list-skus", "--location", region,
                "--size", self.config["size"], "--all",
            )
            usage = self.azure("vm", "list-usage", "--location", region)
            try:
                result[region] = region_capacity(
                    skus, usage, self.config["size"]
                )
            except ConfigError as error:
                result[region] = {
                    "available_hosts": 0,
                    "error": str(error),
                }
        return result

    def plan(self, *, now=None):
        manifest = self.prepare_manifest(now=now)
        manifest["regional_capacity"] = self._region_capacities()
        manifest["status"] = "planned"
        self._save(manifest)
        return manifest

    def _vm_url(self, group):
        subscription = self.config["subscription"]
        resource = (
            f"/subscriptions/{subscription}/resourceGroups/{group}/"
            "providers/Microsoft.DevTestLab/labs/searchbench/"
            "virtualmachines/searchbench"
        )
        return (
            f"https://management.azure.com{resource}"
            "?api-version=2018-09-15"
        )

    def _public_ip(self, compute_id):
        compute_group = compute_id.split("/")[4]
        addresses = self.azure(
            "vm", "list-ip-addresses", "-g", compute_group,
            "-n", "searchbench",
        )
        public = addresses[0]["virtualMachine"]["network"][
            "publicIpAddresses"
        ]
        return public[0]["ipAddress"] if public else None

    def _read_vm(self, group):
        vm = self.azure(
            "rest", "--method", "get", "--url", self._vm_url(group)
        )
        properties = vm["properties"]
        compute_id = properties.get("computeId")
        return {
            "group": group,
            "location": vm["location"],
            "size": properties.get("size"),
            "expiration": properties.get("expirationDate"),
            "compute_id": compute_id,
            "ip": self._public_ip(compute_id) if compute_id else None,
            "provisioning_state": properties.get("provisioningState"),
            "image": (
                properties.get("galleryImageReference") or {}
            ).get("version"),
        }

    def _verify_expiration(self, host, deadline):
        if host["provisioning_state"] != "Succeeded":
            raise ConfigError(
                f"{host['group']} is not provisioned successfully"
            )
        if host["size"] != self.config["size"]:
            raise ConfigError(f"{host['group']} has the wrong VM size")
        if _utc(host["expiration"]) != _utc(deadline):
            raise ConfigError(
                f"{host['group']} native expiration does not match"
            )

    def _adopt_existing(self, manifest):
        group = self.config["existing_group"]
        resource_group = self.azure("group", "show", "-n", group)
        if resource_group.get("tags", {}).get("managed-by") != MARKER:
            raise ConfigError("existing_group is not managed by this tool")
        current = self.azure(
            "rest", "--method", "get", "--url", self._vm_url(group)
        )
        if current.get("tags", {}).get("managed-by") != MARKER:
            raise ConfigError(
                "existing_group VM is not managed by this tool"
            )
        properties = current["properties"]
        if properties.get("size") != self.config["size"]:
            raise ConfigError("existing_group has an incompatible VM size")
        allowed = {
            key: properties[key] for key in (
                "allowClaim",
                "disallowPublicIpAddress",
                "galleryImageReference",
                "isAuthenticationWithSshKey",
                "labSubnetName",
                "labVirtualNetworkId",
                "size",
                "storageType",
                "userName",
            ) if key in properties
        }
        allowed["expirationDate"] = manifest["common_deadline"]
        allowed["sshKey"] = Path(
            self.config["ssh_public_key"]
        ).read_text().strip()
        allowed["scheduleParameters"] = [{
            "name": "LabVmsShutdown",
            "properties": {
                "status": "Disabled",
                "taskType": "LabVmsShutdownTask",
                "dailyRecurrence": {"time": "1900"},
                "timeZoneId": "UTC",
                "notificationSettings": {"status": "Disabled"},
            },
        }]
        tags = dict(current.get("tags") or {})
        tags.update({
            "managed-by": MARKER,
            "expires": manifest["common_deadline"],
            "fleet-index": "1",
            "fleet-engine": "pg_textsearch",
            "fleet-profile": "high",
        })
        body = json.dumps({
            "location": current["location"],
            "tags": tags,
            "properties": allowed,
        }, separators=(",", ":"))
        self.azure(
            "rest", "--method", "put", "--url", self._vm_url(group),
            "--headers", "Content-Type=application/json", "--body", body,
        )
        host = self._read_vm(group)
        self._verify_expiration(host, manifest["common_deadline"])
        host["adopted"] = True
        return host

    def _group_name(self, number, region):
        return (
            f"{self.config['group_prefix']}-{number:02d}-"
            f"{region.lower()}"
        )

    def _state_path(self, number, region):
        return self.state_dir / (
            f"host{number}-{region.lower()}-state.json"
        )

    def _deployment_id(self, group):
        return (
            f"/subscriptions/{self.config['subscription']}/"
            f"resourceGroups/{group}/providers/Microsoft.Resources/"
            "deployments/benchmark"
        )

    def _error_code(self, output):
        for code in CAPACITY_ERRORS:
            if code in output:
                return code
        return "DeploymentFailed"

    def _state_result(self, saved, state):
        vm = saved["vm"]
        return {
            "status": "provisioned",
            "group": saved["group"],
            "location": saved["location"],
            "size": vm["size"],
            "expiration": vm["expirationDate"],
            "compute_id": vm["computeId"],
            "ip": vm["networkInterface"].get("publicIpAddress"),
            "state_path": str(state.resolve()),
            "provisioning_state": vm["provisioningState"],
        }

    def _create_host(self, host, region, manifest):
        number = host["host"]
        group = self._group_name(number, region)
        state = self._state_path(number, region)
        command = [
            sys.executable,
            str(Path(__file__).with_name("azure.py")),
            "create",
            "--subscription", self.config["subscription"],
            "--group", group,
            "--location", region,
            "--size", self.config["size"],
            "--ssh-key", self.config["ssh_public_key"],
            "--ssh-cidr", self.config["ssh_cidr"],
            "--expires", manifest["common_deadline"],
            "--state", str(state),
        ]
        if state.exists():
            saved = json.loads(state.read_text())
            expected = (
                saved.get("subscription") == self.config["subscription"]
                and saved.get("group") == group
                and saved.get("size") == self.config["size"]
                and saved.get("location") == region
                and saved.get("expires") == manifest["common_deadline"]
            )
            if not expected:
                raise ConfigError(f"incompatible state file: {state}")
            if saved.get("status") == "provisioned" and saved.get("vm"):
                provisioned = self._read_vm(group)
                self._verify_expiration(
                    provisioned, manifest["common_deadline"]
                )
                provisioned.pop("provisioning_state")
                provisioned["status"] = "provisioned"
                provisioned["state_path"] = str(state.resolve())
                return provisioned
            command.append("--resume")
        environment = dict(os.environ, TMPDIR=str(self.state_dir))
        result = subprocess.run(
            command, capture_output=True, text=True, env=environment
        )
        log = self.state_dir / f"host{number}-{region}.log"
        log.write_text(result.stdout + result.stderr)
        log.chmod(0o600)
        deployment_id = self._deployment_id(group)
        if result.returncode:
            return {
                "status": "failed",
                "error_code": self._error_code(
                    result.stdout + result.stderr
                ),
                "deployment_id": deployment_id,
                "group": group,
                "location": region,
                "state_path": str(state.resolve()),
            }
        saved = json.loads(state.read_text())
        provisioned = self._state_result(saved, state)
        self._verify_expiration(
            provisioned, manifest["common_deadline"]
        )
        provisioned.pop("provisioning_state")
        return provisioned

    def _ssh_options(self, host):
        return [
            "-i", self.config["ssh_private_key"],
            "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=yes",
            "-o", f"UserKnownHostsFile={host['known_hosts']}",
        ]

    def _capture_host_key(self, host):
        known = Path(host["known_hosts"])
        error = None
        for attempt in range(60):
            try:
                result = subprocess.run(
                    ["ssh-keyscan", "-T", "10", "-H", host["ip"]],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                if result.stdout.strip():
                    break
            except subprocess.CalledProcessError as failure:
                error = failure
            if attempt < 59:
                time.sleep(10)
        else:
            if error:
                raise error
            raise ConfigError(f"SSH host key unavailable: {host['ip']}")
        known.write_text(result.stdout)
        known.chmod(0o600)

    def _ssh(self, host, *remote, capture=False):
        return subprocess.run(
            [
                "ssh", *self._ssh_options(host),
                f"bench@{host['ip']}", remote_command(remote),
            ],
            check=True,
            capture_output=capture,
            text=True,
        )

    def _verify_host(self, host):
        probe = (
            "import json,platform,subprocess;"
            "d=json.loads(subprocess.check_output("
            "['lsblk','-J','-b','-o',"
            "'PATH,MODEL,TYPE,MOUNTPOINTS,SIZE'],"
            "text=True));"
            "print(json.dumps({'cpu_model':next("
            "x.split(':',1)[1].strip() for x in "
            "open('/proc/cpuinfo') if x.startswith('model name')),"
            "'kernel':platform.release(),'topology':d,"
            "'bootstrap_ready':subprocess.run(['test','-f',"
            "'/opt/searchbench/bootstrap.ready']).returncode==0,"
            "'docker_root':subprocess.check_output("
            "['docker','info','--format','{{.DockerRootDir}}'],"
            "text=True).strip()}))"
        )
        result = self._ssh(
            host, "python3", "-c", probe, capture=True
        )
        return hardware_from_probe(json.loads(result.stdout))

    def _bootstrap_host(self, host):
        self._capture_host_key(host)
        self._ssh(host, "mkdir", "-p", "/home/bench/scale-bootstrap")
        scale = Path(__file__).parent
        subprocess.run(
            [
                "scp", *self._ssh_options(host),
                str(scale / "bootstrap.sh"),
                str(scale / "bootstrap_disks.py"),
                f"bench@{host['ip']}:/home/bench/scale-bootstrap/",
            ],
            check=True,
        )
        self._ssh(
            host, "sudo", "-A", "bash",
            "/home/bench/scale-bootstrap/bootstrap.sh",
        )
        return self._verify_host(host)

    def _attempted_regions(self, host):
        return {
            attempt["location"] for attempt in host.get("attempts", [])
            if attempt.get("status") == "failed"
            and attempt.get("error_code") in CAPACITY_ERRORS
        }

    def provision(self, *, now=None, only_hosts=None):
        manifest = self.prepare_manifest(now=now)
        if self.config.get("existing_group"):
            first = manifest["hosts"][0]
            if not first.get("compute_id"):
                first.update(self._adopt_existing(manifest))
                self._save(manifest)
            if not first["bootstrap_ready"]:
                self._capture_host_key(first)
                first.update(self._verify_host(first))
                first["bootstrap_ready"] = True
                self._save(manifest)
        capacities = self._region_capacities()
        manifest["regional_capacity"] = capacities
        for host in manifest["hosts"]:
            capacity = capacities.get(host.get("location"))
            if capacity and capacity.get("family"):
                host["sku_family"] = capacity["family"]
        self._save(manifest)
        targets = set(only_hosts or range(1, 7))
        for host in manifest["hosts"]:
            if host["host"] not in targets or host["bootstrap_ready"]:
                continue
            if host["host"] == 1 and self.config.get("existing_group"):
                continue
            if not host.get("compute_id"):
                attempted = self._attempted_regions(host)
                provisioned = None
                for region in self.config["regions"]:
                    if region in attempted:
                        continue
                    capacity = capacities.get(region, {})
                    if capacity.get("available_hosts", 0) < 1:
                        continue
                    group = self._group_name(host["host"], region)
                    attempt = next((
                        item for item in reversed(host["attempts"])
                        if item.get("location") == region
                        and item.get("status") == "deploying"
                    ), None)
                    if attempt is None:
                        attempt = {
                            "location": region,
                            "status": "deploying",
                            "error_code": None,
                            "deployment_id": self._deployment_id(group),
                            "group": group,
                            "state_path": str(
                                self._state_path(
                                    host["host"], region
                                ).resolve()
                            ),
                        }
                        host["attempts"].append(attempt)
                        self._save(manifest)
                    result = self._create_host(host, region, manifest)
                    attempt.update({
                        "location": region,
                        "status": result["status"],
                        "error_code": result.get("error_code"),
                        "deployment_id": (
                            result.get("deployment_id")
                            or attempt["deployment_id"]
                        ),
                        "group": result.get("group") or attempt["group"],
                        "state_path": (
                            result.get("state_path")
                            or attempt["state_path"]
                        ),
                    })
                    self._save(manifest)
                    if result["status"] == "provisioned":
                        provisioned = result
                        capacities[region]["available_hosts"] -= 1
                        break
                    if result["error_code"] not in CAPACITY_ERRORS:
                        raise ConfigError(
                            f"deployment failed: {result['error_code']}"
                        )
                if not provisioned:
                    manifest["status"] = "blocked"
                    self._save(manifest)
                    raise ConfigError(
                        f"no region could provision host {host['host']}"
                    )
                host.update(provisioned)
                host["sku_family"] = capacities[host["location"]].get(
                    "family"
                )
                host["known_hosts"] = str(
                    (self.state_dir /
                     f"host{host['host']}.known_hosts").resolve()
                )
                host["bootstrap_ready"] = False
                self._save(manifest)
            calibration = self._bootstrap_host(host)
            host.update(calibration)
            host["bootstrap_ready"] = True
            self._save(manifest)
        manifest["status"] = (
            "ready"
            if all(host["bootstrap_ready"] for host in manifest["hosts"])
            else "partial"
        )
        self._save(manifest)
        return manifest

    def status(self):
        if not self.manifest_path.exists():
            raise ConfigError("fleet manifest does not exist")
        manifest = json.loads(self.manifest_path.read_text())
        for host in manifest["hosts"]:
            if not host.get("group"):
                host["bootstrap_ready"] = False
                host["validation_error"] = "resource group is missing"
                continue
            current = self._read_vm(host["group"])
            self._verify_expiration(current, manifest["common_deadline"])
            host.update(current)
            host.pop("validation_error", None)
            key = Path(self.config["ssh_private_key"])
            known = Path(host["known_hosts"])
            if key.is_file() and known.is_file() and host.get("ip"):
                try:
                    host.update(self._verify_host(host))
                    host["bootstrap_ready"] = True
                    host["bootstrap_verification"] = "verified"
                except (
                    ConfigError,
                    json.JSONDecodeError,
                    OSError,
                    subprocess.CalledProcessError,
                    ValueError,
                ) as error:
                    host["bootstrap_ready"] = False
                    host["bootstrap_verification"] = "failed"
                    host["validation_error"] = str(error)
            else:
                host["bootstrap_ready"] = False
                host["bootstrap_verification"] = (
                    "not_checked_missing_ssh_config"
                )
        manifest["status"] = (
            "ready"
            if all(host["bootstrap_ready"] for host in manifest["hosts"])
            else "partial"
        )
        self._save(manifest)
        return manifest


def _load_config(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigError(f"invalid config: {path}") from error


def config_from_manifest(manifest):
    hosts = manifest.get("hosts") or []
    if not hosts:
        raise ConfigError("fleet manifest has no hosts")
    first = hosts[0]
    regions = list(dict.fromkeys(
        host.get("location") for host in hosts
        if host.get("location")
    ))
    if not regions:
        regions = ["status-only"]
    return {
        "subscription": first["subscription"],
        "group_prefix": "searchbench-status",
        "regions": regions,
        "size": first["size"],
        "lifetime_days": 1,
        "ssh_private_key": "status-only",
        "ssh_public_key": "status-only",
        "ssh_cidr": "127.0.0.1/32",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "provision", "status"))
    parser.add_argument("--config", type=Path)
    parser.add_argument("--state", type=Path, required=True)
    args = parser.parse_args()
    if args.action != "status" and not args.config:
        parser.error("--config is required for plan and provision")
    if args.config:
        config = _load_config(args.config)
    else:
        manifest = json.loads(
            (args.state / "fleet-manifest.json").read_text()
        )
        config = manifest.get("private_config") or config_from_manifest(
            manifest
        )
    fleet = FleetAzure(config, args.state)
    result = getattr(fleet, args.action)()
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
