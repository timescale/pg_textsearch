/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * expull.h - Exponential unrolled linked list for posting lists
 *
 * A memory-efficient append-only data structure for accumulating
 * posting list entries during index builds. Blocks grow
 * exponentially (32B, 64B, ..., 32KB) to minimize waste for rare
 * terms while providing amortized O(1) appends for frequent terms.
 *
 * All memory is allocated from a TpArena, so individual frees are
 * not needed — the arena handles bulk deallocation.
 *
 * Inspired by Tantivy's ExpUnrolledLinkedList (stacker/src/expull.rs).
 */
#pragma once

#include <postgres.h>

#include "arena.h"

/*
 * Single posting entry stored in EXPULL blocks.
 * 7 bytes of data; packed to avoid waste in blocks.
 */
typedef struct TpExpullEntry
{
	uint32 doc_id;	  /* Segment-local document ID */
	uint16 frequency; /* Term frequency in document */
	uint8  fieldnorm; /* Quantized document length */
} __attribute__((packed)) TpExpullEntry;

#define TP_EXPULL_ENTRY_SIZE sizeof(TpExpullEntry) /* 7 bytes */

/*
 * Block layout in arena memory:
 *   [ArenaAddr next]  -- 4 bytes: link to next block
 *   [TpExpullEntry entries...]  -- remaining bytes: posting data
 *
 * Usable capacity per block = block_size - sizeof(ArenaAddr)
 */
#define TP_EXPULL_LINK_SIZE sizeof(ArenaAddr)

/*
 * Block sizing: first block is 32 bytes (FIRST_BLOCK = 5, 2^5),
 * each subsequent block doubles up to 32KB (MAX_BLOCK = 15, 2^15).
 */
#define TP_EXPULL_FIRST_BLOCK_LOG2 5  /* 32 bytes */
#define TP_EXPULL_MAX_BLOCK_LOG2   15 /* 32768 bytes */

static inline uint32
tp_expull_block_size(uint32 block_num)
{
	uint32 log2 = TP_EXPULL_FIRST_BLOCK_LOG2 + block_num;

	if (log2 > TP_EXPULL_MAX_BLOCK_LOG2)
		log2 = TP_EXPULL_MAX_BLOCK_LOG2;
	return 1u << log2;
}

/*
 * EXPULL header — stored in the hash table entry, not in the arena.
 * Tracks the linked list state for one term's posting list.
 */
typedef struct TpExpull
{
	ArenaAddr head;			 /* First block in the chain */
	ArenaAddr last_block;	 /* Start of the current (last) block */
	ArenaAddr tail;			 /* Current write position (byte addr) */
	uint16	  remaining_cap; /* Bytes remaining in current block */
	uint16	  block_num;	 /* Number of blocks allocated (for sizing) */
	uint32	  num_entries;	 /* Total posting entries stored */
} TpExpull;

/*
 * Initialize a new EXPULL (does not allocate any blocks yet).
 */
static inline void
tp_expull_init(TpExpull *expull)
{
	expull->head		  = ARENA_ADDR_INVALID;
	expull->last_block	  = ARENA_ADDR_INVALID;
	expull->tail		  = ARENA_ADDR_INVALID;
	expull->remaining_cap = 0;
	expull->block_num	  = 0;
	expull->num_entries	  = 0;
}

/*
 * Append a posting entry to the EXPULL list.
 * Allocates a new block from the arena if needed.
 */
extern void tp_expull_append(
		TpArena	 *arena,
		TpExpull *expull,
		uint32	  doc_id,
		uint16	  frequency,
		uint8	  fieldnorm);

/*
 * Reader state for iterating over EXPULL entries.
 */
typedef struct TpExpullReader
{
	TpArena	 *arena;
	ArenaAddr current_block;	 /* Current block being read */
	uint32	  block_offset;		 /* Byte offset within current block */
	uint32	  block_data_size;	 /* Usable bytes in current block */
	uint32	  block_num;		 /* Block index (for sizing next block) */
	uint32	  entries_remaining; /* Total entries left to read */
} TpExpullReader;

/*
 * Initialize a reader for iterating over an EXPULL list.
 */
extern void tp_expull_reader_init(
		TpExpullReader *reader, TpArena *arena, TpExpull *expull);

/*
 * Read up to `max_entries` postings into `out` array.
 * Returns the number of entries actually read (0 when exhausted).
 *
 * Output is written as TpExpullEntry structs. The caller can
 * convert to TpBlockPosting if needed for the segment writer.
 */
extern uint32 tp_expull_reader_read(
		TpExpullReader *reader, TpExpullEntry *out, uint32 max_entries);
