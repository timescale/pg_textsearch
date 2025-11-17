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
    parser.add_argument('--force', action='store_true', help='Force cleanup of all tables in database')

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

        # Check for existing user tables (excluding system tables)
        check_tables_sql = """
            SELECT COUNT(*) as table_count
            FROM information_schema.tables
            WHERE table_schema = 'public'
            AND table_type = 'BASE TABLE'
        """
        result, error = executor.execute_sql_internal(check_tables_sql)
        table_count = result[0]['table_count'] if result else 0

        if table_count > 0 and args.force:
            logger.warning("=" * 60)
            logger.warning(f"WARNING: Dropping ALL tables in database '{args.database}'")
            logger.warning("=" * 60)

            # Drop all user tables in public schema
            drop_all_sql = """
                DO $$
                DECLARE
                    r RECORD;
                BEGIN
                    -- Drop all tables in public schema (CASCADE will handle dependencies)
                    FOR r IN (SELECT tablename FROM pg_tables WHERE schemaname = 'public')
                    LOOP
                        EXECUTE 'DROP TABLE IF EXISTS public.' || quote_ident(r.tablename) || ' CASCADE';
                    END LOOP;
                END $$;
            """
            _, error = executor.execute_sql_internal(drop_all_sql)
            if error:
                logger.error(f"Failed to clean database: {error}")
                sys.exit(1)

            logger.info("Database cleaned successfully")

        # Clear BM25 registry before processing template
        # This prevents registry overflow when running multiple templates sequentially
        clear_registry_sql = "SELECT bm25_clear_registry();"
        _, error = executor.execute_sql_internal(clear_registry_sql)
        if error:
            if "does not exist" not in str(error):
                # Real error (not just missing function) - should fail
                logger.error(f"Failed to clear registry: {error}")
                sys.exit(1)
            # Function doesn't exist yet - OK for first run
            logger.debug(f"Registry function not yet available: {error}")

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
