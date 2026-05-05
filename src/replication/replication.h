/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * replication.h - Custom WAL resource manager for pg_textsearch
 *
 * pg_textsearch's memtable lives in DSA shared memory and is
 * not directly WAL-logged by Postgres. This rmgr emits three
 * record types so that physical-replication standbys (and primary
 * crash recovery) can apply memtable mutations and segment merges
 * atomically against backend scans:
 *
 *   INSERT_TERMS    — appends a tokenized term list (v2 bm25vector
 *                     payload) for one document's worth of
 *                     postings. Carries: index_oid, ctid, vector
 *                     bytes.
 *   SPILL           — signals that the primary spilled the
 *                     memtable to a segment. Standby redo applies
 *                     the L0 chain-link metapage update and clears
 *                     the in-memory memtable atomically under the
 *                     per-index LW_EXCLUSIVE.
 *   MERGE_LINKAGE   — signals that the primary merged level-N
 *                     segments into level-(N+1). Standby redo
 *                     applies the metapage unlink/link, optional
 *                     new-segment next_segment splice, and atomic
 *                     corpus-stat shrinkage all under the per-index
 *                     LW_EXCLUSIVE so concurrent backend scans
 *                     (which hold LW_SHARED) can't race the unlink.
 *
 * TP_RMGR_ID = 149 is registered on the PostgreSQL custom rmgr
 * registry: https://wiki.postgresql.org/wiki/CustomWALResourceManagers
 */
#ifndef PG_TEXTSEARCH_REPLICATION_H
#define PG_TEXTSEARCH_REPLICATION_H

#include <postgres.h>

#include <access/xlogreader.h>
#include <storage/itemptr.h>
#include <utils/relcache.h>

#include "index/state.h"
#include "types/vector.h"

#define TP_RMGR_ID	 149
#define TP_RMGR_NAME "pg_textsearch"

/* Record info bits (XLOG_TP_*) — distinguish the record types. */
#define XLOG_TP_INSERT_TERMS  0x10
#define XLOG_TP_SPILL		  0x20
#define XLOG_TP_MERGE_LINKAGE 0x30

/*
 * INSERT_TERMS payload (followed by `vector_size` bytes of v2
 * bm25vector wire data — see types/vector.h for the layout).
 */
typedef struct xl_tp_insert_terms
{
	Oid				index_oid;
	ItemPointerData ctid;
	uint32			vector_size; /* bytes of v2 bm25vector following */
} xl_tp_insert_terms;

/*
 * SPILL payload.
 *
 * SPILL carries the L0 chain-link metapage update so the standby
 * applies it atomically with the memtable clear, all under the
 * per-index LW_EXCLUSIVE held by tp_redo_apply_spill. The previous
 * approach of using a separate GenericXLog record for the chain
 * link left a window on the standby (between chain-link replay and
 * SPILL replay) where the new segment was queryable while the
 * memtable still held the same docs — backends could see each
 * spilled doc twice.
 *
 * Block 0 references the metapage; block 1 references the new
 * segment's header page (only present when prev_chain_head is
 * valid, i.e., when the new segment links to an existing chain).
 */
typedef struct xl_tp_spill
{
	Oid			index_oid;
	BlockNumber new_segment_root; /* L0 head after the spill */
	BlockNumber prev_chain_head;  /* prior level_heads[0]; Invalid
								   * if the new segment is the
								   * first in level 0 */
} xl_tp_spill;

/*
 * MERGE_LINKAGE payload.
 *
 * Emitted at the end of a level-N → level-(N+1) merge to (a)
 * unlink the merged source segments from level N, (b) link the
 * new merged segment as the head of level N+1, (c) optionally
 * splice the old level-(N+1) head onto the new segment's
 * next_segment, and (d) shrink the metap corpus stats and the
 * per-index atomics by however many docs/tokens the merge
 * dropped (V5 alive-bitset deads).
 *
 * Reason for being a custom record: standby backends scanning
 * the index hold LW_SHARED on the per-index lock and walk a
 * snapshot of the segment chain. If the linkage replayed via
 * GenericXLog (which only takes buffer locks), the standby's
 * scan could be mid-walk on the OLD source-segment chain when
 * redo unlinks them and the FSM later recycles their pages —
 * the next_segment pointer they're following becomes stale and
 * the walk hits zeroed/recycled blocks. Custom redo takes
 * LW_EXCLUSIVE on the per-index lock so it blocks until any
 * in-flight scan releases LW_SHARED, then applies all four
 * mutations atomically.
 *
 * Block 0 references the metapage; block 1 references the new
 * segment's header page (only present when the prior level+1
 * head is valid, i.e. the new segment is being spliced onto an
 * existing level+1 chain).
 */
typedef struct xl_tp_merge_linkage
{
	Oid	   index_oid;
	uint32 level;				  /* source level being drained */
	uint32 segment_count;		  /* how many were merged */
	uint32 total_at_level;		  /* prior level_counts[level]
								   * (pre-merge); the new value is
								   * total_at_level - segment_count */
	BlockNumber remainder_head;	  /* new level_heads[level] */
	BlockNumber new_segment;	  /* new level_heads[level+1] */
	BlockNumber prev_target_head; /* prior level_heads[level+1];
								   * InvalidBlockNumber if the new
								   * segment is the first at
								   * level+1 (block 1 then absent) */
	uint64 docs_shrinkage;		  /* docs the merge dropped */
	uint64 tokens_shrinkage;	  /* tokens the merge dropped */
} xl_tp_merge_linkage;

/* Registration — called once from _PG_init. */
extern void tp_register_rmgr(void);

/* Rmgr callbacks. */
extern void		   tp_rmgr_redo(XLogReaderState *record);
extern void		   tp_rmgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *tp_rmgr_identify(uint8 info);

/*
 * Emission helpers — called from the primary's insert/spill/merge
 * paths. Return the LSN of the emitted record. The caller MUST
 * already hold the per-index LW_EXCLUSIVE; the helpers manage
 * their own buffer locks and critical section internally
 * (acquiring buffers outside the CS so ReadBuffer ERROR doesn't
 * escalate to PANIC).
 */
extern XLogRecPtr
tp_xlog_insert_terms(Oid index_oid, ItemPointer ctid, const TpVector *vec);
/*
 * Emit a SPILL record AND apply the L0 chain-link metapage update
 * in the same WAL action. Replaces the tp_link_l0_chain_head
 * call that the spill recipes used to emit separately via
 * GenericXLog.
 */
extern XLogRecPtr tp_xlog_spill(Relation index, BlockNumber new_segment_root);
/*
 * Emit a MERGE_LINKAGE record AND apply the metap unlink/link +
 * optional new-segment next_segment splice + atomic shrinkage in
 * the same WAL action. Replaces the GenericXLog block at the tail
 * of tp_merge_level_segments. Closes the standby cache staleness
 * window (see #349).
 */
extern XLogRecPtr tp_xlog_merge_linkage(
		Relation		   index,
		uint32			   level,
		uint32			   segment_count,
		uint32			   total_at_level,
		BlockNumber		   remainder_head,
		BlockNumber		   new_segment,
		uint64			   docs_shrinkage,
		uint64			   tokens_shrinkage,
		TpLocalIndexState *index_state);

#endif /* PG_TEXTSEARCH_REPLICATION_H */
