/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * doc_scores.h - Custom hash table for document score accumulation
 *
 * Uses open-addressing with linear probing for performance.
 * Optimized for insert-heavy workloads during BM25 scoring.
 */
#pragma once

#include "postgres.h"
#include "storage/itemptr.h"

/*
 * Pack CTID into 64-bit key for fast hashing.
 * Format: block_number (32 bits) | offset_number (16 bits) | padding (16 bits)
 */
static inline uint64
ctid_to_key(const ItemPointerData *ctid)
{
	return ((uint64)ItemPointerGetBlockNumber(ctid) << 32) |
		   ((uint64)ItemPointerGetOffsetNumber(ctid) << 16);
}

/*
 * Unpack key back to CTID.
 */
static inline void
key_to_ctid(uint64 key, ItemPointerData *ctid)
{
	ItemPointerSetBlockNumber(ctid, (BlockNumber)(key >> 32));
	ItemPointerSetOffsetNumber(ctid, (OffsetNumber)((key >> 16) & 0xFFFF));
}

/*
 * Hash table entry - 24 bytes, cache-aligned.
 * Key of 0 indicates empty slot (CTID (0,0) is invalid).
 */
typedef struct TpDocScoreEntry
{
	uint64 key;		   /* Packed CTID */
	float4 score;	   /* Accumulated BM25 score */
	float4 doc_length; /* Document length */
} TpDocScoreEntry;

/*
 * Hash table structure.
 * Uses power-of-2 sizing for fast modulo via bitmask.
 */
typedef struct TpDocScoreTable
{
	TpDocScoreEntry *entries;	  /* Entry array */
	uint32			 capacity;	  /* Always power of 2 */
	uint32			 mask;		  /* capacity - 1, for fast modulo */
	uint32			 count;		  /* Number of entries */
	uint32			 max_load;	  /* Resize threshold (75% load) */
	MemoryContext	 mem_context; /* Memory context for allocations */
} TpDocScoreTable;

/*
 * Create a new document score hash table.
 * initial_capacity will be rounded up to next power of 2.
 */
TpDocScoreTable *
tp_doc_score_table_create(int initial_capacity, MemoryContext mem_context);

/*
 * Insert or update a document score.
 * Returns pointer to entry (new or existing).
 * Sets *found to true if entry already existed.
 */
TpDocScoreEntry *tp_doc_score_table_insert(
		TpDocScoreTable *table, const ItemPointerData *ctid, bool *found);

/*
 * Get number of entries in table.
 */
static inline uint32
tp_doc_score_table_count(TpDocScoreTable *table)
{
	return table->count;
}

/*
 * Iterator for extracting all entries.
 */
typedef struct TpDocScoreIterator
{
	TpDocScoreTable *table;
	uint32			 position;
} TpDocScoreIterator;

/*
 * Initialize iterator.
 */
static inline void
tp_doc_score_iter_init(TpDocScoreIterator *iter, TpDocScoreTable *table)
{
	iter->table	   = table;
	iter->position = 0;
}

/*
 * Get next entry, returns NULL when done.
 */
TpDocScoreEntry *tp_doc_score_iter_next(TpDocScoreIterator *iter);

/*
 * Destroy table and free memory.
 */
void tp_doc_score_table_destroy(TpDocScoreTable *table);
