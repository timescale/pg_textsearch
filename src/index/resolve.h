/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * resolve.h - Index name resolution and query validation
 */
#pragma once

#include <postgres.h>

#include <utils/relcache.h>

extern char *tp_get_qualified_index_name(Relation indexRelation);
extern Oid	 tp_resolve_index_name_shared(const char *index_name);
extern void
tp_validate_query_index(Oid query_index_oid, Relation indexRelation);
