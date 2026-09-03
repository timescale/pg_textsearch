/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment_io.h - Segment reader/writer I/O interface
 *
 * Defines TpSegmentReader, TpSegmentWriter, TpSegmentDirectAccess,
 * and TpSegmentPostingIterator types along with all I/O function
 * declarations for reading, writing, and iterating segment data.
 *
 * Requires segment.h for on-disk format types (TpSegmentHeader,
 * TpDictEntry, TpSkipEntry, TpBlockPosting, etc.).
 */
#pragma once

#include "segment/segment.h"

/*
 * Shared overflow guards for the segment write paths.
 *
 * The segment format stores string-pool offsets as uint32, and the
 * build/merge accumulators size their arrays with uint32 capacities.
 * The same arithmetic is needed by the in-memory build path, the
 * parallel-build BufFile path, the single-segment write path, and the
 * merge write path, so it lives here rather than being copied into
 * each of them (issue #432).
 */

/*
 * Size of one string-pool entry: a uint32 length prefix, the term
 * bytes, and the uint32 posting-list offset that follows them.
 */
static inline uint64
tp_string_pool_entry_size(uint32 term_len)
{
	return (uint64)sizeof(uint32) + term_len + sizeof(uint32);
}

/*
 * True when appending entry_size bytes at string_pos would push the
 * string pool past the uint32 offsets used by the segment format.
 * The starting offset and the increment are checked separately so a
 * final term crossing the limit cannot wrap past it unnoticed.
 */
static inline bool
tp_string_pool_offset_overflows(uint64 string_pos, uint64 entry_size)
{
	return string_pos > (uint64)PG_UINT32_MAX ||
		   entry_size > ((uint64)PG_UINT32_MAX + 1) - string_pos;
}

/*
 * Double a uint32 array capacity without wrapping.
 *
 * Doubling a uint32 unchecked wraps to zero once the capacity passes
 * 2^31, which would "grow" the array to a zero-length allocation and
 * let the next indexed write run past its end.  Growth is clamped to
 * PG_UINT32_MAX - 1 instead (PG_UINT32_MAX is reserved as the doc_id
 * sentinel) and errors out once the cap is reached.  A zero capacity
 * starts the array at initial.  what names the elements being counted
 * for the error message.
 */
static inline uint32
tp_grow_capacity(uint32 capacity, uint32 initial, const char *what)
{
	if (capacity == 0)
		return initial;

	if (capacity >= PG_UINT32_MAX - 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("pg_textsearch: too many %s in segment (max %u)",
						what,
						PG_UINT32_MAX - 1)));

	if (capacity > (PG_UINT32_MAX - 1) / 2)
		return PG_UINT32_MAX - 1;

	return capacity * 2;
}

/*
 * Segment reader context
 */

typedef struct TpSegmentReader
{
	Relation	index;
	BlockNumber root_block;
	uint32		segment_version; /* On-disk format version */

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

	/* BufFile-backed reading (for temp file segments, NULL for normal) */
	BufFile *buffile;
	uint64	 buffile_base; /* Base byte offset of segment in BufFile */
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
	uint64		 current_offset;  /* Current write position in logical file */
	char		*buffer;		  /* Write buffer (one page) */
	uint32		 buffer_page;	  /* Which logical page is in buffer */
	uint32		 buffer_pos;	  /* Position within buffer */

	/* Reusable buffer for posting list conversion */
	TpSegmentPosting *posting_buffer;	   /* Reusable posting buffer */
	uint32			  posting_buffer_size; /* Current size of buffer */
} TpSegmentWriter;

/* Forward declarations */
struct TermInfo;
struct TpDocMapBuilder;

/*
 * Function declarations
 */

/* Writer functions */
extern BlockNumber tp_write_segment(
		Relation				index,
		struct TermInfo		   *terms,
		uint32					num_terms,
		struct TpDocMapBuilder *docmap);
extern void tp_segment_writer_init(TpSegmentWriter *writer, Relation index);
extern void
tp_segment_writer_write(TpSegmentWriter *writer, const void *data, Size len);
extern void tp_segment_writer_flush(TpSegmentWriter *writer);
extern void tp_segment_writer_finish(TpSegmentWriter *writer);

/* Reader functions */
extern TpSegmentReader *
tp_segment_open_ex(Relation index, BlockNumber root, bool load_ctids);
extern TpSegmentReader *tp_segment_open(Relation index, BlockNumber root);
extern TpSegmentReader			   *
tp_segment_open_from_buffile(BufFile *file, uint64 base_offset);
extern void tp_segment_read(
		TpSegmentReader *reader,
		uint64			 logical_offset,
		void			*dest,
		uint32			 len);
extern void tp_segment_close(TpSegmentReader *reader);

/* Lazy CTID lookup for deferred resolution */
extern void tp_segment_lookup_ctid(
		TpSegmentReader *reader, uint32 doc_id, ItemPointerData *ctid_out);

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
		uint64				   logical_offset,
		uint32				   len,
		TpSegmentDirectAccess *access);
extern void tp_segment_release_direct(TpSegmentDirectAccess *access);

/* Version-aware dictionary entry reader */
extern void tp_segment_read_dict_entry(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			 index,
		TpDictEntry		*entry);

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

	/*
	 * BMW optimization: cached skip entries and reusable compressed buffer.
	 * When non-NULL, load_block uses these instead of reading skip entries
	 * from disk or palloc/pfree-ing per block.  Set by BMW init code.
	 */
	TpSkipEntry *cached_skip_entries;  /* Pre-loaded skip entries array */
	uint8		*compressed_buf_cache; /* Reusable decompression buffer */

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
		uint64			 skip_index_offset,
		uint32			 block_idx,
		TpSkipEntry		*skip);

/* Seek iterator to target doc ID (for WAND algorithm) */
extern bool tp_segment_posting_iterator_seek(
		TpSegmentPostingIterator *iter,
		uint32					  target_doc_id,
		TpSegmentPosting		**posting);

/* Get current doc ID from iterator (for WAND pivot selection) */
extern uint32
tp_segment_posting_iterator_current_doc_id(TpSegmentPostingIterator *iter);
