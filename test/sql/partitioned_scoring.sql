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
RESET enable_seqscan;
RESET enable_sort;

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
