#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT
cp "$ROOT_DIR/benchmarks/run_filtered_seed.sh" "$TEST_DIR/run.sh"

fail()
{
	echo "filtered-seed benchmark test failed: $*" >&2
	exit 1
}

# Exercise the runner without building a benchmark database.
psql()
{
	if [[ "${STUB_RESULT:-1}" == 1 ]]; then
		echo "FSB_RESULT: shape=single sel=0.1 limit=10 scans=1" \
			"off_ms=2 off_passes=3 on_ms=1 on_passes=1 speedup=2"
	fi
	return "${STUB_STATUS:-0}"
}
export -f psql

status=0
STUB_STATUS=3 bash "$TEST_DIR/run.sh" >"$TEST_DIR/failure.log" 2>&1 ||
	status=$?
[[ "$status" == 3 ]] || fail "psql failure was not propagated"
if compgen -G "$TEST_DIR/results/*.json" >/dev/null; then
	fail "failed run published metrics"
fi

if STUB_RESULT=0 bash "$TEST_DIR/run.sh" >"$TEST_DIR/empty.log" 2>&1; then
	fail "empty result was accepted"
fi
if compgen -G "$TEST_DIR/results/*.json" >/dev/null; then
	fail "empty run published metrics"
fi

bash "$TEST_DIR/run.sh" >"$TEST_DIR/success.log" 2>&1
compgen -G "$TEST_DIR/results/*.json" >/dev/null ||
	fail "successful run did not publish metrics"
grep -q '"bm25_scans": 1' "$TEST_DIR"/results/*.json ||
	fail "successful run lost scan metrics"

echo "filtered-seed benchmark tests passed"
