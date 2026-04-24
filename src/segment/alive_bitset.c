/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * alive_bitset.c - Per-segment alive bitset implementation
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "segment/alive_bitset.h"
#include "segment/io.h"
#include "segment/pagemapper.h"

/*
 * Initialize a raw bitset buffer to all-alive for num_docs.
 * Sets all bits to 1, then clears trailing bits in the last byte.
 */
void
tp_alive_bitset_init_data(uint8 *buf, uint32 num_docs)
{
	uint32 nbytes = tp_alive_bitset_size(num_docs);

	memset(buf, 0xFF, nbytes);

	if (num_docs % 8 != 0)
	{
		uint8 mask = (1 << (num_docs % 8)) - 1;

		buf[nbytes - 1] &= mask;
	}
}

/*
 * Comparator for qsort of uint32 doc_ids.
 */
static int
cmp_uint32(const void *a, const void *b)
{
	uint32 va = *(const uint32 *)a;
	uint32 vb = *(const uint32 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

/*
 * Mark dead docs directly on segment pages in-place.
 *
 * Instead of loading the entire bitset into memory, this function
 * sorts dead_doc_ids by their bitset page, then for each affected
 * page: ReadBuffer, GenericXLog, flip only the relevant bits,
 * and finish.  Only pages containing dead docs are touched.
 *
 * The header alive_count update is batched into the last dirty
 * page's GenericXLog transaction for crash atomicity.  If the
 * header page is the same as the last bitset page, both updates
 * go into a single GenericXLog with one buffer.
 *
 * Returns the new alive_count, or 0 if all docs are now dead.
 */
uint32
tp_alive_bitset_mark_dead_inplace(
		Relation index,
		TpSegmentReader *reader,
		uint32 *dead_doc_ids,
		uint32 dead_count)
{
	uint64 bitset_offset;
	uint32 alive_count;
	uint32 cur_lpage;
	Buffer cur_buf;
	GenericXLogState *xstate;
	Page   page;
	bool   page_dirty;

	if (dead_count == 0)
		return reader->header->alive_count;

	if (reader->header->alive_bitset_offset == 0)
		return 0;

	bitset_offset = reader->header->alive_bitset_offset;
	alive_count   = reader->header->alive_count;

	/*
	 * Sort dead_doc_ids so we process all docs on the same
	 * bitset page together, minimizing ReadBuffer calls.
	 */
	qsort(dead_doc_ids, dead_count, sizeof(uint32), cmp_uint32);

	cur_lpage  = UINT32_MAX;
	cur_buf    = InvalidBuffer;
	xstate     = NULL;
	page       = NULL;
	page_dirty = false;

	for (uint32 i = 0; i < dead_count; i++)
	{
		uint32 doc_id = dead_doc_ids[i];
		uint64 byte_off;
		uint32 lpage;
		uint32 poff;
		uint8  bit_mask;
		uint8 *byte_ptr;

		/*
		 * Compute which logical page and byte offset
		 * within that page this doc_id's bit lives on.
		 */
		byte_off = bitset_offset + (doc_id >> 3);
		lpage    = tp_logical_page(byte_off);
		poff     = tp_page_offset(byte_off);
		bit_mask = (uint8)(1 << (doc_id & 7));

		/* New page? Finish previous, open this one. */
		if (lpage != cur_lpage)
		{
			/* Finish previous page's GenericXLog */
			if (xstate != NULL)
			{
				if (page_dirty)
					GenericXLogFinish(xstate);
				else
					GenericXLogAbort(xstate);
				UnlockReleaseBuffer(cur_buf);
			}

			/* Open the new page */
			{
				BlockNumber phys;

				Assert(lpage < reader->num_pages);
				phys = reader->page_map[lpage];

				cur_buf = ReadBuffer(index, phys);
				LockBuffer(cur_buf, BUFFER_LOCK_EXCLUSIVE);
				xstate = GenericXLogStart(index);
				page   = GenericXLogRegisterBuffer(
						xstate, cur_buf, 0);
			}

			cur_lpage  = lpage;
			page_dirty = false;
		}

		/* Flip the bit in-place (clear = mark dead) */
		byte_ptr = (uint8 *)page + SizeOfPageHeaderData + poff;
		if (*byte_ptr & bit_mask)
		{
			*byte_ptr &= ~bit_mask;
			alive_count--;
			page_dirty = true;
		}
	}

	/*
	 * Update alive_count in the segment header.  Batch
	 * into the last page's GenericXLog if possible.
	 */
	if (xstate != NULL)
	{
		BlockNumber last_phys;

		Assert(cur_lpage < reader->num_pages);
		last_phys = reader->page_map[cur_lpage];

		if (last_phys == reader->root_block)
		{
			/*
			 * Header is on the same page as the last
			 * bitset modification — update in-place.
			 */
			TpSegmentHeader *hdr =
					(TpSegmentHeader *)PageGetContents(
							page);

			hdr->alive_count = alive_count;
			page_dirty = true;
		}
		else
		{
			/*
			 * Header is on a different page.  Register
			 * it in the same GenericXLog transaction
			 * for crash atomicity.
			 */
			Buffer header_buf;
			Page   header_page;

			header_buf = ReadBuffer(
					index, reader->root_block);
			LockBuffer(
					header_buf, BUFFER_LOCK_EXCLUSIVE);
			header_page = GenericXLogRegisterBuffer(
					xstate, header_buf, 0);
			((TpSegmentHeader *)PageGetContents(
					 header_page))
					->alive_count = alive_count;

			/*
			 * Always finish here since we modified the
			 * header, then release both buffers.
			 */
			GenericXLogFinish(xstate);
			UnlockReleaseBuffer(cur_buf);
			UnlockReleaseBuffer(header_buf);
			xstate = NULL;  /* already finished */
		}

		/* Finish if not already done above. */
		if (xstate != NULL)
		{
			if (page_dirty)
				GenericXLogFinish(xstate);
			else
				GenericXLogAbort(xstate);
			UnlockReleaseBuffer(cur_buf);
		}
	}

	return alive_count;
}
