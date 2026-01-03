#!/usr/bin/env python3
"""
OpenSearch Benchmark for MS-MARCO Dataset

Benchmarks OpenSearch (Lucene-based) BM25 search against MS-MARCO passages.
Outputs metrics compatible with the competitive benchmark runner.

Usage:
    # Start OpenSearch first:
    docker-compose up -d

    # Run benchmark:
    python benchmark.py --data-dir /path/to/msmarco/data

Prerequisites:
    pip install opensearch-py
"""

import argparse
import json
import sys
import time
from pathlib import Path
from statistics import mean

try:
    from opensearchpy import OpenSearch
    from opensearchpy.helpers import bulk
except ImportError:
    print("Error: opensearch-py not installed. Run: pip install opensearch-py")
    sys.exit(1)


def get_client(host: str = "localhost", port: int = 9200):
    """Create OpenSearch client"""
    return OpenSearch(
        hosts=[{"host": host, "port": port}],
        http_compress=True,
        timeout=300  # 5 minute timeout for bulk operations
    )


def wait_for_opensearch(client, max_retries: int = 30):
    """Wait for OpenSearch to be ready"""
    print("Waiting for OpenSearch to be ready...")
    for i in range(max_retries):
        try:
            info = client.info()
            print(f"  Connected to OpenSearch {info['version']['number']}")
            return True
        except Exception:
            time.sleep(2)
    raise RuntimeError("OpenSearch not available after waiting")


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


def create_index(client, index_name: str = "msmarco"):
    """Create OpenSearch index with BM25 settings"""
    # Delete existing index
    if client.indices.exists(index=index_name):
        print(f"Deleting existing index: {index_name}")
        client.indices.delete(index=index_name)

    # Create index with BM25 similarity (default in OpenSearch)
    settings = {
        "settings": {
            "number_of_shards": 1,
            "number_of_replicas": 0,
            "refresh_interval": "-1",  # Disable refresh during indexing
            "index": {
                "similarity": {
                    "default": {
                        "type": "BM25",
                        "k1": 1.2,
                        "b": 0.75
                    }
                }
            }
        },
        "mappings": {
            "properties": {
                "passage_id": {"type": "keyword"},
                "passage_text": {"type": "text", "analyzer": "english"}
            }
        }
    }

    print(f"Creating index: {index_name}")
    client.indices.create(index=index_name, body=settings)


def build_index(client, data_dir: Path, index_name: str = "msmarco"):
    """Build OpenSearch index from MS-MARCO passages"""
    create_index(client, index_name)

    def generate_actions():
        for passage_id, passage_text in load_passages(data_dir):
            yield {
                "_index": index_name,
                "_id": passage_id,
                "_source": {
                    "passage_id": passage_id,
                    "passage_text": passage_text
                }
            }

    print("Indexing documents...")
    start_time = time.perf_counter()

    # Bulk index with progress tracking
    doc_count = 0
    batch_size = 10000

    def batched(iterable, n):
        batch = []
        for item in iterable:
            batch.append(item)
            if len(batch) >= n:
                yield batch
                batch = []
        if batch:
            yield batch

    for batch in batched(generate_actions(), batch_size):
        success, _ = bulk(client, batch)
        doc_count += success
        if doc_count % 500000 == 0:
            print(f"  Indexed {doc_count:,} documents...")

    # Re-enable refresh and force merge
    print("Finalizing index...")
    client.indices.put_settings(
        index=index_name,
        body={"refresh_interval": "1s"}
    )
    client.indices.refresh(index=index_name)
    client.indices.forcemerge(index=index_name, max_num_segments=1)

    build_time_ms = (time.perf_counter() - start_time) * 1000

    # Get index stats
    stats = client.indices.stats(index=index_name)
    index_size_bytes = stats["indices"][index_name]["total"]["store"]["size_in_bytes"]
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


def run_query_benchmarks(client, queries: list, index_name: str = "msmarco"):
    """Run query benchmarks and collect latency metrics"""
    results = {}

    # Warm-up
    print("Warming up...")
    for _, query_text in queries[:5]:
        client.search(
            index=index_name,
            body={"query": {"match": {"passage_text": query_text}}, "size": 10}
        )

    # Full throughput test
    print(f"Running throughput test ({len(queries)} queries)...")
    start_time = time.perf_counter()
    for _, query_text in queries:
        client.search(
            index=index_name,
            body={"query": {"match": {"passage_text": query_text}}, "size": 10}
        )
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
        client.search(
            index=index_name,
            body={"query": {"match": {"passage_text": query_text}}, "size": 10}
        )
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
        client.search(
            index=index_name,
            body={"query": {"match": {"passage_text": query_text}}, "size": 10}
        )
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
    parser = argparse.ArgumentParser(description="OpenSearch MS-MARCO Benchmark")
    parser.add_argument("--data-dir", required=True,
                        help="Path to MS-MARCO data directory")
    parser.add_argument("--host", default="localhost",
                        help="OpenSearch host (default: localhost)")
    parser.add_argument("--port", type=int, default=9200,
                        help="OpenSearch port (default: 9200)")
    parser.add_argument("--output", default="opensearch_results.json",
                        help="Output JSON file (default: opensearch_results.json)")
    parser.add_argument("--skip-index", action="store_true",
                        help="Skip index building (use existing index)")
    args = parser.parse_args()

    data_dir = Path(args.data_dir)

    if not data_dir.exists():
        print(f"Error: Data directory not found: {data_dir}")
        sys.exit(1)

    print("=" * 60)
    print("OpenSearch MS-MARCO Benchmark")
    print("=" * 60)

    # Connect to OpenSearch
    client = get_client(args.host, args.port)
    wait_for_opensearch(client)

    results = {
        "engine": "opensearch",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    }

    # Build index
    if args.skip_index and client.indices.exists(index="msmarco"):
        print("Skipping index build (using existing index)")
        stats = client.indices.stats(index="msmarco")
        doc_count = stats["indices"]["msmarco"]["total"]["docs"]["count"]
        results["index"] = {"doc_count": doc_count, "build_time_ms": 0}
    else:
        index_results = build_index(client, data_dir)
        results["index"] = index_results

    # Load queries
    queries = load_queries(data_dir)
    results["num_queries"] = len(queries)

    # Run query benchmarks
    print()
    query_results = run_query_benchmarks(client, queries)
    results["queries"] = query_results

    # Save results
    with open(args.output, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {args.output}")

    # Print summary
    print()
    print("=" * 60)
    print("OPENSEARCH BENCHMARK SUMMARY")
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
