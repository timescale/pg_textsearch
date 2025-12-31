/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment/source.h - Segment implementation of TpDataSource
 */
#pragma once

#include <postgres.h>

#include "../source.h"
#include "segment.h"

/*
 * Create a data source that reads from a single segment.
 * Caller must close with tp_source_close() when done.
 *
 * The segment is opened and kept open for the lifetime of the source.
 */
extern TpDataSource *
tp_segment_source_create(Relation index, BlockNumber segment_root);
