/*
 * segment_analyzer_simple.c
 *    Simplified segment analysis for pg_textsearch
 *
 * This provides a factual dump of segment contents directly from pages,
 * sharing code with the segment reader used during query processing.
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
#include "metapage.h"

/*
 * Analyze and dump index segments to file
 * Reads directly from pages and provides factual information
 */
void
tp_analyze_index_to_file(const char *index_name, const char *filename)
{
	FILE		   *fp;
	Relation		index;
	Oid				index_oid;
	Oid				namespace_oid;
	TpIndexMetaPage metap;
	BlockNumber		segment_root;

	/* Open output file for writing */
	fp = fopen(filename, "w");
	if (!fp)
		elog(ERROR, "Could not open file %s for writing: %m", filename);

	fprintf(fp,
			"================================================================="
			"===============\n");
	fprintf(fp, "BM25 Index Segment Analysis\n");
	fprintf(fp, "Generated: %s\n", timestamptz_to_str(GetCurrentTimestamp()));
	fprintf(fp, "Index: %s\n", index_name);
	fprintf(fp,
			"================================================================="
			"===============\n\n");

	/* Find the index */
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

	/* Get metapage information */
	metap = tp_get_metapage(index);
	if (metap)
	{
		fprintf(fp, "=== METAPAGE INFORMATION ===\n");
		fprintf(fp, "Magic: 0x%08X\n", metap->magic);
		fprintf(fp,
				"Total Documents: %llu\n",
				(unsigned long long)metap->total_docs);
		fprintf(fp,
				"Total Length: %llu\n",
				(unsigned long long)metap->total_len);
		fprintf(fp, "IDF Sum: %.4f\n", metap->idf_sum);
		fprintf(fp, "First Segment: Block %u\n", metap->first_segment);
		fprintf(fp, "First DocID Page: Block %u\n", metap->first_docid_page);
		fprintf(fp, "\n");

		segment_root = metap->first_segment;
		pfree(metap);
	}
	else
	{
		fprintf(fp, "ERROR: Could not read metapage\n");
		segment_root = InvalidBlockNumber;
	}

	/* Analyze segments if they exist */
	if (segment_root != InvalidBlockNumber)
	{
		fprintf(fp, "=== SEGMENTS ===\n");
		tp_analyze_segment_chain(fp, index, segment_root);
	}
	else
	{
		/* Check if there's a segment at block 2 (common location after
		 * spilling) */
		Buffer			 test_buf;
		Page			 test_page;
		TpSegmentHeader *test_header;
		bool			 found_segment = false;

		fprintf(fp, "=== SEGMENTS ===\n");
		fprintf(fp,
				"Note: Metapage shows no segment pointer, checking common "
				"locations...\n");

		/* Try block 2 (typical first segment location) */
		if (RelationGetNumberOfBlocks(index) > 2)
		{
			test_buf = ReadBuffer(index, 2);
			LockBuffer(test_buf, BUFFER_LOCK_SHARE);
			test_page = BufferGetPage(test_buf);

			test_header = (TpSegmentHeader *)((char *)test_page +
											  SizeOfPageHeaderData);
			if (test_header->magic == TP_SEGMENT_MAGIC)
			{
				fprintf(fp,
						"Found segment at block 2 (not linked from "
						"metapage)\n");
				UnlockReleaseBuffer(test_buf);
				tp_analyze_segment_chain(fp, index, 2);
				found_segment = true;
			}
			else
			{
				UnlockReleaseBuffer(test_buf);
			}
		}

		if (!found_segment)
		{
			fprintf(fp, "No segments found\n");
		}
	}

	/* Close resources */
	index_close(index, AccessShareLock);
	fclose(fp);
}

/*
 * Analyze segment chain using the segment reader
 */
void
tp_analyze_segment_chain(FILE *fp, Relation index, BlockNumber first_segment)
{
	BlockNumber current		= first_segment;
	int			segment_num = 0;

	while (current != InvalidBlockNumber)
	{
		TpSegmentReader *reader;
		TpSegmentHeader *header;

		fprintf(fp,
				"\n--- Segment %d (Block %u) ---\n",
				segment_num++,
				current);

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

		/* Header information - factual data from the page */
		fprintf(fp, "Header:\n");
		fprintf(fp, "  Magic: 0x%08X\n", header->magic);
		fprintf(fp, "  Version: %u\n", header->version);
		fprintf(fp, "  Created: %s\n", timestamptz_to_str(header->created_at));
		fprintf(fp, "  Level: %u\n", header->level);
		fprintf(fp, "  Next Segment: %u\n", header->next_segment);

		/* Statistics - directly from header */
		fprintf(fp, "Statistics:\n");
		fprintf(fp, "  Terms: %u\n", header->num_terms);
		fprintf(fp, "  Documents: %u\n", header->num_docs);
		fprintf(fp,
				"  Total Tokens: %llu\n",
				(unsigned long long)header->total_tokens);

		/* Physical layout - factual information */
		fprintf(fp, "Physical Layout:\n");
		fprintf(fp, "  Pages: %u\n", header->num_pages);
		fprintf(fp, "  Data Size: %u bytes\n", header->data_size);
		fprintf(fp, "  Page Index: Block %u\n", header->page_index);

		/* Section offsets - directly from header */
		fprintf(fp, "Section Offsets:\n");
		fprintf(fp, "  Dictionary: %u\n", header->dictionary_offset);
		fprintf(fp, "  Strings: %u\n", header->strings_offset);
		fprintf(fp, "  Entries: %u\n", header->entries_offset);
		fprintf(fp, "  Postings: %u\n", header->postings_offset);
		fprintf(fp, "  Doc Lengths: %u\n", header->doc_lengths_offset);

		/* Page map if available */
		if (reader->page_map && reader->num_pages > 0)
		{
			uint32 i;
			fprintf(fp, "Page Map:\n");
			for (i = 0; i < reader->num_pages; i++)
			{
				fprintf(fp,
						"  Logical Page %u -> Physical Block %u\n",
						i,
						reader->page_map[i]);
			}
		}

		/* Dictionary terms - read using the segment reader */
		if (header->num_terms > 0)
		{
			tp_analyze_dictionary(fp, reader);
		}

		/* Move to next segment */
		current = header->next_segment;

		/* Close reader */
		tp_segment_close(reader);
	}
}

/*
 * Analyze dictionary using the segment reader
 */
void
tp_analyze_dictionary(FILE *fp, TpSegmentReader *reader)
{
	TpDictionary dict_header;
	uint32		*string_offsets;
	uint32		 i;
	uint32		 terms_to_show;

	fprintf(fp, "Dictionary:\n");

	/* Read dictionary header */
	tp_segment_read(
			reader,
			reader->header->dictionary_offset,
			&dict_header,
			sizeof(dict_header.num_terms));

	fprintf(fp, "  Number of terms: %u\n", dict_header.num_terms);

	if (dict_header.num_terms == 0)
		return;

	/* Read string offsets */
	string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
	tp_segment_read(
			reader,
			reader->header->dictionary_offset + sizeof(dict_header.num_terms),
			string_offsets,
			sizeof(uint32) * dict_header.num_terms);

	/* Show first 20 terms */
	terms_to_show = Min(dict_header.num_terms, 20);
	fprintf(fp, "  First %u terms:\n", terms_to_show);

	for (i = 0; i < terms_to_show; i++)
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

		/* Read string */
		string_offset = reader->header->strings_offset + string_offsets[i];
		tp_segment_read(
				reader, string_offset, &string_entry.length, sizeof(uint32));

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
				"    [%04u] '%s' (doc_freq=%u, posting_offset=%u)\n",
				i,
				term_text,
				dict_entry.doc_freq,
				dict_entry.posting_offset);
	}

	if (dict_header.num_terms > terms_to_show)
	{
		fprintf(fp,
				"  ... %u more terms ...\n",
				dict_header.num_terms - terms_to_show);
	}

	pfree(string_offsets);
}
