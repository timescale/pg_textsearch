/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

/*-------------------------------------------------------------------------
 *
 * dump.c
 *    Unified index dump functionality for pg_textsearch
 *
 * IDENTIFICATION
 *    src/dump.c
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <catalog/namespace.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/lsyscache.h>

#include "dump.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "metapage.h"
#include "segment/segment.h"
#include "state.h"

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
	uint64			   segment_tokens  = 0;
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
					segment_tokens += header->total_tokens;
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
					"  Total: %d segments, %lu pages (%.1fMB), "
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
