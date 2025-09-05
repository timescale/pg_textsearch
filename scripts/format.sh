#!/bin/bash
# Format C files using PostgreSQL's pgindent tool

PGINDENT_PATH="$HOME/postgresql/src/tools/pgindent/pgindent"
INDENT_PATH="$HOME/postgresql/src/tools/pg_bsd_indent/pg_bsd_indent"
TYPEDEFS_PATH="$HOME/postgresql/src/tools/pgindent/typedefs.list"

# Check if pgindent is available
if [ ! -f "$PGINDENT_PATH" ]; then
    echo "pgindent not found at $PGINDENT_PATH"
    echo "This script requires a full PostgreSQL source installation."
    echo "For CI environments, consider using clang-format instead."
    exit 1
fi

if [ ! -f "$INDENT_PATH" ]; then
    echo "pg_bsd_indent not found at $INDENT_PATH"
    exit 1
fi

if [ ! -f "$TYPEDEFS_PATH" ]; then
    echo "typedefs.list not found at $TYPEDEFS_PATH"
    exit 1
fi

# Run pgindent
$PGINDENT_PATH \
  --indent="$INDENT_PATH" \
  --typedefs="$TYPEDEFS_PATH" \
  "$@"