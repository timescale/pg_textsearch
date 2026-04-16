/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memory.h - Memory usage visibility functions
 */
#pragma once

#include <postgres.h>

#include <fmgr.h>

extern Datum tp_memory_usage(PG_FUNCTION_ARGS);
