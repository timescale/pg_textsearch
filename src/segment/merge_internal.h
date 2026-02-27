/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * merge_internal.h - Internal merge types and helpers
 *
 * Exposes merge internals needed by build_parallel.c for BufFile
 * merge. These types and functions are not part of the public API.
 */
#pragma once

#include <postgres.h>

#include <storage/buffile.h>

#include "segment.h"
#include "segment_io.h"

/*
 * Merge source state - tracks current position in each source segment
 */
typedef struct TpMergeSource
{
	TpSegmentReader *reader;		 /* Segment reader */
	uint32			 current_idx;	 /* Current term index in dictionary */
	uint32			 num_terms;		 /* Total terms in this segment */
	char			*current_term;	 /* Current term text (palloc'd) */
	TpDictEntry		 current_entry;	 /* dictionary entry */
	bool			 exhausted;		 /* True if no more terms */
	uint32			*string_offsets; /* Cached string offsets array */
} TpMergeSource;

/*
 * Reference to a segment that contains a particular term.
 */
typedef struct TpTermSegmentRef
{
	int			segment_idx; /* Index into sources array */
	TpDictEntry entry;		 /* dict entry from that segment */
} TpTermSegmentRef;

/*
 * Merged term info - tracks which segments have this term.
 */
typedef struct TpMergedTerm
{
	char			 *term;				/* Term text */
	uint32			  term_len;			/* Term length */
	TpTermSegmentRef *segment_refs;		/* Which segments have this term */
	uint32			  num_segment_refs; /* Number of segment refs */
	uint32			  segment_refs_capacity; /* Allocated capacity */
	uint64			  posting_offset;		 /* Offset where postings start */
	uint32			  posting_count;		 /* Number of postings written */
} TpMergedTerm;

/*
 * Current posting info during merge.
 */
typedef struct TpMergePostingInfo
{
	ItemPointerData ctid;		/* CTID for comparison ordering */
	uint32			old_doc_id; /* Doc ID in source segment */
	uint16			frequency;	/* Term frequency */
	uint8			fieldnorm;	/* Encoded fieldnorm (1 byte) */
} TpMergePostingInfo;

/*
 * Posting merge source - tracks position in one segment's posting
 * list for streaming N-way merge.
 */
typedef struct TpPostingMergeSource
{
	TpSegmentReader	  *reader;	  /* Segment reader */
	TpMergePostingInfo current;	  /* Current posting info */
	bool			   exhausted; /* No more postings */

	/* block iteration state */
	uint64			skip_index_offset; /* Offset to skip entries */
	uint16			block_count;	   /* Total blocks */
	uint32			current_block;	   /* Current block index */
	uint32			current_in_block;  /* Position within current block */
	TpSkipEntry		skip_entry;		   /* Current block's skip entry */
	TpBlockPosting *block_postings;	   /* Cached postings for block */
	uint32			block_capacity;	   /* Allocated size */
} TpPostingMergeSource;

/*
 * Mapping from (source_idx, old_doc_id) -> new_doc_id.
 */
typedef struct TpMergeDocMapping
{
	uint32 **old_to_new; /* old_to_new[src_idx][old_doc_id] = new */
	int		 num_sources;
} TpMergeDocMapping;

/*
 * Per-term block info for merge output.
 */
typedef struct MergeTermBlockInfo
{
	uint64 posting_offset;	 /* Offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into skip entries array */
} MergeTermBlockInfo;

/* Forward declaration */
struct TpDocMapBuilder;

/*
 * Merge source operations
 */
extern bool merge_source_advance(TpMergeSource *source);
extern bool
merge_source_init(TpMergeSource *source, Relation index, BlockNumber root);
extern bool
merge_source_init_from_reader(TpMergeSource *source, TpSegmentReader *reader);
extern void merge_source_close(TpMergeSource *source);

/*
 * Term merge operations
 */
extern int	merge_find_min_source(TpMergeSource *sources, int num_sources);
extern void merged_term_add_segment_ref(
		TpMergedTerm *term, int segment_idx, TpDictEntry *entry);

/*
 * Posting merge operations
 */
extern void posting_source_init(
		TpPostingMergeSource *ps, TpSegmentReader *reader, TpDictEntry *entry);
extern void posting_source_init_fast(
		TpPostingMergeSource *ps, TpSegmentReader *reader, TpDictEntry *entry);
extern void posting_source_free(TpPostingMergeSource *ps);
extern bool posting_source_advance(TpPostingMergeSource *ps);
extern bool posting_source_advance_fast(TpPostingMergeSource *ps);
extern int
find_min_posting_source(TpPostingMergeSource *sources, int num_sources);

/*
 * Docmap merge operations
 */
extern struct TpDocMapBuilder *build_merged_docmap(
		TpMergeSource	  *sources,
		int				   num_sources,
		TpMergeDocMapping *mapping,
		bool			   disjoint_sources);
extern void free_merge_doc_mapping(TpMergeDocMapping *mapping);

/*
 * Posting source initialization for a term
 */
extern TpPostingMergeSource *init_term_posting_sources(
		TpMergedTerm *term, TpMergeSource *sources, int *num_psources);
extern TpPostingMergeSource *init_term_posting_sources_fast(
		TpMergedTerm *term, TpMergeSource *sources, int *num_psources);
