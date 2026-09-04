/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include <access/relscan.h>

#include "index/metapage.h"
#include "index/state.h"

void tp_boolean_rescan(
		IndexScanDesc scan, ScanKey keys, int nkeys, TpIndexMetaPage metap);

bool tp_boolean_execute(
		IndexScanDesc	   scan,
		TpLocalIndexState *index_state,
		TpIndexMetaPage	   metap);

bool tp_boolean_next(IndexScanDesc scan);
