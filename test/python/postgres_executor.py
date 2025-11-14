#!/usr/bin/env python3
"""
PostgreSQL SQL executor that captures output like psql.

This module executes SQL statements and captures their output in the
same format as psql, suitable for regression test expected files.
"""
import subprocess
import tempfile
import os
from typing import Optional, Tuple
import psycopg2
from psycopg2.extras import RealDictCursor

class PostgresExecutor:
    """Execute SQL and capture output exactly as psql would."""

    def __init__(self, host='localhost', port=5432, database='postgres',
                 user='postgres', password=None):
        self.host = host
        self.port = port
        self.database = database
        self.user = user
        self.password = password
        self.conn = None

    def connect(self):
        """Establish database connection."""
        self.conn = psycopg2.connect(
            host=self.host,
            port=self.port,
            database=self.database,
            user=self.user,
            password=self.password
        )
        # Set autocommit for DDL statements
        self.conn.autocommit = True

    def disconnect(self):
        """Close database connection."""
        if self.conn:
            self.conn.close()
            self.conn = None

    def execute_sql_psql(self, sql: str) -> str:
        """
        Execute SQL using psql command to get exact output format.

        This ensures we capture output exactly as regression tests expect.
        """
        # Write SQL to temp file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql)
            temp_sql = f.name

        try:
            # Build psql command
            # Use similar settings to pg_regress
            cmd = [
                'psql',
                '-h', self.host,
                '-p', str(self.port),
                '-d', self.database,
                '-U', self.user,
                '--no-psqlrc',  # Don't read .psqlrc
                '-X',  # Don't read .psqlrc
                '-a',  # Echo all input (includes comments)
                '-f', temp_sql
            ]

            # Set PGPASSWORD environment variable if provided
            env = os.environ.copy()
            if self.password:
                env['PGPASSWORD'] = self.password

            # Execute psql and capture output
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                env=env
            )

            # Combine stdout and stderr like regression tests do
            # INFO and NOTICE messages go to stderr in psql
            output = result.stdout
            if result.stderr:
                # Merge stderr into output, preserving INFO/NOTICE messages
                stderr_lines = result.stderr.splitlines()
                output_lines = output.splitlines() if output else []

                # Interleave stderr messages at appropriate positions
                combined = []
                stderr_idx = 0

                for line in output_lines:
                    # Add any stderr messages that should appear before this line
                    while stderr_idx < len(stderr_lines):
                        stderr_line = stderr_lines[stderr_idx]
                        # Keep INFO, NOTICE, WARNING, ERROR messages
                        if any(stderr_line.startswith(prefix) for prefix in ['INFO:', 'NOTICE:', 'WARNING:', 'ERROR:', 'FATAL:', 'PANIC:']):
                            combined.append(stderr_line)
                            stderr_idx += 1
                        elif 'psql:' in stderr_line and 'ERROR:' in stderr_line:
                            # Keep psql error messages
                            combined.append(stderr_line.split('psql:')[1].strip() if 'psql:' in stderr_line else stderr_line)
                            stderr_idx += 1
                        else:
                            stderr_idx += 1
                            break
                    combined.append(line)

                # Add any remaining stderr messages
                while stderr_idx < len(stderr_lines):
                    stderr_line = stderr_lines[stderr_idx]
                    if any(stderr_line.startswith(prefix) for prefix in ['INFO:', 'NOTICE:', 'WARNING:', 'ERROR:', 'FATAL:', 'PANIC:']):
                        combined.append(stderr_line)
                    stderr_idx += 1

                output = '\n'.join(combined)

            return output

        finally:
            # Clean up temp file
            os.unlink(temp_sql)

    def execute_sql_internal(self, sql: str) -> Tuple[Optional[list], Optional[str]]:
        """
        Execute SQL using psycopg2 for internal operations.

        Returns:
            Tuple of (results, error_message)
        """
        if not self.conn:
            self.connect()

        try:
            with self.conn.cursor(cursor_factory=RealDictCursor) as cursor:
                cursor.execute(sql)

                # Try to fetch results if it's a SELECT
                try:
                    results = cursor.fetchall()
                    return results, None
                except psycopg2.ProgrammingError:
                    # Not a SELECT, no results to fetch
                    return None, None

        except Exception as e:
            return None, str(e)

    def get_table_contents(self, table_name: str, order_by: str = 'ctid') -> list:
        """
        Get all rows from a table.

        Args:
            table_name: Name of the table
            order_by: Column to order by (default: ctid)

        Returns:
            List of row dictionaries
        """
        sql = f"SELECT * FROM {table_name} ORDER BY {order_by}"
        results, error = self.execute_sql_internal(sql)

        if error:
            raise Exception(f"Error getting table contents: {error}")

        return results or []

    def table_exists(self, table_name: str) -> bool:
        """Check if a table exists."""
        sql = """
            SELECT EXISTS (
                SELECT 1
                FROM information_schema.tables
                WHERE table_name = %s
            )
        """
        if not self.conn:
            self.connect()

        with self.conn.cursor() as cursor:
            cursor.execute(sql, (table_name,))
            return cursor.fetchone()[0]

    def get_index_info(self, index_name: str) -> dict:
        """Get information about an index."""
        sql = """
            SELECT
                schemaname,
                tablename,
                indexname,
                indexdef
            FROM pg_indexes
            WHERE indexname = %s
        """
        if not self.conn:
            self.connect()

        with self.conn.cursor(cursor_factory=RealDictCursor) as cursor:
            cursor.execute(sql, (index_name,))
            result = cursor.fetchone()
            return dict(result) if result else None

    def reset_database(self):
        """
        Reset the database to a clean state.
        Useful for running tests in isolation.
        """
        if not self.conn:
            self.connect()

        # Drop and recreate the extension
        with self.conn.cursor() as cursor:
            cursor.execute("DROP EXTENSION IF EXISTS pg_textsearch CASCADE")
            cursor.execute("CREATE EXTENSION pg_textsearch")

def format_psql_output(results: list, headers: list) -> str:
    """
    Format query results like psql does.

    Args:
        results: List of row tuples
        headers: List of column names

    Returns:
        Formatted string output
    """
    if not results:
        return "(0 rows)\n"

    # Calculate column widths
    widths = [len(h) for h in headers]
    for row in results:
        for i, val in enumerate(row):
            widths[i] = max(widths[i], len(str(val) if val is not None else ''))

    # Build output
    lines = []

    # Header row
    header_parts = []
    for i, header in enumerate(headers):
        if i == 0:
            header_parts.append(header.ljust(widths[i]))
        else:
            header_parts.append(' | ' + header.ljust(widths[i]))
    lines.append(''.join(header_parts))

    # Separator row
    sep_parts = []
    for i, width in enumerate(widths):
        if i == 0:
            sep_parts.append('-' * width)
        else:
            sep_parts.append('-+-' + '-' * width)
    lines.append(''.join(sep_parts))

    # Data rows
    for row in results:
        row_parts = []
        for i, val in enumerate(row):
            val_str = str(val) if val is not None else ''
            if i == 0:
                row_parts.append(val_str.ljust(widths[i]))
            else:
                row_parts.append(' | ' + val_str.ljust(widths[i]))
        lines.append(''.join(row_parts))

    # Row count
    lines.append(f"({len(results)} row{'s' if len(results) != 1 else ''})")

    return '\n'.join(lines) + '\n'

if __name__ == '__main__':
    # Test executor
    import sys

    if len(sys.argv) < 2:
        print("Usage: postgres_executor.py <sql>")
        sys.exit(1)

    sql = sys.argv[1]

    executor = PostgresExecutor()

    # Test psql-style execution
    print("=== psql output ===")
    output = executor.execute_psql(sql)
    print(output)

    # Test internal execution
    print("\n=== Internal execution ===")
    executor.connect()
    results, error = executor.execute_sql_internal(sql)

    if error:
        print(f"Error: {error}")
    elif results:
        print(f"Results: {results}")
    else:
        print("Query executed successfully")

    executor.disconnect()
