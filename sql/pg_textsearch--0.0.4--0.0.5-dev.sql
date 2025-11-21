-- Upgrade from 0.0.4 to 0.0.5-dev

-- Function to force segment write (spill memtable to disk)
CREATE OR REPLACE FUNCTION bm25_spill_index(index_name text)
RETURNS int4
AS 'MODULE_PATHNAME', 'tp_spill_memtable'
LANGUAGE C VOLATILE STRICT;

-- Debug function to dump segment contents
CREATE OR REPLACE FUNCTION bm25_debug_dump_segment(index_name text, segment_block int4)
RETURNS text
AS 'MODULE_PATHNAME', 'tp_debug_dump_segment'
LANGUAGE C VOLATILE STRICT;
