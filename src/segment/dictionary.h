/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * dictionary.h - Term dictionary for disk segments
 */
#ifndef DICTIONARY_H
#define DICTIONARY_H

#include "postgres.h"
#include "utils/dsa.h"

/* Forward declarations */
struct TpLocalIndexState;

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

#endif /* DICTIONARY_H */
