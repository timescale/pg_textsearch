-- Phase 2 of the memtable v2 redesign (see plan / issue #374):
-- exercise the on-disk write path tp_memtable_append() through
-- the bm25_test_memtable_append() scaffold function, plus the
-- chain-inspection SRF bm25_memtable_chain().
--
-- The scaffold is NOT wired into the index AM yet; these tests
-- use an ordinary pg_textsearch index purely as a host for the
-- memtable pages.  Insert/scan SQL paths still go through the
-- legacy DSA memtable until Phase 4.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE memtable_append_t (id int, body text);

CREATE INDEX memtable_append_idx ON memtable_append_t
    USING bm25 (body) WITH (text_config = 'english');

-- Empty index: head and tail are InvalidBlockNumber, chain is empty.
SELECT bm25_test_memtable_append('memtable_append_idx', 'empty_index');
SELECT count(*) AS chain_pages FROM bm25_memtable_chain('memtable_append_idx');

-- One append: head == tail, chain length 1, page has 1 record.
SELECT bm25_test_memtable_append('memtable_append_idx', 'single_append');
SELECT n_records, next_block IS NULL AS is_tail
    FROM bm25_memtable_chain('memtable_append_idx');

-- Drop and recreate so each scenario starts with an empty chain.
DROP INDEX memtable_append_idx;
CREATE INDEX memtable_append_idx ON memtable_append_t
    USING bm25 (body) WITH (text_config = 'english');

-- Many small appends all fit on the first page.
SELECT bm25_test_memtable_append('memtable_append_idx', 'fits_on_one_page');
SELECT count(*) AS chain_pages, sum(n_records)::int AS total_records
    FROM bm25_memtable_chain('memtable_append_idx');

DROP INDEX memtable_append_idx;
CREATE INDEX memtable_append_idx ON memtable_append_t
    USING bm25 (body) WITH (text_config = 'english');

-- Larger records force the chain to extend past one page.
SELECT bm25_test_memtable_append('memtable_append_idx', 'extends_chain');
SELECT (count(*) >= 2) AS chain_extended,
       sum(n_records) > 0 AS has_records
    FROM bm25_memtable_chain('memtable_append_idx');

DROP INDEX memtable_append_idx;
CREATE INDEX memtable_append_idx ON memtable_append_t
    USING bm25 (body) WITH (text_config = 'english');

-- ctid, doc_length, and vector_bytes all round-trip exactly.
SELECT bm25_test_memtable_append('memtable_append_idx', 'roundtrip');

DROP INDEX memtable_append_idx;
CREATE INDEX memtable_append_idx ON memtable_append_t
    USING bm25 (body) WITH (text_config = 'english');

-- Oversized record is rejected; chain is untouched.
SELECT bm25_test_memtable_append('memtable_append_idx', 'oversized_rejected');
SELECT count(*) AS chain_pages FROM bm25_memtable_chain('memtable_append_idx');

-- Clean up
DROP EXTENSION pg_textsearch CASCADE;
