/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_context.c - Arena-based build context for CREATE INDEX
 *
 * See build_context.h for design overview.
 */
#include <postgres.h>

#include <common/hashfn.h>
#include <inttypes.h>
#include <storage/buffile.h>
#include <storage/bufmgr.h>
#include <utils/memutils.h>

#include "build_context.h"
#include "constants.h"
#include "memtable/arena.h"
#include "memtable/expull.h"
#include "segment/compression.h"
#include "segment/fieldnorm.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "segment/segment_io.h"

/* Forward declarations for hash table support */
static uint32 build_term_hash(const void *key, Size keysize);
static int build_term_match(const void *key1, const void *key2, Size keysize);

/* Comparison function for sorting terms */
static int build_term_info_cmp(const void *a, const void *b);

/* GUC: compression for segments */
extern bool tp_compress_segments;

/*
 * Create a new build context.
 */
TpBuildContext *
tp_build_context_create(Size budget)
{
	TpBuildContext *ctx;
	HASHCTL			info;

	ctx = palloc0(sizeof(TpBuildContext));

	/* Create arena */
	ctx->arena = tp_arena_create();

	/* Create local hash table for terms */
	memset(&info, 0, sizeof(info));
	info.keysize   = sizeof(char *);
	info.entrysize = sizeof(TpBuildTermEntry);
	info.hash	   = build_term_hash;
	info.match	   = build_term_match;
	ctx->terms_ht  = hash_create(
			 "build_terms",
			 16384, /* initial size */
			 &info,
			 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	/* Allocate flat arrays for documents */
	ctx->docs_capacity = TP_BUILD_INITIAL_DOCS;
	ctx->fieldnorms	   = palloc(ctx->docs_capacity * sizeof(uint8));
	ctx->ctids		   = palloc(ctx->docs_capacity * sizeof(ItemPointerData));
	ctx->num_docs	   = 0;
	ctx->total_len	   = 0;
	ctx->budget		   = budget;

	return ctx;
}

/*
 * Hash function: hash the string content pointed to by the key.
 */
static uint32
build_term_hash(const void *key, Size keysize)
{
	const char *term = *(const char *const *)key;

	(void)keysize;
	return DatumGetUInt32(hash_any((const unsigned char *)term, strlen(term)));
}

/*
 * Match function: compare the string content pointed to by keys.
 */
static int
build_term_match(const void *key1, const void *key2, Size keysize)
{
	const char *t1 = *(const char *const *)key1;
	const char *t2 = *(const char *const *)key2;

	(void)keysize;
	return strcmp(t1, t2);
}

/*
 * Grow the flat document arrays when capacity is exceeded.
 */
static void
build_context_grow_docs(TpBuildContext *ctx)
{
	uint32 new_capacity = ctx->docs_capacity * 2;

	ctx->fieldnorms = repalloc(ctx->fieldnorms, new_capacity * sizeof(uint8));
	ctx->ctids = repalloc(ctx->ctids, new_capacity * sizeof(ItemPointerData));
	ctx->docs_capacity = new_capacity;
}

/*
 * Add a single document's terms to the build context.
 */
uint32
tp_build_context_add_document(
		TpBuildContext *ctx,
		char		  **terms,
		int32		   *frequencies,
		int				term_count,
		int32			doc_length,
		ItemPointer		ctid)
{
	uint32 doc_id;
	uint8  norm;
	int	   i;

	Assert(ctx != NULL);
	Assert(ctid != NULL);

	/* Assign sequential doc_id */
	if (ctx->num_docs >= ctx->docs_capacity)
		build_context_grow_docs(ctx);

	doc_id = ctx->num_docs;
	norm   = encode_fieldnorm(doc_length);

	/* Store fieldnorm and CTID */
	ctx->fieldnorms[doc_id] = norm;
	ctx->ctids[doc_id]		= *ctid;
	ctx->num_docs++;
	ctx->total_len += doc_length;

	/* Add each term to the hash table and EXPULL */
	for (i = 0; i < term_count; i++)
	{
		TpBuildTermEntry *entry;
		bool			  found;
		char			 *term_key;

		/*
		 * Look up or create the term entry. The hash table key
		 * is a char* pointer. For new entries, we copy the term
		 * string into the arena so it persists.
		 */
		term_key = terms[i];
		entry	 = hash_search(ctx->terms_ht, &term_key, HASH_ENTER, &found);

		if (!found)
		{
			ArenaAddr str_addr;
			char	 *arena_str;
			uint32	  len = strlen(terms[i]);

			/* Copy term string into arena */
			str_addr  = tp_arena_alloc(ctx->arena, len + 1);
			arena_str = tp_arena_get_ptr(ctx->arena, str_addr);
			memcpy(arena_str, terms[i], len + 1);

			/* Update entry to point to arena copy */
			entry->term		= arena_str;
			entry->term_len = len;
			tp_expull_init(&entry->expull);
		}

		/* Append posting to this term's EXPULL list */
		tp_expull_append(
				ctx->arena,
				&entry->expull,
				doc_id,
				(uint16)frequencies[i],
				norm);
	}

	return doc_id;
}

/*
 * Comparison function for sorting TpBuildTermInfo by term string.
 */
static int
build_term_info_cmp(const void *a, const void *b)
{
	const TpBuildTermInfo *ta = (const TpBuildTermInfo *)a;
	const TpBuildTermInfo *tb = (const TpBuildTermInfo *)b;

	return strcmp(ta->term, tb->term);
}

/*
 * Build a sorted term array from the build context.
 */
TpBuildTermInfo *
tp_build_context_get_sorted_terms(TpBuildContext *ctx, uint32 *num_terms)
{
	HASH_SEQ_STATUS	  status;
	TpBuildTermEntry *entry;
	TpBuildTermInfo	 *terms;
	uint32			  count;
	uint32			  i;

	count = hash_get_num_entries(ctx->terms_ht);
	if (count == 0)
	{
		*num_terms = 0;
		return NULL;
	}

	terms = palloc(count * sizeof(TpBuildTermInfo));
	i	  = 0;

	hash_seq_init(&status, ctx->terms_ht);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		Assert(i < count);
		terms[i].term	  = entry->term;
		terms[i].term_len = entry->term_len;
		terms[i].expull	  = &entry->expull;
		terms[i].doc_freq = entry->expull.num_entries;
		i++;
	}

	Assert(i == count);

	/* Sort lexicographically */
	qsort(terms, count, sizeof(TpBuildTermInfo), build_term_info_cmp);

	*num_terms = count;
	return terms;
}

/*
 * Write a segment from the build context.
 *
 * This mirrors tp_write_segment() but reads from the arena/EXPULL
 * instead of DSA/dshash. Doc IDs are already sequential (assigned
 * during add_document), so no docmap hash lookup is needed.
 */
BlockNumber
tp_write_segment_from_build_ctx(TpBuildContext *ctx, Relation index)
{
	TpBuildTermInfo *terms;
	uint32			 num_terms;
	BlockNumber		 header_block;
	BlockNumber		 page_index_root;
	TpSegmentWriter	 writer;
	TpSegmentHeader	 header;
	TpDictionary	 dict;

	uint32 *string_offsets;
	uint32	string_pos;
	uint32	i;
	Buffer	header_buf;
	Page	header_page;

	/*
	 * Per-term block tracking (same as tp_write_segment).
	 */
	typedef struct
	{
		uint64 posting_offset;
		uint32 skip_entry_start;
		uint16 block_count;
		uint32 doc_freq;
	} TermBlockInfo;

	TermBlockInfo *term_blocks;

	/* Accumulated skip entries */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	/* Get sorted terms */
	terms = tp_build_context_get_sorted_terms(ctx, &num_terms);
	if (num_terms == 0)
		return InvalidBlockNumber;

	/* Initialize writer */
	tp_segment_writer_init(&writer, index);

	if (writer.pages_allocated == 0)
		elog(ERROR,
			 "tp_write_segment_from_build_ctx: "
			 "Failed to allocate first page");

	header_block = writer.pages[0];

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;
	header.num_docs		= ctx->num_docs;
	header.total_tokens = ctx->total_len;

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	dict.num_terms = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Build string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);
	}

	/* Write string offsets array */
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries */
	{
		TpDictEntry placeholder;

		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntry));
	}

	/* Postings start here */
	header.postings_offset = writer.current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(TermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming pass: for each term, read postings from EXPULL
	 * and write compressed blocks.
	 */
	for (i = 0; i < num_terms; i++)
	{
		TpExpullReader reader;
		uint32		   doc_count;
		uint32		   block_idx;
		uint32		   num_blocks;

		/* Record where this term's postings start */
		term_blocks[i].posting_offset	= writer.current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		doc_count				= terms[i].expull->num_entries;
		term_blocks[i].doc_freq = terms[i].doc_freq;

		if (doc_count == 0)
		{
			term_blocks[i].block_count = 0;
			continue;
		}

		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
		term_blocks[i].block_count = (uint16)num_blocks;

		/* Initialize EXPULL reader for this term */
		tp_expull_reader_init(&reader, ctx->arena, terms[i].expull);

		/* Process posting blocks */
		for (block_idx = 0; block_idx < num_blocks; block_idx++)
		{
			TpExpullEntry  entries[TP_BLOCK_SIZE];
			TpBlockPosting block_postings[TP_BLOCK_SIZE];
			TpSkipEntry	   skip;
			uint32		   nread;
			uint32		   j;
			uint16		   max_tf	  = 0;
			uint8		   min_norm	  = 255;
			uint32		   last_docid = 0;

			/* Read a block of entries from EXPULL */
			nread = tp_expull_reader_read(&reader, entries, TP_BLOCK_SIZE);
			Assert(nread > 0);

			/* Convert to TpBlockPosting format */
			for (j = 0; j < nread; j++)
			{
				block_postings[j].doc_id	= entries[j].doc_id;
				block_postings[j].frequency = entries[j].frequency;
				block_postings[j].fieldnorm = entries[j].fieldnorm;
				block_postings[j].reserved	= 0;

				/* Track block stats */
				if (entries[j].doc_id > last_docid)
					last_docid = entries[j].doc_id;
				if (entries[j].frequency > max_tf)
					max_tf = entries[j].frequency;
				if (entries[j].fieldnorm < min_norm)
					min_norm = entries[j].fieldnorm;
			}

			/* Build skip entry */
			skip.last_doc_id	= last_docid;
			skip.doc_count		= (uint8)nread;
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = min_norm;
			skip.posting_offset = writer.current_offset;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			/* Write posting block */
			if (tp_compress_segments)
			{
				uint8  compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];
				uint32 compressed_size;

				compressed_size =
						tp_compress_block(block_postings, nread, compressed);
				skip.flags = TP_BLOCK_FLAG_DELTA;
				tp_segment_writer_write(&writer, compressed, compressed_size);
			}
			else
			{
				skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
				tp_segment_writer_write(
						&writer,
						block_postings,
						nread * sizeof(TpBlockPosting));
			}

			/* Accumulate skip entry */
			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;
		}
	}

	/* Write skip index */
	header.skip_index_offset = writer.current_offset;
	if (skip_entries_count > 0)
		tp_segment_writer_write(
				&writer,
				all_skip_entries,
				skip_entries_count * sizeof(TpSkipEntry));

	/* Write fieldnorm table */
	header.fieldnorm_offset = writer.current_offset;
	if (ctx->num_docs > 0)
		tp_segment_writer_write(
				&writer, ctx->fieldnorms, ctx->num_docs * sizeof(uint8));

	/* Write CTID pages array (BlockNumber per doc) */
	header.ctid_pages_offset = writer.current_offset;
	{
		uint32 *ctid_pages = palloc(ctx->num_docs * sizeof(uint32));

		for (i = 0; i < ctx->num_docs; i++)
			ctid_pages[i] = ItemPointerGetBlockNumber(&ctx->ctids[i]);
		tp_segment_writer_write(
				&writer, ctid_pages, ctx->num_docs * sizeof(uint32));
		pfree(ctid_pages);
	}

	/* Write CTID offsets array (OffsetNumber per doc) */
	header.ctid_offsets_offset = writer.current_offset;
	{
		uint16 *ctid_offsets = palloc(ctx->num_docs * sizeof(uint16));

		for (i = 0; i < ctx->num_docs; i++)
			ctid_offsets[i] = ItemPointerGetOffsetNumber(&ctx->ctids[i]);
		tp_segment_writer_write(
				&writer, ctid_offsets, ctx->num_docs * sizeof(uint16));
		pfree(ctid_offsets);
	}

	/* Flush remaining buffered data */
	tp_segment_writer_flush(&writer);

	/*
	 * Prevent tp_segment_writer_finish from flushing again
	 * (matches existing tp_write_segment pattern).
	 */
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index */
	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);

	/* Finalize header */
	header.page_index = page_index_root;
	header.data_size  = writer.current_offset;
	header.num_pages  = writer.pages_allocated;

	/*
	 * Write dictionary entries with real skip_index_offset
	 * values. This must happen BEFORE tp_segment_writer_finish
	 * so writer.pages is still valid.
	 */
	{
		Buffer dict_buf		= InvalidBuffer;
		uint32 current_page = UINT32_MAX;

		for (i = 0; i < num_terms; i++)
		{
			TpDictEntry entry;
			uint64		entry_offset;
			uint32		logical_page;
			uint32		page_offset;
			BlockNumber physical_block;

			entry.skip_index_offset =
					header.skip_index_offset +
					((uint64)term_blocks[i].skip_entry_start *
					 sizeof(TpSkipEntry));
			entry.block_count = term_blocks[i].block_count;
			entry.reserved	  = 0;
			entry.doc_freq	  = term_blocks[i].doc_freq;

			/* Locate this entry in the segment */
			entry_offset = header.entries_offset +
						   ((uint64)i * sizeof(TpDictEntry));
			logical_page = (uint32)(entry_offset / SEGMENT_DATA_PER_PAGE);
			page_offset	 = (uint32)(entry_offset % SEGMENT_DATA_PER_PAGE);

			if (logical_page >= writer.pages_allocated)
				elog(ERROR,
					 "dict entry %u: logical page %u >= "
					 "pages %u",
					 i,
					 logical_page,
					 writer.pages_allocated);

			/* Read page if different from current */
			if (logical_page != current_page)
			{
				if (current_page != UINT32_MAX)
				{
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);
				}
				physical_block = writer.pages[logical_page];
				dict_buf	   = ReadBuffer(index, physical_block);
				LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
				current_page = logical_page;
			}

			/* Write entry — handle page boundary spanning */
			{
				uint32 bytes_remaining = SEGMENT_DATA_PER_PAGE - page_offset;

				if (bytes_remaining >= sizeof(TpDictEntry))
				{
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					memcpy(dest, &entry, sizeof(TpDictEntry));
				}
				else
				{
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					char *src = (char *)&entry;

					memcpy(dest, src, bytes_remaining);
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);

					logical_page++;
					if (logical_page >= writer.pages_allocated)
						elog(ERROR,
							 "dict entry spans beyond "
							 "allocated pages");

					physical_block = writer.pages[logical_page];
					dict_buf	   = ReadBuffer(index, physical_block);
					LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
					current_page = logical_page;

					page = BufferGetPage(dict_buf);
					dest = (char *)page + SizeOfPageHeaderData;
					memcpy(dest,
						   src + bytes_remaining,
						   sizeof(TpDictEntry) - bytes_remaining);
				}
			}
		}

		if (current_page != UINT32_MAX)
		{
			MarkBufferDirty(dict_buf);
			UnlockReleaseBuffer(dict_buf);
		}
	}

	tp_segment_writer_finish(&writer);

	/* Flush all pages to disk */
	FlushRelationBuffers(index);

	/* Write final header */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page = BufferGetPage(header_buf);
	{
		TpSegmentHeader *hdr = (TpSegmentHeader *)PageGetContents(header_page);
		hdr->strings_offset	 = header.strings_offset;
		hdr->entries_offset	 = header.entries_offset;
		hdr->postings_offset = header.postings_offset;
		hdr->skip_index_offset	 = header.skip_index_offset;
		hdr->fieldnorm_offset	 = header.fieldnorm_offset;
		hdr->ctid_pages_offset	 = header.ctid_pages_offset;
		hdr->ctid_offsets_offset = header.ctid_offsets_offset;
		hdr->num_docs			 = header.num_docs;
		hdr->data_size			 = header.data_size;
		hdr->num_pages			 = header.num_pages;
		hdr->page_index			 = header.page_index;
	}
	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	FlushRelationBuffers(index);

	/* Cleanup */
	pfree(term_blocks);
	pfree(all_skip_entries);
	pfree(string_offsets);
	pfree(terms);
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/*
 * Write a segment from the build context to a BufFile as a flat
 * byte stream. No page boundaries, no page index. Used by parallel
 * build workers to write to SharedFileSet temp files.
 *
 * Returns the data_size (total bytes written for this segment).
 */
uint64
tp_write_segment_to_buffile(TpBuildContext *ctx, BufFile *file)
{
	TpBuildTermInfo *terms;
	uint32			 num_terms;
	TpSegmentHeader	 header;
	TpDictionary	 dict;

	/*
	 * BufFile position tracking. BufFile uses (fileno, offset)
	 * pairs where fileno is the 1GB file segment index. We save
	 * the starting position and end position to handle seek-back
	 * for header/dict entry updates.
	 */
	int	  base_fileno;
	off_t base_file_offset;

	uint32 *string_offsets;
	uint32	string_pos;
	uint32	i;

	/* Current write position in the flat stream */
	uint64 current_offset;

	/* Per-term block tracking */
	typedef struct
	{
		uint64 posting_offset;
		uint32 skip_entry_start;
		uint16 block_count;
		uint32 doc_freq;
	} TermBlockInfo;

	TermBlockInfo *term_blocks;

	/* Accumulated skip entries */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	/* Get sorted terms */
	terms = tp_build_context_get_sorted_terms(ctx, &num_terms);
	if (num_terms == 0)
		return 0;

	/* Record starting position for later seek-back */
	BufFileTell(file, &base_fileno, &base_file_offset);

	current_offset = 0;

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0; /* No pages in flat format */
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;
	header.num_docs		= ctx->num_docs;
	header.total_tokens = ctx->total_len;
	header.page_index	= InvalidBlockNumber;

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	BufFileWrite(file, &header, sizeof(TpSegmentHeader));
	current_offset += sizeof(TpSegmentHeader);

	/* Write dictionary section */
	dict.num_terms = num_terms;
	BufFileWrite(file, &dict, offsetof(TpDictionary, string_offsets));
	current_offset += offsetof(TpDictionary, string_offsets);

	/* Build string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);
	}

	/* Write string offsets array */
	BufFileWrite(file, string_offsets, num_terms * sizeof(uint32));
	current_offset += num_terms * sizeof(uint32);

	/* Write string pool */
	header.strings_offset = current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		BufFileWrite(file, &length, sizeof(uint32));
		BufFileWrite(file, terms[i].term, length);
		BufFileWrite(file, &dict_offset, sizeof(uint32));
		current_offset += sizeof(uint32) + length + sizeof(uint32);
	}

	/* Record entries offset */
	header.entries_offset = current_offset;

	/* Write placeholder dict entries */
	{
		TpDictEntry placeholder;

		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
		{
			BufFileWrite(file, &placeholder, sizeof(TpDictEntry));
			current_offset += sizeof(TpDictEntry);
		}
	}

	/* Postings start here */
	header.postings_offset = current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(TermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/* Streaming pass: write posting blocks */
	for (i = 0; i < num_terms; i++)
	{
		TpExpullReader reader;
		uint32		   doc_count;
		uint32		   block_idx;
		uint32		   num_blocks;

		term_blocks[i].posting_offset	= current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		doc_count				= terms[i].expull->num_entries;
		term_blocks[i].doc_freq = terms[i].doc_freq;

		if (doc_count == 0)
		{
			term_blocks[i].block_count = 0;
			continue;
		}

		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
		term_blocks[i].block_count = (uint16)num_blocks;

		tp_expull_reader_init(&reader, ctx->arena, terms[i].expull);

		for (block_idx = 0; block_idx < num_blocks; block_idx++)
		{
			TpExpullEntry  entries[TP_BLOCK_SIZE];
			TpBlockPosting block_postings[TP_BLOCK_SIZE];
			TpSkipEntry	   skip;
			uint32		   nread;
			uint32		   j;
			uint16		   max_tf	  = 0;
			uint8		   min_norm	  = 255;
			uint32		   last_docid = 0;

			nread = tp_expull_reader_read(&reader, entries, TP_BLOCK_SIZE);
			Assert(nread > 0);

			for (j = 0; j < nread; j++)
			{
				block_postings[j].doc_id	= entries[j].doc_id;
				block_postings[j].frequency = entries[j].frequency;
				block_postings[j].fieldnorm = entries[j].fieldnorm;
				block_postings[j].reserved	= 0;

				if (entries[j].doc_id > last_docid)
					last_docid = entries[j].doc_id;
				if (entries[j].frequency > max_tf)
					max_tf = entries[j].frequency;
				if (entries[j].fieldnorm < min_norm)
					min_norm = entries[j].fieldnorm;
			}

			skip.last_doc_id	= last_docid;
			skip.doc_count		= (uint8)nread;
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = min_norm;
			skip.posting_offset = current_offset;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			if (tp_compress_segments)
			{
				uint8  compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];
				uint32 compressed_size;

				compressed_size =
						tp_compress_block(block_postings, nread, compressed);
				skip.flags = TP_BLOCK_FLAG_DELTA;
				BufFileWrite(file, compressed, compressed_size);
				current_offset += compressed_size;
			}
			else
			{
				skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
				BufFileWrite(
						file, block_postings, nread * sizeof(TpBlockPosting));
				current_offset += nread * sizeof(TpBlockPosting);
			}

			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;
		}
	}

	/* Write skip index */
	header.skip_index_offset = current_offset;
	if (skip_entries_count > 0)
	{
		BufFileWrite(
				file,
				all_skip_entries,
				skip_entries_count * sizeof(TpSkipEntry));
		current_offset += skip_entries_count * sizeof(TpSkipEntry);
	}

	/* Write fieldnorm table */
	header.fieldnorm_offset = current_offset;
	if (ctx->num_docs > 0)
	{
		BufFileWrite(file, ctx->fieldnorms, ctx->num_docs * sizeof(uint8));
		current_offset += ctx->num_docs * sizeof(uint8);
	}

	/* Write CTID pages array */
	header.ctid_pages_offset = current_offset;
	{
		uint32 *ctid_pages = palloc(ctx->num_docs * sizeof(uint32));

		for (i = 0; i < ctx->num_docs; i++)
			ctid_pages[i] = ItemPointerGetBlockNumber(&ctx->ctids[i]);
		BufFileWrite(file, ctid_pages, ctx->num_docs * sizeof(uint32));
		current_offset += ctx->num_docs * sizeof(uint32);
		pfree(ctid_pages);
	}

	/* Write CTID offsets array */
	header.ctid_offsets_offset = current_offset;
	{
		uint16 *ctid_offsets = palloc(ctx->num_docs * sizeof(uint16));

		for (i = 0; i < ctx->num_docs; i++)
			ctid_offsets[i] = ItemPointerGetOffsetNumber(&ctx->ctids[i]);
		BufFileWrite(file, ctid_offsets, ctx->num_docs * sizeof(uint16));
		current_offset += ctx->num_docs * sizeof(uint16);
		pfree(ctid_offsets);
	}

	/* Final data_size */
	header.data_size = current_offset;

	/*
	 * Seek back to write dict entries and header with real offsets.
	 *
	 * Save the end position first (BufFileWrite may have crossed a
	 * 1GB file segment boundary), then seek back to early positions
	 * within this segment (guaranteed to be in the same 1GB segment
	 * since the header/dict entries are near the start), then
	 * restore the end position.
	 */
	{
		TpDictEntry *dict_entries;
		int			 end_fileno;
		off_t		 end_file_offset;

		/* Save end position */
		BufFileTell(file, &end_fileno, &end_file_offset);

		dict_entries = palloc(num_terms * sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
		{
			dict_entries[i].skip_index_offset =
					header.skip_index_offset +
					((uint64)term_blocks[i].skip_entry_start *
					 sizeof(TpSkipEntry));
			dict_entries[i].block_count = term_blocks[i].block_count;
			dict_entries[i].reserved	= 0;
			dict_entries[i].doc_freq	= term_blocks[i].doc_freq;
		}

		/* Seek back to dict entries position */
		{
			uint64 base_abs =
					tp_buffile_composite_offset(base_fileno, base_file_offset);
			int	  dict_fileno;
			off_t dict_offset;

			tp_buffile_decompose_offset(
					base_abs + header.entries_offset,
					&dict_fileno,
					&dict_offset);
			BufFileSeek(file, dict_fileno, dict_offset, SEEK_SET);
		}
		BufFileWrite(file, dict_entries, num_terms * sizeof(TpDictEntry));
		pfree(dict_entries);

		/* Seek back to header position */
		BufFileSeek(file, base_fileno, base_file_offset, SEEK_SET);
		BufFileWrite(file, &header, sizeof(TpSegmentHeader));

		/* Restore end position for caller's next write */
		BufFileSeek(file, end_fileno, end_file_offset, SEEK_SET);
	}

	/* Cleanup */
	pfree(term_blocks);
	pfree(all_skip_entries);
	pfree(string_offsets);
	pfree(terms);

	return current_offset;
}

/*
 * Reset the build context for reuse.
 */
void
tp_build_context_reset(TpBuildContext *ctx)
{
	if (ctx == NULL)
		return;

	/* Reset arena (frees all pages except first) */
	tp_arena_reset(ctx->arena);

	/* Destroy and recreate hash table */
	hash_destroy(ctx->terms_ht);
	{
		HASHCTL info;

		memset(&info, 0, sizeof(info));
		info.keysize   = sizeof(char *);
		info.entrysize = sizeof(TpBuildTermEntry);
		info.hash	   = build_term_hash;
		info.match	   = build_term_match;
		ctx->terms_ht  = hash_create(
				 "build_terms",
				 16384,
				 &info,
				 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
	}

	/* Reset document arrays (keep allocated memory) */
	ctx->num_docs  = 0;
	ctx->total_len = 0;
}

/*
 * Destroy the build context.
 */
void
tp_build_context_destroy(TpBuildContext *ctx)
{
	if (ctx == NULL)
		return;

	tp_arena_destroy(ctx->arena);
	hash_destroy(ctx->terms_ht);
	pfree(ctx->fieldnorms);
	pfree(ctx->ctids);
	pfree(ctx);
}
