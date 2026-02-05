/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/source.h - Memtable implementation of TpDataSource
 */
#pragma once

#include <postgres.h>

#include "../source.h"
#include "../state/state.h"

/*
 * Create a data source that reads from the memtable.
 * Caller must close with tp_source_close() when done.
 */
extern TpDataSource *tp_memtable_source_create(TpLocalIndexState *local_state);
