#!/usr/bin/env python3
"""
Simple template processor that just removes VALIDATE_BM25 comments.

This creates clean SQL files that can be run through psql to generate
expected output files. No validation logic is injected.
"""

import sys
import re

def process_template(template_file, output_file):
    """Remove VALIDATE_BM25 comments from template to create clean SQL."""

    # Pattern to match VALIDATE_BM25 comment lines
    validate_pattern = re.compile(r'^--\s*VALIDATE_BM25:.*$', re.MULTILINE)

    with open(template_file, 'r') as f:
        content = f.read()

    # Remove VALIDATE_BM25 comments
    clean_sql = validate_pattern.sub('', content)

    # Clean up any double blank lines created by removal
    clean_sql = re.sub(r'\n\n\n+', '\n\n', clean_sql)

    with open(output_file, 'w') as f:
        f.write(clean_sql)

    print(f"Generated {output_file} from {template_file}")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} template_file output_file")
        sys.exit(1)

    process_template(sys.argv[1], sys.argv[2])
