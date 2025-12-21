/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment.c - Disk-based segment implementation
 */
#include <stdio.h>
#include <unistd.h>

#include "../dump.h"
#include "../memtable/memtable.h"
#include "../memtable/posting.h"
#include "../memtable/stringtable.h"
#include "../metapage.h"
#include "../state.h"
#include "access/genam.h"
#include "access/hash.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/storage.h"
#include "dictionary.h"
#include "docmap.h"
#include "fieldnorm.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "postgres.h"
#include "segment.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/indexfsm.h"
#include "storage/lock.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*
 * Note: We previously had a global page map cache here, but it was removed
 * due to race conditions when multiple backends accessed it concurrently.
 * Since segments are small and page maps are not frequently re-read in the
 * same backend, the performance impact of removing the cache is minimal.
 */

/*
 * Usable data bytes per page, accounting for page header overhead.
 * This must be consistent between reader and writer.
 */
#define SEGMENT_DATA_PER_PAGE (BLCKSZ - SizeOfPageHeaderData)

/*
 * Helper function to read a term string at a given dictionary index.
 * Returns the allocated string which must be freed by caller.
 */
static char *
read_term_at_index(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			 index,
		uint32			*string_offsets)
{
	TpStringEntry string_entry;
	char		 *term_text;
	uint32		  string_offset;

	/* Check for overflow when calculating string offset */
	if (string_offsets[index] > UINT32_MAX - header->strings_offset)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("string offset overflow detected"),
				 errdetail(
						 "String offset %u + base %u would overflow",
						 string_offsets[index],
						 header->strings_offset)));

	string_offset = header->strings_offset + string_offsets[index];

	/* Read string length */
	tp_segment_read(
			reader, string_offset, &string_entry.length, sizeof(uint32));

	/* Check for overflow when adding sizeof(uint32) */
	if (string_offset > UINT32_MAX - sizeof(uint32))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("string data offset overflow detected"),
				 errdetail(
						 "String offset %u + sizeof(uint32) would overflow",
						 string_offset)));

	/* Allocate buffer and read term text */
	term_text = palloc(string_entry.length + 1);
	tp_segment_read(
			reader,
			string_offset + sizeof(uint32),
			term_text,
			string_entry.length);
	term_text[string_entry.length] = '\0';

	return term_text;
}

/*
 * Helper function to read a dictionary entry at a given index
 */
static void
read_dict_entry(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			 index,
		TpDictEntry		*entry)
{
	uint32 offset_increment;
	uint32 entry_offset;

	/* Check for multiplication overflow */
	if (index > UINT32_MAX / sizeof(TpDictEntry))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dictionary entry index overflow"),
				 errdetail(
						 "Index %u * sizeof(TpDictEntry) would overflow",
						 index)));

	offset_increment = index * sizeof(TpDictEntry);

	/* Check for addition overflow */
	if (offset_increment > UINT32_MAX - header->entries_offset)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dictionary entry offset overflow"),
				 errdetail(
						 "Entry offset %u + increment %u would overflow",
						 header->entries_offset,
						 offset_increment)));

	entry_offset = header->entries_offset + offset_increment;

	tp_segment_read(reader, entry_offset, entry, sizeof(TpDictEntry));
}

/*
 * Helper function to read a V2 dictionary entry at a given index
 */
static void
read_dict_entry_v2(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			 index,
		TpDictEntryV2	*entry)
{
	uint32 offset_increment;
	uint32 entry_offset;

	if (index > UINT32_MAX / sizeof(TpDictEntryV2))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dictionary entry index overflow")));

	offset_increment = index * sizeof(TpDictEntryV2);

	if (offset_increment > UINT32_MAX - header->entries_offset)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("dictionary entry offset overflow")));

	entry_offset = header->entries_offset + offset_increment;
	tp_segment_read(reader, entry_offset, entry, sizeof(TpDictEntryV2));
}

/*
 * Open segment for reading
 */
TpSegmentReader *
tp_segment_open(Relation index, BlockNumber root_block)
{
	TpSegmentReader	   *reader;
	Buffer				header_buf;
	Page				header_page;
	TpSegmentHeader	   *header;
	BlockNumber			page_index_block;
	Buffer				index_buf;
	Page				index_page;
	TpPageIndexSpecial *special;
	BlockNumber		   *page_entries;
	uint32				pages_loaded = 0;
	uint32				i;
	BlockNumber			nblocks;

	/*
	 * Validate root_block is within the relation. In Postgres, blocks are
	 * allocated sequentially from 0 to nblocks-1, so any valid block number
	 * must be < nblocks. This is the standard way to validate block numbers.
	 */
	nblocks = RelationGetNumberOfBlocks(index);
	if (root_block >= nblocks)
		return NULL;

	/* Allocate reader structure */
	reader						 = palloc0(sizeof(TpSegmentReader));
	reader->index				 = index;
	reader->root_block			 = root_block;
	reader->current_buffer		 = InvalidBuffer;
	reader->current_logical_page = UINT32_MAX;

	/* Read header from root block */
	header_buf = ReadBuffer(index, root_block);
	LockBuffer(header_buf, BUFFER_LOCK_SHARE);
	header_page = BufferGetPage(header_buf);

	/* Copy header to reader structure */
	reader->header = palloc(sizeof(TpSegmentHeader));
	memcpy(reader->header,
		   (char *)header_page + SizeOfPageHeaderData,
		   sizeof(TpSegmentHeader));

	header = reader->header;

	/* Validate header magic number */
	if (header->magic != TP_SEGMENT_MAGIC)
	{
		UnlockReleaseBuffer(header_buf);
		pfree(reader->header);
		pfree(reader);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid segment header at block %u", root_block),
				 errdetail(
						 "magic=0x%08X, expected 0x%08X",
						 header->magic,
						 TP_SEGMENT_MAGIC)));
	}

	reader->num_pages = header->num_pages;
	reader->nblocks	  = nblocks;

	/* Get page index location from header */
	page_index_block = header->page_index;

	/* Keep header buffer for later use */
	reader->header_buffer = header_buf;
	LockBuffer(
			header_buf, BUFFER_LOCK_UNLOCK); /* Just unlock, don't release */

	/* Always load page map from disk - no caching due to concurrency issues */
	reader->page_map = palloc(sizeof(BlockNumber) * reader->num_pages);

	/* Read page index chain to build page map */
	while (page_index_block != InvalidBlockNumber &&
		   pages_loaded < reader->num_pages)
	{
		index_buf = ReadBuffer(index, page_index_block);
		LockBuffer(index_buf, BUFFER_LOCK_SHARE);
		index_page = BufferGetPage(index_buf);

		/* Get special area with page index metadata */
		special = (TpPageIndexSpecial *)PageGetSpecialPointer(index_page);

		/* Validate magic number and page type */
		if (special->magic != TP_PAGE_INDEX_MAGIC ||
			special->page_type != TP_PAGE_FILE_INDEX)
		{
			UnlockReleaseBuffer(index_buf);
			ReleaseBuffer(reader->header_buffer);
			pfree(reader->page_map);
			pfree(reader->header);
			pfree(reader);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid page index at block %u",
							page_index_block),
					 errdetail(
							 "magic=0x%08X (expected 0x%08X), "
							 "page_type=%u (expected %u)",
							 special->magic,
							 TP_PAGE_INDEX_MAGIC,
							 special->page_type,
							 TP_PAGE_FILE_INDEX)));
		}

		/* Get pointer to page entries array */
		page_entries = (BlockNumber *)((char *)index_page +
									   SizeOfPageHeaderData);

		/* Copy page entries to our map with validation */
		for (i = 0;
			 i < special->num_entries && pages_loaded < reader->num_pages;
			 i++)
		{
			BlockNumber page_block = page_entries[i];

			/* Validate block number is within relation bounds */
			if (page_block >= nblocks)
			{
				UnlockReleaseBuffer(index_buf);
				ReleaseBuffer(reader->header_buffer);
				pfree(reader->page_map);
				pfree(reader->header);
				pfree(reader);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid page block in segment page_map"),
						 errdetail(
								 "block %u at entry %u >= nblocks %u",
								 page_block,
								 pages_loaded,
								 nblocks)));
			}
			reader->page_map[pages_loaded++] = page_block;
		}

		/* Move to next page in chain */
		page_index_block = special->next_page;

		UnlockReleaseBuffer(index_buf);
	}

	if (pages_loaded != reader->num_pages)
	{
		/* Free allocated memory before erroring out */
		if (reader->page_map)
			pfree(reader->page_map);
		pfree(reader);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("segment page index is incomplete"),
				 errdetail(
						 "Expected %u pages but only loaded %u pages",
						 reader->num_pages,
						 pages_loaded),
				 errhint("The index may be corrupted and should be rebuilt")));
	}

	/*
	 * For V2 segments, preload CTID table into memory for result lookup.
	 * Fieldnorms are now stored inline in TpBlockPosting, so no fieldnorm
	 * cache is needed.
	 *
	 * For large segments (>100K docs), caching is counterproductive because:
	 * - Loading 600KB+ of data upfront is expensive
	 * - Top-k queries only access a small fraction of documents
	 * - PostgreSQL's buffer cache handles per-doc reads efficiently
	 */
	reader->cached_ctids	= NULL;
	reader->cached_num_docs = 0;

#define TP_SEGMENT_CACHE_THRESHOLD 100000 /* Max docs to cache */

	if (header->version >= TP_SEGMENT_FORMAT_V2 && header->num_docs > 0 &&
		header->num_docs <= TP_SEGMENT_CACHE_THRESHOLD &&
		header->ctid_map_offset > 0)
	{
		reader->cached_num_docs = header->num_docs;

		/* Load CTID map (6 bytes per doc) */
		reader->cached_ctids = palloc(
				header->num_docs * sizeof(ItemPointerData));
		tp_segment_read(
				reader,
				header->ctid_map_offset,
				reader->cached_ctids,
				header->num_docs * sizeof(ItemPointerData));
	}

	return reader;
}

void
tp_segment_close(TpSegmentReader *reader)
{
	if (!reader)
		return;

	if (BufferIsValid(reader->current_buffer))
		ReleaseBuffer(reader->current_buffer);

	if (BufferIsValid(reader->header_buffer))
		ReleaseBuffer(reader->header_buffer);

	if (reader->header)
		pfree(reader->header);

	if (reader->page_map)
		pfree(reader->page_map);

	/* Free V2 CTID cache (fieldnorm is inline in postings, no cache needed) */
	if (reader->cached_ctids)
		pfree(reader->cached_ctids);

	pfree(reader);
}

void
tp_segment_read(
		TpSegmentReader *reader, uint32 logical_offset, void *dest, uint32 len)
{
	char  *dest_ptr	  = (char *)dest;
	uint32 bytes_read = 0;

	while (bytes_read < len)
	{
		uint32 logical_page = logical_offset / SEGMENT_DATA_PER_PAGE;
		uint32 page_offset	= logical_offset % SEGMENT_DATA_PER_PAGE;
		uint32 to_read;
		Buffer buf;
		Page   page;
		char  *src;

		/* Calculate how much to read from this page */
		to_read = Min(len - bytes_read, SEGMENT_DATA_PER_PAGE - page_offset);

		/* Check if we have the page in cache */
		if (reader->current_logical_page != logical_page)
		{
			/* Release old buffer if any */
			if (BufferIsValid(reader->current_buffer))
			{
				ReleaseBuffer(reader->current_buffer);
				reader->current_buffer = InvalidBuffer;
			}

			/* Validate page number */
			if (logical_page >= reader->num_pages)
			{
				elog(ERROR,
					 "Invalid logical page %u (max %u), logical_offset=%u, "
					 "BLCKSZ=%d, reader->num_pages=%u",
					 logical_page,
					 reader->num_pages > 0 ? reader->num_pages - 1 : 0,
					 logical_offset,
					 BLCKSZ,
					 reader->num_pages);
			}

			/* Validate physical block number */
			{
				BlockNumber physical = reader->page_map[logical_page];
				if (physical >= reader->nblocks)
				{
					elog(ERROR,
						 "Invalid physical block %u for logical page %u "
						 "(nblocks=%u)",
						 physical,
						 logical_page,
						 reader->nblocks);
				}
			}

			/* Read the physical page */
			buf = ReadBuffer(reader->index, reader->page_map[logical_page]);

			reader->current_buffer		 = buf;
			reader->current_logical_page = logical_page;
		}
		else
		{
			buf = reader->current_buffer;
		}

		/* Lock buffer for reading */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		/* Copy data from page
		 * Data is stored starting at SizeOfPageHeaderData, so we need to add
		 * that
		 */
		page = BufferGetPage(buf);
		src	 = (char *)page + SizeOfPageHeaderData + page_offset;
		memcpy(dest_ptr + bytes_read, src, to_read);

		/* Unlock but keep buffer pinned for potential reuse */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		/* Advance pointers */
		bytes_read += to_read;
		logical_offset += to_read;
	}
}

/*
 * Get direct access to data in a segment page (zero-copy)
 * Returns true if successful, false if data spans multiple pages
 *
 * IMPORTANT: This function reuses reader->current_buffer when possible to
 * avoid creating multiple buffer pins on the same page. The buffer is locked
 * with BUFFER_LOCK_SHARE and must be released by calling
 * tp_segment_release_direct().
 *
 * Unlike tp_segment_read() which unlocks immediately after copying, this
 * function keeps the buffer locked so the caller can safely access the data.
 *
 * NOTE: Currently disabled due to buffer lifecycle issues with PostgreSQL 18
 * async I/O. Always returns false to force fallback to tp_segment_read().
 */
bool
tp_segment_get_direct(
		TpSegmentReader		  *reader,
		uint32				   logical_offset,
		uint32				   len,
		TpSegmentDirectAccess *access)
{
	uint32		logical_page = logical_offset / SEGMENT_DATA_PER_PAGE;
	uint32		page_offset	 = logical_offset % SEGMENT_DATA_PER_PAGE;
	Buffer		buf;
	Page		page;
	BlockNumber physical_block;

	/* Initialize access structure to invalid state */
	access->buffer	  = InvalidBuffer;
	access->page	  = NULL;
	access->data	  = NULL;
	access->available = 0;

	/* Check if data spans pages - if so, can't do zero-copy */
	if (page_offset + len > SEGMENT_DATA_PER_PAGE)
		return false;

	/* Validate logical page */
	if (logical_page >= reader->num_pages)
	{
		elog(ERROR,
			 "Invalid logical page %u (segment has %u pages)",
			 logical_page,
			 reader->num_pages);
	}

	/* Get physical block from page map */
	physical_block = reader->page_map[logical_page];

	/*
	 * Check if this page is already cached in reader->current_buffer.
	 * If so, reuse it to avoid creating another buffer pin on the same page.
	 */
	if (reader->current_logical_page == logical_page &&
		BufferIsValid(reader->current_buffer))
	{
		buf = reader->current_buffer;
	}
	else
	{
		/* Release old buffer if any */
		if (BufferIsValid(reader->current_buffer))
		{
			ReleaseBuffer(reader->current_buffer);
			reader->current_buffer		 = InvalidBuffer;
			reader->current_logical_page = UINT32_MAX;
		}

		/* Read the physical page - this pins the buffer */
		buf = ReadBuffer(reader->index, physical_block);

		/* Cache this buffer for future use */
		reader->current_buffer		 = buf;
		reader->current_logical_page = logical_page;
	}

	/* Lock buffer for reading */
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	/* Get page and data pointer */
	page = BufferGetPage(buf);

	/* Fill in access structure - note: we DON'T store the buffer here
	 * because we're reusing reader->current_buffer. The caller should
	 * call tp_segment_release_direct() which will just unlock, not release
	 * the pin.
	 */
	access->buffer	  = buf;
	access->page	  = page;
	access->data	  = (char *)page + SizeOfPageHeaderData + page_offset;
	access->available = SEGMENT_DATA_PER_PAGE - page_offset;

	return true;
}

/*
 * Release direct access to segment page
 *
 * Since tp_segment_get_direct() reuses reader->current_buffer, we only
 * unlock the buffer here - we do NOT release the pin. The buffer pin
 * will be released when:
 * - tp_segment_read() or tp_segment_get_direct() reads a different page
 * - tp_segment_close() is called
 */
void
tp_segment_release_direct(TpSegmentDirectAccess *access)
{
	if (BufferIsValid(access->buffer))
	{
		LockBuffer(access->buffer, BUFFER_LOCK_UNLOCK);
		/* DO NOT release the buffer - it's managed by the reader */
		access->buffer = InvalidBuffer;
		access->page   = NULL;
		access->data   = NULL;
	}
}

/*
 * Get document length from segment
 */
int32
tp_segment_get_document_length(TpSegmentReader *reader, ItemPointer ctid)
{
	TpSegmentHeader *header = reader->header;
	TpDocLength		 doc_length;
	int32			 left, right, mid;
	int				 cmp;

	if (header->num_docs == 0 || header->doc_lengths_offset == 0)
		return -1;

	/* Binary search through sorted document lengths */
	left  = 0;
	right = header->num_docs - 1;

	while (left <= right)
	{
		mid = left + (right - left) / 2;

		/* Read the document length at mid position */
		tp_segment_read(
				reader,
				header->doc_lengths_offset + (mid * sizeof(TpDocLength)),
				&doc_length,
				sizeof(TpDocLength));

		/* Compare CTIDs */
		cmp = ItemPointerCompare(ctid, &doc_length.ctid);

		if (cmp == 0)
		{
			/* Found the document */
			return (int32)doc_length.length;
		}
		else if (cmp < 0)
		{
			/* Target is before mid */
			right = mid - 1;
		}
		else
		{
			/* Target is after mid */
			left = mid + 1;
		}
	}

	/* Document not found */
	return -1;
}

/* Track FSM reuse statistics for debugging */
static uint32 fsm_pages_reused = 0;
static uint32 pages_extended   = 0;

/*
 * Allocate a single page for segment.
 * First checks the free space map for recycled pages, then extends if needed.
 */
static BlockNumber
allocate_segment_page(Relation index)
{
	Buffer		buffer;
	BlockNumber block;

	/* First, try to get a free page from FSM (recycled from compaction) */
	block = GetFreeIndexPage(index);
	if (block != InvalidBlockNumber)
	{
		/* Reuse a previously freed page */
		buffer = ReadBuffer(index, block);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		/* Initialize the recycled page */
		PageInit(BufferGetPage(buffer), BLCKSZ, 0);

		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);

		fsm_pages_reused++;
		return block;
	}

	/* No free pages available, extend the relation */
	buffer = ReadBufferExtended(
			index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);

	/* The page should already be initialized by RBM_ZERO_AND_LOCK */
	block = BufferGetBlockNumber(buffer);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	pages_extended++;
	return block;
}

/*
 * Report FSM reuse statistics (called at end of index build)
 */
void
tp_report_fsm_stats(void)
{
	if (fsm_pages_reused > 0 || pages_extended > 0)
	{
		elog(DEBUG1,
			 "Page allocation stats: %u reused from FSM, %u extended",
			 fsm_pages_reused,
			 pages_extended);
	}
	/* Reset for next build */
	fsm_pages_reused = 0;
	pages_extended	 = 0;
}

/*
 * Grow writer's page array if needed
 */
static void
tp_segment_writer_grow_pages(TpSegmentWriter *writer)
{
	if (writer->pages_allocated >= writer->pages_capacity)
	{
		uint32 new_capacity = writer->pages_capacity == 0
									? 8
									: writer->pages_capacity * 2;

		if (writer->pages)
		{
			writer->pages = repalloc(
					writer->pages, new_capacity * sizeof(BlockNumber));
		}
		else
		{
			writer->pages = palloc(new_capacity * sizeof(BlockNumber));
		}

		writer->pages_capacity = new_capacity;
	}
}

/*
 * Allocate a new page for the writer
 */
static BlockNumber
tp_segment_writer_allocate_page(TpSegmentWriter *writer)
{
	BlockNumber new_page;

	/* Ensure we have space in the pages array */
	tp_segment_writer_grow_pages(writer);

	/* Allocate the actual page */
	new_page = allocate_segment_page(writer->index);

	/* Add to our tracking array */
	writer->pages[writer->pages_allocated] = new_page;
	writer->pages_allocated++;

	return new_page;
}

/*
 * Write page index (chain of BlockNumbers).
 * This function is also used by segment_merge.c for merged segments.
 */
BlockNumber
write_page_index(Relation index, BlockNumber *pages, uint32 num_pages)
{
	BlockNumber index_root = InvalidBlockNumber;
	BlockNumber prev_block = InvalidBlockNumber;

	/*
	 * Calculate how many index pages we need.
	 * IMPORTANT: PageInit() aligns the special area to MAXALIGN, so we must
	 * account for that when calculating available space. Using raw sizeof()
	 * would give us 1 extra entry that overlaps the special area!
	 */
	uint32 entries_per_page = (BLCKSZ - SizeOfPageHeaderData -
							   MAXALIGN(sizeof(TpPageIndexSpecial))) /
							  sizeof(BlockNumber);
	uint32 num_index_pages = (num_pages + entries_per_page - 1) /
							 entries_per_page;

	/* Allocate index pages incrementally */
	BlockNumber *index_pages = palloc(num_index_pages * sizeof(BlockNumber));
	uint32		 i;

	for (i = 0; i < num_index_pages; i++)
		index_pages[i] = allocate_segment_page(index);

	/*
	 * Write index pages in reverse order (so we can chain them).
	 * Each page i stores entries [i*entries_per_page, (i+1)*entries_per_page).
	 * We iterate in reverse so we can set next_page pointers correctly.
	 */
	for (int i = num_index_pages - 1; i >= 0; i--)
	{
		Buffer				buffer;
		Page				page;
		BlockNumber		   *page_data;
		TpPageIndexSpecial *special;
		uint32				start_idx;
		uint32				entries_to_write;
		uint32				j;

		/* Calculate which entries this page should contain */
		start_idx		 = i * entries_per_page;
		entries_to_write = Min(entries_per_page, num_pages - start_idx);

		buffer = ReadBuffer(index, index_pages[i]);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);

		/* Initialize page with special area */
		PageInit(page, BLCKSZ, sizeof(TpPageIndexSpecial));

		/* Set up special area */
		special			   = (TpPageIndexSpecial *)PageGetSpecialPointer(page);
		special->magic	   = TP_PAGE_INDEX_MAGIC;
		special->version   = TP_PAGE_INDEX_VERSION;
		special->page_type = TP_PAGE_FILE_INDEX;
		special->next_page = prev_block;
		special->num_entries = entries_to_write;
		special->flags		 = 0;

		/* Use the data area after the page header */
		page_data = (BlockNumber *)((char *)page + SizeOfPageHeaderData);

		/* Fill with page numbers from pages[start_idx..start_idx+entries-1] */
		for (j = 0; j < entries_to_write; j++)
			page_data[j] = pages[start_idx + j];

		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);

		prev_block = index_pages[i];
		if (i == 0)
			index_root = index_pages[i];
	}

	pfree(index_pages);
	return index_root;
}

/*
 * Write posting lists section of segment
 */
static void
write_segment_postings(
		TpLocalIndexState *state,
		TpSegmentWriter	  *writer,
		TermInfo		  *terms,
		uint32			   num_terms)
{
	TpMemtable	 *memtable		  = get_memtable(state);
	dshash_table *doclength_table = NULL;
	uint32		  i;

	/* Attach to document length table for doc_length lookups */
	if (memtable && memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		doclength_table = tp_doclength_table_attach(
				state->dsa, memtable->doc_lengths_handle);
	}

	for (i = 0; i < num_terms; i++)
	{
		TpPostingList *posting_list = NULL;

		/* Get posting list from DSA if valid */
		if (terms[i].posting_list_dp != InvalidDsaPointer)
		{
			posting_list = (TpPostingList *)
					dsa_get_address(state->dsa, terms[i].posting_list_dp);
		}

		if (posting_list && posting_list->doc_count > 0)
		{
			/* Get posting entries from DSA */
			TpPostingEntry *entries = (TpPostingEntry *)
					dsa_get_address(state->dsa, posting_list->entries_dp);
			int32 j;

			/* Grow reusable buffer if needed */
			if (writer->posting_buffer_size < (uint32)posting_list->doc_count)
			{
				if (writer->posting_buffer)
					pfree(writer->posting_buffer);
				writer->posting_buffer_size = (uint32)posting_list->doc_count;
				writer->posting_buffer		= palloc(
						 sizeof(TpSegmentPosting) *
						 writer->posting_buffer_size);
			}

			/* Convert all postings to segment format using reusable buffer */
			for (j = 0; j < posting_list->doc_count; j++)
			{
				int32 doc_len = 0;

				writer->posting_buffer[j].ctid = entries[j].ctid;
				writer->posting_buffer[j].frequency =
						(uint16)entries[j].frequency;

				/* Look up doc_length from hash table */
				if (doclength_table)
				{
					doc_len = tp_get_document_length_attached(
							doclength_table, &entries[j].ctid);
				}
				if (doc_len <= 0)
				{
					elog(WARNING,
						 "No doc_length found for ctid (%u,%u), using 1",
						 ItemPointerGetBlockNumber(&entries[j].ctid),
						 ItemPointerGetOffsetNumber(&entries[j].ctid));
					doc_len = 1;
				}
				writer->posting_buffer[j].doc_length = (uint16)doc_len;
			}

			/* Write all postings in a single batch */
			tp_segment_writer_write(
					writer,
					writer->posting_buffer,
					sizeof(TpSegmentPosting) * posting_list->doc_count);

			/* Buffer is reused, not freed here */
		}
	}

	/* Detach from document length table */
	if (doclength_table)
		dshash_detach(doclength_table);
}

/*
 * Write document lengths section of segment
 */
static uint32
write_segment_doc_lengths(TpLocalIndexState *state, TpSegmentWriter *writer)
{
	TpMemtable *memtable  = get_memtable(state);
	uint32		doc_count = 0;

	/* Check if memtable has document lengths */
	if (memtable && memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table	 *doc_lengths_hash;
		dshash_seq_status seq_status;
		TpDocLengthEntry *doc_entry;
		dshash_parameters doc_lengths_params;
		TpDocLength		 *doc_lengths_array;
		uint32			  capacity = 1024;

		/* Setup parameters for doc lengths hash table */
		memset(&doc_lengths_params, 0, sizeof(doc_lengths_params));
		doc_lengths_params.key_size			= sizeof(ItemPointerData);
		doc_lengths_params.entry_size		= sizeof(TpDocLengthEntry);
		doc_lengths_params.hash_function	= dshash_memhash;
		doc_lengths_params.compare_function = dshash_memcmp;

		/* Attach to document lengths hash table */
		doc_lengths_hash = dshash_attach(
				state->dsa,
				&doc_lengths_params,
				memtable->doc_lengths_handle,
				NULL);

		/* Collect document lengths in a single pass */
		doc_lengths_array = palloc(sizeof(TpDocLength) * capacity);

		dshash_seq_init(&seq_status, doc_lengths_hash, false);
		while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(
						&seq_status)) != NULL)
		{
			/* Grow array if needed */
			if (doc_count >= capacity)
			{
				capacity *= 2;
				doc_lengths_array = repalloc(
						doc_lengths_array, sizeof(TpDocLength) * capacity);
			}

			doc_lengths_array[doc_count].ctid = doc_entry->ctid;
			doc_lengths_array[doc_count].length =
					(uint16)doc_entry->doc_length;
			doc_lengths_array[doc_count].reserved = 0;
			doc_count++;
		}
		dshash_seq_term(&seq_status);

		if (doc_count > 0)
		{
			/* Sort by CTID for binary search */
			qsort(doc_lengths_array,
				  doc_count,
				  sizeof(TpDocLength),
				  (int (*)(const void *, const void *))ItemPointerCompare);

			/* Write all document lengths in a single batch */
			tp_segment_writer_write(
					writer,
					doc_lengths_array,
					sizeof(TpDocLength) * doc_count);
		}

		pfree(doc_lengths_array);
		dshash_detach(doc_lengths_hash);
	}

	return doc_count;
}

/*
 * Write segment from memtable
 */
BlockNumber
tp_write_segment(TpLocalIndexState *state, Relation index)
{
	TermInfo	   *terms;
	uint32			num_terms;
	BlockNumber		header_block;
	BlockNumber		page_index_root;
	TpSegmentWriter writer;
	TpSegmentHeader header;
	TpDictionary	dict;

	uint32			*string_offsets;
	uint32			 string_pos;
	uint32			 i;
	Buffer			 header_buf;
	Page			 header_page;
	TpSegmentHeader *existing_header;
	uint32			*posting_offsets;
	uint32			 current_offset;

	/* Initialize the writer to avoid garbage values */
	memset(&writer, 0, sizeof(TpSegmentWriter));

	/* Build sorted dictionary */
	terms = tp_build_dictionary(state, &num_terms);

	if (num_terms == 0)
	{
		tp_free_dictionary(terms, num_terms);
		return InvalidBlockNumber;
	}

	/* Initialize writer with incremental page allocation */
	tp_segment_writer_init(&writer, index);

	/* The first page is allocated in tp_segment_writer_init */
	if (writer.pages_allocated > 0)
	{
		header_block = writer.pages[0];
	}
	else
	{
		elog(ERROR, "tp_write_segment: Failed to allocate first page");
	}

	/* Write header (placeholder - will update offsets later) */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0; /* Will be updated at the end */
	header.num_terms	= num_terms;
	header.level		= 0; /* L0 segment from memtable */
	header.next_segment = InvalidBlockNumber;

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Get corpus statistics from shared state (most up-to-date values) */
	header.num_docs		= state->shared->total_docs;
	header.total_tokens = state->shared->total_len;

	/* Write header (will update with correct values later) */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	dict.num_terms = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Allocate arrays for offsets */
	string_offsets	= palloc0(num_terms * sizeof(uint32));
	posting_offsets = palloc0(num_terms * sizeof(uint32));

	/* Single pass: calculate both string and posting offsets */
	string_pos	   = 0;
	current_offset = 0;
	for (i = 0; i < num_terms; i++)
	{
		TpPostingList *posting_list = NULL;

		/* Calculate string offset */
		string_offsets[i] = string_pos;
		/* TpStringEntry format: length (4) + text + dict_offset (4) */
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);

		/* Calculate posting offset */
		posting_offsets[i] = current_offset;

		/* Get posting list from DSA if valid */
		if (terms[i].posting_list_dp != InvalidDsaPointer)
		{
			posting_list = (TpPostingList *)
					dsa_get_address(state->dsa, terms[i].posting_list_dp);
			if (posting_list && posting_list->doc_count > 0)
			{
				current_offset += sizeof(TpSegmentPosting) *
								  posting_list->doc_count;
			}
		}
	}

	/* Write string offsets array */
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		/* Write string entry: length, text, dict offset */
		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Calculate where postings will start */
	header.entries_offset  = writer.current_offset;
	header.postings_offset = writer.current_offset +
							 (num_terms * sizeof(TpDictEntry));

	/* Write dictionary entries with absolute posting offsets */
	for (i = 0; i < num_terms; i++)
	{
		TpDictEntry	   entry;
		TpPostingList *posting_list = NULL;

		/* Get posting list from DSA if valid */
		if (terms[i].posting_list_dp != InvalidDsaPointer)
		{
			posting_list = (TpPostingList *)
					dsa_get_address(state->dsa, terms[i].posting_list_dp);
		}

		/* Fill in dictionary entry with ABSOLUTE offset */
		entry.posting_offset = header.postings_offset + posting_offsets[i];
		entry.posting_count	 = posting_list ? posting_list->doc_count : 0;
		entry.doc_freq		 = posting_list ? posting_list->doc_freq : 0;

		/* Store the posting offset for this term */
		terms[i].dict_entry_idx = i;

		/* Write the dictionary entry */
		tp_segment_writer_write(&writer, &entry, sizeof(TpDictEntry));
	}

	/* Verify we're at the expected position */
	Assert(writer.current_offset == header.postings_offset);

	pfree(posting_offsets);

	/* Write posting lists */
	write_segment_postings(state, &writer, terms, num_terms);

	/* Write document lengths */
	header.doc_lengths_offset = writer.current_offset;
	header.num_docs			  = write_segment_doc_lengths(state, &writer);

	/* Write page index */
	tp_segment_writer_flush(&writer);

	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;

	/* Update header with actual offsets and page count */
	header.data_size = writer.current_offset;
	header.num_pages = writer.pages_allocated;

	/* Finish with the writer BEFORE updating the header */
	tp_segment_writer_finish(&writer);

	/*
	 * Force dirty buffers to disk. This ensures crash safety for single-server
	 * deployments but does not provide WAL logging for streaming replication
	 * or point-in-time recovery (PITR).
	 *
	 * TODO: Add proper WAL logging for replication support. This would require
	 * registering a resource manager and implementing redo functions.
	 */
	FlushRelationBuffers(index);

	/* Now update the header on disk */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page = BufferGetPage(header_buf);

	/*
	 * Update header fields that were calculated after writing data.
	 * num_docs is set from write_segment_doc_lengths() return value which
	 * is the actual count of documents in THIS segment (not total corpus).
	 */
	existing_header					= (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);
	existing_header->strings_offset = header.strings_offset;
	existing_header->entries_offset = header.entries_offset;
	existing_header->postings_offset	= header.postings_offset;
	existing_header->doc_lengths_offset = header.doc_lengths_offset;
	existing_header->num_docs			= header.num_docs;
	existing_header->data_size			= header.data_size;
	existing_header->num_pages			= header.num_pages;
	existing_header->page_index			= header.page_index;

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/* Flush the header to disk to ensure it's persisted */
	FlushRelationBuffers(index);

	/*
	 * NOTE: We previously tried to truncate the relation here to reclaim
	 * unused space from PostgreSQL's pre-extension. However, this was buggy
	 * because write_page_index() allocates additional pages that weren't
	 * tracked in writer.pages. For now, we skip truncation - the extra space
	 * is minimal and will be reclaimed on VACUUM FULL if needed.
	 *
	 * TODO: If truncation is needed, have write_page_index() return all
	 * allocated block numbers so we can properly track the highest used block.
	 */

	/* Clean up */
	tp_free_dictionary(terms, num_terms);
	pfree(string_offsets);
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/*
 * V2 Segment Writer - Block-based storage with skip index
 *
 * The V2 format organizes posting lists into fixed-size blocks of 128 docs,
 * with a skip index that enables efficient skipping during query execution.
 *
 * Layout:
 *   Header -> Dictionary -> Strings -> DictEntriesV2 ->
 *   SkipIndex -> PostingBlocks -> Fieldnorms -> CTIDMap
 */

/*
 * Build docmap from memtable's document lengths.
 * Returns the docmap builder which must be freed by caller.
 */
static TpDocMapBuilder *
build_docmap_from_memtable(TpLocalIndexState *state)
{
	TpMemtable		 *memtable = get_memtable(state);
	TpDocMapBuilder	 *docmap;
	dshash_table	 *doc_lengths_hash;
	dshash_seq_status seq_status;
	TpDocLengthEntry *doc_entry;
	dshash_parameters doc_lengths_params;

	docmap = tp_docmap_create();

	if (!memtable || memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
		return docmap;

	/* Setup parameters for doc lengths hash table */
	memset(&doc_lengths_params, 0, sizeof(doc_lengths_params));
	doc_lengths_params.key_size			= sizeof(ItemPointerData);
	doc_lengths_params.entry_size		= sizeof(TpDocLengthEntry);
	doc_lengths_params.hash_function	= dshash_memhash;
	doc_lengths_params.compare_function = dshash_memcmp;

	/* Attach to document lengths hash table */
	doc_lengths_hash = dshash_attach(
			state->dsa,
			&doc_lengths_params,
			memtable->doc_lengths_handle,
			NULL);

	/* Iterate through all documents and add to docmap */
	dshash_seq_init(&seq_status, doc_lengths_hash, false);
	while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(&seq_status)) !=
		   NULL)
	{
		tp_docmap_add(docmap, &doc_entry->ctid, (uint32)doc_entry->doc_length);
	}
	dshash_seq_term(&seq_status);
	dshash_detach(doc_lengths_hash);

	/* Finalize to build output arrays */
	tp_docmap_finalize(docmap);

	return docmap;
}

/*
 * Per-term block information for V2 streaming format.
 * Updated for streaming layout: postings written before skip index.
 */
typedef struct TermBlockInfo
{
	uint32 posting_offset;	 /* Absolute offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into accumulated skip entries array */
} TermBlockInfo;

/*
 * Write V2 segment from memtable with streaming format.
 *
 * Layout: [header] → [dictionary] → [postings] → [skip index] →
 *         [fieldnorm] → [ctid map]
 *
 * This matches the merge format: postings written before skip index.
 */
BlockNumber
tp_write_segment_v2(TpLocalIndexState *state, Relation index)
{
	TermInfo		*terms;
	uint32			 num_terms;
	BlockNumber		 header_block;
	BlockNumber		 page_index_root;
	TpSegmentWriter	 writer;
	TpSegmentHeader	 header;
	TpDictionary	 dict;
	TpDocMapBuilder *docmap;

	uint32			*string_offsets;
	uint32			 string_pos;
	uint32			 i;
	Buffer			 header_buf;
	Page			 header_page;
	TpSegmentHeader *existing_header;
	TermBlockInfo	*term_blocks;

	/* Accumulated skip entries for all terms */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	/* Initialize the writer to avoid garbage values */
	memset(&writer, 0, sizeof(TpSegmentWriter));

	/* Build docmap from memtable */
	docmap = build_docmap_from_memtable(state);

	/* Build sorted dictionary */
	terms = tp_build_dictionary(state, &num_terms);

	if (num_terms == 0)
	{
		tp_free_dictionary(terms, num_terms);
		tp_docmap_destroy(docmap);
		return InvalidBlockNumber;
	}

	/* Initialize writer with incremental page allocation */
	tp_segment_writer_init(&writer, index);

	if (writer.pages_allocated > 0)
	{
		header_block = writer.pages[0];
	}
	else
	{
		elog(ERROR, "tp_write_segment_v2: Failed to allocate first page");
	}

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_V2;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Get corpus statistics from shared state */
	header.num_docs		= state->shared->total_docs;
	header.total_tokens = state->shared->total_len;

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	dict.num_terms = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Build string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);
	}

	/* Write string offsets array */
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntryV2);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset - dict entries written after postings loop */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries - we'll fill them in after streaming */
	{
		TpDictEntryV2 placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntryV2));
		for (i = 0; i < num_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntryV2));
	}

	/* Postings start here - streaming format writes postings first */
	header.postings_offset = writer.current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(TermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming pass: for each term, convert postings and write immediately.
	 */
	for (i = 0; i < num_terms; i++)
	{
		TpPostingList  *posting_list = NULL;
		TpPostingEntry *entries		 = NULL;
		uint32			doc_count	 = 0;
		uint32			block_idx;
		uint32			num_blocks;
		TpBlockPosting *block_postings = NULL;

		/* Record where this term's postings start */
		term_blocks[i].posting_offset	= writer.current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		if (terms[i].posting_list_dp != InvalidDsaPointer)
		{
			posting_list = (TpPostingList *)
					dsa_get_address(state->dsa, terms[i].posting_list_dp);
			if (posting_list && posting_list->doc_count > 0)
			{
				entries = (TpPostingEntry *)
						dsa_get_address(state->dsa, posting_list->entries_dp);
				doc_count = posting_list->doc_count;
			}
		}

		term_blocks[i].doc_freq = posting_list ? posting_list->doc_freq : 0;

		if (doc_count == 0)
		{
			term_blocks[i].block_count = 0;
			continue;
		}

		/* Calculate number of blocks */
		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
		if (num_blocks == 0 && doc_count > 0)
			num_blocks = 1;
		term_blocks[i].block_count = (uint16)num_blocks;

		/* Convert postings to block format */
		block_postings = palloc(doc_count * sizeof(TpBlockPosting));
		{
			uint32 j;
			for (j = 0; j < doc_count; j++)
			{
				uint32 doc_id = tp_docmap_lookup(docmap, &entries[j].ctid);
				uint8  norm;

				if (doc_id == UINT32_MAX)
					elog(ERROR,
						 "CTID (%u,%u) not found in docmap",
						 ItemPointerGetBlockNumber(&entries[j].ctid),
						 ItemPointerGetOffsetNumber(&entries[j].ctid));

				norm = tp_docmap_get_fieldnorm(docmap, doc_id);

				block_postings[j].doc_id	= doc_id;
				block_postings[j].frequency = (uint16)entries[j].frequency;
				block_postings[j].fieldnorm = norm;
				block_postings[j].reserved	= 0;
			}
		}

		/* Write posting blocks and build skip entries */
		for (block_idx = 0; block_idx < num_blocks; block_idx++)
		{
			TpSkipEntry skip;
			uint32		block_start = block_idx * TP_BLOCK_SIZE;
			uint32 block_end = Min(block_start + TP_BLOCK_SIZE, doc_count);
			uint32 j;
			uint16 max_tf	   = 0;
			uint8  max_norm	   = 0;
			uint32 last_doc_id = 0;

			/* Calculate block stats */
			for (j = block_start; j < block_end; j++)
			{
				if (block_postings[j].doc_id > last_doc_id)
					last_doc_id = block_postings[j].doc_id;
				if (block_postings[j].frequency > max_tf)
					max_tf = block_postings[j].frequency;
				if (block_postings[j].fieldnorm > max_norm)
					max_norm = block_postings[j].fieldnorm;
			}

			/* Build skip entry with actual posting offset */
			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)(block_end - block_start);
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = max_norm;
			skip.posting_offset = writer.current_offset;
			skip.flags			= TP_BLOCK_FLAG_UNCOMPRESSED;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			/* Accumulate skip entry */
			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;

			/* Write posting block data */
			tp_segment_writer_write(
					&writer,
					&block_postings[block_start],
					(block_end - block_start) * sizeof(TpBlockPosting));
		}

		pfree(block_postings);
	}

	/* Skip index starts here - after all postings */
	header.skip_index_offset = writer.current_offset;

	/* Write all accumulated skip entries */
	if (skip_entries_count > 0)
	{
		tp_segment_writer_write(
				&writer,
				all_skip_entries,
				skip_entries_count * sizeof(TpSkipEntry));
	}

	pfree(all_skip_entries);

	/* Write fieldnorm table */
	header.fieldnorm_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
	}

	/* Write CTID map */
	header.ctid_map_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_map,
				docmap->num_docs * sizeof(ItemPointerData));
	}

	/* Update num_docs to actual count from this segment */
	header.num_docs = docmap->num_docs;

	/* V1 doc_lengths_offset not used in V2, set to 0 */
	header.doc_lengths_offset = 0;

	/* Write page index */
	tp_segment_writer_flush(&writer);

	/*
	 * Mark buffer as empty to prevent tp_segment_writer_finish from flushing
	 * again and overwriting our dict entry updates.
	 */
	writer.buffer_pos = SizeOfPageHeaderData;

	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;

	/* Update header with actual values */
	header.data_size = writer.current_offset;
	header.num_pages = writer.pages_allocated;

	/*
	 * Now write the dictionary entries with correct skip_index_offset values.
	 * Do this BEFORE tp_segment_writer_finish so writer.pages is still valid.
	 */
	{
		Buffer dict_buf;
		uint32 entry_logical_page;
		uint32 current_page = UINT32_MAX;

		for (i = 0; i < num_terms; i++)
		{
			TpDictEntryV2 entry;
			uint32		  entry_offset;
			uint32		  page_offset;
			BlockNumber	  physical_block;

			/* Build the entry */
			entry.skip_index_offset = header.skip_index_offset +
									  (term_blocks[i].skip_entry_start *
									   sizeof(TpSkipEntry));
			entry.block_count = term_blocks[i].block_count;
			entry.reserved	  = 0;
			entry.doc_freq	  = term_blocks[i].doc_freq;

			/* Calculate where this entry is in the segment */
			entry_offset = header.entries_offset + (i * sizeof(TpDictEntryV2));
			entry_logical_page = entry_offset / SEGMENT_DATA_PER_PAGE;
			page_offset		   = entry_offset % SEGMENT_DATA_PER_PAGE;

			/* Read page if different from current */
			if (entry_logical_page != current_page)
			{
				if (current_page != UINT32_MAX)
				{
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);
				}

				physical_block = writer.pages[entry_logical_page];
				dict_buf	   = ReadBuffer(index, physical_block);
				LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
				current_page = entry_logical_page;
			}

			/* Write entry to page */
			{
				Page  page = BufferGetPage(dict_buf);
				char *dest = (char *)page + SizeOfPageHeaderData + page_offset;
				memcpy(dest, &entry, sizeof(TpDictEntryV2));
			}
		}

		/* Release last buffer */
		if (current_page != UINT32_MAX)
		{
			MarkBufferDirty(dict_buf);
			UnlockReleaseBuffer(dict_buf);
		}
	}

	tp_segment_writer_finish(&writer);

	/* Flush to disk */
	FlushRelationBuffers(index);

	/* Update header on disk */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page = BufferGetPage(header_buf);

	existing_header					= (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);
	existing_header->strings_offset = header.strings_offset;
	existing_header->entries_offset = header.entries_offset;
	existing_header->postings_offset	= header.postings_offset;
	existing_header->skip_index_offset	= header.skip_index_offset;
	existing_header->fieldnorm_offset	= header.fieldnorm_offset;
	existing_header->ctid_map_offset	= header.ctid_map_offset;
	existing_header->doc_lengths_offset = header.doc_lengths_offset;
	existing_header->num_docs			= header.num_docs;
	existing_header->data_size			= header.data_size;
	existing_header->num_pages			= header.num_pages;
	existing_header->page_index			= header.page_index;

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	FlushRelationBuffers(index);

	/* Clean up */
	tp_free_dictionary(terms, num_terms);
	pfree(string_offsets);
	pfree(term_blocks);
	tp_docmap_destroy(docmap);
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/*
 * Collect all pages belonging to a segment for later freeing.
 * This includes data pages (from page_map) and page index pages.
 *
 * Returns the total number of pages collected.
 * The caller must pfree the returned pages array when done.
 */
uint32
tp_segment_collect_pages(
		Relation index, BlockNumber root_block, BlockNumber **pages_out)
{
	TpSegmentReader *reader;
	BlockNumber		*all_pages;
	uint32			 num_pages;
	uint32			 capacity;
	BlockNumber		 page_index_block;
	uint32			 i;

	*pages_out = NULL;

	reader = tp_segment_open(index, root_block);
	if (!reader)
		return 0;

	/*
	 * Start with capacity for data pages. The +16 is just an optimization to
	 * reduce reallocs for page index pages; the array grows dynamically below.
	 */
	capacity  = reader->num_pages + 16;
	all_pages = palloc(sizeof(BlockNumber) * capacity);
	num_pages = 0;

	/* Collect all data pages from the page map */
	for (i = 0; i < reader->num_pages; i++)
		all_pages[num_pages++] = reader->page_map[i];

	/* Traverse and collect page index chain */
	page_index_block = reader->header->page_index;
	while (page_index_block != InvalidBlockNumber)
	{
		Buffer				index_buf;
		Page				index_page;
		TpPageIndexSpecial *special;

		/* Grow array if needed */
		if (num_pages >= capacity)
		{
			capacity *= 2;
			all_pages = repalloc(all_pages, sizeof(BlockNumber) * capacity);
		}

		/* Add this page index page */
		all_pages[num_pages++] = page_index_block;

		/* Read the page to get next pointer */
		index_buf = ReadBuffer(index, page_index_block);
		LockBuffer(index_buf, BUFFER_LOCK_SHARE);
		index_page = BufferGetPage(index_buf);

		special = (TpPageIndexSpecial *)PageGetSpecialPointer(index_page);

		/* Validate this is a page index page */
		if (special->magic != TP_PAGE_INDEX_MAGIC)
		{
			UnlockReleaseBuffer(index_buf);
			break;
		}

		page_index_block = special->next_page;
		UnlockReleaseBuffer(index_buf);
	}

	tp_segment_close(reader);

	*pages_out = all_pages;
	return num_pages;
}

/*
 * Free pages belonging to a segment by recording them in the FSM.
 * Call this after the segment is no longer referenced (metapage updated).
 */
void
tp_segment_free_pages(Relation index, BlockNumber *pages, uint32 num_pages)
{
	uint32 i;

	for (i = 0; i < num_pages; i++)
	{
		if (pages[i] == 0)
			elog(ERROR, "attempted to free metapage (block 0)");

		RecordFreeIndexPage(index, pages[i]);
	}
}

/*
 * Unified segment dump function using DumpOutput abstraction.
 * This allows dumping to either StringInfo (SQL return) or FILE.
 */
void
tp_dump_segment_to_output(
		Relation index, BlockNumber segment_root, DumpOutput *out)
{
	TpSegmentHeader header;
	Buffer			header_buf;
	Page			header_page;
	uint32			terms_to_show;

	if (segment_root == InvalidBlockNumber)
	{
		dump_printf(out, "\nNo segments written yet\n");
		return;
	}

	dump_printf(
			out,
			"\n========== Segment at block %u ==========\n",
			segment_root);

	/* Read the header page */
	header_buf = ReadBuffer(index, segment_root);
	LockBuffer(header_buf, BUFFER_LOCK_SHARE);
	header_page = BufferGetPage(header_buf);

	/* Copy header from page */
	memcpy(&header,
		   (char *)header_page + SizeOfPageHeaderData,
		   sizeof(TpSegmentHeader));

	/* Hex dump in full mode (file output) */
	if (out->full_dump)
	{
		unsigned char *page_data = (unsigned char *)header_page;
		int			   i, j;
		int			   bytes_to_dump = BLCKSZ; /* Full page, no truncation */

		dump_printf(
				out, "\n=== RAW PAGE DATA (%d bytes) ===\n", bytes_to_dump);

		for (i = 0; i < bytes_to_dump && i < BLCKSZ; i += 16)
		{
			dump_printf(out, "%04x: ", i);
			for (j = 0; j < 16 && (i + j) < bytes_to_dump; j++)
				dump_printf(out, "%02x ", page_data[i + j]);
			for (; j < 16; j++)
				dump_printf(out, "   ");
			dump_printf(out, " |");
			for (j = 0; j < 16 && (i + j) < bytes_to_dump; j++)
			{
				unsigned char c = page_data[i + j];
				dump_printf(out, "%c", (c >= 32 && c < 127) ? c : '.');
			}
			dump_printf(out, "|\n");
		}
	}

	UnlockReleaseBuffer(header_buf);

	/* Header info */
	dump_printf(out, "\n=== SEGMENT HEADER ===\n");
	dump_printf(
			out,
			"Magic: 0x%08X (expected 0x%08X) %s\n",
			header.magic,
			TP_SEGMENT_MAGIC,
			header.magic == TP_SEGMENT_MAGIC ? "VALID" : "INVALID!");
	dump_printf(out, "Version: %u\n", header.version);
	dump_printf(out, "Pages: %u\n", header.num_pages);
	dump_printf(out, "Data size: %u bytes\n", header.data_size);
	dump_printf(out, "Level: %u\n", header.level);
	dump_printf(out, "Page index: block %u\n", header.page_index);

	/* Corpus statistics */
	dump_printf(out, "\n=== CORPUS STATISTICS ===\n");
	dump_printf(out, "Terms: %u\n", header.num_terms);
	dump_printf(out, "Docs: %u\n", header.num_docs);
	dump_printf(
			out,
			"Total tokens: %llu\n",
			(unsigned long long)header.total_tokens);

	/* Section offsets */
	dump_printf(out, "\n=== SECTION OFFSETS ===\n");
	dump_printf(out, "Dictionary offset: %u\n", header.dictionary_offset);
	dump_printf(out, "Strings offset: %u\n", header.strings_offset);
	dump_printf(out, "Entries offset: %u\n", header.entries_offset);
	if (header.version == TP_SEGMENT_FORMAT_V2)
	{
		dump_printf(out, "Skip index offset: %u\n", header.skip_index_offset);
		dump_printf(out, "Postings offset: %u\n", header.postings_offset);
		dump_printf(out, "Fieldnorm offset: %u\n", header.fieldnorm_offset);
		dump_printf(out, "CTID map offset: %u\n", header.ctid_map_offset);
	}
	else
	{
		dump_printf(out, "Postings offset: %u\n", header.postings_offset);
		dump_printf(
				out, "Doc lengths offset: %u\n", header.doc_lengths_offset);
	}

	/* Page layout summary */
	if (header.data_size > 0)
	{
		dump_printf(out, "\n=== PAGE LAYOUT ===\n");
		dump_printf(
				out,
				"Dictionary: pages %u-%u\n",
				(uint32)(header.dictionary_offset / SEGMENT_DATA_PER_PAGE),
				(uint32)((header.strings_offset - 1) / SEGMENT_DATA_PER_PAGE));
		dump_printf(
				out,
				"Strings:    pages %u-%u\n",
				(uint32)(header.strings_offset / SEGMENT_DATA_PER_PAGE),
				(uint32)((header.entries_offset - 1) / SEGMENT_DATA_PER_PAGE));
		dump_printf(
				out,
				"Entries:    pages %u-%u\n",
				(uint32)(header.entries_offset / SEGMENT_DATA_PER_PAGE),
				(uint32)((header.postings_offset - 1) /
						 SEGMENT_DATA_PER_PAGE));
		dump_printf(
				out,
				"Postings:   pages %u-%u\n",
				(uint32)(header.postings_offset / SEGMENT_DATA_PER_PAGE),
				(uint32)((header.doc_lengths_offset - 1) /
						 SEGMENT_DATA_PER_PAGE));
		dump_printf(
				out,
				"Doc lengths: pages %u-%u\n",
				(uint32)(header.doc_lengths_offset / SEGMENT_DATA_PER_PAGE),
				(uint32)((header.data_size - 1) / SEGMENT_DATA_PER_PAGE));
	}

	/* Dictionary dump */
	if (header.num_terms > 0 && header.dictionary_offset > 0)
	{
		TpSegmentReader *reader;
		TpDictionary	 dict_header;
		uint32			*string_offsets;
		uint32			 i;

		/* Validate offsets */
		if (header.dictionary_offset >= header.data_size ||
			header.strings_offset >= header.data_size ||
			header.entries_offset >= header.data_size)
		{
			dump_printf(out, "\nERROR: Invalid offsets detected\n");
			return;
		}

		dump_printf(
				out,
				"\n=== DICTIONARY TERMS (%u total) ===\n",
				header.num_terms);

		reader = tp_segment_open(index, segment_root);

		/* Read dictionary header */
		tp_segment_read(
				reader,
				header.dictionary_offset,
				&dict_header,
				sizeof(dict_header.num_terms));

		/* Read string offsets */
		string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
		tp_segment_read(
				reader,
				header.dictionary_offset + sizeof(dict_header.num_terms),
				string_offsets,
				sizeof(uint32) * dict_header.num_terms);

		/* In full mode show all terms; otherwise limit */
		terms_to_show = out->full_dump ? header.num_terms
									   : Min(header.num_terms, 20);

		for (i = 0; i < terms_to_show; i++)
		{
			char *term_text;

			term_text = read_term_at_index(reader, &header, i, string_offsets);

			if (strlen(term_text) > 1024)
			{
				dump_printf(out, "  [%u] ERROR: Invalid string length\n", i);
				pfree(term_text);
				continue;
			}

			if (header.version == TP_SEGMENT_FORMAT_V2)
			{
				/* V2 format: block-based storage */
				TpDictEntryV2 entry_v2;
				read_dict_entry_v2(reader, &header, i, &entry_v2);

				dump_printf(
						out,
						"  [%04u] '%-30s' (docs=%4u, blocks=%4u)\n",
						i,
						term_text,
						entry_v2.doc_freq,
						entry_v2.block_count);

				/* Show blocks in full mode or for first few terms */
				if ((out->full_dump || i < 5) && entry_v2.block_count > 0)
				{
					uint32 j;
					uint32 blocks_to_show = out->full_dump
												  ? entry_v2.block_count
												  : Min(entry_v2.block_count,
														3);

					for (j = 0; j < blocks_to_show; j++)
					{
						TpSkipEntry skip;
						uint32		skip_off;
						uint32		k;
						uint32		postings_to_show;

						skip_off = entry_v2.skip_index_offset +
								   j * sizeof(TpSkipEntry);
						tp_segment_read(
								reader, skip_off, &skip, sizeof(TpSkipEntry));

						dump_printf(
								out,
								"         Block %u: docs=%u, "
								"last_doc=%u, max_tf=%u, "
								"offset=%u\n",
								j,
								skip.doc_count,
								skip.last_doc_id,
								skip.block_max_tf,
								skip.posting_offset);

						/* Show some postings from this block */
						postings_to_show = out->full_dump
												 ? skip.doc_count
												 : Min(skip.doc_count, 3);
						if (postings_to_show > 0)
						{
							TpBlockPosting *block_postings;
							block_postings = palloc(
									sizeof(TpBlockPosting) * postings_to_show);
							tp_segment_read(
									reader,
									skip.posting_offset,
									block_postings,
									sizeof(TpBlockPosting) * postings_to_show);

							dump_printf(out, "                  Postings: ");
							for (k = 0; k < postings_to_show; k++)
							{
								dump_printf(
										out,
										"doc%u:%u ",
										block_postings[k].doc_id,
										block_postings[k].frequency);
							}
							if (skip.doc_count > postings_to_show)
								dump_printf(
										out,
										"... (%u more)",
										skip.doc_count - postings_to_show);
							dump_printf(out, "\n");
							pfree(block_postings);
						}
					}
					if (entry_v2.block_count > blocks_to_show)
						dump_printf(
								out,
								"         ... (%u more blocks)\n",
								entry_v2.block_count - blocks_to_show);
				}
			}
			else
			{
				/* V1 format: flat posting lists */
				TpDictEntry dict_entry;
				read_dict_entry(reader, &header, i, &dict_entry);

				dump_printf(
						out,
						"  [%04u] '%-30s' (docs=%4u, postings=%4u)\n",
						i,
						term_text,
						dict_entry.doc_freq,
						dict_entry.posting_count);

				/* Show postings in full mode or for first few terms */
				if ((out->full_dump || i < 5) && dict_entry.posting_count > 0)
				{
					TpSegmentPosting *postings;
					uint32			  j;
					uint32			  postings_to_show =
							   out->full_dump ? dict_entry.posting_count
													  : Min(dict_entry.posting_count, 5);

					postings = palloc(
							sizeof(TpSegmentPosting) * postings_to_show);
					tp_segment_read(
							reader,
							dict_entry.posting_offset,
							postings,
							sizeof(TpSegmentPosting) * postings_to_show);

					dump_printf(out, "         Postings: ");
					for (j = 0; j < postings_to_show; j++)
					{
						dump_printf(
								out,
								"(%u,%u):%u ",
								ItemPointerGetBlockNumber(&postings[j].ctid),
								ItemPointerGetOffsetNumber(&postings[j].ctid),
								postings[j].frequency);
					}
					if (dict_entry.posting_count > postings_to_show)
						dump_printf(
								out,
								"... (%u more)",
								dict_entry.posting_count - postings_to_show);
					dump_printf(out, "\n");

					pfree(postings);
				}
			}

			pfree(term_text);
		}

		if (header.num_terms > terms_to_show)
		{
			dump_printf(
					out,
					"  ... and %u more terms\n",
					header.num_terms - terms_to_show);
		}

		pfree(string_offsets);
		tp_segment_close(reader);
	}

	/* V2-specific: dump fieldnorm table and CTID map */
	if (header.version == TP_SEGMENT_FORMAT_V2 && header.num_docs > 0)
	{
		TpSegmentReader *reader;
		uint32			 docs_to_show;
		uint32			 i;

		reader = tp_segment_open(index, segment_root);

		/* Fieldnorm table */
		dump_printf(
				out, "\n=== FIELDNORM TABLE (%u docs) ===\n", header.num_docs);
		docs_to_show = out->full_dump ? header.num_docs
									  : Min(header.num_docs, 10);
		if (header.fieldnorm_offset > 0)
		{
			uint8 *fieldnorms = palloc(docs_to_show);
			tp_segment_read(
					reader, header.fieldnorm_offset, fieldnorms, docs_to_show);

			dump_printf(out, "  Doc ID -> Length (encoded -> decoded):\n");
			for (i = 0; i < docs_to_show; i++)
			{
				dump_printf(
						out,
						"  [%04u] %3u -> %u\n",
						i,
						fieldnorms[i],
						decode_fieldnorm(fieldnorms[i]));
			}
			if (header.num_docs > docs_to_show)
				dump_printf(
						out,
						"  ... and %u more docs\n",
						header.num_docs - docs_to_show);
			pfree(fieldnorms);
		}

		/* CTID map */
		dump_printf(out, "\n=== CTID MAP (%u docs) ===\n", header.num_docs);
		if (header.ctid_map_offset > 0)
		{
			ItemPointerData *ctids = palloc(
					sizeof(ItemPointerData) * docs_to_show);
			tp_segment_read(
					reader,
					header.ctid_map_offset,
					ctids,
					sizeof(ItemPointerData) * docs_to_show);

			dump_printf(out, "  Doc ID -> CTID:\n");
			for (i = 0; i < docs_to_show; i++)
			{
				dump_printf(
						out,
						"  [%04u] (%u,%u)\n",
						i,
						ItemPointerGetBlockNumber(&ctids[i]),
						ItemPointerGetOffsetNumber(&ctids[i]));
			}
			if (header.num_docs > docs_to_show)
				dump_printf(
						out,
						"  ... and %u more docs\n",
						header.num_docs - docs_to_show);
			pfree(ctids);
		}

		tp_segment_close(reader);
	}

	dump_printf(out, "\n========== End Segment Dump ==========\n");
}

/* Segment writer helper functions */

void
tp_segment_writer_init(TpSegmentWriter *writer, Relation index)
{
	writer->index			= index;
	writer->pages			= NULL;
	writer->pages_allocated = 0;
	writer->pages_capacity	= 0;
	writer->current_offset	= 0;
	writer->buffer			= palloc(BLCKSZ);
	writer->buffer_page		= 0;
	writer->buffer_pos		= SizeOfPageHeaderData; /* Skip page header */

	/* Initialize reusable posting buffer */
	writer->posting_buffer		= NULL;
	writer->posting_buffer_size = 0;

	/* Allocate first page */
	tp_segment_writer_allocate_page(writer);

	/* Initialize first page */
	PageInit((Page)writer->buffer, BLCKSZ, 0);
}

void
tp_segment_writer_write(TpSegmentWriter *writer, const void *data, uint32 len)
{
	const char *src			  = (const char *)data;
	uint32		bytes_written = 0;

	while (bytes_written < len)
	{
		/* Calculate how much we can write to current page */
		uint32 page_space = BLCKSZ - writer->buffer_pos;
		uint32 to_write	  = Min(page_space, len - bytes_written);

		/* Copy data to buffer */
		memcpy(writer->buffer + writer->buffer_pos,
			   src + bytes_written,
			   to_write);
		writer->buffer_pos += to_write;
		writer->current_offset += to_write;
		bytes_written += to_write;

		/* If page is full, flush it */
		if (writer->buffer_pos >= BLCKSZ)
		{
			tp_segment_writer_flush(writer);

			/* Move to next page if we have more data */
			if (bytes_written < len)
			{
				writer->buffer_page++;

				/* Allocate a new page if needed */
				if (writer->buffer_page >= writer->pages_allocated)
				{
					tp_segment_writer_allocate_page(writer);
				}

				/* Initialize new page */
				PageInit((Page)writer->buffer, BLCKSZ, 0);
				writer->buffer_pos = SizeOfPageHeaderData;
			}
		}
	}
}

void
tp_segment_writer_flush(TpSegmentWriter *writer)
{
	Buffer buffer;
	Page   page;

	if (writer->buffer_page >= writer->pages_allocated)
		return; /* Nothing to flush */

	/* Write current buffer to disk */
	buffer = ReadBuffer(writer->index, writer->pages[writer->buffer_page]);
	LockBuffer(
			buffer, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to modify */
	page = BufferGetPage(buffer);
	memcpy(page, writer->buffer, BLCKSZ);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

void
tp_segment_writer_finish(TpSegmentWriter *writer)
{
	/* Flush any remaining data */
	if (writer->buffer_pos > SizeOfPageHeaderData)
	{
		tp_segment_writer_flush(writer);
	}
	pfree(writer->buffer);

	/* Free reusable posting buffer if allocated */
	if (writer->posting_buffer)
	{
		pfree(writer->posting_buffer);
		writer->posting_buffer		= NULL;
		writer->posting_buffer_size = 0;
	}
}
