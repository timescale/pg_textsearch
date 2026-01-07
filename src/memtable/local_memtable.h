/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/local_memtable.h - Per-worker local memtable for parallel builds
 *
 * Unlike the shared DSA-based memtable, a local memtable uses palloc'd memory
 * private to a single backend. This eliminates contention during parallel
 * index builds, at the cost of requiring serialization to write segments.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/hsearch.h>

/*
 * Local posting entry - stored in palloc'd arrays
 */
typedef struct TpLocalPostingEntry
{
	ItemPointerData ctid;	   /* Document heap tuple ID */
	int32			frequency; /* Term frequency in document */
} TpLocalPostingEntry;

/*
 * Local posting list for a single term
 */
typedef struct TpLocalPosting
{
	char				*term;		/* Term string (palloc'd) */
	int32				 term_len;	/* Length of term string */
	int32				 doc_count; /* Number of documents in posting list */
	int32				 capacity;	/* Allocated capacity of entries array */
	TpLocalPostingEntry *entries;	/* Array of posting entries (palloc'd) */
} TpLocalPosting;

/*
 * Document length entry for local hash table
 */
typedef struct TpLocalDocLength
{
	ItemPointerData ctid;	/* Key: Document CTID */
	int32			length; /* Value: Document length (term count) */
	char			status; /* Hash entry status */
} TpLocalDocLength;

/*
 * Local memtable - per-worker in-memory index
 *
 * All data is allocated in a dedicated MemoryContext, allowing fast
 * reset via MemoryContextReset() when the memtable is cleared.
 */
typedef struct TpLocalMemtable
{
	HTAB		 *term_hash;   /* term string -> TpLocalPosting* */
	HTAB		 *doc_lengths; /* CTID -> doc_length */
	MemoryContext mcxt;		   /* Memory context for all allocations */

	/* Statistics */
	int64 total_postings; /* Total posting entries (spill trigger) */
	int32 num_docs;		  /* Number of documents added */
	int32 num_terms;	  /* Number of unique terms */
	int64 total_len;	  /* Sum of document lengths */
} TpLocalMemtable;

/*
 * Iterator for walking posting lists during segment write
 */
typedef struct TpLocalMemtableIterator
{
	TpLocalMemtable *memtable;
	HASH_SEQ_STATUS	 hash_seq;
	bool			 started;
} TpLocalMemtableIterator;

/*
 * Function declarations
 */

/* Memtable lifecycle */
extern TpLocalMemtable *tp_local_memtable_create(void);
extern void				tp_local_memtable_clear(TpLocalMemtable *memtable);
extern void				tp_local_memtable_destroy(TpLocalMemtable *memtable);

/* Document insertion */
extern void tp_local_memtable_add_term(
		TpLocalMemtable *memtable,
		const char		*term,
		int				 term_len,
		ItemPointer		 ctid,
		int32			 frequency);

extern void tp_local_memtable_store_doc_length(
		TpLocalMemtable *memtable, ItemPointer ctid, int32 doc_length);

/* Posting list access for segment writing */
extern void tp_local_memtable_iterator_init(
		TpLocalMemtableIterator *iter, TpLocalMemtable *memtable);

extern TpLocalPosting *
tp_local_memtable_iterator_next(TpLocalMemtableIterator *iter);

/* Document length lookup */
extern int32
tp_local_memtable_get_doc_length(TpLocalMemtable *memtable, ItemPointer ctid);

/* Get all documents with their lengths for building docmap */
typedef void (*TpLocalDocLengthCallback)(
		ItemPointer ctid, int32 doc_length, void *arg);

extern void tp_local_memtable_foreach_doc(
		TpLocalMemtable			*memtable,
		TpLocalDocLengthCallback callback,
		void					*arg);

/* Build sorted term array for dictionary construction */
extern TpLocalPosting **tp_local_memtable_get_sorted_terms(
		TpLocalMemtable *memtable, int *num_terms_out);
