#!/usr/bin/env python3
"""
Aggregate competitive benchmark results into a comparison JSON.

Reads individual engine result files and produces a unified comparison
summary suitable for analysis and reporting.

Usage:
    python compare_results.py --output-dir ./results --timestamp "2025-01-15T10:30:00Z"
"""

import argparse
import json
import os
from pathlib import Path


def load_engine_results(output_dir: Path) -> dict:
    """Load all engine result files from output directory."""
    engines = {}

    engine_files = {
        "pg_textsearch": "pg_textsearch_results.json",
        "paradedb": "paradedb_results.json",
        "tantivy": "tantivy_results.json",
        "opensearch": "opensearch_results.json",
    }

    for engine, filename in engine_files.items():
        filepath = output_dir / filename
        if filepath.exists():
            try:
                with open(filepath, 'r') as f:
                    engines[engine] = json.load(f)
                print(f"  Loaded {engine} results")
            except (json.JSONDecodeError, IOError) as e:
                print(f"  Warning: Failed to load {engine} results: {e}")

    return engines


def extract_metrics(engine_data: dict) -> dict:
    """Extract standardized metrics from engine results."""
    metrics = {}

    # Index metrics
    if "index" in engine_data:
        idx = engine_data["index"]
        if "build_time_ms" in idx:
            metrics["index_build_ms"] = idx["build_time_ms"]
        if "index_size_mb" in idx:
            metrics["index_size_mb"] = idx["index_size_mb"]
        if "doc_count" in idx:
            metrics["doc_count"] = idx["doc_count"]

    # Query metrics
    if "queries" in engine_data:
        q = engine_data["queries"]

        # Throughput
        if "throughput" in q:
            t = q["throughput"]
            if "qps" in t:
                metrics["throughput_qps"] = t["qps"]
            if "avg_ms" in t:
                metrics["query_avg_ms"] = t["avg_ms"]

        # Percentiles
        if "percentiles" in q:
            p = q["percentiles"]
            for key in ["p50", "p90", "p95", "p99", "min", "max"]:
                if key in p:
                    metrics[f"query_{key}_ms"] = p[key]

    return metrics


def create_comparison(engines: dict, timestamp: str) -> dict:
    """Create unified comparison JSON."""
    comparison = {
        "timestamp": timestamp,
        "dataset": "msmarco",
        "engines": list(engines.keys()),
        "results": {},
    }

    # Extract metrics for each engine
    for engine, data in engines.items():
        comparison["results"][engine] = extract_metrics(data)

    # Add document count from first available source
    for engine, data in engines.items():
        if "index" in data and "doc_count" in data["index"]:
            comparison["num_documents"] = data["index"]["doc_count"]
            break

    return comparison


def print_summary(comparison: dict):
    """Print human-readable comparison summary."""
    print("\n" + "=" * 70)
    print("COMPETITIVE BENCHMARK COMPARISON")
    print("=" * 70)
    print(f"Timestamp: {comparison['timestamp']}")
    print(f"Dataset: {comparison['dataset']}")
    if "num_documents" in comparison:
        print(f"Documents: {comparison['num_documents']:,}")
    print()

    # Build comparison table
    engines = comparison["engines"]
    results = comparison["results"]

    # Header
    header = "| Metric           |"
    for engine in engines:
        header += f" {engine:>14} |"
    print(header)
    print("|" + "-" * 18 + "|" + (("-" * 16 + "|") * len(engines)))

    # Metrics to display
    metric_labels = [
        ("index_build_ms", "Index Build (ms)"),
        ("index_size_mb", "Index Size (MB)"),
        ("throughput_qps", "Throughput (QPS)"),
        ("query_avg_ms", "Avg Latency (ms)"),
        ("query_p50_ms", "P50 Latency (ms)"),
        ("query_p95_ms", "P95 Latency (ms)"),
        ("query_p99_ms", "P99 Latency (ms)"),
    ]

    for metric_key, metric_label in metric_labels:
        row = f"| {metric_label:<16} |"
        for engine in engines:
            value = results.get(engine, {}).get(metric_key)
            if value is not None:
                if isinstance(value, float):
                    row += f" {value:>14.2f} |"
                else:
                    row += f" {value:>14} |"
            else:
                row += f" {'N/A':>14} |"
        print(row)

    print()


def main():
    parser = argparse.ArgumentParser(
        description="Aggregate competitive benchmark results"
    )
    parser.add_argument(
        "--output-dir", required=True,
        help="Directory containing engine result files"
    )
    parser.add_argument(
        "--timestamp", required=True,
        help="Benchmark timestamp (ISO format)"
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir)

    if not output_dir.exists():
        print(f"Error: Output directory not found: {output_dir}")
        return 1

    print("Loading engine results...")
    engines = load_engine_results(output_dir)

    if not engines:
        print("No engine results found to aggregate")
        return 1

    print(f"\nAggregating results from {len(engines)} engines...")
    comparison = create_comparison(engines, args.timestamp)

    # Save comparison JSON
    output_file = output_dir / "comparison_results.json"
    with open(output_file, 'w') as f:
        json.dump(comparison, f, indent=2)
    print(f"Comparison saved to: {output_file}")

    # Print summary
    print_summary(comparison)

    return 0


if __name__ == "__main__":
    exit(main())
