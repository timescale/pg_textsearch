#include <postgres.h>

#include <access/amapi.h>
#include <access/genam.h>
#include <access/reloptions.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/tableam.h>
#include <catalog/namespace.h>
#include <catalog/pg_opclass.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/float.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/selfuncs.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>

#include "constants.h"
#include "index.h"
#include "limit.h"
#include "memtable.h"
#include "metapage.h"
#include "posting.h"
#include "query.h"
#include "vector.h"

/* External GUC variable for score logging */
extern bool tp_log_scores;

/* Forward declarations */
static bool tp_search_posting_lists(
		IndexScanDesc	scan,
		TpIndexState   *index_state,
		TpVector	   *query_vector,
		TpIndexMetaPage metap);

/*
 * Tapir Index Access Method Implementation
 *
 * This implements a custom PostgreSQL access method for ranked BM25 search.
 */

/* Index options */
typedef struct TpOptions
{
	int32  vl_len_;			   /* varlena header (do not touch directly!) */
	int32  text_config_offset; /* offset to text config string */
	double k1;				   /* BM25 k1 parameter */
	double b;				   /* BM25 b parameter */
} TpOptions;

/* Relation options - initialized in mod.c */
extern relopt_kind tp_relopt_kind;

/* Helper functions for tp_build */
static void
tp_build_extract_options(
		Relation index,
		char   **text_config_name,
		Oid		*text_config_oid,
		double	*k1,
		double	*b)
{
	TpOptions *options;

	*text_config_name = NULL;
	*text_config_oid  = InvalidOid;

	/* Extract options from index */
	options = (TpOptions *)index->rd_options;
	if (options)
	{
		if (options->text_config_offset > 0)
		{
			*text_config_name = pstrdup(
					(char *)options + options->text_config_offset);
			/* Convert text config name to OID */
			{
				List *names = list_make1(makeString(*text_config_name));

				*text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for tapir "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "tapir(column) WITH (text_config='english')")));
		}

		*k1 = options->k1;
		*b	= options->b;
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for tapir indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "tapir(column) WITH (text_config='english')")));
	}
}

static void
tp_build_init_metapage(
		Relation index, Oid text_config_oid, double k1, double b)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;

	/* Initialize metapage */
	metabuf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);

	tp_init_metapage(metapage, text_config_oid);
	metap	  = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1 = k1;
	metap->b  = b;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

static void
tp_build_finalize_and_update_stats(
		Relation	  index,
		TpIndexState *index_state,
		uint64		 *total_docs,
		uint64		 *total_len)
{
	TpCorpusStatistics *stats;
	Buffer				metabuf;
	Page				metapage;
	TpIndexMetaPage		metap;

	/*
	 * Finalize posting lists (convert to sorted arrays for query performance)
	 */
	if (index_state != NULL)
	{
		tp_finalize_index_build(index_state);

		/* Get actual statistics from the posting list system */
		stats		= tp_get_corpus_statistics(index_state);
		*total_docs = stats->total_docs;
		*total_len	= stats->total_len;
	}

	/* Update metapage with computed statistics */
	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->total_docs = *total_docs;
	metap->total_len  = *total_len;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Access method handler - returns IndexAmRoutine with function pointers
 */
PG_FUNCTION_INFO_V1(tp_handler);

Datum
tp_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine;

	(void)fcinfo; /* unused */

	amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies	  = 0; /* No search strategies - ORDER BY only */
	amroutine->amsupport	  = 8; /* 8 for distance */
	amroutine->amoptsprocnum  = 0;
	amroutine->amcanorder	  = false;
	amroutine->amcanorderbyop = true; /* Supports ORDER BY operators */
#if PG_VERSION_NUM >= 180000
	amroutine->amcanhash			= false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering = false;
#endif
	amroutine->amcanbackward	  = false; /* Cannot scan backwards */
	amroutine->amcanunique		  = false; /* Cannot enforce uniqueness */
	amroutine->amcanmulticol	  = false; /* Single column only */
	amroutine->amoptionalkey	  = true;  /* Can scan without search key */
	amroutine->amsearcharray	  = false; /* No array search support */
	amroutine->amsearchnulls	  = false; /* Cannot search for NULLs */
	amroutine->amstorage		  = false; /* No separate storage type */
	amroutine->amclusterable	  = false; /* Cannot cluster on this index */
	amroutine->ampredlocks		  = false; /* No predicate locking */
	amroutine->amcanparallel	  = false; /* No parallel scan support yet */
	amroutine->amcanbuildparallel = true;
	amroutine->amcaninclude		  = false;		/* No INCLUDE columns */
	amroutine->amusemaintenanceworkmem = false; /* Use work_mem for builds */
	amroutine->amsummarizing		   = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype			   = InvalidOid;

	/* Interface functions */
	amroutine->ambuild			= tp_build;
	amroutine->ambuildempty		= tp_buildempty;
	amroutine->aminsert			= tp_insert;
	amroutine->aminsertcleanup	= NULL;
	amroutine->ambulkdelete		= tp_bulkdelete;
	amroutine->amvacuumcleanup	= tp_vacuumcleanup;
	amroutine->amcanreturn		= NULL;
	amroutine->amcostestimate	= tp_costestimate;
	amroutine->amoptions		= tp_options;
	amroutine->amproperty		= NULL;				 /* No property function */
	amroutine->ambuildphasename = tp_buildphasename; /* No build phase names */
	amroutine->amvalidate		= tp_validate;
	amroutine->amadjustmembers	= NULL; /* No member adjustment */
	amroutine->ambeginscan		= tp_beginscan;
	amroutine->amrescan			= tp_rescan;
	amroutine->amgettuple		= tp_gettuple;
	amroutine->amgetbitmap		= NULL; /* No bitmap scans - ORDER BY only like
										 * pgvector */
	amroutine->amendscan			  = tp_endscan;
	amroutine->ammarkpos			  = NULL; /* No mark/restore support */
	amroutine->amrestrpos			  = NULL;
	amroutine->amestimateparallelscan = NULL; /* No parallel support yet */
	amroutine->aminitparallelscan	  = NULL;
	amroutine->amparallelrescan		  = NULL;

#if PG_VERSION_NUM >= 180000
	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype  = NULL;
#endif

	PG_RETURN_POINTER(amroutine);
}

IndexBulkDeleteResult *
tp_bulkdelete(
		IndexVacuumInfo		   *info,
		IndexBulkDeleteResult  *stats,
		IndexBulkDeleteCallback callback __attribute__((unused)),
		void				   *callback_state __attribute__((unused)))
{
	TpIndexMetaPage metap;

	/* Initialize stats structure if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		stats->num_pages		= 1; /* Minimal pages (just metapage) */
		stats->num_index_tuples = (double)metap->total_docs;

		/* Track that deletion was requested */
		stats->tuples_removed = 0;
		stats->pages_deleted  = 0;

		pfree(metap);
	}
	else
	{
		/* Couldn't read metapage, return minimal stats */
		stats->num_pages		= 0;
		stats->num_index_tuples = 0;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;

		elog(WARNING,
			 "Tapir bulkdelete: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));
	}

	return stats;
}

/* Tapir-specific build phases */
#define TP_PHASE_BUILD_MEMTABLE 2
#define TP_PHASE_WRITE_METADATA 3

char *
tp_buildphasename(int64 phase)
{
	switch (phase)
	{
	case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
		return "initializing";
	case TP_PHASE_BUILD_MEMTABLE:
		return "building memtable";
	case TP_PHASE_WRITE_METADATA:
		return "writing metadata";
	default:
		return NULL;
	}
}

/*
 * Extract terms and frequencies from a TSVector
 *
 * Returns the document length (sum of all term frequencies)
 */
static int
tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out)
{
	int		   term_count = tsvector->size;
	char	 **terms;
	int32	  *frequencies;
	int		   doc_length = 0;
	int		   i;
	WordEntry *we;

	*term_count_out = term_count;

	if (term_count == 0)
	{
		*terms_out		 = NULL;
		*frequencies_out = NULL;
		return 0;
	}

	we = ARRPTR(tsvector);

	terms		= palloc(term_count * sizeof(char *));
	frequencies = palloc(term_count * sizeof(int32));

	for (i = 0; i < term_count; i++)
	{
		char *lexeme_start = STRPTR(tsvector) + we[i].pos;
		int	  lexeme_len   = we[i].len;
		char *lexeme;

		/* Always allocate on heap for terms array since we need to keep them
		 */
		lexeme = palloc(lexeme_len + 1);
		memcpy(lexeme, lexeme_start, lexeme_len);
		lexeme[lexeme_len] = '\0';

		terms[i] = lexeme;

		/*
		 * Get frequency from TSVector - count positions or default to 1
		 */
		if (we[i].haspos)
			frequencies[i] = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequencies[i] = 1;

		doc_length += frequencies[i];
	}

	*terms_out		 = terms;
	*frequencies_out = frequencies;

	return doc_length;
}

/*
 * Free memory allocated for terms array
 */
static void
tp_free_terms_array(char **terms, int term_count)
{
	int i;

	if (terms == NULL)
		return;

	for (i = 0; i < term_count; i++)
		pfree(terms[i]);

	pfree(terms);
}

/*
 * Setup table scanning for index build
 */
static void
tp_setup_table_scan(
		Relation heap, TableScanDesc *scan_out, TupleTableSlot **slot_out)
{
	*scan_out = table_beginscan(heap, GetTransactionSnapshot(), 0, NULL);

	*slot_out = table_slot_create(heap, NULL);
}

/*
 * Process a single document during index build
 *
 * Returns true if document was processed successfully, false to skip
 */
static bool
tp_process_document(
		TupleTableSlot *slot,
		IndexInfo	   *indexInfo,
		Oid				text_config_oid,
		TpIndexState   *index_state,
		Relation		index,
		uint64		   *total_docs)
{
	bool		isnull;
	Datum		text_datum;
	text	   *document_text;
	char	   *document_str;
	ItemPointer ctid;
	Datum		tsvector_datum;
	TSVector	tsvector;
	char	  **terms;
	int32	   *frequencies;
	int			term_count;
	int			doc_length;

	/* Get the text column value (first indexed column) */
	text_datum =
			slot_getattr(slot, indexInfo->ii_IndexAttrNumbers[0], &isnull);

	if (isnull)
		return false; /* Skip NULL documents */

	document_text = DatumGetTextPP(text_datum);

	document_str = text_to_cstring(document_text);

	ctid = &slot->tts_tid;

	/* Validate the TID before processing */
	if (!ItemPointerIsValid(ctid))
	{
		elog(WARNING,
			 "Invalid TID in slot during index build, skipping document");
		pfree(document_str);
		return false;
	}

	/*
	 * Vectorize the document using index metadata
	 */

	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid, /* collation */
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));

	tsvector = DatumGetTSVector(tsvector_datum);

	/* Extract lexemes and frequencies from TSVector */
	doc_length = tp_extract_terms_from_tsvector(
			tsvector, &terms, &frequencies, &term_count);

	if (term_count > 0)
	{
		/*
		 * Add document terms to posting lists (if shared memory available)
		 */

		if (index_state != NULL)
		{
			tp_add_document_terms(
					index_state,
					ctid,
					terms,
					frequencies,
					term_count,
					doc_length);
		}

		/* Free the terms array and individual lexemes */
		tp_free_terms_array(terms, term_count);
		pfree(frequencies);
	}

	/* Store the docid for crash recovery */
	tp_add_docid_to_pages(index, ctid);

	(*total_docs)++;

	pfree(document_str);

	return true;
}

/*
 * Build a new Tapir index
 */
IndexBuildResult *
tp_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	char			 *text_config_name = NULL;
	Oid				  text_config_oid  = InvalidOid;
	double			  k1, b;
	TableScanDesc	  scan;
	TupleTableSlot	 *slot;
	uint64			  total_docs = 0;
	uint64			  total_len	 = 0;
	TpIndexState	 *index_state;

	/* Tapir index build started */
	elog(NOTICE,
		 "Tapir index build started for relation %s",
		 RelationGetRelationName(index));

	/* Report initialization phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE);

	/* Extract options from index */
	tp_build_extract_options(
			index, &text_config_name, &text_config_oid, &k1, &b);

	/* Log configuration being used */
	if (text_config_name)
		elog(NOTICE, "Using text search configuration: %s", text_config_name);
	elog(NOTICE, "Using index options: k1=%.2f, b=%.2f", k1, b);

	/* Initialize metapage */
	tp_build_init_metapage(index, text_config_oid, k1, b);

	/* Initialize index state */
	index_state = tp_get_index_state(RelationGetRelid(index));

	/* Report memtable building phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_BUILD_MEMTABLE);

	/* Setup table scanning */
	tp_setup_table_scan(heap, &scan, &slot);

	/* Process each document in the heap */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		tp_process_document(
				slot,
				indexInfo,
				text_config_oid,
				index_state,
				index,
				&total_docs);
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	/* Report metadata writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITE_METADATA);

	/* Finalize posting lists and update statistics */
	tp_build_finalize_and_update_stats(
			index, index_state, &total_docs, &total_len);

	/* Create index build result */
	result				= (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
	result->heap_tuples = total_docs;
	result->index_tuples = total_docs;

	if (OidIsValid(text_config_oid))
	{
		elog(NOTICE,
			 "Tapir index build completed: %lu documents, avg_length=%.2f, "
			 "text_config='%s' (k1=%.2f, b=%.2f)",
			 total_docs,
			 total_len > 0 ? (float4)(total_len / (double)total_docs) : 0.0,
			 text_config_name ? text_config_name : "unknown",
			 k1,
			 b);
	}
	else
	{
		elog(NOTICE,
			 "Tapir index build completed: %lu documents, avg_length=%.2f "
			 "(text_config=%s, k1=%.2f, b=%.2f)",
			 total_docs,
			 total_len > 0 ? (float4)(total_len / (double)total_docs) : 0.0,
			 text_config_name,
			 k1,
			 b);
	}

	return result;
}

/*
 * Build an empty Tapir index (for CREATE INDEX without data)
 */
void
tp_buildempty(Relation index)
{
	TpOptions	   *options;
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	char		   *text_config_name = NULL;
	Oid				text_config_oid	 = InvalidOid;

	/* Extract options from index */
	options = (TpOptions *)index->rd_options;
	if (options)
	{
		if (options->text_config_offset > 0)
		{
			text_config_name = pstrdup(
					(char *)options + options->text_config_offset);
			{
				List *names = list_make1(makeString(text_config_name));

				text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for tapir "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "tapir(column) WITH (text_config='english')")));
		}
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for tapir indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "tapir(column) WITH (text_config='english')")));
	}

	/* Create and initialize the metapage */
	metabuf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	metapage = BufferGetPage(metabuf);
	tp_init_metapage(metapage, text_config_oid);

	/* Set additional parameters after init */
	metap			   = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1		   = TP_DEFAULT_K1;
	metap->b		   = TP_DEFAULT_B;
	metap->total_docs  = 0;
	metap->total_terms = 0;
	metap->total_len   = 0;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Insert a tuple into the Tapir index
 */
bool
tp_insert(
		Relation		 index,
		Datum			*values,
		bool			*isnull,
		ItemPointer		 ht_ctid,
		Relation		 heapRel,
		IndexUniqueCheck checkUnique,
		bool			 indexUnchanged,
		IndexInfo		*indexInfo)
{
	text		  *document_text;
	Datum		   vector_datum;
	TpVector	  *tpvec;
	TpVectorEntry *vector_entry;
	int32		  *frequencies;
	int			   term_count;
	int			   doc_length = 0;
	int			   i;
	TpIndexState  *index_state;

	(void)heapRel;		  /* unused */
	(void)checkUnique;	  /* unused */
	(void)indexUnchanged; /* unused */
	(void)indexInfo;	  /* unused */

	/* Skip NULL documents */
	if (isnull[0])
		return true;

	/* Get index state */
	index_state = tp_get_index_state(RelationGetRelid(index));
	Assert(index_state != NULL);

	/* Extract text from first column */
	document_text = DatumGetTextPP(values[0]);

	/* Vectorize the document */
	vector_datum = DirectFunctionCall2(
			to_tpvector,
			PointerGetDatum(document_text),
			CStringGetTextDatum(RelationGetRelationName(index)));
	tpvec = (TpVector *)DatumGetPointer(vector_datum);

	/* Extract term IDs and frequencies from tpvector */
	term_count = tpvec->entry_count;
	if (term_count > 0)
	{
		char **terms = palloc(term_count * sizeof(char *));

		frequencies = palloc(term_count * sizeof(int32));

		vector_entry = TPVECTOR_ENTRIES_PTR(tpvec);
		for (i = 0; i < term_count; i++)
		{
			char *lexeme;

			/*
			 * Always allocate on heap for terms array since we need to keep
			 * them
			 */
			lexeme = palloc(vector_entry->lexeme_len + 1);
			memcpy(lexeme, vector_entry->lexeme, vector_entry->lexeme_len);
			lexeme[vector_entry->lexeme_len] = '\0';

			/* Store the lexeme string directly in terms array */
			terms[i]	   = lexeme;
			frequencies[i] = vector_entry->frequency;
			doc_length += vector_entry->frequency;

			vector_entry = get_tpvector_next_entry(vector_entry);
		}

		/* Add document terms to posting lists (if shared memory available) */
		if (index_state != NULL)
		{
			/* Validate TID before adding to posting list */
			if (!ItemPointerIsValid(ht_ctid))
				elog(WARNING, "Invalid TID in tp_insert, skipping");
			else
			{
				tp_add_document_terms(
						index_state,
						ht_ctid,
						terms,
						frequencies,
						term_count,
						doc_length);
			}
		}

		/* Free the terms array and individual lexemes */
		for (i = 0; i < term_count; i++)
			pfree(terms[i]);
		pfree(terms);
		pfree(frequencies);
	}

	/* Store the docid for crash recovery */
	tp_add_docid_to_pages(index, ht_ctid);

	return true;
}

/*
 * Begin a scan of the Tapir index
 */
IndexScanDesc
tp_beginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	TpScanOpaque  so;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Allocate and initialize scan opaque data */
	so				 = (TpScanOpaque)palloc0(sizeof(TpScanOpaqueData));
	so->scan_context = AllocSetContextCreate(
			CurrentMemoryContext,
			"Tapir Scan Context",
			ALLOCSET_DEFAULT_SIZES);
	so->limit	 = -1; /* Initialize limit to -1 (no limit) */
	scan->opaque = so;

	/*
	 * Custom index AMs must allocate ORDER BY arrays themselves. This follows
	 * the pattern from GiST and SP-GiST.
	 */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals  = (Datum *)palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = (bool *)palloc(sizeof(bool) * norderbys);
		/* Initialize all orderbynulls to true, as GiST and SP-GiST do */
		memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
	}

	return scan;
}

/*
 * Restart a scan with new keys
 */
void
tp_rescan(
		IndexScanDesc scan,
		ScanKey		  keys __attribute__((unused)),
		int			  nkeys __attribute__((unused)),
		ScanKey		  orderbys,
		int			  norderbys)
{
	TpScanOpaque	so	  = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage metap = NULL;

	Assert(scan != NULL);
	Assert(scan->opaque != NULL);

	/* Retrieve query limit for optimization */
	{
		int query_limit = tp_get_query_limit(scan->indexRelation);

		if (query_limit > 0)
			so->limit = query_limit;
		else
			so->limit = -1; /* No limit */
	}

	/* Reset scan state */
	if (so)
	{
		/* Clean up any previous results before rescan */
		if (so->result_ctids)
		{
			if (so->scan_context)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);

				pfree(so->result_ctids);
				so->result_ctids = NULL;
				MemoryContextSwitchTo(oldcontext);
			}
			else
			{
				elog(WARNING,
					 "No scan context available for cleanup - memory leak!");
				so->result_ctids = NULL;
			}
		}
		if (so->result_scores)
		{
			if (so->scan_context)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);

				pfree(so->result_scores);
				so->result_scores = NULL;
				MemoryContextSwitchTo(oldcontext);
			}
			else
			{
				elog(WARNING,
					 "No scan context available for cleanup - memory leak!");
				so->result_scores = NULL;
			}
		}

		so->current_pos	 = 0;
		so->result_count = 0;
		so->eof_reached	 = false;
		so->query_vector = NULL;
	}

	/* Process ORDER BY scan keys for <@> operator */
	if (norderbys > 0 && orderbys && so)
	{
		/* Get index metadata to check if we have documents */
		if (!metap)
			metap = tp_get_metapage(scan->indexRelation);

		for (int i = 0; i < norderbys; i++)
		{
			ScanKey orderby = &orderbys[i];

			/* Check for <@> operator strategy */
			if (orderby->sk_strategy == 1) /* Strategy 1: <@> operator */
			{
				Datum query_datum = orderby->sk_argument;
				char *query_cstr;

				/*
				 * Handle tpquery type - text <@> tpquery operation
				 * PostgreSQL's type system guarantees this is a tpquery
				 */
				TpQuery *query = (TpQuery *)DatumGetPointer(query_datum);
				char	*index_name;
				char	*current_index_name;

				/* Validate it's a proper tpquery by checking varlena header */
				if (VARATT_IS_EXTENDED(query) ||
					VARSIZE_ANY(query) <
							VARHDRSZ + sizeof(int32) + sizeof(int32))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid tpquery data")));

				index_name		   = get_tpquery_index_name(query);
				current_index_name = RelationGetRelationName(
						scan->indexRelation);

				/* Validate index name if provided in query */
				if (index_name)
				{
					if (strcmp(index_name, current_index_name) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("tpquery index name mismatch"),
								 errhint("Query specifies index "
										 "\"%s\" but scan is on index "
										 "\"%s\"",
										 index_name,
										 current_index_name)));
				}

				query_cstr = pstrdup(get_tpquery_text(query));

				/* Clear query vector since we're using text directly */
				if (so->query_vector)
				{
					pfree(so->query_vector);
					so->query_vector = NULL;
				}

				/* Store query for processing during gettuple */
				if (so->query_text)
				{
					/*
					 * Free old query text if it was allocated in scan context
					 */
					MemoryContext oldcontext = MemoryContextSwitchTo(
							so->scan_context);

					pfree(so->query_text);
					MemoryContextSwitchTo(oldcontext);
				}

				/* Allocate new query text in scan context */
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(
							so->scan_context);

					so->query_text = pstrdup(query_cstr);
					MemoryContextSwitchTo(oldcontext);
				}

				/* Mark all docs as candidates for ORDER BY operation */
				if (metap && metap->total_docs > 0)
					so->result_count = metap->total_docs;

				pfree(query_cstr);
			}
		}

		if (metap)
			pfree(metap);
	}
}

/*
 * End a scan and cleanup resources
 */
void
tp_endscan(IndexScanDesc scan)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	if (so)
	{
		if (so->scan_context)
			MemoryContextDelete(so->scan_context);

		/* Free query vector if it was allocated */
		if (so->query_vector)
			pfree(so->query_vector);

		pfree(so);
		scan->opaque = NULL;
	}

	/*
	 * Don't free ORDER BY arrays here - they're allocated in our beginscan
	 * but PostgreSQL's core code expects them to persist and will free them.
	 * Just NULL out the pointers for safety.
	 */
	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals  = NULL;
		scan->xs_orderbynulls = NULL;
	}
}

/*
 * Execute BM25 scoring query to get ordered results
 */
static bool
tp_execute_scoring_query(IndexScanDesc scan)
{
	TpScanOpaque	so = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage metap;
	bool			success		= false;
	TpIndexState   *index_state = NULL;
	TpVector	   *query_vector;

	if (!so || !so->query_text)
		return false;

	Assert(so->scan_context != NULL);

	/* Clean up previous results */
	if (so->result_ctids || so->result_scores)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);

		if (so->result_ctids)
		{
			pfree(so->result_ctids);
			so->result_ctids = NULL;
		}
		if (so->result_scores)
		{
			pfree(so->result_scores);
			so->result_scores = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}

	so->result_count = 0;
	so->current_pos	 = 0;

	/* Get index metadata */
	PG_TRY();
	{
		metap = tp_get_metapage(scan->indexRelation);
		if (!metap)
		{
			elog(WARNING,
				 "Failed to get metapage for index %s",
				 RelationGetRelationName(scan->indexRelation));
			return false;
		}
	}
	PG_CATCH();
	{
		elog(WARNING,
			 "Exception while getting metapage for index %s",
			 RelationGetRelationName(scan->indexRelation));
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Perform actual BM25 search using posting lists */
	PG_TRY();
	{
		/* Get the index state with posting lists */
		index_state = tp_get_index_state(
				RelationGetRelid(scan->indexRelation));

		if (!index_state)
		{
			elog(WARNING, "Could not get index state for BM25 search");
			pfree(metap);
			return false;
		}

		/* Use the original query vector stored during rescan, or create one
		 * from text */
		query_vector = so->query_vector;

		if (!query_vector && so->query_text)
		{
			/* We have a text query - convert it to a vector using the index */
			char *index_name = RelationGetRelationName(scan->indexRelation);
			text *index_name_text  = cstring_to_text(index_name);
			text *query_text_datum = cstring_to_text(so->query_text);

			Datum query_vec_datum = DirectFunctionCall2(
					to_tpvector,
					PointerGetDatum(query_text_datum),
					PointerGetDatum(index_name_text));

			query_vector = (TpVector *)DatumGetPointer(query_vec_datum);

			/* Free existing query vector if present */
			if (so->query_vector)
				pfree(so->query_vector);

			/* Store the converted vector for this query execution */
			so->query_vector = query_vector;
		}

		if (!query_vector)
		{
			elog(WARNING, "No query vector available in scan state");
			pfree(metap);
			return false;
		}

		/* Find documents matching the query using posting lists */
		success = tp_search_posting_lists(
				scan, index_state, query_vector, metap);
	}
	PG_CATCH();
	{
		success = false;
	}
	PG_END_TRY();

	pfree(metap);
	return success;
}

/*
 * Search posting lists for documents matching the query vector
 * Returns true on success, false on failure
 */
static bool
tp_search_posting_lists(
		IndexScanDesc	scan,
		TpIndexState   *index_state,
		TpVector	   *query_vector,
		TpIndexMetaPage metap)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	int			 max_results;
	int			 result_count = 0;
	float4		 k1_value;
	float4		 b_value;

	/* Extract terms and frequencies from query vector */
	char		 **query_terms;
	int32		  *query_frequencies;
	TpVectorEntry *entries_ptr;
	int			   entry_count;
	char		  *ptr;
	MemoryContext  oldcontext;

	/* Use limit from scan state, fallback to GUC parameter */
	if (so && so->limit > 0)
		max_results = so->limit;
	else
		max_results = tp_default_limit;

	entry_count		  = query_vector->entry_count;
	query_terms		  = palloc(entry_count * sizeof(char *));
	query_frequencies = palloc(entry_count * sizeof(int32));
	entries_ptr		  = TPVECTOR_ENTRIES_PTR(query_vector);

	/* Parse the query vector entries */
	ptr = (char *)entries_ptr;
	for (int i = 0; i < entry_count; i++)
	{
		TpVectorEntry *entry = (TpVectorEntry *)ptr;
		char		  *term_str;

		/*
		 * Always allocate on heap for query terms array since we need to keep
		 * them
		 */
		term_str = palloc(entry->lexeme_len + 1);
		memcpy(term_str, entry->lexeme, entry->lexeme_len);
		term_str[entry->lexeme_len] = '\0';

		/* Store the term string directly in query terms array */
		query_terms[i]		 = term_str;
		query_frequencies[i] = entry->frequency;

		ptr += sizeof(TpVectorEntry) + MAXALIGN(entry->lexeme_len);
	}

	/* Allocate result arrays in scan context */
	oldcontext		 = MemoryContextSwitchTo(so->scan_context);
	so->result_ctids = palloc(max_results * sizeof(ItemPointerData));
	/* Initialize to invalid TIDs for safety */
	memset(so->result_ctids, 0, max_results * sizeof(ItemPointerData));
	MemoryContextSwitchTo(oldcontext);

	/* Check pointers before using them */
	if (!metap)
		elog(ERROR, "metap is NULL before tp_score_documents");

	/* Extract values from metap before the call to avoid any access issues */
	k1_value = metap->k1;
	b_value	 = metap->b;

	Assert(index_state != NULL);
	Assert(query_terms != NULL);
	Assert(query_frequencies != NULL);
	Assert(so->result_ctids != NULL);

	result_count = tp_score_documents(
			index_state,
			scan->indexRelation,
			query_terms,
			query_frequencies,
			entry_count,
			k1_value,
			b_value,
			max_results,
			so->result_ctids,
			&so->result_scores);

	so->result_count = result_count;
	so->current_pos	 = 0;

	/* Free the query terms array and individual term strings */
	for (int i = 0; i < entry_count; i++)
		pfree(query_terms[i]);

	pfree(query_terms);
	pfree(query_frequencies);

	return result_count > 0;
}

/*
 * Get next tuple from scan (for index-only scans)
 */
bool
tp_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	float4		 bm25_score;
	BlockNumber	 blknum;

	Assert(scan != NULL);
	Assert(so != NULL);

	/* Check if we have a query to process */
	if (!so->query_text)
		return false;

	/* Execute scoring query if we haven't done so yet */
	if (so->result_ctids == NULL && !so->eof_reached)
	{
		if (!tp_execute_scoring_query(scan))
		{
			so->eof_reached = true;
			return false;
		}
	}

	/* Check if we've reached the end */
	if (so->current_pos >= so->result_count || so->eof_reached)
		return false;

	Assert(so->scan_context != NULL);
	Assert(so->result_ctids != NULL);
	Assert(so->current_pos < so->result_count);

	Assert(ItemPointerIsValid(&so->result_ctids[so->current_pos]));

	/* Additional validation - check for obviously invalid block numbers */
	blknum = BlockIdGetBlockNumber(
			&(so->result_ctids[so->current_pos].ip_blkid));
	if (blknum == InvalidBlockNumber || blknum > TP_MAX_BLOCK_NUMBER)
	{
		/* Skip this result and try the next one */
		so->current_pos++;
		if (so->current_pos >= so->result_count)
			return false;
		/* Recursive call to try the next result */
		return tp_gettuple(scan, dir);
	}

	scan->xs_heaptid		= so->result_ctids[so->current_pos];
	scan->xs_recheck		= false;
	scan->xs_recheckorderby = false;

	/* Set ORDER BY distance value if this is an ORDER BY scan */
	if (scan->numberOfOrderBys > 0)
	{
		Assert(scan->numberOfOrderBys == 1);

		/* Set the ORDER BY value (BM25 score) */
		if (!scan->xs_orderbyvals || !scan->xs_orderbynulls)
		{
			/*
			 * ORDER BY arrays not allocated by PostgreSQL - this can happen
			 * when the query planner chooses index scan but ORDER BY arrays
			 * aren't properly initialized. Log a warning and continue.
			 */
			elog(WARNING,
				 "Tapir gettuple: ORDER BY arrays not allocated "
				 "(numberOfOrderBys=%d), continuing "
				 "without ORDER BY values",
				 scan->numberOfOrderBys);
		}
		else if (so->result_scores)
		{
			/*
			 * Convert BM25 score to Datum - PostgreSQL expects negative
			 * values for ASC ordering
			 */
			bm25_score				 = -so->result_scores[so->current_pos];
			scan->xs_orderbyvals[0]	 = Float4GetDatum(bm25_score);
			scan->xs_orderbynulls[0] = false;

			/* Log score if GUC is enabled */
			if (tp_log_scores)
			{
				elog(NOTICE,
					 "Tapir index scan: doc_pos=%d, tid=(%u,%u), "
					 "BM25_score=%.6f",
					 so->current_pos,
					 BlockIdGetBlockNumber(&scan->xs_heaptid.ip_blkid),
					 scan->xs_heaptid.ip_posid,
					 bm25_score);
			}
		}
		else
		{
			/* No scores available - use 0.0 as default */
			elog(WARNING,
				 "Tapir gettuple: result_scores is NULL, using 0.0 "
				 "for ORDER BY");
			scan->xs_orderbyvals[0]	 = Float4GetDatum(0.0);
			scan->xs_orderbynulls[0] = false;
		}
	}

	/* Move to next position */
	so->current_pos++;

	return true;
}

/*
 * Estimate cost of BM25 index scan
 */
void
tp_costestimate(
		PlannerInfo *root,
		IndexPath	*path,
		double		 loop_count,
		Cost		*indexStartupCost,
		Cost		*indexTotalCost,
		Selectivity *indexSelectivity,
		double		*indexCorrelation,
		double		*indexPages)
{
	GenericCosts	costs;
	TpIndexMetaPage metap;
	double			num_tuples = TP_DEFAULT_TUPLE_ESTIMATE;

	/* Never use index without ORDER BY clause */
	if (!path->indexorderbys || list_length(path->indexorderbys) == 0)
	{
		elog(DEBUG1,
			 "Tapir costestimate: path->indexorderbys is %s (len=%d)",
			 path->indexorderbys ? "non-null" : "NULL",
			 path->indexorderbys ? list_length(path->indexorderbys) : -1);
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost	  = get_float8_infinity();
		return;
	}

	/* Check for LIMIT clause and verify it can be safely pushed down */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX)
	{
		int limit = (int)root->limit_tuples;

		if (tp_can_pushdown_limit(root, path, limit))
			tp_store_query_limit(path->indexinfo->indexoid, limit);
	}

	/* Try to get actual statistics from the index */
	if (path->indexinfo && path->indexinfo->indexoid != InvalidOid)
	{
		Relation index_rel =
				index_open(path->indexinfo->indexoid, AccessShareLock);

		if (index_rel)
		{
			metap = tp_get_metapage(index_rel);
			if (metap && metap->total_docs > 0)
				num_tuples = (double)metap->total_docs;

			if (metap)
				pfree(metap);

			index_close(index_rel, AccessShareLock);
		}
	}

	/* Initialize generic costs */
	MemSet (&costs, 0, sizeof(costs))
		;
	genericcostestimate(root, path, loop_count, &costs);

	/* Override with BM25-specific estimates */
	*indexStartupCost = costs.indexStartupCost + 0.01; /* Small startup cost */
	*indexTotalCost	  = costs.indexTotalCost * TP_INDEX_SCAN_COST_FACTOR;

	/*
	 * Calculate selectivity based on LIMIT if available, otherwise use default
	 */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX &&
		num_tuples > 0)
	{
		/* Use LIMIT as upper bound for selectivity calculation */
		double limit_selectivity = Min(1.0, root->limit_tuples / num_tuples);
		*indexSelectivity =
				Max(limit_selectivity, TP_DEFAULT_INDEX_SELECTIVITY);
	}
	else
	{
		*indexSelectivity = TP_DEFAULT_INDEX_SELECTIVITY; /* Assume 10%
														   * selectivity for
														   * text searches */
	}
	*indexCorrelation = 0.0; /* No correlation assumptions */
	*indexPages		  = Max(1.0, num_tuples / 100.0); /* Rough page estimate */
}

/*
 * Parse and validate index options
 */
bytea *
tp_options(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] =
			{{"text_config",
			  RELOPT_TYPE_STRING,
			  offsetof(TpOptions, text_config_offset)},
			 {"k1", RELOPT_TYPE_REAL, offsetof(TpOptions, k1)},
			 {"b", RELOPT_TYPE_REAL, offsetof(TpOptions, b)}};

	return (bytea *)build_reloptions(
			reloptions,
			validate,
			tp_relopt_kind,
			sizeof(TpOptions),
			tab,
			lengthof(tab));
}

/*
 * Validate BM25 index definition
 */
bool
tp_validate(Oid opclassoid)
{
	HeapTuple		tup;
	Form_pg_opclass opclassform;
	Oid				opcintype;
	bool			result = true;

	/* Look up the opclass */
	tup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(tup))
	{
		elog(WARNING, "cache lookup failed for operator class %u", opclassoid);
		return false;
	}

	opclassform = (Form_pg_opclass)GETSTRUCT(tup);
	opcintype	= opclassform->opcintype;

	/* Check if the input type is compatible with text search */
	switch (opcintype)
	{
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID: /* char(n) */
		/* These are acceptable text types */
		result = true;
		break;
	default:
		elog(WARNING,
			 "Tapir index can only be created on text, varchar, or char "
			 "columns (got type OID "
			 "%u)",
			 opcintype);
		result = false;
		break;
	}

	ReleaseSysCache(tup);

	return result;
}

/*
 * Vacuum/cleanup the BM25 index
 */
IndexBulkDeleteResult *
tp_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	TpIndexMetaPage metap;

	/* Initialize stats if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		/* Update statistics with current values */
		stats->num_pages		= 1; /* Minimal pages (just metapage) */
		stats->num_index_tuples = (double)metap->total_docs;

		/* Report current usage statistics */
		if (stats->pages_deleted == 0 && stats->tuples_removed == 0)
		{
			/* No deletions recorded, report full statistics */
			stats->pages_free = 0; /* No free pages in memtable
									* implementation */
		}

		pfree(metap);
	}
	else
	{
		elog(WARNING,
			 "Tapir vacuum cleanup: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));

		/* Keep existing stats if available, otherwise initialize */
		if (stats->num_pages == 0 && stats->num_index_tuples == 0)
		{
			stats->num_pages		= 1; /* At least the metapage */
			stats->num_index_tuples = 0;
		}
	}

	return stats;
}

/*
 * tp_debug_dump_index - Debug function to show internal index structure
 */
PG_FUNCTION_INFO_V1(tp_debug_dump_index);

Datum
tp_debug_dump_index(PG_FUNCTION_ARGS)
{
	text		  *index_name_text = PG_GETARG_TEXT_PP(0);
	char		  *index_name;
	StringInfoData result;
	Oid			   index_oid;
	TpIndexState  *index_state;

	/* Convert text to C string */
	index_name = text_to_cstring(index_name_text);

	initStringInfo(&result);

	appendStringInfo(&result, "Tapir Index Debug: %s\n", index_name);

	/* Get the index relation */
	index_oid =
			get_relname_relid(index_name, get_namespace_oid("public", false));
	if (!OidIsValid(index_oid))
	{
		appendStringInfo(&result, "ERROR: Index '%s' not found\n", index_name);
		PG_RETURN_TEXT_P(cstring_to_text(result.data));
	}

	/* Get index state to inspect corpus statistics */
	index_state = tp_get_index_state(index_oid);
	if (index_state == NULL)
	{
		appendStringInfo(
				&result,
				"ERROR: Could not get index state for '%s'\n",
				index_name);
		PG_RETURN_TEXT_P(cstring_to_text(result.data));
	}

	/* Show corpus statistics */
	appendStringInfo(&result, "Corpus Statistics:\n");
	appendStringInfo(
			&result, "  total_docs: %d\n", index_state->stats.total_docs);
	appendStringInfo(
			&result, "  total_len: %ld\n", index_state->stats.total_len);

	if (index_state->stats.total_docs > 0)
	{
		float avg_doc_len = (float)index_state->stats.total_len /
							(float)index_state->stats.total_docs;

		appendStringInfo(&result, "  avg_doc_len: %.4f\n", avg_doc_len);
	}
	else
	{
		appendStringInfo(&result, "  avg_doc_len: 0 (no documents)\n");
	}

	appendStringInfo(&result, "BM25 Parameters:\n");
	appendStringInfo(&result, "  k1: %.2f\n", index_state->stats.k1);
	appendStringInfo(&result, "  b: %.2f\n", index_state->stats.b);

	/* Show term dictionary and posting lists */
	appendStringInfo(&result, "Term Dictionary:\n");

	if (index_state->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		dsa_area *area = tp_get_dsa_area_for_index(index_state, InvalidOid);

		if (area)
		{
			TpStringHashTable *string_table = tp_hash_table_attach_dsa(
					area, index_state->string_hash_handle);

			if (string_table)
			{
				uint32			   term_count = 0;
				dshash_seq_status  status;
				TpStringHashEntry *entry;

				/* Iterate through all entries using dshash sequential scan */
				dshash_seq_init(
						&status,
						string_table->dshash,
						false); /* shared lock */

				while ((entry = (TpStringHashEntry *)dshash_seq_next(
								&status)) != NULL)
				{
					/* Show term info if it has a posting list */
					if (DsaPointerIsValid(entry->key.flag_field))
					{
						TpPostingList *posting_list =
								dsa_get_address(area, entry->key.flag_field);
						char *stored_str = dsa_get_address(
								area, entry->key.string_or_ptr);

						appendStringInfo(
								&result,
								"  '%s': doc_freq=%d\n",
								stored_str,
								posting_list->doc_count);
						term_count++;
					}
				}

				dshash_seq_term(&status);
				tp_hash_table_detach_dsa(string_table);

				appendStringInfo(&result, "Total terms: %u\n", term_count);
			}
			else
			{
				appendStringInfo(
						&result,
						"  ERROR: Cannot attach to string hash table\n");
			}
		}
		else
		{
			appendStringInfo(&result, "  ERROR: Cannot access DSA area\n");
		}
	}
	else
	{
		appendStringInfo(
				&result, "  No terms (string hash table not initialized)\n");
	}

	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
