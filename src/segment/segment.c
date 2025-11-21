/*-------------------------------------------------------------------------
 *
 * segment/segment.c
 *    Segment implementation for pg_textsearch
 *
 * This file contains stub implementations for segment management.
 * The actual flush and search logic will be added in future PRs.
 *
 * IDENTIFICATION
 *    src/segment/segment.c
 *
 *-------------------------------------------------------------------------
 */
#include "../state.h"
#include "postgres.h"
#include "segment.h"

/*
 * tp_segment_exists - Check if a segment exists for the index
 *
 * This is a stub that always returns false for now.
 * Future implementation will check for actual segment files.
 */
bool
tp_segment_exists(TpLocalIndexState *state)
{
	/* Stub: No segments exist yet */
	return false;
}

/*
 * tp_flush_to_segment - Flush memtable to disk segment
 *
 * This is a stub implementation that does nothing for now.
 * Future implementation will write memtable data to disk.
 */
void
tp_flush_to_segment(TpLocalIndexState *state)
{
	/* Stub: Flush not implemented yet */
	elog(DEBUG1, "Segment flush requested but not yet implemented");
}

/*
 * tp_segment_search - Search segments for matching documents
 *
 * This is a stub that returns no results.
 * Future implementation will search disk segments.
 */
DocumentScoreEntry *
tp_segment_search(
		TpLocalIndexState *state,
		const char		 **query_terms,
		int				   num_query_terms,
		int				  *num_results)
{
	/* Stub: No segment search yet */
	*num_results = 0;
	return NULL;
}
