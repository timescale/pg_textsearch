"""Read-only diagnostic replay on existing native indexes."""

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import time

from benchmarks.scale.native_rewrites import KINDS, query_sql


def select_queries(queries, ordinals=None):
    if not isinstance(queries, list) or not queries:
        raise ValueError("a nonempty prepared query pack is required")
    selected = []
    seen = set()
    for raw in ordinals if ordinals is not None else (0, -1):
        ordinal = raw if raw >= 0 else len(queries) + raw
        if not 0 <= ordinal < len(queries):
            raise ValueError(f"query ordinal out of range: {raw}")
        if ordinal not in seen:
            selected.append((ordinal, queries[ordinal]))
            seen.add(ordinal)
    return selected


def prepared_statement(operation, limit):
    # All Boolean kinds use the same SQL; their tsquery selects the semantics.
    statement = query_sql("conjunction", operation, "original", limit)
    first = "tsquery" if operation == "count" else "text"
    return statement.replace("$1", f"%({first})s").replace(
        "$2", "%(tsquery)s"
    )


def validate_ranked_plan(explanation, empty=False):
    nodes = []

    def visit(node):
        nodes.append(node)
        for child in node.get("Plans", []):
            visit(child)

    for record in explanation:
        visit(record["Plan"])
    if any(node["Node Type"] in (
        "Sort", "Incremental Sort", "CTE Scan", "Subquery Scan"
    ) for node in nodes):
        return False
    if empty and all("Scan" not in node["Node Type"] for node in nodes):
        return True
    return any(
        node["Node Type"] == "Index Scan"
        and node.get("Index Name") == "documents_bm25"
        and node.get("Index Cond") is not None
        and node.get("Order By") is not None
        for node in nodes
    )


def replay_case(connection, query, kind, operation, limit, repeats):
    if min(limit, repeats) <= 0:
        raise ValueError("limit and repeats must be positive")
    text = query["text"]
    tsquery = query["pg_tsquery"][kind]
    if not isinstance(text, str) or not isinstance(tsquery, str):
        raise ValueError("prepared text and tsquery must be strings")
    statement = prepared_statement(operation, limit)
    parameters = {"text": text, "tsquery": tsquery}
    start = time.perf_counter()
    explanation = connection.execute(
        "EXPLAIN (ANALYZE, BUFFERS, SETTINGS, FORMAT JSON) " + statement,
        parameters,
    ).fetchone()[0]
    explained_wall_ms = (time.perf_counter() - start) * 1000
    verified = operation != "search" or validate_ranked_plan(
        explanation, empty=not tsquery
    )
    if not verified:
        raise RuntimeError(
            f"query {query['id']} lacks ranked Boolean pushdown"
        )
    samples = []
    rows = []
    for _ in range(repeats):
        start = time.perf_counter()
        rows = connection.execute(statement, parameters).fetchall()
        samples.append((time.perf_counter() - start) * 1000)
        if operation == "search" and rows:
            ids = [row[0] for row in rows]
            if len(set(ids)) != len(ids):
                raise RuntimeError("ranked scan returned duplicate IDs")
            invalid, found = connection.execute("""
                SELECT count(*) FILTER (
                    WHERE (to_tsvector(
                        current_setting('default_text_search_config')
                            ::regconfig,
                        body
                    ) @@ %(tsquery)s::tsquery) IS NOT TRUE
                ), count(*)
                FROM documents WHERE id = ANY(%(ids)s)
            """, {"tsquery": tsquery, "ids": ids}).fetchone()
            if invalid or found != len(ids):
                raise RuntimeError(
                    "ranked rows failed independent text-search validation"
                )
    return {
        "status": "ok", "sql": statement, "parameters": parameters,
        "explain": explanation, "explained_wall_ms": explained_wall_ms,
        "execution_ms": explanation[0]["Execution Time"],
        "warm_wall_ms": samples, "rows": [list(row) for row in rows],
        "ranked_plan_verified": verified if operation == "search" else None,
        "row_filter_verified": True if operation == "search" else None,
    }


def main(argv=None):
    import psycopg

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--ordinal", type=int, action="append")
    selection.add_argument("--all", action="store_true")
    parser.add_argument("--kind", choices=KINDS, action="append")
    parser.add_argument(
        "--operation", choices=("search", "count"), default="search"
    )
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--dsn", default="")
    parser.add_argument("--build-sha")
    parser.add_argument("--effective-cache-size")
    args = parser.parse_args(argv)
    if min(args.limit, args.repeats, args.timeout) <= 0:
        parser.error("limit, repeats, and timeout must be positive")
    content = args.queries.read_bytes()
    queries = json.loads(content)
    selected = select_queries(
        queries, list(range(len(queries))) if args.all else args.ordinal
    )
    kinds = list(dict.fromkeys(args.kind or KINDS))
    completed = cancelled = 0
    with args.output.open("x") as output, psycopg.connect(
        args.dsn, autocommit=True, prepare_threshold=None
    ) as connection:
        connection.execute("SET default_transaction_read_only=on")
        connection.execute("SET plan_cache_mode=force_custom_plan")
        connection.execute(
            "SELECT set_config('statement_timeout', %s, false)",
            (f"{args.timeout}s",),
        )
        if args.effective_cache_size:
            connection.execute(
                "SELECT set_config('effective_cache_size', %s, false)",
                (args.effective_cache_size,),
            )
        settings = dict(connection.execute("""
            SELECT name, setting FROM pg_settings WHERE name IN (
                'server_version', 'shared_buffers', 'work_mem', 'jit',
                'random_page_cost', 'effective_cache_size', 'plan_cache_mode',
                'default_text_search_config', 'pg_textsearch.default_limit',
                'pg_textsearch.filtered_seed', 'max_parallel_workers',
                'max_parallel_workers_per_gather'
            )
        """).fetchall())
        header = {
            "type": "metadata", "scope": "single-client read-only diagnostic",
            "recorded_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "query_pack": str(args.queries),
            "query_pack_sha256": hashlib.sha256(content).hexdigest(),
            "provided_build_sha": args.build_sha, "settings": settings,
            "ordinals": [ordinal for ordinal, _ in selected],
            "kinds": kinds, "operation": args.operation,
            "limit": args.limit, "repeats": args.repeats,
            "timeout_seconds": args.timeout,
        }
        output.write(json.dumps(header) + "\n")
        output.flush()
        for ordinal, query in selected:
            for kind in kinds:
                row = {
                    "type": "query", "ordinal": ordinal,
                    "query_id": query["id"],
                    "kind": kind, "operation": args.operation,
                }
                start = time.perf_counter()
                try:
                    row.update(replay_case(
                        connection, query, kind, args.operation,
                        args.limit, args.repeats,
                    ))
                except psycopg.errors.QueryCanceled as error:
                    row.update(
                        status="cancelled", sqlstate=error.sqlstate,
                        error=str(error),
                        elapsed_ms=(time.perf_counter() - start) * 1000,
                    )
                    cancelled += 1
                except (psycopg.Error, RuntimeError, ValueError) as error:
                    row.update(status="error", error=str(error))
                    output.write(json.dumps(row) + "\n")
                    output.flush()
                    raise
                else:
                    completed += 1
                output.write(json.dumps(row) + "\n")
                output.flush()
                print(f"{query['id']} {kind}: {row['status']}", flush=True)
    print(json.dumps({"completed": completed, "cancelled": cancelled}))


if __name__ == "__main__":
    main()
