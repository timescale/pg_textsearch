/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment/scan.c - Zero-copy scan execution for segments
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "compression.h"
#include "memtable/posting.h"
#include "query/score.h"
#include "segment/dictionary.h"
#include "segment/fieldnorm.h"
#include "segment/segment.h"
#include "state/state.h"

/*
 * Read a skip entry by block index.
 * Used by BMW scoring to pre-compute block max scores.
 */
void
tp_segment_read_skip_entry(
		TpSegmentReader *reader,
		TpDictEntry		*dict_entry,
		uint16			 block_idx,
		TpSkipEntry		*skip)
{
	uint32 skip_offset = dict_entry->skip_index_offset +
						 (block_idx * sizeof(TpSkipEntry));
	tp_segment_read(reader, skip_offset, skip, sizeof(TpSkipEntry));
}

/*
 * Initialize iterator for a specific term in a segment.
 * Returns true if term found, false otherwise.
 */
bool
tp_segment_posting_iterator_init(
		TpSegmentPostingIterator *iter,
		TpSegmentReader			 *reader,
		const char				 *term)
{
	TpSegmentHeader *header;
	TpDictionary	 dict_header;
	int				 left, right, mid;
	char			*term_buffer = NULL;
	uint32			 buffer_size = 0;

	if (reader == NULL)
		return false;

	header = reader->header;
	if (header == NULL)
		return false;

	iter->reader			  = reader;
	iter->term				  = term;
	iter->current_block		  = 0;
	iter->current_in_block	  = 0;
	iter->initialized		  = false;
	iter->finished			  = true;
	iter->block_postings	  = NULL;
	iter->has_block_access	  = false;
	iter->fallback_block	  = NULL;
	iter->fallback_block_size = 0;

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
			/* Found! Read dictionary entry */
			tp_segment_read(
					reader,
					header->entries_offset + (mid * sizeof(TpDictEntry)),
					&iter->dict_entry,
					sizeof(TpDictEntry));

			iter->dict_entry_idx = mid;
			iter->initialized	 = true;
			iter->finished		 = (iter->dict_entry.block_count == 0);

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

	pfree(term_buffer);
	return false;
}

/*
 * Load a block's postings for iteration.
 * Uses zero-copy access when block data fits within a single page and is
 * uncompressed. Compressed blocks are always decompressed into the fallback
 * buffer. CTIDs are looked up from segment-level cached arrays during
 * iteration.
 */
bool
tp_segment_posting_iterator_load_block(TpSegmentPostingIterator *iter)
{
	uint32 skip_offset;
	uint32 block_size;
	uint32 block_bytes;

	if (iter->current_block >= iter->dict_entry.block_count)
		return false;

	/* Release previous block access if any */
	if (iter->has_block_access)
	{
		tp_segment_release_direct(&iter->block_access);
		iter->has_block_access = false;
		iter->block_postings   = NULL;
	}

	/* Read skip entry for current block (small, always copy) */
	skip_offset = iter->dict_entry.skip_index_offset +
				  (iter->current_block * sizeof(TpSkipEntry));
	tp_segment_read(
			iter->reader, skip_offset, &iter->skip_entry, sizeof(TpSkipEntry));

	block_size	= iter->skip_entry.doc_count;
	block_bytes = block_size * sizeof(TpBlockPosting);

	/* Handle compressed blocks */
	if (iter->skip_entry.flags == TP_BLOCK_FLAG_DELTA)
	{
		uint8 *compressed_buf;

		/* Read compressed data into temporary buffer (max possible size) */
		compressed_buf = palloc(TP_MAX_COMPRESSED_BLOCK_SIZE);
		tp_segment_read(
				iter->reader,
				iter->skip_entry.posting_offset,
				compressed_buf,
				TP_MAX_COMPRESSED_BLOCK_SIZE);

		/* Ensure fallback buffer is large enough */
		if (block_size > iter->fallback_block_size)
		{
			if (iter->fallback_block)
				pfree(iter->fallback_block);
			iter->fallback_block	  = palloc(block_bytes);
			iter->fallback_block_size = block_size;
		}

		/* Decompress into fallback buffer */
		tp_decompress_block(
				compressed_buf, block_size, 0, iter->fallback_block);

		pfree(compressed_buf);
		iter->block_postings = iter->fallback_block;
	}
	else
	{
		/*
		 * Uncompressed block: try zero-copy direct access.
		 * TpBlockPosting requires 4-byte alignment (due to uint32 doc_id).
		 * If the data address is misaligned, fall back to copying.
		 */
		if (tp_segment_get_direct(
					iter->reader,
					iter->skip_entry.posting_offset,
					block_bytes,
					&iter->block_access) &&
			((uintptr_t)iter->block_access.data % sizeof(uint32)) == 0)
		{
			/* Zero-copy: point directly into the page buffer */
			iter->block_postings   = (TpBlockPosting *)iter->block_access.data;
			iter->has_block_access = true;
		}
		else
		{
			/* Release direct access if we got it but it's misaligned */
			if (iter->block_access.data != NULL)
				tp_segment_release_direct(&iter->block_access);
			/* Fallback: block spans page boundary, must copy */
			if (block_size > iter->fallback_block_size)
			{
				if (iter->fallback_block)
					pfree(iter->fallback_block);
				iter->fallback_block	  = palloc(block_bytes);
				iter->fallback_block_size = block_size;
			}

			tp_segment_read(
					iter->reader,
					iter->skip_entry.posting_offset,
					iter->fallback_block,
					block_bytes);

			iter->block_postings = iter->fallback_block;
		}
	}

	iter->current_in_block = 0;
	return true;
}

/*
 * Get next posting from iterator.
 * Converts block posting to TpSegmentPosting for scoring compatibility.
 * Returns false when no more postings.
 */
bool
tp_segment_posting_iterator_next(
		TpSegmentPostingIterator *iter, TpSegmentPosting **posting)
{
	TpBlockPosting *bp;
	uint32			doc_id;

	if (iter->finished || !iter->initialized)
		return false;

	/* Load first block if needed */
	if (iter->block_postings == NULL)
	{
		if (!tp_segment_posting_iterator_load_block(iter))
		{
			/* Release any block access before finishing */
			if (iter->has_block_access)
			{
				tp_segment_release_direct(&iter->block_access);
				iter->has_block_access = false;
			}
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
			/* Release block access before finishing */
			if (iter->has_block_access)
			{
				tp_segment_release_direct(&iter->block_access);
				iter->has_block_access = false;
			}
			iter->finished = true;
			return false;
		}
		if (!tp_segment_posting_iterator_load_block(iter))
		{
			if (iter->has_block_access)
			{
				tp_segment_release_direct(&iter->block_access);
				iter->has_block_access = false;
			}
			iter->finished = true;
			return false;
		}
	}

	/* Get current posting from block */
	bp	   = &iter->block_postings[iter->current_in_block];
	doc_id = bp->doc_id;

	/* Always store doc_id for deferred CTID resolution */
	iter->output_posting.doc_id = doc_id;

	/*
	 * Look up CTID from segment-level cached arrays if available.
	 * By default, CTIDs are not pre-loaded and will be resolved later
	 * via tp_segment_lookup_ctid.
	 */
	if (iter->reader->cached_ctid_pages != NULL)
	{
		Assert(doc_id < iter->reader->cached_num_docs);
		ItemPointerSet(
				&iter->output_posting.ctid,
				iter->reader->cached_ctid_pages[doc_id],
				iter->reader->cached_ctid_offsets[doc_id]);
	}
	else
	{
		/* Mark CTID invalid - will be resolved later */
		ItemPointerSetInvalid(&iter->output_posting.ctid);
	}

	/* Build output posting (fieldnorm is inline in bp) */
	iter->output_posting.frequency	= bp->frequency;
	iter->output_posting.doc_length = (uint16)decode_fieldnorm(bp->fieldnorm);

	*posting = &iter->output_posting;
	iter->current_in_block++;
	return true;
}

/*
 * Free iterator resources.
 */
void
tp_segment_posting_iterator_free(TpSegmentPostingIterator *iter)
{
	/* Release direct block access if active */
	if (iter->has_block_access)
	{
		tp_segment_release_direct(&iter->block_access);
		iter->has_block_access = false;
	}

	/* Free fallback buffer if allocated */
	if (iter->fallback_block)
	{
		pfree(iter->fallback_block);
		iter->fallback_block = NULL;
	}

	iter->block_postings = NULL;
}

/*
 * Get current doc ID from iterator.
 * Returns UINT32_MAX if iterator is finished or not positioned.
 */
uint32
tp_segment_posting_iterator_current_doc_id(TpSegmentPostingIterator *iter)
{
	if (iter->finished || !iter->initialized || iter->block_postings == NULL)
		return UINT32_MAX;

	if (iter->current_in_block >= iter->skip_entry.doc_count)
		return UINT32_MAX;

	return iter->block_postings[iter->current_in_block].doc_id;
}

/*
 * Seek iterator to target doc ID or the first doc ID >= target.
 * Returns true if a posting was found, false if exhausted.
 *
 * Uses binary search on skip entries (each has last_doc_id) to find
 * the right block, then linear scan within the block. This is the
 * core operation for WAND-style doc-ID ordered traversal.
 */
bool
tp_segment_posting_iterator_seek(
		TpSegmentPostingIterator *iter,
		uint32					  target_doc_id,
		TpSegmentPosting		**posting)
{
	uint16		block_count;
	int			left, right, mid;
	uint16		target_block;
	TpSkipEntry skip;

	if (!iter->initialized || iter->finished)
		return false;

	block_count = iter->dict_entry.block_count;

	/*
	 * Binary search skip entries to find block containing target_doc_id.
	 * Each skip entry has last_doc_id = maximum doc ID in that block.
	 * We want the first block where last_doc_id >= target_doc_id.
	 */
	left  = 0;
	right = block_count - 1;

	while (left < right)
	{
		mid = left + (right - left) / 2;

		/* Read skip entry to get last_doc_id */
		tp_segment_read_skip_entry(
				iter->reader, &iter->dict_entry, mid, &skip);

		if (skip.last_doc_id < target_doc_id)
		{
			/* Target is past this block */
			left = mid + 1;
		}
		else
		{
			/* Target might be in this block or earlier */
			right = mid;
		}
	}

	target_block = left;

	/* Check if target is past all blocks */
	if (target_block >= block_count)
	{
		iter->finished = true;
		return false;
	}

	/* Load the target block */
	iter->current_block	   = target_block;
	iter->current_in_block = 0;
	iter->finished		   = false;

	if (!tp_segment_posting_iterator_load_block(iter))
	{
		iter->finished = true;
		return false;
	}

	/* Linear scan within block to find target or first doc >= target */
	while (iter->current_in_block < iter->skip_entry.doc_count)
	{
		TpBlockPosting *bp = &iter->block_postings[iter->current_in_block];

		if (bp->doc_id >= target_doc_id)
		{
			/* Found it - convert to output posting */
			iter->output_posting.doc_id = bp->doc_id;

			/* Resolve CTID if cached, otherwise leave invalid for later */
			if (iter->reader->cached_ctid_pages != NULL)
			{
				Assert(bp->doc_id < iter->reader->cached_num_docs);
				ItemPointerSet(
						&iter->output_posting.ctid,
						iter->reader->cached_ctid_pages[bp->doc_id],
						iter->reader->cached_ctid_offsets[bp->doc_id]);
			}
			else
			{
				ItemPointerSetInvalid(&iter->output_posting.ctid);
			}

			iter->output_posting.frequency	= bp->frequency;
			iter->output_posting.doc_length = (uint16)decode_fieldnorm(
					bp->fieldnorm);

			*posting = &iter->output_posting;
			return true;
		}

		iter->current_in_block++;
	}

	/*
	 * Exhausted this block without finding target.
	 * This shouldn't happen if last_doc_id was correct, but handle it
	 * by moving to next block and trying next().
	 */
	iter->current_block++;
	if (iter->current_block >= block_count)
	{
		iter->finished = true;
		return false;
	}

	/* Load next block and return first posting */
	iter->current_in_block = 0;
	return tp_segment_posting_iterator_next(iter, posting);
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
		elog(ERROR, "process_posting: posting is NULL!");
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

/*
 * Batch lookup doc_freq for multiple terms across a segment chain.
 * Opens each segment ONCE and looks up all terms, avoiding
 * O(terms * segments) segment opens.
 *
 * doc_freqs array should be pre-initialized (typically to 0 or memtable
 * counts). This function ADDS segment doc_freqs to existing values.
 */
void
tp_batch_get_segment_doc_freq(
		Relation	index,
		BlockNumber first_segment,
		char	  **terms,
		int			term_count,
		uint32	   *doc_freqs)
{
	BlockNumber current		= first_segment;
	char	   *term_buffer = NULL;
	uint32		buffer_size = 0;

	while (current != InvalidBlockNumber)
	{
		TpSegmentReader *reader;
		TpSegmentHeader *header;
		TpDictionary	 dict_header;
		int				 term_idx;

		/* Open segment ONCE for all terms */
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

		/* Read dictionary header once per segment */
		tp_segment_read(
				reader,
				header->dictionary_offset,
				&dict_header,
				sizeof(dict_header.num_terms));

		/* Look up each term in this segment */
		for (term_idx = 0; term_idx < term_count; term_idx++)
		{
			const char *term  = terms[term_idx];
			int			left  = 0;
			int			right = dict_header.num_terms - 1;

			/* Binary search for term in dictionary */
			while (left <= right)
			{
				int	   mid = left + (right - left) / 2;
				uint32 string_offset_value;
				uint32 string_offset;
				uint32 string_length;
				int	   cmp;

				tp_segment_read(
						reader,
						header->dictionary_offset +
								sizeof(dict_header.num_terms) +
								(mid * sizeof(uint32)),
						&string_offset_value,
						sizeof(uint32));

				string_offset = header->strings_offset + string_offset_value;

				tp_segment_read(
						reader, string_offset, &string_length, sizeof(uint32));

				if (string_length + 1 > buffer_size)
				{
					if (term_buffer)
						pfree(term_buffer);
					buffer_size = string_length + 1;
					term_buffer = palloc(buffer_size);
				}

				tp_segment_read(
						reader,
						string_offset + sizeof(uint32),
						term_buffer,
						string_length);
				term_buffer[string_length] = '\0';

				cmp = strcmp(term, term_buffer);

				if (cmp == 0)
				{
					/* Found - read dict entry and add doc_freq */
					TpDictEntry dict_entry;
					tp_segment_read(
							reader,
							header->entries_offset +
									(mid * sizeof(TpDictEntry)),
							&dict_entry,
							sizeof(TpDictEntry));
					doc_freqs[term_idx] += dict_entry.doc_freq;
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
		}

		/* Move to next segment and close this one */
		current = header->next_segment;
		tp_segment_close(reader);
	}

	if (term_buffer)
		pfree(term_buffer);
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
/*
 * Process all postings for a term, accumulating scores in hash table.
 */
static void
score_term_postings(
		TpSegmentPostingIterator *iter,
		float4					  idf,
		float4					  query_freq,
		float4					  k1,
		float4					  b,
		float4					  avg_doc_len,
		HTAB					 *hash_table)
{
	TpSegmentPosting *posting;

	while (tp_segment_posting_iterator_next(iter, &posting))
	{
		process_posting(
				posting, idf, query_freq, k1, b, avg_doc_len, hash_table);
	}
}

/*
 * Process a single segment for all terms.
 */
static void
score_segment_for_all_terms(
		TpSegmentReader *reader,
		char		   **terms,
		int				 term_count,
		int32			*query_frequencies,
		uint32			*doc_freqs,
		int32			 total_docs,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len,
		HTAB			*hash_table)
{
	for (int term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpSegmentPostingIterator iter;
		float4					 idf;

		if (!tp_segment_posting_iterator_init(&iter, reader, terms[term_idx]))
			continue;

		/* Accumulate doc_freq */
		doc_freqs[term_idx] += iter.dict_entry.doc_freq;

		/* Calculate IDF with current accumulated doc_freq */
		idf = tp_calculate_idf(doc_freqs[term_idx], total_docs);

		/* Process all postings for this term */
		score_term_postings(
				&iter,
				idf,
				(float4)query_frequencies[term_idx],
				k1,
				b,
				avg_doc_len,
				hash_table);

		tp_segment_posting_iterator_free(&iter);
	}
}

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
	BlockNumber current	   = first_segment;
	HTAB	   *hash_table = (HTAB *)doc_scores_hash;

	while (current != InvalidBlockNumber)
	{
		/*
		 * Use tp_segment_open_ex with load_ctids=true because the exhaustive
		 * path needs CTIDs as hash keys. The lazy CTID loading optimization
		 * is for the BMW path which uses doc_id in the top-k heap.
		 */
		TpSegmentReader *reader = tp_segment_open_ex(index, current, true);
		if (!reader)
			break;

		score_segment_for_all_terms(
				reader,
				terms,
				term_count,
				query_frequencies,
				doc_freqs,
				total_docs,
				k1,
				b,
				avg_doc_len,
				hash_table);

		current = reader->header->next_segment;
		tp_segment_close(reader);
	}
}
