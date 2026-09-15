/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.h - Background compaction request tracking
 */
#pragma once

#include <postgres.h>

#include <utils/rel.h>

typedef enum TpCompactionMode
{
	TP_COMPACTION_INLINE = 0,
	TP_COMPACTION_BACKGROUND,
	TP_COMPACTION_MANUAL
} TpCompactionMode;

#define TP_COMPACTION_LINEAGE_BYTES	 16
#define TP_COMPACTION_LINEAGE_LENGTH (TP_COMPACTION_LINEAGE_BYTES * 2)

extern char *tp_background_compaction_schedule;

extern int		   tp_index_compaction_mode(Relation index_rel);
extern const char *tp_index_compaction_schedule(Relation index_rel);
extern const char *tp_index_compaction_lineage(Relation index_rel);
extern char		  *tp_new_compaction_lineage(void);
extern char *tp_ensure_index_compaction_lineage(Oid indexoid, bool *created);
extern void	 tp_reconcile_index_compaction_options(
		 Oid indexoid, const char *schedule, const char *lineage);
extern char *tp_new_available_compaction_lineage(Oid heap_oid, Oid owner_oid);
extern bool
tp_compaction_lineage_in_use(const char *lineage, Oid heap_oid, Oid owner_oid);
extern void	 tp_lock_compaction_index(Oid indexoid);
extern void	 tp_lock_compaction_lineage(const char *lineage);
extern List *tp_prelock_compaction_indexes(List *indexoids);
extern List				*
tp_prelock_compaction_indexes_nowait(List *indexoids, bool nowait);
extern bool tp_compaction_dispatch_possible(void);
extern void tp_compaction_request(Oid indexoid);
extern void tp_compaction_flush_requests(void);
