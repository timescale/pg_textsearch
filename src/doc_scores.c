/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * doc_scores.c - Custom hash table for document score accumulation
 *
 * Uses open-addressing with linear probing for performance.
 * Key: 64-bit packed CTID. Value: score and doc_length.
 *
 * Design choices:
 * - Linear probing has better cache locality than chaining
 * - Power-of-2 sizing enables fast modulo via bitmask
 * - 75% load factor balances space vs. probe length
 * - Key of 0 marks empty slots (CTID (0,0) is invalid)
 * - Simple FNV-1a variant for 64-bit key hashing
 */
#include "doc_scores.h"
#include "utils/memutils.h"

/*
 * FNV-1a hash for 64-bit key.
 * Fast and provides good distribution.
 */
static inline uint32
hash_key(uint64 key)
{
	uint32 hash = 2166136261u; /* FNV offset basis */

	hash ^= (uint32)(key & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 8) & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 16) & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 24) & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 32) & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 40) & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 48) & 0xFF);
	hash *= 16777619u;
	hash ^= (uint32)((key >> 56) & 0xFF);
	hash *= 16777619u;

	return hash;
}

/*
 * Round up to next power of 2.
 */
static uint32
next_power_of_2(uint32 n)
{
	if (n == 0)
		return 1;
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

/*
 * Create a new document score hash table.
 */
TpDocScoreTable *
tp_doc_score_table_create(int initial_capacity, MemoryContext mem_context)
{
	TpDocScoreTable *table;
	MemoryContext	 old_context;
	uint32			 capacity;

	/* Minimum capacity of 1024 for reasonable performance */
	capacity = next_power_of_2(Max((uint32)initial_capacity, 1024));

	old_context = MemoryContextSwitchTo(mem_context);

	table			   = palloc(sizeof(TpDocScoreTable));
	table->entries	   = palloc0(capacity * sizeof(TpDocScoreEntry));
	table->capacity	   = capacity;
	table->mask		   = capacity - 1;
	table->count	   = 0;
	table->max_load	   = (capacity * 3) / 4; /* 75% load factor */
	table->mem_context = mem_context;

	MemoryContextSwitchTo(old_context);

	return table;
}

/*
 * Resize the hash table to double capacity.
 */
static void
resize_table(TpDocScoreTable *table)
{
	TpDocScoreEntry *old_entries  = table->entries;
	uint32			 old_capacity = table->capacity;
	uint32			 new_capacity = old_capacity * 2;
	MemoryContext	 old_context;
	uint32			 i;

	old_context = MemoryContextSwitchTo(table->mem_context);

	table->entries	= palloc0(new_capacity * sizeof(TpDocScoreEntry));
	table->capacity = new_capacity;
	table->mask		= new_capacity - 1;
	table->max_load = (new_capacity * 3) / 4;
	table->count	= 0;

	/* Rehash all entries */
	for (i = 0; i < old_capacity; i++)
	{
		if (old_entries[i].key != 0)
		{
			uint32			 idx  = hash_key(old_entries[i].key) & table->mask;
			TpDocScoreEntry *slot = &table->entries[idx];

			/* Linear probe to find empty slot */
			while (slot->key != 0)
			{
				idx	 = (idx + 1) & table->mask;
				slot = &table->entries[idx];
			}

			*slot = old_entries[i];
			table->count++;
		}
	}

	pfree(old_entries);
	MemoryContextSwitchTo(old_context);
}

/*
 * Insert or update a document score.
 */
TpDocScoreEntry *
tp_doc_score_table_insert(
		TpDocScoreTable *table, const ItemPointerData *ctid, bool *found)
{
	uint64			 key = ctid_to_key(ctid);
	uint32			 idx;
	TpDocScoreEntry *slot;

	Assert(key != 0); /* CTID (0,0) is invalid */

	/* Resize if needed */
	if (table->count >= table->max_load)
		resize_table(table);

	idx	 = hash_key(key) & table->mask;
	slot = &table->entries[idx];

	/* Linear probe */
	while (slot->key != 0)
	{
		if (slot->key == key)
		{
			/* Found existing entry */
			*found = true;
			return slot;
		}
		idx	 = (idx + 1) & table->mask;
		slot = &table->entries[idx];
	}

	/* Empty slot - insert new entry */
	slot->key = key;
	table->count++;
	*found = false;
	return slot;
}

/*
 * Get next entry from iterator.
 */
TpDocScoreEntry *
tp_doc_score_iter_next(TpDocScoreIterator *iter)
{
	while (iter->position < iter->table->capacity)
	{
		TpDocScoreEntry *entry = &iter->table->entries[iter->position++];
		if (entry->key != 0)
			return entry;
	}
	return NULL;
}

/*
 * Destroy table and free memory.
 */
void
tp_doc_score_table_destroy(TpDocScoreTable *table)
{
	if (table)
	{
		MemoryContext old_context = MemoryContextSwitchTo(table->mem_context);
		pfree(table->entries);
		pfree(table);
		MemoryContextSwitchTo(old_context);
	}
}
