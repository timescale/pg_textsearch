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
	tp_alive_bitset_init_data(bitset->bits, num_docs);

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
 * Write the in-memory bitset back to segment pages using
 * GenericXLog, then update alive_count in the segment header.
 *
 * The header update is batched into the final bitset page's
 * GenericXLog transaction so that the bitset data and
 * alive_count are crash-atomic.  If the header page happens
 * to be the same physical page as the last bitset page, both
 * updates go into a single GenericXLog with one buffer.
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
	 * Write bitset data page by page.  On the last page,
	 * batch the header alive_count update into the same
	 * GenericXLog transaction for crash atomicity.
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
		bool			  is_last_page;
		Buffer			  header_buf = InvalidBuffer;

		logical_offset = bitset_offset + bytes_written;
		logical_page   = tp_logical_page(logical_offset);
		page_off	   = tp_page_offset(logical_offset);

		Assert(logical_page < reader->num_pages);
		physical_block = reader->page_map[logical_page];

		bytes_on_page = SEGMENT_DATA_PER_PAGE - page_off;
		if (bytes_on_page > nbytes - bytes_written)
			bytes_on_page = nbytes - bytes_written;

		is_last_page = (bytes_written + bytes_on_page >= nbytes);

		buf = ReadBuffer(index, physical_block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		xstate = GenericXLogStart(index);
		page   = GenericXLogRegisterBuffer(xstate, buf, 0);

		memcpy((char *)page + SizeOfPageHeaderData + page_off,
			   bitset->bits + bytes_written,
			   bytes_on_page);

		/*
		 * On the last bitset page, also update alive_count
		 * in the segment header within the same GenericXLog
		 * transaction for crash atomicity.
		 */
		if (is_last_page)
		{
			if (physical_block == reader->root_block)
			{
				/*
				 * Header is on the same page — update the
				 * already-registered buffer.
				 */
				TpSegmentHeader *hdr = (TpSegmentHeader *)PageGetContents(
						page);
				hdr->alive_count = bitset->alive_count;
			}
			else
			{
				Page header_page;

				header_buf = ReadBuffer(index, reader->root_block);
				LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
				header_page = GenericXLogRegisterBuffer(xstate, header_buf, 0);
				((TpSegmentHeader *)PageGetContents(header_page))
						->alive_count = bitset->alive_count;
			}
		}

		GenericXLogFinish(xstate);
		UnlockReleaseBuffer(buf);

		if (BufferIsValid(header_buf))
			UnlockReleaseBuffer(header_buf);

		bytes_written += bytes_on_page;
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
