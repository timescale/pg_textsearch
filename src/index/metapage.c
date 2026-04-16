/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * metapage.c - Index metapage operations
 *
 * Handles metapage initialization, reading, and management. The metapage
 * stores index configuration, statistics, and crash recovery state.
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/heapam.h>
#include <catalog/index.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/builtins.h>
#include <utils/rel.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/state.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "types/vector.h"

/* Maximum number of docids that fit in a page */
#define TP_DOCIDS_PER_PAGE                   \
	((BLCKSZ - sizeof(PageHeaderData) -      \
	  MAXALIGN(sizeof(TpDocidPageHeader))) / \
	 sizeof(ItemPointerData))

/*
 * Cached state for the docid page writer.
 * This avoids O(n) chain traversal on every insert by remembering
 * the last page we wrote to.
 */
typedef struct TpDocidWriterState
{
	Oid			index_oid;	/* Index this state is for */
	BlockNumber last_page;	/* Last docid page written to */
	uint32		num_docids; /* Number of docids on that page */
	uint64		generation; /* Spill generation at cache time */
	bool		valid;		/* Is this cache entry valid? */
} TpDocidWriterState;

/* Backend-local cache for docid writer */
static TpDocidWriterState docid_writer_cache =
		{InvalidOid, InvalidBlockNumber, 0, 0, false};

/*
 * Invalidate the docid writer cache.
 * This must be called at the start of index build to prevent stale cache
 * entries from a previous index (e.g., during VACUUM FULL which creates
 * a new index file with different block layout).
 */
void
tp_invalidate_docid_cache(void)
{
	docid_writer_cache.valid = false;
}

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
	metap->first_docid_page	   = InvalidBlockNumber;

	/* Initialize hierarchical segment levels */
	for (i = 0; i < TP_MAX_LEVELS; i++)
	{
		metap->level_heads[i]  = InvalidBlockNumber;
		metap->level_counts[i] = 0;
	}

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

	/* Check version compatibility */
	if (metap->version != TP_METAPAGE_VERSION)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR,
			 "Incompatible index version for \"%s\": found version %d, "
			 "expected %d. Please drop and recreate the index.",
			 RelationGetRelationName(index),
			 metap->version,
			 TP_METAPAGE_VERSION);
	}

	/* Copy metapage data to avoid buffer issues */
	result = (TpIndexMetaPage)palloc(sizeof(TpIndexMetaPageData));
	memcpy(result, metap, sizeof(TpIndexMetaPageData));

	UnlockReleaseBuffer(buf);
	return result;
}

/*
 * Add a document ID to the docid pages for crash recovery
 * This appends the ctid to the chain of docid pages
 *
 * OPTIMIZATION: Uses a backend-local cache to remember the last docid page
 * we wrote to. This avoids O(n) chain traversal on every insert, reducing
 * complexity from O(n²) to O(n) for building an index with n documents.
 */
void
tp_add_docid_to_pages(Relation index, ItemPointer ctid)
{
	Buffer			   metabuf	 = InvalidBuffer;
	Buffer			   docid_buf = InvalidBuffer;
	Page			   metapage, docid_page;
	TpIndexMetaPage	   metap;
	TpDocidPageHeader *docid_header;
	BlockNumber		   target_page;
	uint32			   page_capacity = TP_DOCIDS_PER_PAGE;
	Oid				   index_oid	 = RelationGetRelid(index);
	bool			   cache_valid;

	/*
	 * Check if we have a valid cache entry for this index.
	 * The cache is valid if it's for the same index, the
	 * page isn't full, and the spill generation matches
	 * (a concurrent spill invalidates all caches by
	 * incrementing the generation counter).
	 */
	cache_valid = docid_writer_cache.valid &&
				  docid_writer_cache.index_oid == index_oid &&
				  docid_writer_cache.last_page != InvalidBlockNumber &&
				  docid_writer_cache.num_docids < page_capacity;

	if (cache_valid)
	{
		TpLocalIndexState *ls = tp_get_local_index_state(index_oid);
		if (ls && ls->shared &&
			docid_writer_cache.generation !=
					pg_atomic_read_u64(&ls->shared->spill_generation))
		{
			docid_writer_cache.valid = false;
			cache_valid				 = false;
		}
	}

	if (cache_valid)
	{
		/*
		 * Fast path: use cached page directly without touching metapage.
		 * We still need to verify the page is valid in case of concurrent
		 * modifications (rare during index build, but possible).
		 */
		target_page = docid_writer_cache.last_page;
		docid_buf	= ReadBuffer(index, target_page);
		LockBuffer(docid_buf, BUFFER_LOCK_EXCLUSIVE);

		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/* Verify page is still valid and has room */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC ||
			docid_header->num_docids >= page_capacity)
		{
			/* Cache is stale, invalidate and fall through to slow path */
			UnlockReleaseBuffer(docid_buf);
			docid_buf				 = InvalidBuffer;
			docid_writer_cache.valid = false;
			cache_valid				 = false;
		}
	}

	if (!cache_valid)
	{
		/*
		 * Slow path: need to access metapage to find/create docid page.
		 * This happens on first insert or when cache is invalidated.
		 */
		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		target_page = metap->first_docid_page;

		if (target_page == InvalidBlockNumber)
		{
			GenericXLogState *xlog_state;

			/* No docid pages yet, create the first one */
			docid_buf = ReadBuffer(index, P_NEW);
			LockBuffer(docid_buf, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * Register both the new docid page and the
			 * metapage in one GenericXLog operation. This
			 * ensures atomicity: both the new page init and
			 * the metapage pointer update are WAL-logged
			 * together, so a crash cannot leave
			 * first_docid_page pointing to an uninitialized
			 * block.
			 */
			xlog_state = GenericXLogStart(index);
			docid_page = GenericXLogRegisterBuffer(
					xlog_state, docid_buf, GENERIC_XLOG_FULL_IMAGE);
			PageInit(docid_page, BLCKSZ, 0);

			docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);
			docid_header->magic		 = TP_DOCID_PAGE_MAGIC;
			docid_header->version	 = TP_DOCID_PAGE_VERSION;
			docid_header->num_docids = 0;
			docid_header->next_page	 = InvalidBlockNumber;

			/*
			 * Mark the full content area as used so
			 * GenericXLog does not zero it. Docid pages
			 * manage their own layout and do not use the
			 * standard Postgres ItemId/tuple mechanism.
			 */
			((PageHeader)docid_page)->pd_lower = BLCKSZ;

			/* Update metapage to point to this new page */
			target_page = BufferGetBlockNumber(docid_buf);
			{
				Page			metapage_copy;
				TpIndexMetaPage metap_copy;

				metapage_copy =
						GenericXLogRegisterBuffer(xlog_state, metabuf, 0);
				metap_copy = (TpIndexMetaPage)PageGetContents(metapage_copy);
				metap_copy->first_docid_page = target_page;
			}

			GenericXLogFinish(xlog_state);

			/*
			 * Re-read from the actual buffer page since the
			 * GenericXLog copy is no longer valid after Finish.
			 */
			docid_page	 = BufferGetPage(docid_buf);
			docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);
		}
		else
		{
			/*
			 * Walk chain to find last page. After this, we cache it so
			 * subsequent calls are O(1).
			 */
			BlockNumber current_page = target_page;
			BlockNumber next_page;

			while (current_page != InvalidBlockNumber)
			{
				docid_buf = ReadBuffer(index, current_page);
				LockBuffer(docid_buf, BUFFER_LOCK_SHARE);

				docid_page	 = BufferGetPage(docid_buf);
				docid_header = (TpDocidPageHeader *)PageGetContents(
						docid_page);
				next_page = docid_header->next_page;

				if (next_page == InvalidBlockNumber)
				{
					/* Found last page - upgrade to exclusive lock */
					LockBuffer(docid_buf, BUFFER_LOCK_UNLOCK);
					LockBuffer(docid_buf, BUFFER_LOCK_EXCLUSIVE);
					docid_header = (TpDocidPageHeader *)PageGetContents(
							docid_page);

					/*
					 * Re-check: another backend may have extended
					 * the chain while we released the lock.
					 */
					if (docid_header->next_page != InvalidBlockNumber)
					{
						next_page = docid_header->next_page;
						UnlockReleaseBuffer(docid_buf);
						current_page = next_page;
						continue;
					}

					target_page = current_page;
					break;
				}

				UnlockReleaseBuffer(docid_buf);
				current_page = next_page;
			}
		}

		/* Release metapage early - we don't need it anymore */
		if (BufferIsValid(metabuf))
			UnlockReleaseBuffer(metabuf);
	}

	/* Check if current page has room for another docid */
	if (docid_header->num_docids >= page_capacity)
	{
		/* Current page is full, create a new one */
		Buffer			   new_buf;
		Page			   new_docid_page;
		TpDocidPageHeader *new_header;
		GenericXLogState  *xlog_state;
		Page			   old_copy;
		TpDocidPageHeader *old_copy_hdr;

		new_buf = ReadBuffer(index, P_NEW);
		LockBuffer(new_buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * Register both the new page and the old page in one
		 * GenericXLog operation. This ensures the new page
		 * init and the old page's next_page pointer are
		 * WAL-logged atomically.
		 */
		xlog_state	   = GenericXLogStart(index);
		new_docid_page = GenericXLogRegisterBuffer(
				xlog_state, new_buf, GENERIC_XLOG_FULL_IMAGE);
		PageInit(new_docid_page, BLCKSZ, 0);

		new_header = (TpDocidPageHeader *)PageGetContents(new_docid_page);
		new_header->magic	   = TP_DOCID_PAGE_MAGIC;
		new_header->version	   = TP_DOCID_PAGE_VERSION;
		new_header->num_docids = 0;
		new_header->next_page  = InvalidBlockNumber;

		/* Mark full content area as used for GenericXLog */
		((PageHeader)new_docid_page)->pd_lower = BLCKSZ;

		/* Link old page to new page */
		old_copy	 = GenericXLogRegisterBuffer(xlog_state, docid_buf, 0);
		old_copy_hdr = (TpDocidPageHeader *)PageGetContents(old_copy);
		old_copy_hdr->next_page = BufferGetBlockNumber(new_buf);

		GenericXLogFinish(xlog_state);
		UnlockReleaseBuffer(docid_buf);

		/* Switch to new page */
		docid_buf	 = new_buf;
		docid_page	 = BufferGetPage(new_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);
		target_page	 = BufferGetBlockNumber(new_buf);
	}

	/* Add the docid to the current page */
	{
		GenericXLogState  *xlog_state;
		Page			   page_copy;
		TpDocidPageHeader *hdr_copy;
		ItemPointer		   docids_copy;

		xlog_state = GenericXLogStart(index);
		page_copy  = GenericXLogRegisterBuffer(xlog_state, docid_buf, 0);
		hdr_copy   = (TpDocidPageHeader *)PageGetContents(page_copy);

		docids_copy = (ItemPointer)((char *)hdr_copy +
									MAXALIGN(sizeof(TpDocidPageHeader)));

		docids_copy[hdr_copy->num_docids] = *ctid;
		hdr_copy->num_docids++;

		GenericXLogFinish(xlog_state);

		/* Update local pointer for cache update below */
		docid_header = (TpDocidPageHeader *)PageGetContents(
				BufferGetPage(docid_buf));
	}

	/* Update cache for next call */
	docid_writer_cache.index_oid  = index_oid;
	docid_writer_cache.last_page  = target_page;
	docid_writer_cache.num_docids = docid_header->num_docids;
	{
		TpLocalIndexState *ls = tp_get_local_index_state(index_oid);
		docid_writer_cache.generation =
				(ls && ls->shared)
						? pg_atomic_read_u64(&ls->shared->spill_generation)
						: 0;
	}
	docid_writer_cache.valid = true;

	UnlockReleaseBuffer(docid_buf);
}

/*
 * Clear all docid pages after successful flush to segment
 * This prevents stale docids from being used during recovery
 */
void
tp_clear_docid_pages(Relation index)
{
	Buffer			  metabuf;
	GenericXLogState *state;
	Page			  metapage;
	TpIndexMetaPage	  metap;

	/* Get the metapage to clear the docid page pointer */
	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	state	 = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	/*
	 * Simply clear the first_docid_page pointer. We don't need
	 * to physically delete the pages - they'll be reused or
	 * reclaimed by vacuum. This ensures recovery won't try to
	 * rebuild from stale docids.
	 */
	metap->first_docid_page = InvalidBlockNumber;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(metabuf);

	/* Invalidate this backend's docid writer cache */
	docid_writer_cache.valid = false;

	/*
	 * Increment the spill generation counter so other backends
	 * detect their cached docid page is stale. This is the
	 * broadcast invalidation — each backend checks this
	 * counter against its cached generation on the fast path.
	 */
	{
		TpLocalIndexState *ls = tp_get_local_index_state(
				RelationGetRelid(index));
		if (ls && ls->shared)
			pg_atomic_fetch_add_u64(&ls->shared->spill_generation, 1);
	}
}
