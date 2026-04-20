/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * format.h - On-disk format definitions for segment storage
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/timestamp.h>

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
 * Segment format versions
 */
#define TP_SEGMENT_FORMAT_VERSION_3 3 /* Legacy: uint32 offsets */
#define TP_SEGMENT_FORMAT_VERSION_4 4 /* Legacy: no alive bitset */
#define TP_SEGMENT_FORMAT_VERSION	5 /* Current: alive bitset */

/*
 * V3 legacy segment header - preserved for reading old segments.
 * All offsets are uint32, limiting segments to 4GB.
 */
typedef struct TpSegmentHeaderV3
{
	uint32		magic;
	uint32		version;
	TimestampTz created_at;
	uint32		num_pages;
	uint32		data_size;
	uint32		level;
	BlockNumber next_segment;
	uint32		dictionary_offset;
	uint32		strings_offset;
	uint32		entries_offset;
	uint32		postings_offset;
	uint32		skip_index_offset;
	uint32		fieldnorm_offset;
	uint32		ctid_pages_offset;
	uint32		ctid_offsets_offset;
	uint32		num_terms;
	uint32		num_docs;
	uint64		total_tokens;
	BlockNumber page_index;
} TpSegmentHeaderV3;

/*
 * V4 legacy segment header - preserved for reading old segments.
 * Same as V5 but without alive_bitset_offset / alive_count.
 */
typedef struct TpSegmentHeaderV4
{
	uint32		magic;
	uint32		version;
	TimestampTz created_at;
	uint32		num_pages;
	uint64		data_size;
	uint32		level;
	BlockNumber next_segment;
	uint64		dictionary_offset;
	uint64		strings_offset;
	uint64		entries_offset;
	uint64		postings_offset;
	uint64		skip_index_offset;
	uint64		fieldnorm_offset;
	uint64		ctid_pages_offset;
	uint64		ctid_offsets_offset;
	uint32		num_terms;
	uint32		num_docs;
	uint64		total_tokens;
	BlockNumber page_index;
} TpSegmentHeaderV4;

/*
 * Segment header - stored on the first page (V5: alive bitset)
 */
typedef struct TpSegmentHeader
{
	/* Metadata */
	uint32		magic;		/* TP_SEGMENT_MAGIC */
	uint32		version;	/* Format version */
	TimestampTz created_at; /* Creation time */
	uint32		num_pages;	/* Total pages in segment */
	uint64		data_size;	/* Total data bytes */

	/* Segment management */
	uint32		level;		  /* Storage level (0 for memtable flush) */
	BlockNumber next_segment; /* Next segment in chain */

	/* Section offsets in logical file */
	uint64 dictionary_offset; /* Offset to TpDictionary */
	uint64 strings_offset;	  /* Offset to string pool */
	uint64 entries_offset;	  /* Offset to TpDictEntry array */
	uint64 postings_offset;	  /* Offset to posting data (blocks) */

	/* Block storage offsets */
	uint64 skip_index_offset;	/* Offset to skip index */
	uint64 fieldnorm_offset;	/* Offset to fieldnorm table */
	uint64 ctid_pages_offset;	/* Offset to BlockNumber array */
	uint64 ctid_offsets_offset; /* Offset to OffsetNumber array */

	/* Alive bitset for tombstone tracking (V5+) */
	uint64 alive_bitset_offset; /* Offset to alive bitset data */
	uint32 alive_count;			/* Number of alive docs (bits set) */

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
 * V3 legacy dictionary entry - 12 bytes
 */
typedef struct TpDictEntryV3
{
	uint32 skip_index_offset;
	uint16 block_count;
	uint16 reserved;
	uint32 doc_freq;
} __attribute__((aligned(4))) TpDictEntryV3;

/*
 * Dictionary entry - 16 bytes, block-based storage (V4: uint64 offset)
 *
 * Points to skip index instead of raw postings. The skip index
 * contains block_count TpSkipEntry structures, each pointing to
 * a block of up to TP_BLOCK_SIZE postings.
 *
 * block_count was widened from uint16 to uint32 without a format
 * version bump.  The old layout had uint16 block_count + uint16
 * reserved (always 0) at bytes 8-11.  On little-endian platforms
 * (x86-64, ARM64) these four bytes read identically as a uint32,
 * so existing V4 segments are binary-compatible.
 */
typedef struct TpDictEntry
{
	uint64 skip_index_offset; /* Offset to TpSkipEntry array for this term */
	uint32 block_count;		  /* Number of blocks (and skip entries) */
	uint32 doc_freq;		  /* Document frequency for IDF */
} __attribute__((aligned(8))) TpDictEntry;

/*
 * Block storage constants
 */
#define TP_BLOCK_SIZE 128 /* Documents per block (matches Tantivy) */

/*
 * V3 legacy skip index entry - 16 bytes per block
 */
typedef struct TpSkipEntryV3
{
	uint32 last_doc_id;
	uint8  doc_count;
	uint16 block_max_tf;
	uint8  block_max_norm;
	uint32 posting_offset;
	uint8  flags;
	uint8  reserved[3];
} __attribute__((packed)) TpSkipEntryV3;

/*
 * Skip index entry - 20 bytes per block (V4: uint64 posting_offset)
 *
 * Stored separately from posting data for cache efficiency during BMW.
 * The skip index is a dense array of these entries, one per block.
 */
typedef struct TpSkipEntry
{
	uint32 last_doc_id;	   /* Last segment-local doc ID in block */
	uint8  doc_count;	   /* Number of docs in block (1-128) */
	uint16 block_max_tf;   /* Max term frequency in block (for BMW) */
	uint8  block_max_norm; /* Min fieldnorm in block (shortest doc, for BMW) */
	uint64 posting_offset; /* Byte offset from segment start to block data */
	uint8  flags;		   /* Compression type, etc. */
	uint8  reserved[3];	   /* Future use */
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
