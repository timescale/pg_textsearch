/*-------------------------------------------------------------------------
 *
 * segment.c
 *	  Segment reader implementation for pg_textsearch
 *
 * IDENTIFICATION
 *	  src/segment.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <access/hash.h>
#include <storage/bufmgr.h>
#include <utils/memutils.h>

#include "memtable.h"
#include "posting.h"
#include "segment.h"
#include "state.h"

/*
 * Open a segment for reading.
 * Only reads the header page containing the page mapping table.
 */
TpSegmentReader *
tp_segment_open(Relation index, BlockNumber root_block)
{
	TpSegmentReader *reader;
	Page			 page;

	/* Allocate reader structure */
	reader						 = palloc0(sizeof(TpSegmentReader));
	reader->index				 = index;
	reader->root_block			 = root_block;
	reader->current_buffer		 = InvalidBuffer;
	reader->current_logical_page = UINT32_MAX;

	/* Read header page with page mapping */
	reader->header_buffer = ReadBuffer(index, root_block);
	LockBuffer(reader->header_buffer, BUFFER_LOCK_SHARE);
	page		   = BufferGetPage(reader->header_buffer);
	reader->header = (TpSegmentHeader *)PageGetContents(page);

	/* Validate segment */
	if (reader->header->magic != TP_SEGMENT_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid segment magic number: %08X expected %08X",
						reader->header->magic,
						TP_SEGMENT_MAGIC)));

	if (reader->header->version != TP_SEGMENT_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("unsupported segment version: %u",
						reader->header->version)));

	LockBuffer(reader->header_buffer, BUFFER_LOCK_UNLOCK);

	return reader;
}

/*
 * Close segment reader and release resources.
 */
void
tp_segment_close(TpSegmentReader *reader)
{
	if (BufferIsValid(reader->current_buffer))
		ReleaseBuffer(reader->current_buffer);

	if (BufferIsValid(reader->header_buffer))
		ReleaseBuffer(reader->header_buffer);

	pfree(reader);
}

/*
 * Get physical block number for logical page.
 * O(1) lookup in page mapping table.
 */
static inline BlockNumber
tp_segment_get_physical_block(TpSegmentReader *reader, uint32 logical_page)
{
	if (logical_page >= reader->header->num_pages)
		elog(ERROR,
			 "logical page %u out of range (max %u)",
			 logical_page,
			 reader->header->num_pages - 1);

	return reader->header->page_map[logical_page];
}

/*
 * Read data from segment at logical offset.
 * Handles page boundaries transparently.
 */
void
tp_segment_read(
		TpSegmentReader *reader, uint32 logical_offset, void *dest, uint32 len)
{
	while (len > 0)
	{
		uint32		logical_page = logical_offset / BLCKSZ;
		uint32		page_offset	 = logical_offset % BLCKSZ;
		uint32		available	 = BLCKSZ - page_offset;
		uint32		to_read		 = Min(len, available);
		BlockNumber physical_block;
		Page		page;

		/* Get physical block from mapping */
		physical_block = tp_segment_get_physical_block(reader, logical_page);

		/* Pin page if not already cached */
		if (!BufferIsValid(reader->current_buffer) ||
			reader->current_logical_page != logical_page)
		{
			if (BufferIsValid(reader->current_buffer))
				ReleaseBuffer(reader->current_buffer);

			reader->current_buffer = ReadBuffer(reader->index, physical_block);
			LockBuffer(reader->current_buffer, BUFFER_LOCK_SHARE);
			reader->current_logical_page = logical_page;
		}

		/* Copy data from page */
		page = BufferGetPage(reader->current_buffer);
		memcpy(dest, PageGetContents(page) + page_offset, to_read);

		LockBuffer(reader->current_buffer, BUFFER_LOCK_UNLOCK);

		/* Advance pointers */
		dest = (char *)dest + to_read;
		logical_offset += to_read;
		len -= to_read;
	}
}

/*
 * Binary search helper for dictionary lookup.
 */
static int
tp_segment_compare_terms(
		TpSegmentReader *reader,
		uint32			 dict_index,
		const char		*search_term,
		uint32			 search_hash)
{
	TpDictEntry entry;
	uint32		entry_offset;
	char	   *stored_term;
	int			result;

	/* Read dictionary entry */
	entry_offset = reader->header->dict_offset +
				   dict_index * sizeof(TpDictEntry);
	tp_segment_read(reader, entry_offset, &entry, sizeof(entry));

	/* Quick hash comparison first */
	if (entry.term_hash < search_hash)
		return -1;
	if (entry.term_hash > search_hash)
		return 1;

	/* Hash matches - compare actual strings */
	stored_term = palloc(entry.string_len + 1);
	tp_segment_read(
			reader,
			reader->header->strings_offset + entry.string_offset,
			stored_term,
			entry.string_len);
	stored_term[entry.string_len] = '\0';

	result = strcmp(stored_term, search_term);
	pfree(stored_term);

	return result;
}

/*
 * Get posting list for a term from segment.
 * Returns NULL if term not found.
 */
TpPostingList *
tp_segment_get_posting_list(TpSegmentReader *reader, const char *term)
{
	uint32 dict_count;
	uint32 term_hash;
	uint32 left, right, mid;
	int	   cmp;

	/* Calculate dictionary size */
	if (reader->header->dict_size == 0)
		return NULL;

	dict_count = reader->header->dict_size / sizeof(TpDictEntry);
	if (dict_count == 0)
		return NULL;

	/* Hash the search term */
	term_hash = hash_any((unsigned char *)term, strlen(term));

	/* Binary search dictionary */
	left  = 0;
	right = dict_count - 1;

	while (left <= right)
	{
		mid = (left + right) / 2;
		cmp = tp_segment_compare_terms(reader, mid, term, term_hash);

		if (cmp == 0)
		{
			/* Found - load posting list */
			TpDictEntry		  entry;
			TpPostingList	 *result;
			TpSegmentPosting *segment_postings;
			TpPostingEntry	 *result_postings;
			uint32			  entry_offset;
			uint32			  posting_size;
			uint32			  i;

			/* Read dictionary entry */
			entry_offset = reader->header->dict_offset +
						   mid * sizeof(TpDictEntry);
			tp_segment_read(reader, entry_offset, &entry, sizeof(entry));

			/* Allocate result posting list */
			result = (TpPostingList *)palloc(
					sizeof(TpPostingList) +
					entry.posting_count * sizeof(TpPostingEntry));
			result->doc_count  = entry.posting_count;
			result->capacity   = entry.posting_count;
			result->is_sorted  = true;
			result->doc_freq   = entry.doc_freq;
			result->entries_dp = InvalidDsaPointer;

			/* Read segment postings */
			posting_size	 = entry.posting_count * sizeof(TpSegmentPosting);
			segment_postings = palloc(posting_size);
			tp_segment_read(
					reader,
					reader->header->postings_offset + entry.posting_offset,
					segment_postings,
					posting_size);

			/* Convert to TpPostingEntry format */
			result_postings = (TpPostingEntry *)((char *)result +
												 sizeof(TpPostingList));
			for (i = 0; i < entry.posting_count; i++)
			{
				result_postings[i].ctid		 = segment_postings[i].ctid;
				result_postings[i].doc_id	 = -1; /* Not used in segments */
				result_postings[i].frequency = segment_postings[i].frequency;
			}

			pfree(segment_postings);
			return result;
		}
		else if (cmp < 0)
		{
			left = mid + 1;
		}
		else
		{
			right = mid - 1;
		}
	}

	/* Not found */
	return NULL;
}

/*
 * tp_segment_get_document_length - Get document length from segment
 *
 * Returns the length of the document with the given ctid, or -1 if not found.
 * Performs a linear search through the document lengths section.
 */
int32
tp_segment_get_document_length(TpSegmentReader *reader, ItemPointer ctid)
{
	TpSegmentHeader *header;
	TpDocLength		*doclens;
	uint32			 num_docs;
	uint32			 i;

	if (!reader || !reader->header)
		return -1;

	header = reader->header;

	/* Check if segment has document lengths */
	if (header->doclens_size == 0)
		return -1;

	/* Calculate number of document length entries */
	num_docs = header->doclens_size / sizeof(TpDocLength);

	/* Allocate buffer for document lengths */
	doclens = (TpDocLength *)palloc(header->doclens_size);

	/* Read document lengths section */
	tp_segment_read(
			reader, header->doclens_offset, doclens, header->doclens_size);

	/* Linear search for matching ctid */
	for (i = 0; i < num_docs; i++)
	{
		if (ItemPointerEquals(&doclens[i].ctid, ctid))
		{
			int32 length = doclens[i].length;
			pfree(doclens);
			return length;
		}
	}

	pfree(doclens);
	return -1; /* Not found */
}
