-- Test case: parallel_build_hot_root
-- A parallel index build must record the TID of the HOT-chain ROOT, not the
-- TID of the live heap tuple.  For a HOT-updated row the live version is a
-- HEAP_ONLY_TUPLE; an index entry pointing at it cannot be followed, because
-- heap_hot_search_buffer() expects to start at a chain root.  The row then
-- goes silently missing from index scans while a sequential scan still sees
-- it.
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Build notices name the number of workers actually launched, which is
-- machine dependent; keep them out of the expected output.
SET client_min_messages = warning;

-- Lower threshold so the test table qualifies for a parallel build
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '256MB';

-- fillfactor leaves room on each page, so the UPDATE below can stay HOT
CREATE TABLE hot_root (
    id      INT,
    content TEXT,
    payload INT
) WITH (fillfactor = 50);

INSERT INTO hot_root
SELECT i, 'needle' || i || ' alpha beta gamma', 0
FROM generate_series(1, 300000) AS i;

-- HOT update: touches only a column that is NOT indexed, so every live row
-- version becomes a HEAP_ONLY_TUPLE and its line pointer is not a chain root.
UPDATE hot_root SET payload = 1;

ALTER TABLE hot_root SET (parallel_workers = 2);
SET max_parallel_maintenance_workers = 2;

CREATE INDEX hot_root_idx ON hot_root USING bm25(content)
  WITH (text_config='simple');

-- Each needle occurs in exactly one row, so each ordered index scan must
-- return exactly one row.  Before the fix every one of them returned zero.
SELECT 'needle1'      AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle1','hot_root_idx'))      < 0 ORDER BY content <@> to_bm25query('needle1','hot_root_idx')      LIMIT 5) t
UNION ALL
SELECT 'needle4711'   AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle4711','hot_root_idx'))   < 0 ORDER BY content <@> to_bm25query('needle4711','hot_root_idx')   LIMIT 5) t
UNION ALL
SELECT 'needle100000' AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle100000','hot_root_idx')) < 0 ORDER BY content <@> to_bm25query('needle100000','hot_root_idx') LIMIT 5) t
UNION ALL
SELECT 'needle222222' AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle222222','hot_root_idx')) < 0 ORDER BY content <@> to_bm25query('needle222222','hot_root_idx') LIMIT 5) t
UNION ALL
SELECT 'needle299999' AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle299999','hot_root_idx')) < 0 ORDER BY content <@> to_bm25query('needle299999','hot_root_idx') LIMIT 5) t;

-- A serial rebuild of the same index must give the same answer.
DROP INDEX hot_root_idx;
SET max_parallel_maintenance_workers = 0;
CREATE INDEX hot_root_idx ON hot_root USING bm25(content)
  WITH (text_config='simple');

SELECT 'needle1'      AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle1','hot_root_idx'))      < 0 ORDER BY content <@> to_bm25query('needle1','hot_root_idx')      LIMIT 5) t
UNION ALL
SELECT 'needle100000' AS term, count(*) AS rows FROM (SELECT id FROM hot_root WHERE (content <@> to_bm25query('needle100000','hot_root_idx')) < 0 ORDER BY content <@> to_bm25query('needle100000','hot_root_idx') LIMIT 5) t;

DROP TABLE hot_root;
