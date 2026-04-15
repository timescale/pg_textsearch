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

#include "alive_bitset.h"
#include "pagemapper.h"
#include "segment_io.h"

/*
 * Create an in-memory bitset with all docs alive.
 *
 * Allocates a TpAliveBitset with all bits set to 1, then clears
 * any trailing bits beyond num_docs in the final byte.
 */
TpAliveBitset *
tp_alive_bitset_create(uint32 num_docs)
{
	TpAliveBitset *bitset;
	uint32		   nbytes;

	bitset				= palloc(sizeof(TpAliveBitset));
	bitset->num_docs	= num_docs;
	bitset->alive_count = num_docs;

	nbytes		 = tp_alive_bitset_size(num_docs);
	bitset->bits = palloc(nbytes);
	memset(bitset->bits, 0xFF, nbytes);

	/* Clear trailing bits in the last byte beyond num_docs */
	if (num_docs % 8 != 0)
	{
		uint32 trailing_bits = num_docs % 8;
		uint8  mask			 = (1 << trailing_bits) - 1;

		bitset->bits[nbytes - 1] &= mask;
	}

	return bitset;
}

/*
 * Load the alive bitset from a segment into memory.
 *
 * Returns NULL if the segment has no alive bitset (pre-V5 segments
 * have alive_bitset_offset == 0).
 */
TpAliveBitset *
tp_alive_bitset_load(TpSegmentReader *reader)
{
	TpAliveBitset *bitset;
	uint32		   nbytes;

	if (reader->header->alive_bitset_offset == 0)
		return NULL;

	bitset				= palloc(sizeof(TpAliveBitset));
	bitset->num_docs	= reader->header->num_docs;
	bitset->alive_count = reader->header->alive_count;

	nbytes		 = tp_alive_bitset_size(bitset->num_docs);
	bitset->bits = palloc(nbytes);

	tp_segment_read(
			reader, reader->header->alive_bitset_offset, bitset->bits, nbytes);

	return bitset;
}

/*
 * Mark a document as dead in the in-memory bitset.
 *
 * Returns true if the document was previously alive (and alive_count
 * was decremented), false if it was already dead.
 */
bool
tp_alive_bitset_mark_dead(TpAliveBitset *bitset, uint32 doc_id)
{
	uint32 byte_idx;
	uint8  bit_mask;

	Assert(doc_id < bitset->num_docs);

	byte_idx = doc_id >> 3;
	bit_mask = (uint8)(1 << (doc_id & 7));

	if (!(bitset->bits[byte_idx] & bit_mask))
		return false; /* already dead */

	bitset->bits[byte_idx] &= ~bit_mask;
	bitset->alive_count--;
	return true;
}

/*
 * Write the in-memory bitset back to segment pages using GenericXLog,
 * then update alive_count in the segment header page.
 *
 * The bitset data lives in the segment's logical address space starting
 * at reader->header->alive_bitset_offset.  We iterate the bitset bytes,
 * mapping each range to its physical page, and copy using GenericXLog
 * for crash safety.  Finally, the segment header's alive_count is
 * updated in a separate GenericXLog record.
 */
void
tp_alive_bitset_write(
		TpAliveBitset *bitset, TpSegmentReader *reader, Relation index)
{
	uint64 bitset_offset;
	uint32 nbytes;
	uint32 bytes_written;

	bitset_offset = reader->header->alive_bitset_offset;
	nbytes		  = tp_alive_bitset_size(bitset->num_docs);
	bytes_written = 0;

	/*
	 * Write bitset data page by page.  Each iteration handles one
	 * physical page worth of bitset bytes (or less for the last page).
	 */
	while (bytes_written < nbytes)
	{
		uint64			  logical_offset;
		uint32			  logical_page;
		uint32			  page_off;
		uint32			  bytes_on_page;
		BlockNumber		  physical_block;
		Buffer			  buf;
		Page			  page;
		GenericXLogState *xstate;

		logical_offset = bitset_offset + bytes_written;
		logical_page   = tp_logical_page(logical_offset);
		page_off	   = tp_page_offset(logical_offset);

		Assert(logical_page < reader->num_pages);
		physical_block = reader->page_map[logical_page];

		/* How many bitset bytes fit on this page from page_off */
		bytes_on_page = SEGMENT_DATA_PER_PAGE - page_off;
		if (bytes_on_page > nbytes - bytes_written)
			bytes_on_page = nbytes - bytes_written;

		buf = ReadBuffer(index, physical_block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		xstate = GenericXLogStart(index);
		page   = GenericXLogRegisterBuffer(xstate, buf, 0);

		memcpy((char *)page + SizeOfPageHeaderData + page_off,
			   bitset->bits + bytes_written,
			   bytes_on_page);

		GenericXLogFinish(xstate);
		UnlockReleaseBuffer(buf);

		bytes_written += bytes_on_page;
	}

	/*
	 * Update alive_count in the segment header page.
	 */
	{
		Buffer			  header_buf;
		Page			  header_page;
		TpSegmentHeader	 *hdr;
		GenericXLogState *xstate;

		header_buf = ReadBuffer(index, reader->root_block);
		LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
		xstate		= GenericXLogStart(index);
		header_page = GenericXLogRegisterBuffer(xstate, header_buf, 0);
		hdr			= (TpSegmentHeader *)PageGetContents(header_page);

		hdr->alive_count = bitset->alive_count;

		GenericXLogFinish(xstate);
		UnlockReleaseBuffer(header_buf);
	}
}

/*
 * Free an in-memory alive bitset.
 */
void
tp_alive_bitset_free(TpAliveBitset *bitset)
{
	if (bitset == NULL)
		return;

	if (bitset->bits)
		pfree(bitset->bits);

	pfree(bitset);
}
