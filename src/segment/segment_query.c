/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment_query.c - Zero-copy query execution for segments
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "memtable/posting.h"
#include "operator.h"
#include "segment/dictionary.h"
#include "segment/fieldnorm.h"
#include "segment/segment.h"
#include "state.h"

/*
 * Iterator state for zero-copy segment posting traversal (V1 format)
 */
typedef struct TpSegmentPostingIterator
{
	TpSegmentReader *reader;
	const char		*term;
	uint32			 dict_entry_idx;
	TpDictEntry		 dict_entry;
	uint32			 current_posting;
	uint32			 postings_offset;
	bool			 initialized;
	bool			 finished;
	/* Track current direct access to release buffer properly */
	TpSegmentDirectAccess current_access;
	bool				  has_active_access;
	/* Fallback buffer for when direct access fails */
	TpSegmentPosting fallback_posting;
} TpSegmentPostingIterator;

/*
 * Iterator state for V2 block-based segment traversal
 */
typedef struct TpSegmentPostingIteratorV2
{
	TpSegmentReader *reader;
	const char		*term;
	uint32			 dict_entry_idx;
	TpDictEntryV2	 dict_entry;
	bool			 initialized;
	bool			 finished;

	/* Block iteration state */
	uint32		current_block; /* Current block index (0 to block_count-1) */
	uint32		current_in_block; /* Position within current block */
	TpSkipEntry skip_entry;		  /* Current block's skip entry */

	/* Cached posting data for current block */
	TpBlockPosting *block_postings;
	uint32			block_postings_size;

	/* Output posting (converted to V1-style for compatibility) */
	TpSegmentPosting output_posting;
} TpSegmentPostingIteratorV2;

/*
 * Initialize iterator for a specific term in a segment
 * Returns true if term found, false otherwise
 */
static bool
tp_segment_posting_iterator_init(
		TpSegmentPostingIterator *iter,
		TpSegmentReader			 *reader,
		const char				 *term)
{
	TpSegmentHeader *header = reader->header;
	TpDictionary	 dict_header;
	int				 left, right, mid;
	char			*term_buffer = NULL;
	uint32			 buffer_size = 0;

	iter->reader			= reader;
	iter->term				= term;
	iter->current_posting	= 0;
	iter->initialized		= false;
	iter->finished			= true; /* Default to finished if not found */
	iter->has_active_access = false;

	/* Initialize the direct access structure to prevent garbage values */
	iter->current_access.buffer	   = InvalidBuffer;
	iter->current_access.page	   = NULL;
	iter->current_access.data	   = NULL;
	iter->current_access.available = 0;

	if (header->num_terms == 0 || header->dictionary_offset == 0)
		return false;

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

		/* Read just the single string offset we need for this iteration */
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

		/* Compare terms */
		cmp = strcmp(term, term_buffer);

		if (cmp == 0)
		{
			/* Found it! Read the dictionary entry */
			tp_segment_read(
					reader,
					header->entries_offset + (mid * sizeof(TpDictEntry)),
					&iter->dict_entry,
					sizeof(TpDictEntry));

			iter->dict_entry_idx = mid;
			/*
			 * dict_entry.posting_offset is already an absolute offset (set
			 * during segment write as header.postings_offset + relative_off)
			 */
			iter->postings_offset = iter->dict_entry.posting_offset;
			iter->initialized	  = true;
			iter->finished		  = (iter->dict_entry.posting_count == 0);

			if (term_buffer)
				pfree(term_buffer);
			return true;
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
	return false;
}

/*
 * Get next posting from iterator using zero-copy access
 * Returns false when no more postings
 */
static bool
tp_segment_posting_iterator_next(
		TpSegmentPostingIterator *iter, TpSegmentPosting **posting)
{
	uint32 offset;

	if (iter->finished || !iter->initialized)
		return false;

	if (iter->current_posting >= iter->dict_entry.posting_count)
	{
		/* Release any active buffer before marking as finished */
		if (iter->has_active_access)
		{
			tp_segment_release_direct(&iter->current_access);
			iter->has_active_access = false;
		}
		iter->finished = true;
		return false;
	}

	/* Release previous buffer if we have one */
	if (iter->has_active_access)
	{
		tp_segment_release_direct(&iter->current_access);
		iter->has_active_access = false;
	}

	/* Calculate offset for current posting */
	offset = iter->postings_offset +
			 (iter->current_posting * sizeof(TpSegmentPosting));

	/* Get direct access to the posting */
	if (!tp_segment_get_direct(
				iter->reader,
				offset,
				sizeof(TpSegmentPosting),
				&iter->current_access))
	{
		/* Fallback to regular read if direct access fails */
		tp_segment_read(
				iter->reader,
				offset,
				&iter->fallback_posting,
				sizeof(TpSegmentPosting));
		*posting = &iter->fallback_posting;
	}
	else
	{
		/* Zero-copy: return pointer directly into the page */
		*posting = (TpSegmentPosting *)iter->current_access.data;
		iter->has_active_access = true;
	}

	iter->current_posting++;
	return true;
}

/*
 * V2 Iterator Functions - Block-based posting traversal
 */

/*
 * Initialize V2 iterator for a specific term in a segment.
 * Returns true if term found, false otherwise.
 */
static bool
tp_segment_posting_iterator_init_v2(
		TpSegmentPostingIteratorV2 *iter,
		TpSegmentReader			   *reader,
		const char				   *term)
{
	TpSegmentHeader *header = reader->header;
	TpDictionary	 dict_header;
	int				 left, right, mid;
	char			*term_buffer = NULL;
	uint32			 buffer_size = 0;

	iter->reader			  = reader;
	iter->term				  = term;
	iter->current_block		  = 0;
	iter->current_in_block	  = 0;
	iter->initialized		  = false;
	iter->finished			  = true;
	iter->block_postings	  = NULL;
	iter->block_postings_size = 0;

	if (header->num_terms == 0 || header->dictionary_offset == 0)
		return false;

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

		/* Read string offset */
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

		/* Compare terms */
		cmp = strcmp(term, term_buffer);

		if (cmp == 0)
		{
			/* Found! Read V2 dictionary entry */
			tp_segment_read(
					reader,
					header->entries_offset + (mid * sizeof(TpDictEntryV2)),
					&iter->dict_entry,
					sizeof(TpDictEntryV2));

			iter->dict_entry_idx = mid;
			iter->initialized	 = true;
			iter->finished		 = (iter->dict_entry.block_count == 0);

			if (term_buffer)
				pfree(term_buffer);
			return true;
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
	return false;
}

/*
 * Load a block's postings into the iterator's buffer.
 */
static bool
tp_segment_posting_iterator_load_block_v2(TpSegmentPostingIteratorV2 *iter)
{
	uint32 skip_offset;
	uint32 block_size;

	if (iter->current_block >= iter->dict_entry.block_count)
		return false;

	/* Read skip entry for current block */
	skip_offset = iter->dict_entry.skip_index_offset +
				  (iter->current_block * sizeof(TpSkipEntry));
	tp_segment_read(
			iter->reader, skip_offset, &iter->skip_entry, sizeof(TpSkipEntry));

	/* Allocate or reallocate block buffer */
	block_size = iter->skip_entry.doc_count;
	if (block_size > iter->block_postings_size)
	{
		if (iter->block_postings)
			pfree(iter->block_postings);
		iter->block_postings = palloc(block_size * sizeof(TpBlockPosting));
		iter->block_postings_size = block_size;
	}

	/* Read posting data for this block */
	tp_segment_read(
			iter->reader,
			iter->skip_entry.posting_offset,
			iter->block_postings,
			block_size * sizeof(TpBlockPosting));

	iter->current_in_block = 0;
	return true;
}

/*
 * Get next posting from V2 iterator.
 * Converts block posting to V1-style TpSegmentPosting for compatibility.
 * Returns false when no more postings.
 */
static bool
tp_segment_posting_iterator_next_v2(
		TpSegmentPostingIteratorV2 *iter, TpSegmentPosting **posting)
{
	TpSegmentHeader *header;
	TpBlockPosting	*bp;
	uint32			 doc_id;
	TpCtidMapEntry	 ctid_entry;

	if (iter->finished || !iter->initialized)
		return false;

	header = iter->reader->header;

	/* Load first block if needed */
	if (iter->block_postings == NULL)
	{
		if (!tp_segment_posting_iterator_load_block_v2(iter))
		{
			iter->finished = true;
			return false;
		}
	}

	/* Move to next block if current is exhausted */
	while (iter->current_in_block >= iter->skip_entry.doc_count)
	{
		iter->current_block++;
		if (iter->current_block >= iter->dict_entry.block_count)
		{
			iter->finished = true;
			return false;
		}
		if (!tp_segment_posting_iterator_load_block_v2(iter))
		{
			iter->finished = true;
			return false;
		}
	}

	/* Get current posting from block */
	bp	   = &iter->block_postings[iter->current_in_block];
	doc_id = bp->doc_id;

	/*
	 * Look up CTID from cached array (fast path) or fall back to per-posting
	 * read. Fieldnorm is inline in block posting, no lookup needed.
	 */
	if (iter->reader->cached_ctids && doc_id < iter->reader->cached_num_docs)
	{
		/* Fast path: use cached CTID array */
		iter->output_posting.ctid = iter->reader->cached_ctids[doc_id];
	}
	else
	{
		/* Slow path: per-posting CTID read */
		tp_segment_read(
				iter->reader,
				header->ctid_map_offset + (doc_id * sizeof(ItemPointerData)),
				&ctid_entry,
				sizeof(TpCtidMapEntry));
		iter->output_posting.ctid = ctid_entry.ctid;
	}

	/* Build output posting in V1 format (fieldnorm is inline in bp) */
	iter->output_posting.frequency	= bp->frequency;
	iter->output_posting.doc_length = (uint16)decode_fieldnorm(bp->fieldnorm);

	*posting = &iter->output_posting;
	iter->current_in_block++;
	return true;
}

/*
 * Free V2 iterator resources.
 */
static void
tp_segment_posting_iterator_free_v2(TpSegmentPostingIteratorV2 *iter)
{
	if (iter->block_postings)
	{
		pfree(iter->block_postings);
		iter->block_postings = NULL;
	}
}

/*
 * Helper: Process a single posting and add score to hash table.
 */
static void
process_posting(
		TpSegmentPosting *posting,
		float4			  idf,
		float4			  query_frequency,
		float4			  k1,
		float4			  b,
		float4			  avg_doc_len,
		HTAB			 *hash_table)
{
	float4				tf;
	float4				doc_len;
	float4				term_score;
	double				numerator_d, denominator_d;
	DocumentScoreEntry *doc_entry;
	bool				found;
	ItemPointerData		local_ctid;

	if (posting == NULL)
	{
		elog(ERROR, "tp_process_term_in_segments: posting is NULL!");
	}

	tf = posting->frequency;

	/* Copy ctid to avoid packed member alignment issues */
	local_ctid = posting->ctid;

	/* Use inline document length from posting entry */
	doc_len = (float4)posting->doc_length;

	if (doc_len <= 0.0f)
		return; /* Skip if doc_length is invalid */

	/* Validate TID */
	if (!ItemPointerIsValid(&local_ctid))
		return;

	/* Calculate BM25 term score */
	numerator_d	  = (double)tf * ((double)k1 + 1.0);
	denominator_d = (double)tf +
					(double)k1 * (1.0 - (double)b +
								  (double)b * ((double)doc_len /
											   (double)avg_doc_len));
	term_score = (float4)((double)idf * (numerator_d / denominator_d) *
						  (double)query_frequency);

	/* Add or update document score in hash table */
	doc_entry = (DocumentScoreEntry *)
			hash_search(hash_table, &local_ctid, HASH_ENTER, &found);
	if (!found)
	{
		doc_entry->ctid		  = local_ctid;
		doc_entry->score	  = term_score;
		doc_entry->doc_length = doc_len;
	}
	else
	{
		doc_entry->score += term_score;
	}
}

/*
 * Score documents matching a term across all segments.
 * IDF is pre-computed using unified doc_freq from memtable + segments.
 * Handles both V1 (flat) and V2 (block-based) segment formats.
 */
void
tp_process_term_in_segments(
		Relation					   index,
		BlockNumber					   first_segment,
		const char					  *term,
		float4						   idf,
		float4						   query_frequency,
		float4						   k1,
		float4						   b,
		float4						   avg_doc_len,
		void						  *doc_scores_hash,
		TpLocalIndexState *local_state pg_attribute_unused())
{
	BlockNumber		 current	= first_segment;
	TpSegmentReader *reader		= NULL;
	HTAB			*hash_table = (HTAB *)doc_scores_hash;

	while (current != InvalidBlockNumber)
	{
		TpSegmentHeader *header;

		/* Open segment */
		reader = tp_segment_open(index, current);
		if (!reader)
			break;

		header = reader->header;

		/* Check segment version and use appropriate iterator */
		if (header->version >= TP_SEGMENT_FORMAT_V2)
		{
			/* V2 block-based format */
			TpSegmentPostingIteratorV2 iter_v2;
			TpSegmentPosting		  *posting;

			if (tp_segment_posting_iterator_init_v2(&iter_v2, reader, term))
			{
				while (tp_segment_posting_iterator_next_v2(&iter_v2, &posting))
				{
					process_posting(
							posting,
							idf,
							query_frequency,
							k1,
							b,
							avg_doc_len,
							hash_table);
				}
				tp_segment_posting_iterator_free_v2(&iter_v2);
			}
		}
		else
		{
			/* V1 flat format */
			TpSegmentPostingIterator iter;
			TpSegmentPosting		*posting;

			if (tp_segment_posting_iterator_init(&iter, reader, term))
			{
				while (tp_segment_posting_iterator_next(&iter, &posting))
				{
					process_posting(
							posting,
							idf,
							query_frequency,
							k1,
							b,
							avg_doc_len,
							hash_table);
				}

				/* Release any active buffer when done */
				if (iter.has_active_access)
				{
					tp_segment_release_direct(&iter.current_access);
					iter.has_active_access = false;
				}
			}
		}

		/* Get next segment from header */
		current = header->next_segment;

		/* Close this segment */
		tp_segment_close(reader);
	}
}

/*
 * Sum doc_freq for a term across all segments.
 */
uint32
tp_segment_get_doc_freq(
		Relation index, BlockNumber first_segment, const char *term)
{
	BlockNumber		 current	 = first_segment;
	TpSegmentReader *reader		 = NULL;
	uint32			 doc_freq	 = 0;
	char			*term_buffer = NULL;
	uint32			 buffer_size = 0;

	while (current != InvalidBlockNumber)
	{
		TpSegmentHeader *header;
		TpDictionary	 dict_header;
		int				 left, right;

		reader = tp_segment_open(index, current);
		if (!reader)
			break;

		header = reader->header;
		if (header->num_terms == 0 || header->dictionary_offset == 0)
		{
			current = header->next_segment;
			tp_segment_close(reader);
			continue;
		}

		tp_segment_read(
				reader,
				header->dictionary_offset,
				&dict_header,
				sizeof(dict_header.num_terms));

		left  = 0;
		right = dict_header.num_terms - 1;

		while (left <= right)
		{
			TpStringEntry string_entry;
			int			  cmp;
			uint32		  string_offset_value;
			uint32		  string_offset;
			int			  mid = left + (right - left) / 2;

			tp_segment_read(
					reader,
					header->dictionary_offset + sizeof(dict_header.num_terms) +
							(mid * sizeof(uint32)),
					&string_offset_value,
					sizeof(uint32));

			string_offset = header->strings_offset + string_offset_value;

			tp_segment_read(
					reader,
					string_offset,
					&string_entry.length,
					sizeof(uint32));

			if (string_entry.length + 1 > buffer_size)
			{
				if (term_buffer)
					pfree(term_buffer);
				buffer_size = string_entry.length + 1;
				term_buffer = palloc(buffer_size);
			}

			tp_segment_read(
					reader,
					string_offset + sizeof(uint32),
					term_buffer,
					string_entry.length);
			term_buffer[string_entry.length] = '\0';

			cmp = strcmp(term, term_buffer);

			if (cmp == 0)
			{
				/* Read dict entry based on segment version */
				if (header->version >= TP_SEGMENT_FORMAT_V2)
				{
					TpDictEntryV2 dict_entry_v2;
					tp_segment_read(
							reader,
							header->entries_offset +
									(mid * sizeof(TpDictEntryV2)),
							&dict_entry_v2,
							sizeof(TpDictEntryV2));
					doc_freq += dict_entry_v2.doc_freq;
				}
				else
				{
					TpDictEntry dict_entry;
					tp_segment_read(
							reader,
							header->entries_offset +
									(mid * sizeof(TpDictEntry)),
							&dict_entry,
							sizeof(TpDictEntry));
					doc_freq += dict_entry.doc_freq;
				}
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

		current = header->next_segment;
		tp_segment_close(reader);
	}

	if (term_buffer)
		pfree(term_buffer);

	return doc_freq;
}

/*
 * Score all query terms across a chain of segments efficiently.
 *
 * This function opens each segment ONCE and processes ALL terms, avoiding
 * the O(terms * segments) segment opens of the naive approach.
 *
 * For each segment:
 *   1. Open segment
 *   2. For each term: look up dictionary entry (get doc_freq) and score
 * postings
 *   3. Close segment
 *
 * The doc_freqs array is filled in with the sum of doc_freq across all
 * segments. Scores are accumulated into the doc_scores_hash.
 */
void
tp_score_all_terms_in_segment_chain(
		Relation	index,
		BlockNumber first_segment,
		char	  **terms,
		int			term_count,
		int32	   *query_frequencies,
		uint32	   *doc_freqs, /* OUT: filled with segment doc_freqs */
		int32		total_docs,
		float4		k1,
		float4		b,
		float4		avg_doc_len,
		void	   *doc_scores_hash)
{
	BlockNumber current		= first_segment;
	char	   *term_buffer = NULL;
	uint32		buffer_size = 0;
	HTAB	   *hash_table	= (HTAB *)doc_scores_hash;

	while (current != InvalidBlockNumber)
	{
		TpSegmentReader *reader;
		TpSegmentHeader *header;

		/* Open segment ONCE for all terms */
		reader = tp_segment_open(index, current);
		if (!reader)
			break;

		header = reader->header;

		/* Process each term in this segment */
		for (int term_idx = 0; term_idx < term_count; term_idx++)
		{
			const char	*term = terms[term_idx];
			TpDictionary dict_header;
			int			 left, right, cmp;
			bool		 found			= false;
			uint32		 dict_entry_idx = 0;

			if (header->num_terms == 0 || header->dictionary_offset == 0)
				continue;

			/* Read dictionary header */
			tp_segment_read(
					reader,
					header->dictionary_offset,
					&dict_header,
					sizeof(dict_header.num_terms));

			/* Binary search for term in dictionary */
			left  = 0;
			right = header->num_terms - 1;

			while (left <= right)
			{
				int	   mid = left + (right - left) / 2;
				uint32 string_offset_value;
				uint32 string_offset;

				struct
				{
					uint32 length;
				} string_entry;

				tp_segment_read(
						reader,
						header->dictionary_offset +
								sizeof(dict_header.num_terms) +
								(mid * sizeof(uint32)),
						&string_offset_value,
						sizeof(uint32));

				string_offset = header->strings_offset + string_offset_value;

				tp_segment_read(
						reader,
						string_offset,
						&string_entry.length,
						sizeof(uint32));

				if (string_entry.length + 1 > buffer_size)
				{
					if (term_buffer)
						pfree(term_buffer);
					buffer_size = string_entry.length + 1;
					term_buffer = palloc(buffer_size);
				}

				tp_segment_read(
						reader,
						string_offset + sizeof(uint32),
						term_buffer,
						string_entry.length);
				term_buffer[string_entry.length] = '\0';

				cmp = strcmp(term, term_buffer);

				if (cmp == 0)
				{
					found		   = true;
					dict_entry_idx = mid;
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

			if (!found)
				continue;

			/* Found the term - get doc_freq and process postings */
			if (header->version >= TP_SEGMENT_FORMAT_V2)
			{
				TpDictEntryV2			   dict_entry_v2;
				TpSegmentPostingIteratorV2 iter_v2;
				TpSegmentPosting		  *posting;
				float4					   idf;

				tp_segment_read(
						reader,
						header->entries_offset +
								(dict_entry_idx * sizeof(TpDictEntryV2)),
						&dict_entry_v2,
						sizeof(TpDictEntryV2));

				/* Accumulate doc_freq */
				doc_freqs[term_idx] += dict_entry_v2.doc_freq;

				/* Calculate IDF with current accumulated doc_freq */
				idf = tp_calculate_idf(doc_freqs[term_idx], total_docs);

				/* Initialize iterator directly with dict entry */
				iter_v2.reader					 = reader;
				iter_v2.term					 = term;
				iter_v2.dict_entry_idx			 = dict_entry_idx;
				iter_v2.dict_entry				 = dict_entry_v2;
				iter_v2.initialized				 = true;
				iter_v2.finished				 = false;
				iter_v2.current_block			 = 0;
				iter_v2.current_posting_in_block = 0;
				iter_v2.block_postings			 = NULL;
				iter_v2.block_posting_count		 = 0;

				/* Process all postings for this term */
				while (tp_segment_posting_iterator_next_v2(&iter_v2, &posting))
				{
					process_posting(
							posting,
							idf,
							(float4)query_frequencies[term_idx],
							k1,
							b,
							avg_doc_len,
							hash_table);
				}
				tp_segment_posting_iterator_free_v2(&iter_v2);
			}
			else
			{
				/* V1 format */
				TpDictEntry				 dict_entry;
				TpSegmentPostingIterator iter;
				TpSegmentPosting		*posting;
				float4					 idf;

				tp_segment_read(
						reader,
						header->entries_offset +
								(dict_entry_idx * sizeof(TpDictEntry)),
						&dict_entry,
						sizeof(TpDictEntry));

				/* Accumulate doc_freq */
				doc_freqs[term_idx] += dict_entry.doc_freq;

				/* Calculate IDF with current accumulated doc_freq */
				idf = tp_calculate_idf(doc_freqs[term_idx], total_docs);

				/* Initialize iterator directly */
				if (tp_segment_posting_iterator_init(&iter, reader, term))
				{
					while (tp_segment_posting_iterator_next(&iter, &posting))
					{
						process_posting(
								posting,
								idf,
								(float4)query_frequencies[term_idx],
								k1,
								b,
								avg_doc_len,
								hash_table);
					}

					if (iter.has_active_access)
					{
						tp_segment_release_direct(&iter.current_access);
						iter.has_active_access = false;
					}
				}
			}
		}

		/* Move to next segment and close this one */
		current = header->next_segment;
		tp_segment_close(reader);
	}

	if (term_buffer)
		pfree(term_buffer);
}
