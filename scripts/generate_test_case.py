#!/usr/bin/env python3
"""
Tapir Test Case Generator

This script generates SQL test cases and expected output files for Tapir BM25 scoring.
Given a set of documents and queries, it:
1. Calculates expected BM25 scores using Python's rank_bm25 library
2. Generates a .sql file with table creation, index creation, and test queries
3. Generates a .out file with expected results

Usage:
    python3 scripts/generate_test_case.py \
        --test-name my_test \
        --documents "hello world" "goodbye cruel world" \
        --queries "hello" "world" "cruel" \
        --k1 1.2 --b 0.75 \
        --text-config english
"""

import argparse
import sys
import os
from typing import List, Dict, Tuple
import numpy as np
from rank_bm25 import BM25Okapi
import re


def stem_text_postgresql(text: str, text_config: str = 'english', conn=None) -> List[str]:
    """
    Use PostgreSQL's actual to_tsvector function for text processing.
    This ensures exact matching with Tapir's behavior.
    """
    if conn is None:
        raise ValueError("Database connection required for PostgreSQL stemming")

    with conn.cursor() as cur:
        cur.execute(f"SELECT to_tsvector('{text_config}', %s)::text", (text,))
        tsvector_result = cur.fetchone()[0]

        # Parse tsvector format: 'term1:pos1,pos2 term2:pos3'
        terms = []
        for token in tsvector_result.split():
            if ':' in token:
                term = token.split(':')[0].strip("'")
                # Count term frequency from positions
                positions = token.split(':')[1].split(',')
                terms.extend([term] * len(positions))
            else:
                terms.append(token.strip("'"))

        return terms


def calculate_bm25_scores(documents: List[str], query: str, k1: float = 1.2, b: float = 0.75,
                         text_config: str = 'english', conn=None) -> List[float]:
    """Calculate BM25 scores for documents given a query using PostgreSQL stemming."""
    if conn is None:
        raise ValueError("Database connection required for PostgreSQL stemming")

    # Process documents using PostgreSQL
    processed_docs = [stem_text_postgresql(doc, text_config, conn) for doc in documents]

    # Process query using PostgreSQL
    processed_query = stem_text_postgresql(query, text_config, conn)

    # Create BM25 object with stemming disabled (since we already used PostgreSQL)
    bm25 = BM25Okapi(processed_docs, k1=k1, b=b)
    bm25.tokenizer = lambda x: x  # Disable additional tokenization

    # Note: BM25Okapi already handles epsilon * average_idf for negative IDFs

    # Calculate scores
    scores = bm25.get_scores(processed_query)

    # BM25 scores can be negative (when epsilon * average_idf is applied)
    # Tapir uses negative scores for PostgreSQL ASC ordering
    # So we need to handle both cases:
    # - Positive BM25 scores -> negate them
    # - Negative BM25 scores -> keep them negative (but may need to adjust sign)
    # Actually, for consistency, we should always return the negative of the absolute value
    # to ensure scores are negative for PostgreSQL ordering
    return [-abs(score) if score > 0 else score for score in scores]


def generate_sql_test(test_name: str, documents: List[str], queries: List[str],
                     k1: float = 1.2, b: float = 0.75, text_config: str = 'english',
                     db_config: Dict[str, str] = None) -> Tuple[str, str]:
    """
    Generate SQL test file and expected output with both index creation modes:
    1. Mode 1: Create table, insert data, then create index (bulk build)
    2. Mode 2: Create table, create index, then insert data (incremental build)

    Args:
        test_name: Name for the test case
        documents: List of document texts
        queries: List of query strings
        k1: BM25 k1 parameter
        b: BM25 b parameter
        text_config: PostgreSQL text search configuration
        db_config: Database connection parameters

    Returns:
        Tuple of (sql_content, expected_output_content)
    """
    # Establish database connection for stemming
    conn = None
    if db_config:
        try:
            import psycopg2
            conn = psycopg2.connect(**db_config)
        except Exception as e:
            print(f"Warning: Could not connect to database for stemming: {e}")
            print("Falling back to simplified stemming - results may not match exactly")

    sql_lines = []
    expected_lines = []

    # Header
    header = [
        f"-- Test case: {test_name}",
        f"-- Generated BM25 test with {len(documents)} documents and {len(queries)} queries",
        f"-- Testing both bulk build and incremental build modes",
        "CREATE EXTENSION IF NOT EXISTS pg_textsearch;",
        "SET pg_textsearch.log_scores = true;",
        "SET enable_seqscan = off;",
        ""
    ]
    sql_lines.extend(header)
    expected_lines.extend(header)

    # ===== MODE 1: Bulk build (insert data then create index) =====
    sql_lines.extend([
        "-- MODE 1: Bulk build (insert data, then create index)",
        f"CREATE TABLE {test_name}_bulk (",
        "    id SERIAL PRIMARY KEY,",
        "    content TEXT",
        ");",
        "",
        "-- Insert test documents"
    ])

    expected_lines.extend([
        "-- MODE 1: Bulk build (insert data, then create index)",
        f"CREATE TABLE {test_name}_bulk (",
        "    id SERIAL PRIMARY KEY,",
        "    content TEXT",
        ");",
        "",
        "-- Insert test documents"
    ])

    # Insert documents for bulk mode
    for i, doc in enumerate(documents, 1):
        escaped_doc = doc.replace("'", "''")
        sql_lines.append(f"INSERT INTO {test_name}_bulk (content) VALUES ('{escaped_doc}');")
        expected_lines.append(f"INSERT INTO {test_name}_bulk (content) VALUES ('{escaped_doc}');")
        expected_lines.append(f"INSERT 0 1")

    # Create index after data for bulk mode
    sql_lines.extend([
        "",
        f"-- Create index after data insertion (bulk build)",
        f"CREATE INDEX {test_name}_bulk_idx ON {test_name}_bulk USING bm25(content)",
        f"  WITH (text_config='{text_config}', k1={k1}, b={b});",
        ""
    ])

    expected_lines.extend([
        "",
        f"-- Create index after data insertion (bulk build)",
        f"CREATE INDEX {test_name}_bulk_idx ON {test_name}_bulk USING bm25(content)",
        f"  WITH (text_config='{text_config}', k1={k1}, b={b});",
        f"NOTICE:  BM25 index build started for relation {test_name}_bulk_idx",
        f"NOTICE:  Using text search configuration: {text_config}",
        f"NOTICE:  Using index options: k1={k1:.2f}, b={b:.2f}",
        f"NOTICE:  BM25 index build completed: {len(documents)} documents, avg_length=XXX, text_config='{text_config}' (k1={k1:.2f}, b={b:.2f})",
        ""
    ])

    # Run queries on bulk mode
    for query_idx, query in enumerate(queries):
        sql_lines.extend([
            f"-- Bulk mode query {query_idx + 1}: '{query}'",
            f"SELECT id, content, ROUND((content <@> to_bm25query('{query}', '{test_name}_bulk_idx'))::numeric, 4) as score",
            f"FROM {test_name}_bulk",
            f"ORDER BY content <@> to_bm25query('{query}', '{test_name}_bulk_idx'), id;",
            ""
        ])

        expected_lines.extend([
            f"-- Bulk mode query {query_idx + 1}: '{query}'",
            f"SELECT id, content, ROUND((content <@> to_bm25query('{query}', '{test_name}_bulk_idx'))::numeric, 4) as score",
            f"FROM {test_name}_bulk",
            f"ORDER BY content <@> to_bm25query('{query}', '{test_name}_bulk_idx'), id;"
        ])

        # Calculate expected scores
        if conn:
            scores = calculate_bm25_scores(documents, query, k1, b, text_config, conn)
        else:
            scores = [0.0] * len(documents)
            print(f"Warning: No database connection, using zero scores for query '{query}'")

        # Create results with document IDs and sort by score, then by ID
        results = []
        for i, (doc, score) in enumerate(zip(documents, scores), 1):
            results.append((i, doc, score))

        # Sort by score (ascending, since negative), then by ID
        results.sort(key=lambda x: (x[2], x[0]))

        # Add result table
        expected_lines.append(" id |        content        |  score  ")
        expected_lines.append("----+-----------------------+---------")

        # Add results
        for doc_id, content, score in results:
            # Format score to 4 decimal places
            if abs(score) < 0.0001:
                score_str = "0.0000"
            else:
                score_str = f"{score:.4f}"
            # Truncate content to fit column width
            display_content = content[:21]
            expected_lines.append(f"  {doc_id} | {display_content:<21} | {score_str:>7}")

        expected_lines.append(f"({len(results)} rows)")
        expected_lines.append("")

    # ===== MODE 2: Incremental build (create index then insert data) =====
    sql_lines.extend([
        "-- MODE 2: Incremental build (create index, then insert data)",
        f"CREATE TABLE {test_name}_incr (",
        "    id SERIAL PRIMARY KEY,",
        "    content TEXT",
        ");",
        "",
        f"-- Create index before data insertion (incremental build)",
        f"CREATE INDEX {test_name}_incr_idx ON {test_name}_incr USING bm25(content)",
        f"  WITH (text_config='{text_config}', k1={k1}, b={b});",
        "",
        "-- Insert test documents incrementally"
    ])

    expected_lines.extend([
        "-- MODE 2: Incremental build (create index, then insert data)",
        f"CREATE TABLE {test_name}_incr (",
        "    id SERIAL PRIMARY KEY,",
        "    content TEXT",
        ");",
        "",
        f"-- Create index before data insertion (incremental build)",
        f"CREATE INDEX {test_name}_incr_idx ON {test_name}_incr USING bm25(content)",
        f"  WITH (text_config='{text_config}', k1={k1}, b={b});",
        f"NOTICE:  BM25 index build started for relation {test_name}_incr_idx",
        f"NOTICE:  Using text search configuration: {text_config}",
        f"NOTICE:  Using index options: k1={k1:.2f}, b={b:.2f}",
        f"NOTICE:  BM25 index build completed: 0 documents, avg_length=XXX, text_config='{text_config}' (k1={k1:.2f}, b={b:.2f})",
        "",
        "-- Insert test documents incrementally"
    ])

    # Insert documents for incremental mode
    for i, doc in enumerate(documents, 1):
        escaped_doc = doc.replace("'", "''")
        sql_lines.append(f"INSERT INTO {test_name}_incr (content) VALUES ('{escaped_doc}');")
        expected_lines.append(f"INSERT INTO {test_name}_incr (content) VALUES ('{escaped_doc}');")
        expected_lines.append(f"INSERT 0 1")

    sql_lines.append("")
    expected_lines.append("")

    # Run queries on incremental mode
    for query_idx, query in enumerate(queries):
        sql_lines.extend([
            f"-- Incremental mode query {query_idx + 1}: '{query}'",
            f"SELECT id, content, ROUND((content <@> to_bm25query('{query}', '{test_name}_incr_idx'))::numeric, 4) as score",
            f"FROM {test_name}_incr",
            f"ORDER BY content <@> to_bm25query('{query}', '{test_name}_incr_idx'), id;",
            ""
        ])

        expected_lines.extend([
            f"-- Incremental mode query {query_idx + 1}: '{query}'",
            f"SELECT id, content, ROUND((content <@> to_bm25query('{query}', '{test_name}_incr_idx'))::numeric, 4) as score",
            f"FROM {test_name}_incr",
            f"ORDER BY content <@> to_bm25query('{query}', '{test_name}_incr_idx'), id;"
        ])

        # Calculate expected scores (same as bulk mode)
        if conn:
            scores = calculate_bm25_scores(documents, query, k1, b, text_config, conn)
        else:
            scores = [0.0] * len(documents)

        # Create results with document IDs and sort by score, then by ID
        results = []
        for i, (doc, score) in enumerate(zip(documents, scores), 1):
            results.append((i, doc, score))

        # Sort by score (ascending, since negative), then by ID
        results.sort(key=lambda x: (x[2], x[0]))

        # Add result table
        expected_lines.append(" id |        content        |  score  ")
        expected_lines.append("----+-----------------------+---------")

        # Add results
        for doc_id, content, score in results:
            # Format score to 4 decimal places
            if abs(score) < 0.0001:
                score_str = "0.0000"
            else:
                score_str = f"{score:.4f}"
            # Truncate content to fit column width
            display_content = content[:21]
            expected_lines.append(f"  {doc_id} | {display_content:<21} | {score_str:>7}")

        expected_lines.append(f"({len(results)} rows)")
        expected_lines.append("")

    # Cleanup
    sql_lines.extend([
        "-- Cleanup",
        f"DROP TABLE {test_name}_bulk CASCADE;",
        f"DROP TABLE {test_name}_incr CASCADE;",
        "DROP EXTENSION pg_textsearch CASCADE;"
    ])

    expected_lines.extend([
        "-- Cleanup",
        f"DROP TABLE {test_name}_bulk CASCADE;",
        f"DROP TABLE {test_name}_incr CASCADE;",
        "DROP EXTENSION pg_textsearch CASCADE;"
    ])

    return '\n'.join(sql_lines), '\n'.join(expected_lines)


def main():
    parser = argparse.ArgumentParser(description='Generate Tapir BM25 test cases')
    parser.add_argument('--test-name', required=True, help='Name for the test case')
    parser.add_argument('--documents', nargs='+', required=True, help='Test documents')
    parser.add_argument('--queries', nargs='+', required=True, help='Test queries')
    parser.add_argument('--k1', type=float, default=1.2, help='BM25 k1 parameter')
    parser.add_argument('--b', type=float, default=0.75, help='BM25 b parameter')
    parser.add_argument('--text-config', default='english', help='Text search configuration')
    parser.add_argument('--output-dir', default='test/sql', help='Output directory for SQL file')
    parser.add_argument('--expected-dir', default='test/expected', help='Output directory for expected file')

    # Database connection parameters
    parser.add_argument('--host', default='localhost', help='Database host')
    parser.add_argument('--port', type=int, default=5432, help='Database port')
    parser.add_argument('--dbname', help='Database name (required for accurate scoring)')
    parser.add_argument('--user', help='Database user')
    parser.add_argument('--password', help='Database password')

    args = parser.parse_args()

    # Build database config if provided
    db_config = None
    if args.dbname:
        db_config = {
            'host': args.host,
            'port': args.port,
            'dbname': args.dbname,
        }
        if args.user:
            db_config['user'] = args.user
        if args.password:
            db_config['password'] = args.password

    # Generate test case
    sql_content, expected_content = generate_sql_test(
        args.test_name, args.documents, args.queries,
        args.k1, args.b, args.text_config, db_config
    )

    # Ensure output directories exist
    os.makedirs(args.output_dir, exist_ok=True)
    os.makedirs(args.expected_dir, exist_ok=True)

    # Write SQL file
    sql_path = os.path.join(args.output_dir, f"{args.test_name}.sql")
    with open(sql_path, 'w') as f:
        f.write(sql_content)

    # Write expected output file
    expected_path = os.path.join(args.expected_dir, f"{args.test_name}.out")
    with open(expected_path, 'w') as f:
        f.write(expected_content)

    print(f"Generated test case '{args.test_name}':")
    print(f"  SQL file: {sql_path}")
    print(f"  Expected output file: {expected_path}")
    print(f"  Documents: {len(args.documents)}")
    print(f"  Queries: {len(args.queries)}")
    print(f"  Parameters: k1={args.k1}, b={args.b}, text_config='{args.text_config}'")
    print(f"  Modes: Bulk build and incremental build")


if __name__ == '__main__':
    main()
