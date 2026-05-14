/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * dictionary.c - Term dictionary for disk segments
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "constants.h"
#include "segment/dictionary.h"
#include "segment/io.h"

/*
 * Free dictionary's term strings.
 *
 * Phase 4: the ctids/freqs arrays are owned by the source's memory
 * context (e.g. chain_source's private mcxt) and are dropped when
 * that context is reset.  We only free the per-entry palloc'd term
 * string and the array itself.
 */
void
tp_free_dictionary(TermInfo *terms, uint32 num_terms)
{
	uint32 i;

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
