/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * facet.h - Faceted-search filter pushdown interface
 *
 * Implements a TID allow-list that constrains Block-Max WAND scoring to the
 * documents matching a scalar restriction clause (a "facet"). This lets the
 * top-k threshold reflect only matching documents instead of relying on a
 * post-filter above an over-fetched index scan.
 *
 * The flow mirrors the LIMIT pushdown (index/limit.c):
 *   - tp_costestimate() detects a usable facet clause and stashes a spec in a
 *     per-backend channel keyed by the BM25 index OID.
 *   - tp_rescan() builds the actual TID allow-list from that spec.
 *   - BMW consults the active filter while scoring.
 *
 * Correctness does not depend on the allow-list being exact: PostgreSQL's
 * Filter node above the index scan re-checks the real quals, so a superset is
 * always safe.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/rel.h>

/* GUCs */
extern bool	  tp_enable_facet_pushdown;
extern double tp_facet_selectivity_threshold;

/*
 * A sorted (ascending by CTID) array of heap TIDs that satisfy the facet
 * predicate. Membership is tested with binary search.
 */
typedef struct TpFacetFilter
{
	ItemPointerData *tids;	/* sorted ascending */
	int				 count; /* number of TIDs */
} TpFacetFilter;

/*
 * Planner side: stash a facet spec for a BM25 index. Called from
 * tp_costestimate when a single "Var OP Const" restriction clause is found
 * and passes the selectivity gate. The value Datum is copied into a
 * persistent context; only one spec is retained per backend.
 */
void tp_store_query_facet(
		Oid		   bm25_index_oid,
		Oid		   heap_oid,
		AttrNumber attno,
		Oid		   opno,
		Oid		   collation,
		bool	   var_on_left,
		Datum	   value,
		int16	   typlen,
		bool	   typbyval);

/*
 * Execution side: if a spec was stashed for this index, scan the heap and
 * build the allow-list in the given memory context. Returns NULL when no spec
 * is present or pushdown is disabled. The spec is consumed (one-shot).
 */
TpFacetFilter *tp_build_query_facet(Relation bm25_index, MemoryContext ctx);

/* Clear any stashed spec (transaction end / safety). */
void tp_cleanup_query_facets(void);

/*
 * Active-filter helpers consulted by BMW. The active filter is set around the
 * scoring run and points into the scan's memory context.
 */
void tp_facet_set_active(TpFacetFilter *filter);
void tp_facet_clear_active(void);
bool tp_facet_active(void);

/* Returns true if a filter is active and ctid is NOT in the allow-list. */
bool tp_facet_excludes(ItemPointer ctid);
