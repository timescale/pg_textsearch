/*-------------------------------------------------------------------------
 *
 * metapage.c
 *	  Tapir index metapage operations
 *
 * This module implements metapage initialization, reading, and management
 * for Tapir indexes. The metapage stores index configuration, global
 * statistics, and crash recovery state.
 *
 * IDENTIFICATION
 *	  src/metapage.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/heapam.h>
#include <catalog/index.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/builtins.h>
#include <utils/rel.h>

#include "constants.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "metapage.h"
#include "state.h"
#include "vector.h"

/* Maximum number of docids that fit in a page */
#define TP_DOCIDS_PER_PAGE                   \
	((BLCKSZ - sizeof(PageHeaderData) -      \
	  MAXALIGN(sizeof(TpDocidPageHeader))) / \
	 sizeof(ItemPointerData))

/*
 * Initialize Tapir index metapage
 */
void
tp_init_metapage(Page page, Oid text_config_oid)
{
	TpIndexMetaPage metap;
	PageHeader		phdr;

	/*
	 * Initialize page with no special space - metapage uses page content area
	 */
	PageInit(page, BLCKSZ, 0);
	metap = (TpIndexMetaPage)PageGetContents(page);

	metap->magic			= TP_MAGIC;
	metap->version			= TP_VERSION;
	metap->text_config_oid	= text_config_oid;
	metap->total_docs		= 0;
	metap->total_terms		= 0;
	metap->total_len		= 0;
	metap->root_blkno		= InvalidBlockNumber;
	metap->first_docid_page = InvalidBlockNumber;

	/* Update page header to reflect that we've used space for metapage */
	phdr		   = (PageHeader)page;
	phdr->pd_lower = SizeOfPageHeaderData + sizeof(TpIndexMetaPageData);
}

/*
 * Get Tapir index metapage
 */
TpIndexMetaPage
tp_get_metapage(Relation index)
{
	Buffer			buf;
	Page			page;
	TpIndexMetaPage metap;
	TpIndexMetaPage result;

	/* Validate input relation */
	if (!RelationIsValid(index))
		elog(ERROR, "invalid relation passed to tp_get_metapage");

	buf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	if (!BufferIsValid(buf))
	{
		elog(ERROR,
			 "failed to read metapage buffer for BM25 index \"%s\"",
			 RelationGetRelationName(index));
	}

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	metap = (TpIndexMetaPage)PageGetContents(page);
	if (!metap)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR,
			 "failed to get metapage contents for BM25 index \"%s\"",
			 RelationGetRelationName(index));
	}

	/* Validate magic number */
	if (metap->magic != TP_MAGIC)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR,
			 "Tapir index metapage is corrupted for index \"%s\": expected "
			 "magic "
			 "0x%08X, found 0x%08X",
			 RelationGetRelationName(index),
			 TP_MAGIC,
			 metap->magic);
	}

	/* Copy metapage data to avoid buffer issues */
	result = (TpIndexMetaPage)palloc(sizeof(TpIndexMetaPageData));
	memcpy(result, metap, sizeof(TpIndexMetaPageData));

	UnlockReleaseBuffer(buf);
	return result;
}

/*
 * Add a document ID to the docid pages for crash recovery
 * This appends the ctid to the chain of docid pages
 */
void
tp_add_docid_to_pages(Relation index, ItemPointer ctid)
{
	Buffer			   metabuf, docid_buf;
	Page			   metapage, docid_page;
	TpIndexMetaPage	   metap;
	TpDocidPageHeader *docid_header;
	ItemPointer		   docids;
	BlockNumber		   current_page, new_page;
	int				   page_capacity;

	/* Get the metapage to find the first docid page */
	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	/* Find the last page in the docid chain */
	current_page = metap->first_docid_page;

	if (current_page == InvalidBlockNumber)
	{
		/* No docid pages yet, create the first one */
		new_page  = P_NEW;
		docid_buf = ReadBuffer(index, new_page);
		LockBuffer(docid_buf, BUFFER_LOCK_EXCLUSIVE);

		docid_page = BufferGetPage(docid_buf);
		PageInit(docid_page, BLCKSZ, 0);

		docid_header		= (TpDocidPageHeader *)PageGetContents(docid_page);
		docid_header->magic = TP_DOCID_PAGE_MAGIC;
		docid_header->num_docids = 0;
		docid_header->next_page	 = InvalidBlockNumber;
		docid_header->reserved	 = 0;

		MarkBufferDirty(docid_buf);

		/* Update metapage to point to this new page */
		BlockNumber new_docid_page = BufferGetBlockNumber(docid_buf);
		metap->first_docid_page	   = new_docid_page;

		MarkBufferDirty(metabuf);

		/* Flush metapage to disk immediately to ensure crash recovery works */
		FlushOneBuffer(metabuf);
	}
	else
	{
		/* Follow the chain to find the last page */
		BlockNumber next_page = current_page;

		while (next_page != InvalidBlockNumber)
		{
			current_page = next_page;
			docid_buf	 = ReadBuffer(index, current_page);
			LockBuffer(docid_buf, BUFFER_LOCK_SHARE);

			docid_page	 = BufferGetPage(docid_buf);
			docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);
			next_page	 = docid_header->next_page;

			if (next_page == InvalidBlockNumber)
			{
				/* This is the last page - upgrade to exclusive lock */
				LockBuffer(docid_buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(docid_buf, BUFFER_LOCK_EXCLUSIVE);
				/* Re-read header after lock upgrade */
				docid_header = (TpDocidPageHeader *)PageGetContents(
						docid_page);
				break;
			}
			else
				UnlockReleaseBuffer(docid_buf);
		}
	}

	/* Check if current page has room for another docid */
	page_capacity = TP_DOCIDS_PER_PAGE;

	if (docid_header->num_docids >= page_capacity)
	{
		/* Current page is full, create a new one */
		Buffer			   new_buf = ReadBuffer(index, P_NEW);
		Page			   new_docid_page;
		TpDocidPageHeader *new_header;

		LockBuffer(new_buf, BUFFER_LOCK_EXCLUSIVE);
		new_docid_page = BufferGetPage(new_buf);
		PageInit(new_docid_page, BLCKSZ, 0);

		new_header = (TpDocidPageHeader *)PageGetContents(new_docid_page);
		new_header->magic	   = TP_DOCID_PAGE_MAGIC;
		new_header->num_docids = 0;
		new_header->next_page  = InvalidBlockNumber;
		new_header->reserved   = 0;

		MarkBufferDirty(new_buf);

		/* Link old page to new page */
		docid_header->next_page = BufferGetBlockNumber(new_buf);
		MarkBufferDirty(docid_buf);

		FlushOneBuffer(docid_buf);

		UnlockReleaseBuffer(docid_buf);

		/* Switch to new page */
		docid_buf	 = new_buf;
		docid_page	 = new_docid_page;
		docid_header = new_header;
	}

	/* Add the docid to the current page */
	docids = (ItemPointer)((char *)docid_header +
						   MAXALIGN(sizeof(TpDocidPageHeader)));

	docids[docid_header->num_docids] = *ctid;
	docid_header->num_docids++;

	MarkBufferDirty(docid_buf);

	FlushOneBuffer(docid_buf);

	UnlockReleaseBuffer(docid_buf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * Recover from docid pages by rebuilding the in-memory structures
 * This is called during index startup after a crash
 */
void
tp_recover_from_docid_pages(Relation index)
{
	Buffer			   metabuf, docid_buf;
	Page			   metapage, docid_page;
	TpIndexMetaPage	   metap;
	TpDocidPageHeader *docid_header;
	ItemPointer		   docids;
	BlockNumber		   current_page;
	int				   total_recovered = 0;

	/* Get the metapage to find the first docid page */
	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	current_page = metap->first_docid_page;
	UnlockReleaseBuffer(metabuf);

	if (current_page == InvalidBlockNumber)
		return;

	/* Iterate through all docid pages */
	while (current_page != InvalidBlockNumber)
	{
		docid_buf = ReadBuffer(index, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/* Validate page magic */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
		{
			elog(WARNING,
				 "Invalid docid page magic on block %u, skipping recovery",
				 current_page);
			UnlockReleaseBuffer(docid_buf);
			break;
		}

		/* Process each docid on this page */
		docids = (ItemPointer)((char *)docid_header +
							   sizeof(TpDocidPageHeader));

		for (int i = 0; i < docid_header->num_docids; i++)
		{
			ItemPointer		   ctid = &docids[i];
			Relation		   heap_rel;
			HeapTuple		   tuple;
			Buffer			   heap_buf;
			bool			   valid;
			TpLocalIndexState *local_state;

			/* Get local index state */
			local_state = tp_get_local_index_state(RelationGetRelid(index));

			/* Find the heap relation for this index */
			heap_rel =
					relation_open(index->rd_index->indrelid, AccessShareLock);

			/* Initialize tuple for heap_fetch */
			tuple		  = &((HeapTupleData){0});
			tuple->t_self = *ctid;

			/* Fetch the tuple from the heap */
			valid = heap_fetch(heap_rel, SnapshotAny, tuple, &heap_buf, true);

			if (valid && HeapTupleIsValid(tuple))
			{
				/* Extract the indexed column */
				Datum	   column_value;
				bool	   is_null;
				TupleDesc  tuple_desc = RelationGetDescr(heap_rel);
				AttrNumber attnum	  = index->rd_index->indkey.values[0];

				/* Get the indexed column value */
				column_value =
						heap_getattr(tuple, attnum, tuple_desc, &is_null);

				if (!is_null)
				{
					text		  *document_text;
					Datum		   vector_datum;
					TpVector	  *tpvec;
					TpVectorEntry *vector_entry;
					int			   term_count;
					char		 **terms;
					int32		  *frequencies;
					int			   doc_length = 0;
					char		  *ptr;

					document_text = DatumGetTextPP(column_value);

					/* Vectorize the document */
					vector_datum = DirectFunctionCall2(
							to_tpvector,
							PointerGetDatum(document_text),
							CStringGetTextDatum(
									RelationGetRelationName(index)));
					tpvec = (TpVector *)DatumGetPointer(vector_datum);

					/* Extract term IDs and frequencies from tpvector */
					term_count = tpvec->entry_count;
					if (term_count > 0)
					{
						terms		= palloc(term_count * sizeof(char *));
						frequencies = palloc(term_count * sizeof(int32));

						ptr = (char *)TPVECTOR_ENTRIES_PTR(tpvec);

						for (int j = 0; j < term_count; j++)
						{
							char *term_copy;
							vector_entry = (TpVectorEntry *)ptr;

							/* Copy the lexeme string from vector entry */
							term_copy = palloc(vector_entry->lexeme_len + 1);
							memcpy(term_copy,
								   vector_entry->lexeme,
								   vector_entry->lexeme_len);
							term_copy[vector_entry->lexeme_len] = '\0';

							/* Store the lexeme string directly in terms array
							 */
							terms[j]	   = term_copy;
							frequencies[j] = vector_entry->frequency;
							doc_length += vector_entry->frequency;

							/* Move to next entry */
							ptr += sizeof(TpVectorEntry) +
								   MAXALIGN(vector_entry->lexeme_len);
						}

						/* Add document terms to posting lists */
						tp_add_document_terms(
								local_state,
								ctid,
								terms,
								frequencies,
								term_count,
								doc_length);

						/* Clean up */
						for (int j = 0; j < term_count; j++)
						{
							pfree(terms[j]);
						}
						pfree(terms);
						pfree(frequencies);
						total_recovered++;
					}

					/* Free the vector */
					pfree(tpvec);
				}

				/* Free the tuple */
				heap_freetuple(tuple);
				ReleaseBuffer(heap_buf);
			}

			/* Close the heap relation */
			relation_close(heap_rel, AccessShareLock);
		}

		/* Move to next page */
		current_page = docid_header->next_page;
		UnlockReleaseBuffer(docid_buf);
	}
}
