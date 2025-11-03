/*-------------------------------------------------------------------------
 *
 * segment/segment.c
 *    Segment implementation for pg_textsearch
 *
 * IDENTIFICATION
 *    src/segment/segment.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "../memtable/memtable.h"
#include "../memtable/posting.h"
#include "../memtable/stringtable.h"
#include "../state.h"
#include "access/genam.h"
#include "access/hash.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "dictionary.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "segment.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lock.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*
 * Stub implementations for now
 */

TpSegmentReader *
tp_segment_open(Relation index, BlockNumber root_block)
{
	TpSegmentReader	   *reader;
	Buffer				header_buf;
	Page				header_page;
	TpSegmentHeader	   *header;
	BlockNumber			page_index_block;
	Buffer				index_buf;
	Page				index_page;
	TpPageIndexSpecial *special;
	BlockNumber		   *page_entries;
	uint32				pages_loaded = 0;
	uint32				i;

	/* Allocate reader structure */
	reader						 = palloc0(sizeof(TpSegmentReader));
	reader->index				 = index;
	reader->root_block			 = root_block;
	reader->current_buffer		 = InvalidBuffer;
	reader->current_logical_page = UINT32_MAX;

	/* Read header from root block */
	header_buf = ReadBuffer(index, root_block);
	LockBuffer(header_buf, BUFFER_LOCK_SHARE);
	header_page = BufferGetPage(header_buf);

	/* Copy header to reader structure */
	reader->header = palloc(sizeof(TpSegmentHeader));
	memcpy(reader->header,
		   (char *)header_page + SizeOfPageHeaderData,
		   sizeof(TpSegmentHeader));

	header			  = reader->header;
	reader->num_pages = header->num_pages;

	/* Get page index location from header */
	page_index_block = header->page_index;

	/* Keep header buffer for later use */
	reader->header_buffer = header_buf;
	LockBuffer(
			header_buf, BUFFER_LOCK_UNLOCK); /* Just unlock, don't release */

	/* Allocate page map */
	reader->page_map = palloc(sizeof(BlockNumber) * reader->num_pages);

	/* Read page index chain to build page map */
	while (page_index_block != InvalidBlockNumber &&
		   pages_loaded < reader->num_pages)
	{
		index_buf = ReadBuffer(index, page_index_block);
		LockBuffer(index_buf, BUFFER_LOCK_SHARE);
		index_page = BufferGetPage(index_buf);

		/* Get special area with page index metadata */
		special = (TpPageIndexSpecial *)PageGetSpecialPointer(index_page);

		/* Get pointer to page entries array */
		page_entries = (BlockNumber *)((char *)index_page +
									   SizeOfPageHeaderData);

		/* Copy page entries to our map */
		for (i = 0;
			 i < special->num_entries && pages_loaded < reader->num_pages;
			 i++)
		{
			reader->page_map[pages_loaded++] = page_entries[i];
		}

		/* Move to next page in chain */
		page_index_block = special->next_page;

		UnlockReleaseBuffer(index_buf);
	}

	if (pages_loaded != reader->num_pages)
	{
		elog(WARNING,
			 "Page index incomplete: expected %u pages, loaded %u",
			 reader->num_pages,
			 pages_loaded);
	}

	return reader;
}

void
tp_segment_close(TpSegmentReader *reader)
{
	if (BufferIsValid(reader->current_buffer))
		ReleaseBuffer(reader->current_buffer);

	if (BufferIsValid(reader->header_buffer))
		ReleaseBuffer(reader->header_buffer);

	if (reader->page_map)
		pfree(reader->page_map);

	pfree(reader);
}

void
tp_segment_read(
		TpSegmentReader *reader, uint32 logical_offset, void *dest, uint32 len)
{
	char  *dest_ptr	  = (char *)dest;
	uint32 bytes_read = 0;

	while (bytes_read < len)
	{
		uint32 logical_page = logical_offset / BLCKSZ;
		uint32 page_offset	= logical_offset % BLCKSZ;
		uint32 to_read;
		Buffer buf;
		Page   page;
		char  *src;

		/* Skip page header in offset calculation */
		if (page_offset < SizeOfPageHeaderData)
			page_offset = SizeOfPageHeaderData;

		/* Calculate how much to read from this page */
		to_read = Min(len - bytes_read, BLCKSZ - page_offset);

		/* Check if we have the page in cache */
		if (reader->current_logical_page != logical_page)
		{
			/* Release old buffer if any */
			if (BufferIsValid(reader->current_buffer))
			{
				ReleaseBuffer(reader->current_buffer);
				reader->current_buffer = InvalidBuffer;
			}

			/* Validate page number */
			if (logical_page >= reader->num_pages)
			{
				elog(ERROR,
					 "Invalid logical page %u (max %u)",
					 logical_page,
					 reader->num_pages - 1);
			}

			/* Read the physical page */
			buf = ReadBuffer(reader->index, reader->page_map[logical_page]);

			reader->current_buffer		 = buf;
			reader->current_logical_page = logical_page;
		}
		else
		{
			buf = reader->current_buffer;
		}

		/* Lock buffer for reading */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		/* Copy data from page */
		page = BufferGetPage(buf);
		src	 = (char *)page + page_offset;
		memcpy(dest_ptr + bytes_read, src, to_read);

		/* Unlock but keep buffer pinned for potential reuse */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		/* Advance pointers */
		bytes_read += to_read;
		logical_offset += to_read;
	}
}

TpPostingList *
tp_segment_get_posting_list(TpSegmentReader *reader, const char *term)
{
	TpSegmentHeader *header = reader->header;
	TpDictionary	 dict_header;
	uint32			*string_offsets;
	int				 left, right, mid;
	TpPostingList	*result = NULL;

	if (header->num_terms == 0 || header->dictionary_offset == 0)
		return NULL;

	/* Read dictionary header */
	tp_segment_read(
			reader,
			header->dictionary_offset,
			&dict_header,
			sizeof(dict_header.num_terms));

	/* Read string offsets array */
	string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
	tp_segment_read(
			reader,
			header->dictionary_offset + sizeof(dict_header.num_terms),
			string_offsets,
			sizeof(uint32) * dict_header.num_terms);

	/* Binary search for the term */
	left  = 0;
	right = dict_header.num_terms - 1;

	while (left <= right)
	{
		TpStringEntry string_entry;
		char		 *term_text;
		int			  cmp;
		uint32		  string_offset;

		mid			  = left + (right - left) / 2;
		string_offset = header->strings_offset + string_offsets[mid];

		/* Read string length */
		tp_segment_read(
				reader, string_offset, &string_entry.length, sizeof(uint32));

		/* Read term text */
		term_text = palloc(string_entry.length + 1);
		tp_segment_read(
				reader,
				string_offset + sizeof(uint32),
				term_text,
				string_entry.length);
		term_text[string_entry.length] = '\0';

		/* Compare terms */
		cmp = strcmp(term, term_text);

		if (cmp == 0)
		{
			/* Found it! Read the dictionary entry and posting list */
			TpDictEntry		  dict_entry;
			TpSegmentPosting *postings;
			uint32			  i;

			/* Read dictionary entry */
			tp_segment_read(
					reader,
					header->entries_offset + (mid * sizeof(TpDictEntry)),
					&dict_entry,
					sizeof(TpDictEntry));

			/* Allocate result structure */
			result			  = palloc0(sizeof(TpPostingList));
			result->doc_count = dict_entry.posting_count;
			result->doc_freq  = dict_entry.doc_freq;
			result->capacity  = dict_entry.posting_count;
			result->is_sorted = true; /* Segments store sorted postings */

			/* Read posting data from segment */
			if (dict_entry.posting_count > 0)
			{
				TpPostingEntry *entries;

				postings = palloc(
						sizeof(TpSegmentPosting) * dict_entry.posting_count);
				tp_segment_read(
						reader,
						header->postings_offset + dict_entry.posting_offset,
						postings,
						sizeof(TpSegmentPosting) * dict_entry.posting_count);

				/* Convert to TpPostingEntry array in local memory */
				entries = palloc(
						sizeof(TpPostingEntry) * dict_entry.posting_count);
				for (i = 0; i < dict_entry.posting_count; i++)
				{
					entries[i].ctid		 = postings[i].ctid;
					entries[i].doc_id	 = -1; /* Not used for segments */
					entries[i].frequency = postings[i].frequency;
				}

				/* For segment-based posting lists, we store entries locally,
				 * not in DSA */
				result->entries_dp = (dsa_pointer)
						entries; /* Hack: store local pointer as dsa_pointer */

				pfree(postings);
			}
			else
			{
				result->entries_dp = InvalidDsaPointer;
			}

			pfree(term_text);
			break;
		}
		else if (cmp < 0)
		{
			right = mid - 1;
		}
		else
		{
			left = mid + 1;
		}

		pfree(term_text);
	}

	pfree(string_offsets);
	return result;
}

/*
 * Get document length from segment
 */
int32
tp_segment_get_document_length(TpSegmentReader *reader, ItemPointer ctid)
{
	TpSegmentHeader *header = reader->header;
	TpDocLength		 doc_length;
	uint32			 i;

	if (header->num_docs == 0 || header->doc_lengths_offset == 0)
		return -1;

	/* Linear search through document lengths (could optimize with binary
	 * search) */
	for (i = 0; i < header->num_docs; i++)
	{
		tp_segment_read(
				reader,
				header->doc_lengths_offset + (i * sizeof(TpDocLength)),
				&doc_length,
				sizeof(TpDocLength));

		if (ItemPointerEquals(ctid, &doc_length.ctid))
		{
			return (int32)doc_length.length;
		}
	}

	/* Document not found */
	return -1;
}

/*
 * Allocate pages for segment
 */
static BlockNumber *
allocate_segment_pages(Relation index, uint32 num_pages)
{
	BlockNumber *pages;
	uint32		 i;

	if (num_pages == 0)
	{
		elog(ERROR, "Cannot allocate 0 pages");
	}

	pages = palloc(num_pages * sizeof(BlockNumber));

	elog(DEBUG1, "Allocating %u pages for segment", num_pages);

	for (i = 0; i < num_pages; i++)
	{
		Buffer buffer;

		/* Use RBM_ZERO_AND_LOCK to get a new zero-filled page */
		buffer = ReadBufferExtended(
				index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);

		/* The page should already be initialized by RBM_ZERO_AND_LOCK */
		pages[i] = BufferGetBlockNumber(buffer);

		elog(DEBUG2, "Allocated page %u at block %u", i, pages[i]);

		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}

	return pages;
}

/*
 * Write page index (chain of BlockNumbers)
 */
static BlockNumber
write_page_index(Relation index, BlockNumber *pages, uint32 num_pages)
{
	BlockNumber index_root = InvalidBlockNumber;
	BlockNumber prev_block = InvalidBlockNumber;
	uint32		page_idx   = 0;

	/* Calculate how many index pages we need */
	uint32 entries_per_page = (BLCKSZ - SizeOfPageHeaderData) /
							  sizeof(BlockNumber);
	uint32 num_index_pages = (num_pages + entries_per_page - 1) /
							 entries_per_page;

	/* Allocate index pages */
	BlockNumber *index_pages = allocate_segment_pages(index, num_index_pages);

	/* Write index pages in reverse order (so we can chain them) */
	for (int i = num_index_pages - 1; i >= 0; i--)
	{
		Buffer				buffer;
		Page				page;
		BlockNumber		   *page_data;
		TpPageIndexSpecial *special;
		uint32				entries_to_write;
		uint32				j;

		buffer = ReadBuffer(index, index_pages[i]);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to
													  modify */
		page = BufferGetPage(buffer);

		/* Initialize page with special area */
		PageInit(page, BLCKSZ, sizeof(TpPageIndexSpecial));

		/* Set up special area */
		special			   = (TpPageIndexSpecial *)PageGetSpecialPointer(page);
		special->next_page = prev_block;
		special->page_type = TP_PAGE_FILE_INDEX;
		special->num_entries = Min(entries_per_page, num_pages - page_idx);
		special->flags		 = 0;

		/* Use the data area after the page header */
		page_data = (BlockNumber *)((char *)page + SizeOfPageHeaderData);

		/* Fill with page numbers */
		entries_to_write = special->num_entries;
		for (j = 0; j < entries_to_write; j++)
		{
			page_data[j] = pages[page_idx++];
		}

		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);

		prev_block = index_pages[i];
		if (i == 0)
			index_root = index_pages[i];
	}

	pfree(index_pages);
	return index_root;
}

/*
 * Write segment from memtable
 */
BlockNumber
tp_write_segment(TpLocalIndexState *state, Relation index)
{
	TermInfo		*terms;
	uint32			 num_terms;
	uint32			 num_pages_needed = 10; /* TODO: Calculate actual size */
	BlockNumber		*pages;
	BlockNumber		 header_block;
	BlockNumber		 page_index_root;
	TpSegmentWriter	 writer;
	TpSegmentHeader	 header;
	TpDictionary	 dict;
	uint32			*string_offsets;
	uint32			 string_pos;
	uint32			 i;
	Buffer			 header_buf;
	Page			 header_page;
	TpSegmentHeader *existing_header;
	TpSegmentHeader *updated_header;

	/* Build sorted dictionary */
	terms = tp_build_dictionary(state, &num_terms);

	if (num_terms == 0)
	{
		tp_free_dictionary(terms, num_terms);
		return InvalidBlockNumber;
	}

	elog(DEBUG1,
		 "tp_write_segment: Starting segment write with %u terms",
		 num_terms);

	/* Allocate pages for segment data */
	pages		 = allocate_segment_pages(index, num_pages_needed);
	header_block = pages[0];

	elog(DEBUG1,
		 "tp_write_segment: Allocated %u pages, header at block %u "
		 "(pages[0]=%u)",
		 num_pages_needed,
		 header_block,
		 pages[0]);

	/* Initialize writer */
	tp_segment_writer_init(&writer, index, pages, num_pages_needed);

	/* Write header (placeholder - will update offsets later) */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= num_pages_needed;
	header.num_terms	= num_terms;
	header.num_docs		= 0; /* TODO: Get actual doc count */
	header.total_tokens = 0; /* TODO: Get actual token count */
	header.level		= 0; /* L0 segment from memtable */
	header.next_segment = InvalidBlockNumber;

	/* Write header (will update with correct values later) */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	header.dictionary_offset = writer.current_offset;
	dict.num_terms			 = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Allocate and calculate string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));

	/* Calculate offsets for each string */
	string_pos = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		/* TpStringEntry format: length (4) + text + dict_offset (4) */
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

		elog(DEBUG1,
			 "Writing string %u: '%s' (len=%u) at offset %u",
			 i,
			 terms[i].term,
			 length,
			 writer.current_offset - header.strings_offset);

		/* Write TpStringEntry format: length, then text, then
		 * dict_entry_offset */
		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		/* Write dict entry offset (position in entries array) */
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Write dictionary entries */
	header.entries_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		TpDictEntry	   entry;
		TpPostingList *posting_list = NULL;

		/* Get posting list from DSA if valid */
		if (terms[i].posting_list_dp != InvalidDsaPointer)
		{
			posting_list = (TpPostingList *)
					dsa_get_address(state->dsa, terms[i].posting_list_dp);
		}

		/* Fill in dictionary entry */
		entry.posting_offset = 0; /* Will update with actual offset later */
		entry.posting_count	 = posting_list ? posting_list->doc_count : 0;
		entry.doc_freq		 = posting_list ? posting_list->doc_freq : 0;

		/* Store the posting offset for this term */
		terms[i].dict_entry_idx = i;

		/* Write the dictionary entry */
		tp_segment_writer_write(&writer, &entry, sizeof(TpDictEntry));
	}

	/* Write posting lists */
	header.postings_offset = writer.current_offset;
	{
		uint32 *posting_offsets = palloc0(num_terms * sizeof(uint32));

		for (i = 0; i < num_terms; i++)
		{
			TpPostingList *posting_list = NULL;

			/* Record offset for this term's posting list */
			posting_offsets[i] = writer.current_offset -
								 header.postings_offset;

			/* Get posting list from DSA if valid */
			if (terms[i].posting_list_dp != InvalidDsaPointer)
			{
				posting_list = (TpPostingList *)
						dsa_get_address(state->dsa, terms[i].posting_list_dp);
			}

			if (posting_list && posting_list->doc_count > 0)
			{
				/* Get posting entries from DSA */
				TpPostingEntry *entries = (TpPostingEntry *)
						dsa_get_address(state->dsa, posting_list->entries_dp);

				/* Write each posting */
				int32 j;
				for (j = 0; j < posting_list->doc_count; j++)
				{
					TpSegmentPosting seg_posting;
					seg_posting.ctid	  = entries[j].ctid;
					seg_posting.frequency = (uint16)entries[j].frequency;
					tp_segment_writer_write(
							&writer, &seg_posting, sizeof(TpSegmentPosting));
				}
			}
		}

		pfree(posting_offsets);
	}

	/* Write document lengths */
	header.doc_lengths_offset = writer.current_offset;
	{
		TpMemtable *memtable = get_memtable(state);

		/* Check if memtable has document lengths */
		if (memtable && memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
		{
			dshash_table	 *doc_lengths_hash;
			dshash_seq_status seq_status;
			TpDocLengthEntry *doc_entry;
			uint32			  doc_count = 0;
			dshash_parameters doc_lengths_params;

			/* Setup parameters for doc lengths hash table */
			memset(&doc_lengths_params, 0, sizeof(doc_lengths_params));
			doc_lengths_params.key_size	  = sizeof(ItemPointerData);
			doc_lengths_params.entry_size = sizeof(TpDocLengthEntry);
			/* Use simple hash wrapper for ItemPointer */
			doc_lengths_params.hash_function	= dshash_memhash;
			doc_lengths_params.compare_function = dshash_memcmp;

			/* Attach to document lengths hash table */
			doc_lengths_hash = dshash_attach(
					state->dsa,
					&doc_lengths_params,
					memtable->doc_lengths_handle,
					NULL);

			/* Count documents first */
			dshash_seq_init(&seq_status, doc_lengths_hash, false);
			while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(
							&seq_status)) != NULL)
			{
				doc_count++;
			}
			dshash_seq_term(&seq_status);

			header.num_docs = doc_count;

			/* Now write the document lengths */
			dshash_seq_init(&seq_status, doc_lengths_hash, false);
			while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(
							&seq_status)) != NULL)
			{
				TpDocLength doc_length;
				doc_length.ctid		= doc_entry->ctid;
				doc_length.length	= (uint16)doc_entry->doc_length;
				doc_length.reserved = 0;
				tp_segment_writer_write(
						&writer, &doc_length, sizeof(TpDocLength));
			}
			dshash_seq_term(&seq_status);

			dshash_detach(doc_lengths_hash);
		}
	}

	/* Write page index */
	tp_segment_writer_flush(&writer);
	page_index_root	  = write_page_index(index, pages, num_pages_needed);
	header.page_index = page_index_root;

	/* Update header with actual offsets */
	header.data_size = writer.current_offset;

	/* Finish with the writer BEFORE updating the header */
	tp_segment_writer_finish(&writer);

	elog(DEBUG1, "tp_write_segment: Updating header with offsets:");
	elog(DEBUG1, "  dictionary_offset = %u", header.dictionary_offset);
	elog(DEBUG1, "  strings_offset = %u", header.strings_offset);
	elog(DEBUG1, "  entries_offset = %u", header.entries_offset);
	elog(DEBUG1, "  postings_offset = %u", header.postings_offset);
	elog(DEBUG1, "  doc_lengths_offset = %u", header.doc_lengths_offset);
	elog(DEBUG1, "  data_size = %u", header.data_size);

	/* Force a checkpoint to ensure data is on disk */
	FlushRelationBuffers(index);

	/* Now update the header on disk */
	elog(DEBUG1,
		 "tp_write_segment: Reading header block %u to update offsets",
		 header_block);
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to
													  modify */
	header_page = BufferGetPage(header_buf);

	/* Check what's currently in the header */
	existing_header = (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);
	elog(DEBUG1,
		 "tp_write_segment: Existing header magic = 0x%08X, dictionary_offset "
		 "= %u",
		 existing_header->magic,
		 existing_header->dictionary_offset);

	/* Write header to the beginning of the page's data area */
	memcpy((char *)header_page + SizeOfPageHeaderData,
		   &header,
		   sizeof(TpSegmentHeader));

	/* Verify the update */
	updated_header = (TpSegmentHeader *)((char *)header_page +
										 SizeOfPageHeaderData);
	elog(DEBUG1,
		 "tp_write_segment: After update dictionary_offset = %u",
		 updated_header->dictionary_offset);

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/* Clean up */
	tp_free_dictionary(terms, num_terms);
	pfree(string_offsets);
	pfree(pages);

	elog(DEBUG1,
		 "Wrote segment with %u terms to block %u",
		 num_terms,
		 header_block);

	return header_block;
}

/*
 * Debug function to dump segment contents
 */
void
tp_debug_dump_segment_internal(
		const char *index_name, BlockNumber segment_root)
{
	Relation		index;
	TpSegmentHeader header;
	Buffer			header_buf;
	Page			header_page;
	Oid				index_oid;
	Oid				namespace_oid;
	uint32			terms_to_show;

	if (segment_root == InvalidBlockNumber)
	{
		elog(INFO, "Segment for index '%s' is empty", index_name);
		return;
	}

	/* Find the index OID */
	namespace_oid = LookupNamespaceNoError("public");
	if (namespace_oid == InvalidOid)
		elog(ERROR, "Schema 'public' not found");

	index_oid = get_relname_relid(index_name, namespace_oid);
	if (index_oid == InvalidOid)
		elog(ERROR, "Index '%s' not found", index_name);

	/* Open the index */
	index = index_open(index_oid, AccessShareLock);

	/* Read the header from the first page */
	header_buf = ReadBuffer(index, segment_root);
	LockBuffer(header_buf, BUFFER_LOCK_SHARE);
	header_page = BufferGetPage(header_buf);

	/* Copy header from page */
	memcpy(&header,
		   (char *)header_page + SizeOfPageHeaderData,
		   sizeof(TpSegmentHeader));

	UnlockReleaseBuffer(header_buf);

	/* Dump header info */
	elog(INFO, "========== Segment Dump: %s ==========", index_name);
	elog(INFO, "Root block: %u", segment_root);
	elog(INFO,
		 "Magic: 0x%08X (expected 0x%08X)",
		 header.magic,
		 TP_SEGMENT_MAGIC);
	elog(INFO, "Version: %u", header.version);
	elog(INFO, "Pages: %u", header.num_pages);
	elog(INFO, "Data size: %u bytes", header.data_size);
	elog(INFO, "Level: %u", header.level);
	elog(INFO, "Page index: block %u", header.page_index);

	/* Corpus statistics */
	elog(INFO, "Terms: %u", header.num_terms);
	elog(INFO, "Docs: %u", header.num_docs);
	elog(INFO, "Total tokens: %lu", header.total_tokens);

	/* Section offsets */
	elog(INFO, "Dictionary offset: %u", header.dictionary_offset);
	elog(INFO, "Strings offset: %u", header.strings_offset);
	elog(INFO, "Entries offset: %u", header.entries_offset);
	elog(INFO, "Postings offset: %u", header.postings_offset);
	elog(INFO, "Doc lengths offset: %u", header.doc_lengths_offset);

	/* If we have terms, dump the dictionary */
	if (header.num_terms > 0 && header.dictionary_offset > 0)
	{
		TpSegmentReader *reader;
		TpDictionary	 dict_header;
		uint32			*string_offsets;
		uint32			 i;

		elog(INFO, "");
		elog(INFO, "Dictionary Terms (%u):", header.num_terms);

		/* Open segment reader */
		reader = tp_segment_open(index, segment_root);

		/* Read dictionary header */
		tp_segment_read(
				reader,
				header.dictionary_offset,
				&dict_header,
				sizeof(dict_header.num_terms));

		/* Read string offsets */
		string_offsets = palloc(sizeof(uint32) * dict_header.num_terms);
		tp_segment_read(
				reader,
				header.dictionary_offset + sizeof(dict_header.num_terms),
				string_offsets,
				sizeof(uint32) * dict_header.num_terms);

		/* Show the first few terms */
		terms_to_show = Min(header.num_terms, 20);

		for (i = 0; i < terms_to_show; i++)
		{
			TpStringEntry string_entry;
			char		 *term_text;
			TpDictEntry	  dict_entry;
			uint32 string_offset = header.strings_offset + string_offsets[i];

			elog(DEBUG1,
				 "Reading term %u: string_offset=%u (base=%u + offset=%u)",
				 i,
				 string_offset,
				 header.strings_offset,
				 string_offsets[i]);

			/* Read string length */
			tp_segment_read(
					reader,
					string_offset,
					&string_entry.length,
					sizeof(uint32));
			elog(DEBUG1, "  String length: %u", string_entry.length);

			/* Validate string length */
			if (string_entry.length > 1024) /* Sanity check */
			{
				elog(ERROR,
					 "Invalid string length %u for term %u",
					 string_entry.length,
					 i);
			}

			/* Read term text */
			term_text = palloc(string_entry.length + 1);
			tp_segment_read(
					reader,
					string_offset + sizeof(uint32),
					term_text,
					string_entry.length);
			term_text[string_entry.length] = '\0';
			elog(DEBUG1, "  Term text: '%s'", term_text);

			/* Read dictionary entry for this term */
			tp_segment_read(
					reader,
					header.entries_offset + (i * sizeof(TpDictEntry)),
					&dict_entry,
					sizeof(TpDictEntry));
			elog(DEBUG1,
				 "  Dict entry: docs=%u, postings=%u",
				 dict_entry.doc_freq,
				 dict_entry.posting_count);

			elog(INFO,
				 "  [%u] '%s' (docs=%u, postings=%u)",
				 i,
				 term_text,
				 dict_entry.doc_freq,
				 dict_entry.posting_count);

			pfree(term_text);
		}

		if (header.num_terms > terms_to_show)
		{
			elog(INFO,
				 "  ... and %u more terms",
				 header.num_terms - terms_to_show);
		}

		pfree(string_offsets);
		tp_segment_close(reader);
	}

	elog(INFO, "========== End Segment Dump ==========");

	index_close(index, AccessShareLock);
}

/* Segment writer helper functions */

void
tp_segment_writer_init(
		TpSegmentWriter *writer,
		Relation		 index,
		BlockNumber		*pages,
		uint32			 num_pages)
{
	elog(DEBUG2,
		 "tp_segment_writer_init: Initializing writer with %u pages",
		 num_pages);

	writer->index		   = index;
	writer->pages		   = pages;
	writer->num_pages	   = num_pages;
	writer->current_offset = 0;
	writer->buffer		   = palloc(BLCKSZ);
	writer->buffer_page	   = 0;
	writer->buffer_pos	   = SizeOfPageHeaderData; /* Skip page header */

	/* Initialize first page */
	PageInit((Page)writer->buffer, BLCKSZ, 0);

	elog(DEBUG2,
		 "tp_segment_writer_init: Writer initialized, buffer at %p",
		 writer->buffer);
}

void
tp_segment_writer_write(TpSegmentWriter *writer, const void *data, uint32 len)
{
	const char *src			  = (const char *)data;
	uint32		bytes_written = 0;

	while (bytes_written < len)
	{
		/* Calculate how much we can write to current page */
		uint32 page_space = BLCKSZ - writer->buffer_pos;
		uint32 to_write	  = Min(page_space, len - bytes_written);

		/* Copy data to buffer */
		memcpy(writer->buffer + writer->buffer_pos,
			   src + bytes_written,
			   to_write);
		writer->buffer_pos += to_write;
		writer->current_offset += to_write;
		bytes_written += to_write;

		/* If page is full, flush it */
		if (writer->buffer_pos >= BLCKSZ)
		{
			tp_segment_writer_flush(writer);

			/* Move to next page if we have more data */
			if (bytes_written < len)
			{
				writer->buffer_page++;
				if (writer->buffer_page >= writer->num_pages)
				{
					elog(ERROR, "Segment writer ran out of pages");
				}

				/* Initialize new page */
				PageInit((Page)writer->buffer, BLCKSZ, 0);
				writer->buffer_pos = SizeOfPageHeaderData;
			}
		}
	}
}

void
tp_segment_writer_flush(TpSegmentWriter *writer)
{
	Buffer buffer;
	Page   page;

	if (writer->buffer_page >= writer->num_pages)
		return; /* Nothing to flush */

	/* Write current buffer to disk */
	elog(DEBUG2,
		 "tp_segment_writer_flush: Flushing buffer_page %u to block %u",
		 writer->buffer_page,
		 writer->pages[writer->buffer_page]);
	buffer = ReadBuffer(writer->index, writer->pages[writer->buffer_page]);
	LockBuffer(
			buffer, BUFFER_LOCK_EXCLUSIVE); /* Need exclusive lock to modify */
	page = BufferGetPage(buffer);
	memcpy(page, writer->buffer, BLCKSZ);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

void
tp_segment_writer_finish(TpSegmentWriter *writer)
{
	/* Flush any remaining data */
	if (writer->buffer_pos > SizeOfPageHeaderData)
	{
		tp_segment_writer_flush(writer);
	}
	pfree(writer->buffer);
}
