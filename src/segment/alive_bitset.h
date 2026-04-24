/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * alive_bitset.h - Per-segment alive bitset for tombstone tracking
 *
 * Each segment contains a flat bitset with one bit per document.
 * Bit 1 = alive, bit 0 = dead. Pre-allocated at segment creation
 * with all bits set. VACUUM clears bits for dead documents.
 */
#pragma once

#include <postgres.h>

#include <storage/bufmgr.h>
#include <utils/rel.h>

#include "segment/io.h"

/*
 * Byte size of bitset data for a given doc count.
 */
static inline uint32
tp_alive_bitset_size(uint32 num_docs)
{
	return (num_docs + 7) / 8;
}

/*
 * Check if a doc is alive by reading from segment pages.
 * Used in the hot scoring path — reads one byte via paged
 * I/O.  Returns true if the segment has no bitset (pre-V5)
 * or if all docs are alive, or if the doc's bit is set.
 */
static inline bool
tp_segment_is_alive(TpSegmentReader *reader, uint32 doc_id)
{
	uint64 byte_offset;
	uint8  byte;

	if (reader->header->alive_bitset_offset == 0 ||
		reader->header->alive_count == reader->header->num_docs)
		return true;

	byte_offset = reader->header->alive_bitset_offset + (doc_id >> 3);
	tp_segment_read(reader, byte_offset, &byte, 1);
	return (byte >> (doc_id & 7)) & 1;
}

/*
 * Initialize a raw bitset buffer to all-alive for num_docs.
 * Sets all bits to 1, then clears trailing bits beyond num_docs.
 * Used by segment writers to avoid duplicating the mask logic.
 */
extern void tp_alive_bitset_init_data(uint8 *buf, uint32 num_docs);

/*
 * Mark dead docs directly on segment pages in-place using
 * GenericXLog.  Only touches pages containing dead docs.
 * Updates alive_count in the segment header atomically with
 * the final bitset page modification.
 *
 * dead_doc_ids need not be sorted; the function sorts them
 * internally to group page accesses.
 *
 * Returns the new alive_count, or 0 if all docs are now dead
 * (caller should drop the segment).
 */
extern uint32 tp_alive_bitset_mark_dead_inplace(
		Relation index,
		TpSegmentReader *reader,
		uint32 *dead_doc_ids,
		uint32 dead_count);
