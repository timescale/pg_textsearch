/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * arena.c - Page-based arena allocator for index build memtables
 *
 * Simple bump allocator using 1MB pages. All allocations are
 * sequential within a page; when the current page is full, a new
 * page is allocated. Individual deallocation is not supported —
 * the entire arena is freed at once via tp_arena_reset() or
 * tp_arena_destroy().
 *
 * This design is inspired by Tantivy's MemoryArena (stacker crate)
 * and provides O(1) allocation with zero fragmentation.
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "arena.h"

/* Initial capacity for the pages array */
#define ARENA_INITIAL_PAGES 16

/*
 * Allocate a new 1MB page and add it to the arena.
 */
static void
arena_add_page(TpArena *arena)
{
	if (arena->num_pages >= ARENA_MAX_PAGES)
		elog(ERROR,
			 "arena: exceeded maximum capacity (%d pages, %zu MB)",
			 ARENA_MAX_PAGES,
			 (Size)ARENA_MAX_PAGES);

	/* Grow the pages array if needed */
	if (arena->num_pages >= arena->max_pages)
	{
		uint32 new_max = arena->max_pages * 2;

		if (new_max > ARENA_MAX_PAGES)
			new_max = ARENA_MAX_PAGES;
		arena->pages	 = repalloc(arena->pages, new_max * sizeof(char *));
		arena->max_pages = new_max;
	}

	arena->pages[arena->num_pages] = palloc(ARENA_PAGE_SIZE);
	arena->num_pages++;
	arena->current_offset = 0;
}

/*
 * Create a new arena.
 */
TpArena *
tp_arena_create(void)
{
	TpArena *arena = palloc(sizeof(TpArena));

	arena->pages		  = palloc(ARENA_INITIAL_PAGES * sizeof(char *));
	arena->num_pages	  = 0;
	arena->max_pages	  = ARENA_INITIAL_PAGES;
	arena->current_offset = 0;
	arena->total_bytes	  = 0;

	/* Allocate the first page */
	arena_add_page(arena);

	return arena;
}

/*
 * Allocate `size` bytes from the arena.
 *
 * If the current page doesn't have enough space, a new page is
 * allocated. Allocations larger than ARENA_PAGE_SIZE are not
 * supported (callers should break large data into smaller chunks).
 */
ArenaAddr
tp_arena_alloc(TpArena *arena, Size size)
{
	ArenaAddr addr;
	uint32	  page_idx;
	Size	  aligned_size;

	Assert(arena != NULL);
	Assert(size > 0);

	/* Round up to 4-byte alignment so uint32 stores are safe */
	aligned_size = TYPEALIGN(4, size);

	if (aligned_size > ARENA_PAGE_SIZE)
		elog(ERROR,
			 "arena: allocation of %zu bytes exceeds page size "
			 "(%d bytes)",
			 size,
			 ARENA_PAGE_SIZE);

	/* Check if current page has enough space */
	if (arena->current_offset + aligned_size > ARENA_PAGE_SIZE)
	{
		/* Current page is full, allocate a new one */
		arena_add_page(arena);
	}

	page_idx = arena->num_pages - 1;
	addr	 = arena_addr_make(page_idx, arena->current_offset);

	arena->current_offset += aligned_size;
	arena->total_bytes += aligned_size;

	return addr;
}

/*
 * Reset the arena, freeing all pages except the first.
 * The arena can be reused after reset.
 */
void
tp_arena_reset(TpArena *arena)
{
	uint32 i;

	Assert(arena != NULL);

	/* Free all pages except the first */
	for (i = 1; i < arena->num_pages; i++)
		pfree(arena->pages[i]);

	arena->num_pages	  = 1;
	arena->current_offset = 0;
	arena->total_bytes	  = 0;
}

/*
 * Destroy the arena, freeing all memory.
 */
void
tp_arena_destroy(TpArena *arena)
{
	uint32 i;

	if (arena == NULL)
		return;

	for (i = 0; i < arena->num_pages; i++)
		pfree(arena->pages[i]);

	pfree(arena->pages);
	pfree(arena);
}
