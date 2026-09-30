/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_merge.c - Streaming final merge for parallel index builds
 */
#include <postgres.h>

#include <common/int.h>
#include <limits.h>
#include <miscadmin.h>
#include <storage/buffile.h>
#include <utils/timestamp.h>

#include "access/build_merge.h"
#include "constants.h"
#include "segment/alive_bitset.h"
#include "segment/io.h"
#include "segment/segment.h"

#define TP_BUILD_MERGE_COPY_BUFFER_SIZE (64 * 1024)
#define TP_BUILD_MERGE_DICT_CHUNK		4096

static void
build_merge_rewind(BufFile *file)
{
	if (BufFileSeek(file, 0, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind parallel build merge stream")));
}

static void
build_merge_copy_file(
		BufFile *file, uint64 size, TpMergeSink *sink, char *buffer)
{
	uint64 remaining = size;

	build_merge_rewind(file);
	while (remaining > 0)
	{
		Size chunk = (Size)
				Min(remaining, (uint64)TP_BUILD_MERGE_COPY_BUFFER_SIZE);

		BufFileReadExact(file, buffer, chunk);
		merge_sink_write(sink, buffer, chunk);
		remaining -= chunk;
		CHECK_FOR_INTERRUPTS();
	}
}

static void
build_merge_copy_source(
		TpSegmentReader *reader,
		uint64			 source_offset,
		uint64			 size,
		TpMergeSink		*sink,
		char			*buffer)
{
	uint64 copied = 0;

	while (copied < size)
	{
		Size chunk = (Size)
				Min(size - copied, (uint64)TP_BUILD_MERGE_COPY_BUFFER_SIZE);

		tp_segment_read(reader, source_offset + copied, buffer, chunk);
		merge_sink_write(sink, buffer, chunk);
		copied += chunk;
		CHECK_FOR_INTERRUPTS();
	}
}

static void
build_merge_write_zeros(TpMergeSink *sink, uint64 size, char *buffer)
{
	uint64 remaining = size;

	memset(buffer, 0, TP_BUILD_MERGE_COPY_BUFFER_SIZE);
	while (remaining > 0)
	{
		Size chunk = (Size)
				Min(remaining, (uint64)TP_BUILD_MERGE_COPY_BUFFER_SIZE);

		merge_sink_write(sink, buffer, chunk);
		remaining -= chunk;
	}
}

static void
build_merge_write_alive(TpMergeSink *sink, uint32 num_docs, char *buffer)
{
	uint32 remaining = tp_alive_bitset_size(num_docs);

	memset(buffer, 0xff, TP_BUILD_MERGE_COPY_BUFFER_SIZE);
	while (remaining > 0)
	{
		uint32 chunk = Min(remaining, TP_BUILD_MERGE_COPY_BUFFER_SIZE);

		if (remaining == chunk && num_docs % 8 != 0)
			buffer[chunk - 1] = (1 << (num_docs % 8)) - 1;
		merge_sink_write(sink, buffer, chunk);
		remaining -= chunk;
	}
}

static void
build_merge_flush_block(
		TpMergeSink *sink, BufFile *skips, TpBlockPosting *block, uint32 count)
{
	TpSkipEntry skip = merge_sink_write_posting_block(sink, block, count);

	BufFileWrite(skips, &skip, sizeof(skip));
}

void
tp_write_parallel_build_merge(
		TpMergeSink	  *sink,
		TpMergeSource *sources,
		uint32		   num_sources,
		uint64		   total_docs,
		uint64		   total_tokens)
{
	BufFile			 *string_offsets_file;
	BufFile			 *strings_file;
	BufFile			 *refs_file;
	BufFile			 *skips_file;
	BufFile			 *dict_entries_file;
	TpTermSegmentRef *term_refs;
	uint32			 *source_doc_bases;
	char			 *copy_buffer;
	TpSegmentHeader	  header;
	TpDictionary	  dict;
	uint64			  source_docs		 = 0;
	uint64			  source_tokens		 = 0;
	uint64			  string_pos		 = 0;
	uint64			  skip_entries		 = 0;
	uint64			  skip_bytes_written = 0;
	uint32			  num_terms			 = 0;
	uint32			  i;

	if (num_sources > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("pg_textsearch: too many parallel build segments")));
	if (!tp_document_count_fits(total_docs))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("pg_textsearch: segment exceeds %u documents",
						TP_MAX_GROWABLE_CAPACITY)));

	source_doc_bases =
			palloc_extended(num_sources * sizeof(uint32), MCXT_ALLOC_HUGE);
	for (i = 0; i < num_sources; i++)
	{
		TpSegmentHeader *source_header = sources[i].reader->header;

		if (sources[i].reader->buffile == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("parallel build merge received a non-BufFile "
							"source")));
		if (source_header->alive_bitset_offset != 0 ||
			source_header->alive_count != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("parallel build worker segment has an unexpected "
							"alive bitset")));

		source_doc_bases[i] = (uint32)source_docs;
		if (pg_add_u64_overflow(
					source_docs, source_header->num_docs, &source_docs) ||
			!tp_document_count_fits(source_docs))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_textsearch: segment exceeds %u documents",
							TP_MAX_GROWABLE_CAPACITY)));
		if (pg_add_u64_overflow(
					source_tokens,
					source_header->total_tokens,
					&source_tokens))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_textsearch: token count overflow")));
	}
	if (source_docs != total_docs || source_tokens != total_tokens)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("parallel build worker totals do not match leader "
						"totals"),
				 errdetail(
						 "Worker segments contain %" PRIu64
						 " documents and %" PRIu64
						 " tokens; leader collected %" PRIu64
						 " documents and %" PRIu64 " tokens.",
						 source_docs,
						 source_tokens,
						 total_docs,
						 total_tokens)));

	string_offsets_file = BufFileCreateTemp(false);
	strings_file		= BufFileCreateTemp(false);
	refs_file			= BufFileCreateTemp(false);
	skips_file			= BufFileCreateTemp(false);
	dict_entries_file	= BufFileCreateTemp(false);
	term_refs			= palloc_extended(
			  num_sources * sizeof(TpTermSegmentRef), MCXT_ALLOC_HUGE);
	copy_buffer = palloc(TP_BUILD_MERGE_COPY_BUFFER_SIZE);

	/*
	 * Merge dictionaries once, retaining only one term's exact source
	 * references while spooling compact metadata streams.
	 */
	while (true)
	{
		int			min_idx;
		const char *term;
		uint32		term_len;
		uint32		num_refs	  = 0;
		uint64		term_postings = 0;
		uint64		term_blocks;
		uint32		dict_offset;
		uint32		string_offset;

		min_idx = merge_find_min_source(sources, num_sources);
		if (min_idx < 0)
			break;
		if (num_terms >= TP_MAX_DICTIONARY_TERMS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_textsearch: segment dictionary exceeds %u "
							"terms",
							TP_MAX_DICTIONARY_TERMS)));

		term	 = sources[min_idx].current_term;
		term_len = strlen(term);
		if (tp_string_pool_offset_overflows(
					string_pos, tp_string_pool_entry_size(term_len)))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_textsearch: merged string pool exceeds 4 "
							"GiB")));

		string_offset = (uint32)string_pos;
		dict_offset	  = (uint32)((uint64)num_terms * sizeof(TpDictEntry));
		BufFileWrite(
				string_offsets_file, &string_offset, sizeof(string_offset));
		BufFileWrite(strings_file, &term_len, sizeof(term_len));
		BufFileWrite(strings_file, term, term_len);
		BufFileWrite(strings_file, &dict_offset, sizeof(dict_offset));
		string_pos += tp_string_pool_entry_size(term_len);

		for (i = 0; i < num_sources; i++)
		{
			if (sources[i].exhausted ||
				strcmp(sources[i].current_term, term) != 0)
				continue;

			term_refs[num_refs].segment_idx = (int)i;
			term_refs[num_refs].entry		= sources[i].current_entry;
			if (pg_add_u64_overflow(
						term_postings,
						sources[i].current_entry.doc_freq,
						&term_postings))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("pg_textsearch: posting count overflow")));
			num_refs++;
		}
		if (!tp_document_count_fits(term_postings))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_textsearch: term exceeds %u postings",
							TP_MAX_GROWABLE_CAPACITY)));
		term_blocks = tp_posting_block_count(term_postings);
		if (term_blocks > TP_MAX_GROWABLE_CAPACITY - skip_entries)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_textsearch: segment exceeds %u posting blocks",
							TP_MAX_GROWABLE_CAPACITY)));
		skip_entries += term_blocks;

		BufFileWrite(refs_file, &num_refs, sizeof(num_refs));
		BufFileWrite(
				refs_file, term_refs, num_refs * sizeof(TpTermSegmentRef));

		for (i = 0; i < num_refs; i++)
			merge_source_advance(&sources[term_refs[i].segment_idx]);
		num_terms++;
		CHECK_FOR_INTERRUPTS();
	}

	memset(&header, 0, sizeof(header));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;
	header.num_docs		= (uint32)total_docs;
	header.total_tokens = total_tokens;
	header.page_index	= InvalidBlockNumber;

	merge_sink_write(sink, &header, sizeof(header));
	header.dictionary_offset = sink->current_offset;

	memset(&dict, 0, sizeof(dict));
	dict.num_terms = num_terms;
	merge_sink_write(sink, &dict, offsetof(TpDictionary, string_offsets));
	build_merge_copy_file(
			string_offsets_file,
			(uint64)num_terms * sizeof(uint32),
			sink,
			copy_buffer);

	header.strings_offset = sink->current_offset;
	build_merge_copy_file(strings_file, string_pos, sink, copy_buffer);

	header.entries_offset = sink->current_offset;
	build_merge_write_zeros(sink, tp_dictionary_size(num_terms), copy_buffer);
	header.postings_offset = sink->current_offset;

	build_merge_rewind(refs_file);
	for (i = 0; i < num_terms; i++)
	{
		TpBlockPosting block[TP_BLOCK_SIZE];
		TpDictEntry	   entry;
		uint32		   num_refs;
		uint32		   block_count	 = 0;
		uint32		   doc_count	 = 0;
		uint32		   in_block		 = 0;
		uint64		   expected_docs = 0;
		uint32		   ref_idx;
		int			   previous_source = -1;

		BufFileReadExact(refs_file, &num_refs, sizeof(num_refs));
		if (num_refs == 0 || num_refs > num_sources)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid parallel build term reference count %u",
							num_refs)));
		BufFileReadExact(
				refs_file, term_refs, num_refs * sizeof(TpTermSegmentRef));

		entry.skip_index_offset = skip_bytes_written;

		for (ref_idx = 0; ref_idx < num_refs; ref_idx++)
		{
			TpTermSegmentRef	*ref = &term_refs[ref_idx];
			TpMergeSource		*source;
			TpPostingMergeSource posting_source;

			if (ref->segment_idx <= previous_source || ref->segment_idx < 0 ||
				(uint32)ref->segment_idx >= num_sources)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid parallel build term source index")));
			previous_source = ref->segment_idx;
			source			= &sources[ref->segment_idx];
			expected_docs += ref->entry.doc_freq;

			posting_source_init_fast(
					&posting_source, source->reader, &ref->entry);
			while (!posting_source.exhausted)
			{
				TpBlockPosting *input =
						&posting_source.block_postings
								 [posting_source.current_in_block];
				uint64 output_doc_id;

				if (input->doc_id >= source->reader->header->num_docs)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("parallel build posting document ID %u "
									"exceeds source document count %u",
									input->doc_id,
									source->reader->header->num_docs)));
				output_doc_id = (uint64)source_doc_bases[ref->segment_idx] +
								input->doc_id;
				if (output_doc_id >= total_docs)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("parallel build posting document ID "
									"exceeds merged document count")));

				block[in_block].doc_id	  = (uint32)output_doc_id;
				block[in_block].frequency = input->frequency;
				block[in_block].fieldnorm = input->fieldnorm;
				block[in_block].reserved  = 0;
				in_block++;
				doc_count++;

				posting_source_advance_fast(&posting_source);
				if (in_block == TP_BLOCK_SIZE)
				{
					build_merge_flush_block(sink, skips_file, block, in_block);
					in_block = 0;
					block_count++;
				}
			}
			posting_source_free(&posting_source);
		}

		if (in_block > 0)
		{
			build_merge_flush_block(sink, skips_file, block, in_block);
			block_count++;
		}
		if (doc_count != expected_docs)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("parallel build posting count mismatch"),
					 errdetail(
							 "Expected %" PRIu64 " postings but read %u.",
							 expected_docs,
							 doc_count)));

		entry.block_count = block_count;
		entry.doc_freq	  = doc_count;
		BufFileWrite(dict_entries_file, &entry, sizeof(entry));
		skip_bytes_written += (uint64)block_count * sizeof(TpSkipEntry);
		if ((i % 1000) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	if (skip_bytes_written != skip_entries * sizeof(TpSkipEntry))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("parallel build skip count mismatch")));

	header.skip_index_offset = sink->current_offset;
	build_merge_copy_file(
			skips_file, skip_entries * sizeof(TpSkipEntry), sink, copy_buffer);

	header.fieldnorm_offset = sink->current_offset;
	for (i = 0; i < num_sources; i++)
		build_merge_copy_source(
				sources[i].reader,
				sources[i].reader->header->fieldnorm_offset,
				(uint64)sources[i].reader->header->num_docs * sizeof(uint8),
				sink,
				copy_buffer);

	header.ctid_pages_offset = sink->current_offset;
	for (i = 0; i < num_sources; i++)
		build_merge_copy_source(
				sources[i].reader,
				sources[i].reader->header->ctid_pages_offset,
				(uint64)sources[i].reader->header->num_docs *
						sizeof(BlockNumber),
				sink,
				copy_buffer);

	header.ctid_offsets_offset = sink->current_offset;
	for (i = 0; i < num_sources; i++)
		build_merge_copy_source(
				sources[i].reader,
				sources[i].reader->header->ctid_offsets_offset,
				(uint64)sources[i].reader->header->num_docs *
						sizeof(OffsetNumber),
				sink,
				copy_buffer);

	header.alive_bitset_offset = sink->current_offset;
	header.alive_count		   = (uint32)total_docs;
	if (total_docs > 0)
		build_merge_write_alive(sink, (uint32)total_docs, copy_buffer);

	header.data_size = sink->current_offset;
	tp_segment_writer_flush(&sink->writer);
	sink->writer.buffer_pos = SizeOfPageHeaderData;

	build_merge_rewind(dict_entries_file);
	for (i = 0; i < num_terms;)
	{
		TpDictEntry entries[TP_BUILD_MERGE_DICT_CHUNK];
		uint32		count = Min(TP_BUILD_MERGE_DICT_CHUNK, num_terms - i);
		uint32		j;

		BufFileReadExact(
				dict_entries_file, entries, count * sizeof(TpDictEntry));
		for (j = 0; j < count; j++)
		{
			if (pg_add_u64_overflow(
						header.skip_index_offset,
						entries[j].skip_index_offset,
						&entries[j].skip_index_offset))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("pg_textsearch: skip index offset overflow")));
		}
		merge_sink_write_at(
				sink,
				header.entries_offset + (uint64)i * sizeof(TpDictEntry),
				entries,
				count * sizeof(TpDictEntry));
		i += count;
	}

	merge_sink_finish(sink, &header);

	BufFileClose(string_offsets_file);
	BufFileClose(strings_file);
	BufFileClose(refs_file);
	BufFileClose(skips_file);
	BufFileClose(dict_entries_file);
	pfree(copy_buffer);
	pfree(term_refs);
	pfree(source_doc_bases);
}
