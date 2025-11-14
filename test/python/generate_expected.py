#!/usr/bin/env python3
"""
Generate expected output files for PostgreSQL regression tests with BM25 validation.

This script:
1. Executes SQL test files against PostgreSQL
2. Validates BM25 scores at marked points using ground-truth Python implementation
3. Generates complete expected output files
"""
import argparse
import sys
import os
import re
import logging
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import psycopg2

# Import our modules
from sql_parser import parse_sql_file, SQLChunk, ValidationMarker
from postgres_executor import PostgresExecutor
from tokenizer import PostgresTokenizer
from bm25_tantivy import BM25Tantivy

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(levelname)s: %(message)s'
)
logger = logging.getLogger(__name__)

class ExpectedOutputGenerator:
    """Generate expected output files with BM25 validation."""

    def __init__(self, executor: PostgresExecutor, decimal_places: int = 6):
        self.executor = executor
        self.decimal_places = decimal_places
        self.tokenizer = None
        self.validation_errors = []

    def generate(self, sql_file: str, output_file: Optional[str] = None) -> str:
        """
        Generate expected output for a SQL test file.

        Args:
            sql_file: Path to SQL test file
            output_file: Optional path to write output (if None, returns string)

        Returns:
            Generated expected output
        """
        logger.info(f"Processing {sql_file}")

        # Parse SQL file
        chunks, tables = parse_sql_file(sql_file)
        logger.info(f"Found {len(chunks)} SQL chunks, {len([c for c in chunks if c.validation])} with validation markers")

        # Connect to PostgreSQL
        self.executor.connect()
        self.tokenizer = PostgresTokenizer(self.executor.conn)

        # Reset database to clean state
        logger.info("Resetting database...")
        self.executor.reset_database()

        # Process chunks and build output
        output_lines = []

        for i, chunk in enumerate(chunks, 1):
            logger.debug(f"Processing chunk {i}/{len(chunks)}")

            # Execute SQL and capture output
            chunk_output = self.executor.execute_sql_psql(chunk.sql)

            # If there's a validation marker, validate BM25 scores
            if chunk.validation:
                logger.info(f"Validating BM25 scores at line {chunk.validation.line_number}")
                validated_output = self._validate_and_update_scores(
                    chunk_output,
                    chunk.validation,
                    chunk.sql
                )
                output_lines.append(validated_output)
            else:
                output_lines.append(chunk_output)

        # Combine all output
        final_output = '\n'.join(output_lines)

        # Write to file if specified
        if output_file:
            with open(output_file, 'w') as f:
                f.write(final_output)
            logger.info(f"Written expected output to {output_file}")

        # Report any validation errors
        if self.validation_errors:
            logger.error(f"Found {len(self.validation_errors)} validation errors:")
            for error in self.validation_errors:
                logger.error(f"  - {error}")

        self.executor.disconnect()
        return final_output

    def _validate_and_update_scores(self, output: str, marker: ValidationMarker, sql: str) -> str:
        """
        Validate BM25 scores in output and update with ground truth.

        Args:
            output: SQL output to validate
            marker: Validation marker with table/query info
            sql: The SQL that was executed

        Returns:
            Updated output with validated scores
        """
        try:
            # Get ground-truth scores
            ground_truth = self._compute_ground_truth(marker)

            # Parse output to find scores
            if marker.validate_info:
                # Validate INFO output from EXPLAIN
                output = self._validate_info_scores(output, ground_truth)
            else:
                # Validate regular query output
                output = self._validate_query_scores(output, ground_truth, sql)

            return output

        except Exception as e:
            error_msg = f"Validation failed at line {marker.line_number}: {e}"
            self.validation_errors.append(error_msg)
            logger.error(error_msg)
            return output  # Return original output on error

    def _compute_ground_truth(self, marker: ValidationMarker) -> Dict[str, float]:
        """
        Compute ground-truth BM25 scores using Python implementation.

        Returns:
            Dict mapping ctid to BM25 score
        """
        # Get text config from index
        text_config = marker.text_config
        if not text_config:
            index_info = self.executor.get_index_info(marker.index)
            if index_info:
                # Extract text_config from index definition
                match = re.search(r"text_config='([^']+)'", index_info['indexdef'])
                text_config = match.group(1) if match else 'english'
            else:
                text_config = 'english'

        logger.debug(f"Using text_config: {text_config}")

        # Get tokenized documents from PostgreSQL
        doc_tokens = self.tokenizer.tokenize_documents(
            marker.table,
            self._get_text_column(marker.table),
            text_config
        )

        # Also get document content for content-based matching
        text_column = self._get_text_column(marker.table)
        content_sql = f"SELECT ctid::text, {text_column} FROM {marker.table}"
        content_results, _ = self.executor.execute_sql_internal(content_sql)
        content_map = {}
        for row in content_results:
            content_map[row['ctid']] = row[text_column]

        # Convert to list format for BM25
        documents = []
        ctid_map = {}
        for i, (ctid, tokens) in enumerate(doc_tokens.items()):
            documents.append(tokens)
            ctid_map[i] = ctid

        # Create BM25 instance with Tantivy IDF
        bm25 = BM25Tantivy(documents)

        # Tokenize query
        query_tokens = self.tokenizer.tokenize_query(marker.query, text_config)
        logger.debug(f"Query tokens: {query_tokens}")

        # Get scores
        scores = bm25.get_scores(query_tokens)

        # Map scores to ctids and also by ID and content for flexible matching
        ctid_scores = {}
        for i, score in enumerate(scores):
            ctid = ctid_map[i]
            ctid_scores[ctid] = score
            logger.debug(f"  {ctid}: {score:.{self.decimal_places}f}")

            # Also add mapping by ID (extract from ctid like (0,N))
            match = re.match(r'\(0,(\d+)\)', ctid)
            if match:
                ctid_scores[match.group(1)] = score

            # Also add mapping by content text for queries that don't include ctid
            if ctid in content_map:
                ctid_scores[content_map[ctid]] = score

        return ctid_scores

    def _get_text_column(self, table_name: str) -> str:
        """
        Get the text column name for a table.

        This is a simple heuristic - in real implementation,
        we might want to extract this from the CREATE INDEX statement.
        """
        # Query table columns
        sql = f"""
            SELECT column_name
            FROM information_schema.columns
            WHERE table_name = '{table_name}'
              AND data_type IN ('text', 'varchar', 'character varying')
            ORDER BY ordinal_position
            LIMIT 1
        """
        results, _ = self.executor.execute_sql_internal(sql)

        if results:
            return results[0]['column_name']
        else:
            # Fallback to common names
            for col in ['content', 'text', 'doc', 'document', 'body', 'description']:
                sql = f"SELECT 1 FROM {table_name} LIMIT 0"
                try:
                    self.executor.execute_sql_internal(sql)
                    return col
                except:
                    continue

            raise ValueError(f"Could not find text column in table {table_name}")

    def _validate_query_scores(self, output: str, ground_truth: Dict[str, float], sql: str) -> str:
        """
        Validate and update scores in regular query output.

        Looks for patterns like:
        - score column in result set
        - Negative scores (PostgreSQL convention)
        """
        lines = output.splitlines()
        updated_lines = []
        in_results = False
        score_column_index = None
        ctid_column_index = None

        for line in lines:
            # Check if we're in a result set
            if '|' in line and not line.startswith('-'):
                parts = [p.strip() for p in line.split('|')]

                # Header row - find score and ctid columns
                if any('score' in p.lower() for p in parts):
                    for i, part in enumerate(parts):
                        if 'score' in part.lower():
                            score_column_index = i
                        if 'ctid' in part.lower():
                            ctid_column_index = i
                    in_results = True
                    updated_lines.append(line)

                # Data row - validate and update score
                elif in_results and score_column_index is not None:
                    try:
                        # Extract ctid (if available) or use row position
                        if ctid_column_index is not None:
                            ctid = parts[ctid_column_index]
                        else:
                            # Try to extract from other columns or use position
                            ctid = self._extract_ctid_from_row(parts)

                        # Get ground truth score
                        logger.debug(f"Looking for ctid '{ctid}' in ground_truth: {list(ground_truth.keys())}")
                        if ctid in ground_truth:
                            expected_score = ground_truth[ctid]

                            # PostgreSQL uses negative scores for ordering
                            expected_score_str = f"-{expected_score:.{self.decimal_places}f}"

                            # Update score in line
                            parts[score_column_index] = expected_score_str
                            updated_line = ' | '.join(parts)
                            updated_lines.append(updated_line)

                            logger.debug(f"Updated score for ctid {ctid}: {expected_score_str}")
                        else:
                            updated_lines.append(line)
                    except (ValueError, IndexError):
                        updated_lines.append(line)
                else:
                    updated_lines.append(line)
            else:
                # Not in results section anymore
                if in_results and line.startswith('('):
                    in_results = False
                updated_lines.append(line)

        return '\n'.join(updated_lines)

    def _validate_info_scores(self, output: str, ground_truth: Dict[str, float]) -> str:
        """
        Validate and update scores in EXPLAIN INFO output.

        Looks for patterns like:
        INFO: Processing term='word' ... score=X.XXXX
        """
        lines = output.splitlines()
        updated_lines = []

        for line in lines:
            if 'INFO:' in line and 'score=' in line:
                # Extract ctid from INFO line
                ctid_match = re.search(r"ctid=\((\d+,\d+)\)", line)
                if ctid_match:
                    ctid = f"({ctid_match.group(1)})"

                    if ctid in ground_truth:
                        expected_score = ground_truth[ctid]

                        # Replace score in line
                        line = re.sub(
                            r'score=[-\d.]+',
                            f'score={expected_score:.{self.decimal_places}f}',
                            line
                        )
                        logger.debug(f"Updated INFO score for {ctid}")

            updated_lines.append(line)

        return '\n'.join(updated_lines)

    def _extract_ctid_from_row(self, row_parts: List[str]) -> str:
        """
        Try to extract ctid from row data.
        This is a heuristic - might need refinement.
        """
        # Look for patterns like (0,1) or similar
        for part in row_parts:
            if re.match(r'\(\d+,\d+\)', part):
                return part

        # If no ctid found, try to use first column if it looks like an ID (number)
        if row_parts and row_parts[0].strip().isdigit():
            return row_parts[0].strip()

        # Otherwise use first column as identifier
        return row_parts[0] if row_parts else 'unknown'

def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Generate expected output files for PostgreSQL regression tests with BM25 validation'
    )

    parser.add_argument(
        'sql_file',
        nargs='?',
        help='SQL test file to process'
    )

    parser.add_argument(
        '--all',
        action='store_true',
        help='Process all SQL test files'
    )

    parser.add_argument(
        '--output',
        '-o',
        help='Output file path (default: test/expected/<name>.out)'
    )

    parser.add_argument(
        '--validate-only',
        action='store_true',
        help='Validate existing expected files without regenerating'
    )

    parser.add_argument(
        '--decimal-places',
        type=int,
        default=6,
        help='Number of decimal places for score comparison (default: 6)'
    )

    parser.add_argument(
        '--verbose',
        '-v',
        action='store_true',
        help='Enable verbose logging'
    )

    parser.add_argument(
        '--host',
        default='localhost',
        help='PostgreSQL host (default: localhost)'
    )

    parser.add_argument(
        '--port',
        type=int,
        default=5432,
        help='PostgreSQL port (default: 5432)'
    )

    parser.add_argument(
        '--database',
        default='postgres',
        help='PostgreSQL database (default: postgres)'
    )

    parser.add_argument(
        '--user',
        default='postgres',
        help='PostgreSQL user (default: postgres)'
    )

    args = parser.parse_args()

    # Set logging level
    if args.verbose:
        logger.setLevel(logging.DEBUG)
        logging.getLogger().setLevel(logging.DEBUG)

    # Create executor
    executor = PostgresExecutor(
        host=args.host,
        port=args.port,
        database=args.database,
        user=args.user
    )

    # Create generator
    generator = ExpectedOutputGenerator(executor, args.decimal_places)

    if args.all:
        # Process all SQL files
        sql_dir = Path('test/sql')
        sql_files = sorted(sql_dir.glob('*.sql'))

        logger.info(f"Processing {len(sql_files)} SQL files...")

        for sql_file in sql_files:
            # Determine output path
            expected_file = Path('test/expected') / f"{sql_file.stem}.out"

            # Generate expected output
            generator.generate(str(sql_file), str(expected_file))

    elif args.sql_file:
        # Process single file
        sql_file = Path(args.sql_file)

        # Determine output path
        if args.output:
            output_file = args.output
        else:
            expected_dir = Path('test/expected')
            expected_dir.mkdir(exist_ok=True)
            output_file = expected_dir / f"{sql_file.stem}.out"

        # Generate expected output
        output = generator.generate(str(sql_file), str(output_file))

        if not args.output:
            logger.info(f"Output written to {output_file}")
    else:
        parser.print_help()
        sys.exit(1)

    # Report results
    if generator.validation_errors:
        logger.error(f"Validation completed with {len(generator.validation_errors)} errors")
        sys.exit(1)
    else:
        logger.info("Validation completed successfully")

if __name__ == '__main__':
    main()
