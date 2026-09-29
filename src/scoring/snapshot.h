/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include "index/source.h"
#include "segment/graph_snapshot.h"

typedef struct TpScoringSnapshot
{
	uint64					serial;
	TpSegmentGraphSnapshot *graph;
	int32					total_docs;
	float4					avg_doc_len;
	bool					recovery;
} TpScoringSnapshot;

extern void tp_scoring_snapshot_init(void);
/*
 * Optionally return a new snapshot's totals source for the caller to close.
 * Standalone callers tokenize without source locks and request only totals.
 */
extern TpScoringSnapshot *tp_scoring_snapshot_get(
		Relation		   index,
		const char *const *query_terms,
		int				   query_term_count,
		TpDataSource	 **initial_source);
