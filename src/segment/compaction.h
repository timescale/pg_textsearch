/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

struct TpLocalIndexState;
typedef struct RelationData *Relation;

extern uint64 tp_max_segment_size_bytes(void);
extern void
tp_force_compact(struct TpLocalIndexState *index_state, Relation index);
