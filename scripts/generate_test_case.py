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


def _apply_epsilon_idf_handling(bm25, processed_docs):
    """
    Apply epsilon handling to match Tapir's two-step approach:
    1. Calculate IDF sum using simple epsilon (0.25) for negative IDFs (like index building)
    2. Calculate average IDF from that sum
    3. Use epsilon * average_idf for negative IDFs during scoring (like query time)
    """
    import math

    # Step 1: Recalculate IDF sum using Tapir's index building logic
    idf_sum = 0.0
    term_count = 0

    for term in bm25.idf.keys():
        df = sum(1 for doc in processed_docs if term in doc)
        N = len(processed_docs)
        raw_idf = math.log((N - df + 0.5) / (df + 0.5))

        # Use simple epsilon like Tapir's tp_calculate_idf() during index building
        if raw_idf < 0:
            idf = 0.25  # Simple epsilon
        else:
            idf = raw_idf

        idf_sum += idf
        term_count += 1

    # Step 2: Calculate average IDF from the sum
    if term_count > 0:
        average_idf = idf_sum / term_count
    else:
        return

    # Step 3: Apply epsilon * average_idf for negative IDFs (like query time)
    epsilon = 0.25
    for term in bm25.idf.keys():
        df = sum(1 for doc in processed_docs if term in doc)
        N = len(processed_docs)
        raw_idf = math.log((N - df + 0.5) / (df + 0.5))

        if raw_idf < 0:
            # Use epsilon * average_idf like Tapir's tp_calculate_idf_with_epsilon()
            bm25.idf[term] = epsilon * average_idf
        else:
            bm25.idf[term] = raw_idf


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

    # Apply epsilon handling for negative IDF values like Tapir
    _apply_epsilon_idf_handling(bm25, processed_docs)

    # Calculate scores
    scores = bm25.get_scores(processed_query)

    # Convert to negative scores (Tapir convention for PostgreSQL ASC ordering)
    return [-score for score in scores]


def generate_sql_test(test_name: str, documents: List[str], queries: List[str],
                     k1: float = 1.2, b: float = 0.75, text_config: str = 'english',
                     db_config: Dict[str, str] = None) -> Tuple[str, str]:
    """
    Generate SQL test file and expected output.

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

    # Generate SQL content
    sql_lines = [
        f"-- Test case: {test_name}",
        f"-- Generated BM25 test with {len(documents)} documents and {len(queries)} queries",
        "CREATE EXTENSION IF NOT EXISTS tapir;",
        "SET tapir.log_scores = true;",
        "SET enable_seqscan = off;",
        "",
        f"-- Create test table",
        f"CREATE TABLE {test_name}_docs (",
        "    id SERIAL PRIMARY KEY,",
        "    content TEXT",
        ");",
        "",
        "-- Insert test documents"
    ]

    for i, doc in enumerate(documents, 1):
        sql_lines.append(f"INSERT INTO {test_name}_docs (content) VALUES ('{doc.replace(chr(39), chr(39)+chr(39))}');")

    sql_lines.extend([
        "",
        f"-- Create Tapir index",
        f"CREATE INDEX {test_name}_idx ON {test_name}_docs USING tapir(content)",
        f"  WITH (text_config='{text_config}', k1={k1}, b={b});",
        ""
    ])

    # Generate expected output content
    expected_lines = [
        f"-- Test case: {test_name}",
        f"-- Generated BM25 test with {len(documents)} documents and {len(queries)} queries",
        "CREATE EXTENSION IF NOT EXISTS tapir;",
        "SET tapir.log_scores = true;",
        "SET enable_seqscan = off;",
        f"-- Create test table",
        f"CREATE TABLE {test_name}_docs (",
        "    id SERIAL PRIMARY KEY,",
        "    content TEXT",
        ");",
        ""
    ]

    # Add INSERT statements to expected output
    for i, doc in enumerate(documents, 1):
        expected_lines.append(f"INSERT INTO {test_name}_docs (content) VALUES ('{doc.replace(chr(39), chr(39)+chr(39))}');")
        expected_lines.append(f"INSERT 0 1")

    expected_lines.extend([
        f"-- Create Tapir index",
        f"CREATE INDEX {test_name}_idx ON {test_name}_docs USING tapir(content)",
        f"  WITH (text_config='{text_config}', k1={k1}, b={b});",
        f"NOTICE:  Tapir index build started for relation {test_name}_idx",
        f"NOTICE:  Using text search configuration: {text_config}",
        f"NOTICE:  Using index options: k1={k1:.2f}, b={b:.2f}",
        f"NOTICE:  Tapir index build completed: {len(documents)} documents, avg_length=XXX, text_config='{text_config}' (k1={k1:.2f}, b={b:.2f})",
        ""
    ])

    # Add queries and expected results
    for query_idx, query in enumerate(queries):
        sql_lines.extend([
            f"-- Test query {query_idx + 1}: '{query}'",
            f"SELECT id, content, ROUND((content <@> to_tpquery('{query}', '{test_name}_idx'))::numeric, 4) as score",
            f"FROM {test_name}_docs",
            f"ORDER BY content <@> to_tpquery('{query}', '{test_name}_idx'), id;",
            ""
        ])

        expected_lines.extend([
            f"-- Test query {query_idx + 1}: '{query}'",
            f"SELECT id, content, ROUND((content <@> to_tpquery('{query}', '{test_name}_idx'))::numeric, 4) as score",
            f"FROM {test_name}_docs",
            f"ORDER BY content <@> to_tpquery('{query}', '{test_name}_idx'), id;"
        ])

        # Calculate expected scores
        if conn:
            scores = calculate_bm25_scores(documents, query, k1, b, text_config, conn)
        else:
            # Fallback: use zero scores as placeholder
            scores = [0.0] * len(documents)
            print(f"Warning: No database connection, using zero scores for query '{query}'")

        # Create results with document IDs and sort by score, then by ID
        results = []
        for i, (doc, score) in enumerate(zip(documents, scores), 1):
            results.append((i, doc, score))

        # Sort by score (ascending, since negative), then by ID
        results.sort(key=lambda x: (x[2], x[0]))

        # Add result table header
        expected_lines.append(" id |        content        |  score  ")
        expected_lines.append("----+-----------------------+---------")

        # Add results
        for doc_id, content, score in results:
            # Format score to 4 decimal places, handling -0.0000 case
            if abs(score) < 0.0001:
                score_str = "0.0000"
            else:
                score_str = f"{score:.4f}"
            expected_lines.append(f"  {doc_id} | {content:<21} | {score_str:>7}")

        expected_lines.append(f"({len(results)} rows)")
        expected_lines.append("")

    # Add cleanup
    sql_lines.extend([
        "-- Cleanup",
        f"DROP TABLE {test_name}_docs CASCADE;",
        "DROP EXTENSION tapir CASCADE;"
    ])

    expected_lines.extend([
        "-- Cleanup",
        f"DROP TABLE {test_name}_docs CASCADE;",
        "DROP EXTENSION tapir CASCADE;"
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


if __name__ == '__main__':
    main()
