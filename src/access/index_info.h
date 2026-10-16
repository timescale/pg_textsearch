/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * index_info.h - SQL result for BM25 index header inspection
 */
#pragma once

#include <postgres.h>

#include <fmgr.h>

#include "index/metapage.h"

/*
 * Build the bm25_index_info() result row for the calling SQL function,
 * whose OUT columns must match bm25_index_info().  Exported so the
 * test-only fixture module returns rows through the same code.
 */
extern PGDLLEXPORT Datum tp_index_header_info_datum(
		FunctionCallInfo fcinfo, const TpIndexHeaderInfo *info);
