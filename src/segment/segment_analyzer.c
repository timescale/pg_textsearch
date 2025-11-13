/*
 * segment_analyzer.c
 *    Segment analysis and dumping functionality for pg_textsearch
 *
 * This file provides functions to analyze and dump the contents of
 * BM25 index segments and memtables to text files for debugging and
 * troubleshooting. It shares code with the segment reader used during
 * query processing to ensure consistency.
 */

#include "postgres.h"
#include "access/genam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"

#include "segment/segment.h"
#include "state.h"
#include "memtable/memtable.h"
#include "memtable/stringtable.h"
#include "metapage.h"

/* Forward declaration */
static BlockNumber tp_get_first_segment(Relation index);

/*
 * Analyze and dump index contents to file
 * Includes both memtable and segment information
 */
void
tp_analyze_index_to_file(const char *index_name, const char *filename)
{
	FILE			  *fp;
	Relation		   index;
	Oid				   index_oid;
	Oid				   namespace_oid;
	TpLocalIndexState *state;
	BlockNumber		   segment_root;

	/* Open output file for writing (truncate existing) */
	fp = fopen(filename, "w");
	if (!fp)
		elog(ERROR, "Could not open file %s for writing: %m", filename);

	fprintf(fp,
			"================================================================="
			"===============\n");
	fprintf(fp, "BM25 Index Analysis Report\n");
	fprintf(fp, "Generated: %s\n", timestamptz_to_str(GetCurrentTimestamp()));
	fprintf(fp, "Index: %s\n", index_name);
	fprintf(fp,
			"================================================================="
			"===============\n\n");

	/* Find the index OID */
	namespace_oid = LookupNamespaceNoError("public");
	if (namespace_oid == InvalidOid)
	{
		fprintf(fp, "ERROR: Schema 'public' not found\n");
		fclose(fp);
		return;
	}

	index_oid = get_relname_relid(index_name, namespace_oid);
	if (index_oid == InvalidOid)
	{
		fprintf(fp, "ERROR: Index '%s' not found\n", index_name);
		fclose(fp);
		return;
	}

	/* Open the index */
	index = index_open(index_oid, AccessShareLock);

	/* Get index state */
	state = tp_get_local_index_state(index);
	if (!state)
	{
		fprintf(fp, "ERROR: Could not get index state\n");
		index_close(index, AccessShareLock);
		fclose(fp);
		return;
	}

	/* Dump memtable information */
	fprintf(fp, "=== MEMTABLE ===\n");
	tp_dump_memtable_to_file(fp, state);
	fprintf(fp, "\n");

	/* Check for segments */
	segment_root = tp_get_first_segment(index);
	if (segment_root != InvalidBlockNumber)
	{
		fprintf(fp, "=== SEGMENTS ===\n");
		tp_dump_segment_chain_to_file(fp, index, segment_root);
	}
	else
	{
		fprintf(fp, "=== SEGMENTS ===\n");
		fprintf(fp, "No segments exist for this index\n");
	}

	/* Close resources */
	index_close(index, AccessShareLock);
	fclose(fp);
}

/*
 * Dump memtable contents to file
 */
void
tp_dump_memtable_to_file(FILE *fp, TpLocalIndexState *state)
{
	TpStringHashEntry **terms;
	uint32				num_terms;
	uint32				i;

	if (!state)
	{
		fprintf(fp, "Memtable: State not initialized\n");
		return;
	}

	fprintf(fp, "Memtable Statistics:\n");
	fprintf(fp, "  DSA Handle: %zu\n", (size_t)dsa_get_handle(state->dsa));
	fprintf(fp,
			"  String Table Memory: %zu bytes\n",
			state->string_table_memory);
	fprintf(fp, "  Posting List Memory: %zu bytes\n", state->posting_memory);
	fprintf(fp,
			"  Total Memory Used: %zu bytes\n",
			state->string_table_memory + state->posting_memory);
	fprintf(fp, "\n");

	/* Get sorted terms from string table */
	terms = tp_string_table_get_sorted_entries(state, &num_terms);
	if (!terms)
	{
		fprintf(fp, "Memtable Terms: None\n");
		return;
	}

	fprintf(fp, "Memtable Terms: %u\n", num_terms);
	fprintf(fp, "Format: [index] 'term' (doc_freq=n)\n");
	fprintf(fp, "---------------------------------------------------\n");

	for (i = 0; i < num_terms; i++)
	{
		TpStringHashEntry *entry		= terms[i];
		TpPostingList	  *posting_list = NULL;

		if (entry->posting_list_dp != InvalidDsaPointer)
		{
			posting_list = (TpPostingList *)
					dsa_get_address(state->dsa, entry->posting_list_dp);
		}

		fprintf(fp,
				"[%04u] '%-30s' (doc_freq=%u)\n",
				i,
				entry->term,
				posting_list ? posting_list->doc_count : 0);

		/* Show first few postings for the first 5 terms */
		if (posting_list && posting_list->doc_count > 0 && i < 5)
		{
			uint32 j;
			fprintf(fp, "       Postings: ");
			for (j = 0; j < Min(posting_list->doc_count, 3); j++)
			{
				fprintf(fp,
						"(%u,%u):%u ",
						BlockIdGetBlockNumber(
								&posting_list->postings[j].ctid.ip_blkid),
						posting_list->postings[j].ctid.ip_posid,
						posting_list->postings[j].term_frequency);
			}
			if (posting_list->doc_count > 3)
				fprintf(fp, "...");
			fprintf(fp, "\n");
		}
	}

	pfree(terms);
}

/*
 * Dump segment chain to file using the segment reader
 */
void
tp_dump_segment_chain_to_file(
		FILE *fp, Relation index, BlockNumber first_segment)
{
	BlockNumber current		= first_segment;
	int			segment_num = 0;

	while (current != InvalidBlockNumber)
	{
		TpSegmentReader *reader;
		TpSegmentHeader *header;

		fprintf(fp, "\nSegment %d (Block %u):\n", segment_num++, current);
		fprintf(fp, "---------------------------------------------------\n");

		/* Open segment using the standard reader */
		reader = tp_segment_open(index, current);
		if (!reader)
		{
			fprintf(fp,
					"ERROR: Could not open segment at block %u\n",
					current);
			break;
		}

		header = reader->header;

		/* Dump header information */
		fprintf(fp, "Header:\n");
		fprintf(fp,
				"  Magic: 0x%08X %s\n",
				header->magic,
				header->magic == TP_SEGMENT_MAGIC ? "(valid)" : "(INVALID)");
		fprintf(fp, "  Version: %u\n", header->version);
		fprintf(fp, "  Created: %s\n", timestamptz_to_str(header->created_at));
		fprintf(fp, "  Level: %u\n", header->level);
		fprintf(fp, "  Next Segment: %u\n", header->next_segment);
		fprintf(fp, "\n");

		fprintf(fp, "Statistics:\n");
		fprintf(fp, "  Terms: %u\n", header->num_terms);
		fprintf(fp, "  Documents: %u\n", header->num_docs);
		fprintf(fp,
				"  Total Tokens: %llu\n",
				(unsigned long long)header->total_tokens);
		fprintf(fp, "\n");

		fprintf(fp, "Physical Layout:\n");
		fprintf(fp, "  Pages Used: %u\n", header->num_pages);
		fprintf(fp, "  Data Size: %u bytes\n", header->data_size);
		fprintf(fp, "  Page Index Block: %u\n", header->page_index);
		fprintf(fp, "\n");

		fprintf(fp, "Logical Sections:\n");
		fprintf(fp, "  Dictionary: offset=%u\n", header->dictionary_offset);
		fprintf(fp,
				"  Strings: offset=%u (size=%u bytes)\n",
				header->strings_offset,
				header->entries_offset - header->strings_offset);
		fprintf(fp,
				"  Entries: offset=%u (size=%u bytes)\n",
				header->entries_offset,
				header->postings_offset - header->entries_offset);
		fprintf(fp,
				"  Postings: offset=%u (size=%u bytes)\n",
				header->postings_offset,
				header->doc_lengths_offset - header->postings_offset);
		fprintf(fp,
				"  Doc Lengths: offset=%u (size=%u bytes)\n",
				header->doc_lengths_offset,
				header->data_size - header->doc_lengths_offset);
		fprintf(fp, "\n");

		/* Dump page map */
		fprintf(fp, "Page Map (logical -> physical):\n");
		if (reader->page_map && reader->num_pages > 0)
		{
			uint32 i;
			for (i = 0; i < reader->num_pages; i++)
			{
				uint32 start_offset = i * BLCKSZ;
				uint32 end_offset	= Min((i + 1) * BLCKSZ, header->data_size);
				fprintf(fp,
						"  Page %u: Block %u (offsets %u-%u)\n",
						i,
						reader->page_map[i],
						start_offset,
						end_offset);
			}
		}
		fprintf(fp, "\n");

		/* Dump dictionary terms using the reader */
		if (header->num_terms > 0)
		{
			tp_dump_segment_dictionary_to_file(fp, reader);
		}

		/* Move to next segment */
		current = header->next_segment;

		/* Close reader */
		tp_segment_close(reader);
	}
}

/*
 * Dump segment dictionary using the segment reader
 */
void
tp_dump_segment_dictionary_to_file(FILE *fp, TpSegmentReader *reader)
{
	TpDictionary dict_header;
	uint32		*string_offsets;
	uint32		 i;
	uint32		 max_terms;

	fprintf(fp, "Dictionary Terms:\n");

	/* Read dictionary header */
	tp_segment_read(
			reader,
			reader->header->dictionary_offset,
			&dict_header,
			sizeof(dict_header.num_terms));

	fprintf(fp, "  Number of terms: %u\n", dict_header.num_terms);

	if (dict_header.num_terms == 0)
		return;

	/* Allocate and read string offsets */
	string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
	tp_segment_read(
			reader,
			reader->header->dictionary_offset + sizeof(dict_header.num_terms),
			string_offsets,
			sizeof(uint32) * dict_header.num_terms);

	/* Show first 20 terms and last 5 terms if more than 25 total */
	max_terms = Min(dict_header.num_terms, 25);

	fprintf(fp, "  Terms (showing first %u):\n", max_terms);
	for (i = 0; i < max_terms; i++)
	{
		char		  term_text[256];
		TpStringEntry string_entry;
		TpDictEntry	  dict_entry;
		uint32		  string_offset;

		/* Read dictionary entry */
		tp_segment_read(
				reader,
				reader->header->entries_offset + i * sizeof(TpDictEntry),
				&dict_entry,
				sizeof(TpDictEntry));

		/* Read string length */
		string_offset = reader->header->strings_offset + string_offsets[i];
		tp_segment_read(
				reader, string_offset, &string_entry.length, sizeof(uint32));

		/* Read string text */
		if (string_entry.length > 0 && string_entry.length < sizeof(term_text))
		{
			tp_segment_read(
					reader,
					string_offset + sizeof(uint32),
					term_text,
					string_entry.length);
			term_text[string_entry.length] = '\0';
		}
		else
		{
			snprintf(
					term_text,
					sizeof(term_text),
					"<invalid length %u>",
					string_entry.length);
		}

		fprintf(fp,
				"    [%04u] '%-30s' (doc_freq=%u, idf=%.4f)\n",
				i,
				term_text,
				dict_entry.doc_freq,
				dict_entry.idf);
	}

	if (dict_header.num_terms > 25)
	{
		fprintf(fp, "  ... (%u more terms) ...\n", dict_header.num_terms - 25);
	}

	pfree(string_offsets);
}

/*
 * Get the first segment block from the metapage
 */
static BlockNumber
tp_get_first_segment(Relation index)
{
	TpIndexMetaPage metap;
	BlockNumber		first_segment = InvalidBlockNumber;

	metap = tp_get_metapage(index);
	if (metap)
	{
		first_segment = metap->first_segment;
		pfree(metap);
	}

	return first_segment;
}
