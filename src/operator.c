/*-------------------------------------------------------------------------
 *
 * operator.c
 *	  BM25 scoring and query operations for Tapir
 *
 * This module contains the top-level scoring functions that coordinate
 * between memtable, stringtable, and posting list components.
 *
 * IDENTIFICATION
 *	  src/operator.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <math.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>

#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "metapage.h"
#include "operator.h"
#include "segment/segment.h"
#include "state.h"

/*
 * Centralized IDF calculation (basic version)
 * Calculates IDF using BM25 formula: log(1 + (N - df + 0.5) / (df + 0.5))
 * This formula ensures IDF is always non-negative since log(1 + x) >= 0
 * for all x >= 0.
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	return (float4)log(1.0 + idf_ratio);
}

/*
 * Create and initialize hash table for document scores
 */
static HTAB *
tp_create_doc_scores_hash(int max_results, int32 total_docs)
{
	HASHCTL hash_ctl;

	/* Initialize hash control structure */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);

	/* Create hash table */
	return hash_create(
			"Document Scores", total_docs, &hash_ctl, HASH_ELEM | HASH_BLOBS);
}

/*
 * Comparison function for qsort - sort by BM25 score descending (higher scores
 * first)
 */
static int
compare_document_scores(const void *a, const void *b)
{
	const DocumentScoreEntry *doc_a = *(const DocumentScoreEntry **)a;
	const DocumentScoreEntry *doc_b = *(const DocumentScoreEntry **)b;

	if (doc_a->score > doc_b->score)
		return -1; /* a comes before b (descending order) */
	else if (doc_a->score < doc_b->score)
		return 1; /* b comes before a */
	else
		return 0; /* equal scores */
}

/*
 * Extract and sort documents by BM25 score (descending)
 */
static DocumentScoreEntry **
tp_extract_and_sort_documents(
		HTAB *doc_scores_hash, int max_results, int *actual_count)
{
	DocumentScoreEntry **all_docs;
	DocumentScoreEntry **sorted_docs;
	int					 total_count;
	int					 result_count;
	int					 i;

	/* Get the total number of documents */
	total_count = hash_get_num_entries(doc_scores_hash);

	if (total_count == 0)
	{
		*actual_count = 0;
		return NULL;
	}

	/* Allocate array for ALL documents first */
	all_docs = (DocumentScoreEntry **)palloc(
			total_count * sizeof(DocumentScoreEntry *));

	/* Extract ALL documents from hash table */
	{
		HASH_SEQ_STATUS		seq_status;
		DocumentScoreEntry *current_entry;

		hash_seq_init(&seq_status, doc_scores_hash);
		i = 0;

		while ((current_entry = (DocumentScoreEntry *)hash_seq_search(
						&seq_status)) != NULL)
		{
			all_docs[i++] = current_entry;
		}

		Assert(i == total_count);
	}

	/* Sort ALL documents by score using qsort */
	qsort((void *)all_docs,
		  total_count,
		  sizeof(DocumentScoreEntry *),
		  compare_document_scores);

	/* Take only the top max_results */
	result_count = (total_count < max_results) ? total_count : max_results;

	if (result_count < total_count)
	{
		/* Need to copy just the top results to a smaller array */
		sorted_docs = (DocumentScoreEntry **)palloc(
				result_count * sizeof(DocumentScoreEntry *));
		for (i = 0; i < result_count; i++)
			sorted_docs[i] = all_docs[i];
		pfree(all_docs);
	}
	else
	{
		/* Use the full array */
		sorted_docs = all_docs;
	}

	*actual_count = result_count;
	return sorted_docs;
}

/*
 * Scan docid pages to find all documents in the index
 * Returns an array of CTIDs and the count
 *
 * TODO: Refactor to use a callback function instead of allocating
 * and returning an array. This would avoid memory allocation and
 * allow the caller to process CTIDs as they're discovered.
 */
static ItemPointer
tp_scan_docid_pages(Relation index_relation, int *total_ctids)
{
	Buffer			   metabuf, docid_buf;
	Page			   metapage, docid_page;
	TpIndexMetaPage	   metap;
	TpDocidPageHeader *docid_header;
	ItemPointer		   docids, all_ctids;
	BlockNumber		   current_page;
	int				   capacity = 100; /* Initial capacity */
	int				   count	= 0;

	/* Initialize output */
	*total_ctids = 0;

	/* Allocate initial array */
	all_ctids = (ItemPointer)palloc(capacity * sizeof(ItemPointerData));

	/* Get the metapage to find the first docid page */
	metabuf = ReadBuffer(index_relation, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	current_page = metap->first_docid_page;
	UnlockReleaseBuffer(metabuf);

	if (current_page == InvalidBlockNumber)
	{
		pfree(all_ctids);
		return NULL;
	}

	/* Iterate through all docid pages */
	while (current_page != InvalidBlockNumber)
	{
		docid_buf = ReadBuffer(index_relation, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/* Validate page magic */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
		{
			elog(WARNING,
				 "Invalid docid page magic on block %u, stopping scan",
				 current_page);
			UnlockReleaseBuffer(docid_buf);
			break;
		}

		/* Get docids from this page */
		docids = (ItemPointer)((char *)docid_header +
							   MAXALIGN(sizeof(TpDocidPageHeader)));

		/* Expand array if needed */
		if (count + docid_header->num_docids > capacity)
		{
			capacity  = count + docid_header->num_docids + 100;
			all_ctids = (ItemPointer)
					repalloc(all_ctids, capacity * sizeof(ItemPointerData));
		}

		/* Copy docids from this page */
		for (int i = 0; i < docid_header->num_docids; i++)
		{
			all_ctids[count++] = docids[i];
		}

		/* Move to next page */
		current_page = docid_header->next_page;
		UnlockReleaseBuffer(docid_buf);
	}

	*total_ctids = count;
	return all_ctids;
}

/*
 * Copy results to output arrays
 */
static void
tp_copy_results_to_output(
		DocumentScoreEntry **sorted_docs,
		int					 scored_count,
		ItemPointer			 additional_ctids,
		int					 additional_count,
		ItemPointer			 result_ctids,
		float4			   **result_scores)
{
	int		total_results = scored_count + additional_count;
	float4 *scores;
	int		i;

	/* Allocate scores array */
	scores = (float4 *)palloc(total_results * sizeof(float4));

	/* Copy scored documents */
	for (i = 0; i < scored_count; i++)
	{
		result_ctids[i] = sorted_docs[i]->ctid;
		/* Store BM25 score as computed (should be positive) */
		scores[i] = sorted_docs[i]->score;
	}

	/* Copy additional zero-scored documents */
	for (i = 0; i < additional_count; i++)
	{
		result_ctids[scored_count + i] = additional_ctids[i];
		scores[scored_count + i]	   = 0.0f; /* Zero score */
	}

	*result_scores = scores;
}

/*
 * Score documents using BM25 algorithm
 * Returns number of documents scored
 */
int
tp_score_documents(
		TpLocalIndexState *local_state,
		Relation		   index_relation,
		char			 **query_terms,
		int32			  *query_frequencies,
		int				   query_term_count,
		float4			   k1,
		float4			   b,
		int				   max_results,
		ItemPointer		   result_ctids,
		float4			 **result_scores)
{
	HTAB				*doc_scores_hash;
	DocumentScoreEntry **sorted_docs;
	float4				 avg_doc_len;
	int32				 total_docs;
	int					 scored_count = 0;
	dshash_table		*string_table;
	dshash_table		*doclength_table = NULL;
	TpMemtable			*memtable;
	TpIndexMetaPage		 metap;

	/* Basic sanity checks */
	Assert(local_state != NULL);
	Assert(query_terms != NULL);
	Assert(result_ctids != NULL);
	Assert(result_scores != NULL);

	if (query_term_count <= 0 || max_results <= 0)
		return 0;

	/* Get memtable and corpus statistics */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(ERROR, "Cannot get memtable for scoring - index state corrupted");
		return 0; /* Never reached */
	}

	if (!local_state->shared)
	{
		elog(ERROR, "tp_score_documents: shared state is NULL!");
		return 0;
	}

	total_docs	= local_state->shared->total_docs;
	avg_doc_len = total_docs > 0 ? (float4)(local_state->shared->total_len /
											(double)total_docs)
								 : 0.0f;

	if (total_docs <= 0)
		return 0;

	/* Get segment level heads for querying all levels */
	BlockNumber level_heads[TP_MAX_LEVELS];
	metap = tp_get_metapage(index_relation);
	for (int i = 0; i < TP_MAX_LEVELS; i++)
		level_heads[i] = metap->level_heads[i];
	pfree(metap);

	/* If avg_doc_len is 0, all documents have zero length and
	 * would get zero BM25 scores */
	if (avg_doc_len <= 0.0f)
		return 0;

	/* Create hash table for accumulating document scores - needed for both
	 * memtable and segment searches */
	doc_scores_hash = tp_create_doc_scores_hash(max_results, total_docs);
	if (!doc_scores_hash)
		elog(ERROR, "Failed to create document scores hash table");

	/* Get string hash table if available - may be empty after spill */
	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		string_table = tp_string_table_attach(
				local_state->dsa, memtable->string_hash_handle);
	}
	else
	{
		string_table = NULL;
	}

	/* Attach to document length table for memtable scoring */
	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		doclength_table = tp_doclength_table_attach(
				local_state->dsa, memtable->doc_lengths_handle);
	}

	/* Sum doc_freq from memtable + segments for unified IDF */
	uint32 *unified_doc_freqs = palloc0(query_term_count * sizeof(uint32));
	for (int term_idx = 0; term_idx < query_term_count; term_idx++)
	{
		const char *term = query_terms[term_idx];

		/* Get doc_freq from memtable */
		if (string_table)
		{
			TpPostingList *posting_list = tp_string_table_get_posting_list(
					local_state->dsa, string_table, term);
			if (posting_list && posting_list->doc_count > 0)
				unified_doc_freqs[term_idx] = posting_list->doc_count;
		}

		/* Add doc_freq from all segment levels */
		for (int level = 0; level < TP_MAX_LEVELS; level++)
		{
			if (level_heads[level] != InvalidBlockNumber)
			{
				uint32 segment_doc_freq = tp_segment_get_doc_freq(
						index_relation, level_heads[level], term);
				unified_doc_freqs[term_idx] += segment_doc_freq;
			}
		}
	}

	/* Calculate BM25 scores from memtable if available */
	if (string_table)
	{
		for (int term_idx = 0; term_idx < query_term_count; term_idx++)
		{
			const char	   *term = query_terms[term_idx];
			TpPostingList  *posting_list;
			TpPostingEntry *entries;
			float4			idf;

			posting_list = tp_string_table_get_posting_list(
					local_state->dsa, string_table, term);

			if (!posting_list || posting_list->doc_count == 0)
				continue;

			/* Calculate IDF using unified doc_freq from all sources */
			idf = tp_calculate_idf(unified_doc_freqs[term_idx], total_docs);

			/* Get posting entries */
			entries = tp_get_posting_entries(local_state->dsa, posting_list);
			if (!entries)
				continue;

			/* Process each document in this term's posting list */
			for (int doc_idx = 0; doc_idx < posting_list->doc_count; doc_idx++)
			{
				TpPostingEntry	   *entry = &entries[doc_idx];
				float4				tf	  = entry->frequency;
				float4				doc_len;
				float4				term_score;
				double				numerator_d, denominator_d;
				DocumentScoreEntry *doc_entry;
				bool				found;

				/* Validate TID first */
				if (!ItemPointerIsValid(&entry->ctid))
					continue;

				/* Look up document length from hash table */
				if (!doclength_table)
				{
					elog(ERROR,
						 "Document length table not available for scoring");
				}
				{
					int32 doc_len_int = tp_get_document_length_attached(
							doclength_table, &entry->ctid);
					if (doc_len_int <= 0)
					{
						elog(ERROR,
							 "Failed to get document length for ctid (%u,%u)",
							 BlockIdGetBlockNumber(&entry->ctid.ip_blkid),
							 entry->ctid.ip_posid);
					}
					doc_len = (float4)doc_len_int;
				}

				/* Calculate BM25 term score contribution */
				numerator_d = (double)tf * ((double)k1 + 1.0);

				/* Standard BM25 denominator calculation - avg_doc_len > 0 is
				 * guaranteed */
				denominator_d = (double)tf +
								(double)k1 *
										(1.0 - (double)b +
										 (double)b * ((double)doc_len /
													  (double)avg_doc_len));

				term_score = (float4)((double)idf *
									  (numerator_d / denominator_d) *
									  (double)query_frequencies[term_idx]);

				/* Find or create document entry in hash table */
				doc_entry = (DocumentScoreEntry *)hash_search(
						doc_scores_hash, &entry->ctid, HASH_ENTER, &found);

				if (!found)
				{
					/* New document - initialize */
					doc_entry->ctid		  = entry->ctid;
					doc_entry->score	  = term_score;
					doc_entry->doc_length = doc_len;
				}
				else
				{
					/* Existing document - accumulate score */
					doc_entry->score += term_score;
				}
			}
		}

		dshash_detach(string_table);
	}

	/* Detach from document length table after memtable scoring */
	if (doclength_table)
		dshash_detach(doclength_table);

	/* Score documents from all segment levels */
	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (level_heads[level] == InvalidBlockNumber)
			continue;

		for (int term_idx = 0; term_idx < query_term_count; term_idx++)
		{
			float4 idf =
					tp_calculate_idf(unified_doc_freqs[term_idx], total_docs);
			tp_process_term_in_segments(
					index_relation,
					level_heads[level],
					query_terms[term_idx],
					idf,
					(float4)query_frequencies[term_idx],
					k1,
					b,
					avg_doc_len,
					doc_scores_hash,
					local_state);
		}
	}

	/* Free unified doc_freqs array */
	pfree(unified_doc_freqs);

	/* Extract and sort documents by score */
	sorted_docs = tp_extract_and_sort_documents(
			doc_scores_hash, max_results, &scored_count);

	/* Copy results to output arrays (no additional zero-scored documents) */
	tp_copy_results_to_output(
			sorted_docs,
			scored_count,
			NULL, /* no additional_ctids */
			0,	  /* no additional_count */
			result_ctids,
			result_scores);

	/* Cleanup */
	if (sorted_docs)
		pfree(sorted_docs);
	hash_destroy(doc_scores_hash);

	return scored_count;
}

/*
 * CTID-based score cache implementation
 *
 * This is a backend-local cache that stores multiple scores keyed by
 * CTID + query hash. PostgreSQL may fetch all rows from an index scan
 * before projecting them, so we need to cache multiple entries.
 *
 * The cache uses a simple array with linear search. This is efficient
 * for typical result sets (up to a few hundred rows). For larger result
 * sets, cache misses will cause re-computation, which is acceptable.
 */

#define SCORE_CACHE_SIZE 256

typedef struct ScoreCacheEntry
{
	ItemPointerData ctid;
	Oid				table_oid; /* Table OID to distinguish partitions */
	uint32			query_hash;
	float4			score;
	bool			valid;
} ScoreCacheEntry;

static ScoreCacheEntry score_cache[SCORE_CACHE_SIZE];
static int			   score_cache_next = 0; /* Next slot to use (circular) */
static bool			   score_cache_initialized = false;

/*
 * Initialize the score cache (lazy initialization)
 */
static void
tp_init_score_cache(void)
{
	if (!score_cache_initialized)
	{
		for (int i = 0; i < SCORE_CACHE_SIZE; i++)
			score_cache[i].valid = false;
		score_cache_next		= 0;
		score_cache_initialized = true;
	}
}

/*
 * Cache a score for a CTID + table OID + query hash
 *
 * Called by tp_gettuple() after returning a result from the index scan.
 * The table_oid is needed to distinguish CTIDs from different partitions,
 * since CTIDs are only unique within a single table.
 */
void
tp_cache_score(
		ItemPointer ctid, Oid table_oid, uint32 query_hash, float4 score)
{
	ScoreCacheEntry *entry;

	if (!ItemPointerIsValid(ctid))
		return;

	tp_init_score_cache();

	/* Use circular buffer for cache entries */
	entry			 = &score_cache[score_cache_next];
	score_cache_next = (score_cache_next + 1) % SCORE_CACHE_SIZE;

	entry->ctid		  = *ctid;
	entry->table_oid  = table_oid;
	entry->query_hash = query_hash;
	entry->score	  = score;
	entry->valid	  = true;

	elog(DEBUG2,
		 "tp_cache_score: cached score %.4f for table %u ctid (%u,%u) hash %u",
		 score,
		 table_oid,
		 BlockIdGetBlockNumber(&ctid->ip_blkid),
		 ctid->ip_posid,
		 query_hash);
}

/*
 * Get a cached score for a CTID + table OID + query hash
 *
 * Returns the cached score if found, or 0.0 if not found.
 * Sets *found to true if the cache hit, false otherwise.
 *
 * Called by text_tpquery_score() before recomputing the score.
 * The table_oid is needed to distinguish CTIDs from different partitions.
 */
float4
tp_get_cached_score(
		ItemPointer ctid, Oid table_oid, uint32 query_hash, bool *found)
{
	*found = false;

	if (!ItemPointerIsValid(ctid))
		return 0.0f;

	tp_init_score_cache();

	/* Search cache for matching CTID + table OID + query hash */
	for (int i = 0; i < SCORE_CACHE_SIZE; i++)
	{
		ScoreCacheEntry *entry = &score_cache[i];

		if (!entry->valid)
			continue;

		if (ItemPointerEquals(&entry->ctid, ctid) &&
			entry->table_oid == table_oid && entry->query_hash == query_hash)
		{
			*found = true;
			elog(DEBUG2,
				 "tp_get_cached_score: HIT for table %u ctid (%u,%u) hash %u, "
				 "score %.4f",
				 table_oid,
				 BlockIdGetBlockNumber(&ctid->ip_blkid),
				 ctid->ip_posid,
				 query_hash,
				 entry->score);
			return entry->score;
		}
	}

	elog(DEBUG2,
		 "tp_get_cached_score: MISS for table %u ctid (%u,%u) hash %u",
		 table_oid,
		 BlockIdGetBlockNumber(&ctid->ip_blkid),
		 ctid->ip_posid,
		 query_hash);

	return 0.0f;
}

/*
 * Hash a query (index OID + query text) to generate a cache key
 *
 * Uses a simple djb2-style hash for efficiency.
 */
uint32
tp_hash_query(Oid index_oid, const char *query_text)
{
	uint32 hash = 5381;
	int	   c;

	/* Mix in the index OID */
	hash = ((hash << 5) + hash) ^ (index_oid & 0xFF);
	hash = ((hash << 5) + hash) ^ ((index_oid >> 8) & 0xFF);
	hash = ((hash << 5) + hash) ^ ((index_oid >> 16) & 0xFF);
	hash = ((hash << 5) + hash) ^ ((index_oid >> 24) & 0xFF);

	/* Mix in the query text */
	if (query_text)
	{
		while ((c = *query_text++) != 0)
			hash = ((hash << 5) + hash) ^ c;
	}

	return hash;
}

/*
 * Clear the score cache
 *
 * Called at the end of a scan or when starting a new query.
 */
void
tp_clear_score_cache(void)
{
	if (score_cache_initialized)
	{
		for (int i = 0; i < SCORE_CACHE_SIZE; i++)
			score_cache[i].valid = false;
		score_cache_next = 0;
	}
	elog(DEBUG2, "tp_clear_score_cache: cache cleared");
}

/*
 * Current row CTID and table OID tracking
 *
 * These backend-local variables store the CTID and table OID of the row
 * currently being processed. When tp_gettuple() returns a row, it sets these.
 * When text_tpquery_score() is called during projection, it reads these
 * to look up the cached score. The table OID is needed to distinguish CTIDs
 * from different partitions.
 */
static ItemPointerData current_row_ctid;
static bool			   current_row_ctid_valid	= false;
static Oid			   current_row_table_oid	= InvalidOid;
static uint32		   current_query_hash		= 0;
static bool			   current_query_hash_valid = false;

void
tp_set_current_row_ctid(ItemPointer ctid)
{
	if (ItemPointerIsValid(ctid))
	{
		current_row_ctid	   = *ctid;
		current_row_ctid_valid = true;
	}
	else
	{
		current_row_ctid_valid = false;
	}
}

ItemPointer
tp_get_current_row_ctid(void)
{
	if (current_row_ctid_valid)
		return &current_row_ctid;
	return NULL;
}

void
tp_set_current_row_table_oid(Oid table_oid)
{
	current_row_table_oid = table_oid;
}

Oid
tp_get_current_row_table_oid(void)
{
	return current_row_table_oid;
}

void
tp_set_current_query_hash(uint32 hash)
{
	current_query_hash		 = hash;
	current_query_hash_valid = true;
}

uint32
tp_get_current_query_hash(bool *valid)
{
	if (valid)
		*valid = current_query_hash_valid;
	return current_query_hash;
}
