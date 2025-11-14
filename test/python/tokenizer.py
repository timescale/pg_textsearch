#!/usr/bin/env python3
"""
PostgreSQL tokenization interface.

This module provides functions to tokenize text using PostgreSQL's
text search configuration, ensuring consistency between Python validation
and PostgreSQL's actual tokenization.
"""
import re
from typing import List, Dict, Tuple
import psycopg2
from psycopg2.extras import RealDictCursor

class PostgresTokenizer:
    """Use PostgreSQL's tokenization for consistency."""

    def __init__(self, connection):
        self.conn = connection

    def tokenize_text(self, text: str, text_config: str = 'english') -> List[str]:
        """
        Tokenize text using PostgreSQL's to_tsvector.

        Args:
            text: Text to tokenize
            text_config: PostgreSQL text search configuration

        Returns:
            List of tokens in order
        """
        with self.conn.cursor() as cursor:
            cursor.execute(
                "SELECT to_tsvector(%s, %s)::text AS tsvector",
                (text_config, text)
            )
            result = cursor.fetchone()
            return self._parse_tsvector(result[0])

    def tokenize_documents(self, table_name: str, text_column: str,
                          text_config: str = 'english') -> Dict[str, List[str]]:
        """
        Tokenize all documents in a table.

        Returns:
            Dict mapping ctid to list of tokens
        """
        with self.conn.cursor(cursor_factory=RealDictCursor) as cursor:
            # Get all documents with their tokens
            cursor.execute(f"""
                SELECT
                    ctid::text,
                    {text_column} as raw_text,
                    to_tsvector(%s, {text_column})::text as tsvector
                FROM {table_name}
                ORDER BY ctid
            """, (text_config,))

            documents = {}
            for row in cursor.fetchall():
                tokens = self._parse_tsvector(row['tsvector'])
                documents[row['ctid']] = tokens

            return documents

    def tokenize_query(self, query: str, text_config: str = 'english') -> List[str]:
        """
        Tokenize a query using PostgreSQL's to_tsquery.

        Args:
            query: Query text
            text_config: PostgreSQL text search configuration

        Returns:
            List of query tokens
        """
        with self.conn.cursor() as cursor:
            # Use plainto_tsquery for simple tokenization
            cursor.execute(
                "SELECT plainto_tsquery(%s, %s)::text AS tsquery",
                (text_config, query)
            )
            result = cursor.fetchone()
            return self._parse_tsquery(result[0])

    def get_document_lengths(self, table_name: str, text_column: str,
                           text_config: str = 'english') -> Dict[str, int]:
        """
        Get document lengths (token counts) for all documents.

        Returns:
            Dict mapping ctid to document length
        """
        with self.conn.cursor(cursor_factory=RealDictCursor) as cursor:
            cursor.execute(f"""
                SELECT
                    ctid::text,
                    array_length(
                        string_to_array(
                            to_tsvector(%s, {text_column})::text,
                            ' '
                        ),
                        1
                    ) as doc_length
                FROM {table_name}
                ORDER BY ctid
            """, (text_config,))

            lengths = {}
            for row in cursor.fetchall():
                # Handle NULL (empty documents)
                lengths[row['ctid']] = row['doc_length'] or 0

            return lengths

    def get_term_frequencies(self, table_name: str, text_column: str,
                           text_config: str = 'english') -> Dict[str, Dict[str, int]]:
        """
        Get term frequencies for all documents.

        Returns:
            Dict mapping ctid to dict of term -> frequency
        """
        documents = {}

        with self.conn.cursor(cursor_factory=RealDictCursor) as cursor:
            # Get all documents
            cursor.execute(f"""
                SELECT
                    ctid::text,
                    to_tsvector(%s, {text_column})::text as tsvector
                FROM {table_name}
                ORDER BY ctid
            """, (text_config,))

            for row in cursor.fetchall():
                term_freqs = self._parse_tsvector_with_positions(row['tsvector'])
                documents[row['ctid']] = term_freqs

        return documents

    def _parse_tsvector(self, tsvector_str: str) -> List[str]:
        """
        Parse a tsvector string into a list of tokens.

        Example input: "'document':1 'one':3 'original':2"
        Returns: ['document', 'one', 'original']
        """
        if not tsvector_str:
            return []

        tokens = []
        # Match 'token':positions format
        pattern = r"'([^']+)':\d+(?:,\d+)*"
        matches = re.findall(pattern, tsvector_str)

        return matches

    def _parse_tsvector_with_positions(self, tsvector_str: str) -> Dict[str, int]:
        """
        Parse a tsvector string into term frequencies.

        Example input: "'document':1,5 'one':3 'original':2"
        Returns: {'document': 2, 'one': 1, 'original': 1}
        """
        if not tsvector_str:
            return {}

        term_freqs = {}
        # Match 'token':positions format
        pattern = r"'([^']+)':([0-9,]+)"
        matches = re.findall(pattern, tsvector_str)

        for token, positions in matches:
            # Count positions (frequency = number of positions)
            position_list = positions.split(',')
            term_freqs[token] = len(position_list)

        return term_freqs

    def _parse_tsquery(self, tsquery_str: str) -> List[str]:
        """
        Parse a tsquery string into a list of query tokens.

        Example input: "'document' & 'search'"
        Returns: ['document', 'search']
        """
        if not tsquery_str:
            return []

        # Extract tokens from tsquery
        pattern = r"'([^']+)'"
        matches = re.findall(pattern, tsquery_str)

        return matches

    def get_index_config(self, index_name: str) -> str:
        """
        Get the text search configuration for an index.

        Returns:
            Text search configuration name
        """
        with self.conn.cursor() as cursor:
            # Query pg_indexes to get index definition
            cursor.execute("""
                SELECT indexdef
                FROM pg_indexes
                WHERE indexname = %s
            """, (index_name,))

            result = cursor.fetchone()
            if not result:
                raise ValueError(f"Index {index_name} not found")

            indexdef = result[0]

            # Extract text_config from index definition
            # Look for pattern: WITH (text_config='...')
            pattern = r"text_config='([^']+)'"
            match = re.search(pattern, indexdef)

            if match:
                return match.group(1)
            else:
                return 'english'  # Default

def get_table_documents(conn, table_name: str, text_column: str,
                        text_config: str = 'english') -> List[Tuple[str, List[str]]]:
    """
    Get all documents from a table with their tokens.

    Returns:
        List of (ctid, tokens) tuples
    """
    tokenizer = PostgresTokenizer(conn)
    doc_tokens = tokenizer.tokenize_documents(table_name, text_column, text_config)

    # Convert to list of tuples for BM25
    return [(ctid, tokens) for ctid, tokens in doc_tokens.items()]

if __name__ == '__main__':
    # Test tokenizer
    import sys

    if len(sys.argv) < 2:
        print("Usage: tokenizer.py <text> [config]")
        sys.exit(1)

    text = sys.argv[1]
    config = sys.argv[2] if len(sys.argv) > 2 else 'english'

    # Connect to PostgreSQL
    conn = psycopg2.connect(
        host="localhost",
        database="postgres",
        user="postgres"
    )

    tokenizer = PostgresTokenizer(conn)

    print(f"Text: {text}")
    print(f"Config: {config}")
    print(f"Tokens: {tokenizer.tokenize_text(text, config)}")

    # Also show query tokenization
    print(f"Query tokens: {tokenizer.tokenize_query(text, config)}")

    conn.close()
