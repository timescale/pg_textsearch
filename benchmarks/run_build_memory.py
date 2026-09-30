#!/usr/bin/env python3
"""Measure BM25 build memory on a disposable local PostgreSQL cluster."""

import argparse
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


CASES = ("repeated", "half", "unique", "double", "low-budget", "serial")
FIELDS = (
    "seconds", "phase", "leader_pid", "workers", "leader_rss",
    "leader_private", "worker_private", "build_pss", "temp_bytes",
    "cgroup_current", "cgroup_anon", "cgroup_file",
)


def read_memory(pid, proc=Path("/proc")):
    try:
        text = (proc / str(pid) / "smaps_rollup").read_text()
    except (FileNotFoundError, ProcessLookupError):
        return None
    values = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "kB":
            values[parts[0].rstrip(":")] = int(parts[1]) * 1024
    required = ("Rss", "Pss", "Private_Clean", "Private_Dirty")
    if any(key not in values for key in required):
        raise ValueError(f"Incomplete smaps_rollup for PID {pid}")
    return {
        "rss": values["Rss"],
        "pss": values["Pss"],
        "private": values["Private_Clean"] + values["Private_Dirty"],
    }


def read_cgroup(path):
    def pairs(name):
        return {
            key: int(value)
            for key, value in (
                line.split() for line in (path / name).read_text().splitlines()
            )
        }

    stats = pairs("memory.stat")
    return {
        "current": int((path / "memory.current").read_text()),
        "max": (path / "memory.max").read_text().strip(),
        "anon": stats["anon"],
        "file": stats["file"],
        "events": pairs("memory.events"),
    }


def summarize_samples(samples):
    def peak(rows, field):
        values = [row[field] for row in rows if row[field] is not None]
        return max(values) if values else None

    result = {
        "samples": len(samples),
        "incomplete_samples": sum(s["build_pss"] is None for s in samples),
        "peak_build_pss_bytes": peak(samples, "build_pss"),
        "phases": {},
    }
    for phase in sorted({s["phase"] for s in samples}):
        rows = [s for s in samples if s["phase"] == phase]
        result["phases"][phase] = {
            "peak_build_pss_bytes": peak(rows, "build_pss"),
            "peak_leader_private_bytes": peak(rows, "leader_private"),
            "peak_worker_private_bytes": peak(rows, "worker_private"),
        }
    return result


def temporary_bytes(data):
    total = 0
    for path in (data / "base/pgsql_tmp").rglob("*"):
        try:
            if path.is_file():
                total += path.stat().st_size
        except FileNotFoundError:
            continue  # A worker can unlink its files between samples.
    return total


class Cluster:
    def __init__(self, args, output):
        self.args = args
        self.output = output
        self.data = output / "data"
        self.process = None
        self.socket = None
        self.log = None
        self.env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("PG")
        }
        self.env["LC_ALL"] = "C"

    def start(self):
        self.bin = Path(subprocess.check_output(
            [self.args.pg_config, "--bindir"], text=True
        ).strip())
        self.socket = tempfile.TemporaryDirectory(prefix="pgts-build-memory-")
        self.log = (self.output / "server.log").open("w")
        with (self.output / "initdb.log").open("w") as log:
            subprocess.run(
                [str(self.bin / "initdb"), "-D", str(self.data),
                 "-A", "trust", "--no-locale", "--encoding=UTF8"],
                env=self.env, stdout=log, stderr=subprocess.STDOUT, check=True,
            )
        self.process = subprocess.Popen(
            [str(self.bin / "postgres"), "-D", str(self.data),
             "-k", self.socket.name, "-p", "5432",
             "-c", "listen_addresses=",
             "-c", "shared_preload_libraries=pg_textsearch",
             "-c", "shared_buffers=32MB",
             "-c", "max_connections=12",
             "-c", f"max_worker_processes={max(8, self.args.workers + 2)}",
             "-c", f"max_parallel_workers={self.args.workers}",
             "-c", "pg_textsearch.memory_limit=16MB",
             "-c", "max_wal_size=256MB", "-c", "min_wal_size=80MB",
             "-c", "log_temp_files=0",
             "-c", "log_line_prefix=%m [%p] "],
            env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("PostgreSQL exited; see server.log")
            ready = subprocess.run(
                [str(self.bin / "pg_isready"), "-h", self.socket.name,
                 "-p", "5432", "-d", "postgres"],
                env=self.env, capture_output=True,
            )
            if ready.returncode == 0:
                self.execute("CREATE EXTENSION pg_textsearch")
                return
            time.sleep(0.1)
        raise RuntimeError("PostgreSQL startup timed out; see server.log")

    def command(self, statement):
        return [
            str(self.bin / "psql"), "-X", "-qAt",
            "-h", self.socket.name, "-p", "5432", "-d", "postgres",
            "-v", "ON_ERROR_STOP=1", "-c", statement,
        ]

    def execute(self, statement):
        result = subprocess.run(
            self.command(statement), env=self.env, capture_output=True,
            text=True, timeout=self.args.timeout,
        )
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        return result.stdout.strip()

    def query(self, statement):
        return json.loads(self.execute(statement))

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            # Do not wait for crash recovery after an intentional OOM.
            result = subprocess.run(
                [str(self.bin / "pg_ctl"), "-D", str(self.data),
                 "stop", "-m", "immediate", "-w", "-t", "30"],
                env=self.env, capture_output=True, text=True,
            )
            if result.returncode:
                raise RuntimeError(
                    f"Could not stop benchmark PostgreSQL: {result.stderr}"
                    f"\nData retained at {self.data}"
                )
            self.process.wait(timeout=30)
        if self.log is not None:
            self.log.close()
        if self.socket is not None:
            self.socket.cleanup()
        if self.data.exists():
            shutil.rmtree(self.data)


def sample(cluster, start, cgroup):
    state = cluster.query("""
        SELECT coalesce((
            SELECT json_build_object(
                'pid', a.pid,
                'phase', coalesce(p.phase, 'starting/finishing'),
                'workers', (SELECT coalesce(json_agg(w.pid), '[]'::json)
                            FROM pg_stat_activity w WHERE w.leader_pid=a.pid))
            FROM pg_stat_activity a
            LEFT JOIN pg_stat_progress_create_index p ON p.pid=a.pid
            WHERE a.application_name='build_memory_build'
              AND a.leader_pid IS NULL
        ), 'null'::json)
    """)
    if state is None:
        return None
    leader = read_memory(state["pid"])
    workers = [read_memory(pid) for pid in state["workers"]]
    complete = leader is not None and all(m is not None for m in workers)
    group = read_cgroup(cgroup) if cgroup else None
    return {
        "seconds": round(time.monotonic() - start, 6),
        "phase": state["phase"],
        "leader_pid": state["pid"],
        "workers": len(workers),
        "leader_rss": leader["rss"] if leader else None,
        "leader_private": leader["private"] if leader else None,
        "worker_private": (
            sum(m["private"] for m in workers)
            if all(m is not None for m in workers) else None
        ),
        "build_pss": (
            leader["pss"] + sum(m["pss"] for m in workers)
            if complete else None
        ),
        "temp_bytes": temporary_bytes(cluster.data),
        "cgroup_current": group["current"] if group else None,
        "cgroup_anon": group["anon"] if group else None,
        "cgroup_file": group["file"] if group else None,
    }


def run_case(cluster, name, args, output, cgroup, result):
    terms = args.terms
    if name == "half":
        terms = max(1, terms // 2)
    elif name == "double":
        terms *= 2
    vocabulary = min(1000, args.rows * terms) if name == "repeated" else 0
    workers = 0 if name == "serial" else args.workers
    budget = (
        "16MB" if name in ("serial", "low-budget")
        else args.maintenance_work_mem
    )
    result.update(case=name, rows=args.rows, terms_per_doc=terms,
                  vocabulary=vocabulary, workers_requested=workers,
                  maintenance_work_mem=budget, status="running")
    token = f"((i::bigint - 1) * {terms} + j)"
    if vocabulary:
        token = f"({token} % {vocabulary})"
    width = max(9, len(str(args.rows * args.terms * 2)))
    started = time.monotonic()
    cluster.execute(f"""
        DROP TABLE IF EXISTS docs;
        CREATE TABLE docs AS
        SELECT i AS id, 'anchor ' || (
            SELECT string_agg('t' || lpad({token}::text, {width}, '0'),
                              ' ' ORDER BY j)
            FROM generate_series(1, {terms}) j
        ) AS body FROM generate_series(1, {args.rows}) i;
        ALTER TABLE docs SET (parallel_workers={workers});
        ANALYZE docs;
    """)
    result["load_seconds"] = time.monotonic() - started
    result.update(cluster.query("""
        SELECT json_build_object(
            'text_bytes', sum(octet_length(body)),
            'heap_bytes', pg_relation_size('docs'))
        FROM docs
    """))
    cluster.execute("CHECKPOINT")
    result["cgroup_before"] = read_cgroup(cgroup) if cgroup else None
    samples = []
    log_path = output / f"{name}.log"
    csv_path = output / f"{name}.csv"
    with log_path.open("w") as log, csv_path.open("w") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=FIELDS)
        writer.writeheader()
        started = time.monotonic()
        process = subprocess.Popen(
            cluster.command(f"""
                SET maintenance_work_mem='{budget}';
                SET max_parallel_maintenance_workers={workers};
                SET statement_timeout={int(args.timeout * 1000)};
            """) + ["-c", "\\timing on", "-c", """
                CREATE INDEX probe_idx ON docs USING bm25(body)
                    WITH (text_config='simple');
            """],
            env={**cluster.env, "PGAPPNAME": "build_memory_build"},
            stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            while process.poll() is None:
                if time.monotonic() - started > args.timeout:
                    raise RuntimeError(f"{name}: build timed out")
                row = sample(cluster, started, cgroup)
                if row is not None:
                    samples.append(row)
                    writer.writerow(row)
                    csvfile.flush()
                time.sleep(args.interval)
        finally:
            result["observed_seconds"] = time.monotonic() - started
            if process.poll() is None:
                process.terminate()
            result["exit_code"] = process.wait(timeout=30)
            result.update(summarize_samples(samples))
            if cgroup:
                try:
                    result["cgroup_after"] = read_cgroup(cgroup)
                except FileNotFoundError:
                    result["cgroup_after"] = None
                    result["cgroup_error"] = "Cgroup disappeared after exit"
    if process.returncode:
        raise RuntimeError(f"{name}: CREATE INDEX failed; see {log_path}")
    build_log = log_path.read_text()
    timing = re.search(r"^Time: ([0-9.]+) ms", build_log, re.MULTILINE)
    if timing is None:
        raise RuntimeError(f"{name}: missing CREATE INDEX timing")
    result["build_seconds"] = float(timing[1]) / 1000
    if result["peak_build_pss_bytes"] is None:
        raise RuntimeError(
            f"{name}: no complete memory samples; use more rows"
        )
    launched = re.search(
        r"launched (\d+) of \d+ requested workers", build_log
    )
    result["workers_launched"] = int(launched[1]) if launched else 0
    if result["workers_launched"] != workers:
        raise RuntimeError(f"{name}: requested {workers} workers but launched "
                           f"{result['workers_launched']}")
    result.update(cluster.query(f"""
        SET enable_seqscan=off;
        SELECT json_build_object(
            'index_bytes', pg_relation_size('probe_idx'),
            'matching_docs', (SELECT count(*) FROM (
                SELECT id FROM docs
                ORDER BY body <@> to_bm25query('anchor', 'probe_idx')
                LIMIT {args.rows}) q))
    """))
    if result["matching_docs"] != args.rows:
        raise RuntimeError(f"{name}: anchor query lost documents")
    if not vocabulary:
        last = f"t{args.rows * terms:0{width}d}"
        matches = cluster.query(f"""
            SELECT coalesce(json_agg(id), '[]'::json) FROM (
                SELECT id FROM docs
                ORDER BY body <@> to_bm25query('{last}', 'probe_idx')
                LIMIT 10) q
        """)
        if matches != [args.rows]:
            raise RuntimeError(f"{name}: last-token query returned {matches}")
    result["status"] = "ok"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pg-config",
                        default=os.environ.get("PG_CONFIG", "pg_config"))
    parser.add_argument("--case", choices=CASES, action="append", dest="cases",
                        help="repeat to select cases; default: all six")
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--terms", type=int, default=10)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--maintenance-work-mem", default="64MB")
    parser.add_argument("--interval", type=float, default=0.1,
                        help="delay between samples, seconds")
    parser.add_argument("--timeout", type=float, default=300,
                        help="per-command/build timeout, seconds")
    parser.add_argument("--output", type=Path,
                        help="new results directory; must not already exist")
    parser.add_argument("--cgroup", action="store_true",
                        help="record cgroup v2 metrics; use a private scope")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("memory sampling requires Linux /proc")
    if args.rows <= 0 or args.terms <= 0 or args.workers < 0:
        parser.error("rows/terms must be positive and workers nonnegative")
    if args.workers > 0 and args.rows < 100000:
        parser.error("parallel cases require at least 100000 rows")
    if any(not math.isfinite(v) or v <= 0
           for v in (args.interval, args.timeout)):
        parser.error("interval and timeout must be positive finite seconds")
    if not re.fullmatch(r"[1-9][0-9]*(?:kB|MB|GB)", args.maintenance_work_mem):
        parser.error("maintenance-work-mem must be a size such as 64MB")
    args.cases = list(dict.fromkeys(args.cases or CASES))
    return args


def main():
    args = parse_args()
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ")
    output = (args.output or Path(__file__).resolve().parent /
              "results" / f"build_memory_{timestamp}").resolve()
    output.mkdir(parents=True, exist_ok=False)
    summary = {
        "status": "running", "timestamp": timestamp,
        "options": {k: str(v) if isinstance(v, Path) else v
                    for k, v in vars(args).items()},
        "cases": [],
    }
    cluster = Cluster(args, output)
    try:
        cgroup = None
        if args.cgroup:
            entries = Path("/proc/self/cgroup").read_text().splitlines()
            unified = [line[3:] for line in entries if line.startswith("0::")]
            if len(unified) != 1:
                raise RuntimeError("--cgroup requires a unified cgroup v2")
            cgroup = Path("/sys/fs/cgroup") / unified[0].lstrip("/")
            summary["cgroup_path"] = str(cgroup)
            read_cgroup(cgroup)
        cluster.start()
        summary["server"] = cluster.query("""
            SELECT json_build_object(
                'postgres_version', version(),
                'extension_version', (SELECT extversion FROM pg_extension
                                      WHERE extname='pg_textsearch'),
                'settings', (SELECT json_object_agg(name, setting)
                             FROM pg_settings WHERE name IN (
                                 'shared_buffers', 'block_size', 'fsync',
                                 'max_parallel_workers',
                                 'max_worker_processes',
                                 'pg_textsearch.memory_limit',
                                 'pg_textsearch.compress_segments')))
        """)
        print(f"Results: {output}", flush=True)
        print("case             seconds   peak build PSS MiB   index MiB",
              flush=True)
        for name in args.cases:
            result = {}
            summary["cases"].append(result)
            try:
                run_case(cluster, name, args, output, cgroup, result)
            except (OSError, RuntimeError, ValueError,
                    subprocess.SubprocessError, KeyboardInterrupt) as exc:
                result.update(status="error", error=str(exc) or "Interrupted")
                raise
            finally:
                (output / "summary.json").write_text(
                    json.dumps(summary, indent=2) + "\n"
                )
            print(f"{name:16} {result['build_seconds']:7.2f} "
                  f"{result['peak_build_pss_bytes'] / 2**20:20.1f} "
                  f"{result['index_bytes'] / 2**20:11.2f}", flush=True)
        summary["status"] = "ok"
    except (OSError, RuntimeError, ValueError,
            subprocess.SubprocessError, KeyboardInterrupt) as exc:
        summary.update(status="error", error=str(exc) or "Interrupted")
        print(f"ERROR: {summary['error']}", file=sys.stderr)
    finally:
        try:
            cluster.stop()
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            summary.update(status="error", cleanup_error=str(exc))
            print(f"ERROR: {exc}", file=sys.stderr)
        (output / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n"
        )
    return 0 if summary["status"] == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
