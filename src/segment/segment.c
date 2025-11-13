/*-------------------------------------------------------------------------
 *
 * segment/segment.c
 *    Segment implementation for pg_textsearch
 *
 * IDENTIFICATION
 *    src/segment/segment.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <unistd.h>
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
#include "lib/dshash.h"
#include "miscadmin.h"
#include "segment.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
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

	header			  = reader->header;
	reader->num_pages = header->num_pages;

	elog(DEBUG1, "tp_segment_open: Reading header from block %u", root_block);
	elog(DEBUG1, "  magic = 0x%08X", header->magic);
	elog(DEBUG1, "  num_pages = %u", header->num_pages);
	elog(DEBUG1, "  page_index = %u", header->page_index);
	elog(DEBUG1, "  data_size = %u", header->data_size);

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

		/* Get pointer to page entries array */
		page_entries = (BlockNumber *)((char *)index_page +
									   SizeOfPageHeaderData);

		/* Copy page entries to our map */
		for (i = 0;
			 i < special->num_entries && pages_loaded < reader->num_pages;
			 i++)
		{
			reader->page_map[pages_loaded++] = page_entries[i];
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
		uint32 logical_page = logical_offset / BLCKSZ;
		uint32 page_offset	= logical_offset % BLCKSZ;
		uint32 to_read;
		Buffer buf;
		Page   page;
		char  *src;

		/* Calculate how much to read from this page */
		to_read = Min(len - bytes_read, BLCKSZ - page_offset);

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
 */
bool
tp_segment_get_direct(
		TpSegmentReader		  *reader,
		uint32				   logical_offset,
		uint32				   len,
		TpSegmentDirectAccess *access)
{
	uint32		logical_page = logical_offset / BLCKSZ;
	uint32		page_offset	 = logical_offset % BLCKSZ;
	Buffer		buf;
	Page		page;
	BlockNumber physical_block;

	/* Check if data spans pages - if so, can't do zero-copy */
	if (page_offset + len > BLCKSZ)
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

	/* Read the physical page */
	buf = ReadBuffer(reader->index, physical_block);

	/* Lock buffer for reading */
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	/* Get page and data pointer */
	page = BufferGetPage(buf);

	/* Fill in access structure */
	access->buffer	  = buf;
	access->page	  = page;
	access->data	  = (char *)page + SizeOfPageHeaderData + page_offset;
	access->available = BLCKSZ - page_offset;

	return true;
}

/*
 * Release direct access to segment page
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

/*
 * Allocate a single page for segment
 */
static BlockNumber
allocate_segment_page(Relation index)
{
	Buffer		buffer;
	BlockNumber block;

	/* Use RBM_ZERO_AND_LOCK to get a new zero-filled page */
	buffer = ReadBufferExtended(
			index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);

	/* The page should already be initialized by RBM_ZERO_AND_LOCK */
	block = BufferGetBlockNumber(buffer);

	elog(DEBUG2, "Allocated page at block %u", block);

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

		elog(DEBUG2, "Grew page array capacity to %u", new_capacity);
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

	elog(DEBUG2,
		 "Writer allocated page %u (total: %u)",
		 new_page,
		 writer->pages_allocated);

	return new_page;
}

/*
 * Write page index (chain of BlockNumbers)
 */
static BlockNumber
write_page_index(Relation index, BlockNumber *pages, uint32 num_pages)
{
	BlockNumber index_root = InvalidBlockNumber;
	BlockNumber prev_block = InvalidBlockNumber;
	uint32		page_idx   = 0;

	/* Calculate how many index pages we need */
	/* IMPORTANT: Must account for the special area in page size calculation */
	uint32 entries_per_page = (BLCKSZ - SizeOfPageHeaderData -
							   sizeof(TpPageIndexSpecial)) /
							  sizeof(BlockNumber);
	uint32 num_index_pages = (num_pages + entries_per_page - 1) /
							 entries_per_page;

	/* Allocate index pages incrementally */
	BlockNumber *index_pages = palloc(num_index_pages * sizeof(BlockNumber));
	uint32		 i;

	for (i = 0; i < num_index_pages; i++)
	{
		index_pages[i] = allocate_segment_page(index);
	}

	/* Write index pages in reverse order (so we can chain them) */
	for (int i = num_index_pages - 1; i >= 0; i--)
	{
		Buffer				buffer;
		Page				page;
		BlockNumber		   *page_data;
		TpPageIndexSpecial *special;
		uint32				entries_to_write;
		uint32				j;

		buffer = ReadBuffer(index, index_pages[i]);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to
													  modify */
		page = BufferGetPage(buffer);

		/* Initialize page with special area */
		PageInit(page, BLCKSZ, sizeof(TpPageIndexSpecial));

		/* Set up special area */
		special			   = (TpPageIndexSpecial *)PageGetSpecialPointer(page);
		special->next_page = prev_block;
		special->page_type = TP_PAGE_FILE_INDEX;
		special->num_entries = Min(entries_per_page, num_pages - page_idx);
		special->flags		 = 0;

		/* Use the data area after the page header
		 * IMPORTANT: The actual data must be placed between header and special
		 * area */
		page_data = (BlockNumber *)((char *)page + SizeOfPageHeaderData);

		/* Fill with page numbers */
		entries_to_write = special->num_entries;
		for (j = 0; j < entries_to_write; j++)
		{
			page_data[j] = pages[page_idx++];
		}

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
	uint32 i;

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
				writer->posting_buffer[j].ctid = entries[j].ctid;
				writer->posting_buffer[j].frequency =
						(uint16)entries[j].frequency;
			}

			/* Write all postings in a single batch */
			tp_segment_writer_write(
					writer,
					writer->posting_buffer,
					sizeof(TpSegmentPosting) * posting_list->doc_count);

			/* Buffer is reused, not freed here */
		}
	}
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
	TpSegmentHeader *updated_header;
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

	elog(DEBUG1,
		 "tp_write_segment: Starting segment write with %u terms",
		 num_terms);

	/* Initialize writer with incremental page allocation */
	tp_segment_writer_init(&writer, index);

	/* The first page is allocated in tp_segment_writer_init, so we can get it
	 * now */
	if (writer.pages_allocated > 0)
	{
		header_block = writer.pages[0];
		elog(DEBUG1,
			 "tp_write_segment: Writer initialized with header at block %u",
			 header_block);
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
	elog(DEBUG1,
		 "sizeof(TpSegmentHeader) = %lu, dictionary_offset = %u",
		 (unsigned long)sizeof(TpSegmentHeader),
		 header.dictionary_offset);

	/* Get corpus statistics from metapage */
	{
		TpIndexMetaPage metap = tp_get_metapage(index);
		if (metap)
		{
			header.num_docs		= metap->total_docs;
			header.total_tokens = metap->total_len;
			pfree(metap);
		}
		else
		{
			header.num_docs		= 0;
			header.total_tokens = 0;
		}
	}

	/* Write header (will update with correct values later) */
	elog(DEBUG1,
		 "tp_write_segment: Writing initial header with dictionary_offset=%u",
		 header.dictionary_offset);
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section - already set in header */
	elog(DEBUG1,
		 "tp_write_segment: Setting dictionary_offset to %u",
		 header.dictionary_offset);
	dict.num_terms = num_terms;
	elog(DEBUG1,
		 "Writing dictionary header with num_terms=%u at offset %u "
		 "(buffer_page=%u, block=%u)",
		 num_terms,
		 writer.current_offset,
		 writer.buffer_page,
		 writer.buffer_page < writer.pages_allocated
				 ? writer.pages[writer.buffer_page]
				 : InvalidBlockNumber);

	/* Debug: Check buffer before and after dictionary write */
	{
		uint32 *before_ptr = (uint32 *)(writer.buffer + writer.buffer_pos);
		elog(DEBUG1,
			 "Before dict write: buffer_pos=%u, value at buffer[%u]=%u",
			 writer.buffer_pos,
			 writer.buffer_pos,
			 *before_ptr);
	}

	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	{
		/* Dictionary was just written, check what's in the buffer */
		uint32 *dict_in_buffer = (uint32 *)(writer.buffer +
											SizeOfPageHeaderData + 80);
		elog(DEBUG1,
			 "After dict write: dict num_terms in buffer at offset %lu = %u",
			 (unsigned long)(SizeOfPageHeaderData + 80),
			 *dict_in_buffer);
	}

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

		elog(DEBUG1,
			 "Writing string %u: '%s' (len=%u) at offset %u",
			 i,
			 terms[i].term,
			 length,
			 writer.current_offset - header.strings_offset);

		/* Write string entry: length, text, dict offset */
		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Write dictionary entries with correct posting offsets */
	header.entries_offset = writer.current_offset;
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

		/* Fill in dictionary entry with correct offset */
		entry.posting_offset = posting_offsets[i];
		entry.posting_count	 = posting_list ? posting_list->doc_count : 0;
		entry.doc_freq		 = posting_list ? posting_list->doc_freq : 0;

		/* Store the posting offset for this term */
		terms[i].dict_entry_idx = i;

		/* Write the dictionary entry */
		tp_segment_writer_write(&writer, &entry, sizeof(TpDictEntry));
	}

	pfree(posting_offsets);

	/* Write posting lists */
	header.postings_offset = writer.current_offset;
	write_segment_postings(state, &writer, terms, num_terms);

	/* Write document lengths */
	header.doc_lengths_offset = writer.current_offset;
	header.num_docs			  = write_segment_doc_lengths(state, &writer);

	/* Write page index */
	tp_segment_writer_flush(&writer);

	elog(DEBUG1,
		 "tp_write_segment: Creating page index for %u pages",
		 writer.pages_allocated);

	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;

	elog(DEBUG1,
		 "tp_write_segment: Page index root at block %u",
		 page_index_root);

	/* Update header with actual offsets and page count */
	header.data_size = writer.current_offset;
	header.num_pages = writer.pages_allocated;

	/* Finish with the writer BEFORE updating the header */
	tp_segment_writer_finish(&writer);

	elog(DEBUG1, "tp_write_segment: Updating header with offsets:");
	elog(DEBUG1, "  dictionary_offset = %u", header.dictionary_offset);
	elog(DEBUG1, "  strings_offset = %u", header.strings_offset);
	elog(DEBUG1, "  entries_offset = %u", header.entries_offset);
	elog(DEBUG1, "  postings_offset = %u", header.postings_offset);
	elog(DEBUG1, "  doc_lengths_offset = %u", header.doc_lengths_offset);
	elog(DEBUG1, "  data_size = %u", header.data_size);
	elog(DEBUG1, "  num_pages = %u", header.num_pages);
	elog(DEBUG1, "  page_index = %u", header.page_index);

	/* Force a checkpoint to ensure data is on disk */
	FlushRelationBuffers(index);

	/* Now update the header on disk */
	elog(DEBUG1,
		 "tp_write_segment: Reading header block %u to update offsets",
		 header_block);
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to
													  modify */
	header_page = BufferGetPage(header_buf);

	/* Check what's currently in the header */
	existing_header = (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);
	elog(DEBUG1,
		 "tp_write_segment: Existing header magic = 0x%08X, dictionary_offset "
		 "= %u",
		 existing_header->magic,
		 existing_header->dictionary_offset);

	/* Debug: Check what's at the dictionary offset before update */
	{
		uint32 *dict_num_terms_ptr = (uint32 *)((char *)header_page +
												SizeOfPageHeaderData + 80);
		elog(DEBUG1,
			 "Before update: value at offset 80 (dict num_terms) = %u",
			 *dict_num_terms_ptr);
	}

	/* Only update the header fields that changed, not the entire page
	 * The dictionary starts immediately after the header at offset 80
	 * Be careful not to overwrite dictionary data or change num_terms
	 *
	 * IMPORTANT: Do NOT update num_terms, num_docs, or total_tokens as they
	 * were already set correctly when the header was first written.
	 */

	/* Update only the offset fields that were calculated after writing data */
	elog(DEBUG1,
		 "Updating header offsets: strings=%u, entries=%u, postings=%u, "
		 "doc_lengths=%u",
		 header.strings_offset,
		 header.entries_offset,
		 header.postings_offset,
		 header.doc_lengths_offset);
	elog(DEBUG1,
		 "Header num_terms before update: %u",
		 existing_header->num_terms);

	existing_header->strings_offset		= header.strings_offset;
	existing_header->entries_offset		= header.entries_offset;
	existing_header->postings_offset	= header.postings_offset;
	existing_header->doc_lengths_offset = header.doc_lengths_offset;
	existing_header->data_size			= header.data_size;
	existing_header->num_pages			= header.num_pages;
	existing_header->page_index			= header.page_index;
	/* DO NOT UPDATE: num_terms, num_docs, total_tokens - already correct */

	elog(DEBUG1,
		 "Header num_terms after update: %u",
		 existing_header->num_terms);

	/* Debug: Check what's at the dictionary offset after update */
	{
		uint32 *dict_num_terms_ptr = (uint32 *)((char *)header_page +
												SizeOfPageHeaderData + 80);
		elog(DEBUG1,
			 "After update: value at offset 80 (dict num_terms) = %u",
			 *dict_num_terms_ptr);
	}

	/* Verify the update */
	updated_header = (TpSegmentHeader *)((char *)header_page +
										 SizeOfPageHeaderData);
	elog(DEBUG1,
		 "tp_write_segment: After update dictionary_offset = %u",
		 updated_header->dictionary_offset);

	MarkBufferDirty(header_buf);

	/* Final check before releasing buffer */
	{
		uint32 *dict_num_terms_ptr = (uint32 *)((char *)header_page +
												SizeOfPageHeaderData + 80);
		elog(DEBUG1,
			 "Before releasing buffer: value at offset 80 = %u",
			 *dict_num_terms_ptr);
	}

	UnlockReleaseBuffer(header_buf);

	/* Flush the header to disk to ensure it's persisted */
	FlushRelationBuffers(index);

	/* Truncate relation to reclaim unused space
	 * PostgreSQL pre-extends files to 1GB segments when using P_NEW,
	 * but we only use a small fraction of that space.
	 */
	if (writer.pages_allocated > 0)
	{
		BlockNumber highest_block = 0;
		BlockNumber new_nblocks;
		BlockNumber current_nblocks;
		uint32		i;

		/* Find the highest block number we actually used */
		for (i = 0; i < writer.pages_allocated; i++)
		{
			if (writer.pages[i] > highest_block)
				highest_block = writer.pages[i];
		}

		/* Also check the page index root block */
		if (page_index_root != InvalidBlockNumber &&
			page_index_root > highest_block)
			highest_block = page_index_root;

		/* Truncate to one block after the highest used block */
		new_nblocks		= highest_block + 1;
		current_nblocks = RelationGetNumberOfBlocks(index);

		if (new_nblocks < current_nblocks)
		{
			elog(DEBUG1,
				 "Truncating relation from %u blocks to %u blocks (saving "
				 "%.2fMB)",
				 current_nblocks,
				 new_nblocks,
				 (float)(current_nblocks - new_nblocks) * BLCKSZ /
						 (1024.0 * 1024.0));

			RelationTruncate(index, new_nblocks);
		}
	}

	/* Clean up */
	tp_free_dictionary(terms, num_terms);
	pfree(string_offsets);
	if (writer.pages)
		pfree(writer.pages);

	elog(DEBUG1,
		 "Wrote segment with %u terms to block %u",
		 num_terms,
		 header_block);

	return header_block;
}

/*
 * Debug function to dump segment contents
 */
void
tp_debug_dump_segment_internal(
		const char *index_name, BlockNumber segment_root)
{
	Relation		index;
	TpSegmentHeader header;
	Buffer			header_buf;
	Page			header_page;
	Oid				index_oid;
	Oid				namespace_oid;
	uint32			terms_to_show;

	if (segment_root == InvalidBlockNumber)
	{
		elog(INFO, "Segment for index '%s' is empty", index_name);
		return;
	}

	/* Find the index OID */
	namespace_oid = LookupNamespaceNoError("public");
	if (namespace_oid == InvalidOid)
		elog(ERROR, "Schema 'public' not found");

	index_oid = get_relname_relid(index_name, namespace_oid);
	if (index_oid == InvalidOid)
		elog(ERROR, "Index '%s' not found", index_name);

	/* Open the index */
	index = index_open(index_oid, AccessShareLock);

	/* Read the header from the first page */
	header_buf = ReadBuffer(index, segment_root);
	LockBuffer(header_buf, BUFFER_LOCK_SHARE);
	header_page = BufferGetPage(header_buf);

	/* Copy header from page */
	memcpy(&header,
		   (char *)header_page + SizeOfPageHeaderData,
		   sizeof(TpSegmentHeader));

	/* Debug: log what we just read */
	elog(DEBUG1,
		 "tp_segment_open: Read header from block %u - magic=0x%08X, "
		 "dictionary_offset=%u, strings_offset=%u, entries_offset=%u",
		 segment_root,
		 header.magic,
		 header.dictionary_offset,
		 header.strings_offset,
		 header.entries_offset);

	UnlockReleaseBuffer(header_buf);

	/* Dump header info */
	elog(INFO, "========== Segment Dump: %s ==========", index_name);
	elog(INFO, "Root block: %u", segment_root);
	elog(INFO,
		 "Magic: 0x%08X (expected 0x%08X)",
		 header.magic,
		 TP_SEGMENT_MAGIC);
	elog(INFO, "Version: %u", header.version);
	elog(INFO, "Pages: %u", header.num_pages);
	elog(INFO, "Data size: %u bytes", header.data_size);
	elog(INFO, "Level: %u", header.level);
	elog(INFO, "Page index: block %u", header.page_index);

	/* Corpus statistics */
	elog(INFO, "Terms: %u", header.num_terms);
	elog(INFO, "Docs: %u", header.num_docs);
	elog(INFO, "Total tokens: " UINT64_FORMAT, header.total_tokens);

	/* Section offsets */
	elog(INFO, "Dictionary offset: %u", header.dictionary_offset);
	elog(INFO, "Strings offset: %u", header.strings_offset);
	elog(INFO, "Entries offset: %u", header.entries_offset);
	elog(INFO, "Postings offset: %u", header.postings_offset);
	elog(INFO, "Doc lengths offset: %u", header.doc_lengths_offset);

	/* If we have terms, dump the dictionary */
	if (header.num_terms > 0 && header.dictionary_offset > 0)
	{
		TpSegmentReader *reader;
		TpDictionary	 dict_header;
		uint32			*string_offsets;
		uint32			 i;

		elog(INFO, "");
		elog(INFO,
			 "Dictionary section at offset %u:",
			 header.dictionary_offset);

		/* Open segment reader */
		reader = tp_segment_open(index, segment_root);

		/* Read dictionary header */
		elog(DEBUG1,
			 "Reading dictionary header from offset %u (segment root block "
			 "%u)",
			 header.dictionary_offset,
			 segment_root);
		tp_segment_read(
				reader,
				header.dictionary_offset,
				&dict_header,
				sizeof(dict_header.num_terms));
		elog(DEBUG1,
			 "Read dictionary header with num_terms=%u",
			 dict_header.num_terms);
		elog(INFO, "Dictionary Terms (%u):", dict_header.num_terms);

		/* Read string offsets */
		string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
		tp_segment_read(
				reader,
				header.dictionary_offset + sizeof(dict_header.num_terms),
				string_offsets,
				sizeof(uint32) * dict_header.num_terms);

		/* Show the first few terms */
		terms_to_show = Min(header.num_terms, 20);

		for (i = 0; i < terms_to_show; i++)
		{
			char	   *term_text;
			TpDictEntry dict_entry;
			uint32 string_offset = header.strings_offset + string_offsets[i];

			elog(DEBUG1,
				 "Reading term %u: string_offset=%u (base=%u + offset=%u)",
				 i,
				 string_offset,
				 header.strings_offset,
				 string_offsets[i]);

			/* Read term at this index */
			term_text = read_term_at_index(reader, &header, i, string_offsets);
			elog(DEBUG1, "  String length: %u", (uint32)strlen(term_text));

			/* Validate string length */
			if (strlen(term_text) > 1024) /* Sanity check */
			{
				elog(ERROR,
					 "Invalid string length %u for term %u",
					 (uint32)strlen(term_text),
					 i);
			}

			elog(DEBUG1, "  Term text: '%s'", term_text);

			/* Read dictionary entry for this term */
			read_dict_entry(reader, &header, i, &dict_entry);
			elog(DEBUG1,
				 "  Dict entry: docs=%u, postings=%u",
				 dict_entry.doc_freq,
				 dict_entry.posting_count);

			elog(INFO,
				 "  [%u] '%s' (docs=%u, postings=%u)",
				 i,
				 term_text,
				 dict_entry.doc_freq,
				 dict_entry.posting_count);

			pfree(term_text);
		}

		if (header.num_terms > terms_to_show)
		{
			elog(INFO,
				 "  ... and %u more terms",
				 header.num_terms - terms_to_show);
		}

		pfree(string_offsets);
		tp_segment_close(reader);
	}

	elog(INFO, "========== End Segment Dump ==========");

	index_close(index, AccessShareLock);
}

/*
 * Dump segment contents to a text file for debugging
 * This writes directly to a file to bypass PostgreSQL logging
 */
void
tp_segment_dump_to_file(
		const char *index_name, BlockNumber segment_root, const char *filename)
{
	FILE		   *fp;
	Relation		index;
	TpSegmentHeader header;
	Buffer			header_buf;
	Page			header_page;
	Oid				index_oid;
	Oid				namespace_oid;
	uint32			terms_to_show;

	/* Open the output file */
	fp = fopen(filename, "a");
	if (!fp)
	{
		elog(ERROR, "Could not open file %s for writing: %m", filename);
		return;
	}

	fprintf(fp, "\n\n========================================\n");
	fprintf(fp,
			"Segment Dump at %s\n",
			timestamptz_to_str(GetCurrentTimestamp()));
	fprintf(fp, "========================================\n");

	if (segment_root == InvalidBlockNumber)
	{
		fprintf(fp, "Segment for index '%s' is empty\n", index_name);
		fclose(fp);
		return;
	}

	/* Find the index OID - try slack schema first, then public */
	namespace_oid = LookupNamespaceNoError("slack");
	if (namespace_oid != InvalidOid)
	{
		index_oid = get_relname_relid(index_name, namespace_oid);
	}
	else
	{
		index_oid = InvalidOid;
	}

	/* If not found in slack schema, try public */
	if (index_oid == InvalidOid)
	{
		namespace_oid = LookupNamespaceNoError("public");
		if (namespace_oid != InvalidOid)
		{
			index_oid = get_relname_relid(index_name, namespace_oid);
		}
	}

	if (index_oid == InvalidOid)
	{
		fprintf(fp,
				"ERROR: Index '%s' not found in slack or public schemas\n",
				index_name);
		fclose(fp);
		return;
	}

	/* Open the index */
	index = index_open(index_oid, AccessShareLock);

	/* Read the header from the first page */
	header_buf = ReadBuffer(index, segment_root);
	LockBuffer(header_buf, BUFFER_LOCK_SHARE);
	header_page = BufferGetPage(header_buf);

	/* Copy header from page */
	memcpy(&header,
		   (char *)header_page + SizeOfPageHeaderData,
		   sizeof(TpSegmentHeader));

	/* Dump raw page data for inspection */
	{
		unsigned char *page_data = (unsigned char *)header_page;
		int			   i, j;

		fprintf(fp, "\n=== RAW PAGE DATA (first 512 bytes) ===\n");
		fprintf(fp, "Page address: %p\n", header_page);

		for (i = 0; i < 512 && i < BLCKSZ; i += 16)
		{
			fprintf(fp, "%04x: ", i);
			/* Hex dump */
			for (j = 0; j < 16 && (i + j) < 512; j++)
			{
				fprintf(fp, "%02x ", page_data[i + j]);
			}
			/* ASCII representation */
			fprintf(fp, " |");
			for (j = 0; j < 16 && (i + j) < 512; j++)
			{
				unsigned char c = page_data[i + j];
				fprintf(fp, "%c", (c >= 32 && c < 127) ? c : '.');
			}
			fprintf(fp, "|\n");
		}
	} /* End of hex dump block */

	UnlockReleaseBuffer(header_buf);

	/* Dump header info */
	fprintf(fp, "\n=== SEGMENT HEADER ===\n");
	fprintf(fp, "Index: %s\n", index_name);
	fprintf(fp, "Root block: %u\n", segment_root);
	fprintf(fp,
			"Magic: 0x%08X (expected 0x%08X) %s\n",
			header.magic,
			TP_SEGMENT_MAGIC,
			header.magic == TP_SEGMENT_MAGIC ? "VALID" : "INVALID!");
	fprintf(fp, "Version: %u\n", header.version);
	fprintf(fp, "Pages: %u\n", header.num_pages);
	fprintf(fp, "Data size: %u bytes\n", header.data_size);
	fprintf(fp, "Level: %u\n", header.level);
	fprintf(fp, "Page index: block %u\n", header.page_index);

	/* Corpus statistics */
	fprintf(fp, "\n=== CORPUS STATISTICS ===\n");
	fprintf(fp, "Terms: %u\n", header.num_terms);
	fprintf(fp, "Docs: %u\n", header.num_docs);
	fprintf(fp,
			"Total tokens: %llu\n",
			(unsigned long long)header.total_tokens);

	/* Section offsets */
	fprintf(fp, "\n=== SECTION OFFSETS ===\n");
	fprintf(fp, "Dictionary offset: %u\n", header.dictionary_offset);
	fprintf(fp, "Strings offset: %u\n", header.strings_offset);
	fprintf(fp, "Entries offset: %u\n", header.entries_offset);
	fprintf(fp, "Postings offset: %u\n", header.postings_offset);
	fprintf(fp, "Doc lengths offset: %u\n", header.doc_lengths_offset);

	/* Page layout summary */
	fprintf(fp, "\n=== PAGE LAYOUT ===\n");
	fprintf(fp,
			"Dictionary: pages %u-%u\n",
			header.dictionary_offset / BLCKSZ,
			(header.strings_offset - 1) / BLCKSZ);
	fprintf(fp,
			"Strings:    pages %u-%u\n",
			header.strings_offset / BLCKSZ,
			(header.entries_offset - 1) / BLCKSZ);
	fprintf(fp,
			"Entries:    pages %u-%u\n",
			header.entries_offset / BLCKSZ,
			(header.postings_offset - 1) / BLCKSZ);
	fprintf(fp,
			"Postings:   pages %u-%u\n",
			header.postings_offset / BLCKSZ,
			(header.doc_lengths_offset - 1) / BLCKSZ);
	fprintf(fp,
			"Doc lengths: pages %u-%u\n",
			header.doc_lengths_offset / BLCKSZ,
			(header.data_size - 1) / BLCKSZ);

	/* If we have terms, dump the dictionary */
	if (header.num_terms > 0 && header.dictionary_offset > 0)
	{
		TpSegmentReader *reader;
		TpDictionary	 dict_header;
		uint32			*string_offsets;
		uint32			 i;

		fprintf(fp,
				"\n=== DICTIONARY TERMS (%u total) ===\n",
				header.num_terms);

		/* Validate offsets before trying to read */
		if (header.dictionary_offset >= header.data_size ||
			header.strings_offset >= header.data_size ||
			header.entries_offset >= header.data_size)
		{
			fprintf(fp, "ERROR: Invalid offsets detected:\n");
			fprintf(fp,
					"  dictionary_offset=%u, strings_offset=%u, "
					"entries_offset=%u\n",
					header.dictionary_offset,
					header.strings_offset,
					header.entries_offset);
			fprintf(fp, "  data_size=%u\n", header.data_size);
			fprintf(fp,
					"  Skipping dictionary dump due to invalid offsets.\n");
			fclose(fp);
			index_close(index, NoLock);
			return;
		}

		/* Open segment reader */
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

		/* Show all terms (or first 100 for brevity) */
		terms_to_show = Min(header.num_terms, 100);

		for (i = 0; i < terms_to_show; i++)
		{
			char	   *term_text;
			TpDictEntry dict_entry;
			uint32		string_page;
			uint32		entry_page;
			uint32		posting_page;

			/* Read term at this index */
			term_text = read_term_at_index(reader, &header, i, string_offsets);

			/* Validate string length */
			if (strlen(term_text) > 1024) /* Sanity check */
			{
				fprintf(fp,
						"  [%u] ERROR: Invalid string length %lu\n",
						i,
						strlen(term_text));
				pfree(term_text);
				continue;
			}

			/* Read dictionary entry for this term */
			read_dict_entry(reader, &header, i, &dict_entry);

			/* Calculate which logical pages contain this term's data */
			string_page = (header.strings_offset + string_offsets[i]) / BLCKSZ;
			entry_page	= (header.entries_offset + i * sizeof(TpDictEntry)) /
						 BLCKSZ;
			posting_page = (header.postings_offset +
							dict_entry.posting_offset) /
						   BLCKSZ;

			fprintf(fp,
					"  [%04u] '%-30s' (docs=%4u, postings=%4u)\n",
					i,
					term_text,
					dict_entry.doc_freq,
					dict_entry.posting_count);
			fprintf(fp,
					"         Pages: string[%u] entry[%u] posting[%u] "
					"(offset=%u)\n",
					string_page,
					entry_page,
					posting_page,
					dict_entry.posting_offset);

			/* Dump first few postings for this term */
			if (i < 5 && dict_entry.posting_count > 0)
			{
				TpSegmentPosting *postings;
				uint32 postings_to_show = Min(dict_entry.posting_count, 5);

				postings = palloc(sizeof(TpSegmentPosting) * postings_to_show);
				tp_segment_read(
						reader,
						header.postings_offset + dict_entry.posting_offset,
						postings,
						sizeof(TpSegmentPosting) * postings_to_show);

				fprintf(fp, "         Postings: ");
				for (uint32 j = 0; j < postings_to_show; j++)
				{
					fprintf(fp,
							"(%u,%u):%u ",
							ItemPointerGetBlockNumber(&postings[j].ctid),
							ItemPointerGetOffsetNumber(&postings[j].ctid),
							postings[j].frequency);
				}
				if (dict_entry.posting_count > postings_to_show)
					fprintf(fp,
							"... (%u more)",
							dict_entry.posting_count - postings_to_show);
				fprintf(fp, "\n");

				pfree(postings);
			}

			pfree(term_text);
		}

		if (header.num_terms > terms_to_show)
		{
			fprintf(fp,
					"  ... and %u more terms\n",
					header.num_terms - terms_to_show);
		}

		pfree(string_offsets);
		tp_segment_close(reader);
	}

	fprintf(fp, "\n========== End Segment Dump ==========\n");
	fflush(fp);
	fclose(fp);

	index_close(index, AccessShareLock);

	elog(INFO, "Segment dump written to %s", filename);
}

/* Segment writer helper functions */

void
tp_segment_writer_init(TpSegmentWriter *writer, Relation index)
{
	elog(DEBUG2,
		 "tp_segment_writer_init: Initializing writer for incremental "
		 "allocation");

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

	elog(DEBUG2,
		 "tp_segment_writer_init: Writer initialized with first page, buffer "
		 "at %p",
		 writer->buffer);
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
	elog(DEBUG2,
		 "tp_segment_writer_flush: Flushing buffer_page %u to block %u",
		 writer->buffer_page,
		 writer->pages[writer->buffer_page]);
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
