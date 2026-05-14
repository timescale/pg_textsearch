/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * dictionary.h - Term dictionary for disk segments
 */
#pragma once

#include "postgres.h"
#include "storage/itemptr.h"

/* Forward declarations */
struct TpDataSource;
struct TpSegmentReader;
struct TpSegmentHeader;

/*
 * Term info for building a segment dictionary.
 *
 * The ctids/freqs arrays are owned by the caller (typically the
 * caller's memory context, e.g. the chain source's private mcxt that
 * already holds the parallel ctid/freq arrays).  tp_write_segment
 * reads them directly; it does not free or take ownership.
 */
typedef struct TermInfo
{
	char			*term;			 /* The term text */
	uint32			 term_len;		 /* Term length */
	ItemPointerData *ctids;			 /* Array of doc CTIDs (len = count) */
	int32			*freqs;			 /* Array of term freqs (len = count) */
	uint32			 count;			 /* Length of ctids/freqs */
	uint32			 doc_freq;		 /* Document frequency (used for IDF) */
	uint32			 dict_entry_idx; /* Index in dict_entries array */
} TermInfo;

/*
 * Free a TermInfo[] previously returned by a dictionary builder.
 *
 * Only the `term` strings are freed; the ctids/freqs arrays are
 * presumed to live in the source's memory context and are dropped
 * when that context is reset.
 */
extern void tp_free_dictionary(TermInfo *terms, uint32 num_terms);

/* Shared term-reading helper */
extern char *tp_segment_read_term_at_index(
		struct TpSegmentReader *reader,
		struct TpSegmentHeader *header,
		uint32				   *string_offsets,
		uint32					index);
