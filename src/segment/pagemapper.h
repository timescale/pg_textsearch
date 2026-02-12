/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * pagemapper.h - Segment logical-to-physical address translation
 *
 * Segments store data as a logical contiguous byte stream, but physically
 * the data is spread across multiple Postgres pages. This module provides
 * the translation between logical offsets and physical page locations.
 *
 * Logical address space:
 *   [0 .. data_size) - contiguous byte offsets
 *
 * Physical storage:
 *   Array of BlockNumbers (page_map), each page stores SEGMENT_DATA_PER_PAGE
 *   bytes of logical data (page header is reserved for Postgres).
 */
#pragma once

#include "postgres.h"
#include "storage/bufpage.h"

/*
 * Usable data bytes per segment data page.
 * We use the standard page header; special area is not used for data pages.
 */
#define SEGMENT_DATA_PER_PAGE (BLCKSZ - SizeOfPageHeaderData)

/*
 * Convert a logical byte offset to logical page number.
 */
static inline uint32
tp_logical_page(uint64 logical_offset)
{
	return (uint32)(logical_offset / SEGMENT_DATA_PER_PAGE);
}

/*
 * Convert a logical byte offset to offset within the logical page.
 */
static inline uint32
tp_page_offset(uint64 logical_offset)
{
	return (uint32)(logical_offset % SEGMENT_DATA_PER_PAGE);
}

/*
 * Calculate bytes remaining on current page from given offset.
 */
static inline uint32
tp_bytes_remaining_on_page(uint64 logical_offset)
{
	return SEGMENT_DATA_PER_PAGE - tp_page_offset(logical_offset);
}

/*
 * Check if a read of 'len' bytes starting at 'logical_offset' fits
 * within a single page. Useful for zero-copy access patterns.
 */
static inline bool
tp_fits_on_page(uint64 logical_offset, uint32 len)
{
	return tp_page_offset(logical_offset) + len <= SEGMENT_DATA_PER_PAGE;
}
