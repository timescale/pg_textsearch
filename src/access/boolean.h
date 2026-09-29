/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include <access/relscan.h>
#include <tsearch/ts_type.h>

#include "index/metapage.h"
#include "index/state.h"

/*
 * Each exact operand can retain a candidate iterator, and each distinct exact
 * term can retain an evaluator cursor.  This keeps the combined upper bound at
 * 128, below PostgreSQL's 200 simultaneously held LWLocks.
 *
 * ponytail: force-copy segment iterators if larger Boolean queries matter.
 */
#define TP_BOOLEAN_MAX_EXACT_OPERANDS 64

int tp_boolean_query_exact_operand_count(TSQuery query);

void tp_boolean_rescan(
		IndexScanDesc scan, ScanKey keys, int nkeys, TpIndexMetaPage metap);

bool tp_boolean_execute(IndexScanDesc scan, TpLocalIndexState *index_state);

bool tp_boolean_next(IndexScanDesc scan);
