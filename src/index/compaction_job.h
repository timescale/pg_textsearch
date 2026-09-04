/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_job.h - Managed pg_durable compaction jobs
 */
#pragma once

#include <postgres.h>

extern void tp_compaction_job_preflight(Oid owner_oid, const char *schedule);
extern void tp_compaction_job_activate(Oid indexoid, bool refresh_default);
extern void tp_compaction_job_signal(Oid indexoid);
