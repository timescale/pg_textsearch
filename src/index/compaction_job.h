/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_job.h - Managed pg_durable compaction jobs
 */
#pragma once

#include <postgres.h>

typedef struct TpCompactionJobIdentity
{
	Oid	  heap_oid;
	Oid	  namespace_oid;
	char *index_name;
	Oid	  index_oid;
	Oid	  tablespace_oid;
	Oid	  relfilenumber;
	Oid	  owner_oid;
	char *lineage;
	char *schedule;
	bool  lineage_backfilled;
	bool  schedule_resolved;
} TpCompactionJobIdentity;

extern void tp_compaction_job_preflight(Oid owner_oid, const char *schedule);
extern void tp_compaction_job_activate(Oid indexoid, bool refresh_default);
extern void
tp_compaction_job_activate_with_schedule(Oid indexoid, const char *schedule);
extern void
tp_compaction_job_capture(Oid indexoid, TpCompactionJobIdentity *identity);
extern void tp_compaction_job_resolve_schedule(
		Oid						 indexoid,
		TpCompactionJobIdentity *identity,
		MemoryContext			 result_context);
extern void tp_compaction_job_signal(Oid indexoid);
extern bool tp_compaction_job_lineage_exists(
		const char *lineage, Oid heap_oid, Oid owner_oid);
