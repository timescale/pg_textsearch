/*-------------------------------------------------------------------------
 *
 * flush.c
 *	  Memtable to segment flush implementation for pg_textsearch
 *
 * IDENTIFICATION
 *	  src/flush.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <access/hash.h>
#include <lib/qunique.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/memutils.h>
#include <utils/timestamp.h>

#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "segment/segment.h"
#include "state.h"

/*
 * Term entry for sorting during flush
 */
typedef struct TermEntry
{
	const char	  *term;		 /* Term string */
	uint32		   term_len;	 /* Term length */
	uint32		   term_hash;	 /* Pre-computed hash */
	TpPostingList *posting_list; /* Posting list */
} TermEntry;

/*
 * Comparison function for sorting terms alphabetically
 */
static int
term_entry_compare(const void *a, const void *b)
{
	const TermEntry *ta = (const TermEntry *)a;
	const TermEntry *tb = (const TermEntry *)b;
	return strcmp(ta->term, tb->term);
}

/*
 * Helper to write data to pages with page mapping
 */
static void
write_segment_data(
		Relation	 index,
		BlockNumber *page_map,
		int			*page_count,
		uint32		*logical_offset,
		const void	*data,
		uint32		 len)
{
	uint32 remaining   = len;
	uint32 data_offset = 0;

	while (remaining > 0)
	{
		uint32 page_offset	 = *logical_offset % BLCKSZ;
		uint32 space_on_page = BLCKSZ - page_offset;
		uint32 to_write		 = Min(remaining, space_on_page);
		Buffer buf;
		Page   page;
		char  *dest;

		/* Need a new page? */
		if (page_offset == 0)
		{
			buf = ReadBuffer(index, P_NEW);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
			PageInit(page, BLCKSZ, 0);

			page_map[*page_count] = BufferGetBlockNumber(buf);
			(*page_count)++;
		}
		else
		{
			/* Continue on current page */
			buf = ReadBuffer(index, page_map[*page_count - 1]);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
		}

		/* Write data to page */
		dest = (char *)page + page_offset;
		memcpy(dest, (char *)data + data_offset, to_write);

		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);

		data_offset += to_write;
		*logical_offset += to_write;
		remaining -= to_write;
	}
}

/*
 * Estimate segment size based on memtable contents
 */
#if 0
/* TODO: Update for new segment structure */
uint32
tp_segment_estimate_size(TpLocalIndexState *state, Relation index)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	dshash_seq_status  status;
	TpStringHashEntry *entry;
	uint32			   total_size = 0;

	memtable = get_memtable(state);

	/* Attach to string table */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return 0;

	string_table =
			tp_string_table_attach(state->dsa, memtable->string_hash_handle);

	/* Count terms and estimate sizes */
	dshash_seq_init(&status, string_table, false);
	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		/* Get posting list directly from the entry we already have */
		TpPostingList *posting = NULL;
		if (entry->key.posting_list != InvalidDsaPointer)
		{
			posting = (TpPostingList *)
					dsa_get_address(state->dsa, entry->key.posting_list);
		}

		if (posting && posting->doc_count > 0)
		{
			/* Dictionary entry + string + posting list */
			total_size += sizeof(TpDictEntry);
			total_size += strlen(tp_get_key_str(state->dsa, &entry->key)) + 1;
			total_size += posting->doc_count * sizeof(TpSegmentPosting);
		}
	}
	dshash_seq_term(&status);

	/* Add header, document lengths, and overhead */
	{
		TpIndexMetaPage metap = tp_get_metapage(index);

		total_size += sizeof(TpSegmentHeader);
		total_size += metap->total_docs * sizeof(TpDocLength);
		total_size += BLCKSZ; /* Page overhead estimate */

		pfree(metap);
	}

	return total_size;
}
#endif

/*
 * Flush memtable to disk segment
 */
BlockNumber
tp_flush_memtable_to_segment(TpLocalIndexState *state, Relation index)
{
	/* TODO: Implement actual segment writing */
	return InvalidBlockNumber;
#if 0
	/* Old implementation - needs updating for new segment structure */
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	dshash_table	  *doc_lengths_table;
	dshash_seq_status  seq_status;
	TpStringHashEntry *string_entry;
	TpDocLengthEntry  *doc_entry;
	TermEntry		  *terms;
	TpDocLength		  *doc_lengths;
	TpDictEntry		  *dict_entries;
	BlockNumber		   root_block;
	BlockNumber		  *page_map;
	Buffer			   header_buf;
	Page			   header_page;
	TpSegmentHeader	  *header;
	TpIndexMetaPage	   metap;
	uint32			   logical_offset;
	uint32			   term_count = 0;
	uint32			   doc_count  = 0;
	uint32			   dict_size;
	uint32			   dict_offset;
	uint32			   dict_size_val;
	uint32			   postings_offset;
	uint32			   postings_size_val;
	uint32			   doclens_offset;
	uint32			   doclens_size_val;
	uint32			   strings_offset;
	uint32			   strings_size_val;
	uint32			  *posting_offsets;
	uint32			  *string_offsets;
	int				   page_count = 0;
	int				   max_pages;
	BlockNumber		   prev_first_segment;
	MemoryContext	   flush_context;
	MemoryContext	   old_context;

	/* Create a temporary memory context for flush operations */
	flush_context = AllocSetContextCreate(
			CurrentMemoryContext,
			"Segment Flush Context",
			ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(flush_context);

	/* Get memtable and tables */
	memtable = get_memtable(state);

	/* Attach to string table */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(flush_context);
		return InvalidBlockNumber;
	}

	string_table =
			tp_string_table_attach(state->dsa, memtable->string_hash_handle);

	/* Get document lengths table if it exists */
	if (memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
	{
		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(flush_context);
		return InvalidBlockNumber;
	}

	doc_lengths_table = tp_doclength_table_attach(
			state->dsa, memtable->doc_lengths_handle);

	/* No data to flush? Check metapage for document count */
	{
		TpIndexMetaPage metap	 = tp_get_metapage(index);
		bool			has_docs = (metap->total_docs > 0);

		pfree(metap);

		if (!has_docs)
		{
			MemoryContextSwitchTo(old_context);
			MemoryContextDelete(flush_context);
			return InvalidBlockNumber;
		}
	}

	/* Estimate pages needed */
	{
		uint32 estimated_size = tp_segment_estimate_size(state, index);
		max_pages			  = (estimated_size + BLCKSZ - 1) / BLCKSZ +
					2; /* Add safety margin */
	}
	page_map = palloc(max_pages * sizeof(BlockNumber));

	/* Collect and sort all terms */
	dshash_seq_init(&seq_status, string_table, false);
	while ((string_entry = (TpStringHashEntry *)dshash_seq_next(
					&seq_status)) != NULL)
	{
		/* Get posting list directly from the entry we already have */
		TpPostingList *posting = NULL;
		if (string_entry->key.posting_list != InvalidDsaPointer)
		{
			posting = (TpPostingList *)dsa_get_address(
					state->dsa, string_entry->key.posting_list);
		}

		if (posting && posting->doc_count > 0)
		{
			term_count++;
		}
	}
	dshash_seq_term(&seq_status);

	if (term_count == 0)
	{
		pfree(page_map);
		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(flush_context);
		return InvalidBlockNumber;
	}

	/* Allocate array for terms */
	terms	   = palloc(term_count * sizeof(TermEntry));
	term_count = 0;

	/* Collect terms for sorting */
	dshash_seq_init(&seq_status, string_table, false);
	while ((string_entry = (TpStringHashEntry *)dshash_seq_next(
					&seq_status)) != NULL)
	{
		/* Get posting list directly from the entry we already have */
		TpPostingList *posting = NULL;
		if (string_entry->key.posting_list != InvalidDsaPointer)
		{
			posting = (TpPostingList *)dsa_get_address(
					state->dsa, string_entry->key.posting_list);
		}

		if (posting && posting->doc_count > 0)
		{
			const char *term = tp_get_key_str(state->dsa, &string_entry->key);
			/* Copy term string to local memory */
			char *term_copy = pstrdup(term);

			terms[term_count].term		= term_copy;
			terms[term_count].term_len	= strlen(term_copy);
			terms[term_count].term_hash = DatumGetUInt32(
					hash_any((unsigned char *)term_copy, strlen(term_copy)));
			terms[term_count].posting_list = posting;
			term_count++;
		}
	}
	dshash_seq_term(&seq_status);

	/* Sort terms alphabetically */
	qsort(terms, term_count, sizeof(TermEntry), term_entry_compare);

	/* Collect document lengths */
	dshash_seq_init(&seq_status, doc_lengths_table, false);
	while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(&seq_status)) !=
		   NULL)
	{
		doc_count++;
	}
	dshash_seq_term(&seq_status);

	doc_lengths = palloc(doc_count * sizeof(TpDocLength));
	doc_count	= 0;

	dshash_seq_init(&seq_status, doc_lengths_table, false);
	while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(&seq_status)) !=
		   NULL)
	{
		doc_lengths[doc_count].ctid		= doc_entry->ctid;
		doc_lengths[doc_count].length	= doc_entry->doc_length;
		doc_lengths[doc_count].reserved = 0;
		doc_count++;
	}
	dshash_seq_term(&seq_status);

	/* Get current first segment for linking */
	{
		Buffer			metabuf_temp  = ReadBuffer(index, TP_METAPAGE_BLKNO);
		Page			metapage_temp = BufferGetPage(metabuf_temp);
		TpIndexMetaPage metap_temp	  = (TpIndexMetaPage)PageGetContents(
				   metapage_temp);
		prev_first_segment = metap_temp->first_segment;
		ReleaseBuffer(metabuf_temp);
	}

	/* Allocate first page for header */
	header_buf = ReadBuffer(index, P_NEW);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page = BufferGetPage(header_buf);
	PageInit(header_page, BLCKSZ, 0);

	root_block	= BufferGetBlockNumber(header_buf);
	page_map[0] = root_block;
	page_count	= 1;

	/* Initialize header (will update offsets as we write) */
	header				= (TpSegmentHeader *)PageGetContents(header_page);
	header->magic		= TP_SEGMENT_MAGIC;
	header->version		= TP_SEGMENT_VERSION;
	header->num_pages	= 0; /* Will update at end */
	header->data_size	= 0; /* Will update at end */
	header->num_terms	= term_count;
	header->num_docs	= doc_count;
	header->total_terms = memtable->total_terms;
	/* Get total_doc_length from metapage */
	{
		TpIndexMetaPage metap	 = tp_get_metapage(index);
		header->total_doc_length = metap->total_len;
		pfree(metap);
	}
	header->next_segment = prev_first_segment; /* Link to previous head */
	header->created_at	 = GetCurrentTimestamp();
	header->level		 = 0; /* Memtable flush */

	/*
	 * Release the header buffer BEFORE starting to write data.
	 * write_segment_data may need to lock this same buffer (since the header
	 * page can also hold data), and Postgres buffer locks are not recursive.
	 * We'll re-acquire it at the end to update final header fields.
	 */
	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/* Start after header and page map (we'll backfill page map) */
	logical_offset = sizeof(TpSegmentHeader) + max_pages * sizeof(BlockNumber);

	/* Write dictionary */
	dict_offset	 = logical_offset;
	dict_size	 = term_count * sizeof(TpDictEntry);
	dict_entries = palloc(dict_size);

	/* Build dictionary with placeholder offsets */
	for (uint32 i = 0; i < term_count; i++)
	{
		dict_entries[i].term_hash	   = terms[i].term_hash;
		dict_entries[i].string_offset  = 0; /* Will update */
		dict_entries[i].string_len	   = terms[i].term_len;
		dict_entries[i].posting_offset = 0; /* Will update */
		dict_entries[i].posting_count  = terms[i].posting_list->doc_count;
		dict_entries[i].doc_freq	   = terms[i].posting_list->doc_count;
	}

	write_segment_data(
			index,
			page_map,
			&page_count,
			&logical_offset,
			dict_entries,
			dict_size);
	dict_size_val = logical_offset - dict_offset;

	/* Write posting lists and track offsets */
	postings_offset = logical_offset;
	posting_offsets = palloc(term_count * sizeof(uint32));

	for (uint32 i = 0; i < term_count; i++)
	{
		TpPostingList	 *posting = terms[i].posting_list;
		TpPostingEntry	 *entries;
		TpSegmentPosting *segment_postings;
		uint32			  posting_size;

		posting_offsets[i] = logical_offset;

		/* Get entries from memtable posting list */
		entries = tp_get_posting_entries(state->dsa, posting);
		if (!entries)
			continue;

		/* Convert to segment format */
		posting_size	 = posting->doc_count * sizeof(TpSegmentPosting);
		segment_postings = palloc(posting_size);

		for (int j = 0; j < posting->doc_count; j++)
		{
			segment_postings[j].ctid	  = entries[j].ctid;
			segment_postings[j].frequency = entries[j].frequency;
		}

		write_segment_data(
				index,
				page_map,
				&page_count,
				&logical_offset,
				segment_postings,
				posting_size);

		pfree(segment_postings);
	}
	postings_size_val = logical_offset - postings_offset;

	/* Write document lengths */
	doclens_offset = logical_offset;
	write_segment_data(
			index,
			page_map,
			&page_count,
			&logical_offset,
			doc_lengths,
			doc_count * sizeof(TpDocLength));
	doclens_size_val = logical_offset - doclens_offset;

	/* Write string pool and track offsets */
	strings_offset = logical_offset;
	string_offsets = palloc(term_count * sizeof(uint32));

	for (uint32 i = 0; i < term_count; i++)
	{
		uint32 str_len;

		str_len = terms[i].term_len + 1; /* Include null terminator */
		string_offsets[i] = logical_offset;
		write_segment_data(
				index,
				page_map,
				&page_count,
				&logical_offset,
				terms[i].term,
				str_len);
	}
	strings_size_val = logical_offset - strings_offset;

	/* Update dictionary with actual offsets */
	for (uint32 i = 0; i < term_count; i++)
	{
		dict_entries[i].string_offset  = string_offsets[i];
		dict_entries[i].posting_offset = posting_offsets[i];
	}

	/* Rewrite dictionary with correct offsets */
	logical_offset = dict_offset;
	write_segment_data(
			index,
			page_map,
			&page_count,
			&logical_offset,
			dict_entries,
			dict_size);

	/*
	 * Now re-acquire the header buffer and update all header fields.
	 * We released it earlier to avoid recursive buffer locking.
	 */
	header_buf = ReadBuffer(index, root_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page = BufferGetPage(header_buf);
	header		= (TpSegmentHeader *)PageGetContents(header_page);

	/* Update header with final counts and offsets */
	header->dict_offset		= dict_offset;
	header->dict_size		= dict_size_val;
	header->postings_offset = postings_offset;
	header->postings_size	= postings_size_val;
	header->doclens_offset	= doclens_offset;
	header->doclens_size	= doclens_size_val;
	header->strings_offset	= strings_offset;
	header->strings_size	= strings_size_val;
	header->num_pages		= page_count;
	header->data_size		= logical_offset;

	/* Write page map to header */
	memcpy(header->page_map, page_map, page_count * sizeof(BlockNumber));

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/*
	 * Update metapage to link this segment and persist corpus statistics.
	 * Before we clear the memtable, we need to save the corpus statistics
	 * to the metapage so they persist across flushes.
	 */
	{
		Buffer metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		Page   metapage;
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		/* Link new segment at head of chain */
		metap->first_segment = root_block;

		/*
		 * Note: Corpus statistics (total_docs, total_len, idf_sum) are
		 * already up-to-date in metapage, as they are maintained in
		 * real-time by tp_add_document_terms(). No need to update them here.
		 */

		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);
	}

	/* Clear the memtable */
	tp_string_table_clear(
			state->dsa, &state->shared->memory_usage, string_table);

	/* Clear document lengths table */
	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_destroy(doc_lengths_table);
		memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	}

	/*
	 * Clear docid pages since documents are now safely in the segment.
	 * This prevents stale docids from being used during recovery.
	 */
	tp_clear_docid_pages(index);

	/*
	 * Reset term count statistics.
	 * Note: Corpus statistics (total_docs, total_len, idf_sum) remain
	 * in metapage and are not cleared.
	 */
	memtable->total_terms = 0;

	/* Clean up */
	pfree(page_map);
	pfree(terms);
	pfree(doc_lengths);
	pfree(dict_entries);
	pfree(posting_offsets);
	pfree(string_offsets);

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(flush_context);

	elog(DEBUG1,
		 "Flushed %u terms and %u documents to segment at block %u",
		 term_count,
		 doc_count,
		 root_block);

	return root_block;
#endif
}
