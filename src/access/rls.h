/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#ifndef TP_ACCESS_RLS_H
#define TP_ACCESS_RLS_H

#include <postgres.h>

#include <utils/rel.h>

extern bool tp_rls_allowed_for_current_utility(void);
extern void tp_rls_note_bm25_build(void);
extern void tp_check_bm25_build_allowed(Relation heap);
extern bool tp_check_bm25_index_create_allowed(Oid indexrelid);
extern void tp_check_bm25_hierarchy_allowed(Oid relid);
extern void tp_check_rls_enable_allowed(Oid relid);

#endif /* TP_ACCESS_RLS_H */
