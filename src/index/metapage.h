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
	 * Running count of docs inserted into this index incarnation.
	 * Upper-bounds the per-segment sum:
	 *
	 *     total_docs ≥ Σ segment.num_docs
	 *
	 * over all on-disk segments reachable from level_heads[], with
	 * equality right after CREATE INDEX / REINDEX.  Slack appears
	 * when VACUUM rebuilds a pre-V5 segment into a V5 segment
	 * containing only alive docs: Σ segment.num_docs drops but
	 * total_docs is never decremented.  (V5 segments aren't
	 * rebuilt — VACUUM only flips alive bits, leaving num_docs
	 * fixed.  The live shared-memory atomic additionally counts
	 * memtable docs; this disk copy lags between spills.)
	 *
	 * Per-segment dictionary doc_freq values likewise stay at
	 * their segment-creation values; doc_freq(t) ≤ num_docs holds
	 * within every segment by construction.  Chained together:
	 *
	 *     total_docs ≥ Σ segment.num_docs
	 *              ≥ Σ segment.doc_freq(t)
	 *              = doc_freq(t) globally
	 *
	 * so BM25's N ≥ df(t) invariant — which tp_calculate_idf
	 * relies on (violating it yields negative IDF) — is preserved.
	 * REINDEX rebuilds from the heap and resets total_docs to the
	 * live count.
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
