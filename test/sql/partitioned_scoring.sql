-- Parent index names must use the scored row's leaf-local statistics.
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE tenant_docs (tenant int, id int, body text)
    PARTITION BY LIST (tenant);
-- The first child has no physical index, even several levels down.
CREATE TABLE tenant_empty PARTITION OF tenant_docs FOR VALUES IN (0)
    PARTITION BY LIST (id);
CREATE TABLE tenant_one PARTITION OF tenant_docs FOR VALUES IN (1);
CREATE TABLE tenant_two PARTITION OF tenant_docs FOR VALUES IN (2)
    PARTITION BY RANGE (id);
CREATE TABLE tenant_two_leaf PARTITION OF tenant_two
    FOR VALUES FROM (1) TO (100);
CREATE INDEX tenant_bm25 ON tenant_docs USING bm25(body)
    WITH (text_config = 'english');
INSERT INTO tenant_docs
SELECT 1, g, CASE WHEN g = 1 THEN 'apple orchard' ELSE 'banana orchard' END
FROM generate_series(1, 10) g;
INSERT INTO tenant_docs
SELECT 2, g, 'apple orchard' FROM generate_series(1, 10) g;

-- Deliberately exercise standalone scoring, not just cached index scores.
SELECT tenant, bool_and(abs(
    (body <@> to_bm25query('apple', 'tenant_bm25')) +
    CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
         ELSE ln(1 + 0.5 / 10.5) END) < 0.000001) AS local_stats
FROM tenant_docs WHERE id = 1 GROUP BY tenant ORDER BY tenant;

SELECT abs((body <@> 'apple') + ln(1 + 0.5 / 10.5)) < 0.000001
    AS implicit_local_stats
FROM tenant_docs WHERE tenant = 2 AND id = 1;

-- A generic plan cannot resolve the query's parent index at planning time.
SET plan_cache_mode = force_generic_plan;
PREPARE tenant_score(text, text, int) AS
SELECT abs((body <@> to_bm25query($1, $2)) + ln(1 + 0.5 / 10.5))
    < 0.000001 AS prepared_local_stats
FROM tenant_docs WHERE tenant = $3 AND id = 1;
EXECUTE tenant_score('apple', 'tenant_bm25', 2);
DEALLOCATE tenant_score;
RESET plan_cache_mode;

SELECT (SELECT abs((d.body <@> to_bm25query('apple', 'tenant_bm25')) +
                   ln(1 + 0.5 / 10.5)) < 0.000001)
    AS correlated_local_stats
FROM tenant_docs d WHERE tenant = 2 AND id = 1;

-- Ordered scans must continue to use leaf-local scores for projection too.
SET enable_seqscan = off;
SET enable_sort = off;
DO $$
DECLARE
    plan jsonb;
BEGIN
    EXECUTE $query$
        EXPLAIN (FORMAT JSON, COSTS OFF)
        SELECT bool_and(abs(score + ln(1 + 0.5 / 10.5)) < 0.000001)
        FROM (
            SELECT body <@> to_bm25query('apple', 'tenant_bm25') AS score
            FROM tenant_docs WHERE tenant = 2 ORDER BY score LIMIT 3
        ) s
    $query$ INTO plan;
    IF NOT jsonb_path_exists(plan,
        '$.** ? (@."Node Type" == "Index Scan" &&
                 @."Index Name" == "tenant_two_leaf_body_idx" &&
                 exists (@."Order By"))') OR
       jsonb_path_exists(plan, '$.** ? (@."Node Type" like_regex "Sort")')
    THEN
        RAISE EXCEPTION 'expected an ordered leaf index scan: %', plan;
    END IF;
END
$$;
SELECT bool_and(abs(score + ln(1 + 0.5 / 10.5)) < 0.000001)
    AS ordered_local_stats
FROM (
    SELECT body <@> to_bm25query('apple', 'tenant_bm25') AS score
    FROM tenant_docs WHERE tenant = 2 ORDER BY score LIMIT 3
) s;

-- Sorting outside a subquery or CTE keeps the ordered scan of an ordinary
-- index and of a parent index alike.
CREATE TABLE plain_docs (id int, body text);
INSERT INTO plain_docs
SELECT g, CASE WHEN g % 10 = 0 THEN 'apple orchard' ELSE 'banana grove' END
FROM generate_series(1, 200) g;
CREATE INDEX plain_bm25 ON plain_docs USING bm25(body)
    WITH (text_config = 'english');
DO $$
DECLARE
    item record;
    shape text;
    plan jsonb;
BEGIN
    FOR item IN SELECT * FROM (VALUES
            ('plain_docs', 'plain_bm25', 'plain_bm25'),
            ('tenant_docs', 'tenant_bm25', 'tenant_two_leaf_body_idx'))
            v(tbl, idx, scan_idx)
    LOOP
        FOREACH shape IN ARRAY ARRAY[
            'SELECT * FROM (%s) t ORDER BY s LIMIT 5',
            'WITH t AS (%s) SELECT * FROM t ORDER BY s LIMIT 5']
        LOOP
            EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || format(shape,
                format('SELECT body <@> to_bm25query(%L, %L) AS s FROM %I',
                       'apple', item.idx, item.tbl)) INTO plan;
            IF NOT jsonb_path_exists(plan,
                   '$.** ? (@."Node Type" == "Index Scan" &&
                            @."Index Name" == $idx && exists (@."Order By"))',
                   jsonb_build_object('idx', item.scan_idx)) OR
               jsonb_path_exists(plan,
                   '$.** ? (@."Node Type" like_regex "Sort")')
            THEN
                RAISE EXCEPTION 'expected an ordered % scan: %',
                    item.scan_idx, plan;
            END IF;
        END LOOP;
    END LOOP;
END
$$;
SELECT tenant, abs(s + CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
                            ELSE ln(1 + 0.5 / 10.5) END) < 0.000001
    AS subquery_ordered_local_stats
FROM (
    SELECT tenant, body <@> to_bm25query('apple', 'tenant_bm25') AS s
    FROM tenant_docs
) t ORDER BY s LIMIT 3;
DROP TABLE plain_docs;
RESET enable_seqscan;
RESET enable_sort;

-- Without an index scan, a join inside the subquery still carries each
-- row's partition to the sorted projection.
SET enable_indexscan = off;
SELECT tenant, abs(s + CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
                            ELSE ln(1 + 0.5 / 10.5) END) < 0.000001
    AS subquery_sorted_local_stats
FROM (
    SELECT d.tenant, d.body <@> to_bm25query('apple', 'tenant_bm25') AS s
    FROM tenant_docs d JOIN (VALUES (1), (2)) v(tenant) USING (tenant)
    WHERE d.id = 1
) t ORDER BY s;
-- A scalar subquery earlier in the target list leaves the score's table
-- unchanged.
SELECT d.tenant, (SELECT 1 FROM (VALUES (1)) s(x)) AS one,
    abs((d.body <@> to_bm25query('apple', 'tenant_bm25')) +
        CASE WHEN d.tenant = 1 THEN ln(1 + 9.5 / 1.5)
             ELSE ln(1 + 0.5 / 10.5) END) < 0.000001 AS sublink_local_stats
FROM tenant_docs d JOIN (VALUES (1), (2)) v(tenant) USING (tenant)
WHERE d.id = 1 ORDER BY d.tenant;
RESET enable_indexscan;

-- A forced sort must agree with index ordering, across all partitions.
SET enable_indexscan = off;
SELECT tenant, abs(score + CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
                               ELSE ln(1 + 0.5 / 10.5) END) < 0.000001
    AS sorted_local_stats
FROM (
    SELECT tenant, body <@> 'apple' AS score
    FROM tenant_docs WHERE id = 1 ORDER BY score
) s;
RESET enable_indexscan;

-- A standalone score can be a hash key, not just a projection or filter.
SET enable_nestloop = off;
SET enable_mergejoin = off;
SELECT count(*) AS partition_score_hash_matches
FROM tenant_docs a JOIN tenant_docs b
ON (a.body <@> to_bm25query('apple', 'tenant_bm25')) =
   (b.body <@> to_bm25query('apple', 'tenant_bm25'))
WHERE a.id = 1 AND b.id = 1;
RESET enable_nestloop;
RESET enable_mergejoin;

-- Partition identity must survive a join and null extension.
SELECT d.tenant, bool_and(abs(
    (d.body <@> to_bm25query('apple', 'tenant_bm25')) +
    CASE WHEN d.tenant = 1 THEN ln(1 + 9.5 / 1.5)
         ELSE ln(1 + 0.5 / 10.5) END) < 0.000001) AS joined_local_stats
FROM tenant_docs d JOIN (VALUES (1), (2)) t(tenant) USING (tenant)
WHERE d.id = 1 GROUP BY d.tenant ORDER BY d.tenant;

SELECT (d.body <@> to_bm25query('apple', 'tenant_bm25')) IS NULL
    AS null_extended_score
FROM (VALUES (3)) t(tenant)
LEFT JOIN tenant_docs d ON d.tenant = t.tenant AND d.id = 1;

-- An upsert scores EXCLUDED with the conflicting row's leaf partition.
CREATE TABLE upsert_docs (tenant int, id int, body text, score float8,
    PRIMARY KEY (tenant, id)) PARTITION BY LIST (tenant);
CREATE TABLE upsert_one PARTITION OF upsert_docs FOR VALUES IN (1);
CREATE TABLE upsert_two PARTITION OF upsert_docs FOR VALUES IN (2);
CREATE INDEX upsert_bm25 ON upsert_docs USING bm25(body)
    WITH (text_config = 'english');
INSERT INTO upsert_docs (tenant, id, body)
SELECT tenant, id, body FROM tenant_docs;
INSERT INTO upsert_docs (tenant, id, body)
VALUES (1, 1, 'apple orchard'), (2, 1, 'apple orchard')
ON CONFLICT (tenant, id) DO UPDATE
SET score = EXCLUDED.body <@> to_bm25query('apple', 'upsert_bm25')
RETURNING tenant, abs(score + CASE WHEN tenant = 1 THEN ln(1 + 9.5 / 1.5)
                                   ELSE ln(1 + 0.5 / 10.5) END) < 0.000001
    AS upsert_local_stats;
DROP TABLE upsert_docs;
-- So does an ordinary index, even when a generic plan hides its name.
CREATE TABLE plain_upsert (id int PRIMARY KEY, body text, score float8);
CREATE INDEX plain_upsert_bm25 ON plain_upsert USING bm25(body)
    WITH (text_config = 'english');
INSERT INTO plain_upsert VALUES (1, 'apple orchard'), (2, 'banana grove');
PREPARE plain_upsert_score(text) AS
INSERT INTO plain_upsert VALUES (1, 'apple orchard')
ON CONFLICT (id) DO UPDATE
SET score = EXCLUDED.body <@> to_bm25query($1, 'plain_upsert_bm25')
RETURNING score IS NOT NULL AS plain_upsert_scored;
EXECUTE plain_upsert_score('apple');
SET plan_cache_mode = force_generic_plan;
EXECUTE plain_upsert_score('apple');
RESET plan_cache_mode;
DEALLOCATE plain_upsert_score;
DROP TABLE plain_upsert;

-- RETURNING old scores a moved row with the partition it left (PG18+).
CREATE TABLE move_docs (tenant int, id int, body text)
    PARTITION BY LIST (tenant);
CREATE TABLE move_one PARTITION OF move_docs FOR VALUES IN (1);
CREATE TABLE move_two PARTITION OF move_docs FOR VALUES IN (2);
CREATE INDEX move_bm25 ON move_docs USING bm25(body)
    WITH (text_config = 'english');
INSERT INTO move_docs SELECT tenant, id, body FROM tenant_docs;
DO $$
DECLARE
    old_local_stats boolean;
BEGIN
    IF current_setting('server_version_num')::int < 180000 THEN
        RETURN;
    END IF;
    EXECUTE $query$
        UPDATE move_docs SET tenant = 2 WHERE tenant = 1 AND id = 1
        RETURNING abs((old.body <@> to_bm25query('apple', 'move_bm25')) +
                      ln(1 + 9.5 / 1.5)) < 0.000001
    $query$ INTO old_local_stats;
    IF NOT old_local_stats THEN
        RAISE EXCEPTION 'RETURNING old used the new row''s partition';
    END IF;
END $$;
DROP TABLE move_docs;
-- A row returned by an INSTEAD OF trigger has no tableoid to carry.
CREATE VIEW tenant_view AS SELECT tenant, id, body FROM tenant_docs;
CREATE FUNCTION tenant_view_update() RETURNS trigger LANGUAGE plpgsql
AS $$BEGIN RETURN NEW; END$$;
CREATE TRIGGER tenant_view_update INSTEAD OF UPDATE ON tenant_view
    FOR EACH ROW EXECUTE FUNCTION tenant_view_update();
DO $$
BEGIN
    EXECUTE $query$
        EXPLAIN (VERBOSE, COSTS OFF)
        UPDATE tenant_view SET body = body WHERE tenant = 2 AND id = 1
        RETURNING body <@> to_bm25query('apple', 'tenant_bm25')
    $query$;
END $$;
DROP VIEW tenant_view;
DROP FUNCTION tenant_view_update();

-- Rows alternating between leaves reuse each leaf's index and IDF cache.
CREATE TABLE cache_docs (tenant int, id int, body text)
    PARTITION BY LIST (tenant);
CREATE TABLE cache_one PARTITION OF cache_docs FOR VALUES IN (1);
CREATE TABLE cache_two PARTITION OF cache_docs FOR VALUES IN (2);
CREATE INDEX cache_bm25 ON cache_docs USING bm25(body)
    WITH (text_config = 'english');
INSERT INTO cache_docs
SELECT t, g, 'apple orchard'
FROM generate_series(1, 2) t, generate_series(1, 10) g;
-- Segment IDF lookups read index blocks; memtable cache lookups do not.
SELECT bm25_spill_index('cache_two_body_idx');
ANALYZE cache_docs;
SET enable_nestloop = off;
SET enable_mergejoin = off;
-- Probing the hashed table in series order alternates the scored leaves.
EXPLAIN (COSTS OFF)
SELECT count(d.body <@> to_bm25query('apple', 'cache_bm25'))
FROM generate_series(1, 200) g
JOIN cache_docs d ON d.tenant = g % 2 + 1 AND d.id = (g - 1) / 2 % 10 + 1;
BEGIN;
SELECT pg_stat_get_xact_numscans('pg_inherits_relid_seqno_index'::regclass)
           AS inherits_scans,
       pg_stat_get_xact_blocks_fetched('cache_two_body_idx'::regclass)
           AS leaf_blocks \gset
SELECT count(d.body <@> to_bm25query('apple', 'cache_bm25'))
    AS alternating_scores
FROM generate_series(1, 200) g
JOIN cache_docs d ON d.tenant = g % 2 + 1 AND d.id = (g - 1) / 2 % 10 + 1;
SELECT pg_stat_get_xact_numscans('pg_inherits_relid_seqno_index'::regclass)
           - :inherits_scans < 100 AS leaves_resolved_once,
       pg_stat_get_xact_blocks_fetched('cache_two_body_idx'::regclass)
           - :leaf_blocks < 100 AS idf_cached_per_leaf;
COMMIT;
RESET enable_nestloop;
RESET enable_mergejoin;
DROP TABLE cache_docs;

-- Binding a multi-leaf aggregate must not disable an independent BMW scan.
SET enable_seqscan = off;
SET enable_sort = off;
DO $$
DECLARE
    plan jsonb;
BEGIN
    EXECUTE $query$
        EXPLAIN (FORMAT JSON, COSTS OFF)
        SELECT (
            SELECT body FROM tenant_docs WHERE tenant = 2
            ORDER BY body <@> to_bm25query('apple', 'tenant_bm25') LIMIT 1
        ), sum(body <@> to_bm25query('apple', 'tenant_bm25'))
        FROM tenant_docs
    $query$ INTO plan;
    IF NOT jsonb_path_exists(plan,
        '$.** ? (@."Node Type" == "Index Scan" &&
                 @."Index Name" == "tenant_two_leaf_body_idx")') THEN
        RAISE EXCEPTION 'expected the named parent''s leaf index scan: %', plan;
    END IF;
END
$$;
RESET enable_seqscan;
RESET enable_sort;

-- Authorize through the named parent, without granting direct leaf access.
CREATE ROLE tenant_reader;
GRANT SELECT (tenant, id, body) ON tenant_docs TO tenant_reader;
ALTER TABLE tenant_docs ENABLE ROW LEVEL SECURITY;
CREATE POLICY tenant_two_only ON tenant_docs USING (tenant = 2);
SET ROLE tenant_reader;
SELECT abs((body <@> 'apple') + ln(1 + 0.5 / 10.5)) < 0.000001
    AS parent_grant_sufficient
FROM tenant_docs WHERE tenant = 2 AND id = 1;
SELECT count(*) = 10 AND bool_and(abs(
    (body <@> to_bm25query('apple', 'tenant_bm25')) + ln(1 + 0.5 / 10.5))
    < 0.000001) AS rls_local_stats
FROM tenant_docs;
-- Only the planner can supply a partition, so a hidden one cannot be named.
\set VERBOSITY terse
SELECT bm25_text_bm25query_score(
    'apple orchard', to_bm25query('apple', 'tenant_bm25'),
    'tenant_one'::regclass::oid);
SELECT bm25_textarray_bm25query_score(
    ARRAY['apple orchard'], to_bm25query('apple', 'tenant_bm25'),
    'tenant_one'::regclass::oid);
\set VERBOSITY default
RESET ROLE;
ALTER TABLE tenant_docs DISABLE ROW LEVEL SECURITY;
REVOKE SELECT (tenant, id, body) ON tenant_docs FROM tenant_reader;
-- A leaf grant does not authorize scoring with the parent's index.  The
-- subquery defers index resolution from the planner to the scorer.
GRANT SELECT ON tenant_two_leaf TO tenant_reader;
SET ROLE tenant_reader;
SELECT body <@> (SELECT to_bm25query('apple', 'tenant_bm25'))
FROM tenant_two_leaf WHERE id = 1;
RESET ROLE;
REVOKE SELECT ON tenant_two_leaf FROM tenant_reader;
DROP ROLE tenant_reader;

-- Context-free calls cannot silently pick one tenant's index.
SELECT bm25_text_bm25query_score(
    'apple orchard', to_bm25query('apple', 'tenant_bm25'));
-- Nor can a query attach the parent index to an unrelated table.
CREATE TABLE unrelated_docs (body text);
INSERT INTO unrelated_docs VALUES ('apple orchard');
SELECT body <@> (SELECT to_bm25query('apple', 'tenant_bm25'))
FROM unrelated_docs;
DROP TABLE unrelated_docs;

-- A group of identical text values has no single source partition.
SELECT body, body <@> to_bm25query('apple', 'tenant_bm25')
FROM tenant_docs GROUP BY body;
-- Nor does the output of a subquery that is not flattened into its parent.
SELECT s.body <@> to_bm25query('apple', 'tenant_bm25')
FROM (SELECT body FROM tenant_docs ORDER BY id LIMIT 1) s;

-- Extension SQL that predates partition-aware scoring needs an update.
BEGIN;
ALTER EXTENSION pg_textsearch
    DROP FUNCTION bm25_text_bm25query_score(text, bm25query, internal);
DROP FUNCTION bm25_text_bm25query_score(text, bm25query, internal);
SELECT body <@> to_bm25query('apple', 'tenant_bm25')
FROM tenant_docs WHERE tenant = 2 AND id = 1;
ROLLBACK;

SELECT bm25_spill_index('tenant_two_leaf_body_idx');
SELECT abs((body <@> to_bm25query('apple', 'tenant_two_body_idx')) +
           ln(1 + 0.5 / 10.5)) < 0.000001 AS spilled_subparent_stats
FROM tenant_two WHERE id = 1;

-- Array expression indexes use the same partition binding.
CREATE INDEX tenant_array_bm25 ON tenant_docs USING bm25 ((ARRAY[body]))
    WITH (text_config = 'english');
SELECT abs(((ARRAY[body]) <@>
    to_bm25query('apple', 'tenant_array_bm25')) + ln(1 + 0.5 / 10.5))
    < 0.000001 AS array_local_stats
FROM tenant_docs WHERE tenant = 2 AND id = 1;

DROP TABLE tenant_docs;
DROP EXTENSION pg_textsearch CASCADE;
