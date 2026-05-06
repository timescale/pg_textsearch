#!/bin/bash
#
# replication_correctness.sh — physical replication correctness
# tests focusing on long-lived standby backends across schema
# variations and primary write events (insert/update/delete/spill/
# merge).
#
# Failure mode tracked here: long-lived standby backend staleness
# (#345). A standby backend that stays alive across primary inserts
# rebuilds its in-memory view once on first scan and is never
# invalidated by WAL replay; subsequent primary inserts and segment
# merges are invisible to it. Surfaces in this file as "0 results"
# (or undercount) for documents that ARE visible on disk to the
# standby.
#
# Before PR #343 there was a second surface here — long-lived
# standby backends erroring on segment reads when the segments
# arrived via WAL replay. That was #342 (page-index pages weren't
# WAL-logged at all), fixed by PR #343. The remaining failures in
# this file all map to #345.
#
# A1 PASSES today — it reads segments that existed in the initial
# basebackup, so the long-lived backend's stale view is still
# correct. All other tests FAIL today against #345.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55440
STANDBY_PORT=55441
TEST_DB=replication_correctness
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_corr_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_corr_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# -------------------------------------------------------------------
# Test A1: Long-lived read after primary spill.
# After the primary spills its memtable, the long-lived standby
# backend re-queries and sees the spilled (now-segment) data.
# Expected: PASS today (segments replicate via GenericXLog).
# -------------------------------------------------------------------
test_long_lived_read_after_primary_spill() {
    log "=== A1: read after primary spill ==="
    primary_sql "
        DROP TABLE IF EXISTS a1_docs CASCADE;
        CREATE TABLE a1_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO a1_docs (content)
            SELECT 'doc ' || i || ' alpha bravo charlie'
            FROM generate_series(1,200) i;
        CREATE INDEX a1_idx ON a1_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    setup_standby
    wait_for_standby_catchup

    long_lived_open "${STANDBY_PORT}"
    local before
    before=$(long_lived_query "
        SELECT count(*) FROM (
            SELECT id FROM a1_docs
            ORDER BY content <@> to_bm25query('alpha', 'a1_idx')
            LIMIT 1000
        ) t;")
    primary_sql "SELECT bm25_spill_index('a1_idx');" >/dev/null
    wait_for_standby_catchup
    local after
    after=$(long_lived_query "
        SELECT count(*) FROM (
            SELECT id FROM a1_docs
            ORDER BY content <@> to_bm25query('alpha', 'a1_idx')
            LIMIT 1000
        ) t;")
    long_lived_close

    log "before=${before} after=${after} (both should be 200)"
    if [ "${before}" != "200" ] || [ "${after}" != "200" ]; then
        error "A1: counts wrong (before=${before}, after=${after})"
    fi
    log "A1 PASSED"
}

# -------------------------------------------------------------------
# Test A3: Memtable+segment mix.
# Primary has data both in a segment (post-spill) AND in the
# memtable (post-spill inserts). Long-lived standby backend should
# see ALL of it.
# Expected: FAIL today — the segment portion replicates fine, but
# the memtable portion is invisible to the long-lived backend.
# -------------------------------------------------------------------
test_long_lived_memtable_segment_mix() {
    log "=== A3: memtable + segment mix (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS a3_docs CASCADE;
        CREATE TABLE a3_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO a3_docs (content)
            SELECT 'spilled doc ' || i || ' alpha'
            FROM generate_series(1,100) i;
        CREATE INDEX a3_idx ON a3_docs USING bm25(content)
            WITH (text_config='simple');
        SELECT bm25_spill_index('a3_idx');
    " >/dev/null
    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (
            SELECT id FROM a3_docs
            ORDER BY content <@> to_bm25query('alpha', 'a3_idx')
            LIMIT 1000
        ) t;
    "
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO a3_docs (content)
                SELECT 'memtable doc ' || i || ' alpha'
                FROM generate_series(1,50) i;
        \" >/dev/null
        wait_for_standby_catchup"

    log "before=${LL_BEFORE} (expect 100), after=${LL_AFTER} (expect 150)"
    if [ "${LL_BEFORE}" != "100" ]; then
        error "A3: before count wrong (${LL_BEFORE})"
    fi
    if [ "${LL_AFTER}" != "150" ]; then
        error "A3 BUG (expected): standby long-lived backend missed \
memtable inserts (before=${LL_BEFORE}, after=${LL_AFTER}, expected=150)"
    fi
    log "A3 PASSED"
}

# -------------------------------------------------------------------
# Test A4: UPDATE replication (HOT and non-HOT).
# UPDATE on primary changes the indexed text. Long-lived standby
# backend should see the new content searchable.
# Expected: FAIL today.
# -------------------------------------------------------------------
test_long_lived_update_replication() {
    log "=== A4: UPDATE replication (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS a4_docs CASCADE;
        CREATE TABLE a4_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO a4_docs (content)
            SELECT 'original doc ' || i || ' xenon'
            FROM generate_series(1,20) i;
        CREATE INDEX a4_idx ON a4_docs USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    long_lived_open "${STANDBY_PORT}"
    local xenon_before zircon_before
    xenon_before=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM a4_docs
            ORDER BY content <@> to_bm25query('xenon', 'a4_idx')
            LIMIT 100) t;")
    zircon_before=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM a4_docs
            ORDER BY content <@> to_bm25query('zircon', 'a4_idx')
            LIMIT 100) t;")

    primary_sql "
        UPDATE a4_docs SET content = 'replaced doc ' || id || ' zircon'
        WHERE id <= 10;
    " >/dev/null
    wait_for_standby_catchup

    local xenon_after zircon_after
    xenon_after=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM a4_docs
            ORDER BY content <@> to_bm25query('xenon', 'a4_idx')
            LIMIT 100) t;")
    zircon_after=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM a4_docs
            ORDER BY content <@> to_bm25query('zircon', 'a4_idx')
            LIMIT 100) t;")
    long_lived_close

    log "xenon: before=${xenon_before} (expect 20), after=${xenon_after} (expect 10)"
    log "zircon: before=${zircon_before} (expect 0), after=${zircon_after} (expect 10)"

    if [ "${xenon_after}" != "10" ] || [ "${zircon_after}" != "10" ]; then
        error "A4 BUG (expected): standby long-lived backend missed \
UPDATEs (xenon=${xenon_after}/expected 10, zircon=${zircon_after}/expected 10)"
    fi
    log "A4 PASSED"
}

# -------------------------------------------------------------------
# Test A5: DELETE + VACUUM replication.
# Deleted+vacuumed rows should not appear in standby search results.
# Expected: FAIL today — same WAL-replayed-segment issue as A2.
# Long-lived standby backend cannot read segments created by primary
# spill+merge that arrived via WAL replay. Will pass once the rmgr-
# based replication implementation lands.
# -------------------------------------------------------------------
test_long_lived_delete_vacuum() {
    log "=== A5: DELETE + VACUUM ==="
    primary_sql "
        DROP TABLE IF EXISTS a5_docs CASCADE;
        CREATE TABLE a5_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO a5_docs (content)
            SELECT 'doc ' || i || ' alpha'
            FROM generate_series(1,100) i;
        CREATE INDEX a5_idx ON a5_docs USING bm25(content)
            WITH (text_config='simple');
        SELECT bm25_spill_index('a5_idx');
    " >/dev/null
    wait_for_standby_catchup

    long_lived_open "${STANDBY_PORT}"
    local before
    before=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM a5_docs
            ORDER BY content <@> to_bm25query('alpha', 'a5_idx')
            LIMIT 1000) t;")

    primary_sql "DELETE FROM a5_docs WHERE id <= 30;" >/dev/null
    primary_sql "VACUUM a5_docs;" >/dev/null
    wait_for_standby_catchup

    local after
    after=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM a5_docs
            ORDER BY content <@> to_bm25query('alpha', 'a5_idx')
            LIMIT 1000) t;")
    long_lived_close

    log "before=${before} (expect 100), after=${after} (expect 70)"
    if [[ "${before}" == *ERROR* ]] || [[ "${after}" == *ERROR* ]] || \
       [ "${before}" != "100" ] || [ "${after}" != "70" ]; then
        error "A5 BUG (expected): standby long-lived backend cannot read \
WAL-replayed segments (before=${before}, after=${after}, \
expected before=100 after=70)"
    fi
    log "A5 PASSED"
}

# -------------------------------------------------------------------
# Test E1: Partitioned table replication.
# Long-lived standby backend should see new inserts to a
# partitioned table.
# Expected: FAIL today.
# -------------------------------------------------------------------
test_long_lived_partitioned_table() {
    log "=== E1: partitioned table (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS e1_docs CASCADE;
        CREATE TABLE e1_docs (
            id INT,
            tenant INT,
            content TEXT
        ) PARTITION BY RANGE (tenant);
        CREATE TABLE e1_docs_t1 PARTITION OF e1_docs
            FOR VALUES FROM (0) TO (10);
        CREATE TABLE e1_docs_t2 PARTITION OF e1_docs
            FOR VALUES FROM (10) TO (20);
        INSERT INTO e1_docs (id, tenant, content)
            SELECT i, i % 20, 'doc ' || i || ' alpha'
            FROM generate_series(1,40) i;
        CREATE INDEX e1_idx_t1 ON e1_docs_t1 USING bm25(content)
            WITH (text_config='simple');
        CREATE INDEX e1_idx_t2 ON e1_docs_t2 USING bm25(content)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    # Query against one partition's index directly.
    local query="
        SELECT count(*) FROM (SELECT id FROM e1_docs_t1
            ORDER BY content <@> to_bm25query('alpha', 'e1_idx_t1')
            LIMIT 100) t;
    "
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO e1_docs (id, tenant, content) VALUES
                (101, 5, 'newdoc 101 alpha'),
                (102, 7, 'newdoc 102 alpha');
        \" >/dev/null
        wait_for_standby_catchup"

    log "before=${LL_BEFORE}, after=${LL_AFTER}"
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       ! [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] || \
       [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; then
        error "E1 BUG (expected): standby long-lived backend missed \
partition insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi
    log "E1 PASSED"
}

# -------------------------------------------------------------------
# Test E2: Expression index.
# Index on lower(content). Long-lived standby backend should see
# new rows via the expression index.
# Expected: FAIL today.
# -------------------------------------------------------------------
test_long_lived_expression_index() {
    log "=== E2: expression index (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS e2_docs CASCADE;
        CREATE TABLE e2_docs (id SERIAL PRIMARY KEY, content TEXT);
        INSERT INTO e2_docs (content) VALUES
            ('Initial Document ALPHA'),
            ('Another Initial Doc BRAVO');
        CREATE INDEX e2_idx ON e2_docs USING bm25(lower(content))
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (SELECT id FROM e2_docs
            ORDER BY lower(content) <@> to_bm25query('alpha', 'e2_idx')
            LIMIT 100) t;
    "
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO e2_docs (content) VALUES
                ('Brand New Document ALPHA');
        \" >/dev/null
        wait_for_standby_catchup"

    log "before=${LL_BEFORE} (expect 1), after=${LL_AFTER} (expect 2)"
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       ! [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] || \
       [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; then
        error "E2 BUG (expected): expression-index standby long-lived \
backend missed insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi
    log "E2 PASSED"
}

# -------------------------------------------------------------------
# Test E3: Partial index.
# Long-lived standby backend should see new rows that match the
# WHERE predicate.
# Expected: FAIL today.
# -------------------------------------------------------------------
test_long_lived_partial_index() {
    log "=== E3: partial index (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS e3_docs CASCADE;
        CREATE TABLE e3_docs (id SERIAL PRIMARY KEY,
                              content TEXT,
                              published BOOL);
        INSERT INTO e3_docs (content, published) VALUES
            ('published initial alpha', true),
            ('draft initial alpha', false);
        CREATE INDEX e3_idx ON e3_docs USING bm25(content)
            WITH (text_config='simple') WHERE published;
    " >/dev/null
    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (SELECT id FROM e3_docs
            WHERE published
            ORDER BY content <@> to_bm25query('alpha', 'e3_idx')
            LIMIT 100) t;
    "
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO e3_docs (content, published) VALUES
                ('newly published alpha', true);
        \" >/dev/null
        wait_for_standby_catchup"

    log "before=${LL_BEFORE} (expect 1), after=${LL_AFTER} (expect 2)"
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       ! [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] || \
       [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; then
        error "E3 BUG (expected): partial-index standby long-lived \
backend missed insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi
    log "E3 PASSED"
}

# -------------------------------------------------------------------
# Test E4: text[] (array) index.
# Long-lived standby backend should see new array-column inserts.
# Expected: FAIL today.
# -------------------------------------------------------------------
test_long_lived_text_array() {
    log "=== E4: text[] index (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS e4_docs CASCADE;
        CREATE TABLE e4_docs (id SERIAL PRIMARY KEY,
                              tags TEXT[]);
        INSERT INTO e4_docs (tags) VALUES
            (ARRAY['initial','alpha','bravo']);
        CREATE INDEX e4_idx ON e4_docs USING bm25(tags)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    local query="
        SELECT count(*) FROM (SELECT id FROM e4_docs
            ORDER BY tags <@> to_bm25query('alpha', 'e4_idx')
            LIMIT 100) t;
    "
    long_lived_before_after "${STANDBY_PORT}" "${query}" \
        "primary_sql \"
            INSERT INTO e4_docs (tags) VALUES
                (ARRAY['new','alpha','charlie']);
        \" >/dev/null
        wait_for_standby_catchup"

    log "before=${LL_BEFORE} (expect 1), after=${LL_AFTER} (expect 2)"
    if [[ "${LL_AFTER}" == *ERROR* ]] || \
       ! [[ "${LL_AFTER}" =~ ^[0-9]+$ ]] || \
       [ "${LL_AFTER}" -le "${LL_BEFORE}" ]; then
        error "E4 BUG (expected): text[]-index standby long-lived \
backend missed insert (before=${LL_BEFORE}, after=${LL_AFTER})"
    fi
    log "E4 PASSED"
}

# -------------------------------------------------------------------
# Test E5: Multiple indexes on same table.
# Two indexes on the same table (different columns or different
# text_configs). Both should reflect new inserts to a long-lived
# backend.
# Expected: FAIL today (both indexes have stale memtables).
# -------------------------------------------------------------------
test_long_lived_multiple_indexes_same_table() {
    log "=== E5: multiple indexes on same table (should FAIL today) ==="
    primary_sql "
        DROP TABLE IF EXISTS e5_docs CASCADE;
        CREATE TABLE e5_docs (id SERIAL PRIMARY KEY,
                              title TEXT,
                              body  TEXT);
        INSERT INTO e5_docs (title, body) VALUES
            ('initial title alpha', 'initial body bravo');
        CREATE INDEX e5_title_idx ON e5_docs USING bm25(title)
            WITH (text_config='simple');
        CREATE INDEX e5_body_idx  ON e5_docs USING bm25(body)
            WITH (text_config='simple');
    " >/dev/null
    wait_for_standby_catchup

    long_lived_open "${STANDBY_PORT}"
    local title_before body_before
    title_before=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM e5_docs
            ORDER BY title <@> to_bm25query('alpha','e5_title_idx')
            LIMIT 100) t;")
    body_before=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM e5_docs
            ORDER BY body <@> to_bm25query('bravo','e5_body_idx')
            LIMIT 100) t;")

    primary_sql "
        INSERT INTO e5_docs (title, body) VALUES
            ('new title alpha', 'new body bravo');
    " >/dev/null
    wait_for_standby_catchup

    local title_after body_after
    title_after=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM e5_docs
            ORDER BY title <@> to_bm25query('alpha','e5_title_idx')
            LIMIT 100) t;")
    body_after=$(long_lived_query "
        SELECT count(*) FROM (SELECT id FROM e5_docs
            ORDER BY body <@> to_bm25query('bravo','e5_body_idx')
            LIMIT 100) t;")
    long_lived_close

    log "title: before=${title_before}, after=${title_after}"
    log "body:  before=${body_before}, after=${body_after}"
    if [[ "${title_after}" == *ERROR* ]] || \
       [[ "${body_after}" == *ERROR* ]] || \
       ! [[ "${title_after}" =~ ^[0-9]+$ ]] || \
       ! [[ "${body_after}" =~ ^[0-9]+$ ]] || \
       [ "${title_after}" -le "${title_before}" ] || \
       [ "${body_after}" -le "${body_before}" ]; then
        error "E5 BUG (expected): standby long-lived backend missed \
inserts on one or both indexes (title=${title_after}, body=${body_after})"
    fi
    log "E5 PASSED"
}

main() {
    log "Starting pg_textsearch replication correctness tests..."
    check_required_tools
    setup_primary

    test_long_lived_read_after_primary_spill
    test_long_lived_memtable_segment_mix
    test_long_lived_update_replication
    test_long_lived_delete_vacuum
    test_long_lived_partitioned_table
    test_long_lived_expression_index
    test_long_lived_partial_index
    test_long_lived_text_array
    test_long_lived_multiple_indexes_same_table

    log "All replication correctness tests completed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
