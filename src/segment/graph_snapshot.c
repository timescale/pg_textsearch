/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * graph_snapshot.c - Atomic segment-root graph snapshots
 */
#include <postgres.h>

#include <common/int.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/memutils.h>
#include <utils/timestamp.h>

#include "index/metapage.h"
#include "segment/graph_snapshot.h"
#include "segment/io.h"
#include "segment/segment.h"

int tp_debug_segment_graph_snapshot_pause_ms = 0;

static void
tp_debug_segment_graph_snapshot_pause(Relation index)
{
	TimestampTz deadline;

	if (tp_debug_segment_graph_snapshot_pause_ms <= 0)
		return;

	ereport(LOG,
			(errmsg("pg_textsearch segment graph snapshot pause for index %u "
					"backend %d",
					RelationGetRelid(index),
					MyProcPid)));
	deadline = TimestampTzPlusMilliseconds(
			GetCurrentTimestamp(), tp_debug_segment_graph_snapshot_pause_ms);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (GetCurrentTimestamp() >= deadline)
			break;
		pg_usleep(10000L);
	}
	ereport(LOG,
			(errmsg("pg_textsearch segment graph snapshot resume for index %u "
					"backend %d",
					RelationGetRelid(index),
					MyProcPid)));
}

TpSegmentGraphSnapshot *
tp_segment_graph_snapshot_create(Relation index)
{
	TpSegmentGraphSnapshot *snapshot;
	TpIndexMetaPage			metap;
	Buffer					buffer;
	Page					page;
	uint32					expected_roots = 0;

	if (!RelationIsValid(index))
		elog(ERROR,
			 "invalid relation passed to tp_segment_graph_snapshot_create");

	buffer = ReadBuffer(index, TP_METAPAGE_BLKNO);
	if (!BufferIsValid(buffer))
		elog(ERROR,
			 "failed to read metapage buffer for BM25 index \"%s\"",
			 RelationGetRelationName(index));

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page			   = BufferGetPage(buffer);
	metap			   = tp_metapage_copy_from_page(index, page);
	snapshot		   = palloc0(sizeof(TpSegmentGraphSnapshot));
	snapshot->metapage = *metap;
	pfree(metap);

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

	UnlockReleaseBuffer(buffer);
	tp_debug_segment_graph_snapshot_pause(index);

	return snapshot;
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
