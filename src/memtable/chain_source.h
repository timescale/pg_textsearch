/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * chain_source.h - TpDataSource over the on-disk memtable page
 *                  chain (issue #374).
 *
 * Reads document records from the chain rooted at
 * meta.memtable_head_blkno, aggregates them into in-memory
 * per-term posting lists and a per-ctid doc-length map, and
 * serves them through the standard TpDataSourceOps interface.
 *
 * Concurrency contract:
 *
 *     per-index LWLock SHARED
 *         └─► metapage buffer SHARED  (held briefly, released)
 *         └─► chain page buffer SHARED  (one at a time)
 *
 * The per-index LWLock SHARED acquisition (idempotent within an
 * xact via tp_acquire_index_lock) excludes spill (which holds
 * LW_EXCLUSIVE) from racing the eager open-walk.  Concurrent
 * writers also hold the per-index lock SHARED so they coexist
 * with us; their tail-buffer EXCL serializes against our SHARED
 * read of the same page.
 *
 * Memory ownership: the constructor creates a child
 * MemoryContext under CurrentMemoryContext; all accumulators,
 * key copies, and HTAB internals live inside it.  `close()`
 * deletes the child context in one shot (no per-entry frees).
 */
#pragma once

#include <postgres.h>

#include <utils/rel.h>

#include "index/source.h"
#include "index/state.h"

/*
 * Construct a chain-backed data source for `rel`.
 *
 * Eagerly walks the entire chain in the constructor (one pass,
 * one SHARED buffer lock at a time).  The returned TpDataSource
 * serves get_postings / get_doc_length from in-memory
 * accumulators thereafter.
 *
 * When `query_term_count > 0`, the term HTAB is pre-populated
 * with `query_terms[0..query_term_count-1]` (NUL-terminated
 * lexeme bytes; UTF-8 / index text_config's encoding) and the
 * chain walk only materialises postings for those terms.  This is
 * the typical scoring path: a 2–5 term query against a memtable
 * with thousands of unique lexemes avoids building an HTAB of
 * every lexeme just to read N of them.  Pass `query_terms=NULL,
 * query_term_count=0` to disable the filter and accumulate every
 * lexeme — this is the contract required by the spill path
 * (which needs the full term dictionary) and by callers that read
 * the source's corpus totals without inspecting per-term
 * postings.
 *
 * Returns NULL if the chain is empty (mirrors the existing
 * tp_memtable_source_create() contract that an absent memtable
 * is signalled by a NULL source).
 *
 * Caller must free via tp_source_close().
 */
extern TpDataSource *tp_memtable_chain_source_create(
		TpLocalIndexState *state,
		Relation		   rel,
		const char *const *query_terms,
		int				   query_term_count);

/*
 * Return the total number of memtable chain pages walked by a
 * chain source (regular pages + fragment continuation pages).
 *
 * Used by recovery / fresh-backend paths to seed
 * shared->chain_page_count after shmem init has reset the atomic
 * to zero, so the auto-spill heuristic isn't blind to a chain
 * that already exists on disk.  Returns 0 when src is NULL.
 *
 * Asserts that `src` is a chain source (not any other
 * TpDataSource implementation).
 */
extern uint32 tp_memtable_chain_source_page_count(TpDataSource *src);

/*
 * Extract a sorted term dictionary + finalized doc map from a
 * chain source.  Used by the spill path (`tp_do_spill`) to feed
 * `tp_write_segment` without re-walking the chain pages.
 *
 * The output arrays live in `dest_mcxt` and survive the source's
 * `close()`.  Each TermInfo's `term`, `ctids`, and `freqs` are
 * deep-copied out of the source's private mcxt so the caller can
 * safely close the source before consuming the dictionary.  The
 * TpDocMapBuilder is built fresh and `tp_docmap_finalize`-ed; the
 * caller frees it via `tp_docmap_destroy`.
 *
 * Asserts that `src` is a chain source (not any other TpDataSource
 * impl).
 */
struct TermInfo;
struct TpDocMapBuilder;
extern void tp_memtable_chain_source_extract(
		TpDataSource			*src,
		MemoryContext			 dest_mcxt,
		struct TermInfo		   **out_terms,
		uint32					*out_num_terms,
		struct TpDocMapBuilder **out_docmap);
