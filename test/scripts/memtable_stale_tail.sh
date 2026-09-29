#!/bin/bash
#
# A corrupt metapage tail must fail promptly, not trap writers in a retry loop.
#

set -euo pipefail

PG_CONFIG="${PG_CONFIG:-pg_config}"
export PATH="$("${PG_CONFIG}" --bindir):${PATH}"
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pg_textsearch_stale_tail.XXXXXX")
DATA_DIR="${TEST_DIR}/data"
LOGFILE="${TEST_DIR}/postgres.log"
export PGHOST="${TEST_DIR}"
export PGPORT=55469
export PGDATABASE=postgres
clients=()

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate -w >/dev/null
    fi
    for pid in "${clients[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
    if [ "${status}" -ne 0 ]; then
        for logfile in "${TEST_DIR}"/*.log; do
            [ -f "${logfile}" ] || continue
            echo "==> ${logfile} <==" >&2
            tail -n 60 "${logfile}" >&2
        done
    fi
    rm -rf "${TEST_DIR}"
    exit "${status}"
}
trap cleanup EXIT INT TERM

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

sql() {
    psql -XqAt -v ON_ERROR_STOP=1 "$@"
}

initdb -D "${DATA_DIR}" -A trust --no-locale >/dev/null
cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${PGPORT}
unix_socket_directories = '${PGHOST}'
listen_addresses = ''
shared_preload_libraries = 'pg_textsearch'
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.bulk_load_threshold = 0
EOF
pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null
sql <<'SQL'
CREATE EXTENSION pg_textsearch;
CREATE TABLE docs (id int, body text);
CREATE INDEX docs_bm25 ON docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
INSERT INTO docs
SELECT i, (SELECT string_agg(md5(i::text || ':' || j::text), ' ')
           FROM generate_series(1, 60) j)
FROM generate_series(1, 400) i;
SQL
relation=$(sql -c "SELECT pg_relation_filepath('docs_bm25')")
block_size=$(sql -c "SHOW block_size")
checksums=$(sql -c "SHOW data_checksums")
pg_ctl stop -D "${DATA_DIR}" -m fast -w >/dev/null
if [ "${checksums}" = on ]; then
    pg_checksums --disable -D "${DATA_DIR}" >/dev/null
fi

# The frozen v6 prefix is 108 bytes; v7 appends head and tail block numbers.
# PostgreSQL's page header is 24 bytes. Only modify our stopped test cluster.
python3 - "${DATA_DIR}/${relation}" "${block_size}" <<'PY'
import os
import struct
import sys

with open(sys.argv[1], "r+b") as index:
    index.seek(24 + 108)
    head, tail = struct.unpack("=II", index.read(8))
    assert head != tail and head != 0xFFFFFFFF and tail != 0xFFFFFFFF
    index.seek(head * int(sys.argv[2]) + 24 + 12)
    assert struct.unpack("=I", index.read(4))[0] != 0xFFFFFFFF
    index.seek(24 + 112)
    index.write(struct.pack("=I", head))
    index.flush()
    os.fsync(index.fileno())
PY

pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null
queries=(
    "INSERT INTO docs VALUES (401, 'normal short document')"
    "INSERT INTO docs SELECT 402, string_agg(md5(j::text), ' ')
         FROM generate_series(1, 1000) j"
    "UPDATE docs SET body = 'changed indexed document' WHERE id = 1"
)
labels=(regular oversized update)
for i in "${!queries[@]}"; do
    sql -v VERBOSITY=verbose -c \
        "SET statement_timeout = '5s'; ${queries[i]}" \
        >"${TEST_DIR}/${labels[i]}.log" 2>&1 &
    clients+=("$!")
done

# statement_timeout cannot break the old loop while its LWLock is held.
deadline=$((SECONDS + 10))
for i in "${!clients[@]}"; do
    while kill -0 "${clients[i]}" 2>/dev/null; do
        ((SECONDS < deadline)) ||
            fail "${labels[i]} writer did not reject the stale tail"
        sleep 0.05
    done
    if wait "${clients[i]}"; then
        fail "${labels[i]} writer accepted the corrupt tail"
    fi
    grep -Fq 'ERROR:  XX002:' "${TEST_DIR}/${labels[i]}.log" ||
        fail "${labels[i]} writer did not report index corruption"
    grep -Fq 'HINT:  REINDEX the index.' "${TEST_DIR}/${labels[i]}.log" ||
        fail "${labels[i]} writer did not provide a repair hint"
done
clients=()
[ "$(sql -c 'SELECT count(*) FROM docs')" = 400 ] ||
    fail "failed writes changed the table"

# Error cleanup must release locks, and the advertised repair must work.
sql -c "SET statement_timeout = '10s'; REINDEX INDEX docs_bm25"
for query in "${queries[@]}"; do
    sql -c "SET statement_timeout = '5s'; ${query}"
done
[ "$(sql -c 'SELECT count(*) FROM docs')" = 402 ] ||
    fail "writes after REINDEX did not succeed"

# Healthy concurrent extensions must still retry, for both record sizes.
for terms in 60 1000; do
    cat >"${TEST_DIR}/insert.sql" <<EOF
INSERT INTO docs
SELECT 0, string_agg(md5(j::text), ' ')
FROM generate_series(1, ${terms}) j;
EOF
    pgbench -n -c 8 -t 25 -f "${TEST_DIR}/insert.sql" \
        >"${TEST_DIR}/concurrent-${terms}.log" 2>&1
done
[ "$(sql -c 'SELECT count(*) FROM docs')" = 802 ] ||
    fail "concurrent extensions lost writes"
echo "Stale-tail rejection, REINDEX repair, and concurrent extensions passed"
