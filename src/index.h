#pragma once

#include <postgres.h>

#include <access/amapi.h>
#include <storage/block.h>
#include <storage/bufpage.h>

#include "constants.h"
#include "metapage.h"
#include "vector.h"

/*
 * Tapir Index Access Method
 *
 * Tapir implements a single-column index for BM25 full-text search.
 * Each index uses PostgreSQL text search configurations and maintains
 * global statistics needed for BM25 scoring.
 */

/* BM25 scan opaque data */
typedef struct TpScanOpaqueData
{
	MemoryContext scan_context; /* Memory context for scan */

	/* Query processing state */
	char	 *query_text;	/* Search query text */
	TpVector *query_vector; /* Original query vector from ORDER BY */

	/* Scan results state */
	ItemPointer result_ctids;  /* Array of matching CTIDs */
	float4	   *result_scores; /* Array of BM25 scores */
	int			result_count;  /* Number of results */
	int			current_pos;   /* Current position in results */
	bool		eof_reached;   /* End of scan flag */

	/* LIMIT optimization */
	int limit; /* Query LIMIT value, -1 if none */
} TpScanOpaqueData;

typedef TpScanOpaqueData *TpScanOpaque;

struct IndexInfo;
struct PlannerInfo;
struct IndexPath;
struct IndexVacuumInfo;
struct IndexBulkDeleteResult;
struct IndexBuildResult;

/* Shared utility functions */
Oid tp_resolve_index_name_shared(const char *index_name);

/* Access method handler */
Datum tp_handler(PG_FUNCTION_ARGS);

struct IndexBulkDeleteResult *tp_bulkdelete(
		struct IndexVacuumInfo		 *info,
		struct IndexBulkDeleteResult *stats,
		IndexBulkDeleteCallback		  callback,
		void						 *callback_state);
char *tp_buildphasename(int64 phase);

/* Internal access method functions */
struct IndexBuildResult		 *
tp_build(Relation heap, Relation index, struct IndexInfo *indexInfo);
void tp_buildempty(Relation index);
bool tp_insert(
		Relation		  index,
		Datum			 *values,
		bool			 *isnull,
		ItemPointer		  ht_ctid,
		Relation		  heapRel,
		IndexUniqueCheck  checkUnique,
		bool			  indexUnchanged,
		struct IndexInfo *indexInfo);

IndexScanDesc tp_beginscan(Relation index, int nkeys, int norderbys);
void		  tp_rescan(
				 IndexScanDesc scan,
				 ScanKey	   keys,
				 int		   nkeys,
				 ScanKey	   orderbys,
				 int		   norderbys);

void tp_endscan(IndexScanDesc scan);
bool tp_gettuple(IndexScanDesc scan, ScanDirection dir);
void tp_costestimate(
		struct PlannerInfo *root,
		struct IndexPath   *path,
		double				loop_count,
		Cost			   *indexStartupCost,
		Cost			   *indexTotalCost,
		Selectivity		   *indexSelectivity,
		double			   *indexCorrelation,
		double			   *indexPages);

bytea						 *tp_options(Datum reloptions, bool validate);
bool						  tp_validate(Oid opclassoid);
struct IndexBulkDeleteResult *tp_vacuumcleanup(
		struct IndexVacuumInfo *info, struct IndexBulkDeleteResult *stats);

/* Query limit tracking - now in limits.h */

/* Include state management structures */
#include "state.h"

/* Shared document processing function */
extern bool tp_process_document_text(
		text			  *document_text,
		ItemPointer		   ctid,
		Oid				   text_config_oid,
		TpLocalIndexState *index_state,
		int32			  *doc_length_out);

/* GUC variable for logging BM25 scores */
extern bool tp_log_scores;

/* IDF sum calculation for average IDF */
extern void tp_calculate_idf_sum(TpLocalIndexState *index_state);
