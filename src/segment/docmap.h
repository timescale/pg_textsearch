/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * docmap.h - Document ID mapping for V2 segment format
 *
 * In V2 format, posting lists use compact 4-byte segment-local doc IDs
 * instead of 6-byte CTIDs. This module provides:
 * - Collection of unique documents during segment build
 * - Assignment of sequential doc IDs (0 to N-1)
 * - CTID → doc_id lookup during posting conversion
 * - Arrays for CTID map and fieldnorm table
 */
#pragma once

#include "postgres.h"
#include "storage/itemptr.h"
#include "utils/hsearch.h"

/*
 * Entry in the CTID → doc_id hash table (build-time only).
 * Used to quickly look up a document's assigned ID when converting postings.
 */
typedef struct TpDocMapEntry
{
	ItemPointerData ctid;		/* Key: heap tuple location */
	uint32			doc_id;		/* Value: segment-local doc ID */
	uint32			doc_length; /* Document length (for fieldnorm) */
} TpDocMapEntry;

/*
 * Entry for sorted CTID lookup (binary search).
 */
typedef struct TpCtidLookupEntry
{
	ItemPointerData ctid;
	uint32			doc_id;
} TpCtidLookupEntry;

/*
 * Document map builder context.
 * Collects documents and assigns sequential IDs during segment write.
 */
typedef struct TpDocMapBuilder
{
	HTAB  *ctid_to_id; /* Hash table: CTID → doc_id */
	uint32 num_docs;   /* Number of documents assigned */
	uint32 capacity;   /* Current capacity of arrays */
	bool   finalized;  /* True after tp_docmap_finalize called */

	/* Output arrays (indexed by doc_id, valid after finalize) */
	ItemPointerData *ctid_map;	 /* doc_id → CTID */
	uint8			*fieldnorms; /* doc_id → encoded length (1 byte) */

	/* Sorted lookup array for fast binary search (valid after finalize) */
	TpCtidLookupEntry *ctid_sorted; /* Sorted by CTID for binary search */
} TpDocMapBuilder;

/*
 * Create a new document map builder (constructor).
 * Call this before collecting documents.
 */
extern TpDocMapBuilder *tp_docmap_create(void);

/*
 * Add a document to the map.
 * Returns the assigned doc_id (reuses existing ID if CTID already present).
 * doc_length is stored for fieldnorm encoding; if CTID already exists, the
 * original doc_length is kept (callers should ensure consistent lengths).
 */
extern uint32
tp_docmap_add(TpDocMapBuilder *builder, ItemPointer ctid, uint32 doc_length);

/*
 * Look up doc_id for a CTID using hash table.
 * Returns UINT32_MAX if not found.
 */
extern uint32 tp_docmap_lookup(TpDocMapBuilder *builder, ItemPointer ctid);

/*
 * Fast lookup using binary search on sorted array.
 * Requires tp_docmap_finalize() to have been called.
 * Returns UINT32_MAX if not found.
 */
extern uint32
tp_docmap_lookup_fast(TpDocMapBuilder *builder, ItemPointer ctid);

/*
 * Finalize the document map.
 * Builds the ctid_map and fieldnorms arrays sorted by doc_id.
 * After this call, the hash table is no longer needed.
 */
extern void tp_docmap_finalize(TpDocMapBuilder *builder);

/*
 * Free the document map builder and all associated memory.
 */
extern void tp_docmap_destroy(TpDocMapBuilder *builder);

/*
 * Get the CTID for a doc_id. Requires finalize to have been called.
 */
static inline ItemPointer
tp_docmap_get_ctid(TpDocMapBuilder *builder, uint32 doc_id)
{
	Assert(builder->finalized);
	if (doc_id >= builder->num_docs)
		return NULL;
	return &builder->ctid_map[doc_id];
}

/*
 * Get the fieldnorm for a doc_id. Requires finalize to have been called.
 */
static inline uint8
tp_docmap_get_fieldnorm(TpDocMapBuilder *builder, uint32 doc_id)
{
	Assert(builder->finalized);
	if (doc_id >= builder->num_docs)
		return 0;
	return builder->fieldnorms[doc_id];
}
