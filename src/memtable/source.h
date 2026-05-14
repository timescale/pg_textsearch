/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * source.h - Memtable implementation of TpDataSource
 */
#pragma once

#include <postgres.h>

#include <utils/rel.h>

#include "index/source.h"
#include "index/state.h"

/*
 * Create a data source that reads from the memtable.
 * Caller must close with tp_source_close() when done.
 *
 * Phase 4 of issue #374: this delegates to
 * tp_memtable_chain_source_create.  `rel` must be a valid open
 * index relation; passing NULL is a programmer error.
 */
extern TpDataSource *
tp_memtable_source_create(TpLocalIndexState *local_state, Relation rel);
