"""Warm sequential diagnostics on existing native/ParadeDB indexes."""

import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import statistics
import time

from benchmarks.scale.replay_queries import replay_case, select_queries


ENGINES = ("pg_textsearch", "paradedb")
KINDS = ("disjunction", "conjunction")


def sample_ordinals(length, samples):
    if min(length, samples) <= 0:
        raise ValueError("pack length and sample count must be positive")
    count = min(length, samples)
    if count == 1:
        return [0]
    return [
        (i * (length - 1) + (count - 1) // 2) // (count - 1)
        for i in range(count)
    ]


def parade_statement(kind, operation, limit):
    if (
        kind not in KINDS or operation not in ("search", "count")
        or type(limit) is not int or limit <= 0
    ):
        raise ValueError("invalid ParadeDB query kind, operation, or limit")
    operator = {"conjunction": "&&&", "disjunction": "|||"}[kind]
    projection = "id" if operation == "search" else "count(*)"
    statement = (
        f"SELECT {projection} FROM documents WHERE body {operator} %(text)s"
    )
    if operation == "search":
        statement += f" ORDER BY paradedb.score(id) DESC LIMIT {limit}"
    return statement


def parade_case(connection, query, kind, operation, limit, repeats):
    statement = parade_statement(kind, operation, limit)
    parameters = {"text": query["text"]}
    explanation = connection.execute(
        "EXPLAIN (ANALYZE, BUFFERS, SETTINGS, TIMING OFF, FORMAT JSON) "
        + statement, parameters,
    ).fetchone()[0]
    samples = []
    for _ in range(repeats):
        start = time.perf_counter()
        rows = connection.execute(statement, parameters).fetchall()
        samples.append((time.perf_counter() - start) * 1000)
        if (
            operation == "search"
            and len({row[0] for row in rows}) != len(rows)
        ):
            raise RuntimeError("ParadeDB returned duplicate ranked IDs")
    return {
        "status": "ok", "sql": statement, "parameters": parameters,
        "explain": explanation, "warm_wall_ms": samples,
        "rows": [list(row) for row in rows],
    }


def database_metadata(connection, engine, timeout, effective_cache_size):
    connection.execute("SET default_transaction_read_only=on")
    connection.execute("SET plan_cache_mode=force_custom_plan")
    connection.execute(
        "SELECT set_config('statement_timeout', %s, false)", (f"{timeout}s",)
    )
    if effective_cache_size:
        connection.execute(
            "SELECT set_config('effective_cache_size', %s, false)",
            (effective_cache_size,),
        )
    index = connection.execute("""
        SELECT oid, pg_relation_filenode(oid), pg_relation_size(oid),
               pg_get_indexdef(oid), reloptions
        FROM pg_class WHERE oid='documents_bm25'::regclass
    """).fetchone()
    if engine == "pg_textsearch":
        options = dict(option.split("=", 1) for option in index[4] or [])
        if "text_config" not in options:
            raise ValueError("native index lacks an explicit text_config")
        connection.execute(
            "SELECT set_config('default_text_search_config', %s, false)",
            (options["text_config"],),
        )
    settings = dict(connection.execute("""
        SELECT name, setting FROM pg_settings WHERE name IN (
            'server_version', 'shared_buffers', 'work_mem', 'jit',
            'random_page_cost', 'effective_cache_size', 'plan_cache_mode',
            'default_text_search_config', 'max_parallel_workers',
            'max_parallel_workers_per_gather', 'pg_textsearch.default_limit',
            'pg_textsearch.filtered_seed', 'pg_textsearch.filtered_seed_margin'
        )
    """).fetchall())
    version = connection.execute(
        "SELECT extversion FROM pg_extension WHERE extname=%s",
        ("pg_textsearch" if engine == "pg_textsearch" else "pg_search",),
    ).fetchone()
    if version is None:
        raise ValueError(f"{engine} extension is not installed")
    return {
        "type": "database", "engine": engine, "index": list(index[:4]),
        "index_options": index[4], "settings": settings,
        "extension_version": version[0],
    }


def _validated_run(records):
    if (
        not records or records[0].get("type") != "metadata"
        or records[-1].get("type") != "complete"
    ):
        raise ValueError("incomplete diagnostic output")
    header = records[0]
    if not header["ordinals"] or not header["kinds"]:
        raise ValueError("diagnostic workload must not be empty")
    databases = [
        row for row in records[1:-1] if row.get("type") == "database"
    ]
    if (
        len(databases) != len(ENGINES)
        or {row.get("engine") for row in databases} != set(ENGINES)
        or any(
            any(name not in row for name in (
                "index", "index_options", "settings", "extension_version",
            ))
            for row in databases
        )
    ):
        raise ValueError("missing, duplicate, or incomplete database provenance")
    queries = {}
    for row in records[1:-1]:
        if row.get("type") != "query":
            continue
        key = (row["engine"], row["ordinal"], row["kind"], row["operation"])
        if key in queries:
            raise ValueError("duplicate diagnostic query")
        if row["status"] not in ("ok", "cancelled"):
            raise ValueError("invalid diagnostic query status")
        if row["status"] == "ok":
            value = row["median_ms"]
            if type(value) not in (int, float) or not (
                math.isfinite(value) and value > 0
            ):
                raise ValueError("invalid diagnostic latency")
            if not isinstance(row.get("rows"), list):
                raise ValueError("missing diagnostic rows")
        queries[key] = row
    expected = {
        (engine, ordinal, kind, header["operation"])
        for engine in ENGINES for ordinal in header["ordinals"]
        for kind in header["kinds"]
    }
    if queries.keys() != expected:
        raise ValueError("diagnostic query population is incomplete")
    summary = records[-1]
    completed = sum(row["status"] == "ok" for row in queries.values())
    if (
        summary.get("completed") != completed
        or summary.get("cancelled") != len(queries) - completed
    ):
        raise ValueError("diagnostic completion counts do not match")
    return header, queries


def analyze_runs(before, after):
    before_header, baseline = _validated_run(before)
    after_header, candidate = _validated_run(after)
    for name in (
        "query_pack_sha256", "ordinals", "kinds",
        "operation", "limit", "repeats",
    ):
        if before_header[name] != after_header[name]:
            raise ValueError(f"diagnostic workload changed: {name}")
    before_databases = {
        row["engine"]: row for row in before if row.get("type") == "database"
    }
    after_databases = {
        row["engine"]: row for row in after if row.get("type") == "database"
    }
    if before_databases.keys() != after_databases.keys():
        raise ValueError("diagnostic database population changed")
    for engine, database in before_databases.items():
        for name in ("index", "index_options", "settings"):
            if database[name] != after_databases[engine][name]:
                raise ValueError(f"{engine} provenance changed: {name}")
    differences = []
    ratios = {engine: [] for engine in ENGINES}
    unsuccessful = 0
    for key, previous in baseline.items():
        current = candidate[key]
        if previous["query_id"] != current["query_id"]:
            raise ValueError("diagnostic query identity changed")
        if previous["status"] != "ok" or current["status"] != "ok":
            unsuccessful += 1
            continue
        ratios[key[0]].append(current["median_ms"] / previous["median_ms"])
        if previous["rows"] != current["rows"]:
            differences.append({
                "engine": key[0], "ordinal": key[1], "kind": key[2],
                "query_id": previous["query_id"],
                "before": previous["rows"], "after": current["rows"],
            })
    cross_ratios = []
    count_differences = []
    for key, native in candidate.items():
        if key[0] != "pg_textsearch":
            continue
        parade = candidate[("paradedb", *key[1:])]
        if native["status"] != "ok" or parade["status"] != "ok":
            continue
        if key[3] == "count" and native["rows"] != parade["rows"]:
            count_differences.append({
                "ordinal": key[1], "kind": key[2],
                "query_id": native["query_id"],
                "native": native["rows"], "parade": parade["rows"],
            })
            continue
        cross_ratios.append(native["median_ms"] / parade["median_ms"])
    return {
        "scope": "paired warm sequential diagnostic, not concurrent QPS",
        "unsuccessful": unsuccessful,
        "native_rows_changed": sum(
            row["engine"] == "pg_textsearch" for row in differences
        ),
        "parade_rows_changed": sum(
            row["engine"] == "paradedb" for row in differences
        ),
        "differences": differences,
        "cross_engine_count_differences": count_differences,
        "candidate_native_vs_parade": {
            "paired_successes": len(cross_ratios),
            "native_faster": sum(value < 1 for value in cross_ratios),
            "median_native_over_parade": (
                statistics.median(cross_ratios) if cross_ratios else None
            ),
        },
        "engines": {
            engine: {
                "paired_successes": len(values),
                "median_after_before": (
                    statistics.median(values) if values else None
                ),
                "geomean_after_before": (
                    math.exp(statistics.mean(map(math.log, values)))
                    if values else None
                ),
            }
            for engine, values in ratios.items()
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="read-only paired query replay")
    run.add_argument("--queries", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--native-dsn", required=True)
    run.add_argument("--parade-dsn", required=True)
    run.add_argument("--provided-native-build-sha")
    run.add_argument("--effective-cache-size")
    selection = run.add_mutually_exclusive_group()
    selection.add_argument("--samples", type=int, default=None)
    selection.add_argument("--ordinal", type=int, action="append")
    selection.add_argument("--all", action="store_true")
    run.add_argument("--kind", choices=KINDS, action="append")
    run.add_argument(
        "--operation", choices=("search", "count"), default="search"
    )
    run.add_argument("--limit", type=int, default=10)
    run.add_argument("--repeats", type=int, default=3)
    run.add_argument("--timeout", type=int, default=60)
    analysis = commands.add_parser(
        "analyze", help="compare baseline/candidate runs"
    )
    analysis.add_argument("--before", type=Path, required=True)
    analysis.add_argument("--after", type=Path, required=True)
    analysis.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "analyze":
        report = analyze_runs(*[
            [json.loads(line) for line in path.read_text().splitlines()]
            for path in (args.before, args.after)
        ])
        with args.output.open("x") as output:
            output.write(json.dumps(report, indent=2) + "\n")
        return 1 if (
            report["native_rows_changed"] or report["unsuccessful"]
            or report["cross_engine_count_differences"]
        ) else 0
    if min(args.limit, args.repeats, args.timeout) <= 0:
        parser.error("limit, repeats, and timeout must be positive")
    import psycopg

    content = args.queries.read_bytes()
    queries = json.loads(content)
    ordinals = (
        range(len(queries)) if args.all else
        sample_ordinals(len(queries), args.samples) if args.samples is not None
        else args.ordinal
    )
    selected = select_queries(queries, ordinals)
    kinds = list(dict.fromkeys(args.kind or KINDS))
    for _, query in selected:
        if not isinstance(query.get("text"), str) or any(
            not isinstance(query.get("pg_tsquery", {}).get(kind), str)
            for kind in kinds
        ):
            raise ValueError("a prepared text/pg_tsquery pack is required")
    completed = cancelled = 0
    with (
        args.output.open("x") as output,
        psycopg.connect(args.native_dsn, autocommit=True,
                        prepare_threshold=None) as native,
        psycopg.connect(args.parade_dsn, autocommit=True,
                        prepare_threshold=None) as parade,
    ):
        def emit(row):
            output.write(json.dumps(row) + "\n")
            output.flush()

        emit({
            "type": "metadata", "format": 1,
            "scope": "paired warm sequential diagnostic; no cache eviction",
            "recorded_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "query_pack_sha256": hashlib.sha256(content).hexdigest(),
            "provided_native_build_sha": args.provided_native_build_sha,
            "ordinals": [ordinal for ordinal, _ in selected], "kinds": kinds,
            "operation": args.operation, "limit": args.limit,
            "repeats": args.repeats, "timeout_seconds": args.timeout,
        })
        connections = {"pg_textsearch": native, "paradedb": parade}
        metadata = {}
        for engine, connection in connections.items():
            metadata[engine] = database_metadata(
                connection, engine, args.timeout, args.effective_cache_size
            )
            emit(metadata[engine])
        for ordinal, query in selected:
            for kind in kinds:
                for engine in reversed(ENGINES):
                    row = {
                        "type": "query", "engine": engine, "ordinal": ordinal,
                        "query_id": query["id"], "kind": kind,
                        "operation": args.operation,
                    }
                    start = time.perf_counter()
                    try:
                        measure = (
                            replay_case if engine == "pg_textsearch"
                            else parade_case
                        )
                        row.update(measure(
                            connections[engine], query, kind, args.operation,
                            args.limit, args.repeats,
                        ))
                        row["median_ms"] = statistics.median(
                            row["warm_wall_ms"]
                        )
                    except psycopg.errors.QueryCanceled as error:
                        row.update(
                            status="cancelled", sqlstate=error.sqlstate,
                            elapsed_ms=(time.perf_counter() - start) * 1000,
                        )
                        cancelled += 1
                    except (psycopg.Error, RuntimeError, ValueError) as error:
                        emit(row | {"status": "error", "error": str(error)})
                        raise
                    else:
                        completed += 1
                    emit(row)
        for engine, connection in connections.items():
            current = database_metadata(
                connection, engine, args.timeout, args.effective_cache_size
            )
            if current != metadata[engine]:
                raise RuntimeError(
                    f"{engine} provenance changed during replay"
                )
        emit({
            "type": "complete", "completed": completed,
            "cancelled": cancelled,
        })
    return 2 if cancelled else 0


if __name__ == "__main__":
    raise SystemExit(main())
