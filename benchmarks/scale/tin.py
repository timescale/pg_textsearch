#!/usr/bin/env python3
"""Run the independently resumable expanded TIN benchmark campaign."""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from benchmarks.scale.run import (
    BENCHMARKER_SHA,
    PG_TEXTSEARCH_SHA,
    ConfigError,
    Runner,
    file_sha256,
    validate_storage,
)


PARADEDB_TAG = "paradedb/paradedb:v0.27.0-pg18"
PARADEDB_IMAGE = (
    "paradedb/paradedb@sha256:"
    "37607fa963841e46b8a2461faf4fe3535dce9bb8019bc2dde7f207729d571ca3"
)
DATABASE_CPUS = "0-7"
CLIENT_CPUS = "24-31"
PLAN_CACHE_MODE = "force_custom_plan"
RANDOM_PAGE_COST = 1.1
COMMON_QUERY_SETTINGS = {
    "plan_cache_mode": PLAN_CACHE_MODE,
    "random_page_cost": RANDOM_PAGE_COST,
}
DATABASE_PROOF_PROTOCOL = "rowcount-sizes-v1"
BUILD_RESOURCES = {
    "cpus": DATABASE_CPUS,
    "memory": "192g",
    "memory_swap": "192g",
    "shm_size": "64g",
    "shared_buffers": "32GB",
    "effective_cache_size": "160GB",
    "maintenance_work_mem": "8GB",
    "max_connections": 128,
    "track_io_timing": True,
    "plan_cache_mode": PLAN_CACHE_MODE,
    "random_page_cost": RANDOM_PAGE_COST,
    "minimum_claimed": False,
}
PROFILES = {
    "high": {
        "cpus": DATABASE_CPUS,
        "memory": "192g",
        "memory_swap": "192g",
        "shm_size": "64g",
        "shared_buffers": "32GB",
        "effective_cache_size": "160GB",
        "maintenance_work_mem": "256MB",
        "max_connections": 128,
        "track_io_timing": True,
        "plan_cache_mode": PLAN_CACHE_MODE,
        "random_page_cost": RANDOM_PAGE_COST,
    },
    "constrained32": {
        "cpus": DATABASE_CPUS,
        "memory": "32g",
        "memory_swap": "32g",
        "shm_size": "64g",
        "shared_buffers": "24GB",
        "effective_cache_size": "28GB",
        "maintenance_work_mem": "256MB",
        "max_connections": 128,
        "track_io_timing": True,
        "plan_cache_mode": PLAN_CACHE_MODE,
        "random_page_cost": RANDOM_PAGE_COST,
    },
    "constrained16": {
        "cpus": DATABASE_CPUS,
        "memory": "16g",
        "memory_swap": "16g",
        "shm_size": "64g",
        "shared_buffers": "12GB",
        "effective_cache_size": "14GB",
        "maintenance_work_mem": "256MB",
        "max_connections": 128,
        "track_io_timing": True,
        "plan_cache_mode": PLAN_CACHE_MODE,
        "random_page_cost": RANDOM_PAGE_COST,
    },
}
ANALYZERS = {
    "english-no-stopwords": {
        "role": "primary",
        "native_config": "public.tin_english_no_stopwords",
        "paradedb": {
            "tokenizer": {
                "type": "default",
                "stemmer": "English",
                "k1": 1.2,
                "b": 0.75,
            },
        },
    },
    "english-stopwords": {
        "role": "control",
        "native_config": "pg_catalog.english",
        "paradedb": {
            "tokenizer": {
                "type": "default",
                "stemmer": "English",
                "stopwords_language": "English",
                "k1": 1.2,
                "b": 0.75,
            },
        },
    },
}


def analyzer_sql(engine, analyzer):
    if analyzer not in ANALYZERS:
        raise ConfigError(f"unknown analyzer: {analyzer}")
    if engine == "pg_textsearch":
        config = ANALYZERS[analyzer]["native_config"]
        if analyzer == "english-stopwords":
            setup = ""
        else:
            setup = """
DROP TEXT SEARCH CONFIGURATION IF EXISTS tin_english_no_stopwords CASCADE;
DROP TEXT SEARCH DICTIONARY IF EXISTS tin_english_stem;
CREATE TEXT SEARCH DICTIONARY tin_english_stem (
  TEMPLATE = snowball,
  Language = english
);
CREATE TEXT SEARCH CONFIGURATION tin_english_no_stopwords
  (COPY = pg_catalog.english);
ALTER TEXT SEARCH CONFIGURATION tin_english_no_stopwords
  ALTER MAPPING FOR asciiword, asciihword, hword_asciipart, word,
  hword, hword_part
  WITH tin_english_stem;
"""
        return (
            "DROP INDEX IF EXISTS documents_bm25;\n"
            f"{setup}"
            "CREATE INDEX documents_bm25 ON documents USING bm25 (body) "
            f"WITH (text_config = '{config}', k1 = 1.2, b = 0.75);\n"
            "ALTER DATABASE postgres SET default_text_search_config = "
            f"'{config}';\n"
        )
    if engine == "paradedb":
        tokenizer = json.dumps(
            ANALYZERS[analyzer]["paradedb"],
            separators=(",", ":"),
        )
        text_fields = json.dumps(
            {"body": json.loads(tokenizer)},
            separators=(",", ":"),
        )
        return (
            "DROP INDEX IF EXISTS documents_bm25;\n"
            "CREATE INDEX documents_bm25 ON documents USING bm25 "
            "(id, body) WITH (key_field = 'id', "
            f"text_fields = '{text_fields}');\n"
        )
    raise ConfigError(f"unsupported TIN engine: {engine}")


def _family_kinds(family):
    if family == "interleaved-all":
        return ("disjunction", "conjunction", "phrase")
    if family == "interleaved-conjunction-phrase":
        return ("conjunction", "phrase")
    if family in {"disjunction", "conjunction", "phrase"}:
        return (family,)
    raise ConfigError(f"unknown query family: {family}")


def scenario_sql(
    engine,
    *,
    operation,
    family,
    limit,
    native_query_sql=None,
    shape="rank_first",
):
    kinds = _family_kinds(family)
    sql_operation = "search" if operation == "concurrent-writes" else operation
    if engine == "pg_textsearch":
        if native_query_sql is None:
            from benchmarks.scale.native_rewrites import query_sql

            native_query_sql = query_sql
        return {
            kind: native_query_sql(kind, sql_operation, shape, limit)
            for kind in kinds
        }
    if engine != "paradedb":
        raise ConfigError(f"unsupported TIN engine: {engine}")
    operator = {
        "disjunction": "|||",
        "conjunction": "&&&",
        "phrase": "@@@",
    }
    if sql_operation == "count":
        return {
            kind: (
                f"SELECT count(*) FROM documents WHERE body "
                f"{operator[kind]} $1"
            )
            for kind in kinds
        }
    return {
        kind: (
            f"SELECT id FROM documents WHERE body {operator[kind]} $1 "
            f"ORDER BY paradedb.score(id) DESC LIMIT {limit}"
        )
        for kind in kinds
    }


def _cell(profile, analyzer, pack, operation, query_family):
    writes = operation == "concurrent-writes"
    return {
        "cell_id": (
            f"{profile}.{analyzer}.{pack}.{operation}.{query_family}"
        ),
        "profile": profile,
        "analyzer": analyzer,
        "analyzer_role": ANALYZERS[analyzer]["role"],
        "pack": pack,
        "operation": operation,
        "query_family": query_family,
        "sql_shape": "rank_first",
        "vus": 8,
        "limit": 10,
        "update_rate": 1000 if writes else 0,
        "preallocated_writers": 32 if writes else 0,
        "max_writers": 64 if writes else 0,
    }


def _primary_cells(profile, analyzer):
    cells = []
    for pack in ("natural", "spans"):
        for family in (
            "disjunction",
            "conjunction",
            "phrase",
            "interleaved-all",
            "interleaved-conjunction-phrase",
        ):
            cells.append(_cell(profile, analyzer, pack, "search", family))
        for family in (
            "disjunction", "conjunction", "phrase", "interleaved-all"
        ):
            cells.append(_cell(profile, analyzer, pack, "count", family))
        for family in ("disjunction", "interleaved-all"):
            cells.append(
                _cell(
                    profile,
                    analyzer,
                    pack,
                    "concurrent-writes",
                    family,
                )
            )
    return cells


def _control_cells(profile):
    return [
        _cell(profile, "english-stopwords", pack, "search", family)
        for pack in ("natural", "spans")
        for family in ("disjunction", "conjunction", "phrase")
    ]


def build_tin_plan(*, smoke, accept_terms, dataset_kind):
    if not smoke and dataset_kind == "msmarco-v2" and not accept_terms:
        raise ConfigError(
            "MS MARCO terms acceptance is required for the full benchmark"
        )
    cells = []
    if dataset_kind == "wikipedia":
        cells = [
            _cell(
                profile,
                "english-no-stopwords",
                "natural",
                "count",
                "disjunction",
            )
            for profile in PROFILES
        ]
    else:
        for profile in PROFILES:
            cells.extend(_primary_cells(profile, "english-no-stopwords"))
            cells.extend(_control_cells(profile))
    return {
        "format": 1,
        "campaign": "tin-expanded-v1",
        "dataset": (
            "synthetic-smoke" if smoke else dataset_kind
        ),
        "benchmarker_sha": BENCHMARKER_SHA,
        "pg_textsearch_sha": PG_TEXTSEARCH_SHA,
        "paradedb_tag": PARADEDB_TAG,
        "paradedb_digest": PARADEDB_IMAGE,
        "database_cpus": DATABASE_CPUS,
        "client_cpus": CLIENT_CPUS,
        "build_resources": BUILD_RESOURCES,
        "common_query_settings": COMMON_QUERY_SETTINGS,
        "profiles": PROFILES,
        "analyzers": ANALYZERS,
        "cache_reset": {
            "status": "required-before-profile",
            "hook_env": "PGTS_CACHE_RESET_HOOK",
            "dedicated_vm_required": True,
            "host_cache_drop_implemented": False,
            "required_method": "posix_fadvise_dontneed",
            "container_cgroup_memory_recorded_after_start": True,
        },
        "sql_shapes": {
            "default": "rank_first",
            "primary_status": "pending-full-corpus-pilot",
            "available": ["rank_first", "filter_first", "original"],
            "original_has_arbitrary_inner_limit": False,
            "rank_first": (
                "exact, but sparse conjunction/phrase matches may exhaust "
                "the OR-ranked stream"
            ),
            "filter_first": (
                "exact Boolean candidate materialization followed by "
                "standalone scoring"
            ),
        },
        "controls": {
            "wikipedia": {
                "dataset": "wikipedia",
                "operation": "count",
                "query_family": "disjunction",
                "pack": "natural",
                "separate_campaign": True,
            },
        },
        "cells": cells,
    }


def select_cells(
    cells,
    *,
    profiles=None,
    operations=None,
    packs=None,
    analyzers=None,
    families=None,
):
    selectors = {
        "profile": profiles,
        "operation": operations,
        "pack": packs,
        "analyzer": analyzers,
        "query_family": families,
    }
    return [
        cell for cell in cells
        if all(
            not values or cell[key] in values
            for key, values in selectors.items()
        )
    ]


def apply_sql_shape(cells, shape):
    if shape not in {"rank_first", "filter_first", "original"}:
        raise ConfigError(f"unknown SQL shape: {shape}")
    return [
        cell | {
            "cell_id": f"{cell['cell_id']}.shape-{shape}",
            "sql_shape": shape,
        }
        for cell in cells
    ]


def interleaved_selection(iteration, query_count, kind_count):
    if query_count < 1 or kind_count < 1:
        raise ConfigError("interleaved query and kind counts must be positive")
    kind_index = iteration % kind_count
    query_index = (
        (iteration // kind_count)
        + kind_index * (query_count // kind_count)
    ) % query_count
    return query_index, kind_index


def ordered_cells(cells):
    operation_order = {
        "search": 0,
        "count": 1,
        "concurrent-writes": 2,
    }
    pack_order = {"natural": 0, "spans": 1}
    return sorted(
        cells,
        key=lambda cell: (
            operation_order[cell["operation"]],
            pack_order[cell["pack"]],
            cell["query_family"],
            cell["cell_id"],
        ),
    )


def _duration_seconds(value):
    units = {"s": 1, "m": 60, "h": 3600}
    try:
        return float(value[:-1]) * units[value[-1]]
    except (KeyError, ValueError, IndexError) as error:
        raise ConfigError(
            f"unsupported benchmark duration: {value}"
        ) from error


def database_size_proof(text, *, expected_rows):
    lines = text.splitlines()
    if len(lines) != 2 or len(lines[1].split("\t")) != 2:
        raise ConfigError("incomplete database row-count and size proof")
    values = [lines[0], *lines[1].split("\t")]
    if not all(value.isdecimal() for value in values):
        raise ConfigError("invalid database row-count and size proof")
    rows, index_bytes, table_bytes = map(int, values)
    if rows != expected_rows:
        raise ConfigError(
            f"database row count {rows} does not match expected "
            f"{expected_rows}"
        )
    if index_bytes <= 0 or table_bytes < index_bytes:
        raise ConfigError("invalid database relation sizes")
    return {
        "rows": rows, "index_bytes": index_bytes,
        "table_total_bytes": table_bytes,
    }


def load_quality(summary, *, update_rate, duration):
    metrics = summary["metrics"]
    updates = metrics["update_failures"]
    attempts = updates["passes"] + updates["fails"]
    if attempts <= 0:
        raise ConfigError("concurrent-write cell did not exercise updates")
    actual = metrics.get("achieved_updates", {"count": 0})["count"]
    if actual != updates["fails"]:
        raise ConfigError("successful write counter does not reconcile")
    dropped = metrics.get("dropped_iterations", {"count": 0})["count"]
    target = int(update_rate * _duration_seconds(duration))
    target_met = actual >= target and dropped == 0
    return {
        "target_rate": update_rate,
        "duration": duration,
        "target_updates": target,
        "actual_writes": actual,
        "attempted_writes": attempts,
        "failed_writes": updates["passes"],
        "dropped_arrivals": dropped,
        "target_met": target_met,
        "unequal_pressure": not target_met,
    }


def _sql_literal(value):
    return "'" + value.replace("'", "''") + "'"


def probe_parameter_types(engine, operation, kind):
    if engine == "paradedb":
        return "text"
    if operation == "count":
        return "tsquery"
    if kind == "disjunction":
        return "text"
    return "text, tsquery"


def probe_sql(*, engine, operation, kind, statement, query, explain):
    if engine == "paradedb":
        value = (
            json.dumps(query["text"])
            if kind == "phrase"
            else query["text"]
        )
        types = probe_parameter_types(engine, operation, kind)
        arguments = _sql_literal(value)
    elif operation == "count":
        types = probe_parameter_types(engine, operation, kind)
        arguments = (
            f"{_sql_literal(query['pg_tsquery'][kind])}::tsquery"
        )
    elif kind == "disjunction":
        types = probe_parameter_types(engine, operation, kind)
        arguments = _sql_literal(query["text"])
    else:
        types = probe_parameter_types(engine, operation, kind)
        arguments = (
            f"{_sql_literal(query['text'])}, "
            f"{_sql_literal(query['pg_tsquery'][kind])}::tsquery"
        )
    execute = f"EXECUTE tin_probe({arguments})"
    if explain:
        execute = (
            "EXPLAIN (COSTS, VERBOSE, SETTINGS, FORMAT JSON) "
            + execute
        )
    return (
        f"PREPARE tin_probe({types}) AS {statement}; "
        f"{execute}; DEALLOCATE tin_probe"
    )


def _plan_node_types(value):
    if isinstance(value, dict):
        types = (
            [value["Node Type"]] if "Node Type" in value else []
        )
        for child in value.values():
            types.extend(_plan_node_types(child))
        return types
    if isinstance(value, list):
        types = []
        for child in value:
            types.extend(_plan_node_types(child))
        return types
    return []


def classify_explain(text, *, engine, operation, shape):
    plan = json.loads(text)
    node_types = _plan_node_types(plan)
    indexed = "Index Scan" in node_types
    if engine != "pg_textsearch" or shape == "original":
        return "indexed" if indexed else "captured"
    if indexed:
        return "indexed"
    if operation == "count":
        return "non_indexed_count"
    raise ConfigError(
        "rewritten native search plan does not contain an Index Scan"
    )


def parse_plan_audit(
    text, *, queries, engine, operation, kind, shape
):
    chunks = {}
    current = None
    lines = []
    for line in text.splitlines():
        if line.startswith("TIN_AUDIT_BEGIN:"):
            current = int(line.rsplit(":", 1)[1])
            lines = []
        elif line.startswith("TIN_AUDIT_END:"):
            ended = int(line.rsplit(":", 1)[1])
            if current != ended:
                raise ConfigError("plan audit markers are unbalanced")
            chunks[current] = "\n".join(lines).strip()
            current = None
        elif current is not None:
            lines.append(line)
    if current is not None or len(chunks) != len(queries):
        raise ConfigError("plan audit did not return every query")
    records = []
    for index, query in enumerate(queries):
        chunk = chunks[index]
        if "ERROR:" in chunk:
            if "canceling statement due to statement timeout" not in chunk:
                raise ConfigError(
                    f"plan audit SQL failed for query {query['id']}"
                )
            classification = "timeout"
            censored = True
        elif (
            engine == "pg_textsearch"
            and query.get("pg_tsquery", {}).get(kind) == ""
        ):
            empty_plan = json.loads(chunk)
            node_types = _plan_node_types(empty_plan)
            if (
                "Result" in node_types
                and not any(
                    "Scan" in value
                    and value not in {"CTE Scan", "Subquery Scan"}
                    for value in node_types
                )
            ):
                classification = "empty_query"
            else:
                classification = "non_indexed_empty"
            censored = False
        else:
            try:
                classification = classify_explain(
                    chunk,
                    engine=engine,
                    operation=operation,
                    shape=shape,
                )
            except ConfigError:
                classification = "non_indexed_search"
            censored = False
        records.append({
            "index": index,
            "query_id": query["id"],
            "classification": classification,
            "censored": censored,
        })
    counts = {
        classification: sum(
            record["classification"] == classification
            for record in records
        )
        for classification in {
            record["classification"] for record in records
        }
    }
    disallowed = {
        "timeout",
        "non_indexed_search",
        "non_indexed_count",
        "non_indexed_empty",
    }
    return {
        "audited_queries": len(records),
        "indexed_queries": counts.get("indexed", 0),
        "empty_queries": counts.get("empty_query", 0),
        "classifications": counts,
        "timing_allowed": not any(
            record["classification"] in disallowed
            for record in records
        ),
        "records": records,
    }


class TinRunner(Runner):
    def __init__(
        self,
        root,
        artifacts,
        smoke,
        run_id,
        dataset_kind,
        dataset_path,
    ):
        super().__init__(
            root,
            artifacts,
            smoke,
            run_id,
            dataset_kind,
            dataset_path,
        )
        self.tin_dir = self.run_dir / "tin"
        self.tin_dir.mkdir(parents=True, exist_ok=True)

    def install(self):
        installer = Runner(
            self.root, self.artifacts / "tin-install", self.smoke,
            self.run_id, self.dataset_kind, None,
        )
        installer.tools = self.tools
        installer.install()
        self.command(["docker", "pull", PARADEDB_IMAGE])
        inspect = subprocess.run(
            ["docker", "image", "inspect", PARADEDB_IMAGE],
            check=True,
            capture_output=True,
            text=True,
        )
        digests = json.loads(inspect.stdout)[0].get("RepoDigests") or []
        digests = [
            value for value in digests
            if value.startswith("paradedb/paradedb@sha256:")
        ]
        if PARADEDB_IMAGE not in digests:
            raise ConfigError(
                "ParadeDB v0.27.0-pg18 did not match the campaign digest"
            )
        legacy = json.loads(
            (installer.artifacts / "images.lock.json").read_text()
        )
        lock = {
            "paradedb_tag": PARADEDB_TAG,
            "paradedb": PARADEDB_IMAGE,
            "pg_textsearch_source": legacy["pg_textsearch_source"],
            "pg_textsearch_image_id": legacy["pg_textsearch_image_id"],
            "postgres_base": legacy["postgres_base"],
            "benchmarker": BENCHMARKER_SHA,
        }
        self._write_or_validate_json(
            self.artifacts / "tin-images.lock.json", lock
        )

    def server_args(
        self, *, name, volume, image, profile, engine, phase="query"
    ):
        if phase not in {"build", "query"}:
            raise ConfigError(f"unknown database phase: {phase}")
        resources = (
            BUILD_RESOURCES if phase == "build" else PROFILES[profile]
        )
        args = [
            "docker",
            "run",
            "-d",
            "--name",
            name,
            "--label",
            "managed-by=pg_textsearch-tin",
            "--label",
            f"run-id={self.run_id}",
            "--label",
            f"engine={engine}",
            f"--shm-size={resources['shm_size']}",
            f"--cpuset-cpus={resources['cpus']}",
            f"--memory={resources['memory']}",
            f"--memory-swap={resources['memory_swap']}",
            "--mount",
            f"type=volume,source={volume},target=/var/lib/postgresql",
            "-e",
            "POSTGRES_PASSWORD=postgres",
            "-p",
            "127.0.0.1:15432:5432",
            image,
            "-c",
            f"shared_buffers={resources['shared_buffers']}",
            "-c",
            f"effective_cache_size={resources['effective_cache_size']}",
            "-c",
            (
                "maintenance_work_mem="
                f"{resources['maintenance_work_mem']}"
            ),
            "-c",
            f"max_connections={resources['max_connections']}",
            "-c",
            (
                "track_io_timing="
                f"{'on' if resources['track_io_timing'] else 'off'}"
            ),
            "-c",
            f"plan_cache_mode={resources['plan_cache_mode']}",
            "-c",
            f"random_page_cost={resources['random_page_cost']}",
        ]
        if engine == "pg_textsearch":
            args.extend(
                ["-c", "shared_preload_libraries=pg_textsearch"]
            )
        return args

    def require_cache_reset(self, phase, volume):
        hook = os.environ.get("PGTS_CACHE_RESET_HOOK")
        if not hook:
            raise ConfigError(
                "PGTS_CACHE_RESET_HOOK is required before each memory profile"
            )
        hook_path = Path(hook).resolve()
        if not hook_path.is_file():
            raise ConfigError(f"cache reset hook is not a file: {hook_path}")
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        result = subprocess.run(
            [hook, phase, str(self.run_dir), volume],
            check=True,
            capture_output=True,
            text=True,
        )
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        evidence = json.loads(lines[-1]) if lines else {}
        if not (
            evidence.get("performed")
            and evidence.get("dedicated_vm")
            and evidence.get("verified")
            and evidence.get("method") == "posix_fadvise_dontneed"
            and evidence.get("phase") == phase
            and evidence.get("volume") == volume
            and evidence.get("files", 0) > 0
            and evidence.get("resident_pages_after") == 0
        ):
            raise ConfigError(
                "cache reset hook did not return verified evidence"
            )
        record = {
            "phase": phase,
            "volume": volume,
            "started_at": started,
            "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "hook": str(hook_path),
            "hook_sha256": file_sha256(hook_path),
            "evidence": evidence,
        }
        directory = self.tin_dir / "cache-reset" / phase
        directory.mkdir(parents=True, exist_ok=True)
        output = directory / f"{time.time_ns()}.json"
        output.write_text(json.dumps(record, indent=2) + "\n")
        return record

    @staticmethod
    def cache_reset_hook_sha256():
        hook = os.environ.get("PGTS_CACHE_RESET_HOOK")
        if not hook:
            raise ConfigError(
                "PGTS_CACHE_RESET_HOOK is required before benchmark execution"
            )
        hook_path = Path(hook).resolve()
        if not hook_path.is_file():
            raise ConfigError(f"cache reset hook is not a file: {hook_path}")
        return file_sha256(hook_path)

    def enrich_query_pack(
        self,
        container,
        analyzer,
        pack,
        *,
        enrich_queries=None,
    ):
        if enrich_queries is None:
            from benchmarks.scale.native_rewrites import enrich_queries
        source = self.dataset / "k6" / f"{pack}.json"
        if not source.is_file():
            raise ConfigError(f"query pack is missing: {source}")
        directory = self.tin_dir / "query-packs" / analyzer
        directory.mkdir(parents=True, exist_ok=True)
        output = directory / f"{pack}.json"
        fingerprint = {
            "format": 1,
            "analyzer": analyzer,
            "text_config": ANALYZERS[analyzer]["native_config"],
            "pack": pack,
            "source_sha256": file_sha256(source),
        }
        state = output.with_suffix(".fingerprint.json")
        if output.is_file() or state.is_file():
            if not (output.is_file() and state.is_file()):
                raise ConfigError(
                    f"incomplete enriched query checkpoint: {output}"
                )
            if json.loads(state.read_text()) != fingerprint:
                raise ConfigError(
                    f"incompatible enriched query checkpoint: {output}"
                )
            return output
        queries = json.loads(source.read_text())
        enriched = enrich_queries(
            container,
            ANALYZERS[analyzer]["native_config"],
            queries,
        )
        if len(enriched) != len(queries):
            raise ConfigError("native query enrichment changed pack length")
        for original, result in zip(queries, enriched, strict=True):
            if any(result.get(key) != value
                   for key, value in original.items()):
                raise ConfigError(
                    "native query enrichment changed source query fields"
                )
            mapping = result.get("pg_tsquery")
            if set(mapping or {}) != {
                "disjunction", "conjunction", "phrase"
            }:
                raise ConfigError(
                    "native query enrichment omitted a query family"
                )
        temporary = output.with_suffix(".json.partial")
        temporary.write_text(
            json.dumps(enriched, indent=2, ensure_ascii=False) + "\n"
        )
        temporary.replace(output)
        self._write_or_validate_json(state, fingerprint)
        return output

    def _volume_name(self, kind, engine, analyzer=None):
        suffix = hashlib.sha256(
            str(self.run_dir).encode()
        ).hexdigest()[:10]
        parts = ["pgts-tin", suffix, kind, engine.replace("_", "-")]
        if analyzer:
            parts.append(analyzer)
        return "-".join(parts)

    def _ensure_volume(self, name, labels):
        inspect = subprocess.run(
            ["docker", "volume", "inspect", name],
            capture_output=True,
            text=True,
        )
        existed = inspect.returncode == 0
        expected = {
            "managed-by": "pg_textsearch-tin",
            "run-id": self.run_id,
        } | labels
        if not existed:
            command = ["docker", "volume", "create"]
            for key, value in expected.items():
                command.extend(["--label", f"{key}={value}"])
            command.append(name)
            self.command(command)
            return False
        actual = json.loads(inspect.stdout)[0].get("Labels") or {}
        if any(actual.get(key) != value for key, value in expected.items()):
            raise ConfigError(f"refusing foreign Docker volume: {name}")
        return True

    def _require_owned_volume(self, name, labels):
        inspect = subprocess.run(
            ["docker", "volume", "inspect", name],
            check=True,
            capture_output=True,
            text=True,
        )
        actual = json.loads(inspect.stdout)[0].get("Labels") or {}
        expected = {
            "managed-by": "pg_textsearch-tin",
            "run-id": self.run_id,
        } | labels
        if any(actual.get(key) != value for key, value in expected.items()):
            raise ConfigError(f"refusing foreign Docker volume: {name}")
        users = subprocess.run(
            [
                "docker", "ps", "-aq", "--filter", f"volume={name}",
                "--format", "{{.ID}}",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if users:
            raise ConfigError(
                f"source Docker volume must be offline before clone: {name}"
            )

    def _clone_volume(
        self,
        source,
        target,
        labels,
        checkpoint,
        *,
        image,
        source_labels,
    ):
        self._require_owned_volume(source, source_labels)
        existed = self._ensure_volume(target, labels)
        value = {
            "source": source,
            "target": target,
            "labels": labels,
            "source_labels": source_labels,
            "image": image,
        }
        if checkpoint.is_file():
            if not existed:
                raise ConfigError(
                    f"cloned volume missing for resume: {target}"
                )
            self._write_or_validate_json(checkpoint, value)
            return
        if existed:
            raise ConfigError(
                f"uncheckpointed clone volume already exists: {target}"
            )
        self.command([
            "docker",
            "run",
            "--rm",
            "--mount",
            f"type=volume,source={source},target=/source,readonly",
            "--mount",
            f"type=volume,source={target},target=/destination",
            image,
            "bash",
            "-ceu",
            "cp -a /source/. /destination/",
        ])
        self._write_or_validate_json(
            checkpoint,
            value,
        )

    def _remove_owned_write_volume(self, name, labels):
        inspect = subprocess.run(
            ["docker", "volume", "inspect", name],
            capture_output=True,
            text=True,
        )
        if inspect.returncode != 0:
            return
        actual = json.loads(inspect.stdout)[0].get("Labels") or {}
        expected = {
            "managed-by": "pg_textsearch-tin",
            "run-id": self.run_id,
        } | labels
        if any(actual.get(key) != value for key, value in expected.items()):
            raise ConfigError(f"refusing foreign write volume: {name}")
        users = subprocess.run(
            [
                "docker", "ps", "-aq", "--filter", f"volume={name}",
                "--format", "{{.ID}}",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if users:
            raise ConfigError(
                f"write volume is still attached to a container: {name}"
            )
        self.command(["docker", "volume", "rm", name])

    def _prepare_write_volume(
        self,
        *,
        engine,
        analyzer,
        profile,
        cell_id,
        source_volume,
        image,
        result_dir,
    ):
        cell_hash = hashlib.sha256(cell_id.encode()).hexdigest()[:12]
        volume = self._volume_name(
            "write", engine, f"{analyzer}-{profile}-{cell_hash}"
        )
        labels = {
            "kind": "write-attempt",
            "engine": engine,
            "analyzer": analyzer,
            "profile": profile,
            "cell": cell_id,
        }
        self._remove_owned_write_volume(volume, labels)
        attempt = result_dir / "write-attempts" / str(time.time_ns())
        attempt.mkdir(parents=True)
        try:
            self._clone_volume(
                source_volume,
                volume,
                labels,
                attempt / "clone.json",
                image=image,
                source_labels={
                    "kind": "analyzer",
                    "engine": engine,
                    "analyzer": analyzer,
                },
            )
        except (
            ConfigError,
            OSError,
            subprocess.CalledProcessError,
        ) as error:
            self._write_or_validate_json(
                attempt / "retention.json",
                {
                    "retention_requested": True,
                    "volume": volume,
                    "labels": labels,
                    "reason": "write-clone-failed",
                    "error": str(error),
                },
            )
            raise
        return volume, attempt

    def _images(self):
        path = self.artifacts / "tin-images.lock.json"
        if not path.is_file():
            raise ConfigError("run the TIN install stage first")
        lock = json.loads(path.read_text())
        if (
            lock.get("paradedb_tag") != PARADEDB_TAG
            or lock.get("paradedb") != PARADEDB_IMAGE
        ):
            raise ConfigError(
                "TIN image lock does not pin the ParadeDB campaign digest"
            )
        native_image = lock.get("pg_textsearch_image_id", "")
        if (
            lock.get("pg_textsearch_source") != PG_TEXTSEARCH_SHA
            or not native_image.startswith("sha256:")
        ):
            raise ConfigError(
                "TIN image lock does not pin the native source image"
            )
        return {
            "pg_textsearch": native_image,
            "paradedb": lock["paradedb"],
        }, lock

    def _start_database(
        self, *, name, volume, image, profile, engine, output,
        phase="query",
    ):
        existing = subprocess.run(
            ["docker", "container", "inspect", name],
            capture_output=True,
            text=True,
        )
        if existing.returncode == 0:
            raise ConfigError(
                f"refusing preexisting Docker container: {name} already exists"
            )
        if "no such" not in existing.stderr.lower():
            raise ConfigError(
                f"could not verify Docker container name is free: {name}"
            )
        self.command(
            self.server_args(
                name=name,
                volume=volume,
                image=image,
                profile=profile,
                engine=engine,
                phase=phase,
            ),
            output=output,
        )
        owned = getattr(self, "_owned_containers", {})
        owned[name] = engine
        self._owned_containers = owned
        self._wait(name)

    def _stop_database(self, name, log):
        expected_engine = getattr(
            self, "_owned_containers", {}
        ).get(name)
        if expected_engine is None:
            raise ConfigError(
                f"refusing untracked Docker container stop: {name}"
            )
        inspect = subprocess.run(
            ["docker", "container", "inspect", name],
            capture_output=True,
            text=True,
        )
        if inspect.returncode != 0:
            raise ConfigError(
                f"owned Docker container is missing: {name}"
            )
        labels = (
            json.loads(inspect.stdout)[0]
            .get("Config", {})
            .get("Labels", {})
            or {}
        )
        expected = {
            "managed-by": "pg_textsearch-tin",
            "run-id": self.run_id,
            "engine": expected_engine,
        }
        if any(labels.get(key) != value
               for key, value in expected.items()):
            raise ConfigError(
                f"refusing foreign Docker container stop: {name}"
            )
        self.command(
            ["docker", "stop", "--timeout", "60", name]
        )
        self.command(["docker", "logs", name], output=log)
        self.command(["docker", "rm", name])
        del self._owned_containers[name]

    def _base_fingerprint(self, engine, image):
        prepared = self.prepared_dataset(engine)
        return {
            "format": 1,
            "engine": engine,
            "image": image,
            "dataset_manifest": file_sha256(
                self.dataset / "manifest.json"
            ),
            "schema": file_sha256(prepared / "schema.yaml"),
            "pre_sql": file_sha256(
                prepared / engine / "pre.sql"
            ),
            "post_sql": file_sha256(
                prepared / engine / "post.sql"
            ),
            "prepared_corpus_has_index": False,
            "build_resources": BUILD_RESOURCES,
            "benchmarker": BENCHMARKER_SHA,
        }

    def prepared_dataset(self, engine):
        output = self.tin_dir / "prepared-dataset" / engine
        backend = output / engine
        backend.mkdir(parents=True, exist_ok=True)
        data = output / "data"
        source_data = (self.dataset / "data").resolve()
        if data.exists() or data.is_symlink():
            if not data.is_symlink() or data.resolve() != source_data:
                raise ConfigError(
                    f"incompatible prepared data link: {data}"
                )
        else:
            data.symlink_to(source_data, target_is_directory=True)
        for name in ("schema.yaml", "manifest.json"):
            source = self.dataset / name
            if not source.is_file():
                if name == "manifest.json":
                    continue
                raise ConfigError(f"prepared dataset source missing: {source}")
            destination = output / name
            if destination.is_file():
                if file_sha256(destination) != file_sha256(source):
                    raise ConfigError(
                        f"incompatible prepared dataset file: {destination}"
                    )
            else:
                shutil.copyfile(source, destination)
        pre_sql = self.dataset / engine / "pre.sql"
        destination = backend / "pre.sql"
        if destination.is_file():
            if file_sha256(destination) != file_sha256(pre_sql):
                raise ConfigError(
                    f"incompatible prepared pre.sql: {destination}"
                )
        else:
            shutil.copyfile(pre_sql, destination)
        post_sql = backend / "post.sql"
        if post_sql.is_file() and post_sql.read_text() != "":
            raise ConfigError(f"prepared post.sql is not empty: {post_sql}")
        post_sql.touch(exist_ok=True)
        return output

    def build_base(self, engine):
        images, _ = self._images()
        image = images[engine]
        directory = self.tin_dir / "build" / engine / "prepared-corpus"
        directory.mkdir(parents=True, exist_ok=True)
        volume = self._volume_name("corpus", engine)
        existed = self._ensure_volume(
            volume,
            {"kind": "prepared-corpus", "engine": engine},
        )
        fingerprint = self._base_fingerprint(engine, image)
        complete = directory / "complete.json"
        if complete.is_file():
            if not existed:
                raise ConfigError(f"prepared volume missing: {volume}")
            self._write_or_validate_json(complete, fingerprint)
            return volume
        if existed:
            raise ConfigError(
                f"uncheckpointed prepared volume already exists: {volume}"
            )
        name = f"pgts-tin-build-{engine.replace('_', '-')}"
        self._start_database(
            name=name,
            volume=volume,
            image=image,
            profile="high",
            engine=engine,
            output=directory / "container-start.log",
            phase="build",
        )
        started = time.time()
        try:
            prepared = self.prepared_dataset(engine)
            env = os.environ | {
                f"{engine.upper()}_URL": (
                    "postgres://postgres:postgres@127.0.0.1:15432/postgres"
                )
            }
            self.command(
                [
                    "taskset",
                    "-c",
                    CLIENT_CPUS,
                    str(self.tools / "benchmarker/bin/loader"),
                    "load",
                    "--backend",
                    engine,
                    "--backends-dir",
                    str(self.dataset / "backends"),
                    "--workers",
                    "8",
                    "--batch-size",
                    "10000",
                    str(prepared),
                ],
                env=env,
                output=directory / "load-and-baseline-index.log",
            )
            metadata = {
                "elapsed_seconds": time.time() - started,
                "prepared_corpus_contains_baseline_index": False,
            }
            self._write_or_validate_json(
                directory / "build-resources.json",
                BUILD_RESOURCES | metadata,
            )
            self._write_or_validate_json(complete, fingerprint)
        finally:
            self._stop_database(name, directory / "postgres.log")
        return volume

    def build_analyzer(self, engine, analyzer):
        images, _ = self._images()
        base = self.build_base(engine)
        directory = self.tin_dir / "build" / engine / analyzer
        directory.mkdir(parents=True, exist_ok=True)
        volume = self._volume_name("analyzer", engine, analyzer)
        clone = directory / "clone.json"
        self._clone_volume(
            base,
            volume,
            {
                "kind": "analyzer",
                "engine": engine,
                "analyzer": analyzer,
            },
            clone,
            image=images[engine],
            source_labels={
                "kind": "prepared-corpus",
                "engine": engine,
            },
        )
        sql = analyzer_sql(engine, analyzer)
        fingerprint = {
            "format": 1,
            "engine": engine,
            "analyzer": ANALYZERS[analyzer],
            "image": images[engine],
            "sql_sha256": hashlib.sha256(sql.encode()).hexdigest(),
            "build_resources": BUILD_RESOURCES,
            "base": json.loads(
                (self.tin_dir / "build" / engine /
                 "prepared-corpus/complete.json").read_text()
            ),
            "database_proof_protocol": DATABASE_PROOF_PROTOCOL,
        }
        complete = directory / "complete.json"
        if complete.is_file():
            self._write_or_validate_json(complete, fingerprint)
            build_path = directory / "build.json"
            proof_path = directory / "rowcount-and-sizes.tsv"
            if not build_path.is_file() or not proof_path.is_file():
                raise ConfigError(
                    f"analyzer database proof is missing: {directory}"
                )
            current_proof = database_size_proof(
                proof_path.read_text(),
                expected_rows=json.loads(
                    (self.dataset / "manifest.json").read_text()
                )["rows"],
            )
            build_metadata = json.loads(build_path.read_text())
            if (
                build_metadata.get("database_proof_protocol")
                != DATABASE_PROOF_PROTOCOL
                or build_metadata.get("database") != current_proof
            ):
                raise ConfigError(
                    f"analyzer database proof is stale: {directory}"
                )
            return volume
        name = (
            f"pgts-tin-analyzer-{engine.replace('_', '-')}-"
            f"{analyzer}"
        )
        self._start_database(
            name=name,
            volume=volume,
            image=images[engine],
            profile="high",
            engine=engine,
            output=directory / "container-start.log",
            phase="build",
        )
        build_started = time.time()
        try:
            sql_path = directory / "index.sql"
            sql_path.write_text(sql)
            index_started = time.time()
            self.command(
                [
                    "docker",
                    "exec",
                    name,
                    "psql",
                    "-U",
                    "postgres",
                    "-v",
                    "ON_ERROR_STOP=1",
                    "-c",
                    sql,
                ],
                output=directory / "index-build.log",
            )
            index_seconds = time.time() - index_started
            vacuum_started = time.time()
            self.command(
                [
                    "docker", "exec", name, "psql", "-U", "postgres",
                    "-v", "ON_ERROR_STOP=1", "-c",
                    "VACUUM (ANALYZE) documents",
                ],
                output=directory / "vacuum-analyze.log",
            )
            vacuum_seconds = time.time() - vacuum_started
            self.command(
                [
                    "docker", "exec", name, "psql", "-U", "postgres",
                    "-XAt", "-F", "\t", "-v", "ON_ERROR_STOP=1", "-c",
                    "SELECT count(*) FROM documents; "
                    "SELECT pg_relation_size('documents_bm25'::regclass), "
                    "pg_total_relation_size('documents'::regclass)",
                ],
                output=directory / "rowcount-and-sizes.tsv",
            )
            size_proof = database_size_proof(
                (directory / "rowcount-and-sizes.tsv").read_text(),
                expected_rows=json.loads(
                    (self.dataset / "manifest.json").read_text()
                )["rows"],
            )
            self.command(
                [
                    "docker", "exec", name, "psql", "-U", "postgres",
                    "-XAt", "-F", "\t", "-c",
                    "SELECT name, setting, unit, source FROM pg_settings "
                    "WHERE name IN ('shared_buffers',"
                    "'effective_cache_size','maintenance_work_mem',"
                    "'plan_cache_mode','random_page_cost') "
                    "ORDER BY name",
                ],
                output=directory / "postgres-build-settings.tsv",
            )
            if engine == "pg_textsearch":
                for pack in ("natural", "spans"):
                    self.enrich_query_pack(name, analyzer, pack)
            build_path = directory / "build.json"
            build_temporary = build_path.with_suffix(".json.partial")
            build_temporary.write_text(
                json.dumps(
                    {
                        "resources": BUILD_RESOURCES,
                        "database_proof_protocol": (
                            DATABASE_PROOF_PROTOCOL
                        ),
                        "database": size_proof,
                        "index_seconds": index_seconds,
                        "vacuum_analyze_seconds": vacuum_seconds,
                        "total_seconds": time.time() - build_started,
                        "rowcount_and_sizes": "rowcount-and-sizes.tsv",
                        "postgres_settings": (
                            "postgres-build-settings.tsv"
                        ),
                    },
                    indent=2,
                )
                + "\n"
            )
            build_temporary.replace(build_path)
            self._write_or_validate_json(complete, fingerprint)
        finally:
            self._stop_database(name, directory / "postgres.log")
        return volume

    def build_selected(self, engines, analyzers):
        for analyzer in analyzers:
            self.build_analyzer("pg_textsearch", analyzer)
        for engine in engines:
            if engine == "pg_textsearch":
                continue
            for analyzer in analyzers:
                self.build_analyzer(engine, analyzer)

    def _run_environment(
        self, *, engine, cell, query_pack, scenario, result_dir
    ):
        return os.environ | {
            f"{engine.upper()}_URL": (
                "postgres://postgres:postgres@127.0.0.1:15432/postgres"
            ),
            "ENGINE": engine,
            "CELL_ID": cell["cell_id"],
            "PACK": cell["pack"],
            "PROFILE": cell["profile"],
            "ANALYZER": cell["analyzer"],
            "OPERATION": cell["operation"],
            "QUERY_FAMILY": cell["query_family"],
            "QUERY_FILE": str(query_pack),
            "SCENARIO_FILE": str(scenario),
            "UPDATE_TARGETS_FILE": str(
                self.dataset / "k6/update_targets.json"
            ),
            "UPDATE_RATE": str(cell["update_rate"]),
            "VUS": str(cell["vus"]),
            "DURATION": "3s" if self.smoke else "5m",
            "WARMUP": "2s" if self.smoke else "60s",
            "TOTAL_DURATION": "8s" if self.smoke else "7m",
            "BACKENDS_DIR": str(self.dataset / "backends"),
            "DASHBOARD_EXPORT_DIR": str(result_dir),
            "DASHBOARD_EXPORT_PREFIX": (
                f"{engine}_{cell['profile']}_{cell['analyzer']}_"
                f"{cell['pack']}_{cell['operation']}_"
                f"{cell['query_family']}"
            ),
        }

    def _record_write_quality(
        self, *, result_dir, cell, engine, duration, sql_status
    ):
        summary_path = result_dir / "k6-summary.json"
        if not summary_path.is_file():
            raise ConfigError(
                f"completed write cell is missing k6 metrics: {result_dir}"
            )
        quality = load_quality(
            json.loads(summary_path.read_text()),
            update_rate=cell["update_rate"],
            duration=duration,
        )
        quality["engine"] = engine
        quality["cell_id"] = cell["cell_id"]
        quality["sql_status"] = sql_status
        self._write_or_validate_json(
            result_dir / "load-quality.json", quality
        )
        return quality

    def _probe_cell(
        self, container, engine, cell, query_pack, sql_templates, result_dir
    ):
        queries = json.loads(query_pack.read_text())
        probe_dir = result_dir / "probes"
        probe_dir.mkdir(parents=True, exist_ok=True)
        records = []
        for kind in _family_kinds(cell["query_family"]):
            query = next(
                (
                    candidate for candidate in queries
                    if (
                        engine == "paradedb"
                        or candidate.get("pg_tsquery", {}).get(kind)
                    )
                ),
                None,
            )
            if query is None:
                raise ConfigError(
                    f"no nonempty {kind} query is available for probe"
                )
            fingerprint = {
                "format": 1,
                "engine": engine,
                "operation": cell["operation"],
                "kind": kind,
                "shape": cell["sql_shape"],
                "query_id": query["id"],
                "statement": sql_templates[kind],
            }
            complete = probe_dir / f"{kind}.complete.json"
            if complete.is_file():
                saved = json.loads(complete.read_text())
                if saved.get("fingerprint") != fingerprint:
                    raise ConfigError(
                        f"incompatible query probe resume: {complete}"
                    )
                records.append(saved["result"])
                continue
            explain_path = probe_dir / f"{kind}.explain.json"
            result_path = probe_dir / f"{kind}.result.txt"
            explain = probe_sql(
                engine=engine,
                operation=cell["operation"],
                kind=kind,
                statement=sql_templates[kind],
                query=query,
                explain=True,
            )
            execute = probe_sql(
                engine=engine,
                operation=cell["operation"],
                kind=kind,
                statement=sql_templates[kind],
                query=query,
                explain=False,
            )
            base = [
                "docker", "exec", "--env",
                (
                    "PGOPTIONS=-c statement_timeout=30000 "
                    f"-c plan_cache_mode={PLAN_CACHE_MODE} "
                    f"-c random_page_cost={RANDOM_PAGE_COST}"
                ),
                container, "psql", "-U", "postgres",
                "-q", "-XAt", "-v", "ON_ERROR_STOP=1", "-c",
            ]
            probe_env = os.environ.copy()
            probe_env.pop("PGOPTIONS", None)
            try:
                self.command(
                    base + [explain],
                    output=explain_path,
                    env=probe_env,
                )
            except subprocess.CalledProcessError:
                if (
                    "canceling statement due to statement timeout"
                    not in explain_path.read_text()
                ):
                    raise
                result = {
                    "kind": kind,
                    "query_id": query["id"],
                    "status": "timeout",
                    "censored": True,
                    "timeout_stage": "explain",
                    "classification": "not_captured",
                    "plan": explain_path.name,
                    "result": None,
                }
                self._write_or_validate_json(
                    complete,
                    {"fingerprint": fingerprint, "result": result},
                )
                records.append(result)
                continue
            classification = classify_explain(
                explain_path.read_text(),
                engine=engine,
                operation=cell["operation"],
                shape=cell["sql_shape"],
            )
            try:
                self.command(
                    base + [execute],
                    output=result_path,
                    env=probe_env,
                )
            except subprocess.CalledProcessError:
                if (
                    "canceling statement due to statement timeout"
                    not in result_path.read_text()
                ):
                    raise
                result = {
                    "kind": kind,
                    "query_id": query["id"],
                    "status": "timeout",
                    "censored": True,
                    "timeout_stage": "result",
                    "classification": classification,
                    "plan": explain_path.name,
                    "result": result_path.name,
                }
                self._write_or_validate_json(
                    complete,
                    {"fingerprint": fingerprint, "result": result},
                )
                records.append(result)
                continue
            result = {
                "kind": kind,
                "query_id": query["id"],
                "status": "ok",
                "censored": False,
                "classification": classification,
                "plan": explain_path.name,
                "result": result_path.name,
            }
            if cell["operation"] == "count":
                values = [
                    line.strip()
                    for line in result_path.read_text().splitlines()
                    if line.strip()
                ]
                if len(values) != 1:
                    raise ConfigError(
                        f"count probe did not return one scalar: {result_path}"
                    )
                try:
                    result["actual_count"] = int(values[0])
                except ValueError as error:
                    raise ConfigError(
                        f"count probe returned a non-integer: {result_path}"
                    ) from error
            else:
                result["result_rows"] = sum(
                    1 for line in result_path.read_text().splitlines()
                    if line.strip()
                )
            self._write_or_validate_json(
                complete,
                {"fingerprint": fingerprint, "result": result},
            )
            records.append(result)
        return {
            "selection_policy": "family-major-strided-v1",
            "records": records,
            "timing_allowed": not any(
                record["classification"] == "non_indexed_count"
                for record in records
            ),
        }

    def _audit_native_queries(
        self,
        *,
        container,
        phase_dir,
        phase,
        profile,
        analyzer,
        pack,
        operation,
        kind,
        shape,
        statement,
        query_pack,
    ):
        operation = (
            "search" if operation == "concurrent-writes" else operation
        )
        fingerprint = {
            "format": 1,
            "protocol": "all-query-custom-plan-v1",
            "phase": phase,
            "profile": profile,
            "analyzer": analyzer,
            "pack": pack,
            "operation": operation,
            "kind": kind,
            "shape": shape,
            "statement_sha256": hashlib.sha256(
                statement.encode()
            ).hexdigest(),
            "query_pack_sha256": file_sha256(query_pack),
            "code_sha256": file_sha256(Path(__file__)),
            "plan_cache_mode": PLAN_CACHE_MODE,
            "random_page_cost": RANDOM_PAGE_COST,
            "statement_timeout_ms": 30_000,
            "parameter_types": probe_parameter_types(
                "pg_textsearch", operation, kind
            ),
        }
        key = hashlib.sha256(
            json.dumps(fingerprint, sort_keys=True).encode()
        ).hexdigest()[:16]
        directory = phase_dir / "plan-audits" / key
        directory.mkdir(parents=True, exist_ok=True)
        complete = directory / "complete.json"
        if complete.is_file():
            saved = json.loads(complete.read_text())
            if saved.get("fingerprint") != fingerprint:
                raise ConfigError(
                    f"incompatible plan audit resume: {complete}"
                )
            return saved["summary"]
        queries = json.loads(query_pack.read_text())
        sql_path = directory / "audit.sql"
        output = directory / "plans.jsonstream"
        statements = [
            "SET statement_timeout = '30s'",
            f"SET plan_cache_mode = {PLAN_CACHE_MODE}",
            f"SET random_page_cost = {RANDOM_PAGE_COST}",
        ]
        for index, query in enumerate(queries):
            statements.extend([
                f"SELECT 'TIN_AUDIT_BEGIN:{index}'",
                probe_sql(
                    engine="pg_textsearch",
                    operation=operation,
                    kind=kind,
                    statement=statement,
                    query=query,
                    explain=True,
                ),
                f"SELECT 'TIN_AUDIT_END:{index}'",
            ])
        sql_path.write_text(";\n".join(statements) + ";\n")
        container_path = f"/tin-plan-audit-{key}.sql"
        self.command([
            "docker", "cp", str(sql_path),
            f"{container}:{container_path}",
        ])
        try:
            self.command(
                [
                    "docker", "exec", "--env",
                    (
                        "PGOPTIONS=-c statement_timeout=30000 "
                        f"-c plan_cache_mode={PLAN_CACHE_MODE} "
                        f"-c random_page_cost={RANDOM_PAGE_COST}"
                    ),
                    container, "psql", "-U", "postgres", "-q", "-XAt",
                    "-f", container_path,
                ],
                output=output,
            )
        finally:
            self.command([
                "docker", "exec", container, "rm", "-f", container_path,
            ])
        summary = parse_plan_audit(
            output.read_text(),
            queries=queries,
            engine="pg_textsearch",
            operation=operation,
            kind=kind,
            shape=shape,
        )
        summary["audit_key"] = key
        self._write_or_validate_json(
            complete,
            {"fingerprint": fingerprint, "summary": summary},
        )
        return summary

    def run_cells(self, engines, cells):
        images, image_lock = self._images()
        script = self.root / "benchmarks/scale/benchmarker/k6/tin.js"
        failures = []
        pressure = []
        groups = {}
        for engine in engines:
            for cell in cells:
                key = (cell["profile"], engine, cell["analyzer"])
                groups.setdefault(key, []).append(cell)
        for (profile, engine, analyzer), group in groups.items():
            phase = f"{profile}.{engine}.{analyzer}"
            source_volume = self._volume_name("analyzer", engine, analyzer)
            build_dir = self.tin_dir / "build" / engine / analyzer
            build_fingerprint = build_dir / "complete.json"
            if not build_fingerprint.is_file():
                raise ConfigError(
                    f"TIN analyzer build is missing: {engine}/{analyzer}"
                )
            name = (
                f"pgts-tin-run-{engine.replace('_', '-')}-{analyzer}"
            )
            phase_dir = (
                self.tin_dir / "phases" / profile / engine / analyzer
            )
            phase_dir.mkdir(parents=True, exist_ok=True)
            volume = self._volume_name(
                "profile", engine, f"{analyzer}-{profile}"
            )
            self._clone_volume(
                source_volume,
                volume,
                {
                    "kind": "profile",
                    "engine": engine,
                    "analyzer": analyzer,
                    "profile": profile,
                },
                phase_dir / "clone.json",
                image=images[engine],
                source_labels={
                    "kind": "analyzer",
                    "engine": engine,
                    "analyzer": analyzer,
                },
            )
            cache_record = self.require_cache_reset(phase, volume)
            (phase_dir / "cache-reset.latest.json").write_text(
                json.dumps(cache_record, indent=2) + "\n"
            )
            self._start_database(
                name=name,
                volume=volume,
                image=images[engine],
                profile=profile,
                engine=engine,
                output=phase_dir / "container-start.log",
            )
            pending_writes = []
            try:
                self.command(
                    ["docker", "inspect", name],
                    output=phase_dir / "container-inspect.json",
                )
                self.command(
                    [
                        "docker", "exec", name, "sh", "-ceu",
                        "printf '%s\\n' '[memory.current]'; "
                        "cat /sys/fs/cgroup/memory.current; "
                        "printf '%s\\n' '[memory.max]'; "
                        "cat /sys/fs/cgroup/memory.max; "
                        "printf '%s\\n' '[memory.stat]'; "
                        "cat /sys/fs/cgroup/memory.stat",
                    ],
                    output=phase_dir / "cgroup-memory.txt",
                )
                self.command(
                    [
                        "docker", "exec", name, "psql", "-U", "postgres",
                        "-XAt", "-F", "\t", "-c",
                        "SELECT name, setting, unit, source "
                        "FROM pg_settings ORDER BY name",
                    ],
                    output=phase_dir / "postgres-settings.tsv",
                )
                for cell in ordered_cells(group):
                    query_pack = (
                        self.tin_dir / "query-packs" / analyzer /
                        f"{cell['pack']}.json"
                    )
                    if not query_pack.is_file():
                        raise ConfigError(
                            f"enriched query pack is missing: {query_pack}"
                        )
                    sql = scenario_sql(
                        engine,
                        operation=cell["operation"],
                        family=cell["query_family"],
                        limit=cell["limit"],
                        shape=cell["sql_shape"],
                    )
                    result_dir = (
                        self.tin_dir / "results" / engine /
                        cell["cell_id"]
                    )
                    result_dir.mkdir(parents=True, exist_ok=True)
                    scenario = (
                        self.tin_dir / "scenarios" / engine /
                        f"{cell['cell_id']}.json"
                    )
                    scenario.parent.mkdir(parents=True, exist_ok=True)
                    self._write_or_validate_json(
                        scenario,
                        {
                            "operation": cell["operation"],
                            "query_family": cell["query_family"],
                            "sql_shape": cell["sql_shape"],
                            "selection_policy": (
                                "family-major-strided-v1"
                            ),
                            "plan_cache_mode": PLAN_CACHE_MODE,
                            "random_page_cost": RANDOM_PAGE_COST,
                            "kinds": list(
                                _family_kinds(cell["query_family"])
                            ),
                            "sql": sql,
                        },
                    )
                    source_image = {
                        "source": (
                            PG_TEXTSEARCH_SHA
                            if engine == "pg_textsearch"
                            else PARADEDB_TAG
                        ),
                        "image": images[engine],
                    }
                    fingerprint = self.cell_fingerprint(
                        cell=cell,
                        engine=engine,
                        script=script,
                        query_pack=query_pack,
                        images_lock=(
                            self.artifacts / "tin-images.lock.json"
                        ),
                        analyzer=json.loads(
                            build_fingerprint.read_text()
                        ),
                        sql_templates=sql,
                        source_image=source_image,
                    )
                    fingerprint["phase_resources"] = PROFILES[profile]
                    hook_sha = self.cache_reset_hook_sha256()
                    if cell["operation"] == "concurrent-writes":
                        cell_hash = hashlib.sha256(
                            cell["cell_id"].encode()
                        ).hexdigest()[:12]
                        write_volume = self._volume_name(
                            "write",
                            engine,
                            f"{analyzer}-{profile}-{cell_hash}",
                        )
                        write_labels = {
                            "kind": "write-attempt",
                            "engine": engine,
                            "analyzer": analyzer,
                            "profile": profile,
                            "cell": cell["cell_id"],
                        }
                        fingerprint["write_clone"] = {
                            "source": source_volume,
                            "target": write_volume,
                            "labels": write_labels,
                            "source_labels": {
                                "kind": "analyzer",
                                "engine": engine,
                                "analyzer": analyzer,
                            },
                            "image": images[engine],
                        }
                    else:
                        fingerprint["profile_clone"] = json.loads(
                            (phase_dir / "clone.json").read_text()
                        )
                    fingerprint["cache_reset_hook_sha256"] = hook_sha
                    audits = []
                    if engine == "pg_textsearch":
                        for kind in _family_kinds(
                            cell["query_family"]
                        ):
                            audits.append(
                                self._audit_native_queries(
                                    container=name,
                                    phase_dir=phase_dir,
                                    phase=phase,
                                    profile=profile,
                                    analyzer=analyzer,
                                    pack=cell["pack"],
                                    operation=cell["operation"],
                                    kind=kind,
                                    shape=cell["sql_shape"],
                                    statement=sql[kind],
                                    query_pack=query_pack,
                                )
                            )
                    fingerprint["plan_audit_protocol"] = (
                        "all-query-custom-plan-v1"
                    )
                    fingerprint["plan_audits"] = [
                        {
                            "audit_key": audit["audit_key"],
                            "audited_queries": audit["audited_queries"],
                            "classifications": audit["classifications"],
                            "timing_allowed": audit["timing_allowed"],
                        }
                        for audit in audits
                    ]
                    complete = result_dir / "complete.json"
                    if complete.is_file():
                        self._write_or_validate_json(
                            complete, fingerprint
                        )
                        if (
                            cell["operation"]
                            == "concurrent-writes"
                        ):
                            quality = self._record_write_quality(
                                result_dir=result_dir,
                                cell=cell,
                                engine=engine,
                                duration=(
                                    "3s" if self.smoke else "5m"
                                ),
                                sql_status="ok",
                            )
                            pressure.append(quality)
                            clone = fingerprint["write_clone"]
                            try:
                                self._remove_owned_write_volume(
                                    clone["target"], clone["labels"]
                                )
                            except (
                                ConfigError,
                                OSError,
                                subprocess.CalledProcessError,
                            ) as error:
                                self._write_or_validate_json(
                                    result_dir
                                    / "write-cleanup-failed.json",
                                    {
                                        "retention_requested": True,
                                        "volume": clone["target"],
                                        "labels": clone["labels"],
                                        "reason": (
                                            "resumed-write-cleanup-"
                                            "failed"
                                        ),
                                        "error": str(error),
                                    },
                                )
                                raise
                            self._write_or_validate_json(
                                result_dir / "write-cleanup.json",
                                {
                                    "removed": True,
                                    "volume": clone["target"],
                                    "labels": clone["labels"],
                                    "reason": (
                                        "completed-write-artifacts-"
                                        "captured"
                                    ),
                                },
                            )
                        continue
                    if any(
                        not audit["timing_allowed"] for audit in audits
                    ):
                        failure = {
                            "engine": engine,
                            "cell_id": cell["cell_id"],
                            "kind": "all_query_plan_audit_failed",
                            "audits": [
                                {
                                    key: value
                                    for key, value in audit.items()
                                    if key != "records"
                                }
                                for audit in audits
                            ],
                            "fingerprint": fingerprint,
                        }
                        self._write_or_validate_json(
                            result_dir / "plan-audit-failed.json",
                            failure,
                        )
                        failures.append(failure)
                        continue
                    probe = self._probe_cell(
                        name,
                        engine,
                        cell,
                        query_pack,
                        sql,
                        result_dir,
                    )
                    self._write_or_validate_json(
                        result_dir / "probes/summary.json", probe
                    )
                    if not probe["timing_allowed"]:
                        failure = {
                            "engine": engine,
                            "cell_id": cell["cell_id"],
                            "kind": "non_indexed_count_plan",
                            "probe": "probes/summary.json",
                            "fingerprint": fingerprint,
                        }
                        self._write_or_validate_json(
                            result_dir / "plan-gate-failed.json",
                            failure,
                        )
                        failures.append(failure)
                        continue
                    run_env = self._run_environment(
                        engine=engine,
                        cell=cell,
                        query_pack=query_pack,
                        scenario=scenario,
                        result_dir=result_dir,
                    )
                    if cell["operation"] == "concurrent-writes":
                        pending_writes.append({
                            "cell": cell,
                            "result_dir": result_dir,
                            "run_env": run_env,
                            "fingerprint": fingerprint,
                        })
                        continue
                    result = self._execute_k6_cell(
                        self.tools / "benchmarker",
                        script,
                        run_env,
                        result_dir,
                        fingerprint,
                    )
                    if result["status"] != "ok":
                        failures.append({
                            "engine": engine,
                            "cell_id": cell["cell_id"],
                            "exit_code": result["exit_code"],
                        })
            finally:
                self._stop_database(name, phase_dir / "postgres.log")
            for pending in pending_writes:
                cell = pending["cell"]
                result_dir = pending["result_dir"]
                fingerprint = pending["fingerprint"]
                write_volume, attempt = self._prepare_write_volume(
                    engine=engine,
                    analyzer=analyzer,
                    profile=profile,
                    cell_id=cell["cell_id"],
                    source_volume=source_volume,
                    image=images[engine],
                    result_dir=result_dir,
                )
                clone = json.loads(
                    (attempt / "clone.json").read_text()
                )
                if clone != fingerprint["write_clone"]:
                    self._write_or_validate_json(
                        attempt / "retention.json",
                        {
                            "retained": True,
                            "volume": write_volume,
                            "labels": clone.get("labels"),
                            "reason": "write-clone-fingerprint-mismatch",
                        },
                    )
                    raise ConfigError(
                        f"write clone fingerprint mismatch: {attempt}"
                    )
                write_phase = (
                    f"{phase}.{cell['cell_id']}.attempt-{attempt.name}"
                )
                try:
                    write_cache = self.require_cache_reset(
                        write_phase, write_volume
                    )
                except (
                    ConfigError,
                    OSError,
                    subprocess.CalledProcessError,
                ) as error:
                    self._write_or_validate_json(
                        attempt / "retention.json",
                        {
                            "retained": True,
                            "volume": write_volume,
                            "labels": clone["labels"],
                            "reason": "write-cache-reset-failed",
                            "error": str(error),
                        },
                    )
                    raise
                (attempt / "cache-reset.json").write_text(
                    json.dumps(write_cache, indent=2) + "\n"
                )
                write_hash = hashlib.sha256(
                    cell["cell_id"].encode()
                ).hexdigest()[:12]
                write_name = (
                    f"pgts-tin-write-{engine.replace('_', '-')}-"
                    f"{write_hash}"
                )
                try:
                    self._start_database(
                        name=write_name,
                        volume=write_volume,
                        image=images[engine],
                        profile=profile,
                        engine=engine,
                        output=attempt / "container-start.log",
                    )
                except (
                    ConfigError,
                    OSError,
                    subprocess.CalledProcessError,
                ) as error:
                    self._write_or_validate_json(
                        attempt / "retention.json",
                        {
                            "retained": True,
                            "volume": write_volume,
                            "labels": clone["labels"],
                            "reason": "write-container-start-failed",
                            "error": str(error),
                        },
                    )
                    raise
                attempt_error = None
                result = None
                try:
                    self.command(
                        ["docker", "inspect", write_name],
                        output=attempt / "container-inspect.json",
                    )
                    self.command(
                        [
                            "docker", "exec", write_name, "sh", "-ceu",
                            "printf '%s\\n' '[memory.current]'; "
                            "cat /sys/fs/cgroup/memory.current; "
                            "printf '%s\\n' '[memory.max]'; "
                            "cat /sys/fs/cgroup/memory.max; "
                            "printf '%s\\n' '[memory.stat]'; "
                            "cat /sys/fs/cgroup/memory.stat",
                        ],
                        output=attempt / "cgroup-memory.txt",
                    )
                    result = self._execute_k6_cell(
                        self.tools / "benchmarker",
                        script,
                        pending["run_env"],
                        result_dir,
                        fingerprint,
                    )
                    (attempt / "k6-result.json").write_text(
                        json.dumps(result, indent=2) + "\n"
                    )
                except (
                    ConfigError,
                    OSError,
                    subprocess.CalledProcessError,
                ) as error:
                    attempt_error = error
                finally:
                    try:
                        self._stop_database(
                            write_name, attempt / "postgres.log"
                        )
                    except (
                        ConfigError,
                        OSError,
                        subprocess.CalledProcessError,
                    ) as error:
                        self._write_or_validate_json(
                            attempt / "retention.json",
                            {
                                "retained": True,
                                "volume": write_volume,
                                "labels": clone["labels"],
                                "reason": "container-stop-failed",
                                "error": str(error),
                                "execution_error": (
                                    str(attempt_error)
                                    if attempt_error is not None
                                    else None
                                ),
                            },
                        )
                        raise
                if attempt_error is not None:
                    self._write_or_validate_json(
                        attempt / "retention.json",
                        {
                            "retained": True,
                            "volume": write_volume,
                            "labels": clone["labels"],
                            "reason": "write-execution-exception",
                            "error": str(attempt_error),
                        },
                    )
                    raise attempt_error
                if result["status"] == "ok":
                    try:
                        quality = self._record_write_quality(
                            result_dir=result_dir,
                            cell=cell,
                            engine=engine,
                            duration=pending["run_env"]["DURATION"],
                            sql_status="ok",
                        )
                    except (ConfigError, OSError) as error:
                        self._write_or_validate_json(
                            attempt / "retention.json",
                            {
                                "retained": True,
                                "volume": write_volume,
                                "labels": clone["labels"],
                                "reason": (
                                    "write-artifact-validation-failed"
                                ),
                                "error": str(error),
                            },
                        )
                        raise
                    pressure.append(quality)
                    try:
                        self._remove_owned_write_volume(
                            write_volume, clone["labels"]
                        )
                    except (
                        ConfigError,
                        OSError,
                        subprocess.CalledProcessError,
                    ) as error:
                        self._write_or_validate_json(
                            attempt / "retention.json",
                            {
                                "retention_requested": True,
                                "volume": write_volume,
                                "labels": clone["labels"],
                                "reason": "write-cleanup-failed",
                                "error": str(error),
                            },
                        )
                        raise
                    self._write_or_validate_json(
                        result_dir / "write-cleanup.json",
                        {
                            "removed": True,
                            "volume": write_volume,
                            "labels": clone["labels"],
                            "reason": (
                                "completed-write-artifacts-captured"
                            ),
                        },
                    )
                else:
                    self._write_or_validate_json(
                        attempt / "retention.json",
                        {
                            "retained": True,
                            "volume": write_volume,
                            "labels": clone["labels"],
                            "reason": "failed-write-cell-forensics",
                            "sql_status": result["status"],
                            "exit_code": result["exit_code"],
                        },
                    )
                    summary_path = result_dir / "k6-summary.json"
                    if summary_path.is_file():
                        quality = load_quality(
                            json.loads(summary_path.read_text()),
                            update_rate=cell["update_rate"],
                            duration=pending["run_env"]["DURATION"],
                        )
                    else:
                        quality = {
                            "target_rate": cell["update_rate"],
                            "duration": pending["run_env"]["DURATION"],
                            "status": "unavailable",
                            "target_met": False,
                            "unequal_pressure": True,
                        }
                    quality["engine"] = engine
                    quality["cell_id"] = cell["cell_id"]
                    quality["sql_status"] = result["status"]
                    self._write_or_validate_json(
                        result_dir / "load-quality.json", quality
                    )
                    pressure.append(quality)
                    failures.append({
                        "engine": engine,
                        "cell_id": cell["cell_id"],
                        "exit_code": result["exit_code"],
                        "load_quality": quality,
                    })
        summary = {
            "status": "failed" if failures else "ok",
            "failures": failures,
            "selected_cells": len(cells),
            "engines": engines,
            "images": image_lock,
            "load_quality": pressure,
        }
        path = self.tin_dir / "summary.json"
        temporary = path.with_suffix(".json.partial")
        temporary.write_text(json.dumps(summary, indent=2) + "\n")
        temporary.replace(path)
        if failures:
            raise ConfigError(
                f"TIN campaign has {len(failures)} failed cells"
            )

    @staticmethod
    def cell_fingerprint(
        *,
        cell,
        engine,
        script,
        query_pack,
        images_lock,
        analyzer,
        sql_templates,
        source_image,
    ):
        return {
            "format": 1,
            "cell": cell,
            "engine": engine,
            "analyzer": analyzer,
            "profile_resources": PROFILES[cell["profile"]],
            "build_resources": BUILD_RESOURCES,
            "database_cpus": DATABASE_CPUS,
            "client_cpus": CLIENT_CPUS,
            "sql_templates": sql_templates,
            "probe_protocol": "prepared-explain-custom-plan-v2",
            "plan_audit_protocol": "all-query-custom-plan-v1",
            "plan_cache_mode": PLAN_CACHE_MODE,
            "random_page_cost": RANDOM_PAGE_COST,
            "selection_policy": "family-major-strided-v1",
            "load_quality_protocol": "reconciled-flat-k6-counters-v2",
            "source_image": source_image,
            "script_sha256": file_sha256(script),
            "query_pack_sha256": file_sha256(query_pack),
            "images_lock_sha256": file_sha256(images_lock),
            "benchmarker_sha": BENCHMARKER_SHA,
        }


def _selector(parser, name, choices):
    destination = {
        "profile": "profiles",
        "operation": "operations",
        "pack": "packs",
        "analyzer": "analyzers",
        "family": "families",
        "engine": "engines",
    }[name]
    parser.add_argument(
        f"--{name}",
        action="append",
        choices=choices,
        dest=destination,
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "stage",
        choices=(
            "plan",
            "install",
            "prepare",
            "prepared",
            "build",
            "run",
            "all",
            "status",
        ),
    )
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument(
        "--dataset",
        choices=("msmarco-v2", "wikipedia"),
        default="msmarco-v2",
    )
    parser.add_argument("--dataset-path", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--sql-shape",
        choices=("rank_first", "filter_first", "original"),
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        default=Path(
            os.environ.get(
                "PGTS_SCALE_ROOT",
                "/srv/searchbench/pg_textsearch-scale",
            )
        ),
    )
    _selector(parser, "profile", PROFILES)
    _selector(parser, "operation", (
        "search", "count", "concurrent-writes"
    ))
    _selector(parser, "pack", ("natural", "spans"))
    _selector(parser, "analyzer", ANALYZERS)
    _selector(parser, "engine", ("pg_textsearch", "paradedb"))
    _selector(parser, "family", (
        "disjunction",
        "conjunction",
        "phrase",
        "interleaved-all",
        "interleaved-conjunction-phrase",
    ))
    args = parser.parse_args(argv)
    accepted = os.environ.get("MSMARCO_ACCEPT_TERMS", "").lower() in {
        "1", "true", "yes",
    }
    try:
        plan = build_tin_plan(
            smoke=args.smoke,
            accept_terms=accepted,
            dataset_kind=args.dataset,
        )
        plan["selected_cells"] = select_cells(
            plan["cells"],
            profiles=set(args.profiles or ()),
            operations=set(args.operations or ()),
            packs=set(args.packs or ()),
            analyzers=set(args.analyzers or ()),
            families=set(args.families or ()),
        )
        if args.sql_shape:
            plan["selected_cells"] = apply_sql_shape(
                plan["selected_cells"], args.sql_shape
            )
        if args.stage == "plan" or args.dry_run:
            print(json.dumps(plan, indent=2))
            return 0
        validate_storage(args.artifacts, smoke=args.smoke)
        runner = TinRunner(
            REPO_ROOT,
            args.artifacts.resolve(),
            args.smoke,
            args.run_id,
            args.dataset,
            args.dataset_path,
        )
        runner._write_or_validate_json(runner.tin_dir / "plan.json", plan)
        if args.stage == "status":
            print(runner.tin_dir)
            return 0
        if args.stage in {"install", "all"}:
            runner.install()
        if args.stage in {"prepare", "all"}:
            runner.prepare()
        engines = args.engines or ["pg_textsearch", "paradedb"]
        analyzers = args.analyzers or list(ANALYZERS)
        if args.stage == "prepared":
            for engine in engines:
                runner.build_base(engine)
        if args.stage in {"build", "all"}:
            runner.build_selected(engines, analyzers)
        if args.stage in {"run", "all"}:
            runner.run_cells(engines, plan["selected_cells"])
        return 0
    except (
        ConfigError,
        OSError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
