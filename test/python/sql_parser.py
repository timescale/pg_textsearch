#!/usr/bin/env python3
"""
Parser for SQL test files with validation markers.

This module splits SQL files into executable chunks and identifies
BM25 validation markers for score verification.
"""
import re
from dataclasses import dataclass
from typing import List, Optional, Tuple

@dataclass
class ValidationMarker:
    """Represents a BM25 validation point in the SQL file."""
    line_number: int
    table: str
    query: str
    index: str
    text_config: Optional[str] = None
    validate_info: bool = False

    def __repr__(self):
        return f"ValidationMarker(line={self.line_number}, table={self.table}, query='{self.query}')"

@dataclass
class SQLChunk:
    """A chunk of SQL with optional validation marker."""
    sql: str
    start_line: int
    end_line: int
    validation: Optional[ValidationMarker] = None

class SQLParser:
    """Parse SQL test files and extract validation markers."""

    # Regex for validation markers
    VALIDATE_PATTERN = re.compile(
        r'--\s*VALIDATE_BM25(?:_SCAN)?:\s*'
        r'table=(\w+)\s+'
        r'query="([^"]+)"\s+'
        r'index=(\w+)'
        r'(?:\s+text_config=(\w+))?'
    )

    def __init__(self, sql_content: str):
        self.lines = sql_content.splitlines()
        self.chunks: List[SQLChunk] = []

    def parse(self) -> List[SQLChunk]:
        """Parse SQL content into chunks with validation markers."""
        current_chunk = []
        current_marker = None
        chunk_start = 0

        for i, line in enumerate(self.lines, 1):
            # Check for validation marker
            marker_match = self.VALIDATE_PATTERN.match(line)
            if marker_match:
                current_marker = ValidationMarker(
                    line_number=i,
                    table=marker_match.group(1),
                    query=marker_match.group(2),
                    index=marker_match.group(3),
                    text_config=marker_match.group(4),
                    validate_info='_SCAN' in line
                )
                continue

            # Add all lines to current chunk (including comments)
            # Preserve everything for proper output format
            if line.strip() or line.startswith('--'):
                if not current_chunk:
                    chunk_start = i
                current_chunk.append(line)

            # End of statement (semicolon at end of line)
            if line.rstrip().endswith(';'):
                if current_chunk:
                    chunk = SQLChunk(
                        sql='\n'.join(current_chunk),
                        start_line=chunk_start,
                        end_line=i,
                        validation=current_marker
                    )
                    self.chunks.append(chunk)
                    current_chunk = []
                    current_marker = None

        # Handle any remaining SQL
        if current_chunk:
            chunk = SQLChunk(
                sql='\n'.join(current_chunk),
                start_line=chunk_start,
                end_line=len(self.lines),
                validation=current_marker
            )
            self.chunks.append(chunk)

        return self.chunks

    def extract_table_operations(self) -> dict:
        """Extract CREATE TABLE and INSERT statements to track table state."""
        tables = {}

        for chunk in self.chunks:
            sql_upper = chunk.sql.upper()

            # CREATE TABLE
            if 'CREATE TABLE' in sql_upper:
                match = re.search(r'CREATE TABLE\s+(\w+)', chunk.sql, re.IGNORECASE)
                if match:
                    table_name = match.group(1)
                    tables[table_name] = {'creates': chunk, 'inserts': []}

            # INSERT INTO
            if 'INSERT INTO' in sql_upper:
                match = re.search(r'INSERT INTO\s+(\w+)', chunk.sql, re.IGNORECASE)
                if match:
                    table_name = match.group(1)
                    if table_name in tables:
                        tables[table_name]['inserts'].append(chunk)

        return tables

def parse_sql_file(filepath: str) -> Tuple[List[SQLChunk], dict]:
    """
    Parse a SQL test file.

    Returns:
        Tuple of (chunks, table_operations)
    """
    with open(filepath, 'r') as f:
        content = f.read()

    parser = SQLParser(content)
    chunks = parser.parse()
    tables = parser.extract_table_operations()

    return chunks, tables

if __name__ == '__main__':
    import sys

    if len(sys.argv) != 2:
        print("Usage: sql_parser.py <sql_file>")
        sys.exit(1)

    chunks, tables = parse_sql_file(sys.argv[1])

    print(f"Parsed {len(chunks)} SQL chunks")
    for i, chunk in enumerate(chunks, 1):
        print(f"\nChunk {i} (lines {chunk.start_line}-{chunk.end_line}):")
        if chunk.validation:
            print(f"  Validation: {chunk.validation}")
        print(f"  SQL: {chunk.sql[:100]}..." if len(chunk.sql) > 100 else f"  SQL: {chunk.sql}")

    print(f"\n\nFound {len(tables)} tables:")
    for table_name, info in tables.items():
        print(f"  {table_name}: {len(info['inserts'])} INSERT statements")
