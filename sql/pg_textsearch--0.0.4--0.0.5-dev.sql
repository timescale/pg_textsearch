-- Upgrade from 0.0.4 to 0.0.5-dev

-- Function to force segment write (spill memtable to disk)
CREATE OR REPLACE FUNCTION bm25_spill_index(index_name text)
RETURNS int4
AS 'MODULE_PATHNAME', 'tp_spill_memtable'
LANGUAGE C VOLATILE STRICT;

-- Drop old debug function and create new one with simpler name
DROP FUNCTION IF EXISTS bm25_debug_dump_index(text, boolean);
CREATE FUNCTION bm25_dump_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_dump_index'
    LANGUAGE C STRICT STABLE;
