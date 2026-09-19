/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/transam.h>
#include <access/xact.h>
#include <access/xlog.h>
#include <catalog/pg_am_d.h>
#include <common/int.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <storage/lmgr.h>
#include <storage/lock.h>
#include <storage/lwlock.h>
#include <utils/hsearch.h>
#include <utils/timestamp.h>

#include "access/am.h"
#include "constants.h"
#include "index/metapage.h"
#include "index/state.h"
#include "segment/alive_bitset.h"
#include "segment/compaction.h"
#include "segment/format.h"
#include "segment/graph_snapshot.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/pagemapper.h"
#include "segment/tombstone.h"

#define TP_COMPACTION_MAINTENANCE_LOCK_SUBID 3
#define TP_COMPACTION_PUBLICATION_LOCK_SUBID 4

typedef struct TpSegmentEstimate
{
	uint64 bytes;
	uint64 docs;
	uint64 terms;
	uint64 string_bytes;
	uint64 postings;
	uint64 skip_entries;
	uint64 pages;
} TpSegmentEstimate;

typedef struct TpCompactionSource
{
	BlockNumber		  root;
	uint32			  source_level;
	uint32			  chain_position;
	bool			  has_dead_docs;
	uint64			  total_tokens;
	TpSegmentEstimate estimate;
} TpCompactionSource;

typedef struct TpCompactionBatch
{
	uint32			  first_source;
	uint32			  source_count;
	uint32			  output_level;
	bool			  uncombinable;
	TpSegmentEstimate estimate;
} TpCompactionBatch;

typedef struct TpCompactionPlan
{
	TpCompactionSource *sources;
	uint32				num_sources;
	uint32				source_capacity;
	TpCompactionBatch  *batches;
	uint32				num_batches;
	uint32				output_capacity;
	uint16				prefix_counts[TP_MAX_LEVELS];
	BlockNumber			selected_heads[TP_MAX_LEVELS];
	uint16				selected_counts[TP_MAX_LEVELS];
	BlockNumber			retained_heads[TP_MAX_LEVELS];
	uint16				retained_counts[TP_MAX_LEVELS];
} TpCompactionPlan;

typedef struct TpCompactionOutput
{
	BlockNumber				 output_heads[TP_MAX_LEVELS];
	uint16					 output_counts[TP_MAX_LEVELS];
	BlockNumber				*owned_output_roots;
	uint32					 owned_output_count;
	uint32					 owned_output_capacity;
	uint64					 removed_docs;
	uint64					 removed_tokens;
	TpDetachedTombstoneBatch tombstones;
	bool					 publication_started;
} TpCompactionOutput;

typedef struct TpCompactionPublication
{
	TpIndexMetaPageData metapage;
	BlockNumber			predecessor;
	uint32				predecessor_level;
	BlockNumber			l0_head;
	uint16				l0_count;
} TpCompactionPublication;

typedef enum TpStatsRebasePolicy
{
	TP_STATS_REBASE_STRICT,
	TP_STATS_REBASE_CLAMP_LEGACY_VACUUM
} TpStatsRebasePolicy;

static bool tp_debug_compaction_build_active = false;

static void tp_free_compaction_plan(TpCompactionPlan *plan);

static void
tp_debug_compaction_pause(int pause_ms, const char *phase, Oid index_oid)
{
	TimestampTz deadline;

	if (pause_ms <= 0)
		return;

	ereport(LOG,
			(errmsg("pg_textsearch compaction pause at %s for index %u "
					"backend %d",
					phase,
					index_oid,
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
			(errmsg("pg_textsearch compaction resume after %s for index %u "
					"backend %d",
					phase,
					index_oid,
					MyProcPid)));
}

void
tp_debug_compaction_allocation_pause(
		Relation index, TpCompactionAllocationPause phase)
{
	const char *phase_name;

	if (!tp_debug_compaction_build_active ||
		tp_debug_compaction_pause_after_allocation != (int)phase)
		return;

	switch (phase)
	{
	case TP_COMPACTION_ALLOCATION_PAUSE_OUTPUT_DATA:
		phase_name = "output-data";
		break;
	case TP_COMPACTION_ALLOCATION_PAUSE_PAGE_INDEX:
		phase_name = "page-index";
		break;
	case TP_COMPACTION_ALLOCATION_PAUSE_TOMBSTONE:
		phase_name = "tombstone";
		break;
	case TP_COMPACTION_ALLOCATION_PAUSE_NONE:
	default:
		return;
	}

	tp_debug_compaction_pause_after_allocation =
			TP_COMPACTION_ALLOCATION_PAUSE_NONE;
	tp_debug_compaction_pause(60000, phase_name, RelationGetRelid(index));
}

void
tp_compaction_lock(Relation index)
{
	/*
	 * Use pg_am's otherwise-unused object-subid space, matching managed
	 * compaction's private lock namespace but with a distinct discriminator.
	 * A relation ShareUpdateExclusiveLock cannot be used here because the
	 * pre-commit dispatcher intentionally holds that mode while signaling a
	 * worker that may compact before the writer commits.
	 */
	LockDatabaseObject(
			AccessMethodRelationId,
			RelationGetRelid(index),
			TP_COMPACTION_MAINTENANCE_LOCK_SUBID,
			ExclusiveLock);
}

void
tp_compaction_unlock(Relation index)
{
	UnlockDatabaseObject(
			AccessMethodRelationId,
			RelationGetRelid(index),
			TP_COMPACTION_MAINTENANCE_LOCK_SUBID,
			ExclusiveLock);
}

void
tp_compaction_publication_lock(Relation index, LOCKMODE mode)
{
	Assert(mode == ShareLock || mode == ExclusiveLock);
	LockDatabaseObject(
			AccessMethodRelationId,
			RelationGetRelid(index),
			TP_COMPACTION_PUBLICATION_LOCK_SUBID,
			mode);
}

void
tp_compaction_publication_unlock(Relation index, LOCKMODE mode)
{
	Assert(mode == ShareLock || mode == ExclusiveLock);
	UnlockDatabaseObject(
			AccessMethodRelationId,
			RelationGetRelid(index),
			TP_COMPACTION_PUBLICATION_LOCK_SUBID,
			mode);
}

static bool
tp_compaction_maintenance_lock_held(Relation index)
{
	LOCKTAG tag;

	SET_LOCKTAG_OBJECT(
			tag,
			MyDatabaseId,
			AccessMethodRelationId,
			RelationGetRelid(index),
			TP_COMPACTION_MAINTENANCE_LOCK_SUBID);
	return LockHeldByMe(&tag, ExclusiveLock, true);
}

static bool
tp_compaction_is_private(TpLocalIndexState *index_state, Relation index)
{
	if (index_state == NULL || index_state->shared == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("compaction requires a valid per-index state")));

	if (index_state->lock_held)
	{
		if (index_state->lock_mode != LW_EXCLUSIVE ||
			!LWLockHeldByMeInMode(&index_state->shared->lock, LW_EXCLUSIVE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("private compaction requires the per-index "
							"exclusive lock")));
		return true;
	}

	if (!tp_compaction_maintenance_lock_held(index))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("runtime compaction requires the per-index "
						"maintenance lock")));

	return false;
}

uint64
tp_max_segment_size_bytes(void)
{
	return (uint64)tp_max_segment_size_mb * 1024 * 1024;
}

static bool
tp_u64_add(uint64 left, uint64 right, uint64 *result)
{
	return !pg_add_u64_overflow(left, right, result);
}

static bool
tp_u64_multiply(uint64 left, uint64 right, uint64 *result)
{
	return !pg_mul_u64_overflow(left, right, result);
}

static bool
tp_u64_round_up_divide(uint64 value, uint64 divisor, uint64 *result)
{
	uint64 rounded;

	Assert(divisor > 0);
	if (value == 0)
	{
		*result = 0;
		return true;
	}

	if (!tp_u64_add(value, divisor - 1, &rounded))
		return false;

	*result = rounded / divisor;
	return true;
}

/*
 * Calculate the conservative current-format bound.  Arithmetic overflow is
 * distinct from a representational limit: source overflow is corrupt
 * metadata, while an otherwise valid oversized source remains uncombinable.
 */
static bool
tp_estimate_physical_bytes(TpSegmentEstimate *estimate, bool *representable)
{
	uint64 bytes = sizeof(TpSegmentHeader);
	uint64 contribution;
	uint64 data_pages;
	uint64 entries_per_index_page;
	uint64 index_pages;
	uint64 total_pages;

	*representable = true;

	if (!tp_u64_add(bytes, sizeof(uint32), &bytes) ||
		!tp_u64_multiply(estimate->terms, sizeof(uint32), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes) ||
		!tp_u64_add(bytes, estimate->string_bytes, &bytes) ||
		!tp_u64_multiply(
				estimate->terms, sizeof(TpDictEntry), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes) ||
		!tp_u64_multiply(
				estimate->postings, sizeof(TpBlockPosting), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes) ||
		!tp_u64_multiply(
				estimate->skip_entries, sizeof(TpSkipEntry), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes) ||
		!tp_u64_multiply(estimate->docs, sizeof(uint8), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes) ||
		!tp_u64_multiply(estimate->docs, sizeof(BlockNumber), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes) ||
		!tp_u64_multiply(
				estimate->docs, sizeof(OffsetNumber), &contribution) ||
		!tp_u64_add(bytes, contribution, &bytes))
		return false;

	if (!tp_document_count_fits(estimate->docs))
		*representable = false;
	else if (!tp_u64_add(
					 bytes,
					 tp_alive_bitset_size((uint32)estimate->docs),
					 &bytes))
		return false;

	if (!tp_u64_round_up_divide(bytes, SEGMENT_DATA_PER_PAGE, &data_pages))
		return false;

	entries_per_index_page = (BLCKSZ - SizeOfPageHeaderData -
							  MAXALIGN(sizeof(TpPageIndexSpecial))) /
							 sizeof(BlockNumber);
	Assert(entries_per_index_page > 0);

	if (!tp_u64_round_up_divide(
				data_pages, entries_per_index_page, &index_pages) ||
		!tp_u64_add(data_pages, index_pages, &total_pages) ||
		!tp_u64_multiply(total_pages, BLCKSZ, &estimate->bytes))
		return false;

	if (!tp_document_count_fits(estimate->docs) ||
		estimate->terms > TP_MAX_DICTIONARY_TERMS ||
		estimate->string_bytes > TP_MAX_STRING_POOL_BYTES ||
		estimate->skip_entries > TP_MAX_GROWABLE_CAPACITY ||
		data_pages > PG_UINT32_MAX || total_pages >= InvalidBlockNumber)
		*representable = false;

	return true;
}

static void
tp_collect_source_estimate(
		TpCompactionSource *source,
		TpSegmentReader	   *reader,
		bool				pause_source_estimate,
		Oid					index_oid)
{
	TpSegmentHeader *header = reader->header;
	bool			 representable;

	if (header->entries_offset < header->strings_offset)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("segment at block %u has reversed string offsets",
						source->root)));

	source->estimate.docs		  = header->num_docs;
	source->estimate.terms		  = header->num_terms;
	source->estimate.string_bytes = header->entries_offset -
									header->strings_offset;
	source->estimate.pages = header->num_pages;

	for (uint32 term = 0; term < header->num_terms; term++)
	{
		TpDictEntry entry;
		uint64		term_skip_entries;

		tp_segment_read_dict_entry(reader, header, term, &entry);
		if (!tp_u64_add(
					source->estimate.postings,
					(uint64)entry.doc_freq,
					&source->estimate.postings))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment posting count overflow at block %u",
							source->root)));
		if (!tp_u64_round_up_divide(
					(uint64)entry.doc_freq,
					TP_BLOCK_SIZE,
					&term_skip_entries) ||
			!tp_u64_add(
					source->estimate.skip_entries,
					term_skip_entries,
					&source->estimate.skip_entries))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment skip entry count overflow at block %u",
							source->root)));
	}

	if (pause_source_estimate)
		tp_debug_compaction_pause(
				tp_debug_compaction_pause_source_estimate_ms,
				"source-estimate",
				index_oid);

	if (!tp_estimate_physical_bytes(&source->estimate, &representable))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("segment size estimate overflow at block %u",
						source->root)));

	if (!representable)
		source->estimate.bytes = PG_UINT64_MAX;
}

static BlockNumber
tp_collect_source(
		Relation		  index,
		TpCompactionPlan *plan,
		BlockNumber		  root,
		uint32			  level,
		uint32			  chain_position)
{
	TpCompactionSource *source;
	TpSegmentReader	   *reader;
	BlockNumber			next;

	if (!BlockNumberIsValid(root))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("segment chain for level %u ended before its "
						"recorded count",
						level)));
	if (plan->num_sources >= plan->source_capacity)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("compaction source plan exceeded its capacity")));

	source				   = &plan->sources[plan->num_sources++];
	source->root		   = root;
	source->source_level   = level;
	source->chain_position = chain_position;

	reader = tp_segment_open(index, root);
	if (reader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not open segment at block %u", root)));

	if (reader->header->level != level)
	{
		uint32 recorded_level = reader->header->level;

		tp_segment_close(reader);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("segment at block %u records level %u but is "
						"linked from level %u",
						root,
						recorded_level,
						level)));
	}

	tp_collect_source_estimate(
			source,
			reader,
			plan->num_sources == 1 &&
					tp_compaction_maintenance_lock_held(index),
			RelationGetRelid(index));
	source->has_dead_docs = reader->header->alive_bitset_offset > 0 &&
							reader->header->alive_count <
									reader->header->num_docs;
	source->total_tokens = reader->header->total_tokens;
	next				 = reader->header->next_segment;
	tp_segment_close(reader);
	return next;
}

static bool
tp_estimate_add(
		const TpSegmentEstimate *left,
		const TpSegmentEstimate *right,
		uint64					 budget,
		TpSegmentEstimate		*result)
{
	bool representable;

	memset(result, 0, sizeof(*result));
	if (!tp_u64_add(left->docs, right->docs, &result->docs) ||
		!tp_u64_add(left->terms, right->terms, &result->terms) ||
		!tp_u64_add(
				left->string_bytes,
				right->string_bytes,
				&result->string_bytes) ||
		!tp_u64_add(left->postings, right->postings, &result->postings) ||
		!tp_u64_add(
				left->skip_entries,
				right->skip_entries,
				&result->skip_entries) ||
		!tp_u64_add(left->pages, right->pages, &result->pages) ||
		!tp_estimate_physical_bytes(result, &representable))
		return false;

	return representable && result->bytes <= budget;
}

static bool
tp_metapage_segment_graph_matches(Page page, const TpIndexMetaPage snapshot)
{
	TpIndexMetaPage current = (TpIndexMetaPage)PageGetContents(page);

	return current->magic == snapshot->magic &&
		   current->text_config_oid == snapshot->text_config_oid &&
		   current->total_docs == snapshot->total_docs &&
		   current->_unused_total_terms == snapshot->_unused_total_terms &&
		   current->total_len == snapshot->total_len &&
		   current->k1 == snapshot->k1 && current->b == snapshot->b &&
		   current->root_blkno == snapshot->root_blkno &&
		   current->term_stats_root == snapshot->term_stats_root &&
		   current->_unused_docid_page == snapshot->_unused_docid_page &&
		   memcmp(current->level_heads,
				  snapshot->level_heads,
				  sizeof(current->level_heads)) == 0 &&
		   memcmp(current->level_counts,
				  snapshot->level_counts,
				  sizeof(current->level_counts)) == 0 &&
		   (current->version < TP_METAPAGE_VERSION
					? snapshot->capabilities == 0
					: current->capabilities == snapshot->capabilities);
}

static void
tp_collect_force_sources(
		Relation index, const TpIndexMetaPage snapshot, TpCompactionPlan *plan)
{
	uint32 total_sources = 0;

	memset(plan, 0, sizeof(*plan));
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (pg_add_u32_overflow(
					total_sources,
					(uint32)snapshot->level_counts[level],
					&total_sources))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment count overflow in index \"%s\"",
							RelationGetRelationName(index))));
		plan->selected_heads[level]	 = snapshot->level_heads[level];
		plan->selected_counts[level] = snapshot->level_counts[level];
		plan->retained_heads[level]	 = InvalidBlockNumber;
	}

	if (total_sources == 0)
		return;

	plan->source_capacity = total_sources;
	plan->sources = palloc0(sizeof(TpCompactionSource) * total_sources);

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = snapshot->level_heads[level];

		for (uint32 position = 0; position < snapshot->level_counts[level];
			 position++)
			current = tp_collect_source(index, plan, current, level, position);

		if (BlockNumberIsValid(current))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment chain for level %u exceeds its "
							"recorded count",
							level)));
	}

	Assert(plan->num_sources == total_sources);
	plan->num_sources = total_sources;
}

static uint32
tp_size_class(uint64 bytes)
{
	uint64 limit = TP_BASE_LEVEL_SIZE_BYTES;

	for (uint32 level = 0; level < TP_MAX_LEVELS - 1; level++)
	{
		if (bytes <= limit)
			return level;
		if (limit > PG_UINT64_MAX / (uint64)tp_segments_per_level)
			return TP_MAX_LEVELS - 1;
		limit *= (uint64)tp_segments_per_level;
	}
	return TP_MAX_LEVELS - 1;
}

static void
tp_append_bounded_batches(
		TpCompactionPlan *plan, uint32 first_source, uint32 source_count)
{
	uint64 budget = tp_max_segment_size_bytes();
	uint32 source_index;
	uint32 source_end;

	if (source_count == 0)
		return;
	if (first_source > plan->num_sources ||
		source_count > plan->num_sources - first_source)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid compaction source range")));

	source_index = first_source;
	source_end	 = first_source + source_count;
	while (source_index < source_end)
	{
		TpCompactionBatch *batch;

		if (plan->num_batches >= plan->source_capacity)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("compaction batch plan exceeded its capacity")));
		batch = &plan->batches[plan->num_batches++];

		batch->first_source = source_index;
		batch->source_count = 1;
		batch->estimate		= plan->sources[source_index].estimate;
		batch->uncombinable = batch->estimate.bytes > budget;
		source_index++;

		while (!batch->uncombinable && source_index < source_end)
		{
			TpSegmentEstimate combined;

			if (!tp_estimate_add(
						&batch->estimate,
						&plan->sources[source_index].estimate,
						budget,
						&combined))
				break;

			batch->estimate = combined;
			batch->source_count++;
			source_index++;
		}
	}
}

static void
tp_build_force_batches(TpCompactionPlan *plan)
{
	uint32 *order;
	uint16	assigned_counts[TP_MAX_LEVELS];

	if (plan->num_sources == 0)
		return;

	plan->output_capacity = PG_UINT16_MAX;
	plan->batches = palloc0(sizeof(TpCompactionBatch) * plan->num_sources);
	tp_append_bounded_batches(plan, 0, plan->num_sources);

	memcpy(assigned_counts, plan->retained_counts, sizeof(assigned_counts));
	order = palloc(sizeof(uint32) * plan->num_batches);
	for (uint32 i = 0; i < plan->num_batches; i++)
		order[i] = i;

	/* Stable insertion sort: equal-size batches retain source order. */
	for (uint32 i = 1; i < plan->num_batches; i++)
	{
		uint32 batch_index = order[i];
		uint32 position	   = i;

		while (position > 0 &&
			   plan->batches[order[position - 1]].estimate.bytes <
					   plan->batches[batch_index].estimate.bytes)
		{
			order[position] = order[position - 1];
			position--;
		}
		order[position] = batch_index;
	}

	for (uint32 i = 0; i < plan->num_batches; i++)
	{
		TpCompactionBatch *batch	 = &plan->batches[order[i]];
		uint32			   preferred = tp_size_class(batch->estimate.bytes);
		uint32			   selected	 = TP_MAX_LEVELS;

		for (uint32 level = preferred; level < TP_MAX_LEVELS; level++)
		{
			if (assigned_counts[level] < PG_UINT16_MAX)
			{
				selected = level;
				break;
			}
		}

		if (selected == TP_MAX_LEVELS)
		{
			for (int level = (int)preferred - 1; level >= 0; level--)
			{
				if (assigned_counts[level] < PG_UINT16_MAX)
				{
					selected = (uint32)level;
					break;
				}
			}
		}

		if (selected == TP_MAX_LEVELS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bounded compaction cannot place %u output "
							"segments",
							plan->num_batches)));

		batch->output_level = selected;
		assigned_counts[selected]++;
	}

	pfree(order);
}

static bool
tp_plan_chains_match(Relation index, TpCompactionPlan *plan)
{
	uint32 source_index = 0;

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = InvalidBlockNumber;
		bool		has_selected_sources;

		while (source_index < plan->num_sources &&
			   plan->sources[source_index].source_level < level)
			source_index++;

		has_selected_sources = source_index < plan->num_sources &&
							   plan->sources[source_index].source_level ==
									   level;
		if (has_selected_sources)
			current = plan->sources[source_index].root;

		while (source_index < plan->num_sources &&
			   plan->sources[source_index].source_level == level)
		{
			TpSegmentReader *reader;

			if (plan->sources[source_index].root != current)
				return false;

			reader = tp_segment_open(index, current);
			if (reader == NULL)
				return false;
			current = reader->header->next_segment;
			tp_segment_close(reader);
			source_index++;
		}

		if (has_selected_sources && current != plan->retained_heads[level])
			return false;
	}

	return source_index == plan->num_sources;
}

static bool
tp_plan_is_noop(
		Relation index, const TpIndexMetaPage snapshot, TpCompactionPlan *plan)
{
	Buffer buf;
	Page   page;
	bool   matches;

	if (plan->num_batches != plan->num_sources)
		return false;

	for (uint32 i = 0; i < plan->num_batches; i++)
	{
		if (plan->batches[i].source_count != 1 ||
			plan->batches[i].output_level != plan->sources[i].source_level)
			return false;
	}

	/*
	 * A structural singleton rewrite is still useful when its alive bitmap
	 * contains dead documents: merge rewrites only survivors and corrects
	 * corpus statistics.  In particular, an all-zero singleton disappears.
	 */
	for (uint32 i = 0; i < plan->num_sources; i++)
	{
		if (plan->sources[i].has_dead_docs)
			return false;
	}

	buf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page	= BufferGetPage(buf);
	matches = tp_metapage_segment_graph_matches(page, snapshot);
	UnlockReleaseBuffer(buf);
	return matches && tp_plan_chains_match(index, plan);
}

static void
tp_initialize_compaction_output(
		const TpCompactionPlan *plan, TpCompactionOutput *output)
{
	memset(output, 0, sizeof(*output));
	memcpy(output->output_heads,
		   plan->retained_heads,
		   sizeof(output->output_heads));
	if (plan->num_batches > 0)
		output->owned_output_roots = palloc(
				sizeof(BlockNumber) * plan->num_batches);
	output->owned_output_capacity = plan->num_batches;
	output->tombstones.head		  = InvalidBlockNumber;
	output->tombstones.tail		  = InvalidBlockNumber;
}

static void
tp_discard_compaction_output(Relation index, TpCompactionOutput *output)
{
	TpDetachedTombstoneBatch tombstones = output->tombstones;

	output->tombstones.head			   = InvalidBlockNumber;
	output->tombstones.tail			   = InvalidBlockNumber;
	output->tombstones.container_pages = 0;
	output->tombstones.owned_pages	   = NULL;
	output->tombstones.owned_count	   = 0;
	output->tombstones.owned_capacity  = 0;

	for (uint32 i = 0; i < output->owned_output_count; i++)
		tp_discard_unpublished_segment(index, output->owned_output_roots[i]);
	if (output->owned_output_roots != NULL)
		pfree(output->owned_output_roots);
	output->owned_output_roots	  = NULL;
	output->owned_output_count	  = 0;
	output->owned_output_capacity = 0;

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		output->output_heads[level]	 = InvalidBlockNumber;
		output->output_counts[level] = 0;
	}

	if (tombstones.owned_pages != NULL)
		tp_tombstone_discard_detached(index, tombstones);
}

static bool
tp_segment_page_link(Page page, uint32 *level, BlockNumber *next)
{
	char  *contents = PageGetContents(page);
	uint32 magic;
	uint32 version;

	memcpy(&magic, contents, sizeof(magic));
	memcpy(&version, contents + sizeof(magic), sizeof(version));
	if (magic != TP_SEGMENT_MAGIC)
		return false;

	if (version <= TP_SEGMENT_FORMAT_VERSION_3)
	{
		TpSegmentHeaderV3 *header = (TpSegmentHeaderV3 *)contents;

		*level = header->level;
		*next  = header->next_segment;
	}
	else if (version <= TP_SEGMENT_FORMAT_VERSION_4)
	{
		TpSegmentHeaderV4 *header = (TpSegmentHeaderV4 *)contents;

		*level = header->level;
		*next  = header->next_segment;
	}
	else if (version <= TP_SEGMENT_FORMAT_VERSION)
	{
		TpSegmentHeader *header = (TpSegmentHeader *)contents;

		*level = header->level;
		*next  = header->next_segment;
	}
	else
		return false;

	return true;
}

static bool
tp_read_segment_link(
		Relation	 index,
		BlockNumber	 root,
		uint32		 expected_level,
		BlockNumber *next)
{
	Buffer		buf;
	Page		page;
	uint32		level;
	BlockNumber next_segment;
	bool		decoded;

	if (!BlockNumberIsValid(root))
		return false;

	buf = ReadBuffer(index, root);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page	= BufferGetPage(buf);
	decoded = tp_segment_page_link(page, &level, &next_segment);
	UnlockReleaseBuffer(buf);

	if (!decoded || level != expected_level)
		return false;
	*next = next_segment;
	return true;
}

static void
tp_segment_page_set_link(Page page, BlockNumber next)
{
	char  *contents = PageGetContents(page);
	uint32 version	= ((TpSegmentHeader *)contents)->version;

	if (version <= TP_SEGMENT_FORMAT_VERSION_3)
		((TpSegmentHeaderV3 *)contents)->next_segment = next;
	else
		((TpSegmentHeader *)contents)->next_segment = next;
}

static bool
tp_metapage_identity_matches(
		const TpIndexMetaPageData *current,
		const TpIndexMetaPageData *snapshot)
{
	if (current->version != TP_METAPAGE_VERSION &&
		current->version != TP_METAPAGE_VERSION_V8 &&
		current->version != TP_METAPAGE_VERSION_V7 &&
		current->version != TP_METAPAGE_VERSION_V6)
		return false;

	return current->magic == snapshot->magic &&
		   current->text_config_oid == snapshot->text_config_oid &&
		   current->_unused_total_terms == snapshot->_unused_total_terms &&
		   current->k1 == snapshot->k1 && current->b == snapshot->b &&
		   current->root_blkno == snapshot->root_blkno &&
		   current->term_stats_root == snapshot->term_stats_root &&
		   current->_unused_docid_page == snapshot->_unused_docid_page;
}

static BlockNumber
tp_metapage_pending_free_head(const TpIndexMetaPage metap)
{
	return metap->version < TP_METAPAGE_VERSION_V8 ? InvalidBlockNumber
												   : metap->pending_free_head;
}

static bool
tp_validate_selected_runs(
		Relation			  index,
		const TpIndexMetaPage snapshot,
		TpIndexMetaPage		  current_meta,
		TpCompactionPlan	 *plan,
		BlockNumber			 *predecessor,
		uint32				 *predecessor_level)
{
	HASHCTL ctl;
	HTAB   *visited;
	uint32	source_index = 0;
	bool	valid		 = false;

	*predecessor	   = InvalidBlockNumber;
	*predecessor_level = TP_MAX_LEVELS;
	if (!tp_metapage_identity_matches(current_meta, snapshot))
		return false;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize	  = sizeof(BlockNumber);
	ctl.entrysize = sizeof(BlockNumber);
	visited		  = hash_create(
			  "compaction validation roots",
			  Max((long)plan->num_sources, 16L),
			  &ctl,
			  HASH_ELEM | HASH_BLOBS);

	for (uint32 i = 0; i < plan->num_sources; i++)
	{
		bool found;

		(void)hash_search(visited, &plan->sources[i].root, HASH_ENTER, &found);
		if (found)
			goto done;
	}

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current;
		BlockNumber level_predecessor = InvalidBlockNumber;

		if ((uint32)plan->prefix_counts[level] +
					(uint32)plan->selected_counts[level] +
					(uint32)plan->retained_counts[level] !=
			(uint32)snapshot->level_counts[level])
			goto done;

		if (level == 0)
		{
			uint32 prefix_count;

			if (current_meta->level_counts[level] <
				snapshot->level_counts[level])
				goto done;
			prefix_count = (uint32)current_meta->level_counts[level] -
						   (uint32)snapshot->level_counts[level];
			current = current_meta->level_heads[level];
			for (uint32 i = 0; i < prefix_count; i++)
			{
				BlockNumber next;
				bool		found;

				if (current == snapshot->level_heads[level])
					goto done;
				(void)hash_search(visited, &current, HASH_ENTER, &found);
				if (found)
					goto done;
				if (!tp_read_segment_link(index, current, level, &next))
					goto done;
				level_predecessor = current;
				current			  = next;
			}
			if (current != snapshot->level_heads[level])
				goto done;
		}
		else
		{
			if (current_meta->level_counts[level] !=
						snapshot->level_counts[level] ||
				current_meta->level_heads[level] !=
						snapshot->level_heads[level])
				goto done;
			current = current_meta->level_heads[level];
		}

		if (plan->selected_counts[level] == 0)
		{
			if (plan->prefix_counts[level] != 0 ||
				plan->selected_heads[level] != InvalidBlockNumber)
				goto done;
			continue;
		}

		for (uint16 i = 0; i < plan->prefix_counts[level]; i++)
		{
			BlockNumber next;
			bool		found;

			if (!BlockNumberIsValid(current))
				goto done;
			(void)hash_search(visited, &current, HASH_ENTER, &found);
			if (found || !tp_read_segment_link(index, current, level, &next))
				goto done;
			level_predecessor = current;
			current			  = next;
		}

		if (plan->selected_heads[level] != current)
			goto done;
		if (BlockNumberIsValid(level_predecessor))
		{
			if (BlockNumberIsValid(*predecessor))
				goto done;
			*predecessor	   = level_predecessor;
			*predecessor_level = level;
		}

		for (uint16 i = 0; i < plan->selected_counts[level]; i++)
		{
			BlockNumber next;

			if (source_index >= plan->num_sources ||
				plan->sources[source_index].source_level != level ||
				plan->sources[source_index].root != current ||
				!tp_read_segment_link(index, current, level, &next))
				goto done;
			current = next;
			source_index++;
		}
		if (current != plan->retained_heads[level])
			goto done;
		if (BlockNumberIsValid(current))
		{
			bool found;

			(void)hash_search(visited, &current, HASH_FIND, &found);
			if (found)
				goto done;
		}
	}

	valid = source_index == plan->num_sources;

done:
	hash_destroy(visited);
	return valid;
}

static bool
tp_publication_identity_matches(
		const TpIndexMetaPage		   current,
		const TpCompactionPublication *publication)
{
	if (!tp_metapage_identity_matches(current, &publication->metapage))
		return false;

	if (current->level_heads[0] != publication->l0_head ||
		current->level_counts[0] != publication->l0_count)
		return false;

	for (uint32 level = 1; level < TP_MAX_LEVELS; level++)
	{
		if (current->level_heads[level] !=
					publication->metapage.level_heads[level] ||
			current->level_counts[level] !=
					publication->metapage.level_counts[level])
			return false;
	}

	return true;
}

static void
tp_prepare_compaction_publication(
		TpLocalIndexState		*index_state,
		Relation				 index,
		const TpIndexMetaPage	 snapshot,
		TpCompactionPlan		*plan,
		TpCompactionPublication *publication)
{
	volatile bool	acquired_here = false;
	TpIndexMetaPage current_meta;
	BlockNumber		predecessor;
	uint32			predecessor_level;
	bool			valid;

	PG_TRY();
	{
		if (!index_state->lock_held)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			acquired_here = true;
		}
		else if (
				index_state->lock_mode != LW_EXCLUSIVE ||
				!LWLockHeldByMeInMode(
						&index_state->shared->lock, LW_EXCLUSIVE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("private compaction preparation requires the "
							"per-index exclusive lock")));

		current_meta = tp_get_metapage(index);
		valid		 = tp_validate_selected_runs(
				   index,
				   snapshot,
				   current_meta,
				   plan,
				   &predecessor,
				   &predecessor_level);
		if (valid)
		{
			memcpy(&publication->metapage,
				   current_meta,
				   sizeof(publication->metapage));
			publication->predecessor	   = predecessor;
			publication->predecessor_level = predecessor_level;
			publication->l0_head		   = current_meta->level_heads[0];
			publication->l0_count		   = current_meta->level_counts[0];
		}
		pfree(current_meta);

		if (acquired_here)
		{
			tp_release_index_lock(index_state);
			acquired_here = false;
		}
	}
	PG_FINALLY();
	{
		if (acquired_here && index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	if (!valid)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("compaction graph changed before publication "
						"preparation for index \"%s\"",
						RelationGetRelationName(index))));
}

static bool
tp_validate_compaction_output(
		Relation index, TpCompactionPlan *plan, TpCompactionOutput *output)
{
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = output->output_heads[level];

		for (uint16 i = 0; i < output->output_counts[level]; i++)
		{
			BlockNumber next;

			if (!tp_read_segment_link(index, current, level, &next))
				return false;
			current = next;
		}
		if (current != plan->retained_heads[level])
			return false;
	}

	if (output->tombstones.container_pages == 0)
		return output->tombstones.head == InvalidBlockNumber &&
			   output->tombstones.tail == InvalidBlockNumber;

	{
		BlockNumber current = output->tombstones.head;

		for (uint32 i = 0; i < output->tombstones.container_pages; i++)
		{
			Buffer			buf;
			Page			page;
			TpTombstonePage tombstone;
			BlockNumber		next;

			if (!BlockNumberIsValid(current))
				return false;
			buf = ReadBuffer(index, current);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!tp_tombstone_page_is_valid(page))
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
			tombstone = tp_tombstone_page(page);
			next	  = tombstone->next_page;
			UnlockReleaseBuffer(buf);

			if (i + 1 == output->tombstones.container_pages)
			{
				if (current != output->tombstones.tail ||
					next != InvalidBlockNumber)
					return false;
			}
			current = next;
		}
		return current == InvalidBlockNumber;
	}
}

static void
tp_build_compaction_output(
		Relation			  index,
		const TpIndexMetaPage snapshot,
		TpCompactionPlan	 *plan,
		TpCompactionOutput	 *output)
{
	uint64		 selected_docs	 = 0;
	uint64		 selected_tokens = 0;
	uint64		 output_docs	 = 0;
	uint64		 output_tokens	 = 0;
	BlockNumber *displaced_pages = NULL;
	uint32		 displaced_count = 0;

	tp_initialize_compaction_output(plan, output);
	tp_debug_compaction_build_active = true;
	PG_TRY();
	{
		for (uint32 i = 0; i < plan->num_sources; i++)
		{
			if (!tp_u64_add(
						selected_docs,
						plan->sources[i].estimate.docs,
						&selected_docs) ||
				!tp_u64_add(
						selected_tokens,
						plan->sources[i].total_tokens,
						&selected_tokens))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("source segment statistics overflow")));
		}

		for (uint32 reverse = plan->num_batches; reverse > 0; reverse--)
		{
			TpCompactionBatch	 *batch = &plan->batches[reverse - 1];
			BlockNumber			 *roots;
			TpMergedSegmentResult result;
			uint32				  output_level = batch->output_level;

			roots = palloc(sizeof(BlockNumber) * batch->source_count);
			for (uint32 i = 0; i < batch->source_count; i++)
				roots[i] = plan->sources[batch->first_source + i].root;

			if (tp_merge_segment_batch(
						index,
						roots,
						batch->source_count,
						output_level,
						output->output_heads[output_level],
						&result))
			{
				if (output->owned_output_count >=
					output->owned_output_capacity)
				{
					tp_discard_unpublished_segment(index, result.root);
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("compaction output ownership overflow")));
				}
				output->owned_output_roots[output->owned_output_count++] =
						result.root;
				if (output->output_counts[output_level] == PG_UINT16_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("compaction output count overflow at "
									"level %u",
									output_level)));
				output->output_heads[output_level] = result.root;
				output->output_counts[output_level]++;
				if ((uint32)plan->retained_counts[output_level] +
							(uint32)output->output_counts[output_level] >
					plan->output_capacity)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("compaction output exceeded level %u "
									"capacity",
									output_level)));
				if (!tp_u64_add(
							output_docs,
							(uint64)result.num_docs,
							&output_docs) ||
					!tp_u64_add(
							output_tokens,
							result.total_tokens,
							&output_tokens))
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("compaction output statistics "
									"overflow")));
			}

			pfree(roots);
		}

		if (output_docs > selected_docs || output_tokens > selected_tokens)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("compaction output statistics exceed source "
							"statistics")));

		output->removed_docs   = selected_docs - output_docs;
		output->removed_tokens = selected_tokens - output_tokens;
		if (snapshot->total_docs < output->removed_docs ||
			snapshot->total_len < output->removed_tokens)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("compaction source statistics exceed index "
							"statistics")));

		for (uint32 i = 0; i < plan->num_sources; i++)
		{
			BlockNumber *pages;
			uint32		 num_pages;
			uint32		 new_count;

			num_pages = tp_segment_collect_pages(
					index, plan->sources[i].root, &pages);
			if (num_pages > UINT32_MAX - displaced_count)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("compaction displaced-page count overflow")));
			new_count = displaced_count + num_pages;
			if (num_pages > 0)
			{
				if (displaced_pages == NULL)
					displaced_pages = palloc(sizeof(BlockNumber) * new_count);
				else
					displaced_pages = repalloc(
							displaced_pages, sizeof(BlockNumber) * new_count);
				memcpy(displaced_pages + displaced_count,
					   pages,
					   sizeof(BlockNumber) * num_pages);
			}
			displaced_count = new_count;
			if (pages != NULL)
				pfree(pages);
		}
		tp_tombstone_build_detached(
				index,
				displaced_pages,
				displaced_count,
				InvalidFullTransactionId,
				&output->tombstones);
		if (displaced_pages != NULL)
		{
			pfree(displaced_pages);
			displaced_pages = NULL;
		}

		/*
		 * Output and detached tombstone pages have their own WAL records,
		 * but remain unreachable.  A backend crash before publication can
		 * leak an incomplete allocation until REINDEX; handled errors
		 * discard every complete object recorded in output.
		 */
	}
	PG_CATCH();
	{
		tp_debug_compaction_build_active = false;
		tp_discard_compaction_output(index, output);
		PG_RE_THROW();
	}
	PG_END_TRY();
	tp_debug_compaction_build_active = false;
}

static bool
tp_publish_compaction_output(
		TpLocalIndexState			  *index_state,
		Relation					   index,
		TpCompactionPlan			  *plan,
		TpCompactionOutput			  *output,
		const TpCompactionPublication *publication,
		TpStatsRebasePolicy			   stats_policy)
{
	volatile Buffer metabuf						 = InvalidBuffer;
	volatile Buffer predecessor_buf				 = InvalidBuffer;
	volatile Buffer tailbuf						 = InvalidBuffer;
	GenericXLogState *volatile publication_state = NULL;
	volatile bool acquired_here					 = false;
	BlockNumber	  predecessor					 = publication->predecessor;
	uint32		  predecessor_level = publication->predecessor_level;
	bool		  published			= false;

	PG_TRY();
	{
		Page			current_page;
		TpIndexMetaPage current_meta;
		uint16			current_counts[TP_MAX_LEVELS];
		uint64			current_docs;
		uint64			current_tokens;
		BlockNumber		current_pending;
		XLogRecPtr		publication_lsn;
		Page			meta_copy;
		TpIndexMetaPage meta;

		if (!index_state->lock_held)
		{
			tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
			acquired_here = true;
		}
		else if (
				index_state->lock_mode != LW_EXCLUSIVE ||
				!LWLockHeldByMeInMode(
						&index_state->shared->lock, LW_EXCLUSIVE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("compaction publication requires the per-index "
							"exclusive lock")));

		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		current_page = BufferGetPage(metabuf);
		current_meta = (TpIndexMetaPage)PageGetContents(current_page);
		if (!tp_publication_identity_matches(current_meta, publication))
		{
			UnlockReleaseBuffer(metabuf);
			metabuf = InvalidBuffer;
			if (acquired_here)
			{
				tp_release_index_lock(index_state);
				acquired_here = false;
			}
			goto publication_done;
		}

		memcpy(current_counts,
			   current_meta->level_counts,
			   sizeof(current_counts));
		current_docs	= current_meta->total_docs;
		current_tokens	= current_meta->total_len;
		current_pending = tp_metapage_pending_free_head(current_meta);
		if (stats_policy != TP_STATS_REBASE_STRICT &&
			stats_policy != TP_STATS_REBASE_CLAMP_LEGACY_VACUUM)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("invalid compaction statistic rebase policy")));

		for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
		{
			uint32 new_count;

			if (current_counts[level] < plan->selected_counts[level])
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("compaction source count changed at level "
								"%u",
								level)));
			new_count = (uint32)current_counts[level] -
						(uint32)plan->selected_counts[level] +
						(uint32)output->output_counts[level];
			if (new_count > plan->output_capacity)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("bm25 segment count limit reached at "
								"level "
								"%u",
								level)));
		}
		if (stats_policy == TP_STATS_REBASE_STRICT &&
			(current_docs < output->removed_docs ||
			 current_tokens < output->removed_tokens))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("compaction shrinkage exceeds current index "
							"statistics")));

		if (BlockNumberIsValid(predecessor))
		{
			Page		predecessor_page;
			uint32		recorded_level;
			BlockNumber recorded_next;

			if (predecessor_level >= TP_MAX_LEVELS)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("invalid compaction predecessor level")));

			predecessor_buf = ReadBuffer(index, predecessor);
			LockBuffer(predecessor_buf, BUFFER_LOCK_EXCLUSIVE);
			predecessor_page = BufferGetPage(predecessor_buf);
			if (!tp_segment_page_link(
						predecessor_page, &recorded_level, &recorded_next) ||
				recorded_level != predecessor_level ||
				recorded_next != plan->selected_heads[predecessor_level])
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("compaction predecessor changed before "
								"publication")));
		}

		if (tp_debug_panic_before_compaction_publish)
			elog(PANIC,
				 "pg_textsearch: debug crash before compaction "
				 "publication");

		/*
		 * Every output and detached tombstone page has already been
		 * WAL-logged.  WAL insertion order places this reachability record
		 * after those page records, without a relation-wide buffer flush.
		 */
		publication_state = GenericXLogStart(index);
		meta_copy		  = GenericXLogRegisterBuffer(
				(GenericXLogState *)publication_state, metabuf, 0);

		if (BufferIsValid(predecessor_buf))
		{
			Page predecessor_copy;

			predecessor_copy = GenericXLogRegisterBuffer(
					(GenericXLogState *)publication_state, predecessor_buf, 0);
			((PageHeader)predecessor_copy)->pd_lower = BLCKSZ;
			tp_segment_page_set_link(
					predecessor_copy, output->output_heads[predecessor_level]);
		}

		tailbuf = tp_tombstone_attach_detached(
				(GenericXLogState *)publication_state,
				index,
				output->tombstones,
				current_pending);
		tp_metapage_upgrade_to_current(index, meta_copy);
		meta = (TpIndexMetaPage)PageGetContents(meta_copy);

		for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
		{
			bool level_changes = plan->selected_counts[level] > 0 ||
								 output->output_counts[level] > 0;

			if (!level_changes)
				continue;
			if (!BlockNumberIsValid(predecessor) || level != predecessor_level)
				meta->level_heads[level] = output->output_heads[level];
			meta->level_counts[level] =
					(uint16)((uint32)current_counts[level] -
							 (uint32)plan->selected_counts[level] +
							 (uint32)output->output_counts[level]);
		}
		if (output->tombstones.container_pages > 0)
			meta->pending_free_head = output->tombstones.head;
		if (stats_policy == TP_STATS_REBASE_CLAMP_LEGACY_VACUUM)
		{
			meta->total_docs = current_docs >= output->removed_docs
									 ? current_docs - output->removed_docs
									 : 0;
			meta->total_len	 = current_tokens >= output->removed_tokens
									 ? current_tokens - output->removed_tokens
									 : 0;
		}
		else
		{
			meta->total_docs = current_docs - output->removed_docs;
			meta->total_len	 = current_tokens - output->removed_tokens;
		}

		publication_lsn = GenericXLogFinish(
				(GenericXLogState *)publication_state);
		publication_state			= NULL;
		output->publication_started = true;
		if (tp_debug_panic_after_compaction_publish)
		{
			if (RelationNeedsWAL(index))
				XLogFlush(publication_lsn);
			elog(PANIC,
				 "pg_textsearch: debug crash after compaction "
				 "publication");
		}
		if (BufferIsValid(tailbuf))
		{
			UnlockReleaseBuffer(tailbuf);
			tailbuf = InvalidBuffer;
		}
		if (BufferIsValid(predecessor_buf))
		{
			UnlockReleaseBuffer(predecessor_buf);
			predecessor_buf = InvalidBuffer;
		}
		UnlockReleaseBuffer(metabuf);
		metabuf = InvalidBuffer;
		if (acquired_here)
		{
			tp_release_index_lock(index_state);
			acquired_here = false;
		}

		for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
		{
			output->output_heads[level]	 = InvalidBlockNumber;
			output->output_counts[level] = 0;
		}
		output->tombstones.head			   = InvalidBlockNumber;
		output->tombstones.tail			   = InvalidBlockNumber;
		output->tombstones.container_pages = 0;
		if (output->tombstones.owned_pages != NULL)
			pfree(output->tombstones.owned_pages);
		output->tombstones.owned_pages	  = NULL;
		output->tombstones.owned_count	  = 0;
		output->tombstones.owned_capacity = 0;
		if (output->owned_output_roots != NULL)
			pfree(output->owned_output_roots);
		output->owned_output_roots	  = NULL;
		output->owned_output_count	  = 0;
		output->owned_output_capacity = 0;
		published					  = true;

	publication_done:
		Assert(!published || output->publication_started);
	}
	PG_CATCH();
	{
		if (!output->publication_started)
		{
			if (publication_state != NULL)
				GenericXLogAbort((GenericXLogState *)publication_state);
			if (BufferIsValid(tailbuf))
			{
				if (InterruptHoldoffCount == 0)
					HOLD_INTERRUPTS();
				UnlockReleaseBuffer(tailbuf);
			}
			if (BufferIsValid(predecessor_buf))
			{
				if (InterruptHoldoffCount == 0)
					HOLD_INTERRUPTS();
				UnlockReleaseBuffer(predecessor_buf);
			}
			if (BufferIsValid(metabuf))
			{
				if (InterruptHoldoffCount == 0)
					HOLD_INTERRUPTS();
				UnlockReleaseBuffer(metabuf);
			}
			if (acquired_here && index_state->lock_held)
				tp_release_index_lock(index_state);
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	return published;
}

static void
tp_publish_detached_tombstones(
		TpLocalIndexState		 *index_state,
		Relation				  index,
		TpDetachedTombstoneBatch *batch)
{
	volatile Buffer metabuf						 = InvalidBuffer;
	volatile Buffer tailbuf						 = InvalidBuffer;
	GenericXLogState *volatile publication_state = NULL;
	volatile bool acquired_here					 = false;
	volatile bool published						 = false;

	PG_TRY();
	{
		Page			current_page;
		TpIndexMetaPage current_meta;
		BlockNumber		current_pending;
		Page			meta_copy;
		TpIndexMetaPage meta;

		if (!index_state->lock_held)
		{
			tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
			acquired_here = true;
		}
		else if (
				index_state->lock_mode != LW_EXCLUSIVE ||
				!LWLockHeldByMeInMode(
						&index_state->shared->lock, LW_EXCLUSIVE))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("tombstone publication requires the per-index "
							"exclusive lock")));

		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		current_page	= BufferGetPage(metabuf);
		current_meta	= (TpIndexMetaPage)PageGetContents(current_page);
		current_pending = tp_metapage_pending_free_head(current_meta);

		publication_state = GenericXLogStart(index);
		meta_copy		  = GenericXLogRegisterBuffer(
				(GenericXLogState *)publication_state, metabuf, 0);
		tailbuf = tp_tombstone_attach_detached(
				(GenericXLogState *)publication_state,
				index,
				*batch,
				current_pending);
		tp_metapage_upgrade_to_current(index, meta_copy);
		meta					= (TpIndexMetaPage)PageGetContents(meta_copy);
		meta->pending_free_head = batch->head;

		GenericXLogFinish((GenericXLogState *)publication_state);
		publication_state = NULL;
		published		  = true;

		if (BufferIsValid(tailbuf))
		{
			UnlockReleaseBuffer(tailbuf);
			tailbuf = InvalidBuffer;
		}
		UnlockReleaseBuffer(metabuf);
		metabuf = InvalidBuffer;
		if (acquired_here)
		{
			tp_release_index_lock(index_state);
			acquired_here = false;
		}

		pfree(batch->owned_pages);
		batch->head			   = InvalidBlockNumber;
		batch->tail			   = InvalidBlockNumber;
		batch->container_pages = 0;
		batch->owned_pages	   = NULL;
		batch->owned_count	   = 0;
		batch->owned_capacity  = 0;
	}
	PG_CATCH();
	{
		if (!published && publication_state != NULL)
			GenericXLogAbort((GenericXLogState *)publication_state);
		if (BufferIsValid(tailbuf))
		{
			if (InterruptHoldoffCount == 0)
				HOLD_INTERRUPTS();
			UnlockReleaseBuffer(tailbuf);
		}
		if (BufferIsValid(metabuf))
		{
			if (InterruptHoldoffCount == 0)
				HOLD_INTERRUPTS();
			UnlockReleaseBuffer(metabuf);
		}
		if (acquired_here && index_state->lock_held)
			tp_release_index_lock(index_state);
		if (published)
		{
			if (batch->owned_pages != NULL)
				pfree(batch->owned_pages);
			batch->head			   = InvalidBlockNumber;
			batch->tail			   = InvalidBlockNumber;
			batch->container_pages = 0;
			batch->owned_pages	   = NULL;
			batch->owned_count	   = 0;
			batch->owned_capacity  = 0;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
tp_complete_compaction_publication(
		TpLocalIndexState	 *index_state,
		Relation			  index,
		const TpIndexMetaPage snapshot,
		TpCompactionPlan	 *plan,
		TpCompactionOutput	 *output,
		FullTransactionId	  reclaim_fxid,
		bool				  defer_reclaim,
		TpStatsRebasePolicy	  stats_policy)
{
	FullTransactionId		merged_fxid;
	TpCompactionPublication publication;
	volatile bool			publication_locked = false;

	output->publication_started = false;
	PG_TRY();
	{
		/*
		 * The prepared segment and tombstone chains are unreachable and
		 * immutable.  Validate them completely before any publication lock.
		 */
		if (!tp_validate_compaction_output(index, plan, output))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid prepared compaction output for index "
							"\"%s\"",
							RelationGetRelationName(index))));

		/*
		 * Ordinary compaction assigns and restamps here.  An atomic prepared
		 * replacement arrives with a VACUUM-assigned XID and tombstones
		 * already stamped during construction.  A split replacement
		 * publishes without reclaim work and samples its horizon afterward.
		 */
		if (defer_reclaim)
		{
			Assert(!FullTransactionIdIsValid(reclaim_fxid));
			Assert(output->tombstones.container_pages == 0);
		}
		else if (!FullTransactionIdIsValid(reclaim_fxid))
		{
			merged_fxid = GetCurrentFullTransactionId();
			tp_tombstone_restamp_detached(
					index, output->tombstones, merged_fxid);
			if (!index_state->lock_held)
				tp_debug_compaction_pause(
						tp_debug_compaction_pause_after_restamp_ms,
						"after-restamp",
						RelationGetRelid(index));
		}

		if (!index_state->lock_held)
		{
			/* Stabilize the spill-prepended prefix through publication. */
			tp_compaction_publication_lock(index, ExclusiveLock);
			publication_locked = true;
		}

		tp_prepare_compaction_publication(
				index_state, index, snapshot, plan, &publication);
		if (!index_state->lock_held)
			tp_debug_compaction_pause(
					tp_debug_compaction_pause_before_publish_ms,
					"before-publish",
					RelationGetRelid(index));

		if (!tp_publish_compaction_output(
					index_state,
					index,
					plan,
					output,
					&publication,
					stats_policy))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("compaction graph changed while publication was "
							"serialized for index \"%s\"",
							RelationGetRelationName(index))));

		if (publication_locked)
		{
			tp_compaction_publication_unlock(index, ExclusiveLock);
			publication_locked = false;
		}
	}
	PG_CATCH();
	{
		if (publication_locked)
			tp_compaction_publication_unlock(index, ExclusiveLock);
		if (!output->publication_started)
			tp_discard_compaction_output(index, output);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static TpIndexMetaPage
tp_prepare_single_replacement_plan(
		TpLocalIndexState *index_state,
		Relation		   index,
		uint32			   level,
		BlockNumber		   source_root,
		TpCompactionPlan  *plan)
{
	volatile bool			acquired_here = false;
	TpSegmentGraphSnapshot *graph;
	TpIndexMetaPage			snapshot;
	const BlockNumber	   *roots;
	uint32					root_count;
	uint32					position;
	bool					found = false;
	bool					private_compaction;

	if (level >= TP_MAX_LEVELS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid replacement segment level %u", level)));

	memset(plan, 0, sizeof(*plan));
	private_compaction = tp_compaction_is_private(index_state, index);
	PG_TRY();
	{
		if (!private_compaction)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			acquired_here = true;
		}

		graph = tp_segment_graph_snapshot_create(index);
		if (acquired_here)
		{
			tp_release_index_lock(index_state);
			acquired_here = false;
		}
	}
	PG_FINALLY();
	{
		if (acquired_here && index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	roots = tp_segment_graph_snapshot_level(graph, level, &root_count);
	for (position = 0; position < root_count; position++)
	{
		if (roots[position] == source_root)
		{
			found = true;
			break;
		}
	}
	if (!found)
	{
		tp_segment_graph_snapshot_free(graph);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("prepared replacement source %u is no longer "
						"published at level %u",
						source_root,
						level)));
	}

	snapshot  = palloc(sizeof(*snapshot));
	*snapshot = graph->metapage;

	for (uint32 current_level = 0; current_level < TP_MAX_LEVELS;
		 current_level++)
	{
		plan->selected_heads[current_level] = InvalidBlockNumber;
		plan->retained_heads[current_level] =
				snapshot->level_heads[current_level];
		plan->retained_counts[current_level] =
				snapshot->level_counts[current_level];
	}

	plan->source_capacity			= 1;
	plan->output_capacity			= PG_UINT16_MAX;
	plan->sources					= palloc0(sizeof(TpCompactionSource));
	plan->sources[0].root			= source_root;
	plan->sources[0].source_level	= level;
	plan->sources[0].chain_position = position;
	plan->num_sources				= 1;
	plan->prefix_counts[level]		= (uint16)position;
	plan->selected_heads[level]		= source_root;
	plan->selected_counts[level]	= 1;
	plan->retained_heads[level]		= (position + 1 < root_count)
											? roots[position + 1]
											: InvalidBlockNumber;
	plan->retained_counts[level]	= (uint16)(root_count - position - 1);

	tp_segment_graph_snapshot_free(graph);
	return snapshot;
}

static void
tp_prepare_single_replacement_root(
		Relation	index,
		BlockNumber replacement_root,
		uint32		level,
		BlockNumber next)
{
	volatile Buffer buf				 = InvalidBuffer;
	GenericXLogState *volatile state = NULL;

	if (!BlockNumberIsValid(replacement_root))
		return;

	PG_TRY();
	{
		Page			 page;
		Page			 copy;
		TpSegmentHeader *header;

		buf = ReadBuffer(index, replacement_root);
		LockBuffer((Buffer)buf, BUFFER_LOCK_EXCLUSIVE);
		page   = BufferGetPage((Buffer)buf);
		header = (TpSegmentHeader *)PageGetContents(page);
		if (header->magic != TP_SEGMENT_MAGIC ||
			header->version != TP_SEGMENT_FORMAT_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid prepared replacement segment at "
							"block %u",
							replacement_root)));

		state = GenericXLogStart(index);
		copy  = GenericXLogRegisterBuffer(
				 (GenericXLogState *)state, (Buffer)buf, 0);
		((PageHeader)copy)->pd_lower = BLCKSZ;
		header				 = (TpSegmentHeader *)PageGetContents(copy);
		header->level		 = level;
		header->next_segment = next;

		GenericXLogFinish((GenericXLogState *)state);
		state = NULL;
		UnlockReleaseBuffer((Buffer)buf);
		buf = InvalidBuffer;
	}
	PG_CATCH();
	{
		if (state != NULL)
			GenericXLogAbort((GenericXLogState *)state);
		if (BufferIsValid((Buffer)buf))
		{
			if (InterruptHoldoffCount == 0)
				HOLD_INTERRUPTS();
			UnlockReleaseBuffer((Buffer)buf);
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
}

void
tp_publish_prepared_segment_replacement(
		TpLocalIndexState			   *index_state,
		Relation						index,
		uint32							level,
		BlockNumber						source_root,
		BlockNumber						replacement_root,
		uint64							removed_docs,
		uint64							removed_tokens,
		TpSegmentReplacementReclaimMode reclaim_mode)
{
	TpCompactionPlan   plan;
	TpCompactionOutput output;
	bool			   defer_reclaim = reclaim_mode ==
						 TP_SEGMENT_REPLACEMENT_RECLAIM_AFTER_PUBLICATION;
	FullTransactionId reclaim_fxid = InvalidFullTransactionId;
	TpDetachedTombstoneBatch volatile deferred_tombstones;
	TpIndexMetaPage volatile snapshot				  = NULL;
	BlockNumber *volatile source_pages				  = NULL;
	volatile bool		 completion_started			  = false;
	volatile bool		 deferred_tombstones_attached = false;
	volatile BlockNumber unclaimed_root				  = replacement_root;
	uint32				 source_page_count;

	memset(&plan, 0, sizeof(plan));
	memset(&output, 0, sizeof(output));
	memset((TpDetachedTombstoneBatch *)&deferred_tombstones,
		   0,
		   sizeof(deferred_tombstones));
	output.tombstones.head	 = InvalidBlockNumber;
	output.tombstones.tail	 = InvalidBlockNumber;
	deferred_tombstones.head = InvalidBlockNumber;
	deferred_tombstones.tail = InvalidBlockNumber;

	PG_TRY();
	{
		BlockNumber *pages;

		if (BlockNumberIsValid((BlockNumber)unclaimed_root))
		{
			output.owned_output_roots	 = palloc(sizeof(BlockNumber));
			output.owned_output_roots[0] = (BlockNumber)unclaimed_root;
			output.owned_output_count	 = 1;
			output.owned_output_capacity = 1;
			unclaimed_root				 = InvalidBlockNumber;
		}

		if (reclaim_mode != TP_SEGMENT_REPLACEMENT_RECLAIM_ATOMIC &&
			reclaim_mode != TP_SEGMENT_REPLACEMENT_RECLAIM_AFTER_PUBLICATION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid prepared replacement reclaim mode")));

		snapshot = tp_prepare_single_replacement_plan(
				index_state, index, level, source_root, &plan);
		memcpy(output.output_heads,
			   plan.retained_heads,
			   sizeof(output.output_heads));
		if (BlockNumberIsValid(replacement_root))
		{
			tp_prepare_single_replacement_root(
					index,
					replacement_root,
					level,
					plan.retained_heads[level]);
			output.output_heads[level]	= replacement_root;
			output.output_counts[level] = 1;
		}
		output.removed_docs	  = removed_docs;
		output.removed_tokens = removed_tokens;

		source_page_count =
				tp_segment_collect_pages(index, source_root, &pages);
		source_pages = pages;
		if (source_page_count == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prepared replacement source %u has no pages",
							source_root)));
		if (!defer_reclaim)
		{
			reclaim_fxid = GetCurrentFullTransactionId();
			tp_tombstone_build_detached(
					index,
					(BlockNumber *)source_pages,
					source_page_count,
					reclaim_fxid,
					&output.tombstones);
			pfree((BlockNumber *)source_pages);
			source_pages = NULL;
		}

		completion_started = true;
		tp_complete_compaction_publication(
				index_state,
				index,
				(TpIndexMetaPage)snapshot,
				&plan,
				&output,
				reclaim_fxid,
				defer_reclaim,
				TP_STATS_REBASE_CLAMP_LEGACY_VACUUM);
		if (defer_reclaim)
		{
			reclaim_fxid = ReadNextFullTransactionId();
			tp_tombstone_build_detached(
					index,
					(BlockNumber *)source_pages,
					source_page_count,
					reclaim_fxid,
					(TpDetachedTombstoneBatch *)&deferred_tombstones);
			tp_publish_detached_tombstones(
					index_state,
					index,
					(TpDetachedTombstoneBatch *)&deferred_tombstones);
			deferred_tombstones_attached = true;
			pfree((BlockNumber *)source_pages);
			source_pages = NULL;
		}
	}
	PG_CATCH();
	{
		if (BlockNumberIsValid((BlockNumber)unclaimed_root))
			tp_discard_unpublished_segment(index, (BlockNumber)unclaimed_root);
		else if (!completion_started && !output.publication_started)
			tp_discard_compaction_output(index, &output);
		if (!deferred_tombstones_attached &&
			deferred_tombstones.owned_pages != NULL)
			tp_tombstone_discard_detached(
					index, *(TpDetachedTombstoneBatch *)&deferred_tombstones);
		if (source_pages != NULL)
			pfree((BlockNumber *)source_pages);
		if (snapshot != NULL)
			pfree((TpIndexMetaPage)snapshot);
		tp_free_compaction_plan(&plan);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pfree((TpIndexMetaPage)snapshot);
	tp_free_compaction_plan(&plan);
}

static uint32
tp_compaction_candidate(
		const uint16 level_counts[TP_MAX_LEVELS], uint32 first_level)
{
	/*
	 * Every level is a candidate, including the top one: it compacts
	 * into itself rather than promoting, so its debt is reducible.
	 */
	for (uint32 level = first_level; level < TP_MAX_LEVELS; level++)
	{
		if ((uint32)level_counts[level] >= (uint32)tp_segments_per_level)
			return level;
	}

	return TP_MAX_LEVELS;
}

/*
 * Advisory: does any level sit at the compaction threshold?
 *
 * Shares tp_compaction_candidate() with the planner so this stays in
 * step with bm25_needs_compaction(), and carries the same caveat: a
 * level whose segments are all over budget reports true even though no
 * pass can reduce it.  Callers must not use this alone as a retry
 * condition.
 */
bool
tp_compaction_needed(Relation index)
{
	TpIndexMetaPageData *metap;
	bool				 needed;

	metap  = tp_get_metapage(index);
	needed = tp_compaction_candidate(metap->level_counts, 0) < TP_MAX_LEVELS;
	pfree(metap);

	return needed;
}

static void
tp_initialize_ordinary_plan(
		Relation index, const TpIndexMetaPage snapshot, TpCompactionPlan *plan)
{
	uint32 total_sources = 0;

	memset(plan, 0, sizeof(*plan));
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (pg_add_u32_overflow(
					total_sources,
					(uint32)snapshot->level_counts[level],
					&total_sources))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment count overflow in index \"%s\"",
							RelationGetRelationName(index))));

		plan->selected_heads[level]	 = InvalidBlockNumber;
		plan->retained_heads[level]	 = snapshot->level_heads[level];
		plan->retained_counts[level] = snapshot->level_counts[level];
	}

	Assert(total_sources > 0);
	plan->source_capacity = total_sources;
	plan->output_capacity = (uint32)tp_max_segments_per_level;
	plan->sources		  = palloc0(
			sizeof(TpCompactionSource) * plan->source_capacity);
	plan->batches = palloc0(sizeof(TpCompactionBatch) * plan->source_capacity);
}

static uint32
tp_select_level_prefix(
		Relation			  index,
		const TpIndexMetaPage snapshot,
		TpCompactionPlan	 *plan,
		uint32				  level,
		uint32				  prefix_count)
{
	BlockNumber current;
	uint32		first_source;
	uint32		chain_position;

	if (plan->retained_counts[level] < prefix_count)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("compaction level %u has no threshold-sized prefix",
						level)));

	first_source   = plan->num_sources;
	current		   = plan->retained_heads[level];
	chain_position = (uint32)snapshot->level_counts[level] -
					 (uint32)plan->retained_counts[level];
	if (plan->selected_counts[level] == 0)
		plan->selected_heads[level] = current;

	for (uint32 i = 0; i < prefix_count; i++)
		current = tp_collect_source(
				index, plan, current, level, chain_position + i);

	plan->selected_counts[level] += (uint16)prefix_count;
	plan->retained_heads[level] = current;
	plan->retained_counts[level] -= (uint16)prefix_count;
	return first_source;
}

/*
 * Give back a trailing run of batches that combine nothing.
 *
 * A one-source batch rewrites its whole segment and produces a segment
 * of the same size, so the pass pays a full copy for no reduction.  At
 * the tail of a prefix those are usually the level's oldest and largest
 * runs, already past max_segment_size and unable to absorb anything, so
 * a single pairable pair at the head can otherwise authorize rewriting
 * every one of them.
 *
 * Returning them is cheap precisely because the selection is a head
 * prefix: the chain behind it is untouched, so moving the retained head
 * back to the first returned segment restores the level without
 * rewriting any segment header.  They also keep their level, so nothing
 * about them changes on disk.  A one-source batch in the middle of a
 * prefix is still rewritten -- excising it would require repointing its
 * predecessor, which no longer fits in the publishing metapage record.
 *
 * This applies only to the level a pass chose to compact.  The capacity
 * recourse below selects a level to make room at, and there promoting a
 * one-source batch is the mechanism that makes it: handing those back
 * would leave the level exactly as full and turn a wasteful pass into a
 * failed one.
 */
static void
tp_trim_uncombinable_tail(
		TpCompactionPlan *plan, uint32 first_batch, uint32 level)
{
	while (plan->num_batches > first_batch &&
		   plan->batches[plan->num_batches - 1].source_count == 1)
	{
		TpCompactionBatch *batch = &plan->batches[plan->num_batches - 1];

		plan->retained_heads[level] = plan->sources[batch->first_source].root;
		plan->retained_counts[level]++;
		Assert(plan->selected_counts[level] > 0);
		plan->selected_counts[level]--;
		plan->num_sources = batch->first_source;
		plan->num_batches--;
	}
}

static void
tp_assign_ordinary_batches(
		TpCompactionPlan *plan,
		uint32			  first_batch,
		uint32			  planned_outputs[TP_MAX_LEVELS])
{
	for (uint32 i = first_batch; i < plan->num_batches; i++)
	{
		TpCompactionBatch  *batch  = &plan->batches[i];
		TpCompactionSource *source = &plan->sources[batch->first_source];
		uint32				minimum_level = source->source_level + 1;
		uint32 output_level = tp_size_class(batch->estimate.bytes);

		if (output_level < minimum_level)
			output_level = minimum_level;

		/*
		 * The top level is the ladder's terminal bucket, not a wall.
		 * A run that would promote past it stays there and compacts
		 * into itself, so the level is reducible like any other and
		 * carries no special count ceiling.
		 */
		if (output_level > TP_MAX_LEVELS - 1)
			output_level = TP_MAX_LEVELS - 1;

		for (uint32 j = 1; j < batch->source_count; j++)
		{
			if (plan->sources[batch->first_source + j].source_level !=
				source->source_level)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("ordinary compaction batch crosses levels")));
		}

		batch->output_level = output_level;
		planned_outputs[output_level]++;
	}
}

static bool
tp_build_empty_plan(
		Relation			  index,
		const TpIndexMetaPage snapshot,
		uint32				  first_level,
		TpCompactionPlan	 *plan)
{
	for (uint32 level = first_level; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = snapshot->level_heads[level];

		for (uint32 position = 0;
			 position < (uint32)snapshot->level_counts[level];
			 position++)
		{
			TpSegmentReader *reader;
			BlockNumber		 next;
			bool			 empty;

			if (!BlockNumberIsValid(current))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("segment chain for level %u ended before "
								"its recorded count",
								level)));

			reader = tp_segment_open(index, current);
			if (reader == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not open segment at block %u",
								current)));
			next  = reader->header->next_segment;
			empty = reader->header->alive_bitset_offset > 0 &&
					reader->header->alive_count == 0;
			tp_segment_close(reader);

			if (empty)
			{
				uint32 first_source;
				uint32 first_batch;
				uint32 prefix_count = position + 1;

				tp_initialize_ordinary_plan(index, snapshot, plan);
				first_source = tp_select_level_prefix(
						index, snapshot, plan, level, prefix_count);
				first_batch = plan->num_batches;
				tp_append_bounded_batches(plan, first_source, prefix_count);

				for (uint32 i = first_batch; i < plan->num_batches; i++)
				{
					TpCompactionBatch *batch = &plan->batches[i];

					for (uint32 j = 0; j < batch->source_count; j++)
					{
						if (plan->sources[batch->first_source + j]
									.source_level != level)
							ereport(ERROR,
									(errcode(ERRCODE_INTERNAL_ERROR),
									 errmsg("empty cleanup batch crosses "
											"levels")));
					}
					batch->output_level = level;
				}

				return true;
			}

			current = next;
		}

		if (BlockNumberIsValid(current))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment chain for level %u exceeds its "
							"recorded count",
							level)));
	}

	return false;
}

static bool
tp_build_ordinary_plan(
		Relation			  index,
		const TpIndexMetaPage snapshot,
		uint32				  first_level,
		TpCompactionPlan	 *plan)
{
	uint32 threshold	= (uint32)tp_segments_per_level;
	uint32 search_level = first_level;

	memset(plan, 0, sizeof(*plan));

	while (true)
	{
		uint32 planned_outputs[TP_MAX_LEVELS] = {0};
		uint32 candidate;
		uint32 first_source;
		uint32 first_batch;

		candidate =
				tp_compaction_candidate(snapshot->level_counts, search_level);
		if (candidate >= TP_MAX_LEVELS)
			return false;

		tp_initialize_ordinary_plan(index, snapshot, plan);
		first_source = tp_select_level_prefix(
				index, snapshot, plan, candidate, threshold);
		first_batch = plan->num_batches;
		tp_append_bounded_batches(plan, first_source, threshold);
		tp_trim_uncombinable_tail(plan, first_batch, candidate);
		tp_assign_ordinary_batches(plan, first_batch, planned_outputs);

		/*
		 * Outputs no longer always land above the candidate: the top
		 * level compacts into itself.  Check every level this plan
		 * would grow, and treat a full level uniformly -- it is over
		 * capacity only when it also has too few segments to compact.
		 */
		for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
		{
			while (planned_outputs[level] > 0 &&
				   (uint64)plan->retained_counts[level] +
								   (uint64)planned_outputs[level] >
						   (uint64)plan->output_capacity)
			{
				if (plan->retained_counts[level] < threshold)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("bm25 segment count limit reached at "
									"level %u",
									level)));

				first_source = tp_select_level_prefix(
						index, snapshot, plan, level, threshold);
				first_batch = plan->num_batches;
				tp_append_bounded_batches(plan, first_source, threshold);
				tp_assign_ordinary_batches(plan, first_batch, planned_outputs);
			}
		}

		if (plan->num_batches < plan->num_sources)
			return true;

		/*
		 * Promoting only singleton batches would leave the level count
		 * unchanged and make the outer driver chase the threshold up the
		 * hierarchy indefinitely.  Abandon this candidate, but keep
		 * searching: a level with no legal count reduction must not
		 * starve a higher level that has one.
		 */
		tp_free_compaction_plan(plan);
		memset(plan, 0, sizeof(*plan));
		search_level = candidate + 1;
	}
}

static void
tp_free_compaction_plan(TpCompactionPlan *plan)
{
	if (plan->sources != NULL)
		pfree(plan->sources);
	if (plan->batches != NULL)
		pfree(plan->batches);
}

static bool
tp_select_compaction_plan(
		TpLocalIndexState *index_state,
		Relation		   index,
		uint32			   first_level,
		bool			   empty_only,
		TpIndexMetaPage	  *snapshot_out,
		TpCompactionPlan  *plan)
{
	volatile bool	acquired_here = false;
	TpIndexMetaPage snapshot;
	bool			selected = false;
	bool			private_compaction;

	Assert(snapshot_out != NULL);
	*snapshot_out = NULL;
	memset(plan, 0, sizeof(*plan));
	private_compaction = tp_compaction_is_private(index_state, index);

	PG_TRY();
	{
		if (!private_compaction)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			acquired_here = true;
		}

		snapshot = tp_get_metapage(index);
		if (acquired_here)
		{
			tp_release_index_lock(index_state);
			acquired_here = false;
		}

		selected = tp_build_empty_plan(index, snapshot, first_level, plan);
		if (!selected && !empty_only &&
			tp_compaction_candidate(snapshot->level_counts, first_level) <
					TP_MAX_LEVELS)
			selected =
					tp_build_ordinary_plan(index, snapshot, first_level, plan);
	}
	PG_FINALLY();
	{
		if (acquired_here && index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	if (!selected)
	{
		pfree(snapshot);
		tp_free_compaction_plan(plan);
		memset(plan, 0, sizeof(*plan));
		return false;
	}

	*snapshot_out = snapshot;
	return true;
}

static bool
tp_select_force_compaction_plan(
		TpLocalIndexState *index_state,
		Relation		   index,
		TpIndexMetaPage	  *snapshot_out,
		TpCompactionPlan  *plan)
{
	volatile bool	acquired_here = false;
	TpIndexMetaPage snapshot;
	bool			selected;
	bool			private_compaction;

	Assert(snapshot_out != NULL);
	*snapshot_out = NULL;
	memset(plan, 0, sizeof(*plan));
	private_compaction = tp_compaction_is_private(index_state, index);

	PG_TRY();
	{
		if (!private_compaction)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			acquired_here = true;
		}

		snapshot = tp_get_metapage(index);
		if (acquired_here)
		{
			tp_release_index_lock(index_state);
			acquired_here = false;
		}

		tp_collect_force_sources(index, snapshot, plan);
		tp_build_force_batches(plan);
		selected = plan->num_sources > 0 &&
				   !tp_plan_is_noop(index, snapshot, plan);
	}
	PG_FINALLY();
	{
		if (acquired_here && index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	if (!selected)
	{
		pfree(snapshot);
		tp_free_compaction_plan(plan);
		memset(plan, 0, sizeof(*plan));
		return false;
	}

	*snapshot_out = snapshot;
	return true;
}

/*
 * Select, build, and publish one bounded compaction pass.  Runtime callers
 * hold the per-index maintenance object lock; CREATE INDEX instead holds its
 * private
 * per-index lock for the whole build because the index is not yet visible.
 *
 * Runtime selection and publication preparation take LW_SHARED briefly,
 * output construction holds no per-index lock, and only the final
 * GenericXLog publication takes fair LW_EXCLUSIVE.  The maintenance lock
 * keeps selected segment payloads immutable while a concurrent spill may
 * prepend an L0 prefix.
 */
static bool
tp_compact_once(
		TpLocalIndexState *index_state,
		Relation		   index,
		uint32			   first_level,
		bool			   empty_only)
{
	TpIndexMetaPage	   snapshot;
	TpCompactionPlan   plan;
	TpCompactionOutput output;
	uint32			   drained;
	bool			   private_compaction;

	private_compaction = tp_compaction_is_private(index_state, index);
	if (first_level >= TP_MAX_LEVELS)
		return false;
	if (!tp_select_compaction_plan(
				index_state, index, first_level, empty_only, &snapshot, &plan))
		return false;

	if (!private_compaction)
		tp_debug_compaction_pause(
				tp_debug_compaction_pause_after_select_ms,
				"after-select",
				RelationGetRelid(index));

	drained = tp_tombstone_drain(
			index,
			private_compaction ? NULL : index_state,
			tp_reclaim_horizon(NULL),
			/* own_lock */ !private_compaction);
	if (drained > 0)
		IndexFreeSpaceMapVacuum(index);

	tp_build_compaction_output(index, snapshot, &plan, &output);
	tp_complete_compaction_publication(
			index_state,
			index,
			snapshot,
			&plan,
			&output,
			InvalidFullTransactionId,
			false,
			TP_STATS_REBASE_STRICT);
	pfree(snapshot);
	tp_free_compaction_plan(&plan);
	return true;
}

bool
tp_compact_step(TpLocalIndexState *index_state, Relation index)
{
	return tp_compact_once(index_state, index, 0, false);
}

bool
tp_compact_empty_step(TpLocalIndexState *index_state, Relation index)
{
	return tp_compact_once(index_state, index, 0, true);
}

void
tp_force_compact(TpLocalIndexState *index_state, Relation index)
{
	TpIndexMetaPage	   snapshot;
	TpCompactionPlan   plan;
	TpCompactionOutput output;
	bool			   private_compaction;

	private_compaction = tp_compaction_is_private(index_state, index);
	if (!tp_select_force_compaction_plan(index_state, index, &snapshot, &plan))
		return;

	if (!private_compaction)
		tp_debug_compaction_pause(
				tp_debug_compaction_pause_after_select_ms,
				"after-select",
				RelationGetRelid(index));

	tp_build_compaction_output(index, snapshot, &plan, &output);
	tp_complete_compaction_publication(
			index_state,
			index,
			snapshot,
			&plan,
			&output,
			InvalidFullTransactionId,
			false,
			TP_STATS_REBASE_STRICT);
	pfree(snapshot);
	tp_free_compaction_plan(&plan);
}
