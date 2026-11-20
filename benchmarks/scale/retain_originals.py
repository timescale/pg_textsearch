#!/usr/bin/env python3
"""Bring explicitly inventoried benchmark originals online after timing."""

import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import time

from benchmarks.scale.fleet_control import (
    _atomic_json,
    validate_retained_container,
)


def validate_plan(plan):
    units = plan.get("idle_units")
    if not isinstance(units, list) or not units or any(
        not isinstance(unit, str)
        or not re.fullmatch(r"[A-Za-z0-9_.@-]+", unit)
        for unit in units
    ):
        raise ValueError("explicit benchmark worker units are required")
    databases = plan.get("databases")
    if not isinstance(databases, list) or not databases:
        raise ValueError("explicit original database inventory is required")
    volumes, ports = set(), set()
    for row in databases:
        volume, port = row.get("volume"), row.get("port")
        labels = row.get("labels") or {}
        engine = row.get("engine")
        if (
            not isinstance(volume, str)
            or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", volume)
            or type(port) is not int
            or not 1024 <= port <= 65535
            or volume in volumes
            or port in ports
        ):
            raise ValueError("invalid or duplicate volume/port assignment")
        if (
            engine not in ("pg_textsearch", "paradedb")
            or labels.get("managed-by") not in (
                "pg_textsearch-tin", "pg_textsearch-scale",
            )
            or not labels.get("run-id")
            or str(labels.get("kind", "")).startswith("write")
            or (
                labels.get("engine") != engine
                and not (
                    labels.get("kind") == "proof"
                    and labels.get("engine") is None
                    and engine == "pg_textsearch"
                )
            )
        ):
            raise ValueError("foreign, disposable, or mismatched volume")
        image = row.get("image")
        if (
            not isinstance(image, str)
            or not re.fullmatch(
                r"(?:sha256:|[A-Za-z0-9][A-Za-z0-9._:/-]*@sha256:)"
                r"[0-9a-f]{64}",
                image,
            )
            or type(row.get("expect_search_index")) is not bool
        ):
            raise ValueError("pinned image and index expectation required")
        volumes.add(volume)
        ports.add(port)


def require_idle(units):
    for unit in units:
        result = subprocess.run(
            ["systemctl", "is-active", "--quiet", unit],
            check=False,
        )
        if result.returncode == 0:
            raise ValueError("benchmark worker is active: " + unit)
        if result.returncode not in (3, 4):
            raise ValueError("could not verify worker is idle: " + unit)


def validate_image_layout(info):
    env = dict(
        entry.split("=", 1)
        for entry in info.get("Config", {}).get("Env", [])
        if "=" in entry
    )
    if (
        env.get("PG_MAJOR") != "18"
        or env.get("PGDATA") != "/var/lib/postgresql/18/docker"
    ):
        raise ValueError(
            "image does not use the expected PostgreSQL 18 layout"
        )


def _settings(engine, buffers):
    args = [
        "-c", "shared_buffers=" + buffers,
        "-c", "maintenance_work_mem=256MB",
        "-c", "random_page_cost=1.1",
    ]
    if engine == "pg_textsearch":
        args.extend(["-c", "shared_preload_libraries=pg_textsearch"])
    return args


def validate_runtime(info, image, row):
    config = info.get("Config") or {}
    base = image.get("Config") or {}
    env = [
        value for value in (base.get("Env") or [])
        if not value.startswith("POSTGRES_PASSWORD=")
    ] + ["POSTGRES_PASSWORD=postgres"]
    entrypoint = base.get("Entrypoint") or []
    cmd = config.get("Cmd")
    mounts = info.get("Mounts") or []
    if (
        not entrypoint
        or sorted(config.get("Env") or []) != sorted(env)
        or config.get("Entrypoint") != entrypoint
        or config.get("User", "") != base.get("User", "")
        or cmd not in [
            _settings(row["engine"], buffers)
            for buffers in ("512MB", "1GB")
        ]
        or info.get("Path") != entrypoint[0]
        or info.get("Args") != entrypoint[1:] + (cmd or [])
        or len(mounts) != 1
        or mounts[0].get("Type") != "volume"
        or mounts[0].get("Name") != row["volume"]
        or mounts[0].get("Destination") != "/var/lib/postgresql"
        or mounts[0].get("RW") is not True
    ):
        raise ValueError("retained container runtime configuration mismatch")


def validate_index_proof(proof, expected):
    if (
        proof["valid"] is not True
        or proof["count"] != (1 if expected else 0)
        or expected and (
            proof.get("expected_name") is not True
            or proof.get("body_indexed") is not True
        )
    ):
        raise ValueError("original search-index inventory mismatch")


def _json_command(args):
    return json.loads(subprocess.check_output(args, text=True))


def _psql(name, sql):
    return subprocess.check_output([
        "docker", "exec", name, "psql", "-U", "postgres", "-XAt",
        "-v", "ON_ERROR_STOP=1", "-c", sql,
    ], text=True).strip()


def restore(plan, report_path):
    validate_plan(plan)
    require_idle(plan["idle_units"])
    ids = subprocess.check_output(
        ["docker", "ps", "-aq"], text=True
    ).split()
    containers = _json_command(["docker", "inspect", *ids]) if ids else []
    for info in containers:
        if (
            (info.get("State") or {}).get("Running")
            and (info.get("Config", {}).get("Labels") or {}).get(
                "managed-by"
            ) != "pg_textsearch-tin-retained"
        ):
            raise ValueError("non-retained container is still running")
    by_name = {item["Name"].lstrip("/"): item for item in containers}
    prepared = []
    for row in plan["databases"]:
        volume, = _json_command([
            "docker", "volume", "inspect", row["volume"],
        ])
        if (
            volume["Name"] != row["volume"]
            or (volume.get("Labels") or {}) != row["labels"]
        ):
            raise ValueError("original volume ownership changed")
        image, = _json_command(["docker", "image", "inspect", row["image"]])
        validate_image_layout(image)
        name = (
            "pgts-retained-" + row["engine"].replace("_", "-")
            + "-" + str(row["port"])
        )
        existing = by_name.get(name)
        if existing:
            validate_retained_container(
                existing, image=row["image"],
                volume=row["volume"], port=row["port"],
            )
            if existing["Image"] != image["Id"]:
                raise ValueError("retained container binary image changed")
            validate_runtime(existing, image, row)
        for info in containers:
            if info["Name"] == "/" + name:
                continue
            if any(
                mount.get("Name") == row["volume"]
                for mount in info.get("Mounts", [])
            ):
                raise ValueError("original volume has another container")
            for bindings in (
                (info.get("HostConfig") or {}).get("PortBindings") or {}
            ).values():
                if any(
                    binding.get("HostPort") == str(row["port"])
                    for binding in (bindings or [])
                ):
                    raise ValueError(
                        "retention port belongs to another container"
                    )
        if not existing:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", row["port"]))
        version = subprocess.check_output([
            "docker", "run", "--rm", "--entrypoint", "/bin/cat",
            "--mount", "type=volume,source=" + row["volume"]
            + ",target=/var/lib/postgresql,readonly",
            row["image"], "/var/lib/postgresql/18/docker/PG_VERSION",
        ], text=True).strip()
        if version != "18":
            raise ValueError("expected an initialized PostgreSQL 18 volume")
        prepared.append((row, name, existing))
    require_idle(plan["idle_units"])
    report = {"status": "in_progress", "databases": []}
    _atomic_json(report_path, report)
    for row, name, existing in prepared:
        if existing:
            if not existing.get("State", {}).get("Running"):
                subprocess.run(["docker", "start", name], check=True)
        else:
            args = [
                "docker", "run", "-d", "--name", name,
                "--label", "managed-by=pg_textsearch-tin-retained",
                "--label", "source-volume=" + row["volume"],
                "--mount", "type=volume,source=" + row["volume"]
                + ",target=/var/lib/postgresql",
                "-e", "POSTGRES_PASSWORD=postgres",
                "-p", "127.0.0.1:" + str(row["port"]) + ":5432",
                "--shm-size=2g", row["image"],
            ]
            args.extend(_settings(row["engine"], "512MB"))
            subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        deadline = time.monotonic() + 120
        while subprocess.run(
            ["docker", "exec", name, "pg_isready", "-U", "postgres"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode:
            if time.monotonic() >= deadline:
                raise ValueError("retained database did not become ready")
            time.sleep(1)
        proof = json.loads(_psql(name, """
SELECT json_build_object(
    'count', count(*),
    'valid', coalesce(bool_and(
        i.indisvalid AND i.indisready AND i.indislive), true),
    'expected_name', bool_and(
        c.relname = 'documents_bm25' AND n.nspname = 'public'),
    'body_indexed', bool_and(EXISTS (
        SELECT 1 FROM pg_attribute att
        WHERE att.attrelid = i.indrelid AND att.attname = 'body'
          AND att.attnum = ANY(i.indkey))))
FROM pg_index i
JOIN pg_class c ON c.oid = i.indexrelid
JOIN pg_namespace n ON n.oid = c.relnamespace
JOIN pg_am a ON a.oid = c.relam
WHERE i.indrelid = 'public.documents'::regclass AND a.amname = 'bm25'
"""))
        validate_index_proof(proof, row["expect_search_index"])
        if not row["expect_search_index"]:
            query = "SELECT id FROM public.documents LIMIT 1"
        elif row["engine"] == "pg_textsearch":
            query = (
                "SELECT id FROM public.documents ORDER BY body <@> "
                "to_bm25query('xylophone','public.documents_bm25') LIMIT 1"
            )
        else:
            query = (
                "SELECT id FROM public.documents "
                "WHERE body @@@ 'xylophone' LIMIT 1"
            )
        output = _psql(name, "SET statement_timeout='30s'; " + query)
        report["databases"].append({
            **row, "container": name, "index_proof": proof,
            "query_output": output, "queryable": True,
        })
        _atomic_json(report_path, report)
        print(name + " queryable", flush=True)
    report["status"] = "complete"
    _atomic_json(report_path, report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    restore(json.loads(args.plan.read_text()), args.report)


if __name__ == "__main__":
    main()
