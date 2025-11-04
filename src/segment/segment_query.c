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
#include "postgres.h"

#include "segment/segment.h"
#include "segment/dictionary.h"
#include "memtable/posting.h"
#include "operator.h"
#include "state.h"
#include "utils/memutils.h"

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
	uint32			*string_offsets;
	int				 left, right, mid;

	iter->reader		  = reader;
	iter->term			  = term;
	iter->current_posting = 0;
	iter->initialized	  = false;
	iter->finished		  = true; /* Default to finished if not found */

	if (header->num_terms == 0 || header->dictionary_offset == 0)
		return false;

	/* Read dictionary header */
	tp_segment_read(
			reader,
			header->dictionary_offset,
			&dict_header,
			sizeof(dict_header.num_terms));

	/* Read string offsets array */
	string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
	tp_segment_read(
			reader,
			header->dictionary_offset + sizeof(dict_header.num_terms),
			string_offsets,
			sizeof(uint32) * dict_header.num_terms);

	/* Binary search for the term */
	left  = 0;
	right = dict_header.num_terms - 1;

	while (left <= right)
	{
		TpStringEntry string_entry;
		char		 *term_text;
		int			  cmp;
		uint32		  string_offset;

		mid			  = left + (right - left) / 2;
		string_offset = header->strings_offset + string_offsets[mid];

		/* Read string length */
		tp_segment_read(
				reader, string_offset, &string_entry.length, sizeof(uint32));

		/* Read term text */
		term_text = palloc(string_entry.length + 1);
		tp_segment_read(
				reader,
				string_offset + sizeof(uint32),
				term_text,
				string_entry.length);
		term_text[string_entry.length] = '\0';

		/* Compare terms */
		cmp = strcmp(term, term_text);

		if (cmp == 0)
		{
			/* Found it! Read the dictionary entry */
			tp_segment_read(
					reader,
					header->entries_offset + (mid * sizeof(TpDictEntry)),
					&iter->dict_entry,
					sizeof(TpDictEntry));

			iter->dict_entry_idx  = mid;
			iter->postings_offset = header->postings_offset +
									iter->dict_entry.posting_offset;
			iter->initialized = true;
			iter->finished	  = (iter->dict_entry.posting_count == 0);

			pfree(term_text);
			pfree(string_offsets);
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

		pfree(term_text);
	}

	pfree(string_offsets);
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
	TpSegmentDirectAccess access;
	uint32				  offset;

	if (iter->finished || !iter->initialized)
		return false;

	if (iter->current_posting >= iter->dict_entry.posting_count)
	{
		iter->finished = true;
		return false;
	}

	/* Calculate offset for current posting */
	offset = iter->postings_offset +
			 (iter->current_posting * sizeof(TpSegmentPosting));

	/* Get direct access to the posting */
	if (!tp_segment_get_direct(
				iter->reader, offset, sizeof(TpSegmentPosting), &access))
	{
		/* Fallback to regular read if direct access fails */
		static TpSegmentPosting temp_posting;
		tp_segment_read(
				iter->reader, offset, &temp_posting, sizeof(TpSegmentPosting));
		*posting = &temp_posting;
	}
	else
	{
		/* Zero-copy: return pointer directly into the page */
		*posting = (TpSegmentPosting *)access.data;
	}

	iter->current_posting++;
	return true;
}

/* Forward declaration for document length lookup */
extern int32 tp_get_document_length(
		TpLocalIndexState *local_state, Relation index, ItemPointer ctid);

/*
 * Process a term across all segments using zero-copy iteration
 * This function is called from the query execution path in operator.c
 */
void
tp_process_term_in_segments(
		Relation		   index,
		BlockNumber		   first_segment,
		const char		  *term,
		int32			   total_docs,
		float8			   avg_idf,
		float4			   query_frequency,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		void			  *doc_scores_hash, /* HTAB* */
		TpLocalIndexState *local_state)
{
	BlockNumber				 current = first_segment;
	TpSegmentReader			*reader	 = NULL;
	TpSegmentPostingIterator iter;
	TpSegmentPosting		*posting;
	int32  doc_freq = 0; /* Total document frequency across segments */
	float4 idf;

	/* First pass: count total document frequency across all segments */
	current = first_segment;
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
			/* Add document count from this segment */
			doc_freq += iter.dict_entry.doc_freq;
		}

		/* Get next segment from header */
		header	= reader->header;
		current = header->next_segment;

		/* Close this segment */
		tp_segment_close(reader);
	}

	/* If no documents found, nothing to process */
	if (doc_freq == 0)
		return;

	/* Calculate IDF for this term using the total document frequency */
	idf = tp_calculate_idf_with_epsilon(doc_freq, total_docs, avg_idf);

	/* Second pass: process posting lists and calculate scores */
	current = first_segment;
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
				float4				tf = posting->frequency;
				float4				doc_len;
				float4				term_score;
				double				numerator_d, denominator_d;
				DocumentScoreEntry *doc_entry;
				bool				found;
				HTAB			   *hash_table = (HTAB *)doc_scores_hash;

				/* Look up document length */
				doc_len = (float4)
						tp_segment_get_document_length(reader, &posting->ctid);
				if (doc_len <= 0.0f)
				{
					/* Document not found in this segment, try memtable */
					doc_len = (float4)tp_get_document_length(
							local_state, index, &posting->ctid);
					if (doc_len <= 0.0f)
						continue; /* Skip if still not found */
				}

				/* Validate TID */
				if (!ItemPointerIsValid(&posting->ctid))
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
						hash_table, &posting->ctid, HASH_ENTER, &found);
				if (!found)
				{
					doc_entry->ctid		  = posting->ctid;
					doc_entry->score	  = term_score;
					doc_entry->doc_length = doc_len;
				}
				else
				{
					doc_entry->score += term_score;
				}
			}
		}

		/* Get next segment from header */
		header	= reader->header;
		current = header->next_segment;

		/* Close this segment */
		tp_segment_close(reader);
	}
}
