/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * metapage.c - Index metapage operations
 *
 * Handles metapage initialization, reading, and management. The metapage
 * stores index configuration, statistics, and segment chain heads.
 */
#include <postgres.h>

#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/rel.h>

#include "constants.h"
#include "index/metapage.h"

/*
 * Initialize Tapir index metapage
 */
void
tp_init_metapage(Page page, Oid text_config_oid)
{
	TpIndexMetaPage metap;
	PageHeader		phdr;
	int				i;

	/*
	 * Initialize page with no special space - metapage uses page content area
	 */
	PageInit(page, BLCKSZ, 0);
	metap = (TpIndexMetaPage)PageGetContents(page);

	metap->magic			   = TP_METAPAGE_MAGIC;
	metap->version			   = TP_METAPAGE_VERSION;
	metap->text_config_oid	   = text_config_oid;
	metap->total_docs		   = 0;
	metap->_unused_total_terms = 0;
	metap->total_len		   = 0;
	metap->root_blkno		   = InvalidBlockNumber;
	metap->_unused_docid_page  = InvalidBlockNumber;

	/* Initialize hierarchical segment levels */
	for (i = 0; i < TP_MAX_LEVELS; i++)
	{
		metap->level_heads[i]  = InvalidBlockNumber;
		metap->level_counts[i] = 0;
	}

	/*
	 * Memtable v2 chain: explicitly InvalidBlockNumber (=
	 * 0xFFFFFFFF) — NOT zero, since block 0 is the metapage
	 * itself.  See TP_METAPAGE_VERSION in constants.h.
	 */
	metap->memtable_head_blkno = InvalidBlockNumber;
	metap->memtable_tail_blkno = InvalidBlockNumber;

	/* Update page header to reflect that we've used space for metapage */
	phdr		   = (PageHeader)page;
	phdr->pd_lower = SizeOfPageHeaderData + sizeof(TpIndexMetaPageData);
}

/*
 * Get Tapir index metapage
 */
TpIndexMetaPage
tp_get_metapage(Relation index)
{
	Buffer			buf;
	Page			page;
	TpIndexMetaPage metap;
	TpIndexMetaPage result;

	/* Validate input relation */
	if (!RelationIsValid(index))
		elog(ERROR, "invalid relation passed to tp_get_metapage");

	buf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	if (!BufferIsValid(buf))
	{
		elog(ERROR,
			 "failed to read metapage buffer for BM25 index \"%s\"",
			 RelationGetRelationName(index));
	}

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	metap = (TpIndexMetaPage)PageGetContents(page);
	if (!metap)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR,
			 "failed to get metapage contents for BM25 index \"%s\"",
			 RelationGetRelationName(index));
	}

	/* Validate magic number */
	if (metap->magic != TP_METAPAGE_MAGIC)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR,
			 "Tapir index metapage is corrupted for index \"%s\": expected "
			 "magic "
			 "0x%08X, found 0x%08X",
			 RelationGetRelationName(index),
			 TP_METAPAGE_MAGIC,
			 metap->magic);
	}

	/*
	 * Check version compatibility.  v6 was the last
	 * shared-memory-memtable release; v7 (this version) replaces
	 * that with an on-disk paged chain and drops the docid
	 * recovery pages.  A v6 index may have unspilled documents
	 * in docid pages (since removed) that the new code cannot
	 * read, so we reject v6 with an explicit REINDEX hint rather
	 * than silently truncating the corpus.
	 */
	if (metap->version != TP_METAPAGE_VERSION)
	{
		uint32 found_version = metap->version;

		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible pg_textsearch index version for "
						"\"%s\": found %u, expected %u",
						RelationGetRelationName(index),
						found_version,
						TP_METAPAGE_VERSION),
				 errhint("This index was created by a previous release of "
						 "pg_textsearch and uses an incompatible on-disk "
						 "format.  Run REINDEX INDEX %s to rebuild it.",
						 RelationGetRelationName(index))));
	}

	/* Copy metapage data to avoid buffer issues */
	result = (TpIndexMetaPage)palloc(sizeof(TpIndexMetaPageData));
	memcpy(result, metap, sizeof(TpIndexMetaPageData));

	UnlockReleaseBuffer(buf);
	return result;
}
