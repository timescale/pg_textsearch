-- Unit-level coverage for the on-disk write path
-- tp_memtable_append() (memtable v2, issue #374), driven through
-- the bm25_test_memtable_append() scaffold function, plus the
-- chain-inspection SRF bm25_memtable_chain().
--
-- These tests use an ordinary pg_textsearch index purely as a
-- host for the memtable pages; the assertions are about the
-- chain layout, not end-to-end query semantics (which the rest
-- of the regression suite covers).

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

-- Oversized record goes through the multi-page (fragment) path:
-- a fragment head page followed by N continuation pages and a
-- fresh tail page, with a small bookend record on either side
-- to verify the head record stays on the original page and the
-- new tail can still accept subsequent appends.
SELECT bm25_test_memtable_append('memtable_append_idx', 'multi_page_fragment');
-- Page shape:
--   small_record_page (n_records=1, flags=0)
--   fragment_head     (n_records=1, FRAGMENT flag set on the record;
--                                   page flags=0)
--   continuation*N    (n_records=0, CONTINUATION flag set on the page,
--                                   page flags=0x02)
--   new_tail          (n_records=1, flags=0)
SELECT n_records FROM bm25_memtable_chain('memtable_append_idx');
-- Assert page-level flag bits.  The continuation pages must carry
-- TP_MEMTABLE_PAGE_FLAG_CONTINUATION (0x02); every other page must
-- have flags = 0.  Iterated record_count of fragment continuation
-- pages is 0 because the fragment head holds the record's metadata
-- and continuations carry only opaque tail bytes.
SELECT
    sum(((flags & 2) <> 0)::int)::int AS continuation_pages,
    sum(CASE WHEN (flags & 2) <> 0 THEN 0 ELSE 1 END)::int AS regular_pages,
    bool_and((flags & 2) <> 0 OR flags = 0) AS only_known_flags
FROM bm25_memtable_chain('memtable_append_idx');
SELECT
    bool_and(
        CASE WHEN (flags & 2) <> 0 THEN n_records = 0
                                   ELSE n_records >= 0 END
    ) AS continuation_pages_have_zero_records
FROM bm25_memtable_chain('memtable_append_idx');

-- Clean up
DROP EXTENSION pg_textsearch CASCADE;
