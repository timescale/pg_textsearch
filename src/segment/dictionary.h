/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * dictionary.h - Term dictionary for disk segments
 */
#pragma once

#include "postgres.h"
#include "utils/dsa.h"

/* Forward declarations */
struct TpLocalIndexState;
struct TpSegmentReader;
struct TpSegmentHeader;

/*
 * Term info for building dictionary
 */
typedef struct TermInfo
{
	char	   *term;			 /* The term text */
	uint32		term_len;		 /* Term length */
	dsa_pointer posting_list_dp; /* DSA pointer to posting list */
	uint32		dict_entry_idx;	 /* Index in dict_entries array */
} TermInfo;

/* Dictionary building functions */
extern TermInfo *
tp_build_dictionary(struct TpLocalIndexState *state, uint32 *num_terms);
extern void tp_free_dictionary(TermInfo *terms, uint32 num_terms);

/* Shared term-reading helper */
extern char *tp_segment_read_term_at_index(
		struct TpSegmentReader *reader,
		struct TpSegmentHeader *header,
		uint32				   *string_offsets,
		uint32					index);
