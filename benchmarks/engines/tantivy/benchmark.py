#!/usr/bin/env python3
"""
Tantivy Benchmark for MS-MARCO Dataset

Benchmarks Tantivy BM25 search against the MS-MARCO passage collection.
Outputs metrics compatible with the competitive benchmark runner.

Usage:
    python benchmark.py --data-dir /path/to/msmarco/data [--index-dir ./tantivy_index]

Prerequisites:
    pip install tantivy
"""

import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path
from statistics import mean

try:
    import tantivy
except ImportError:
    print("Error: tantivy-py not installed. Run: pip install tantivy")
    sys.exit(1)


def load_passages(data_dir: Path):
    """Load passages from MS-MARCO collection.tsv"""
    collection_file = data_dir / "collection.tsv"
    if not collection_file.exists():
        raise FileNotFoundError(f"Collection file not found: {collection_file}")

    print(f"Loading passages from {collection_file}...")
    count = 0
    with open(collection_file, 'r', encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2:
                passage_id, passage_text = parts[0], parts[1]
                yield passage_id, passage_text
                count += 1
                if count % 1000000 == 0:
                    print(f"  Loaded {count:,} passages...")
    print(f"  Total: {count:,} passages")


def load_queries(data_dir: Path, limit: int = None):
    """Load queries from MS-MARCO queries.dev.tsv"""
    queries_file = data_dir / "queries.dev.tsv"
    if not queries_file.exists():
        raise FileNotFoundError(f"Queries file not found: {queries_file}")

    print(f"Loading queries from {queries_file}...")
    queries = []
    with open(queries_file, 'r', encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2:
                query_id, query_text = parts[0], parts[1]
                queries.append((query_id, query_text))
                if limit and len(queries) >= limit:
                    break
    print(f"  Loaded {len(queries):,} queries")
    return queries


def build_index(data_dir: Path, index_dir: Path):
    """Build Tantivy index from MS-MARCO passages"""
    # Remove existing index
    if index_dir.exists():
        shutil.rmtree(index_dir)
    index_dir.mkdir(parents=True)

    # Create schema
    schema_builder = tantivy.SchemaBuilder()
    schema_builder.add_text_field("passage_id", stored=True)
    schema_builder.add_text_field("passage_text", stored=True)
    schema = schema_builder.build()

    # Create index
    index = tantivy.Index(schema, path=str(index_dir))
    writer = index.writer(heap_size=512 * 1024 * 1024)  # 512MB heap

    print("Building Tantivy index...")
    start_time = time.perf_counter()
    doc_count = 0

    for passage_id, passage_text in load_passages(data_dir):
        doc = tantivy.Document()
        doc.add_text("passage_id", passage_id)
        doc.add_text("passage_text", passage_text)
        writer.add_document(doc)
        doc_count += 1

        # Commit periodically to manage memory
        if doc_count % 500000 == 0:
            writer.commit()
            print(f"  Committed {doc_count:,} documents...")

    writer.commit()
    writer.wait_merging_threads()

    build_time_ms = (time.perf_counter() - start_time) * 1000

    # Get index size
    index_size_bytes = sum(
        f.stat().st_size for f in index_dir.rglob('*') if f.is_file()
    )
    index_size_mb = index_size_bytes / (1024 * 1024)

    print(f"Index build complete:")
    print(f"  Documents: {doc_count:,}")
    print(f"  Build time: {build_time_ms:.2f} ms ({build_time_ms/1000:.1f} s)")
    print(f"  Index size: {index_size_mb:.2f} MB")

    return {
        "doc_count": doc_count,
        "build_time_ms": build_time_ms,
        "index_size_mb": index_size_mb
    }


def run_query_benchmarks(index_dir: Path, queries: list):
    """Run query benchmarks and collect latency metrics"""
    # Open index for searching
    schema_builder = tantivy.SchemaBuilder()
    schema_builder.add_text_field("passage_id", stored=True)
    schema_builder.add_text_field("passage_text", stored=True)
    schema = schema_builder.build()

    index = tantivy.Index(schema, path=str(index_dir))
    index.reload()
    searcher = index.searcher()

    results = {}

    # Warm-up
    print("Warming up...")
    for _, query_text in queries[:5]:
        query = index.parse_query(query_text, ["passage_text"])
        searcher.search(query, 10)

    # Full throughput test
    print(f"Running throughput test ({len(queries)} queries)...")
    start_time = time.perf_counter()
    for _, query_text in queries:
        query = index.parse_query(query_text, ["passage_text"])
        searcher.search(query, 10)
    total_time_ms = (time.perf_counter() - start_time) * 1000

    results["throughput"] = {
        "total_queries": len(queries),
        "total_ms": total_time_ms,
        "avg_ms": total_time_ms / len(queries),
        "qps": len(queries) / (total_time_ms / 1000)
    }
    print(f"  Throughput: {results['throughput']['qps']:.2f} QPS")

    # Query length analysis
    print("Running query length analysis...")
    length_buckets = {
        "1-2 words": [],
        "3-5 words": [],
        "6-8 words": [],
        "9+ words": []
    }

    for _, query_text in queries:
        word_count = len(query_text.split())
        if word_count <= 2:
            bucket = "1-2 words"
        elif word_count <= 5:
            bucket = "3-5 words"
        elif word_count <= 8:
            bucket = "6-8 words"
        else:
            bucket = "9+ words"

        start = time.perf_counter()
        query = index.parse_query(query_text, ["passage_text"])
        searcher.search(query, 10)
        latency_ms = (time.perf_counter() - start) * 1000
        length_buckets[bucket].append(latency_ms)

    results["by_length"] = {}
    for bucket, latencies in length_buckets.items():
        if latencies:
            results["by_length"][bucket] = {
                "count": len(latencies),
                "avg_ms": mean(latencies)
            }
            print(f"  {bucket}: {len(latencies)} queries, avg {mean(latencies):.3f} ms")

    # Latency percentiles (sample 100 random queries)
    print("Running latency profiling (100 random queries)...")
    import random
    sample_queries = random.sample(queries, min(100, len(queries)))
    latencies = []

    for _, query_text in sample_queries:
        start = time.perf_counter()
        query = index.parse_query(query_text, ["passage_text"])
        searcher.search(query, 10)
        latency_ms = (time.perf_counter() - start) * 1000
        latencies.append(latency_ms)

    latencies.sort()
    n = len(latencies)
    results["percentiles"] = {
        "min": latencies[0],
        "p50": latencies[int(n * 0.50)],
        "p90": latencies[int(n * 0.90)],
        "p95": latencies[int(n * 0.95)],
        "p99": latencies[min(int(n * 0.99), n - 1)],
        "max": latencies[-1],
        "avg": mean(latencies)
    }
    print(f"  P50: {results['percentiles']['p50']:.3f} ms")
    print(f"  P95: {results['percentiles']['p95']:.3f} ms")
    print(f"  P99: {results['percentiles']['p99']:.3f} ms")

    return results


def main():
    parser = argparse.ArgumentParser(description="Tantivy MS-MARCO Benchmark")
    parser.add_argument("--data-dir", required=True,
                        help="Path to MS-MARCO data directory")
    parser.add_argument("--index-dir", default="./tantivy_index",
                        help="Path for Tantivy index (default: ./tantivy_index)")
    parser.add_argument("--output", default="tantivy_results.json",
                        help="Output JSON file (default: tantivy_results.json)")
    parser.add_argument("--skip-index", action="store_true",
                        help="Skip index building (use existing index)")
    args = parser.parse_args()

    data_dir = Path(args.data_dir)
    index_dir = Path(args.index_dir)

    if not data_dir.exists():
        print(f"Error: Data directory not found: {data_dir}")
        sys.exit(1)

    print("=" * 60)
    print("Tantivy MS-MARCO Benchmark")
    print("=" * 60)

    results = {
        "engine": "tantivy",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    }

    # Build index
    if args.skip_index and index_dir.exists():
        print("Skipping index build (using existing index)")
        # Estimate doc count from index
        results["index"] = {"doc_count": "unknown", "build_time_ms": 0}
    else:
        index_results = build_index(data_dir, index_dir)
        results["index"] = index_results

    # Load queries
    queries = load_queries(data_dir)
    results["num_queries"] = len(queries)

    # Run query benchmarks
    print()
    query_results = run_query_benchmarks(index_dir, queries)
    results["queries"] = query_results

    # Save results
    with open(args.output, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {args.output}")

    # Print summary
    print()
    print("=" * 60)
    print("TANTIVY BENCHMARK SUMMARY")
    print("=" * 60)
    if "index" in results and results["index"].get("build_time_ms"):
        print(f"Index Build Time: {results['index']['build_time_ms']:.0f} ms")
        print(f"Index Size: {results['index'].get('index_size_mb', 0):.2f} MB")
    print(f"Throughput: {query_results['throughput']['qps']:.2f} QPS")
    print(f"Avg Latency: {query_results['throughput']['avg_ms']:.3f} ms")
    print(f"P50 Latency: {query_results['percentiles']['p50']:.3f} ms")
    print(f"P95 Latency: {query_results['percentiles']['p95']:.3f} ms")
    print(f"P99 Latency: {query_results['percentiles']['p99']:.3f} ms")


if __name__ == "__main__":
    main()
