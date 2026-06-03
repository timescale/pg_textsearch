/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * dump.c - Index dump and debugging utilities
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <catalog/namespace.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/lsyscache.h>

#include "access/am.h"
#include "debug/dump.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "index/source.h"
#include "index/state.h"
#include "memtable/chain_source.h"
#include "memtable/page.h"
#include "segment/io.h"
#include "segment/segment.h"

/* Output size limits for string mode */
#define MAX_OUTPUT_SIZE	 (256 * 1024) /* 256KB soft limit */
#define MAX_DOCS_TO_SHOW 20			  /* Records to show in full */

/* Forward declaration - implemented in segment.c */
extern void tp_dump_segment_to_output(
		Relation index, BlockNumber segment_root, DumpOutput *out);

/*
 * Dump memtable contents (v2 on-disk chain).
 *
 * Walks the on-disk page chain rooted at the metapage's
 * memtable_head_blkno field and prints per-page summaries plus
 * a per-document CTID + doc_length listing.  In v2 there is no
 * in-memory term dictionary or posting list — those are
 * reconstructed by the chain source at query time.  The TpVector
 * payload on each record contains the terms; we expose it via
 * vector_len here for sizing visibility but do not decode it
 * (see bm25_dump_index summarize section for term counts).
 */
static void
dump_memtable(DumpOutput *out, TpLocalIndexState *index_state, Relation rel)
{
	TpIndexMetaPage metap;
	BlockNumber		blk;
	uint32			page_count = 0;
	uint32			rec_count  = 0;
	uint32			docs_shown = 0;
	int				max_docs;

	(void)index_state;

	if (rel == NULL)
	{
		dump_printf(out, "Memtable Pages:\n  (relation not open)\n");
		return;
	}

	metap = tp_get_metapage(rel);
	if (metap == NULL || !BlockNumberIsValid(metap->memtable_head_blkno))
	{
		dump_printf(out, "Memtable Pages:\n  (empty)\n");
		if (metap)
			pfree(metap);
		return;
	}

	max_docs = out->full_dump ? INT_MAX : MAX_DOCS_TO_SHOW;
	dump_printf(out, "Memtable Pages:\n");

	for (blk = metap->memtable_head_blkno; BlockNumberIsValid(blk);)
	{
		Buffer		buf;
		Page		page;
		BlockNumber next;
		int			n;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBuffer(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (!tp_memtable_page_is_valid(page))
		{
			dump_printf(out, "  block %u: INVALID (missing magic)\n", blk);
			UnlockReleaseBuffer(buf);
			break;
		}

		n	 = tp_memtable_page_n_records(page);
		next = tp_memtable_page_get_next(page);
		page_count++;

		dump_printf(out, "  block %u: n_records=%d, next=%u\n", blk, n, next);

		if (!out->full_dump && out->str &&
			out->str->len > (int)MAX_OUTPUT_SIZE)
		{
			rec_count += n;
			UnlockReleaseBuffer(buf);
			blk = next;
			continue;
		}

		{
			TpMemtableRecord *rec = tp_memtable_page_first(page);
			while (rec != NULL)
			{
				rec_count++;
				if (docs_shown < (uint32)max_docs)
				{
					dump_printf(
							out,
							"    CTID (%u,%u): doc_length=%d, "
							"vector_len=%u\n",
							BlockIdGetBlockNumber(&rec->ctid.ip_blkid),
							rec->ctid.ip_posid,
							rec->doc_length,
							rec->vector_len);
					docs_shown++;
				}
				rec = tp_memtable_page_next(page, rec);
			}
		}

		UnlockReleaseBuffer(buf);
		blk = next;
	}

	if (docs_shown < rec_count)
		dump_printf(
				out,
				"  ... (showing %u of %u records)\n",
				docs_shown,
				rec_count);

	dump_printf(out, "Total memtable pages: %u\n", page_count);
	dump_printf(out, "Total memtable records: %u\n", rec_count);

	pfree(metap);
}

/*
 * Summarize index statistics without dumping content
 */
void
tp_summarize_index_to_output(const char *index_name, DumpOutput *out)
{
	Oid				   index_oid;
	Relation		   index_rel = NULL;
	TpIndexMetaPage	   metap	 = NULL;
	TpLocalIndexState *index_state;
	uint32			   memtable_terms = 0;
	uint32			   memtable_docs  = 0;
	int				   segment_count  = 0;
	uint32			   segment_terms  = 0;
	uint32			   segment_docs	  = 0;
	uint32			   segment_alive  = 0;
	bool			   acquired_lock  = false;

	dump_printf(out, "Index: %s\n", index_name);

	index_oid = tp_resolve_index_name_shared(index_name);
	if (!OidIsValid(index_oid))
	{
		dump_printf(out, "ERROR: Index '%s' not found\n", index_name);
		return;
	}

	/* Open the index */
	index_rel = index_open(index_oid, AccessShareLock);

	/* Get the metapage */
	metap = tp_get_metapage(index_rel);

	/* Get index state */
	index_state = tp_get_local_index_state(index_oid);
	if (index_state == NULL)
	{
		dump_printf(
				out,
				"ERROR: Could not get index state for '%s'\n",
				index_name);
		if (metap)
			pfree(metap);
		if (index_rel)
			index_close(index_rel, AccessShareLock);
		return;
	}

	/*
	 * Issue #404: the segment walks below open pages by block number from
	 * metap->level_heads[], which races a concurrent spill/merge that
	 * frees and recycles those blocks ("invalid segment header"). Hold the
	 * per-index lock SHARED and re-read the metapage under it. On error the
	 * end-of-xact LWLockReleaseAll plus state cleanup reset the lock.
	 */
	if (index_state != NULL && !index_state->lock_held)
	{
		tp_acquire_index_lock(index_state, LW_SHARED);
		acquired_lock = true;
	}
	if (index_state != NULL)
	{
		TpIndexMetaPage fresh = tp_get_metapage(index_rel);

		if (metap)
			pfree(metap);
		metap = fresh;
	}
	/*
	 * Corpus statistics.
	 *
	 * docs_persisted / len_persisted come from the metapage and
	 * are the durable source of truth: Σ segment.num_docs and
	 * Σ segment.total_len respectively (see metapage.h).
	 *
	 * total_docs / total_len reported here are the live in-flight
	 * totals (persisted + on-disk memtable chain), computed by
	 * adding a fresh chain-source open onto the metapage values.
	 * Tests that need a deterministic post-merge / post-vacuum
	 * corpus count should grep for docs_persisted / len_persisted
	 * instead.
	 */
	{
		TpDataSource *chain_src = tp_memtable_chain_source_create(
				index_state, index_rel, NULL, 0);
		uint64 metap_docs = metap ? metap->total_docs : 0;
		uint64 metap_len  = metap ? metap->total_len : 0;
		uint64 chain_docs = chain_src ? (uint64)chain_src->total_docs : 0;
		uint64 chain_len  = chain_src ? (uint64)chain_src->total_len : 0;
		uint64 total_docs = metap_docs + chain_docs;
		uint64 total_len  = metap_len + chain_len;

		if (chain_src)
			tp_source_close(chain_src);

		dump_printf(out, "\nCorpus Statistics:\n");
		dump_printf(out, "  total_docs: " UINT64_FORMAT "\n", total_docs);
		dump_printf(out, "  total_len: " UINT64_FORMAT "\n", total_len);
		if (metap)
		{
			dump_printf(
					out,
					"  docs_persisted: " UINT64_FORMAT "\n",
					metap->total_docs);
			dump_printf(
					out,
					"  len_persisted: " UINT64_FORMAT "\n",
					metap->total_len);
		}

		if (total_docs > 0)
		{
			float avg_doc_len = (float)total_len / (float)total_docs;
			dump_printf(out, "  avg_doc_len: %.2f\n", avg_doc_len);
		}
	}

	/* BM25 parameters */
	dump_printf(out, "\nBM25 Parameters:\n");
	if (metap)
	{
		dump_printf(out, "  k1: %.2f\n", metap->k1);
		dump_printf(out, "  b: %.2f\n", metap->b);
	}

	/* Memory usage */
	if (index_state->dsa)
	{
		size_t dsa_total_size = dsa_get_total_size(index_state->dsa);
		dump_printf(out, "\nMemory Usage:\n");
		dump_printf(
				out,
				"  DSA total size: %zu bytes (%.2f MB)\n",
				dsa_total_size,
				(double)dsa_total_size / (1024.0 * 1024.0));
	}

	/* Count memtable documents from the on-disk page chain (v2). */
	memtable_terms = 0;
	memtable_docs  = 0;
	if (metap && BlockNumberIsValid(metap->memtable_head_blkno))
	{
		BlockNumber blk = metap->memtable_head_blkno;

		while (BlockNumberIsValid(blk))
		{
			Buffer buf;
			Page   page;

			CHECK_FOR_INTERRUPTS();
			buf = ReadBuffer(index_rel, blk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (tp_memtable_page_is_valid(page))
			{
				memtable_docs += tp_memtable_page_n_records(page);
				blk = tp_memtable_page_get_next(page);
			}
			else
			{
				/* Should not happen on a healthy chain. */
				blk = InvalidBlockNumber;
			}
			UnlockReleaseBuffer(buf);
		}
	}

	dump_printf(out, "\nMemtable:\n");
	dump_printf(out, "  terms: %u\n", memtable_terms);
	dump_printf(out, "  documents: %u\n", memtable_docs);

	/* Segment summary by level */
	dump_printf(out, "\nSegments:\n");
	{
		int	   level;
		uint64 segment_pages = 0;
		bool   has_segments	 = false;

		for (level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber current_segment;
			int			level_segment_count = 0;

			if (!metap || metap->level_heads[level] == InvalidBlockNumber)
				continue;

			has_segments	= true;
			current_segment = metap->level_heads[level];

			while (current_segment != InvalidBlockNumber)
			{
				TpSegmentReader *reader;

				CHECK_FOR_INTERRUPTS();

				reader = tp_segment_open(index_rel, current_segment);
				if (reader && reader->header)
				{
					TpSegmentHeader *header = reader->header;
					Size			 seg_size;

					segment_count++;
					level_segment_count++;
					segment_terms += header->num_terms;
					segment_docs += header->num_docs;
					segment_alive += header->alive_count;
					segment_pages += header->num_pages;
					seg_size = (Size)header->num_pages * BLCKSZ;

					if (header->alive_count < header->num_docs)
						dump_printf(
								out,
								"  L%d Segment %d: block=%u, "
								"pages=%u, size=%.1fMB, "
								"terms=%u, docs=%u "
								"(alive=%u, dead=%u)\n",
								level,
								level_segment_count,
								current_segment,
								header->num_pages,
								(double)seg_size / (1024.0 * 1024.0),
								header->num_terms,
								header->num_docs,
								header->alive_count,
								header->num_docs - header->alive_count);
					else
						dump_printf(
								out,
								"  L%d Segment %d: block=%u, "
								"pages=%u, size=%.1fMB, "
								"terms=%u, docs=%u\n",
								level,
								level_segment_count,
								current_segment,
								header->num_pages,
								(double)seg_size / (1024.0 * 1024.0),
								header->num_terms,
								header->num_docs);

					current_segment = header->next_segment;
					tp_segment_close(reader);
				}
				else
				{
					current_segment = InvalidBlockNumber;
				}
			}
		}

		if (has_segments)
		{
			if (segment_alive < segment_docs)
				dump_printf(
						out,
						"  Total: %d segments, " UINT64_FORMAT
						" pages (%.1fMB), "
						"%u terms, %u docs "
						"(alive=%u, dead=%u)\n",
						segment_count,
						segment_pages,
						(double)(segment_pages * BLCKSZ) / (1024.0 * 1024.0),
						segment_terms,
						segment_docs,
						segment_alive,
						segment_docs - segment_alive);
			else
				dump_printf(
						out,
						"  Total: %d segments, " UINT64_FORMAT
						" pages (%.1fMB), "
						"%u terms, %u docs\n",
						segment_count,
						segment_pages,
						(double)(segment_pages * BLCKSZ) / (1024.0 * 1024.0),
						segment_terms,
						segment_docs);
		}
		else
		{
			dump_printf(out, "  (none)\n");
		}
	}

	/* Index size */
	dump_printf(out, "\nIndex Size:\n");
	dump_printf(
			out,
			"  on-disk: %lu bytes\n",
			(unsigned long)RelationGetNumberOfBlocks(index_rel) * BLCKSZ);

	/* Cleanup */
	if (acquired_lock)
		tp_release_index_lock(index_state);
	if (metap)
		pfree(metap);
	if (index_rel)
		index_close(index_rel, AccessShareLock);
}

/*
 * Main dump function - dumps entire index to output
 */
void
tp_dump_index_to_output(const char *index_name, DumpOutput *out)
{
	Oid				   index_oid;
	Relation		   index_rel = NULL;
	TpIndexMetaPage	   metap	 = NULL;
	TpLocalIndexState *index_state;
	bool			   acquired_lock = false;

	dump_printf(out, "Tapir Index Debug: %s\n", index_name);

	index_oid = tp_resolve_index_name_shared(index_name);
	if (!OidIsValid(index_oid))
	{
		dump_printf(out, "ERROR: Index '%s' not found\n", index_name);
		return;
	}

	/* Open the index */
	index_rel = index_open(index_oid, AccessShareLock);

	/* Get the metapage */
	metap = tp_get_metapage(index_rel);

	/* Get index state */
	index_state = tp_get_local_index_state(index_oid);
	if (index_state == NULL)
	{
		dump_printf(
				out,
				"ERROR: Could not get index state for '%s'\n",
				index_name);
		if (metap)
			pfree(metap);
		if (index_rel)
			index_close(index_rel, AccessShareLock);
		return;
	}

	/*
	 * Issue #404: hold the per-index lock (SHARED) and re-read the
	 * metapage under it before walking segments below; see the matching
	 * comment in tp_summarize_index_to_output.
	 */
	if (index_state != NULL && !index_state->lock_held)
	{
		tp_acquire_index_lock(index_state, LW_SHARED);
		acquired_lock = true;
	}
	if (index_state != NULL)
	{
		TpIndexMetaPage fresh = tp_get_metapage(index_rel);

		if (metap)
			pfree(metap);
		metap = fresh;
	}

	/*
	 * Corpus statistics.  See the comment in
	 * tp_summarize_index_to_output above for the docs_persisted /
	 * len_persisted rationale (metapage = durable Σ).  total_docs
	 * / total_len here include any in-flight on-disk memtable
	 * chain entries via a fresh chain-source open.
	 */
	{
		TpDataSource *chain_src = tp_memtable_chain_source_create(
				index_state, index_rel, NULL, 0);
		uint64 metap_docs = metap ? metap->total_docs : 0;
		uint64 metap_len  = metap ? metap->total_len : 0;
		uint64 chain_docs = chain_src ? (uint64)chain_src->total_docs : 0;
		uint64 chain_len  = chain_src ? (uint64)chain_src->total_len : 0;
		uint64 total_docs = metap_docs + chain_docs;
		uint64 total_len  = metap_len + chain_len;

		if (chain_src)
			tp_source_close(chain_src);

		dump_printf(out, "Corpus Statistics:\n");
		dump_printf(out, "  total_docs: " UINT64_FORMAT "\n", total_docs);
		dump_printf(out, "  total_len: " UINT64_FORMAT "\n", total_len);
		if (metap)
		{
			dump_printf(
					out,
					"  docs_persisted: " UINT64_FORMAT "\n",
					metap->total_docs);
			dump_printf(
					out,
					"  len_persisted: " UINT64_FORMAT "\n",
					metap->total_len);
		}

		if (total_docs > 0)
		{
			float avg_doc_len = (float)total_len / (float)total_docs;
			dump_printf(out, "  avg_doc_len: %.4f\n", avg_doc_len);
		}
		else
		{
			dump_printf(out, "  avg_doc_len: 0 (no documents)\n");
		}
	}

	/* DSA memory */
	if (index_state->dsa)
	{
		size_t dsa_total_size = dsa_get_total_size(index_state->dsa);
		dump_printf(out, "Memory Usage:\n");
		dump_printf(
				out,
				"  DSA total size: %zu bytes (%.2f MB)\n",
				dsa_total_size,
				(double)dsa_total_size / (1024.0 * 1024.0));
	}

	/* BM25 parameters */
	dump_printf(out, "BM25 Parameters:\n");
	if (metap)
	{
		dump_printf(out, "  k1: %.2f\n", metap->k1);
		dump_printf(out, "  b: %.2f\n", metap->b);

		dump_printf(out, "Metapage Info:\n");
		dump_printf(out, "  magic: 0x%08X\n", metap->magic);
		dump_printf(out, "  version: %u\n", metap->version);
		dump_printf(
				out,
				"  memtable_head_blkno: %u\n",
				metap->memtable_head_blkno);
		dump_printf(
				out,
				"  memtable_tail_blkno: %u\n",
				metap->memtable_tail_blkno);
	}

	/* Memtable contents */
	dump_memtable(out, index_state, index_rel);

	/* Detailed segment dump (first 2 per level) */
	{
		int		  level;
		int		  total_segments = 0;
		int		  dumped_count	 = 0;
		const int max_dump		 = 2;
		bool	  has_segments	 = false;

		/* First count total segments across all levels */
		for (level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber current_segment;

			if (!metap || metap->level_heads[level] == InvalidBlockNumber)
				continue;

			has_segments	= true;
			current_segment = metap->level_heads[level];

			while (current_segment != InvalidBlockNumber)
			{
				TpSegmentReader *reader;

				reader = tp_segment_open(index_rel, current_segment);
				if (reader && reader->header)
				{
					total_segments++;
					current_segment = reader->header->next_segment;
					tp_segment_close(reader);
				}
				else
				{
					current_segment = InvalidBlockNumber;
				}
			}
		}

		/* Now dump first N segments from each level */
		for (level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber current_segment;
			int			level_dumped = 0;

			if (!metap || metap->level_heads[level] == InvalidBlockNumber)
				continue;

			current_segment = metap->level_heads[level];

			while (current_segment != InvalidBlockNumber &&
				   level_dumped < max_dump)
			{
				TpSegmentReader *reader;

				CHECK_FOR_INTERRUPTS();

				dump_printf(out, "\nL%d ", level);
				tp_dump_segment_to_output(index_rel, current_segment, out);
				dumped_count++;
				level_dumped++;

				/* Read next_segment from header to traverse the chain */
				reader = tp_segment_open(index_rel, current_segment);
				if (reader && reader->header)
				{
					current_segment = reader->header->next_segment;
					tp_segment_close(reader);
				}
				else
				{
					current_segment = InvalidBlockNumber;
				}
			}
		}

		if (total_segments > dumped_count)
		{
			dump_printf(
					out,
					"\n... %d more segments not shown\n",
					total_segments - dumped_count);
		}

		if (!has_segments)
		{
			dump_printf(out, "\nNo segments written yet\n");
		}
	}

	/* Cleanup */
	if (acquired_lock)
		tp_release_index_lock(index_state);
	if (metap)
		pfree(metap);
	if (index_rel)
		index_close(index_rel, AccessShareLock);
}

/*
 * tp_dump_index - Debug function to show internal index structure
 * including both memtable and all segments
 *
 * Takes index name and optional filename. If filename is provided,
 * writes full dump to file (no truncation, includes hex dumps).
 * Otherwise returns truncated output as text.
 */
PG_FUNCTION_INFO_V1(tp_dump_index);

Datum
tp_dump_index(PG_FUNCTION_ARGS)
{
	text *index_name_text = PG_GETARG_TEXT_PP(0);
	char *index_name	  = text_to_cstring(index_name_text);

	/* All dump functions require superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to dump index")));

	/* Check for optional filename parameter */
#ifdef DEBUG_DUMP_INDEX
	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
	{
		/* File mode - full dump with hex */
		text *filename_text = PG_GETARG_TEXT_PP(1);
		char *filename		= text_to_cstring(filename_text);
		FILE *fp;

		fp = fopen(filename, "w");
		if (!fp)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", filename)));
		}

		{
			DumpOutput out;
			dump_init_file(&out, fp);
			tp_dump_index_to_output(index_name, &out);
		}

		fclose(fp);

		elog(INFO, "Index dump written to %s", filename);
		PG_RETURN_TEXT_P(cstring_to_text_with_len(filename, strlen(filename)));
	}
	else
#endif
	{
		/* String mode - truncated output for SQL return */
		StringInfoData result;
		DumpOutput	   out;

		initStringInfo(&result);
		dump_init_string(&out, &result);
		tp_dump_index_to_output(index_name, &out);

		PG_RETURN_TEXT_P(cstring_to_text(result.data));
	}
}

/*
 * tp_summarize_index - Show index statistics without dumping content
 *
 * A faster alternative to bm25_dump_index that shows only statistics:
 * corpus statistics, BM25 parameters, memory usage, memtable/segment counts.
 */
PG_FUNCTION_INFO_V1(tp_summarize_index);

Datum
tp_summarize_index(PG_FUNCTION_ARGS)
{
	text		  *index_name_text = PG_GETARG_TEXT_PP(0);
	char		  *index_name	   = text_to_cstring(index_name_text);
	StringInfoData result;
	DumpOutput	   out;

	/* Superuser only */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to summarize index")));

	initStringInfo(&result);
	dump_init_string(&out, &result);
	tp_summarize_index_to_output(index_name, &out);

	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*
 * Page visualization support - only available in debug builds.
 * These functions write to arbitrary file paths, so they are gated
 * behind DEBUG_DUMP_INDEX to reduce attack surface in release builds.
 */
#ifdef DEBUG_DUMP_INDEX

/*
 * Page map entry - tracks what each page is used for
 */
typedef struct PageMapEntry
{
	uint8 level;	   /* Segment level (0-7), or 255 for special */
	uint8 seg_in_lvl;  /* Segment number within level */
	uint8 page_type;   /* See PAGE_* constants below */
	uint8 data_region; /* See DATA_* constants below */
} PageMapEntry;

/* Page types */
#define PAGE_UNUSED	   0 /* Empty/free page */
#define PAGE_METAPAGE  1 /* Index metapage (block 0) */
#define PAGE_SEG_HDR   3 /* Segment header */
#define PAGE_SEG_INDEX 4 /* Segment page index */
#define PAGE_SEG_DATA  5 /* Segment data page */

/* Data regions within segment (for PAGE_SEG_DATA) */
#define DATA_DICTIONARY 0 /* Dictionary + strings + entries */
#define DATA_POSTING	1 /* Posting lists (compressed blocks) */
#define DATA_SKIP		2 /* Skip index entries */
#define DATA_DOCMAP		3 /* Fieldnorms + CTID mapping */

/* ANSI color codes - 256-color mode for segment backgrounds */
#define ANSI_RESET "\033[0m"
#define ANSI_DIM   "\033[2m"

/*
 * Segment background colors (256-color palette)
 * Format: \033[48;5;Nm for background color N
 * Using distinct, visible colors that work with black text
 */
static const int segment_bg_colors[] = {
		196, /* Red */
		46,	 /* Green */
		33,	 /* Blue */
		226, /* Yellow */
		201, /* Magenta */
		51,	 /* Cyan */
		208, /* Orange */
		141, /* Light purple */
		118, /* Lime */
		213, /* Pink */
		75,	 /* Sky blue */
		220, /* Gold */
		177, /* Violet */
		119, /* Light green */
		209, /* Salmon */
		147, /* Light blue */
};
#define NUM_SEGMENT_COLORS \
	(sizeof(segment_bg_colors) / sizeof(segment_bg_colors[0]))

/* Segment info for legend */
typedef struct SegmentInfo
{
	int			level;
	int			seg_in_level;
	int			global_idx; /* Index into segment_colors */
	uint32		num_pages;
	BlockNumber root_block;
} SegmentInfo;

/*
 * Get character for page type/region
 * Returns single character mnemonic (space for postings to reduce clutter)
 */
static char
get_page_char(PageMapEntry *e)
{
	switch (e->page_type)
	{
	case PAGE_UNUSED:
		return '.';
	case PAGE_METAPAGE:
		return 'M';
	case PAGE_SEG_HDR:
		return 'H'; /* Header */
	case PAGE_SEG_INDEX:
		return 'i'; /* Index */
	case PAGE_SEG_DATA:
		switch (e->data_region)
		{
		case DATA_DICTIONARY:
			return 'd';
		case DATA_POSTING:
			return ' '; /* Blank - most common, reduces clutter */
		case DATA_SKIP:
			return 's';
		case DATA_DOCMAP:
			return 'm';
		default:
			return '?';
		}
	default:
		return '?';
	}
}

/*
 * Mark the metapage in the page map.
 */
static void
mark_special_pages(
		Relation		index_rel,
		TpIndexMetaPage metap,
		PageMapEntry   *page_map,
		BlockNumber		total_blocks)
{
	(void)index_rel;
	(void)metap;
	(void)total_blocks;

	/* Mark metapage */
	page_map[0].page_type = PAGE_METAPAGE;
	page_map[0].level	  = 255;
}

/*
 * Count segments in the index.
 */
static int
count_segments(Relation index_rel, TpIndexMetaPage metap)
{
	int count = 0;
	int level;

	if (!metap)
		return 0;

	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg_root = metap->level_heads[level];

		while (seg_root != InvalidBlockNumber)
		{
			TpSegmentReader *reader = tp_segment_open(index_rel, seg_root);

			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			count++;
			seg_root = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

	return count;
}

/*
 * Collect segment info for the legend.
 * Caller must allocate segments array with enough space.
 */
static void
collect_segment_info(
		Relation index_rel, TpIndexMetaPage metap, SegmentInfo *segments)
{
	int num_segments = 0;
	int level;

	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg_root;
		int			seg_in_level = 0;

		if (!metap || metap->level_heads[level] == InvalidBlockNumber)
			continue;

		seg_root = metap->level_heads[level];

		while (seg_root != InvalidBlockNumber)
		{
			TpSegmentReader *reader;

			reader = tp_segment_open(index_rel, seg_root);
			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			segments[num_segments].level		= level;
			segments[num_segments].seg_in_level = seg_in_level;
			segments[num_segments].global_idx	= num_segments;
			segments[num_segments].num_pages	= reader->num_pages;
			segments[num_segments].root_block	= seg_root;

			num_segments++;
			seg_in_level++;
			seg_root = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}
}

/*
 * Classify a segment page by its data region based on byte offset.
 */
static void
classify_segment_page(
		PageMapEntry	*entry,
		TpSegmentReader *reader,
		uint32			 page_idx,
		int				 global_idx,
		int				 seg_in_level)
{
	uint64 page_start_offset;

	entry->level	  = global_idx;
	entry->seg_in_lvl = seg_in_level;

	/* First byte offset of this logical page */
	page_start_offset = (uint64)page_idx * (BLCKSZ - SizeOfPageHeaderData);

	if (page_idx == 0)
	{
		entry->page_type = PAGE_SEG_HDR;
	}
	else if (page_start_offset < reader->header->postings_offset)
	{
		entry->page_type   = PAGE_SEG_DATA;
		entry->data_region = DATA_DICTIONARY;
	}
	else if (page_start_offset < reader->header->skip_index_offset)
	{
		entry->page_type   = PAGE_SEG_DATA;
		entry->data_region = DATA_POSTING;
	}
	else if (page_start_offset < reader->header->fieldnorm_offset)
	{
		entry->page_type   = PAGE_SEG_DATA;
		entry->data_region = DATA_SKIP;
	}
	else
	{
		entry->page_type   = PAGE_SEG_DATA;
		entry->data_region = DATA_DOCMAP;
	}
}

/*
 * Mark page index pages for a segment.
 */
static void
mark_page_index_pages(
		Relation		 index_rel,
		TpSegmentReader *reader,
		PageMapEntry	*page_map,
		BlockNumber		 total_blocks,
		int				 global_idx,
		int				 seg_in_level)
{
	BlockNumber		   pi_blk;
	TpPageIndexSpecial pi_special;
	int				   pi_count = 0;

	if (reader->header->page_index == InvalidBlockNumber)
		return;

	pi_blk = reader->header->page_index;

	while (pi_blk != InvalidBlockNumber && pi_blk < total_blocks &&
		   pi_count < 1000)
	{
		Buffer buf;
		Page   page;

		page_map[pi_blk].level		= global_idx;
		page_map[pi_blk].seg_in_lvl = seg_in_level;
		page_map[pi_blk].page_type	= PAGE_SEG_INDEX;

		buf = ReadBuffer(index_rel, pi_blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		memcpy(&pi_special,
			   PageGetSpecialPointer(page),
			   sizeof(TpPageIndexSpecial));
		UnlockReleaseBuffer(buf);

		pi_blk = pi_special.next_page;
		pi_count++;
	}
}

/*
 * Mark all segment pages in the page map (second pass).
 */
static void
mark_segment_pages(
		Relation		index_rel,
		TpIndexMetaPage metap,
		PageMapEntry   *page_map,
		BlockNumber		total_blocks,
		SegmentInfo	   *segments,
		int				num_segments)
{
	int level;

	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg_root;
		int			seg_in_level = 0;

		if (!metap || metap->level_heads[level] == InvalidBlockNumber)
			continue;

		seg_root = metap->level_heads[level];

		while (seg_root != InvalidBlockNumber)
		{
			TpSegmentReader *reader;
			uint32			 i;
			int				 global_idx = -1;

			reader = tp_segment_open(index_rel, seg_root);
			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			/* Find global index for this segment */
			for (i = 0; i < (uint32)num_segments; i++)
			{
				if (segments[i].level == level &&
					segments[i].seg_in_level == seg_in_level)
				{
					global_idx = i;
					break;
				}
			}

			/* Mark data pages */
			for (i = 0; i < reader->num_pages; i++)
			{
				BlockNumber phys_page = reader->page_map[i];

				if (phys_page < total_blocks)
				{
					classify_segment_page(
							&page_map[phys_page],
							reader,
							i,
							global_idx,
							seg_in_level);
				}
			}

			/* Mark page index pages */
			mark_page_index_pages(
					index_rel,
					reader,
					page_map,
					total_blocks,
					global_idx,
					seg_in_level);

			seg_in_level++;
			seg_root = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}
}

/* Page counts structure for statistics */
typedef struct PageCounts
{
	uint32 empty;
	uint32 header;
	uint32 dict;
	uint32 post;
	uint32 skip;
	uint32 docmap;
	uint32 idx;
} PageCounts;

/*
 * Count pages by type.
 */
static void
count_page_types(
		PageMapEntry *page_map, BlockNumber total_blocks, PageCounts *counts)
{
	BlockNumber blk;

	memset(counts, 0, sizeof(PageCounts));

	for (blk = 0; blk < total_blocks; blk++)
	{
		PageMapEntry *e = &page_map[blk];

		switch (e->page_type)
		{
		case PAGE_UNUSED:
			counts->empty++;
			break;
		case PAGE_METAPAGE:
			/* Metapage counted separately, not in output */
			break;
		case PAGE_SEG_HDR:
			counts->header++;
			break;
		case PAGE_SEG_INDEX:
			counts->idx++;
			break;
		case PAGE_SEG_DATA:
			switch (e->data_region)
			{
			case DATA_DICTIONARY:
				counts->dict++;
				break;
			case DATA_POSTING:
				counts->post++;
				break;
			case DATA_SKIP:
				counts->skip++;
				break;
			case DATA_DOCMAP:
				counts->docmap++;
				break;
			}
			break;
		}
	}
}

/*
 * Write the header and legend to the output file.
 */
static void
write_pageviz_header(
		FILE		*fp,
		const char	*index_name,
		BlockNumber	 total_blocks,
		SegmentInfo *segments,
		int			 num_segments,
		PageCounts	*counts)
{
	int current_level = -1;
	int i;

	fprintf(fp, "# Page Visualization: %s\n", index_name);
	fprintf(fp,
			"# Total: %u pages (%.1f MB), %d segments\n",
			total_blocks,
			(double)total_blocks * BLCKSZ / (1024.0 * 1024.0),
			num_segments);
	fprintf(fp, "#\n");

	/* Segment legend organized by level */
	fprintf(fp, "# Segments (background color indicates segment):\n");

	for (i = 0; i < num_segments; i++)
	{
		SegmentInfo *seg = &segments[i];
		int			 bg	 = segment_bg_colors[i % NUM_SEGMENT_COLORS];
		double size_mb	 = (double)seg->num_pages * BLCKSZ / (1024.0 * 1024.0);

		if (seg->level != current_level)
		{
			if (current_level >= 0)
				fprintf(fp, "\n");
			fprintf(fp, "#   L%d: ", seg->level);
			current_level = seg->level;
		}
		else
		{
			fprintf(fp, "  ");
		}

		fprintf(fp,
				"\033[48;5;%dm S%d " ANSI_RESET " (%u pg, %.1fMB)",
				bg,
				i,
				seg->num_pages,
				size_mb);
	}
	fprintf(fp, "\n");

	fprintf(fp, "#\n");
	fprintf(fp, "# Special: \033[48;5;15m\033[30mM" ANSI_RESET "=metapage\n");
	fprintf(fp,
			"# Letters: H=header d=dictionary (blank)=postings "
			"s=skip m=docmap i=idx .=empty\n");
	fprintf(fp, "#\n");
	fprintf(fp,
			"# Page counts: empty=%u header=%u dict=%u post=%u "
			"skip=%u docmap=%u idx=%u\n",
			counts->empty,
			counts->header,
			counts->dict,
			counts->post,
			counts->skip,
			counts->docmap,
			counts->idx);
	fprintf(fp, "#\n");
}

/*
 * Write the page map visualization.
 */
static void
write_pageviz_map(
		FILE		 *fp,
		PageMapEntry *page_map,
		BlockNumber	  total_blocks,
		PageCounts	 *counts)
{
	BlockNumber blk;
	int			col = 0;

	for (blk = 0; blk < total_blocks; blk++)
	{
		PageMapEntry *e = &page_map[blk];
		char		  c = get_page_char(e);

		switch (e->page_type)
		{
		case PAGE_UNUSED:
			fprintf(fp, ANSI_DIM "%c" ANSI_RESET, c);
			break;
		case PAGE_METAPAGE:
			fprintf(fp, "\033[48;5;15m\033[30m%c" ANSI_RESET, c);
			break;
		case PAGE_SEG_HDR:
		case PAGE_SEG_INDEX:
		case PAGE_SEG_DATA:
		{
			int bg = segment_bg_colors[e->level % NUM_SEGMENT_COLORS];
			fprintf(fp, "\033[48;5;%dm%c" ANSI_RESET, bg, c);
			break;
		}
		default:
			fprintf(fp, "%c", c);
			break;
		}

		col++;
		if (col >= 128)
		{
			fprintf(fp, "\n");
			col = 0;
		}
	}

	if (col > 0)
		fprintf(fp, "\n");

	/* Summary line */
	{
		uint32 used = total_blocks - counts->empty;
		fprintf(fp,
				"# Used: %u  Empty: %u  Utilization: %.1f%%\n",
				used,
				counts->empty,
				100.0 * used / total_blocks);
	}
}

/*
 * Dump page visualization to file - main orchestrator.
 *
 * Output format:
 * - Legend at top showing segments organized by level with assigned colors
 * - Letter mnemonics indicate page type:
 *   H=header d=dictionary (blank)=postings s=skip m=docmap i=idx .=empty
 * - Background colors distinguish segments (16-color palette, cycling)
 * - Line breaks every 128 characters
 */
static void
tp_debug_pageviz_to_file(const char *index_name, const char *filename)
{
	Oid				index_oid;
	Relation		index_rel	 = NULL;
	TpIndexMetaPage metap		 = NULL;
	BlockNumber		total_blocks = 0;
	PageMapEntry   *page_map	 = NULL;
	SegmentInfo	   *segments	 = NULL;
	int				num_segments = 0;
	PageCounts		counts;
	FILE		   *fp;

	/* Open output file */
	fp = fopen(filename, "w");
	if (!fp)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));
	}

	/* Resolve index */
	index_oid = tp_resolve_index_name_shared(index_name);
	if (!OidIsValid(index_oid))
	{
		fprintf(fp, "ERROR: Index '%s' not found\n", index_name);
		fclose(fp);
		return;
	}

	/* Open index and get metapage */
	index_rel	 = index_open(index_oid, AccessShareLock);
	total_blocks = RelationGetNumberOfBlocks(index_rel);
	metap		 = tp_get_metapage(index_rel);

	if (total_blocks == 0)
	{
		fprintf(fp, "ERROR: Index has no blocks\n");
		goto cleanup;
	}

	/* Allocate and build page map */
	page_map = palloc0(total_blocks * sizeof(PageMapEntry));

	/* Count and allocate segments array */
	num_segments = count_segments(index_rel, metap);
	if (num_segments > 0)
		segments = palloc(num_segments * sizeof(SegmentInfo));

	mark_special_pages(index_rel, metap, page_map, total_blocks);
	if (num_segments > 0)
		collect_segment_info(index_rel, metap, segments);
	mark_segment_pages(
			index_rel, metap, page_map, total_blocks, segments, num_segments);

	/* Count and write output */
	count_page_types(page_map, total_blocks, &counts);
	write_pageviz_header(
			fp, index_name, total_blocks, segments, num_segments, &counts);
	write_pageviz_map(fp, page_map, total_blocks, &counts);

cleanup:
	if (segments)
		pfree(segments);
	if (page_map)
		pfree(page_map);
	if (metap)
		pfree(metap);
	if (index_rel)
		index_close(index_rel, AccessShareLock);
	fclose(fp);
}

/*
 * tp_debug_pageviz - SQL function to dump page visualization
 *
 * Outputs ANSI-colored page layout (128 chars per line):
 *   H=header d=dictionary (blank)=postings s=skip m=docmap i=idx .=empty
 * Background colors distinguish segments (16-color palette)
 *
 * Only available in debug builds (compile with -DDEBUG_DUMP_INDEX)
 * because it writes to arbitrary file paths.
 */
PG_FUNCTION_INFO_V1(tp_debug_pageviz);

Datum
tp_debug_pageviz(PG_FUNCTION_ARGS)
{
	text *index_name_text = PG_GETARG_TEXT_PP(0);
	text *filename_text	  = PG_GETARG_TEXT_PP(1);
	char *index_name	  = text_to_cstring(index_name_text);
	char *filename		  = text_to_cstring(filename_text);

	/* Superuser only - writes to arbitrary file path */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to write page visualization")));

	tp_debug_pageviz_to_file(index_name, filename);

	elog(INFO, "Page visualization written to %s", filename);
	PG_RETURN_TEXT_P(cstring_to_text_with_len(filename, strlen(filename)));
}

#endif /* DEBUG_DUMP_INDEX */
