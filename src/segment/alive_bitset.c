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

#include "debug/injection.h"
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
 * was decremented), false if it was already dead or out of range.
 */
bool
tp_alive_bitset_mark_dead(TpAliveBitset *bitset, uint32 doc_id)
{
	uint32 byte_idx;
	uint8  bit_mask;

	Assert(doc_id < bitset->num_docs);

	/*
	 * Defense in depth for non-assert builds: a doc_id past num_docs is
	 * stale -- e.g. a segment shrunk by a concurrent merge between VACUUM's
	 * identify and mark phases.  Callers serialize source replacement with
	 * the maintenance object lock, but the per-index lock is not held across
	 * bitmap updates.
	 */
	if (doc_id >= bitset->num_docs)
		return false;

	byte_idx = doc_id >> 3;
	bit_mask = (uint8)(1 << (doc_id & 7));

	if (!(bitset->bits[byte_idx] & bit_mask))
		return false; /* already dead */

	bitset->bits[byte_idx] &= ~bit_mask;
	bitset->alive_count--;
	return true;
}

/*
 * Write the in-memory bitset back to segment pages using GenericXLog.
 *
 * Each page's newly cleared bits and the corresponding alive_count decrement
 * must share one WAL record.  A multi-page write can stop between records;
 * the header must describe the bits already persisted at every such boundary.
 * dead_per_page counts successful mark_dead calls per logical bitmap page,
 * starting at the page containing alive_bitset_offset.  Consume each count
 * after its WAL record completes.  Callers serialize loading, marking, and
 * writing against other bitmap writers and segment replacement; on ERROR,
 * discard the in-memory bitset and counts before retrying.
 */
void
tp_alive_bitset_write(
		TpAliveBitset	*bitset,
		TpSegmentReader *reader,
		Relation		 index,
		uint32			*dead_per_page)
{
	uint64 bitset_offset;
	uint32 nbytes;
	uint32 bytes_written;
	uint32 page_index = 0;

	bitset_offset = reader->header->alive_bitset_offset;
	nbytes		  = tp_alive_bitset_size(bitset->num_docs);
	bytes_written = 0;

	while (bytes_written < nbytes)
	{
		uint64			  logical_offset;
		uint32			  logical_page;
		uint32			  page_off;
		uint32			  bytes_on_page;
		uint32			  newly_dead = dead_per_page[page_index];
		BlockNumber		  physical_block;
		Buffer			  buf;
		Buffer			  header_buf = InvalidBuffer;
		Page			  page;
		Page			  header_page;
		uint8			 *page_bits;
		TpSegmentHeader	 *hdr;
		GenericXLogState *xstate;

		logical_offset = bitset_offset + bytes_written;
		logical_page   = tp_logical_page(logical_offset);
		page_off	   = tp_page_offset(logical_offset);

		Assert(logical_page < reader->num_pages);
		physical_block = reader->page_map[logical_page];

		bytes_on_page = SEGMENT_DATA_PER_PAGE - page_off;
		if (bytes_on_page > nbytes - bytes_written)
			bytes_on_page = nbytes - bytes_written;

		buf = ReadBuffer(index, physical_block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		xstate	  = GenericXLogStart(index);
		page	  = GenericXLogRegisterBuffer(xstate, buf, 0);
		page_bits = (uint8 *)page + SizeOfPageHeaderData + page_off;

		memcpy(page_bits, bitset->bits + bytes_written, bytes_on_page);

		if (newly_dead > 0)
		{
			if (physical_block == reader->root_block)
				header_page = page;
			else
			{
				header_buf = ReadBuffer(index, reader->root_block);
				LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
				header_page = GenericXLogRegisterBuffer(xstate, header_buf, 0);
			}
			hdr = (TpSegmentHeader *)PageGetContents(header_page);
			Assert(hdr->alive_count >= newly_dead);
			hdr->alive_count -= newly_dead;
		}

		GenericXLogFinish(xstate);
		dead_per_page[page_index++] = 0;
		UnlockReleaseBuffer(buf);

		if (BufferIsValid(header_buf))
			UnlockReleaseBuffer(header_buf);

		bytes_written += bytes_on_page;
		TP_INJECTION_POINT(TP_INJECTION_ALIVE_BITSET_AFTER_PAGE);
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
