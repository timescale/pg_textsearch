-- Unit-level coverage for the new on-disk memtable page format.
-- Exercises tp_memtable_page_{init,append,iterate,can_fit,is_valid}
-- via the bm25_test_memtable_page() scaffold function. Each case
-- returns 'OK' on success or 'FAIL: <reason>' otherwise.
--
-- Memtable v2 page format (see plan / issue #374).

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- An uninitialized (zero-filled) page must be rejected.
SELECT bm25_test_memtable_page('reject_uninitialized');

-- After init, the page is valid, has 0 records, an invalid next_block,
-- and free_offset pointing past the header.
SELECT bm25_test_memtable_page('init_empty');

-- Iteration over an empty page yields no records.
SELECT bm25_test_memtable_page('iterate_empty');

-- A single appended record round-trips its ctid, doc_length, and
-- vector bytes verbatim.
SELECT bm25_test_memtable_page('one_record_roundtrip');

-- Many records of varying sizes appended sequentially are all
-- recoverable in insertion order.
SELECT bm25_test_memtable_page('many_records_roundtrip');

-- can_fit() returns false when the requested record would not fit.
SELECT bm25_test_memtable_page('can_fit_boundary');

-- Appending records until the page is full leaves the page in a
-- consistent state (n_records matches what was actually appended,
-- and iteration walks them all).
SELECT bm25_test_memtable_page('fill_until_full');

-- next_block setter / getter round-trip.
SELECT bm25_test_memtable_page('next_block_roundtrip');

-- Clean up
DROP EXTENSION pg_textsearch CASCADE;
