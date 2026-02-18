/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * expull.c - Exponential unrolled linked list for posting lists
 *
 * See expull.h for design overview.
 */
#include <postgres.h>

#include "arena.h"
#include "expull.h"

/*
 * Allocate a new block and link it into the EXPULL chain.
 */
static void
expull_alloc_block(TpArena *arena, TpExpull *expull)
{
	uint32	   block_size;
	uint32	   data_cap;
	ArenaAddr  block_addr;
	ArenaAddr *next_ptr;

	block_size = tp_expull_block_size(expull->block_num);
	data_cap   = block_size - TP_EXPULL_LINK_SIZE;

	block_addr = tp_arena_alloc(arena, block_size);

	/* Initialize the next-pointer at the start of the new block */
	next_ptr  = (ArenaAddr *)tp_arena_get_ptr(arena, block_addr);
	*next_ptr = ARENA_ADDR_INVALID;

	/* Link previous block to this new block */
	if (expull->last_block != ARENA_ADDR_INVALID)
	{
		ArenaAddr *prev_next = (ArenaAddr *)
				tp_arena_get_ptr(arena, expull->last_block);
		*prev_next = block_addr;
	}
	else
	{
		/* First block ever */
		expull->head = block_addr;
	}

	/* Update state to point to the new block */
	expull->last_block = block_addr;
	{
		uint32 page = arena_addr_page(block_addr);
		uint32 off	= arena_addr_offset(block_addr);

		expull->tail = arena_addr_make(page, off + TP_EXPULL_LINK_SIZE);
	}
	expull->remaining_cap = data_cap;
	expull->block_num++;
}

/*
 * Append a posting entry to the EXPULL list.
 */
void
tp_expull_append(
		TpArena	 *arena,
		TpExpull *expull,
		uint32	  doc_id,
		uint16	  frequency,
		uint8	  fieldnorm)
{
	TpExpullEntry *entry_ptr;

	Assert(arena != NULL);
	Assert(expull != NULL);

	/* Allocate a new block if needed */
	if (expull->remaining_cap < TP_EXPULL_ENTRY_SIZE)
		expull_alloc_block(arena, expull);

	Assert(expull->remaining_cap >= TP_EXPULL_ENTRY_SIZE);

	/* Write the entry at the tail position */
	entry_ptr		  = (TpExpullEntry *)tp_arena_get_ptr(arena, expull->tail);
	entry_ptr->doc_id = doc_id;
	entry_ptr->frequency = frequency;
	entry_ptr->fieldnorm = fieldnorm;

	/* Advance tail */
	{
		uint32 page = arena_addr_page(expull->tail);
		uint32 off	= arena_addr_offset(expull->tail);

		expull->tail = arena_addr_make(page, off + TP_EXPULL_ENTRY_SIZE);
	}
	expull->remaining_cap -= TP_EXPULL_ENTRY_SIZE;
	expull->num_entries++;
}

/*
 * Initialize a reader for iterating over an EXPULL list.
 */
void
tp_expull_reader_init(TpExpullReader *reader, TpArena *arena, TpExpull *expull)
{
	uint32 first_block_size;
	uint32 first_data_cap;

	Assert(reader != NULL);
	Assert(arena != NULL);
	Assert(expull != NULL);

	reader->arena			  = arena;
	reader->entries_remaining = expull->num_entries;
	reader->block_num		  = 0;

	if (expull->num_entries == 0 || expull->head == ARENA_ADDR_INVALID)
	{
		reader->current_block	= ARENA_ADDR_INVALID;
		reader->block_offset	= 0;
		reader->block_data_size = 0;
		return;
	}

	reader->current_block = expull->head;
	reader->block_offset  = 0;

	/* First block data capacity */
	first_block_size		= tp_expull_block_size(0);
	first_data_cap			= first_block_size - TP_EXPULL_LINK_SIZE;
	reader->block_data_size = first_data_cap;
}

/*
 * Read up to max_entries postings from the EXPULL list.
 * Returns number of entries read (0 when exhausted).
 */
uint32
tp_expull_reader_read(
		TpExpullReader *reader, TpExpullEntry *out, uint32 max_entries)
{
	uint32 count = 0;

	Assert(reader != NULL);
	Assert(out != NULL);

	while (count < max_entries && reader->entries_remaining > 0)
	{
		uint32 avail_bytes;
		uint32 avail_entries;
		uint32 to_read;
		char  *block_base;
		char  *data_start;

		if (reader->current_block == ARENA_ADDR_INVALID)
			break;

		/* Bytes available in current block from current offset */
		avail_bytes	  = reader->block_data_size - reader->block_offset;
		avail_entries = avail_bytes / TP_EXPULL_ENTRY_SIZE;

		if (avail_entries == 0)
		{
			/*
			 * Current block exhausted. Follow the next pointer.
			 */
			ArenaAddr *next_ptr;
			ArenaAddr  next_block;

			block_base = (char *)
					tp_arena_get_ptr(reader->arena, reader->current_block);
			next_ptr   = (ArenaAddr *)block_base;
			next_block = *next_ptr;

			if (next_block == ARENA_ADDR_INVALID)
			{
				reader->current_block = ARENA_ADDR_INVALID;
				break;
			}

			reader->block_num++;
			reader->current_block	= next_block;
			reader->block_offset	= 0;
			reader->block_data_size = tp_expull_block_size(reader->block_num) -
									  TP_EXPULL_LINK_SIZE;
			continue;
		}

		/* Read entries from current block */
		to_read = Min(avail_entries, max_entries - count);
		to_read = Min(to_read, reader->entries_remaining);

		block_base = (char *)
				tp_arena_get_ptr(reader->arena, reader->current_block);
		data_start = block_base + TP_EXPULL_LINK_SIZE + reader->block_offset;

		memcpy(&out[count], data_start, to_read * TP_EXPULL_ENTRY_SIZE);

		count += to_read;
		reader->block_offset += to_read * TP_EXPULL_ENTRY_SIZE;
		reader->entries_remaining -= to_read;
	}

	return count;
}
