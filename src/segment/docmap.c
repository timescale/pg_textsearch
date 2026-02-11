/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
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

	builder->num_docs	  = 0;
	builder->capacity	  = 0;
	builder->finalized	  = false;
	builder->ctid_pages	  = NULL;
	builder->ctid_offsets = NULL;
	builder->fieldnorms	  = NULL;

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
 * Comparison function for sorting by CTID.
 * This ensures doc_ids are assigned in CTID order, which means
 * postings sorted by CTID are also sorted by doc_id - critical
 * for sequential access to CTID arrays during query iteration.
 */
static int
docmap_entry_cmp_by_ctid(const void *a, const void *b)
{
	const TpDocMapEntry *ea = (const TpDocMapEntry *)a;
	const TpDocMapEntry *eb = (const TpDocMapEntry *)b;

	/* Cast away const - ItemPointerCompare doesn't modify arguments */
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

	/*
	 * Collect all entries from hash table.
	 * Use HUGE allocation since this can exceed MaxAllocSize for
	 * very large segments (e.g., 138M docs * 16 bytes = 2.2GB).
	 */
	entries = palloc_extended(
			sizeof(TpDocMapEntry) * builder->num_docs, MCXT_ALLOC_HUGE);
	i = 0;

	hash_seq_init(&scan, builder->ctid_to_id);
	while ((entry = (TpDocMapEntry *)hash_seq_search(&scan)) != NULL)
	{
		entries[i++] = *entry;
	}

	Assert(i == builder->num_docs);

	/*
	 * Sort by CTID to assign doc_ids in CTID order.
	 * This maintains the invariant: CTID order = doc_id order.
	 * Postings sorted by CTID will then be sorted by doc_id,
	 * enabling sequential access to CTID arrays during iteration.
	 */
	qsort(entries,
		  builder->num_docs,
		  sizeof(TpDocMapEntry),
		  docmap_entry_cmp_by_ctid);

	/* Allocate output arrays (split CTID storage for better cache locality) */
	builder->capacity	  = builder->num_docs;
	builder->ctid_pages	  = palloc(sizeof(BlockNumber) * builder->num_docs);
	builder->ctid_offsets = palloc(sizeof(OffsetNumber) * builder->num_docs);
	builder->fieldnorms	  = palloc(sizeof(uint8) * builder->num_docs);

	/*
	 * Fill arrays and reassign doc_ids in CTID order.
	 * Update hash table entries so lookups return the correct new doc_id.
	 */
	for (i = 0; i < builder->num_docs; i++)
	{
		TpDocMapEntry *hash_entry;

		/* Array position i = doc_id i (CTID-sorted order) */
		builder->ctid_pages[i]	 = ItemPointerGetBlockNumber(&entries[i].ctid);
		builder->ctid_offsets[i] = ItemPointerGetOffsetNumber(
				&entries[i].ctid);
		builder->fieldnorms[i] = encode_fieldnorm(entries[i].doc_length);

		/* Update hash table entry with new doc_id */
		hash_entry = (TpDocMapEntry *)hash_search(
				builder->ctid_to_id, &entries[i].ctid, HASH_FIND, NULL);
		Assert(hash_entry != NULL);
		hash_entry->doc_id = i;
	}

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

	if (builder->ctid_pages)
		pfree(builder->ctid_pages);

	if (builder->ctid_offsets)
		pfree(builder->ctid_offsets);

	if (builder->fieldnorms)
		pfree(builder->fieldnorms);

	pfree(builder);
}
