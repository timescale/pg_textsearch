#!/usr/bin/env python3
"""Initialize only Azure local NVMe disks on a dedicated benchmark machine."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import urllib.request


MOUNT = Path("/srv/searchbench")
MARKER = "pg_textsearch-scale"


def select_disks(topology):
    disks = []
    for disk in topology["blockdevices"]:
        if (disk.get("model") or "").strip() != "Microsoft NVMe Direct Disk":
            continue
        if (disk["type"] != "disk" or disk.get("children")
                or any(disk.get("mountpoints") or [])
                or not disk["path"].startswith("/dev/nvme")
                or disk["size"] < 100 * 1024**3):
            raise ValueError(f"Refusing nonempty/unsafe disk {disk['path']}")
        disks.append(disk["path"])
    if not disks:
        raise ValueError("No unused Azure local NVMe disks; no OS-disk fallback")
    return sorted(disks)


def require_benchmark_vm(expected_compute_id):
    request = urllib.request.Request(
        "http://169.254.169.254/metadata/instance/compute"
        "?api-version=2021-02-01", headers={"Metadata": "true"},
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=5) as response:
        metadata = json.load(response)
    if metadata.get("resourceId", "").lower() != expected_compute_id.lower():
        raise ValueError("IMDS compute resource does not match the controller")
    tags = {
        item.get("name"): item.get("value")
        for item in metadata.get("tagsList", [])
    }
    if not tags and metadata.get("tags"):
        tags = dict(
            item.split(":", 1)
            for item in metadata["tags"].split(";")
            if ":" in item
        )
    if tags.get("managed-by") != MARKER:
        raise ValueError("IMDS ownership marker is missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected-compute-id", required=True)
    parser.add_argument("--initialize-ephemeral-storage", action="store_true")
    args = parser.parse_args()
    require_benchmark_vm(args.expected_compute_id)
    mounted = subprocess.run(
        ["findmnt", "-rn", "-o", "LABEL", "--mountpoint", str(MOUNT)],
        capture_output=True, text=True,
    )
    if mounted.returncode == 0:
        if mounted.stdout.strip() != "searchbench":
            raise ValueError("Benchmark mount exists with an unexpected label")
        print(f"Already mounted: {MOUNT}")
        return
    topology = json.loads(subprocess.check_output(
        ["lsblk", "-J", "-b", "-o", "PATH,MODEL,TYPE,MOUNTPOINTS,SIZE"],
        text=True,
    ))
    disks = select_disks(topology)
    print(json.dumps({"ephemeral_disks": disks, "mountpoint": str(MOUNT)}))
    if not args.initialize_ephemeral_storage:
        return
    if os.geteuid() != 0:
        raise ValueError("Storage initialization requires root")
    for disk in disks:
        signature = subprocess.run(
            ["blkid", "-p", disk], capture_output=True, text=True,
        )
        if signature.returncode != 2:
            raise ValueError(f"Refusing disk with signature or read error: {disk}")
    device = disks[0]
    if len(disks) > 1:
        device = "/dev/md/searchbench"
        subprocess.run(
            ["mdadm", "--create", device, "--run", "--level=0",
             f"--raid-devices={len(disks)}", "--metadata=1.2", *disks],
            check=True,
        )
    subprocess.run(["mkfs.ext4", "-L", "searchbench", "-m", "0", device],
                   check=True)
    MOUNT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["mount", "-o", "noatime", device, str(MOUNT)], check=True)
    uuid = subprocess.check_output(
        ["blkid", "-s", "UUID", "-o", "value", device], text=True,
    ).strip()
    with Path("/etc/fstab").open("a") as fstab:
        fstab.write(f"UUID={uuid} {MOUNT} ext4 noatime,nofail 0 2\n")
    subprocess.run(["chown", "bench:bench", str(MOUNT)], check=True)


if __name__ == "__main__":
    main()
