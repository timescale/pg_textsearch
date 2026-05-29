/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * chain_source.c - TpDataSource over the on-disk memtable page
 *                  chain (issue #374).
 *
 * See chain_source.h for the concurrency contract and high-level
 * semantics.
 *
 * A scaffold SQL function is exposed at the bottom of this file
 * (bm25_test_chain_source) for unit-level coverage, complementing
 * the end-to-end coverage in the regression suite.
 */
#include <postgres.h>

#include <access/hash.h>
#include <access/relation.h>
#include <catalog/index.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/block.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <utils/builtins.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/relcache.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "index/source.h"
#include "index/state.h"
#include "memtable/chain_source.h"
#include "memtable/chain_walker.h"
#include "memtable/page.h"
#include "types/vector.h"

/*
 * One row in the per-term accumulator hash table.  The key is a
 * NUL-terminated lexeme C string copied into the source memory
 * context; the value carries parallel (ctid, freq) arrays plus
 * the rolling doc_freq.  Arrays grow geometrically.
 */
typedef struct ChainTermEntry
{
	const char		*term;		  /* hash key; pointer into source mcxt */
	ItemPointerData *ctids;		  /* count slots; in source mcxt */
	int32			*frequencies; /* count slots; in source mcxt */
	int32			 count;		  /* records in this term */
	int32			 capacity;	  /* allocated array capacity */
	int32			 doc_freq;	  /* documents containing the term */
} ChainTermEntry;

/*
 * One row in the per-ctid doc-length hash table.  Keyed by raw
 * ItemPointerData (HASH_BLOBS).  Records (ctid → doc_length);
 * duplicate ctids are an invariant violation (each heap tuple
 * writes exactly one chain record) and we surface them as
 * DATA_CORRUPTED at open time.
 */
typedef struct ChainDocLenEntry
{
	ItemPointerData ctid;
	int32			doc_length;
} ChainDocLenEntry;

typedef struct TpMemtableChainSource
{
	TpDataSource  base;			   /* must be first */
	MemoryContext mcxt;			   /* private context, deleted in close() */
	HTAB		 *term_ht;		   /* char* → ChainTermEntry */
	HTAB		 *doclen_ht;	   /* ItemPointerData → ChainDocLenEntry */
	uint32		  chain_pages;	   /* total chain pages walked
									* (regular + continuation); used to
									* seed shared->chain_page_count when
									* a backend reopens an index whose
									* counter atomic was reset to 0 by
									* shmem init. */
	bool filter_terms;			   /* true when constructor was passed
									* a non-empty query_terms[]: the
									* term HTAB is pre-populated with
									* those terms (and only those) at
									* construction time, and ingest_terms
									* uses HASH_FIND (not HASH_ENTER) so
									* non-query terms are skipped.  See
									* issue #374, query-term filtering. */
	TpLocalIndexState *lock_state; /* state we acquired the per-index
									* LWLock on (NULL if the lock was
									* already held by an outer caller
									* before this source was created). */
} TpMemtableChainSource;

/* ---------- hash helpers ---------- */

static uint32
chain_term_hash(const void *key, Size keysize)
{
	const char *term = *(const char *const *)key;

	(void)keysize;
	return DatumGetUInt32(hash_any((const unsigned char *)term, strlen(term)));
}

static int
chain_term_match(const void *key1, const void *key2, Size keysize)
{
	const char *t1 = *(const char *const *)key1;
	const char *t2 = *(const char *const *)key2;

	(void)keysize;
	return strcmp(t1, t2);
}

/* ---------- term-table append helpers ---------- */

static ChainTermEntry *
lookup_or_create_term(
		TpMemtableChainSource *src, const char *term, int term_len)
{
	bool			found;
	const char	   *key;
	ChainTermEntry *entry;

	/*
	 * The HTAB key is `const char *` stored in the entry's
	 * first field.  At HASH_ENTER, dynahash memcpy's `*keyPtr`
	 * (i.e. the pointer value `term`) into entry->term.  Since
	 * `term` here may be a transient stack buffer we must
	 * overwrite entry->term with a durable copy living in
	 * src->mcxt before any subsequent lookup.  Overwriting the
	 * first field is safe because the hash bucket is determined
	 * by the *bytes* the pointer references, not the pointer
	 * address, and the bytes are unchanged after we copy them.
	 *
	 * Filter mode (set when the constructor was passed a
	 * non-empty query_terms[]): query terms have already been
	 * pre-inserted into the HTAB, so HASH_FIND is sufficient and
	 * any miss means the lexeme is not a query term — return
	 * NULL so the caller skips term_entry_append.  This is the
	 * key win: for an N-term query against a memtable with M
	 * unique lexemes, we do O(N) HASH_ENTERs at construction +
	 * O(records × terms_per_record) HASH_FINDs during the walk,
	 * vs. O(records × terms_per_record) HASH_ENTERs in the
	 * legacy unfiltered path.  HASH_FIND is materially cheaper
	 * (no allocator traffic, no bucket-split rehash).
	 */
	key = term;
	if (src->filter_terms)
	{
		entry = (ChainTermEntry *)
				hash_search(src->term_ht, &key, HASH_FIND, &found);
		return found ? entry : NULL;
	}

	entry = (ChainTermEntry *)
			hash_search(src->term_ht, &key, HASH_ENTER, &found);

	if (!found)
	{
		MemoryContext old = MemoryContextSwitchTo(src->mcxt);
		char		 *copy;

		copy = palloc(term_len + 1);
		memcpy(copy, term, term_len);
		copy[term_len] = '\0';

		entry->term		   = copy; /* overwrites the HTAB key slot */
		entry->ctids	   = NULL;
		entry->frequencies = NULL;
		entry->count	   = 0;
		entry->capacity	   = 0;
		entry->doc_freq	   = 0;

		MemoryContextSwitchTo(old);
	}

	return entry;
}

static void
term_entry_append(
		TpMemtableChainSource *src,
		ChainTermEntry		  *entry,
		ItemPointer			   ctid,
		int32				   frequency)
{
	if (entry->count == entry->capacity)
	{
		MemoryContext old	  = MemoryContextSwitchTo(src->mcxt);
		int32		  new_cap = entry->capacity == 0 ? 4 : entry->capacity * 2;

		if (entry->ctids == NULL)
		{
			entry->ctids	   = palloc(new_cap * sizeof(ItemPointerData));
			entry->frequencies = palloc(new_cap * sizeof(int32));
		}
		else
		{
			entry->ctids =
					repalloc(entry->ctids, new_cap * sizeof(ItemPointerData));
			entry->frequencies =
					repalloc(entry->frequencies, new_cap * sizeof(int32));
		}
		entry->capacity = new_cap;
		MemoryContextSwitchTo(old);
	}

	entry->ctids[entry->count]		 = *ctid;
	entry->frequencies[entry->count] = frequency;
	entry->count++;
	entry->doc_freq++;
}

/* ---------- record ingestion ---------- */

/*
 * Record one (ctid → doc_length) row in the source's
 * doc-length HTAB.  Duplicate ctids in the chain would indicate
 * a corrupt write (each heap tuple writes exactly one chain
 * record); ereport on collision.
 */
static void
ingest_doclen(TpMemtableChainSource *src, ItemPointer ctid, int32 doc_length)
{
	bool			  found;
	ChainDocLenEntry *dle;

	dle = (ChainDocLenEntry *)
			hash_search(src->doclen_ht, ctid, HASH_ENTER, &found);
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch chain has duplicate ctid (%u,%u)",
						ItemPointerGetBlockNumber(ctid),
						ItemPointerGetOffsetNumber(ctid))));
	dle->doc_length = doc_length;
}

/*
 * Decode a v2 TpVector payload and append (ctid, freq) entries
 * to per-term accumulators.  Payload may be inline on a record
 * or reassembled across continuation pages by the caller.
 */
static void
ingest_terms(
		TpMemtableChainSource *src,
		ItemPointer			   ctid,
		const char			  *vector_bytes,
		uint32				   vector_len)
{
	TpVector	  *vec;
	TpVectorEntry *entry;

	if (vector_len == 0)
		return;

	/*
	 * The bytes here come from chain pages we wrote ourselves
	 * via `tp_add_document_terms` → `create_tpvector_from_strings`,
	 * which always emits a well-formed v2 buffer.  Disk-level
	 * corruption is caught by the page CRC in the buffer
	 * manager, and structural corruption of an in-process write
	 * would point at our own bug rather than untrusted input.
	 * Run a full bounds-check only in assert-enabled builds so
	 * we don't pay for re-walking every varint on the read path.
	 */
#ifdef USE_ASSERT_CHECKING
	tpvector_validate_v2_buffer(vector_bytes, vector_len);
#endif
	vec = (TpVector *)vector_bytes;

	entry = get_tpvector_first_entry(vec);
	for (int i = 0; i < vec->entry_count; i++)
	{
		TpVectorEntryView v;
		ChainTermEntry	 *te;
		char			  stackbuf[256];
		char			 *term_cstr;

		/*
		 * Decode-and-advance in a single pass: returns the next
		 * entry pointer in addition to populating `v`, so we
		 * don't pay for a second varint decode of (freq, lex_len)
		 * just to advance the cursor.  The legacy pattern
		 * (`tpvector_entry_decode` + `get_tpvector_next_entry`)
		 * decoded both varints twice per entry, which became the
		 * dominant per-record cost on read-heavy workloads
		 * against the on-disk memtable chain (issue #374).
		 */
		entry = tpvector_entry_decode_advance(entry, &v);

		/*
		 * Stack-allocate the lookup key for short lexemes
		 * (the common case in English / European text after
		 * stemming).  Fall back to a palloc for unusually
		 * long ones.  The stack copy is reused per record
		 * and not retained.
		 */
		if (v.lexeme_len < sizeof(stackbuf))
			term_cstr = stackbuf;
		else
			term_cstr = palloc(v.lexeme_len + 1);

		memcpy(term_cstr, v.lexeme, v.lexeme_len);
		term_cstr[v.lexeme_len] = '\0';

		te = lookup_or_create_term(src, term_cstr, (int)v.lexeme_len);
		if (te != NULL)
			term_entry_append(src, te, ctid, (int32)v.frequency);

		if (term_cstr != stackbuf)
			pfree(term_cstr);
	}
}

/* ---------- chain walk ---------- */

/*
 * Walk every record in the on-disk chain and ingest it into
 * src->doclen_ht and src->term_ht.  Delegates page-level
 * traversal (buffer locking, fragment reassembly, structural
 * validation) to the shared TpChainWalker primitive in
 * chain_walker.c so the apply protocol (cache.c) can reuse it.
 */
static void
walk_chain(TpMemtableChainSource *src, Relation rel)
{
	BlockNumber			start;
	TpIndexMetaPage		metap = tp_get_metapage(rel);
	TpChainWalker	   *walker;
	TpChainWalkerRecord rec;

	start = metap->memtable_head_blkno;
	pfree(metap);

	walker = tp_chain_walker_open(rel, start, 0, src->mcxt);

	while (tp_chain_walker_next(walker, &rec))
	{
		ingest_doclen(src, &rec.ctid, rec.doc_length);
		ingest_terms(src, &rec.ctid, rec.vector_bytes, rec.vector_len);

		/*
		 * Fragment payloads are palloc'd in src->mcxt by the
		 * walker; the buffer survives until src is closed.  We
		 * could pfree() here to bound peak usage, but the source
		 * mcxt is short-lived (per-query) and fragment payloads
		 * are rare enough that the bookkeeping isn't worth it.
		 */
	}

	src->chain_pages = tp_chain_walker_pages_visited(walker);
	tp_chain_walker_close(walker);
}

/* ---------- TpDataSourceOps implementations ---------- */

static TpPostingData *
chain_get_postings(TpDataSource *source, const char *term)
{
	TpMemtableChainSource *src = (TpMemtableChainSource *)source;
	ChainTermEntry		  *entry;
	const char			  *key;
	bool				   found;
	TpPostingData		  *data;

	if (term == NULL || term[0] == '\0')
		return NULL;

	key	  = term;
	entry = (ChainTermEntry *)
			hash_search(src->term_ht, &key, HASH_FIND, &found);
	if (!found || entry->count == 0)
		return NULL;

	data		   = tp_alloc_posting_data(entry->count);
	data->count	   = entry->count;
	data->doc_freq = entry->doc_freq;
	memcpy(data->ctids, entry->ctids, entry->count * sizeof(ItemPointerData));
	memcpy(data->frequencies,
		   entry->frequencies,
		   entry->count * sizeof(int32));
	return data;
}

static void
chain_free_postings(TpDataSource *source, TpPostingData *data)
{
	(void)source;
	tp_free_posting_data(data);
}

static int32
chain_get_doc_length(TpDataSource *source, ItemPointer ctid)
{
	TpMemtableChainSource *src = (TpMemtableChainSource *)source;
	ChainDocLenEntry	  *dle;
	bool				   found;

	dle = (ChainDocLenEntry *)
			hash_search(src->doclen_ht, ctid, HASH_FIND, &found);
	if (!found)
		return -1;
	return dle->doc_length;
}

static uint32
chain_get_doc_freq(TpDataSource *source, const char *term)
{
	TpMemtableChainSource *src = (TpMemtableChainSource *)source;
	ChainTermEntry		  *entry;
	const char			  *key;
	bool				   found;

	if (term == NULL || term[0] == '\0')
		return 0;

	key	  = term;
	entry = (ChainTermEntry *)
			hash_search(src->term_ht, &key, HASH_FIND, &found);
	if (!found)
		return 0;
	return (uint32)entry->doc_freq;
}

static void
chain_close(TpDataSource *source)
{
	TpMemtableChainSource *src = (TpMemtableChainSource *)source;

	if (src->mcxt != NULL)
		MemoryContextDelete(src->mcxt);
	if (src->lock_state != NULL)
		tp_release_index_lock(src->lock_state);
	pfree(src);
}

static const TpDataSourceOps chain_source_ops = {
		.get_postings	= chain_get_postings,
		.free_postings	= chain_free_postings,
		.get_doc_length = chain_get_doc_length,
		.get_doc_freq	= chain_get_doc_freq,
		.close			= chain_close,
};

/* ---------- public constructor ---------- */

TpDataSource *
tp_memtable_chain_source_create(
		TpLocalIndexState *state,
		Relation		   rel,
		const char *const *query_terms,
		int				   query_term_count)
{
	TpMemtableChainSource *src;
	MemoryContext		   mcxt;
	HASHCTL				   info;
	TpLocalIndexState	  *lock_state_to_release;

	Assert(state != NULL);
	Assert(rel != NULL);
	Assert(query_term_count >= 0);
	Assert(query_term_count == 0 || query_terms != NULL);

	/*
	 * Acquire the per-index LWLock in SHARED mode for the whole
	 * scan.  This excludes spill (LW_EXCLUSIVE) which would
	 * otherwise rip chain pages out from under us.
	 *
	 * Lock ownership: if the caller already held the lock (e.g.,
	 * scan.c acquires SHARED for the whole scan and we're being
	 * created from inside that scope), tp_acquire_index_lock is a
	 * no-op and *we* must not release on close — otherwise the
	 * outer caller would suddenly be operating without the lock.
	 * We remember whether we ourselves grabbed the lock; close()
	 * releases only when we own the acquisition.  Releasing
	 * eagerly in close() (instead of deferring to the xact-end
	 * callback) is required because the LWLock memory lives in
	 * the shared DSA region, which is freed by
	 * tp_cleanup_index_shared_memory() when an index is dropped
	 * (including ON COMMIT DROP on a temp table); leaving the
	 * lock held past the source's lifetime risks the
	 * LWLockReleaseAll() at xact-end dereferencing freed memory.
	 */
	if (state->lock_held)
		lock_state_to_release = NULL;
	else
		lock_state_to_release = state;
	tp_acquire_index_lock(state, LW_SHARED);
	/*
	 * Defensive: if our acquire was a no-op because the caller
	 * already held an exclusive lock (e.g., spill path), still
	 * don't release on close().
	 */
	if (lock_state_to_release != NULL && !state->lock_held)
		lock_state_to_release = NULL;

	/*
	 * Quick empty-chain test under SHARED metapage lock so we
	 * skip the constructor when there are no records.  This
	 * matches the existing tp_memtable_source_create() contract
	 * of returning NULL for an empty memtable.
	 */
	{
		Buffer metabuf;
		bool   empty;

		metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		/*
		 * Use the version-tolerant helper (issue #383): on a v6
		 * page the on-disk field bytes are zero (= block 0,
		 * the metapage itself), not InvalidBlockNumber.
		 */
		empty =
				(tp_metapage_read_memtable_head(BufferGetPage(metabuf)) ==
				 InvalidBlockNumber);
		UnlockReleaseBuffer(metabuf);
		if (empty)
		{
			/*
			 * Release the lock we just acquired (if any) before
			 * returning NULL — the chain_close path doesn't run
			 * for a NULL source.
			 */
			if (lock_state_to_release != NULL)
				tp_release_index_lock(lock_state_to_release);
			return NULL;
		}
	}

	/*
	 * Allocate the source struct in CurrentMemoryContext so it
	 * survives our private mcxt deletion in close().  All
	 * accumulator memory lives inside mcxt.
	 */
	src	 = (TpMemtableChainSource *)palloc0(sizeof(TpMemtableChainSource));
	mcxt = AllocSetContextCreate(
			CurrentMemoryContext,
			"pg_textsearch chain source",
			ALLOCSET_DEFAULT_SIZES);
	src->mcxt		  = mcxt;
	src->base.ops	  = &chain_source_ops;
	src->lock_state	  = lock_state_to_release;
	src->filter_terms = (query_term_count > 0);

	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);
		long		  initial_term_buckets;

		/*
		 * Filter mode: pre-size the term HTAB to exactly the
		 * number of query terms (rounded up to dynahash's
		 * minimum).  Unfiltered mode keeps the legacy 1024
		 * default since it must accommodate the entire term
		 * dictionary in the chain (used by the spill path).
		 */
		initial_term_buckets = src->filter_terms ? (long)query_term_count + 1
												 : 1024L;

		memset(&info, 0, sizeof(info));
		info.keysize   = sizeof(char *);
		info.entrysize = sizeof(ChainTermEntry);
		info.hash	   = chain_term_hash;
		info.match	   = chain_term_match;
		info.hcxt	   = mcxt;
		src->term_ht   = hash_create(
				  "pg_textsearch chain source: terms",
				  initial_term_buckets,
				  &info,
				  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

		memset(&info, 0, sizeof(info));
		info.keysize   = sizeof(ItemPointerData);
		info.entrysize = sizeof(ChainDocLenEntry);
		info.hcxt	   = mcxt;
		src->doclen_ht = hash_create(
				"pg_textsearch chain source: doc lengths",
				1024,
				&info,
				HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		MemoryContextSwitchTo(old);
	}

	/*
	 * Filter mode: pre-insert query terms into the term HTAB
	 * with empty postings.  The chain walk's ingest_terms uses
	 * HASH_FIND-only against this HTAB, so any lexeme not
	 * pre-inserted is skipped (no posting list materialised, no
	 * dynahash growth, no per-term memcpy).  This is the
	 * dominant read-path speedup for queries against a sizeable
	 * memtable: instead of building an HTAB with every unique
	 * lexeme in the memtable (and then reading only N of them),
	 * we build an HTAB with exactly the N terms the caller is
	 * going to read.  Issue #374.
	 */
	if (src->filter_terms)
	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		for (int q = 0; q < query_term_count; q++)
		{
			const char	   *qt = query_terms[q];
			Size			qt_len;
			char		   *copy;
			ChainTermEntry *entry;
			const char	   *key;
			bool			found;

			if (qt == NULL || qt[0] == '\0')
				continue;
			qt_len = strlen(qt);
			copy   = palloc(qt_len + 1);
			memcpy(copy, qt, qt_len + 1);

			/*
			 * Briefly flip filter_terms off so this HASH_ENTER
			 * goes through the normal create path.  We re-enable
			 * filtering after pre-insertion completes; the walk
			 * uses HASH_FIND-only.
			 */
			src->filter_terms = false;
			key				  = copy;
			entry			  = (ChainTermEntry *)
					hash_search(src->term_ht, &key, HASH_ENTER, &found);
			src->filter_terms = true;
			if (!found)
			{
				entry->term		   = copy;
				entry->ctids	   = NULL;
				entry->frequencies = NULL;
				entry->count	   = 0;
				entry->capacity	   = 0;
				entry->doc_freq	   = 0;
			}
			else
			{
				/*
				 * Duplicate query terms (e.g. "the the") collapse
				 * into a single HTAB entry — already present from
				 * an earlier iteration, nothing more to do.
				 */
				pfree(copy);
			}
		}

		MemoryContextSwitchTo(old);
	}

	walk_chain(src, rel);

	/*
	 * Corpus-level counters: report just the in-flight memtable
	 * contribution (derived from this chain).  Scoring callers
	 * (bm25.c, types/query.c) layer this on top of the
	 * persisted-segment totals stored on the metapage to obtain
	 * whole-index totals.
	 */
	{
		long			  doc_count;
		int64			  total_len = 0;
		HASH_SEQ_STATUS	  seq;
		ChainDocLenEntry *dle;

		doc_count = hash_get_num_entries(src->doclen_ht);
		hash_seq_init(&seq, src->doclen_ht);
		while ((dle = (ChainDocLenEntry *)hash_seq_search(&seq)) != NULL)
			total_len += dle->doc_length;

		src->base.total_docs = (int32)doc_count;
		src->base.total_len	 = total_len;
	}

	return (TpDataSource *)src;
}

/*
 * Return the total number of chain pages walked by a chain source.
 * Includes both regular memtable pages and continuation pages of
 * any fragments encountered.  Matches the writer's
 * shared->chain_page_count accounting (which adds +1 per regular
 * page and +(2 + n_cont) per fragment publish).  Used by
 * tp_rebuild_index_from_disk to seed the atomic counter when a
 * backend reopens an index whose shmem state was just (re-)created.
 */
uint32
tp_memtable_chain_source_page_count(TpDataSource *base)
{
	TpMemtableChainSource *src;

	if (base == NULL)
		return 0;

	Assert(base->ops == &chain_source_ops);
	src = (TpMemtableChainSource *)base;
	return src->chain_pages;
}

/* ---------- public extractor ---------- */

#include "segment/dictionary.h"
#include "segment/docmap.h"

static int
term_info_cmp(const void *a, const void *b)
{
	const TermInfo *ta = (const TermInfo *)a;
	const TermInfo *tb = (const TermInfo *)b;
	int				r  = strcmp(ta->term, tb->term);

	return r;
}

void
tp_memtable_chain_source_extract(
		TpDataSource	 *base,
		MemoryContext	  dest_mcxt,
		TermInfo		**out_terms,
		uint32			 *out_num_terms,
		TpDocMapBuilder **out_docmap)
{
	TpMemtableChainSource *src = (TpMemtableChainSource *)base;
	MemoryContext		   old;
	HASH_SEQ_STATUS		   seq;
	ChainTermEntry		  *te;
	ChainDocLenEntry	  *dle;
	uint32				   num_terms;
	uint32				   i;
	TermInfo			  *terms;
	TpDocMapBuilder		  *docmap;

	Assert(base != NULL);
	Assert(base->ops == &chain_source_ops);
	Assert(out_terms != NULL && out_num_terms != NULL && out_docmap != NULL);

	old = MemoryContextSwitchTo(dest_mcxt);

	/* Build docmap from the doclen HTAB. */
	docmap = tp_docmap_create();
	hash_seq_init(&seq, src->doclen_ht);
	while ((dle = (ChainDocLenEntry *)hash_seq_search(&seq)) != NULL)
		tp_docmap_add(docmap, &dle->ctid, (uint32)dle->doc_length);
	tp_docmap_finalize(docmap);

	/* Materialize the term dictionary. */
	num_terms = (uint32)hash_get_num_entries(src->term_ht);
	if (num_terms == 0)
	{
		*out_terms	   = NULL;
		*out_num_terms = 0;
		*out_docmap	   = docmap;
		MemoryContextSwitchTo(old);
		return;
	}

	terms = (TermInfo *)palloc0(num_terms * sizeof(TermInfo));
	i	  = 0;
	hash_seq_init(&seq, src->term_ht);
	while ((te = (ChainTermEntry *)hash_seq_search(&seq)) != NULL)
	{
		uint32 c = (uint32)te->count;

		terms[i].term_len = (uint32)strlen(te->term);
		terms[i].term	  = (char *)palloc(terms[i].term_len + 1);
		memcpy(terms[i].term, te->term, terms[i].term_len + 1);
		terms[i].count	  = c;
		terms[i].doc_freq = (uint32)te->doc_freq;

		if (c > 0)
		{
			terms[i].ctids = (ItemPointerData *)palloc(
					c * sizeof(ItemPointerData));
			memcpy(terms[i].ctids, te->ctids, c * sizeof(ItemPointerData));
			terms[i].freqs = (int32 *)palloc(c * sizeof(int32));
			memcpy(terms[i].freqs, te->frequencies, c * sizeof(int32));
		}

		i++;
	}
	Assert(i == num_terms);

	/* Sort lexicographically for the segment writer's dict layout. */
	qsort(terms, num_terms, sizeof(TermInfo), term_info_cmp);

	for (i = 0; i < num_terms; i++)
		terms[i].dict_entry_idx = i;

	*out_terms	   = terms;
	*out_num_terms = num_terms;
	*out_docmap	   = docmap;

	MemoryContextSwitchTo(old);
}

/* ---------------------------------------------------------------------
 * Scaffold SQL function for unit-level coverage.
 *
 * bm25_test_chain_source(idx_name text, case_name text) -> text
 *
 * Each case writes one or more records via tp_memtable_append()
 * with real TpVector payloads, then opens a chain source over
 * the same index and verifies the read-back through the
 * TpDataSource interface.  Returns 'OK' or 'FAIL: <detail>'.
 *
 * The harness drops and re-creates the host index between test
 * cases (in the SQL harness) so each case starts on an empty
 * chain.
 * ---------------------------------------------------------------------
 */

#include "memtable/log.h"

PG_FUNCTION_INFO_V1(bm25_test_chain_source);

#define TEST_FAIL(fmt, ...)                                 \
	do                                                      \
	{                                                       \
		char *_buf = psprintf("FAIL: " fmt, ##__VA_ARGS__); \
		if (rel)                                            \
			index_close(rel, RowExclusiveLock);             \
		if (src)                                            \
			tp_source_close(src);                           \
		PG_RETURN_TEXT_P(cstring_to_text(_buf));            \
	} while (0)

#define TEST_OK()                                \
	do                                           \
	{                                            \
		if (src)                                 \
			tp_source_close(src);                \
		index_close(rel, RowExclusiveLock);      \
		PG_RETURN_TEXT_P(cstring_to_text("OK")); \
	} while (0)

static TpVector *
test_make_tpvector(
		const char	*index_name,
		int			 n_terms,
		const char **terms,
		const int32 *freqs)
{
	return create_tpvector_from_strings(index_name, n_terms, terms, freqs);
}

static int32
test_append_terms(
		Relation	 rel,
		const char	*idx_name,
		ItemPointer	 ctid,
		int			 n_terms,
		const char **terms,
		const int32 *freqs)
{
	TpVector *v			 = test_make_tpvector(idx_name, n_terms, terms, freqs);
	int32	  doc_length = 0;

	for (int i = 0; i < n_terms; i++)
		doc_length += freqs[i];

	tp_memtable_append(rel, ctid, doc_length, (const char *)v, VARSIZE(v));
	pfree(v);
	return doc_length;
}

Datum
bm25_test_chain_source(PG_FUNCTION_ARGS)
{
	text			  *idx_text	 = PG_GETARG_TEXT_PP(0);
	text			  *case_text = PG_GETARG_TEXT_PP(1);
	char			  *idx_name	 = text_to_cstring(idx_text);
	char			  *case_name = text_to_cstring(case_text);
	Oid				   oid		 = tp_resolve_index_name_shared(idx_name);
	Relation		   rel		 = NULL;
	TpDataSource	  *src		 = NULL;
	TpLocalIndexState *state;

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pg_textsearch index \"%s\" not found", idx_name)));

	rel	  = index_open(oid, RowExclusiveLock);
	state = tp_get_local_index_state(oid);
	if (state == NULL)
		TEST_FAIL("local index state not available");

	if (strcmp(case_name, "empty_chain") == 0)
	{
		ItemPointerData any_ctid;

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src != NULL)
			TEST_FAIL("chain source over empty index is non-NULL");

		ItemPointerSet(&any_ctid, 1, 1);
		/* Without a source we cannot exercise the API, so just OK. */
		TEST_OK();
	}
	else if (strcmp(case_name, "one_doc_one_term") == 0)
	{
		ItemPointerData ctid;
		const char	   *terms[] = {"alpha"};
		int32			freqs[] = {3};
		int32			expected_doc_length;
		TpPostingData  *post;

		ItemPointerSet(&ctid, 100, 1);
		expected_doc_length =
				test_append_terms(rel, idx_name, &ctid, 1, terms, freqs);

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL after one append");

		if (src->total_docs != 1)
			TEST_FAIL("total_docs=%d, expected 1", src->total_docs);
		if (src->total_len != expected_doc_length)
			TEST_FAIL(
					"total_len=%lld, expected %d",
					(long long)src->total_len,
					expected_doc_length);

		post = tp_source_get_postings(src, "alpha");
		if (post == NULL)
			TEST_FAIL("get_postings(alpha) returned NULL");
		if (post->count != 1)
			TEST_FAIL("count=%d, expected 1", post->count);
		if (post->doc_freq != 1)
			TEST_FAIL("doc_freq=%d, expected 1", post->doc_freq);
		if (!ItemPointerEquals(&post->ctids[0], &ctid))
			TEST_FAIL("ctid roundtrip mismatch");
		if (post->frequencies[0] != 3)
			TEST_FAIL("freq=%d, expected 3", post->frequencies[0]);
		tp_source_free_postings(src, post);

		if (tp_source_get_doc_length(src, &ctid) != expected_doc_length)
			TEST_FAIL("get_doc_length mismatch");
		TEST_OK();
	}
	else if (strcmp(case_name, "multi_doc_shared_term") == 0)
	{
		const char	   *terms[] = {"shared"};
		int32			freqs[] = {1};
		const int		N		= 5;
		TpPostingData  *post;
		ItemPointerData expected_ctids[5];

		for (int i = 0; i < N; i++)
		{
			ItemPointerSet(
					&expected_ctids[i],
					(BlockNumber)(200 + i),
					(OffsetNumber)(i + 1));
			test_append_terms(
					rel, idx_name, &expected_ctids[i], 1, terms, freqs);
		}

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL");

		post = tp_source_get_postings(src, "shared");
		if (post == NULL)
			TEST_FAIL("get_postings(shared) NULL");
		if (post->count != N)
			TEST_FAIL("count=%d, expected %d", post->count, N);
		if (post->doc_freq != N)
			TEST_FAIL("doc_freq=%d, expected %d", post->doc_freq, N);
		for (int i = 0; i < N; i++)
		{
			if (!ItemPointerEquals(&post->ctids[i], &expected_ctids[i]))
				TEST_FAIL("ctid mismatch at i=%d", i);
			if (post->frequencies[i] != 1)
				TEST_FAIL(
						"freq mismatch at i=%d: got %d",
						i,
						post->frequencies[i]);
		}
		tp_source_free_postings(src, post);
		TEST_OK();
	}
	else if (strcmp(case_name, "multi_term") == 0)
	{
		ItemPointerData ctid;
		const char	   *terms[] = {"red", "green", "blue"};
		int32			freqs[] = {2, 1, 4};
		TpPostingData  *post;

		ItemPointerSet(&ctid, 300, 1);
		test_append_terms(rel, idx_name, &ctid, 3, terms, freqs);

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL");

		for (int i = 0; i < 3; i++)
		{
			post = tp_source_get_postings(src, terms[i]);
			if (post == NULL)
				TEST_FAIL("get_postings(%s) NULL", terms[i]);
			if (post->count != 1)
				TEST_FAIL("count for %s = %d", terms[i], post->count);
			if (post->frequencies[0] != freqs[i])
				TEST_FAIL(
						"freq for %s = %d, expected %d",
						terms[i],
						post->frequencies[0],
						freqs[i]);
			if (!ItemPointerEquals(&post->ctids[0], &ctid))
				TEST_FAIL("ctid mismatch for %s", terms[i]);
			tp_source_free_postings(src, post);
		}
		TEST_OK();
	}
	else if (strcmp(case_name, "multi_page_chain") == 0)
	{
		const int N = 30;
		/*
		 * Use a fixed lexeme so the per-record TpVector is
		 * relatively large from filler frequencies and forces
		 * the chain past one page.
		 */
		char		   big_term[512];
		const char	  *terms[1];
		int32		   freqs[1];
		TpPostingData *post;

		for (size_t i = 0; i < sizeof(big_term) - 1; i++)
			big_term[i] = 'a' + (char)(i % 26);
		big_term[sizeof(big_term) - 1] = '\0';
		terms[0]					   = big_term;
		freqs[0]					   = 1;

		for (int i = 0; i < N; i++)
		{
			ItemPointerData ctid;
			ItemPointerSet(
					&ctid, (BlockNumber)(400 + i), (OffsetNumber)(i + 1));
			test_append_terms(rel, idx_name, &ctid, 1, terms, freqs);
		}

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL");

		if (src->total_docs != N)
			TEST_FAIL("total_docs=%d, expected %d", src->total_docs, N);

		post = tp_source_get_postings(src, big_term);
		if (post == NULL)
			TEST_FAIL("get_postings(big_term) NULL");
		if (post->count != N)
			TEST_FAIL("count=%d, expected %d", post->count, N);
		tp_source_free_postings(src, post);
		TEST_OK();
	}
	else if (strcmp(case_name, "term_not_found") == 0)
	{
		ItemPointerData ctid;
		const char	   *terms[] = {"present"};
		int32			freqs[] = {1};
		TpPostingData  *post;

		ItemPointerSet(&ctid, 500, 1);
		test_append_terms(rel, idx_name, &ctid, 1, terms, freqs);

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL");

		post = tp_source_get_postings(src, "absent");
		if (post != NULL)
			TEST_FAIL("get_postings(absent) returned non-NULL");
		TEST_OK();
	}
	else if (strcmp(case_name, "doc_length_lookup") == 0)
	{
		ItemPointerData ctid_a, ctid_b, ctid_missing;
		const char	   *t_a[] = {"x", "y"};
		int32			f_a[] = {3, 4};
		const char	   *t_b[] = {"z"};
		int32			f_b[] = {7};

		ItemPointerSet(&ctid_a, 600, 1);
		ItemPointerSet(&ctid_b, 600, 2);
		ItemPointerSet(&ctid_missing, 9999, 1);
		test_append_terms(rel, idx_name, &ctid_a, 2, t_a, f_a);
		test_append_terms(rel, idx_name, &ctid_b, 1, t_b, f_b);

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL");

		if (tp_source_get_doc_length(src, &ctid_a) != 7)
			TEST_FAIL(
					"doc_length(ctid_a)=%d, expected 7",
					tp_source_get_doc_length(src, &ctid_a));
		if (tp_source_get_doc_length(src, &ctid_b) != 7)
			TEST_FAIL(
					"doc_length(ctid_b)=%d, expected 7",
					tp_source_get_doc_length(src, &ctid_b));
		if (tp_source_get_doc_length(src, &ctid_missing) != -1)
			TEST_FAIL("doc_length(missing) != -1");
		TEST_OK();
	}
	else if (strcmp(case_name, "fragment_roundtrip") == 0)
	{
		/*
		 * Construct a TpVector whose payload exceeds
		 * TP_MEMTABLE_PAGE_MAX_VECTOR_LEN so the writer takes
		 * the fragment branch (head page + N continuations
		 * + fresh tail).  Then verify the chain source
		 * reassembles the payload and round-trips per-term
		 * postings and the doc-length entry.
		 */
		const int		N_TERMS	 = 240;
		const int		LEX_LEN	 = 60;
		const char	  **terms	 = palloc(N_TERMS * sizeof(char *));
		int32		   *freqs	 = palloc(N_TERMS * sizeof(int32));
		int32			expected = 0;
		ItemPointerData ctid;
		TpPostingData  *post;
		uint32			vec_len;

		for (int i = 0; i < N_TERMS; i++)
		{
			char *t = palloc(LEX_LEN + 1);
			snprintf(t, LEX_LEN + 1, "frag_%05d_", i);
			/*
			 * Pad with deterministic bytes so each lexeme is
			 * exactly LEX_LEN characters; the lookup later
			 * needs to use the same encoding.
			 */
			for (int j = (int)strlen(t); j < LEX_LEN; j++)
				t[j] = 'a' + (j % 26);
			t[LEX_LEN] = '\0';
			terms[i]   = t;
			freqs[i]   = i + 1;
			expected += freqs[i];
		}

		/*
		 * Sanity: confirm the resulting TpVector exceeds the
		 * single-page cap so we're actually exercising the
		 * fragment path.
		 */
		{
			TpVector *v = test_make_tpvector(idx_name, N_TERMS, terms, freqs);
			vec_len		= VARSIZE(v);
			pfree(v);
		}
		if (vec_len <= TP_MEMTABLE_PAGE_MAX_VECTOR_LEN)
			TEST_FAIL(
					"test built TpVector with len=%u (<= max %u); "
					"increase N_TERMS/LEX_LEN to exercise fragmentation",
					vec_len,
					(unsigned)TP_MEMTABLE_PAGE_MAX_VECTOR_LEN);

		ItemPointerSet(&ctid, 700, 1);
		test_append_terms(rel, idx_name, &ctid, N_TERMS, terms, freqs);

		src = tp_memtable_chain_source_create(state, rel, NULL, 0);
		if (src == NULL)
			TEST_FAIL("chain source NULL after fragment append");

		if (src->total_docs != 1)
			TEST_FAIL("total_docs=%d, expected 1", src->total_docs);
		if (src->total_len != expected)
			TEST_FAIL(
					"total_len=%lld, expected %d",
					(long long)src->total_len,
					expected);
		if (tp_source_get_doc_length(src, &ctid) != expected)
			TEST_FAIL(
					"doc_length=%d, expected %d",
					tp_source_get_doc_length(src, &ctid),
					expected);

		/* Spot-check a handful of terms across the payload. */
		{
			int probe_idx[] = {0, 1, N_TERMS / 4, N_TERMS / 2, N_TERMS - 1};

			for (size_t k = 0; k < sizeof(probe_idx) / sizeof(probe_idx[0]);
				 k++)
			{
				int			i = probe_idx[k];
				const char *t = terms[i];

				post = tp_source_get_postings(src, t);
				if (post == NULL)
					TEST_FAIL("get_postings(%s) returned NULL", t);
				if (post->count != 1)
					TEST_FAIL("count for %s = %d, expected 1", t, post->count);
				if (post->doc_freq != 1)
					TEST_FAIL(
							"doc_freq for %s = %d, expected 1",
							t,
							post->doc_freq);
				if (!ItemPointerEquals(&post->ctids[0], &ctid))
					TEST_FAIL("ctid mismatch for %s", t);
				if (post->frequencies[0] != freqs[i])
					TEST_FAIL(
							"freq for %s = %d, expected %d",
							t,
							post->frequencies[0],
							freqs[i]);
				tp_source_free_postings(src, post);
			}
		}

		for (int i = 0; i < N_TERMS; i++)
			pfree((void *)terms[i]);
		pfree(terms);
		pfree(freqs);
		TEST_OK();
	}

	TEST_FAIL("unknown case '%s'", case_name);
}
