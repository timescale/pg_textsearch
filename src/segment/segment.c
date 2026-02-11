/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment.c - Disk-based segment implementation
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/hash.h>
#include <access/table.h>
#include <catalog/namespace.h>
#include <catalog/storage.h>
#include <inttypes.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <stdio.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/indexfsm.h>
#include <storage/lock.h>
#include <unistd.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/timestamp.h>

#include "compression.h"
#include "debug/dump.h"
#include "dictionary.h"
#include "docmap.h"
#include "fieldnorm.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "pagemapper.h"
#include "segment.h"
#include "state/metapage.h"
#include "state/state.h"

/* External: compression GUC from mod.c */
extern bool tp_compress_segments;

/*
 * Note: We previously had a global page map cache here, but it was removed
 * due to race conditions when multiple backends accessed it concurrently.
 * Since segments are small and page maps are not frequently re-read in the
 * same backend, the performance impact of removing the cache is minimal.
 */

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
	uint64		  string_offset;

	string_offset = header->strings_offset + string_offsets[index];

	/* Read string length */
	tp_segment_read(
			reader, string_offset, &string_entry.length, sizeof(uint32));

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
 * Version-aware dictionary entry reader.
 * V3 segments have 12-byte TpDictEntryV3; V4 have 16-byte TpDictEntry.
 */
void
tp_segment_read_dict_entry(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			 index,
		TpDictEntry		*entry)
{
	uint64 entry_offset;

	if (reader->segment_version <= TP_SEGMENT_FORMAT_VERSION_3)
	{
		TpDictEntryV3 v3;

		entry_offset = header->entries_offset +
					   (uint64)index * sizeof(TpDictEntryV3);
		tp_segment_read(reader, entry_offset, &v3, sizeof(TpDictEntryV3));

		/* Widen V3 fields to V4 */
		entry->skip_index_offset = (uint64)v3.skip_index_offset;
		entry->block_count		 = v3.block_count;
		entry->reserved			 = v3.reserved;
		entry->doc_freq			 = v3.doc_freq;
	}
	else
	{
		entry_offset = header->entries_offset +
					   (uint64)index * sizeof(TpDictEntry);
		tp_segment_read(reader, entry_offset, entry, sizeof(TpDictEntry));
	}
}

/*
 * Open segment for reading.
 * If load_ctids is true, preloads all CTID arrays into memory (expensive).
 * If load_ctids is false, skips CTID preloading - use tp_segment_lookup_ctid
 * for deferred resolution.
 */
TpSegmentReader *
tp_segment_open_ex(Relation index, BlockNumber root_block, bool load_ctids)
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

	/*
	 * Read raw magic and version first to determine format, then
	 * read the correct header struct and widen to V4 if needed.
	 */
	{
		uint32 raw_magic;
		uint32 raw_version;

		memcpy(&raw_magic, PageGetContents(header_page), sizeof(uint32));
		memcpy(&raw_version,
			   (char *)PageGetContents(header_page) + sizeof(uint32),
			   sizeof(uint32));

		/* Validate magic before version dispatch */
		if (raw_magic != TP_SEGMENT_MAGIC)
		{
			UnlockReleaseBuffer(header_buf);
			pfree(reader);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid segment header at block %u", root_block),
					 errdetail(
							 "magic=0x%08X, expected 0x%08X",
							 raw_magic,
							 TP_SEGMENT_MAGIC)));
		}

		reader->segment_version = raw_version;
		reader->header			= palloc(sizeof(TpSegmentHeader));

		if (raw_version <= TP_SEGMENT_FORMAT_VERSION_3)
		{
			/* V3: read legacy struct and widen to V4 */
			TpSegmentHeaderV3 v3;

			memcpy(&v3,
				   PageGetContents(header_page),
				   sizeof(TpSegmentHeaderV3));

			header						= reader->header;
			header->magic				= v3.magic;
			header->version				= v3.version;
			header->created_at			= v3.created_at;
			header->num_pages			= v3.num_pages;
			header->data_size			= (uint64)v3.data_size;
			header->level				= v3.level;
			header->next_segment		= v3.next_segment;
			header->dictionary_offset	= (uint64)v3.dictionary_offset;
			header->strings_offset		= (uint64)v3.strings_offset;
			header->entries_offset		= (uint64)v3.entries_offset;
			header->postings_offset		= (uint64)v3.postings_offset;
			header->skip_index_offset	= (uint64)v3.skip_index_offset;
			header->fieldnorm_offset	= (uint64)v3.fieldnorm_offset;
			header->ctid_pages_offset	= (uint64)v3.ctid_pages_offset;
			header->ctid_offsets_offset = (uint64)v3.ctid_offsets_offset;
			header->num_terms			= v3.num_terms;
			header->num_docs			= v3.num_docs;
			header->total_tokens		= v3.total_tokens;
			header->page_index			= v3.page_index;
		}
		else if (raw_version == TP_SEGMENT_FORMAT_VERSION)
		{
			/* V4: direct copy */
			memcpy(reader->header,
				   PageGetContents(header_page),
				   sizeof(TpSegmentHeader));
			header = reader->header;
		}
		else
		{
			UnlockReleaseBuffer(header_buf);
			pfree(reader->header);
			pfree(reader);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unsupported segment format version %u "
							"at block %u",
							raw_version,
							root_block)));
		}
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
	 * Optionally preload CTID arrays into memory for result lookup.
	 * When load_ctids is false, callers should use tp_segment_lookup_ctid
	 * for deferred resolution of individual CTIDs.
	 */
	reader->cached_ctid_pages	= NULL;
	reader->cached_ctid_offsets = NULL;
	reader->cached_num_docs		= 0;

	if (load_ctids && header->num_docs > 0 && header->ctid_pages_offset > 0)
	{
		reader->cached_num_docs = header->num_docs;

		/* Load CTID pages array (4 bytes per doc) */
		reader->cached_ctid_pages = palloc(
				header->num_docs * sizeof(BlockNumber));
		tp_segment_read(
				reader,
				header->ctid_pages_offset,
				reader->cached_ctid_pages,
				header->num_docs * sizeof(BlockNumber));

		/* Load CTID offsets array (2 bytes per doc) */
		reader->cached_ctid_offsets = palloc(
				header->num_docs * sizeof(OffsetNumber));
		tp_segment_read(
				reader,
				header->ctid_offsets_offset,
				reader->cached_ctid_offsets,
				header->num_docs * sizeof(OffsetNumber));
	}

	return reader;
}

/*
 * Open segment for reading (default: skip CTID preloading).
 * This is the standard entry point for query execution.
 */
TpSegmentReader *
tp_segment_open(Relation index, BlockNumber root_block)
{
	return tp_segment_open_ex(index, root_block, false);
}

/*
 * Look up a single CTID by doc_id.
 * Used for deferred CTID resolution when CTIDs weren't preloaded.
 */
void
tp_segment_lookup_ctid(
		TpSegmentReader *reader, uint32 doc_id, ItemPointerData *ctid_out)
{
	BlockNumber	 page;
	OffsetNumber offset;

	Assert(reader != NULL);
	Assert(ctid_out != NULL);
	Assert(doc_id < reader->header->num_docs);

	/* If CTIDs were preloaded, use the cache */
	if (reader->cached_ctid_pages != NULL)
	{
		ItemPointerSet(
				ctid_out,
				reader->cached_ctid_pages[doc_id],
				reader->cached_ctid_offsets[doc_id]);
		return;
	}

	/* Read page number (4 bytes) from ctid_pages array */
	tp_segment_read(
			reader,
			reader->header->ctid_pages_offset + doc_id * sizeof(BlockNumber),
			&page,
			sizeof(BlockNumber));

	/* Read offset (2 bytes) from ctid_offsets array */
	tp_segment_read(
			reader,
			reader->header->ctid_offsets_offset +
					doc_id * sizeof(OffsetNumber),
			&offset,
			sizeof(OffsetNumber));

	ItemPointerSet(ctid_out, page, offset);
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

	/* Free CTID caches (fieldnorm is inline in postings, no cache needed) */
	if (reader->cached_ctid_pages)
		pfree(reader->cached_ctid_pages);
	if (reader->cached_ctid_offsets)
		pfree(reader->cached_ctid_offsets);

	pfree(reader);
}

void
tp_segment_read(
		TpSegmentReader *reader, uint64 logical_offset, void *dest, uint32 len)
{
	char  *dest_ptr	  = (char *)dest;
	uint32 bytes_read = 0;

	while (bytes_read < len)
	{
		uint32 logical_page = (uint32)(logical_offset / SEGMENT_DATA_PER_PAGE);
		uint32 page_offset	= (uint32)(logical_offset % SEGMENT_DATA_PER_PAGE);
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
					 "Invalid logical page %u (max %u), "
					 "logical_offset=%" PRIu64 ", "
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
 * This function creates its OWN buffer pin, independent of
 * reader->current_buffer. This is necessary because multiple iterators may
 * have active direct accesses while other code calls tp_segment_read() which
 * modifies reader->current_buffer.
 *
 * The buffer is locked with BUFFER_LOCK_SHARE and must be released by calling
 * tp_segment_release_direct(), which will both unlock AND release the pin.
 */
bool
tp_segment_get_direct(
		TpSegmentReader		  *reader,
		uint64				   logical_offset,
		uint32				   len,
		TpSegmentDirectAccess *access)
{
	uint32 logical_page = (uint32)(logical_offset / SEGMENT_DATA_PER_PAGE);
	uint32 page_offset	= (uint32)(logical_offset % SEGMENT_DATA_PER_PAGE);
	Buffer buf;
	Page   page;
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
	 * Always create a fresh buffer pin for direct access.
	 * This ensures the buffer remains valid even if tp_segment_read()
	 * is called and releases reader->current_buffer.
	 */
	buf = ReadBuffer(reader->index, physical_block);

	/* Lock buffer for reading */
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	/* Get page and data pointer */
	page = BufferGetPage(buf);

	/*
	 * Fill in access structure with our own buffer pin.
	 * The caller MUST call tp_segment_release_direct() to release this pin.
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
 * Since tp_segment_get_direct() creates its own buffer pin, we must
 * both unlock AND release the buffer here.
 */
void
tp_segment_release_direct(TpSegmentDirectAccess *access)
{
	if (BufferIsValid(access->buffer))
	{
		LockBuffer(access->buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(access->buffer);
		access->buffer = InvalidBuffer;
		access->page   = NULL;
		access->data   = NULL;
	}
}

/*
 * Allocate a single page for segment.
 * Checks FSM for recycled pages first, then extends the relation if needed.
 */
static BlockNumber
allocate_segment_page(Relation index)
{
	Buffer		buffer;
	BlockNumber block;

	/* Try to get a free page from FSM (recycled from compaction) */
	block = GetFreeIndexPage(index);
	if (block != InvalidBlockNumber)
	{
		buffer = ReadBuffer(index, block);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		PageInit(BufferGetPage(buffer), BLCKSZ, 0);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
		return block;
	}

	/* No free pages available, extend the relation */
	buffer = ReadBufferExtended(
			index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
	block = BufferGetBlockNumber(buffer);
	PageInit(BufferGetPage(buffer), BLCKSZ, 0);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
	return block;
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
 * Allocate a new page for the writer.
 * Uses pre-allocated pool if available (parallel builds), otherwise
 * FSM/extend.
 */
static BlockNumber
tp_segment_writer_allocate_page(TpSegmentWriter *writer)
{
	BlockNumber new_page;
	Buffer		buf;

	tp_segment_writer_grow_pages(writer);

	/* Parallel builds use pre-allocated pool to avoid FSM in workers */
	if (writer->page_pool != NULL && writer->pool_next != NULL)
	{
		uint32 idx = pg_atomic_fetch_add_u32(writer->pool_next, 1);
		if (idx < writer->pool_size)
		{
			new_page								 = writer->page_pool[idx];
			writer->pages[writer->pages_allocated++] = new_page;
			return new_page;
		}

		/* Pool exhausted - extend directly (FSM unsafe in parallel workers) */
		buf = ReadBufferExtended(
				writer->index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
		new_page = BufferGetBlockNumber(buf);
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
		writer->pages[writer->pages_allocated++] = new_page;
		return new_page;
	}

	/* Non-parallel: use FSM via allocate_segment_page */
	new_page = allocate_segment_page(writer->index);
	writer->pages[writer->pages_allocated++] = new_page;
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
 * Calculate entries per page index page (for pool estimation).
 * Exported so parallel build can estimate page index pages needed.
 */
uint32
tp_page_index_entries_per_page(void)
{
	return (BLCKSZ - SizeOfPageHeaderData -
			MAXALIGN(sizeof(TpPageIndexSpecial))) /
		   sizeof(BlockNumber);
}

/*
 * Segment Writer - Block-based storage with skip index
 *
 * Posting lists are organized into fixed-size blocks of 128 docs,
 * with a skip index that enables efficient skipping during query execution.
 *
 * Layout:
 *   Header -> Dictionary -> Strings -> DictEntries ->
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
 * Per-term block information for streaming format.
 * Updated for streaming layout: postings written before skip index.
 */
typedef struct TermBlockInfo
{
	uint64 posting_offset;	 /* Absolute offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into accumulated skip entries array */
} TermBlockInfo;

/*
 * Write segment from memtable with streaming format.
 *
 * Layout: [header] → [dictionary] → [postings] → [skip index] →
 *         [fieldnorm] → [ctid map]
 *
 * This matches the merge format: postings written before skip index.
 */
BlockNumber
tp_write_segment(TpLocalIndexState *state, Relation index)
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
		elog(ERROR, "tp_write_segment: Failed to allocate first page");
	}

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
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
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset - dict entries written after postings loop */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries - we'll fill them in after streaming */
	{
		TpDictEntry placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntry));
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

		/* Calculate number of blocks (always >= 1 since doc_count > 0 here) */
		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
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
			uint8  min_norm	   = 255;
			uint32 last_doc_id = 0;

			/* Calculate block stats */
			for (j = block_start; j < block_end; j++)
			{
				if (block_postings[j].doc_id > last_doc_id)
					last_doc_id = block_postings[j].doc_id;
				if (block_postings[j].frequency > max_tf)
					max_tf = block_postings[j].frequency;
				if (block_postings[j].fieldnorm < min_norm)
					min_norm = block_postings[j].fieldnorm;
			}

			/* Build skip entry with actual posting offset */
			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)(block_end - block_start);
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = min_norm;
			skip.posting_offset = writer.current_offset;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			/* Write posting block data (compressed or uncompressed) */
			if (tp_compress_segments)
			{
				uint8  compressed_buf[TP_MAX_COMPRESSED_BLOCK_SIZE];
				uint32 compressed_size;

				compressed_size = tp_compress_block(
						&block_postings[block_start],
						block_end - block_start,
						compressed_buf);

				skip.flags = TP_BLOCK_FLAG_DELTA;
				tp_segment_writer_write(
						&writer, compressed_buf, compressed_size);
			}
			else
			{
				skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
				tp_segment_writer_write(
						&writer,
						&block_postings[block_start],
						(block_end - block_start) * sizeof(TpBlockPosting));
			}

			/* Accumulate skip entry */
			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc_huge(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;
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

	/* Write CTID pages array */
	header.ctid_pages_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));
	}

	/* Write CTID offsets array */
	header.ctid_offsets_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));
	}

	/* Update num_docs to actual count from this segment */
	header.num_docs = docmap->num_docs;

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
			TpDictEntry entry;
			uint64		entry_offset;
			uint32		page_offset;
			BlockNumber physical_block;

			/* Build the entry */
			entry.skip_index_offset =
					header.skip_index_offset +
					((uint64)term_blocks[i].skip_entry_start *
					 sizeof(TpSkipEntry));
			entry.block_count = term_blocks[i].block_count;
			entry.reserved	  = 0;
			entry.doc_freq	  = term_blocks[i].doc_freq;

			/* Calculate where this entry is in the segment */
			entry_offset = header.entries_offset +
						   ((uint64)i * sizeof(TpDictEntry));
			entry_logical_page = (uint32)(entry_offset /
										  SEGMENT_DATA_PER_PAGE);
			page_offset = (uint32)(entry_offset % SEGMENT_DATA_PER_PAGE);

			/* Bounds check */
			if (entry_logical_page >= writer.pages_allocated)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("dict entry %u logical page %u >= "
								"pages_allocated %u",
								i,
								entry_logical_page,
								writer.pages_allocated)));

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

			/* Write entry to page - handle page boundary spanning */
			{
				uint32 bytes_on_this_page = SEGMENT_DATA_PER_PAGE -
											page_offset;

				if (bytes_on_this_page >= sizeof(TpDictEntry))
				{
					/* Entry fits entirely on this page */
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					memcpy(dest, &entry, sizeof(TpDictEntry));
				}
				else
				{
					/* Entry spans two pages */
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					char *src = (char *)&entry;

					/* Write first part to current page */
					memcpy(dest, src, bytes_on_this_page);

					/* Move to next page */
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);

					entry_logical_page++;
					if (entry_logical_page >= writer.pages_allocated)
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("dict entry spans beyond allocated")));

					physical_block = writer.pages[entry_logical_page];
					dict_buf	   = ReadBuffer(index, physical_block);
					LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
					current_page = entry_logical_page;

					/* Write remaining part to next page */
					page = BufferGetPage(dict_buf);
					dest = (char *)page + SizeOfPageHeaderData;
					memcpy(dest,
						   src + bytes_on_this_page,
						   sizeof(TpDictEntry) - bytes_on_this_page);
				}
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

	existing_header = (TpSegmentHeader *)PageGetContents(header_page);
	existing_header->strings_offset		 = header.strings_offset;
	existing_header->entries_offset		 = header.entries_offset;
	existing_header->postings_offset	 = header.postings_offset;
	existing_header->skip_index_offset	 = header.skip_index_offset;
	existing_header->fieldnorm_offset	 = header.fieldnorm_offset;
	existing_header->ctid_pages_offset	 = header.ctid_pages_offset;
	existing_header->ctid_offsets_offset = header.ctid_offsets_offset;
	existing_header->num_docs			 = header.num_docs;
	existing_header->data_size			 = header.data_size;
	existing_header->num_pages			 = header.num_pages;
	existing_header->page_index			 = header.page_index;

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

	/* Version-aware header read */
	{
		uint32 raw_version;
		memcpy(&raw_version,
			   (char *)PageGetContents(header_page) + sizeof(uint32),
			   sizeof(uint32));

		if (raw_version <= TP_SEGMENT_FORMAT_VERSION_3)
		{
			TpSegmentHeaderV3 v3;
			memcpy(&v3,
				   PageGetContents(header_page),
				   sizeof(TpSegmentHeaderV3));

			header.magic			   = v3.magic;
			header.version			   = v3.version;
			header.created_at		   = v3.created_at;
			header.num_pages		   = v3.num_pages;
			header.data_size		   = (uint64)v3.data_size;
			header.level			   = v3.level;
			header.next_segment		   = v3.next_segment;
			header.dictionary_offset   = (uint64)v3.dictionary_offset;
			header.strings_offset	   = (uint64)v3.strings_offset;
			header.entries_offset	   = (uint64)v3.entries_offset;
			header.postings_offset	   = (uint64)v3.postings_offset;
			header.skip_index_offset   = (uint64)v3.skip_index_offset;
			header.fieldnorm_offset	   = (uint64)v3.fieldnorm_offset;
			header.ctid_pages_offset   = (uint64)v3.ctid_pages_offset;
			header.ctid_offsets_offset = (uint64)v3.ctid_offsets_offset;
			header.num_terms		   = v3.num_terms;
			header.num_docs			   = v3.num_docs;
			header.total_tokens		   = v3.total_tokens;
			header.page_index		   = v3.page_index;
		}
		else
		{
			memcpy(&header,
				   PageGetContents(header_page),
				   sizeof(TpSegmentHeader));
		}
	}

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
	dump_printf(out, "Data size: %" PRIu64 " bytes\n", header.data_size);
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
	dump_printf(
			out, "Dictionary offset: %" PRIu64 "\n", header.dictionary_offset);
	dump_printf(out, "Strings offset: %" PRIu64 "\n", header.strings_offset);
	dump_printf(out, "Entries offset: %" PRIu64 "\n", header.entries_offset);
	dump_printf(
			out, "Skip index offset: %" PRIu64 "\n", header.skip_index_offset);
	dump_printf(out, "Postings offset: %" PRIu64 "\n", header.postings_offset);
	dump_printf(
			out, "Fieldnorm offset: %" PRIu64 "\n", header.fieldnorm_offset);
	dump_printf(
			out, "CTID pages offset: %" PRIu64 "\n", header.ctid_pages_offset);
	dump_printf(
			out,
			"CTID offsets offset: %" PRIu64 "\n",
			header.ctid_offsets_offset);

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
				(uint32)((header.skip_index_offset - 1) /
						 SEGMENT_DATA_PER_PAGE));
		dump_printf(
				out,
				"Skip index: pages %u-%u\n",
				(uint32)(header.skip_index_offset / SEGMENT_DATA_PER_PAGE),
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

			{
				/* Block-based storage */
				TpDictEntry entry;
				tp_segment_read_dict_entry(reader, &header, i, &entry);

				dump_printf(
						out,
						"  [%04u] '%-30s' (docs=%4u, blocks=%4u)\n",
						i,
						term_text,
						entry.doc_freq,
						entry.block_count);

				/* Show blocks in full mode or for first few terms */
				if ((out->full_dump || i < 5) && entry.block_count > 0)
				{
					uint32 j;
					uint32 blocks_to_show = out->full_dump
												  ? entry.block_count
												  : Min(entry.block_count, 3);

					for (j = 0; j < blocks_to_show; j++)
					{
						TpSkipEntry skip;
						uint32		k;
						uint32		postings_to_show;

						tp_segment_read_skip_entry(
								reader, entry.skip_index_offset, j, &skip);

						dump_printf(
								out,
								"         Block %u: docs=%u, "
								"last_doc=%u, max_tf=%u, "
								"offset=%" PRIu64 "\n",
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
					if (entry.block_count > blocks_to_show)
						dump_printf(
								out,
								"         ... (%u more blocks)\n",
								entry.block_count - blocks_to_show);
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

	/* Dump fieldnorm table and CTID map */
	if (header.num_docs > 0)
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
		if (header.ctid_pages_offset > 0)
		{
			BlockNumber	 *pages	  = palloc(sizeof(BlockNumber) * docs_to_show);
			OffsetNumber *offsets = palloc(
					sizeof(OffsetNumber) * docs_to_show);
			tp_segment_read(
					reader,
					header.ctid_pages_offset,
					pages,
					sizeof(BlockNumber) * docs_to_show);
			tp_segment_read(
					reader,
					header.ctid_offsets_offset,
					offsets,
					sizeof(OffsetNumber) * docs_to_show);

			dump_printf(out, "  Doc ID -> CTID:\n");
			for (i = 0; i < docs_to_show; i++)
			{
				dump_printf(
						out, "  [%04u] (%u,%u)\n", i, pages[i], offsets[i]);
			}
			if (header.num_docs > docs_to_show)
				dump_printf(
						out,
						"  ... and %u more docs\n",
						header.num_docs - docs_to_show);
			pfree(pages);
			pfree(offsets);
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

	/* No page pool for normal builds */
	writer->page_pool = NULL;
	writer->pool_size = 0;
	writer->pool_next = NULL;

	/* Allocate first page */
	tp_segment_writer_allocate_page(writer);

	/* Initialize first page */
	PageInit((Page)writer->buffer, BLCKSZ, 0);
}

/*
 * Initialize segment writer with pre-allocated page pool.
 * Used by parallel builds where pages are pre-allocated to avoid FSM
 * contention.
 */
void
tp_segment_writer_init_with_pool(
		TpSegmentWriter	 *writer,
		Relation		  index,
		BlockNumber		 *page_pool,
		uint32			  pool_size,
		pg_atomic_uint32 *pool_next)
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

	/* Set up page pool */
	writer->page_pool = page_pool;
	writer->pool_size = pool_size;
	writer->pool_next = pool_next;

	/* Allocate first page (from pool) */
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
	Buffer		buffer;
	Page		page;
	BlockNumber block;

	if (writer->buffer_page >= writer->pages_allocated)
		return; /* Nothing to flush */

	block = writer->pages[writer->buffer_page];

	/* Write current buffer to disk */
	buffer = ReadBuffer(writer->index, block);
	LockBuffer(
			buffer, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to modify */
	page = BufferGetPage(buffer);

	/*
	 * Copy only the data portion of the page, preserving the page header
	 * that the buffer manager has set up. This avoids corrupting the LSN
	 * and other buffer manager state.
	 */
	memcpy((char *)page + SizeOfPageHeaderData,
		   (char *)writer->buffer + SizeOfPageHeaderData,
		   BLCKSZ - SizeOfPageHeaderData);

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
