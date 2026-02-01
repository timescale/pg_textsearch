/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * local_memtable.c - Per-worker local memtable for parallel builds
 */
#include <postgres.h>

#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "constants.h"
#include "local_memtable.h"

/*
 * Hash entry for term lookup.
 * Stores the full term alongside the posting list.
 */
typedef struct TpLocalTermHashEntry
{
	char		   key[NAMEDATALEN]; /* Term string (truncated for hash key) */
	char		  *term;			 /* Full term string (palloc'd) */
	int32		   term_len;		 /* Length of full term */
	TpLocalPosting posting;			 /* Posting list (embedded, not pointer) */
} TpLocalTermHashEntry;

/*
 * Comparison function for sorting terms alphabetically
 */
static int
local_term_posting_cmp(const void *a, const void *b)
{
	const TpLocalTermPosting *pa = (const TpLocalTermPosting *)a;
	const TpLocalTermPosting *pb = (const TpLocalTermPosting *)b;
	return strcmp(pa->term, pb->term);
}

/*
 * Comparison function for sorting posting entries by CTID
 */
static int
posting_entry_cmp(const void *a, const void *b)
{
	const TpPostingEntry *ea = (const TpPostingEntry *)a;
	const TpPostingEntry *eb = (const TpPostingEntry *)b;
	BlockNumber			  blk_a, blk_b;
	OffsetNumber		  off_a, off_b;

	/* Compare block numbers first */
	blk_a = ItemPointerGetBlockNumber(&ea->ctid);
	blk_b = ItemPointerGetBlockNumber(&eb->ctid);
	if (blk_a != blk_b)
		return (blk_a < blk_b) ? -1 : 1;

	/* Then offset numbers */
	off_a = ItemPointerGetOffsetNumber(&ea->ctid);
	off_b = ItemPointerGetOffsetNumber(&eb->ctid);
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

	/*
	 * Allocate the memtable struct in the CURRENT context (parent), not in
	 * mcxt. This allows tp_local_memtable_clear() to reset mcxt without
	 * freeing the memtable struct itself.
	 */
	memtable = palloc0(sizeof(TpLocalMemtable));

	/* Create a dedicated memory context for internal data */
	mcxt = AllocSetContextCreate(
			CurrentMemoryContext, "LocalMemtable", ALLOCSET_DEFAULT_SIZES);

	memtable->mcxt = mcxt;
	oldcxt		   = MemoryContextSwitchTo(mcxt);

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

	/* Delete the memory context (frees all internal data) */
	MemoryContextDelete(memtable->mcxt);

	/* Free the memtable struct itself (allocated in parent context) */
	pfree(memtable);
}

/*
 * Get or create a posting list for a term.
 * Returns pointer to the embedded posting in the hash entry.
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

	/* Truncate term for hash key if needed */
	key_len = (term_len >= NAMEDATALEN) ? NAMEDATALEN - 1 : term_len;
	memcpy(key, term, key_len);
	key[key_len] = '\0';

	entry = hash_search(memtable->term_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		oldcxt = MemoryContextSwitchTo(memtable->mcxt);

		/* Store full term in hash entry */
		entry->term = palloc(term_len + 1);
		memcpy(entry->term, term, term_len);
		entry->term[term_len] = '\0';
		entry->term_len		  = term_len;

		/* Initialize embedded posting list */
		entry->posting.doc_count = 0;
		entry->posting.capacity	 = TP_INITIAL_POSTING_LIST_CAPACITY;
		entry->posting.entries	 = palloc(
				  sizeof(TpPostingEntry) * entry->posting.capacity);

		memtable->num_terms++;

		MemoryContextSwitchTo(oldcxt);
	}

	return &entry->posting;
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
	TpLocalPosting *posting;
	TpPostingEntry *entry;
	MemoryContext	oldcxt;

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
				posting->entries, sizeof(TpPostingEntry) * new_capacity);
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

	return &entry->posting;
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
 * Get all terms with posting lists, sorted alphabetically by term.
 *
 * Also sorts entries within each posting list by CTID.
 * Returns palloc'd array that caller must pfree.
 */
TpLocalTermPosting *
tp_local_memtable_get_sorted_terms(
		TpLocalMemtable *memtable, int *num_terms_out)
{
	TpLocalTermPosting	 *sorted;
	TpLocalTermHashEntry *entry;
	HASH_SEQ_STATUS		  status;
	int					  count = 0;
	int					  i;

	Assert(memtable != NULL);
	Assert(num_terms_out != NULL);

	/* Allocate array */
	sorted = palloc(sizeof(TpLocalTermPosting) * memtable->num_terms);

	/* Collect all term/posting pairs from hash table */
	hash_seq_init(&status, memtable->term_hash);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		sorted[count].term	   = entry->term;
		sorted[count].term_len = entry->term_len;
		sorted[count].posting  = &entry->posting;
		count++;
	}

	/* Sort alphabetically by term */
	qsort(sorted, count, sizeof(TpLocalTermPosting), local_term_posting_cmp);

	/* Sort entries within each posting list by CTID */
	for (i = 0; i < count; i++)
	{
		qsort(sorted[i].posting->entries,
			  sorted[i].posting->doc_count,
			  sizeof(TpPostingEntry),
			  posting_entry_cmp);
	}

	*num_terms_out = count;
	return sorted;
}
