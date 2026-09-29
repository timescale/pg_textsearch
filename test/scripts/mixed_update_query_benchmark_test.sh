#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
RUNNER="$ROOT_DIR/benchmarks/datasets/msmarco/mixed-update-query/run.sh"

fail()
{
	echo "mixed update/query benchmark test failed: $*" >&2
	exit 1
}

[[ -x "$RUNNER" ]] || fail "runner is missing or not executable"
bash -n "$RUNNER"

if grep -nE "<<'EOSQL'.*\\|\\|[[:space:]]*$" "$RUNNER"; then
	fail "heredoc command continuation can leak shell text into SQL"
fi

grep -Fq -- "-F \$'\\t'" "$RUNNER" ||
	fail "wait-event samples are not emitted as tab-separated data"

grep -q "wait_summary.tsv" "$RUNNER" ||
	fail "runner does not persist aggregated wait-event counts"
grep -Fq ': >"$OUTPUT_DIR/waits.tsv"' "$RUNNER" ||
	fail "runner does not create the wait sample file before monitoring"

if grep -q "env | grep '\\^PG'" "$RUNNER"; then
	fail "runner can persist PostgreSQL credentials"
fi
if grep -q 'pg_database=\$PGDATABASE' "$RUNNER"; then
	fail "runner can persist credentials embedded in PGDATABASE"
fi

grep -q 'HELPER_SCHEMA="pgts_benchmark_$RUN_SUFFIX"' "$RUNNER" ||
	fail "helper tables are not isolated in a benchmark schema"
if grep -q 'HELPER_SCHEMA="pg_' "$RUNNER"; then
	fail "helper schema uses PostgreSQL's reserved pg_ prefix"
fi
grep -q "RUN_SUFFIX" "$RUNNER" ||
	fail "helper table names are not unique per run"
grep -q "output directory already exists" "$RUNNER" ||
	fail "runner does not atomically claim the output directory"
grep -q "READER_APP=.*RUN_SUFFIX" "$RUNNER" ||
	fail "reader application names are not unique per run"
grep -q "WRITER_APP=.*RUN_SUFFIX" "$RUNNER" ||
	fail "writer application names are not unique per run"
grep -q "wait sampler failed" "$RUNNER" ||
	fail "wait sampler failures are not surfaced"
grep -q 'kill -TERM "$RUNNER_PID"' "$RUNNER" ||
	fail "wait sampler failure does not stop the workload"
grep -q "wait -n -p" "$RUNNER" ||
	fail "runner does not detect the first workload process failure"
if grep -q "if ! wait" "$RUNNER"; then
	fail "negated wait commands lose the child exit status"
fi
grep -q "ACTIVE_PID" "$RUNNER" ||
	fail "foreground benchmark phases are not signal-safe"
grep -q "finished_role" "$RUNNER" ||
	fail "runner does not preserve the primary workload failure"
grep -q "cleanup_workload" "$RUNNER" ||
	fail "runner does not clean up active clients and helper tables"

help_output="$("$RUNNER" --help)"
grep -q "READ_CLIENTS" <<<"$help_output" ||
	fail "help does not document READ_CLIENTS"
grep -q "UPDATE_RATE" <<<"$help_output" ||
	fail "help does not document UPDATE_RATE"
grep -q -- "--dry-run" <<<"$help_output" ||
	fail "help does not document --dry-run"

dry_run_output="$(
	TABLE=docs \
	ID_COLUMN=id \
	TEXT_COLUMN=body \
	INDEX=docs_bm25 \
	QUERY_TABLE=queries \
	QUERY_COLUMN=query_text \
	DURATION=42 \
	READ_CLIENTS=7 \
	UPDATE_RATE=123 \
	UPDATE_TARGETS=321 \
	OUTPUT_DIR=/tmp/mixed-results \
	"$RUNNER" --dry-run
)"

for expected in \
	"table=docs" \
	"id_column=id" \
	"text_column=body" \
	"index=docs_bm25" \
	"query_table=queries" \
	"query_column=query_text" \
	"duration_seconds=42" \
	"read_clients=7" \
	"update_rate=123" \
	"update_targets=321" \
	"output_dir=/tmp/mixed-results"
do
	grep -q "^${expected}$" <<<"$dry_run_output" ||
		fail "dry-run output is missing ${expected}"
done

qualified_output="$(
		TABLE=search.docs \
		ID_COLUMN=id \
		TEXT_COLUMN=body \
		INDEX=search.docs_bm25 \
		QUERY_TABLE=search.queries \
		QUERY_COLUMN=query_text \
		"$RUNNER" --dry-run
)"
grep -q '^table=search.docs$' <<<"$qualified_output" ||
		fail "schema-qualified table names are rejected"
grep -q '^index=search.docs_bm25$' <<<"$qualified_output" ||
		fail "schema-qualified index names are rejected"

invalid_output="$(
		TABLE='docs; DROP TABLE docs' "$RUNNER" --dry-run 2>&1
)" && fail "unsafe SQL identifier was accepted"

grep -q "TABLE must be an optionally schema-qualified SQL name" \
		<<<"$invalid_output" ||
		fail "unsafe identifier error was not actionable"

arithmetic_marker="/tmp/pgts-benchmark-arithmetic-$$"
rm -f "$arithmetic_marker"
if READ_CLIENTS="CPU_COUNT[\$(touch $arithmetic_marker)0]" \
		"$RUNNER" --dry-run >/dev/null 2>&1
then
		fail "unsafe numeric input was accepted"
fi
if [[ -e "$arithmetic_marker" ]]; then
		rm -f "$arithmetic_marker"
		fail "READ_CLIENTS was evaluated before validation"
fi

STUB_TMP="$(mktemp -d)"
trap 'rm -rf "$STUB_TMP"' EXIT
mkdir "$STUB_TMP/bin"

cat >"$STUB_TMP/bin/psql" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

command_text=""
file=""
while (($#)); do
		case "$1" in
			-f)
				shift
				file="${1:-}"
				;;
			-*c*)
				shift
				command_text="${1:-}"
				;;
		esac
		shift || true
done

if [[ -n "$file" ]]; then
		echo "SETUP $file" >>"$STUB_LOG"
		[[ "${STUB_FAIL_SETUP:-0}" != 1 ]] || exit 8
		exit 0
fi

if [[ -n "$command_text" ]]; then
		case "$command_text" in
			*"DROP SCHEMA"*)
				echo "DROP $command_text" >>"$STUB_LOG"
				;;
			*"SHOW server_version"*)
				echo "18.6"
				;;
			*"extversion"*)
				echo "1.5.0-dev"
				;;
			*"count(*) FROM pgts_benchmark_"*)
				echo "10"
				;;
			*"count(*) FROM "*)
				echo "1000"
				;;
		esac
		exit 0
fi

input="$(cat)"
if [[ "$input" == *"FROM pg_stat_activity"* ]]; then
		[[ "${STUB_FAIL_SAMPLER:-0}" != 1 ]] || exit 9
		printf '2026-01-01 00:00:00+00\tstub\tLWLock\ttapir_index_lock\t\n'
elif [[ "$input" == *"current_database()"* ]]; then
		printf 'stubdb\tstubuser\tlocal\t5432\n'
fi
EOF

cat >"$STUB_TMP/bin/pgbench" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
trap 'exit 143' TERM

log_prefix=""
script=""
while (($#)); do
		case "$1" in
			--log-prefix=*)
				log_prefix="${1#*=}"
				;;
			-f)
				shift
				script="${1:-}"
				;;
		esac
		shift || true
done

if [[ -n "$log_prefix" ]]; then
		printf '0 1 1000 0 0 0\n' >"$log_prefix.$$"
fi

if [[ "${STUB_FAIL_WRITER:-0}" == 1 &&
		"${PGAPPNAME:-}" == pgts-writer-* ]]
then
		exit 7
fi
if [[ "${STUB_FAIL_WRITER:-0}" == 1 &&
		"$log_prefix" == *mixed_reader_pgbench ]]
then
		sleep 5
fi
if [[ "${STUB_FAIL_SAMPLER:-0}" == 1 &&
		"$log_prefix" == *mixed_*_pgbench ]]
then
		sleep 5
fi
if [[ "${STUB_LONG_WARMUP:-0}" == 1 && -z "$log_prefix" ]]; then
		sleep 5
fi

printf '%s\n' \
		"number of transactions actually processed: 10" \
		"latency average = 1.000 ms" \
		"tps = 10.000 (without initial connection time)"
EOF

chmod +x "$STUB_TMP/bin/psql" "$STUB_TMP/bin/pgbench"

run_stubbed()
{
		env \
			PATH="$STUB_TMP/bin:$PATH" \
			STUB_LOG="$STUB_TMP/psql.log" \
			USER=stubuser \
			PGDATABASE='postgresql://stubuser:secret@stubhost/stubdb' \
			TABLE=search.docs \
			ID_COLUMN=id \
			TEXT_COLUMN=body \
			INDEX=search.docs_bm25 \
			QUERY_TABLE=search.queries \
			QUERY_COLUMN=query_text \
			DURATION=1 \
			WARMUP=1 \
			READ_CLIENTS=2 \
			READ_THREADS=2 \
			UPDATE_RATE=10 \
			UPDATE_TARGETS=10 \
			"$@" \
			"$RUNNER"
}

stub_output="$STUB_TMP/results"
run_stubbed OUTPUT_DIR="$stub_output" >"$STUB_TMP/normal.out"
grep -q '^database=stubdb$' "$stub_output/metadata.env" ||
		fail "metadata does not record the resolved database"
if grep -q 'secret\|postgresql://' "$stub_output/metadata.env"; then
		fail "metadata persists credentials from PGDATABASE"
fi
for setting in table index duration_seconds read_clients update_rate; do
		grep -q "^${setting}=" "$stub_output/metadata.env" ||
			fail "metadata omits $setting"
done
find "$stub_output" -name 'mixed_writer_pgbench.*' -print -quit |
		grep -q . || fail "writer transaction logs are not preserved"
grep -q $'^samples\tapplication_name\twait_event_type\twait_event$' \
		"$stub_output/wait_summary.tsv" ||
		fail "wait summary is not headered TSV"
grep -q '^DROP DROP SCHEMA' "$STUB_TMP/psql.log" ||
		fail "successful run does not remove its helper schema"

if run_stubbed OUTPUT_DIR="$stub_output" >"$STUB_TMP/collision.out" 2>&1
then
		fail "existing output directory was reused"
fi
grep -q 'output directory already exists' "$STUB_TMP/collision.out" ||
		fail "output collision error is not actionable"

: >"$STUB_TMP/psql.log"
if run_stubbed STUB_FAIL_SETUP=1 OUTPUT_DIR="$STUB_TMP/setup-failure" \
		>"$STUB_TMP/setup-failure.out" 2>&1
then
		fail "setup failure was ignored"
fi
if grep -q '^DROP ' "$STUB_TMP/psql.log"; then
		fail "runner drops a helper schema it did not successfully create"
fi

: >"$STUB_TMP/psql.log"
if run_stubbed STUB_FAIL_WRITER=1 OUTPUT_DIR="$STUB_TMP/writer-failure" \
		>"$STUB_TMP/writer-failure.out" 2>&1
then
		fail "writer failure was ignored"
fi
grep -q 'writer pgbench failed' "$STUB_TMP/writer-failure.out" ||
		fail "terminated reader obscures the primary writer failure"
grep -q '^DROP ' "$STUB_TMP/psql.log" ||
		fail "writer failure does not clean up the helper schema"

: >"$STUB_TMP/psql.log"
if run_stubbed STUB_FAIL_SAMPLER=1 OUTPUT_DIR="$STUB_TMP/sampler-failure" \
		>"$STUB_TMP/sampler-failure.out" 2>&1
then
		fail "wait sampler failure was ignored"
fi
grep -q 'wait sampler failed' "$STUB_TMP/sampler-failure.out" ||
		fail "wait sampler failure is not reported"
grep -q '^DROP ' "$STUB_TMP/psql.log" ||
		fail "wait sampler failure does not clean up the helper schema"

: >"$STUB_TMP/psql.log"
env \
		PATH="$STUB_TMP/bin:$PATH" \
		STUB_LOG="$STUB_TMP/psql.log" \
		STUB_LONG_WARMUP=1 \
		USER=stubuser \
		PGDATABASE=stubdb \
		TABLE=search.docs \
		ID_COLUMN=id \
		TEXT_COLUMN=body \
		INDEX=search.docs_bm25 \
		QUERY_TABLE=search.queries \
		QUERY_COLUMN=query_text \
		DURATION=1 \
		WARMUP=1 \
		READ_CLIENTS=2 \
		READ_THREADS=2 \
		UPDATE_RATE=10 \
		UPDATE_TARGETS=10 \
		OUTPUT_DIR="$STUB_TMP/interrupted" \
		"$RUNNER" \
		>"$STUB_TMP/interrupted.out" 2>&1 &
runner_pid=$!
for _ in {1..40}; do
		[[ -f "$STUB_TMP/interrupted/warmup.log" ]] && break
		sleep 0.05
done
[[ -f "$STUB_TMP/interrupted/warmup.log" ]] ||
		fail "interruption test did not reach the warmup phase"
kill -TERM "$runner_pid"
if wait "$runner_pid"; then
		fail "interrupted runner exited successfully"
fi
grep -q '^DROP ' "$STUB_TMP/psql.log" ||
		fail "signal interruption does not clean up the helper schema"

echo "mixed update/query benchmark tests passed"
