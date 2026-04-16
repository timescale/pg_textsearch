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

#include "segment_io.h"

/*
 * In-memory alive bitset, used during VACUUM to batch-update
 * the on-disk bitset.  Not used during scoring — scoring reads
 * bitset bytes directly from segment pages via paged I/O.
 */
typedef struct TpAliveBitset
{
	uint8 *bits;		/* Bitset data (1=alive, 0=dead) */
	uint32 num_docs;	/* Total doc count (bitset capacity) */
	uint32 alive_count; /* Number of alive docs */
} TpAliveBitset;

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
 * Create an in-memory bitset with all docs alive.
 * Used during VACUUM to load, modify, and write back.
 */
extern TpAliveBitset *tp_alive_bitset_create(uint32 num_docs);

/*
 * Load the alive bitset from a segment into memory.
 * Returns NULL if the segment has no bitset (pre-V5).
 */
extern TpAliveBitset *tp_alive_bitset_load(TpSegmentReader *reader);

/*
 * Mark a doc as dead.  Returns true if it was alive
 * (i.e., the alive_count was decremented).
 */
extern bool tp_alive_bitset_mark_dead(TpAliveBitset *bitset, uint32 doc_id);

/*
 * Write the in-memory bitset back to segment pages and
 * update the segment header's alive_count.  Uses
 * GenericXLog for crash safety.  The header update is
 * batched into the final bitset page's GenericXLog
 * transaction for atomicity.
 */
extern void tp_alive_bitset_write(
		TpAliveBitset *bitset, TpSegmentReader *reader, Relation index);

/*
 * Free an in-memory bitset.
 */
extern void tp_alive_bitset_free(TpAliveBitset *bitset);
