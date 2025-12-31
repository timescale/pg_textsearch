/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment/source.c - Segment implementation of TpDataSource
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "../source.h"
#include "dictionary.h"
#include "segment.h"
#include "source.h"

/*
 * Segment-specific data source state
 */
typedef struct TpSegmentSource
{
	TpDataSource	 base;	 /* Must be first */
	TpSegmentReader *reader; /* Segment reader */
} TpSegmentSource;

/*
 * Find a term in the segment dictionary using binary search.
 * Returns the dictionary entry index, or UINT32_MAX if not found.
 */
static uint32
segment_find_term(TpSegmentReader *reader, const char *term)
{
	TpSegmentHeader *header = reader->header;
	TpDictionary	 dict_header;
	int				 left, right, mid;
	char			*term_buffer = NULL;
	uint32			 buffer_size = 0;
	uint32			 found_idx	 = UINT32_MAX;

	if (header->num_terms == 0 || header->dictionary_offset == 0)
		return UINT32_MAX;

	/* Read dictionary header */
	tp_segment_read(
			reader,
			header->dictionary_offset,
			&dict_header,
			sizeof(dict_header.num_terms));

	/* Binary search for the term */
	left  = 0;
	right = dict_header.num_terms - 1;

	while (left <= right)
	{
		TpStringEntry string_entry;
		int			  cmp;
		uint32		  string_offset_value;
		uint32		  string_offset;

		mid = left + (right - left) / 2;

		/* Read the string offset for this entry */
		tp_segment_read(
				reader,
				header->dictionary_offset + sizeof(dict_header.num_terms) +
						(mid * sizeof(uint32)),
				&string_offset_value,
				sizeof(uint32));

		string_offset = header->strings_offset + string_offset_value;

		/* Read string length */
		tp_segment_read(
				reader, string_offset, &string_entry.length, sizeof(uint32));

		/* Reallocate buffer if needed */
		if (string_entry.length + 1 > buffer_size)
		{
			if (term_buffer)
				pfree(term_buffer);
			buffer_size = string_entry.length + 1;
			term_buffer = palloc(buffer_size);
		}

		/* Read term text */
		tp_segment_read(
				reader,
				string_offset + sizeof(uint32),
				term_buffer,
				string_entry.length);
		term_buffer[string_entry.length] = '\0';

		cmp = strcmp(term, term_buffer);
		if (cmp == 0)
		{
			found_idx = mid;
			break;
		}
		else if (cmp < 0)
		{
			right = mid - 1;
		}
		else
		{
			left = mid + 1;
		}
	}

	if (term_buffer)
		pfree(term_buffer);

	return found_idx;
}

/*
 * Get posting data for a term from segment (V2 format).
 * Returns columnar data with parallel ctid and frequency arrays.
 */
static TpPostingData *
segment_get_postings(TpDataSource *source, const char *term)
{
	TpSegmentSource *ss		= (TpSegmentSource *)source;
	TpSegmentReader *reader = ss->reader;
	TpSegmentHeader *header = reader->header;
	uint32			 term_idx;
	TpDictEntry		 dict_entry;
	TpPostingData	*data;
	uint32			 total_postings;
	uint32			 current_idx;
	uint32			 block_idx;

	/* Find term in dictionary */
	term_idx = segment_find_term(reader, term);
	if (term_idx == UINT32_MAX)
		return NULL;

	/* Read dictionary entry */
	tp_segment_read(
			reader,
			header->entries_offset + (term_idx * sizeof(TpDictEntry)),
			&dict_entry,
			sizeof(TpDictEntry));

	if (dict_entry.block_count == 0)
		return NULL;

	/* Count total postings across all blocks */
	total_postings = 0;
	for (block_idx = 0; block_idx < dict_entry.block_count; block_idx++)
	{
		TpSkipEntry skip_entry;
		uint32		skip_offset;

		skip_offset = dict_entry.skip_index_offset +
					  (block_idx * sizeof(TpSkipEntry));
		tp_segment_read(reader, skip_offset, &skip_entry, sizeof(TpSkipEntry));
		total_postings += skip_entry.doc_count;
	}

	if (total_postings == 0)
		return NULL;

	/* Allocate columnar data */
	data		   = tp_alloc_posting_data(total_postings);
	data->count	   = total_postings;
	data->doc_freq = dict_entry.doc_freq;

	/* Read postings from all blocks */
	current_idx = 0;
	for (block_idx = 0; block_idx < dict_entry.block_count; block_idx++)
	{
		TpSkipEntry		skip_entry;
		uint32			skip_offset;
		uint32			block_bytes;
		TpBlockPosting *block_postings;
		uint32			i;

		/* Read skip entry */
		skip_offset = dict_entry.skip_index_offset +
					  (block_idx * sizeof(TpSkipEntry));
		tp_segment_read(reader, skip_offset, &skip_entry, sizeof(TpSkipEntry));

		if (skip_entry.doc_count == 0)
			continue;

		/* Allocate and read block postings */
		block_bytes	   = skip_entry.doc_count * sizeof(TpBlockPosting);
		block_postings = palloc(block_bytes);
		tp_segment_read(
				reader,
				skip_entry.posting_offset,
				block_postings,
				block_bytes);

		/* Convert block postings to columnar format */
		for (i = 0; i < skip_entry.doc_count; i++)
		{
			TpBlockPosting *bp	   = &block_postings[i];
			uint32			doc_id = bp->doc_id;

			/* Convert doc_id to CTID using cached arrays */
			if (reader->cached_ctid_pages && reader->cached_ctid_offsets &&
				doc_id < reader->cached_num_docs)
			{
				ItemPointerSetBlockNumber(
						&data->ctids[current_idx],
						reader->cached_ctid_pages[doc_id]);
				ItemPointerSetOffsetNumber(
						&data->ctids[current_idx],
						reader->cached_ctid_offsets[doc_id]);
			}
			else
			{
				/* Fallback: invalid CTID if lookup not available */
				ItemPointerSetInvalid(&data->ctids[current_idx]);
			}

			data->frequencies[current_idx] = bp->frequency;
			current_idx++;
		}

		pfree(block_postings);
	}

	data->count = current_idx;
	return data;
}

/*
 * Free posting data - just use the common helper.
 */
static void
segment_free_postings(TpDataSource *source, TpPostingData *data)
{
	(void)source; /* unused */
	tp_free_posting_data(data);
}

/*
 * Get document length for a CTID from segment.
 * Uses the fieldnorm table to get quantized length, then dequantizes.
 */
static int32
segment_get_doc_length(TpDataSource *source, ItemPointer ctid)
{
	TpSegmentSource *ss		= (TpSegmentSource *)source;
	TpSegmentReader *reader = ss->reader;
	TpSegmentHeader *header = reader->header;
	uint32			 doc_id;
	uint8			 fieldnorm;
	uint32			 i;

	/* Search for the doc_id corresponding to this CTID */
	if (!reader->cached_ctid_pages || !reader->cached_ctid_offsets)
		return -1;

	/* Linear search for CTID - could be optimized with hash */
	for (i = 0; i < reader->cached_num_docs; i++)
	{
		if (reader->cached_ctid_pages[i] == ItemPointerGetBlockNumber(ctid) &&
			reader->cached_ctid_offsets[i] == ItemPointerGetOffsetNumber(ctid))
		{
			doc_id = i;
			goto found;
		}
	}
	return -1;

found:
	/* Read fieldnorm for this doc_id */
	if (header->fieldnorm_offset == 0)
		return -1;

	tp_segment_read(
			reader,
			header->fieldnorm_offset + doc_id,
			&fieldnorm,
			sizeof(uint8));

	/* Dequantize fieldnorm to approximate document length */
	/* Using Lucene's SmallFloat format: value = (1 + mantissa/8) * 2^exponent
	 */
	if (fieldnorm == 0)
		return 0;
	else
	{
		uint8 exp = (fieldnorm >> 3) & 0x1F;
		uint8 man = fieldnorm & 0x07;
		return (int32)((1.0 + man / 8.0) * (1 << exp));
	}
}

/*
 * Close the segment source and free resources.
 */
static void
segment_close(TpDataSource *source)
{
	TpSegmentSource *ss = (TpSegmentSource *)source;

	if (ss->reader)
		tp_segment_close(ss->reader);

	pfree(ss);
}

/* Virtual function table for segment source */
static const TpDataSourceOps segment_source_ops = {
		.get_postings	= segment_get_postings,
		.free_postings	= segment_free_postings,
		.get_doc_length = segment_get_doc_length,
		.close			= segment_close,
};

/*
 * Create a data source that reads from a single segment.
 */
TpDataSource *
tp_segment_source_create(Relation index, BlockNumber segment_root)
{
	TpSegmentSource *ss;
	TpSegmentReader *reader;

	Assert(index != NULL);
	Assert(segment_root != InvalidBlockNumber);

	reader = tp_segment_open(index, segment_root);
	if (!reader)
		return NULL;

	ss					= (TpSegmentSource *)palloc0(sizeof(TpSegmentSource));
	ss->base.ops		= &segment_source_ops;
	ss->base.total_docs = reader->header->num_docs;
	ss->base.total_len	= reader->header->total_tokens;
	ss->reader			= reader;

	return (TpDataSource *)ss;
}
