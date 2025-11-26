/*-------------------------------------------------------------------------
 *
 * segment_query.c
 *    Zero-copy query execution for segments
 *
 * This file implements direct, allocation-free query processing for
 * segments, allowing queries to iterate through posting lists without
 * copying data.
 *
 * IDENTIFICATION
 *    src/segment/segment_query.c
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "memtable/posting.h"
#include "operator.h"
#include "segment/dictionary.h"
#include "segment/segment.h"
#include "state.h"

/*
 * Iterator state for zero-copy segment posting traversal
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
 * Score documents matching a term across all segments.
 * IDF is pre-computed using unified doc_freq from memtable + segments.
 */
void
tp_process_term_in_segments(
		Relation		   index,
		BlockNumber		   first_segment,
		const char		  *term,
		float4			   idf,
		float4			   query_frequency,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		void			  *doc_scores_hash,
		TpLocalIndexState *local_state)
{
	BlockNumber				 current = first_segment;
	TpSegmentReader			*reader	 = NULL;
	TpSegmentPostingIterator iter;
	TpSegmentPosting		*posting;
	while (current != InvalidBlockNumber)
	{
		TpSegmentHeader *header;

		/* Open segment */
		reader = tp_segment_open(index, current);
		if (!reader)
			break;

		/* Initialize iterator for this term */
		if (tp_segment_posting_iterator_init(&iter, reader, term))
		{
			/* Process each posting using zero-copy iteration */
			while (tp_segment_posting_iterator_next(&iter, &posting))
			{
				float4				tf;
				float4				doc_len;
				float4				term_score;
				double				numerator_d, denominator_d;
				DocumentScoreEntry *doc_entry;
				bool				found;
				HTAB			   *hash_table = (HTAB *)doc_scores_hash;
				ItemPointerData		local_ctid;

				/* Check for NULL posting */
				if (posting == NULL)
				{
					elog(ERROR,
						 "tp_process_term_in_segments: posting is NULL!");
				}

				tf = posting->frequency;

				/* Copy ctid to avoid packed member alignment issues */
				local_ctid = posting->ctid;

				/* Look up document length */
				doc_len = (float4)
						tp_segment_get_document_length(reader, &local_ctid);

				if (doc_len <= 0.0f)
				{
					/* Document not found in this segment, try memtable */
					doc_len = (float4)tp_get_document_length(
							local_state, index, &local_ctid);
					if (doc_len <= 0.0f)
						continue; /* Skip if still not found */
				}

				/* Validate TID */
				if (!ItemPointerIsValid(&local_ctid))
					continue;

				/* Calculate BM25 term score */
				numerator_d	  = (double)tf * ((double)k1 + 1.0);
				denominator_d = (double)tf +
								(double)k1 *
										(1.0 - (double)b +
										 (double)b * ((double)doc_len /
													  (double)avg_doc_len));
				term_score = (float4)((double)idf *
									  (numerator_d / denominator_d) *
									  (double)query_frequency);

				/* Add or update document score in hash table */
				doc_entry = (DocumentScoreEntry *)hash_search(
						hash_table, &local_ctid, HASH_ENTER, &found);
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

			/* Release any active buffer when done with this iterator */
			if (iter.has_active_access)
			{
				tp_segment_release_direct(&iter.current_access);
				iter.has_active_access = false;
			}
		}

		/* Get next segment from header */
		header	= reader->header;
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
				TpDictEntry dict_entry;
				tp_segment_read(
						reader,
						header->entries_offset + (mid * sizeof(TpDictEntry)),
						&dict_entry,
						sizeof(TpDictEntry));
				doc_freq += dict_entry.doc_freq;
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
