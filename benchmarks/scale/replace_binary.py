"""Replace a retained native library without rebuilding its data."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def validate_target(info, image, volume, port=None):
    if (
        info["Image"] != image or not info["State"]["Running"]
        or len(info["Mounts"]) != 1
        or info["Mounts"][0].get("Type") != "volume"
        or info["Mounts"][0].get("Name") != volume
    ):
        raise RuntimeError("foreign, stopped, or unexpected container/volume")
    if port is not None:
        actual = info["NetworkSettings"]["Ports"].get("5432/tcp", []) or []
        if not any(binding["HostPort"] == str(port) for binding in actual):
            raise RuntimeError("database port is outside target container")
        configured = (
            info["HostConfig"]["PortBindings"].get("5432/tcp", []) or []
        )
        if not any(binding["HostPort"] == str(port) for binding in configured):
            raise RuntimeError("database requires a fixed host port")


def index_identity(connection, index):
    row = connection.execute("""
        SELECT oid, pg_relation_filenode(oid), pg_relation_size(oid)
        FROM pg_class WHERE oid = %s::regclass
    """, (index,)).fetchone()
    if row is None:
        raise RuntimeError("expected original index is missing")
    return list(row)


def await_ready(container):
    for _ in range(60):
        ready = subprocess.run(
            [
                "docker", "exec", container, "pg_isready",
                "-h", "127.0.0.1", "-U", "postgres",
            ],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if ready.returncode == 0:
            return
        time.sleep(1)
    raise RuntimeError("container did not become ready within 60 seconds")


def write_journal(path, state):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    temporary.replace(path)


def main(argv=None):
    import psycopg

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--expected-image", required=True)
    parser.add_argument("--expected-volume", required=True)
    parser.add_argument("--expected-original-sha256", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--backup-directory", type=Path, required=True)
    parser.add_argument("--worker-unit", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--index", default="public.documents_bm25")
    parser.add_argument(
        "--library", default="/usr/lib/postgresql/18/lib/pg_textsearch.so"
    )
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.container):
        parser.error("container must be a Docker name or ID")
    if (
        hashlib.sha256(args.binary.read_bytes()).hexdigest()
        != args.binary_sha256
    ):
        raise RuntimeError("replacement binary checksum mismatch")
    info = json.loads(command("docker", "inspect", args.container))[0]
    validate_target(
        info, args.expected_image, args.expected_volume, port=args.port
    )
    if command(
        "systemctl", "show", args.worker_unit, "-p", "ActiveState"
    ) != "ActiveState=inactive":
        raise RuntimeError("benchmark worker is not inactive")
    current_sha = command(
        "docker", "exec", args.container, "sha256sum", args.library
    ).split()[0]
    environment = dict(
        entry.split("=", 1) for entry in info["Config"]["Env"]
    )

    def connect():
        return psycopg.connect(
            host="127.0.0.1", port=args.port,
            user=os.environ.get("PGUSER", "postgres"),
            dbname=os.environ.get("PGDATABASE", "postgres"),
            password=os.environ.get(
                "PGPASSWORD", environment.get("POSTGRES_PASSWORD")
            ),
            autocommit=True,
        )

    args.backup_directory.mkdir(parents=True, exist_ok=True)
    journal = args.backup_directory / f"{args.container}.json"
    if journal.exists():
        prior = json.loads(journal.read_text())
        if (
            prior["status"] == "ready"
            and current_sha == args.binary_sha256
            and prior["binary_sha256"] == args.binary_sha256
            and prior["source_sha"] == args.source_sha
            and prior["volume"] == args.expected_volume
            and prior["image"] == args.expected_image
            and prior["port"] == args.port
            and prior["index"] == args.index
            and prior.get("library") == args.library
            and prior["original_sha256"] == args.expected_original_sha256
        ):
            with connect() as connection:
                connection.execute("SET default_transaction_read_only=on")
                current_index = index_identity(connection, args.index)
            if current_index[:2] != prior["after"][:2]:
                raise RuntimeError("journaled index identity changed")
            print(json.dumps(prior))
            return
        raise RuntimeError("prior deployment needs explicit reconciliation")
    if current_sha != args.expected_original_sha256:
        raise RuntimeError("original library checksum mismatch")
    backup = args.backup_directory / f"{args.container}.original.so"
    if backup.exists():
        raise RuntimeError("unjournaled backup needs explicit reconciliation")
    with connect() as connection:
        connection.execute("SET default_transaction_read_only=on")
        before = index_identity(connection, args.index)
    # Exclusive creation prevents simultaneous deployments in this directory.
    with journal.open("x") as output:
        state = {
            "status": "preparing", "container": args.container,
            "volume": args.expected_volume, "image": args.expected_image,
            "port": args.port, "index": args.index,
            "library": args.library, "before": before,
            "source_sha": args.source_sha, "binary_sha256": args.binary_sha256,
            "original_sha256": current_sha, "original_backup": str(backup),
            "scope": "library replacement; no reload or index rebuild",
        }
        output.write(json.dumps(state, indent=2) + "\n")
    command("docker", "cp", f"{args.container}:{args.library}", str(backup))
    if hashlib.sha256(backup.read_bytes()).hexdigest() != current_sha:
        raise RuntimeError("original backup checksum mismatch")
    state["status"] = "backed_up"
    write_journal(journal, state)
    command(
        "docker", "cp", str(args.binary),
        f"{args.container}:{args.library}.replay",
    )
    command(
        "docker", "exec", args.container, "chmod", "755",
        args.library + ".replay",
    )
    # Atomic rename keeps the old inode mapped until the old backends exit.
    command(
        "docker", "exec", args.container,
        "mv", args.library + ".replay", args.library,
    )
    try:
        command("docker", "restart", "--time", "60", args.container)
        await_ready(args.container)
        with connect() as connection:
            connection.execute("SET default_transaction_read_only=on")
            after = index_identity(connection, args.index)
        if before[:2] != after[:2]:
            raise RuntimeError("original index identity changed")
        installed = command(
            "docker", "exec", args.container, "sha256sum", args.library
        ).split()[0]
        if installed != args.binary_sha256:
            raise RuntimeError("installed library checksum mismatch")
    except (
        subprocess.CalledProcessError, psycopg.Error, RuntimeError
    ) as error:
        state.update(status="restoring", error=str(error))
        write_journal(journal, state)
        command("docker", "stop", "--time", "60", args.container)
        command(
            "docker", "cp", str(backup), f"{args.container}:{args.library}"
        )
        command("docker", "start", args.container)
        await_ready(args.container)
        restored = command(
            "docker", "exec", args.container, "sha256sum", args.library
        ).split()[0]
        if restored != current_sha:
            raise RuntimeError("restoration checksum mismatch")
        state["status"] = "rolled_back"
        write_journal(journal, state)
        raise RuntimeError(
            "replacement failed; original library restored"
        ) from error
    state.update(status="ready", after=after)
    write_journal(journal, state)
    print(json.dumps(state))


if __name__ == "__main__":
    main()
