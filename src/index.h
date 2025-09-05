#ifndef INDEX_H
#define INDEX_H

#include "postgres.h"
#include "constants.h"
#include "access/amapi.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "vector.h"

/*
 * Tapir Index Access Method
 *
 * Tapir implements a single-column index for BM25 full-text search.
 * Each index uses PostgreSQL text search configurations and maintains
 * global statistics needed for BM25 scoring.
 */

/* Opaque data structures for Tapir index */
typedef struct TpIndexMetaPageData
{
	uint32		magic;			/* Magic number for validation */
	uint32		version;		/* Index format version */
	Oid			text_config_oid;	/* Text search configuration OID */
	uint64		total_docs;		/* Total number of documents */
	uint64		total_terms;	/* Total term occurrences across all docs */
	float4		avg_doc_length; /* Average document length */
	float4		k1;				/* BM25 k1 parameter */
	float4		b;				/* BM25 b parameter */
	BlockNumber root_blkno;		/* Root page of the index tree */
	BlockNumber term_stats_root;	/* Root page of term statistics B-tree */
	BlockNumber first_docid_page; /* First page of docid chain for crash recovery */
}			TpIndexMetaPageData;

typedef TpIndexMetaPageData * TpIndexMetaPage;

/*
 * Document ID page structure for crash recovery
 * Stores ItemPointerData (ctid) entries for documents in the index
 */
#define TP_DOCID_PAGE_MAGIC 0x54504449  /* "TPDI" in hex */

typedef struct TpDocidPageHeader
{
	uint32		magic;			/* Magic number for validation */
	uint32		num_docids;		/* Number of docids stored on this page */
	BlockNumber next_page;		/* Next page in chain, or InvalidBlockNumber */
	uint32		reserved;		/* Reserved for future use */
} TpDocidPageHeader;

/* Maximum number of docids that fit in a page */
#define TP_DOCIDS_PER_PAGE \
	((BLCKSZ - sizeof(PageHeaderData) - sizeof(TpDocidPageHeader)) / sizeof(ItemPointerData))

/* BM25 scan opaque data */
typedef struct TpScanOpaqueData
{
	MemoryContext scan_context; /* Memory context for scan */

	/* Query processing state */
	char	   *query_text;		/* Search query text */
	TpVector *query_vector;	/* Original query vector from ORDER BY */

	/* Scan results state */
	ItemPointer result_ctids;	/* Array of matching CTIDs */
	float4	   *result_scores;	/* Array of BM25 scores */
	int			result_count;	/* Number of results */
	int			current_pos;	/* Current position in results */
	bool		eof_reached;	/* End of scan flag */

	/* LIMIT optimization */
	int			limit;			/* Query LIMIT value, -1 if none */
}			TpScanOpaqueData;

typedef TpScanOpaqueData * TpScanOpaque;

/* Forward declarations */
struct IndexInfo;
struct PlannerInfo;
struct IndexPath;
struct IndexVacuumInfo;
struct IndexBulkDeleteResult;
struct IndexBuildResult;

/* Access method handler */
Datum		tp_handler(PG_FUNCTION_ARGS);

struct IndexBulkDeleteResult *tp_bulkdelete(struct IndexVacuumInfo *info,
									  struct IndexBulkDeleteResult *stats,
									  IndexBulkDeleteCallback callback,
									  void *callback_state);
char	   *tp_buildphasename(int64 phase);

/* Helper functions */
void		tp_init_metapage(Page page, Oid text_config_oid);
TpIndexMetaPage tp_get_metapage(Relation index);


/* Internal access method functions */
struct IndexBuildResult *tp_build(Relation heap, Relation index, struct IndexInfo *indexInfo);
void		tp_buildempty(Relation index);
bool		tp_insert(Relation index,
					   Datum *values,
					   bool *isnull,
					   ItemPointer ht_ctid,
					   Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
IndexScanDesc tp_beginscan(Relation index, int nkeys, int norderbys);
void		tp_rescan(IndexScanDesc scan,
					   ScanKey keys,
					   int nkeys,
					   ScanKey orderbys,
					   int norderbys);
void		tp_endscan(IndexScanDesc scan);
bool		tp_gettuple(IndexScanDesc scan, ScanDirection dir);
void		tp_costestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
bytea	   *tp_options(Datum reloptions, bool validate);
bool		tp_validate(Oid opclassoid);
struct IndexBulkDeleteResult *tp_vacuumcleanup(struct IndexVacuumInfo *info,
										 struct IndexBulkDeleteResult *stats);

/* Document ID page management for crash recovery */
void		tp_add_docid_to_pages(Relation index, ItemPointer ctid);
void		tp_recover_from_docid_pages(Relation index);

/* Query limit tracking functions */
void		tp_store_query_limit(Oid index_oid, int limit);
int			tp_get_query_limit(Relation index_rel);
void		tp_cleanup_query_limits(void);

#endif							/* INDEX_H */
