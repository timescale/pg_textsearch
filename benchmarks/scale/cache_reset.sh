#!/usr/bin/env bash
set -euo pipefail
directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec sudo -A /srv/searchbench/pg_textsearch-scale/venv/bin/python \
    "$directory/cache_reset.py" "$@"
