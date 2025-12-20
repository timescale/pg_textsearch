/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * docmap.c - Document ID mapping implementation
 */
#include "docmap.h"
#include "fieldnorm.h"
#include "postgres.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/* Initial capacity for document arrays */
#define DOCMAP_INITIAL_CAPACITY 1024

/*
 * Hash function for ItemPointerData keys
 */
static uint32
ctid_hash(const void *key, Size keysize)
{
	const ItemPointerData *ctid	  = (const ItemPointerData *)key;
	uint32				   block  = ItemPointerGetBlockNumber(ctid);
	uint16				   offset = ItemPointerGetOffsetNumber(ctid);

	(void)keysize; /* unused */

	/* Combine block and offset into a single hash */
	return block ^ ((uint32)offset << 16) ^ offset;
}

/*
 * Comparison function for ItemPointerData keys
 */
static int
ctid_match(const void *key1, const void *key2, Size keysize)
{
	(void)keysize; /* unused */
	return ItemPointerCompare((ItemPointer)key1, (ItemPointer)key2);
}

TpDocMapBuilder *
tp_docmap_create(void)
{
	TpDocMapBuilder *builder;
	HASHCTL			 hash_ctl;

	builder = palloc0(sizeof(TpDocMapBuilder));

	/* Create hash table for CTID → doc_id lookup */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(TpDocMapEntry);
	hash_ctl.hash	   = ctid_hash;
	hash_ctl.match	   = ctid_match;
	hash_ctl.hcxt	   = CurrentMemoryContext;

	builder->ctid_to_id = hash_create(
			"DocMap CTID->ID",
			DOCMAP_INITIAL_CAPACITY,
			&hash_ctl,
			HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	builder->num_docs	 = 0;
	builder->capacity	 = 0;
	builder->finalized	 = false;
	builder->ctid_map	 = NULL;
	builder->fieldnorms	 = NULL;
	builder->ctid_sorted = NULL;

	return builder;
}

uint32
tp_docmap_add(TpDocMapBuilder *builder, ItemPointer ctid, uint32 doc_length)
{
	TpDocMapEntry *entry;
	bool		   found;

	Assert(!builder->finalized);

	/* Guard: UINT32_MAX is reserved as "not found" sentinel */
	if (builder->num_docs >= UINT32_MAX - 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many documents in segment (max %u)",
						UINT32_MAX - 1)));

	/* Look up or create entry in hash table */
	entry = (TpDocMapEntry *)
			hash_search(builder->ctid_to_id, ctid, HASH_ENTER, &found);

	if (found)
	{
		/* Document already exists, return existing ID */
		return entry->doc_id;
	}

	/* New document - assign next sequential ID */
	entry->doc_id	  = builder->num_docs;
	entry->doc_length = doc_length;
	builder->num_docs++;

	return entry->doc_id;
}

uint32
tp_docmap_lookup(TpDocMapBuilder *builder, ItemPointer ctid)
{
	TpDocMapEntry *entry;

	entry = (TpDocMapEntry *)
			hash_search(builder->ctid_to_id, ctid, HASH_FIND, NULL);

	if (entry == NULL)
		return UINT32_MAX;

	return entry->doc_id;
}

/*
 * Comparison function for sorting by doc_id
 */
static int
docmap_entry_cmp(const void *a, const void *b)
{
	const TpDocMapEntry *ea = (const TpDocMapEntry *)a;
	const TpDocMapEntry *eb = (const TpDocMapEntry *)b;

	if (ea->doc_id < eb->doc_id)
		return -1;
	if (ea->doc_id > eb->doc_id)
		return 1;
	return 0;
}

/*
 * Comparison function for sorting/searching by CTID
 */
static int
ctid_lookup_cmp(const void *a, const void *b)
{
	const TpCtidLookupEntry *ea = (const TpCtidLookupEntry *)a;
	const TpCtidLookupEntry *eb = (const TpCtidLookupEntry *)b;

	/* Cast away const for ItemPointerCompare which takes non-const args */
	return ItemPointerCompare((ItemPointer)&ea->ctid, (ItemPointer)&eb->ctid);
}

void
tp_docmap_finalize(TpDocMapBuilder *builder)
{
	HASH_SEQ_STATUS scan;
	TpDocMapEntry  *entry;
	TpDocMapEntry  *entries;
	uint32			i;

	Assert(!builder->finalized);

	if (builder->num_docs == 0)
	{
		builder->finalized = true;
		return;
	}

	/* Collect all entries from hash table */
	entries = palloc(sizeof(TpDocMapEntry) * builder->num_docs);
	i		= 0;

	hash_seq_init(&scan, builder->ctid_to_id);
	while ((entry = (TpDocMapEntry *)hash_seq_search(&scan)) != NULL)
	{
		entries[i++] = *entry;
	}

	Assert(i == builder->num_docs);

	/* Sort by doc_id to ensure correct array indexing */
	qsort(entries, builder->num_docs, sizeof(TpDocMapEntry), docmap_entry_cmp);

	/* Allocate output arrays */
	builder->capacity	= builder->num_docs;
	builder->ctid_map	= palloc(sizeof(ItemPointerData) * builder->num_docs);
	builder->fieldnorms = palloc(sizeof(uint8) * builder->num_docs);

	/* Fill arrays from sorted entries */
	for (i = 0; i < builder->num_docs; i++)
	{
		Assert(entries[i].doc_id == i);
		builder->ctid_map[i]   = entries[i].ctid;
		builder->fieldnorms[i] = encode_fieldnorm(entries[i].doc_length);
	}

	/* Build sorted CTID lookup array for binary search */
	builder->ctid_sorted = palloc(
			sizeof(TpCtidLookupEntry) * builder->num_docs);
	for (i = 0; i < builder->num_docs; i++)
	{
		builder->ctid_sorted[i].ctid   = builder->ctid_map[i];
		builder->ctid_sorted[i].doc_id = i;
	}
	qsort(builder->ctid_sorted,
		  builder->num_docs,
		  sizeof(TpCtidLookupEntry),
		  ctid_lookup_cmp);

	pfree(entries);
	builder->finalized = true;
}

void
tp_docmap_destroy(TpDocMapBuilder *builder)
{
	if (builder == NULL)
		return;

	if (builder->ctid_to_id)
		hash_destroy(builder->ctid_to_id);

	if (builder->ctid_map)
		pfree(builder->ctid_map);

	if (builder->fieldnorms)
		pfree(builder->fieldnorms);

	if (builder->ctid_sorted)
		pfree(builder->ctid_sorted);

	pfree(builder);
}

/*
 * Fast CTID → doc_id lookup using binary search.
 * Requires tp_docmap_finalize() to have been called.
 * Returns UINT32_MAX if not found.
 */
uint32
tp_docmap_lookup_fast(TpDocMapBuilder *builder, ItemPointer ctid)
{
	TpCtidLookupEntry  key;
	TpCtidLookupEntry *result;

	Assert(builder->finalized);

	if (builder->num_docs == 0)
		return UINT32_MAX;

	key.ctid = *ctid;
	result =
			bsearch(&key,
					builder->ctid_sorted,
					builder->num_docs,
					sizeof(TpCtidLookupEntry),
					ctid_lookup_cmp);

	if (result == NULL)
		return UINT32_MAX;

	return result->doc_id;
}
