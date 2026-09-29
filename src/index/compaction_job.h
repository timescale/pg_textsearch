/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_job.h - Managed pg_durable compaction jobs
 */
#pragma once

#include <postgres.h>

typedef struct TpCompactionJobObjects TpCompactionJobObjects;

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

typedef enum TpManagedIntentFlags
{
	TP_MANAGED_INTENT_REFRESH_DEFAULT	= 1 << 0,
	TP_MANAGED_INTENT_RECONCILE_OPTIONS = 1 << 1,
	TP_MANAGED_INTENT_PRESERVE_SCHEDULE = 1 << 2,
	TP_MANAGED_INTENT_LINEAGE_SUPPLIED	= 1 << 3,
	TP_MANAGED_INTENT_DISABLE			= 1 << 4,
	TP_MANAGED_INTENT_POST_PUBLICATION	= 1 << 5,
	TP_MANAGED_INTENT_RECONCILE_LINEAGE = 1 << 6
} TpManagedIntentFlags;

typedef struct TpManagedIndexIntent
{
	Oid						index_oid;
	TpCompactionJobIdentity source;
	char				   *schedule;
	char				   *lineage;
	int						flags;
	SubTransactionId		subid;
} TpManagedIndexIntent;

extern void tp_compaction_job_preflight(Oid owner_oid, const char *schedule);
extern TpCompactionJobObjects			  *
tp_compaction_job_try_lock_objects(bool invalid_is_error);
extern void tp_compaction_job_activate(
		const TpCompactionJobObjects *objects,
		Oid							  indexoid,
		bool						  refresh_default);
extern void tp_compaction_job_activate_with_schedule(
		const TpCompactionJobObjects *objects,
		Oid							  indexoid,
		const char					 *schedule);
extern void
tp_compaction_job_capture(Oid indexoid, TpCompactionJobIdentity *identity);
extern void tp_compaction_job_resolve_schedule(
		const TpCompactionJobObjects *objects,
		Oid							  indexoid,
		TpCompactionJobIdentity		 *identity,
		MemoryContext				  result_context);
extern void
tp_compaction_job_signal(const TpCompactionJobObjects *objects, Oid indexoid);
extern bool tp_compaction_job_lineage_exists(
		const TpCompactionJobObjects *objects,
		const char					 *lineage,
		Oid							  heap_oid,
		Oid							  owner_oid);
