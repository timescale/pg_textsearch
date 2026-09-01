/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/transam.h>
#include <common/int.h>
#include <storage/bufmgr.h>
#include <storage/lwlock.h>

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
	bool			  indivisible;
	TpSegmentEstimate estimate;
} TpCompactionBatch;

typedef struct TpCompactionPlan
{
	TpCompactionSource *sources;
	uint32				num_sources;
	TpCompactionBatch  *batches;
	uint32				num_batches;
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
 * metadata, while an otherwise valid oversized source remains indivisible.
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
	uint32 source_index	 = 0;

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

	plan->sources = palloc0(sizeof(TpCompactionSource) * total_sources);

	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = snapshot->level_heads[level];

		for (uint32 position = 0; position < snapshot->level_counts[level];
			 position++)
		{
			TpCompactionSource *source;
			TpSegmentReader	   *reader;

			if (!BlockNumberIsValid(current))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("segment chain for level %u ended before its "
								"recorded count",
								level)));

			source				   = &plan->sources[source_index++];
			source->root		   = current;
			source->source_level   = level;
			source->chain_position = position;

			reader = tp_segment_open(index, current);
			if (reader == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not open segment at block %u",
								current)));

			if (reader->header->level != level)
			{
				uint32 recorded_level = reader->header->level;

				tp_segment_close(reader);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("segment at block %u records level %u but is "
								"linked from level %u",
								current,
								recorded_level,
								level)));
			}

			tp_collect_source_estimate(source, reader);
			current = reader->header->next_segment;
			tp_segment_close(reader);
		}

		if (BlockNumberIsValid(current))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment chain for level %u exceeds its "
							"recorded count",
							level)));
	}

	Assert(source_index == total_sources);
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
tp_build_force_batches(TpCompactionPlan *plan)
{
	uint64	budget		 = tp_max_segment_size_bytes();
	uint32	source_index = 0;
	uint32 *order;
	uint16	assigned_counts[TP_MAX_LEVELS];

	if (plan->num_sources == 0)
		return;

	plan->batches = palloc0(sizeof(TpCompactionBatch) * plan->num_sources);

	while (source_index < plan->num_sources)
	{
		TpCompactionBatch *batch = &plan->batches[plan->num_batches++];

		batch->first_source = source_index;
		batch->source_count = 1;
		batch->estimate		= plan->sources[source_index].estimate;
		batch->indivisible	= batch->estimate.bytes > budget;
		source_index++;

		while (!batch->indivisible && source_index < plan->num_sources)
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

		while (source_index < plan->num_sources &&
			   plan->sources[source_index].source_level < level)
			source_index++;

		if (source_index < plan->num_sources &&
			plan->sources[source_index].source_level == level)
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

		if (BlockNumberIsValid(current))
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
tp_publish_force_plan(
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
				 errmsg("metapage changed during force compaction of "
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
tp_execute_force_plan(
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
			if (output_counts[batch->output_level] >= PG_UINT16_MAX)
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
	tp_publish_force_plan(
			index,
			snapshot,
			output_heads,
			output_counts,
			pending_free_head,
			final_docs,
			final_tokens);
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

	tp_execute_force_plan(index, snapshot, &plan);

	pfree(snapshot);
	pfree(plan.sources);
	pfree(plan.batches);
}
