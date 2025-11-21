/*-------------------------------------------------------------------------
 *
 * segment/dictionary.c
 *    Dictionary building for segments
 *
 * This file contains stub implementations for dictionary building.
 * The actual dictionary construction logic will be added in future PRs.
 *
 * IDENTIFICATION
 *    src/segment/dictionary.c
 *
 *-------------------------------------------------------------------------
 */
#include "../state.h"
#include "dictionary.h"
#include "postgres.h"

/*
 * tp_build_dictionary - Build a sorted dictionary from memtable terms
 *
 * This is a stub that returns NULL for now.
 * Future implementation will extract and sort terms from the memtable.
 */
TermInfo *
tp_build_dictionary(TpLocalIndexState *state, uint32 *num_terms)
{
	/* Stub: Dictionary building not implemented yet */
	*num_terms = 0;
	return NULL;
}

/*
 * tp_free_dictionary - Free dictionary memory
 *
 * This is a stub that does nothing for now.
 * Future implementation will properly free dictionary resources.
 */
void
tp_free_dictionary(TermInfo *terms, uint32 num_terms)
{
	/* Stub: Nothing to free yet */
}
