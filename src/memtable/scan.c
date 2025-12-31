/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/scan.c - Memtable scan operations
 *
 * This module provides the search interface for scanning the memtable
 * (and segments) during index scans.
 */
#include <postgres.h>

#include <access/relscan.h>
#include <utils/memutils.h>

#include "memtable.h"
#include "scan.h"
#include "state/limit.h"
#include "state/metapage.h"
#include "state/state.h"
#include "types/score.h"
#include "types/vector.h"

/*
 * Search the memtable (and segments) for documents matching the query vector.
 * Returns true on success (results stored in scan opaque), false on failure.
 *
 * This is the main entry point called from am/scan.c during tp_gettuple.
 */
bool
tp_memtable_search(
		IndexScanDesc	   scan,
		TpLocalIndexState *index_state,
		TpVector		  *query_vector,
		TpIndexMetaPage	   metap)
{
	TpScanOpaque  so = (TpScanOpaque)scan->opaque;
	int			  max_results;
	int			  result_count = 0;
	float4		  k1_value;
	float4		  b_value;
	MemoryContext oldcontext;

	/* Extract terms and frequencies from query vector */
	char		 **query_terms;
	int32		  *query_frequencies;
	TpVectorEntry *entries_ptr;
	int			   entry_count;
	char		  *ptr;

	/* Use limit from scan state, fallback to GUC parameter */
	if (so && so->limit > 0)
		max_results = so->limit;
	else
		max_results = tp_default_limit;

	entry_count		  = query_vector->entry_count;
	query_terms		  = palloc(entry_count * sizeof(char *));
	query_frequencies = palloc(entry_count * sizeof(int32));
	entries_ptr		  = TPVECTOR_ENTRIES_PTR(query_vector);

	/* Parse the query vector entries */
	ptr = (char *)entries_ptr;
	for (int i = 0; i < entry_count; i++)
	{
		TpVectorEntry *entry = (TpVectorEntry *)ptr;
		char		  *term_str;

		/* Allocate on heap for query terms array */
		term_str = palloc(entry->lexeme_len + 1);
		memcpy(term_str, entry->lexeme, entry->lexeme_len);
		term_str[entry->lexeme_len] = '\0';

		/* Store the term string directly in query terms array */
		query_terms[i]		 = term_str;
		query_frequencies[i] = entry->frequency;

		ptr += sizeof(TpVectorEntry) + MAXALIGN(entry->lexeme_len);
	}

	/* Allocate result arrays in scan context */
	oldcontext		 = MemoryContextSwitchTo(so->scan_context);
	so->result_ctids = palloc(max_results * sizeof(ItemPointerData));
	/* Initialize to invalid TIDs for safety */
	memset(so->result_ctids, 0, max_results * sizeof(ItemPointerData));
	MemoryContextSwitchTo(oldcontext);

	/* Extract values from metap */
	Assert(metap != NULL);
	k1_value = metap->k1;
	b_value	 = metap->b;

	Assert(index_state != NULL);
	Assert(query_terms != NULL);
	Assert(query_frequencies != NULL);
	Assert(so->result_ctids != NULL);

	/* Score documents using the unified scoring function */
	result_count = tp_score_documents(
			index_state,
			scan->indexRelation,
			query_terms,
			query_frequencies,
			entry_count,
			k1_value,
			b_value,
			max_results,
			so->result_ctids,
			&so->result_scores);

	so->result_count = result_count;
	so->current_pos	 = 0;

	/* Free the query terms array and individual term strings */
	for (int i = 0; i < entry_count; i++)
		pfree(query_terms[i]);

	pfree(query_terms);
	pfree(query_frequencies);

	return result_count > 0;
}
