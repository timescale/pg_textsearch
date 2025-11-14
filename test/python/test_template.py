#!/usr/bin/env python3
"""
Test the template processor system.
"""

import sys
import logging
from pathlib import Path

from postgres_executor import PostgresExecutor
from template_processor import TemplateProcessor

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)

def main():
    # Database connection settings
    host = sys.argv[1] if len(sys.argv) > 1 else 'localhost'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5433
    database = sys.argv[3] if len(sys.argv) > 3 else 'postgres'
    user = sys.argv[4] if len(sys.argv) > 4 else 'tjg'

    # Connect to database
    executor = PostgresExecutor(host, port, database, user)
    executor.connect()

    # Clean up any existing test tables
    cleanup_sql = "DROP TABLE IF EXISTS validation_docs CASCADE"
    executor.execute_sql_internal(cleanup_sql)
    logger.info("Cleaned up existing test tables")

    # Process template
    processor = TemplateProcessor(executor)

    template_path = '../sql/validation_test.sql.template'
    output_path = '/tmp/validation_test.sql'

    logger.info(f"Processing template: {template_path}")
    processor.process_template(template_path, output_path)

    logger.info(f"Generated SQL written to: {output_path}")

    # Show the generated SQL
    with open(output_path, 'r') as f:
        print("\n=== Generated SQL ===")
        print(f.read())

if __name__ == '__main__':
    main()
