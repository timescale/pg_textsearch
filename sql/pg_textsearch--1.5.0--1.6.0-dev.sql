-- Upgrade from 1.5.0 to 1.6.0-dev

-- Verify the library is loaded. The version-equality check lives in
-- the main install file (pg_textsearch--<default_version>.sql); upgrade
-- scripts must accept any loaded version because they may run as
-- intermediate steps in a chain that ends at default_version, not at
-- the version named in this filename.
DO $$
BEGIN
    IF pg_catalog.current_setting('pg_textsearch.library_version', true)
       IS NULL THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
END $$;

-- Inspect current header facts without reading index contents.
CREATE FUNCTION @extschema@.bm25_index_info(
    idx regclass,
    OUT metapage_version bigint,
    OUT capabilities bigint,
    OUT all_documents_indexed boolean,
    OUT legacy_docid_pointer_present boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'tp_index_info'
LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;

COMMENT ON FUNCTION @extschema@.bm25_index_info(regclass) IS
    'Report current BM25 header facts. Requires ownership of the index or MAINTAIN on its table (the privilege REINDEX requires). NULL means a field is absent or its layout is unknown, or, for every field, that the index has no header yet; capability bits and legacy pointers do not attest to index completeness.';
