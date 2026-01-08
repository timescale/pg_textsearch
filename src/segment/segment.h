/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment.h - Disk-based segment structures
 */
#pragma once

#include <postgres.h>

#include <access/htup_details.h>

#include "constants.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/timestamp.h"

/*
 * Page types for segment pages.
 * Magic and version constants are defined in constants.h.
 */
typedef enum TpPageType
{
	TP_PAGE_SEGMENT_HEADER = 1, /* Segment header page */
	TP_PAGE_FILE_INDEX,			/* File index (page map) pages */
	TP_PAGE_DATA				/* Data pages containing actual content */
} TpPageType;

/*
 * Special area for page index pages (goes in page special area).
 * Contains magic/version for validation.
 */
typedef struct TpPageIndexSpecial
{
	uint32		magic;	   /* TP_PAGE_INDEX_MAGIC */
	uint16		version;   /* TP_PAGE_INDEX_VERSION */
	uint16		page_type; /* TpPageType enum value */
	BlockNumber next_page; /* Next page in chain, InvalidBlockNumber if last */
	uint16		num_entries; /* Number of BlockNumber entries on this page */
	uint16		flags;		 /* Reserved for future use */
} TpPageIndexSpecial;

/*
 * Segment format version
 */
#define TP_SEGMENT_FORMAT_VERSION 2 /* Block storage with skip index */

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
	BlockNumber next_segment; /* Next segment in chain */

	/* Section offsets in logical file */
	uint32 dictionary_offset; /* Offset to TpDictionary */
	uint32 strings_offset;	  /* Offset to string pool */
	uint32 entries_offset;	  /* Offset to TpDictEntry array */
	uint32 postings_offset;	  /* Offset to posting data (blocks) */

	/* Block storage offsets */
	uint32 skip_index_offset;	/* Offset to skip index (TpSkipEntry arrays) */
	uint32 fieldnorm_offset;	/* Offset to fieldnorm table (1 byte/doc) */
	uint32 ctid_pages_offset;	/* Offset to BlockNumber array (4 bytes/doc) */
	uint32 ctid_offsets_offset; /* Offset to OffsetNumber array (2 bytes/doc)
								 */

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
 *
 * TODO: Optimize storage for short strings. Current overhead is 8 bytes per
 * term (4-byte length + 4-byte dict_entry_offset). Since most stemmed terms
 * are short (3-10 chars), this overhead is significant. Options:
 * - Use 1-byte length (terms rarely > 255 chars), saves 3 bytes/term
 * - Eliminate dict_entry_offset by computing from term index, saves 4
 *   bytes/term
 * - Use varint encoding for length
 */
typedef struct TpStringEntry
{
	uint32 length;						/* String length */
	char   text[FLEXIBLE_ARRAY_MEMBER]; /* String text (variable length) */
	/* Immediately after text: uint32 dict_entry_offset */
} TpStringEntry;

/*
 * Dictionary entry - 12 bytes, block-based storage
 *
 * Points to skip index instead of raw postings. The skip index
 * contains block_count TpSkipEntry structures, each pointing to
 * a block of up to TP_BLOCK_SIZE postings.
 */
typedef struct TpDictEntry
{
	uint32 skip_index_offset; /* Offset to TpSkipEntry array for this term */
	uint16 block_count;		  /* Number of blocks (and skip entries) */
	uint16 reserved;		  /* Padding */
	uint32 doc_freq;		  /* Document frequency for IDF */
} __attribute__((aligned(4))) TpDictEntry;

/*
 * Segment posting - output format for iteration
 * 10 bytes, packed. Used when converting block postings for scoring.
 */
typedef struct TpSegmentPosting
{
	ItemPointerData ctid;	   /* 6 bytes - heap tuple ID */
	uint16			frequency; /* 2 bytes - term frequency */
	uint16 doc_length;		   /* 2 bytes - document length (from fieldnorm) */
} __attribute__((packed)) TpSegmentPosting;

/*
 * Block storage constants
 */
#define TP_BLOCK_SIZE 128 /* Documents per block (matches Tantivy) */

/*
 * Skip index entry - 16 bytes per block
 *
 * Stored separately from posting data for cache efficiency during BMW.
 * The skip index is a dense array of these entries, one per block.
 */
typedef struct TpSkipEntry
{
	uint32 last_doc_id;	   /* Last segment-local doc ID in block */
	uint8  doc_count;	   /* Number of docs in block (1-128) */
	uint16 block_max_tf;   /* Max term frequency in block (for BMW) */
	uint8  block_max_norm; /* Fieldnorm ID of max-scoring doc (for BMW) */
	uint32 posting_offset; /* Byte offset from segment start to block data */
	uint8  flags;		   /* Compression type, etc. */
	uint8  reserved[3];	   /* Future use, ensures 16-byte alignment */
} __attribute__((packed)) TpSkipEntry;

/* Skip entry flags */
#define TP_BLOCK_FLAG_UNCOMPRESSED 0x00 /* Raw doc IDs and frequencies */
#define TP_BLOCK_FLAG_DELTA		   0x01 /* Delta-encoded doc IDs */
#define TP_BLOCK_FLAG_FOR		   0x02 /* Frame-of-reference (Phase 3) */
#define TP_BLOCK_FLAG_PFOR		   0x03 /* Patched FOR (Phase 3) */

/*
 * Block posting entry - 8 bytes, used in uncompressed blocks
 *
 * Unlike TpSegmentPosting, uses segment-local doc ID instead of CTID.
 * CTID lookup happens via the doc ID -> CTID mapping table.
 *
 * Fieldnorm is stored inline (using former padding bytes) to avoid
 * per-posting buffer manager lookups during scoring. This is critical
 * for performance: each buffer manager access adds ~300-500ns even when
 * pages are cached, which dominates query time for large posting lists.
 */
typedef struct TpBlockPosting
{
	uint32 doc_id;	  /* Segment-local document ID */
	uint16 frequency; /* Term frequency in document */
	uint8  fieldnorm; /* Quantized document length (Lucene SmallFloat) */
	uint8  reserved;  /* Padding for alignment */
} TpBlockPosting;

/*
 * CTID map entry - 6 bytes per document
 *
 * Maps segment-local doc IDs to heap CTIDs. This enables using
 * compact 4-byte doc IDs in posting lists while still being able
 * to look up the actual heap tuple.
 */
typedef struct TpCtidMapEntry
{
	ItemPointerData ctid; /* 6 bytes - heap tuple location */
} __attribute__((packed)) TpCtidMapEntry;

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
	BlockNumber	 nblocks; /* Relation size for validation */

	/* Current data page */
	Buffer current_buffer;
	uint32 current_logical_page;

	/*
	 * CTID arrays for result lookup (loaded at segment open).
	 * Split storage for better packing and cache locality during scoring.
	 */
	BlockNumber	 *cached_ctid_pages;   /* Page numbers (4 bytes/doc) */
	OffsetNumber *cached_ctid_offsets; /* Tuple offsets (2 bytes/doc) */
	uint32		  cached_num_docs;	   /* Number of docs cached */
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

/* Debug functions */
struct DumpOutput; /* Forward declaration */
extern void tp_dump_segment_to_output(
		Relation index, BlockNumber segment_root, struct DumpOutput *out);

/* Page index writing (used by segment_merge.c) */
extern BlockNumber
write_page_index(Relation index, BlockNumber *pages, uint32 num_pages);

/* Page reclamation for segment compaction */
extern uint32 tp_segment_collect_pages(
		Relation index, BlockNumber root_block, BlockNumber **pages_out);
extern void
tp_segment_free_pages(Relation index, BlockNumber *pages, uint32 num_pages);

/* FSM statistics reporting (for debugging) */
extern void tp_report_fsm_stats(void);

/* Zero-copy query execution - defined in segment_query.c */
struct TpLocalIndexState;  /* Forward declaration */
struct DocumentScoreEntry; /* Defined in types/score.h */

extern void tp_process_term_in_segments(
		Relation				  index,
		BlockNumber				  first_segment,
		const char				 *term,
		float4					  idf, /* Pre-computed IDF using unified df */
		float4					  query_frequency,
		float4					  k1,
		float4					  b,
		float4					  avg_doc_len,
		void					 *doc_scores_hash, /* HTAB* */
		struct TpLocalIndexState *local_state);

/* Look up doc_freq for a term from segments (for operator scoring) */
extern uint32 tp_segment_get_doc_freq(
		Relation index, BlockNumber first_segment, const char *term);

/* Batch lookup doc_freq for multiple terms - opens each segment once */
extern void tp_batch_get_segment_doc_freq(
		Relation	index,
		BlockNumber first_segment,
		char	  **terms,
		int			term_count,
		uint32	   *doc_freqs);

/* Efficient: score all terms in a segment chain, opening each segment once */
extern void tp_score_all_terms_in_segment_chain(
		Relation	index,
		BlockNumber first_segment,
		char	  **terms,
		int			term_count,
		int32	   *query_frequencies,
		uint32	   *doc_freqs, /* OUT: filled with segment doc_freqs */
		int32		total_docs,
		float4		k1,
		float4		b,
		float4		avg_doc_len,
		void	   *doc_scores_hash);

/*
 * Segment posting iterator for block-based traversal.
 * Used by BMW scoring to access individual blocks and skip entries.
 */
typedef struct TpSegmentPostingIterator
{
	TpSegmentReader *reader;
	const char		*term;
	uint32			 dict_entry_idx;
	TpDictEntry		 dict_entry;
	bool			 initialized;
	bool			 finished;

	/* Block iteration state */
	uint32		current_block; /* Current block index (0 to block_count-1) */
	uint32		current_in_block; /* Position within current block */
	TpSkipEntry skip_entry;		  /* Current block's skip entry */

	/* Zero-copy block access (preferred path) */
	TpSegmentDirectAccess block_access;
	bool				  has_block_access;

	/* Block postings pointer - points to either direct data or fallback buf */
	TpBlockPosting *block_postings;

	/* Fallback buffer for when block spans page boundaries */
	TpBlockPosting *fallback_block;
	uint32			fallback_block_size;

	/* Output posting (converted for scoring compatibility) */
	TpSegmentPosting output_posting;
} TpSegmentPostingIterator;

/* Segment posting iterator functions */
extern bool tp_segment_posting_iterator_init(
		TpSegmentPostingIterator *iter,
		TpSegmentReader			 *reader,
		const char				 *term);
extern bool
tp_segment_posting_iterator_load_block(TpSegmentPostingIterator *iter);
extern bool tp_segment_posting_iterator_next(
		TpSegmentPostingIterator *iter, TpSegmentPosting **posting);
extern void tp_segment_posting_iterator_free(TpSegmentPostingIterator *iter);

/* Read a skip entry by block index */
extern void tp_segment_read_skip_entry(
		TpSegmentReader *reader,
		TpDictEntry		*dict_entry,
		uint16			 block_idx,
		TpSkipEntry		*skip);

/* Seek iterator to target doc ID (for WAND algorithm) */
extern bool tp_segment_posting_iterator_seek(
		TpSegmentPostingIterator *iter,
		uint32					  target_doc_id,
		TpSegmentPosting		**posting);

/* Get current doc ID from iterator (for WAND pivot selection) */
extern uint32
tp_segment_posting_iterator_current_doc_id(TpSegmentPostingIterator *iter);

/*
 * Unified binary search for term in segment dictionary.
 * Returns term index (0 to num_terms-1) if found, or -1 if not found.
 * If entry_out is not NULL and term is found, populates it with dict entry.
 */
extern int tp_segment_find_term(
		TpSegmentReader *reader, const char *term, TpDictEntry *entry_out);

/*
 * Read a term string at a given dictionary index.
 * Returns palloc'd string that must be freed by caller.
 * Used by merge and dump operations.
 */
extern char *tp_segment_read_term_at_index(
		TpSegmentReader *reader, uint32 index, uint32 *string_offsets);
