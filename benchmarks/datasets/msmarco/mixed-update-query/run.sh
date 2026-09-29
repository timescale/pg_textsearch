#!/usr/bin/env bash

set -euo pipefail

usage()
{
	cat <<'EOF'
Run concurrent top-10 BM25 queries and rate-limited indexed updates.

The defaults target the MS MARCO v2 tables. Override TABLE, ID_COLUMN,
TEXT_COLUMN, INDEX, QUERY_TABLE, and QUERY_COLUMN for another compatible
corpus, including benchmarks/datasets/msmarco.

Usage:
  run.sh [--dry-run]
  run.sh --help

Connection environment:
  PGHOST, PGPORT, PGUSER, PGDATABASE

Workload environment:
  TABLE=msmarco_v2_passages
  ID_COLUMN=passage_id
  TEXT_COLUMN=passage_text
  INDEX=msmarco_v2_bm25_idx
  QUERY_TABLE=msmarco_v2_queries
  QUERY_COLUMN=query_text
  DURATION=600
  WARMUP=60
  READ_CLIENTS=32
  READ_THREADS=min(READ_CLIENTS, online CPUs)
  UPDATE_RATE=1000
  UPDATE_TARGETS=100000
  OUTPUT_DIR=benchmarks/results/mixed-update-query/<timestamp>

Options:
  --dry-run  Validate configuration and print the resolved values.
  --help     Show this help.
EOF
}

die()
{
	echo "ERROR: $*" >&2
	exit 1
}

validate_identifier()
{
	local name="$1"
	local value="$2"

	[[ "$value" =~ ^[A-Za-z_][A-Za-z0-9_\$]*$ ]] ||
		die "$name must be a simple SQL identifier: $value"
}

validate_relation_name()
{
	local name="$1"
	local value="$2"

	[[ "$value" =~ ^[A-Za-z_][A-Za-z0-9_\$]*(\.[A-Za-z_][A-Za-z0-9_\$]*)?$ ]] ||
		die "$name must be an optionally schema-qualified SQL name: $value"
}

validate_positive_integer()
{
	local name="$1"
	local value="$2"

	[[ "$value" =~ ^[1-9][0-9]*$ ]] ||
		die "$name must be a positive integer: $value"
}

extract_metric()
{
	local pattern="$1"
	local file="$2"

	awk -v pattern="$pattern" '
		$0 ~ pattern {
			for (i = 1; i <= NF; i++)
				if ($i ~ /^[0-9]+([.][0-9]+)?$/)
					value = $i
		}
		END {
			if (value != "")
				print value
		}
	' "$file"
}

latency_p99_ms()
{
	local log_prefix="$1"
	local latency_file

	latency_file="$(mktemp)"
	find "$(dirname "$log_prefix")" -maxdepth 1 -type f \
		-name "$(basename "$log_prefix").*" -print0 |
		xargs -0 -r awk '$3 ~ /^[0-9]+$/ { print $3 / 1000.0 }' \
		>"$latency_file"

	if [[ ! -s "$latency_file" ]]; then
		rm -f "$latency_file"
		echo "n/a"
		return
	fi

	sort -n "$latency_file" |
		awk '
			{ values[NR] = $1 }
			END {
				pos = int((NR * 99 + 99) / 100)
				printf "%.3f\n", values[pos]
			}
		'
	rm -f "$latency_file"
}

wait_for_processes()
{
	local first_pid="$1"
	local second_pid="$2"
	local monitor_file="$3"

	while kill -0 "$first_pid" 2>/dev/null ||
		kill -0 "$second_pid" 2>/dev/null
	do
		if ! psql -X -qAt -F $'\t' -v ON_ERROR_STOP=1 \
			-v reader_app="$READER_APP" \
			-v writer_app="$WRITER_APP" \
			>>"$monitor_file" <<'EOSQL'
SELECT clock_timestamp(),
       application_name,
       COALESCE(wait_event_type, ''),
       COALESCE(wait_event, ''),
       COALESCE(array_to_string(pg_blocking_pids(pid), ','), '')
FROM pg_stat_activity
WHERE datname = current_database()
  AND application_name IN (:'reader_app', :'writer_app')
ORDER BY application_name, pid;
EOSQL
		then
			echo "wait sampler failed; wait-event results are incomplete" >&2
			kill -TERM "$RUNNER_PID"
			return 1
		fi
		sleep 1
	done
}

cleanup_workload()
{
	local status=$?
	local pid

	trap - EXIT INT TERM

	for pid in "${ACTIVE_PID:-}" "${READER_PID:-}" "${WRITER_PID:-}" \
		"${MONITOR_PID:-}"
	do
		if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
			kill "$pid" 2>/dev/null || true
		fi
	done
	for pid in "${ACTIVE_PID:-}" "${READER_PID:-}" "${WRITER_PID:-}" \
		"${MONITOR_PID:-}"
	do
		if [[ -n "$pid" ]]; then
			wait "$pid" 2>/dev/null || true
		fi
	done

	if [[ "${HELPER_SCHEMA_CREATED:-0}" == 1 ]] &&
		command -v psql >/dev/null
	then
		if ! psql -X -q -v ON_ERROR_STOP=1 \
			-c "DROP SCHEMA IF EXISTS $HELPER_SCHEMA CASCADE" >/dev/null
		then
			echo "WARNING: could not remove benchmark helper schema" >&2
		fi
	fi

	if [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
		rm -rf "$WORK_DIR"
	fi

	exit "$status"
}

run_reader()
{
	local output_file="$1"
	local log_prefix="$2"

	exec env PGAPPNAME="$READER_APP" pgbench -n \
		-c "$READ_CLIENTS" \
		-j "$READ_THREADS" \
		-T "$DURATION" \
		-P 10 \
		-l \
		--log-prefix="$log_prefix" \
		-f "$READER_SQL" \
		>"$output_file" 2>&1
}

TABLE="${TABLE:-msmarco_v2_passages}"
ID_COLUMN="${ID_COLUMN:-passage_id}"
TEXT_COLUMN="${TEXT_COLUMN:-passage_text}"
INDEX="${INDEX:-msmarco_v2_bm25_idx}"
QUERY_TABLE="${QUERY_TABLE:-msmarco_v2_queries}"
QUERY_COLUMN="${QUERY_COLUMN:-query_text}"
DURATION="${DURATION:-600}"
WARMUP="${WARMUP:-60}"
READ_CLIENTS="${READ_CLIENTS:-32}"
UPDATE_RATE="${UPDATE_RATE:-1000}"
UPDATE_TARGETS="${UPDATE_TARGETS:-100000}"
validate_positive_integer "READ_CLIENTS" "$READ_CLIENTS"
CPU_COUNT="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
if [[ ! "$CPU_COUNT" =~ ^[1-9][0-9]*$ ]]; then
	CPU_COUNT=1
fi
if (( CPU_COUNT < READ_CLIENTS )); then
	DEFAULT_READ_THREADS="$CPU_COUNT"
else
	DEFAULT_READ_THREADS="$READ_CLIENTS"
fi
READ_THREADS="${READ_THREADS:-$DEFAULT_READ_THREADS}"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
ROOT_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT_DIR/benchmarks/results/mixed-update-query/$TIMESTAMP}"
RUN_NONCE="$(LC_ALL=C od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
[[ "$RUN_NONCE" =~ ^[0-9a-f]{16}$ ]] ||
	die "could not generate a benchmark run identifier"
RUN_SUFFIX="${TIMESTAMP//[^A-Za-z0-9_]/_}_$$_$RUN_NONCE"
HELPER_SCHEMA="pgts_benchmark_$RUN_SUFFIX"
TARGET_TABLE="update_targets"
QUERY_POOL_TABLE="query_pool"
READER_APP="pgts-reader-$RUN_SUFFIX"
WRITER_APP="pgts-writer-$RUN_SUFFIX"
RUNNER_PID=$$
WORK_DIR=""
ACTIVE_PID=""
READER_PID=""
WRITER_PID=""
MONITOR_PID=""
HELPER_SCHEMA_CREATED=0

case "${1:-}" in
	--help|-h)
		usage
		exit 0
		;;
	--dry-run)
		DRY_RUN=1
		;;
	"")
		DRY_RUN=0
		;;
	*)
		usage >&2
		die "unknown option: $1"
		;;
esac

for pair in \
	"TABLE:$TABLE" \
	"INDEX:$INDEX" \
	"QUERY_TABLE:$QUERY_TABLE"
do
	validate_relation_name "${pair%%:*}" "${pair#*:}"
done

for pair in \
	"ID_COLUMN:$ID_COLUMN" \
	"TEXT_COLUMN:$TEXT_COLUMN" \
	"QUERY_COLUMN:$QUERY_COLUMN"
do
	validate_identifier "${pair%%:*}" "${pair#*:}"
done

for pair in \
	"DURATION:$DURATION" \
	"WARMUP:$WARMUP" \
	"READ_CLIENTS:$READ_CLIENTS" \
	"READ_THREADS:$READ_THREADS" \
	"UPDATE_RATE:$UPDATE_RATE" \
	"UPDATE_TARGETS:$UPDATE_TARGETS"
do
	validate_positive_integer "${pair%%:*}" "${pair#*:}"
done

cat <<EOF
table=$TABLE
id_column=$ID_COLUMN
text_column=$TEXT_COLUMN
index=$INDEX
query_table=$QUERY_TABLE
query_column=$QUERY_COLUMN
duration_seconds=$DURATION
warmup_seconds=$WARMUP
read_clients=$READ_CLIENTS
read_threads=$READ_THREADS
update_rate=$UPDATE_RATE
update_targets=$UPDATE_TARGETS
output_dir=$OUTPUT_DIR
EOF

if (( DRY_RUN )); then
	exit 0
fi

command -v psql >/dev/null || die "psql is not available"
command -v pgbench >/dev/null || die "pgbench is not available"

mkdir -p "$(dirname "$OUTPUT_DIR")"
if ! mkdir "$OUTPUT_DIR"; then
	if [[ -e "$OUTPUT_DIR" ]]; then
		die "output directory already exists: $OUTPUT_DIR"
	fi
	die "could not create output directory: $OUTPUT_DIR"
fi
WORK_DIR="$(mktemp -d "$OUTPUT_DIR/work.XXXXXX")"

export PGHOST="${PGHOST:-127.0.0.1}"
export PGPORT="${PGPORT:-5432}"
export PGUSER="${PGUSER:-$USER}"
export PGDATABASE="${PGDATABASE:-postgres}"

trap 'exit 130' INT TERM
trap cleanup_workload EXIT

psql -X -qAt -v ON_ERROR_STOP=1 <<EOSQL >/dev/null
SELECT '$TABLE'::regclass;
SELECT '$INDEX'::regclass;
SELECT '$QUERY_TABLE'::regclass;
EOSQL

SETUP_SQL="$WORK_DIR/setup.sql"
READER_SQL="$WORK_DIR/reader.sql"
WRITER_SQL="$WORK_DIR/writer.sql"

cat >"$SETUP_SQL" <<EOSQL
\set ON_ERROR_STOP on

BEGIN;

CREATE SCHEMA $HELPER_SCHEMA;

CREATE UNLOGGED TABLE $HELPER_SCHEMA.$TARGET_TABLE AS
SELECT row_number() OVER ()::bigint AS target_id,
       $ID_COLUMN AS document_id
FROM $TABLE
LIMIT $UPDATE_TARGETS;
ALTER TABLE $HELPER_SCHEMA.$TARGET_TABLE
    ADD PRIMARY KEY (target_id);

CREATE UNLOGGED TABLE $HELPER_SCHEMA.$QUERY_POOL_TABLE AS
SELECT row_number() OVER ()::bigint AS query_id,
       $QUERY_COLUMN::text AS query_text
FROM $QUERY_TABLE
WHERE $QUERY_COLUMN IS NOT NULL
  AND $QUERY_COLUMN <> '';
ALTER TABLE $HELPER_SCHEMA.$QUERY_POOL_TABLE
    ADD PRIMARY KEY (query_id);

ANALYZE $HELPER_SCHEMA.$TARGET_TABLE;
ANALYZE $HELPER_SCHEMA.$QUERY_POOL_TABLE;

COMMIT;
EOSQL

psql -X -q -v ON_ERROR_STOP=1 -f "$SETUP_SQL" &
ACTIVE_PID=$!
setup_status=0
if wait "$ACTIVE_PID"; then
	setup_status=0
else
	setup_status=$?
fi
ACTIVE_PID=""
(( setup_status == 0 )) ||
	die "benchmark setup failed; see the preceding psql error"
HELPER_SCHEMA_CREATED=1

TARGET_COUNT="$(
	psql -X -qAt -v ON_ERROR_STOP=1 \
		-c "SELECT count(*) FROM $HELPER_SCHEMA.$TARGET_TABLE"
)"
QUERY_COUNT="$(
	psql -X -qAt -v ON_ERROR_STOP=1 \
		-c "SELECT count(*) FROM $HELPER_SCHEMA.$QUERY_POOL_TABLE"
)"

(( TARGET_COUNT > 0 )) || die "no update targets were selected"
(( QUERY_COUNT > 0 )) || die "no benchmark queries were selected"

cat >"$READER_SQL" <<EOSQL
\set query_id random(1, $QUERY_COUNT)
SELECT $ID_COLUMN
FROM $TABLE
ORDER BY $TEXT_COLUMN <@> to_bm25query(
    (SELECT query_text
     FROM $HELPER_SCHEMA.$QUERY_POOL_TABLE
     WHERE query_id = :query_id),
    '$INDEX')
LIMIT 10;
EOSQL

cat >"$WRITER_SQL" <<EOSQL
\set target_id random(1, $TARGET_COUNT)
UPDATE $TABLE AS documents
SET $TEXT_COLUMN = CASE
    WHEN right(documents.$TEXT_COLUMN, 11) = ' pgtsupdate'
        THEN left(documents.$TEXT_COLUMN,
                  length(documents.$TEXT_COLUMN) - 11)
    ELSE documents.$TEXT_COLUMN || ' pgtsupdate'
END
FROM $HELPER_SCHEMA.$TARGET_TABLE AS targets
WHERE targets.target_id = :target_id
  AND documents.$ID_COLUMN = targets.document_id;
EOSQL

POSTGRES_VERSION="$(psql -X -qAt -c 'SHOW server_version')"
EXTENSION_VERSION="$(
	psql -X -qAt -c \
		"SELECT extversion FROM pg_extension WHERE extname = 'pg_textsearch'"
)"
TABLE_ROWS="$(psql -X -qAt -c "SELECT count(*) FROM $TABLE")"
IFS=$'\t' read -r DATABASE_NAME DATABASE_USER SERVER_ADDRESS SERVER_PORT < <(
	psql -X -qAt -F $'\t' <<'EOSQL'
SELECT current_database(),
       current_user,
       COALESCE(inet_server_addr()::text, 'local'),
       COALESCE(inet_server_port()::text, 'local');
EOSQL
)

{
	echo "timestamp=$TIMESTAMP"
	echo "postgres_version=$POSTGRES_VERSION"
	echo "extension_version=$EXTENSION_VERSION"
	echo "database=$DATABASE_NAME"
	echo "database_user=$DATABASE_USER"
	echo "server_address=$SERVER_ADDRESS"
	echo "server_port=$SERVER_PORT"
	echo "table=$TABLE"
	echo "id_column=$ID_COLUMN"
	echo "text_column=$TEXT_COLUMN"
	echo "index=$INDEX"
	echo "query_table=$QUERY_TABLE"
	echo "query_column=$QUERY_COLUMN"
	echo "table_rows=$TABLE_ROWS"
	echo "query_count=$QUERY_COUNT"
	echo "target_count=$TARGET_COUNT"
	echo "duration_seconds=$DURATION"
	echo "warmup_seconds=$WARMUP"
	echo "read_clients=$READ_CLIENTS"
	echo "read_threads=$READ_THREADS"
	echo "update_rate=$UPDATE_RATE"
	echo "update_targets=$UPDATE_TARGETS"
} >"$OUTPUT_DIR/metadata.env"

echo
echo "Warming the query workload for ${WARMUP}s..."
env PGAPPNAME="$READER_APP" pgbench -n -c 1 -j 1 -T "$WARMUP" \
	-f "$READER_SQL" >"$OUTPUT_DIR/warmup.log" 2>&1 &
ACTIVE_PID=$!
warmup_status=0
if wait "$ACTIVE_PID"; then
	warmup_status=0
else
	warmup_status=$?
fi
ACTIVE_PID=""
(( warmup_status == 0 )) ||
	die "warmup pgbench failed; see $OUTPUT_DIR/warmup.log"

echo "Running read-only phase for ${DURATION}s..."
READ_ONLY_LOG_PREFIX="$OUTPUT_DIR/read_only_pgbench"
run_reader "$OUTPUT_DIR/read_only.out" "$READ_ONLY_LOG_PREFIX" &
ACTIVE_PID=$!
read_only_status=0
if wait "$ACTIVE_PID"; then
	read_only_status=0
else
	read_only_status=$?
fi
ACTIVE_PID=""
(( read_only_status == 0 )) ||
	die "read-only pgbench failed; see $OUTPUT_DIR/read_only.out"

READ_ONLY_QPS="$(extract_metric '^tps = ' "$OUTPUT_DIR/read_only.out")"
READ_ONLY_AVG_MS="$(
	extract_metric '^latency average = ' "$OUTPUT_DIR/read_only.out"
)"
READ_ONLY_TXNS="$(
	extract_metric '^number of transactions actually processed:' \
		"$OUTPUT_DIR/read_only.out"
)"
READ_ONLY_P99_MS="$(latency_p99_ms "$READ_ONLY_LOG_PREFIX")"

echo "Running mixed read/update phase for ${DURATION}s..."
MIXED_LOG_PREFIX="$OUTPUT_DIR/mixed_reader_pgbench"
MIXED_WRITER_LOG_PREFIX="$OUTPUT_DIR/mixed_writer_pgbench"
env PGAPPNAME="$READER_APP" pgbench -n \
	-c "$READ_CLIENTS" \
	-j "$READ_THREADS" \
	-T "$DURATION" \
	-P 10 \
	-l \
	--log-prefix="$MIXED_LOG_PREFIX" \
	-f "$READER_SQL" \
	>"$OUTPUT_DIR/mixed_reader.out" 2>&1 &
READER_PID=$!

env PGAPPNAME="$WRITER_APP" pgbench -n \
	-c 1 \
	-j 1 \
	-T "$DURATION" \
	-R "$UPDATE_RATE" \
	-P 10 \
	-l \
	--log-prefix="$MIXED_WRITER_LOG_PREFIX" \
	-f "$WRITER_SQL" \
	>"$OUTPUT_DIR/mixed_writer.out" 2>&1 &
WRITER_PID=$!

: >"$OUTPUT_DIR/waits.tsv"
wait_for_processes "$READER_PID" "$WRITER_PID" "$OUTPUT_DIR/waits.tsv" &
MONITOR_PID=$!

finished_status=0
finished_pid=""
if wait -n -p finished_pid "$READER_PID" "$WRITER_PID"; then
	finished_status=0
else
	finished_status=$?
fi

if [[ "$finished_pid" == "$READER_PID" ]]; then
	finished_role="reader"
	reader_status=$finished_status
	remaining_pid="$WRITER_PID"
	remaining_role="writer"
else
	finished_role="writer"
	writer_status=$finished_status
	remaining_pid="$READER_PID"
	remaining_role="reader"
fi

if (( finished_status != 0 )) && kill -0 "$remaining_pid" 2>/dev/null; then
	kill "$remaining_pid"
fi

remaining_status=0
if wait "$remaining_pid"; then
	remaining_status=0
else
	remaining_status=$?
fi

if [[ "$remaining_pid" == "$READER_PID" ]]; then
	reader_status=$remaining_status
else
	writer_status=$remaining_status
fi

monitor_status=0
if wait "$MONITOR_PID"; then
	monitor_status=0
else
	monitor_status=$?
fi

READER_PID=""
WRITER_PID=""
MONITOR_PID=""

if (( finished_status != 0 )); then
	die "$finished_role pgbench failed; see \
$OUTPUT_DIR/mixed_${finished_role}.out"
fi
if (( remaining_status != 0 )); then
	die "$remaining_role pgbench failed; see \
$OUTPUT_DIR/mixed_${remaining_role}.out"
fi
(( reader_status == 0 )) ||
	die "reader pgbench failed; see $OUTPUT_DIR/mixed_reader.out"
(( writer_status == 0 )) ||
	die "writer pgbench failed; see $OUTPUT_DIR/mixed_writer.out"
(( monitor_status == 0 )) ||
	die "wait sampler failed; see $OUTPUT_DIR/waits.tsv"

MIXED_QPS="$(extract_metric '^tps = ' "$OUTPUT_DIR/mixed_reader.out")"
MIXED_AVG_MS="$(
	extract_metric '^latency average = ' "$OUTPUT_DIR/mixed_reader.out"
)"
MIXED_TXNS="$(
	extract_metric '^number of transactions actually processed:' \
		"$OUTPUT_DIR/mixed_reader.out"
)"
MIXED_P99_MS="$(latency_p99_ms "$MIXED_LOG_PREFIX")"
UPDATES="$(
	extract_metric '^number of transactions actually processed:' \
		"$OUTPUT_DIR/mixed_writer.out"
)"
UPDATE_TPS="$(extract_metric '^tps = ' "$OUTPUT_DIR/mixed_writer.out")"
UPDATE_AVG_MS="$(
	extract_metric '^latency average = ' "$OUTPUT_DIR/mixed_writer.out"
)"

cat >"$OUTPUT_DIR/summary.tsv" <<EOF
phase	qps	avg_latency_ms	p99_latency_ms	queries	updates	update_tps	update_avg_latency_ms
read_only	$READ_ONLY_QPS	$READ_ONLY_AVG_MS	$READ_ONLY_P99_MS	$READ_ONLY_TXNS	0	0	0
with_updates	$MIXED_QPS	$MIXED_AVG_MS	$MIXED_P99_MS	$MIXED_TXNS	$UPDATES	$UPDATE_TPS	$UPDATE_AVG_MS
EOF

echo
column -t -s $'\t' "$OUTPUT_DIR/summary.tsv" 2>/dev/null ||
	cat "$OUTPUT_DIR/summary.tsv"
echo
echo "Lock-wait samples:"
{
	printf 'samples\tapplication_name\twait_event_type\twait_event\n'
	awk -F '\t' '
	NF >= 4 {
		type = ($3 == "" ? "CPU" : $3)
		event = ($4 == "" ? "running" : $4)
		key = $2 "\t" type "\t" event
		count[key]++
	}
	END {
		for (key in count)
			printf "%d\t%s\n", count[key], key
	}
	' "$OUTPUT_DIR/waits.tsv" | sort -nr
} >"$OUTPUT_DIR/wait_summary.tsv"
if [[ "$(wc -l <"$OUTPUT_DIR/wait_summary.tsv")" -gt 1 ]]; then
	head -20 "$OUTPUT_DIR/wait_summary.tsv"
else
	echo "(none sampled)"
fi
echo
echo "Results: $OUTPUT_DIR"
