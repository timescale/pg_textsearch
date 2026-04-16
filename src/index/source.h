/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * source.h - Abstract data source interface for posting lists
 *
 * Defines a columnar interface that both memtable and segment implement.
 * This allows scoring code to be agnostic to the underlying storage.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>

/*
 * Columnar posting data for a term.
 * Arrays are parallel - ctids[i] corresponds to frequencies[i].
 */
typedef struct TpPostingData
{
	ItemPointerData *ctids;		  /* Array of document CTIDs */
	int32			*frequencies; /* Array of term frequencies */
	int32			 count;		  /* Number of entries */
	int32			 doc_freq;	  /* Document frequency (for IDF) */
} TpPostingData;

/*
 * Abstract data source interface.
 * Both memtable and segment implement this interface.
 */
typedef struct TpDataSource TpDataSource;

typedef struct TpDataSourceOps
{
	/*
	 * Get posting data for a term.
	 * Returns NULL if term not found.
	 * Caller must free with free_postings().
	 */
	TpPostingData *(*get_postings)(TpDataSource *source, const char *term);

	/*
	 * Free posting data returned by get_postings().
	 */
	void (*free_postings)(TpDataSource *source, TpPostingData *data);

	/*
	 * Get document length for a CTID.
	 * Returns -1 if not found.
	 */
	int32 (*get_doc_length)(TpDataSource *source, ItemPointer ctid);

	/*
	 * Close and free the data source.
	 */
	void (*close)(TpDataSource *source);
} TpDataSourceOps;

struct TpDataSource
{
	const TpDataSourceOps *ops;
	int32				   total_docs; /* Corpus document count */
	int64				   total_len;  /* Corpus total length */
};

/*
 * Convenience macros for calling interface methods
 */
#define tp_source_get_postings(src, term) \
	((src)->ops->get_postings((src), (term)))
#define tp_source_free_postings(src, data) \
	((src)->ops->free_postings((src), (data)))
#define tp_source_get_doc_length(src, ctid) \
	((src)->ops->get_doc_length((src), (ctid)))
#define tp_source_close(src) ((src)->ops->close((src)))

/*
 * Helper to allocate posting data with given capacity.
 * Allocates in CurrentMemoryContext.
 */
extern TpPostingData *tp_alloc_posting_data(int32 capacity);

/*
 * Helper to free posting data allocated by tp_alloc_posting_data().
 */
extern void tp_free_posting_data(TpPostingData *data);
