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

#include <miscadmin.h>
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
	 * Check version compatibility.
	 *
	 * v7 (current) is the on-disk memtable redesign (issue
	 * #374).  v6 is read-compatible (issue #383): the layout is
	 * byte-identical up through level_counts[]; v7 only appends
	 * memtable_head_blkno and memtable_tail_blkno.  We accept
	 * v6 here and normalize the missing fields to
	 * InvalidBlockNumber in the returned copy.  The first
	 * metapage mutation upgrades the on-disk page to v7
	 * atomically via tp_metapage_upgrade_to_current() inside
	 * the surrounding GenericXLog record.
	 *
	 * v6 with a non-Invalid _unused_docid_page (formerly
	 * first_docid_page) is also accepted here.  It indicates
	 * either (a) the operator called bm25_spill_index() before
	 * the binary swap and the pointer is stale bookkeeping
	 * safe to orphan, or (b) v1.2.x was SIGKILL'd with
	 * in-flight documents in the chain and those documents
	 * are now lost.  We cannot distinguish those cases from
	 * the on-disk bytes alone, so we log a LOG-level forensic
	 * message from the upgrade helper (which fires once, on
	 * first write) and proceed.  Operators who suspect lost
	 * documents should REINDEX.
	 *
	 * v5 and earlier are not read-compatible and require
	 * REINDEX as before.
	 */
	if (metap->version != TP_METAPAGE_VERSION &&
		metap->version != TP_METAPAGE_VERSION_V6)
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

	/*
	 * Note: a v6 metapage with a non-Invalid _unused_docid_page
	 * is accepted here as well — the operator-visible LOG fires
	 * once, from tp_metapage_upgrade_to_current() on the first
	 * write, at the point we actually orphan the pointer.  See
	 * the rationale block in that function.  Emitting the LOG
	 * here would spam (this function is on the hot path; the
	 * upgrade fires at most once per index lifetime).
	 */

	/*
	 * Copy metapage data to avoid buffer issues.  On v6 we only
	 * copy the v6-sized prefix and then explicitly initialize
	 * the v7-new fields, since the on-disk bytes at those
	 * offsets are unrelated (PageInit zero-fill, NOT
	 * InvalidBlockNumber = 0xFFFFFFFF).
	 */
	result = (TpIndexMetaPage)palloc(sizeof(TpIndexMetaPageData));
	if (metap->version == TP_METAPAGE_VERSION)
	{
		memcpy(result, metap, sizeof(TpIndexMetaPageData));
	}
	else
	{
		Assert(metap->version == TP_METAPAGE_VERSION_V6);
		memcpy(result, metap, TP_INDEX_METAPAGE_DATA_SIZE_V6);
		result->memtable_head_blkno = InvalidBlockNumber;
		result->memtable_tail_blkno = InvalidBlockNumber;
	}

	UnlockReleaseBuffer(buf);
	return result;
}

/*
 * Read memtable_head_blkno from a metapage buffer page in a
 * version-tolerant way (issue #383).  See header for contract.
 */
BlockNumber
tp_metapage_read_memtable_head(Page page)
{
	TpIndexMetaPage metap = (TpIndexMetaPage)PageGetContents(page);

	if (metap->version == TP_METAPAGE_VERSION_V6)
		return InvalidBlockNumber;

	return metap->memtable_head_blkno;
}

/*
 * Read memtable_tail_blkno from a metapage buffer page in a
 * version-tolerant way (issue #383).  See header for contract.
 */
BlockNumber
tp_metapage_read_memtable_tail(Page page)
{
	TpIndexMetaPage metap = (TpIndexMetaPage)PageGetContents(page);

	if (metap->version == TP_METAPAGE_VERSION_V6)
		return InvalidBlockNumber;

	return metap->memtable_tail_blkno;
}

/*
 * In-place v6 -> v7 upgrade (issue #383).  See header for
 * caller contract.  No-op when the page is already v7.
 */
void
tp_metapage_upgrade_to_current(Relation index, Page page)
{
	TpIndexMetaPage metap;
	PageHeader		phdr;
	Size			v7_pd_lower;
	BlockNumber		orphan_docid_page;

	metap = (TpIndexMetaPage)PageGetContents(page);

	if (metap->version == TP_METAPAGE_VERSION)
		return;

	/*
	 * tp_get_metapage() is the only legitimate way to first
	 * observe the on-disk version, and it has already filtered
	 * out anything besides v6/v7.  Reaching this function with
	 * any other version means a caller mutated the metapage
	 * without going through the open path — refuse to stamp
	 * an unknown layout as v7, which would silently corrupt
	 * the page (the new chain head/tail fields may land on
	 * top of meaningful bytes in a future format).
	 */
	if (metap->version != TP_METAPAGE_VERSION_V6)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch: cannot upgrade metapage "
						"with unknown version %u",
						metap->version),
				 errhint("Caller must validate the metapage version "
						 "via tp_get_metapage() before calling "
						 "tp_metapage_upgrade_to_current().")));

	orphan_docid_page = metap->_unused_docid_page;

	metap->memtable_head_blkno = InvalidBlockNumber;
	metap->memtable_tail_blkno = InvalidBlockNumber;

	/*
	 * Orphan any stale v1.2.x docid-chain pointer.
	 *
	 * v1.2.x and earlier maintained a "docid chain" of pages
	 * anchored by first_docid_page (now renamed
	 * _unused_docid_page).  Inserts wrote ctids there before
	 * the in-memory memtable; on a clean restart the chain was
	 * replayed to re-tokenize pending documents from heap.
	 * v1.3 (issue #374) replaced the docid chain with an
	 * on-disk memtable, so the pointer's target is meaningless
	 * to the v1.3+ binary.
	 *
	 * If the pointer is non-Invalid at upgrade time, the
	 * operator either:
	 *   - called bm25_spill_index() before the binary swap
	 *     (data is in segments; the chain is stale bookkeeping
	 *     safe to orphan), or
	 *   - did not give v1.2.x a chance to drain (in-flight
	 *     documents in the chain are lost — they were
	 *     scheduled for re-tokenization that v1.3 cannot do).
	 *
	 * We cannot distinguish those cases from the on-disk bytes
	 * alone.  Log a one-time LOG-level message recording the
	 * orphaned pointer for forensics and proceed; the upgrade
	 * fires at most once per index lifetime so this LOG fires
	 * at most once per upgraded index (vs the previous design
	 * which logged from tp_get_metapage and so fired once per
	 * backend per opened-but-not-yet-written v6 index).
	 *
	 * RecoveryInProgress() suppresses the LOG on a hot standby
	 * — standbys never reach this function (it runs only on
	 * the primary, inside a writer's GenericXLog scope), but
	 * the guard documents that intent.
	 */
	if (BlockNumberIsValid(orphan_docid_page) && !RecoveryInProgress())
		ereport(LOG,
				(errmsg("pg_textsearch: upgrading index \"%s\" "
						"from a pre-v1.3 metapage (v6) and "
						"orphaning a non-empty docid chain "
						"pointer (first_docid_page = %u)",
						RelationGetRelationName(index),
						orphan_docid_page),
				 errdetail(
						 "The docid chain was a v1.2.x mechanism for "
						 "replaying in-flight documents from heap on "
						 "restart.  v1.3 has no equivalent.  If you "
						 "did not call bm25_spill_index() on this "
						 "index before swapping binaries, some "
						 "recently-inserted documents may be missing "
						 "from query results."),
				 errhint("If query results look incomplete, "
						 "REINDEX INDEX %s to rebuild from heap.",
						 RelationGetRelationName(index))));

	metap->_unused_docid_page = InvalidBlockNumber;
	metap->version			  = TP_METAPAGE_VERSION;

	/*
	 * Bump pd_lower so the two newly-included BlockNumber
	 * fields are inside the "used" area of the page.
	 * GenericXLog computes the page hole as
	 * [pd_lower, pd_upper) and skips that region in the
	 * recorded image — if we didn't bump pd_lower, the new
	 * fields would land in the hole and replay would zero
	 * them, leaving a v7 page with chain head/tail == 0 (i.e.
	 * pointing at the metapage block itself).
	 */
	v7_pd_lower = SizeOfPageHeaderData + sizeof(TpIndexMetaPageData);
	phdr		= (PageHeader)page;
	if (phdr->pd_lower < v7_pd_lower)
		phdr->pd_lower = v7_pd_lower;
}
