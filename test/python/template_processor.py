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
from typing import List, Dict, Tuple, Optional
from dataclasses import dataclass
import math

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

    def process_template(self, template_path: str, output_path: str):
        """
        Process a SQL template file and generate validation SQL.

        Args:
            template_path: Path to template SQL file
            output_path: Path to write generated SQL
        """
        # First pass: Parse template and collect validation points
        validations = self._parse_template(template_path)

        # Process each validation point
        output_lines = []

        for validation in validations:
            # Execute setup SQL to create tables/indexes
            if validation.setup_sql:
                logger.debug(f"Executing setup SQL for validation at line {validation.line_number}")
                results, error = self.executor.execute_sql_internal(validation.setup_sql)
                if error:
                    logger.error(f"Setup SQL failed: {error}")
                    # Include setup SQL in output even if it failed
                    output_lines.append(validation.setup_sql)
                    output_lines.append(f"-- ERROR in setup: {error}\n")
                    continue
                else:
                    # Include successful setup SQL in output
                    output_lines.append(validation.setup_sql)

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

        # Write output
        with open(output_path, 'w') as f:
            f.write(''.join(output_lines))

        logger.info(f"Generated validation SQL: {output_path}")

    def _parse_template(self, template_path: str) -> List[ValidationTemplate]:
        """
        Parse template file and extract validation points with their setup SQL.

        Returns:
            List of ValidationTemplate objects with setup_sql populated
        """
        with open(template_path, 'r') as f:
            lines = f.readlines()

        validations = []
        setup_sql = []  # Accumulate setup SQL
        i = 0

        while i < len(lines):
            line = lines[i]

            # Check for validation marker
            match = self.VALIDATE_PATTERN.search(line)
            if match:
                # Extract validation info
                validation = ValidationTemplate(
                    line_number=i + 1,
                    table=match.group(1),
                    query=match.group(2),
                    index=match.group(3),
                    text_config=match.group(4),
                    setup_sql=''.join(setup_sql)  # All SQL up to this point
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

                # Don't add the validation query to setup_sql
                # (it's the query being validated, not setup)
            else:
                # Skip psql meta-commands (they start with backslash)
                if line.strip().startswith('\\'):
                    pass  # Skip psql meta-commands
                else:
                    # Accumulate non-validation SQL as setup
                    setup_sql.append(line)

            i += 1

        return validations

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

                # Build row values based on columns
                values = []
                for col in columns:
                    col_lower = col.lower()
                    if col_lower == 'id':
                        # Need to get the ID from the actual table
                        id_sql = f"SELECT id FROM {table} WHERE content = %s"
                        with self.executor.conn.cursor() as cursor:
                            cursor.execute(id_sql, (content,))
                            result = cursor.fetchone()
                            if result:
                                values.append(str(result[0]))
                    elif col_lower == 'content':
                        # Escape single quotes in content
                        escaped_content = content.replace("'", "''")
                        values.append(f"'{escaped_content}'::text")
                    elif col_lower in ('score', 'relevance'):
                        # Round to 6 decimal places for consistency
                        values.append(f"ROUND({neg_score}::numeric, 6)")

                if values:
                    rows.append(f"  SELECT {', '.join(values)}")

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
