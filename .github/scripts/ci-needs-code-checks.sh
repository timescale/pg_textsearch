#!/usr/bin/env bash

# Usage: .github/scripts/ci-needs-code-checks.sh BASE_REF HEAD_REF
# Print true if the PR-style three-dot diff touches a watched path, otherwise
# false. Errors exit nonzero without printing a decision. No Actions env needed.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 BASE_REF HEAD_REF" >&2
    exit 1
fi

# Resolve paths relative to this script, not the caller's working directory.
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
base_sha=$(git -C "$repo_root" rev-parse --verify --end-of-options "$1^{commit}")
head_sha=$(git -C "$repo_root" rev-parse --verify --end-of-options "$2^{commit}")
changed_files=$(mktemp "${TMPDIR:-/tmp}/pg-textsearch-ci-paths.XXXXXX")
trap 'rm -f -- "$changed_files"' EXIT

# Disable rename detection so moving code out of a watched path still counts.
# Run diff separately so a failure cannot become a false skip. NUL delimiters
# handle arbitrary file names.
git -C "$repo_root" diff --no-renames --name-only -z \
    "$base_sha...$head_sha" > "$changed_files"

run_tests=false
while IFS= read -r -d '' path; do
    case "$path" in
        src/* | test/* | sql/* | Makefile | pg_textsearch.control | \
        .clang-format | .github/workflows/ci.yml | \
        .github/scripts/ci-needs-code-checks.sh | \
        .github/scripts/build-postgres-injection.sh | \
        .github/scripts/build-postgres-sanitizer.sh | \
        scripts/msvc-build.ps1)
            run_tests=true
            break
            ;;
    esac
done < "$changed_files"
printf '%s\n' "$run_tests"
