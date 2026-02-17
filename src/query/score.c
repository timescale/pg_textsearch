/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * score.c - BM25 scoring operators and document ranking
 */
#include <postgres.h>

#include <math.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>

#include "memtable/memtable.h"
#include "query/bmw.h"
#include "query/score.h"
#include "segment/segment.h"
#include "state/metapage.h"
#include "state/state.h"

/*
 * Centralized IDF calculation (basic version)
 * Calculates IDF using BM25 formula: log(1 + (N - df + 0.5) / (df + 0.5))
 * This formula ensures IDF is always non-negative since log(1 + x) >= 0
 * for all x >= 0.
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	return (float4)log(1.0 + idf_ratio);
}

/*
 * Get unified doc_freq for a term (memtable + all segments).
 * Returns 0 if term not found in any source.
 */
static uint32
tp_get_unified_doc_freq(
		TpLocalIndexState *local_state,
		Relation		   index,
		const char		  *term,
		BlockNumber		  *level_heads)
{
	uint32		   doc_freq = 0;
	TpPostingList *posting_list;
	int			   level;

	/* Get doc_freq from memtable */
	posting_list = tp_get_posting_list(local_state, term);
	if (posting_list && posting_list->doc_count > 0)
		doc_freq = posting_list->doc_count;

	/* Add doc_freq from all segment levels */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (level_heads[level] != InvalidBlockNumber)
		{
			doc_freq +=
					tp_segment_get_doc_freq(index, level_heads[level], term);
		}
	}

	return doc_freq;
}

/*
 * Batch get unified doc_freq for multiple terms (memtable + all segments).
 * Much faster than calling tp_get_unified_doc_freq in a loop because
 * it opens each segment only once instead of once per term.
 */
static void
tp_batch_get_unified_doc_freq(
		TpLocalIndexState *local_state,
		Relation		   index,
		char			 **terms,
		int				   term_count,
		BlockNumber		  *level_heads,
		uint32			  *doc_freqs)
{
	int level;
	int i;

	/* Initialize doc_freqs with memtable counts */
	for (i = 0; i < term_count; i++)
	{
		TpPostingList *posting_list =
				tp_get_posting_list(local_state, terms[i]);
		doc_freqs[i] = (posting_list && posting_list->doc_count > 0)
							 ? posting_list->doc_count
							 : 0;
	}

	/* Add doc_freq from all segment levels (batch lookup) */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (level_heads[level] != InvalidBlockNumber)
		{
			tp_batch_get_segment_doc_freq(
					index, level_heads[level], terms, term_count, doc_freqs);
		}
	}
}

/*
 * Score documents using BM25 algorithm
 * Returns number of documents scored
 */
int
tp_score_documents(
		TpLocalIndexState *local_state,
		Relation		   index_relation,
		char			 **query_terms,
		int32			  *query_frequencies,
		int				   query_term_count,
		float4			   k1,
		float4			   b,
		int				   max_results,
		ItemPointer		   result_ctids,
		float4			 **result_scores)
{
	float4			avg_doc_len;
	int32			total_docs;
	TpIndexMetaPage metap;
	BlockNumber		level_heads[TP_MAX_LEVELS];
	int				i;

	/* Basic sanity checks */
	Assert(local_state != NULL);
	Assert(query_terms != NULL);
	Assert(result_ctids != NULL);
	Assert(result_scores != NULL);

	if (query_term_count <= 0 || max_results <= 0)
		return 0;

	if (!local_state->shared)
	{
		elog(ERROR, "tp_score_documents: shared state is NULL!");
		return 0;
	}

	total_docs	= local_state->shared->total_docs;
	avg_doc_len = total_docs > 0 ? (float4)(local_state->shared->total_len /
											(double)total_docs)
								 : 0.0f;

	if (total_docs <= 0)
		return 0;

	/* Get segment level heads for querying all levels */
	metap = tp_get_metapage(index_relation);
	for (i = 0; i < TP_MAX_LEVELS; i++)
		level_heads[i] = metap->level_heads[i];
	pfree(metap);

	/* If avg_doc_len is 0, all documents have zero length and
	 * would get zero BM25 scores */
	if (avg_doc_len <= 0.0f)
		return 0;

	/*
	 * BMW fast path for single-term queries.
	 * Uses Block-Max WAND to skip blocks that can't contribute to top-k.
	 */
	if (query_term_count == 1)
	{
		const char *term = query_terms[0];
		uint32		doc_freq;
		float4		idf;
		float4	   *scores;
		int			result_count;
		TpBMWStats	stats;

		/* Get unified doc_freq across memtable and segments */
		doc_freq = tp_get_unified_doc_freq(
				local_state, index_relation, term, level_heads);
		if (doc_freq == 0)
			return 0;

		/* Calculate IDF */
		idf = tp_calculate_idf(doc_freq, total_docs);

		/* Allocate scores array */
		scores = (float4 *)palloc(max_results * sizeof(float4));

		/* Run BMW scoring */
		result_count = tp_score_single_term_bmw(
				local_state,
				index_relation,
				term,
				idf,
				k1,
				b,
				avg_doc_len,
				max_results,
				result_ctids,
				scores,
				&stats);

		/* Log BMW stats if enabled */
		if (tp_log_bmw_stats)
		{
			elog(LOG,
				 "BMW stats: memtable=%lu docs, segments=%lu docs "
				 "(blocks: %lu scanned, %lu skipped, %.1f%% skip), "
				 "seeks=%lu, results=%lu",
				 (unsigned long)stats.memtable_docs,
				 (unsigned long)stats.segment_docs_scored,
				 (unsigned long)stats.blocks_scanned,
				 (unsigned long)stats.blocks_skipped,
				 (stats.blocks_scanned + stats.blocks_skipped) > 0
						 ? 100.0 * stats.blocks_skipped /
								   (stats.blocks_scanned +
									stats.blocks_skipped)
						 : 0.0,
				 (unsigned long)stats.seeks_performed,
				 (unsigned long)stats.docs_in_results);
		}

		*result_scores = scores;
		return result_count;
	}

	/*
	 * BMW fast path for multi-term queries.
	 * Uses block-level upper bounds to skip non-contributing blocks.
	 */
	if (query_term_count >= 2)
	{
		uint32	  *doc_freqs;
		float4	  *idfs;
		float4	  *scores;
		int		   result_count;
		int		   i;
		TpBMWStats stats;

		/* Batch lookup doc_freqs for all terms (opens each segment once) */
		doc_freqs = palloc(query_term_count * sizeof(uint32));
		tp_batch_get_unified_doc_freq(
				local_state,
				index_relation,
				query_terms,
				query_term_count,
				level_heads,
				doc_freqs);

		/* Convert doc_freqs to IDFs */
		idfs = palloc(query_term_count * sizeof(float4));
		for (i = 0; i < query_term_count; i++)
		{
			idfs[i] = (doc_freqs[i] > 0)
							? tp_calculate_idf(doc_freqs[i], total_docs)
							: 0.0f;
		}
		pfree(doc_freqs);

		/* Allocate scores array */
		scores = (float4 *)palloc(max_results * sizeof(float4));

		/* Run multi-term BMW scoring */
		result_count = tp_score_multi_term_bmw(
				local_state,
				index_relation,
				query_terms,
				query_term_count,
				query_frequencies,
				idfs,
				k1,
				b,
				avg_doc_len,
				max_results,
				result_ctids,
				scores,
				&stats);

		pfree(idfs);

		/* Log BMW stats if enabled */
		if (tp_log_bmw_stats)
		{
			elog(LOG,
				 "BMW stats: memtable=%lu docs, segments=%lu docs "
				 "(blocks: %lu scanned, %lu skipped, %.1f%% skip), "
				 "seeks=%lu, results=%lu",
				 (unsigned long)stats.memtable_docs,
				 (unsigned long)stats.segment_docs_scored,
				 (unsigned long)stats.blocks_scanned,
				 (unsigned long)stats.blocks_skipped,
				 (stats.blocks_scanned + stats.blocks_skipped) > 0
						 ? 100.0 * stats.blocks_skipped /
								   (stats.blocks_scanned +
									stats.blocks_skipped)
						 : 0.0,
				 (unsigned long)stats.seeks_performed,
				 (unsigned long)stats.docs_in_results);
		}

		*result_scores = scores;
		return result_count;
	}

	/* Not reachable: query_term_count is >= 1 (checked above) so one
	 * of the single-term or multi-term BMW paths always handles it. */
	return 0;
}
