#include <postgres.h>

#include <access/amapi.h>
#include <access/genam.h>
#include <access/generic_xlog.h>
#include <access/heapam.h>
#include <access/itup.h>
#include <access/reloptions.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/xlog.h>
#include <catalog/index.h>
#include <catalog/namespace.h>
#include <catalog/pg_opclass.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <executor/spi.h>
#include <float.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <nodes/tidbitmap.h>
#include <optimizer/cost.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <parser/parse_relation.h>
#include <stdint.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <storage/ipc.h>
#include <storage/shmem.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/float.h>
#include <utils/hsearch.h>
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
#include "vector.h"

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
			elog(DEBUG1,
				 "Using text search configuration: %s",
				 *text_config_name);
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
		elog(DEBUG1, "Using index options: k1=%.2f, b=%.2f", *k1, *b);
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
		elog(DEBUG2, "About to call tp_finalize_index_build");
		tp_finalize_index_build(index_state);
		elog(DEBUG2, "Returned from tp_finalize_index_build");

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

	elog(DEBUG2, "tp_handler: initializing access method");
	amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies	 = 0; /* No search strategies - ORDER BY only */
	amroutine->amsupport	 = 0; /* No support functions */
	amroutine->amoptsprocnum = 0;
	amroutine->amcanorder = true; /* Can return ordered results for ORDER BY */
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
		IndexBulkDeleteCallback callback,
		void				   *callback_state)
{
	TpIndexMetaPage metap;

	elog(DEBUG2,
		 "tp_bulkdelete: index=%s",
		 RelationGetRelationName(info->index));

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

		elog(DEBUG1,
			 "Tapir bulkdelete: index %s has %u pages, %.0f documents",
			 RelationGetRelationName(info->index),
			 stats->num_pages,
			 stats->num_index_tuples);
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
#define TAPIR_PHASE_BUILD_MEMTABLE 2
#define TAPIR_PHASE_WRITE_METADATA 3

char *
tp_buildphasename(int64 phase)
{
	elog(DEBUG2, "tp_buildphasename: phase=%ld", (long)phase);

	switch (phase)
	{
	case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
		return "initializing";
	case TAPIR_PHASE_BUILD_MEMTABLE:
		return "building memtable";
	case TAPIR_PHASE_WRITE_METADATA:
		return "writing metadata";
	default:
		return NULL; /* Unknown phase */
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

	elog(DEBUG1, "TSVector has %d terms", term_count);

	terms		= palloc(term_count * sizeof(char *));
	frequencies = palloc(term_count * sizeof(int32));

	for (i = 0; i < term_count; i++)
	{
		char *lexeme_start = STRPTR(tsvector) + we[i].pos;
		int	  lexeme_len   = we[i].len;
		char *lexeme;

		elog(DEBUG1,
			 "Processing term %d of %d: pos=%d, len=%d",
			 i,
			 term_count,
			 we[i].pos,
			 lexeme_len);

		/* Always allocate on heap for terms array since we need to keep them
		 */
		lexeme = palloc(lexeme_len + 1);
		memcpy(lexeme, lexeme_start, lexeme_len);
		lexeme[lexeme_len] = '\0';

		elog(DEBUG1, "Term %d: lexeme='%s'", i, lexeme);

		terms[i] = lexeme;

		/*
		 * Get frequency from TSVector - count positions or default to 1
		 */
		if (we[i].haspos)
		{
			frequencies[i] = (int32)POSDATALEN(tsvector, &we[i]);
			elog(DEBUG1, "Term %d has %d positions", i, frequencies[i]);
		}
		else
		{
			frequencies[i] = 1;
			elog(DEBUG1, "Term %d defaulting to frequency 1", i);
		}

		doc_length += frequencies[i];
		elog(DEBUG1, "Doc length after term %d: %d", i, doc_length);
	}

	*terms_out		 = terms;
	*frequencies_out = frequencies;

	elog(DEBUG1, "Finished processing all %d terms", term_count);
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
	{
		pfree(terms[i]);
	}
	pfree(terms);
}

/*
 * Setup table scanning for index build
 */
static void
tp_setup_table_scan(
		Relation heap, TableScanDesc *scan_out, TupleTableSlot **slot_out)
{
	elog(DEBUG1,
		 "Starting table scan for heap %s",
		 RelationGetRelationName(heap));

	*scan_out = table_beginscan(heap, GetTransactionSnapshot(), 0, NULL);
	elog(DEBUG1, "Created table scan");

	*slot_out = table_slot_create(heap, NULL);
	elog(DEBUG1, "Created table slot");
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

	elog(DEBUG1, "Processing document");

	/* Get the text column value (first indexed column) */
	text_datum =
			slot_getattr(slot, indexInfo->ii_IndexAttrNumbers[0], &isnull);

	elog(DEBUG1, "Got text datum, isnull=%d", isnull);

	if (isnull)
		return false; /* Skip NULL documents */

	document_text = DatumGetTextPP(text_datum);
	elog(DEBUG1, "Got document_text=%p", document_text);

	document_str = text_to_cstring(document_text);
	elog(DEBUG1, "Got document_str='%s'", document_str);

	ctid = &slot->tts_tid;

	/* Validate the TID before processing */
	if (!ItemPointerIsValid(ctid))
	{
		elog(WARNING,
			 "Invalid TID in slot during index build, skipping document");
		pfree(document_str);
		return false;
	}

	elog(DEBUG1, "TID is valid");

	/*
	 * Vectorize the document using index metadata
	 */
	elog(DEBUG1,
		 "About to vectorize document with text_config_oid=%u",
		 text_config_oid);

	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid, /* collation */
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));

	tsvector = DatumGetTSVector(tsvector_datum);
	elog(DEBUG1, "Got tsvector=%p", tsvector);

	/* Extract lexemes and frequencies from TSVector */
	doc_length = tp_extract_terms_from_tsvector(
			tsvector, &terms, &frequencies, &term_count);

	if (term_count > 0)
	{
		/*
		 * Add document terms to posting lists (if shared memory available)
		 */
		elog(DEBUG1,
			 "index_state=%p, about to add document terms",
			 index_state);

		if (index_state != NULL)
		{
			elog(DEBUG1,
				 "Calling tp_add_document_terms with %d terms",
				 term_count);

			tp_add_document_terms(
					index_state,
					ctid,
					terms,
					frequencies,
					term_count,
					doc_length);

			elog(DEBUG1, "Finished tp_add_document_terms");
		}

		/* Free the terms array and individual lexemes */
		tp_free_terms_array(terms, term_count);
		pfree(frequencies);
	}

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

	elog(DEBUG2,
		 "tp_build: heap=%s, index=%s",
		 RelationGetRelationName(heap),
		 RelationGetRelationName(index));
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
	{
		elog(NOTICE, "Using text search configuration: %s", text_config_name);
	}
	elog(NOTICE, "Using index options: k1=%.2f, b=%.2f", k1, b);

	/* Initialize metapage */
	tp_build_init_metapage(index, text_config_oid, k1, b);

	/* Initialize index state */
	index_state = tp_get_index_state(
			RelationGetRelid(index), RelationGetRelationName(index));

	elog(DEBUG2,
		 "Index build: Got index_state=%p for index %s",
		 index_state,
		 RelationGetRelationName(index));

	/* Report memtable building phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TAPIR_PHASE_BUILD_MEMTABLE);

	/* Setup table scanning */
	tp_setup_table_scan(heap, &scan, &slot);

	/* Process each document in the heap */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		tp_process_document(
				slot, indexInfo, text_config_oid, index_state, &total_docs);
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	/* Report metadata writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TAPIR_PHASE_WRITE_METADATA);

	/* Finalize posting lists and update statistics */
	elog(DEBUG2, "About to call tp_build_finalize_and_update_stats");
	tp_build_finalize_and_update_stats(
			index, index_state, &total_docs, &total_len);
	elog(DEBUG2, "Returned from tp_build_finalize_and_update_stats");

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

	elog(DEBUG2,
		 "Building empty Tapir index for %s",
		 RelationGetRelationName(index));

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
	index_state = tp_get_index_state(
			RelationGetRelid(index), RelationGetRelationName(index));
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
			{
				elog(WARNING, "Invalid TID in bm25insert, skipping");
			}
			else
			{
				elog(DEBUG2,
					 "tp_insert: calling tp_add_document_terms with %d terms",
					 term_count);
				tp_add_document_terms(
						index_state,
						ht_ctid,
						terms,
						frequencies,
						term_count,
						doc_length);
			}
		}
		else
		{
			elog(DEBUG2,
				 "tp_insert: index_state is NULL, skipping "
				 "tp_add_document_terms");
		}

		/* Free the terms array and individual lexemes */
		for (i = 0; i < term_count; i++)
		{
			pfree(terms[i]);
		}
		pfree(terms);
		pfree(frequencies);
	}

	/* Store the docid for crash recovery */
	tp_add_docid_to_pages(index, ht_ctid);

	elog(DEBUG2,
		 "tp_insert: index=%s, ctid=(%u,%u)",
		 RelationGetRelationName(index),
		 ItemPointerGetBlockNumber(ht_ctid),
		 ItemPointerGetOffsetNumber(ht_ctid));

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

	elog(DEBUG1,
		 "Tapir begin scan: index=%s, nkeys=%d, norderbys=%d",
		 RelationGetRelationName(index),
		 nkeys,
		 norderbys);

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
		elog(DEBUG2,
			 "tp_beginscan: allocated ORDER BY arrays for %d clauses",
			 norderbys);
	}

	return scan;
}

/*
 * Restart a scan with new keys
 */
void
tp_rescan(
		IndexScanDesc scan,
		ScanKey		  keys,
		int			  nkeys,
		ScanKey		  orderbys,
		int			  norderbys)
{
	TpScanOpaque	so	  = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage metap = NULL;

	elog(DEBUG1, "tp_rescan: nkeys=%d, norderbys=%d", nkeys, norderbys);

	if (!scan || !scan->opaque)
	{
		elog(ERROR, "tp_rescan called with NULL scan or opaque data");
		return;
	}

	/* Retrieve query limit for optimization */
	if (so)
	{
		int query_limit = tp_get_query_limit(scan->indexRelation);

		if (query_limit > 0)
		{
			so->limit = query_limit;
			elog(DEBUG2,
				 "tp_rescan: Using LIMIT %d for scan optimization",
				 query_limit);
		}
		else
		{
			so->limit = -1; /* No limit */
			elog(DEBUG2, "tp_rescan: No LIMIT detected for scan optimization");
		}
	}

	/* Reset scan state */
	if (so)
	{
		elog(DEBUG2, "tp_rescan called on scan %p, so=%p", scan, so);

		/* Clean up any previous results before rescan */
		if (so->result_ctids)
		{
			elog(DEBUG2,
				 "Cleaning up previous result_ctids=%p during rescan",
				 so->result_ctids);
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
			elog(DEBUG2,
				 "Cleaning up previous result_scores=%p during rescan",
				 so->result_scores);
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

		elog(DEBUG2, "Resetting scan state");
		so->current_pos	 = 0;
		so->result_count = 0;
		so->eof_reached	 = false;
		so->query_vector = NULL;
	}

	/* Process scan keys to set up the search */
	if (nkeys > 0 && keys)
	{
		/* Get index metadata to check if we have documents */
		metap = tp_get_metapage(scan->indexRelation);

		for (int i = 0; i < nkeys; i++)
		{
			ScanKey key = &keys[i];

			elog(DEBUG1,
				 "tp_rescan key %d: strategy=%d, flags=%d",
				 i,
				 key->sk_strategy,
				 key->sk_flags);

			/* Check for <@> operator strategy */
			if (so && key->sk_strategy == 1) /* Strategy 1: <@> operator */
			{
				elog(DEBUG1, "tp_rescan: <@> operator strategy detected");
			}
		}

		pfree(metap);
	}
	else
	{
		elog(DEBUG1, "tp_rescan: no search keys provided");
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

			elog(DEBUG1,
				 "Tapir ORDER BY key %d: strategy=%d, flags=%d",
				 i,
				 orderby->sk_strategy,
				 orderby->sk_flags);

			/* Check for <@> operator strategy */
			if (orderby->sk_strategy == 1) /* Strategy 1: <@> operator */
			{
				/*
				 * Extract query from ORDER BY key - it's a tpvector, not
				 * text
				 */
				Datum	  query_datum  = orderby->sk_argument;
				TpVector *query_vector = (TpVector *)DatumGetPointer(
						query_datum);
				char *query_cstr;

				/* Store the original query vector in scan state */
				so->query_vector = query_vector;

				/* Extract the original query text from the vector structure */
				/* For now, we'll reconstruct the query from the vector terms
				 */
				if (query_vector && query_vector->entry_count > 0)
				{
					StringInfo	   query_buf   = makeStringInfo();
					TpVectorEntry *entries_ptr = TPVECTOR_ENTRIES_PTR(
							query_vector);
					char *ptr	= (char *)entries_ptr;
					bool  first = true;

					elog(DEBUG3,
						 "Reconstructing query from %d vector entries",
						 query_vector->entry_count);

					for (int j = 0; j < query_vector->entry_count; j++)
					{
						TpVectorEntry *entry = (TpVectorEntry *)ptr;

						elog(DEBUG3,
							 "Entry %d: lexeme='%s', lexeme_len=%d",
							 j,
							 entry->lexeme,
							 entry->lexeme_len);

						if (!first)
							appendStringInfoChar(query_buf, ' ');
						appendStringInfoString(query_buf, entry->lexeme);
						first = false;

						/* Move to next entry - ensure proper alignment */
						ptr += sizeof(TpVectorEntry) +
							   MAXALIGN(entry->lexeme_len);
					}
					query_cstr = pstrdup(query_buf->data);
					pfree(query_buf->data);
					pfree(query_buf);
				}
				else
				{
					query_cstr = pstrdup("");
				}

				elog(DEBUG3,
					 "Tapir ORDER BY query reconstructed: %s",
					 query_cstr);

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

				elog(DEBUG2,
					 "Tapir rescan: stored query_text='%s' in so=%p",
					 so->query_text ? so->query_text : "NULL",
					 so);

				/* Mark all docs as candidates for ORDER BY operation */
				if (metap && metap->total_docs > 0)
				{
					so->result_count = metap->total_docs;
					elog(DEBUG1,
						 "Tapir ORDER BY: processing %lu documents",
						 metap->total_docs);
				}

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

	elog(DEBUG2, "Tapir end scan called for scan %p", scan);

	if (so)
	{
		if (so->scan_context)
		{
			elog(DEBUG2,
				 "Deleting scan context %p for scan %p",
				 so->scan_context,
				 scan);
			MemoryContextDelete(so->scan_context);
		}
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

	elog(DEBUG2, "Tapir scan cleanup complete");
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

	elog(DEBUG2,
		 "tp_execute_scoring_query: so=%p, query_text=%s",
		 so,
		 so ? so->query_text : "NULL");

	if (!so || !so->query_text)
		return false;

	if (!so->scan_context)
	{
		elog(ERROR, "Tapir scan context is NULL");
		return false;
	}

	/* Clean up any previous results */
	elog(DEBUG1,
		 "Cleaning up previous results. result_ctids=%p, result_count=%d",
		 so->result_ctids,
		 so->result_count);

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
				RelationGetRelid(scan->indexRelation),
				RelationGetRelationName(scan->indexRelation));

		if (!index_state)
		{
			elog(WARNING, "Could not get index state for BM25 search");
			pfree(metap);
			return false;
		}

		/* Use the original query vector stored during rescan */
		query_vector = so->query_vector;

		if (!query_vector)
		{
			elog(WARNING, "No query vector available in scan state");
			pfree(metap);
			return false;
		}

		elog(DEBUG2,
			 "Tapir search: query='%s', vector has %d terms",
			 so->query_text,
			 query_vector->entry_count);

		/* Find documents matching the query using posting lists */
		success = tp_search_posting_lists(
				scan, index_state, query_vector, metap);
	}
	PG_CATCH();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
		ErrorData	 *errdata	 = CopyErrorData();

		elog(WARNING,
			 "Exception during BM25 search for query '%s': %s",
			 so->query_text,
			 errdata->message);
		FreeErrorData(errdata);
		MemoryContextSwitchTo(oldcontext);
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
	{
		max_results = so->limit;
		elog(DEBUG1,
			 "Tapir: Using LIMIT optimization with max_results=%d",
			 max_results);
	}
	else
	{
		max_results = tp_default_limit; /* Use GUC parameter as fallback */
		elog(DEBUG2,
			 "Tapir: No limit optimization, using GUC default "
			 "max_results=%d",
			 max_results);
	}

	entry_count		  = query_vector->entry_count;
	query_terms		  = palloc(entry_count * sizeof(char *));
	query_frequencies = palloc(entry_count * sizeof(int32));
	entries_ptr		  = TPVECTOR_ENTRIES_PTR(query_vector);

	elog(DEBUG2, "Tapir search: parsing %d query terms", entry_count);

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

		elog(DEBUG3,
			 "Query term %d: '%s', freq=%d",
			 i,
			 term_str,
			 query_frequencies[i]);

		ptr += sizeof(TpVectorEntry) + MAXALIGN(entry->lexeme_len);
	}

	/* Allocate result arrays in scan context */
	oldcontext		 = MemoryContextSwitchTo(so->scan_context);
	so->result_ctids = palloc(max_results * sizeof(ItemPointerData));
	/* Initialize to invalid TIDs for safety */
	memset(so->result_ctids, 0, max_results * sizeof(ItemPointerData));
	MemoryContextSwitchTo(oldcontext);

	/* Use single-pass BM25 scoring algorithm for efficiency */
	elog(DEBUG2,
		 "Tapir search: using single-pass BM25 scoring for %d terms",
		 entry_count);

	elog(DEBUG2,
		 "Calling tp_score_documents with max_results=%d",
		 max_results);

	/* Check pointers before using them */
	if (!metap)
	{
		elog(ERROR, "metap is NULL before tp_score_documents");
	}

	/* Extract values from metap before the call to avoid any access issues */
	k1_value = metap->k1;
	b_value	 = metap->b;

	elog(DEBUG3, "BM25 parameters: k1=%f, b=%f", k1_value, b_value);

	if (!index_state)
	{
		elog(ERROR, "index_state is NULL before tp_score_documents");
	}
	if (!query_terms)
	{
		elog(ERROR, "query_terms is NULL before tp_score_documents");
	}
	if (!query_frequencies)
	{
		elog(ERROR, "query_frequencies is NULL before tp_score_documents");
	}
	if (!so->result_ctids)
	{
		elog(ERROR, "result_ctids is NULL before tp_score_documents");
	}

	elog(DEBUG3, "All pointers validated");

	/* Check if any query terms are NULL */
	for (int i = 0; i < entry_count; i++)
	{
		if (!query_terms[i])
		{
			elog(ERROR, "NULL query term at position %d", i);
		}
	}

	result_count = tp_score_documents(
			index_state,
			query_terms,
			query_frequencies,
			entry_count,
			k1_value,
			b_value,
			max_results,
			so->result_ctids,
			&so->result_scores);

	elog(DEBUG2, "tp_score_documents returned %d results", result_count);

	so->result_count = result_count;
	so->current_pos	 = 0;

	elog(DEBUG2,
		 "Tapir search completed: found %d matching documents (max_results "
		 "was %d)",
		 result_count,
		 max_results);

	/* Validate results */
	for (int i = 0; i < result_count; i++)
	{
		if (!ItemPointerIsValid(&so->result_ctids[i]))
		{
			elog(ERROR, "Invalid TID at position %d after scoring", i);
		}
	}

	/* Free the query terms array and individual term strings */
	for (int i = 0; i < entry_count; i++)
	{
		pfree(query_terms[i]);
	}
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

	elog(DEBUG3,
		 "Tapir gettuple: dir=%d, scan->xs_orderbyvals=%p, "
		 "scan->xs_orderbynulls=%p",
		 dir,
		 scan->xs_orderbyvals,
		 scan->xs_orderbynulls);

	if (!scan)
	{
		elog(ERROR, "Tapir gettuple called with NULL scan");
		return false;
	}

	if (!so)
	{
		elog(ERROR,
			 "Tapir gettuple: no scan opaque data (scan->opaque is NULL)");
		return false;
	}

	elog(DEBUG3,
		 "Scan state: result_count=%d, current_pos=%d",
		 so->result_count,
		 so->current_pos);

	/* Check if we have a query to process */
	if (!so->query_text)
	{
		elog(DEBUG3, "Tapir gettuple: no query text, so=%p", so);
		return false;
	}

	/* Execute scoring query if we haven't done so yet */
	if (so->result_ctids == NULL && !so->eof_reached)
	{
		if (!tp_execute_scoring_query(scan))
		{
			so->eof_reached = true;
			return false;
		}
		/* Verify so is still valid after query execution */
		if (!so)
		{
			elog(ERROR,
				 "Tapir gettuple: scan opaque became NULL after "
				 "query execution");
			return false;
		}
		elog(DEBUG2,
			 "Tapir scoring query completed: %d results found",
			 so->result_count);
	}

	/* Check if we've reached the end */
	if (so->current_pos >= so->result_count || so->eof_reached)
	{
		elog(DEBUG1, "Tapir gettuple: end of results reached");
		return false;
	}

	elog(DEBUG2,
		 "Tapir gettuple: about to access result_ctids[%d], result_ctids=%p, "
		 "scan=%p, so=%p",
		 so->current_pos,
		 so->result_ctids,
		 scan,
		 so);

	/* Verify scan context is still valid */
	if (!so->scan_context)
	{
		elog(ERROR, "Tapir gettuple: scan_context is NULL!");
		return false;
	}

	/* Set the current item pointer for this result */
	if (!so->result_ctids)
	{
		elog(ERROR,
			 "Tapir gettuple: result_ctids is NULL at position %d",
			 so->current_pos);
		return false;
	}

	/* Extra validation before access */
	if (so->current_pos >= so->result_count)
	{
		elog(ERROR,
			 "Tapir gettuple: current_pos %d >= result_count %d",
			 so->current_pos,
			 so->result_count);
		return false;
	}

	elog(DEBUG2,
		 "Setting heap TID: block=%u, offset=%u",
		 BlockIdGetBlockNumber(&(so->result_ctids[so->current_pos].ip_blkid)),
		 so->result_ctids[so->current_pos].ip_posid);

	/* Validate TID before setting */
	if (!ItemPointerIsValid(&so->result_ctids[so->current_pos]))
	{
		elog(ERROR, "Invalid TID at position %d", so->current_pos);
		return false;
	}

	/* Additional validation - check for obviously invalid block numbers */
	blknum = BlockIdGetBlockNumber(
			&(so->result_ctids[so->current_pos].ip_blkid));
	if (blknum == InvalidBlockNumber || blknum > TP_MAX_BLOCK_NUMBER)
	{
		elog(WARNING,
			 "Suspicious block number %u at position %d, skipping this result",
			 blknum,
			 so->current_pos);
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
		if (scan->numberOfOrderBys != 1)
		{
			elog(ERROR,
				 "Tapir gettuple: numberOfOrderBys must be 1, got %d",
				 scan->numberOfOrderBys);
			return false;
		}

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
		else
		{
			/* Additional validation for pointer sanity */
			uintptr_t orderbyvals_addr	= (uintptr_t)scan->xs_orderbyvals;
			uintptr_t orderbynulls_addr = (uintptr_t)scan->xs_orderbynulls;

			/* Check if pointers are in reasonable memory range (heuristic) */
			if (orderbyvals_addr < TP_MIN_MEMORY_ADDRESS ||
				orderbyvals_addr > TP_MAX_MEMORY_ADDRESS ||
				orderbynulls_addr < TP_MIN_MEMORY_ADDRESS ||
				orderbynulls_addr > TP_MAX_MEMORY_ADDRESS)
			{
				elog(WARNING,
					 "Tapir gettuple: ORDER BY arrays have invalid pointers "
					 "(vals=%p, nulls=%p), "
					 "skipping ORDER BY",
					 scan->xs_orderbyvals,
					 scan->xs_orderbynulls);
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

				elog(DEBUG2,
					 "Tapir gettuple: set ORDER BY value = %f",
					 bm25_score);
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
	}

	/* Move to next position */
	so->current_pos++;

	return true; /* Successfully returned a tuple */
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

	elog(DEBUG2,
		 "tp_costestimate: indexoid=%u, loop_count=%f",
		 path->indexinfo->indexoid,
		 loop_count);

	/* Never use index without ORDER BY clause */
	if (!path->indexorderbys || list_length(path->indexorderbys) == 0)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost	  = get_float8_infinity();
		return;
	}

	elog(DEBUG1,
		 "Tapir cost estimation called for %d clauses, %d orderbys",
		 list_length(path->indexclauses),
		 list_length(path->indexorderbys));

	/* Check for LIMIT clause and verify it can be safely pushed down */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX)
	{
		int limit = (int)root->limit_tuples;

		if (tp_can_pushdown_limit(root, path, limit))
		{
			tp_store_query_limit(path->indexinfo->indexoid, limit);
			elog(DEBUG1,
				 "Tapir: Safe LIMIT pushdown detected - LIMIT %d for index %u",
				 limit,
				 path->indexinfo->indexoid);
		}
		else
		{
			elog(DEBUG1,
				 "Tapir: LIMIT %d detected but pushdown is unsafe for index "
				 "%u",
				 limit,
				 path->indexinfo->indexoid);
		}
	}
	else
	{
		elog(DEBUG2,
			 "Tapir: No LIMIT detected (limit_tuples=%f)",
			 root ? root->limit_tuples : -1.0);
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
			{
				num_tuples = (double)metap->total_docs;
			}
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
	 * Make index scan very attractive
	 */
	*indexSelectivity = TP_DEFAULT_INDEX_SELECTIVITY; /* Assume 10%
													   * selectivity for text
													   * searches */
	*indexCorrelation = 0.0; /* No correlation assumptions */
	*indexPages		  = Max(1.0, num_tuples / 100.0); /* Rough page estimate */

	elog(DEBUG1,
		 "Tapir cost estimate: startup=%.2f, total=%.2f, sel=%.2f, pages=%.2f",
		 *indexStartupCost,
		 *indexTotalCost,
		 *indexSelectivity,
		 *indexPages);
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

	elog(DEBUG2, "tp_options: validate=%d", validate);

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

	elog(DEBUG2, "tp_validate: opclassoid=%u", opclassoid);

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

	if (result)
		elog(DEBUG1,
			 "Tapir index validation passed for type OID %u",
			 opcintype);

	return result;
}

/*
 * Vacuum/cleanup the BM25 index
 */
IndexBulkDeleteResult *
tp_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	TpIndexMetaPage metap;

	elog(DEBUG1,
		 "Tapir vacuum called for relation %s",
		 RelationGetRelationName(info->index));

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

		elog(DEBUG1,
			 "Tapir vacuum cleanup: index %s has %u pages, %.0f documents",
			 RelationGetRelationName(info->index),
			 stats->num_pages,
			 stats->num_index_tuples);
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
	index_state = tp_get_index_state(index_oid, index_name);
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
