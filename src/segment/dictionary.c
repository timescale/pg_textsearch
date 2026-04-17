/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * dictionary.c - Term dictionary for disk segments
 */
#include <postgres.h>

#include <lib/dshash.h>
#include <utils/memutils.h>

#include "constants.h"
#include "index/state.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "segment/dictionary.h"
#include "segment/io.h"

/*
 * Comparison function for qsort
 */
static int
term_info_cmp(const void *a, const void *b)
{
	const TermInfo *ta = (const TermInfo *)a;
	const TermInfo *tb = (const TermInfo *)b;
	return strcmp(ta->term, tb->term);
}

/*
 * Build sorted dictionary from memtable
 * Returns array of TermInfo sorted lexicographically
 */
TermInfo *
tp_build_dictionary(TpLocalIndexState *state, uint32 *num_terms)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	dshash_seq_status  status;
	TpStringHashEntry *entry;
	TermInfo		  *terms;
	uint32			   count	= 0;
	uint32			   capacity = 1024;

	/* Get memtable from shared state */
	memtable = (TpMemtable *)
			dsa_get_address(state->dsa, state->shared->memtable_dp);
	if (!memtable)
	{
		elog(ERROR, "memtable not found in shared memory");
	}

	/* Check if memtable has been cleared (no string hash table) */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		*num_terms = 0;
		return NULL;
	}

	/* Attach to string hash table */
	string_table =
			tp_string_table_attach(state->dsa, memtable->string_hash_handle);
	if (!string_table)
	{
		elog(ERROR, "failed to attach to string table");
	}

	/* Allocate initial array */
	terms = palloc(sizeof(TermInfo) * capacity);

	/* Iterate through all terms in string table using dshash sequential scan
	 */
	dshash_seq_init(&status, string_table, false); /* shared lock */

	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		const char *term = tp_get_key_str(state->dsa, &entry->key);

		/* Skip if no term (shouldn't happen) */
		if (!term)
			continue;

		/* Resize if needed */
		if (count >= capacity)
		{
			capacity *= 2;
			terms = repalloc(terms, sizeof(TermInfo) * capacity);
		}

		/* Store term info */
		terms[count].term			 = pstrdup(term);
		terms[count].term_len		 = strlen(term);
		terms[count].posting_list_dp = entry->key.posting_list;
		terms[count].dict_entry_idx =
				count; /* Will be updated after sorting */
		count++;
	}

	dshash_seq_term(&status);
	dshash_detach(string_table);

	/* Sort terms lexicographically */
	qsort(terms, count, sizeof(TermInfo), term_info_cmp);

	/* After sorting, dict_entry_idx should map sorted position to original
	 * posting */

	*num_terms = count;

	return terms;
}

/*
 * Free dictionary
 */
void
tp_free_dictionary(TermInfo *terms, uint32 num_terms)
{
	uint32 i;

	/* Handle NULL terms array */
	if (!terms)
		return;

	for (i = 0; i < num_terms; i++)
	{
		if (terms[i].term)
			pfree(terms[i].term);
	}

	pfree(terms);
}

/*
 * Read a term string from a segment's string pool at a given dictionary
 * index. Returns a palloc'd string that must be freed by the caller.
 */
char *
tp_segment_read_term_at_index(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			*string_offsets,
		uint32			 index)
{
	uint64 string_offset;
	uint32 length;
	char  *term;

	string_offset = header->strings_offset + string_offsets[index];

	/* Read string length */
	tp_segment_read(reader, string_offset, &length, sizeof(uint32));

	if (length > TP_MAX_TERM_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt segment: term length %u exceeds "
						"maximum",
						length)));

	/* Allocate and read string */
	term = palloc(length + 1);
	tp_segment_read(reader, string_offset + sizeof(uint32), term, length);
	term[length] = '\0';

	return term;
}
