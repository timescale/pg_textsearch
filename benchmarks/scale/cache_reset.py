#!/usr/bin/env python3
"""Evict only an offline, owned benchmark volume's file cache."""

import argparse
import ctypes
import json
import mmap
import os
from pathlib import Path
import stat
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.scale.run import ConfigError, validate_storage


def resident_pages(fd, size):
    if not size:
        return 0
    with mmap.mmap(fd, size, access=mmap.ACCESS_COPY) as mapping:
        address = ctypes.addressof(ctypes.c_char.from_buffer(mapping))
        count = (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE
        vector = (ctypes.c_ubyte * count)()
        libc = ctypes.CDLL(None, use_errno=True)
        libc.mincore.argtypes = (
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
        )
        libc.mincore.restype = ctypes.c_int
        if libc.mincore(address, size, vector):
            number = ctypes.get_errno()
            raise OSError(number, os.strerror(number))
        return sum(value & 1 for value in vector)


def evict_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise ConfigError(f"refusing nonregular cache target: {path}")
        os.fsync(fd)
        before = resident_pages(fd, info.st_size)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        after = resident_pages(fd, info.st_size)
        if after:
            raise ConfigError(
                f"file cache eviction incomplete for {path}: {after} pages"
            )
        return info.st_size, before
    finally:
        os.close(fd)


def docker_output(*args):
    return subprocess.check_output(["docker", *args], text=True).strip()


def reset_volume(phase, run_dir, volume):
    root = Path("/srv/searchbench/pg_textsearch-scale")
    validate_storage(root, smoke=True)
    run_dir = Path(run_dir).resolve()
    if run_dir.parent != root / "runs":
        raise ConfigError("cache reset requires a dedicated benchmark run")
    docker_root = Path(docker_output(
        "info", "--format", "{{.DockerRootDir}}"
    ))
    if docker_root != Path("/srv/searchbench/docker"):
        raise ConfigError("refusing a non-benchmark Docker root")
    if docker_output("ps", "-q"):
        raise ConfigError("cache reset requires an idle benchmark host")
    info, = json.loads(docker_output("volume", "inspect", volume))
    labels = info.get("Labels") or {}
    if (
        labels.get("managed-by") != "pg_textsearch-tin"
        or labels.get("run-id") != run_dir.name
    ):
        raise ConfigError("refusing a foreign cache-reset volume")
    if docker_output("ps", "-aq", "--filter", f"volume={volume}"):
        raise ConfigError("volume is still attached to a container")
    mount = Path(info["Mountpoint"])
    expected = docker_root / "volumes" / volume / "_data"
    if mount != expected or mount.resolve() != expected:
        raise ConfigError("refusing an unexpected volume mountpoint")
    files = total_bytes = evicted_pages = 0
    for directory, directories, names in os.walk(mount, followlinks=False):
        for name in directories + names:
            path = Path(directory) / name
            if path.is_symlink():
                raise ConfigError(f"refusing symlink in cache target: {path}")
        for name in names:
            size, before = evict_file(Path(directory) / name)
            files += 1
            total_bytes += size
            evicted_pages += before
    if not files:
        raise ConfigError("cache-reset volume is empty")
    return {
        "performed": True,
        "dedicated_vm": True,
        "verified": True,
        "method": "posix_fadvise_dontneed",
        "verification": "mincore-zero-resident-pages-per-file",
        "phase": phase,
        "volume": volume,
        "files": files,
        "bytes": total_bytes,
        "previously_resident_pages": evicted_pages,
        "resident_pages_after": 0,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase")
    parser.add_argument("run_directory")
    parser.add_argument("volume")
    args = parser.parse_args(argv)
    try:
        evidence = reset_volume(
            args.phase, args.run_directory, args.volume
        )
    except (ConfigError, OSError, subprocess.CalledProcessError) as error:
        print(f"cache reset failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
