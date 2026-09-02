/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/transam.h>
#include <common/int.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <storage/lwlock.h>

#include "access/am.h"
#include "constants.h"
#include "index/metapage.h"
#include "index/state.h"
#include "segment/alive_bitset.h"
#include "segment/compaction.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/pagemapper.h"
#include "segment/tombstone.h"

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
	BlockNumber			retained_heads[TP_MAX_LEVELS];
	uint16				retained_counts[TP_MAX_LEVELS];
} TpCompactionPlan;

static void
tp_require_compaction_lock(TpLocalIndexState *index_state)
{
	if (index_state == NULL || index_state->shared == NULL ||
		!index_state->lock_held || index_state->lock_mode != LW_EXCLUSIVE ||
		!LWLockHeldByMe(&index_state->shared->lock))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("compaction requires the per-index exclusive lock")));
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

	if (estimate->docs > PG_UINT32_MAX - 7)
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

	if (estimate->docs > PG_UINT32_MAX || estimate->terms > PG_UINT32_MAX ||
		estimate->string_bytes > PG_UINT32_MAX ||
		estimate->skip_entries > PG_UINT32_MAX || data_pages > PG_UINT32_MAX ||
		total_pages >= InvalidBlockNumber)
		*representable = false;

	return true;
}

static void
tp_collect_source_estimate(TpCompactionSource *source, TpSegmentReader *reader)
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

	tp_collect_source_estimate(source, reader);
	next = reader->header->next_segment;
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
tp_metapage_matches_snapshot(Page page, const TpIndexMetaPage snapshot)
{
	TpIndexMetaPage current = (TpIndexMetaPage)PageGetContents(page);
	BlockNumber		memtable_head;
	BlockNumber		memtable_tail;
	BlockNumber		pending_free_head;

	if (current->version == TP_METAPAGE_VERSION_V6)
	{
		memtable_head = InvalidBlockNumber;
		memtable_tail = InvalidBlockNumber;
	}
	else
	{
		memtable_head = current->memtable_head_blkno;
		memtable_tail = current->memtable_tail_blkno;
	}

	pending_free_head = current->version < TP_METAPAGE_VERSION
							  ? InvalidBlockNumber
							  : current->pending_free_head;

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
		   memtable_head == snapshot->memtable_head_blkno &&
		   memtable_tail == snapshot->memtable_tail_blkno &&
		   pending_free_head == snapshot->pending_free_head;
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
		plan->retained_heads[level] = InvalidBlockNumber;
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

	buf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page	= BufferGetPage(buf);
	matches = tp_metapage_matches_snapshot(page, snapshot);
	UnlockReleaseBuffer(buf);
	return matches && tp_plan_chains_match(index, plan);
}

static void
tp_publish_plan(
		Relation			  index,
		const TpIndexMetaPage snapshot,
		const BlockNumber	  output_heads[TP_MAX_LEVELS],
		const uint16		  output_counts[TP_MAX_LEVELS],
		BlockNumber			  pending_free_head,
		uint64				  output_docs,
		uint64				  output_tokens)
{
	Buffer			  metabuf;
	Page			  current_page;
	GenericXLogState *xlog_state;
	Page			  meta_copy;
	TpIndexMetaPage	  meta;

	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	current_page = BufferGetPage(metabuf);

	if (!tp_metapage_matches_snapshot(current_page, snapshot))
	{
		UnlockReleaseBuffer(metabuf);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("metapage changed during compaction of "
						"index \"%s\"",
						RelationGetRelationName(index))));
	}

	xlog_state = GenericXLogStart(index);
	meta_copy  = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);
	tp_metapage_upgrade_to_current(index, meta_copy);
	meta = (TpIndexMetaPage)PageGetContents(meta_copy);

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		meta->level_heads[level]  = output_heads[level];
		meta->level_counts[level] = output_counts[level];
	}
	meta->pending_free_head = pending_free_head;
	meta->total_docs		= output_docs;
	meta->total_len			= output_tokens;

	GenericXLogFinish(xlog_state);
	UnlockReleaseBuffer(metabuf);
}

static void
tp_execute_plan(
		Relation index, const TpIndexMetaPage snapshot, TpCompactionPlan *plan)
{
	BlockNumber		  output_heads[TP_MAX_LEVELS];
	uint16			  output_counts[TP_MAX_LEVELS];
	uint64			  selected_docs	  = 0;
	uint64			  selected_tokens = 0;
	uint64			  output_docs	  = 0;
	uint64			  output_tokens	  = 0;
	uint64			  removed_docs;
	uint64			  removed_tokens;
	uint64			  final_docs;
	uint64			  final_tokens;
	BlockNumber		  pending_free_head = snapshot->pending_free_head;
	FullTransactionId merged_fxid		= ReadNextFullTransactionId();

	memcpy(output_heads, plan->retained_heads, sizeof(output_heads));
	memcpy(output_counts, plan->retained_counts, sizeof(output_counts));

	for (uint32 i = 0; i < plan->num_sources; i++)
	{
		TpSegmentReader *reader;

		reader = tp_segment_open(index, plan->sources[i].root);
		if (reader == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not open segment at block %u",
							plan->sources[i].root)));

		if (!tp_u64_add(
					selected_docs,
					(uint64)reader->header->num_docs,
					&selected_docs) ||
			!tp_u64_add(
					selected_tokens,
					reader->header->total_tokens,
					&selected_tokens))
		{
			tp_segment_close(reader);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("source segment statistics overflow")));
		}
		tp_segment_close(reader);
	}

	for (uint32 reverse = plan->num_batches; reverse > 0; reverse--)
	{
		TpCompactionBatch	 *batch = &plan->batches[reverse - 1];
		BlockNumber			 *roots;
		TpMergedSegmentResult result;

		roots = palloc(sizeof(BlockNumber) * batch->source_count);
		for (uint32 i = 0; i < batch->source_count; i++)
			roots[i] = plan->sources[batch->first_source + i].root;

		if (tp_merge_segment_batch(
					index,
					roots,
					batch->source_count,
					batch->output_level,
					output_heads[batch->output_level],
					&result))
		{
			if (output_counts[batch->output_level] >= plan->output_capacity)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("compaction output exceeded level %u "
								"capacity",
								batch->output_level)));

			output_heads[batch->output_level] = result.root;
			output_counts[batch->output_level]++;
			if (!tp_u64_add(
						output_docs, (uint64)result.num_docs, &output_docs) ||
				!tp_u64_add(
						output_tokens, result.total_tokens, &output_tokens))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("compaction output statistics overflow")));
		}

		pfree(roots);
	}

	if (output_docs > selected_docs || output_tokens > selected_tokens)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("compaction output statistics exceed source "
						"statistics")));

	removed_docs   = selected_docs - output_docs;
	removed_tokens = selected_tokens - output_tokens;
	if (snapshot->total_docs < removed_docs ||
		snapshot->total_len < removed_tokens)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("compaction source statistics exceed index "
						"statistics")));

	final_docs	 = snapshot->total_docs - removed_docs;
	final_tokens = snapshot->total_len - removed_tokens;

	for (uint32 i = 0; i < plan->num_sources; i++)
	{
		BlockNumber *pages;
		uint32		 num_pages;

		num_pages =
				tp_segment_collect_pages(index, plan->sources[i].root, &pages);
		pending_free_head = tp_tombstone_enqueue(
				index, pages, num_pages, merged_fxid, pending_free_head);
		if (pages != NULL)
			pfree(pages);
	}

	FlushRelationBuffers(index);
	tp_publish_plan(
			index,
			snapshot,
			output_heads,
			output_counts,
			pending_free_head,
			final_docs,
			final_tokens);
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

	for (uint32 i = 0; i < prefix_count; i++)
		current = tp_collect_source(
				index, plan, current, level, chain_position + i);

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

static void tp_free_compaction_plan(TpCompactionPlan *plan);

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

/*
 * Plan and run a single bounded compaction pass, searching for a
 * triggered level at or above first_level.  Returns true when a plan
 * executed and false when no level at or above first_level carries
 * threshold debt this engine can reduce.
 *
 * One pass is one publication: the plan is built in full -- including
 * any recursive compaction of a blocking destination level -- and
 * validated against the per-level segment capacity before
 * tp_execute_plan touches a page.  A caller that must bound how long it
 * holds the per-index exclusive lock can therefore run exactly one pass
 * and release, leaving the index in a consistent state.
 */
static bool
tp_compact_once(
		TpLocalIndexState *index_state, Relation index, uint32 first_level)
{
	TpIndexMetaPage	 snapshot;
	TpCompactionPlan plan;
	uint32			 drained;

	tp_require_compaction_lock(index_state);
	if (first_level >= TP_MAX_LEVELS)
		return false;

	/*
	 * Reclaiming displaced pages is part of compacting, not part of every
	 * write.  Leave the tombstone chain alone unless a level is actually
	 * triggered, so an ordinary spill cannot free pages that a merge
	 * parked for standby-safe reclaim.
	 */
	snapshot = tp_get_metapage(index);
	if (tp_compaction_candidate(snapshot->level_counts, first_level) >=
		TP_MAX_LEVELS)
	{
		pfree(snapshot);
		return false;
	}
	pfree(snapshot);

	drained = tp_tombstone_drain(
			index, NULL, tp_reclaim_horizon(NULL), /* own_lock */ false);
	if (drained > 0)
		IndexFreeSpaceMapVacuum(index);

	snapshot = tp_get_metapage(index);
	if (!tp_build_ordinary_plan(index, snapshot, first_level, &plan))
	{
		pfree(snapshot);
		tp_free_compaction_plan(&plan);
		return false;
	}

	tp_execute_plan(index, snapshot, &plan);
	pfree(snapshot);
	tp_free_compaction_plan(&plan);
	return true;
}

void
tp_maybe_compact_level(
		TpLocalIndexState *index_state, Relation index, uint32 first_level)
{
	while (tp_compact_once(index_state, index, first_level))
		;
}

bool
tp_compact_step(TpLocalIndexState *index_state, Relation index)
{
	return tp_compact_once(index_state, index, 0);
}

void
tp_force_compact(TpLocalIndexState *index_state, Relation index)
{
	TpIndexMetaPage	 snapshot;
	TpCompactionPlan plan;

	tp_require_compaction_lock(index_state);
	snapshot = tp_get_metapage(index);

	tp_collect_force_sources(index, snapshot, &plan);
	tp_build_force_batches(&plan);

	if (plan.num_sources == 0 || tp_plan_is_noop(index, snapshot, &plan))
	{
		pfree(snapshot);
		if (plan.sources != NULL)
			pfree(plan.sources);
		if (plan.batches != NULL)
			pfree(plan.batches);
		return;
	}

	tp_execute_plan(index, snapshot, &plan);

	pfree(snapshot);
	tp_free_compaction_plan(&plan);
}
