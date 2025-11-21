/*-------------------------------------------------------------------------
 *
 * segment.h
 *    Disk-based segment structures for pg_textsearch
 *
 * IDENTIFICATION
 *    src/segment/segment.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SEGMENT_H
#define SEGMENT_H

#include "access/htup_details.h"
#include "postgres.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/timestamp.h"

/*
 * Magic number for segment validation
 */
#define TP_SEGMENT_MAGIC   0x54415049 /* "TAPI" */
#define TP_SEGMENT_VERSION 1		  /* Segment format version */

/*
 * Page types for segment pages
 */
typedef enum TpPageType
{
	TP_PAGE_SEGMENT_HEADER = 1, /* Segment header page */
	TP_PAGE_FILE_INDEX,			/* File index (page map) pages */
	TP_PAGE_DATA				/* Data pages containing actual content */
} TpPageType;

/*
 * Segment header - stored on the first page
 * This will be the main metadata structure for disk segments
 */
typedef struct TpSegmentHeader
{
	/* Metadata */
	uint32		magic;		/* TP_SEGMENT_MAGIC */
	uint32		version;	/* Format version */
	TimestampTz created_at; /* Creation time */
	uint32		num_pages;	/* Total pages in segment */
	uint32		data_size;	/* Total data bytes */

	/* Segment management */
	uint32		level;		  /* Storage level (0 for memtable flush) */
	BlockNumber next_segment; /* Next segment in chain */

	/* Section offsets in logical file */
	uint32 dictionary_offset;  /* Offset to dictionary */
	uint32 strings_offset;	   /* Offset to string pool */
	uint32 entries_offset;	   /* Offset to entries array */
	uint32 postings_offset;	   /* Offset to posting lists */
	uint32 doc_lengths_offset; /* Offset to document lengths */

	/* Corpus statistics */
	uint32 num_terms;	 /* Total unique terms */
	uint32 num_docs;	 /* Total documents */
	uint64 total_tokens; /* Sum of all document lengths */

	/* Page index reference */
	BlockNumber page_index; /* First page of the page index */
} TpSegmentHeader;

/*
 * Document score entry used for search results
 */
typedef struct DocumentScoreEntry
{
	uint32 doc_id;
	float4 score;
} DocumentScoreEntry;

/* Forward declarations for future implementation */
struct TpLocalIndexState;

/* Segment management functions - stub declarations for now */
extern bool tp_segment_exists(struct TpLocalIndexState *state);
extern void tp_flush_to_segment(struct TpLocalIndexState *state);
extern DocumentScoreEntry *tp_segment_search(
		struct TpLocalIndexState *state,
		const char				**query_terms,
		int						  num_query_terms,
		int						 *num_results);

#endif /* SEGMENT_H */
