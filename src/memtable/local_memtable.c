/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/local_memtable.c - Per-worker local memtable for parallel builds
 */
#include <postgres.h>

#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "constants.h"
#include "local_memtable.h"

/*
 * Hash entry for term lookup
 */
typedef struct TpLocalTermHashEntry
{
	char			key[NAMEDATALEN]; /* Term string (truncated for key) */
	TpLocalPosting *posting;		  /* Pointer to full posting data */
} TpLocalTermHashEntry;

/*
 * Comparison function for sorting terms alphabetically
 */
static int
local_posting_cmp(const void *a, const void *b)
{
	const TpLocalPosting *pa = *(const TpLocalPosting **)a;
	const TpLocalPosting *pb = *(const TpLocalPosting **)b;
	return strcmp(pa->term, pb->term);
}

/*
 * Comparison function for sorting posting entries by CTID
 */
static int
posting_entry_cmp(const void *a, const void *b)
{
	const TpLocalPostingEntry *ea = (const TpLocalPostingEntry *)a;
	const TpLocalPostingEntry *eb = (const TpLocalPostingEntry *)b;

	/* Compare block numbers first */
	BlockNumber blk_a = ItemPointerGetBlockNumber(&ea->ctid);
	BlockNumber blk_b = ItemPointerGetBlockNumber(&eb->ctid);
	if (blk_a != blk_b)
		return (blk_a < blk_b) ? -1 : 1;

	/* Then offset numbers */
	OffsetNumber off_a = ItemPointerGetOffsetNumber(&ea->ctid);
	OffsetNumber off_b = ItemPointerGetOffsetNumber(&eb->ctid);
	if (off_a != off_b)
		return (off_a < off_b) ? -1 : 1;

	return 0;
}

/*
 * Create a new local memtable
 */
TpLocalMemtable *
tp_local_memtable_create(void)
{
	TpLocalMemtable *memtable;
	MemoryContext	 mcxt;
	MemoryContext	 oldcxt;
	HASHCTL			 hash_ctl;

	/* Create a dedicated memory context for this memtable */
	mcxt = AllocSetContextCreate(
			CurrentMemoryContext, "LocalMemtable", ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(mcxt);

	memtable	   = palloc0(sizeof(TpLocalMemtable));
	memtable->mcxt = mcxt;

	/* Create term hash table */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(TpLocalTermHashEntry);
	hash_ctl.hcxt	   = mcxt;

	memtable->term_hash = hash_create(
			"LocalMemtable Terms",
			1024, /* initial size */
			&hash_ctl,
			HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	/* Create document lengths hash table */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(TpLocalDocLength);
	hash_ctl.hcxt	   = mcxt;

	memtable->doc_lengths = hash_create(
			"LocalMemtable DocLengths",
			1024,
			&hash_ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	MemoryContextSwitchTo(oldcxt);

	return memtable;
}

/*
 * Clear a local memtable for reuse (after spilling to segment)
 */
void
tp_local_memtable_clear(TpLocalMemtable *memtable)
{
	HASHCTL		  hash_ctl;
	MemoryContext oldcxt;

	if (!memtable)
		return;

	/* Reset the memory context - this frees all allocations */
	MemoryContextReset(memtable->mcxt);

	oldcxt = MemoryContextSwitchTo(memtable->mcxt);

	/* Recreate hash tables */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(TpLocalTermHashEntry);
	hash_ctl.hcxt	   = memtable->mcxt;

	memtable->term_hash = hash_create(
			"LocalMemtable Terms",
			1024,
			&hash_ctl,
			HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(TpLocalDocLength);
	hash_ctl.hcxt	   = memtable->mcxt;

	memtable->doc_lengths = hash_create(
			"LocalMemtable DocLengths",
			1024,
			&hash_ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Reset statistics */
	memtable->total_postings = 0;
	memtable->num_docs		 = 0;
	memtable->num_terms		 = 0;
	memtable->total_len		 = 0;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Destroy a local memtable and free all memory
 */
void
tp_local_memtable_destroy(TpLocalMemtable *memtable)
{
	if (!memtable)
		return;

	/* Deleting the memory context frees everything */
	MemoryContextDelete(memtable->mcxt);
}

/*
 * Get or create a posting list for a term
 */
static TpLocalPosting *
get_or_create_posting(
		TpLocalMemtable *memtable, const char *term, int term_len)
{
	TpLocalTermHashEntry *entry;
	bool				  found;
	MemoryContext		  oldcxt;
	char				  key[NAMEDATALEN];
	int					  key_len;

	/* Truncate term for hash key if needed, but preserve original term_len */
	key_len = (term_len >= NAMEDATALEN) ? NAMEDATALEN - 1 : term_len;
	memcpy(key, term, key_len);
	key[key_len] = '\0';

	entry = hash_search(memtable->term_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* Create new posting list */
		oldcxt = MemoryContextSwitchTo(memtable->mcxt);

		entry->posting			  = palloc(sizeof(TpLocalPosting));
		entry->posting->term	  = pstrdup(term);
		entry->posting->term_len  = term_len;
		entry->posting->doc_count = 0;
		entry->posting->capacity  = TP_INITIAL_POSTING_LIST_CAPACITY;
		entry->posting->entries	  = palloc(
				  sizeof(TpLocalPostingEntry) * entry->posting->capacity);

		memtable->num_terms++;

		MemoryContextSwitchTo(oldcxt);
	}

	return entry->posting;
}

/*
 * Add a term occurrence to the memtable
 */
void
tp_local_memtable_add_term(
		TpLocalMemtable *memtable,
		const char		*term,
		int				 term_len,
		ItemPointer		 ctid,
		int32			 frequency)
{
	TpLocalPosting		*posting;
	TpLocalPostingEntry *entry;
	MemoryContext		 oldcxt;

	Assert(memtable != NULL);
	Assert(term != NULL);
	Assert(ctid != NULL);

	posting = get_or_create_posting(memtable, term, term_len);

	/* Grow array if needed */
	if (posting->doc_count >= posting->capacity)
	{
		int new_capacity = posting->capacity * TP_POSTING_LIST_GROWTH_FACTOR;

		oldcxt			 = MemoryContextSwitchTo(memtable->mcxt);
		posting->entries = repalloc(
				posting->entries, sizeof(TpLocalPostingEntry) * new_capacity);
		MemoryContextSwitchTo(oldcxt);

		posting->capacity = new_capacity;
	}

	/* Add entry */
	entry			 = &posting->entries[posting->doc_count];
	entry->ctid		 = *ctid;
	entry->frequency = frequency;

	posting->doc_count++;
	memtable->total_postings++;
}

/*
 * Store document length
 */
void
tp_local_memtable_store_doc_length(
		TpLocalMemtable *memtable, ItemPointer ctid, int32 doc_length)
{
	TpLocalDocLength *entry;
	bool			  found;

	Assert(memtable != NULL);
	Assert(ctid != NULL);

	entry = hash_search(memtable->doc_lengths, ctid, HASH_ENTER, &found);

	if (!found)
	{
		entry->ctid = *ctid;
		memtable->num_docs++;
		memtable->total_len += doc_length;
	}
	else
	{
		/* Adjust total length by the difference between new and old values */
		memtable->total_len += doc_length - entry->length;
	}

	entry->length = doc_length;
}

/*
 * Get document length
 */
int32
tp_local_memtable_get_doc_length(TpLocalMemtable *memtable, ItemPointer ctid)
{
	TpLocalDocLength *entry;
	bool			  found;

	Assert(memtable != NULL);
	Assert(ctid != NULL);

	entry = hash_search(memtable->doc_lengths, ctid, HASH_FIND, &found);

	if (!found)
		return 0;

	return entry->length;
}

/*
 * Initialize iterator for walking all posting lists
 */
void
tp_local_memtable_iterator_init(
		TpLocalMemtableIterator *iter, TpLocalMemtable *memtable)
{
	Assert(iter != NULL);
	Assert(memtable != NULL);

	iter->memtable = memtable;
	iter->started  = false;
}

/*
 * Get next posting list from iterator
 */
TpLocalPosting *
tp_local_memtable_iterator_next(TpLocalMemtableIterator *iter)
{
	TpLocalTermHashEntry *entry;

	Assert(iter != NULL);

	if (!iter->started)
	{
		hash_seq_init(&iter->hash_seq, iter->memtable->term_hash);
		iter->started = true;
	}

	entry = hash_seq_search(&iter->hash_seq);
	if (entry == NULL)
		return NULL;

	return entry->posting;
}

/*
 * Iterate over all documents with their lengths
 */
void
tp_local_memtable_foreach_doc(
		TpLocalMemtable			*memtable,
		TpLocalDocLengthCallback callback,
		void					*arg)
{
	HASH_SEQ_STATUS	  status;
	TpLocalDocLength *entry;

	Assert(memtable != NULL);
	Assert(callback != NULL);

	hash_seq_init(&status, memtable->doc_lengths);

	while ((entry = hash_seq_search(&status)) != NULL)
	{
		callback(&entry->ctid, entry->length, arg);
	}
}

/*
 * Get all posting lists sorted alphabetically by term
 *
 * Also sorts entries within each posting list by CTID.
 * Returns palloc'd array that caller must pfree.
 */
TpLocalPosting **
tp_local_memtable_get_sorted_terms(
		TpLocalMemtable *memtable, int *num_terms_out)
{
	TpLocalPosting		  **sorted;
	TpLocalMemtableIterator iter;
	TpLocalPosting		   *posting;
	int						count = 0;
	int						i;

	Assert(memtable != NULL);
	Assert(num_terms_out != NULL);

	/* Allocate array */
	sorted = palloc(sizeof(TpLocalPosting *) * memtable->num_terms);

	/* Collect all postings */
	tp_local_memtable_iterator_init(&iter, memtable);
	while ((posting = tp_local_memtable_iterator_next(&iter)) != NULL)
	{
		sorted[count++] = posting;
	}

	/* Sort alphabetically by term */
	qsort(sorted, count, sizeof(TpLocalPosting *), local_posting_cmp);

	/* Sort entries within each posting list by CTID */
	for (i = 0; i < count; i++)
	{
		qsort(sorted[i]->entries,
			  sorted[i]->doc_count,
			  sizeof(TpLocalPostingEntry),
			  posting_entry_cmp);
	}

	*num_terms_out = count;
	return sorted;
}
