/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am.h - BM25 access method shared definitions
 */
#pragma once

#include <postgres.h>

#include <access/amapi.h>
#include <access/reloptions.h>
#include <storage/block.h>
#include <storage/bufpage.h>

#include "state/state.h"
#include "types/vector.h"

/*
 * BM25 scan opaque data - internal state for index scans
 */
typedef struct TpScanOpaqueData
{
	MemoryContext scan_context; /* Memory context for scan */

	/* Query processing state */
	char	 *query_text;	/* Search query text */
	TpVector *query_vector; /* Original query vector from ORDER BY */
	Oid		  index_oid;	/* Index OID */

	/* Scan results state */
	ItemPointer result_ctids;  /* Array of matching CTIDs */
	float4	   *result_scores; /* Array of BM25 scores */
	int			result_count;  /* Number of results */
	int			current_pos;   /* Current position in results */
	bool		eof_reached;   /* End of scan flag */

	/* LIMIT optimization */
	int limit;			  /* Query LIMIT value, -1 if none */
	int max_results_used; /* Internal limit used for current batch */
} TpScanOpaqueData;

typedef TpScanOpaqueData *TpScanOpaque;

/* Index options structure */
typedef struct TpOptions
{
	int32  vl_len_;			   /* varlena header (do not touch directly!) */
	int32  text_config_offset; /* offset to text config string */
	double k1;				   /* BM25 k1 parameter */
	double b;				   /* BM25 b parameter */
} TpOptions;

/* Forward declarations */
struct IndexInfo;
struct PlannerInfo;
struct IndexPath;
struct IndexVacuumInfo;
struct IndexBulkDeleteResult;
struct IndexBuildResult;

/*
 * Shared utility functions
 */

/* Resolve index name to OID (supports schema.index notation) */
Oid tp_resolve_index_name_shared(const char *index_name);

/* Get qualified index name for display */
char *tp_get_qualified_index_name(Relation indexRelation);

/* Cached score for ORDER BY optimization */
float8 tp_get_cached_score(void);

/*
 * Access method handler
 */
Datum tp_handler(PG_FUNCTION_ARGS);

/*
 * Build functions (am/build.c)
 */
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

/* Shared document processing function */
bool tp_process_document_text(
		text			  *document_text,
		ItemPointer		   ctid,
		Oid				   text_config_oid,
		TpLocalIndexState *index_state,
		Relation		   index_rel,
		int32			  *doc_length_out);

/* IDF sum calculation for average IDF */
void tp_calculate_idf_sum(TpLocalIndexState *index_state);

/* Build progress tracking for partitioned tables */
void tp_build_progress_begin(void);
void tp_build_progress_end(void);
void tp_build_progress_reset(void);
bool tp_build_progress_is_active(void);
void tp_build_progress_count_long_word(void);

/*
 * Scan functions (am/scan.c)
 */
IndexScanDesc tp_beginscan(Relation index, int nkeys, int norderbys);
void		  tp_rescan(
				 IndexScanDesc scan,
				 ScanKey	   keys,
				 int		   nkeys,
				 ScanKey	   orderbys,
				 int		   norderbys);
void tp_endscan(IndexScanDesc scan);
bool tp_gettuple(IndexScanDesc scan, ScanDirection dir);

/*
 * Vacuum functions (am/vacuum.c)
 */
struct IndexBulkDeleteResult *tp_bulkdelete(
		struct IndexVacuumInfo		 *info,
		struct IndexBulkDeleteResult *stats,
		IndexBulkDeleteCallback		  callback,
		void						 *callback_state);
struct IndexBulkDeleteResult *tp_vacuumcleanup(
		struct IndexVacuumInfo *info, struct IndexBulkDeleteResult *stats);
char *tp_buildphasename(int64 phase);

/*
 * Handler functions (am/handler.c)
 */
bytea *tp_options(Datum reloptions, bool validate);
bool   tp_validate(Oid opclassoid);

/* Relation options kind - initialized in mod.c */
extern relopt_kind tp_relopt_kind;
