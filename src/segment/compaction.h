/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

struct TpLocalIndexState;
typedef struct RelationData *Relation;

extern uint64 tp_max_segment_size_bytes(void);

/*
 * Report whether any level holds at least segments_per_level segments.
 * Advisory only; see the comment on the definition.
 */
extern bool tp_compaction_needed(Relation index);
extern void tp_maybe_compact_level(
		struct TpLocalIndexState *index_state,
		Relation				  index,
		uint32					  first_level);

/*
 * Run at most one bounded compaction pass and report whether one ran.
 * Splitting a cascade into passes lets each pass run in its own
 * transaction, so the per-index exclusive lock is released between
 * passes rather than held for the whole cascade.
 */
extern bool
tp_compact_step(struct TpLocalIndexState *index_state, Relation index);

extern void
tp_force_compact(struct TpLocalIndexState *index_state, Relation index);
