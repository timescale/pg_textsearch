/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_context.h - Arena-based build context for CREATE INDEX
 *
 * Replaces the DSA-based memtable during index builds with a
 * process-local arena + EXPULL structure. This eliminates DSA
 * fragmentation, dshash lock overhead, and posting list
 * reallocation copies.
 *
 * The build context is NOT shared between backends — it is used
 * only during serial CREATE INDEX (and per-worker in parallel
 * builds). Concurrent INSERT operations continue to use the
 * shared dshash/DSA memtable.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/hsearch.h>

#include "memtable/arena.h"
#include "memtable/expull.h"
#include "segment/segment.h"

/*
 * Hash table entry for one term during index build.
 * The term string is stored in the arena; the HTAB key
 * is a pointer to it.
 */
typedef struct TpBuildTermEntry
{
	char	*term; /* Key: pointer to arena-stored null-terminated string */
	uint32	 term_len;
	TpExpull expull; /* Inline EXPULL posting list header */
} TpBuildTermEntry;

/*
 * Position hash table entry: maps (term, doc_id) to an
 * arena-stored position array for V6 phrase queries.
 */
typedef struct TpBuildPosKey
{
	const char *term;  /* Pointer to arena-stored term string */
	uint32		doc_id; /* Sequential document ID */
} TpBuildPosKey;

typedef struct TpBuildPosEntry
{
	TpBuildPosKey key;
	ArenaAddr	  positions_addr; /* Arena offset to uint16[] array */
	uint16		  count;		  /* Number of positions stored */
} TpBuildPosEntry;

/*
 * Initial and growth sizes for flat arrays.
 */
#define TP_BUILD_INITIAL_DOCS 4096

/*
 * Build context for CREATE INDEX.
 * Holds all in-memory state for one spill batch.
 */
typedef struct TpBuildContext
{
	/* Arena for EXPULL blocks, term strings, and position data */
	TpArena *arena;

	/* Local hash table: term string -> TpBuildTermEntry */
	HTAB *terms_ht;

	/* Position hash table: (term, doc_id) -> position array in arena */
	HTAB *positions_ht;

	/* Flat arrays indexed by doc_id (sequential assignment) */
	uint8			*fieldnorms; /* Encoded fieldnorm per doc */
	ItemPointerData *ctids;		 /* Heap CTID per doc */
	uint32			 num_docs;	 /* Documents in current batch */
	uint32			 docs_capacity;

	/* Corpus statistics for this batch */
	uint64 total_len; /* Sum of document lengths */

	/* Budget for flush decisions */
	Size budget; /* Max arena bytes before flush */
} TpBuildContext;

/*
 * Create a new build context.
 * budget: max arena bytes before caller should flush (0 = no limit).
 */
extern TpBuildContext *tp_build_context_create(Size budget);

/*
 * Add a single document's terms to the build context.
 * Assigns a sequential doc_id and stores fieldnorm + CTID.
 *
 * terms: array of null-terminated term strings
 * frequencies: parallel array of term frequencies
 * term_count: number of terms
 * doc_length: sum of all frequencies (for fieldnorm encoding)
 * ctid: heap tuple ID
 *
 * Returns the assigned doc_id.
 */
extern uint32 tp_build_context_add_document(
		TpBuildContext *ctx,
		char		  **terms,
		int32		   *frequencies,
		uint16		  **positions,
		int				term_count,
		int32			doc_length,
		ItemPointer		ctid);

/*
 * Check if the build context should be flushed (budget exceeded).
 */
static inline bool
tp_build_context_should_flush(TpBuildContext *ctx)
{
	if (ctx->budget == 0)
		return false;
	return tp_arena_mem_usage(ctx->arena) >= ctx->budget;
}

/*
 * Build a sorted term array from the build context.
 * Returns a palloc'd array of TermInfo-like structures.
 * Caller must pfree the returned array.
 *
 * The returned TermInfo entries have:
 *   - term, term_len: from the hash table entry
 *   - posting_list_dp: UNUSED (set to InvalidDsaPointer)
 *   - expull: pointer to the TpExpull in the hash table entry
 */
typedef struct TpBuildTermInfo
{
	char	 *term;
	uint32	  term_len;
	TpExpull *expull;	/* Points into HTAB entry */
	uint32	  doc_freq; /* Number of documents for this term */
} TpBuildTermInfo;

extern TpBuildTermInfo *
tp_build_context_get_sorted_terms(TpBuildContext *ctx, uint32 *num_terms);

/*
 * Write a segment from the build context to an index relation.
 * Returns the header block number of the written segment.
 */
extern BlockNumber
tp_write_segment_from_build_ctx(TpBuildContext *ctx, Relation index);

/*
 * Write a segment from the build context to a BufFile as a flat
 * byte stream (no page boundaries). Used by parallel build workers.
 * Returns the data_size (total bytes written for this segment).
 */
extern uint64 tp_write_segment_to_buffile(TpBuildContext *ctx, BufFile *file);

/*
 * Reset the build context for reuse (new spill batch).
 * Frees all arena memory and clears the hash table and arrays.
 */
extern void tp_build_context_reset(TpBuildContext *ctx);

/*
 * Destroy the build context, freeing all memory.
 */
extern void tp_build_context_destroy(TpBuildContext *ctx);
