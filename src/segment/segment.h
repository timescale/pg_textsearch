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

#include "postgres.h"

#include "access/htup_details.h"
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
 * Special area for page index pages (goes in page special area)
 */
typedef struct TpPageIndexSpecial
{
	BlockNumber next_page; /* Next page in chain, InvalidBlockNumber if last */
	uint16		page_type; /* TpPageType enum value */
	uint16		num_entries; /* Number of BlockNumber entries on this page */
	uint16		flags;		 /* Reserved for future use */
} TpPageIndexSpecial;

/*
 * Segment header - stored on the first page
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
	BlockNumber next_segment; /* Next segment in chain (for multi-segment
								 support) */

	/* Section offsets in logical file */
	uint32 dictionary_offset;  /* Offset to TpDictionary */
	uint32 strings_offset;	   /* Offset to string pool */
	uint32 entries_offset;	   /* Offset to TpDictEntry array */
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
 * Dictionary structure for fast term lookup
 *
 * The dictionary is a sorted array of string offsets, enabling binary search.
 * Each string is stored as:
 * [length:uint32][text:char*][dict_entry_offset:uint32] This allows us to
 * quickly find a term and jump to its TpDictEntry.
 */
typedef struct TpDictionary
{
	uint32 num_terms; /* Number of terms in dictionary */
	uint32 string_offsets[FLEXIBLE_ARRAY_MEMBER]; /* Sorted array of offsets to
													 strings */
} TpDictionary;

/*
 * String entry in string pool
 */
typedef struct TpStringEntry
{
	uint32 length;						/* String length */
	char   text[FLEXIBLE_ARRAY_MEMBER]; /* String text (variable length) */
	/* Immediately after text: uint32 dict_entry_offset */
} TpStringEntry;

/*
 * Dictionary entry - 12 bytes, compact without string info
 */
typedef struct TpDictEntry
{
	uint32 posting_offset; /* Logical offset to posting list */
	uint32 posting_count;  /* Number of postings */
	uint32 doc_freq;	   /* Document frequency for IDF */
} __attribute__((aligned(4))) TpDictEntry;

/*
 * Posting entry - 8 bytes, packed
 */
typedef struct TpSegmentPosting
{
	ItemPointerData ctid;	   /* 6 bytes - heap tuple ID */
	uint16			frequency; /* 2 bytes - term frequency */
} __attribute__((packed)) TpSegmentPosting;

/*
 * Document length - 12 bytes (padded to 16)
 */
typedef struct TpDocLength
{
	ItemPointerData ctid;	  /* 6 bytes */
	uint16			length;	  /* 2 bytes - total terms in doc */
	uint32			reserved; /* 4 bytes padding */
} TpDocLength;

/*
 * Segment reader context
 */

typedef struct TpSegmentReader
{
	Relation	index;
	BlockNumber root_block;

	/* Cached header */
	TpSegmentHeader *header;
	Buffer			 header_buffer;

	/* Cached page map loaded from page index */
	BlockNumber *page_map;
	uint32		 num_pages;

	/* Current data page */
	Buffer current_buffer;
	uint32 current_logical_page;
} TpSegmentReader;

/*
 * Segment writer context
 */
typedef struct TpSegmentWriter
{
	Relation	 index;
	BlockNumber *pages; /* Dynamically allocated array of page blocks */
	uint32		 pages_allocated; /* Number of pages allocated so far */
	uint32		 pages_capacity;  /* Capacity of the pages array */
	uint32		 current_offset;  /* Current write position in logical file */
	char		*buffer;		  /* Write buffer (one page) */
	uint32		 buffer_page;	  /* Which logical page is in buffer */
	uint32		 buffer_pos;	  /* Position within buffer */

	/* Reusable buffer for posting list conversion */
	TpSegmentPosting *posting_buffer;	   /* Reusable posting buffer */
	uint32			  posting_buffer_size; /* Current size of buffer */
} TpSegmentWriter;

/* Forward declarations for index.c */
struct TpLocalIndexState;

/*
 * Function declarations
 */

/* Writer functions */
extern BlockNumber
			tp_write_segment(struct TpLocalIndexState *state, Relation index);
extern void tp_segment_writer_init(TpSegmentWriter *writer, Relation index);
extern void
tp_segment_writer_write(TpSegmentWriter *writer, const void *data, uint32 len);
extern void tp_segment_writer_flush(TpSegmentWriter *writer);
extern void tp_segment_writer_finish(TpSegmentWriter *writer);

/* Reader functions */
extern TpSegmentReader *tp_segment_open(Relation index, BlockNumber root);
extern void				tp_segment_read(
					TpSegmentReader *reader,
					uint32			 logical_offset,
					void			*dest,
					uint32			 len);
extern void tp_segment_close(TpSegmentReader *reader);

/* Zero-copy reader functions */
typedef struct TpSegmentDirectAccess
{
	Buffer buffer;	  /* Pinned buffer */
	Page   page;	  /* Page pointer */
	void  *data;	  /* Pointer to data in page */
	uint32 available; /* Bytes available from this position */
} TpSegmentDirectAccess;

extern bool tp_segment_get_direct(
		TpSegmentReader		  *reader,
		uint32				   logical_offset,
		uint32				   len,
		TpSegmentDirectAccess *access);
extern void tp_segment_release_direct(TpSegmentDirectAccess *access);

/* Document length lookup */
extern int32
tp_segment_get_document_length(TpSegmentReader *reader, ItemPointer ctid);

/* Debug functions */
extern void tp_debug_dump_segment_internal(
		const char *index_name, BlockNumber segment_root);
extern void tp_segment_dump_to_file(
		const char *index_name,
		BlockNumber segment_root,
		const char *filename);

/* Segment analyzer functions */
extern void
tp_analyze_index_to_file(const char *index_name, const char *filename);
extern void
tp_analyze_segment_chain(FILE *fp, Relation index, BlockNumber first_segment);
extern void tp_analyze_dictionary(FILE *fp, struct TpSegmentReader *reader);

/* Zero-copy query execution - defined in segment_query.c */
struct TpLocalIndexState; /* Forward declaration */
typedef struct DocumentScoreEntry
{
	ItemPointerData ctid;
	float4			score;
	float4			doc_length;
} DocumentScoreEntry;

extern void tp_process_term_in_segments(
		Relation				  index,
		BlockNumber				  first_segment,
		const char				 *term,
		int32					  total_docs,
		float8					  avg_idf,
		float4					  query_frequency,
		float4					  k1,
		float4					  b,
		float4					  avg_doc_len,
		void					 *doc_scores_hash, /* HTAB* */
		struct TpLocalIndexState *local_state);

#endif /* SEGMENT_H */
