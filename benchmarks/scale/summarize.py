#!/usr/bin/env python3
"""Export canonical benchmark cells as search-only CSV statistics."""

import argparse
import csv
import json
import math
from pathlib import Path
import sys


FIELDS = (
    "engine", "mode", "limit", "query_pack", "vus", "status",
    "failure_phase", "exclusion_reason",
    "search_count", "search_seconds", "search_qps",
    "mean_ms", "p50_ms", "p95_ms", "p99_ms",
    "achieved_updates", "update_seconds", "achieved_updates_per_second",
    "dropped_updates",
)


def percentile(ordered, fraction):
    position = (len(ordered) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    return ordered[lower] + (
        ordered[upper] - ordered[lower]
    ) * (position - lower)


def elapsed(run, query):
    timestamps = query["timestamps"]
    if not timestamps or len(timestamps) != len(query["latencies"]):
        raise ValueError("missing or inconsistent query timestamps")
    # Upstream's idle timeout can freeze endTime before queries resume.
    end = max(run["endTime"], max(timestamps))
    seconds = (end - run["startTime"]) / 1000
    if seconds <= 0:
        raise ValueError("nonpositive measurement interval")
    return seconds


def sample_count(rate):
    return rate["passes"] + rate["fails"]


def summarize(root, engines):
    for engine in engines:
        cells = sorted((root / engine).glob("*/*/*vu"))
        if not cells:
            raise ValueError(
                f"no workload cells for requested engine: {engine}"
            )
        for cell in cells:
            complete = (cell / "complete.json").is_file()
            failed = (cell / "failed.json").is_file()
            if complete == failed:
                raise ValueError(f"ambiguous or unfinished cell: {cell}")
            mode, limit, pack_vus = cell.relative_to(root / engine).parts
            pack, vus = pack_vus.rsplit("-", 1)
            row = {
                "engine": engine, "mode": mode,
                "limit": int(limit.removeprefix("top")),
                "query_pack": pack, "vus": int(vus.removesuffix("vu")),
                "status": "ok" if complete else "failed",
                "failure_phase": (
                    "measured" if (cell / "k6.log").exists() else "warmup"
                ) if failed else "",
            }
            if failed:
                yield row
                continue
            fingerprint = json.loads((cell / "complete.json").read_text())
            if (
                engine == "paradedb" and mode == "phrase"
                and fingerprint.get("phrase_protocol") != "quoted-parser-v1"
            ):
                row.update(
                    status="invalid_semantics",
                    exclusion_reason="unverified_phrase_protocol",
                )
                yield row
                continue
            files = list(cell.glob(f"{engine}_*.json"))
            if len(files) != 1:
                raise ValueError(f"expected one raw dashboard export: {cell}")
            dashboard = json.loads(files[0].read_text())
            search_runs = [
                run for run in dashboard["runs"].values()
                if "search" in run["queries"]
            ]
            if len(search_runs) != 1:
                raise ValueError(f"expected one search run: {cell}")
            run, = search_runs
            query = run["queries"]["search"]
            latencies = sorted(query["latencies"])
            seconds = elapsed(run, query)
            metrics = json.loads(
                (cell / "k6-summary.json").read_text()
            )["metrics"]
            if any(
                metrics[name]["value"] != 0
                for name in ("benchmark_errors", "benchmark_timeouts")
            ):
                raise ValueError(f"completed cell contains errors: {cell}")
            updates = metrics.get(
                "update_failures", {"passes": 0, "fails": 0, "value": 0}
            )
            expected = sample_count(
                metrics["benchmark_errors"]
            ) - sample_count(updates)
            if expected != len(latencies):
                raise ValueError(f"inconsistent search sample count: {cell}")
            row.update(
                search_count=len(latencies), search_seconds=seconds,
                search_qps=len(latencies) / seconds,
                mean_ms=math.fsum(latencies) / len(latencies),
                p50_ms=percentile(latencies, 0.50),
                p95_ms=percentile(latencies, 0.95),
                p99_ms=percentile(latencies, 0.99),
            )
            if mode == "mixed":
                update_run, = [
                    run for run in dashboard["runs"].values()
                    if "updates" in run["queries"]
                ]
                update_query = update_run["queries"]["updates"]
                update_seconds = elapsed(update_run, update_query)
                count = metrics["achieved_updates"]["count"]
                if (
                    count != len(update_query["latencies"])
                    or updates["value"] != 0
                ):
                    raise ValueError(
                        f"inconsistent update sample count: {cell}"
                    )
                row.update(
                    achieved_updates=count, update_seconds=update_seconds,
                    achieved_updates_per_second=count / update_seconds,
                    dropped_updates=metrics.get(
                        "dropped_iterations", {}
                    ).get("count", 0),
                )
            yield row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument(
        "--engine", action="append",
        choices=("pg_textsearch", "paradedb", "lead"),
        help="include only this engine; repeat to select multiple engines",
    )
    args = parser.parse_args()
    try:
        rows = list(summarize(
            args.run_directory,
            args.engine or ("pg_textsearch", "paradedb", "lead"),
        ))
        if not rows:
            raise ValueError("no workload cells found")
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    writer = csv.DictWriter(sys.stdout, fieldnames=FIELDS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)


if __name__ == "__main__":
    main()
