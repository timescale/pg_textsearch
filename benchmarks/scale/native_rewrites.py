"""Exact SQL alternatives for separately labeled native benchmark lanes."""

import base64
import json
import subprocess


KINDS = ("disjunction", "conjunction", "phrase")
SHAPES = ("original", "rank_first", "filter_first")


class RewriteError(RuntimeError):
    pass


def _literal(value):
    return "'" + value.replace("'", "''") + "'"


def enrich_queries(container, text_config, queries):
    """Compute Boolean query trees with the index's actual PostgreSQL analyzer."""
    config = _literal(text_config) + "::regconfig"
    records = []
    for ordinal, query in enumerate(queries):
        if not isinstance(query.get("text"), str):
            raise RewriteError("query text must be a string")
        # Base64 keeps COPY's end marker and embedded newlines out of its data.
        encoded = base64.b64encode(query["text"].encode("utf-8")).decode("ascii")
        records.append(f"{ordinal}\t{encoded}\n")
    if not records:
        return []
    script = (
        "SET standard_conforming_strings = on;\n"
        "SET client_min_messages = error;\n"
        "CREATE TEMP TABLE rewrite_inputs (ordinal integer, encoded text);\n"
        "COPY rewrite_inputs FROM STDIN;\n"
        + "".join(records)
        + "\\.\n"
        "WITH inputs AS (\n"
        " SELECT ordinal, convert_from(decode(encoded,'base64'),'UTF8') AS body\n"
        " FROM rewrite_inputs\n"
        ")\n"
        "SELECT json_build_object(\n"
        " 'ordinal',ordinal,\n"
        " 'disjunction',COALESCE((SELECT\n"
        "   string_agg(quote_literal(lexeme),' | ' ORDER BY lexeme)"
        "::tsquery::text\n"
        "   FROM unnest(tsvector_to_array(to_tsvector("
        + config + ",body))) AS lexeme),''),\n"
        " 'conjunction',plainto_tsquery(" + config + ",body)::text,\n"
        " 'phrase',phraseto_tsquery(" + config + ",body)::text\n"
        ") FROM inputs ORDER BY ordinal;\n"
    )
    result = subprocess.run(
        [
            "docker", "exec", "-i", container, "psql", "-U", "postgres",
            "-XqAt", "-v", "ON_ERROR_STOP=1",
        ],
        input=script, capture_output=True, text=True,
    )
    if result.returncode:
        raise RewriteError(
            f"PostgreSQL query preparation failed: {result.stderr.strip()}"
        )
    try:
        rows = [json.loads(line) for line in result.stdout.splitlines()]
    except json.JSONDecodeError as error:
        raise RewriteError("invalid PostgreSQL query preparation output") from error
    if (
        len(rows) != len(queries)
        or [row.get("ordinal") for row in rows] != list(range(len(queries)))
        or any(
            not isinstance(row.get(kind), str)
            for row in rows for kind in KINDS
        )
    ):
        raise RewriteError("incomplete or inconsistent prepared query pack")
    return [
        query | {"pg_tsquery": {kind: row[kind] for kind in KINDS}}
        for query, row in zip(queries, rows)
    ]


def query_sql(kind, operation, shape, limit):
    if kind not in KINDS or shape not in SHAPES:
        raise RewriteError("unknown query kind or SQL shape")
    if operation == "count":
        return (
            "SELECT count(*) FROM documents "
            "WHERE numnode($1::tsquery) > 0 AND body @@ $1::tsquery"
        )
    if operation != "search":
        raise RewriteError("unknown query operation")
    if not isinstance(limit, int) or isinstance(limit, bool) or limit <= 0:
        raise RewriteError("search limit must be a positive integer")
    score = "body <@> to_bm25query($1, 'documents_bm25')"
    if kind == "disjunction":
        return f"SELECT id FROM documents ORDER BY {score} LIMIT {limit}"
    if shape == "original":
        return (
            "SELECT id FROM documents WHERE body @@ $2::tsquery "
            f"ORDER BY {score} LIMIT {limit}"
        )
    if shape == "filter_first":
        return (
            "WITH candidates AS MATERIALIZED ("
            "SELECT id, body FROM documents "
            "WHERE numnode($2::tsquery) > 0 AND body @@ $2::tsquery"
            ") SELECT id FROM candidates "
            f"ORDER BY {score} LIMIT {limit}"
        )
    return (
        "SELECT id FROM ("
        f"SELECT id, body, {score} AS score FROM documents "
        f"ORDER BY {score} OFFSET 0"
        ") ranked WHERE numnode($2::tsquery) > 0 AND body @@ $2::tsquery "
        f"ORDER BY score LIMIT {limit}"
    )
