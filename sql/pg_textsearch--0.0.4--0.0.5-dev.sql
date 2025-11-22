-- Upgrade from 0.0.4 to 0.0.5-dev

-- Function to force segment write (spill memtable to disk)
CREATE OR REPLACE FUNCTION bm25_spill_index(index_name text)
RETURNS int4
AS 'MODULE_PATHNAME', 'tp_spill_memtable'
LANGUAGE C VOLATILE STRICT;

-- New unified debug dump function
CREATE FUNCTION bm25_debug_dump(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_debug_dump'
    LANGUAGE C STRICT STABLE;
