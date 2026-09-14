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
#include <nodes/pg_list.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/memutils.h>
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
	metap->pending_free_head   = InvalidBlockNumber;

	/* Update page header to reflect that we've used space for metapage */
	phdr		   = (PageHeader)page;
	phdr->pd_lower = SizeOfPageHeaderData + sizeof(TpIndexMetaPageData);
}

void
tp_check_level_count_increment(TpIndexMetaPage metap, uint32 level)
{
	Assert(metap != NULL);

	if (level >= TP_MAX_LEVELS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid bm25 segment level %u", level)));

	if (metap->level_counts[level] >= tp_max_segments_per_level)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("bm25 segment count limit reached at level %u",
						level)));
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
	 * the binary swap and the pointer is stale bookkeeping, or
	 * (b) v1.2.x was SIGKILL'd with in-flight documents in the
	 * chain and those documents are now lost.  We cannot
	 * distinguish those cases from the on-disk bytes alone, so
	 * the upgrade helper emits a client-visible WARNING (once,
	 * on first write) and PRESERVES the pointer as a durable
	 * marker; the scan path re-surfaces it via
	 * tp_warn_if_pending_docid() until a REINDEX clears it.
	 * Operators who suspect lost documents should REINDEX.
	 *
	 * v5 and earlier are not read-compatible and require
	 * REINDEX as before.
	 */
	if (metap->version != TP_METAPAGE_VERSION &&
		metap->version != TP_METAPAGE_VERSION_V7 &&
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
	 * is accepted here as well — the operator-visible WARNING
	 * fires once, from tp_metapage_upgrade_to_current() on the
	 * first write, at the point we upgrade the page and preserve
	 * the pointer as a durable marker.  See the rationale block
	 * in that function.  The scan path re-surfaces the marker via
	 * tp_warn_if_pending_docid(); this function stays silent
	 * because it is on the hot read path.
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
	else if (metap->version == TP_METAPAGE_VERSION_V7)
	{
		/* v7 has memtable head/tail but no pending_free_head. */
		memcpy(result, metap, TP_INDEX_METAPAGE_DATA_SIZE_V7);
		result->pending_free_head = InvalidBlockNumber;
	}
	else
	{
		Assert(metap->version == TP_METAPAGE_VERSION_V6);
		memcpy(result, metap, TP_INDEX_METAPAGE_DATA_SIZE_V6);
		result->memtable_head_blkno = InvalidBlockNumber;
		result->memtable_tail_blkno = InvalidBlockNumber;
		result->pending_free_head	= InvalidBlockNumber;
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

	metap = (TpIndexMetaPage)PageGetContents(page);

	if (metap->version == TP_METAPAGE_VERSION)
		return; /* already v8 */

	if (metap->version != TP_METAPAGE_VERSION_V6 &&
		metap->version != TP_METAPAGE_VERSION_V7)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch: cannot upgrade metapage "
						"with unknown version %u",
						metap->version),
				 errhint("Caller must validate the metapage version "
						 "via tp_get_metapage() before calling "
						 "tp_metapage_upgrade_to_current().")));

	/*
	 * v6 lacks the memtable chain fields and may carry a stale
	 * docid pointer; v7 already has the chain fields.  Only the
	 * v6 path initializes memtable head/tail and inspects the
	 * legacy docid pointer.
	 */
	if (metap->version == TP_METAPAGE_VERSION_V6)
	{
		BlockNumber orphan_docid_page = metap->_unused_docid_page;

		metap->memtable_head_blkno = InvalidBlockNumber;
		metap->memtable_tail_blkno = InvalidBlockNumber;

		if (BlockNumberIsValid(orphan_docid_page) && !RecoveryInProgress())
			ereport(WARNING,
					(errmsg("pg_textsearch: upgraded index \"%s\" "
							"from a pre-v1.3 on-disk format that may "
							"have held unspilled documents",
							RelationGetRelationName(index)),
					 errdetail(
							 "The pre-v1.3 docid chain (first page "
							 "%u) replayed in-flight documents from "
							 "the heap on restart; v1.3+ has no "
							 "equivalent, so any documents not "
							 "spilled before the binary swap are "
							 "absent from the index.",
							 orphan_docid_page),
					 errhint("Run \"REINDEX INDEX %s;\" to rebuild "
							 "from the table and clear this warning.",
							 RelationGetRelationName(index))));

		/*
		 * Preserve orphan_docid_page as a durable "results may be
		 * incomplete" marker.  It rides through v8 in the
		 * _unused_docid_page slot and is re-surfaced to clients on
		 * the scan path (at most once per session) by
		 * tp_warn_if_pending_docid() until a REINDEX rebuilds the
		 * index from the heap and clears it.  Do NOT reset it to
		 * InvalidBlockNumber here.
		 */
	}

	/* Common to v6 and v7: introduce the deferred-free chain head. */
	metap->pending_free_head = InvalidBlockNumber;

	metap->version = TP_METAPAGE_VERSION;

	/*
	 * Bump pd_lower so every newly-included field is inside the
	 * "used" area; GenericXLog skips the [pd_lower, pd_upper) hole
	 * and would otherwise zero the new fields on replay.
	 */
	{
		Size v8_pd_lower = SizeOfPageHeaderData + sizeof(TpIndexMetaPageData);
		phdr			 = (PageHeader)page;
		if (phdr->pd_lower < v8_pd_lower)
			phdr->pd_lower = v8_pd_lower;
	}
}

/*
 * Per-backend set of index OIDs already inspected for the
 * "results may be incomplete" marker this session.  Lives in
 * TopMemoryContext so it survives the transient scan contexts.  An OID
 * is recorded on the FIRST inspection regardless of outcome, so a
 * clean index costs at most one metapage read per session and a marked
 * index warns at most once -- neither floods a plan that reopens the
 * scan many times (e.g. the inner side of a nested loop).
 */
static List *tp_pending_docid_seen = NIL;

/*
 * tp_warn_if_pending_docid
 *
 * Read-path guard for the pre-v1.3 upgrade marker.  When a pre-v1.3
 * index was upgraded while it still carried unspilled documents,
 * tp_metapage_upgrade_to_current() preserves the legacy docid pointer
 * in _unused_docid_page as a durable "results may be incomplete"
 * marker.  This helper surfaces that marker to the client as a WARNING
 * on the scan path, so read-only workloads -- which never trigger the
 * write-path upgrade WARNING -- still learn that a REINDEX is needed.
 *
 * Inspects the metapage at most once per index per backend session
 * (see tp_pending_docid_seen).  Runs during recovery too: a hot
 * standby serving reads against an upgraded primary must still see the
 * advisory, even though the REINDEX that clears it has to run on the
 * primary.
 */
void
tp_warn_if_pending_docid(Relation index)
{
	Oid				index_oid = RelationGetRelid(index);
	TpIndexMetaPage metap;
	BlockNumber		marker;
	MemoryContext	old;

	if (list_member_oid(tp_pending_docid_seen, index_oid))
		return;

	metap  = tp_get_metapage(index);
	marker = metap->_unused_docid_page;
	pfree(metap);

	/*
	 * Record the index before (possibly) warning so a clean index is
	 * never re-read and a marked index warns only once this session.
	 */
	old					  = MemoryContextSwitchTo(TopMemoryContext);
	tp_pending_docid_seen = lappend_oid(tp_pending_docid_seen, index_oid);
	MemoryContextSwitchTo(old);

	if (!BlockNumberIsValid(marker))
		return;

	ereport(WARNING,
			(errmsg("pg_textsearch: index \"%s\" may return "
					"incomplete results after upgrade from a "
					"pre-v1.3 on-disk format",
					RelationGetRelationName(index)),
			 errhint("Some documents inserted before the "
					 "upgrade may be missing. Run \"REINDEX "
					 "INDEX %s;\" to rebuild from the table and "
					 "clear this warning.",
					 RelationGetRelationName(index))));
}
