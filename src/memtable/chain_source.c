/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * chain_source.c - TpDataSource over the on-disk memtable page
 *                  chain (Phase 3 of issue #374).
 *
 * See chain_source.h for the concurrency contract and high-level
 * semantics.
 *
 * Scaffold SQL functions are exposed at the bottom of this file
 * (bm25_test_chain_source).  Phase 3 is scaffold-only; the new
 * source is not yet plumbed into the index AM.  See plan.md.
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
 * ItemPointerData (HASH_BLOBS).  Phase 3 records (ctid →
 * doc_length); duplicate ctids are an invariant violation
 * (each heap tuple writes exactly one chain record) and we
 * surface them as DATA_CORRUPTED at open time.
 */
typedef struct ChainDocLenEntry
{
	ItemPointerData ctid;
	int32			doc_length;
} ChainDocLenEntry;

typedef struct TpMemtableChainSource
{
	TpDataSource  base;		 /* must be first */
	MemoryContext mcxt;		 /* private context, deleted in close() */
	HTAB		 *term_ht;	 /* char* → ChainTermEntry */
	HTAB		 *doclen_ht; /* ItemPointerData → ChainDocLenEntry */
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

/* ---------- structural page validation ---------- */

/*
 * Per-page invariants the chain walker assumes when iterating
 * records.  These guard against silent out-of-bounds reads if
 * the on-page bytes are corrupt.  Called under SHARED buffer
 * lock; never modifies the page.
 */
static void
validate_page_layout(Page page, BlockNumber blkno)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	if (hdr->free_offset < TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET ||
		hdr->free_offset > BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch memtable page block %u has "
						"out-of-range free_offset %u",
						blkno,
						hdr->free_offset)));

	if (hdr->next_block == blkno)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch memtable page block %u links to "
						"itself",
						blkno)));
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
	 */
	key	  = term;
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

	tpvector_validate_v2_buffer(vector_bytes, vector_len);
	vec = (TpVector *)vector_bytes;

	entry = get_tpvector_first_entry(vec);
	for (int i = 0; i < vec->entry_count; i++)
	{
		TpVectorEntryView v;
		ChainTermEntry	 *te;
		char			  stackbuf[256];
		char			 *term_cstr;

		tpvector_entry_decode(entry, &v);

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
		term_entry_append(src, te, ctid, (int32)v.frequency);

		if (term_cstr != stackbuf)
			pfree(term_cstr);

		entry = get_tpvector_next_entry(entry);
	}
}

/*
 * Reassemble a multi-page (FRAGMENT) record's payload.  On
 * entry, `head_page` is the SHARED-locked page hosting the
 * fragment head record `rec`.  This function walks the
 * continuation chain (SHARED-locking each continuation page
 * one at a time) and copies the full payload into a palloc'd
 * buffer in src->mcxt.
 *
 * Returns the block number of the first non-continuation page
 * after the last continuation (i.e. where the outer chain walk
 * should resume), and writes the reassembled buffer pointer
 * to *out_buf.  The caller is responsible for ingesting the
 * payload before that buffer is reclaimed.
 *
 * Holding the head page SHARED across the walk is sufficient
 * because: spill holds per-index LW_EXCLUSIVE (incompatible
 * with our source's LW_SHARED), so the chain cannot disappear
 * under us; concurrent writers only extend NEW (orphan) pages
 * and only EXCL-lock the old tail and metapage, neither of
 * which we touch here.
 */
static BlockNumber
reassemble_fragment(
		TpMemtableChainSource *src,
		Relation			   rel,
		Page				   head_page,
		TpMemtableRecord	  *rec,
		char				 **out_buf)
{
	TpMemtablePageHeader *hdr;
	uint32				  full_len = rec->vector_len;
	char				 *vec_start;
	char				 *page_end;
	uint32				  inline_len;
	char				 *full;
	uint32				  copied;
	BlockNumber			  cont_blkno;
	MemoryContext		  old;

	hdr		  = tp_memtable_page_header(head_page);
	vec_start = (char *)rec->vector_bytes;
	page_end  = (char *)head_page + hdr->free_offset;

	if (page_end <= vec_start)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch fragment head record has no inline "
						"payload")));

	inline_len = (uint32)(page_end - vec_start);

	if (inline_len >= full_len)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch fragment record has inline_len=%u "
						">= full_len=%u (should have been a regular record)",
						inline_len,
						full_len)));

	old	 = MemoryContextSwitchTo(src->mcxt);
	full = palloc(full_len);
	MemoryContextSwitchTo(old);

	memcpy(full, rec->vector_bytes, inline_len);
	copied	   = inline_len;
	cont_blkno = hdr->next_block;

	while (copied < full_len)
	{
		Buffer		cbuf;
		Page		cpage;
		const char *chunk;
		uint32		chunk_len;
		BlockNumber next_blk;

		CHECK_FOR_INTERRUPTS();

		if (cont_blkno == InvalidBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch fragment continuation chain "
							"truncated: copied=%u, expected=%u",
							copied,
							full_len)));

		cbuf = ReadBuffer(rel, cont_blkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		if (!tp_memtable_page_is_continuation(cpage))
		{
			UnlockReleaseBuffer(cbuf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch fragment continuation at block "
							"%u is not a continuation page",
							cont_blkno)));
		}

		chunk = tp_memtable_continuation_page_chunk(cpage, &chunk_len);
		if (chunk_len == 0 || copied + chunk_len > full_len)
		{
			UnlockReleaseBuffer(cbuf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch fragment continuation at block "
							"%u has invalid chunk_len=%u (copied=%u, "
							"full_len=%u)",
							cont_blkno,
							chunk_len,
							copied,
							full_len)));
		}

		memcpy(full + copied, chunk, chunk_len);
		copied += chunk_len;
		next_blk = tp_memtable_page_get_next(cpage);
		UnlockReleaseBuffer(cbuf);
		cont_blkno = next_blk;
	}

	*out_buf = full;
	/*
	 * `cont_blkno` is the next_block of the LAST continuation
	 * page we read — i.e. the block where the outer chain walk
	 * should resume.  May be InvalidBlockNumber if the fragment
	 * was the tail of the chain.
	 */
	return cont_blkno;
}

/* ---------- chain walk ---------- */

static void
walk_chain(TpMemtableChainSource *src, Relation rel)
{
	BlockNumber		cur;
	TpIndexMetaPage metap = tp_get_metapage(rel);

	cur = metap->memtable_head_blkno;
	pfree(metap);

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;
		TpMemtableRecord	 *rec;
		BlockNumber			  next;
		uint16				  seen;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(rel, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (!tp_memtable_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable page at block %u in "
							"index \"%s\" has invalid magic",
							cur,
							RelationGetRelationName(rel))));
		}

		/*
		 * The outer chain walk must only ever traverse regular
		 * memtable pages.  Continuation pages are reached
		 * exclusively via reassemble_fragment() and skipped
		 * past when the outer walk resumes.  Encountering a
		 * continuation page here means either the writer
		 * miscomputed next_block, or a fragment head record's
		 * post-continuation pointer was lost.
		 */
		if (tp_memtable_page_is_continuation(page))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable outer walk reached "
							"continuation page at block %u in index "
							"\"%s\"",
							cur,
							RelationGetRelationName(rel))));
		}

		validate_page_layout(page, cur);

		hdr	 = tp_memtable_page_header(page);
		next = hdr->next_block;

		seen = 0;
		for (rec = tp_memtable_page_first(page); rec != NULL;
			 rec = tp_memtable_page_next(page, rec))
		{
			if (seen >= hdr->n_records)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_textsearch memtable page block %u "
								"iteration exceeds declared n_records=%u",
								cur,
								hdr->n_records)));

			/*
			 * Defensive: each record's vector_len must fit in
			 * the remaining page bytes.  free_offset already
			 * ensures the record stream fits collectively;
			 * this checks per-record bounds.  Skipped for
			 * FRAGMENT records, whose vector_len is the FULL
			 * payload across pages (the on-page byte budget
			 * is the inline prefix, bounded by free_offset).
			 */
			if ((rec->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) == 0)
			{
				char *rec_end = (char *)rec +
								tp_memtable_record_size(rec->vector_len);
				char *page_end = (char *)page + hdr->free_offset;

				if (rec_end > page_end)
				{
					UnlockReleaseBuffer(buf);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("pg_textsearch memtable record at "
									"block %u, index %u extends past "
									"free_offset",
									cur,
									seen)));
				}
			}

			ingest_doclen(src, &rec->ctid, rec->doc_length);

			if ((rec->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) != 0)
			{
				char	   *full;
				BlockNumber post_cont;

				/*
				 * FRAGMENT records must be the sole record on
				 * their head page.  Otherwise the outer
				 * walk's `next` semantics become ambiguous
				 * (other records on the same page would never
				 * be reached after the fragment redirects the
				 * walk past the continuation chain).
				 */
				if (hdr->n_records != 1)
				{
					UnlockReleaseBuffer(buf);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("pg_textsearch fragment head page %u "
									"has n_records=%u (must be 1)",
									cur,
									hdr->n_records)));
				}

				post_cont = reassemble_fragment(src, rel, page, rec, &full);
				ingest_terms(src, &rec->ctid, full, rec->vector_len);
				pfree(full);

				/*
				 * Resume the outer walk after the last
				 * continuation, not at the head page's
				 * next_block (which points to the first
				 * continuation).
				 */
				next = post_cont;
				seen++;
				break;
			}

			ingest_terms(src, &rec->ctid, rec->vector_bytes, rec->vector_len);
			seen++;
		}

		if (seen != hdr->n_records)
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable page block %u iterated "
							"%u records, header says n_records=%u",
							cur,
							seen,
							hdr->n_records)));

		UnlockReleaseBuffer(buf);
		cur = next;
	}
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

static void
chain_close(TpDataSource *source)
{
	TpMemtableChainSource *src = (TpMemtableChainSource *)source;

	if (src->mcxt != NULL)
		MemoryContextDelete(src->mcxt);
	pfree(src);
}

static const TpDataSourceOps chain_source_ops = {
		.get_postings	= chain_get_postings,
		.free_postings	= chain_free_postings,
		.get_doc_length = chain_get_doc_length,
		.close			= chain_close,
};

/* ---------- public constructor ---------- */

TpDataSource *
tp_memtable_chain_source_create(TpLocalIndexState *state, Relation rel)
{
	TpMemtableChainSource *src;
	MemoryContext		   mcxt;
	HASHCTL				   info;

	Assert(state != NULL);
	Assert(rel != NULL);

	/*
	 * Acquire the per-index LWLock in SHARED mode for the whole
	 * scan.  This excludes Phase 4 spill (LW_EXCLUSIVE) which
	 * would otherwise rip chain pages out from under us.
	 * Idempotent within an xact; released by the standard
	 * xact-end callback.
	 */
	tp_acquire_index_lock(state, LW_SHARED);

	/*
	 * Quick empty-chain test under SHARED metapage lock so we
	 * skip the constructor when there are no records.  This
	 * matches the existing tp_memtable_source_create() contract
	 * of returning NULL for an empty memtable.
	 */
	{
		Buffer			metabuf;
		TpIndexMetaPage metap;
		bool			empty;

		metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		metap = (TpIndexMetaPage)PageGetContents(BufferGetPage(metabuf));
		empty = (metap->memtable_head_blkno == InvalidBlockNumber);
		UnlockReleaseBuffer(metabuf);
		if (empty)
			return NULL;
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
	src->mcxt	  = mcxt;
	src->base.ops = &chain_source_ops;

	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		memset(&info, 0, sizeof(info));
		info.keysize   = sizeof(char *);
		info.entrysize = sizeof(ChainTermEntry);
		info.hash	   = chain_term_hash;
		info.match	   = chain_term_match;
		info.hcxt	   = mcxt;
		src->term_ht   = hash_create(
				  "pg_textsearch chain source: terms",
				  1024,
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

	walk_chain(src, rel);

	/*
	 * Corpus-level counters: derive from the chain itself so
	 * scoring computed from this source is internally
	 * consistent.  Phase 4 will replace these with whole-index
	 * totals (memtable chain + segments).
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
 * The scaffold drops and re-creates the host index between
 * test cases (in the SQL harness) so each case starts on an
 * empty chain.
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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

		src = tp_memtable_chain_source_create(state, rel);
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
