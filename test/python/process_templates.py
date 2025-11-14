#!/usr/bin/env python3
"""
Process SQL template files to generate validation SQL.

This script is called from the Makefile to convert .sql.template files
to .sql files with BM25 validation.
"""

import sys
import os
import argparse
import logging
from pathlib import Path

from postgres_executor import PostgresExecutor
from template_processor import TemplateProcessor

logging.basicConfig(
    format='%(levelname)s: %(message)s',
    level=logging.INFO
)
logger = logging.getLogger(__name__)

def main():
    parser = argparse.ArgumentParser(description='Process SQL template files for BM25 validation')
    parser.add_argument('template', help='Input template file (.sql.template)')
    parser.add_argument('output', help='Output SQL file')
    parser.add_argument('--host', default='localhost', help='Database host')
    parser.add_argument('--port', type=int, default=5432, help='Database port')
    parser.add_argument('--database', default='postgres', help='Database name')
    parser.add_argument('--user', default=os.environ.get('USER', 'postgres'), help='Database user')
    parser.add_argument('--verbose', action='store_true', help='Enable verbose output')

    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    # Validate input file
    template_path = Path(args.template)
    if not template_path.exists():
        logger.error(f"Template file not found: {template_path}")
        sys.exit(1)

    if not str(template_path).endswith('.sql.template'):
        logger.error(f"Input file must be a .sql.template file: {template_path}")
        sys.exit(1)

    # Connect to database
    executor = PostgresExecutor(
        host=args.host,
        port=args.port,
        database=args.database,
        user=args.user
    )

    try:
        executor.connect()

        # Clean up any existing test tables from previous runs
        # This helps ensure templates run in a clean state
        cleanup_tables = [
            'validation_docs', 'test_docs', 'docs', 'documents',
            'aerodocs', 'mixed_docs', 'string_test', 'manyterms_docs'
        ]
        for table in cleanup_tables:
            cleanup_sql = f"DROP TABLE IF EXISTS {table} CASCADE"
            executor.execute_sql_internal(cleanup_sql)

        # Process template
        processor = TemplateProcessor(executor)
        processor.process_template(str(template_path), args.output)

        logger.info(f"Successfully generated: {args.output}")

    except Exception as e:
        logger.error(f"Failed to process template: {e}")
        sys.exit(1)

    finally:
        executor.disconnect()

if __name__ == '__main__':
    main()
