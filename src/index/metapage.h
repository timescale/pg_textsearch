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
	 * Unflushed memtable docs are NOT counted here; the on-disk
	 * memtable chain (issue #374) is the in-flight buffer and
	 * scoring composes its corpus totals from this metapage value
	 * plus a chain-source open at read time.
	 */
	uint64 total_docs;
	uint64 _unused_total_terms;		/* Unused, retained for on-disk compat */
	uint64 total_len;				/* Σ segment.total_len, same frame as
									   total_docs */
	float4		k1;					/* BM25 k1 parameter */
	float4		b;					/* BM25 b parameter */
	BlockNumber root_blkno;			/* Root page of the index tree */
	BlockNumber term_stats_root;	/* Root page of term statistics B-tree */
	BlockNumber _unused_docid_page; /* Reserved: was first_docid_page,
									 * retired by issue #374 (the
									 * on-disk memtable obsoletes
									 * docid pages).  Kept to preserve
									 * metapage offsets; always
									 * InvalidBlockNumber. */

	/* Hierarchical segment storage (LSM-style) */
	BlockNumber level_heads[TP_MAX_LEVELS]; /* Head of segment chain per level
											 */
	uint16 level_counts[TP_MAX_LEVELS];		/* Segment count per level */

	/*
	 * On-disk memtable (issue #374): head/tail of the page
	 * chain.  Both fields hold InvalidBlockNumber when the
	 * chain is empty; tail_blkno may differ from head_blkno
	 * once the chain has more than one page.  All mutations
	 * to these fields go through GenericXLog atomic with the
	 * corresponding chain-page mutation.  Introduced in
	 * TP_METAPAGE_VERSION 7.
	 */
	BlockNumber memtable_head_blkno;
	BlockNumber memtable_tail_blkno;
} TpIndexMetaPageData;

typedef TpIndexMetaPageData *TpIndexMetaPage;

/*
 * V6 metapage size (bytes), used by:
 *   - tp_metapage_upgrade_to_current() when bumping pd_lower on
 *     in-place upgrade.
 *   - Tests / debug code that downgrades a v7 page to v6 for
 *     compat-path exercise.
 *
 * v6 had no memtable_head_blkno / memtable_tail_blkno fields;
 * everything else is byte-identical.  Use offsetof() so we never
 * drift from the real prefix layout of the live struct.
 */
#define TP_INDEX_METAPAGE_DATA_SIZE_V6 \
	offsetof(TpIndexMetaPageData, memtable_head_blkno)

/*
 * Pin the historical v6 prefix size at 108 bytes.  v6 was frozen
 * by the v1.2.x releases; any future struct edit that changes the
 * layout of bytes [0, 108) would silently break v6 read compat
 * (issue #383).  v7 only appends new fields after this prefix.
 */
StaticAssertDecl(
		TP_INDEX_METAPAGE_DATA_SIZE_V6 == 108,
		"v6 metapage prefix layout changed - "
		"backward-compat is broken");

/*
 * Metapage operations
 */
extern void			   tp_init_metapage(Page page, Oid text_config_oid);
extern TpIndexMetaPage tp_get_metapage(Relation index);

/*
 * Read the on-disk memtable chain head/tail from a buffer page,
 * tolerating v6 metapages (where the fields do not exist on disk
 * and the underlying bytes are zero — which would otherwise be
 * misinterpreted as block 0, i.e. the metapage itself, rather
 * than InvalidBlockNumber).
 *
 * Caller must hold at least BUFFER_LOCK_SHARE on the metapage
 * buffer.  These helpers are for direct-buffer reader sites that
 * bypass tp_get_metapage() (which already normalizes its palloc'd
 * copy).  See src/memtable/log.c and src/memtable/chain_source.c.
 */
extern BlockNumber tp_metapage_read_memtable_head(Page page);
extern BlockNumber tp_metapage_read_memtable_tail(Page page);

/*
 * In-place v6 -> v7 metapage upgrade (issue #383).  Must be
 * called on a GenericXLogRegisterBuffer-returned page copy under
 * BUFFER_LOCK_EXCLUSIVE, BEFORE setting any v7-specific field on
 * the page.  On a v7 page this is a no-op.  On a v6 page the
 * helper:
 *   - initializes memtable_head_blkno = memtable_tail_blkno =
 *     InvalidBlockNumber,
 *   - clears any stale _unused_docid_page pointer (and emits a
 *     LOG-level forensic message if it was non-Invalid; see the
 *     rationale block in tp_metapage_upgrade_to_current()),
 *   - bumps the version field to TP_METAPAGE_VERSION,
 *   - bumps pd_lower to cover the new fields so GenericXLog
 *     records them in the page image.
 *
 * The upgrade piggybacks on the surrounding GenericXLog record;
 * there is no extra WAL traffic.  Concurrent writers cannot race
 * the upgrade — the EXCLUSIVE buffer lock serializes them, and a
 * later writer simply observes v7 and the helper is a no-op.
 *
 * The Relation is only used to format the orphan-pointer LOG
 * message with the index name; the function does not otherwise
 * touch it.
 */
extern void tp_metapage_upgrade_to_current(Relation index, Page page);
