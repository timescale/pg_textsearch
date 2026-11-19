#!/usr/bin/env python3
"""Deterministic natural-query and controlled-span workload generation."""

import hashlib
import json
from pathlib import Path
import random
import re
from typing import Iterable, Sequence


class QueryError(RuntimeError):
    """A safe, actionable query preparation error."""


def _stable_rank(seed: int, query_id: str, text: str) -> str:
    value = f"{seed}\0{query_id}\0{text}".encode()
    return hashlib.sha256(value).hexdigest()


def load_queries(
    paths: Sequence[Path],
    *,
    source: str,
    seed: int,
    limit: int | None = None,
) -> list[dict[str, str]]:
    """Load two-column query TSV files and select a stable subset."""
    queries: list[dict[str, str]] = []
    seen: set[str] = set()
    for path in paths:
        with path.open(encoding="utf-8") as query_file:
            for line_number, line in enumerate(query_file, 1):
                fields = line.rstrip("\n").split("\t", 1)
                if len(fields) != 2 or not fields[0] or not fields[1].strip():
                    raise QueryError(
                        f"invalid query TSV record at "
                        f"{path.name}:{line_number}"
                    )
                query_id = f"{source}:{fields[0]}"
                if query_id in seen:
                    raise QueryError(
                        f"duplicate query identifier at "
                        f"{path.name}:{line_number}"
                    )
                seen.add(query_id)
                queries.append(
                    {
                        "id": query_id,
                        "text": fields[1].strip(),
                        "source": source,
                    }
                )
    queries.sort(
        key=lambda row: (
            _stable_rank(seed, row["id"], row["text"]),
            row["id"],
        )
    )
    if limit is not None:
        if limit <= 0:
            raise QueryError("query limit must be greater than zero")
        queries = queries[:limit]
    if not queries:
        raise QueryError("no non-empty queries were loaded")
    return queries


def _tokens(queries: Iterable[dict[str, str]]) -> list[str]:
    tokens: list[str] = []
    for query in queries:
        tokens.extend(re.findall(r"[^\W_]+(?:'[^\W_]+)?", query["text"].lower()))
    return tokens


def lead_disjunction(text: str) -> str:
    tokens = _tokens([{"text": text}])
    if not tokens:
        raise QueryError("Lead query has no searchable terms")
    # TinQL defaults to AND; quoted terms also escape its query operators.
    return " OR ".join(f'"{token}"' for token in tokens)


def _interleave_spans(
    queries: list[dict[str, object]], per_span: int
) -> list[dict[str, object]]:
    return [
        query
        for sample in range(per_span)
        for query in queries[sample::per_span]
    ]


def build_span_queries(
    natural: Sequence[dict[str, str]],
    *,
    spans: Iterable[int] = range(2, 16),
    per_span: int,
    seed: int,
) -> list[dict[str, object]]:
    """Build deterministic disjunction queries with exact term counts."""
    if per_span <= 0:
        raise QueryError("per_span must be greater than zero")
    token_pool = _tokens(natural)
    if not token_pool:
        raise QueryError("natural query pool contains no searchable terms")

    rng = random.Random(seed)
    offset = rng.randrange(len(token_pool))
    result: list[dict[str, object]] = []
    for span in spans:
        if span < 2 or span > 15:
            raise QueryError("span values must be between 2 and 15")
        for number in range(per_span):
            start = offset + (span * per_span) + (number * span)
            terms = [
                token_pool[(start + index) % len(token_pool)]
                for index in range(span)
            ]
            result.append(
                {
                    "id": f"span{span:02d}-{number:04d}",
                    "text": " ".join(terms),
                    "source": "deterministic-span",
                    "span": span,
                }
            )
    return _interleave_spans(result, per_span)


def build_corpus_span_queries(
    documents: Sequence[dict[str, str]],
    *,
    spans: Iterable[int] = range(2, 16),
    per_span: int,
    seed: int,
) -> list[dict[str, object]]:
    """Sample consecutive token spans without crossing document boundaries."""
    candidates = []
    for document in documents:
        tokens = _tokens([{"text": document["body"]}])
        if tokens:
            candidates.append((document["source_id"], tokens))
    if not candidates:
        raise QueryError("corpus sample contains no searchable terms")
    result = []
    for span in spans:
        eligible = [(doc_id, tokens) for doc_id, tokens in candidates
                    if len(tokens) >= span]
        if not eligible:
            raise QueryError(f"no sampled document supports span {span}")
        for number in range(per_span):
            digest = hashlib.sha256(
                f"{seed}\0{span}\0{number}".encode()
            ).digest()
            doc_id, tokens = eligible[
                int.from_bytes(digest[:8], "big") % len(eligible)
            ]
            offset = int.from_bytes(digest[8:16], "big") % (
                len(tokens) - span + 1
            )
            result.append({
                "id": f"span{span:02d}-{number:04d}",
                "text": " ".join(tokens[offset:offset + span]),
                "source": "corpus-span",
                "source_doc_id": doc_id,
                "span": span,
                "offset": offset,
                "seed": seed,
            })
    return _interleave_spans(result, per_span)


def sample_parquet_spans(
    data_path: Path, *, spans: Iterable[int], per_span: int, seed: int
) -> list[dict[str, object]]:
    import pyarrow as pa
    import pyarrow.dataset as pads

    dataset = pads.dataset(str(data_path), format="parquet")
    count = dataset.count_rows()
    needed = max(1000, per_span * 20)
    indices = sorted({
        int.from_bytes(
            hashlib.sha256(f"{seed}\0{number}".encode()).digest()[:8],
            "big",
        ) % count
        for number in range(needed)
    })
    table = dataset.take(pa.array(indices, type=pa.int64()))
    return build_corpus_span_queries(
        table.select(["source_id", "body"]).to_pylist(),
        spans=spans,
        per_span=per_span,
        seed=seed,
    )


def build_workload_matrix() -> list[dict[str, object]]:
    """Describe comparable cells without fabricating unsupported semantics."""
    matrix: list[dict[str, object]] = []
    engines = ("pg_textsearch", "paradedb", "lead")
    for engine in engines:
        for limit in (10, 100):
            matrix.append(
                {
                    "engine": engine,
                    "mode": "disjunction",
                    "limit": limit,
                    "support": (
                        "compatibility-only" if engine == "lead" else "exact"
                    ),
                    "scale": 10_000 if engine == "lead" else "full",
                }
            )

    for mode in ("phrase", "conjunction", "count"):
        matrix.append(
            {
                "engine": "pg_textsearch",
                "mode": mode,
                "limit": None,
                "support": "exact",
            }
        )
    for engine in ("paradedb", "lead"):
        for mode in ("phrase", "conjunction", "count"):
            matrix.append(
                {
                    "engine": engine,
                    "mode": mode,
                    "limit": None,
                    "support": (
                        "compatibility-only" if engine == "lead" else "exact"
                    ),
                    "scale": 10_000 if engine == "lead" else "full",
                }
            )
    for engine in engines:
        matrix.append(
            {
                "engine": engine,
                "mode": "mixed-updates",
                "limit": 10,
                "support": (
                    "unsupported" if engine == "lead" else "exact"
                ),
                **(
                    {
                        "reason": "Lead stores no search index data, so update "
                        "cost is not performance-comparable"
                    }
                    if engine == "lead"
                    else {}
                ),
            }
        )
    return matrix


def write_query_pack(
    output: Path,
    natural: Sequence[dict[str, str]],
    *,
    spans: Iterable[int],
    per_span: int,
    seed: int,
) -> None:
    """Write deterministic JSON inputs consumed directly by k6 scripts."""
    output.mkdir(parents=True, exist_ok=True)
    span_queries = build_span_queries(
        natural, spans=spans, per_span=per_span, seed=seed
    )
    files = {
        "natural.json": list(natural),
        "spans.json": span_queries,
        "matrix.json": build_workload_matrix(),
    }
    for name, value in files.items():
        path = output / name
        temporary = path.with_suffix(path.suffix + ".partial")
        temporary.write_text(
            json.dumps(value, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)
