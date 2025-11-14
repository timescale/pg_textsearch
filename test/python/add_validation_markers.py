#!/usr/bin/env python3
"""
Add VALIDATE_BM25 markers to SQL test files.

This script analyzes SQL files and adds validation markers before SELECT queries
that use the BM25 scoring operator (<@>).
"""

import re
import sys
import argparse
from pathlib import Path

def extract_table_and_index(sql_lines, select_index):
    """
    Extract table name and index name from context before a SELECT statement.

    Looks backwards from the SELECT to find the most recent CREATE INDEX
    and CREATE TABLE statements.
    """
    table_name = None
    index_name = None

    # Look backwards from the SELECT statement
    for i in range(select_index - 1, -1, -1):
        line = sql_lines[i]

        # Look for CREATE INDEX
        index_match = re.search(r'CREATE\s+INDEX\s+(\w+)\s+ON\s+(\w+)', line, re.IGNORECASE)
        if index_match and not index_name:
            index_name = index_match.group(1)
            if not table_name:
                table_name = index_match.group(2)

        # Look for CREATE TABLE
        table_match = re.search(r'CREATE\s+TABLE\s+(\w+)', line, re.IGNORECASE)
        if table_match and not table_name:
            table_name = table_match.group(1)

        # Stop if we have both
        if table_name and index_name:
            break

    return table_name, index_name

def extract_query_term(select_line):
    """Extract the query term from a to_bm25query call."""
    # Look for to_bm25query('term', 'index')
    match = re.search(r"to_bm25query\s*\(\s*'([^']+)'", select_line)
    if match:
        return match.group(1)
    return None

def add_markers_to_file(filepath):
    """Add VALIDATE_BM25 markers to a SQL file."""
    with open(filepath, 'r') as f:
        lines = f.readlines()

    output_lines = []
    i = 0
    markers_added = 0

    while i < len(lines):
        line = lines[i]

        # Check if this is a SELECT with BM25 scoring
        if re.search(r'SELECT.*<@>', line, re.IGNORECASE):
            # Check if there's already a VALIDATE_BM25 marker
            if i > 0 and 'VALIDATE_BM25' in lines[i-1]:
                # Marker already exists, keep it
                output_lines.append(line)
                i += 1
                continue

            # Extract query information
            query_term = extract_query_term(line)

            # Find the complete SELECT statement (may span multiple lines)
            select_lines = [line]
            j = i + 1
            while j < len(lines) and not lines[j].rstrip().endswith(';'):
                select_lines.append(lines[j])
                query_term = query_term or extract_query_term(lines[j])
                j += 1
            if j < len(lines):
                select_lines.append(lines[j])
                query_term = query_term or extract_query_term(lines[j])

            # Get table and index names from context
            table_name, index_name = extract_table_and_index(lines, i)

            if table_name and index_name and query_term:
                # Add validation marker
                marker = f'-- VALIDATE_BM25: table={table_name} query="{query_term}" index={index_name}\n'
                output_lines.append(marker)
                markers_added += 1

            # Add the SELECT lines
            for select_line in select_lines:
                output_lines.append(select_line)

            # Skip the lines we've already processed
            i = j + 1
        else:
            output_lines.append(line)
            i += 1

    if markers_added > 0:
        # Write the modified content back
        with open(filepath, 'w') as f:
            f.writelines(output_lines)
        print(f"Added {markers_added} VALIDATE_BM25 markers to {filepath}")
        return True
    else:
        print(f"No markers needed for {filepath}")
        return False

def main():
    parser = argparse.ArgumentParser(description='Add VALIDATE_BM25 markers to SQL files')
    parser.add_argument('files', nargs='+', help='SQL files to process')
    parser.add_argument('--dry-run', action='store_true', help='Show what would be done without modifying files')

    args = parser.parse_args()

    modified_count = 0
    for filepath in args.files:
        path = Path(filepath)
        if not path.exists():
            print(f"File not found: {filepath}")
            continue

        if not str(path).endswith('.sql'):
            print(f"Skipping non-SQL file: {filepath}")
            continue

        if args.dry_run:
            print(f"Would process: {filepath}")
        else:
            if add_markers_to_file(path):
                modified_count += 1

    if not args.dry_run:
        print(f"\nModified {modified_count} files")

if __name__ == '__main__':
    main()
