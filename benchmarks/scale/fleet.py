#!/usr/bin/env python3
"""Run one immutable TIN benchmark shard on a dedicated fleet worker."""

import argparse
import hashlib
import json
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

from benchmarks.scale.run import ConfigError, file_sha256, validate_storage
from benchmarks.scale.tin import (
    ANALYZERS,
    BUILD_RESOURCES,
    DATABASE_PROOF_PROTOCOL,
    PROFILES,
    REPO_ROOT,
    TinRunner,
    analyzer_sql,
    apply_sql_shape,
    build_tin_plan,
    database_size_proof,
    load_quality,
    select_cells,
)


ENGINES = ("pg_textsearch", "paradedb")
DATASET_ROWS = {
    "msmarco-v2": 138_364_198,
    "wikipedia": 6_407_814,
}
TERMINAL_MARKERS = {
    "complete.json": "completed",
    "failed.json": "failed",
    "plan-gate-failed.json": "plan_gated",
    "plan-audit-failed.json": "plan_audit_failed",
}
MEASURED_FAILURES = {
    "failed",
    "plan_gated",
    "plan_audit_failed",
}


def _atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def fleet_plan(dataset):
    plan = build_tin_plan(
        smoke=False,
        accept_terms=True,
        dataset_kind=dataset,
    )
    plan["selected_cells"] = apply_sql_shape(
        plan["cells"], "filter_first"
    )
    return plan


def shard_cells(plan, engine, profile):
    if engine not in ENGINES:
        raise ConfigError(f"unsupported fleet engine: {engine}")
    if profile not in PROFILES:
        raise ConfigError(f"unsupported fleet profile: {profile}")
    return select_cells(
        plan["selected_cells"], profiles={profile}
    )


def _marker_fingerprint(marker, payload):
    if marker == "complete.json":
        return payload
    return payload.get("fingerprint")


def _validate_terminal(result_dir, engine, cell, marker):
    path = result_dir / marker
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigError(f"invalid terminal marker: {path}") from error
    fingerprint = _marker_fingerprint(marker, payload)
    if not isinstance(fingerprint, dict):
        raise ConfigError(f"terminal fingerprint is missing: {path}")
    if (
        fingerprint.get("engine") != engine
        or fingerprint.get("cell") != cell
    ):
        raise ConfigError(f"terminal fingerprint mismatch: {path}")
    if marker == "complete.json" and not (
        result_dir / "k6-summary.json"
    ).is_file():
        raise ConfigError(
            f"completed cell is missing k6-summary.json: {result_dir}"
        )
    if marker == "plan-gate-failed.json":
        reference = payload.get("probe")
        if (
            not isinstance(reference, str)
            or not (result_dir / reference).is_file()
        ):
            raise ConfigError(
                f"terminal referenced artifact is missing: {path}"
            )
    return {
        "cell_id": cell["cell_id"],
        "status": TERMINAL_MARKERS[marker],
        "marker": marker,
        "result_dir": str(result_dir),
    }


def scan_cell_results(
    tin_dir,
    engine,
    cells,
    *,
    archive_partials=False,
    attempt_id=None,
):
    statuses = []
    for cell in cells:
        result_dir = tin_dir / "results" / engine / cell["cell_id"]
        markers = [
            marker for marker in TERMINAL_MARKERS
            if (result_dir / marker).is_file()
        ]
        if len(markers) > 1:
            raise ConfigError(
                f"expected exactly one terminal marker: {result_dir}"
            )
        if markers:
            statuses.append(
                _validate_terminal(
                    result_dir, engine, cell, markers[0]
                )
            )
            continue
        contents = list(result_dir.iterdir()) if result_dir.is_dir() else []
        archived = None
        status = "pending"
        if contents and archive_partials:
            if not attempt_id:
                raise ConfigError("partial archival requires an attempt id")
            archived = (
                tin_dir / "fleet" / "forensics" / engine
                / cell["cell_id"] / attempt_id
            )
            if archived.exists():
                raise ConfigError(
                    f"partial archive already exists: {archived}"
                )
            archived.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(result_dir), str(archived))
            result_dir.mkdir(parents=True)
        elif contents:
            status = "partial"
        else:
            result_dir.mkdir(parents=True, exist_ok=True)
        record = {
            "cell_id": cell["cell_id"],
            "status": status,
            "marker": None,
            "result_dir": str(result_dir),
        }
        if archived is not None:
            record["archived_partial"] = str(archived)
        statuses.append(record)
    return statuses


def run_pending_cells(runner, engine, cells, statuses):
    pending_ids = {
        status["cell_id"] for status in statuses
        if status["status"] == "pending"
    }
    pending = [
        cell for cell in cells if cell["cell_id"] in pending_ids
    ]
    if pending:
        runner.run_cells([engine], pending)
    return pending


def build_shard_summary(
    *,
    engine,
    profile,
    dataset,
    run_id,
    cells,
    statuses,
    write_quality,
    host,
):
    counts = {}
    for status in statuses:
        name = status["status"]
        counts[name] = counts.get(name, 0) + 1
    terminal = sum(
        counts.get(name, 0) for name in TERMINAL_MARKERS.values()
    )
    measured = any(
        status["status"] in MEASURED_FAILURES for status in statuses
    )
    incomplete = terminal != len(cells)
    if incomplete:
        overall = "infrastructure_error"
        exit_code = 3
    elif measured:
        overall = "measured_failures"
        exit_code = 2
    else:
        overall = "ok"
        exit_code = 0
    return {
        "format": 1,
        "status": overall,
        "dataset": dataset,
        "run_id": run_id,
        "engine": engine,
        "profile": profile,
        "host": host,
        "expected_cells": len(cells),
        "terminal_cells": terminal,
        "terminal_counts": counts,
        "cells": statuses,
        "write_quality_complete": all(
            quality.get("complete") is True
            for quality in write_quality
        ),
        "write_quality": write_quality,
    }, exit_code


def _read_json(path, description):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigError(f"invalid {description}: {path}") from error


def _validate_query_pack(runner, analyzer, pack):
    path = runner.tin_dir / "query-packs" / analyzer / f"{pack}.json"
    state = path.with_suffix(".fingerprint.json")
    if not path.is_file() or not state.is_file():
        raise ConfigError(f"imported query pack is missing: {path}")
    fingerprint = _read_json(state, "query-pack fingerprint")
    source = runner.dataset / "k6" / f"{pack}.json"
    expected = {
        "format": 1,
        "analyzer": analyzer,
        "text_config": ANALYZERS[analyzer]["native_config"],
        "pack": pack,
        "source_sha256": file_sha256(source),
    }
    if fingerprint != expected:
        raise ConfigError(f"imported query pack is stale: {path}")


def validate_imported_inputs(runner, engine, cells):
    manifest_path = runner.dataset / "manifest.json"
    manifest = _read_json(manifest_path, "dataset manifest")
    expected_rows = DATASET_ROWS[runner.dataset_kind]
    if manifest.get("rows") != expected_rows:
        raise ConfigError(
            f"dataset manifest row count is not {expected_rows}"
        )
    for path in (
        runner.dataset / "k6" / "update_targets.json",
        runner.dataset / "backends",
    ):
        if not path.exists():
            raise ConfigError(f"dataset runtime input is missing: {path}")
    images, image_lock = runner._images()
    analyzers = {cell["analyzer"] for cell in cells}
    packs = {cell["pack"] for cell in cells}
    for analyzer in analyzers:
        build_dir = runner.tin_dir / "build" / engine / analyzer
        complete_path = build_dir / "complete.json"
        build_path = build_dir / "build.json"
        proof_path = build_dir / "rowcount-and-sizes.tsv"
        complete = _read_json(complete_path, "analyzer fingerprint")
        build = _read_json(build_path, "analyzer build metadata")
        proof = database_size_proof(
            proof_path.read_text(), expected_rows=expected_rows
        )
        base_path = (
            runner.tin_dir / "build" / engine
            / "prepared-corpus/complete.json"
        )
        expected_complete = {
            "format": 1,
            "engine": engine,
            "analyzer": ANALYZERS[analyzer],
            "image": images[engine],
            "sql_sha256": hashlib.sha256(
                analyzer_sql(engine, analyzer).encode()
            ).hexdigest(),
            "build_resources": BUILD_RESOURCES,
            "base": _read_json(
                base_path, "prepared-corpus fingerprint"
            ),
            "database_proof_protocol": DATABASE_PROOF_PROTOCOL,
        }
        if (
            complete != expected_complete
            or build.get("database_proof_protocol")
            != DATABASE_PROOF_PROTOCOL
            or build.get("database") != proof
        ):
            raise ConfigError(
                f"imported analyzer proof is incompatible: "
                f"{engine}/{analyzer}"
            )
        runner._require_owned_volume(
            runner._volume_name("analyzer", engine, analyzer),
            {
                "kind": "analyzer",
                "engine": engine,
                "analyzer": analyzer,
            },
        )
        for pack in packs:
            _validate_query_pack(runner, analyzer, pack)
    return image_lock


def _write_quality(runner, engine, cells, statuses):
    by_id = {cell["cell_id"]: cell for cell in cells}
    records = []
    for status in statuses:
        cell = by_id[status["cell_id"]]
        if cell["operation"] != "concurrent-writes":
            continue
        if status["status"] in {"plan_gated", "plan_audit_failed"}:
            records.append({
                "cell_id": cell["cell_id"],
                "status": "not_executed",
                "complete": True,
            })
            continue
        if status["status"] not in {"completed", "failed"}:
            records.append({
                "cell_id": cell["cell_id"],
                "status": status["status"],
                "complete": False,
            })
            continue
        result_dir = (
            runner.tin_dir / "results" / engine / cell["cell_id"]
        )
        if status["status"] == "completed":
            quality = runner._record_write_quality(
                result_dir=result_dir,
                cell=cell,
                engine=engine,
                duration="5m",
                sql_status="ok",
            )
        else:
            quality_path = result_dir / "load-quality.json"
            summary_path = result_dir / "k6-summary.json"
            if summary_path.is_file():
                quality = load_quality(
                    _read_json(summary_path, "k6 summary"),
                    update_rate=cell["update_rate"],
                    duration="5m",
                )
                quality.update({
                    "engine": engine,
                    "cell_id": cell["cell_id"],
                    "sql_status": "failed",
                })
                runner._write_or_validate_json(quality_path, quality)
            else:
                quality = _read_json(
                    quality_path, "write load quality"
                )
        records.append({
            "cell_id": cell["cell_id"],
            "status": status["status"],
            "complete": True,
            "quality": quality,
        })
    return records


def _source_manifest(runner, engine, profile, plan, cells, host, image_lock):
    return {
        "format": 1,
        "host": host,
        "engine": engine,
        "profile": profile,
        "dataset": runner.dataset_kind,
        "run_id": runner.run_id,
        "expected_cell_ids": [cell["cell_id"] for cell in cells],
        "plan_sha256": file_sha256(runner.tin_dir / "plan.json"),
        "dataset_manifest_sha256": file_sha256(
            runner.dataset / "manifest.json"
        ),
        "fleet_source_sha256": file_sha256(Path(__file__)),
        "tin_source_sha256": file_sha256(
            runner.root / "benchmarks/scale/tin.py"
        ),
        "driver_sha256": file_sha256(
            runner.root / "benchmarks/scale/benchmarker/k6/tin.js"
        ),
        "images": image_lock,
        "plan_sources": {
            "benchmarker_sha": plan["benchmarker_sha"],
            "pg_textsearch_sha": plan["pg_textsearch_sha"],
            "paradedb_digest": plan["paradedb_digest"],
        },
    }


def _preserve_runner_summary(runner, history, attempt_id):
    path = runner.tin_dir / "summary.json"
    if not path.is_file():
        return None
    destination = history / f"{attempt_id}.previous-run-cells-summary.json"
    history.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise ConfigError(f"fleet history already exists: {destination}")
    path.replace(destination)
    return destination


def _is_measured_run_error(
    error, summary_path, engine, pending_ids, final_statuses
):
    if not isinstance(error, ConfigError) or not summary_path.is_file():
        return False
    try:
        run_summary = _read_json(summary_path, "run-cells summary")
    except ConfigError:
        return False
    final_by_id = {
        status["cell_id"]: status["status"]
        for status in final_statuses
    }
    if any(
        final_by_id.get(cell_id) not in TERMINAL_MARKERS.values()
        for cell_id in pending_ids
    ):
        return False
    new_failures = {
        cell_id for cell_id in pending_ids
        if final_by_id.get(cell_id) in MEASURED_FAILURES
    }
    failures = run_summary.get("failures")
    if (
        not isinstance(failures, list)
        or any(not isinstance(failure, dict) for failure in failures)
    ):
        return False
    failure_ids = [failure.get("cell_id") for failure in failures]
    if (
        not new_failures
        or run_summary.get("status") != "failed"
        or run_summary.get("selected_cells") != len(pending_ids)
        or run_summary.get("engines") != [engine]
        or any(failure.get("engine") != engine for failure in failures)
        or len(failure_ids) != len(set(failure_ids))
        or set(failure_ids) != new_failures
        or str(error) != (
            f"TIN campaign has {len(failures)} failed cells"
        )
    ):
        return False
    return True


def execute_shard(
    runner,
    *,
    engine,
    profile,
    dataset,
    build=False,
    host=None,
    attempt_id=None,
):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", runner.run_id):
        raise ConfigError(
            "run id must be one safe path component using only "
            "letters, digits, underscores, and hyphens"
        )
    plan = fleet_plan(dataset)
    runner._write_or_validate_json(runner.tin_dir / "plan.json", plan)
    cells = shard_cells(plan, engine, profile)
    if build:
        if engine != "pg_textsearch":
            raise ConfigError(
                "--build is only safe on a native worker; "
                "import ParadeDB builds and enriched query packs"
            )
        runner.build_selected(
            [engine], sorted({cell["analyzer"] for cell in cells})
        )
    image_lock = validate_imported_inputs(runner, engine, cells)
    host = host or socket.gethostname()
    attempt_id = attempt_id or str(time.time_ns())
    shard_dir = runner.tin_dir / "fleet" / "shards" / (
        f"{engine}-{profile}"
    )
    shard_dir.mkdir(parents=True, exist_ok=True)
    manifest = _source_manifest(
        runner, engine, profile, plan, cells, host, image_lock
    )
    runner._write_or_validate_json(
        shard_dir / "manifest.json", manifest
    )
    statuses = scan_cell_results(
        runner.tin_dir,
        engine,
        cells,
        archive_partials=True,
        attempt_id=attempt_id,
    )
    history = shard_dir / "history"
    pending = [
        status for status in statuses
        if status["status"] == "pending"
    ]
    run_error = None
    if pending:
        _preserve_runner_summary(runner, history, attempt_id)
        try:
            run_pending_cells(runner, engine, cells, statuses)
        except (
            ConfigError,
            OSError,
            subprocess.CalledProcessError,
            json.JSONDecodeError,
        ) as error:
            run_error = error
        current_summary = runner.tin_dir / "summary.json"
        if current_summary.is_file():
            history.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(
                current_summary,
                history / f"{attempt_id}.run-cells-summary.json",
            )
    final_statuses = scan_cell_results(
        runner.tin_dir, engine, cells, archive_partials=False
    )
    measured_run_error = _is_measured_run_error(
        run_error,
        runner.tin_dir / "summary.json",
        engine,
        {status["cell_id"] for status in pending},
        final_statuses,
    )
    quality = _write_quality(runner, engine, cells, final_statuses)
    summary, exit_code = build_shard_summary(
        engine=engine,
        profile=profile,
        dataset=dataset,
        run_id=runner.run_id,
        cells=cells,
        statuses=final_statuses,
        write_quality=quality,
        host=host,
    )
    if run_error is not None:
        summary["run_error"] = {
            "type": type(run_error).__name__,
            "message": str(run_error),
        }
        if not measured_run_error:
            summary["status"] = "infrastructure_error"
            exit_code = 3
    _atomic_json(shard_dir / "state.json", {
        "attempt_id": attempt_id,
        "status": summary["status"],
        "terminal_counts": summary["terminal_counts"],
    })
    _atomic_json(shard_dir / "shard-summary.json", summary)
    _atomic_json(
        history / f"{attempt_id}.state.json",
        {
            "attempt_id": attempt_id,
            "host": host,
            "status": summary["status"],
            "exit_code": exit_code,
        },
    )
    return summary, exit_code


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", choices=ENGINES, required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument(
        "--dataset",
        choices=tuple(DATASET_ROWS),
        required=True,
    )
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--dataset-path", type=Path)
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args(argv)
    try:
        validate_storage(args.artifacts, smoke=False)
        runner = TinRunner(
            args.root.resolve(),
            args.artifacts.resolve(),
            False,
            args.run_id,
            args.dataset,
            args.dataset_path,
        )
        _, exit_code = execute_shard(
            runner,
            engine=args.engine,
            profile=args.profile,
            dataset=args.dataset,
            build=args.build,
        )
        return exit_code
    except (
        ConfigError,
        OSError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
    ) as error:
        print(f"INFRASTRUCTURE ERROR: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
