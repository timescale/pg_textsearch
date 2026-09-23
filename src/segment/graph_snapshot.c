/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * graph_snapshot.c - Atomic segment-root and memtable read snapshots
 */
#include <postgres.h>

#include <common/int.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/memutils.h>
#include <utils/timestamp.h>

#include "index/metapage.h"
#include "memtable/chain_walker.h"
#include "segment/graph_snapshot.h"
#include "segment/io.h"
#include "segment/segment.h"

int tp_debug_segment_graph_snapshot_pause_before_lock_ms   = 0;
int tp_debug_segment_graph_snapshot_pause_before_unlock_ms = 0;
int tp_debug_segment_graph_snapshot_pause_ms			   = 0;

static void
tp_debug_segment_graph_snapshot_pause(
		Relation index, int pause_ms, const char *phase)
{
	TimestampTz deadline;

	if (pause_ms <= 0)
		return;

	ereport(LOG,
			(errmsg("pg_textsearch segment graph snapshot pause at %s for "
					"index %u backend %d",
					phase,
					RelationGetRelid(index),
					MyProcPid)));
	deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), pause_ms);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (GetCurrentTimestamp() >= deadline)
			break;
		pg_usleep(10000L);
	}
	ereport(LOG,
			(errmsg("pg_textsearch segment graph snapshot resume after %s for "
					"index %u backend %d",
					phase,
					RelationGetRelid(index),
					MyProcPid)));
}

static void
tp_read_memtable_snapshot_candidate(
		Relation index, BlockNumber *head, BlockNumber *tail)
{
	Buffer buffer;
	Page   page;

	buffer = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page  = BufferGetPage(buffer);
	*head = tp_metapage_read_memtable_head(page);
	*tail = tp_metapage_read_memtable_tail(page);
	UnlockReleaseBuffer(buffer);
}

/*
 * Writers extend in tail -> new page -> metapage order.  Snapshot retry paths
 * must therefore release metapage before tail, matching the reverse of their
 * tail -> metapage acquisition.
 */
static void
tp_release_snapshot_buffers(Buffer buffer, Buffer tail_buffer)
{
	UnlockReleaseBuffer(buffer);
	if (BufferIsValid(tail_buffer))
		UnlockReleaseBuffer(tail_buffer);
}

static void
tp_capture_segment_roots(Relation index, TpSegmentGraphSnapshot *snapshot)
{
	uint32 expected_roots = 0;

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (pg_add_u32_overflow(
					expected_roots,
					(uint32)snapshot->metapage.level_counts[level],
					&expected_roots))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("BM25 segment-root count overflow")));
	}
	if (expected_roots > MaxAllocSize / sizeof(BlockNumber))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BM25 segment-root snapshot is too large")));
	if (expected_roots > 0)
		snapshot->roots = palloc_array(BlockNumber, expected_roots);

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = snapshot->metapage.level_heads[level];

		snapshot->level_offsets[level] = snapshot->root_count;
		for (uint32 i = 0; i < (uint32)snapshot->metapage.level_counts[level];
			 i++)
		{
			BlockNumber next;

			if (!BlockNumberIsValid(current))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("BM25 segment chain at level %u is shorter "
								"than its recorded count",
								level)));
			if (!tp_segment_read_next(index, current, &next))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("could not read BM25 segment root %u at "
								"level %u",
								current,
								level)));
			snapshot->roots[snapshot->root_count++] = current;
			current									= next;
		}
		if (BlockNumberIsValid(current))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("BM25 segment chain at level %u is longer than "
							"its recorded count or contains a cycle",
							level)));
	}
	snapshot->level_offsets[TP_MAX_LEVELS] = snapshot->root_count;
	Assert(snapshot->root_count == expected_roots);
}

static TpSegmentGraphSnapshot *
tp_segment_graph_snapshot_create_internal(Relation index, bool capture_roots)
{
	if (!RelationIsValid(index))
		elog(ERROR, "invalid relation passed to segment graph snapshot");

	if (capture_roots)
		tp_debug_segment_graph_snapshot_pause(
				index,
				tp_debug_segment_graph_snapshot_pause_before_lock_ms,
				"before-lock");

	for (;;)
	{
		TpSegmentGraphSnapshot *snapshot;
		TpIndexMetaPage			metap;
		Buffer					buffer;
		Buffer					tail_buffer = InvalidBuffer;
		BlockNumber				candidate_head;
		BlockNumber				candidate_tail;

		CHECK_FOR_INTERRUPTS();

		tp_read_memtable_snapshot_candidate(
				index, &candidate_head, &candidate_tail);
		if (BlockNumberIsValid(candidate_head) !=
			BlockNumberIsValid(candidate_tail))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("BM25 memtable head/tail validity mismatch")));

		if (BlockNumberIsValid(candidate_tail))
		{
			/*
			 * Never retain the candidate metapage lock while acquiring the
			 * tail: an extending writer already holds this tail EXCLUSIVE
			 * before it requests the metapage EXCLUSIVE.
			 */
			tail_buffer = ReadBuffer(index, candidate_tail);
			LockBuffer(tail_buffer, BUFFER_LOCK_SHARE);
		}

		buffer = ReadBuffer(index, TP_METAPAGE_BLKNO);
		if (!BufferIsValid(buffer))
			elog(ERROR,
				 "failed to read metapage buffer for BM25 index \"%s\"",
				 RelationGetRelationName(index));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		metap = tp_metapage_copy_from_page(index, BufferGetPage(buffer));

		if (metap->memtable_head_blkno != candidate_head ||
			metap->memtable_tail_blkno != candidate_tail)
		{
			pfree(metap);
			tp_release_snapshot_buffers(buffer, tail_buffer);
			continue;
		}

		snapshot		   = palloc0(sizeof(TpSegmentGraphSnapshot));
		snapshot->metapage = *metap;
		pfree(metap);

		tp_memtable_chain_snapshot_capture_locked(
				tail_buffer,
				snapshot->metapage.memtable_head_blkno,
				snapshot->metapage.memtable_tail_blkno,
				&snapshot->memtable);
		if (capture_roots)
			tp_capture_segment_roots(index, snapshot);

		if (capture_roots)
			tp_debug_segment_graph_snapshot_pause(
					index,
					tp_debug_segment_graph_snapshot_pause_before_unlock_ms,
					"before-unlock");
		UnlockReleaseBuffer(buffer);
		if (BufferIsValid(tail_buffer))
			UnlockReleaseBuffer(tail_buffer);
		if (capture_roots)
			tp_debug_segment_graph_snapshot_pause(
					index,
					tp_debug_segment_graph_snapshot_pause_ms,
					"after-unlock");

		return snapshot;
	}
}

TpSegmentGraphSnapshot *
tp_segment_graph_snapshot_create(Relation index)
{
	return tp_segment_graph_snapshot_create_internal(index, true);
}

TpSegmentGraphSnapshot *
tp_segment_graph_snapshot_create_metadata(Relation index)
{
	return tp_segment_graph_snapshot_create_internal(index, false);
}

const BlockNumber *
tp_segment_graph_snapshot_level(
		const TpSegmentGraphSnapshot *snapshot, uint32 level, uint32 *count)
{
	uint32 offset;

	Assert(snapshot != NULL);
	Assert(count != NULL);
	if (level >= TP_MAX_LEVELS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid BM25 segment level %u", level)));

	offset = snapshot->level_offsets[level];
	*count = snapshot->level_offsets[level + 1] - offset;
	if (*count == 0)
		return NULL;
	return snapshot->roots + offset;
}

void
tp_segment_graph_snapshot_free(TpSegmentGraphSnapshot *snapshot)
{
	if (snapshot == NULL)
		return;
	if (snapshot->roots != NULL)
		pfree(snapshot->roots);
	pfree(snapshot);
}
