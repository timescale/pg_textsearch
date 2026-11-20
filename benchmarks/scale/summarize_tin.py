#!/usr/bin/env python3
"""Export reconciled expanded TIN benchmark cells as public CSV."""

import argparse
import csv
import json
import math
from pathlib import Path
import sys

if __package__:
    from .summarize import elapsed, percentile
else:
    from summarize import elapsed, percentile


FIELDS = (
    "dataset", "engine", "cell_id", "profile", "analyzer",
    "analyzer_role", "query_pack", "operation", "query_family",
    "query_kinds", "sql_shape", "vus", "limit", "status",
    "failure_phase", "exclusion_reason", "search_count",
    "search_seconds", "search_qps", "mean_ms", "p50_ms", "p95_ms",
    "p99_ms", "error_count", "timeout_count", "per_kind_counts",
    "per_kind_latency_status", "target_write_rate", "target_writes",
    "actual_writes", "attempted_writes", "failed_writes",
    "dropped_arrivals", "write_seconds", "achieved_write_rate",
    "write_rate_basis", "target_met", "load_quality",
)

TERMINALS = {
    "complete.json": ("ok", ""),
    "failed.json": ("failed", "measured"),
    "plan-gate-failed.json": ("plan_gated", "plan_gate"),
    "plan-audit-failed.json": ("plan_gated", "plan_audit"),
}

NON_DASHBOARD_JSON = set(TERMINALS) | {
    "k6-summary.json", "load-quality.json", "write-cleanup.json",
}


def _read_json(path):
    return json.loads(path.read_text())


def _fingerprint(marker, data):
    if marker == "complete.json":
        return data
    return data["fingerprint"]


def _metric_counts(metric):
    passes = metric["passes"]
    fails = metric["fails"]
    if (
        not isinstance(passes, int)
        or not isinstance(fails, int)
        or passes < 0
        or fails < 0
    ):
        raise ValueError("invalid k6 rate counter")
    return passes, fails


def _one_file(cell, pattern, description):
    files = sorted(cell.glob(pattern))
    if len(files) != 1:
        raise ValueError(f"expected one {description}: {cell}")
    return files[0]


def _dashboard(cell):
    files = [
        path for path in cell.glob("*.json")
        if path.name not in NON_DASHBOARD_JSON
    ]
    if len(files) != 1:
        raise ValueError(f"expected one raw dashboard export: {cell}")
    return _read_json(files[0])


def _optional_dashboard(cell):
    files = [
        path for path in cell.glob("*.json")
        if path.name not in NON_DASHBOARD_JSON
    ]
    if len(files) > 1:
        raise ValueError(f"expected at most one dashboard export: {cell}")
    return _read_json(files[0]) if files else None


def _query(run, name):
    value = run["queries"][name]
    timestamps = value["timestamps"]
    latencies = value["latencies"]
    if (
        not timestamps
        or len(timestamps) != len(latencies)
        or not all(isinstance(item, (int, float)) for item in latencies)
    ):
        raise ValueError(f"missing or inconsistent {name} samples")
    return value


def _find_query(dashboard, name, cell):
    matches = [
        (run, run["queries"][name])
        for run in dashboard["runs"].values()
        if name in run.get("queries", {})
    ]
    if len(matches) != 1:
        raise ValueError(f"expected one {name} dashboard series: {cell}")
    run, _ = matches[0]
    return run, _query(run, name)


def _per_kind_counts(cell, expected_kinds, search_count):
    path = _one_file(cell, "*.csv", "per-query CSV")
    counts = {kind: 0 for kind in expected_kinds}
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"empty per-query CSV: {cell}")
    for row in rows:
        query_id = row.get("query_id", "")
        try:
            kind = query_id.rsplit(":", 1)[1]
            count = int(row["count"])
        except (IndexError, KeyError, ValueError) as error:
            raise ValueError(f"invalid per-query CSV: {cell}") from error
        if kind not in counts or count <= 0:
            raise ValueError(f"unexpected query kind in per-query CSV: {cell}")
        counts[kind] += count
    if sum(counts.values()) != search_count or any(
        count == 0 for count in counts.values()
    ):
        raise ValueError(f"per-query CSV sample count mismatch: {cell}")
    return counts


def _duration_seconds(value):
    units = {"s": 1, "m": 60, "h": 3600}
    try:
        return float(value[:-1]) * units[value[-1]]
    except (KeyError, ValueError, IndexError) as error:
        raise ValueError(
            f"unsupported measurement duration: {value}"
        ) from error


def _validate_load_quality(
    cell, quality, *, target_rate, target_updates,
    measurement_seconds, actual, attempts, failed, dropped,
):
    if _duration_seconds(quality["duration"]) != measurement_seconds:
        raise ValueError(
            f"load quality measurement duration mismatch: {cell}"
        )
    if quality.get("target_updates") != target_updates:
        raise ValueError(f"load quality target updates mismatch: {cell}")
    target_met = actual >= target_updates and dropped == 0
    expected = {
        "target_rate": target_rate,
        "actual_writes": actual,
        "attempted_writes": attempts,
        "failed_writes": failed,
        "dropped_arrivals": dropped,
        "target_met": target_met,
        "unequal_pressure": not target_met,
    }
    if any(quality.get(key) != value for key, value in expected.items()):
        raise ValueError(f"load quality does not match raw counters: {cell}")
    return target_met


def _write_measurement(
    cell, row, metrics, dashboard, measurement_seconds,
    failed_measurement=False,
):
    update_failures, update_successes = _metric_counts(
        metrics["update_failures"]
    )
    update_attempts = update_failures + update_successes
    if row["operation"] != "concurrent-writes":
        if update_attempts or metrics.get(
            "achieved_updates", {"count": 0}
        )["count"]:
            raise ValueError(f"non-write cell contains write samples: {cell}")
        return update_attempts, update_failures, None

    actual = metrics.get("achieved_updates", {"count": 0})["count"]
    if actual != update_successes:
        raise ValueError(f"successful write counter mismatch: {cell}")
    dropped = metrics.get(
        "dropped_iterations", {"count": 0}
    )["count"]
    quality = _read_json(cell / "load-quality.json")
    target_rate = row["target_write_rate"]
    target_updates = int(target_rate * measurement_seconds)
    target_met = _validate_load_quality(
        cell, quality, target_rate=target_rate,
        target_updates=target_updates,
        measurement_seconds=measurement_seconds,
        actual=actual, attempts=update_attempts,
        failed=update_failures, dropped=dropped,
    )
    row.update(
        target_writes=target_updates,
        actual_writes=actual,
        attempted_writes=update_attempts,
        failed_writes=update_failures,
        dropped_arrivals=dropped,
        target_met=target_met,
        load_quality="met" if target_met else "degraded",
    )
    if dashboard is not None:
        matches = [
            run for run in dashboard["runs"].values()
            if "updates" in run.get("queries", {})
        ]
        if len(matches) > 1:
            raise ValueError(
                f"expected at most one updates dashboard series: {cell}"
            )
        expected_samples = (
            update_successes if failed_measurement else update_attempts
        )
        if not matches:
            if expected_samples:
                raise ValueError(f"missing write latency samples: {cell}")
            row["write_rate_basis"] = (
                "unavailable_no_success_latency_samples"
            )
            return update_attempts, update_failures, quality
        update_run = matches[0]
        updates = update_run["queries"]["updates"]
        timestamps = updates["timestamps"]
        latencies = updates["latencies"]
        if len(timestamps) != len(latencies):
            raise ValueError(f"inconsistent write latency samples: {cell}")
        if len(latencies) != expected_samples:
            raise ValueError(f"write sample count mismatch: {cell}")
        if not latencies:
            row["write_rate_basis"] = (
                "unavailable_no_success_latency_samples"
            )
            return update_attempts, update_failures, quality
        write_seconds = elapsed(update_run, updates)
        row.update(
            write_seconds=write_seconds,
            achieved_write_rate=actual / write_seconds,
            write_rate_basis="successful_writes_per_dashboard_elapsed",
        )
    return update_attempts, update_failures, quality


def _measurement(cell, row, expected_kinds, measurement_seconds):
    dashboard = _dashboard(cell)
    search_run, search = _find_query(dashboard, "queries", cell)
    latencies = sorted(search["latencies"])
    search_seconds = elapsed(search_run, search)
    metrics = _read_json(cell / "k6-summary.json")["metrics"]
    errors, error_successes = _metric_counts(
        metrics["benchmark_errors"]
    )
    timeouts, timeout_successes = _metric_counts(
        metrics["benchmark_timeouts"]
    )
    update_attempts, _, quality = _write_measurement(
        cell, row, metrics, dashboard, measurement_seconds
    )
    expected_samples = len(latencies) + update_attempts
    if (
        error_successes + errors != expected_samples
        or timeout_successes + timeouts != expected_samples
    ):
        raise ValueError(f"k6 and dashboard sample count mismatch: {cell}")

    kind_counts = _per_kind_counts(
        cell, expected_kinds, len(latencies)
    )
    row.update(
        error_count=errors,
        timeout_count=timeouts,
        per_kind_counts=json.dumps(
            kind_counts, sort_keys=True, separators=(",", ":")
        ),
        per_kind_latency_status="unavailable_aggregated_query_csv",
    )

    reasons = []
    if errors:
        reasons.append("sql_errors")
    if timeouts:
        reasons.append("timeouts")
    if quality is not None and quality.get("sql_status") != "ok":
        reasons.append("sql_failure")

    if reasons:
        row.update(
            status="failed",
            failure_phase="measured",
            exclusion_reason=";".join(reasons),
        )
        return
    row.update(
        search_count=len(latencies),
        search_seconds=search_seconds,
        search_qps=len(latencies) / search_seconds,
        mean_ms=math.fsum(latencies) / len(latencies),
        p50_ms=percentile(latencies, 0.50),
        p95_ms=percentile(latencies, 0.95),
        p99_ms=percentile(latencies, 0.99),
    )


def _failed_measurement(cell, row, measurement_seconds):
    summary = cell / "k6-summary.json"
    if not summary.is_file():
        return
    metrics = _read_json(summary)["metrics"]
    errors, _ = _metric_counts(metrics["benchmark_errors"])
    timeouts, _ = _metric_counts(metrics["benchmark_timeouts"])
    row.update(error_count=errors, timeout_count=timeouts)
    dashboard = _optional_dashboard(cell)
    _write_measurement(
        cell, row, metrics, dashboard, measurement_seconds,
        failed_measurement=True,
    )


def _cell_row(
    root, dataset, engine, cell, measurement_seconds
):
    markers = [name for name in TERMINALS if (cell / name).is_file()]
    if len(markers) != 1:
        raise ValueError(f"ambiguous or unfinished TIN cell: {cell}")
    marker = markers[0]
    marker_data = _read_json(cell / marker)
    fingerprint = _fingerprint(marker, marker_data)
    metadata = fingerprint["cell"]
    if metadata["cell_id"] != cell.name or fingerprint["engine"] != engine:
        raise ValueError(f"cell fingerprint does not match path: {cell}")
    scenario = root / "scenarios" / engine / f"{cell.name}.json"
    expected_kinds = (
        _read_json(scenario)["kinds"] if scenario.is_file() else []
    )
    status, phase = TERMINALS[marker]
    row = {
        "dataset": dataset,
        "engine": engine,
        "cell_id": metadata["cell_id"],
        "profile": metadata["profile"],
        "analyzer": metadata["analyzer"],
        "analyzer_role": metadata["analyzer_role"],
        "query_pack": metadata["pack"],
        "operation": metadata["operation"],
        "query_family": metadata["query_family"],
        "query_kinds": ",".join(expected_kinds),
        "sql_shape": metadata["sql_shape"],
        "vus": metadata["vus"],
        "limit": metadata["limit"],
        "status": status,
        "failure_phase": phase,
        "target_write_rate": metadata["update_rate"],
        "load_quality": (
            "" if metadata["operation"] == "concurrent-writes"
            else "not_applicable"
        ),
    }
    if marker == "complete.json":
        if not expected_kinds:
            raise ValueError(f"missing scenario metadata: {cell}")
        _measurement(
            cell, row, expected_kinds, measurement_seconds
        )
    else:
        if marker == "failed.json":
            row["failure_phase"] = (
                "measured" if (cell / "k6.log").exists() else "warmup"
            )
        row["exclusion_reason"] = marker_data.get(
            "kind", marker_data.get("status", status)
        )
        if marker == "failed.json":
            _failed_measurement(
                cell, row, measurement_seconds
            )
    return row


def summarize(root, engines=None):
    if not (root / "results").is_dir() and (root / "tin").is_dir():
        root = root / "tin"
    plan = _read_json(root / "plan.json")
    duration = plan.get(
        "measurement_duration",
        "3s" if plan["dataset"] == "synthetic-smoke" else "5m",
    )
    measurement_seconds = _duration_seconds(duration)
    results = root / "results"
    selected = engines or sorted(
        path.name for path in results.iterdir() if path.is_dir()
    )
    for engine in selected:
        engine_dir = results / engine
        cells = sorted(path for path in engine_dir.iterdir() if path.is_dir())
        if not cells:
            raise ValueError(f"no TIN cells for requested engine: {engine}")
        for cell in cells:
            yield _cell_row(
                root, plan["dataset"], engine, cell,
                measurement_seconds,
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument(
        "--engine", action="append",
        help="include only this engine; repeat to select multiple engines",
    )
    args = parser.parse_args()
    try:
        rows = list(summarize(args.run_directory, args.engine))
        if not rows:
            raise ValueError("no TIN workload cells found")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    writer = csv.DictWriter(sys.stdout, fieldnames=FIELDS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)


if __name__ == "__main__":
    main()
