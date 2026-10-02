/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * array.h - Text array helpers for BM25 indexing
 */
#pragma once

#include <postgres.h>

#include <utils/relcache.h>

/* Check if a type OID is a text-like array */
extern bool tp_is_text_array_type(Oid typid);

/*
 * Does the index's first key yield a text-like array? Reads the
 * index tuple descriptor, so plain columns and expressions are
 * handled alike.
 */
extern bool tp_index_key_is_text_array(Relation index);

/* Flatten a text array into a single space-separated text */
extern text *tp_flatten_text_array(Datum array_datum);
