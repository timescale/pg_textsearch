#!/usr/bin/env python3
"""
Template-based SQL validation system.

Processes SQL template files with VALIDATE_BM25 markers to generate:
1. Temp tables with expected results (using Tantivy BM25 scores)
2. The actual query
3. Comparison queries to verify results match
"""

import re
import logging
import subprocess
import tempfile
from typing import List, Dict, Tuple, Optional
from dataclasses import dataclass
import math
import os

from bm25_tantivy import BM25Tantivy
from tokenizer import PostgresTokenizer
from postgres_executor import PostgresExecutor

logger = logging.getLogger(__name__)

@dataclass
class ValidationTemplate:
    """Represents a validation point in the SQL template."""
    line_number: int
    table: str
    query: str
    index: str
    text_config: Optional[str] = None
    original_sql: str = ""
    setup_sql: str = ""  # SQL needed before this validation

class TemplateProcessor:
    """Process SQL templates with validation markers."""

    # Pattern for validation markers
    VALIDATE_PATTERN = re.compile(
        r'--\s*VALIDATE_BM25:\s*'
        r'table=(\w+)\s+'
        r'query="([^"]+)"\s+'
        r'index=(\w+)'
        r'(?:\s+text_config=(\w+))?'
    )

    def __init__(self, executor: PostgresExecutor):
        self.executor = executor
        self.tokenizer = PostgresTokenizer(executor.conn)
        # Store database connection info for psql subprocess
        self.db_host = executor.host
        self.db_port = executor.port
        self.db_name = executor.database
        self.db_user = executor.user

    def _execute_sql_via_psql(self, sql: str, check_on_error_stop: bool = True) -> Tuple[str, str]:
        """
        Execute SQL via psql subprocess to properly handle meta-commands.

        Args:
            sql: SQL to execute
            check_on_error_stop: Whether to check ON_ERROR_STOP setting in SQL

        Returns:
            Tuple of (stdout, stderr)
        """
        # Check if SQL contains ON_ERROR_STOP off (which means errors are expected)
        on_error_stop = 'ON_ERROR_STOP=1'  # Default
        if check_on_error_stop and '\\set ON_ERROR_STOP off' in sql:
            # Don't set ON_ERROR_STOP=1 if the SQL explicitly turns it off
            on_error_stop = None

        # Write SQL to temporary file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql)
            temp_sql_file = f.name

        try:
            # Build psql command
            cmd = [
                'psql',
                '-h', self.db_host,
                '-p', str(self.db_port),
                '-d', self.db_name,
                '-U', self.db_user,
                '-f', temp_sql_file
            ]
            if on_error_stop:
                cmd.extend(['-v', on_error_stop])

            # Execute psql
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                env={**os.environ, 'PGPASSWORD': ''}  # Use empty password or from env
            )

            return result.stdout, result.stderr

        finally:
            # Clean up temp file
            os.unlink(temp_sql_file)

    def process_template(self, template_path: str, output_path: str):
        """
        Process a SQL template file and generate validation SQL.

        Args:
            template_path: Path to template SQL file
            output_path: Path to write generated SQL
        """
        # First pass: Parse template and collect validation points
        validations, cleanup_sql = self._parse_template(template_path)

        # Process each validation point
        output_lines = []
        executed_sql_hash = set()  # Track what SQL we've already executed

        for validation in validations:
            # Execute incremental setup SQL for this validation
            if validation.setup_sql.strip():
                logger.debug(f"Executing setup SQL for validation at line {validation.line_number}")
                # Use psql subprocess to properly handle meta-commands
                stdout, stderr = self._execute_sql_via_psql(validation.setup_sql)

                # Check for errors in stderr (psql reports errors there)
                # Only fail early if ON_ERROR_STOP is not explicitly turned off
                has_error_stop_off = '\\set ON_ERROR_STOP off' in validation.setup_sql
                if stderr and 'ERROR' in stderr and not has_error_stop_off:
                    logger.error(f"Setup SQL failed at line {validation.line_number}: {stderr}")
                    # Include setup SQL in output even if it failed
                    output_lines.append(validation.setup_sql)
                    output_lines.append(f"-- ERROR in setup at line {validation.line_number}: {stderr}\n")
                    # Fail early - exit immediately on error
                    logger.error(f"Stopping processing due to error")
                    import sys
                    sys.exit(1)
                else:
                    # Include successful setup SQL in output
                    output_lines.append(validation.setup_sql)
                    if stdout:
                        logger.debug(f"Setup SQL output: {stdout}")
                    if stderr and has_error_stop_off:
                        logger.debug(f"Expected error (ON_ERROR_STOP off): {stderr}")

            # Now compute expected scores with tables/indexes in place
            try:
                expected_scores = self._compute_expected_scores(validation)

                # Generate validation SQL
                validation_sql = self._generate_validation_sql(validation, expected_scores)
                output_lines.append(f"\n-- Validation for line {validation.line_number}\n")
                output_lines.append(validation_sql)

            except Exception as e:
                logger.error(f"Failed to generate validation for line {validation.line_number}: {e}")
                output_lines.append(f"\n-- ERROR generating validation for line {validation.line_number}: {e}\n")
                output_lines.append(validation.original_sql)

        # Execute cleanup SQL (DROP statements at end of template)
        if cleanup_sql and cleanup_sql.strip():
            logger.debug(f"Executing cleanup SQL for {template_path}")
            stdout, stderr = self._execute_sql_via_psql(cleanup_sql)

            # Include cleanup SQL in output
            output_lines.append("\n-- Cleanup\n")
            output_lines.append(cleanup_sql)

            if stderr and 'ERROR' in stderr:
                logger.warning(f"Cleanup SQL had errors (may be expected): {stderr}")

        # Write output
        with open(output_path, 'w') as f:
            f.write(''.join(output_lines))

        logger.info(f"Generated validation SQL: {output_path}")

    def _parse_template(self, template_path: str) -> Tuple[List[ValidationTemplate], str]:
        """
        Parse template file and extract validation points with their setup SQL.

        Returns:
            Tuple of (List of ValidationTemplate objects, cleanup SQL after last validation)
        """
        with open(template_path, 'r') as f:
            lines = f.readlines()

        validations = []
        all_setup_sql = []  # All accumulated setup SQL
        last_validation_end = 0  # Track where the last validation ended
        last_validation_line = -1  # Track the line number of the last validation
        i = 0

        while i < len(lines):
            line = lines[i]

            # Check for validation marker
            match = self.VALIDATE_PATTERN.search(line)
            if match:
                # Only include SQL since the last validation (incremental)
                incremental_sql = ''.join(all_setup_sql[last_validation_end:])

                # Extract validation info
                validation = ValidationTemplate(
                    line_number=i + 1,
                    table=match.group(1),
                    query=match.group(2),
                    index=match.group(3),
                    text_config=match.group(4),
                    setup_sql=incremental_sql  # Only new SQL since last validation
                )

                # Find the SQL query that follows the marker
                sql_lines = []
                i += 1
                while i < len(lines):
                    sql_lines.append(lines[i])
                    if lines[i].rstrip().endswith(';'):
                        break
                    i += 1

                validation.original_sql = ''.join(sql_lines)
                validations.append(validation)

                # Update the markers for the next validation
                last_validation_end = len(all_setup_sql)
                last_validation_line = i

                # Don't add the validation query to setup_sql
                # (it's the query being validated, not setup)
            else:
                # Accumulate non-validation SQL as setup
                # This includes psql meta-commands which will be properly
                # handled by psql subprocess execution
                all_setup_sql.append(line)

            i += 1

        # Collect cleanup SQL (everything after the last validation)
        cleanup_sql = ''
        if last_validation_line >= 0 and last_validation_line < len(lines) - 1:
            cleanup_sql = ''.join(lines[last_validation_line + 1:])

        return validations, cleanup_sql

    def _generate_validation_sql(self, validation: ValidationTemplate,
                                 expected_scores: Dict[str, float]) -> str:
        """
        Generate SQL that creates temp table with expected results and validates.

        Returns SQL that:
        1. Creates temp table with expected BM25 scores
        2. Runs the actual query
        3. Compares results
        """
        # Parse the original SQL to understand what columns are selected
        columns = self._extract_columns(validation.original_sql)

        # Generate temp table with expected results
        temp_table_sql = self._create_temp_table_sql(
            validation.table,
            expected_scores,
            columns
        )

        # Generate comparison SQL
        comparison_sql = self._create_comparison_sql(
            validation.original_sql,
            columns
        )

        return f"""-- Create temp table with expected results (Tantivy BM25 scores)
{temp_table_sql}

-- Run actual query and compare
{validation.original_sql.rstrip()}

-- Validate results match expected
{comparison_sql}
"""

    def _compute_expected_scores(self, validation: ValidationTemplate) -> Dict[str, float]:
        """
        Compute expected BM25 scores using Tantivy formula.

        Returns:
            Dict mapping document identifier to expected score
        """
        # Get text config from index if not specified
        text_config = validation.text_config
        if not text_config:
            index_info = self.executor.get_index_info(validation.index)
            if index_info:
                match = re.search(r"text_config='([^']+)'", index_info['indexdef'])
                text_config = match.group(1) if match else 'english'
            else:
                text_config = 'english'

        logger.debug(f"Using text_config: {text_config}")

        # Get tokenized documents
        text_column = self._get_text_column(validation.table)
        doc_tokens = self.tokenizer.tokenize_documents(
            validation.table,
            text_column,
            text_config
        )

        if not doc_tokens:
            logger.warning(f"No documents found in table {validation.table}")
            return {}

        # Get document content for identification
        content_sql = f"SELECT ctid::text, {text_column} FROM {validation.table}"
        content_results, error = self.executor.execute_sql_internal(content_sql)
        if error:
            logger.error(f"Failed to get document content: {error}")
            return {}

        content_map = {}
        for row in content_results:
            content_map[row['ctid']] = row[text_column]

        # Convert to BM25 format
        documents = []
        ctid_map = {}
        for i, (ctid, tokens) in enumerate(doc_tokens.items()):
            documents.append(tokens)
            ctid_map[i] = ctid

        # Create BM25 instance with Tantivy IDF
        bm25 = BM25Tantivy(documents)

        # Tokenize query
        query_tokens = self.tokenizer.tokenize_query(validation.query, text_config)
        logger.debug(f"Query tokens: {query_tokens}")

        # Get scores
        scores = bm25.get_scores(query_tokens)

        # Map scores to document identifiers
        doc_scores = {}
        for i, score in enumerate(scores):
            ctid = ctid_map[i]
            # Use content as key for easier matching
            if ctid in content_map:
                doc_scores[content_map[ctid]] = score
                logger.debug(f"Document '{content_map[ctid]}': score={score}")

        return doc_scores

    def _extract_columns(self, sql: str) -> List[str]:
        """Extract column names from SELECT statement."""
        # Simple regex to find SELECT columns
        select_match = re.search(r'SELECT\s+(.*?)\s+FROM', sql, re.IGNORECASE | re.DOTALL)
        if select_match:
            columns_str = select_match.group(1)
            # Parse column expressions (simplified)
            columns = []
            for col in columns_str.split(','):
                col = col.strip()
                # Extract alias if present (case insensitive)
                as_match = re.search(r'\s+[Aa][Ss]\s+(\w+)$', col)
                if as_match:
                    alias = as_match.group(1)
                    columns.append(alias)
                else:
                    # Try to find simple column name (not ROUND or other functions)
                    # Look for simple word at the beginning
                    col_match = re.match(r'^(\w+)$', col)
                    if col_match:
                        columns.append(col_match.group(1))
            logger.debug(f"Extracted columns: {columns}")
            return columns
        return []

    def _create_temp_table_sql(self, table: str, expected_scores: Dict[str, float],
                               columns: List[str]) -> str:
        """Create SQL for temp table with expected results."""
        # Only include documents that would match the query (have non-zero scores)
        rows = []
        for content, score in expected_scores.items():
            if score > 0:  # Only include documents with positive scores
                # PostgreSQL uses negative scores for ordering
                neg_score = -score
                # For validation, we only need the scores
                # Round to 6 decimal places for consistency
                rows.append(f"  SELECT ROUND({neg_score}::numeric, 6)")

        if not rows:
            return "-- No documents match the query"

        return f"""CREATE TEMP TABLE expected_results AS
{' UNION ALL\n'.join(rows)};"""

    def _create_comparison_sql(self, original_sql: str, columns: List[str]) -> str:
        """Create SQL to compare actual vs expected results."""
        # Remove trailing semicolon from original SQL
        query = original_sql.rstrip().rstrip(';')

        # Build column list for comparison
        col_list = ', '.join(columns)

        return f"""-- Compare actual results with expected
WITH actual_results AS (
{query}
)
SELECT
  CASE
    WHEN (SELECT COUNT(*) FROM expected_results) = (SELECT COUNT(*) FROM actual_results)
     AND NOT EXISTS (
            SELECT {col_list} FROM expected_results
            EXCEPT
            SELECT {col_list} FROM actual_results
          )
     AND NOT EXISTS (
            SELECT {col_list} FROM actual_results
            EXCEPT
            SELECT {col_list} FROM expected_results
          )
    THEN 'PASS: Results match expected'
    ELSE 'FAIL: Results do not match expected'
  END as validation_result;

-- Show differences if any
WITH actual_results AS (
{query}
)
SELECT 'Missing from actual:' as issue, {col_list}
FROM expected_results
EXCEPT
SELECT 'Missing from actual:' as issue, {col_list}
FROM actual_results
UNION ALL
SELECT 'Extra in actual:' as issue, {col_list}
FROM actual_results
EXCEPT
SELECT 'Extra in actual:' as issue, {col_list}
FROM expected_results;"""

    def _get_text_column(self, table_name: str) -> str:
        """Get the text column name for a table."""
        sql = f"""
            SELECT column_name
            FROM information_schema.columns
            WHERE table_name = '{table_name}'
              AND data_type IN ('text', 'varchar', 'character varying')
            ORDER BY ordinal_position
            LIMIT 1
        """
        results, error = self.executor.execute_sql_internal(sql)

        if error:
            logger.error(f"Failed to get text column: {error}")
            return 'content'  # Default assumption

        if results:
            return results[0]['column_name']
        else:
            return 'content'  # Default assumption
