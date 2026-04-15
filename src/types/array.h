/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * array.h - Text array helpers for BM25 indexing
 */
#pragma once

#include <postgres.h>

/* Check if a type OID is a text-like array */
extern bool tp_is_text_array_type(Oid typid);

/* Flatten a text array into a single space-separated text */
extern text *tp_flatten_text_array(Datum array_datum);
