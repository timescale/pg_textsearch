/*-------------------------------------------------------------------------
 *
 * segment.h
 *	  Disk-based segment structures and operations for pg_textsearch
 *
 * IDENTIFICATION
 *	  src/segment.h
 *
 *-------------------------------------------------------------------------
 */
#pragma once

#include <postgres.h>

#include <storage/block.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/rel.h>
#include <utils/timestamp.h>

#include "constants.h"

/* Magic number for segment validation */
#define TP_SEGMENT_MAGIC   0x54505347 /* "TPSG" */
#define TP_SEGMENT_VERSION 1

/* Forward declarations */
typedef struct TpLocalIndexState TpLocalIndexState;
typedef struct TpPostingList	 TpPostingList;

/*
 * Segment header with embedded page mapping table.
 * Stored at the beginning of the root page.
 */
typedef struct TpSegmentHeader
{
	/* Metadata */
	uint32 magic;	  /* TP_SEGMENT_MAGIC */
	uint32 version;	  /* Format version */
	uint32 num_pages; /* Total pages in segment */
	uint32 data_size; /* Total data bytes */

	/* Statistics */
	uint32 num_terms;		 /* Number of unique terms */
	uint32 num_docs;		 /* Number of documents */
	uint64 total_terms;		 /* Total term occurrences */
	uint64 total_doc_length; /* Sum of all document lengths */

	/* Section offsets (logical byte offsets) */
	uint32 dict_offset;		/* Dictionary start */
	uint32 dict_size;		/* Dictionary size */
	uint32 postings_offset; /* Posting lists start */
	uint32 postings_size;	/* Posting lists size */
	uint32 doclens_offset;	/* Document lengths start */
	uint32 doclens_size;	/* Document lengths size */
	uint32 strings_offset;	/* String pool start */
	uint32 strings_size;	/* String pool size */

	/* Segment management */
	BlockNumber next_segment; /* Next segment in index */
	TimestampTz created_at;	  /* Creation time */
	uint32		level;		  /* LSM level (0 for memtable flush) */

	/* Page mapping table - maps logical page -> physical BlockNumber */
	BlockNumber page_map[FLEXIBLE_ARRAY_MEMBER];
} TpSegmentHeader;

/*
 * Dictionary entry - fixed size for binary search.
 * Zero-copy: can be used directly from disk.
 */
typedef struct TpDictEntry
{
	uint32 term_hash;	   /* Pre-computed hash for fast comparison */
	uint32 string_offset;  /* Logical offset in string pool */
	uint32 string_len;	   /* Term length */
	uint32 posting_offset; /* Logical offset to posting list */
	uint32 posting_count;  /* Number of postings */
	uint32 doc_freq;	   /* Document frequency for IDF */
} __attribute__((aligned(8))) TpDictEntry;

/*
 * Posting entry - compact representation.
 * Zero-copy: can be used directly from disk.
 */
typedef struct TpSegmentPosting
{
	ItemPointerData ctid;	   /* 6 bytes - heap tuple ID */
	uint16			frequency; /* 2 bytes - term frequency */
} __attribute__((packed)) TpSegmentPosting;

/*
 * Document length entry.
 * Zero-copy: can be used directly from disk.
 */
typedef struct TpDocLength
{
	ItemPointerData ctid;	  /* 6 bytes */
	uint16			length;	  /* 2 bytes - total terms in doc */
	uint32			reserved; /* 4 bytes padding for alignment */
} TpDocLength;

/* Maximum pages that fit in root page header */
#define TP_MAX_PAGES_IN_HEADER \
	((BLCKSZ - offsetof(TpSegmentHeader, page_map)) / sizeof(BlockNumber))

/*
 * Segment reader structure
 */
typedef struct TpSegmentReader
{
	Relation	index;
	BlockNumber root_block;

	/* Cached header with page map */
	TpSegmentHeader *header;
	Buffer			 header_buffer;

	/* Current data page */
	Buffer current_buffer;
	uint32 current_logical_page;
} TpSegmentReader;

/* Function declarations */

/* Reader operations */
extern TpSegmentReader			   *
tp_segment_open(Relation index, BlockNumber root_block);
extern void tp_segment_close(TpSegmentReader *reader);
extern void tp_segment_read(
		TpSegmentReader *reader,
		uint32			 logical_offset,
		void			*dest,
		uint32			 len);
extern TpPostingList *
tp_segment_get_posting_list(TpSegmentReader *reader, const char *term);

/* Writer operations (in flush.c) */
extern BlockNumber
tp_flush_memtable_to_segment(TpLocalIndexState *state, Relation index);

/* Utility functions */
extern uint32 tp_segment_estimate_size(TpLocalIndexState *state);
