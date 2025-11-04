/*-------------------------------------------------------------------------
 *
 * segment/segment_size.c
 *    Segment size estimation for pg_textsearch
 *
 * IDENTIFICATION
 *    src/segment/segment_size.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "../memtable/memtable.h"
#include "../memtable/posting.h"
#include "../memtable/stringtable.h"
#include "../metapage.h"
#include "../state.h"
#include "lib/dshash.h"
#include "segment.h"

/*
 * Estimate the size needed for a segment based on memtable contents
 */
uint32
tp_segment_estimate_size(TpLocalIndexState *state, Relation index)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	dshash_seq_status  status;
	TpStringHashEntry *entry;
	uint32			   total_size		= 0;
	uint32			   num_terms		= 0;
	uint32			   total_postings	= 0;
	uint32			   total_string_len = 0;
	TpIndexMetaPage	   metap;

	/* Get memtable from shared state */
	memtable = (TpMemtable *)
			dsa_get_address(state->dsa, state->shared->memtable_dp);
	if (!memtable)
		return BLCKSZ; /* Minimum size */

	/* Attach to string hash table */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return BLCKSZ;

	string_table =
			tp_string_table_attach(state->dsa, memtable->string_hash_handle);
	if (!string_table)
		return BLCKSZ;

	/* Count terms and calculate sizes */
	dshash_seq_init(&status, string_table, false);
	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		const char	  *term	   = tp_get_key_str(state->dsa, &entry->key);
		TpPostingList *posting = NULL;

		if (!term)
			continue;

		if (entry->key.posting_list != InvalidDsaPointer)
		{
			posting = (TpPostingList *)
					dsa_get_address(state->dsa, entry->key.posting_list);
		}

		if (posting && posting->doc_count > 0)
		{
			num_terms++;
			total_string_len += strlen(term);
			total_postings += posting->doc_count;
		}
	}
	dshash_seq_term(&status);
	dshash_detach(string_table);

	/* Calculate size components */
	/* 1. Header */
	total_size += sizeof(TpSegmentHeader);

	/* 2. Dictionary */
	total_size += sizeof(TpDictionary);
	total_size += num_terms * sizeof(uint32);	   /* string offsets */
	total_size += num_terms * sizeof(TpDictEntry); /* dictionary entries */

	/* 3. Strings section */
	/* Each string: length(4) + text + dict_offset(4) */
	total_size += num_terms * (sizeof(uint32) * 2); /* overhead per string */
	total_size += total_string_len;					/* actual string data */

	/* 4. Postings section */
	total_size += total_postings * sizeof(TpSegmentPosting);

	/* 5. Document lengths section */
	metap = tp_get_metapage(index);
	if (metap)
	{
		total_size += metap->total_docs * sizeof(TpDocLength);
		pfree(metap);
	}

	/* 6. Page index (1 entry per data page) */
	uint32 estimated_pages = (total_size + BLCKSZ - 1) / BLCKSZ;
	/* Page index overhead: header + block numbers */
	total_size += sizeof(TpPageIndexSpecial);
	total_size += estimated_pages * sizeof(BlockNumber);

	/* Add 20% overhead for alignment and page headers */
	total_size = (uint32)(total_size * 1.2);

	/* Ensure minimum size */
	if (total_size < BLCKSZ)
		total_size = BLCKSZ;

	elog(DEBUG1,
		 "Segment size estimate: %u terms, %u postings, %u string bytes = %u "
		 "total bytes (%u pages)",
		 num_terms,
		 total_postings,
		 total_string_len,
		 total_size,
		 (total_size + BLCKSZ - 1) / BLCKSZ);

	return total_size;
}
