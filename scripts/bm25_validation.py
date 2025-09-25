#!/usr/bin/env python3
"""
BM25 Score Validation Tool for Tapir

This script compares BM25 scores from Tapir against a reference Python implementation.
It fetches documents from PostgreSQL, applies text processing (stemming via to_tsvector),
and calculates BM25 scores using both methods for comparison.
"""

import argparse
import sys
import json
from typing import List, Dict, Tuple, Optional
import numpy as np
import pandas as pd
import psycopg2
from psycopg2.extras import RealDictCursor
from rank_bm25 import BM25Okapi
from tabulate import tabulate


class BM25Validator:
    def __init__(self, db_config: Dict[str, str], k1: float = 1.2, b: float = 0.75):
        """
        Initialize the BM25 validator.

        Args:
            db_config: Database connection parameters
            k1: BM25 k1 parameter (default 1.2)
            b: BM25 b parameter (default 0.75)
        """
        self.db_config = db_config
        self.k1 = k1
        self.b = b
        self.conn = None
        self.corpus = []
        self.doc_ids = []
        self.bm25 = None

    def connect(self):
        """Connect to the PostgreSQL database."""
        try:
            self.conn = psycopg2.connect(**self.db_config)
            print(f"Connected to database: {self.db_config.get('dbname', 'default')}")
        except psycopg2.Error as e:
            print(f"Error connecting to database: {e}")
            sys.exit(1)

    def fetch_corpus(self, table_name: str, text_column: str, id_column: str = 'id',
                     text_config: str = 'english', where_clause: str = None):
        """
        Fetch the corpus from the database with to_tsvector applied.

        Args:
            table_name: Name of the table
            text_column: Name of the text column
            id_column: Name of the ID column
            text_config: PostgreSQL text search configuration
            where_clause: Optional WHERE clause
        """
        with self.conn.cursor(cursor_factory=RealDictCursor) as cur:
            # Build the query
            query = f"""
                SELECT
                    {id_column} as doc_id,
                    {text_column} as original_text,
                    to_tsvector('{text_config}', {text_column})::text as tsvector_text,
                    array_to_string(
                        ARRAY(
                            SELECT lexeme
                            FROM unnest(to_tsvector('{text_config}', {text_column}))
                        ), ' '
                    ) as stemmed_text,
                    char_length({text_column}) as doc_length
                FROM {table_name}
            """

            if where_clause:
                query += f" WHERE {where_clause}"

            query += f" ORDER BY {id_column}"

            print(f"Fetching corpus with query:\n{query}")
            cur.execute(query)
            results = cur.fetchall()

            self.corpus = []
            self.doc_ids = []
            self.doc_info = []

            for row in results:
                # Parse tsvector to get terms with positions
                tsvector_text = row['tsvector_text']
                terms = []

                # Parse tsvector format: 'term1:pos1,pos2 term2:pos3'
                for token in tsvector_text.split():
                    if ':' in token:
                        term = token.split(':')[0].strip("'")
                        # Repeat term for each position (to maintain frequency)
                        positions = token.split(':')[1].split(',')
                        terms.extend([term] * len(positions))
                    else:
                        # Term without positions (shouldn't happen with to_tsvector)
                        terms.append(token.strip("'"))

                self.corpus.append(terms)
                self.doc_ids.append(row['doc_id'])
                self.doc_info.append({
                    'doc_id': row['doc_id'],
                    'original_text': row['original_text'][:100] + '...' if len(row['original_text']) > 100 else row['original_text'],
                    'doc_length': row['doc_length'],
                    'term_count': len(terms)
                })

            print(f"Fetched {len(self.corpus)} documents")
            print(f"Average document length: {np.mean([d['term_count'] for d in self.doc_info]):.2f} terms")

            # Initialize BM25 with the corpus (disable stemming since we already used PostgreSQL's to_tsvector)
            self.bm25 = BM25Okapi(self.corpus, k1=self.k1, b=self.b)
            # Disable any additional preprocessing since corpus is already processed by PostgreSQL
            self.bm25.tokenizer = lambda x: x  # Identity function - no additional tokenization

            # Fix IDF values to use epsilon handling like Tapir
            self._apply_epsilon_idf_handling()

    def _apply_epsilon_idf_handling(self):
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

        for term in self.bm25.idf.keys():
            df = sum(1 for doc in self.corpus if term in doc)
            N = len(self.corpus)
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
        for term in self.bm25.idf.keys():
            df = sum(1 for doc in self.corpus if term in doc)
            N = len(self.corpus)
            raw_idf = math.log((N - df + 0.5) / (df + 0.5))

            if raw_idf < 0:
                # Use epsilon * average_idf like Tapir's tp_calculate_idf_with_epsilon()
                self.bm25.idf[term] = epsilon * average_idf
            else:
                self.bm25.idf[term] = raw_idf

    def calculate_python_scores(self, query: str, text_config: str = 'english') -> Dict[int, float]:
        """
        Calculate BM25 scores using the Python implementation.

        Args:
            query: Query string
            text_config: PostgreSQL text search configuration for query processing

        Returns:
            Dictionary mapping document IDs to scores
        """
        # Process query through to_tsvector to match corpus processing
        with self.conn.cursor() as cur:
            cur.execute(f"SELECT to_tsvector('{text_config}', %s)::text", (query,))
            tsvector_result = cur.fetchone()[0]

            # Parse query terms from tsvector
            query_terms = []
            for token in tsvector_result.split():
                if ':' in token:
                    term = token.split(':')[0].strip("'")
                    query_terms.append(term)
                else:
                    query_terms.append(token.strip("'"))

        print(f"Query terms (stemmed): {query_terms}")

        # Calculate scores
        scores = self.bm25.get_scores(query_terms)

        # Map scores to document IDs
        return {doc_id: score for doc_id, score in zip(self.doc_ids, scores)}

    def calculate_tapir_scores(self, query: str, table_name: str, text_column: str,
                              index_name: str, id_column: str = 'id', where_clause: str = None) -> Dict[int, float]:
        """
        Calculate BM25 scores using Tapir.

        Args:
            query: Query string
            table_name: Name of the table
            text_column: Name of the text column
            index_name: Name of the Tapir index
            id_column: Name of the ID column
            where_clause: Optional WHERE clause

        Returns:
            Dictionary mapping document IDs to scores
        """
        with self.conn.cursor(cursor_factory=RealDictCursor) as cur:
            # Build the query
            query_sql = f"""
                SELECT
                    {id_column} as doc_id,
                    ({text_column} <@> to_tpquery(%s, %s))::float8 as score
                FROM {table_name}
            """

            if where_clause:
                query_sql += f" WHERE {where_clause}"

            query_sql += f" ORDER BY {id_column}"

            cur.execute(query_sql, (query, index_name))
            results = cur.fetchall()

            # Tapir returns actual BM25 scores (can be positive or negative)
            # Use scores directly for comparison
            return {row['doc_id']: row['score'] for row in results}

    def compare_scores(self, python_scores: Dict[int, float], tapir_scores: Dict[int, float],
                       tolerance: float = 0.001) -> pd.DataFrame:
        """
        Compare Python and Tapir BM25 scores.

        Args:
            python_scores: Scores from Python BM25
            tapir_scores: Scores from Tapir
            tolerance: Acceptable difference threshold

        Returns:
            DataFrame with comparison results
        """
        # Ensure we have the same documents
        doc_ids = sorted(set(python_scores.keys()) | set(tapir_scores.keys()))

        comparison_data = []
        for doc_id in doc_ids:
            python_score = python_scores.get(doc_id, 0.0)
            tapir_score = tapir_scores.get(doc_id, 0.0)
            # Tapir returns negative BM25 scores for PostgreSQL ASC ordering
            diff = abs(python_score - (-tapir_score))

            # Get document info
            doc_info = next((d for d in self.doc_info if d['doc_id'] == doc_id), {})

            comparison_data.append({
                'doc_id': doc_id,
                'document': doc_info.get('original_text', ''),
                'python_score': python_score,
                'tapir_score': tapir_score,
                'difference': diff,
                'match': '✓' if diff <= tolerance else '✗',
                'terms': doc_info.get('term_count', 0)
            })

        df = pd.DataFrame(comparison_data)

        # Sort by Python score (descending) for better readability
        df = df.sort_values('python_score', ascending=False)

        return df

    def run_validation(self, table_name: str, text_column: str, index_name: str,
                      queries: List[str], id_column: str = 'id',
                      text_config: str = 'english', where_clause: str = None,
                      tolerance: float = 0.001):
        """
        Run the full validation process.

        Args:
            table_name: Name of the table
            text_column: Name of the text column
            index_name: Name of the Tapir index
            queries: List of queries to test
            id_column: Name of the ID column
            text_config: PostgreSQL text search configuration
            where_clause: Optional WHERE clause
            tolerance: Acceptable difference threshold
        """
        print(f"\n{'='*80}")
        print(f"BM25 Validation for {table_name}.{text_column}")
        print(f"Index: {index_name}, Config: {text_config}, k1={self.k1}, b={self.b}")
        print(f"{'='*80}\n")

        # Fetch corpus
        self.fetch_corpus(table_name, text_column, id_column, text_config, where_clause)

        # Process each query
        for query in queries:
            print(f"\n{'-'*60}")
            print(f"Query: '{query}'")
            print(f"{'-'*60}")

            # Calculate scores
            python_scores = self.calculate_python_scores(query, text_config)
            tapir_scores = self.calculate_tapir_scores(query, table_name, text_column,
                                                       index_name, id_column, where_clause)

            # Compare
            df = self.compare_scores(python_scores, tapir_scores, tolerance)

            # Display results
            print("\nTop 10 Results:")
            display_df = df.head(10)[['doc_id', 'document', 'python_score', 'tapir_score', 'difference', 'match']]
            print(tabulate(display_df, headers='keys', tablefmt='grid', floatfmt='.6f', showindex=False))

            # Statistics
            print(f"\nStatistics:")
            print(f"  Total documents: {len(df)}")
            print(f"  Matching scores (diff < {tolerance}): {(df['match'] == '✓').sum()}")
            print(f"  Mismatched scores: {(df['match'] == '✗').sum()}")
            print(f"  Average difference: {df['difference'].mean():.6f}")
            print(f"  Max difference: {df['difference'].max():.6f}")
            print(f"  Min difference: {df['difference'].min():.6f}")

            # Correlation
            correlation = df['python_score'].corr(df['tapir_score'])
            print(f"  Score correlation: {correlation:.6f}")

            # Ranking comparison
            df['python_rank'] = df['python_score'].rank(ascending=False, method='min')
            df['tapir_rank'] = df['tapir_score'].rank(ascending=False, method='min')

            # Try Spearman correlation if scipy is available
            try:
                rank_correlation = df['python_rank'].corr(df['tapir_rank'], method='spearman')
                print(f"  Rank correlation (Spearman): {rank_correlation:.6f}")
            except (ImportError, ModuleNotFoundError):
                # Fallback to Pearson on ranks if scipy not available
                rank_correlation = df['python_rank'].corr(df['tapir_rank'])
                print(f"  Rank correlation (Pearson on ranks): {rank_correlation:.6f}")

            # Show mismatches if any
            mismatches = df[df['match'] == '✗']
            if len(mismatches) > 0:
                print(f"\nMismatched scores (showing up to 5):")
                display_mismatches = mismatches.head(5)[['doc_id', 'document', 'python_score',
                                                         'tapir_score', 'difference', 'terms']]
                print(tabulate(display_mismatches, headers='keys', tablefmt='grid',
                              floatfmt='.6f', showindex=False))


def main():
    parser = argparse.ArgumentParser(description='Validate Tapir BM25 scores against Python implementation')
    parser.add_argument('--host', default='localhost', help='Database host')
    parser.add_argument('--port', type=int, default=5432, help='Database port')
    parser.add_argument('--dbname', required=True, help='Database name')
    parser.add_argument('--user', default='postgres', help='Database user')
    parser.add_argument('--password', help='Database password')

    parser.add_argument('--table', required=True, help='Table name')
    parser.add_argument('--text-column', required=True, help='Text column name')
    parser.add_argument('--index-name', required=True, help='Tapir index name')
    parser.add_argument('--id-column', default='id', help='ID column name')
    parser.add_argument('--text-config', default='english', help='Text search configuration')
    parser.add_argument('--where', help='WHERE clause for filtering')

    parser.add_argument('--k1', type=float, default=1.2, help='BM25 k1 parameter')
    parser.add_argument('--b', type=float, default=0.75, help='BM25 b parameter')

    parser.add_argument('--queries', nargs='+', required=True, help='Queries to test')
    parser.add_argument('--tolerance', type=float, default=0.001, help='Score difference tolerance')

    args = parser.parse_args()

    # Database configuration
    db_config = {
        'host': args.host,
        'port': args.port,
        'dbname': args.dbname,
        'user': args.user
    }
    if args.password:
        db_config['password'] = args.password

    # Create validator
    validator = BM25Validator(db_config, k1=args.k1, b=args.b)
    validator.connect()

    try:
        # Run validation
        validator.run_validation(
            table_name=args.table,
            text_column=args.text_column,
            index_name=args.index_name,
            queries=args.queries,
            id_column=args.id_column,
            text_config=args.text_config,
            where_clause=args.where,
            tolerance=args.tolerance
        )
    finally:
        if validator.conn:
            validator.conn.close()


if __name__ == '__main__':
    main()
