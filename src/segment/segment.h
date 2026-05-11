/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment.h - Disk-based segment structures
 */
#pragma once

#include <postgres.h>

#include <access/htup_details.h>
#include <port/atomics.h>
#include <storage/buffile.h>

#include "constants.h"
#include "segment/format.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/timestamp.h"

/*
 * BufFile segment size (1 GB).  BufFile stores data across
 * multiple 1 GB physical files.  Composite offsets encode both
 * fileno and position: fileno * TP_BUFFILE_SEG_SIZE + file_offset.
 */
#define TP_BUFFILE_SEG_SIZE ((uint64)(1024 * 1024 * 1024))

static inline uint64
tp_buffile_composite_offset(int fileno, off_t file_offset)
{
	return (uint64)fileno * TP_BUFFILE_SEG_SIZE + (uint64)file_offset;
}

static inline void
tp_buffile_decompose_offset(uint64 composite, int *fileno, off_t *offset)
{
	*fileno = (int)(composite / TP_BUFFILE_SEG_SIZE);
	*offset = (off_t)(composite % TP_BUFFILE_SEG_SIZE);
}

/*
 * Segment posting - output format for iteration
 * 14 bytes, packed. Used when converting block postings for scoring.
 *
 * doc_id is segment-local, used for deferred CTID resolution in lazy loading.
 * When CTIDs are pre-loaded, ctid is set immediately. When lazy loading,
 * ctid is invalid and doc_id is used to look up CTID at result extraction.
 */
typedef struct TpSegmentPosting
{
	ItemPointerData ctid;	   /* 6 bytes - heap tuple ID (may be invalid) */
	uint32			doc_id;	   /* 4 bytes - segment-local doc ID */
	uint16			frequency; /* 2 bytes - term frequency */
	uint16 doc_length;		   /* 2 bytes - document length (from fieldnorm) */
} __attribute__((packed)) TpSegmentPosting;

/* Version-aware struct size helpers */
static inline size_t
tp_dict_entry_size(uint32 version)
{
	return (version <= TP_SEGMENT_FORMAT_VERSION_3) ? sizeof(TpDictEntryV3)
													: sizeof(TpDictEntry);
}

static inline size_t
tp_skip_entry_size(uint32 version)
{
	return (version <= TP_SEGMENT_FORMAT_VERSION_3) ? sizeof(TpSkipEntryV3)
													: sizeof(TpSkipEntry);
}

/*
 * Document length - 12 bytes (padded to 16)
 */
typedef struct TpDocLength
{
	ItemPointerData ctid;	  /* 6 bytes */
	uint16			length;	  /* 2 bytes - total terms in doc */
	uint32			reserved; /* 4 bytes padding */
} TpDocLength;

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

/*
 * Mark a segment buffer dirty and immediately emit a full-page WAL image when
 * WAL is required for the relation.  Segment pages use custom layouts, so the
 * full image must not use REGBUF_STANDARD hole compression.
 *
 * This helper is intended for callers that have just populated an entire
 * segment page from scratch (e.g. tp_segment_writer_flush,
 * write_page_index_internal), where a full-page image is necessary anyway.
 * For small in-place updates to an already-written segment page (dict-entry
 * backpatches, header patches), prefer GenericXLog delta logging — it
 * produces much smaller WAL records.
 *
 * Callers must hold an exclusive buffer lock and must not perform any
 * ERROR-capable work between the final page modification and this call.
 */
extern void tp_segment_log_dirty_buffer(Relation index, Buffer buffer);
