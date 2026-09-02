/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.h - Background compaction request tracking
 */
#pragma once

#include <postgres.h>

#include <utils/guc.h>
#include <utils/rel.h>

typedef enum TpCompactionMode
{
	TP_COMPACTION_INLINE = 0,
	TP_COMPACTION_BACKGROUND,
	TP_COMPACTION_OFF
} TpCompactionMode;

extern char *tp_compaction_request_function;

extern int	tp_index_compaction_mode(Relation index_rel);
extern bool tp_compaction_dispatch_possible(void);
extern bool tp_check_compaction_request_function(
		char **newval, void **extra, GucSource source);
extern void tp_compaction_request(Oid indexoid);
extern void tp_compaction_flush_requests(void);
