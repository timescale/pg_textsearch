/*
 * Copyright (c) 2025 Tiger Data, Inc.
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

#include "am/am.h"
#include "debug/dump.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "segment/segment.h"
#include "state/metapage.h"
#include "state/state.h"

/* Output size limits for string mode */
#define MAX_OUTPUT_SIZE		  (256 * 1024) /* 256KB soft limit */
#define MAX_TERMS_FULL_DETAIL 20		   /* Show full posting lists */
#define MAX_TERMS_SUMMARY	  100		   /* Show just doc frequency */
#define MAX_DOCS_TO_SHOW	  10		   /* Doc length entries to show */
#define MAX_POSTINGS_SHOWN	  5			   /* Postings per term */

/* Forward declaration - implemented in segment.c */
extern void tp_dump_segment_to_output(
		Relation index, BlockNumber segment_root, DumpOutput *out);

/*
 * Dump memtable contents
 */
static void
dump_memtable(DumpOutput *out, TpLocalIndexState *index_state)
{
	TpMemtable *memtable = get_memtable(index_state);
	dsa_area   *area	 = index_state->dsa;

	dump_printf(out, "Term Dictionary:\n");

	if (memtable && memtable->string_hash_handle != DSHASH_HANDLE_INVALID &&
		area)
	{
		dshash_table *string_table =
				tp_string_table_attach(area, memtable->string_hash_handle);

		if (string_table)
		{
			uint32			   term_count  = 0;
			uint32			   terms_shown = 0;
			uint32			   max_terms_full;
			uint32			   max_terms_summary;
			dshash_seq_status  status;
			TpStringHashEntry *entry;

			max_terms_full	  = out->full_dump ? UINT32_MAX
											   : MAX_TERMS_FULL_DETAIL;
			max_terms_summary = out->full_dump ? UINT32_MAX
											   : MAX_TERMS_SUMMARY;

			dshash_seq_init(&status, string_table, false);

			while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) !=
				   NULL)
			{
				CHECK_FOR_INTERRUPTS();

				if (DsaPointerIsValid(entry->key.posting_list))
				{
					TpPostingList *posting_list;
					const char	  *stored_str;

					term_count++;

					/* Check output size limit */
					if (!out->full_dump && out->str &&
						out->str->len > (int)MAX_OUTPUT_SIZE)
					{
						/* Stop adding terms */
						continue;
					}

					posting_list =
							dsa_get_address(area, entry->key.posting_list);
					stored_str = tp_get_key_str(area, &entry->key);

					if (terms_shown < max_terms_full)
					{
						/* Full detail */
						TpPostingEntry *postings;
						int				max_show;
						int				i;

						dump_printf(
								out,
								"  '%s': doc_freq=%d, postings=",
								stored_str,
								posting_list->doc_count);

						postings = tp_get_posting_entries(area, posting_list);
						max_show = out->full_dump ? posting_list->doc_count
												  : MAX_POSTINGS_SHOWN;

						for (i = 0;
							 i < posting_list->doc_count && i < max_show;
							 i++)
						{
							if (i > 0)
								dump_printf(out, ",");
							dump_printf(
									out,
									"(%u,%u):%d",
									BlockIdGetBlockNumber(
											&postings[i].ctid.ip_blkid),
									postings[i].ctid.ip_posid,
									postings[i].frequency);
						}

						if (posting_list->doc_count > max_show)
						{
							dump_printf(
									out,
									"... (%d more)",
									posting_list->doc_count - max_show);
						}
						dump_printf(out, "\n");
					}
					else if (terms_shown < max_terms_summary)
					{
						dump_printf(
								out,
								"  '%s': doc_freq=%d\n",
								stored_str,
								posting_list->doc_count);
					}

					terms_shown++;
				}
			}

			dshash_seq_term(&status);
			dshash_detach(string_table);

			if (terms_shown < term_count)
			{
				dump_printf(
						out,
						"  ... showing %u of %u terms (output truncated)\n",
						terms_shown,
						term_count);
			}
			dump_printf(out, "Total terms: %u\n", term_count);
		}
		else
		{
			dump_printf(out, "  ERROR: Cannot attach to string hash table\n");
		}
	}
	else
	{
		dump_printf(out, "  No terms (string hash table not initialized)\n");
	}

	/* Document length hash table */
	dump_printf(out, "Document Length Hash Table:\n");
	if (memtable && memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID &&
		area)
	{
		dshash_table *doclength_table =
				tp_doclength_table_attach(area, memtable->doc_lengths_handle);

		if (doclength_table)
		{
			dshash_seq_status status;
			TpDocLengthEntry *entry;
			int				  total_count = 0;
			int				  shown_count = 0;
			int max_docs = out->full_dump ? INT_MAX : MAX_DOCS_TO_SHOW;

			dshash_seq_init(&status, doclength_table, false);

			while ((entry = (TpDocLengthEntry *)dshash_seq_next(&status)) !=
				   NULL)
			{
				CHECK_FOR_INTERRUPTS();

				total_count++;

				if (shown_count < max_docs)
				{
					dump_printf(
							out,
							"  CTID (%u,%u): doc_length=%d\n",
							BlockIdGetBlockNumber(&entry->ctid.ip_blkid),
							entry->ctid.ip_posid,
							entry->doc_length);
					shown_count++;
				}
			}

			if (shown_count < total_count)
				dump_printf(
						out,
						"  ... (showing %d of %d entries)\n",
						shown_count,
						total_count);

			dump_printf(
					out, "Total document length entries: %d\n", total_count);

			dshash_seq_term(&status);
			dshash_detach(doclength_table);
		}
		else
		{
			dump_printf(
					out,
					"  ERROR: Cannot attach to document length hash table\n");
		}
	}
	else
	{
		dump_printf(out, "  No document length table (not initialized)\n");
	}
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
	TpMemtable		  *memtable;
	dsa_area		  *area;
	uint32			   memtable_terms  = 0;
	uint32			   memtable_docs   = 0;
	int				   segment_count   = 0;
	uint32			   segment_terms   = 0;
	uint32			   segment_docs	   = 0;
	uint32			   recovery_pages  = 0;
	uint32			   recovery_docids = 0;

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

	/* Corpus statistics */
	dump_printf(out, "\nCorpus Statistics:\n");
	dump_printf(out, "  total_docs: %d\n", index_state->shared->total_docs);
	dump_printf(
			out, "  total_len: %ld\n", (long)index_state->shared->total_len);

	if (index_state->shared->total_docs > 0)
	{
		float avg_doc_len = (float)index_state->shared->total_len /
							(float)index_state->shared->total_docs;
		dump_printf(out, "  avg_doc_len: %.2f\n", avg_doc_len);
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

	/* Count memtable terms without iterating content */
	memtable = get_memtable(index_state);
	area	 = index_state->dsa;

	if (memtable && memtable->string_hash_handle != DSHASH_HANDLE_INVALID &&
		area)
	{
		dshash_table *string_table =
				tp_string_table_attach(area, memtable->string_hash_handle);
		if (string_table)
		{
			dshash_seq_status  status;
			TpStringHashEntry *entry;

			dshash_seq_init(&status, string_table, false);
			while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) !=
				   NULL)
			{
				CHECK_FOR_INTERRUPTS();
				if (DsaPointerIsValid(entry->key.posting_list))
					memtable_terms++;
			}
			dshash_seq_term(&status);
			dshash_detach(string_table);
		}
	}

	/* Count memtable documents */
	if (memtable && memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID &&
		area)
	{
		dshash_table *doclength_table =
				tp_doclength_table_attach(area, memtable->doc_lengths_handle);
		if (doclength_table)
		{
			dshash_seq_status status;
			TpDocLengthEntry *entry;

			dshash_seq_init(&status, doclength_table, false);
			while ((entry = (TpDocLengthEntry *)dshash_seq_next(&status)) !=
				   NULL)
			{
				CHECK_FOR_INTERRUPTS();
				memtable_docs++;
			}
			dshash_seq_term(&status);
			dshash_detach(doclength_table);
		}
	}

	dump_printf(out, "\nMemtable:\n");
	dump_printf(out, "  terms: %u\n", memtable_terms);
	dump_printf(out, "  documents: %u\n", memtable_docs);

	/* Count recovery pages */
	if (metap && metap->first_docid_page != InvalidBlockNumber)
	{
		Buffer			   docid_buf;
		Page			   docid_page;
		TpDocidPageHeader *docid_header;
		BlockNumber		   current_page = metap->first_docid_page;

		while (current_page != InvalidBlockNumber && recovery_pages < 10000)
		{
			CHECK_FOR_INTERRUPTS();

			docid_buf = ReadBuffer(index_rel, current_page);
			LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
			docid_page	 = BufferGetPage(docid_buf);
			docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

			if (docid_header->magic == TP_DOCID_PAGE_MAGIC)
			{
				recovery_docids += docid_header->num_docids;
				recovery_pages++;
				current_page = docid_header->next_page;
			}
			else
			{
				current_page = InvalidBlockNumber;
			}

			UnlockReleaseBuffer(docid_buf);
		}
	}

	dump_printf(out, "\nRecovery Pages:\n");
	dump_printf(out, "  pages: %u\n", recovery_pages);
	dump_printf(out, "  docids: %u\n", recovery_docids);

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
					segment_pages += header->num_pages;
					seg_size = (Size)header->num_pages * BLCKSZ;

					dump_printf(
							out,
							"  L%d Segment %d: block=%u, pages=%u, "
							"size=%.1fMB, terms=%u, docs=%u\n",
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
			dump_printf(
					out,
					"  Total: %d segments, " UINT64_FORMAT " pages (%.1fMB), "
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

	/* Corpus statistics */
	dump_printf(out, "Corpus Statistics:\n");
	dump_printf(out, "  total_docs: %d\n", index_state->shared->total_docs);
	dump_printf(
			out, "  total_len: %ld\n", (long)index_state->shared->total_len);

	if (index_state->shared->total_docs > 0)
	{
		float avg_doc_len = (float)index_state->shared->total_len /
							(float)index_state->shared->total_docs;
		dump_printf(out, "  avg_doc_len: %.4f\n", avg_doc_len);
	}
	else
	{
		dump_printf(out, "  avg_doc_len: 0 (no documents)\n");
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

		dump_printf(out, "Metapage Recovery Info:\n");
		dump_printf(out, "  magic: 0x%08X\n", metap->magic);
		dump_printf(out, "  first_docid_page: %u\n", metap->first_docid_page);
	}

	/* Memtable contents */
	dump_memtable(out, index_state);

	/* Crash recovery info */
	dump_printf(out, "Crash Recovery:\n");
	if (metap && metap->first_docid_page != InvalidBlockNumber)
	{
		Buffer			   docid_buf;
		Page			   docid_page;
		TpDocidPageHeader *docid_header;
		BlockNumber		   current_page = metap->first_docid_page;
		int				   page_count	= 0;
		int				   total_docids = 0;

		while (current_page != InvalidBlockNumber)
		{
			CHECK_FOR_INTERRUPTS();

			docid_buf = ReadBuffer(index_rel, current_page);
			LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
			docid_page	 = BufferGetPage(docid_buf);
			docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

			if (docid_header->magic == TP_DOCID_PAGE_MAGIC)
			{
				total_docids += docid_header->num_docids;
				page_count++;
				current_page = docid_header->next_page;
			}
			else
			{
				current_page = InvalidBlockNumber;
			}

			UnlockReleaseBuffer(docid_buf);

			if (page_count > 10000)
				break;
		}

		dump_printf(
				out, "  Pages: %d, Documents: %d\n", page_count, total_docids);
	}
	else
	{
		dump_printf(out, "  No recovery pages\n");
	}

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

	/* Check for optional filename parameter */
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

	initStringInfo(&result);
	dump_init_string(&out, &result);
	tp_summarize_index_to_output(index_name, &out);

	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*
 * Page map entry - tracks what each page is used for
 */
typedef struct PageMapEntry
{
	uint8 level;	   /* Segment level (0-7), or 255 for special */
	uint8 seg_in_lvl;  /* Segment number within level */
	uint8 page_type;   /* 0=unused, 1=header, 2=pageindex, 3=data */
	uint8 data_region; /* For data: 0=dict, 1=posting, 2=docmap, 3=skip */
} PageMapEntry;

#define PAGE_UNUSED	   0
#define PAGE_METAPAGE  1
#define PAGE_DOCID	   2
#define PAGE_SEG_HDR   3
#define PAGE_SEG_INDEX 4
#define PAGE_SEG_DATA  5

/* Data regions within segment */
#define DATA_DICTIONARY 0
#define DATA_POSTING	1
#define DATA_DOCMAP		2
#define DATA_SKIP		3

/* ANSI color codes */
#define ANSI_RESET	   "\033[0m"
#define ANSI_BOLD	   "\033[1m"
#define ANSI_DIM	   "\033[2m"
#define ANSI_FG_BLACK  "\033[30m"
#define ANSI_FG_WHITE  "\033[37m"
#define ANSI_FG_CYAN   "\033[36m"
#define ANSI_FG_YELLOW "\033[33m"
#define ANSI_FG_GREEN  "\033[32m"
#define ANSI_FG_MAGENT "\033[35m"
#define ANSI_FG_BLUE   "\033[34m"
#define ANSI_FG_RED	   "\033[31m"
#define ANSI_BG_CYAN   "\033[46m"
#define ANSI_BG_YELLOW "\033[43m"
#define ANSI_BG_GREEN  "\033[42m"
#define ANSI_BG_MAGENT "\033[45m"
#define ANSI_BG_BLUE   "\033[44m"
#define ANSI_BG_WHITE  "\033[47m"

/*
 * Get segment character: 0-9, a-z, A-Z
 */
static char
get_segment_char(int level, int seg_in_level)
{
	int idx = 0;
	int l;

	/* Compute global segment index across all levels */
	for (l = 0; l < level; l++)
		idx += 10; /* Rough estimate, actual count varies */

	idx = level * 10 + seg_in_level;

	if (idx < 10)
		return '0' + idx;
	else if (idx < 36)
		return 'a' + (idx - 10);
	else if (idx < 62)
		return 'A' + (idx - 36);
	else
		return '#';
}

/*
 * Get ANSI color for level
 */
static const char *
get_level_color(int level)
{
	switch (level)
	{
	case 0:
		return ANSI_FG_CYAN;
	case 1:
		return ANSI_FG_YELLOW;
	case 2:
		return ANSI_FG_GREEN;
	default:
		return ANSI_FG_MAGENT;
	}
}

/*
 * Dump page map visualization to file
 */
void
tp_dump_page_map_to_file(const char *index_name, const char *filename)
{
	Oid				index_oid;
	Relation		index_rel	 = NULL;
	TpIndexMetaPage metap		 = NULL;
	BlockNumber		total_blocks = 0;
	PageMapEntry   *page_map	 = NULL;
	FILE		   *fp;
	int				level;
	int				line_width = 80;
	BlockNumber		blk;
	int				seg_count = 0;

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

	/* Allocate page map */
	page_map = palloc0(total_blocks * sizeof(PageMapEntry));

	/* Mark metapage */
	page_map[0].page_type = PAGE_METAPAGE;

	/* Mark docid pages */
	if (metap && metap->first_docid_page != InvalidBlockNumber)
	{
		BlockNumber current = metap->first_docid_page;
		int			count	= 0;

		while (current != InvalidBlockNumber && current < total_blocks &&
			   count < 10000)
		{
			Buffer			   buf;
			Page			   page;
			TpDocidPageHeader *hdr;

			page_map[current].page_type = PAGE_DOCID;

			buf = ReadBuffer(index_rel, current);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			hdr	 = (TpDocidPageHeader *)PageGetContents(page);

			if (hdr->magic == TP_DOCID_PAGE_MAGIC)
				current = hdr->next_page;
			else
				current = InvalidBlockNumber;

			UnlockReleaseBuffer(buf);
			count++;
		}
	}

	/* Process all segments */
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
			uint32			 pages_per_blk;
			uint32			 dict_end_page;
			uint32			 posting_end_page;
			uint32			 skip_end_page;

			reader = tp_segment_open(index_rel, seg_root);
			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			/* Calculate logical page boundaries for different regions */
			pages_per_blk = BLCKSZ - MAXALIGN(SizeOfPageHeaderData);
			dict_end_page = (reader->header->postings_offset + pages_per_blk -
							 1) /
							pages_per_blk;
			posting_end_page = (reader->header->skip_index_offset +
								pages_per_blk - 1) /
							   pages_per_blk;
			skip_end_page = (reader->header->fieldnorm_offset + pages_per_blk -
							 1) /
							pages_per_blk;

			/* Mark all pages belonging to this segment */
			for (i = 0; i < reader->num_pages && i < reader->num_pages; i++)
			{
				BlockNumber phys_page = reader->page_map[i];

				if (phys_page >= total_blocks)
					continue;

				page_map[phys_page].level	   = level;
				page_map[phys_page].seg_in_lvl = seg_in_level;

				if (i == 0)
				{
					/* Header page */
					page_map[phys_page].page_type = PAGE_SEG_HDR;
				}
				else if (i < dict_end_page)
				{
					page_map[phys_page].page_type	= PAGE_SEG_DATA;
					page_map[phys_page].data_region = DATA_DICTIONARY;
				}
				else if (i < posting_end_page)
				{
					page_map[phys_page].page_type	= PAGE_SEG_DATA;
					page_map[phys_page].data_region = DATA_POSTING;
				}
				else if (i < skip_end_page)
				{
					page_map[phys_page].page_type	= PAGE_SEG_DATA;
					page_map[phys_page].data_region = DATA_SKIP;
				}
				else
				{
					page_map[phys_page].page_type	= PAGE_SEG_DATA;
					page_map[phys_page].data_region = DATA_DOCMAP;
				}
			}

			/* Mark page index pages */
			if (reader->header->page_index != InvalidBlockNumber)
			{
				BlockNumber		   pi_blk = reader->header->page_index;
				TpPageIndexSpecial pi_special;
				int				   pi_count = 0;

				while (pi_blk != InvalidBlockNumber && pi_blk < total_blocks &&
					   pi_count < 1000)
				{
					Buffer buf;
					Page   page;

					page_map[pi_blk].level		= level;
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

			seg_count++;
			seg_in_level++;
			seg_root = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

	/* Write header */
	fprintf(fp, "Page Map: %s\n", index_name);
	fprintf(fp,
			"Total pages: %u (%.1f MB)\n",
			total_blocks,
			(double)total_blocks * BLCKSZ / (1024.0 * 1024.0));
	fprintf(fp, "Segments: %d\n\n", seg_count);

	/* Write legend */
	fprintf(fp, "Legend:\n");
	fprintf(fp,
			"  " ANSI_BOLD "M" ANSI_RESET " = Metapage  " ANSI_FG_BLUE
			"D" ANSI_RESET " = Docid  " ANSI_DIM "." ANSI_RESET " = Empty\n");
	fprintf(fp, "  Segments: 0-9, a-z, A-Z (by level order)\n");
	fprintf(fp,
			"  Colors: " ANSI_FG_CYAN "L0" ANSI_RESET " " ANSI_FG_YELLOW
			"L1" ANSI_RESET " " ANSI_FG_GREEN "L2" ANSI_RESET
			" " ANSI_FG_MAGENT "L3+" ANSI_RESET "\n");
	fprintf(fp,
			"  " ANSI_BOLD "Bold" ANSI_RESET "=header  normal=data  " ANSI_DIM
			"dim" ANSI_RESET "=pageindex\n\n");

	/* Write page map */
	for (blk = 0; blk < total_blocks; blk++)
	{
		PageMapEntry *e = &page_map[blk];
		char		  c;
		const char	 *color_start = "";
		const char	 *color_end	  = ANSI_RESET;
		const char	 *style		  = "";

		switch (e->page_type)
		{
		case PAGE_UNUSED:
			c			= '.';
			color_start = ANSI_DIM;
			break;

		case PAGE_METAPAGE:
			c			= 'M';
			color_start = ANSI_BOLD;
			break;

		case PAGE_DOCID:
			c			= 'D';
			color_start = ANSI_FG_BLUE;
			break;

		case PAGE_SEG_HDR:
			c			= get_segment_char(e->level, e->seg_in_lvl);
			color_start = get_level_color(e->level);
			style		= ANSI_BOLD;
			break;

		case PAGE_SEG_INDEX:
			c			= get_segment_char(e->level, e->seg_in_lvl);
			color_start = get_level_color(e->level);
			style		= ANSI_DIM;
			break;

		case PAGE_SEG_DATA:
			c			= get_segment_char(e->level, e->seg_in_lvl);
			color_start = get_level_color(e->level);
			break;

		default:
			c			= '?';
			color_start = ANSI_FG_RED;
			break;
		}

		fprintf(fp, "%s%s%c%s", color_start, style, c, color_end);

		if ((blk + 1) % line_width == 0)
			fprintf(fp, "\n");
	}

	/* Final newline if needed */
	if (total_blocks % line_width != 0)
		fprintf(fp, "\n");

	/* Summary statistics */
	{
		uint32 used = 0, empty = 0;
		for (blk = 0; blk < total_blocks; blk++)
		{
			if (page_map[blk].page_type == PAGE_UNUSED)
				empty++;
			else
				used++;
		}
		fprintf(fp,
				"\nUsed: %u pages  Empty: %u pages  Utilization: %.1f%%\n",
				used,
				empty,
				100.0 * used / total_blocks);
	}

cleanup:
	if (page_map)
		pfree(page_map);
	if (metap)
		pfree(metap);
	if (index_rel)
		index_close(index_rel, AccessShareLock);
	fclose(fp);
}

/*
 * tp_dump_page_map - SQL function to dump page map visualization
 */
PG_FUNCTION_INFO_V1(tp_dump_page_map);

Datum
tp_dump_page_map(PG_FUNCTION_ARGS)
{
	text *index_name_text = PG_GETARG_TEXT_PP(0);
	text *filename_text	  = PG_GETARG_TEXT_PP(1);
	char *index_name	  = text_to_cstring(index_name_text);
	char *filename		  = text_to_cstring(filename_text);

	tp_dump_page_map_to_file(index_name, filename);

	elog(INFO, "Page map written to %s", filename);
	PG_RETURN_TEXT_P(cstring_to_text_with_len(filename, strlen(filename)));
}
