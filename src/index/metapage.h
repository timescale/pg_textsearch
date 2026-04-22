/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * metapage.h - Index metapage structures and operations
 */
#pragma once

#include <postgres.h>

#include <storage/block.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/rel.h>

#include "constants.h"

typedef struct TpLocalIndexState TpLocalIndexState;

/*
 * Tapir Index Metapage Structure
 *
 * The metapage is stored on block 0 of every Tapir index and contains
 * configuration parameters and global statistics needed for BM25 scoring.
 *
 * Segment hierarchy: LSM-style tiered compaction with TP_MAX_LEVELS levels.
 * Level 0 receives segments from memtable spills (~8MB each).
 * When a level reaches segments_per_level segments, they are merged into
 * a single segment at the next level. This provides exponentially larger
 * segments at higher levels while bounding write amplification.
 */
typedef struct TpIndexMetaPageData
{
	uint32 magic;			/* Magic number for validation */
	uint32 version;			/* Index format version */
	Oid	   text_config_oid; /* Text search configuration OID */
	/*
	 * Corpus size for BM25 scoring.  Satisfies the exact invariant
	 *
	 *     total_docs = Σ segment.num_docs
	 *
	 * where the sum is over all on-disk segments reachable from
	 * level_heads[] and num_docs is the segment header field (fixed
	 * at segment creation).  VACUUM keeps the invariant by
	 * decrementing total_docs whenever a segment's num_docs changes
	 * or the segment leaves the chain: pre-V5 rebuild shrinks
	 * num_docs from old to docs_added; a V5 segment dropped because
	 * all docs are dead contributes zero.  V5 bitset flips that
	 * leave survivors are invisible here — they shrink alive_count,
	 * not num_docs.
	 *
	 * Per-segment dictionary doc_freq(t) ≤ segment.num_docs by
	 * construction, so
	 *
	 *     total_docs = Σ segment.num_docs
	 *              ≥ Σ segment.doc_freq(t)
	 *              = doc_freq(t) globally
	 *
	 * and BM25's N ≥ df(t) precondition for tp_calculate_idf holds.
	 *
	 * The shared-memory atomic additionally counts unflushed
	 * memtable docs and is the source persisted at every spill;
	 * this disk copy therefore lags between spills but is still in
	 * sync with Σ segment.num_docs at spill/sync boundaries.
	 */
	uint64 total_docs;
	uint64 _unused_total_terms;	  /* Unused, retained for on-disk compat */
	uint64 total_len;			  /* Σ segment.total_len, same frame as
									 total_docs */
	float4		k1;				  /* BM25 k1 parameter */
	float4		b;				  /* BM25 b parameter */
	BlockNumber root_blkno;		  /* Root page of the index tree */
	BlockNumber term_stats_root;  /* Root page of term statistics B-tree */
	BlockNumber first_docid_page; /* First page of docid chain for crash
								   * recovery */

	/* Hierarchical segment storage (LSM-style) */
	BlockNumber level_heads[TP_MAX_LEVELS]; /* Head of segment chain per level
											 */
	uint16 level_counts[TP_MAX_LEVELS];		/* Segment count per level */
} TpIndexMetaPageData;

typedef TpIndexMetaPageData *TpIndexMetaPage;

/*
 * Document ID page structure for crash recovery
 * Stores ItemPointerData (ctid) entries for documents in the index.
 * Magic and version constants are defined in constants.h.
 */
typedef struct TpDocidPageHeader
{
	uint32		magic;		/* TP_DOCID_PAGE_MAGIC */
	uint32		version;	/* TP_DOCID_PAGE_VERSION */
	uint32		num_docids; /* Number of docids stored on this page */
	BlockNumber next_page;	/* Next page in chain, or InvalidBlockNumber */
} TpDocidPageHeader;

/*
 * Metapage operations
 */
extern void			   tp_init_metapage(Page page, Oid text_config_oid);
extern TpIndexMetaPage tp_get_metapage(Relation index);

/*
 * Persist the shared-memory atomic into the metapage.  Called at
 * every spill site; caller must hold LW_EXCLUSIVE on the per-index
 * lock.
 */
extern void
tp_sync_metapage_stats(Relation index, TpLocalIndexState *index_state);
/*
 * Document ID operations for crash recovery
 */
extern void tp_add_docid_to_pages(Relation index, ItemPointer ctid);
extern void tp_clear_docid_pages(Relation index);
extern void tp_invalidate_docid_cache(void);
