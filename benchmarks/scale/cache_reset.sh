#!/usr/bin/env bash
set -euo pipefail
directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec sudo -A /usr/bin/python3 \
    "$directory/cache_reset.py" "$@"
