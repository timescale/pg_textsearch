#!/usr/bin/env python3
"""Rebuild failed stopword audits without modifying frozen campaign sources."""

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import time


def validate_recovery(state, pending):
    if (
        state.get("status") != "infrastructure_error"
        or state.get("run_error", {}).get("type") != "JSONDecodeError"
        or not state.get("run_error", {}).get("message", "").startswith(
            "Expecting value:"
        )
    ):
        raise ValueError("recovery requires a recorded audit parse failure")
    if not pending:
        raise ValueError("recovery requires pending cells")
    if any(
        cell.get("analyzer") != "english-stopwords"
        or cell.get("pack") != "spans"
        or cell.get("operation") != "search"
        or cell.get("query_family")
        not in {"conjunction", "disjunction", "phrase"}
        for cell in pending
    ):
        raise ValueError("pending cells are not stopword span controls")
    if len(pending) != 3 or {
        cell["query_family"] for cell in pending
    } != {"conjunction", "disjunction", "phrase"}:
        raise ValueError("recovery requires exactly three control families")
    recorded = state.get("cells", [])
    pending_ids = {
        row["cell_id"] for row in recorded
        if row.get("status") in {"pending", "partial"}
    }
    if (
        state.get("expected_cells") != 28
        or state.get("terminal_cells") != 25
        or len(recorded) != 28
        or len({row.get("cell_id") for row in recorded}) != 28
        or pending_ids != {cell["cell_id"] for cell in pending}
    ):
        raise ValueError("controls do not match recorded pending cells")


def recover(args):
    from benchmarks.scale import tin
    from benchmarks.scale.run import file_sha256

    root = args.root.resolve()
    if Path(tin.__file__).resolve() != root / "benchmarks/scale/tin.py":
        raise ValueError("set PYTHONPATH to the frozen campaign root")
    if subprocess.check_output(["docker", "ps", "-q"], text=True).strip():
        raise ValueError("audit recovery requires an idle Docker host")
    runner = tin.TinRunner(
        root, args.artifacts.resolve(), False, args.run_id,
        args.dataset, None,
    )
    shard = (
        runner.tin_dir / "fleet/shards"
        / f"pg_textsearch-{args.profile}"
    )
    state = json.loads((shard / "shard-summary.json").read_text())
    manifest = json.loads((shard / "manifest.json").read_text())
    if (
        manifest["engine"] != "pg_textsearch"
        or manifest["profile"] != args.profile
        or manifest["dataset"] != args.dataset
        or manifest["run_id"] != args.run_id
    ):
        raise ValueError("recovery arguments do not match the shard")
    sources = {
        root / "benchmarks/scale/tin.py": manifest["tin_source_sha256"],
        root / "benchmarks/scale/benchmarker/k6/tin.js":
            manifest["driver_sha256"],
    }
    if any(file_sha256(path) != digest for path, digest in sources.items()):
        raise ValueError("frozen campaign sources have changed")
    plan = json.loads((runner.tin_dir / "plan.json").read_text())
    selected = [
        cell for cell in plan["selected_cells"]
        if cell["profile"] == args.profile
    ]
    if {cell["cell_id"] for cell in selected} != set(
        manifest["expected_cell_ids"]
    ):
        raise ValueError("shard plan does not match its original manifest")
    markers = (
        "complete.json", "failed.json", "plan-gate-failed.json",
        "plan-audit-failed.json",
    )
    pending = [
        cell for cell in selected
        if not any(
            (runner.tin_dir / "results/pg_textsearch"
             / cell["cell_id"] / marker).is_file()
            for marker in markers
        )
    ]
    validate_recovery(state, pending)
    parser_path = args.parser_source.resolve()
    spec = importlib.util.spec_from_file_location(
        "benchmarks.scale.audit_recovery_parser", parser_path
    )
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    analyzer = "english-stopwords"
    phase = f"{args.profile}.pg_textsearch.{analyzer}"
    phase_dir = (
        runner.tin_dir / "phases" / args.profile
        / "pg_textsearch" / analyzer
    )
    if not (phase_dir / "clone.json").is_file():
        raise ValueError("existing profile clone checkpoint is required")
    out = phase_dir / "audit-recovery" / str(time.time_ns())
    out.mkdir(parents=True)
    for directory in (phase_dir / "plan-audits").iterdir():
        if (
            (directory / "plans.jsonstream").is_file()
            and not (directory / "complete.json").is_file()
        ):
            shutil.copytree(directory, out / directory.name)
    proof = {
        "status": "running",
        "parser_source_sha256": file_sha256(parser_path),
        "frozen_sources": {str(p): h for p, h in sources.items()},
        "pending_cells": [cell["cell_id"] for cell in pending],
        "audits": [],
    }
    proof_path = out / "recovery.json"
    proof_path.write_text(json.dumps(proof, indent=2) + "\n")
    images, _ = runner._images()
    volume = runner._volume_name(
        "profile", "pg_textsearch", f"{analyzer}-{args.profile}"
    )
    runner._clone_volume(
        runner._volume_name("analyzer", "pg_textsearch", analyzer),
        volume,
        {
            "kind": "profile", "engine": "pg_textsearch",
            "analyzer": analyzer, "profile": args.profile,
        },
        phase_dir / "clone.json",
        image=images["pg_textsearch"],
        source_labels={
            "kind": "analyzer", "engine": "pg_textsearch",
            "analyzer": analyzer,
        },
    )
    name = "pgts-tin-audit-recovery"
    original_parser = tin.parse_plan_audit
    runner._start_database(
        name=name, volume=volume, image=images["pg_textsearch"],
        profile=args.profile, engine="pg_textsearch",
        output=out / "container-start.log",
    )
    try:
        # Only the offline EXPLAIN decoder changes, never the measured runner.
        tin.parse_plan_audit = parser.parse_plan_audit
        for cell in pending:
            kind = cell["query_family"]
            sql = tin.scenario_sql(
                "pg_textsearch", operation=cell["operation"],
                family=kind, limit=cell["limit"], shape=cell["sql_shape"],
            )
            audit = runner._audit_native_queries(
                container=name, phase_dir=phase_dir, phase=phase,
                profile=args.profile, analyzer=analyzer, pack=cell["pack"],
                operation=cell["operation"], kind=kind,
                shape=cell["sql_shape"], statement=sql[kind],
                query_pack=(
                    runner.tin_dir / "query-packs" / analyzer / "spans.json"
                ),
            )
            checkpoint = (
                phase_dir / "plan-audits" / audit["audit_key"]
                / "complete.json"
            )
            proof["audits"].append({
                "cell_id": cell["cell_id"],
                "summary": {
                    k: v for k, v in audit.items() if k != "records"
                },
                "checkpoint_sha256": file_sha256(checkpoint),
            })
            proof_path.write_text(json.dumps(proof, indent=2) + "\n")
    finally:
        tin.parse_plan_audit = original_parser
        runner._stop_database(name, out / "postgres.log")
    if any(file_sha256(path) != digest for path, digest in sources.items()):
        raise ValueError("frozen sources changed during recovery")
    proof["status"] = "complete"
    proof_path.write_text(json.dumps(proof, indent=2) + "\n")
    print(json.dumps({"proof": str(proof_path), "status": "complete"}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--dataset", default="msmarco-v2")
    parser.add_argument(
        "--profile", choices=("high", "constrained32", "constrained16"),
        required=True,
    )
    parser.add_argument("--parser-source", type=Path, required=True)
    recover(parser.parse_args())


if __name__ == "__main__":
    main()
