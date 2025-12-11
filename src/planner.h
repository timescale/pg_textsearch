/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * planner.h - Parse analysis hook for implicit index resolution
 */
#pragma once

#include <postgres.h>

/* Initialize parse analysis hook (called from _PG_init) */
void tp_planner_hook_init(void);
