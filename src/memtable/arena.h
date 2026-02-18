/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * arena.h - Page-based arena allocator for index build memtables
 *
 * A simple bump allocator that allocates from 1MB pages. All memory
 * is freed at once when the arena is destroyed or reset, making it
 * ideal for batch operations like index builds where individual
 * deallocation is not needed.
 *
 * Addresses are 32-bit (ArenaAddr): upper 12 bits = page index,
 * lower 20 bits = offset within page. This gives a maximum capacity
 * of 4096 pages * 1MB = 4GB per arena.
 */
#pragma once

#include <postgres.h>

/*
 * Arena address type - compact 32-bit reference into arena memory.
 * Encodes page index (12 bits) and offset (20 bits).
 */
typedef uint32 ArenaAddr;

#define ARENA_ADDR_INVALID ((ArenaAddr)0xFFFFFFFF)

/* Page size: 1MB (2^20 bytes) */
#define ARENA_PAGE_BITS	  20
#define ARENA_PAGE_SIZE	  (1 << ARENA_PAGE_BITS)
#define ARENA_OFFSET_MASK ((1 << ARENA_PAGE_BITS) - 1)

/* Maximum pages per arena: 4096 (2^12) -> 4GB total */
#define ARENA_MAX_PAGES 4096

/*
 * Build an ArenaAddr from page index and offset.
 */
static inline ArenaAddr
arena_addr_make(uint32 page_idx, uint32 offset)
{
	Assert(page_idx < ARENA_MAX_PAGES);
	Assert(offset < ARENA_PAGE_SIZE);
	return (page_idx << ARENA_PAGE_BITS) | offset;
}

/*
 * Extract page index from an ArenaAddr.
 */
static inline uint32
arena_addr_page(ArenaAddr addr)
{
	return addr >> ARENA_PAGE_BITS;
}

/*
 * Extract offset from an ArenaAddr.
 */
static inline uint32
arena_addr_offset(ArenaAddr addr)
{
	return addr & ARENA_OFFSET_MASK;
}

/*
 * Arena allocator state.
 */
typedef struct TpArena
{
	char **pages;		   /* Array of 1MB page pointers */
	uint32 num_pages;	   /* Number of allocated pages */
	uint32 max_pages;	   /* Capacity of pages array */
	uint32 current_offset; /* Write position in current page */
	Size   total_bytes;	   /* Total bytes allocated (for budget) */
} TpArena;

/*
 * Create a new arena.
 */
extern TpArena *tp_arena_create(void);

/*
 * Allocate `size` bytes from the arena.
 * Returns an ArenaAddr referencing the allocated memory.
 * Allocations are rounded up to 4-byte alignment.
 */
extern ArenaAddr tp_arena_alloc(TpArena *arena, Size size);

/*
 * Resolve an ArenaAddr to a raw pointer.
 * The pointer is valid until the arena is destroyed or reset.
 */
static inline void *
tp_arena_get_ptr(TpArena *arena, ArenaAddr addr)
{
	Assert(addr != ARENA_ADDR_INVALID);
	Assert(arena_addr_page(addr) < arena->num_pages);
	return arena->pages[arena_addr_page(addr)] + arena_addr_offset(addr);
}

/*
 * Return total bytes allocated from this arena.
 * Used for budget-based flushing decisions.
 */
static inline Size
tp_arena_mem_usage(TpArena *arena)
{
	return arena->total_bytes;
}

/*
 * Reset the arena, releasing all pages back to palloc.
 * The arena structure itself remains valid for reuse.
 */
extern void tp_arena_reset(TpArena *arena);

/*
 * Destroy the arena, freeing all memory including the
 * arena structure itself.
 */
extern void tp_arena_destroy(TpArena *arena);
