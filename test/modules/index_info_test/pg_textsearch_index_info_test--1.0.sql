\echo Use "CREATE EXTENSION pg_textsearch_index_info_test" to load this file. \quit

CREATE FUNCTION index_info_test_header(
    version bigint, bits bigint, legacy_pointer boolean,
    used_bytes integer, valid_magic boolean,
    OUT metapage_version bigint, OUT capabilities bigint,
    OUT all_documents_indexed boolean,
    OUT legacy_docid_pointer_present boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'index_info_test_header'
LANGUAGE C STRICT;

-- A valid version 9 header on a page whose PostgreSQL page header claims
-- the given page size and layout version.
CREATE FUNCTION index_info_test_header_layout(
    page_size integer, layout_version integer,
    OUT metapage_version bigint, OUT capabilities bigint,
    OUT all_documents_indexed boolean,
    OUT legacy_docid_pointer_present boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'index_info_test_header_layout'
LANGUAGE C STRICT;

-- The same C function declared with a scalar result, to exercise the
-- result-type check in tp_index_header_info_datum().
CREATE FUNCTION index_info_test_header_scalar(
    version bigint, bits bigint, legacy_pointer boolean,
    used_bytes integer, valid_magic boolean)
RETURNS bigint
AS 'MODULE_PATHNAME', 'index_info_test_header'
LANGUAGE C STRICT;

CREATE FUNCTION index_info_test_inspect(
    regclass,
    OUT metapage_version bigint, OUT capabilities bigint,
    OUT all_documents_indexed boolean,
    OUT legacy_docid_pointer_present boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'index_info_test_inspect'
LANGUAGE C STRICT;

CREATE FUNCTION index_info_test_raw_page(regclass) RETURNS bytea
AS 'MODULE_PATHNAME', 'index_info_test_raw_page'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION
    index_info_test_header(bigint, bigint, boolean, integer, boolean)
    FROM PUBLIC;
REVOKE ALL ON FUNCTION
    index_info_test_header_scalar(bigint, bigint, boolean, integer, boolean)
    FROM PUBLIC;
REVOKE ALL ON FUNCTION index_info_test_raw_page(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION index_info_test_inspect(regclass) FROM PUBLIC;
REVOKE ALL ON FUNCTION index_info_test_header_layout(integer, integer)
    FROM PUBLIC;
