/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.h - Background compaction request tracking
 */
#pragma once

#include <postgres.h>

#include <utils/guc.h>

typedef enum TpCompactionMode
{
	TP_COMPACTION_INLINE = 0,
	TP_COMPACTION_BACKGROUND,
	TP_COMPACTION_OFF
} TpCompactionMode;

extern int	 tp_compaction_mode;
extern char *tp_compaction_request_function;

extern bool tp_check_compaction_request_function(
		char **newval, void **extra, GucSource source);
extern void tp_compaction_request(Oid indexoid);
extern void tp_compaction_flush_requests(void);
extern void tp_compaction_reset_requests(void);
