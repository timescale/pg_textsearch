/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * phrase.h - Phrase query matching using positional information
 *
 * Provides position intersection algorithms for verifying that
 * query terms appear adjacent and in order within a document.
 * Used as a post-filter after BMW candidate selection.
 */
#pragma once

#include <postgres.h>

/*
 * Maximum number of terms in a single phrase query.
 * Limits stack allocation in the phrase check hot path.
 */
#define TP_MAX_PHRASE_TERMS 32

/*
 * Maximum positions per term per document for phrase checking.
 * Documents with more positions than this are clamped (positions
 * beyond this limit are silently ignored — extremely rare in
 * practice since it implies a single term appearing 4096+ times
 * in one document).
 */
#define TP_MAX_POSITIONS_PER_DOC 4096

/*
 * Check whether a phrase occurs in a document given position
 * lists for each term in the phrase.
 *
 * pos_lists:   array of sorted position arrays, one per phrase term
 * pos_counts:  number of positions in each list
 * num_terms:   number of terms in the phrase (length of pos_lists)
 *
 * Returns true if there exists at least one position p such that
 * term[0] is at position p, term[1] at p+1, ..., term[n-1] at p+n-1.
 *
 * Uses a merge-based algorithm that scans forward through position
 * lists without backtracking.  Worst-case O(sum of all pos_counts),
 * but typically much faster due to early exit.
 */
static inline bool
tp_check_phrase_match(
		uint32 **pos_lists,
		uint32 *pos_counts,
		int num_terms)
{
	uint32 cursors[TP_MAX_PHRASE_TERMS];
	int    i;

	Assert(num_terms >= 2);
	Assert(num_terms <= TP_MAX_PHRASE_TERMS);

	/* Bail early if any term has no positions */
	for (i = 0; i < num_terms; i++)
	{
		if (pos_counts[i] == 0)
			return false;
		cursors[i] = 0;
	}

	/*
	 * For each candidate start position from term[0],
	 * check if subsequent terms appear at consecutive
	 * positions.
	 */
	while (cursors[0] < pos_counts[0])
	{
		uint32 target = pos_lists[0][cursors[0]];
		bool   match  = true;

		for (i = 1; i < num_terms; i++)
		{
			target++;

			/*
			 * Advance cursor[i] past positions
			 * less than target.
			 */
			while (cursors[i] < pos_counts[i] &&
				   pos_lists[i][cursors[i]] < target)
				cursors[i]++;

			if (cursors[i] >= pos_counts[i])
			{
				/*
				 * Term i exhausted — no more
				 * possible matches.
				 */
				return false;
			}

			if (pos_lists[i][cursors[i]] != target)
			{
				match = false;
				break;
			}
		}

		if (match)
			return true;

		cursors[0]++;
	}

	return false;
}

/*
 * Parse a query string to detect phrase syntax.
 *
 * If the query text is enclosed in double quotes (e.g.,
 * '"full text search"'), this function strips the quotes
 * and returns true.  The caller should then split the
 * inner text by whitespace to get the phrase terms.
 *
 * out_text:    receives pointer to the phrase content
 *              (within the input string, not copied)
 * out_len:     receives length of phrase content
 *
 * Returns true if phrase syntax was detected, false for
 * a normal multi-term query.
 */
static inline bool
tp_parse_phrase_query(
		const char *query_text,
		int query_len,
		const char **out_text,
		int *out_len)
{
	/* Must be at least 3 chars: '"x"' */
	if (query_len < 3)
		return false;

	if (query_text[0] == '"' &&
		query_text[query_len - 1] == '"')
	{
		*out_text = query_text + 1;
		*out_len  = query_len - 2;
		return true;
	}

	return false;
}

/*
 * Split a phrase string into individual terms.
 *
 * Splits on whitespace. Returns the number of terms found.
 * terms[] receives pointers into the input string (not copied).
 * term_lens[] receives the length of each term.
 * max_terms limits the output array size.
 *
 * Note: modifies the input string in-place by inserting NUL
 * terminators at whitespace boundaries.
 */
static inline int
tp_split_phrase_terms(
		char *phrase_text,
		int phrase_len,
		char **terms,
		int *term_lens,
		int max_terms)
{
	int count = 0;
	int i     = 0;

	while (i < phrase_len && count < max_terms)
	{
		/* Skip whitespace */
		while (i < phrase_len &&
			   (phrase_text[i] == ' ' ||
				phrase_text[i] == '	'))
			i++;

		if (i >= phrase_len)
			break;

		/* Start of term */
		terms[count] = &phrase_text[i];

		/* Find end of term */
		while (i < phrase_len &&
			   phrase_text[i] != ' ' &&
			   phrase_text[i] != '	')
			i++;

		term_lens[count] = (int)(&phrase_text[i] - terms[count]);

		/* NUL-terminate */
		if (i < phrase_len)
			phrase_text[i++] = '\0';

		count++;
	}

	return count;
}
