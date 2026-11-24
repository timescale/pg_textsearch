"""Profile one owned PostgreSQL backend without changing its retained index."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time
import uuid

from benchmarks.scale.compare_queries import database_metadata
from benchmarks.scale.native_rewrites import KINDS
from benchmarks.scale.replay_queries import (
    prepared_statement, replay_case, select_queries,
)


def host_backend_pid(container, namespace_pid, proc=Path("/proc")):
    matches = []
    output = subprocess.check_output(
        ["docker", "top", container, "-eo", "pid"], text=True
    )
    for value in output.splitlines()[1:]:
        pid = int(value.strip())
        for line in (proc / str(pid) / "status").read_text().splitlines():
            if (
                line.startswith("NSpid:")
                and int(line.split()[-1]) == namespace_pid
            ):
                matches.append(pid)
    if len(matches) != 1:
        raise RuntimeError(f"ambiguous backend PID mapping: {matches}")
    return matches[0]


def mapped_paths(maps):
    paths = set()
    for line in maps.splitlines():
        fields = line.split(maxsplit=5)
        if (
            len(fields) != 6 or "x" not in fields[1]
            or not fields[5].startswith("/")
        ):
            continue
        path = fields[5]
        if path.endswith(" (deleted)"):
            raise RuntimeError(
                "backend maps a deleted binary; restart its container"
            )
        paths.add(path)
    return paths


def validate_receipt(receipt, container, index=None):
    if (
        receipt.get("status") != "ready"
        or receipt.get("container") != container
        or not re.fullmatch(r"[0-9a-f]{64}", receipt.get("binary_sha256", ""))
        or not re.fullmatch(r"[0-9a-f]{40}", receipt.get("source_sha", ""))
        or (index is not None and receipt.get("after") != index[:3])
    ):
        raise ValueError(
            "a ready deployment receipt for this container is required"
        )
    return receipt


def validate_container(container):
    labels = json.loads(subprocess.check_output(
        [
            "docker", "inspect", "--format",
            "{{json .Config.Labels}}", container,
        ],
        text=True,
    )) or {}
    if labels.get("managed-by") != "pg_textsearch-tin-retained":
        raise RuntimeError("container ownership marker does not match")


def stop_recorder(recorder, timeout=10):
    if recorder.poll() is None:
        recorder.send_signal(signal.SIGINT)
    try:
        result = recorder.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        recorder.terminate()
        try:
            recorder.wait(timeout=5)
        except subprocess.TimeoutExpired:
            recorder.kill()
            recorder.wait(timeout=5)
        raise RuntimeError(
            "perf ignored shutdown; owned recorder was terminated"
        )
    if result not in (0, 130, -signal.SIGINT):
        raise RuntimeError(f"perf record failed with exit status {result}")
    return result


@contextmanager
def profile_output(path):
    path.mkdir(mode=0o700)
    try:
        yield path
    finally:
        if os.geteuid() == 0 and "SUDO_UID" in os.environ:
            uid = int(os.environ["SUDO_UID"])
            gid = int(os.environ["SUDO_GID"])
            for entry in path.rglob("*"):
                os.chown(entry, uid, gid, follow_symlinks=False)
            os.chown(path, uid, gid, follow_symlinks=False)


def copy_symbols(pid, directory, expected_sha256):
    proc = Path("/proc") / str(pid)
    paths = mapped_paths((proc / "maps").read_text())
    libraries = [
        path for path in paths if Path(path).name == "pg_textsearch.so"
    ]
    if len(libraries) != 1:
        raise RuntimeError(
            "backend must map exactly one pg_textsearch library"
        )
    hashes = {}
    for path in sorted(paths):
        source = proc / "root" / path.lstrip("/")
        target = directory / path.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        hashes[path] = hashlib.sha256(target.read_bytes()).hexdigest()
    if hashes[libraries[0]] != expected_sha256:
        raise RuntimeError(
            "mapped native library does not match expected checksum"
        )
    return hashes, libraries[0]


def main(argv=None):
    import psycopg

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsn", default="")
    parser.add_argument("--container", required=True)
    parser.add_argument("--queries", type=Path, required=True)
    parser.add_argument("--ordinal", type=int, default=0)
    parser.add_argument("--kind", choices=KINDS, required=True)
    parser.add_argument(
        "--operation", choices=("search", "count"), default="search"
    )
    parser.add_argument("--output", type=Path, required=True)
    identity = parser.add_mutually_exclusive_group(required=True)
    identity.add_argument("--receipt", type=Path)
    identity.add_argument("--expected-binary-sha256")
    parser.add_argument("--provided-build-sha")
    parser.add_argument("--effective-cache-size")
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args(argv)
    if min(args.limit, args.seconds, args.timeout) <= 0:
        parser.error("limit, seconds, and timeout must be positive")
    receipt = (
        validate_receipt(json.loads(args.receipt.read_text()), args.container)
        if args.receipt else None
    )
    expected = (
        receipt["binary_sha256"] if receipt else args.expected_binary_sha256
    )
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        parser.error("expected binary checksum must be SHA-256")
    validate_container(args.container)
    content = args.queries.read_bytes()
    ordinal, query = select_queries(json.loads(content), [args.ordinal])[0]
    with (
        profile_output(args.output),
        psycopg.connect(
            args.dsn, autocommit=True,
            prepare_threshold=None, connect_timeout=5,
        ) as connection,
    ):
        metadata = database_metadata(
            connection, "pg_textsearch",
            args.timeout, args.effective_cache_size,
        )
        if receipt:
            validate_receipt(receipt, args.container, metadata["index"])
        application = "pgts-profile-" + uuid.uuid4().hex
        namespace_pid = connection.execute(
            "SELECT set_config('application_name', %s, false), "
            "pg_backend_pid()",
            (application,),
        ).fetchone()[1]
        proof = subprocess.check_output([
            "docker", "exec", args.container, "psql", "-XAtw",
            "-U", "postgres", "-d", connection.info.dbname,
            "-v", "ON_ERROR_STOP=1", "-c",
            "SELECT pid FROM pg_stat_activity "
            f"WHERE pid={int(namespace_pid)} "
            f"AND application_name='{application}'",
        ], text=True).strip()
        if proof != str(namespace_pid):
            raise RuntimeError(
                "SQL connection does not belong to selected container"
            )
        pid = host_backend_pid(args.container, namespace_pid)
        baseline = replay_case(
            connection, query, args.kind, args.operation, args.limit, 1
        )
        hashes, library = copy_symbols(pid, args.output / "symbols", expected)
        record = {
            "scope": "single-backend read-only CPU profile; not QPS",
            "query_pack_sha256": hashlib.sha256(content).hexdigest(),
            "ordinal": ordinal, "query": query, "kind": args.kind,
            "operation": args.operation, "container": args.container,
            "backend_pid": namespace_pid, "host_pid": pid,
            "binary_sha256": expected, "symbol_sha256": hashes,
            "provided_build_sha": (
                receipt["source_sha"] if receipt else args.provided_build_sha
            ),
            "metadata": metadata, "baseline": baseline,
            "event": "cpu-clock:u", "frequency": 99,
            "call_graph": "dwarf,8192", "timeout_seconds": args.timeout,
        }
        statement = prepared_statement(args.operation, args.limit, args.kind)
        parameters = {
            "text": query["text"], "tsquery": query["pg_tsquery"][args.kind],
        }
        with (args.output / "record.log").open("x") as log:
            recorder = subprocess.Popen([
                "perf", "record", "-e", "cpu-clock:u", "-F", "99",
                "--call-graph", "dwarf,8192", "--buildid-all",
                "-p", str(pid), "-o", str(args.output / "perf.data"),
            ], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 3
                while not (args.output / "perf.data").exists():
                    if (
                        recorder.poll() is not None
                        or time.monotonic() >= deadline
                    ):
                        raise RuntimeError(
                            "perf did not start; inspect record.log"
                        )
                    time.sleep(.02)
                if recorder.poll() is not None:
                    raise RuntimeError(
                        "perf exited during startup; inspect record.log"
                    )
                record["iterations_ms"] = []
                start = time.perf_counter()
                while True:
                    iteration = time.perf_counter()
                    rows = connection.execute(statement, parameters).fetchall()
                    record["iterations_ms"].append(
                        (time.perf_counter() - iteration) * 1000
                    )
                    if [list(row) for row in rows] != baseline["rows"]:
                        raise RuntimeError("profiled query results changed")
                    if time.perf_counter() - start >= args.seconds:
                        break
                record["status"] = "ok"
            except psycopg.errors.QueryCanceled as error:
                record.update(status="cancelled", sqlstate=error.sqlstate)
            finally:
                stop_recorder(recorder)
        source = Path("/proc") / str(pid) / "root" / library.lstrip("/")
        if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
            raise RuntimeError("native library changed during profiling")
        if database_metadata(
            connection, "pg_textsearch",
            args.timeout, args.effective_cache_size,
        ) != metadata:
            raise RuntimeError("index or settings changed during profiling")
        for name, mode in (
            ("self", "--no-children"), ("inclusive", "--children"),
        ):
            with (args.output / f"report-{name}.txt").open("x") as output:
                subprocess.run([
                    "perf", "report", "--stdio",
                    "-i", str(args.output / "perf.data"),
                    "--symfs", str(args.output / "symbols"),
                    "--sort", "dso,symbol",
                    mode, "-g", "none", "--percent-limit", "0.5",
                ], stdout=output, check=True)
        (args.output / "result.json").write_text(
            json.dumps(record, indent=2) + "\n"
        )
    return 2 if record["status"] == "cancelled" else 0


if __name__ == "__main__":
    raise SystemExit(main())
