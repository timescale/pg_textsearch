/* Test-only synthetic headers and raw-page comparisons; never shipped. */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <access/relation.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/rel.h>

#include "access/index_info.h"
#include "index/metapage.h"

PG_MODULE_MAGIC;

/*
 * PostgreSQL loads extension libraries locally, not into the global symbol
 * namespace. Resolve pg_textsearch functions from its own library handle.
 */
typedef void (*index_info_test_fn)(void);

static index_info_test_fn
index_info_test_lookup(const char *name)
{
	return (index_info_test_fn)
			load_external_function("$libdir/pg_textsearch", name, true, NULL);
}

PG_FUNCTION_INFO_V1(index_info_test_header);
PG_FUNCTION_INFO_V1(index_info_test_header_layout);
PG_FUNCTION_INFO_V1(index_info_test_raw_page);
PG_FUNCTION_INFO_V1(index_info_test_inspect);

/* Build a page whose metapage fields and pd_lower are as requested. */
static Page
index_info_test_build_page(
		int64 version, int64 bits, bool legacy_pointer, int32 used, bool magic)
{
	Page			page;
	TpIndexMetaPage meta;

	if (version < 0 || version > PG_UINT32_MAX || bits < 0 ||
		bits > PG_UINT32_MAX || used < 0 ||
		(Size)used > BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
		elog(ERROR, "invalid test header arguments");

	page = palloc0(BLCKSZ);
	PageInit(page, BLCKSZ, 0);
	meta						 = (TpIndexMetaPage)PageGetContents(page);
	meta->magic					 = magic ? TP_METAPAGE_MAGIC : 0;
	meta->version				 = (uint32)version;
	meta->capabilities			 = (uint32)bits;
	meta->_unused_docid_page	 = legacy_pointer ? 123 : InvalidBlockNumber;
	((PageHeader)page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + used;
	return page;
}

/*
 * Run the header parser on a synthetic page, check that it left the page
 * unchanged, and return the same row bm25_index_info() would.
 */
static Datum
index_info_test_inspect_page(FunctionCallInfo fcinfo, Page page)
{
	Page			  before = palloc(BLCKSZ);
	TpIndexHeaderInfo info;
	Datum			  result;
	void (*inspect)(Page, TpIndexHeaderInfo *);
	Datum (*to_datum)(FunctionCallInfo, const TpIndexHeaderInfo *);

	inspect = (void (*)(Page, TpIndexHeaderInfo *))index_info_test_lookup(
			"tp_inspect_metapage_page");
	to_datum = (Datum (*)(FunctionCallInfo, const TpIndexHeaderInfo *))
			index_info_test_lookup("tp_index_header_info_datum");

	memcpy(before, page, BLCKSZ);
	inspect(page, &info);
	if (memcmp(before, page, BLCKSZ) != 0)
		elog(ERROR, "inspection changed a synthetic page");

	result = to_datum(fcinfo, &info);
	pfree(before);
	return result;
}

Datum
index_info_test_header(PG_FUNCTION_ARGS)
{
	Page  page;
	Datum result;

	if (!superuser())
		elog(ERROR, "must be superuser to construct test headers");

	page = index_info_test_build_page(
			PG_GETARG_INT64(0),
			PG_GETARG_INT64(1),
			PG_GETARG_BOOL(2),
			PG_GETARG_INT32(3),
			PG_GETARG_BOOL(4));
	result = index_info_test_inspect_page(fcinfo, page);
	pfree(page);
	PG_RETURN_DATUM(result);
}

/*
 * A valid version 9 header on a page whose PostgreSQL header claims the
 * given page size and layout version.  The field is written directly:
 * PageSetPageSizeAndVersion() asserts on the values this test needs.
 */
Datum
index_info_test_header_layout(PG_FUNCTION_ARGS)
{
	int32 page_size		 = PG_GETARG_INT32(0);
	int32 layout_version = PG_GETARG_INT32(1);
	Page  page;
	Datum result;

	if (!superuser())
		elog(ERROR, "must be superuser to construct test headers");
	if (page_size < 0 || page_size > 0xFF00 || (page_size & 0xFF) != 0 ||
		layout_version < 0 || layout_version > 0xFF)
		elog(ERROR, "invalid test page layout arguments");

	page = index_info_test_build_page(
			TP_METAPAGE_VERSION_V9, 1, false, 128, true);
	((PageHeader)page)->pd_pagesize_version = (uint16)(page_size |
													   layout_version);
	result = index_info_test_inspect_page(fcinfo, page);
	pfree(page);
	PG_RETURN_DATUM(result);
}

Datum
index_info_test_raw_page(PG_FUNCTION_ARGS)
{
	Relation index;
	Buffer	 buf;
	bytea	*result;

	if (!superuser())
		elog(ERROR, "must be superuser to inspect raw test pages");
	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	buf	  = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	result = palloc(VARHDRSZ + BLCKSZ);
	SET_VARSIZE(result, VARHDRSZ + BLCKSZ);
	memcpy(VARDATA(result), BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	index_close(index, AccessShareLock);
	PG_RETURN_BYTEA_P(result);
}

/*
 * Run the header I/O wrapper on any relation, skipping the bm25 checks in
 * bm25_index_info(), so that a relation without block 0 can be tested.
 */
Datum
index_info_test_inspect(PG_FUNCTION_ARGS)
{
	Relation		  rel;
	TpIndexHeaderInfo info;
	TpIndexHeaderInfo (*inspect)(Relation);
	Datum (*to_datum)(FunctionCallInfo, const TpIndexHeaderInfo *);

	if (!superuser())
		elog(ERROR, "must be superuser to inspect test relations");
	inspect = (TpIndexHeaderInfo (*)(Relation))index_info_test_lookup(
			"tp_inspect_metapage");
	to_datum = (Datum (*)(FunctionCallInfo, const TpIndexHeaderInfo *))
			index_info_test_lookup("tp_index_header_info_datum");

	rel	 = relation_open(PG_GETARG_OID(0), AccessShareLock);
	info = inspect(rel);
	relation_close(rel, AccessShareLock);
	PG_RETURN_DATUM(to_datum(fcinfo, &info));
}
