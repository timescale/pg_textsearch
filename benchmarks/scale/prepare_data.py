#!/usr/bin/env python3
"""Prepare private MS MARCO inputs without exposing corpus text."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Mapping, Sequence

from benchmarks.scale.dataset import (
    DatasetError,
    MSMARCO_V2_PASSAGE_MD5,
    MSMARCO_V2_PASSAGE_ROWS,
    prepare_parquet,
    prepare_synthetic,
    require_terms_acceptance,
)
from benchmarks.scale.queries import (
    QueryError,
    load_queries,
    sample_parquet_spans,
    write_query_pack,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Prepare reproducible MS MARCO v2 benchmark inputs"
    )
    commands = parser.add_subparsers(dest="command", required=True)

    acceptance = commands.add_parser(
        "acceptance",
        help="gate any dataset download on explicit terms acceptance",
    )
    acceptance.add_argument("--accept-msmarco-terms", action="store_true")

    synthetic = commands.add_parser(
        "synthetic",
        help="create a clearly labeled pipeline-only smoke corpus",
    )
    synthetic.add_argument("--output", required=True, type=Path)
    synthetic.add_argument("--rows", type=int, default=10_000)
    synthetic.add_argument("--seed", type=int, default=20260304)

    corpus = commands.add_parser(
        "corpus", help="convert the full passage archive to Parquet shards"
    )
    corpus.add_argument("--archive", required=True, type=Path)
    corpus.add_argument("--output", required=True, type=Path)
    corpus.add_argument(
        "--expected-md5", default=MSMARCO_V2_PASSAGE_MD5
    )
    corpus.add_argument(
        "--expected-rows", type=int, default=MSMARCO_V2_PASSAGE_ROWS
    )
    corpus.add_argument("--batch-rows", type=int, default=50_000)
    corpus.add_argument("--accept-msmarco-terms", action="store_true")

    queries = commands.add_parser(
        "queries", help="create deterministic natural and span query packs"
    )
    queries.add_argument(
        "--query",
        action="append",
        required=True,
        metavar="SOURCE=PATH",
        help="two-column query TSV; repeat for dev and TREC sources",
    )
    queries.add_argument("--output", required=True, type=Path)
    queries.add_argument("--seed", type=int, default=20260304)
    queries.add_argument(
        "--natural-limit",
        type=int,
        default=0,
        help="stable total subset size; 0 keeps all queries",
    )
    queries.add_argument("--spans", default="2-15")
    queries.add_argument("--per-span", type=int, default=100)
    queries.add_argument(
        "--corpus-parquet",
        type=Path,
        help="Parquet file/directory used for boundary-preserving spans",
    )
    queries.add_argument("--accept-msmarco-terms", action="store_true")
    return parser


def _query_specs(values: Sequence[str]) -> list[tuple[str, Path]]:
    specs: list[tuple[str, Path]] = []
    for value in values:
        if "=" not in value:
            raise QueryError("--query must use SOURCE=PATH")
        source, raw_path = value.split("=", 1)
        if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", source):
            raise QueryError(
                "query source must contain lowercase letters, digits, "
                "underscores, or hyphens"
            )
        if not raw_path:
            raise QueryError("--query path must not be empty")
        specs.append((source, Path(raw_path)))
    return specs


def _spans(value: str) -> list[int]:
    spans: list[int] = []
    for part in value.split(","):
        if "-" in part:
            start_text, end_text = part.split("-", 1)
            try:
                start, end = int(start_text), int(end_text)
            except ValueError as error:
                raise QueryError("spans must be integers or ranges") from error
            spans.extend(range(start, end + 1))
        else:
            try:
                spans.append(int(part))
            except ValueError as error:
                raise QueryError("spans must be integers or ranges") from error
    if not spans:
        raise QueryError("at least one span is required")
    return spans


def _select_natural(
    queries: list[dict[str, str]], seed: int, limit: int
) -> list[dict[str, str]]:
    if limit < 0:
        raise QueryError("natural limit must not be negative")
    queries.sort(
        key=lambda row: hashlib.sha256(
            f"{seed}\0{row['id']}\0{row['text']}".encode()
        ).hexdigest()
    )
    return queries if limit == 0 else queries[:limit]


def main(
    arguments: Sequence[str] | None = None,
    *,
    environment: Mapping[str, str] | None = None,
) -> int:
    args = _parser().parse_args(arguments)
    environment = os.environ if environment is None else environment
    try:
        if args.command == "synthetic":
            manifest = prepare_synthetic(
                args.output, rows=args.rows, seed=args.seed
            )
            synthetic_queries = [
                {
                    "id": f"synthetic:{number:02d}",
                    "text": text,
                    "source": "synthetic-smoke",
                }
                for number, text in enumerate(
                    (
                        "search index benchmark",
                        "document ranking query",
                        "cache memory latency",
                        "segment storage engine",
                        "update query workload",
                        "exact result ranking",
                        "token language search",
                        "telemetry metric system",
                    ),
                    1,
                )
            ]
            write_query_pack(
                args.output / "k6",
                synthetic_queries,
                spans=range(2, 16),
                per_span=10,
                seed=args.seed,
            )
            corpus_spans = sample_parquet_spans(
                args.output / "data",
                spans=range(2, 16),
                per_span=10,
                seed=args.seed,
            )
            (args.output / "k6" / "spans.json").write_text(
                json.dumps(corpus_spans, indent=2) + "\n"
            )
            print(
                f"prepared {manifest['rows']} synthetic smoke rows; "
                "not comparable to full scale"
            )
            return 0
        require_terms_acceptance(
            args.accept_msmarco_terms, environment
        )
        if args.command == "acceptance":
            print("MS MARCO terms acceptance recorded for this invocation")
            return 0
        if args.command == "corpus":
            manifest = prepare_parquet(
                args.archive,
                args.output,
                expected_md5=args.expected_md5,
                expected_rows=args.expected_rows,
                batch_rows=args.batch_rows,
            )
            print(
                f"prepared {manifest['rows']} rows in "
                f"{len(manifest['members'])} Parquet shards"
            )
            return 0

        natural: list[dict[str, str]] = []
        for source, path in _query_specs(args.query):
            natural.extend(
                load_queries([path], source=source, seed=args.seed)
            )
        natural = _select_natural(
            natural, args.seed, args.natural_limit
        )
        write_query_pack(
            args.output,
            natural,
            spans=_spans(args.spans),
            per_span=args.per_span,
            seed=args.seed,
        )
        if args.corpus_parquet:
            spans = sample_parquet_spans(
                args.corpus_parquet,
                spans=_spans(args.spans),
                per_span=args.per_span,
                seed=args.seed,
            )
            (args.output / "spans.json").write_text(
                json.dumps(spans, indent=2, ensure_ascii=False) + "\n"
            )
        print(f"prepared {len(natural)} natural queries")
        return 0
    except (DatasetError, QueryError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
