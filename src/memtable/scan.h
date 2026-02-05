/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/scan.h - Memtable scan operations
 */
#pragma once

#include <postgres.h>

#include <access/relscan.h>

#include "am/am.h"
#include "state/metapage.h"
#include "state/state.h"
#include "types/vector.h"

/*
 * Search the memtable and segments for documents matching the query vector.
 * Results are stored in the scan opaque structure.
 *
 * Returns true on success (at least one result), false otherwise.
 */
bool tp_memtable_search(
		IndexScanDesc	   scan,
		TpLocalIndexState *index_state,
		TpVector		  *query_vector,
		TpIndexMetaPage	   metap);
